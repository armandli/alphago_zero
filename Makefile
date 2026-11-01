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
NPROC      := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
# each libtorch translation unit needs ~2-3 GB in cc1plus; running nproc jobs on a
# low-memory machine (e.g. WSL2) gets compilers OOM-killed, so cap jobs by RAM
MEM_PER_JOB_KB ?= 3000000
MEM_JOBS   := $(shell awk -v per=$(MEM_PER_JOB_KB) '/^MemAvailable:/ { j = int($$2 / per); print (j < 1 ? 1 : j) }' /proc/meminfo 2>/dev/null)
JOBS       ?= $(shell j=$(NPROC); m=$(or $(MEM_JOBS),$(NPROC)); echo $$(( j < m ? j : m )))

.PHONY: all build test configure rebuild clean check-torch

all: build

build: $(BUILD_DIR)/Makefile
	cmake --build $(BUILD_DIR) --config Release -j$(JOBS)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

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
