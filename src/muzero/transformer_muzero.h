#ifndef RLGAMES_MUZERO_TRANSFORMER_MUZERO
#define RLGAMES_MUZERO_TRANSFORMER_MUZERO

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <torch/torch.h>

namespace rlgames::muzero {

namespace s = std;
namespace t = torch;

// value and reward heads predict 3 outcome classes from the mover's point of
// view (loss, draw, win, i.e. -1, 0, +1); the scalar is P(win) - P(loss)
constexpr int NUM_OUTCOME_CLASSES = 3;

inline t::Tensor outcome_scalar(const t::Tensor& logits) {
  t::Tensor p = t::softmax(logits.to(t::kFloat), -1);
  return p.select(-1, 2) - p.select(-1, 0);
}

struct TransformerMuZeroConfig {
  // from the encoder
  int64_t num_tokens        = 81;
  int64_t num_features      = 17;
  int64_t num_actions       = 82;
  int64_t num_token_classes = 3;

  int64_t dims        = 128;
  int64_t num_heads   = 4;
  int64_t repr_layers = 4;
  int64_t dyn_layers  = 3;
  int64_t pred_layers = 1;
  int64_t proj_dims   = 128;

  s::vector<int64_t> to_vector() const {
    return {num_tokens, num_features, num_actions, num_token_classes,
            dims, num_heads, repr_layers, dyn_layers, pred_layers, proj_dims};
  }
  static TransformerMuZeroConfig from_vector(const s::vector<int64_t>& v) {
    if (v.size() != 10) throw s::invalid_argument("bad TransformerMuZeroConfig");
    TransformerMuZeroConfig c;
    c.num_tokens = v[0]; c.num_features = v[1]; c.num_actions = v[2];
    c.num_token_classes = v[3]; c.dims = v[4]; c.num_heads = v[5];
    c.repr_layers = v[6]; c.dyn_layers = v[7]; c.pred_layers = v[8];
    c.proj_dims = v[9];
    return c;
  }
};

struct RMSNormImpl : t::nn::Module {
  explicit RMSNormImpl(int64_t dims, double eps = 1e-6): eps(eps) {
    weight = register_parameter("weight", t::ones({dims}));
  }
  t::Tensor forward(const t::Tensor& x) {
    t::Tensor xf = x.to(t::kFloat);
    t::Tensor y = xf * t::rsqrt(xf.pow(2).mean(-1, true) + eps);
    return (y * weight).to(x.scalar_type());
  }
  t::Tensor weight;
  double    eps;
};
TORCH_MODULE(RMSNorm);

// bidirectional multi-head self-attention with PyTorch's fused kernel
struct AttentionImpl : t::nn::Module {
  AttentionImpl(int64_t dims, int64_t heads): heads(heads) {
    qkv = register_module("qkv", t::nn::Linear(t::nn::LinearOptions(dims, 3 * dims).bias(false)));
    out = register_module("out", t::nn::Linear(t::nn::LinearOptions(dims, dims).bias(false)));
  }
  t::Tensor forward(const t::Tensor& x) {
    int64_t b = x.size(0), n = x.size(1), d = x.size(2);
    t::Tensor q_k_v = qkv(x).view({b, n, 3, heads, d / heads}).permute({2, 0, 3, 1, 4});
    t::Tensor y = at::scaled_dot_product_attention(q_k_v[0], q_k_v[1], q_k_v[2]);
    return out(y.transpose(1, 2).reshape({b, n, d}));
  }
  int64_t       heads;
  t::nn::Linear qkv{nullptr}, out{nullptr};
};
TORCH_MODULE(Attention);

struct SwiGLUImpl : t::nn::Module {
  explicit SwiGLUImpl(int64_t dims) {
    // 2/3 of a 4x MLP, rounded up to a multiple of 32
    int64_t hidden = ((dims * 8 / 3) + 31) / 32 * 32;
    w1 = register_module("w1", t::nn::Linear(t::nn::LinearOptions(dims, hidden).bias(false)));
    w3 = register_module("w3", t::nn::Linear(t::nn::LinearOptions(dims, hidden).bias(false)));
    w2 = register_module("w2", t::nn::Linear(t::nn::LinearOptions(hidden, dims).bias(false)));
  }
  t::Tensor forward(const t::Tensor& x) {
    return w2(t::silu(w1(x)) * w3(x));
  }
  t::nn::Linear w1{nullptr}, w2{nullptr}, w3{nullptr};
};
TORCH_MODULE(SwiGLU);

// pre-norm transformer block
struct BlockImpl : t::nn::Module {
  BlockImpl(int64_t dims, int64_t heads) {
    norm1 = register_module("norm1", RMSNorm(dims));
    attn  = register_module("attn", Attention(dims, heads));
    norm2 = register_module("norm2", RMSNorm(dims));
    mlp   = register_module("mlp", SwiGLU(dims));
  }
  t::Tensor forward(t::Tensor x) {
    x = x + attn(norm1(x));
    return x + mlp(norm2(x));
  }
  RMSNorm   norm1{nullptr}, norm2{nullptr};
  Attention attn{nullptr};
  SwiGLU    mlp{nullptr};
};
TORCH_MODULE(Block);

struct Prediction {
  t::Tensor policy_logits;     // (B, A)
  t::Tensor value_logits;      // (B, 3)
  t::Tensor ownership_logits;  // (B, T, C) final owner seen from the mover
};

struct DynamicsOutput {
  t::Tensor latent;            // (B, T, D)
  t::Tensor reward_logits;     // (B, 3) reward of the action for its mover
};

// MuZero with every network a bidirectional transformer over board tokens:
//
//   represent h: token embedding (class embedding + feature projection +
//     learned position embedding), repr_layers blocks, LayerNorm without
//     affine parameters, which bounds the latent like MuZero's min-max
//     scaling of hidden states
//   dynamics g: a learned action token is prepended and the action's point
//     is marked, dyn_layers blocks; the action token reads out the reward,
//     the board tokens give the next latent after the same normalization
//   predict f: pred_layers blocks; per-point policy logits plus a pooled pass
//     logit, pooled MLP value head, per-token ownership head
//   project / predict_projection: SimSiam heads (EfficientZero consistency)
//   reconstruct: token classes of the board the latent stands for
//
// Attention connects any two points in one layer, which suits Go's long
// range interactions (ladders, liberties of large strings).
struct TransformerMuZeroImpl : t::nn::Module {
  explicit TransformerMuZeroImpl(const TransformerMuZeroConfig& config): config(config) {
    if (config.dims % config.num_heads != 0)
      throw s::invalid_argument("dims must be divisible by num_heads");
    if (config.num_actions != config.num_tokens + 1)
      throw s::invalid_argument("expects one action per token plus pass");
    int64_t d = config.dims;
    int64_t c = config.num_token_classes;
    int64_t p = config.proj_dims;

    class_embedding = register_module("class_embedding", t::nn::Embedding(c, d));
    feature_proj = register_module("feature_proj",
      t::nn::Linear(config.num_features - c, d));
    pos_embedding = register_parameter("pos_embedding",
      t::randn({config.num_tokens, d}) * 0.02);
    repr_blocks = register_module("repr_blocks", make_blocks(config.repr_layers));
    repr_norm = register_module("repr_norm",
      t::nn::LayerNorm(t::nn::LayerNormOptions({d}).elementwise_affine(false)));

    action_embedding = register_module("action_embedding",
      t::nn::Embedding(config.num_actions, d));
    action_marker = register_parameter("action_marker", t::randn({d}) * 0.02);
    dyn_blocks = register_module("dyn_blocks", make_blocks(config.dyn_layers));
    dyn_norm = register_module("dyn_norm",
      t::nn::LayerNorm(t::nn::LayerNormOptions({d}).elementwise_affine(false)));
    reward_norm = register_module("reward_norm", RMSNorm(d));
    reward_head = register_module("reward_head", t::nn::Linear(d, NUM_OUTCOME_CLASSES));

    pred_blocks = register_module("pred_blocks", make_blocks(config.pred_layers));
    pred_norm = register_module("pred_norm", RMSNorm(d));
    policy_head = register_module("policy_head", t::nn::Linear(d, 1));
    pass_head = register_module("pass_head", t::nn::Linear(d, 1));
    value_head = register_module("value_head", t::nn::Sequential(
      t::nn::Linear(d, d), t::nn::SiLU(), t::nn::Linear(d, NUM_OUTCOME_CLASSES)));
    ownership_head = register_module("ownership_head", t::nn::Linear(d, c));

    recon_head = register_module("recon_head", t::nn::Linear(d, c));
    projector = register_module("projector", t::nn::Sequential(
      t::nn::Linear(d, p), t::nn::LayerNorm(t::nn::LayerNormOptions({p})),
      t::nn::ReLU(), t::nn::Linear(p, p)));
    predictor = register_module("predictor", t::nn::Sequential(
      t::nn::Linear(p, p), t::nn::LayerNorm(t::nn::LayerNormOptions({p})),
      t::nn::ReLU(), t::nn::Linear(p, p)));
  }

