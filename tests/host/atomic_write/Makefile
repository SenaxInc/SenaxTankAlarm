# Host tests for the atomic file writer that every saved file of the client and the server goes
# through: tankalarm_posix_write_file_atomic() in TankAlarm-112025-Common/src/TankAlarm_Platform.h
#
#   make -C tests/host/atomic_write test
#
# Builds the test twice, with ASan/UBSan at -O1 and plain at -O2, and runs both. The header
# needs no ArduinoJson; ARDUINOJSON_DIR is accepted (CI passes it to every suite) but unused.
# stub/Arduino.h stands in for the little of Arduino.h that the header uses.

# Paths are resolved from this Makefile's location, so the suite runs from any directory.
SUITE_DIR  := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
REPO_ROOT  := $(abspath $(SUITE_DIR)/../../..)
COMMON_DIR := $(REPO_ROOT)/TankAlarm-112025-Common/src
BUILD_DIR  ?= $(SUITE_DIR)/build

ARDUINOJSON_DIR ?=

CXX      ?= g++
CXXFLAGS := -std=gnu++17 -Wall -Wextra -Werror
CPPFLAGS := -I$(SUITE_DIR)/stub -I$(COMMON_DIR)
SANITIZE := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
OPTIMIZE := -O2

SRC  := $(SUITE_DIR)/atomic_write_test.cpp
DEPS := $(SRC) $(SUITE_DIR)/stub/Arduino.h $(COMMON_DIR)/TankAlarm_Platform.h

TEST_SAN := $(BUILD_DIR)/atomic_write_test_san
TEST_O2  := $(BUILD_DIR)/atomic_write_test_o2

.PHONY: test all clean

all: $(TEST_SAN) $(TEST_O2)

# Each run writes its files into a folder of its own under the build folder.
test: $(TEST_SAN) $(TEST_O2)
	$(TEST_SAN) $(BUILD_DIR)/files_san
	$(TEST_O2) $(BUILD_DIR)/files_o2

$(TEST_SAN): $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SANITIZE) $(CPPFLAGS) $(SRC) -o $@

$(TEST_O2): $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(OPTIMIZE) $(CPPFLAGS) $(SRC) -o $@

clean:
	rm -rf $(BUILD_DIR)
