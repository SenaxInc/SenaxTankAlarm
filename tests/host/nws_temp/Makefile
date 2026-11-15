# Host tests for TankAlarm-112025-Server-BluesOpta/NwsTemperature.h (outdoor
# temperature for the VIN Voltage History chart).
#
#   make test ARDUINOJSON_DIR=/path/to/ArduinoJson/src    # ArduinoJson v7.4.3
#
# Builds the suite twice and runs both: with AddressSanitizer and
# UndefinedBehaviorSanitizer at -O1, and plain at -O2 with -Werror (see
# tests/host/warm_store/Makefile for why only the plain build uses -Werror).

CXX ?= g++
SERVER_DIR := ../../../TankAlarm-112025-Server-BluesOpta
BUILD := build

WARNINGS := -std=gnu++17 -Wall -Wextra
INCLUDES = -isystem $(ARDUINOJSON_DIR) -I$(SERVER_DIR)
SAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
             -D_GLIBCXX_ASSERTIONS
OPT_FLAGS := -O2 -Werror

SOURCES := nws_temp_test.cpp
HEADERS := $(SERVER_DIR)/NwsTemperature.h

.PHONY: test clean check-arduinojson

test: $(BUILD)/nws_temp_test_san $(BUILD)/nws_temp_test_o2
	./$(BUILD)/nws_temp_test_san
	./$(BUILD)/nws_temp_test_o2

check-arduinojson:
	@[ -f "$(ARDUINOJSON_DIR)/ArduinoJson.h" ] || { echo "Set ARDUINOJSON_DIR to the src folder of ArduinoJson v7.4.3"; exit 1; }

$(BUILD)/nws_temp_test_san: $(SOURCES) $(HEADERS) | check-arduinojson
	@mkdir -p $(BUILD)
	$(CXX) $(WARNINGS) $(SAN_FLAGS) $(INCLUDES) $(SOURCES) -o $@

$(BUILD)/nws_temp_test_o2: $(SOURCES) $(HEADERS) | check-arduinojson
	@mkdir -p $(BUILD)
	$(CXX) $(WARNINGS) $(OPT_FLAGS) $(INCLUDES) $(SOURCES) -o $@

clean:
	rm -rf $(BUILD)
