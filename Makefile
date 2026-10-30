BUILD_DIR  := build
LIBTORCH   ?= $(CURDIR)/libtorch
CMAKE_ARGS  = -DTorch_DIR=$(LIBTORCH)/share/cmake/Torch \
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
