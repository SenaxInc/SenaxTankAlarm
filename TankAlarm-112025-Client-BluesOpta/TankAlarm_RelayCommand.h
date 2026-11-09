// TankAlarm_RelayCommand.h - pure decisions for relay commands received in relay.qi (S-T01, CL-4).
//
// Clear Relay names a sensor by its stable number k (the config "number", MonitorConfig::sensorIndex,
// the "k" in every note). v2.2.16 servers sent the dashboard's list position as
// "relay_reset_sensor" instead, which the client applied as a monitor slot and so could clear the
// wrong sensor (S-T01/S-T06). This header decides what a command means; the sketch acts on it.
//
// Sensor numbers belong to the client that owns the sensor, so a by-number command is always
// resolved here, on the source, and is never forwarded. If the resolved monitor drives relays on
// another client, the source asks the server to forward ordinary relay/state OFF commands to it
// (queued, not confirmed until C-T02).
//
// Pure: ArduinoJson and C headers only, C++14, no RTTI, so tests/host/relay_command compiles this
// exact file. Unknown keys are ignored, so later fields (C-T02: cid, t, exp; C-A02:
// relay_clear_all) extend RelayCommand and RelayCmdKind without changing callers.

#ifndef TANKALARM_RELAY_COMMAND_H
#define TANKALARM_RELAY_COMMAND_H

#include <stdint.h>
#include <string.h>
#include <ArduinoJson.h>

// A JSON integer in [lo, hi] (hi <= 0xFFFFFFFF). Also accepts a number with an exact integral
// value such as 3.0, because a note body can pass through a double on the Notecard/Notehub path.
// Rejects booleans, strings, null, fractions, NaN and out-of-range values; never maps a bad value
// to 0 or 1 the way as<uint8_t>() does.
static inline bool relayJsonUint(JsonVariantConst v, uint32_t lo, uint32_t hi, uint32_t &out) {
  if (v.isNull() || v.is<bool>()) return false;
  if (v.is<uint32_t>()) {
    const uint32_t x = v.as<uint32_t>();
    if (x < lo || x > hi) return false;
    out = x;
    return true;
  }
  if (!v.is<double>()) return false;
  const double d = v.as<double>();
  if (!(d >= (double)lo && d <= (double)hi)) return false;  // also rejects NaN
  const uint32_t x = (uint32_t)d;
  if ((double)x != d) return false;
  out = x;
  return true;
}

enum RelayCmdKind : uint8_t {
  RELAY_CMD_NONE = 0,             // no Clear Relay key: the caller runs the existing relay/state path
  RELAY_CMD_RESET_BY_NUMBER,      // relay_reset_sensor_number = k, an integer 1..255
  RELAY_CMD_RESET_INVALID,        // relay_reset_sensor_number present (even null), not 1..255
  RELAY_CMD_RESET_LEGACY_IGNORED  // only relay_reset_sensor, any value (v2.2.16 list position)
};

struct RelayCommand {
  RelayCmdKind kind;
  uint8_t sensorNumber;  // RELAY_CMD_RESET_BY_NUMBER only
};

// Precedence: relay_reset_sensor_number (never falls back to the legacy key, and wins over
// relay/state in the same note), then relay_reset_sensor, then NONE. A key that is present counts
// whatever its value, JSON null included: isUnbound() is true only for a missing key, while isNull()
// is also true for an explicit null and would let {"relay_reset_sensor_number":null,"relay":1,
// "state":true} fall through and switch a relay. Only NONE runs relay/state (fail closed).
static inline RelayCmdKind relayClassifyCommand(JsonObjectConst doc, RelayCommand &out) {
  out.kind = RELAY_CMD_NONE;
  out.sensorNumber = 0;
  JsonVariantConst byNumber = doc["relay_reset_sensor_number"];
  if (!byNumber.isUnbound()) {
    uint32_t k = 0;
    if (relayJsonUint(byNumber, 1, 255, k)) {
      out.kind = RELAY_CMD_RESET_BY_NUMBER;
      out.sensorNumber = (uint8_t)k;
    } else {
      out.kind = RELAY_CMD_RESET_INVALID;
    }
    return out.kind;
  }
  if (!doc["relay_reset_sensor"].isUnbound()) out.kind = RELAY_CMD_RESET_LEGACY_IGNORED;
  return out.kind;
}

enum RelayResolve : uint8_t {
  RELAY_RESOLVE_OK = 0,
  RELAY_RESOLVE_NOT_FOUND,
  RELAY_RESOLVE_DUPLICATE,
  RELAY_RESOLVE_INVALID  // k == 0, or a null table with count > 0
};

// numbers[i] is monitor slot i's sensor number. Only a unique match resolves; slot is written only
// on RELAY_RESOLVE_OK.
static inline RelayResolve relayResolveSensorNumber(const uint8_t *numbers, uint8_t count, uint8_t k,
                                                    uint8_t &slot) {
  if (k == 0 || (count > 0 && numbers == nullptr)) return RELAY_RESOLVE_INVALID;
  uint8_t found = 0;
  uint8_t at = 0;
  for (uint8_t i = 0; i < count; ++i) {
    if (numbers[i] != k) continue;
    if (found == 0) at = i;
    if (found < 2) ++found;
  }
  if (found == 0) return RELAY_RESOLVE_NOT_FOUND;
  if (found > 1) return RELAY_RESOLVE_DUPLICATE;
  slot = at;
  return RELAY_RESOLVE_OK;
}

