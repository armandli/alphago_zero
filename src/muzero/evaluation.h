#ifndef RLGAMES_MUZERO_EVALUATION
#define RLGAMES_MUZERO_EVALUATION

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <torch/torch.h>

#include <agent_base.h>
#include <go_game.h>
#include <inference.h>
#include <mcgs.h>
#include <transformer_muzero.h>

namespace rlgames::muzero {

namespace s  = std;
namespace sf = std::filesystem;
namespace t  = torch;

struct MatchResult {
  int wins = 0, draws = 0, losses = 0;
  int games() const { return wins + draws + losses; }
  double score() const { return games() ? (wins + 0.5 * draws) / games() : 0.; }
};

// A player in the arena: either searches with a network (the arena batches
// its requests) or picks moves directly on the CPU.
template <typename Game>
struct ArenaPlayer {
  virtual ~ArenaPlayer() = default;
  virtual s::string name() const = 0;
  virtual Inference* inference() { return nullptr; }
  virtual int simulations() const { return 0; }
  virtual MCGSConfig search_config() const { return MCGSConfig(); }
  // CPU players only
  virtual int select(const Game&, s::mt19937_64&) { return Game::PASS; }
};

template <typename Game>
struct NetworkPlayer : ArenaPlayer<Game> {
  NetworkPlayer(s::string name, Inference* inference, int simulations, MCGSConfig config = MCGSConfig()):
    mName(s::move(name)), mInference(inference), mSimulations(simulations), mConfig(config)
  {}
  s::string name() const override { return mName; }
  Inference* inference() override { return mInference; }
  int simulations() const override { return mSimulations; }
  MCGSConfig search_config() const override { return mConfig; }

private:
  s::string  mName;
  Inference* mInference;
  int        mSimulations;
  MCGSConfig mConfig;
};

// uniformly random legal move, never passing while a point is playable;
// the rating anchor (Elo 0)
template <typename Game>
struct RandomPlayer : ArenaPlayer<Game> {
  s::string name() const override { return "random"; }
  int select(const Game& g, s::mt19937_64& rng) override {
    ActionList<Game::NUM_ACTIONS> legal;
    g.legal_actions(legal);
    if (legal.size == 1) return legal[0];
    return legal[s::uniform_int_distribution<int>(0, legal.size - 2)(rng)];
  }
};

// UCT with light random playouts (random legal moves that do not fill the
// mover's own eyes, pass when none is left). A fixed, non-learning baseline
// whose strength grows with its simulations.
template <typename Game>
struct RolloutPlayer : ArenaPlayer<Game> {
  explicit RolloutPlayer(int simulations, float exploration = 1.0F):
    mSimulations(simulations), mC(exploration)
  {}
  s::string name() const override { return s::format("mcts_rollout_{}", mSimulations); }

  int select(const Game& root, s::mt19937_64& rng) override {
    ActionList<Game::NUM_ACTIONS> legal;
    root.legal_actions(legal);
    if (legal.size == 1) return legal[0];
    mNodes.clear();
    mNodes.push_back(make_node(root, -1, -1));
    for (int i = 0; i < mSimulations; ++i) {
      int n = 0;
      // selection
      while (mNodes[n].untried.empty() and not mNodes[n].children.empty())
        n = best_child(n);
      // expansion
      if (not mNodes[n].untried.empty()) {
        auto& untried = mNodes[n].untried;
        size_t k = s::uniform_int_distribution<size_t>(0, untried.size() - 1)(rng);
        int action = untried[k];
        untried[k] = untried.back();
        untried.pop_back();
        Game g = mNodes[n].state;
        g.apply(action);
        mNodes.push_back(make_node(g, n, action));
        int c = static_cast<int>(mNodes.size()) - 1;
        mNodes[n].children.push_back(c);
        n = c;
      }
      // playout and backup: wins count for the player who moved into a node
      Game g = mNodes[n].state;
      playout(g, rng);
      int winner = g.outcome(0) > 0 ? 0 : 1;
      while (n != -1) {
        Node& node = mNodes[n];
        node.visits += 1;
        if (node.parent >= 0 and mNodes[node.parent].state.to_play() == winner) node.wins += 1;
        n = node.parent;
      }
    }
    int best = mNodes[0].children.front();
    for (int c : mNodes[0].children)
      if (mNodes[c].visits > mNodes[best].visits) best = c;
    return mNodes[best].action;
  }

private:
  struct Node {
    Game           state;
    int            parent;
    int            action;
    s::vector<int> children;
    s::vector<int> untried;
    int            visits = 0;
    int            wins   = 0;
  };

