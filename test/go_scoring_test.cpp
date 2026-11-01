// Bug #1: territory flood fill marked whole regions as dame.
// Minor: board printout row labels misaligned for SZ > 9.

#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <types.h>
#include <go_types.h>

namespace R = rlgames;
using R::Player;
using R::Pt;

namespace {

template <ubyte SZ>
void fill_column(R::GoBoard<SZ>& b, Player p, int col) {
  for (int r = 0; r < SZ; ++r) b.place_stone(p, Pt(r, col));
}

}  // namespace

TEST(GoAreaScore, LargeTerritoryBorderedByOneColorIsTerritory) {
  // black wall on column 4 splits the board into two 4x9 regions,
  // both bordered only by black: 9 stones + 72 territory
  R::GoBoard<9> b;
  fill_column(b, Player::Black, 4);
  R::GoAreaScore<9> score(b, 0.F);
  EXPECT_FLOAT_EQ(score.winning_margin(), 81.F);
  EXPECT_EQ(score.winner(), Player::Black);
}

TEST(GoAreaScore, LargeWhiteTerritory) {
  R::GoBoard<13> b;
  fill_column(b, Player::White, 6);
  R::GoAreaScore<13> score(b, 0.F);
  EXPECT_FLOAT_EQ(score.winning_margin(), -169.F);
  EXPECT_EQ(score.winner(), Player::White);
}

TEST(GoAreaScore, SmallEnclosedCorner) {
  R::GoBoard<9> b;
  for (auto [r, c] : std::vector<std::pair<int, int>>{
         {0, 2}, {1, 2}, {2, 0}, {2, 1}, {2, 2}})
    b.place_stone(Player::Black, Pt(r, c));
  for (int r = 3; r < 9; ++r)
    for (int c = 0; c < 9; ++c) b.place_stone(Player::White, Pt(r, c));
  for (int r = 0; r < 3; ++r)
    for (int c = 3; c < 9; ++c) b.place_stone(Player::White, Pt(r, c));
  R::GoAreaScore<9> score(b, 0.F);
  // black: 5 stones + 4 territory; white: 72 stones
  EXPECT_FLOAT_EQ(score.winning_margin(), 9.F - 72.F);
}

TEST(GoAreaScore, RegionBorderedByBothColorsIsDame) {
  // black wall col 3, white wall col 5: column 4 touches both -> dame
  R::GoBoard<9> b;
  fill_column(b, Player::Black, 3);
  fill_column(b, Player::White, 5);
  R::GoAreaScore<9> score(b, 0.F);
  // black 9 + 27, white 9 + 27, column 4 is dame
  EXPECT_FLOAT_EQ(score.winning_margin(), 0.F);
}

TEST(GoAreaScore, EmptyBoardIsAllDameAndWhiteWinsOnKomi) {
  R::GoBoard<9> b;
  R::GoAreaScore<9> score(b, 1.5F);
  EXPECT_FLOAT_EQ(score.winning_margin(), -1.5F);
  EXPECT_EQ(score.winner(), Player::White);
}

TEST(GoGameState, WinnerAfterTwoPassesUsesCorrectTerritory) {
  // black builds a wall while white passes; then both pass
  R::GoGameState<9> gs;
  for (int r = 0; r < 9; ++r) {
    gs.apply_move(R::Move(R::M::Play, Pt(r, 4)));
    gs.apply_move(R::Move(R::M::Pass));
  }
  gs.apply_move(R::Move(R::M::Pass));
  ASSERT_TRUE(gs.is_over());
  EXPECT_EQ(gs.winner(), Player::Black);
}

namespace {

template <ubyte SZ>
void expect_board_aligned() {
  R::GoBoard<SZ> b;
  std::ostringstream ss;
  b.print(ss);
  std::vector<std::string> lines;
  std::istringstream iss(ss.str());
  for (std::string l; std::getline(iss, l);) lines.push_back(l);
  ASSERT_EQ(lines.size(), SZ + 2U);

  size_t col_a = lines.front().find('A');
  ASSERT_NE(col_a, std::string::npos);
  EXPECT_EQ(lines.back().find('A'), col_a);
  for (size_t i = 1; i <= SZ; ++i) {
    const std::string& row = lines[i];
    ASSERT_GT(row.size(), col_a) << row;
    // first intersection sits under column A, preceded by a space
    EXPECT_EQ(row[col_a], '.') << "row: '" << row << "'";
    EXPECT_EQ(row[col_a - 1], ' ') << "row: '" << row << "'";
    // label is the row number, right-aligned before the board
    EXPECT_EQ(std::stoi(row.substr(0, col_a)), int(SZ - i + 1));
  }
}

}  // namespace

