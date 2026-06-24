/*
  TankAlarm-112025 A0602 Diagnostic — RAW vs MANAGED discriminator (USB only)

  WHY THIS EXISTS
  ---------------
  The v2.0.46 "I2C contention" fix did not correct the field symptom (a stuck / never-valid
  4-20mA current-loop reading), and the A0602 status LED was observed RED (= "ready for
  address" = UNMANAGED; healthy is GREEN = "has address"). Production firmware talks RAW
  framed I2C and never runs OptaController, so the module is never driven through the
  Blueprint bootstrap. This sketch decides, in ONE boot, whether that is the cause.

  See CODE REVIEW/OPTA_A0602_USB_DIAGNOSTIC_PLAN_06242026.md (Phase C2 / decision tree).

  WHAT IT DOES (one automatic pass on boot, re-runnable with 'r')
    PHASE 1  RAW production protocol BEFORE any managed bootstrap, @ 0x64 and 0x0A
             (setPwm P1 ON -> configure current ADC -> GET channel function -> framed read),
             printing raw bytes + CRC for every transaction.
    PHASE 2  MANAGED bootstrap via the official OptaController / OptaBlue API
             (the A0602 status LED should turn GREEN), enumerate (type + assigned address),
             configure all channels as current ADC, read pinCurrent()/getAdc().
    PHASE 3  RAW production protocol AGAIN, @ 0x64, 0x0A, and the ASSIGNED address.

  INTERPRETATION (printed as a heuristic verdict at the end)
    - managed read OK (~4mA) but RAW-before failed  => production RAW/UNMANAGED path is the bug
    - RAW works only AFTER managed, at the ASSIGNED addr => module must be managed first; 0x64 wrong
    - no expansion enumerated / LED never GREEN     => AUX bus / module power / bootstrap problem
    - both managed and RAW-before work              => not an addressing/management fault

  This is a DIAGNOSTIC sketch. It does NOT touch the Notecard and changes no production behavior.

  Board: Arduino Opta + Opta Ext A0602   FQBN: arduino:mbed_opta:opta   Serial: 115200
*/

#include <Wire.h>
#include <TankAlarm_Common.h>   // raw helpers: tankalarm_setPwm / configureCurrentAdcChannel /
                                // readCurrentAdcFramed / getAnalogChannelFunction / optaCrc8 + config
#include "OptaBlue.h"           // official managed API: OptaController / AnalogExpansion

#if defined(TANKALARM_DFU_MCUBOOT)
#include <MCUboot.h>            // this Opta has the MCUboot bootloader; it rejects UNSIGNED images.
                                // Build signed (security=sien) and confirm the boot (see setup()).
#endif

using namespace Opta;

// Required by TankAlarm_I2C.h (extern-declared there)
uint32_t gCurrentLoopI2cErrors = 0;
uint32_t gI2cBusRecoveryCount  = 0;

#ifndef SERIAL_BAUD
#define SERIAL_BAUD 115200
#endif

// Addresses the production firmware probes (mirror the client defaults)
#ifndef CURRENT_LOOP_I2C_ADDRESS
#define CURRENT_LOOP_I2C_ADDRESS 0x64
#endif
#ifndef CURRENT_LOOP_I2C_ALT_ADDRESS_1
#define CURRENT_LOOP_I2C_ALT_ADDRESS_1 0x0A   // == OPTA_DEFAULT_SLAVE_I2C_ADDRESS (unmanaged)
#endif

static const uint8_t  DIAG_ADDR_PRIMARY = CURRENT_LOOP_I2C_ADDRESS;        // 0x64
static const uint8_t  DIAG_ADDR_DEFAULT = CURRENT_LOOP_I2C_ALT_ADDRESS_1;  // 0x0A
static const uint8_t  DIAG_CHANNEL      = 0;     // CH0 (wellhead channel in the field)
static const uint8_t  DIAG_PWM_CHANNEL  = 0;     // P1 high-side gate
static const uint32_t DIAG_WARMUP_MS    = 800;   // short fixed loop warmup for diagnostics
static const uint32_t DIAG_I2C_CLOCK_HZ = 100000;

