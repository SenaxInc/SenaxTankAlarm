// Host tests for TankAlarm-112025-Server-BluesOpta/WarmTierStore.h (S-D03).
//
//   make test ARDUINOJSON_DIR=<ArduinoJson v7.4.3>/src
//
// Every WARM_* I/O call goes through warm_test_io.h, which counts operations
// and can fail any one of them; ArduinoJson allocations go through a
// TestAllocator that can fail any one of them. The v2.2.15 rollup math and
// warm-tier readers are ported in legacy_v2215.h as oracles.

#include "warm_test_io.h"
#include "WarmTierStore.h"
#include "legacy_v2215.h"

#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <deque>
#include <map>
#include <random>
#include <string>
#include <vector>

#ifndef WARM_TEST_EDGE_STRIDE
#define WARM_TEST_EDGE_STRIDE 1  // byte stride inside the first/last 2 KB of the truncation sweep
#endif

// ============================================================================
// Minimal test framework
// ============================================================================
static long gChecks = 0;
static long gFailures = 0;

static std::string fmtStr(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static std::string fmtStr(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return std::string(buf);
}

static void reportFailure(const char *file, int line, const char *expr, const std::string &detail) {
  ++gFailures;
  if (gFailures <= 200) {
    fprintf(stderr, "%s:%d: CHECK failed: %s%s%s\n", file, line, expr, detail.empty() ? "" : "  -- ",
            detail.c_str());
  }
}

#define CHECK(cond)                                                            \
  do {                                                                         \
    ++gChecks;                                                                 \
    if (!(cond)) reportFailure(__FILE__, __LINE__, #cond, std::string());      \
  } while (0)

#define CHECK_MSG(cond, ...)                                                   \
  do {                                                                         \
    ++gChecks;                                                                 \
    if (!(cond)) reportFailure(__FILE__, __LINE__, #cond, fmtStr(__VA_ARGS__)); \
  } while (0)

// ============================================================================
// Hooks
// ============================================================================
static std::vector<std::string> gLogLines;
static long gYieldCalls = 0;
static uint32_t gClockMs = 0;
static uint32_t gClockStep = 0;

struct NoteRecord {
  std::string path;
  uint32_t bytes;
  bool ok;
};
static std::vector<NoteRecord> gNotes;

static void hookLog(const char *level, const char *msg) { gLogLines.push_back(std::string(level) + ": " + msg); }
static void hookYield() { ++gYieldCalls; }
static uint32_t hookNowMs() {
  const uint32_t v = gClockMs;
  gClockMs += gClockStep;
  return v;
}
static void hookNoteWrite(const char *path, uint32_t bytes, bool ok) {
  NoteRecord r = {path ? path : "", bytes, ok};
  gNotes.push_back(r);
}

static WarmHooks makeHooks(TestAllocator *alloc) {
  WarmHooks h = {hookYield, hookLog, hookNoteWrite, hookNowMs, alloc};
  return h;
}

static void resetHooks() {
  gLogLines.clear();
  gYieldCalls = 0;
  gClockMs = 0;
  gClockStep = 0;
  gNotes.clear();
  fi::reset();
}

static size_t countLogs(const char *needle) {
  size_t n = 0;
  for (const std::string &l : gLogLines) {
    if (l.find(needle) != std::string::npos) n++;
  }
  return n;
}

// Operation indices of one kind since the last fi::reset()
static std::vector<long> opIndices(int op) {
  std::vector<long> out;
  const std::vector<int> &trace = fi::state().trace;
  for (size_t i = 0; i < trace.size(); ++i) {
    if (trace[i] == op) out.push_back((long)i);
  }
  return out;
}

// v2.2.15's roundTo() (TankAlarm_Utils.h tankalarm_roundTo)
static float testRoundTo(float val, int decimals) {
  float multiplier = pow(10, decimals);
  return round(val * multiplier) / multiplier;
}

static uint8_t testAlarmCount(void *ctx, const char *uid, uint8_t k, double begin, double end) {
  return legacyAlarmCount(*static_cast<const std::vector<LegacyAlarm> *>(ctx), uid, k, begin, end);
}

// ============================================================================
// File helpers
// ============================================================================
static std::string gRoot;

static std::string joinPath(const std::string &a, const std::string &b) { return a + "/" + b; }

static bool fileExists(const std::string &p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0;
}

static std::string readFile(const std::string &p) {
  std::string out;
  FILE *f = fopen(p.c_str(), "rb");
  if (!f) return out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  fclose(f);
  return out;
}

static void writeFile(const std::string &p, const std::string &data) {
  FILE *f = fopen(p.c_str(), "wb");
  if (!f) {
    fprintf(stderr, "cannot write %s\n", p.c_str());
    exit(2);
  }
  const size_t w = fwrite(data.data(), 1, data.size(), f);
  fclose(f);
  if (w != data.size()) {
    fprintf(stderr, "short write %s\n", p.c_str());
    exit(2);
  }
}

static void removeFile(const std::string &p) { remove(p.c_str()); }

static std::vector<std::string> listDir(const std::string &dir) {
  std::vector<std::string> names;
  DIR *d = opendir(dir.c_str());
  if (!d) return names;
  while (struct dirent *e = readdir(d)) {
    const std::string n = e->d_name;
    if (n != "." && n != "..") names.push_back(n);
  }
  closedir(d);
  std::sort(names.begin(), names.end());
  return names;
}

static void clearDir(const std::string &dir) {
  for (const std::string &n : listDir(dir)) removeFile(joinPath(dir, n));
}

static std::string monthFile(const std::string &dir, int y, int m, const char *suffix = "") {
  char buf[256];
  if (!warmMonthPath(buf, sizeof(buf), dir.c_str(), y, m, suffix)) return std::string();
  return buf;
}

static std::string makeDir(const std::string &name) {
  const std::string p = joinPath(gRoot, name);
  mkdir(p.c_str(), 0777);
  clearDir(p);
  // warmMergeMonth's path buffers hold 95 characters (sized for "/fs/history")
  const std::string probe = monthFile(p, 2026, 9);
  CHECK_MSG(!probe.empty() && probe.size() < 96, "test dir %s is too long", p.c_str());
  return p;
}

static bool anyTmp(const std::string &dir) {
  for (const std::string &n : listDir(dir)) {
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".tmp") == 0) return true;
  }
  return false;
}

static double epochOf(int y, int m, int d, double seconds = 0.0) {
  return (double)warmCivilToDn(y, m, d) * 86400.0 + seconds;
}

static int32_t dnOf(int y, int m, int d) { return warmCivilToDn(y, m, d); }

// ============================================================================
// Row / scan helpers
// ============================================================================
static WarmNewRow makeRow(const char *uid, uint32_t d, uint8_t k, uint16_t n, float base, uint8_t al) {
  WarmNewRow r;
  r.c = uid;
  r.d = d;
  r.k = k;
  r.al = al;
  r.state = WARM_ROW_ADD;
  r.mn = base;
  r.mx = base + 5.0f;
  r.av = base + 2.5f;
  r.op = base + 1.0f;
  r.cl = base + 4.0f;
  r.vt = 12.5f;
  r.n = n;
  return r;
}

static std::string encodeRow(const WarmNewRow &r) {
  char buf[WARM_ROW_MAX_WRITE];
  const WarmHooks h = makeHooks(nullptr);
  bool noMemory = false;
  const size_t n = warmEncodeRow(r, buf, sizeof(buf), h, &noMemory);
  return std::string(buf, n);
}

static std::string joinArray(const std::vector<std::string> &rows) {
  std::string out = "[";
  for (size_t i = 0; i < rows.size(); ++i) {
    if (i) out += ",";
    out += rows[i];
  }
  out += "]";
  return out;
}

struct Collected {
  std::vector<std::string> raw;
  long stopAfter = -1;
};

static bool collectVisitor(JsonObjectConst row, const char *raw, size_t rawLen, void *ctx) {
  (void)row;
  Collected &c = *static_cast<Collected *>(ctx);
  c.raw.push_back(std::string(raw, rawLen));
  return !(c.stopAfter >= 0 && (long)c.raw.size() >= c.stopAfter);
}

static WarmStatus scanPath(const std::string &path, bool salvage, uint32_t maxBytes, Collected *out,
                           uint32_t *rows = nullptr, uint32_t *offset = nullptr) {
  const WarmHooks h = makeHooks(nullptr);
  return warmScanFile(path.c_str(), salvage, maxBytes, 0, out ? collectVisitor : nullptr, out, h, rows, offset);
}

// (d|c|k) -> list of n, parsed with a whole-document ArduinoJson oracle
static std::map<std::string, std::vector<long>> fileKeys(const std::string &path, bool *parsed = nullptr) {
  std::map<std::string, std::vector<long>> keys;
  JsonDocument doc;
  const std::string text = readFile(path);
  const DeserializationError err = deserializeJson(doc, text);
  if (parsed) *parsed = !err && doc.is<JsonArray>();
  if (err) return keys;
  for (JsonObject row : doc.as<JsonArray>()) {
    const std::string key = fmtStr("%lu|%s|%u", (unsigned long)(row["d"] | 0UL), (const char *)(row["c"] | ""),
                                   (unsigned)(row["k"] | 0u));
    keys[key].push_back(row["n"] | 0L);
  }
  return keys;
}

static std::string keyOf(uint32_t d, const char *c, unsigned k) { return fmtStr("%lu|%s|%u", (unsigned long)d, c, k); }

// ============================================================================
// Fleet (hot tier) helpers
// ============================================================================
struct Fleet {
  std::vector<LegacyRing> rings;
  std::vector<LegacyAlarm> alarms;
  std::vector<WarmSeries> view;

  const WarmSeries *series() {
    view.clear();
    for (const LegacyRing &r : rings) view.push_back(r.series());
    return view.data();
  }
  uint8_t count() const { return (uint8_t)rings.size(); }

  void addSensors(size_t n, uint16_t cap) {
    for (size_t i = 0; i < n; ++i) {
      rings.push_back(LegacyRing(fmtStr("dev:86445%010lu", (unsigned long)(i + 1)), (uint8_t)(1 + i % 4), cap));
    }
  }

  // perDay snapshots spread over each day of [firstDn, lastDn]
  void fill(int32_t firstDn, int32_t lastDn, int perDay, float base) {
    for (int32_t dn = firstDn; dn <= lastDn; ++dn) {
      for (int s = 0; s < perDay; ++s) {
        const double ts = (double)dn * 86400.0 + 3600.0 + s * (80000.0 / perDay);
        for (size_t i = 0; i < rings.size(); ++i) {
          rings[i].add(ts, base + (float)i + 0.1f * (float)(dn % 7) + 0.3f * (float)s, 12.0f + 0.1f * (float)s);
        }
      }
    }
  }
};

static WarmRollupConfig defaultConfig(const std::string &dir) {
  WarmRollupConfig c;
  c.dir = dir.c_str();
  c.maxFileBytes = 2UL * 31UL * 20UL * WARM_ROW_MAX_WRITE;
  c.maxSeries = 20;
  c.maxBackfillDays = 92;
  c.retainedMonths = 3;
  c.maxBatches = 16;
  c.maxWrites = 2;
  c.budgetMs = 8000;
  return c;
}

static WarmTickSummary tick(WarmRollupState &s, Fleet &fleet, double now, const WarmRollupConfig &cfg,
                            const WarmHooks &h) {
  return warmRollupTick(s, now, fleet.series(), fleet.count(), testAlarmCount, &fleet.alarms, testRoundTo, cfg, h);
}

// Ticks until the scheduler is idle; returns the number of ticks (or -1)
static int tickUntilIdle(WarmRollupState &s, Fleet &fleet, double now, const WarmRollupConfig &cfg,
                         const WarmHooks &h, uint32_t *writes = nullptr) {
  for (int i = 0; i < 100; ++i) {
    const WarmTickSummary sum = tick(s, fleet, now, cfg, h);
    if (writes) *writes += sum.writes;
    if (sum.batches == 0) return i;
  }
  return -1;
}

// ============================================================================
// Calendar
// ============================================================================
static void testCalendar() {
  const int32_t first = dnOf(2020, 1, 1);
  const int32_t last = dnOf(2099, 12, 31);
  CHECK(first == 18262);
  long bad = 0;
  for (int32_t dn = first; dn <= last; ++dn) {
    int y, m, d;
    warmDnToCivil(dn, &y, &m, &d);
    const time_t t = (time_t)dn * 86400;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    bool ok = tmv.tm_year + 1900 == y && tmv.tm_mon + 1 == m && tmv.tm_mday == d;
    ok = ok && warmCivilToDn(y, m, d) == dn;
    const uint32_t ymd = warmDnToYmd(dn);
    int32_t back = -1;
    ok = ok && ymd == (uint32_t)(y * 10000 + m * 100 + d) && warmYmdToDn(ymd, &back) && back == dn;
    ok = ok && warmFirstDnOfMonth(dn) == warmCivilToDn(y, m, 1);
    int y2, m2, d2;
    warmDnToCivil(warmLastDnOfMonth(dn) + 1, &y2, &m2, &d2);
    ok = ok && d2 == 1 && warmLastDnOfMonth(dn) >= dn && (m2 != m);
    ok = ok && warmEpochToDn((double)dn * 86400.0) == dn && warmEpochToDn((double)dn * 86400.0 - 0.001) == dn - 1 &&
         warmEpochToDn((double)dn * 86400.0 + 86399.999) == dn;
    if (!ok && ++bad <= 5) CHECK_MSG(ok, "day number %ld", (long)dn);
  }
  CHECK(bad == 0);

  CHECK(warmEpochToDn(NAN) == INT32_MIN);
  CHECK(warmEpochToDn(1e300) == INT32_MAX);
  CHECK(warmEpochToDn(-1e300) == INT32_MIN);

  const uint32_t rejects[] = {0u, 20260230u, 20261301u, 19991231u, 21000101u, 20260100u, 20260431u, 20250229u,
                              2026091u};
  for (uint32_t v : rejects) CHECK_MSG(!warmYmdToDn(v, nullptr), "%lu accepted", (unsigned long)v);
  int32_t dn = 0;
  CHECK(warmYmdToDn(20240229u, &dn) && dn == dnOf(2024, 2, 29));
  CHECK(warmLastDnOfMonth(dnOf(2024, 2, 10)) == dnOf(2024, 2, 29));
  CHECK(warmLastDnOfMonth(dnOf(2025, 2, 10)) == dnOf(2025, 2, 28));
  CHECK(warmLastDnOfMonth(dnOf(2100, 2, 10)) == dnOf(2100, 2, 28));
  CHECK(warmLastDnOfMonth(dnOf(2026, 12, 31)) == dnOf(2026, 12, 31));

  // warmRetainedFloorDn(today, 3) == the oldest month pruneDailySummaryFiles()
  // keeps (it deletes months MAX_DAILY_SUMMARY_MONTHS+1..24 back, S:8393-8406)
  bad = 0;
  for (int32_t t = dnOf(2024, 1, 1); t <= dnOf(2030, 12, 31); ++t) {
    int curYear, curMonth, dd;
    warmDnToCivil(t, &curYear, &curMonth, &dd);
    int oldYear = curYear;
    int oldMonth = curMonth - 3;
    while (oldMonth <= 0) { oldMonth += 12; oldYear--; }
    if (warmRetainedFloorDn(t, 3) != warmCivilToDn(oldYear, oldMonth, 1) && ++bad <= 3) {
      CHECK_MSG(false, "retained floor for %lu", (unsigned long)warmDnToYmd(t));
    }
  }
  CHECK(bad == 0);

  char path[40];
  CHECK(warmMonthPath(path, sizeof(path), "/fs/history", 2026, 9, ".tmp") &&
        strcmp(path, "/fs/history/daily_202609.json.tmp") == 0);
  volatile size_t small = 20;  // opaque, so the compiler does not flag the deliberate truncation
  CHECK(!warmMonthPath(path, small, "/fs/history", 2026, 9, ""));
}

// ============================================================================
// Element splitter and scan grammar
// ============================================================================
static std::string elementOfSize(size_t size) {
  // {"p":"xxx"} has 8 bytes of framing
  return "{\"p\":\"" + std::string(size - 8, 'x') + "\"}";
}

static std::string fleetMonthFixture(int sensors, int y, int m, std::vector<size_t> *rowEnds) {
  std::vector<std::string> rows;
  std::vector<std::string> uids;
  for (int i = 0; i < sensors; ++i) uids.push_back(fmtStr("dev:864475%09d", i + 1));
  const int32_t firstDn = warmCivilToDn(y, m, 1);
  const int32_t lastDn = warmLastDnOfMonth(firstDn);
  for (int32_t dn = firstDn; dn <= lastDn; ++dn) {
    for (int i = 0; i < sensors; ++i) {
      WarmNewRow r = makeRow(uids[(size_t)i].c_str(), warmDnToYmd(dn), (uint8_t)(1 + i % 4), (uint16_t)(20 + i),
                             100.0f + (float)i * 3.3f + (float)(dn % 5) * 0.7f, (uint8_t)(i % 3));
      rows.push_back(encodeRow(r));
    }
  }
  const std::string text = joinArray(rows);
  if (rowEnds) {
    rowEnds->clear();
    size_t pos = 1;
    for (size_t i = 0; i < rows.size(); ++i) {
      pos += rows[i].size();
      rowEnds->push_back(pos);  // offset just after the row's closing brace
      pos += 1;                 // ',' or ']'
    }
  }
  return text;
}

static void testGrammar() {
  const std::string dir = makeDir("grammar");
  const std::string path = joinPath(dir, "g.json");
  const uint32_t big = 1u << 20;

  struct Case {
    const char *name;
    std::string text;
    WarmStatus strict;
    size_t rows;         // rows seen by a strict scan
    size_t salvageRows;  // rows returned by a salvage scan
  };
  const std::vector<Case> cases = {
      {"empty array", "[]", WARM_OK, 0, 0},
      {"whitespace", " \r\n\t[ \n ] \n", WARM_OK, 0, 0},
      {"one row", "[{\"d\":1}]", WARM_OK, 1, 1},
      {"spaced rows", "[ {\"d\":1} ,\n {\"d\":2} ]", WARM_OK, 2, 2},
      {"escaped quote and braces in strings", "[{\"c\":\"a\\\"}{[\"},{\"c\":\"}\"}]", WARM_OK, 2, 2},
      {"escaped backslash", "[{\"c\":\"back\\\\\"},{\"d\":2}]", WARM_OK, 2, 2},
      {"nested values", "[{\"x\":{\"y\":[1,{\"z\":\"]}\"}]},\"d\":3}]", WARM_OK, 1, 1},
      {"bytes after the array", "[{\"d\":1}]garbage{", WARM_OK, 1, 1},
      {"trailing comma", "[{\"d\":1},]", WARM_CORRUPT, 1, 1},
      {"missing comma", "[{\"d\":1}{\"d\":2}]", WARM_CORRUPT, 1, 1},
      {"number element", "[{\"d\":1},2]", WARM_CORRUPT, 1, 1},
      {"array element", "[[1]]", WARM_CORRUPT, 0, 0},
      {"not an array", "{\"d\":1}", WARM_CORRUPT, 0, 0},
      {"empty file", "", WARM_CORRUPT, 0, 0},
      {"invalid json in a balanced element", "[{\"d\":1},{\"d\":}]", WARM_CORRUPT, 1, 1},
      {"unterminated", "[{\"d\":1},{\"d\":2", WARM_CORRUPT, 1, 1},
      {"leading comma", "[,{\"d\":1}]", WARM_CORRUPT, 0, 0},
      {"1024-byte element", "[" + elementOfSize(1024) + "]", WARM_OK, 1, 1},
      {"1025-byte element", "[" + elementOfSize(1025) + "]", WARM_CORRUPT, 0, 0},
  };
  for (const Case &c : cases) {
    writeFile(path, c.text);
    resetHooks();
    Collected got;
    uint32_t rows = 0;
    const WarmStatus st = scanPath(path, false, big, &got, &rows);
    CHECK_MSG(st == c.strict, "%s: strict %s, expected %s", c.name, warmStatusName(st), warmStatusName(c.strict));
    CHECK_MSG(got.raw.size() == c.rows && rows == c.rows, "%s: %zu rows", c.name, got.raw.size());
    Collected salv;
    const WarmStatus ss = scanPath(path, true, big, &salv);
    CHECK_MSG(ss == WARM_OK && salv.raw.size() == c.salvageRows, "%s: salvage %s with %zu rows", c.name,
              warmStatusName(ss), salv.raw.size());
  }

  // Raw element bytes are returned verbatim
  writeFile(path, "[ {\"c\":\"a\\\"}{[\"} , {\"n\":1.50,\"tr\":7} ]");
  Collected raw;
  CHECK(scanPath(path, false, big, &raw) == WARM_OK && raw.raw.size() == 2 && raw.raw[0] == "{\"c\":\"a\\\"}{[\"}" &&
        raw.raw[1] == "{\"n\":1.50,\"tr\":7}");

  // Missing file is ABSENT; a file over the cap is TOO_BIG without a single read
  removeFile(path);
  CHECK(scanPath(path, false, big, nullptr) == WARM_ABSENT);
  writeFile(path, "[{\"d\":1},{\"d\":2}]");
  resetHooks();
  CHECK(scanPath(path, false, 10, nullptr) == WARM_TOO_BIG);
  CHECK(fi::state().perOp[fi::OP_FREAD] == 0);
  // Salvage reads only up to the cap
  Collected capped;
  CHECK(scanPath(path, true, 10, &capped) == WARM_OK && capped.raw.size() == 1);

  // NULL visitor counts; a visitor returning false stops with OK; yield every 32 rows
  std::vector<std::string> many;
  for (int i = 0; i < 100; ++i) many.push_back(fmtStr("{\"d\":%d}", i));
  writeFile(path, joinArray(many));
  resetHooks();
  uint32_t counted = 0;
  CHECK(scanPath(path, false, big, nullptr, &counted) == WARM_OK && counted == 100);
  CHECK(gYieldCalls == 3);
  Collected stop;
  stop.stopAfter = 3;
  uint32_t stopRows = 0;
  CHECK(scanPath(path, false, big, &stop, &stopRows) == WARM_OK && stop.raw.size() == 3 && stopRows == 3);
  {
    const WarmHooks h = makeHooks(nullptr);
    uint32_t limited = 0;
    CHECK(warmScanFile(path.c_str(), false, big, 7, nullptr, nullptr, h, &limited, nullptr) == WARM_OK && limited == 7);
  }

  // A full 20-sensor month (620 rows, ~76 KB): past both old size limits
  const std::string month = fleetMonthFixture(20, 2026, 10, nullptr);
  writeFile(path, month);
  Collected all;
  CHECK(scanPath(path, false, big, &all) == WARM_OK && all.raw.size() == 620);
  CHECK_MSG(month.size() > 16384 && month.size() < 100000, "fixture is %zu bytes", month.size());
  CHECK(joinArray(all.raw) == month);
  removeFile(path);
}

// ============================================================================
// Truncation sweep
// ============================================================================
static void testTruncation() {
  const std::string dir = makeDir("truncation");
  const std::string path = joinPath(dir, "t.json");
  std::vector<size_t> ends;
  const std::string full = fleetMonthFixture(20, 2026, 10, &ends);
  const size_t len = full.size();
  const uint32_t big = 1u << 20;

  std::vector<size_t> cuts;
  for (size_t k = 0; k < len; ++k) {
    const bool edge = k < 2048 || k >= len - 2048;
    if ((edge && k % WARM_TEST_EDGE_STRIDE == 0) || (!edge && k % 97 == 0)) cuts.push_back(k);
  }
  long bad = 0;
  for (size_t k : cuts) {
    writeFile(path, full.substr(0, k));
    uint32_t strictRows = 0;
    const WarmStatus st = scanPath(path, false, big, nullptr, &strictRows);
    const bool strictOk = (k == 0) ? st == WARM_CORRUPT : (st != WARM_OK && st != WARM_IO_ERROR);
    Collected salv;
    const WarmStatus ss = scanPath(path, true, big, &salv);
    const size_t complete = (size_t)(std::upper_bound(ends.begin(), ends.end(), k) - ends.begin());
    bool salvageOk = ss == WARM_OK && salv.raw.size() == complete;
    for (size_t i = 0; salvageOk && i < salv.raw.size(); ++i) {
      salvageOk = full.compare(ends[i] - salv.raw[i].size(), salv.raw[i].size(), salv.raw[i]) == 0;
    }
    if ((!strictOk || !salvageOk) && ++bad <= 5) {
      CHECK_MSG(false, "cut at %zu: strict %s, salvage %s with %zu rows (expected %zu)", k, warmStatusName(st),
                warmStatusName(ss), salv.raw.size(), complete);
    }
  }
  CHECK_MSG(bad == 0, "%ld bad cuts of %zu", bad, cuts.size());

  writeFile(path, full);
  CHECK(scanPath(path, false, big, nullptr) == WARM_OK);

  // A file that ends before its size (early EOF) is an I/O error, never corrupt
  resetHooks();
  fi::state().readBudget = (long)(len / 2);
  CHECK(scanPath(path, false, big, nullptr) == WARM_IO_ERROR);
  resetHooks();
  fi::state().readBudget = (long)(len / 2);
  CHECK(scanPath(path, true, big, nullptr) == WARM_IO_ERROR);

  // A read error is an I/O error, in strict and salvage mode
  resetHooks();
  fi::state().failAt = 6;  // fopen, fseek, ftell, fseek, fread, fread, [fread]
  CHECK(scanPath(path, false, big, nullptr) == WARM_IO_ERROR);
  CHECK(fi::state().failedOp == fi::OP_FREAD);
  resetHooks();
  fi::state().failAt = 6;
  CHECK(scanPath(path, true, big, nullptr) == WARM_IO_ERROR);
  resetHooks();
  fi::state().failAt = 2;  // ftell
  CHECK(scanPath(path, false, big, nullptr) == WARM_IO_ERROR);
  resetHooks();
  fi::state().failAt = 0;  // fopen of an existing file
  CHECK(scanPath(path, false, big, nullptr) == WARM_IO_ERROR);

  // Size max+1: TOO_BIG without reading; salvage keeps every complete row
  resetHooks();
  CHECK(scanPath(path, false, (uint32_t)len - 1, nullptr) == WARM_TOO_BIG);
  CHECK(fi::state().perOp[fi::OP_FREAD] == 0);
  Collected salv;
  CHECK(scanPath(path, true, (uint32_t)len - 1, &salv) == WARM_OK && salv.raw.size() == ends.size());
  removeFile(path);
}

// ============================================================================
// Differential test against the v2.2.15 rollup
// ============================================================================
static void testDifferential() {
  const std::string dirA = makeDir("diff_a");
  const std::string dirB = makeDir("diff_b");
  std::mt19937 rng(20260922u);
  auto real = [&rng](double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); };
  auto integer = [&rng](int a, int b) { return std::uniform_int_distribution<int>(a, b)(rng); };
  const WarmHooks h = makeHooks(nullptr);
  const uint32_t big = 1u << 20;
  std::vector<WarmNewRow> rows(WARM_MAX_DAYS_PER_BATCH * 20);

  long compared = 0, nanCases = 0, bad = 0;
  for (int iter = 0; iter < 2000; ++iter) {
    const int32_t dn = dnOf(2025, 1, 1) + integer(0, 1000);
    const double dayBegin = (double)dn * 86400.0;
    const bool nanCase = integer(0, 9) == 0;
    Fleet fleet;
    const int sensors = integer(1, 5);
    for (int s = 0; s < sensors; ++s) {
      fleet.rings.push_back(LegacyRing(fmtStr("dev:1234%02d", s), (uint8_t)integer(1, 4), 90));
      const int snaps = integer(0, 150);  // > 90 wraps the ring
      for (int j = 0; j < snaps; ++j) {
        double ts;
        switch (integer(0, 9)) {
          case 0: ts = dayBegin; break;
          case 1: ts = dayBegin + 86400.0; break;
          case 2: ts = dayBegin - 0.001; break;
          case 3: ts = dayBegin + 86399.999; break;
          default: ts = real(dayBegin - 86400.0, dayBegin + 2 * 86400.0); break;
        }
        float level = (float)(round(real(-10.0, 250.0) * 100.0) / 100.0);
        if (nanCase && integer(0, 9) == 0) level = NAN;
        const float volt = integer(0, 3) == 0 ? 0.0f : (float)real(10.0, 14.5);
        fleet.rings.back().add(ts, level, volt);
      }
    }
    for (int a = integer(0, 5); a > 0; --a) {
      LegacyAlarm al = {fmtStr("dev:1234%02d", integer(0, sensors - 1)), (uint8_t)integer(1, 4),
                        real(dayBegin - 3600.0, dayBegin + 90000.0)};
      fleet.alarms.push_back(al);
    }

    const uint16_t n = warmComputeRows(fleet.series(), fleet.count(), dn, dn, testAlarmCount, &fleet.alarms,
                                       testRoundTo, rows.data(), (uint16_t)rows.size());
    int y, m, d;
    warmDnToCivil(dn, &y, &m, &d);
    const std::string path = monthFile(dirA, y, m);
    removeFile(path);

    bool hasNan = false;
    for (const LegacyRing &r : fleet.rings) {
      for (uint16_t j = 0; j < r.snapshotCount; ++j) {
        const TelemetrySnapshot &s = r.snapshots[(size_t)((r.writeIndex - r.snapshotCount + j + r.cap()) % r.cap())];
        if (isnan(s.level) && s.timestamp >= dayBegin && s.timestamp < dayBegin + 86400.0) hasNan = true;
      }
    }
    if (hasNan) {
      // Non-finite levels are skipped: n counts only finite in-day snapshots and nothing serializes as null
      nanCases++;
      for (uint16_t i = 0; i < n; ++i) {
        long finite = 0;
        for (const LegacyRing &r : fleet.rings) {
          if (strcmp(r.clientUid.c_str(), rows[i].c) != 0) continue;
          for (uint16_t j = 0; j < r.snapshotCount; ++j) {
            const TelemetrySnapshot &s = r.snapshots[(size_t)((r.writeIndex - r.snapshotCount + j + r.cap()) % r.cap())];
            if (!isnan(s.level) && s.timestamp >= dayBegin && s.timestamp < dayBegin + 86400.0) finite++;
          }
        }
        CHECK(rows[i].n == finite && encodeRow(rows[i]).find("null") == std::string::npos);
      }
      continue;
    }

    const std::string legacy = legacyRollupDay(fleet.rings, fleet.alarms, warmDnToYmd(dn), dayBegin, testRoundTo);
    if (legacy.empty()) {
      CHECK(n == 0);
      continue;
    }
    WarmMergeResult res = {};
    const WarmStatus st = warmMergeMonth(dirA.c_str(), y, m, rows.data(), n, big, h, &res);
    const std::string got = readFile(path);
    compared++;
    if ((st != WARM_OK || !res.wrote || got != legacy) && ++bad <= 3) {
      CHECK_MSG(false, "case %d differs:\n  new    %s\n  legacy %s", iter, got.c_str(), legacy.c_str());
    }
  }
  CHECK_MSG(bad == 0, "%ld of %ld cases differ", bad, compared);
  CHECK(compared > 1000 && nanCases > 50);
  clearDir(dirA);

  // A multi-day batch writes exactly the bytes of repeated single-day runs
  bad = 0;
  for (int iter = 0; iter < 200; ++iter) {
    clearDir(dirA);
    clearDir(dirB);
    const int y = 2026, m = integer(1, 12);
    const int32_t first = dnOf(y, m, integer(1, 20));
    const int32_t lastDn = first + WARM_MAX_DAYS_PER_BATCH - 1;
    Fleet fleet;
    const int sensors = integer(1, 6);
    for (int s = 0; s < sensors; ++s) {
      fleet.rings.push_back(LegacyRing(fmtStr("dev:5678%02d", s), (uint8_t)(s % 4 + 1), 90));
      for (int j = integer(0, 90); j > 0; --j) {
        fleet.rings.back().add(real((double)first * 86400.0, (double)(lastDn + 1) * 86400.0),
                               (float)real(0.0, 100.0), (float)real(11.0, 13.0));
      }
    }
    for (int a = integer(0, 8); a > 0; --a) {
      LegacyAlarm al = {fmtStr("dev:5678%02d", integer(0, sensors - 1)), (uint8_t)integer(1, 4),
                        real((double)first * 86400.0, (double)(lastDn + 1) * 86400.0)};
      fleet.alarms.push_back(al);
    }
    for (int32_t dn = first; dn <= lastDn; ++dn) {
      const uint16_t n = warmComputeRows(fleet.series(), fleet.count(), dn, dn, testAlarmCount, &fleet.alarms,
                                         testRoundTo, rows.data(), (uint16_t)rows.size());
      if (n > 0) CHECK(warmMergeMonth(dirA.c_str(), y, m, rows.data(), n, big, h, nullptr) == WARM_OK);
    }
    const uint16_t n = warmComputeRows(fleet.series(), fleet.count(), first, lastDn, testAlarmCount, &fleet.alarms,
                                       testRoundTo, rows.data(), (uint16_t)rows.size());
    if (n > 0) CHECK(warmMergeMonth(dirB.c_str(), y, m, rows.data(), n, big, h, nullptr) == WARM_OK);
    if (readFile(monthFile(dirA, y, m)) != readFile(monthFile(dirB, y, m)) && ++bad <= 3) {
      CHECK_MSG(false, "multi-day batch %d differs from single-day runs", iter);
    }
  }
  CHECK(bad == 0);
  clearDir(dirA);
  clearDir(dirB);
}

