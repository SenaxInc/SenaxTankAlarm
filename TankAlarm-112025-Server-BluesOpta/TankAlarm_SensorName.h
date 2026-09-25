// TankAlarm_SensorName.h - how the server names a sensor to people, and how it keeps a sensor's
// Display Number ("un", the "Display Number" box on the client config page) in step with the
// client. Pure: C headers only, host-tested in tests/host/sensor_name.
//
// NAMING RULE (owner decision 2026-09-24). Every SMS and email that names a sensor calls it
//   site + " " + label    (each with leading and trailing spaces and tabs removed; a blank
//                           part left out, "Sensor" when both are blank)
//   + " #<Display Number>" only when the Display Number is 1-255.
// TRIMMING differs slightly between the three places that build a name. This file trims only
// ASCII spaces and tabs at the ends and keeps inner runs. The daily-email Apps Script's
// String.trim() also strips every other Unicode space and line break at the ends (NBSP, BOM,
// CR/LF, ...). The JSONata route's $trim (per the JSONata docs) also turns inner tabs and line
// breaks into spaces and collapses each run of them to one space. So the three agree for names
// of printable text with single inner spaces (what the config page produces); a name with NBSP
// at an end, or with inner tabs or doubled spaces, can differ by those characters between SMS
// and the email routes.
// UTF-8: each part is cut at a character boundary, and a part that already ends in an
// incomplete multi-byte sequence (a client or older server stored it cut by bytes) loses that
// partial character, so SMS and email never carry a broken character.
// The internal sensor number (sensorIndex, "k") is never printed in an SMS or email: it is a
// registry key, not something an operator set. Examples:
//   "Silas Cox Wellhead high alarm 34.4 in"      (Display Number blank)
//   "Silas Cox Wellhead #7 high alarm 34.4 in"   (Display Number 7)
// Serial and transmission logs are diagnostics and keep printing sensorIndex.
//
// DISPLAY NUMBER UPDATES. The client stamps "un" on every telemetry note and on every
// daily-report sensors[] entry while the sensor's Display Number is set (1-255), and omits it
// when the box is blank (0):
//   sendTelemetry():      if (cfg.userNumber > 0) doc["un"] = cfg.userNumber;
//   appendDailyMonitor(): if (cfg.userNumber > 0) t["un"] = cfg.userNumber;
// So on those notes a missing "un" means "no Display Number", and a Display Number that was set
// and later cleared must go back to 0 on the server. Writing only when "un" is present left the
// old number in the registry (and in "#N" on emails and the dashboard) forever.
// Alarm and unload notes do not always carry "un", so there a missing "un" keeps the number.

#ifndef TANKALARM_SENSOR_NAME_H
#define TANKALARM_SENSOR_NAME_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Display Number to store after a note.
//   hasUn               - the note has an "un" key
//   un                  - its value (read as a signed integer)
//   noteAlwaysCarriesUn - the client stamps "un" on this note type whenever it is set
//   current             - the number the server holds now (0 = none)
// A present "un" outside 0-255 is not a valid Display Number and leaves the current one.
static inline uint8_t noteDisplayNumber(bool hasUn, int32_t un, bool noteAlwaysCarriesUn,
                                        uint8_t current) {
  if (hasUn) {
    return (0 <= un && un <= 255) ? (uint8_t)un : current;
  }
  return noteAlwaysCarriesUn ? (uint8_t)0 : current;
}

// Longest part of a site or a label that goes into a name (the SMS budget is 160 bytes).
#define SENSOR_NAME_PART_MAX 23u
// Room composeSensorText keeps for the name even when the tail is long.
#define SENSOR_NAME_MIN_ROOM 16

// Largest n <= min(strlen(s), maxBytes) that does not end inside a UTF-8 sequence, so s[0..n)
// can be copied without splitting a character. A null s gives 0. Scans at most maxBytes + 1
// bytes of s.
static inline size_t utf8FitLen(const char *s, size_t maxBytes) {
  if (s == NULL) {
    return 0;
  }
  size_t n = 0;
  while (n < maxBytes && s[n] != '\0') {
    ++n;
  }
  if (s[n] == '\0') {
    return n;  // the whole string fits
  }
  // Cut before s[n]: back off while s[n] is a continuation byte (10xxxxxx).
  while (n > 0 && (((unsigned char)s[n]) & 0xC0u) == 0x80u) {
    --n;
  }
  return n;
}

