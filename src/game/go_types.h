#ifndef RLGAMES_GO_TYPES
#define RLGAMES_GO_TYPES

#include <cassert>
#include <cstring>
#include <bitset>
#include <array>
#include <vector>
#include <unordered_set>
#include <functional>
#include <algorithm>
#include <ostream>
#include <utility>

#include <type_alias.h>
#include <types.h>
#include <zobrist_hash.h>
#include <game_base.h>

namespace rlgames {

namespace s = std;

// Liberties and color of one string of connected stones. A string is
// identified by its root point (one of its stones), which stays the same for
// as long as the string exists; its stones are found through GoBoard.
template <ubyte SZ>
struct GoStr {
  static constexpr uint SIZE = SZ;
  static constexpr uint IZ   = SZ * SZ;

  GoStr() = default;
  GoStr(Player color, const s::bitset<IZ>& liberties):
    mLiberties(liberties), mColor(color), mSize(1U)
  {}

  Player color() const { return mColor; }
  const s::bitset<IZ>& liberties() const { return mLiberties; }
  size_t num_liberties() const { return mLiberties.count(); }
  uint size() const { return mSize; }

private:
  template <ubyte> friend struct GoBoard;

  s::bitset<IZ> mLiberties;
  Player        mColor = Player::Unknown;
  udyte         mSize  = 0U;
};

template <ubyte SZ> struct GoAreaScore;

// Go board with stable string identities.
//
// Each point records the root point of its string (NONE when empty); string
// data lives at the root's slot of a fixed array, and a circular next-stone
// list links the stones of each string. Merging relabels only the smaller
// string, capturing walks only the captured stones, and nothing ever moves
// another string, so string ids and GoStr pointers stay valid until that
// string itself is merged or captured. All storage is fixed-size, so copying
// a board is a plain memory copy without allocation.
template <ubyte SZ>
struct GoBoard : Board<GoBoard<SZ>> {
  friend struct GoAreaScore<SZ>;
  static constexpr uint  SIZE = SZ;
  static constexpr uint  IZ   = SZ * SZ;
  static constexpr udyte NONE = 0xFFFFU;
  static_assert(IZ < NONE);

  GoBoard(): mHash(EMPTY_BOARD) {
    mRoot.fill(NONE);
    mNext.fill(NONE);
  }

  uint size() const { return SZ; }
  uint hash() const { return mHash; }
  [[nodiscard]] bool is_on_grid(Pt pt) const { return (pt.r < SZ and pt.c < SZ); }

  [[nodiscard]] Player get(Pt pt) const {
    assert(is_on_grid(pt));

    udyte root = mRoot[index<SZ>(pt)];
    return (root == NONE) ? Player::Unknown : mStrings[root].color();
  }

  // same stones on the same points; string roots may differ
  [[nodiscard]] bool same_stones(const GoBoard& o) const {
    for (uint i = 0; i < IZ; ++i) {
      udyte a = mRoot[i];
      udyte b = o.mRoot[i];
      if ((a == NONE) != (b == NONE)) return false;
      if (a != NONE and mStrings[a].color() != o.mStrings[b].color())
        return false;
    }
    return true;
  }

  // valid until this string is merged or captured
  const GoStr<SZ>* get_string(Pt pt) const {
    assert(is_on_grid(pt));

    udyte root = mRoot[index<SZ>(pt)];
    return (root == NONE) ? nullptr : &mStrings[root];
  }

  // Place a stone, merge it with friendly neighbours and capture opponent
  // strings left without liberties. A self-capturing stone stays on the
  // board with no liberties, which lets callers detect it.
  void place_stone(Player player, Pt pt) {
    assert(is_on_grid(pt));
    assert(player != Player::Unknown);
    uint idx = index<SZ>(pt);
    assert(mRoot[idx] == NONE);

    mHash ^= zobrist_hash<SZ>(player, pt);

    s::bitset<IZ> liberties;
    for (Pt n : neighbours(pt))
      if (is_on_grid(n) and mRoot[index<SZ>(n)] == NONE)
        liberties.set(index<SZ>(n));
    mRoot[idx]    = static_cast<udyte>(idx);
    mNext[idx]    = static_cast<udyte>(idx);
    mStrings[idx] = GoStr<SZ>(player, liberties);

    // every neighbouring string loses the liberty at pt
    s::array<udyte, 4> roots;
    uint nroots = distinct_neighbour_roots(pt, roots);
    for (uint i = 0; i < nroots; ++i)
      mStrings[roots[i]].mLiberties.reset(idx);

    // merge friendly strings, then capture opponents without liberties
    for (uint i = 0; i < nroots; ++i)
      if (mStrings[roots[i]].color() == player)
        merge_strings(mRoot[idx], roots[i]);
    for (uint i = 0; i < nroots; ++i)
      if (mStrings[roots[i]].color() != player
          and mStrings[roots[i]].num_liberties() == 0)
        capture_string(roots[i]);
  }

