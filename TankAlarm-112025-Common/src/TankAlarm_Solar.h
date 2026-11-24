/**
 * TankAlarm_Solar.h
 * 
 * SunSaver MPPT Solar Charger Monitoring via RS485 Modbus RTU
 * 
 * Hardware Requirements:
 * - Arduino Opta with built-in RS485:
 *   - Opta WiFi (AFX00002), or
 *   - Opta RS485 (AFX00001)
 *   - Opta Lite (AFX00003) requires an external RS485 transceiver
 * - Morningstar MRC-1 (MeterBus to EIA-485 Adapter) - Recommended
 *   - Powered by SunSaver via RJ-11 cable (no external power required)
 *   - Provides isolated RS-485 connection to Opta
 *   - Wiring: Opta A(-) to MRC-1 B(-), Opta B(+) to MRC-1 A(+), GND to G
 * 
 * Alternative (DIY, not recommended):
 * - Generic TTL to RS-485 module with auto-flow control (e.g., XY-017)
 *   - Warning: Requires voltage step-down from SunSaver 12V to 5V
 *   - Not isolated - risk of ground loops
 * 
 * Modbus Protocol:
 * - Protocol: Modbus RTU over RS-485
 * - Default Slave ID: 1
 * - Baud Rate: 9600 (typical)
 * - Data Format: 8N1 or 8N2
 * 
 * Copyright (c) 2025-2026 Senax Inc. All rights reserved.
 */

#ifndef TANKALARM_SOLAR_H
#define TANKALARM_SOLAR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ============================================================================
// SunSaver MPPT Modbus Register Addresses (Holding Registers, Function Code 03)
// Note: ArduinoModbus uses 0-based addresses, so address = register - 1
// Register addresses verified by direct bench capture against a SunSaver MPPT
// charger via MRC-1 on 2026-04-22. The legacy choice of 0x0010..0x0013 was
// reading min/max derived registers that hold zero on this firmware revision.
// The live filtered ADC values are at 0x0008..0x000C (adc_*_f).
// ============================================================================

// Voltage and Current Registers (Real-time, filtered)
#define SS_REG_BATTERY_VOLTAGE      0x0008  // adc_vb_f: filtered battery voltage
#define SS_REG_ARRAY_VOLTAGE        0x0009  // adc_va_f: filtered array (panel) voltage
#define SS_REG_LOAD_VOLTAGE         0x000A  // adc_vl_f: filtered load voltage
#define SS_REG_CHARGE_CURRENT       0x000B  // adc_ic_f: filtered charge current
#define SS_REG_LOAD_CURRENT         0x000C  // adc_il_f: filtered load current

// The registers below are where the SunSaver MPPT MODBUS Specification v11 puts them, as written
// out in OptaView's profile (OptaView/profiles/sunsaver-mppt.json). The host test
// tests/host/sunsaver_settings checks every address and bit here against that profile. None of
// them has been confirmed on hardware yet, so production builds do not read them: see
// SOLAR_ENABLE_UNVERIFIED_REGISTERS in SolarManager::readRegisters(). The addresses this file
// had before (0x001B, 0x001C, 0x002B to 0x002F, 0x0034, 0x0038, 0x003D, 0x003E) belong to other
// registers, which is why the bench read "values that look like extra voltage snapshots" there:
// 0x002B and 0x002C are the battery's lowest and highest voltage today.

// Temperatures: signed degrees C in the low byte
#define SS_REG_HEATSINK_TEMP        0x000D  // T_hs
#define SS_REG_BATTERY_TEMP         0x000E  // T_batt: the remote sensor (RTS) when one is connected, else the controller's own

// Status
#define SS_REG_CHARGE_STATE         0x0011  // charge_state: a SolarChargeState
#define SS_REG_ARRAY_FAULT          0x0012  // array_fault: SS_ARRAY_FAULT_* bits
#define SS_REG_LOAD_STATE           0x001A  // load_state: a SolarLoadState
#define SS_REG_LOAD_FAULT           0x001B  // load_fault: SS_LOAD_FAULT_* bits
#define SS_REG_ALARM_HI             0x0023  // alarm, high word: SS_ALARM_* bits 16 and up
#define SS_REG_ALARM_LO             0x0024  // alarm, low word: SS_ALARM_* bits 0 to 15

