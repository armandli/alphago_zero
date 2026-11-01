#ifndef RLGAMES_MUZERO_TRAINING_LOOP
#define RLGAMES_MUZERO_TRAINING_LOOP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <torch/torch.h>

#include <evaluation.h>
#include <experience.h>
#include <inference.h>
#include <learner.h>
#include <mcgs.h>
#include <selfplay.h>
#include <transformer_muzero.h>

namespace rlgames::muzero {

namespace s  = std;
namespace sf = std::filesystem;
namespace t  = torch;

struct TrainingConfig {
  s::string data_dir    = "data/muzero_9x9";
  bool      resume      = true;
  double    max_minutes = 60.;          // wall clock budget of this run
  int64_t   max_steps   = 1000000;      // learner steps, across runs
  uint64_t  seed        = 0;            // 0 picks a random seed

  // learner
  int    batch_size      = 256;
  int    unroll_steps    = 5;
  bool   augment         = true;        // random board symmetry per sample
  size_t replay_capacity = 400000;      // positions
  size_t min_replay      = 20000;       // positions before training starts
  // training samples per self-play position: the learner waits when it
  // gets ahead, self-play waits when it is more than selfplay_lead
  // positions ahead
  double sample_reuse  = 2.;
  size_t selfplay_lead = 40000;
  int    publish_interval    = 50;      // steps between weight pushes to self-play
  int    snapshot_interval   = 1000;    // steps between evaluated snapshots
  int    checkpoint_interval = 500;

  // self-play
  int  num_actors = 4;
  bool bf16_inference = true;           // else float16

  bool evaluate       = true;
  int  log_interval_s = 30;

  TransformerMuZeroConfig model;
  SelfPlayConfig          selfplay;
  MCGSConfig              search;
  LearnerConfig           learner;
  EvaluatorConfig         evaluator;
};

inline s::string snapshot_name(int64_t step) { return s::format("step_{:08d}", step); }

// MuZero training with every component on its own threads:
//   actors (CPU): self-play, games_per_actor games each, searches batched
//   learner (GPU): samples the replay buffer and trains, pushing weights to
//     self-play and saving checkpoints and snapshots
//   evaluator (GPU + CPU): rates each snapshot against older snapshots, a
//     random player and a UCT rollout baseline
// Everything persists under data_dir:
//   experience/games_*.bin  every self-play game (reloaded on resume)
//   checkpoint/             model, optimizer, step, generated positions
//   snapshots/step_*.pt     model snapshots, one per snapshot_interval
//   eval/                   match results, Elo ratings, summary log
//   train_log.csv           losses and throughput over time
//   config.txt              the options of the last run
//   final_model.pt          the model at the end of the run
template <typename Game, typename Encoder>
struct TrainingLoop {
  TrainingLoop(const TrainingConfig& config, const Encoder* encoder):
    mConfig(config), mEncoder(encoder)
  {}

