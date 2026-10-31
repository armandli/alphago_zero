// Bug #5: "pass" / "resign" could not be entered.
// Bug #6: vs_zero_* accepted illegal human moves.
// Bug #7: out-of-range row numbers crashed (uncaught std::out_of_range).
// Minor: index_to_char fell off the end of a non-void function.

#include <optional>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include <types.h>
#include <go_types.h>
#include <input_parser.h>

namespace R = rlgames;
using R::M;
using R::Move;
using R::Pt;

TEST(StringToMove, PassAndResignAnyCase) {
  for (const char* s : {"pass", "PASS", "Pass"}) {
    auto m = R::string_to_move(s, 9);
    ASSERT_TRUE(m) << s;
    EXPECT_EQ(m->mty, M::Pass) << s;
  }
  for (const char* s : {"resign", "RESIGN", "Resign"}) {
    auto m = R::string_to_move(s, 9);
    ASSERT_TRUE(m) << s;
    EXPECT_EQ(m->mty, M::Resign) << s;
  }
}

TEST(StringToMove, Coordinates) {
  auto e5 = R::string_to_move("E5", 9);
  ASSERT_TRUE(e5);
  EXPECT_EQ(*e5, Move(M::Play, Pt(4, 4)));
  EXPECT_EQ(*R::string_to_move("e5", 9), *e5);
  // I is skipped in Go coordinates
  EXPECT_EQ(*R::string_to_move("J1", 9), Move(M::Play, Pt(0, 8)));
  EXPECT_EQ(*R::string_to_move("N13", 13), Move(M::Play, Pt(12, 12)));
}

TEST(StringToMove, RejectsGarbageWithoutThrowing) {
  for (const char* s : {
         "", "E", "I5", "i5", "5E", "E5x", "E 5", "E-1", "A0", "@5",
         "A99999999999", "A99999999999999999999999"})
  {
    std::optional<Move> m;
    EXPECT_NO_THROW(m = R::string_to_move(s, 19)) << s;
    EXPECT_FALSE(m) << s;
  }
}

TEST(StringToMove, RejectsOffBoard) {
  EXPECT_FALSE(R::string_to_move("K1", 9));
  EXPECT_FALSE(R::string_to_move("A10", 9));
  EXPECT_TRUE(R::string_to_move("J9", 9));
}

TEST(StringToMove, StreamExtraction) {
  std::istringstream in("pass");
  Move m(M::Unknown);
  in >> m;
  EXPECT_EQ(m.mty, M::Pass);
}

TEST(IndexToChar, RoundTripsThroughParser) {
  for (ubyte c = 0; c < 19; ++c) {
    std::string s = std::string(1, R::index_to_char(c)) + "1";
    auto m = R::string_to_move(s, 19);
    ASSERT_TRUE(m) << s;
    EXPECT_EQ(m->mpt.c, c) << s;
  }
}

TEST(ParseMove, RepromptsUntilLegal) {
  R::GoGameState<9> gs;
  gs.apply_move(Move(M::Play, Pt(4, 4)));  // black E5
  // white to move: garbage, off-board, occupied, then legal
  std::istringstream in("hello\nZ9\nE5\nD4\n");
  std::ostringstream out;
  auto m = R::parse_move(gs, 9, in, out);
  ASSERT_TRUE(m);
  EXPECT_EQ(*m, Move(M::Play, Pt(3, 3)));
  std::string log = out.str();
  EXPECT_NE(log.find("unable to parse"), std::string::npos);
  EXPECT_NE(log.find("illegal move"), std::string::npos);
}

TEST(ParseMove, AcceptsPassAndResign) {
  R::GoGameState<9> gs;
  std::istringstream in1("pass\n"), in2("resign\n");
  std::ostringstream out;
  EXPECT_EQ(R::parse_move(gs, 9, in1, out)->mty, M::Pass);
  EXPECT_EQ(R::parse_move(gs, 9, in2, out)->mty, M::Resign);
}

TEST(ParseMove, HumanCanEndGameByPassing) {
  R::GoGameState<9> gs;
  gs.apply_move(Move(M::Pass));  // AI (pretend) passes
  std::istringstream in("pass\n");
  std::ostringstream out;
  auto m = R::parse_move(gs, 9, in, out);
  ASSERT_TRUE(m);
  gs.apply_move(*m);
  EXPECT_TRUE(gs.is_over());
}

TEST(ParseMove, ReturnsNulloptAtEndOfInput) {
  R::GoGameState<9> gs;
  std::istringstream in("bogus\n");
  std::ostringstream out;
  EXPECT_FALSE(R::parse_move(gs, 9, in, out));
}
