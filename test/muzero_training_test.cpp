// MuZero training: batch targets under symmetries, learner losses and
// checkpoints, evaluation (Elo, arena, baselines), a full training run and
// its resume.

#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <torch/torch.h>

#include <go_game.h>
#include <go_encoder.h>
#include <experience.h>
#include <selfplay.h>
#include <learner.h>
#include <evaluation.h>
#include <training_loop.h>

namespace Z  = rlgames::muzero;
namespace t  = torch;
namespace fs = std::filesystem;

using Game = Z::GoGame<9>;
using Enc  = Z::GoEncoder<9>;

namespace {

Z::TransformerMuZeroConfig tiny_config() {
  Z::TransformerMuZeroConfig c;
  c.dims = 32; c.num_heads = 4; c.repr_layers = 1; c.dyn_layers = 1;
  c.pred_layers = 1; c.proj_dims = 16;
  return c;
}

// a recorded random game; every move a full search with a one-hot policy
std::shared_ptr<Z::Trajectory> random_game(std::mt19937_64& rng, const Enc& enc, int max_moves = 40) {
  Z::TrajectoryRecorder<Game, Enc> rec(&enc);
  auto tr = rec.begin(0);
  Game g(7.5F, max_moves);
  Z::ActionList<Game::NUM_ACTIONS> legal;
  while (not g.is_terminal()) {
    g.legal_actions(legal);
    Z::SearchResult r;
    r.action = legal[std::uniform_int_distribution<int>(0, legal.size - 1)(rng)];
    r.policy.assign(Game::NUM_ACTIONS, 0.F);
    r.policy[r.action] = 1.F;
    r.root_value = 0.F;
    rec.move(*tr, g, r, true);
  }
  rec.finish(*tr, g);
  return tr;
}

t::Device test_device() {
  return t::cuda::is_available() ? t::Device(t::kCUDA) : t::Device(t::kCPU);
}

fs::path fresh_dir(const std::string& name) {
  fs::path d = fs::temp_directory_path() / name;
  fs::remove_all(d);
  return d;
}

}  // namespace

// ------------------------------------------------------------------ batch

TEST(BatchBuilder, TargetsAndMasksAtTheEndOfAGame) {
  Enc enc;
  std::mt19937_64 rng(1);
  auto tr = random_game(rng, enc);
  Z::BatchBuilder<Enc> builder(&enc, 1, 5, false);
  Z::TrainingBatch b;
  b.resize(1, 5, 81, Enc::NUM_FEATURES, 82);
  int t0 = tr->length - 2;  // two moves before the end
  builder.fill(*tr, t0, 0, 0, b);
  // states t0, t0+1, t0+2 exist; t0+2 is terminal
  EXPECT_EQ(b.state_mask, (std::vector<float>{1, 1, 1, 0, 0, 0}));
  EXPECT_EQ(b.value_mask, (std::vector<float>{1, 1, 0, 0, 0, 0}));
  EXPECT_EQ(b.policy_mask, (std::vector<float>{1, 1, 0, 0, 0, 0}));
  EXPECT_EQ(b.reward_mask, (std::vector<float>{1, 1, 0, 0, 0}));
  EXPECT_EQ(b.actions[0], tr->actions[t0]);
  EXPECT_EQ(b.actions[1], tr->actions[t0 + 1]);
  EXPECT_EQ(b.values[0], tr->value_target(t0) + 1);
  // the last move's reward is the result for its mover
  EXPECT_EQ(b.rewards[1], tr->rewards[t0 + 1] + 1);
  EXPECT_NE(tr->rewards[t0 + 1], 0);
  EXPECT_EQ(b.policies[tr->actions[t0]], 1.F);
}

TEST(BatchBuilder, SymmetryMovesEveryTargetTogether) {
  Enc enc;
  std::mt19937_64 rng(2);
  auto tr = random_game(rng, enc);
  Z::BatchBuilder<Enc> builder(&enc, 1, 3, true);
  int T = 81, F = Enc::NUM_FEATURES, A = 82;
  for (int sym = 0; sym < 8; ++sym) {
    Z::TrainingBatch b;
    b.resize(1, 3, T, F, A);
    builder.fill(*tr, 5, sym, 0, b);
    const auto& tmap = enc.token_map(sym);
    const auto& amap = enc.action_map(sym);
    for (int i = 0; i < T; ++i) {
      for (int f = 0; f < F; ++f)
        ASSERT_EQ(b.observations[tmap[i] * F + f], tr->observation(5)[i * F + f]);
      ASSERT_EQ(b.tokens[tmap[i]], tr->token_classes(5)[i]);
    }
    EXPECT_EQ(b.actions[0], amap[tr->actions[5]]);
    EXPECT_EQ(b.policies[amap[tr->actions[5]]], 1.F);
    // ownership is relative to the side to move at t
    int owner = tr->final_owner[40];
    int expected = tr->to_play[5] == 0 ? owner : Enc::flip_token_class(owner);
    EXPECT_EQ(b.ownership[tmap[40]], expected);
  }
}