// The charge the controller counted on its own "today", 0.1 Ah per count. It has no clock, so its
// day need not be the client's, whose own figures cover the time between two daily reports (see
// SolarManager::resetDailyStats()).
#define SS_REG_AHC_DAILY            0x002D  // Ahc_daily

// ============================================================================
// Stored charge settings
// ============================================================================
// What the controller has stored (its charging profile, LVD and LVR, the DIP
// switches) is read by the charger check: TankAlarm_SunSaverSettings.h has the
// registers and the rules, and SolarManager::startChargerCheck() below runs it.
// The settings are in EEPROM at 0xE000 and up. Registers 0x0033, 0x0035 and
// 0x0036, which this file used to read as "setpoints", are the controller's
// lowest and highest battery voltage and a reserved word (SunSaver MPPT MODBUS
// Specification v11).
//
// Charger programming writes a LiFePO4 charging profile and the LVD and LVR into
// that block when the client's config asks for it (TankAlarm_SunSaverProgram.h
// has the limits and the procedure, SolarManager::serviceChargerProgramming()
// below runs it). It is compiled in only with TANKALARM_SUNSAVER_PROGRAMMING set
// to 1, which stays 0 until a bench run has shown how a real SunSaver takes the
// writes and the restart. A client built with 0 never writes to the controller:
// it answers each request with "not built" and does nothing else.
#ifndef TANKALARM_SUNSAVER_PROGRAMMING
#define TANKALARM_SUNSAVER_PROGRAMMING 0
#endif

// ============================================================================
// Scaling Factors for 12V System
// Formula: Actual = (Raw * Scale) / 32768
// ============================================================================
// Per Morningstar SunSaver MPPT PDU: V_PU (12V controller) = 96.667 V, I_PU = 79.16 A.
#define SS_SCALE_VOLTAGE_12V        96.667f   // Voltage scaling: Raw * 96.667 / 32768
// The same as a sunsaver::ScaleId (TankAlarm_SunSaverSettings.h), for the charger check, which
// compares what the charger says about its own LVD with this scale. Change the two together.
#define SS_VOLTAGE_SCALE_ID         sunsaver::SCALE_96667
#define SS_SCALE_CURRENT_12V        79.16f    // Current scaling: Raw * 79.16  / 32768
#define SS_SCALE_DIVISOR            32768.0f  // Common divisor

// ============================================================================
// Charge State Values (charge_state, 0x0011)
// ============================================================================
enum SolarChargeState : uint8_t {
  CHARGE_STATE_START      = 0,  // Controller starting up
  CHARGE_STATE_NIGHT_CHECK= 1,  // Checking for night
  CHARGE_STATE_DISCONNECT = 2,  // Disconnected
  CHARGE_STATE_NIGHT      = 3,  // Night mode (solar offline)
  CHARGE_STATE_FAULT      = 4,  // Fault condition
  CHARGE_STATE_BULK       = 5,  // Bulk: all the current the array gives, the battery below its absorption voltage
  CHARGE_STATE_ABSORPTION = 6,  // Absorption: the battery held at its absorption (top-up) voltage
  CHARGE_STATE_FLOAT      = 7,  // Float: the absorption time is done, the battery held at its float voltage
  CHARGE_STATE_EQUALIZE   = 8   // Equalization charging (if configured)
};

// ============================================================================
// Load State Values (load_state, 0x001A)
// ============================================================================
enum SolarLoadState : uint8_t {
  LOAD_STATE_START       = 0,
  LOAD_STATE_ON          = 1,
  LOAD_STATE_LVD_WARNING = 2,  // still on: the LVD is near
  LOAD_STATE_LVD         = 3,  // off: low voltage disconnect
  LOAD_STATE_FAULT       = 4,
  LOAD_STATE_DISCONNECT  = 5
};

// ============================================================================
// Fault and alarm bits
// ============================================================================
// array_fault (0x0012)
#define SS_ARRAY_FAULT_OVERCURRENT      (1u << 0)  // Overcurrent
#define SS_ARRAY_FAULT_FETS_SHORTED     (1u << 1)  // FETs shorted
#define SS_ARRAY_FAULT_SOFTWARE         (1u << 2)  // Software bug
#define SS_ARRAY_FAULT_BATTERY_HVD      (1u << 3)  // Battery HVD
#define SS_ARRAY_FAULT_ARRAY_HVD        (1u << 4)  // Array HVD
#define SS_ARRAY_FAULT_EEPROM_EDIT      (1u << 5)  // EEPROM setting edit: settings were written, a restart is due
#define SS_ARRAY_FAULT_RTS_SHORTED      (1u << 6)  // RTS shorted
#define SS_ARRAY_FAULT_RTS_DISCONNECTED (1u << 7)  // RTS was valid, now disconnected
#define SS_ARRAY_FAULT_LOCAL_TEMP       (1u << 8)  // Local temperature sensor failed

