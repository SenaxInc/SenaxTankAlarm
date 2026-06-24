/**
 * TankAlarm_I2C.h
 * 
 * Shared I2C bus recovery, scanning, and current-loop reading functions
 * for TankAlarm 112025 components.
 * 
 * Consumed by Client, Server, Viewer, and I2C Utility sketches.
 * All functions are static inline to match the header-only pattern used
 * by TankAlarm_Diagnostics.h and TankAlarm_Platform.h.
 * 
 * DESIGN NOTES:
 *   - DFU guard: passed as a bool parameter (Client-specific, others pass false)
 *   - Watchdog kick: passed as a function pointer (each sketch provides its own)
 *   - Error counters: extern-declared here, defined in each sketch's globals
 *   - Bus scan: generic function that accepts expected address/name arrays
 * 
 * Copyright (c) 2025-2026 Senax Inc. All rights reserved.
 */

#ifndef TANKALARM_I2C_H
#define TANKALARM_I2C_H

#include <Arduino.h>
#include <Wire.h>
#include "TankAlarm_Config.h"

// ============================================================================
// I2C Recovery Trigger Types
// ============================================================================

/**
 * Identifies what caused an I2C bus recovery attempt.
 * Used for diagnostic logging via Notecard (diag.qo).
 */
enum I2CRecoveryTrigger {
  I2C_RECOVERY_NOTECARD_FAILURE = 0,  // Notecard unresponsive after threshold
  I2C_RECOVERY_SENSOR_ONLY     = 1,  // All current-loop sensors failing, Notecard OK
  I2C_RECOVERY_DUAL_FAILURE    = 2,  // Both Notecard and sensors failing
  I2C_RECOVERY_HEALTH_CHECK    = 3,  // Server/Viewer health check triggered
  I2C_RECOVERY_MANUAL          = 4   // I2C Utility or manual trigger
};

// ============================================================================
// I2C Error Counters (extern — defined in each sketch)
// ============================================================================

// Total I2C NACK / short-read errors on current-loop channels
extern uint32_t gCurrentLoopI2cErrors;
// Number of times recoverI2CBus() has been invoked since boot
extern uint32_t gI2cBusRecoveryCount;

// ============================================================================
// I2C Bus Recovery
// ============================================================================

/**
 * Attempt to recover a hung I2C bus by toggling SCL as GPIO.
 * 
 * Handles the classic I2C failure mode where a slave is stuck driving SDA low.
 * Sequence: Wire.end() → toggle SCL 16× → STOP condition → Wire.begin()
 * 
 * @param dfuInProgress  If true, skip recovery to avoid interfering with
 *                       firmware update transfer (Client-specific; Server/Viewer
 *                       pass false)
 * @param kickWatchdog   Optional function pointer to kick the hardware watchdog
 *                       before the time-consuming recovery procedure. Pass
 *                       nullptr on sketches without a watchdog.
 */
static inline void tankalarm_recoverI2CBus(
    bool dfuInProgress,
    void (*kickWatchdog)() = nullptr
) {
  if (dfuInProgress) {
    Serial.println(F("I2C recovery skipped - DFU in progress"));
    return;
  }

  if (kickWatchdog) {
    kickWatchdog();
  }

  Serial.println(F("I2C bus recovery: toggling SCL..."));

  // Deinitialize Wire to release pins
  Wire.end();

  // Toggle SCL manually to unstick any slave.
  // On Arduino Opta: Wire = I2C3, so SCL = PIN_WIRE_SCL (PH_7) and SDA = PIN_WIRE_SDA (PH_8).
  // (The PIN_WIRE_* macros already resolve to the correct pins; the prior PB_8/PB_9 comment was wrong.)
#if defined(ARDUINO_OPTA)
  const int I2C_SCL_PIN = PIN_WIRE_SCL;
  const int I2C_SDA_PIN = PIN_WIRE_SDA;
#else
  const int I2C_SCL_PIN = SCL;
  const int I2C_SDA_PIN = SDA;
#endif

  pinMode(I2C_SDA_PIN, INPUT);
  pinMode(I2C_SCL_PIN, OUTPUT);

  // Clock out up to 16 bits to free any stuck slave
  for (int i = 0; i < 16; i++) {
    digitalWrite(I2C_SCL_PIN, LOW);
    delayMicroseconds(5);
    digitalWrite(I2C_SCL_PIN, HIGH);
    delayMicroseconds(5);
  }

  // Generate STOP condition: SDA goes low then high while SCL is high
  digitalWrite(I2C_SDA_PIN, LOW);
  pinMode(I2C_SDA_PIN, OUTPUT);
  delayMicroseconds(5);
  digitalWrite(I2C_SCL_PIN, HIGH);
  delayMicroseconds(5);
  digitalWrite(I2C_SDA_PIN, HIGH);
  delayMicroseconds(5);

  // Reinitialize Wire (stays at 100 kHz default — see OPTA_I2C_COMMUNICATION.md)
  Wire.begin();
  Wire.setTimeout(I2C_WIRE_TIMEOUT_MS);  // Guard against indefinite blocking on bus hang

  gI2cBusRecoveryCount++;
  Serial.print(F("I2C bus recovery complete (count="));
  Serial.print(gI2cBusRecoveryCount);
  Serial.println(F(")"));
}