// ============================================================================
// Merge semantics
// ============================================================================
static void testMergeSemantics() {
  const std::string dir = makeDir("merge");
  const std::string path = monthFile(dir, 2026, 9);
  const uint32_t big = 1u << 20;
  WarmHooks h = makeHooks(nullptr);
  WarmMergeResult res = {};

  // (1) absent file: a new file with exactly the encoded rows
  resetHooks();
  WarmNewRow a = makeRow("dev:a", 20260901, 1, 4, 10.0f, 0);
  WarmNewRow b = makeRow("dev:b", 20260901, 2, 5, 20.0f, 1);
  WarmNewRow rows1[] = {a, b};
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, rows1, 2, big, h, &res) == WARM_OK && res.wrote);
  CHECK(readFile(path) == joinArray({encodeRow(a), encodeRow(b)}));
  CHECK(!anyTmp(dir) && gNotes.size() == 1 && gNotes[0].ok && gNotes[0].path == path &&
        gNotes[0].bytes == readFile(path).size());

  // (2)(3) rows for other days, unknown keys and number formatting survive byte for byte
  const std::string oddRow = "{\"d\":20260901,\"c\":\"dev:x\",\"k\":3,\"mn\":1.50,\"tr\":7,\"n\":3}";
  const std::string existing = "[" + encodeRow(a) + ", " + oddRow + "]";
  writeFile(path, existing);
  WarmNewRow c = makeRow("dev:a", 20260902, 1, 4, 11.0f, 0);
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &c, 1, big, h, &res) == WARM_OK && res.wrote);
  CHECK(readFile(path) == joinArray({encodeRow(a), oddRow, encodeRow(c)}));

  // (4) existing n >= new n: no write at all
  resetHooks();
  const std::string before = readFile(path);
  WarmNewRow same = makeRow("dev:a", 20260902, 1, 4, 99.0f, 0);
  WarmNewRow fewer = makeRow("dev:x", 20260901, 3, 2, 99.0f, 0);
  WarmNewRow rows4[] = {same, fewer};
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, rows4, 2, big, h, &res) == WARM_OK && !res.wrote);
  CHECK(readFile(path) == before && fi::state().perOp[fi::OP_FWRITE] == 0 && fi::state().writeOpens.empty());
  CHECK(rows4[0].state == WARM_ROW_SUPERSEDED && rows4[1].state == WARM_ROW_SUPERSEDED);

  // (5) larger n replaces the row; al becomes max(old, new)
  WarmNewRow oldRow = makeRow("dev:r", 20260903, 1, 5, 30.0f, 3);
  writeFile(path, joinArray({encodeRow(oldRow), encodeRow(a)}));
  WarmNewRow better = makeRow("dev:r", 20260903, 1, 6, 31.0f, 1);
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &better, 1, big, h, &res) == WARM_OK && res.wrote);
  WarmNewRow expect5 = better;
  expect5.al = 3;
  CHECK(readFile(path) == joinArray({encodeRow(a), encodeRow(expect5)}));

  // (5b) S-D03: the same n and a higher al (an alarm that arrived after the day was rolled
  // up) also replaces the row; the same n and a lower al does not
  WarmNewRow moreAl = expect5;
  moreAl.al = 4;
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &moreAl, 1, big, h, &res) == WARM_OK && res.wrote);
  CHECK(moreAl.state == WARM_ROW_REPLACES && readFile(path) == joinArray({encodeRow(a), encodeRow(moreAl)}));
  resetHooks();
  WarmNewRow lessAl = expect5;
  lessAl.al = 2;
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &lessAl, 1, big, h, &res) == WARM_OK && !res.wrote);
  CHECK(lessAl.state == WARM_ROW_SUPERSEDED && fi::state().perOp[fi::OP_FWRITE] == 0);
  CHECK(readFile(path) == joinArray({encodeRow(a), encodeRow(moreAl)}));

  // (6) duplicate keys in the file: any n >= new n makes it SUPERSEDED ...
  WarmNewRow dupLow = makeRow("dev:d", 20260904, 2, 4, 1.0f, 0);
  WarmNewRow dupHigh = makeRow("dev:d", 20260904, 2, 9, 2.0f, 0);
  writeFile(path, joinArray({encodeRow(dupLow), encodeRow(dupHigh)}));
  WarmNewRow mid = makeRow("dev:d", 20260904, 2, 6, 3.0f, 0);
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &mid, 1, big, h, &res) == WARM_OK && !res.wrote);
  // ... and when every copy is worse, all copies go and one row is written
  WarmNewRow dup1 = makeRow("dev:d", 20260904, 2, 2, 1.0f, 4);
  WarmNewRow dup2 = makeRow("dev:d", 20260904, 2, 3, 2.0f, 1);
  writeFile(path, joinArray({encodeRow(dup1), encodeRow(a), encodeRow(dup2)}));
  WarmNewRow top = makeRow("dev:d", 20260904, 2, 6, 3.0f, 2);
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &top, 1, big, h, &res) == WARM_OK && res.wrote);
  WarmNewRow expect6 = top;
  expect6.al = 4;
  CHECK(readFile(path) == joinArray({encodeRow(a), encodeRow(expect6)}));

  // (7) rows of a sensor that left the hot tier are kept (v2.2.15 deleted every row of the date)
  WarmNewRow gone = makeRow("dev:gone", 20260905, 1, 7, 5.0f, 0);
  writeFile(path, joinArray({encodeRow(gone)}));
  WarmNewRow other = makeRow("dev:here", 20260905, 1, 7, 6.0f, 0);
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &other, 1, big, h, &res) == WARM_OK && res.wrote);
  CHECK(readFile(path) == joinArray({encodeRow(gone), encodeRow(other)}));

  // (8) a result over maxFileBytes is TOO_BIG and leaves the file alone
  resetHooks();
  const std::string beforeBig = readFile(path);
  WarmNewRow more = makeRow("dev:more", 20260906, 1, 7, 5.0f, 0);
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &more, 1, (uint32_t)beforeBig.size() + 10, h, &res) == WARM_TOO_BIG);
  CHECK(readFile(path) == beforeBig && !anyTmp(dir) && countLogs("would exceed") == 1);
  // ... also when the existing rows alone are over the cap after a salvage
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &more, 1, 5, h, &res) == WARM_TOO_BIG || readFile(path) == beforeBig);
  CHECK(readFile(path) == beforeBig);

  // A row too long to encode is skipped with an error; the others are stored
  resetHooks();
  removeFile(path);
  const std::string longUid(300, 'u');
  WarmNewRow tooLong = makeRow(longUid.c_str(), 20260907, 1, 3, 1.0f, 0);
  WarmNewRow fine = makeRow("dev:fine", 20260907, 1, 3, 1.0f, 0);
  WarmNewRow rowsLong[] = {tooLong, fine};
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, rowsLong, 2, big, h, &res) == WARM_OK && res.wrote);
  CHECK(readFile(path) == joinArray({encodeRow(fine)}) && countLogs("not stored") == 1);
  CHECK(rowsLong[0].state == WARM_ROW_TOO_LONG);

  // A leftover .tmp from a power cut is removed by the next merge (see testTmpRecovery)
  writeFile(path + ".tmp", "[{\"partial\":");
  WarmNewRow again = makeRow("dev:fine", 20260907, 1, 3, 1.0f, 0);
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &again, 1, big, h, &res) == WARM_OK && !res.wrote);
  CHECK(!anyTmp(dir));

  // S-D03: a row too long to encode that would replace a stored row (larger n) leaves
  // the stored row in place; it used to drop it and write neither. The stored row can
  // be longer than we write (rows are read up to WARM_ELEM_MAX).
  resetHooks();
  const std::string storedLong = fmtStr("{\"d\":20260908,\"c\":\"%s\",\"k\":1,\"mn\":1,\"n\":3}", longUid.c_str());
  writeFile(path, joinArray({storedLong, encodeRow(fine)}));
  const std::string beforeLong = readFile(path);
  WarmNewRow longer = makeRow(longUid.c_str(), 20260908, 1, 9, 2.0f, 0);
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &longer, 1, big, h, &res) == WARM_OK && !res.wrote);
  CHECK(longer.state == WARM_ROW_TOO_LONG && readFile(path) == beforeLong && countLogs("not stored") == 1);
  CHECK(fi::state().writeOpens.empty());
  // ... also when the same batch rewrites the file
  WarmNewRow pair[] = {longer, makeRow("dev:fine2", 20260908, 1, 3, 1.0f, 0)};
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, pair, 2, big, h, &res) == WARM_OK && res.wrote);
  CHECK(readFile(path) == joinArray({storedLong, encodeRow(fine), encodeRow(pair[1])}));
  // ... and when it fits only without the stored row's al, which a replacement carries
  // over: it is measured with the largest al (so a row this close to the limit is not
  // stored even when nothing is replaced)
  resetHooks();
  const size_t baseLen = encodeRow(makeRow("", 20260909, 1, 3, 1.0f, 0)).size();
  const std::string edgeUid(WARM_ROW_MAX_WRITE - 1 - baseLen, 'E');
  WarmNewRow edge = makeRow(edgeUid.c_str(), 20260909, 1, 3, 1.0f, 0);
  CHECK(encodeRow(edge).size() == WARM_ROW_MAX_WRITE - 1);  // fits with al 0
  const std::string storedEdge = fmtStr("{\"d\":20260909,\"c\":\"%s\",\"k\":1,\"al\":200,\"n\":2}", edgeUid.c_str());
  writeFile(path, joinArray({storedEdge}));
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &edge, 1, big, h, &res) == WARM_OK && !res.wrote);
  CHECK(edge.state == WARM_ROW_TOO_LONG && readFile(path) == joinArray({storedEdge}) && countLogs("not stored") == 1);
  clearDir(dir);
}

