# TankAlarm-112025-Common

Shared library for TankAlarm 112025 Server, Client, and Viewer components.

## Overview

This library provides common constants, utility functions, platform abstractions, and Notecard helpers that are shared across all TankAlarm 112025 components.

## Installation

### Option 1: Symlink (Recommended for Development)

Create a symlink in your Arduino libraries folder:

**Windows (PowerShell as Admin):**
```powershell
New-Item -ItemType Junction -Path "$env:USERPROFILE\Documents\Arduino\libraries\TankAlarm-112025-Common" -Target "C:\path\to\SenaxTankAlarm\TankAlarm-112025-Common"
```

**Linux/macOS:**
```bash
ln -s /path/to/SenaxTankAlarm/TankAlarm-112025-Common ~/Arduino/libraries/TankAlarm-112025-Common
```

### Option 2: Copy

Copy the entire `TankAlarm-112025-Common` folder to your Arduino libraries folder:
- Windows: `%USERPROFILE%\Documents\Arduino\libraries\`
- Linux: `~/Arduino/libraries/`
- macOS: `~/Documents/Arduino/libraries/`

## Usage

Include the main header in your sketch:

```cpp
#include <TankAlarm_Common.h>
```

Or include specific headers:

```cpp
#include <TankAlarm_Platform.h>   // Platform detection and abstractions
#include <TankAlarm_Utils.h>      // Utility functions
#include <TankAlarm_Notecard.h>   // Notecard helper functions
```

## Components

### TankAlarm_Common.h
Main header that includes all other headers. Also defines:
- `FIRMWARE_VERSION` - Version string
- `FIRMWARE_BUILD_DATE` - Build date
- `NOTECARD_I2C_ADDRESS` - Default 0x17
- `NOTECARD_I2C_FREQUENCY` - Default 400kHz
- `MAX_TANK_RECORDS` - Default 64
- `WATCHDOG_TIMEOUT_SECONDS` - Default 30
- Notefile names (TELEMETRY_FILE, ALARM_FILE, etc.)

### TankAlarm_Platform.h
Platform detection and abstractions:
- PROGMEM compatibility for non-AVR platforms
- Filesystem availability detection
- Watchdog macros (`TANKALARM_WATCHDOG_KICK`, `TANKALARM_WATCHDOG_START`)
- POSIX file I/O helpers (Mbed OS only)
- `MbedWatchdogHelper` class

### TankAlarm_Utils.h
Utility functions:
- `strlcpy()` - Safe string copy (for platforms without it)
- `tankalarm_roundTo()` - Round float to decimal places
- `tankalarm_computeNextAlignedEpoch()` - Schedule aligned tasks

### TankAlarm_Notecard.h
Notecard helper functions:
- `tankalarm_ensureTimeSync()` - Sync time from Notecard
- `tankalarm_currentEpoch()` - Get current epoch from last sync

### TankAlarm_Solar.h
SunSaver MPPT solar charger monitoring via Modbus RTU over RS-485:
- `SolarConfig` - Configuration structure (slave ID, baud rate, thresholds)
- `SolarData` - Data structure with all readings and status
- `SolarManager` - Main class for polling and health assessment
- `SolarManager::lineResting()` - True for one Modbus timeout (the one that request had) after a request that got no usable reply: the poll, the charger check and the startup probe all keep off the line meanwhile, so that a late reply is not taken for the answer to the next request
- Modbus register definitions for SunSaver MPPT
- Battery voltage thresholds for 12V AGM systems
- Fault and alarm bitfield parsing
- Charge state descriptions (Night, Bulk, Absorption, Float, etc.)

**Hardware Required:**
- Arduino Opta with RS485 (WiFi/RS485 model or RS485 shield)
- Morningstar MRC-1 (MeterBus to EIA-485 Adapter) — **power switch must be ON** (LED is solid
  green either way; switch OFF = RS-485 side inert)
- SunSaver MPPT solar charge controller

### TankAlarm_SunSaverSettings.h
The charger check: what a SunSaver MPPT has stored, and whether it suits the battery. Included by `TankAlarm_Solar.h`. It has no Arduino dependency (it includes only `TankAlarm_Battery.h` of this library), so `tests/host/sunsaver_settings` runs it on a PC.
- The registers the check reads, in a fixed order: the custom charge settings of both banks, LVD and LVR, the LED thresholds, and the status words that say how they are used (DIP switches, faults, the LVD in effect)
- `SnapshotReader` - Reads them a few at a time through a `Reader`, so that `loop()` keeps running
- `judgeSettingsScale()`, `judgeLiveScale()` - How the controller encodes its voltages (x100 or x96.667), told from the stored setpoints and from the controller's own LVD
- `evaluate()` - Findings and a verdict for a battery chemistry. Each register is judged by itself: one that did not answer leaves the judgement open, and takes nothing from one that did answer and is wrong
- `chemistryForBatteryType()` - The chemistry a `BatteryType` of the client config is judged as. The client judges against it, and the server uses it to tell a report about a battery that is no longer configured
- `systemVolts()` - 12 or 24 V from the battery voltage, only when both scales give the same system
- `floatReferenceVolts()` - Where the charger holds a full battery, for the no-charge alert
- `SolarManager::startChargerCheck()` and `serviceChargerCheck()` in `TankAlarm_Solar.h` run it on the Opta. It only reads.

### TankAlarm_SunSaverProgram.h
Charger programming: writes a LiFePO4 charging profile, the LVD and the LVR into a SunSaver MPPT when a client's config asks for it. It builds on `TankAlarm_SunSaverSettings.h`, is included by `TankAlarm_Solar.h`, and has no Arduino dependency either, so `tests/host/sunsaver_program` runs it on a PC against a simulated SunSaver.
- `Profile`, `lifepo4Profile()`, `validateProfile()` - What can be asked for: absorption and float voltage, absorption time, HVD and HVR, LVD and LVR, for a 12 V battery. Each value is held to a fixed range and the values to their order (LVD below LVR, float below absorption, absorption below HVD), and voltages to steps of 0.05 V. The limits are in this file and nowhere else, so no config can widen them
- `buildPlan()` - The 17 registers a run sets, each with the word it has to hold, in the order of writing: equalization off; the limits on the charge voltage (the HVD, the regulation ceiling) and the charge voltages of both banks that come down or stay; temperature compensation off; the charge voltages that go up; the limits that go up; then the LVD's load current compensation, the LVD and the LVR. So wherever a run stops, at any battery temperature, each charge voltage in effect is no higher than the higher of the one found and the one asked for, and the LVD in effect no lower than the lower of the two. A register that already holds its word is not written
- `Programmer` - One run. It reads the charger check's registers and refuses to write on anything it relies on being missing or wrong (the `Refusal` bits: a voltage encoding it cannot prove, a fault, a battery below the new LVR, current on the LOAD terminals that the request did not allow, and more). Then it restarts the controller before it writes anything, and writes only once the controller has shown that it restarted (it fell silent and answers again, or a pending edit it reported has cleared). It writes one register at a time with function 6 and reads each one back, restarts the controller again so that the settings take effect, and reads them once more (the `Outcome`). A run takes about 10 seconds and never longer than `RUN_LIMIT_MS`, a minute, whatever the Modbus timeout: near that limit it writes nothing more and ends the shortest safe way. It starts no request that could end past the limit, counting what ArduinoModbus may add to a timeout (`Bus::requestLimitMs()`, `modbusRequestLimitMs()`: twice the timeout and more), and once it has written it keeps the time for its last restart command
- `Supervisor` - When a run may begin. A request waits up to an hour for the client's clock and the charger, is refused when it is more than a day old by then, and each request runs once. A journal in the client's flash records the request that waits, how far a run came, which controller it talked to and the Modbus function it read it with, and the result that has yet to go to the server. So a client that restarts loses nothing: it gives that controller the restart it may still owe, also when the config names another controller now or has the charger off, and answers a request that waited as interrupted
- `FunctionChoice` - The Modbus function a run reads its controller with: the one the poll found, or the one in the journal for the controller a restart is owed to. When neither is known it finds out: function 3, and function 4 for a register the controller refuses that way (`readLimitMs()`: such a read may take two requests)
- `LineTurns` - Who has the line in a pass of the client's loop: after a poll, the charger check or programming get the next pass in which the line is free, also when the line has to rest first after a request that got no reply
- `SolarManager::beginChargerProgramming()`, `requestChargerProgram()`, `withdrawChargerProgram()` and `serviceChargerProgramming()` in `TankAlarm_Solar.h` run it on the Opta, through a port that leaves the line silent for the Modbus frame gap (3.5 characters) before each request of a run, since ArduinoModbus keeps none
- The procedure is compiled in only with `TANKALARM_SUNSAVER_PROGRAMMING` set to 1 (default 0, in `TankAlarm_Solar.h`). A client built with 0 answers every request with "not built" and never writes to the controller

### TankAlarm_SunSaverReport.h
The charger check and charger programming as JSON. It needs ArduinoJson, so the sketches include it themselves (`TankAlarm_Common.h` does not).
- `encodeReport()` - The compact report a client sends to the server (`chg-check` on `config_ack.qo`)
- `decodeReport()` - Reads it back; a report from a newer or an older client still decodes
- `reportCheckTime()`, `reportReplaces()` - Which report the server keeps: the newest by the time of the check, since a report can arrive late
- `describeReport()` - Verdict, findings and every register with its value as text, for the server's `/charger` page
- `readProfile()`, `readLoadAllowed()`, `readRequestId()` - A programming request as a config carries it (`solarCharger.program`)
- `encodeProgramResult()`, `decodeProgramResult()` - The result of a run, as the client sends it (`chg-prog` on `config_ack.qo`) and the server keeps it
- `programResultReplaces()` - Which result the server keeps: that of the newest request, and the latest result of that request
- `describeProgramResult()` - How a run went, why nothing was written, and every register before and after, for the server's `/charger` page
- `encodeJournal()`, `decodeJournal()` - The client's journal of a run, in its flash

### TankAlarm_Battery.h
Battery voltage monitoring sourced from the SunSaver MPPT (Modbus RS-485) or an external analog Vin voltage divider — auto-selected by the client's `getEffectiveBatteryVoltage()` with strict MPPT > Vin-divider priority. The Notecard `card.voltage` API is **not** used on the Blues "Wireless for Opta" carrier (the Notecard V+ is the regulated ~5V DC-DC rail, not the battery).

- `BatteryType` - Enum (LEAD_ACID_12V, LIFEPO4_12V, LIPO, CUSTOM)
- `BatteryAlertType` - Alert levels (NONE, LOW, CRITICAL, HIGH, DECLINING, USB_LOST, RECOVERED)
- `BatteryData` - Voltage from the active source, source identifier (`mode` = "mppt" | "vin-divider"), running min/max stats, validity flag
- `BatteryConfig` - Thresholds, poll interval, alert enable
- `initBatteryConfig()` - Initialize config with defaults for battery type
- `getBatteryStateDescription()` - User-friendly battery state description

**Features:**
- Per-boot running min/max voltage included in the daily report
- Low / critical / high / recovery alerts (suppressed when MPPT is the active source so the
  alert path does not duplicate `solarCharger.alertOnLowBattery`)
- Daily-report battery section tagged with the active source (`battery.src`)

**Not currently implemented in firmware:**
- Trend analysis (daily/weekly/monthly voltage changes — the Notecard-provided trend window
  was removed with Fix 11; struct fields remain reserved but always read 0)
- State-of-charge estimation
- USB power fallback detection

**Hardware Required (at least one of):**
- SunSaver MPPT solar charge controller wired through a Morningstar MRC-1 (MeterBus to EIA-485 Adapter) for the RS-485 source
- External voltage divider (R1 high-side, R2 low-side) on an Opta analog input for the Vin divider source

With neither source enabled, battery voltage is unknown and battery alerts / daily-report battery section are skipped.

## Example

```cpp
#include <TankAlarm_Common.h>
#include <Wire.h>
#include <Notecard.h>

