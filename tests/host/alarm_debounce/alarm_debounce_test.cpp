// Host tests for TankAlarm-112025-Client-BluesOpta/TankAlarm_AlarmDebounce.h (C-T01).
//
// Build and run: make -C tests/host/alarm_debounce test
//
// Analog vectors use high=80, low=20, hysteresis=5 and need=3 (ALARM_DEBOUNCE_COUNT) unless a
// test says otherwise. "hL"/"lL" means the test starts with the high/low latch already set.
// Sample indexes are 0-based.

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "TankAlarm_AlarmDebounce.h"

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

static const uint8_t kNeed = 3;                 // client ALARM_DEBOUNCE_COUNT
static const uint32_t kHourMs = 3600000u;       // checkAlarmRateLimit hourly window
static const uint32_t kMinIntervalMs = 300000u; // MIN_ALARM_INTERVAL_SECONDS * 1000

// ---------------------------------------------------------------------------------------------
// Analog harness: the same call sequence evaluateAlarms() uses for a valid sample.
// ---------------------------------------------------------------------------------------------

struct AnalogSim {
  float high;
  float low;
  float hyst;
  uint8_t need;
  bool highLatched;
  bool lowLatched;
  uint8_t highEnter;
  uint8_t highExit;
  uint8_t lowEnter;
  uint8_t lowExit;
};

static AnalogSim makeSim(float high, float low, float hyst, bool highLatched, bool lowLatched,
                         uint8_t need) {
  AnalogSim s;
  s.high = high;
  s.low = low;
  s.hyst = hyst;
  s.need = need;
  s.highLatched = highLatched;
  s.lowLatched = lowLatched;
  s.highEnter = 0;
  s.highExit = 0;
  s.lowEnter = 0;
  s.lowExit = 0;
  return s;
}

static AnalogSim defaultSim(bool highLatched, bool lowLatched) {
  return makeSim(80.0f, 20.0f, 5.0f, highLatched, lowLatched, kNeed);
}

// Index of the LAST edge of each kind (-1 = none) and the total number of edges.
struct RunResult {
  int highEnterAt;
  int highExitAt;
  int lowEnterAt;
  int lowExitAt;
  int edges;
};

// One sample through the header, as evaluateAlarms() does it.
static void stepAnalog(AnalogSim &s, float x, uint8_t &highEdge, uint8_t &lowEdge) {
  const AlarmAnalogConditions c = alarmAnalogConditions(x, s.high, s.low, s.hyst);
  alarmAnalogEvaluate(c, s.need, s.highLatched, s.lowLatched, s.highEnter, s.highExit,
                      s.lowEnter, s.lowExit, highEdge, lowEdge);
}

static RunResult runAnalog(AnalogSim &s, const float *xs, size_t n) {
  RunResult r;
  r.highEnterAt = -1;
  r.highExitAt = -1;
  r.lowEnterAt = -1;
  r.lowExitAt = -1;
  r.edges = 0;
  for (size_t i = 0; i < n; ++i) {
    uint8_t highEdge = ALARM_EDGE_NONE;
    uint8_t lowEdge = ALARM_EDGE_NONE;
    stepAnalog(s, xs[i], highEdge, lowEdge);
    CHECK(!(s.highLatched && s.lowLatched));
    if (highEdge == ALARM_EDGE_ENTER) { r.highEnterAt = (int)i; r.edges++; }
    if (highEdge == ALARM_EDGE_EXIT) { r.highExitAt = (int)i; r.edges++; }
    if (lowEdge == ALARM_EDGE_ENTER) { r.lowEnterAt = (int)i; r.edges++; }
    if (lowEdge == ALARM_EDGE_EXIT) { r.lowExitAt = (int)i; r.edges++; }
  }
  return r;
}

#define RUN(sim, arr) runAnalog((sim), (arr), COUNT_OF(arr))

// ---------------------------------------------------------------------------------------------
// T1-T10: strict consecutive debounce (F-04, R-03, H-06). v2.2.15 latched or cleared on T1-T5.
// ---------------------------------------------------------------------------------------------

