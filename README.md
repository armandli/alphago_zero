# alphago_zero
Implementation of alphago zero in C++

# dependencies
- CMake >= 3.15
- GCC >= 14 (C++26: `<print>`, `std::println`)
- LibTorch (C++ distribution of PyTorch, C++20 or later headers)
- CUDA toolkit matching your LibTorch build (only for CUDA builds of LibTorch, e.g. `cu132` needs CUDA 13.2)

Fetched automatically by CMake at configure time:
- [CLI11](https://github.com/CLIUtils/CLI11) v2.7.2
- [FTXUI](https://github.com/ArthurSonzogni/FTXUI) v7.0.0
- [simdjson](https://github.com/simdjson/simdjson) v5.0.8 (model config parsing, training result output)
- [GoogleTest](https://github.com/google/googletest) v1.17.0 (unit tests; disable with `-DALPHAGO_BUILD_TESTS=OFF`)

# build
```
make                                  # finds ./libtorch or ~/.local/libtorch
make LIBTORCH=/path/to/libtorch       # explicit LibTorch location
make CXX_COMPILER=g++-14              # explicit compiler
make CUDA_HOME=/usr/local/cuda-13.2   # explicit CUDA toolkit
```
Binaries are written to `build/bin/`.

# test
```
make test
```
