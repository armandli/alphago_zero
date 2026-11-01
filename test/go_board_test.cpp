// GoBoard with stable string identities, checked against a naive reference
// board that recomputes groups and liberties by flood fill.

#include <array>
#include <random>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include <types.h>
#include <go_types.h>

namespace R = rlgames;
using R::Player;
using R::Pt;

namespace {

// Simple, obviously-correct board: colors only, everything by flood fill.
template <ubyte SZ>
struct RefBoard {
  static constexpr int IZ = SZ * SZ;
  std::array<Player, IZ> color;

  RefBoard() { color.fill(Player::Unknown); }

  static std::vector<int> adj(int i) {
    std::vector<int> out;
    int r = i / SZ, c = i % SZ;
    if (r > 0)      out.push_back(i - SZ);
    if (c > 0)      out.push_back(i - 1);
    if (r < SZ - 1) out.push_back(i + SZ);
    if (c < SZ - 1) out.push_back(i + 1);
    return out;
  }

  // stones of the group at i, and its liberty count
  std::pair<std::vector<int>, int> group(int i) const {
    std::vector<bool> seen(IZ, false), lib(IZ, false);
    std::vector<int> stack{i}, stones;
    seen[i] = true;
    int libs = 0;
    while (not stack.empty()) {
      int x = stack.back(); stack.pop_back();
      stones.push_back(x);
      for (int n : adj(x)) {
        if (color[n] == Player::Unknown) {
          if (not lib[n]) { lib[n] = true; ++libs; }
        } else if (color[n] == color[i] and not seen[n]) {
          seen[n] = true;
          stack.push_back(n);
        }
      }
    }
    return {stones, libs};
  }

  // same semantics as GoBoard::place_stone: captures opponents, keeps a
  // self-capturing stone on the board
  void place(Player p, int i) {
    color[i] = p;
    for (int n : adj(i))
      if (color[n] != Player::Unknown and color[n] != p) {
        auto [stones, libs] = group(n);
        if (libs == 0)
          for (int s : stones) color[s] = Player::Unknown;
      }
  }

  unsigned hash() const {
    unsigned h = R::EMPTY_BOARD;
    for (int i = 0; i < IZ; ++i)
      if (color[i] != Player::Unknown)
        h ^= R::zobrist_hash<SZ>(color[i], R::point<SZ>(i));
    return h;
  }
};

template <ubyte SZ>
void expect_same(const R::GoBoard<SZ>& b, const RefBoard<SZ>& ref) {
  ASSERT_EQ(b.hash(), ref.hash());
  for (int i = 0; i < RefBoard<SZ>::IZ; ++i) {
    Pt pt = R::point<SZ>(i);
    ASSERT_EQ(b.get(pt), ref.color[i]) << "point " << i;
    const R::GoStr<SZ>* str = b.get_string(pt);
    if (ref.color[i] == Player::Unknown) {
      ASSERT_EQ(str, nullptr);
      continue;
    }
    ASSERT_NE(str, nullptr);
    auto [stones, libs] = ref.group(i);
    ASSERT_EQ(str->num_liberties(), size_t(libs)) << "point " << i;
    ASSERT_EQ(str->size(), stones.size()) << "point " << i;
    ASSERT_EQ(str->color(), ref.color[i]);
    // stones of one group share one string; others don't
    for (int s : stones) ASSERT_EQ(b.get_string(R::point<SZ>(s)), str);
  }
}

// random stone placements of both colors (not alternating, self-capture
// included), checking the board and the copy-free queries after every one
template <ubyte SZ>
void random_differential(unsigned seed, int games) {
  std::mt19937 rng(seed);
  for (int g = 0; g < games; ++g) {
    R::GoBoard<SZ> board;
    RefBoard<SZ> ref;
    for (int step = 0; step < 3 * SZ * SZ; ++step) {
      std::vector<int> empty;
      for (int i = 0; i < RefBoard<SZ>::IZ; ++i)
        if (ref.color[i] == Player::Unknown) empty.push_back(i);
      if (empty.empty()) break;
      Player p = (rng() & 1) ? Player::Black : Player::White;

      // queries for every empty point agree with actually placing
      for (int i : empty) {
        RefBoard<SZ> after = ref;
        after.place(p, i);
        ASSERT_EQ(board.hash_after(p, R::point<SZ>(i)), after.hash());
        ASSERT_EQ(board.is_self_capture(p, R::point<SZ>(i)),
                  after.group(i).second == 0)
          << "seed " << seed << " game " << g << " step " << step;
      }

      int i = empty[std::uniform_int_distribution<size_t>(0, empty.size() - 1)(rng)];
      board.place_stone(p, R::point<SZ>(i));
      ref.place(p, i);
      expect_same(board, ref);
      if (::testing::Test::HasFatalFailure()) return;
      // a self-capturing stone is left on the board; remove it from both by
      // rebuilding, so later moves start from a legal position
      if (ref.group(i).second == 0) {
        R::GoBoard<SZ> rebuilt;
        RefBoard<SZ> legal;
        for (int k = 0; k < RefBoard<SZ>::IZ; ++k)
          if (ref.color[k] != Player::Unknown and ref.group(k).second > 0) {
            rebuilt.place_stone(ref.color[k], R::point<SZ>(k));
            legal.place(ref.color[k], k);
          }
        board = rebuilt;
        ref = legal;
        expect_same(board, ref);
      }
    }
  }
}

}  // namespace

