// Host tests for TankAlarm-112025-Client-BluesOpta/TankAlarm_RelayCommand.h (S-T01, relay/float CL-4).
//
//   make -C tests/host/relay_command test ARDUINOJSON_DIR=/path/to/ArduinoJson/src
//
// legacy_v2216_relay.h is a frozen copy of what v2.2.16 does with a relay.qi note; it is used to
// show that old firmware ignores the new key and that the old key named a list position.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ArduinoJson.h>

#include "TankAlarm_RelayCommand.h"
#include "legacy_v2216_relay.h"

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

static const char *kUid = "dev:111111111111111";

static RelayCmdKind classify(const char *json, RelayCommand &cmd) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    printf("FAIL bad test JSON: %s\n", json);
    ++gFailures;
  }
  return relayClassifyCommand(doc.as<JsonObjectConst>(), cmd);
}

static LegacyDecision legacy(const char *json) {
  JsonDocument doc;
  deserializeJson(doc, json);
  return legacyV2216Decide(doc, kUid);
}

static bool uintOf(const char *json, uint32_t lo, uint32_t hi, uint32_t &out) {
  JsonDocument doc;
  deserializeJson(doc, json);
  return relayJsonUint(doc["v"], lo, hi, out);
}

static void testStrictInteger() {
  uint32_t v = 0;
  CHECK(uintOf("{\"v\":1}", 1, 255, v) && v == 1);
  CHECK(uintOf("{\"v\":255}", 1, 255, v) && v == 255);
  CHECK(uintOf("{\"v\":3.0}", 1, 255, v) && v == 3);  // integral double (Notecard/Notehub path)
  const char *bad[] = {"{\"v\":0}", "{\"v\":256}", "{\"v\":-1}", "{\"v\":1.5}", "{\"v\":\"1\"}",
                       "{\"v\":true}", "{\"v\":false}", "{\"v\":null}", "{}", "{\"v\":[1]}",
                       "{\"v\":{\"k\":1}}", "{\"v\":4294967296}", "{\"v\":1e300}",
                       // Just past 32 bits: narrowed, these would wrap to 1 and 255. is<uint32_t>()
                       // range-checks the stored integer, so they are refused.
                       "{\"v\":4294967297}", "{\"v\":4294967551}", "{\"v\":-4294967295}",
                       "{\"v\":18446744073709551617}", "{\"v\":9223372036854775807}"};
  for (const char *j : bad) {
    v = 77;
    CHECK(!uintOf(j, 1, 255, v));
    CHECK(v == 77);  // out is untouched on failure
  }
}

