// TankAlarm_OptaIo.h - Opta I/O tables and pure planning helpers for relays and floats (CL-1).
//
// The pin numbers of the Opta's I1-I8 inputs, R1-R4 relay coils, relay LEDs and LED_USER; the
// input plan (which monitor, clear button or Vin reading owns each terminal, and the pinMode
// calls a config change needs); the clear-button key resolver and a non-blocking button step;
// the indicator LED mask. Nothing here touches hardware: callers do the pinMode/digitalWrite.
// Host-tested in tests/host/opta_io; under ARDUINO_OPTA the tables are also checked against the
// core at compile time.
//
// Terminal numbers in configs are 0-7 = I1-I8 and relay numbers 0-3 = R1-R4. Every text shown
// to people (ACK messages, serial output, web pages) is 1-based.

#ifndef TANKALARM_OPTA_IO_H
#define TANKALARM_OPTA_IO_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------
// Pin tables (Arduino Mbed OS Opta core 4.6.0, variants/OPTA/pins_arduino.h)
// ---------------------------------------------------------------------------------------------

static constexpr uint8_t OPTA_INPUT_COUNT = 8;      // I1..I8
static constexpr uint8_t OPTA_RELAY_COUNT = 4;      // R1..R4
static constexpr uint8_t OPTA_IO_MAX_MONITORS = 8;  // == client MAX_MONITORS
static constexpr int16_t OPTA_LED_USER_PIN = 25;    // LED_USER (LEDB)
// Relay LEDs and LED_USER are lit by HIGH. Placeholder until bench A2/L1 confirm it (D6).
static constexpr uint8_t OPTA_LED_ON_LEVEL = 1;

// Terminal t (0-7 = I1-I8) -> PIN_A0..PIN_A7, or -1.
static constexpr int16_t optaInputPin(int32_t t) {
  return (t >= 0 && t < 8) ? (int16_t)(15 + t) : (int16_t)-1;
}
// Relay r (0-3 = R1-R4) -> RELAY1..RELAY4 (D0..D3), or -1.
static constexpr int16_t optaRelayCoilPin(int32_t r) {
  return (r >= 0 && r < 4) ? (int16_t)r : (int16_t)-1;
}
// Relay r -> LED_RELAY1..LED_RELAY4. Not in order: LED_D1 is 9 and LED_D2 is 8.
static constexpr int16_t optaRelayLedPin(int32_t r) {
  return r == 0 ? (int16_t)7 : r == 1 ? (int16_t)9 : r == 2 ? (int16_t)8 : r == 3 ? (int16_t)153 : (int16_t)-1;
}
static constexpr bool optaIsTerminal(int32_t t) { return t >= 0 && t < 8; }
// Same expression as the client's readAnalogSensor: an out-of-range channel reads I1.
static constexpr int16_t optaLegacyAnalogChannel(int32_t p) {
  return (p >= 0 && p < 8) ? (int16_t)p : (int16_t)0;
}
// v2.2.16 getRelayPin (LED_D0 + r = 7, 8, 9, 10), which drove LEDs instead of coils (H-11/H-12).
// For the bench sketch's legacy command and regression tests ONLY; firmware must never use it.
static constexpr int16_t optaLegacyRelayPin(int32_t r) {
  return (r >= 0 && r < 4) ? (int16_t)(7 + r) : (int16_t)-1;
}

#if defined(ARDUINO_OPTA)
#include <Arduino.h>
// CI installs an unpinned core: a renumbering must break the build, not the wiring.
static_assert(optaRelayCoilPin(0) == (int32_t)RELAY1 && optaRelayCoilPin(1) == (int32_t)RELAY2 &&
                  optaRelayCoilPin(2) == (int32_t)RELAY3 && optaRelayCoilPin(3) == (int32_t)RELAY4,
              "Opta core moved RELAY1..4: fix optaRelayCoilPin before building");
static_assert(optaRelayLedPin(0) == (int32_t)LED_RELAY1 && optaRelayLedPin(1) == (int32_t)LED_RELAY2 &&
                  optaRelayLedPin(2) == (int32_t)LED_RELAY3 && optaRelayLedPin(3) == (int32_t)LED_RELAY4,
              "Opta core moved LED_RELAY1..4: fix optaRelayLedPin before building");
