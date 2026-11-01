// MuZero building blocks: game adapter, token encoder, experience storage,
// transformer model, batched inference.

#include <filesystem>
#include <random>
#include <set>
#include <vector>

#include <gtest/gtest.h>
#include <torch/torch.h>

#include <go_game.h>
#include <go_encoder.h>
#include <experience.h>
#include <transformer_muzero.h>
#include <inference.h>

namespace Z  = rlgames::muzero;
namespace R  = rlgames;
namespace t  = torch;
namespace fs = std::filesystem;

using Game = Z::GoGame<9>;
using Enc  = Z::GoEncoder<9>;

namespace {

Game play(std::initializer_list<int> actions) {
  Game g;
  for (int a : actions) g.apply(a);
  return g;
}

Z::TransformerMuZeroConfig tiny_config() {
  Z::TransformerMuZeroConfig c;
  c.dims = 32; c.num_heads = 4; c.repr_layers = 1; c.dyn_layers = 1;
  c.pred_layers = 1; c.proj_dims = 16;
  return c;
}

std::shared_ptr<Z::Trajectory> random_trajectory(std::mt19937_64& rng, const Enc& enc) {
  auto tr = std::make_shared<Z::Trajectory>();
  tr->observation_size = enc.observation_size();
  tr->num_tokens = enc.num_tokens();
  tr->num_actions = enc.num_actions();
  Game g(7.5F, 30);
  Z::ActionList<Game::NUM_ACTIONS> legal;
  auto add_state = [&]{
    size_t f = tr->features.size();
    tr->features.resize(f + tr->observation_size);
    enc.encode(g, tr->features.data() + f);
    size_t k = tr->tokens.size();
    tr->tokens.resize(k + tr->num_tokens);
    enc.encode_tokens(g, tr->tokens.data() + k);
    tr->to_play.push_back(g.to_play());
  };
  while (not g.is_terminal()) {
    add_state();
    g.legal_actions(legal);
    int a = legal[std::uniform_int_distribution<int>(0, legal.size - 1)(rng)];
    tr->actions.push_back(a);
    for (int i = 0; i < tr->num_actions; ++i) tr->policies.push_back(i == a ? 1.F : 0.F);
    tr->policy_valid.push_back(rng() & 1);
    tr->root_values.push_back(0.25F);
    int mover = g.to_play();
    g.apply(a);
    tr->rewards.push_back(g.is_terminal() ? g.outcome(mover) : 0);
    ++tr->length;
  }
  add_state();
  auto own = g.ownership();
  tr->final_owner.assign(own.begin(), own.end());
  tr->outcome = g.outcome(0);
  tr->score_margin = g.score_margin();
  return tr;
}

}  // namespace

// ------------------------------------------------------------------- game

TEST(GoGame, ActionsAndPlayers) {
  Game g;
  EXPECT_EQ(g.to_play(), 0);
  Z::ActionList<Game::NUM_ACTIONS> legal;
  g.legal_actions(legal);
  EXPECT_EQ(legal.size, 82);  // 81 points + pass
  g.apply(40);
  EXPECT_EQ(g.to_play(), 1);
  EXPECT_EQ(g.board().get(R::Pt(4, 4)), R::Player::Black);
  EXPECT_FALSE(g.is_legal(40));
  EXPECT_EQ(g.last_action(), 40);
  g.apply(Game::PASS);
  EXPECT_EQ(g.second_last_action(), 40);
  EXPECT_FALSE(g.is_terminal());
  g.apply(Game::PASS);
  EXPECT_TRUE(g.is_terminal());
}

TEST(GoGame, KomiDecidesEmptyBoardAndMoveCapEndsGame) {
  Game g(7.5F);
  g.apply(Game::PASS);
  g.apply(Game::PASS);
  EXPECT_EQ(g.outcome(1), 1);   // white wins on komi
  EXPECT_EQ(g.outcome(0), -1);
  Game capped(7.5F, 3);
  capped.apply(10); capped.apply(20); capped.apply(30);
  EXPECT_TRUE(capped.is_terminal());
}