// ============================================================================
// I2C Bus Scan
// ============================================================================

/**
 * Result of an I2C bus scan.
 */
struct I2CScanResult {
  uint8_t foundCount;       // Number of expected devices found
  uint8_t expectedCount;    // Total expected devices
  uint8_t retryCount;       // Number of retry attempts used
  uint8_t unexpectedCount;  // Number of unexpected devices on the bus
  bool allFound;            // True if all expected devices responded
};

/**
 * Scan the I2C bus for expected devices with configurable retry.
 * Also reports any unexpected devices found on the bus.
 *
 * @param expectedAddrs  Array of expected I2C addresses
 * @param expectedNames  Array of human-readable device names (same order)
 * @param count          Number of entries in the arrays
 * @return I2CScanResult summarising what was found
 */
static inline I2CScanResult tankalarm_scanI2CBus(
    const uint8_t *expectedAddrs,
    const char * const *expectedNames,
    uint8_t count
) {
  I2CScanResult result = {0, count, 0, 0, false};

  while (result.retryCount < I2C_STARTUP_SCAN_RETRIES && !result.allFound) {
    if (result.retryCount > 0) {
      Serial.print(F("I2C bus scan: retry "));
      Serial.print(result.retryCount);
      Serial.print(F(" of "));
      Serial.println((uint8_t)(I2C_STARTUP_SCAN_RETRIES - 1));
      delay(I2C_STARTUP_SCAN_RETRY_DELAY_MS);
    } else {
      Serial.println(F("I2C bus scan:"));
    }

    result.allFound = true;
    result.foundCount = 0;
    for (uint8_t idx = 0; idx < count; idx++) {
      Wire.beginTransmission(expectedAddrs[idx]);
      uint8_t err = Wire.endTransmission();
      Serial.print(F("  0x"));
      if (expectedAddrs[idx] < 0x10) Serial.print('0');
      Serial.print(expectedAddrs[idx], HEX);
      Serial.print(F(" "));
      Serial.print(expectedNames[idx]);
      if (err == 0) {
        Serial.println(F(" - OK"));
        result.foundCount++;
      } else {
        Serial.print(F(" - NOT FOUND (err="));
        Serial.print(err);
        Serial.println(F(")"));
        result.allFound = false;
      }
    }
    result.retryCount++;
  }

  if (!result.allFound) {
    Serial.println(F("WARNING: Not all expected I2C devices found after retries"));
  }

  // Quick scan for unexpected devices
  result.unexpectedCount = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    bool isExpected = false;
    for (uint8_t idx = 0; idx < count; idx++) {
      if (addr == expectedAddrs[idx]) {
        isExpected = true;
        break;
      }
    }
    if (isExpected) continue;
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      result.unexpectedCount++;
      Serial.print(F("  0x"));
      if (addr < 0x10) Serial.print('0');
      Serial.print(addr, HEX);
      Serial.println(F(" - UNEXPECTED device"));
    }
  }

  return result;
}

// ============================================================================
// PWM / Output Gating (A0602 Expansion)
// ============================================================================

