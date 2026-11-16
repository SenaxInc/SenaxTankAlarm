// Host tests for TankAlarm-112025-Server-BluesOpta/EmailStaleness.h (late/stale readings in
// the daily email and alarm reminders).
//
// Build and run: make -C tests/host/email_staleness test
//
// Besides the helpers, a few sketch-text checks pin how sendDailyEmail() and
// checkAlarmReminders() use them, so a later edit cannot quietly drop the markers.

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>

#include "EmailStaleness.h"

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

#define CHECK_STR(actual, expected)                                                       \
  do {                                                                                    \
    ++gChecks;                                                                            \
    if (strcmp((actual), (expected)) != 0) {                                              \
      ++gFailures;                                                                        \
      printf("FAIL %s:%d: got \"%s\", expected \"%s\"\n", __FILE__, __LINE__, (actual),   \
             (expected));                                                                 \
    }                                                                                     \
  } while (0)

// The server's limits (LATE_READING_THRESHOLD_SECONDS, STALE_CLIENT_THRESHOLD_SECONDS)
static const double kLate = 108000.0;   // 30 h
static const double kStale = 176400.0;  // 49 h
static const double kNow = 1794830400.0;  // 2026-11-16 12:00:00 UTC

// ---------------------------------------------------------------------------------------------
// classifyReadingAge
// ---------------------------------------------------------------------------------------------
static void testClassify() {
  double age = -1.0;

  // Boundaries: the limits are inclusive (">="), like checkStaleClients()
  CHECK(classifyReadingAge(kNow, kNow - (kLate - 1.0), kLate, kStale, &age) == READING_AGE_FRESH);
  CHECK(age == kLate - 1.0);
  CHECK(classifyReadingAge(kNow, kNow - kLate, kLate, kStale, &age) == READING_AGE_LATE);
  CHECK(age == kLate);
  CHECK(classifyReadingAge(kNow, kNow - (kStale - 1.0), kLate, kStale, &age) == READING_AGE_LATE);
  CHECK(classifyReadingAge(kNow, kNow - kStale, kLate, kStale, &age) == READING_AGE_STALE);
  CHECK(age == kStale);

  // The Silas incident: last reading 73.2 h before the email
  CHECK(classifyReadingAge(kNow, kNow - 73.2 * 3600.0, kLate, kStale, &age) == READING_AGE_STALE);
  CHECK(fabs(age / 3600.0 - 73.2) < 1e-9);

  // A healthy client: newest reading a few hours old
  CHECK(classifyReadingAge(kNow, kNow - 7.5 * 3600.0, kLate, kStale, nullptr) == READING_AGE_FRESH);

  // No clock, or no reading time: unknown, age 0
  age = -1.0;
  CHECK(classifyReadingAge(0.0, kNow, kLate, kStale, &age) == READING_AGE_UNKNOWN);
  CHECK(age == 0.0);
  age = -1.0;
  CHECK(classifyReadingAge(kNow, 0.0, kLate, kStale, &age) == READING_AGE_UNKNOWN);
  CHECK(age == 0.0);
  CHECK(classifyReadingAge(kNow, -5.0, kLate, kStale, &age) == READING_AGE_UNKNOWN);
  CHECK(classifyReadingAge(NAN, kNow, kLate, kStale, &age) == READING_AGE_UNKNOWN);
  CHECK(classifyReadingAge(kNow, NAN, kLate, kStale, &age) == READING_AGE_UNKNOWN);

  // A reading stamped ahead of the server clock is age 0, fresh
  age = -1.0;
  CHECK(classifyReadingAge(kNow, kNow + 90.0, kLate, kStale, &age) == READING_AGE_FRESH);
  CHECK(age == 0.0);

  // A limit of 0 is off
  CHECK(classifyReadingAge(kNow, kNow - 400000.0, 0.0, kStale, nullptr) == READING_AGE_STALE);
  CHECK(classifyReadingAge(kNow, kNow - 400000.0, kLate, 0.0, nullptr) == READING_AGE_LATE);
  CHECK(classifyReadingAge(kNow, kNow - 400000.0, 0.0, 0.0, nullptr) == READING_AGE_FRESH);
}

