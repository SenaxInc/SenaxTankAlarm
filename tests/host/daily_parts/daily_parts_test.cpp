// Host tests for TankAlarm-112025-Server-BluesOpta/TankAlarm_DailyParts.h (S-D08).
//
//   make -C tests/host/daily_parts test ARDUINOJSON_DIR=/path/to/ArduinoJson/src
//
// The helper decides which part of a multi-part daily report is its first part. The tests cover
// the helper's truth table, the decisions handleDaily() takes from it (voltage, signal, alarm
// reconcile) on notes built the way the client builds them, and the sketch lines those
// decisions mirror. The sketch paths can be overridden: daily_parts_test [server.ino [client.ino]].

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ArduinoJson.h>

#include "TankAlarm_DailyParts.h"

#ifndef SERVER_SKETCH
#define SERVER_SKETCH "../../../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino"
#endif
#ifndef CLIENT_SKETCH
#define CLIENT_SKETCH "../../../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino"
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

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

// ---------------------------------------------------------------------------------------------
// The helper's truth table.
// ---------------------------------------------------------------------------------------------

static void testTruthTable() {
  // A note without a schema version: parts 0 and 1, as before.
  CHECK(dailyIsFirstPart(0, 0));
  CHECK(dailyIsFirstPart(0, 1));
  CHECK(!dailyIsFirstPart(0, 2));
  CHECK(!dailyIsFirstPart(0, 3));
  CHECK(!dailyIsFirstPart(0, 255));

  // A note with a schema version: only part 0.
  CHECK(dailyIsFirstPart(1, 0));
  CHECK(!dailyIsFirstPart(1, 1));
  CHECK(!dailyIsFirstPart(1, 2));
  CHECK(!dailyIsFirstPart(1, 255));
  CHECK(dailyIsFirstPart(2, 0));
  CHECK(!dailyIsFirstPart(2, 1));
  CHECK(!dailyIsFirstPart(2, 2));
  CHECK(!dailyIsFirstPart(2, 255));

  // Every part number, for schema versions around the boundary and at the extremes. A
  // negative version is not a schema version, so it reads as a note without one.
  static const int kSchemas[] = {INT_MIN, -2, -1, 0, 1, 2, 3, 7, 255, 256, 65536, INT_MAX};
  for (size_t si = 0; si < COUNT_OF(kSchemas); ++si) {
    const int schema = kSchemas[si];
    for (int p = 0; p <= 255; ++p) {
      const uint8_t part = static_cast<uint8_t>(p);
      const bool expected = (schema >= 1) ? (p == 0) : (p == 0 || p == 1);
      CHECK(dailyIsFirstPart(schema, part) == expected);
    }
  }
}

// ---------------------------------------------------------------------------------------------
// What handleDaily() decides from a note. The lines between the markers follow the sketch, and
// testSketchText() pins each of them, so this cannot drift from the firmware unnoticed.
// ---------------------------------------------------------------------------------------------

// RULE_SHIPPED is this firmware. RULE_V2216 is the rule up to v2.2.16 (part 0 or 1, whatever
// the schema), kept here to show which notes are handled the same and which are not.
enum Rule { RULE_SHIPPED, RULE_V2216 };

struct DailyDecision {
  bool isFirstPart;
  bool vinAccepted;        // the report's voltage is stored for the client
  bool signalRead;         // the "sig" block is stored for the client
  bool runAlarmReconcile;  // alarms the note does not list are cleared
};

static bool sameDecision(const DailyDecision &a, const DailyDecision &b) {
  return a.isFirstPart == b.isFirstPart && a.vinAccepted == b.vinAccepted &&
         a.signalRead == b.signalRead && a.runAlarmReconcile == b.runAlarmReconcile;
}

