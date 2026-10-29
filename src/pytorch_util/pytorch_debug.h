#ifndef RLGAMES_PYTORCH_DEBUG
#define RLGAMES_PYTORCH_DEBUG

#include <cassert>
#include <iostream>

#include <type_alias.h>

#include <torch/torch.h>

namespace rlgames {

namespace s = std;
namespace t = torch;

void check_dim(t::Tensor tensor, init_list<sint64> expected_dims) {
  assert(tensor.dim() == (sint64)expected_dims.size());
  uint i = 0;
  for (int expected : expected_dims) {
    assert(tensor.size(i) == expected);
    i++;
  }
}

void print_dim(t::Tensor tensor) {
  s::cout << "[";
  for (uint i = 0; i < tensor.dim(); ++i)
    s::cout << tensor.size(i) << ", ";
  s::cout << "]" << s::endl;
}

}  // namespace rlgames

#endif  // RLGAMES_PYTORCH_DEBUG