/**
 * Configure and trigger a PWM output on the A0602 expansion module over raw I2C.
 * Used for ultra-low-power solid-state power gating of connected transmitters.
 *
 * @param ch        PWM channel index (0-3 representing physical terminals P1-P4)
 * @param period    PWM period in microseconds (set to 0 to disable / pull LOW)
 * @param pulse     PWM pulse (high time) in microseconds (e.g. 10ms period with 9.99ms pulse for ON)
 * @param i2cAddr   I2C address of the A0602 module
 * @return True if message was successfully received and acknowledged, false otherwise
 */
static inline bool tankalarm_setPwm(
    uint8_t ch,
    uint32_t period,
    uint32_t pulse,
    uint8_t i2cAddr
) {
  uint8_t buf[13];
  
  // Header: command set, argument PWM set, payload length 9 bytes
  buf[0] = 0x01; // BP_CMD_SET
  buf[1] = 0x13; // ARG_OA_SET_PWM
  buf[2] = 0x09; // LEN_OA_SET_PWM
  
  // Payload:
  buf[3] = ch;   // Channel index (e.g. 0 for P1)
  
  // Period (uint32_t, little-endian)
  buf[4] = (uint8_t)(period & 0xFF);
  buf[5] = (uint8_t)((period >> 8) & 0xFF);
  buf[6] = (uint8_t)((period >> 16) & 0xFF);
  buf[7] = (uint8_t)((period >> 24) & 0xFF);
  
  // Pulse width (uint32_t, little-endian)
  buf[8] = (uint8_t)(pulse & 0xFF);
  buf[9] = (uint8_t)((pulse >> 8) & 0xFF);
  buf[10] = (uint8_t)((pulse >> 16) & 0xFF);
  buf[11] = (uint8_t)((pulse >> 24) & 0xFF);
  
  // Computation of standard CRC8 using polynomial 0x07 with 0x00 start value
  uint8_t crc = 0x00;
  for (uint8_t i = 0; i < 12; i++) {
    crc ^= buf[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      if (crc & 0x80) {
        crc = (crc << 1) ^ 0x07;
      } else {
        crc <<= 1;
      }
    }
  }
  buf[12] = crc;

  Wire.beginTransmission(i2cAddr);
  Wire.write(buf, 13);
  uint8_t err = Wire.endTransmission();
  return (err == 0);
}

// ============================================================================
// Current Loop Reading (A0602 Expansion)
// ============================================================================

/**
 * Read a 4-20mA current loop value from the A0602 expansion module over I2C.
 *
 * Retries up to I2C_CURRENT_LOOP_MAX_RETRIES on failure with detailed
 * error logging on the final attempt.  Increments gCurrentLoopI2cErrors
 * on terminal failure.
 *
 * @param channel   A0602 channel number (0-7)
 * @param i2cAddr   I2C address of the A0602 (default CURRENT_LOOP_I2C_ADDRESS)
 * @return Current in milliamps (4.0–20.0), or -1.0f on failure
 */
static inline float tankalarm_readCurrentLoopMilliamps(
    int16_t channel,
    uint8_t i2cAddr
) {
  if (channel < 0) {
    return -1.0f;
  }

  const uint8_t MAX_I2C_RETRIES = I2C_CURRENT_LOOP_MAX_RETRIES;
  for (uint8_t attempt = 0; attempt < MAX_I2C_RETRIES; attempt++) {
    if (attempt > 0) {
      delay(2);  // Brief delay between retries
    }

    Wire.beginTransmission(i2cAddr);
    Wire.write((uint8_t)channel);
    uint8_t err = Wire.endTransmission();
    if (err != 0) {
      if (attempt == MAX_I2C_RETRIES - 1) {
        // Log specific I2C error code on final attempt:
        // 1=data too long, 2=NACK address, 3=NACK data, 4=other, 5=timeout
        Serial.print(F("I2C NACK from 0x"));
        Serial.print(i2cAddr, HEX);
        Serial.print(F(" ch="));
        Serial.print(channel);
        Serial.print(F(" err="));
        Serial.println(err);
        gCurrentLoopI2cErrors++;
      }
      continue;
    }

    delay(1); // Give A0602 time to process the channel change

    if (Wire.requestFrom(i2cAddr, (uint8_t)2) != 2) {
      if (attempt == MAX_I2C_RETRIES - 1) {
        Serial.print(F("I2C short read from 0x"));
        Serial.print(i2cAddr, HEX);
        Serial.print(F(" ch="));
        Serial.println(channel);
        gCurrentLoopI2cErrors++;
      }
      // Drain any partial data from the Wire buffer
      while (Wire.available()) { Wire.read(); }
      continue;
    }

    // Guard Wire.read() with Wire.available() check
    if (Wire.available() < 2) {
      if (attempt == MAX_I2C_RETRIES - 1) {
        Serial.println(F("I2C buffer underrun"));
        gCurrentLoopI2cErrors++;
      }
      while (Wire.available()) { Wire.read(); }
      continue;
    }

    uint16_t raw = ((uint16_t)Wire.read() << 8) | Wire.read();
    return 4.0f + (raw / 65535.0f) * 16.0f;
  }

  return -1.0f;  // All retries exhausted
}