TEST(GoGame, OwnershipCountsTerritory) {
  // black wall on column 4: everything black
  Game g;
  for (int r = 0; r < 9; ++r) { g.apply(r * 9 + 4); g.apply(Game::PASS); }
  auto own = g.ownership();
  for (int i = 0; i < 81; ++i) EXPECT_EQ(own[i], 1) << i;
  EXPECT_FLOAT_EQ(g.score_margin(), 81.F - 7.5F);
}

// ---------------------------------------------------------------- encoder

TEST(GoEncoder, FeaturesAreRelativeToTheMover) {
  Enc enc;
  Game g = play({40, 41});  // black E5, white F5; black to move
  std::vector<uint8_t> f(enc.observation_size());
  enc.encode(g, f.data());
  auto feat = [&](int tok, int i) { return f[tok * Enc::NUM_FEATURES + i]; };
  EXPECT_EQ(feat(40, 1), 1);  // own stone
  EXPECT_EQ(feat(41, 2), 1);  // opponent stone
  EXPECT_EQ(feat(40, 3 + 2), 1);  // own string with 3 liberties
  EXPECT_EQ(feat(41, 7 + 2), 1);  // opponent string with 3 liberties
  EXPECT_EQ(feat(0, 0), 1);   // empty
  EXPECT_EQ(feat(0, 11), 1);  // legal
  EXPECT_EQ(feat(40, 11), 0);
  EXPECT_EQ(feat(41, 13), 1); // last move
  EXPECT_EQ(feat(40, 14), 1); // move before last
  EXPECT_EQ(feat(5, 16), 1);  // black to move
  // after a pass, white to move sees the same stones swapped
  g.apply(Game::PASS);
  enc.encode(g, f.data());
  EXPECT_EQ(feat(41, 1), 1);
  EXPECT_EQ(feat(40, 2), 1);
  EXPECT_EQ(feat(5, 15), 1);  // last move was a pass
  EXPECT_EQ(feat(5, 16), 0);
}

TEST(GoEncoder, KoForbiddenPointIsMarked) {
  Enc enc;
  // black captures in a ko at E5; white may not retake immediately
  Game g = play({39, 40, 31, 32, 49, 50, Game::PASS, 42, 41});
  ASSERT_EQ(g.board().get(R::Pt(4, 4)), R::Player::Unknown);
  std::vector<uint8_t> f(enc.observation_size());
  enc.encode(g, f.data());
  EXPECT_EQ(f[40 * Enc::NUM_FEATURES + 12], 1);
  EXPECT_EQ(f[40 * Enc::NUM_FEATURES + 11], 0);
}

TEST(GoEncoder, SymmetriesArePermutationsAndKeepPassFixed) {
  Enc enc;
  std::set<std::vector<int>> distinct;
  for (int sym = 0; sym < enc.num_symmetries(); ++sym) {
    const auto& tmap = enc.token_map(sym);
    std::set<int> image(tmap.begin(), tmap.end());
    EXPECT_EQ(image.size(), 81U);
    EXPECT_EQ(enc.action_map(sym)[Game::PASS], Game::PASS);
    distinct.insert(tmap);
  }
  EXPECT_EQ(distinct.size(), 8U);
  EXPECT_EQ(enc.token_map(0)[17], 17);  // identity
}

TEST(GoEncoder, SymmetryMapsLegalMovesToLegalMoves) {
  // play the same moves on a transformed board: legal features must map
  Enc enc;
  std::mt19937 rng(5);
  for (int sym = 0; sym < 8; ++sym) {
    Game a, b;
    for (int m = 0; m < 30; ++m) {
      Z::ActionList<Game::NUM_ACTIONS> legal;
      a.legal_actions(legal);
      int act = legal[std::uniform_int_distribution<int>(0, legal.size - 2)(rng)];
      a.apply(act);
      b.apply(enc.action_map(sym)[act]);
    }
    std::vector<uint8_t> fa(enc.observation_size()), fb(enc.observation_size());
    enc.encode(a, fa.data());
    enc.encode(b, fb.data());
    for (int i = 0; i < 81; ++i)
      for (int k = 0; k < Enc::NUM_FEATURES; ++k)
        ASSERT_EQ(fa[i * Enc::NUM_FEATURES + k],
                  fb[enc.token_map(sym)[i] * Enc::NUM_FEATURES + k]) << sym << " " << i << " " << k;
  }
}

