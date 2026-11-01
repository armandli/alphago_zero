#ifndef RLGAMES_MUZERO_INFERENCE
#define RLGAMES_MUZERO_INFERENCE

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <torch/torch.h>

#include <transformer_muzero.h>

namespace rlgames::muzero {

namespace s = std;
namespace t = torch;

// Network requests collected from many searches for one batched call.
// Latents never leave the inference backend: requests name them by id, and
// each search owns a disjoint range of ids for its nodes.
//   initial:   observation -> latent stored at latent_id, plus prediction
//   recurrent: (latent parent, action) -> latent stored at child, plus
//              prediction and reward
struct InferenceRequests {
  int                observation_size = 0;
  s::vector<uint8_t> observations;    // num_initial * observation_size
  s::vector<int64_t> initial_latent;
  s::vector<int64_t> parent_latent;
  s::vector<int64_t> actions;
  s::vector<int64_t> child_latent;

  void clear() {
    observations.clear();
    initial_latent.clear();
    parent_latent.clear();
    actions.clear();
    child_latent.clear();
  }
  int num_initial() const { return static_cast<int>(initial_latent.size()); }
  int num_recurrent() const { return static_cast<int>(child_latent.size()); }
  int size() const { return num_initial() + num_recurrent(); }

  // returns the index of the request among the initial ones
  int add_initial(const uint8_t* observation, int64_t latent_id) {
    observations.insert(observations.end(), observation, observation + observation_size);
    initial_latent.push_back(latent_id);
    return num_initial() - 1;
  }
  // returns the index of the request among the recurrent ones
  int add_recurrent(int64_t parent, int action, int64_t child) {
    parent_latent.push_back(parent);
    actions.push_back(action);
    child_latent.push_back(child);
    return num_recurrent() - 1;
  }
};

struct InferenceResults {
  int              num_actions = 0;
  s::vector<float> initial_logits;    // num_initial * A
  s::vector<float> initial_values;    // mover's view, P(win) - P(loss)
  s::vector<float> recurrent_logits;  // num_recurrent * A
  s::vector<float> recurrent_values;
  s::vector<float> recurrent_rewards; // reward of the action for its mover

  const float* initial_policy(int i) const { return initial_logits.data() + static_cast<size_t>(i) * num_actions; }
  const float* recurrent_policy(int i) const { return recurrent_logits.data() + static_cast<size_t>(i) * num_actions; }
};

struct Inference {
  virtual ~Inference() = default;
  virtual void infer(const InferenceRequests& requests, InferenceResults& results) = 0;
  virtual int num_actions() const = 0;
  virtual int observation_size() const = 0;
  // latent ids available: [0, latent_capacity())
  virtual int64_t latent_capacity() const = 0;
};

// The network on the GPU with a latent pool of `capacity` entries. Thread
// safe: calls from several self-play threads are serialized, so one thread's
// network call overlaps the others' search work. The model is an inference
// copy in a reduced precision; set_weights refreshes it from the learner.
struct ModelInference : Inference {
  ModelInference(
      const TransformerMuZeroConfig& config,
      t::Device device,
      t::ScalarType dtype,
      int64_t capacity):
    mModel(config), mDevice(device), mDtype(dtype), mCapacity(capacity)
  {
    mModel->to(device, dtype);
    mModel->eval();
    mPool = t::zeros({capacity, config.num_tokens, config.dims},
                     t::TensorOptions().device(device).dtype(dtype));
  }

  int num_actions() const override { return static_cast<int>(mModel->config.num_actions); }
  int observation_size() const override {
    return static_cast<int>(mModel->config.num_tokens * mModel->config.num_features);
  }
  int64_t latent_capacity() const override { return mCapacity; }

  void set_weights(TransformerMuZero& source, uint32_t version) {
    s::lock_guard<s::mutex> lock(mMutex);
    copy_weights(mModel, source);
    mVersion = version;
  }
  uint32_t version() const { return mVersion.load(); }
  uint64_t num_positions() const { return mPositions.load(); }
  uint64_t num_calls() const { return mCalls.load(); }

  void infer(const InferenceRequests& req, InferenceResults& res) override {
    s::lock_guard<s::mutex> lock(mMutex);
    t::InferenceMode guard;
    const TransformerMuZeroConfig& c = mModel->config;
    int64_t A = c.num_actions;
    res.num_actions = static_cast<int>(A);
    int ni = req.num_initial();
    int nr = req.num_recurrent();
    mPositions += ni + nr;
    ++mCalls;

    res.initial_logits.resize(static_cast<size_t>(ni) * A);
    res.initial_values.resize(ni);
    if (ni > 0) {
      t::Tensor obs = t::from_blob(const_cast<uint8_t*>(req.observations.data()),
        {ni, c.num_tokens, c.num_features}, t::kUInt8).to(mDevice);
      t::Tensor ids = ids_tensor(req.initial_latent);
      t::Tensor latent = mModel->represent(obs);
      mPool.index_copy_(0, ids, latent);
      Prediction p = mModel->predict(latent);
      copy_out(p.policy_logits, res.initial_logits.data());
      copy_out(outcome_scalar(p.value_logits), res.initial_values.data());
    }

    res.recurrent_logits.resize(static_cast<size_t>(nr) * A);
    res.recurrent_values.resize(nr);
    res.recurrent_rewards.resize(nr);
    if (nr > 0) {
      t::Tensor parents = ids_tensor(req.parent_latent);
      t::Tensor children = ids_tensor(req.child_latent);
      t::Tensor actions = t::from_blob(const_cast<int64_t*>(req.actions.data()),
        {nr}, t::kLong).to(mDevice);
      DynamicsOutput d = mModel->dynamics(mPool.index_select(0, parents), actions);
      mPool.index_copy_(0, children, d.latent);
      Prediction p = mModel->predict(d.latent);
      copy_out(p.policy_logits, res.recurrent_logits.data());
      copy_out(outcome_scalar(p.value_logits), res.recurrent_values.data());
      copy_out(outcome_scalar(d.reward_logits), res.recurrent_rewards.data());
    }
  }

private:
  t::Tensor ids_tensor(const s::vector<int64_t>& ids) const {
    for (int64_t id : ids)
      if (id < 0 or id >= mCapacity) throw s::out_of_range("latent id out of range");
    return t::from_blob(const_cast<int64_t*>(ids.data()),
      {static_cast<int64_t>(ids.size())}, t::kLong).to(mDevice);
  }
  static void copy_out(const t::Tensor& src, float* dst) {
    t::Tensor cpu = src.to(t::kCPU, t::kFloat).contiguous();
    s::memcpy(dst, cpu.data_ptr<float>(), sizeof(float) * cpu.numel());
  }

  TransformerMuZero     mModel;
  t::Device             mDevice;
  t::ScalarType         mDtype;
  int64_t               mCapacity;
  t::Tensor             mPool;
  s::mutex              mMutex;
  s::atomic<uint32_t>   mVersion{0};
  s::atomic<uint64_t>   mPositions{0};
  s::atomic<uint64_t>   mCalls{0};
};

}  // namespace rlgames::muzero

#endif  // RLGAMES_MUZERO_INFERENCE
