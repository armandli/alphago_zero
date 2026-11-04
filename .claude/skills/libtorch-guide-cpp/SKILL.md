---
name: libtorch-guide-cpp
description: Expert reference guide for the libtorch C++ deep learning library (the C++ frontend of PyTorch). Auto-activates when writing or reviewing C++ code that uses libtorch, torch::nn::Module, TORCH_MODULE, torch::Tensor, torch::optim, TransformerEncoder, Conv2d, BatchNorm2d, Linear, Embedding, torch::save, torch::load, or encode_state for neural network inference; when setting up CMake with find_package(Torch), TORCH_INCLUDE_DIRS, TORCH_LIBRARIES, or TORCH_CXX_FLAGS; when implementing training loops, asymmetric loss, or neural heuristics; when the user asks how to define a model, run a forward pass, train with Adam, save/load weights, stack tensors, or use accessors in C++. Covers correct module definition patterns, the Impl/TORCH_MODULE idiom, training vs eval mode, known C++ API gaps vs Python PyTorch, CMake integration, ODR hazards, and version-specific issues found in libtorch 2.5.1. Do NOT use for Python PyTorch, TensorFlow, ONNX Runtime, or other ML frameworks.
---

# libtorch C++ Guide

libtorch is the C++ frontend of PyTorch. It provides `torch::Tensor`, `torch::nn`, `torch::optim`,
and serialization — everything needed to define, train, and run neural networks without Python.

**This project uses libtorch 2.5.1** installed at `/home/armandli/alphago_zero/libtorch`.

## Architecture

| Component | Namespace / Header | Purpose |
|-----------|-------------------|---------|
| Tensor ops | `torch::` / `<torch/torch.h>` | All tensor math, creation, indexing |
| Modules | `torch::nn::` | Layer definitions (Conv2d, Linear, …) |
| Optimizers | `torch::optim::` | Adam, SGD, … |
| Serialization | `torch::save` / `torch::load` | Write/read module weights |
| Autograd | `torch::autograd::` | Automatic differentiation (mostly implicit) |

One header covers everything: `#include <torch/torch.h>`.

## CMake Setup

libtorch is not installed via package manager — point cmake at the extracted directory.

```cmake
# Root CMakeLists.txt
set(TORCH_PATH "/path/to/libtorch"
    CACHE PATH "Path to libtorch installation")
find_package(Torch REQUIRED PATHS ${TORCH_PATH} NO_DEFAULT_PATH)
```

Per-target wiring (use SYSTEM PRIVATE for include dirs to suppress torch header warnings):

```cmake
add_library(my_model model.cpp)
target_include_directories(my_model SYSTEM PRIVATE ${TORCH_INCLUDE_DIRS})
target_link_libraries(my_model PUBLIC ${TORCH_LIBRARIES})
target_compile_options(my_model PRIVATE ${TORCH_CXX_FLAGS})
set_property(TARGET my_model PROPERTY CXX_STANDARD 17)
```

**C++ standard**: libtorch 2.5.1 compiles cleanly at C++17. If the same target also includes
project headers that use `std::format` or C++20 concepts, bump to C++20.

See [references/cmake-setup.md](references/cmake-setup.md) for multi-target layouts and ODR pitfalls.

## Defining a Module — The Impl/TORCH_MODULE Idiom

Every custom module uses a two-part pattern required by libtorch's reference-counted shared_ptr:

```cpp
// mymodel.h
struct MyModelImpl : torch::nn::Module {
  torch::nn::Linear fc1{nullptr}, fc2{nullptr};

  MyModelImpl() {
    fc1 = register_module("fc1", torch::nn::Linear(128, 64));
    fc2 = register_module("fc2", torch::nn::Linear(64, 1));
  }

  torch::Tensor forward(torch::Tensor x) {
    x = torch::relu(fc1->forward(x));
    return fc2->forward(x).squeeze(1);
  }
};
TORCH_MODULE(MyModel);  // defines MyModel as shared_ptr<MyModelImpl>
```