  void run(const s::atomic<bool>& interrupt) {
    const TrainingConfig& c = mConfig;
    sf::path data(c.data_dir);
    sf::path ckpt = data / "checkpoint";
    sf::path snapshots = data / "snapshots";
    sf::create_directories(ckpt);
    sf::create_directories(snapshots);
    uint64_t seed = c.seed ? c.seed : s::random_device()();
    t::manual_seed(seed);
    s::mt19937_64 rng(seed);
    t::Device device = t::cuda::is_available() ? t::Device(t::kCUDA) : t::Device(t::kCPU);
    t::ScalarType dtype = device.is_cuda() ? (c.bf16_inference ? t::kBFloat16 : t::kHalf) : t::kFloat;

    // learner; a checkpoint's model configuration wins over the options
    bool resumed = c.resume and sf::exists(ckpt / "model.pt");
    TransformerMuZero model = resumed ? load_model((ckpt / "model.pt").string(), t::kCPU)
                                      : TransformerMuZero(c.model);
    TransformerMuZeroConfig model_config = model->config;
    Learner learner(model, c.learner, device);
    uint64_t previous_positions = 0;
    if (resumed and learner.load_checkpoint(ckpt.string())) {
      s::ifstream(ckpt / "positions.txt") >> previous_positions;
      s::cout << "[train] resumed from step " << learner.step() << s::endl;
    }
    s::cout << s::format("[train] TransformerMuZero d{} h{} layers {}/{}/{}: {} parameters, device {}",
      model_config.dims, model_config.num_heads, model_config.repr_layers, model_config.dyn_layers,
      model_config.pred_layers, learner.model()->num_parameters(), device.str()) << s::endl;
    write_config(data / "config.txt", model_config);

    // experience
    ReplayBuffer replay(c.replay_capacity);
    ExperienceStore store((data / "experience").string());
    if (c.resume) {
      auto games = store.load_recent(c.replay_capacity);
      for (auto& g : games) replay.add(g);
      if (not games.empty())
        s::cout << "[train] loaded " << replay.num_games() << " games, "
                << replay.num_positions() << " positions of experience" << s::endl;
    }
    uint64_t loaded_positions = replay.total_positions();
    auto generated = [&]{ return previous_positions + replay.total_positions() - loaded_positions; };

    // self-play inference with a latent pool for every search
    int slots = latent_slots_per_search(c.selfplay);
    int64_t per_actor = static_cast<int64_t>(c.selfplay.games_per_actor) * slots;
    ModelInference inference(model_config, device, dtype, per_actor * c.num_actors);
    inference.set_weights(learner.model(), static_cast<uint32_t>(learner.step()));

    s::atomic<bool> stop{false};
    SelfPlayStats stats;
    // finished positions only: moves of games still in progress cannot be
    // trained on yet, and with thousands of concurrent games they would
    // make self-play look far ahead of the learner
    auto played = [&]{ return static_cast<double>(previous_positions + stats.positions.load()); };
    SampleRatio ratio(c.batch_size, c.sample_reuse, c.min_replay, c.selfplay_lead);
    s::atomic<int64_t> step_now{learner.step()};

    s::vector<s::thread> actors;
    for (int i = 0; i < c.num_actors; ++i) {
      uint64_t aseed = rng();
      actors.emplace_back([&, i, aseed]{
        SelfPlayActor<Game, Encoder> actor(c.selfplay, c.search, mEncoder, &inference,
          per_actor * i, &replay, &store, &stats, [&]{ return inference.version(); }, aseed);
        actor.set_pause([&]{
          return ratio.selfplay_should_pause(step_now.load(), played(), replay.num_positions());
        });
        actor.run(stop);
      });
    }
    s::cout << s::format("[train] {} self-play threads x {} games, {}/{} simulations, data in {}",
      c.num_actors, c.selfplay.games_per_actor, c.selfplay.num_simulations,
      c.selfplay.fast_simulations, data.string()) << s::endl;

    // evaluator
    s::unique_ptr<SnapshotEvaluator<Game, Encoder>> evaluator;
    s::thread eval_thread;
    auto save_snapshot = [&](int64_t step) {
      sf::path p = snapshots / (snapshot_name(step) + ".pt");
      save_model(learner.model(), p.string());
      return p;
    };
    if (c.evaluate) {
      evaluator = s::make_unique<SnapshotEvaluator<Game, Encoder>>(
        c.evaluator, (data / "eval").string(), model_config, mEncoder, device, dtype, rng());
      s::vector<sf::path> existing;
      for (const auto& e : sf::directory_iterator(snapshots))
        if (e.path().extension() == ".pt") existing.push_back(e.path());
      s::sort(existing.begin(), existing.end());
      for (const sf::path& p : existing) evaluator->add_existing(p.stem().string(), p.string());
      if (existing.empty()) {
        sf::path p = save_snapshot(learner.step());
        evaluator->submit(p.stem().string(), p.string());
      }
      eval_thread = s::thread([&]{ evaluator->run(stop); });
    }

    // learner
    s::mutex stats_mutex;
    LearnerStats stats_sum;
    int stats_count = 0;
    s::atomic<int64_t> waiting_ms{0};
    auto checkpoint = [&]{
      learner.save_checkpoint(ckpt.string());
      s::ofstream(ckpt / "positions.txt") << generated() << "\n";
      store.flush();
    };
    s::thread learner_thread([&]{
      s::mt19937_64 lrng(rng());
      BatchBuilder<Encoder> builder(mEncoder, c.batch_size, c.unroll_steps, c.augment);
      TrainingBatch batch;
      while (not stop.load() and learner.step() < c.max_steps) {
        if (not ratio.learner_may_step(learner.step(), played(), replay.num_positions())) {
          s::this_thread::sleep_for(s::chrono::milliseconds(10));
          waiting_ms += 10;
          continue;
        }
        builder.build(replay, lrng, batch);
        LearnerStats st = learner.train_step(batch);
        {
          s::lock_guard<s::mutex> lock(stats_mutex);
          stats_sum.add(st);
          ++stats_count;
        }
        int64_t step = learner.step();
        step_now = step;
        if (step % c.publish_interval == 0)
          inference.set_weights(learner.model(), static_cast<uint32_t>(step));
        if (step % c.checkpoint_interval == 0) checkpoint();
        if (step % c.snapshot_interval == 0) {
          sf::path p = save_snapshot(step);
          if (evaluator) evaluator->submit(p.stem().string(), p.string());
        }
      }
      stop = true;
    });

    // progress log
    sf::path log_path = data / "train_log.csv";
    bool header = not sf::exists(log_path);
    s::ofstream log(log_path, s::ios::app);
    if (header)
      log << "seconds,step,games,positions,replay_positions,black_win_rate,mean_game_length,"
             "loss,policy,value,reward,consistency,reconstruction,ownership,grad_norm,lr,"
             "nn_positions_per_s,learner_wait_fraction,last_elo\n";
    auto t0 = s::chrono::steady_clock::now();
    auto last = t0;
    uint64_t last_games = 0, last_positions = 0, last_nn = 0;
    uint64_t last_black = 0, last_length = 0;
    while (not stop.load()) {
      for (int i = 0; i < 10 * c.log_interval_s and not stop.load() and not interrupt.load(); ++i)
        s::this_thread::sleep_for(s::chrono::milliseconds(100));
      auto now = s::chrono::steady_clock::now();
      double elapsed = s::chrono::duration<double>(now - t0).count();
      if (interrupt.load() or elapsed >= c.max_minutes * 60.) break;
      double dt = s::chrono::duration<double>(now - last).count();
      last = now;
      LearnerStats avg;
      {
        s::lock_guard<s::mutex> lock(stats_mutex);
        avg = stats_sum.mean(stats_count);
        stats_sum = LearnerStats();
        stats_count = 0;
      }
      uint64_t games = stats.games, positions = stats.positions;
      uint64_t black = stats.black_wins, length = stats.length_total;
      uint64_t nn = inference.num_positions();
      double new_games = static_cast<double>(games - last_games);
      double black_rate = new_games > 0 ? (black - last_black) / new_games : 0.;
      double mean_len = new_games > 0 ? (length - last_length) / new_games : 0.;
      double wait = s::min(1., waiting_ms.exchange(0) / (1000. * dt));
      double elo = evaluator ? evaluator->last_elo() : 0.;
      s::cout << s::format(
        "[train] {:>5.0f}s step {} | games {} ({:.1f}/s, len {:.0f}, black {:.0f}%) replay {} | "
        "loss {:.3f} (p {:.3f} v {:.3f} r {:.3f} c {:.3f} g {:.3f} o {:.3f}) lr {:.1e} | "
        "nn {:.0f} pos/s | learner waits {:.0f}% | elo {:.0f}",
        elapsed, learner.step(), games, new_games / dt, mean_len, 100. * black_rate,
        replay.num_positions(), avg.loss, avg.policy, avg.value, avg.reward, avg.consistency,
        avg.reconstruction, avg.ownership, avg.learning_rate, (nn - last_nn) / dt,
        100. * wait, elo) << s::endl;
      log << s::format("{:.0f},{},{},{},{},{:.3f},{:.1f},{:.5f},{:.5f},{:.5f},{:.5f},{:.5f},{:.5f},"
                       "{:.5f},{:.4f},{:.3e},{:.0f},{:.3f},{:.1f}\n",
        elapsed, learner.step(), games, positions, replay.num_positions(), black_rate, mean_len,
        avg.loss, avg.policy, avg.value, avg.reward, avg.consistency, avg.reconstruction,
        avg.ownership, avg.grad_norm, avg.learning_rate, (nn - last_nn) / dt, wait, elo);
      log.flush();
      last_games = games; last_positions = positions; last_nn = nn;
      last_black = black; last_length = length;
    }

    s::cout << "[train] stopping" << s::endl;
    stop = true;
    learner_thread.join();
    for (s::thread& th : actors) th.join();
    if (eval_thread.joinable()) eval_thread.join();
    checkpoint();
    save_model(learner.model(), (data / "final_model.pt").string());
    s::cout << "[train] checkpoint and final model saved at step " << learner.step() << s::endl;

    // rate the final model
    if (evaluator) {
      sf::path p = save_snapshot(learner.step());
      s::atomic<bool> no_stop{false};
      evaluator->evaluate(p.stem().string(), p.string(), no_stop);
    }
  }

private:
  void write_config(const sf::path& path, const TransformerMuZeroConfig& m) const {
    const TrainingConfig& c = mConfig;
    s::ofstream out(path);
    out << "model.dims " << m.dims << "\nmodel.heads " << m.num_heads
        << "\nmodel.repr_layers " << m.repr_layers << "\nmodel.dyn_layers " << m.dyn_layers
        << "\nmodel.pred_layers " << m.pred_layers << "\nmodel.proj_dims " << m.proj_dims
        << "\nbatch_size " << c.batch_size << "\nunroll_steps " << c.unroll_steps
        << "\nreplay_capacity " << c.replay_capacity << "\nmin_replay " << c.min_replay
        << "\nsample_reuse " << c.sample_reuse << "\nlearning_rate " << c.learner.learning_rate
        << "\ntotal_steps " << c.learner.total_steps << "\nnum_actors " << c.num_actors
        << "\ngames_per_actor " << c.selfplay.games_per_actor
        << "\nsimulations " << c.selfplay.num_simulations
        << "\nfast_simulations " << c.selfplay.fast_simulations
        << "\nfull_search_prob " << c.selfplay.full_search_prob
        << "\nkomi " << c.selfplay.komi << "\nmax_moves " << c.selfplay.max_moves
        << "\nroot " << (c.search.root == MCGSConfig::Root::Gumbel ? "gumbel" : "puct")
        << "\nmax_minutes " << c.max_minutes << "\n";
  }

  TrainingConfig mConfig;
  const Encoder* mEncoder;
};

}  // namespace rlgames::muzero

#endif  // RLGAMES_MUZERO_TRAINING_LOOP
