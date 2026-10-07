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

// C323: the retired numbers a posted config reuses, as handleConfigPost finds them: one parse of
// the cached payload (sensorNumbersCachedState), then sensorNumbersRetiredReuse on the posted
// "sensors". wantKnown false means no usable snapshot, so handleConfigPost does not guard.
static void expectRetired(const char *postedJson, const char *cachedPayload, bool wantKnown,
                          unsigned wantHigh, size_t wantCount, const char *wantList) {
  JsonDocument doc;
  CHECK(!deserializeJson(doc, postedJson));
  uint8_t high = 77;
  uint8_t active[32];
  memset(active, 0xA5, sizeof(active));
  const bool known = sensorNumbersCachedState(cachedPayload, high, active);
  char list[64];
  memset(list, 'x', sizeof(list));
  const size_t count = known ? sensorNumbersRetiredReuse(doc["sensors"], active, high, list, sizeof(list)) : 0;
  if (!known) list[0] = '\0';
  ++gChecks;
  if (known != wantKnown || high != wantHigh || count != wantCount || strcmp(list, wantList) != 0) {
    ++gFailures;
    printf("FAIL retired of %s with cache %s: got %d/%u/%u \"%s\", expected %d/%u/%u \"%s\"\n", postedJson,
           cachedPayload ? cachedPayload : "(none)", (int)known, (unsigned)high, (unsigned)count, list,
           (int)wantKnown, wantHigh, (unsigned)wantCount, wantList);
  }
  // The cached high mark is the one sensorNumbersHighMark gets: the same as sensorNumbersCachedHigh.
  CHECK(high == sensorNumbersCachedHigh(cachedPayload));
  // An unknown snapshot leaves the active set empty.
  if (!known) {
    bool empty = true;
    for (size_t i = 0; i < sizeof(active); ++i) empty = empty && active[i] == 0;
    CHECK(empty);
  }
}