// ============================================================================
// Quarantine and salvage
// ============================================================================
static void testQuarantine() {
  const std::string dir = makeDir("quarantine");
  const std::string path = monthFile(dir, 2026, 9);
  const std::string bad = monthFile(dir, 2026, 9, ".bad");
  const uint32_t big = 1u << 20;
  const WarmHooks h = makeHooks(nullptr);
  WarmMergeResult res = {};

  const WarmNewRow r1 = makeRow("dev:q", 20260901, 1, 5, 1.0f, 0);
  const WarmNewRow r2 = makeRow("dev:q", 20260902, 1, 5, 2.0f, 0);
  const WarmNewRow r3 = makeRow("dev:q", 20260903, 1, 5, 3.0f, 0);
  const std::string corrupt = "[" + encodeRow(r1) + "," + encodeRow(r2) + "," + encodeRow(r3) + ",{\"d\":2026";

  // Corrupt file, no .bad yet: .bad = old bytes; new file = salvaged prefix + new rows
  resetHooks();
  writeFile(path, corrupt);
  WarmNewRow add = makeRow("dev:q", 20260904, 1, 5, 4.0f, 0);
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &add, 1, big, h, &res) == WARM_OK);
  CHECK(res.wrote && res.quarantined && res.salvagedRows == 3);
  CHECK(readFile(bad) == corrupt);
  CHECK(readFile(path) == joinArray({encodeRow(r1), encodeRow(r2), encodeRow(r3), encodeRow(add)}));
  CHECK(countLogs("unreadable") == 1 && countLogs("rebuilt; kept 3 rows") == 1 && !anyTmp(dir));

  // Corrupt again with an older .bad present: the .bad is replaced, no permanent failure
  resetHooks();
  const std::string corrupt2 = "[" + encodeRow(r1) + ",garbage";
  writeFile(path, corrupt2);
  WarmNewRow add2 = makeRow("dev:q", 20260905, 1, 5, 5.0f, 0);
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &add2, 1, big, h, &res) == WARM_OK && res.quarantined);
  CHECK(readFile(bad) == corrupt2 && readFile(path) == joinArray({encodeRow(r1), encodeRow(add2)}));
  WarmNewRow add3 = makeRow("dev:q", 20260906, 1, 5, 6.0f, 0);
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &add3, 1, big, h, &res) == WARM_OK && res.wrote && !res.quarantined);
  CHECK(readFile(path) == joinArray({encodeRow(r1), encodeRow(add2), encodeRow(add3)}));

  // Power cut between quarantine and rebuild (file gone, .bad present): salvage from .bad
  resetHooks();
  removeFile(path);
  writeFile(bad, corrupt);
  WarmNewRow add4 = makeRow("dev:q", 20260902, 1, 5, 9.0f, 0);  // same key as r2, same n: superseded
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &add4, 1, big, h, &res) == WARM_OK && res.wrote);
  CHECK(readFile(path) == joinArray({encodeRow(r1), encodeRow(r2), encodeRow(r3)}) && readFile(bad) == corrupt);

  // A failed rename(file, .bad) is an I/O error and changes nothing
  clearDir(dir);
  writeFile(path, corrupt);
  resetHooks();
  WarmNewRow add5 = makeRow("dev:q", 20260907, 1, 5, 7.0f, 0);
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &add5, 1, big, h, &res) == WARM_OK);
  long firstRename = -1;
  {
    // find the index of the rename(file, .bad) in a clean run
    clearDir(dir);
    writeFile(path, corrupt);
    resetHooks();
    fi::state().failAt = -1;
    long idx = -1;
    for (long probe = 0; probe < 400 && idx < 0; ++probe) {
      clearDir(dir);
      writeFile(path, corrupt);
      resetHooks();
      fi::state().failAt = probe;
      WarmNewRow r = add5;
      warmMergeMonth(dir.c_str(), 2026, 9, &r, 1, big, h, &res);
      if (fi::state().failedOp == fi::OP_RENAME) idx = probe;
    }
    firstRename = idx;
  }
  CHECK(firstRename >= 0);
  clearDir(dir);
  writeFile(path, corrupt);
  resetHooks();
  fi::state().failAt = firstRename;
  WarmNewRow add6 = add5;
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &add6, 1, big, h, &res) == WARM_IO_ERROR);
  CHECK(readFile(path) == corrupt && !fileExists(bad) && !anyTmp(dir));

  // A failed rename(.tmp, file) puts the original back
  clearDir(dir);
  writeFile(path, corrupt);
  resetHooks();
  WarmNewRow add9 = add5;
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &add9, 1, big, h, &res) == WARM_OK && res.quarantined);
  const std::string rebuilt = readFile(path);
  const std::vector<long> renames = opIndices(fi::OP_RENAME);
  CHECK(renames.size() == 2);  // file -> .bad, .tmp -> file
  if (renames.size() == 2) {
    clearDir(dir);
    writeFile(path, corrupt);
    resetHooks();
    fi::state().failAt = renames[1];
    WarmNewRow add10 = add5;
    CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &add10, 1, big, h, &res) == WARM_IO_ERROR);
    CHECK(readFile(path) == corrupt && !fileExists(bad) && !anyTmp(dir));

    // ... and when putting it back fails too, the complete .tmp is kept beside
    // the .bad (the state a power cut between the renames leaves), and the next
    // merge restores it: it already holds the rows salvaged from the .bad (S-D03)
    clearDir(dir);
    writeFile(path, corrupt);
    resetHooks();
    fi::state().failAt = renames[1];
    fi::state().failAt2 = renames[1] + 1;
    WarmNewRow add11 = add5;
    CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &add11, 1, big, h, &res) == WARM_IO_ERROR);
    const std::vector<long> tried = opIndices(fi::OP_RENAME);
    CHECK(tried.size() == 3 && tried[2] == renames[1] + 1);
    CHECK(!fileExists(path) && readFile(bad) == corrupt && readFile(path + ".tmp") == rebuilt);
    resetHooks();
    WarmNewRow add12 = add5;
    CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &add12, 1, big, h, &res) == WARM_OK && !res.wrote);
    CHECK(readFile(path) == rebuilt && readFile(bad) == corrupt && !anyTmp(dir));
    CHECK(countLogs("restored from its .tmp (4 rows)") == 1 && fi::state().writeOpens.empty());
  }

  // A file over the size cap is quarantined and its first maxBytes salvaged
  clearDir(dir);
  std::vector<std::string> rowsText;
  for (int d = 1; d <= 20; ++d) rowsText.push_back(encodeRow(makeRow("dev:big", 20260900u + (uint32_t)d, 1, 5, 1.0f, 0)));
  const std::string oversize = joinArray(rowsText);
  writeFile(path, oversize);
  resetHooks();
  WarmNewRow add7 = makeRow("dev:new", 20260921, 1, 5, 1.0f, 0);
  const uint32_t cap = (uint32_t)(oversize.size() - 100);
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &add7, 1, cap + 400, h, &res) == WARM_OK && !res.quarantined);
  clearDir(dir);
  writeFile(path, oversize);
  WarmNewRow add8 = add7;
  const WarmStatus st = warmMergeMonth(dir.c_str(), 2026, 9, &add8, 1, cap, h, &res);
  CHECK(st == WARM_TOO_BIG || (st == WARM_OK && res.quarantined));
  CHECK(readFile(path) == oversize || readFile(bad) == oversize);
  clearDir(dir);
}

