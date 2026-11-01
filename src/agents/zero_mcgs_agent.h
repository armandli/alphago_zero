#ifndef RLGAMES_ZERO_MCGS_AGENT
#define RLGAMES_ZERO_MCGS_AGENT

#include <cassert>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <array>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <torch/torch.h>

#include <type_alias.h>
#include <types.h>
#include <pytorch_util.h>
#include <zero_episodic_exp.h>
#include <agent_base.h>

namespace rlgames {

namespace s = std;
namespace t = torch;

// AlphaZero agent searching with Monte-Carlo graph search (MCGS).
//
// Positions reached by different move orders share one node, so every
// position is evaluated by the network once. Values follow "Monte-Carlo
// Graph Search from First Principles" (KataGo): visit counts live on edges,
// and a node's value is recomputed from its children on every backup,
//   Q(n) = (U(n) + sum_e N(e) * -Q(child(e))) / (1 + sum_e N(e))
// with U the network value (or the real result for finished games) and the
// sign flip because the player to move alternates. A node shared by several
// parents therefore gives all of them the same, complete estimate. A
// simulation that follows an edge with fewer visits than the node it leads
// to stops there and backs up that node's current value without a network
// call; so does one that would revisit a node already on its own path.
//
// Selection is PUCT. Unvisited edges use the prior-weighted mix of the
// node's value and its visited children as their value estimate. Dirichlet
// noise is mixed into the root priors once per search. The policy target
// recorded for training is the root edge visit counts, as with MCTS.
//
// GameState must provide position_key() and same_position() (see
// GoGameState) in addition to the usual game interface.
template <
  typename Model,
  typename NoiseDist,
  typename RGen,
  typename Board,
  typename GameState,
  uint BF>
struct ZeroMCGSAgent :
    public AgentBase<Board, GameState,
      ZeroMCGSAgent<Model, NoiseDist, RGen, Board, GameState, BF>>
{
  static constexpr float MIN_SCORE = -1.F;
  static constexpr float MAX_SCORE =  1.F;
  static constexpr float TIE_SCORE =  0.F;

  ZeroMCGSAgent(
      Model& model,
      t::Device device,
      uint simulations,
      float exploration_factor,
      float noise_alpha,
      float noise_factor,
      uint seed):
    mModel(model),
    mNoise(noise_alpha),
    mDevice(device),
    mGen(seed),
    mExp(nullptr),
    mSimulations(simulations),
    mEFactor(exploration_factor),
    mNoiseFactor(noise_factor)
  {}

  [[nodiscard]] Move select_move(const GameState& gs) {
    assert(not gs.is_over());

    // search is pure inference: BatchNorm must use its running statistics
    // and no autograd graph is needed; train() switches back
    t::NoGradGuard no_grad;
    mModel.model->eval();

    reset();
    int root = new_node(GameState(gs));
    insert_position(root);
    evaluate(root);
    init_root_priors();

    // a forced move needs no search
    if (mNodes[root].edge_count > 1)
      for (uint i = 0; i < mSimulations; ++i)
        simulate();

    append_experience();
    return mModel.action_encoder.idx_to_move(best_root_edge_action());
  }

  void set_exp(ZeroEpisodicExpCollector& exp) {
    mExp = &exp;
  }

  // statistics of the last search, for tests and diagnostics
  [[nodiscard]] size_t num_nodes() const { return mNodes.size(); }
  [[nodiscard]] uint num_evaluations() const { return mEvaluations; }
  [[nodiscard]] float root_value() const { return mNodes.at(0).q; }
  [[nodiscard]] s::array<uint, BF> root_visit_counts() const {
    s::array<uint, BF> counts{};
    const Node& root = mNodes.at(0);
    for (uint i = 0; i < root.edge_count; ++i) {
      const Edge& e = mEdges[root.edge_begin + i];
      counts[e.midx] = e.visits;
    }
    return counts;
  }

protected:
  struct Edge {
    uint  midx;          // action index
    float prior;         // network prior, renormalized over legal moves
    int   child  = -1;
    uint  visits = 0;
  };
  struct Node {
    GameState gs;
    uint      edge_begin = 0;
    uint      edge_count = 0;
    float     value      = 0.F;  // U: network value or real result
    float     q          = 0.F;  // from the perspective of gs.next_player()
    uint      visits     = 0;
    bool      terminal   = false;
    uint      path_stamp = 0;    // == mStamp while on the current path
  };

  void reset() {
    mNodes.clear();
    mEdges.clear();
    mTable.clear();
    mPath.clear();
    mEvaluations = 0;
    mStamp = 0;
    mNodes.reserve(mSimulations + 1);
  }

  // value of a finished game from the perspective of the player to move
  static float terminal_value(GameState& gs) {
    Player winner = gs.winner();
    if (winner == Player::Unknown) return TIE_SCORE;
    return (winner == gs.next_player()) ? MAX_SCORE : MIN_SCORE;
  }

  int new_node(GameState&& gs) {
    Node node{s::move(gs)};
    if (node.gs.is_over()) {
      node.terminal = true;
      node.value    = terminal_value(node.gs);
      node.q        = node.value;
      node.visits   = 1;
    }
    mNodes.push_back(s::move(node));
    return static_cast<int>(mNodes.size()) - 1;
  }

  int find_position(const GameState& gs) const {
    auto [first, last] = mTable.equal_range(gs.position_key());
    for (auto it = first; it != last; ++it)
      if (mNodes[it->second].gs.same_position(gs))
        return it->second;
    return -1;
  }

  void insert_position(int n) {
    mTable.emplace(mNodes[n].gs.position_key(), n);
  }

  // network evaluation of a non-terminal node: value and edges
  void evaluate(int n) {
    assert(not mNodes[n].terminal);
    ++mEvaluations;

    TensorP state = mModel.state_encoder.encode_state(mNodes[n].gs, mDevice);
    TensorP avout = mModel.model->forward(state);
    t::Tensor tpriors = avout.x.to(t::Device(t::kCPU), t::kFloat).contiguous();
    assert(tpriors.numel() == BF);
    const float* priors = tpriors.data_ptr<float>();
    float value = avout.y.item().to<float>();

    // one edge per distinct legal action; resign maps onto the pass index
    // and is never searched
    s::array<bool, BF> seen{};
    uint begin = static_cast<uint>(mEdges.size());
    float prior_sum = 0.F;
    for (const Move& m : mNodes[n].gs.legal_moves()) {
      if (m.mty == M::Resign) continue;
      uint midx = mModel.action_encoder.move_to_idx(m);
      assert(midx < BF);
      if (seen[midx]) continue;
      seen[midx] = true;
      Edge e;
      e.midx  = midx;
      e.prior = s::max(priors[midx], 0.F);
      prior_sum += e.prior;
      mEdges.push_back(e);
    }
    uint count = static_cast<uint>(mEdges.size()) - begin;
    assert(count > 0);  // pass is always legal in a running game
    for (uint i = begin; i < begin + count; ++i)
      mEdges[i].prior = (prior_sum > 0.F) ? mEdges[i].prior / prior_sum
                                          : 1.F / static_cast<float>(count);

    Node& node = mNodes[n];
    node.edge_begin = begin;
    node.edge_count = count;
    node.value      = value;
    node.q          = value;
    node.visits     = 1;
  }

  // root priors with Dirichlet noise, sampled once per search
  void init_root_priors() {
    const Node& root = mNodes[0];
    mRootPrior.resize(root.edge_count);
    for (uint i = 0; i < root.edge_count; ++i)
      mRootPrior[i] = mEdges[root.edge_begin + i].prior;
    if (mNoiseFactor <= 0.F or root.edge_count < 2) return;

    s::array<float, BF> noise = mNoise(mGen);
    float sum = 0.F;
    for (uint i = 0; i < root.edge_count; ++i)
      sum += noise[mEdges[root.edge_begin + i].midx];
    if (not (sum > 0.F)) return;
    for (uint i = 0; i < root.edge_count; ++i) {
      float n = noise[mEdges[root.edge_begin + i].midx] / sum;
      mRootPrior[i] = (1.F - mNoiseFactor) * mRootPrior[i] + mNoiseFactor * n;
    }
  }

  // one simulation: descend, add at most one node, back up
  void simulate() {
    ++mStamp;
    mPath.clear();
    int n = 0;
    while (true) {
      mNodes[n].path_stamp = mStamp;
      int e = select_edge(n);
      mPath.emplace_back(n, e);

      if (mEdges[e].child < 0) {
        GameState g = mNodes[n].gs;
        g.apply_move(mModel.action_encoder.idx_to_move(mEdges[e].midx));
        if (not g.is_over()) {
          int existing = find_position(g);
          if (existing >= 0) {
            // transposition: link to the shared node, no network call
            mEdges[e].child = existing;
            break;
          }
        }
        int c = new_node(s::move(g));
        mEdges[e].child = c;
        if (not mNodes[c].terminal) {
          insert_position(c);
          evaluate(c);
        }
        break;
      }

      int c = mEdges[e].child;
      const Node& child = mNodes[c];
      if (child.terminal
          or mEdges[e].visits < child.visits
          or child.path_stamp == mStamp)
        break;
      n = c;
    }
    backup();
  }

  void backup() {
    for (auto it = mPath.rbegin(); it != mPath.rend(); ++it) {
      mEdges[it->second].visits += 1;
      update_node(it->first);
    }
  }

  // edge value from the parent's perspective
  float edge_q(const Edge& e) const {
    return -mNodes[e.child].q;
  }

  void update_node(int n) {
    Node& node = mNodes[n];
    uint visits = 1;
    float sum = node.value;
    for (uint i = 0; i < node.edge_count; ++i) {
      const Edge& e = mEdges[node.edge_begin + i];
      if (e.visits == 0) continue;
      visits += e.visits;
      sum += static_cast<float>(e.visits) * edge_q(e);
    }
    node.visits = visits;
    node.q = sum / static_cast<float>(visits);
  }

  int select_edge(int n) {
    const Node& node = mNodes[n];
    assert(node.edge_count > 0);
    const float* priors = (n == 0) ? mRootPrior.data() : nullptr;

    uint sum_visits = 0;
    float sum_p = 0.F;
    float sum_pq = 0.F;
    for (uint i = 0; i < node.edge_count; ++i) {
      const Edge& e = mEdges[node.edge_begin + i];
      if (e.visits == 0) continue;
      float p = priors ? priors[i] : e.prior;
      sum_visits += e.visits;
      sum_p += p;
      sum_pq += p * edge_q(e);
    }
    // value estimate for unvisited edges
    float v_mix = node.value;
    if (sum_visits > 0 and sum_p > 0.F)
      v_mix = (node.value + static_cast<float>(sum_visits) * sum_pq / sum_p)
            / (1.F + static_cast<float>(sum_visits));
    float explore =
      mEFactor * s::sqrt(static_cast<float>(s::max(sum_visits, 1U)));

    mScores.resize(node.edge_count);
    for (uint i = 0; i < node.edge_count; ++i) {
      const Edge& e = mEdges[node.edge_begin + i];
      float p = priors ? priors[i] : e.prior;
      float q = (e.visits == 0) ? v_mix : edge_q(e);
      mScores[i] = q + explore * p / (1.F + static_cast<float>(e.visits));
    }
    return static_cast<int>(node.edge_begin) + random_argmax(mScores);
  }

  uint best_root_edge_action() {
    const Node& root = mNodes[0];
    mScores.resize(root.edge_count);
    for (uint i = 0; i < root.edge_count; ++i)
      mScores[i] = static_cast<float>(mEdges[root.edge_begin + i].visits);
    return mEdges[root.edge_begin + random_argmax(mScores)].midx;
  }

  void append_experience() {
    if (not mExp) return;
    const Node& root = mNodes[0];
    TensorP state = mModel.state_encoder.encode_state(root.gs, mDevice);
    s::vector<float> visit_counts(BF, 0.F);
    uint total = 0;
    for (uint i = 0; i < root.edge_count; ++i) {
      const Edge& e = mEdges[root.edge_begin + i];
      visit_counts[e.midx] = static_cast<float>(e.visits);
      total += e.visits;
    }
    // forced move: no search ran, the only move is the target
    if (total == 0 and root.edge_count > 0)
      visit_counts[mEdges[root.edge_begin].midx] = 1.F;
    mExp->append(state, visit_counts);
  }

  // index of a maximum, ties broken uniformly at random
  uint random_argmax(const s::vector<float>& v) {
    assert(not v.empty());
    float best = -s::numeric_limits<float>::infinity();
    uint count = 0;
    uint choice = 0;
    for (uint i = 0; i < v.size(); ++i) {
      if (v[i] > best) {
        best = v[i];
        count = 1;
        choice = i;
      } else if (v[i] == best) {
        // reservoir sampling over the tied maxima
        ++count;
        if (mGen() % count == 0) choice = i;
      }
    }
    return choice;
  }

  Model&                               mModel;
  NoiseDist                            mNoise;
  t::Device                            mDevice;
  RGen                                 mGen;
  ZeroEpisodicExpCollector*            mExp;
  uint                                 mSimulations;
  float                                mEFactor;
  float                                mNoiseFactor;

  s::vector<Node>                      mNodes;
  s::vector<Edge>                      mEdges;
  s::unordered_multimap<uint64, int>   mTable;
  s::vector<s::pair<int, int>>         mPath;
  s::vector<float>                     mRootPrior;
  s::vector<float>                     mScores;
  uint                                 mEvaluations = 0;
  uint                                 mStamp       = 0;
};

}  // namespace rlgames

#endif  // RLGAMES_ZERO_MCGS_AGENT
