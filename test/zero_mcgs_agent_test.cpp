// Monte-Carlo graph search agent.
//
// Regression tests carried over from the MCTS agent:
//   #3  search runs in eval mode without autograd
//   #4  finished games use the real result, not the network value
//   #9  root noise is sampled once per search
//   #10 illegal moves are never selected
// plus MCGS-specific behaviour: transpositions, value backup, forced moves.

#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include <types.h>
#include <go_types.h>
#include <splitmix.h>
#include <null_distribution.h>
#include <pytorch_util.h>
#include <go_action_encoder.h>
#include <go_zero_encoder.h>
#include <zero_episodic_exp.h>
#include <model_base.h>
#include <zero_mcgs_agent.h>

#include "test_helpers.h"

namespace R = rlgames;
namespace T = rlgames::test;
namespace t = torch;
using T::P;
using T::Pass;

namespace {

using Container = R::ModelContainer<
  T::TestModel,
  R::ZeroGoStateEncoder<T::TSZ>,
  R::ZeroGoActionEncoder<T::TSZ>,
  t::optim::Adam>;

template <template <uint, typename> class Noise = R::null_distribution>
using Agent = R::ZeroMCGSAgent<
  Container,
  Noise<T::TASZ, float>,
  R::Splitmix,
  R::GoBoard<T::TSZ>,
  R::GoGameState<T::TSZ>,
  T::TASZ>;

// exposes the search graph
struct Inspect : Agent<> {
  using Agent<>::Agent;
  using Agent<>::mNodes;
  using Agent<>::mEdges;
};

constexpr uint PASS_IDX = T::TSZ * T::TSZ;

Container make_container() {
  return Container(
    T::TestModel(),
    R::ZeroGoStateEncoder<T::TSZ>(),
    R::ZeroGoActionEncoder<T::TSZ>(),
    1E-3F);
}

template <typename A = Agent<>>
A make_agent(Container& c, uint sims, uint seed, float noise_factor = 0.F,
             float exploration = 0.5F) {
  return A(c, t::kCPU, sims, exploration, 0.03F, noise_factor, seed);
}

R::ZeroExperience random_experience(int n) {
  return R::ZeroExperience(
    t::randn({n, T::TPLANE, T::TSZ, T::TSZ}),
    t::zeros({n, 2}),
    t::rand({n, T::TASZ}) + 0.1F,
    t::ones({n}));
}

// only the given points get prior mass; everything else exactly 0
void focus_priors(Container& c, std::initializer_list<std::pair<int, int>> pts) {
  c.model->policy_bias.fill_(-std::numeric_limits<float>::infinity());
  for (auto [r, col] : pts) c.model->policy_bias[r * T::TSZ + col] = 0.F;
}

}  // namespace

// ------------------------------------------------- carried over from MCTS

TEST(ZeroMCGSAgent, SearchRunsInEvalModeWithoutAutograd) {
  Container c = make_container();
  R::ZeroExperience warmup = random_experience(8);
  (void)R::train(c, warmup);
  t::Tensor mean_before = c.model->bn->running_mean.clone();
  t::Tensor var_before  = c.model->bn->running_var.clone();

  auto agent = make_agent(c, 20, 1);
  (void)agent.select_move(R::GoGameState<T::TSZ>());

  EXPECT_GT(c.model->forward_calls, 0);
  EXPECT_FALSE(c.model->last_training);
  EXPECT_FALSE(c.model->last_grad_enabled);
  EXPECT_TRUE(t::equal(c.model->bn->running_mean, mean_before));
  EXPECT_TRUE(t::equal(c.model->bn->running_var, var_before));
  EXPECT_TRUE(t::GradMode::is_enabled());

  R::ZeroExperience exp = random_experience(8);
  (void)R::train(c, exp);
  EXPECT_TRUE(c.model->is_training());
}

TEST(ZeroMCGSAgent, PassesWhenPassingWinsTheFinishedGame) {
  // black wall on column 4, white has only passed; passing wins for black
  std::vector<R::Move> moves;
  for (int r = 0; r < 9; ++r) { moves.push_back(P(r, 4)); moves.push_back(Pass()); }
  auto gs = T::play<T::TSZ>(moves);
  ASSERT_EQ(gs.next_player(), R::Player::Black);

  Container c = make_container();
  c.model->zero_weights();
  c.model->fixed_value = 1.F;  // network: "player to move is winning"
  for (uint seed = 1; seed <= 5; ++seed) {
    auto agent = make_agent(c, 300, seed);
    EXPECT_EQ(agent.select_move(gs).mty, R::M::Pass) << "seed " << seed;
  }
}

