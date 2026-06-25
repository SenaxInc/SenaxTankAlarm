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

// Temperature
#define SS_REG_HEATSINK_TEMP        0x001B  // Register 28: Heatsink Temperature (°C, signed)
#define SS_REG_BATTERY_TEMP         0x001C  // Register 29: Battery Temperature (°C, signed, if RTS connected)

// Status Registers
#define SS_REG_CHARGE_STATE         0x002B  // Register 44: Charge State
#define SS_REG_FAULTS               0x002C  // Register 45: Faults (bitfield)
#define SS_REG_ALARMS               0x002E  // Register 47: Alarms (bitfield)
#define SS_REG_LOAD_STATE           0x002F  // Register 48: Load State

// Daily Statistics
#define SS_REG_BATTERY_V_MIN_DAILY  0x003D  // Register 62: Minimum Battery Voltage Today
#define SS_REG_BATTERY_V_MAX_DAILY  0x003E  // Register 63: Maximum Battery Voltage Today
#define SS_REG_AH_DAILY             0x0034  // Register 53: Amp-hours charged today
#define SS_REG_WH_DAILY             0x0038  // Register 57: Watt-hours charged today (if available)

// ============================================================================
// Charge Setpoint Registers (used for chemistry verification vs. UI selection)
// ============================================================================
// SunSaver MPPT chemistry is selected via a 4-position DIP switch on the unit
// (Sealed/Gel/Flooded/L16) and CANNOT be set over Modbus. However the active
// setpoints derived from that DIP selection are exposed in RAM and reflect
// whatever chemistry the controller booted with. We read them back to verify
// the installer flipped the right switches against what was selected in the
// web UI. Addresses per SunSaver MPPT MODBUS Specification:
//   V_reg   = absorption / regulation voltage
//   V_float = float voltage
//   V_eq    = equalization voltage (zero for chemistries that don't equalize)
#define SS_REG_V_REG                0x0033  // Regulation (absorption) setpoint
#define SS_REG_V_FLOAT              0x0035  // Float setpoint
#define SS_REG_V_EQ                 0x0036  // Equalization setpoint (0 = disabled)

// ============================================================================
// Scaling Factors for 12V System
// Formula: Actual = (Raw * Scale) / 32768
// ============================================================================
// Per Morningstar SunSaver MPPT PDU: V_PU (12V controller) = 96.667 V, I_PU = 79.16 A.
#define SS_SCALE_VOLTAGE_12V        96.667f   // Voltage scaling: Raw * 96.667 / 32768
#define SS_SCALE_CURRENT_12V        79.16f    // Current scaling: Raw * 79.16  / 32768
#define SS_SCALE_DIVISOR            32768.0f  // Common divisor

// ============================================================================
// Charge State Values
// ============================================================================
enum SolarChargeState : uint8_t {
  CHARGE_STATE_START      = 0,  // Controller starting up
  CHARGE_STATE_NIGHT_CHECK= 1,  // Checking for night
  CHARGE_STATE_DISCONNECT = 2,  // Disconnected
  CHARGE_STATE_NIGHT      = 3,  // Night mode (solar offline)
  CHARGE_STATE_FAULT      = 4,  // Fault condition
  CHARGE_STATE_BULK       = 5,  // Bulk charging (battery below 80%)
  CHARGE_STATE_ABSORPTION = 6,  // Absorption charging (battery 80-100%)
  CHARGE_STATE_FLOAT      = 7,  // Float charging (battery fully charged)
  CHARGE_STATE_EQUALIZE   = 8   // Equalization charging (if configured)
};

// ============================================================================
// Fault Bitfield Definitions (Register 45)
// ============================================================================
#define SS_FAULT_OVERCURRENT        (1 << 0)  // Bit 0: Overcurrent
#define SS_FAULT_FET_SHORT          (1 << 1)  // Bit 1: FET short
#define SS_FAULT_SOFTWARE           (1 << 2)  // Bit 2: Software fault
#define SS_FAULT_BATT_HVD           (1 << 3)  // Bit 3: Battery high voltage disconnect
#define SS_FAULT_ARRAY_HVD          (1 << 4)  // Bit 4: Array high voltage disconnect
#define SS_FAULT_DIP_SW_FAULT       (1 << 5)  // Bit 5: DIP switch changed
#define SS_FAULT_RESET_FAULT        (1 << 6)  // Bit 6: Settings reset (EEPROM corrupt)
#define SS_FAULT_RTS_DISCONN        (1 << 7)  // Bit 7: RTS (temp sensor) disconnected
#define SS_FAULT_RTS_SHORT          (1 << 8)  // Bit 8: RTS shorted
#define SS_FAULT_HEATSINK_LIMIT     (1 << 9)  // Bit 9: Heatsink temperature limit