  // would the stone have no liberties after captures (move not applied)
  [[nodiscard]] bool is_self_capture(Player player, Pt pt) const {
    assert(is_on_grid(pt));
    assert(mRoot[index<SZ>(pt)] == NONE);

    for (Pt n : neighbours(pt)) {
      if (not is_on_grid(n)) continue;
      udyte root = mRoot[index<SZ>(n)];
      if (root == NONE) return false;                      // empty neighbour
      const GoStr<SZ>& string = mStrings[root];
      size_t libs = string.num_liberties();
      if (string.color() == player) {
        if (libs > 1) return false;                        // keeps a liberty
      } else if (libs == 1) {
        return false;                                      // gets captured
      }
    }
    return true;
  }

  // the board hash after place_stone(player, pt), without applying it
  [[nodiscard]] uint hash_after(Player player, Pt pt) const {
    assert(is_on_grid(pt));
    assert(mRoot[index<SZ>(pt)] == NONE);

    uint h = mHash ^ zobrist_hash<SZ>(player, pt);
    s::array<udyte, 4> roots;
    uint nroots = distinct_neighbour_roots(pt, roots);
    for (uint i = 0; i < nroots; ++i) {
      const GoStr<SZ>& string = mStrings[roots[i]];
      // an adjacent opponent string whose last liberty is pt is captured
      if (string.color() != player and string.num_liberties() == 1) {
        udyte stone = roots[i];
        do {
          h ^= zobrist_hash<SZ>(string.color(), point<SZ>(stone));
          stone = mNext[stone];
        } while (stone != roots[i]);
      }
    }
    return h;
  }

  s::ostream& print(s::ostream& out) const {
    char bchar = 'X';
    char wchar = '0';

    out << "   ";
    if (SZ > 9) out << " ";
    for (char c = 'A', i = 0; i < SZ; c++, i++) {
      if (c == 'I') c++;
      out << c << ' ';
    }
    out << '\n';
    for (ubyte i = SZ - 1; i < SZ; --i) {
      // row labels are right-aligned to the header width
      out << ' ';
      if (SZ > 9 and i + 1 < 10) out << ' ';
      out << i + 1 << ' ';
      for (ubyte j = 0; j < SZ; ++j) {
        Player color = get(Pt(i, j));
        if      (color == Player::Black) out << bchar;
        else if (color == Player::White) out << wchar;
        else                             out << '.';
        out << ' ';
      }
      out << i + 1 << '\n';
    }
    out << "   ";
    if (SZ > 9) out << " ";
    for (char c = 'A', i = 0; i < SZ; c++, i++) {
      if (c == 'I') c++;
      out << c << ' ';
    }
    out << '\n';
    return out;
  }

  // string root of every point, in hex
  s::ostream& debug(s::ostream& out) const {
    for (ubyte i = 0; i < SZ; ++i) {
      for (ubyte j = 0; j < SZ; ++j) {
        udyte root = mRoot[index<SZ>(Pt(i, j))];
        if (root == NONE)
          out << "   ";
        else
          out << s::hex << root << s::dec << " ";
      }
      out << "\n";
    }
    return out;
  }

protected:
  // roots of the strings next to pt, each once
  uint distinct_neighbour_roots(Pt pt, s::array<udyte, 4>& roots) const {
    uint count = 0;
    for (Pt n : neighbours(pt)) {
      if (not is_on_grid(n)) continue;
      udyte root = mRoot[index<SZ>(n)];
      if (root == NONE) continue;
      if (s::find(roots.begin(), roots.begin() + count, root)
          == roots.begin() + count)
        roots[count++] = root;
    }
    return count;
  }