// ---------------------------------------------------------------------------------------------
// epochToCivil / formatEpochStamp / formatEpochDate
// ---------------------------------------------------------------------------------------------
static void checkCivilAgainstGmtime(int64_t secs) {
  time_t t = (time_t)secs;
  struct tm tmv;
  if (!gmtime_r(&t, &tmv)) return;
  CivilTime c;
  epochToCivil(secs, &c);
  ++gChecks;
  if (c.year != tmv.tm_year + 1900 || c.month != tmv.tm_mon + 1 || c.day != tmv.tm_mday ||
      c.hour != tmv.tm_hour || c.minute != tmv.tm_min) {
    ++gFailures;
    printf("FAIL epochToCivil(%lld): %ld-%u-%u %u:%u, gmtime %d-%d-%d %d:%d\n", (long long)secs,
           (long)c.year, c.month, c.day, c.hour, c.minute, tmv.tm_year + 1900, tmv.tm_mon + 1,
           tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
  }
}

static void testCivil() {
  CivilTime c;
  epochToCivil(0, &c);
  CHECK(c.year == 1970 && c.month == 1 && c.day == 1 && c.hour == 0 && c.minute == 0);
  epochToCivil(-1, &c);  // one second before the epoch
  CHECK(c.year == 1969 && c.month == 12 && c.day == 31 && c.hour == 23 && c.minute == 59);
  epochToCivil(951782400, &c);  // 2000-02-29 00:00 (leap day, century divisible by 400)
  CHECK(c.year == 2000 && c.month == 2 && c.day == 29);
  epochToCivil(4107542400LL, &c);  // 2100-03-01 00:00 (2100 is not a leap year)
  CHECK(c.year == 2100 && c.month == 3 && c.day == 1);

  // Every 7 h 13 min over 1970-2106, against the C library
  for (int64_t s = 0; s < 4294967295LL; s += 25980) checkCivilAgainstGmtime(s);
  // Around every month end of 2024-2028
  for (int64_t s = 1704067200LL; s < 1830297600LL; s += 86400) {
    checkCivilAgainstGmtime(s - 1);
    checkCivilAgainstGmtime(s);
  }
}

static void testStamp() {
  char buf[32];

  // US Central standard time (-360), DST off: 2026-11-16 12:00 UTC -> 06:00 local
  CHECK(localOffsetSeconds(-360, false) == -21600);
  CHECK(localOffsetSeconds(-360, true) == -18000);
  CHECK(localOffsetSeconds(0, false) == 0);
  CHECK(localOffsetSeconds(330, false) == 19800);  // India +5:30

  formatEpochStamp(kNow, localOffsetSeconds(-360, false), false, buf, sizeof(buf));
  CHECK_STR(buf, "2026-11-16 06:00");
  formatEpochStamp(kNow, localOffsetSeconds(-360, true), false, buf, sizeof(buf));
  CHECK_STR(buf, "2026-11-16 07:00");
  // No time zone configured: say it is UTC
  formatEpochStamp(kNow, 0, true, buf, sizeof(buf));
  CHECK_STR(buf, "2026-11-16 12:00 UTC");
  // The offset can move the date back a day
  formatEpochStamp(kNow - 9.0 * 3600.0, localOffsetSeconds(-360, false), false, buf, sizeof(buf));
  CHECK_STR(buf, "2026-11-15 21:00");
  // Seconds are dropped, not rounded
  formatEpochStamp(kNow + 59.9, 0, false, buf, sizeof(buf));
  CHECK_STR(buf, "2026-11-16 12:00");
  // India +5:30
  formatEpochStamp(kNow, localOffsetSeconds(330, false), false, buf, sizeof(buf));
  CHECK_STR(buf, "2026-11-16 17:30");

  // No time: empty
  strcpy(buf, "junk");
  CHECK(formatEpochStamp(0.0, 0, true, buf, sizeof(buf)) == 0);
  CHECK_STR(buf, "");
  strcpy(buf, "junk");
  formatEpochStamp(NAN, 0, true, buf, sizeof(buf));
  CHECK_STR(buf, "");

  // A small buffer truncates and stays terminated
  char small[8];
  formatEpochStamp(kNow, 0, true, small, sizeof(small));
  CHECK_STR(small, "2026-11");
  CHECK(formatEpochStamp(kNow, 0, true, nullptr, 4) < 0);
  CHECK(formatEpochStamp(kNow, 0, true, small, 0) < 0);

  // Subject date: the local date, not the UTC date
  formatEpochDate(kNow, localOffsetSeconds(-360, false), buf, sizeof(buf));
  CHECK_STR(buf, "2026-11-16");
  formatEpochDate(1794787200.0 + 3600.0, localOffsetSeconds(-360, false), buf, sizeof(buf));  // 01:00 UTC
  CHECK_STR(buf, "2026-11-15");
  formatEpochDate(1794787200.0 - 3600.0, localOffsetSeconds(600, false), buf, sizeof(buf));  // 23:00 UTC, +10
  CHECK_STR(buf, "2026-11-16");
  formatEpochDate(0.0, 0, buf, sizeof(buf));
  CHECK_STR(buf, "");
}

// ---------------------------------------------------------------------------------------------
// SiteList / appendSubjectMarker
// ---------------------------------------------------------------------------------------------
static void testSiteList() {
  SiteList l;
  siteListInit(&l);
  CHECK_STR(siteListText(&l), "");

  siteListAdd(&l, "Silas");
  CHECK_STR(siteListText(&l), "Silas");
  siteListAdd(&l, "North");
  siteListAdd(&l, "Silas");  // two clients at one site show once
  siteListAdd(&l, "");       // ignored
  siteListAdd(&l, nullptr);  // ignored
  CHECK_STR(siteListText(&l), "Silas, North");
  CHECK(l.shown == 2);

  // A name that is a prefix of another is still added
  siteListAdd(&l, "Sil");
  CHECK_STR(siteListText(&l), "Silas, North, Sil");

  // Overflow: names that do not fit are counted, and the text says so
  siteListInit(&l);
  char name[32];
  for (int i = 0; i < 12; ++i) {
    snprintf(name, sizeof(name), "Site number %02d", i);  // 14 characters
    siteListAdd(&l, name);
  }
  // 5 names = 5 * 14 + 4 * 2 = 78 <= 80; the 6th would make 94
  CHECK(l.shown == 5);
  CHECK(l.more == 7);
  CHECK_STR(siteListText(&l),
            "Site number 00, Site number 01, Site number 02, Site number 03, Site number 04 +7 more");
  // Finished text is stable
  CHECK_STR(siteListText(&l),
            "Site number 00, Site number 01, Site number 02, Site number 03, Site number 04 +7 more");

  // A first name longer than the cap: the text still names the count
  siteListInit(&l);
  std::string longName(SITE_LIST_TEXT_CAP + 5, 'x');
  siteListAdd(&l, longName.c_str());
  CHECK_STR(siteListText(&l), "1 site");
  siteListInit(&l);
  siteListAdd(&l, longName.c_str());
  longName[0] = 'y';
  siteListAdd(&l, longName.c_str());
  CHECK_STR(siteListText(&l), "2 sites");

  // Exactly at the cap fits
  siteListInit(&l);
  std::string capName(SITE_LIST_TEXT_CAP, 'z');
  siteListAdd(&l, capName.c_str());
  CHECK(l.shown == 1 && l.more == 0);
  CHECK(strlen(siteListText(&l)) == SITE_LIST_TEXT_CAP);

  // 255+ names saturate, not wrap
  siteListInit(&l);
  siteListAdd(&l, capName.c_str());
  for (int i = 0; i < 300; ++i) {
    snprintf(name, sizeof(name), "S%d", i);
    siteListAdd(&l, name);
  }
  CHECK(l.more == 255);
  CHECK(strstr(siteListText(&l), " +255 more") != nullptr);
}

static void testSubjectMarker() {
  char subject[192];
  strcpy(subject, "Daily Sensor Summary - 2026-11-16");
  appendSubjectMarker(subject, sizeof(subject), "NOT REPORTING", "Silas");
  CHECK_STR(subject, "Daily Sensor Summary - 2026-11-16 - NOT REPORTING: Silas");
  appendSubjectMarker(subject, sizeof(subject), "NO NEW DATA", "North");
  CHECK_STR(subject, "Daily Sensor Summary - 2026-11-16 - NOT REPORTING: Silas - NO NEW DATA: North");

  // Nothing to add
  strcpy(subject, "Daily");
  appendSubjectMarker(subject, sizeof(subject), "NOT REPORTING", "");
  appendSubjectMarker(subject, sizeof(subject), "NOT REPORTING", nullptr);
  CHECK_STR(subject, "Daily");

  // Truncates at the buffer, terminated
  char small[24];
  strcpy(small, "Daily Sensor Summary");
  appendSubjectMarker(small, sizeof(small), "NOT REPORTING", "Silas");
  CHECK(strlen(small) == sizeof(small) - 1);
  CHECK_STR(small, "Daily Sensor Summary - ");

  // A full buffer stays as it is; an unterminated one is terminated
  char full[8];
  memcpy(full, "1234567", 8);
  appendSubjectMarker(full, sizeof(full), "X", "Y");
  CHECK_STR(full, "1234567");
  memset(full, 'a', sizeof(full));
  appendSubjectMarker(full, sizeof(full), "X", "Y");
  CHECK(full[sizeof(full) - 1] == '\0');
}

// ---------------------------------------------------------------------------------------------
// Sketch-text checks
// ---------------------------------------------------------------------------------------------
static std::string readFile(const char *path) {
  std::string out;
  FILE *f = fopen(path, "rb");
  if (!f) return out;
  char chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) out.append(chunk, n);
  fclose(f);
  return out;
}

