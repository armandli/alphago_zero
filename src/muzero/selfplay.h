#ifndef RLGAMES_MUZERO_SELFPLAY
#define RLGAMES_MUZERO_SELFPLAY

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include <experience.h>
#include <inference.h>
#include <mcgs.h>

namespace rlgames::muzero {

namespace s = std;

// builds a Trajectory move by move from a game and its search results
template <typename Game, typename Encoder>
struct TrajectoryRecorder {
  explicit TrajectoryRecorder(const Encoder* encoder): mEncoder(encoder) {}

  s::shared_ptr<Trajectory> begin(uint32_t model_version) const {
    auto t = s::make_shared<Trajectory>();
    t->observation_size = mEncoder->observation_size();
    t->num_tokens = mEncoder->num_tokens();
    t->num_actions = mEncoder->num_actions();
    t->model_version = model_version;
    return t;
  }

  // records the state before the move, then applies the move to game
  void move(Trajectory& t, Game& game, const SearchResult& result, bool policy_valid) const {
    add_state(t, game);
    t.actions.push_back(static_cast<int16_t>(result.action));
    t.policies.insert(t.policies.end(), result.policy.begin(), result.policy.end());
    t.policy_valid.push_back(policy_valid ? 1 : 0);
    t.root_values.push_back(result.root_value);
    int mover = game.to_play();
    game.apply(result.action);
    t.rewards.push_back(static_cast<int8_t>(game.is_terminal() ? game.outcome(mover) : 0));
    ++t.length;
  }

  // records the terminal state and the final result
  void finish(Trajectory& t, const Game& game) const {
    add_state(t, game);
    auto owner = game.ownership();
    t.final_owner.assign(owner.begin(), owner.end());
    t.outcome = static_cast<int8_t>(game.outcome(0));
    t.score_margin = game.score_margin();
  }

private:
  void add_state(Trajectory& t, const Game& game) const {
    size_t f = t.features.size();
    t.features.resize(f + t.observation_size);
    mEncoder->encode(game, t.features.data() + f);
    size_t k = t.tokens.size();
    t.tokens.resize(k + t.num_tokens);
    mEncoder->encode_tokens(game, t.tokens.data() + k);
    t.to_play.push_back(static_cast<uint8_t>(game.to_play()));
  }

  const Encoder* mEncoder;
};

struct SelfPlayStats {
  s::atomic<uint64_t> games{0};
  s::atomic<uint64_t> positions{0};      // of finished games
  s::atomic<uint64_t> moves{0};          // played, including unfinished games
  s::atomic<uint64_t> full_searches{0};
  s::atomic<uint64_t> black_wins{0};
  s::atomic<uint64_t> white_wins{0};
  s::atomic<uint64_t> length_total{0};   // sum of finished game lengths
};

struct SelfPlayConfig {
  int   games_per_actor = 512;
  // playout cap randomization (KataGo): a move gets a full search with
  // probability full_search_prob and becomes a policy training target;
  // other moves get a fast search that only advances the game. Values are
  // trained on every move.
  int   num_simulations  = 32;
  int   fast_simulations = 8;
  float full_search_prob = 0.25F;
  // visit count sampling, used by the PUCT root only
  float temperature       = 1.F;
  int   temperature_moves = 16;
  float komi              = 7.5F;
  int   max_moves         = 162;  // 2 * 81
};

// latent ids one search needs: root plus one per simulation, with slack
inline int latent_slots_per_search(const SelfPlayConfig& c) {
  return s::max(c.num_simulations, c.fast_simulations) + 2;
}

// A worker thread playing games_per_actor games at once against itself.
// Every step it advances each game's search to its next network request,
// sends all of them in one batched call, then feeds the answers back.
// Finished games go to the replay buffer and the experience store.
template <typename Game, typename Encoder>
struct SelfPlayActor {
  using Search = MCGS<Game, Encoder>;

  SelfPlayActor(
      const SelfPlayConfig& config,
      const MCGSConfig& search_config,
      const Encoder* encoder,
      Inference* inference,
      int64_t latent_base,
      ReplayBuffer* replay,
      ExperienceStore* store,
      SelfPlayStats* stats,
      s::function<uint32_t()> model_version,
      uint64_t seed):
    mConfig(config), mInference(inference), mReplay(replay), mStore(store),
    mStats(stats), mModelVersion(s::move(model_version)), mRecorder(encoder), mRng(seed)
  {
    int slots = latent_slots_per_search(config);
    for (int i = 0; i < config.games_per_actor; ++i)
      mSlots.push_back(Slot{
        Game(config.komi, config.max_moves),
        s::make_unique<Search>(encoder, search_config,
                               latent_base + static_cast<int64_t>(i) * slots, slots, mRng()),
        nullptr, false, 0});
  }

