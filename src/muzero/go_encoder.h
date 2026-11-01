#ifndef RLGAMES_MUZERO_GO_ENCODER
#define RLGAMES_MUZERO_GO_ENCODER

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include <type_alias.h>
#include <types.h>
#include <go_types.h>
#include <go_game.h>

namespace rlgames::muzero {

namespace s = std;

// One token per board point, every feature relative to the side to move so
// the network always evaluates from the mover's perspective. The model turns
// a token into an embedding: a learned embedding of its class (empty, own,
// opponent), a projection of the remaining binary features and a learned
// position embedding.
//
// Features (uint8, 0/1):
//   0 empty  1 own stone  2 opponent stone           (one hot = token class)
//   3..6   own string with 1, 2, 3, 4+ liberties
//   7..10  opponent string with 1, 2, 3, 4+ liberties
//   11     legal move for the side to move
//   12     empty but forbidden by (super)ko
//   13     last move   14  move before last
//   15     last move was a pass (broadcast)
//   16     side to move is black (broadcast; komi favours white)
//
// Token classes (reconstruction / ownership targets): 0 empty, 1 own,
// 2 opponent; absolute classes use 1 black, 2 white.
// Symmetries: the 8 of the square (D4); pass maps to itself.
template <ubyte SZ>
struct GoEncoder {
  using Game = GoGame<SZ>;
  static constexpr int NUM_TOKENS        = Game::NUM_POINTS;
  static constexpr int NUM_FEATURES      = 17;
  static constexpr int NUM_ACTIONS       = Game::NUM_ACTIONS;
  static constexpr int NUM_TOKEN_CLASSES = 3;
  static constexpr int NUM_SYMMETRIES    = 8;
  static constexpr int OBSERVATION_SIZE  = NUM_TOKENS * NUM_FEATURES;

  GoEncoder() {
    for (int sym = 0; sym < NUM_SYMMETRIES; ++sym) {
      s::vector<int> tmap(NUM_TOKENS);
      for (int r = 0; r < SZ; ++r)
        for (int c = 0; c < SZ; ++c) {
          auto [tr, tc] = transform(sym, r, c);
          tmap[r * SZ + c] = tr * SZ + tc;
        }
      s::vector<int> amap = tmap;
      amap.push_back(Game::PASS);
      mTokenMaps.push_back(s::move(tmap));
      mActionMaps.push_back(s::move(amap));
    }
  }

  int num_tokens() const { return NUM_TOKENS; }
  int num_features() const { return NUM_FEATURES; }
  int num_actions() const { return NUM_ACTIONS; }
  int num_token_classes() const { return NUM_TOKEN_CLASSES; }
  int observation_size() const { return OBSERVATION_SIZE; }

  void encode(const Game& game, uint8_t* features) const {
    const GoBoard<SZ>& board = game.board();
    Player me = game.state().next_player();
    bool running = not game.is_terminal();
    int last = game.last_action();
    int second = game.second_last_action();
    uint8_t last_pass = (last == Game::PASS) ? 1 : 0;
    uint8_t black = (me == Player::Black) ? 1 : 0;

    for (int i = 0; i < NUM_TOKENS; ++i) {
      uint8_t* f = features + i * NUM_FEATURES;
      s::fill(f, f + NUM_FEATURES, uint8_t{0});
      Pt pt = point<SZ>(static_cast<uint>(i));
      const GoStr<SZ>* string = board.get_string(pt);
      if (string == nullptr) {
        f[0] = 1;
        if (running) {
          Move m(M::Play, pt);
          bool legal = game.state().is_valid_move(m);
          f[11] = legal ? 1 : 0;
          if (not legal and game.state().does_move_violate_ko(m)) f[12] = 1;
        }
      } else {
        bool own = string->color() == me;
        f[own ? 1 : 2] = 1;
        size_t libs = string->num_liberties();
        int bucket = static_cast<int>(s::min<size_t>(libs, 4)) - 1;
        if (bucket >= 0) f[(own ? 3 : 7) + bucket] = 1;
      }
      if (i == last)   f[13] = 1;
      if (i == second) f[14] = 1;
      f[15] = last_pass;
      f[16] = black;
    }
  }

  // 0 empty, 1 own, 2 opponent
  void encode_tokens(const Game& game, uint8_t* tokens) const {
    const GoBoard<SZ>& board = game.board();
    Player me = game.state().next_player();
    for (int i = 0; i < NUM_TOKENS; ++i) {
      Player p = board.get(point<SZ>(static_cast<uint>(i)));
      tokens[i] = p == Player::Unknown ? 0 : (p == me ? 1 : 2);
    }
  }

  // 0 empty, 1 black, 2 white
  void encode_tokens_absolute(const Game& game, uint8_t* tokens) const {
    const GoBoard<SZ>& board = game.board();
    for (int i = 0; i < NUM_TOKENS; ++i) {
      Player p = board.get(point<SZ>(static_cast<uint>(i)));
      tokens[i] = p == Player::Unknown ? 0 : (p == Player::Black ? 1 : 2);
    }
  }

  // absolute <-> relative for the other player
  static int flip_token_class(int c) { return c == 0 ? 0 : 3 - c; }

  int num_symmetries() const { return NUM_SYMMETRIES; }
  // token i of the original board lands on token_map(sym)[i]
  const s::vector<int>& token_map(int sym) const { return mTokenMaps[sym]; }
  const s::vector<int>& action_map(int sym) const { return mActionMaps[sym]; }

  // rotations by 0, 90, 180, 270 degrees, then the same after a transpose
  static s::pair<int, int> transform(int sym, int r, int c) {
    if (sym >= 4) s::swap(r, c);
    for (int i = 0; i < sym % 4; ++i) {
      int nr = c;
      int nc = SZ - 1 - r;
      r = nr;
      c = nc;
    }
    return {r, c};
  }

private:
  s::vector<s::vector<int>> mTokenMaps;
  s::vector<s::vector<int>> mActionMaps;
};

}  // namespace rlgames::muzero

#endif  // RLGAMES_MUZERO_GO_ENCODER