static void testClassify() {
  RelayCommand c;
  CHECK(classify("{\"relay_reset_sensor_number\":2,\"source\":\"server-dashboard\",\"_sv\":1}", c) == RELAY_CMD_RESET_BY_NUMBER && c.sensorNumber == 2);
  // the full envelope the server sends after S5 (relay/float release)
  CHECK(classify("{\"_target\":\"dev:111111111111111\",\"_type\":\"relay\",\"relay_reset_sensor_number\":7,\"source\":\"server-dashboard\",\"_sv\":1}", c) == RELAY_CMD_RESET_BY_NUMBER && c.sensorNumber == 7);
  // unknown future keys (C-T02) do not change the meaning
  CHECK(classify("{\"relay_reset_sensor_number\":7,\"cid\":12,\"t\":1790000000,\"exp\":1790003600}", c) == RELAY_CMD_RESET_BY_NUMBER && c.sensorNumber == 7);
  const char *invalid[] = {"{\"relay_reset_sensor_number\":0}", "{\"relay_reset_sensor_number\":256}",
                           "{\"relay_reset_sensor_number\":-1}", "{\"relay_reset_sensor_number\":1.5}",
                           "{\"relay_reset_sensor_number\":\"1\"}", "{\"relay_reset_sensor_number\":true}",
                           "{\"relay_reset_sensor_number\":[1]}", "{\"relay_reset_sensor_number\":null}"};
  for (const char *j : invalid) {
    CHECK(classify(j, c) == RELAY_CMD_RESET_INVALID);
    CHECK(c.sensorNumber == 0);
  }
  // never falls back to the legacy key
  CHECK(classify("{\"relay_reset_sensor_number\":0,\"relay_reset_sensor\":1}", c) == RELAY_CMD_RESET_INVALID);
  CHECK(classify("{\"relay_reset_sensor_number\":3,\"relay_reset_sensor\":1}", c) == RELAY_CMD_RESET_BY_NUMBER && c.sensorNumber == 3);
  // the reset wins over relay/state in the same note
  CHECK(classify("{\"relay_reset_sensor_number\":3,\"relay\":1,\"state\":true}", c) == RELAY_CMD_RESET_BY_NUMBER);
  // legacy only
  CHECK(classify("{\"relay_reset_sensor\":1}", c) == RELAY_CMD_RESET_LEGACY_IGNORED);
  CHECK(classify("{\"relay_reset_sensor\":\"x\"}", c) == RELAY_CMD_RESET_LEGACY_IGNORED);
  // A present key counts even when it is JSON null; only a missing key is absent. isNull() would
  // treat these as absent and let relay/state switch a relay (Copilot review, fail closed).
  CHECK(classify("{\"relay_reset_sensor_number\":null,\"relay\":1,\"state\":true}", c) == RELAY_CMD_RESET_INVALID);
  CHECK(classify("{\"relay_reset_sensor_number\":null,\"relay_reset_sensor\":1}", c) == RELAY_CMD_RESET_INVALID);  // no fallback
  CHECK(classify("{\"relay_reset_sensor\":null}", c) == RELAY_CMD_RESET_LEGACY_IGNORED);
  CHECK(classify("{\"relay_reset_sensor\":null,\"relay\":1,\"state\":true}", c) == RELAY_CMD_RESET_LEGACY_IGNORED);
  // relay/state and empty notes fall through unchanged
  CHECK(classify("{\"relay\":1,\"state\":true,\"source\":\"server\"}", c) == RELAY_CMD_NONE);
  CHECK(classify("{}", c) == RELAY_CMD_NONE);
  {
    JsonDocument arr;
    deserializeJson(arr, "[1,2]");
    CHECK(relayClassifyCommand(arr.as<JsonObjectConst>(), c) == RELAY_CMD_NONE);  // not an object
  }
}

static void testResolve() {
  uint8_t slot = 99;
  const uint8_t inOrder[] = {1, 2, 3};
  const uint8_t reordered[] = {2, 1, 3};
  const uint8_t sparse[] = {1, 3, 7};
  const uint8_t dup[] = {1, 2, 2};
  CHECK(relayResolveSensorNumber(inOrder, 3, 1, slot) == RELAY_RESOLVE_OK && slot == 0);
  CHECK(relayResolveSensorNumber(inOrder, 3, 3, slot) == RELAY_RESOLVE_OK && slot == 2);
  CHECK(relayResolveSensorNumber(reordered, 3, 1, slot) == RELAY_RESOLVE_OK && slot == 1);
  CHECK(relayResolveSensorNumber(reordered, 3, 2, slot) == RELAY_RESOLVE_OK && slot == 0);
  CHECK(relayResolveSensorNumber(sparse, 3, 7, slot) == RELAY_RESOLVE_OK && slot == 2);
  slot = 99;
  CHECK(relayResolveSensorNumber(sparse, 3, 2, slot) == RELAY_RESOLVE_NOT_FOUND && slot == 99);
  CHECK(relayResolveSensorNumber(dup, 3, 2, slot) == RELAY_RESOLVE_DUPLICATE && slot == 99);
  CHECK(relayResolveSensorNumber(dup, 3, 1, slot) == RELAY_RESOLVE_OK && slot == 0);
  CHECK(relayResolveSensorNumber(inOrder, 3, 0, slot) == RELAY_RESOLVE_INVALID);
  CHECK(relayResolveSensorNumber(nullptr, 0, 1, slot) == RELAY_RESOLVE_NOT_FOUND);
  CHECK(relayResolveSensorNumber(nullptr, 3, 1, slot) == RELAY_RESOLVE_INVALID);
  // a removed monitor: its number is simply gone
  CHECK(relayResolveSensorNumber(inOrder, 2, 3, slot) == RELAY_RESOLVE_NOT_FOUND);
  uint8_t full[8] = {1, 2, 3, 4, 5, 6, 7, 255};
  CHECK(relayResolveSensorNumber(full, 8, 255, slot) == RELAY_RESOLVE_OK && slot == 7);
}

