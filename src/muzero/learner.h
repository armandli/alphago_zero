#ifndef RLGAMES_MUZERO_LEARNER
#define RLGAMES_MUZERO_LEARNER

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <ATen/autocast_mode.h>
#include <torch/torch.h>

#include <experience.h>
#include <transformer_muzero.h>

namespace rlgames::muzero {

namespace s  = std;
namespace t  = torch;
namespace sf = std::filesystem;

// B positions unrolled K steps; step k of a sample is state t+k of its game.
// Masks are 1 where the target exists:
//   policy: t+k is not terminal and had a full search
//   value / ownership: t+k is not terminal
//   reward: the transition t+k -> t+k+1 happened
//   state: t+k exists (reconstruction; consistency for k >= 1)
struct TrainingBatch {
  int B = 0, K = 0, T = 0, F = 0, A = 0;

  s::vector<uint8_t> observations;         // B, T, F       state t
  s::vector<uint8_t> target_observations;  // B, K, T, F    states t+1..t+K
  s::vector<int64_t> actions;              // B, K
  s::vector<float>   policies;             // B, K+1, A
  s::vector<float>   policy_mask;          // B, K+1
  s::vector<int64_t> values;               // B, K+1   outcome class
  s::vector<float>   value_mask;           // B, K+1
  s::vector<int64_t> rewards;              // B, K     outcome class
  s::vector<float>   reward_mask;          // B, K
  s::vector<int64_t> tokens;               // B, K+1, T  token classes of t+k
  s::vector<int64_t> ownership;            // B, K+1, T  final owner seen from t+k
  s::vector<float>   state_mask;           // B, K+1

  void resize(int b, int k, int tk, int f, int a) {
    B = b; K = k; T = tk; F = f; A = a;
    observations.assign(static_cast<size_t>(b) * tk * f, 0);
    target_observations.assign(static_cast<size_t>(b) * k * tk * f, 0);
    actions.assign(static_cast<size_t>(b) * k, 0);
    policies.assign(static_cast<size_t>(b) * (k + 1) * a, 0.F);
    policy_mask.assign(static_cast<size_t>(b) * (k + 1), 0.F);
    values.assign(static_cast<size_t>(b) * (k + 1), 1);
    value_mask.assign(static_cast<size_t>(b) * (k + 1), 0.F);
    rewards.assign(static_cast<size_t>(b) * k, 1);
    reward_mask.assign(static_cast<size_t>(b) * k, 0.F);
    tokens.assign(static_cast<size_t>(b) * (k + 1) * tk, 0);
    ownership.assign(static_cast<size_t>(b) * (k + 1) * tk, 0);
    state_mask.assign(static_cast<size_t>(b) * (k + 1), 0.F);
  }
};

// samples positions from the replay buffer and builds unrolled targets,
// each sample under a random symmetry of the board
template <typename Encoder>
struct BatchBuilder {
  BatchBuilder(const Encoder* encoder, int batch_size, int unroll_steps, bool augment):
    mEncoder(encoder), mBatchSize(batch_size), mUnroll(unroll_steps), mAugment(augment)
  {}

  void build(const ReplayBuffer& replay, s::mt19937_64& rng, TrainingBatch& batch) {
    batch.resize(mBatchSize, mUnroll, mEncoder->num_tokens(), mEncoder->num_features(),
                 mEncoder->num_actions());
    replay.sample(mBatchSize, rng, mRefs);
    s::uniform_int_distribution<int> sym(0, mAugment ? mEncoder->num_symmetries() - 1 : 0);
    for (int b = 0; b < static_cast<int>(mRefs.size()); ++b)
      fill(*mRefs[b].trajectory, mRefs[b].t, sym(rng), b, batch);
  }

