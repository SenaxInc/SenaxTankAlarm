/**
 * TankAlarm_Solar.cpp
 * 
 * SunSaver MPPT Solar Charger Monitoring Implementation
 * Uses ArduinoModbus library for Modbus RTU communication over RS-485
 * 
 * Copyright (c) 2025-2026 Senax Inc. All rights reserved.
 */

#include "TankAlarm_Solar.h"
#include "TankAlarm_Battery.h"
#include "TankAlarm_Common.h"

#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)

#include <ArduinoRS485.h>
#include <ArduinoModbus.h>

#if defined(ARDUINO_OPTA)
static RS485Class sSolarRS485(Serial2, PB_10, PB_14, PB_13);
#define TANKALARM_SOLAR_RS485 sSolarRS485
#else
#define TANKALARM_SOLAR_RS485 RS485
#endif

// Static message buffers for descriptions (avoid dynamic allocation)
static char _faultDescBuffer[128];
static char _alarmDescBuffer[128];

// --- Modbus diagnostics (field troubleshooting of scOk:0) ---
// File-local because ModbusRTUClient lives in this translation unit. Exposed via the
// SolarManager accessor methods defined below. These distinguish a transport failure
// (timeout / CRC / illegal address) from a CRC-valid read rejected by the plausibility
// clamp. The failure COUNT is incremented once per logical read (in
// readRegistersWithFallback) so a normal FC03->FC04 fallback probe is not double-counted.
static uint32_t sSolarModbusErrorCount = 0;
static char     sSolarLastModbusError[40] = {0};
static uint16_t sSolarLastFailAddress = 0;
static uint16_t sSolarLastResponseMs = 0;
static bool     sSolarLastReadImplausible = false;

// R1 / R5 (v2.0.53): per-error-type buckets and the most recent classified tag. The tag is
// set by classifyModbusErrorTag() inside captureSolarModbusError(); the buckets are
// incremented exactly once per LOGICAL failure inside readRegistersWithFallback() so a
// fallback FC probe is not double-counted. In steady state the five buckets sum to
// sSolarModbusErrorCount. Reset together by resetModbusErrorStats().
static uint32_t sSolarErrTimeout      = 0;  // "Timeout"
static uint32_t sSolarErrCrc          = 0;  // "CRC error" / "CRC mismatch"
static uint32_t sSolarErrIllegalAddr  = 0;  // "Illegal data address"
static uint32_t sSolarErrIllegalFunc  = 0;  // "Illegal function"
static uint32_t sSolarErrOther        = 0;  // catch-all (unknown / library-init failure)
static char     sSolarLastErrorTag[8]  = {0};  // short tag: "to"|"crc"|"ida"|"ifu"|"?"

// Classify the last ModbusRTUClient.lastError() string into one of five short tags.
// Uses substring match because the library returns prose like "Illegal data address"
// not enum codes. Tag set here is read by readRegistersWithFallback() to pick the
// bucket to increment.
static void classifyModbusErrorTag(const char *errText) {
  if (!errText || !errText[0]) {
    strlcpy(sSolarLastErrorTag, "?", sizeof(sSolarLastErrorTag));
    return;
  }
  if (strstr(errText, "Timeout") || strstr(errText, "timeout") || strstr(errText, "timed") || strstr(errText, "timed out")) {
    strlcpy(sSolarLastErrorTag, "to", sizeof(sSolarLastErrorTag));
  } else if (strstr(errText, "CRC") || strstr(errText, "crc")) {
    strlcpy(sSolarLastErrorTag, "crc", sizeof(sSolarLastErrorTag));
  } else if (strstr(errText, "Illegal data address") || strstr(errText, "data address")) {
    strlcpy(sSolarLastErrorTag, "ida", sizeof(sSolarLastErrorTag));
  } else if (strstr(errText, "Illegal function") || strstr(errText, "function")) {
    strlcpy(sSolarLastErrorTag, "ifu", sizeof(sSolarLastErrorTag));
  } else {
    strlcpy(sSolarLastErrorTag, "?", sizeof(sSolarLastErrorTag));
  }
}

// Increment the per-bucket lifetime counter that matches sSolarLastErrorTag. Called
// once per logical failure where sSolarModbusErrorCount is also incremented, so the
// per-bucket totals sum to the lifetime total.
static void incrementSolarErrorBucket() {
  if      (!strcmp(sSolarLastErrorTag, "to"))  ++sSolarErrTimeout;
  else if (!strcmp(sSolarLastErrorTag, "crc")) ++sSolarErrCrc;
  else if (!strcmp(sSolarLastErrorTag, "ida")) ++sSolarErrIllegalAddr;
  else if (!strcmp(sSolarLastErrorTag, "ifu")) ++sSolarErrIllegalFunc;
  else                                          ++sSolarErrOther;
}

// The line rest. A request that got no usable reply may still be answered: late, after this
// side gave up. A Modbus reply does not say which register it answers, so a request sent into
// that silence would take the stray reply for its own. After such a request everything that
// sends on this link (the poll, the charger check, the startup probe) therefore leaves the line
// alone for one Modbus timeout: SolarManager::lineResting(). A request the controller refused
// with a Modbus exception has had its answer and needs no rest. The rest is the timeout of the
// request that missed: a config update that shortens the timeout restarts the link at once, and
// a reply to the request before it can still be on its way. Every request waits for the end of
// the rest before it goes (waitForLineRest()): a retry within a poll too, and the other function
// code after a miss.
static unsigned long sSolarLastMissMillis = 0;
static unsigned long sSolarMissRestMs = 0;     // how long the line rests after that miss
static bool          sSolarMissSeen = false;
static unsigned long sSolarTimeoutMs = 0;      // the timeout ModbusRTUClient was last given
static uint8_t       sSolarPollMisses = 0;     // misses since the poll under way began: see pollMayGoOn()

// True for the text of a Modbus exception that the controller itself answered with: exception
// codes 1 to 8, as ModbusRTUClient.lastError() words them (libmodbus's modbus_strerror()). The
// controller is there and has answered, if only to say it is busy, and the line is free again.
// Anything else means that no usable reply came from the controller. Codes 10 and 11 among them ("Gateway path
// unavailable", "Target device failed to respond"): a gateway sends them when the device behind
// it did not answer. And "Invalid exception code", which libmodbus also gives for an exception
// frame of the wrong length or for another function code: that can be a late reply to an earlier
// request.
static bool solarErrorIsRefusal(const char *text) {
  static const char *const kExceptions[] = {
      "Illegal function",                // 1
      "Illegal data address",            // 2
      "Illegal data value",              // 3
      "Slave device or server failure",  // 4
      "Acknowledge",                     // 5
      "Slave device or server is busy",  // 6
      "Negative acknowledge",            // 7
      "Memory parity error",             // 8
  };
  for (size_t i = 0; i < sizeof(kExceptions) / sizeof(kExceptions[0]); ++i) {
    if (strcmp(text, kExceptions[i]) == 0) {
      return true;
    }
  }
  return false;
}

// Drain any stale bytes left in the RS-485 UART RX buffer before a new Modbus
// transaction. ArduinoModbus enables MODBUS_ERROR_RECOVERY_PROTOCOL (flush on CRC/FC
// error) but NOT _LINK (no flush on timeout), so a late/garbage reply can linger and
// corrupt the next request. Cheap belt-and-suspenders.
static void drainRS485Rx() {
  while (TANKALARM_SOLAR_RS485.available()) {
    (void)TANKALARM_SOLAR_RS485.read();
  }
}

// True while the line rests after a request that got no usable reply (see above).
static bool solarLineResting() {
  if (!sSolarMissSeen) {
    return false;
  }
  if ((unsigned long)(millis() - sSolarLastMissMillis) < sSolarMissRestMs) {
    return true;
  }
  sSolarMissSeen = false;  // over: millis() may wrap before the next miss
  return false;
}