static DailyDecision decide(const char *json, Rule rule = RULE_SHIPPED) {
  DailyDecision d = {false, false, false, false};
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json);
  CHECK(err == DeserializationError::Ok);
  if (err != DeserializationError::Ok) return d;

  // --- handleDaily() ---
  uint8_t part = doc["p"].as<uint8_t>();
  bool isFirstPart = dailyIsFirstPart(doc["_sv"] | 0, part);
  if (rule == RULE_V2216) isFirstPart = (part == 0 || part == 1);  // not in the sketch any more

  float vinVoltage = doc["v"].as<float>();
  const char *vsrc = doc["vs"] | "";
  bool vTrusted = (strcmp(vsrc, "mppt") == 0 || strcmp(vsrc, "vin-divider") == 0);
  JsonObject dailySolar = doc["solar"];
  if (!dailySolar.isNull()) {
    float solarBv = dailySolar["bv"] | 0.0f;
    if (solarBv > 0.0f) {
      vinVoltage = solarBv;
      vTrusted = true;
    }
  }
  const bool vinAccepted = isFirstPart && vinVoltage > 0.0f && (vTrusted || vinVoltage >= 6.0f);

  JsonObject sigObj = doc["sig"];
  const bool signalRead = (isFirstPart && !sigObj.isNull());

  JsonArray dailyAlarms = doc["alarms"];
  int dailySchema = doc["_sv"] | 0;
  bool runAlarmReconcile = isFirstPart && (dailyAlarms || dailySchema >= 2);
  // --- end ---

  d.isFirstPart = isFirstPart;
  d.vinAccepted = vinAccepted;
  d.signalRead = signalRead;
  d.runAlarmReconcile = runAlarmReconcile;
  return d;
}

// One part of a daily report the way the client's sendDailyReport() and publishNote() build it:
// part 0 carries the voltage, the signal block and the alarm list; every part carries its
// sensors, "m" (more parts follow) and the schema version.
struct PartSpec {
  int schema;          // "_sv"; 0 = the note carries none (a client before v1.1.6)
  uint8_t part;        // "p", 0-based
  uint8_t totalParts;
  bool alarmsArray;    // part 0 has an "alarms" array (always, on a current client)
  bool alarmActive;    // ... and it lists sensor 1 in high alarm
};

static void buildPart(const PartSpec &s, char *out, size_t outLen) {
  JsonDocument doc;
  doc["c"] = "dev:860322068012345";
  doc["s"] = "North Lease";
  doc["t"] = 1760000000;
  doc["p"] = s.part;
  if (s.part == 0) {
    doc["v"] = 12.6;
    doc["vs"] = "vin-divider";
    JsonObject sig = doc["sig"].to<JsonObject>();
    sig["bars"] = 3;
    sig["rsrp"] = -101;
    if (s.alarmsArray) {
      JsonArray alarms = doc["alarms"].to<JsonArray>();
      if (s.alarmActive) {
        JsonObject a = alarms.add<JsonObject>();
        a["k"] = 1;
        a["hi"] = true;
        a["lo"] = false;
      }
    }
  }
  JsonArray sensors = doc["sensors"].to<JsonArray>();
  JsonObject t = sensors.add<JsonObject>();
  t["k"] = s.part + 1;
  t["st"] = "currentLoop";
  t["ma"] = 11.25;
  t["lvl"] = 42.5;
  doc["m"] = (s.part + 1 < s.totalParts);
  if (s.schema != 0) doc["_sv"] = s.schema;
  doc["fv"] = "2.2.16";
  const size_t n = serializeJson(doc, out, outLen);
  CHECK(n > 0 && n + 1 < outLen);
}

static DailyDecision decidePart(int schema, uint8_t part, uint8_t totalParts, bool alarmsArray,
                                bool alarmActive, Rule rule = RULE_SHIPPED) {
  PartSpec s;
  s.schema = schema;
  s.part = part;
  s.totalParts = totalParts;
  s.alarmsArray = alarmsArray;
  s.alarmActive = alarmActive;
  char json[512];
  buildPart(s, json, sizeof(json));
  return decide(json, rule);
}

