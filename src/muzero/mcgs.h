#ifndef RLGAMES_MUZERO_MCGS
#define RLGAMES_MUZERO_MCGS

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

#include <type_alias.h>
#include <go_game.h>
#include <inference.h>

namespace rlgames::muzero {

namespace s = std;

struct MCGSConfig {
  enum class Root { Gumbel, PUCT };

  Root  root = Root::Gumbel;
  // Gumbel MuZero (Danihelka et al. 2022)
  int   max_considered_actions = 16;
  float maxvisit_init  = 50.F;   // c_visit
  float value_scale    = 0.1F;   // c_scale
  bool  rescale_values = true;   // min-max normalize completed Q per node
  // AlphaZero / MuZero PUCT
  float puct_c             = 1.25F;
  float dirichlet_alpha    = 0.15F;
  float dirichlet_fraction = 0.25F;
  // merge nodes reached by different move orders
  bool  transpositions = true;
};

struct SearchResult {
  int              action      = -1;
  s::vector<float> policy;           // training target over all actions
  float            root_value  = 0.F;
  int              simulations = 0;
};

// For each of num_simulations simulations, the visit count the root action
// it visits must have; the root picks the best scored action with that
// count, which implements Gumbel-top-k with sequential halving (as in mctx).
inline s::vector<int> gumbel_considered_visits(int max_considered, int num_simulations) {
  s::vector<int> out;
  if (max_considered <= 1) {
    for (int i = 0; i < num_simulations; ++i) out.push_back(i);
    return out;
  }
  int log2max = static_cast<int>(s::ceil(s::log2(static_cast<double>(max_considered))));
  s::vector<int> visits(max_considered, 0);
  int considered = max_considered;
  while (static_cast<int>(out.size()) < num_simulations) {
    int extra = s::max(1, num_simulations / (log2max * considered));
    for (int k = 0; k < extra; ++k) {
      out.insert(out.end(), visits.begin(), visits.begin() + considered);
      for (int i = 0; i < considered; ++i) visits[i] += 1;
    }
    considered = s::max(2, considered / 2);
  }
  out.resize(num_simulations);
  return out;
}

// Monte-Carlo graph search over a learned MuZero model.
//
// Nodes are latent states produced by the model: the root by the
// representation network, every other node by the dynamics network applied
// to its parent's latent. The real rules are still known, so the search
// follows the real game alongside each latent to mask illegal actions, give
// finished games their exact result, and detect transpositions: when a move
// order reaches a position the graph already holds (same stones, same player
// to move), the edge links to the existing node instead of asking the
// dynamics network for a new latent, so each position is evaluated once.
//
// Values follow "Monte-Carlo Graph Search from First Principles" (KataGo):
//   Q(n) = (U(n) + sum_e N(e) (r(e) - Q(child(e)))) / (1 + sum_e N(e))
// with U the network value and N(e) edge visits. A simulation that follows
// an edge with fewer visits than the node it reaches, or that would revisit
// a node on its own path, stops there and backs up that node's value
// without a network call.
//
// The search is resumable so many games can share one batched network call:
// step() advances simulations until one needs the network and adds that
// request; receive() delivers the answer. Latents live in the inference
// backend at ids [latent_base, latent_base + latent_capacity).
//
// Root selection is Gumbel MuZero (Gumbel-top-k, sequential halving,
// improved policy target softmax(logits + sigma(completed Q))), which keeps
// improving the policy with very few simulations, or PUCT with Dirichlet
// noise and visit count targets.
template <typename Game, typename Encoder>
struct MCGS {
  static constexpr int A = Game::NUM_ACTIONS;

  MCGS(const Encoder* encoder, const MCGSConfig& config,
       int64_t latent_base, int latent_capacity, uint64_t seed):
    mEncoder(encoder), mConfig(config), mLatentBase(latent_base),
    mLatentCapacity(latent_capacity), mRng(seed),
    mObservation(encoder->observation_size())
  {}

