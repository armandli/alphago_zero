// Bug #2: self-play appended the accumulated buffers after every game, so
// game k of a batch ended up in the training data (batchsize - k + 1) times.

#include <vector>

#include <gtest/gtest.h>

#include <types.h>
#include <go_types.h>
#include <pytorch_util.h>
#include <zero_episodic_exp.h>
#include <go_zero_encoder.h>
#include <self_play.h>

#include "test_helpers.h"

namespace R = rlgames;
namespace T = rlgames::test;
namespace t = torch;

namespace {

// Always passes; records one experience row per move, tagged with the game
// number so duplicates are detectable.
struct StubAgent {
  R::ZeroEpisodicExpCollector* exp  = nullptr;
  int*                         game = nullptr;

  void set_exp(R::ZeroEpisodicExpCollector& e) { exp = &e; }

  R::Move select_move(const R::GoGameState<T::TSZ>& gs) {
    // a fresh game starts with no previous move
    if (gs.previous_move().mty == R::M::Unknown) ++*game;
    R::TensorP st(
      t::full({T::TPLANE, T::TSZ, T::TSZ}, float(*game)),
      t::zeros({2}));
    exp->append(st, std::vector<float>(T::TASZ, 1.F));
    return R::Move(R::M::Pass);
  }
};

}  // namespace

TEST(SelfPlayBatch, EachGameAppearsExactlyOnce) {
  int game = 0;
  StubAgent a1{nullptr, &game}, a2{nullptr, &game};
  R::ZeroGoStateEncoder<T::TSZ> enc;
  constexpr uint batchsize = 4;

  R::SelfPlayBatch batch = R::self_play_batch<T::TSZ>(
    a1, a2, batchsize, 100, enc.state_size(), T::TASZ, t::kCPU);

  // every game is Black pass, White pass: one row per agent per game
  ASSERT_EQ(batch.experience.boards.size(0), 2 * batchsize);
  ASSERT_EQ(batch.experience.states.size(0), 2 * batchsize);
  ASSERT_EQ(batch.experience.visit_counts.size(0), 2 * batchsize);
  ASSERT_EQ(batch.experience.rewards.size(0), 2 * batchsize);

  // each game number shows up exactly twice (once per agent)
  t::Tensor tags = batch.experience.boards.select(1, 0).select(1, 0).select(1, 0);
  for (int g = 1; g <= int(batchsize); ++g)
    EXPECT_EQ(tags.eq(float(g)).sum().item<int>(), 2) << "game " << g;

  EXPECT_EQ(batch.step_counts, std::vector<uint>(batchsize, 2U));
  EXPECT_EQ(batch.game_microseconds.size(), batchsize);
}

TEST(SelfPlayBatch, RewardsMatchWinner) {
  int game = 0;
  StubAgent a1{nullptr, &game}, a2{nullptr, &game};
  R::ZeroGoStateEncoder<T::TSZ> enc;

  R::SelfPlayBatch batch = R::self_play_batch<T::TSZ>(
    a1, a2, 3, 100, enc.state_size(), T::TASZ, t::kCPU);

  // empty board after two passes: white wins on komi
  EXPECT_EQ(batch.a1_wins, 0U);
  EXPECT_EQ(batch.a2_wins, 3U);
  EXPECT_EQ(batch.ties, 0U);
  // first 3 rows are black's (buffer1), last 3 white's (buffer2)
  t::Tensor r = batch.experience.rewards;
  EXPECT_TRUE(r.slice(0, 0, 3).eq(-1.F).all().item<bool>());
  EXPECT_TRUE(r.slice(0, 3, 6).eq(1.F).all().item<bool>());
}