**Rules:**
- Declare sub-modules as `torch::nn::SomeLayer{nullptr}` (null-initialised), then assign in the
  constructor via `register_module(name, ...)`.
- Always call `register_module` — otherwise parameters are invisible to the optimizer.
- `TORCH_MODULE(Foo)` makes `Foo` a `torch::nn::ModuleHolder<FooImpl>`, which behaves like a
  `shared_ptr` with `->` forwarding. Use `model->forward(x)`, not `model.forward(x)`.

## Training Pattern

```cpp
MyModel model;
model->train();

torch::optim::Adam opt(model->parameters(),
                       torch::optim::AdamOptions(1e-3));

for (int epoch = 0; epoch < N_EPOCHS; ++epoch) {
  auto [input, target] = make_batch();     // your data loading

  opt.zero_grad();
  torch::Tensor pred = model->forward(input);
  torch::Tensor loss = torch::mse_loss(pred, target);
  loss.backward();
  opt.step();

  std::cout << loss.item<double>() << "\n";
}
```

## Inference Pattern

```cpp
MyModel model;
torch::load(model, "weights.pt");
model->eval();

{
  torch::NoGradGuard no_grad;
  torch::Tensor input = make_input();
  float result = model->forward(input).template item<float>();
  // ^^^ .template needed inside a template function; plain .item<float>() elsewhere
}
```

## Save / Load

```cpp
torch::save(model, "model.pt");   // saves all registered parameters
torch::load(model, "model.pt");   // restores in-place; model must already be constructed
```

Works on any `TORCH_MODULE`-wrapped type. The model must be constructed with the same architecture
before loading — load does not reconstruct the graph.

## Tensor Creation Cheat-Sheet

```cpp
torch::zeros({B, C, H, W})            // float32 by default
torch::ones({N})
torch::arange(N, torch::kLong)        // [0, 1, …, N-1] as int64
torch::tensor(std::vector<float>{…})  // from a std::vector
torch::stack(vec_of_tensors, 0)       // stack along new dim 0
torch::cat({a, b}, 1)                 // concatenate along existing dim 1
```

## Tensor Shape Ops

```cpp
x.unsqueeze(0)        // insert dim 0: [C,H,W] → [1,C,H,W]
x.squeeze(1)          // remove dim 1 if size==1
x.reshape({B, -1})    // -1 inferred
x.permute({0, 2, 3, 1})  // reorder dims
x.mean({2, 3})        // global average pool over H and W
x.mean(0)             // mean over dim 0 (seq-pool)
x.size(0)             // → int64_t
```

## Known C++ API Gaps vs Python PyTorch (2.5.1)

These Python options **do not exist** in the C++ `TransformerEncoderLayerOptions`:

| Python kwarg | C++ workaround |
|-------------|----------------|
| `batch_first=True` | Permute input to `[seq, B, D]` before `encoder->forward()`, then permute or `mean(0)` after |
| `norm_first=True` | Not available — pre-norm transformer requires a custom `Module` |

The transformer always expects `[seq_len, batch, d_model]` in this version. See the layout
example in [references/pitfalls.md](references/pitfalls.md#transformer-layout).

## Decision Guide

| Task | Use |
|------|-----|
| Spatial features (images, grids) | CNN (`Conv2d` + `BatchNorm2d` + ReLU + global avg pool) |
| Sequence / relational reasoning | Transformer (`TransformerEncoder`) |
| Simple function approximation | MLP (`Linear` stack) |
| Approximately admissible heuristic | Asymmetric MSE loss (penalise overestimation 4×) |

## Additional Resources

- CMake multi-target layout, C++ standard compatibility, ODR hazards: [references/cmake-setup.md](references/cmake-setup.md)
- Full layer API — Conv2d, Linear, BatchNorm, Embedding, TransformerEncoder options, optimizers: [references/api-reference.md](references/api-reference.md)
- C++ vs Python gaps, version-specific issues, common compiler errors: [references/pitfalls.md](references/pitfalls.md)
- Complete working code (CNN model, Attention model, training loop, asymmetric loss, neural A*): [references/examples.md](references/examples.md)