TEST(ZeroMCGSAgent, AvoidsPassWhenPassingLosesTheFinishedGame) {
  std::vector<R::Move> moves = {P(4, 8), P(0, 4)};
  for (int r = 1; r < 9; ++r) { moves.push_back(Pass()); moves.push_back(P(r, 4)); }
  moves.push_back(P(3, 8));
  moves.push_back(Pass());
  auto gs = T::play<T::TSZ>(moves);
  {
    auto over = gs;
    over.apply_move(Pass());
    ASSERT_EQ(over.winner(), R::Player::White);
  }

  Container c = make_container();
  c.model->zero_weights();
  c.model->fixed_value = -1.F;  // network: "player to move is losing"
  for (uint seed = 1; seed <= 5; ++seed) {
    auto agent = make_agent(c, 300, seed);
    EXPECT_NE(agent.select_move(gs).mty, R::M::Pass) << "seed " << seed;
  }
}

TEST(ZeroMCGSAgent, NoiseSampledOncePerSearch) {
  Container c = make_container();
  auto agent = make_agent<Agent<T::CountingNoise>>(c, 50, 1, 0.25F);
  T::CountingNoise<T::TASZ, float>::calls = 0;
  (void)agent.select_move(R::GoGameState<T::TSZ>());
  EXPECT_EQ((T::CountingNoise<T::TASZ, float>::calls), 1);
  (void)agent.select_move(R::GoGameState<T::TSZ>());
  EXPECT_EQ((T::CountingNoise<T::TASZ, float>::calls), 2);
}

TEST(ZeroMCGSAgent, NeverSelectsIllegalMove) {
  // all prior mass on the occupied A1 point
  auto gs = T::play<T::TSZ>({P(0, 0), P(0, 1)});
  Container c = make_container();
  c.model->zero_weights();
  c.model->fixed_value = 1.F;
  focus_priors(c, {{0, 0}});
  for (uint seed = 1; seed <= 5; ++seed) {
    Inspect agent = make_agent<Inspect>(c, 400, seed);
    R::Move m = agent.select_move(gs);
    EXPECT_TRUE(gs.is_valid_move(m)) << "seed " << seed;
    // every edge in the graph is a legal move of its node
    for (size_t n = 0; n < agent.mNodes.size(); ++n) {
      const auto& node = agent.mNodes[n];
      for (uint i = 0; i < node.edge_count; ++i) {
        R::Move em = c.action_encoder.idx_to_move(agent.mEdges[node.edge_begin + i].midx);
        EXPECT_TRUE(node.gs.is_valid_move(em));
      }
    }
  }
}

// ----------------------------------------------------------------- MCGS

TEST(ZeroMCGSAgent, RootVisitsEqualSimulations) {
  Container c = make_container();
  constexpr uint sims = 200;
  auto agent = make_agent(c, sims, 3);
  R::ZeroEpisodicExpCollector exp(4, R::ZeroGoStateEncoder<T::TSZ>().state_size(), T::TASZ, t::kCPU);
  agent.set_exp(exp);
  (void)agent.select_move(R::GoGameState<T::TSZ>());

  auto counts = agent.root_visit_counts();
  EXPECT_EQ(std::accumulate(counts.begin(), counts.end(), 0U), sims);
  // the recorded policy target is exactly the root visit counts
  exp.complete_episode(0.F);
  t::Tensor vc = exp.experience().visit_counts;
  ASSERT_EQ(vc.size(0), 1);
  for (uint i = 0; i < T::TASZ; ++i)
    EXPECT_EQ(vc[0][i].item<float>(), float(counts[i])) << "action " << i;
}

TEST(ZeroMCGSAgent, ForcedMoveNeedsNoSearch) {
  // white owns the whole board except two one-point eyes: black's only
  // legal move is to pass (playing an eye is suicide)
  R::GoBoard<T::TSZ> board;
  for (int r = 0; r < T::TSZ; ++r)
    for (int col = 0; col < T::TSZ; ++col)
      if (not ((r == 0 and col == 0) or (r == 8 and col == 8)))
        board.place_stone(R::Player::White, R::Pt(r, col));
  R::GoGameState<T::TSZ> gs(
    board, R::Player::Black, P(4, 4), P(4, 3), std::unordered_set<uint>());
  ASSERT_EQ(gs.legal_moves().size(), 2U);  // pass, resign

  Container c = make_container();
  auto agent = make_agent(c, 500, 1);
  R::ZeroEpisodicExpCollector exp(4, R::ZeroGoStateEncoder<T::TSZ>().state_size(), T::TASZ, t::kCPU);
  agent.set_exp(exp);
  EXPECT_EQ(agent.select_move(gs).mty, R::M::Pass);
  EXPECT_EQ(agent.num_evaluations(), 1U);
  EXPECT_EQ(agent.num_nodes(), 1U);

  exp.complete_episode(0.F);
  t::Tensor vc = exp.experience().visit_counts[0];
  EXPECT_EQ(vc[PASS_IDX].item<float>(), 1.F);
  EXPECT_EQ(vc.sum().item<float>(), 1.F);
}