// Before every request: waits for the end of the line rest, then drops what came in meanwhile.
// Without the wait a retry of the poll, or its other function code after a miss, went out into
// the silence after the miss, and a late reply to the request before passed for its own. Those
// replies look alike for every register: after a miss on the array voltage and a refusal of the
// other function code, the retry 100 ms later could take the array voltage for the battery's.
// The charger check waits outside, without holding loop() (SolarManager::lineResting()), and ends
// its step at a miss, so it never waits here. A poll waits here at most SOLAR_POLL_MAX_MISSES - 1
// times: pollMayGoOn().
static void waitForLineRest() {
  while (solarLineResting()) {
#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    TANKALARM_WATCHDOG_KICK(Watchdog::get_instance());
#elif defined(ARDUINO_ARCH_STM32)
    IWatchdog.reload();
#endif
    delay(10);
  }
  drainRS485Rx();
}

// False once the poll under way has had SOLAR_POLL_MAX_MISSES misses: it then sends nothing more.
// poll() and begin() start the count.
static bool pollMayGoOn() {
  return sSolarPollMisses < SOLAR_POLL_MAX_MISSES;
}

// Capture the reason for a low-level Modbus read failure (does NOT increment the failure
// count -- that happens once per logical read in readRegistersWithFallback).
static void captureSolarModbusError(uint16_t address, uint16_t elapsedMs) {
  sSolarLastFailAddress = address;
  sSolarLastResponseMs = elapsedMs;
  const char *e = ModbusRTUClient.lastError();
  if (!e || !e[0]) {
    e = "unknown";
  }
  size_t i = 0;
  for (; e[i] && i < sizeof(sSolarLastModbusError) - 1; ++i) {
    sSolarLastModbusError[i] = e[i];
  }
  sSolarLastModbusError[i] = '\0';
  // R1/R5: classify the error text into a short tag so the per-bucket counter
  // (incremented once per logical failure in readRegistersWithFallback) picks the
  // right bucket, and so the client can emit "scErr" without re-running the classifier.
  classifyModbusErrorTag(sSolarLastModbusError);
  if (!solarErrorIsRefusal(sSolarLastModbusError)) {
    sSolarLastMissMillis = millis();  // the line rests from now (see lineResting())
    sSolarMissRestMs = sSolarTimeoutMs;
    sSolarMissSeen = true;
    if (sSolarPollMisses < 255) {
      ++sSolarPollMisses;
    }
  }
}

static bool readHoldingRegisters(uint8_t slaveId, uint16_t startAddress, uint8_t count, uint16_t *buffer) {
  waitForLineRest();
  unsigned long _t0 = millis();
  if (!ModbusRTUClient.requestFrom(slaveId, HOLDING_REGISTERS, startAddress, count)) {
    captureSolarModbusError(startAddress, (uint16_t)(millis() - _t0));
    return false;
  }

  if (ModbusRTUClient.available() < count) {
    captureSolarModbusError(startAddress, (uint16_t)(millis() - _t0));
    return false;
  }

  for (uint8_t index = 0; index < count; ++index) {
    int val = ModbusRTUClient.read();
    if (val < 0) {
      captureSolarModbusError(startAddress, (uint16_t)(millis() - _t0));
      return false;
    }
    buffer[index] = (uint16_t)val;
  }

  sSolarLastResponseMs = (uint16_t)(millis() - _t0);
  return true;
}

static bool readInputRegisters(uint8_t slaveId, uint16_t startAddress, uint8_t count, uint16_t *buffer) {
  waitForLineRest();
  unsigned long _t0 = millis();
  if (!ModbusRTUClient.requestFrom(slaveId, INPUT_REGISTERS, startAddress, count)) {
    captureSolarModbusError(startAddress, (uint16_t)(millis() - _t0));
    return false;
  }

  if (ModbusRTUClient.available() < count) {
    captureSolarModbusError(startAddress, (uint16_t)(millis() - _t0));
    return false;
  }

  for (uint8_t index = 0; index < count; ++index) {
    int val = ModbusRTUClient.read();
    if (val < 0) {
      captureSolarModbusError(startAddress, (uint16_t)(millis() - _t0));
      return false;
    }
    buffer[index] = (uint16_t)val;
  }

  sSolarLastResponseMs = (uint16_t)(millis() - _t0);
  return true;
}

static bool readRegistersWithFallback(uint8_t slaveId, uint16_t startAddress, uint8_t count, uint16_t *buffer, uint8_t &cachedFC, bool fastFail = false) {
  if (!pollMayGoOn()) {
    return false;  // the poll has had its last miss: no request, and no new failure to count
  }
  if (cachedFC == 3) {
    if (readHoldingRegisters(slaveId, startAddress, count, buffer)) {
      return true;
    }
    // Primary FC failed. Try the alternate WITHOUT discarding the cache first: a single
    // transient timeout on a known-good FC03 device must not force permanent FC04
    // re-probing on every subsequent poll (R-4 hardening, v2.0.45). Only switch the cache
    // if the alternate FC genuinely answers.
    //
    // Fix S3 (Part A, v2.0.49): when the caller passes fastFail=true (the bus has already
    // produced one or more consecutive failures and is most likely dead), SKIP the
    // alternate-FC probe. We do not discard the cache — R-4 is preserved — we just stop
    // wasting ~1 s per failed read on a known-dead bus. The cooperative pulse sampler can
    // then continue running between polls instead of being starved for up to 6 s.
    // After a miss the alternate waits for the line rest, and a poll at its last miss tries
    // nothing more (pollMayGoOn()).
    if (!fastFail && pollMayGoOn() && readInputRegisters(slaveId, startAddress, count, buffer)) {
      cachedFC = 4;
      return true;
    }
    ++sSolarModbusErrorCount;
    incrementSolarErrorBucket();  // R5
    return false;
  } else if (cachedFC == 4) {
    if (readInputRegisters(slaveId, startAddress, count, buffer)) {
      return true;
    }
    if (!fastFail && pollMayGoOn() && readHoldingRegisters(slaveId, startAddress, count, buffer)) {
      cachedFC = 3;
      return true;
    }
    ++sSolarModbusErrorCount;
    incrementSolarErrorBucket();  // R5
    return false;
  }

  // Uncached: try holding first, then input
  if (readHoldingRegisters(slaveId, startAddress, count, buffer)) {
    cachedFC = 3;
    return true;
  }
  if (pollMayGoOn() && readInputRegisters(slaveId, startAddress, count, buffer)) {
    cachedFC = 4;
    return true;
  }
  ++sSolarModbusErrorCount;
  incrementSolarErrorBucket();  // R5
  return false;
}

// Read 'count' contiguous registers ONE AT A TIME (count=1 per Modbus transaction). The SunSaver
// MPPT via the Morningstar MRC-1 MeterBus->RS-485 bridge was bench-verified (2026-04-22) ONLY with
// single-register reads -- firmware/sunsaver-modbus-test and firmware/sunsaver-rs485-raw both poll
// one register per request. A multi-register block read is a DIFFERENT Modbus transaction the
// MeterBus bridge does not reliably answer; in the field that presents as "config applied + poller
// running but every poll fails" (scOk:0) while the single-register startup probe in begin() succeeds.
// Returns true only if EVERY register reads OK, and SHORT-CIRCUITS on the first failure so a dead
// bus costs one read, not 'count' reads. The watchdog is kicked before each read because a
// slow-but-responding bus can take up to one Modbus timeout per register.
static bool readRegistersIndividually(uint8_t slaveId, uint16_t startAddress, uint8_t count,
                                      uint16_t *buffer, uint8_t &cachedFC, bool fastFail = false) {
  for (uint8_t i = 0; i < count; ++i) {
#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    TANKALARM_WATCHDOG_KICK(Watchdog::get_instance());
#elif defined(ARDUINO_ARCH_STM32)
    IWatchdog.reload();
#endif
    if (!readRegistersWithFallback(slaveId, (uint16_t)(startAddress + i), 1, &buffer[i], cachedFC, fastFail)) {
      return false;
    }
  }
  return true;
}