  // features (B, T, F) uint8 -> latent (B, T, D)
  t::Tensor represent(const t::Tensor& features) {
    int64_t c = config.num_token_classes;
    t::Tensor f = features.to(pos_embedding.scalar_type());
    // features 0..c-1 one-hot encode the token class
    t::Tensor cls = (features.slice(-1, 1, 2).squeeze(-1).to(t::kLong)
                   + 2 * features.slice(-1, 2, 3).squeeze(-1).to(t::kLong));
    t::Tensor x = class_embedding(cls) + feature_proj(f.slice(-1, c)) + pos_embedding;
    return repr_norm(run_blocks(repr_blocks, x));
  }

  // latent (B, T, D), actions (B,) int64
  DynamicsOutput dynamics(const t::Tensor& latent, const t::Tensor& actions) {
    int64_t n = latent.size(1);
    t::Tensor positions = t::arange(n, actions.options());
    // pass (index n) marks no point
    t::Tensor onehot = actions.unsqueeze(1).eq(positions.unsqueeze(0)).to(latent.scalar_type());
    t::Tensor tokens = latent + onehot.unsqueeze(-1) * action_marker;
    t::Tensor action_token = action_embedding(actions).unsqueeze(1);
    t::Tensor x = run_blocks(dyn_blocks, t::cat({action_token, tokens}, 1));
    t::Tensor head = x.select(1, 0);
    t::Tensor next = x.slice(1, 1);
    return {dyn_norm(next), reward_head(reward_norm(head))};
  }

