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
- Modbus register definitions for SunSaver MPPT
- Battery voltage thresholds for 12V AGM systems
- Fault and alarm bitfield parsing
- Charge state descriptions (Night, Bulk, Absorption, Float, etc.)

**Hardware Required:**
- Arduino Opta with RS485 (WiFi/RS485 model or RS485 shield)
- Morningstar MRC-1 (MeterBus to EIA-485 Adapter)
- SunSaver MPPT solar charge controller

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
