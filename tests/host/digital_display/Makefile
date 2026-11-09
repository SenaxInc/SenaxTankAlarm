# Host tests for how the server shows digital (float switch) sensors (S3, C-A04 B4-B8):
# TankAlarm-112025-Server-BluesOpta/TankAlarm_DigitalDisplay.h and the viewer's sensor card.
#
#   make test ARDUINOJSON_DIR=/path/to/ArduinoJson/src    # ArduinoJson v7.4.3
#
# Builds the C++ suite twice and runs both: with AddressSanitizer and
# UndefinedBehaviorSanitizer at -O1, and plain at -O2. Only the plain build uses
# -Werror (the sanitizers' instrumentation causes false-positive warnings). The
# language flags are the Opta core's (gnu++14, no RTTI, unsigned char). Then runs
# viewer_cards_test.js under node (18 or later).

CXX ?= g++
NODE ?= node
SERVER_DIR := ../../../TankAlarm-112025-Server-BluesOpta
BUILD := build

WARNINGS := -std=gnu++14 -fno-rtti -funsigned-char -Wall -Wextra
INCLUDES = -isystem $(ARDUINOJSON_DIR) -I$(SERVER_DIR)
DEFINES = -DSERVER_SKETCH='"$(abspath $(SERVER_DIR))/TankAlarm-112025-Server-BluesOpta.ino"'
SAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
             -D_GLIBCXX_ASSERTIONS
OPT_FLAGS := -O2 -Werror

SOURCES := digital_display_test.cpp
HEADERS := $(SERVER_DIR)/TankAlarm_DigitalDisplay.h

.PHONY: test clean check-arduinojson

test: $(BUILD)/digital_display_test_san $(BUILD)/digital_display_test_o2
	./$(BUILD)/digital_display_test_san
	./$(BUILD)/digital_display_test_o2
	$(NODE) viewer_cards_test.js

check-arduinojson:
	@[ -f "$(ARDUINOJSON_DIR)/ArduinoJson.h" ] || { echo "Set ARDUINOJSON_DIR to the src folder of ArduinoJson v7.4.3"; exit 1; }

$(BUILD)/digital_display_test_san: $(SOURCES) $(HEADERS) | check-arduinojson
	@mkdir -p $(BUILD)
	$(CXX) $(WARNINGS) $(SAN_FLAGS) $(INCLUDES) $(DEFINES) $(SOURCES) -o $@

$(BUILD)/digital_display_test_o2: $(SOURCES) $(HEADERS) | check-arduinojson
	@mkdir -p $(BUILD)
	$(CXX) $(WARNINGS) $(OPT_FLAGS) $(INCLUDES) $(DEFINES) $(SOURCES) -o $@

clean:
	rm -rf $(BUILD)
