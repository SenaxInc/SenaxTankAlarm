# Host tests for the server's late/stale reading helpers (daily email and reminders):
# TankAlarm-112025-Server-BluesOpta/EmailStaleness.h
#
#   make -C tests/host/email_staleness test
#
# Builds the test twice, with ASan/UBSan at -O1 and plain at -O2, and runs both. The header
# needs no ArduinoJson; ARDUINOJSON_DIR is accepted (CI passes it to every suite) but unused.
# The test also reads the server sketch to check that sendDailyEmail() and the alarm
# reminders use the helpers.

# Paths are resolved from this Makefile's location, so the suite runs from any directory.
SUITE_DIR  := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
REPO_ROOT  := $(abspath $(SUITE_DIR)/../../..)
SERVER_DIR := $(REPO_ROOT)/TankAlarm-112025-Server-BluesOpta
BUILD_DIR  ?= $(SUITE_DIR)/build

ARDUINOJSON_DIR ?=

CXX      ?= g++
# The Opta builds with gnu++14 and plain char unsigned; test the header the same way.
CXXFLAGS := -std=gnu++14 -funsigned-char -Wall -Wextra -Werror
CPPFLAGS := -I$(SERVER_DIR) -DSERVER_SKETCH_PATH='"$(SERVER_DIR)/TankAlarm-112025-Server-BluesOpta.ino"'
SANITIZE := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
OPTIMIZE := -O2

SRC  := $(SUITE_DIR)/email_staleness_test.cpp
DEPS := $(SRC) $(SERVER_DIR)/EmailStaleness.h

TEST_SAN := $(BUILD_DIR)/email_staleness_test_san
TEST_O2  := $(BUILD_DIR)/email_staleness_test_o2

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