  void start(const Game& root, int num_simulations, bool add_noise) {
    mNodes.clear();
    mEdges.clear();
    mTable.clear();
    mNumSimulations = s::min(num_simulations, mLatentCapacity - 1);
    mSimulations = 0;
    mNoise = add_noise;
    mPendingNode = -1;
    mFinished = false;
    mForcedAction = -1;
    mStamp = 0;
    new_node(root);

    ActionList<A> legal;
    root.legal_actions(legal);
    assert(legal.size > 0);
    if (legal.size == 1) {
      // a forced move needs no search
      mForcedAction = legal[0];
      mFinished = true;
    }
  }

  // false when the search is finished; true when it added one request
  bool step(InferenceRequests& requests) {
    if (mFinished) return false;
    if (mPendingNode >= 0) return true;
    if (not mNodes[0].expanded) {
      mEncoder->encode(mNodes[0].state, mObservation.data());
      mRequest = requests.add_initial(mObservation.data(), latent_id(0));
      mPendingNode = 0;
      mPendingInitial = true;
      return true;
    }
    while (mSimulations < mNumSimulations)
      if (simulate(requests)) return true;
    mFinished = true;
    return false;
  }

  void receive(const InferenceResults& results) {
    assert(mPendingNode >= 0);
    int node = mPendingNode;
    mPendingNode = -1;
    if (mPendingInitial) {
      expand(node, results.initial_policy(mRequest), results.initial_values[mRequest]);
      init_root();
    } else {
      expand(node, results.recurrent_policy(mRequest), results.recurrent_values[mRequest]);
      backup();
    }
  }

  bool finished() const { return mFinished; }
  bool waiting() const { return mPendingNode >= 0; }

  SearchResult result(float temperature) {
    SearchResult r;
    r.policy.assign(A, 0.F);
    if (mForcedAction >= 0) {
      r.action = mForcedAction;
      r.policy[mForcedAction] = 1.F;
      return r;
    }
    const Node& root = mNodes[0];
    r.root_value = root.q;
    r.simulations = mSimulations;
    if (mConfig.root == MCGSConfig::Root::Gumbel) gumbel_result(r);
    else                                          puct_result(r, temperature);
    return r;
  }

  size_t num_nodes() const { return mNodes.size(); }
  int num_simulations_done() const { return mSimulations; }

protected:
  struct Edge {
    int16_t action;
    float   logit;   // log prior over legal actions
    float   prior;
    int     child  = -1;
    int     visits = 0;
    float   reward = 0.F;
  };
  struct Node {
    Game  state;
    int   edge_begin = 0;
    int   edge_count = 0;
    float value      = 0.F;  // network value U
    float q          = 0.F;
    int   visits     = 0;
    bool  terminal   = false;
    bool  expanded   = false;
    uint  stamp      = 0;    // == mStamp while on the current path
  };

  int64_t latent_id(int node) const { return mLatentBase + node; }

  int new_node(const Game& g) {
    Node n;
    n.state = g;
    n.terminal = g.is_terminal();
    if (n.terminal) n.visits = 1;
    mNodes.push_back(n);
    return static_cast<int>(mNodes.size()) - 1;
  }

  int find_position(const Game& g) const {
    auto [first, last] = mTable.equal_range(g.position_key());
    for (auto it = first; it != last; ++it)
      if (mNodes[it->second].state.same_position(g)) return it->second;
    return -1;
  }

  void expand(int n, const float* logits, float value) {
    ActionList<A> legal;
    mNodes[n].state.legal_actions(legal);
    Node& node = mNodes[n];
    node.edge_begin = static_cast<int>(mEdges.size());
    node.edge_count = legal.size;
    float mx = -s::numeric_limits<float>::infinity();
    for (int a : legal) mx = s::max(mx, logits[a]);
    float sum = 0.F;
    for (int a : legal) sum += s::exp(logits[a] - mx);
    float lse = mx + s::log(sum);
    for (int a : legal) {
      Edge e;
      e.action = static_cast<int16_t>(a);
      e.logit = logits[a] - lse;
      e.prior = s::exp(e.logit);
      mEdges.push_back(e);
    }
    node.value = value;
    node.q = value;
    node.visits = 1;
    node.expanded = true;
  }