// ============================================================================
// Scheduler (L-29)
// ============================================================================
static void testScheduler() {
  const std::string dir = makeDir("sched");
  const WarmRollupConfig cfg = defaultConfig(dir);
  const WarmHooks h = makeHooks(nullptr);

  // (1) cursor 0 with a 45-day ring converges in ascending order
  {
    resetHooks();
    Fleet fleet;
    fleet.addSensors(2, 90);
    const int32_t today = dnOf(2026, 9, 20);
    fleet.fill(today - 45, today - 1, 2, 50.0f);
    WarmRollupState s;
    const double now = (double)today * 86400.0 + 3600.0;
    uint32_t lastYmd = 0;
    int ticks = 0;
    for (; ticks < 30; ++ticks) {
      const WarmTickSummary sum = tick(s, fleet, now, cfg, h);
      if (sum.batches == 0) break;
      CHECK(sum.writes <= cfg.maxWrites && sum.batches <= cfg.maxBatches);
      CHECK(sum.firstYmd > lastYmd && sum.lastYmd >= sum.firstYmd);
      if (ticks == 0) CHECK(sum.writes == 2);
      lastYmd = sum.lastYmd;
    }
    CHECK(ticks > 1 && ticks < 30);
    CHECK(s.nextDn == today && s.cursorYmd == warmDnToYmd(today - 1));
    // every day of the window is present, and each month file only holds its own month
    const auto aug = fileKeys(monthFile(dir, 2026, 8));
    const auto sep = fileKeys(monthFile(dir, 2026, 9));
    long missing = 0;
    for (int32_t dn = today - 45; dn <= today - 1; ++dn) {
      int y, m, d;
      warmDnToCivil(dn, &y, &m, &d);
      const auto &keys = (m == 8) ? aug : sep;
      for (const LegacyRing &r : fleet.rings) {
        auto it = keys.find(keyOf(warmDnToYmd(dn), r.clientUid.c_str(), r.sensorIndex));
        if (it == keys.end() || it->second.size() != 1 || it->second[0] != 2) missing++;
      }
    }
    CHECK_MSG(missing == 0, "%ld rows missing", missing);
    for (const auto &kv : aug) CHECK(kv.first.compare(0, 6, "202608") == 0);
    for (const auto &kv : sep) CHECK(kv.first.compare(0, 6, "202609") == 0);
    CHECK(aug.size() + sep.size() == 90);

    // (7) the same data again after a "reboot": reads only, zero writes
    const std::string augBefore = readFile(monthFile(dir, 2026, 8));
    const std::string sepBefore = readFile(monthFile(dir, 2026, 9));
    resetHooks();
    s.nextDn = -1;
    s.stats = WarmRollupStats();
    uint32_t writes = 0;
    CHECK(tickUntilIdle(s, fleet, now, cfg, h, &writes) >= 1);
    CHECK(writes == 0 && fi::state().perOp[fi::OP_FWRITE] == 0 && fi::state().writeOpens.empty());
    CHECK(s.stats.noChange > 0 && s.stats.writes == 0);
    CHECK(readFile(monthFile(dir, 2026, 8)) == augBefore && readFile(monthFile(dir, 2026, 9)) == sepBefore);
    clearDir(dir);
  }

  // (2) a 10-day outage across a month boundary fills both month files
  {
    resetHooks();
    Fleet fleet;
    fleet.addSensors(3, 90);
    fleet.fill(dnOf(2026, 8, 20), dnOf(2026, 8, 27), 4, 10.0f);
    WarmRollupState s;
    CHECK(tickUntilIdle(s, fleet, epochOf(2026, 8, 28, 1800.0), cfg, h) >= 1);
    CHECK(s.cursorYmd == 20260827u && s.nextDn == dnOf(2026, 8, 28));
    fleet.fill(dnOf(2026, 8, 28), dnOf(2026, 9, 7), 4, 10.0f);  // received after the outage
    CHECK(tickUntilIdle(s, fleet, epochOf(2026, 9, 8, 1800.0), cfg, h) >= 1);
    const auto aug = fileKeys(monthFile(dir, 2026, 8));
    const auto sep = fileKeys(monthFile(dir, 2026, 9));
    CHECK(aug.size() == 12 * 3 && sep.size() == 7 * 3);
    CHECK(aug.count(keyOf(20260831u, fleet.rings[0].clientUid.c_str(), fleet.rings[0].sensorIndex)) == 1);
    CHECK(s.cursorYmd == 20260907u);
    clearDir(dir);
  }

  // (3) per-tick limits: writes, batches and the time budget; work resumes next tick
  {
    resetHooks();
    Fleet fleet;
    fleet.addSensors(2, 90);
    const int32_t today = dnOf(2026, 9, 20);
    fleet.fill(today - 40, today - 1, 2, 5.0f);
    WarmRollupConfig small = cfg;
    small.maxBatches = 3;
    small.maxWrites = 100;
    WarmRollupState s;
    const double now = (double)today * 86400.0 + 7200.0;
    WarmTickSummary sum = tick(s, fleet, now, small, h);
    CHECK(sum.batches == 3);
    const int32_t resumeAt = s.nextDn;
    sum = tick(s, fleet, now, small, h);
    CHECK(sum.batches == 3 && sum.firstYmd == warmDnToYmd(resumeAt));
    // 3 s per nowMs() call against an 8 s budget: two batches, then stop
    // (a "reboot" first, so the whole window is due again)
    CHECK(tickUntilIdle(s, fleet, now, small, h) >= 0 && s.nextDn == today);
    s.nextDn = -1;
    gClockStep = 3000;
    sum = tick(s, fleet, now, cfg, h);
    CHECK(sum.batches == 2);
    gClockStep = 0;
    CHECK(s.stats.lastTickMs >= 8000 && s.stats.maxTickMs >= 8000);
    CHECK(tickUntilIdle(s, fleet, now, cfg, h) >= 1 && s.nextDn == today);
    clearDir(dir);
  }

  // (4) the backfill floor: 92 days, retained months, oldest snapshot
  {
    resetHooks();
    Fleet fleet;
    fleet.addSensors(1, 400);
    const int32_t today = dnOf(2026, 9, 30);
    fleet.fill(today - 130, today - 1, 1, 7.0f);
    WarmRollupState s;
    const double now = (double)today * 86400.0 + 600.0;
    CHECK(tickUntilIdle(s, fleet, now, cfg, h) >= 1);
    // yesterday - 91 = Jun 30 (the retained floor is Jun 1)
    auto jun = fileKeys(monthFile(dir, 2026, 6));
    CHECK(jun.size() == 1 && jun.begin()->first.compare(0, 8, "20260630") == 0);
    CHECK(!fileExists(monthFile(dir, 2026, 5)));
    clearDir(dir);

    WarmRollupConfig wide = cfg;
    wide.maxBackfillDays = 200;
    WarmRollupState s2;
    CHECK(tickUntilIdle(s2, fleet, now, wide, h) >= 1);
    jun = fileKeys(monthFile(dir, 2026, 6));
    CHECK(jun.size() == 30 && !fileExists(monthFile(dir, 2026, 5)));  // retained months bind: Jun 1
    clearDir(dir);

    Fleet young;
    young.addSensors(1, 400);
    young.fill(today - 10, today - 1, 1, 7.0f);
    WarmRollupState s3;
    CHECK(tickUntilIdle(s3, young, now, wide, h) >= 1);
    CHECK(fileKeys(monthFile(dir, 2026, 9)).size() == 10 && !fileExists(monthFile(dir, 2026, 8)));
    clearDir(dir);
  }

  // (5) no snapshots: the cursor advances with no I/O
  {
    resetHooks();
    Fleet empty;
    WarmRollupState s;
    s.cursorYmd = 20260801u;
    tick(s, empty, epochOf(2026, 9, 20, 100.0), cfg, h);
    CHECK(s.cursorYmd == 20260919u && s.nextDn == dnOf(2026, 9, 20) && fi::state().ops == 0);
  }

  // (6) a clock that goes backwards resets the position and pulls the cursor back
  {
    resetHooks();
    Fleet fleet;
    fleet.addSensors(1, 90);
    fleet.fill(dnOf(2026, 9, 1), dnOf(2026, 9, 19), 2, 3.0f);
    WarmRollupState s;
    CHECK(tickUntilIdle(s, fleet, epochOf(2026, 9, 20, 100.0), cfg, h) >= 1);
    CHECK(s.cursorYmd == 20260919u);
    CHECK(tickUntilIdle(s, fleet, epochOf(2026, 9, 10, 100.0), cfg, h) >= 1);
    CHECK(s.cursorYmd == 20260909u && s.nextDn == dnOf(2026, 9, 10));
    CHECK(fileKeys(monthFile(dir, 2026, 9)).size() == 19);  // nothing was lost
    clearDir(dir);
  }

  // (8) a late snapshot for D-5 re-rolls that day once, with n+1
  {
    resetHooks();
    Fleet fleet;
    fleet.addSensors(2, 90);
    const int32_t today = dnOf(2026, 9, 21);
    fleet.fill(today - 20, today - 1, 3, 40.0f);
    WarmRollupState s;
    const double now = (double)today * 86400.0 + 3600.0;
    CHECK(tickUntilIdle(s, fleet, now, cfg, h) >= 1);
    const uint32_t writesBefore = s.stats.writes;
    const double lateTs = (double)(today - 6) * 86400.0 + 50000.0;  // D = yesterday, D-5
    fleet.rings[0].add(lateTs, 41.0f, 12.0f);
    warmNoteSnapshot(s, lateTs);
    warmNoteSnapshot(s, (double)today * 86400.0 + 7000.0);  // today's data is not late
    CHECK(s.stats.lateMarks == 1 && s.dirtyDn == today - 6);
    uint32_t writes = 0;
    CHECK(tickUntilIdle(s, fleet, now + 3600.0, cfg, h, &writes) >= 1);
    CHECK(writes == 1 && s.stats.writes == writesBefore + 1);
    const auto keys = fileKeys(monthFile(dir, 2026, 9));
    auto it = keys.find(keyOf(warmDnToYmd(today - 6), fleet.rings[0].clientUid.c_str(), fleet.rings[0].sensorIndex));
    CHECK(it != keys.end() && it->second.size() == 1 && it->second[0] == 4);
    clearDir(dir);
  }

  // (9) early samples evicted from the ring (smaller n) leave the stored row alone
  {
    resetHooks();
    Fleet fleet;
    fleet.addSensors(1, 10);
    const int32_t today = dnOf(2026, 9, 21);
    fleet.fill(today - 2, today - 1, 4, 10.0f);  // 8 of 10 slots
    WarmRollupState s;
    const double now = (double)today * 86400.0 + 3600.0;
    CHECK(tickUntilIdle(s, fleet, now, cfg, h) >= 1);
    const std::string before = readFile(monthFile(dir, 2026, 9));
    fleet.fill(today, today, 4, 10.0f);  // evicts two samples of today-2
    const double lateTs = (double)(today - 2) * 86400.0 + 86000.0;
    fleet.rings[0].add(lateTs, 1.0f, 12.0f);  // late, evicts a third: n is now 2 < 4
    warmNoteSnapshot(s, lateTs);
    uint32_t writes = 0;
    CHECK(tickUntilIdle(s, fleet, now + 7200.0, cfg, h, &writes) >= 1);
    CHECK(writes == 0 && readFile(monthFile(dir, 2026, 9)) == before);
    clearDir(dir);
  }

  // (10) an I/O error keeps the position; a later tick completes
  {
    resetHooks();
    Fleet fleet;
    fleet.addSensors(2, 90);
    const int32_t today = dnOf(2026, 9, 21);
    fleet.fill(today - 5, today - 1, 2, 10.0f);
    WarmRollupState s;
    const double now = (double)today * 86400.0 + 3600.0;
    long renameIdx = -1;
    for (long probe = 0; probe < 200 && renameIdx < 0; ++probe) {
      clearDir(dir);
      WarmRollupState p;
      resetHooks();
      fi::state().failAt = probe;
      tick(p, fleet, now, cfg, h);
      if (fi::state().failedOp == fi::OP_RENAME) renameIdx = probe;
    }
    CHECK(renameIdx >= 0);
    clearDir(dir);
    resetHooks();
    s.cursorYmd = 20260901u;
    fi::state().failAt = renameIdx;
    const WarmTickSummary sum = tick(s, fleet, now, cfg, h);
    CHECK(sum.batches == 0 && s.stats.ioErrors == 1 && s.stats.lastError == WARM_IO_ERROR);
    CHECK(s.nextDn == today - 5 && s.cursorYmd == 20260901u && gLogLines.size() == 1);
    CHECK(countLogs("will retry next hour") == 1 && !anyTmp(dir) && !fileExists(monthFile(dir, 2026, 9)));
    resetHooks();
    CHECK(tickUntilIdle(s, fleet, now + 3600.0, cfg, h) >= 1);
    CHECK(s.nextDn == today && s.cursorYmd == warmDnToYmd(today - 1) &&
          fileKeys(monthFile(dir, 2026, 9)).size() == 10);
    clearDir(dir);
  }

  // (10b) a month file that can never be read is retried WARM_MAX_FAILED_TICKS
  // times, then skipped until a restart so the months after it are rolled
  {
    resetHooks();
    Fleet fleet;
    fleet.addSensors(2, 90);
    const int32_t today = dnOf(2026, 9, 5);
    fleet.fill(dnOf(2026, 8, 25), today - 1, 2, 10.0f);
    const std::string aug = monthFile(dir, 2026, 8);
    writeFile(aug, "[]");
    fi::state().failReadsOf = aug;
    WarmRollupState s;
    const double now = (double)today * 86400.0 + 3600.0;
    for (int i = 1; i < WARM_MAX_FAILED_TICKS; ++i) {
      CHECK(tick(s, fleet, now, cfg, h).batches == 0 && s.nextDn == dnOf(2026, 8, 25) && s.failedTicks == i);
    }
    CHECK(s.stats.ioErrors == (uint32_t)(WARM_MAX_FAILED_TICKS - 1) && !fileExists(monthFile(dir, 2026, 9)));
    const WarmTickSummary sum = tick(s, fleet, now, cfg, h);
    CHECK(sum.firstYmd == 20260825u && sum.lastYmd == 20260904u && sum.batches == 2 && sum.writes == 1);
    CHECK(s.stats.ioErrors == (uint32_t)WARM_MAX_FAILED_TICKS && s.stats.skippedDays == 7 && s.failedTicks == 0);
    CHECK(countLogs("skipped 20260825-20260831 until restart") == 1);
    CHECK(s.nextDn == today && s.cursorYmd == 20260904u);
    CHECK(readFile(aug) == "[]" && fileKeys(monthFile(dir, 2026, 9)).size() == 4 * 2);
    // a restart tries the month again
    s.nextDn = -1;
    CHECK(tick(s, fleet, now, cfg, h).batches == 0 && s.failedTicks == 1);
    // a tick that gets through clears the count
    fi::state().failReadsOf.clear();
    CHECK(tick(s, fleet, now, cfg, h).batches > 0 && s.failedTicks == 0);
    clearDir(dir);
  }

  // (10c) a batch that gets through ends the streak: a month that starts failing after
  // another month's failures still gets its own WARM_MAX_FAILED_TICKS tries
  {
    resetHooks();
    Fleet fleet;
    fleet.addSensors(2, 90);
    const int32_t today = dnOf(2026, 9, 5);
    fleet.fill(dnOf(2026, 8, 25), today - 1, 2, 10.0f);
    const std::string aug = monthFile(dir, 2026, 8);
    const std::string sep = monthFile(dir, 2026, 9);
    writeFile(aug, "[]");
    writeFile(sep, "[]");
    fi::state().failReadsOf = aug;
    WarmRollupState s;
    const double now = (double)today * 86400.0 + 3600.0;
    for (int i = 1; i < WARM_MAX_FAILED_TICKS; ++i) {
      CHECK(tick(s, fleet, now, cfg, h).batches == 0 && s.nextDn == dnOf(2026, 8, 25) && s.failedTicks == i);
    }
    // August gets through, then September fails for the first time in the same tick
    fi::state().failReadsOf = sep;
    gLogLines.clear();
    const WarmTickSummary sum = tick(s, fleet, now, cfg, h);
    CHECK(sum.firstYmd == 20260825u && sum.lastYmd == 20260831u && sum.batches == 1 && sum.writes == 1);
    CHECK(s.failedTicks == 1 && s.nextDn == dnOf(2026, 9, 1) && s.stats.skippedDays == 0);
    CHECK(countLogs("will retry next hour") == 1 && countLogs("until restart") == 0);
    CHECK(fileKeys(aug).size() == 7 * 2 && readFile(sep) == "[]");
    for (int i = 2; i < WARM_MAX_FAILED_TICKS; ++i) {
      CHECK(tick(s, fleet, now, cfg, h).batches == 0 && s.nextDn == dnOf(2026, 9, 1) && s.failedTicks == i);
    }
    CHECK(s.stats.skippedDays == 0 && countLogs("until restart") == 0);
    // September's own WARM_MAX_FAILED_TICKS-th failure skips it
    const WarmTickSummary skip = tick(s, fleet, now, cfg, h);
    CHECK(skip.firstYmd == 20260901u && skip.lastYmd == 20260904u && skip.batches == 1 && skip.writes == 0);
    CHECK(s.failedTicks == 0 && s.stats.skippedDays == 4 && s.nextDn == today && s.cursorYmd == 20260904u);
    CHECK(countLogs("skipped 20260901-20260904 until restart") == 1 && readFile(sep) == "[]");
    CHECK(s.stats.ioErrors == (uint32_t)(2 * WARM_MAX_FAILED_TICKS - 1));
    clearDir(dir);
  }

  // (11) a clock before 2024 does nothing
  {
    resetHooks();
    Fleet fleet;
    fleet.addSensors(1, 90);
    fleet.fill(dnOf(2023, 10, 1), dnOf(2023, 10, 5), 2, 1.0f);
    WarmRollupState s;
    const WarmTickSummary sum = tick(s, fleet, 1700000000.0, cfg, h);
    CHECK(sum.batches == 0 && s.nextDn == -1 && s.cursorYmd == 0 && fi::state().ops == 0);
    CHECK(tick(s, fleet, 0.0, cfg, h).batches == 0 && fi::state().ops == 0);
  }

  // Allocation failure of the row buffer defers the tick without moving anything
  {
    resetHooks();
    Fleet fleet;
    fleet.addSensors(1, 90);
    fleet.fill(dnOf(2026, 9, 10), dnOf(2026, 9, 12), 2, 1.0f);
    WarmRollupState s;
    fi::state().mallocFailAt = 0;
    tick(s, fleet, epochOf(2026, 9, 13, 60.0), cfg, h);
    CHECK(s.stats.noMemory == 1 && countLogs("rollup deferred") == 1 && s.cursorYmd == 0);
    CHECK(!fileExists(monthFile(dir, 2026, 9)));
    CHECK(tickUntilIdle(s, fleet, epochOf(2026, 9, 13, 60.0), cfg, h) >= 1 && s.cursorYmd == 20260912u);
    clearDir(dir);
  }
}

// ============================================================================
// Fault injection: every I/O operation and every allocation of a tick
// ============================================================================
static const char kStaleTmp[] = "[{\"d\":2026";  // a .tmp cut by a power cut mid-write

struct FaultScenario {
  std::string dir;
  WarmRollupConfig cfg;
  Fleet fleet;
  std::string p0;       // month file before the tick
  std::string p1;       // month file after a clean tick
  WarmRollupState s0;   // scheduler state before the tick
  double now;