  // merge two strings of the same color; the larger keeps its root
  void merge_strings(udyte a, udyte b) {
    if (a == b) return;
    assert(mStrings[a].color() == mStrings[b].color());
    if (mStrings[a].size() < mStrings[b].size()) s::swap(a, b);

    udyte stone = b;
    do {
      mRoot[stone] = a;
      stone = mNext[stone];
    } while (stone != b);
    s::swap(mNext[a], mNext[b]);  // splice the two circular lists

    GoStr<SZ>& keep = mStrings[a];
    keep.mLiberties |= mStrings[b].mLiberties;
    keep.mSize      += mStrings[b].mSize;
  }

  // remove a string; its stones become liberties of the strings around them
  void capture_string(udyte root) {
    Player color = mStrings[root].color();
    udyte stone = root;
    do {
      for (Pt n : neighbours(point<SZ>(stone))) {
        if (not is_on_grid(n)) continue;
        udyte nroot = mRoot[index<SZ>(n)];
        if (nroot != NONE and nroot != root)
          mStrings[nroot].mLiberties.set(stone);
      }
      stone = mNext[stone];
    } while (stone != root);

    stone = root;
    do {
      udyte next = mNext[stone];
      mHash ^= zobrist_hash<SZ>(color, point<SZ>(stone));
      mRoot[stone] = NONE;
      mNext[stone] = NONE;
      stone = next;
    } while (stone != root);
  }

private:
  s::array<udyte, IZ>     mRoot;     // string root of each point, NONE if empty
  s::array<udyte, IZ>     mNext;     // next stone of the same string (circular)
  s::array<GoStr<SZ>, IZ> mStrings;  // valid at root points only
  uint                    mHash;
};

template <ubyte SZ>
s::ostream& operator<<(s::ostream& out, const GoBoard<SZ>& board) {
  return board.print(out);
}

template <ubyte SZ>
constexpr float default_komi() {
  if      constexpr(SZ < 5)              return 0.F;
  else if constexpr(SZ >= 5 &&  SZ < 7)  return 0.5F;
  else if constexpr(SZ >= 7 &&  SZ < 10) return 1.5F;
  else if constexpr(SZ >= 10 && SZ < 13) return 2.5F;
  else if constexpr(SZ >= 13 && SZ < 14) return 3.5F;
  else if constexpr(SZ >= 14 && SZ < 16) return 4.5F;
  else if constexpr(SZ >= 16 && SZ < 17) return 5.5F;
  else if constexpr(SZ >= 17 && SZ < 19) return 6.5F;
  else                                   return 7.5F;
}

// scoring using area rule: player pieces on board + territory + komi
template <ubyte SZ>
struct GoAreaScore {
  static constexpr uint  SIZE = SZ;
  static constexpr uint  IZ   = SZ * SZ;
  static constexpr ubyte DAME = static_cast<ubyte>(s::to_underlying(Player::White))
                              | static_cast<ubyte>(s::to_underlying(Player::Black));

  explicit GoAreaScore(const GoBoard<SZ>& board, float komi = 7.5):
    mBoard(board),
    mBlackPoints(0.F), mBlackTerritory(0.F),
    mWhitePoints(0.F), mWhiteTerritory(0.F),
    mDames(0), mKomi(komi)
  {}

  [[nodiscard]] Player winner() {
    if (not is_winner_computed()) compute_winner();
    if (mBlackPoints + mBlackTerritory >
        mWhitePoints + mWhiteTerritory + mKomi)
      return Player::Black;
    else
      return Player::White;
  }

  float winning_margin() {
    if (not is_winner_computed()) compute_winner();
    return mBlackPoints - mWhitePoints
         + mBlackTerritory - mWhiteTerritory
         - mKomi;
  }

