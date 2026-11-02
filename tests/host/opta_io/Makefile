# Host tests for the Opta I/O helpers (CL-1):
# TankAlarm-112025-Common/src/TankAlarm_OptaIo.h
#
#   make -C tests/host/opta_io test
#
# Builds the test twice with the firmware's language and char flags (gnu++14, no RTTI, unsigned
# char): with ASan/UBSan at -O1, and at -Os (the firmware's level) with -Werror; runs both. The
# header needs no ArduinoJson; ARDUINOJSON_DIR is accepted (CI passes it to every suite) but unused.

# Paths are resolved from this Makefile's location, so the suite runs from any directory.
SUITE_DIR  := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
REPO_ROOT  := $(abspath $(SUITE_DIR)/../../..)
COMMON_DIR := $(REPO_ROOT)/TankAlarm-112025-Common/src
BUILD_DIR  ?= $(SUITE_DIR)/build

ARDUINOJSON_DIR ?=

CXX      ?= g++
CXXFLAGS := -std=gnu++14 -fno-rtti -funsigned-char -Wall -Wextra -Wno-missing-field-initializers
CPPFLAGS := -I$(COMMON_DIR)
# vptr checking needs RTTI, which the firmware flags turn off.
SANITIZE := -O1 -g -fsanitize=address,undefined -fno-sanitize=vptr -fno-sanitize-recover=all
OPTIMIZE := -Os -Werror

SRC  := $(SUITE_DIR)/opta_io_test.cpp
DEPS := $(SRC) $(COMMON_DIR)/TankAlarm_OptaIo.h

TEST_SAN := $(BUILD_DIR)/opta_io_test_san
TEST_OS  := $(BUILD_DIR)/opta_io_test_os

.PHONY: test all clean

all: $(TEST_SAN) $(TEST_OS)

test: $(TEST_SAN) $(TEST_OS)
	$(TEST_SAN)
	$(TEST_OS)

$(TEST_SAN): $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SANITIZE) $(CPPFLAGS) $(SRC) -o $@

$(TEST_OS): $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(OPTIMIZE) $(CPPFLAGS) $(SRC) -o $@

clean:
	rm -rf $(BUILD_DIR)