// The field client today: v2.2.16, schema 1, 0-based parts.
static void testSchema1Notes() {
  // One sensor, so the whole report is part 0: voltage, signal and reconcile, as before.
  DailyDecision d = decidePart(1, 0, 1, true, false);
  CHECK(d.isFirstPart && d.vinAccepted && d.signalRead && d.runAlarmReconcile);
  d = decidePart(1, 0, 1, true, true);
  CHECK(d.isFirstPart && d.vinAccepted && d.signalRead && d.runAlarmReconcile);

  // A second part is not a first part and reconciles nothing.
  d = decidePart(1, 1, 2, true, true);
  CHECK(!d.isFirstPart && !d.vinAccepted && !d.signalRead && !d.runAlarmReconcile);
  d = decidePart(1, 2, 3, true, true);
  CHECK(!d.isFirstPart && !d.vinAccepted && !d.signalRead && !d.runAlarmReconcile);

  // Schema 1 without an alarms array (clients before the array was always sent): no reconcile.
  d = decidePart(1, 0, 1, false, false);
  CHECK(d.isFirstPart && d.vinAccepted && d.signalRead && !d.runAlarmReconcile);

  // The literal notes: part 0 and part 1 of a two-part schema 1 report.
  d = decide("{\"c\":\"dev:1\",\"s\":\"Site\",\"t\":1760000000,\"p\":0,\"v\":12.4,\"vs\":\"mppt\","
             "\"sig\":{\"bars\":2},\"alarms\":[{\"k\":1,\"hi\":true,\"lo\":false}],"
             "\"sensors\":[{\"k\":1,\"lvl\":40}],\"m\":true,\"_sv\":1,\"fv\":\"2.2.16\"}");
  CHECK(d.isFirstPart && d.vinAccepted && d.signalRead && d.runAlarmReconcile);
  d = decide("{\"c\":\"dev:1\",\"s\":\"Site\",\"t\":1760000000,\"p\":1,"
             "\"sensors\":[{\"k\":2,\"lvl\":12}],\"m\":false,\"_sv\":1,\"fv\":\"2.2.16\"}");
  CHECK(!d.isFirstPart && !d.vinAccepted && !d.signalRead && !d.runAlarmReconcile);
}

// Schema 2 and later: a first part without an alarms array means "no active alarms", so the
// reconcile runs on the schema alone. That must never happen on part 1.
static void testSchema2Notes() {
  // Part 0 with an alarm listed, with an empty list, and with no list: reconcile.
  DailyDecision d = decidePart(2, 0, 2, true, true);
  CHECK(d.isFirstPart && d.vinAccepted && d.signalRead && d.runAlarmReconcile);
  d = decidePart(2, 0, 2, true, false);
  CHECK(d.isFirstPart && d.runAlarmReconcile);
  d = decidePart(2, 0, 2, false, false);
  CHECK(d.isFirstPart && d.runAlarmReconcile);

  // Part 1 has no alarms array. S-D08, the regression this suite prevents: it is not a first
  // part, so the reconcile does not run and the alarms part 0 confirmed stay.
  d = decidePart(2, 1, 2, true, true);
  CHECK(!d.isFirstPart && !d.vinAccepted && !d.signalRead && !d.runAlarmReconcile);
  // The rule up to v2.2.16 ran it, with no alarms to keep: every high/low alarm was cleared.
  d = decidePart(2, 1, 2, true, true, RULE_V2216);
  CHECK(d.isFirstPart && d.runAlarmReconcile);

  // Later parts and later schema versions.
  for (int schema = 2; schema <= 5; ++schema) {
    CHECK(decidePart(schema, 0, 8, true, true).runAlarmReconcile);
    for (int p = 1; p < 8; ++p) {
      d = decidePart(schema, static_cast<uint8_t>(p), 8, true, true);
      CHECK(!d.isFirstPart && !d.vinAccepted && !d.signalRead && !d.runAlarmReconcile);
    }
  }

  // The literal note: part 1 of a two-part schema 2 report.
  d = decide("{\"c\":\"dev:1\",\"s\":\"Site\",\"t\":1760000000,\"p\":1,"
             "\"sensors\":[{\"k\":2,\"lvl\":12}],\"m\":false,\"_sv\":2,\"fv\":\"3.0.0\"}");
  CHECK(!d.isFirstPart && !d.runAlarmReconcile);
}