// load_fault (0x001B)
#define SS_LOAD_FAULT_SHORT_CIRCUIT     (1u << 0)  // External short circuit
#define SS_LOAD_FAULT_OVERCURRENT       (1u << 1)  // Overcurrent
#define SS_LOAD_FAULT_FETS_SHORTED      (1u << 2)  // FETs shorted
#define SS_LOAD_FAULT_SOFTWARE          (1u << 3)  // Software bug
#define SS_LOAD_FAULT_HVD               (1u << 4)  // HVD
#define SS_LOAD_FAULT_HEATSINK_HOT      (1u << 5)  // Heatsink over-temperature
#define SS_LOAD_FAULT_EEPROM_EDIT       (1u << 6)  // EEPROM setting edit: settings were written, a restart is due

// alarm (0x0023 and 0x0024). The specification does not say how its 24 bits split across the two
// words. As in OptaView's profile, bit 0 is taken as the lowest bit of the low word (0x0024), so
// the alarms are (0x0023 << 16) | 0x0024. Bits 8, 9, 12 and 13 have no name.
#define SS_ALARM_RTS_OPEN               (1ul << 0)   // RTS open
#define SS_ALARM_RTS_SHORTED            (1ul << 1)   // RTS shorted
#define SS_ALARM_RTS_DISCONNECTED       (1ul << 2)   // RTS disconnected
#define SS_ALARM_THS_OPEN               (1ul << 3)   // Ths open: the heatsink sensor
#define SS_ALARM_THS_SHORTED            (1ul << 4)   // Ths shorted
#define SS_ALARM_HOT                    (1ul << 5)   // SSMPPT hot
#define SS_ALARM_CURRENT_LIMIT          (1ul << 6)   // Current limit
#define SS_ALARM_CURRENT_OFFSET         (1ul << 7)   // Current offset
#define SS_ALARM_UNCALIBRATED           (1ul << 10)  // Uncalibrated
#define SS_ALARM_RTS_MISWIRE            (1ul << 11)  // RTS miswire
#define SS_ALARM_MISWIRE                (1ul << 14)  // Miswire
#define SS_ALARM_FET_OPEN               (1ul << 15)  // FET open
#define SS_ALARM_P12                    (1ul << 16)  // P12
#define SS_ALARM_HIGH_VA_CURRENT_LIMIT  (1ul << 17)  // High Va current limit

// ============================================================================
// Battery Health Thresholds (12V AGM System)
// ============================================================================
#define BATTERY_VOLTAGE_CRITICAL    11.5f   // Critical low voltage (immediate alarm)
#define BATTERY_VOLTAGE_LOW         11.8f   // Low voltage warning
#define BATTERY_VOLTAGE_NORMAL      12.0f   // Normal minimum voltage
#define BATTERY_VOLTAGE_FLOAT       13.4f   // Float charge voltage (fully charged)
#define BATTERY_VOLTAGE_HIGH        14.8f   // High voltage warning (overcharge)

// ============================================================================
// Solar Data Structure
// ============================================================================
struct SolarData {
  // Real-time measurements
  float batteryVoltage;       // Battery voltage (V)
  float arrayVoltage;         // Solar panel voltage (V)
  float chargeCurrent;        // Charging current (A)
  float loadCurrent;          // Load current (A)
  int8_t heatsinkTemp;        // Heatsink temperature (°C)
  int8_t batteryTemp;         // Battery temperature (°C): the RTS when one is connected, else the controller's own sensor

  // Status
  SolarChargeState chargeState; // Current charge state
  uint16_t arrayFaults;       // SS_ARRAY_FAULT_* bits
  uint16_t loadFaults;        // SS_LOAD_FAULT_* bits
  uint32_t alarms;            // SS_ALARM_* bits
  bool loadOn;                // Load output on (LOAD_STATE_ON or LOAD_STATE_LVD_WARNING)