  // final owner of every point under area scoring: the stone's color, the
  // surrounding color for territory, Player::Unknown for dame
  s::array<Player, IZ> ownership() {
    s::array<ubyte, IZ> labels = create_territory_labeling();
    s::array<Player, IZ> owner;
    for (uint i = 0; i < IZ; ++i) {
      Player stone = mBoard.get(point<SZ>(i));
      if (stone != Player::Unknown)
        owner[i] = stone;
      else if (labels[i] == static_cast<ubyte>(s::to_underlying(Player::Black)))
        owner[i] = Player::Black;
      else if (labels[i] == static_cast<ubyte>(s::to_underlying(Player::White)))
        owner[i] = Player::White;
      else
        owner[i] = Player::Unknown;
    }
    return owner;
  }

protected:
  s::array<ubyte, IZ> create_territory_labeling() {
    struct Recursion {
      Recursion(const GoBoard<SZ>& board): board(board) {}
      ubyte operator()(Pt spt, s::bitset<IZ>& points) {
        points.set(index<SZ>(spt));
        ubyte coloring = 0;
        for (Pt neighbour : neighbours(spt)) {
          if (not board.is_on_grid(neighbour)) continue;
          Player color = board.get(neighbour);
          if ((not points.test(index<SZ>(neighbour)))
              and color == Player::Unknown)
          {
            ubyte rret = this->operator()(neighbour, points);
            coloring |= rret;
          } else if (color != Player::Unknown) {
            coloring |= static_cast<ubyte>(s::to_underlying(color));
          }
        }
        // 0 here means "no bordering stone found yet"; only the top-level
        // caller may turn an empty coloring into DAME, otherwise a dead-end
        // in the flood fill would mark a whole territory as dame
        return coloring;
      }
    private:
      const GoBoard<SZ>& board;
    } recursive_labeling(mBoard);

    s::array<ubyte, IZ> labels;
    s::memset(labels.data(), 0U, sizeof(ubyte) * IZ);
    for (uint i = 0; i < IZ; ++i) {
      Pt pt = point<SZ>(i);
      if (mBoard.get(pt) == Player::Unknown and labels[i] == 0U) {
        s::bitset<IZ> points;
        ubyte coloring = recursive_labeling(pt, points);
        if (coloring == 0) coloring = DAME;
        for (uint i = 0; i < IZ; ++i)
          if (points.test(i))
            labels[i] = coloring;
      }
    }
    return labels;
  }

  void compute_score(const s::array<ubyte, IZ>& labels) {
    for (uint i = 0; i < IZ; ++i) {
      Pt pt = point<SZ>(i);
      Player color = mBoard.get(pt);
      switch (color) {
      case Player::Black:
        mBlackPoints++;
        break;

      case Player::White:
        mWhitePoints++;
        break;

      case Player::Unknown:
        if (labels[i] == static_cast<ubyte>(s::to_underlying(Player::Black)))
          mBlackTerritory++;
        else if (labels[i] == static_cast<ubyte>(s::to_underlying(Player::White)))
          mWhiteTerritory++;
        else if (labels[i] == DAME)
          mDames++;
        else
          s::unreachable();
        break;

      default:
        s::unreachable();
      }
    }
  }

  void compute_winner() {
    s::array<ubyte, IZ> labels = create_territory_labeling();
    compute_score(labels);
  }

  bool is_winner_computed() {
    if (mBlackPoints == 0.F and mWhitePoints == 0.F
        and mBlackTerritory == 0.F and mWhiteTerritory == 0.F)
      return false;
    else
      return true;
  }

private:
  const GoBoard<SZ>& mBoard;
  float              mBlackPoints;
  float              mBlackTerritory;
  float              mWhitePoints;
  float              mWhiteTerritory;
  float              mDames;
  float              mKomi;
};

template <ubyte SZ>
struct GoGameState : GameState<GoBoard<SZ>, GoGameState<SZ>> {
  static constexpr uint SIZE = SZ;
  static constexpr uint IZ   = SZ * SZ;

  GoGameState():
    mBoard(),
    mNPlayer(Player::Black),
    mPMove(M::Unknown),
    mPPMove(M::Unknown),
    mHistory()
  {}
  GoGameState(
      const GoBoard<SZ>& board,
      Player player,
      Move pmove,
      Move ppmove,
      const s::unordered_set<uint>& history):
    mBoard(board),
    mNPlayer(player),
    mPMove(pmove),
    mPPMove(ppmove),
    mHistory(history.begin(), history.end())
  {
    s::ranges::sort(mHistory);
  }
  GoGameState(const GoGameState& o):
    mBoard(o.mBoard),
    mNPlayer(o.mNPlayer),
    mPMove(o.mPMove),
    mPPMove(o.mPPMove),
    mHistory(o.mHistory)
  {}
  GoGameState& operator=(const GoGameState& o) {
    mBoard   = o.mBoard;
    mNPlayer = o.mNPlayer;
    mPMove   = o.mPMove;
    mPPMove  = o.mPPMove;
    mHistory = o.mHistory;
    return *this;
  }
  GoGameState(GoGameState&& o) noexcept :
    mBoard(s::move(o.mBoard)),
    mNPlayer(o.mNPlayer),
    mPMove(o.mPMove),
    mPPMove(o.mPPMove),
    mHistory(s::move(o.mHistory))
  {}
  GoGameState& operator=(GoGameState&& o) noexcept {
    mBoard   = s::move(o.mBoard);
    mNPlayer = o.mNPlayer;
    mPMove   = o.mPMove;
    mPPMove  = o.mPPMove;
    mHistory = s::move(o.mHistory);
    return *this;
  }

