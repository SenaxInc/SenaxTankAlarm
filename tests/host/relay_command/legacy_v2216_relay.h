// legacy_v2216_relay.h - FROZEN copy of the decisions v2.2.16 makes for one relay.qi note
// (processRelayCommand, client .ino 9460-9538 at c9a9cce), without Serial, GPIO or the 5 s
// cooldown. Test-only: it shows that old firmware ignores the new Clear Relay key and what the old
// key did. Never edit it to match new behaviour.

#ifndef LEGACY_V2216_RELAY_H
#define LEGACY_V2216_RELAY_H

#include <stdint.h>
#include <string.h>
#include <ArduinoJson.h>

enum LegacyAction : uint8_t {
  LEGACY_NOT_FOR_US = 0,   // "target" names another device
  LEGACY_RESET_SLOT,       // resetRelayForMonitor(slot)
  LEGACY_RESET_DROPPED,    // relay_reset_sensor >= MAX_MONITORS: returns without acting
  LEGACY_SET,              // relay (1-4) + state
  LEGACY_INVALID           // "Invalid relay command" / "Invalid relay number": nothing happens
};

struct LegacyDecision {
  LegacyAction action;
  uint8_t slot;   // LEGACY_RESET_SLOT
  uint8_t relay;  // LEGACY_SET, 0-based
  bool state;     // LEGACY_SET
};

static inline LegacyDecision legacyV2216Decide(const JsonDocument &doc, const char *deviceUid) {
  LegacyDecision d = {LEGACY_INVALID, 0, 0, false};
  if (!doc["target"].isNull()) {
    const char *targetUid = doc["target"].as<const char *>();
    if (targetUid && targetUid[0] != '\0' && strcmp(targetUid, deviceUid) != 0) {
      d.action = LEGACY_NOT_FOR_US;
      return d;
    }
  }
  if (!doc["relay_reset_sensor"].isNull()) {
    uint8_t sensorIdx = doc["relay_reset_sensor"].as<uint8_t>();
    if (sensorIdx < 8) {  // MAX_MONITORS
      d.action = LEGACY_RESET_SLOT;
      d.slot = sensorIdx;
    } else {
      d.action = LEGACY_RESET_DROPPED;
    }
    return d;
  }
  if (doc["relay"].isNull() || doc["state"].isNull()) return d;
  uint8_t relayNum = doc["relay"].as<uint8_t>();
  if (relayNum < 1 || relayNum > 4) return d;  // MAX_RELAYS
  d.action = LEGACY_SET;
  d.relay = (uint8_t)(relayNum - 1);
  d.state = doc["state"].as<bool>();
  return d;
}

#endif  // LEGACY_V2216_RELAY_H