static_assert(OPTA_LED_USER_PIN == (int32_t)LED_USER,
              "Opta core moved LED_USER: fix OPTA_LED_USER_PIN before building");
static_assert(optaInputPin(0) == (int32_t)PIN_A0 && optaInputPin(1) == (int32_t)PIN_A1 &&
                  optaInputPin(2) == (int32_t)PIN_A2 && optaInputPin(3) == (int32_t)PIN_A3 &&
                  optaInputPin(4) == (int32_t)PIN_A4 && optaInputPin(5) == (int32_t)PIN_A5 &&
                  optaInputPin(6) == (int32_t)PIN_A6 && optaInputPin(7) == (int32_t)PIN_A7,
              "Opta core moved PIN_A0..A7: fix optaInputPin before building");
static_assert(optaInputPin(0) == (int32_t)I1 && optaInputPin(1) == (int32_t)I2 &&
                  optaInputPin(2) == (int32_t)I3 && optaInputPin(3) == (int32_t)I4 &&
                  optaInputPin(4) == (int32_t)I5 && optaInputPin(5) == (int32_t)I6 &&
                  optaInputPin(6) == (int32_t)I7 && optaInputPin(7) == (int32_t)I8,
              "Opta core moved I1..I8: fix optaInputPin before building");
static_assert(OPTA_INPUT_COUNT == NUM_ANALOG_INPUTS,
              "Opta core changed NUM_ANALOG_INPUTS: review the I1..I8 tables");
#endif

// ---------------------------------------------------------------------------------------------
// I/O plan: which claim owns each terminal, and in which class (ADC or GPIO) it is read.
//
// Rules of optaBuildIoPlan (each one is a case in tests/host/opta_io):
// 1. The plan starts empty with prev's usedClass. `out` may be the same object as `prev`.
// 2. Claims. Monitors past monitorCount (or 8) are ignored.
//    - analog: optaLegacyAnalogChannel(primaryPin), role ANALOG; outside 0-7 it reads I1 as
//      v2.2.16 did, with warning ANALOG_CLAMPED. Never unassigned.
//    - current loop (A0602 expansion): no terminal.
//    - digital: primaryPin as CONTACT when 0-7, else UNASSIGNED (no 2+idx fallback).
//    - pulse: pulsePin if 0-7, else primaryPin if 0-7, else UNASSIGNED. One claim only.
//    - any other iface: no terminal.
//    - button: buttonTerminal 0-7 as BUTTON; -1 is off; any other value is UNASSIGNED.
//    - Vin: vinTerminal as VIN when enabled and 0-7.
// 3. Per terminal. CONTACT, PULSE and BUTTON are exclusive: with any other claim on the
//    terminal, every non-Vin claim gets CONFLICT (no winner among them). Vin has no fault path,
//    so it keeps the terminal with warning VIN_CONFLICT. Analog and Vin claims may share, with
//    warnings ANALOG_SHARED / ANALOG_VIN_SHARED.
// 4. Class. ANALOG and VIN are ADC; CONTACT and BUTTON are ADC when contactsUseAdc, else GPIO;
//    PULSE is GPIO. A GPIO terminal also records its pull (pulsePull or contactPull).
// 5. RESTART. The core caches an AnalogIn per pin on the first analogRead and pinMode builds a
//    new DigitalInOut; neither hands the pin back to the other, so a terminal is only used in
//    the class it first had since boot. A winner of the other class gets RESTART (Vin: warning
//    VIN_RESTART) and the terminal stays unused until optaIoPlanInit, i.e. a reboot.
// 6. monitorTerminal and buttonTerminal are -1 for any claim in error.
// ---------------------------------------------------------------------------------------------