SolarManager::SolarManager() 
  : _initialized(false), _lastPollMillis(0), _noChargeConsecutivePolls(0), _cachedHoldingFC(0),
    _floatReferenceV(0.0f), _readingFactor((float)sunsaver::readingFactor(sunsaver::SCALE_UNKNOWN, SS_VOLTAGE_SCALE_ID)),
    _packVolts(0), _packChemistry(sunsaver::CHEMISTRY_NOT_JUDGED), _chargerCheckStepMillis(0), _chargerCheckGapMs(0),
    _chargerSettingsRead(false) {
  memset(&_data, 0, sizeof(SolarData));
  memset(&_config, 0, sizeof(SolarConfig));
  
  // Set defaults
  _config.enabled = false;
  _config.modbusSlaveId = SOLAR_DEFAULT_SLAVE_ID;
  _config.modbusBaudRate = SOLAR_DEFAULT_BAUD_RATE;
  _config.modbusTimeoutMs = SOLAR_DEFAULT_TIMEOUT_MS;
  _config.pollIntervalSec = SOLAR_DEFAULT_POLL_INTERVAL_SEC;
  _config.batteryLowVoltage = BATTERY_VOLTAGE_LOW;
  _config.batteryCriticalVoltage = BATTERY_VOLTAGE_CRITICAL;
  _config.batteryHighVoltage = BATTERY_VOLTAGE_HIGH;
  _config.alertOnLowBattery = true;
  // 2026-04-22: alertOnFault default flipped to FALSE because the status/fault
  // register addresses on this firmware revision are not yet verified and
  // returned implausible values during bench testing. Re-enable in user config
  // (or per-build override) once SS_REG_FAULTS / SS_REG_ALARMS are confirmed.
  _config.alertOnFault = false;
  _config.alertOnCommFailure = false;
  _config.includeInDailyReport = true;
}

bool SolarManager::begin(const SolarConfig& config) {
  // A charger check that was under way belongs to the link as it was, and so does what the
  // last one found: another charger may be on the line now, or none.
  _chargerCheck.cancel();
  _chargerSettingsRead = false;
  setFloatReference(0.0f);

  if (!config.enabled) {
    _initialized = false;
    return true;  // Not an error, just disabled
  }
  
  setConfig(config);
  
  // Initialize Modbus RTU Client
  // Arduino Opta RS485 uses the built-in RS485 interface.
  // Modbus RTU spec requires 8N2 when no parity is used; SunSaver MPPT
  // (and Morningstar MRC-1 adapter) expect 8N2. ArduinoModbus default is
  // 8N1, which often appears to work but produces intermittent CRC errors.
  if (!ModbusRTUClient.begin(TANKALARM_SOLAR_RS485, _config.modbusBaudRate, SERIAL_8N2)) {
    Serial.println(F("Solar: Failed to initialize Modbus RTU Client"));
    _initialized = false;
    return false;
  }
  
  // Set read timeout. Clamp to >=500 ms because SunSaver MPPT can take several
  // hundred ms to assemble a reply over the MRC-1 MeterBus->RS-485 bridge,
  // and the default 200 ms in older saved configs causes the client to
  // give up before the reply arrives.
  if (_config.modbusTimeoutMs < 500) {
    _config.modbusTimeoutMs = 500;
  }
  ModbusRTUClient.setTimeout(_config.modbusTimeoutMs);
  sSolarTimeoutMs = _config.modbusTimeoutMs;

  // RS-485 timing fix per Arduino forum thread #1421875 post #18:
  // The Opta needs a post-TX delay of one full character time before DE drops,
  // otherwise the last byte of the Modbus query is corrupted on the wire and
  // the slave silently rejects it. setDelays(pre, post) is in microseconds.
  //
  // Fix S1 (v2.0.49): the post-delay must SCALE WITH BAUD RATE. The previous
  // hardcoded 1200 us was safe at 9600 baud (1 char time = 1146 us) but at the higher
  // baud rates the sanitizer also permits (19200..115200) it held DE active past the
  // Modbus 3.5-char inter-frame gap and caused bus contention with a fast slave's
  // response. One 11-bit character time (8N2) = 11,000,000 / baud microseconds, clamped
  // to a safe floor (150 us protects 115200 from underrunning the DE FET) and ceiling
  // (1500 us is more than enough for the slowest supported baud).
  //
  // Note on truncation margin: we add 50 microseconds of safety margin to the raw quotient
  // (11,000,000 / 9600 = 1145 us) to ensure integer truncation doesn't drop the DE pin
  // 0.83 microseconds BEFORE the stop bits have fully completed transmitting. This scales
  // 9600 to 1195 us (matching the stable 1200 us baseline).
  uint32_t postDelayUs = (11000000UL / _config.modbusBaudRate) + 50UL;
  if (postDelayUs < 150)  postDelayUs = 150;
  if (postDelayUs > 1500) postDelayUs = 1500;
  TANKALARM_SOLAR_RS485.setDelays(0, postDelayUs);
  
  Serial.print(F("Solar: Modbus RTU initialized at "));
  Serial.print(_config.modbusBaudRate);
  Serial.print(F(" baud 8N2, slave ID "));
  Serial.print(_config.modbusSlaveId);
  Serial.print(F(", timeout "));
  Serial.print(_config.modbusTimeoutMs);
  Serial.print(F(" ms, postDelay "));
  Serial.print(postDelayUs);
  Serial.println(F(" us"));
  
  _initialized = true;
  _data.communicationOk = false;
  _data.consecutiveErrors = 0;
  
  // A request of the link as it was may have gone unanswered a moment ago (a config update
  // restarts the link at any time): the probe waits like every other request
  // (waitForLineRest()). At most one Modbus timeout of that link, and not at all at boot. The
  // probe and the first read count their misses as one poll does (SOLAR_POLL_MAX_MISSES).
  sSolarPollMisses = 0;

  // Probe both holding and input register models before first full poll.
  uint16_t startupProbe = 0;
  bool startupHolding = readHoldingRegisters(_config.modbusSlaveId, SS_REG_BATTERY_VOLTAGE, 1, &startupProbe);
  bool startupInput = false;
  if (!startupHolding) {
    startupInput = readInputRegisters(_config.modbusSlaveId, SS_REG_BATTERY_VOLTAGE, 1, &startupProbe);
  }

  if (startupHolding) {
    _cachedHoldingFC = 3;
    Serial.println(F("Solar: Startup probe OK via FC03 (holding)"));
  } else if (startupInput) {
    _cachedHoldingFC = 4;
    Serial.println(F("Solar: Startup probe OK via FC04 (input)"));
  } else {
    _cachedHoldingFC = 0;
    Serial.println(F("Solar: Startup probe failed for both FC03 and FC04"));
  }

  // Do an initial full read to populate the data cache when possible.
  if (!readRegisters()) {
    Serial.println(F("Solar: Modbus transport initialized, but full initial read failed"));
  }
  
  return true;
}

void SolarManager::end() {
  _chargerCheck.cancel();
  if (_initialized) {
    ModbusRTUClient.end();
    _initialized = false;
  }
}

void SolarManager::setConfig(const SolarConfig& config) {
  _config = config;
}

