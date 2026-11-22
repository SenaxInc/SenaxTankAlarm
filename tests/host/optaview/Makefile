# Host tests for OptaView, run against a simulated SunSaver MPPT instead of an RS-485 port and
# an Ethernet port. There are two suites: optaview_test.cpp (the Modbus core, the profile loader,
# the device layer and the console) and optaview_web_test.cpp (the HTTP reader and writer, the
# network address policy, the web server and its API).
#
#   make test ARDUINOJSON_DIR=/path/to/ArduinoJson/src    # ArduinoJson v7.4.3
#
# Lints the SunSaver profile, checks that the copies of the profile and of the web page that are
# embedded in the sketch are current, then builds each suite twice and runs both builds: with
# AddressSanitizer and UndefinedBehaviorSanitizer at -O1, and plain at -O2. Only the plain build
# uses -Werror: GCC advises against combining it with the sanitizers, whose instrumentation
# causes false-positive warnings. The one it raises here (-Wmaybe-uninitialized, inside
# ArduinoJson's iterators) is switched off for the sanitizer build; the plain build keeps it.
#
# The suites are compiled as C++14 because that is what the Opta's toolchain speaks, so a
# newer language feature fails here before it fails in the Arduino build.
#
# The web page without an Opta (OptaView/README.md has more on both):
#
#   make preview ARDUINOJSON_DIR=...         # the firmware's web server and console on this PC,
#                                            # with the simulated SunSaver: http://127.0.0.1:8080/
#   make browser-test ARDUINOJSON_DIR=...    # drives that page in Chromium; needs Node.js and
#                                            # Playwright (NODE_PATH names where it is installed)

CXX ?= g++
PYTHON ?= python3
OPTAVIEW_DIR := ../../../OptaView
BUILD := build

WARNINGS := -std=gnu++14 -Wall -Wextra
INCLUDES = -isystem $(ARDUINOJSON_DIR) -I$(OPTAVIEW_DIR)
SAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
             -D_GLIBCXX_ASSERTIONS -Wno-maybe-uninitialized
OPT_FLAGS := -O2 -Werror

NODE ?= node
PREVIEW_ARGS ?=
BROWSER_TEST_ARGS ?=

SOURCES := optaview_test.cpp
HEADERS := sim_sunsaver.h \
           test_support.h \
           $(OPTAVIEW_DIR)/OptaViewText.h \
           $(OPTAVIEW_DIR)/OptaViewModbus.h \
           $(OPTAVIEW_DIR)/OptaViewProfile.h \
           $(OPTAVIEW_DIR)/OptaViewDevice.h \
           $(OPTAVIEW_DIR)/OptaViewConsole.h \
           $(OPTAVIEW_DIR)/OptaViewBuiltinProfile.h
WEB_SOURCES := optaview_web_test.cpp
WEB_HEADERS := $(HEADERS) \
               $(OPTAVIEW_DIR)/OptaViewHttp.h \
               $(OPTAVIEW_DIR)/OptaViewNet.h \
               $(OPTAVIEW_DIR)/OptaViewWeb.h \
               $(OPTAVIEW_DIR)/OptaViewWebPage.h
PREVIEW_SOURCES := optaview_preview.cpp

.PHONY: test clean check-arduinojson check-profile check-page preview browser-test

test: check-profile check-page \
      $(BUILD)/optaview_test_san $(BUILD)/optaview_test_o2 \
      $(BUILD)/optaview_web_test_san $(BUILD)/optaview_web_test_o2
	./$(BUILD)/optaview_test_san
	./$(BUILD)/optaview_test_o2
	./$(BUILD)/optaview_web_test_san
	./$(BUILD)/optaview_web_test_o2

check-arduinojson:
	@[ -f "$(ARDUINOJSON_DIR)/ArduinoJson.h" ] || { echo "Set ARDUINOJSON_DIR to the src folder of ArduinoJson v7.4.3"; exit 1; }

check-profile:
	$(PYTHON) $(OPTAVIEW_DIR)/tools/check_profile.py
	$(PYTHON) $(OPTAVIEW_DIR)/tools/embed_profile.py --check

# web/index.html keeps to what its Content-Security-Policy allows, and OptaViewWebPage.h is it.
check-page:
	$(PYTHON) $(OPTAVIEW_DIR)/tools/embed_web.py --check

$(BUILD)/optaview_test_san: $(SOURCES) $(HEADERS) | check-arduinojson
	@mkdir -p $(BUILD)
	$(CXX) $(WARNINGS) $(SAN_FLAGS) $(INCLUDES) $(SOURCES) -o $@

$(BUILD)/optaview_test_o2: $(SOURCES) $(HEADERS) | check-arduinojson
	@mkdir -p $(BUILD)
	$(CXX) $(WARNINGS) $(OPT_FLAGS) $(INCLUDES) $(SOURCES) -o $@

$(BUILD)/optaview_web_test_san: $(WEB_SOURCES) $(WEB_HEADERS) | check-arduinojson
	@mkdir -p $(BUILD)
	$(CXX) $(WARNINGS) $(SAN_FLAGS) $(INCLUDES) $(WEB_SOURCES) -o $@

$(BUILD)/optaview_web_test_o2: $(WEB_SOURCES) $(WEB_HEADERS) | check-arduinojson
	@mkdir -p $(BUILD)
	$(CXX) $(WARNINGS) $(OPT_FLAGS) $(INCLUDES) $(WEB_SOURCES) -o $@

# The firmware's web server and console as a program for this PC (POSIX sockets), with the
# simulated SunSaver behind them.
$(BUILD)/optaview_preview: $(PREVIEW_SOURCES) $(WEB_HEADERS) | check-arduinojson
	@mkdir -p $(BUILD)
	$(CXX) $(WARNINGS) $(OPT_FLAGS) $(INCLUDES) $(PREVIEW_SOURCES) -o $@

preview: check-page $(BUILD)/optaview_preview
	./$(BUILD)/optaview_preview $(PREVIEW_ARGS)

browser-test: check-page $(BUILD)/optaview_preview
	$(NODE) browser/page_test.js $(BUILD)/optaview_preview $(BROWSER_TEST_ARGS)

clean:
	rm -rf $(BUILD)