  Node make_node(const Game& g, int parent, int action) const {
    Node node{g, parent, action, {}, {}};
    if (not g.is_terminal()) {
      ActionList<Game::NUM_ACTIONS> legal;
      g.legal_actions(legal);
      node.untried.assign(legal.begin(), legal.end());
    }
    return node;
  }

  int best_child(int n) const {
    const Node& node = mNodes[n];
    double log_n = s::log(static_cast<double>(s::max(node.visits, 1)));
    int best = node.children.front();
    double best_score = -1.;
    for (int c : node.children) {
      const Node& ch = mNodes[c];
      double score = static_cast<double>(ch.wins) / ch.visits
                   + mC * s::sqrt(log_n / ch.visits);
      if (score > best_score) { best_score = score; best = c; }
    }
    return best;
  }

  static void playout(Game& g, s::mt19937_64& rng) {
    IsPointAnEye<GoBoard<Game::SIZE>> is_eye;
    s::vector<int> candidates;
    while (not g.is_terminal()) {
      candidates.clear();
      for (int a = 0; a < Game::NUM_POINTS; ++a)
        if (g.board().get(point<Game::SIZE>(static_cast<uint>(a))) == Player::Unknown)
          candidates.push_back(a);
      Player me = g.state().next_player();
      int chosen = Game::PASS;
      while (not candidates.empty()) {
        size_t k = s::uniform_int_distribution<size_t>(0, candidates.size() - 1)(rng);
        int a = candidates[k];
        Pt pt = point<Game::SIZE>(static_cast<uint>(a));
        if (g.is_legal(a) and not is_eye(g.board(), pt, me)) { chosen = a; break; }
        candidates[k] = candidates.back();
        candidates.pop_back();
      }
      g.apply(chosen);
    }
  }