static bool    gControllerStarted = false;
static uint8_t gManagedAddr       = 0x00;   // address OptaController assigns (captured in Phase 2)
static bool    gExpansionDetected = false;
static bool    gManagedReadOk     = false;
static uint32_t gBootMs           = 0;      // millis() at end of setup()
static bool    gAutoRan           = false;  // full sequence has auto-run once

// Result of one verbose framed GET-ADC transaction
struct FramedResult {
  bool     txOk;       // endTransmission == 0
  bool     gotFrame;   // received the full 7-byte answer
  bool     headerOk;   // 0x03 0x0A 0x03
  bool     crcOk;
  bool     chOk;
  uint16_t raw;
  float    ma;
};

// ---------------------------------------------------------------------------
// small print helpers
// ---------------------------------------------------------------------------
static void p2(uint8_t v) { if (v < 0x10) Serial.print('0'); Serial.print(v, HEX); }

static void rule(const char *title) {
  Serial.println();
  Serial.println(F("=================================================="));
  Serial.print(F(" "));
  Serial.println(title ? title : "");
  Serial.println(F("=================================================="));
  Serial.flush();
}

static bool i2cAck(uint8_t a) {
  if (a < 0x08 || a > 0x77) return false;
  Wire.beginTransmission(a);
  return (Wire.endTransmission() == 0);
}

// ---------------------------------------------------------------------------
// Full bus scan — reveals exactly which addresses ACK (0x17 / 0x64 / 0x0A / 0x0B..)
// ---------------------------------------------------------------------------
static void fullBusScan() {
  Serial.println(F("  I2C bus scan (0x08..0x77):"));
  uint8_t count = 0;
  for (uint8_t a = 0x08; a <= 0x77; ++a) {
    if (!i2cAck(a)) continue;
    count++;
    Serial.print(F("    0x")); p2(a); Serial.print(F("  ACK"));
    if (a == NOTECARD_I2C_ADDRESS)        Serial.print(F("   <- Notecard"));
    else if (a == DIAG_ADDR_PRIMARY)      Serial.print(F("   <- A0602 production addr (0x64)"));
    else if (a == DIAG_ADDR_DEFAULT)      Serial.print(F("   <- Blueprint UNMANAGED default (0x0A)"));
    else if (a >= 0x0B && a <= 0x0F)      Serial.print(F("   <- Blueprint ASSIGNED range"));
    Serial.println();
  }
  Serial.print(F("    total devices: ")); Serial.println(count);
}