// ------------------------------------------------------------- experience

TEST(Experience, StoreRoundTripAndTruncation) {
  Enc enc;
  std::mt19937_64 rng(1);
  fs::path dir = fs::temp_directory_path() / "muzero_store_test";
  fs::remove_all(dir);
  std::vector<std::shared_ptr<Z::Trajectory>> written;
  {
    Z::ExperienceStore store(dir.string(), 3);
    for (int i = 0; i < 7; ++i) {
      written.push_back(random_trajectory(rng, enc));
      store.write(*written.back());
    }
  }
  Z::ExperienceStore reader(dir.string());
  auto loaded = reader.load_recent(1u << 30);
  ASSERT_EQ(loaded.size(), 7U);
  for (size_t i = 0; i < 7; ++i) {
    EXPECT_EQ(loaded[i]->length, written[i]->length);
    EXPECT_EQ(loaded[i]->features, written[i]->features);
    EXPECT_EQ(loaded[i]->actions, written[i]->actions);
    EXPECT_EQ(loaded[i]->policies, written[i]->policies);
    EXPECT_EQ(loaded[i]->final_owner, written[i]->final_owner);
    EXPECT_EQ(loaded[i]->outcome, written[i]->outcome);
  }
  // load_recent keeps the newest games within the position budget
  size_t last_two = written[5]->length + written[6]->length;
  auto recent = reader.load_recent(last_two);
  ASSERT_EQ(recent.size(), 2U);
  EXPECT_EQ(recent[1]->actions, written[6]->actions);

  // a truncated final record is skipped
  fs::path last;
  for (auto& e : fs::directory_iterator(dir)) last = std::max(last, e.path());
  auto size = fs::file_size(last);
  fs::resize_file(last, size - 10);
  EXPECT_EQ(Z::ExperienceStore(dir.string()).load_recent(1u << 30).size(), 6U);
  fs::remove_all(dir);
}

TEST(Experience, ReplayWindowAndUniformSampling) {
  Enc enc;
  std::mt19937_64 rng(2);
  Z::ReplayBuffer replay(100);
  size_t total = 0;
  for (int i = 0; i < 20; ++i) {
    auto tr = random_trajectory(rng, enc);
    total += tr->length;
    replay.add(tr);
  }
  EXPECT_EQ(replay.total_positions(), total);
  EXPECT_LT(replay.num_positions(), 100U + 30U);
  EXPECT_GE(replay.num_positions(), 100U - 30U);
  std::vector<Z::PositionRef> refs;
  replay.sample(500, rng, refs);
  ASSERT_EQ(refs.size(), 500U);
  for (auto& r : refs) {
    EXPECT_GE(r.t, 0);
    EXPECT_LT(r.t, r.trajectory->length);
  }
}

// ------------------------------------------------------------------ model

