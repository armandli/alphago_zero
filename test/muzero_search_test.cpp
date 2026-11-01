// MuZero MCGS against a fake network, so the search logic (real terminal
// rewards, transpositions, Gumbel schedule, latent capacity) is isolated.

#include <algorithm>
#include <numeric>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include <go_game.h>
#include <go_encoder.h>
#include <inference.h>
#include <mcgs.h>

namespace Z = rlgames::muzero;

using Game   = Z::GoGame<9>;
using Enc    = Z::GoEncoder<9>;
using Search = Z::MCGS<Game, Enc>;

namespace {

// constant network: logits = bias (0 except configured actions), value v
struct FakeInference : Z::Inference {
  std::vector<float> bias = std::vector<float>(Game::NUM_ACTIONS, 0.F);
  float value = 0.F;
  int calls = 0, initial = 0, recurrent = 0;
  std::set<int64_t> latents_written;

  void infer(const Z::InferenceRequests& req, Z::InferenceResults& res) override {
    ++calls;
    res.num_actions = Game::NUM_ACTIONS;
    int ni = req.num_initial(), nr = req.num_recurrent();
    initial += ni;
    recurrent += nr;
    res.initial_logits.clear();
    res.recurrent_logits.clear();
    for (int i = 0; i < ni; ++i) {
      res.initial_logits.insert(res.initial_logits.end(), bias.begin(), bias.end());
      latents_written.insert(req.initial_latent[i]);
    }
    for (int i = 0; i < nr; ++i) {
      res.recurrent_logits.insert(res.recurrent_logits.end(), bias.begin(), bias.end());
      latents_written.insert(req.child_latent[i]);
    }
    res.initial_values.assign(ni, value);
    res.recurrent_values.assign(nr, value);
    res.recurrent_rewards.assign(nr, 0.F);
  }
  int num_actions() const override { return Game::NUM_ACTIONS; }
  int observation_size() const override { return Enc::OBSERVATION_SIZE; }
  int64_t latent_capacity() const override { return 1 << 20; }
};

// runs a search to the end; checks every step makes at most one request
Z::SearchResult run(Search& search, FakeInference& net, float temperature = 0.F) {
  Z::InferenceRequests req;
  req.observation_size = Enc::OBSERVATION_SIZE;
  Z::InferenceResults res;
  while (true) {
    req.clear();
    if (not search.step(req)) break;
    EXPECT_EQ(req.size(), 1);
    net.infer(req, res);
    search.receive(res);
  }
  EXPECT_TRUE(search.finished());
  return search.result(temperature);
}

Z::MCGSConfig config(Z::MCGSConfig::Root root, bool transpositions = true) {
  Z::MCGSConfig c;
  c.root = root;
  c.transpositions = transpositions;
  return c;
}

// black wall on column 4, white has only passed: passing wins for black
Game black_wins_by_passing() {
  Game g;
  for (int r = 0; r < 9; ++r) { g.apply(r * 9 + 4); g.apply(Game::PASS); }
  return g;
}

const Z::MCGSConfig::Root ROOTS[] = {Z::MCGSConfig::Root::Gumbel, Z::MCGSConfig::Root::PUCT};

}  // namespace

TEST(GumbelConsideredVisits, SequentialHalvingSchedule) {
  EXPECT_EQ(Z::gumbel_considered_visits(4, 8), (std::vector<int>{0, 0, 0, 0, 1, 1, 2, 2}));
  EXPECT_EQ(Z::gumbel_considered_visits(1, 4), (std::vector<int>{0, 1, 2, 3}));
  for (int m : {2, 5, 16})
    for (int n : {1, 7, 64}) {
      auto seq = Z::gumbel_considered_visits(m, n);
      ASSERT_EQ(static_cast<int>(seq.size()), n);
      for (int i = 0; i < std::min(m, n); ++i) EXPECT_EQ(seq[i], 0);
    }
}

