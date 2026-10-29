BUILD_DIR  := build
LIBTORCH   ?= $(CURDIR)/libtorch
CMAKE_ARGS  = -DTorch_DIR=$(LIBTORCH)/share/cmake/Torch \
              -DCMAKE_INSTALL_PREFIX=$(BUILD_DIR)/install
JOBS       := $(shell nproc)

.PHONY: all build configure rebuild clean

all: build

build: $(BUILD_DIR)/Makefile
	cmake --build $(BUILD_DIR) --config Release -j$(JOBS)

$(BUILD_DIR)/Makefile:
	mkdir -p $(BUILD_DIR)
	cmake -S . -B $(BUILD_DIR) $(CMAKE_ARGS)

configure:
	mkdir -p $(BUILD_DIR)
	cmake -S . -B $(BUILD_DIR) $(CMAKE_ARGS)

rebuild: clean build

clean:
	rm -rf $(BUILD_DIR)
