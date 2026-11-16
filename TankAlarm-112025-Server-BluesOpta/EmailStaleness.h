// EmailStaleness.h - how old is a reading, for the daily email and reminders.
//
// Pure helpers (no Arduino, mbed or ArduinoJson), host-tested in tests/host/email_staleness.
//
// Why (2026-11-16): a solar site's battery went flat and the client stopped reporting. The
// stale-client SMS fired at 49.9 h, but the daily email kept printing the site's last reading
// (23 psi, 11.35 mA) for three days as if it were current. The server always knew each
// reading's time (SensorRecord::lastUpdateEpoch); sendDailyEmail() just never sent it. These
// helpers decide when a reading counts as late or stale, format its time for the email, and
// build the "not reporting" site list for the subject and the top of the email.

#ifndef TANKALARM_EMAIL_STALENESS_H
#define TANKALARM_EMAIL_STALENESS_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum ReadingAge : uint8_t {
  READING_AGE_UNKNOWN = 0,  // no clock, or the reading has no time
  READING_AGE_FRESH,
  READING_AGE_LATE,         // older than the late limit: at least one daily report missed
  READING_AGE_STALE         // older than the stale limit: the stale-client SMS limit
};

// Classifies a reading taken (or received) at readingEpoch, as seen at now. Both are Unix
// seconds; anything not above 0 (including NaN) is "no time". A reading stamped a little
// ahead of the server clock counts as age 0. A limit of 0 or less is off. When ageSecondsOut
// is given it receives the age in seconds (0 for UNKNOWN).
static inline ReadingAge classifyReadingAge(double now, double readingEpoch,
                                            double lateSeconds, double staleSeconds,
                                            double *ageSecondsOut) {
  if (ageSecondsOut) *ageSecondsOut = 0.0;
  if (!(now > 0.0) || !(readingEpoch > 0.0)) return READING_AGE_UNKNOWN;
  double age = now - readingEpoch;
  if (!(age > 0.0)) age = 0.0;
  if (ageSecondsOut) *ageSecondsOut = age;
  if (staleSeconds > 0.0 && age >= staleSeconds) return READING_AGE_STALE;
  if (lateSeconds > 0.0 && age >= lateSeconds) return READING_AGE_LATE;
  return READING_AGE_FRESH;
}

struct CivilTime {
  int32_t year;
  uint8_t month;   // 1-12
  uint8_t day;     // 1-31
  uint8_t hour;    // 0-23
  uint8_t minute;  // 0-59
};

// Unix seconds to a proleptic Gregorian date and time (UTC, or local if the caller has
// already added its offset). Days-from-civil inverse by Howard Hinnant; valid for any
// int64 day count, with no gmtime() and no static buffer.
static inline void epochToCivil(int64_t secs, CivilTime *out) {
  int64_t days = secs / 86400;
  int64_t rem = secs % 86400;
  if (rem < 0) {
    rem += 86400;
    days -= 1;
  }
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const uint32_t doe = (uint32_t)(days - era * 146097);                       // [0, 146096]
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                // [0, 365]
  const uint32_t mp = (5 * doy + 2) / 153;                                     // [0, 11]
  const uint32_t d = doy - (153 * mp + 2) / 5 + 1;                             // [1, 31]
  const uint32_t m = mp < 10 ? mp + 3 : mp - 9;                                // [1, 12]
  out->year = (int32_t)((int64_t)yoe + era * 400 + (m <= 2 ? 1 : 0));
  out->month = (uint8_t)m;
  out->day = (uint8_t)d;
  out->hour = (uint8_t)(rem / 3600);
  out->minute = (uint8_t)((rem % 3600) / 60);
}

// The server's local-time offset in seconds: the configured standard offset (minutes from
// UTC, e.g. -360 for US Central) plus one hour while the manual DST toggle is on. The same
// rule scheduleNextDailyEmail() uses.
static inline int32_t localOffsetSeconds(int16_t tzOffsetMinutes, bool dstEnabled) {
  return ((int32_t)tzOffsetMinutes + (dstEnabled ? 60 : 0)) * 60;
}