  void prepare(bool staleTmp = false) {
    clearDir(dir);
    writeFile(monthFile(dir, 2026, 9), p0);
    if (staleTmp) writeFile(monthFile(dir, 2026, 9, ".tmp"), kStaleTmp);
  }
};

static void buildFaultScenario(FaultScenario &sc) {
  sc.dir = makeDir("faults");
  sc.cfg = defaultConfig(sc.dir);
  const WarmHooks h = makeHooks(nullptr);
  sc.fleet.addSensors(3, 90);
  sc.fleet.fill(dnOf(2026, 9, 10), dnOf(2026, 9, 14), 3, 20.0f);
  // roll Sep 10..14 into the file
  WarmRollupState s;
  tickUntilIdle(s, sc.fleet, epochOf(2026, 9, 15, 60.0), sc.cfg, h);
  // late data for Sep 12 (REPLACES) and new days Sep 15..16 (ADD); Sep 13/14 stay SUPERSEDED
  const double late = epochOf(2026, 9, 12, 70000.0);
  sc.fleet.rings[1].add(late, 25.0f, 12.0f);
  warmNoteSnapshot(s, late);
  sc.fleet.fill(dnOf(2026, 9, 15), dnOf(2026, 9, 16), 3, 20.0f);
  sc.p0 = readFile(monthFile(sc.dir, 2026, 9));
  sc.s0 = s;
  sc.now = epochOf(2026, 9, 17, 3600.0);
  sc.prepare();
  WarmRollupState clean = sc.s0;
  resetHooks();
  const WarmTickSummary sum = tick(clean, sc.fleet, sc.now, sc.cfg, h);
  CHECK(sum.batches == 1 && sum.writes == 1);
  sc.p1 = readFile(monthFile(sc.dir, 2026, 9));
  CHECK(sc.p1 != sc.p0 && clean.nextDn == dnOf(2026, 9, 17));
}

static void checkFaultOutcome(FaultScenario &sc, const WarmRollupState &s, const char *what, long index,
                              long *failures, long *successes, const char *staleTmp = nullptr) {
  const std::string p = readFile(monthFile(sc.dir, 2026, 9));
  const bool failed = s.stats.ioErrors + s.stats.noMemory > 0;
  if (failed) {
    (*failures)++;
    CHECK_MSG(p == sc.p0, "%s %ld: file changed on failure", what, index);
    CHECK_MSG(s.nextDn == sc.s0.dirtyDn && s.cursorYmd == sc.s0.cursorYmd, "%s %ld: position moved", what, index);
    CHECK_MSG(gLogLines.size() == 1, "%s %ld: %zu log lines", what, index, gLogLines.size());
  } else {
    (*successes)++;
    CHECK_MSG(p == sc.p1 && s.nextDn == dnOf(2026, 9, 17), "%s %ld: wrong result on success", what, index);
  }
  // S-D03: a tick that fails before it knows whether the month file exists
  // keeps a stale .tmp (it may be the only copy); the next tick drops it
  const bool keptStale = failed && staleTmp && readFile(monthFile(sc.dir, 2026, 9, ".tmp")) == staleTmp;
  CHECK_MSG((keptStale || !anyTmp(sc.dir)) && !fileExists(monthFile(sc.dir, 2026, 9, ".bad")),
            "%s %ld: leftovers", what, index);
}

static void testFaultInjection() {
  FaultScenario sc;
  buildFaultScenario(sc);
  const WarmHooks h = makeHooks(nullptr);

  // Count the operations of a clean tick
  sc.prepare();
  resetHooks();
  WarmRollupState probe = sc.s0;
  tick(probe, sc.fleet, sc.now, sc.cfg, h);
  const long totalOps = fi::state().ops;
  CHECK(totalOps > 20);

  long failures = 0, successes = 0;
  long failedKinds[fi::OP_COUNT] = {};
  for (long n = 0; n < totalOps; ++n) {
    sc.prepare();
    resetHooks();
    fi::state().failAt = n;
    WarmRollupState s = sc.s0;
    tick(s, sc.fleet, sc.now, sc.cfg, h);
    const bool failed = s.stats.ioErrors + s.stats.noMemory > 0;
    const int op = fi::state().failedOp;
    if (failed && op >= 0 && op < fi::OP_COUNT) failedKinds[op]++;
    checkFaultOutcome(sc, s, fi::opName(fi::state().failedOp), n, &failures, &successes);
    // the next tick without a fault completes and leaves no .tmp
    resetHooks();
    tick(s, sc.fleet, sc.now, sc.cfg, h);
    CHECK_MSG(readFile(monthFile(sc.dir, 2026, 9)) == sc.p1 && !anyTmp(sc.dir), "op %ld: no recovery", n);
  }
  CHECK(failures > 0 && successes >= 0);
  const int mustFail[] = {fi::OP_STAT, fi::OP_FOPEN, fi::OP_FREAD, fi::OP_FWRITE, fi::OP_FFLUSH, fi::OP_FCLOSE,
                          fi::OP_RENAME, fi::OP_FSEEK, fi::OP_FTELL};
  for (int op : mustFail) CHECK_MSG(failedKinds[op] > 0, "no failure injected into %s", fi::opName(op));

  // Again with a .tmp left by a power cut, so its stat() and remove() are
  // failed too (the merge then overwrites the .tmp, so those faults alone do
  // not stop it). A failed stat() of the month file stops the tick before it
  // can decide, so that tick keeps the .tmp and the next one drops it (S-D03).
  sc.prepare(true);
  resetHooks();
  WarmRollupState probeTmp = sc.s0;
  tick(probeTmp, sc.fleet, sc.now, sc.cfg, h);
  CHECK(readFile(monthFile(sc.dir, 2026, 9)) == sc.p1 && !anyTmp(sc.dir) && fi::state().perOp[fi::OP_REMOVE] == 1);
  const long totalOpsTmp = fi::state().ops;
  long removeFaults = 0;
  for (long n = 0; n < totalOpsTmp; ++n) {
    sc.prepare(true);
    resetHooks();
    fi::state().failAt = n;
    WarmRollupState s = sc.s0;
    tick(s, sc.fleet, sc.now, sc.cfg, h);
    if (fi::state().failedOp == fi::OP_REMOVE) removeFaults++;
    checkFaultOutcome(sc, s, fi::opName(fi::state().failedOp), n, &failures, &successes, kStaleTmp);
    resetHooks();
    tick(s, sc.fleet, sc.now, sc.cfg, h);
    CHECK_MSG(readFile(monthFile(sc.dir, 2026, 9)) == sc.p1 && !anyTmp(sc.dir), "stale op %ld: no recovery", n);
  }
  CHECK(removeFaults == 1);

  // Every ArduinoJson allocation (and the row buffer malloc)
  TestAllocator alloc;
  const WarmHooks ha = makeHooks(&alloc);
  sc.prepare();
  resetHooks();
  WarmRollupState counted = sc.s0;
  tick(counted, sc.fleet, sc.now, sc.cfg, ha);
  const long totalAllocs = alloc.calls;
  CHECK(totalAllocs > 10 && alloc.live() == 0 && readFile(monthFile(sc.dir, 2026, 9)) == sc.p1);
  long allocFailures = 0;
  for (long n = 0; n < totalAllocs; ++n) {
    sc.prepare();
    resetHooks();
    alloc.calls = 0;
    alloc.failAt = n;
    WarmRollupState s = sc.s0;
    tick(s, sc.fleet, sc.now, sc.cfg, ha);
    if (s.stats.noMemory > 0) allocFailures++;
    CHECK_MSG(s.stats.ioErrors == 0, "allocation %ld reported an I/O error", n);
    checkFaultOutcome(sc, s, "allocation", n, &failures, &successes);
    CHECK_MSG(alloc.live() == 0, "allocation %ld leaked %ld blocks", n, alloc.live());
  }
  alloc.failAt = -1;
  CHECK(allocFailures > 0);
  sc.prepare();
  resetHooks();
  fi::state().mallocFailAt = 0;
  WarmRollupState s = sc.s0;
  tick(s, sc.fleet, sc.now, sc.cfg, h);
  checkFaultOutcome(sc, s, "row buffer", 0, &failures, &successes);
  CHECK(s.stats.noMemory == 1);
  clearDir(sc.dir);
}

// ============================================================================
// H-23 replay: a month that grows past 8 KB and 16 KB keeps every row
// ============================================================================
static void testH23Replay() {
  const size_t fleets[] = {1, 2, 3, 4, 8, 20};
  for (size_t sensors : fleets) {
    const std::string dir = makeDir(fmtStr("h23_%zu", sensors));
    const WarmRollupConfig cfg = defaultConfig(dir);
    const WarmHooks h = makeHooks(nullptr);
    resetHooks();
    Fleet fleet;
    fleet.addSensors(sensors, 90);
    WarmRollupState s;
    const std::string path = monthFile(dir, 2026, 10);
    size_t maxSize = 0;
    bool passed8k = false, passed16k = false;
    long bad = 0;
    for (int day = 1; day <= 31; ++day) {
      fleet.fill(dnOf(2026, 10, day), dnOf(2026, 10, day), 6, 30.0f);
      tick(s, fleet, (double)(dnOf(2026, 10, day) + 1) * 86400.0 + 1800.0, cfg, h);
      bool parsed = false;
      const auto keys = fileKeys(path, &parsed);
      bool ok = parsed && keys.size() == (size_t)day * sensors;
      for (int d = 1; ok && d <= day; ++d) {
        for (const LegacyRing &r : fleet.rings) {
          auto it = keys.find(keyOf(20261000u + (uint32_t)d, r.clientUid.c_str(), r.sensorIndex));
          ok = ok && it != keys.end() && it->second.size() == 1 && it->second[0] == 6;
        }
      }
      if (!ok && ++bad <= 3) CHECK_MSG(false, "%zu sensors, day %d: month file incomplete", sensors, day);
      const size_t size = readFile(path).size();
      maxSize = std::max(maxSize, size);
      if (size > 8192) passed8k = true;
      if (size > 16384) passed16k = true;
    }
    CHECK(bad == 0);
    if (sensors >= 8) CHECK_MSG(passed8k && passed16k, "%zu sensors: max %zu bytes", sensors, maxSize);
    CHECK(s.stats.ioErrors == 0 && s.stats.tooBig == 0 && s.stats.quarantined == 0);

    // The reader visitors agree with whole-document oracles over the same file
    JsonDocument doc;
    const std::string text = readFile(path);
    CHECK(!deserializeJson(doc, text));
    uint32_t rows = 0;
    CHECK(warmScanFile(path.c_str(), false, cfg.maxFileBytes, 0, nullptr, nullptr, h, &rows, nullptr) == WARM_OK &&
          rows == 31 * sensors);  // compare: "previous month exists"
    for (const LegacyRing &r : fleet.rings) {
      WarmSensorStats got;
      warmSensorStatsInit(got, r.clientUid.c_str(), r.sensorIndex);
      CHECK(warmScanFile(path.c_str(), false, cfg.maxFileBytes, 0, warmSensorStatsVisitor, &got, h, nullptr,
                         nullptr) == WARM_OK);
      const WarmSensorStats want = legacySensorStats(doc, r.clientUid.c_str(), r.sensorIndex);
      CHECK(got.minLevel == want.minLevel && got.maxLevel == want.maxLevel && got.sumAvg == want.sumAvg &&
            got.days == want.days);
      // yoy: one month folded into the year totals the way the sketch does it
      float yMin = 9999.0f, yMax = -9999.0f, ySum = 0.0f;
      int yDays = 0;
      bool found = false;
      if (got.days > 0) {
        if (got.minLevel < yMin) yMin = got.minLevel;
        if (got.maxLevel > yMax) yMax = got.maxLevel;
        ySum += got.sumAvg;
        yDays += got.days;
        found = true;
      }
      const LegacyYoy yoy = legacyYoyMonth(doc, r.clientUid.c_str(), r.sensorIndex);
      CHECK(yMin == yoy.yMin && yMax == yoy.yMax && ySum == yoy.ySum && yDays == yoy.yDays &&
            found == yoy.foundAnyMonth);
    }
    WarmMonthSensor sensorsBuf[20] = {};
    WarmMonthSummary summary = {sensorsBuf, 20, 0};
    CHECK(warmScanFile(path.c_str(), false, cfg.maxFileBytes, 0, warmMonthSummaryVisitor, &summary, h, nullptr,
                       nullptr) == WARM_OK);
    const std::vector<WarmMonthSensor> want = legacyMonthSummary(doc, 20);
    CHECK((size_t)summary.count == want.size());
    for (size_t i = 0; i < want.size() && i < (size_t)summary.count; ++i) {
      const WarmMonthSensor &a = summary.sensors[i];
      const WarmMonthSensor &b = want[i];
      CHECK(strcmp(a.clientUid, b.clientUid) == 0 && a.sensorIndex == b.sensorIndex && a.minL == b.minL &&
            a.maxL == b.maxL && a.sumL == b.sumL && a.sumV == b.sumV && a.count == b.count &&
            a.voltCount == b.voltCount);
    }
    clearDir(dir);
    rmdir(dir.c_str());
  }
}

// ============================================================================
// Archived-client manifest (M-54, M-55)
// ============================================================================
static WarmManifestEntry manifestEntry(int i, const std::string &store, size_t pad = 0) {
  static std::deque<std::string> keep;  // keeps the strings alive (and in place) for the entry's pointers
  keep.push_back(fmtStr("dev:86447306%07d", i));
  const char *uid = keep.back().c_str();
  keep.push_back(fmtStr("Site %d%s", i, std::string(pad, 'x').c_str()));
  const char *site = keep.back().c_str();
  keep.push_back(fmtStr("Site %d (Jan 2025 - Sep 2026)", i));
  const char *label = keep.back().c_str();
  keep.push_back(fmtStr("%s/dev:864473060000001/archived_clients/Site_%d_202501-202609_%07d.json", store.c_str(), i, i));
  const char *file = keep.back().c_str();
  WarmManifestEntry e = {uid, site, label, file, 1735700000.0 + i, 1790000000.0 + i, 1790100000.0 + i, (uint8_t)(1 + i % 8)};
  return e;
}

