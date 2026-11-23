# Host tests for the server's daily report first-part rule (S-D08):
# TankAlarm-112025-Server-BluesOpta/TankAlarm_DailyParts.h
#
#   make -C tests/host/daily_parts test ARDUINOJSON_DIR=/path/to/ArduinoJson/src    # ArduinoJson v7.4.3
#
# Builds the test twice, with ASan/UBSan at -O1 and plain at -O2, and runs both. Only the plain
# build uses -Werror (the sanitizers' instrumentation causes false-positive warnings). The
# header needs no ArduinoJson; the test uses it to read daily notes the way handleDaily() does.
# The test also reads the server and client sketches for the lines it mirrors.

# Paths are resolved from this Makefile's location, so the suite runs from any directory.
SUITE_DIR  := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
REPO_ROOT  := $(abspath $(SUITE_DIR)/../../..)
SERVER_DIR := $(REPO_ROOT)/TankAlarm-112025-Server-BluesOpta
CLIENT_DIR := $(REPO_ROOT)/TankAlarm-112025-Client-BluesOpta
BUILD_DIR  ?= $(SUITE_DIR)/build

ARDUINOJSON_DIR ?=

SERVER_SKETCH := $(SERVER_DIR)/TankAlarm-112025-Server-BluesOpta.ino
CLIENT_SKETCH := $(CLIENT_DIR)/TankAlarm-112025-Client-BluesOpta.ino

CXX      ?= g++
CXXFLAGS := -std=gnu++17 -Wall -Wextra
CPPFLAGS  = -isystem $(ARDUINOJSON_DIR) -I$(SERVER_DIR) \
            -DSERVER_SKETCH='"$(SERVER_SKETCH)"' -DCLIENT_SKETCH='"$(CLIENT_SKETCH)"'
SANITIZE := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
OPTIMIZE := -O2 -Werror

SRC  := $(SUITE_DIR)/daily_parts_test.cpp
DEPS := $(SRC) $(SERVER_DIR)/TankAlarm_DailyParts.h

TEST_SAN := $(BUILD_DIR)/daily_parts_test_san
TEST_O2  := $(BUILD_DIR)/daily_parts_test_o2

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
