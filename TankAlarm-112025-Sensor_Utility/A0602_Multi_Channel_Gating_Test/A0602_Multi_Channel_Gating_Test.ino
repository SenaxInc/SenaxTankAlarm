/*
  TankAlarm-112025 Multi-Channel PWM Gating Test Utility

  Purpose:
  - This diagnostic sketch cycles through all four gating terminals (P1, P2, P3, P4)
    on the A0602 module one by one.
  - Helps the operator visually verify if the correct high-side gate terminal is being
    energized and if the corresponding board level LEDs light up.
  - Can be used to check loop voltage output on each screw terminal with a voltmeter.
*/

#include <Arduino.h>
#include <Wire.h>
#include <TankAlarm_Common.h>

#ifndef CURRENT_LOOP_I2C_ADDRESS
#define CURRENT_LOOP_I2C_ADDRESS 0x64
#endif

uint32_t gCurrentLoopI2cErrors = 0;
uint32_t gI2cBusRecoveryCount  = 0;
uint8_t a0602I2cAddr = CURRENT_LOOP_I2C_ADDRESS;
bool a0602Located = false;

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && (millis() - t0) < 3000) { delay(10); }

  Serial.println();
  Serial.println(F("=================================================="));
  Serial.println(F("      A0602 MULTI-CHANNEL PWM SWEEP Gating Test"));
  Serial.println(F("=================================================="));

  Wire.begin();
  Wire.setTimeout(100);

  // Scan bus for the module
  for (uint8_t addr = 0x08; addr < 0x77; ++addr) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      if (addr == CURRENT_LOOP_I2C_ADDRESS || addr == 0x0A || addr == 0x0B) {
        Serial.print(F("Found A0602 Expansion at I2C address: 0x"));
        Serial.println(addr, HEX);
        a0602I2cAddr = addr;
        a0602Located = true;
      }
    }
  }

  if (!a0602Located) {
    Serial.print(F("No A0602 module detected. Using default address: 0x"));
    Serial.println(a0602I2cAddr, HEX);
  }
}

void loop() {
  // Loop through all four physical gating channels P1 (logical 0) to P4 (logical 3)
  for (uint8_t ch = 0; ch < 4; ++ch) {
    Serial.print(F("\n[SWEEP] Turning ON Physical Terminal P"));
    Serial.print(ch + 1);
    Serial.print(F(" (ch "));
    Serial.print(ch);
    Serial.println(F(")..."));

    // Turn channel fully ON
    bool actionOk = tankalarm_setPwm(ch, 10000, 9999, a0602I2cAddr);
    if (actionOk) {
      Serial.print(F("   Physical Terminal P"));
      Serial.print(ch + 1);
      Serial.println(F(" energized. Verify board LED is LIT and voltmeter shows +12V."));
    } else {
      Serial.print(F("   NACK / Failed to write command to ch "));
      Serial.println(ch);
    }

    // Keep it on for 5 seconds
    delay(5000);

    Serial.print(F("[SWEEP] Turning OFF Physical Terminal P"));
    Serial.print(ch + 1);
    Serial.println(F("..."));

    // Turn channel OFF
    tankalarm_setPwm(ch, 0, 0, a0602I2cAddr);
    
    // Pause briefly
    delay(2000);
  }

  Serial.println(F("\n======================================================="));
  Serial.println(F(" Sweep round complete! Starting next sweep in 5s..."));
  Serial.println(F("======================================================="));
  delay(5000);
}
