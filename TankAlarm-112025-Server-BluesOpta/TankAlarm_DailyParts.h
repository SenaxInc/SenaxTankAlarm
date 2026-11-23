// TankAlarm_DailyParts.h - which part of a multi-part daily report is its first part (S-D08).
//
// Includes only <stdint.h>: no Arduino, mbed or ArduinoJson dependencies, so
// tests/host/daily_parts compiles this exact file with a PC compiler and exercises the same
// rule the server firmware runs.
//
// A client splits a daily report that does not fit one note into parts ("p"). The first part
// alone carries the report-wide block: battery voltage ("v", "solar"), signal ("sig") and the
// active alarm list ("alarms"). handleDaily() reads that block, and runs the alarm reconcile
// that clears high/low alarms the report does not list, only on the first part.
//
// Part numbering by client version (git history of the client's sendDailyReport()):
//   - until 2025-12-22 (before the first tagged release, v0.9): 1-based, in the key "part";
//     no "_sv". The server stopped reading "part" in v1.1.7, so every part of such a report
//     now reads as p = 0.
//   - since then (v0.9 and every later release): 0-based, in "p".
//   - "_sv" (NOTEFILE_SCHEMA_VERSION) was added in v1.1.6 (2026-03-12), when parts had
//     already been 0-based for almost three months. It was 2 in v1.7.2 to v1.8.4 and is 1 in
//     every other release.
// No client version stamps "_sv" and numbers its parts from 1, so in a note that carries a
// schema version, part 1 is always a second part.

#ifndef TANKALARM_DAILY_PARTS_H
#define TANKALARM_DAILY_PARTS_H

#include <stdint.h>

// True when `part` (the note's "p", 0 when absent) is the first part of its daily report.
// `schemaVersion` is the note's "_sv", 0 when absent.
//   - schemaVersion >= 1: only part 0. Part 1 has no "alarms" array, and on a schema 2+ note a
//     first part without one means "no active alarms", so treating part 1 as a first part would
//     clear every high/low alarm that part 0 had just confirmed. (The server drops a note whose
//     "_sv" is above its own NOTEFILE_SCHEMA_VERSION before handleDaily() sees it, so this is
//     reachable as soon as that constant is 2 or more, as it was in v1.8.0 to v1.8.4.)
//   - schemaVersion < 1 (a note without a schema version): parts 0 and 1, the rule the server
//     has always applied to these notes. It is kept so their handling does not change; such a
//     part 1 carries no voltage, signal or alarms, and without a schema version the reconcile
//     runs only when the note has an "alarms" array.
static inline bool dailyIsFirstPart(int schemaVersion, uint8_t part) {
  if (schemaVersion >= 1) {
    return part == 0;
  }
  return part == 0 || part == 1;
}

#endif  // TANKALARM_DAILY_PARTS_H
