# libtorch C++ API Reference

## Layer Options

### torch::nn::Conv2dOptions

```cpp
torch::nn::Conv2d conv = register_module("conv",
    torch::nn::Conv2d(
        torch::nn::Conv2dOptions(in_channels, out_channels, kernel_size)
            .stride(1)
            .padding(1)       // "same" padding for 3x3 kernel on any size
            .dilation(1)
            .groups(1)
            .bias(true)));
```

Output spatial size: `floor((H + 2*padding - dilation*(kernel-1) - 1) / stride + 1)`

### torch::nn::LinearOptions

```cpp
torch::nn::Linear fc = register_module("fc",
    torch::nn::Linear(
        torch::nn::LinearOptions(in_features, out_features)
            .bias(true)));
```

### torch::nn::BatchNorm2dOptions

```cpp
torch::nn::BatchNorm2d bn = register_module("bn",
    torch::nn::BatchNorm2d(num_features));
// num_features = number of channels (C in [B,C,H,W])
```

### torch::nn::EmbeddingOptions

```cpp
torch::nn::Embedding emb = register_module("emb",
    torch::nn::Embedding(num_embeddings, embedding_dim));
// lookup: emb->forward(index_tensor)  // index_tensor: int64
```

### torch::nn::TransformerEncoderLayerOptions (2.5.1)

```cpp
auto enc_layer = torch::nn::TransformerEncoderLayer(
    torch::nn::TransformerEncoderLayerOptions(d_model, nhead)
        .dim_feedforward(2048)   // default 2048
        .dropout(0.1));          // default 0.1
// NOTE: batch_first NOT available — input must be [seq, B, D]
// NOTE: norm_first NOT available — post-norm only
```

### torch::nn::TransformerEncoderOptions

```cpp
torch::nn::TransformerEncoder encoder = register_module("encoder",
    torch::nn::TransformerEncoder(enc_layer, num_layers));
// forward: encoder->forward(src)        // no mask
//          encoder->forward(src, mask)  // optional src_mask [seq, seq]
```

## Activation Functions (free functions)

```cpp
torch::relu(x)      // in-place: torch::relu_(x)
torch::gelu(x)
torch::sigmoid(x)
torch::tanh(x)
torch::softmax(x, dim)
torch::log_softmax(x, dim)
```

## Loss Functions (free functions)

```cpp
torch::mse_loss(pred, target)
torch::l1_loss(pred, target)
torch::binary_cross_entropy(pred, target)
torch::cross_entropy_loss(pred, target)   // target is class indices
torch::nll_loss(pred, target)

// Reduction: torch::Reduction::Mean (default), Sum, None
torch::mse_loss(pred, target, torch::Reduction::None)
```

Custom loss example (asymmetric MSE):
```cpp
torch::Tensor asymmetric_mse(torch::Tensor pred, torch::Tensor target,
                              float w_over = 4.f, float w_under = 1.f) {
  auto over  = torch::relu(pred - target);
  auto under = torch::relu(target - pred);
  return (w_over * over.pow(2) + w_under * under.pow(2)).mean();
}
```

## Optimizers

```cpp
// Adam
torch::optim::Adam opt(model->parameters(),
                       torch::optim::AdamOptions(lr)
                           .betas({0.9, 0.999})
                           .eps(1e-8)
                           .weight_decay(0.0));

// SGD with momentum
torch::optim::SGD opt(model->parameters(),
                      torch::optim::SGDOptions(lr)
                          .momentum(0.9)
                          .weight_decay(1e-4));

// Step
opt.zero_grad();
loss.backward();
opt.step();
```

## Tensor Accessors (CPU only)

For reading/writing individual tensor elements on CPU (much faster than repeated `.item<>()`):

```cpp
auto t = torch::zeros({C, H, W});
auto a = t.accessor<float, 3>();   // 3 = number of dimensions
a[ch][row][col] = 1.f;
```

Accessor type: `torch::TensorAccessor<scalar_t, ndim>`.
Only valid on contiguous CPU tensors. Do not use on GPU tensors.

## Device Management

```cpp
torch::Device device = torch::cuda::is_available()
                       ? torch::kCUDA : torch::kCPU;
model->to(device);
input = input.to(device);

// Explicit device tensor creation
torch::zeros({B, C, H, W}, device)
torch::zeros({B, C, H, W}, torch::TensorOptions().device(device).dtype(torch::kFloat32))
```

## Gradient Control

```cpp
// Disable gradient tracking for a scope (inference)
{
  torch::NoGradGuard no_grad;
  auto out = model->forward(input);
}

// Disable on a single tensor
auto t = x.detach();

// Training vs eval mode (affects BatchNorm, Dropout)
model->train();   // training
model->eval();    // inference
```

## Tensor Item Extraction

```cpp
float v  = t.item<float>();     // scalar tensor → float
double v = t.item<double>();
int64_t v = t.item<int64_t>();

// Inside a template function — requires 'template' keyword:
float v = tensor.template item<float>();
```

## Module Inspection

```cpp
// Count parameters
int64_t n = 0;
for (const auto& p : model->parameters())
  n += p.numel();

// Named parameters (for custom optimizers or analysis)
for (const auto& [name, p] : model->named_parameters())
  std::cout << name << ": " << p.sizes() << "\n";
```
