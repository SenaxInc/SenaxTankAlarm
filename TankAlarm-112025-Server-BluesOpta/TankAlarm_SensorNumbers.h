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

// The high mark a cached config payload (the snapshot's JSON text) carries: the larger of its "snh"
// and its highest sensor number. Snapshots saved before snh existed carry only their numbers.
// Parses only those two fields, so it needs little memory. 0 for a null, empty or unreadable payload.
static inline uint8_t sensorNumbersCachedHigh(const char *payload) {
  if (!payload || payload[0] == '\0') return 0;
  JsonDocument filter;
  filter["snh"] = true;
  filter["sensors"][0]["number"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) return 0;
  const uint8_t snh = sensorNumbersByteValue(doc["snh"]);
  const uint8_t top = sensorNumbersMaxNumber(doc["sensors"]);
  return snh > top ? snh : top;
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