// The text of the function that starts at `signature`, up to the next line that starts with "}".
static std::string functionBody(const std::string &sketch, const char *signature) {
  size_t start = sketch.find(signature);
  if (start == std::string::npos) return std::string();
  size_t end = sketch.find("\n}\n", start);
  if (end == std::string::npos) return std::string();
  return sketch.substr(start, end - start);
}

static bool contains(const std::string &text, const char *needle) {
  return text.find(needle) != std::string::npos;
}

static void testSketch() {
  const std::string sketch = readFile(SERVER_SKETCH_PATH);
  CHECK(!sketch.empty());
  if (sketch.empty()) return;

  CHECK(contains(sketch, "#include \"EmailStaleness.h\""));
  CHECK(contains(sketch, "#define LATE_READING_THRESHOLD_SECONDS 108000UL"));
  CHECK(contains(sketch, "#define STALE_CLIENT_THRESHOLD_SECONDS 176400UL"));

  const std::string daily = functionBody(sketch, "static void sendDailyEmail() {");
  CHECK(!daily.empty());
  // Per sensor: the reading's time, its age class, and its age and local time when old
  CHECK(contains(daily, "classifyReadingAge(now, gSensorRecords[i].lastUpdateEpoch,"));
  CHECK(contains(daily, "obj[\"u\"] = (uint32_t)gSensorRecords[i].lastUpdateEpoch;"));
  CHECK(contains(daily, "obj[\"stale\"] = true;"));
  CHECK(contains(daily, "obj[\"late\"] = true;"));
  CHECK(contains(daily, "obj[\"ageH\"] = roundTo(ageSec / 3600.0, 1);"));
  CHECK(contains(daily, "formatEpochStamp(gSensorRecords[i].lastUpdateEpoch, tzOffsetSec, markUtc,"));
  // Site summary and subject, gated on the Stale Readings Warning toggle (default on)
  CHECK(contains(daily, "bool staleWarning = true;"));
  CHECK(contains(daily, "staleWarning = fmtDoc[\"summaryStaleWarning\"] | true;"));
  CHECK(contains(daily, "appendSubjectMarker(subject, sizeof(subject), \"NOT REPORTING\", siteListText(&staleSites));"));
  CHECK(contains(daily, "doc[\"staleCount\"] = staleClientCount;"));
  CHECK(contains(daily, "doc[\"lateCount\"] = lateClientCount;"));
  // Same limits as the stale-client SMS
  CHECK(contains(daily, "(double)STALE_CLIENT_THRESHOLD_SECONDS"));
  CHECK(contains(daily, "(double)LATE_READING_THRESHOLD_SECONDS"));
  // The subject date is the local date
  CHECK(contains(daily, "formatEpochDate(now, tzOffsetSec, dateStr, sizeof(dateStr));"));
  CHECK(!contains(daily, "gmtime("));
  // An oversized report drops optional fields before giving up
  CHECK(contains(daily, "doc.remove(\"fmt\");"));
  CHECK(contains(daily, "s.remove(\"asOf\");"));
  // The renderers take "message" or "type" as the alarm shape: never in the daily body
  CHECK(!contains(daily, "doc[\"message\"]"));
  CHECK(!contains(daily, "doc[\"type\"]"));

  const std::string reminders = functionBody(sketch, "static void checkAlarmReminders() {");
  CHECK(!reminders.empty());
  CHECK(contains(reminders, "classifyReadingAge(now, rec.lastUpdateEpoch,"));
  CHECK(contains(reminders, "no new reading for"));
}

int main() {
  testClassify();
  testCivil();
  testStamp();
  testSiteList();
  testSubjectMarker();
  testSketch();
  printf("email_staleness: %lu checks, %lu failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