// ---------------------------------------------------------------------------
// Verbose framed GET-ADC read (prints every byte + CRC) — mirrors the production
// tankalarm_readCurrentAdcFramed() wire format but never hides the bytes.
// ---------------------------------------------------------------------------
static FramedResult diagFramedRead(uint8_t addr, uint8_t channel) {
  FramedResult r = { false, false, false, false, false, 0, NAN };

  uint8_t req[5];
  req[0] = 0x02;  // BP_CMD_GET
  req[1] = 0x0A;  // ARG_OA_GET_ADC
  req[2] = 0x01;  // LEN
  req[3] = channel;
  req[4] = tankalarm_optaCrc8(req, 4);

  Wire.beginTransmission(addr);
  Wire.write(req, 5);
  r.txOk = (Wire.endTransmission() == 0);
  Serial.print(F("    framed GET ADC  -> tx ")); Serial.print(r.txOk ? F("ACK") : F("NACK"));
  if (!r.txOk) { Serial.println(); return r; }

  delay(1);
  uint8_t a[7];
  uint8_t n = 0;
  uint8_t got = Wire.requestFrom(addr, (uint8_t)7);
  while (Wire.available() && n < 7) { a[n++] = Wire.read(); }
  while (Wire.available()) { (void)Wire.read(); }
  r.gotFrame = (got == 7 && n == 7);

  Serial.print(F("  bytes=")); Serial.print(n); Serial.print(F(" ["));
  for (uint8_t i = 0; i < n; ++i) { p2(a[i]); if (i + 1 < n) Serial.print(' '); }
  Serial.print(F("]"));
  if (!r.gotFrame) { Serial.println(F("  (short read)")); return r; }

  r.headerOk = (a[0] == 0x03 && a[1] == 0x0A && a[2] == 0x03);
  r.crcOk    = (tankalarm_optaCrc8(a, 6) == a[6]);
  r.chOk     = (a[3] == channel);
  r.raw      = (uint16_t)a[4] | ((uint16_t)a[5] << 8);
  r.ma       = 25.0f * (float)r.raw / 65535.0f;

  Serial.print(F("  hdr=")); Serial.print(r.headerOk ? F("ok") : F("BAD"));
  Serial.print(F(" crc="));  Serial.print(r.crcOk ? F("ok") : F("BAD"));
  Serial.print(F(" ch="));   Serial.print(r.chOk ? F("ok") : F("BAD"));
  Serial.print(F(" raw=0x")); p2((r.raw >> 8) & 0xFF); p2(r.raw & 0xFF);
  Serial.print(F(" mA=")); Serial.println(r.ma, 3);
  return r;
}

// Legacy 1-byte-write / 2-byte-read shortcut (the pre-v1.9.23 path) — shows what a
// stale output register returns.
static void diagLegacyRead(uint8_t addr, uint8_t channel) {
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)channel);
  uint8_t e = Wire.endTransmission();
  Serial.print(F("    legacy 1+2     -> tx ")); Serial.print(e == 0 ? F("ACK") : F("NACK"));
  if (e != 0) { Serial.println(); return; }
  delay(1);
  (void)Wire.requestFrom(addr, (uint8_t)2);
  uint8_t b0 = 0, b1 = 0, n = 0;
  if (Wire.available()) { b0 = Wire.read(); n++; }
  if (Wire.available()) { b1 = Wire.read(); n++; }
  while (Wire.available()) { (void)Wire.read(); }
  if (n < 2) { Serial.println(F("  (short read)")); return; }
  uint16_t raw = ((uint16_t)b0 << 8) | b1;
  Serial.print(F("  bytes=[")); p2(b0); Serial.print(' '); p2(b1);
  Serial.print(F("] mA(legacy)=")); Serial.println(4.0f + (raw / 65535.0f) * 16.0f, 3);
}

