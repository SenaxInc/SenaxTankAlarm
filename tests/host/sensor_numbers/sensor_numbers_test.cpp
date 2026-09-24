// Host tests for TankAlarm-112025-Server-BluesOpta/TankAlarm_SensorNumbers.h (S-T01/S1).
//
// Build and run: make -C tests/host/sensor_numbers test ARDUINOJSON_DIR=/path/to/ArduinoJson/src
//
// Each case parses a literal config and checks the "sensors" array the way handleConfigPost
// does, comparing the result code and the exact message.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ArduinoJson.h>

#include "TankAlarm_SensorNumbers.h"

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

// Parses json and checks its "sensors" array with a message buffer of msgLen bytes.
static uint8_t checkJson(const char *json, char *msg, size_t msgLen) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json);
  CHECK(!err);
  return sensorNumbersCheck(doc["sensors"].as<JsonArrayConst>(), msg, msgLen);
}

static void expectResult(const char *json, uint8_t wantCode, const char *wantMsg) {
  char msg[80];
  memset(msg, 'x', sizeof(msg));
  const uint8_t got = checkJson(json, msg, sizeof(msg));
  ++gChecks;
  if (got != wantCode || strcmp(msg, wantMsg) != 0) {
    ++gFailures;
    printf("FAIL %s: got %u \"%s\", expected %u \"%s\"\n", json, (unsigned)got, msg,
           (unsigned)wantCode, wantMsg);
  }
}

static void expectOk(const char *json) { expectResult(json, SENSOR_NUMBERS_OK, ""); }

static void testAccepted() {
  expectOk("{}");                                   // no sensors key: a null array
  expectOk("{\"sensors\":null}");
  expectOk("{\"sensors\":[]}");
  expectOk("{\"sensors\":[{},{},{}]}");             // missing numbers are positions 1, 2, 3
  expectOk("{\"sensors\":[{\"number\":1},{\"number\":2},{\"number\":3}]}");
  expectOk("{\"sensors\":[{\"number\":2},{\"number\":1},{\"number\":3}]}");
  expectOk("{\"sensors\":[{\"number\":1},{\"number\":3},{\"number\":4}]}");  // #2 removed, #4 added
  expectOk("{\"sensors\":[{\"number\":255}]}");
  expectOk("{\"sensors\":[{\"number\":null}]}");    // null means the position, as on the client
  expectOk("{\"sensors\":[{\"number\":3},{}]}");    // position 2 defaults to 2
  expectOk("{\"sensors\":[{\"number\":1},{\"number\":2},{\"number\":3},{\"number\":4},"
           "{\"number\":5},{\"number\":6},{\"number\":7},{\"number\":8}]}");
  // Shaped like the Config Generator's output after sensor #2 was removed and one added.
  expectOk(
      "{\"productUid\":\"com.example.tankalarm\",\"deviceUid\":\"dev:860322068000001\","
      "\"site\":\"North Yard\",\"deviceLabel\":\"Tank bank\",\"clientFleet\":\"tankalarm-clients\","
      "\"serverFleet\":\"tankalarm-server\",\"sampleSeconds\":1800,\"reportHour\":11,"
      "\"reportMinute\":0,\"powerSupply\":\"grid\",\"sensors\":["
      "{\"id\":\"A\",\"monitorType\":\"tank\",\"name\":\"Tank 1\",\"contents\":\"Diesel\","
      "\"number\":1,\"userNumber\":1,\"sensor\":\"current\",\"primaryPin\":0,\"secondaryPin\":-1,"
      "\"loopChannel\":0,\"rpmPin\":-1,\"hysteresis\":2.0,\"daily\":true,\"upload\":true,"
      "\"reportThreshold\":0,\"stuckDetection\":true,\"calibrationEnabled\":true,"
      "\"currentLoopType\":\"pressure\",\"sensorMountHeight\":2,\"sensorRangeMin\":0,"
      "\"sensorRangeMax\":5,\"sensorRangeUnit\":\"PSI\",\"highAlarm\":110,\"lowAlarm\":12,"
      "\"alarmsEnabled\":true,\"fluidType\":\"diesel\",\"measurementUnit\":\"inches\"},"
      "{\"id\":\"B\",\"monitorType\":\"tank\",\"name\":\"Tank 3\",\"contents\":\"Water\","
      "\"number\":3,\"userNumber\":3,\"sensor\":\"analog\",\"primaryPin\":1,\"secondaryPin\":-1,"
      "\"loopChannel\":-1,\"rpmPin\":-1,\"hysteresis\":2.0,\"daily\":true,\"upload\":true,"
      "\"reportThreshold\":0,\"stuckDetection\":false,\"calibrationEnabled\":false,"
      "\"analogVoltageMin\":0,\"analogVoltageMax\":10,\"highAlarm\":90,\"alarmsEnabled\":true},"
      "{\"id\":\"C\",\"monitorType\":\"tank\",\"name\":\"Tank 4\",\"contents\":\"\","
      "\"number\":4,\"userNumber\":0,\"sensor\":\"digital\",\"primaryPin\":2,\"secondaryPin\":-1,"
      "\"loopChannel\":-1,\"rpmPin\":-1,\"hysteresis\":0,\"daily\":true,\"upload\":true,"
      "\"reportThreshold\":0,\"stuckDetection\":true,\"calibrationEnabled\":false,"
      "\"digitalSwitchMode\":\"NO\",\"digitalTrigger\":\"activated\",\"alarmsEnabled\":true}],"
      "\"snh\":4}");
}