  Prediction predict(const t::Tensor& latent) {
    t::Tensor h = pred_norm(run_blocks(pred_blocks, latent));
    t::Tensor pooled = h.mean(1);
    t::Tensor policy = t::cat({policy_head(h).squeeze(-1), pass_head(pooled)}, 1);
    return {policy, value_head->forward(pooled), ownership_head(h)};
  }

  t::Tensor project(const t::Tensor& latent) { return projector->forward(latent); }
  t::Tensor predict_projection(const t::Tensor& proj) { return predictor->forward(proj); }
  t::Tensor reconstruct(const t::Tensor& latent) { return recon_head(latent); }

  int64_t num_parameters() {
    int64_t n = 0;
    for (const auto& p : parameters()) n += p.numel();
    return n;
  }

  TransformerMuZeroConfig config;

  t::nn::Embedding  class_embedding{nullptr};
  t::nn::Linear     feature_proj{nullptr};
  t::Tensor         pos_embedding;
  t::nn::ModuleList repr_blocks{nullptr};
  t::nn::LayerNorm  repr_norm{nullptr};

  t::nn::Embedding  action_embedding{nullptr};
  t::Tensor         action_marker;
  t::nn::ModuleList dyn_blocks{nullptr};
  t::nn::LayerNorm  dyn_norm{nullptr};
  RMSNorm           reward_norm{nullptr};
  t::nn::Linear     reward_head{nullptr};

  t::nn::ModuleList pred_blocks{nullptr};
  RMSNorm           pred_norm{nullptr};
  t::nn::Linear     policy_head{nullptr}, pass_head{nullptr};
  t::nn::Sequential value_head{nullptr};
  t::nn::Linear     ownership_head{nullptr};

  t::nn::Linear     recon_head{nullptr};
  t::nn::Sequential projector{nullptr}, predictor{nullptr};

private:
  t::nn::ModuleList make_blocks(int64_t layers) {
    t::nn::ModuleList blocks;
    for (int64_t i = 0; i < layers; ++i)
      blocks->push_back(Block(config.dims, config.num_heads));
    return blocks;
  }
  static t::Tensor run_blocks(t::nn::ModuleList& blocks, t::Tensor x) {
    for (auto& m : *blocks) x = m->as<Block>()->forward(x);
    return x;
  }
};
TORCH_MODULE(TransformerMuZero);

// copy every parameter and buffer of src into dst (dtypes may differ)
inline void copy_weights(TransformerMuZero& dst, TransformerMuZero& src) {
  t::NoGradGuard no_grad;
  auto dp = dst->named_parameters(true);
  auto sp = src->named_parameters(true);
  for (const auto& item : sp) dp[item.key()].copy_(item.value());
  auto db = dst->named_buffers(true);
  auto sb = src->named_buffers(true);
  for (const auto& item : sb) db[item.key()].copy_(item.value());
}

// a model file holds its configuration followed by the weights, so it can
// be loaded without knowing its size
inline void save_model(TransformerMuZero& model, const s::string& path) {
  t::serialize::OutputArchive archive;
  s::vector<int64_t> cfg = model->config.to_vector();
  archive.write("config", t::tensor(cfg, t::kLong));
  t::serialize::OutputArchive weights;
  model->save(weights);
  archive.write("weights", weights);
  archive.save_to(path);
}

inline TransformerMuZero load_model(const s::string& path, t::Device device = t::kCPU) {
  t::serialize::InputArchive archive;
  archive.load_from(path, device);
  t::Tensor cfg_t;
  archive.read("config", cfg_t);
  cfg_t = cfg_t.to(t::kCPU);
  s::vector<int64_t> cfg(cfg_t.data_ptr<int64_t>(), cfg_t.data_ptr<int64_t>() + cfg_t.numel());
  TransformerMuZero model(TransformerMuZeroConfig::from_vector(cfg));
  t::serialize::InputArchive weights;
  archive.read("weights", weights);
  model->load(weights);
  model->to(device);
  return model;
}

}  // namespace rlgames::muzero

#endif  // RLGAMES_MUZERO_TRANSFORMER_MUZERO