static void testStrictDebounce() {
  {  // T1: alternating trigger/band samples never latch HIGH
    AnalogSim s = defaultSim(false, false);
    const float xs[] = {90, 50, 90, 50, 90};
    const RunResult r = RUN(s, xs);
    CHECK(r.edges == 0);
    CHECK(!s.highLatched && !s.lowLatched);
  }
  {  // T2 hL: alternating release/hysteresis-band samples never clear
    AnalogSim s = defaultSim(true, false);
    const float xs[] = {70, 78, 70, 78, 70};
    const RunResult r = RUN(s, xs);
    CHECK(r.edges == 0);
    CHECK(s.highLatched);
  }
  {  // T3: mirror of T1 on the low side
    AnalogSim s = defaultSim(false, false);
    const float xs[] = {10, 50, 10, 50, 10};
    const RunResult r = RUN(s, xs);
    CHECK(r.edges == 0);
    CHECK(!s.lowLatched);
  }
  {  // T4 lL: mirror of T2 on the low side
    AnalogSim s = defaultSim(false, true);
    const float xs[] = {30, 22, 30, 22, 30};
    const RunResult r = RUN(s, xs);
    CHECK(r.edges == 0);
    CHECK(s.lowLatched);
  }
  {  // T5: evidence does not survive 1000 normal samples
    static float xs[1003];
    xs[0] = 90.0f;
    for (size_t i = 1; i <= 1000; ++i) xs[i] = 50.0f;
    xs[1001] = 90.0f;
    xs[1002] = 90.0f;
    AnalogSim s = defaultSim(false, false);
    const RunResult r = RUN(s, xs);
    CHECK(r.edges == 0);
    CHECK(!s.highLatched);
  }
  {  // T6: three consecutive high samples latch on the third
    AnalogSim s = defaultSim(false, false);
    const float xs[] = {90, 90, 90};
    const RunResult r = RUN(s, xs);
    CHECK(r.edges == 1);
    CHECK(r.highEnterAt == 2);
    CHECK(s.highLatched && !s.lowLatched);
  }
  {  // T7 hL: three consecutive release samples clear on the third
    AnalogSim s = defaultSim(true, false);
    const float xs[] = {70, 70, 70};
    const RunResult r = RUN(s, xs);
    CHECK(r.edges == 1);
    CHECK(r.highExitAt == 2);
    CHECK(!s.highLatched);
  }
  {  // T8 hL: one high sample interrupts both the clear and the low evidence
    AnalogSim s = defaultSim(true, false);
    const float xs[] = {10, 90, 10, 10};
    const RunResult r = RUN(s, xs);
    CHECK(r.edges == 0);
    CHECK(s.highLatched && !s.lowLatched);
  }
  {  // T9 hL: direct HIGH->LOW sends clear then low on the same sample (as v2.2.15)
    AnalogSim s = defaultSim(true, false);
    const float xs[] = {10, 10, 10};
    const RunResult r = RUN(s, xs);
    CHECK(r.edges == 2);
    CHECK(r.highExitAt == 2);
    CHECK(r.lowEnterAt == 2);
    CHECK(!s.highLatched && s.lowLatched);
  }
  {  // T10 lL: direct LOW->HIGH sends high only; low is unlatched silently (as v2.2.15)
    AnalogSim s = defaultSim(false, true);
    const float xs[] = {90, 90, 90};
    const RunResult r = RUN(s, xs);
    CHECK(r.edges == 1);
    CHECK(r.highEnterAt == 2);
    CHECK(r.lowExitAt == -1);
    CHECK(s.highLatched && !s.lowLatched);
  }
}

// ---------------------------------------------------------------------------------------------
// T11-T12: threshold and hysteresis boundaries (same comparisons as v2.2.15).
// ---------------------------------------------------------------------------------------------

