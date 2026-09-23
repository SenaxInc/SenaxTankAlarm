# Host unit tests

These tests compile hardware-free firmware logic with the PC's C++ compiler and run it, so behaviour that is hard to reproduce on an Opta (millis() wrap-around, truncated files, allocation failure, long outages) is checked on every pull request.

Each suite lives in its own folder, `tests/host/<suite>/`, with a `Makefile` that has a `test` target. The `host-tests` job in `.github/workflows/arduino-ci-112025.yml` runs `make -C tests/host/<suite> test` for every suite and fails the workflow if any test fails. `build-firmware` waits for this job.

Suites that need ArduinoJson receive its `src` folder as `ARDUINOJSON_DIR`. CI clones ArduinoJson v7.4.3, the version the firmware is built with.

A suite may use another tool the Ubuntu runner provides. `email_bridge` runs the Google Apps Script email bridge from the server's `/email-setup` page under node (18 or later), with stubs for the Apps Script services; run it with `node tests/host/email_bridge/email_bridge_test.js`.

To run a suite locally (Linux, macOS or WSL, with g++ or clang++):

```bash
git clone --depth 1 --branch v7.4.3 https://github.com/bblanchon/ArduinoJson.git /tmp/ArduinoJson
make -C tests/host/<suite> test ARDUINOJSON_DIR=/tmp/ArduinoJson/src
```

C++ suites can test only headers with no Arduino or mbed dependencies: the code under test lives in small sketch-local headers that the sketch includes, so the tests exercise the same source the firmware compiles. Other suites test code embedded in the sketch directly; `email_bridge` extracts the Apps Script from the `/email-setup` page in the server sketch.