  int             mSimulations;
  float           mC;
  s::vector<Node> mNodes;
};

// Plays `games` games between a and b at once, colors alternating, each
// game opened with `opening_moves` random moves for variety. Network
// players' searches are batched: one call per player per step. Returns the
// result from a's point of view.
template <typename Game, typename Encoder>
MatchResult play_match(
    ArenaPlayer<Game>& a,
    ArenaPlayer<Game>& b,
    const Encoder* encoder,
    int games,
    int opening_moves,
    float komi,
    int max_moves,
    s::mt19937_64& rng)
{
  using Search = MCGS<Game, Encoder>;
  struct Slot {
    Game                  game;
    bool                  a_black;
    s::unique_ptr<Search> search[2];  // 0: a, 1: b
    bool                  searching = false;
    bool                  done = false;
  };
  ArenaPlayer<Game>* players[2] = {&a, &b};
  int capacity[2] = {players[0]->simulations() + 2, players[1]->simulations() + 2};
  for (int p = 0; p < 2; ++p)
    if (players[p]->inference()
        and players[p]->inference()->latent_capacity() < static_cast<int64_t>(games) * capacity[p])
      throw s::invalid_argument("inference latent capacity too small for the match");

  s::vector<Slot> slots(games);
  for (int i = 0; i < games; ++i) {
    slots[i].game = Game(komi, max_moves);
    slots[i].a_black = (i % 2 == 0);
    for (int p = 0; p < 2; ++p)
      if (players[p]->inference())
        slots[i].search[p] = s::make_unique<Search>(encoder, players[p]->search_config(),
          static_cast<int64_t>(i) * capacity[p], capacity[p], rng());
  }
  // random openings, never passing
  for (Slot& slot : slots)
    for (int m = 0; m < opening_moves and not slot.game.is_terminal(); ++m) {
      ActionList<Game::NUM_ACTIONS> legal;
      slot.game.legal_actions(legal);
      if (legal.size <= 1) break;
      slot.game.apply(legal[s::uniform_int_distribution<int>(0, legal.size - 2)(rng)]);
    }

  MatchResult result;
  InferenceRequests req[2];
  InferenceResults res[2];
  req[0].observation_size = req[1].observation_size = encoder->observation_size();
  s::vector<int> waiting[2];
  int active = games;
  while (active > 0) {
    for (int p = 0; p < 2; ++p) { req[p].clear(); waiting[p].clear(); }
    for (int i = 0; i < games; ++i) {
      Slot& slot = slots[i];
      while (not slot.done) {
        if (slot.game.is_terminal()) {
          int outcome = slot.game.outcome(slot.a_black ? 0 : 1);
          if (outcome > 0) result.wins += 1;
          else if (outcome < 0) result.losses += 1;
          else result.draws += 1;
          slot.done = true;
          --active;
          break;
        }
        int side = slot.game.to_play();
        int p = ((side == 0) == slot.a_black) ? 0 : 1;
        if (players[p]->inference()) {
          Search& search = *slot.search[p];
          if (not slot.searching) {
            search.start(slot.game, players[p]->simulations(), false);
            slot.searching = true;
          }
          if (search.step(req[p])) { waiting[p].push_back(i); break; }
          slot.game.apply(search.result(0.F).action);
          slot.searching = false;
        } else {
          slot.game.apply(players[p]->select(slot.game, rng));
        }
      }
    }
    for (int p = 0; p < 2; ++p) {
      if (waiting[p].empty()) continue;
      players[p]->inference()->infer(req[p], res[p]);
      for (int i : waiting[p]) slots[i].search[p]->receive(res[p]);
    }
  }
  return result;
}

// Elo ratings of every player from all results at once, fit as a
// Bradley-Terry model (a draw is half a win for each side) with the
// minorization-maximization algorithm. Each pair that met also gets one
// virtual draw, so a player that never loses keeps a finite rating.
struct EloTable {
  void add(const s::string& a, const s::string& b, const MatchResult& r) {
    mPlayers.insert(a);
    mPlayers.insert(b);
    auto& ab = mScores[{a, b}];
    ab.first += r.wins + 0.5 * r.draws;
    ab.second += r.games();
    auto& ba = mScores[{b, a}];
    ba.first += r.losses + 0.5 * r.draws;
    ba.second += r.games();
  }

