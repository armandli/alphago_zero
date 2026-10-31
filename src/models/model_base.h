#ifndef RLGAMES_MODEL_BASE
#define RLGAMES_MODEL_BASE

#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <fstream>
#include <numeric>
#include <algorithm>

#include <pytorch_util.h>
#include <go_zero_encoder.h>
#include <zero_episodic_exp.h>

#include <json_io.h>

#include <torch/torch.h>

namespace rlgames {

namespace s = std;
namespace t = torch;

template <typename NNModel, typename SE, typename AE, typename Optim>
struct ModelContainer {
  NNModel model;
  SE      state_encoder;
  AE      action_encoder;
  Optim   optimizer;

  ModelContainer(
      NNModel m,
      SE&& se,
      AE&& ae,
      float learning_rate,
      float weight_decay = 0.F):
    model(m),
    state_encoder(s::move(se)),
    action_encoder(s::move(ae)),
    optimizer(
      m->parameters(),
      t::optim::AdamOptions(learning_rate).weight_decay(weight_decay))
  {}
};

template <typename Model>
void save_model(
    Model& model,
    const s::string& model_file,
    const s::string& optimizer_file)
{
  t::save(model.model, model_file);
  t::save(model.optimizer, optimizer_file);
}

template <typename Model>
void load_model(
    Model& model,
    const s::string& model_file,
    const s::string& optimizer_file,
    t::Device device)
{
  t::load(model.model, model_file, device);
  t::load(model.optimizer, optimizer_file, device);
}

template <typename Model>
[[nodiscard]] float train(Model& model, ZeroExperience& exp) {
  t::Tensor visit_sums = t::sum(exp.visit_counts, -1)
    .reshape({exp.visit_counts.size(0), 1});
  t::Tensor visit_counts = t::div(exp.visit_counts, visit_sums);

  // the agent switches the model to eval mode for search
  model.model->train();
  model.model->zero_grad();

  TensorP avout = model.model->forward(TensorP(exp.boards, exp.states));
  // forward() squeezes its outputs; restore the batch dimension so a batch
  // of one lines up with the targets
  t::Tensor policy = avout.x.reshape_as(visit_counts);
  t::Tensor value  = avout.y.reshape_as(exp.rewards);
  // clamp so a probability that underflows to 0 can't produce 0 * -inf = NaN
  constexpr float MIN_PROB = 1E-8F;
  t::Tensor policy_loss = t::mean(
    -1.F * visit_counts.detach() * t::log(t::clamp_min(policy, MIN_PROB)));
  t::Tensor value_loss = t::mse_loss(value, exp.rewards.detach());
  t::Tensor loss = policy_loss + value_loss;

  loss.backward();
  model.optimizer.step();

  return loss.item().to<float>();
}

}  // namespace rlgames

#endif  // RLGAMES_MODEL_BASE