// n, less an incomplete UTF-8 sequence at the end of s[0..n): when s[0..n) ends in a lead byte
// followed by fewer continuation bytes than it announces (a lone C3, E2 82 or F0 9F 98), the
// length before that lead byte; otherwise n. Needed because a stored site or label can end
// mid-character (the client and the server copy them with strlcpy, which cuts by bytes), and
// utf8FitLen keeps a string that fits whole. Malformed bytes elsewhere are left as they are.
// Reads only s[0..n); a null s gives 0.
static inline size_t utf8CompleteLen(const char *s, size_t n) {
  if (s == NULL) {
    return 0;
  }
  size_t i = n;
  // Back over up to 3 continuation bytes (10xxxxxx) to the byte that should lead them.
  while (i > 0 && n - i < 3 && (((unsigned char)s[i - 1]) & 0xC0u) == 0x80u) {
    --i;
  }
  if (i == 0) {
    return n;  // only continuation bytes: no lead byte to judge by
  }
  const unsigned char lead = (unsigned char)s[i - 1];
  size_t need;  // bytes the sequence that starts at s[i - 1] should have
  if ((lead & 0xE0u) == 0xC0u) {
    need = 2;
  } else if ((lead & 0xF0u) == 0xE0u) {
    need = 3;
  } else if ((lead & 0xF8u) == 0xF0u) {
    need = 4;
  } else {
    return n;  // ASCII, a 4th continuation byte or an invalid byte: nothing cut here
  }
  return (n - (i - 1) < need) ? i - 1 : n;
}

// A space or a tab: the blanks trimmed from both ends of a site or a label (CR-8).
static inline bool sensorNameBlank(char c) {
  return c == ' ' || c == '\t';
}

// s past its leading spaces and tabs (null stays null).
static inline const char *sensorNameSkipBlank(const char *s) {
  if (s == NULL) {
    return NULL;
  }
  while (sensorNameBlank(*s)) {
    ++s;
  }
  return s;
}

// Bytes of s to use when it may take at most maxBytes: cut at a UTF-8 boundary, drop an
// incomplete multi-byte sequence left at the end by an earlier byte cut (utf8CompleteLen), then
// drop trailing spaces and tabs (so a part of only blanks is empty); repeated until neither
// changes, so "Tank \xC3" gives "Tank". Leading blanks are skipped by the caller
// (sensorNameSkipBlank) before the cap, so they never use up the part's bytes.
static inline size_t sensorNamePartLen(const char *s, size_t maxBytes) {
  size_t n = utf8FitLen(s, maxBytes);
  for (;;) {
    n = utf8CompleteLen(s, n);
    const size_t before = n;
    while (n > 0 && sensorNameBlank(s[n - 1])) {
      --n;
    }
    if (n == before) {
      return n;
    }
  }
}

// Writes the sensor's name (the NAMING RULE above) into out and returns its length.
//   site, label - may be null (read as empty); each is capped at SENSOR_NAME_PART_MAX bytes
//   userNumber  - the Display Number; 0 = none
// When the name is longer than outLen - 1 it is shortened: the " #<n>" is kept whole, then the
// label is shortened, then the site (or the "Sensor" fallback), each at a UTF-8 boundary with
// trailing spaces and tabs removed. If nothing of the site and label is left the number is written as
// "#7" (no leading space). If " #<n>" itself does not fit, the number is left out entirely
// (never half a number) and so is the rest. out is always NUL-terminated when outLen > 0;
// outLen 0 writes nothing.
static inline size_t formatSensorName(char *out, size_t outLen, const char *site,
                                      const char *label, uint8_t userNumber) {
  if (out == NULL || outLen == 0) {
    return 0;
  }
  const size_t room = outLen - 1;
  // Leading spaces and tabs are skipped before the cap, so they never use up a part's bytes
  // (only spaces and tabs: see TRIMMING above for how the email routes differ).
  const char *s = (site != NULL) ? sensorNameSkipBlank(site) : "";
  const char *l = (label != NULL) ? sensorNameSkipBlank(label) : "";
  size_t sLen = sensorNamePartLen(s, SENSOR_NAME_PART_MAX);
  size_t lLen = sensorNamePartLen(l, SENSOR_NAME_PART_MAX);
  if (sLen == 0 && lLen == 0) {
    s = "Sensor";
    sLen = 6;
  }

  char num[8];  // " #255" + NUL
  size_t numLen = 0;
  if (userNumber > 0) {
    int w = snprintf(num, sizeof(num), " #%u", (unsigned)userNumber);
    numLen = (w > 0) ? (size_t)w : 0;
  }

  // Room for the site and label once the number is reserved.
  size_t baseRoom;
  if (numLen <= room) {
    baseRoom = room - numLen;
  } else {
    baseRoom = 0;
    numLen = 0;  // the number alone does not fit: leave it out
  }

  if (sLen + ((sLen > 0 && lLen > 0) ? 1 : 0) + lLen > baseRoom) {
    if (sLen > 0 && lLen > 0 && baseRoom >= sLen + 2) {
      // Shorten the label first; the site stays whole.
      lLen = sensorNamePartLen(l, baseRoom - sLen - 1);
    } else if (sLen > 0) {
      // No room for any of the label: drop it and shorten the site.
      lLen = 0;
      sLen = sensorNamePartLen(s, sLen < baseRoom ? sLen : baseRoom);
    } else {
      lLen = sensorNamePartLen(l, lLen < baseRoom ? lLen : baseRoom);
    }
  }

  size_t pos = 0;
  if (sLen > 0) {
    memcpy(out + pos, s, sLen);
    pos += sLen;
  }
  if (sLen > 0 && lLen > 0) {
    out[pos++] = ' ';
  }
  if (lLen > 0) {
    memcpy(out + pos, l, lLen);
    pos += lLen;
  }
  if (numLen > 0) {
    const char *n = (pos > 0) ? num : num + 1;  // "#7" when nothing precedes it
    const size_t nLen = (pos > 0) ? numLen : numLen - 1;
    memcpy(out + pos, n, nLen);
    pos += nLen;
  }
  out[pos] = '\0';
  return pos;
}

