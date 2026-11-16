// EmailStaleness.h - how old is a reading, for the daily email and reminders.
//
// Pure helpers (no Arduino, mbed or ArduinoJson), host-tested in tests/host/email_staleness.
//
// Why (2026-11-16): a solar site's battery went flat and the client stopped reporting. The
// stale-client SMS fired at 49.9 h, but the daily email kept printing the site's last reading
// (23 psi, 11.35 mA) for three days as if it were current. The server always knew each
// reading's time (SensorRecord::lastUpdateEpoch); sendDailyEmail() just never sent it. These
// helpers decide when a reading counts as late or stale, format its time for the email, and
// build the site lists ("not reporting", "no new readings") for the subject and the top of
// the email.

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

// A client sends its daily report at the daily email time minus an offset (0 to 8 h, 2 h by
// default), with readings up to a few sample intervals older than the report. So at a
// healthy email the newest reading is a few hours old, and at the first email after a missed
// report it is about a day old: under any fixed limit that also suits slow schedules. This
// also counts a FRESH reading as LATE when the client's last daily report was made
// missedReportSeconds or more ago and nothing newer has arrived since (the reading is no newer
// than that report, give or take REPORT_BATCH_SLACK_SECONDS for a report sent in parts).
// lastReportEpoch is when the client made its last daily report, or 0 if the server has not
// seen one (it does not keep report times across a restart): then only the age limits apply.
#ifndef REPORT_BATCH_SLACK_SECONDS
#define REPORT_BATCH_SLACK_SECONDS 1800.0
#endif

static inline ReadingAge classifyReportedReadingAge(double now, double readingEpoch,
                                                    double lastReportEpoch,
                                                    double missedReportSeconds,
                                                    double lateSeconds, double staleSeconds,
                                                    double *ageSecondsOut) {
  const ReadingAge age =
      classifyReadingAge(now, readingEpoch, lateSeconds, staleSeconds, ageSecondsOut);
  if (age != READING_AGE_FRESH) return age;
  if (missedReportSeconds > 0.0 && lastReportEpoch > 0.0 &&
      now - lastReportEpoch >= missedReportSeconds &&
      readingEpoch <= lastReportEpoch + REPORT_BATCH_SLACK_SECONDS) {
    return READING_AGE_LATE;
  }
  return age;
}

// A client as a whole, from its newest reading, for the site lists. As above, except that a
// client whose last daily report was made within the stale limit is still reporting: its old
// readings (a failed sensor, or the first report after a battery outage) make it LATE ("no
// new readings"), not STALE ("not reporting").
static inline ReadingAge classifyClientAge(double now, double newestReadingEpoch,
                                           double lastReportEpoch, double missedReportSeconds,
                                           double lateSeconds, double staleSeconds) {
  ReadingAge age = classifyReportedReadingAge(now, newestReadingEpoch, lastReportEpoch,
                                              missedReportSeconds, lateSeconds, staleSeconds,
                                              nullptr);
  if (age == READING_AGE_STALE && lastReportEpoch > 0.0 && staleSeconds > 0.0 &&
      now - lastReportEpoch < staleSeconds) {
    age = READING_AGE_LATE;
  }
  return age;
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

// A short list of site names, for the email subject and the banner at the top of the email.
// A name already in the list is not added again (two clients at one site show once). Names
// are kept while they fit in SITE_LIST_TEXT_CAP characters joined by ", "; the rest are only
// counted, and siteListFormat() ends the list with " +N more". A repeat of a name that did
// not fit is recognised by a hash of the name, for the first SITE_LIST_MORE_HASHES such names.
#ifndef SITE_LIST_TEXT_CAP
#define SITE_LIST_TEXT_CAP 80
#endif
#ifndef SITE_LIST_MORE_HASHES
#define SITE_LIST_MORE_HASHES 16
#endif
// Room for a formatted list: the names, then " +NNN more"
#define SITE_LIST_TEXT_SIZE (SITE_LIST_TEXT_CAP + 16)

struct SiteList {
  char names[SITE_LIST_TEXT_CAP + 2];        // the names that fit, each NUL-terminated
  uint16_t used;                             // bytes used in names
  uint16_t joined;                           // length of the names joined by ", "
  uint8_t shown;                             // names kept
  uint8_t more;                              // other names, that did not fit
  uint8_t hashed;                            // entries used in moreHash
  uint32_t moreHash[SITE_LIST_MORE_HASHES];  // hashes of the names counted in more
};

static inline void siteListInit(SiteList *l) {
  l->names[0] = '\0';
  l->used = 0;
  l->joined = 0;
  l->shown = 0;
  l->more = 0;
  l->hashed = 0;
}

// FNV-1a, for recognising a name that did not fit
static inline uint32_t siteNameHash(const char *name) {
  uint32_t h = 2166136261u;
  for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
    h ^= *p;
    h *= 16777619u;
  }
  return h;
}

// The number of distinct names added: kept and counted.
static inline unsigned siteListCount(const SiteList *l) {
  return (unsigned)l->shown + (unsigned)l->more;
}

// True when the name was added before, kept or counted.
static inline bool siteListHas(const SiteList *l, const char *name) {
  if (!name || name[0] == '\0') return false;
  const char *p = l->names;
  for (uint8_t i = 0; i < l->shown; ++i) {
    if (strcmp(p, name) == 0) return true;
    p += strlen(p) + 1;
  }
  if (l->hashed > 0) {
    const uint32_t h = siteNameHash(name);
    for (uint8_t i = 0; i < l->hashed; ++i) {
      if (l->moreHash[i] == h) return true;
    }
  }
  return false;
}