  // polled every step; self-play waits while it returns true, e.g. while
  // the learner lags behind the data
  void set_pause(s::function<bool()> pause) { mPause = s::move(pause); }

  void run(const s::atomic<bool>& stop) {
    InferenceRequests requests;
    requests.observation_size = mInference->observation_size();
    InferenceResults results;
    s::vector<int> waiting;
    for (Slot& slot : mSlots) new_game(slot);
    while (not stop.load(s::memory_order_relaxed)) {
      if (mPause and mPause()) {
        s::this_thread::sleep_for(s::chrono::milliseconds(5));
        continue;
      }
      requests.clear();
      waiting.clear();
      for (int i = 0; i < static_cast<int>(mSlots.size()); ++i) {
        Slot& slot = mSlots[i];
        while (not slot.search->step(requests)) play_move(slot);
        waiting.push_back(i);
      }
      mInference->infer(requests, results);
      for (int i : waiting) mSlots[i].search->receive(results);
    }
  }

private:
  struct Slot {
    Game                      game;
    s::unique_ptr<Search>     search;
    s::shared_ptr<Trajectory> trajectory;
    bool                      full;
    int                       move;
  };

  void new_game(Slot& slot) {
    slot.game = Game(mConfig.komi, mConfig.max_moves);
    slot.trajectory = mRecorder.begin(mModelVersion());
    slot.move = 0;
    start_search(slot);
  }

  void start_search(Slot& slot) {
    s::uniform_real_distribution<float> u(0.F, 1.F);
    slot.full = u(mRng) < mConfig.full_search_prob;
    slot.search->start(slot.game,
      slot.full ? mConfig.num_simulations : mConfig.fast_simulations, true);
  }

  void play_move(Slot& slot) {
    float temperature = slot.move < mConfig.temperature_moves ? mConfig.temperature : 0.F;
    SearchResult result = slot.search->result(temperature);
    // a forced move (no search) is still a valid one-hot policy target
    mRecorder.move(*slot.trajectory, slot.game, result, slot.full or result.simulations == 0);
    ++slot.move;
    mStats->moves += 1;
    if (slot.full) mStats->full_searches += 1;
    if (slot.game.is_terminal()) {
      mRecorder.finish(*slot.trajectory, slot.game);
      finish_game(slot.trajectory);
      new_game(slot);
    } else {
      start_search(slot);
    }
  }

  void finish_game(s::shared_ptr<Trajectory> t) {
    if (mStore) mStore->write(*t);
    mStats->positions += t->length;
    mStats->length_total += t->length;
    mStats->games += 1;
    if (t->outcome > 0) mStats->black_wins += 1;
    else                mStats->white_wins += 1;
    mReplay->add(s::move(t));
  }

  SelfPlayConfig                   mConfig;
  Inference*                       mInference;
  ReplayBuffer*                    mReplay;
  ExperienceStore*                 mStore;
  SelfPlayStats*                   mStats;
  s::function<uint32_t()>          mModelVersion;
  TrajectoryRecorder<Game, Encoder> mRecorder;
  s::mt19937_64                    mRng;
  s::vector<Slot>                  mSlots;
  s::function<bool()>              mPause;
};

// Keeps training samples per self-play position at `reuse`: the learner
// waits when it gets ahead of the data, self-play waits when it gets more
// than `lead` positions ahead of the learner, so the ratio holds whatever
// the balance of CPU and GPU.
struct SampleRatio {
  SampleRatio(int batch_size, double reuse, size_t min_positions, size_t lead):
    mBatch(batch_size), mReuse(reuse), mMin(min_positions), mLead(lead)
  {}

  bool learner_may_step(int64_t step, double positions_played, size_t replay_positions) const {
    if (replay_positions < mMin) return false;
    double sampled = static_cast<double>(step + 1) * mBatch;
    return sampled <= mReuse * (positions_played - static_cast<double>(mMin)) + mBatch;
  }
  bool selfplay_should_pause(int64_t step, double positions_played, size_t replay_positions) const {
    if (replay_positions < mMin) return false;
    double allowed = static_cast<double>(step) * mBatch / mReuse + static_cast<double>(mMin + mLead);
    return positions_played > allowed;
  }

private:
  int    mBatch;
  double mReuse;
  size_t mMin;
  size_t mLead;
};

}  // namespace rlgames::muzero

#endif  // RLGAMES_MUZERO_SELFPLAY
