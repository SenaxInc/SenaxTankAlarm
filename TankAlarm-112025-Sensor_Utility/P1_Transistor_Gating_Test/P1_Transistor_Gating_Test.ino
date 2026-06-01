/*
  TankAlarm-112025 P1 Transistor Gating & Sensor Reading Utility

  Purpose:
  - This is a diagnostic program to verify physical gating AND take direct USB reads of your
    4-20mA sensor on Channel 0 (CH0) connected via the A0602 expansion module.
  - It automatically scans the I2C bus to find the A0602 module address.
  - It repeatedly cycle-tests:
    1. Turns P1 ON (High-side transistor channel 0).
    2. Waits for 3.0 seconds (3000ms) of stabilization delay.
    3. Takes several sequential 4-20mA current loop readings from CH0, spaced 300ms apart.
    4. Calculates pressure (PSI) using a 0-50 PSI range to check if it matches 1.61psi (4.516ma).
    5. Displays all intermediate reads so you see if the reading is stable or drifting.
    6. Turns P1 OFF to simulate the low-power gating cycle.
    7. Pauses for 5 seconds before the next test cycle.

  How to use:
  1. Upload this sketch to the Arduino Opta.
  2. Open the Serial Monitor at 115200 baud.
  3. Observe the transistor gate state and direct sensor reads on CH0.
*/

#include <Arduino.h>
#include <Wire.h>
#include <TankAlarm_Common.h>

#ifndef NOTECARD_I2C_ADDRESS
#define NOTECARD_I2C_ADDRESS 0x17
#endif

// Default I2C Address for A0602 is 0x64
#ifndef CURRENT_LOOP_I2C_ADDRESS
#define CURRENT_LOOP_I2C_ADDRESS 0x64
#endif

uint8_t resolvedA0602Addr = CURRENT_LOOP_I2C_ADDRESS;
bool a0602Found = false;

// We use the common helpers declared in <TankAlarm_I2C.h>
extern uint32_t gCurrentLoopI2cErrors;
extern uint32_t gI2cBusRecoveryCount;

// Dummy definitions for extern requirements
uint32_t gCurrentLoopI2cErrors = 0;
uint32_t gI2cBusRecoveryCount = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {
    delay(10);
  }

  Serial.println();
  Serial.println(F("=================================================="));
  Serial.println(F("      P1 GATING & CH0 SENSOR DIRECT USB READER"));
  Serial.println(F("=================================================="));
  Serial.println(F("Initializing I2C bus..."));

  Wire.begin();
  Wire.setTimeout(100); // 100ms timeout

  // Probe LEDs
#if defined(LED_D0)
  pinMode(LED_D0, OUTPUT);
  digitalWrite(LED_D0, LOW);
#endif

  Serial.println(F("Scanning I2C bus for A0602 and Blues Notecard..."));
  for (uint8_t addr = 0x08; addr < 0x77; ++addr) {
    Wire.beginTransmission(addr);
    byte err = Wire.endTransmission();
    if (err == 0) {
      Serial.print(F(" -> Found device at I2C address: 0x"));
      if (addr < 16) Serial.print(F("0"));
      Serial.print(addr, HEX);

      if (addr == NOTECARD_I2C_ADDRESS) {
        Serial.println(F(" [Blues Notecard]"));
      } else if (addr == CURRENT_LOOP_I2C_ADDRESS || addr == 0x0A || addr == 0x0B) {
        Serial.println(F(" [A0602 Expansion Module]"));
        resolvedA0602Addr = addr;
        a0602Found = true;
      } else {
        Serial.println(F(" [Unknown Device]"));
      }
    }
  }

  if (!a0602Found) {
    Serial.println(F("WARNING: A0602 Expansion module not detected during I2C scan."));
    Serial.print(F("Falling back to default address: 0x"));
    Serial.println(resolvedA0602Addr, HEX);
  } else {
    Serial.print(F("Using resolved A0602 I2C Address: 0x"));
    Serial.println(resolvedA0602Addr, HEX);
  }

  Serial.println(F("\nStarting Gating + Sensor Sampling Loop..."));
}

void loop() {
  Serial.println();
  Serial.println(F("---------------------------------------------"));
  Serial.println(F("[GATING CYCLE STARTED]: Turning P1 ON..."));

  // 1. Enable P1 transistor (ch=0)
  bool onOk = tankalarm_setPwm(0, 10000, 9999, resolvedA0602Addr);
  if (onOk) {
    Serial.println(F("SUCCESS: Transistor enabled. LED D0 is HIGH."));
#if defined(LED_D0)
    digitalWrite(LED_D0, HIGH);
#endif
  } else {
    Serial.println(F("ERROR: Failed to turn on P1 via I2C."));
  }

  // 2. Wait for stabilization delay
  Serial.println(F("STABILIZATION: Waiting 3.0 seconds (3000ms) for sensor power to stabilize..."));
  delay(3000);

  // 3. Take 5 sequential readings to observe stability and values
  Serial.println(F("SAMPLING: Reading Channel 0 (CH0) 5 times, 300ms apart..."));
  float sumMa = 0;
  int validReads = 0;

  for (int s = 1; s <= 5; ++s) {
    float ma = tankalarm_readCurrentLoopMilliamps(0, resolvedA0602Addr);
    if (ma >= 0.0f) {
      float psi = (ma - 4.0f) * (50.0f / 16.0f);
      if (psi < 0.0f) psi = 0.0f; // clamp negative noise
      
      Serial.print(F("  -> Read #"));
      Serial.print(s);
      Serial.print(F(": "));
      Serial.print(ma, 4);
      Serial.print(F(" mA | Scaled: "));
      Serial.print(psi, 4);
      Serial.println(F(" psi"));
      
      sumMa += ma;
      validReads++;
    } else {
      Serial.print(F("  -> Read #"));
      Serial.print(s);
      Serial.println(F(": FAILED (I2C read error)"));
    }
    delay(300);
  }

  if (validReads > 0) {
    float avgMa = sumMa / validReads;
    float avgPsi = (avgMa - 4.0f) * (50.0f / 16.0f);
    if (avgPsi < 0.0f) avgPsi = 0.0f;

    Serial.println();
    Serial.print(F("SUCCESSFUL AVERAGE: "));
    Serial.print(avgMa, 4);
    Serial.print(F(" mA | "));
    Serial.print(avgPsi, 4);
    Serial.println(F(" psi"));
  } else {
    Serial.println(F("\nERROR: All sensor readings failed."));
  }

  // 4. Turn P1 transistor OFF
  Serial.println();
  Serial.println(F("[GATING CYCLE ENDED]: Turning P1 OFF..."));
  bool offOk = tankalarm_setPwm(0, 0, 0, resolvedA0602Addr);
  if (offOk) {
    Serial.println(F("SUCCESS: Transistor disabled. LED D0 is LOW."));
#if defined(LED_D0)
    digitalWrite(LED_D0, LOW);
#endif
  } else {
    Serial.println(F("ERROR: Failed to turn off P1 via I2C."));
  }

  // Wait 10 seconds before starting the next reading
  Serial.println(F("Sleeping 10 seconds before next measurement..."));
  delay(10000);
}