Notecard notecard;
double gLastSyncedEpoch = 0.0;
unsigned long gLastSyncMillis = 0;

#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
MbedWatchdogHelper mbedWatchdog;
#endif

void setup() {
  Serial.begin(115200);
  
  Wire.begin();
  Wire.setClock(NOTECARD_I2C_FREQUENCY);
  
  notecard.begin(NOTECARD_I2C_ADDRESS);
  
  // Sync time
  tankalarm_ensureTimeSync(notecard, gLastSyncedEpoch, gLastSyncMillis, true);
  
  // Start watchdog
  #ifdef TANKALARM_WATCHDOG_AVAILABLE
    TANKALARM_WATCHDOG_START(mbedWatchdog, WATCHDOG_TIMEOUT_SECONDS * 1000);
  #endif
}

void loop() {
  // Periodic time sync
  tankalarm_ensureTimeSync(notecard, gLastSyncedEpoch, gLastSyncMillis);
  
  // Get current time
  double now = tankalarm_currentEpoch(gLastSyncedEpoch, gLastSyncMillis);
  
  // Kick watchdog
  #ifdef TANKALARM_WATCHDOG_AVAILABLE
    TANKALARM_WATCHDOG_KICK(mbedWatchdog);
  #endif
  
  delay(1000);
}
```

## Dependencies

- [Blues Wireless Notecard](https://github.com/blues/note-arduino) - Notecard Arduino library
- [ArduinoRS485](https://github.com/arduino-libraries/ArduinoRS485) - RS-485 hardware interface (for solar monitoring)
- [ArduinoModbus](https://github.com/arduino-libraries/ArduinoModbus) - Modbus protocol (for solar monitoring)

## License

Copyright (c) 2025-2026 Senax Inc. All rights reserved.
