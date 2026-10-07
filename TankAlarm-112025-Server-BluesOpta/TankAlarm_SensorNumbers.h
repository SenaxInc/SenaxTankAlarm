// TankAlarm_SensorNumbers.h - checks the sensor numbers of a client config before the server
// caches and dispatches it (S-T01/S1), and keeps the config's sensor-number high mark "snh" from
// going down (CR-5, see below).
//
// A sensor's number is its identity everywhere: the config "number", the "k" in every note, the
// server registry, learned calibration, alarm contacts and Clear Relay. The client takes "number"
// when it is an integer 0-255 and accepts 0 and duplicates without complaint, so the server must
// refuse them. Without a usable "number" the client's result depends on the path: a load from
// flash uses position+1, but a live config update keeps that slot's previous number
// (applyConfigUpdate does not reset the monitor first). The server therefore requires every
// sensor to carry its number; the Config Generator always writes it.
//
// This header needs only ArduinoJson and the C library, so tests/host/sensor_numbers can build it
// with the PC compiler.

#ifndef TANKALARM_SENSOR_NUMBERS_H
#define TANKALARM_SENSOR_NUMBERS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ArduinoJson.h>

#define SENSOR_NUMBERS_OK        0
#define SENSOR_NUMBERS_INVALID   1  // "sensors" is not a list, an entry is not an object, or its number is
                                    // missing or not an integer 1-255
#define SENSOR_NUMBERS_DUPLICATE 2  // two sensors resolve to the same number

// Checks a config's "sensors" value, passed as it is in the document (not converted to an array,
// which would turn an object or a string into a null array and let it through). Every entry needs a
// "number"; a missing or null one is INVALID (see above). Returns SENSOR_NUMBERS_OK, or an error
// with a short reason in msg (always NUL-terminated when msgLen > 0; msg may be null). A missing or
// null "sensors" is OK: a config without sensors changes none. Any other non-list value is INVALID.
static inline uint8_t sensorNumbersCheck(JsonVariantConst sensorsValue, char *msg, size_t msgLen) {
  if (msg && msgLen) msg[0] = '\0';
  if (sensorsValue.isNull()) return SENSOR_NUMBERS_OK;
  if (!sensorsValue.is<JsonArrayConst>()) {
    if (msg && msgLen) snprintf(msg, msgLen, "sensors must be a list");
    return SENSOR_NUMBERS_INVALID;
  }
  JsonArrayConst sensors = sensorsValue.as<JsonArrayConst>();
  uint8_t seen[32];  // one bit per number 0-255
  memset(seen, 0, sizeof(seen));
  size_t position = 0;
  for (JsonVariantConst t : sensors) {
    ++position;
    if (!t.is<JsonObjectConst>()) {
      if (msg && msgLen) snprintf(msg, msgLen, "sensor %u is not an object", (unsigned)position);
      return SENSOR_NUMBERS_INVALID;
    }
    JsonVariantConst n = t["number"];
    uint32_t k;
    if (n.isNull()) {
      if (msg && msgLen) snprintf(msg, msgLen, "sensor %u has no number", (unsigned)position);
      return SENSOR_NUMBERS_INVALID;
    } else if (!n.is<bool>() && n.is<uint8_t>()) {
      k = n.as<uint8_t>();
    } else {
      k = 0;  // 256, -1, 1.5, "1", true, arrays and objects
    }
    if (k < 1 || k > 255) {
      if (msg && msgLen) snprintf(msg, msgLen, "sensor %u: number must be a whole number 1-255", (unsigned)position);
      return SENSOR_NUMBERS_INVALID;
    }
    const uint8_t bit = (uint8_t)(1u << (k & 7u));
    if (seen[k >> 3] & bit) {
      if (msg && msgLen) snprintf(msg, msgLen, "sensor number %u is used twice", (unsigned)k);
      return SENSOR_NUMBERS_DUPLICATE;
    }
    seen[k >> 3] |= bit;
  }
  return SENSOR_NUMBERS_OK;
}

// ---- The high-water mark "snh" (CR-5) --------------------------------------------------------
// The Config Generator writes "snh", the highest sensor number the client has ever used, so a
// removed sensor's number is never handed out again. The client does not keep snh; the server's
// cached config snapshot is the only copy. handleConfigPost therefore never lets a post lower it:
// an imported old file without snh (or with a lower one) must not let a retired number come back.
// Until S6 the snapshot itself can lose or lower snh: eviction at the 20-entry cap, DELETE
// /api/client and restoring an older FTP backup. A deleted client's mark is forgotten by design
// (owner, 2026-10-07: no tombstone).

// The value of an "snh" or "number" when it is an integer 0-255, else 0 (missing, null, 256, -1,
// 1.5, "5", true, lists and objects count as no value).
static inline uint8_t sensorNumbersByteValue(JsonVariantConst v) {
  if (v.is<bool>() || !v.is<uint8_t>()) return 0;
  return v.as<uint8_t>();
}