// Notes without a schema version (clients before v1.1.6, and notes those clients buffered):
// the old rule, unchanged.
static void testLegacyNotes() {
  DailyDecision d = decidePart(0, 0, 2, true, true);
  CHECK(d.isFirstPart && d.vinAccepted && d.signalRead && d.runAlarmReconcile);
  // Without an alarms array nothing is reconciled: the schema gate does not apply.
  d = decidePart(0, 0, 2, false, false);
  CHECK(d.isFirstPart && d.vinAccepted && d.signalRead && !d.runAlarmReconcile);
  // Part 1 as the client builds it still counts as a first part, and still does nothing.
  d = decidePart(0, 1, 2, true, true);
  CHECK(d.isFirstPart && !d.vinAccepted && !d.signalRead && !d.runAlarmReconcile);
  d = decidePart(0, 2, 3, true, true);
  CHECK(!d.isFirstPart && !d.runAlarmReconcile);

  // A part 1 that does carry the first-part block is read as before.
  d = decide("{\"c\":\"dev:1\",\"p\":1,\"v\":12.4,\"vs\":\"mppt\",\"sig\":{\"bars\":2},"
             "\"alarms\":[{\"k\":1,\"hi\":true,\"lo\":false}],\"sensors\":[],\"m\":false}");
  CHECK(d.isFirstPart && d.vinAccepted && d.signalRead && d.runAlarmReconcile);
  d = decide("{\"c\":\"dev:1\",\"p\":1,\"alarms\":[],\"sensors\":[],\"m\":false}");
  CHECK(d.isFirstPart && d.runAlarmReconcile);
  d = decide("{\"c\":\"dev:1\",\"p\":2,\"v\":12.4,\"vs\":\"mppt\",\"sig\":{\"bars\":2},"
             "\"alarms\":[{\"k\":1,\"hi\":true,\"lo\":false}],\"sensors\":[],\"m\":false}");
  CHECK(!d.isFirstPart && !d.vinAccepted && !d.signalRead && !d.runAlarmReconcile);

  // The oldest clients numbered parts from 1 in "part", which the server no longer reads, so
  // each of their parts reads as p = 0. They sent no "v", "sig" or "alarms".
  d = decide("{\"c\":\"dev:1\",\"part\":2,\"vinVoltage\":12.4,\"tanks\":[],\"more\":false}");
  CHECK(d.isFirstPart && !d.vinAccepted && !d.signalRead && !d.runAlarmReconcile);
}

// A whole report, part by part: the reconcile runs once, on part 0.
static void testWholeReports() {
  static const int kSchemas[] = {0, 1, 2, 3};
  for (size_t si = 0; si < COUNT_OF(kSchemas); ++si) {
    const int schema = kSchemas[si];
    for (int total = 1; total <= 8; ++total) {
      for (int withAlarm = 0; withAlarm <= 1; ++withAlarm) {
        int reconciles = 0;
        int voltages = 0;
        int signals = 0;
        int v2216Reconciles = 0;
        for (int p = 0; p < total; ++p) {
          const uint8_t part = static_cast<uint8_t>(p);
          const uint8_t totalParts = static_cast<uint8_t>(total);
          const DailyDecision d = decidePart(schema, part, totalParts, true, withAlarm != 0);
          if (d.runAlarmReconcile) {
            ++reconciles;
            CHECK(p == 0);
          }
          if (d.vinAccepted) ++voltages;
          if (d.signalRead) ++signals;
          if (decidePart(schema, part, totalParts, true, withAlarm != 0, RULE_V2216).runAlarmReconcile) {
            ++v2216Reconciles;
          }
        }
        CHECK(reconciles == 1);
        CHECK(voltages == 1);
        CHECK(signals == 1);
        // Up to v2.2.16 a multi-part report at schema 2+ reconciled twice; the second run, on
        // part 1, had no alarm list.
        CHECK(v2216Reconciles == ((schema >= 2 && total >= 2) ? 2 : 1));
      }
    }
  }
}