// Adds a name. Returns true when it is new to the list (kept or counted).
static inline bool siteListAdd(SiteList *l, const char *name) {
  if (!name || name[0] == '\0') return false;
  if (siteListHas(l, name)) return false;
  const size_t len = strlen(name);
  const size_t joined = (size_t)l->joined + (l->shown > 0 ? 2 : 0) + len;
  if (joined > SITE_LIST_TEXT_CAP || l->shown == 255) {
    if (l->hashed < SITE_LIST_MORE_HASHES) l->moreHash[l->hashed++] = siteNameHash(name);
    if (l->more < 255) l->more++;
    return true;
  }
  // joined <= CAP keeps used + len + 1 <= CAP + 1: each name is at least 1 byte, each
  // separator 2, and each name takes len + 1 bytes here.
  memcpy(l->names + l->used, name, len + 1);
  l->used = (uint16_t)(l->used + len + 1);
  l->joined = (uint16_t)joined;
  l->shown++;
  return true;
}

static inline size_t siteListTailLen(unsigned rest) {
  if (rest == 0) return 0;
  char tmp[24];
  const int n = snprintf(tmp, sizeof(tmp), " +%u more", rest);
  return n > 0 ? (size_t)n : 0;
}

// Writes the list into out (cap bytes, NUL-terminated): as many names as fit with ", "
// between them, then " +N more" for the rest, never part of a name. "Silas, North +2 more".
// When not even one name fits it writes "N sites" ("1 site"), and when that does not fit
// either, "". Returns the length written.
static inline size_t siteListFormat(const SiteList *l, char *out, size_t cap) {
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  const unsigned total = siteListCount(l);
  if (total == 0) return 0;
  // The most names that fit together with the tail for the rest
  unsigned best = 0;
  size_t len = 0;
  const char *p = l->names;
  for (unsigned k = 1; k <= l->shown; ++k) {
    len += (k > 1 ? 2 : 0) + strlen(p);
    p += strlen(p) + 1;
    if (len + siteListTailLen(total - k) < cap) best = k;
  }
  if (best == 0) {
    const int n = snprintf(out, cap, "%u site%s", total, total == 1 ? "" : "s");
    if (n < 0 || (size_t)n >= cap) {
      out[0] = '\0';
      return 0;
    }
    return (size_t)n;
  }
  size_t pos = 0;
  p = l->names;
  for (unsigned k = 0; k < best; ++k) {
    if (k > 0) {
      out[pos++] = ',';
      out[pos++] = ' ';
    }
    const size_t n = strlen(p);
    memcpy(out + pos, p, n);
    pos += n;
    p += n + 1;
  }
  out[pos] = '\0';
  if (best < total) {
    const int n = snprintf(out + pos, cap - pos, " +%u more", total - best);
    if (n > 0) pos += (size_t)n;
  }
  return pos;
}

// The shortest marker appendSubjectMarker() can add for this list: " - <label>: N sites".
static inline size_t subjectMarkerMinLen(const char *label, const SiteList *sites) {
  if (!label || !sites || siteListCount(sites) == 0) return 0;
  const unsigned total = siteListCount(sites);
  const int n = snprintf(nullptr, 0, " - %s: %u site%s", label, total, total == 1 ? "" : "s");
  return n > 0 ? (size_t)n : 0;
}

// Appends " - <label>: <sites>" to a NUL-terminated subject of cap bytes, leaving `reserve`
// bytes unused for a later marker. The list is cut at a name, with " +N more" for the rest
// (siteListFormat). Nothing is appended when the list is empty or not even
// " - <label>: N sites" fits.
static inline void appendSubjectMarker(char *subject, size_t cap, size_t reserve,
                                       const char *label, const SiteList *sites) {
  if (!subject || cap == 0) return;
  size_t used = 0;
  while (used < cap && subject[used] != '\0') used++;
  if (used >= cap) {
    subject[cap - 1] = '\0';
    return;
  }
  if (!label || !sites || siteListCount(sites) == 0) return;
  size_t avail = cap - used;  // bytes, with the NUL
  if (avail <= reserve) return;
  avail -= reserve;
  const int head = snprintf(nullptr, 0, " - %s: ", label);
  if (head < 0 || (size_t)head + 1 >= avail) return;
  snprintf(subject + used, avail, " - %s: ", label);
  if (siteListFormat(sites, subject + used + head, avail - (size_t)head) == 0) {
    subject[used] = '\0';
  }
}

// Appends both site lists to the subject, the more important first: it gets the room it
// needs less what the second needs to show at least its count, when both counts fit.
static inline void appendSubjectMarkers(char *subject, size_t cap, const char *firstLabel,
                                        const SiteList *first, const char *secondLabel,
                                        const SiteList *second) {
  if (!subject || cap == 0) return;
  size_t used = 0;
  while (used < cap && subject[used] != '\0') used++;
  size_t reserve = subjectMarkerMinLen(secondLabel, second);
  if (used + subjectMarkerMinLen(firstLabel, first) + reserve >= cap) reserve = 0;
  appendSubjectMarker(subject, cap, reserve, firstLabel, first);
  appendSubjectMarker(subject, cap, 0, secondLabel, second);
}

#endif  // TANKALARM_EMAIL_STALENESS_H
