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

#include <initializer_list>
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
static const double kMissed = 90000.0;  // MISSED_DAILY_REPORT_SECONDS, 25 h
static const double kHour = 3600.0;

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
// classifyReportedReadingAge / classifyClientAge
// ---------------------------------------------------------------------------------------------
// A default client: daily report at the email time minus 2 h, last sample up to 30 min before
// the report. The email is at kNow.
static void testReported() {
  double age = -1.0;
  const double report = kNow - 2.0 * kHour;          // today's report
  const double reading = report - 0.5 * kHour;       // its newest sample
  // Healthy: fresh, by age and by report
  CHECK(classifyReportedReadingAge(kNow, reading, report, kMissed, kLate, kStale, &age) ==
        READING_AGE_FRESH);
  CHECK(age == 2.5 * kHour);

  // Today's report missed: yesterday's report (26 h ago) is the last, and the reading (26.5 h)
  // is under the 30 h age limit. This is the first email of an outage; it must be marked.
  const double yReport = report - 24.0 * kHour;
  const double yReading = reading - 24.0 * kHour;
  CHECK(classifyReadingAge(kNow, yReading, kLate, kStale, nullptr) == READING_AGE_FRESH);
  CHECK(classifyReportedReadingAge(kNow, yReading, yReport, kMissed, kLate, kStale, &age) ==
        READING_AGE_LATE);
  CHECK(age == 26.5 * kHour);
  // The next email: 50.5 h, stale
  CHECK(classifyReportedReadingAge(kNow + 24 * kHour, yReading, yReport, kMissed, kLate, kStale,
                                   nullptr) == READING_AGE_STALE);
  // A T-8:00 schedule: 32.5 h at the first email after a missed report, 56.5 h at the next
  CHECK(classifyReportedReadingAge(kNow, kNow - 32.5 * kHour, kNow - 32 * kHour, kMissed, kLate,
                                   kStale, nullptr) == READING_AGE_LATE);

  // The missed-report limit is inclusive, like the others
  CHECK(classifyReportedReadingAge(kNow, kNow - kMissed, kNow - kMissed, kMissed, kLate, kStale,
                                   nullptr) == READING_AGE_LATE);
  CHECK(classifyReportedReadingAge(kNow, kNow - kMissed + 1.0, kNow - kMissed + 1.0, kMissed,
                                   kLate, kStale, nullptr) == READING_AGE_FRESH);

  // A reading newer than the last report (telemetry since) is new, report missed or not
  CHECK(classifyReportedReadingAge(kNow, kNow - kHour, yReport, kMissed, kLate, kStale, nullptr) ==
        READING_AGE_FRESH);
  // ... but within the slack of a report sent in parts it belongs to that report
  CHECK(classifyReportedReadingAge(kNow, yReport + 600.0, yReport, kMissed, kLate, kStale,
                                   nullptr) == READING_AGE_LATE);
  CHECK(classifyReportedReadingAge(kNow, yReport + 1801.0, yReport, kMissed, kLate, kStale,
                                   nullptr) == READING_AGE_FRESH);

  // T-0 schedule (report at the email minute, not yet in): yesterday's report is 24 h old, so
  // not missed; the reading is not marked until the age limit
  CHECK(classifyReportedReadingAge(kNow, kNow - 24.5 * kHour, kNow - 24.0 * kHour, kMissed, kLate,
                                   kStale, nullptr) == READING_AGE_FRESH);
  // Slow sampling on a long offset: reading 20 h old in a report 8 h old is fresh
  CHECK(classifyReportedReadingAge(kNow, kNow - 20 * kHour, kNow - 8 * kHour, kMissed, kLate,
                                   kStale, nullptr) == READING_AGE_FRESH);

  // No report seen (server restarted) or the rule off: the age limits alone
  CHECK(classifyReportedReadingAge(kNow, yReading, 0.0, kMissed, kLate, kStale, nullptr) ==
        READING_AGE_FRESH);
  CHECK(classifyReportedReadingAge(kNow, yReading, yReport, 0.0, kLate, kStale, nullptr) ==
        READING_AGE_FRESH);
  CHECK(classifyReportedReadingAge(kNow, kNow - kLate, 0.0, kMissed, kLate, kStale, nullptr) ==
        READING_AGE_LATE);
  // No clock or no reading time stays unknown
  CHECK(classifyReportedReadingAge(0.0, yReading, yReport, kMissed, kLate, kStale, &age) ==
        READING_AGE_UNKNOWN);
  CHECK(age == 0.0);
  CHECK(classifyReportedReadingAge(kNow, 0.0, yReport, kMissed, kLate, kStale, nullptr) ==
        READING_AGE_UNKNOWN);
  // A report stamped ahead of the server clock is not missed
  CHECK(classifyReportedReadingAge(kNow, kNow - kHour, kNow + 60.0, kMissed, kLate, kStale,
                                   nullptr) == READING_AGE_FRESH);

  // Client level. Silent for 60 h: not reporting.
  CHECK(classifyClientAge(kNow, kNow - 60 * kHour, kNow - 60 * kHour, kMissed, kLate, kStale) ==
        READING_AGE_STALE);
  CHECK(classifyClientAge(kNow, kNow - 60 * kHour, 0.0, kMissed, kLate, kStale) ==
        READING_AGE_STALE);
  // Still sending its daily report, but the newest reading is 60 h old (failed sensor, or
  // the first report after a battery outage): no new readings, not "not reporting"
  CHECK(classifyClientAge(kNow, kNow - 60 * kHour, report, kMissed, kLate, kStale) ==
        READING_AGE_LATE);
  CHECK(classifyClientAge(kNow, kNow - 60 * kHour, kNow - (kStale - 1.0), kMissed, kLate,
                          kStale) == READING_AGE_LATE);
  CHECK(classifyClientAge(kNow, kNow - 60 * kHour, kNow - kStale, kMissed, kLate, kStale) ==
        READING_AGE_STALE);
  // First missed report, healthy, unknown
  CHECK(classifyClientAge(kNow, yReading, yReport, kMissed, kLate, kStale) == READING_AGE_LATE);
  CHECK(classifyClientAge(kNow, reading, report, kMissed, kLate, kStale) == READING_AGE_FRESH);
  CHECK(classifyClientAge(kNow, 0.0, report, kMissed, kLate, kStale) == READING_AGE_UNKNOWN);
}