// No change for any note a schema 0 or schema 1 client builds: voltage, signal and reconcile
// come out the same as under the rule up to v2.2.16. (isFirstPart itself differs for part 1 at
// schema 1; nothing in that note depends on it.)
static void testSameOutcomeBelowSchema2() {
  for (int schema = 0; schema <= 1; ++schema) {
    for (int total = 1; total <= 8; ++total) {
      for (int p = 0; p < total; ++p) {
        for (int alarms = 0; alarms < 3; ++alarms) {  // no array, empty array, one alarm
          const uint8_t part = static_cast<uint8_t>(p);
          const uint8_t totalParts = static_cast<uint8_t>(total);
          const DailyDecision now = decidePart(schema, part, totalParts, alarms >= 1, alarms == 2);
          const DailyDecision was =
              decidePart(schema, part, totalParts, alarms >= 1, alarms == 2, RULE_V2216);
          CHECK(now.vinAccepted == was.vinAccepted);
          CHECK(now.signalRead == was.signalRead);
          CHECK(now.runAlarmReconcile == was.runAlarmReconcile);
          if (schema == 0) CHECK(sameDecision(now, was));
        }
      }
    }
  }
}

// How the fields are read: "_sv" and "p" as the sketch reads them.
static void testFieldForms() {
  // No "_sv", null, or a string: no schema version, so the old rule.
  CHECK(decide("{\"p\":1,\"alarms\":[]}").isFirstPart);
  CHECK(decide("{\"p\":1,\"alarms\":[],\"_sv\":null}").isFirstPart);
  CHECK(decide("{\"p\":1,\"alarms\":[],\"_sv\":\"2\"}").isFirstPart);
  CHECK(decide("{\"p\":1,\"alarms\":[],\"_sv\":0}").isFirstPart);
  CHECK(decide("{\"p\":1,\"alarms\":[],\"_sv\":-1}").isFirstPart);
  CHECK(!decide("{\"p\":1,\"alarms\":[],\"_sv\":1}").isFirstPart);
  CHECK(!decide("{\"p\":1,\"alarms\":[],\"_sv\":2}").isFirstPart);
  CHECK(!decide("{\"p\":1,\"alarms\":[],\"_sv\":3}").isFirstPart);

  // A schema 1+ part 1 that carried the first-part block would be ignored. No client builds one.
  DailyDecision d = decide("{\"p\":1,\"v\":12.4,\"vs\":\"mppt\",\"solar\":{\"bv\":12.9},"
                           "\"sig\":{\"bars\":2},\"alarms\":[],\"_sv\":1}");
  CHECK(!d.isFirstPart && !d.vinAccepted && !d.signalRead && !d.runAlarmReconcile);

  // A missing "p" reads as part 0.
  d = decide("{\"v\":12.4,\"vs\":\"mppt\",\"sig\":{\"bars\":2},\"alarms\":[],\"_sv\":1}");
  CHECK(d.isFirstPart && d.vinAccepted && d.signalRead && d.runAlarmReconcile);
  CHECK(!decide("{\"p\":255,\"alarms\":[],\"_sv\":1}").isFirstPart);
  CHECK(!decide("{\"p\":255,\"alarms\":[]}").isFirstPart);

  // The voltage rules are unchanged on a first part: an untagged ~5 V reading is rejected, a
  // tagged one and the MPPT pack voltage are accepted.
  CHECK(!decide("{\"p\":0,\"v\":5.0,\"_sv\":1}").vinAccepted);
  CHECK(decide("{\"p\":0,\"v\":12.4,\"_sv\":1}").vinAccepted);
  CHECK(decide("{\"p\":0,\"v\":5.0,\"vs\":\"vin-divider\",\"_sv\":1}").vinAccepted);
  CHECK(decide("{\"p\":0,\"v\":5.0,\"solar\":{\"bv\":12.9},\"_sv\":1}").vinAccepted);
  CHECK(!decide("{\"p\":0,\"_sv\":1}").vinAccepted);
}