  // sample b of an already resized batch: position t of tr unrolled under
  // symmetry sym
  void fill(const Trajectory& tr, int t0, int sym, int b, TrainingBatch& batch) const {
    int T = batch.T, F = batch.F, A = batch.A, K = batch.K;
    const s::vector<int>& tmap = mEncoder->token_map(sym);
    const s::vector<int>& amap = mEncoder->action_map(sym);

    auto copy_obs = [&](int state, uint8_t* dst) {
      const uint8_t* src = tr.observation(state);
      for (int i = 0; i < T; ++i)
        s::copy(src + i * F, src + (i + 1) * F, dst + tmap[i] * F);
    };

    copy_obs(t0, batch.observations.data() + static_cast<size_t>(b) * T * F);
    for (int k = 0; k <= K; ++k) {
      int j = t0 + k;
      size_t bk = static_cast<size_t>(b) * (K + 1) + k;
      if (j <= tr.length) {
        batch.state_mask[bk] = 1.F;
        const uint8_t* tok = tr.token_classes(j);
        for (int i = 0; i < T; ++i) batch.tokens[bk * T + tmap[i]] = tok[i];
        if (k > 0)
          copy_obs(j, batch.target_observations.data()
                      + (static_cast<size_t>(b) * K + k - 1) * T * F);
      }
      if (j < tr.length) {
        batch.value_mask[bk] = 1.F;
        batch.values[bk] = tr.value_target(j) + 1;
        // final owner relative to the side to move at t+k
        bool white = tr.to_play[j] != 0;
        for (int i = 0; i < T; ++i) {
          int owner = tr.final_owner[i];
          batch.ownership[bk * T + tmap[i]] = white ? Encoder::flip_token_class(owner) : owner;
        }
        if (tr.policy_valid[j]) {
          batch.policy_mask[bk] = 1.F;
          const float* p = tr.policies.data() + static_cast<size_t>(j) * A;
          for (int a = 0; a < A; ++a) batch.policies[bk * A + amap[a]] = p[a];
        }
      }
      if (k < K) {
        size_t bk2 = static_cast<size_t>(b) * K + k;
        if (j < tr.length) {
          batch.actions[bk2] = amap[tr.actions[j]];
          batch.rewards[bk2] = tr.rewards[j] + 1;
          batch.reward_mask[bk2] = 1.F;
        } else {
          // past the end of the game: any action, no targets
          batch.actions[bk2] = A - 1;
        }
      }
    }
  }

private:
  const Encoder*         mEncoder;
  int                    mBatchSize;
  int                    mUnroll;
  bool                   mAugment;
  s::vector<PositionRef> mRefs;
};

struct LearnerConfig {
  double learning_rate     = 1e-3;
  double weight_decay      = 0.01;
  int    warmup_steps      = 300;
  int    total_steps       = 15000;   // cosine decay horizon
  double final_lr_fraction = 0.1;
  double grad_clip         = 1.0;
  bool   bf16              = true;    // autocast forward/backward on CUDA
  double dynamics_gradient_scale = 0.5;

  double policy_weight         = 1.0;
  double value_weight          = 1.0;
  double reward_weight         = 1.0;
  double consistency_weight    = 2.0;
  double reconstruction_weight = 0.5;
  double ownership_weight      = 0.5;
};

struct LearnerStats {
  int64_t step = 0;
  double loss = 0., policy = 0., value = 0., reward = 0.;
  double consistency = 0., reconstruction = 0., ownership = 0.;
  double grad_norm = 0., learning_rate = 0.;

  void add(const LearnerStats& o) {
    loss += o.loss; policy += o.policy; value += o.value; reward += o.reward;
    consistency += o.consistency; reconstruction += o.reconstruction;
    ownership += o.ownership; grad_norm += o.grad_norm;
    learning_rate = o.learning_rate;
    step = o.step;
  }
  LearnerStats mean(int n) const {
    LearnerStats r = *this;
    if (n > 0) {
      r.loss /= n; r.policy /= n; r.value /= n; r.reward /= n;
      r.consistency /= n; r.reconstruction /= n; r.ownership /= n; r.grad_norm /= n;
    }
    return r;
  }
};

// MuZero training on K step unrolls:
//   policy: cross entropy to the search policy
//   value, reward: cross entropy over loss/draw/win
//   consistency: EfficientZero's SimSiam loss, negative cosine between the
//     predicted projection of the unrolled latent and the stop-gradient
//     projection of the representation of the real state t+k
//   reconstruction: decode the real board's token classes from the latent
//     (Scholz et al. 2021), which grounds the dynamics in the board
//   ownership: final owner of every point (KataGo), which splits the
//     win/loss signal into per-point credit
// Terms at k >= 1 are scaled by 1/K and the latent's gradient is scaled by
// dynamics_gradient_scale entering each dynamics step, as in MuZero.
// AdamW with linear warmup and cosine decay; bf16 autocast on CUDA.
struct Learner {
  Learner(TransformerMuZero model, const LearnerConfig& config, t::Device device):
    mModel(s::move(model)), mConfig(config), mDevice(device)
  {
    mModel->to(device);
    mModel->train();
    // no weight decay on norms, biases, embeddings
    s::vector<t::Tensor> decay, no_decay;
    for (const auto& item : mModel->named_parameters(true)) {
      const s::string& name = item.key();
      bool matrix = item.value().dim() >= 2
                and name.find("embedding") == s::string::npos;
      (matrix ? decay : no_decay).push_back(item.value());
    }
    s::vector<t::optim::OptimizerParamGroup> groups;
    groups.emplace_back(decay, s::make_unique<t::optim::AdamWOptions>(
      t::optim::AdamWOptions(config.learning_rate).weight_decay(config.weight_decay)
        .betas({0.9, 0.95})));
    groups.emplace_back(no_decay, s::make_unique<t::optim::AdamWOptions>(
      t::optim::AdamWOptions(config.learning_rate).weight_decay(0.0).betas({0.9, 0.95})));
    mOptimizer = s::make_unique<t::optim::AdamW>(groups);
  }