static void testBoundaries() {
  {  // T11a: x == high triggers; just below does not
    AnalogSim s = defaultSim(false, false);
    const float at[] = {80, 80, 80};
    RunResult r = RUN(s, at);
    CHECK(r.edges == 1 && r.highEnterAt == 2);
    s = defaultSim(false, false);
    const float below[] = {79.99f, 79.99f, 79.99f};
    r = RUN(s, below);
    CHECK(r.edges == 0 && !s.highLatched);
  }
  {  // T11b hL: x == high - hyst holds; just below releases
    AnalogSim s = defaultSim(true, false);
    const float at[] = {75, 75, 75};
    RunResult r = RUN(s, at);
    CHECK(r.edges == 0 && s.highLatched);
    s = defaultSim(true, false);
    const float below[] = {74.9f, 74.9f, 74.9f};
    r = RUN(s, below);
    CHECK(r.edges == 1 && r.highExitAt == 2 && !s.highLatched);
  }
  {  // T11c: x == low triggers; lL: x == low + hyst holds, just above releases
    AnalogSim s = defaultSim(false, false);
    const float at[] = {20, 20, 20};
    RunResult r = RUN(s, at);
    CHECK(r.edges == 1 && r.lowEnterAt == 2 && s.lowLatched);
    s = defaultSim(false, true);
    const float band[] = {25, 25, 25};
    r = RUN(s, band);
    CHECK(r.edges == 0 && s.lowLatched);
    s = defaultSim(false, true);
    const float above[] = {25.1f, 25.1f, 25.1f};
    r = RUN(s, above);
    CHECK(r.edges == 1 && r.lowExitAt == 2 && !s.lowLatched);
  }
  {  // T12: hysteresis 0 and negative (clamped to 0)
    const float hysts[] = {0.0f, -5.0f};
    for (size_t h = 0; h < COUNT_OF(hysts); ++h) {
      AnalogSim s = makeSim(80.0f, 20.0f, hysts[h], true, false, kNeed);
      const float below[] = {79.9f, 79.9f, 79.9f};
      RunResult r = RUN(s, below);
      CHECK(r.edges == 1 && r.highExitAt == 2 && !s.highLatched);
      // Still above the threshold: an unclamped -5 would release at x < 85.
      s = makeSim(80.0f, 20.0f, hysts[h], true, false, kNeed);
      const float stillHigh[] = {82, 82, 82};
      r = RUN(s, stillHigh);
      CHECK(r.edges == 0 && s.highLatched);
      // Low side: an unclamped -5 would release at x > 15.
      s = makeSim(80.0f, 20.0f, hysts[h], false, true, kNeed);
      const float stillLow[] = {18, 18, 18};
      r = RUN(s, stillLow);
      CHECK(r.edges == 0 && s.lowLatched);
    }
  }
}

// ---------------------------------------------------------------------------------------------
// T13-T16: overlapping zones, inert thresholds, NaN, need 0/1.
// ---------------------------------------------------------------------------------------------

