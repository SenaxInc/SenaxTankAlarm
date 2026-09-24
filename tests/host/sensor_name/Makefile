# Host tests for how the server keeps a sensor's Display Number ("un") in step with telemetry and
# daily-report notes: TankAlarm-112025-Server-BluesOpta/TankAlarm_SensorName.h, plus pins on the
# sketch text that uses it.
#
#   make test
#
# Builds the suite twice and runs both: with AddressSanitizer and UndefinedBehaviorSanitizer at
# -O1, and plain at -O2. Only the plain build uses -Werror (the sanitizers' instrumentation causes
# false-positive warnings). The language flags are the Opta core's (gnu++14, no RTTI, unsigned
# char). ARDUINOJSON_DIR, which CI passes to every suite, is not used.

CXX ?= g++
SERVER_DIR := ../../../TankAlarm-112025-Server-BluesOpta
BUILD := build

WARNINGS := -std=gnu++14 -fno-rtti -funsigned-char -Wall -Wextra
INCLUDES = -I$(SERVER_DIR)
DEFINES = -DSERVER_SKETCH='"$(abspath $(SERVER_DIR))/TankAlarm-112025-Server-BluesOpta.ino"'
SAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
             -D_GLIBCXX_ASSERTIONS
OPT_FLAGS := -O2 -Werror

SOURCES := sensor_name_test.cpp
HEADERS := $(SERVER_DIR)/TankAlarm_SensorName.h

.PHONY: test clean

test: $(BUILD)/sensor_name_test_san $(BUILD)/sensor_name_test_o2
	./$(BUILD)/sensor_name_test_san
	./$(BUILD)/sensor_name_test_o2

$(BUILD)/sensor_name_test_san: $(SOURCES) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(WARNINGS) $(SAN_FLAGS) $(INCLUDES) $(DEFINES) $(SOURCES) -o $@

$(BUILD)/sensor_name_test_o2: $(SOURCES) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(WARNINGS) $(OPT_FLAGS) $(INCLUDES) $(DEFINES) $(SOURCES) -o $@

clean:
	rm -rf $(BUILD)