  // Daily statistics: the client's own, since the last daily report (resetDailyStats())
  float batteryVoltageMinDaily; // Lowest battery voltage
  float batteryVoltageMaxDaily; // Highest battery voltage
  float ampHoursDaily;        // Amp-hours charged, the controller's own "today" (SS_REG_AHC_DAILY)

  // Derived health indicators
  bool batteryHealthy;        // Overall battery health status
  bool solarHealthy;          // Overall solar system health status
  bool hasFault;              // Any fault condition present
  bool hasAlarm;              // Any alarm condition present
  bool isCharging;            // Currently charging (bulk, absorption, or equalize)
  bool isFullyCharged;        // Battery fully charged (float mode)
  bool noChargeAlertActive;   // Solar panel daylight but zero charge alert active
  bool dailyStatsSeeded;      // True when software daily tracking has been seeded
  
  // Communication status
  bool communicationOk;       // Last Modbus read successful (5-poll smoothed via SOLAR_COMM_FAILURE_THRESHOLD)
  uint32_t lastReadMillis;    // Timestamp of last successful read (millis() since boot; 0 = never)
  uint8_t consecutiveErrors;  // Count of consecutive read errors

  // R2 / R6 (v2.0.53): per-poll observability for ambiguous-failure disambiguation.
  bool lastPollOk;            // R6: most recent poll succeeded (NO smoothing); for realtime indicator
  uint32_t firstReadMillis;   // R2: millis() of the FIRST successful read since boot (0 = never)

  // v2.1.3: 12V vs 24V bank classification (LOCAL-ONLY — not transmitted; see header note
  // above the OPTA_SUPPLY_* constants). 0 = unknown (no successful poll yet), else 12 or 24.
  uint8_t systemNominalV;
};

// ============================================================================
// Solar Alert Types
// ============================================================================
enum SolarAlertType : uint8_t {
  SOLAR_ALERT_NONE              = 0,
  SOLAR_ALERT_BATTERY_LOW       = 1,  // Battery below low threshold
  SOLAR_ALERT_BATTERY_CRITICAL  = 2,  // Battery below critical threshold
  SOLAR_ALERT_BATTERY_HIGH      = 3,  // Battery overvoltage
  SOLAR_ALERT_FAULT             = 4,  // SunSaver fault condition
  SOLAR_ALERT_ALARM             = 5,  // SunSaver alarm condition
  SOLAR_ALERT_COMM_FAILURE      = 6,  // Modbus communication failure
  SOLAR_ALERT_HEATSINK_TEMP     = 7,  // Heatsink overtemperature
  SOLAR_ALERT_NO_CHARGE         = 8   // No charging during daylight (potential panel issue)
};

// ============================================================================
// 12V vs 24V system classification (v2.1.3)
// ============================================================================
// The Opta / A0602 supply spec is 12-24 VDC +/-20% => 28.8V absolute maximum.
// A "24V" lead-acid bank exceeds that while charging (absorption 28.2-28.8V,
// equalization 29.2-30.6V, plus RTS cold-weather temperature compensation), so
// 24V installations MUST power the Opta through a DC-DC buck converter (see
// Bill of Materials). Firmware classifies the bank from the live battery
// voltage on every successful poll (SolarData.systemNominalV = 12 or 24) and
// keeps it LOCAL-ONLY for future protective logic — it is deliberately NOT
// transmitted (bandwidth): the server can derive the class from the actual
// voltage readings already attached to telemetry (`v`) and the daily report
// (`v`/`vs`, plus the solar block's `bv`), all emitted only when a voltage
// source has data. No alert/SMS fires on this today (user decision
// 2026-07-02), and firmware does not write a charge voltage cap over Modbus
// (temperature compensation would defeat a cap anyway). The only writes are
// those of charger programming (see "Stored charge settings" above), which a
// config has to ask for and which refuses anything but a 12 V system. The
// constants below are retained for future logic.
#ifndef OPTA_SUPPLY_ABS_MAX_V
#define OPTA_SUPPLY_ABS_MAX_V  28.8f  // Opta datasheet: 12-24 VDC +/-20%
#endif
#ifndef OPTA_SUPPLY_WARN_V
#define OPTA_SUPPLY_WARN_V     28.0f  // margin below the absolute ceiling (future use)
#endif
#ifndef SOLAR_24V_SYSTEM_MIN_V
#define SOLAR_24V_SYSTEM_MIN_V 17.0f  // live battery V above this => 24V bank
#endif

