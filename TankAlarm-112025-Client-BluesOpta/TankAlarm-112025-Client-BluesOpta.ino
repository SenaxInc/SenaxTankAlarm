/*
  Tank Alarm Client 112025 - Arduino Opta + Blues Notecard
  Version: see FIRMWARE_VERSION in TankAlarm_Common.h

  Hardware:
  - Arduino Opta Lite (STM32H747XI dual-core)
  - Arduino Pro Opta Ext A0602 (analog and current-loop expansion)
  - Blues Wireless Notecard for Opta adapter (cellular + GPS)

  Features:
  - Multi-sensor monitoring with per-monitor alarm thresholds
  - Blues Notecard telemetry for server ingestion
  - SMS alarm escalation via server
  - Daily report schedule aligned with server
  - Configuration persisted to internal flash (LittleFS)
  - Remote configuration updates pushed from server via Notecard
  - Compile output includes hardware requirement summary based on configuration

  Created: November 2025
  Using GitHub Copilot for code generation
*/

#define DEVICE_ROLE TANKALARM_ROLE_CLIENT

// Shared library - common constants and utilities
#include <TankAlarm_Common.h>

#include <Arduino.h>
#include <Wire.h>

// Arduino_Opta_Blueprint: official A0602 expansion driver. Used ONLY at boot to run the
// Blueprint reset/address-assignment handshake so the A0602 ends up at a real assigned
// I2C address (OPTA_CONTROLLER_FIRST_AVAILABLE_ADDRESS = 0x0B+) instead of the legacy
// 0x64/0x0A probe defaults that the field A0602 was never at. Once the address is
// captured, the existing v2.0.46 raw Blueprint-frame read path (configureCurrentAdcChannel
// + getAnalogChannelFunction + readCurrentAdcFramed) talks to that discovered address and
// remains in charge of every per-sample transaction -- OptaController::update() is NOT
// called in the hot loop, so it never races the raw frames on the shared I2C bus.
#include "OptaBlue.h"

#if defined(TANKALARM_DFU_MCUBOOT)
#include <MCUboot.h>
#else
#warning "TANKALARM_DFU_MCUBOOT is NOT defined - MCUboot OTA updates are DISABLED in this build"
#endif

#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
#include <ArduinoRS485.h>
#endif

#include <ArduinoJson.h>
#include <memory>
#include <math.h>
#include <string.h>

// POSIX-compliant standard library headers
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>

// Forward-declare FluidType so Arduino's auto-generated function prototypes
// (inserted near the top of the .ino) can reference it before its definition below.
enum FluidType : uint8_t;

// POSIX file I/O types (for platforms that support it)
#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  #include <fcntl.h>
  #include <sys/stat.h>
#endif

// Filesystem support - Mbed OS filesystem instance
#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  #include "rtos/ThisThread.h"
  #include "MBRBlockDevice.h"
  using namespace std::chrono;
  using namespace std::chrono_literals;

  // QSPI is MBR-partitioned by KeyProvisioning (standard Arduino Opta layout):
  //   p1 WiFi (unused) | p2 MCUboot OTA staging | p3 KVStore (unused) | p4 user data.
  // This application's LittleFS config store lives on partition 4. We mount/format
  // ONLY that partition: reformatting the whole device would destroy the MCUboot
  // OTA partition (p2) and break firmware updates.
  #define TANKALARM_APP_DATA_PARTITION 4

  // Mbed OS filesystem instance - mounted at "/cfg" (NOT "/fs": the MCUboot core
  // reserves "/fs" for the OTA secondary slot on partition 2; sharing that mount
  // name shadowed the core's OTA mount and silently broke firmware staging).
  static LittleFileSystem *mbedFS = nullptr;
  static BlockDevice *mbedBD = nullptr;
  static mbed::MBRBlockDevice *mbedAppPart = nullptr;
  static MbedWatchdogHelper mbedWatchdog;
  static bool gStorageAvailable = false;
  
  // POSIX-compatible file path prefix for Mbed OS VFS (app config store, partition 4).
  // Must NOT be "/fs" -- that mount name is reserved by the MCUboot core for the OTA
  // secondary slot on partition 2; a shared name shadows the core's OTA mount.
  #define POSIX_FS_PREFIX "/cfg"
  #define FILESYSTEM_AVAILABLE
  #define POSIX_FILE_IO_AVAILABLE
#elif defined(ARDUINO_ARCH_STM32)
  #include <LittleFS.h>
  #include <IWatchdog.h>
  #define FILESYSTEM_AVAILABLE
  static bool gStorageAvailable = false;
#endif

static inline bool isStorageAvailable() {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    return gStorageAvailable && (mbedFS != nullptr);
  #else
    return gStorageAvailable;
  #endif
#else
  return false;
#endif
}

// Debug mode - controls Serial output and Notecard debug logging
// For PRODUCTION: Leave commented out (default) to save 5-10% power consumption
// For DEVELOPMENT: Uncomment the line below for troubleshooting and monitoring
//#define DEBUG_MODE

// Solar hardware test serial output (opt-in)
// Enable during RS-485/Modbus bring-up to print one structured line per poll.
// Leave disabled for normal deployments to minimize serial/log overhead.
//#define SOLAR_HW_TEST_SERIAL

// Optional test-only override: force-enable SunSaver polling with known defaults
// even when flash config currently has solarCharger.enabled=false.
// IMPORTANT: leave this commented in production -- it bypasses the server-pushed
// flash config and forces the SunSaver poller on regardless of what the server says.
//#define SOLAR_HW_TEST_FORCE_SOLAR_CONFIG

#ifdef SOLAR_HW_TEST_SERIAL
  #define SOLAR_TEST_PRINT(x) Serial.print(x)
  #define SOLAR_TEST_PRINTLN(x) Serial.println(x)
#else
  #define SOLAR_TEST_PRINT(x)
  #define SOLAR_TEST_PRINTLN(x)
#endif

// Debug output macros - no-op when DEBUG_MODE is disabled
#ifdef DEBUG_MODE
  #define DEBUG_BEGIN(baud) Serial.begin(baud)
  #define DEBUG_PRINT(x) Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
  #define DEBUG_PRINTF(x, y) Serial.print(x, y)
#else
  #define DEBUG_BEGIN(baud)
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
  #define DEBUG_PRINTF(x, y)
#endif

// Wrapper for shared library roundTo function
static inline float roundTo(float val, int decimals) { return tankalarm_roundTo(val, decimals); }

// Optional: Create a "ClientConfig.h" file in this sketch folder to set
// compile-time defaults (e.g. #define DEFAULT_PRODUCT_UID "com.company.product:project").
// If the file does not exist, the product UID must be set via the Config Generator on the server.
#if __has_include("ClientConfig.h")
  #include "ClientConfig.h"
#endif

#ifndef DEFAULT_PRODUCT_UID
#define DEFAULT_PRODUCT_UID ""  // Set via ClientConfig.h or Config Generator
#endif

// SOLAR_OUTBOUND_INTERVAL_MINUTES and SOLAR_INBOUND_INTERVAL_MINUTES
// are defined in TankAlarm_Config.h (via TankAlarm_Common.h)

#ifndef CLIENT_CONFIG_PATH
#define CLIENT_CONFIG_PATH "/client_config.json"
#endif

// Persisted record of the firmware build sequence that last booted on this device.
// Compared on boot to FIRMWARE_BUILD_SEQ to detect a just-installed firmware (OTA or USB)
// and trigger an immediate confirmation telemetry sync. Lives on the app data partition.
#ifndef FIRMWARE_MARKER_FILE
#define FIRMWARE_MARKER_FILE "/cfg/fw_marker.json"
#endif

// ============================================================================
// Client Notefile Names — Outbound (.qo) and Inbound (.qi)
// These override the common header where client perspective differs.
// Blues Notecard rule: note.add ONLY accepts .qo/.qos/.db/.dbs/.dbx
//                     note.get reads from .qi/.qis/.db/.dbx
// Cross-device delivery is handled by Notehub Routes (no fleet:/device: prefixes).
// ============================================================================

// Outbound data notefiles (client → Notehub → routed to server)
#define TELEMETRY_FILE TELEMETRY_OUTBOX_FILE   // "telemetry.qo"
#define ALARM_FILE     ALARM_OUTBOX_FILE       // "alarm.qo"
#define DAILY_FILE     DAILY_OUTBOX_FILE       // "daily.qo"
#define UNLOAD_FILE    UNLOAD_OUTBOX_FILE      // "unload.qo"

// Config: client reads config.qi (inbound), sends acks via config_ack.qo (outbound)
// CONFIG_INBOX_FILE, CONFIG_ACK_OUTBOX_FILE are defined in TankAlarm_Common.h

// Relay: client reads relay.qi (inbound commands from server)
// RELAY_CONTROL_FILE is already correct from common header ("relay.qi")

// Serial logs: client sends logs outbound, receives requests inbound
#define SERIAL_LOG_FILE SERIAL_LOG_OUTBOX_FILE  // "serial_log.qo"
// SERIAL_REQUEST_FILE is already correct ("serial_request.qi")

// Location: client receives requests inbound, sends responses outbound
#define LOCATION_RESPONSE_FILE LOCATION_RESPONSE_OUTBOX_FILE  // "location_response.qo"
// LOCATION_REQUEST_FILE is already correct ("location_request.qi")

// Health telemetry: client sends periodic system health status
#define HEALTH_FILE HEALTH_OUTBOX_FILE  // "health.qo"

// Diagnostics: I2C recovery events, future extension for config/DFU events
#define DIAG_FILE DIAG_OUTBOX_FILE  // "diag.qo"

// Health telemetry feature flag (disabled by default for bandwidth conservation)
// Uncomment to enable periodic system health reporting to Notehub.
//#define TANKALARM_HEALTH_TELEMETRY_ENABLED

// Health telemetry interval (default: every 6 hours)
#ifndef HEALTH_TELEMETRY_INTERVAL_MS
#define HEALTH_TELEMETRY_INTERVAL_MS (6UL * 60UL * 60UL * 1000UL)
#endif

// CLIENT_SERIAL_BUFFER_SIZE is defined in TankAlarm_Common.h

#ifndef NOTE_BUFFER_PATH
#define NOTE_BUFFER_PATH "/pending_notes.log"
#endif

#ifndef NOTE_BUFFER_TEMP_PATH
#define NOTE_BUFFER_TEMP_PATH "/pending_notes.tmp"
#endif

#ifndef NOTE_BUFFER_MAX_BYTES
#define NOTE_BUFFER_MAX_BYTES 16384
#endif

#ifndef NOTE_BUFFER_MIN_HEADROOM
#define NOTE_BUFFER_MIN_HEADROOM 2048
#endif

#ifndef MAX_MONITORS
#define MAX_MONITORS 8
#endif

// DEFAULT_SAMPLE_INTERVAL_SEC, DEFAULT_LEVEL_CHANGE_THRESHOLD,
// DEFAULT_REPORT_HOUR, DEFAULT_REPORT_MINUTE are defined in TankAlarm_Config.h
#define DEFAULT_SAMPLE_SECONDS          DEFAULT_SAMPLE_INTERVAL_SEC

#ifndef CURRENT_LOOP_I2C_ADDRESS
#define CURRENT_LOOP_I2C_ADDRESS 0x64
#endif

#ifndef CURRENT_LOOP_I2C_ALT_ADDRESS_1
#define CURRENT_LOOP_I2C_ALT_ADDRESS_1 0x0A
#endif

#ifndef CURRENT_LOOP_I2C_ALT_ADDRESS_2
#define CURRENT_LOOP_I2C_ALT_ADDRESS_2 0x00
#endif

#ifndef ALARM_DEBOUNCE_COUNT
#define ALARM_DEBOUNCE_COUNT 3  // Require 3 consecutive samples to trigger/clear alarm
#endif

#ifndef SENSOR_STUCK_THRESHOLD
#define SENSOR_STUCK_THRESHOLD 10  // Same reading 10 times = stuck sensor
#endif

#ifndef SENSOR_FAILURE_THRESHOLD
#define SENSOR_FAILURE_THRESHOLD 5  // 5 consecutive read failures = sensor failed
#endif

#ifndef CURRENT_LOOP_FAULT_MA
// Below this current the 4-20mA loop is treated as broken/disconnected (live-zero fault).
// A healthy transmitter never drives below ~3.8mA; 3.6mA leaves margin for sampling noise.
#define CURRENT_LOOP_FAULT_MA 3.6f
#endif

#ifndef CURRENT_LOOP_GATED_SETTLE_MS
// Minimum inter-sample settle (ms) for power-gated 4-20mA reads. The A0602 current-loop ADC
// needs time to deliver a fresh conversion after the channel is selected; the validated
// standalone gating test (P1_Transistor_Gating_Test) sampled at 300ms and read correctly,
// while a too-small configured delay returns stale/constant values that look like a pegged
// reading. When gating is on, the effective settle is floored to this value.
#define CURRENT_LOOP_GATED_SETTLE_MS 300
#endif

#ifndef CURRENT_LOOP_OVER_RANGE_MA
// Above this current a 4-20mA loop reading is rejected as a fault. A healthy transmitter cannot
// legitimately exceed 20mA full scale; a value above ~21mA (e.g. a raw 0xFFFF read that scales
// to 25mA, or a near-full-scale garbage frame) is an I2C/loop fault, not a real reading. Without
// this guard linearMap(mA,4,20,...) extrapolates the bad value into a fabricated level/pressure.
#define CURRENT_LOOP_OVER_RANGE_MA 21.0f
#endif

#ifndef A0602_WIRE_TIMEOUT_MS
// Scoped Wire byte-timeout (ms) for A0602 current-loop transactions. The A0602 does NOT clock-
// stretch, so it needs only a short timeout; failing fast lets the loop() bus recovery engage
// sooner instead of blocking. The Notecard (which DOES clock-stretch) keeps I2C_WIRE_TIMEOUT_MS.
#define A0602_WIRE_TIMEOUT_MS 25
#endif

#ifndef A0602_PEROP_I2C_CLOCK_HZ
// Per-operation I2C clock for A0602 framed transactions. Raised ONLY around the A0602 frames
// (then restored to I2C_NORMAL_CLOCK_HZ before any Notecard op) to shrink the A0602's bus-
// occupancy window ~4x WITHOUT ever running the clock-sensitive Notecard at 400kHz.
#define A0602_PEROP_I2C_CLOCK_HZ 400000
#endif

#ifndef I2C_NORMAL_CLOCK_HZ
// Shared-bus default clock (Notecard-safe). Restored after every A0602 per-op window.
#define I2C_NORMAL_CLOCK_HZ 100000
#endif

#ifndef MAX_ALARMS_PER_HOUR
#define MAX_ALARMS_PER_HOUR 10  // Maximum alarms per monitor per hour
#endif

#ifndef MAX_GLOBAL_ALARMS_PER_HOUR
#define MAX_GLOBAL_ALARMS_PER_HOUR 30  // Maximum alarms across ALL sensors per hour
#endif

#ifndef TELEMETRY_OUTBOX_MAX_PENDING
#define TELEMETRY_OUTBOX_MAX_PENDING 15  // Max telemetry.qo notes in outbox; older ones are dropped
#endif

#ifndef TELEMETRY_NOTE_ID_LEN
#define TELEMETRY_NOTE_ID_LEN 48         // Max length of a Notecard note ID string
#endif

#ifndef TELEMETRY_TRIM_COLLECTION_HEADROOM
#define TELEMETRY_TRIM_COLLECTION_HEADROOM 4  // Extra IDs collected beyond the limit to detect overflow
#endif

// Config schema versioning — bump when adding/removing fields to detect stale configs
#ifndef CONFIG_SCHEMA_VERSION
#define CONFIG_SCHEMA_VERSION 3
#endif

#ifndef MIN_ALARM_INTERVAL_SECONDS
#define MIN_ALARM_INTERVAL_SECONDS 300  // Minimum 5 minutes between same alarm type
#endif

// Tank unload detection constants
#ifndef UNLOAD_DEFAULT_DROP_PERCENT
#define UNLOAD_DEFAULT_DROP_PERCENT 50.0f  // Default: 50% drop from peak = unload event
#endif

#ifndef UNLOAD_DEFAULT_EMPTY_HEIGHT
#define UNLOAD_DEFAULT_EMPTY_HEIGHT 2.0f  // Default empty height when at/below sensor (inches)
#endif

#ifndef UNLOAD_MIN_PEAK_HEIGHT
#define UNLOAD_MIN_PEAK_HEIGHT 12.0f  // Minimum peak height before tracking starts (inches)
#endif

#ifndef UNLOAD_DEBOUNCE_COUNT
#define UNLOAD_DEBOUNCE_COUNT 3  // Require 3 consecutive low readings to confirm unload
#endif

// Default momentary relay duration (30 minutes in seconds)
#ifndef DEFAULT_RELAY_MOMENTARY_SECONDS
#define DEFAULT_RELAY_MOMENTARY_SECONDS 1800  // 30 minutes
#endif

// ============================================================================
// Power Conservation State Machine
// Progressive duty-cycle reduction based on battery voltage.
// Uses hysteresis thresholds to prevent oscillation during charge/discharge.
// Voltage source: getEffectiveBatteryVoltage() — SunSaver MPPT (Modbus) preferred,
// analog Vin divider as fallback. Notecard card.voltage is never used (Fix 9/11).
// ============================================================================
enum PowerState : uint8_t {
  POWER_STATE_NORMAL            = 0,  // Full operation
  POWER_STATE_ECO               = 1,  // Reduced polling, longer sleep
  POWER_STATE_LOW_POWER         = 2,  // Minimal polling, relays frozen
  POWER_STATE_CRITICAL_HIBERNATE = 3  // Essential monitoring only, relays OFF
};

// --- Entry thresholds (voltage falling) ---
// We enter a worse state when battery drops BELOW these values.
#ifndef POWER_ECO_ENTER_VOLTAGE
#define POWER_ECO_ENTER_VOLTAGE            12.0f   // Enter ECO below 12.0V (~25% SOC lead-acid)
#endif
#ifndef POWER_LOW_ENTER_VOLTAGE
#define POWER_LOW_ENTER_VOLTAGE            11.8f   // Enter LOW_POWER below 11.8V (~10% SOC)
#endif
#ifndef POWER_CRITICAL_ENTER_VOLTAGE
#define POWER_CRITICAL_ENTER_VOLTAGE       11.5f   // Enter CRITICAL below 11.5V (risk of damage)
#endif

// --- Exit thresholds (voltage rising, with hysteresis) ---
// We return to a better state when battery rises ABOVE these values.
// The gap between enter and exit prevents rapid state toggling.
#ifndef POWER_CRITICAL_EXIT_VOLTAGE
#define POWER_CRITICAL_EXIT_VOLTAGE        12.2f   // Exit CRITICAL above 12.2V (+0.7V hysteresis)
#endif
#ifndef POWER_LOW_EXIT_VOLTAGE
#define POWER_LOW_EXIT_VOLTAGE             12.3f   // Exit LOW_POWER above 12.3V (+0.5V hysteresis)
#endif
#ifndef POWER_ECO_EXIT_VOLTAGE
#define POWER_ECO_EXIT_VOLTAGE             12.4f   // Exit ECO above 12.4V (+0.4V hysteresis)
#endif

// --- Timing for each power state ---
// Loop sleep duration (rtos::ThisThread::sleep_for)
#ifndef POWER_NORMAL_SLEEP_MS
#define POWER_NORMAL_SLEEP_MS              100       // 100ms (existing default)
#endif
#ifndef POWER_ECO_SLEEP_MS
#define POWER_ECO_SLEEP_MS                 5000      // 5 seconds
#endif
#ifndef POWER_LOW_SLEEP_MS
#define POWER_LOW_SLEEP_MS                 30000     // 30 seconds
#endif
#ifndef POWER_CRITICAL_SLEEP_MS
#define POWER_CRITICAL_SLEEP_MS            300000    // 5 minutes
#endif

// Daily firmware-update safety net (power-state override). In LOW_POWER and
// CRITICAL_HIBERNATE the frequent DFU check is skipped to conserve energy, but an OTA must
// NEVER be permanently blocked by power state. At least once per this interval the device
// force-checks (and applies) a pending firmware update regardless of voltage or power state.
#ifndef DAILY_DFU_CHECK_INTERVAL_MS
#define DAILY_DFU_CHECK_INTERVAL_MS        86400000UL  // 24 hours
#endif
// Grace period after boot before the first low-power DFU check, so the Notecard has time to
// connect/sync before we query it.
#ifndef DAILY_DFU_BOOT_GRACE_MS
#define DAILY_DFU_BOOT_GRACE_MS            120000UL    // 2 minutes
#endif

#ifndef POWER_STATE_PERIODIC_LOG_MS
#define POWER_STATE_PERIODIC_LOG_MS        1800000UL // 30 minutes
#endif

#ifndef POWER_STATE_TRANSITION_LOG_MIN_MS
#define POWER_STATE_TRANSITION_LOG_MIN_MS  300000UL  // 5 minutes
#endif

// Outbound sync multipliers (applied to base outbound interval)
#ifndef POWER_ECO_OUTBOUND_MULTIPLIER
#define POWER_ECO_OUTBOUND_MULTIPLIER      2    // 2x slower (e.g., 12h instead of 6h)
#endif
#ifndef POWER_LOW_OUTBOUND_MULTIPLIER
#define POWER_LOW_OUTBOUND_MULTIPLIER      4    // 4x slower (e.g., 24h instead of 6h)
#endif

// Inbound check multipliers
#ifndef POWER_ECO_INBOUND_MULTIPLIER
#define POWER_ECO_INBOUND_MULTIPLIER       4    // 4x slower
#endif
#ifndef POWER_LOW_INBOUND_MULTIPLIER
#define POWER_LOW_INBOUND_MULTIPLIER       12   // 12x slower
#endif

// Awaiting-config inbound poll interval (grid mode, software poll only)
// Used when monitorCount == 0 — device is unconfigured and needs to receive
// its first config from the server as quickly as possible.
#ifndef AWAITING_CONFIG_INBOUND_INTERVAL_MS
#define AWAITING_CONFIG_INBOUND_INTERVAL_MS  60000UL  // 1 minute
#endif

// Awaiting-config Notecard inbound interval (solar/periodic mode only)
// Overrides SOLAR_INBOUND_INTERVAL_MINUTES when monitorCount == 0 so the
// Notecard wakes to check for inbound notes more aggressively during provisioning.
// Restores to SOLAR_INBOUND_INTERVAL_MINUTES automatically once config arrives
// (configureNotecardHubMode() is called by applyConfigUpdate).
#ifndef AWAITING_CONFIG_SOLAR_INBOUND_MINUTES
#define AWAITING_CONFIG_SOLAR_INBOUND_MINUTES  5  // 5 minutes
#endif

// Sample interval multiplier
#ifndef POWER_ECO_SAMPLE_MULTIPLIER
#define POWER_ECO_SAMPLE_MULTIPLIER        2    // 2x slower
#endif
#ifndef POWER_LOW_SAMPLE_MULTIPLIER
#define POWER_LOW_SAMPLE_MULTIPLIER        4    // 4x slower
#endif

// Minimum consecutive readings before changing power state (debounce)
#ifndef POWER_STATE_DEBOUNCE_COUNT
#define POWER_STATE_DEBOUNCE_COUNT         3
#endif

// Digital sensor (float switch) constants
#ifndef DIGITAL_SWITCH_THRESHOLD
#define DIGITAL_SWITCH_THRESHOLD 0.5f  // Threshold to determine activated vs not-activated state
#endif

#ifndef DIGITAL_SENSOR_ACTIVATED_VALUE
#define DIGITAL_SENSOR_ACTIVATED_VALUE 1.0f  // Value returned when switch is activated
#endif

#ifndef DIGITAL_SENSOR_NOT_ACTIVATED_VALUE
#define DIGITAL_SENSOR_NOT_ACTIVATED_VALUE 0.0f  // Value returned when switch is not activated
#endif

// Helper function: Get pressure-to-inches conversion factor by unit name string
static float getPressureConversionFactorByName(const char* unit) {
  if (strcmp(unit, "bar") == 0) return getPressureConversionFactor(PressureUnit::BAR);
  if (strcmp(unit, "kPa") == 0) return getPressureConversionFactor(PressureUnit::KPA);
  if (strcmp(unit, "mbar") == 0) return getPressureConversionFactor(PressureUnit::MBAR);
  if (strcmp(unit, "inH2O") == 0) return getPressureConversionFactor(PressureUnit::IN_H2O);
  return getPressureConversionFactor(PressureUnit::PSI); // Default: PSI
}

// Helper function: Get distance-to-inches conversion factor by unit name string
static float getDistanceConversionFactorByName(const char* unit) {
  if (strcmp(unit, "m") == 0) return getDistanceConversionFactor(DistanceUnit::METER);
  if (strcmp(unit, "cm") == 0) return getDistanceConversionFactor(DistanceUnit::CENTIMETER);
  if (strcmp(unit, "ft") == 0) return getDistanceConversionFactor(DistanceUnit::FOOT);
  return getDistanceConversionFactor(DistanceUnit::INCH); // Default: assume inches
}

// Forward decl: defined after MonitorConfig is in scope below.
struct MonitorConfig;
static float getEffectiveSpecificGravity(const MonitorConfig &cfg);

// Object types - what is being monitored
enum ObjectType : uint8_t {
  OBJECT_TANK = 0,        // Liquid storage tank (level monitoring)
  OBJECT_ENGINE = 1,      // Engine or motor (RPM monitoring)
  OBJECT_PUMP = 2,        // Pump (status or flow monitoring)
  OBJECT_GAS = 3,         // Gas pressure system (propane, natural gas, etc.)
  OBJECT_FLOW = 4,        // Flow meter (liquid or gas flow rate)
  OBJECT_CUSTOM = 255     // User-defined/other
};

// Fluid type presets - used to derive specific gravity for pressure->height conversion.
// Specific gravity (SG) is the ratio of fluid density to water density at standard conditions.
// SG affects how much head pressure a column of fluid produces:
//   inches_of_fluid = (PSI * 27.68) / SG
// So a 27.68 in/PSI conversion (assuming water) under-reports level for fluids lighter
// than water (e.g., diesel SG=0.85 reads ~17% low) and over-reports for heavier fluids.
enum FluidType : uint8_t {
  FLUID_WATER = 0,         // 1.00
  FLUID_DIESEL = 1,        // 0.85
  FLUID_GASOLINE = 2,      // 0.74
  FLUID_HEATING_OIL = 3,   // 0.85
  FLUID_PROPANE_LPG = 4,   // 0.50 (liquid LPG)
  FLUID_BRINE = 5,         // 1.20
  FLUID_CRUDE_OIL = 6,     // 0.83
  FLUID_USED_OIL = 7,      // 0.92
  FLUID_GLYCOL_50 = 8,     // 1.07 (50/50 propylene glycol antifreeze)
  FLUID_DEF_ADBLUE = 9,    // 1.09 (Diesel Exhaust Fluid / AdBlue)
  FLUID_ETHANOL = 10,      // 0.79
  FLUID_CUSTOM = 255       // Use MonitorConfig.fluidSpecificGravity (manual entry)
};

// Returns the preset SG for a fluid type. For FLUID_CUSTOM, returns 0.0 to signal
// "caller must use the manually entered fluidSpecificGravity".
static float getPresetSpecificGravity(FluidType ft) {
  switch (ft) {
    case FLUID_WATER:        return 1.00f;
    case FLUID_DIESEL:       return 0.85f;
    case FLUID_GASOLINE:     return 0.74f;
    case FLUID_HEATING_OIL:  return 0.85f;
    case FLUID_PROPANE_LPG:  return 0.50f;
    case FLUID_BRINE:        return 1.20f;
    case FLUID_CRUDE_OIL:    return 0.83f;
    case FLUID_USED_OIL:     return 0.92f;
    case FLUID_GLYCOL_50:    return 1.07f;
    case FLUID_DEF_ADBLUE:   return 1.09f;
    case FLUID_ETHANOL:      return 0.79f;
    case FLUID_CUSTOM:       return 0.0f;  // signal: use manual override
    default:                 return 1.00f;
  }
}

static FluidType parseFluidTypeName(const char *name) {
  if (!name) return FLUID_WATER;
  if (strcmp(name, "water") == 0)        return FLUID_WATER;
  if (strcmp(name, "diesel") == 0)       return FLUID_DIESEL;
  if (strcmp(name, "gasoline") == 0)     return FLUID_GASOLINE;
  if (strcmp(name, "heatingOil") == 0)   return FLUID_HEATING_OIL;
  if (strcmp(name, "propane") == 0 || strcmp(name, "lpg") == 0) return FLUID_PROPANE_LPG;
  if (strcmp(name, "brine") == 0)        return FLUID_BRINE;
  if (strcmp(name, "crudeOil") == 0)     return FLUID_CRUDE_OIL;
  if (strcmp(name, "usedOil") == 0)      return FLUID_USED_OIL;
  if (strcmp(name, "glycol50") == 0)     return FLUID_GLYCOL_50;
  if (strcmp(name, "def") == 0 || strcmp(name, "adblue") == 0) return FLUID_DEF_ADBLUE;
  if (strcmp(name, "ethanol") == 0)      return FLUID_ETHANOL;
  if (strcmp(name, "custom") == 0)       return FLUID_CUSTOM;
  return FLUID_WATER;
}

static const char *fluidTypeName(FluidType ft) {
  switch (ft) {
    case FLUID_WATER:       return "water";
    case FLUID_DIESEL:      return "diesel";
    case FLUID_GASOLINE:    return "gasoline";
    case FLUID_HEATING_OIL: return "heatingOil";
    case FLUID_PROPANE_LPG: return "propane";
    case FLUID_BRINE:       return "brine";
    case FLUID_CRUDE_OIL:   return "crudeOil";
    case FLUID_USED_OIL:    return "usedOil";
    case FLUID_GLYCOL_50:   return "glycol50";
    case FLUID_DEF_ADBLUE:  return "def";
    case FLUID_ETHANOL:     return "ethanol";
    case FLUID_CUSTOM:      return "custom";
    default:                return "water";
  }
}

// Sensor interface types - how the measurement is taken
enum SensorInterface : uint8_t {
  SENSOR_DIGITAL = 0,       // Binary on/off (float switch, relay contact)
  SENSOR_ANALOG = 1,        // Voltage output (0-10V, 1-5V)
  SENSOR_CURRENT_LOOP = 2,  // 4-20mA current loop
  SENSOR_PULSE = 3          // Pulse/frequency counting (hall effect, flow meter)
};

// 4-20mA current loop sensor subtypes
enum CurrentLoopSensorType : uint8_t {
  CURRENT_LOOP_PRESSURE = 0,    // Pressure sensor mounted near bottom of tank (e.g., Dwyer 626-06-CB-P1-E5-S1)
                                // 4mA = empty (0 PSI), 20mA = full (max PSI)
  CURRENT_LOOP_ULTRASONIC = 1   // Ultrasonic sensor mounted on top of tank (e.g., Siemens Sitrans LU240)
                                // 4mA = full (sensor close to liquid), 20mA = empty (sensor far from liquid)
};

// Hall effect sensor types for RPM measurement
enum HallEffectSensorType : uint8_t {
  HALL_EFFECT_UNIPOLAR = 0,     // Triggered by single pole (usually South), reset when field removed
  HALL_EFFECT_BIPOLAR = 1,      // Latching: South pole turns ON, North pole turns OFF
  HALL_EFFECT_OMNIPOLAR = 2,    // Responds to either North or South pole
  HALL_EFFECT_ANALOG = 3        // Linear/analog: outputs voltage proportional to magnetic field strength
};

// Hall effect detection method
enum HallEffectDetectionMethod : uint8_t {
  HALL_DETECT_PULSE = 0,        // Count pulses (transitions) - traditional method
  HALL_DETECT_TIME_BASED = 1    // Measure time between pulses - more flexible for different magnet types
};

// Relay trigger conditions - which alarm type triggers the relay
enum RelayTrigger : uint8_t {
  RELAY_TRIGGER_ANY = 0,   // Trigger on any alarm (high or low)
  RELAY_TRIGGER_HIGH = 1,  // Trigger only on high alarm
  RELAY_TRIGGER_LOW = 2    // Trigger only on low alarm
};

// Relay engagement mode - how long the relay stays on
enum RelayMode : uint8_t {
  RELAY_MODE_MOMENTARY = 0,     // Momentary on for configurable duration, then auto-off
  RELAY_MODE_UNTIL_CLEAR = 1,   // Stay on until alarm clears
  RELAY_MODE_MANUAL_RESET = 2   // Stay on until manually reset from server
};

// Default relay engagement duration (30 minutes in seconds)
#define RELAY_DEFAULT_MOMENTARY_SECONDS 1800

struct MonitorConfig {
  char id;                 // Friendly identifier (A, B, C ...)
  char name[24];           // Label shown in reports (e.g., "North Tank", "Main Pump")
  char contents[24];       // What the tank contains (e.g., "Diesel", "Water") - not used for RPM monitors
  uint8_t sensorIndex;   // Numeric reference (1, 2, 3...)
  uint8_t userNumber;    // Optional user-assigned display number (0 = unset)
  ObjectType objectType;   // What is being monitored (tank, engine, pump, gas, flow)
  SensorInterface sensorInterface; // How measurement is taken (digital, analog, currentLoop, pulse)
  int16_t primaryPin;      // Digital pin or analog channel
  int16_t secondaryPin;    // Optional secondary pin (unused by default)
  int16_t currentLoopChannel; // 4-20mA channel index (-1 if unused)
  int16_t pulsePin;        // Pulse sensor pin for RPM/flow (-1 if unused)
  uint8_t pulsesPerUnit;   // Pulses per revolution (RPM) or per gallon (flow), default 1
  HallEffectSensorType hallEffectType; // Type of hall effect sensor (unipolar, bipolar, omnipolar, analog)
  HallEffectDetectionMethod hallEffectDetection; // Detection method (pulse counting or time-based)
  uint32_t pulseSampleDurationMs; // Sample duration for pulse measurement (default 60000ms = 60s)
  bool pulseAccumulatedMode; // If true, count pulses between telemetry reports for very low rates
  bool alarmsEnabled;      // True only when alarm thresholds were explicitly configured
  float highAlarmThreshold;   // High threshold for triggering alarm
  float lowAlarmThreshold;    // Low threshold for triggering alarm
  float hysteresisValue;   // Hysteresis band (default 2.0)
  bool enableDailyReport;  // Include in daily summary
  bool enableAlarmSms;     // Escalate SMS when alarms trigger
  bool enableServerUpload; // Send telemetry to server
  float reportThreshold;   // Minimum change, in THIS monitor's own measurement unit, before a
                           // change-based telemetry note is sent. 0 = disabled (baseline + daily +
                           // alarms only). Unit-agnostic: inches for tanks, PSI for gas, RPM for
                           // engines, GPM for flow, etc.
  char relayTargetClient[48]; // Client UID to trigger relays on (empty = none)
  uint8_t relayMask;       // Bitmask of relays to trigger (bit 0=relay 1, etc.)
  RelayTrigger relayTrigger; // Which alarm type triggers the relay (any, high, low)
  RelayMode relayMode;     // How long relay stays on (momentary, until_clear, manual_reset)
  uint16_t relayMomentarySeconds[4]; // Per-relay momentary duration in seconds (0 = use default 30 min)
  uint32_t relayMaxOnSeconds;  // Max ON duration for MANUAL_RESET mode (0 = no limit, default)
  // Digital sensor (float switch) specific settings
  char digitalTrigger[16]; // 'activated' or 'not_activated' - when to trigger alarm for digital sensors
  char digitalSwitchMode[4]; // 'NO' for normally-open, 'NC' for normally-closed (default: NO)
  // 4-20mA current loop sensor settings
  CurrentLoopSensorType currentLoopType; // Pressure (bottom-mounted) or Ultrasonic (top-mounted)
  float sensorMountHeight; // For ultrasonic: distance from sensor to tank bottom (inches)
                           // For pressure: height of sensor above tank bottom (inches, usually 0-2)
  float sensorRangeMin;    // Minimum native sensor range (e.g., 0 for 0-5 PSI or 0-10m)
  float sensorRangeMax;    // Maximum native sensor range (e.g., 5 for 0-5 PSI, 10 for 0-10m)
  char sensorRangeUnit[8]; // Unit for sensor range: "PSI", "bar", "m", "ft", "in", etc.
  bool pwmGatingEnabled;       // Gating control via high-side transistor enabled
  int16_t pwmGatingChannel;    // Transistor output channel pin index (0 = P1, 1 = P2, 2 = P3, 3 = P4)
  uint32_t pwmGatingWarmup;    // Time in milliseconds for physical Loop power-up stabilization (3000ms by default)
  uint16_t pwmGatingSampleDelay; // Delay in milliseconds within the average sampling loop (e.g. 5ms or 300ms)
  // Fluid characterization (for liquid tanks; ignored for OBJECT_GAS / non-tank objects).
  // Used as the fallback SG before the server's calibration learning takes over.
  FluidType fluidType;             // Fluid preset (default FLUID_WATER)
  float fluidSpecificGravity;      // Manual SG override; only used when fluidType == FLUID_CUSTOM
  // Server-pushed learned calibration (current-loop sensors only). When hasLearnedCalibration
  // is set, the client applies level = calSlope*mA + calOffset + calTempCoef*(calTempF-70)
  // instead of the theoretical conversion, so the client's level (and therefore its alarm
  // thresholds) match the server's calibrated display. calVersion (cv) is echoed back in
  // every note so the server knows which calibration the client used.
  bool hasLearnedCalibration;      // True once the server has pushed a learned calibration
  float calSlope;                  // Learned inches per mA
  float calOffset;                 // Learned offset (inches)
  float calTempCoef;               // Temp coefficient (inches per °F from 70°F); 0 = no temp comp
  float calTempF;                  // Server-pushed temperature for tempCoef (°F); used only when calTempCoef != 0
  uint32_t calVersion;             // Calibration version (cv) echoed back to the server
  // Analog voltage sensor settings (for sensors like Dwyer 626 with voltage output)
  float analogVoltageMin;  // Minimum voltage output (e.g., 0.0 for 0-10V, 1.0 for 1-5V)
  float analogVoltageMax;  // Maximum voltage output (e.g., 10.0 for 0-10V, 5.0 for 1-5V)
  // Measurement unit for display/reporting
  char measurementUnit[8]; // "inches", "psi", "rpm", "gpm", etc.
  // Expected pulse rate for baseline comparison (by object type)
  float expectedPulseRate; // Expected RPM for engines, GPM for flow, etc. (0 = not configured)
  // Stuck sensor detection
  bool stuckDetectionEnabled; // true = flag sensor as failed after 10 identical readings
  // Tank unload tracking configuration
  bool trackUnloads;          // true = this tank is regularly emptied, track unload events
  float unloadEmptyHeight;    // Default empty height when level drops to/below sensor height (inches)
  float unloadDropThreshold;  // Minimum drop to consider as unload (inches, default 50% of tank height)
  float unloadDropPercent;    // Alternative: minimum drop as percentage of peak height (0-100, default 50)
  bool unloadAlarmSms;        // Send SMS notification when tank is unloaded
  bool unloadAlarmEmail;      // Include unload events in daily email
};

struct ClientConfig {
  uint8_t configSchemaVersion;  // Schema version for forward/backward compat detection
  char siteName[32];
  char deviceUid[32];   // Device UID (e.g., dev:...)
  char deviceLabel[24];
  char clientFleet[32]; // Fleet for this device context
  char serverFleet[32]; // Target fleet name for server (e.g., "tankalarm-server")
  char productUid[64];  // Notehub product UID (configurable for different fleets)
  char dailyEmail[64];
  uint16_t sampleSeconds;
  uint8_t reportHour;
  uint8_t reportMinute;
  uint8_t monitorCount;
  MonitorConfig monitors[MAX_MONITORS];
  // Optional clear button configuration
  int8_t clearButtonPin;        // Pin for physical clear button (-1 = disabled)
  bool clearButtonActiveHigh;   // true = button active when HIGH, false = active when LOW (with pullup)
  // Power saving configuration
  bool solarPowered;            // true = solar powered (use power saving features), false = grid-tied
  // I2C sensor configuration  
  uint8_t currentLoopI2cAddress; // I2C address for 4-20mA current loop sensor (default 0x64)
  // Solar/Battery charger monitoring configuration (SunSaver MPPT via RS-485)
  // Requires: Arduino Opta with RS485 + Morningstar MRC-1 adapter
  SolarConfig solarCharger;     // Solar charger monitoring configuration
  // Battery voltage monitoring via Notecard (when wired directly to battery)
  // Provides low voltage alerts and trend analysis
  BatteryConfig batteryMonitor; // Notecard battery voltage monitoring
  // Analog voltage divider for reading actual battery voltage (Vin)
  // Requires external resistor divider wired to an Opta analog input
  VinMonitorConfig vinMonitor;  // Optional analog Vin voltage divider
  // Solar-Only (No Battery) mode configuration
  // For installations powered directly by solar panel without battery backup
  SolarOnlyConfig solarOnlyConfig;
  // Remote-tunable power conservation thresholds (0.0 = use compile-time default)
  // Allows server to adjust voltage trip points without firmware update
  float powerEcoEnterV;       // Enter ECO below this voltage (default: POWER_ECO_ENTER_VOLTAGE)
  float powerLowEnterV;       // Enter LOW_POWER below this voltage
  float powerCriticalEnterV;  // Enter CRITICAL below this voltage
  float powerEcoExitV;        // Exit ECO above this voltage
  float powerLowExitV;        // Exit LOW_POWER above this voltage
  float powerCriticalExitV;   // Exit CRITICAL above this voltage
  // Remote-tunable health check interval (0 = use compile-time default)
  uint32_t healthCheckBaseIntervalMs;  // Base Notecard health check interval in ms
  uint32_t configEpoch;                // Generation timestamp of the currently active configuration
};

struct MonitorRuntime {
  float currentInches;
  float currentSensorMa;        // Raw sensor reading in milliamps (for 4-20mA sensors)
  float currentSensorVoltage;   // Raw sensor reading in volts (for analog voltage sensors)
  double lastReadingEpoch;      // Epoch when current reading was actually acquired
  bool sampleReused;            // True when this cycle reused the previous reading
  bool lastPwmEnableOk;         // v1.9.22: result of the last P1 power-gate enable (gated 4-20mA)
  float lastReportedValue;   // Last value pushed via change-based telemetry (monitor's own unit)
  float lastDailySentValue;  // Last value included in a daily report (monitor's own unit)
  bool highAlarmLatched;
  bool lowAlarmLatched;
  unsigned long lastSampleMillis;
  unsigned long lastAlarmSendMillis;
  // Debouncing state
  uint8_t highAlarmDebounceCount;
  uint8_t lowAlarmDebounceCount;
  uint8_t highClearDebounceCount;
  uint8_t lowClearDebounceCount;
  // Sensor failure detection
  float lastValidReading;
  bool hasLastValidReading;
  uint8_t consecutiveFailures;
  uint8_t stuckReadingCount;
  bool sensorFailed;
  uint8_t recoveryCount;         // Consecutive good readings after failure (debounce)
  // Rate limiting
  unsigned long alarmTimestamps[MAX_ALARMS_PER_HOUR];
  uint8_t alarmCount;
  unsigned long lastHighAlarmMillis;
  unsigned long lastLowAlarmMillis;
  unsigned long lastClearAlarmMillis;
  unsigned long lastSensorFaultMillis;
  // Tank unload tracking state
  float unloadPeakInches;         // Highest level seen since last unload event
  float unloadPeakSensorMa;       // Sensor mA at peak level (for logging)
  double unloadPeakEpoch;         // Timestamp of peak reading
  bool unloadTracking;            // true = currently tracking fill cycle
};

static ClientConfig gConfig;
static MonitorRuntime gMonitorState[MAX_MONITORS];

// Global alarm rate limiting (across all sensors)
static unsigned long gGlobalAlarmTimestamps[MAX_GLOBAL_ALARMS_PER_HOUR];
static uint8_t gGlobalAlarmCount = 0;

// Solar/Battery charger monitoring (SunSaver MPPT via RS-485)
static SolarManager gSolarManager;
static unsigned long gLastSolarAlarmMillis = 0;
static SolarAlertType gLastSolarAlert = SOLAR_ALERT_NONE;
#define SOLAR_ALARM_MIN_INTERVAL_MS 3600000UL  // Min 1 hour between same solar alarm

// Battery voltage monitoring via Notecard (when wired directly to battery)
static BatteryData gBatteryData;
static unsigned long gLastBatteryPollMillis = 0;
static unsigned long gLastBatteryAlarmMillis = 0;
static BatteryAlertType gLastBatteryAlert = BATTERY_ALERT_NONE;
static float gLastBatteryAlertVoltage = 0.0f;

// Analog Vin voltage divider monitoring
static float gVinVoltage = 0.0f;          // Last reading from voltage divider (actual battery V)
static unsigned long gLastVinPollMillis = 0;

// Solar-Only (No Battery) runtime state
static bool gSolarOnlyStartupComplete = false;   // Has startup debounce/warmup passed?
static bool gSolarOnlySensorsReady = false;       // Is voltage high enough for sensors?
static bool gSolarOnlySunsetActive = false;       // Is sunset protocol in progress?
static unsigned long gSolarOnlySunsetStart = 0;   // When voltage decline was first detected
static float gSolarOnlyLastVin = 0.0f;            // Previous Vin reading for trend detection
static double gSolarOnlyLastReportEpoch = 0.0;    // Last daily report epoch (persisted to flash)
static uint32_t gSolarOnlyBootCount = 0;          // Boot counter (persisted to flash)
static bool gSolarOnlyBatteryFailed = false;      // Battery failure fallback active?
static uint8_t gSolarOnlyBatFailCount = 0;        // Consecutive critical battery readings
// Set true on the first boot after a firmware version change (OTA or USB update). Triggers an
// immediate confirmation telemetry sync so transmission can be verified without waiting for the
// next periodic/daily sync window.
static bool gFirmwareJustUpdated = false;
static unsigned long gSolarOnlyBatFailLastIncrMillis = 0; // When bat-fail count was last incremented
static bool gSolarOnlyStateSaved = false;         // Has state been saved during sunset?

// 24-hour decay period for battery failure counter (prevents long-term false accumulation)
#ifndef SOLAR_BAT_FAIL_DECAY_MS
#define SOLAR_BAT_FAIL_DECAY_MS (24UL * 60UL * 60UL * 1000UL)  // 24 hours
#endif

// Is solar-only behavior active (either configured or battery failure fallback)?
static bool isSolarOnlyActive() {
  return gConfig.solarOnlyConfig.enabled || gSolarOnlyBatteryFailed;
}

// Power conservation state machine
static PowerState gPowerState = POWER_STATE_NORMAL;
static PowerState gPreviousPowerState = POWER_STATE_NORMAL;
static float gEffectiveBatteryVoltage = 0.0f;  // Best voltage from either source
static const char *gEffectiveVoltageSource = nullptr;  // Fix 7: source of the last effective voltage ("mppt" | "vin-divider"); nullptr when none
static uint8_t gPowerStateDebounce = 0;        // Consecutive readings at proposed new state
static unsigned long gPowerStateChangeMillis = 0; // When the current power state was entered
static unsigned long gLastPowerStateLogMillis = 0; // Rate-limit power state log messages
static unsigned long gLastPowerStateTransitionLogMillis = 0; // Rate-limit transition log spam

// Health telemetry tracking
#ifdef TANKALARM_HEALTH_TELEMETRY_ENABLED
static unsigned long gLastHealthTelemetryMillis = 0;
static uint32_t gHeapMinFreeBytes = UINT32_MAX;  // Low-watermark tracker
static uint32_t gNotecardCommErrorCount = 0;      // Cumulative I2C/serial comm failures
static uint32_t gStorageWriteErrorCount = 0;      // Cumulative flash write failures
#endif

static Notecard notecard;
static char gDeviceUID[48] = {0};
static unsigned long gLastTelemetryMillis = 0;
static unsigned long gLastHeartbeatMillis = 0;
static unsigned long gLastConfigCheckMillis = 0;
static unsigned long gLastTimeSyncMillis = 0;
static double gLastSyncedEpoch = 0.0;
static double gNextDailyReportEpoch = 0.0;

// DFU (Device Firmware Update) state tracking
static unsigned long gLastDfuCheckMillis = 0;
// Last forced daily DFU check while in a low-power state (LOW_POWER/CRITICAL_HIBERNATE).
// 0 = not yet run this boot; the first check fires after DAILY_DFU_BOOT_GRACE_MS.
static unsigned long gLastDailyDfuCheckMillis = 0;
static bool gDfuUpdateAvailable = false;
static char gDfuVersion[32] = {0};
static bool gDfuInProgress = false;

static bool gConfigDirty = false;
// BugFix v1.6.2 (I-11): Deferred ACK state — ACK is sent only after persistence succeeds.
static bool gPendingConfigAck = false;
static char gPendingConfigAckVersion[16] = {0};
static bool gPendingConfigAckSuccess = true;
static char gPendingConfigAckMessage[48] = "Config applied and persisted";
static unsigned long gPendingConfigAckRetryAt = 0;
static unsigned long gPendingConfigAckRetryDelayMs = 5000UL;
static bool gHardwareSummaryPrinted = false;
static bool gChemistryChecked = false;

// Network failure handling
static unsigned long gLastSuccessfulNotecardComm = 0;
static unsigned long gLastSuccessfulNoteSend = 0;  // Last successful note.add (separate from card.wireless)
static uint8_t gNotecardFailureCount = 0;
static bool gNotecardAvailable = true;
#define NOTECARD_RETRY_INTERVAL 60000UL  // Retry after 60 seconds
#define NOTECARD_MODEM_STALL_MS (4UL * 60UL * 60UL * 1000UL)  // 4 hours without note send = modem stall

// Cellular signal strength cache (updated by checkNotecardHealth via card.wireless)
static int8_t gSignalBars = -1;     // 0-4 bars, -1 = unknown
static int16_t gSignalRssi = 0;     // RSSI in dBm
static int16_t gSignalRsrp = 0;     // RSRP in dBm (LTE reference signal power)
static int16_t gSignalRsrq = 0;     // RSRQ in dB  (LTE reference signal quality)
static char gSignalRat[8] = {0};    // Radio access technology (e.g., "lte", "catm")

// I2C bus error tracking (current loop / A0602)
// Non-static: extern-linked via TankAlarm_I2C.h shared functions
uint32_t gCurrentLoopI2cErrors = 0;
uint32_t gI2cBusRecoveryCount = 0;

// v2.0.47: A0602 managed-addressing state. Blueprint expansions are assigned a dynamic I2C
// address (from OPTA_CONTROLLER_FIRST_AVAILABLE_ADDRESS = 0x0B upward) when the controller
// runs its bootstrap; the legacy 0x64/0x0A probe missed those entirely. bootstrapA0602Managed()
// in setup() runs OptaController.begin()+update() ONCE to discover the real assigned address
// (and turn the A0602 status LED GREEN). gConfig.currentLoopI2cAddress is then steered at that
// address so the existing v2.0.46 framed-protocol read path talks to the right place. The
// controller is NOT polled after bootstrap (no OptaController.update() in the hot loop), so it
// never competes with the raw Blueprint frames for the shared I2C bus.
static int     gA0602DeviceIndex    = -1;     // -1 => bootstrap failed / no expansion
static uint8_t gA0602ManagedAddress = 0x00;   // assigned address (0x0B..) captured at boot
static bool    gOptaControllerReady = false;

// v2.0.46: A0602 current-loop diagnostics (daily-windowed; reset with gCurrentLoopI2cErrors).
// Make the shared-bus health observable in Notehub so a field A0602 fault is measurable, and
// so the per-op 400kHz window's effect on read latency (gLastClBurstMicros) can be confirmed.
enum ClFaultReason : uint8_t {
  CL_FAULT_NONE = 0,        // last A0602 read OK
  CL_FAULT_PWM_NACK = 1,    // P1 power-gate enable NACKed (loop unpowered)
  CL_FAULT_CONFIG_NACK = 2, // ADC channel config NACKed
  CL_FAULT_FUNC_WRONG = 3,  // channel not in current-ADC mode
  CL_FAULT_READ_FAIL = 4,   // every framed sample failed
  CL_FAULT_OVER_RANGE = 5   // reading rejected as >21mA / 0xFFFF garbage
};

// Fix 12 (v2.0.51): stable short strings for the current-loop fault enum, emitted in
// telemetry/daily/alarm payloads when a current-loop read fails. Server-side dashboards
// can show these directly (or map them to human-readable text), so a failed read can never
// be confused for a successful 0.0 PSI / 0.0 in reading.
static inline const char *clFaultReasonString(uint8_t reason) {
  switch (reason) {
    case CL_FAULT_NONE:        return "ok";
    case CL_FAULT_PWM_NACK:    return "pwm_nack";
    case CL_FAULT_CONFIG_NACK: return "config_nack";
    case CL_FAULT_FUNC_WRONG:  return "func_wrong";
    case CL_FAULT_READ_FAIL:   return "read_fail";
    case CL_FAULT_OVER_RANGE:  return "over_range";
    default:                   return "unknown";
  }
}
uint32_t gCurrentLoopReadsOk = 0;            // valid A0602 reads this window
uint32_t gCurrentLoopOverRange = 0;          // reads rejected as over-range/garbage this window
uint8_t  gLastClFaultReason = CL_FAULT_NONE; // last A0602 fault code (see ClFaultReason)
uint32_t gLastClBurstMicros = 0;             // duration (us) of the last A0602 read burst

// Startup I2C scan results — persisted for first health telemetry report
static bool gStartupNotecardFound = false;
static bool gStartupCurrentLoopFound = false;
static uint8_t gStartupScanRetries = 0;
static uint8_t gStartupUnexpectedDevices = 0;
static bool gStartupScanReported = false;

static const size_t DAILY_NOTE_PAYLOAD_LIMIT = 960U;

// Relay control state
// MAX_RELAYS is defined in TankAlarm_Common.h
static bool gRelayState[MAX_RELAYS] = {false, false, false, false};
static unsigned long gLastRelayCheckMillis = 0;

// Per-relay runtime state (RelayRuntime struct defined in TankAlarm_Common.h)
static RelayRuntime gRelayRuntime[MAX_RELAYS] = {};

// Forward declarations for relay helper functions
// (Arduino auto-prototyping does not handle default parameters correctly)
static uint8_t findMonitorForRelay(uint8_t relayNum);
static void activateRelayForMonitor(uint8_t monitorIdx, uint8_t relayMask,
                                     RelaySource source, unsigned long now,
                                     uint32_t customDurationSec = 0);
static void deactivateRelayForMonitor(uint8_t monitorIdx, uint8_t relayMask);
static bool isMonitorRelayActive(uint8_t monitorIdx);
static uint8_t getMonitorActiveRelayMask(uint8_t monitorIdx);

// Clear button state for debouncing
static unsigned long gClearButtonLastPressTime = 0;
static bool gClearButtonLastState = false;
static bool gClearButtonInitialized = false;
#define CLEAR_BUTTON_DEBOUNCE_MS 50
#define CLEAR_BUTTON_MIN_PRESS_MS 500  // Require 500ms press to clear (prevent accidental triggers)

// On-board front-panel USER button (BTN_USER) — short-press triggers an
// immediate hub.sync so the operator can pull pending config/relay updates
// from Notehub without waiting for the next polling interval.
#ifdef BTN_USER
static bool gUserButtonInitialized = false;
static bool gUserButtonLastState = false;          // debounced pressed-state
static unsigned long gUserButtonChangeTime = 0;    // last raw transition
static unsigned long gUserButtonLastSyncTime = 0;  // last hub.sync trigger
#define USER_BUTTON_DEBOUNCE_MS 50
#define USER_BUTTON_SYNC_COOLDOWN_MS 30000UL  // throttle hub.sync to once per 30s
#endif

// Temporary continuous-mode "service window" — opened by USER button on
// solar/periodic clients so any inbound notes that arrive at Notehub during
// the window are delivered immediately. Auto-restores to periodic when expired.
static unsigned long gServiceWindowUntil = 0;     // 0 = inactive; else millis() expiry
static bool gServiceWindowActive = false;
#define SERVICE_WINDOW_DURATION_MS 1800000UL  // 30 minutes

// RPM sensor state for Hall effect pulse counting
// We track pulses per monitor that uses an RPM sensor
static unsigned long gRpmLastSampleMillis[MAX_MONITORS] = {0};
static float gRpmLastReading[MAX_MONITORS] = {0.0f};
static int gRpmLastPinState[MAX_MONITORS];  // Initialized dynamically in setup()
// For time-based detection: track time between pulses
static unsigned long gRpmLastPulseTime[MAX_MONITORS] = {0};
static unsigned long gRpmPulsePeriodMs[MAX_MONITORS] = {0};
// For accumulated mode: count pulses between telemetry reports
static volatile uint32_t gRpmAccumulatedPulses[MAX_MONITORS] = {0};
static unsigned long gRpmAccumulatedStartMillis[MAX_MONITORS] = {0};
static bool gRpmAccumulatedInitialized[MAX_MONITORS] = {false};

// Atomic access helpers for volatile pulse counter (protects against future interrupt use)
// On 32-bit ARM (Cortex-M7 in STM32H747XI), 32-bit aligned reads/writes are atomic,
// but we use interrupt guards for portability and read-modify-write safety
static inline uint32_t atomicReadAndResetPulses(uint8_t idx) {
  noInterrupts();
  uint32_t count = gRpmAccumulatedPulses[idx];
  gRpmAccumulatedPulses[idx] = 0;
  interrupts();
  return count;
}

static inline void atomicResetPulses(uint8_t idx) {
  noInterrupts();
  gRpmAccumulatedPulses[idx] = 0;
  interrupts();
}

static inline void atomicIncrementPulses(uint8_t idx) {
  noInterrupts();
  gRpmAccumulatedPulses[idx]++;
  interrupts();
}

static inline uint32_t atomicReadPulses(uint8_t idx) {
  noInterrupts();
  uint32_t count = gRpmAccumulatedPulses[idx];
  interrupts();
  return count;
}

// Default RPM sampling duration in milliseconds (60 seconds for 1 RPM minimum detection)
// To detect 0.1 RPM, use pulseAccumulatedMode=true with sampleSeconds >= 600
#ifndef RPM_SAMPLE_DURATION_MS
#define RPM_SAMPLE_DURATION_MS 60000
#endif

// Helper: Get recommended pulse sampling parameters based on expected rate
// This helps configure optimal sampling for the expected RPM/flow rate range
// Returns: pulseSampleDurationMs, pulseAccumulatedMode recommendations
struct PulseSamplingRecommendation {
  uint32_t sampleDurationMs;  // Recommended sample duration
  bool accumulatedMode;       // Whether to use accumulated mode
  const char *description;    // Human-readable description
};

// Serial log buffer structure for client
struct SerialLogEntry {
  double timestamp;
  char message[160];
};

struct ClientSerialLog {
  SerialLogEntry entries[CLIENT_SERIAL_BUFFER_SIZE];
  uint8_t writeIndex;
  uint8_t count;
};

static ClientSerialLog gSerialLog;
static unsigned long gLastSerialRequestCheckMillis = 0;
static unsigned long gLastLocationRequestCheckMillis = 0;

// Forward declarations
static PulseSamplingRecommendation getRecommendedPulseSampling(float expectedRate);
static inline bool detectPulseEdge(HallEffectSensorType hallType, int lastState, int currentState);
static void startPulseSample(uint8_t idx, const MonitorConfig &cfg);
static float getMonitorHeight(const MonitorConfig &cfg);
static void initializeStorage();
static void ensureConfigLoaded();
static void createDefaultConfig(ClientConfig &cfg);
static bool loadConfigFromFlash(ClientConfig &cfg);
static void initMonitorDefaults(MonitorConfig &mon, uint8_t index);
static void parseMonitorFromJson(MonitorConfig &mon, JsonObjectConst t, uint8_t index);
static bool saveConfigToFlash(const ClientConfig &cfg);
static void printHardwareRequirements(const ClientConfig &cfg);
static bool i2cAck(uint8_t address);
static uint8_t resolveCurrentLoopI2cAddress(uint8_t preferredAddress);
static void initializeNotecard();
static void configureNotecardHubMode();
static void syncTimeFromNotecard();
static double currentEpoch();
static void scheduleNextDailyReport();
static void checkForFirmwareUpdate();
static void enableDfuMode();
static void pollForConfigUpdates();
static void applyConfigUpdate(const JsonDocument &doc);
static bool sendConfigAck(bool success, const char *message, const char *configVersion);
static bool sendOtaReportNote(const char *st, const char *targetV, const char *reason);
static void reportOtaOutcome();
static void persistConfigIfDirty();
static void retryPendingConfigAckIfDue();
static void sampleMonitors();
static float readDigitalSensor(const MonitorConfig &cfg, uint8_t idx);
static float readAnalogSensor(const MonitorConfig &cfg, uint8_t idx);
static float readCurrentLoopSensor(const MonitorConfig &cfg, uint8_t idx);
static float readPulseSensor(const MonitorConfig &cfg, uint8_t idx);
static float readMonitorSensor(uint8_t idx);

// ============================================================================
// Non-blocking Pulse Sampler State Machine
// ============================================================================
// Instead of blocking the main loop with delay(1) for up to 60 seconds,
// the pulse sampler runs as a cooperative state machine. Each call to
// pollPulseSampler() does a short burst of work (up to PULSE_POLL_BURST_MS),
// then returns. When the configured sample duration has elapsed, the result
// is finalized and made available via readPulseSensorResult().
// Between sampling periods, readMonitorSensor() returns the last computed value.
// ============================================================================
#ifndef PULSE_POLL_BURST_MS
#define PULSE_POLL_BURST_MS 50  // Max ms per poll call (keeps loop responsive)
#endif

enum PulseSamplerState : uint8_t {
  PULSE_STATE_IDLE = 0,        // Not sampling, waiting for next sample request
  PULSE_STATE_SAMPLING = 1,    // Actively counting pulses/edges
  PULSE_STATE_COMPLETE = 2     // Sample complete, result ready
};

struct PulseSamplerContext {
  PulseSamplerState state;
  unsigned long sampleStartMs;
  uint32_t sampleDurationMs;
  bool useAccumulatedMode;
  bool useTimeBased;
  uint8_t pulsesPerRev;
  HallEffectSensorType hallType;
  int pin;
  // Pulse counting state
  uint32_t pulseCount;
  unsigned long lastPulseTimeMs;
  int lastPinState;
  // Time-based state
  unsigned long firstPulseTime;
  unsigned long secondPulseTime;
  bool firstPulseDetected;
  bool secondPulseDetected;
  // Result
  float resultRpm;
  bool resultReady;
};

static PulseSamplerContext gPulseSampler[MAX_MONITORS];
static bool gPulseSamplerInitialized = false;

static void initPulseSamplers() {
  if (gPulseSamplerInitialized) return;
  memset(gPulseSampler, 0, sizeof(gPulseSampler));
  gPulseSamplerInitialized = true;
}

// Detect an edge based on hall effect sensor type
static inline bool detectPulseEdge(HallEffectSensorType hallType, int lastState, int currentState) {
  switch (hallType) {
    case HALL_EFFECT_UNIPOLAR:
    case HALL_EFFECT_ANALOG:
      return (lastState == HIGH && currentState == LOW);
    case HALL_EFFECT_BIPOLAR:
    case HALL_EFFECT_OMNIPOLAR:
      return (lastState != currentState);
    default:
      return (lastState == HIGH && currentState == LOW);
  }
}

// Start a new sampling period for a pulse sensor
static void startPulseSample(uint8_t idx, const MonitorConfig &cfg) {
  PulseSamplerContext &ctx = gPulseSampler[idx];

  int pin = (cfg.pulsePin >= 0 && cfg.pulsePin < 255) ? cfg.pulsePin :
            ((cfg.primaryPin >= 0 && cfg.primaryPin < 255) ? cfg.primaryPin : (2 + idx));
  pinMode(pin, INPUT_PULLUP);

  uint8_t pulsesPerRev = (cfg.pulsesPerUnit > 0) ? cfg.pulsesPerUnit : 1;

  // Determine sampling parameters
  uint32_t sampleDurationMs;
  bool useAccumulatedMode;

  if (cfg.pulseSampleDurationMs > 0) {
    sampleDurationMs = cfg.pulseSampleDurationMs;
    useAccumulatedMode = cfg.pulseAccumulatedMode;
  } else if (cfg.expectedPulseRate > 0.0f) {
    PulseSamplingRecommendation rec = getRecommendedPulseSampling(cfg.expectedPulseRate);
    sampleDurationMs = rec.sampleDurationMs;
    useAccumulatedMode = rec.accumulatedMode;
  } else {
    sampleDurationMs = RPM_SAMPLE_DURATION_MS;
    useAccumulatedMode = cfg.pulseAccumulatedMode;
  }

  ctx.state = PULSE_STATE_SAMPLING;
  ctx.sampleStartMs = millis();
  ctx.sampleDurationMs = sampleDurationMs;
  ctx.useAccumulatedMode = useAccumulatedMode;
  ctx.useTimeBased = (!useAccumulatedMode && cfg.hallEffectDetection == HALL_DETECT_TIME_BASED);
  ctx.pulsesPerRev = pulsesPerRev;
  ctx.hallType = cfg.hallEffectType;
  ctx.pin = pin;
  ctx.pulseCount = 0;
  ctx.lastPulseTimeMs = 0;
  ctx.lastPinState = digitalRead(pin);
  ctx.firstPulseTime = 0;
  ctx.secondPulseTime = 0;
  ctx.firstPulseDetected = false;
  ctx.secondPulseDetected = false;
  ctx.resultReady = false;

  // For accumulated mode, use the global accumulated counter
  if (useAccumulatedMode && !gRpmAccumulatedInitialized[idx]) {
    atomicResetPulses(idx);
    gRpmAccumulatedStartMillis[idx] = millis();
    gRpmLastPinState[idx] = ctx.lastPinState;
    gRpmAccumulatedInitialized[idx] = true;
  }
}

// Poll the pulse sampler for a short burst. Returns true when the sample is complete.
static bool pollPulseSampler(uint8_t idx) {
  PulseSamplerContext &ctx = gPulseSampler[idx];
  if (ctx.state != PULSE_STATE_SAMPLING) return (ctx.state == PULSE_STATE_COMPLETE);

  const unsigned long DEBOUNCE_MS = 2;
  const float MS_PER_MINUTE = 60000.0f;
  unsigned long now = millis();
  unsigned long elapsed = now - ctx.sampleStartMs;

  // --- ACCUMULATED MODE ---
  if (ctx.useAccumulatedMode) {
    // Short burst to catch pulses (up to PULSE_POLL_BURST_MS)
    unsigned long burstStart = millis();
    int lastState = gRpmLastPinState[idx];
    unsigned long lastPulseTime = ctx.lastPulseTimeMs;

    while ((millis() - burstStart) < PULSE_POLL_BURST_MS) {
      int currentState = digitalRead(ctx.pin);
      if (detectPulseEdge(ctx.hallType, lastState, currentState)) {
        unsigned long pt = millis();
        if (pt - lastPulseTime >= DEBOUNCE_MS) {
          atomicIncrementPulses(idx);
          lastPulseTime = pt;
        }
      }
      lastState = currentState;
    }
    gRpmLastPinState[idx] = lastState;
    ctx.lastPulseTimeMs = lastPulseTime;

    // Accumulated mode: finalize when the telemetry sample interval has elapsed
    // (the caller drives this via sampleDurationMs, typically == sampleSeconds * 1000)
    unsigned long accumElapsed = now - gRpmAccumulatedStartMillis[idx];
    if (accumElapsed >= ctx.sampleDurationMs || (accumElapsed > 1000 && elapsed >= ctx.sampleDurationMs)) {
      uint32_t pulseCount = atomicReadAndResetPulses(idx);
      if (accumElapsed > 1000 && pulseCount > 0) {
        ctx.resultRpm = ((float)pulseCount * MS_PER_MINUTE) /
                        ((float)accumElapsed * (float)ctx.pulsesPerRev);
      } else {
        ctx.resultRpm = 0.0f;
      }
      gRpmAccumulatedStartMillis[idx] = now;
      ctx.resultReady = true;
      ctx.state = PULSE_STATE_COMPLETE;
      return true;
    }
    return false;
  }

  // Check if sample duration has elapsed (for non-accumulated modes)
  bool timeUp = (elapsed >= ctx.sampleDurationMs);

  // --- TIME-BASED MODE ---
  if (ctx.useTimeBased) {
    if (!ctx.secondPulseDetected) {
      unsigned long burstStart = millis();
      int lastState = ctx.lastPinState;

      while ((millis() - burstStart) < PULSE_POLL_BURST_MS && !ctx.secondPulseDetected) {
        int currentState = digitalRead(ctx.pin);
        if (detectPulseEdge(ctx.hallType, lastState, currentState)) {
          unsigned long pt = millis();
          if (pt - ctx.lastPulseTimeMs >= DEBOUNCE_MS) {
            ctx.lastPulseTimeMs = pt;
            if (!ctx.firstPulseDetected) {
              ctx.firstPulseTime = pt;
              ctx.firstPulseDetected = true;
            } else {
              ctx.secondPulseTime = pt;
              ctx.secondPulseDetected = true;
            }
          }
        }
        lastState = currentState;
      }
      ctx.lastPinState = lastState;
    }

    // Early exit if we got both pulses, or time is up
    if (ctx.secondPulseDetected || timeUp) {
      if (ctx.secondPulseDetected) {
        unsigned long period = ctx.secondPulseTime - ctx.firstPulseTime;
        if (period > 0) {
          ctx.resultRpm = MS_PER_MINUTE / ((float)period * (float)ctx.pulsesPerRev);
        } else {
          ctx.resultRpm = gRpmLastReading[idx];
        }
      } else {
        ctx.resultRpm = gRpmLastReading[idx]; // Keep last reading
      }
      ctx.resultReady = true;
      ctx.state = PULSE_STATE_COMPLETE;
      return true;
    }
    return false;
  }

  // --- PULSE COUNTING MODE ---
  {
    unsigned long burstStart = millis();
    int lastState = ctx.lastPinState;

    while ((millis() - burstStart) < PULSE_POLL_BURST_MS) {
      int currentState = digitalRead(ctx.pin);
      if (detectPulseEdge(ctx.hallType, lastState, currentState)) {
        unsigned long pt = millis();
        if (pt - ctx.lastPulseTimeMs >= DEBOUNCE_MS) {
          ctx.pulseCount++;
          ctx.lastPulseTimeMs = pt;
        }
      }
      lastState = currentState;
    }
    ctx.lastPinState = lastState;

    if (timeUp) {
      ctx.resultRpm = ((float)ctx.pulseCount * MS_PER_MINUTE) /
                      ((float)ctx.sampleDurationMs * (float)ctx.pulsesPerRev);
      ctx.resultReady = true;
      ctx.state = PULSE_STATE_COMPLETE;
      return true;
    }
    return false;
  }
}

// Read the pulse sensor result for a given index.
// If a sample is still in progress, returns the last known reading.
// If a sample just completed, returns the new result and resets state.
static float readPulseSensorResult(uint8_t idx) {
  PulseSamplerContext &ctx = gPulseSampler[idx];
  if (ctx.resultReady) {
    float rpm = ctx.resultRpm;
    gRpmLastReading[idx] = rpm;
    ctx.state = PULSE_STATE_IDLE;
    ctx.resultReady = false;
    return rpm;
  }
  return gRpmLastReading[idx];
}
static void evaluateAlarms(uint8_t idx);
static void sendTelemetry(uint8_t idx, const char *reason, bool syncNow);
static float getEffectiveBatteryVoltage();
static void sendRegistration(const char *reason);
static void sendAlarm(uint8_t idx, const char *alarmType, float inches);
static bool checkAlarmRateLimit(uint8_t idx, const char *alarmType);
static void sendDailyReport();
static void publishNote(const char *fileName, JsonDocument &doc, bool syncNow);
static void bufferNoteForRetry(const char *fileName, const char *payload, bool syncNow);
static void flushBufferedNotes();
static void pruneNoteBufferIfNeeded();
static void trimTelemetryOutbox();
static void ensureTimeSync();
static void updateDailyScheduleIfNeeded();
static bool checkNotecardHealth();
static bool appendDailyMonitor(JsonDocument &doc, JsonArray &array, uint8_t monitorIndex, size_t payloadLimit);
static void pollForRelayCommands();
static void processRelayCommand(const JsonDocument &doc);
static void setRelayState(uint8_t relayNum, bool state);
static void initializeRelays();
static void triggerRemoteRelays(const char *targetClient, uint8_t relayMask, bool activate);
static int getRelayPin(uint8_t relayIndex);
static float readVinDividerVoltage();
static void checkRelayMomentaryTimeout(unsigned long now);
static bool fetchNotecardLocation(float &latitude, float &longitude);
static void resetRelayForMonitor(uint8_t idx);
static void initializeClearButton();
static void checkClearButton(unsigned long now);
static void initializeUserButton();
static void checkUserButton(unsigned long now);
static void openServiceWindow(unsigned long now);
static void checkServiceWindowExpiry(unsigned long now);
static void clearAllRelayAlarms();
static void addSerialLog(const char *message);
static void pollForSerialRequests();
static void pollForLocationRequests();
static void pollForSyncRequests();
static void sendSerialLogs();
static void sendSerialAck(const char *status);
static void evaluateUnload(uint8_t idx);
static void sendUnloadEvent(uint8_t idx, float peakInches, float currentInches, double peakEpoch);
static void sendSolarAlarm(SolarAlertType alertType);
static void logSolarHardwareTestHeartbeat(unsigned long now);
static void logSolarPollSnapshot(unsigned long now, SolarAlertType alertType);
static bool appendSolarDataToDaily(JsonDocument &doc);
static void recoverI2CBus();
static void logI2CRecoveryEvent(I2CRecoveryTrigger trigger);
// Battery voltage monitoring (sourced from SunSaver MPPT or Vin divider via
// getEffectiveBatteryVoltage(); Fix 11: Notecard card.voltage is never used).
static bool pollBatteryVoltage(BatteryData &data, const BatteryConfig &cfg);
static void checkBatteryAlerts(const BatteryData &data, const BatteryConfig &cfg);
static void sendBatteryAlarm(BatteryAlertType alertType, float voltage);
static bool appendBatteryDataToDaily(JsonDocument &doc);
// Power conservation
static void updatePowerState();
static void sendPowerStateChange(PowerState oldState, PowerState newState, float voltage);
static const char* getPowerStateDescription(PowerState state);
static unsigned long getPowerStateSleepMs(PowerState state);
// Solar-only (no battery) mode
static void loadSolarStateFromFlash();
static void saveSolarStateToFlash();
// Firmware-update detection: compares persisted build seq to FIRMWARE_BUILD_SEQ at boot
static void checkFirmwareUpdateMarker();
static void performStartupDebounce();
static void checkSolarOnlySunsetProtocol(unsigned long now);
static bool isSensorVoltageGateOpen();
// Diagnostics
static uint32_t freeRam();
static void safeSleep(unsigned long ms);
#ifdef TANKALARM_HEALTH_TELEMETRY_ENABLED
static void sendHealthTelemetry();
#endif

static bool i2cAck(uint8_t address) {
  if (address < 0x08 || address > 0x77) {
    return false;
  }
  Wire.beginTransmission(address);
  return (Wire.endTransmission() == 0);
}

static uint8_t resolveCurrentLoopI2cAddress(uint8_t preferredAddress) {
  const uint8_t fallbackAddress = CURRENT_LOOP_I2C_ADDRESS;
  const uint8_t candidates[] = {
    preferredAddress,
    fallbackAddress,
    CURRENT_LOOP_I2C_ALT_ADDRESS_1,
    CURRENT_LOOP_I2C_ALT_ADDRESS_2
  };

  for (uint8_t idx = 0; idx < (uint8_t)(sizeof(candidates) / sizeof(candidates[0])); ++idx) {
    uint8_t candidate = candidates[idx];
    if (candidate < 0x08 || candidate > 0x77 || candidate == NOTECARD_I2C_ADDRESS) {
      continue;
    }

    bool duplicate = false;
    for (uint8_t prev = 0; prev < idx; ++prev) {
      if (candidates[prev] == candidate) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }

    if (i2cAck(candidate)) {
      return candidate;
    }
  }

  if (preferredAddress >= 0x08 && preferredAddress <= 0x77 && preferredAddress != NOTECARD_I2C_ADDRESS) {
    return preferredAddress;
  }
  return fallbackAddress;
}

// v2.0.47: Run the official Arduino_Opta_Blueprint controller bootstrap ONCE to discover
// the A0602's assigned I2C address (Blueprint assigns from OPTA_CONTROLLER_FIRST_AVAILABLE_ADDRESS
// = 0x0B upward). This replaces blind probing of the legacy 0x64 / 0x0A defaults that the field
// A0602 was never at -- the root cause of the "stuck ~18 mA" symptom: every Blueprint frame
// was silently NACKed at the wrong address so the read path returned stale/garbage bytes.
//
// Side effects of OptaController::begin():
//  - Calls Wire.begin() (idempotent) and Wire.setClock(400000). We restore 100 kHz before
//    returning so the next Notecard transaction sees the documented-stable bus speed.
//  - Emits OPTA_CONTROLLER_RESET to every expansion and re-runs address assignment. This is
//    expensive; MUST NOT be called outside setup(). After bootstrap we do NOT call
//    OptaController.update() in the hot loop, so the controller never races the v2.0.46
//    raw framed read path for the shared bus.
//
// Returns true if an EXPANSION_OPTA_ANALOG was discovered (LED turns GREEN on the module).
// On false the caller should fall back to legacy probing -- callable raw frames at the legacy
// addresses still work if the module was previously addressed.
static bool bootstrapA0602Managed() {
  OptaController.begin();
  OptaController.update();
  delay(20);
  OptaController.update();

  int n = OptaController.getExpansionNum();
  for (int i = 0; i < n; ++i) {
    if (OptaController.getExpansionType(i) == EXPANSION_OPTA_ANALOG) {
      gA0602DeviceIndex    = i;
      gA0602ManagedAddress = OptaController.getExpansionI2Caddress(i);
      break;
    }
  }
  gOptaControllerReady = (gA0602DeviceIndex >= 0);

  // Restore the Notecard-safe I2C clock (OptaController forced 400 kHz). Mirrors the
  // v2.0.46 per-op clock-window discipline used inside the raw read path.
  Wire.setClock(I2C_NORMAL_CLOCK_HZ);
  return gOptaControllerReady;
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) {
    delay(10);
  }

  Serial.println();
  Serial.print(F("Tank Alarm Client 112025 v"));
  Serial.print(F(FIRMWARE_VERSION));
  Serial.print(F(" ("));
  Serial.print(F(FIRMWARE_BUILD_DATE));
  Serial.println(F(")"));

#if defined(TANKALARM_DFU_MCUBOOT)
  Serial.println(F("  OTA: MCUboot ENABLED"));
#else
  Serial.println(F("  OTA: MCUboot DISABLED (no -DTANKALARM_DFU_MCUBOOT)"));
#endif

#if defined(TANKALARM_DFU_MCUBOOT)
  // Confirm this image unconditionally as early as possible so MCUboot does not
  // roll back even when QSPI storage or Notecard are unavailable (e.g. right
  // after a USB flash that erased the QSPI MBR). The loop() gate below provides
  // a belt-and-suspenders second confirmation once peripherals are fully up.
  MCUboot::confirmSketch();
  Serial.println(F("MCUboot: sketch confirmed (early, unconditional)"));
#endif

  // Initialize serial log buffer
  memset(&gSerialLog, 0, sizeof(ClientSerialLog));

  // Set analog resolution to 12-bit to match the /4095.0f divisor used in readMonitorSensor
  analogReadResolution(12);

  initializeStorage();
  ensureConfigLoaded();

#if defined(SOLAR_HW_TEST_SERIAL) && defined(SOLAR_HW_TEST_FORCE_SOLAR_CONFIG)
  // Test override so RS-485 bring-up can be exercised without editing flash JSON.
  gConfig.solarCharger.enabled = true;
  gConfig.solarCharger.modbusSlaveId = 1;
  gConfig.solarCharger.modbusBaudRate = 9600;
  gConfig.solarCharger.modbusTimeoutMs = 500;
  gConfig.solarCharger.pollIntervalSec = 10;
  gConfig.solarCharger.alertOnCommFailure = true;
  Serial.println(F("Solar HW test: forced solarCharger config enabled"));
#endif

  printHardwareRequirements(gConfig);

  Wire.begin();
  Wire.setTimeout(I2C_WIRE_TIMEOUT_MS);  // Guard against indefinite blocking on bus hang

  // v2.0.47: A0602 managed bootstrap -- one-shot Blueprint controller handshake to discover
  // the real assigned I2C address (0x0B+) and turn the A0602 status LED GREEN. Restores the
  // 100 kHz Notecard-safe clock before returning. Must run after Wire.begin() and BEFORE the
  // bus scan + Notecard init below so the rest of setup() addresses the A0602 correctly.
  bool a0602Managed = bootstrapA0602Managed();
  if (a0602Managed) {
    Serial.print(F("A0602: managed at 0x"));
    if (gA0602ManagedAddress < 0x10) Serial.print('0');
    Serial.print(gA0602ManagedAddress, HEX);
    Serial.print(F(" (device index "));
    Serial.print(gA0602DeviceIndex);
    Serial.println(F(")"));
    // Steer the v2.0.46 raw framed read path at the actually-assigned address rather than
    // the legacy 0x64 / 0x0A defaults the field A0602 was never at.
    gConfig.currentLoopI2cAddress = gA0602ManagedAddress;
  } else {
    Serial.println(F("A0602: managed bootstrap FAILED -- falling back to legacy address probe"));
  }

  uint8_t configuredCurrentLoopAddr = gConfig.currentLoopI2cAddress;
  if (configuredCurrentLoopAddr < 0x08 || configuredCurrentLoopAddr > 0x77 || configuredCurrentLoopAddr == NOTECARD_I2C_ADDRESS) {
    configuredCurrentLoopAddr = CURRENT_LOOP_I2C_ADDRESS;
  }
  uint8_t resolvedCurrentLoopAddr = resolveCurrentLoopI2cAddress(configuredCurrentLoopAddr);
  if (resolvedCurrentLoopAddr != configuredCurrentLoopAddr) {
    Serial.print(F("I2C: current loop address override 0x"));
    if (configuredCurrentLoopAddr < 0x10) Serial.print('0');
    Serial.print(configuredCurrentLoopAddr, HEX);
    Serial.print(F(" -> 0x"));
    if (resolvedCurrentLoopAddr < 0x10) Serial.print('0');
    Serial.println(resolvedCurrentLoopAddr, HEX);
  }
  gConfig.currentLoopI2cAddress = resolvedCurrentLoopAddr;

  // ---- I2C bus scan: verify expected devices are present ----
  {
    const uint8_t expectedAddrs[] = { NOTECARD_I2C_ADDRESS, resolvedCurrentLoopAddr };
    const char *expectedNames[] = { "Notecard", "A0602 Current Loop" };
    I2CScanResult scanResult = tankalarm_scanI2CBus(expectedAddrs, expectedNames, 2);

    // Persist results for first health telemetry report (3.2.5)
    gStartupScanRetries = scanResult.retryCount;
    gStartupUnexpectedDevices = scanResult.unexpectedCount;

    // Determine per-device status via quick probes
    Wire.beginTransmission(NOTECARD_I2C_ADDRESS);
    gStartupNotecardFound = (Wire.endTransmission() == 0);
    Wire.beginTransmission(resolvedCurrentLoopAddr);
    gStartupCurrentLoopFound = (Wire.endTransmission() == 0);
  }

  initializeNotecard();
#if defined(TANKALARM_DFU_MCUBOOT)
  tankalarm_otaSelfCheck();   // print OTA partition readiness for diagnostics
  tankalarm_resolvePendingOta(notecard);
#endif
  ensureTimeSync();
  scheduleNextDailyReport();

#ifdef TANKALARM_WATCHDOG_AVAILABLE
  // Initialize watchdog timer
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    // Mbed OS Watchdog
    uint32_t timeoutMs = WATCHDOG_TIMEOUT_SECONDS * 1000;
    if (mbedWatchdog.start(timeoutMs)) {
      Serial.print(F("Mbed Watchdog enabled: "));
      Serial.print(WATCHDOG_TIMEOUT_SECONDS);
      Serial.println(F(" seconds"));
    } else {
      Serial.println(F("Warning: Watchdog initialization failed"));
    }
  #else
    // STM32duino Watchdog
    IWatchdog.begin(WATCHDOG_TIMEOUT_SECONDS * 1000000UL);
    Serial.print(F("Watchdog timer enabled: "));
    Serial.print(WATCHDOG_TIMEOUT_SECONDS);
    Serial.println(F(" seconds"));
  #endif
#else
  Serial.println(F("Warning: Watchdog timer not available on this platform"));
#endif

  // Initialize RPM sensor state arrays dynamically
  for (uint8_t i = 0; i < MAX_MONITORS; ++i) {
    // Read actual pin state if RPM pin is configured, to avoid phantom edge detection
    int rpmPin = (i < gConfig.monitorCount) ? gConfig.monitors[i].pulsePin : -1;
    if (rpmPin >= 0) {
      pinMode(rpmPin, INPUT_PULLUP);
      gRpmLastPinState[i] = digitalRead(rpmPin);
    } else {
      gRpmLastPinState[i] = HIGH;
    }
    gRpmLastSampleMillis[i] = 0;
    gRpmLastReading[i] = 0.0f;
    gRpmLastPulseTime[i] = 0;
    gRpmPulsePeriodMs[i] = 0;
  }

  // Explicitly initialize relay runtime state
  for (uint8_t i = 0; i < MAX_RELAYS; ++i) {
    gRelayRuntime[i] = {};
  }

  for (uint8_t i = 0; i < gConfig.monitorCount; ++i) {
    gMonitorState[i].currentInches = 0.0f;
    gMonitorState[i].currentSensorMa = 0.0f;
    gMonitorState[i].currentSensorVoltage = 0.0f;
    gMonitorState[i].lastReadingEpoch = 0.0;
    gMonitorState[i].sampleReused = false;
    gMonitorState[i].lastReportedValue = -9999.0f;
    gMonitorState[i].lastDailySentValue = -9999.0f;
    gMonitorState[i].highAlarmLatched = false;
    gMonitorState[i].lowAlarmLatched = false;
    gMonitorState[i].lastSampleMillis = 0;
    gMonitorState[i].lastAlarmSendMillis = 0;
    gMonitorState[i].highAlarmDebounceCount = 0;
    gMonitorState[i].lowAlarmDebounceCount = 0;
    gMonitorState[i].highClearDebounceCount = 0;
    gMonitorState[i].lowClearDebounceCount = 0;
    gMonitorState[i].lastValidReading = 0.0f;
    gMonitorState[i].hasLastValidReading = false;
    gMonitorState[i].consecutiveFailures = 0;
    gMonitorState[i].stuckReadingCount = 0;
    gMonitorState[i].sensorFailed = false;
    gMonitorState[i].recoveryCount = 0;
    gMonitorState[i].alarmCount = 0;
    // BugFix v1.6.2 (M-13): Initialize last-alarm timestamps so the first alarm
    // after boot is NOT suppressed by the per-type minimum-interval check.
    // Setting them to (now - interval - 1) ensures the very first alarm passes.
    {
      unsigned long bootNow = millis();
      unsigned long expired = bootNow - (MIN_ALARM_INTERVAL_SECONDS * 1000UL + 1);
      // If millis() is still tiny (< interval), use 0 which also won't suppress
      // because the unsigned subtraction in the rate-limit check will wrap large.
      if (bootNow < MIN_ALARM_INTERVAL_SECONDS * 1000UL + 1) {
        expired = 0;
      }
      gMonitorState[i].lastHighAlarmMillis = expired;
      gMonitorState[i].lastLowAlarmMillis = expired;
      gMonitorState[i].lastClearAlarmMillis = expired;
      gMonitorState[i].lastSensorFaultMillis = expired;
    }
    // Initialize unload tracking state
    gMonitorState[i].unloadPeakInches = 0.0f;
    gMonitorState[i].unloadPeakSensorMa = 0.0f;
    gMonitorState[i].unloadPeakEpoch = 0.0;
    gMonitorState[i].unloadTracking = false;
    for (uint8_t j = 0; j < MAX_ALARMS_PER_HOUR; ++j) {
      gMonitorState[i].alarmTimestamps[j] = 0;
    }
  }

  initializeRelays();
  initializeClearButton();
  initializeUserButton();
  
  // Initialize solar/battery charger monitoring (SunSaver MPPT via RS-485)
  if (gConfig.solarCharger.enabled) {
    if (gSolarManager.begin(gConfig.solarCharger)) {
      if (gSolarManager.isCommunicationOk()) {
        Serial.println(F("Solar charger monitoring enabled"));
        addSerialLog("Solar charger monitoring initialized");
      } else {
        Serial.println(F("Solar charger transport initialized, initial Modbus read failed"));
        addSerialLog("Solar charger initial Modbus read failed");
      }
    } else {
      Serial.println(F("Warning: Solar charger initialization failed"));
      addSerialLog("Solar charger init failed");
    }
  }

  // Initialize battery voltage monitoring
  // Fix 11: source is auto-picked by getEffectiveBatteryVoltage() (SunSaver MPPT preferred,
  // analog Vin divider as fallback, or nothing). No Notecard I/O and no platform compile
  // guard required — the previous Fix 10 Opta gate is no longer needed here because
  // pollBatteryVoltage() never touches the Notecard.
  if (gConfig.batteryMonitor.enabled) {
    Serial.println(F("Battery voltage monitoring enabled (source: MPPT/Vin auto-select)"));
    addSerialLog("Battery voltage monitoring initialized");
    // Initialize battery data structure
    memset(&gBatteryData, 0, sizeof(BatteryData));
    // Do initial poll
    pollBatteryVoltage(gBatteryData, gConfig.batteryMonitor);
  }

  // Initialize analog Vin voltage divider monitoring
  if (gConfig.vinMonitor.enabled) {
    float ratio = vinDividerRatio(&gConfig.vinMonitor);
    float maxV = vinMaxReadableVoltage(&gConfig.vinMonitor);
    Serial.print(F("Vin monitor enabled: pin=A"));
    Serial.print(gConfig.vinMonitor.analogPin);
    Serial.print(F(" R1="));
    Serial.print(gConfig.vinMonitor.r1Kohm, 1);
    Serial.print(F("k R2="));
    Serial.print(gConfig.vinMonitor.r2Kohm, 1);
    Serial.print(F("k ratio="));
    Serial.print(ratio, 4);
    Serial.print(F(" maxV="));
    Serial.println(maxV, 1);
    addSerialLog("Vin voltage divider monitoring initialized");
    // Do initial read
    gVinVoltage = readVinDividerVoltage();
  }

  // Solar-Only (No Battery) mode initialization
  if (isSolarOnlyActive()) {
    loadSolarStateFromFlash();
    gSolarOnlyBootCount++;
    Serial.print(F("Solar-only mode: boot #"));
    Serial.println(gSolarOnlyBootCount);
    if (gSolarOnlyLastReportEpoch > 0.0) {
      Serial.print(F("  Last report epoch: "));
      Serial.println((unsigned long)gSolarOnlyLastReportEpoch);
    }
    addSerialLog("Solar-only mode active");
    // Perform startup debounce (blocks until power is stable)
    performStartupDebounce();
    // Save updated boot count
    saveSolarStateToFlash();
  } else {
    // Non-solar-only: startup is always complete
    gSolarOnlyStartupComplete = true;
    gSolarOnlySensorsReady = true;
  }

  // Detect a just-installed firmware (version changed since the last boot, via OTA or USB).
  // When true, we force an immediate sync after the boot note below so transmission can be
  // confirmed right away instead of waiting for the next periodic/daily sync window.
  checkFirmwareUpdateMarker();

  // Immediate boot telemetry so the server sees this device right away
  // instead of waiting for the first sample interval (default 30 min).
  // Solar-only clients with monitors configured skip this to avoid
  // wasting power on brownout reboots — they rely on the loop interval.
  // However, unconfigured solar-only clients (monitorCount == 0) DO send
  // a registration note at boot because there is no other way for the
  // server to discover them and push an initial configuration.
#ifdef TANKALARM_WATCHDOG_AVAILABLE
  // Reset watchdog before boot telemetry — the Notecard I2C transaction
  // inside publishNote() can block for up to 30 s, which combined with
  // post-watchdog-enable init could exceed the watchdog window.
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    mbedWatchdog.kick();
  #else
    IWatchdog.reload();
  #endif
#endif
  if (gConfig.monitorCount > 0) {
    if (!isSolarOnlyActive()) {
      Serial.println(F("Sending boot telemetry..."));
      addSerialLog("Boot telemetry");
      sampleMonitors();
    }
    // Solar clients with monitors rely on loop sample interval
  } else {
    // No monitors configured — send a lightweight registration note
    // so the server discovers this device and can push a config to it.
    // This is safe for solar-only clients: the registration payload is
    // tiny, startup debounce has already confirmed stable power, and
    // the device will never be discoverable otherwise.
    Serial.println(F("No monitors configured — sending registration..."));
    addSerialLog("Boot registration (no monitors)");
    sendRegistration("boot");
  }

  // Firmware-update confirmation: if a new firmware version just booted, force an immediate
  // sync so the boot note above (telemetry for configured clients, registration otherwise)
  // transmits right now. This lets an operator verify two-way communication immediately after
  // an update rather than waiting up to the periodic outbound window (6 h on solar clients).
  if (gFirmwareJustUpdated) {
    Serial.println(F("Firmware updated — forcing immediate sync to confirm transmission"));
    addSerialLog("Firmware updated - immediate transmission test");
#ifdef TANKALARM_WATCHDOG_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    mbedWatchdog.kick();
  #else
    IWatchdog.reload();
  #endif
#endif
    J *fwSyncReq = notecard.newRequest("hub.sync");
    if (fwSyncReq) {
      J *fwSyncRsp = notecard.requestAndResponse(fwSyncReq);
      if (fwSyncRsp) {
        const char *fwSyncErr = JGetString(fwSyncRsp, "err");
        if (fwSyncErr && fwSyncErr[0] != '\0') {
          Serial.print(F("Firmware-update sync warning: "));
          Serial.println(fwSyncErr);
        } else {
          Serial.println(F("Firmware-update confirmation sync initiated"));
        }
        notecard.deleteResponse(fwSyncRsp);
      }
    }
  }

  // Report any pending OTA outcome (applied/reverted) to the server so the dashboard reflects
  // update success/failure instead of it only living in Notehub's dfu.status.
  reportOtaOutcome();

  Serial.println(F("Client setup complete"));
  tankalarm_printHeapStats();
  addSerialLog("Client started successfully");
}

void loop() {
#if defined(TANKALARM_DFU_MCUBOOT)
  // Belt-and-suspenders secondary confirmation once peripherals are up.
  // The unconditional early confirmSketch() in setup() is the primary gate;
  // this catches the case where loop() is reached with Notecard available.
  tankalarm_markFirmwareHealthy();
#endif

  unsigned long now = millis();

  logSolarHardwareTestHeartbeat(now);

#ifdef TANKALARM_WATCHDOG_AVAILABLE
  // Reset watchdog timer to prevent system reset
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    mbedWatchdog.kick();
  #else
    IWatchdog.reload();
  #endif
#endif

  // Check notecard health periodically (with exponential backoff)
  // Use remote-tunable base interval if configured, otherwise compile-time default
  static unsigned long lastHealthCheck = 0;
  const unsigned long healthBaseInterval = (gConfig.healthCheckBaseIntervalMs > 0) 
    ? gConfig.healthCheckBaseIntervalMs 
    : NOTECARD_HEALTH_CHECK_BASE_INTERVAL_MS;
  static unsigned long healthCheckInterval = NOTECARD_HEALTH_CHECK_BASE_INTERVAL_MS;
  if (now - lastHealthCheck > healthCheckInterval) {
    lastHealthCheck = now;
    if (!gNotecardAvailable) {
      bool recovered = checkNotecardHealth();
      if (recovered) {
        // Notecard recovered — reset backoff to base interval
        healthCheckInterval = healthBaseInterval;
        Serial.println(F("Notecard health check interval reset to base"));
      } else {
        // Still failing — exponential backoff up to max
        if (healthCheckInterval < NOTECARD_HEALTH_CHECK_MAX_INTERVAL_MS) {
          healthCheckInterval *= 2;
          if (healthCheckInterval > NOTECARD_HEALTH_CHECK_MAX_INTERVAL_MS) {
            healthCheckInterval = NOTECARD_HEALTH_CHECK_MAX_INTERVAL_MS;
          }
        }
        // Enforce higher floor in low-power states to conserve battery
        if (gPowerState >= POWER_STATE_LOW_POWER && healthCheckInterval < 1200000UL) {
          healthCheckInterval = 1200000UL;  // 20 min floor in LOW_POWER+
        } else if (gPowerState >= POWER_STATE_ECO && healthCheckInterval < 600000UL) {
          healthCheckInterval = 600000UL;   // 10 min floor in ECO
        }
        Serial.print(F("Notecard health check backoff: next in "));
        Serial.print(healthCheckInterval / 60000UL);
        Serial.println(F(" min"));
      }
    }
  }

  // ---- Prolonged I2C failure detection ----
  // If both Notecard AND all current-loop sensors are failing for an extended
  // period, the system is in a "zombie" state (alive but blind).  Force a
  // hardware reset via the watchdog rather than running indefinitely broken.
  {
    static uint32_t consecutiveTotalI2cFailLoops = 0;
    bool notecardDown = !gNotecardAvailable;
    bool anyCurrentLoopFailed = false;
    for (uint8_t i = 0; i < gConfig.monitorCount; i++) {
      if (gConfig.monitors[i].sensorInterface == SENSOR_CURRENT_LOOP &&
          gMonitorState[i].consecutiveFailures >= SENSOR_FAILURE_THRESHOLD) {
        anyCurrentLoopFailed = true;
        break;
      }
    }
    if (notecardDown && anyCurrentLoopFailed) {
      consecutiveTotalI2cFailLoops++;
      if (consecutiveTotalI2cFailLoops == I2C_DUAL_FAIL_RECOVERY_LOOPS) {
        // First escalation: attempt I2C bus recovery
        Serial.println(F("I2C: sustained dual failure — attempting bus recovery"));
        recoverI2CBus();
        logI2CRecoveryEvent(I2C_RECOVERY_DUAL_FAILURE);
        tankalarm_ensureNotecardBinding(notecard);
        // Fix C1 (v2.0.50): the A0602's Blueprint-assigned address lives in its own MCU's
        // RAM and normally survives a tankalarm_recoverI2CBus() SCL-toggle cycle. BUT if
        // the module was externally power-cycled (brownout, loop-power loss, hardware reset)
        // it lost its 0x0B assignment and is now listening at the OPTA_DEFAULT_SLAVE_I2C_ADDRESS
        // (0x0A == CURRENT_LOOP_I2C_ALT_ADDRESS_1). A lightweight re-probe finds it and
        // steers gConfig.currentLoopI2cAddress at runtime (not persisted). We deliberately do
        // NOT call bootstrapA0602Managed() here — see the comment block at its definition for
        // why (OPTA_CONTROLLER_RESET is broadcast + expensive). See review document
        // CODE_REVIEW_06252026_CURRENT_LOOP_SENSORS.md §10 for full rationale.
        if (!gDfuInProgress) {
          delay(50);  // brief peripheral settle after Wire.end()/begin() (§7.1)
          uint8_t prev = gConfig.currentLoopI2cAddress;
          uint8_t resolved = resolveCurrentLoopI2cAddress(prev);
          if (resolved != prev) {
            gConfig.currentLoopI2cAddress = resolved;  // runtime steering only
            Serial.print(F("A0602: post-recovery re-probe address change 0x"));
            if (prev < 0x10) Serial.print('0');
            Serial.print(prev, HEX);
            Serial.print(F(" -> 0x"));
            if (resolved < 0x10) Serial.print('0');
            Serial.println(resolved, HEX);
          }
          if (resolved == 0x0A) {
            // §9.3: distinguish "address found" from "sensor working." A re-probe to 0x0A
            // means the A0602 is at its UNMANAGED default; raw framed reads MAY or MAY NOT
            // be serviced by an unmanaged module. Operator should treat this as a hint that
            // a maintenance-window re-management may be needed.
            Serial.println(F("WARNING: A0602 at unmanaged default 0x0A after recovery; "
                             "framed reads may still fail until module is re-managed"));
          }
        }
      } else if (consecutiveTotalI2cFailLoops >= I2C_DUAL_FAIL_RESET_LOOPS) {
        // Prolonged dual failure — force watchdog reset
        Serial.println(F("FATAL: Prolonged I2C failure on all buses. Forcing reset."));
        // Stop kicking the watchdog — triggers hardware reset within 30s
        while (true) { delay(100); }
      }
    } else {
      consecutiveTotalI2cFailLoops = 0;
    }
  }

  // ---- Current-loop-only I2C failure detection ----
  // If all current-loop sensors are failing but Notecard is fine, the A0602
  // expansion may have locked up.  Attempt bus recovery independently.
  // Uses exponential backoff to avoid excessive recovery cycles on
  // persistent hardware faults (threshold doubles each time, capped at
  // I2C_SENSOR_RECOVERY_MAX_BACKOFF × base threshold).
  {
    static uint16_t consecutiveSensorOnlyFailLoops = 0;
    static uint8_t sensorRecoveryBackoff = 1;
    static uint8_t sensorRecoveryTotalAttempts = 0;
    if (gNotecardAvailable) {  // Notecard is OK — isolate to sensor bus
      bool allCurrentLoopFailed = false;
      uint8_t currentLoopCount = 0;
      uint8_t failedCount = 0;
      for (uint8_t i = 0; i < gConfig.monitorCount; i++) {
        if (gConfig.monitors[i].sensorInterface == SENSOR_CURRENT_LOOP) {
          currentLoopCount++;
          if (gMonitorState[i].consecutiveFailures >= SENSOR_FAILURE_THRESHOLD) {
            failedCount++;
          }
        }
      }
      allCurrentLoopFailed = (currentLoopCount > 0 && failedCount == currentLoopCount);
      if (allCurrentLoopFailed) {
        consecutiveSensorOnlyFailLoops++;
        uint16_t effectiveThreshold = (uint16_t)I2C_SENSOR_ONLY_RECOVERY_THRESHOLD * sensorRecoveryBackoff;
        if (consecutiveSensorOnlyFailLoops >= effectiveThreshold) {
          if (sensorRecoveryTotalAttempts >= I2C_SENSOR_RECOVERY_MAX_ATTEMPTS) {
            // Circuit breaker: stop retrying after max total attempts
            if (sensorRecoveryTotalAttempts == I2C_SENSOR_RECOVERY_MAX_ATTEMPTS) {
              Serial.println(F("I2C: sensor recovery attempts exhausted — permanent fault"));
              sensorRecoveryTotalAttempts++;  // prevent repeat log
            }
          } else {
            Serial.print(F("I2C: all current-loop sensors failing — bus recovery (backoff x"));
            Serial.print(sensorRecoveryBackoff);
            Serial.println(F(")"));
            recoverI2CBus();
            // v2.0.46: recoverI2CBus() does Wire.end()/begin(); re-bind the Notecard so its
            // I2C session survives the bus cycle (mirrors the dual-failure recovery path).
            tankalarm_ensureNotecardBinding(notecard);
            // Fix C1 (v2.0.50): same as the dual-failure site — lightweight re-probe to catch
            // an externally power-cycled A0602 that dropped to its 0x0A unmanaged default.
            // See CODE_REVIEW_06252026_CURRENT_LOOP_SENSORS.md §10 for rationale.
            if (!gDfuInProgress) {
              delay(50);  // brief peripheral settle (§7.1)
              uint8_t prev = gConfig.currentLoopI2cAddress;
              uint8_t resolved = resolveCurrentLoopI2cAddress(prev);
              if (resolved != prev) {
                gConfig.currentLoopI2cAddress = resolved;  // runtime steering only
                Serial.print(F("A0602: post-recovery re-probe address change 0x"));
                if (prev < 0x10) Serial.print('0');
                Serial.print(prev, HEX);
                Serial.print(F(" -> 0x"));
                if (resolved < 0x10) Serial.print('0');
                Serial.println(resolved, HEX);
              }
              if (resolved == 0x0A) {
                Serial.println(F("WARNING: A0602 at unmanaged default 0x0A after recovery; "
                                 "framed reads may still fail until module is re-managed"));
              }
            }
            logI2CRecoveryEvent(I2C_RECOVERY_SENSOR_ONLY);
            sensorRecoveryTotalAttempts++;
            // Reset sensor failure counters to give them a fresh chance
            for (uint8_t i = 0; i < gConfig.monitorCount; i++) {
              if (gConfig.monitors[i].sensorInterface == SENSOR_CURRENT_LOOP) {
                gMonitorState[i].consecutiveFailures = 0;
                gMonitorState[i].sensorFailed = false;
              }
            }
            // Exponential backoff: double threshold each time, capped
            if (sensorRecoveryBackoff < I2C_SENSOR_RECOVERY_MAX_BACKOFF) {
              sensorRecoveryBackoff *= 2;
            }
          }
          consecutiveSensorOnlyFailLoops = 0;
        }
      } else {
        consecutiveSensorOnlyFailLoops = 0;
        sensorRecoveryBackoff = 1;  // Reset backoff when sensors recover
        sensorRecoveryTotalAttempts = 0;  // Reset circuit breaker on full recovery
      }
    } else {
      consecutiveSensorOnlyFailLoops = 0;  // dual-fail path handles this
    }
  }

  // ---- Power-state-aware sample interval ----
  // In ECO/LOW_POWER states, sample less frequently to conserve energy
  unsigned long sampleInterval = (unsigned long)gConfig.sampleSeconds * 1000UL;
  if (gPowerState == POWER_STATE_ECO) {
    sampleInterval *= POWER_ECO_SAMPLE_MULTIPLIER;
  } else if (gPowerState == POWER_STATE_LOW_POWER) {
    sampleInterval *= POWER_LOW_SAMPLE_MULTIPLIER;
  }
  // In CRITICAL_HIBERNATE, skip sampling entirely
  if (gPowerState != POWER_STATE_CRITICAL_HIBERNATE) {
    if (now - gLastTelemetryMillis >= sampleInterval) {
      gLastTelemetryMillis = now;
      if (gConfig.monitorCount > 0) {
        sampleMonitors();
      } else if (now - gLastHeartbeatMillis >= HEALTH_TELEMETRY_INTERVAL_MS) {
        // No monitors configured — send infrequent heartbeat (every 6h)
        // so the server knows this device is still online and awaiting configuration.
        gLastHeartbeatMillis = now;
        sendRegistration("heartbeat");
      }
    }
  }

  // ---- Power-state-aware polling intervals ----
  // Determine base polling interval based on power source.
  // When unconfigured (monitorCount == 0) use an aggressive interval regardless
  // of power source — the device is idle and needs config as fast as possible.
  unsigned long baseInboundInterval;
  if (gConfig.monitorCount == 0) {
    baseInboundInterval = AWAITING_CONFIG_INBOUND_INTERVAL_MS;  // 1 min — awaiting first config
  } else if (gConfig.solarPowered) {
    baseInboundInterval = (unsigned long)SOLAR_INBOUND_INTERVAL_MINUTES * 60000UL;
  } else {
    baseInboundInterval = 600000UL; // 10 minutes for grid power
  }

  // Apply power-state multiplier (skipped when awaiting config)
  unsigned long inboundInterval = baseInboundInterval;
  if (gConfig.monitorCount > 0) {
    if (gPowerState == POWER_STATE_ECO) {
      inboundInterval *= POWER_ECO_INBOUND_MULTIPLIER;
    } else if (gPowerState == POWER_STATE_LOW_POWER) {
      inboundInterval *= POWER_LOW_INBOUND_MULTIPLIER;
    }
  }
  // In CRITICAL_HIBERNATE, skip all inbound polling

  if (gPowerState != POWER_STATE_CRITICAL_HIBERNATE) {
    if (now - gLastConfigCheckMillis >= inboundInterval) {
      gLastConfigCheckMillis = now;
      pollForConfigUpdates();
    }

    if (now - gLastRelayCheckMillis >= inboundInterval) {
      gLastRelayCheckMillis = now;
      pollForRelayCommands();
    }

    if (now - gLastSerialRequestCheckMillis >= inboundInterval) {
      gLastSerialRequestCheckMillis = now;
      pollForSerialRequests();
    }

    if (now - gLastLocationRequestCheckMillis >= inboundInterval) {
      gLastLocationRequestCheckMillis = now;
      pollForLocationRequests();
    }

    // Check for server-requested sync (helps push pending inbound notes
    // through weak cellular links — see Phase 3 sync-on-demand)
    if (now - gLastConfigCheckMillis < 2000UL) {
      // Poll sync_request.qi right after config check (same cadence, minimal overhead)
      pollForSyncRequests();
    }
  }

  // Check for momentary relay timeout (30 minutes) — skip in CRITICAL (relays are off)
  if (gPowerState != POWER_STATE_CRITICAL_HIBERNATE) {
    checkRelayMomentaryTimeout(now);
  }
  
  // Check for physical clear button press
  checkClearButton(now);

  // Check for front-panel USER button press (triggers immediate hub.sync)
  checkUserButton(now);

  // Restore periodic mode when the temporary service window expires
  checkServiceWindowExpiry(now);
  
  // Poll solar charger for battery health data (SunSaver MPPT via RS-485)
  // In CRITICAL_HIBERNATE we still poll the solar charger (if enabled) to detect
  // battery recovery, but at reduced frequency (controlled by sleep duration).
  if (gSolarManager.isEnabled()) {
    if (gSolarManager.poll(now)) {
      // Chemistry verification: once setpoints are read, cross-check them
      // against the user-selected battery type / pack voltage. This is a
      // one-shot log so it doesn't spam the serial console.
      if (!gChemistryChecked && gSolarManager.getData().setpointsValid) {
        gChemistryChecked = true;
        char chemMsg[160];
        SolarManager::ChemistryCheck cc = gSolarManager.verifyChemistry(
          (uint8_t)gConfig.batteryMonitor.batteryType,
          gConfig.batteryMonitor.nominalVoltage,
          chemMsg, sizeof(chemMsg));
        Serial.print(F("Solar: chemistry check: "));
        switch (cc) {
          case SolarManager::CHEMISTRY_CHECK_OK:               Serial.print(F("OK — "));        break;
          case SolarManager::CHEMISTRY_CHECK_MISMATCH:         Serial.print(F("MISMATCH — ")); break;
          case SolarManager::CHEMISTRY_CHECK_VOLTAGE_MISMATCH: Serial.print(F("V_PACK MISMATCH — ")); break;
          case SolarManager::CHEMISTRY_CHECK_PENDING:          Serial.print(F("pending — ")); break;
        }
        Serial.println(chemMsg);
      }

      // A poll was attempted - check alerts using the refreshed communication state
      // (suppress alarm sending in CRITICAL to save power).
      SolarAlertType alert = gSolarManager.checkAlerts();
      if (alert != SOLAR_ALERT_NONE && gNotecardAvailable && gPowerState != POWER_STATE_CRITICAL_HIBERNATE) {
        // Only send alert if different from last, or enough time has passed
        if (alert != gLastSolarAlert || 
            (now - gLastSolarAlarmMillis >= SOLAR_ALARM_MIN_INTERVAL_MS)) {
          sendSolarAlarm(alert);
          gLastSolarAlert = alert;
          gLastSolarAlarmMillis = now;
        }
      } else if (alert == SOLAR_ALERT_NONE) {
        gLastSolarAlert = SOLAR_ALERT_NONE;  // Clear last alert state
      }

      // Optional serial output for hardware bring-up.
      logSolarPollSnapshot(now, alert);
    }
  }
  
  // Poll battery voltage (sourced from MPPT/Vin divider — never the Notecard)
  // Always poll even in CRITICAL — needed to detect battery recovery.
  // Fix 11: pollBatteryVoltage no longer issues Notecard I/O, so the previous Fix 10 Opta
  // compile guard and the `gNotecardAvailable` gate are no longer required for the poll
  // itself. sendBatteryAlarm() is still internally gated on gNotecardAvailable.
  if (gConfig.batteryMonitor.enabled) {
    unsigned long batteryPollInterval = (unsigned long)gConfig.batteryMonitor.pollIntervalSec * 1000UL;
    // In reduced power states, poll less often (but still poll for recovery detection)
    if (gPowerState >= POWER_STATE_LOW_POWER) {
      batteryPollInterval *= 2;  // 2x slower when conserving
    }
    if (now - gLastBatteryPollMillis >= batteryPollInterval) {
      gLastBatteryPollMillis = now;
      if (pollBatteryVoltage(gBatteryData, gConfig.batteryMonitor)) {
        // Suppress normal battery alert processing in CRITICAL to avoid redundant alarms
        if (gPowerState != POWER_STATE_CRITICAL_HIBERNATE) {
          checkBatteryAlerts(gBatteryData, gConfig.batteryMonitor);
        }
      }
    }
  }
  
  // Poll analog Vin voltage divider (when hardware is connected)
  // Always poll even in CRITICAL — needed to detect battery recovery.
  if (gConfig.vinMonitor.enabled) {
    unsigned long vinPollInterval = (unsigned long)gConfig.vinMonitor.pollIntervalSec * 1000UL;
    if (gPowerState >= POWER_STATE_LOW_POWER) {
      vinPollInterval *= 2;  // 2x slower when conserving
    }
    if (now - gLastVinPollMillis >= vinPollInterval) {
      gLastVinPollMillis = now;
      gVinVoltage = readVinDividerVoltage();
    }
  }
  
  // ---- Update power conservation state (after polling battery sources) ----
  updatePowerState();
  
  // Periodic firmware update check via Notecard host-pull MCUboot DFU
  // Interval matches inbound poll: 10 min (grid) / 1 hour (solar)
  // Skip in LOW_POWER and CRITICAL — firmware updates are not urgent
  if (gPowerState <= POWER_STATE_ECO) {
    unsigned long dfuInterval = gConfig.solarPowered
        ? (unsigned long)SOLAR_INBOUND_INTERVAL_MINUTES * 60000UL  // 1 hour (solar)
        : 600000UL;  // 10 minutes (grid)
    if (now - gLastDfuCheckMillis > dfuInterval) {
      if (!gDfuInProgress && gNotecardAvailable) {
        gLastDfuCheckMillis = now;  // only advance the timer when a check actually runs
#ifdef TANKALARM_WATCHDOG_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
        mbedWatchdog.kick();  // QSPI blacklist mount + dfu.status query can be slow
  #else
        IWatchdog.reload();
  #endif
#endif
        checkForFirmwareUpdate();
        // Always apply a pushed OTA update from Notehub when one is available. The per-client
        // update policy was removed — the device unconditionally accepts firmware pushed to it.
        if (gDfuUpdateAvailable) {
          Serial.println(F("Auto-DFU: Applying available firmware update (MCUboot)..."));
          enableDfuMode();
        }
      } else {
        // Could not check this cycle (DFU busy or Notecard offline). Do NOT burn the
        // full interval — a brief Notecard/I2C hiccup at the tick should not defer the
        // OTA check by a whole hour. Retry in ~1 min. (now > dfuInterval in this branch
        // because gLastDfuCheckMillis >= 0, so the subtraction cannot underflow.)
        gLastDfuCheckMillis = now - dfuInterval + 60000UL;
        Serial.println(F("DFU check deferred (DFU busy or Notecard offline) - retrying ~1 min"));
      }
    }
  }

  // ---- Daily firmware-update safety net (power-state override) ----
  // The frequent DFU check above is intentionally skipped in LOW_POWER and CRITICAL_HIBERNATE
  // to conserve energy. But an OTA must NEVER be permanently blocked by power state — otherwise
  // a device that mis-reads its voltage (or has a genuinely weak battery) could get stuck on
  // broken firmware with no remote way to recover. So at least once per day, regardless of
  // voltage or power state, force a sync + DFU check and apply any pending update. The device
  // already wakes every POWER_CRITICAL_SLEEP_MS (5 min) even in hibernate, so this needs no RTC
  // alarm — it just runs on a normal loop iteration once the daily interval has elapsed.
  if (gPowerState > POWER_STATE_ECO) {  // only LOW_POWER / CRITICAL_HIBERNATE (ECO/NORMAL handled above)
    bool dailyDfuDue = (gLastDailyDfuCheckMillis == 0)
        ? (now >= DAILY_DFU_BOOT_GRACE_MS)
        : (now - gLastDailyDfuCheckMillis >= DAILY_DFU_CHECK_INTERVAL_MS);
    if (dailyDfuDue && !gDfuInProgress && gNotecardAvailable) {
      gLastDailyDfuCheckMillis = now;
      Serial.println(F("Daily DFU window: checking for firmware update (power-state override)"));
      addSerialLog("Daily DFU check (low-power override)");
#ifdef TANKALARM_WATCHDOG_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
      mbedWatchdog.kick();
  #else
      IWatchdog.reload();
  #endif
#endif
      // Force a sync so the Notecard pulls the latest dfu.status/firmware from Notehub even
      // though inbound polling is otherwise suspended in this state.
      J *dfuSyncReq = notecard.newRequest("hub.sync");
      if (dfuSyncReq) {
        J *dfuSyncRsp = notecard.requestAndResponse(dfuSyncReq);
        if (dfuSyncRsp) notecard.deleteResponse(dfuSyncRsp);
      }
#ifdef TANKALARM_WATCHDOG_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
      mbedWatchdog.kick();
  #else
      IWatchdog.reload();
  #endif
#endif
      checkForFirmwareUpdate();
      // Apply regardless of power state — recovery from broken/old firmware takes priority over
      // power conservation. enableDfuMode() kicks the watchdog throughout the staging operation.
      // The per-client update policy was removed: a pushed OTA is always accepted.
      if (gDfuUpdateAvailable) {
        Serial.println(F("Daily DFU window: applying available firmware update (MCUboot)..."));
        addSerialLog("Daily DFU apply (low-power override)");
        enableDfuMode();
      }
    }
  }

  persistConfigIfDirty();
  retryPendingConfigAckIfDue();

  // Periodic health telemetry (when enabled, skip in CRITICAL to save power)
#ifdef TANKALARM_HEALTH_TELEMETRY_ENABLED
  if (gPowerState <= POWER_STATE_ECO && gNotecardAvailable) {
    if (now - gLastHealthTelemetryMillis >= HEALTH_TELEMETRY_INTERVAL_MS) {
      gLastHealthTelemetryMillis = now;
      sendHealthTelemetry();
    }
    // Track heap low-watermark on every loop iteration (cheap operation)
    uint32_t currentHeap = tankalarm_freeRam();
    if (currentHeap > 0 && currentHeap < gHeapMinFreeBytes) {
      gHeapMinFreeBytes = currentHeap;
    }
  }
#endif
  
  // Skip time sync and daily reports in CRITICAL_HIBERNATE
  if (gPowerState != POWER_STATE_CRITICAL_HIBERNATE) {
    ensureTimeSync();
    updateDailyScheduleIfNeeded();

    bool reportDue = false;
    if (isSolarOnlyActive()) {
      // Opportunistic reporting: send ASAP if overdue by configured threshold
      double nowEpoch = currentEpoch();
      if (nowEpoch > 0.0 && gSolarOnlyStartupComplete && isSensorVoltageGateOpen()) {
        double hoursSinceLastReport = 9999.0;
        if (gSolarOnlyLastReportEpoch > 0.0) {
          hoursSinceLastReport = (nowEpoch - gSolarOnlyLastReportEpoch) / 3600.0;
        }
        if (hoursSinceLastReport >= (double)gConfig.solarOnlyConfig.opportunisticReportHours) {
          reportDue = true;
        }
        // Also honor the scheduled time if it has passed
        if (gNextDailyReportEpoch > 0.0 && nowEpoch >= gNextDailyReportEpoch) {
          reportDue = true;
        }
      }
    } else {
      // Standard scheduled reporting
      reportDue = (gNextDailyReportEpoch > 0.0 && currentEpoch() >= gNextDailyReportEpoch);

      // Fallback: if Notecard time never syncs (no cellular/GPS), fire daily
      // report based on millis() every 24h to avoid permanent blackout.
      if (!reportDue && currentEpoch() <= 0.0) {
        static unsigned long lastFallbackReportMillis = 0;
        if (lastFallbackReportMillis == 0) {
          // First check: only fire after 24h of uptime (allow time for Notecard sync)
          if (millis() >= 24UL * 60UL * 60UL * 1000UL) {
            Serial.println(F("WARNING: No time sync — sending daily report via millis fallback"));
            reportDue = true;
            lastFallbackReportMillis = millis();
          }
        } else if ((millis() - lastFallbackReportMillis) >= 24UL * 60UL * 60UL * 1000UL) {
          Serial.println(F("WARNING: No time sync — sending daily report via millis fallback"));
          reportDue = true;
          lastFallbackReportMillis = millis();
        }
      }
    }

    if (reportDue) {
      sendDailyReport();
      gSolarOnlyLastReportEpoch = currentEpoch();
      if (isSolarOnlyActive()) {
        saveSolarStateToFlash();  // Persist last report epoch across power cycles
      }
      scheduleNextDailyReport();
    }
  }
  
  // Solar-only sunset protocol: detect declining voltage and save state
  if (isSolarOnlyActive() && gSolarOnlyStartupComplete) {
    checkSolarOnlySunsetProtocol(now);
  }

  // Sleep to reduce power consumption between loop iterations
  // Duration is controlled by the power conservation state machine.
  // Higher states = longer sleep = lower power draw.
  // For sleep durations longer than the watchdog timeout, we sleep in chunks
  // and kick the watchdog between each chunk to prevent a hardware reset.
  unsigned long sleepMs = getPowerStateSleepMs(gPowerState);
  safeSleep(sleepMs);
}

// Helper: Get recommended pulse sampling parameters based on expected rate
// This helps configure optimal sampling for the expected RPM/flow rate range
// Returns: pulseSampleDurationMs, pulseAccumulatedMode recommendations
static PulseSamplingRecommendation getRecommendedPulseSampling(float expectedRate) {
  PulseSamplingRecommendation rec;
  
  if (expectedRate <= 0.0f) {
    // No expected rate configured - use defaults
    rec.sampleDurationMs = RPM_SAMPLE_DURATION_MS;
    rec.accumulatedMode = false;
    rec.description = "Default (60s sample)";
  } else if (expectedRate < 1.0f) {
    // Very low rate (< 1 RPM/GPM): use accumulated mode
    // Count pulses over entire telemetry interval for accuracy
    rec.sampleDurationMs = 60000;  // 60s sample within each interval
    rec.accumulatedMode = true;
    rec.description = "Accumulated mode (very low rate)";
  } else if (expectedRate < 10.0f) {
    // Low rate (1-10 RPM/GPM): longer sample for accuracy
    rec.sampleDurationMs = 60000;  // 60 seconds
    rec.accumulatedMode = false;
    rec.description = "60s sample (low rate)";
  } else if (expectedRate < 100.0f) {
    // Medium rate (10-100 RPM/GPM): moderate sample
    rec.sampleDurationMs = 30000;  // 30 seconds
    rec.accumulatedMode = false;
    rec.description = "30s sample (medium rate)";
  } else if (expectedRate < 1000.0f) {
    // High rate (100-1000 RPM/GPM): shorter sample is sufficient
    rec.sampleDurationMs = 10000;  // 10 seconds
    rec.accumulatedMode = false;
    rec.description = "10s sample (high rate)";
  } else {
    // Very high rate (> 1000 RPM): quick sample
    rec.sampleDurationMs = 3000;   // 3 seconds
    rec.accumulatedMode = false;
    rec.description = "3s sample (very high rate)";
  }
  
  return rec;
}

// Helper function: Get tank height/capacity based on sensor configuration
static float getMonitorHeight(const MonitorConfig &cfg) {
  // Gas pressure monitors don't have a meaningful "height" — return the raw
  // pressure full-scale value so callers that compare current reading vs height
  // (e.g., percentage gauges) still get sensible numbers in the sensor's own units.
  if (cfg.objectType == OBJECT_GAS) {
    return cfg.sensorRangeMax;
  }
  if (cfg.sensorInterface == SENSOR_CURRENT_LOOP) {
    if (cfg.currentLoopType == CURRENT_LOOP_ULTRASONIC) {
      // For ultrasonic sensors, mount height IS the tank height (distance to bottom)
      return cfg.sensorMountHeight;
    } else {
      // Pressure-driven liquid level: divide by SG so non-water fluids size correctly.
      float sg = getEffectiveSpecificGravity(cfg);
      float rangeInches = cfg.sensorRangeMax * getPressureConversionFactorByName(cfg.sensorRangeUnit) / sg;
      return rangeInches + cfg.sensorMountHeight;
    }
  } else if (cfg.sensorInterface == SENSOR_ANALOG) {
    float sg = getEffectiveSpecificGravity(cfg);
    float rangeInches = cfg.sensorRangeMax * getPressureConversionFactorByName(cfg.sensorRangeUnit) / sg;
    return rangeInches + cfg.sensorMountHeight;
  } else if (cfg.sensorInterface == SENSOR_DIGITAL) {
    // Digital sensors are binary, treat 1.0 as full
    return 1.0f;
  }
  return 0.0f;
}

// Returns the specific gravity to use for pressure->height conversion in the client's
// local fallback path (used until/unless the server's calibration learning takes over).
// Priority:  manual override (FLUID_CUSTOM + fluidSpecificGravity > 0)  >  fluid preset  >  water (1.0)
static float getEffectiveSpecificGravity(const MonitorConfig &cfg) {
  if (cfg.fluidType == FLUID_CUSTOM && cfg.fluidSpecificGravity >= 0.3f && cfg.fluidSpecificGravity <= 2.0f) {
    return cfg.fluidSpecificGravity;
  }
  // For non-custom presets, fluidSpecificGravity > 0 still wins as an explicit override
  // (lets the server push a learned-calibration-derived SG without changing the type label).
  if (cfg.fluidSpecificGravity >= 0.3f && cfg.fluidSpecificGravity <= 2.0f) {
    return cfg.fluidSpecificGravity;
  }
  float preset = getPresetSpecificGravity(cfg.fluidType);
  return (preset > 0.0f) ? preset : 1.0f;
}

/**
 * Recover from interrupted atomic writes at boot.
 * Called once during initializeStorage() after filesystem mount.
 *
 * If a .tmp file exists but the target does NOT, the rename failed
 * after a write completed — complete the rename now.
 * If BOTH exist, the original is still valid — delete stale .tmp.
 */
#ifdef POSIX_FILE_IO_AVAILABLE
static void recoverOrphanedTmpFiles() {
  static const char * const criticalFiles[] = {
    "/cfg/client_config.json",
    "/cfg/pending_notes.log",
    nullptr
  };

  for (int i = 0; criticalFiles[i] != nullptr; ++i) {
    char tmpPath[256];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", criticalFiles[i]);

    if (tankalarm_posix_file_exists(tmpPath)) {
      if (!tankalarm_posix_file_exists(criticalFiles[i])) {
        // Target missing + tmp exists → rename was interrupted; complete it
        if (rename(tmpPath, criticalFiles[i]) == 0) {
          Serial.print(F("Recovered config from .tmp: "));
          Serial.println(criticalFiles[i]);
        } else {
          Serial.print(F("ERROR: Could not recover: "));
          Serial.println(criticalFiles[i]);
        }
      } else {
        // Both exist → original is valid; clean up stale tmp
        remove(tmpPath);
        #ifdef DEBUG_MODE
        Serial.print(F("Cleaned stale .tmp: "));
        Serial.println(tmpPath);
        #endif
      }
    }
  }
}
#endif // POSIX_FILE_IO_AVAILABLE

static void initializeStorage() {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    // Mbed OS LittleFileSystem initialization.
    //
    // The QSPI flash is MBR-partitioned (see KeyProvisioning). We mount LittleFS
    // on partition 4 (user data) ONLY, leaving partition 2 reserved for MCUboot
    // OTA staging. We must NEVER reformat the whole device, or we would destroy
    // the OTA partition and break firmware updates.
    gStorageAvailable = false;
    mbedBD = BlockDevice::get_default_instance();
    if (!mbedBD) {
      Serial.println(F("Error: No default block device found"));
      Serial.println(F("Warning: Filesystem not available - configuration will not persist"));
      return;
    }

    // Wrap partition 4 (requires an MBR created by KeyProvisioning).
    mbedAppPart = new mbed::MBRBlockDevice(mbedBD, TANKALARM_APP_DATA_PARTITION);
    if (mbedAppPart->init() != 0) {
      // No MBR / partition 4 present -> board not provisioned for the MCUboot
      // layout. Do NOT reformat the whole device (that would erase the OTA
      // partition). Run without persistence and tell the operator to provision.
      Serial.println(F("ERROR: QSPI partition 4 not found - board not provisioned for MCUboot layout."));
      Serial.println(F("Run TankAlarm-112025-KeyProvisioning to create the MBR partition table."));
      Serial.println(F("Running without persistence to protect the OTA partition."));
      delete mbedAppPart;
      mbedAppPart = nullptr;
      return;
    }

    mbedFS = new LittleFileSystem("cfg");
    int err = mbedFS->mount(mbedAppPart);
    if (err) {
      // Format ONLY partition 4 (never the whole device).
      Serial.println(F("Filesystem mount failed, formatting app partition (p4)..."));
      err = mbedFS->reformat(mbedAppPart);
      if (err) {
        Serial.println(F("LittleFS format failed; running without persistence"));
        delete mbedFS;
        mbedFS = nullptr;
        return;
      }
    }
    gStorageAvailable = true;
    Serial.println(F("Mbed OS LittleFileSystem initialized (QSPI partition 4)"));
    // Recover from any interrupted atomic writes (power loss during rename)
    recoverOrphanedTmpFiles();
  #else
    // STM32duino LittleFS
    if (!LittleFS.begin()) {
      gStorageAvailable = false;
      Serial.println(F("LittleFS init failed; running without persistence"));
      return;
    }
    gStorageAvailable = true;
  #endif
#else
  Serial.println(F("Warning: Filesystem not available on this platform - configuration will not persist"));
#endif
}

static void ensureConfigLoaded() {
#ifdef FILESYSTEM_AVAILABLE
  if (!isStorageAvailable()) {
    // Degraded mode: run with defaults in RAM only.
    createDefaultConfig(gConfig);
    Serial.println(F("Warning: Using default config (filesystem unavailable)"));
    return;
  }
  if (!loadConfigFromFlash(gConfig)) {
    createDefaultConfig(gConfig);
    gConfigDirty = true;
    persistConfigIfDirty();
    Serial.println(F("Default configuration written to flash"));
  }
#else
  // Filesystem not available - create default config in RAM only
  createDefaultConfig(gConfig);
  Serial.println(F("Warning: Using default config (no persistence available)"));
#endif
}

static void createDefaultConfig(ClientConfig &cfg) {
  memset(&cfg, 0, sizeof(ClientConfig));
  cfg.configSchemaVersion = CONFIG_SCHEMA_VERSION;
  strlcpy(cfg.siteName, "Opta Tank Site", sizeof(cfg.siteName));
  strlcpy(cfg.deviceLabel, "Unconfigured Client", sizeof(cfg.deviceLabel));
  strlcpy(cfg.clientFleet, "tankalarm-clients", sizeof(cfg.clientFleet));
  strlcpy(cfg.serverFleet, "tankalarm-server", sizeof(cfg.serverFleet));
  strlcpy(cfg.dailyEmail, "reports@example.com", sizeof(cfg.dailyEmail));
  cfg.sampleSeconds = DEFAULT_SAMPLE_SECONDS;
  cfg.reportHour = DEFAULT_REPORT_HOUR;
  cfg.reportMinute = DEFAULT_REPORT_MINUTE;
  cfg.monitorCount = 1;

  cfg.monitors[0].id = 'A';
  strlcpy(cfg.monitors[0].name, "Primary Tank", sizeof(cfg.monitors[0].name));
  cfg.monitors[0].contents[0] = '\0'; // Empty by default
  cfg.monitors[0].sensorIndex = 1;
  cfg.monitors[0].objectType = OBJECT_TANK;          // Default: tank level monitoring
  cfg.monitors[0].sensorInterface = SENSOR_ANALOG;   // Default: analog voltage sensor
  cfg.monitors[0].primaryPin = 0; // A0 on Opta Ext
  cfg.monitors[0].secondaryPin = -1;
  cfg.monitors[0].currentLoopChannel = -1;
  cfg.monitors[0].pulsePin = -1; // No pulse sensor by default
  cfg.monitors[0].pulsesPerUnit = 1; // Default: 1 pulse per revolution/gallon
  cfg.monitors[0].hallEffectType = HALL_EFFECT_UNIPOLAR; // Default: unipolar sensor
  cfg.monitors[0].hallEffectDetection = HALL_DETECT_PULSE; // Default: pulse counting method
  cfg.monitors[0].pulseSampleDurationMs = RPM_SAMPLE_DURATION_MS; // Default: 60 seconds
  cfg.monitors[0].pulseAccumulatedMode = false; // Default: single sample mode
  cfg.monitors[0].alarmsEnabled = false; // No alarms until explicitly configured
  cfg.monitors[0].highAlarmThreshold = 1e9f;  // Inert default — unreachable for any unit
  cfg.monitors[0].lowAlarmThreshold = -1e9f;  // Inert default — unreachable for any unit
  cfg.monitors[0].hysteresisValue = 2.0f; // 2 unit hysteresis band
  cfg.monitors[0].enableDailyReport = true;
  cfg.monitors[0].enableAlarmSms = false; // No SMS until explicitly configured
  cfg.monitors[0].enableServerUpload = true;
  cfg.monitors[0].reportThreshold = DEFAULT_LEVEL_CHANGE_THRESHOLD; // 0 = change-based telemetry off
  cfg.monitors[0].relayTargetClient[0] = '\0'; // No relay target by default
  cfg.monitors[0].relayMask = 0; // No relays triggered by default
  cfg.monitors[0].relayTrigger = RELAY_TRIGGER_ANY; // Default: trigger on any alarm
  cfg.monitors[0].relayMode = RELAY_MODE_MOMENTARY; // Default: momentary activation
  // Default: all relays use 30 minutes (0 = use default)
  for (uint8_t r = 0; r < 4; ++r) {
    cfg.monitors[0].relayMomentarySeconds[r] = 0;
  }
  cfg.monitors[0].digitalTrigger[0] = '\0'; // Not a digital sensor by default
  strlcpy(cfg.monitors[0].digitalSwitchMode, "NO", sizeof(cfg.monitors[0].digitalSwitchMode)); // Default: normally-open
  cfg.monitors[0].currentLoopType = CURRENT_LOOP_PRESSURE; // Default: pressure sensor (most common)
  cfg.monitors[0].sensorMountHeight = 0.0f; // Default: sensor at tank bottom
  cfg.monitors[0].sensorRangeMin = 0.0f;    // Default: 0 (e.g., 0 PSI or 0 meters)
  cfg.monitors[0].sensorRangeMax = 5.0f;    // Default: 5 (e.g., 5 PSI for typical pressure sensor)
  strlcpy(cfg.monitors[0].sensorRangeUnit, "PSI", sizeof(cfg.monitors[0].sensorRangeUnit)); // Default: PSI
  cfg.monitors[0].pwmGatingEnabled = true;  // Default: gating enabled on P1 for current loop backward compat
  cfg.monitors[0].pwmGatingChannel = 0;     // Default: P1
  cfg.monitors[0].pwmGatingWarmup = 3000;   // Default: 3000ms stabilization delay
  cfg.monitors[0].pwmGatingSampleDelay = 300; // Default: 300ms sensor read/debounce delay
  cfg.monitors[0].analogVoltageMin = 0.0f;  // Default: 0V (for 0-10V sensors)
  cfg.monitors[0].analogVoltageMax = 10.0f; // Default: 10V (for 0-10V sensors)
  cfg.monitors[0].fluidType = FLUID_WATER;       // Default fluid: water (SG 1.0)
  cfg.monitors[0].fluidSpecificGravity = 0.0f;   // 0 = use preset for fluidType
  cfg.monitors[0].hasLearnedCalibration = false; // No server-pushed calibration yet
  cfg.monitors[0].calSlope = 0.0f;
  cfg.monitors[0].calOffset = 0.0f;
  cfg.monitors[0].calTempCoef = 0.0f;
  cfg.monitors[0].calTempF = 0.0f;
  cfg.monitors[0].calVersion = 0;
  strlcpy(cfg.monitors[0].measurementUnit, "inches", sizeof(cfg.monitors[0].measurementUnit)); // Default: inches
  cfg.monitors[0].expectedPulseRate = 0.0f; // Default: not configured (0 = no baseline)
  cfg.monitors[0].stuckDetectionEnabled = false; // Default: off (opt-in via config)
  // Tank unload tracking defaults (disabled)
  cfg.monitors[0].trackUnloads = false;     // Default: not a fill-and-empty tank
  cfg.monitors[0].unloadEmptyHeight = UNLOAD_DEFAULT_EMPTY_HEIGHT; // Default empty reading
  cfg.monitors[0].unloadDropThreshold = 0.0f; // Default: use percentage instead
  cfg.monitors[0].unloadDropPercent = UNLOAD_DEFAULT_DROP_PERCENT; // Default: 50% drop = unload
  cfg.monitors[0].unloadAlarmSms = false;   // Default: no SMS on unload
  cfg.monitors[0].unloadAlarmEmail = true;  // Default: include in email summary
  
  // Clear button defaults (disabled)
  cfg.clearButtonPin = -1;           // -1 = disabled
  cfg.clearButtonActiveHigh = false; // Active LOW with pullup (button connects to GND)
  
  // Power saving defaults (grid-tied, no special power saving)
  cfg.solarPowered = false;          // false = grid-tied (default)
  
  // I2C address defaults
  cfg.currentLoopI2cAddress = CURRENT_LOOP_I2C_ADDRESS; // Default 0x64
  
  // Solar/Battery charger monitoring defaults (disabled)
  // Requires: Arduino Opta with RS485 (Opta WiFi AFX00002 or Opta RS485 AFX00001),
  // Morningstar MRC-1 adapter, and SunSaver MPPT
  cfg.solarCharger.enabled = false;                          // Disabled by default
  cfg.solarCharger.modbusSlaveId = SOLAR_DEFAULT_SLAVE_ID;   // Default: 1
  cfg.solarCharger.modbusBaudRate = SOLAR_DEFAULT_BAUD_RATE; // Default: 9600
  cfg.solarCharger.modbusTimeoutMs = SOLAR_DEFAULT_TIMEOUT_MS; // Default: 1000ms (begin() floors to 500ms min)
  cfg.solarCharger.pollIntervalSec = SOLAR_DEFAULT_POLL_INTERVAL_SEC; // Default: 60s
  cfg.solarCharger.batteryLowVoltage = BATTERY_VOLTAGE_LOW;  // Default: 11.8V
  cfg.solarCharger.batteryCriticalVoltage = BATTERY_VOLTAGE_CRITICAL; // Default: 11.5V
  cfg.solarCharger.batteryHighVoltage = BATTERY_VOLTAGE_HIGH; // Default: 14.8V
  cfg.solarCharger.alertOnLowBattery = true;                 // Send alerts for low battery
  cfg.solarCharger.alertOnFault = false;                     // Disabled until SunSaver fault register addresses are bench-verified (2026-04-22)
  cfg.solarCharger.alertOnCommFailure = false;               // Don't alert on comm failures (too noisy)
  cfg.solarCharger.includeInDailyReport = true;              // Include in daily report
  
  // Battery voltage monitoring defaults (Notecard direct to battery)
  // Requires: Notecard VIN wired directly to 12V battery (not through 5V regulator)
  initBatteryConfig(&cfg.batteryMonitor, BATTERY_TYPE_AGM, 12);
  cfg.batteryMonitor.enabled = false;                        // Disabled by default
  
  // Analog Vin voltage divider defaults (disabled)
  // Requires: External resistor divider wired from battery to an Opta analog input
  initVinMonitorConfig(&cfg.vinMonitor);                     // Disabled by default
  
  // Solar-Only (No Battery) mode defaults (disabled)
  initSolarOnlyConfig(&cfg.solarOnlyConfig);                 // Disabled by default
}

// ============================================================================
// Monitor Config Parsing Helpers
// ============================================================================
// Shared by loadConfigFromFlash() and applyConfigUpdate() to eliminate
// ~150 lines of duplicated per-monitor JSON field parsing.
//
// Usage:
//   loadConfigFromFlash:  initMonitorDefaults(mon, i);  parseMonitorFromJson(mon, t, i);
//   applyConfigUpdate:    parseMonitorFromJson(mon, t, i);  // keeps existing values
// ============================================================================

/**
 * Initialize a MonitorConfig with sensible defaults before JSON parsing.
 * Called during loadConfigFromFlash; not needed for applyConfigUpdate
 * (which preserves existing values for absent fields).
 */
static void initMonitorDefaults(MonitorConfig &mon, uint8_t index) {
  // Identity
  mon.id = 'A' + index;
  strlcpy(mon.name, "Tank", sizeof(mon.name));
  mon.contents[0] = '\0';
  mon.sensorIndex = index + 1;
  mon.userNumber = 0;
  mon.objectType = OBJECT_TANK;

  // Sensor hardware
  mon.sensorInterface = SENSOR_ANALOG;
  mon.primaryPin = 0;
  mon.secondaryPin = -1;
  mon.currentLoopChannel = -1;
  mon.pulsePin = -1;
  mon.pulsesPerUnit = 1;
  mon.hallEffectType = HALL_EFFECT_UNIPOLAR;
  mon.hallEffectDetection = HALL_DETECT_PULSE;
  mon.pulseSampleDurationMs = RPM_SAMPLE_DURATION_MS;
  mon.pulseAccumulatedMode = false;
  mon.expectedPulseRate = 0.0f;

  // Alarm thresholds
  mon.alarmsEnabled = false; // No alarms until explicitly configured
  mon.highAlarmThreshold = 1e9f;  // Inert default — unreachable for any unit
  mon.lowAlarmThreshold = -1e9f;  // Inert default — unreachable for any unit
  mon.hysteresisValue = 2.0f;

  // Notifications
  mon.enableDailyReport = true;
  mon.enableAlarmSms = false; // No SMS until explicitly configured
  mon.enableServerUpload = true;
  mon.reportThreshold = DEFAULT_LEVEL_CHANGE_THRESHOLD; // 0 = change-based telemetry off

  // Relay control
  mon.relayTargetClient[0] = '\0';
  mon.relayMask = 0;
  mon.relayTrigger = RELAY_TRIGGER_ANY;
  mon.relayMode = RELAY_MODE_MOMENTARY;
  for (uint8_t r = 0; r < 4; ++r) {
    mon.relayMomentarySeconds[r] = 0;
  }

  // Digital sensor
  mon.digitalTrigger[0] = '\0';
  strlcpy(mon.digitalSwitchMode, "NO", sizeof(mon.digitalSwitchMode));

  // Current loop
  mon.currentLoopType = CURRENT_LOOP_PRESSURE;
  mon.sensorMountHeight = 0.0f;

  // Sensor range
  mon.sensorRangeMin = 0.0f;
  mon.sensorRangeMax = 5.0f;
  strlcpy(mon.sensorRangeUnit, "PSI", sizeof(mon.sensorRangeUnit));
  mon.pwmGatingEnabled = true;  // Default: gating enabled on P1 for current loop backward compat
  mon.pwmGatingChannel = 0;     // Default: P1
  mon.pwmGatingWarmup = 3000;   // Default: 3000ms stabilization delay
  mon.pwmGatingSampleDelay = 300; // Default: 300ms sensor read/debounce delay
  mon.analogVoltageMin = 0.0f;
  mon.analogVoltageMax = 10.0f;

  // Fluid characterization defaults (water, no manual SG override)
  mon.fluidType = FLUID_WATER;
  mon.fluidSpecificGravity = 0.0f;
  mon.hasLearnedCalibration = false;
  mon.calSlope = 0.0f;
  mon.calOffset = 0.0f;
  mon.calTempCoef = 0.0f;
  mon.calTempF = 0.0f;
  mon.calVersion = 0;

  // Stuck sensor detection
  mon.stuckDetectionEnabled = true;
  // Unload tracking
  mon.trackUnloads = false;
  mon.unloadEmptyHeight = UNLOAD_DEFAULT_EMPTY_HEIGHT;
  mon.unloadDropThreshold = 0.0f;
  mon.unloadDropPercent = UNLOAD_DEFAULT_DROP_PERCENT;
  mon.unloadAlarmSms = false;
  mon.unloadAlarmEmail = true;
}

/**
 * Parse monitor configuration fields from a JSON object into a MonitorConfig.
 * Uses "update if present" semantics: fields absent from JSON are left unchanged.
 * This allows the same function to serve both:
 *   - loadConfigFromFlash: MonitorConfig is pre-initialized via initMonitorDefaults()
 *   - applyConfigUpdate:   MonitorConfig already holds live values
 *
 * Supports both old and new JSON field name variants for backwards compatibility:
 *   sensorInterface / sensor,  pulsePin / rpmPin,  pulsesPerUnit / pulsesPerRev,
 *   pulseSampleDurationMs / rpmSampleDurationMs,  pulseAccumulatedMode / rpmAccumulatedMode
 */
static void parseMonitorFromJson(MonitorConfig &mon, JsonObjectConst t, uint8_t index) {
  // ---- Identity ----
  const char *idStr = t["id"].as<const char *>();
  if (idStr) mon.id = idStr[0];

  const char *nameStr = t["name"].as<const char *>();
  if (nameStr) strlcpy(mon.name, nameStr, sizeof(mon.name));

  const char *contentsStr = t["contents"].as<const char *>();
  if (contentsStr) strlcpy(mon.contents, contentsStr, sizeof(mon.contents));

  if (t["number"].is<uint8_t>()) mon.sensorIndex = t["number"].as<uint8_t>();
  if (t["userNumber"].is<uint8_t>()) mon.userNumber = t["userNumber"].as<uint8_t>();

  // Object type (what is being monitored)
  const char *objType = t["monitorType"].as<const char *>();
  if (objType) {
    if (strcmp(objType, "engine") == 0)      mon.objectType = OBJECT_ENGINE;
    else if (strcmp(objType, "pump") == 0)   mon.objectType = OBJECT_PUMP;
    else if (strcmp(objType, "gas") == 0)    mon.objectType = OBJECT_GAS;
    else if (strcmp(objType, "flow") == 0)   mon.objectType = OBJECT_FLOW;
    else if (strcmp(objType, "rpm") == 0)    mon.objectType = OBJECT_ENGINE;
    else                                     mon.objectType = OBJECT_TANK;
  }

  // ---- Sensor interface ----
  const char *sensor = t["sensor"].as<const char *>();
  if (sensor) {
    if (strcmp(sensor, "digital") == 0) {
      mon.sensorInterface = SENSOR_DIGITAL;
    } else if (strcmp(sensor, "current") == 0 || strcmp(sensor, "currentLoop") == 0) {
      mon.sensorInterface = SENSOR_CURRENT_LOOP;
    } else if (strcmp(sensor, "rpm") == 0 || strcmp(sensor, "pulse") == 0) {
      mon.sensorInterface = SENSOR_PULSE;
    } else {
      mon.sensorInterface = SENSOR_ANALOG;
    }
  }

  // ---- Pin assignments ----
  if (t["primaryPin"].is<int>()) {
    mon.primaryPin = t["primaryPin"].as<int>();
  } else if (mon.sensorInterface == SENSOR_DIGITAL && mon.primaryPin <= 0) {
    // Default digital sensor to pin 2 when not explicitly configured
    mon.primaryPin = 2;
  }
  if (t["secondaryPin"].is<int>()) mon.secondaryPin = t["secondaryPin"].as<int>();
  if (t["loopChannel"].is<int>()) mon.currentLoopChannel = t["loopChannel"].as<int>();

  if (t["rpmPin"].is<int>()) {
    mon.pulsePin = t["rpmPin"].as<int>();
  }

  if (t["pulsesPerRev"].is<uint8_t>()) {
    mon.pulsesPerUnit = max((uint8_t)1, t["pulsesPerRev"].as<uint8_t>());
  }

  // ---- Hall effect config ----
  const char *hallType = t["hallEffectType"].as<const char *>();
  if (hallType) {
    if (strcmp(hallType, "bipolar") == 0)        mon.hallEffectType = HALL_EFFECT_BIPOLAR;
    else if (strcmp(hallType, "omnipolar") == 0)  mon.hallEffectType = HALL_EFFECT_OMNIPOLAR;
    else if (strcmp(hallType, "analog") == 0)     mon.hallEffectType = HALL_EFFECT_ANALOG;
    else                                          mon.hallEffectType = HALL_EFFECT_UNIPOLAR;
  }

  const char *hallDetect = t["hallEffectDetection"].as<const char *>();
  if (hallDetect) {
    if (strcmp(hallDetect, "time") == 0) mon.hallEffectDetection = HALL_DETECT_TIME_BASED;
    else                                 mon.hallEffectDetection = HALL_DETECT_PULSE;
  }

  // ---- Pulse sampling ----
  if (t["rpmSampleDurationMs"].is<uint32_t>()) {
    mon.pulseSampleDurationMs = t["rpmSampleDurationMs"].as<uint32_t>();
  }

  if (t["rpmAccumulatedMode"].is<bool>()) {
    mon.pulseAccumulatedMode = t["rpmAccumulatedMode"].as<bool>();
  }

  if (t["expectedPulseRate"].is<float>()) mon.expectedPulseRate = t["expectedPulseRate"].as<float>();

  // ---- Alarm thresholds ----
  if (t["alarmsEnabled"].is<bool>()) {
    mon.alarmsEnabled = t["alarmsEnabled"].as<bool>();
  } else {
    // Legacy configs: infer alarmsEnabled from presence of threshold fields
    mon.alarmsEnabled = t["highAlarm"].is<float>() || t["lowAlarm"].is<float>();
  }
  if (t["highAlarm"].is<float>()) mon.highAlarmThreshold = t["highAlarm"].as<float>();
  if (t["lowAlarm"].is<float>()) mon.lowAlarmThreshold = t["lowAlarm"].as<float>();
  if (t["hysteresis"].is<float>()) {
    float hysteresis = t["hysteresis"].as<float>();
    mon.hysteresisValue = (hysteresis >= 0.0f) ? hysteresis : 0.0f;
  }

  // ---- Notification flags ----
  if (t["daily"].is<bool>()) mon.enableDailyReport = t["daily"].as<bool>();
  if (t["alarmSms"].is<bool>()) mon.enableAlarmSms = t["alarmSms"].as<bool>();
  if (t["upload"].is<bool>()) mon.enableServerUpload = t["upload"].as<bool>();

  // ---- Change-based telemetry threshold (monitor's own unit; 0 = disabled) ----
  if (t["reportThreshold"].is<float>()) {
    float rt = t["reportThreshold"].as<float>();
    mon.reportThreshold = (rt > 0.0f) ? rt : 0.0f;
  }

  // ---- Relay control ----
  const char *relayTarget = t["relayTargetClient"].as<const char *>();
  if (relayTarget) strlcpy(mon.relayTargetClient, relayTarget, sizeof(mon.relayTargetClient));

  if (t["relayMask"].is<uint8_t>()) mon.relayMask = t["relayMask"].as<uint8_t>();

  const char *relayTriggerStr = t["relayTrigger"].as<const char *>();
  if (relayTriggerStr) {
    if (strcmp(relayTriggerStr, "high") == 0)       mon.relayTrigger = RELAY_TRIGGER_HIGH;
    else if (strcmp(relayTriggerStr, "low") == 0)    mon.relayTrigger = RELAY_TRIGGER_LOW;
    else                                             mon.relayTrigger = RELAY_TRIGGER_ANY;
  }

  const char *relayModeStr = t["relayMode"].as<const char *>();
  if (relayModeStr) {
    if (strcmp(relayModeStr, "until_clear") == 0)       mon.relayMode = RELAY_MODE_UNTIL_CLEAR;
    else if (strcmp(relayModeStr, "manual_reset") == 0)  mon.relayMode = RELAY_MODE_MANUAL_RESET;
    else                                                 mon.relayMode = RELAY_MODE_MOMENTARY;
  }

  if (!t["relayMomentaryDurations"].isNull()) {
    JsonArrayConst durations = t["relayMomentaryDurations"].as<JsonArrayConst>();
    for (uint8_t r = 0; r < 4 && r < durations.size(); r++) {
      uint16_t dur = durations[r].as<uint16_t>();
      mon.relayMomentarySeconds[r] = (dur > 86400) ? (uint16_t)86400 : dur;
    }
  }

  // Max ON duration safety timeout for MANUAL_RESET mode (0 = no limit)
  mon.relayMaxOnSeconds = t["relayMaxOnSeconds"] | (uint32_t)0;
  if (mon.relayMaxOnSeconds > 604800) mon.relayMaxOnSeconds = 604800;  // Cap at 7 days

  // ---- Digital sensor ----
  const char *digitalTriggerStr = t["digitalTrigger"].as<const char *>();
  if (digitalTriggerStr) strlcpy(mon.digitalTrigger, digitalTriggerStr, sizeof(mon.digitalTrigger));

  const char *digitalSwitchModeStr = t["digitalSwitchMode"].as<const char *>();
  if (digitalSwitchModeStr) {
    if (strcmp(digitalSwitchModeStr, "NC") == 0) {
      strlcpy(mon.digitalSwitchMode, "NC", sizeof(mon.digitalSwitchMode));
    } else {
      strlcpy(mon.digitalSwitchMode, "NO", sizeof(mon.digitalSwitchMode));
    }
  }

  // ---- Current loop / sensor calibration ----
  const char *currentLoopTypeStr = t["currentLoopType"].as<const char *>();
  if (currentLoopTypeStr) {
    if (strcmp(currentLoopTypeStr, "ultrasonic") == 0)  mon.currentLoopType = CURRENT_LOOP_ULTRASONIC;
    else                                                mon.currentLoopType = CURRENT_LOOP_PRESSURE;
  }

  if (t["sensorMountHeight"].is<float>()) {
    mon.sensorMountHeight = fmaxf(0.0f, t["sensorMountHeight"].as<float>());
  }

  // ---- PWM Gating control ----
  if (t.containsKey("pwmGatingEnabled")) {
    mon.pwmGatingEnabled = t["pwmGatingEnabled"].as<bool>();
  }
  if (t["pwmGatingChannel"].is<int>()) {
    mon.pwmGatingChannel = t["pwmGatingChannel"].as<int>();
  }
  if (t["pwmGatingWarmup"].is<uint32_t>()) {
    mon.pwmGatingWarmup = t["pwmGatingWarmup"].as<uint32_t>();
  }
  if (t["pwmGatingSampleDelay"].is<uint16_t>()) {
    mon.pwmGatingSampleDelay = t["pwmGatingSampleDelay"].as<uint16_t>();
  }

  // ---- Sensor range ----
  if (t["sensorRangeMin"].is<float>()) mon.sensorRangeMin = t["sensorRangeMin"].as<float>();
  if (t["sensorRangeMax"].is<float>()) mon.sensorRangeMax = t["sensorRangeMax"].as<float>();

  const char *rangeUnitStr = t["sensorRangeUnit"].as<const char *>();
  if (rangeUnitStr) strlcpy(mon.sensorRangeUnit, rangeUnitStr, sizeof(mon.sensorRangeUnit));

  // ---- Fluid characterization (liquid tanks only; ignored for OBJECT_GAS / RPM / flow) ----
  // Accept either a preset name ("diesel", "gasoline", ...) or numeric enum value.
  const char *fluidTypeStr = t["fluidType"].as<const char *>();
  if (fluidTypeStr) {
    mon.fluidType = parseFluidTypeName(fluidTypeStr);
  } else if (t["fluidType"].is<uint8_t>()) {
    mon.fluidType = (FluidType)t["fluidType"].as<uint8_t>();
  }
  if (t["fluidSpecificGravity"].is<float>()) {
    float sg = t["fluidSpecificGravity"].as<float>();
    // Reject implausible values silently; SG between 0.3 and 2.0 covers anything realistic.
    if (sg >= 0.3f && sg <= 2.0f) {
      mon.fluidSpecificGravity = sg;
    } else if (sg == 0.0f) {
      mon.fluidSpecificGravity = 0.0f;  // 0 = use preset table value
    }
  }

  // ---- Server-pushed learned calibration (current-loop sensors) ----
  // The server fits the regression and pushes the coefficients (plus the version cv and the
  // temperature it used) so the client applies the same calibrated level locally — keeping
  // the client's alarm thresholds aligned with the server's calibrated display.
  if (t["calHasCal"].is<bool>() && t["calHasCal"].as<bool>()) {
    mon.hasLearnedCalibration = true;
    mon.calSlope = t["calSlope"] | 0.0f;
    mon.calOffset = t["calOffset"] | 0.0f;
    mon.calTempCoef = t["calTempCoef"] | 0.0f;
    mon.calTempF = t["calTempF"] | 0.0f;
    mon.calVersion = t["calVersion"] | (uint32_t)0;
  } else {
    mon.hasLearnedCalibration = false;
    mon.calSlope = 0.0f;
    mon.calOffset = 0.0f;
    mon.calTempCoef = 0.0f;
    mon.calTempF = 0.0f;
    mon.calVersion = 0;
  }

  // ---- Measurement unit (for display/reporting) ----
  // Explicit field takes priority; otherwise derive from object type and sensor config
  const char *muStr = t["measurementUnit"].as<const char *>();
  if (muStr && muStr[0] != '\0') {
    strlcpy(mon.measurementUnit, muStr, sizeof(mon.measurementUnit));
  } else if (mon.measurementUnit[0] == '\0' || mon.objectType != OBJECT_TANK) {
    // Derive from context if not already set, or if non-tank type still has default "inches"
    switch (mon.objectType) {
      case OBJECT_GAS:
        // Gas pressure: use sensor range unit (PSI, bar, etc.), normalized to lowercase
        if (mon.sensorRangeUnit[0] != '\0') {
          strlcpy(mon.measurementUnit, mon.sensorRangeUnit, sizeof(mon.measurementUnit));
          for (char *p = mon.measurementUnit; *p; ++p) *p = tolower((unsigned char)*p);
        } else {
          strlcpy(mon.measurementUnit, "psi", sizeof(mon.measurementUnit));
        }
        break;
      case OBJECT_ENGINE:
        strlcpy(mon.measurementUnit, "rpm", sizeof(mon.measurementUnit));
        break;
      case OBJECT_FLOW:
        strlcpy(mon.measurementUnit, "gpm", sizeof(mon.measurementUnit));
        break;
      default:
        // Tank and others: use sensor range unit if non-tank, otherwise inches
        if (mon.sensorInterface == SENSOR_CURRENT_LOOP && mon.sensorRangeUnit[0] != '\0'
            && strcmp(mon.sensorRangeUnit, "PSI") != 0) {
          // Current loop with non-default range unit
          strlcpy(mon.measurementUnit, mon.sensorRangeUnit, sizeof(mon.measurementUnit));
        }
        // For sensors with PSI pressure sensors, leave empty — server converts PSI→inches
        break;
    }
  }

  // ---- Analog voltage range ----
  if (t["analogVoltageMin"].is<float>()) mon.analogVoltageMin = t["analogVoltageMin"].as<float>();
  if (t["analogVoltageMax"].is<float>()) mon.analogVoltageMax = t["analogVoltageMax"].as<float>();

  // ---- Stuck sensor detection ----
  if (t["stuckDetection"].is<bool>()) mon.stuckDetectionEnabled = t["stuckDetection"].as<bool>();

  // ---- Unload tracking ----
  if (t["trackUnloads"].is<bool>()) mon.trackUnloads = t["trackUnloads"].as<bool>();
  if (t["unloadEmptyHeight"].is<float>()) {
    mon.unloadEmptyHeight = fmaxf(0.0f, t["unloadEmptyHeight"].as<float>());
  }
  if (t["unloadDropThreshold"].is<float>()) {
    mon.unloadDropThreshold = fmaxf(0.0f, t["unloadDropThreshold"].as<float>());
  }
  if (t["unloadDropPercent"].is<float>()) {
    float pct = t["unloadDropPercent"].as<float>();
    mon.unloadDropPercent = constrain(pct, 10.0f, 95.0f);
  }
  if (t["unloadAlarmSms"].is<bool>()) mon.unloadAlarmSms = t["unloadAlarmSms"].as<bool>();
  if (t["unloadAlarmEmail"].is<bool>()) mon.unloadAlarmEmail = t["unloadAlarmEmail"].as<bool>();
}

static void sanitizeSolarConfig(SolarConfig &sc) {
  // modbusSlaveId: 1..247 (reject 0 broadcast and >247)
  if (sc.modbusSlaveId == 0 || sc.modbusSlaveId > 247) {
    sc.modbusSlaveId = SOLAR_DEFAULT_SLAVE_ID;
  }
  // modbusBaudRate: allowlist {9600, 19200, 38400, 57600, 115200}
  if (sc.modbusBaudRate != 9600 && sc.modbusBaudRate != 19200 &&
      sc.modbusBaudRate != 38400 && sc.modbusBaudRate != 57600 &&
      sc.modbusBaudRate != 115200) {
    sc.modbusBaudRate = SOLAR_DEFAULT_BAUD_RATE;
  }
  // pollIntervalSec: floor at 5s to avoid bus saturation, ceiling at 3600
  if (sc.pollIntervalSec < 5) sc.pollIntervalSec = 5;
  if (sc.pollIntervalSec > 3600) sc.pollIntervalSec = 3600;
  // battery thresholds ordering: Critical < Low < High
  if (sc.batteryCriticalVoltage >= sc.batteryLowVoltage || sc.batteryLowVoltage >= sc.batteryHighVoltage) {
    Serial.println(F("WARNING: Solar battery thresholds out of order; using defaults"));
    sc.batteryLowVoltage = BATTERY_VOLTAGE_LOW;
    sc.batteryCriticalVoltage = BATTERY_VOLTAGE_CRITICAL;
    sc.batteryHighVoltage = BATTERY_VOLTAGE_HIGH;
  }
  // modbusTimeoutMs floor of 500 ms (clamped in begin() but keep here for consistency)
  if (sc.modbusTimeoutMs < 500) sc.modbusTimeoutMs = 500;
  // ...and a ceiling of 4000 ms. A single realtime read tries both Modbus function codes
  // (up to 2 x timeout), and v2.0.43 wraps it in SOLAR_REALTIME_MAX_ATTEMPTS retries; capping
  // the timeout keeps the whole solar poll well under WATCHDOG_TIMEOUT_SECONDS (30 s) even on a
  // dead bus. A legitimate SunSaver/MRC-1 reply is sub-second, so 4 s is a generous ceiling.
  if (sc.modbusTimeoutMs > 4000) sc.modbusTimeoutMs = 4000;
}

static bool loadConfigFromFlash(ClientConfig &cfg) {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    // Mbed OS file operations
    if (!mbedFS) return false;
    
    FILE *file = fopen("/cfg/client_config.json", "r");
    if (!file) {
      return false;
    }
    
    // Read file into buffer
    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    if (fileSize <= 0 || fileSize > 8192) {
      fclose(file);
      return false;
    }
    
    char *buffer = (char *)malloc(fileSize + 1);
    if (!buffer) {
      fclose(file);
      return false;
    }
    
    size_t bytesRead = fread(buffer, 1, fileSize, file);
    buffer[bytesRead] = '\0';
    fclose(file);
    
    std::unique_ptr<JsonDocument> docPtr(new JsonDocument());
    if (!docPtr) {
      free(buffer);
      return false;
    }
    JsonDocument &doc = *docPtr;
    DeserializationError err = deserializeJson(doc, buffer);
    free(buffer);
  #else
    // STM32duino file operations
    if (!LittleFS.exists(CLIENT_CONFIG_PATH)) {
      return false;
    }

    File file = LittleFS.open(CLIENT_CONFIG_PATH, "r");
    if (!file) {
      return false;
    }

    std::unique_ptr<JsonDocument> docPtr(new JsonDocument());
    if (!docPtr) {
      file.close();
      return false;
    }
    JsonDocument &doc = *docPtr;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
  #endif

  if (err) {
    Serial.println(F("Config deserialization failed"));
    return false;
  }

  memset(&cfg, 0, sizeof(ClientConfig));

  // Config schema versioning: detect stale configs from older firmware
  cfg.configSchemaVersion = doc["configSchemaVersion"].is<int>() 
    ? (uint8_t)doc["configSchemaVersion"].as<int>() 
    : 0;
  if (cfg.configSchemaVersion != CONFIG_SCHEMA_VERSION) {
    Serial.print(F("Config schema mismatch: stored="));
    Serial.print(cfg.configSchemaVersion);
    Serial.print(F(" expected="));
    Serial.println(CONFIG_SCHEMA_VERSION);
    // Continue loading — fields absent in older schema get safe defaults from memset(0)
    // The config will be re-saved with the current schema version on next persist
    cfg.configSchemaVersion = CONFIG_SCHEMA_VERSION;
  }

  strlcpy(cfg.siteName, doc["site"].as<const char *>() ? doc["site"].as<const char *>() : "", sizeof(cfg.siteName));
  strlcpy(cfg.deviceUid, doc["deviceUid"].as<const char *>() ? doc["deviceUid"].as<const char *>() : "", sizeof(cfg.deviceUid));
  strlcpy(cfg.deviceLabel, doc["deviceLabel"].as<const char *>() ? doc["deviceLabel"].as<const char *>() : "", sizeof(cfg.deviceLabel));
  strlcpy(cfg.serverFleet, doc["serverFleet"].as<const char *>() ? doc["serverFleet"].as<const char *>() : "", sizeof(cfg.serverFleet));
  strlcpy(cfg.clientFleet, doc["clientFleet"].as<const char *>() ? doc["clientFleet"].as<const char *>() : "", sizeof(cfg.clientFleet));
  strlcpy(cfg.dailyEmail, doc["dailyEmail"].as<const char *>() ? doc["dailyEmail"].as<const char *>() : "", sizeof(cfg.dailyEmail));
  // Product UID: persisted so a remotely-configured UID survives reboot
  strlcpy(cfg.productUid, doc["productUid"].as<const char *>() ? doc["productUid"].as<const char *>() : "", sizeof(cfg.productUid));

  cfg.sampleSeconds = doc["sampleSeconds"].is<uint16_t>() ? doc["sampleSeconds"].as<uint16_t>() : DEFAULT_SAMPLE_SECONDS;
  cfg.reportHour = doc["reportHour"].is<uint8_t>() ? doc["reportHour"].as<uint8_t>() : DEFAULT_REPORT_HOUR;
  cfg.reportMinute = doc["reportMinute"].is<uint8_t>() ? doc["reportMinute"].as<uint8_t>() : DEFAULT_REPORT_MINUTE;
  
  // Load clear button configuration
  cfg.clearButtonPin = doc["clearButtonPin"].is<int>() ? doc["clearButtonPin"].as<int8_t>() : -1;
  cfg.clearButtonActiveHigh = doc["clearButtonActiveHigh"].is<bool>() ? doc["clearButtonActiveHigh"].as<bool>() : false;
  
  // Load power saving configuration
  cfg.solarPowered = doc["solarPowered"].is<bool>() ? doc["solarPowered"].as<bool>() : false;
  
  // Load I2C address configuration (allows runtime override of compile-time default)
  cfg.currentLoopI2cAddress = doc["currentLoopI2cAddress"].is<int>() 
    ? (uint8_t)doc["currentLoopI2cAddress"].as<int>() 
    : CURRENT_LOOP_I2C_ADDRESS;

  // Load solar charger configuration (SunSaver MPPT via RS-485)
  JsonObject solarCfg = doc["solarCharger"].as<JsonObject>();
  if (solarCfg) {
    cfg.solarCharger.enabled = solarCfg["enabled"].is<bool>() ? solarCfg["enabled"].as<bool>() : false;
    cfg.solarCharger.modbusSlaveId = solarCfg["slaveId"].is<int>() ? (uint8_t)solarCfg["slaveId"].as<int>() : SOLAR_DEFAULT_SLAVE_ID;
    cfg.solarCharger.modbusBaudRate = solarCfg["baudRate"].is<int>() ? (uint16_t)solarCfg["baudRate"].as<int>() : SOLAR_DEFAULT_BAUD_RATE;
    cfg.solarCharger.modbusTimeoutMs = solarCfg["timeoutMs"].is<int>() ? (uint16_t)solarCfg["timeoutMs"].as<int>() : SOLAR_DEFAULT_TIMEOUT_MS;
    cfg.solarCharger.pollIntervalSec = solarCfg["pollIntervalSec"].is<int>() ? (uint16_t)solarCfg["pollIntervalSec"].as<int>() : SOLAR_DEFAULT_POLL_INTERVAL_SEC;
    cfg.solarCharger.batteryLowVoltage = solarCfg["batteryLowV"].is<float>() ? solarCfg["batteryLowV"].as<float>() : BATTERY_VOLTAGE_LOW;
    cfg.solarCharger.batteryCriticalVoltage = solarCfg["batteryCriticalV"].is<float>() ? solarCfg["batteryCriticalV"].as<float>() : BATTERY_VOLTAGE_CRITICAL;
    cfg.solarCharger.batteryHighVoltage = solarCfg["batteryHighV"].is<float>() ? solarCfg["batteryHighV"].as<float>() : BATTERY_VOLTAGE_HIGH;
    cfg.solarCharger.alertOnLowBattery = solarCfg["alertOnLow"].is<bool>() ? solarCfg["alertOnLow"].as<bool>() : true;
    cfg.solarCharger.alertOnFault = solarCfg["alertOnFault"].is<bool>() ? solarCfg["alertOnFault"].as<bool>() : false;
    cfg.solarCharger.alertOnCommFailure = solarCfg["alertOnCommFail"].is<bool>() ? solarCfg["alertOnCommFail"].as<bool>() : false;
    cfg.solarCharger.includeInDailyReport = solarCfg["includeInDaily"].is<bool>() ? solarCfg["includeInDaily"].as<bool>() : true;
  } else {
    // Default values if solarCharger object not present
    cfg.solarCharger.enabled = false;
    cfg.solarCharger.modbusSlaveId = SOLAR_DEFAULT_SLAVE_ID;
    cfg.solarCharger.modbusBaudRate = SOLAR_DEFAULT_BAUD_RATE;
    cfg.solarCharger.modbusTimeoutMs = SOLAR_DEFAULT_TIMEOUT_MS;
    cfg.solarCharger.pollIntervalSec = SOLAR_DEFAULT_POLL_INTERVAL_SEC;
    cfg.solarCharger.batteryLowVoltage = BATTERY_VOLTAGE_LOW;
    cfg.solarCharger.batteryCriticalVoltage = BATTERY_VOLTAGE_CRITICAL;
    cfg.solarCharger.batteryHighVoltage = BATTERY_VOLTAGE_HIGH;
    cfg.solarCharger.alertOnLowBattery = true;
    cfg.solarCharger.alertOnFault = false;
    cfg.solarCharger.alertOnCommFailure = false;
    cfg.solarCharger.includeInDailyReport = true;
  }
  
  // Sanitize and clamp loaded solar configuration (Task 1.3)
  sanitizeSolarConfig(cfg.solarCharger);

  // Load decoupled batteryConfig block (server v1.6.8+ generator).
  // Carries chemistry + nominal pack voltage; initBatteryConfig() computes
  // scaled thresholds. This is the single source of truth for battery setup.
  JsonObject batCfgNew = doc["batteryConfig"].as<JsonObject>();
  if (batCfgNew) {
    bool batEnabled = batCfgNew["enabled"].is<bool>() ? batCfgNew["enabled"].as<bool>() : true;
    BatteryType bt = batCfgNew["batteryType"].is<int>()
      ? (BatteryType)batCfgNew["batteryType"].as<int>()
      : BATTERY_TYPE_AGM;
    uint8_t nominalV = batCfgNew["nominalVoltage"].is<int>()
      ? (uint8_t)batCfgNew["nominalVoltage"].as<int>()
      : 12;
    initBatteryConfig(&cfg.batteryMonitor, bt, nominalV);
    cfg.batteryMonitor.enabled = batEnabled && (bt != BATTERY_TYPE_NONE);
    Serial.print(F("Battery config: type="));
    Serial.print(batteryTypeLabel(bt));
    Serial.print(F(" nominalV="));
    Serial.print(nominalV);
    Serial.print(F(" enabled="));
    Serial.println(cfg.batteryMonitor.enabled ? F("yes") : F("no"));
  } else {
    // No batteryConfig block — apply safe defaults (AGM 12V, monitoring disabled).
    initBatteryConfig(&cfg.batteryMonitor, BATTERY_TYPE_AGM, 12);
    cfg.batteryMonitor.enabled = false;
  }

  // Load analog Vin voltage divider configuration
  JsonObject vinCfg = doc["vinMonitor"].as<JsonObject>();
  if (vinCfg) {
    cfg.vinMonitor.enabled = vinCfg["enabled"].is<bool>() ? vinCfg["enabled"].as<bool>() : false;
    cfg.vinMonitor.analogPin = vinCfg["pin"].is<int>() ? (uint8_t)vinCfg["pin"].as<int>() : VIN_MONITOR_DEFAULT_PIN;
    cfg.vinMonitor.r1Kohm = vinCfg["r1Kohm"].is<float>() ? vinCfg["r1Kohm"].as<float>() : VIN_MONITOR_DEFAULT_R1_KOHM;
    cfg.vinMonitor.r2Kohm = vinCfg["r2Kohm"].is<float>() ? vinCfg["r2Kohm"].as<float>() : VIN_MONITOR_DEFAULT_R2_KOHM;
    cfg.vinMonitor.pollIntervalSec = vinCfg["pollIntervalSec"].is<int>() ? (uint16_t)vinCfg["pollIntervalSec"].as<int>() : VIN_MONITOR_DEFAULT_POLL_SEC;
    cfg.vinMonitor.includeInDailyReport = vinCfg["includeInDaily"].is<bool>() ? vinCfg["includeInDaily"].as<bool>() : true;
    // Validate pin range (A0-A7)
    if (cfg.vinMonitor.analogPin > 7) cfg.vinMonitor.analogPin = VIN_MONITOR_DEFAULT_PIN;
    // Validate resistor values
    if (cfg.vinMonitor.r1Kohm <= 0.0f) cfg.vinMonitor.r1Kohm = VIN_MONITOR_DEFAULT_R1_KOHM;
    if (cfg.vinMonitor.r2Kohm <= 0.0f) cfg.vinMonitor.r2Kohm = VIN_MONITOR_DEFAULT_R2_KOHM;
  } else {
    initVinMonitorConfig(&cfg.vinMonitor);
  }

  // Load solar-only (no battery) mode configuration
  JsonObject soCfg = doc["solarOnlyConfig"].as<JsonObject>();
  if (soCfg) {
    cfg.solarOnlyConfig.enabled = soCfg["enabled"].is<bool>() ? soCfg["enabled"].as<bool>() : false;
    cfg.solarOnlyConfig.startupDebounceVoltage = soCfg["startupDebounceVoltage"].is<float>() ? soCfg["startupDebounceVoltage"].as<float>() : SOLAR_ONLY_DEFAULT_DEBOUNCE_VOLTAGE;
    cfg.solarOnlyConfig.startupDebounceSec = soCfg["startupDebounceSec"].is<int>() ? (uint16_t)soCfg["startupDebounceSec"].as<int>() : SOLAR_ONLY_DEFAULT_DEBOUNCE_SEC;
    cfg.solarOnlyConfig.startupWarmupSec = soCfg["startupWarmupSec"].is<int>() ? (uint16_t)soCfg["startupWarmupSec"].as<int>() : SOLAR_ONLY_DEFAULT_WARMUP_SEC;
    cfg.solarOnlyConfig.sensorGateVoltage = soCfg["sensorGateVoltage"].is<float>() ? soCfg["sensorGateVoltage"].as<float>() : SOLAR_ONLY_DEFAULT_SENSOR_GATE_VOLTAGE;
    cfg.solarOnlyConfig.sunsetVoltage = soCfg["sunsetVoltage"].is<float>() ? soCfg["sunsetVoltage"].as<float>() : SOLAR_ONLY_DEFAULT_SUNSET_VOLTAGE;
    cfg.solarOnlyConfig.sunsetConfirmSec = soCfg["sunsetConfirmSec"].is<int>() ? (uint16_t)soCfg["sunsetConfirmSec"].as<int>() : SOLAR_ONLY_DEFAULT_SUNSET_CONFIRM_SEC;
    cfg.solarOnlyConfig.opportunisticReportHours = soCfg["opportunisticReportHours"].is<int>() ? (uint16_t)soCfg["opportunisticReportHours"].as<int>() : SOLAR_ONLY_DEFAULT_REPORT_HOURS;
    cfg.solarOnlyConfig.batteryFailureFallback = soCfg["batteryFailureFallback"].is<bool>() ? soCfg["batteryFailureFallback"].as<bool>() : false;
    cfg.solarOnlyConfig.batteryFailureThreshold = soCfg["batteryFailureThreshold"].is<int>() ? (uint8_t)soCfg["batteryFailureThreshold"].as<int>() : SOLAR_ONLY_DEFAULT_FAILURE_THRESHOLD;
  } else {
    initSolarOnlyConfig(&cfg.solarOnlyConfig);
  }

  // Load remote-tunable power thresholds (0.0 = use compile-time default)
  cfg.powerEcoEnterV = doc["powerEcoEnterV"].is<float>() ? doc["powerEcoEnterV"].as<float>() : 0.0f;
  cfg.powerLowEnterV = doc["powerLowEnterV"].is<float>() ? doc["powerLowEnterV"].as<float>() : 0.0f;
  cfg.powerCriticalEnterV = doc["powerCriticalEnterV"].is<float>() ? doc["powerCriticalEnterV"].as<float>() : 0.0f;
  cfg.powerEcoExitV = doc["powerEcoExitV"].is<float>() ? doc["powerEcoExitV"].as<float>() : 0.0f;
  cfg.powerLowExitV = doc["powerLowExitV"].is<float>() ? doc["powerLowExitV"].as<float>() : 0.0f;
  cfg.powerCriticalExitV = doc["powerCriticalExitV"].is<float>() ? doc["powerCriticalExitV"].as<float>() : 0.0f;

  // Load remote-tunable health check interval
  cfg.healthCheckBaseIntervalMs = doc["healthCheckBaseIntervalMs"].is<int>() 
    ? (uint32_t)doc["healthCheckBaseIntervalMs"].as<int>() 
    : 0;

  // Load config epoch timestamp
  cfg.configEpoch = doc["configEpoch"].is<int>() ? (uint32_t)doc["configEpoch"].as<int>() : 0;

  // Support both "sensors" (from server) and "monitors" (local config) array names
  JsonArray monitorsArray = doc["sensors"].as<JsonArray>();
  if (!monitorsArray) {
    monitorsArray = doc["monitors"].as<JsonArray>();
  }
  cfg.monitorCount = monitorsArray ? min<uint8_t>(monitorsArray.size(), MAX_MONITORS) : 0;

  for (uint8_t i = 0; i < cfg.monitorCount; ++i) {
    JsonObjectConst t = monitorsArray[i];
    initMonitorDefaults(cfg.monitors[i], i);
    parseMonitorFromJson(cfg.monitors[i], t, i);
  }

  // Warn about overlapping relay ownership (ambiguous timeout behavior)
  for (uint8_t i = 0; i < cfg.monitorCount; i++) {
    for (uint8_t j = i + 1; j < cfg.monitorCount; j++) {
      if (cfg.monitors[i].relayMask & cfg.monitors[j].relayMask) {
        Serial.print(F("WARNING: Monitors "));
        Serial.print(i);
        Serial.print(F(" and "));
        Serial.print(j);
        Serial.println(F(" share relay bits - timeout behavior may be ambiguous"));
      }
    }
  }

  return true;
#else
  return false; // Filesystem not available
#endif
}

static bool saveConfigToFlash(const ClientConfig &cfg) {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) return false;
  #endif
  
  std::unique_ptr<JsonDocument> docPtr(new JsonDocument());
  if (!docPtr) return false;
  JsonDocument &doc = *docPtr;

  doc["configSchemaVersion"] = cfg.configSchemaVersion;
  doc["site"] = cfg.siteName;
  doc["deviceUid"] = cfg.deviceUid;
  doc["deviceLabel"] = cfg.deviceLabel;
  doc["clientFleet"] = cfg.clientFleet;
  doc["serverFleet"] = cfg.serverFleet;
  // Product UID: persist to flash so a remotely-pushed UID survives reboot
  if (cfg.productUid[0] != '\0') {
    doc["productUid"] = cfg.productUid;
  }
  doc["sampleSeconds"] = cfg.sampleSeconds;
  doc["reportHour"] = cfg.reportHour;
  doc["reportMinute"] = cfg.reportMinute;
  doc["dailyEmail"] = cfg.dailyEmail;
  
  // Save clear button configuration
  doc["clearButtonPin"] = cfg.clearButtonPin;
  doc["clearButtonActiveHigh"] = cfg.clearButtonActiveHigh;
  
  // Save power saving configuration
  doc["solarPowered"] = cfg.solarPowered;
  
  // Save I2C address configuration
  doc["currentLoopI2cAddress"] = cfg.currentLoopI2cAddress;

  // Save solar charger configuration (SunSaver MPPT via RS-485)
  JsonObject solarCfg = doc["solarCharger"].to<JsonObject>();
  solarCfg["enabled"] = cfg.solarCharger.enabled;
  solarCfg["slaveId"] = cfg.solarCharger.modbusSlaveId;
  solarCfg["baudRate"] = cfg.solarCharger.modbusBaudRate;
  solarCfg["timeoutMs"] = cfg.solarCharger.modbusTimeoutMs;
  solarCfg["pollIntervalSec"] = cfg.solarCharger.pollIntervalSec;
  solarCfg["batteryLowV"] = cfg.solarCharger.batteryLowVoltage;
  solarCfg["batteryCriticalV"] = cfg.solarCharger.batteryCriticalVoltage;
  solarCfg["batteryHighV"] = cfg.solarCharger.batteryHighVoltage;
  solarCfg["alertOnLow"] = cfg.solarCharger.alertOnLowBattery;
  solarCfg["alertOnFault"] = cfg.solarCharger.alertOnFault;
  solarCfg["alertOnCommFail"] = cfg.solarCharger.alertOnCommFailure;
  solarCfg["includeInDaily"] = cfg.solarCharger.includeInDailyReport;

  // Save battery configuration (decoupled chemistry + nominal voltage).
  // Mirrors the schema produced by the server config generator so the loader's
  // single `batteryConfig` parser handles both server-pushed and flash-restored
  // configs.
  JsonObject batCfg = doc["batteryConfig"].to<JsonObject>();
  batCfg["enabled"] = cfg.batteryMonitor.enabled;
  batCfg["batteryType"] = (int)cfg.batteryMonitor.batteryType;
  batCfg["nominalVoltage"] = cfg.batteryMonitor.nominalVoltage;

  // Save analog Vin voltage divider configuration (I-25 fix v1.6.13).
  // applyConfigUpdate() applies inbound vinMonitor settings, so the save
  // path must mirror the loader to keep the round-trip durable.
  JsonObject vinCfg = doc["vinMonitor"].to<JsonObject>();
  vinCfg["enabled"] = cfg.vinMonitor.enabled;
  vinCfg["pin"] = cfg.vinMonitor.analogPin;
  vinCfg["r1Kohm"] = cfg.vinMonitor.r1Kohm;
  vinCfg["r2Kohm"] = cfg.vinMonitor.r2Kohm;
  vinCfg["pollIntervalSec"] = cfg.vinMonitor.pollIntervalSec;
  vinCfg["includeInDaily"] = cfg.vinMonitor.includeInDailyReport;

  // Save solar-only (no-battery) mode configuration (I-26 fix v1.6.13).
  // Same shape as the load/apply path so a server-pushed solarOnlyConfig
  // survives a reboot in offline-only deployments.
  JsonObject soCfg = doc["solarOnlyConfig"].to<JsonObject>();
  soCfg["enabled"] = cfg.solarOnlyConfig.enabled;
  soCfg["startupDebounceVoltage"] = cfg.solarOnlyConfig.startupDebounceVoltage;
  soCfg["startupDebounceSec"] = cfg.solarOnlyConfig.startupDebounceSec;
  soCfg["startupWarmupSec"] = cfg.solarOnlyConfig.startupWarmupSec;
  soCfg["sensorGateVoltage"] = cfg.solarOnlyConfig.sensorGateVoltage;
  soCfg["sunsetVoltage"] = cfg.solarOnlyConfig.sunsetVoltage;
  soCfg["sunsetConfirmSec"] = cfg.solarOnlyConfig.sunsetConfirmSec;
  soCfg["opportunisticReportHours"] = cfg.solarOnlyConfig.opportunisticReportHours;
  soCfg["batteryFailureFallback"] = cfg.solarOnlyConfig.batteryFailureFallback;
  soCfg["batteryFailureThreshold"] = cfg.solarOnlyConfig.batteryFailureThreshold;

  JsonArray sensors = doc["sensors"].to<JsonArray>();
  for (uint8_t i = 0; i < cfg.monitorCount; ++i) {
    JsonObject t = sensors.add<JsonObject>();
    char idBuffer[2] = {cfg.monitors[i].id, '\0'};
    t["id"] = idBuffer;
    t["name"] = cfg.monitors[i].name;
    if (cfg.monitors[i].contents[0] != '\0') {
      t["contents"] = cfg.monitors[i].contents;
    }
    t["number"] = cfg.monitors[i].sensorIndex;
    if (cfg.monitors[i].userNumber > 0) {
      t["userNumber"] = cfg.monitors[i].userNumber;
    }
    // Serialize object type (matches config generator key)
    switch (cfg.monitors[i].objectType) {
      case OBJECT_ENGINE: t["monitorType"] = "rpm"; break;
      case OBJECT_PUMP:   t["monitorType"] = "pump"; break;
      case OBJECT_GAS:    t["monitorType"] = "gas"; break;
      case OBJECT_FLOW:   t["monitorType"] = "flow"; break;
      default:            t["monitorType"] = "tank"; break;
    }
    switch (cfg.monitors[i].sensorInterface) {
      case SENSOR_DIGITAL: t["sensor"] = "digital"; break;
      case SENSOR_CURRENT_LOOP: t["sensor"] = "current"; break;
      case SENSOR_PULSE: t["sensor"] = "rpm"; break;
      default: t["sensor"] = "analog"; break;
    }
    t["primaryPin"] = cfg.monitors[i].primaryPin;
    t["secondaryPin"] = cfg.monitors[i].secondaryPin;
    t["loopChannel"] = cfg.monitors[i].currentLoopChannel;
    t["rpmPin"] = cfg.monitors[i].pulsePin;
    t["pulsesPerRev"] = cfg.monitors[i].pulsesPerUnit;
    // Save hall effect sensor type
    switch (cfg.monitors[i].hallEffectType) {
      case HALL_EFFECT_BIPOLAR: t["hallEffectType"] = "bipolar"; break;
      case HALL_EFFECT_OMNIPOLAR: t["hallEffectType"] = "omnipolar"; break;
      case HALL_EFFECT_ANALOG: t["hallEffectType"] = "analog"; break;
      case HALL_EFFECT_UNIPOLAR:
      default: t["hallEffectType"] = "unipolar"; break;
    }
    // Save hall effect detection method
    switch (cfg.monitors[i].hallEffectDetection) {
      case HALL_DETECT_TIME_BASED: t["hallEffectDetection"] = "time"; break;
      case HALL_DETECT_PULSE:
      default: t["hallEffectDetection"] = "pulse"; break;
    }
    // Save RPM sampling configuration
    t["rpmSampleDurationMs"] = cfg.monitors[i].pulseSampleDurationMs;
    t["rpmAccumulatedMode"] = cfg.monitors[i].pulseAccumulatedMode;
    if (cfg.monitors[i].expectedPulseRate > 0.0f) {
      t["expectedPulseRate"] = cfg.monitors[i].expectedPulseRate;
    }
    t["alarmsEnabled"] = cfg.monitors[i].alarmsEnabled;
    t["highAlarm"] = cfg.monitors[i].highAlarmThreshold;
    t["lowAlarm"] = cfg.monitors[i].lowAlarmThreshold;
    t["hysteresis"] = cfg.monitors[i].hysteresisValue;
    t["daily"] = cfg.monitors[i].enableDailyReport;
    t["alarmSms"] = cfg.monitors[i].enableAlarmSms;
    t["upload"] = cfg.monitors[i].enableServerUpload;
    t["reportThreshold"] = cfg.monitors[i].reportThreshold;
    // Save relay control settings
    t["relayTargetClient"] = cfg.monitors[i].relayTargetClient;
    t["relayMask"] = cfg.monitors[i].relayMask;
    // Save relay trigger condition as string
    switch (cfg.monitors[i].relayTrigger) {
      case RELAY_TRIGGER_HIGH: t["relayTrigger"] = "high"; break;
      case RELAY_TRIGGER_LOW: t["relayTrigger"] = "low"; break;
      default: t["relayTrigger"] = "any"; break;
    }
    // Save relay mode as string
    switch (cfg.monitors[i].relayMode) {
      case RELAY_MODE_UNTIL_CLEAR: t["relayMode"] = "until_clear"; break;
      case RELAY_MODE_MANUAL_RESET: t["relayMode"] = "manual_reset"; break;
      default: t["relayMode"] = "momentary"; break;
    }
    // Save per-relay momentary durations
    JsonArray durations = t["relayMomentaryDurations"].to<JsonArray>();
    for (uint8_t r = 0; r < 4; ++r) {
      durations.add(cfg.monitors[i].relayMomentarySeconds[r]);
    }
    // Save max ON duration for MANUAL_RESET safety timeout
    if (cfg.monitors[i].relayMaxOnSeconds > 0) {
      t["relayMaxOnSeconds"] = cfg.monitors[i].relayMaxOnSeconds;
    }
    // Save digital sensor trigger state (for float switches)
    if (cfg.monitors[i].digitalTrigger[0] != '\0') {
      t["digitalTrigger"] = cfg.monitors[i].digitalTrigger;
    }
    // Save digital switch mode (NO/NC)
    t["digitalSwitchMode"] = cfg.monitors[i].digitalSwitchMode;
    // Save 4-20mA current loop sensor type
    switch (cfg.monitors[i].currentLoopType) {
      case CURRENT_LOOP_ULTRASONIC: t["currentLoopType"] = "ultrasonic"; break;
      default: t["currentLoopType"] = "pressure"; break;
    }
    // Save sensor mount height (for calibration)
    t["sensorMountHeight"] = cfg.monitors[i].sensorMountHeight;
    // Save PWM Gating configurations
    t["pwmGatingEnabled"] = cfg.monitors[i].pwmGatingEnabled;
    t["pwmGatingChannel"] = cfg.monitors[i].pwmGatingChannel;
    t["pwmGatingWarmup"] = cfg.monitors[i].pwmGatingWarmup;
    t["pwmGatingSampleDelay"] = cfg.monitors[i].pwmGatingSampleDelay;
    // Save sensor native range settings
    t["sensorRangeMin"] = cfg.monitors[i].sensorRangeMin;
    t["sensorRangeMax"] = cfg.monitors[i].sensorRangeMax;
    t["sensorRangeUnit"] = cfg.monitors[i].sensorRangeUnit;
    // Save fluid characterization (allows server to display preset and round-trip custom SG)
    t["fluidType"] = fluidTypeName(cfg.monitors[i].fluidType);
    if (cfg.monitors[i].fluidSpecificGravity > 0.0f) {
      t["fluidSpecificGravity"] = cfg.monitors[i].fluidSpecificGravity;
    }
    // Persist server-pushed learned calibration so it survives reboots until the next push.
    if (cfg.monitors[i].hasLearnedCalibration) {
      t["calHasCal"] = true;
      t["calSlope"] = cfg.monitors[i].calSlope;
      t["calOffset"] = cfg.monitors[i].calOffset;
      t["calTempCoef"] = cfg.monitors[i].calTempCoef;
      t["calTempF"] = cfg.monitors[i].calTempF;
      t["calVersion"] = cfg.monitors[i].calVersion;
    }
    // Save analog voltage range settings
    t["analogVoltageMin"] = cfg.monitors[i].analogVoltageMin;
    t["analogVoltageMax"] = cfg.monitors[i].analogVoltageMax;
    // Save stuck sensor detection setting
    t["stuckDetection"] = cfg.monitors[i].stuckDetectionEnabled;
    // Save tank unload tracking settings
    t["trackUnloads"] = cfg.monitors[i].trackUnloads;
    t["unloadEmptyHeight"] = cfg.monitors[i].unloadEmptyHeight;
    t["unloadDropThreshold"] = cfg.monitors[i].unloadDropThreshold;
    t["unloadDropPercent"] = cfg.monitors[i].unloadDropPercent;
    t["unloadAlarmSms"] = cfg.monitors[i].unloadAlarmSms;
    t["unloadAlarmEmail"] = cfg.monitors[i].unloadAlarmEmail;
  }

  // Save remote-tunable power thresholds (only if non-default)
  if (cfg.powerEcoEnterV > 0.0f) doc["powerEcoEnterV"] = cfg.powerEcoEnterV;
  if (cfg.powerLowEnterV > 0.0f) doc["powerLowEnterV"] = cfg.powerLowEnterV;
  if (cfg.powerCriticalEnterV > 0.0f) doc["powerCriticalEnterV"] = cfg.powerCriticalEnterV;
  if (cfg.powerEcoExitV > 0.0f) doc["powerEcoExitV"] = cfg.powerEcoExitV;
  if (cfg.powerLowExitV > 0.0f) doc["powerLowExitV"] = cfg.powerLowExitV;
  if (cfg.powerCriticalExitV > 0.0f) doc["powerCriticalExitV"] = cfg.powerCriticalExitV;

  // Save remote-tunable health check interval (only if non-default)
  if (cfg.healthCheckBaseIntervalMs > 0) doc["healthCheckBaseIntervalMs"] = cfg.healthCheckBaseIntervalMs;

  // Save config epoch
  doc["configEpoch"] = cfg.configEpoch;

  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    // Mbed OS — atomic write-to-temp-then-rename
    // Use fixed buffer instead of Arduino String to avoid heap fragmentation
    char jsonBuf[4096];
    size_t len = serializeJson(doc, jsonBuf, sizeof(jsonBuf));
    if (len == 0) {
      Serial.println(F("Failed to serialize config"));
      return false;
    }
    if (!tankalarm_posix_write_file_atomic("/cfg/client_config.json",
                                            jsonBuf, len)) {
      Serial.println(F("Failed to write config"));
      return false;
    }
    return true;
  #else
    // Use fixed buffer instead of Arduino String to avoid heap fragmentation
    char jsonBuf[4096];
    size_t len = serializeJson(doc, jsonBuf, sizeof(jsonBuf));
    if (len == 0) {
      Serial.println(F("Failed to serialize config"));
      return false;
    }
    if (!tankalarm_littlefs_write_file_atomic(CLIENT_CONFIG_PATH,
            (const uint8_t *)jsonBuf, len)) {
      Serial.println(F("Failed to write config"));
      return false;
    }
    return true;
  #endif
#else
  return false; // Filesystem not available
#endif
}

static void printHardwareRequirements(const ClientConfig &cfg) {
  if (gHardwareSummaryPrinted) {
    return;
  }
  gHardwareSummaryPrinted = true;

  bool needsAnalogExpansion = false;
  bool needsCurrentLoop = false;
  bool needsRelayOutput = false;
  bool needsRpmSensor = false;
  bool hasPressureSensor = false;
  bool hasUltrasonicSensor = false;

  for (uint8_t i = 0; i < cfg.monitorCount; ++i) {
    if (cfg.monitors[i].sensorInterface == SENSOR_ANALOG) {
      needsAnalogExpansion = true;
    }
    if (cfg.monitors[i].sensorInterface == SENSOR_CURRENT_LOOP) {
      needsCurrentLoop = true;
      if (cfg.monitors[i].currentLoopType == CURRENT_LOOP_PRESSURE) {
        hasPressureSensor = true;
      } else if (cfg.monitors[i].currentLoopType == CURRENT_LOOP_ULTRASONIC) {
        hasUltrasonicSensor = true;
      }
    }
    if (cfg.monitors[i].sensorInterface == SENSOR_PULSE) {
      needsRpmSensor = true;
    }
    if (cfg.monitors[i].enableAlarmSms) {
      needsRelayOutput = true; // indicates coil notifications on Opta outputs
    }
  }

  Serial.println(F("--- Hardware Requirements ---"));
  Serial.println(F("Base: Arduino Opta Lite"));
  Serial.println(F("Connectivity: Blues Wireless Notecard (I2C 0x17)"));
  if (needsAnalogExpansion || needsCurrentLoop) {
    Serial.println(F("Analog/Current: Arduino Pro Opta Ext A0602"));
  }
  if (needsCurrentLoop) {
    Serial.println(F("Current loop interface required (4-20mA module)"));
    if (hasPressureSensor) {
      Serial.println(F("  - Pressure sensor (bottom-mounted, e.g., Dwyer 626-06-CB-P1-E5-S1)"));
    }
    if (hasUltrasonicSensor) {
      Serial.println(F("  - Ultrasonic sensor (top-mounted, e.g., Siemens Sitrans LU240)"));
    }
  }
  if (needsRpmSensor) {
    Serial.println(F("Hall effect RPM sensor connected to digital input"));
  }
  if (needsRelayOutput) {
    Serial.println(F("Relay outputs wired for audible/visual alarm"));
  }
  if (cfg.solarCharger.enabled) {
    Serial.println(F("Solar Charger Monitoring (SunSaver MPPT):"));
    Serial.println(F("  - Arduino Opta WiFi (AFX00002) or Opta RS485 (AFX00001)"));
    Serial.println(F("    Opta Lite (AFX00003) requires external RS485 transceiver"));
    Serial.println(F("  - Morningstar MRC-1 (MeterBus to EIA-485 Adapter)"));
    Serial.println(F("    Powered by SunSaver via RJ-11, no external power needed"));
    Serial.println(F("  - RS485 Wiring: Opta A(-) to MRC-1 B(-), Opta B(+) to MRC-1 A(+)"));
    Serial.print(F("  - Modbus: Slave ID "));
    Serial.print(cfg.solarCharger.modbusSlaveId);
    Serial.print(F(", "));
    Serial.print(cfg.solarCharger.modbusBaudRate);
    Serial.println(F(" baud"));
  } else if (cfg.solarPowered) {
    Serial.println(F("Solar Power (Non-Modbus Charger):"));
    Serial.println(F("  - Compatible with simple regulators (e.g., SunKeeper-6)"));
    Serial.println(F("  - No RS485 or Modbus telemetry required"));
    Serial.println(F("  - Use batteryMonitor and/or vinMonitor for voltage health"));
  }
  if (cfg.batteryMonitor.enabled) {
    Serial.println(F("Battery Voltage Monitoring (Notecard direct):"));
    Serial.println(F("  - Notecard VIN wired directly to 12V battery"));
    Serial.println(F("  - Optional: Schottky diode for reverse polarity protection"));
    Serial.print(F("  - Battery type: "));
    Serial.print(batteryTypeLabel(cfg.batteryMonitor.batteryType));
    Serial.print(F(" / "));
    Serial.print(cfg.batteryMonitor.nominalVoltage);
    Serial.println(F("V nominal"));
    Serial.print(F("  - Thresholds: Low="));
    Serial.print(cfg.batteryMonitor.lowVoltage, 1);
    Serial.print(F("V, Critical="));
    Serial.print(cfg.batteryMonitor.criticalVoltage, 1);
    Serial.println(F("V"));
  }
  if (cfg.vinMonitor.enabled) {
    float ratio = vinDividerRatio(&cfg.vinMonitor);
    float maxV = vinMaxReadableVoltage(&cfg.vinMonitor);
    Serial.println(F("Vin Voltage Divider Monitor:"));
    Serial.print(F("  - Analog pin: A"));
    Serial.println(cfg.vinMonitor.analogPin);
    Serial.print(F("  - Resistors: R1="));
    Serial.print(cfg.vinMonitor.r1Kohm, 1);
    Serial.print(F("k, R2="));
    Serial.print(cfg.vinMonitor.r2Kohm, 1);
    Serial.println(F("k"));
    Serial.print(F("  - Divider ratio: "));
    Serial.print(ratio, 4);
    Serial.print(F(", Max readable: "));
    Serial.print(maxV, 1);
    Serial.println(F("V"));
  }
  if (cfg.solarOnlyConfig.enabled) {
    Serial.println(F("Solar-Only (No Battery) Mode:"));
    Serial.println(F("  - Device operates only during daylight hours"));
    Serial.println(F("  - Daily reports sent opportunistically on startup"));
    if (cfg.vinMonitor.enabled) {
      Serial.print(F("  - Startup debounce: "));
      Serial.print(cfg.solarOnlyConfig.startupDebounceVoltage, 1);
      Serial.print(F("V for "));
      Serial.print(cfg.solarOnlyConfig.startupDebounceSec);
      Serial.println(F("s"));
      Serial.print(F("  - Sensor gate voltage: "));
      Serial.print(cfg.solarOnlyConfig.sensorGateVoltage, 1);
      Serial.println(F("V"));
      Serial.print(F("  - Sunset threshold: "));
      Serial.print(cfg.solarOnlyConfig.sunsetVoltage, 1);
      Serial.println(F("V"));
    } else {
      Serial.print(F("  - Startup warmup: "));
      Serial.print(cfg.solarOnlyConfig.startupWarmupSec);
      Serial.println(F("s (no Vin divider)"));
    }
    Serial.print(F("  - Opportunistic report threshold: "));
    Serial.print(cfg.solarOnlyConfig.opportunisticReportHours);
    Serial.println(F("h"));
  }
  if (cfg.solarOnlyConfig.batteryFailureFallback) {
    Serial.println(F("Battery Failure Fallback:"));
    Serial.print(F("  - Auto-enable solar-only behaviors after "));
    Serial.print(cfg.solarOnlyConfig.batteryFailureThreshold);
    Serial.println(F(" consecutive critical readings"));
  }
  Serial.println(F("-----------------------------"));
}

static void configureNotecardHubMode() {
  // Configure Notecard hub mode based on power source
  J *req = notecard.newRequest("hub.set");
  if (req) {
    // Use configurable product UID - allows fleet-specific deployments without recompilation
    const char *productUid = (gConfig.productUid[0] != '\0') ? gConfig.productUid : DEFAULT_PRODUCT_UID;
    if (productUid[0] == '\0') {
      Serial.println(F("WARNING: No Product UID configured!"));
      Serial.println(F("  Create ClientConfig.h or push config from server Config Generator."));
      addSerialLog("No Product UID - Notecard will not sync");
    }
    JAddStringToObject(req, "product", productUid);
    if (gConfig.clientFleet[0] != '\0') {
      JAddStringToObject(req, "fleet", gConfig.clientFleet);
    }
    Serial.print(F("Product UID: "));
    Serial.println(productUid);
    
    // Power saving configuration based on power source
    if (gConfig.solarPowered) {
      // Solar powered: Use periodic mode with extended inbound check to save power.
      // When unconfigured, use a short inbound interval so the Notecard wakes
      // frequently to receive the first config push from the server.
      // configureNotecardHubMode() is called again by applyConfigUpdate(), which
      // restores SOLAR_INBOUND_INTERVAL_MINUTES once monitors are configured.
      int inboundMinutes = (gConfig.monitorCount == 0)
          ? AWAITING_CONFIG_SOLAR_INBOUND_MINUTES
          : SOLAR_INBOUND_INTERVAL_MINUTES;
      JAddStringToObject(req, "mode", "periodic");
      JAddIntToObject(req, "outbound", SOLAR_OUTBOUND_INTERVAL_MINUTES);  // Sync every 6 hours
      JAddIntToObject(req, "inbound", inboundMinutes);
    } else {
      // Grid-tied: Use continuous mode for faster response times
      JAddStringToObject(req, "mode", "continuous");
      // In continuous mode, outbound/inbound are not used - always connected
    }
    
    // Verify hub mode was set — if this fails the Notecard stays in
    // "minimum" mode and won't pull inbound notes (config.qi).
    // checkNotecardHealth() will retry on recovery if it fails here.
    J *rsp = notecard.requestAndResponse(req);
    if (rsp) {
      const char *err = JGetString(rsp, "err");
      if (err && err[0] != '\0') {
        Serial.print(F("WARNING: hub.set failed: "));
        Serial.println(err);
      }
      notecard.deleteResponse(rsp);
    } else {
      Serial.println(F("WARNING: hub.set no response (Notecard not ready?)"));
    }
  }
  
  // Disable GPS location tracking for power savings
  // GPS is one of the most power-hungry components on the Notecard and is not used by this application
  req = notecard.newRequest("card.location.mode");
  if (req) {
    JAddStringToObject(req, "mode", "off");
    notecard.sendRequest(req);
  }
  
  // Disable accelerometer motion tracking for power savings
  // The accelerometer is not used by this sensor monitoring application
  // card.motion.sync controls motion-triggered syncing; stop prevents
  // the Notecard from syncing on motion detection.
  req = notecard.newRequest("card.motion.sync");
  if (req) {
    JAddBoolToObject(req, "stop", true);
    notecard.sendRequest(req);
  }
}

static void initializeNotecard() {
#ifdef DEBUG_MODE
  notecard.setDebugOutputStream(Serial);
#endif
  tankalarm_ensureNotecardBinding(notecard);

  // Configure hub mode (fire-and-forget — if the Notecard isn't ready yet,
  // these will silently fail; checkNotecardHealth() reconfigures hub mode
  // when the Notecard next responds, ensuring inbound sync is established).
  configureNotecardHubMode();

  // Enable user DFU before the first startup sync so Notehub firmware is
  // accepted into the host-pull channel instead of any persisted ODFU channel.
  tankalarm_enableIapDfu(notecard);

  // Take the Notecard OUT of outboard DFU (ODFU) mode. The Blues Wireless for
  // Opta carrier does not route the BOOT0/NRST/USART1 lines the Notecard needs
  // to flash the STM32 host directly, so any ODFU attempt always fails with
  // "stmConnectToBootloader: timeout" ({odfu-fail}). Setting name:"-" clears
  // the outboard host MCU type so Notehub never tries to push firmware to the
  // host itself. Instead the delivered image stays in Notecard storage and the
  // host pulls it via dfu.get + applies it with MCUboot
  // (tankalarm_performMcubootUpdate). off:true is kept as a belt-and-suspenders
  // guard against any autonomous host reset.
  {
    J *dfuReq = notecard.newRequest("card.dfu");
    if (dfuReq) {
      JAddStringToObject(dfuReq, "name", "-");
      JAddBoolToObject(dfuReq, "off", true);
      J *dfuRsp = notecard.requestAndResponse(dfuReq);
      if (dfuRsp) {
        notecard.deleteResponse(dfuRsp);
      }
    }
  }

  // Force an immediate sync so any inbound notes queued on Notehub
  // (e.g. config.qi) are pulled down right away at startup. This must happen
  // after clearing ODFU so a persisted stm32 host type cannot trigger a failed
  // outboard DFU attempt during the first sync after a USB update.
  J *syncReq = notecard.newRequest("hub.sync");
  if (syncReq) {
    J *syncRsp = notecard.requestAndResponse(syncReq);
    if (syncRsp) {
      const char *syncErr = JGetString(syncRsp, "err");
      if (syncErr && syncErr[0] != '\0') {
        Serial.print(F("Startup hub.sync warning: "));
        Serial.println(syncErr);
      } else {
        Serial.println(F("Startup hub.sync initiated"));
      }
      notecard.deleteResponse(syncRsp);
    }
  }

  // Try to retrieve the Notecard's Device UID (e.g. "dev:860322068012345").
  J *req = notecard.newRequest("hub.get");
  if (req) {
    J *rsp = notecard.requestAndResponse(req);
    if (rsp) {
      const char *uid = JGetString(rsp, "device");
      if (uid && uid[0] != '\0') {
        strlcpy(gDeviceUID, uid, sizeof(gDeviceUID));
        // Persist UID in config so it survives reboot even if the Notecard
        // hasn't synced yet on next startup (avoids phantom client IDs).
        if (strcmp(gConfig.deviceUid, uid) != 0) {
          strlcpy(gConfig.deviceUid, uid, sizeof(gConfig.deviceUid));
          gConfigDirty = true;
        }
      }
      notecard.deleteResponse(rsp);
    }
  }

  // Fallback chain: persisted UID from config → deviceLabel → hardcoded default
  if (gDeviceUID[0] == '\0' && gConfig.deviceUid[0] != '\0') {
    strlcpy(gDeviceUID, gConfig.deviceUid, sizeof(gDeviceUID));
    Serial.println(F("Device UID loaded from config (Notecard not ready)"));
  }
  if (gDeviceUID[0] == '\0') {
    strlcpy(gDeviceUID, gConfig.deviceLabel, sizeof(gDeviceUID));
    Serial.println(F("Warning: Using deviceLabel as UID fallback"));
  }

  Serial.print(F("Device UID: "));
  Serial.println(gDeviceUID);
}

static bool checkNotecardHealth() {
  J *req = notecard.newRequest("card.wireless");
  if (!req) {
    gNotecardFailureCount++;
    if (gNotecardFailureCount >= NOTECARD_FAILURE_THRESHOLD && gNotecardAvailable) {
      gNotecardAvailable = false;
      Serial.println(F("Notecard unavailable - entering offline mode"));
    }
    // After sustained failures beyond threshold, attempt I2C bus recovery
    if (gNotecardFailureCount == I2C_NOTECARD_RECOVERY_THRESHOLD) {
      Serial.println(F("Sustained Notecard failure - attempting I2C bus recovery"));
      recoverI2CBus();
      logI2CRecoveryEvent(I2C_RECOVERY_NOTECARD_FAILURE);
      tankalarm_ensureNotecardBinding(notecard);
    }
    return false;
  }
  
  J *rsp = notecard.requestAndResponse(req);
  if (!rsp) {
    gNotecardFailureCount++;
    if (gNotecardFailureCount >= NOTECARD_FAILURE_THRESHOLD && gNotecardAvailable) {
      gNotecardAvailable = false;
      Serial.println(F("Notecard unavailable - entering offline mode"));
    }
    // After sustained failures beyond threshold, attempt I2C bus recovery
    if (gNotecardFailureCount == I2C_NOTECARD_RECOVERY_THRESHOLD) {
      Serial.println(F("Sustained Notecard failure - attempting I2C bus recovery"));
      recoverI2CBus();
      logI2CRecoveryEvent(I2C_RECOVERY_NOTECARD_FAILURE);
      tankalarm_ensureNotecardBinding(notecard);
    }
    return false;
  }
  
  // Check for error in card.wireless response
  const char *wirelessErr = JGetString(rsp, "err");
  if (wirelessErr && wirelessErr[0] != '\0') {
    Serial.print(F("card.wireless error: "));
    Serial.println(wirelessErr);
    notecard.deleteResponse(rsp);
    // The Notecard answered over I2C, so the transport is healthy even though the
    // cellular layer reported an error. Mark it available so OTA checks and note
    // sends are not blocked by a wireless-only fault - otherwise a unit that
    // briefly went offline could stay flagged offline indefinitely.
    if (!gNotecardAvailable) {
      Serial.println(F("Notecard responding (I2C OK, wireless error) - online mode restored"));
      tankalarm_ensureNotecardBinding(notecard);
      configureNotecardHubMode();
    }
    gNotecardAvailable = true;
    gNotecardFailureCount = 0;
    gLastSuccessfulNotecardComm = millis();
    return true;  // Notecard is responding (I2C OK), just wireless issue
  }

  // Extract cellular signal strength from card.wireless response
  J *net = JGetObject(rsp, "net");
  if (net) {
    int bars = JGetInt(rsp, "bars");
    if (bars >= 0 && bars <= 4) gSignalBars = (int8_t)bars;
    int rssi = JGetInt(net, "rssi");
    if (rssi != 0) gSignalRssi = (int16_t)rssi;
    int rsrp = JGetInt(net, "rsrp");
    if (rsrp != 0) gSignalRsrp = (int16_t)rsrp;
    int rsrq = JGetInt(net, "rsrq");
    if (rsrq != 0) gSignalRsrq = (int16_t)rsrq;
    const char *rat = JGetString(net, "rat");
    if (rat && rat[0] != '\0') strlcpy(gSignalRat, rat, sizeof(gSignalRat));
  } else {
    // Fallback: some Notecard firmware versions put bars at top level
    int bars = JGetInt(rsp, "bars");
    if (bars >= 0 && bars <= 4) gSignalBars = (int8_t)bars;
  }
  notecard.deleteResponse(rsp);
  
  // Notecard is responding — reinitialize Notecard I2C binding if recovering
  if (!gNotecardAvailable) {
    Serial.println(F("Notecard recovered - online mode restored"));
    // Re-attach Notecard to Wire in case bus was recovered
    tankalarm_ensureNotecardBinding(notecard);
    // Reconfigure hub mode — the initial boot-time hub.set may have failed
    // silently while the Notecard was unresponsive, leaving it in "minimum"
    // mode which does not pull inbound notes (config.qi would stay on Notehub).
    configureNotecardHubMode();
  }
  gNotecardAvailable = true;
  gNotecardFailureCount = 0;
  gLastSuccessfulNotecardComm = millis();

  // Modem stall detection: card.wireless responds (I2C OK) but no note has
  // been successfully sent in 4+ hours. This indicates the modem may be stuck
  // internally (e.g., locked in a bad cellular state). Issue card.restart to
  // reset the modem and re-establish the cellular connection.
  if (gLastSuccessfulNoteSend > 0 &&
      (millis() - gLastSuccessfulNoteSend) > NOTECARD_MODEM_STALL_MS) {
    Serial.println(F("WARNING: Modem stall detected — card.wireless OK but no notes sent in 4+ hours"));
    Serial.println(F("Issuing card.restart to reset modem..."));
    addSerialLog("Modem stall: card.restart issued");
    J *restartReq = notecard.newRequest("card.restart");
    if (restartReq) {
      J *restartRsp = notecard.requestAndResponse(restartReq);
      if (restartRsp) {
        const char *restartErr = JGetString(restartRsp, "err");
        if (restartErr && restartErr[0] != '\0') {
          Serial.print(F("WARNING: card.restart failed: "));
          Serial.println(restartErr);
        }
        notecard.deleteResponse(restartRsp);
      } else {
        Serial.println(F("WARNING: card.restart no response"));
      }
    }
    gLastSuccessfulNoteSend = millis();  // Reset to avoid repeated restarts
    gNotecardAvailable = false;  // Mark unavailable until next health check confirms recovery
    return true;  // Health check itself succeeded
  }

  // Deferred UID resolution: if the Notecard wasn't ready at boot, the device
  // UID may still be a fallback (deviceLabel).  Now that the Notecard is
  // healthy, retry hub.get to obtain the real UID and persist it.
  if (gConfig.deviceUid[0] == '\0') {
    J *uidReq = notecard.newRequest("hub.get");
    if (uidReq) {
      J *uidRsp = notecard.requestAndResponse(uidReq);
      if (uidRsp) {
        const char *uid = JGetString(uidRsp, "device");
        if (uid && uid[0] != '\0') {
          strlcpy(gDeviceUID, uid, sizeof(gDeviceUID));
          strlcpy(gConfig.deviceUid, uid, sizeof(gConfig.deviceUid));
          gConfigDirty = true;
          Serial.print(F("Device UID resolved: "));
          Serial.println(uid);
        }
        notecard.deleteResponse(uidRsp);
      }
    }
  }

  flushBufferedNotes();
  return true;
}

static void syncTimeFromNotecard() {
  J *req = notecard.newRequest("card.time");
  if (!req) {
    gNotecardFailureCount++;
    return;
  }
  J *rsp = notecard.requestAndResponse(req);
  if (!rsp) {
    gNotecardFailureCount++;
    return;
  }

  // Check for error response (e.g., "time is not yet set {no-time}")
  // This is normal during startup before Notecard syncs with cloud
  const char *err = JGetString(rsp, "err");
  if (err && strlen(err) > 0) {
    // Time not yet available - this is expected during startup
    // Will retry on next call - don't count this as a failure
    notecard.deleteResponse(rsp);
    return;
  }

  double time = JGetNumber(rsp, "time");
  if (time > 0) {
    gLastSyncedEpoch = time;
    gLastTimeSyncMillis = millis();
    gLastSuccessfulNotecardComm = millis();
    gNotecardFailureCount = 0;
  }
  notecard.deleteResponse(rsp);
}

static double currentEpoch() {
  if (gLastSyncedEpoch <= 0.0) {
    return 0.0;
  }
  unsigned long deltaMs = millis() - gLastTimeSyncMillis;
  return gLastSyncedEpoch + (double)deltaMs / 1000.0;
}

static void ensureTimeSync() {
  if (millis() - gLastTimeSyncMillis > 6UL * 60UL * 60UL * 1000UL || gLastSyncedEpoch <= 0.0) {
    syncTimeFromNotecard();
  }
}

static void scheduleNextDailyReport() {
  double epoch = currentEpoch();
  if (epoch <= 0.0) {
    gNextDailyReportEpoch = 0.0;
    return;
  }

  double dayStart = floor(epoch / 86400.0) * 86400.0;
  double scheduled = dayStart + (double)gConfig.reportHour * 3600.0 + (double)gConfig.reportMinute * 60.0;
  if (scheduled <= epoch) {
    scheduled += 86400.0;
  }

  gNextDailyReportEpoch = scheduled;
}

// ============================================================================
// Device Firmware Update (DFU) via Blues Notecard — host-pull MCUboot mode
// Firmware is downloaded by the Notecard, then the host pulls it with dfu.get,
// stages it in the MCUboot secondary slot, and reboots into the trial image.
// ============================================================================

// Firmware length from last dfu.status (needed by enableDfuMode)
static uint32_t gDfuFirmwareLength = 0;
static TankAlarmDfuStatus gDfuStatus;

// Stop a Notehub-pushed DFU we are deliberately refusing (downgrade/equal version
// or a locally blacklisted version) so the Notecard stops advertising mode:"ready"
// and the host does not re-evaluate the same image on every check. Deduped by
// version so a given refused image is stopped/logged only once. Returns true if
// this call actually issued the stop (i.e. the version was not already handled).
static bool stopRefusedFirmware(const char *version, const char *reason) {
  static char lastStopped[24] = {0};
  if (version && strncmp(lastStopped, version, sizeof(lastStopped)) == 0) {
    return false;  // already handled this version
  }
  J *req = notecard.newRequest("dfu.status");
  if (req) {
    JAddBoolToObject(req, "stop", true);
    JAddStringToObject(req, "status", reason);
    JAddStringToObject(req, "name", "user");
    J *rsp = notecard.requestAndResponse(req);
    if (rsp) notecard.deleteResponse(rsp);
  }
  if (version) strlcpy(lastStopped, version, sizeof(lastStopped));
  return true;
}

static void checkForFirmwareUpdate() {
  TankAlarmDfuStatus dfuStatus;
  if (!tankalarm_checkDfuStatus(notecard, dfuStatus)) {
    // Notecard comm failure: clear any stale "update available" so a prior
    // cycle's metadata cannot drive an apply with outdated length/version.
    Serial.println(F("DFU check: dfu.status query failed (Notecard comm) - skipping"));
    gDfuUpdateAvailable = false;
    gDfuFirmwareLength = 0;
    gDfuVersion[0] = '\0';
    return;
  }

  // Track downloading state.
  // The Notecard reports mode="downloading" while it pulls the image into its
  // OWN storage — the host has not begun staging yet. We must NOT set
  // gDfuInProgress here: that flag is owned exclusively by enableDfuMode() to
  // mark an active host staging/apply. Setting it on a transient Notecard
  // download would latch it true forever (both loop() DFU paths only call this
  // function when !gDfuInProgress), permanently suppressing all future checks
  // so the device never sees the eventual mode="ready". Just clear the
  // available flags and return; the next check picks up "ready".
  if (dfuStatus.downloading) {
    Serial.println(F("DFU download in progress (Notecard pulling image)..."));
    gDfuUpdateAvailable = false;
    gDfuFirmwareLength = 0;
    return;
  }

  // Track errors
  if (dfuStatus.error) {
    Serial.print(F("DFU error: "));
    Serial.println(dfuStatus.errorMsg);
    // Clear stale state so the error path cannot leave a previous cycle's
    // "available" flag set and trigger an apply with outdated metadata.
    gDfuUpdateAvailable = false;
    gDfuFirmwareLength = 0;
    gDfuVersion[0] = '\0';
    return;
  }

  // Detect update available
  if (dfuStatus.updateAvailable && dfuStatus.version[0] != '\0') {
    // Check local blacklist before anything else
#if defined(TANKALARM_DFU_MCUBOOT)
    if (tankalarm_isVersionBlacklisted(dfuStatus.version)) {
      if (stopRefusedFirmware(dfuStatus.version, "blacklisted (previous rollback)")) {
        Serial.print(F("DFU: Version "));
        Serial.print(dfuStatus.version);
        Serial.println(F(" is locally blacklisted - stopped."));
      }
      return;
    }

    // Downgrade guard: only auto-apply a STRICTLY NEWER image. Notehub is
    // expected to assign the intended target, but a stale/incorrect assignment
    // (or an older .slot.bin) must never trigger a downgrade-then-rollback loop.
    // Compare offered vs running by monotonic semantic-version sequence (versionToSeq).
    // Must compare against the running FIRMWARE_VERSION's sequence, NOT FIRMWARE_BUILD_SEQ
    // (a separate monotonic build counter that only coincided with versionToSeq through 1.9.x).
    if (tankalarm_versionToSeq(dfuStatus.version) <= tankalarm_versionToSeq(FIRMWARE_VERSION)) {
      if (stopRefusedFirmware(dfuStatus.version, "not newer than running firmware")) {
        Serial.print(F("DFU: Offered v"));
        Serial.print(dfuStatus.version);
        Serial.print(F(" is not newer than running v"));
        Serial.print(F(FIRMWARE_VERSION));
        Serial.println(F(" - ignoring (no downgrade), stopped."));
      }
      gDfuUpdateAvailable = false;
      gDfuVersion[0] = '\0';
      gDfuFirmwareLength = 0;
      return;
    }
#endif

    gDfuStatus = dfuStatus;

    if (!gDfuUpdateAvailable || strcmp(gDfuVersion, dfuStatus.version) != 0) {
      gDfuUpdateAvailable = true;
      gDfuFirmwareLength = dfuStatus.firmwareLength;
      strlcpy(gDfuVersion, dfuStatus.version, sizeof(gDfuVersion));

      Serial.println(F("========================================"));
      Serial.print(F("FIRMWARE UPDATE AVAILABLE: v"));
      Serial.println(gDfuVersion);
      Serial.print(F("Current version: "));
      Serial.println(F(FIRMWARE_VERSION));
      Serial.print(F("Size: "));
      Serial.print(gDfuFirmwareLength);
      Serial.println(F(" bytes"));
      Serial.println(F("Device will auto-update on next check (MCUboot)"));
      Serial.println(F("========================================"));

      addSerialLog("Firmware update available (MCUboot)");
    }
  } else if (gDfuUpdateAvailable) {
    gDfuUpdateAvailable = false;
    gDfuVersion[0] = '\0';
    gDfuFirmwareLength = 0;
    memset(&gDfuStatus, 0, sizeof(gDfuStatus));
  }
}

static void dfuKickWatchdog() {
#ifdef TANKALARM_WATCHDOG_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    mbedWatchdog.kick();
  #else
    IWatchdog.reload();
  #endif
#endif
}

static bool sendOtaReportNote(const char *st, const char *targetV, const char *reason) {
  if (!gNotecardAvailable) return false;
  J *req = notecard.newRequest("note.add");
  if (!req) return false;
  JAddStringToObject(req, "file", CONFIG_ACK_OUTBOX_FILE);
  JAddBoolToObject(req, "sync", true);
  J *body = JCreateObject();
  if (!body) { JDelete(req); return false; }
  JAddStringToObject(body, "c", gDeviceUID);
  JAddStringToObject(body, "st", st);
  if (targetV && targetV[0] != '\0') JAddStringToObject(body, "target_v", targetV);
  JAddStringToObject(body, "from_v", FIRMWARE_VERSION);
  if (reason && reason[0] != '\0') JAddStringToObject(body, "reason", reason);
  JAddNumberToObject(body, "epoch", currentEpoch());
  JAddItemToObject(req, "body", body);
  bool ok = false;
  J *rsp = notecard.requestAndResponse(req);
  if (rsp) {
    const char *err = JGetString(rsp, "err");
    ok = (!err || err[0] == '\0');
    notecard.deleteResponse(rsp);
  }
  Serial.print(F("OTA report ("));
  Serial.print(st);
  Serial.print(F("): "));
  Serial.println(ok ? F("queued") : F("failed"));
  return ok;
}

// Bridge any pending MCUboot OTA outcome (applied/reverted) to the server for dashboard
// visibility. Best-effort, fires once per outcome (deduped on QSPI). Safe no-op if nothing pending.
static void reportOtaOutcome() {
#if defined(TANKALARM_DFU_MCUBOOT)
  TankAlarmOtaReport rep;
  if (!tankalarm_peekOtaReport(rep)) return;
  const bool applied = (strcmp(rep.status, "applied") == 0);
  bool sent = sendOtaReportNote(applied ? "ota-applied" : "ota-reverted", rep.targetV,
                                applied ? nullptr : "trial boot reverted by MCUboot");
  if (sent) {
    tankalarm_markOtaReported(rep.targetV, rep.status);
    addSerialLog(applied ? "OTA applied reported to server" : "OTA reverted reported to server");
  }
#endif
}

static void enableDfuMode() {
  if (gDfuInProgress) {
    Serial.println(F("DFU already in progress"));
    return;
  }
  if (gDfuFirmwareLength == 0) {
    Serial.println(F("ERROR: No firmware length — cannot apply update"));
    return;
  }

  gDfuInProgress = true;
  if (gConfigDirty) {
    persistConfigIfDirty(); // Save state first
  }

  const char *restoreMode = gConfig.solarPowered ? "periodic" : "continuous";

  Serial.println(F("========================================"));
  Serial.println(F("MCUboot DFU: Staging update to QSPI..."));
  Serial.println(F("========================================"));
  addSerialLog("MCUboot staging started");

  // Disable solar manager polling to avoid serial/modbus/I2C contention
  if (gSolarManager.isEnabled()) {
    gSolarManager.end();
  }

#if defined(TANKALARM_DFU_MCUBOOT)
  bool ok = tankalarm_performMcubootUpdate(
      notecard, gDfuStatus, restoreMode, DEVICE_ROLE, dfuKickWatchdog);
#else
  bool ok = false;
  // MCUboot DFU support is not compiled in — stop the pending update so the
  // Client does not repeatedly attempt to apply it on every DFU check cycle.
  {
    J *req = notecard.newRequest("dfu.status");
    if (req) {
      JAddBoolToObject(req, "stop", true);
      JAddStringToObject(req, "status", "MCUboot DFU not supported in this build");
      JAddStringToObject(req, "name", "user");
      J *rsp = notecard.requestAndResponse(req);
      if (rsp) notecard.deleteResponse(rsp);
    }
  }
#endif

  gDfuInProgress = false; // Only reached on failure

  if (!ok) {
    Serial.println(F("MCUboot DFU failed — resuming normal operation"));
    addSerialLog("MCUboot staging failed");
    // Tell the server the staging attempt failed (host never swapped) for dashboard visibility.
    sendOtaReportNote("ota-stage-failed", gDfuStatus.version, "MCUboot staging failed");
    // Clear the pending-update flags so a failed staging attempt (preflight reject,
    // role mismatch, transient error) does not retry the same image every check.
    // A genuinely newer image is re-detected on the next successful check.
    gDfuUpdateAvailable = false;
    gDfuVersion[0] = '\0';
    gDfuFirmwareLength = 0;
    if (gConfig.solarCharger.enabled) {
      gSolarManager.begin(gConfig.solarCharger);
    }
  }
}

static void updateDailyScheduleIfNeeded() {
  if (gNextDailyReportEpoch <= 0.0 && gLastSyncedEpoch > 0.0) {
    scheduleNextDailyReport();
  }
}

static bool sendConfigAck(bool success, const char *message, const char *configVersion) {
  J *req = notecard.newRequest("note.add");
  if (!req) {
    return false;
  }
  JAddStringToObject(req, "file", CONFIG_ACK_OUTBOX_FILE);
  JAddBoolToObject(req, "sync", true);

  J *body = JCreateObject();
  if (!body) {
    JDelete(req);
    return false;
  }

  JAddStringToObject(body, "c", gDeviceUID);
  JAddStringToObject(body, "st", success ? "applied" : "failed");
  if (configVersion && configVersion[0] != '\0') {
    JAddStringToObject(body, "cv", configVersion);
  }
  if (message) {
    JAddStringToObject(body, "message", message);
  }
  JAddNumberToObject(body, "epoch", currentEpoch());
  JAddItemToObject(req, "body", body);

  bool ackQueued = false;
  J *rsp = notecard.requestAndResponse(req);
  if (rsp) {
    const char *err = JGetString(rsp, "err");
    if (err && err[0] != '\0') {
      Serial.print(F("WARNING: Config ACK failed to queue: "));
      Serial.println(err);
    } else {
      Serial.print(F("Config ACK sent: "));
      Serial.println(success ? F("applied") : F("failed"));
      ackQueued = true;
    }
    notecard.deleteResponse(rsp);
  } else {
    Serial.println(F("WARNING: Config ACK no response from Notecard"));
  }
  return ackQueued;
}

static void pollForConfigUpdates() {
  // Skip if notecard is known to be offline
  if (!gNotecardAvailable) {
    unsigned long now = millis();
    if (now - gLastSuccessfulNotecardComm > NOTECARD_RETRY_INTERVAL) {
      // Periodically retry to see if notecard is back
      checkNotecardHealth();
    }
    return;
  }

  J *req = notecard.newRequest("note.get");
  if (!req) {
    gNotecardFailureCount++;
    return;
  }

  JAddStringToObject(req, "file", CONFIG_INBOX_FILE);
  // Peek without deleting — delete after successful processing for crash safety

  J *rsp = notecard.requestAndResponse(req);
  if (!rsp) {
    gNotecardFailureCount++;
    if (gNotecardFailureCount >= NOTECARD_FAILURE_THRESHOLD) {
      checkNotecardHealth();
    }
    return;
  }

  gLastSuccessfulNotecardComm = millis();
  gNotecardFailureCount = 0;

  J *body = JGetObject(rsp, "body");
  if (body) {
    bool ackSent = false;
    char *json = JConvertToJSONString(body);
    if (json) {
      std::unique_ptr<JsonDocument> docPtr(new JsonDocument());
      if (docPtr) {
        JsonDocument &doc = *docPtr;
        DeserializationError err = deserializeJson(doc, json);
        NoteFree(json);
        if (!err) {
          // Extract config version hash for ACK tracking (injected by server)
          const char *cv = doc["_cv"] | "";
          uint32_t inboundTs = doc["_ts"] | (uint32_t)0;
          
          if (inboundTs > 0 && inboundTs <= gConfig.configEpoch) {
            Serial.print(F("Ignoring obsolete queued config (newer version already active). Inbound TS: "));
            Serial.print(inboundTs);
            Serial.print(F(" Active TS: "));
            Serial.println(gConfig.configEpoch);
            // Send config ACK indicating that we took it but bypassed it to prevent overwriting
            sendConfigAck(true, "Config bypassed: newer version already exists", cv);
            ackSent = true;
          } else {
            if (inboundTs > 0) {
              gConfig.configEpoch = inboundTs;
            }
            applyConfigUpdate(doc);
            // BugFix v1.6.2 (I-11): Defer ACK until persistence succeeds.
            // Store the config version and mark pending — persistConfigIfDirty()
            // will send the ACK after saveConfigToFlash() completes.
            strlcpy(gPendingConfigAckVersion, cv, sizeof(gPendingConfigAckVersion));
            gPendingConfigAck = true;
            gPendingConfigAckSuccess = true;
            strlcpy(gPendingConfigAckMessage, "Config applied and persisted", sizeof(gPendingConfigAckMessage));
            gPendingConfigAckRetryAt = 0;
            gPendingConfigAckRetryDelayMs = 5000UL;
            gConfigDirty = true;
            // Delete the inbound note now — the config is in memory and will be
            // persisted shortly. If persistence fails, a failure ACK is sent.
            ackSent = true;
          }
        } else {
          Serial.println(F("Config update invalid JSON"));
          sendConfigAck(false, "Invalid JSON", nullptr);
          ackSent = true;  // Delete note even on parse failure (note is useless)
        }
      } else {
        NoteFree(json);
        Serial.println(F("OOM processing config update"));
      }
    }
    // Only consume the note if ACK was sent (or note was unparseable).
    // If ACK failed, keep the note so it is retried on the next poll cycle.
    if (ackSent) {
      J *delReq = notecard.newRequest("note.get");
      if (delReq) {
        JAddStringToObject(delReq, "file", CONFIG_INBOX_FILE);
        JAddBoolToObject(delReq, "delete", true);
        J *delRsp = notecard.requestAndResponse(delReq);
        if (delRsp) notecard.deleteResponse(delRsp);
      }
    } else {
      Serial.println(F("Config ACK failed — keeping config.qi for retry"));
    }
  }

  notecard.deleteResponse(rsp);
}

static void reinitializeHardware() {
  // Reinitialize I2C bus in case of configuration changes or bus issues
  Wire.end();
  delay(10);
  Wire.begin();
  Wire.setTimeout(I2C_WIRE_TIMEOUT_MS);  // Guard against indefinite blocking on bus hang

  // Reinitialize all monitor sensors with new configuration
  for (uint8_t i = 0; i < gConfig.monitorCount; ++i) {
    const MonitorConfig &cfg = gConfig.monitors[i];
    
    // Configure digital pins if needed
    if (cfg.sensorInterface == SENSOR_DIGITAL) {
      if (cfg.primaryPin >= 0 && cfg.primaryPin < 255) {
        pinMode(cfg.primaryPin, INPUT_PULLUP);
      }
      if (cfg.secondaryPin >= 0 && cfg.secondaryPin < 255) {
        pinMode(cfg.secondaryPin, INPUT_PULLUP);
      }
    }
    
    // If sensor was in a failed/stuck state, notify server before clearing
    if (gMonitorState[i].sensorFailed) {
      Serial.print(F("Sending sensor-recovered for sensor "));
      Serial.println(cfg.name);
      if (checkAlarmRateLimit(i, "sensor-recovered")) {
        JsonDocument recovDoc;
        recovDoc["c"] = gDeviceUID;
        recovDoc["s"] = gConfig.siteName;
        recovDoc["k"] = cfg.sensorIndex;
        recovDoc["y"] = "sensor-recovered";
        recovDoc["rd"] = 0;
        recovDoc["t"] = currentEpoch();
        publishNote(ALARM_FILE, recovDoc, true);
      }
    }

    // If alarm was latched, send clear to server before resetting
    if (gMonitorState[i].highAlarmLatched || gMonitorState[i].lowAlarmLatched) {
      Serial.print(F("Clearing latched alarm for sensor "));
      Serial.println(cfg.name);
      JsonDocument clearDoc;
      clearDoc["c"] = gDeviceUID;
      clearDoc["s"] = gConfig.siteName;
      clearDoc["k"] = cfg.sensorIndex;
      clearDoc["y"] = "clear";
      clearDoc["rd"] = 0;
      clearDoc["t"] = currentEpoch();
      publishNote(ALARM_FILE, clearDoc, true);
    }

    // Reset monitor runtime state for hardware changes
    gMonitorState[i].highAlarmDebounceCount = 0;
    gMonitorState[i].lowAlarmDebounceCount = 0;
    gMonitorState[i].highClearDebounceCount = 0;
    gMonitorState[i].lowClearDebounceCount = 0;
    gMonitorState[i].highAlarmLatched = false;
    gMonitorState[i].lowAlarmLatched = false;
    gMonitorState[i].consecutiveFailures = 0;
    gMonitorState[i].stuckReadingCount = 0;
    gMonitorState[i].sensorFailed = false;
    gMonitorState[i].lastValidReading = 0.0f;
    gMonitorState[i].hasLastValidReading = false;
    gMonitorState[i].lastReportedValue = -9999.0f;
  }
  
  // Re-attach Notecard after Wire reinit
  tankalarm_ensureNotecardBinding(notecard);

  // Reconfigure Notecard hub settings (may have changed due to power mode)
  configureNotecardHubMode();
  
  Serial.println(F("Hardware reinitialized after config update"));
}

static void resetTelemetryBaselines() {
  for (uint8_t i = 0; i < MAX_MONITORS; ++i) {
    gMonitorState[i].lastReportedValue = -9999.0f;
  }
}

static void clearPendingConfigAckState() {
  gPendingConfigAck = false;
  gPendingConfigAckVersion[0] = '\0';
  gPendingConfigAckSuccess = true;
  strlcpy(gPendingConfigAckMessage, "Config applied and persisted", sizeof(gPendingConfigAckMessage));
  gPendingConfigAckRetryAt = 0;
  gPendingConfigAckRetryDelayMs = 5000UL;
}

static bool isPendingConfigAckRetryDue() {
  if (gPendingConfigAckRetryAt == 0) {
    return true;
  }
  return (long)(millis() - gPendingConfigAckRetryAt) >= 0;
}

static void schedulePendingConfigAckRetry() {
  unsigned long delayMs = gPendingConfigAckRetryDelayMs;
  if (delayMs < 5000UL) {
    delayMs = 5000UL;
  }
  gPendingConfigAckRetryAt = millis() + delayMs;
  if (gPendingConfigAckRetryDelayMs < 60000UL) {
    gPendingConfigAckRetryDelayMs *= 2;
    if (gPendingConfigAckRetryDelayMs > 60000UL) {
      gPendingConfigAckRetryDelayMs = 60000UL;
    }
  }
}

static void attemptPendingConfigAckSend() {
  if (!gPendingConfigAck || !isPendingConfigAckRetryDue()) {
    return;
  }
  if (sendConfigAck(gPendingConfigAckSuccess, gPendingConfigAckMessage, gPendingConfigAckVersion)) {
    clearPendingConfigAckState();
  } else {
    schedulePendingConfigAckRetry();
  }
}

static void applyConfigUpdate(const JsonDocument &doc) {
  bool hardwareChanged = false;
  
  if (!doc["site"].isNull()) {
    strlcpy(gConfig.siteName, doc["site"].as<const char *>(), sizeof(gConfig.siteName));
  }
  if (!doc["deviceLabel"].isNull()) {
    strlcpy(gConfig.deviceLabel, doc["deviceLabel"].as<const char *>(), sizeof(gConfig.deviceLabel));
  }
  if (!doc["serverFleet"].isNull()) {
    strlcpy(gConfig.serverFleet, doc["serverFleet"].as<const char *>(), sizeof(gConfig.serverFleet));
  }
  if (!doc["clientFleet"].isNull()) {
    const char *newFleet = doc["clientFleet"].as<const char *>();
    if (newFleet && strcmp(gConfig.clientFleet, newFleet) != 0) {
      strlcpy(gConfig.clientFleet, newFleet, sizeof(gConfig.clientFleet));
      hardwareChanged = true;  // Fleet change requires Notecard hub.set reconfiguration
    }
  }
  // Product UID: allow server to push a Notehub product UID to unconfigured devices
  if (!doc["productUid"].isNull()) {
    const char *newPuid = doc["productUid"].as<const char *>();
    if (newPuid && strcmp(gConfig.productUid, newPuid) != 0) {
      strlcpy(gConfig.productUid, newPuid, sizeof(gConfig.productUid));
      hardwareChanged = true;  // Product UID change requires Notecard hub.set reconfiguration
    }
  }
  if (!doc["sampleSeconds"].isNull()) {
    gConfig.sampleSeconds = doc["sampleSeconds"].as<uint16_t>();
  }
  if (!doc["reportHour"].isNull()) {
    gConfig.reportHour = doc["reportHour"].as<uint8_t>();
  }
  if (!doc["reportMinute"].isNull()) {
    gConfig.reportMinute = doc["reportMinute"].as<uint8_t>();
  }
  if (!doc["dailyEmail"].isNull()) {
    strlcpy(gConfig.dailyEmail, doc["dailyEmail"].as<const char *>(), sizeof(gConfig.dailyEmail));
  }
  
  // Handle clear button configuration
  if (!doc["clearButtonPin"].isNull()) {
    int8_t newPin = doc["clearButtonPin"].as<int8_t>();
    if (newPin != gConfig.clearButtonPin) {
      gConfig.clearButtonPin = newPin;
      hardwareChanged = true;  // Need to reinitialize button pin
    }
  }
  if (!doc["clearButtonActiveHigh"].isNull()) {
    gConfig.clearButtonActiveHigh = doc["clearButtonActiveHigh"].as<bool>();
  }
  
  // Handle power saving configuration
  if (!doc["solarPowered"].isNull()) {
    bool newSolarPowered = doc["solarPowered"].as<bool>();
    if (newSolarPowered != gConfig.solarPowered) {
      gConfig.solarPowered = newSolarPowered;
      hardwareChanged = true;  // Need to reconfigure Notecard hub settings
    }
  }

  // Handle battery configuration (chemistry + nominal pack voltage).
  // Mirrors the loadConfigFromFlash() parser: when present, this overrides
  // any prior thresholds via initBatteryConfig() so chemistry changes pushed
  // from the server take effect without requiring a reboot.
  if (!doc["batteryConfig"].isNull()) {    JsonObjectConst batCfg = doc["batteryConfig"].as<JsonObjectConst>();
    bool batEnabled = batCfg["enabled"].is<bool>() ? batCfg["enabled"].as<bool>() : true;
    BatteryType bt = batCfg["batteryType"].is<int>()
      ? (BatteryType)batCfg["batteryType"].as<int>()
      : gConfig.batteryMonitor.batteryType;
    uint8_t nominalV = batCfg["nominalVoltage"].is<int>()
      ? (uint8_t)batCfg["nominalVoltage"].as<int>()
      : gConfig.batteryMonitor.nominalVoltage;
    BatteryType prevType = gConfig.batteryMonitor.batteryType;
    uint8_t prevNominal  = gConfig.batteryMonitor.nominalVoltage;
    bool prevEnabled     = gConfig.batteryMonitor.enabled;
    initBatteryConfig(&gConfig.batteryMonitor, bt, nominalV);
    gConfig.batteryMonitor.enabled = batEnabled && (bt != BATTERY_TYPE_NONE);
    if (prevType != bt || prevNominal != nominalV || prevEnabled != gConfig.batteryMonitor.enabled) {
      Serial.print(F("Battery config updated: type="));
      Serial.print(batteryTypeLabel(bt));
      Serial.print(F(" nominalV="));
      Serial.print(nominalV);
      Serial.print(F(" enabled="));
      Serial.println(gConfig.batteryMonitor.enabled ? F("yes") : F("no"));
      // Re-arm the chemistry cross-check so the next solar poll re-verifies
      // against the new selection.
      if (gSolarManager.isEnabled()) {
        gChemistryChecked = false;
        Serial.println(F("Solar: retrace chemistry verification on next poll"));
      }
    }
  }

  // Handle solar charger (SunSaver MPPT via RS-485) configuration update.
  // I-24 fix (v1.6.13): mirror the loadConfigFromFlash()/saveConfigToFlash()
  // schema so server-pushed solarCharger changes actually take effect at
  // runtime instead of being silently dropped and overwritten on next save.
  // If the enabled flag or transport parameters change, restart the
  // SolarManager so the RS-485 transport picks up the new settings.
  if (!doc["solarCharger"].isNull()) {
    JsonObjectConst solarCfg = doc["solarCharger"].as<JsonObjectConst>();
    bool prevEnabled  = gConfig.solarCharger.enabled;
    uint8_t prevSlave = gConfig.solarCharger.modbusSlaveId;
    uint16_t prevBaud = gConfig.solarCharger.modbusBaudRate;
    uint16_t prevTo   = gConfig.solarCharger.modbusTimeoutMs;

    if (solarCfg["enabled"].is<bool>())            gConfig.solarCharger.enabled = solarCfg["enabled"].as<bool>();
    if (solarCfg["slaveId"].is<int>())             gConfig.solarCharger.modbusSlaveId = (uint8_t)solarCfg["slaveId"].as<int>();
    if (solarCfg["baudRate"].is<int>())            gConfig.solarCharger.modbusBaudRate = (uint16_t)solarCfg["baudRate"].as<int>();
    if (solarCfg["timeoutMs"].is<int>())           gConfig.solarCharger.modbusTimeoutMs = (uint16_t)solarCfg["timeoutMs"].as<int>();
    if (solarCfg["pollIntervalSec"].is<int>())     gConfig.solarCharger.pollIntervalSec = (uint16_t)solarCfg["pollIntervalSec"].as<int>();
    if (solarCfg["batteryLowV"].is<float>())       gConfig.solarCharger.batteryLowVoltage = solarCfg["batteryLowV"].as<float>();
    if (solarCfg["batteryCriticalV"].is<float>())  gConfig.solarCharger.batteryCriticalVoltage = solarCfg["batteryCriticalV"].as<float>();
    if (solarCfg["batteryHighV"].is<float>())      gConfig.solarCharger.batteryHighVoltage = solarCfg["batteryHighV"].as<float>();
    if (solarCfg["alertOnLow"].is<bool>())         gConfig.solarCharger.alertOnLowBattery = solarCfg["alertOnLow"].as<bool>();
    if (solarCfg["alertOnFault"].is<bool>())       gConfig.solarCharger.alertOnFault = solarCfg["alertOnFault"].as<bool>();
    if (solarCfg["alertOnCommFail"].is<bool>())    gConfig.solarCharger.alertOnCommFailure = solarCfg["alertOnCommFail"].as<bool>();
    if (solarCfg["includeInDaily"].is<bool>())     gConfig.solarCharger.includeInDailyReport = solarCfg["includeInDaily"].as<bool>();

    bool transportChanged = (prevSlave != gConfig.solarCharger.modbusSlaveId) ||
                            (prevBaud  != gConfig.solarCharger.modbusBaudRate) ||
                            (prevTo    != gConfig.solarCharger.modbusTimeoutMs);

    // Sanitize and clamp configuration parameters (Task 1.3)
    sanitizeSolarConfig(gConfig.solarCharger);

    if (gConfig.solarCharger.enabled && (!prevEnabled || transportChanged)) {
      // (Re)initialize transport with the new config.
      gSolarManager.end();
      if (gSolarManager.begin(gConfig.solarCharger)) {
        Serial.println(F("Solar charger reinitialized via config update"));
        addSerialLog("Solar charger reinitialized via config update");
      } else {
        Serial.println(F("Solar charger reinit FAILED via config update"));
        addSerialLog("Solar charger reinit failed via config update");
      }
    } else if (!gConfig.solarCharger.enabled && prevEnabled) {
      gSolarManager.end();
      Serial.println(F("Solar charger disabled via config update"));
      addSerialLog("Solar charger disabled via config update");
    } else if (gConfig.solarCharger.enabled && prevEnabled && !transportChanged) {
      // Non-transport change: refresh thresholds/flags directly (Task 1.1)
      uint16_t prevPoll = gSolarManager.getConfig().pollIntervalSec;
      gSolarManager.setConfig(gConfig.solarCharger);
      if (prevPoll != gConfig.solarCharger.pollIntervalSec) {
        gSolarManager.forcePollSoon();
      }
      Serial.println(F("Solar: runtime config refreshed (no transport restart)"));
    }
  }

  // Handle Vin voltage divider configuration
  if (!doc["vinMonitor"].isNull()) {
    JsonObjectConst vinCfg = doc["vinMonitor"].as<JsonObjectConst>();
    bool wasEnabled = gConfig.vinMonitor.enabled;
    gConfig.vinMonitor.enabled = vinCfg["enabled"].is<bool>() ? vinCfg["enabled"].as<bool>() : false;
    if (vinCfg["pin"].is<int>()) {
      uint8_t newPin = (uint8_t)vinCfg["pin"].as<int>();
      if (newPin <= 7) gConfig.vinMonitor.analogPin = newPin;
    }
    if (vinCfg["r1Kohm"].is<float>() && vinCfg["r1Kohm"].as<float>() > 0.0f) {
      gConfig.vinMonitor.r1Kohm = vinCfg["r1Kohm"].as<float>();
    }
    if (vinCfg["r2Kohm"].is<float>() && vinCfg["r2Kohm"].as<float>() > 0.0f) {
      gConfig.vinMonitor.r2Kohm = vinCfg["r2Kohm"].as<float>();
    }
    if (vinCfg["pollIntervalSec"].is<int>()) {
      gConfig.vinMonitor.pollIntervalSec = (uint16_t)vinCfg["pollIntervalSec"].as<int>();
    }
    if (gConfig.vinMonitor.enabled && !wasEnabled) {
      gLastVinPollMillis = 0;  // Force immediate first read
      gVinVoltage = 0.0f;
      Serial.println(F("Vin monitor enabled"));
    } else if (!gConfig.vinMonitor.enabled && wasEnabled) {
      gVinVoltage = 0.0f;
      Serial.println(F("Vin monitor disabled"));
    }
  }

  // Handle Solar-Only (No Battery) configuration update
  if (!doc["solarOnlyConfig"].isNull()) {
    JsonObjectConst soCfg = doc["solarOnlyConfig"].as<JsonObjectConst>();
    bool wasEnabled = gConfig.solarOnlyConfig.enabled;
    gConfig.solarOnlyConfig.enabled = soCfg["enabled"].is<bool>() ? soCfg["enabled"].as<bool>() : false;
    if (soCfg["startupDebounceVoltage"].is<float>()) gConfig.solarOnlyConfig.startupDebounceVoltage = soCfg["startupDebounceVoltage"].as<float>();
    if (soCfg["startupDebounceSec"].is<int>()) gConfig.solarOnlyConfig.startupDebounceSec = (uint16_t)soCfg["startupDebounceSec"].as<int>();
    if (soCfg["startupWarmupSec"].is<int>()) gConfig.solarOnlyConfig.startupWarmupSec = (uint16_t)soCfg["startupWarmupSec"].as<int>();
    if (soCfg["sensorGateVoltage"].is<float>()) gConfig.solarOnlyConfig.sensorGateVoltage = soCfg["sensorGateVoltage"].as<float>();
    if (soCfg["sunsetVoltage"].is<float>()) gConfig.solarOnlyConfig.sunsetVoltage = soCfg["sunsetVoltage"].as<float>();
    if (soCfg["sunsetConfirmSec"].is<int>()) gConfig.solarOnlyConfig.sunsetConfirmSec = (uint16_t)soCfg["sunsetConfirmSec"].as<int>();
    if (soCfg["opportunisticReportHours"].is<int>()) gConfig.solarOnlyConfig.opportunisticReportHours = (uint16_t)soCfg["opportunisticReportHours"].as<int>();
    if (soCfg["batteryFailureFallback"].is<bool>()) gConfig.solarOnlyConfig.batteryFailureFallback = soCfg["batteryFailureFallback"].as<bool>();
    if (soCfg["batteryFailureThreshold"].is<int>()) gConfig.solarOnlyConfig.batteryFailureThreshold = (uint8_t)soCfg["batteryFailureThreshold"].as<int>();
    if (gConfig.solarOnlyConfig.enabled && !wasEnabled) {
      // Device is already running with stable power — skip startup debounce.
      // performStartupDebounce() is only called from setup() on boot.
      gSolarOnlyStartupComplete = true;
      gSolarOnlySensorsReady = true;
      gSolarOnlySunsetActive = false;
      gSolarOnlyStateSaved = false;
      loadSolarStateFromFlash();
      Serial.println(F("Solar-only mode enabled (runtime)"));
      addSerialLog("Solar-only mode enabled via config update");
    } else if (!gConfig.solarOnlyConfig.enabled && wasEnabled) {
      gSolarOnlyStartupComplete = true;
      gSolarOnlySensorsReady = true;
      gSolarOnlyBatteryFailed = false;
      gSolarOnlyBatFailCount = 0;
      Serial.println(F("Solar-only mode disabled"));
      addSerialLog("Solar-only mode disabled via config update");
    }
  }

  // Handle remote-tunable power conservation thresholds
  if (!doc["powerEcoEnterV"].isNull()) gConfig.powerEcoEnterV = doc["powerEcoEnterV"].as<float>();
  if (!doc["powerLowEnterV"].isNull()) gConfig.powerLowEnterV = doc["powerLowEnterV"].as<float>();
  if (!doc["powerCriticalEnterV"].isNull()) gConfig.powerCriticalEnterV = doc["powerCriticalEnterV"].as<float>();
  if (!doc["powerEcoExitV"].isNull()) gConfig.powerEcoExitV = doc["powerEcoExitV"].as<float>();
  if (!doc["powerLowExitV"].isNull()) gConfig.powerLowExitV = doc["powerLowExitV"].as<float>();
  if (!doc["powerCriticalExitV"].isNull()) gConfig.powerCriticalExitV = doc["powerCriticalExitV"].as<float>();

  // Validate hysteresis: exit voltage must be greater than enter voltage for each tier
  if (gConfig.powerEcoExitV > 0.0f && gConfig.powerEcoEnterV > 0.0f &&
      gConfig.powerEcoExitV <= gConfig.powerEcoEnterV) {
    gConfig.powerEcoExitV = gConfig.powerEcoEnterV + 0.2f;
    Serial.println(F("WARNING: powerEcoExitV <= powerEcoEnterV — corrected (+0.2V)"));
  }
  if (gConfig.powerLowExitV > 0.0f && gConfig.powerLowEnterV > 0.0f &&
      gConfig.powerLowExitV <= gConfig.powerLowEnterV) {
    gConfig.powerLowExitV = gConfig.powerLowEnterV + 0.2f;
    Serial.println(F("WARNING: powerLowExitV <= powerLowEnterV — corrected (+0.2V)"));
  }
  if (gConfig.powerCriticalExitV > 0.0f && gConfig.powerCriticalEnterV > 0.0f &&
      gConfig.powerCriticalExitV <= gConfig.powerCriticalEnterV) {
    gConfig.powerCriticalExitV = gConfig.powerCriticalEnterV + 0.2f;
    Serial.println(F("WARNING: powerCriticalExitV <= powerCriticalEnterV — corrected (+0.2V)"));
  }

  // Handle remote-tunable health check interval
  if (!doc["healthCheckBaseIntervalMs"].isNull()) {
    gConfig.healthCheckBaseIntervalMs = (uint32_t)doc["healthCheckBaseIntervalMs"].as<int>();
  }

  if (!doc["sensors"].isNull()) {
    hardwareChanged = true;  // Monitor configuration affects hardware
    JsonArrayConst sensors = doc["sensors"].as<JsonArrayConst>();
    uint8_t newCount = min<uint8_t>(sensors.size(), MAX_MONITORS);

    // Clean up state for monitors being removed (count decreasing)
    for (uint8_t i = newCount; i < gConfig.monitorCount; ++i) {
      // Send recovery/clear notes for removed monitors with active alarms
      if (gMonitorState[i].sensorFailed) {
        JsonDocument recovDoc;
        recovDoc["c"] = gDeviceUID;
        recovDoc["s"] = gConfig.siteName;
        recovDoc["k"] = gConfig.monitors[i].sensorIndex;
        recovDoc["y"] = "sensor-recovered";
        recovDoc["rd"] = 0;
        recovDoc["t"] = currentEpoch();
        publishNote(ALARM_FILE, recovDoc, true);
      }
      if (gMonitorState[i].highAlarmLatched || gMonitorState[i].lowAlarmLatched) {
        JsonDocument clearDoc;
        clearDoc["c"] = gDeviceUID;
        clearDoc["s"] = gConfig.siteName;
        clearDoc["k"] = gConfig.monitors[i].sensorIndex;
        clearDoc["y"] = "clear";
        clearDoc["rd"] = 0;
        clearDoc["t"] = currentEpoch();
        publishNote(ALARM_FILE, clearDoc, true);
      }
      // Zero out stale runtime state so it's clean if monitor is re-added later
      memset(&gMonitorState[i], 0, sizeof(MonitorRuntime));
    }

    gConfig.monitorCount = newCount;
    for (uint8_t i = 0; i < gConfig.monitorCount; ++i) {
      // Capture previous state before config update
      bool wasAlarmsEnabled = gConfig.monitors[i].alarmsEnabled;
      bool wasStuckEnabled = gConfig.monitors[i].stuckDetectionEnabled;
      char wasRelayTarget[48];
      strlcpy(wasRelayTarget, gConfig.monitors[i].relayTargetClient, sizeof(wasRelayTarget));
      uint8_t wasRelayMask = gConfig.monitors[i].relayMask;

      JsonObjectConst t = sensors[i];
      parseMonitorFromJson(gConfig.monitors[i], t, i);

      // BugFix 04022026: When alarmsEnabled transitions from true to false,
      // clear any latched alarm state and send a "clear" note to the server.
      // Previously, stale highAlarmLatched/lowAlarmLatched persisted and were
      // reported in daily reports, causing phantom alarms on the dashboard.
      if (wasAlarmsEnabled && !gConfig.monitors[i].alarmsEnabled) {
        if (gMonitorState[i].highAlarmLatched || gMonitorState[i].lowAlarmLatched) {
          gMonitorState[i].highAlarmLatched = false;
          gMonitorState[i].lowAlarmLatched = false;
          gMonitorState[i].highAlarmDebounceCount = 0;
          gMonitorState[i].lowAlarmDebounceCount = 0;
          gMonitorState[i].highClearDebounceCount = 0;
          gMonitorState[i].lowClearDebounceCount = 0;
          // Send explicit clear so server clears its alarmActive flag
          sendAlarm(i, "clear", gMonitorState[i].currentInches);
          // Deactivate any local alarm (relay/buzzer)
          activateLocalAlarm(i, false);
        }
      }

      // BugFix 04022026: When stuckDetectionEnabled transitions true -> false,
      // clear sensorFailed and related state. Previously, a sensor marked as
      // failed would remain suppressed (no alarms, no telemetry) until reboot.
      if (wasStuckEnabled && !gConfig.monitors[i].stuckDetectionEnabled) {
        if (gMonitorState[i].sensorFailed) {
          // Send recovery note so server clears its sensor-stuck alarm
          JsonDocument recovDoc;
          recovDoc["c"] = gDeviceUID;
          recovDoc["s"] = gConfig.siteName;
          recovDoc["k"] = gConfig.monitors[i].sensorIndex;
          recovDoc["y"] = "sensor-recovered";
          recovDoc["rd"] = 0;
          recovDoc["t"] = currentEpoch();
          publishNote(ALARM_FILE, recovDoc, true);
        }
        gMonitorState[i].sensorFailed = false;
        gMonitorState[i].stuckReadingCount = 0;
        gMonitorState[i].consecutiveFailures = 0;
        gMonitorState[i].hasLastValidReading = false;
        gMonitorState[i].recoveryCount = 0;
      }

      // BugFix 04022026: When relay config is removed from an existing monitor
      // (relayMask set to 0 or relayTargetClient cleared), deactivate active
      // relays that were energized under the old config. Previously, relay
      // runtime state persisted and relays stayed energized indefinitely.
      if (isMonitorRelayActive(i)) {
        bool relayRemoved = (gConfig.monitors[i].relayMask == 0) ||
                            (gConfig.monitors[i].relayTargetClient[0] == '\0');
        bool relayTargetChanged = (wasRelayTarget[0] != '\0') &&
                                  (strcmp(wasRelayTarget, gConfig.monitors[i].relayTargetClient) != 0);
        bool relayMaskChanged = (wasRelayMask != 0) && (wasRelayMask != gConfig.monitors[i].relayMask);
        if (relayRemoved || relayTargetChanged || relayMaskChanged) {
          // Deactivate relays using the OLD config (where the target/mask was)
          if (wasRelayTarget[0] != '\0' && wasRelayMask != 0) {
            triggerRemoteRelays(wasRelayTarget, wasRelayMask, false);
          }
          uint8_t activeMask = getMonitorActiveRelayMask(i);
          deactivateRelayForMonitor(i, activeMask);
          Serial.print(F("Relay deactivated: config removed/changed for monitor "));
          Serial.println(i);
        }
      }

      // Side effect: reset accumulated state when pulse mode config changes
      if (!t["pulseAccumulatedMode"].isNull() || !t["rpmAccumulatedMode"].isNull()) {
        atomicResetPulses(i);
        gRpmAccumulatedStartMillis[i] = millis();
        gRpmAccumulatedInitialized[i] = false;
      }
      // BugFix 04022026: Reset unload tracking unconditionally when the field
      // is present, not only when enabled. Previously, disabling trackUnloads
      // left stale peak/epoch state that could cause false unload events if
      // the feature was later re-enabled.
      if (!t["trackUnloads"].isNull()) {
        gMonitorState[i].unloadTracking = false;
        gMonitorState[i].unloadPeakInches = 0.0f;
        gMonitorState[i].unloadPeakEpoch = 0.0;
      }
    }
  }

  gConfigDirty = true;
  
  if (hardwareChanged) {
    reinitializeHardware();
    // Hardware change invalidates old sensor baselines — force fresh readings
    resetTelemetryBaselines();
    // Reset power state so voltage thresholds are re-evaluated cleanly
    gPowerState = POWER_STATE_NORMAL;
    gPowerStateDebounce = 0;
  }
  
  printHardwareRequirements(gConfig);
  scheduleNextDailyReport();
  Serial.println(F("Configuration updated from server"));
  addSerialLog("Config updated from server");
}

static void persistConfigIfDirty() {
  if (!gConfigDirty) {
    return;
  }

#ifdef FILESYSTEM_AVAILABLE
  static bool warned = false;
  if (!isStorageAvailable()) {
    if (!warned) {
      Serial.println(F("Warning: Filesystem unavailable - skipping config persistence"));
      warned = true;
    }
    return;
  }
#endif

  if (saveConfigToFlash(gConfig)) {
    gConfigDirty = false;
    // BugFix v1.6.2 (I-11): Now that config is safely on flash, send the deferred ACK.
    if (gPendingConfigAck) {
      gPendingConfigAckSuccess = true;
      strlcpy(gPendingConfigAckMessage, "Config applied and persisted", sizeof(gPendingConfigAckMessage));
      attemptPendingConfigAckSend();
    }
  } else {
    // Persistence failed — notify server so it can flag the issue.
    if (gPendingConfigAck) {
      gPendingConfigAckSuccess = false;
      strlcpy(gPendingConfigAckMessage, "Flash persistence failed", sizeof(gPendingConfigAckMessage));
      attemptPendingConfigAckSend();
    }
  }
}

static void retryPendingConfigAckIfDue() {
  if (!gPendingConfigAck || gConfigDirty) {
    return;
  }
  attemptPendingConfigAckSend();
}

/**
 * Attempt to recover a hung I2C bus.
 * Thin wrapper around tankalarm_recoverI2CBus() from TankAlarm_I2C.h,
 * providing Client-specific DFU guard and watchdog kick.
 */
static void recoverI2CBus() {
#ifdef TANKALARM_WATCHDOG_AVAILABLE
  // Provide a watchdog-kick callback for the shared recovery function
  auto kickWd = []() {
    #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
      mbedWatchdog.kick();
    #else
      IWatchdog.reload();
    #endif
  };
  tankalarm_recoverI2CBus(gDfuInProgress, kickWd);
#else
  tankalarm_recoverI2CBus(gDfuInProgress);
#endif
}

/**
 * Publish an I2C recovery diagnostic event via Notecard.
 *
 * Sends a lightweight "diag.qo" note containing trigger type, cumulative
 * recovery count, current I2C error count, and timestamp.  Only publishes
 * when the Notecard is reachable (SENSOR_ONLY trigger); for NOTECARD_FAILURE
 * and DUAL_FAILURE triggers the Notecard is down, so the event is recorded
 * only in the daily counter and Serial log.
 *
 * Rate-limited to at most one note per 60 seconds to prevent flooding
 * during rapid-fire recovery cycles.
 */
static void logI2CRecoveryEvent(I2CRecoveryTrigger trigger) {
  // Rate limit: at most one diagnostic note per 60 seconds
  static unsigned long lastDiagNoteMillis = 0;
  const unsigned long DIAG_RATE_LIMIT_MS = 60000UL;
  unsigned long now = millis();

  if (!gNotecardAvailable || (now - lastDiagNoteMillis < DIAG_RATE_LIMIT_MS && lastDiagNoteMillis != 0)) {
    // Can't publish (Notecard down) or rate-limited — just log to Serial
    Serial.print(F("I2C recovery event (trigger="));
    Serial.print((uint8_t)trigger);
    Serial.print(F(", count="));
    Serial.print(gI2cBusRecoveryCount);
    Serial.println(F(") — not published"));
    return;
  }

  lastDiagNoteMillis = now;

  JsonDocument doc;
  doc["ev"] = "i2c-recovery";
  doc["trigger"] = (uint8_t)trigger;
  doc["count"] = gI2cBusRecoveryCount;
  doc["i2c_errs"] = gCurrentLoopI2cErrors;
  doc["cl_ok"] = gCurrentLoopReadsOk;
  doc["cl_fault"] = gLastClFaultReason;
  doc["cl_dur_us"] = gLastClBurstMicros;
  double epoch = currentEpoch();
  if (epoch > 0) {
    doc["t"] = epoch;
  }

  publishNote(DIAG_FILE, doc, false);  // false = don't force immediate sync
  Serial.print(F("I2C recovery event published (trigger="));
  Serial.print((uint8_t)trigger);
  Serial.println(F(")"));
}

/**
 * Read a 4-20mA current loop value from the A0602 expansion module.
 * Thin wrapper around tankalarm_readCurrentLoopMilliamps() from TankAlarm_I2C.h,
 * using the runtime-configurable I2C address from gConfig.
 */
static float readCurrentLoopMilliamps(int16_t channel) {
  // Use runtime-configurable I2C address (falls back to compile-time default)
  uint8_t i2cAddr = gConfig.currentLoopI2cAddress;
  if (i2cAddr < 0x08 || i2cAddr > 0x77 || i2cAddr == NOTECARD_I2C_ADDRESS) {
    i2cAddr = CURRENT_LOOP_I2C_ADDRESS;
  }
  return tankalarm_readCurrentLoopMilliamps(channel, i2cAddr);
}

static float linearMap(float value, float inMin, float inMax, float outMin, float outMax) {
  if (fabs(inMax - inMin) < 0.0001f) {
    return outMin;
  }
  float pct = (value - inMin) / (inMax - inMin);
  pct = constrain(pct, 0.0f, 1.0f);
  return outMin + pct * (outMax - outMin);
}

static bool validateSensorReading(uint8_t idx, float reading) {
  if (idx >= gConfig.monitorCount) {
    return false;
  }

  // Reject non-finite readings (NaN/inf) up front. A NaN compares false against every
  // bound below, so without this gate a faulted reading (e.g. an under-range current loop
  // returning NAN, or a degenerate linearMap) would slip through as "valid".
  if (!isfinite(reading)) {
    MonitorRuntime &nfState = gMonitorState[idx];
    nfState.consecutiveFailures++;
    if (nfState.consecutiveFailures >= SENSOR_FAILURE_THRESHOLD && !nfState.sensorFailed) {
      nfState.sensorFailed = true;
      Serial.print(F("Non-finite sensor reading for monitor "));
      Serial.println(gConfig.monitors[idx].name);
      if (checkAlarmRateLimit(idx, "sensor-fault")) {
        JsonDocument doc;
        doc["c"] = gDeviceUID;
        doc["s"] = gConfig.siteName;
        doc["k"] = gConfig.monitors[idx].sensorIndex;
        doc["y"] = "sensor-fault";
        doc["t"] = currentEpoch();
        publishNote(ALARM_FILE, doc, true);
      }
    }
    return false;
  }

  const MonitorConfig &cfg = gConfig.monitors[idx];
  MonitorRuntime &state = gMonitorState[idx];

  // Calculate valid range based on sensor type
  float minValid;
  float maxValid;
  
  // Check if sensor has native range configured (current loop or analog with voltage range)
  bool hasNativeRange = (cfg.sensorRangeMax > cfg.sensorRangeMin);
  bool isCurrentLoop = (cfg.sensorInterface == SENSOR_CURRENT_LOOP);
  bool isAnalogWithVoltageRange = (cfg.sensorInterface == SENSOR_ANALOG && 
                                    cfg.analogVoltageMax > cfg.analogVoltageMin &&
                                    hasNativeRange);
  
  if ((isCurrentLoop || isAnalogWithVoltageRange) && hasNativeRange) {
    // For sensors with native range, calculate max from sensor range
    if (cfg.objectType == OBJECT_GAS) {
      // Gas pressure: bound is just the raw range (no inches conversion / mount add).
      maxValid = cfg.sensorRangeMax * 1.1f;
    } else if (isCurrentLoop && cfg.currentLoopType == CURRENT_LOOP_ULTRASONIC) {
      // Ultrasonic: max level is sensorMountHeight (when tank is full)
      maxValid = cfg.sensorMountHeight * 1.1f;
    } else {
      // Pressure-driven liquid level: include SG so bounds match the actual fluid column.
      float conversionFactor = getPressureConversionFactorByName(cfg.sensorRangeUnit);
      float sg = getEffectiveSpecificGravity(cfg);
      maxValid = ((cfg.sensorRangeMax * conversionFactor / sg) + cfg.sensorMountHeight) * 1.1f;
    }
    minValid = -maxValid * 0.1f;
  } else if (cfg.sensorInterface == SENSOR_DIGITAL) {
    // Digital sensors have simple 0/1 values
    minValid = -0.5f;
    maxValid = 1.5f;
  } else {
    // For RPM and other sensors without native range, use alarm thresholds as reference
    maxValid = cfg.highAlarmThreshold * 2.0f; // Allow up to 2x high alarm as valid
    minValid = -maxValid * 0.1f;
  }
  
  if (reading < minValid || reading > maxValid) {
    state.consecutiveFailures++;
    if (state.consecutiveFailures >= SENSOR_FAILURE_THRESHOLD) {
      if (!state.sensorFailed) {
        state.sensorFailed = true;
        Serial.print(F("Sensor failure detected for monitor "));
        Serial.println(cfg.name);
        // Send sensor failure alert with rate limiting
        if (checkAlarmRateLimit(idx, "sensor-fault")) {
          JsonDocument doc;
          doc["c"] = gDeviceUID;
          doc["s"] = gConfig.siteName;
          doc["k"] = cfg.sensorIndex;
          doc["y"] = "sensor-fault";
          doc["rd"] = reading;
          doc["t"] = currentEpoch();
          publishNote(ALARM_FILE, doc, true);
        }
      }
    }
    return false;
  }

  // Check for stuck sensor (same reading multiple times)
  // BugFix v1.6.2 (M-12): Exempt monitors actively tracking an unload — a slowly
  // emptying tank legitimately produces near-identical readings that would
  // otherwise trip the stuck detector.
  bool exemptFromStuck = cfg.trackUnloads && state.unloadTracking;
  if (cfg.stuckDetectionEnabled && !exemptFromStuck && state.hasLastValidReading && fabs(reading - state.lastValidReading) < 0.05f) {
    state.stuckReadingCount++;
    if (state.stuckReadingCount >= SENSOR_STUCK_THRESHOLD) {
      if (!state.sensorFailed) {
        state.sensorFailed = true;
        Serial.print(F("Stuck sensor detected for monitor "));
        Serial.println(cfg.name);
        // Send stuck sensor alert with rate limiting
        if (checkAlarmRateLimit(idx, "sensor-stuck")) {
          JsonDocument doc;
          doc["c"] = gDeviceUID;
          doc["s"] = gConfig.siteName;
          doc["k"] = cfg.sensorIndex;
          doc["y"] = "sensor-stuck";
          doc["rd"] = reading;
          doc["t"] = currentEpoch();
          publishNote(ALARM_FILE, doc, true);
        }
      }
      return false;
    }
  } else {
    state.stuckReadingCount = 0;
  }

  // Reading is valid - reset failure counters
  state.consecutiveFailures = 0;
  if (state.sensorFailed) {
    state.recoveryCount++;
    // Require 3 consecutive good readings before declaring recovered
    if (state.recoveryCount >= 3) {
      state.sensorFailed = false;
      state.recoveryCount = 0;
      Serial.print(F("Sensor recovered for monitor "));
      Serial.println(cfg.name);
      // Send recovery notification (rate-limited)
      if (checkAlarmRateLimit(idx, "sensor-recovered")) {
        JsonDocument doc;
        doc["c"] = gDeviceUID;
        doc["s"] = gConfig.siteName;
        doc["k"] = cfg.sensorIndex;
        doc["y"] = "sensor-recovered";
        doc["rd"] = reading;
        doc["t"] = currentEpoch();
        publishNote(ALARM_FILE, doc, true);
      }
    }
  }
  state.lastValidReading = reading;
  state.hasLastValidReading = true;
  return true;
}

// ============================================================================
// Sensor Type Helper Functions (extracted from readMonitorSensor for clarity)
// ============================================================================

/**
 * Read a digital (float switch) sensor.
 * The digitalSwitchMode field controls how the hardware is interpreted:
 * - "NO" (normally-open): Switch is open by default, closes when fluid is present
 *   - With INPUT_PULLUP: HIGH = switch open (no fluid), LOW = switch closed (fluid present)
 *   - activated when pin is LOW
 * - "NC" (normally-closed): Switch is closed by default, opens when fluid is present
 *   - With INPUT_PULLUP: LOW = switch closed (no fluid), HIGH = switch open (fluid present)
 *   - activated when pin is HIGH
 * Returns DIGITAL_SENSOR_ACTIVATED_VALUE or DIGITAL_SENSOR_NOT_ACTIVATED_VALUE.
 */
static float readDigitalSensor(const MonitorConfig &cfg, uint8_t idx) {
  int pin = (cfg.primaryPin >= 0 && cfg.primaryPin < 255) ? cfg.primaryPin : (2 + idx);
  pinMode(pin, INPUT_PULLUP);
  int level = digitalRead(pin);

  bool isNormallyClosed = (strcmp(cfg.digitalSwitchMode, "NC") == 0);

  // For NO switches: LOW = activated (switch closed, fluid present)
  // For NC switches: HIGH = activated (switch opened, fluid present)
  bool isActivated;
  if (isNormallyClosed) {
    isActivated = (level == HIGH); // NC switch opens (goes HIGH) when activated
  } else {
    isActivated = (level == LOW);  // NO switch closes (goes LOW) when activated
  }

  return isActivated ? DIGITAL_SENSOR_ACTIVATED_VALUE : DIGITAL_SENSOR_NOT_ACTIVATED_VALUE;
}

/**
 * Read an analog voltage sensor (e.g., Dwyer 626 with 0-10V, 1-5V, 0-5V output).
 * Uses pressure-to-height conversion based on sensor native range.
 *
 * Configuration:
 * - analogVoltageMin/Max: Voltage output range (e.g., 0-10V, 1-5V)
 * - sensorRangeMin/Max: Pressure range in sensorRangeUnit (e.g., 0-5 PSI)
 * - sensorMountHeight: Height of sensor above tank bottom (inches)
 *
 * Returns liquid height in inches.
 */
static float readAnalogSensor(const MonitorConfig &cfg, uint8_t idx) {
  // Use explicit bounds check for channel (A0602 has channels 0-7)
  int channel = (cfg.primaryPin >= 0 && cfg.primaryPin < 8) ? cfg.primaryPin : 0;

  // Validate that we have a valid sensor range configured
  if (cfg.sensorRangeMax <= cfg.sensorRangeMin || cfg.analogVoltageMax <= cfg.analogVoltageMin) {
    return NAN; // Invalid configuration — fault rather than report a plausible-but-fake 0
  }

  // Read voltage (Opta A0602 analog inputs: 0-10V mapped to 0-4095)
  float total = 0.0f;
  const uint8_t samples = 8;
  for (uint8_t s = 0; s < samples; ++s) {
    int raw = analogRead(channel);
    total += (float)raw / 4095.0f * 10.0f; // Convert to 0-10V
    delay(2);
  }
  float voltage = total / samples;

  // Store raw voltage reading for telemetry
  gMonitorState[idx].currentSensorVoltage = voltage;

  // Live-zero fault guard (Fix 2): a sensor with an elevated minimum output voltage
  // (e.g. a 1-5V transducer) reading far below that minimum is unpowered or disconnected.
  // Return NAN so validateSensorReading() escalates a sensor-fault instead of letting
  // linearMap() produce a negative value that is then silently clamped to a fabricated 0.0.
  if (cfg.analogVoltageMin >= 0.5f && voltage < cfg.analogVoltageMin * 0.2f) {
    return NAN;
  }

  // Map voltage to sensor's native pressure units
  float pressure = linearMap(voltage, cfg.analogVoltageMin, cfg.analogVoltageMax,
                             cfg.sensorRangeMin, cfg.sensorRangeMax);

  // Gas pressure monitors: report the raw pressure in its native unit (no level conversion).
  if (cfg.objectType == OBJECT_GAS) {
    if (pressure < 0.0f) pressure = 0.0f;
    return pressure;
  }

  // Convert pressure to liquid height in inches using fluid SG (defaults to water=1.0).
  // inches_of_fluid = (pressure * 27.68 in_H2O/PSI) / SG
  float conversionFactor = getPressureConversionFactorByName(cfg.sensorRangeUnit);
  float sg = getEffectiveSpecificGravity(cfg);
  float liquidAboveSensor = (pressure * conversionFactor) / sg;

  // Total height from tank bottom = liquid above sensor + sensor mount height
  float levelInches = liquidAboveSensor + cfg.sensorMountHeight;

  // Clamp: minimum is 0 (empty tank)
  if (levelInches < 0.0f) levelInches = 0.0f;

  return levelInches;
}

/**
 * Read a 4-20mA current loop sensor (pressure or ultrasonic).
 * Handles both pressure sensors (mounted near bottom) and ultrasonic sensors
 * (mounted on top) via the currentLoopType configuration.
 * Returns liquid height in inches.
 */
static float readCurrentLoopSensor(const MonitorConfig &cfg, uint8_t idx) {
  // Use explicit bounds check for current loop channel
  int16_t channel = (cfg.currentLoopChannel >= 0 && cfg.currentLoopChannel < 8) ? cfg.currentLoopChannel : 0;
  // Validate that we have a valid sensor range configured
  if (cfg.sensorRangeMax <= cfg.sensorRangeMin) {
    gMonitorState[idx].currentSensorMa = 0.0f;
    return NAN; // Invalid configuration — fault rather than report a plausible-but-fake 0
  }

  // Resolve current-loop expansion module address
  uint8_t i2cAddr = gConfig.currentLoopI2cAddress;
  if (i2cAddr < 0x08 || i2cAddr > 0x77 || i2cAddr == NOTECARD_I2C_ADDRESS) {
    i2cAddr = CURRENT_LOOP_I2C_ADDRESS;
  }

  // Enable solid-state power gating by pulling physical terminal HIGH (transistor ON).
  // Fix C2 (v2.0.50): PWM enable runs at the normal 100 kHz bus speed BEFORE the A0602
  // burst window opens. This keeps high-speed transactions tightly scoped around the
  // actual A0602 framed reads (§7.3, §8.2.1).
  if (cfg.pwmGatingEnabled) {
    // The I2C ACK only confirms the command was received, not that the rail came up.
    // Retry the enable a couple of times so a transient bus NACK doesn't leave the
    // transmitter unpowered (which would read as an under-range / sensor fault).
    bool pwmOnSuccess = false;
    for (uint8_t attempt = 0; attempt < 3 && !pwmOnSuccess; ++attempt) {
      if (attempt > 0) delay(5);
      pwmOnSuccess = tankalarm_setPwm(cfg.pwmGatingChannel, 10000, 9999, i2cAddr);
    }
    if (pwmOnSuccess) {
      gMonitorState[idx].lastPwmEnableOk = true;
    } else {
      Serial.print(F("WARNING: Failed to enable sensor power gating on P"));
      Serial.print(cfg.pwmGatingChannel + 1);
      Serial.println(F(" via I2C"));
      // Safety (v1.9.22): when the P1 high-side switch fails to enable, the transmitter is
      // UNPOWERED, so reading the channel now would capture a floating/stale value (a
      // plausible-but-false reading such as ~18mA / 43.8psi). Do NOT sample. Record the failed
      // enable, drive P1 off defensively, and return a sensor fault so validateSensorReading()
      // escalates instead of publishing a fabricated pressure.
      gMonitorState[idx].lastPwmEnableOk = false;
      gMonitorState[idx].currentSensorMa = 0.0f;
      gMonitorState[idx].sampleReused = true;
      gLastClFaultReason = CL_FAULT_PWM_NACK;
      (void)tankalarm_setPwm(cfg.pwmGatingChannel, 0, 0, i2cAddr);
      return NAN;
    }
  }

  // A0602 Blueprint transactions run in OWNED, scoped windows on the shared bus (v2.0.46):
  //  - a SHORT byte-timeout (the A0602 doesn't clock-stretch, so fail fast into loop() recovery);
  //  - the per-op 400 kHz clock to shrink the A0602's bus occupancy ~4x.
  // BOTH are RESTORED before every early return and around the warmup gap, so neither ever
  // leaks to a Notecard operation on the shared bus.
  //
  // Fix C2 (v2.0.50): the burst window is now SPLIT into two phases (§7.3 / §10.4):
  //   1. CONFIGURE burst: open -> configure ADC channel + verify -> close (back to 100 kHz)
  //   2. (warmup delay, no I/O activity, normal bus clock)
  //   3. SAMPLE burst:    open -> priming read + N samples -> close (back to 100 kHz)
  //   4. PWM disable runs at normal 100 kHz bus speed.
  // gLastClBurstMicros accumulates active burst time only (§9-2): configWindowUs + sampleWindowUs.

  // ---- CONFIGURE burst window ----
  const uint32_t configStartUs = micros();
  Wire.setTimeout(A0602_WIRE_TIMEOUT_MS);
  Wire.setClock(A0602_PEROP_I2C_CLOCK_HZ);

  // Configure the A0602 channel as a 4-20mA current ADC via the framed Blueprint protocol
  // BEFORE the warmup (Fix C2, v2.0.50). In the power-gated model the channel loses its
  // config when P1 is switched off, so we (re)configure here on every powered read. Putting
  // the configure BEFORE the warmup means the AD74412R sense node is connected to the I/O
  // pin during the entire stabilization window, so the transmitter's loop current can settle
  // electrically while the rail is being brought up. Without this, the channel sits in
  // high-impedance during warmup and the first priming read captures a freshly-configured
  // sense node that hasn't completed its electrical settling.
  // v2.0.45: the SET ACK only means "queued"; CONFIRM the channel actually entered
  // current-ADC mode via GET_CHANNEL_FUNCTION (0x40) before trusting a reading. Retry config 3x.
  // GRACEFUL gate: hard-fault (NAN) only if the SET is NACKed every attempt OR the expansion
  // POSITIVELY reports a non-current-ADC function. If the A0602 firmware never answers 0x40,
  // fall through to the read (the framed GET ADC has its own CRC/channel guard) so we never
  // brick a sensor whose expansion firmware predates GET_CHANNEL_FUNCTION.
  bool adcConfigOk = false;
  bool funcReadable = false;
  bool funcVerified = false;
  for (uint8_t attempt = 0; attempt < 3 && !funcVerified; ++attempt) {
    if (attempt > 0) delay(10);
    if (!tankalarm_configureCurrentAdcChannel((uint8_t)channel, i2cAddr)) {
      continue;  // SET NACK -- retry
    }
    adcConfigOk = true;
    unsigned long fstart = millis();
    while ((millis() - fstart) < 100) {
      uint8_t fun = 0xFF;
      if (tankalarm_getAnalogChannelFunction((uint8_t)channel, i2cAddr, fun)) {
        funcReadable = true;
        if (fun == TANKALARM_OA_FUNC_CURRENT_INPUT_EXT_POWER) { funcVerified = true; break; }
      }
      delay(5);
    }
  }

  // ---- Close CONFIGURE burst window (Fix C2: restore normal bus settings BEFORE any
  //      cleanup commands or warmup, so PWM-off in the early-return paths runs at 100 kHz
  //      and the warmup delay doesn't sit inside the high-speed window). §8.2.2 / §9-1.
  Wire.setClock(I2C_NORMAL_CLOCK_HZ);
  Wire.setTimeout(I2C_WIRE_TIMEOUT_MS);
  const uint32_t configWindowUs = micros() - configStartUs;

  // Early-return paths (Fix C2, §9-1): preserve the FULL existing side-effect set
  // (counter, fault reason, raw mA clear, sampleReused, PWM-off) so fleet telemetry
  // (cl_fault / i2c_cl_err) and the dual/sensor-only recovery escalation logic continue
  // to see the same signal as before C2.
  if (!adcConfigOk) {
    Serial.print(F("ERROR: A0602 current-ADC channel config NACK on ch "));
    Serial.println(channel);
    gCurrentLoopI2cErrors++;
    gLastClFaultReason = CL_FAULT_CONFIG_NACK;
    gMonitorState[idx].currentSensorMa = 0.0f;
    gMonitorState[idx].sampleReused = true;
    if (cfg.pwmGatingEnabled) {
      // PWM-off at normal 100 kHz bus speed (burst window already closed above).
      bool pwmOffOk = false;
      for (uint8_t a = 0; a < 3 && !pwmOffOk; ++a) {
        if (a > 0) delay(5);
        pwmOffOk = tankalarm_setPwm(cfg.pwmGatingChannel, 0, 0, i2cAddr);
      }
    }
    gLastClBurstMicros = configWindowUs;  // §9-2: only the active CONFIGURE window
    return NAN;
  }
  if (funcReadable && !funcVerified) {
    // We positively read the channel function and it is NOT current-ADC: a real
    // misconfiguration. Do not trust the reading.
    Serial.print(F("ERROR: A0602 ch "));
    Serial.print(channel);
    Serial.println(F(" not in current-ADC mode after config; rejecting read"));
    gCurrentLoopI2cErrors++;
    gLastClFaultReason = CL_FAULT_FUNC_WRONG;
    gMonitorState[idx].currentSensorMa = 0.0f;
    gMonitorState[idx].sampleReused = true;
    if (cfg.pwmGatingEnabled) {
      bool pwmOffOk = false;
      for (uint8_t a = 0; a < 3 && !pwmOffOk; ++a) {
        if (a > 0) delay(5);
        pwmOffOk = tankalarm_setPwm(cfg.pwmGatingChannel, 0, 0, i2cAddr);
      }
    }
    gLastClBurstMicros = configWindowUs;  // §9-2
    return NAN;
  }

  // ---- Warmup delay (Fix C2: now AFTER configure, so the AD74412R sense node is connected
  //      and the transmitter can drive loop current into a real sense resistor during the
  //      entire stabilization window). No I/O activity here; bus is at normal 100 kHz so any
  //      hypothetical interleaved transaction would be Notecard-safe — though the loop is
  //      synchronous and nothing else can interleave anyway. §6.4 / §10.4.
  if (cfg.pwmGatingEnabled) {
    // Feed the watchdog across the (multi-second) warmup so several current-loop
    // monitors read sequentially in one loop() pass can't starve the watchdog.
    // Chunk the stabilization delay and kick between chunks.
    uint32_t remaining = cfg.pwmGatingWarmup;
    const uint32_t chunk = 1000; // 1s slices keep us well inside the WDT window
    while (remaining > 0) {
      uint32_t slice = (remaining > chunk) ? chunk : remaining;
      delay(slice);
      remaining -= slice;
#ifdef TANKALARM_WATCHDOG_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
      mbedWatchdog.kick();
  #else
      IWatchdog.reload();
  #endif
#endif
    }
  }

  // ---- SAMPLE burst window (Fix C2): re-open the 400 kHz / short-timeout window for the
  //      priming read and the N sample reads. PWM disable runs AFTER the window closes.
  const uint32_t sampleStartUs = micros();
  Wire.setTimeout(A0602_WIRE_TIMEOUT_MS);
  Wire.setClock(A0602_PEROP_I2C_CLOCK_HZ);

#ifdef TANKALARM_WATCHDOG_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    mbedWatchdog.kick();
  #else
    IWatchdog.reload();
  #endif
#endif

  // Inter-sample settle. The A0602 current-loop ADC needs time to deliver a fresh conversion
  // after the channel is selected. The validated standalone gating test (P1_Transistor_Gating_Test)
  // sampled at 300ms spacing and read the transmitter correctly; the small production default
  // (e.g. 5ms) is too aggressive and can return a stale, constant high value that looks like a
  // pegged reading. When gating is enabled, floor the settle to the proven cadence regardless of
  // the configured pwmGatingSampleDelay.
  const uint32_t sampleSettleMs = cfg.pwmGatingEnabled
      ? (((uint32_t)cfg.pwmGatingSampleDelay > (uint32_t)CURRENT_LOOP_GATED_SETTLE_MS)
            ? (uint32_t)cfg.pwmGatingSampleDelay : (uint32_t)CURRENT_LOOP_GATED_SETTLE_MS)
      : 5UL;

  // Priming read (gated sensors only): take one framed conversion and discard it, then settle,
  // so the averaged samples below reflect the freshly-powered+configured transmitter rather than
  // a stale pre-warmup ADC value. This mirrors the proven P1_Transistor_Gating_Test sequence.
  if (cfg.pwmGatingEnabled) {
    (void)tankalarm_readCurrentAdcFramed((uint8_t)channel, i2cAddr);
    delay(sampleSettleMs);
#ifdef TANKALARM_WATCHDOG_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    mbedWatchdog.kick();
  #else
    IWatchdog.reload();
  #endif
#endif
  }

  // BugFix v1.6.2 (M-1): Multi-sample averaging for current-loop sensors.
  // I2C reads are slower than ADC, so we use 4 samples (vs 8 for analog).
  const uint8_t numSamples = 4;
  float total = 0.0f;
  uint8_t validSamples = 0;
  for (uint8_t s = 0; s < numSamples; ++s) {
    float sample = tankalarm_readCurrentAdcFramed((uint8_t)channel, i2cAddr);
    if (sample >= 0.0f) {
      total += sample;
      validSamples++;
    }
    if (s < numSamples - 1) {
      delay(sampleSettleMs);
#ifdef TANKALARM_WATCHDOG_AVAILABLE
      // Settle can total ~1s across samples when gating is on; kick the WDT.
      if (cfg.pwmGatingEnabled) {
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
        mbedWatchdog.kick();
  #else
        IWatchdog.reload();
  #endif
      }
#endif
    }
  }

  // ---- Close SAMPLE burst window (Fix C2: restore normal bus settings BEFORE the PWM-off
  //      command so the high-speed / short-timeout window is tightly scoped). §8.2.1.
  Wire.setClock(I2C_NORMAL_CLOCK_HZ);
  Wire.setTimeout(I2C_WIRE_TIMEOUT_MS);
  const uint32_t sampleWindowUs = micros() - sampleStartUs;

  // Turn off sensor power gating once readings complete to achieve low-power gating (transistor OFF).
  // v2.0.45: retry OFF (mirrors the ON retry) so a transient bus NACK can't leave the transmitter
  // powered, which would distort the solar power budget and future observations.
  // Fix C2 (v2.0.50): PWM-off now runs at normal 100 kHz bus speed (burst window closed above).
  if (cfg.pwmGatingEnabled) {
    bool pwmOffSuccess = false;
    for (uint8_t attempt = 0; attempt < 3 && !pwmOffSuccess; ++attempt) {
      if (attempt > 0) delay(5);
      pwmOffSuccess = tankalarm_setPwm(cfg.pwmGatingChannel, 0, 0, i2cAddr);
    }
    if (!pwmOffSuccess) {
      Serial.print(F("WARNING: Failed to disable sensor power gating on P"));
      Serial.print(cfg.pwmGatingChannel + 1);
      Serial.println(F(" via I2C"));
      gCurrentLoopI2cErrors++;
    }
  }

  // Fix C2 (§9-2): record the active A0602 burst time (CONFIGURE + SAMPLE windows only);
  // the warmup gap between them is EXCLUDED so historical cl_dur_us comparisons stay meaningful.
  gLastClBurstMicros = configWindowUs + sampleWindowUs;

  float milliamps;
  if (validSamples == 0) {
    // Total acquisition failure (every I2C sample failed). Returning the previous level here
    // would mask a disconnected/unpowered transmitter as healthy data. Clear the raw mA so
    // no stale value is transmitted, and return NAN so validateSensorReading() escalates a
    // sensor-fault after the failure threshold (sampleMonitors() still reuses the last level
    // for display continuity). v2.0.45: count framed-path total failures into the fleet
    // diagnostic so a silently-failing A0602 is visible in i2c_cl_err / i2c-error-rate.
    gCurrentLoopI2cErrors++;
    gLastClFaultReason = CL_FAULT_READ_FAIL;
    gMonitorState[idx].currentSensorMa = 0.0f;
    gMonitorState[idx].sampleReused = true;
    return NAN;
  }
  milliamps = total / validSamples;

  // Store raw mA reading for telemetry
  gMonitorState[idx].currentSensorMa = milliamps;

  // Over-range fault (v2.0.46): a 4-20mA transmitter cannot legitimately exceed 20mA full scale.
  // A value above CURRENT_LOOP_OVER_RANGE_MA (e.g. a raw 0xFFFF read scaling to 25mA, or a near-
  // full-scale garbage frame that passed CRC) is an I2C/loop fault. Reject it rather than let
  // linearMap() extrapolate a fabricated level/pressure. Clear the raw mA so it isn't transmitted.
  if (milliamps > CURRENT_LOOP_OVER_RANGE_MA) {
    Serial.print(F("ERROR: A0602 over-range read "));
    Serial.print(milliamps, 2);
    Serial.println(F("mA (>full-scale) — rejecting as fault"));
    gCurrentLoopI2cErrors++;
    gCurrentLoopOverRange++;
    gLastClFaultReason = CL_FAULT_OVER_RANGE;
    gMonitorState[idx].currentSensorMa = 0.0f;
    gMonitorState[idx].sampleReused = true;
    return NAN;
  }

  // Live-zero fault: a 4-20mA loop reading below ~3.6mA means the loop is open/broken or the
  // transmitter is unpowered. Return NAN so validateSensorReading() escalates a sensor-fault
  // instead of reporting a plausible-but-wrong level. This guard sits BEFORE the calibration
  // branch so it protects both the learned-calibration and theoretical paths (a learned fit
  // would otherwise resolve 0mA to calOffset, a healthy-looking static depth).
  //
  // This applies to gas pressure monitors too: a healthy 4-20mA transmitter sitting at true
  // zero pressure still sources 4mA (the live zero), so an under-range reading is ALWAYS a
  // loop fault, never a legitimate 0 PSI. Without this gate an unpowered/open gas loop would
  // read ~0mA and be reported as a real 0.0 reading instead of a sensor fault.
  if (milliamps < CURRENT_LOOP_FAULT_MA) {
    return NAN;
  }

  // Valid in-range A0602 reading (v2.0.46 observability): count success + clear the fault code.
  gCurrentLoopReadsOk++;
  gLastClFaultReason = CL_FAULT_NONE;

  // Server-pushed learned calibration overrides the theoretical conversion so the client's
  // level (and alarm thresholds) match the server's calibrated display. Skips gas, which
  // reports raw pressure rather than a fluid level.
  if (cfg.hasLearnedCalibration && cfg.objectType != OBJECT_GAS) {
    float level = cfg.calSlope * milliamps + cfg.calOffset;
    if (cfg.calTempCoef != 0.0f) {
      level += cfg.calTempCoef * (cfg.calTempF - 70.0f);
    }
    if (level < 0.0f) level = 0.0f;
    return level;
  }

  // Handle different 4-20mA sensor types using native sensor range
  float levelInches;
  if (cfg.currentLoopType == CURRENT_LOOP_ULTRASONIC) {
    // Ultrasonic sensor mounted on TOP of tank (e.g., Siemens Sitrans LU240)
    // 4mA = minimum distance (sensorRangeMin), 20mA = maximum distance (sensorRangeMax)
    // sensorMountHeight = distance from sensor to tank bottom when empty

    float distanceNative = linearMap(milliamps, 4.0f, 20.0f,
                                     cfg.sensorRangeMin, cfg.sensorRangeMax);
    float distanceInches = distanceNative * getDistanceConversionFactorByName(cfg.sensorRangeUnit);

    // Calculate liquid level: tank height - distance from sensor to surface
    levelInches = cfg.sensorMountHeight - distanceInches;
    if (levelInches < 0.0f) levelInches = 0.0f;
  } else {
    // Pressure sensor mounted near BOTTOM of tank (e.g., Dwyer 626-06-CB-P1-E5-S1)
    // 4mA = sensorRangeMin (e.g., 0 PSI), 20mA = sensorRangeMax (e.g., 5 PSI)
    // sensorMountHeight = height of sensor above tank bottom (usually 0-2 inches)

    float pressure = linearMap(milliamps, 4.0f, 20.0f,
                               cfg.sensorRangeMin, cfg.sensorRangeMax);

    // Gas pressure monitors: report the raw pressure in its native unit. No PSI->inches
    // conversion, no mount-height add — gas pressure isn't a fluid column.
    if (cfg.objectType == OBJECT_GAS) {
      if (pressure < 0.0f) pressure = 0.0f;
      return pressure;
    }

    // Liquid level: divide by fluid specific gravity so we don't assume water.
    // inches_of_fluid = (pressure_PSI * 27.68 in_H2O/PSI) / SG
    float conversionFactor = getPressureConversionFactorByName(cfg.sensorRangeUnit);
    float sg = getEffectiveSpecificGravity(cfg);
    float liquidAboveSensor = (pressure * conversionFactor) / sg;

    // Total height from tank bottom = liquid above sensor + sensor mount height
    levelInches = liquidAboveSensor + cfg.sensorMountHeight;
    if (levelInches < 0.0f) levelInches = 0.0f;
  }
  return levelInches;
}

/**
 * Read a pulse/RPM sensor via cooperative state machine.
 * Each call does a short polling burst then returns.
 * When the configured sample duration elapses, the result is finalized.
 * Between samples, the last computed reading is returned immediately.
 */
static float readPulseSensor(const MonitorConfig &cfg, uint8_t idx) {
  initPulseSamplers();

  PulseSamplerContext &pctx = gPulseSampler[idx];

  // Start a new sample if idle
  if (pctx.state == PULSE_STATE_IDLE) {
    startPulseSample(idx, cfg);
  }

  // Poll the sampler (non-blocking burst)
  pollPulseSampler(idx);

  // Return the best available reading
  return readPulseSensorResult(idx);
}

// ============================================================================
// readMonitorSensor — dispatches to sensor-type-specific helpers
// ============================================================================

static float readMonitorSensor(uint8_t idx) {
  if (idx >= gConfig.monitorCount) {
    return 0.0f;
  }

  // Default to fresh each cycle; specific sensor paths mark reuse when needed.
  gMonitorState[idx].sampleReused = false;

  const MonitorConfig &cfg = gConfig.monitors[idx];

  switch (cfg.sensorInterface) {
    case SENSOR_DIGITAL:      return readDigitalSensor(cfg, idx);
    case SENSOR_ANALOG:       return readAnalogSensor(cfg, idx);
    case SENSOR_CURRENT_LOOP: return readCurrentLoopSensor(cfg, idx);
    case SENSOR_PULSE:        return readPulseSensor(cfg, idx);
    default:                  return NAN;  // Unknown/invalid interface — treat as a fault, not a 0 reading
  }
}

// True if any configured monitor uses the 4-20mA current-loop interface (A0602). Used to
// decide whether to defer Notecard outbox trimming until AFTER sampling, so the heavy Notecard
// I2C traffic in trimTelemetryOutbox() does not share the bus with the timing-sensitive A0602
// transactions (the Blueprint protocol assumes a sole bus master).
static bool hasCurrentLoopMonitor() {
  for (uint8_t i = 0; i < gConfig.monitorCount; ++i) {
    if (gConfig.monitors[i].sensorInterface == SENSOR_CURRENT_LOOP) {
      return true;
    }
  }
  return false;
}

static void sampleMonitors() {
  // Solar-only sensor voltage gating: skip sampling if power is insufficient
  if (isSolarOnlyActive() && !isSensorVoltageGateOpen()) {
    return;  // Voltage too low for reliable sensor readings
  }

  // v2.0.46: TWO-PHASE sampling pass to guarantee single-owner bus access for the timing-
  // sensitive A0602 (current-loop) reads. The Notecard is an I2C slave, but every
  // publishNote()/trim is a HOST-initiated transaction that can hold the shared Wire bus for up
  // to ~30s; running one between two A0602 frames corrupts the framed read. So on current-loop
  // devices we (A) acquire + validate ALL sensors with NO Notecard I/O, then (B) evaluate
  // alarms/unloads and publish telemetry AFTER every A0602 read is complete. Devices with no
  // current-loop monitor keep the original interleaved behaviour (no isolation needed).
  const bool deferPublishes = hasCurrentLoopMonitor();
  if (!deferPublishes) {
    trimTelemetryOutbox();
  } else {
    // Drain any stale bytes a prior Notecard transaction may have left in the Wire RX buffer
    // so they cannot be misread as the first bytes of an A0602 answer frame.
    while (Wire.available()) { (void)Wire.read(); }
  }

  // ---- Phase A: acquire + validate every sensor (NO Notecard publishes when deferring) ----
  for (uint8_t i = 0; i < gConfig.monitorCount; ++i) {
    const double sampleEpoch = currentEpoch();
    float inches = readMonitorSensor(i);

    // Validate sensor reading
    if (!validateSensorReading(i, inches)) {
      // Keep previous valid reading if sensor failed
      inches = gMonitorState[i].currentInches;
      gMonitorState[i].sampleReused = true;
    } else {
      gMonitorState[i].currentInches = inches;
      if (!gMonitorState[i].sampleReused) {
        gMonitorState[i].lastReadingEpoch = sampleEpoch;
      }
    }

    if (!deferPublishes) {
      // No current-loop monitor present: keep the original interleaved evaluate + publish.
      evaluateAlarms(i);
      if (gConfig.monitors[i].trackUnloads && !gMonitorState[i].sensorFailed) {
        evaluateUnload(i);
      }
      if (gConfig.monitors[i].enableServerUpload && !gMonitorState[i].sensorFailed) {
        const float threshold = gConfig.monitors[i].reportThreshold;
        const bool needBaseline = (gMonitorState[i].lastReportedValue < 0.0f);
        const bool thresholdEnabled = (threshold > 0.0f);
        const bool changeExceeded = thresholdEnabled && (fabs(inches - gMonitorState[i].lastReportedValue) >= threshold);
        if (needBaseline || changeExceeded) {
          sendTelemetry(i, "sample", false);
          gMonitorState[i].lastReportedValue = inches;
        }
      }
    }
  }

  // Non-current-loop devices are fully handled in Phase A above.
  if (!deferPublishes) {
    return;
  }

  // ---- Between phases: all A0602 reads are done. Drain any RX residue before Notecard I/O. ----
  while (Wire.available()) { (void)Wire.read(); }

  // ---- Phase B: evaluate alarms/unloads and publish telemetry (the deferred Notecard I/O) ----
  for (uint8_t i = 0; i < gConfig.monitorCount; ++i) {
    const float inches = gMonitorState[i].currentInches;
    evaluateAlarms(i);

    // Evaluate tank unload if tracking is enabled for this tank
    if (gConfig.monitors[i].trackUnloads && !gMonitorState[i].sensorFailed) {
      evaluateUnload(i);
    }

    if (gConfig.monitors[i].enableServerUpload && !gMonitorState[i].sensorFailed) {
      const float threshold = gConfig.monitors[i].reportThreshold;
      const bool needBaseline = (gMonitorState[i].lastReportedValue < 0.0f);
      const bool thresholdEnabled = (threshold > 0.0f);
      const bool changeExceeded = thresholdEnabled && (fabs(inches - gMonitorState[i].lastReportedValue) >= threshold);
      if (needBaseline || changeExceeded) {
        sendTelemetry(i, "sample", false);
        gMonitorState[i].lastReportedValue = inches;
      }
    }
  }

  // Deferred outbox trim for current-loop devices: now that all A0602 reads AND the Phase-B
  // publishes are done, trim the outbox once.
  trimTelemetryOutbox();
}

static bool restorePersistentRelayAfterBoot(uint8_t idx, bool highAlarmActive, bool lowAlarmActive, unsigned long sampleNow) {
  if (idx >= gConfig.monitorCount) {
    return false;
  }

  const MonitorConfig &cfg = gConfig.monitors[idx];
  MonitorRuntime &state = gMonitorState[idx];
  if (cfg.relayTargetClient[0] == '\0' || cfg.relayMask == 0) {
    return false;
  }
  if (cfg.relayMode != RELAY_MODE_UNTIL_CLEAR && cfg.relayMode != RELAY_MODE_MANUAL_RESET) {
    return false;
  }
  if (!highAlarmActive && !lowAlarmActive) {
    return false;
  }

  state.highAlarmLatched = highAlarmActive;
  state.lowAlarmLatched = !highAlarmActive && lowAlarmActive;
  state.highAlarmDebounceCount = 0;
  state.lowAlarmDebounceCount = 0;
  state.highClearDebounceCount = 0;
  state.lowClearDebounceCount = 0;
  activateLocalAlarm(idx, true);
  triggerRemoteRelays(cfg.relayTargetClient, cfg.relayMask, true);
  activateRelayForMonitor(idx, cfg.relayMask, RELAY_SRC_ALARM, sampleNow);
  Serial.print(F("Persistent relay restored after reboot for monitor "));
  Serial.println(cfg.name);
  return true;
}

static void evaluateAlarms(uint8_t idx) {
  const MonitorConfig &cfg = gConfig.monitors[idx];
  MonitorRuntime &state = gMonitorState[idx];

  // Skip alarm evaluation if alarms were never configured
  if (!cfg.alarmsEnabled) {
    return;
  }

  // Skip alarm evaluation if sensor has failed
  if (state.sensorFailed) {
    return;
  }

  unsigned long sampleNow = millis();
  bool firstAlarmSample = (state.lastSampleMillis == 0);
  state.lastSampleMillis = sampleNow;

  // Handle digital sensors (float switches) differently
  if (cfg.sensorInterface == SENSOR_DIGITAL) {
    // For digital sensors, currentInches is either DIGITAL_SENSOR_ACTIVATED_VALUE (1.0) or DIGITAL_SENSOR_NOT_ACTIVATED_VALUE (0.0)
    bool isActivated = (state.currentInches > DIGITAL_SWITCH_THRESHOLD);
    bool shouldAlarm = false;
    bool triggerOnActivated = true;  // Track what condition triggers the alarm
    
    // Determine if we should alarm based on trigger configuration
    if (cfg.digitalTrigger[0] != '\0') {
      if (strcmp(cfg.digitalTrigger, "activated") == 0) {
        shouldAlarm = isActivated;  // Alarm when switch is activated
        triggerOnActivated = true;
      } else if (strcmp(cfg.digitalTrigger, "not_activated") == 0) {
        shouldAlarm = !isActivated;  // Alarm when switch is NOT activated
        triggerOnActivated = false;
      }
    } else {
      // Legacy behavior: use highAlarm/lowAlarm thresholds
      // Only one of these should be configured for a digital sensor
      // highAlarm = 1 means trigger when reading is 1.0 (switch activated)
      // lowAlarm = 0 means trigger when reading is 0.0 (switch not activated)
      bool hasHighAlarm = (cfg.highAlarmThreshold >= DIGITAL_SENSOR_ACTIVATED_VALUE);
      bool hasLowAlarm = (cfg.lowAlarmThreshold == DIGITAL_SENSOR_NOT_ACTIVATED_VALUE);
      
      if (hasHighAlarm && !hasLowAlarm) {
        shouldAlarm = isActivated;
        triggerOnActivated = true;
      } else if (hasLowAlarm && !hasHighAlarm) {
        shouldAlarm = !isActivated;
        triggerOnActivated = false;
      } else if (hasHighAlarm) {
        // Default to high alarm behavior if both are set
        shouldAlarm = isActivated;
        triggerOnActivated = true;
      }
    }

    if (firstAlarmSample && shouldAlarm && restorePersistentRelayAfterBoot(idx, true, false, sampleNow)) {
      return;
    }
    
    // Handle alarm state with debouncing
    if (shouldAlarm && !state.highAlarmLatched) {
      state.highAlarmDebounceCount++;
      state.highClearDebounceCount = 0;
      if (state.highAlarmDebounceCount >= ALARM_DEBOUNCE_COUNT) {
        state.highAlarmLatched = true;
        state.highAlarmDebounceCount = 0;
        // Send alarm with descriptive type based on configured trigger condition
        const char *alarmType = triggerOnActivated ? "triggered" : "not_triggered";
        sendAlarm(idx, alarmType, state.currentInches);
      }
    } else if (!shouldAlarm && state.highAlarmLatched) {
      state.highClearDebounceCount++;
      state.highAlarmDebounceCount = 0;
      if (state.highClearDebounceCount >= ALARM_DEBOUNCE_COUNT) {
        state.highAlarmLatched = false;
        state.highClearDebounceCount = 0;
        sendAlarm(idx, "clear", state.currentInches);
      }
    } else if (!shouldAlarm) {
      state.highAlarmDebounceCount = 0;
    } else {
      state.highClearDebounceCount = 0;
    }
    return;  // Skip the standard analog threshold evaluation
  }

  // Standard analog/current loop sensor alarm evaluation with hysteresis
  // Clamp hysteresis to be non-negative; a negative value would invert the clear bands.
  float hyst = (cfg.hysteresisValue > 0.0f) ? cfg.hysteresisValue : 0.0f;
  float highTrigger = cfg.highAlarmThreshold;
  float highClear = cfg.highAlarmThreshold - hyst;
  float lowTrigger = cfg.lowAlarmThreshold;
  float lowClear = cfg.lowAlarmThreshold + hyst;

  bool highCondition = state.currentInches >= highTrigger;
  bool lowCondition = state.currentInches <= lowTrigger;
  // Decoupled clear conditions: the high alarm clears once the level falls below the high
  // threshold minus hysteresis; the low alarm clears once the level rises above the low
  // threshold plus hysteresis. Previously both alarms shared a single mid-band clearCondition
  // ((x < highClear) && (x > lowClear)); when (highThreshold - lowThreshold) <= 2*hysteresis
  // that band was empty/inverted and a latched alarm could NEVER clear, and high-alarm
  // clearing was incorrectly coupled to the (possibly unused) low threshold.
  bool highClearCondition = state.currentInches < highClear;
  bool lowClearCondition = state.currentInches > lowClear;

  if (firstAlarmSample && restorePersistentRelayAfterBoot(idx, highCondition, !highCondition && lowCondition, sampleNow)) {
    return;
  }

  // Handle high alarm with debouncing
  if (highCondition && !state.highAlarmLatched) {
    state.highAlarmDebounceCount++;
    state.lowAlarmDebounceCount = 0;
    state.highClearDebounceCount = 0;
    if (state.highAlarmDebounceCount >= ALARM_DEBOUNCE_COUNT) {
      state.highAlarmLatched = true;
      state.lowAlarmLatched = false;
      state.highAlarmDebounceCount = 0;
      sendAlarm(idx, "high", state.currentInches);
    }
  } else if (state.highAlarmLatched && highClearCondition) {
    state.highClearDebounceCount++;
    state.highAlarmDebounceCount = 0;
    if (state.highClearDebounceCount >= ALARM_DEBOUNCE_COUNT) {
      state.highAlarmLatched = false;
      state.highClearDebounceCount = 0;
      sendAlarm(idx, "clear", state.currentInches);
    }
  } else if (!highCondition && !highClearCondition) {
    state.highAlarmDebounceCount = 0;
  }
  if (highCondition) {
    state.highClearDebounceCount = 0;
  }

  // Handle low alarm with debouncing
  if (lowCondition && !state.lowAlarmLatched) {
    state.lowAlarmDebounceCount++;
    state.highAlarmDebounceCount = 0;
    state.lowClearDebounceCount = 0;
    if (state.lowAlarmDebounceCount >= ALARM_DEBOUNCE_COUNT) {
      state.lowAlarmLatched = true;
      state.highAlarmLatched = false;
      state.lowAlarmDebounceCount = 0;
      sendAlarm(idx, "low", state.currentInches);
    }
  } else if (state.lowAlarmLatched && lowClearCondition) {
    state.lowClearDebounceCount++;
    state.lowAlarmDebounceCount = 0;
    if (state.lowClearDebounceCount >= ALARM_DEBOUNCE_COUNT) {
      state.lowAlarmLatched = false;
      state.lowClearDebounceCount = 0;
      sendAlarm(idx, "clear", state.currentInches);
    }
  } else if (!lowCondition && !lowClearCondition) {
    state.lowAlarmDebounceCount = 0;
  }
  if (lowCondition) {
    state.lowClearDebounceCount = 0;
  }
}

// ---- Registration note for unconfigured clients ----
// Sends a minimal telemetry payload (no sensor data) so the server
// discovers this device and can push a configuration to it.
static void sendRegistration(const char *reason) {
  JsonDocument doc;
  doc["c"] = gDeviceUID;
  doc["s"] = gConfig.siteName;
  doc["r"] = reason;
  doc["t"] = currentEpoch();
  doc["mc"] = 0;  // Signals server: no monitors configured
  doc["fv"] = FIRMWARE_VERSION;

  publishNote(TELEMETRY_FILE, doc, true);  // sync immediately so server sees us fast
  Serial.println(F("Registration note sent"));
}

// Writes the unified, self-describing sensor payload shared by telemetry, alarm, and daily
// notes. Emits object type (ot), measurement unit (mu), sensor interface (st), the raw
// reading (ma/vt/fl/rm) for the server's learned-calibration input, the client-computed
// level (lvl), the capacity/full-scale (cap), and the calibration version (cv) the client
// applied. The server trusts lvl when cv matches its own calibration; raw is kept so the
// server can re-derive the level when the client's cv is stale.
static void buildSensorObject(JsonObject o, uint8_t idx) {
  const MonitorConfig &cfg = gConfig.monitors[idx];
  MonitorRuntime &state = gMonitorState[idx];

  switch (cfg.objectType) {
    case OBJECT_ENGINE: o["ot"] = "engine"; break;
    case OBJECT_PUMP:   o["ot"] = "pump";   break;
    case OBJECT_GAS:    o["ot"] = "gas";    break;
    case OBJECT_FLOW:   o["ot"] = "flow";   break;
    default:            o["ot"] = "tank";   break;
  }
  if (cfg.measurementUnit[0] != '\0') {
    o["mu"] = cfg.measurementUnit;
  }

  switch (cfg.sensorInterface) {
    case SENSOR_DIGITAL:
      o["st"] = "digital";
      o["fl"] = roundTo(state.currentInches, 1);  // 1.0 or 0.0 (float switch state)
      break;
    case SENSOR_CURRENT_LOOP:
      // Fix 12 (v2.0.51): current-loop sensors are RAW-mA-only on the wire. The server
      // converts mA -> engineering units using the sensor config it gave us; the client
      // deliberately does NOT emit a computed `lvl`/`cap` for current-loop so a failed
      // read or stale-reused value cannot silently masquerade as a real measurement.
      //   - Successful read  -> emit `ma` (always, even at live-zero ~4 mA).
      //   - Failed read (ru) -> emit `fault: "<short_string>"` and DO NOT emit `ma`.
      // `pg` (PWM gate enable result) is kept for diagnostics on either path.
      o["st"] = "currentLoop";
      if (cfg.pwmGatingEnabled) o["pg"] = state.lastPwmEnableOk ? 1 : 0;
      if (state.sampleReused) {
        o["fault"] = clFaultReasonString(gLastClFaultReason);
      } else {
        o["ma"] = roundTo(state.currentSensorMa, 2);
      }
      break;
    case SENSOR_ANALOG:
      o["st"] = "analog";
      if (state.currentSensorVoltage > 0.0f) o["vt"] = roundTo(state.currentSensorVoltage, 3);
      break;
    case SENSOR_PULSE:
    default:
      o["st"] = "pulse";
      o["rm"] = roundTo(state.currentInches, 1);
      break;
  }

  // Self-describing level + capacity so every note decodes with zero registry state.
  // Fix 12 (v2.0.51): current-loop sensors are excluded — the raw `ma` (or `fault` string)
  // emitted above is authoritative; emitting `lvl`/`cap` here would create the ambiguous
  // "lvl:0 could be a real 0 PSI or a faulted/reused default" condition that motivated
  // this change. Other sensor types still emit lvl + cap so existing dashboards keep working.
  if (cfg.sensorInterface != SENSOR_CURRENT_LOOP) {
    o["lvl"] = roundTo(state.currentInches, 1);
    o["cap"] = roundTo(getMonitorHeight(cfg), 1);
  }
  if (cfg.hasLearnedCalibration) {
    o["cv"] = cfg.calVersion;
  }

  // Data-quality flags (Fix 8): mark when lvl is NOT a fresh valid acquisition so the server
  // and dashboard do not treat a reused or faulted reading as an authoritative measurement.
  if (state.sensorFailed) o["sf"] = 1;   // sensor currently in a failed/fault state
  if (state.sampleReused) o["ru"] = 1;   // this value was reused from a previous cycle
}

static void sendTelemetry(uint8_t idx, const char *reason, bool syncNow) {
  if (idx >= gConfig.monitorCount) {
    return;
  }

  const MonitorConfig &cfg = gConfig.monitors[idx];
  MonitorRuntime &state = gMonitorState[idx];

  JsonDocument doc;
  doc["c"] = gDeviceUID;
  doc["s"] = gConfig.siteName;
  doc["k"] = cfg.sensorIndex;
  doc["n"] = cfg.name;
  if (cfg.userNumber > 0) doc["un"] = cfg.userNumber;

  // Object type, measurement unit, sensor type, raw reading, lvl, cap, cv
  buildSensorObject(doc.as<JsonObject>(), idx);

  doc["r"] = reason;
  // Use acquisition time so stale/reused values do not get a fresh timestamp.
  doc["t"] = (state.lastReadingEpoch > 0.0) ? state.lastReadingEpoch : currentEpoch();

  // TEMPORARY (2026-06-15): include system voltage in every telemetry note so the dashboard
  // VIN reflects the live battery/MPPT reading instead of waiting for the once-daily report.
  // This duplicates the voltage already sent in the daily report and may be REMOVED later once
  // we are satisfied with daily-only reporting (or replaced by a dedicated power note).
  float telemetryVoltage = getEffectiveBatteryVoltage();
  if (telemetryVoltage > 0.0f) {
    doc["v"] = roundTo(telemetryVoltage, 2);
    // Fix 7: tag the measurement source so the server only displays voltage that came from
    // the MPPT charge controller or the analog Vin divider (never a Notecard ~5V rail).
    if (gEffectiveVoltageSource) doc["vs"] = gEffectiveVoltageSource;
  }

  // MPPT/RS-485 observability: when the SunSaver charger is configured, report whether the
  // Modbus link is currently healthy (scOk=1) or failing (scOk=0). Without this an
  // enabled-but-not-communicating charger is indistinguishable from a disabled one -- it sends
  // no voltage, and the comm-failure alarm defaults off, so the failure is otherwise silent.
  if (gSolarManager.isEnabled()) {
    doc["scOk"] = gSolarManager.isCommunicationOk() ? 1 : 0;
    // v2.0.45: when the link READS but the values are rejected by the plausibility clamp
    // (scOk:0 yet a CRC-valid Modbus read happened), flag scImpl:1 so the dashboard can tell
    // "no link" (timeout) apart from "values rejected" (data). Omitted on a healthy link.
    if (!gSolarManager.isCommunicationOk() && gSolarManager.wasLastReadImplausible()) {
      doc["scImpl"] = 1;
    }
  }

  publishNote(TELEMETRY_FILE, doc, syncNow);
}

static bool checkAlarmRateLimit(uint8_t idx, const char *alarmType) {
  if (idx >= gConfig.monitorCount) {
    return false;
  }

  MonitorRuntime &state = gMonitorState[idx];
  unsigned long now = millis();

  // Check minimum interval between same alarm type
  unsigned long minInterval = MIN_ALARM_INTERVAL_SECONDS * 1000UL;
  
  if (strcmp(alarmType, "high") == 0) {
    if (now - state.lastHighAlarmMillis < minInterval) {
      Serial.print(F("Rate limit: High alarm suppressed for monitor "));
      Serial.println(idx);
      return false;
    }
  } else if (strcmp(alarmType, "low") == 0) {
    if (now - state.lastLowAlarmMillis < minInterval) {
      Serial.print(F("Rate limit: Low alarm suppressed for monitor "));
      Serial.println(idx);
      return false;
    }
  } else if (strcmp(alarmType, "clear") == 0 || strcmp(alarmType, "sensor-recovered") == 0) {
    // Never rate-limit clear/recovery notes — the server needs them to resolve alarms
    state.lastClearAlarmMillis = now;
    return true;
  } else if (strcmp(alarmType, "relay_timeout") == 0) {
    // BugFix v1.6.2 (I-13): Relay safety timeouts must never be rate-limited —
    // these are critical safety notifications that the operator must receive.
    return true;
  } else if (strcmp(alarmType, "sensor-fault") == 0 || strcmp(alarmType, "sensor-stuck") == 0) {
    if (now - state.lastSensorFaultMillis < minInterval) {
      Serial.print(F("Rate limit: Sensor fault suppressed for monitor "));
      Serial.println(idx);
      return false;
    }
  }

  // Check hourly rate limit - remove timestamps older than 1 hour
  // BugFix v1.6.2 (I-12): Guard against unsigned underflow when millis() < 1 hour.
  // When uptime is less than 1 hour, all timestamps are inherently recent — skip pruning.
  if (now >= 3600000UL) {
    unsigned long oneHourAgo = now - 3600000UL;
    uint8_t validCount = 0;
    for (uint8_t i = 0; i < state.alarmCount; ++i) {
      if (state.alarmTimestamps[i] > oneHourAgo) {
        state.alarmTimestamps[validCount++] = state.alarmTimestamps[i];
      }
    }
    state.alarmCount = validCount;
  }

  // Check if we've exceeded the hourly limit
  if (state.alarmCount >= MAX_ALARMS_PER_HOUR) {
    Serial.print(F("Rate limit: Hourly limit exceeded for monitor "));
    Serial.print(idx);
    Serial.print(F(" ("));
    Serial.print(state.alarmCount);
    Serial.print(F("/"));
    Serial.print(MAX_ALARMS_PER_HOUR);
    Serial.println(F(")"));
    return false;
  }

  // Global alarm rate limit — cap total alarms across ALL sensors per hour
  // BugFix 04022026 (MED-12): Check global cap BEFORE committing per-monitor timestamp.
  // Previously, per-monitor timestamp was added first, so global rejection still consumed
  // the per-monitor budget — accelerating per-monitor rate exhaustion.
  {
    // BugFix v1.6.2 (I-12): Same unsigned-underflow guard for global alarm budget.
    if (now >= 3600000UL) {
      unsigned long oneHourAgo = now - 3600000UL;
      uint8_t gValid = 0;
      for (uint8_t g = 0; g < gGlobalAlarmCount; ++g) {
        if (gGlobalAlarmTimestamps[g] > oneHourAgo) {
          gGlobalAlarmTimestamps[gValid++] = gGlobalAlarmTimestamps[g];
        }
      }
      gGlobalAlarmCount = gValid;
    }

    if (gGlobalAlarmCount >= MAX_GLOBAL_ALARMS_PER_HOUR) {
      Serial.print(F("Rate limit: Global hourly cap reached ("));
      Serial.print(gGlobalAlarmCount);
      Serial.print(F("/"));
      Serial.print(MAX_GLOBAL_ALARMS_PER_HOUR);
      Serial.println(F(")"));
      return false;
    }
  }

  // Both per-monitor and global checks passed — commit timestamps to both budgets
  if (state.alarmCount < MAX_ALARMS_PER_HOUR) {
    state.alarmTimestamps[state.alarmCount++] = now;
  }
  if (gGlobalAlarmCount < MAX_GLOBAL_ALARMS_PER_HOUR) {
    gGlobalAlarmTimestamps[gGlobalAlarmCount++] = now;
  }

  // Update last alarm time for this type
  if (strcmp(alarmType, "high") == 0) {
    state.lastHighAlarmMillis = now;
  } else if (strcmp(alarmType, "low") == 0) {
    state.lastLowAlarmMillis = now;
  } else if (strcmp(alarmType, "clear") == 0) {
    state.lastClearAlarmMillis = now;
  } else if (strcmp(alarmType, "sensor-fault") == 0 || strcmp(alarmType, "sensor-stuck") == 0) {
    state.lastSensorFaultMillis = now;
  }

  return true;
}

static void activateLocalAlarm(uint8_t idx, bool active) {
  // Use Opta's built-in relay outputs for local alarm indication.
  // If the monitor has a configured relayMask, use that; otherwise fall back
  // to mapping monitor index directly to a relay (monitor 0 -> relay 0, etc.)
  uint8_t mask = 0;
  if (idx < gConfig.monitorCount) {
    mask = gConfig.monitors[idx].relayMask;
  }

  if (mask != 0) {
    // Activate all relays specified in the bitmask
    for (uint8_t r = 0; r < 4; ++r) {
      if (mask & (1 << r)) {
        int relayPin = getRelayPin(r);
        if (relayPin >= 0) {
          pinMode(relayPin, OUTPUT);
          digitalWrite(relayPin, active ? HIGH : LOW);
        }
      }
    }
  } else {
    // Legacy fallback: map monitor index directly to relay (only works for sensors 0-3)
    int relayPin = getRelayPin(idx);
    if (relayPin >= 0) {
      pinMode(relayPin, OUTPUT);
      digitalWrite(relayPin, active ? HIGH : LOW);
    }
  }
  
  if (active) {
    Serial.print(F("LOCAL ALARM ACTIVE - Sensor "));
    Serial.println(idx);
  } else {
    Serial.print(F("LOCAL ALARM CLEARED - Sensor "));
    Serial.println(idx);
  }
}

static void sendAlarm(uint8_t idx, const char *alarmType, float inches) {
  if (idx >= gConfig.monitorCount) {
    return;
  }

  const MonitorConfig &cfg = gConfig.monitors[idx];
  bool allowSmsEscalation = cfg.enableAlarmSms;

  // Always activate local alarm regardless of rate limits
  // relay_timeout is an operational notification, not an alarm condition
  bool isRelayTimeout = (strcmp(alarmType, "relay_timeout") == 0);
  bool isAlarm = (strcmp(alarmType, "clear") != 0) && !isRelayTimeout;
  activateLocalAlarm(idx, isAlarm);

  // BugFix 04022026 (CRITICAL-2): Relay control must execute BEFORE rate limiting.
  // Previously, relay logic was inside the Notecard transmission block, so rate-limited
  // alarms never actuated remote relays — a safety-critical gap for pump/valve control.
  // BugFix v1.6.0: relay_timeout must NOT re-activate relays (would defeat safety timeout).
  if (cfg.relayTargetClient[0] != '\0' && cfg.relayMask != 0) {
    bool shouldActivateRelay = false;
    bool shouldDeactivateRelay = false;
    
    // Check if this alarm type matches the relay trigger condition
    // relay_timeout is excluded — it is a notification, not an alarm trigger
    if (isAlarm) {
      if (cfg.relayTrigger == RELAY_TRIGGER_ANY) {
        shouldActivateRelay = true;
      } else if (cfg.relayTrigger == RELAY_TRIGGER_HIGH && strcmp(alarmType, "high") == 0) {
        shouldActivateRelay = true;
      } else if (cfg.relayTrigger == RELAY_TRIGGER_LOW && strcmp(alarmType, "low") == 0) {
        shouldActivateRelay = true;
      }
    } else if (!isRelayTimeout) {
      // BugFix 04022026 (CRITICAL-3): UNTIL_CLEAR relay clearing was broken.
      // When alarmType is "clear", comparing it against "high"/"low" always fails.
      // Fix: When the relay is active for this monitor and mode is UNTIL_CLEAR,
      // unconditionally clear — the alarm condition has been resolved regardless
      // of which specific threshold originally triggered it.
      if (cfg.relayMode == RELAY_MODE_UNTIL_CLEAR && isMonitorRelayActive(idx)) {
        shouldDeactivateRelay = true;
      }
    }
    
    if (shouldActivateRelay && !isMonitorRelayActive(idx)) {
      triggerRemoteRelays(cfg.relayTargetClient, cfg.relayMask, true);
      activateRelayForMonitor(idx, cfg.relayMask, RELAY_SRC_ALARM, millis());
      Serial.print(F("Relay activated for "));
      Serial.print(alarmType);
      Serial.print(F(" alarm (mode: "));
      switch (cfg.relayMode) {
        case RELAY_MODE_MOMENTARY: Serial.print(F("momentary 30min")); break;
        case RELAY_MODE_UNTIL_CLEAR: Serial.print(F("until clear")); break;
        case RELAY_MODE_MANUAL_RESET: Serial.print(F("manual reset")); break;
      }
      Serial.println(F(")"));
    } else if (shouldDeactivateRelay) {
      uint8_t activeMask = getMonitorActiveRelayMask(idx);
      triggerRemoteRelays(cfg.relayTargetClient, activeMask, false);
      deactivateRelayForMonitor(idx, activeMask);
      Serial.println(F("Relay deactivated on alarm clear"));
    }
  }

  // Check rate limit before sending remote alarm notification
  // Note: Rate limiting only gates Notecard message transmission, NOT relay actuation above
  if (!checkAlarmRateLimit(idx, alarmType)) {
    return;  // Rate limit exceeded — relay already handled above
  }

  MonitorRuntime &state = gMonitorState[idx];
  state.lastAlarmSendMillis = millis();

  // Try to send via network if available
  if (gNotecardAvailable) {
    JsonDocument doc;
    doc["c"] = gDeviceUID;
    doc["s"] = gConfig.siteName;
    doc["k"] = cfg.sensorIndex;
    if (cfg.userNumber > 0) doc["un"] = cfg.userNumber;
    doc["y"] = alarmType;
    // Object type, measurement unit, sensor type, raw reading, lvl, cap, cv
    buildSensorObject(doc.as<JsonObject>(), idx);

    doc["th"] = roundTo(cfg.highAlarmThreshold, 1);
    doc["tl"] = roundTo(cfg.lowAlarmThreshold, 1);
    if (allowSmsEscalation) {
      doc["se"] = true;  // Only include when true (false is default)
    }
    doc["t"] = currentEpoch();

    publishNote(ALARM_FILE, doc, true);
    Serial.print(F("Alarm sent for monitor "));
    Serial.print(cfg.name);
    Serial.print(F(" type "));
    Serial.println(alarmType);
    
    char logMsg[128];
    // Universal (not liquid-level-specific): label the reading with the monitor's own unit
    // (psi/rpm/gpm/inches) rather than a hardcoded "in" suffix. Falls back to "in" only when
    // no unit is configured (legacy/bootstrap default).
    const char *logUnit = (cfg.measurementUnit[0] != '\0') ? cfg.measurementUnit : "in";
    snprintf(logMsg, sizeof(logMsg), "Alarm: %s - %s - %.1f %s", cfg.name, alarmType, inches, logUnit);
    addSerialLog(logMsg);
  } else {
    Serial.print(F("Network offline - local alarm only for monitor "));
    Serial.print(cfg.name);
    Serial.print(F(" type "));
    Serial.println(alarmType);
  }
}

// ============================================================================
// Tank Unload Detection
// ============================================================================
// Detects when a tank has been emptied/unloaded and logs the event.
// 
// Algorithm:
// 1. Track the peak (highest) level seen since the last unload event
// 2. When level drops significantly (configurable %) from peak, trigger unload event
// 3. If level drops to/below sensor height, use default empty height
// 4. Log: peak height, new low height, timestamps
// 5. Optionally send SMS/email notification
//
// Use cases:
// - Fill-and-empty sensors (fuel delivery sensors, milk sensors, etc.)
// - NOT for sensors that fluctuate through in/out ports
// ============================================================================

static uint8_t gUnloadDebounceCount[MAX_MONITORS] = {0};

static void evaluateUnload(uint8_t idx) {
  if (idx >= gConfig.monitorCount) {
    return;
  }

  const MonitorConfig &cfg = gConfig.monitors[idx];
  MonitorRuntime &state = gMonitorState[idx];

  // Skip if not tracking unloads or sensor failed
  if (!cfg.trackUnloads || state.sensorFailed) {
    return;
  }

  float currentInches = state.currentInches;
  
  // Determine the unload threshold (use percentage or absolute, whichever is configured)
  float dropPercent = (cfg.unloadDropPercent > 0.0f) ? cfg.unloadDropPercent : UNLOAD_DEFAULT_DROP_PERCENT;
  float minPeakHeight = UNLOAD_MIN_PEAK_HEIGHT;
  
  // If we haven't started tracking yet, wait for tank to reach minimum peak
  if (!state.unloadTracking) {
    if (currentInches >= minPeakHeight) {
      // Start tracking - tank has reached minimum fill level
      state.unloadTracking = true;
      state.unloadPeakInches = currentInches;
      state.unloadPeakSensorMa = state.currentSensorMa;
      state.unloadPeakEpoch = currentEpoch();
      Serial.print(F("Unload tracking started for "));
      Serial.print(cfg.name);
      Serial.print(F(" at "));
      Serial.print(currentInches);
      Serial.println(F(" inches"));
    }
    return;
  }

  // Update peak if current level is higher
  if (currentInches > state.unloadPeakInches) {
    state.unloadPeakInches = currentInches;
    state.unloadPeakSensorMa = state.currentSensorMa;
    state.unloadPeakEpoch = currentEpoch();
    gUnloadDebounceCount[idx] = 0;  // Reset debounce on new peak
    return;
  }

  // Calculate threshold for unload detection
  float dropThreshold = state.unloadPeakInches * (dropPercent / 100.0f);
  float unloadTriggerLevel = state.unloadPeakInches - dropThreshold;
  
  // Use configured absolute threshold if set and lower than percentage-based
  if (cfg.unloadDropThreshold > 0.0f) {
    float absoluteTrigger = state.unloadPeakInches - cfg.unloadDropThreshold;
    if (absoluteTrigger < unloadTriggerLevel) {
      unloadTriggerLevel = absoluteTrigger;
    }
  }
  
  // Check if level has dropped enough to be considered an unload
  if (currentInches <= unloadTriggerLevel) {
    // Debounce: require consecutive low readings
    gUnloadDebounceCount[idx]++;
    
    if (gUnloadDebounceCount[idx] >= UNLOAD_DEBOUNCE_COUNT) {
      // Determine the "empty" level to report
      float emptyHeight = currentInches;
      
      // If level is at or below sensor mount height, use default empty height
      if (currentInches <= cfg.sensorMountHeight) {
        emptyHeight = (cfg.unloadEmptyHeight > 0.0f) ? cfg.unloadEmptyHeight : UNLOAD_DEFAULT_EMPTY_HEIGHT;
      }
      
      // Send unload event
      sendUnloadEvent(idx, state.unloadPeakInches, emptyHeight, state.unloadPeakEpoch);
      
      // Reset tracking for next fill cycle - tank must refill above minimum before next unload
      state.unloadTracking = false;
      gUnloadDebounceCount[idx] = 0;
    }
  } else {
    // Level not low enough - reset debounce counter
    gUnloadDebounceCount[idx] = 0;
  }
}

static void sendUnloadEvent(uint8_t idx, float peakInches, float currentInches, double peakEpoch) {
  if (idx >= gConfig.monitorCount) {
    return;
  }

  const MonitorConfig &cfg = gConfig.monitors[idx];
  MonitorRuntime &state = gMonitorState[idx];

  Serial.print(F("Tank unload detected: "));
  Serial.print(cfg.name);
  Serial.print(F(" peak="));
  Serial.print(peakInches);
  Serial.print(F("in, current="));
  Serial.print(currentInches);
  Serial.println(F("in"));

  // Log to serial
  char logMsg[128];
  snprintf(logMsg, sizeof(logMsg), "Unload: %s peak=%.1fin, empty=%.1fin", 
           cfg.name, peakInches, currentInches);
  addSerialLog(logMsg);

  // Send unload event via Notecard if network available
  if (gNotecardAvailable) {
    JsonDocument doc;
    doc["c"] = gDeviceUID;
    doc["s"] = gConfig.siteName;
    doc["k"] = cfg.sensorIndex;
    // Note: "type" = "unload" omitted — routing is by file (unload.qi)
    doc["pk"] = roundTo(peakInches, 1);      // Peak height
    doc["em"] = roundTo(currentInches, 1);   // Empty/low height
    doc["pt"] = peakEpoch;                    // Peak timestamp
    doc["t"] = currentEpoch();               // Event timestamp
    
    // Include raw sensor readings only if available
    if (state.unloadPeakSensorMa >= 4.0f) {
      doc["pma"] = roundTo(state.unloadPeakSensorMa, 2);
    }
    if (state.currentSensorMa >= 4.0f) {
      doc["ema"] = roundTo(state.currentSensorMa, 2);
    }
    // Include measurement unit so server can display correct units
    if (cfg.measurementUnit[0] != '\0') {
      doc["mu"] = cfg.measurementUnit;
    }

    // BugFix 04022026 (HIGH-8): Include notification preferences so server can
    // trigger SMS/email for unload events. Previously these fields were missing,
    // causing handleUnload() on the server to read undefined values.
    if (cfg.unloadAlarmSms) {
      doc["sms"] = true;
    }
    if (cfg.unloadAlarmEmail) {
      doc["email"] = true;
    }

    publishNote(UNLOAD_FILE, doc, true);
    Serial.println(F("Unload event sent to server"));
  } else {
    Serial.println(F("Network offline - unload event not sent"));
  }
}

// ============================================================================
// Solar/Battery Charger Alarm Functions (SunSaver MPPT via RS-485)
// ============================================================================

static void logSolarHardwareTestHeartbeat(unsigned long now) {
#ifdef SOLAR_HW_TEST_SERIAL
  static unsigned long lastHeartbeatMillis = 0;
  if (now - lastHeartbeatMillis < 10000UL) {
    return;
  }

  lastHeartbeatMillis = now;

  SOLAR_TEST_PRINT(F("SolarTest ms="));
  SOLAR_TEST_PRINT(now);
  SOLAR_TEST_PRINT(F(" solarEnabled="));
  SOLAR_TEST_PRINT(gSolarManager.isEnabled() ? 1 : 0);
  SOLAR_TEST_PRINT(F(" battEnabled="));
  SOLAR_TEST_PRINT(gConfig.batteryMonitor.enabled ? 1 : 0);
  SOLAR_TEST_PRINT(F(" notecard="));
  SOLAR_TEST_PRINT(gNotecardAvailable ? F("OK") : F("FAIL"));
  SOLAR_TEST_PRINT(F(" power="));
  SOLAR_TEST_PRINT(getPowerStateDescription(gPowerState));

  if (gSolarManager.isEnabled()) {
    const SolarData &data = gSolarManager.getData();
    SOLAR_TEST_PRINT(F(" comm="));
    SOLAR_TEST_PRINT(data.communicationOk ? F("OK") : F("FAIL"));
    SOLAR_TEST_PRINT(F(" err="));
    SOLAR_TEST_PRINT(data.consecutiveErrors);
  }

  SOLAR_TEST_PRINTLN("");
#else
  (void)now;
#endif
}

static void logSolarPollSnapshot(unsigned long now, SolarAlertType alertType) {
#ifdef SOLAR_HW_TEST_SERIAL
  const SolarData &data = gSolarManager.getData();

  SOLAR_TEST_PRINT(F("SolarPoll ms="));
  SOLAR_TEST_PRINT(now);
  SOLAR_TEST_PRINT(F(" comm="));
  SOLAR_TEST_PRINT(data.communicationOk ? F("OK") : F("FAIL"));
  SOLAR_TEST_PRINT(F(" err="));
  SOLAR_TEST_PRINT(data.consecutiveErrors);
  SOLAR_TEST_PRINT(F(" bv="));
  SOLAR_TEST_PRINT(roundTo(data.batteryVoltage, 2));
  SOLAR_TEST_PRINT(F(" av="));
  SOLAR_TEST_PRINT(roundTo(data.arrayVoltage, 2));
  SOLAR_TEST_PRINT(F(" ic="));
  SOLAR_TEST_PRINT(roundTo(data.chargeCurrent, 2));
  SOLAR_TEST_PRINT(F(" lc="));
  SOLAR_TEST_PRINT(roundTo(data.loadCurrent, 2));
  SOLAR_TEST_PRINT(F(" cs="));
  SOLAR_TEST_PRINT(gSolarManager.getChargeStateDescription());
  SOLAR_TEST_PRINT(F(" faults=0x"));
  if (data.faults < 0x1000) SOLAR_TEST_PRINT('0');
  if (data.faults < 0x0100) SOLAR_TEST_PRINT('0');
  if (data.faults < 0x0010) SOLAR_TEST_PRINT('0');
  SOLAR_TEST_PRINT(data.faults);
  SOLAR_TEST_PRINT(F(" alarms=0x"));
  if (data.alarms < 0x1000) SOLAR_TEST_PRINT('0');
  if (data.alarms < 0x0100) SOLAR_TEST_PRINT('0');
  if (data.alarms < 0x0010) SOLAR_TEST_PRINT('0');
  SOLAR_TEST_PRINT(data.alarms);

  if (alertType != SOLAR_ALERT_NONE) {
    SOLAR_TEST_PRINT(F(" alert="));
    SOLAR_TEST_PRINT(gSolarManager.getAlertDescription(alertType));
  }

  SOLAR_TEST_PRINTLN("");
#else
  (void)now;
  (void)alertType;
#endif
}

static void sendSolarAlarm(SolarAlertType alertType) {
  if (!gSolarManager.isEnabled() || alertType == SOLAR_ALERT_NONE) {
    return;
  }
  
  const SolarData &data = gSolarManager.getData();
  const char *alertDesc = gSolarManager.getAlertDescription(alertType);
  
  Serial.print(F("Solar alert: "));
  Serial.println(alertDesc);
  
  char logMsg[128];
  snprintf(logMsg, sizeof(logMsg), "Solar: %s (%.2fV)", alertDesc, data.batteryVoltage);
  addSerialLog(logMsg);
  
  if (!gNotecardAvailable) {
    Serial.println(F("Network offline - solar alarm not sent"));
    return;
  }
  
  JsonDocument doc;
  doc["c"] = gDeviceUID;
  doc["s"] = gConfig.siteName;
  doc["y"] = "solar";
  doc["t"] = currentEpoch();
  
  // Alert type ("desc" omitted — derivable from alert enum on server)
  switch (alertType) {
    case SOLAR_ALERT_BATTERY_LOW:     doc["alert"] = "battery_low"; break;
    case SOLAR_ALERT_BATTERY_CRITICAL: doc["alert"] = "battery_critical"; break;
    case SOLAR_ALERT_BATTERY_HIGH:    doc["alert"] = "battery_high"; break;
    case SOLAR_ALERT_FAULT:           doc["alert"] = "fault"; break;
    case SOLAR_ALERT_ALARM:           doc["alert"] = "alarm"; break;
    case SOLAR_ALERT_COMM_FAILURE:    doc["alert"] = "comm_fail"; break;
    case SOLAR_ALERT_HEATSINK_TEMP:   doc["alert"] = "heatsink_temp"; break;
    case SOLAR_ALERT_NO_CHARGE:       doc["alert"] = "no_charge"; break;
    default:                          doc["alert"] = "unknown"; break;
  }
  
  // Battery and solar data (essential only)
  doc["bv"] = roundTo(data.batteryVoltage, 2);       // Battery voltage
  doc["av"] = roundTo(data.arrayVoltage, 2);         // Array (solar) voltage
  doc["ic"] = roundTo(data.chargeCurrent, 2);        // Charge current
  
  // Include faults/alarms descriptions only if present
  if (data.hasFault) {
    doc["faults"] = gSolarManager.getFaultDescription();
  }
  if (data.hasAlarm) {
    doc["alarms"] = gSolarManager.getAlarmDescription();
  }
  
  // SMS escalation follows the corresponding alert policy.
  bool escalateSms = false;
  if (alertType == SOLAR_ALERT_BATTERY_CRITICAL) {
    escalateSms = gConfig.solarCharger.alertOnLowBattery;
  } else if (alertType == SOLAR_ALERT_FAULT || alertType == SOLAR_ALERT_ALARM) {
    escalateSms = gConfig.solarCharger.alertOnFault;
  }
  if (escalateSms) {
    doc["se"] = true;
  }
  
  publishNote(ALARM_FILE, doc, true);
  Serial.println(F("Solar alarm sent to server"));
}

// Append solar charger data to daily report (called from sendDailyReport)
static bool appendSolarDataToDaily(JsonDocument &doc) {
  if (!gSolarManager.isEnabled() || !gConfig.solarCharger.includeInDailyReport) {
    return false;
  }
  
  const SolarData &data = gSolarManager.getData();
  
  // When the Modbus link is down, surface a minimal failure status instead of silently
  // omitting the solar block -- otherwise an enabled-but-not-communicating MPPT looks
  // identical to a disabled one on the dashboard / event log.
  if (!data.communicationOk) {
    JsonObject solarFail = doc["solar"].to<JsonObject>();
    solarFail["commOk"] = 0;
    solarFail["errs"] = data.consecutiveErrors;
    // v2.0.45 taxonomy: surface WHY the link failed so a field scOk:0 can be classified
    // remotely (timeout = physical; illegal data address = register/slave; invalid CRC = noise),
    // and tell a transport failure apart from a CRC-valid-but-rejected read (scImpl).
    solarFail["merr"]  = gSolarManager.getModbusErrorCount();
    solarFail["maddr"] = gSolarManager.getLastModbusFailAddress();
    solarFail["mms"]   = gSolarManager.getLastModbusResponseMs();
    if (gSolarManager.wasLastReadImplausible()) solarFail["scImpl"] = 1;
    const char *lastErr = gSolarManager.getLastModbusError();
    if (lastErr && lastErr[0]) solarFail["merrTxt"] = lastErr;
    return true;
  }
  
  JsonObject solar = doc["solar"].to<JsonObject>();
  solar["commOk"] = 1;
  
  // Current readings
  solar["bv"] = roundTo(data.batteryVoltage, 2);       // Battery voltage
  solar["av"] = roundTo(data.arrayVoltage, 2);         // Array voltage
  solar["ic"] = roundTo(data.chargeCurrent, 2);        // Charge current
  
  // Daily statistics (Task 1.2 & 2.1)
#ifdef SOLAR_ENABLE_UNVERIFIED_REGISTERS
  solar["bvMin"] = roundTo(data.batteryVoltageMinDaily, 2);
  solar["bvMax"] = roundTo(data.batteryVoltageMaxDaily, 2);
  if (data.ampHoursDaily > 0.0f) {
    solar["ah"] = roundTo(data.ampHoursDaily, 1);      // Amp-hours today (only if non-zero)
  }
  solar["ht"] = data.heatsinkTemp;  // Heatsink temp °C
#else
  // Software daily min/max (omitted if unseeded)
  if (data.batteryVoltageMinDaily > 0.1f) {
    solar["bvMin"] = roundTo(data.batteryVoltageMinDaily, 2);
  }
  if (data.batteryVoltageMaxDaily > 0.1f) {
    solar["bvMax"] = roundTo(data.batteryVoltageMaxDaily, 2);
  }
#endif

  // Omitted: healthy, battOk (derivable from bv thresholds); cs (charge state string)
  // commOk is implicit (if this data exists, comm is OK)
  
  // Include any active faults/alarms (only when present)
  if (data.hasFault) {
    solar["faults"] = gSolarManager.getFaultDescription();
  }
  if (data.hasAlarm) {
    solar["alarms"] = gSolarManager.getAlarmDescription();
  }
  
  return true;
}

// ============================================================================
// Battery Voltage Monitoring Functions
// Source-agnostic: reads voltage from getEffectiveBatteryVoltage() (SunSaver MPPT preferred,
// analog Vin divider as fallback). Notecard card.voltage is never used (Fix 11).
// ============================================================================

/**
 * Poll battery voltage from the active source (MPPT or Vin divider) and update BatteryData.
 * Returns true if a valid voltage was sampled, false otherwise.
 *
 * Fix 11: previously read Notecard card.voltage; on the Blues "Wireless for Opta" carrier
 * card.voltage is the regulated ~5V DC-DC rail (not the 12V battery) and so was unusable.
 * This function now reads getEffectiveBatteryVoltage(), which honors strict priority
 * MPPT > Vin divider (Fix 9). Trend statistics (weekly/monthly/SOC) that the Notecard used
 * to provide are not reconstructed in firmware; running min/max are tracked across this boot.
 */
static bool pollBatteryVoltage(BatteryData &data, const BatteryConfig &cfg) {
  float v = getEffectiveBatteryVoltage();
  const char *src = gEffectiveVoltageSource;

  if (v <= 0.0f || src == nullptr) {
    data.valid = false;
    return false;
  }

  data.voltage = v;

  // Repurposed: data.mode now holds the source identifier ("mppt" | "vin-divider"), not the
  // Notecard's voltage-mode classification. Used by checkBatteryAlerts() to defer to
  // solarCharger's own alert pipeline when MPPT is the active source.
  strlcpy(data.mode, src, sizeof(data.mode));

  // Notecard-specific fields are not applicable to MPPT/Vin sources
  data.usbPowered = false;
  data.uptimeMinutes = (uint32_t)(millis() / 60000UL);

  // Running min/max across this boot. voltageMin starts at 0 (memset) which would otherwise
  // permanently win the min comparison — seed it on first valid sample.
  if (data.voltageMin <= 0.0f || data.voltage < data.voltageMin) {
    data.voltageMin = data.voltage;
  }
  if (data.voltage > data.voltageMax) {
    data.voltageMax = data.voltage;
  }
  // No firmware-side trend tracking yet — leave dailyChange/weeklyChange/monthlyChange at 0,
  // which the alarm/daily emitters already skip via `if (... != 0.0f)` gates.

  data.isHealthy = (data.voltage >= cfg.normalVoltage && data.voltage <= cfg.highVoltage);
  data.isCharging = false;
  data.isDeclining = false;
  data.valid = true;
  data.lastReadMillis = millis();

  // Log voltage reading
  Serial.print(F("Battery: "));
  Serial.print(data.voltage, 2);
  Serial.print(F("V ["));
  Serial.print(src);
  Serial.print(F("] ("));
  Serial.print(getBatteryStateDescription(data.voltage, &cfg));
  Serial.println(F(")"));

  return true;
}

/**
 * Check battery data against thresholds and trigger alerts if needed.
 *
 * Fix 11: when the active source is "mppt", suppress alerting from this pipeline because
 * gSolarManager already publishes its own low/critical/high alerts via
 * solarCharger.alertOnLowBattery (avoids double-alerting on the same MPPT reading). When the
 * active source is "vin-divider" (or anything else), this pipeline owns the alerts.
 */
static void checkBatteryAlerts(const BatteryData &data, const BatteryConfig &cfg) {
  if (!data.valid) return;

  // Fix 11: defer to solarCharger when MPPT is the active source.
  if (data.mode[0] != '\0' && strcmp(data.mode, "mppt") == 0) {
    return;
  }
  
  unsigned long now = millis();
  BatteryAlertType alert = BATTERY_ALERT_NONE;
  
  // Check voltage thresholds (most critical first)
  if (data.voltage <= cfg.criticalVoltage) {
    if (cfg.alertOnCritical) {
      alert = BATTERY_ALERT_CRITICAL;
    }
  } else if (data.voltage <= cfg.lowVoltage) {
    if (cfg.alertOnLow) {
      alert = BATTERY_ALERT_LOW;
    }
  } else if (data.voltage >= cfg.highVoltage) {
    // High voltage (overcharge) alert
    alert = BATTERY_ALERT_HIGH;
  } else if (data.isDeclining && cfg.alertOnDeclining) {
    // Significant declining trend
    alert = BATTERY_ALERT_DECLINING;
  } else if (data.voltage >= cfg.normalVoltage && gLastBatteryAlert != BATTERY_ALERT_NONE) {
    // Battery recovered to normal
    if (cfg.alertOnRecovery) {
      alert = BATTERY_ALERT_RECOVERED;
    }
  }
  
  // Send alert if needed (with rate limiting)
  if (alert != BATTERY_ALERT_NONE) {
    bool shouldSend = false;
    
    // Always send critical alerts immediately
    if (alert == BATTERY_ALERT_CRITICAL) {
      shouldSend = true;
    }
    // For other alerts, check rate limiting
    else if (alert != gLastBatteryAlert || 
             now - gLastBatteryAlarmMillis >= BATTERY_ALARM_MIN_INTERVAL_MS) {
      shouldSend = true;
    }
    
    if (shouldSend && gNotecardAvailable) {
      sendBatteryAlarm(alert, data.voltage);
      gLastBatteryAlert = alert;
      gLastBatteryAlarmMillis = now;
      gLastBatteryAlertVoltage = data.voltage;
    }
  } else if (gLastBatteryAlert != BATTERY_ALERT_NONE && data.voltage >= cfg.normalVoltage) {
    // Clear alert state when voltage returns to normal
    gLastBatteryAlert = BATTERY_ALERT_NONE;
  }
}

/**
 * Send battery voltage alert to server.
 */
static void sendBatteryAlarm(BatteryAlertType alertType, float voltage) {
  const char *alertDesc;
  switch (alertType) {
    case BATTERY_ALERT_LOW:       alertDesc = "Battery voltage low"; break;
    case BATTERY_ALERT_CRITICAL:  alertDesc = "Battery voltage CRITICAL"; break;
    case BATTERY_ALERT_HIGH:      alertDesc = "Battery overvoltage"; break;
    case BATTERY_ALERT_DECLINING: alertDesc = "Battery voltage declining"; break;
    case BATTERY_ALERT_RECOVERED: alertDesc = "Battery voltage recovered"; break;
    default:                      alertDesc = "Battery alert"; break;
  }
  
  Serial.print(F("Battery alert: "));
  Serial.print(alertDesc);
  Serial.print(F(" ("));
  Serial.print(voltage, 2);
  Serial.println(F("V)"));
  
  char logMsg[128];
  snprintf(logMsg, sizeof(logMsg), "Battery: %s (%.2fV)", alertDesc, voltage);
  addSerialLog(logMsg);
  
  if (!gNotecardAvailable) {
    Serial.println(F("Network offline - battery alarm not sent"));
    return;
  }
  
  JsonDocument doc;
  doc["c"] = gDeviceUID;
  doc["s"] = gConfig.siteName;
  doc["y"] = "battery";
  doc["t"] = currentEpoch();
  
  // Alert type ("desc" and "state" omitted — derivable from alert + voltage on server)
  switch (alertType) {
    case BATTERY_ALERT_LOW:       doc["alert"] = "low"; break;
    case BATTERY_ALERT_CRITICAL:  doc["alert"] = "critical"; break;
    case BATTERY_ALERT_HIGH:      doc["alert"] = "high"; break;
    case BATTERY_ALERT_DECLINING: doc["alert"] = "declining"; break;
    case BATTERY_ALERT_RECOVERED: doc["alert"] = "recovered"; break;
    default:                      doc["alert"] = "unknown"; break;
  }
  
  // Voltage only — server can derive state description and SOC
  doc["v"] = roundTo(voltage, 2);
  
  // Include weekly trend only if meaningful
  if (gBatteryData.valid && gBatteryData.weeklyChange != 0.0f) {
    doc["weekly"] = roundTo(gBatteryData.weeklyChange, 2);
  }
  
  // SMS escalation for critical alerts only
  if (alertType == BATTERY_ALERT_CRITICAL) {
    doc["se"] = true;
  }
  
  publishNote(ALARM_FILE, doc, true);
  Serial.println(F("Battery alarm sent to server"));
}

/**
 * Append battery voltage data to daily report.
 */
static bool appendBatteryDataToDaily(JsonDocument &doc) {
  if (!gConfig.batteryMonitor.enabled || !gConfig.batteryMonitor.includeInDailyReport) {
    return false;
  }
  
  if (!gBatteryData.valid) {
    // Try to get fresh data
    pollBatteryVoltage(gBatteryData, gConfig.batteryMonitor);
  }
  
  if (!gBatteryData.valid) {
    return false;
  }
  
  JsonObject battery = doc["battery"].to<JsonObject>();
  
  // Current voltage (state/healthy derivable from voltage + config thresholds on server)
  battery["v"] = roundTo(gBatteryData.voltage, 2);

  // Fix 11: tag the active source ("mppt" | "vin-divider") so the server can distinguish
  // batteryMonitor readings from the legacy Notecard card.voltage readings in historical data.
  if (gBatteryData.mode[0] != '\0') {
    battery["src"] = (const char *)gBatteryData.mode;
  }
  
  // Stats over analysis period
  if (gBatteryData.voltageMin > 0.0f) {
    battery["vMin"] = roundTo(gBatteryData.voltageMin, 2);
    battery["vMax"] = roundTo(gBatteryData.voltageMax, 2);
    // vAvg omitted — derivable from vMin/vMax approximation on server
  }
  
  // Trend data (only include non-zero trends)
  if (gBatteryData.weeklyChange != 0.0f) {
    battery["weekly"] = roundTo(gBatteryData.weeklyChange, 2);
  }
  
  // Omitted: state (string), healthy (bool), daily/monthly trends, soc, uptime
  // Server derives these from voltage + configured thresholds
  
  return true;
}

// ============================================================================
// Power Conservation State Machine
// Progressive duty-cycle reduction with hysteresis-based recovery.
// Driven by getEffectiveBatteryVoltage() (SunSaver MPPT > Vin divider; Fix 9/11) — the
// Notecard card.voltage rail is never considered.
// ============================================================================

/**
 * Get human-readable description of a power state.
 */
static const char* getPowerStateDescription(PowerState state) {
  switch (state) {
    case POWER_STATE_NORMAL:             return "NORMAL";
    case POWER_STATE_ECO:                return "ECO";
    case POWER_STATE_LOW_POWER:          return "LOW_POWER";
    case POWER_STATE_CRITICAL_HIBERNATE: return "CRITICAL_HIBERNATE";
    default:                             return "UNKNOWN";
  }
}

/**
 * Get the loop sleep duration (ms) for a given power state.
 */
static unsigned long getPowerStateSleepMs(PowerState state) {
  switch (state) {
    case POWER_STATE_ECO:                return POWER_ECO_SLEEP_MS;
    case POWER_STATE_LOW_POWER:          return POWER_LOW_SLEEP_MS;
    case POWER_STATE_CRITICAL_HIBERNATE: return POWER_CRITICAL_SLEEP_MS;
    case POWER_STATE_NORMAL:
    default:                             return POWER_NORMAL_SLEEP_MS;
  }
}

/**
 * Send a server notification when the power state changes.
 * Logs the transition so the server has a clear record of hibernation
 * entry and exit times. Sends a "returning to normal" message on recovery.
 */
static void sendPowerStateChange(PowerState oldState, PowerState newState, float voltage) {
  const char *oldDesc = getPowerStateDescription(oldState);
  const char *newDesc = getPowerStateDescription(newState);
  
  // Determine if this is an improvement (recovering) or degradation
  bool recovering = (newState < oldState);
  
  // Build alert description
  char alertDesc[96];
  if (recovering) {
    snprintf(alertDesc, sizeof(alertDesc), "Power recovered: %s -> %s (%.2fV)", oldDesc, newDesc, voltage);
  } else {
    snprintf(alertDesc, sizeof(alertDesc), "Power reduced: %s -> %s (%.2fV)", oldDesc, newDesc, voltage);
  }
  
  Serial.print(F("Power state change: "));
  Serial.println(alertDesc);
  addSerialLog(alertDesc);
  
  if (!gNotecardAvailable) {
    Serial.println(F("Network offline - power state change not sent"));
    return;
  }
  
  JsonDocument doc;
  doc["c"] = gDeviceUID;
  doc["s"] = gConfig.siteName;
  doc["y"] = "power";
  doc["t"] = currentEpoch();
  
  // State transition (compact: "from"/"to" encode direction, no need for "recovering" or "desc")
  doc["from"] = oldDesc;
  doc["to"] = newDesc;
  doc["v"] = roundTo(voltage, 2);
  
  // Duration in previous state (seconds) — only if meaningful
  if (gPowerStateChangeMillis > 0) {
    doc["dur"] = (millis() - gPowerStateChangeMillis) / 1000UL;
  }
  
  // SMS escalation for critical hibernation entry only
  if (newState == POWER_STATE_CRITICAL_HIBERNATE && !recovering) {
    doc["se"] = true;
  }
  
  publishNote(ALARM_FILE, doc, true);
  Serial.println(F("Power state change sent to server"));
}

// ============================================================================
// Solar-Only (No Battery) Mode Implementation
// ============================================================================

/**
 * Load persisted solar-only state from flash.
 * Restores last report epoch and boot count across power cycles.
 */
static void loadSolarStateFromFlash() {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) return;
    FILE *file = fopen(SOLAR_STATE_FILE, "r");
    if (!file) {
      Serial.println(F("No solar state file found - first boot"));
      return;
    }
    char buf[256];
    size_t len = fread(buf, 1, sizeof(buf) - 1, file);
    fclose(file);
    buf[len] = '\0';
    
    JsonDocument doc;
    if (deserializeJson(doc, buf) == DeserializationError::Ok) {
      gSolarOnlyLastReportEpoch = doc["lastReportEpoch"].is<double>() ? doc["lastReportEpoch"].as<double>() : 0.0;
      gSolarOnlyBootCount = doc["bootCount"].is<unsigned long>() ? doc["bootCount"].as<unsigned long>() : 0;
      gSolarOnlyBatteryFailed = doc["batteryFailed"].is<bool>() ? doc["batteryFailed"].as<bool>() : false;
      gSolarOnlySunsetActive = doc["sunsetActive"].is<bool>() ? doc["sunsetActive"].as<bool>() : false;
      Serial.println(F("Solar state loaded from flash"));
    }
  #else
    // STM32 LittleFS path
    if (!LittleFS.exists(SOLAR_STATE_FILE)) {
      Serial.println(F("No solar state file found - first boot"));
      return;
    }
    File file = LittleFS.open(SOLAR_STATE_FILE, "r");
    if (!file) {
      Serial.println(F("No solar state file found - first boot"));
      return;
    }
    char buf[256];
    size_t len = file.readBytes(buf, sizeof(buf) - 1);
    file.close();
    buf[len] = '\0';
    
    JsonDocument doc;
    if (deserializeJson(doc, buf) == DeserializationError::Ok) {
      gSolarOnlyLastReportEpoch = doc["lastReportEpoch"].is<double>() ? doc["lastReportEpoch"].as<double>() : 0.0;
      gSolarOnlyBootCount = doc["bootCount"].is<unsigned long>() ? doc["bootCount"].as<unsigned long>() : 0;
      gSolarOnlyBatteryFailed = doc["batteryFailed"].is<bool>() ? doc["batteryFailed"].as<bool>() : false;
      gSolarOnlySunsetActive = doc["sunsetActive"].is<bool>() ? doc["sunsetActive"].as<bool>() : false;
      Serial.println(F("Solar state loaded from flash (LittleFS)"));
    }
  #endif
#endif
}

/**
 * Save solar-only state to flash for persistence across power cycles.
 */
static void saveSolarStateToFlash() {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) return;
    JsonDocument doc;
    doc["lastReportEpoch"] = gSolarOnlyLastReportEpoch;
    doc["bootCount"] = gSolarOnlyBootCount;
    doc["batteryFailed"] = gSolarOnlyBatteryFailed;
    doc["sunsetActive"] = gSolarOnlySunsetActive;
    
    char buf[256];
    size_t len = serializeJson(doc, buf, sizeof(buf));
    
    // Use atomic write to prevent corruption on power loss/brownout
    if (!tankalarm_posix_write_file_atomic(SOLAR_STATE_FILE, buf, len)) {
      Serial.println(F("Warning: Failed to save solar state"));
    }
  #else
    // STM32 LittleFS path
    JsonDocument doc;
    doc["lastReportEpoch"] = gSolarOnlyLastReportEpoch;
    doc["bootCount"] = gSolarOnlyBootCount;
    doc["batteryFailed"] = gSolarOnlyBatteryFailed;
    doc["sunsetActive"] = gSolarOnlySunsetActive;
    
    char buf[256];
    size_t len = serializeJson(doc, buf, sizeof(buf));
    
    if (!tankalarm_littlefs_write_file_atomic(SOLAR_STATE_FILE,
            (const uint8_t *)buf, len)) {
      Serial.println(F("Warning: Failed to save solar state (LittleFS)"));
    }
  #endif
#endif
}

/**
 * Detect whether the firmware was just installed (version changed since the last boot).
 *
 * Compares the running FIRMWARE_BUILD_SEQ against a small marker file persisted on the app
 * data partition. On a mismatch (a fresh OTA or USB flash, or the very first boot of this
 * firmware) it sets gFirmwareJustUpdated and records the new sequence so the check fires
 * only once per update. setup() uses the flag to force an immediate confirmation telemetry
 * sync, so an operator can verify two-way communication right after an update instead of
 * waiting for the next periodic/daily sync window (up to 6 h on solar clients).
 */
static void checkFirmwareUpdateMarker() {
#ifdef FILESYSTEM_AVAILABLE
  uint16_t storedSeq = 0;
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) return;
    FILE *file = fopen(FIRMWARE_MARKER_FILE, "r");
    if (file) {
      char buf[96];
      size_t len = fread(buf, 1, sizeof(buf) - 1, file);
      fclose(file);
      buf[len] = '\0';
      JsonDocument doc;
      if (deserializeJson(doc, buf) == DeserializationError::Ok) {
        storedSeq = doc["seq"].is<uint16_t>() ? doc["seq"].as<uint16_t>() : 0;
      }
    }
  #else
    if (LittleFS.exists(FIRMWARE_MARKER_FILE)) {
      File file = LittleFS.open(FIRMWARE_MARKER_FILE, "r");
      if (file) {
        char buf[96];
        size_t len = file.readBytes(buf, sizeof(buf) - 1);
        file.close();
        buf[len] = '\0';
        JsonDocument doc;
        if (deserializeJson(doc, buf) == DeserializationError::Ok) {
          storedSeq = doc["seq"].is<uint16_t>() ? doc["seq"].as<uint16_t>() : 0;
        }
      }
    }
  #endif

  if (storedSeq == (uint16_t)FIRMWARE_BUILD_SEQ) {
    return;  // Same firmware as the last boot — no confirmation needed.
  }

  gFirmwareJustUpdated = true;
  Serial.print(F("Firmware change detected (stored seq "));
  Serial.print(storedSeq);
  Serial.print(F(" -> running seq "));
  Serial.print(FIRMWARE_BUILD_SEQ);
  Serial.println(F("); will send immediate confirmation telemetry"));

  // Record the running build sequence so this only fires once per update.
  JsonDocument out;
  out["seq"] = (uint16_t)FIRMWARE_BUILD_SEQ;
  out["v"] = FIRMWARE_VERSION;
  char obuf[96];
  size_t olen = serializeJson(out, obuf, sizeof(obuf));
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!tankalarm_posix_write_file_atomic(FIRMWARE_MARKER_FILE, obuf, olen)) {
      Serial.println(F("Warning: failed to write firmware marker"));
    }
  #else
    if (!tankalarm_littlefs_write_file_atomic(FIRMWARE_MARKER_FILE,
            (const uint8_t *)obuf, olen)) {
      Serial.println(F("Warning: failed to write firmware marker (LittleFS)"));
    }
  #endif
#endif
}

/**
 * Perform startup debounce for solar-only mode.
 * Blocks in setup() until power is deemed stable enough to operate.
 *
 * With Vin divider: waits until Vin >= startupDebounceVoltage for startupDebounceSec continuously.
 *   Safety timeout: aborts after STARTUP_DEBOUNCE_MAX_WAIT_MS (default 5 minutes) if voltage
 *   never stabilizes, to prevent infinite blocking on brownout/bad-wiring conditions.
 * Without Vin divider: waits startupWarmupSec as a fixed timer.
 */
// Maximum time to wait for voltage-based startup debounce before giving up (5 minutes)
#ifndef STARTUP_DEBOUNCE_MAX_WAIT_MS
#define STARTUP_DEBOUNCE_MAX_WAIT_MS (5UL * 60UL * 1000UL)
#endif

static void performStartupDebounce() {
  Serial.println(F("Solar-only: startup debounce..."));
  
  if (gConfig.vinMonitor.enabled) {
    // Voltage-based debounce: wait for stable voltage above threshold
    float debounceV = gConfig.solarOnlyConfig.startupDebounceVoltage;
    uint16_t debounceSec = gConfig.solarOnlyConfig.startupDebounceSec;
    unsigned long debounceMs = (unsigned long)debounceSec * 1000UL;
    unsigned long stableStart = 0;
    bool stable = false;
    unsigned long debounceLoopStart = millis();
    
    Serial.print(F("  Waiting for Vin >= "));
    Serial.print(debounceV, 1);
    Serial.print(F("V for "));
    Serial.print(debounceSec);
    Serial.print(F("s (max wait: "));
    Serial.print(STARTUP_DEBOUNCE_MAX_WAIT_MS / 60000UL);
    Serial.println(F(" min)"));
    
    while (!stable) {
      // Safety timeout: prevent infinite blocking if voltage never stabilizes
      if (millis() - debounceLoopStart >= STARTUP_DEBOUNCE_MAX_WAIT_MS) {
        Serial.print(F("  WARNING: Startup debounce timeout after "));
        Serial.print(STARTUP_DEBOUNCE_MAX_WAIT_MS / 60000UL);
        Serial.print(F(" min (Vin="));
        Serial.print(gVinVoltage, 2);
        Serial.println(F("V) — proceeding with degraded power"));
        addSerialLog("Startup debounce timeout - proceeding anyway");
        break;
      }
      
      gVinVoltage = readVinDividerVoltage();
      
      if (gVinVoltage >= debounceV) {
        if (stableStart == 0) {
          stableStart = millis();
        } else if (millis() - stableStart >= debounceMs) {
          stable = true;
        }
      } else {
        stableStart = 0;  // Reset — voltage dropped
      }
      
      if (!stable) {
        safeSleep(2000);
      }
    }
    
    if (stable) {
      Serial.print(F("  Stable at "));
      Serial.print(gVinVoltage, 2);
      Serial.println(F("V"));
    }
    gSolarOnlyStartupComplete = true;
    
    // Check if voltage is high enough for sensors
    gSolarOnlySensorsReady = (gVinVoltage >= gConfig.solarOnlyConfig.sensorGateVoltage);
  } else {
    // Timer-based warmup: wait fixed duration without voltage feedback
    uint16_t warmupSec = gConfig.solarOnlyConfig.startupWarmupSec;
    Serial.print(F("  Timer warmup: "));
    Serial.print(warmupSec);
    Serial.println(F("s"));
    
    unsigned long warmupMs = (unsigned long)warmupSec * 1000UL;
    unsigned long warmupStart = millis();
    
    while (millis() - warmupStart < warmupMs) {
      safeSleep(2000);
    }
    
    Serial.println(F("  Warmup complete"));
    gSolarOnlyStartupComplete = true;
    gSolarOnlySensorsReady = true;  // Assume OK without voltage info
  }
  
  addSerialLog("Solar-only startup debounce complete");
}

/**
 * Check if the sensor voltage gate is open (sufficient power for sensors).
 * With Vin divider: checks Vin against sensorGateVoltage.
 * Without Vin divider: returns true after startup debounce is complete.
 */
static bool isSensorVoltageGateOpen() {
  if (!isSolarOnlyActive()) return true;
  if (!gSolarOnlyStartupComplete) return false;
  
  if (gConfig.vinMonitor.enabled && gVinVoltage > 0.5f) {
    bool gateOpen = (gVinVoltage >= gConfig.solarOnlyConfig.sensorGateVoltage);
    if (gateOpen != gSolarOnlySensorsReady) {
      gSolarOnlySensorsReady = gateOpen;
      if (gateOpen) {
        Serial.println(F("Solar-only: sensors ready (voltage gate open)"));
      } else {
        Serial.println(F("Solar-only: sensors gated (voltage too low)"));
      }
    }
    return gateOpen;
  }
  
  // Without Vin divider, assume sensors are ready after warmup
  return gSolarOnlySensorsReady;
}

/**
 * Sunset protocol: detect declining voltage and save state before power loss.
 * Called every loop iteration when solar-only mode is active.
 *
 * With Vin divider: detects voltage dropping below sunsetVoltage with declining trend.
 * Without Vin divider: no sunset detection possible (relies on graceful power loss handling).
 */
static void checkSolarOnlySunsetProtocol(unsigned long now) {
  // Only works with Vin divider — without it, we can't detect sunset
  if (!gConfig.vinMonitor.enabled || gVinVoltage <= 0.5f) return;
  
  float sunsetV = gConfig.solarOnlyConfig.sunsetVoltage;
  uint16_t confirmSec = gConfig.solarOnlyConfig.sunsetConfirmSec;
  
  if (gVinVoltage < sunsetV && gVinVoltage <= gSolarOnlyLastVin) {
    // Voltage is below sunset threshold AND declining
    if (!gSolarOnlySunsetActive) {
      gSolarOnlySunsetActive = true;
      gSolarOnlySunsetStart = now;
      saveSolarStateToFlash();  // Persist sunset-active flag for reboot recovery
      Serial.print(F("Solar-only: sunset protocol started (Vin="));
      Serial.print(gVinVoltage, 2);
      Serial.println(F("V)"));
    } else if (now - gSolarOnlySunsetStart >= (unsigned long)confirmSec * 1000UL) {
      // Confirmed declining — save state and prepare for shutdown
      if (!gSolarOnlyStateSaved) {
        gSolarOnlyStateSaved = true;
        Serial.println(F("Solar-only: sunset confirmed — saving state"));
        addSerialLog("Sunset protocol: saving state");
        
        // Save state to flash
        saveSolarStateToFlash();
        
        // Flush any pending telemetry — verify sync accepted before shutdown
        if (gNotecardAvailable) {
          J *req = notecard.newRequest("hub.sync");
          if (req) {
            J *syncRsp = notecard.requestAndResponse(req);
            if (syncRsp) {
              const char *syncErr = JGetString(syncRsp, "err");
              if (syncErr && syncErr[0] != '\0') {
                Serial.print(F("  WARNING: hub.sync failed: "));
                Serial.println(syncErr);
              } else {
                Serial.println(F("  Flushed pending data"));
              }
              notecard.deleteResponse(syncRsp);
            } else {
              Serial.println(F("  WARNING: hub.sync no response"));
            }
          }
        }
        
        // Send a sunset notification to the server
        if (gNotecardAvailable) {
          JsonDocument doc;
          doc["c"] = gDeviceUID;
          doc["s"] = gConfig.siteName;
          doc["y"] = "solar_sunset";
          doc["t"] = currentEpoch();
          doc["v"] = roundTo(gVinVoltage, 2);
          doc["bootCount"] = gSolarOnlyBootCount;
          doc["uptime"] = millis() / 1000UL;
          publishNote(ALARM_FILE, doc, true);
        }
      }
    }
  } else if (gVinVoltage >= sunsetV) {
    // Voltage recovered above sunset threshold — cancel protocol
    if (gSolarOnlySunsetActive) {
      gSolarOnlySunsetActive = false;
      gSolarOnlyStateSaved = false;
      gSolarOnlySunsetStart = 0;
      Serial.println(F("Solar-only: sunset protocol cancelled (voltage recovered)"));
    }
  }
  
  gSolarOnlyLastVin = gVinVoltage;
}

/**
 * Determine the best available battery voltage from all monitoring sources.
 * Uses STRICT PRIORITY (Fix 9): the highest-priority source that has valid recent data
 * wins; lower-priority sources are NEVER allowed to override it, even if they read lower.
 * The previous "lower of" logic let a noisy ADC divider drag a clean MPPT reading down
 * and trip a bogus CRITICAL_HIBERNATE. Records the winning source in
 * gEffectiveVoltageSource (Fix 7) so telemetry/daily can tag it.
 *
 * Sources (in strict priority order):
 *   1. SunSaver MPPT via Modbus RS-485 (most accurate, digital charge-controller reading)
 *   2. Analog Vin voltage divider (direct ADC of battery via resistor divider, less accurate)
 *
 * NOTE (Fix 9 update): the Notecard card.voltage is NEVER used as a battery source on ANY
 * platform. On the Blues "Wireless for Opta" carrier the Notecard V+ is the regulated ~5V
 * DC-DC rail and physically cannot read the 12V battery (would misreport and trip a bogus
 * CRITICAL_HIBERNATE). Even on hypothetical hardware where the Notecard V+ would be the
 * battery, the SunSaver MPPT and the analog Vin divider are the only sanctioned battery
 * sources — the Notecard path is deliberately removed from this function so no future
 * configuration or wiring change can resurrect it as a power-state input.
 */
static float getEffectiveBatteryVoltage() {
  float voltage = 0.0f;
  bool hasVoltage = false;
  const char *source = nullptr;
  
  // Source 1 (HIGHEST PRIORITY): SunSaver MPPT via Modbus RS-485.
  // The MPPT is an industrial digital charge controller that measures the battery terminal
  // directly and reports over RS-485. It is the most accurate source available, so when it is
  // present and healthy NOTHING is allowed to override it (Fix 9). The previous "use the
  // lower of" logic let a noisy/uncalibrated Vin divider drag a clean 12.3V MPPT reading
  // down to e.g. 11.9V and trip a bogus CRITICAL_HIBERNATE.
  if (gSolarManager.isEnabled() && gSolarManager.isCommunicationOk()) {
    const SolarData &solar = gSolarManager.getData();
    if (solar.batteryVoltage > 0.1f) {
      voltage = solar.batteryVoltage;
      hasVoltage = true;
      source = "mppt";
    }
  }
  
  // Source 2 (FALLBACK): analog Vin voltage divider.
  // Only consulted when MPPT did not provide a valid reading. ADC dividers are subject to
  // component tolerance, supply noise, and temperature drift, so they are a fallback — never
  // an override (Fix 9).
  if (!hasVoltage && gConfig.vinMonitor.enabled && gVinVoltage > 0.5f) {
    voltage = gVinVoltage;
    hasVoltage = true;
    source = "vin-divider";
  }
  
  // Notecard card.voltage is intentionally NOT considered here on any platform (Fix 9
  // update). The Notecard battery monitor (pollBatteryVoltage / gBatteryData) now sources
  // from this same function via Fix 11, so all voltage paths agree on which physical sensor
  // is the truth — there is no longer a separate Notecard-derived reading anywhere.
  
  gEffectiveVoltageSource = hasVoltage ? source : nullptr;
  return hasVoltage ? voltage : 0.0f;
}

/**
 * Evaluate battery voltage and update the power conservation state.
 * Uses hysteresis thresholds to prevent rapid oscillation:
 *   - Enter a worse state at the ENTER threshold (falling voltage)
 *   - Exit back to a better state at the EXIT threshold (rising voltage, higher than enter)
 * Requires POWER_STATE_DEBOUNCE_COUNT consecutive readings at the new state before transitioning.
 *
 * Called once per loop iteration, after battery/solar polling.
 */
static void updatePowerState() {
  float voltage = getEffectiveBatteryVoltage();
  
  // If no battery monitoring is active, stay in NORMAL
  if (voltage <= 0.0f) {
    gPowerState = POWER_STATE_NORMAL;
    return;
  }
  
  gEffectiveBatteryVoltage = voltage;
  
  // Use remote-tunable thresholds if configured, otherwise compile-time defaults
  const float ecoEnter      = (gConfig.powerEcoEnterV > 0.0f) ? gConfig.powerEcoEnterV : POWER_ECO_ENTER_VOLTAGE;
  const float lowEnter      = (gConfig.powerLowEnterV > 0.0f) ? gConfig.powerLowEnterV : POWER_LOW_ENTER_VOLTAGE;
  const float criticalEnter = (gConfig.powerCriticalEnterV > 0.0f) ? gConfig.powerCriticalEnterV : POWER_CRITICAL_ENTER_VOLTAGE;
  const float ecoExit       = (gConfig.powerEcoExitV > 0.0f) ? gConfig.powerEcoExitV : POWER_ECO_EXIT_VOLTAGE;
  const float lowExit       = (gConfig.powerLowExitV > 0.0f) ? gConfig.powerLowExitV : POWER_LOW_EXIT_VOLTAGE;
  const float criticalExit  = (gConfig.powerCriticalExitV > 0.0f) ? gConfig.powerCriticalExitV : POWER_CRITICAL_EXIT_VOLTAGE;
  
  // Determine the proposed state based on current voltage and hysteresis direction
  PowerState proposed;
  
  if (gPowerState == POWER_STATE_NORMAL) {
    // Currently NORMAL — check if we should degrade
    if (voltage < criticalEnter) {
      proposed = POWER_STATE_CRITICAL_HIBERNATE;
    } else if (voltage < lowEnter) {
      proposed = POWER_STATE_LOW_POWER;
    } else if (voltage < ecoEnter) {
      proposed = POWER_STATE_ECO;
    } else {
      proposed = POWER_STATE_NORMAL;
    }
  } else if (gPowerState == POWER_STATE_ECO) {
    // Currently ECO — can degrade further or recover
    if (voltage < criticalEnter) {
      proposed = POWER_STATE_CRITICAL_HIBERNATE;
    } else if (voltage < lowEnter) {
      proposed = POWER_STATE_LOW_POWER;
    } else if (voltage >= ecoExit) {
      proposed = POWER_STATE_NORMAL;  // Recovered
    } else {
      proposed = POWER_STATE_ECO;     // Stay in ECO
    }
  } else if (gPowerState == POWER_STATE_LOW_POWER) {
    // Currently LOW_POWER — can degrade to CRITICAL or recover
    if (voltage < criticalEnter) {
      proposed = POWER_STATE_CRITICAL_HIBERNATE;
    } else if (voltage >= lowExit) {
      proposed = POWER_STATE_ECO;     // Step up one level (not straight to NORMAL)
    } else {
      proposed = POWER_STATE_LOW_POWER;
    }
  } else {
    // Currently CRITICAL_HIBERNATE — only recover if voltage is high enough
    if (voltage >= criticalExit) {
      proposed = POWER_STATE_LOW_POWER;  // Step up one level at a time
    } else {
      proposed = POWER_STATE_CRITICAL_HIBERNATE;
    }
  }
  
  // Debounce: require consecutive readings at the proposed new state
  if (proposed != gPowerState) {
    gPowerStateDebounce++;
    if (gPowerStateDebounce >= POWER_STATE_DEBOUNCE_COUNT) {
      // State transition confirmed
      PowerState oldState = gPowerState;
      gPowerState = proposed;
      gPowerStateDebounce = 0;
      bool powerChangeSent = false;
      
      // Handle relay safety on entering CRITICAL
      if (gPowerState == POWER_STATE_CRITICAL_HIBERNATE) {
        sendPowerStateChange(oldState, gPowerState, voltage);
        powerChangeSent = true;
        // De-energize all relays to eliminate coil current draw (~100mA each)
        for (uint8_t i = 0; i < MAX_RELAYS; i++) {
          setRelayState(i, false);
        }
        Serial.println(F("CRITICAL HIBERNATE: All relays de-energized for battery protection"));
        addSerialLog("Relays off - critical battery");
      }
      
      // BugFix 04022026 (MED-15): Restore relay state when recovering FROM critical hibernate.
      // Relays forced OFF during hibernate should be reactivated if the alarm condition
      // that triggered them (UNTIL_CLEAR / MANUAL_RESET) is still latched.
      if (oldState == POWER_STATE_CRITICAL_HIBERNATE && gPowerState != POWER_STATE_CRITICAL_HIBERNATE) {
        for (uint8_t i = 0; i < gConfig.monitorCount; i++) {
          const MonitorConfig &cfg = gConfig.monitors[i];
          if (cfg.relayTargetClient[0] == '\0' || cfg.relayMask == 0) continue;
          if (cfg.relayMode == RELAY_MODE_MOMENTARY) continue;  // Momentary expired during hibernate
          
          bool alarmStillActive = gMonitorState[i].highAlarmLatched || gMonitorState[i].lowAlarmLatched;
          if (alarmStillActive && isMonitorRelayActive(i)) {
            triggerRemoteRelays(cfg.relayTargetClient, cfg.relayMask, true);
            activateRelayForMonitor(i, cfg.relayMask, RELAY_SRC_ALARM, millis());
            Serial.print(F("Relay restored after hibernate recovery for monitor "));
            Serial.println(cfg.name);
          }
        }
      }
      
      // Notify server of the state change (entry or recovery)
      if (!powerChangeSent) {
        sendPowerStateChange(oldState, gPowerState, voltage);
      }
      unsigned long transitionNow = millis();
      gPowerStateChangeMillis = transitionNow;
      gPreviousPowerState = oldState;
      gLastPowerStateLogMillis = transitionNow;
      
      // Log transition with rate limiting to prevent serial/log flooding near thresholds
      if (gLastPowerStateTransitionLogMillis == 0 ||
          (transitionNow - gLastPowerStateTransitionLogMillis) >= POWER_STATE_TRANSITION_LOG_MIN_MS) {
        gLastPowerStateTransitionLogMillis = transitionNow;
        Serial.print(F("Power state: "));
        Serial.print(getPowerStateDescription(oldState));
        Serial.print(F(" -> "));
        Serial.print(getPowerStateDescription(gPowerState));
        Serial.print(F(" ("));
        Serial.print(voltage, 2);
        Serial.println(F("V)"));
      }
    }
  } else {
    gPowerStateDebounce = 0;  // Reset debounce if proposed matches current
  }
  
  // Periodic power state log (every 30 minutes, only when not NORMAL)
  if (gPowerState != POWER_STATE_NORMAL) {
    unsigned long now = millis();
    if (now - gLastPowerStateLogMillis >= POWER_STATE_PERIODIC_LOG_MS) {
      gLastPowerStateLogMillis = now;
      char logMsg[96];
      snprintf(logMsg, sizeof(logMsg), "Power: %s (%.2fV, sleep=%lums)", 
               getPowerStateDescription(gPowerState), voltage, getPowerStateSleepMs(gPowerState));
      Serial.println(logMsg);
      addSerialLog(logMsg);
    }
  }
  
  // Battery failure fallback: for solar+battery setups, detect persistent critical voltage
  // and auto-enable solar-only behaviors (opportunistic reporting, sunset protocol)
  if (gConfig.solarOnlyConfig.batteryFailureFallback && !gConfig.solarOnlyConfig.enabled) {
    if (gPowerState == POWER_STATE_CRITICAL_HIBERNATE) {
      gSolarOnlyBatFailCount++;
      gSolarOnlyBatFailLastIncrMillis = millis();
      if (gSolarOnlyBatFailCount >= gConfig.solarOnlyConfig.batteryFailureThreshold && !gSolarOnlyBatteryFailed) {
        gSolarOnlyBatteryFailed = true;
        gSolarOnlyStartupComplete = true;  // Already running, no debounce needed
        gSolarOnlySensorsReady = true;
        Serial.println(F("BATTERY FAILURE DETECTED — enabling solar-only fallback behaviors"));
        addSerialLog("Battery failure: solar-only fallback active");
        
        // Notify server
        if (gNotecardAvailable) {
          JsonDocument doc;
          doc["c"] = gDeviceUID;
          doc["s"] = gConfig.siteName;
          doc["y"] = "battery_failure";
          doc["t"] = currentEpoch();
          doc["v"] = roundTo(voltage, 2);
          doc["failCount"] = gSolarOnlyBatFailCount;
          doc["se"] = true;  // Escalate via SMS
          publishNote(ALARM_FILE, doc, true);
        }
        
        // Load solar state if not already loaded
        loadSolarStateFromFlash();
        saveSolarStateToFlash();
      }
    } else if (gPowerState < POWER_STATE_CRITICAL_HIBERNATE) {
      // Battery exited CRITICAL — apply time-based decay instead of immediate reset.
      // A battery oscillating between CRITICAL and LOW_POWER within 24 hours will
      // correctly accumulate counts toward the failure threshold. Only decay the
      // counter after 24 hours without a CRITICAL reading, preventing slow
      // accumulation over days/weeks of borderline voltage from triggering a false
      // state transition.
      if (gSolarOnlyBatFailCount > 0 && 
          (millis() - gSolarOnlyBatFailLastIncrMillis >= SOLAR_BAT_FAIL_DECAY_MS)) {
        Serial.println(F("Solar bat-fail counter decayed (24h without increment)"));
        gSolarOnlyBatFailCount = 0;
      }
      if (gSolarOnlyBatteryFailed && gPowerState <= POWER_STATE_ECO) {
        // Full recovery to ECO or better — deactivate fallback
        gSolarOnlyBatteryFailed = false;
        Serial.println(F("Battery recovered — solar-only fallback deactivated"));
        addSerialLog("Battery recovered: normal mode restored");
        saveSolarStateToFlash();
      }
    }
  }
}

static void sendDailyReport() {
  uint8_t eligibleIndices[MAX_MONITORS];
  uint8_t eligibleCount = 0;
  for (uint8_t i = 0; i < gConfig.monitorCount; ++i) {
    if (gConfig.monitors[i].enableDailyReport) {
      eligibleIndices[eligibleCount++] = i;
    }
  }

  if (eligibleCount == 0) {
    return;
  }

  double reportEpoch = currentEpoch();
  size_t monitorCursor = 0;
  uint8_t part = 0;
  bool queuedAny = false;

  // Best available voltage for daily report:
  // Sourced strictly from getEffectiveBatteryVoltage() which maps to active Vin loop,
  // RS-485 Modbus, and batteryMonitor offsets. If none of these are configured,
  // we do not include a dummy/LDO Notecard reading (which reads 5V system LDO).
  float vinVoltage = getEffectiveBatteryVoltage();

  while (monitorCursor < eligibleCount) {
    JsonDocument doc;
    doc["c"] = gDeviceUID;
    doc["s"] = gConfig.siteName;
    doc["t"] = reportEpoch;
    doc["p"] = part;  // 0-based part number

    // Include VIN voltage in the first part of the daily report
    if (part == 0 && vinVoltage > 0.0f) {
      doc["v"] = vinVoltage;
      // Fix 7: tag the voltage source (MPPT or Vin divider) so the server can trust it.
      if (gEffectiveVoltageSource) doc["vs"] = gEffectiveVoltageSource;
    }
    
    // Include solar charger data in the first part of the daily report
    if (part == 0) {
      appendSolarDataToDaily(doc);
      // Fix 11: appendBatteryDataToDaily now sources from MPPT/Vin via
      // getEffectiveBatteryVoltage() and never touches the Notecard, so it is safe to run
      // on all platforms (the previous Fix 10 Opta compile guard is no longer needed).
      appendBatteryDataToDaily(doc);
      // Include power conservation state in daily report
      if (gPowerState != POWER_STATE_NORMAL) {
        JsonObject powerObj = doc["power"].to<JsonObject>();
        powerObj["state"] = getPowerStateDescription(gPowerState);
        powerObj["v"] = roundTo(gEffectiveBatteryVoltage, 2);
        powerObj["sleepMs"] = (long)getPowerStateSleepMs(gPowerState);
        if (gPowerStateChangeMillis > 0) {
          powerObj["stateDurSec"] = (millis() - gPowerStateChangeMillis) / 1000UL;
        }
      }
      // Include cellular signal strength (from last card.wireless check)
      if (gSignalBars >= 0) {
        JsonObject sigObj = doc["sig"].to<JsonObject>();
        sigObj["bars"] = gSignalBars;
        if (gSignalRssi != 0) sigObj["rssi"] = gSignalRssi;
        if (gSignalRsrp != 0) sigObj["rsrp"] = gSignalRsrp;
        if (gSignalRsrq != 0) sigObj["rsrq"] = gSignalRsrq;
        if (gSignalRat[0] != '\0') sigObj["rat"] = gSignalRat;
      }
      // Include active alarm summary as backup notification path.
      // If the original alarm note was lost due to weak signal, the server
      // can detect active alarms from this daily report instead.
      // BugFix 04022026: Only report alarms for monitors with alarmsEnabled.
      // Previously, stale latched state from before alarmsEnabled was disabled
      // would be reported, causing phantom alarms on the server dashboard.
      {
        // Always emit the alarms array in part 0 (schema 2+), even when empty. An empty
        // array is an explicit "no active alarms" signal the server uses to reconcile and
        // clear orphaned alarms whose clear note was lost. Omitting it entirely leaves the
        // server unable to distinguish "no active alarms" from "no alarm data in this report".
        JsonArray alarmsArr = doc["alarms"].to<JsonArray>();
        for (uint8_t ai = 0; ai < gConfig.monitorCount; ++ai) {
          if (gConfig.monitors[ai].alarmsEnabled &&
              (gMonitorState[ai].highAlarmLatched || gMonitorState[ai].lowAlarmLatched)) {
            JsonObject a = alarmsArr.add<JsonObject>();
            a["k"] = gConfig.monitors[ai].sensorIndex;
            a["hi"] = gMonitorState[ai].highAlarmLatched;
            a["lo"] = gMonitorState[ai].lowAlarmLatched;
          }
        }
      }
    }

    JsonArray sensors = doc["sensors"].to<JsonArray>();
    bool addedMonitor = false;
    bool partHasMetadata = (part == 0);  // part 0 carries the bulky VIN/solar/signal/alarm block

    while (monitorCursor < eligibleCount) {
      uint8_t monitorIdx = eligibleIndices[monitorCursor];
      if (appendDailyMonitor(doc, sensors, monitorIdx, DAILY_NOTE_PAYLOAD_LIMIT)) {
        ++monitorCursor;
        addedMonitor = true;
      } else {
        // This monitor does not fit in the current part.
        if (addedMonitor || partHasMetadata) {
          // The part already has content (a prior monitor and/or part-0 metadata). Publish it
          // as-is and retry THIS monitor in a fresh, metadata-free part. Crucially we do NOT
          // advance monitorCursor, so the monitor is never silently dropped — fixing the case
          // where bulky part-0 metadata could push the first monitor out of the report.
          break;
        }
        // Empty, metadata-free part: this single monitor is too large on its own. Try once
        // more with minimal headroom; if it still does not fit, skip it to avoid an infinite
        // loop (a pathological oversized monitor that cannot fit any part).
        if (appendDailyMonitor(doc, sensors, monitorIdx, DAILY_NOTE_PAYLOAD_LIMIT + 48U)) {
          ++monitorCursor;
          addedMonitor = true;
        } else {
          Serial.println(F("Daily report entry skipped; single monitor exceeds payload limit"));
          ++monitorCursor;
        }
        break;
      }
    }

    // Publish if the part carries any content: a monitor, or the part-0 metadata block.
    // A metadata-only part 0 is valid and lets a deferred monitor move to a clean part.
    if (!addedMonitor && !partHasMetadata) {
      continue;
    }

    doc["m"] = (monitorCursor < eligibleCount);  // more parts follow
    bool syncNow = (monitorCursor >= eligibleCount);
    publishNote(DAILY_FILE, doc, syncNow);
    queuedAny = true;
    ++part;
  }

  if (queuedAny) {
    Serial.println(F("Daily report queued"));
  }

  // I2C error rate alerting — notify operators of degrading connections
  // before they cause measurement gaps.
  if (gCurrentLoopI2cErrors >= I2C_ERROR_ALERT_THRESHOLD) {
    Serial.print(F("I2C: 24h error count "));
    Serial.print(gCurrentLoopI2cErrors);
    Serial.println(F(" exceeds alert threshold — publishing alarm"));
    JsonDocument doc;
    doc["c"] = gDeviceUID;
    doc["s"] = gConfig.siteName;
    doc["y"] = "i2c-error-rate";
    doc["errs"] = gCurrentLoopI2cErrors;
    doc["recs"] = gI2cBusRecoveryCount;
    doc["ok"] = gCurrentLoopReadsOk;
    doc["or"] = gCurrentLoopOverRange;
    doc["t"] = currentEpoch();
    publishNote(ALARM_FILE, doc, true);
  }

  // Reset I2C error counters after daily report so telemetry reflects
  // the recent 24-hour window rather than lifetime totals.
  gCurrentLoopI2cErrors = 0;
  gI2cBusRecoveryCount = 0;
  gCurrentLoopReadsOk = 0;
  gCurrentLoopOverRange = 0;
  // Fix S2 (v2.0.49): reset Modbus error counters too so "merr" in the next solar
  // block is a windowed 24-hour total instead of lifetime-since-boot. Matches the
  // adjacent I2C error reset pattern.
  gSolarManager.resetModbusErrorStats();
}

static bool appendDailyMonitor(JsonDocument &doc, JsonArray &array, uint8_t monitorIndex, size_t payloadLimit) {
  if (monitorIndex >= gConfig.monitorCount) {
    return false;
  }

  const MonitorConfig &cfg = gConfig.monitors[monitorIndex];
  MonitorRuntime &state = gMonitorState[monitorIndex];

  JsonObject t = array.add<JsonObject>();
  t["n"] = cfg.name;                              // label/name
  t["k"] = cfg.sensorIndex;                     // monitor number
  if (cfg.userNumber > 0) t["un"] = cfg.userNumber;

  // Object type, measurement unit, sensor type, raw reading, lvl, cap, cv.
  // Every daily monitor is now self-describing so the server can decode it with
  // zero registry state after a restart.
  buildSensorObject(t, monitorIndex);

  if (measureJson(doc) > payloadLimit) {
    size_t currentSize = array.size();
    if (currentSize > 0) {
      array.remove(currentSize - 1);
    }
    return false;
  }

  state.lastDailySentValue = state.currentInches;
  return true;
}

// Trim the telemetry.qo outbox so at most TELEMETRY_OUTBOX_MAX_PENDING notes are queued.
// If more are pending, the oldest notes are deleted to make room for the new one.
// This prevents unbounded queue growth when the Notecard cannot sync for an extended period.
// The loop retries if the initial collection window was exceeded (large backlog recovery).
// A hard pass limit and zero-deletion guard ensure the function exits gracefully if the
// Notecard returns errors so the main loop is never blocked indefinitely.
#ifndef TELEMETRY_TRIM_MAX_PASSES
#define TELEMETRY_TRIM_MAX_PASSES 10  // Max retry iterations when draining a large backlog
#endif
static void trimTelemetryOutbox() {
  if (!gNotecardAvailable) {
    return;
  }

  // Static buffer to avoid a stack allocation on every call.
  static const uint8_t MAX_IDS = TELEMETRY_OUTBOX_MAX_PENDING + TELEMETRY_TRIM_COLLECTION_HEADROOM;
  static char noteIds[MAX_IDS][TELEMETRY_NOTE_ID_LEN];

  uint8_t totalDeleted = 0;
  bool overflowed = true;  // start true to enter the loop at least once
  uint8_t passes = 0;

  // Retry if the queue exceeded MAX_IDS on the previous pass so that a large
  // backlog (e.g., after a long outage) is fully drained to the target limit.
  // TELEMETRY_TRIM_MAX_PASSES caps iterations so a persistent Notecard error
  // (e.g., I2C failure) cannot block the main loop indefinitely.
  while (overflowed && passes < TELEMETRY_TRIM_MAX_PASSES) {
    // Kick watchdog at top of each trim pass — each pass performs multiple
    // blocking I2C transactions (note.changes + N × note.delete).
    #ifdef TANKALARM_WATCHDOG_AVAILABLE
      #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
        mbedWatchdog.kick();
      #endif
    #endif
    overflowed = false;
    passes++;

    J *req = notecard.newRequest("note.changes");
    if (!req) {
      break;
    }
    JAddStringToObject(req, "file", TELEMETRY_FILE);

    J *rsp = notecard.requestAndResponse(req);
    if (!rsp) {
      break;
    }

    const char *err = JGetString(rsp, "err");
    if (err && err[0] != '\0') {
      notecard.deleteResponse(rsp);
      break;
    }

    J *notes = JGetObject(rsp, "notes");
    if (!notes) {
      notecard.deleteResponse(rsp);
      break;
    }

    // Collect IDs oldest-first; detect if the queue extends beyond our window.
    uint8_t count = 0;
    J *note = notes->child;
    while (note && count < MAX_IDS) {
      if (note->string) {
        strlcpy(noteIds[count], note->string, TELEMETRY_NOTE_ID_LEN);
        count++;
      }
      note = note->next;
    }
    if (note) {
      overflowed = true;  // more notes exist beyond MAX_IDS — loop again after deleting
    }
    notecard.deleteResponse(rsp);

    if (count < TELEMETRY_OUTBOX_MAX_PENDING) {
      break;  // Under the limit — nothing to do
    }

    // Delete the oldest notes, leaving TELEMETRY_OUTBOX_MAX_PENDING - 1 pending so that
    // the note about to be added brings the total exactly to TELEMETRY_OUTBOX_MAX_PENDING.
    // count == 15: toDelete = 1 → 14 pending → after add = 15 ✓
    // count == 17: toDelete = 3 → 14 pending → after add = 15 ✓
    uint8_t toDelete = count - TELEMETRY_OUTBOX_MAX_PENDING + 1;
    uint8_t deletedThisPass = 0;
    for (uint8_t i = 0; i < toDelete; i++) {
      #ifdef TANKALARM_WATCHDOG_AVAILABLE
        #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
          mbedWatchdog.kick();
        #endif
      #endif
      J *delReq = notecard.newRequest("note.delete");
      if (!delReq) {
        overflowed = false;  // stop retrying if allocations fail
        break;
      }
      JAddStringToObject(delReq, "file", TELEMETRY_FILE);
      JAddStringToObject(delReq, "note", noteIds[i]);
      J *delRsp = notecard.requestAndResponse(delReq);
      if (delRsp) {
        const char *delErr = JGetString(delRsp, "err");
        if (!delErr || delErr[0] == '\0') {
          totalDeleted++;
          deletedThisPass++;
        }
        notecard.deleteResponse(delRsp);
      }
    }
    // If no deletions succeeded this pass, the Notecard is not accepting deletes;
    // stop retrying to avoid spinning until TELEMETRY_TRIM_MAX_PASSES is exhausted.
    if (deletedThisPass == 0) {
      Serial.println(F("trimTelemetryOutbox: note.delete failed, aborting trim"));
      break;
    }
  }

  if (totalDeleted > 0) {
    Serial.print(F("trimTelemetryOutbox: dropped "));
    Serial.print(totalDeleted);
    Serial.println(F(" old telemetry note(s) to stay under limit"));
  }
}

static void publishNote(const char *fileName, JsonDocument &doc, bool syncNow) {
  // Build serialized payload once for both live send and buffering
  // fileName is already a plain .qo notefile name (e.g., "telemetry.qo")
  // Cross-device routing is handled by Notehub Routes — no fleet: prefix needed

  // Stamp the schema version and firmware version INTO the document before serialization so it is native to the
  // payload. This guarantees _sv and fv survive the offline flash buffer and flushBufferedNotes()
  // (which re-send the serialized string verbatim) — previously _sv was added to the CJSON
  // body only on the live path and was lost for any buffered/flushed note.
  doc["_sv"] = NOTEFILE_SCHEMA_VERSION;
  doc["fv"] = FIRMWARE_VERSION;

  // Measure JSON first to handle oversized payloads via dynamic allocation
  size_t needed = measureJson(doc);
  if (needed == 0) {
    Serial.println(F("publishNote: empty JSON document"));
    return;
  }

  // Static buffer for typical payloads; heap fallback for oversized ones
  static char staticBuf[2048];
  char *dynamicBuf = nullptr;
  char *buffer = staticBuf;
  size_t bufSize = sizeof(staticBuf);

  if (needed >= sizeof(staticBuf)) {
    dynamicBuf = (char *)malloc(needed + 1);
    if (dynamicBuf) {
      buffer = dynamicBuf;
      bufSize = needed + 1;
    } else {
      Serial.print(F("publishNote: payload needs "));
      Serial.print(needed);
      Serial.println(F(" bytes, heap alloc failed — dropping"));
      return;
    }
  }

  size_t len = serializeJson(doc, buffer, bufSize);
  if (len == 0 || len >= bufSize) {
    Serial.println(F("publishNote: JSON serialization failed"));
    if (dynamicBuf) free(dynamicBuf);
    return;
  }
  buffer[len] = '\0';

  if (!gNotecardAvailable) {
    Serial.println(F("publishNote: Notecard unavailable — buffering"));
    bufferNoteForRetry(fileName, buffer, syncNow);
    if (dynamicBuf) free(dynamicBuf);
    return;
  }

  J *req = notecard.newRequest("note.add");
  if (!req) {
    Serial.println(F("publishNote: newRequest(note.add) returned null"));
    gNotecardFailureCount++;
    bufferNoteForRetry(fileName, buffer, syncNow);
    if (dynamicBuf) free(dynamicBuf);
    return;
  }

  JAddStringToObject(req, "file", fileName);
  if (syncNow) {
    JAddBoolToObject(req, "sync", true);
  }

  J *body = JParse(buffer);
  if (!body) {
    Serial.println(F("publishNote: JParse failed on serialized JSON"));
    JDelete(req);
    bufferNoteForRetry(fileName, buffer, syncNow);
    if (dynamicBuf) free(dynamicBuf);
    return;
  }

  // _sv is already present in the serialized payload (stamped into doc above), so no
  // post-serialization stamp is needed here.

  JAddItemToObject(req, "body", body);

#ifdef TANKALARM_WATCHDOG_AVAILABLE
  // Kick watchdog before the potentially long-blocking I2C transaction
  // with the Notecard (can take up to 30 s if the modem is busy).
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    mbedWatchdog.kick();
  #else
    IWatchdog.reload();
  #endif
#endif

  // Use requestAndResponse to capture the Notecard's error message (if any)
  J *rsp = notecard.requestAndResponse(req);
  if (rsp) {
    const char *err = JGetString(rsp, "err");
    if (err && err[0] != '\0') {
      Serial.print(F("publishNote: Notecard error: "));
      Serial.println(err);
      notecard.deleteResponse(rsp);
      gNotecardFailureCount++;
      bufferNoteForRetry(fileName, buffer, syncNow);
    } else {
      notecard.deleteResponse(rsp);
      gLastSuccessfulNotecardComm = millis();
      gLastSuccessfulNoteSend = millis();
      gNotecardFailureCount = 0;
      flushBufferedNotes();
    }
  } else {
    Serial.println(F("publishNote: requestAndResponse returned null (I2C failure?)"));
    gNotecardFailureCount++;
    bufferNoteForRetry(fileName, buffer, syncNow);
  }
  if (dynamicBuf) free(dynamicBuf);
}

static void bufferNoteForRetry(const char *fileName, const char *payload, bool syncNow) {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) {
      Serial.println(F("Warning: Filesystem not available; note dropped"));
      return;
    }
    FILE *file = fopen("/cfg/pending_notes.log", "a");
    if (!file) {
      Serial.println(F("Failed to open note buffer; dropping payload"));
      return;
    }
    fprintf(file, "%s\t%c\t%s\n", fileName, syncNow ? '1' : '0', payload);
    fclose(file);
  #else
    File file = LittleFS.open(NOTE_BUFFER_PATH, "a");
    if (!file) {
      Serial.println(F("Failed to open note buffer; dropping payload"));
      return;
    }
    file.print(fileName);
    file.print('\t');
    file.print(syncNow ? '1' : '0');
    file.print('\t');
    file.println(payload);
    file.close();
  #endif
  Serial.println(F("Note buffered for retry"));
  pruneNoteBufferIfNeeded();
#else
  Serial.println(F("Warning: Filesystem not available; note dropped"));
#endif
}

// Replay line buffer must hold a full buffered note line:
//   fileName + '\t' + syncFlag + '\t' + payload + newline
// publishNote() serializes through a 2048-byte static buffer, so size this to cover those
// payloads. The previous 1024-byte buffer silently skipped larger buffered notes (e.g. daily
// reports) on replay, losing exactly the backup data weak-signal recovery depends on.
#define NOTE_REPLAY_LINE_MAX 2304
static void flushBufferedNotes() {
#ifdef FILESYSTEM_AVAILABLE
  if (!gNotecardAvailable) {
    return;
  }
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) {
      return;
    }
    
    FILE *src = fopen("/cfg/pending_notes.log", "r");
    if (!src) {
      return;
    }
    
    FILE *tmp = fopen("/cfg/pending_notes.tmp", "w");
    if (!tmp) {
      fclose(src);
      return;
    }
    
    bool wroteFailures = false;
    uint8_t flushCount = 0;
    char lineBuffer[NOTE_REPLAY_LINE_MAX];  // sized to the max publishable payload (see define)
    while (fgets(lineBuffer, sizeof(lineBuffer), src) != nullptr) {
#ifdef TANKALARM_WATCHDOG_AVAILABLE
      // Each note.add is a blocking I2C transaction; kick watchdog per iteration
      // to prevent starvation when many notes are buffered.
      #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
        mbedWatchdog.kick();
      #else
        IWatchdog.reload();
      #endif
#endif
      // Cap per-call flushes to avoid monopolizing the loop
      if (++flushCount > 20) {
        // Re-write remaining unprocessed lines to the tmp file
        fprintf(tmp, "%s", lineBuffer); // current line (still has newline)
        while (fgets(lineBuffer, sizeof(lineBuffer), src) != nullptr) {
          fprintf(tmp, "%s", lineBuffer);
        }
        wroteFailures = true;
        break;
      }
      // Check if line was truncated (no newline at end of non-empty buffer)
      size_t len = strlen(lineBuffer);
      if (len == sizeof(lineBuffer) - 1 && lineBuffer[len - 1] != '\n') {
        #ifdef DEBUG_MODE
        Serial.println(F("Warning: truncated line in note buffer, skipping"));
        #endif
        // Skip rest of the truncated line
        int ch;
        while ((ch = fgetc(src)) != EOF && ch != '\n') {}
        continue;
      }
      // Trim trailing whitespace (newline, CR, spaces) in-place
      size_t lineLen = strlen(lineBuffer);
      while (lineLen > 0 && (lineBuffer[lineLen - 1] == '\n' || lineBuffer[lineLen - 1] == '\r' || lineBuffer[lineLen - 1] == ' ')) {
        lineBuffer[--lineLen] = '\0';
      }
      if (lineLen == 0) continue;

      // Parse tab-delimited fields: fileName\tsyncFlag\tpayload
      char *tab1 = strchr(lineBuffer, '\t');
      if (!tab1) continue;
      char *tab2 = strchr(tab1 + 1, '\t');
      if (!tab2) continue;

      *tab1 = '\0';  // null-terminate fileName
      *tab2 = '\0';  // null-terminate syncToken

      const char *fileName = lineBuffer;
      bool syncNow = (*(tab1 + 1) == '1');
      const char *payload = tab2 + 1;

      J *req = notecard.newRequest("note.add");
      if (!req) {
        wroteFailures = true;
        *tab1 = '\t'; *tab2 = '\t';  // restore tabs for re-serialization
        fprintf(tmp, "%s\n", lineBuffer);
        continue;
      }
      JAddStringToObject(req, "file", fileName);
      if (syncNow) {
        JAddBoolToObject(req, "sync", true);
      }

      J *body = JParse(payload);
      if (!body) {
        JDelete(req);
        continue;
      }
      JAddItemToObject(req, "body", body);

      J *addRsp = notecard.requestAndResponse(req);
      if (!addRsp) {
        wroteFailures = true;
        *tab1 = '\t'; *tab2 = '\t';  // restore tabs for re-serialization
        fprintf(tmp, "%s\n", lineBuffer);
      } else {
        const char *addErr = JGetString(addRsp, "err");
        if (addErr && addErr[0] != '\0') {
          wroteFailures = true;
          *tab1 = '\t'; *tab2 = '\t';  // restore tabs for re-serialization
          fprintf(tmp, "%s\n", lineBuffer);
        }
        notecard.deleteResponse(addRsp);
      }
    }
    
    fclose(src);
    fclose(tmp);
    
    if (wroteFailures) {
      // Atomic rename — LittleFS handles overwrite; do NOT remove() first
      if (rename("/cfg/pending_notes.tmp", "/cfg/pending_notes.log") != 0) {
        Serial.println(F("WARNING: note buffer rename failed"));
        // tmp file preserved for recovery; original .log may still exist
      }
    } else {
      // All notes sent successfully — remove both files
      remove("/cfg/pending_notes.log");
      remove("/cfg/pending_notes.tmp");
    }
  #else
    if (!LittleFS.exists(NOTE_BUFFER_PATH)) {
      return;
    }

    File src = LittleFS.open(NOTE_BUFFER_PATH, "r");
    if (!src) {
      return;
    }

    File tmp = LittleFS.open(NOTE_BUFFER_TEMP_PATH, "w");
    if (!tmp) {
      src.close();
      return;
    }

    bool wroteFailures = false;
    uint8_t flushCount = 0;
    while (src.available()) {
#ifdef TANKALARM_WATCHDOG_AVAILABLE
      // Each note.add is a blocking I2C transaction; kick watchdog per iteration
      #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
        mbedWatchdog.kick();
      #else
        IWatchdog.reload();
      #endif
#endif
      // Cap per-call flushes to avoid monopolizing the loop
      if (++flushCount > 20) {
        // Copy remaining data to tmp
        while (src.available()) {
          tmp.write(src.read());
        }
        wroteFailures = true;
        break;
      }
      // Read line into fixed buffer instead of Arduino String
      char lineBuffer[1024];
      size_t lineLen = 0;
      while (src.available() && lineLen < sizeof(lineBuffer) - 1) {
        char c = src.read();
        if (c == '\n') break;
        lineBuffer[lineLen++] = c;
      }
      lineBuffer[lineLen] = '\0';

      // Trim trailing whitespace (CR, spaces) in-place
      while (lineLen > 0 && (lineBuffer[lineLen - 1] == '\r' || lineBuffer[lineLen - 1] == ' ')) {
        lineBuffer[--lineLen] = '\0';
      }
      if (lineLen == 0) continue;

      // Parse tab-delimited fields: fileName\tsyncFlag\tpayload
      char *tab1 = strchr(lineBuffer, '\t');
      if (!tab1) continue;
      char *tab2 = strchr(tab1 + 1, '\t');
      if (!tab2) continue;

      *tab1 = '\0';  // null-terminate fileName
      *tab2 = '\0';  // null-terminate syncToken

      const char *fileName = lineBuffer;
      bool syncNow = (*(tab1 + 1) == '1');
      const char *payload = tab2 + 1;

      J *req = notecard.newRequest("note.add");
      if (!req) {
        wroteFailures = true;
        *tab1 = '\t'; *tab2 = '\t';  // restore tabs for re-serialization
        tmp.println(lineBuffer);
        continue;
      }
      JAddStringToObject(req, "file", fileName);
      if (syncNow) {
        JAddBoolToObject(req, "sync", true);
      }

      J *body = JParse(payload);
      if (!body) {
        JDelete(req);
        continue;
      }
      JAddItemToObject(req, "body", body);

      J *addRsp = notecard.requestAndResponse(req);
      if (!addRsp) {
        wroteFailures = true;
        *tab1 = '\t'; *tab2 = '\t';  // restore tabs for re-serialization
        tmp.println(lineBuffer);
      } else {
        const char *addErr = JGetString(addRsp, "err");
        if (addErr && addErr[0] != '\0') {
          wroteFailures = true;
          *tab1 = '\t'; *tab2 = '\t';  // restore tabs for re-serialization
          tmp.println(lineBuffer);
        }
        notecard.deleteResponse(addRsp);
      }
    }

    src.close();
    tmp.close();

    if (wroteFailures) {
      // Atomic rename — LittleFS handles overwrite; do NOT remove() first
      if (!LittleFS.rename(NOTE_BUFFER_TEMP_PATH, NOTE_BUFFER_PATH)) {
        Serial.println(F("WARNING: note buffer rename failed"));
      }
    } else {
      LittleFS.remove(NOTE_BUFFER_PATH);
      LittleFS.remove(NOTE_BUFFER_TEMP_PATH);
    }
  #endif
#endif
}

static void pruneNoteBufferIfNeeded() {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) {
      return;
    }
    
    FILE *file = fopen("/cfg/pending_notes.log", "r");
    if (!file) {
      return;
    }
    
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    if (size <= NOTE_BUFFER_MAX_BYTES) {
      fclose(file);
      return;
    }
    
    size_t targetSize = NOTE_BUFFER_MAX_BYTES > NOTE_BUFFER_MIN_HEADROOM ? (NOTE_BUFFER_MAX_BYTES - NOTE_BUFFER_MIN_HEADROOM) : (NOTE_BUFFER_MAX_BYTES / 2);
    if (targetSize == 0) {
      targetSize = NOTE_BUFFER_MAX_BYTES / 2;
    }
    long startOffset = (size > (long)targetSize) ? (size - (long)targetSize) : 0;

    if (fseek(file, startOffset, SEEK_SET) != 0) {
      fclose(file);
      remove("/cfg/pending_notes.log");
      return;
    }

    // Skip partial line if we seeked into the middle
    if (startOffset > 0) {
      char ch;
      while (fread(&ch, 1, 1, file) == 1 && ch != '\n') {
        // consume until newline
      }
    }

    FILE *tmp = fopen("/cfg/pending_notes.tmp", "w");
    if (!tmp) {
      fclose(file);
      return;
    }

    // Use buffered copy for better performance
    char copyBuffer[256];
    size_t bytesRead;
    bool writeError = false;
    while ((bytesRead = fread(copyBuffer, 1, sizeof(copyBuffer), file)) > 0) {
      if (fwrite(copyBuffer, 1, bytesRead, tmp) != bytesRead) {
        writeError = true;
        break;
      }
    }

    fclose(file);
    fclose(tmp);
    
    if (writeError) {
      remove("/cfg/pending_notes.tmp");
      Serial.println(F("Failed to copy note buffer"));
      return;
    }
    // Atomic rename — LittleFS handles overwrite; do NOT remove() first
    if (rename("/cfg/pending_notes.tmp", "/cfg/pending_notes.log") != 0) {
      Serial.println(F("WARNING: note prune rename failed"));
      // tmp preserved for recovery; original was not removed
    } else {
      Serial.println(F("Note buffer pruned"));
    }
  #else
    if (!LittleFS.exists(NOTE_BUFFER_PATH)) {
      return;
    }

    File file = LittleFS.open(NOTE_BUFFER_PATH, "r");
    if (!file) {
      return;
    }

    size_t size = file.size();
    if (size <= NOTE_BUFFER_MAX_BYTES) {
      file.close();
      return;
    }

    size_t targetSize = NOTE_BUFFER_MAX_BYTES > NOTE_BUFFER_MIN_HEADROOM ? (NOTE_BUFFER_MAX_BYTES - NOTE_BUFFER_MIN_HEADROOM) : (NOTE_BUFFER_MAX_BYTES / 2);
    if (targetSize == 0) {
      targetSize = NOTE_BUFFER_MAX_BYTES / 2;
    }
    size_t startOffset = (size > targetSize) ? (size - targetSize) : 0;

    if (!file.seek(startOffset)) {
      file.close();
      LittleFS.remove(NOTE_BUFFER_PATH);
      return;
    }

    if (startOffset > 0) {
      file.readStringUntil('\n');
    }

    File tmp = LittleFS.open(NOTE_BUFFER_TEMP_PATH, "w");
    if (!tmp) {
      file.close();
      return;
    }

    while (file.available()) {
      tmp.write(file.read());
    }

    file.close();
    tmp.close();
    // Atomic rename — LittleFS handles overwrite; do NOT remove() first
    if (!LittleFS.rename(NOTE_BUFFER_TEMP_PATH, NOTE_BUFFER_PATH)) {
      Serial.println(F("WARNING: note prune rename failed"));
    } else {
      Serial.println(F("Note buffer pruned"));
    }
  #endif
#endif
}

// ============================================================================
// Relay Control Functions
// ============================================================================

// Get the Arduino pin number for a relay (0-based index: 0=D0, 1=D1, 2=D2, 3=D3)
// Note: On Arduino Opta, LED_D0-LED_D3 constants are used for relay control
static int getRelayPin(uint8_t relayIndex) {
#if defined(ARDUINO_OPTA)
  if (relayIndex < 4) {
    return LED_D0 + relayIndex;
  }
#endif
  return -1;
}

static void initializeRelays() {
  // Initialize Opta relay outputs (D0-D3)
  for (uint8_t i = 0; i < MAX_RELAYS; ++i) {
    int relayPin = getRelayPin(i);
    if (relayPin >= 0) {
      pinMode(relayPin, OUTPUT);
      digitalWrite(relayPin, LOW);
      gRelayState[i] = false;
    }
  }
#if defined(ARDUINO_OPTA)
  Serial.println(F("Relay control initialized: 4 relays (D0-D3)"));
#else
  Serial.println(F("Warning: Relay control not available on this platform"));
#endif
}

// Set relay state (relayNum is 0-based: 0=relay1, 1=relay2, etc.)
static void setRelayState(uint8_t relayNum, bool state) {
  if (relayNum >= MAX_RELAYS) {
    Serial.print(F("Invalid relay number: "));
    Serial.println(relayNum);
    return;
  }

  int relayPin = getRelayPin(relayNum);
  if (relayPin >= 0) {
    digitalWrite(relayPin, state ? HIGH : LOW);
    gRelayState[relayNum] = state;
    
    Serial.print(F("Relay "));
    Serial.print(relayNum + 1);
    Serial.print(F(" (D"));
    Serial.print(relayNum);
    Serial.print(F(") set to "));
    Serial.println(state ? "ON" : "OFF");
  } else {
    Serial.println(F("Warning: Relay control not available on this platform"));
  }
}

// Find which monitor owns a given relay bit (first match wins)
static uint8_t findMonitorForRelay(uint8_t relayNum) {
  for (uint8_t i = 0; i < gConfig.monitorCount; i++) {
    if (gConfig.monitors[i].relayMask & (1 << relayNum)) return i;
  }
  return MAX_MONITORS;  // Sentinel: no owner
}

// Activate relays with full runtime bookkeeping (used by alarm and manual paths)
static void activateRelayForMonitor(uint8_t monitorIdx, uint8_t relayMask,
                                     RelaySource source, unsigned long now,
                                     uint32_t customDurationSec) {
  for (uint8_t r = 0; r < MAX_RELAYS; r++) {
    if (relayMask & (1 << r)) {
      setRelayState(r, true);
      gRelayRuntime[r].active = true;
      gRelayRuntime[r].ownerMonitor = monitorIdx;
      gRelayRuntime[r].source = source;
      gRelayRuntime[r].activatedAt = now;
      gRelayRuntime[r].customDurationSec = customDurationSec;
    }
  }
}

// Deactivate relays with full runtime bookkeeping
static void deactivateRelayForMonitor(uint8_t monitorIdx, uint8_t relayMask) {
  for (uint8_t r = 0; r < MAX_RELAYS; r++) {
    if (relayMask & (1 << r)) {
      setRelayState(r, false);
      gRelayRuntime[r] = {};  // Zero-init all fields
    }
  }
}

// Check if a monitor has any active relays in gRelayRuntime
static bool isMonitorRelayActive(uint8_t monitorIdx) {
  for (uint8_t r = 0; r < MAX_RELAYS; r++) {
    if (gRelayRuntime[r].active && gRelayRuntime[r].ownerMonitor == monitorIdx) {
      return true;
    }
  }
  return false;
}

// Get the bitmask of relays currently active for a monitor
static uint8_t getMonitorActiveRelayMask(uint8_t monitorIdx) {
  uint8_t mask = 0;
  for (uint8_t r = 0; r < MAX_RELAYS; r++) {
    if (gRelayRuntime[r].active && gRelayRuntime[r].ownerMonitor == monitorIdx) {
      mask |= (1 << r);
    }
  }
  return mask;
}

static void pollForRelayCommands() {
  // Skip if notecard is known to be offline
  if (!gNotecardAvailable) {
    unsigned long now = millis();
    if (now - gLastSuccessfulNotecardComm > NOTECARD_RETRY_INTERVAL) {
      checkNotecardHealth();
    }
    return;
  }

  J *req = notecard.newRequest("note.get");
  if (!req) {
    gNotecardFailureCount++;
    return;
  }

  JAddStringToObject(req, "file", RELAY_CONTROL_FILE);
  // Peek without deleting — delete only after the command is validated and executed so a
  // transient parse/handler failure or a reboot mid-processing cannot silently drop it.

  J *rsp = notecard.requestAndResponse(req);
  if (!rsp) {
    gNotecardFailureCount++;
    if (gNotecardFailureCount >= NOTECARD_FAILURE_THRESHOLD) {
      checkNotecardHealth();
    }
    return;
  }

  gLastSuccessfulNotecardComm = millis();
  gNotecardFailureCount = 0;

  bool consume = false;
  J *body = JGetObject(rsp, "body");
  if (body) {
    char *json = JConvertToJSONString(body);
    if (json) {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, json);
      NoteFree(json);
      if (!err) {
        int noteSchema = doc["_sv"] | 0;
        if (noteSchema > NOTEFILE_SCHEMA_VERSION) {
          // Future schema we cannot interpret — consume our inbox copy without acting.
          Serial.println(F("Relay command from newer schema — ignoring"));
          consume = true;
        } else {
          // processRelayCommand() validates the target UID and enforces the cooldown.
          processRelayCommand(doc);
          consume = true;
        }
      } else {
        Serial.println(F("Relay command invalid JSON — discarding"));
        consume = true;  // malformed note will never parse; discard to avoid blocking the queue
      }
    }
  } else {
    consume = true;  // no body to process — consume the empty/placeholder note
  }

  notecard.deleteResponse(rsp);

  if (consume) {
    J *delReq = notecard.newRequest("note.get");
    if (delReq) {
      JAddStringToObject(delReq, "file", RELAY_CONTROL_FILE);
      JAddBoolToObject(delReq, "delete", true);
      J *delRsp = notecard.requestAndResponse(delReq);
      if (delRsp) notecard.deleteResponse(delRsp);
    }
  }
}

static void processRelayCommand(const JsonDocument &doc) {
  // Verify this command is targeted to this client (if target field present)
  if (!doc["target"].isNull()) {
    const char *targetUid = doc["target"].as<const char*>();
    if (targetUid && targetUid[0] != '\0' && strcmp(targetUid, gDeviceUID) != 0) {
      Serial.print(F("Relay command not for this device (target: "));
      Serial.print(targetUid);
      Serial.println(F(") — ignored"));
      return;
    }
  }

  // Rate limit: reject commands arriving faster than the minimum cooldown
  // to prevent rapid toggling from stale queued Notes or route replays
  {
    static unsigned long lastRelayCommandMillis = 0;
    unsigned long now = millis();
    if (lastRelayCommandMillis != 0 && (now - lastRelayCommandMillis) < RELAY_COMMAND_COOLDOWN_MS) {
      Serial.println(F("Relay command rate-limited — too soon after last command"));
      return;
    }
    lastRelayCommandMillis = now;
  }

  // Handle monitor relay reset command from server first
  // Command format: { "relay_reset_sensor": 0-7 }
  // This is a standalone command that doesn't require relay/state fields
  if (!doc["relay_reset_sensor"].isNull()) {
    uint8_t sensorIdx = doc["relay_reset_sensor"].as<uint8_t>();
    if (sensorIdx < MAX_MONITORS) {
      resetRelayForMonitor(sensorIdx);
    }
    return;  // This is a complete command
  }
  
  // Command format for standard relay control:
  // {
  //   "relay": 1-4,           // Relay number (1-based)
  //   "state": true/false,    // ON/OFF
  //   "duration": 0,          // Optional: auto-off duration in seconds (0 = manual)
  //   "source": "server"      // Optional: source of command (server, client, alarm)
  // }

  if (doc["relay"].isNull() || doc["state"].isNull()) {
    Serial.println(F("Invalid relay command: missing relay or state"));
    return;
  }

  uint8_t relayNum = doc["relay"].as<uint8_t>();
  if (relayNum < 1 || relayNum > MAX_RELAYS) {
    Serial.print(F("Invalid relay number in command: "));
    Serial.println(relayNum);
    return;
  }

  // Convert from 1-based to 0-based
  relayNum = relayNum - 1;

  bool state = doc["state"].as<bool>();
  const char *source = doc["source"] | "unknown";
  
  Serial.print(F("Relay command received from "));
  Serial.print(source);
  Serial.print(F(": Relay "));
  Serial.print(relayNum + 1);
  Serial.print(F(" -> "));
  Serial.println(state ? "ON" : "OFF");

  // Activate/deactivate through unified helpers (which handle GPIO + bookkeeping)
  if (state) {
    uint8_t mask = 1 << relayNum;
    uint8_t monIdx = findMonitorForRelay(relayNum);
    uint32_t dur = doc["duration"] | (uint32_t)0;
    activateRelayForMonitor(monIdx, mask, RELAY_SRC_MANUAL, millis(), dur);
  } else {
    uint8_t mask = 1 << relayNum;
    uint8_t monIdx = findMonitorForRelay(relayNum);
    deactivateRelayForMonitor(monIdx, mask);
  }
}

static void triggerRemoteRelays(const char *targetClient, uint8_t relayMask, bool activate) {
  if (!targetClient || targetClient[0] == '\0' || relayMask == 0) {
    return;
  }

  if (!gNotecardAvailable) {
    Serial.println(F("Cannot trigger remote relays - notecard offline"));
    return;
  }

  // Send commands for each relay bit set in the mask
  for (uint8_t relayNum = 1; relayNum <= 4; ++relayNum) {
    uint8_t bit = relayNum - 1;
    if (relayMask & (1 << bit)) {
      J *req = notecard.newRequest("note.add");
      if (!req) {
        continue;
      }

      // Use relay_forward.qo — Route #1 delivers to server as relay_forward.qi,
      // then server re-issues via command.qo → Route #2 → target client relay.qi
      JAddStringToObject(req, "file", RELAY_FORWARD_OUTBOX_FILE);
      JAddBoolToObject(req, "sync", true);

      J *body = JCreateObject();
      if (!body) {
        JDelete(req);
        continue;
      }

      // Relay forwarding payload: server reads these fields and re-issues the command
      JAddStringToObject(body, "target", targetClient);
      JAddStringToObject(body, "client", gDeviceUID);
      JAddNumberToObject(body, "relay", relayNum);
      JAddBoolToObject(body, "state", activate);
      JAddStringToObject(body, "source", "client-alarm");
      
      JAddItemToObject(req, "body", body);
      
      bool queued = notecard.sendRequest(req);
      if (queued) {
        Serial.print(F("Queued relay command for client "));
        Serial.print(targetClient);
        Serial.print(F(": Relay "));
        Serial.print(relayNum);
        Serial.print(F(" -> "));
        Serial.println(activate ? "ON" : "OFF");
      } else {
        Serial.print(F("Failed to queue relay command for relay "));
        Serial.println(relayNum);
      }
    }
  }
}

// Check and deactivate relays that have exceeded their individual momentary timeout
// or the safety max-on timeout for MANUAL_RESET mode.
// Each relay is tracked independently via gRelayRuntime[].
static void checkRelayMomentaryTimeout(unsigned long now) {
  for (uint8_t r = 0; r < MAX_RELAYS; r++) {
    if (!gRelayRuntime[r].active) continue;
    
    uint8_t monIdx = gRelayRuntime[r].ownerMonitor;
    uint32_t elapsedMs = now - gRelayRuntime[r].activatedAt;
    
    bool expired = false;
    const char *reason = nullptr;
    bool hasOwner = (monIdx < gConfig.monitorCount);
    
    if (hasOwner) {
      const MonitorConfig &cfg = gConfig.monitors[monIdx];
      
      // Check momentary mode timeout
      if (cfg.relayMode == RELAY_MODE_MOMENTARY) {
        uint16_t seconds = cfg.relayMomentarySeconds[r];
        if (seconds == 0) {
          seconds = DEFAULT_RELAY_MOMENTARY_SECONDS;
        }
        uint32_t durationMs = (uint32_t)seconds * 1000UL;
        if (elapsedMs >= durationMs) {
          expired = true;
          reason = "momentary timeout";
        }
      }
      
      // Check manual-reset safety timeout (relayMaxOnSeconds or custom duration)
      if (!expired && (cfg.relayMode == RELAY_MODE_MANUAL_RESET || gRelayRuntime[r].source == RELAY_SRC_MANUAL)) {
        uint32_t limit = gRelayRuntime[r].customDurationSec;
        if (limit == 0) limit = cfg.relayMaxOnSeconds;
        if (limit > 0) {
          uint32_t elapsedSec = elapsedMs / 1000UL;
          if (elapsedSec >= limit) {
            expired = true;
            reason = "safety max-on timeout";
          }
        }
      }
    } else if (gRelayRuntime[r].source == RELAY_SRC_MANUAL && gRelayRuntime[r].customDurationSec > 0) {
      // Ownerless manual relay with explicit duration — still honor the timeout
      uint32_t elapsedSec = elapsedMs / 1000UL;
      if (elapsedSec >= gRelayRuntime[r].customDurationSec) {
        expired = true;
        reason = "manual duration timeout";
      }
    }
    
    if (expired) {
      Serial.print(F("Relay "));
      Serial.print(r + 1);
      Serial.print(F(": "));
      Serial.print(reason);
      Serial.print(F(" after "));
      Serial.print(elapsedMs / 1000UL);
      Serial.print(F("s for monitor "));
      Serial.println(monIdx);
      
      // Deactivate the single relay
      if (hasOwner && gConfig.monitors[monIdx].relayTargetClient[0] != '\0') {
        triggerRemoteRelays(gConfig.monitors[monIdx].relayTargetClient, 1 << r, false);
      }
      deactivateRelayForMonitor(monIdx, 1 << r);
      
      // Send timeout notification through alarm pipeline for safety timeouts
      // (only when we have an owning monitor to route the alarm through)
      if (hasOwner && strcmp(reason, "safety max-on timeout") == 0) {
        sendAlarm(monIdx, "relay_timeout", gMonitorState[monIdx].currentInches);
      }
    }
  }
}

// Reset relay for a specific monitor (called from server manual reset command)
static void resetRelayForMonitor(uint8_t idx) {
  if (idx >= gConfig.monitorCount) {
    return;
  }
  
  const MonitorConfig &cfg = gConfig.monitors[idx];
  uint8_t activeMask = getMonitorActiveRelayMask(idx);
  
  if (activeMask != 0 && cfg.relayTargetClient[0] != '\0') {
    triggerRemoteRelays(cfg.relayTargetClient, activeMask, false);
    Serial.print(F("Manual relay reset for monitor "));
    Serial.println(idx);
  }
  deactivateRelayForMonitor(idx, activeMask);
}

// ============================================================================
// Clear Button Support (Physical Button to Clear All Relay Alarms)
// ============================================================================

// Initialize the clear button pin if configured
static void initializeClearButton() {
  if (gConfig.clearButtonPin < 0) {
    // Clear button disabled
    gClearButtonInitialized = false;
    return;
  }
  
  // Configure the button pin
  if (gConfig.clearButtonActiveHigh) {
    // Button is active HIGH - use INPUT (external pull-down required)
    pinMode(gConfig.clearButtonPin, INPUT);
  } else {
    // Button is active LOW - use INPUT_PULLUP (button connects to GND)
    pinMode(gConfig.clearButtonPin, INPUT_PULLUP);
  }
  
  gClearButtonInitialized = true;
  gClearButtonLastState = false;
  gClearButtonLastPressTime = 0;
  
  Serial.print(F("Clear button initialized on pin "));
  Serial.print(gConfig.clearButtonPin);
  Serial.println(gConfig.clearButtonActiveHigh ? F(" (active HIGH)") : F(" (active LOW with pullup)"));
}

// Check for clear button press (with debouncing)
static void checkClearButton(unsigned long now) {
  if (!gClearButtonInitialized || gConfig.clearButtonPin < 0) {
    return;
  }
  
  // Read the button state
  bool buttonPhysical = digitalRead(gConfig.clearButtonPin);
  bool buttonPressed = gConfig.clearButtonActiveHigh ? buttonPhysical : !buttonPhysical;
  
  // Debounce: only register state change after stable for CLEAR_BUTTON_DEBOUNCE_MS
  if (buttonPressed != gClearButtonLastState) {
    // State changed - reset the timer
    gClearButtonLastPressTime = now;
    gClearButtonLastState = buttonPressed;
    return;
  }
  
  // Button state is stable
  if (buttonPressed && (now - gClearButtonLastPressTime >= CLEAR_BUTTON_MIN_PRESS_MS)) {
    // Button has been held for minimum press time - trigger clear
    Serial.println(F("Clear button pressed - clearing all relay alarms"));
    addSerialLog("Clear button pressed - clearing all relay alarms");
    clearAllRelayAlarms();
    
    // Reset the timer to require release before next trigger
    gClearButtonLastPressTime = now;
    gClearButtonLastState = false;  // Require button release before next action
    
    // Wait for button release to prevent repeated triggers
    unsigned long releaseWaitStart = millis();
    while (millis() - releaseWaitStart < 2000) {  // Wait up to 2 seconds
      bool stillPressed = gConfig.clearButtonActiveHigh ? 
                          digitalRead(gConfig.clearButtonPin) : 
                          !digitalRead(gConfig.clearButtonPin);
      if (!stillPressed) {
        break;
      }
      delay(50);
    }
  }
}

// ============================================================================
// Front-Panel USER Button (BTN_USER) — manual hub.sync trigger
// ============================================================================
// On Arduino Opta the BTN_USER pin is the front-panel pushbutton (active LOW
// with internal pullup). A short press forces an immediate Notecard hub.sync
// so a field tech can pull pending config / relay commands from Notehub
// without waiting for the next inbound polling interval (which can be
// minutes-to-hours on solar/cellular). Throttled by USER_BUTTON_SYNC_COOLDOWN_MS
// to prevent button-mashing from spamming Notehub.

static void initializeUserButton() {
#ifdef BTN_USER
  pinMode(BTN_USER, INPUT_PULLUP);
  gUserButtonInitialized = true;
  gUserButtonLastState = false;
  gUserButtonChangeTime = 0;
  gUserButtonLastSyncTime = 0;
  Serial.println(F("USER button enabled (press to force hub.sync)"));
#endif
}

static void checkUserButton(unsigned long now) {
#ifdef BTN_USER
  if (!gUserButtonInitialized) return;

  // BTN_USER is active LOW with pullup
  bool pressedNow = (digitalRead(BTN_USER) == LOW);

  // Debounce: state must be stable for USER_BUTTON_DEBOUNCE_MS
  if (pressedNow != gUserButtonLastState) {
    if (gUserButtonChangeTime == 0) {
      gUserButtonChangeTime = now;
      return;
    }
    if (now - gUserButtonChangeTime < USER_BUTTON_DEBOUNCE_MS) {
      return;
    }
    // Stable transition
    gUserButtonLastState = pressedNow;
    gUserButtonChangeTime = 0;

    // Trigger on press (LOW edge), not on release
    if (pressedNow) {
      if (gUserButtonLastSyncTime != 0 &&
          (now - gUserButtonLastSyncTime) < USER_BUTTON_SYNC_COOLDOWN_MS) {
        unsigned long remaining =
            (USER_BUTTON_SYNC_COOLDOWN_MS - (now - gUserButtonLastSyncTime)) / 1000UL;
        Serial.print(F("USER button: hub.sync throttled ("));
        Serial.print(remaining);
        Serial.println(F("s remaining)"));
        return;
      }

      Serial.println(F("USER button pressed - forcing hub.sync"));
      addSerialLog("USER button hub.sync triggered");
      gUserButtonLastSyncTime = now;

      if (!gNotecardAvailable) {
        Serial.println(F("  (Notecard unavailable - sync deferred)"));
        return;
      }
      J *syncReq = notecard.newRequest("hub.sync");
      if (syncReq) {
        if (notecard.sendRequest(syncReq)) {
          Serial.println(F("  hub.sync command sent"));
        } else {
          Serial.println(F("  hub.sync command failed"));
        }
      }

      // Open a temporary continuous-mode service window so any inbound
      // notes that arrive during the next SERVICE_WINDOW_DURATION_MS are
      // delivered immediately (instead of waiting for the next periodic
      // inbound interval). Skipped on grid clients (already continuous)
      // and in low-power states (don't burn battery).
      openServiceWindow(now);
    }
  } else {
    gUserButtonChangeTime = 0;
  }
#else
  (void)now;
#endif
}

// ----------------------------------------------------------------------------
// Service window: temporarily switch the Notecard to continuous mode so any
// inbound notes that arrive during the window are delivered immediately.
// Used by the USER button to give a field tech a ~2 minute service window
// to push config changes from the server without waiting on the periodic
// inbound interval. No-op on grid clients (already continuous) and in
// low-power states (don't burn battery for convenience).
// ----------------------------------------------------------------------------
static void openServiceWindow(unsigned long now) {
  if (!gNotecardAvailable) return;
  // Grid clients are already continuous — nothing to do.
  if (!gConfig.solarPowered) return;
  // Don't burn battery in degraded power states.
  if (gPowerState >= POWER_STATE_ECO) {
    Serial.println(F("  Service window: skipped (power state degraded)"));
    return;
  }

  // If a window is already open, just extend it.
  if (gServiceWindowActive) {
    gServiceWindowUntil = now + SERVICE_WINDOW_DURATION_MS;
    Serial.print(F("  Service window extended to "));
    Serial.print(SERVICE_WINDOW_DURATION_MS / 60000UL);
    Serial.println(F(" min"));
    return;
  }

  J *req = notecard.newRequest("hub.set");
  if (!req) return;
  JAddStringToObject(req, "mode", "continuous");
  if (!notecard.sendRequest(req)) {
    Serial.println(F("  Service window: hub.set continuous failed"));
    return;
  }

  gServiceWindowActive = true;
  gServiceWindowUntil = now + SERVICE_WINDOW_DURATION_MS;
  Serial.print(F("  Service window opened: continuous mode for "));
  Serial.print(SERVICE_WINDOW_DURATION_MS / 60000UL);
  Serial.println(F(" min"));
  addSerialLog("Service window opened (continuous mode)");
}

static void checkServiceWindowExpiry(unsigned long now) {
  if (!gServiceWindowActive) return;
  if ((long)(now - gServiceWindowUntil) < 0) return;  // not yet expired

  // Restore periodic mode (matching configureNotecardHubMode logic for solar).
  if (gNotecardAvailable) {
    J *req = notecard.newRequest("hub.set");
    if (req) {
      int inboundMinutes = (gConfig.monitorCount == 0)
          ? AWAITING_CONFIG_SOLAR_INBOUND_MINUTES
          : SOLAR_INBOUND_INTERVAL_MINUTES;
      JAddStringToObject(req, "mode", "periodic");
      JAddIntToObject(req, "outbound", SOLAR_OUTBOUND_INTERVAL_MINUTES);
      JAddIntToObject(req, "inbound", inboundMinutes);
      notecard.sendRequest(req);
    }
  }
  gServiceWindowActive = false;
  gServiceWindowUntil = 0;
  Serial.println(F("Service window closed - restored periodic mode"));
  addSerialLog("Service window closed");
}

// Clear all relay alarms for all sensors (turn off all relays and reset state)
static void clearAllRelayAlarms() {
  bool anyCleared = false;
  
  for (uint8_t i = 0; i < gConfig.monitorCount; ++i) {
    if (isMonitorRelayActive(i)) {
      resetRelayForMonitor(i);
      anyCleared = true;
    }
  }
  
  // Also turn off any locally controlled relays (including ownerless manual relays)
  for (uint8_t r = 0; r < MAX_RELAYS; ++r) {
    if (gRelayState[r] || gRelayRuntime[r].active) {
      setRelayState(r, false);
      gRelayRuntime[r] = {};  // Clear stale bookkeeping
      anyCleared = true;
    }
  }
  
  if (anyCleared) {
    Serial.println(F("All relay alarms cleared"));
  } else {
    Serial.println(F("No active relay alarms to clear"));
  }
}

// ============================================================================
// Serial Logging for Remote Debugging
// ============================================================================

static void addSerialLog(const char *message) {
  if (!message || strlen(message) == 0) {
    return;
  }

  SerialLogEntry &entry = gSerialLog.entries[gSerialLog.writeIndex];
  entry.timestamp = currentEpoch();
  
  // Truncate if necessary
  size_t len = strlen(message);
  if (len >= sizeof(entry.message)) {
    len = sizeof(entry.message) - 1;
  }
  memcpy(entry.message, message, len);
  entry.message[len] = '\0';

  gSerialLog.writeIndex = (gSerialLog.writeIndex + 1) % CLIENT_SERIAL_BUFFER_SIZE;
  if (gSerialLog.count < CLIENT_SERIAL_BUFFER_SIZE) {
    gSerialLog.count++;
  }
}

static void pollForSerialRequests() {
  // Check for serial log requests from server
  uint8_t processed = 0;
  while (processed < 5) {  // Cap iterations to avoid watchdog starvation
#ifdef TANKALARM_WATCHDOG_AVAILABLE
    #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
      mbedWatchdog.kick();
    #else
      IWatchdog.reload();
    #endif
#endif
    J *req = notecard.newRequest("note.get");
    if (!req) {
      return;
    }
    
    JAddStringToObject(req, "file", SERIAL_REQUEST_FILE);
    // Peek without deleting — delete only after the request is handled (crash safety).
    
    J *rsp = notecard.requestAndResponse(req);
    if (!rsp) {
      return;
    }

    J *body = JGetObject(rsp, "body");
    if (!body) {
      notecard.deleteResponse(rsp);
      break;
    }

    const char *request = JGetString(body, "request");
    if (request && strcmp(request, "send_logs") == 0) {
      DEBUG_PRINTLN(F("Serial log request received from server"));
      addSerialLog("Serial log request received");
      sendSerialAck("processing");
      sendSerialLogs();
      sendSerialAck("complete");
    }

    notecard.deleteResponse(rsp);
    // Consume the note now that it has been handled (or was an unrecognized/empty request).
    J *delReq = notecard.newRequest("note.get");
    if (delReq) {
      JAddStringToObject(delReq, "file", SERIAL_REQUEST_FILE);
      JAddBoolToObject(delReq, "delete", true);
      J *delRsp = notecard.requestAndResponse(delReq);
      if (delRsp) notecard.deleteResponse(delRsp);
    }
    processed++;
  }
}

static void sendSerialAck(const char *status) {
  J *req = notecard.newRequest("note.add");
  if (!req) {
    return;
  }

  JAddStringToObject(req, "file", SERIAL_ACK_OUTBOX_FILE);
  JAddBoolToObject(req, "sync", true);

  J *body = JCreateObject();
  if (!body) {
    JDelete(req);
    return;
  }

  JAddStringToObject(body, "client", gDeviceUID);
  JAddStringToObject(body, "status", status);
  JAddItemToObject(req, "body", body);

  bool queued = notecard.sendRequest(req);
  if (queued) {
    DEBUG_PRINT(F("Sent serial ack: "));
    DEBUG_PRINTLN(status);
  }
}

static void sendSerialLogs() {
  if (gSerialLog.count == 0) {
    DEBUG_PRINTLN(F("No serial logs to send"));
    return;
  }

  // Create a note with an array of log entries
  J *req = notecard.newRequest("note.add");
  if (!req) {
    return;
  }

  // Send serial logs as plain .qo — Notehub Route delivers to server
  JAddStringToObject(req, "file", SERIAL_LOG_FILE);
  JAddBoolToObject(req, "sync", true);

  J *body = JCreateObject();
  if (!body) {
    JDelete(req);
    return;
  }

  JAddStringToObject(body, "client", gDeviceUID);

  J *logsArray = JCreateArray();
  if (!logsArray) {
    JDelete(body);
    JDelete(req);
    return;
  }

  // Add most recent logs from circular buffer (limit to 20)
  uint8_t startIdx = (gSerialLog.count < CLIENT_SERIAL_BUFFER_SIZE) ? 0 : gSerialLog.writeIndex;
  uint8_t sentCount = 0;
  uint8_t startOffset = (gSerialLog.count > 20) ? (gSerialLog.count - 20) : 0;
  
  for (uint8_t i = startOffset; i < gSerialLog.count && sentCount < 20; ++i) {  // Limit to 20 most recent logs
    uint8_t idx = (startIdx + i) % CLIENT_SERIAL_BUFFER_SIZE;
    SerialLogEntry &entry = gSerialLog.entries[idx];
    
    if (entry.message[0] == '\0') {
      continue;
    }

    J *logEntry = JCreateObject();
    if (!logEntry) {
      break;
    }

    JAddNumberToObject(logEntry, "timestamp", entry.timestamp);
    JAddStringToObject(logEntry, "message", entry.message);
    JAddItemToArray(logsArray, logEntry);
    sentCount++;
  }

  JAddItemToObject(body, "logs", logsArray);
  JAddItemToObject(req, "body", body);

  bool queued = notecard.sendRequest(req);
  if (queued) {
    DEBUG_PRINT(F("Sent "));
    DEBUG_PRINT(sentCount);
    DEBUG_PRINTLN(F(" serial logs to server"));
  } else {
    DEBUG_PRINTLN(F("Failed to queue serial logs"));
  }
}

// ============================================================================
// Location Request Handling
// ============================================================================
// Server can request the client's GPS location (e.g., for NWS weather lookup during calibration)
// Client responds with location via cell tower triangulation (low power)

static void pollForLocationRequests() {
  // Check for location requests from server
  J *req = notecard.newRequest("note.get");
  if (!req) {
    return;
  }
  
  JAddStringToObject(req, "file", LOCATION_REQUEST_FILE);
  // Peek without deleting — delete only after the location response has been sent (crash safety).
  
  J *rsp = notecard.requestAndResponse(req);
  if (!rsp) {
    return;
  }

  J *body = JGetObject(rsp, "body");
  if (!body) {
    notecard.deleteResponse(rsp);
    return;
  }

  const char *request = JGetString(body, "request");
  if (request && strcmp(request, "get_location") == 0) {
    Serial.println(F("Location request received from server"));
    
    // Fetch location and send response
    float latitude = 0.0f, longitude = 0.0f;
    bool hasLocation = fetchNotecardLocation(latitude, longitude);
    
    // Send location response — plain .qo, Notehub Route delivers to server
    J *respReq = notecard.newRequest("note.add");
    if (respReq) {
      JAddStringToObject(respReq, "file", LOCATION_RESPONSE_FILE);
      JAddBoolToObject(respReq, "sync", true);
      
      J *respBody = JCreateObject();
      if (respBody) {
        JAddStringToObject(respBody, "client", gDeviceUID);
        if (hasLocation) {
          JAddNumberToObject(respBody, "lat", latitude);
          JAddNumberToObject(respBody, "lon", longitude);
          JAddBoolToObject(respBody, "valid", true);
        } else {
          JAddBoolToObject(respBody, "valid", false);
          JAddStringToObject(respBody, "error", "Location unavailable");
        }
        JAddItemToObject(respReq, "body", respBody);
        
        if (notecard.sendRequest(respReq)) {
          Serial.println(F("Location response sent to server"));
        } else {
          Serial.println(F("Failed to send location response"));
        }
      }
    }
  }

  notecard.deleteResponse(rsp);
  // Consume the note now that the request has been handled (or was unrecognized/empty).
  J *delReq = notecard.newRequest("note.get");
  if (delReq) {
    JAddStringToObject(delReq, "file", LOCATION_REQUEST_FILE);
    JAddBoolToObject(delReq, "delete", true);
    J *delRsp = notecard.requestAndResponse(delReq);
    if (delRsp) notecard.deleteResponse(delRsp);
  }
}

// ============================================================================
// Server-Requested Sync (Phase 3: Sync-on-Demand)
// ============================================================================
// The server can send a "sync" command via command.qo → sync_request.qi to
// force this client to perform an immediate Notecard sync (hub.sync).
// This is critical for low-signal clients where inbound notes are stuck
// "Pending sync to Notecard" — the forced sync increases the chance of
// pulling pending config/relay notes through a marginal cellular link.

static void pollForSyncRequests() {
  J *req = notecard.newRequest("note.get");
  if (!req) return;

  JAddStringToObject(req, "file", SYNC_REQUEST_FILE);
  // Peek without deleting — delete only after the sync has been initiated (crash safety).

  J *rsp = notecard.requestAndResponse(req);
  if (!rsp) return;

  J *body = JGetObject(rsp, "body");
  if (!body) {
    notecard.deleteResponse(rsp);
    return;
  }

  const char *request = JGetString(body, "request");
  bool handled = (request && strcmp(request, "sync") == 0);
  notecard.deleteResponse(rsp);

  if (handled) {
    Serial.println(F("Sync request received from server — forcing hub.sync"));
    addSerialLog("Server-requested hub.sync initiated");

    // Execute hub.sync to force immediate Notecard sync with Notehub
    J *syncReq = notecard.newRequest("hub.sync");
    if (syncReq) {
      if (notecard.sendRequest(syncReq)) {
        Serial.println(F("hub.sync command sent to Notecard"));
      } else {
        Serial.println(F("hub.sync command failed"));
      }
    }
  }

  // Consume the note now that the sync has been initiated (or it was an unrecognized note).
  J *delReq = notecard.newRequest("note.get");
  if (delReq) {
    JAddStringToObject(delReq, "file", SYNC_REQUEST_FILE);
    JAddBoolToObject(delReq, "delete", true);
    J *delRsp = notecard.requestAndResponse(delReq);
    if (delRsp) notecard.deleteResponse(delRsp);
  }
}

// ============================================================================
// Analog Vin Voltage Divider Reading
// ============================================================================

/**
 * Read actual battery voltage via an external voltage divider on an Opta analog input.
 *
 * Wiring: Battery+ --> R1 --> Analog Pin --> R2 --> GND
 * The ADC reads the divided voltage; we reverse the ratio to get battery voltage.
 *
 * Takes 8 samples with 2ms settling delay between each (same pattern as readMonitorSensor).
 * Total time: ~16ms — negligible power cost.
 *
 * @return Battery voltage in volts, or 0.0 if Vin monitor is disabled or ratio is invalid.
 */
static float readVinDividerVoltage() {
  if (!gConfig.vinMonitor.enabled) return 0.0f;

  float ratio = vinDividerRatio(&gConfig.vinMonitor);
  if (ratio <= 0.0f) return 0.0f;

  uint8_t pin = gConfig.vinMonitor.analogPin;
  if (pin > 7) return 0.0f;

  // Multi-sample averaging: 8 samples with 2ms settling delay
  float total = 0.0f;
  const uint8_t samples = 8;
  for (uint8_t i = 0; i < samples; ++i) {
    int raw = analogRead(pin);
    float pinVoltage = (float)raw / VIN_MONITOR_ADC_MAX * VIN_MONITOR_ADC_REF_VOLTAGE;
    total += pinVoltage;
    delay(2);
  }
  float avgPinVoltage = total / samples;

  // Reverse the divider ratio to get actual battery voltage
  float batteryVoltage = avgPinVoltage / ratio;

  if (batteryVoltage > 0.5f) {  // Filter out noise when nothing is connected
    Serial.print(F("Vin divider: pin="));
    Serial.print(avgPinVoltage, 2);
    Serial.print(F("V -> battery="));
    Serial.print(batteryVoltage, 2);
    Serial.println(F("V"));
  }

  return batteryVoltage;
}

// ============================================================================
// Notecard Location Fetching (Cell Tower Triangulation)
// ============================================================================
// Fetches the device's location from the Notecard using cell tower triangulation
// This is much lower power than GPS and provides approximate location for weather lookups

static bool fetchNotecardLocation(float &latitude, float &longitude) {
  // First, request a location fix using cell tower triangulation
  J *req = notecard.newRequest("card.location");
  if (!req) {
    Serial.println(F("Failed to create card.location request"));
    return false;
  }
  
  J *rsp = notecard.requestAndResponse(req);
  if (!rsp) {
    Serial.println(F("No response from card.location"));
    return false;
  }
  
  // Check for error response
  const char *err = JGetString(rsp, "err");
  if (err && strlen(err) > 0) {
    // Location may not be available yet (device just powered on, no cell signal, etc.)
    Serial.print(F("card.location: "));
    Serial.println(err);
    notecard.deleteResponse(rsp);
    return false;
  }
  
  // Extract latitude and longitude
  // Response format: { "lat": 38.8894, "lon": -77.0352, "status": "GPS,WiFi,Triangulated", ... }
  double lat = JGetNumber(rsp, "lat");
  double lon = JGetNumber(rsp, "lon");
  const char *status = JGetString(rsp, "status");
  
  notecard.deleteResponse(rsp);
  
  // Validate coordinates (must be non-zero and in valid range)
  if (lat == 0.0 && lon == 0.0) {
    Serial.println(F("card.location: No valid coordinates"));
    return false;
  }
  
  if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
    Serial.println(F("card.location: Coordinates out of range"));
    return false;
  }
  
  latitude = (float)lat;
  longitude = (float)lon;
  
  Serial.print(F("Location: "));
  Serial.print(latitude, 4);
  Serial.print(F(", "));
  Serial.print(longitude, 4);
  if (status) {
    Serial.print(F(" ("));
    Serial.print(status);
    Serial.print(F(")"));
  }
  Serial.println();

  return true;
}

// ============================================================================
// Diagnostics
// ============================================================================

/**
 * Sleep for the given duration in milliseconds, kicking the watchdog in
 * safe-sized chunks so a long sleep cannot trigger a hardware reset.
 *
 * On Mbed/Opta: uses rtos::ThisThread::sleep_for() in chunks of at most
 * half the watchdog timeout (15 s default).
 * On other platforms: falls back to delay().
 */
static void safeSleep(unsigned long ms) {
#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  const unsigned long maxChunk = (WATCHDOG_TIMEOUT_SECONDS * 1000UL) / 2;
  unsigned long remaining = ms;
  while (remaining > 0) {
    unsigned long chunk = (remaining > maxChunk) ? maxChunk : remaining;
    rtos::ThisThread::sleep_for(std::chrono::milliseconds(chunk));
    #ifdef TANKALARM_WATCHDOG_AVAILABLE
      mbedWatchdog.kick();
    #endif
    remaining -= chunk;
  }
#else
  // Non-Mbed fallback: chunked sleep with watchdog kick to avoid WDT reset
  if (ms == 0) return;
  #ifdef TANKALARM_WATCHDOG_AVAILABLE
    const unsigned long maxChunk = (WATCHDOG_TIMEOUT_SECONDS * 1000UL) / 2;
  #else
    const unsigned long maxChunk = ms;
  #endif
  unsigned long remaining = ms;
  while (remaining > 0) {
    unsigned long chunk = (remaining > maxChunk) ? maxChunk : remaining;
    delay(chunk);
    #ifdef TANKALARM_WATCHDOG_AVAILABLE
      IWatchdog.reload();
    #endif
    remaining -= chunk;
  }
#endif
}

/**
 * Get current free heap bytes for field diagnostics.
 * Delegates to the shared tankalarm_freeRam() implementation.
 */
static uint32_t freeRam() { return tankalarm_freeRam(); }

// ============================================================================
// Health Telemetry (optional, behind TANKALARM_HEALTH_TELEMETRY_ENABLED flag)
// ============================================================================
#ifdef TANKALARM_HEALTH_TELEMETRY_ENABLED
/**
 * Send a lightweight health status note to Notehub for field diagnosis.
 * Includes: heap stats, uptime, power state, notecard comm health,
 * storage availability, and firmware version.
 *
 * Bandwidth footprint: ~200 bytes per note, sent at HEALTH_TELEMETRY_INTERVAL_MS.
 * No impact on normal operation when TANKALARM_HEALTH_TELEMETRY_ENABLED is not defined.
 */
static void sendHealthTelemetry() {
  if (!gNotecardAvailable) return;

  TankAlarmHealthSnapshot snap = tankalarm_collectHealthSnapshot();
  snap.heapMinFreeBytes = gHeapMinFreeBytes;
  snap.notecardCommErrors = gNotecardCommErrorCount;
  snap.storageWriteErrors = gStorageWriteErrorCount;
  snap.storageAvailable = isStorageAvailable();

  JsonDocument doc;
  doc["heap_free"] = snap.heapFreeBytes;
  doc["heap_min_free"] = (snap.heapMinFreeBytes == UINT32_MAX) ? 0 : snap.heapMinFreeBytes;
  doc["uptime_s"] = snap.uptimeSeconds;
  doc["power_state"] = (uint8_t)gPowerState;
  doc["power_state_name"] = getPowerStateDescription(gPowerState);
  doc["battery_v"] = roundTo(gEffectiveBatteryVoltage, 2);
  doc["notecard_errors"] = snap.notecardCommErrors;
  doc["storage_errors"] = snap.storageWriteErrors;
  doc["storage_ok"] = snap.storageAvailable;
  doc["fw_version"] = FIRMWARE_VERSION;

  // I2C bus health counters
  doc["i2c_cl_err"] = gCurrentLoopI2cErrors;
  doc["i2c_bus_recover"] = gI2cBusRecoveryCount;
  // v2.0.46 A0602 current-loop diagnostics (daily window): valid-read count, over-range/garbage
  // rejections, last fault code (see ClFaultReason), and last read-burst duration (us) so the
  // shared-bus health and the per-op 400kHz window's effect are observable in Notehub.
  doc["cl_ok"] = gCurrentLoopReadsOk;
  doc["cl_or"] = gCurrentLoopOverRange;
  doc["cl_fault"] = gLastClFaultReason;
  doc["cl_dur_us"] = gLastClBurstMicros;

  // Include solar data if available
  if (gSolarManager.isEnabled()) {
    doc["solar_enabled"] = true;
  }

  // Include monitor count for fleet visibility
  uint8_t activeMonitors = 0;
  for (uint8_t i = 0; i < gConfig.monitorCount; i++) {
    if (gConfig.monitors[i].sensorInterface != SENSOR_DIGITAL ||
        gConfig.monitors[i].primaryPin >= 0) {
      activeMonitors++;
    }
  }
  doc["monitors"] = activeMonitors;

  // Include startup I2C scan results (first report only) — 3.2.5
  if (!gStartupScanReported) {
    doc["scan_nc"] = gStartupNotecardFound;           // Notecard found at boot?
    doc["scan_cl"] = gStartupCurrentLoopFound;        // Current loop (A0602) found?
    doc["scan_retries"] = gStartupScanRetries;        // Retry attempts used
    doc["scan_unexpected"] = gStartupUnexpectedDevices; // Unexpected I2C devices
    gStartupScanReported = true;
  }

  publishNote(HEALTH_FILE, doc, false);

  DEBUG_PRINTLN(F("Health telemetry sent"));
}
#endif // TANKALARM_HEALTH_TELEMETRY_ENABLED