// ---------------------------------------------------------------------------
// One full RAW production-style transaction at a given address.
// ---------------------------------------------------------------------------
static FramedResult rawProductionSequence(uint8_t addr, const char *when) {
  Serial.print(F("  -- RAW @ 0x")); p2(addr); Serial.print(F(" (")); Serial.print(when); Serial.println(F(") --"));
  FramedResult r = { false, false, false, false, false, 0, NAN };

  bool ack = i2cAck(addr);
  Serial.print(F("    ack=")); Serial.println(ack ? F("yes") : F("no"));
  if (!ack) return r;

  // Gate P1 ON exactly as production does (10ms period, 9.999ms pulse), retry 3x.
  bool on = false;
  for (uint8_t i = 0; i < 3 && !on; ++i) {
    if (i) delay(5);
    on = tankalarm_setPwm(DIAG_PWM_CHANNEL, 10000, 9999, addr);
  }
  Serial.print(F("    setPwm P")); Serial.print(DIAG_PWM_CHANNEL + 1);
  Serial.print(F(" ON -> ")); Serial.println(on ? F("ACK") : F("NACK"));
  delay(DIAG_WARMUP_MS);

  // Configure CH as current ADC (framed) and confirm the channel function.
  bool cfg = tankalarm_configureCurrentAdcChannel(DIAG_CHANNEL, addr);
  Serial.print(F("    configureCurrentAdc -> ")); Serial.println(cfg ? F("ACK") : F("NACK/garbled"));

  uint8_t fun = 0xFF;
  bool fOk = tankalarm_getAnalogChannelFunction(DIAG_CHANNEL, addr, fun);
  Serial.print(F("    getChannelFunction  -> "));
  if (fOk) { Serial.print(F("fun=0x")); p2(fun); Serial.println(F("  (expect 0x04 CURRENT_INPUT_EXT_POWER)")); }
  else     { Serial.println(F("read failed")); }

  // Production helper (rejects 0xFFFF, returns -1 on any framing/CRC fault).
  float prodMa = tankalarm_readCurrentAdcFramed(DIAG_CHANNEL, addr);
  Serial.print(F("    prod readFramed    -> "));
  if (prodMa < 0) Serial.println(F("FAULT (-1)"));
  else { Serial.print(prodMa, 3); Serial.println(F(" mA")); }

  // Verbose byte-level dumps.
  r = diagFramedRead(addr, DIAG_CHANNEL);
  diagLegacyRead(addr, DIAG_CHANNEL);

  // Gate P1 OFF.
  (void)tankalarm_setPwm(DIAG_PWM_CHANNEL, 0, 0, addr);
  return r;
}

// ---------------------------------------------------------------------------
// Managed bootstrap via the official OptaController / OptaBlue API.
// ---------------------------------------------------------------------------
static void managedSequence() {
  if (!gControllerStarted) {
    OptaController.begin();
    gControllerStarted = true;
    Serial.println(F("  OptaController.begin() done -- WATCH THE A0602 STATUS LED."));
    Serial.println(F("  GREEN = module managed/addressed (healthy). RED/BLUE = still unmanaged."));
  }
  OptaController.update();
  delay(50);
  OptaController.update();

  int n = OptaController.getExpansionNum();
  Serial.print(F("  Detected expansions: ")); Serial.println(n);
  gExpansionDetected = (n > 0);

  for (int i = 0; i < n; ++i) {
    Serial.print(F("    expansion[")); Serial.print(i);
    Serial.print(F("] type=")); Serial.print((int)OptaController.getExpansionType(i));
    uint8_t addr = OptaController.getExpansionI2Caddress(i);
    Serial.print(F(" i2c=0x")); p2(addr); Serial.println();
    if (gManagedAddr == 0) gManagedAddr = addr;
  }

  if (n <= 0) {
    Serial.println(F("  No expansion enumerated. (LED likely NOT green.)"));
    Serial.println(F("  -> Check AUX connector seating and the external 12-24V expansion supply."));
    return;
  }

  for (int i = 0; i < n; ++i) {
    AnalogExpansion exp = OptaController.getExpansion(i);
    if (!exp) continue;

    for (int ch = 0; ch < OA_AN_CHANNELS_NUM; ++ch) {
      AnalogExpansion::beginChannelAsCurrentAdc(OptaController, i, ch);
      delay(2);
    }
    exp.updateAnalogInputs();

    Serial.print(F("  managed reads (exp ")); Serial.print(exp.getIndex()); Serial.println(F("):"));
    for (int ch = 0; ch < 8; ++ch) {
      uint16_t adc = exp.getAdc(ch, false);
      float ma = exp.pinCurrent(ch, false);
      Serial.print(F("    CH")); Serial.print(ch);
      Serial.print(F(": adc=")); Serial.print(adc);
      Serial.print(F(" mA="));
      if (isnan(ma)) Serial.println(F("NaN / READ FAIL"));
      else           Serial.println(ma, 3);
      if (ch == DIAG_CHANNEL && !isnan(ma) && ma >= 3.5f && ma <= 20.5f) gManagedReadOk = true;
    }
  }
}