// Writes prefix + NAME + tail into out (NAME per formatSensorName). The tail - the reading and
// the fixed wording, which starts with its own space - has priority: the name gets whatever is
// left after the prefix and the whole tail, but at least SENSOR_NAME_MIN_ROOM bytes (or all that
// is left after the prefix, if less); the tail is then copied as far as it fits, at a UTF-8
// boundary. out is always NUL-terminated when outLen > 0. Returns true when the whole tail fit.
//   composeSensorText(msg, sizeof(msg), "", "Silas Cox", "Wellhead", 0, " high alarm 34.4 in")
//     -> "Silas Cox Wellhead high alarm 34.4 in"
static inline bool composeSensorText(char *out, size_t outLen, const char *prefix,
                                     const char *site, const char *label, uint8_t userNumber,
                                     const char *tail) {
  const char *t = (tail != NULL) ? tail : "";
  if (out == NULL || outLen == 0) {
    return t[0] == '\0';
  }
  const size_t room = outLen - 1;
  size_t pos = utf8FitLen(prefix, room);
  if (pos > 0) {
    memcpy(out, prefix, pos);
  }
  const size_t left = room - pos;
  const size_t tailLen = strlen(t);

  // Signed, so a tail longer than the buffer cannot wrap the name's room around.
  ptrdiff_t nameRoom = (ptrdiff_t)left - (ptrdiff_t)tailLen;
  const ptrdiff_t minRoom =
      ((ptrdiff_t)left < SENSOR_NAME_MIN_ROOM) ? (ptrdiff_t)left : (ptrdiff_t)SENSOR_NAME_MIN_ROOM;
  if (nameRoom < minRoom) {
    nameRoom = minRoom;
  }

  char nameBuf[64];
  const size_t cap =
      ((size_t)nameRoom + 1 < sizeof(nameBuf)) ? (size_t)nameRoom + 1 : sizeof(nameBuf);
  const size_t nameLen = formatSensorName(nameBuf, cap, site, label, userNumber);
  memcpy(out + pos, nameBuf, nameLen);
  pos += nameLen;

  const size_t tailFit = utf8FitLen(t, room - pos);
  memcpy(out + pos, t, tailFit);
  pos += tailFit;
  out[pos] = '\0';
  return tailFit == tailLen;
}

// The SNOOZED notice's hint, and the short one used when the whole notice does not fit (CR-8):
// a 23-byte contact name with a long alarm type (not_triggered, sensor-fault) or reading cut
// " to resume now." (or more) off the end. The short hint keeps "Reply UNSNOOZE" whole.
#define SNOOZE_HINT " Auto-resumes on recovery; reply UNSNOOZE to resume now."
#define SNOOZE_HINT_SHORT " Reply UNSNOOZE to resume."

// Writes the one-time SNOOZED/RESUMED notice broadcastSnoozeChange sends:
//   "SNOOZED: " + NAME + " reminders paused by <who>. Still in <type> alarm (<reading>)." + hint
//   "RESUMED: " + NAME + " reminders active again by <who>. Still in <type> alarm (<reading>)."
// reading is "34.4 in" for an analog sensor or the state ("ON"/"OFF") for a float. When the
// SNOOZED text with SNOOZE_HINT does not fit outLen it is composed again with SNOOZE_HINT_SHORT.
// Returns true when the whole text fit (composeSensorText's result).
static inline bool composeSnoozeText(char *out, size_t outLen, bool snoozed, const char *site,
                                     const char *label, uint8_t userNumber, const char *who,
                                     const char *alarmType, const char *reading) {
  char tail[160];
  const char *prefix = snoozed ? "SNOOZED: " : "RESUMED: ";
  snprintf(tail, sizeof(tail), " reminders %s by %s. Still in %s alarm (%s).%s",
           snoozed ? "paused" : "active again", (who != NULL) ? who : "",
           (alarmType != NULL) ? alarmType : "", (reading != NULL) ? reading : "",
           snoozed ? SNOOZE_HINT : "");
  const bool fit = composeSensorText(out, outLen, prefix, site, label, userNumber, tail);
  if (fit || !snoozed) {
    return fit;
  }
  snprintf(tail, sizeof(tail), " reminders %s by %s. Still in %s alarm (%s).%s", "paused",
           (who != NULL) ? who : "", (alarmType != NULL) ? alarmType : "",
           (reading != NULL) ? reading : "", SNOOZE_HINT_SHORT);
  return composeSensorText(out, outLen, prefix, site, label, userNumber, tail);
}

#endif  // TANKALARM_SENSOR_NAME_H
