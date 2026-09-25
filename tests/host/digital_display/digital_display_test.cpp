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
  CHECK(strcmp(dailyReconcileAlarmType(nullptr, "digital", true, nullptr), "triggered") == 0);
  // Floats latch only on the high channel: a lone low latch keeps "low" whatever the config says.
  CHECK(strcmp(dailyReconcileAlarmType("", "digital", false, ""), "low") == 0);
  CHECK(strcmp(dailyReconcileAlarmType("", "digital", false, "not_activated"), "low") == 0);
  CHECK(strcmp(dailyReconcileAlarmType(nullptr, "digital", false, nullptr), "low") == 0);
  CHECK(strcmp(dailyReconcileAlarmType("not_triggered", "digital", false, ""), "not_triggered") == 0);  // y still wins
  // CR-10a: y is trusted only for a digital or not-yet-known type; a stray float type on a known
  // non-digital sensor falls through to high/low.
  CHECK(strcmp(dailyReconcileAlarmType("triggered", "analog", false, ""), "low") == 0);
  CHECK(strcmp(dailyReconcileAlarmType("triggered", "currentLoop", true, ""), "high") == 0);
  CHECK(strcmp(dailyReconcileAlarmType("not_triggered", "currentLoop", false, "not_activated"), "low") == 0);
  CHECK(strcmp(dailyReconcileAlarmType("triggered", "pulse", true, ""), "high") == 0);
  CHECK(strcmp(dailyReconcileAlarmType("triggered", nullptr, false, ""), "triggered") == 0);  // unknown type
  CHECK(strcmp(dailyReconcileAlarmType("", "analog", true, "not_activated"), "high") == 0);     // unchanged
  CHECK(strcmp(dailyReconcileAlarmType("", "currentLoop", false, ""), "low") == 0);             // unchanged
  CHECK(strcmp(dailyReconcileAlarmType("high", "analog", false, ""), "low") == 0);              // y only for float types
  CHECK(strcmp(dailyReconcileAlarmType(nullptr, nullptr, true, nullptr), "high") == 0);
}