// ---------------------------------------------------------------------------------------------
// noteReadingEpoch
// ---------------------------------------------------------------------------------------------
static void testNoteReadingEpoch() {
  const double t = kNow - 74 * kHour;  // the last good reading
  const double arrival = kNow - 2 * kHour;
  // A note with "t" dates the reading by it, fresh or reused (a reused value carries the last
  // good reading's time)
  CHECK(noteReadingEpoch(t, arrival, true, false, false) == t);
  CHECK(noteReadingEpoch(t, arrival, true, true, false) == t);
  CHECK(noteReadingEpoch(t, arrival, false, true, false) == t);
  // An older client's alarm note: "t" is the send time, so it dates only a fresh value
  CHECK(noteReadingEpoch(arrival, arrival, false, false, true) == arrival);
  CHECK(noteReadingEpoch(arrival, arrival, false, true, true) == 0.0);
  // No "t": a 2.2.16+ client has had no reading since boot (a reboot while the sensor is
  // failed must not make its old value look new), and a flagged value is no reading
  CHECK(noteReadingEpoch(0.0, arrival, true, false, false) == 0.0);
  CHECK(noteReadingEpoch(0.0, arrival, true, true, false) == 0.0);
  CHECK(noteReadingEpoch(0.0, arrival, false, true, false) == 0.0);
  // ... and a client that never sends "t" is dated on arrival, as before
  CHECK(noteReadingEpoch(0.0, arrival, false, false, false) == arrival);
  CHECK(noteReadingEpoch(0.0, 0.0, false, false, false) == 0.0);
  CHECK(noteReadingEpoch(-5.0, arrival, false, false, false) == arrival);

  // The reboot case end to end: the sensor's reading time stays at the last good reading, so
  // the email keeps marking it stale
  double readingEpoch = t;
  const double fromReport = noteReadingEpoch(0.0, arrival, true, true, false);
  if (fromReport > 0.0) readingEpoch = fromReport;
  CHECK(classifyReportedReadingAge(kNow, readingEpoch, arrival, kMissed, kLate, kStale, nullptr) ==
        READING_AGE_STALE);
}

