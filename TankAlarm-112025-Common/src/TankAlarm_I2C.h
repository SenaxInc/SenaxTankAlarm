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
  // On Arduino Opta: SCL = PIN_WIRE_SCL (PB_8), SDA = PIN_WIRE_SDA (PB_9)
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

#endif // TANKALARM_I2C_H
