// Bug #3:  search ran in training mode with autograd enabled.
// Bug #4:  terminal positions used the network's value, not the real result.
// Bug #9:  Dirichlet noise was resampled on every branch selection at every
//          node instead of once per search at the root.
// Bug #10: illegal moves scored MIN_SCORE and could tie with (and be chosen
//          over) legal moves whose value is exactly MIN_SCORE.

#include <cmath>
#include <limits>
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
#include <zero_agent.h>

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
using Agent = R::ZeroAgent<
  Container,
  Noise<T::TASZ, float>,
  R::Splitmix,
  R::GoBoard<T::TSZ>,
  R::GoGameState<T::TSZ>,
  T::TASZ>;

Container make_container() {
  return Container(
    T::TestModel(),
    R::ZeroGoStateEncoder<T::TSZ>(),
    R::ZeroGoActionEncoder<T::TSZ>(),
    1E-3F);
}

template <template <uint, typename> class Noise = R::null_distribution>
Agent<Noise> make_agent(
    Container& c,
    uint expansions,
    uint seed,
    float noise_factor = 0.F)
{
  return Agent<Noise>(c, t::kCPU, expansions, 0.5F, 0.03F, noise_factor, seed);
}

R::ZeroExperience random_experience(int n) {
  return R::ZeroExperience(
    t::randn({n, T::TPLANE, T::TSZ, T::TSZ}),
    t::zeros({n, 2}),
    t::rand({n, T::TASZ}) + 0.1F,
    t::ones({n}));
}

}  // namespace

TEST(ZeroAgent, SearchRunsInEvalModeWithoutAutograd) {
  Container c = make_container();
  // give BatchNorm non-trivial running statistics first
  R::ZeroExperience warmup = random_experience(8);
  (void)R::train(c, warmup);
  t::Tensor mean_before = c.model->bn->running_mean.clone();
  t::Tensor var_before  = c.model->bn->running_var.clone();

  auto agent = make_agent(c, 20, 1);
  (void)agent.select_move(R::GoGameState<T::TSZ>());

  EXPECT_GT(c.model->forward_calls, 0);
  EXPECT_FALSE(c.model->last_training);
  EXPECT_FALSE(c.model->last_grad_enabled);
  // eval-mode BatchNorm must not update its running statistics
  EXPECT_TRUE(t::equal(c.model->bn->running_mean, mean_before));
  EXPECT_TRUE(t::equal(c.model->bn->running_var, var_before));
  // the guard is scoped to the search
  EXPECT_TRUE(t::GradMode::is_enabled());

  // training switches back to training mode with gradients
  R::ZeroExperience exp = random_experience(8);
  (void)R::train(c, exp);
  EXPECT_TRUE(c.model->is_training());
  EXPECT_TRUE(c.model->last_training);
  EXPECT_TRUE(c.model->last_grad_enabled);
}

TEST(ZeroAgent, PassesWhenPassingWinsTheFinishedGame) {
  // black wall on column 4, white has only passed; black to move.
  // passing ends the game and black wins on the board.
  std::vector<R::Move> moves;
  for (int r = 0; r < 9; ++r) { moves.push_back(P(r, 4)); moves.push_back(Pass()); }
  auto gs = T::play<T::TSZ>(moves);
  ASSERT_EQ(gs.next_player(), R::Player::Black);

  Container c = make_container();
  c.model->zero_weights();      // uniform priors
  c.model->fixed_value = 1.F;   // network: "player to move is winning"
  // With the old code the terminal pass node took the network's +1 for
  // white, i.e. -1 for black, indistinguishable from every other move.
  for (uint seed = 1; seed <= 5; ++seed) {
    auto agent = make_agent(c, 300, seed);
    EXPECT_EQ(agent.select_move(gs).mty, R::M::Pass) << "seed " << seed;
  }
}

TEST(ZeroAgent, AvoidsPassWhenPassingLosesTheFinishedGame) {
  // white wall on column 4; black has two stones on the right side;
  // white just passed. passing ends the game and black loses.
  std::vector<R::Move> moves = {P(4, 8), P(0, 4)};
  for (int r = 1; r < 9; ++r) { moves.push_back(Pass()); moves.push_back(P(r, 4)); }
  moves.push_back(P(3, 8));
  moves.push_back(Pass());
  auto gs = T::play<T::TSZ>(moves);
  ASSERT_EQ(gs.next_player(), R::Player::Black);
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

TEST(ZeroAgent, NoiseSampledOncePerSearch) {
  Container c = make_container();
  auto agent = make_agent<T::CountingNoise>(c, 50, 1, 0.25F);
  T::CountingNoise<T::TASZ, float>::calls = 0;
  (void)agent.select_move(R::GoGameState<T::TSZ>());
  EXPECT_EQ((T::CountingNoise<T::TASZ, float>::calls), 1);
  (void)agent.select_move(R::GoGameState<T::TSZ>());
  EXPECT_EQ((T::CountingNoise<T::TASZ, float>::calls), 2);
}

TEST(ZeroAgent, NeverSelectsIllegalMoveOnScoreTie) {
  // black A1, white B1; black to move. All prior mass sits on the occupied
  // A1 point, so every legal move has prior exactly 0, and the network value
  // makes every visited legal move score exactly -1 (the old MIN_SCORE that
  // illegal moves were initialised with).
  auto gs = T::play<T::TSZ>({P(0, 0), P(0, 1)});

  Container c = make_container();
  c.model->zero_weights();
  c.model->fixed_value = 1.F;
  c.model->policy_bias.fill_(-std::numeric_limits<float>::infinity());
  c.model->policy_bias[0] = 0.F;

  for (uint seed = 1; seed <= 5; ++seed) {
    auto agent = make_agent(c, 400, seed);
    // the old code would expand an illegal move (asserting in place_stone)
    R::Move m = agent.select_move(gs);
    EXPECT_TRUE(gs.is_valid_move(m)) << "seed " << seed;
  }
}
