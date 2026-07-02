/*
  A0602 DAC Loop-Power RAW-FRAME Diagnostic (v2.1.0 read-fail forensics)

  Reproduces the CLIENT's exact raw-I2C sequence and dumps every byte:
    - OptaController bootstrap ONCE (like client setup), then 100 kHz
    - Raw framed SET CH_DAC / SET DAC 11V / SET CH_ADC adding_adc  (ACK-checked)
    - LED pilot on
    - Phase B: 5 framed GET_ADC reads IMMEDIATELY (300 ms apart) w/ hex dumps
    - Phase C: 3000 ms of BUS SILENCE (client warmup model), then 5 more reads
    - Phase D: high-impedance + LED off; repeat forever every 10 s

  Discriminates:
    H1 "GET_ADC doesn't work on DAC+added-ADC channels" -> Phase B fails too
    H2 "A0602 comms-timeout resets DAC during silent warmup" -> B ok, C fails
    H3 "config frames rejected" -> ACK dumps show it
    H4 "sensor/wiring" -> frames valid but raw==0 everywhere
*/

#include <Arduino.h>
#include <Wire.h>
#include "OptaBlue.h"

#if defined(TANKALARM_DFU_MCUBOOT)
#include <MCUboot.h>
#endif

using namespace Opta;

#define SENSOR_CH 0

static uint8_t gAddr = 0x0B;

static uint8_t crc8(const uint8_t *d, size_t n) {
  uint8_t c = 0;
  for (size_t i = 0; i < n; ++i) {
    c ^= d[i];
    for (uint8_t b = 0; b < 8; ++b) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
  }
  return c;
}

static void dumpHex(const char *tag, const uint8_t *d, uint8_t n) {
  Serial.print(tag);
  for (uint8_t i = 0; i < n; ++i) {
    Serial.print(' ');
    if (d[i] < 0x10) Serial.print('0');
    Serial.print(d[i], HEX);
  }
  Serial.println();
}

// Send a SET frame then read+dump the 4-byte ACK. Returns true on valid ACK.
static bool sendSetFrame(const char *tag, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(gAddr);
  uint8_t w = Wire.write(buf, len);
  uint8_t e = Wire.endTransmission();
  Serial.print(tag); Serial.print(F(" write=")); Serial.print(w);
  Serial.print(F(" err=")); Serial.println(e);
  if (w != len || e != 0) return false;
  delay(1);
  uint8_t ack[8]; uint8_t n = 0;
  uint8_t got = Wire.requestFrom(gAddr, (uint8_t)4);
  while (Wire.available() && n < 8) ack[n++] = Wire.read();
  dumpHex("  ACK:", ack, n);
  if (got != 4 || n < 4) return false;
  return (ack[0] == 0x04 && ack[1] == 0x20 && ack[2] == 0x00 && crc8(ack, 3) == ack[3]);
}

static bool cfgDacLoop(uint8_t ch, uint16_t settleMs) {
  uint8_t b1[9] = {0x01, 0x0C, 0x05, ch, 0x00, 0x02, 0x02, 0x00, 0};
  b1[8] = crc8(b1, 8);
  if (!sendSetFrame("CH_DAC", b1, 9)) return false;
  if (settleMs) delay(settleMs);   // let expansion main loop APPLY the DAC function
  uint8_t b2[8] = {0x01, 0x0D, 0x04, ch, 0xFF, 0x1F, 0x01, 0};
  b2[7] = crc8(b2, 7);
  if (!sendSetFrame("SET_DAC", b2, 8)) return false;
  if (settleMs) delay(settleMs);   // let the 11V value latch into the applied DAC
  uint8_t b3[11] = {0x01, 0x09, 0x07, ch, 0x01, 0x02, 0x01, 0x02, 0x00, 0x01, 0};
  b3[10] = crc8(b3, 10);
  if (!sendSetFrame("CH_ADC+add", b3, 11)) return false;
  return true;
}

// GET channel function (0x40): reveals what the expansion APPLIED.
// Expect 1=VOLTAGE_OUTPUT after CH_DAC, 5=CURRENT_INPUT_LOOP_POWER after add-ADC special case,
// 4=CURRENT_INPUT_EXT_POWER if the special case was MISSED (timing bug signature).
static void readFun(uint8_t ch) {
  uint8_t req[5] = {0x02, 0x40, 0x01, ch, 0};
  req[4] = crc8(req, 4);
  Wire.beginTransmission(gAddr);
  Wire.write(req, 5);
  if (Wire.endTransmission() != 0) { Serial.println(F("FUN write err")); return; }
  delay(1);
  uint8_t a[8]; uint8_t n = 0;
  uint8_t got = Wire.requestFrom(gAddr, (uint8_t)6);
  while (Wire.available() && n < 8) a[n++] = Wire.read();
  dumpHex("  FUN ANS:", a, n);
  if (got == 6 && n >= 6 && a[0] == 0x03 && a[1] == 0x40 && crc8(a, 5) == a[5]) {
    Serial.print(F("  -> applied function = "));
    Serial.println(a[4]);
  }
}

