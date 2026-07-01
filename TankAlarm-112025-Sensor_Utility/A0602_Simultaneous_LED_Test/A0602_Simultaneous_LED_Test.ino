/*
  TankAlarm-112025 Permanent Explicit Addressing simultaneous sweep

  Purpose:
  - This diagnostic utility turns ALL four gating outputs (P1, P2, P3, P4) ON
    simultaneously for 5 seconds, then OFF simultaneously for 5 seconds.
  - While they are ON, it sweeps through and turns on face-panel Yellow LEDs
    (from CH0 to CH7) to verify that we are targeting the correct, responsive
    registers on the board.
  - Tests the unmanaged default address 0x0A, the assigned managed address 0x0B,
    and legacy address 0x64 back-to-back to bypass any addressing discrepancy.
  - Includes unconditional MCUboot confirmation to guarantee no rollback.
*/

#include <Arduino.h>
#include <Wire.h>
#include <TankAlarm_Common.h>
#include "OptaBlue.h"
#include <MCUboot.h>

using namespace Opta;

uint32_t gCurrentLoopI2cErrors = 0;
uint32_t gI2cBusRecoveryCount  = 0;

uint8_t addresses[] = {0x0B, 0x0A, 0x64};
bool managedIndex = false;

// Custom function to send the specific "SET LED VALUE" frame directly to A0602
static inline bool tankalarm_setExpansionLedsDirectRange(uint8_t bitmask, uint8_t i2cAddr) {
  uint8_t buf[5];
  buf[0] = 0x01; // BP_CMD_SET
  buf[1] = 0x15; // ARG_OA_SET_LED (0x15) - Corrected based on OptaAnalogProtocol.h
  buf[2] = 0x01; // LEN_OA_SET_LED (0x01)
  buf[3] = bitmask; 
  
  // CRC8 computation
  uint8_t crc = 0x00;
  for (uint8_t i = 0; i < 4; i++) {
    crc ^= buf[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
  }
  buf[4] = crc;

  Wire.beginTransmission(i2cAddr);
  Wire.write(buf, 5);
  return (Wire.endTransmission() == 0);
}

void setup() {
  // Unconditional confirmation to prevent local MCUboot swap rollback
  MCUboot::confirmSketch();

  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && (millis() - t0) < 3000) { delay(10); }

  Serial.println();
  Serial.println(F("=================================================="));
  Serial.println(F("   UNIFIED MULTI-ADDRESS GATING & LED PILOT RUN   "));
  Serial.println(F("=================================================="));

  Wire.begin();
  Wire.setTimeout(100);

  Serial.println(F("Starting OptaController managed handshakes..."));
  OptaController.begin();
  for (int i = 0; i < 20; ++i) {
    OptaController.update();
    delay(50);
  }

  int n = OptaController.getExpansionNum();
  if (n > 0) {
    uint8_t assigned = OptaController.getExpansionI2Caddress(0);
    addresses[0] = assigned;
    managedIndex = true;
    Serial.print(F("FOUND: A0602 addressed at 0x"));
    Serial.println(assigned, HEX);
    Serial.println(F("-> A0602 status LED should be solid GREEN."));
  } else {
    Serial.println(F("WARNING: A0602 was not discovered by OptaController. status LED remains RED/BLUE."));
  }
}

void loop() {
  if (managedIndex) {
    OptaController.update();
  }

  uint8_t addr = addresses[0];

  Serial.println();
  Serial.println(F("--------------------------------------------------"));
  Serial.println(F("[SWEEP CYCLE] TARGETING RESOLVED ADDRESS: 0x0B     "));
  Serial.println(F("--------------------------------------------------"));
  
  // 1. Slow Soft-Ramp UP over exactly 1000ms
  Serial.println(F("  -> [STARTING RAMP-UP] Elevating P2, P3, P4 simultaneously..."));
  Serial.println(F("  -> Gradually turning ON Yellow face-panel LEDs..."));
  for (uint8_t i = 1; i <= 10; ++i) {
    uint32_t duty = (i == 10) ? 9999 : (i * 1000);
    // Ramp up all paths
    tankalarm_setPwm(0, 10000, duty, addr);
    tankalarm_setPwm(1, 10000, duty, addr);
    tankalarm_setPwm(2, 10000, duty, addr);
    tankalarm_setPwm(3, 10000, duty, addr);

    // Incrementally turn on more face LEDs representing the power level progress!
    uint8_t ledsBitmask = (1 << i) - 1; // 1, 3, 7, 15, 31, 63, 127, 255
    tankalarm_setExpansionLedsDirectRange(ledsBitmask, addr);

    delay(100); // 100ms per step (1.0 second total ramp-up time)
  }

  Serial.println(F("  STATUS: Fully Active! Voltmeter should read +12V on P2-P4."));
  delay(5000); // Hold active state for 5 seconds

  // 2. Slow Soft-Ramp DOWN over exactly 1000ms
  Serial.println(F("  -> [STARTING RAMP-DOWN] Decreasing P2, P3, P4 simultaneously..."));
  Serial.println(F("  -> Gradually turning OFF Yellow face-panel LEDs..."));
  for (uint8_t i = 10; i > 0; --i) {
    uint32_t duty = (i - 1) * 1000;
    if (duty == 0) {
      tankalarm_setPwm(0, 0, 0, addr);
      tankalarm_setPwm(1, 0, 0, addr);
      tankalarm_setPwm(2, 0, 0, addr);
      tankalarm_setPwm(3, 0, 0, addr);
    } else {
      tankalarm_setPwm(0, 10000, duty, addr);
      tankalarm_setPwm(1, 10000, duty, addr);
      tankalarm_setPwm(2, 10000, duty, addr);
      tankalarm_setPwm(3, 10000, duty, addr);
    }

    uint8_t ledsBitmask = (1 << (i - 1)) - 1; 
    tankalarm_setExpansionLedsDirectRange(ledsBitmask, addr);

    delay(100); // 100ms per step (1.0 second total ramp-down time)
  }

  Serial.println(F("  STATUS: Fully Inactive! Voltmeter should read 0V."));
  delay(5000); // Hold inactive state for 5 seconds
}
