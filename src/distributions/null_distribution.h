#ifndef RLGAMES_NULL_DISTRIBUTION
#define RLGAMES_NULL_DISTRIBUTION

#include <array>
#include <initializer_list>
#include <algorithm>

#include <type_alias.h>

namespace rlgames {

namespace s = std;

// Placeholder distribution returning all-zero vectors.
template <uint Size, typename RealType = float>
struct null_distribution {
  using ty = RealType;

  struct param_type {
    using distribution_type = null_distribution;

    explicit param_type(s::initializer_list<ty>) {}

    ty param(uint) const { return 0.F; }
    ty operator[](uint) const { return 0.F; }

    bool operator==(const param_type&) const { return true; }
    bool operator!=(const param_type&) const { return false; }
  };

  null_distribution() = default;
  explicit null_distribution(ty) {}
  explicit null_distribution(s::initializer_list<ty>) {}

  template <typename Generator>
  s::array<ty, Size> operator()(Generator&) { return generate(); }

  template <typename Generator>
  s::array<ty, Size> operator()(Generator&, const param_type&) {
    return generate();
  }

  ty min() const { return 0.; }
  ty max() const { return 0.; }

  bool operator==(const null_distribution<Size, ty>&) const { return true; }
  bool operator!=(const null_distribution<Size, ty>&) const { return false; }

private:
  s::array<ty, Size> generate() {
    s::array<ty, Size> ret;
    s::ranges::fill(ret, 0.F);
    return ret;
  }
};

}  // namespace rlgames

#endif  // RLGAMES_NULL_DISTRIBUTION