// Same values as the client's SensorInterface.
enum : uint8_t { OPTA_IFACE_DIGITAL = 0, OPTA_IFACE_ANALOG = 1, OPTA_IFACE_CURRENT_LOOP = 2, OPTA_IFACE_PULSE = 3 };
enum OptaIoRole : uint8_t { OPTA_ROLE_NONE = 0, OPTA_ROLE_ANALOG, OPTA_ROLE_VIN, OPTA_ROLE_CONTACT, OPTA_ROLE_PULSE, OPTA_ROLE_BUTTON };
enum OptaIoClass : uint8_t { OPTA_CLASS_NONE = 0, OPTA_CLASS_ADC, OPTA_CLASS_GPIO };
enum OptaIoErr : uint8_t { OPTA_IOE_OK = 0, OPTA_IOE_UNASSIGNED, OPTA_IOE_CONFLICT, OPTA_IOE_RESTART };
enum OptaPull : uint8_t { OPTA_PULL_NONE = 0, OPTA_PULL_UP };

// Warning bits (OptaIoPlan::warnings).
static constexpr uint8_t OPTA_IOW_ANALOG_SHARED = 0x01;      // two analog monitors on one terminal
static constexpr uint8_t OPTA_IOW_ANALOG_VIN_SHARED = 0x02;  // analog monitor and Vin on one terminal
static constexpr uint8_t OPTA_IOW_ANALOG_CLAMPED = 0x04;     // analog primaryPin outside 0-7, read as I1
static constexpr uint8_t OPTA_IOW_VIN_CONFLICT = 0x08;       // Vin shares a terminal with a contact/pulse/button
static constexpr uint8_t OPTA_IOW_VIN_RESTART = 0x10;        // Vin terminal was used as GPIO earlier this boot

struct OptaMonitorIo {
  uint8_t iface;       // OPTA_IFACE_*
  int16_t primaryPin;
  int16_t pulsePin;
};

struct OptaIoRequest {
  OptaMonitorIo monitors[OPTA_IO_MAX_MONITORS];
  uint8_t monitorCount;    // entries past 8 are ignored
  int16_t buttonTerminal;  // resolved clear button terminal 0-7 (optaResolveClearButton), or -1
  bool vinEnabled;
  int16_t vinTerminal;     // claimed only when enabled and 0-7, as readVinVoltage reads it
};

struct OptaIoOptions {
  bool contactsUseAdc;  // floats and the clear button are read with analogRead (true) or digitalRead
  uint8_t contactPull;  // OptaPull; used only when contactsUseAdc is false
  uint8_t pulsePull;    // OptaPull; v2.2.16 used INPUT_PULLUP
};
// Placeholders until bench A4 (contacts) and A6 (pulse) settle them (D6).
static constexpr OptaIoOptions OPTA_IO_DEFAULT_OPTIONS = { true, OPTA_PULL_NONE, OPTA_PULL_UP };

struct OptaIoPlan {
  int16_t monitorTerminal[OPTA_IO_MAX_MONITORS];  // contact/pulse: terminal; analog: channel; else -1
  uint8_t monitorErr[OPTA_IO_MAX_MONITORS];       // OptaIoErr
  int16_t buttonTerminal;                         // -1 when off or in error
  uint8_t buttonErr;                              // OptaIoErr
  uint8_t role[OPTA_INPUT_COUNT];                 // winning OptaIoRole per terminal (NONE if unused or contested)
  uint8_t cls[OPTA_INPUT_COUNT];                  // OptaIoClass this plan configures per terminal
  uint8_t pull[OPTA_INPUT_COUNT];                 // OptaPull set on a GPIO terminal, else NONE
  uint8_t usedClass[OPTA_INPUT_COUNT];            // first class used since boot (sticky)
  uint8_t warnings;                               // OPTA_IOW_* bits
};

// The boot state: nothing claimed, nothing used.
static inline void optaIoPlanInit(OptaIoPlan &p) {
  for (uint8_t i = 0; i < OPTA_IO_MAX_MONITORS; ++i) {
    p.monitorTerminal[i] = -1;
    p.monitorErr[i] = OPTA_IOE_OK;
  }
  p.buttonTerminal = -1;
  p.buttonErr = OPTA_IOE_OK;
  for (uint8_t t = 0; t < OPTA_INPUT_COUNT; ++t) {
    p.role[t] = OPTA_ROLE_NONE;
    p.cls[t] = OPTA_CLASS_NONE;
    p.pull[t] = OPTA_PULL_NONE;
    p.usedClass[t] = OPTA_CLASS_NONE;
  }
  p.warnings = 0;
}

