# Host tests for the client's Clear Relay command decisions (S-T01, relay/float CL-4):
# TankAlarm-112025-Client-BluesOpta/TankAlarm_RelayCommand.h
#
#   make -C tests/host/relay_command test ARDUINOJSON_DIR=/path/to/ArduinoJson/src    # v7.4.3
#
# Builds the suite twice and runs both: with AddressSanitizer and UndefinedBehaviorSanitizer at
# -O1, and plain at -O2. Only the plain build uses -Werror (the sanitizers' instrumentation causes
# false-positive warnings). The header is compiled with the firmware's language and char flags
# (variants/OPTA/cxxflags.txt: gnu++14, no RTTI, unsigned char), so the test sees what the Opta sees.

# Paths are resolved from this Makefile's location, so the suite runs from any directory.
SUITE_DIR  := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
REPO_ROOT  := $(abspath $(SUITE_DIR)/../../..)
CLIENT_DIR := $(REPO_ROOT)/TankAlarm-112025-Client-BluesOpta
BUILD_DIR  ?= $(SUITE_DIR)/build

ARDUINOJSON_DIR ?=

CXX      ?= g++
CXXFLAGS := -std=gnu++14 -fno-rtti -funsigned-char -Wall -Wextra
CPPFLAGS  = -isystem $(ARDUINOJSON_DIR) -I$(CLIENT_DIR) -I$(SUITE_DIR) \
            -DCLIENT_SKETCH='"$(CLIENT_DIR)/TankAlarm-112025-Client-BluesOpta.ino"'
SANITIZE := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
OPTIMIZE := -O2 -Werror

SRC  := $(SUITE_DIR)/relay_command_test.cpp
DEPS := $(SRC) $(SUITE_DIR)/legacy_v2216_relay.h $(CLIENT_DIR)/TankAlarm_RelayCommand.h

TEST_SAN := $(BUILD_DIR)/relay_command_test_san
TEST_O2  := $(BUILD_DIR)/relay_command_test_o2

.PHONY: test all clean check-arduinojson

all: $(TEST_SAN) $(TEST_O2)

test: $(TEST_SAN) $(TEST_O2)
	$(TEST_SAN)
	$(TEST_O2)

check-arduinojson:
	@[ -f "$(ARDUINOJSON_DIR)/ArduinoJson.h" ] || { echo "Set ARDUINOJSON_DIR to the src folder of ArduinoJson v7.4.3"; exit 1; }

$(TEST_SAN): $(DEPS) | check-arduinojson
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SANITIZE) $(CPPFLAGS) $(SRC) -o $@

$(TEST_O2): $(DEPS) | check-arduinojson
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(OPTIMIZE) $(CPPFLAGS) $(SRC) -o $@

clean:
	rm -rf $(BUILD_DIR)
