# Host unit tests

These tests compile hardware-free firmware logic with the PC's C++ compiler and run it, so behaviour that is hard to reproduce on an Opta (millis() wrap-around, truncated files, allocation failure, long outages) is checked on every pull request.

Each suite lives in its own folder, `tests/host/<suite>/`, with a `Makefile` that has a `test` target. The `host-tests` job in `.github/workflows/arduino-ci-112025.yml` runs `make -C tests/host/<suite> test` for every suite and fails the workflow if any test fails. `build-firmware` waits for this job.

Suites that need ArduinoJson receive its `src` folder as `ARDUINOJSON_DIR`. The host tests pin ArduinoJson v7.4.3, which CI clones; the firmware jobs install the latest release.

A suite may use another tool the Ubuntu runner provides. `email_bridge` runs the Google Apps Script email bridge from the server's `/email-setup` page under node (18 or later), with stubs for the Apps Script services; run it with `node tests/host/email_bridge/email_bridge_test.js`. `optaview` runs the OptaView headers against a simulated SunSaver MPPT in two test programs, one for the Modbus core and the console and one for the web server, and first uses python3 to lint the register profile and to check that the copies of the profile and of the web page that are embedded in the sketch are current. Its `browser-test` target drives OptaView's web page in Chromium and needs Node.js and Playwright, so it is not part of `test`: the `optaview-page` job of the same workflow runs it (see `OptaView/README.md`).

`sunsaver_settings` tests the charger check that the client runs on its SunSaver MPPT: `TankAlarm_SunSaverSettings.h` (which registers it reads, how it tells the encoding of the voltages, and whether the settings suit the battery) and `TankAlarm_SunSaverReport.h` (the report the client sends and the server shows, and which of two reports the server keeps), both in `TankAlarm-112025-Common/src`. It runs them against the simulated SunSaver of the `optaview` suite, through OptaView's Modbus master. The client and OptaView do not share code, so this suite also compares the client's register table with OptaView's profile, entry by entry, and runs the two scale tests side by side on the same words.

`sunsaver_program` tests charger programming, which writes a LiFePO4 charging profile and the LVD and LVR into the SunSaver when a client's config asks for it: `TankAlarm_SunSaverProgram.h` (the limits of what may be asked for, which registers a run sets and in which order, the run that restarts the controller, writes and reads back, and the supervisor that decides when a request runs and keeps the client's journal) and the part of `TankAlarm_SunSaverReport.h` that carries a request, a result and the journal as JSON. It runs them against the same simulated SunSaver, which raises its "EEPROM setting edit" fault on a write until it restarts, and which can be made to refuse writes or a restart. Besides the single cases it goes through every request of a run: one frame lost or damaged at each of them, a register that does not take its word, a restart that does not come, and the client losing its power before each request and each journal write, with and without the controller losing its power too. It runs whole runs with the shortest, the default and the longest Modbus timeout a config allows, with replies that come at once or just in time, and with the line going dead at each request in turn, and checks that no run takes longer than a minute and that none writes once it nears that limit. After each it checks that the controller is left no worse for the battery than it was found, that the result says what the controller holds, and that a client that starts again settles what the run left open.

To run a suite locally (Linux, macOS or WSL, with g++ or clang++):

```bash
git clone --depth 1 --branch v7.4.3 https://github.com/bblanchon/ArduinoJson.git /tmp/ArduinoJson
make -C tests/host/<suite> test ARDUINOJSON_DIR=/tmp/ArduinoJson/src
```

C++ suites can test only headers with no Arduino or mbed dependencies: the code under test lives in small sketch-local headers that the sketch includes, so the tests exercise the same source the firmware compiles. Other suites test code embedded in the sketch directly; `email_bridge` extracts the Apps Script from the `/email-setup` page in the server sketch.