// The defect S-T01 fixes: the dashboard sent the card's list position (registry order), the old
// client used it as a slot; the new client resolves the number.
static void testLegacyDivergence() {
  const uint8_t monitors[] = {2, 1, 3};  // config order: slot 0 is sensor #2
  // Card for sensor #1 was at list position 0 in the dashboard (registry order 1,2,3).
  LegacyDecision old = legacy("{\"relay_reset_sensor\":0}");
  CHECK(old.action == LEGACY_RESET_SLOT && old.slot == 0);  // v2.2.16 cleared slot 0 = sensor #2 (wrong)
  uint8_t slot = 99;
  CHECK(relayResolveSensorNumber(monitors, 3, 1, slot) == RELAY_RESOLVE_OK && slot == 1);  // new: sensor #1
  // Old firmware ignores every envelope the new server sends ("Invalid relay command").
  CHECK(legacy("{\"_target\":\"dev:111111111111111\",\"_type\":\"relay\",\"relay_reset_sensor_number\":1,\"source\":\"server-dashboard\",\"_sv\":1}").action == LEGACY_INVALID);
  CHECK(legacy("{\"relay_reset_sensor_number\":255}").action == LEGACY_INVALID);
  // ...and the new classifier ignores the old key.
  RelayCommand c;
  CHECK(classify("{\"relay_reset_sensor\":0}", c) == RELAY_CMD_RESET_LEGACY_IGNORED);
  // Unchanged relay/state decisions still mean the same on both.
  CHECK(legacy("{\"relay\":2,\"state\":true}").action == LEGACY_SET);
  CHECK(classify("{\"relay\":2,\"state\":true}", c) == RELAY_CMD_NONE);
  // Intended difference: v2.2.16 read a null Clear Relay key as absent and ran relay/state; the new
  // classifier fails closed and switches nothing. No server sends a null key.
  CHECK(legacy("{\"relay_reset_sensor\":null,\"relay\":2,\"state\":true}").action == LEGACY_SET);
  CHECK(classify("{\"relay_reset_sensor\":null,\"relay\":2,\"state\":true}", c) == RELAY_CMD_RESET_LEGACY_IGNORED);
  CHECK(legacy("{\"target\":\"dev:222\",\"relay_reset_sensor\":1}").action == LEGACY_NOT_FOR_US);
}

static void testScope() {
  CHECK(relayScopeOf(0, "dev:222", kUid) == RELAY_SCOPE_NONE);
  CHECK(relayScopeOf(0x10, "", kUid) == RELAY_SCOPE_NONE);  // only bits 0-3 are relays
  CHECK(relayScopeOf(1, "", kUid) == RELAY_SCOPE_LOCAL);
  CHECK(relayScopeOf(1, nullptr, kUid) == RELAY_SCOPE_LOCAL);
  CHECK(relayScopeOf(3, kUid, kUid) == RELAY_SCOPE_LOCAL);
  CHECK(relayScopeOf(3, "dev:222", kUid) == RELAY_SCOPE_REMOTE);
  CHECK(relayScopeOf(3, "dev:222", "") == RELAY_SCOPE_REMOTE);
  CHECK(relayScopeOf(3, "dev:222", nullptr) == RELAY_SCOPE_REMOTE);
}

static void testResults() {
  CHECK(relayClearResultFromResolve(RELAY_RESOLVE_OK, 0x5) == RELAY_CLEAR_RELEASED);
  CHECK(relayClearResultFromResolve(RELAY_RESOLVE_OK, 0) == RELAY_CLEAR_NONE_ACTIVE);
  CHECK(relayClearResultFromResolve(RELAY_RESOLVE_NOT_FOUND, 0) == RELAY_CLEAR_UNKNOWN_SENSOR);
  CHECK(relayClearResultFromResolve(RELAY_RESOLVE_DUPLICATE, 0) == RELAY_CLEAR_DUPLICATE_SENSOR);
  CHECK(relayClearResultFromResolve(RELAY_RESOLVE_INVALID, 0) == RELAY_CLEAR_INVALID);
  const char *names[] = {"released", "none-active", "unknown-sensor", "duplicate-sensor", "invalid", "legacy-ignored"};
  for (uint8_t i = 0; i < RELAY_CLEAR_RESULT_COUNT; ++i) CHECK(strcmp(relayClearResultName(i), names[i]) == 0);
  CHECK(strcmp(relayClearResultName(RELAY_CLEAR_RESULT_COUNT), "invalid") == 0);
  CHECK(strcmp(relayClearResultName(255), "invalid") == 0);
}

