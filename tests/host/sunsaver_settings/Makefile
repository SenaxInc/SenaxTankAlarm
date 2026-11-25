# Host tests for the charger check that the client runs on its SunSaver MPPT:
# TankAlarm-112025-Common/src/TankAlarm_SunSaverSettings.h (what the controller has stored and
# whether it suits the battery) and TankAlarm_SunSaverReport.h (the same as JSON).
#
#   make test ARDUINOJSON_DIR=/path/to/ArduinoJson/src    # ArduinoJson v7.4.3
#
# The tests run against the simulated SunSaver MPPT of the OptaView suite (../optaview), through
# OptaView's Modbus master, and compare the client's register table and scale test with
# OptaView's profile and scale test. Builds the test twice and runs both builds: with
# AddressSanitizer and UndefinedBehaviorSanitizer at -O1, and plain at -O2 with -Werror (see
# ../optaview/Makefile for why only the plain build has it).
#
# The suite is compiled as C++14 because that is what the Opta's toolchain speaks.

# Paths are resolved from this Makefile's location, so the suite runs from any directory.
SUITE_DIR    := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
REPO_ROOT    := $(abspath $(SUITE_DIR)/../../..)
COMMON_DIR   := $(REPO_ROOT)/TankAlarm-112025-Common/src
OPTAVIEW_DIR := $(REPO_ROOT)/OptaView
SIM_DIR      := $(REPO_ROOT)/tests/host/optaview
BUILD_DIR    ?= $(SUITE_DIR)/build

CXX ?= g++

WARNINGS := -std=gnu++14 -Wall -Wextra
INCLUDES  = -isystem $(ARDUINOJSON_DIR) -I$(COMMON_DIR) -I$(OPTAVIEW_DIR) -I$(SIM_DIR)
SAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
             -D_GLIBCXX_ASSERTIONS -Wno-maybe-uninitialized
OPT_FLAGS := -O2 -Werror

SRC  := $(SUITE_DIR)/sunsaver_settings_test.cpp
# Every header the test includes, directly or through another header, outside ArduinoJson.
DEPS := $(SRC) \
        $(COMMON_DIR)/TankAlarm_SunSaverSettings.h \
        $(COMMON_DIR)/TankAlarm_SunSaverProgram.h \
        $(COMMON_DIR)/TankAlarm_SunSaverReport.h \
        $(COMMON_DIR)/TankAlarm_Battery.h \
        $(SIM_DIR)/sim_sunsaver.h \
        $(SIM_DIR)/test_support.h \
        $(OPTAVIEW_DIR)/OptaViewText.h \
        $(OPTAVIEW_DIR)/OptaViewModbus.h \
        $(OPTAVIEW_DIR)/OptaViewProfile.h \
        $(OPTAVIEW_DIR)/OptaViewDevice.h \
        $(OPTAVIEW_DIR)/OptaViewConsole.h \
        $(OPTAVIEW_DIR)/OptaViewBuiltinProfile.h

TEST_SAN := $(BUILD_DIR)/sunsaver_settings_test_san
TEST_O2  := $(BUILD_DIR)/sunsaver_settings_test_o2

.PHONY: test clean check-arduinojson

test: $(TEST_SAN) $(TEST_O2)
	$(TEST_SAN)
	$(TEST_O2)

check-arduinojson:
	@[ -f "$(ARDUINOJSON_DIR)/ArduinoJson.h" ] || { echo "Set ARDUINOJSON_DIR to the src folder of ArduinoJson v7.4.3"; exit 1; }

$(TEST_SAN): $(DEPS) | check-arduinojson
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(WARNINGS) $(SAN_FLAGS) $(INCLUDES) $(SRC) -o $@

$(TEST_O2): $(DEPS) | check-arduinojson
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(WARNINGS) $(OPT_FLAGS) $(INCLUDES) $(SRC) -o $@

clean:
	rm -rf $(BUILD_DIR)