static void testReconcileSensorType() {
  CHECK(strcmp(dailyReconcileSensorType("digital", "analog", "currentLoop"), "digital") == 0);  // report st first
  CHECK(strcmp(dailyReconcileSensorType("analog", "digital", "digital"), "analog") == 0);
  CHECK(strcmp(dailyReconcileSensorType("", "digital", "analog"), "digital") == 0);             // then the config
  CHECK(strcmp(dailyReconcileSensorType(nullptr, "current", "digital"), "current") == 0);
  CHECK(strcmp(dailyReconcileSensorType("", "", "digital"), "digital") == 0);                   // then the record
  CHECK(strcmp(dailyReconcileSensorType(nullptr, nullptr, "analog"), "analog") == 0);
  CHECK(strcmp(dailyReconcileSensorType("", "", ""), "") == 0);
  CHECK(strcmp(dailyReconcileSensorType(nullptr, nullptr, nullptr), "") == 0);
  // Copilot's cases: a float whose record is empty or stale is classified from the report or config.
  CHECK(strcmp(dailyReconcileAlarmType("", dailyReconcileSensorType("", "digital", ""), true, "not_activated"),
               "not_triggered") == 0);
  CHECK(strcmp(dailyReconcileAlarmType("", dailyReconcileSensorType("digital", "", "analog"), true, ""),
               "triggered") == 0);
  // A record still typed "digital" whose sensor is now analog keeps high/low.
  CHECK(strcmp(dailyReconcileAlarmType("", dailyReconcileSensorType("analog", "", "digital"), false, ""), "low") == 0);
  CHECK(strcmp(dailyReconcileAlarmType("", dailyReconcileSensorType("", "current", "digital"), true, ""), "high") == 0);
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

// Mirrors handleDaily's call site (pinned in testSketchText): the booleans are computed from the
// report's sensors[] entry exactly as the sketch does.
static bool dailyAdmits(const char *json, const char *sensorType, bool trustLevel = true) {
  JsonDocument doc;
  deserializeJson(doc, json);
  JsonObjectConst t = doc.as<JsonObjectConst>();
  float mA = 0.0f;
  if (t["ma"]) {
    mA = t["ma"].as<float>();
  } else if (t["sensorMa"]) {
    mA = t["sensorMa"].as<float>();
  }
  const double sensorEpoch = t["t"] | 0.0;
  const bool dailyMaPresent = !t["ma"].isNull() || !t["sensorMa"].isNull();
  const bool dailyMaInRange = (mA >= 4.0f && mA <= 20.0f);
  const bool dailyValuePresent = !t["lvl"].isNull() || !t["fl"].isNull() || !t["rm"].isNull();
  return dailyReadingAdmissible(sensorType, sensorEpoch > 0.0, trustLevel, dailyMaPresent,
                                dailyMaInRange, dailyValuePresent);
}

// R03: a real 0 is data (admitted); a missing reading is a gap (never filled).
static void testDailyAdmission() {
  // Real zeros.
  CHECK(dailyAdmits("{\"k\":1,\"st\":\"currentLoop\",\"ma\":4.00,\"t\":1758700000}", "currentLoop"));  // empty tank, level 0
  CHECK(dailyAdmits("{\"k\":1,\"st\":\"digital\",\"fl\":0,\"lvl\":0,\"t\":1758700000}", "digital"));  // float OFF
  CHECK(dailyAdmits("{\"k\":1,\"st\":\"digital\",\"fl\":0,\"t\":1758700000}", "digital"));
  CHECK(dailyAdmits("{\"k\":1,\"st\":\"analog\",\"lvl\":0,\"t\":1758700000}", "analog"));             // 0 psi
  CHECK(dailyAdmits("{\"k\":1,\"st\":\"pulse\",\"rm\":0,\"lvl\":0,\"t\":1758700000}", "pulse"));     // engine stopped
  CHECK(dailyAdmits("{\"k\":1,\"sensorMa\":20.0,\"t\":1758700000}", "currentLoop"));
  CHECK(dailyAdmits("{\"k\":1,\"lvl\":0,\"t\":1758700000}", ""));                                    // st unknown yet
  CHECK(dailyAdmits("{\"k\":1,\"lvl\":12.5,\"t\":1758700000}", "analog"));
  // Missing readings.
  CHECK(!dailyAdmits("{\"k\":1,\"st\":\"analog\",\"lvl\":0}", "analog"));                           // no t
  CHECK(!dailyAdmits("{\"k\":1,\"st\":\"digital\",\"fl\":1,\"t\":0}", "digital"));
  CHECK(!dailyAdmits("{\"k\":1,\"st\":\"currentLoop\",\"ma\":4.00}", "currentLoop"));               // no t
  CHECK(!dailyAdmits("{\"k\":1,\"st\":\"currentLoop\",\"fault\":\"loop_open\",\"ma_raw\":0.2,\"t\":1758700000}",
                     "currentLoop"));                                                                // failed read
  CHECK(!dailyAdmits("{\"k\":1,\"st\":\"currentLoop\",\"lvl\":0,\"t\":1758700000}", "currentLoop"));  // lvl without ma
  CHECK(!dailyAdmits("{\"k\":1,\"ma\":3.8,\"t\":1758700000}", "currentLoop"));                        // under range
  CHECK(!dailyAdmits("{\"k\":1,\"ma\":20.5,\"t\":1758700000}", "currentLoop"));                       // over range
  CHECK(!dailyAdmits("{\"k\":1,\"st\":\"analog\",\"vt\":4.9,\"t\":1758700000}", "analog"));           // no value
  CHECK(!dailyAdmits("{\"k\":1,\"st\":\"analog\",\"lvl\":null,\"t\":1758700000}", "analog"));
  CHECK(!dailyAdmits("{\"k\":1,\"ma\":12.0,\"t\":1758700000}", "currentLoop", false));             // Fix 8 distrust
  CHECK(!dailyReadingAdmissible(nullptr, true, true, false, false, false));
  CHECK(dailyReadingAdmissible(nullptr, true, true, false, false, true));
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
  // A float is exempt even with no cached config snapshot.
  CHECK(strstr(text, "bool stuckDisabledInConfig = isDigitalSensorType(rec->sensorType);") != nullptr);
  CHECK(strstr(text, "if (alarmNoteCarriesValue(doc.as<JsonObjectConst>())) {") != nullptr);
  // The missed-alarm reconcile classifies by the effective type (report st, config, record) and
  // never stores it in the record.
  CHECK(strstr(text, "dailyReconcileAlarmType(a[\"y\"] | \"\", effType, hiAlarm, cfgTrigger)") != nullptr);
  CHECK(strstr(text, "dailyReconcileAlarmType(a[\"y\"] | \"\", rec->sensorType,") == nullptr);
  CHECK(strstr(text, "const char *effType = dailyReconcileSensorType(reportSt, cfgSensor, rec->sensorType);") != nullptr);
  // reportSt comes from this report's sensors[] entry for the alarm's k.
  CHECK(strstr(text, "if (t[\"k\"].is<int>() && t[\"k\"].as<int>() == sensorIdx) {\n"
                     "              reportSt = t[\"st\"] | \"\";") != nullptr);
  CHECK(strstr(text, "configSensorAndTriggerFor(clientUid, sensorIdx, cfgSensor, sizeof(cfgSensor), cfgTrigger,") != nullptr);
  CHECK(strstr(text, "configDigitalTriggerFor(") == nullptr);
  CHECK(strstr(text, "strlcpy(sensorOut, ct[\"sensor\"] | \"\", sensorLen);") != nullptr);
  CHECK(strstr(text, "strlcpy(triggerOut, ct[\"digitalTrigger\"] | \"\", triggerLen);") != nullptr);
  CHECK(strstr(text, "obj[\"sensorType\"] = \"digital\";") != nullptr);
  // Float texts show the switch state (ON/OFF), not a reading and unit. The name before each
  // tail comes from composeSensorText (pinned in tests/host/sensor_name).
  CHECK(strstr(text, "snprintf(tail, sizeof(tail), \" Float Switch clear (%s)\", digitalStateText(rec->currentValue));") != nullptr);
  CHECK(strstr(text, "snprintf(tail, sizeof(tail), \" still in %s alarm (%s)\",\n"
                     "               type, digitalStateText(rec.currentValue));") != nullptr);  // reminder
  // Snooze/resume notice: the float's state is the reading composeSnoozeText prints in "(%s)"
  // (TankAlarm_SensorName.h, tested in tests/host/sensor_name).
  CHECK(strstr(text, "    strlcpy(reading, digitalStateText(rec.currentValue), sizeof(reading));\n") != nullptr);
  CHECK(strstr(text, "composeSnoozeText(message, sizeof(message), snoozed, rec.site, rec.label, rec.userNumber, who,\n"
                     "                    rec.alarmType, reading);") != nullptr);
  CHECK(strstr(text, "  rec->currentValue = level;\n  // S-D03") == nullptr);  // the unconditional write is gone
  // R03: handleDaily admits a daily reading by value presence (the helper above), never by value.
  CHECK(strstr(text, "newLevel > 0.0f") == nullptr);
  CHECK(strstr(text, "    const bool dailyMaPresent = !t[\"ma\"].isNull() || !t[\"sensorMa\"].isNull();\n"
                     "    const bool dailyMaInRange = (mA >= 4.0f && mA <= 20.0f);\n"
                     "    const bool dailyValuePresent = !t[\"lvl\"].isNull() || !t[\"fl\"].isNull() || !t[\"rm\"].isNull();\n"
                     "    if (dailyReadingAdmissible(rec->sensorType, sensorEpoch > 0.0, trustLevel, dailyMaPresent,\n"
                     "                               dailyMaInRange, dailyValuePresent)) {") != nullptr);
  CHECK(strstr(text, "    double sensorEpoch = t[\"t\"] | 0.0;") != nullptr);
  // R02: handleDaily's sensors[] loop never moves lastUpdateEpoch back. Its only write in the loop
  // (from its upsert to the history snapshot) is the guarded one; a record the loop's own upsert
  // just made (server clock, not a reading) takes `now` outright.
  CHECK(strstr(text, "    if (trustLevel) {\n      rec->currentValue = newLevel;\n    }\n") != nullptr);
  CHECK(strstr(text, "\n    if (recCreated || now > rec->lastUpdateEpoch) rec->lastUpdateEpoch = now;\n    gSensorRegistryDirty = true;\n") != nullptr);
  CHECK(strstr(text, "\n    rec->lastUpdateEpoch = now;\n") == nullptr);
  CHECK(strstr(text, "    bool recCreated = false;\n"
                     "    SensorRecord *rec = upsertSensorRecord(clientUid, sensorIndex, &recCreated);\n") != nullptr);
  {
    const char *loopBegin = strstr(text, "    SensorRecord *rec = upsertSensorRecord(clientUid, sensorIndex, &recCreated);\n");
    const char *loopEnd = loopBegin ? strstr(loopBegin, "      recordTelemetrySnapshot(clientUid, siteName, sensorIndex,") : nullptr;
    CHECK(loopBegin != nullptr && loopEnd != nullptr);
    if (loopBegin && loopEnd) {
      int writes = 0;
      int guarded = 0;
      static const char kWrite[] = "rec->lastUpdateEpoch = ";
      static const char kGuard[] = "if (recCreated || now > rec->lastUpdateEpoch) ";
      for (const char *p = strstr(loopBegin, kWrite); p && p < loopEnd; p = strstr(p + 1, kWrite)) {
        ++writes;
        const size_t g = sizeof(kGuard) - 1;
        if ((size_t)(p - loopBegin) >= g && strncmp(p - g, kGuard, g) == 0) ++guarded;
      }
      CHECK(writes == 1);
      CHECK(guarded == 1);
    }
  }
  // R02 (reconcile): the missed-alarm reconcile takes no reading, so it no longer stamps the
  // report time over an existing record's time (the loop above could not lower it again). Its
  // only lastUpdateEpoch write is for a record it just created: this part's `t` for k, else the
  // report time.
  CHECK(strstr(text, "\n          rec->lastUpdateEpoch = (epoch > 0.0) ? epoch : currentEpoch();\n") == nullptr);
  CHECK(strstr(text, "              reportSensorEpoch = t[\"t\"] | 0.0;\n") != nullptr);
  CHECK(strstr(text, "          if (recCreated) {\n"
                     "            rec->lastUpdateEpoch = (reportSensorEpoch > 0.0)\n"
                     "                                       ? reportSensorEpoch\n"
                     "                                       : ((epoch > 0.0) ? epoch : currentEpoch());\n"
                     "          }\n") != nullptr);
  {
    const char *recBegin = strstr(text, "        SensorRecord *rec = (sensorIdx >= 1) ? upsertSensorRecord(clientUid, sensorIdx, &recCreated) : nullptr;\n");
    const char *recEnd = recBegin ? strstr(recBegin, "    // Reconciliation: clear alarms on server for sensors that the client") : nullptr;
    CHECK(recBegin != nullptr && recEnd != nullptr);
    if (recBegin && recEnd) {
      int writes = 0;
      static const char kWrite[] = "lastUpdateEpoch = ";
      for (const char *p = strstr(recBegin, kWrite); p && p < recEnd; p = strstr(p + 1, kWrite)) ++writes;
      CHECK(writes == 1);
    }
  }
  // upsertSensorRecord reports a new record, set only on the creation path.
  CHECK(strstr(text, "static SensorRecord *upsertSensorRecord(const char *clientUid, uint8_t sensorIndex, bool *created = nullptr);") != nullptr);
  CHECK(strstr(text, "static SensorRecord *upsertSensorRecord(const char *clientUid, uint8_t sensorIndex, bool *created) {\n") != nullptr);
  CHECK(strstr(text, "  if (created) *created = false;\n") != nullptr);
  CHECK(strstr(text, "  insertSensorIntoHash(newIndex);\n  gSensorRegistryDirty = true;\n  if (created) *created = true;\n") != nullptr);
  // R11: handleDaily stores the raw mA, like handleTelemetry/handleAlarm; the >=4.0 clamp is gone.
  CHECK(strstr(text, "(mA >= 4.0f) ? mA : 0.0f") == nullptr);
  CHECK(strstr(text, "      mA = t[\"ma\"].as<float>();\n      rec->sensorMa = mA;\n") != nullptr);
  CHECK(strstr(text, "      mA = t[\"sensorMa\"].as<float>();\n      rec->sensorMa = mA;\n") != nullptr);
  CHECK(strstr(text, "    } else if (strcmp(rec->sensorType, \"currentLoop\") == 0) {\n"
                     "      // Fix 13: a current-loop daily with NO raw mA") != nullptr);
  // R10: the missed-alarm reconcile creates a missing record (not a search-only lookup) and fills
  // an empty site from the note.
  CHECK(strstr(text, "search without upserting") == nullptr);
  CHECK(strstr(text, "SensorRecord *rec = (sensorIdx >= 1) ? upsertSensorRecord(clientUid, sensorIdx, &recCreated) : nullptr;\n"
                     "        if (rec && rec->site[0] == '\\0') {\n"
                     "          const char *noteSite = doc[\"s\"] | \"\";\n"
                     "          if (noteSite[0] != '\\0') strlcpy(rec->site, noteSite, sizeof(rec->site));\n"
                     "        }\n"
                     "        if (rec && !rec->alarmActive) {") != nullptr);
}

int main() {
  testState();
  testReconcileType();
  testReconcileSensorType();
  testNoteValue();
  testStuckDisabled();
  testDailyAdmission();
  testSketchText();
  if (gFailures) {
    printf("digital_display: %lu of %lu checks FAILED\n", gFailures, gChecks);
    return 1;
  }
  printf("digital_display: all %lu checks passed\n", gChecks);
  return 0;
}