static inline uint8_t optaIoRoleClass(uint8_t role, const OptaIoOptions &opt) {
  switch (role) {
    case OPTA_ROLE_ANALOG:
    case OPTA_ROLE_VIN:
      return OPTA_CLASS_ADC;
    case OPTA_ROLE_CONTACT:
    case OPTA_ROLE_BUTTON:
      return opt.contactsUseAdc ? OPTA_CLASS_ADC : OPTA_CLASS_GPIO;
    case OPTA_ROLE_PULSE:
      return OPTA_CLASS_GPIO;
    default:
      return OPTA_CLASS_NONE;
  }
}

// Pull a GPIO-class role is configured with.
static inline uint8_t optaIoRolePull(uint8_t role, const OptaIoOptions &opt) {
  if (role == OPTA_ROLE_PULSE) return opt.pulsePull;
  if (role == OPTA_ROLE_CONTACT || role == OPTA_ROLE_BUTTON) return opt.contactPull;
  return OPTA_PULL_NONE;
}

// Terminal a monitor claims (-1 for none) and its role (NONE when it never claims one).
// role != NONE with -1 means UNASSIGNED.
static inline int16_t optaMonitorClaim(const OptaMonitorIo &m, uint8_t &role) {
  switch (m.iface) {
    case OPTA_IFACE_ANALOG:
      role = OPTA_ROLE_ANALOG;
      return optaLegacyAnalogChannel(m.primaryPin);
    case OPTA_IFACE_DIGITAL:
      role = OPTA_ROLE_CONTACT;
      return optaIsTerminal(m.primaryPin) ? m.primaryPin : (int16_t)-1;
    case OPTA_IFACE_PULSE:
      role = OPTA_ROLE_PULSE;
      if (optaIsTerminal(m.pulsePin)) return m.pulsePin;
      return optaIsTerminal(m.primaryPin) ? m.primaryPin : (int16_t)-1;
    default:
      role = OPTA_ROLE_NONE;  // current loop reads the A0602; the parser makes no other iface
      return -1;
  }
}

// Claim slots used by the plan and the summary: 0-7 monitors, then the button, then Vin.
static constexpr uint8_t OPTA_IO_SLOT_BUTTON = OPTA_IO_MAX_MONITORS;
static constexpr uint8_t OPTA_IO_SLOT_VIN = OPTA_IO_MAX_MONITORS + 1;
static constexpr uint8_t OPTA_IO_SLOTS = OPTA_IO_MAX_MONITORS + 2;