// ---------------------------------------------------------------------------------------------
// SiteList / appendSubjectMarker
// ---------------------------------------------------------------------------------------------
static std::string fmtList(const SiteList &l, size_t cap = SITE_LIST_TEXT_SIZE) {
  std::string buf(cap, '#');
  const size_t n = siteListFormat(&l, &buf[0], cap);
  CHECK(n == strlen(buf.c_str()));
  CHECK(n < cap);
  return std::string(buf.c_str());
}

static void testSiteList() {
  SiteList l;
  siteListInit(&l);
  CHECK(fmtList(l) == "");
  CHECK(siteListCount(&l) == 0);

  CHECK(siteListAdd(&l, "Silas"));
  CHECK(fmtList(l) == "Silas");
  CHECK(siteListAdd(&l, "North"));
  CHECK(!siteListAdd(&l, "Silas"));  // two clients at one site show and count once
  CHECK(!siteListAdd(&l, ""));       // ignored
  CHECK(!siteListAdd(&l, nullptr));  // ignored
  CHECK(fmtList(l) == "Silas, North");
  CHECK(siteListCount(&l) == 2);
  CHECK(siteListHas(&l, "Silas") && siteListHas(&l, "North"));
  CHECK(!siteListHas(&l, "Sil") && !siteListHas(&l, "") && !siteListHas(&l, nullptr));

  // A name that is a prefix of another is still added; a name with ", " is one name
  CHECK(siteListAdd(&l, "Sil"));
  CHECK(siteListAdd(&l, "Smith, North"));
  CHECK(!siteListAdd(&l, "Smith, North"));
  CHECK(fmtList(l) == "Silas, North, Sil, Smith, North");
  CHECK(siteListCount(&l) == 4);

  // Equal names at different addresses are one name (the server's names live in records)
  char copy[8];
  strcpy(copy, "Silas");
  CHECK(!siteListAdd(&l, copy));

  // Names that do not fit the text are still known, so a repeat is never counted twice
  siteListInit(&l);
  char names[12][32];
  for (int i = 0; i < 12; ++i) {
    snprintf(names[i], sizeof(names[i]), "Site number %02d", i);  // 14 characters
    CHECK(siteListAdd(&l, names[i]));
  }
  for (int i = 0; i < 12; ++i) {  // a second client at each site
    char again[32];
    snprintf(again, sizeof(again), "Site number %02d", i);
    CHECK(!siteListAdd(&l, again));
    CHECK(siteListHas(&l, again));
  }
  CHECK(siteListCount(&l) == 12);
  // In the payload's 96 bytes: 5 names = 78, with " +7 more" 86; a 6th makes 94 + 8
  const char *five = "Site number 00, Site number 01, Site number 02, Site number 03, Site number 04";
  CHECK(fmtList(l) == std::string(five) + " +7 more");
  CHECK(fmtList(l) == std::string(five) + " +7 more");  // formatting does not change the list

  // A smaller buffer cuts at a name, never inside one
  CHECK(fmtList(l, 40) == "Site number 00, Site number 01 +10 more");
  CHECK(fmtList(l, 39) == "Site number 00 +11 more");
  CHECK(fmtList(l, 24) == "Site number 00 +11 more");
  CHECK(fmtList(l, 23) == "12 sites");
  CHECK(fmtList(l, 9) == "12 sites");
  CHECK(fmtList(l, 8) == "");
  CHECK(fmtList(l, 1) == "");
  CHECK(siteListFormat(&l, nullptr, 10) == 0);

  // The tail can vanish at the last name: all names fit where all but one with " +1 more"
  // would not
  siteListInit(&l);
  CHECK(siteListAdd(&l, "Alpha"));
  CHECK(siteListAdd(&l, "B"));
  CHECK(fmtList(l, strlen("Alpha, B") + 1) == "Alpha, B");
  CHECK(fmtList(l, strlen("Alpha, B")) == "2 sites");

  // The reviewer's case: 19 distinct 31-character names, each at two clients. Every name is
  // kept, so the count is 19 and the nineteenth is still found.
  siteListInit(&l);
  static char longNames[19][40];
  for (int i = 0; i < 19; ++i) {
    snprintf(longNames[i], sizeof(longNames[i]), "Thirty-one-char site name #%04u", (unsigned)i);
    CHECK(strlen(longNames[i]) == 31);
  }
  for (int pass = 0; pass < 2; ++pass) {
    for (int i = 0; i < 19; ++i) CHECK(siteListAdd(&l, longNames[i]) == (pass == 0));
  }
  CHECK(siteListCount(&l) == 19);
  CHECK(siteListHas(&l, longNames[18]));
  CHECK(fmtList(l) == std::string(longNames[0]) + ", " + longNames[1] + " +17 more");

  // A first name longer than the space: the text still names the count
  siteListInit(&l);
  std::string longName(SITE_LIST_TEXT_SIZE + 5, 'x');
  CHECK(siteListAdd(&l, longName.c_str()));
  CHECK(!siteListAdd(&l, longName.c_str()));
  CHECK(fmtList(l) == "1 site");
  std::string longName2(SITE_LIST_TEXT_SIZE + 5, 'y');
  CHECK(siteListAdd(&l, longName2.c_str()));
  CHECK(fmtList(l) == "2 sites");

  // Exactly filling the space fits
  siteListInit(&l);
  std::string capName(SITE_LIST_TEXT_SIZE - 1, 'z');
  CHECK(siteListAdd(&l, capName.c_str()));
  CHECK(fmtList(l) == capName);
  CHECK(siteListAdd(&l, "B"));
  CHECK(fmtList(l) == "2 sites");

  // Every client of a full registry is kept; past SITE_LIST_MAX_NAMES names are only counted
  siteListInit(&l);
  static char many[300][16];
  for (int i = 0; i < 300; ++i) {
    snprintf(many[i], sizeof(many[i]), "S%u", (unsigned)i);
    CHECK(siteListAdd(&l, many[i]));
  }
  CHECK(l.count == SITE_LIST_MAX_NAMES);
  CHECK(l.extra == 300 - SITE_LIST_MAX_NAMES);
  CHECK(siteListCount(&l) == 300);
  CHECK(siteListHas(&l, many[SITE_LIST_MAX_NAMES - 1]));
  CHECK(!siteListAdd(&l, many[SITE_LIST_MAX_NAMES - 1]));
  CHECK(fmtList(l).size() < SITE_LIST_TEXT_SIZE);
  CHECK(fmtList(l).find(" more") != std::string::npos);
}

