// MuZero self-play training on 9x9 Go: a transformer world model searched by
// Monte-Carlo graph search and trained on its own games, with snapshots
// evaluated as it goes. Everything is written under --data-dir.

#include <atomic>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <print>
#include <string>

#include <CLI/CLI.hpp>

#include <go_game.h>
#include <go_encoder.h>
#include <training_loop.h>

namespace s = std;
namespace Z = rlgames::muzero;

namespace {
s::atomic<bool> interrupted{false};
void on_signal(int) { interrupted = true; }
}  // namespace

int main(int argc, char* argv[]) try {
  using Game = Z::GoGame<9>;
  using Enc  = Z::GoEncoder<9>;
  Enc encoder;

  Z::TrainingConfig c;
  c.model.num_tokens        = Enc::NUM_TOKENS;
  c.model.num_features      = Enc::NUM_FEATURES;
  c.model.num_actions       = Enc::NUM_ACTIONS;
  c.model.num_token_classes = Enc::NUM_TOKEN_CLASSES;
  s::string root = "gumbel";

  CLI::App app{"MuZero self-play training for 9x9 Go"};
  app.add_option("--data-dir", c.data_dir, "Output directory for experience, snapshots, logs")->capture_default_str();
  app.add_flag("!--no-resume", c.resume, "Start fresh instead of resuming from --data-dir");
  app.add_option("--minutes", c.max_minutes, "Wall clock budget of this run")->capture_default_str();
  app.add_option("--max-steps", c.max_steps, "Stop at this learner step")->capture_default_str();
  app.add_option("--seed", c.seed, "Random seed, 0 for random")->capture_default_str();

  app.add_option("--dims", c.model.dims, "Transformer width")->capture_default_str();
  app.add_option("--heads", c.model.num_heads, "Attention heads")->capture_default_str();
  app.add_option("--repr-layers", c.model.repr_layers, "Representation blocks")->capture_default_str();
  app.add_option("--dyn-layers", c.model.dyn_layers, "Dynamics blocks")->capture_default_str();
  app.add_option("--pred-layers", c.model.pred_layers, "Prediction blocks")->capture_default_str();

  app.add_option("--batch-size", c.batch_size, "Training batch size")->capture_default_str();
  app.add_option("--unroll", c.unroll_steps, "Unroll steps K")->capture_default_str();
  app.add_option("--lr", c.learner.learning_rate, "Peak learning rate")->capture_default_str();
  app.add_option("--lr-horizon", c.learner.total_steps, "Cosine decay horizon in steps")->capture_default_str();
  app.add_option("--warmup", c.learner.warmup_steps, "Warmup steps")->capture_default_str();
  app.add_option("--replay", c.replay_capacity, "Replay buffer capacity in positions")->capture_default_str();
  app.add_option("--min-replay", c.min_replay, "Positions before training starts")->capture_default_str();
  app.add_option("--sample-reuse", c.sample_reuse, "Training samples per self-play position")->capture_default_str();
  app.add_option("--snapshot-interval", c.snapshot_interval, "Steps between evaluated snapshots")->capture_default_str();
  app.add_option("--checkpoint-interval", c.checkpoint_interval, "Steps between checkpoints")->capture_default_str();

  app.add_option("--actors", c.num_actors, "Self-play threads")->capture_default_str();
  app.add_option("--games-per-actor", c.selfplay.games_per_actor, "Concurrent games per thread")->capture_default_str();
  app.add_option("--simulations", c.selfplay.num_simulations, "Simulations of a full search")->capture_default_str();
  app.add_option("--fast-simulations", c.selfplay.fast_simulations, "Simulations of a fast search")->capture_default_str();
  app.add_option("--full-search-prob", c.selfplay.full_search_prob, "Probability of a full search")->capture_default_str();
  app.add_option("--root", root, "Root selection: gumbel or puct")
    ->check(CLI::IsMember({"gumbel", "puct"}))->capture_default_str();
  app.add_option("--komi", c.selfplay.komi, "Komi")->capture_default_str();

  app.add_flag("!--no-eval", c.evaluate, "Disable snapshot evaluation");
  app.add_option("--eval-games", c.evaluator.games, "Games per snapshot match")->capture_default_str();
  app.add_option("--eval-baseline-games", c.evaluator.baseline_games, "Games per baseline match")->capture_default_str();
  app.add_option("--eval-simulations", c.evaluator.simulations, "Search budget in evaluation")->capture_default_str();
  app.add_option("--eval-rollouts", c.evaluator.rollout_simulations, "UCT rollout baseline simulations, 0 disables")->capture_default_str();
  app.add_option("--log-interval", c.log_interval_s, "Seconds between progress lines")->capture_default_str();
  CLI11_PARSE(app, argc, argv);

  c.search.root = (root == "puct") ? Z::MCGSConfig::Root::PUCT : Z::MCGSConfig::Root::Gumbel;
  c.evaluator.komi = c.selfplay.komi;
  c.evaluator.max_moves = c.selfplay.max_moves;

  s::signal(SIGINT, on_signal);
  s::signal(SIGTERM, on_signal);
  Z::TrainingLoop<Game, Enc> loop(c, &encoder);
  loop.run(interrupted);
  return 0;
} catch (const s::exception& e) {
  s::println(stderr, "error: {}", e.what());
  return 1;
}
