#ifndef RLGAMES_ENCODER_BASE
#define RLGAMES_ENCODER_BASE

#include <vector>

#include <torch/torch.h>

#include <type_alias.h>
#include <pytorch_util.h>

namespace rlgames {

namespace s = std;
namespace t = torch;

template <typename GameState, typename EType, typename Sub>
struct StateEncoderBase {
  EType encode_state(const GameState& gs, t::Device device) const {
    return static_cast<const Sub*>(this)->encode_state(gs, device);
  }
  [[nodiscard]] TensorDimP state_size() const {
    return static_cast<const Sub*>(this)->state_size();
  }
};

template <typename Move, typename Sub>
struct ActionEncoderBase {
  t::Tensor encode_action(const Move& a, t::Device device) const {
    return static_cast<const Sub*>(this)->encode_action(a, device);
  }
  Move decode_action(t::Tensor tensor) const {
    return static_cast<const Sub*>(this)->decode_action(tensor);
  }
  uint move_to_idx(const Move& m) const {
    return static_cast<const Sub*>(this)->move_to_idx(m);
  }
  Move idx_to_move(uint idx) const {
    return static_cast<const Sub*>(this)->idx_to_move(idx);
  }
  [[nodiscard]] static consteval uint action_size() {
    return Sub::action_size();
  }
};

}  // namespace rlgames

#endif  // RLGAMES_ENCODER_BASE
