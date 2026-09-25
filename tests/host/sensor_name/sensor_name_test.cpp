// Host tests for TankAlarm-112025-Server-BluesOpta/TankAlarm_SensorName.h: how the server names a
// sensor in SMS and email text (site + label, "#N" only for a Display Number, never the internal
// sensorIndex), how it keeps a sensor's Display Number ("un") in step with the client, and the
// sketch text that uses both (alarm, reminder, snooze and unload texts, the daily email's "#N"
// and the dashboard's Latest line).
//
//   make -C tests/host/sensor_name test

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "TankAlarm_SensorName.h"

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

#define CHECK_STR(actual, expected)                                                    \
  do {                                                                                 \
    ++gChecks;                                                                         \
    if (strcmp((actual), (expected)) != 0) {                                           \
      ++gFailures;                                                                     \
      printf("FAIL %s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, (actual),   \
             (expected));                                                              \
    }                                                                                  \
  } while (0)

// The sketch builds each tail with snprintf into char tail[160]. This wrapper does the same
// without the compiler's truncation analysis (the max-length cases truncate on purpose).
#ifdef __GNUC__
__attribute__((format(printf, 3, 4)))
#endif
static int tailf(char *out, size_t outLen, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int w = vsnprintf(out, outLen, fmt, ap);
  va_end(ap);
  return w;
}

// True when s is well-formed UTF-8 (lead bytes followed by the right number of continuation
// bytes; no stray continuation bytes; no truncated sequence at the end).
static bool validUtf8(const char *s) {
  const unsigned char *p = (const unsigned char *)s;
  while (*p != 0) {
    unsigned need;
    if (*p < 0x80) {
      need = 0;
    } else if ((*p & 0xE0) == 0xC0) {
      need = 1;
    } else if ((*p & 0xF0) == 0xE0) {
      need = 2;
    } else if ((*p & 0xF8) == 0xF0) {
      need = 3;
    } else {
      return false;
    }
    ++p;
    for (unsigned i = 0; i < need; ++i, ++p) {
      if ((*p & 0xC0) != 0x80) {
        return false;  // also catches the terminating NUL mid-sequence
      }
    }
  }
  return true;
}

static void testNoteDisplayNumber() {
  CHECK(noteDisplayNumber(true, 5, true, 0) == 5);      // set
  CHECK(noteDisplayNumber(true, 0, true, 5) == 0);      // explicit 0 clears
  CHECK(noteDisplayNumber(false, 0, true, 5) == 0);     // telemetry/daily without "un": cleared on the client
  CHECK(noteDisplayNumber(false, 0, false, 5) == 5);    // a note that need not carry "un" keeps it
  CHECK(noteDisplayNumber(true, 300, true, 5) == 5);    // out of range: not a Display Number
  CHECK(noteDisplayNumber(true, -1, true, 5) == 5);
  CHECK(noteDisplayNumber(true, 255, true, 5) == 255);  // range ends
  CHECK(noteDisplayNumber(true, 256, true, 5) == 5);
  CHECK(noteDisplayNumber(true, 1, false, 0) == 1);     // present "un" wins on any note
  CHECK(noteDisplayNumber(true, 7, true, 7) == 7);      // unchanged
  CHECK(noteDisplayNumber(false, 0, true, 0) == 0);
  CHECK(noteDisplayNumber(true, INT32_MIN, true, 9) == 9);
  CHECK(noteDisplayNumber(true, INT32_MAX, true, 9) == 9);
}

static void testUtf8FitLen() {
  CHECK(utf8FitLen(nullptr, 10) == 0);
  CHECK(utf8FitLen("", 10) == 0);
  CHECK(utf8FitLen("abc", 10) == 3);
  CHECK(utf8FitLen("abc", 3) == 3);
  CHECK(utf8FitLen("abc", 2) == 2);
  CHECK(utf8FitLen("abc", 0) == 0);
  // "a" + e-acute (C3 A9) + "b": a cut after 2 bytes would split the e-acute.
  static const char kAcute[] = "a\xC3\xA9" "b";
  CHECK(utf8FitLen(kAcute, 1) == 1);
  CHECK(utf8FitLen(kAcute, 2) == 1);
  CHECK(utf8FitLen(kAcute, 3) == 3);
  CHECK(utf8FitLen(kAcute, 4) == 4);
  // Euro sign (E2 82 AC), 3 bytes, and a 4-byte emoji (F0 9F 9B A2).
  static const char kEuro[] = "\xE2\x82\xAC\xE2\x82\xAC";
  CHECK(utf8FitLen(kEuro, 2) == 0);
  CHECK(utf8FitLen(kEuro, 3) == 3);
  CHECK(utf8FitLen(kEuro, 5) == 3);
  CHECK(utf8FitLen(kEuro, 6) == 6);
  static const char kEmoji[] = "x\xF0\x9F\x9B\xA2y";
  CHECK(utf8FitLen(kEmoji, 1) == 1);
  CHECK(utf8FitLen(kEmoji, 4) == 1);
  CHECK(utf8FitLen(kEmoji, 5) == 5);
  CHECK(utf8FitLen(kEmoji, 100) == 6);
}

static void checkName(const char *site, const char *label, uint8_t un, size_t outLen,
                      const char *expected, int line) {
  char out[80];
  memset(out, 'Z', sizeof(out));
  const size_t n = formatSensorName(out, outLen, site, label, un);
  ++gChecks;
  if (strcmp(out, expected) != 0 || n != strlen(expected)) {
    ++gFailures;
    printf("FAIL line %d: formatSensorName(\"%s\", \"%s\", %u, %u) = \"%s\" (%u), want \"%s\"\n", line,
           site ? site : "(null)", label ? label : "(null)", (unsigned)un, (unsigned)outLen, out,
           (unsigned)n, expected);
  }
  CHECK(n < outLen);
  CHECK(out[outLen - 1] == '\0' || strlen(out) < outLen);
  if (outLen < sizeof(out)) {
    CHECK(out[outLen] == 'Z');  // nothing written past outLen
  }
  CHECK(validUtf8(out));
}
#define NAME(site, label, un, outLen, expected) checkName(site, label, un, outLen, expected, __LINE__)

static void testFormatSensorName() {
  // The owner's examples.
  NAME("Silas Cox", "Wellhead", 0, 64, "Silas Cox Wellhead");
  NAME("Silas Cox", "Wellhead", 7, 64, "Silas Cox Wellhead #7");
  NAME("Silas Cox", "Wellhead", 255, 64, "Silas Cox Wellhead #255");
  NAME("Silas", "", 0, 64, "Silas");
  NAME("", "Cox Wellhead", 0, 64, "Cox Wellhead");
  NAME("", "Cox Wellhead", 4, 64, "Cox Wellhead #4");
  NAME("", "", 0, 64, "Sensor");
  NAME("", "", 3, 64, "Sensor #3");
  NAME(nullptr, nullptr, 0, 64, "Sensor");
  NAME(nullptr, "Wellhead", 0, 64, "Wellhead");
  NAME("Silas", nullptr, 2, 64, "Silas #2");
  // Trailing spaces trimmed; an all-space part is empty.
  NAME("Silas Cox  ", "Wellhead ", 0, 64, "Silas Cox Wellhead");
  NAME("   ", "Wellhead", 0, 64, "Wellhead");
  NAME("   ", "   ", 1, 64, "Sensor #1");
  // CR-8: leading spaces and tabs are trimmed too, and a tab counts as a space, as the daily
  // email script's String.trim() does.
  NAME(" Silas Cox", "Wellhead", 0, 64, "Silas Cox Wellhead");
  NAME("Silas Cox", "  Wellhead", 7, 64, "Silas Cox Wellhead #7");
  NAME("  Silas Cox  ", "  Wellhead  ", 0, 64, "Silas Cox Wellhead");
  NAME("\tSilas Cox\t", "\t Wellhead \t", 3, 64, "Silas Cox Wellhead #3");
  NAME("Silas\tCox", "Well\thead", 0, 64, "Silas\tCox Well\thead");  // inner tabs are kept
  NAME("\t", " \t ", 0, 64, "Sensor");                              // blanks only: empty
  NAME(" \t", "Wellhead", 2, 64, "Wellhead #2");
  // Leading blanks do not use up the 23-byte cap: 3 blanks + a 23-byte site keeps all 23.
  NAME(" \t ABCDEFGHIJKLMNOPQRSTUVW", "", 0, 64, "ABCDEFGHIJKLMNOPQRSTUVW");
  NAME("\tABCDEFGHIJKLMNOPQRSTUVWXYZ01234", "Pump", 9, 64, "ABCDEFGHIJKLMNOPQRSTUVW Pump #9");
  // The cap lands after a tab: trimmed like a space.
  NAME("ABCDEFGHIJKLMNOPQRSTUV\tWXYZ", "", 0, 64, "ABCDEFGHIJKLMNOPQRSTUV");
  // Shortening to fit outLen works from the trimmed parts.
  NAME("  Silas Cox", "\tWellhead", 7, 21, "Silas Cox Wellhea #7");
  NAME(" Silas", "  Cox Wellhead", 7, 8, "Sila #7");
  // Each part capped at 23 bytes: a 31-byte site is cut to 23.
  NAME("ABCDEFGHIJKLMNOPQRSTUVWXYZ01234", "", 0, 64, "ABCDEFGHIJKLMNOPQRSTUVW");
  NAME("ABCDEFGHIJKLMNOPQRSTUVWXYZ01234", "Pump", 9, 64, "ABCDEFGHIJKLMNOPQRSTUVW Pump #9");
  // The cap lands after a trailing space: trimmed.
  NAME("ABCDEFGHIJKLMNOPQRSTUV WXYZ", "", 0, 64, "ABCDEFGHIJKLMNOPQRSTUV");
  // A 2-byte character straddling the 23-byte cap is left out whole: 22 ASCII + e-acute.
  NAME("ABCDEFGHIJKLMNOPQRSTUV\xC3\xA9", "", 0, 64, "ABCDEFGHIJKLMNOPQRSTUV");
  NAME("", "ABCDEFGHIJKLMNOPQRSTUV\xC3\xA9X", 0, 64, "ABCDEFGHIJKLMNOPQRSTUV");
  NAME("ABCDEFGHIJKLMNOPQRSTU\xC3\xA9", "", 0, 64, "ABCDEFGHIJKLMNOPQRSTU\xC3\xA9");  // exactly 23
  // Shortening to fit outLen: the label first, then the site; the number stays whole.
  NAME("Silas Cox", "Wellhead", 7, 22, "Silas Cox Wellhead #7");  // exactly fits (21 bytes)
  NAME("Silas Cox", "Wellhead", 7, 21, "Silas Cox Wellhea #7");
  NAME("Silas Cox", "Wellhead", 7, 13, "Silas Cox #7");            // no room for " W": label dropped
  NAME("Silas Cox", "Wellhead", 7, 15, "Silas Cox W #7");
  NAME("Silas", "Cox Wellhead", 7, 8, "Sila #7");
  NAME("Silas", "Cox Wellhead", 7, 4, "#7");
  NAME("Silas", "Cox Wellhead", 7, 3, "");
  NAME("Silas", "Cox Wellhead", 7, 1, "");
  NAME("Silas", "Cox Wellhead", 0, 4, "Sil");
  NAME("Silas", "Cox Wellhead", 0, 11, "Silas Cox");     // "Silas Cox " trimmed
  NAME("Silas", "Cox Wellhead", 255, 6, "#255");
  NAME("Silas", "Cox Wellhead", 255, 5, "");
  NAME("", "", 3, 5, "S #3");                            // "Sensor" fallback shortened like a site
  NAME("", "", 3, 6, "Se #3");
  NAME("", "", 3, 7, "Sen #3");
  NAME("Silas Cox", "", 0, 7, "Silas");                  // "Silas " trimmed
  // A cut inside a 2-byte character drops the whole character.
  NAME("Caf\xC3\xA9", "Tank", 0, 5, "Caf");
  NAME("Caf\xC3\xA9", "Tank", 0, 6, "Caf\xC3\xA9");
  NAME("Site", "\xC3\xA9\xC3\xA9", 0, 8, "Site \xC3\xA9");
  NAME("Site", "\xC3\xA9\xC3\xA9", 0, 7, "Site");         // "Site " + half a character -> "Site"

  // outLen 0 writes nothing; null out is safe.
  char guard[4] = {'Q', 'Q', 'Q', 'Q'};
  CHECK(formatSensorName(guard, 0, "Silas", "Cox", 7) == 0);
  CHECK(guard[0] == 'Q');
  CHECK(formatSensorName(nullptr, 10, "Silas", "Cox", 7) == 0);

  // Every size from 1 to 64 for a few inputs: bounded, NUL-terminated, valid UTF-8, and a
  // number is either whole or absent.
  static const char *const kSites[] = {"Silas Cox", "", "Caf\xC3\xA9 \xE2\x82\xAC Station Longname X", "   ",
                                        " \t Silas Cox \t"};
  static const char *const kLabels[] = {"Wellhead", "", "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9 Tank"};
  static const uint8_t kNums[] = {0, 7, 255};
  for (size_t si = 0; si < sizeof(kSites) / sizeof(kSites[0]); ++si) {
    for (size_t li = 0; li < sizeof(kLabels) / sizeof(kLabels[0]); ++li) {
      for (size_t ni = 0; ni < sizeof(kNums) / sizeof(kNums[0]); ++ni) {
        for (size_t outLen = 1; outLen <= 64; ++outLen) {
          char out[65];
          memset(out, 'Z', sizeof(out));
          const size_t n = formatSensorName(out, outLen, kSites[si], kLabels[li], kNums[ni]);
          CHECK(n < outLen && out[n] == '\0' && strlen(out) == n);
          CHECK(out[outLen] == 'Z');
          CHECK(validUtf8(out));
          CHECK(n == 0 || (out[n - 1] != ' ' && out[n - 1] != '\t'));
          CHECK(n == 0 || (out[0] != ' ' && out[0] != '\t'));  // CR-8: no leading blank
          if (kNums[ni] == 0) {
            CHECK(strchr(out, '#') == nullptr);
          } else if (n > 0) {
            char want[8];
            snprintf(want, sizeof(want), "#%u", (unsigned)kNums[ni]);
            const size_t wl = strlen(want);
            CHECK(n >= wl && strcmp(out + n - wl, want) == 0);
          }
        }
      }
    }
  }
}

static void testComposeSensorText() {
  char msg[160];
  char tail[160];

  // The owner's analog alarm example, with and without a Display Number.
  tailf(tail, sizeof(tail), " %s alarm %.1f %s", "high", 34.4f, "in");
  CHECK(composeSensorText(msg, sizeof(msg), "", "Silas Cox", "Wellhead", 0, tail));
  CHECK_STR(msg, "Silas Cox Wellhead high alarm 34.4 in");
  CHECK(composeSensorText(msg, sizeof(msg), "", "Silas Cox", "Wellhead", 7, tail));
  CHECK_STR(msg, "Silas Cox Wellhead #7 high alarm 34.4 in");
  // A site-only record (the field fleet before labels): no "sensor 1".
  CHECK(composeSensorText(msg, sizeof(msg), "", "Silas Cox", "", 0, tail));
  CHECK_STR(msg, "Silas Cox high alarm 34.4 in");

  // Each call site's text (tails built with the sketch's format strings, pinned below).
  tailf(tail, sizeof(tail), " Relay safety timeout - relay forced OFF");
  CHECK(composeSensorText(msg, sizeof(msg), "", "Silas Cox", "Pump Relay", 2, tail));
  CHECK_STR(msg, "Silas Cox Pump Relay #2 Relay safety timeout - relay forced OFF");
  tailf(tail, sizeof(tail), " Float Switch %s", "ACTIVATED");
  CHECK(composeSensorText(msg, sizeof(msg), "", "Silas Cox", "High Float", 0, tail));
  CHECK_STR(msg, "Silas Cox High Float Float Switch ACTIVATED");
  tailf(tail, sizeof(tail), " Float Switch clear (%s)", "OFF");
  CHECK(composeSensorText(msg, sizeof(msg), "", "Silas Cox", "High Float", 3, tail));
  CHECK_STR(msg, "Silas Cox High Float #3 Float Switch clear (OFF)");
  tailf(tail, sizeof(tail), " still in %s alarm (%.1f %s)", "high", 34.4f, "in");
  CHECK(composeSensorText(msg, sizeof(msg), "REMINDER: ", "Silas Cox", "Wellhead", 0, tail));
  CHECK_STR(msg, "REMINDER: Silas Cox Wellhead still in high alarm (34.4 in)");
  tailf(tail, sizeof(tail), " still in %s alarm (%s)", "triggered", "ON");
  CHECK(composeSensorText(msg, sizeof(msg), "REMINDER: ", "Silas Cox", "High Float", 3, tail));
  CHECK_STR(msg, "REMINDER: Silas Cox High Float #3 still in triggered alarm (ON)");
  tailf(tail, sizeof(tail), " reminders %s by %s. Still in %s alarm (%.1f %s).%s", "paused", "Jim",
        "high", 34.4f, "in", " Auto-resumes on recovery; reply UNSNOOZE to resume now.");
  CHECK(composeSensorText(msg, sizeof(msg), "SNOOZED: ", "Silas Cox", "Wellhead", 0, tail));
  CHECK_STR(msg, "SNOOZED: Silas Cox Wellhead reminders paused by Jim. Still in high alarm (34.4 in). "
                 "Auto-resumes on recovery; reply UNSNOOZE to resume now.");
  tailf(tail, sizeof(tail), " reminders %s by %s. Still in %s alarm (%s).%s", "active again", "dashboard",
        "triggered", "ON", "");
  CHECK(composeSensorText(msg, sizeof(msg), "RESUMED: ", "Silas Cox", "High Float", 3, tail));
  CHECK_STR(msg, "RESUMED: Silas Cox High Float #3 reminders active again by dashboard. Still in triggered alarm (ON).");
  tailf(tail, sizeof(tail), " unloaded: %.1f %s delivered (peak %.1f, now %.1f)", 85.5f, "in", 90.0f, 4.5f);
  CHECK(composeSensorText(msg, sizeof(msg), "", "Silas Cox", "Wellhead", 0, tail));
  CHECK_STR(msg, "Silas Cox Wellhead unloaded: 85.5 in delivered (peak 90.0, now 4.5)");
  CHECK(composeSensorText(msg, sizeof(msg), "", "Silas Cox", "Wellhead", 7, tail));
  CHECK_STR(msg, "Silas Cox Wellhead #7 unloaded: 85.5 in delivered (peak 90.0, now 4.5)");

  // Max-length cases: 23-byte site, 23-byte label (both longer in the record), Display Number
  // 255, the longest alarm type/unit/who and a wide reading. strlen <= 159 and the reading is
  // kept.
  static const char kSite[] = "SSSSSSSSSSSSSSSSSSSSSSSxxxxxxxx";   // 31 bytes, capped at 23
  static const char kLabel[] = "LLLLLLLLLLLLLLLLLLLLLLL";          // 23 bytes (char label[24])
  static const char kType[] = "TTTTTTTTTTTTTTTTTTTTTTT";           // char alarmType[24]
  static const char kUnit[] = "gallons";                           // char measurementUnit[8]
  static const char kWho[] = "WWWWWWWWWWWWWWWWWWWWWWW";            // char who[24]
  const float kValue = -99999.9f;
  struct Case {
    const char *prefix;
    const char *reading;  // must survive in the text
  };
  char reading[48];
  tailf(reading, sizeof(reading), "%.1f %s", kValue, kUnit);
  for (int which = 0; which < 9; ++which) {
    Case c = {"", reading};
    switch (which) {
      case 0: tailf(tail, sizeof(tail), " Relay safety timeout - relay forced OFF"); c.reading = "forced OFF"; break;
      case 1: tailf(tail, sizeof(tail), " Float Switch %s", "NOT ACTIVATED"); c.reading = "NOT ACTIVATED"; break;
      case 2: tailf(tail, sizeof(tail), " Float Switch clear (%s)", "OFF"); c.reading = "(OFF)"; break;
      case 3: tailf(tail, sizeof(tail), " %s alarm %.1f %s", kType, kValue, kUnit); break;
      case 4:
        c.prefix = "REMINDER: ";
        tailf(tail, sizeof(tail), " still in %s alarm (%s)", kType, "OFF");
        c.reading = "(OFF)";
        break;
      case 5:
        c.prefix = "REMINDER: ";
        tailf(tail, sizeof(tail), " still in %s alarm (%.1f %s)", kType, kValue, kUnit);
        break;
      case 6:
        c.prefix = "SNOOZED: ";
        tailf(tail, sizeof(tail), " reminders %s by %s. Still in %s alarm (%s).%s", "paused", kWho, kType, "OFF",
              " Auto-resumes on recovery; reply UNSNOOZE to resume now.");
        c.reading = "alarm (OFF).";
        break;
      case 7:
        c.prefix = "SNOOZED: ";
        tailf(tail, sizeof(tail), " reminders %s by %s. Still in %s alarm (%.1f %s).%s", "paused", kWho, kType,
              kValue, kUnit, " Auto-resumes on recovery; reply UNSNOOZE to resume now.");
        break;
      default:
        tailf(tail, sizeof(tail), " unloaded: %.1f %s delivered (peak %.1f, now %.1f)", kValue, kUnit, kValue,
              kValue);
        break;
    }
    memset(msg, 'Z', sizeof(msg));
    composeSensorText(msg, sizeof(msg), c.prefix, kSite, kLabel, 255, tail);
    ++gChecks;
    if (!(strlen(msg) <= 159 && strstr(msg, c.reading) != nullptr && validUtf8(msg) &&
          strncmp(msg, c.prefix, strlen(c.prefix)) == 0)) {
      ++gFailures;
      printf("FAIL max-length case %d: \"%s\"\n", which, msg);
    }
    // The name keeps at least 16 bytes: 11 of the site and " #255".
    CHECK(strncmp(msg + strlen(c.prefix), "SSSSSSSSSSS", 11) == 0);
    CHECK(strstr(msg, " #255 ") != nullptr);
  }
  // With realistic values the whole max-name text fits in 160: 23+1+23+5 bytes of name.
  tailf(tail, sizeof(tail), " %s alarm %.1f %s", "high", 34.4f, "in");
  CHECK(composeSensorText(msg, sizeof(msg), "", kSite, kLabel, 255, tail));
  CHECK_STR(msg, "SSSSSSSSSSSSSSSSSSSSSSS LLLLLLLLLLLLLLLLLLLLLLL #255 high alarm 34.4 in");
  // So do the relay-timeout, float, reminder, unload and RESUMED texts.
  static const char kMaxName[] = "SSSSSSSSSSSSSSSSSSSSSSS LLLLLLLLLLLLLLLLLLLLLLL #255";  // 52 bytes
  for (int which = 0; which < 6; ++which) {
    const char *prefix = "";
    switch (which) {
      case 0: tailf(tail, sizeof(tail), " Relay safety timeout - relay forced OFF"); break;
      case 1: tailf(tail, sizeof(tail), " Float Switch %s", "NOT ACTIVATED"); break;
      case 2: tailf(tail, sizeof(tail), " Float Switch clear (%s)", "OFF"); break;
      case 3:
        prefix = "REMINDER: ";
        tailf(tail, sizeof(tail), " still in %s alarm (%.1f %s)", "high", 34.4f, "in");
        break;
      case 4:
        prefix = "RESUMED: ";
        tailf(tail, sizeof(tail), " reminders %s by %s. Still in %s alarm (%.1f %s).%s", "active again", "SMS reply",
              "high", 34.4f, "in", "");
        break;
      default:
        tailf(tail, sizeof(tail), " unloaded: %.1f %s delivered (peak %.1f, now %.1f)", 85.5f, "in", 90.0f, 4.5f);
        break;
    }
    CHECK(composeSensorText(msg, sizeof(msg), prefix, kSite, kLabel, 255, tail));
    CHECK(strncmp(msg + strlen(prefix), kMaxName, strlen(kMaxName)) == 0 &&
          strcmp(msg + strlen(prefix) + strlen(kMaxName), tail) == 0);
  }
  // A SNOOZED notice carries the auto-resume sentence: its tail is 112 bytes with who "Jim",
  // 118 with "SMS reply" or "dashboard" and 132 with a 23-byte contact name, which leaves 38,
  // 32 or 18 bytes for the name. The tail stays whole; the label is shortened first.
  static const char kAutoResume[] = " Auto-resumes on recovery; reply UNSNOOZE to resume now.";
  tailf(tail, sizeof(tail), " reminders %s by %s. Still in %s alarm (%.1f %s).%s", "paused", "Jim", "high", 34.4f,
        "in", kAutoResume);
  CHECK(strlen(tail) == 112);
  CHECK(composeSensorText(msg, sizeof(msg), "SNOOZED: ", kSite, kLabel, 255, tail));
  CHECK_STR(msg, "SNOOZED: SSSSSSSSSSSSSSSSSSSSSSS LLLLLLLLL #255 reminders paused by Jim. Still in high alarm "
                 "(34.4 in). Auto-resumes on recovery; reply UNSNOOZE to resume now.");
  tailf(tail, sizeof(tail), " reminders %s by %s. Still in %s alarm (%.1f %s).%s", "paused", "SMS reply", "high",
        34.4f, "in", kAutoResume);
  CHECK(strlen(tail) == 118);
  CHECK(composeSensorText(msg, sizeof(msg), "SNOOZED: ", kSite, kLabel, 255, tail));
  CHECK_STR(msg, "SNOOZED: SSSSSSSSSSSSSSSSSSSSSSS LLL #255 reminders paused by SMS reply. Still in high alarm "
                 "(34.4 in). Auto-resumes on recovery; reply UNSNOOZE to resume now.");
  static const char kWho23[] = "Dashboard user name xyz";  // 23 bytes (char who[24])
  tailf(tail, sizeof(tail), " reminders %s by %s. Still in %s alarm (%.1f %s).%s", "paused", kWho23, "high", 34.4f,
        "in", kAutoResume);
  CHECK(strlen(tail) == 132);
  // Today's field name without a Display Number (18 bytes) still fits whole ...
  CHECK(composeSensorText(msg, sizeof(msg), "SNOOZED: ", "Silas Cox", "Wellhead", 0, tail));
  CHECK_STR(msg, "SNOOZED: Silas Cox Wellhead reminders paused by Dashboard user name xyz. Still in high alarm "
                 "(34.4 in). Auto-resumes on recovery; reply UNSNOOZE to resume now.");
  // ... and with Display Number 7 (21 bytes) the label loses 3 bytes.
  CHECK(composeSensorText(msg, sizeof(msg), "SNOOZED: ", "Silas Cox", "Wellhead", 7, tail));
  CHECK_STR(msg, "SNOOZED: Silas Cox Wellh #7 reminders paused by Dashboard user name xyz. Still in high alarm "
                 "(34.4 in). Auto-resumes on recovery; reply UNSNOOZE to resume now.");
  CHECK(strlen(msg) == 159);

  // A long prefix plus a tail longer than outLen: no overflow, NUL-terminated, returns false.
  char small[40];
  memset(small, 'Z', sizeof(small));
  static const char kLongPrefix[] = "PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP";  // 56 bytes
  static const char kLongTail[] =
      " tail tail tail tail tail tail tail tail tail tail tail tail tail tail tail tail tail tail";
  CHECK(!composeSensorText(small, 20, kLongPrefix, "Silas", "Cox", 7, kLongTail));
  CHECK(strlen(small) == 19 && small[19] == '\0' && small[20] == 'Z');
  CHECK(!composeSensorText(small, 30, "REMINDER: ", "Silas", "Cox", 7, kLongTail));
  CHECK(strlen(small) <= 29 && small[30] == 'Z');
  CHECK(strncmp(small, "REMINDER: Silas Cox #7", 22) == 0);  // name keeps its 16 bytes
  CHECK(!composeSensorText(small, 12, "REMINDER: ", "Silas", "Cox", 0, kLongTail));
  CHECK_STR(small, "REMINDER: S");  // only 1 byte left after the prefix: the name gets it
  // With a Display Number that byte cannot hold " #7": no half number, and no name without it.
  CHECK(!composeSensorText(small, 12, "REMINDER: ", "Silas", "Cox", 7, kLongTail));
  CHECK_STR(small, "REMINDER:  ");
  // A tail longer than the whole buffer with no prefix.
  CHECK(!composeSensorText(msg, 40, "", "Silas Cox", "Wellhead", 7, kLongTail));
  CHECK(strlen(msg) == 39);
  CHECK(strncmp(msg, "Silas Cox Wel #7 tail", 21) == 0);  // 16 bytes of room: "Silas Cox Wel #7"
  // Null prefix and tail are empty; outLen 0 and 1.
  CHECK(composeSensorText(msg, sizeof(msg), nullptr, "Silas", "", 0, nullptr));
  CHECK_STR(msg, "Silas");
  small[0] = 'Q';
  CHECK(!composeSensorText(small, 0, "", "Silas", "", 0, " x"));
  CHECK(small[0] == 'Q');
  CHECK(!composeSensorText(small, 1, "", "Silas", "", 0, " x"));
  CHECK(small[0] == '\0');
  CHECK(!composeSensorText(nullptr, 10, "", "Silas", "", 0, " x"));
  // The tail is cut at a UTF-8 boundary.
  // (20 bytes of room: "Site" + 16 bytes of tail would end inside the 8th e-acute.)
  CHECK(!composeSensorText(small, 21, "", "Site", "", 0, " \xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"));
  CHECK(validUtf8(small) && strlen(small) == 19);
  CHECK_STR(small, "Site \xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9");
}

static bool endsWith(const char *s, const char *suffix) {
  const size_t n = strlen(s);
  const size_t k = strlen(suffix);
  return n >= k && strcmp(s + n - k, suffix) == 0;
}

// CR-8: broadcastSnoozeChange's SNOOZED/RESUMED text. The long hint when it fits; otherwise the
// short hint, which keeps "Reply UNSNOOZE" whole.
static void testComposeSnoozeText() {
  char msg[160];
  CHECK(strcmp(SNOOZE_HINT, " Auto-resumes on recovery; reply UNSNOOZE to resume now.") == 0);
  CHECK(strcmp(SNOOZE_HINT_SHORT, " Reply UNSNOOZE to resume.") == 0);

  // Everyday texts are unchanged (the same strings testComposeSensorText pins).
  CHECK(composeSnoozeText(msg, sizeof(msg), true, "Silas Cox", "Wellhead", 0, "Jim", "high", "34.4 in"));
  CHECK_STR(msg, "SNOOZED: Silas Cox Wellhead reminders paused by Jim. Still in high alarm (34.4 in). "
                 "Auto-resumes on recovery; reply UNSNOOZE to resume now.");
  CHECK(composeSnoozeText(msg, sizeof(msg), false, "Silas Cox", "High Float", 3, "dashboard", "triggered", "ON"));
  CHECK_STR(msg, "RESUMED: Silas Cox High Float #3 reminders active again by dashboard. Still in triggered alarm (ON).");
  static const char kWho23[] = "Christopher Worthington";  // 23 bytes (char who[24])
  CHECK(strlen(kWho23) == 23);
  // A 23-byte contact name with a short alarm type still fits with the long hint.
  CHECK(composeSnoozeText(msg, sizeof(msg), true, "Silas Cox", "Wellhead", 0, kWho23, "high", "34.4 in"));
  CHECK_STR(msg, "SNOOZED: Silas Cox Wellhead reminders paused by Christopher Worthington. Still in high alarm "
                 "(34.4 in). Auto-resumes on recovery; reply UNSNOOZE to resume now.");

  // A 23-byte contact name with not_triggered (a float): the long text would cut the hint, so the
  // short hint is used and the whole text fits.
  CHECK(composeSnoozeText(msg, sizeof(msg), true, "Johnson Ranch North", "High Float", 0, kWho23,
                          "not_triggered", "OFF"));
  CHECK_STR(msg, "SNOOZED: Johnson Ranch North High Float reminders paused by Christopher Worthington. Still in "
                 "not_triggered alarm (OFF). Reply UNSNOOZE to resume.");
  // ... and with sensor-fault on an analog sensor, a 28-byte site (capped at 23) and Display Number 7.
  CHECK(composeSnoozeText(msg, sizeof(msg), true, "North Dakota Pump Station 12", "Wellhead", 7, kWho23,
                          "sensor-fault", "0.0 in"));
  CHECK_STR(msg, "SNOOZED: North Dakota Pump Stati Wellhead #7 reminders paused by Christopher Worthington. Still "
                 "in sensor-fault alarm (0.0 in). Reply UNSNOOZE to resume.");
  // The longest fields: 23-byte who and alarm type, a 16-byte reading, a 31-byte site, a 23-byte
  // label and Display Number 255. The short text still fits whole; the name gets 20 bytes.
  CHECK(composeSnoozeText(msg, sizeof(msg), true, "SSSSSSSSSSSSSSSSSSSSSSSxxxxxxxx", "LLLLLLLLLLLLLLLLLLLLLLL", 255,
                          "WWWWWWWWWWWWWWWWWWWWWWW", "TTTTTTTTTTTTTTTTTTTTTTT", "-12345.6 gallons"));
  CHECK_STR(msg, "SNOOZED: SSSSSSSSSSSSSSS #255 reminders paused by WWWWWWWWWWWWWWWWWWWWWWW. Still in "
                 "TTTTTTTTTTTTTTTTTTTTTTT alarm (-12345.6 gallons). Reply UNSNOOZE to resume.");
  CHECK(strlen(msg) == 159);

  // Every who length 0-23 with each alarm type and reading: the SNOOZED text always fits, ends with
  // one of the two hints (the long one whenever it fits) and keeps "UNSNOOZE"; RESUMED has no hint.
  static const char *const kTypes[] = {"high", "low", "triggered", "not_triggered", "sensor-fault",
                                       "sensor-stuck", "relay_timeout", "TTTTTTTTTTTTTTTTTTTTTTT"};
  static const char *const kReadings[] = {"ON", "OFF", "0.0 in", "34.4 in", "-12345.6 gallons", "-99999.9 gallons"};
  static const char *const kSitesS[] = {"Silas Cox", "Johnson Ranch North", "SSSSSSSSSSSSSSSSSSSSSSSxxxxxxxx", ""};
  char who[24];
  for (size_t w = 0; w <= 23; ++w) {
    memset(who, 'W', w);
    who[w] = '\0';
    for (size_t ti = 0; ti < sizeof(kTypes) / sizeof(kTypes[0]); ++ti) {
      for (size_t ri = 0; ri < sizeof(kReadings) / sizeof(kReadings[0]); ++ri) {
        for (size_t si = 0; si < sizeof(kSitesS) / sizeof(kSitesS[0]); ++si) {
          const bool fit = composeSnoozeText(msg, sizeof(msg), true, kSitesS[si], "LLLLLLLLLLLLLLLLLLLLLLL", 255, who,
                                             kTypes[ti], kReadings[ri]);
          ++gChecks;
          if (!(fit && strlen(msg) <= 159 && strncmp(msg, "SNOOZED: ", 9) == 0 &&
                strstr(msg, "UNSNOOZE") != nullptr &&
                (endsWith(msg, SNOOZE_HINT) || endsWith(msg, SNOOZE_HINT_SHORT)))) {
            ++gFailures;
            printf("FAIL snooze who=%u type=%s reading=%s: \"%s\"\n", (unsigned)w, kTypes[ti], kReadings[ri], msg);
          }
          // The short hint only when the long text does not fit.
          char longTail[200];
          tailf(longTail, sizeof(longTail), " reminders paused by %s. Still in %s alarm (%s).%s", who, kTypes[ti],
                kReadings[ri], SNOOZE_HINT);
          char probe[160];
          const bool longFits = composeSensorText(probe, sizeof(probe), "SNOOZED: ", kSitesS[si],
                                                  "LLLLLLLLLLLLLLLLLLLLLLL", 255, longTail);
          CHECK(longFits == endsWith(msg, SNOOZE_HINT));
          CHECK(!longFits || strcmp(probe, msg) == 0);
          composeSnoozeText(msg, sizeof(msg), false, kSitesS[si], "LLLLLLLLLLLLLLLLLLLLLLL", 255, who, kTypes[ti],
                            kReadings[ri]);
          CHECK(strncmp(msg, "RESUMED: ", 9) == 0 && strstr(msg, "UNSNOOZE") == nullptr && endsWith(msg, ").") );
        }
      }
    }
  }
}

// strstr from `from`, or nullptr when `from` was not found.
static const char *findAfter(const char *needle, const char *from) {
  return (from != nullptr) ? strstr(from, needle) : nullptr;
}

// True when needle occurs in [begin, end).
static bool foundBetween(const char *needle, const char *begin, const char *end) {
  const char *p = findAfter(needle, begin);
  return p != nullptr && end != nullptr && p < end;
}

static unsigned countOf(const char *text, const char *needle) {
  unsigned n = 0;
  for (const char *p = strstr(text, needle); p != nullptr; p = strstr(p + 1, needle)) {
    ++n;
  }
  return n;
}

static void testSketchText() {
  FILE *f = fopen(SERVER_SKETCH, "rb");
  CHECK(f != nullptr);
  if (!f) return;
  static char text[2 << 20];
  const size_t n = fread(text, 1, sizeof(text) - 1, f);
  fclose(f);
  text[n] = '\0';
  CHECK(n > 500000 && n < sizeof(text) - 1);
  CHECK(strstr(text, "#include \"TankAlarm_SensorName.h\"") != nullptr);

  // handleTelemetry and handleDaily's sensors[] loop use the helper, with noteAlwaysCarriesUn = true.
  static const char kTelemetryCall[] =
      "    JsonVariantConst un = doc[\"un\"];\n"
      "    const uint8_t displayNumber = noteDisplayNumber(!un.isNull(), un.is<int32_t>() ? un.as<int32_t>() : -1,\n"
      "                                                    true, rec->userNumber);\n";
  static const char kDailyCall[] =
      "      JsonVariantConst un = t[\"un\"];\n"
      "      const uint8_t displayNumber = noteDisplayNumber(!un.isNull(), un.is<int32_t>() ? un.as<int32_t>() : -1,\n"
      "                                                      true, rec->userNumber);\n";
  const char *telemetry = strstr(text, "static void handleTelemetry(JsonDocument &doc, double epoch) {");
  const char *alarm = strstr(text, "static void handleAlarm(JsonDocument &doc, double epoch) {");
  const char *alarmEnd = strstr(text, "static ClientMetadata *findClientMetadata(const char *clientUid) {");
  const char *daily = strstr(text, "static void handleDaily(JsonDocument &doc, double epoch) {");
  const char *unload = strstr(text, "static void handleUnload(JsonDocument &doc, double epoch) {");
  const char *unloadEnd = strstr(text, "static void logUnloadEvent(const UnloadLogEntry &entry) {");
  const char *buildUnload = strstr(text, "static void buildUnloadText(char *out, size_t outLen, const UnloadLogEntry &entry) {");
  const char *unloadSms = strstr(text, "static void sendUnloadSms(const UnloadLogEntry &entry) {");
  const char *unloadEmail = strstr(text, "static void sendUnloadEmail(const UnloadLogEntry &entry) {");
  const char *upsert = strstr(text, "static SensorRecord *upsertSensorRecord(const char *clientUid, uint8_t sensorIndex, bool *created) {");
  const char *snooze = strstr(text, "static void broadcastSnoozeChange(const SensorRecord &rec, bool snoozed, const char *who) {");
  const char *snoozeEnd = strstr(text, "static bool applyReminderSnooze(SensorRecord &rec, bool snooze, const char *who) {");
  const char *reminders = strstr(text, "static void checkAlarmReminders() {");
  CHECK(telemetry != nullptr && alarm != nullptr && alarmEnd != nullptr && daily != nullptr);
  CHECK(unload != nullptr && unloadEnd != nullptr && buildUnload != nullptr && unloadSms != nullptr);
  CHECK(unloadEmail != nullptr && upsert != nullptr && snooze != nullptr && snoozeEnd != nullptr);
  CHECK(reminders != nullptr);
  const char *remindersEnd = findAfter("\n}\n", reminders);
  const char *telemetryCall = findAfter(kTelemetryCall, telemetry);
  CHECK(telemetryCall != nullptr && alarm != nullptr && telemetryCall < alarm);
  const char *dailyCall = findAfter(kDailyCall, daily);
  CHECK(dailyCall != nullptr);
  const char *sensorsLoop = findAfter("  JsonArray sensors = doc[\"sensors\"];\n  for (JsonObject t : sensors) {", daily);
  CHECK(sensorsLoop != nullptr && dailyCall != nullptr && sensorsLoop < dailyCall);
  // Four call sites: telemetry, daily, alarm and unload (alarm and unload keep the number when
  // "un" is missing: recovered/clear/fault alarm notes and unload notes never carry it).
  CHECK(countOf(text, "noteDisplayNumber(") == 4);
  CHECK(foundBetween("  JsonVariantConst un = doc[\"un\"];\n"
                     "  const uint8_t displayNumber = noteDisplayNumber(!un.isNull(), un.is<int32_t>() ? un.as<int32_t>() : -1,\n"
                     "                                                  false, known != nullptr ? known->userNumber : 0);\n",
                     unload, unloadEnd));
  // R12: handleAlarm uses the helper with noteAlwaysCarriesUn = false and marks the registry dirty
  // on a change, like telemetry.
  CHECK(foundBetween("    JsonVariantConst un = doc[\"un\"];\n"
                     "    const uint8_t displayNumber = noteDisplayNumber(!un.isNull(), un.is<int32_t>() ? un.as<int32_t>() : -1,\n"
                     "                                                    false, rec->userNumber);\n"
                     "    if (displayNumber != rec->userNumber) {\n"
                     "      rec->userNumber = displayNumber;\n"
                     "      gSensorRegistryDirty = true;\n"
                     "    }\n", alarm, alarmEnd));
  // The presence-only writes are gone everywhere (a null "un" read as 0, 300 cleared the number).
  CHECK(strstr(text, "    if (t.containsKey(\"un\")) {\n      rec->userNumber = t[\"un\"].as<uint8_t>();") == nullptr);
  CHECK(strstr(text, "containsKey(\"un\")") == nullptr);
  CHECK(strstr(text, "[\"un\"].as<uint8_t>()") == nullptr);

  // CR-7: handleAlarm's sensor path and handleUnload drop a note whose "k" is missing, not an
  // integer or < 1 (the telemetry guard), before any record, log, SMS or email work.
  static const char kAlarmGuard[] =
      "  if (!doc[\"k\"].is<int>() || doc[\"k\"].as<int>() < 1) {\n"
      "    Serial.println(F(\"Alarm dropped: missing/invalid sensor index k\"));\n"
      "    return;\n"
      "  }\n"
      "  uint8_t sensorIndex = doc[\"k\"].as<uint8_t>();\n"
      "  SensorRecord *rec = upsertSensorRecord(clientUid, sensorIndex);\n";
  static const char kUnloadGuard[] =
      "  if (!doc[\"k\"].is<int>() || doc[\"k\"].as<int>() < 1) {\n"
      "    Serial.println(F(\"Unload dropped: missing/invalid sensor index k\"));\n"
      "    return;\n"
      "  }\n"
      "  uint8_t sensorIndex = doc[\"k\"].as<uint8_t>();\n";
  const char *alarmGuard = findAfter(kAlarmGuard, alarm);
  const char *systemReturn = findAfter("  if (isSystemAlarm) {\n", alarm);
  CHECK(alarmGuard != nullptr && alarmEnd != nullptr && alarmGuard < alarmEnd);
  CHECK(systemReturn != nullptr && alarmGuard != nullptr && systemReturn < alarmGuard);  // system alarms carry no k
  CHECK(countOf(text, "uint8_t sensorIndex = doc[\"k\"].as<uint8_t>();") == 3);  // telemetry, alarm, unload
  const char *unloadGuard = findAfter(kUnloadGuard, unload);
  CHECK(unloadGuard != nullptr && unloadEnd != nullptr && unloadGuard < unloadEnd);
  const char *unloadUidCheck = findAfter("  if (!isValidClientUid(clientUid)) {\n", unload);
  const char *unloadLookup = findAfter("findSensorByHash(clientUid, sensorIndex)", unload);
  const char *unloadLog = findAfter("  logUnloadEvent(entry);\n", unload);
  CHECK(unloadUidCheck != nullptr && unloadGuard != nullptr && unloadUidCheck < unloadGuard);
  CHECK(unloadGuard != nullptr && unloadLookup != nullptr && unloadLog != nullptr && unloadGuard < unloadLookup &&
        unloadLookup < unloadLog);
  CHECK(!foundBetween("doc[\"k\"].as<uint8_t>()", unload, unloadGuard));  // no read before the guard

  // sendDailyEmail still sends sensorIndex (old pasted scripts print '#undefined' without it) and
  // userNumber only when set.
  const char *email = strstr(text, "static void sendDailyEmail() {");
  CHECK(email != nullptr);
  CHECK(findAfter("    obj[\"sensorIndex\"] = gSensorRecords[i].sensorIndex;\n"
                  "    if (gSensorRecords[i].userNumber > 0) {\n"
                  "      obj[\"userNumber\"] = gSensorRecords[i].userNumber;\n"
                  "    }\n", email) != nullptr);
  // R13: "sensorMa" only for a current-loop sensor (or an untyped record holding a reading), so
  // voltage, pulse and float lines print no " (0 mA)".
  CHECK(findAfter("    if (strcmp(gSensorRecords[i].sensorType, \"currentLoop\") == 0 ||\n"
                  "        (gSensorRecords[i].sensorType[0] == '\\0' && gSensorRecords[i].sensorMa > 0.0f)) {\n"
                  "      obj[\"sensorMa\"] = roundTo(gSensorRecords[i].sensorMa, 2);\n"
                  "    }\n", email) != nullptr);
  CHECK(countOf(text, "obj[\"sensorMa\"] = ") == 1);
  CHECK(strstr(text, "\n    obj[\"sensorMa\"] = roundTo(gSensorRecords[i].sensorMa, 2);\n") == nullptr);

  // Dashboard "Latest" line: the client-level "un" follows the same sensor as "n"/"k".
  CHECK(strstr(text, "      clientObj[\"k\"] = rec.sensorIndex;\n"
                     "      // \"un\" follows the same sensor as \"n\"/\"k\": drop an older sensor's number when this one has none.\n"
                     "      if (rec.userNumber > 0) {\n"
                     "        clientObj[\"un\"] = rec.userNumber;\n"
                     "      } else {\n"
                     "        clientObj.remove(\"un\");\n"
                     "      }\n") != nullptr);

  // --- SMS/email texts name the sensor with composeSensorText, never with sensorIndex. ---
  // Four builders: handleAlarm, buildUnloadText and checkAlarmReminders call composeSensorText;
  // broadcastSnoozeChange calls composeSnoozeText (TankAlarm_SensorName.h), which calls it.
  CHECK(countOf(text, "composeSensorText(") == 3);
  CHECK(countOf(text, "composeSnoozeText(") == 1);
  // handleAlarm: one tail per branch, one compose, the alarm id unchanged.
  CHECK(foundBetween("      snprintf(tail, sizeof(tail), \" Relay safety timeout - relay forced OFF\");\n", alarm, alarmEnd));
  CHECK(foundBetween("      snprintf(tail, sizeof(tail), \" Float Switch %s\", stateDesc);\n", alarm, alarmEnd));
  CHECK(foundBetween("      snprintf(tail, sizeof(tail), \" Float Switch clear (%s)\", digitalStateText(rec->currentValue));\n", alarm, alarmEnd));
  CHECK(foundBetween("      snprintf(tail, sizeof(tail), \" %s alarm %.1f %s\", rec->alarmType, rec->currentValue, rec->measurementUnit[0] ? rec->measurementUnit : \"in\");\n", alarm, alarmEnd));
  CHECK(foundBetween("    char tail[160];\n", alarm, alarmEnd));
  CHECK(foundBetween("    char message[160];\n"
                     "    composeSensorText(message, sizeof(message), \"\", rec->site, rec->label, rec->userNumber, tail);\n",
                     alarm, alarmEnd));
  CHECK(foundBetween("    snprintf(alarmId, sizeof(alarmId), \"%s_%d\", clientUid, (int)rec->sensorIndex);\n"
                     "    sendSmsAlert(message, alarmId);\n"
                     "    sendEmailAlert(\"TankAlarm Alert\", message, alarmId);", alarm, alarmEnd));
  // A record first created by an alarm gets its site from the note (fill only, never overwrite),
  // before the alarm text is composed; handleAlarm never writes the label.
  const char *alarmSiteFill = findAfter("  if (rec->site[0] == '\\0') {\n"
                                        "    strlcpy(rec->site, doc[\"s\"] | \"\", sizeof(rec->site));\n"
                                        "  }\n", alarm);
  const char *alarmCompose = findAfter("    composeSensorText(message, sizeof(message), \"\", rec->site", alarm);
  CHECK(alarmSiteFill != nullptr && alarmCompose != nullptr && alarmSiteFill < alarmCompose);
  CHECK(!foundBetween("strlcpy(rec->label", alarm, alarmEnd));
  CHECK(!foundBetween("\n  strlcpy(rec->site, doc[\"s\"] | \"\", sizeof(rec->site));\n", alarm, alarmEnd));
  // checkAlarmReminders.
  CHECK(foundBetween("      snprintf(tail, sizeof(tail), \" still in %s alarm (%s)\",\n"
                     "               type, digitalStateText(rec.currentValue));\n", reminders, remindersEnd));
  CHECK(foundBetween("      snprintf(tail, sizeof(tail), \" still in %s alarm (%.1f %s)\",\n"
                     "               type, rec.currentValue,\n"
                     "               rec.measurementUnit[0] ? rec.measurementUnit : \"in\");\n", reminders, remindersEnd));
  CHECK(foundBetween("    char message[160];\n"
                     "    composeSensorText(message, sizeof(message), \"REMINDER: \", rec.site, rec.label, rec.userNumber, tail);\n",
                     reminders, remindersEnd));
  CHECK(foundBetween("    snprintf(alarmId, sizeof(alarmId), \"%s_%d\", rec.clientUid, (int)rec.sensorIndex);\n"
                     "    sendSmsAlert(message, alarmId);\n"
                     "    sendEmailAlert(\"TankAlarm Reminder\", message, alarmId);", reminders, remindersEnd));
  // broadcastSnoozeChange (SMS reply path): the reading is the float's state or "value unit", and
  // composeSnoozeText (tested above) builds the text, falling back to the short UNSNOOZE hint.
  CHECK(foundBetween("    strlcpy(reading, digitalStateText(rec.currentValue), sizeof(reading));\n", snooze, snoozeEnd));
  CHECK(foundBetween("    snprintf(reading, sizeof(reading), \"%.1f %s\", rec.currentValue,\n"
                     "             rec.measurementUnit[0] ? rec.measurementUnit : \"in\");\n",
                     snooze, snoozeEnd));
  CHECK(foundBetween("  char message[160];\n"
                     "  composeSnoozeText(message, sizeof(message), snoozed, rec.site, rec.label, rec.userNumber, who,\n"
                     "                    rec.alarmType, reading);\n",
                     snooze, snoozeEnd));
  CHECK(!foundBetween("char tail[", snooze, snoozeEnd));
  CHECK(strstr(text, "reply UNSNOOZE to resume now.") == nullptr);  // the hints live in the header
  CHECK(foundBetween("  snprintf(alarmId, sizeof(alarmId), \"%s_%d\", rec.clientUid, (int)rec.sensorIndex);\n", snooze, snoozeEnd));
  CHECK(countOf(text, "char tail[160];") == 3);
  CHECK(strstr(text, "char tail[2") == nullptr && strstr(text, "char tail[3") == nullptr);
  // Unload: one builder shared by the SMS and the email; alarm ids unchanged.
  CHECK(foundBetween("  snprintf(tail, sizeof(tail), \" unloaded: %.1f %s delivered (peak %.1f, now %.1f)\",\n"
                     "           delivered, u, entry.peakInches, entry.emptyInches);\n"
                     "  composeSensorText(out, outLen, \"\", entry.siteName, entry.tankLabel, entry.userNumber, tail);\n",
                     buildUnload, unloadSms));
  CHECK(foundBetween("  char message[160];\n  buildUnloadText(message, sizeof(message), entry);\n", unloadSms, unloadEmail));
  CHECK(foundBetween("  char message[160];\n  buildUnloadText(message, sizeof(message), entry);\n", unloadEmail, upsert));
  CHECK(countOf(text, "buildUnloadText(message, sizeof(message), entry);") == 2);
  CHECK(foundBetween("  snprintf(alarmId, sizeof(alarmId), \"%s_%d\", entry.clientUid, (int)entry.sensorIndex);\n"
                     "  sendSmsAlert(message, alarmId);\n", unloadSms, unloadEmail));
  CHECK(foundBetween("  snprintf(alarmId, sizeof(alarmId), \"%s_%d\", entry.clientUid, (int)entry.sensorIndex);\n"
                     "  sendEmailAlert(\"TankAlarm Unload Report\", message, alarmId);\n", unloadEmail, upsert));
  // handleUnload names the sensor from the record and never uses a "Tank" placeholder (CR-7): an
  // unknown label stays empty in the text, the unload log / api "n" and the record.
  CHECK(strstr(text, "  uint8_t sensorIndex;         // Internal sensor index\n"
                     "  uint8_t userNumber;          // Display Number (0 = none); names the sensor as \" #N\" in texts\n") != nullptr);
  CHECK(foundBetween("  const char *noteLabel = doc[\"n\"] | \"\";", unload, unloadEnd));
  CHECK(strstr(text, "doc[\"n\"] | \"Tank\"") == nullptr);
  CHECK(foundBetween("  const SensorRecord *known = findSensorByHash(clientUid, sensorIndex);\n"
                     "  const char *tankLabel = noteLabel;\n"
                     "  if (tankLabel[0] == '\\0' && known != nullptr) {\n"
                     "    tankLabel = known->label;\n"
                     "  }\n"
                     "  JsonVariantConst un = doc[\"un\"];\n", unload, unloadEnd));
  CHECK(!foundBetween("\"Tank\";", unload, unloadEnd) && !foundBetween("\"Tank\",", unload, unloadEnd) &&
        !foundBetween("\"Tank\")", unload, unloadEnd));
  CHECK(foundBetween("  strlcpy(entry.tankLabel, tankLabel, sizeof(entry.tankLabel));\n", unload, unloadEnd));
  CHECK(foundBetween("  entry.userNumber = displayNumber;\n", unload, unloadEnd));
  // Only a label the note carries replaces the record's; an empty one is never filled in.
  CHECK(foundBetween("    if (noteLabel[0] != '\\0') {\n"
                     "      strlcpy(rec->label, noteLabel, sizeof(rec->label));\n"
                     "    }\n", unload, unloadEnd));
  CHECK(!foundBetween("strlcpy(rec->label, tankLabel", unload, unloadEnd));
  // With no label the unload text names the sensor by site (and Display Number) alone.
  {
    char msg[160];
    char tail[160];
    tailf(tail, sizeof(tail), " unloaded: %.1f %s delivered (peak %.1f, now %.1f)", 85.5f, "in", 90.0f, 4.5f);
    CHECK(composeSensorText(msg, sizeof(msg), "", "Silas Cox", "", 0, tail));
    CHECK_STR(msg, "Silas Cox unloaded: 85.5 in delivered (peak 90.0, now 4.5)");
    CHECK(composeSensorText(msg, sizeof(msg), "", "Silas Cox", "", 4, tail));
    CHECK_STR(msg, "Silas Cox #4 unloaded: 85.5 in delivered (peak 90.0, now 4.5)");
  }
  // /api/unloads keeps "n" and "k" and adds "un" only for a Display Number.
  CHECK(strstr(text, "    obj[\"n\"] = entry.tankLabel;              // Tank label\n"
                     "    obj[\"k\"] = entry.sensorIndex;             // Sensor index\n"
                     "    if (entry.userNumber > 0) {\n"
                     "      obj[\"un\"] = entry.userNumber;          // Display Number (left out when blank)\n"
                     "    }\n") != nullptr);

  // Negative pins: the old "sensor <k>" / "#<k>" naming is gone from every text.
  CHECK(strstr(text, "? \" #\" : \" sensor \"") == nullptr);
  CHECK(strstr(text, "\"%s #%d unloaded") == nullptr);
  CHECK(strstr(text, "rec->userNumber > 0 ? rec->userNumber : rec->sensorIndex") == nullptr);
  CHECK(strstr(text, "rec.userNumber > 0 ? rec.userNumber : rec.sensorIndex") == nullptr);
  CHECK(strstr(text, "char shortSite[24];") == nullptr);
}

int main() {
  testNoteDisplayNumber();
  testUtf8FitLen();
  testFormatSensorName();
  testComposeSensorText();
  testComposeSnoozeText();
  testSketchText();
  if (gFailures) {
    printf("sensor_name: %lu of %lu checks FAILED\n", gFailures, gChecks);
    return 1;
  }
  printf("sensor_name: all %lu checks passed\n", gChecks);
  return 0;
}