bool SolarManager::poll(unsigned long nowMillis) {
  if (!_config.enabled || !_initialized) {
    return false;  // Not active — no data
  }
  
  // Check if it's time to poll
  unsigned long intervalMs = (unsigned long)_config.pollIntervalSec * 1000UL;
  if (nowMillis - _lastPollMillis < intervalMs) {
    return false;  // Not due yet — use isCommunicationOk() to distinguish from failure
  }
  // A request went unanswered a moment ago (one of the charger check, say): the poll stays due
  // and runs once the line has rested.
  if (lineResting()) {
    return false;
  }
  
  _lastPollMillis = nowMillis;
  sSolarPollMisses = 0;
  readRegisters();
  return true;
}

bool SolarManager::lineResting() const {
  return solarLineResting();
}

bool SolarManager::readRegisters() {
  bool success = true;
  SolarData nextData = _data;

  // ==========================================================================
  // 2026-04-22 FIELD-READY POLICY:
  // Only the live/filtered ADC block at 0x0008..0x000C has been bench-verified
  // on this SunSaver MPPT firmware revision. The previously-coded addresses
  // for temperature (0x001B/0x001C), status (0x002B/0x002C/0x002E/0x002F),
  // daily Ah (0x0034) and daily V min/max (0x003D/0x003E) all returned either
  // zeros or values that look like extra voltage snapshots, NOT the documented
  // SunSaver semantics. Reading them and feeding the result into derived
  // health flags (chargeState/faults/alarms/heatsinkTemp) caused false
  // "FAULT" indications in the bench capture (e.g. faults=0x4235 with no
  // physical fault). Until the addresses are confirmed against a Morningstar
  // datasheet for this exact firmware, we skip those reads entirely.
  //
  // To re-enable for bench experimentation, define SOLAR_ENABLE_UNVERIFIED_REGISTERS.
  //
  // BENCH-VERIFICATION TODO (per /memories/repo/sunsaver-production-fix-2026-04-22.md):
  //   1. Probe register addresses 0x0021, 0x0023, 0x002F, 0x0032 alongside the currently
  //      coded 0x002B/0x002C/0x002E and look for sane values matching the SunSaver PDU
  //      charge_state enum (range 0..8) and the documented fault/alarm bitfield widths.
  //   2. Capture `faults` and `alarms` while inducing known conditions on the bench:
  //      flip a DIP switch (expect SS_FAULT_DIP_SW_FAULT), disconnect the RTS sensor
  //      (expect SS_FAULT_RTS_DISCONN / SS_ALARM_RTS_DISCONN), short the array briefly
  //      (expect SS_FAULT_OVERCURRENT). Watch the bit positions actually change.
  //   3. Once at least chargeState + faults + alarms verify against known conditions,
  //      gate by SOLAR_ENABLE_UNVERIFIED_REGISTERS for a beta cohort first, observe a
  //      fleet-wide passive capture (collect bits but suppress SMS) before baking into
  //      production. See review CODE_REVIEW_06252026_RS485_SUNSAVER_COMMUNICATION.md
  //      §2 Inconsistency E for the rationale.
  // ==========================================================================

  // Real-time block. Read EXACTLY the four registers the bench-proven
  // firmware/sunsaver-modbus-test.ino reads: 0x0008 batt V, 0x0009 array V,
  // 0x000B charge I, 0x000C load I. 0x000A (filtered load voltage) is intentionally
  // SKIPPED -- it was read into a 5th slot but never consumed, and a NACK on that one
  // register would short-circuit readRegistersIndividually() and fail the whole poll.
  // realtimeRegs index map: [0]=battV [1]=arrV [2]=chargeI [3]=loadI.
  uint16_t realtimeRegs[4];
  // Bounded retry around the VERIFIED realtime registers only. A single transient
  // RS-485 disturbance otherwise fails the entire poll, and after
  // SOLAR_COMM_FAILURE_THRESHOLD such polls the client would raise a false comm-failure
  // alarm. The watchdog is kicked before EVERY attempt and (via readRegistersIndividually)
  // before each register, so the retry loop can never starve it.
  //
  // Fix S3 (Part B, v2.0.49): once we have ANY consecutive failure already on record
  // the bus is most likely dead. Cap the retry loop to 1 attempt and propagate
  // fastFail down so each register read also skips its alternate-FC probe. Worst-case
  // dead-bus blocking drops from ~6.2 s to ~1 s per poll, which keeps the cooperative
  // pulse sampler running between polls. Once a poll succeeds, _data.consecutiveErrors
  // resets to 0 and full retry/dual-FC behavior is restored (so R-4 hardening still
  // absorbs the first transient).
  const bool fastFail = (_data.consecutiveErrors >= 1);
  const uint8_t maxAttempts = fastFail ? 1 : SOLAR_REALTIME_MAX_ATTEMPTS;
  bool realtimeOk = false;
  for (uint8_t attempt = 0; attempt < maxAttempts; ++attempt) {
#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    TANKALARM_WATCHDOG_KICK(Watchdog::get_instance());
#elif defined(ARDUINO_ARCH_STM32)
    IWatchdog.reload();
#endif
    if (attempt > 0) {
      if (!pollMayGoOn()) {
        break;  // the bus is most likely dead, and every request now waits a rest first
      }
      delay(SOLAR_RETRY_DELAY_MS);  // brief settle before re-attempting (short, WDT-safe)
    }
    // Two contiguous single-register reads, skipping the unused 0x000A in between.
    // readRegistersIndividually() reads count=1 per Modbus transaction (the bench-verified
    // transaction the MRC-1/SunSaver answers) and kicks the WDT per register.
    if (readRegistersIndividually(_config.modbusSlaveId, SS_REG_BATTERY_VOLTAGE, 2, &realtimeRegs[0], _cachedHoldingFC, fastFail) &&
        readRegistersIndividually(_config.modbusSlaveId, SS_REG_CHARGE_CURRENT, 2, &realtimeRegs[2], _cachedHoldingFC, fastFail)) {
      realtimeOk = true;
      break;
    }
  }

  if (realtimeOk) {
#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  TANKALARM_WATCHDOG_KICK(Watchdog::get_instance());
#elif defined(ARDUINO_ARCH_STM32)
    IWatchdog.reload();
#endif
    float battV = scaleVoltage(realtimeRegs[0]);
    float arrV  = scaleVoltage(realtimeRegs[1]);
    float chgI  = scaleCurrent(realtimeRegs[2]);  // signed (see scaleCurrent)
    float lodI  = scaleCurrent(realtimeRegs[3]);  // signed

    // Saturate small negative readings (normal no-charge / night offsets) to zero so
    // they cannot be misread as out-of-range.
    if (arrV < 0.0f) arrV = 0.0f;
    if (chgI < 0.0f) chgI = 0.0f;
    if (lodI < 0.0f) lodI = 0.0f;

    // TRANSPORT-HEALTH GATE (v2.0.45): only the battery voltage decides scOk. A
    // CRC-valid Modbus read must never be discarded as a comm failure because a
    // data-quality field looks odd -- treating the current registers as unsigned
    // previously turned a healthy night-time read (small negative charge current ->
    // ~158 A) into scOk:0. Array V and currents are now clamped as data quality only.
    if (battV >= 5.0f && battV <= 40.0f) {
      nextData.batteryVoltage = battV;
      nextData.arrayVoltage   = (arrV <= 80.0f)  ? arrV : nextData.arrayVoltage;
      nextData.chargeCurrent  = (chgI <= 100.0f) ? chgI : nextData.chargeCurrent;
      nextData.loadCurrent    = (lodI <= 100.0f) ? lodI : nextData.loadCurrent;
      // v2.1.3: classify the bank on every plausible read (tracking only, no alert).
      nextData.systemNominalV = (battV > SOLAR_24V_SYSTEM_MIN_V) ? 24 : 12;
      sSolarLastReadImplausible = false;
    } else {
      // CRC-valid read but the battery voltage itself is implausible: a DATA problem,
      // distinct from a transport failure. Flag it (scImpl) so the dashboard can tell
      // "no link" apart from "values rejected."
      sSolarLastReadImplausible = true;
      Serial.print(F("WARNING: Solar battery voltage implausible, rejecting poll (bv="));
      Serial.print(battV, 2);
      Serial.println(F(")"));
      success = false;
    }
  } else {
    sSolarLastReadImplausible = false;  // genuine transport failure, not a value rejection
    success = false;
  }

  // Suspect register blocks: only read when explicitly enabled for bench work.
  // In production these stay zeroed so derived health logic does not false-trip.
#ifdef SOLAR_ENABLE_UNVERIFIED_REGISTERS
  uint16_t temperatureRegs[2];
  if (success && readRegistersWithFallback(_config.modbusSlaveId, SS_REG_HEATSINK_TEMP, 2, temperatureRegs, _cachedHoldingFC)) {
    nextData.heatsinkTemp = (int8_t)((int16_t)temperatureRegs[0]);
    nextData.batteryTemp = (int8_t)((int16_t)temperatureRegs[1]);
  }

  uint16_t statusRegs[5];
  if (success && readRegistersWithFallback(_config.modbusSlaveId, SS_REG_CHARGE_STATE, 5, statusRegs, _cachedHoldingFC)) {
    nextData.chargeState = (SolarChargeState)(statusRegs[0] & 0xFF);
    nextData.faults = statusRegs[1];
    nextData.alarms = statusRegs[3];
    nextData.loadOn = (statusRegs[4] != 0);
  }

  uint16_t dailyChargeRegs[1];
  if (success && readRegistersWithFallback(_config.modbusSlaveId, SS_REG_AH_DAILY, 1, dailyChargeRegs, _cachedHoldingFC)) {
    nextData.ampHoursDaily = dailyChargeRegs[0] * 0.1f;  // Scale: 0.1 Ah per count
  }

  uint16_t dailyVoltageRegs[2];
  if (success && readRegistersWithFallback(_config.modbusSlaveId, SS_REG_BATTERY_V_MIN_DAILY, 2, dailyVoltageRegs, _cachedHoldingFC)) {
    nextData.batteryVoltageMinDaily = scaleVoltage(dailyVoltageRegs[0]);
    nextData.batteryVoltageMaxDaily = scaleVoltage(dailyVoltageRegs[1]);
  }
#else
  // Force unverified fields to safe defaults every poll so a stale value from
  // a prior bench session can never leak into health logic or daily reports.
  nextData.heatsinkTemp = 0;
  nextData.batteryTemp = 0;
  nextData.chargeState = CHARGE_STATE_START;
  nextData.faults = 0;
  nextData.alarms = 0;
  nextData.loadOn = false;
  nextData.ampHoursDaily = 0.0f;
  nextData.wattHoursDaily = 0.0f;

  // Track daily min/max in software for production builds (Task 2.1 & SR-5)
  if (success) {
    if (!nextData.dailyStatsSeeded) {
      nextData.batteryVoltageMinDaily = nextData.batteryVoltage;
      nextData.batteryVoltageMaxDaily = nextData.batteryVoltage;
      nextData.dailyStatsSeeded = true;
    } else {
      if (nextData.batteryVoltage < nextData.batteryVoltageMinDaily) {
        nextData.batteryVoltageMinDaily = nextData.batteryVoltage;
      }
      if (nextData.batteryVoltage > nextData.batteryVoltageMaxDaily) {
        nextData.batteryVoltageMaxDaily = nextData.batteryVoltage;
      }
    }
  }
#endif

  // Track no-charge alert (Task 2.2, R-1 & SR-2)
  if (success) {
    // The voltage a full battery is held at, as this firmware reads it. The charger check
    // supplies it when the custom charge settings are in use and can be trusted
    // (setFloatReference). Otherwise it is BATTERY_VOLTAGE_FLOAT per 12 V of pack, taken to what
    // this firmware reads for a battery at that voltage (_readingFactor: from a charger that
    // reports with x100 it reads 3.3 % low, a full 24 V battery at 26.8 V as 25.91 V), and scaled
    // down for a battery whose low-voltage threshold says it sits lower than lead-acid. It is
    // never scaled up: a LiFePO4 battery has a low threshold of 13 V and more, and scaling by
    // that put the float target near 15 V, where a full battery in daylight looked like one that
    // was not being charged.
    // The low threshold can be a figure per 12 V whatever the pack (the default, 11.8 V, is
    // one, and a config without batteryLowV keeps it on a 24 V pack too) or one for a 24 V
    // pack: a threshold above SOLAR_24V_SYSTEM_MIN_V is taken as the latter.
    // The size of the pack is the config's when it names one (setPackVolts()). The voltage read
    // decides only when it does not: a 12 V battery that a fault has taken above
    // SOLAR_24V_SYSTEM_MIN_V would pass for a 24 V one, and with the float target of a 24 V pack
    // it would look like one that was not being charged, hiding its BATTERY_HIGH.
    const uint8_t packVolts = (_packVolts != 0) ? _packVolts : nextData.systemNominalV;
    const float packs = (packVolts == 24) ? 2.0f : 1.0f;
    float lowPer12V = _config.batteryLowVoltage;
    if (lowPer12V > SOLAR_24V_SYSTEM_MIN_V) {
      lowPer12V /= 2.0f;
    }
    float scale = lowPer12V / BATTERY_VOLTAGE_LOW;
    if (!(scale > 0.0f) || scale > 1.0f) {
      scale = 1.0f;
    }
    float floatThreshold = BATTERY_VOLTAGE_FLOAT * scale * packs * _readingFactor;
    // A battery the config names, whose float voltages the charger check knows (setPackChemistry()):
    // a charger set anywhere in that range holds it full, and a full battery at the lowest of them
    // is no battery that needs charge. The default is then never more than 0.2 V above that
    // voltage, the margin of the comparison below, for the pack and as this firmware reads it.
    // That takes a Li-ion battery, full at 12.6 V and held as low as 11.7 V per 12 V, from
    // lead-acid's 13.4 V to 11.9 V. A LiFePO4 battery, held from 13.2 V, keeps 13.4 V on a 12 V
    // pack, and has 26.6 V in place of 26.8 V on a 24 V one.
    sunsaver::VoltRange absorbRange = {0.0, 0.0};
    sunsaver::VoltRange floatRange = {0.0, 0.0};
    if (sunsaver::chargeRanges(_packChemistry, &absorbRange, &floatRange)) {
      const float lowestFull = (float)floatRange.low * packs * _readingFactor + 0.2f;
      if (floatThreshold > lowestFull) {
        floatThreshold = lowestFull;
      }
    }
    if (_floatReferenceV > 8.0f) {
      floatThreshold = _floatReferenceV;
    }
    // Never above the high-battery limit of the pack. A charger can hold a full battery higher
    // than that (a float voltage of 16.5 V, say, which the charger check reports as wrong for the
    // battery), and a battery above the limit would then count as one that needs charge: in
    // daylight without charge current NO_CHARGE would come, and checkAlerts() gives it before
    // BATTERY_HIGH. Like the low threshold, the limit is one per 12 V up to
    // SOLAR_24V_SYSTEM_MIN_V (the default, 14.8 V, is one, and a 24 V pack keeps it when the
    // config names none) and one for the pack above it.
    float highLimit = _config.batteryHighVoltage;
    if (highLimit <= SOLAR_24V_SYSTEM_MIN_V) {
      highLimit *= packs;
    }
    if (floatThreshold > highLimit) {
      floatThreshold = highLimit;
    }

    bool batteryNeedsCharge = (nextData.batteryVoltage < (floatThreshold - 0.2f));
    bool daylightDetected = (nextData.arrayVoltage > (nextData.batteryVoltage + 3.0f));
    bool zeroChargingDetected = (nextData.chargeCurrent < 0.1f);

    if (batteryNeedsCharge && daylightDetected && zeroChargingDetected) {
      if (_noChargeConsecutivePolls < 3) {
        _noChargeConsecutivePolls++;
      }
      if (_noChargeConsecutivePolls >= 3) {
        nextData.noChargeAlertActive = true;
      }
    } else {
      _noChargeConsecutivePolls = 0;
      nextData.noChargeAlertActive = false;
    }
  } else {
    // Clear alerts immediately on failed poll so old state does not leak (R-2)
    _noChargeConsecutivePolls = 0;
    _data.noChargeAlertActive = false;
    nextData.noChargeAlertActive = false;
  }
  
  // Update communication status
  if (success) {
    _data = nextData;
    _data.communicationOk = true;
    _data.lastReadMillis = millis();
    _data.consecutiveErrors = 0;
    // R2/R6 (v2.0.53): mark this poll as successful (no smoothing) and stamp
    // the FIRST successful read since boot. firstReadMillis stays at the first
    // success value for the lifetime of the boot, so the dashboard can tell
    // "link has succeeded at some point" apart from "never succeeded."
    _data.lastPollOk = true;
    if (_data.firstReadMillis == 0) {
      _data.firstReadMillis = _data.lastReadMillis;
    }
  } else {
    _data.lastPollOk = false;  // R6
    _data.consecutiveErrors++;
    if (_data.consecutiveErrors >= SOLAR_COMM_FAILURE_THRESHOLD) {
      _data.communicationOk = false;
      Serial.print(F("Solar: Modbus communication failure ("));
      Serial.print(_data.consecutiveErrors);
      Serial.println(F(" consecutive errors)"));
    }
  }
  
  // Update derived status
  updateHealthStatus();
  
  return success;
}

