# Host tests for TankAlarm-112025-Server-BluesOpta/WarmTierStore.h (S-D03).
#
#   make test ARDUINOJSON_DIR=/path/to/ArduinoJson/src    # ArduinoJson v7.4.3
#
# Builds the suite twice and runs both: with AddressSanitizer and
# UndefinedBehaviorSanitizer at -O1, and plain at -O2 (the optimizer finds
# different warnings, and the plain build runs the full truncation sweep).

CXX ?= g++
SERVER_DIR := ../../../TankAlarm-112025-Server-BluesOpta
BUILD := build

WARNINGS := -std=gnu++17 -Wall -Wextra -Werror
INCLUDES = -isystem $(ARDUINOJSON_DIR) -I$(SERVER_DIR)
SAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
             -D_GLIBCXX_ASSERTIONS -DWARM_TEST_EDGE_STRIDE=4
OPT_FLAGS := -O2

SOURCES := warm_store_test.cpp
HEADERS := warm_test_io.h legacy_v2215.h $(SERVER_DIR)/WarmTierStore.h

.PHONY: test clean check-arduinojson

test: $(BUILD)/warm_store_test_san $(BUILD)/warm_store_test_o2
	./$(BUILD)/warm_store_test_san
	./$(BUILD)/warm_store_test_o2

check-arduinojson:
	@[ -f "$(ARDUINOJSON_DIR)/ArduinoJson.h" ] || { echo "Set ARDUINOJSON_DIR to the src folder of ArduinoJson v7.4.3"; exit 1; }

$(BUILD)/warm_store_test_san: $(SOURCES) $(HEADERS) | check-arduinojson
	@mkdir -p $(BUILD)
	$(CXX) $(WARNINGS) $(SAN_FLAGS) $(INCLUDES) $(SOURCES) -o $@

$(BUILD)/warm_store_test_o2: $(SOURCES) $(HEADERS) | check-arduinojson
	@mkdir -p $(BUILD)
	$(CXX) $(WARNINGS) $(OPT_FLAGS) $(INCLUDES) $(SOURCES) -o $@

clean:
	rm -rf $(BUILD)