// Formats epoch (Unix seconds) shifted by offsetSeconds as "YYYY-MM-DD HH:MM". When
// markUtc is true " UTC" is appended (use it when no time zone is configured, so a reader
// is not left guessing). Writes "" for an epoch that is not above 0. Always NUL-terminates
// a non-empty buffer; returns the snprintf result (the untruncated length, or < 0).
static inline int formatEpochStamp(double epoch, int32_t offsetSeconds, bool markUtc,
                                   char *buf, size_t cap) {
  if (!buf || cap == 0) return -1;
  buf[0] = '\0';
  if (!(epoch > 0.0)) return 0;
  CivilTime t;
  epochToCivil((int64_t)floor(epoch) + (int64_t)offsetSeconds, &t);
  return snprintf(buf, cap, "%04ld-%02u-%02u %02u:%02u%s", (long)t.year, (unsigned)t.month,
                  (unsigned)t.day, (unsigned)t.hour, (unsigned)t.minute, markUtc ? " UTC" : "");
}

// Formats the local date only, "YYYY-MM-DD" (the daily email subject's {date} token).
static inline int formatEpochDate(double epoch, int32_t offsetSeconds, char *buf, size_t cap) {
  if (!buf || cap == 0) return -1;
  buf[0] = '\0';
  if (!(epoch > 0.0)) return 0;
  CivilTime t;
  epochToCivil((int64_t)floor(epoch) + (int64_t)offsetSeconds, &t);
  return snprintf(buf, cap, "%04ld-%02u-%02u", (long)t.year, (unsigned)t.month, (unsigned)t.day);
}

// A short ", "-separated list of site names, for the email subject and banner. A name
// already in the list is not added again (two clients at one site show once). Once the
// next name would not fit, it is counted in `more` instead and siteListText() ends the
// list with " +N more".
#ifndef SITE_LIST_TEXT_CAP
#define SITE_LIST_TEXT_CAP 80
#endif

struct SiteList {
  char text[SITE_LIST_TEXT_CAP + 16];  // names, then room for " +NNN more"
  uint8_t shown;                       // names in text
  uint8_t more;                        // names that did not fit
};

static inline void siteListInit(SiteList *l) {
  l->text[0] = '\0';
  l->shown = 0;
  l->more = 0;
}

static inline bool siteListContains(const SiteList *l, const char *name) {
  const size_t n = strlen(name);
  const char *p = l->text;
  while (*p) {
    const char *end = strstr(p, ", ");
    const size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len == n && strncmp(p, name, n) == 0) return true;
    if (!end) break;
    p = end + 2;
  }
  return false;
}

static inline void siteListAdd(SiteList *l, const char *name) {
  if (!name || name[0] == '\0') return;
  if (siteListContains(l, name)) return;
  const size_t used = strlen(l->text);
  const size_t need = (l->shown > 0 ? 2 : 0) + strlen(name);
  if (used + need > SITE_LIST_TEXT_CAP) {
    if (l->more < 255) l->more++;
    return;
  }
  if (l->shown > 0) strcat(l->text, ", ");
  strcat(l->text, name);
  if (l->shown < 255) l->shown++;
}

// The list as text: "Silas, North" or "Silas, North +2 more". If the very first name was too
// long to fit, the text is "N sites" so the caller never prints an empty list.
static inline const char *siteListText(SiteList *l) {
  const size_t used = strlen(l->text);
  if (l->more > 0) {
    if (l->shown == 0) {
      snprintf(l->text, sizeof(l->text), "%u site%s", (unsigned)l->more, l->more == 1 ? "" : "s");
    } else {
      snprintf(l->text + used, sizeof(l->text) - used, " +%u more", (unsigned)l->more);
    }
    l->more = 0;  // finished: a second call returns the same text
  }
  return l->text;
}

// Appends " - <label>: <sites>" to a NUL-terminated subject, truncating safely at cap.
// Nothing is appended when sites is empty.
static inline void appendSubjectMarker(char *subject, size_t cap, const char *label,
                                       const char *sites) {
  if (!subject || cap == 0 || !sites || sites[0] == '\0') return;
  size_t used = 0;
  while (used < cap && subject[used] != '\0') used++;
  if (used >= cap) {
    subject[cap - 1] = '\0';
    return;
  }
  snprintf(subject + used, cap - used, " - %s: %s", label, sites);
}

#endif  // TANKALARM_EMAIL_STALENESS_H