TEST(GoBoard, MatchesReferenceOnRandomPlacements9) { random_differential<9>(1, 40); }
TEST(GoBoard, MatchesReferenceOnRandomPlacements13) { random_differential<13>(2, 10); }
TEST(GoBoard, MatchesReferenceOnRandomPlacements5) { random_differential<5>(3, 300); }

TEST(GoBoard, CopyIsPlainMemoryCopy) {
  static_assert(std::is_trivially_copyable_v<R::GoBoard<9>>);
  static_assert(std::is_trivially_copyable_v<R::GoBoard<13>>);
  static_assert(std::is_trivially_copyable_v<R::GoBoard<19>>);

  R::GoBoard<9> a;
  a.place_stone(Player::Black, Pt(4, 4));
  R::GoBoard<9> b = a;
  b.place_stone(Player::White, Pt(4, 5));
  EXPECT_EQ(a.get(Pt(4, 5)), Player::Unknown);
  EXPECT_EQ(b.get(Pt(4, 5)), Player::White);
  EXPECT_EQ(a.get_string(Pt(4, 4))->num_liberties(), 4U);
  EXPECT_EQ(b.get_string(Pt(4, 4))->num_liberties(), 3U);
}

TEST(GoBoard, StringIdentityIsStableWhileOthersChange) {
  // the bag-based board moved strings around on every removal; here an
  // untouched string keeps its slot while others merge and get captured
  R::GoBoard<9> b;
  b.place_stone(Player::Black, Pt(8, 8));       // the string we watch
  const R::GoStr<9>* watched = b.get_string(Pt(8, 8));

  b.place_stone(Player::White, Pt(0, 0));       // white A1
  b.place_stone(Player::Black, Pt(0, 1));
  b.place_stone(Player::Black, Pt(2, 2));
  b.place_stone(Player::Black, Pt(2, 3));       // merge
  b.place_stone(Player::Black, Pt(1, 0));       // captures white A1
  ASSERT_EQ(b.get(Pt(0, 0)), Player::Unknown);

  EXPECT_EQ(b.get_string(Pt(8, 8)), watched);
  EXPECT_EQ(watched->color(), Player::Black);
  EXPECT_EQ(watched->num_liberties(), 2U);
  EXPECT_EQ(watched->size(), 1U);
}

TEST(GoBoard, MergeKeepsLargerStringRoot) {
  R::GoBoard<9> b;
  for (int c = 0; c < 4; ++c) b.place_stone(Player::Black, Pt(4, c));  // size 4
  const R::GoStr<9>* big = b.get_string(Pt(4, 0));
  b.place_stone(Player::Black, Pt(4, 6));                              // size 1
  b.place_stone(Player::Black, Pt(4, 5));                              // joins it
  b.place_stone(Player::Black, Pt(4, 4));                              // joins all
  EXPECT_EQ(b.get_string(Pt(4, 6)), big);
  EXPECT_EQ(big->size(), 7U);
  // A5..G5 from the edge: 7 liberties above, 7 below, 1 at H5
  EXPECT_EQ(big->num_liberties(), 15U);
}

TEST(GoBoard, CaptureRestoresLibertiesAndHash) {
  R::GoBoard<9> b;
  unsigned empty_hash = b.hash();
  b.place_stone(Player::White, Pt(0, 0));
  b.place_stone(Player::Black, Pt(0, 1));
  unsigned before_capture = b.hash();
  EXPECT_EQ(b.hash_after(Player::Black, Pt(1, 0)),
            empty_hash ^ R::zobrist_hash<9>(Player::Black, Pt(0, 1))
                       ^ R::zobrist_hash<9>(Player::Black, Pt(1, 0)));
  b.place_stone(Player::Black, Pt(1, 0));
  EXPECT_EQ(b.get(Pt(0, 0)), Player::Unknown);
  EXPECT_NE(b.hash(), before_capture);
  // both black stones regained the A1 liberty
  EXPECT_EQ(b.get_string(Pt(0, 1))->num_liberties(), 3U);
  EXPECT_EQ(b.get_string(Pt(1, 0))->num_liberties(), 3U);
}

TEST(GoGameState, LegalMovesUnchangedAroundKo) {
  // classic ko: black captures, white may not recapture immediately
  R::GoGameState<9> gs;
  using M = R::M;
  for (auto m : std::vector<R::Move>{
         R::Move(M::Play, Pt(4, 3)), R::Move(M::Play, Pt(4, 4)),
         R::Move(M::Play, Pt(3, 4)), R::Move(M::Play, Pt(3, 5)),
         R::Move(M::Play, Pt(5, 4)), R::Move(M::Play, Pt(5, 5)),
         R::Move(M::Pass),           R::Move(M::Play, Pt(4, 6)),
         R::Move(M::Play, Pt(4, 5))})  // black captures white E5
    gs.apply_move(m);
  ASSERT_EQ(gs.board().get(Pt(4, 4)), Player::Unknown);
  R::Move recapture(M::Play, Pt(4, 4));
  EXPECT_TRUE(gs.does_move_violate_ko(recapture));
  EXPECT_FALSE(gs.is_valid_move(recapture));
}