// v2.2.15 wrote the manifest as {"archives":[ ...entries in this key order... ]}
static std::string manifestText(const std::vector<WarmManifestEntry> &entries) {
  JsonDocument doc;
  JsonArray arr = doc["archives"].to<JsonArray>();
  for (const WarmManifestEntry &e : entries) {
    JsonObject o = arr.add<JsonObject>();
    o["clientUid"] = e.clientUid;
    o["site"] = e.site;
    o["displayLabel"] = e.displayLabel;
    o["firstSeenEpoch"] = e.firstSeenEpoch;
    o["lastUpdateEpoch"] = e.lastUpdateEpoch;
    o["archiveEpoch"] = e.archiveEpoch;
    o["ftpFile"] = e.ftpFile;
    o["sensorCount"] = e.sensorCount;
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

static std::vector<std::string> loadedFiles(JsonDocument &doc) {
  std::vector<std::string> files;
  for (JsonObject o : doc["archives"].as<JsonArray>()) files.push_back(o["ftpFile"] | "");
  return files;
}

static void testManifest() {
  const std::string dir = makeDir("manifest");
  const std::string path = joinPath(dir, "archived_clients.json");
  const std::string bad = path + ".bad";
  const std::string store = "/tankalarm";
  const uint32_t maxBytes = 32768;
  const WarmHooks h = makeHooks(nullptr);
  uint32_t salvaged = 0;

  // Absent: an empty list
  {
    JsonDocument doc;
    CHECK(warmLoadManifest(path.c_str(), doc, maxBytes, true, &salvaged, h) == WARM_MAN_ABSENT);
    CHECK(doc["archives"].is<JsonArray>() && doc["archives"].size() == 0);
  }

  // v2.2.15 manifests of 2047, 2048 and 2049 bytes load strictly (the old reader stopped at 2047)
  for (size_t target = 2047; target <= 2049; ++target) {
    // whole entries (~290 bytes each) up to 100-400 bytes short of the target, then pad the last site name
    std::vector<WarmManifestEntry> entries;
    for (int i = 0; manifestText(entries).size() + 400 < target; ++i) entries.push_back(manifestEntry(i, store));
    const size_t base = manifestText(entries).size();
    CHECK(base < target);
    entries.back() = manifestEntry((int)entries.size() - 1, store, target - base);
    const std::string text = manifestText(entries);
    CHECK_MSG(text.size() == target, "fixture is %zu bytes", text.size());
    writeFile(path, text);
    JsonDocument doc;
    CHECK(warmLoadManifest(path.c_str(), doc, maxBytes, true, &salvaged, h) == WARM_MAN_OK);
    CHECK(doc["archives"].size() == entries.size());
  }

  // Truncated mid-entry: every complete entry is salvaged (read-only)
  std::vector<WarmManifestEntry> five;
  for (int i = 0; i < 5; ++i) five.push_back(manifestEntry(100 + i, store));
  const std::string fiveText = manifestText(five);
  const size_t cut = fiveText.find(five[3].ftpFile);
  writeFile(path, fiveText.substr(0, cut));
  {
    JsonDocument doc;
    CHECK(warmLoadManifest(path.c_str(), doc, maxBytes, true, &salvaged, h) == WARM_MAN_SALVAGED && salvaged == 3);
    const std::vector<std::string> files = loadedFiles(doc);
    CHECK(files.size() == 3 && files[0] == five[0].ftpFile && files[2] == five[2].ftpFile);
    CHECK(warmLoadManifest(path.c_str(), doc, maxBytes, false, &salvaged, h) == WARM_MAN_UNREADABLE);
    CHECK(readFile(path) == fiveText.substr(0, cut) && !fileExists(bad));
  }
  // Wrong prefix: nothing to salvage
  writeFile(path, "{\"ftpEnabled\":true,\"archives\":[" + fiveText.substr(13, 200));
  {
    JsonDocument doc;
    CHECK(warmLoadManifest(path.c_str(), doc, maxBytes, true, &salvaged, h) == WARM_MAN_SALVAGED && salvaged == 0);
    CHECK(doc["archives"].is<JsonArray>() && doc["archives"].size() == 0);
  }
  // Over the size cap: the entries in the first maxBytes are salvaged
  writeFile(path, fiveText);
  {
    JsonDocument doc;
    CHECK(warmLoadManifest(path.c_str(), doc, (uint32_t)cut, true, &salvaged, h) == WARM_MAN_SALVAGED &&
          salvaged == 3);
  }
  // I/O errors are not "unreadable"
  {
    JsonDocument doc;
    resetHooks();
    fi::state().failAt = 0;  // stat
    CHECK(warmLoadManifest(path.c_str(), doc, maxBytes, true, &salvaged, h) == WARM_MAN_IO_ERROR);
    resetHooks();
    fi::state().readBudget = 100;
    CHECK(warmLoadManifest(path.c_str(), doc, maxBytes, true, &salvaged, h) == WARM_MAN_IO_ERROR);
    resetHooks();
  }

  // 60 appends keep the newest 48, one log line per dropped entry, file under 32 KB
  clearDir(dir);
  resetHooks();
  WarmManifestAppendResult res;
  for (int i = 0; i < 60; ++i) {
    CHECK(warmManifestAppend(path.c_str(), bad.c_str(), manifestEntry(i, store), 48, maxBytes, h, &res) == WARM_OK);
  }
  CHECK(countLogs("dropped oldest entry") == 12 && readFile(path).size() <= maxBytes);
  {
    JsonDocument doc;
    CHECK(warmLoadManifest(path.c_str(), doc, maxBytes, false, &salvaged, h) == WARM_MAN_OK);
    const std::vector<std::string> files = loadedFiles(doc);
    CHECK(files.size() == 48 && files.front() == manifestEntry(12, store).ftpFile &&
          files.back() == manifestEntry(59, store).ftpFile);
  }

  // The byte cap binds before the entry cap for large entries
  clearDir(dir);
  resetHooks();
  for (int i = 0; i < 48; ++i) {
    CHECK(warmManifestAppend(path.c_str(), bad.c_str(), manifestEntry(i, store, 700), 48, maxBytes, h, &res) ==
          WARM_OK);
  }
  CHECK(readFile(path).size() < maxBytes && countLogs("dropped oldest entry") > 0);

  // A re-archive of the same ftpFile replaces its entry
  clearDir(dir);
  resetHooks();
  CHECK(warmManifestAppend(path.c_str(), bad.c_str(), manifestEntry(1, store), 48, maxBytes, h, &res) == WARM_OK);
  CHECK(warmManifestAppend(path.c_str(), bad.c_str(), manifestEntry(2, store), 48, maxBytes, h, &res) == WARM_OK);
  WarmManifestEntry again = manifestEntry(1, store, 5);
  CHECK(warmManifestAppend(path.c_str(), bad.c_str(), again, 48, maxBytes, h, &res) == WARM_OK);
  {
    JsonDocument doc;
    CHECK(warmLoadManifest(path.c_str(), doc, maxBytes, false, &salvaged, h) == WARM_MAN_OK);
    JsonArray arr = doc["archives"].as<JsonArray>();
    CHECK(arr.size() == 2 && strcmp(arr[1]["ftpFile"] | "", again.ftpFile) == 0 &&
          strcmp(arr[1]["site"] | "", again.site) == 0);
  }

  // Injected failures at every operation leave the old manifest byte-identical
  clearDir(dir);
  writeFile(path, fiveText);
  resetHooks();
  const WarmManifestEntry added = manifestEntry(200, store);
  CHECK(warmManifestAppend(path.c_str(), bad.c_str(), added, 48, maxBytes, h, &res) == WARM_OK);
  const std::string expected = readFile(path);
  CHECK(expected == manifestText({five[0], five[1], five[2], five[3], five[4], added}));
  const long totalOps = fi::state().ops;
  long failures = 0;
  for (long n = 0; n < totalOps; ++n) {
    clearDir(dir);
    writeFile(path, fiveText);
    resetHooks();
    fi::state().failAt = n;
    const WarmStatus st = warmManifestAppend(path.c_str(), bad.c_str(), added, 48, maxBytes, h, &res);
    const std::string now = readFile(path);
    if (st != WARM_OK) {
      failures++;
      CHECK_MSG(now == fiveText, "op %ld (%s): manifest changed on failure", n, fi::opName(fi::state().failedOp));
      CHECK(countLogs("archive is on FTP at") == 1);
    } else {
      CHECK_MSG(now == expected, "op %ld: wrong manifest", n);
    }
    CHECK_MSG(!anyTmp(dir) && !fileExists(bad), "op %ld: leftovers", n);
  }
  CHECK(failures > 0);
  // ... and at every allocation
  TestAllocator alloc;
  const WarmHooks ha = makeHooks(&alloc);
  clearDir(dir);
  writeFile(path, fiveText);
  resetHooks();
  CHECK(warmManifestAppend(path.c_str(), bad.c_str(), added, 48, maxBytes, ha, &res) == WARM_OK);
  const long totalAllocs = alloc.calls;
  for (long n = 0; n < totalAllocs; ++n) {
    clearDir(dir);
    writeFile(path, fiveText);
    resetHooks();
    alloc.calls = 0;
    alloc.failAt = n;
    const WarmStatus st = warmManifestAppend(path.c_str(), bad.c_str(), added, 48, maxBytes, ha, &res);
    const std::string now = readFile(path);
    CHECK_MSG(st == WARM_OK ? now == expected : (st == WARM_NO_MEMORY && now == fiveText), "allocation %ld", n);
    CHECK_MSG(alloc.live() == 0, "allocation %ld leaked", n);
  }

  // Unreadable manifest: salvaged, original kept as .bad, new entry appended
  clearDir(dir);
  resetHooks();
  const std::string truncated = fiveText.substr(0, cut);
  writeFile(path, truncated);
  CHECK(warmManifestAppend(path.c_str(), bad.c_str(), added, 48, maxBytes, h, &res) == WARM_OK);
  CHECK(res.loaded == WARM_MAN_SALVAGED && res.quarantined && res.salvaged == 3);
  CHECK(readFile(bad) == truncated && readFile(path) == manifestText({five[0], five[1], five[2], added}));
  CHECK(countLogs("salvaged 3 entries, original saved as archived_clients.json.bad") == 1);

  // A failed move to .bad refuses to write: the unreadable file stays, no .bad, no .tmp.
  // The move is the first rename of a successful salvage (the manifest is written one
  // fwrite per byte first, so it comes hundreds of operations in; take it from the trace).
  clearDir(dir);
  writeFile(path, truncated);
  resetHooks();
  CHECK(warmManifestAppend(path.c_str(), bad.c_str(), added, 48, maxBytes, h, &res) == WARM_OK);
  const std::vector<long> salvageRenames = opIndices(fi::OP_RENAME);
  CHECK(!salvageRenames.empty());
  const long moveIdx = salvageRenames.empty() ? -1 : salvageRenames[0];
  clearDir(dir);
  writeFile(path, truncated);
  resetHooks();
  fi::state().failAt = moveIdx;
  CHECK(warmManifestAppend(path.c_str(), bad.c_str(), added, 48, maxBytes, h, &res) == WARM_IO_ERROR);
  CHECK(readFile(path) == truncated && !fileExists(bad) && !anyTmp(dir) && countLogs("write failed") == 1);

  // A failed rename(.tmp, manifest) puts the unreadable manifest back unchanged
  clearDir(dir);
  writeFile(path, truncated);
  resetHooks();
  CHECK(warmManifestAppend(path.c_str(), bad.c_str(), added, 48, maxBytes, h, &res) == WARM_OK && res.quarantined);
  const std::string salvagedText = readFile(path);
  const std::vector<long> renames = opIndices(fi::OP_RENAME);
  CHECK(renames.size() == 2);  // manifest -> .bad, .tmp -> manifest
  if (renames.size() == 2) {
    clearDir(dir);
    writeFile(path, truncated);
    resetHooks();
    fi::state().failAt = renames[1];
    CHECK(warmManifestAppend(path.c_str(), bad.c_str(), added, 48, maxBytes, h, &res) == WARM_IO_ERROR);
    CHECK(readFile(path) == truncated && !fileExists(bad) && !anyTmp(dir));

    // ... and when putting it back fails too, the complete new manifest stays
    // in .tmp (as after a power cut between the renames), and the next append
    // finishes that rename before it adds its entry
    clearDir(dir);
    writeFile(path, truncated);
    resetHooks();
    fi::state().failAt = renames[1];
    fi::state().failAt2 = renames[1] + 1;
    CHECK(warmManifestAppend(path.c_str(), bad.c_str(), added, 48, maxBytes, h, &res) == WARM_IO_ERROR);
    const std::vector<long> tried = opIndices(fi::OP_RENAME);
    CHECK(tried.size() == 3 && tried[2] == renames[1] + 1);
    CHECK(!fileExists(path) && readFile(bad) == truncated && readFile(path + ".tmp") == salvagedText);
    resetHooks();
    const WarmManifestEntry later = manifestEntry(201, store);
    CHECK(warmManifestAppend(path.c_str(), bad.c_str(), later, 48, maxBytes, h, &res) == WARM_OK);
    CHECK(res.loaded == WARM_MAN_OK && readFile(path) == manifestText({five[0], five[1], five[2], added, later}));
    CHECK(readFile(bad) == truncated && !anyTmp(dir));
  }

  // ?file= accepts only exact ftpFile values from the manifest, including 'dev:' paths
  clearDir(dir);
  resetHooks();
  writeFile(path, manifestText({five[0], five[1]}));
  {
    JsonDocument doc;
    CHECK(warmLoadManifest(path.c_str(), doc, maxBytes, true, &salvaged, h) == WARM_MAN_OK);
    const std::string listed = five[1].ftpFile;
    CHECK(listed.find("dev:") != std::string::npos);
    CHECK(warmManifestHasFile(doc, listed.c_str()) && warmManifestHasFile(doc, five[0].ftpFile));
    CHECK(!warmManifestHasFile(doc, (listed + "x").c_str()));
    CHECK(!warmManifestHasFile(doc, listed.substr(1).c_str()));
    CHECK(!warmManifestHasFile(doc, listed.substr(0, listed.size() - 1).c_str()));
    CHECK(!warmManifestHasFile(doc, manifestEntry(300, store).ftpFile));
    CHECK(!warmManifestHasFile(doc, "") && !warmManifestHasFile(doc, nullptr));
    JsonDocument empty;
    CHECK(warmLoadManifest(joinPath(dir, "missing.json").c_str(), empty, maxBytes, true, &salvaged, h) ==
          WARM_MAN_ABSENT);
    CHECK(!warmManifestHasFile(empty, listed.c_str()));
  }
  clearDir(dir);
}

// ============================================================================
// Provenance: only fresh readings, on the day they were taken
// ============================================================================
static bool closeTo(float a, float b) { return fabsf(a - b) < 0.001f; }

// Row (d, c, k) of a month file, parsed with a whole-document ArduinoJson oracle
static bool findRow(const std::string &path, uint32_t d, const char *c, unsigned k, JsonDocument &out) {
  JsonDocument doc;
  if (deserializeJson(doc, readFile(path)) != DeserializationError::Ok) return false;
  for (JsonObject row : doc.as<JsonArray>()) {
    if ((row["d"] | 0UL) == d && strcmp(row["c"] | "", c) == 0 && (row["k"] | 0u) == k) {
      out.set(row);
      return true;
    }
  }
  return false;
}

static void testProvenance() {
  const std::string dir = makeDir("provenance");
  const WarmRollupConfig cfg = defaultConfig(dir);
  const WarmHooks h = makeHooks(nullptr);

  // (1) A day without readings stays blank, and a row uses only its own day's readings.
  // Sensor A reads on Sep 1-3 and 7-9 (on Sep 9 only in the last minute), sensor B on
  // Sep 1-9 except Sep 5. The rollup runs each night, as on a server that stays up.
  {
    resetHooks();
    Fleet fleet;
    fleet.rings.push_back(LegacyRing("dev:864450000000001", 1, 90));
    fleet.rings.push_back(LegacyRing("dev:864450000000002", 2, 90));
    WarmRollupState s;
    for (int day = 1; day <= 10; ++day) {
      const double d0 = epochOf(2026, 9, day);
      LegacyRing &a = fleet.rings[0];
      LegacyRing &b = fleet.rings[1];
      if (day <= 3 || day == 7 || day == 8) {
        a.add(d0 + 6 * 3600.0, 30.0f + (float)day, 12.5f);
        a.add(d0 + 12 * 3600.0, 29.0f + (float)day, 12.6f);
        a.add(d0 + 18 * 3600.0, 31.0f + (float)day, 12.7f);
      } else if (day == 9) {
        a.add(d0 + 86340.0, 20.0f, 0.0f);
        a.add(d0 + 86399.0, 21.0f, 0.0f);
      }
      if (day <= 9 && day != 5) b.add(d0 + 3600.0 * day, 50.0f + (float)day, 0.0f);
      CHECK(tickUntilIdle(s, fleet, epochOf(2026, 9, day + 1, 1800.0), cfg, h) >= 1);
    }

    const LegacyRing &a = fleet.rings[0];
    const LegacyRing &b = fleet.rings[1];
    const std::string path = monthFile(dir, 2026, 9);
    const auto keys = fileKeys(path);
    CHECK(keys.size() == 6 + 8);
    for (int day = 1; day <= 10; ++day) {
      const uint32_t ymd = 20260900u + (uint32_t)day;
      const bool wantA = day <= 3 || (day >= 7 && day <= 9);
      const bool wantB = day <= 9 && day != 5;
      CHECK_MSG((keys.count(keyOf(ymd, a.clientUid.c_str(), a.sensorIndex)) == 1) == wantA, "A on %lu",
                (unsigned long)ymd);
      CHECK_MSG((keys.count(keyOf(ymd, b.clientUid.c_str(), b.sensorIndex)) == 1) == wantB, "B on %lu",
                (unsigned long)ymd);
    }

    // Sep 7, after A's gap: opens at its own first reading; nothing from Sep 3
    JsonDocument row;
    CHECK(findRow(path, 20260907u, a.clientUid.c_str(), a.sensorIndex, row));
    CHECK(row["n"].as<int>() == 3 && closeTo(row["op"].as<float>(), 37.0f) && closeTo(row["mn"].as<float>(), 36.0f) &&
          closeTo(row["mx"].as<float>(), 38.0f) && closeTo(row["av"].as<float>(), 37.0f) &&
          closeTo(row["cl"].as<float>(), 38.0f) && closeTo(row["vt"].as<float>(), 12.6f));
    // Sep 9: readings only in its last minute, and the row is built from those alone
    CHECK(findRow(path, 20260909u, a.clientUid.c_str(), a.sensorIndex, row));
    CHECK(row["n"].as<int>() == 2 && closeTo(row["op"].as<float>(), 20.0f) && closeTo(row["cl"].as<float>(), 21.0f) &&
          closeTo(row["mn"].as<float>(), 20.0f) && closeTo(row["mx"].as<float>(), 21.0f) &&
          closeTo(row["av"].as<float>(), 20.5f) && row["vt"].as<float>() == 0.0f);
    // Sep 6, after B's gap: its single reading, not Sep 4's
    CHECK(findRow(path, 20260906u, b.clientUid.c_str(), b.sensorIndex, row));
    CHECK(row["n"].as<int>() == 1 && closeTo(row["op"].as<float>(), 56.0f) && closeTo(row["av"].as<float>(), 56.0f));

    // A reboot's re-check of the whole window fills nothing in either
    const std::string before = readFile(path);
    s.nextDn = -1;
    CHECK(tickUntilIdle(s, fleet, epochOf(2026, 9, 11, 1800.0), cfg, h) >= 1);
    CHECK(readFile(path) == before);
    clearDir(dir);
  }

  // (2) Acquisition time: whole seconds, or 0 (left out) when it is not known
  {
    const double now = 1790294400.0;  // 2026-09-25 00:00:00Z
    CHECK(warmAcquisitionEpoch(0.0, now) == 0.0);
    CHECK(warmAcquisitionEpoch(-5.0, now) == 0.0);
    CHECK(warmAcquisitionEpoch(NAN, now) == 0.0);
    CHECK(warmAcquisitionEpoch(INFINITY, 0.0) == 0.0);
    CHECK(warmAcquisitionEpoch(1577836799.0, now) == 0.0);
    CHECK(warmAcquisitionEpoch(1577836800.0, now) == 1577836800.0);
    // server clock not set: any time from 2020 on (up to the 2099 limit of the date helpers)
    CHECK(warmAcquisitionEpoch(1577836800.0, 0.0) == 1577836800.0);
    CHECK(warmAcquisitionEpoch(now + 400.0 * 86400.0, 0.0) == now + 400.0 * 86400.0);
    CHECK(warmAcquisitionEpoch(4102444799.0, 0.0) == 4102444799.0);
    CHECK(warmAcquisitionEpoch(4102444800.0, 0.0) == 0.0);
    CHECK(warmAcquisitionEpoch(now + 3600.0, now) == now + 3600.0);
    CHECK(warmAcquisitionEpoch(now + 3601.0, now) == 0.0);
    CHECK(warmAcquisitionEpoch(now - 0.5, now) == now - 1.0);  // floored: stays on its own day
    CHECK(warmEpochToDn(warmAcquisitionEpoch(now - 0.001, now)) == warmEpochToDn(now) - 1);
    CHECK(warmAcquisitionEpoch(1790207744.75, now) == 1790207744.0);
  }

  // (3) Older hot-tier files: which reloaded timestamps may be on the wrong day
  {
    const double mid = 1790294400.0;  // a UTC midnight, and a multiple of 128
    CHECK(warmLegacyEpochAmbiguous(mid));
    CHECK(warmLegacyEpochAmbiguous(mid - 512.0) && warmLegacyEpochAmbiguous(mid + 512.0));
    CHECK(warmLegacyEpochAmbiguous(mid - 128.0) && warmLegacyEpochAmbiguous(mid + 384.0));
    CHECK(!warmLegacyEpochAmbiguous(mid - 640.0) && !warmLegacyEpochAmbiguous(mid + 640.0));
    CHECK(!warmLegacyEpochAmbiguous(mid + 128.0 * 300.0));
    CHECK(!warmLegacyEpochAmbiguous(mid + 1.0) && !warmLegacyEpochAmbiguous(mid - 1.0) &&
          !warmLegacyEpochAmbiguous(mid + 100.0) && !warmLegacyEpochAmbiguous(mid + 0.5));
    CHECK(!warmLegacyEpochAmbiguous(0.0) && !warmLegacyEpochAmbiguous(NAN) && !warmLegacyEpochAmbiguous(INFINITY));
  }

  // (4) hot_tier.json and client notes: an epoch written as a uint32_t round-trips exactly.
  // saveHotTierSnapshot adds (uint32_t)ts. Older firmware added the double: ArduinoJson 7.4.3
  // keeps one that fits a float as a float and writes 7 digits, which moves both of these onto
  // another day.
  {
    const double stamps[] = {1790294400.0, 1790207744.0};  // 2026-09-25 00:00:00Z, 2026-09-23 23:55:44Z
    JsonDocument saved;
    JsonArray arr = saved.to<JsonArray>();
    for (double ts : stamps) {
      JsonArray entry = arr.add<JsonArray>();
      entry.add((uint32_t)ts);
      entry.add(40.25f);
      entry.add(12.5f);
    }
    std::string text;
    serializeJson(saved, text);
    JsonDocument loaded;
    CHECK(deserializeJson(loaded, text) == DeserializationError::Ok);
    for (size_t i = 0; i < 2; ++i) {
      CHECK_MSG(loaded[i][0].as<double>() == stamps[i] && loaded[i][0].is<uint32_t>(), "%s", text.c_str());
    }

    JsonDocument legacySaved;
    JsonArray legacyArr = legacySaved.to<JsonArray>();
    for (double ts : stamps) legacyArr.add<JsonArray>().add(ts);
    std::string legacyText;
    serializeJson(legacySaved, legacyText);
    JsonDocument legacyLoaded;
    CHECK(deserializeJson(legacyLoaded, legacyText) == DeserializationError::Ok);
    for (size_t i = 0; i < 2; ++i) {
      const double back = legacyLoaded[i][0].as<double>();
      CHECK_MSG(back != stamps[i] && warmEpochToDn(back) != warmEpochToDn(stamps[i]), "%s", legacyText.c_str());
      // what loadHotTierSnapshot drops on the first boot after the update
      CHECK(!legacyLoaded[i][0].is<uint32_t>() && warmLegacyEpochAmbiguous(floor(back)));
    }

    // From v2.2.16 (#318) a client sends every note `t` as a whole minute, truncated, and as a
    // uint32_t: it comes back exactly, on the day the reading was taken. As a double, a whole
    // minute that is a multiple of 1920 s (one in 32) would be kept as a float and come back
    // minutes off.
    const double taken[] = {1790294399.6, 1790294400.4, 1790332859.9};  // Sep 24 23:59:59.6, Sep 25 00:00:00.4, 10:40:59.9
    const double minute[] = {1790294340.0, 1790294400.0, 1790332800.0};
    for (size_t i = 0; i < 3; ++i) {
      const uint32_t whole = (uint32_t)taken[i];
      JsonDocument note;
      note["t"] = whole - whole % 60U;
      std::string noteText;
      serializeJson(note, noteText);
      JsonDocument got;
      CHECK(deserializeJson(got, noteText) == DeserializationError::Ok);
      CHECK_MSG(got["t"].as<double>() == minute[i] && got["t"].is<uint32_t>(), "%s", noteText.c_str());
      CHECK(warmEpochToDn(got["t"].as<double>()) == warmEpochToDn(taken[i]));
    }
    for (size_t i = 1; i < 3; ++i) {  // the multiples of 1920 s
      JsonDocument note;
      note["t"] = minute[i];
      std::string noteText;
      serializeJson(note, noteText);
      JsonDocument got;
      CHECK(deserializeJson(got, noteText) == DeserializationError::Ok);
      CHECK_MSG(got["t"].as<double>() != minute[i], "%s", noteText.c_str());
    }
  }

  // (5) The same acquisition is found anywhere in the ring, up to 1 s either way, by time alone
  {
    const double t0 = 1790294400.0;
    LegacyRing r("dev:864450000000003", 1, 4);
    for (int i = 0; i < 6; ++i) r.add(t0 + 60.0 * i, 10.0f + (float)i, 0.0f);  // wraps: keeps i = 2..5
    CHECK(r.writeIndex == 2 && r.snapshotCount == 4);
    auto has = [&r](uint16_t count, uint16_t writeIndex, double ts) {
      return warmRingHasAcquisition(r.snapshots.data(), r.cap(), count, writeIndex, ts);
    };
    for (int i = 2; i < 6; ++i) {
      const double ts = t0 + 60.0 * i;
      CHECK_MSG(has(4, 2, ts) && has(4, 2, ts - 1.0) && has(4, 2, ts + 1.0), "entry %d", i);
      CHECK_MSG(!has(4, 2, ts + 2.0) && !has(4, 2, ts - 2.0), "entry %d", i);
    }
    CHECK(!has(4, 2, t0) && !has(4, 2, t0 + 60.0));  // overwritten
    CHECK(has(9, 2, t0 + 300.0));                     // a count above cap is clamped
    // only the entries in use are scanned
    CHECK(has(2, 2, t0 + 240.0) && has(2, 2, t0 + 300.0) && !has(2, 2, t0 + 120.0));
    CHECK(has(4, 0, t0 + 120.0) && has(1, 0, t0 + 180.0) && !has(1, 0, t0 + 300.0));
    CHECK(!has(0, 2, t0 + 300.0) && !has(4, 2, NAN));
    CHECK(!warmRingHasAcquisition(nullptr, 4, 4, 0, t0));
    CHECK(!warmRingHasAcquisition(r.snapshots.data(), 0, 4, 0, t0 + 120.0));

    // Telemetry (t rounded up), then the daily report's copy (t truncated): one snapshot, also
    // when the server recomputed the current-loop level with a newer temperature in between
    Fleet fleet;
    fleet.rings.push_back(LegacyRing("dev:864450000000004", 1, 90));
    LegacyRing &ring = fleet.rings[0];
    auto record = [&ring](double ts, float level) {
      if (!warmRingHasAcquisition(ring.snapshots.data(), ring.cap(), ring.snapshotCount, ring.writeIndex, ts)) {
        ring.add(ts, level, 0.0f);
      }
    };
    const double t = epochOf(2026, 9, 21, 50000.0);
    record(t + 1.0, 40.0f);
    record(t + 600.0, 41.0f);
    record(t, 40.02f);
    CHECK(ring.snapshotCount == 2);
    std::vector<WarmNewRow> rows(4);
    const uint16_t n = warmComputeRows(fleet.series(), fleet.count(), dnOf(2026, 9, 21), dnOf(2026, 9, 21), nullptr,
                                       nullptr, testRoundTo, rows.data(), (uint16_t)rows.size());
    CHECK(n == 1 && rows[0].n == 2 && closeTo(rows[0].av, 40.5f));

    // From v2.2.16 (#318) every copy carries the same whole minute: readings a minute apart are
    // both kept, and a second copy of one is found
    const double m = epochOf(2026, 9, 21, 60000.0);  // 16:40:00Z
    record(m, 42.0f);
    record(m + 60.0, 43.0f);
    record(m, 42.01f);
    CHECK(ring.snapshotCount == 4 && ring.snapshots[2].timestamp == m && ring.snapshots[3].timestamp == m + 60.0);
  }

  // (6) The daily report's (and an on-demand note's) voltage goes only with a reading from the same
  // UTC day, within the hour
  {
    const double d0 = epochOf(2026, 9, 21);
    CHECK(warmSameUtcDayWithin(d0 + 3600.0, d0 + 7200.0, 3600.0));
    CHECK(warmSameUtcDayWithin(d0 + 7200.0, d0 + 3600.0, 3600.0));
    CHECK(!warmSameUtcDayWithin(d0 + 3600.0, d0 + 7201.0, 3600.0));
    CHECK(!warmSameUtcDayWithin(d0 - 5.0, d0 + 5.0, 3600.0));        // 10 s apart across midnight
    CHECK(!warmSameUtcDayWithin(d0 - 1200.0, d0 + 1800.0, 3600.0));  // 23:40 reading, 00:30 report
    CHECK(!warmSameUtcDayWithin(d0 - 1200.0, d0 + 1200.0, 3600.0));  // 23:40 reading re-sent at 00:20
    CHECK(warmSameUtcDayWithin(d0 + 5.0, d0 + 5.0, 0.0));
    CHECK(!warmSameUtcDayWithin(0.0, d0, 1e9) && !warmSameUtcDayWithin(d0, 0.0, 1e9) &&
          !warmSameUtcDayWithin(NAN, d0, 1e9) && !warmSameUtcDayWithin(d0, -1.0, 1e9));
    // A note's "v" is the client's last voltage poll, up to WARM_VIN_MAX_AGE_SEC (1 h) before
    // the note was built. A telemetry note is built within 5 minutes of its reading, so its
    // voltage is kept only for a reading from 01:00:00 to 23:54:59
    CHECK(WARM_VIN_MAX_AGE_SEC == 3600.0);
    auto telemetryVin = [](double t) { return warmVinOnReadingDay(t, t, t + 300.0); };
    CHECK(telemetryVin(d0 + 3600.0) && telemetryVin(d0 + 43200.0) && telemetryVin(d0 + 86099.0));
    CHECK(!telemetryVin(d0 + 3599.0) && !telemetryVin(d0 + 1.0) && !telemetryVin(d0 + 86100.0) &&
          !telemetryVin(d0 - 1.0) && !telemetryVin(d0 - 300.0));
    // Vin polled at 23:57:10, sampled at 00:00:40: the voltage is from the day before
    CHECK(!telemetryVin(d0 + 40.0));
    // The daily report's voltage: polled up to 1 h before the report was built
    CHECK(warmVinOnReadingDay(d0 + 18000.0, d0 + 18600.0, d0 + 18600.0));  // 05:00 reading, 05:10 report
    CHECK(warmVinOnReadingDay(d0 + 3540.0, d0 + 3600.0, d0 + 3600.0));     // 00:59 reading, 01:00 report
    CHECK(!warmVinOnReadingDay(d0 + 1200.0, d0 + 1800.0, d0 + 1800.0));    // 00:20 reading, 00:30 report
    CHECK(!warmVinOnReadingDay(d0 - 600.0, d0 + 600.0, d0 + 600.0));       // reading and report days differ
    CHECK(!warmVinOnReadingDay(d0 + 43200.0, d0 + 86400.0, d0 + 86400.0));
    // An on-demand note built at arrival, up to an hour after its reading
    CHECK(warmVinOnReadingDay(d0 + 7200.0, d0 + 7200.0, d0 + 10800.0));
    CHECK(!warmVinOnReadingDay(d0 + 82800.0, d0 + 82800.0, d0 + 86400.0));
    // Not a time, or a build span that ends before it starts
    CHECK(!warmVinOnReadingDay(0.0, d0 + 7200.0, d0 + 7200.0) && !warmVinOnReadingDay(d0 + 7200.0, 0.0, 0.0) &&
          !warmVinOnReadingDay(NAN, d0 + 7200.0, d0 + 7200.0) && !warmVinOnReadingDay(d0 + 7200.0, NAN, NAN) &&
          !warmVinOnReadingDay(d0 + 7200.0, d0 + 7200.0, NAN) &&
          !warmVinOnReadingDay(d0 + 7200.0, d0 + 7300.0, d0 + 7200.0) &&
          !warmVinOnReadingDay(d0 + 7200.0, d0 + 7200.0, INFINITY));
  }

  // (7) An alarm that arrives after its day was rolled up counts on that day. Sep 21 has one
  // reading and is rolled at 00:40 on Sep 22 with al 0; a high alarm from 23:30 on Sep 21
  // arrives at 02:00 and marks the day (logAlarmEvent). The re-roll has the same n and al 1,
  // which replaces the row. A reboot's re-check (the alarm log is RAM only) keeps al 1.
  {
    resetHooks();
    Fleet fleet;
    fleet.rings.push_back(LegacyRing("dev:864450000000005", 1, 90));
    fleet.rings[0].add(epochOf(2026, 9, 21, 5 * 3600.0), 40.0f, 12.5f);
    const std::string uid = fleet.rings[0].clientUid;
    const std::string path = monthFile(dir, 2026, 9);
    WarmRollupState s;
    CHECK(tickUntilIdle(s, fleet, epochOf(2026, 9, 22, 2400.0), cfg, h) >= 1);
    JsonDocument row;
    CHECK(findRow(path, 20260921u, uid.c_str(), 1, row) && row["n"].as<int>() == 1 && row["al"].as<int>() == 0);

    const double alarmAt = epochOf(2026, 9, 21, 84600.0);
    fleet.alarms.push_back(LegacyAlarm{uid, 1, alarmAt});
    const uint32_t marks = s.stats.lateMarks;
    warmNoteSnapshot(s, alarmAt);
    CHECK(s.stats.lateMarks == marks + 1);
    uint32_t writes = 0;
    CHECK(tickUntilIdle(s, fleet, epochOf(2026, 9, 22, 7200.0), cfg, h, &writes) >= 1 && writes == 1);
    const auto keys = fileKeys(path);
    CHECK(keys.size() == 1 && keys.begin()->second.size() == 1);
    CHECK(findRow(path, 20260921u, uid.c_str(), 1, row) && row["n"].as<int>() == 1 && row["al"].as<int>() == 1 &&
          closeTo(row["av"].as<float>(), 40.0f) && closeTo(row["vt"].as<float>(), 12.5f));

    // Marked again with nothing new: re-rolled and read, not written
    const std::string after = readFile(path);
    warmNoteSnapshot(s, alarmAt);
    writes = 0;
    CHECK(tickUntilIdle(s, fleet, epochOf(2026, 9, 22, 7300.0), cfg, h, &writes) >= 1 && writes == 0);
    CHECK(readFile(path) == after);

    // A reboot empties the alarm log; the re-check keeps al 1
    fleet.alarms.clear();
    s.nextDn = -1;
    writes = 0;
    CHECK(tickUntilIdle(s, fleet, epochOf(2026, 9, 22, 10800.0), cfg, h, &writes) >= 1 && writes == 0);
    CHECK(readFile(path) == after);
    clearDir(dir);
  }

  // (8) A ring's legacyCount oldest entries (saved by older firmware) never enter a row, also
  // after the ring wraps (the sketch lowers the count when a full ring overwrites one)
  {
    const double d = epochOf(2026, 9, 21);
    LegacyRing r("dev:864450000000006", 1, 6);
    auto append = [&r](double ts, float level, float volts) {  // recordTelemetrySnapshot's append
      if (r.snapshotCount >= r.cap() && r.legacyCount > 0) r.legacyCount--;
      r.add(ts, level, volts);
    };
    append(d + 3600.0, 0.0f, 11.0f);   // older firmware: e.g. a boot placeholder
    append(d + 7200.0, 99.0f, 13.0f);  // older firmware: e.g. an alarm value
    append(d + 10800.0, 40.0f, 12.4f);
    append(d + 14400.0, 42.0f, 12.6f);
    std::vector<WarmNewRow> rows(4);
    auto compute = [&rows](const WarmSeries &sr) {
      return warmComputeRows(&sr, 1, dnOf(2026, 9, 21), dnOf(2026, 9, 21), nullptr, nullptr, testRoundTo,
                             rows.data(), (uint16_t)rows.size());
    };
    CHECK(compute(r.series()) == 1 && rows[0].n == 4);
    r.legacyCount = 2;
    CHECK(compute(r.series()) == 1 && rows[0].n == 2 && closeTo(rows[0].mn, 40.0f) && closeTo(rows[0].mx, 42.0f) &&
          closeTo(rows[0].av, 41.0f) && closeTo(rows[0].op, 40.0f) && closeTo(rows[0].cl, 42.0f) &&
          closeTo(rows[0].vt, 12.5f));
    WarmSeries all = r.series();
    all.legacyCount = 4;
    CHECK(compute(all) == 0);
    all.legacyCount = 200;  // more than the ring holds
    CHECK(compute(all) == 0);

    append(d + 18000.0, 43.0f, 12.5f);
    append(d + 21600.0, 44.0f, 12.5f);
    CHECK(r.legacyCount == 2 && r.snapshotCount == 6);
    append(d + 25200.0, 45.0f, 12.5f);  // overwrites the placeholder
    CHECK(r.legacyCount == 1 && r.writeIndex == 1);
    CHECK(compute(r.series()) == 1 && rows[0].n == 5 && closeTo(rows[0].mn, 40.0f) && closeTo(rows[0].mx, 45.0f) &&
          closeTo(rows[0].op, 40.0f) && closeTo(rows[0].cl, 45.0f));
    append(d + 28800.0, 46.0f, 12.5f);  // overwrites the alarm value: nothing is legacy any more
    CHECK(r.legacyCount == 0 && compute(r.series()) == 1 && rows[0].n == 6 && closeTo(rows[0].mn, 40.0f) &&
          closeTo(rows[0].mx, 46.0f));
  }

  // (9) The first boot after an update from v2.2.15. That firmware rolled Sep 21 (n 2); the
  // daily report's copy of a Sep 21 reading, with the report-time voltage, reached its ring
  // afterwards, and it never rolled Sep 22. Its whole ring is loaded as legacy: the boot
  // re-check neither rewrites Sep 21 nor adds Sep 22, and the update day's row (Sep 23)
  // holds only the reading recorded after the update. Without the mark it would do both.
  {
    resetHooks();
    Fleet fleet;
    fleet.rings.push_back(LegacyRing("dev:864450000000007", 1, 90));
    LegacyRing &ring = fleet.rings[0];
    const std::string uid = ring.clientUid;
    const std::string path = monthFile(dir, 2026, 9);
    ring.add(epochOf(2026, 9, 21, 3600.0), 40.0f, 12.5f);
    ring.add(epochOf(2026, 9, 21, 7200.0), 42.0f, 12.5f);
    WarmRollupState old;
    CHECK(tickUntilIdle(old, fleet, epochOf(2026, 9, 22, 1800.0), cfg, h) >= 1);
    JsonDocument row;
    CHECK(findRow(path, 20260921u, uid.c_str(), 1, row) && row["n"].as<int>() == 2);
    ring.add(epochOf(2026, 9, 21, 79200.0), 41.0f, 12.9f);
    ring.add(epochOf(2026, 9, 22, 3600.0), 43.0f, 12.5f);
    ring.add(epochOf(2026, 9, 23, 3600.0), 44.0f, 12.5f);
    const std::string before = readFile(path);

    ring.legacyCount = ring.snapshotCount;  // loadHotTierSnapshot: an older hot_tier.json
    ring.add(epochOf(2026, 9, 23, 43200.0), 45.0f, 12.6f);
    WarmRollupState booted;
    uint32_t writes = 0;
    CHECK(tickUntilIdle(booted, fleet, epochOf(2026, 9, 23, 50000.0), cfg, h, &writes) >= 1 && writes == 0);
    CHECK(readFile(path) == before);
    CHECK(tickUntilIdle(booted, fleet, epochOf(2026, 9, 24, 1800.0), cfg, h, &writes) >= 1 && writes == 1);
    CHECK(findRow(path, 20260921u, uid.c_str(), 1, row) && row["n"].as<int>() == 2);
    CHECK(!findRow(path, 20260922u, uid.c_str(), 1, row));
    CHECK(findRow(path, 20260923u, uid.c_str(), 1, row) && row["n"].as<int>() == 1 &&
          closeTo(row["av"].as<float>(), 45.0f) && closeTo(row["vt"].as<float>(), 12.6f));

    ring.legacyCount = 0;
    WarmRollupState unmarked;
    CHECK(tickUntilIdle(unmarked, fleet, epochOf(2026, 9, 24, 3600.0), cfg, h) >= 1);
    CHECK(findRow(path, 20260921u, uid.c_str(), 1, row) && row["n"].as<int>() == 3);
    CHECK(findRow(path, 20260922u, uid.c_str(), 1, row) && row["n"].as<int>() == 1);
    clearDir(dir);
  }
}

// ============================================================================
// Leftover .tmp (S-D03): dropped beside its month file, restored without it
// ============================================================================
static void testTmpRecovery() {
  const std::string dir = makeDir("tmprecovery");
  const std::string path = monthFile(dir, 2026, 9);
  const std::string tmp = path + ".tmp";
  const std::string bad = monthFile(dir, 2026, 9, ".bad");
  const uint32_t big = 1u << 20;
  const WarmHooks h = makeHooks(nullptr);
  WarmMergeResult res = {};

  const WarmNewRow r1 = makeRow("dev:t", 20260901, 1, 5, 1.0f, 0);
  const WarmNewRow r2 = makeRow("dev:t", 20260902, 1, 5, 2.0f, 0);
  const WarmNewRow r3 = makeRow("dev:t", 20260903, 1, 5, 3.0f, 0);
  const std::string complete = joinArray({encodeRow(r1), encodeRow(r2)});

  // (1) the month's first write, cut before its rename: the complete .tmp is restored
  // and then merged into like the file it is
  resetHooks();
  writeFile(tmp, complete);
  WarmNewRow add1 = r3;
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &add1, 1, big, h, &res) == WARM_OK && res.wrote);
  CHECK(readFile(path) == joinArray({encodeRow(r1), encodeRow(r2), encodeRow(r3)}) && !anyTmp(dir));
  CHECK(countLogs("restored from its .tmp (2 rows)") == 1);
  // ... also when the merge has nothing to add
  clearDir(dir);
  resetHooks();
  writeFile(tmp, complete);
  WarmNewRow same = r2;
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &same, 1, big, h, &res) == WARM_OK && !res.wrote);
  CHECK(readFile(path) == complete && !anyTmp(dir) && fi::state().writeOpens.empty());

  // (2) a rebuild cut between its renames (file -> .bad done, .tmp -> file not): the
  // .tmp holds the rows salvaged from the .bad plus that merge's rows, some of which the
  // hot tier may no longer have, so it is restored rather than salvaged again
  clearDir(dir);
  resetHooks();
  const std::string corrupt = "[" + encodeRow(r1) + "," + encodeRow(r2) + ",{\"d\":2026";
  const WarmNewRow gone = makeRow("dev:gone", 20260904, 1, 5, 4.0f, 0);
  writeFile(bad, corrupt);
  writeFile(tmp, joinArray({encodeRow(r1), encodeRow(r2), encodeRow(gone)}));
  WarmNewRow add2 = r3;
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &add2, 1, big, h, &res) == WARM_OK && res.wrote);
  CHECK(!res.quarantined && res.salvagedRows == 0);
  CHECK(readFile(path) == joinArray({encodeRow(r1), encodeRow(r2), encodeRow(gone), encodeRow(r3)}));
  CHECK(readFile(bad) == corrupt && !anyTmp(dir));

  // (3) a .tmp cut mid-write (or empty) is dropped ...
  const std::string partials[] = {"", "[", complete.substr(0, complete.size() - 1),
                                  complete.substr(0, complete.size() / 2)};
  for (const std::string &partial : partials) {
    clearDir(dir);
    resetHooks();
    writeFile(tmp, partial);
    WarmNewRow add3 = r3;
    CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &add3, 1, big, h, &res) == WARM_OK && res.wrote);
    CHECK_MSG(readFile(path) == joinArray({encodeRow(r3)}) && !anyTmp(dir) && countLogs("restored") == 0,
              "partial .tmp of %zu bytes", partial.size());
  }
  // ... and with a .bad beside it, the rows are salvaged from the .bad again
  clearDir(dir);
  resetHooks();
  writeFile(bad, corrupt);
  writeFile(tmp, "[" + encodeRow(r1));
  WarmNewRow add4 = r3;
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &add4, 1, big, h, &res) == WARM_OK && res.wrote);
  CHECK(readFile(path) == joinArray({encodeRow(r1), encodeRow(r2), encodeRow(r3)}) && readFile(bad) == corrupt);
  CHECK(!anyTmp(dir) && countLogs("restored") == 0);

  // (4) beside its month file a .tmp is dropped, even a complete one: the file is the
  // committed copy
  clearDir(dir);
  resetHooks();
  writeFile(path, complete);
  writeFile(tmp, joinArray({encodeRow(r3)}));
  WarmNewRow add5 = r2;
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &add5, 1, big, h, &res) == WARM_OK && !res.wrote);
  CHECK(readFile(path) == complete && !anyTmp(dir));

  // (5) a .tmp over the size cap is not one the merge wrote: dropped
  clearDir(dir);
  resetHooks();
  writeFile(tmp, complete);
  WarmNewRow add6 = r3;
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &add6, 1, (uint32_t)complete.size() - 1, h, &res) == WARM_OK);
  CHECK(readFile(path) == joinArray({encodeRow(r3)}) && !anyTmp(dir));

  // (6) when the .tmp cannot be checked or renamed, nothing is written and it is kept
  // for the next merge: a failed stat of it, a failed read of it, a failed rename
  clearDir(dir);
  resetHooks();
  writeFile(tmp, complete);
  WarmNewRow probe = r3;
  CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &probe, 1, big, h, &res) == WARM_OK);
  const std::vector<long> stats = opIndices(fi::OP_STAT);
  const std::vector<long> reads = opIndices(fi::OP_FREAD);
  const std::vector<long> renames = opIndices(fi::OP_RENAME);
  CHECK(stats.size() >= 2 && !reads.empty() && !renames.empty());
  if (stats.size() >= 2 && !reads.empty() && !renames.empty()) {
    const long faults[] = {stats[1], reads[0], renames[0]};
    for (long at : faults) {
      clearDir(dir);
      resetHooks();
      writeFile(tmp, complete);
      fi::state().failAt = at;
      WarmNewRow add7 = r3;
      CHECK_MSG(warmMergeMonth(dir.c_str(), 2026, 9, &add7, 1, big, h, &res) == WARM_IO_ERROR, "fault at op %ld", at);
      CHECK_MSG(!fileExists(path) && readFile(tmp) == complete && fi::state().writeOpens.empty(), "fault at op %ld",
                at);
      resetHooks();
      WarmNewRow add8 = r3;
      CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &add8, 1, big, h, &res) == WARM_OK && res.wrote);
      CHECK(readFile(path) == joinArray({encodeRow(r1), encodeRow(r2), encodeRow(r3)}) && !anyTmp(dir));
    }
  }
  // ... and no memory to read it
  {
    TestAllocator alloc;
    alloc.failAt = 0;
    const WarmHooks ha = makeHooks(&alloc);
    clearDir(dir);
    resetHooks();
    writeFile(tmp, complete);
    WarmNewRow add9 = r3;
    CHECK(warmMergeMonth(dir.c_str(), 2026, 9, &add9, 1, big, ha, &res) == WARM_NO_MEMORY);
    CHECK(!fileExists(path) && readFile(tmp) == complete && alloc.live() == 0);
  }

  // (7) warmRecoverMonthTmp alone (pruneDailySummaryFiles) reports the month file's state
  clearDir(dir);
  resetHooks();
  int state = -2;
  CHECK(warmRecoverMonthTmp(path.c_str(), tmp.c_str(), big, h, &state) == WARM_OK && state == 0);
  writeFile(tmp, complete);
  CHECK(warmRecoverMonthTmp(path.c_str(), tmp.c_str(), big, h, &state) == WARM_OK && state == 1);
  CHECK(readFile(path) == complete && !fileExists(tmp));
  writeFile(tmp, "[");
  CHECK(warmRecoverMonthTmp(path.c_str(), tmp.c_str(), big, h, &state) == WARM_OK && state == 1);
  CHECK(readFile(path) == complete && !fileExists(tmp));
  removeFile(path);
  writeFile(tmp, "[");
  CHECK(warmRecoverMonthTmp(path.c_str(), tmp.c_str(), big, h, &state) == WARM_OK && state == 0);
  CHECK(!fileExists(path) && !fileExists(tmp));
  clearDir(dir);
}

