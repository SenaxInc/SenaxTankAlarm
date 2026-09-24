// Host tests for TankAlarm-112025-Common/src/TankAlarm_OptaIo.h (CL-1).
//
// Build and run: make -C tests/host/opta_io test
//
// Expected values are written out (pin numbers from Opta core 4.6.0, variants/OPTA/pins_arduino.h),
// never recomputed with the function under test. Terminals are 0-based here: I3 is terminal 2
// and pin 17.

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "TankAlarm_OptaIo.h"

static unsigned long gChecks = 0;
static unsigned long gFailures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    ++gChecks;                                                         \
    if (!(cond)) {                                                     \
      ++gFailures;                                                     \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
    }                                                                  \
  } while (0)

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

// Exhaustive loops count their mismatches and report the first one, instead of a line each.
struct Tally {
  unsigned long bad;
  long first;
};

static void tally(Tally &t, bool ok, long value) {
  if (ok) return;
  if (t.bad == 0) t.first = value;
  ++t.bad;
}

#define CHECK_TALLY(name, t)                                                          \
  do {                                                                                \
    ++gChecks;                                                                        \
    if ((t).bad != 0) {                                                               \
      ++gFailures;                                                                    \
      printf("FAIL %s:%d: %s: %lu mismatches, first at %ld\n", __FILE__, __LINE__,    \
             name, (t).bad, (t).first);                                               \
    }                                                                                 \
  } while (0)

// Contacts read by analogRead (the plan's default) or by digitalRead with no pull; pulse with
// the v2.2.16 pull-up. Written out so the tests do not follow OPTA_IO_DEFAULT_OPTIONS.
static const OptaIoOptions kAdc = { true, OPTA_PULL_NONE, OPTA_PULL_UP };
static const OptaIoOptions kGpio = { false, OPTA_PULL_NONE, OPTA_PULL_UP };

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

static OptaIoRequest emptyRequest() {
  OptaIoRequest r;
  memset(&r, 0, sizeof(r));
  for (uint8_t i = 0; i < OPTA_IO_MAX_MONITORS; ++i) {
    r.monitors[i].iface = 0xEE;  // past monitorCount: must never be read
    r.monitors[i].primaryPin = 3;
    r.monitors[i].pulsePin = 3;
  }
  r.monitorCount = 0;
  r.buttonTerminal = -1;
  r.vinEnabled = false;
  r.vinTerminal = 0;
  return r;
}

static void addMonitor(OptaIoRequest &r, uint8_t iface, int16_t primaryPin, int16_t pulsePin = -1) {
  OptaMonitorIo &m = r.monitors[r.monitorCount++];
  m.iface = iface;
  m.primaryPin = primaryPin;
  m.pulsePin = pulsePin;
}

static OptaIoPlan bootPlan() {
  OptaIoPlan p;
  optaIoPlanInit(p);
  return p;
}

static OptaIoPlan build(const OptaIoRequest &r, const OptaIoOptions &opt, const OptaIoPlan &prev) {
  OptaIoPlan out;
  optaBuildIoPlan(r, opt, prev, out);
  return out;
}

static bool samePlan(const OptaIoPlan &a, const OptaIoPlan &b) {
  for (uint8_t i = 0; i < OPTA_IO_MAX_MONITORS; ++i) {
    if (a.monitorTerminal[i] != b.monitorTerminal[i] || a.monitorErr[i] != b.monitorErr[i]) return false;
  }
  for (uint8_t t = 0; t < OPTA_INPUT_COUNT; ++t) {
    if (a.role[t] != b.role[t] || a.cls[t] != b.cls[t] || a.usedClass[t] != b.usedClass[t]) return false;
  }
  return a.buttonTerminal == b.buttonTerminal && a.buttonErr == b.buttonErr && a.warnings == b.warnings;
}

// True when no terminal has a role, a class or a first use.
static bool planIsEmpty(const OptaIoPlan &p) {
  for (uint8_t t = 0; t < OPTA_INPUT_COUNT; ++t) {
    if (p.role[t] != OPTA_ROLE_NONE || p.cls[t] != OPTA_CLASS_NONE || p.usedClass[t] != OPTA_CLASS_NONE) {
      return false;
    }
  }
  return true;
}

// Every pin-op call in the targeted tests goes through here: no op may target a pin outside
// 15-22 or a terminal that `next` reads as ADC, and no terminal gets two ops.
static uint8_t pinOps(const OptaIoPlan &prev, const OptaIoPlan &next, const OptaIoOptions &opt,
                      OptaPinOp *ops) {
  const uint8_t n = optaPlanPinOps(prev, next, opt, ops, OPTA_INPUT_COUNT);
  CHECK(n <= OPTA_INPUT_COUNT);
  bool seen[OPTA_INPUT_COUNT] = {false, false, false, false, false, false, false, false};
  for (uint8_t k = 0; k < n && k < OPTA_INPUT_COUNT; ++k) {
    const int16_t pin = ops[k].pin;
    CHECK(pin >= 15 && pin <= 22);
    if (pin < 15 || pin > 22) continue;
    const int t = pin - 15;
    CHECK(next.cls[t] != OPTA_CLASS_ADC);
    CHECK(!seen[t]);
    seen[t] = true;
    CHECK(ops[k].pull == OPTA_PULL_NONE || ops[k].pull == OPTA_PULL_UP);
  }
  return n;
}

static bool isOp(const OptaPinOp &op, int16_t pin, uint8_t pull) { return op.pin == pin && op.pull == pull; }

// ---------------------------------------------------------------------------------------------
// T1 tables
// ---------------------------------------------------------------------------------------------

// The core 4.6.0 numbers, pinned here as well as against the core in the firmware build.
static_assert(optaInputPin(0) == 15 && optaInputPin(1) == 16 && optaInputPin(2) == 17 &&
                  optaInputPin(3) == 18 && optaInputPin(4) == 19 && optaInputPin(5) == 20 &&
                  optaInputPin(6) == 21 && optaInputPin(7) == 22,
              "I1..I8 are pins 15..22");
static_assert(optaRelayCoilPin(0) == 0 && optaRelayCoilPin(1) == 1 && optaRelayCoilPin(2) == 2 &&
                  optaRelayCoilPin(3) == 3,
              "RELAY1..4 are D0..D3");
static_assert(optaRelayLedPin(0) == 7 && optaRelayLedPin(1) == 9 && optaRelayLedPin(2) == 8 &&
                  optaRelayLedPin(3) == 153,
              "LED_RELAY1..4 are 7, 9, 8, 153");
static_assert(OPTA_LED_USER_PIN == 25, "LED_USER is LEDB, 25");