  void init_root() {
    const Node& root = mNodes[0];
    int n = root.edge_count;
    mGumbel.assign(n, 0.F);
    mRootPrior.resize(n);
    for (int i = 0; i < n; ++i) mRootPrior[i] = mEdges[root.edge_begin + i].prior;
    if (mConfig.root == MCGSConfig::Root::Gumbel) {
      if (mNoise) {
        s::uniform_real_distribution<float> u(1e-10F, 1.F);
        for (float& g : mGumbel) g = -s::log(-s::log(u(mRng)));
      }
      int considered = s::min(mConfig.max_considered_actions, n);
      mSchedule = gumbel_considered_visits(considered, s::max(mNumSimulations, 1));
    } else if (mNoise and n > 1) {
      s::gamma_distribution<float> gamma(mConfig.dirichlet_alpha, 1.F);
      s::vector<float> noise(n);
      float sum = 0.F;
      for (float& x : noise) sum += (x = gamma(mRng));
      for (int i = 0; i < n; ++i)
        mRootPrior[i] = (1.F - mConfig.dirichlet_fraction) * mRootPrior[i]
                      + mConfig.dirichlet_fraction * noise[i] / s::max(sum, 1e-20F);
    }
  }

  // runs one simulation; returns true when it stopped at a new leaf that
  // needs the network, in which case receive() finishes it
  bool simulate(InferenceRequests& requests) {
    ++mStamp;
    mPath.clear();
    int n = 0;
    while (true) {
      mNodes[n].stamp = mStamp;
      int e = (n == 0) ? select_root() : select_interior(n);
      mPath.emplace_back(n, e);
      if (mEdges[e].child < 0) {
        Game g = mNodes[n].state;
        int mover = g.to_play();
        g.apply(mEdges[e].action);
        bool terminal = g.is_terminal();
        mEdges[e].reward = terminal ? static_cast<float>(g.outcome(mover)) : 0.F;
        if (mConfig.transpositions and not terminal) {
          int existing = find_position(g);
          if (existing >= 0) {
            mEdges[e].child = existing;
            backup();
            return false;
          }
        }
        if (static_cast<int>(mNodes.size()) >= mLatentCapacity) {
          // out of latent slots: finish with what we have
          mNumSimulations = mSimulations;
          return false;
        }
        int c = new_node(g);
        mEdges[e].child = c;
        if (terminal) {
          backup();
          return false;
        }
        if (mConfig.transpositions) mTable.emplace(g.position_key(), c);
        mRequest = requests.add_recurrent(latent_id(n), mEdges[e].action, latent_id(c));
        mPendingNode = c;
        mPendingInitial = false;
        return true;
      }
      const Node& child = mNodes[mEdges[e].child];
      if (child.terminal or mEdges[e].visits < child.visits or child.stamp == mStamp) {
        backup();
        return false;
      }
      n = mEdges[e].child;
    }
  }

  void backup() {
    for (auto it = mPath.rbegin(); it != mPath.rend(); ++it) {
      mEdges[it->second].visits += 1;
      update_node(it->first);
    }
    ++mSimulations;
  }

  float edge_q(const Node& parent, const Edge& e) const {
    const Node& child = mNodes[e.child];
    if (child.terminal) return e.reward;
    float sign = child.state.to_play() == parent.state.to_play() ? 1.F : -1.F;
    return e.reward + sign * child.q;
  }

  void update_node(int n) {
    Node& node = mNodes[n];
    int visits = 1;
    float sum = node.value;
    for (int i = 0; i < node.edge_count; ++i) {
      const Edge& e = mEdges[node.edge_begin + i];
      if (e.visits == 0) continue;
      visits += e.visits;
      sum += static_cast<float>(e.visits) * edge_q(node, e);
    }
    node.visits = visits;
    node.q = sum / static_cast<float>(visits);
  }