// ============================================================================
// Current Loop Reading — Official Blueprint Framed Protocol (A0602)  [v1.9.23]
// ============================================================================
// The legacy tankalarm_readCurrentLoopMilliamps() above does a bare
// `write(channel) / requestFrom(2)` shortcut that NEVER configures the A0602 input
// channel into 4-20mA current-ADC mode and does not use a real Blueprint GET frame.
// On standard A0602 firmware that returns a stale/default register (the constant ~18mA
// / 43.8psi symptom). The functions below speak the SAME framed protocol the official
// Arduino_Opta_Blueprint library uses (and that the proven Blueprint_CH0_Test relies on):
//   - configure the channel:  SET ARG_OA_CH_ADC (0x09), 7-byte payload, CRC8
//   - read the value:         GET ARG_OA_GET_ADC (0x0A), CRC8, answer parsed + CRC-verified
// All command IDs, payload byte order, the CRC (poly 0x07, init 0) and the 0-25mA ADC
// scale were copied verbatim from the installed library source (OptaAnalogProtocol.h,
// OptaMsgCommon.cpp, AnalogExpansion.cpp) — not guessed.
//
// Blueprint protocol constants (literal values mirrored from the library headers):
//   BP_CMD_SET=0x01 BP_CMD_GET=0x02 BP_ANS_GET=0x03 ; header is [CMD][ARG][LEN] then payload then CRC
//   ARG_OA_CH_ADC=0x09  LEN_OA_CH_ADC=0x07
//   ARG_OA_GET_ADC=0x0A LEN_OA_GET_ADC=0x01 ; ANS_LEN_OA_GET_ADC=0x03 (channel + 16-bit LE value)
//   OA_VOLTAGE_ADC=0 OA_CURRENT_ADC=1 ; OA_ENABLE=0x01 OA_DISABLE=0x02
// beginChannelAsCurrentAdc => beginChannelAsAdc(ch, OA_CURRENT_ADC, pull_down=false,
//   rejection=true, diagnostic=false, moving_avg=0, adding_adc=false).
// pinCurrent (plain current ADC channel) => mA = 25.0 * raw / 65535.0.

// CRC-8, polynomial 0x07, initial value 0x00 (Opta Blueprint frame CRC).
static inline uint8_t tankalarm_optaCrc8(const uint8_t *data, size_t len) {
  uint8_t crc = 0x00;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; ++b) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

/**
 * Configure an A0602 channel as a 4-20mA current ADC via the framed Blueprint protocol.
 * Must be called AFTER loop power is applied (e.g. after the P1 gate warmup) and BEFORE
 * tankalarm_readCurrentAdcFramed(); in the power-gated low-power model the channel config
 * is re-applied on every power-on cycle.
 *
 * @return true if the config frame was written (endTransmission ACK), false on I2C NACK.
 */