static void testTables() {
  const int16_t inputs[8] = {15, 16, 17, 18, 19, 20, 21, 22};
  for (int32_t t = 0; t < 8; ++t) {
    CHECK(optaInputPin(t) == inputs[t]);
    CHECK(optaIsTerminal(t));
  }
  const int16_t coils[4] = {0, 1, 2, 3};
  const int16_t leds[4] = {7, 9, 8, 153};
  for (int32_t r = 0; r < 4; ++r) {
    CHECK(optaRelayCoilPin(r) == coils[r]);
    CHECK(optaRelayLedPin(r) == leds[r]);
  }
  CHECK(OPTA_LED_USER_PIN == 25);
  CHECK(OPTA_INPUT_COUNT == 8);
  CHECK(OPTA_RELAY_COUNT == 4);
  CHECK(OPTA_IO_MAX_MONITORS == 8);

  const int32_t badInputs[] = {-1, 8, 255, INT16_MIN, INT16_MAX, INT32_MIN, INT32_MAX};
  for (size_t k = 0; k < COUNT_OF(badInputs); ++k) {
    CHECK(optaInputPin(badInputs[k]) == -1);
    CHECK(!optaIsTerminal(badInputs[k]));
  }
  const int32_t badRelays[] = {-1, 4, 8, 255, INT16_MIN, INT16_MAX, INT32_MIN, INT32_MAX};
  for (size_t k = 0; k < COUNT_OF(badRelays); ++k) {
    CHECK(optaRelayCoilPin(badRelays[k]) == -1);
    CHECK(optaRelayLedPin(badRelays[k]) == -1);
    CHECK(optaLegacyRelayPin(badRelays[k]) == -1);
  }
}

// ---------------------------------------------------------------------------------------------
// T2 disjointness
// ---------------------------------------------------------------------------------------------

static void testDisjoint() {
  int16_t pins[17];
  size_t n = 0;
  for (int32_t t = 0; t < 8; ++t) pins[n++] = optaInputPin(t);
  for (int32_t r = 0; r < 4; ++r) pins[n++] = optaRelayCoilPin(r);
  for (int32_t r = 0; r < 4; ++r) pins[n++] = optaRelayLedPin(r);
  pins[n++] = OPTA_LED_USER_PIN;
  CHECK(n == COUNT_OF(pins));
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = i + 1; j < n; ++j) CHECK(pins[i] != pins[j]);
  }
  // SPI MISO, I2C, Serial1, LEDR/LEDG, A8-A11 (aliases of the coil ports PI_6/PI_5/PI_7/PI_4),
  // BTN_USER.
  const int16_t reserved[] = {10, 11, 12, 13, 14, 23, 24, 26, 27, 28, 29, 94};
  for (size_t i = 0; i < n; ++i) {
    for (size_t k = 0; k < COUNT_OF(reserved); ++k) CHECK(pins[i] != reserved[k]);
  }
}

// ---------------------------------------------------------------------------------------------
// T3 legacy regression (v2.2.16 getRelayPin = LED_D0 + r)
// ---------------------------------------------------------------------------------------------

static void testLegacyRelayPins() {
  const int16_t legacy[4] = {7, 8, 9, 10};
  for (int32_t r = 0; r < 4; ++r) {
    CHECK(optaLegacyRelayPin(r) == legacy[r]);
    for (int32_t c = 0; c < 4; ++c) CHECK(optaLegacyRelayPin(r) != optaRelayCoilPin(c));
  }
  // Relays 1/2/3 lit the LEDs of R1/R3/R2 and closed nothing (H-11/H-12); relay 4 drove pin 10,
  // which is neither an LED nor a coil.
  CHECK(optaLegacyRelayPin(0) == optaRelayLedPin(0));
  CHECK(optaLegacyRelayPin(1) == optaRelayLedPin(2));
  CHECK(optaLegacyRelayPin(2) == optaRelayLedPin(1));
  for (int32_t l = 0; l < 4; ++l) CHECK(optaLegacyRelayPin(3) != optaRelayLedPin(l));
}

// ---------------------------------------------------------------------------------------------
// T4 terminal resolution (exhaustive over int16_t)
// ---------------------------------------------------------------------------------------------

static void testTerminalResolution() {
  Tally isTerm = {0, 0}, pin = {0, 0}, alias = {0, 0};
  Tally digital = {0, 0}, pulse = {0, 0}, pulsePrimary = {0, 0};
  const OptaIoPlan boot = bootPlan();
  for (int32_t v = INT16_MIN; v <= INT16_MAX; ++v) {
    const bool expect = v >= 0 && v <= 7;
    tally(isTerm, optaIsTerminal(v) == expect, v);
    const int16_t p = optaInputPin(v);
    tally(pin, p == (expect ? 15 + v : -1), v);
    tally(alias, p < 26 || p > 29, v);

    OptaIoRequest r = emptyRequest();
    addMonitor(r, OPTA_IFACE_DIGITAL, (int16_t)v);
    OptaIoPlan d = build(r, kGpio, boot);
    tally(digital, expect ? (d.monitorErr[0] == OPTA_IOE_OK && d.monitorTerminal[0] == v &&
                             d.role[v] == OPTA_ROLE_CONTACT)
                          : (d.monitorErr[0] == OPTA_IOE_UNASSIGNED && d.monitorTerminal[0] == -1 &&
                             planIsEmpty(d)),
          v);

    r = emptyRequest();
    addMonitor(r, OPTA_IFACE_PULSE, -1, (int16_t)v);
    d = build(r, kGpio, boot);
    tally(pulse, expect ? (d.monitorErr[0] == OPTA_IOE_OK && d.monitorTerminal[0] == v &&
                           d.role[v] == OPTA_ROLE_PULSE)
                        : (d.monitorErr[0] == OPTA_IOE_UNASSIGNED && d.monitorTerminal[0] == -1 &&
                           planIsEmpty(d)),
          v);

    r = emptyRequest();
    addMonitor(r, OPTA_IFACE_PULSE, (int16_t)v, -1);
    d = build(r, kGpio, boot);
    tally(pulsePrimary, expect ? (d.monitorErr[0] == OPTA_IOE_OK && d.monitorTerminal[0] == v)
                               : (d.monitorErr[0] == OPTA_IOE_UNASSIGNED && planIsEmpty(d)),
          v);
  }
  CHECK_TALLY("optaIsTerminal accepts exactly 0..7", isTerm);
  CHECK_TALLY("optaInputPin maps exactly 0..7 to 15..22", pin);
  CHECK_TALLY("optaInputPin never yields A8-A11 (26-29)", alias);
  CHECK_TALLY("digital primaryPin resolves exactly 0..7", digital);
  CHECK_TALLY("pulse pulsePin resolves exactly 0..7", pulse);
  CHECK_TALLY("pulse primaryPin fallback resolves exactly 0..7", pulsePrimary);
}

// ---------------------------------------------------------------------------------------------
// T5 analog channel (exhaustive; CL:5695 is `(p >= 0 && p < 8) ? p : 0`)
// ---------------------------------------------------------------------------------------------

static void testAnalogChannel() {
  Tally channel = {0, 0}, planned = {0, 0};
  const OptaIoPlan boot = bootPlan();
  for (int32_t v = INT16_MIN; v <= INT16_MAX; ++v) {
    const int32_t legacy = (v >= 0 && v < 8) ? v : 0;
    tally(channel, optaLegacyAnalogChannel(v) == legacy, v);

    OptaIoRequest r = emptyRequest();
    addMonitor(r, OPTA_IFACE_ANALOG, (int16_t)v);
    const OptaIoPlan p = build(r, kAdc, boot);
    const uint8_t clamped = (v >= 0 && v < 8) ? 0 : OPTA_IOW_ANALOG_CLAMPED;
    tally(planned, p.monitorErr[0] == OPTA_IOE_OK && p.monitorTerminal[0] == legacy &&
                       p.role[legacy] == OPTA_ROLE_ANALOG && p.cls[legacy] == OPTA_CLASS_ADC &&
                       p.warnings == clamped,
          v);
  }
  CHECK_TALLY("optaLegacyAnalogChannel == CL:5695", channel);
  CHECK_TALLY("analog monitor plans the CL:5695 channel", planned);
}

