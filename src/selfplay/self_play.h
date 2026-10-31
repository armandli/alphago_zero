#ifndef RLGAMES_SELF_PLAY
#define RLGAMES_SELF_PLAY

#include <chrono>
#include <utility>
#include <vector>

#include <type_alias.h>
#include <types.h>
#include <go_types.h>
#include <pytorch_util.h>
#include <zero_episodic_exp.h>

#include <torch/torch.h>

namespace rlgames {

namespace s = std;
namespace t = torch;

struct SelfPlayBatch {
  ZeroExperience     experience;
  s::vector<uint>    step_counts;
  s::vector<sint64>  game_microseconds;
  uint64             a1_wins = 0;
  uint64             a2_wins = 0;
  uint64             ties    = 0;

  explicit SelfPlayBatch(t::Device device): experience(device) {}
};

// Play `batchsize` games, agent1 as Black and agent2 as White, and collect
// the experience of every move exactly once.
// Agents need select_move(const GoGameState<SZ>&) and
// set_exp(ZeroEpisodicExpCollector&).
template <ubyte SZ, typename Agent1, typename Agent2>
SelfPlayBatch self_play_batch(
    Agent1& agent1,
    Agent2& agent2,
    uint batchsize,
    uint max_bsize,
    const TensorDimP& state_size,
    uint action_size,
    t::Device device)
{
  namespace c = s::chrono;
  constexpr float WIN_SCORE  =  1.F;
  constexpr float LOSS_SCORE = -1.F;
  constexpr float TIE_SCORE  =  0.F;

  SelfPlayBatch result(device);

  ZeroEpisodicExpCollector buffer1(max_bsize, state_size, action_size, device);
  ZeroEpisodicExpCollector buffer2(max_bsize, state_size, action_size, device);
  agent1.set_exp(buffer1);
  agent2.set_exp(buffer2);

  for (uint j = 0; j < batchsize; ++j) {
    GoGameState<SZ> state;
    Player turn = Player::Black;

    auto gstart = c::high_resolution_clock::now();
    uint step_count = 0;

    while (not state.is_over()) {
      Move move(M::Pass);
      switch (turn) {
      case Player::Black:
        move = agent1.select_move(state);
        break;

      case Player::White:
        move = agent2.select_move(state);
        break;

      default:
        s::unreachable();
      }
      state.apply_move(move);
      step_count++;
      turn = other_player(turn);
    }

    result.step_counts.push_back(step_count);
    switch (state.winner()) {
    case Player::Black:
      buffer1.complete_episode(WIN_SCORE);
      buffer2.complete_episode(LOSS_SCORE);
      result.a1_wins += 1;
      break;

    case Player::White:
      buffer1.complete_episode(LOSS_SCORE);
      buffer2.complete_episode(WIN_SCORE);
      result.a2_wins += 1;
      break;

    case Player::Unknown:
      buffer1.complete_episode(TIE_SCORE);
      buffer2.complete_episode(TIE_SCORE);
      result.ties += 1;
      break;

    default:
      s::unreachable();
    }

    auto gstop = c::high_resolution_clock::now();
    result.game_microseconds.push_back(
      c::duration_cast<c::microseconds>(gstop - gstart).count());
  }

  // the buffers accumulate every game of the batch, so they are appended
  // once, after the last game; appending per game would duplicate the
  // earlier games in the training data
  append_experiences(result.experience, buffer1, buffer2);
  return result;
}

}  // namespace rlgames

#endif  // RLGAMES_SELF_PLAY