static void testDuplicates() {
  expectResult("{\"sensors\":[{\"number\":1},{\"number\":1}]}", SENSOR_NUMBERS_DUPLICATE,
               "sensor number 1 is used twice");
  expectResult("{\"sensors\":[{},{\"number\":1}]}", SENSOR_NUMBERS_DUPLICATE,
               "sensor number 1 is used twice");  // position 1 defaults to 1
  expectResult("{\"sensors\":[{\"number\":2},{}]}", SENSOR_NUMBERS_DUPLICATE,
               "sensor number 2 is used twice");  // position 2 defaults to 2
  expectResult("{\"sensors\":[{\"number\":255},{\"number\":7},{\"number\":255}]}",
               SENSOR_NUMBERS_DUPLICATE, "sensor number 255 is used twice");
}

static void testInvalid() {
  static const char *const kBadNumbers[] = {"0", "256", "-1", "1.5", "\"1\"", "true", "false",
                                            "[]", "{}", "1000000", "-256"};
  for (size_t i = 0; i < sizeof(kBadNumbers) / sizeof(kBadNumbers[0]); ++i) {
    char json[96];
    snprintf(json, sizeof(json), "{\"sensors\":[{\"number\":1},{\"number\":%s}]}", kBadNumbers[i]);
    expectResult(json, SENSOR_NUMBERS_INVALID, "sensor 2: number must be a whole number 1-255");
  }
  expectResult("{\"sensors\":[{\"number\":0}]}", SENSOR_NUMBERS_INVALID,
               "sensor 1: number must be a whole number 1-255");
  expectResult("{\"sensors\":[{\"number\":1},5]}", SENSOR_NUMBERS_INVALID,
               "sensor 2 is not an object");
  expectResult("{\"sensors\":[null]}", SENSOR_NUMBERS_INVALID, "sensor 1 is not an object");
  expectResult("{\"sensors\":[[]]}", SENSOR_NUMBERS_INVALID, "sensor 1 is not an object");
  // The first problem in array order is the one reported.
  expectResult("{\"sensors\":[{\"number\":4},{\"number\":4},{\"number\":0}]}",
               SENSOR_NUMBERS_DUPLICATE, "sensor number 4 is used twice");
}

static void testMessageBuffer() {
  JsonDocument doc;
  CHECK(!deserializeJson(doc, "{\"sensors\":[{\"number\":1},{\"number\":1}]}"));
  const JsonArrayConst sensors = doc["sensors"].as<JsonArrayConst>();

  // An exact-size heap buffer, so AddressSanitizer reports any write past msgLen. volatile keeps
  // the length out of the optimizer's view (a constant would trigger -Wformat-truncation).
  volatile size_t smallLen = 8;
  char *small = new char[smallLen];
  CHECK(sensorNumbersCheck(sensors, small, smallLen) == SENSOR_NUMBERS_DUPLICATE);
  CHECK(strcmp(small, "sensor ") == 0);  // truncated and NUL-terminated
  delete[] small;

  char one[1] = {'x'};
  volatile size_t oneLen = 1;
  CHECK(sensorNumbersCheck(sensors, one, oneLen) == SENSOR_NUMBERS_DUPLICATE);
  CHECK(one[0] == '\0');

  // No message buffer at all: the result is still right.
  CHECK(sensorNumbersCheck(sensors, nullptr, 0) == SENSOR_NUMBERS_DUPLICATE);
  char untouched[4] = {'x', 'x', 'x', 'x'};
  CHECK(sensorNumbersCheck(sensors, untouched, 0) == SENSOR_NUMBERS_DUPLICATE);
  CHECK(untouched[0] == 'x');

  // An accepted config clears the message.
  JsonDocument okDoc;
  CHECK(!deserializeJson(okDoc, "{\"sensors\":[{\"number\":1}]}"));
  char msg[16];
  memset(msg, 'x', sizeof(msg));
  CHECK(sensorNumbersCheck(okDoc["sensors"].as<JsonArrayConst>(), msg, sizeof(msg)) ==
        SENSOR_NUMBERS_OK);
  CHECK(msg[0] == '\0');
}

int main() {
  testAccepted();
  testDuplicates();
  testInvalid();
  testMessageBuffer();
  printf("sensor_numbers: %lu checks, %lu failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
