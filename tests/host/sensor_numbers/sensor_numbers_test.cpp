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

// Parses json and checks its "sensors" value as handleConfigPost does (the raw variant), with a
// message buffer of msgLen bytes.
static uint8_t checkJson(const char *json, char *msg, size_t msgLen) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json);
  CHECK(!err);
  return sensorNumbersCheck(doc["sensors"], msg, msgLen);
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
  expectOk("{\"sensors\":[{\"number\":1},{\"number\":2},{\"number\":3}]}");
  expectOk("{\"sensors\":[{\"number\":2},{\"number\":1},{\"number\":3}]}");
  expectOk("{\"sensors\":[{\"number\":1},{\"number\":3},{\"number\":4}]}");  // #2 removed, #4 added
  expectOk("{\"sensors\":[{\"number\":255}]}");
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
  // A present "sensors" that is not a list: converting it to an array would give a null array.
  static const char *const kNotLists[] = {"{\"number\":0}", "{}", "\"1\"", "5", "true", "false"};
  for (size_t i = 0; i < sizeof(kNotLists) / sizeof(kNotLists[0]); ++i) {
    char json[64];
    snprintf(json, sizeof(json), "{\"sensors\":%s}", kNotLists[i]);
    expectResult(json, SENSOR_NUMBERS_INVALID, "sensors must be a list");
  }
  // Every sensor needs its number: a live config update would keep the slot's old number.
  expectResult("{\"sensors\":[{},{},{}]}", SENSOR_NUMBERS_INVALID, "sensor 1 has no number");
  expectResult("{\"sensors\":[{\"number\":null}]}", SENSOR_NUMBERS_INVALID,
               "sensor 1 has no number");
  expectResult("{\"sensors\":[{\"number\":3},{}]}", SENSOR_NUMBERS_INVALID,
               "sensor 2 has no number");  // a client holding [1,3] would end up with 3,3
  expectResult("{\"sensors\":[{},{\"number\":1}]}", SENSOR_NUMBERS_INVALID,
               "sensor 1 has no number");
  expectResult("{\"sensors\":[{\"number\":2},{\"name\":\"Tank\"}]}", SENSOR_NUMBERS_INVALID,
               "sensor 2 has no number");
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
  char msg[80];  // as large as expectResult's: a constant 16 could trip -Wformat-truncation if inlined
  memset(msg, 'x', sizeof(msg));
  CHECK(sensorNumbersCheck(okDoc["sensors"].as<JsonArrayConst>(), msg, sizeof(msg)) ==
        SENSOR_NUMBERS_OK);
  CHECK(msg[0] == '\0');
}

// CR-5: the high mark stored with a posted config, as handleConfigPost computes it from the posted
// config and the cached snapshot's payload text (nullptr when the client has no snapshot).
static uint8_t highMarkFor(const char *postedJson, const char *cachedPayload) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, postedJson);
  CHECK(!err);
  const uint8_t cached = sensorNumbersCachedHigh(cachedPayload);
  return sensorNumbersHighMark(doc["snh"], cached, doc["sensors"]);
}

static void expectHighMark(const char *postedJson, const char *cachedPayload, unsigned want) {
  const unsigned got = highMarkFor(postedJson, cachedPayload);
  ++gChecks;
  if (got != want) {
    ++gFailures;
    printf("FAIL high mark of %s with cache %s: got %u, expected %u\n", postedJson,
           cachedPayload ? cachedPayload : "(none)", got, want);
  }
}

