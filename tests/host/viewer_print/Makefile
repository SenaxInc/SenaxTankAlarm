# Host tests for the Viewer daily report value lines:
# TankAlarm-112025-Viewer-BluesOpta/TankAlarm_ViewerPrint.h
#
#   make -C tests/host/viewer_print test
#
# Builds the test twice, with ASan/UBSan at -O1 and plain at -O2, and runs both. The header
# needs no ArduinoJson; ARDUINOJSON_DIR is accepted (CI passes it to every suite) but unused.

# Paths are resolved from this Makefile's location, so the suite runs from any directory.
SUITE_DIR  := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
REPO_ROOT  := $(abspath $(SUITE_DIR)/../../..)
VIEWER_DIR := $(REPO_ROOT)/TankAlarm-112025-Viewer-BluesOpta
BUILD_DIR  ?= $(SUITE_DIR)/build

ARDUINOJSON_DIR ?=

CXX      ?= g++
CXXFLAGS := -std=gnu++17 -Wall -Wextra -Werror
CPPFLAGS := -I$(VIEWER_DIR)
SANITIZE := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
OPTIMIZE := -O2

SRC  := $(SUITE_DIR)/viewer_print_test.cpp
DEPS := $(SRC) $(VIEWER_DIR)/TankAlarm_ViewerPrint.h

TEST_SAN := $(BUILD_DIR)/viewer_print_test_san
TEST_O2  := $(BUILD_DIR)/viewer_print_test_o2

.PHONY: test all clean

all: $(TEST_SAN) $(TEST_O2)

test: $(TEST_SAN) $(TEST_O2)
	$(TEST_SAN)
	$(TEST_O2)

$(TEST_SAN): $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SANITIZE) $(CPPFLAGS) $(SRC) -o $@

$(TEST_O2): $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(OPTIMIZE) $(CPPFLAGS) $(SRC) -o $@

clean:
	rm -rf $(BUILD_DIR)