// ============================================================================
// Alarm Bitfield Definitions (Register 47)
// ============================================================================
#define SS_ALARM_RTS_OPEN           (1 << 0)  // Bit 0: RTS open
#define SS_ALARM_RTS_SHORT          (1 << 1)  // Bit 1: RTS shorted
#define SS_ALARM_RTS_DISCONN        (1 << 2)  // Bit 2: RTS disconnected
#define SS_ALARM_HEATSINK_LIMIT     (1 << 3)  // Bit 3: Heatsink temperature limit
#define SS_ALARM_CURRENT_LIMIT      (1 << 4)  // Bit 4: Current limit reached
#define SS_ALARM_CURRENT_OFFSET     (1 << 5)  // Bit 5: Current offset error
#define SS_ALARM_BATT_SENSE         (1 << 6)  // Bit 6: Battery sense out of range
#define SS_ALARM_BATT_SENSE_DISC    (1 << 7)  // Bit 7: Battery sense disconnected
#define SS_ALARM_UNCALIBRATED       (1 << 8)  // Bit 8: Controller uncalibrated
#define SS_ALARM_RTS_MISWIRE        (1 << 9)  // Bit 9: RTS miswired
#define SS_ALARM_HVD                (1 << 10) // Bit 10: High voltage disconnect
#define SS_ALARM_LOG_TIMEOUT        (1 << 11) // Bit 11: Log timeout
#define SS_ALARM_EEPROM             (1 << 12) // Bit 12: EEPROM access error

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
  int8_t batteryTemp;         // Battery temperature (°C, if RTS connected)
  
  // Status
  SolarChargeState chargeState; // Current charge state
  uint16_t faults;            // Fault bitfield
  uint16_t alarms;            // Alarm bitfield
  bool loadOn;                // Load output state
  
  // Daily statistics
  float batteryVoltageMinDaily; // Minimum battery voltage today
  float batteryVoltageMaxDaily; // Maximum battery voltage today
  float ampHoursDaily;        // Amp-hours charged today
  float wattHoursDaily;       // Watt-hours charged today
  
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

  // Charge setpoints read from controller (reflects DIP-switch chemistry).
  // setpointsValid is false until we successfully read them at least once.
  bool setpointsValid;
  float vRegSetpoint;         // Absorption / regulation voltage (V)
  float vFloatSetpoint;       // Float voltage (V)
  float vEqSetpoint;          // Equalization voltage (V); 0 = disabled
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
// The setpoint and status/fault/daily blocks are intentionally NOT retried. Note the interaction
// with SOLAR_COMM_FAILURE_THRESHOLD: up to (attempts * threshold) low-level reads can occur
// before a comm failure is declared, which is the intended trade for transient-noise tolerance.
#ifndef SOLAR_REALTIME_MAX_ATTEMPTS
#define SOLAR_REALTIME_MAX_ATTEMPTS     3
#endif
#ifndef SOLAR_RETRY_DELAY_MS
#define SOLAR_RETRY_DELAY_MS            100
#endif

// ============================================================================
// SolarManager Class Interface
// ============================================================================
#ifdef __cplusplus

class SolarManager {
public:
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

  // Reset daily statistics (call at midnight or report time)
  void resetDailyStats();

  // Force a poll execution on the next opportunity
  void forcePollSoon() { _lastPollMillis = 0; }

  // Chemistry verification result vs. user-selected battery type.
  enum ChemistryCheck : uint8_t {
    CHEMISTRY_CHECK_OK             = 0,  // Setpoints match expected chemistry
    CHEMISTRY_CHECK_PENDING        = 1,  // Setpoints not yet read
    CHEMISTRY_CHECK_MISMATCH       = 2,  // Setpoints out of range for selected chemistry
    CHEMISTRY_CHECK_VOLTAGE_MISMATCH = 3 // Setpoints suggest a different pack voltage
  };

  /**
   * Compare the controller's read-back setpoints against the chemistry/voltage
   * the user selected in the web UI. Returns CHEMISTRY_CHECK_PENDING until
   * setpointsValid is true. The check is intentionally lenient: it only flags
   * clear mismatches (e.g. AGM selected but V_eq is non-zero, or pack voltage
   * is half of expected).
   *
   * @param expectedType    Chemistry the user picked in the web UI
   * @param nominalVoltage  Pack voltage the user picked (12 or 24)
   * @param outDescription  Optional buffer (>= 96 chars) for human-readable explanation
   * @param descriptionLen  Length of outDescription buffer
   */
  ChemistryCheck verifyChemistry(uint8_t expectedType,
                                 uint8_t nominalVoltage,
                                 char* outDescription = nullptr,
                                 size_t descriptionLen = 0) const;
  
private:
  SolarConfig _config;
  SolarData _data;
  bool _initialized;
  unsigned long _lastPollMillis;
  uint8_t _noChargeConsecutivePolls;
  uint8_t _cachedHoldingFC; // Caches whether FC03 (holding) or FC04 (inputs) works: 3 = holding, 4 = inputs, 0 = unknown
  
  // Modbus communication
  bool readRegisters();
  float scaleVoltage(uint16_t raw) const;
  float scaleCurrent(uint16_t raw) const;
  
  // Health assessment
  void updateHealthStatus();
};

#endif // __cplusplus

#endif // TANKALARM_SOLAR_H
