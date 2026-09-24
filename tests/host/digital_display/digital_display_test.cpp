// Host tests for TankAlarm-112025-Server-BluesOpta/TankAlarm_DigitalDisplay.h (S3, C-A04 B4-B8).
//
//   make -C tests/host/digital_display test ARDUINOJSON_DIR=/path/to/ArduinoJson/src

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ArduinoJson.h>

#include "TankAlarm_DigitalDisplay.h"

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

static bool carries(const char *json) {
  JsonDocument doc;
  deserializeJson(doc, json);
  return alarmNoteCarriesValue(doc.as<JsonObjectConst>());
}

static bool stuckOff(const char *json) {
  JsonDocument doc;
  deserializeJson(doc, json);
  return configSensorStuckDisabled(doc.as<JsonObjectConst>());
}

static void testState() {
  CHECK(strcmp(digitalStateText(1.0f), "ON") == 0);
  CHECK(strcmp(digitalStateText(0.51f), "ON") == 0);
  CHECK(strcmp(digitalStateText(0.5f), "OFF") == 0);  // same threshold as formatSwitch (> 0.5)
  CHECK(strcmp(digitalStateText(0.0f), "OFF") == 0);
  CHECK(strcmp(digitalStateText(NAN), "OFF") == 0);
  CHECK(isDigitalSensorType("digital"));
  CHECK(!isDigitalSensorType("Digital"));
  CHECK(!isDigitalSensorType("analog"));
  CHECK(!isDigitalSensorType(""));
  CHECK(!isDigitalSensorType(nullptr));
}

static void testReconcileType() {
  CHECK(strcmp(dailyReconcileAlarmType("triggered", "digital", true, ""), "triggered") == 0);
  CHECK(strcmp(dailyReconcileAlarmType("not_triggered", "digital", true, ""), "not_triggered") == 0);
  CHECK(strcmp(dailyReconcileAlarmType("not_triggered", "", true, ""), "not_triggered") == 0);  // y wins even before st is known
  CHECK(strcmp(dailyReconcileAlarmType("triggered", "digital", true, "not_activated"), "triggered") == 0);  // y beats the config
  CHECK(strcmp(dailyReconcileAlarmType("", "digital", true, ""), "triggered") == 0);            // 2.2.16 client, no config
  CHECK(strcmp(dailyReconcileAlarmType("", "digital", true, "activated"), "triggered") == 0);
  CHECK(strcmp(dailyReconcileAlarmType("", "digital", true, "not_activated"), "not_triggered") == 0);
  CHECK(strcmp(dailyReconcileAlarmType("bogus", "digital", true, "not_activated"), "not_triggered") == 0);
  CHECK(strcmp(dailyReconcileAlarmType("bogus", "digital", true, "bogus"), "triggered") == 0);
  CHECK(strcmp(dailyReconcileAlarmType(nullptr, "digital", false, nullptr), "triggered") == 0);
  CHECK(strcmp(dailyReconcileAlarmType("", "analog", true, "not_activated"), "high") == 0);     // unchanged
  CHECK(strcmp(dailyReconcileAlarmType("", "currentLoop", false, ""), "low") == 0);             // unchanged
  CHECK(strcmp(dailyReconcileAlarmType("high", "analog", false, ""), "low") == 0);              // y only for float types
  CHECK(strcmp(dailyReconcileAlarmType(nullptr, nullptr, true, nullptr), "high") == 0);
}

static void testNoteValue() {
  CHECK(carries("{\"lvl\":0}"));      // 0 is a reading
  CHECK(carries("{\"lvl\":42.5}"));
  CHECK(carries("{\"fl\":0}"));
  CHECK(carries("{\"rm\":0}"));
  CHECK(carries("{\"ma\":4.0}"));
  CHECK(carries("{\"sensorMa\":12}"));
  CHECK(!carries("{}"));
  CHECK(!carries("{\"y\":\"clear\",\"rd\":0}"));                  // config-push clear (CL:4859-4869)
  CHECK(!carries("{\"y\":\"sensor-fault\"}"));                    // CL:5484-5490
  CHECK(!carries("{\"y\":\"sensor-recovered\",\"rd\":0}"));
  CHECK(!carries("{\"fault\":\"loop_open\",\"ma_raw\":0.2}"));    // failed current-loop read
  CHECK(!carries("{\"vt\":4.9}"));                                // voltage alone is not the level
  CHECK(!carries("{\"lvl\":null}"));
}

static void testStuckDisabled() {
  CHECK(stuckOff("{\"sensor\":\"digital\",\"stuckDetection\":true}"));
  CHECK(stuckOff("{\"sensor\":\"digital\"}"));
  CHECK(!stuckOff("{\"sensor\":\"analog\",\"stuckDetection\":true}"));
  CHECK(stuckOff("{\"sensor\":\"analog\",\"stuckDetection\":false}"));
  CHECK(stuckOff("{\"sensor\":\"current\"}"));  // missing flag: unchanged meaning (disabled)
  CHECK(!stuckOff("{\"stuckDetection\":true}"));
}

// The server sketch uses the helpers where it decides (B4, B5, B6, B8) and where it writes the
// float SMS texts (B7).
static void testSketchText() {
  FILE *f = fopen(SERVER_SKETCH, "rb");
  CHECK(f != nullptr);
  if (!f) return;
  static char text[2 << 20];
  const size_t n = fread(text, 1, sizeof(text) - 1, f);
  fclose(f);
  text[n] = '\0';
  CHECK(n > 500000 && n < sizeof(text) - 1);
  CHECK(strstr(text, "#include \"TankAlarm_DigitalDisplay.h\"") != nullptr);
  CHECK(strstr(text, "stuckDisabledInConfig = configSensorStuckDisabled(ct);") != nullptr);
  CHECK(strstr(text, "if (alarmNoteCarriesValue(doc.as<JsonObjectConst>())) {") != nullptr);
  CHECK(strstr(text, "dailyReconcileAlarmType(a[\"y\"] | \"\", rec->sensorType, hiAlarm, cfgTrigger)") != nullptr);
  CHECK(strstr(text, "configDigitalTriggerFor(clientUid, sensorIdx, cfgTrigger, sizeof(cfgTrigger));") != nullptr);
  CHECK(strstr(text, "strlcpy(out, ct[\"digitalTrigger\"] | \"\", outLen);") != nullptr);
  CHECK(strstr(text, "obj[\"sensorType\"] = \"digital\";") != nullptr);
  CHECK(strstr(text, "Float Switch clear (%s)") != nullptr);
  CHECK(strstr(text, "type, digitalStateText(rec.currentValue));") != nullptr);  // reminder
  CHECK(strstr(text, "Still in %s alarm (%s).%s") != nullptr);                  // snooze/resume notice
  CHECK(strstr(text, "rec.alarmType, digitalStateText(rec.currentValue),") != nullptr);
  CHECK(strstr(text, "  rec->currentValue = level;\n  // S-D03") == nullptr);  // the unconditional write is gone
}

int main() {
  testState();
  testReconcileType();
  testNoteValue();
  testStuckDisabled();
  testSketchText();
  if (gFailures) {
    printf("digital_display: %lu of %lu checks FAILED\n", gFailures, gChecks);
    return 1;
  }
  printf("digital_display: all %lu checks passed\n", gChecks);
  return 0;
}