float SolarManager::scaleVoltage(uint16_t raw) const {
  // Formula: Voltage = (Raw * 100) / 32768 for 12V system
  return (raw * SS_SCALE_VOLTAGE_12V) / SS_SCALE_DIVISOR;
}

float SolarManager::scaleCurrent(uint16_t raw) const {
  // Formula: Current = (Raw * 79.16) / 32768 for 12V system.
  // The SunSaver filtered current registers (adc_ic_f 0x000B, adc_il_f 0x000C) are SIGNED
  // (two's complement). Cast through int16_t FIRST so a small negative current near zero
  // (no-charge / night) scales to a small negative value, NOT a huge positive one. Reading
  // it as unsigned wrapped -16 counts to ~158 A, which tripped the plausibility clamp and
  // silently turned a healthy Modbus read into scOk:0 (fixed v2.0.45).
  return ((float)(int16_t)raw * SS_SCALE_CURRENT_12V) / SS_SCALE_DIVISOR;
}

// --- Modbus diagnostics accessors (file-local state defined near the top) ---
uint32_t SolarManager::getModbusErrorCount() const { return sSolarModbusErrorCount; }
const char* SolarManager::getLastModbusError() const { return sSolarLastModbusError; }
uint16_t SolarManager::getLastModbusFailAddress() const { return sSolarLastFailAddress; }
uint16_t SolarManager::getLastModbusResponseMs() const { return sSolarLastResponseMs; }
bool SolarManager::wasLastReadImplausible() const { return sSolarLastReadImplausible; }