TEST(MuZeroMCGS, ResultIsAValidLegalPolicy) {
  Enc enc;
  for (auto root : ROOTS) {
    FakeInference net;
    Search search(&enc, config(root), 0, 66, 1);
    Game g;
    g.apply(40);
    search.start(g, 64, true);
    Z::SearchResult r = run(search, net, 1.F);
    float sum = std::accumulate(r.policy.begin(), r.policy.end(), 0.F);
    EXPECT_NEAR(sum, 1.F, 1e-4);
    EXPECT_EQ(r.policy[40], 0.F);       // occupied point gets nothing
    EXPECT_TRUE(g.is_legal(r.action));
    EXPECT_EQ(r.simulations, 64);
  }
}

TEST(MuZeroMCGS, ForcedMoveNeedsNoNetwork) {
  // white owns everything but two one-point eyes: black can only pass
  Enc enc;
  Game g;
  for (int i = 0; i < 81; ++i) {
    if (i == 0 or i == 80) continue;
    // alternate so white places every stone: black passes in between
    g.apply(Game::PASS);
    g.apply(i);
  }
  ASSERT_EQ(g.to_play(), 0);
  FakeInference net;
  Search search(&enc, config(Z::MCGSConfig::Root::Gumbel), 0, 34, 1);
  search.start(g, 32, true);
  Z::SearchResult r = run(search, net);
  EXPECT_EQ(r.action, Game::PASS);
  EXPECT_EQ(r.policy[Game::PASS], 1.F);
  EXPECT_EQ(net.calls, 0);
}

TEST(MuZeroMCGS, UsesRealTerminalRewards) {
  // the network sees nothing (value 0 everywhere); only the real result of
  // passing distinguishes the moves
  Enc enc;
  Game g = black_wins_by_passing();
  for (auto root : ROOTS)
    for (uint64_t seed = 1; seed <= 3; ++seed) {
      FakeInference net;
      net.bias[Game::PASS] = 2.F;  // so Gumbel-top-k considers it
      Search search(&enc, config(root), 0, 130, seed);
      search.start(g, 128, false);
      Z::SearchResult r = run(search, net);
      EXPECT_EQ(r.action, Game::PASS) << (root == Z::MCGSConfig::Root::PUCT) << " " << seed;
      EXPECT_GT(r.root_value, 0.F);
    }
}

TEST(MuZeroMCGS, TranspositionsSaveNetworkCalls) {
  // priors on four points only: move orders of the same stones meet
  Enc enc;
  auto focused = [] {
    FakeInference net;
    std::fill(net.bias.begin(), net.bias.end(), -20.F);
    for (int a : {20, 24, 56, 60}) net.bias[a] = 0.F;
    return net;
  };
  FakeInference with = focused(), without = focused();
  Search a(&enc, config(Z::MCGSConfig::Root::PUCT, true), 0, 302, 3);
  Search b(&enc, config(Z::MCGSConfig::Root::PUCT, false), 0, 302, 3);
  a.start(Game(), 300, false);
  b.start(Game(), 300, false);
  run(a, with);
  run(b, without);
  EXPECT_LT(with.recurrent, without.recurrent);
  EXPECT_LT(a.num_nodes(), b.num_nodes());
}

TEST(MuZeroMCGS, StaysWithinItsLatentRange) {
  Enc enc;
  FakeInference net;
  Search search(&enc, config(Z::MCGSConfig::Root::Gumbel), 1000, 10, 4);
  search.start(Game(), 500, true);  // more simulations than slots
  Z::SearchResult r = run(search, net);
  EXPECT_LE(r.simulations, 9);
  EXPECT_LE(search.num_nodes(), 10U);
  for (int64_t id : net.latents_written) {
    EXPECT_GE(id, 1000);
    EXPECT_LT(id, 1010);
  }
}

TEST(MuZeroMCGS, DeterministicForASeed) {
  Enc enc;
  for (auto root : ROOTS) {
    FakeInference n1, n2;
    Search a(&enc, config(root), 0, 66, 9), b(&enc, config(root), 0, 66, 9);
    Game g;
    g.apply(30);
    a.start(g, 64, true);
    b.start(g, 64, true);
    Z::SearchResult ra = run(a, n1, 1.F), rb = run(b, n2, 1.F);
    EXPECT_EQ(ra.action, rb.action);
    EXPECT_EQ(ra.policy, rb.policy);
  }
}
