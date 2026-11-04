# libtorch C++ Examples

All examples are from the sokoban project and compile against libtorch 2.5.1 at C++17/20.

---

## CNN Model (sokoban_model.h / sokoban_model.cpp)

```cpp
// sokoban_model.h
struct SokobanCNNImpl : torch::nn::Module {
  torch::nn::Conv2d      conv1{nullptr}, conv2{nullptr}, conv3{nullptr};
  torch::nn::BatchNorm2d bn1{nullptr},   bn2{nullptr},   bn3{nullptr};
  torch::nn::Linear      fc1{nullptr},   fc2{nullptr};

  SokobanCNNImpl();
  torch::Tensor forward(torch::Tensor x);
};
TORCH_MODULE(SokobanCNN);

// sokoban_model.cpp
SokobanCNNImpl::SokobanCNNImpl() {
  conv1 = register_module("conv1",
      torch::nn::Conv2d(torch::nn::Conv2dOptions(5, 32, 3).padding(1)));
  bn1   = register_module("bn1", torch::nn::BatchNorm2d(32));
  conv2 = register_module("conv2",
      torch::nn::Conv2d(torch::nn::Conv2dOptions(32, 64, 3).padding(1)));
  bn2   = register_module("bn2", torch::nn::BatchNorm2d(64));
  conv3 = register_module("conv3",
      torch::nn::Conv2d(torch::nn::Conv2dOptions(64, 128, 3).padding(1)));
  bn3   = register_module("bn3", torch::nn::BatchNorm2d(128));
  fc1   = register_module("fc1", torch::nn::Linear(128, 256));
  fc2   = register_module("fc2", torch::nn::Linear(256,   1));
}

torch::Tensor SokobanCNNImpl::forward(torch::Tensor x) {
  // x: [B, 5, 10, 10]
  x = torch::relu(bn1->forward(conv1->forward(x)));  // [B, 32, 10, 10]
  x = torch::relu(bn2->forward(conv2->forward(x)));  // [B, 64, 10, 10]
  x = torch::relu(bn3->forward(conv3->forward(x)));  // [B, 128, 10, 10]
  x = x.mean({2, 3});                                // [B, 128] global avg pool
  x = torch::relu(fc1->forward(x));                  // [B, 256]
  return fc2->forward(x).squeeze(1);                 // [B]
}
```

---

## Attention Model — ViT-Style (sokoban_model.cpp)

Demonstrates the seq-first permutation workaround for missing `batch_first`:

```cpp
SokobanAttnImpl::SokobanAttnImpl() {
  channel_proj = register_module("channel_proj",
      torch::nn::Linear(torch::nn::LinearOptions(5, 64).bias(false)));
  pos_embed    = register_module("pos_embed",
      torch::nn::Embedding(100, 64));   // 100 tokens = 10x10 grid

  auto enc_layer = torch::nn::TransformerEncoderLayer(
      torch::nn::TransformerEncoderLayerOptions(64, 4)   // d=64, 4 heads
          .dim_feedforward(256)
          .dropout(0.1));                // no batch_first in 2.5.1
  encoder = register_module("encoder",
      torch::nn::TransformerEncoder(enc_layer, 4));      // 4 layers

  fc1 = register_module("fc1", torch::nn::Linear(64, 128));
  fc2 = register_module("fc2", torch::nn::Linear(128,  1));
}

torch::Tensor SokobanAttnImpl::forward(torch::Tensor x) {
  int B = x.size(0);

  // Project binary channels → token embeddings: [B, 100, 64]
  auto flat   = x.permute({0, 2, 3, 1}).reshape({B, 100, 5});
  auto tokens = channel_proj->forward(flat);

  // Add positional embeddings
  auto pos_idx = torch::arange(100, x.options().dtype(torch::kLong));
  tokens = tokens + pos_embed->forward(pos_idx).unsqueeze(0).expand({B, -1, -1});

  // seq-first layout required by TransformerEncoder in 2.5.1
  auto seq = tokens.permute({1, 0, 2});    // [100, B, 64]
  seq      = encoder->forward(seq);        // [100, B, 64]
  auto pooled = seq.mean(0);               // [B, 64]

  pooled = torch::relu(fc1->forward(pooled));
  return fc2->forward(pooled).squeeze(1);
}
```

