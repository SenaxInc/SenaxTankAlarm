/*
  TankAlarm-112025 A0602 DAC Loop-Powered (I1+/I1-) Verification

  Purpose:
  - This diagnostic utility tests the internally powered loop-powered (2-wire) 
    method directly on the A0602 analog channels.
  - Instead of using PWM channels (P1-P4) for external power gating, this method:
    1. Directs analog CH0 (terminals I1+ and I1-) to act as a Voltage DAC at 11V.
    2. Uses that 11V output on the I1+ terminal to power your sensor's loop.
    3. Adds a current ADC to the SAME channel internally so we can read the 4-20mA draw via pinCurrent()!
  - Bypasses the blown/damaged PWM P1 MOSFET completely and is highly
    resilient to external wiring damage!
  - ALSO provides a continuous calibrate-zero session: keeps loop-power active
    on CH0 continuously so you can hold down the Dwyer 626/628 zero-point button
    for 10+ seconds while watching real-time mA conversion.
  - Turns ON Yellow face LED CH0 to act as a solid power-pilot light!
*/

#include <Arduino.h>
#include <Wire.h>
#include "OptaBlue.h"

#if defined(TANKALARM_DFU_MCUBOOT)
#include <MCUboot.h>            // this Opta has the MCUboot bootloader
#endif

using namespace Opta;

#define PERIODIC_UPDATE_TIME 500 // Speed up display to 500ms for fast calibration feedback
#define SENSOR_CH 0 // Channel 0 (Terminal I1+ and I1-)

uint32_t gCurrentLoopI2cErrors = 0;
uint32_t gI2cBusRecoveryCount  = 0;

// Custom function to send the specific "SET LED VALUE" frame directly to A0602
// Turning on bitmask 0x01 lights up face Yellow LED CH0 as a pilot light.
static inline bool tankalarm_setExpansionLedsDirect(uint8_t bitmask, uint8_t i2cAddr) {
  uint8_t buf[5];
  buf[0] = 0x01; // BP_CMD_SET
  buf[1] = 0x15; // ARG_OA_SET_LED (0x15)
  buf[2] = 0x01; // LEN_OA_SET_LED
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
#if defined(TANKALARM_DFU_MCUBOOT)
  // Lock this diagnostic program so MCUboot does not rollback
  MCUboot::confirmSketch();
#endif

  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && (millis() - t0) < 3000) { delay(10); }

  Serial.println();
  Serial.println(F("=================================================="));
  Serial.println(F(" A0602 DWYER SENSOR CALIBRATION & PILOT LED RUN "));
  Serial.println(F("=================================================="));
  Serial.println(F("Initializing OptaController..."));

  OptaController.begin();

  // Run initial assignment updates
  for (int i = 0; i < 20; ++i) {
    OptaController.update();
    delay(50);
  }

  int expansions = OptaController.getExpansionNum();
  Serial.print(F("Discovered expansions: "));
  Serial.println(expansions);

  uint8_t resolvedAddr = 0x0B;

  for (int i = 0; i < expansions; i++) {
    AnalogExpansion exp = OptaController.getExpansion(i);
    if (exp) {
      resolvedAddr = OptaController.getExpansionI2Caddress(i);
      Serial.print(F("Opta Analog Expansion ["));
      Serial.print(i);
      Serial.print(F("] found at address 0x"));
      Serial.println(resolvedAddr, HEX);
      Serial.println(F("Configuring CH0 as Voltage DAC..."));

      // 1. Initialize channel 0 as a voltage DAC
      // Set limit current to false (allows channel to output up to 25mA to power the sensor loop)
      exp.beginChannelAsDac(SENSOR_CH,        // channel index
                            OA_VOLTAGE_DAC,   // DAC type
                            false,            // limit current (false so it can power the loop)
                            false,            // No slew rate
                            OA_SLEW_RATE_0);  // Slew rate setting.

      Serial.println(F("Setting DAC output to 11.0V (max output voltage)..."));
      exp.pinVoltage(SENSOR_CH, 11.0, true);

      // Give some time for the voltage output to stabilize
      delay(300);

      // 2. Add current ADC to the same channel
      Serial.println(F("Adding Current ADC on CH0 to read internal current draw..."));
      exp.addCurrentAdcOnChannel(SENSOR_CH);
    }
  }

  // 3. Command Yellow LED CH0 to turn ON to signify active power gating
  Serial.println(F("Illuminating face Yellow LED CH0 as a Pilot Light..."));
  tankalarm_setExpansionLedsDirect(0x01, resolvedAddr);
  
  Serial.println(F("\n=================================================="));
  Serial.println(F("  CALIBRATION SESSION ACTIVE INDEFINITELY          "));
  Serial.println(F("=================================================="));
  Serial.println(F("INSTRUCTIONS FOR OPERATOR:"));
  Serial.println(F("  1. Watch the A0602 Face-Panel."));
  Serial.println(F("     -> The CH0 Yellow face LED is SOLID LIT (Pilot ON)."));
  Serial.println(F("  2. Locate the physical button on your Dwyer 626/628 sensor body."));
  Serial.println(F("  3. Press and HOLD the sensor button continuously for 10 seconds."));
  Serial.println(F("  4. Watch this serial console:"));
  Serial.println(F("     -> The Current Draw should transition from ~3.69mA to ~4.00mA!"));
  Serial.println(F("==================================================\n"));
}

void loop() {
  OptaController.update();

  static unsigned long lastUpdate = 0;
  unsigned long now = millis();

  if (now - lastUpdate > PERIODIC_UPDATE_TIME) {
    lastUpdate = now;

    for (int i = 0; i < OptaController.getExpansionNum(); i++) {
      AnalogExpansion exp = OptaController.getExpansion(i);
      if (exp) {
        float value = exp.pinCurrent(SENSOR_CH);
        Serial.print(F("CH0 (I1+/I1-) -> Live Current Draw: "));
        Serial.print(abs(value), 3);
        Serial.println(F(" mA"));
      }
    }
  }
}