// ============================================================================
// Solar Configuration
// ============================================================================
struct SolarConfig {
  bool enabled;               // true = solar monitoring enabled
  uint8_t modbusSlaveId;      // Modbus slave ID (default: 1)
  uint16_t modbusBaudRate;    // Baud rate (default: 9600)
  uint16_t modbusTimeoutMs;   // Modbus read timeout (default: 1000ms)
  uint16_t pollIntervalSec;   // Polling interval (default: 60 seconds)
  
  // Battery thresholds (customizable for different battery types)
  float batteryLowVoltage;    // Low voltage warning threshold (default: 11.8V)
  float batteryCriticalVoltage; // Critical voltage alarm threshold (default: 11.5V)
  float batteryHighVoltage;   // High voltage warning threshold (default: 14.8V)
  
  // Alert configuration
  bool alertOnLowBattery;     // Send alert on low battery (default: true)
  bool alertOnFault;          // Send alert on SunSaver fault (default: true)
  bool alertOnCommFailure;    // Send alert on communication failure (default: false)
  bool includeInDailyReport;  // Include solar data in daily report (default: true)
};

// ============================================================================
// Default Configuration Values
// ============================================================================
#define SOLAR_DEFAULT_SLAVE_ID          1
#define SOLAR_DEFAULT_BAUD_RATE         9600
#define SOLAR_DEFAULT_TIMEOUT_MS        1000
#define SOLAR_DEFAULT_POLL_INTERVAL_SEC 60
#define SOLAR_COMM_FAILURE_THRESHOLD    5

// Bounded retry for the VERIFIED realtime register block (0x0008..0x000C) only, for RS-485
// noise immunity. Total Modbus attempts per poll = SOLAR_REALTIME_MAX_ATTEMPTS (1 initial +
// retries). Worst-case added blocking on a dead bus is roughly (attempts-1) * (2 * timeout)
// because each attempt may try both function codes; the watchdog is kicked before each attempt.
// The status/fault/daily blocks are intentionally NOT retried. Note the interaction
// with SOLAR_COMM_FAILURE_THRESHOLD: up to (attempts * threshold) low-level reads can occur
// before a comm failure is declared, which is the intended trade for transient-noise tolerance.
#ifndef SOLAR_REALTIME_MAX_ATTEMPTS
#define SOLAR_REALTIME_MAX_ATTEMPTS     3
#endif
#ifndef SOLAR_RETRY_DELAY_MS
#define SOLAR_RETRY_DELAY_MS            100
#endif

// Charger check: how many registers one serviceChargerCheck() call reads (each is a Modbus
// request of its own), and the least time between two such calls. The check reads 49 registers,
// so with these figures loop() is held for about as long as one poll, 13 times over a few seconds.
#ifndef SOLAR_CHARGER_CHECK_REGISTERS_PER_STEP
#define SOLAR_CHARGER_CHECK_REGISTERS_PER_STEP 4
#endif
#ifndef SOLAR_CHARGER_CHECK_STEP_GAP_MS
#define SOLAR_CHARGER_CHECK_STEP_GAP_MS        250
#endif

// ============================================================================
// SolarManager Class Interface
// ============================================================================
#ifdef __cplusplus

#include "TankAlarm_SunSaverProgram.h"

class SolarManager {
public:
  // Writes the journal of charger programming to the client's flash. True only once the file
  // holds it whole. See beginChargerProgramming().
  typedef bool (*ChargerJournalWriter)(const sunsaver::Journal &journal);

  SolarManager();
  
  // Initialization
  bool begin(const SolarConfig& config);
  void end();
  
  // Configuration
  void setConfig(const SolarConfig& config);
  const SolarConfig& getConfig() const { return _config; }
  
  // Polling (call periodically from main loop)
  // Returns true when a poll attempt was performed, regardless of read success.
  bool poll(unsigned long nowMillis);

  // True for one Modbus timeout after a request that got no usable reply. Its reply may still
  // be on its way, and the next request would take it for its own answer, so the poll and the
  // charger check both keep off the line meanwhile: a poll that is due waits (poll() returns
  // false) and so does the next step of a charger check.
  bool lineResting() const;
  
  // Data access
  const SolarData& getData() const { return _data; }
  