// ---------------------------------------------------------------- learner

TEST(Learner, OverfitsAFixedBatch) {
  t::manual_seed(0);
  Enc enc;
  std::mt19937_64 rng(3);
  Z::ReplayBuffer replay(10000);
  for (int i = 0; i < 4; ++i) replay.add(random_game(rng, enc, 20));
  Z::BatchBuilder<Enc> builder(&enc, 8, 2, false);
  Z::TrainingBatch batch;
  builder.build(replay, rng, batch);

  Z::LearnerConfig lc;
  lc.learning_rate = 3e-3;
  lc.warmup_steps = 5;
  lc.total_steps = 200;
  Z::Learner learner(Z::TransformerMuZero(tiny_config()), lc, test_device());
  Z::LearnerStats first = learner.train_step(batch);
  Z::LearnerStats st;
  for (int i = 0; i < 100; ++i) st = learner.train_step(batch);
  EXPECT_LT(st.loss, 0.6 * first.loss) << first.loss << " -> " << st.loss;
  EXPECT_LT(st.policy, 0.6 * first.policy) << first.policy << " -> " << st.policy;
  EXPECT_LT(st.value, first.value);
  EXPECT_EQ(learner.step(), 101);
  EXPECT_TRUE(std::isfinite(st.grad_norm));
}

TEST(Learner, LearningRateWarmsUpThenDecays) {
  Z::LearnerConfig lc;
  lc.learning_rate = 1e-3; lc.warmup_steps = 10; lc.total_steps = 110; lc.final_lr_fraction = 0.1;
  Z::Learner learner(Z::TransformerMuZero(tiny_config()), lc, t::kCPU);
  EXPECT_NEAR(learner.learning_rate(0), 1e-4, 1e-9);
  EXPECT_NEAR(learner.learning_rate(9), 1e-3, 1e-9);
  EXPECT_NEAR(learner.learning_rate(60), 0.55e-3, 1e-6);
  EXPECT_NEAR(learner.learning_rate(500), 1e-4, 1e-9);
}

TEST(Learner, CheckpointRoundTrip) {
  t::manual_seed(4);
  Enc enc;
  std::mt19937_64 rng(4);
  Z::ReplayBuffer replay(10000);
  for (int i = 0; i < 3; ++i) replay.add(random_game(rng, enc, 20));
  Z::BatchBuilder<Enc> builder(&enc, 8, 2, false);
  Z::TrainingBatch batch;
  builder.build(replay, rng, batch);
  fs::path dir = fresh_dir("muzero_ckpt_test");

  Z::Learner a(Z::TransformerMuZero(tiny_config()), Z::LearnerConfig(), t::kCPU);
  for (int i = 0; i < 3; ++i) (void)a.train_step(batch);
  a.save_checkpoint(dir.string());
  Z::Learner b(Z::TransformerMuZero(tiny_config()), Z::LearnerConfig(), t::kCPU);
  ASSERT_TRUE(b.load_checkpoint(dir.string()));
  EXPECT_EQ(b.step(), 3);
  // identical weights and optimizer state give the identical next step
  EXPECT_NEAR(a.train_step(batch).loss, b.train_step(batch).loss, 1e-5);
  EXPECT_FALSE(b.load_checkpoint((dir / "missing").string()));
  fs::remove_all(dir);
}

// ------------------------------------------------------------- evaluation

TEST(EloTable, OrdersPlayersAndAnchors) {
  Z::EloTable elo;
  elo.add("strong", "random", Z::MatchResult{30, 0, 2});
  elo.add("medium", "random", Z::MatchResult{20, 0, 12});
  elo.add("strong", "medium", Z::MatchResult{24, 0, 8});
  auto r = elo.ratings("random");
  EXPECT_DOUBLE_EQ(r["random"], 0.);
  EXPECT_GT(r["medium"], 0.);
  EXPECT_GT(r["strong"], r["medium"]);
}