  // Q of visited edges, the mixed value estimate for unvisited ones; also
  // returns the edges' total and maximum visits
  void completed_q(int n, const float* priors, s::vector<float>& cq,
                   int& sum_visits, int& max_visits) const {
    const Node& node = mNodes[n];
    cq.resize(node.edge_count);
    sum_visits = 0;
    max_visits = 0;
    float sum_p = 0.F;
    float sum_pq = 0.F;
    for (int i = 0; i < node.edge_count; ++i) {
      const Edge& e = mEdges[node.edge_begin + i];
      if (e.visits == 0) continue;
      float q = edge_q(node, e);
      cq[i] = q;
      sum_visits += e.visits;
      max_visits = s::max(max_visits, e.visits);
      sum_p += priors[i];
      sum_pq += priors[i] * q;
    }
    float v_mix = node.value;
    if (sum_visits > 0 and sum_p > 0.F)
      v_mix = (node.value + sum_visits * sum_pq / sum_p) / (1.F + sum_visits);
    for (int i = 0; i < node.edge_count; ++i)
      if (mEdges[node.edge_begin + i].visits == 0) cq[i] = v_mix;
  }

  // sigma(completed Q) of Gumbel MuZero
  void sigma(s::vector<float>& cq, int max_visits) const {
    if (cq.empty()) return;
    if (mConfig.rescale_values) {
      auto [lo, hi] = s::minmax_element(cq.begin(), cq.end());
      float l = *lo;
      float range = s::max(*hi - l, 1e-8F);
      for (float& q : cq) q = (q - l) / range;
    } else {
      for (float& q : cq) q = (q + 1.F) * 0.5F;
    }
    float scale = (mConfig.maxvisit_init + max_visits) * mConfig.value_scale;
    for (float& q : cq) q *= scale;
  }

  const float* edge_priors(int n) {
    const Node& node = mNodes[n];
    if (n == 0) return mRootPrior.data();
    mPriors.resize(node.edge_count);
    for (int i = 0; i < node.edge_count; ++i) mPriors[i] = mEdges[node.edge_begin + i].prior;
    return mPriors.data();
  }

  int select_root() {
    const Node& root = mNodes[0];
    if (mConfig.root == MCGSConfig::Root::PUCT) return select_puct(0);
    int sum_visits, max_visits;
    completed_q(0, mRootPrior.data(), mScratch, sum_visits, max_visits);
    sigma(mScratch, max_visits);
    int target = mSchedule[s::min(mSimulations, static_cast<int>(mSchedule.size()) - 1)];
    int best = -1;
    int fallback = 0;
    float best_score = -s::numeric_limits<float>::infinity();
    float fallback_score = -s::numeric_limits<float>::infinity();
    for (int i = 0; i < root.edge_count; ++i) {
      const Edge& e = mEdges[root.edge_begin + i];
      float score = mGumbel[i] + e.logit + mScratch[i];
      if (e.visits == target and score > best_score) {
        best_score = score;
        best = i;
      }
      if (score > fallback_score) {
        fallback_score = score;
        fallback = i;
      }
    }
    return root.edge_begin + (best >= 0 ? best : fallback);
  }

  // deterministic interior selection of Gumbel MuZero: the action whose
  // visit share lags the improved policy the most
  int select_interior(int n) {
    if (mConfig.root == MCGSConfig::Root::PUCT) return select_puct(n);
    const Node& node = mNodes[n];
    int sum_visits, max_visits;
    completed_q(n, edge_priors(n), mScratch, sum_visits, max_visits);
    sigma(mScratch, max_visits);
    float mx = -s::numeric_limits<float>::infinity();
    for (int i = 0; i < node.edge_count; ++i) {
      mScratch[i] += mEdges[node.edge_begin + i].logit;
      mx = s::max(mx, mScratch[i]);
    }
    float sum = 0.F;
    for (float& x : mScratch) sum += (x = s::exp(x - mx));
    int best = 0;
    float best_score = -s::numeric_limits<float>::infinity();
    for (int i = 0; i < node.edge_count; ++i) {
      float score = mScratch[i] / sum
                  - static_cast<float>(mEdges[node.edge_begin + i].visits) / (1.F + sum_visits);
      if (score > best_score) {
        best_score = score;
        best = i;
      }
    }
    return node.edge_begin + best;
  }