static inline bool tankalarm_configureCurrentAdcChannel(uint8_t channel, uint8_t i2cAddr) {
  uint8_t buf[11];
  buf[0] = 0x01;          // BP_CMD_SET
  buf[1] = 0x09;          // ARG_OA_CH_ADC
  buf[2] = 0x07;          // LEN_OA_CH_ADC (7-byte payload)
  buf[3] = channel;       // OA_CH_ADC_CHANNEL_POS
  buf[4] = 0x01;          // OA_CH_ADC_TYPE_POS = OA_CURRENT_ADC
  buf[5] = 0x02;          // OA_CH_ADC_PULL_DOWN_POS = OA_DISABLE (false)
  buf[6] = 0x01;          // OA_CH_ADC_REJECTION_POS = OA_ENABLE (true)
  buf[7] = 0x02;          // OA_CH_ADC_DIAGNOSTIC_POS = OA_DISABLE (false)
  buf[8] = 0x00;          // OA_CH_ADC_MOVING_AVE_POS = 0
  buf[9] = 0x02;          // OA_CH_ADC_ADDING_ADC_POS = OA_DISABLE (single ADC)
  buf[10] = tankalarm_optaCrc8(buf, 10);

  Wire.beginTransmission(i2cAddr);
  Wire.write(buf, 11);
  if (Wire.endTransmission() != 0) {
    return false;
  }
  // Read and validate the SET acknowledge frame: [BP_ANS_SET=0x04][ANS_ARG_OA_ACK=0x20]
  // [LEN=0x00][CRC]. v1.9.24 (post-impl review): a missing/garbled ACK means the channel-config
  // command was NOT confirmed by the module. This stays non-fatal at the call site (the GET read
  // CRC-validates the real channel state independently), but returning false surfaces the failed
  // config so it can be logged/diagnosed instead of being silently assumed successful.
  delay(1);
  uint8_t ack[4];
  uint8_t an = 0;
  uint8_t agot = Wire.requestFrom(i2cAddr, (uint8_t)4);
  while (Wire.available() && an < 4) { ack[an++] = Wire.read(); }
  while (Wire.available()) { (void)Wire.read(); }
  if (agot != 4 || an != 4) {
    return false;
  }
  if (ack[0] != 0x04 || ack[1] != 0x20 || ack[2] != 0x00) {
    return false;
  }
  if (tankalarm_optaCrc8(ack, 3) != ack[3]) {
    return false;
  }
  return true;
}

/**
 * Read one 4-20mA sample from an A0602 channel using the framed GET protocol with full
 * answer header + CRC validation. Returns milliamps on the A0602's 0-25mA current-ADC
 * scale, or -1.0f on ANY I2C/framing/CRC failure (caller treats <0 as a fault — it never
 * fabricates a plausible value from a bad frame).
 */
static inline float tankalarm_readCurrentAdcFramed(uint8_t channel, uint8_t i2cAddr) {
  uint8_t req[5];
  req[0] = 0x02;          // BP_CMD_GET
  req[1] = 0x0A;          // ARG_OA_GET_ADC
  req[2] = 0x01;          // LEN_OA_GET_ADC
  req[3] = channel;       // OA_CH_ADC_CHANNEL_POS
  req[4] = tankalarm_optaCrc8(req, 4);

  Wire.beginTransmission(i2cAddr);
  Wire.write(req, 5);
  if (Wire.endTransmission() != 0) {
    return -1.0f;
  }
  delay(1); // allow the expansion to stage its answer buffer
  // Answer = [BP_ANS_GET=0x03][ARG=0x0A][LEN=0x03][channel][value_lo][value_hi][CRC] = 7 bytes
  const uint8_t ANS_LEN = 7;
  uint8_t got = Wire.requestFrom(i2cAddr, ANS_LEN);
  uint8_t a[7];
  uint8_t n = 0;
  while (Wire.available() && n < ANS_LEN) { a[n++] = Wire.read(); }
  while (Wire.available()) { (void)Wire.read(); }
  if (got != ANS_LEN || n != ANS_LEN) {
    return -1.0f;
  }
  // Validate framing: command, argument, length, then CRC over the first 6 bytes.
  if (a[0] != 0x03 || a[1] != 0x0A || a[2] != 0x03) {
    return -1.0f;
  }
  if (tankalarm_optaCrc8(a, 6) != a[6]) {
    return -1.0f;
  }
  // v1.9.24 (post-impl review): reject an answer for a different channel. Guards against bus
  // cross-talk or a stale answer buffer from a prior transaction returning another channel's
  // value, which CRC alone would not catch (a different channel's frame is still valid CRC).
  if (a[3] != channel) {
    return -1.0f;
  }
  uint16_t raw = (uint16_t)a[4] | ((uint16_t)a[5] << 8); // little-endian
  // v2.0.46: an all-ones raw sample is the I2C "bus pulled high"/garbage signature. It scales to
  // 25mA — above a 4-20mA loop's full scale and indistinguishable-by-value from a real reading
  // once converted to float. Reject it here, while the raw word is still in hand, so a failed
  // frame can never masquerade as a ~25mA reading downstream.
  if (raw == 0xFFFF) {
    return -1.0f;
  }
  return 25.0f * (float)raw / 65535.0f; // A0602 current-ADC full scale (matches pinCurrent)
}