// ---------------------------------------------------------------------------------------------
// Sketch text: handleDaily() uses the helper, the lines decide() mirrors are the sketch's, and
// the client still builds its parts the way the rule assumes.
// ---------------------------------------------------------------------------------------------

static char gSketchText[4u << 20];

static bool readSketch(const char *path, size_t minBytes) {
  gSketchText[0] = '\0';
  FILE *f = fopen(path, "rb");
  CHECK(f != nullptr);
  if (!f) {
    printf("cannot open %s\n", path);
    return false;
  }
  const size_t n = fread(gSketchText, 1, sizeof(gSketchText) - 1, f);
  fclose(f);
  gSketchText[n] = '\0';
  CHECK(n > minBytes && n < sizeof(gSketchText) - 1);
  CHECK(strlen(gSketchText) == n);  // no NUL bytes
  return n > minBytes && n < sizeof(gSketchText) - 1;
}

static int countOf(const char *text, const char *needle) {
  int count = 0;
  for (const char *p = strstr(text, needle); p != nullptr; p = strstr(p + 1, needle)) ++count;
  return count;
}

static void testServerSketchText(const char *path) {
  if (!readSketch(path, 500000)) return;
  const char *text = gSketchText;

  CHECK(countOf(text, "#include \"TankAlarm_DailyParts.h\"") == 1);

  // The first-part decision goes through the helper, with the note's own schema version.
  CHECK(countOf(text, "  uint8_t part = doc[\"p\"].as<uint8_t>();\n"
                      "  bool isFirstPart = dailyIsFirstPart(doc[\"_sv\"] | 0, part);\n") == 1);
  CHECK(countOf(text, "dailyIsFirstPart(") == 1);
  CHECK(strstr(text, "(part == 0 || part == 1)") == nullptr);
  CHECK(strstr(text, "part == 1") == nullptr);

  // Its three uses, as decide() mirrors them. A fourth use needs a look at this suite.
  CHECK(countOf(text, "isFirstPart") == 4);
  CHECK(countOf(text, "  const bool vinAccepted = isFirstPart && vinVoltage > 0.0f && (vTrusted || vinVoltage >= 6.0f);\n") == 1);
  CHECK(countOf(text, "  JsonObject sigObj = doc[\"sig\"];\n"
                      "  if (isFirstPart && !sigObj.isNull()) {\n") == 1);
  CHECK(countOf(text, "  JsonArray dailyAlarms = doc[\"alarms\"];\n"
                      "  int dailySchema = doc[\"_sv\"] | 0;\n") == 1);
  CHECK(countOf(text, "  bool runAlarmReconcile = isFirstPart && (dailyAlarms || dailySchema >= 2);\n"
                      "  if (runAlarmReconcile) {\n") == 1);

  // The voltage terms decide() mirrors.
  CHECK(strstr(text, "  float vinVoltage = doc[\"v\"].as<float>();\n") != nullptr);
  CHECK(strstr(text, "  const char *vsrc = doc[\"vs\"] | \"\";\n"
                     "  bool vTrusted = (strcmp(vsrc, \"mppt\") == 0 || strcmp(vsrc, \"vin-divider\") == 0);\n"
                     "  JsonObject dailySolar = doc[\"solar\"];\n"
                     "  if (!dailySolar.isNull()) {\n"
                     "    float solarBv = dailySolar[\"bv\"] | 0.0f;\n"
                     "    if (solarBv > 0.0f) {\n"
                     "      vinVoltage = solarBv;\n"
                     "      vTrusted = true;\n"
                     "    }\n"
                     "  }\n") != nullptr);

  // All of it is in handleDaily(), in this order.
  const char *fn = strstr(text, "\nstatic void handleDaily(JsonDocument &doc, double epoch) {\n");
  const char *first = fn ? strstr(fn, "  bool isFirstPart = dailyIsFirstPart(") : nullptr;
  const char *vin = first ? strstr(first, "  const bool vinAccepted = isFirstPart") : nullptr;
  const char *sig = vin ? strstr(vin, "  if (isFirstPart && !sigObj.isNull()) {") : nullptr;
  const char *rec = sig ? strstr(sig, "  bool runAlarmReconcile = isFirstPart") : nullptr;
  const char *end = rec ? strstr(rec, "\n}\n") : nullptr;
  CHECK(fn != nullptr && first != nullptr && vin != nullptr && sig != nullptr && rec != nullptr &&
        end != nullptr);
  if (fn && rec) {
    // No other function starts between handleDaily's opening line and the last of them.
    const char *nextFn = strstr(fn + 1, "\nstatic ");
    CHECK(nextFn == nullptr || nextFn > rec);
  }
}