  TransformerMuZero& model() { return mModel; }
  int64_t step() const { return mStep; }

  double learning_rate(int64_t step) const {
    const LearnerConfig& c = mConfig;
    if (step < c.warmup_steps)
      return c.learning_rate * static_cast<double>(step + 1) / c.warmup_steps;
    double progress = s::min(1.0, static_cast<double>(step - c.warmup_steps)
                                  / s::max(1, c.total_steps - c.warmup_steps));
    double cosine = 0.5 * (1.0 + s::cos(M_PI * progress));
    return c.learning_rate * (c.final_lr_fraction + (1.0 - c.final_lr_fraction) * cosine);
  }

  LearnerStats train_step(const TrainingBatch& batch) {
    const LearnerConfig& c = mConfig;
    int B = batch.B, K = batch.K, T = batch.T, F = batch.F, A = batch.A;
    auto u8 = [&](const s::vector<uint8_t>& v, s::vector<int64_t> shape) {
      return t::from_blob(const_cast<uint8_t*>(v.data()), shape, t::kUInt8).to(mDevice);
    };
    auto f32 = [&](const s::vector<float>& v, s::vector<int64_t> shape) {
      return t::from_blob(const_cast<float*>(v.data()), shape, t::kFloat).to(mDevice);
    };
    auto i64 = [&](const s::vector<int64_t>& v, s::vector<int64_t> shape) {
      return t::from_blob(const_cast<int64_t*>(v.data()), shape, t::kLong).to(mDevice);
    };
    t::Tensor obs         = u8(batch.observations, {B, T, F});
    t::Tensor target_obs  = u8(batch.target_observations, {B, K, T, F});
    t::Tensor actions     = i64(batch.actions, {B, K});
    t::Tensor policies    = f32(batch.policies, {B, K + 1, A});
    t::Tensor policy_mask = f32(batch.policy_mask, {B, K + 1});
    t::Tensor values      = i64(batch.values, {B, K + 1});
    t::Tensor value_mask  = f32(batch.value_mask, {B, K + 1});
    t::Tensor rewards     = i64(batch.rewards, {B, K});
    t::Tensor reward_mask = f32(batch.reward_mask, {B, K});
    t::Tensor tokens      = i64(batch.tokens, {B, K + 1, T});
    t::Tensor ownership   = i64(batch.ownership, {B, K + 1, T});
    t::Tensor state_mask  = f32(batch.state_mask, {B, K + 1});

    double lr = learning_rate(mStep);
    for (auto& group : mOptimizer->param_groups())
      static_cast<t::optim::AdamWOptions&>(group.options()).lr(lr);

    bool autocast = c.bf16 and mDevice.is_cuda();
    if (autocast) {
      at::autocast::set_autocast_enabled(at::kCUDA, true);
      at::autocast::set_autocast_dtype(at::kCUDA, at::kBFloat16);
    }

    auto masked_mean = [](const t::Tensor& loss, const t::Tensor& mask) {
      return (loss * mask).sum() / t::clamp_min(mask.sum(), 1.0);
    };
    auto ce_classes = [](const t::Tensor& logits, const t::Tensor& target) {
      // logits (..., C), target (...) -> per-element cross entropy
      t::Tensor logp = t::log_softmax(logits.to(t::kFloat), -1);
      return -logp.gather(-1, target.unsqueeze(-1)).squeeze(-1);
    };

    t::Tensor zero = t::zeros({}, t::TensorOptions().device(mDevice));
    t::Tensor l_policy = zero, l_value = zero, l_reward = zero;
    t::Tensor l_cons = zero, l_recon = zero, l_own = zero;

    // consistency targets for all K future states in one call
    t::Tensor target_proj;
    {
      t::NoGradGuard no_grad;
      target_proj = mModel->project(mModel->represent(target_obs.view({B * K, T, F})))
                      .view({B, K, T, -1});
    }

    t::Tensor latent = mModel->represent(obs);
    for (int k = 0; k <= K; ++k) {
      double scale = (k == 0) ? 1.0 : 1.0 / K;
      if (k > 0) {
        t::Tensor scaled = latent * c.dynamics_gradient_scale
                         + latent.detach() * (1.0 - c.dynamics_gradient_scale);
        DynamicsOutput d = mModel->dynamics(scaled, actions.select(1, k - 1));
        latent = d.latent;
        l_reward = l_reward + scale * masked_mean(
          ce_classes(d.reward_logits, rewards.select(1, k - 1)), reward_mask.select(1, k - 1));

        t::Tensor online = mModel->predict_projection(mModel->project(latent));
        t::Tensor cos = t::cosine_similarity(online.to(t::kFloat).flatten(1),
                                             target_proj.select(1, k - 1).to(t::kFloat).flatten(1), 1);
        l_cons = l_cons + scale * masked_mean(-cos, state_mask.select(1, k));
      }

      Prediction p = mModel->predict(latent);
      t::Tensor logp = t::log_softmax(p.policy_logits.to(t::kFloat), -1);
      t::Tensor pce = -(policies.select(1, k) * logp).sum(-1);
      l_policy = l_policy + scale * masked_mean(pce, policy_mask.select(1, k));
      l_value = l_value + scale * masked_mean(
        ce_classes(p.value_logits, values.select(1, k)), value_mask.select(1, k));
      t::Tensor own = ce_classes(p.ownership_logits, ownership.select(1, k)).mean(-1);
      l_own = l_own + scale * masked_mean(own, value_mask.select(1, k));
      t::Tensor recon = ce_classes(mModel->reconstruct(latent), tokens.select(1, k)).mean(-1);
      l_recon = l_recon + scale * masked_mean(recon, state_mask.select(1, k));
    }

    t::Tensor loss = c.policy_weight * l_policy + c.value_weight * l_value
                   + c.reward_weight * l_reward + c.consistency_weight * l_cons
                   + c.reconstruction_weight * l_recon + c.ownership_weight * l_own;
    if (autocast) {
      at::autocast::set_autocast_enabled(at::kCUDA, false);
      at::autocast::clear_cache();
    }

    mOptimizer->zero_grad();
    loss.backward();
    double grad_norm = t::nn::utils::clip_grad_norm_(mModel->parameters(), c.grad_clip);
    mOptimizer->step();
    ++mStep;

    LearnerStats st;
    st.step = mStep;
    st.loss = loss.item<double>();
    st.policy = l_policy.item<double>();
    st.value = l_value.item<double>();
    st.reward = l_reward.item<double>();
    st.consistency = l_cons.item<double>();
    st.reconstruction = l_recon.item<double>();
    st.ownership = l_own.item<double>();
    st.grad_norm = grad_norm;
    st.learning_rate = lr;
    return st;
  }

  // model.pt, optimizer.pt and step.txt in dir
  void save_checkpoint(const s::string& dir) {
    sf::create_directories(dir);
    save_model(mModel, (sf::path(dir) / "model.pt").string());
    t::save(*mOptimizer, (sf::path(dir) / "optimizer.pt").string());
    s::ofstream(sf::path(dir) / "step.txt") << mStep << "\n";
  }

  // false when dir holds no checkpoint
  bool load_checkpoint(const s::string& dir) {
    sf::path d(dir);
    if (not sf::exists(d / "model.pt") or not sf::exists(d / "step.txt")) return false;
    TransformerMuZero loaded = load_model((d / "model.pt").string(), mDevice);
    copy_weights(mModel, loaded);
    if (sf::exists(d / "optimizer.pt")) t::load(*mOptimizer, (d / "optimizer.pt").string());
    s::ifstream(d / "step.txt") >> mStep;
    return true;
  }

private:
  TransformerMuZero               mModel;
  LearnerConfig                   mConfig;
  t::Device                       mDevice;
  s::unique_ptr<t::optim::AdamW>  mOptimizer;
  int64_t                         mStep = 0;
};

}  // namespace rlgames::muzero

#endif  // RLGAMES_MUZERO_LEARNER
