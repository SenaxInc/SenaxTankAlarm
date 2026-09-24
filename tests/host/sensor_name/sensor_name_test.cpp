// Host tests for TankAlarm-112025-Server-BluesOpta/TankAlarm_SensorName.h: how the server keeps a
// sensor's Display Number ("un") in step with telemetry and daily-report notes, and the sketch
// text that uses it (the daily email's "#N" and the dashboard's Latest line).
//
//   make -C tests/host/sensor_name test

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "TankAlarm_SensorName.h"

#ifndef SERVER_SKETCH
#define SERVER_SKETCH "../../../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino"
#endif

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

static void testNoteDisplayNumber() {
  CHECK(noteDisplayNumber(true, 5, true, 0) == 5);      // set
  CHECK(noteDisplayNumber(true, 0, true, 5) == 0);      // explicit 0 clears
  CHECK(noteDisplayNumber(false, 0, true, 5) == 0);     // telemetry/daily without "un": cleared on the client
  CHECK(noteDisplayNumber(false, 0, false, 5) == 5);    // a note that need not carry "un" keeps it
  CHECK(noteDisplayNumber(true, 300, true, 5) == 5);    // out of range: not a Display Number
  CHECK(noteDisplayNumber(true, -1, true, 5) == 5);
  CHECK(noteDisplayNumber(true, 255, true, 5) == 255);  // range ends
  CHECK(noteDisplayNumber(true, 256, true, 5) == 5);
  CHECK(noteDisplayNumber(true, 1, false, 0) == 1);     // present "un" wins on any note
  CHECK(noteDisplayNumber(true, 7, true, 7) == 7);      // unchanged
  CHECK(noteDisplayNumber(false, 0, true, 0) == 0);
  CHECK(noteDisplayNumber(true, INT32_MIN, true, 9) == 9);
  CHECK(noteDisplayNumber(true, INT32_MAX, true, 9) == 9);
}

// strstr from `from`, or nullptr when `from` was not found.
static const char *findAfter(const char *needle, const char *from) {
  return (from != nullptr) ? strstr(from, needle) : nullptr;
}

static void testSketchText() {
  FILE *f = fopen(SERVER_SKETCH, "rb");
  CHECK(f != nullptr);
  if (!f) return;
  static char text[2 << 20];
  const size_t n = fread(text, 1, sizeof(text) - 1, f);
  fclose(f);
  text[n] = '\0';
  CHECK(n > 500000 && n < sizeof(text) - 1);
  CHECK(strstr(text, "#include \"TankAlarm_SensorName.h\"") != nullptr);

  // handleTelemetry and handleDaily's sensors[] loop use the helper, with noteAlwaysCarriesUn = true.
  static const char kTelemetryCall[] =
      "    JsonVariantConst un = doc[\"un\"];\n"
      "    const uint8_t displayNumber = noteDisplayNumber(!un.isNull(), un.is<int32_t>() ? un.as<int32_t>() : -1,\n"
      "                                                    true, rec->userNumber);\n";
  static const char kDailyCall[] =
      "      JsonVariantConst un = t[\"un\"];\n"
      "      const uint8_t displayNumber = noteDisplayNumber(!un.isNull(), un.is<int32_t>() ? un.as<int32_t>() : -1,\n"
      "                                                      true, rec->userNumber);\n";
  const char *telemetry = strstr(text, "static void handleTelemetry(JsonDocument &doc, double epoch) {");
  const char *alarm = strstr(text, "static void handleAlarm(JsonDocument &doc, double epoch) {");
  const char *daily = strstr(text, "static void handleDaily(JsonDocument &doc, double epoch) {");
  CHECK(telemetry != nullptr && alarm != nullptr && daily != nullptr);
  const char *telemetryCall = findAfter(kTelemetryCall, telemetry);
  CHECK(telemetryCall != nullptr && alarm != nullptr && telemetryCall < alarm);
  const char *dailyCall = findAfter(kDailyCall, daily);
  CHECK(dailyCall != nullptr);
  const char *sensorsLoop = findAfter("  JsonArray sensors = doc[\"sensors\"];\n  for (JsonObject t : sensors) {", daily);
  CHECK(sensorsLoop != nullptr && dailyCall != nullptr && sensorsLoop < dailyCall);
  // Exactly two call sites.
  unsigned calls = 0;
  for (const char *p = strstr(text, "noteDisplayNumber("); p != nullptr; p = strstr(p + 1, "noteDisplayNumber(")) {
    ++calls;
  }
  CHECK(calls == 2);
  // The presence-only writes are gone from telemetry and daily; handleAlarm keeps its own.
  CHECK(strstr(text, "    if (t.containsKey(\"un\")) {\n      rec->userNumber = t[\"un\"].as<uint8_t>();") == nullptr);
  const char *alarmWrite = findAfter("  if (doc.containsKey(\"un\")) {\n    rec->userNumber = doc[\"un\"].as<uint8_t>();\n  }", telemetry);
  CHECK(alarmWrite != nullptr && alarm != nullptr && daily != nullptr && alarmWrite > alarm && alarmWrite < daily);
  if (alarmWrite != nullptr) {
    CHECK(strstr(alarmWrite + 1, "  if (doc.containsKey(\"un\")) {\n    rec->userNumber = doc[\"un\"].as<uint8_t>();\n  }") == nullptr);
  }

  // sendDailyEmail still sends sensorIndex (old pasted scripts print '#undefined' without it) and
  // userNumber only when set.
  const char *email = strstr(text, "static void sendDailyEmail() {");
  CHECK(email != nullptr);
  CHECK(findAfter("    obj[\"sensorIndex\"] = gSensorRecords[i].sensorIndex;\n"
                  "    if (gSensorRecords[i].userNumber > 0) {\n"
                  "      obj[\"userNumber\"] = gSensorRecords[i].userNumber;\n"
                  "    }\n", email) != nullptr);

  // Dashboard "Latest" line: the client-level "un" follows the same sensor as "n"/"k".
  CHECK(strstr(text, "      clientObj[\"k\"] = rec.sensorIndex;\n"
                     "      // \"un\" follows the same sensor as \"n\"/\"k\": drop an older sensor's number when this one has none.\n"
                     "      if (rec.userNumber > 0) {\n"
                     "        clientObj[\"un\"] = rec.userNumber;\n"
                     "      } else {\n"
                     "        clientObj.remove(\"un\");\n"
                     "      }\n") != nullptr);
}

int main() {
  testNoteDisplayNumber();
  testSketchText();
  if (gFailures) {
    printf("sensor_name: %lu of %lu checks FAILED\n", gFailures, gChecks);
    return 1;
  }
  printf("sensor_name: all %lu checks passed\n", gChecks);
  return 0;
}