  s::map<s::string, double> ratings(const s::string& anchor, double anchor_rating = 0.) const {
    s::map<s::string, double> gamma;
    for (const s::string& p : mPlayers) gamma[p] = 1.;
    for (int it = 0; it < 1000; ++it) {
      double change = 0.;
      for (const s::string& p : mPlayers) {
        double wins = 0., denom = 0.;
        for (const auto& [key, wn] : mScores) {
          if (key.first != p) continue;
          double n = wn.second + 1.;
          wins += wn.first + 0.5;
          denom += n / (gamma[p] + gamma[key.second]);
        }
        if (denom <= 0.) continue;
        double g = wins / denom;
        change = s::max(change, s::abs(s::log(g / gamma[p])));
        gamma[p] = g;
      }
      if (change < 1e-9) break;
    }
    double ref = gamma.contains(anchor) ? gamma[anchor] : 1.;
    s::map<s::string, double> out;
    for (const auto& [p, g] : gamma) out[p] = anchor_rating + 400. * s::log10(g / ref);
    return out;
  }

private:
  s::set<s::string>                                       mPlayers;
  s::map<s::pair<s::string, s::string>, s::pair<double, double>> mScores;
};

struct EvaluatorConfig {
  int            games          = 32;   // per snapshot opponent
  int            baseline_games = 32;   // per baseline
  int            simulations    = 32;   // network players' search budget
  int            opening_moves  = 4;
  int            rollout_simulations = 200;  // 0 disables the rollout baseline
  s::vector<int> gauntlet       = {1, 3};    // snapshots this many places back
  float          komi           = 7.5F;
  int            max_moves      = 162;
};

// For every new snapshot plays the snapshots `gauntlet` places back, the
// random player (rating anchor at 0) and the UCT rollout baseline, then
// refits Elo over all results. Writes dir/results.csv (every match),
// dir/ratings.csv (current ratings) and dir/summary.log, and reloads
// results.csv on restart.
template <typename Game, typename Encoder>
struct SnapshotEvaluator {
  SnapshotEvaluator(const EvaluatorConfig& config, const s::string& dir,
                    const TransformerMuZeroConfig& model_config, const Encoder* encoder,
                    t::Device device, t::ScalarType dtype, uint64_t seed):
    mConfig(config), mDir(dir), mEncoder(encoder), mDevice(device), mRng(seed)
  {
    sf::create_directories(mDir);
    int64_t capacity = static_cast<int64_t>(s::max(config.games, config.baseline_games))
                     * (config.simulations + 2);
    mCurrent  = s::make_unique<ModelInference>(model_config, device, dtype, capacity);
    mOpponent = s::make_unique<ModelInference>(model_config, device, dtype, capacity);
    load_results();
  }

  // a snapshot evaluated in an earlier run, kept as a gauntlet opponent
  void add_existing(const s::string& name, const s::string& file) {
    s::lock_guard<s::mutex> lock(mMutex);
    mSnapshots.emplace_back(name, file);
  }

  void submit(const s::string& name, const s::string& file) {
    {
      s::lock_guard<s::mutex> lock(mMutex);
      mQueue.emplace_back(name, file);
    }
    mCv.notify_one();
  }

  // evaluates queued snapshots until stop; when evaluation falls behind
  // training only the newest snapshot is evaluated
  void run(const s::atomic<bool>& stop) {
    while (not stop.load()) {
      s::pair<s::string, s::string> job;
      {
        s::unique_lock<s::mutex> lock(mMutex);
        mCv.wait_for(lock, s::chrono::milliseconds(500), [&]{ return not mQueue.empty(); });
        if (mQueue.empty()) continue;
        job = mQueue.back();
        mQueue.clear();
      }
      evaluate(job.first, job.second, stop);
    }
  }