TEST(GoBoardPrint, RowsAlignedWithHeader9) { expect_board_aligned<9>(); }
TEST(GoBoardPrint, RowsAlignedWithHeader13) { expect_board_aligned<13>(); }

// transposition identity used by the graph search

TEST(GoGameStatePosition, MoveOrderDoesNotMatter) {
  R::GoGameState<9> a, b;
  for (auto [r, c] : std::vector<std::pair<int, int>>{{2, 2}, {6, 6}, {2, 6}})
    a.apply_move(R::Move(R::M::Play, Pt(r, c)));
  for (auto [r, c] : std::vector<std::pair<int, int>>{{2, 6}, {6, 6}, {2, 2}})
    b.apply_move(R::Move(R::M::Play, Pt(r, c)));
  EXPECT_TRUE(a.same_position(b));
  EXPECT_EQ(a.position_key(), b.position_key());
}

TEST(GoGameStatePosition, PreviousPassDoesNotMatter) {
  // same stones (B E5, W A1), white to move in both
  R::GoGameState<9> after_pass;   // B E5, W A1, B pass
  for (auto m : {R::Move(R::M::Play, Pt(4, 4)), R::Move(R::M::Play, Pt(0, 0)),
                 R::Move(R::M::Pass)})
    after_pass.apply_move(m);
  R::GoGameState<9> after_play;   // B pass, W A1, B E5
  for (auto m : {R::Move(R::M::Pass), R::Move(R::M::Play, Pt(0, 0)),
                 R::Move(R::M::Play, Pt(4, 4))})
    after_play.apply_move(m);
  ASSERT_EQ(after_pass.next_player(), after_play.next_player());
  ASSERT_EQ(after_pass.previous_move().mty, R::M::Pass);
  ASSERT_EQ(after_play.previous_move().mty, R::M::Play);
  EXPECT_TRUE(after_pass.same_position(after_play));
  EXPECT_EQ(after_pass.position_key(), after_play.position_key());
}

TEST(GoGameStatePosition, DifferentPlayerToMoveSameStones) {
  // B E5, W A1 with black to move, vs. the same stones with white to move
  R::GoGameState<9> black_to_move;   // B E5, W A1
  for (auto m : {R::Move(R::M::Play, Pt(4, 4)), R::Move(R::M::Play, Pt(0, 0))})
    black_to_move.apply_move(m);
  R::GoGameState<9> white_to_move;   // B pass, W A1, B E5
  for (auto m : {R::Move(R::M::Pass), R::Move(R::M::Play, Pt(0, 0)),
                 R::Move(R::M::Play, Pt(4, 4))})
    white_to_move.apply_move(m);
  ASSERT_TRUE(black_to_move.board().same_stones(white_to_move.board()));
  ASSERT_NE(black_to_move.next_player(), white_to_move.next_player());
  EXPECT_FALSE(black_to_move.same_position(white_to_move));
  EXPECT_NE(black_to_move.position_key(), white_to_move.position_key());
}

TEST(GoGameStatePosition, CapturedStonesCompareByContentNotHistory) {
  // reach the same final stones once with a capture and once without
  R::GoGameState<9> a;
  for (auto m : std::vector<R::Move>{
         R::Move(R::M::Play, Pt(0, 1)), R::Move(R::M::Play, Pt(0, 0)),
         R::Move(R::M::Play, Pt(1, 0))})  // black captures white A1
    a.apply_move(m);
  R::GoGameState<9> b;
  for (auto m : std::vector<R::Move>{
         R::Move(R::M::Play, Pt(1, 0)), R::Move(R::M::Pass),
         R::Move(R::M::Play, Pt(0, 1))})
    b.apply_move(m);
  ASSERT_EQ(a.board().get(Pt(0, 0)), Player::Unknown);
  EXPECT_TRUE(a.same_position(b));
  EXPECT_EQ(a.position_key(), b.position_key());
}