static void testOverlapAndExtremes() {
  {  // T13a: high=50, low=60 (misconfigured); 55 is in both zones and never latches
    AnalogSim s = makeSim(50.0f, 60.0f, 5.0f, false, false, kNeed);
    const float xs[] = {55, 55, 55, 55, 55};
    const RunResult r = RUN(s, xs);
    CHECK(r.edges == 0 && !s.highLatched && !s.lowLatched);
  }
  {  // T13b lL: v2.2.15 latched HIGH here after 3 samples; neither side moves now
    AnalogSim s = makeSim(50.0f, 60.0f, 5.0f, false, true, kNeed);
    const float xs[] = {55, 55, 55, 55, 55};
    const RunResult r = RUN(s, xs);
    CHECK(r.edges == 0 && !s.highLatched && s.lowLatched);
  }
  {  // T13c: outside the overlap each side still works
    AnalogSim s = makeSim(50.0f, 60.0f, 5.0f, false, false, kNeed);
    const float lowOnly[] = {40, 40, 40};
    RunResult r = RUN(s, lowOnly);
    CHECK(r.edges == 1 && r.lowEnterAt == 2 && s.lowLatched);
    s = makeSim(50.0f, 60.0f, 5.0f, false, false, kNeed);
    const float highOnly[] = {70, 70, 70};
    r = RUN(s, highOnly);
    CHECK(r.edges == 1 && r.highEnterAt == 2 && s.highLatched);
  }
  {  // T14: inert +/-1e9 thresholds never trigger
    AnalogSim s = makeSim(1e9f, -1e9f, 5.0f, false, false, kNeed);
    const float xs[] = {1e6f, 1e6f, 1e6f, 1e6f, 1e6f};
    const RunResult r = RUN(s, xs);
    CHECK(r.edges == 0 && !s.highLatched && !s.lowLatched);
  }
  {  // T15: NaN makes every flag false. The firmware must gate NaN before this call (it skips
     // sampleReused samples); otherwise a latched alarm would count NaN as neither side.
    const AlarmAnalogConditions c = alarmAnalogConditions(NAN, 80.0f, 20.0f, 5.0f);
    CHECK(!c.high && !c.low && !c.highRelease && !c.lowRelease);
  }
  {  // T16: need=1 enters on the first sample; need=0 behaves like need=1
    const uint8_t needs[] = {1, 0};
    for (size_t k = 0; k < COUNT_OF(needs); ++k) {
      AnalogSim s = makeSim(80.0f, 20.0f, 5.0f, false, false, needs[k]);
      const float up[] = {90};
      RunResult r = RUN(s, up);
      CHECK(r.edges == 1 && r.highEnterAt == 0 && s.highLatched);
      const float down[] = {70};
      r = RUN(s, down);
      CHECK(r.edges == 1 && r.highExitAt == 0 && !s.highLatched);
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Digital: alarmDebounceStep(trigger=a, release=!a) must match v2.2.15 exactly.
// ---------------------------------------------------------------------------------------------

// Verbatim port of the v2.2.15 digital debounce (client .ino :6313-6335 at 4d2a12a), with
// sendAlarm() replaced by the returned edge.
struct LegacyDigital {
  bool highAlarmLatched;
  uint8_t highAlarmDebounceCount;
  uint8_t highClearDebounceCount;
};

static uint8_t legacyDigitalStep(LegacyDigital &state, bool shouldAlarm) {
  if (shouldAlarm && !state.highAlarmLatched) {
    state.highAlarmDebounceCount++;
    state.highClearDebounceCount = 0;
    if (state.highAlarmDebounceCount >= kNeed) {
      state.highAlarmLatched = true;
      state.highAlarmDebounceCount = 0;
      return ALARM_EDGE_ENTER;
    }
  } else if (!shouldAlarm && state.highAlarmLatched) {
    state.highClearDebounceCount++;
    state.highAlarmDebounceCount = 0;
    if (state.highClearDebounceCount >= kNeed) {
      state.highAlarmLatched = false;
      state.highClearDebounceCount = 0;
      return ALARM_EDGE_EXIT;
    }
  } else if (!shouldAlarm) {
    state.highAlarmDebounceCount = 0;
  } else {
    state.highClearDebounceCount = 0;
  }
  return ALARM_EDGE_NONE;
}

static void testDigitalEquivalence() {
  unsigned long sequences = 0;
  unsigned long mismatches = 0;
  for (unsigned len = 1; len <= 14; ++len) {
    for (uint32_t bits = 0; bits < (1u << len); ++bits) {
      ++sequences;
      LegacyDigital legacy = {false, 0, 0};
      bool latched = false;
      uint8_t enterCount = 0;
      uint8_t exitCount = 0;
      bool same = true;
      for (unsigned i = 0; i < len; ++i) {
        const bool shouldAlarm = ((bits >> i) & 1u) != 0;
        const uint8_t oldEdge = legacyDigitalStep(legacy, shouldAlarm);
        const uint8_t newEdge = alarmDebounceStep(latched, shouldAlarm, !shouldAlarm, kNeed,
                                                  enterCount, exitCount);
        if (newEdge == ALARM_EDGE_ENTER) latched = true;
        else if (newEdge == ALARM_EDGE_EXIT) latched = false;
        if (oldEdge != newEdge || legacy.highAlarmLatched != latched ||
            legacy.highAlarmDebounceCount != enterCount ||
            legacy.highClearDebounceCount != exitCount) {
          same = false;
        }
      }
      if (!same) {
        if (mismatches < 5) printf("digital mismatch: len=%u bits=0x%x\n", len, (unsigned)bits);
        ++mismatches;
      }
    }
  }
  CHECK(sequences == 32766ul);
  CHECK(mismatches == 0);
}

// ---------------------------------------------------------------------------------------------
// Property test: random analog vectors; every edge is backed by exactly `need` consecutive
// qualifying samples, and any `need` consecutive qualifying samples produce the latch state.
// ---------------------------------------------------------------------------------------------

static uint32_t gLcg = 12345u;
static uint32_t lcgNext() {
  gLcg = gLcg * 1103515245u + 12345u;
  return gLcg >> 16;
}

static void testProperties() {
  static const float kValues[] = {5, 15, 20, 22, 25, 30, 50, 70, 74.9f, 75, 78, 80, 85, 90};
  const float H = 80.0f, L = 20.0f, hyst = 5.0f;
  const size_t kLen = 40;
  unsigned long bad = 0;
  gLcg = 12345u;
  for (int v = 0; v < 20000; ++v) {
    float xs[kLen];
    for (size_t i = 0; i < kLen; ++i) xs[i] = kValues[lcgNext() % COUNT_OF(kValues)];
    AnalogSim s = makeSim(H, L, hyst, false, false, kNeed);
    bool ok = true;
    for (size_t i = 0; i < kLen; ++i) {
      uint8_t highEdge = ALARM_EDGE_NONE;
      uint8_t lowEdge = ALARM_EDGE_NONE;
      stepAnalog(s, xs[i], highEdge, lowEdge);
      if (s.highLatched && s.lowLatched) ok = false;
      // Edges need the previous kNeed samples (this one included) to all qualify.
      for (size_t back = 0; back < kNeed; ++back) {
        const bool have = (i >= back);
        const float x = have ? xs[i - back] : 0.0f;
        if (highEdge == ALARM_EDGE_ENTER && !(have && x >= H && !(x <= L))) ok = false;
        if (highEdge == ALARM_EDGE_EXIT && !(have && x < H - hyst)) ok = false;
        if (lowEdge == ALARM_EDGE_ENTER && !(have && x <= L && !(x >= H))) ok = false;
        if (lowEdge == ALARM_EDGE_EXIT && !(have && x > L + hyst)) ok = false;
      }
      // Conversely, kNeed consecutive qualifying samples always leave the latch in that state.
      if (i + 1 >= kNeed) {
        bool allHigh = true, allHighRelease = true, allLow = true, allLowRelease = true;
        for (size_t back = 0; back < kNeed; ++back) {
          const float x = xs[i - back];
          allHigh = allHigh && (x >= H) && !(x <= L);
          allHighRelease = allHighRelease && (x < H - hyst);
          allLow = allLow && (x <= L) && !(x >= H);
          allLowRelease = allLowRelease && (x > L + hyst);
        }
        if (allHigh && !s.highLatched) ok = false;
        if (allHighRelease && s.highLatched) ok = false;
        if (allLow && !s.lowLatched) ok = false;
        if (allLowRelease && s.lowLatched) ok = false;
      }
    }
    if (!ok) {
      if (bad < 5) printf("property failure in vector %d\n", v);
      ++bad;
    }
  }
  CHECK(bad == 0);
}

// ---------------------------------------------------------------------------------------------
// Window predicate: wrap-safe hourly prune (M-12d) and the boot stamp (F-05/M-12a).
// ---------------------------------------------------------------------------------------------

// The prune loop checkAlarmRateLimit runs over alarmTimestamps / gGlobalAlarmTimestamps.
static uint8_t pruneHour(uint32_t *stamps, uint8_t count, uint32_t now) {
  uint8_t validCount = 0;
  for (uint8_t i = 0; i < count; ++i) {
    if (alarmWithinWindow(now, stamps[i], kHourMs)) {
      stamps[validCount++] = stamps[i];
    }
  }
  return validCount;
}

// v2.2.15's prune test, kept to show the new predicate is identical before the wrap.
static bool legacyKeep(uint32_t now, uint32_t ts) {
  if (now < kHourMs) return true;  // v2.2.15 skipped pruning in the first hour
  return ts > now - kHourMs;
}

static void testWindow() {
  // W1/W2: stamps taken just before the wrap
  CHECK(alarmWithinWindow(0x00001000u, 0xFFFF0000u, kHourMs));
  CHECK(!alarmWithinWindow(0x00100000u, 0xFF000000u, kHourMs));

  {  // W3: in the first hour every stamp in [0, now] is kept
    const uint32_t nows[] = {0u, 1u, 60000u, kHourMs - 1u};
    bool allKept = true;
    for (size_t k = 0; k < COUNT_OF(nows); ++k) {
      for (uint32_t ts = 0; ts <= nows[k]; ++ts) {
        if (!alarmWithinWindow(nows[k], ts, kHourMs)) allKept = false;
      }
    }
    CHECK(allKept);
  }
  {  // W3b: before the wrap (ts <= now) the new predicate equals v2.2.15's
    uint32_t seed = 12345u;
    bool same = true;
    for (int n = 0; n < 200000; ++n) {
      seed = seed * 1664525u + 1013904223u;
      const uint32_t now = seed;
      seed = seed * 1664525u + 1013904223u;
      const uint32_t age = seed % (2u * kHourMs);
      const uint32_t ts = (age <= now) ? now - age : 0u;
      if (alarmWithinWindow(now, ts, kHourMs) != legacyKeep(now, ts)) same = false;
    }
    CHECK(same);
  }

  // W4: exact window edge, both before and across the wrap
  CHECK(alarmWithinWindow(5000000u, 5000000u - 3599999u, kHourMs));
  CHECK(!alarmWithinWindow(5000000u, 5000000u - kHourMs, kHourMs));
  CHECK(alarmWithinWindow(1000u, 1000u - 3599999u, kHourMs));
  CHECK(!alarmWithinWindow(1000u, 1000u - kHourMs, kHourMs));

  {  // W5: setup() stamps every per-type timestamp one interval + 1 ms in the past, so the
     // first alarm of each type passes at any uptime, including the first 300 s.
    const uint32_t boot = 2000u;
    const uint32_t stamp = boot - (kMinIntervalMs + 1u);
    const uint32_t nows[] = {boot, 120000u, 300000u, 4000000000u};
    for (size_t k = 0; k < COUNT_OF(nows); ++k) {
      CHECK(!alarmWithinWindow(nows[k], stamp, kMinIntervalMs));
    }
    // Documented residual: a type never sent re-enters the window once per 49.7-day wrap.
    CHECK(alarmWithinWindow(stamp + 299999u, stamp, kMinIntervalMs));
  }
  {  // W6: ten stamps from the last hour before the wrap
    uint32_t stamps[10];
    for (uint32_t i = 0; i < 10; ++i) stamps[i] = 0xFFFFFFFFu - 3000000u + i * 100000u;
    uint32_t copy[10];
    for (int i = 0; i < 10; ++i) copy[i] = stamps[i];
    CHECK(pruneHour(copy, 10, 700000u) == 8);
    CHECK(copy[0] == stamps[2] && copy[7] == stamps[9]);
    for (int i = 0; i < 10; ++i) copy[i] = stamps[i];
    CHECK(pruneHour(copy, 10, 100000u) == 10);
    // An hour after the wrap all have aged out; v2.2.15 kept every one of them until
    // millis() reached them again 49.7 days later (the budget stayed exhausted).
    for (int i = 0; i < 10; ++i) copy[i] = stamps[i];
    CHECK(pruneHour(copy, 10, 5000000u) == 0);
    unsigned legacyKept = 0;
    for (int i = 0; i < 10; ++i) legacyKept += legacyKeep(5000000u, stamps[i]) ? 1u : 0u;
    CHECK(legacyKept == 10u);
  }
}

int main() {
  testStrictDebounce();
  testBoundaries();
  testOverlapAndExtremes();
  testDigitalEquivalence();
  testProperties();
  testWindow();
  printf("alarm_debounce: %lu checks, %lu failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
