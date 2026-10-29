#ifndef RLGAMES_GO_ONEPLANE
#define RLGAMES_GO_ONEPLANE

#include <cassert>
#include <utility>

#include <torch/torch.h>

#include <type_alias.h>
#include <types.h>
#include <game_base.h>
#include <go_types.h>
#include <encoder_base.h>

namespace rlgames {

namespace s = std;
namespace t = torch;

// Encodes the board as a single SZ-by-SZ input plane.
template <ubyte SZ>
struct OnePlaneGoStateEncoder :
    public StateEncoderBase<
      GoGameState<SZ>, t::Tensor, OnePlaneGoStateEncoder<SZ>>
{
  t::Tensor encode_state(
      const GoGameState<SZ>& gs,
      t::Device device) const
  {
    const GoBoard<SZ>& board = gs.board();
    for (uint i = 0; i < SZ; ++i)
      for (uint j = 0; j < SZ; ++j) {
        Pt pt(i, j);
        switch (board.get(pt)) {
        case Player::Black:
        case Player::White:
          if (board.get(Pt(i, j)) == gs.next_player())
            mState[index<SZ>(pt)] = 1.f;
          else
            mState[index<SZ>(pt)] = -1.f;
          break;

        case Player::Unknown:
          mState[index<SZ>(pt)] = 0.f;
          break;

        default:
          s::unreachable();
        }
      }
    t::Tensor m = t::from_blob(mState, {(sint)SZ, (sint)SZ});
    if (device.type() == t::kCUDA)
      return m.to(device);
    else
      return m.clone();
  }
  [[nodiscard]] TensorDimP state_size() const {
    return TensorDimP(TensorDim(SZ, SZ), TensorDim(0));
  }

private:
  mutable float mState[SZ * SZ];

  constexpr uint plane_size() const { return SZ * SZ; }
};

}  // namespace rlgames

#endif  // RLGAMES_GO_ONEPLANE