static inline void optaBuildIoPlan(const OptaIoRequest &req, const OptaIoOptions &opt,
                                   const OptaIoPlan &prev, OptaIoPlan &out) {
  uint8_t used[OPTA_INPUT_COUNT];
  memcpy(used, prev.usedClass, sizeof(used));  // before the init, so out may be prev
  optaIoPlanInit(out);
  memcpy(out.usedClass, used, sizeof(used));

  int16_t term[OPTA_IO_SLOTS];
  uint8_t role[OPTA_IO_SLOTS];
  uint8_t *err[OPTA_IO_SLOTS];  // Vin has no error field
  for (uint8_t k = 0; k < OPTA_IO_SLOTS; ++k) {
    term[k] = -1;
    role[k] = OPTA_ROLE_NONE;
    err[k] = nullptr;
  }
  for (uint8_t i = 0; i < OPTA_IO_MAX_MONITORS; ++i) err[i] = &out.monitorErr[i];
  err[OPTA_IO_SLOT_BUTTON] = &out.buttonErr;

  const uint8_t n = req.monitorCount < OPTA_IO_MAX_MONITORS ? req.monitorCount : OPTA_IO_MAX_MONITORS;
  for (uint8_t i = 0; i < n; ++i) {
    const OptaMonitorIo &m = req.monitors[i];
    term[i] = optaMonitorClaim(m, role[i]);
    if (role[i] != OPTA_ROLE_NONE && term[i] < 0) out.monitorErr[i] = OPTA_IOE_UNASSIGNED;
    if (m.iface == OPTA_IFACE_ANALOG && !optaIsTerminal(m.primaryPin)) out.warnings |= OPTA_IOW_ANALOG_CLAMPED;
  }
  if (optaIsTerminal(req.buttonTerminal)) {
    term[OPTA_IO_SLOT_BUTTON] = req.buttonTerminal;
    role[OPTA_IO_SLOT_BUTTON] = OPTA_ROLE_BUTTON;
  } else if (req.buttonTerminal != -1) {
    out.buttonErr = OPTA_IOE_UNASSIGNED;
  }
  if (req.vinEnabled && optaIsTerminal(req.vinTerminal)) {
    term[OPTA_IO_SLOT_VIN] = req.vinTerminal;
    role[OPTA_IO_SLOT_VIN] = OPTA_ROLE_VIN;
  }

  for (uint8_t t = 0; t < OPTA_INPUT_COUNT; ++t) {
    uint8_t claims = 0, analog = 0, exclusive = 0, exclusiveRole = OPTA_ROLE_NONE;
    bool vin = false;
    for (uint8_t k = 0; k < OPTA_IO_SLOTS; ++k) {
      if (term[k] != t) continue;
      ++claims;
      if (role[k] == OPTA_ROLE_ANALOG) {
        ++analog;
      } else if (role[k] == OPTA_ROLE_VIN) {
        vin = true;
      } else {
        ++exclusive;
        exclusiveRole = role[k];
      }
    }
    if (claims == 0) continue;

    uint8_t winner;
    if (exclusive > 0 && claims > 1) {
      for (uint8_t k = 0; k < OPTA_IO_SLOTS; ++k) {
        if (term[k] == t && err[k] != nullptr) *err[k] = OPTA_IOE_CONFLICT;
      }
      if (!vin) continue;
      out.warnings |= OPTA_IOW_VIN_CONFLICT;
      winner = OPTA_ROLE_VIN;
    } else if (exclusive == 1) {
      winner = exclusiveRole;
    } else {
      if (analog >= 2) out.warnings |= OPTA_IOW_ANALOG_SHARED;
      if (analog > 0 && vin) out.warnings |= OPTA_IOW_ANALOG_VIN_SHARED;
      winner = analog > 0 ? OPTA_ROLE_ANALOG : OPTA_ROLE_VIN;
    }

    const uint8_t cls = optaIoRoleClass(winner, opt);
    if (out.usedClass[t] != OPTA_CLASS_NONE && out.usedClass[t] != cls) {
      for (uint8_t k = 0; k < OPTA_IO_SLOTS; ++k) {
        if (term[k] != t) continue;
        if (err[k] == nullptr) {
          out.warnings |= OPTA_IOW_VIN_RESTART;
        } else if (*err[k] == OPTA_IOE_OK) {
          *err[k] = OPTA_IOE_RESTART;
        }
      }
      continue;
    }
    out.role[t] = winner;
    out.cls[t] = cls;
    out.pull[t] = (cls == OPTA_CLASS_GPIO) ? optaIoRolePull(winner, opt) : (uint8_t)OPTA_PULL_NONE;
    if (out.usedClass[t] == OPTA_CLASS_NONE) out.usedClass[t] = cls;
  }

  for (uint8_t i = 0; i < n; ++i) {
    out.monitorTerminal[i] = (out.monitorErr[i] == OPTA_IOE_OK) ? term[i] : (int16_t)-1;
  }
  out.buttonTerminal = (out.buttonErr == OPTA_IOE_OK) ? term[OPTA_IO_SLOT_BUTTON] : (int16_t)-1;
}

// One pinMode call: pull NONE = pinMode(INPUT), UP = pinMode(INPUT_PULLUP).
struct OptaPinOp {
  int16_t pin;
  uint8_t pull;
};