// Blueprint channel-function value for a 4-20mA current ADC with external loop power.
// Verified against the installed Arduino_Opta_Blueprint library (AnalogCommonCfg.h CfgFun_t):
// CH_FUNC_CURRENT_INPUT_EXT_POWER = 4. COUPLING: the expansion reports this function for our
// configure frame ONLY because tankalarm_configureCurrentAdcChannel() sends
// ADDING_ADC = OA_DISABLE. If that byte ever becomes OA_ENABLE the channel reports
// CH_FUNC_CURRENT_INPUT_LOOP_POWER (5) and THIS constant must change with it.
#ifndef TANKALARM_OA_FUNC_CURRENT_INPUT_EXT_POWER
#define TANKALARM_OA_FUNC_CURRENT_INPUT_EXT_POWER 0x04
#endif

/**
 * Query the ACTUAL configured function of an A0602 channel via the framed Blueprint
 * GET_CHANNEL_FUNCTION (0x40) protocol. The SET CH_ADC ACK only means "command queued" --
 * the expansion applies the channel reconfiguration later in its own main loop. This read
 * confirms the AD74412R is genuinely in current-ADC mode before we trust a reading.
 * Frame verified against OptaAnalogProtocol.h: request [0x02][0x40][0x01][ch][CRC];
 * answer [0x03][0x40][0x02][ch][fun][CRC].
 * Returns true + funOut on a fully-validated answer; false on ANY I2C/framing/CRC failure
 * (false means "could not read the function", NOT "wrong function" -- the caller must
 * distinguish the two so an A0602 firmware that doesn't implement 0x40 is not hard-failed).
 */
static inline bool tankalarm_getAnalogChannelFunction(uint8_t channel, uint8_t i2cAddr, uint8_t &funOut) {
  if (channel >= 8) {
    return false;
  }
  uint8_t req[5];
  req[0] = 0x02;        // BP_CMD_GET
  req[1] = 0x40;        // ARG_GET_CHANNEL_FUNCTION
  req[2] = 0x01;        // LEN_GET_CHANNEL_FUNCTION
  req[3] = channel;     // GET_CHANNEL_FUNCTION_CH_POS
  req[4] = tankalarm_optaCrc8(req, 4);

  Wire.beginTransmission(i2cAddr);
  Wire.write(req, 5);
  if (Wire.endTransmission() != 0) {
    return false;
  }
  delay(1); // allow the expansion to stage its answer buffer
  // Answer = [BP_ANS_GET=0x03][ARG=0x40][LEN=0x02][channel][function][CRC] = 6 bytes
  const uint8_t ANS_LEN = 6;
  uint8_t a[6];
  uint8_t n = 0;
  uint8_t got = Wire.requestFrom(i2cAddr, ANS_LEN);
  while (Wire.available() && n < ANS_LEN) { a[n++] = Wire.read(); }
  while (Wire.available()) { (void)Wire.read(); }
  if (got != ANS_LEN || n != ANS_LEN) {
    return false;
  }
  if (a[0] != 0x03 || a[1] != 0x40 || a[2] != 0x02 || a[3] != channel) {
    return false;
  }
  if (tankalarm_optaCrc8(a, 5) != a[5]) {
    return false;
  }
  funOut = a[4];
  return true;
}

/**
 * Poll tankalarm_getAnalogChannelFunction() until the channel reports current-ADC mode
 * (CH_FUNC_CURRENT_INPUT_EXT_POWER) or timeoutMs elapses. Convenience wrapper; callers that
 * need to distinguish "unreadable" from "wrong function" should call the getter directly.
 */
static inline bool tankalarm_waitCurrentAdcFunction(uint8_t channel, uint8_t i2cAddr, uint16_t timeoutMs = 100) {
  const unsigned long start = millis();
  for (;;) {
    uint8_t fun = 0xFF;
    if (tankalarm_getAnalogChannelFunction(channel, i2cAddr, fun) &&
        fun == TANKALARM_OA_FUNC_CURRENT_INPUT_EXT_POWER) {
      return true;
    }
    if ((millis() - start) >= timeoutMs) {
      return false;
    }
    delay(5);
  }
}

#endif // TANKALARM_I2C_H