TEST(TransformerMuZero, ShapesAndActionConditioning) {
  t::manual_seed(0);
  Z::TransformerMuZero model(tiny_config());
  Enc enc;
  Game g = play({40, 41});
  std::vector<uint8_t> f(enc.observation_size());
  enc.encode(g, f.data());
  t::Tensor obs = t::from_blob(f.data(), {1, 81, Enc::NUM_FEATURES}, t::kUInt8).repeat({2, 1, 1});
  t::Tensor s0 = model->represent(obs);
  EXPECT_EQ(s0.sizes(), (std::vector<int64_t>{2, 81, 32}));
  // latent is normalized per token
  EXPECT_NEAR(s0.mean(-1).abs().max().item<float>(), 0.F, 1e-4);
  auto pred = model->predict(s0);
  EXPECT_EQ(pred.policy_logits.sizes(), (std::vector<int64_t>{2, 82}));
  EXPECT_EQ(pred.value_logits.sizes(), (std::vector<int64_t>{2, 3}));
  EXPECT_EQ(pred.ownership_logits.sizes(), (std::vector<int64_t>{2, 81, 3}));
  auto d = model->dynamics(s0, t::tensor({10, Game::PASS}, t::kLong));
  EXPECT_EQ(d.latent.sizes(), s0.sizes());
  EXPECT_EQ(d.reward_logits.sizes(), (std::vector<int64_t>{2, 3}));
  // different actions from the same state give different next states
  EXPECT_GT((d.latent[0] - d.latent[1]).abs().max().item<float>(), 1e-3);
  EXPECT_GT(model->num_parameters(), 0);
}

TEST(TransformerMuZero, SaveLoadRoundTrip) {
  t::manual_seed(1);
  Z::TransformerMuZero model(tiny_config());
  fs::path path = fs::temp_directory_path() / "muzero_model_test.pt";
  Z::save_model(model, path.string());
  Z::TransformerMuZero loaded = Z::load_model(path.string());
  EXPECT_EQ(loaded->config.dims, 32);
  Enc enc;
  std::vector<uint8_t> f(3 * enc.observation_size());
  enc.encode(Game(), f.data());
  enc.encode(play({40}), f.data() + enc.observation_size());
  enc.encode(play({40, 41, 30}), f.data() + 2 * enc.observation_size());
  t::Tensor obs = t::from_blob(f.data(), {3, 81, Enc::NUM_FEATURES}, t::kUInt8);
  t::NoGradGuard ng;
  EXPECT_TRUE(t::allclose(model->predict(model->represent(obs)).policy_logits,
                          loaded->predict(loaded->represent(obs)).policy_logits));
  fs::remove(path);
}

// -------------------------------------------------------------- inference

TEST(ModelInference, MatchesDirectModelCalls) {
  t::manual_seed(2);
  Z::TransformerMuZero model(tiny_config());
  model->eval();
  Z::ModelInference inf(model->config, t::kCPU, t::kFloat, 16);
  inf.set_weights(model, 7);
  EXPECT_EQ(inf.version(), 7U);

  Enc enc;
  Game g = play({40});
  std::vector<uint8_t> f(enc.observation_size());
  enc.encode(g, f.data());
  Z::InferenceRequests req;
  req.observation_size = enc.observation_size();
  Z::InferenceResults res;
  req.add_initial(f.data(), 3);
  inf.infer(req, res);

  t::NoGradGuard ng;
  t::Tensor obs = t::from_blob(f.data(), {1, 81, Enc::NUM_FEATURES}, t::kUInt8);
  t::Tensor s0 = model->represent(obs);
  auto p0 = model->predict(s0);
  for (int a = 0; a < 82; ++a)
    EXPECT_NEAR(res.initial_logits[a], p0.policy_logits[0][a].item<float>(), 1e-4);
  EXPECT_NEAR(res.initial_values[0], Z::outcome_scalar(p0.value_logits)[0].item<float>(), 1e-5);

  // recurrent request reads latent 3 back from the pool
  req.clear();
  req.add_recurrent(3, 10, 4);
  inf.infer(req, res);
  auto d = model->dynamics(s0, t::tensor({10}, t::kLong));
  auto p1 = model->predict(d.latent);
  EXPECT_NEAR(res.recurrent_logits[5], p1.policy_logits[0][5].item<float>(), 1e-4);
  EXPECT_NEAR(res.recurrent_rewards[0], Z::outcome_scalar(d.reward_logits)[0].item<float>(), 1e-5);

  req.clear();
  req.add_recurrent(3, 10, 99);  // out of range
  EXPECT_THROW(inf.infer(req, res), std::out_of_range);
}