// ---------------------------------------------------------------------------
// Verdict heuristic
// ---------------------------------------------------------------------------
static void printVerdict(bool rawBeforeOk, bool rawAfterAssignedOk) {
  rule("VERDICT (heuristic -- confirm against the plan doc)");
  if (gManagedReadOk && !rawBeforeOk) {
    Serial.println(F("  BRANCH 1: managed read works, RAW-before did NOT."));
    Serial.println(F("  -> The production RAW/UNMANAGED path is the suspect; HW/bus/power look OK."));
    if (rawAfterAssignedOk) {
      Serial.print(F("  -> RAW works AFTER managed bootstrap at assigned 0x")); p2(gManagedAddr);
      Serial.println(F(": the module must be MANAGED first, and 0x64 is the wrong address."));
    }
    Serial.println(F("  FIX DIRECTION: adopt OptaController/AnalogExpansion in production, or run"));
    Serial.println(F("  the Blueprint bootstrap so the module is addressed/GREEN before any raw read."));
  } else if (gManagedReadOk && rawBeforeOk) {
    Serial.println(F("  Managed AND RAW-before both work -> not an addressing/management fault."));
    Serial.println(F("  -> Re-examine bus contention/timing under FULL production load (Notecard+Modbus)."));
  } else if (!gExpansionDetected) {
    Serial.println(F("  BRANCH 3: OptaController did NOT enumerate the A0602 (LED likely not green)."));
    Serial.println(F("  -> AUX/expansion bus or module power/health. Not a firmware contention issue."));
  } else {
    Serial.println(F("  BRANCH 2: expansion enumerated but managed read failed/invalid."));
    Serial.println(F("  -> Loop power (P1 gate), channel wiring, or the transmitter. Limited further over USB."));
  }
}

// ---------------------------------------------------------------------------
// Full automatic pass
// ---------------------------------------------------------------------------
static void printMenu();

static void runFullSequence() {
  gExpansionDetected = false;
  gManagedReadOk     = false;
  gManagedAddr       = 0;   // re-capture each pass (keep gControllerStarted)

  rule("PHASE 1: RAW production protocol BEFORE managed bootstrap");
  fullBusScan();
  FramedResult b64 = rawProductionSequence(DIAG_ADDR_PRIMARY, "before");
  FramedResult b0a = rawProductionSequence(DIAG_ADDR_DEFAULT, "before");
  bool rawBeforeOk = (b64.headerOk && b64.crcOk) || (b0a.headerOk && b0a.crcOk);

  rule("PHASE 2: MANAGED bootstrap (OptaController / OptaBlue)");
  managedSequence();

  rule("PHASE 3: RAW production protocol AFTER managed bootstrap");
  fullBusScan();
  (void)rawProductionSequence(DIAG_ADDR_PRIMARY, "after");
  (void)rawProductionSequence(DIAG_ADDR_DEFAULT, "after");
  FramedResult am = { false, false, false, false, false, 0, NAN };
  if (gManagedAddr && gManagedAddr != DIAG_ADDR_PRIMARY && gManagedAddr != DIAG_ADDR_DEFAULT) {
    am = rawProductionSequence(gManagedAddr, "after@assigned");
  }
  bool rawAfterAssignedOk = (am.headerOk && am.crcOk);

  printVerdict(rawBeforeOk, rawAfterAssignedOk);
  printMenu();
}

// ---------------------------------------------------------------------------
// banner / menu / serial loop
// ---------------------------------------------------------------------------
static void printBanner() {
  Serial.println();
  Serial.println(F("=================================================="));
  Serial.println(F(" TankAlarm-112025 A0602 Diagnostic (RAW vs MANAGED)"));
  Serial.println(F("=================================================="));
  Serial.print(F(" Firmware base: v")); Serial.print(F(FIRMWARE_VERSION));
  Serial.print(F("  build ")); Serial.print(F(__DATE__)); Serial.print(F(" ")); Serial.println(F(__TIME__));
  Serial.println(F(" Probes A0602 @ 0x64 and 0x0A on CH0, P1 gate. Notecard untouched."));
}