---

## State Encoder (5-channel binary grid)

```cpp
torch::Tensor encode_state(const SokobanBoard& board, const BoardState& st) {
  auto t = torch::zeros({5, 10, 10});
  auto a = t.accessor<float, 3>();

  for (const Pt& p : board.walls())   a[0][p.x][p.y] = 1.f;
  for (const Pt& p : board.goals())   a[2][p.x][p.y] = 1.f;
  for (const Pt& p : st.boxes)        a[3][p.x][p.y] = 1.f;
  a[4][st.player.x][st.player.y] = 1.f;

  for (const Pt& p : board.spaces()) {
    if (board.goals().find(p) == board.goals().end() and
        st.boxes.find(p)      == st.boxes.end()      and
        p                     != st.player)
      a[1][p.x][p.y] = 1.f;
  }
  return t;
}
```

Channels: `[wall, empty-floor, goal, box, player]`. Two bits set = box-on-goal (ch 2+3) or
player-on-goal (ch 2+4).

---

## Asymmetric MSE Loss

```cpp
// asymmetric_loss.h
inline torch::Tensor asymmetric_mse(torch::Tensor pred, torch::Tensor target,
                                     float w_over = 4.f, float w_under = 1.f) {
  auto over  = torch::relu(pred - target);
  auto under = torch::relu(target - pred);
  return (w_over * over.pow(2) + w_under * under.pow(2)).mean();
}
```

With `w_over=4, w_under=1`: overestimation is penalised 4× more. Biases the network toward
underestimation so the learned heuristic stays approximately admissible for A*.

---

## Training Loop with Curriculum

```cpp
SokobanCNN model;
model->train();
torch::optim::Adam opt(model->parameters(), torch::optim::AdamOptions(1e-3f));

for (int epoch = 1; epoch <= 200; ++epoch) {
  // Curriculum: depth grows linearly from 5 to 50
  float progress  = (float)(epoch - 1) / 199.f;
  int   max_depth = (int)std::round(5 + progress * 45);
  gen.set_max_depth(max_depth);

  for (int step = 0; step < 500; ++step) {
    auto batch = gen.next_batch(128);

    std::vector<torch::Tensor> encoded;
    for (int i = 0; i < 128; ++i)
      encoded.push_back(encode_state(gen.board_at(batch.board_indices[i]),
                                     batch.states[i]));

    torch::Tensor input  = torch::stack(encoded, 0);  // [128, 5, H, W]
    torch::Tensor target = torch::tensor(
        std::vector<float>(batch.costs.begin(), batch.costs.end()));

    opt.zero_grad();
    auto pred = model->forward(input);
    auto loss = asymmetric_mse(pred, target);
    loss.backward();
    opt.step();
  }
}
torch::save(model, "cnn_final.pt");
```

---

## Neural Heuristic Wrapper (satisfies Heuristic concept)

```cpp
template <typename ModelType>
struct NeuralHeuristic {
  ModelType model;

  explicit NeuralHeuristic(const std::filesystem::path& weights_path) {
    torch::load(model, weights_path.string());
    model->eval();
  }

  int operator()(SokobanBoard& board) {
    torch::NoGradGuard no_grad;
    auto input = encode_state(board).unsqueeze(0);       // [1, 5, H, W]
    float cost = model->forward(input).template item<float>();
    return (int)std::floor(std::max(0.f, cost));
  }
};

// Usage:
NeuralHeuristic<SokobanCNN> h("cnn_final.pt");
int steps = astar(game, h);
```