static void testHighMark() {
  static const char kCached5[] = "{\"site\":\"North Yard\",\"sensors\":[{\"number\":1},{\"number\":3}],\"snh\":5}";

  // The posted snh, when it is the largest, is kept.
  expectHighMark("{\"sensors\":[{\"number\":1},{\"number\":3}],\"snh\":7}", kCached5, 7);
  // Import of an old file without snh: the cached 5 stays, so #4 and #5 are never reused.
  expectHighMark("{\"sensors\":[{\"number\":1},{\"number\":2}]}", kCached5, 5);
  // A lower posted snh cannot lower it either.
  expectHighMark("{\"sensors\":[{\"number\":1}],\"snh\":2}", kCached5, 5);
  // The highest posted number wins over both.
  expectHighMark("{\"sensors\":[{\"number\":9},{\"number\":1}],\"snh\":2}", kCached5, 9);
  // No snapshot: posted snh and numbers only.
  expectHighMark("{\"sensors\":[{\"number\":1},{\"number\":2}],\"snh\":4}", nullptr, 4);
  expectHighMark("{\"sensors\":[{\"number\":1},{\"number\":2}]}", nullptr, 2);
  expectHighMark("{\"sensors\":[{\"number\":1},{\"number\":2}]}", "", 2);
  expectHighMark("{}", nullptr, 0);
  expectHighMark("{\"sensors\":[]}", nullptr, 0);
  expectHighMark("{\"sensors\":[],\"snh\":6}", nullptr, 6);
  expectHighMark("{\"snh\":255}", nullptr, 255);
  // A snapshot saved before snh existed: its highest number is the mark.
  expectHighMark("{\"sensors\":[{\"number\":1}]}", "{\"sensors\":[{\"number\":1},{\"number\":3}]}", 3);
  // A snapshot with neither, or unreadable: no mark from it.
  expectHighMark("{\"sensors\":[{\"number\":1}]}", "{\"site\":\"x\"}", 1);
  expectHighMark("{\"sensors\":[{\"number\":1}]}", "not json", 1);
  expectHighMark("{\"sensors\":[{\"number\":1}]}", "{\"sensors\":[{\"number\":4}", 1);
  // Unusable snh values count as none, posted or cached.
  static const char *const kBadSnh[] = {"256", "-1", "1.5", "\"9\"", "true", "null", "[]", "{}", "1000"};
  for (size_t i = 0; i < sizeof(kBadSnh) / sizeof(kBadSnh[0]); ++i) {
    char posted[96];
    char cached[96];
    snprintf(posted, sizeof(posted), "{\"sensors\":[{\"number\":2}],\"snh\":%s}", kBadSnh[i]);
    snprintf(cached, sizeof(cached), "{\"sensors\":[{\"number\":1}],\"snh\":%s}", kBadSnh[i]);
    expectHighMark(posted, cached, 2);
  }
  // Unusable numbers do not raise it (handleConfigPost refuses them before this runs anyway).
  expectHighMark("{\"sensors\":[{\"number\":300},{\"number\":1.5},{},5,{\"number\":\"9\"}]}", nullptr, 0);
  expectHighMark("{\"sensors\":{\"number\":9}}", "{\"sensors\":\"9\"}", 0);
  // The cached mark is read from anywhere in a full snapshot, with calibration injected.
  expectHighMark("{\"sensors\":[{\"number\":1}]}",
                 "{\"productUid\":\"com.example.tankalarm\",\"sensors\":[{\"number\":1,\"name\":\"Tank\","
                 "\"calibration\":{\"points\":[[1,2],[3,4]]}},{\"number\":6}],\"snh\":8,\"cv\":\"abc\"}",
                 8);

  // sensorNumbersHighMark with a missing posted snh value.
  JsonDocument doc;
  CHECK(!deserializeJson(doc, "{\"sensors\":[{\"number\":4}]}"));
  CHECK(sensorNumbersHighMark(doc["snh"], 0, doc["sensors"]) == 4);
  CHECK(sensorNumbersHighMark(doc["snh"], 200, doc["sensors"]) == 200);
  CHECK(sensorNumbersHighMark(doc["missing"], 0, doc["missing"]) == 0);
}

int main() {
  testAccepted();
  testDuplicates();
  testInvalid();
  testMessageBuffer();
  testHighMark();
  printf("sensor_numbers: %lu checks, %lu failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
