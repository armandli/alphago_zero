#ifndef RLGAMES_MUZERO_EXPERIENCE
#define RLGAMES_MUZERO_EXPERIENCE

#include <algorithm>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace rlgames::muzero {

namespace s  = std;
namespace sf = std::filesystem;

// One finished self-play game. States are indexed 0..length, actions and
// search statistics 0..length-1; state `length` is terminal.
struct Trajectory {
  int                length           = 0;
  int                observation_size = 0;
  int                num_tokens       = 0;
  int                num_actions      = 0;

  s::vector<uint8_t> features;      // (length + 1) * observation_size
  s::vector<uint8_t> tokens;        // (length + 1) * num_tokens, mover's view
  s::vector<uint8_t> to_play;       // length + 1
  s::vector<int16_t> actions;       // length
  s::vector<int8_t>  rewards;       // length, for the player who moved
  s::vector<float>   root_values;   // length, search value for the mover
  s::vector<float>   policies;      // length * num_actions
  s::vector<uint8_t> policy_valid;  // length; 0 for fast (playout capped) moves
  s::vector<uint8_t> final_owner;   // num_tokens: 0 none, 1 black, 2 white
  int8_t             outcome        = 0;  // final result for player 0
  float              score_margin   = 0.F;  // black's area margin, komi included
  uint32_t           model_version  = 0;

  // value target of state t: the final result for the side to move
  int value_target(int t) const {
    return to_play[t] == 0 ? outcome : -outcome;
  }
  const uint8_t* observation(int t) const {
    return features.data() + static_cast<size_t>(t) * observation_size;
  }
  const uint8_t* token_classes(int t) const {
    return tokens.data() + static_cast<size_t>(t) * num_tokens;
  }
};

struct PositionRef {
  s::shared_ptr<const Trajectory> trajectory;
  int                             t;
};

// Sliding window over the most recent `capacity` positions with uniform
// sampling over positions, as MuZero does for board games. Each trajectory
// records the global index of its first position, so sampling a position is
// a binary search over the window. Thread safe.
struct ReplayBuffer {
  explicit ReplayBuffer(size_t capacity): mCapacity(capacity) {}

  void add(s::shared_ptr<const Trajectory> trajectory) {
    if (trajectory->length == 0) return;
    s::unique_lock lock(mMutex);
    mEntries.push_back(Entry{mTotalPositions, s::move(trajectory)});
    size_t len = static_cast<size_t>(mEntries.back().trajectory->length);
    mTotalPositions += len;
    mPositions += len;
    ++mTotalGames;
    while (mEntries.size() > 1
           and mPositions - mEntries.front().trajectory->length >= mCapacity) {
      mPositions -= mEntries.front().trajectory->length;
      mEntries.pop_front();
    }
  }

  void sample(int n, s::mt19937_64& rng, s::vector<PositionRef>& out) const {
    s::shared_lock lock(mMutex);
    out.clear();
    if (mEntries.empty()) return;
    uint64_t lo = mEntries.front().start;
    uint64_t hi = mEntries.back().start + mEntries.back().trajectory->length;
    s::uniform_int_distribution<uint64_t> dist(lo, hi - 1);
    for (int i = 0; i < n; ++i) {
      uint64_t g = dist(rng);
      auto it = s::upper_bound(mEntries.begin(), mEntries.end(), g,
        [](uint64_t v, const Entry& e){ return v < e.start; });
      --it;
      out.push_back(PositionRef{it->trajectory, static_cast<int>(g - it->start)});
    }
  }

  size_t num_positions() const { s::shared_lock l(mMutex); return mPositions; }
  size_t num_games() const { s::shared_lock l(mMutex); return mEntries.size(); }
  uint64_t total_positions() const { s::shared_lock l(mMutex); return mTotalPositions; }
  uint64_t total_games() const { s::shared_lock l(mMutex); return mTotalGames; }

private:
  struct Entry {
    uint64_t                        start;
    s::shared_ptr<const Trajectory> trajectory;
  };

  size_t                  mCapacity;
  mutable s::shared_mutex mMutex;
  s::deque<Entry>         mEntries;
  size_t                  mPositions      = 0;
  uint64_t                mTotalPositions = 0;
  uint64_t                mTotalGames     = 0;
};

// Appends trajectories to rotating files dir/games_NNNNNN.bin, each a
// sequence of records:
//   u32 magic "MZTJ", u32 version, i32 length, i32 observation_size,
//   i32 num_tokens, i32 num_actions, i8 outcome, f32 score_margin,
//   u32 model_version, then features, tokens, to_play, actions, rewards,
//   root_values, policies, policy_valid, final_owner as raw arrays.
// A truncated last record (interrupted write) is ignored when loading.
struct ExperienceStore {
  static constexpr uint32_t MAGIC   = 0x4a545a4d;  // "MZTJ"
  static constexpr uint32_t VERSION = 1;

  explicit ExperienceStore(const s::string& dir, int games_per_file = 2000):
    mDir(dir), mGamesPerFile(games_per_file)
  {
    sf::create_directories(mDir);
    for (const sf::path& p : files())
      mNextFile = s::max(mNextFile, file_index(p) + 1);
  }
  ~ExperienceStore() { flush(); }