// The highest usable "number" (1-255) in a "sensors" value, 0 when there is none or it is not a list.
static inline uint8_t sensorNumbersMaxNumber(JsonVariantConst sensorsValue) {
  uint8_t high = 0;
  if (!sensorsValue.is<JsonArrayConst>()) return 0;
  for (JsonVariantConst t : sensorsValue.as<JsonArrayConst>()) {
    if (!t.is<JsonObjectConst>()) continue;
    const uint8_t n = sensorNumbersByteValue(t["number"]);
    if (n > high) high = n;
  }
  return high;
}

// What a cached config payload (the snapshot's JSON text) says about the client's numbers. high is
// its high mark: the larger of its "snh" and its highest sensor number (snapshots saved before snh
// existed carry only their numbers); 0 for a null, empty or unreadable payload. active gets one bit
// per usable "number" (1-255) in its "sensors" (bit n&7 of active[n>>3]). Parses only those two
// fields, so it needs little memory. Returns true when the payload has a "sensors" list, so the
// numbers in use are known; false (high still set, active empty) for a null, empty or unreadable
// payload or one without a "sensors" list.
static inline bool sensorNumbersCachedState(const char *payload, uint8_t &high, uint8_t active[32]) {
  high = 0;
  memset(active, 0, 32);
  if (!payload || payload[0] == '\0') return false;
  JsonDocument filter;
  filter["snh"] = true;
  filter["sensors"][0]["number"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) return false;
  const uint8_t snh = sensorNumbersByteValue(doc["snh"]);
  const uint8_t top = sensorNumbersMaxNumber(doc["sensors"]);
  high = snh > top ? snh : top;
  JsonVariantConst sensors = doc["sensors"];
  if (!sensors.is<JsonArrayConst>()) return false;
  for (JsonVariantConst t : sensors.as<JsonArrayConst>()) {
    if (!t.is<JsonObjectConst>()) continue;
    const uint8_t n = sensorNumbersByteValue(t["number"]);
    if (n > 0) active[n >> 3] |= (uint8_t)(1u << (n & 7u));
  }
  return true;
}

// The high mark a cached config payload carries (see sensorNumbersCachedState).
static inline uint8_t sensorNumbersCachedHigh(const char *payload) {
  uint8_t high = 0;
  uint8_t active[32];
  sensorNumbersCachedState(payload, high, active);
  return high;
}

// ---- Reusing a retired number (C323, owner 2026-10-07) ---------------------------------------
// A posted number at or below the cached high mark that the cached config no longer holds belonged
// to a removed sensor; its history, alarm contacts and learned calibration are still keyed by it.
// handleConfigPost refuses such a post (409) unless the operator confirmed the reuse.

// Counts the posted "sensors" numbers n with 1 <= n <= cachedHigh that are not in active (the
// cached set, from sensorNumbersCachedState), and writes them in ascending order as "3, 5" into
// list. Only whole numbers are written: one that does not fit ends the list there. list is always
// NUL-terminated when listLen > 0 and may be null. Entries that are not objects or carry no usable
// number are skipped (sensorNumbersCheck refuses them first). Returns the count (0: none).
static inline size_t sensorNumbersRetiredReuse(JsonVariantConst postedSensors, const uint8_t active[32],
                                               uint8_t cachedHigh, char *list, size_t listLen) {
  if (list && listLen) list[0] = '\0';
  if (!postedSensors.is<JsonArrayConst>()) return 0;
  uint8_t retired[32];
  memset(retired, 0, sizeof(retired));
  for (JsonVariantConst t : postedSensors.as<JsonArrayConst>()) {
    if (!t.is<JsonObjectConst>()) continue;
    const uint8_t n = sensorNumbersByteValue(t["number"]);
    const uint8_t bit = (uint8_t)(1u << (n & 7u));
    if (n == 0 || n > cachedHigh || (active[n >> 3] & bit)) continue;
    retired[n >> 3] |= bit;
  }
  size_t count = 0;
  size_t used = 0;
  bool full = !list || listLen == 0;
  for (unsigned n = 1; n <= 255; ++n) {
    if (!(retired[n >> 3] & (1u << (n & 7u)))) continue;
    ++count;
    if (full) continue;
    char token[16];
    const int len = count > 1 ? snprintf(token, sizeof(token), ", %u", n)
                              : snprintf(token, sizeof(token), "%u", n);
    if (len <= 0 || used + (size_t)len >= listLen) {
      full = true;
      continue;
    }
    memcpy(list + used, token, (size_t)len + 1);
    used += (size_t)len;
  }
  return count;
}

// The snh to store with a posted config: the largest of the posted "snh" (ignored unless an integer
// 0-255), the cached snapshot's high mark (sensorNumbersCachedHigh, 0 when there is no snapshot) and
// the highest sensor number in the posted "sensors". Never lower than any of them.
static inline uint8_t sensorNumbersHighMark(JsonVariantConst postedSnh, uint8_t cachedHigh,
                                            JsonVariantConst sensorsValue) {
  uint8_t high = sensorNumbersByteValue(postedSnh);
  if (cachedHigh > high) high = cachedHigh;
  const uint8_t top = sensorNumbersMaxNumber(sensorsValue);
  if (top > high) high = top;
  return high;
}

#endif  // TANKALARM_SENSOR_NUMBERS_H
