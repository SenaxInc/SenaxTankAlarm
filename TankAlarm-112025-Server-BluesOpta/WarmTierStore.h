// WarmTierStore.h - warm-tier (daily summary) storage for the TankAlarm 112025 server
//
// S-D03: the warm tier keeps one JSON array per UTC month in
// /fs/history/daily_YYYYMM.json with one row per (day, client, sensor):
//   {"d":20260921,"c":"dev:...","k":1,"mn":..,"mx":..,"av":..,"op":..,"cl":..,"al":0,"vt":12.4,"n":24}
// and the archived-client manifest in /fs/archived_clients.json. The file
// formats are the same as v2.2.15; what changed is how they are read and
// written:
//  - Files are read one array element at a time through a small buffer, so
//    memory use does not grow with the file (a 20-sensor month is ~76 KB).
//  - A merge never empties a file because a read failed. Only stat() reporting
//    ENOENT starts a new file; I/O and allocation failures abort without
//    writing; an unparseable file is kept as <file>.bad and its readable
//    prefix is carried into the rebuilt file.
//  - Writes go to <file>.tmp and are renamed over the target. fwrite, fflush
//    and fclose are all checked, and the target is never removed first.
//  - The rollup backfills every missed day still in the hot tier, in bounded
//    batches, and re-rolls days that received late snapshots or alarms.
//
// This header has no Arduino or mbed dependency so tests/host/warm_store can
// build it with the PC compiler. Tests define WARM_IO_OVERRIDE and supply the
// WARM_* I/O macros themselves to inject faults.

#ifndef TANKALARM_WARM_TIER_STORE_H
#define TANKALARM_WARM_TIER_STORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include <ArduinoJson.h>

// ============================================================================
// I/O indirection (the host tests route these through fault injection)
// ============================================================================
#ifndef WARM_IO_OVERRIDE
#define WARM_FOPEN(path, mode) fopen((path), (mode))
#define WARM_FREAD(buf, size, n, f) fread((buf), (size), (n), (f))
#define WARM_FWRITE(buf, size, n, f) fwrite((buf), (size), (n), (f))
#define WARM_FFLUSH(f) fflush(f)
#define WARM_FCLOSE(f) fclose(f)
#define WARM_FERROR(f) ferror(f)
#define WARM_FSEEK(f, off, whence) fseek((f), (off), (whence))
#define WARM_FTELL(f) ftell(f)
#define WARM_RENAME(from, to) rename((from), (to))
#define WARM_REMOVE(path) remove(path)
#define WARM_STAT(path, st) stat((path), (st))
#define WARM_MKDIR(path) mkdir((path), 0777)
#define WARM_MALLOC(n) malloc(n)
#define WARM_FREE(p) free(p)
#endif

// ============================================================================
// Limits
// ============================================================================
#ifndef WARM_ROW_MAX_WRITE
#define WARM_ROW_MAX_WRITE 256        // Longest daily row we write (real rows are ~120 B)
#endif
#ifndef WARM_ELEM_MAX
#define WARM_ELEM_MAX 1024            // Longest array element we read (row or manifest entry)
#endif
#ifndef WARM_READ_BUF
#define WARM_READ_BUF 256             // fread chunk
#endif
#ifndef WARM_MAX_DAYS_PER_BATCH
#define WARM_MAX_DAYS_PER_BATCH 8     // Days merged into a month file per rewrite
#endif
#ifndef WARM_YIELD_EVERY
#define WARM_YIELD_EVERY 32           // Rows between yield() (watchdog) calls
#endif
#ifndef WARM_MAX_FAILED_TICKS
#define WARM_MAX_FAILED_TICKS 6       // Ticks in a row a failing month is retried before it is skipped
#endif
#ifndef WARM_MIN_VALID_EPOCH
#define WARM_MIN_VALID_EPOCH 1704067200.0  // 2024-01-01Z: below this the clock is not set
#endif
#ifndef WARM_MAX_VALID_EPOCH
#define WARM_MAX_VALID_EPOCH 4102444800.0  // 2100-01-01Z: the date helpers stop at 2099
#endif
#ifndef WARM_MAX_FUTURE_SKEW_SEC
#define WARM_MAX_FUTURE_SKEW_SEC 3600.0    // A reading's time may lead the server clock by this much (both are network time)
#endif

static_assert(WARM_READ_BUF > 0 && WARM_READ_BUF <= 65535, "WARM_READ_BUF must fit uint16_t");
static_assert(WARM_MAX_FAILED_TICKS >= 1 && WARM_MAX_FAILED_TICKS <= 255, "WARM_MAX_FAILED_TICKS must fit uint8_t");

// ============================================================================
// Types
// ============================================================================

// Hot-tier ring entry (moved here from the sketch; layout unchanged)
struct TelemetrySnapshot {
  double timestamp;           // Epoch timestamp
  float level;                // Level in inches
  float voltage;              // VIN voltage (0 if not available)
};

enum WarmStatus : uint8_t {
  WARM_OK = 0,
  WARM_ABSENT,       // stat()/fopen() reported ENOENT
  WARM_CORRUPT,      // bytes read fine but are not a valid array of objects
  WARM_TOO_BIG,      // over the size cap
  WARM_IO_ERROR,     // stat/open/read/write/rename failed, or a short read
  WARM_NO_MEMORY     // malloc or ArduinoJson allocation failed
};

static inline const char *warmStatusName(uint8_t status) {
  switch (status) {
    case WARM_OK: return "ok";
    case WARM_ABSENT: return "absent";
    case WARM_CORRUPT: return "corrupt";
    case WARM_TOO_BIG: return "too_big";
    case WARM_IO_ERROR: return "io_error";
    case WARM_NO_MEMORY: return "no_memory";
    default: return "unknown";
  }
}

// Platform hooks. Every member may be null.
struct WarmHooks {
  void (*yield)();                                            // kick the watchdog
  void (*log)(const char *level, const char *msg);            // "info" / "warn" / "error"
  void (*noteWrite)(const char *path, uint32_t bytes, bool ok);  // flash write accounting
  uint32_t (*nowMs)();                                        // millis()
  ArduinoJson::Allocator *jsonAlloc;                          // null = ArduinoJson's default
};

// One hot-tier ring, viewed in place (no copy)
struct WarmSeries {
  const char *uid;
  uint8_t k;
  const TelemetrySnapshot *ring;
  uint16_t cap, count, writeIndex;
};

enum WarmRowState : uint8_t { WARM_ROW_ADD = 0, WARM_ROW_SUPERSEDED = 1, WARM_ROW_REPLACES = 2 };

// A freshly computed daily row, before it is merged into the month file
struct WarmNewRow {
  const char *c;
  uint32_t d;
  uint8_t k, al, state;
  float mn, mx, av, op, cl, vt;
  uint16_t n;
};

typedef uint8_t (*WarmAlarmCountFn)(void *ctx, const char *uid, uint8_t k, double begin, double end);
typedef float (*WarmRoundFn)(float value, int decimals);
// Returning false stops the scan; the scan still reports WARM_OK.
typedef bool (*WarmRowVisitor)(JsonObjectConst row, const char *raw, size_t rawLen, void *ctx);

struct WarmRollupStats {
  uint32_t writes, noChange, ioErrors, noMemory, tooBig, quarantined, salvagedRows, skippedDays, lateMarks;
  uint8_t lastError;
  uint32_t lastErrorYmd, lastTickMs, maxTickMs;
};

// Rollup scheduler state. nextDn/dirtyDn live in RAM only; cursorYmd is the
// persisted "lastRollup" (latest day rolled up).
struct WarmRollupState {
  int32_t nextDn = -1;           // next day to roll; -1 = boot, re-check the whole window
  int32_t dirtyDn = INT32_MAX;   // earliest rolled day that received a late snapshot or alarm
  uint32_t cursorYmd = 0;
  uint8_t failedTicks = 0;       // ticks in a row that stopped on an I/O or memory error
  WarmRollupStats stats = {};
};