  void write(const Trajectory& t) {
    s::lock_guard<s::mutex> lock(mMutex);
    if (not mOut.is_open() or mGamesInFile >= mGamesPerFile) {
      if (mOut.is_open()) mOut.close();
      mOut.open(mDir / s::format("games_{:06d}.bin", mNextFile++),
                s::ios::binary | s::ios::app);
      if (not mOut) throw s::runtime_error("unable to open experience file in " + mDir.string());
      mGamesInFile = 0;
    }
    put<uint32_t>(MAGIC);
    put<uint32_t>(VERSION);
    put<int32_t>(t.length);
    put<int32_t>(t.observation_size);
    put<int32_t>(t.num_tokens);
    put<int32_t>(t.num_actions);
    put<int8_t>(t.outcome);
    put<float>(t.score_margin);
    put<uint32_t>(t.model_version);
    put_vec(t.features);
    put_vec(t.tokens);
    put_vec(t.to_play);
    put_vec(t.actions);
    put_vec(t.rewards);
    put_vec(t.root_values);
    put_vec(t.policies);
    put_vec(t.policy_valid);
    put_vec(t.final_owner);
    ++mGamesInFile;
    ++mGamesWritten;
  }

  void flush() {
    s::lock_guard<s::mutex> lock(mMutex);
    if (mOut.is_open()) mOut.flush();
  }

  uint64_t games_written() const { return mGamesWritten; }

  // the most recent trajectories holding at most max_positions positions
  s::vector<s::shared_ptr<Trajectory>> load_recent(size_t max_positions) const {
    s::vector<s::shared_ptr<Trajectory>> out;
    size_t positions = 0;
    s::vector<sf::path> fs = files();
    for (auto it = fs.rbegin(); it != fs.rend() and positions < max_positions; ++it) {
      s::vector<s::shared_ptr<Trajectory>> in_file = load_file(*it);
      for (auto jt = in_file.rbegin(); jt != in_file.rend() and positions < max_positions; ++jt) {
        positions += (*jt)->length;
        out.push_back(*jt);
      }
    }
    s::reverse(out.begin(), out.end());
    return out;
  }

  static s::vector<s::shared_ptr<Trajectory>> load_file(const sf::path& path) {
    s::vector<s::shared_ptr<Trajectory>> out;
    s::ifstream in(path, s::ios::binary);
    while (in) {
      auto t = s::make_shared<Trajectory>();
      uint32_t magic = 0, version = 0;
      if (not get(in, magic) or magic != MAGIC) break;
      if (not get(in, version) or version != VERSION) break;
      bool ok = get(in, t->length) and get(in, t->observation_size)
            and get(in, t->num_tokens) and get(in, t->num_actions)
            and get(in, t->outcome) and get(in, t->score_margin)
            and get(in, t->model_version);
      if (not ok or t->length < 0) break;
      size_t n = static_cast<size_t>(t->length);
      ok = get_vec(in, t->features, (n + 1) * t->observation_size)
       and get_vec(in, t->tokens, (n + 1) * t->num_tokens)
       and get_vec(in, t->to_play, n + 1)
       and get_vec(in, t->actions, n)
       and get_vec(in, t->rewards, n)
       and get_vec(in, t->root_values, n)
       and get_vec(in, t->policies, n * t->num_actions)
       and get_vec(in, t->policy_valid, n)
       and get_vec(in, t->final_owner, t->num_tokens);
      if (not ok) break;  // truncated record
      out.push_back(s::move(t));
    }
    return out;
  }

private:
  s::vector<sf::path> files() const {
    s::vector<sf::path> out;
    if (not sf::exists(mDir)) return out;
    for (const auto& e : sf::directory_iterator(mDir)) {
      s::string name = e.path().filename().string();
      if (name.starts_with("games_") and name.ends_with(".bin")) out.push_back(e.path());
    }
    s::sort(out.begin(), out.end());
    return out;
  }
  static int file_index(const sf::path& p) {
    s::string stem = p.stem().string();
    return s::stoi(stem.substr(6));
  }

  template <typename T> void put(T v) {
    mOut.write(reinterpret_cast<const char*>(&v), sizeof(T));
  }
  template <typename T> void put_vec(const s::vector<T>& v) {
    mOut.write(reinterpret_cast<const char*>(v.data()), sizeof(T) * v.size());
  }
  template <typename T> static bool get(s::ifstream& in, T& v) {
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&v), sizeof(T)));
  }
  template <typename T> static bool get_vec(s::ifstream& in, s::vector<T>& v, size_t n) {
    v.resize(n);
    if (n == 0) return true;
    return static_cast<bool>(in.read(reinterpret_cast<char*>(v.data()), sizeof(T) * n));
  }

  sf::path      mDir;
  int           mGamesPerFile;
  int           mNextFile     = 0;
  int           mGamesInFile  = 0;
  uint64_t      mGamesWritten = 0;
  s::mutex      mMutex;
  s::ofstream   mOut;
};

}  // namespace rlgames::muzero

#endif  // RLGAMES_MUZERO_EXPERIENCE