// R1/R5 (v2.0.53): per-error-type accessors.
const char* SolarManager::getLastModbusErrorTag() const { return sSolarLastErrorTag; }
uint32_t SolarManager::getModbusErrTimeout()     const { return sSolarErrTimeout; }
uint32_t SolarManager::getModbusErrCrc()         const { return sSolarErrCrc; }
uint32_t SolarManager::getModbusErrIllegalAddr() const { return sSolarErrIllegalAddr; }
uint32_t SolarManager::getModbusErrIllegalFunc() const { return sSolarErrIllegalFunc; }
uint32_t SolarManager::getModbusErrOther()       const { return sSolarErrOther; }
void SolarManager::resetModbusErrorStats() {
  sSolarModbusErrorCount = 0;
  sSolarLastModbusError[0] = '\0';
  sSolarLastFailAddress = 0;
  sSolarLastResponseMs = 0;
  sSolarLastReadImplausible = false;
  // R1/R5: also clear the classified tag and the five per-bucket counters so the
  // daily window starts fresh in lockstep with the lifetime total.
  sSolarLastErrorTag[0] = '\0';
  sSolarErrTimeout = 0;
  sSolarErrCrc = 0;
  sSolarErrIllegalAddr = 0;
  sSolarErrIllegalFunc = 0;
  sSolarErrOther = 0;
}

void SolarManager::updateHealthStatus() {
  // Update derived flags
  _data.hasFault = (_data.faults != 0);
  _data.hasAlarm = (_data.alarms != 0);
  
#ifdef SOLAR_ENABLE_UNVERIFIED_REGISTERS
  // Charge state indicators
  _data.isCharging = (_data.chargeState == CHARGE_STATE_BULK || 
                      _data.chargeState == CHARGE_STATE_ABSORPTION ||
                      _data.chargeState == CHARGE_STATE_EQUALIZE);
  _data.isFullyCharged = (_data.chargeState == CHARGE_STATE_FLOAT);
#else
  // Software-derived charge state indicators (Task 2.1)
  _data.isCharging = (_data.chargeCurrent >= 0.1f);
  _data.isFullyCharged = (!_data.isCharging && _data.batteryVoltage >= BATTERY_VOLTAGE_FLOAT);

  // Fix S4 (v2.0.49): derive a sensible chargeState from the verified electrical values
  // so getChargeStateDescription() no longer permanently reports "Starting" in production.
  // The placeholder set in readRegisters() (#else branch -> CHARGE_STATE_START) is
  // overwritten here every poll. Without verified status-register addresses we can only
  // distinguish four useful states from the live ADC block:
  //   FLOAT       = fully charged + not charging (battery at float voltage, no current in)
  //   BULK        = actively charging (any current flowing into the battery)
  //   NIGHT       = no panel input (array voltage at or below battery voltage)
  //   DISCONNECT  = panel present but no charging (likely fault / disconnect / regulating)
  // ABSORPTION and EQUALIZE require status-register verification to distinguish; they
  // both fall under BULK here. See SOLAR_ENABLE_UNVERIFIED_REGISTERS bench-verification
  // TODO in readRegisters() for the path to richer state reporting.
  if (_data.isFullyCharged) {
    _data.chargeState = CHARGE_STATE_FLOAT;
  } else if (_data.isCharging) {
    _data.chargeState = CHARGE_STATE_BULK;
  } else if (_data.arrayVoltage < (_data.batteryVoltage + 1.0f)) {
    _data.chargeState = CHARGE_STATE_NIGHT;
  } else {
    _data.chargeState = CHARGE_STATE_DISCONNECT;
  }
#endif
  
  // Battery health assessment
  _data.batteryHealthy = (_data.batteryVoltage >= _config.batteryLowVoltage) &&
                         (_data.batteryVoltage <= _config.batteryHighVoltage) &&
                         !_data.hasFault;
  
  // Overall solar system health
  _data.solarHealthy = _data.batteryHealthy && 
                       _data.communicationOk && 
                       !_data.hasFault && 
                       !_data.hasAlarm &&
                       !_data.noChargeAlertActive;
}