// ============================================================================
// Hot-tier prune cutoff (S-D03): no day is pruned before the rollup processed it
// ============================================================================

// pruneHotTierIfNeeded()'s compaction: keeps the snapshots at or after cutoff
static void pruneRing(LegacyRing &r, double cutoff) {
  std::vector<TelemetrySnapshot> kept;
  const WarmSeries s = r.series();
  for (uint16_t j = 0; j < r.snapshotCount; ++j) {
    const TelemetrySnapshot &snap = r.snapshots[warmRingIndex(s, r.snapshotCount, j)];
    if (snap.timestamp >= cutoff) kept.push_back(snap);
  }
  for (size_t j = 0; j < kept.size(); ++j) r.snapshots[j] = kept[j];
  r.snapshotCount = (uint16_t)kept.size();
  r.writeIndex = (uint16_t)(kept.size() % r.cap());
}

static void testPruneCutoff() {
  // The cutoff is at most the start of the first day the rollup has not processed
  {
    WarmRollupState s;
    const double cutoff = epochOf(2026, 6, 22, 3600.0);
    CHECK(warmPruneCutoff(s, cutoff) == cutoff);  // before the first tick
    s.nextDn = dnOf(2026, 9, 20);                  // up to date
    CHECK(warmPruneCutoff(s, cutoff) == cutoff);
    s.nextDn = dnOf(2026, 6, 1);                   // a backfill at June 1
    CHECK(warmPruneCutoff(s, cutoff) == epochOf(2026, 6, 1));
    s.nextDn = dnOf(2026, 9, 20);
    s.dirtyDn = dnOf(2026, 5, 30);                 // a late snapshot marked May 30
    CHECK(warmPruneCutoff(s, cutoff) == epochOf(2026, 5, 30));
  }

  // A 30-day backfill takes several ticks (2 rewrites each). Pruning a 7-day hot tier
  // after every tick, as the sketch does after each hourly rollup, loses no day with
  // the clamped cutoff; with the plain one, Sep 1..12 are pruned before they are rolled.
  const std::string dir = makeDir("prunecut");
  const WarmRollupConfig cfg = defaultConfig(dir);
  const WarmHooks h = makeHooks(nullptr);
  const int32_t today = dnOf(2026, 9, 20);
  const double now = (double)today * 86400.0 + 3600.0;
  const double retention = now - 7.0 * 86400.0;
  for (int clamp = 1; clamp >= 0; --clamp) {
    clearDir(dir);
    resetHooks();
    Fleet fleet;
    fleet.addSensors(2, 90);
    fleet.fill(today - 30, today - 1, 2, 30.0f);
    WarmRollupState s;
    int ticks = 0;
    for (; ticks < 20; ++ticks) {
      const WarmTickSummary sum = tick(s, fleet, now, cfg, h);
      const double cutoff = clamp ? warmPruneCutoff(s, retention) : retention;
      for (LegacyRing &r : fleet.rings) pruneRing(r, cutoff);
      if (sum.batches == 0) break;
    }
    CHECK_MSG(ticks == (clamp ? 3 : 2) && s.nextDn == today, "clamp %d: %d ticks", clamp, ticks);
    const auto aug = fileKeys(monthFile(dir, 2026, 8));
    const auto sep = fileKeys(monthFile(dir, 2026, 9));
    long missing = 0;
    for (int32_t dn = today - 30; dn <= today - 1; ++dn) {
      int y, m, d;
      warmDnToCivil(dn, &y, &m, &d);
      const auto &keys = (m == 8) ? aug : sep;
      for (const LegacyRing &r : fleet.rings) {
        auto it = keys.find(keyOf(warmDnToYmd(dn), r.clientUid.c_str(), r.sensorIndex));
        if (it == keys.end() || it->second.size() != 1 || it->second[0] != 2) missing++;
      }
    }
    CHECK_MSG(missing == (clamp ? 0 : 24), "clamp %d: %ld rows missing", clamp, missing);
    // once the rollup is idle the plain cutoff applies again
    CHECK(warmPruneCutoff(s, retention) == retention);
  }

  // A month retried after an I/O error keeps its days in the hot tier too
  {
    clearDir(dir);
    resetHooks();
    Fleet fleet;
    fleet.addSensors(1, 90);
    fleet.fill(today - 30, today - 1, 2, 30.0f);
    WarmRollupState s;
    tick(s, fleet, now, cfg, h);
    const int32_t at = s.nextDn;
    CHECK(at == dnOf(2026, 9, 1));
    resetHooks();
    fi::state().failAt = 0;
    tick(s, fleet, now, cfg, h);
    CHECK(s.stats.ioErrors == 1 && s.nextDn == at && warmPruneCutoff(s, retention) == (double)at * 86400.0);
  }
  clearDir(dir);
}