TEST(ZeroMCGSAgent, TranspositionsShareNodesAndEvaluations) {
  // priors on four points only: different move orders of the same stones
  // meet constantly
  Container c = make_container();
  c.model->zero_weights();
  c.model->fixed_value = 0.F;
  focus_priors(c, {{2, 2}, {2, 6}, {6, 2}, {6, 6}});
  Inspect agent = make_agent<Inspect>(c, 400, 7);
  (void)agent.select_move(R::GoGameState<T::TSZ>());

  const auto& nodes = agent.mNodes;
  // more linked edges than non-root nodes: some nodes have several parents
  size_t linked = 0;
  for (const auto& e : agent.mEdges) linked += (e.child >= 0);
  EXPECT_GT(linked, nodes.size() - 1);

  // each position is stored once and evaluated once
  size_t terminal = 0;
  for (size_t a = 0; a < nodes.size(); ++a) {
    terminal += nodes[a].terminal;
    if (nodes[a].terminal) continue;
    for (size_t b = a + 1; b < nodes.size(); ++b)
      if (not nodes[b].terminal)
        ASSERT_FALSE(nodes[a].gs.same_position(nodes[b].gs)) << a << " vs " << b;
  }
  EXPECT_EQ(agent.num_evaluations(), nodes.size() - terminal);
  EXPECT_LT(agent.num_evaluations(), 400U);  // transpositions cost no network call
}

TEST(ZeroMCGSAgent, RootValueIsConsistentWithItsChildren) {
  Container c = make_container();  // random network: non-trivial values
  Inspect agent = make_agent<Inspect>(c, 300, 11);
  (void)agent.select_move(R::GoGameState<T::TSZ>());

  // Q(root) = (U + sum_e N(e) * -Q(child)) / (1 + sum_e N(e))
  const auto& root = agent.mNodes[0];
  double sum = root.value;
  uint visits = 1;
  for (uint i = 0; i < root.edge_count; ++i) {
    const auto& e = agent.mEdges[root.edge_begin + i];
    if (e.visits == 0) continue;
    visits += e.visits;
    sum += e.visits * -double(agent.mNodes[e.child].q);
  }
  EXPECT_EQ(root.visits, visits);
  EXPECT_NEAR(agent.root_value(), sum / visits, 1E-4);
  EXPECT_LE(std::abs(agent.root_value()), 1.F);
}

TEST(ZeroMCGSAgent, DeterministicForASeed) {
  Container c = make_container();
  auto gs = T::play<T::TSZ>({P(4, 4), P(2, 3)});
  auto a1 = make_agent(c, 150, 42);
  auto a2 = make_agent(c, 150, 42);
  R::Move m1 = a1.select_move(gs);
  R::Move m2 = a2.select_move(gs);
  EXPECT_EQ(m1, m2);
  EXPECT_EQ(a1.root_visit_counts(), a2.root_visit_counts());
  EXPECT_EQ(a1.num_nodes(), a2.num_nodes());
}

TEST(ZeroMCGSAgent, PlaysAFullSelfPlayGame) {
  // small searches until the game ends; every move must be legal
  Container c = make_container();
  auto black = make_agent(c, 16, 1, 0.25F);
  auto white = make_agent(c, 16, 2, 0.25F);
  R::GoGameState<T::TSZ> gs;
  uint moves = 0;
  while (not gs.is_over() and moves < 400) {
    R::Move m = (gs.next_player() == R::Player::Black)
      ? black.select_move(gs) : white.select_move(gs);
    ASSERT_TRUE(gs.is_valid_move(m)) << "move " << moves;
    gs.apply_move(m);
    ++moves;
  }
  EXPECT_TRUE(gs.is_over());
}

TEST(ZeroMCGSAgent, SharedNodeVisitsNeverExceedBestIncomingEdge) {
  // the KataGo stop rule: a simulation only descends through an edge that
  // has at least as many visits as the node it reaches, so a node shared by
  // several parents is not searched once per parent
  Container c = make_container();
  c.model->zero_weights();
  c.model->fixed_value = 0.F;
  focus_priors(c, {{2, 2}, {2, 6}, {6, 2}, {6, 6}});
  Inspect agent = make_agent<Inspect>(c, 600, 5);
  (void)agent.select_move(R::GoGameState<T::TSZ>());

  const auto& nodes = agent.mNodes;
  std::vector<uint> best_incoming(nodes.size(), 0);
  std::vector<uint> parents(nodes.size(), 0);
  for (const auto& e : agent.mEdges) {
    if (e.child < 0) continue;
    best_incoming[e.child] = std::max(best_incoming[e.child], e.visits);
    parents[e.child] += 1;
  }
  size_t shared = 0;
  for (size_t n = 1; n < nodes.size(); ++n) {
    if (nodes[n].terminal) continue;
    shared += (parents[n] > 1);
    EXPECT_LE(nodes[n].visits, best_incoming[n]) << "node " << n;
  }
  EXPECT_GT(shared, 0U);
}