SolarAlertType SolarManager::checkAlerts() const {
  if (!_config.enabled) {
    return SOLAR_ALERT_NONE;
  }

  // Priority order: most critical first.
  //
  // m-8 fix (v1.6.13): when the Modbus link is down, _data.batteryVoltage,
  // _data.heatsinkTemp, _data.hasFault and _data.hasAlarm are stale (held
  // from the last successful poll). Acting on stale data can fire a
  // BATTERY_CRITICAL alert long after the battery is gone, or hide a real
  // fault behind an old "OK" snapshot. Surface the comm failure first (if
  // the operator opted in) and suppress the data-driven branches whenever
  // communicationOk is false.
  if (!_data.communicationOk) {
    if (_config.alertOnCommFailure) {
      return SOLAR_ALERT_COMM_FAILURE;
    }
    return SOLAR_ALERT_NONE;
  }

  // Critical battery voltage
  if (_data.batteryVoltage < _config.batteryCriticalVoltage && _data.batteryVoltage > 0) {
    return SOLAR_ALERT_BATTERY_CRITICAL;
  }
  
  // Hardware faults
  if (_data.hasFault && _config.alertOnFault) {
    return SOLAR_ALERT_FAULT;
  }

  // No charging during daylight (panel or fuse issue, Task 2.2)
  if (_data.noChargeAlertActive) {
    return SOLAR_ALERT_NO_CHARGE;
  }
  
  // Low battery voltage (warning level)
  if (_data.batteryVoltage < _config.batteryLowVoltage && _data.batteryVoltage > 0) {
    return SOLAR_ALERT_BATTERY_LOW;
  }
  
  // High battery voltage (overcharge)
  if (_data.batteryVoltage > _config.batteryHighVoltage) {
    return SOLAR_ALERT_BATTERY_HIGH;
  }
  
  // Heatsink temperature warning
  if (_data.heatsinkTemp > 60) {  // 60°C is typical warning threshold
    return SOLAR_ALERT_HEATSINK_TEMP;
  }
  
  // Alarm conditions
  if (_data.hasAlarm && _config.alertOnFault) {
    return SOLAR_ALERT_ALARM;
  }
  
  return SOLAR_ALERT_NONE;
}

const char* SolarManager::getAlertDescription(SolarAlertType alert) const {
  switch (alert) {
    case SOLAR_ALERT_NONE:
      return "OK";
    case SOLAR_ALERT_BATTERY_LOW:
      return "Battery voltage low";
    case SOLAR_ALERT_BATTERY_CRITICAL:
      return "Battery voltage CRITICAL";
    case SOLAR_ALERT_BATTERY_HIGH:
      return "Battery overvoltage";
    case SOLAR_ALERT_FAULT:
      return getFaultDescription();
    case SOLAR_ALERT_ALARM:
      return getAlarmDescription();
    case SOLAR_ALERT_COMM_FAILURE:
      return "Solar charger communication failure";
    case SOLAR_ALERT_HEATSINK_TEMP:
      return "Solar charger overheating";
    case SOLAR_ALERT_NO_CHARGE:
      return "No solar charging detected";
    default:
      return "Unknown alert";
  }
}

const char* SolarManager::getChargeStateDescription() const {
  switch (_data.chargeState) {
    case CHARGE_STATE_START:
      return "Starting";
    case CHARGE_STATE_NIGHT_CHECK:
      return "Night Check";
    case CHARGE_STATE_DISCONNECT:
      return "Disconnected";
    case CHARGE_STATE_NIGHT:
      return "Night";
    case CHARGE_STATE_FAULT:
      return "FAULT";
    case CHARGE_STATE_BULK:
      return "Bulk";
    case CHARGE_STATE_ABSORPTION:
      return "Absorption";
    case CHARGE_STATE_FLOAT:
      return "Float";
    case CHARGE_STATE_EQUALIZE:
      return "Equalize";
    default:
      return "Unknown";
  }
}

const char* SolarManager::getFaultDescription() const {
  if (_data.faults == 0) {
    return "No faults";
  }
  
  _faultDescBuffer[0] = '\0';
  
  if (_data.faults & SS_FAULT_OVERCURRENT)    strlcat(_faultDescBuffer, "Overcurrent ", sizeof(_faultDescBuffer));
  if (_data.faults & SS_FAULT_FET_SHORT)      strlcat(_faultDescBuffer, "FET-Short ", sizeof(_faultDescBuffer));
  if (_data.faults & SS_FAULT_SOFTWARE)       strlcat(_faultDescBuffer, "SW-Fault ", sizeof(_faultDescBuffer));
  if (_data.faults & SS_FAULT_BATT_HVD)       strlcat(_faultDescBuffer, "Batt-HVD ", sizeof(_faultDescBuffer));
  if (_data.faults & SS_FAULT_ARRAY_HVD)      strlcat(_faultDescBuffer, "Array-HVD ", sizeof(_faultDescBuffer));
  if (_data.faults & SS_FAULT_DIP_SW_FAULT)   strlcat(_faultDescBuffer, "DIP-SW ", sizeof(_faultDescBuffer));
  if (_data.faults & SS_FAULT_RESET_FAULT)    strlcat(_faultDescBuffer, "Reset ", sizeof(_faultDescBuffer));
  if (_data.faults & SS_FAULT_RTS_DISCONN)    strlcat(_faultDescBuffer, "RTS-Disc ", sizeof(_faultDescBuffer));
  if (_data.faults & SS_FAULT_RTS_SHORT)      strlcat(_faultDescBuffer, "RTS-Short ", sizeof(_faultDescBuffer));
  if (_data.faults & SS_FAULT_HEATSINK_LIMIT) strlcat(_faultDescBuffer, "Heatsink-Limit ", sizeof(_faultDescBuffer));
  
  // Trim trailing space
  size_t len = strlen(_faultDescBuffer);
  if (len > 0 && _faultDescBuffer[len - 1] == ' ') {
    _faultDescBuffer[len - 1] = '\0';
  }
  
  return _faultDescBuffer;
}

const char* SolarManager::getAlarmDescription() const {
  if (_data.alarms == 0) {
    return "No alarms";
  }
  
  _alarmDescBuffer[0] = '\0';
  
  if (_data.alarms & SS_ALARM_RTS_OPEN)        strlcat(_alarmDescBuffer, "RTS-Open ", sizeof(_alarmDescBuffer));
  if (_data.alarms & SS_ALARM_RTS_SHORT)       strlcat(_alarmDescBuffer, "RTS-Short ", sizeof(_alarmDescBuffer));
  if (_data.alarms & SS_ALARM_RTS_DISCONN)     strlcat(_alarmDescBuffer, "RTS-Disc ", sizeof(_alarmDescBuffer));
  if (_data.alarms & SS_ALARM_HEATSINK_LIMIT)  strlcat(_alarmDescBuffer, "Heatsink ", sizeof(_alarmDescBuffer));
  if (_data.alarms & SS_ALARM_CURRENT_LIMIT)   strlcat(_alarmDescBuffer, "I-Limit ", sizeof(_alarmDescBuffer));
  if (_data.alarms & SS_ALARM_CURRENT_OFFSET)  strlcat(_alarmDescBuffer, "I-Offset ", sizeof(_alarmDescBuffer));
  if (_data.alarms & SS_ALARM_BATT_SENSE)      strlcat(_alarmDescBuffer, "Batt-Sense ", sizeof(_alarmDescBuffer));
  if (_data.alarms & SS_ALARM_BATT_SENSE_DISC) strlcat(_alarmDescBuffer, "Sense-Disc ", sizeof(_alarmDescBuffer));
  if (_data.alarms & SS_ALARM_UNCALIBRATED)    strlcat(_alarmDescBuffer, "Uncal ", sizeof(_alarmDescBuffer));
  if (_data.alarms & SS_ALARM_RTS_MISWIRE)     strlcat(_alarmDescBuffer, "RTS-Miswire ", sizeof(_alarmDescBuffer));
  if (_data.alarms & SS_ALARM_HVD)             strlcat(_alarmDescBuffer, "HVD ", sizeof(_alarmDescBuffer));
  if (_data.alarms & SS_ALARM_LOG_TIMEOUT)     strlcat(_alarmDescBuffer, "Log-Timeout ", sizeof(_alarmDescBuffer));
  if (_data.alarms & SS_ALARM_EEPROM)          strlcat(_alarmDescBuffer, "EEPROM ", sizeof(_alarmDescBuffer));
  
  // Trim trailing space
  size_t len = strlen(_alarmDescBuffer);
  if (len > 0 && _alarmDescBuffer[len - 1] == ' ') {
    _alarmDescBuffer[len - 1] = '\0';
  }
  
  return _alarmDescBuffer;
}

