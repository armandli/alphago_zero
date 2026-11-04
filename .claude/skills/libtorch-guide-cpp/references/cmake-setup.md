# CMake Setup for libtorch

## Locating the Library

libtorch is not a system library — it ships as a self-contained directory. Tell CMake where it is:

```cmake
# Root CMakeLists.txt — set before project() if possible
set(TORCH_PATH "/path/to/libtorch"
    CACHE PATH "Path to libtorch installation")

find_package(Torch REQUIRED PATHS ${TORCH_PATH} NO_DEFAULT_PATH)
```

`NO_DEFAULT_PATH` prevents CMake from accidentally picking up a system PyTorch install that may
differ in ABI.

`find_package(Torch)` populates:

| Variable | Contents |
|----------|----------|
| `TORCH_INCLUDE_DIRS` | Paths to torch and ATen headers |
| `TORCH_LIBRARIES` | All `.so` / `.lib` files needed to link |
| `TORCH_CXX_FLAGS` | `-D_GLIBCXX_USE_CXX11_ABI=0` or `=1` (must match how libtorch was compiled) |

## Per-Target Wiring

```cmake
add_library(my_model model.cpp)

# SYSTEM PRIVATE suppresses warnings from torch headers (they are numerous)
target_include_directories(my_model SYSTEM PRIVATE ${TORCH_INCLUDE_DIRS})
target_link_libraries(my_model PUBLIC ${TORCH_LIBRARIES})
target_compile_options(my_model PRIVATE ${TORCH_CXX_FLAGS})
set_property(TARGET my_model PROPERTY CXX_STANDARD 17)
```

`PUBLIC` on `target_link_libraries` propagates torch to any target that links `my_model`, so
downstream targets don't need to repeat the link.

## Multi-Target Layout

When a project has separate model, training, and app targets that all use torch:

```
src/
  model/        ← model.h + model.cpp, links torch
  training/     ← data_generator.cpp, links game only (no torch)
  app/
    puzzle_train.cpp   ← links model + training + torch
    puzzle_astar_nn.cpp ← links model + torch
```

**Separation rule**: Only targets that call torch APIs need `TORCH_INCLUDE_DIRS` and
`TORCH_LIBRARIES`. A data generator that only manipulates game state does not.

## C++ Standard Compatibility

libtorch 2.5.1 compiles cleanly at C++17. Issues arise when mixing with other project headers:

| Project headers use | Minimum standard for that target |
|--------------------|----------------------------------|
| Only libtorch | C++17 |
| `std::format` | C++20 |
| Concepts (`requires`, `concept`) | C++20 |
| `std::format` + concepts | C++20 |

Set the standard per-target so only the targets that need it pay the cost:

```cmake
set_property(TARGET my_model PROPERTY CXX_STANDARD 17)
set_property(TARGET my_app   PROPERTY CXX_STANDARD 20)
```

## ODR Hazard: Header-Only Libraries + libtorch

If your project has a header-only library (CMake `INTERFACE` target) whose headers define non-inline
free functions, and that library is included by multiple `.cpp` files that all also link libtorch,
the linker will error with "multiple definition of …".

The fix is to mark all free function definitions in those headers `inline`:

```cpp
// game_base.h — header-only, included by model.cpp AND data_generator.cpp
inline bool operator==(const Pt& a, const Pt& b) { … }
inline Pt operator+(const Pt& p, const Dr& d) { … }
inline std::ostream& operator<<(std::ostream& out, const Pt& pt) { … }
```

`constexpr` functions are implicitly `inline` and do not need this fix.

## Verifying the Build

```bash
cmake -S . -B build -DTORCH_PATH=/path/to/libtorch
cmake --build build --target my_model my_app
ldd build/bin/my_app | grep torch   # confirm dynamic libs found
```

If `ldd` shows "not found", set `LD_LIBRARY_PATH` or add an rpath:

```cmake
set_target_properties(my_app PROPERTIES
    BUILD_RPATH "/path/to/libtorch/lib"
    INSTALL_RPATH "/path/to/libtorch/lib")
```