static SiteList listOf(std::initializer_list<const char *> names) {
  SiteList l;
  siteListInit(&l);
  for (const char *n : names) siteListAdd(&l, n);
  return l;
}

static void testSubjectMarker() {
  const SiteList silas = listOf({"Silas"});
  const SiteList north = listOf({"North"});
  const SiteList none = listOf({});
  char subject[192];
  strcpy(subject, "Daily Sensor Summary - 2026-11-16");
  appendSubjectMarker(subject, sizeof(subject), 0, "NOT REPORTING", &silas);
  CHECK_STR(subject, "Daily Sensor Summary - 2026-11-16 - NOT REPORTING: Silas");
  appendSubjectMarker(subject, sizeof(subject), 0, "NO NEW READINGS", &north);
  CHECK_STR(subject,
            "Daily Sensor Summary - 2026-11-16 - NOT REPORTING: Silas - NO NEW READINGS: North");

  // Nothing to add
  strcpy(subject, "Daily");
  appendSubjectMarker(subject, sizeof(subject), 0, "NOT REPORTING", &none);
  appendSubjectMarker(subject, sizeof(subject), 0, "NOT REPORTING", nullptr);
  appendSubjectMarker(subject, sizeof(subject), 0, nullptr, &silas);
  CHECK_STR(subject, "Daily");
  CHECK(subjectMarkerMinLen("NO NEW READINGS", &none) == 0);
  CHECK(subjectMarkerMinLen("NO NEW READINGS", &north) == strlen(" - NO NEW READINGS: 1 site"));

  // Two long lists: the first leaves room for the second's count, and neither is cut inside
  // a name (the case a fleet-wide outage produces)
  const SiteList stale = listOf({"Silas Ranch Wellhead", "North Pasture Tank Battery", "Cox Lease",
                                 "Bravo", "Echo Ridge", "Foxtrot"});
  const SiteList late = listOf({"Alpha Unit 4 Battery", "Charlie Saltwater Battery",
                                "Delta Tank Farm", "Golf", "Hotel"});
  strcpy(subject, "Daily Tank Summary - 2026-11-16");
  appendSubjectMarkers(subject, sizeof(subject), "NOT REPORTING", &stale, "NO NEW READINGS", &late);
  CHECK_STR(subject,
            "Daily Tank Summary - 2026-11-16 - NOT REPORTING: Silas Ranch Wellhead, North Pasture "
            "Tank Battery, Cox Lease, Bravo, Echo Ridge, Foxtrot - NO NEW READINGS: Alpha Unit 4 "
            "Battery +4 more");
  CHECK(strlen(subject) < sizeof(subject));

  // A longer template: the first list leaves room for the second's count
  std::string tmpl(100, 'T');
  strcpy(subject, tmpl.c_str());
  appendSubjectMarkers(subject, sizeof(subject), "NOT REPORTING", &stale, "NO NEW READINGS", &late);
  CHECK_STR(subject, (tmpl + " - NOT REPORTING: Silas Ranch Wellhead +5 more - NO NEW READINGS: "
                             "5 sites").c_str());
  // Longer still: both as counts
  tmpl.assign(130, 'T');
  strcpy(subject, tmpl.c_str());
  appendSubjectMarkers(subject, sizeof(subject), "NOT REPORTING", &stale, "NO NEW READINGS", &late);
  CHECK_STR(subject, (tmpl + " - NOT REPORTING: 6 sites - NO NEW READINGS: 5 sites").c_str());
  // No room for both counts: the first list wins
  tmpl.assign(150, 'T');
  strcpy(subject, tmpl.c_str());
  appendSubjectMarkers(subject, sizeof(subject), "NOT REPORTING", &stale, "NO NEW READINGS", &late);
  CHECK_STR(subject, (tmpl + " - NOT REPORTING: 6 sites").c_str());
  // Only the second list
  strcpy(subject, "Daily");
  appendSubjectMarkers(subject, sizeof(subject), "NOT REPORTING", &none, "NO NEW READINGS", &north);
  CHECK_STR(subject, "Daily - NO NEW READINGS: North");

  // No room: nothing is added, not a fragment
  char small[24];
  strcpy(small, "Daily Sensor Summary");
  appendSubjectMarker(small, sizeof(small), 0, "NOT REPORTING", &silas);
  CHECK_STR(small, "Daily Sensor Summary");
  char exact[64];
  strcpy(exact, "Daily");
  appendSubjectMarker(exact, strlen("Daily - NOT REPORTING: Silas") + 1, 0, "NOT REPORTING", &silas);
  CHECK_STR(exact, "Daily - NOT REPORTING: Silas");
  strcpy(exact, "Daily");
  appendSubjectMarker(exact, strlen("Daily - NOT REPORTING: Silas"), 0, "NOT REPORTING", &silas);
  CHECK_STR(exact, "Daily");
  // A long name that does not fit gives way to the count
  const SiteList ranch = listOf({"Silas Ranch Wellhead"});
  strcpy(exact, "Daily");
  appendSubjectMarker(exact, strlen("Daily - NOT REPORTING: 1 site") + 1, 0, "NOT REPORTING", &ranch);
  CHECK_STR(exact, "Daily - NOT REPORTING: 1 site");
  strcpy(exact, "Daily");
  appendSubjectMarker(exact, strlen("Daily - NOT REPORTING: 1 site"), 0, "NOT REPORTING", &ranch);
  CHECK_STR(exact, "Daily");
  // The reserve is kept
  strcpy(exact, "Daily");
  appendSubjectMarker(exact, sizeof(exact), sizeof(exact) - 6, "NOT REPORTING", &silas);
  CHECK_STR(exact, "Daily");

  // A full buffer stays as it is; an unterminated one is terminated
  char full[8];
  memcpy(full, "1234567", 8);
  appendSubjectMarker(full, sizeof(full), 0, "X", &silas);
  CHECK_STR(full, "1234567");
  memset(full, 'a', sizeof(full));
  appendSubjectMarker(full, sizeof(full), 0, "X", &silas);
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

// The text from `from` to the first `to` after it, or "" when either is missing.
static std::string between(const std::string &text, const char *from, const char *to) {
  const size_t a = text.find(from);
  if (a == std::string::npos) return std::string();
  const size_t b = text.find(to, a);
  if (b == std::string::npos) return std::string();
  return text.substr(a, b - a);
}

static void testSketch() {
  const std::string sketch = readFile(SERVER_SKETCH_PATH);
  CHECK(!sketch.empty());
  if (sketch.empty()) return;

  CHECK(contains(sketch, "#include \"EmailStaleness.h\""));
  CHECK(contains(sketch, "#define MISSED_DAILY_REPORT_SECONDS 90000UL"));
  CHECK(contains(sketch, "#define LATE_READING_THRESHOLD_SECONDS 108000UL"));
  CHECK(contains(sketch, "#define STALE_CLIENT_THRESHOLD_SECONDS 176400UL"));

  // The shared rule: the client's last daily report time with the server's limits
  const std::string last = functionBody(sketch, "static double lastDailyReportEpoch(const char *clientUid) {");
  CHECK(contains(last, "return meta ? meta->dailyReportEpoch : 0.0;"));
  const std::string sensorAge = functionBody(sketch, "static ReadingAge sensorReadingAge(");
  CHECK(contains(sensorAge, "classifyReportedReadingAge(now, sensorReadingEpoch(rec), lastDailyReportEpoch(rec.clientUid),"));
  CHECK(contains(sensorAge, "(double)MISSED_DAILY_REPORT_SECONDS"));
  CHECK(contains(sensorAge, "(double)LATE_READING_THRESHOLD_SECONDS"));
  CHECK(contains(sensorAge, "(double)STALE_CLIENT_THRESHOLD_SECONDS"));
  const std::string clientAge = functionBody(sketch, "static ReadingAge dailyEmailClientAge(");
  CHECK(contains(clientAge, "return READING_AGE_UNKNOWN;"));  // each client once
  CHECK(contains(clientAge, "classifyClientAge(now, newest, lastDailyReportEpoch(uid),"));
  CHECK(contains(clientAge, "newest = sensorReadingEpoch(gSensorRecords[j]);"));
  CHECK(!contains(clientAge, "lastUpdateEpoch"));

  // The reading's time: the client's acquisition "t", which a reused or failed value does not
  // advance, kept across restarts; the last note's time only until the server knows it
  const std::string readingTime = functionBody(sketch, "static double sensorReadingEpoch(const SensorRecord &rec) {");
  CHECK(contains(readingTime, "return (rec.readingEpoch > 0.0) ? rec.readingEpoch : rec.lastUpdateEpoch;"));
  const std::string telemetry = functionBody(sketch, "static void handleTelemetry(JsonDocument &doc, double epoch) {");
  CHECK(contains(telemetry, "double readingT = noteReadingEpoch(doc[\"t\"] | 0.0, now, exactT, flagged, false);"));
  CHECK(contains(telemetry, "if (strcmp(doc[\"r\"] | \"\", \"ondemand\") == 0 && !exactT) readingT = 0.0;"));
  CHECK(contains(telemetry, "if (readingT > 0.0) rec->readingEpoch = readingT;"));
  const std::string alarm = functionBody(sketch, "static void handleAlarm(JsonDocument &doc, double epoch) {");
  CHECK(contains(alarm, "if (!doc[\"ot\"].isNull() || !doc[\"lvl\"].isNull() || !doc[\"ma\"].isNull()) {"));
  CHECK(contains(alarm, "noteReadingEpoch(doc[\"t\"] | 0.0, rec->lastUpdateEpoch, exactT, flagged, !exactT);"));
  CHECK(contains(alarm, "if (readingT > 0.0) rec->readingEpoch = readingT;"));
  const std::string dailyIn = functionBody(sketch, "static void handleDaily(JsonDocument &doc, double epoch) {");
  CHECK(contains(dailyIn, "noteReadingEpoch(sensorEpoch, now, exactT,"));
  CHECK(contains(dailyIn, "dailyFaulted || !t[\"fault\"].isNull(), false);"));
  CHECK(!contains(dailyIn, "rec->readingEpoch = now;"));
  // Every note that sets readingEpoch goes through noteReadingEpoch(), except the unload
  CHECK(contains(sketch, "const bool exactT = compareFirmwareVersions(doc[\"fv\"] | \"\", CLIENT_EXACT_T_SINCE) >= 0;"));
  CHECK(contains(sketch, "if (rec.readingEpoch > 0.0) obj[\"re\"] = (uint32_t)rec.readingEpoch;"));
  CHECK(contains(sketch, "rec.readingEpoch = obj[\"re\"] | 0.0;"));
  CHECK(contains(sketch, "existing->readingEpoch = obj[\"re\"] | 0.0;"));
  // Every client's name fits the site lists
  CHECK(contains(sketch, "static_assert(SITE_LIST_MAX_NAMES >= MAX_SENSOR_RECORDS,"));
  CHECK(contains(clientAge, "(double)MISSED_DAILY_REPORT_SECONDS"));
  CHECK(contains(clientAge, "(double)STALE_CLIENT_THRESHOLD_SECONDS"));

  const std::string daily = functionBody(sketch, "static void sendDailyEmail() {");
  CHECK(!daily.empty());
  // Per sensor: the reading's time, its age class, and its age and local time when old
  CHECK(contains(daily, "ReadingAge age = sensorReadingAge(gSensorRecords[i], now, &ageSec);"));
  CHECK(!contains(daily, "classifyReadingAge("));  // never the age limits alone
  CHECK(contains(daily, "obj[\"u\"] = (uint32_t)sensorReadingEpoch(gSensorRecords[i]);"));
  CHECK(contains(daily, "obj[\"stale\"] = true;"));
  CHECK(contains(daily, "obj[\"late\"] = true;"));
  CHECK(contains(daily, "obj[\"ageH\"] = roundTo(ageSec / 3600.0, 1);"));
  CHECK(contains(daily, "formatEpochStamp(sensorReadingEpoch(gSensorRecords[i]), tzOffsetSec, markUtc,"));
  CHECK(!contains(daily, "lastUpdateEpoch"));  // the reading's time, not the last note's
  // Site lists: each client once, a site in the worse list only, counts of distinct names
  CHECK(contains(daily, "static SiteList staleSites;"));
  CHECK(contains(daily, "siteListInit(&staleSites);"));
  CHECK(contains(daily, "if (dailyEmailClientAge(i, now, &name) != want) continue;"));
  CHECK(contains(daily, "} else if (!siteListHas(&staleSites, name)) {"));
  CHECK(contains(daily, "doc[\"staleCount\"] = siteListCount(&staleSites);"));
  CHECK(contains(daily, "doc[\"lateCount\"] = siteListCount(&lateSites);"));
  CHECK(!contains(daily, "doc[\"lateH\"]"));
  // Subject and summary gated on the Stale Readings Warning toggle (default on); the first
  // list leaves room for the second
  CHECK(contains(daily, "bool staleWarning = true;"));
  CHECK(contains(daily, "staleWarning = fmtDoc[\"summaryStaleWarning\"] | true;"));
  CHECK(contains(daily, "appendSubjectMarkers(subject, sizeof(subject), \"NOT REPORTING\", &staleSites,"));
  CHECK(contains(daily, "\"NO NEW READINGS\", &lateSites);"));
  // The subject date is the local date
  CHECK(contains(daily, "formatEpochDate(now, tzOffsetSec, dateStr, sizeof(dateStr));"));
  CHECK(!contains(daily, "gmtime("));
  // An oversized report drops optional fields, least useful first, before giving up:
  // u and current readings' times, then fmt, then the id, then old readings' times
  const std::string ladder = between(daily, "static char buffer[MAX_EMAIL_BUFFER];", "Daily email payload too large");
  CHECK(!ladder.empty());
  const size_t rU = ladder.find("s.remove(\"u\");");
  const size_t rFmt = ladder.find("doc.remove(\"fmt\");");
  const size_t rId = ladder.find("doc.remove(\"id\");");
  const size_t rAsOf = ladder.rfind("s.remove(\"asOf\");");
  CHECK(rU != std::string::npos && rFmt != std::string::npos && rId != std::string::npos &&
        rAsOf != std::string::npos);
  CHECK(rU < rFmt && rFmt < rId && rId < rAsOf);
  CHECK(contains(ladder, "if (!(s[\"stale\"] | false) && !(s[\"late\"] | false)) {"));
  // The renderers take "message" or "type" as the alarm shape: never in the daily body
  CHECK(!contains(daily, "doc[\"message\"]"));
  CHECK(!contains(daily, "doc[\"type\"]"));

  const std::string reminders = functionBody(sketch, "static void checkAlarmReminders() {");
  CHECK(!reminders.empty());
  CHECK(contains(reminders, "ReadingAge age = sensorReadingAge(rec, now, &ageSec);"));
  CHECK(contains(reminders, "no new reading for"));

  // The Apps Script bridge on /email-setup prints the lists and marks old readings
  CHECK(contains(sketch, "' NOT REPORTING (no data for over ' + (b.staleH || 49) + ' h): '"));
  CHECK(contains(sketch, "' with NO NEW READINGS: ' + (b.lateSites || '') + ' ***'"));
  CHECK(!contains(sketch, "NO NEW DATA"));
  CHECK(!contains(sketch, "b.lateH"));
}

int main() {
  testClassify();
  testCivil();
  testStamp();
  testReported();
  testNoteReadingEpoch();
  testSiteList();
  testSubjectMarker();
  testSketch();
  printf("email_staleness: %lu checks, %lu failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
