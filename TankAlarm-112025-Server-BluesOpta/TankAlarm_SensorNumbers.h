// TankAlarm_SensorNumbers.h - checks the sensor numbers of a client config before the server
// caches and dispatches it (S-T01/S1).
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

#endif  // TANKALARM_SENSOR_NUMBERS_H
