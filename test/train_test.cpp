// Bug #11: policy loss computed log(softmax) directly; a probability that
//          underflows to exactly 0 gave 0 * -inf = NaN.
// Also:    train() must put the model in training mode (search uses eval),
//          and a batch of one must line up with its targets.

#include <cmath>
#include <limits>

#include <gtest/gtest.h>

#include <pytorch_util.h>
#include <go_action_encoder.h>
#include <go_zero_encoder.h>
#include <zero_episodic_exp.h>
#include <model_base.h>

#include "test_helpers.h"

namespace R = rlgames;
namespace T = rlgames::test;
namespace t = torch;

namespace {

using Container = R::ModelContainer<
  T::TestModel,
  R::ZeroGoStateEncoder<T::TSZ>,
  R::ZeroGoActionEncoder<T::TSZ>,
  t::optim::Adam>;

Container make_container() {
  return Container(
    T::TestModel(),
    R::ZeroGoStateEncoder<T::TSZ>(),
    R::ZeroGoActionEncoder<T::TSZ>(),
    1E-3F);
}

R::ZeroExperience experience(int n, t::Tensor visit_counts) {
  return R::ZeroExperience(
    t::randn({n, T::TPLANE, T::TSZ, T::TSZ}),
    t::zeros({n, 2}),
    visit_counts,
    t::ones({n}));
}

bool all_parameters_finite(Container& c) {
  for (auto& p : c.model->parameters())
    if (not t::isfinite(p).all().item<bool>()) return false;
  return true;
}

}  // namespace

TEST(Train, ZeroProbabilityMovesDoNotProduceNaN) {
  Container c = make_container();
  // moves 5..9 get probability exactly 0 and were never visited
  c.model->policy_bias.slice(0, 5, 10).fill_(
    -std::numeric_limits<float>::infinity());
  t::Tensor vc = t::rand({4, T::TASZ}) + 0.1F;
  vc.slice(1, 5, 10).zero_();

  R::ZeroExperience exp = experience(4, vc);
  float loss = R::train(c, exp);
  EXPECT_TRUE(std::isfinite(loss)) << loss;
  EXPECT_TRUE(all_parameters_finite(c));
}

TEST(Train, BatchOfOne) {
  Container c = make_container();
  float loss = 0.F;
  R::ZeroExperience exp = experience(1, t::rand({1, T::TASZ}) + 0.1F);
  ASSERT_NO_THROW(loss = R::train(c, exp));
  EXPECT_TRUE(std::isfinite(loss));
  EXPECT_TRUE(all_parameters_finite(c));
}

TEST(Train, SwitchesModelToTrainingMode) {
  Container c = make_container();
  c.model->eval();
  R::ZeroExperience exp = experience(4, t::rand({4, T::TASZ}) + 0.1F);
  (void)R::train(c, exp);
  EXPECT_TRUE(c.model->is_training());
  EXPECT_TRUE(c.model->last_training);
}