static bool setLeds(uint8_t mask) {
  uint8_t b[5] = {0x01, 0x15, 0x01, mask, 0};
  b[4] = crc8(b, 4);
  Wire.beginTransmission(gAddr);
  Wire.write(b, 5);
  return Wire.endTransmission() == 0;
}

static bool setHighZ(uint8_t ch) {
  uint8_t b[5] = {0x01, 0x24, 0x01, ch, 0};
  b[4] = crc8(b, 4);
  return sendSetFrame("HIGH_Z", b, 5);
}

// Framed GET_ADC with full dump. Returns raw or -1.
static int32_t readAdcRaw(uint8_t ch) {
  uint8_t req[5] = {0x02, 0x0A, 0x01, ch, 0};
  req[4] = crc8(req, 4);
  Wire.beginTransmission(gAddr);
  Wire.write(req, 5);
  uint8_t e = Wire.endTransmission();
  if (e != 0) { Serial.print(F("GET_ADC write err=")); Serial.println(e); return -1; }
  delay(1);
  uint8_t a[10]; uint8_t n = 0;
  uint8_t got = Wire.requestFrom(gAddr, (uint8_t)7);
  while (Wire.available() && n < 10) a[n++] = Wire.read();
  dumpHex("  ANS:", a, n);
  if (got != 7 || n < 7) { Serial.println(F("  -> short answer")); return -1; }
  if (a[0] != 0x03 || a[1] != 0x0A || a[2] != 0x03) { Serial.println(F("  -> bad header")); return -1; }
  if (crc8(a, 6) != a[6]) { Serial.println(F("  -> bad CRC")); return -1; }
  if (a[3] != ch) { Serial.println(F("  -> wrong channel echo")); return -1; }
  uint16_t raw = (uint16_t)a[4] | ((uint16_t)a[5] << 8);
  Serial.print(F("  -> raw=")); Serial.print(raw);
  Serial.print(F(" mA=")); Serial.println(25.0f * raw / 65535.0f, 3);
  return raw;
}

static void readBurst(const char *tag, uint8_t count, uint16_t gapMs) {
  Serial.println(tag);
  for (uint8_t i = 0; i < count; ++i) {
    (void)readAdcRaw(SENSOR_CH);
    if (i < count - 1) delay(gapMs);
  }
}

void setup() {
#if defined(TANKALARM_DFU_MCUBOOT)
  MCUboot::confirmSketch();
#endif
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && (millis() - t0) < 3000) delay(10);

  Serial.println(F("\n=== A0602 DAC RAW-FRAME DIAG (client-sequence forensics) ==="));

  // Client-identical bootstrap: begin + 2 updates only, then 100 kHz.
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
  Serial.print(F("A0602 at 0x")); Serial.println(gAddr, HEX);
  Serial.println(F("Bus clock now 100 kHz; controller will NOT be polled again."));
}

void loop() {
  static bool useSettle = false;   // alternate: back-to-back vs settled config
  useSettle = !useSettle;
  uint16_t settle = useSettle ? 300 : 0;

  Serial.println(F("\n---------- DUTY CYCLE ----------"));
  Serial.print(F("[1] Raw DAC loop-power config (settleMs="));
  Serial.print(settle); Serial.println(F("):"));
  bool ok = cfgDacLoop(SENSOR_CH, settle);
  Serial.print(F("cfgDacLoop -> ")); Serial.println(ok ? F("OK") : F("FAIL"));
  readFun(SENSOR_CH);
  setLeds(0x01);

  if (ok) {
    // Phase B: immediate reads (no silent gap) - H1 discriminator
    readBurst("[2] Phase B: reads IMMEDIATELY after config (300ms apart):", 5, 300);

    // Phase C: silent warmup like the client, then reads - H2 discriminator
    Serial.println(F("[3] Phase C: 3000ms BUS SILENCE (client warmup model)..."));
    delay(3000);
    readBurst("    reads after silent warmup:", 5, 300);
    readFun(SENSOR_CH);
  }

  Serial.println(F("[5] Power down:"));
  setHighZ(SENSOR_CH);
  setLeds(0x00);
  Serial.println(F("Sleeping 5s until next duty cycle..."));
  delay(5000);
}