enum RelayScope : uint8_t { RELAY_SCOPE_NONE = 0, RELAY_SCOPE_LOCAL, RELAY_SCOPE_REMOTE };

// How a monitor's relay binding is configured, by the relay/float plan's rule R6: no relays (relayMask
// 0; "no relay" is always written as mask 0), this client's own relays (blank target or this device's
// own UID; the new page's "This client" writes a blank target), or another client's relays.
// v2.2.16's tracked activation and boot-restore paths still skip a blank target (they test
// relayTargetClient[0] directly) until CL-6b converts them to this rule, so in CL-4 this function
// only labels the Clear Relay log line; it changes no relay. C-A02 drives relays by it.
static inline RelayScope relayScopeOf(uint8_t relayMask, const char *target, const char *ownUid) {
  if ((relayMask & 0x0F) == 0) return RELAY_SCOPE_NONE;
  if (target == nullptr || target[0] == '\0') return RELAY_SCOPE_LOCAL;
  if (ownUid != nullptr && ownUid[0] != '\0' && strcmp(target, ownUid) == 0) return RELAY_SCOPE_LOCAL;
  return RELAY_SCOPE_REMOTE;
}

// Outcome of one Clear Relay command. Logged today; C-T02 reports it to the server ("rr").
enum RelayClearResult : uint8_t {
  // RELAY_CLEAR_RELEASED: relays tracked as ON for the sensor were released here. A local binding's
  // coils are off; for a remote binding the OFF was only requested (queued for the server to forward),
  // not confirmed and not retried until C-A02 (retry) and C-T02 (confirmation). See relayClearLogWord.
  RELAY_CLEAR_RELEASED = 0,      // tracked relays released (remote: OFF requested, not confirmed)
  RELAY_CLEAR_NONE_ACTIVE,       // sensor found; nothing tracked as ON for it
  RELAY_CLEAR_UNKNOWN_SENSOR,    // no monitor has this number
  RELAY_CLEAR_DUPLICATE_SENSOR,  // two monitors have this number: nothing is guessed
  RELAY_CLEAR_INVALID,           // relay_reset_sensor_number is not an integer 1..255
  RELAY_CLEAR_LEGACY_IGNORED,    // relay_reset_sensor (list position) from an old server
  RELAY_CLEAR_RESULT_COUNT
};

static inline const char *relayClearResultName(uint8_t result) {
  static const char *const kNames[RELAY_CLEAR_RESULT_COUNT] = {
      "released", "none-active", "unknown-sensor", "duplicate-sensor", "invalid", "legacy-ignored"};
  return result < RELAY_CLEAR_RESULT_COUNT ? kNames[result] : "invalid";
}

// The result word for the Clear Relay log line. A remote binding's release only queues an OFF request
// (not confirmed; C-A02 retries, C-T02 confirms), so it is logged as "off-requested" instead of
// "released". A word swap, not a suffix, so the line stays inside the 160-byte serial log entry. The
// result name reported later ("rr", C-T02) is still relayClearResultName.
static inline const char *relayClearLogWord(uint8_t result, uint8_t scope) {
  if (result == RELAY_CLEAR_RELEASED && scope == RELAY_SCOPE_REMOTE) return "off-requested";
  return relayClearResultName(result);
}

static inline RelayClearResult relayClearResultFromResolve(RelayResolve rc, uint8_t activeMask) {
  switch (rc) {
    case RELAY_RESOLVE_OK:        return activeMask ? RELAY_CLEAR_RELEASED : RELAY_CLEAR_NONE_ACTIVE;
    case RELAY_RESOLVE_NOT_FOUND: return RELAY_CLEAR_UNKNOWN_SENSOR;
    case RELAY_RESOLVE_DUPLICATE: return RELAY_CLEAR_DUPLICATE_SENSOR;
    case RELAY_RESOLVE_INVALID:
    default:                      return RELAY_CLEAR_INVALID;
  }
}

struct RelayClearOutcome {
  RelayResolve resolve;
  uint8_t slot;        // the monitor slot; meaningful only when resolve == RELAY_RESOLVE_OK
  uint8_t activeMask;  // relays tracked as ON for that monitor; 0 unless resolve == RELAY_RESOLVE_OK
  RelayClearResult result;
};

// The whole Clear Relay step for sensor number k over the configured monitors' numbers. The firmware
// side effects are passed in, so the host test runs this same code with recording doubles:
//   activeMaskOf(slot) -> uint8_t  reads the relays tracked as ON (getMonitorActiveRelayMask);
//   log(outcome)                   writes the one log line;
//   release(slot)                  switches the monitor's relays off (resetRelayForMonitor).
// release runs once, after log, and only when exactly one monitor has number k; activeMaskOf runs
// only then too. Nothing else is touched.
template <typename ActiveMaskFn, typename LogFn, typename ReleaseFn>
static inline RelayClearOutcome relayClearSensorNumber(const uint8_t *numbers, uint8_t count, uint8_t k,
                                                       ActiveMaskFn activeMaskOf, LogFn log,
                                                       ReleaseFn release) {
  RelayClearOutcome o;
  o.slot = 0;
  o.resolve = relayResolveSensorNumber(numbers, count, k, o.slot);
  o.activeMask = (o.resolve == RELAY_RESOLVE_OK) ? (uint8_t)activeMaskOf(o.slot) : (uint8_t)0;
  o.result = relayClearResultFromResolve(o.resolve, o.activeMask);
  log(o);
  if (o.resolve == RELAY_RESOLVE_OK) release(o.slot);
  return o;
}

#endif  // TANKALARM_RELAY_COMMAND_H