  // Health assessment
  SolarAlertType checkAlerts() const;
  const char* getAlertDescription(SolarAlertType alert) const;
  const char* getChargeStateDescription() const;
  const char* getFaultDescription() const;
  const char* getAlarmDescription() const;
  
  // Status helpers
  bool isEnabled() const { return _config.enabled && _initialized; }
  bool isCommunicationOk() const { return _data.communicationOk; }
  bool isBatteryHealthy() const { return _data.batteryHealthy; }
  bool isSolarHealthy() const { return _data.solarHealthy; }

  // R3 (v2.0.53): expose the Modbus library init state separately from the OR'd
  // isEnabled(). Lets the client emit "configured but library failed to init" in
  // telemetry instead of going silent (which looks identical to "user disabled solar").
  bool isInitialized() const { return _initialized; }

  // Modbus diagnostics for field troubleshooting of scOk:0. These let the operator
  // distinguish a transport failure (timeout/CRC/illegal address -> getLastModbusError())
  // from a CRC-valid read rejected by the plausibility clamp (wasLastReadImplausible()).
  uint32_t getModbusErrorCount() const;
  const char* getLastModbusError() const;
  uint16_t getLastModbusFailAddress() const;
  uint16_t getLastModbusResponseMs() const;
  bool wasLastReadImplausible() const;
  void resetModbusErrorStats();

  // R1 / R5 (v2.0.53): per-error-type classification. getLastModbusErrorTag() returns one
  // of "to" (Timeout), "crc" (CRC error), "ida" (Illegal data address), "ifu" (Illegal
  // function), "?" (other). Per-bucket lifetime counts mirror sSolarModbusErrorCount; the
  // sum of the five buckets equals getModbusErrorCount() in steady state.
  const char* getLastModbusErrorTag() const;
  uint32_t getModbusErrTimeout()      const;
  uint32_t getModbusErrCrc()          const;
  uint32_t getModbusErrIllegalAddr()  const;
  uint32_t getModbusErrIllegalFunc()  const;
  uint32_t getModbusErrOther()        const;

  // Starts the daily figures (the battery's lowest and highest voltage) over with the next poll.
  // The client calls it once a daily report has gone out, so that each report covers its own day.
  void resetDailyStats();

  // Force a poll execution on the next opportunity
  void forcePollSoon() { _lastPollMillis = 0; }

  // ---------------------------------------------------------------------------------------
  // Charger check: what the controller has stored (charging profile, LVD and LVR, DIP
  // switches), read a few registers at a time so that loop() keeps running. It only reads.
  // The caller judges the result with sunsaver::evaluate() (TankAlarm_SunSaverSettings.h).
  // ---------------------------------------------------------------------------------------

  // Starts a pass over the registers. What an earlier pass read is dropped. Returns false
  // when no pass can start: the charger is not enabled, or no poll has succeeded yet (the
  // poll finds out which Modbus function the controller answers).
  bool startChargerCheck();

  // Gives up a pass that is under way.
  void cancelChargerCheck();

  // Call from loop(). Reads the next few registers when it is time to. Returns true in the
  // call that ends a pass; then chargerCheckDone() or chargerCheckFailed() says how.
  bool serviceChargerCheck(unsigned long nowMillis);

  bool chargerCheckActive() const { return _chargerCheck.active(); }
  bool chargerCheckDone() const { return _chargerCheck.done(); }      // the snapshot is complete
  bool chargerCheckFailed() const { return _chargerCheck.failed(); }  // the link was not fit for it

  // The registers of the last pass. Complete only when chargerCheckDone().
  const sunsaver::Snapshot& chargerSnapshot() const { return _chargerCheck.snapshot(); }

  // True once a pass has completed since begin().
  bool chargerSettingsRead() const { return _chargerSettingsRead; }

  // The voltage the charger holds a full battery at, as this firmware reads the battery. The
  // no-charge alert compares the battery with it. 0 (the default) means "not known": the alert
  // then uses BATTERY_VOLTAGE_FLOAT per 12 V of pack. The client sets it from the charger check,
  // and begin() clears it.
  void setFloatReference(float volts) { _floatReferenceV = volts; }
  float getFloatReference() const { return _floatReferenceV; }

