# libtorch C++ Pitfalls and Known Gaps

## Version: libtorch 2.5.1

---

## Transformer Layout — batch_first Missing

**Problem**: Python `nn.TransformerEncoderLayer(d_model, nhead, batch_first=True)` lets you pass
`[B, seq, D]` tensors. In libtorch 2.5.1 C++, `TransformerEncoderLayerOptions` has no
`batch_first` field. Passing `[B, seq, D]` silently produces wrong results.

**Fix**: Permute to seq-first before the encoder, then pool over dim 0:

```cpp
// tokens: [B, seq, D]
auto seq = tokens.permute({1, 0, 2});    // [seq, B, D]
seq = encoder->forward(seq);             // [seq, B, D]
auto pooled = seq.mean(0);               // [B, D]  — mean over sequence
```

**Status**: `batch_first` exists in Python since 1.9; the C++ API still lacks it as of 2.5.1.
Check `include/torch/csrc/api/include/torch/nn/options/transformerlayer.h` to see if it's been
added in a newer install.

---

## norm_first (Pre-Norm Transformer) Missing

**Problem**: Python supports `norm_first=True` for pre-LayerNorm ("pre-norm") transformers, which
typically train more stably. The C++ `TransformerEncoderLayerOptions` does not have this.

**Fix**: Write a custom encoder layer `struct` that applies LayerNorm before attention and FFN.
No automatic workaround in libtorch 2.5.1.

---

## .item<T>() in Template Context

**Problem**: In a template member function or template free function, calling `.item<float>()` on
a dependent expression fails to compile:

```
error: expected primary-expression before 'float'
```

**Fix**: Use `.template item<float>()`:

```cpp
template <typename ModelType>
int NeuralHeuristic<ModelType>::operator()(Board& b) {
  float cost = model->forward(input).template item<float>();  // correct
  // float cost = model->forward(input).item<float>();        // compile error
}
```

This is a standard C++ rule: member template calls on dependent types require the `template`
keyword. Only needed inside templates — plain code can use `.item<float>()` directly.

---

## ODR Violations with Header-Only Libraries

**Problem**: libtorch is a "heavy" library — when two `.cpp` files both include a header-only
project library (e.g., a game state library compiled as a CMake `INTERFACE` target) and both also
link libtorch, the linker errors with "multiple definition of …" for every non-inline free function.

```
multiple definition of `sokoban::operator==(const Pt&, const Pt&)`
```

**Why**: Both `.cpp` files get their own copy of the function body, which is fine for a plain
executable but libtorch's linking triggers the check.

**Fix**: Add `inline` to every free function definition in the header-only library:

```cpp
// game_base.h
inline bool operator==(const Pt& a, const Pt& b) { … }
inline Pt   operator+(const Pt& p, const Dr& d) { … }
inline std::ostream& operator<<(std::ostream& out, const Pt& p) { … }
```

`constexpr` functions are already implicitly `inline`. Only non-constexpr, non-member functions
need this.

---

## torch::save / torch::load Require Pre-Constructed Model

**Problem**: `torch::load(model, path)` does not reconstruct the architecture — it only fills
weights into an already-constructed model. Loading into a default-constructed (empty) model
silently succeeds but all forward passes produce garbage or crash.

**Fix**: Always construct the model with the correct architecture first:

```cpp
SokobanCNN model;          // constructor runs: registers all layers
torch::load(model, path);  // fills in the weights
model->eval();
```

---

## ABI Mismatch: _GLIBCXX_USE_CXX11_ABI

**Problem**: libtorch ships in two ABI flavours (old ABI and new C++11 ABI). If the flag
`-D_GLIBCXX_USE_CXX11_ABI=0` or `=1` in `TORCH_CXX_FLAGS` does not match how your project's other
libraries were compiled, linking fails with obscure symbol errors about `std::string` or
`std::vector`.

**Fix**: Always pass `${TORCH_CXX_FLAGS}` to every target that links torch. Do not mix torch
objects with objects compiled without that flag.

---

## BatchNorm Behaviour Difference Between train() and eval()

`torch::nn::BatchNorm2d` behaves differently in training vs inference:

- `train()` mode: normalises using the batch mean/variance; updates running statistics.
- `eval()` mode: normalises using the stored running mean/variance (fixed).

Always call `model->train()` before the training loop and `model->eval()` before inference.
Forgetting `eval()` before inference causes non-deterministic outputs that vary with batch
composition.

---

## Dropout is a No-Op in eval() Mode

`torch::nn::Dropout` and `torch::nn::Dropout2d` are silently disabled when the module is in
`eval()` mode. This is correct behaviour but can mask over-fitting: a model that only works
because Dropout is active will appear fine during eval.

---

## Tensor Accessor Validity

`t.accessor<float, 3>()` is only valid on:
- Contiguous CPU tensors (`t.is_contiguous()` must be true)
- Tensors with the matching scalar type

Accessing a GPU tensor or a non-contiguous view via an accessor is undefined behaviour. Use
`.cpu().contiguous()` before accessing if the tensor may have come from a permute or a GPU:

```cpp
auto a = t.cpu().contiguous().accessor<float, 3>();
```

---

## Gaps Not Yet Investigated (as of this project)

These Python features likely have C++ equivalents but were not needed here — verify before use:

| Feature | Python | C++ Status |
|---------|--------|------------|
| Mixed precision | `torch.autocast` | `torch::autocast` exists; API untested |
| TorchScript export | `torch.jit.script` | `torch::jit::script` available; not verified |
| Multi-GPU DDP | `DistributedDataParallel` | C++ API exists; complex setup, not tested |
| Learning rate schedulers | `torch.optim.lr_scheduler.*` | Partial C++ support; `torch::optim::StepLR` exists |
| `torch.compile` (2.0+) | graph compilation | No C++ equivalent in 2.5.1 |
| `DataLoader` | `torch.utils.data.DataLoader` | C++ `torch::data::DataLoader` exists but limited |
