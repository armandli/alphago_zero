BUILD_DIR  := build
LIBTORCH   ?= $(firstword $(wildcard $(CURDIR)/libtorch $(HOME)/.local/libtorch) $(CURDIR)/libtorch)
# C++26 features (<print>, std::println) need GCC >= 14
CXX_COMPILER ?= $(or $(shell command -v g++-14 2>/dev/null),g++)
# libtorch CUDA builds need nvcc discoverable at configure time
CUDA_HOME  ?= /usr/local/cuda
ifneq ($(wildcard $(CUDA_HOME)/bin/nvcc),)
export PATH := $(CUDA_HOME)/bin:$(PATH)
endif
CMAKE_ARGS  = -DTorch_DIR=$(LIBTORCH)/share/cmake/Torch \
              -DCMAKE_CXX_COMPILER=$(CXX_COMPILER) \
              -DCMAKE_INSTALL_PREFIX=$(BUILD_DIR)/install
JOBS       := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

.PHONY: all build configure rebuild clean check-torch

all: build

build: $(BUILD_DIR)/Makefile
	cmake --build $(BUILD_DIR) --config Release -j$(JOBS)

check-torch:
	@test -f $(LIBTORCH)/share/cmake/Torch/TorchConfig.cmake || \
	  { echo "error: libtorch not found at $(LIBTORCH); run 'make LIBTORCH=/path/to/libtorch'"; exit 1; }

$(BUILD_DIR)/Makefile: | check-torch
	mkdir -p $(BUILD_DIR)
	cmake -S . -B $(BUILD_DIR) $(CMAKE_ARGS)

configure: check-torch
	mkdir -p $(BUILD_DIR)
	cmake -S . -B $(BUILD_DIR) $(CMAKE_ARGS)

rebuild: clean build

clean:
	rm -rf $(BUILD_DIR)