struct WarmRollupConfig {
  const char *dir;
  uint32_t maxFileBytes;
  uint8_t maxSeries;
  int32_t maxBackfillDays;
  uint8_t retainedMonths, maxBatches, maxWrites;
  uint32_t budgetMs;
};

struct WarmMergeResult {
  bool wrote, quarantined;
  uint32_t bytes, salvagedRows, failOffset;
};

struct WarmTickSummary {
  uint32_t firstYmd, lastYmd;   // days covered by the batches that ran
  uint8_t batches, writes;
};

// ============================================================================
// Small helpers
// ============================================================================

// Public API only: the firmware builds against whatever ArduinoJson is current
static inline JsonDocument warmMakeDoc(const WarmHooks &h) {
  return h.jsonAlloc ? JsonDocument(h.jsonAlloc) : JsonDocument();
}

static inline void warmLogf(const WarmHooks &h, const char *level, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static inline void warmLogf(const WarmHooks &h, const char *level, const char *fmt, ...) {
  if (!h.log) return;
  char msg[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  h.log(level, msg);
}

static inline void warmCopyStr(char *dst, size_t cap, const char *src) {
  if (!dst || cap == 0) return;
  size_t n = src ? strlen(src) : 0;
  if (n >= cap) n = cap - 1;
  if (n > 0) memcpy(dst, src, n);
  dst[n] = '\0';
}

// Ring position j (0 = oldest) as the sketch walks it
static inline uint16_t warmRingIndex(const WarmSeries &s, uint16_t count, uint16_t j) {
  return (uint16_t)(((int)s.writeIndex - (int)count + (int)j + (int)s.cap) % (int)s.cap);
}

// ============================================================================
// Date math: day numbers (dn, 0 = 1970-01-01), UTC, no mktime/TZ dependence
// ============================================================================

static inline int32_t warmCivilToDn(int y, int m, int d) {
  // Howard Hinnant's days_from_civil
  y -= (m <= 2) ? 1 : 0;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int mp = (m > 2) ? m - 3 : m + 9;
  const int doy = (153 * mp + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int32_t)(era * 146097 + doe - 719468);
}

static inline void warmDnToCivil(int32_t dn, int *y, int *m, int *d) {
  const int32_t z = dn + 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int32_t doe = z - era * 146097;
  const int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int32_t mp = (5 * doy + 2) / 153;
  const int32_t dd = doy - (153 * mp + 2) / 5 + 1;
  const int32_t mm = (mp < 10) ? mp + 3 : mp - 9;
  *y = (int)(yoe + era * 400 + (mm <= 2 ? 1 : 0));
  *m = (int)mm;
  *d = (int)dd;
}

static inline uint32_t warmDnToYmd(int32_t dn) {
  int y, m, d;
  warmDnToCivil(dn, &y, &m, &d);
  if (y < 0) return 0;
  return (uint32_t)y * 10000u + (uint32_t)m * 100u + (uint32_t)d;
}

// Accepts 2020-01-01..2099-12-31 only; anything else (including 0) is "no date"
static inline bool warmYmdToDn(uint32_t ymd, int32_t *dn) {
  const int y = (int)(ymd / 10000u);
  const int m = (int)((ymd / 100u) % 100u);
  const int d = (int)(ymd % 100u);
  if (y < 2020 || y > 2099 || m < 1 || m > 12 || d < 1 || d > 31) return false;
  const int32_t v = warmCivilToDn(y, m, d);
  if (warmDnToYmd(v) != ymd) return false;  // e.g. Feb 30
  if (dn) *dn = v;
  return true;
}

static inline int32_t warmEpochToDn(double epoch) {
  const double days = floor(epoch / 86400.0);
  if (!(days >= -2147483648.0)) return INT32_MIN;  // also NaN
  if (days > 2147483647.0) return INT32_MAX;
  return (int32_t)days;
}

static inline int32_t warmFirstDnOfMonth(int32_t dn) {
  int y, m, d;
  warmDnToCivil(dn, &y, &m, &d);
  return warmCivilToDn(y, m, 1);
}

static inline int32_t warmLastDnOfMonth(int32_t dn) {
  int y, m, d;
  warmDnToCivil(dn, &y, &m, &d);
  if (++m > 12) { m = 1; ++y; }
  return warmCivilToDn(y, m, 1) - 1;
}

// First day of the month `months` months before today's month: the oldest
// month pruneDailySummaryFiles() keeps when months = MAX_DAILY_SUMMARY_MONTHS.
static inline int32_t warmRetainedFloorDn(int32_t todayDn, int months) {
  int y, m, d;
  warmDnToCivil(todayDn, &y, &m, &d);
  m -= months;
  while (m <= 0) { m += 12; --y; }
  return warmCivilToDn(y, m, 1);
}

static inline bool warmMonthPath(char *out, size_t cap, const char *dir, int year, int month, const char *suffix) {
  const int n = snprintf(out, cap, "%s/daily_%04d%02d.json%s", dir ? dir : "", year, month, suffix ? suffix : "");
  return n > 0 && (size_t)n < cap;
}

// 1 = exists, 0 = does not exist (ENOENT), -1 = could not tell
static inline int warmStat(const char *path) {
  struct stat st;
  errno = 0;
  if (WARM_STAT(path, &st) == 0) return 1;
  return (errno == ENOENT) ? 0 : -1;
}

static inline long warmFileSize(FILE *f) {
  if (WARM_FSEEK(f, 0, SEEK_END) != 0) return -1;
  const long size = WARM_FTELL(f);
  if (size < 0 || WARM_FSEEK(f, 0, SEEK_SET) != 0) return -1;
  return size;
}

// ============================================================================
// S-D03: what enters the hot tier: fresh readings, at the time they were taken
// ============================================================================

// A reading's acquisition time in whole seconds, or 0 when it is unknown: before
// 2020 (0 = the client clock was not set yet), not finite, from 2100 on, or more
// than WARM_MAX_FUTURE_SKEW_SEC ahead of serverNow (serverNow <= 0: server clock
// not set). There is deliberately no fallback to a receive time, which would file
// the reading under a day it was not taken on. Flooring never changes the UTC day.
static inline double warmAcquisitionEpoch(double eventEpoch, double serverNow) {
  if (!(eventEpoch >= 1577836800.0 && eventEpoch < WARM_MAX_VALID_EPOCH)) return 0.0;  // also NaN
  if (serverNow > 0.0 && eventEpoch > serverNow + WARM_MAX_FUTURE_SKEW_SEC) return 0.0;
  return floor(eventEpoch);
}

// Clients send telemetry and alarm `t` as a double, and it arrives rounded to the
// nearest second (ArduinoJson writes 10 significant digits). So a `t` of exactly
// 00:00:00Z may be from the last half second of the day before, and its day is not
// known. The daily report's per-sensor `t` is truncated and does not need this.
static inline bool warmRoundedEpochAtMidnight(double t) {
  return t > 0.0 && fmod(t, 86400.0) == 0.0;
}

// Older firmware saved hot-tier timestamps as doubles. ArduinoJson keeps one that
// fits a float (a multiple of 128 s) as a float and writes 7 digits, so it reloads
// up to 512 s off, again as a multiple of 128. Near a UTC midnight such a value
// may belong to the other day.
static inline bool warmLegacyEpochAmbiguous(double ts) {
  if (!(ts > 0.0) || fmod(ts, 128.0) != 0.0) return false;
  const double intoDay = ts - floor(ts / 86400.0) * 86400.0;
  return intoDay <= 512.0 || intoDay >= 86400.0 - 512.0;
}

// True when the ring already holds this acquisition. Telemetry `t` arrives
// rounded to the second and the daily report's per-sensor `t` truncated, so two
// copies of one reading can be 1 s apart, and they can arrive out of order. The
// ring holds one sensor, which is read once per sample pass, so the time decides.
// The level cannot: a current-loop level is recomputed on arrival, with the
// temperature of that moment.
static inline bool warmRingHasAcquisition(const TelemetrySnapshot *ring, uint16_t cap, uint16_t count,
                                          uint16_t writeIndex, double ts) {
  if (!ring || cap == 0) return false;
  const WarmSeries s = {nullptr, 0, ring, cap, count, writeIndex};
  const uint16_t cnt = (count > cap) ? cap : count;
  for (uint16_t j = 0; j < cnt; ++j) {
    if (fabs(ring[warmRingIndex(s, cnt, j)].timestamp - ts) <= 1.0) return true;
  }
  return false;
}

// True when a and b are on the same UTC day and at most maxDiffSec apart; false
// when either is not a time (<= 0).
static inline bool warmSameUtcDayWithin(double a, double b, double maxDiffSec) {
  if (!(a > 0.0) || !(b > 0.0)) return false;
  return fabs(a - b) <= maxDiffSec && warmEpochToDn(a) == warmEpochToDn(b);
}

// ============================================================================
// Buffered reader and array element splitter
// ============================================================================

enum WarmElem : uint8_t { WARM_ELEM_ROW, WARM_ELEM_END, WARM_ELEM_BAD, WARM_ELEM_IO, WARM_ELEM_OVERRUN };

// Also satisfies ArduinoJson's custom reader contract (read()/readBytes()).
struct WarmFileReader {
  FILE *f;
  uint32_t fileSize, total, limit;  // limit 0 = none
  uint16_t pos, len;
  bool ioError, eof, overrun;
  char buf[WARM_READ_BUF];

  WarmFileReader() : f(nullptr), fileSize(0), total(0), limit(0), pos(0), len(0),
                     ioError(false), eof(false), overrun(false) {}

  void begin(FILE *file, uint32_t size, uint32_t lim) {
    f = file; fileSize = size; total = 0; limit = lim; pos = 0; len = 0;
    ioError = false; eof = false; overrun = false;
  }

  int read() {
    if (limit != 0 && total >= limit) { overrun = true; return -1; }
    if (pos >= len) {
      if (ioError || eof || !f) return -1;
      const size_t got = WARM_FREAD(buf, 1, sizeof(buf), f);
      if (got == 0) {
        if (WARM_FERROR(f)) ioError = true; else eof = true;
        return -1;
      }
      pos = 0;
      len = (uint16_t)got;
    }
    total++;
    return (uint8_t)buf[pos++];
  }

  size_t readBytes(char *dst, size_t n) {
    size_t i = 0;
    for (; i < n; ++i) {
      const int c = read();
      if (c < 0) break;
      dst[i] = (char)c;
    }
    return i;
  }

  // A read error or a file shorter than its size is I/O, never corruption,
  // so a flaky read cannot get a good file quarantined.
  bool ioFailed() const { return ioError || (eof && total < fileSize); }

  WarmElem eofKind() const {
    if (ioFailed()) return WARM_ELEM_IO;
    if (overrun) return WARM_ELEM_OVERRUN;
    return WARM_ELEM_BAD;
  }
};

static inline int warmSkipSpace(WarmFileReader &r) {
  int c;
  do { c = r.read(); } while (c == ' ' || c == '\t' || c == '\r' || c == '\n');
  return c;
}

// Returns the next element of a JSON array (after the '[' was consumed) as
// raw bytes in out (NUL-terminated). Only objects are accepted. Tracks
// strings, escapes and nesting at the byte level, so it does not depend on
// how far ArduinoJson reads ahead. Bytes after the closing ']' are ignored.
static inline WarmElem warmNextElement(WarmFileReader &r, bool &first, char *out, size_t cap, size_t &len) {
  len = 0;
  int c = warmSkipSpace(r);
  if (c < 0) return r.eofKind();
  if (c == ']') return WARM_ELEM_END;
  if (!first) {
    if (c != ',') return WARM_ELEM_BAD;
    c = warmSkipSpace(r);
    if (c < 0) return r.eofKind();
  }
  if (c != '{') return WARM_ELEM_BAD;
  first = false;

  bool inString = false, escape = false;
  int depth = 0;
  for (;;) {
    if (len + 1 >= cap) return WARM_ELEM_BAD;  // element too long
    out[len++] = (char)c;
    if (inString) {
      if (escape) escape = false;
      else if (c == '\\') escape = true;
      else if (c == '"') inString = false;
    } else if (c == '"') {
      inString = true;
    } else if (c == '{' || c == '[') {
      depth++;
    } else if (c == '}' || c == ']') {
      if (--depth == 0) {
        out[len] = '\0';
        return WARM_ELEM_ROW;
      }
    }
    c = r.read();
    if (c < 0) return r.eofKind();
  }
}

static inline WarmStatus warmElemStatus(WarmElem e) {
  if (e == WARM_ELEM_IO) return WARM_IO_ERROR;
  if (e == WARM_ELEM_OVERRUN) return WARM_TOO_BIG;
  return WARM_CORRUPT;
}

// ============================================================================
// Month file scan
// ============================================================================

static inline WarmStatus warmScanElements(WarmFileReader &r, uint32_t maxRows, WarmRowVisitor visit,
                                          void *ctx, const WarmHooks &h, uint32_t *rowsOut) {
  int c = warmSkipSpace(r);
  if (c < 0) return warmElemStatus(r.eofKind());
  if (c != '[') return WARM_CORRUPT;

  char elem[WARM_ELEM_MAX + 1];
  JsonDocument rowDoc = warmMakeDoc(h);
  bool first = true;
  uint32_t rows = 0;
  for (;;) {
    size_t len = 0;
    const WarmElem e = warmNextElement(r, first, elem, sizeof(elem), len);
    if (e == WARM_ELEM_END) return WARM_OK;
    if (e != WARM_ELEM_ROW) return warmElemStatus(e);
    const DeserializationError err = deserializeJson(rowDoc, elem, len);
    if (err == DeserializationError::NoMemory) return WARM_NO_MEMORY;
    if (err || !rowDoc.is<JsonObject>()) return WARM_CORRUPT;
    *rowsOut = ++rows;
    const bool more = visit ? visit(rowDoc.as<JsonObjectConst>(), elem, len, ctx) : true;
    if (h.yield && (rows % WARM_YIELD_EVERY) == 0) h.yield();
    if (!more || (maxRows != 0 && rows >= maxRows)) return WARM_OK;
  }
}

// Streams the rows of a month file (or any JSON array of objects) to visit.
//  strict  (salvage=false): OK only for a complete, valid array no larger than
//          maxBytes; ABSENT, CORRUPT, TOO_BIG, IO_ERROR or NO_MEMORY otherwise.
//  salvage (salvage=true):  reads at most maxBytes and turns CORRUPT/TOO_BIG
//          into OK with the rows before the damage; IO_ERROR and NO_MEMORY
//          are still returned.
// maxRows 0 = no limit. *failOffset = bytes consumed when the scan stopped.
// Contract: on any status other than OK the visitor has already seen some rows,
// so callers must throw away whatever they aggregated.
static inline WarmStatus warmScanFile(const char *path, bool salvage, uint32_t maxBytes, uint32_t maxRows,
                                      WarmRowVisitor visit, void *ctx, const WarmHooks &h,
                                      uint32_t *rowsOut, uint32_t *failOffset) {
  uint32_t rows = 0;
  if (rowsOut) *rowsOut = 0;
  if (failOffset) *failOffset = 0;

  errno = 0;
  FILE *f = WARM_FOPEN(path, "rb");
  if (!f) {
    if (errno == ENOENT) return WARM_ABSENT;
    return (warmStat(path) == 0) ? WARM_ABSENT : WARM_IO_ERROR;
  }

  WarmFileReader r;
  WarmStatus st;
  const long size = warmFileSize(f);
  if (size < 0) {
    st = WARM_IO_ERROR;
  } else if (!salvage && (uint32_t)size > maxBytes) {
    st = WARM_TOO_BIG;  // decided without reading
  } else if (size == 0) {
    st = salvage ? WARM_OK : WARM_CORRUPT;
  } else {
    r.begin(f, (uint32_t)size, ((uint32_t)size > maxBytes) ? maxBytes : 0);
    st = warmScanElements(r, maxRows, visit, ctx, h, &rows);
  }
  WARM_FCLOSE(f);  // read-only: a close error cannot lose data

  if (salvage && (st == WARM_CORRUPT || st == WARM_TOO_BIG)) st = WARM_OK;
  if (rowsOut) *rowsOut = rows;
  if (failOffset) *failOffset = r.total;
  return st;
}

// ============================================================================
// Row computation (same math as the v2.2.15 single-day rollup)
// ============================================================================

// Computes rows for days firstDn..lastDn (same month, at most
// WARM_MAX_DAYS_PER_BATCH days), ordered by day and then by series, which is
// the order repeated single-day rollups would have appended them in.
// Snapshots with a non-finite level are skipped (they would serialize as null).
static inline uint16_t warmComputeRows(const WarmSeries *series, uint8_t ns, int32_t firstDn, int32_t lastDn,
                                       WarmAlarmCountFn alarmFn, void *actx, WarmRoundFn roundFn,
                                       WarmNewRow *out, uint16_t cap) {
  uint16_t count = 0;
  const int64_t span = (int64_t)lastDn - (int64_t)firstDn;
  if (!series || !out || !roundFn || span < 0 || span >= WARM_MAX_DAYS_PER_BATCH) return 0;

  for (int32_t di = 0; di <= (int32_t)span; ++di) {
    const int32_t dn = firstDn + di;
    const double dayBegin = (double)dn * 86400.0;
    const double dayEnd = dayBegin + 86400.0;
    for (uint8_t i = 0; i < ns; ++i) {
      const WarmSeries &s = series[i];
      if (!s.uid || !s.ring || s.cap == 0 || s.count == 0) continue;
      const uint16_t cnt = (s.count > s.cap) ? s.cap : s.count;

      // Accumulators and update order as in v2.2.15, so a single day's row is
      // bit-identical to what it wrote.
      float minLevel = 999999.0f, maxLevel = -999999.0f;
      float sumLevel = 0.0f, sumVoltage = 0.0f;
      float openingLevel = 0.0f, closingLevel = 0.0f;
      double oldestTs = 1e18, newestTs = 0.0;
      uint16_t n = 0, voltCount = 0;
      for (uint16_t j = 0; j < cnt; ++j) {
        const TelemetrySnapshot &snap = s.ring[warmRingIndex(s, cnt, j)];
        if (!(snap.timestamp >= dayBegin && snap.timestamp < dayEnd)) continue;
        if (!isfinite(snap.level)) continue;

        if (snap.level < minLevel) minLevel = snap.level;
        if (snap.level > maxLevel) maxLevel = snap.level;
        sumLevel += snap.level;
        n++;

        if (snap.timestamp < oldestTs) { oldestTs = snap.timestamp; openingLevel = snap.level; }
        if (snap.timestamp > newestTs) { newestTs = snap.timestamp; closingLevel = snap.level; }

        if (snap.voltage > 0.0f && isfinite(snap.voltage)) { sumVoltage += snap.voltage; voltCount++; }
      }
      if (n == 0) continue;
      if (count >= cap) return count;

      WarmNewRow &row = out[count++];
      row.c = s.uid;
      row.d = warmDnToYmd(dn);
      row.k = s.k;
      row.al = alarmFn ? alarmFn(actx, s.uid, s.k, dayBegin, dayEnd) : 0;
      row.state = WARM_ROW_ADD;
      row.mn = roundFn(minLevel, 1);
      row.mx = roundFn(maxLevel, 1);
      row.av = roundFn(sumLevel / n, 1);
      row.op = roundFn(openingLevel, 1);
      row.cl = roundFn(closingLevel, 1);
      row.vt = voltCount > 0 ? roundFn(sumVoltage / voltCount, 2) : 0.0f;
      row.n = n;
    }
  }
  return count;
}

// Serializes a row with the v2.2.15 key order and value types. Returns the
// length, or 0 when it does not fit in cap (or, with *noMemory set, when
// ArduinoJson could not allocate).
static inline size_t warmEncodeRow(const WarmNewRow &r, char *out, size_t cap, const WarmHooks &h, bool *noMemory) {
  if (noMemory) *noMemory = false;
  JsonDocument doc = warmMakeDoc(h);
  doc["d"] = r.d;
  doc["c"] = r.c;
  doc["k"] = r.k;
  doc["mn"] = r.mn;
  doc["mx"] = r.mx;
  doc["av"] = r.av;
  doc["op"] = r.op;
  doc["cl"] = r.cl;
  doc["al"] = r.al;
  doc["vt"] = r.vt;
  doc["n"] = r.n;
  if (doc.overflowed()) {
    if (noMemory) *noMemory = true;
    return 0;
  }
  if (!out || measureJson(doc) >= cap) return 0;
  return serializeJson(doc, out, cap);
}

// ============================================================================
// Checked write-to-.tmp-then-rename writer (also an ArduinoJson custom Writer)
// ============================================================================
struct WarmAtomicWriter {
  FILE *f;
  char path[112];
  char tmp[116];
  uint32_t bytes;
  bool failed;

  WarmAtomicWriter() : f(nullptr), bytes(0), failed(false) { path[0] = '\0'; tmp[0] = '\0'; }
  ~WarmAtomicWriter() { abort(); }
  WarmAtomicWriter(const WarmAtomicWriter &) = delete;
  WarmAtomicWriter &operator=(const WarmAtomicWriter &) = delete;

  bool begin(const char *target) {
    abort();
    bytes = 0;
    failed = false;
    const int n1 = snprintf(path, sizeof(path), "%s", target ? target : "");
    const int n2 = snprintf(tmp, sizeof(tmp), "%s.tmp", target ? target : "");
    if (n1 <= 0 || (size_t)n1 >= sizeof(path) || n2 <= 0 || (size_t)n2 >= sizeof(tmp)) {
      path[0] = '\0';
      tmp[0] = '\0';  // never remove a truncated name
      failed = true;
      return false;
    }
    f = WARM_FOPEN(tmp, "wb");
    if (!f) failed = true;
    return f != nullptr;
  }

  size_t write(uint8_t c) { return write(&c, 1); }

  size_t write(const uint8_t *s, size_t n) {
    if (failed || !f) return 0;
    const size_t w = WARM_FWRITE(s, 1, n, f);
    if (w != n || WARM_FERROR(f)) failed = true;
    bytes += (uint32_t)w;
    return w;
  }

  // Flushes and closes the .tmp and renames it over the target. With
  // moveAsideTo, the current target is renamed there first (to keep an
  // unreadable original); a power cut between the two renames leaves a
  // complete .tmp beside a missing target. Any failure removes the .tmp and
  // leaves the target as it was, except when the original cannot be put back:
  // then the .tmp is the only complete copy and is kept, which is the same
  // state as that power cut. The target is never removed before the rename.
  bool commit(const WarmHooks &h, const char *moveAsideTo = nullptr) {
    if (!f) failed = true;
    if (f) {
      if (!failed && (WARM_FFLUSH(f) != 0 || WARM_FERROR(f))) failed = true;
      if (WARM_FCLOSE(f) != 0) failed = true;  // write-back errors surface here
      f = nullptr;
    }
    bool ok = !failed && path[0] != '\0';
    bool keepTmp = false;
    if (ok && moveAsideTo && WARM_RENAME(path, moveAsideTo) != 0) ok = false;
    if (ok && WARM_RENAME(tmp, path) != 0) {
      if (moveAsideTo && WARM_RENAME(moveAsideTo, path) != 0) keepTmp = true;  // put the original back
      ok = false;
    }
    if (!ok) {
      failed = true;
      if (tmp[0] && !keepTmp) WARM_REMOVE(tmp);  // the data is regenerable; the target is untouched
    }
    if (h.noteWrite) h.noteWrite(path, ok ? bytes : 0, ok);
    return ok;
  }

  void abort() {
    if (f) {
      WARM_FCLOSE(f);
      f = nullptr;
      WARM_REMOVE(tmp);
      failed = true;
    }
  }
};

// ============================================================================
// Month merge
// ============================================================================

struct WarmMergeCtx {
  WarmNewRow *rows;
  uint16_t count;
  uint32_t minD, maxD;
  WarmAtomicWriter *out;
  uint32_t maxBytes;
  bool wroteAny;
  bool tooBig;
};

// Pass 1: decide, per new row, whether the file already holds an equal or
// better row (SUPERSEDED) or a worse one (REPLACES). A row is better with a
// larger n, or with the same n and a larger al (S-D03: an alarm that arrived
// after its day was rolled up). Duplicate keys in the file resolve to
// SUPERSEDED.
static inline bool warmDecideVisitor(JsonObjectConst row, const char *raw, size_t rawLen, void *ctx) {
  (void)raw;
  (void)rawLen;
  WarmMergeCtx &m = *static_cast<WarmMergeCtx *>(ctx);
  const uint32_t d = row["d"] | (uint32_t)0;
  if (d < m.minD || d > m.maxD) return true;
  const uint8_t k = row["k"] | (uint8_t)0;
  const char *c = row["c"] | "";
  const uint32_t exN = row["n"] | (uint32_t)0;
  const uint8_t exAl = row["al"] | (uint8_t)0;
  for (uint16_t i = 0; i < m.count; ++i) {
    WarmNewRow &nr = m.rows[i];
    if (nr.d != d || nr.k != k || strcmp(nr.c, c) != 0) continue;
    if (exN > nr.n || (exN == nr.n && exAl >= nr.al)) nr.state = WARM_ROW_SUPERSEDED;
    else if (nr.state != WARM_ROW_SUPERSEDED) nr.state = WARM_ROW_REPLACES;
  }
  return true;
}

// Pass 2: copy every existing row byte for byte (unknown keys and number
// formatting survive), except rows being replaced; a replaced row's alarm
// count is carried over when it is higher (the 50-entry alarm ring may have
// forgotten alarms since the first rollup; it lives only in RAM, so a reboot
// forgets them all).
static inline bool warmCopyVisitor(JsonObjectConst row, const char *raw, size_t rawLen, void *ctx) {
  WarmMergeCtx &m = *static_cast<WarmMergeCtx *>(ctx);
  const uint32_t d = row["d"] | (uint32_t)0;
  if (d >= m.minD && d <= m.maxD) {
    const uint8_t k = row["k"] | (uint8_t)0;
    const char *c = row["c"] | "";
    bool drop = false;
    for (uint16_t i = 0; i < m.count; ++i) {
      WarmNewRow &nr = m.rows[i];
      if (nr.state != WARM_ROW_REPLACES || nr.d != d || nr.k != k || strcmp(nr.c, c) != 0) continue;
      const uint8_t oldAl = row["al"] | (uint8_t)0;
      if (oldAl > nr.al) nr.al = oldAl;
      drop = true;
    }
    if (drop) return true;
  }
  if (m.out->bytes + (m.wroteAny ? 1u : 0u) + rawLen + 1u > m.maxBytes) {
    m.tooBig = true;
    return false;
  }
  if (m.wroteAny) m.out->write((uint8_t)',');
  m.out->write((const uint8_t *)raw, rawLen);
  m.wroteAny = true;
  return !m.out->failed;
}

// Merges rows (all in year/month) into dir/daily_YYYYMM.json.
// Returns OK (res->wrote says whether the file changed), IO_ERROR or
// NO_MEMORY (nothing written; retry later), or TOO_BIG (the result would
// exceed maxFileBytes; nothing written). Invariants: at most one written row
// per (d, c, k); rows that are not re-rolled are kept; a stored row is only
// replaced by one with a larger n, or the same n and a larger al, and its al
// never goes down; the file is never emptied by a failed read; .bad is only
// ever created by rename.
static inline WarmStatus warmMergeMonth(const char *dir, int year, int month, WarmNewRow *rows, uint16_t count,
                                        uint32_t maxFileBytes, const WarmHooks &h, WarmMergeResult *res) {
  WarmMergeResult local = {};
  WarmMergeResult &r = res ? *res : local;
  r = WarmMergeResult();

  char path[96], tmpPath[100], badPath[100];
  if (!warmMonthPath(path, sizeof(path), dir, year, month, "") ||
      !warmMonthPath(tmpPath, sizeof(tmpPath), dir, year, month, ".tmp") ||
      !warmMonthPath(badPath, sizeof(badPath), dir, year, month, ".bad")) {
    return WARM_IO_ERROR;
  }

  // A leftover .tmp can only come from a power cut mid-write; drop it.
  if (warmStat(tmpPath) == 1) WARM_REMOVE(tmpPath);

  // Choose the source. Only ENOENT means "start a new file".
  const int pathState = warmStat(path);
  if (pathState < 0) return WARM_IO_ERROR;
  const char *src = nullptr;
  bool salvage = false;
  bool moveAside = false;
  if (pathState == 1) {
    src = path;
  } else {
    const int badState = warmStat(badPath);
    if (badState < 0) return WARM_IO_ERROR;
    if (badState == 1) {  // power cut between quarantine and rebuild
      src = badPath;
      salvage = true;
    }
  }

  WarmMergeCtx mc;
  mc.rows = rows;
  mc.count = count;
  mc.minD = UINT32_MAX;
  mc.maxD = 0;
  mc.out = nullptr;
  mc.maxBytes = maxFileBytes;
  mc.wroteAny = false;
  mc.tooBig = false;
  for (uint16_t i = 0; i < count; ++i) {
    rows[i].state = WARM_ROW_ADD;
    if (rows[i].d < mc.minD) mc.minD = rows[i].d;
    if (rows[i].d > mc.maxD) mc.maxD = rows[i].d;
  }

  // Pass 1 (read only)
  if (src) {
    uint32_t seen = 0, off = 0;
    WarmStatus st = warmScanFile(src, salvage, maxFileBytes, 0, warmDecideVisitor, &mc, h, &seen, &off);
    if (!salvage && (st == WARM_CORRUPT || st == WARM_TOO_BIG)) {
      // The original becomes .bad only when the rebuild commits (logged there)
      warmLogf(h, "error", "Warm tier: %s unreadable (%s at byte %lu); rebuilding from its readable rows",
               path, warmStatusName(st), (unsigned long)off);
      r.failOffset = off;
      salvage = true;
      moveAside = true;
      for (uint16_t i = 0; i < count; ++i) rows[i].state = WARM_ROW_ADD;
      st = warmScanFile(src, true, maxFileBytes, 0, warmDecideVisitor, &mc, h, &seen, &off);
    }
    if (st == WARM_ABSENT) st = WARM_IO_ERROR;  // stat() said it was there
    if (st != WARM_OK) return st;
    if (salvage) r.salvagedRows = seen;
  }

  // Nothing new or better: no flash write at all.
  uint16_t pending = 0;
  for (uint16_t i = 0; i < count; ++i) {
    if (rows[i].state != WARM_ROW_SUPERSEDED) pending++;
  }
  if (pending == 0 && !salvage) return WARM_OK;

  // Pass 2: stream the merged array into .tmp
  WARM_MKDIR(dir);  // usually exists already
  WarmAtomicWriter w;
  if (!w.begin(path)) {
    w.commit(h);  // records the failed write
    return WARM_IO_ERROR;
  }
  mc.out = &w;
  w.write((uint8_t)'[');
  if (src && !(salvage && r.salvagedRows == 0)) {
    uint32_t copied = 0, off = 0;
    const WarmStatus st = warmScanFile(src, salvage, maxFileBytes, salvage ? r.salvagedRows : 0,
                                       warmCopyVisitor, &mc, h, &copied, &off);
    if (st != WARM_OK) {
      w.abort();
      return (st == WARM_NO_MEMORY) ? WARM_NO_MEMORY : WARM_IO_ERROR;
    }
  }
  if (!mc.tooBig && !w.failed) {
    char enc[WARM_ROW_MAX_WRITE];
    for (uint16_t i = 0; i < count && !mc.tooBig && !w.failed; ++i) {
      if (rows[i].state == WARM_ROW_SUPERSEDED) continue;
      bool noMemory = false;
      const size_t len = warmEncodeRow(rows[i], enc, sizeof(enc), h, &noMemory);
      if (len == 0) {
        if (noMemory) {
          w.abort();
          return WARM_NO_MEMORY;
        }
        warmLogf(h, "error", "Warm tier: row %lu for sensor %u over %u bytes; not stored (%.48s)",
                 (unsigned long)rows[i].d, (unsigned)rows[i].k, (unsigned)WARM_ROW_MAX_WRITE, rows[i].c);
        continue;
      }
      if (w.bytes + (mc.wroteAny ? 1u : 0u) + len + 1u > maxFileBytes) {
        mc.tooBig = true;
        break;
      }
      if (mc.wroteAny) w.write((uint8_t)',');
      w.write((const uint8_t *)enc, len);
      mc.wroteAny = true;
    }
  }
  if (mc.tooBig) {
    w.abort();
    warmLogf(h, "error", "Warm tier: %s would exceed %lu bytes; %u rows for %lu-%lu not stored",
             path, (unsigned long)maxFileBytes, (unsigned)pending, (unsigned long)mc.minD, (unsigned long)mc.maxD);
    return WARM_TOO_BIG;
  }
  w.write((uint8_t)']');
  if (!w.commit(h, moveAside ? badPath : nullptr)) return WARM_IO_ERROR;

  r.wrote = true;
  r.quarantined = moveAside;  // the unreadable original is now <file>.bad
  r.bytes = w.bytes;
  if (salvage) {
    warmLogf(h, "warn", "Warm tier: %s rebuilt; kept %lu rows, original saved as .bad",
             path, (unsigned long)r.salvagedRows);
  }
  return WARM_OK;
}

// ============================================================================
// Rollup scheduler
// ============================================================================

// One bounded maintenance step (called hourly). Rolls days from s.nextDn up
// to yesterday in batches of at most WARM_MAX_DAYS_PER_BATCH days inside one
// month. On the first call after boot it re-checks the whole window the hot
// tier still covers (floor: maxBackfillDays, retained months, oldest
// snapshot), which restores days a crash or an older firmware lost; re-rolls
// of unchanged days cost reads only. IO_ERROR/NO_MEMORY stop the step
// without moving the position; the next call retries. After
// WARM_MAX_FAILED_TICKS calls in a row stop that way, the rest of the failing
// month is skipped (until the next boot's re-check) so one bad file cannot
// hold up the days after it.
static inline WarmTickSummary warmRollupTick(WarmRollupState &s, double now, const WarmSeries *series, uint8_t ns,
                                             WarmAlarmCountFn alarmFn, void *actx, WarmRoundFn roundFn,
                                             const WarmRollupConfig &cfg, const WarmHooks &h) {
  WarmTickSummary sum = {0, 0, 0, 0};
  if (!(now >= WARM_MIN_VALID_EPOCH && now < WARM_MAX_VALID_EPOCH)) return sum;  // clock not set
  const uint32_t t0 = h.nowMs ? h.nowMs() : 0;
  const int32_t todayDn = warmEpochToDn(now);
  const int32_t yDn = todayDn - 1;

  int32_t cursorDn = 0;
  bool haveCursor = warmYmdToDn(s.cursorYmd, &cursorDn);
  if (haveCursor && cursorDn > yDn) {  // clock went backwards
    cursorDn = yDn;
    s.cursorYmd = warmDnToYmd(yDn);
  }

  if (ns > cfg.maxSeries) ns = cfg.maxSeries;  // keeps rows per batch within cap
  bool any = false;
  int32_t oldestDn = INT32_MAX;
  for (uint8_t i = 0; series && i < ns; ++i) {
    const WarmSeries &sr = series[i];
    if (!sr.uid || !sr.ring || sr.cap == 0) continue;
    const uint16_t cnt = (sr.count > sr.cap) ? sr.cap : sr.count;
    for (uint16_t j = 0; j < cnt; ++j) {
      const int32_t dn = warmEpochToDn(sr.ring[warmRingIndex(sr, cnt, j)].timestamp);
      any = true;
      if (dn < oldestDn) oldestDn = dn;
    }
  }
  if (!any) {  // nothing to roll: same effect as v2.2.15's "no rows" path
    if (!haveCursor || cursorDn < yDn) s.cursorYmd = warmDnToYmd(yDn);
    s.nextDn = yDn + 1;
    s.dirtyDn = INT32_MAX;
    return sum;
  }

  int32_t floorDn = yDn - (cfg.maxBackfillDays > 0 ? cfg.maxBackfillDays - 1 : 0);
  const int32_t retainedDn = warmRetainedFloorDn(todayDn, cfg.retainedMonths);
  if (retainedDn > floorDn) floorDn = retainedDn;
  if (oldestDn > floorDn) floorDn = oldestDn;

  if (s.nextDn < 0 || s.nextDn > yDn + 1) s.nextDn = floorDn;  // boot, or clock went backwards
  if (s.dirtyDn < s.nextDn) s.nextDn = s.dirtyDn;              // late snapshots or alarms
  s.dirtyDn = INT32_MAX;
  if (s.nextDn < floorDn) s.nextDn = floorDn;
  if (s.nextDn > yDn) return sum;  // up to date: no I/O

  // ns <= maxSeries, so a batch never needs more than cap rows.
  const uint16_t cap = (uint16_t)(WARM_MAX_DAYS_PER_BATCH * cfg.maxSeries);
  if (cap == 0) return sum;
  WarmNewRow *rows = (WarmNewRow *)WARM_MALLOC(sizeof(WarmNewRow) * cap);
  if (!rows) {
    s.stats.noMemory++;
    s.stats.lastError = WARM_NO_MEMORY;
    s.stats.lastErrorYmd = warmDnToYmd(s.nextDn);
    warmLogf(h, "warn", "Warm tier: rollup deferred (no memory for %u rows)", (unsigned)cap);
    return sum;
  }

  bool failed = false;
  while (s.nextDn <= yDn && sum.batches < cfg.maxBatches && sum.writes < cfg.maxWrites) {
    if (h.nowMs && (uint32_t)(h.nowMs() - t0) >= cfg.budgetMs) break;
    const int32_t st = s.nextDn;
    int32_t en = st + (WARM_MAX_DAYS_PER_BATCH - 1);
    if (en > yDn) en = yDn;
    const int32_t monthEnd = warmLastDnOfMonth(st);
    if (en > monthEnd) en = monthEnd;
    int year, month, day;
    warmDnToCivil(st, &year, &month, &day);

    const uint16_t n = warmComputeRows(series, ns, st, en, alarmFn, actx, roundFn, rows, cap);
    WarmStatus status = WARM_OK;
    WarmMergeResult res = {};
    if (n > 0) {
      status = warmMergeMonth(cfg.dir, year, month, rows, n, cfg.maxFileBytes, h, &res);
      if (res.quarantined) {
        s.stats.quarantined++;
        s.stats.salvagedRows += res.salvagedRows;
      }
    }
    if (status != WARM_OK && status != WARM_TOO_BIG) {
      if (status == WARM_NO_MEMORY) s.stats.noMemory++; else s.stats.ioErrors++;
      s.stats.lastError = status;
      s.stats.lastErrorYmd = warmDnToYmd(st);
      failed = true;
      char path[96];
      if (!warmMonthPath(path, sizeof(path), cfg.dir, year, month, "")) path[0] = '\0';
      if (++s.failedTicks < WARM_MAX_FAILED_TICKS) {
        warmLogf(h, "warn", "Warm tier: %s not written (%s); will retry next hour", path, warmStatusName(status));
        break;
      }
      // Still failing: skip the rest of this month so later months are rolled
      en = (monthEnd < yDn) ? monthEnd : yDn;
      s.failedTicks = 0;
      s.stats.skippedDays += (uint32_t)(en - st + 1);
      warmLogf(h, "error", "Warm tier: %s not written (%s) in %u tries; skipped %lu-%lu until restart", path,
               warmStatusName(status), (unsigned)WARM_MAX_FAILED_TICKS, (unsigned long)warmDnToYmd(st),
               (unsigned long)warmDnToYmd(en));
    } else if (status == WARM_TOO_BIG) {  // already logged by the merge; move on
      s.stats.tooBig++;
      s.stats.skippedDays += (uint32_t)(en - st + 1);
      s.stats.lastError = status;
      s.stats.lastErrorYmd = warmDnToYmd(st);
    } else if (res.wrote) {
      s.stats.writes++;
      sum.writes++;
    } else if (n > 0) {
      s.stats.noChange++;
    }
    if (sum.batches == 0) sum.firstYmd = warmDnToYmd(st);
    sum.lastYmd = warmDnToYmd(en);
    sum.batches++;
    if (h.yield) h.yield();

    s.nextDn = en + 1;
    if (!haveCursor || en > cursorDn) {
      cursorDn = en;
      haveCursor = true;
      s.cursorYmd = warmDnToYmd(en);
    }
  }
  WARM_FREE(rows);
  if (!failed) s.failedTicks = 0;

  const uint32_t elapsed = h.nowMs ? (uint32_t)(h.nowMs() - t0) : 0;
  s.stats.lastTickMs = elapsed;
  if (elapsed > s.stats.maxTickMs) s.stats.maxTickMs = elapsed;
  return sum;
}

// Called for every snapshot added to the hot tier, and for every alarm logged
// with a known time (S-D03). A snapshot or alarm for a day that was already
// rolled up marks it for a re-roll on the next tick. O(1); the mark is lost on
// reboot, which is harmless because the first tick after boot re-checks the
// whole window anyway.
static inline void warmNoteSnapshot(WarmRollupState &s, double ts) {
  if (s.nextDn < 0) return;
  const int32_t dn = warmEpochToDn(ts);
  if (dn < s.nextDn && dn < s.dirtyDn) {
    s.dirtyDn = dn;
    s.stats.lateMarks++;
  }
}

// ============================================================================
// Readers: month aggregates used by the sketch's warm-tier consumers
// ============================================================================

// One sensor over a month: min of mn, max of mx, sum of av, days
// (populateStatsFromDailySummary and /api/history/yoy)
struct WarmSensorStats {
  const char *uid;
  uint8_t k;
  float minLevel, maxLevel, sumAvg;
  uint16_t days;
};

static inline void warmSensorStatsInit(WarmSensorStats &s, const char *uid, uint8_t k) {
  s.uid = uid ? uid : "";
  s.k = k;
  s.minLevel = 999999.0f;
  s.maxLevel = -999999.0f;
  s.sumAvg = 0.0f;
  s.days = 0;
}

static inline bool warmSensorStatsVisitor(JsonObjectConst entry, const char *raw, size_t rawLen, void *ctx) {
  (void)raw;
  (void)rawLen;
  WarmSensorStats &s = *static_cast<WarmSensorStats *>(ctx);
  const char *uid = entry["c"] | "";
  uint8_t sensorIdx = entry["k"] | 0;
  if (strcmp(uid, s.uid) != 0 || sensorIdx != s.k) return true;

  float mn = entry["mn"] | 0.0f;
  float mx = entry["mx"] | 0.0f;
  float av = entry["av"] | 0.0f;
  if (mn < s.minLevel) s.minLevel = mn;
  if (mx > s.maxLevel) s.maxLevel = mx;
  s.sumAvg += av;
  s.days++;
  return true;
}

// Every sensor over a month (monthly FTP archive fallback when the hot tier
// has no data for the month)
struct WarmMonthSensor {
  char clientUid[48];
  uint8_t sensorIndex;
  float minL, maxL, sumL, sumV;
  uint16_t count, voltCount;
};

struct WarmMonthSummary {
  WarmMonthSensor *sensors;
  uint8_t cap, count;
};

static inline bool warmMonthSummaryVisitor(JsonObjectConst de, const char *raw, size_t rawLen, void *ctx) {
  (void)raw;
  (void)rawLen;
  WarmMonthSummary &sum = *static_cast<WarmMonthSummary *>(ctx);
  const char *wUid = de["c"] | "";
  uint8_t wIdx = de["k"] | 0;
  float dMin = de["mn"] | 999999.0f;
  float dMax = de["mx"] | -999999.0f;
  float dAvg = de["av"] | 0.0f;
  float dVt  = de["vt"] | 0.0f;
  uint16_t dN = de["n"] | (uint16_t)1;

  // Find or create the sensor's slot
  WarmMonthSensor *ws = nullptr;
  for (uint8_t w = 0; w < sum.count; ++w) {
    if (strcmp(sum.sensors[w].clientUid, wUid) == 0 && sum.sensors[w].sensorIndex == wIdx) { ws = &sum.sensors[w]; break; }
  }
  if (!ws && sum.count < sum.cap) {
    ws = &sum.sensors[sum.count++];
    warmCopyStr(ws->clientUid, sizeof(ws->clientUid), wUid);
    ws->sensorIndex = wIdx;
    ws->minL = 999999.0f; ws->maxL = -999999.0f;
    ws->sumL = 0.0f; ws->sumV = 0.0f;
    ws->count = 0; ws->voltCount = 0;
  }
  if (!ws) return true;

  if (dMin < ws->minL) ws->minL = dMin;
  if (dMax > ws->maxL) ws->maxL = dMax;
  ws->sumL += dAvg * dN;
  ws->count += dN;
  if (dVt > 0.0f) { ws->sumV += dVt * dN; ws->voltCount += dN; }
  return true;
}

// ============================================================================
// Archived-client manifest (/fs/archived_clients.json)
// ============================================================================

enum WarmManifestStatus : uint8_t {
  WARM_MAN_OK = 0,
  WARM_MAN_ABSENT,       // no file: doc = {"archives":[]}
  WARM_MAN_SALVAGED,     // unreadable file: doc holds the entries before the damage
  WARM_MAN_IO_ERROR,
  WARM_MAN_NO_MEMORY,
  WARM_MAN_UNREADABLE    // unreadable and salvage not allowed
};

static inline const char *warmManifestStatusName(uint8_t status) {
  switch (status) {
    case WARM_MAN_OK: return "ok";
    case WARM_MAN_ABSENT: return "absent";
    case WARM_MAN_SALVAGED: return "salvaged";
    case WARM_MAN_IO_ERROR: return "io_error";
    case WARM_MAN_NO_MEMORY: return "no_memory";
    case WARM_MAN_UNREADABLE: return "unreadable";
    default: return "unknown";
  }
}

// Rebuilds doc from the complete entries at the start of an unreadable
// manifest. Every writer of this file produces compact JSON whose only key is
// "archives", so the entries start right after {"archives":[ .
static inline WarmManifestStatus warmManifestSalvage(FILE *f, uint32_t size, uint32_t maxBytes, JsonDocument &doc,
                                                     uint32_t *salvaged, const WarmHooks &h) {
  static const char prefix[] = "{\"archives\":[";
  doc.clear();
  JsonArray arr = doc["archives"].to<JsonArray>();
  if (doc.overflowed()) return WARM_MAN_NO_MEMORY;
  if (size == 0) return WARM_MAN_SALVAGED;
  if (WARM_FSEEK(f, 0, SEEK_SET) != 0) return WARM_MAN_IO_ERROR;

  WarmFileReader r;
  r.begin(f, size, (size < maxBytes) ? size : maxBytes);
  for (size_t i = 0; i < sizeof(prefix) - 1; ++i) {
    const int c = r.read();
    if (c < 0 && r.eofKind() == WARM_ELEM_IO) return WARM_MAN_IO_ERROR;
    if (c != (uint8_t)prefix[i]) return WARM_MAN_SALVAGED;  // not ours: nothing to salvage
  }

  char elem[WARM_ELEM_MAX + 1];
  JsonDocument entry = warmMakeDoc(h);
  bool first = true;
  uint32_t n = 0;
  for (;;) {
    size_t len = 0;
    const WarmElem e = warmNextElement(r, first, elem, sizeof(elem), len);
    if (e == WARM_ELEM_IO) return WARM_MAN_IO_ERROR;
    if (e != WARM_ELEM_ROW) break;  // END, damage, or size cap
    const DeserializationError err = deserializeJson(entry, elem, len);
    if (err == DeserializationError::NoMemory) return WARM_MAN_NO_MEMORY;
    if (err || !entry.is<JsonObject>()) break;
    if (!arr.add(entry.as<JsonObjectConst>()) || doc.overflowed()) return WARM_MAN_NO_MEMORY;
    if (salvaged) *salvaged = ++n;
    if (h.yield && (n % WARM_YIELD_EVERY) == 0) h.yield();
  }
  return WARM_MAN_SALVAGED;
}

// Loads the manifest into doc without a text buffer. OK and ABSENT leave
// doc["archives"] an array; SALVAGED (allowSalvage only) leaves the entries
// that could be read. Never writes.
static inline WarmManifestStatus warmLoadManifest(const char *path, JsonDocument &doc, uint32_t maxBytes,
                                                  bool allowSalvage, uint32_t *salvaged, const WarmHooks &h) {
  if (salvaged) *salvaged = 0;
  const int exists = warmStat(path);
  if (exists < 0) return WARM_MAN_IO_ERROR;
  if (exists == 0) {
    doc.clear();
    doc["archives"].to<JsonArray>();
    return doc.overflowed() ? WARM_MAN_NO_MEMORY : WARM_MAN_ABSENT;
  }

  FILE *f = WARM_FOPEN(path, "rb");
  if (!f) return WARM_MAN_IO_ERROR;
  WarmManifestStatus st;
  const long size = warmFileSize(f);
  if (size < 0) {
    st = WARM_MAN_IO_ERROR;
  } else if (size == 0 || (uint32_t)size > maxBytes) {
    st = allowSalvage ? warmManifestSalvage(f, (uint32_t)size, maxBytes, doc, salvaged, h) : WARM_MAN_UNREADABLE;
  } else {
    WarmFileReader r;
    r.begin(f, (uint32_t)size, (uint32_t)size);
    const DeserializationError err = deserializeJson(doc, r);
    if (err == DeserializationError::NoMemory) {
      st = WARM_MAN_NO_MEMORY;
    } else if (err && r.ioFailed()) {
      st = WARM_MAN_IO_ERROR;
    } else if (err || !doc["archives"].is<JsonArray>()) {
      st = allowSalvage ? warmManifestSalvage(f, (uint32_t)size, maxBytes, doc, salvaged, h) : WARM_MAN_UNREADABLE;
    } else {
      st = WARM_MAN_OK;
    }
  }
  WARM_FCLOSE(f);
  if (st == WARM_MAN_UNREADABLE) doc.clear();
  return st;
}

// True when the manifest lists exactly this ftpFile: the only archive paths
// /api/history/archived?file= fetches from FTP.
static inline bool warmManifestHasFile(const JsonDocument &man, const char *ftpFile) {
  if (!ftpFile || !ftpFile[0]) return false;
  for (JsonObjectConst entry : man["archives"].as<JsonArrayConst>()) {
    const char *listed = entry["ftpFile"] | "";
    if (strcmp(listed, ftpFile) == 0) return true;
  }
  return false;
}

struct WarmManifestEntry {
  const char *clientUid, *site, *displayLabel, *ftpFile;
  double firstSeenEpoch, lastUpdateEpoch, archiveEpoch;
  uint8_t sensorCount;
};

struct WarmManifestAppendResult {
  WarmManifestStatus loaded;
  uint32_t salvaged;       // entries kept from an unreadable manifest
  uint16_t dropped;        // oldest entries dropped to stay within the limits
  bool quarantined;        // the unreadable manifest was moved to badPath
};

static inline const char *warmBaseName(const char *path) {
  const char *slash = path ? strrchr(path, '/') : nullptr;
  return slash ? slash + 1 : (path ? path : "");
}

// Adds (or replaces, by ftpFile) one entry and rewrites the manifest through
// .tmp + rename. Keeps at most maxEntries entries and under maxBytes by
// dropping the oldest (the archive files stay on FTP). An unreadable manifest
// is salvaged and moved to badPath, but only once its replacement is fully
// written. Returns OK, IO_ERROR or NO_MEMORY; on failure the manifest on disk
// is unchanged (see WarmAtomicWriter::commit for the one exception). Logs
// every failure and every dropped entry.
static inline WarmStatus warmManifestAppend(const char *path, const char *badPath, const WarmManifestEntry &e,
                                            uint16_t maxEntries, uint32_t maxBytes, const WarmHooks &h,
                                            WarmManifestAppendResult *out) {
  WarmManifestAppendResult local = {};
  WarmManifestAppendResult &res = out ? *out : local;
  res = WarmManifestAppendResult();
  const char *ftpFile = e.ftpFile ? e.ftpFile : "";

  // A commit() that could not put the moved-aside manifest back left the
  // complete new manifest in .tmp; finish that rename first, as
  // recoverOrphanedTmpFiles() would at boot, instead of overwriting it.
  char tmpPath[116];
  const int tn = snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
  if (tn > 0 && (size_t)tn < sizeof(tmpPath) && warmStat(path) == 0 && warmStat(tmpPath) == 1 &&
      WARM_RENAME(tmpPath, path) != 0) {
    warmLogf(h, "warn", "Archive manifest not updated (io_error); archive is on FTP at %s", ftpFile);
    return WARM_IO_ERROR;
  }

  JsonDocument man = warmMakeDoc(h);
  res.loaded = warmLoadManifest(path, man, maxBytes, true, &res.salvaged, h);
  if (res.loaded != WARM_MAN_OK && res.loaded != WARM_MAN_ABSENT && res.loaded != WARM_MAN_SALVAGED) {
    warmLogf(h, "warn", "Archive manifest not updated (%s); archive is on FTP at %s",
             warmManifestStatusName(res.loaded), ftpFile);
    return (res.loaded == WARM_MAN_NO_MEMORY) ? WARM_NO_MEMORY : WARM_IO_ERROR;
  }

  JsonArray entries = man["archives"].as<JsonArray>();
  for (size_t i = entries.size(); i > 0; --i) {  // a re-archive replaces its entry
    const char *existing = entries[i - 1]["ftpFile"] | "";
    if (strcmp(existing, ftpFile) == 0) entries.remove(i - 1);
  }
  JsonObject entry = entries.add<JsonObject>();
  entry["clientUid"] = e.clientUid;
  entry["site"] = e.site;
  entry["displayLabel"] = e.displayLabel;
  entry["firstSeenEpoch"] = e.firstSeenEpoch;
  entry["lastUpdateEpoch"] = e.lastUpdateEpoch;
  entry["archiveEpoch"] = e.archiveEpoch;
  entry["ftpFile"] = ftpFile;
  entry["sensorCount"] = e.sensorCount;
  if (man.overflowed()) {
    warmLogf(h, "warn", "Archive manifest not updated (no_memory); archive is on FTP at %s", ftpFile);
    return WARM_NO_MEMORY;
  }
  while (entries.size() > 1 && (entries.size() > maxEntries || measureJson(man) >= maxBytes)) {
    warmLogf(h, "warn", "Archive manifest full; dropped oldest entry %s (file stays on FTP)",
             entries[0]["ftpFile"] | "?");
    entries.remove(0);
    res.dropped++;
  }

  const bool moveAside = (res.loaded == WARM_MAN_SALVAGED);
  WarmAtomicWriter w;
  if (w.begin(path)) serializeJson(man, w);
  if (!w.commit(h, moveAside ? badPath : nullptr)) {
    warmLogf(h, "error", "Archive manifest write failed; archive is on FTP at %s", ftpFile);
    return WARM_IO_ERROR;
  }
  if (moveAside) {
    res.quarantined = true;
    warmLogf(h, "error", "Archive manifest unreadable; salvaged %lu entries, original saved as %s",
             (unsigned long)res.salvaged, warmBaseName(badPath));
  }
  return WARM_OK;
}

#endif  // TANKALARM_WARM_TIER_STORE_H
