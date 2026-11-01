#ifndef RLGAMES_MUZERO_GO_GAME
#define RLGAMES_MUZERO_GO_GAME

#include <array>
#include <cassert>
#include <cstdint>

#include <type_alias.h>
#include <types.h>
#include <go_types.h>

namespace rlgames::muzero {

namespace s = std;

// fixed capacity list of action indices
template <int N>
struct ActionList {
  s::array<int16_t, N> actions;
  int size = 0;

  void push_back(int a) { actions[size++] = static_cast<int16_t>(a); }
  int operator[](int i) const { return actions[i]; }
  const int16_t* begin() const { return actions.data(); }
  const int16_t* end() const { return actions.data() + size; }
};

// Go as MuZero sees it: actions are indices (point r * SZ + c, PASS = SZ * SZ),
// players are 0 (black) and 1 (white), and the game ends after two passes or
// after max_moves. Outcomes use area scoring with the given komi (the 9x9
// standard 7.5 by default, so there are no draws).
template <ubyte SZ>
struct GoGame {
  static constexpr int SIZE        = SZ;
  static constexpr int NUM_POINTS  = SZ * SZ;
  static constexpr int NUM_ACTIONS = SZ * SZ + 1;
  static constexpr int PASS        = SZ * SZ;
  static constexpr int NO_MOVE     = -1;

  GoGame() = default;
  explicit GoGame(float komi, int max_moves = 2 * SZ * SZ):
    mKomi(komi), mMaxMoves(max_moves)
  {}

  int to_play() const {
    return mState.next_player() == Player::Black ? 0 : 1;
  }
  int num_moves() const { return mMoves; }
  int last_action() const { return mLast; }
  int second_last_action() const { return mSecondLast; }
  float komi() const { return mKomi; }
  const GoGameState<SZ>& state() const { return mState; }
  const GoBoard<SZ>& board() const { return mState.board(); }

  bool is_terminal() const {
    return mState.is_over() or mMoves >= mMaxMoves;
  }

  bool is_legal(int action) const {
    if (is_terminal()) return false;
    return mState.is_valid_move(to_move(action));
  }

  // every legal action; pass is always legal in a running game
  void legal_actions(ActionList<NUM_ACTIONS>& out) const {
    out.size = 0;
    if (is_terminal()) return;
    for (int a = 0; a < NUM_POINTS; ++a)
      if (mState.is_valid_move(to_move(a))) out.push_back(a);
    out.push_back(PASS);
  }

  void apply(int action) {
    assert(not is_terminal());
    assert(is_legal(action));
    mState.apply_move(to_move(action));
    mSecondLast = mLast;
    mLast = action;
    ++mMoves;
  }

  // final result for player (0 black, 1 white): +1 win, -1 loss
  int outcome(int player) const {
    GoAreaScore<SZ> score(mState.board(), mKomi);
    Player winner = score.winner();
    int winner_idx = (winner == Player::Black) ? 0 : 1;
    return winner_idx == player ? 1 : -1;
  }

  // area score margin from black's point of view, komi included
  float score_margin() const {
    GoAreaScore<SZ> score(mState.board(), mKomi);
    return score.winning_margin();
  }

  // final owner per point: 0 none, 1 black, 2 white
  s::array<uint8_t, NUM_POINTS> ownership() const {
    GoAreaScore<SZ> score(mState.board(), mKomi);
    s::array<Player, NUM_POINTS> owner = score.ownership();
    s::array<uint8_t, NUM_POINTS> out{};
    for (int i = 0; i < NUM_POINTS; ++i)
      out[i] = owner[i] == Player::Black ? 1 : owner[i] == Player::White ? 2 : 0;
    return out;
  }

  // transposition identity (stones and player to move)
  uint64 position_key() const { return mState.position_key(); }
  bool same_position(const GoGame& o) const {
    return mState.same_position(o.mState);
  }

  static Move to_move(int action) {
    if (action == PASS) return Move(M::Pass);
    return Move(M::Play, point<SZ>(static_cast<uint>(action)));
  }
  static int to_action(const Move& m) {
    if (m.mty == M::Play) return static_cast<int>(index<SZ>(m.mpt));
    return PASS;
  }

private:
  GoGameState<SZ> mState;
  float           mKomi       = 7.5F;
  int             mMaxMoves   = 2 * SZ * SZ;
  int             mMoves      = 0;
  int             mLast       = NO_MOVE;
  int             mSecondLast = NO_MOVE;
};

}  // namespace rlgames::muzero

#endif  // RLGAMES_MUZERO_GO_GAME