TEST(Arena, RolloutBaselineBeatsRandom) {
  Enc enc;
  std::mt19937_64 rng(5);
  Z::RolloutPlayer<Game> rollout(100);
  Z::RandomPlayer<Game> random;
  Z::MatchResult r = Z::play_match<Game, Enc>(rollout, random, &enc, 8, 2, 7.5F, 162, rng);
  EXPECT_EQ(r.games(), 8);
  EXPECT_GE(r.wins, 7);
}

TEST(Arena, NetworkPlayersAreBatchedAndColorsAlternate) {
  t::manual_seed(6);
  Enc enc;
  std::mt19937_64 rng(6);
  Z::TransformerMuZero m(tiny_config());
  Z::ModelInference inf_a(m->config, t::kCPU, t::kFloat, 6 * 10);
  Z::ModelInference inf_b(m->config, t::kCPU, t::kFloat, 6 * 10);
  inf_a.set_weights(m, 0);
  inf_b.set_weights(m, 0);
  Z::NetworkPlayer<Game> a("a", &inf_a, 8), b("b", &inf_b, 8);
  Z::MatchResult r = Z::play_match<Game, Enc>(a, b, &enc, 6, 2, 7.5F, 40, rng);
  EXPECT_EQ(r.games(), 6);
  // batched: far fewer calls than positions searched
  EXPECT_LT(inf_a.num_calls() * 2, inf_a.num_positions());
}

// ---------------------------------------------------------- training loop

TEST(TrainingLoop, RunsWritesDataAndResumes) {
  fs::path dir = fresh_dir("muzero_loop_test");
  Enc enc;
  Z::TrainingConfig c;
  c.data_dir = dir.string();
  c.model = tiny_config();
  c.batch_size = 16;
  c.unroll_steps = 2;
  c.min_replay = 300;
  c.sample_reuse = 1.;
  c.selfplay_lead = 400;
  c.max_steps = 20;
  c.max_minutes = 5.;
  c.snapshot_interval = 10;
  c.checkpoint_interval = 10;
  c.publish_interval = 5;
  c.num_actors = 2;
  c.selfplay.games_per_actor = 8;
  c.selfplay.num_simulations = 8;
  c.selfplay.fast_simulations = 4;
  c.selfplay.max_moves = 40;
  c.evaluator.games = 4;
  c.evaluator.baseline_games = 4;
  c.evaluator.simulations = 4;
  c.evaluator.rollout_simulations = 10;
  c.evaluator.max_moves = 40;
  c.log_interval_s = 1;
  c.seed = 7;
  std::atomic<bool> interrupt{false};
  {
    Z::TrainingLoop<Game, Enc> loop(c, &enc);
    loop.run(interrupt);
  }
  EXPECT_TRUE(fs::exists(dir / "final_model.pt"));
  EXPECT_TRUE(fs::exists(dir / "checkpoint" / "model.pt"));
  EXPECT_TRUE(fs::exists(dir / "snapshots" / "step_00000000.pt"));
  EXPECT_TRUE(fs::exists(dir / "snapshots" / "step_00000020.pt"));
  EXPECT_TRUE(fs::exists(dir / "eval" / "ratings.csv"));
  EXPECT_TRUE(fs::exists(dir / "train_log.csv"));
  auto games = Z::ExperienceStore((dir / "experience").string()).load_recent(1u << 30);
  EXPECT_GT(games.size(), 0U);
  int step = 0;
  std::ifstream(dir / "checkpoint" / "step.txt") >> step;
  EXPECT_EQ(step, 20);

  // resume: continues from step 20 with the stored experience (the learner
  // may finish its steps before a new game ends; unfinished games are not
  // stored)
  c.max_steps = 30;
  {
    Z::TrainingLoop<Game, Enc> loop(c, &enc);
    loop.run(interrupt);
  }
  std::ifstream(dir / "checkpoint" / "step.txt") >> step;
  EXPECT_EQ(step, 30);
  EXPECT_GE(Z::ExperienceStore((dir / "experience").string()).load_recent(1u << 30).size(),
            games.size());
  std::ifstream log(dir / "train_log.csv");
  std::string line;
  int header_lines = 0;
  while (std::getline(log, line)) header_lines += line.starts_with("seconds,");
  EXPECT_EQ(header_lines, 1);  // appended, not overwritten
  fs::remove_all(dir);
}