  // synchronous evaluation of one snapshot; returns the summary line
  s::string evaluate(const s::string& name, const s::string& file, const s::atomic<bool>& stop) {
    auto t0 = s::chrono::steady_clock::now();
    load_into(*mCurrent, file);
    NetworkPlayer<Game> current(name, mCurrent.get(), mConfig.simulations);
    s::vector<s::pair<s::string, s::string>> snapshots;
    {
      s::lock_guard<s::mutex> lock(mMutex);
      snapshots = mSnapshots;
    }
    s::ostringstream line;
    line << "[eval] " << name << ":";
    for (int back : mConfig.gauntlet) {
      if (stop.load()) return "";
      int idx = static_cast<int>(snapshots.size()) - back;
      if (idx < 0) continue;
      const auto& [oname, ofile] = snapshots[idx];
      load_into(*mOpponent, ofile);
      NetworkPlayer<Game> opponent(oname, mOpponent.get(), mConfig.simulations);
      MatchResult r = match(current, opponent, mConfig.games);
      record(name, oname, r);
      line << s::format(" vs {} {}-{}", oname, r.wins, r.losses);
    }
    if (stop.load()) return "";
    RandomPlayer<Game> random;
    MatchResult rr = match(current, random, mConfig.baseline_games);
    record(name, random.name(), rr);
    line << s::format(" vs random {}-{}", rr.wins, rr.losses);
    s::string rollout_name;
    if (mConfig.rollout_simulations > 0 and not stop.load()) {
      RolloutPlayer<Game> rollout(mConfig.rollout_simulations);
      rollout_name = rollout.name();
      MatchResult r = match(current, rollout, mConfig.baseline_games);
      record(name, rollout_name, r);
      line << s::format(" vs {} {}-{}", rollout_name, r.wins, r.losses);
    }
    s::map<s::string, double> elo = mElo.ratings("random");
    write_ratings(elo);
    double secs = s::chrono::duration<double>(s::chrono::steady_clock::now() - t0).count();
    line << s::format(" | elo {:.0f}", elo[name]);
    if (not rollout_name.empty()) line << s::format(" ({} {:.0f})", rollout_name, elo[rollout_name]);
    line << s::format(" [{:.0f}s]", secs);
    {
      s::lock_guard<s::mutex> lock(mMutex);
      mSnapshots.emplace_back(name, file);
      mLastElo = elo[name];
    }
    s::ofstream(mDir / "summary.log", s::ios::app) << line.str() << "\n";
    s::cout << line.str() << s::endl;
    return line.str();
  }

  double last_elo() const { s::lock_guard<s::mutex> l(mMutex); return mLastElo; }
  s::map<s::string, double> ratings() const { return mElo.ratings("random"); }

private:
  MatchResult match(ArenaPlayer<Game>& a, ArenaPlayer<Game>& b, int games) {
    return play_match<Game, Encoder>(a, b, mEncoder, games, mConfig.opening_moves,
                                     mConfig.komi, mConfig.max_moves, mRng);
  }

  void load_into(ModelInference& inf, const s::string& file) {
    TransformerMuZero model = load_model(file, mDevice);
    inf.set_weights(model, 0);
  }

  void record(const s::string& a, const s::string& b, const MatchResult& r) {
    mElo.add(a, b, r);
    sf::path path = mDir / "results.csv";
    bool header = not sf::exists(path);
    s::ofstream out(path, s::ios::app);
    if (header) out << "player,opponent,wins,draws,losses\n";
    out << a << ',' << b << ',' << r.wins << ',' << r.draws << ',' << r.losses << '\n';
  }

  void load_results() {
    s::ifstream in(mDir / "results.csv");
    s::string line;
    s::getline(in, line);
    while (s::getline(in, line)) {
      s::istringstream ss(line);
      s::string a, b, w, d, l;
      if (not (s::getline(ss, a, ',') and s::getline(ss, b, ',') and s::getline(ss, w, ',')
               and s::getline(ss, d, ',') and s::getline(ss, l, ','))) continue;
      mElo.add(a, b, MatchResult{s::stoi(w), s::stoi(d), s::stoi(l)});
    }
  }

  void write_ratings(const s::map<s::string, double>& elo) const {
    s::ofstream out(mDir / "ratings.csv");
    out << "player,elo\n";
    for (const auto& [p, e] : elo) out << p << ',' << s::format("{:.1f}", e) << '\n';
  }

  EvaluatorConfig                          mConfig;
  sf::path                                 mDir;
  const Encoder*                           mEncoder;
  t::Device                                mDevice;
  s::mt19937_64                            mRng;
  s::unique_ptr<ModelInference>            mCurrent;
  s::unique_ptr<ModelInference>            mOpponent;
  EloTable                                 mElo;

  mutable s::mutex                         mMutex;
  s::condition_variable                    mCv;
  s::deque<s::pair<s::string, s::string>>  mQueue;
  s::vector<s::pair<s::string, s::string>> mSnapshots;
  double                                   mLastElo = 0.;
};

}  // namespace rlgames::muzero

#endif  // RLGAMES_MUZERO_EVALUATION