static void testRetiredReuse() {
  // The cached config holds #1, #2, #4 and once used #5 (snh 5): #3 and #5 are retired.
  static const char kCached[] = "{\"site\":\"North Yard\",\"sensors\":[{\"number\":1},{\"number\":2},{\"number\":4}],\"snh\":5}";
  expectRetired("{\"sensors\":[{\"number\":1},{\"number\":2},{\"number\":3}]}", kCached, true, 5, 1, "3");
  expectRetired("{\"sensors\":[{\"number\":5},{\"number\":1},{\"number\":2},{\"number\":3}]}", kCached, true, 5, 2, "3, 5");
  // New numbers past the mark, a subset of the active set, and an empty list are not reuse.
  expectRetired("{\"sensors\":[{\"number\":1},{\"number\":2},{\"number\":4},{\"number\":6}]}", kCached, true, 5, 0, "");
  expectRetired("{\"sensors\":[{\"number\":1},{\"number\":2}]}", kCached, true, 5, 0, "");
  expectRetired("{\"sensors\":[]}", kCached, true, 5, 0, "");
  expectRetired("{}", kCached, true, 5, 0, "");
  expectRetired("{\"sensors\":{\"number\":3}}", kCached, true, 5, 0, "");
  // Unusable posted entries are skipped (sensorNumbersCheck refuses them first).
  expectRetired("{\"sensors\":[5,{\"number\":0},{\"number\":3.5},{\"number\":\"3\"},{\"number\":300},{}]}", kCached,
                true, 5, 0, "");
  // A snapshot saved before snh existed: its highest number is the mark, gaps below it are retired.
  expectRetired("{\"sensors\":[{\"number\":1},{\"number\":2}]}", "{\"sensors\":[{\"number\":1},{\"number\":3}]}",
                true, 3, 1, "2");
  // Every sensor removed: all numbers up to the mark are retired.
  expectRetired("{\"sensors\":[{\"number\":2},{\"number\":1}]}", "{\"sensors\":[],\"snh\":4}", true, 4, 2, "1, 2");
  // Unusable cached entries count as not in use.
  expectRetired("{\"sensors\":[{\"number\":1}]}", "{\"sensors\":[{\"number\":\"1\"},{\"number\":2}],\"snh\":2}",
                true, 2, 1, "1");
  // No usable snapshot: no guard (commissioning), but the mark is still read where there is one.
  expectRetired("{\"sensors\":[{\"number\":1}]}", nullptr, false, 0, 0, "");
  expectRetired("{\"sensors\":[{\"number\":1}]}", "", false, 0, 0, "");
  expectRetired("{\"sensors\":[{\"number\":1}]}", "not json", false, 0, 0, "");
  expectRetired("{\"sensors\":[{\"number\":1}]}", "{\"sensors\":[{\"number\":4}", false, 0, 0, "");
  expectRetired("{\"sensors\":[{\"number\":1}]}", "{\"site\":\"x\",\"snh\":6}", false, 6, 0, "");
  expectRetired("{\"sensors\":[{\"number\":1}]}", "{\"sensors\":\"1,2\",\"snh\":6}", false, 6, 0, "");
  expectRetired("{\"sensors\":[{\"number\":1}]}", "{\"sensors\":null,\"snh\":3}", false, 3, 0, "");
  // A full snapshot with calibration injected.
  expectRetired("{\"sensors\":[{\"number\":1},{\"number\":2},{\"number\":7}]}",
                "{\"productUid\":\"com.example.tankalarm\",\"sensors\":[{\"number\":1,\"name\":\"Tank\","
                "\"calibration\":{\"points\":[[1,2],[3,4]]}},{\"number\":6}],\"snh\":8,\"cv\":\"abc\"}",
                true, 8, 2, "2, 7");
  // At 255: the numbers above 248 are retired, listed in ascending order whatever the posted order.
  expectRetired("{\"sensors\":[{\"number\":255},{\"number\":250},{\"number\":1}]}",
                "{\"sensors\":[{\"number\":1}],\"snh\":255}", true, 255, 2, "250, 255");

  // The list buffer: whole numbers only, always NUL-terminated, never past listLen.
  uint8_t high = 0;
  uint8_t active[32];
  CHECK(sensorNumbersCachedState("{\"sensors\":[],\"snh\":255}", high, active));
  CHECK(high == 255);
  JsonDocument posted;
  CHECK(!deserializeJson(posted, "{\"sensors\":[{\"number\":100},{\"number\":7},{\"number\":255},{\"number\":12},"
                                 "{\"number\":1},{\"number\":200},{\"number\":30},{\"number\":254}]}"));
  char big[64];
  memset(big, 'x', sizeof(big));
  CHECK(sensorNumbersRetiredReuse(posted["sensors"], active, high, big, sizeof(big)) == 8);
  CHECK(strcmp(big, "1, 7, 12, 30, 100, 200, 254, 255") == 0);
  // Exact-size heap buffers, so AddressSanitizer reports any write past listLen.
  volatile size_t fitLen = 9;  // "1, 7, 12" is 8 bytes plus the NUL
  char *fit = new char[fitLen];
  CHECK(sensorNumbersRetiredReuse(posted["sensors"], active, high, fit, fitLen) == 8);
  CHECK(strcmp(fit, "1, 7, 12") == 0);
  delete[] fit;
  volatile size_t shortLen = 8;  // one byte short of "1, 7, 12": ", 12" is left out whole
  char *shortList = new char[shortLen];
  CHECK(sensorNumbersRetiredReuse(posted["sensors"], active, high, shortList, shortLen) == 8);
  CHECK(strcmp(shortList, "1, 7") == 0);
  delete[] shortList;
  char one[1] = {'x'};
  volatile size_t oneLen = 1;
  CHECK(sensorNumbersRetiredReuse(posted["sensors"], active, high, one, oneLen) == 8);
  CHECK(one[0] == '\0');
  char untouched[4] = {'x', 'x', 'x', 'x'};
  CHECK(sensorNumbersRetiredReuse(posted["sensors"], active, high, untouched, 0) == 8);
  CHECK(untouched[0] == 'x');
  CHECK(sensorNumbersRetiredReuse(posted["sensors"], active, high, nullptr, 0) == 8);
  // A cached high of 0 (no mark) retires nothing.
  CHECK(sensorNumbersRetiredReuse(posted["sensors"], active, 0, big, sizeof(big)) == 0);
  CHECK(big[0] == '\0');
}

int main() {
  testAccepted();
  testDuplicates();
  testInvalid();
  testMessageBuffer();
  testHighMark();
  testRetiredReuse();
  printf("sensor_numbers: %lu checks, %lu failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
