# Host tests for stable sensor numbers (S-T01/S1):
#  - TankAlarm-112025-Server-BluesOpta/TankAlarm_SensorNumbers.h, the check handleConfigPost runs
#    before a client config is cached or sent (sensor_numbers_test.cpp)
#  - the numbering helpers of the server's Config Generator page (generator_numbers_test.js)
#
#   make test ARDUINOJSON_DIR=/path/to/ArduinoJson/src    # ArduinoJson v7.4.3; node 18 or later
#
# Builds the C++ suite twice and runs both: with AddressSanitizer and UndefinedBehaviorSanitizer
# at -O1, and plain at -O2. Only the plain build uses -Werror: GCC advises against combining it
# with the sanitizers, whose instrumentation causes false-positive warnings. The flags match the
# Opta build (gnu++14, no RTTI, unsigned char).

CXX ?= g++
NODE ?= node
SERVER_DIR := ../../../TankAlarm-112025-Server-BluesOpta
BUILD := build

WARNINGS := -std=gnu++14 -fno-rtti -funsigned-char -Wall -Wextra
INCLUDES = -isystem $(ARDUINOJSON_DIR) -I$(SERVER_DIR)
SAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
             -D_GLIBCXX_ASSERTIONS
OPT_FLAGS := -O2 -Werror

SOURCES := sensor_numbers_test.cpp
HEADERS := $(SERVER_DIR)/TankAlarm_SensorNumbers.h

.PHONY: test clean check-arduinojson

test: $(BUILD)/sensor_numbers_test_san $(BUILD)/sensor_numbers_test_o2
	./$(BUILD)/sensor_numbers_test_san
	./$(BUILD)/sensor_numbers_test_o2
	$(NODE) generator_numbers_test.js

check-arduinojson:
	@[ -f "$(ARDUINOJSON_DIR)/ArduinoJson.h" ] || { echo "Set ARDUINOJSON_DIR to the src folder of ArduinoJson v7.4.3"; exit 1; }

$(BUILD)/sensor_numbers_test_san: $(SOURCES) $(HEADERS) | check-arduinojson
	@mkdir -p $(BUILD)
	$(CXX) $(WARNINGS) $(SAN_FLAGS) $(INCLUDES) $(SOURCES) -o $@

$(BUILD)/sensor_numbers_test_o2: $(SOURCES) $(HEADERS) | check-arduinojson
	@mkdir -p $(BUILD)
	$(CXX) $(WARNINGS) $(OPT_FLAGS) $(INCLUDES) $(SOURCES) -o $@

clean:
	rm -rf $(BUILD)
