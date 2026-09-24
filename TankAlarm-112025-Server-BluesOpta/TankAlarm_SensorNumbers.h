// TankAlarm_SensorNumbers.h - checks the sensor numbers of a client config before the server
// caches and dispatches it (S-T01/S1).
//
// A sensor's number is its identity everywhere: the config "number", the "k" in every note, the
// server registry, learned calibration, alarm contacts and Clear Relay. The client takes "number"
// when it is an integer 0-255 and otherwise uses position+1 (initMonitorDefaults and
// parseMonitorFromJson in the client sketch), and it accepts 0 and duplicates without complaint,
// so the server must refuse them.
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
#define SENSOR_NUMBERS_INVALID   1  // an entry is not an object, or its number is not an integer 1-255
#define SENSOR_NUMBERS_DUPLICATE 2  // two sensors resolve to the same number

// Checks every entry of a config's "sensors" array. A missing (or null) "number" means position+1,
// as on the client. Returns SENSOR_NUMBERS_OK, or an error with a short reason in msg (always
// NUL-terminated when msgLen > 0; msg may be null). A null array is OK: a config without sensors
// changes none.
static inline uint8_t sensorNumbersCheck(JsonArrayConst sensors, char *msg, size_t msgLen) {
  if (msg && msgLen) msg[0] = '\0';
  if (sensors.isNull()) return SENSOR_NUMBERS_OK;
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
      k = (uint32_t)position;
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