// The log word: a remote release only requested OFF, so it is not logged as "released".
static const char kClearLineFmt[] = "Clear Relay sensor #%u -> monitor %u (%s): %s, active 0x%X, binding %s%s";

static void testLogWord() {
  CHECK(strcmp(relayClearLogWord(RELAY_CLEAR_RELEASED, RELAY_SCOPE_REMOTE), "off-requested") == 0);
  CHECK(strcmp(relayClearLogWord(RELAY_CLEAR_RELEASED, RELAY_SCOPE_LOCAL), "released") == 0);
  CHECK(strcmp(relayClearLogWord(RELAY_CLEAR_RELEASED, RELAY_SCOPE_NONE), "released") == 0);
  CHECK(strcmp(relayClearLogWord(RELAY_CLEAR_NONE_ACTIVE, RELAY_SCOPE_REMOTE), "none-active") == 0);
  for (uint8_t i = 1; i < RELAY_CLEAR_RESULT_COUNT; ++i) {
    for (uint8_t s = RELAY_SCOPE_NONE; s <= RELAY_SCOPE_REMOTE; ++s) {
      CHECK(strcmp(relayClearLogWord(i, s), relayClearResultName(i)) == 0);  // only RELEASED+REMOTE differs
    }
  }
  CHECK(strcmp(relayClearLogWord(255, RELAY_SCOPE_REMOTE), "invalid") == 0);
  // Worst case of the sketch's line (23-char name, 47-char target, widest numbers) still fits the
  // 160-byte buffer and serial log entry without truncation.
  char name[24];
  char target[48];
  memset(name, 'N', sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  memset(target, 'T', sizeof(target) - 1);
  target[sizeof(target) - 1] = '\0';
  char line[160];
  const int len = snprintf(line, sizeof(line), kClearLineFmt, 255u, 255u, name,
                           relayClearLogWord(RELAY_CLEAR_RELEASED, RELAY_SCOPE_REMOTE), 0xFFu, "remote ", target);
  CHECK(len > 0 && len < (int)sizeof(line));
}

// The sketch must use the classifier and never read the legacy key as a slot again.
static void testSketchText() {
  FILE *f = fopen(CLIENT_SKETCH, "rb");
  CHECK(f != nullptr);
  if (!f) return;
  static char text[1 << 20];
  const size_t n = fread(text, 1, sizeof(text) - 1, f);
  fclose(f);
  text[n] = '\0';
  CHECK(n > 100000 && n < sizeof(text) - 1);
  CHECK(strstr(text, "#include \"TankAlarm_RelayCommand.h\"") != nullptr);
  CHECK(strstr(text, "relayClassifyCommand(doc.as<JsonObjectConst>(), cmd)") != nullptr);
  CHECK(strstr(text, "doc[\"relay_reset_sensor\"]") == nullptr);
  CHECK(strstr(text, "static RelayClearResult clearRelaysForSensorNumber(uint8_t sensorNumber) {") != nullptr);
  // The sketch runs the host-tested step with the real side effects, and checks _target too.
  CHECK(strstr(text, "const RelayClearOutcome o = relayClearSensorNumber(") != nullptr);
  CHECK(strstr(text, "[](uint8_t slot) { return getMonitorActiveRelayMask(slot); },") != nullptr);
  CHECK(strstr(text, "[](uint8_t slot) { resetRelayForMonitor(slot); });") != nullptr);
  CHECK(strstr(text, "numbers[i] = gConfig.monitors[i].sensorIndex;") != nullptr);
  CHECK(strstr(text, "const char *routedUid = doc[\"_target\"].as<const char*>();") != nullptr);
  CHECK(strstr(text, "RELAY_CMD_RESET_BY_NUMBER:\n        clearRelaysForSensorNumber(cmd.sensorNumber);\n        return;") != nullptr);
  // The Clear Relay log line uses the tested word and the format whose worst case testLogWord measures.
  CHECK(strstr(text, "cfg.name, relayClearLogWord(o.result, scope),") != nullptr);
  CHECK(strstr(text, kClearLineFmt) != nullptr);
  CHECK(strstr(text, "char line[160];") != nullptr);
}

// The Clear Relay step with recording doubles for its three side effects.
struct ClearRecorder {
  uint8_t masks[8];
  int maskCalls;
  int logCalls;
  int releaseCalls;
  uint8_t releasedSlot;
  RelayClearOutcome logged;
  int order[3];  // 1 = mask, 2 = log, 3 = release, in call order
  int orderLen;
};

static RelayClearOutcome runClear(ClearRecorder &r, const uint8_t *numbers, uint8_t count, uint8_t k) {
  r.maskCalls = r.logCalls = r.releaseCalls = r.orderLen = 0;
  r.releasedSlot = 0xFF;
  return relayClearSensorNumber(
      numbers, count, k,
      [&r](uint8_t slot) {
        ++r.maskCalls;
        if (r.orderLen < 3) r.order[r.orderLen++] = 1;
        return r.masks[slot];
      },
      [&r](const RelayClearOutcome &o) {
        ++r.logCalls;
        r.logged = o;
        if (r.orderLen < 3) r.order[r.orderLen++] = 2;
      },
      [&r](uint8_t slot) {
        ++r.releaseCalls;
        r.releasedSlot = slot;
        if (r.orderLen < 3) r.order[r.orderLen++] = 3;
      });
}

static void testClearStep() {
  ClearRecorder r;
  memset(&r, 0, sizeof(r));
  r.masks[0] = 0x2;  // sensor #2 has relay 2 on
  r.masks[1] = 0x1;
  r.masks[2] = 0x0;
  const uint8_t numbers[] = {2, 1, 3};  // config order differs from number order

  RelayClearOutcome o = runClear(r, numbers, 3, 1);
  CHECK(o.resolve == RELAY_RESOLVE_OK && o.slot == 1 && o.activeMask == 0x1);
  CHECK(o.result == RELAY_CLEAR_RELEASED);
  CHECK(r.maskCalls == 1 && r.logCalls == 1 && r.releaseCalls == 1 && r.releasedSlot == 1);
  CHECK(r.orderLen == 3 && r.order[0] == 1 && r.order[1] == 2 && r.order[2] == 3);  // log before release
  CHECK(r.logged.slot == 1 && r.logged.result == RELAY_CLEAR_RELEASED);

  o = runClear(r, numbers, 3, 3);  // found, nothing on: still released (clears any tracked state)
  CHECK(o.result == RELAY_CLEAR_NONE_ACTIVE && r.releaseCalls == 1 && r.releasedSlot == 2);

  o = runClear(r, numbers, 3, 9);  // unknown: logged, nothing read or released
  CHECK(o.result == RELAY_CLEAR_UNKNOWN_SENSOR);
  CHECK(r.maskCalls == 0 && r.logCalls == 1 && r.releaseCalls == 0 && o.activeMask == 0);

  const uint8_t dup[] = {4, 2, 4};
  o = runClear(r, dup, 3, 4);  // duplicate: nothing guessed
  CHECK(o.result == RELAY_CLEAR_DUPLICATE_SENSOR);
  CHECK(r.maskCalls == 0 && r.logCalls == 1 && r.releaseCalls == 0);

  o = runClear(r, dup, 3, 2);  // the unique one next to a duplicate still resolves
  CHECK(o.result == RELAY_CLEAR_RELEASED && r.releaseCalls == 1 && r.releasedSlot == 1);

  o = runClear(r, numbers, 3, 0);  // 0 is never a sensor number
  CHECK(o.result == RELAY_CLEAR_INVALID && r.releaseCalls == 0 && r.maskCalls == 0);

  o = runClear(r, nullptr, 0, 1);  // no monitors
  CHECK(o.result == RELAY_CLEAR_UNKNOWN_SENSOR && r.releaseCalls == 0 && r.logCalls == 1);

  o = runClear(r, numbers, 2, 3);  // count limits the search: #3 is past the configured monitors
  CHECK(o.result == RELAY_CLEAR_UNKNOWN_SENSOR && r.releaseCalls == 0);
}

int main() {
  testStrictInteger();
  testClassify();
  testResolve();
  testLegacyDivergence();
  testScope();
  testResults();
  testLogWord();
  testClearStep();
  testSketchText();
  if (gFailures) {
    printf("relay_command: %lu of %lu checks FAILED\n", gFailures, gChecks);
    return 1;
  }
  printf("relay_command: all %lu checks passed\n", gChecks);
  return 0;
}