// The pinMode calls that take the terminals from `prev` to `next`: configure a terminal that
// becomes GPIO or changes pull, release (INPUT, no pull) one that stops being GPIO. At most one
// per terminal, so ops[8] is always enough; returns the number written (stops at maxOps). Never
// an op for a terminal that `next` reads as ADC: pinMode would replace the pin's setup.
// CL-1: the pulls come from the plans, not the options, so a config push that changes only
// pulsePull or contactPull still reconfigures the pin.
static inline uint8_t optaPlanPinOps(const OptaIoPlan &prev, const OptaIoPlan &next,
                                     OptaPinOp *ops, uint8_t maxOps) {
  uint8_t n = 0;
  for (uint8_t t = 0; t < OPTA_INPUT_COUNT && n < maxOps; ++t) {
    if (next.cls[t] == OPTA_CLASS_ADC) continue;
    const bool wasGpio = prev.cls[t] == OPTA_CLASS_GPIO;
    if (next.cls[t] == OPTA_CLASS_GPIO) {
      const uint8_t want = next.pull[t];
      if (wasGpio && prev.pull[t] == want) continue;
      ops[n].pin = optaInputPin(t);
      ops[n].pull = want;
      ++n;
    } else if (wasGpio) {
      ops[n].pin = optaInputPin(t);
      ops[n].pull = OPTA_PULL_NONE;
      ++n;
    }
  }
  return n;
}

// ---------------------------------------------------------------------------------------------
// ACK summary
// ---------------------------------------------------------------------------------------------

// Token for claim slot k, or 0 and "" when it has none.
static inline size_t optaIoSummaryToken(const OptaIoPlan &p, const OptaIoRequest &req, uint8_t k,
                                        char *tok, size_t len) {
  if (len == 0) return 0;
  tok[0] = '\0';
  uint8_t err;
  int16_t t;
  char who[8];
  if (k < OPTA_IO_MAX_MONITORS) {
    err = p.monitorErr[k];
    if (err == OPTA_IOE_OK) return 0;
    uint8_t role;
    t = optaMonitorClaim(req.monitors[k], role);
    snprintf(who, sizeof(who), "m%u", (unsigned)(k + 1));
  } else if (k == OPTA_IO_SLOT_BUTTON) {
    err = p.buttonErr;
    t = req.buttonTerminal;
    snprintf(who, sizeof(who), "btn");
  } else if (k == OPTA_IO_SLOT_VIN && (p.warnings & OPTA_IOW_VIN_CONFLICT)) {
    err = OPTA_IOE_CONFLICT;
    t = req.vinTerminal;
    snprintf(who, sizeof(who), "vin");
  } else {
    return 0;
  }
  const unsigned input = optaIsTerminal(t) ? (unsigned)(t + 1) : 0u;  // printed only for dup/reboot
  int w;
  switch (err) {
    case OPTA_IOE_UNASSIGNED: w = snprintf(tok, len, "%s no input", who); break;
    case OPTA_IOE_CONFLICT:   w = snprintf(tok, len, "%s dup I%u", who, input); break;
    case OPTA_IOE_RESTART:    w = snprintf(tok, len, "%s reboot I%u", who, input); break;
    default: return 0;
  }
  if (w < 0) return 0;
  return ((size_t)w < len) ? (size_t)w : len - 1;
}

// Short text for the config ACK "message" (client buffer 48 bytes incl. NUL). Tokens joined by
// "; ": "m<i+1> no input", "m<i+1> dup I<t+1>", "m<i+1> reboot I<t+1>", "btn no input",
// "btn dup I<t+1>", "btn reboot I<t+1>", "vin dup I<t+1>". A token that does not fit is never
// cut: the text ends with "+" instead. Returns the length; 0 and "" when the plan has no errors.
static inline size_t optaIoSummary(const OptaIoPlan &p, const OptaIoRequest &req, char *buf, size_t len) {
  if (buf == nullptr || len == 0) return 0;
  buf[0] = '\0';
  char tok[24];
  uint8_t last = OPTA_IO_SLOTS;
  for (uint8_t k = 0; k < OPTA_IO_SLOTS; ++k) {
    if (optaIoSummaryToken(p, req, k, tok, sizeof(tok)) > 0) last = k;
  }
  size_t pos = 0;
  for (uint8_t k = 0; k < OPTA_IO_SLOTS; ++k) {
    const size_t tl = optaIoSummaryToken(p, req, k, tok, sizeof(tok));
    if (tl == 0) continue;
    const size_t sep = pos > 0 ? 2 : 0;
    const size_t plus = (k == last) ? 0 : 1;  // keep room for "+" while more tokens follow
    if (pos + sep + tl + plus > len - 1) {
      if (pos + 1 <= len - 1) buf[pos++] = '+';
      break;
    }
    if (sep) {
      buf[pos++] = ';';
      buf[pos++] = ' ';
    }
    memcpy(buf + pos, tok, tl);
    pos += tl;
  }
  buf[pos] = '\0';
  return pos;
}