  // ---------------------------------------------------------------------------------------
  // Charger programming: writes a LiFePO4 charging profile and the LVD and LVR into the
  // controller when the client's config asks for it (sunsaver::Supervisor in
  // TankAlarm_SunSaverProgram.h decides what is done and when). The client keeps the journal in
  // its flash and sends the results to the server. None of it is touched by begin() and end(),
  // which a config change calls to restart the link.
  //
  // With TANKALARM_SUNSAVER_PROGRAMMING 0 nothing here sends to the controller: every request
  // is refused with REFUSE_NOT_BUILT, the journal keeps only the requests answered and the
  // result that has yet to go out, and serviceChargerProgramming() does nothing.
  // ---------------------------------------------------------------------------------------

  // Once at start, with the journal from flash (sunsaver::cautiousJournal() when the file is
  // there and cannot be read, sunsaver::emptyJournal() when there is none) and the function that
  // writes it.
  void beginChargerProgramming(const sunsaver::Journal &journal, ChargerJournalWriter writer);

  // A config carried a request. See sunsaver::Supervisor::request(): false for a request that
  // this client has dealt with already. The request is in flash, or answered, on return.
  bool requestChargerProgram(uint32_t requestId, const sunsaver::Profile &profile, uint16_t profileErrors,
                             uint32_t clientRefusals, bool loadCurrentAllowed, double epochNow);

  // A request that waits is given up (see sunsaver::Supervisor::withdraw()). False when none
  // waits.
  bool withdrawChargerProgram(uint32_t refusals, double epochNow);

  // Call from every pass of loop(), whether the charger is enabled or not: a request that waits
  // runs out of time here too. `allowed` is false while the client must not talk to the
  // controller (a firmware update is being staged, or a poll has just talked to it). Nothing is
  // sent while the line rests. A request runs only once a poll has succeeded. A duty left by a
  // run that was cut short goes to the controller that run talked to, whatever the config names
  // now, and is made on its own timetable when no poll speaks for that controller: with the
  // charger off in the config, the link is started for it and stopped after it (see
  // sunsaver::Supervisor::mayTalk()). Returns true when a run talked to it: that held loop() for the whole run
  // (about 10 seconds, never more than a minute: see sunsaver::RUN_LIMIT_MS; with the watchdog
  // fed), and the controller may hold other settings now. A charger check under way is dropped
  // then, and the next poll is due at once.
  bool serviceChargerProgramming(double epochNow, bool allowed);

  // Whether programming would talk to the controller now if the line were free: a run or a look
  // at the controller is due (see sunsaver::Supervisor::wantsLine()). The client's loop then lets
  // it have the pass after a poll, when every pass is due to poll. False without programming.
  bool chargerProgrammingWantsLine(double epochNow) const;

  // The result that waits to be sent (reportDue(), report(), reportRequestId(), reportMillis(),
  // reportEpoch()), the journal, and whether a request or a duty is open.
  const sunsaver::Supervisor& chargerProgramming() const { return _programming; }
  // The result has gone to the Notecard or into the note buffer in flash.
  void chargerProgramReportTaken();

private:
  SolarConfig _config;
  SolarData _data;
  bool _initialized;
  unsigned long _lastPollMillis;
  uint8_t _noChargeConsecutivePolls;
  uint8_t _cachedHoldingFC; // Caches whether FC03 (holding) or FC04 (inputs) works: 3 = holding, 4 = inputs, 0 = unknown
  float _floatReferenceV;   // See setFloatReference(). 0 = not known.

  // Charger check
  sunsaver::SnapshotReader _chargerCheck;
  unsigned long _chargerCheckStepMillis;  // when the last step ran
  unsigned long _chargerCheckGapMs;       // the pause before the next one (0 before the first)
  bool _chargerSettingsRead;

  // Charger programming
  class JournalFile : public sunsaver::JournalStore {
   public:
    JournalFile() : writer(nullptr) {}
    bool saveJournal(const sunsaver::Journal &journal) override { return writer != nullptr && writer(journal); }
    ChargerJournalWriter writer;
  };
  sunsaver::Supervisor _programming;
  JournalFile _journalFile;
  
  // Modbus communication
  bool startModbusLink();  // ModbusRTUClient on the RS-485 port, as _config says: false when it does not start
  bool readRegisters();
  float scaleVoltage(uint16_t raw) const;
  float scaleCurrent(uint16_t raw) const;
  
  // Health assessment
  void updateHealthStatus();
};

#endif // __cplusplus

#endif // TANKALARM_SOLAR_H