  int select_puct(int n) {
    const Node& node = mNodes[n];
    const float* priors = edge_priors(n);
    int sum_visits, max_visits;
    completed_q(n, priors, mScratch, sum_visits, max_visits);
    float explore = mConfig.puct_c * s::sqrt(static_cast<float>(s::max(sum_visits, 1)));
    int best = 0;
    float best_score = -s::numeric_limits<float>::infinity();
    for (int i = 0; i < node.edge_count; ++i) {
      float score = mScratch[i]
                  + explore * priors[i] / (1.F + mEdges[node.edge_begin + i].visits);
      if (score > best_score) {
        best_score = score;
        best = i;
      }
    }
    return node.edge_begin + best;
  }

  void gumbel_result(SearchResult& r) {
    const Node& root = mNodes[0];
    int sum_visits, max_visits;
    completed_q(0, mRootPrior.data(), mScratch, sum_visits, max_visits);
    sigma(mScratch, max_visits);
    // action: the best scored among the most visited (the last survivors of
    // sequential halving)
    int best = 0;
    float best_score = -s::numeric_limits<float>::infinity();
    for (int i = 0; i < root.edge_count; ++i) {
      const Edge& e = mEdges[root.edge_begin + i];
      if (e.visits != max_visits) continue;
      float score = mGumbel[i] + e.logit + mScratch[i];
      if (score > best_score) {
        best_score = score;
        best = i;
      }
    }
    r.action = mEdges[root.edge_begin + best].action;
    // improved policy: softmax(logits + sigma(completed Q))
    float mx = -s::numeric_limits<float>::infinity();
    for (int i = 0; i < root.edge_count; ++i) {
      mScratch[i] += mEdges[root.edge_begin + i].logit;
      mx = s::max(mx, mScratch[i]);
    }
    float sum = 0.F;
    for (float& x : mScratch) sum += (x = s::exp(x - mx));
    for (int i = 0; i < root.edge_count; ++i)
      r.policy[mEdges[root.edge_begin + i].action] = mScratch[i] / sum;
  }

  void puct_result(SearchResult& r, float temperature) {
    const Node& root = mNodes[0];
    int total = 0;
    int best = 0;
    for (int i = 0; i < root.edge_count; ++i) {
      const Edge& e = mEdges[root.edge_begin + i];
      total += e.visits;
      if (e.visits > mEdges[root.edge_begin + best].visits) best = i;
    }
    for (int i = 0; i < root.edge_count; ++i) {
      const Edge& e = mEdges[root.edge_begin + i];
      r.policy[e.action] = total > 0 ? static_cast<float>(e.visits) / total : mRootPrior[i];
    }
    if (temperature > 0.F and total > 0) {
      s::vector<double> w(root.edge_count);
      for (int i = 0; i < root.edge_count; ++i)
        w[i] = s::pow(static_cast<double>(mEdges[root.edge_begin + i].visits), 1.0 / temperature);
      s::discrete_distribution<int> d(w.begin(), w.end());
      best = d(mRng);
    }
    r.action = mEdges[root.edge_begin + best].action;
  }

  const Encoder*                      mEncoder;
  MCGSConfig                          mConfig;
  int64_t                             mLatentBase;
  int                                 mLatentCapacity;
  s::mt19937_64                       mRng;
  s::vector<uint8_t>                  mObservation;

  s::vector<Node>                     mNodes;
  s::vector<Edge>                     mEdges;
  s::unordered_multimap<uint64, int>  mTable;
  s::vector<s::pair<int, int>>        mPath;

  int                                 mNumSimulations = 0;
  int                                 mSimulations    = 0;
  bool                                mNoise          = false;
  bool                                mFinished       = true;
  int                                 mForcedAction   = -1;
  int                                 mPendingNode    = -1;
  bool                                mPendingInitial = false;
  int                                 mRequest        = 0;
  uint                                mStamp          = 0;

  s::vector<float>                    mGumbel;
  s::vector<float>                    mRootPrior;
  s::vector<int>                      mSchedule;
  s::vector<float>                    mScratch;
  s::vector<float>                    mPriors;
};

}  // namespace rlgames::muzero

#endif  // RLGAMES_MUZERO_MCGS