  const GoBoard<SZ>& board() const { return mBoard; }
  Player next_player() const { return mNPlayer; }
  Move previous_move() const { return mPMove; }

  // Transposition identity for graph search: the stones and the player to
  // move. The superko history and whether the last move was a pass are
  // deliberately left out, so positions reached by different move orders
  // meet. position_key() is a hash of that identity, same_position() the
  // exact comparison to resolve collisions.
  [[nodiscard]] uint64 position_key() const {
    uint64 key = static_cast<uint64>(mBoard.hash()) << 32;
    key |= static_cast<uint64>(s::to_underlying(mNPlayer));
    return key;
  }
  [[nodiscard]] bool same_position(const GoGameState& o) const {
    return mNPlayer == o.mNPlayer and mBoard.same_stones(o.mBoard);
  }

  [[nodiscard]] bool is_over() const {
    if (mPMove.mty == M::Unknown)  return false;
    if (mPMove.mty == M::Resign)   return true;
    if (mPPMove.mty == M::Unknown) return false;
    return (mPMove.mty == M::Pass and mPPMove.mty == M::Pass);
  }
  bool does_move_violate_ko(Move move) const {
    if (move.mty != M::Play) return false;
    return s::ranges::binary_search(
      mHistory, mBoard.hash_after(mNPlayer, move.mpt));
  }
  [[nodiscard]] bool is_valid_move(Move move) const {
    if (is_over())                                          return false;
    if (move.mty == M::Pass or move.mty == M::Resign)       return true;
    // does not allow self capture in this game
    return
      mBoard.get(move.mpt) == Player::Unknown
      and not is_move_self_capture(move)
      and not does_move_violate_ko(move);
  }
  s::vector<Move> legal_moves() const {
    if (is_over()) return s::vector<Move>();
    s::vector<Move> ret; ret.reserve(IZ);
    for (uint r = 0; r < SZ; ++r)
      for (uint c = 0; c < SZ; ++c) {
        Move m(M::Play, Pt(r, c));
        if (is_valid_move(m))
          ret.push_back(m);
      }
    ret.push_back(Move(M::Pass));
    ret.push_back(Move(M::Resign));
    return ret;
  }
  [[nodiscard]] Player winner() {
    if (not is_over()) return Player::Unknown;
    if (mPMove.mty == M::Resign) return mNPlayer;
    GoAreaScore<SZ> scorer(mBoard, default_komi<SZ>());
    return scorer.winner();
  }
  GoGameState& apply_move(Move move) {
    mPPMove = mPMove;
    mPMove  = move;
    if (move.mty == M::Play) {
      mBoard.place_stone(mNPlayer, move.mpt);
      uint h = mBoard.hash();
      auto it = s::ranges::lower_bound(mHistory, h);
      if (it == s::end(mHistory) or *it != h) mHistory.insert(it, h);
    }
    mNPlayer = other_player(mNPlayer);
    return *this;
  }

protected:
  // self capture is always pruned — it's always bad
  bool is_move_self_capture(Move move) const {
    if (move.mty != M::Play) return false;
    return mBoard.is_self_capture(mNPlayer, move.mpt);
  }

private:
  GoBoard<SZ>            mBoard;
  Player                 mNPlayer;   // next player
  Move                   mPMove;     // previous move
  Move                   mPPMove;    // move before previous
  // zobrist hashes of past positions, sorted: a flat vector copies without
  // per-element allocation, which matters because search copies states
  s::vector<uint>        mHistory;
};

}  // namespace rlgames

#endif  // RLGAMES_GO_TYPES