void SolarManager::resetDailyStats() {
  // Note: The SunSaver resets its own daily stats at midnight (based on its RTC)
  // This just resets our local tracking
  _data.batteryVoltageMinDaily = _data.batteryVoltage;
  _data.batteryVoltageMaxDaily = _data.batteryVoltage;
  _data.ampHoursDaily = 0.0f;
  _data.wattHoursDaily = 0.0f;
  _data.dailyStatsSeeded = false;
}

// ---------------------------------------------------------------------------------------------
// Charger check: what the controller has stored, read a few registers at a time
// ---------------------------------------------------------------------------------------------

namespace {

// How the charger check reads: one register per request, with the function code the poll found
// to work. The other function code is not tried, so a register this controller does not have
// costs one request and no more.
class ChargerCheckBus : public sunsaver::Reader {
 public:
  ChargerCheckBus(uint8_t slaveId, uint8_t functionCode) : _slaveId(slaveId), _functionCode(functionCode) {}

  sunsaver::IoStatus readRegister(uint16_t addr, uint16_t *raw) override {
    TANKALARM_WATCHDOG_KICK(Watchdog::get_instance());

    // The poll's diagnostics (scErr, scMaddr, scResMs in the telemetry) describe the polls. A
    // register that an older controller refuses here is no failing link, so they are put back.
    char keptError[sizeof(sSolarLastModbusError)];
    char keptTag[sizeof(sSolarLastErrorTag)];
    memcpy(keptError, sSolarLastModbusError, sizeof(keptError));
    memcpy(keptTag, sSolarLastErrorTag, sizeof(keptTag));
    const uint16_t keptAddress = sSolarLastFailAddress;
    const uint16_t keptResponseMs = sSolarLastResponseMs;

    sSolarLastModbusError[0] = '\0';
    uint16_t word = 0;
    const bool ok = (_functionCode == 4) ? readInputRegisters(_slaveId, addr, 1, &word)
                                         : readHoldingRegisters(_slaveId, addr, 1, &word);
    sunsaver::IoStatus status = sunsaver::SSIO_OK;
    if (ok) {
      *raw = word;
    } else {
      // The controller answered with a Modbus exception, or nothing usable came back.
      status = solarErrorIsRefusal(sSolarLastModbusError) ? sunsaver::SSIO_REFUSED : sunsaver::SSIO_NO_REPLY;
    }

    memcpy(sSolarLastModbusError, keptError, sizeof(keptError));
    memcpy(sSolarLastErrorTag, keptTag, sizeof(keptTag));
    sSolarLastFailAddress = keptAddress;
    sSolarLastResponseMs = keptResponseMs;
    return status;
  }

 private:
  uint8_t _slaveId;
  uint8_t _functionCode;
};

}  // namespace

bool SolarManager::startChargerCheck() {
  // The poll comes first: it finds the function code, and a link it cannot use is not fit for
  // 49 more requests.
  if (!_config.enabled || !_initialized || !_data.lastPollOk || (_cachedHoldingFC != 3 && _cachedHoldingFC != 4)) {
    return false;
  }
  _chargerCheck.start();
  _chargerCheckStepMillis = millis();
  _chargerCheckGapMs = 0;
  return true;
}

void SolarManager::cancelChargerCheck() {
  _chargerCheck.cancel();
}

bool SolarManager::serviceChargerCheck(unsigned long nowMillis) {
  if (!_chargerCheck.active()) {
    return false;
  }
  if (!_config.enabled || !_initialized) {
    _chargerCheck.cancel();
    return false;
  }
  // Not into the silence after a request that got no reply, whether it was one of this pass
  // or of a poll. A step ends at such a request, so the rest begins with the step's end.
  if (lineResting() || nowMillis - _chargerCheckStepMillis < _chargerCheckGapMs) {
    return false;
  }
  ChargerCheckBus bus(_config.modbusSlaveId, _cachedHoldingFC);
  const bool ended = _chargerCheck.step(bus, SOLAR_CHARGER_CHECK_REGISTERS_PER_STEP);
  _chargerCheckStepMillis = millis();
  _chargerCheckGapMs = SOLAR_CHARGER_CHECK_STEP_GAP_MS;
  if (ended && _chargerCheck.done()) {
    _chargerSettingsRead = true;
  }
  return ended;
}

#else // Platform not supported

// Stub implementation for non-RS485 platforms
SolarManager::SolarManager()
  : _initialized(false), _lastPollMillis(0), _noChargeConsecutivePolls(0), _cachedHoldingFC(0),
    _floatReferenceV(0.0f), _readingFactor(1.0f), _packVolts(0), _packChemistry(sunsaver::CHEMISTRY_NOT_JUDGED),
    _chargerCheckStepMillis(0), _chargerCheckGapMs(0),
    _chargerSettingsRead(false) {
  memset(&_data, 0, sizeof(SolarData));
  memset(&_config, 0, sizeof(SolarConfig));
}

bool SolarManager::begin(const SolarConfig& config) {
  (void)config;
  Serial.println(F("Solar: RS485 not available on this platform"));
  return false;
}

void SolarManager::end() {}
void SolarManager::setConfig(const SolarConfig& config) { _config = config; }
bool SolarManager::poll(unsigned long nowMillis) { (void)nowMillis; return false; }
bool SolarManager::lineResting() const { return false; }
SolarAlertType SolarManager::checkAlerts() const { return SOLAR_ALERT_NONE; }
const char* SolarManager::getAlertDescription(SolarAlertType alert) const { (void)alert; return "N/A"; }
const char* SolarManager::getChargeStateDescription() const { return "N/A"; }
const char* SolarManager::getFaultDescription() const { return "N/A"; }
const char* SolarManager::getAlarmDescription() const { return "N/A"; }
void SolarManager::resetDailyStats() {}
bool SolarManager::readRegisters() { return false; }
float SolarManager::scaleVoltage(uint16_t raw) const { (void)raw; return 0.0f; }
float SolarManager::scaleCurrent(uint16_t raw) const { (void)raw; return 0.0f; }
void SolarManager::updateHealthStatus() {}
uint32_t SolarManager::getModbusErrorCount() const { return 0; }
const char* SolarManager::getLastModbusError() const { return ""; }
uint16_t SolarManager::getLastModbusFailAddress() const { return 0; }
uint16_t SolarManager::getLastModbusResponseMs() const { return 0; }
bool SolarManager::wasLastReadImplausible() const { return false; }
void SolarManager::resetModbusErrorStats() {}
// R1/R5 (v2.0.53) no-op stubs for the non-Opta build branch.
const char* SolarManager::getLastModbusErrorTag() const { return ""; }
uint32_t SolarManager::getModbusErrTimeout()      const { return 0; }
uint32_t SolarManager::getModbusErrCrc()          const { return 0; }
uint32_t SolarManager::getModbusErrIllegalAddr()  const { return 0; }
uint32_t SolarManager::getModbusErrIllegalFunc()  const { return 0; }
uint32_t SolarManager::getModbusErrOther()        const { return 0; }
bool SolarManager::startChargerCheck() { return false; }
void SolarManager::cancelChargerCheck() {}
bool SolarManager::serviceChargerCheck(unsigned long nowMillis) { (void)nowMillis; return false; }

#endif // Platform check
