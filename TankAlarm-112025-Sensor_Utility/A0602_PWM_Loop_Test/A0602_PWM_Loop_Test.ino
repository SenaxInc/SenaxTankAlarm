/*
  TankAlarm-112025 A0602 PWM Switched-Supply Loop Validation (P2 -> I1+)

  PURPOSE (2026-07-02 bench session)
  ----------------------------------
  Validates the v2.1.3 PWM loop-power method using the PRODUCTION helpers from
  TankAlarm_I2C.h, on the bench wiring:

      12V battery ---> Vpwm rail
      P2 (PWM ch 1) -> transmitter (+)          <- P1 is FRIED on this unit, do not use
      transmitter (-) -> I1+ (analog ch 0)      <- loop returns through internal 100R to GND

  Each duty cycle exercises the exact production sequence from readCurrentLoopSensor()'s
  PWM branch:
    1. tankalarm_rampUpPwm(P2)      - SOFT-START, 10 steps over ~1s (inrush protection)
    2. ACK probe setPwm(P2, full)   - "did the expansion accept PWM" (pwm_nack detection)
    3. tankalarm_configureCurrentAdcChannel(ch0) - externally-powered current ADC
    4. LED CH0 pilot ON
    5. 3000ms warmup
    6. 5x tankalarm_readCurrentAdcFramed(ch0) @300ms - UNIPOLAR scale (ext-power mode)
    7. tankalarm_rampDownPwm(P2)    - SOFT-STOP, 10 steps over ~1s (flyback protection)
    8. LED off, 8s idle (voltmeter P2 during idle should read ~0V, ~12V during burst)

  EXPECTED: ~4.0-4.6 mA with the Dwyer 0-50psi transmitter at a few feet of head
  (4.48mA at 1.5psi). A steady value in the 4-20mA span = wiring + method VALIDATED.

  Board: Arduino Opta + A0602   FQBN: arduino:mbed_opta:opta:security=sien
  Build with -DTANKALARM_DFU_MCUBOOT and --library TankAlarm-112025-Common
*/

#include <Wire.h>
#include <TankAlarm_Common.h>   // production raw helpers (TankAlarm_I2C.h)
#include "OptaBlue.h"

#if defined(TANKALARM_DFU_MCUBOOT)
#include <MCUboot.h>
#endif

using namespace Opta;

// Required by TankAlarm_I2C.h (extern-declared there)
uint32_t gCurrentLoopI2cErrors = 0;
uint32_t gI2cBusRecoveryCount  = 0;

#define SENSOR_CH 0   // I1+ / I1- (measurement input)
#define PWM_CH    1   // P2 terminal (P1 is fried on this unit)

static uint8_t gAddr = 0x0B;
static uint32_t gCycle = 0;

void setup() {
#if defined(TANKALARM_DFU_MCUBOOT)
  MCUboot::confirmSketch();  // lock this image so the bootloader does not roll back
#endif
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && (millis() - t0) < 3000) delay(10);

  Serial.println(F("\n=================================================="));
  Serial.println(F(" A0602 PWM SWITCHED-SUPPLY LOOP VALIDATION (v2.1.3)"));
  Serial.println(F("=================================================="));
  Serial.println(F("Wiring: battery->Vpwm, P2->sensor(+), sensor(-)->I1+"));
  Serial.println(F("PWM output: P2 (ch 1) with SOFT RAMP up/down"));
  Serial.println(F("Expected: steady ~4.0-4.6 mA (0-50psi Dwyer, low head)"));

  // Client-identical bootstrap: discover the A0602's managed address once, then
  // leave the controller alone and restore the Notecard-safe 100 kHz clock.
  OptaController.begin();
  OptaController.update();
  delay(20);
  OptaController.update();
  int n = OptaController.getExpansionNum();
  for (int i = 0; i < n; ++i) {
    if (OptaController.getExpansionType(i) == EXPANSION_OPTA_ANALOG) {
      gAddr = OptaController.getExpansionI2Caddress(i);
      break;
    }
  }
  Wire.setClock(100000);
  Serial.print(F("A0602 managed at 0x"));
  Serial.println(gAddr, HEX);
  Serial.println(F("Bus at 100 kHz; starting duty cycles...\n"));
}

