#ifndef RLGAMES_TEST_HELPERS
#define RLGAMES_TEST_HELPERS

#include <array>
#include <optional>
#include <utility>
#include <vector>

#include <type_alias.h>
#include <types.h>
#include <go_types.h>
#include <pytorch_util.h>

#include <torch/torch.h>
#include <gtest/gtest.h>

namespace rlgames::test {

namespace s = std;
namespace t = torch;

constexpr ubyte TSZ    = 9;
constexpr uint  TPLANE = 9;                // ZeroGoStateEncoder board planes
constexpr uint  TASZ   = TSZ * TSZ + 1;    // ZeroGoActionEncoder action size

// Small controllable model with the same interface as the real models.
// - BatchNorm so train/eval mode is observable
// - policy_bias lets a test force exact-zero priors (via -inf)
// - fixed_value lets a test force the value head output
// - records the grad mode and training flag seen by the last forward()
struct TestModelImpl : t::nn::Module {
  TestModelImpl():
    bn(register_module("bn", t::nn::BatchNorm2d(TPLANE))),
    pl(register_module("pl", t::nn::Linear(TPLANE * TSZ * TSZ, TASZ))),
    vl(register_module("vl", t::nn::Linear(TPLANE * TSZ * TSZ, 1))),
    policy_bias(register_buffer("policy_bias", t::zeros({TASZ})))
  {}

  TensorP forward(TensorP state) {
    forward_calls++;
    last_grad_enabled = t::GradMode::is_enabled();
    last_training     = is_training();

    t::Tensor x = state.x;
    if (x.dim() == 3) x = x.unsqueeze(0);
    x = bn(x).flatten(1);
    t::Tensor q = t::softmax(pl(x) + policy_bias, -1);
    t::Tensor v = t::tanh(vl(x));
    if (fixed_value) v = t::full_like(v, *fixed_value);
    return TensorP(q.squeeze(), v.squeeze());
  }

  // zero all weights so the policy is uniform (before policy_bias)
  void zero_weights() {
    t::NoGradGuard g;
    for (auto& p : parameters()) p.zero_();
  }

  t::nn::BatchNorm2d   bn;
  t::nn::Linear        pl, vl;
  t::Tensor            policy_bias;
  s::optional<float>   fixed_value;
  long                 forward_calls     = 0;
  bool                 last_grad_enabled = true;
  bool                 last_training     = true;
};
TORCH_MODULE(TestModel);

// Noise distribution that counts how often it is sampled.
template <uint Size, typename RealType = float>
struct CountingNoise {
  explicit CountingNoise(RealType) {}

  template <typename G>
  s::array<RealType, Size> operator()(G&) {
    ++calls;
    return s::array<RealType, Size>{};
  }

  static inline long calls = 0;
};

// Build a game state by playing moves alternately starting with Black.
template <ubyte SZ>
GoGameState<SZ> play(const s::vector<Move>& moves) {
  GoGameState<SZ> gs;
  for (const Move& m : moves) {
    EXPECT_TRUE(gs.is_valid_move(m));
    gs.apply_move(m);
  }
  return gs;
}

inline Move P(int r, int c) { return Move(M::Play, Pt(r, c)); }
inline Move Pass() { return Move(M::Pass); }

}  // namespace rlgames::test

#endif  // RLGAMES_TEST_HELPERS