static void printMenu() {
  Serial.println();
  Serial.println(F("Commands:"));
  Serial.println(F("  r  - Re-run full PHASE 1/2/3 sequence"));
  Serial.println(F("  s  - I2C bus scan only"));
  Serial.println(F("  1  - RAW production protocol only (0x64 + 0x0A)"));
  Serial.println(F("  2  - MANAGED (OptaController) bootstrap + read only"));
  Serial.println(F("  g  - Toggle P1 high-side gate (observe the A0602 output LED)"));
  Serial.println(F("  h  - Help"));
  Serial.println();
}

void setup() {
#if defined(TANKALARM_DFU_MCUBOOT)
  // MCUboot board: confirm this freshly-flashed image immediately so the bootloader does not
  // revert it as a failed trial boot. (Mirrors the production client's early confirmSketch().)
  MCUboot::confirmSketch();
#endif
  Serial.begin(SERIAL_BAUD);
  unsigned long t0 = millis();
  while (!Serial && (millis() - t0) < 1500) { delay(10); }

  printBanner();

  Wire.begin();
  Wire.setClock(DIAG_I2C_CLOCK_HZ);     // 100k to start (OptaController raises to 400k in Phase 2)
  Wire.setTimeout(I2C_WIRE_TIMEOUT_MS);

  // setup() intentionally returns FAST and does NO I2C here, so the USB-CDC serial enumerates
  // cleanly BEFORE any heavy/blocking bus work. On this host a long blocking setup() repeatedly
  // hung the CDC endpoint ("device not functioning") on boot. The PHASE 1/2/3 sequence runs from
  // loop(): automatically once after a short grace period, or on demand via r / 1 / 2.
  Serial.println(F("setup() complete (fast) -- USB serial is live."));
  Serial.println(F("Auto-running full sequence in ~5s; or press r / 1 / 2 now."));
  printMenu();
  Serial.flush();
  gBootMs = millis();
}

void loop() {
  if (gControllerStarted) {
    OptaController.update();
  }

  // Deferred auto-run: give the USB-CDC ~5s to stabilize after the fast setup() before any
  // heavy/blocking I2C, which on this host has repeatedly hung the serial endpoint on boot.
  if (!gAutoRan && (millis() - gBootMs) > 5000) {
    gAutoRan = true;
    runFullSequence();
  }

  if (!Serial.available()) { delay(10); return; }
  char c = (char)Serial.read();
  if (c == '\r' || c == '\n') return;

  switch (c) {
    case 'h': case '?': printMenu(); break;
    case 'r': gAutoRan = true; runFullSequence(); break;
    case 's': rule("I2C bus scan"); fullBusScan(); break;
    case '1':
      rule("RAW production protocol only");
      fullBusScan();
      (void)rawProductionSequence(DIAG_ADDR_PRIMARY, "manual");
      (void)rawProductionSequence(DIAG_ADDR_DEFAULT, "manual");
      break;
    case '2':
      rule("MANAGED (OptaController) only");
      managedSequence();
      break;
    case 'g': {
      static bool on = false;
      on = !on;
      uint8_t addr = gManagedAddr ? gManagedAddr : DIAG_ADDR_PRIMARY;
      bool ok = on ? tankalarm_setPwm(DIAG_PWM_CHANNEL, 10000, 9999, addr)
                   : tankalarm_setPwm(DIAG_PWM_CHANNEL, 0, 0, addr);
      Serial.print(F("P")); Serial.print(DIAG_PWM_CHANNEL + 1);
      Serial.print(on ? F(" ON @0x") : F(" OFF @0x")); p2(addr);
      Serial.print(F(" -> ")); Serial.println(ok ? F("ACK") : F("NACK"));
      break;
    }
    default:
      Serial.print(F("Unknown: ")); Serial.println(c);
      printMenu();
      break;
  }
}