// What the rule relies on in the client: parts are numbered from 0 in "p", every note is
// stamped with the schema version, and the alarm list is built in part 0's block.
static void testClientSketchText(const char *path) {
  if (!readSketch(path, 200000)) return;
  const char *text = gSketchText;

  CHECK(countOf(text, "    doc[\"p\"] = part;  // 0-based part number\n") == 1);
  CHECK(countOf(text, "doc[\"p\"] = ") == 1);
  CHECK(strstr(text, "doc[\"part\"]") == nullptr);
  CHECK(countOf(text, "  doc[\"_sv\"] = NOTEFILE_SCHEMA_VERSION;\n") == 1);

  const char *init = strstr(text, "  uint8_t part = 0;\n");
  const char *loop = init ? strstr(init, "  while (monitorCursor < eligibleCount) {\n") : nullptr;
  const char *stamp = loop ? strstr(loop, "    doc[\"p\"] = part;  // 0-based part number\n") : nullptr;
  const char *block = stamp ? strstr(stamp, "    if (part == 0) {\n") : nullptr;
  const char *sig = block ? strstr(block, "        JsonObject sigObj = doc[\"sig\"].to<JsonObject>();\n") : nullptr;
  const char *alarms = sig ? strstr(sig, "        JsonArray alarmsArr = doc[\"alarms\"].to<JsonArray>();\n") : nullptr;
  const char *blockEnd = alarms ? strstr(alarms, "\n    }\n\n    JsonArray sensors = doc[\"sensors\"].to<JsonArray>();\n") : nullptr;
  const char *publish = blockEnd ? strstr(blockEnd, "    publishNote(DAILY_FILE, doc, syncNow);\n") : nullptr;
  const char *next = publish ? strstr(publish, "    ++part;\n") : nullptr;
  CHECK(init != nullptr && loop != nullptr && stamp != nullptr && block != nullptr &&
        sig != nullptr && alarms != nullptr && blockEnd != nullptr && publish != nullptr &&
        next != nullptr);
  // The alarm list and the signal block are built once each, in that block.
  CHECK(countOf(text, "doc[\"alarms\"].to<JsonArray>()") == 1);
  CHECK(countOf(text, "doc[\"sig\"].to<JsonObject>()") == 1);
  CHECK(countOf(text, "    if (part == 0 && vinVoltage > 0.0f) {\n      doc[\"v\"] = vinVoltage;\n") == 1);
  CHECK(countOf(text, "publishNote(DAILY_FILE, ") == 1);
}

int main(int argc, char **argv) {
  const char *serverSketch = (argc > 1) ? argv[1] : SERVER_SKETCH;
  const char *clientSketch = (argc > 2) ? argv[2] : CLIENT_SKETCH;

  testTruthTable();
  testSchema1Notes();
  testSchema2Notes();
  testLegacyNotes();
  testWholeReports();
  testSameOutcomeBelowSchema2();
  testFieldForms();
  testServerSketchText(serverSketch);
  testClientSketchText(clientSketch);

  if (gFailures) {
    printf("daily_parts: %lu of %lu checks FAILED\n", gFailures, gChecks);
    return 1;
  }
  printf("daily_parts: all %lu checks passed\n", gChecks);
  return 0;
}