// ---------------------------------------------------------------------------------------------
// Clear button
// ---------------------------------------------------------------------------------------------

enum : uint8_t { OPTA_BTNW_NONE = 0, OPTA_BTNW_INVALID, OPTA_BTNW_LEGACY_IGNORED };

// Clear button terminal from the config keys (R3). clearButtonInput (hasNew) is a terminal 0-7
// or -1 and wins outright. The legacy clearButtonPin held a raw pin number, which is never
// honoured: a legacy value >= 0 disables the button with a warning.
static inline int16_t optaResolveClearButton(bool hasNew, int32_t newVal, bool hasLegacy,
                                             int32_t legacyVal, uint8_t &warn) {
  warn = OPTA_BTNW_NONE;
  if (hasNew) {
    if (optaIsTerminal(newVal)) return (int16_t)newVal;
    if (newVal != -1) warn = OPTA_BTNW_INVALID;
    return -1;
  }
  if (hasLegacy && legacyVal >= 0) warn = OPTA_BTNW_LEGACY_IGNORED;
  return -1;
}

static constexpr uint32_t OPTA_BUTTON_DEBOUNCE_MS = 50;    // == CLEAR_BUTTON_DEBOUNCE_MS
static constexpr uint32_t OPTA_BUTTON_MIN_PRESS_MS = 500;  // == CLEAR_BUTTON_MIN_PRESS_MS
enum : uint8_t { OPTA_BTN_WAIT_RELEASE = 0, OPTA_BTN_ARMED, OPTA_BTN_FIRED };

struct OptaButton {
  uint8_t state;        // OPTA_BTN_*
  bool raw;             // last level seen
  uint32_t rawSinceMs;  // when `raw` last changed
};

// Assumes pressed, so a button held or stuck at boot or across a config push never fires until
// it has been released.
static inline void optaButtonReset(OptaButton &b, uint32_t nowMs) {
  b.state = OPTA_BTN_WAIT_RELEASE;
  b.raw = true;
  b.rawSinceMs = nowMs;
}

// Non-blocking replacement for the v2.2.16 2 s release wait. True exactly once per press held
// stable for 500 ms after a release stable for 50 ms; any change of level restarts the timer.
// The stable time is an unsigned difference, so a millis() wrap is harmless.
static inline bool optaButtonStep(OptaButton &b, bool pressed, uint32_t nowMs) {
  if (pressed != b.raw) {
    b.raw = pressed;
    b.rawSinceMs = nowMs;
    return false;
  }
  const uint32_t stableMs = nowMs - b.rawSinceMs;
  if (!pressed) {
    if (b.state != OPTA_BTN_ARMED && stableMs >= OPTA_BUTTON_DEBOUNCE_MS) b.state = OPTA_BTN_ARMED;
    return false;
  }
  if (b.state == OPTA_BTN_ARMED && stableMs >= OPTA_BUTTON_MIN_PRESS_MS) {
    b.state = OPTA_BTN_FIRED;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------------------------
// Indicators (D1)
// ---------------------------------------------------------------------------------------------

static constexpr uint8_t OPTA_IND_USER_LED_BIT = 4;  // bits 0-3: relay LEDs R1-R4; bit 4: LED_USER

// Relay LED r mirrors the coil output as actually driven; LED_USER lights while any level alarm
// is latched; everything is off in CRITICAL hibernate. The caller writes only bits in prev ^ next.
static constexpr uint8_t optaIndicatorMask(uint8_t coilDriveMask, bool anyAlarmLatched, bool criticalInhibit) {
  return criticalInhibit ? (uint8_t)0 : (uint8_t)((coilDriveMask & 0x0F) | (anyAlarmLatched ? 0x10 : 0x00));
}
static constexpr int16_t optaIndicatorPin(uint8_t index) {  // not `bit`: Arduino has a bit() macro
  return index < 4 ? optaRelayLedPin(index) : index == OPTA_IND_USER_LED_BIT ? OPTA_LED_USER_PIN : (int16_t)-1;
}

#endif  // TANKALARM_OPTA_IO_H