// ============================================================================
int main() {
  // Always under /tmp, not $TMPDIR: the store's path buffers are sized for
  // "/fs/history", and macOS's long $TMPDIR would overflow them (see makeDir)
  char root[] = "/tmp/warm_store_test_XXXXXX";
  if (!mkdtemp(root)) {
    perror("mkdtemp");
    return 2;
  }
  gRoot = root;

  struct Test {
    const char *name;
    void (*fn)();
  };
  const Test tests[] = {
      {"calendar", testCalendar},
      {"grammar", testGrammar},
      {"truncation", testTruncation},
      {"differential", testDifferential},
      {"merge semantics", testMergeSemantics},
      {"quarantine", testQuarantine},
      {"scheduler", testScheduler},
      {"fault injection", testFaultInjection},
      {"H-23 replay", testH23Replay},
      {"manifest", testManifest},
      {"provenance", testProvenance},
      {"tmp recovery", testTmpRecovery},
      {"prune cutoff", testPruneCutoff},
  };
  for (const Test &t : tests) {
    const long before = gFailures;
    resetHooks();
    t.fn();
    printf("%-16s %s\n", t.name, gFailures == before ? "ok" : "FAILED");
  }

  for (const std::string &n : listDir(gRoot)) {
    const std::string p = joinPath(gRoot, n);
    clearDir(p);
    rmdir(p.c_str());
  }
  rmdir(gRoot.c_str());

  printf("%ld checks, %ld failures\n", gChecks, gFailures);
  return gFailures ? 1 : 0;
}