void loop() {
  gCycle++;
  Serial.print(F("---------- DUTY CYCLE "));
  Serial.print(gCycle);
  Serial.println(F(" ----------"));

  // 1. Soft-start the P2 gate (production inrush protection)
  Serial.println(F("[1] Soft ramp-up P2: 0 -> 99.99% duty over ~1s (10 steps)"));
  tankalarm_rampUpPwm(PWM_CH, gAddr);

  // 2. ACK probe (production pwm_nack detection)
  bool pwmOk = false;
  for (uint8_t attempt = 0; attempt < 3 && !pwmOk; ++attempt) {
    if (attempt > 0) delay(5);
    pwmOk = tankalarm_setPwm(PWM_CH, 10000, 9999, gAddr);
  }
  Serial.print(F("[2] PWM full-on ACK probe: "));
  Serial.println(pwmOk ? F("OK") : F("FAIL (pwm_nack)"));
  if (!pwmOk) {
    tankalarm_rampDownPwm(PWM_CH, gAddr);
    Serial.println(F("    aborting cycle; retry in 8s"));
    delay(8000);
    return;
  }

  // 3. Configure the sensor channel as an externally-powered current ADC
  bool cfgOk = false;
  for (uint8_t attempt = 0; attempt < 3 && !cfgOk; ++attempt) {
    if (attempt > 0) delay(5);
    cfgOk = tankalarm_configureCurrentAdcChannel(SENSOR_CH, gAddr);
  }
  Serial.print(F("[3] CH0 ext-power current ADC config: "));
  Serial.println(cfgOk ? F("OK") : F("FAIL (config_nack)"));
  if (!cfgOk) {
    tankalarm_rampDownPwm(PWM_CH, gAddr);
    (void)tankalarm_setExpansionLeds(0x00, gAddr);
    delay(8000);
    return;
  }

  // 4. Pilot LED on (mirrors production: sensor channel LED while loop is powered)
  (void)tankalarm_setExpansionLeds(0x01 << SENSOR_CH, gAddr);

  // 5. Warmup (production default 3000ms; config applies during this window)
  Serial.println(F("[4] Warmup 3000ms (transmitter boot + config apply)..."));
  delay(3000);

  // 6. Sampling burst - UNIPOLAR production reader (ext-power scale)
  Serial.println(F("[5] 5 framed samples (unipolar 0-25mA scale):"));
  float total = 0.0f;
  uint8_t valid = 0;
  for (uint8_t s = 0; s < 5; ++s) {
    float ma = tankalarm_readCurrentAdcFramed(SENSOR_CH, gAddr);
    Serial.print(F("    sample "));
    Serial.print(s + 1);
    Serial.print(F(": "));
    if (ma >= 0.0f) {
      Serial.print(ma, 3);
      Serial.println(F(" mA"));
      total += ma;
      valid++;
    } else {
      Serial.println(F("READ FAIL"));
    }
    if (s < 4) delay(300);
  }

  if (valid > 0) {
    float avg = total / valid;
    Serial.print(F("[6] AVERAGE: "));
    Serial.print(avg, 3);
    Serial.print(F(" mA (n="));
    Serial.print(valid);
    Serial.print(F("/5) -> "));
    if (avg >= 3.6f && avg <= 21.0f) {
      Serial.println(F("VALID 4-20mA READING - wiring + PWM method OK"));
    } else if (avg < 3.6f) {
      Serial.println(F("below live-zero: loop open / unpowered / polarity?"));
    } else {
      Serial.println(F("over-range: check wiring"));
    }
  } else {
    Serial.println(F("[6] ALL SAMPLES FAILED - check A0602 comms/config"));
  }

  // 7. Soft-stop the gate (production flyback protection)
  Serial.println(F("[7] Soft ramp-down P2: 99.99% -> 0 over ~1s (10 steps)"));
  tankalarm_rampDownPwm(PWM_CH, gAddr);
  (void)tankalarm_setExpansionLeds(0x00, gAddr);

  Serial.println(F("[8] Loop unpowered. Idle 8s (P2 should voltmeter ~0V now)\n"));
  delay(8000);
}
