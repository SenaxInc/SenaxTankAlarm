# Host tests for OptaView (the Modbus core, the profile loader, the device layer and the
# console), run against a simulated SunSaver MPPT instead of an RS-485 port.
#
#   make test ARDUINOJSON_DIR=/path/to/ArduinoJson/src    # ArduinoJson v7.4.3
#
# Lints the SunSaver profile and checks that the copy embedded in the sketch is current,
# then builds the suite twice and runs both: with AddressSanitizer and
# UndefinedBehaviorSanitizer at -O1, and plain at -O2. Only the plain build uses -Werror:
# GCC advises against combining it with the sanitizers, whose instrumentation causes
# false-positive warnings. The one it raises here (-Wmaybe-uninitialized, inside ArduinoJson's
# iterators) is switched off for the sanitizer build; the plain build keeps it.
#
# The suite is compiled as C++14 because that is what the Opta's toolchain speaks, so a
# newer language feature fails here before it fails in the Arduino build.

CXX ?= g++
PYTHON ?= python3
OPTAVIEW_DIR := ../../../OptaView
BUILD := build

WARNINGS := -std=gnu++14 -Wall -Wextra
INCLUDES = -isystem $(ARDUINOJSON_DIR) -I$(OPTAVIEW_DIR)
SAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
             -D_GLIBCXX_ASSERTIONS -Wno-maybe-uninitialized
OPT_FLAGS := -O2 -Werror

SOURCES := optaview_test.cpp
HEADERS := sim_sunsaver.h \
           $(OPTAVIEW_DIR)/OptaViewText.h \
           $(OPTAVIEW_DIR)/OptaViewModbus.h \
           $(OPTAVIEW_DIR)/OptaViewProfile.h \
           $(OPTAVIEW_DIR)/OptaViewDevice.h \
           $(OPTAVIEW_DIR)/OptaViewConsole.h \
           $(OPTAVIEW_DIR)/OptaViewBuiltinProfile.h

.PHONY: test clean check-arduinojson check-profile

test: check-profile $(BUILD)/optaview_test_san $(BUILD)/optaview_test_o2
	./$(BUILD)/optaview_test_san
	./$(BUILD)/optaview_test_o2

check-arduinojson:
	@[ -f "$(ARDUINOJSON_DIR)/ArduinoJson.h" ] || { echo "Set ARDUINOJSON_DIR to the src folder of ArduinoJson v7.4.3"; exit 1; }

check-profile:
	$(PYTHON) $(OPTAVIEW_DIR)/tools/check_profile.py
	$(PYTHON) $(OPTAVIEW_DIR)/tools/embed_profile.py --check

$(BUILD)/optaview_test_san: $(SOURCES) $(HEADERS) | check-arduinojson
	@mkdir -p $(BUILD)
	$(CXX) $(WARNINGS) $(SAN_FLAGS) $(INCLUDES) $(SOURCES) -o $@

$(BUILD)/optaview_test_o2: $(SOURCES) $(HEADERS) | check-arduinojson
	@mkdir -p $(BUILD)
	$(CXX) $(WARNINGS) $(OPT_FLAGS) $(INCLUDES) $(SOURCES) -o $@

clean:
	rm -rf $(BUILD)