// ---------------------------------------------------------------------------------------------
// T6 field property: the analog fleet is untouched
// ---------------------------------------------------------------------------------------------

static const int16_t kFieldPins[] = {-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 99, 255, INT16_MIN, INT16_MAX};
static const uint8_t kFieldIfaces[] = {OPTA_IFACE_ANALOG, OPTA_IFACE_CURRENT_LOOP};
struct VinCase {
  bool enabled;
  int16_t terminal;
};
static const VinCase kVinCases[] = {{false, 3}, {true, 0}, {true, 1}, {true, 2}, {true, 3},
                                    {true, 4},  {true, 5}, {true, 6}, {true, 7}, {true, 8}};
static const size_t kFieldMonitorShapes = COUNT_OF(kFieldPins) * COUNT_OF(kFieldIfaces);

static void setFieldMonitor(OptaIoRequest &r, uint8_t i, size_t shape) {
  r.monitors[i].iface = kFieldIfaces[shape % COUNT_OF(kFieldIfaces)];
  r.monitors[i].primaryPin = kFieldPins[shape / COUNT_OF(kFieldIfaces)];
  r.monitors[i].pulsePin = -1;
}

// Every analog/current-loop config, whatever its pins, Vin and history: no error, no GPIO
// terminal, no pinMode call, the v2.2.16 channel, and only the analog warnings it earns.
static bool fieldPlanOk(const OptaIoRequest &r, const OptaIoOptions &opt, const OptaIoPlan &prev,
                        OptaIoPlan &out) {
  out = build(r, opt, prev);
  bool ok = out.buttonErr == OPTA_IOE_OK && out.buttonTerminal == -1;
  uint8_t expectWarn = 0;
  int channelCount[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  for (uint8_t i = 0; i < OPTA_IO_MAX_MONITORS; ++i) ok = ok && out.monitorErr[i] == OPTA_IOE_OK;
  for (uint8_t i = 0; i < r.monitorCount; ++i) {
    const OptaMonitorIo &m = r.monitors[i];
    if (m.iface == OPTA_IFACE_ANALOG) {
      const int ch = (m.primaryPin >= 0 && m.primaryPin < 8) ? m.primaryPin : 0;
      if (ch != m.primaryPin) expectWarn |= OPTA_IOW_ANALOG_CLAMPED;
      ++channelCount[ch];
      ok = ok && out.monitorTerminal[i] == ch;
    } else {
      ok = ok && out.monitorTerminal[i] == -1;
    }
  }
  for (int t = 0; t < 8; ++t) {
    if (channelCount[t] >= 2) expectWarn |= OPTA_IOW_ANALOG_SHARED;
    if (channelCount[t] >= 1 && r.vinEnabled && r.vinTerminal == t) expectWarn |= OPTA_IOW_ANALOG_VIN_SHARED;
    ok = ok && out.cls[t] != OPTA_CLASS_GPIO && out.role[t] != OPTA_ROLE_CONTACT &&
         out.role[t] != OPTA_ROLE_PULSE && out.role[t] != OPTA_ROLE_BUTTON;
  }
  ok = ok && out.warnings == expectWarn;
  OptaPinOp ops[OPTA_INPUT_COUNT];
  const OptaIoPlan boot = bootPlan();
  ok = ok && optaPlanPinOps(boot, out, opt, ops, OPTA_INPUT_COUNT) == 0;
  ok = ok && optaPlanPinOps(out, out, opt, ops, OPTA_INPUT_COUNT) == 0;
  ok = ok && optaPlanPinOps(prev, out, opt, ops, OPTA_INPUT_COUNT) == 0;
  return ok;
}

static uint32_t lcgNext(uint32_t &state) {
  state = state * 1664525u + 1013904223u;
  return state >> 8;
}

static void testFieldProperty() {
  const OptaIoOptions optionSets[] = {kAdc, kGpio, OPTA_IO_DEFAULT_OPTIONS};
  for (size_t o = 0; o < COUNT_OF(optionSets); ++o) {
    const OptaIoOptions &opt = optionSets[o];
    Tally exhaustive = {0, 0}, sampled = {0, 0};
    OptaIoPlan prev = bootPlan();  // each config is pushed on top of the previous one
    OptaIoPlan out;
    long caseId = 0;
    for (size_t v = 0; v < COUNT_OF(kVinCases); ++v) {
      for (size_t a = 0; a < kFieldMonitorShapes; ++a) {
        OptaIoRequest r = emptyRequest();
        r.vinEnabled = kVinCases[v].enabled;
        r.vinTerminal = kVinCases[v].terminal;
        r.monitorCount = 1;
        setFieldMonitor(r, 0, a);
        tally(exhaustive, fieldPlanOk(r, opt, prev, out), caseId++);
        prev = out;
        for (size_t b = 0; b < kFieldMonitorShapes; ++b) {
          r.monitorCount = 2;
          setFieldMonitor(r, 1, b);
          tally(exhaustive, fieldPlanOk(r, opt, prev, out), caseId++);
          prev = out;
        }
      }
    }
    CHECK_TALLY("analog fleet, 1-2 monitors, exhaustive", exhaustive);

    uint32_t seed = 0x0C1A0001u;
    for (long n = 0; n < 200000; ++n) {
      OptaIoRequest r = emptyRequest();
      r.monitorCount = (uint8_t)(1 + lcgNext(seed) % 8);
      for (uint8_t i = 0; i < r.monitorCount; ++i) setFieldMonitor(r, i, lcgNext(seed) % kFieldMonitorShapes);
      const VinCase &vc = kVinCases[lcgNext(seed) % COUNT_OF(kVinCases)];
      r.vinEnabled = vc.enabled;
      r.vinTerminal = vc.terminal;
      tally(sampled, fieldPlanOk(r, opt, prev, out), n);
      prev = out;
    }
    CHECK_TALLY("analog fleet, 1-8 monitors, 200000 sampled", sampled);
  }
}

// ---------------------------------------------------------------------------------------------
// T7 conflicts (terminal 2 = I3 unless noted)
// ---------------------------------------------------------------------------------------------

static void testConflicts() {
  const OptaIoPlan boot = bootPlan();
  {  // contact/contact; a claim on another terminal is unaffected
    OptaIoRequest r = emptyRequest();
    addMonitor(r, OPTA_IFACE_DIGITAL, 2);
    addMonitor(r, OPTA_IFACE_DIGITAL, 2);
    addMonitor(r, OPTA_IFACE_DIGITAL, 5);
    const OptaIoPlan p = build(r, kAdc, boot);
    CHECK(p.monitorErr[0] == OPTA_IOE_CONFLICT && p.monitorErr[1] == OPTA_IOE_CONFLICT);
    CHECK(p.monitorTerminal[0] == -1 && p.monitorTerminal[1] == -1);
    CHECK(p.monitorErr[2] == OPTA_IOE_OK && p.monitorTerminal[2] == 5 && p.role[5] == OPTA_ROLE_CONTACT);
    CHECK(p.role[2] == OPTA_ROLE_NONE && p.cls[2] == OPTA_CLASS_NONE && p.usedClass[2] == OPTA_CLASS_NONE);
    CHECK(p.warnings == 0);
    // the class does not matter
    const OptaIoPlan g = build(r, kGpio, boot);
    CHECK(g.monitorErr[0] == OPTA_IOE_CONFLICT && g.monitorErr[1] == OPTA_IOE_CONFLICT);
    CHECK(g.role[2] == OPTA_ROLE_NONE && g.cls[2] == OPTA_CLASS_NONE);
  }
  {  // contact/pulse
    OptaIoRequest r = emptyRequest();
    addMonitor(r, OPTA_IFACE_DIGITAL, 2);
    addMonitor(r, OPTA_IFACE_PULSE, -1, 2);
    const OptaIoPlan p = build(r, kAdc, boot);
    CHECK(p.monitorErr[0] == OPTA_IOE_CONFLICT && p.monitorErr[1] == OPTA_IOE_CONFLICT);
    CHECK(p.role[2] == OPTA_ROLE_NONE && p.warnings == 0);
  }
  {  // pulse/analog: the analog monitor loses too
    OptaIoRequest r = emptyRequest();
    addMonitor(r, OPTA_IFACE_PULSE, -1, 2);
    addMonitor(r, OPTA_IFACE_ANALOG, 2);
    const OptaIoPlan p = build(r, kAdc, boot);
    CHECK(p.monitorErr[0] == OPTA_IOE_CONFLICT && p.monitorErr[1] == OPTA_IOE_CONFLICT);
    CHECK(p.monitorTerminal[1] == -1 && p.role[2] == OPTA_ROLE_NONE && p.warnings == 0);
  }
  {  // button/contact
    OptaIoRequest r = emptyRequest();
    r.buttonTerminal = 2;
    addMonitor(r, OPTA_IFACE_DIGITAL, 2);
    const OptaIoPlan p = build(r, kAdc, boot);
    CHECK(p.buttonErr == OPTA_IOE_CONFLICT && p.buttonTerminal == -1);
    CHECK(p.monitorErr[0] == OPTA_IOE_CONFLICT && p.role[2] == OPTA_ROLE_NONE);
  }
  {  // button/analog
    OptaIoRequest r = emptyRequest();
    r.buttonTerminal = 2;
    addMonitor(r, OPTA_IFACE_ANALOG, 2);
    const OptaIoPlan p = build(r, kAdc, boot);
    CHECK(p.buttonErr == OPTA_IOE_CONFLICT && p.buttonTerminal == -1);
    CHECK(p.monitorErr[0] == OPTA_IOE_CONFLICT && p.role[2] == OPTA_ROLE_NONE);
  }
  {  // three claims
    OptaIoRequest r = emptyRequest();
    addMonitor(r, OPTA_IFACE_DIGITAL, 2);
    addMonitor(r, OPTA_IFACE_ANALOG, 2);
    addMonitor(r, OPTA_IFACE_PULSE, 2, 2);
    const OptaIoPlan p = build(r, kAdc, boot);
    CHECK(p.monitorErr[0] == OPTA_IOE_CONFLICT && p.monitorErr[1] == OPTA_IOE_CONFLICT &&
          p.monitorErr[2] == OPTA_IOE_CONFLICT);
    CHECK(p.role[2] == OPTA_ROLE_NONE && p.warnings == 0);
    r = emptyRequest();
    r.buttonTerminal = 2;
    addMonitor(r, OPTA_IFACE_ANALOG, 2);
    addMonitor(r, OPTA_IFACE_ANALOG, 2);
    const OptaIoPlan q = build(r, kAdc, boot);
    CHECK(q.buttonErr == OPTA_IOE_CONFLICT && q.monitorErr[0] == OPTA_IOE_CONFLICT &&
          q.monitorErr[1] == OPTA_IOE_CONFLICT);
    CHECK(q.role[2] == OPTA_ROLE_NONE && q.warnings == 0);
  }
  {  // Vin + contact: the contact loses, Vin keeps the terminal with a warning
    OptaIoRequest r = emptyRequest();
    r.vinEnabled = true;
    r.vinTerminal = 2;
    addMonitor(r, OPTA_IFACE_DIGITAL, 2);
    const OptaIoPlan p = build(r, kGpio, boot);
    CHECK(p.monitorErr[0] == OPTA_IOE_CONFLICT && p.monitorTerminal[0] == -1);
    CHECK(p.role[2] == OPTA_ROLE_VIN && p.cls[2] == OPTA_CLASS_ADC && p.usedClass[2] == OPTA_CLASS_ADC);
    CHECK(p.warnings == OPTA_IOW_VIN_CONFLICT);
    addMonitor(r, OPTA_IFACE_ANALOG, 2);  // Vin + contact + analog
    const OptaIoPlan q = build(r, kGpio, boot);
    CHECK(q.monitorErr[0] == OPTA_IOE_CONFLICT && q.monitorErr[1] == OPTA_IOE_CONFLICT);
    CHECK(q.role[2] == OPTA_ROLE_VIN && q.warnings == OPTA_IOW_VIN_CONFLICT);
  }
  {  // analog/analog: warning only
    OptaIoRequest r = emptyRequest();
    addMonitor(r, OPTA_IFACE_ANALOG, 2);
    addMonitor(r, OPTA_IFACE_ANALOG, 2);
    const OptaIoPlan p = build(r, kAdc, boot);
    CHECK(p.monitorErr[0] == OPTA_IOE_OK && p.monitorErr[1] == OPTA_IOE_OK);
    CHECK(p.monitorTerminal[0] == 2 && p.monitorTerminal[1] == 2);
    CHECK(p.role[2] == OPTA_ROLE_ANALOG && p.cls[2] == OPTA_CLASS_ADC);
    CHECK(p.warnings == OPTA_IOW_ANALOG_SHARED);
    r.vinEnabled = true;  // analog/analog/Vin
    r.vinTerminal = 2;
    const OptaIoPlan q = build(r, kAdc, boot);
    CHECK(q.monitorErr[0] == OPTA_IOE_OK && q.monitorErr[1] == OPTA_IOE_OK && q.role[2] == OPTA_ROLE_ANALOG);
    CHECK(q.warnings == (OPTA_IOW_ANALOG_SHARED | OPTA_IOW_ANALOG_VIN_SHARED));
  }
  {  // analog/Vin: warning only
    OptaIoRequest r = emptyRequest();
    r.vinEnabled = true;
    r.vinTerminal = 2;
    addMonitor(r, OPTA_IFACE_ANALOG, 2);
    const OptaIoPlan p = build(r, kAdc, boot);
    CHECK(p.monitorErr[0] == OPTA_IOE_OK && p.monitorTerminal[0] == 2);
    CHECK(p.role[2] == OPTA_ROLE_ANALOG && p.warnings == OPTA_IOW_ANALOG_VIN_SHARED);
  }
  {  // Vin alone
    OptaIoRequest r = emptyRequest();
    r.vinEnabled = true;
    r.vinTerminal = 2;
    const OptaIoPlan p = build(r, kAdc, boot);
    CHECK(p.role[2] == OPTA_ROLE_VIN && p.cls[2] == OPTA_CLASS_ADC && p.warnings == 0);
  }
  {  // current loop with primaryPin 0 (as the server sends) + a contact on I1: no conflict
    OptaIoRequest r = emptyRequest();
    addMonitor(r, OPTA_IFACE_CURRENT_LOOP, 0);
    addMonitor(r, OPTA_IFACE_DIGITAL, 0);
    const OptaIoPlan p = build(r, kAdc, boot);
    CHECK(p.monitorErr[0] == OPTA_IOE_OK && p.monitorTerminal[0] == -1);
    CHECK(p.monitorErr[1] == OPTA_IOE_OK && p.monitorTerminal[1] == 0 && p.role[0] == OPTA_ROLE_CONTACT);
    CHECK(p.warnings == 0);
  }
}

// ---------------------------------------------------------------------------------------------
// T8 shapes the server's config generator produces
// ---------------------------------------------------------------------------------------------

static void testGeneratorShapes() {
  const OptaIoPlan boot = bootPlan();
  {  // rpm with primaryPin == pulsePin: one claim
    OptaIoRequest r = emptyRequest();
    addMonitor(r, OPTA_IFACE_PULSE, 3, 3);
    const OptaIoPlan p = build(r, kAdc, boot);
    CHECK(p.monitorErr[0] == OPTA_IOE_OK && p.monitorTerminal[0] == 3);
    CHECK(p.role[3] == OPTA_ROLE_PULSE && p.cls[3] == OPTA_CLASS_GPIO && p.warnings == 0);
  }
  {  // rpm with primaryPin 3, pulsePin 5: only 5 is claimed, so a contact on 3 is fine
    OptaIoRequest r = emptyRequest();
    addMonitor(r, OPTA_IFACE_PULSE, 3, 5);
    addMonitor(r, OPTA_IFACE_DIGITAL, 3);
    const OptaIoPlan p = build(r, kAdc, boot);
    CHECK(p.monitorErr[0] == OPTA_IOE_OK && p.monitorTerminal[0] == 5 && p.role[5] == OPTA_ROLE_PULSE);
    CHECK(p.monitorErr[1] == OPTA_IOE_OK && p.monitorTerminal[1] == 3 && p.role[3] == OPTA_ROLE_CONTACT);
  }
  {  // rpm with no pulsePin, or an invalid one, uses primaryPin
    OptaIoRequest r = emptyRequest();
    addMonitor(r, OPTA_IFACE_PULSE, 2, -1);
    addMonitor(r, OPTA_IFACE_PULSE, 4, 99);
    const OptaIoPlan p = build(r, kAdc, boot);
    CHECK(p.monitorErr[0] == OPTA_IOE_OK && p.monitorTerminal[0] == 2 && p.role[2] == OPTA_ROLE_PULSE);
    CHECK(p.monitorErr[1] == OPTA_IOE_OK && p.monitorTerminal[1] == 4 && p.role[4] == OPTA_ROLE_PULSE);
  }
  {  // no usable terminal: UNASSIGNED, no 2+idx fallback, nothing claimed
    const int16_t bad[] = {-1, 8, 26};
    for (size_t k = 0; k < COUNT_OF(bad); ++k) {
      OptaIoRequest r = emptyRequest();
      addMonitor(r, OPTA_IFACE_DIGITAL, bad[k]);
      addMonitor(r, OPTA_IFACE_PULSE, bad[k], bad[k]);
      const OptaIoPlan p = build(r, kAdc, boot);
      CHECK(p.monitorErr[0] == OPTA_IOE_UNASSIGNED && p.monitorTerminal[0] == -1);
      CHECK(p.monitorErr[1] == OPTA_IOE_UNASSIGNED && p.monitorTerminal[1] == -1);
      CHECK(planIsEmpty(p) && p.warnings == 0);
    }
  }
  {  // monitorCount 9 or 255: only the 8 entries are processed
    const uint8_t counts[] = {9, 255};
    for (size_t k = 0; k < COUNT_OF(counts); ++k) {
      OptaIoRequest r = emptyRequest();
      for (int16_t t = 0; t < 8; ++t) addMonitor(r, OPTA_IFACE_DIGITAL, t);
      r.monitorCount = counts[k];
      const OptaIoPlan p = build(r, kAdc, boot);
      for (uint8_t i = 0; i < 8; ++i) {
        CHECK(p.monitorErr[i] == OPTA_IOE_OK && p.monitorTerminal[i] == i && p.role[i] == OPTA_ROLE_CONTACT);
      }
    }
  }
  {  // an unknown iface claims nothing
    OptaIoRequest r = emptyRequest();
    addMonitor(r, 7, 2, 2);
    const OptaIoPlan p = build(r, kAdc, boot);
    CHECK(p.monitorErr[0] == OPTA_IOE_OK && p.monitorTerminal[0] == -1 && planIsEmpty(p));
  }
  {  // button: -1 is off; anything but 0-7 is UNASSIGNED
    OptaIoRequest r = emptyRequest();
    OptaIoPlan p = build(r, kAdc, boot);
    CHECK(p.buttonErr == OPTA_IOE_OK && p.buttonTerminal == -1 && planIsEmpty(p));
    r.buttonTerminal = 7;
    p = build(r, kAdc, boot);
    CHECK(p.buttonErr == OPTA_IOE_OK && p.buttonTerminal == 7 && p.role[7] == OPTA_ROLE_BUTTON);
    CHECK(p.cls[7] == OPTA_CLASS_ADC);
    p = build(r, kGpio, boot);
    CHECK(p.cls[7] == OPTA_CLASS_GPIO);
    const int16_t bad[] = {8, -2, 26, INT16_MIN, INT16_MAX};
    for (size_t k = 0; k < COUNT_OF(bad); ++k) {
      r.buttonTerminal = bad[k];
      p = build(r, kAdc, boot);
      CHECK(p.buttonErr == OPTA_IOE_UNASSIGNED && p.buttonTerminal == -1 && planIsEmpty(p));
    }
  }
  {  // Vin disabled or outside 0-7 claims nothing
    OptaIoRequest r = emptyRequest();
    addMonitor(r, OPTA_IFACE_DIGITAL, 2);
    r.vinEnabled = false;
    r.vinTerminal = 2;
    OptaIoPlan p = build(r, kAdc, boot);
    CHECK(p.monitorErr[0] == OPTA_IOE_OK && p.warnings == 0);
    r.vinEnabled = true;
    r.vinTerminal = 8;
    p = build(r, kAdc, boot);
    CHECK(p.monitorErr[0] == OPTA_IOE_OK && p.warnings == 0 && p.role[2] == OPTA_ROLE_CONTACT);
  }
}

// ---------------------------------------------------------------------------------------------
// T9 transitions: pin ops and RESTART
// ---------------------------------------------------------------------------------------------

static void testTransitions() {
  const OptaIoPlan boot = bootPlan();
  OptaPinOp ops[OPTA_INPUT_COUNT];
  OptaIoRequest contact = emptyRequest();
  addMonitor(contact, OPTA_IFACE_DIGITAL, 2);
  OptaIoRequest pulse = emptyRequest();
  addMonitor(pulse, OPTA_IFACE_PULSE, -1, 2);

  // Contact read by analogRead, then a pulse on the same terminal: RESTART, no op.
  const OptaIoPlan p1 = build(contact, kAdc, boot);
  CHECK(p1.monitorErr[0] == OPTA_IOE_OK && p1.role[2] == OPTA_ROLE_CONTACT);
  CHECK(p1.cls[2] == OPTA_CLASS_ADC && p1.usedClass[2] == OPTA_CLASS_ADC);
  CHECK(pinOps(boot, p1, kAdc, ops) == 0);
  const OptaIoPlan p2 = build(pulse, kAdc, p1);
  CHECK(p2.monitorErr[0] == OPTA_IOE_RESTART && p2.monitorTerminal[0] == -1);
  CHECK(p2.role[2] == OPTA_ROLE_NONE && p2.cls[2] == OPTA_CLASS_NONE && p2.usedClass[2] == OPTA_CLASS_ADC);
  CHECK(pinOps(p1, p2, kAdc, ops) == 0);

  // Pulse, then a contact (ADC) on the same terminal: RESTART and the pull-up is released.
  const OptaIoPlan q1 = build(pulse, kAdc, boot);
  CHECK(q1.monitorErr[0] == OPTA_IOE_OK && q1.monitorTerminal[0] == 2 && q1.role[2] == OPTA_ROLE_PULSE);
  CHECK(q1.cls[2] == OPTA_CLASS_GPIO && q1.usedClass[2] == OPTA_CLASS_GPIO);
  CHECK(pinOps(boot, q1, kAdc, ops) == 1 && isOp(ops[0], 17, OPTA_PULL_UP));
  const OptaIoPlan q2 = build(contact, kAdc, q1);
  CHECK(q2.monitorErr[0] == OPTA_IOE_RESTART && q2.monitorTerminal[0] == -1);
  CHECK(q2.role[2] == OPTA_ROLE_NONE && q2.cls[2] == OPTA_CLASS_NONE);
  CHECK(pinOps(q1, q2, kAdc, ops) == 1 && isOp(ops[0], 17, OPTA_PULL_NONE));

  // Pulse removed: released, and the terminal stays GPIO-used.
  const OptaIoPlan q3 = build(emptyRequest(), kAdc, q1);
  CHECK(q3.role[2] == OPTA_ROLE_NONE && q3.cls[2] == OPTA_CLASS_NONE && q3.usedClass[2] == OPTA_CLASS_GPIO);
  CHECK(pinOps(q1, q3, kAdc, ops) == 1 && isOp(ops[0], 17, OPTA_PULL_NONE));

  // Pulse moved I3 -> I4: release I3, pull-up on I4.
  OptaIoRequest pulse4 = emptyRequest();
  addMonitor(pulse4, OPTA_IFACE_PULSE, -1, 3);
  const OptaIoPlan q4 = build(pulse4, kAdc, q1);
  CHECK(q4.monitorErr[0] == OPTA_IOE_OK && q4.monitorTerminal[0] == 3 && q4.cls[3] == OPTA_CLASS_GPIO);
  CHECK(pinOps(q1, q4, kAdc, ops) == 2 && isOp(ops[0], 17, OPTA_PULL_NONE) && isOp(ops[1], 18, OPTA_PULL_UP));

  // The same config applied twice: the same plan and no op.
  const OptaIoPlan q1again = build(pulse, kAdc, q1);
  CHECK(samePlan(q1, q1again));
  CHECK(pinOps(q1, q1again, kAdc, ops) == 0);

  // RESTART persists over an unrelated config and clears only with optaIoPlanInit (a reboot).
  OptaIoRequest other = emptyRequest();
  addMonitor(other, OPTA_IFACE_DIGITAL, 5);
  const OptaIoPlan p3 = build(other, kAdc, p2);
  CHECK(p3.monitorErr[0] == OPTA_IOE_OK && p3.usedClass[2] == OPTA_CLASS_ADC);
  const OptaIoPlan p4 = build(pulse, kAdc, p3);
  CHECK(p4.monitorErr[0] == OPTA_IOE_RESTART && p4.cls[2] == OPTA_CLASS_NONE);
  CHECK(pinOps(p3, p4, kAdc, ops) == 0);
  const OptaIoPlan p5 = build(pulse, kAdc, boot);
  CHECK(p5.monitorErr[0] == OPTA_IOE_OK && p5.cls[2] == OPTA_CLASS_GPIO);

  // Contacts read by digitalRead: contact -> pulse keeps the class and changes only the pull.
  const OptaIoPlan g1 = build(contact, kGpio, boot);
  CHECK(g1.monitorErr[0] == OPTA_IOE_OK && g1.role[2] == OPTA_ROLE_CONTACT && g1.cls[2] == OPTA_CLASS_GPIO);
  CHECK(pinOps(boot, g1, kGpio, ops) == 1 && isOp(ops[0], 17, OPTA_PULL_NONE));
  const OptaIoPlan g2 = build(pulse, kGpio, g1);
  CHECK(g2.monitorErr[0] == OPTA_IOE_OK && g2.role[2] == OPTA_ROLE_PULSE && g2.cls[2] == OPTA_CLASS_GPIO);
  CHECK(pinOps(g1, g2, kGpio, ops) == 1 && isOp(ops[0], 17, OPTA_PULL_UP));
  const OptaIoPlan g3 = build(contact, kGpio, g2);
  CHECK(g3.monitorErr[0] == OPTA_IOE_OK && g3.role[2] == OPTA_ROLE_CONTACT);
  CHECK(pinOps(g2, g3, kGpio, ops) == 1 && isOp(ops[0], 17, OPTA_PULL_NONE));
  const OptaIoOptions gpioUp = { false, OPTA_PULL_UP, OPTA_PULL_UP };
  const OptaIoPlan u1 = build(contact, gpioUp, boot);
  const OptaIoPlan u2 = build(pulse, gpioUp, u1);
  CHECK(u2.monitorErr[0] == OPTA_IOE_OK && pinOps(u1, u2, gpioUp, ops) == 0);  // same pull

  // An analog monitor or Vin on a terminal used as GPIO this boot.
  OptaIoRequest analog = emptyRequest();
  addMonitor(analog, OPTA_IFACE_ANALOG, 2);
  const OptaIoPlan a1 = build(analog, kAdc, q1);
  CHECK(a1.monitorErr[0] == OPTA_IOE_RESTART && a1.monitorTerminal[0] == -1 && a1.role[2] == OPTA_ROLE_NONE);
  CHECK(pinOps(q1, a1, kAdc, ops) == 1 && isOp(ops[0], 17, OPTA_PULL_NONE));
  OptaIoRequest vin = emptyRequest();
  vin.vinEnabled = true;
  vin.vinTerminal = 2;
  const OptaIoPlan v1 = build(vin, kAdc, q1);
  CHECK(v1.warnings == OPTA_IOW_VIN_RESTART && v1.role[2] == OPTA_ROLE_NONE && v1.cls[2] == OPTA_CLASS_NONE);
  CHECK(pinOps(q1, v1, kAdc, ops) == 1 && isOp(ops[0], 17, OPTA_PULL_NONE));
  OptaIoRequest vinContact = vin;  // Vin wins a conflict on a GPIO-used terminal: still a restart
  addMonitor(vinContact, OPTA_IFACE_DIGITAL, 2);
  const OptaIoPlan v2 = build(vinContact, kAdc, q1);
  CHECK(v2.monitorErr[0] == OPTA_IOE_CONFLICT && v2.role[2] == OPTA_ROLE_NONE);
  CHECK(v2.warnings == (OPTA_IOW_VIN_CONFLICT | OPTA_IOW_VIN_RESTART));

  // Plans not built from each other: GPIO -> ADC still gets no op on the ADC terminal.
  const OptaIoPlan fresh = build(contact, kAdc, boot);
  CHECK(pinOps(q1, fresh, kAdc, ops) == 0);

  // Eight pulses need eight ops; maxOps stops early.
  OptaIoRequest eight = emptyRequest();
  for (int16_t t = 0; t < 8; ++t) addMonitor(eight, OPTA_IFACE_PULSE, -1, t);
  const OptaIoPlan e1 = build(eight, kAdc, boot);
  CHECK(pinOps(boot, e1, kAdc, ops) == 8);
  const int16_t pins[8] = {15, 16, 17, 18, 19, 20, 21, 22};
  for (uint8_t k = 0; k < 8; ++k) CHECK(isOp(ops[k], pins[k], OPTA_PULL_UP));
  CHECK(optaPlanPinOps(boot, e1, kAdc, ops, 3) == 3 && isOp(ops[2], 17, OPTA_PULL_UP));
  CHECK(optaPlanPinOps(boot, e1, kAdc, ops, 0) == 0);
  CHECK(pinOps(e1, boot, kAdc, ops) == 8 && isOp(ops[7], 22, OPTA_PULL_NONE));

  // out may be the same object as prev.
  OptaIoPlan same = q1;
  optaBuildIoPlan(contact, kAdc, same, same);
  CHECK(samePlan(same, q2));
}

// ---------------------------------------------------------------------------------------------
// T10 clear-button key resolver
// ---------------------------------------------------------------------------------------------

static void testButtonResolver() {
  uint8_t w = 0xEE;
  CHECK(optaResolveClearButton(true, 0, false, 0, w) == 0 && w == OPTA_BTNW_NONE);
  w = 0xEE;
  CHECK(optaResolveClearButton(true, 7, false, 0, w) == 7 && w == OPTA_BTNW_NONE);
  w = 0xEE;
  CHECK(optaResolveClearButton(true, -1, false, 0, w) == -1 && w == OPTA_BTNW_NONE);
  const int32_t invalid[] = {8, -2, 99, 15, INT32_MIN, INT32_MAX};
  for (size_t k = 0; k < COUNT_OF(invalid); ++k) {
    w = 0xEE;
    CHECK(optaResolveClearButton(true, invalid[k], false, 0, w) == -1 && w == OPTA_BTNW_INVALID);
    w = 0xEE;
    CHECK(optaResolveClearButton(true, invalid[k], true, 5, w) == -1 && w == OPTA_BTNW_INVALID);
  }
  // The new key wins outright; the legacy key is not looked at.
  w = 0xEE;
  CHECK(optaResolveClearButton(true, 3, true, 5, w) == 3 && w == OPTA_BTNW_NONE);
  w = 0xEE;
  CHECK(optaResolveClearButton(true, -1, true, 5, w) == -1 && w == OPTA_BTNW_NONE);
  // A legacy raw pin number is never honoured.
  const int32_t legacy[] = {0, 7, 15, 99};
  for (size_t k = 0; k < COUNT_OF(legacy); ++k) {
    w = 0xEE;
    CHECK(optaResolveClearButton(false, 0, true, legacy[k], w) == -1 && w == OPTA_BTNW_LEGACY_IGNORED);
  }
  w = 0xEE;
  CHECK(optaResolveClearButton(false, 0, true, -1, w) == -1 && w == OPTA_BTNW_NONE);
  w = 0xEE;
  CHECK(optaResolveClearButton(false, 4, false, 5, w) == -1 && w == OPTA_BTNW_NONE);  // both absent
}

// ---------------------------------------------------------------------------------------------
// T11 non-blocking button step
// ---------------------------------------------------------------------------------------------

struct ButtonSim {
  OptaButton b;
  uint32_t now;
  unsigned fires;
};

static void simStart(ButtonSim &s, uint32_t startMs) {
  optaButtonReset(s.b, startMs);
  s.now = startMs;
  s.fires = 0;
}

// Holds a level for `ms`, stepping every millisecond from now to now + ms inclusive. The next
// hold starts at now + ms, so `ms` is the time between two edges.
static void simHold(ButtonSim &s, bool pressed, uint32_t ms) {
  for (uint32_t k = 0; k <= ms; ++k) {
    if (optaButtonStep(s.b, pressed, s.now + k)) ++s.fires;
  }
  s.now += ms;
}

static const bool kPressed = true;
static const bool kReleased = false;

static void testButtonStep() {
  CHECK(OPTA_BUTTON_DEBOUNCE_MS == 50u);
  CHECK(OPTA_BUTTON_MIN_PRESS_MS == 500u);
  // Around 0x1000 ms and across the millis() wrap: identical results.
  const uint32_t starts[] = {0x00001000u, 0xFFFFFF00u};
  for (size_t k = 0; k < COUNT_OF(starts); ++k) {
    ButtonSim s;
    // Held (or stuck) from boot or a config push: never fires.
    simStart(s, starts[k]);
    simHold(s, kPressed, 10000);
    CHECK(s.fires == 0 && s.b.state == OPTA_BTN_WAIT_RELEASE);

    // A 49 ms release does not arm it.
    simStart(s, starts[k]);
    simHold(s, kReleased, 49);
    CHECK(s.b.state == OPTA_BTN_WAIT_RELEASE);
    simHold(s, kPressed, 600);
    CHECK(s.fires == 0);

    // 50 ms released arms it; the press fires at 500 ms, not 499.
    simStart(s, starts[k]);
    simHold(s, kReleased, 50);
    CHECK(s.b.state == OPTA_BTN_ARMED);
    simHold(s, kPressed, 499);
    CHECK(s.fires == 0 && s.b.state == OPTA_BTN_ARMED);
    simHold(s, kPressed, 1);
    CHECK(s.fires == 1 && s.b.state == OPTA_BTN_FIRED);

    // Held 10 s: one fire.
    simStart(s, starts[k]);
    simHold(s, kReleased, 50);
    simHold(s, kPressed, 10000);
    CHECK(s.fires == 1);

    // Chatter restarts the timer.
    simStart(s, starts[k]);
    simHold(s, kReleased, 50);
    simHold(s, kPressed, 300);
    simHold(s, kReleased, 10);
    simHold(s, kPressed, 300);
    CHECK(s.fires == 0 && s.b.state == OPTA_BTN_ARMED);

    // A new press after a 50 ms release fires again; after a 49 ms release it does not.
    simStart(s, starts[k]);
    simHold(s, kReleased, 50);
    simHold(s, kPressed, 500);
    simHold(s, kReleased, 50);
    simHold(s, kPressed, 500);
    CHECK(s.fires == 2);
    simStart(s, starts[k]);
    simHold(s, kReleased, 50);
    simHold(s, kPressed, 500);
    simHold(s, kReleased, 49);
    simHold(s, kPressed, 600);
    CHECK(s.fires == 1 && s.b.state == OPTA_BTN_FIRED);
  }
}

// ---------------------------------------------------------------------------------------------
// T12 indicators (D1)
// ---------------------------------------------------------------------------------------------

static void testIndicators() {
  Tally rule = {0, 0};
  for (unsigned mask = 0; mask < 16; ++mask) {
    for (int alarm = 0; alarm < 2; ++alarm) {
      for (int inhibit = 0; inhibit < 2; ++inhibit) {
        const uint8_t expect = (uint8_t)(inhibit ? 0u : (mask | (alarm ? 0x10u : 0u)));
        const long id = (long)(mask * 4 + alarm * 2 + inhibit);
        tally(rule, optaIndicatorMask((uint8_t)mask, alarm != 0, inhibit != 0) == expect, id);
        // Only bits 0-3 of the coil mask are relays.
        tally(rule, optaIndicatorMask((uint8_t)(mask | 0xF0u), alarm != 0, inhibit != 0) == expect, id);
      }
    }
  }
  CHECK_TALLY("optaIndicatorMask rule", rule);
  CHECK(OPTA_IND_USER_LED_BIT == 4);
  const int16_t pins[5] = {7, 9, 8, 153, 25};
  for (uint8_t b = 0; b < 5; ++b) CHECK(optaIndicatorPin(b) == pins[b]);
  CHECK(optaIndicatorPin(5) == -1);
  CHECK(optaIndicatorPin(255) == -1);
}

// ---------------------------------------------------------------------------------------------
// T13 ACK summary
// ---------------------------------------------------------------------------------------------

// Summary into a 64-byte array of which only `len` bytes may be written.
static bool summaryIs(const OptaIoPlan &p, const OptaIoRequest &r, size_t len, const char *expect) {
  char buf[64];
  memset(buf, 'X', sizeof(buf));
  const size_t n = optaIoSummary(p, r, buf, len);
  for (size_t i = len; i < sizeof(buf); ++i) {
    if (buf[i] != 'X') return false;
  }
  if (len == 0) return n == 0;
  return n < len && buf[n] == '\0' && n == strlen(buf) && strcmp(buf, expect) == 0;
}

static void testSummary() {
  const OptaIoPlan boot = bootPlan();
  {  // no errors
    OptaIoRequest r = emptyRequest();
    addMonitor(r, OPTA_IFACE_ANALOG, 2);
    addMonitor(r, OPTA_IFACE_ANALOG, 2);
    r.vinEnabled = true;
    r.vinTerminal = 2;
    const OptaIoPlan p = build(r, kAdc, boot);
    CHECK(summaryIs(p, r, 48, ""));
  }
  {  // the example from the spec, on a hand-made plan
    OptaIoRequest r = emptyRequest();
    addMonitor(r, OPTA_IFACE_ANALOG, 0);
    addMonitor(r, OPTA_IFACE_DIGITAL, 2);
    OptaIoPlan p = bootPlan();
    p.monitorErr[1] = OPTA_IOE_CONFLICT;
    p.buttonErr = OPTA_IOE_UNASSIGNED;
    CHECK(summaryIs(p, r, 48, "m2 dup I3; btn no input"));
  }
  {  // no input, two monitors on one terminal, button no input
    OptaIoRequest r = emptyRequest();
    addMonitor(r, OPTA_IFACE_DIGITAL, -1);
    const OptaIoPlan p = build(r, kAdc, boot);
    CHECK(summaryIs(p, r, 48, "m1 no input"));
    r = emptyRequest();
    addMonitor(r, OPTA_IFACE_DIGITAL, 2);
    addMonitor(r, OPTA_IFACE_DIGITAL, 2);
    r.buttonTerminal = 9;
    const OptaIoPlan q = build(r, kAdc, boot);
    CHECK(summaryIs(q, r, 48, "m1 dup I3; m2 dup I3; btn no input"));
  }
  {  // button dup, pulse dup (named by its pulse terminal), vin dup, m8
    OptaIoRequest r = emptyRequest();
    addMonitor(r, OPTA_IFACE_CURRENT_LOOP, 0);
    addMonitor(r, OPTA_IFACE_PULSE, 6, 2);
    r.buttonTerminal = 2;
    const OptaIoPlan p = build(r, kAdc, boot);
    CHECK(summaryIs(p, r, 48, "m2 dup I3; btn dup I3"));
    r = emptyRequest();
    r.vinEnabled = true;
    r.vinTerminal = 4;
    addMonitor(r, OPTA_IFACE_DIGITAL, 4);
    const OptaIoPlan q = build(r, kAdc, boot);
    CHECK(summaryIs(q, r, 48, "m1 dup I5; vin dup I5"));
    r = emptyRequest();
    for (int16_t t = 0; t < 7; ++t) addMonitor(r, OPTA_IFACE_ANALOG, t);
    addMonitor(r, OPTA_IFACE_DIGITAL, 26);
    const OptaIoPlan m8 = build(r, kAdc, boot);
    CHECK(summaryIs(m8, r, 48, "m8 no input"));
  }
  {  // reboot tokens; a Vin restart is only a warning bit
    OptaIoRequest pulse = emptyRequest();
    addMonitor(pulse, OPTA_IFACE_PULSE, -1, 7);
    const OptaIoPlan used = build(pulse, kAdc, boot);
    OptaIoRequest r = emptyRequest();
    addMonitor(r, OPTA_IFACE_DIGITAL, 7);
    const OptaIoPlan p = build(r, kAdc, used);
    CHECK(summaryIs(p, r, 48, "m1 reboot I8"));
    r = emptyRequest();
    r.buttonTerminal = 7;
    const OptaIoPlan q = build(r, kAdc, used);
    CHECK(summaryIs(q, r, 48, "btn reboot I8"));
    r = emptyRequest();
    r.vinEnabled = true;
    r.vinTerminal = 7;
    const OptaIoPlan v = build(r, kAdc, used);
    CHECK(v.warnings == OPTA_IOW_VIN_RESTART && summaryIs(v, r, 48, ""));
  }
  {  // overflow: whole tokens only, then "+"
    OptaIoRequest r = emptyRequest();
    for (int t = 0; t < 8; ++t) addMonitor(r, OPTA_IFACE_DIGITAL, -1);
    const OptaIoPlan p = build(r, kAdc, boot);
    CHECK(summaryIs(p, r, 48, "m1 no input; m2 no input; m3 no input+"));
    CHECK(summaryIs(p, r, 64, "m1 no input; m2 no input; m3 no input; m4 no input+"));
    CHECK(summaryIs(p, r, 12, "+"));
    CHECK(summaryIs(p, r, 2, "+"));
    CHECK(summaryIs(p, r, 1, ""));
    CHECK(summaryIs(p, r, 0, ""));

    OptaIoRequest two = emptyRequest();
    addMonitor(two, OPTA_IFACE_DIGITAL, -1);
    addMonitor(two, OPTA_IFACE_DIGITAL, -1);
    const OptaIoPlan q = build(two, kAdc, boot);
    CHECK(summaryIs(q, two, 25, "m1 no input; m2 no input"));  // exact fit, no "+"
    CHECK(summaryIs(q, two, 24, "m1 no input+"));

    OptaIoRequest one = emptyRequest();
    addMonitor(one, OPTA_IFACE_DIGITAL, -1);
    const OptaIoPlan o = build(one, kAdc, boot);
    CHECK(summaryIs(o, one, 12, "m1 no input"));
    CHECK(summaryIs(o, one, 11, "+"));
  }
}

int main() {
  testTables();
  testDisjoint();
  testLegacyRelayPins();
  testTerminalResolution();
  testAnalogChannel();
  testFieldProperty();
  testConflicts();
  testGeneratorShapes();
  testTransitions();
  testButtonResolver();
  testButtonStep();
  testIndicators();
  testSummary();
  printf("opta_io: %lu checks, %lu failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
