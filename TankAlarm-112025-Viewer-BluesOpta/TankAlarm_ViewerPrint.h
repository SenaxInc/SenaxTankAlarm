// TankAlarm_ViewerPrint.h — value and status lines of the Viewer's printed daily report.
//
// The printout states each sensor's reading by the same rules as the dashboard cards
// (VIEWER_DASHBOARD_HTML renderDataCard, unitLabel, formatValue, formatLevel): a fault
// replaces the value, a float switch (sensorType "digital") reads ON/OFF, a tank with no
// unit reads in feet and inches (inches alone under a foot), and any other sensor reads
// its value with its unit (inches/psi/rpm/gpm in any case shortened to in/psi/rpm/gpm).
// Plain C++, no Arduino dependencies, so tests/host/viewer_print can compile it.
#ifndef TANKALARM_VIEWER_PRINT_H
#define TANKALARM_VIEWER_PRINT_H

#include <math.h>
#include <stdio.h>
#include <string.h>

static inline bool viewerPrintHas(const char *s) {
  return s && s[0] != '\0';
}

// ASCII case-insensitive equality (the dashboard lowercases a unit before mapping it).
static inline bool viewerPrintSameText(const char *a, const char *b) {
  for (; *a && *b; ++a, ++b) {
    char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
    char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
    if (ca != cb) return false;
  }
  return *a == *b;
}

// Unit shown after a value: the measurement unit (inches/psi/rpm/gpm in any case as
// in/psi/rpm/gpm, anything else as sent), else the object type's default unit (gas psi,
// rpm rpm, flow gpm, anything else in).
static inline const char *viewerPrintUnit(const char *objectType, const char *measurementUnit) {
  if (viewerPrintHas(measurementUnit)) {
    if (viewerPrintSameText(measurementUnit, "inches")) return "in";
    if (viewerPrintSameText(measurementUnit, "psi")) return "psi";
    if (viewerPrintSameText(measurementUnit, "rpm")) return "rpm";
    if (viewerPrintSameText(measurementUnit, "gpm")) return "gpm";
    return measurementUnit;
  }
  if (viewerPrintHas(objectType) && strcmp(objectType, "gas") == 0) return "psi";
  if (viewerPrintHas(objectType) && strcmp(objectType, "rpm") == 0) return "rpm";
  if (viewerPrintHas(objectType) && strcmp(objectType, "flow") == 0) return "gpm";
  return "in";
}

static inline bool viewerPrintIsDigital(const char *sensorType) {
  return viewerPrintHas(sensorType) && strcmp(sensorType, "digital") == 0;
}

// A record with no object type is a tank (the dashboard defaults ot to "tank").
static inline bool viewerPrintIsTank(const char *objectType) {
  return !viewerPrintHas(objectType) || strcmp(objectType, "tank") == 0;
}

// True when the value prints in feet and inches: a tank sensor with no unit.
static inline bool viewerPrintFeetInches(const char *objectType, const char *sensorType,
                                         const char *measurementUnit) {
  return !viewerPrintIsDigital(sensorType) && viewerPrintIsTank(objectType) &&
         !viewerPrintHas(measurementUnit);
}

// Writes the row label ("Level:", "Switch:" or "Value:", padded to 8) and the reading.
static inline void viewerPrintValueLine(const char *objectType, const char *sensorType,
                                        const char *measurementUnit, float value,
                                        const char *fault, char *label, size_t labelLen,
                                        char *out, size_t outLen) {
  const bool digital = viewerPrintIsDigital(sensorType);
  snprintf(label, labelLen, "%s",
           digital ? "Switch: " : (viewerPrintIsTank(objectType) ? "Level:  " : "Value:  "));
  if (viewerPrintHas(fault)) {
    snprintf(out, outLen, "FAULT (%s)", fault);
  } else if (!isfinite(value)) {
    snprintf(out, outLen, "--");
  } else if (digital) {
    snprintf(out, outLen, "%s", value > 0.5f ? "ON" : "OFF");
  } else if (viewerPrintFeetInches(objectType, sensorType, measurementUnit)) {
    if (value < 0.0f) {
      snprintf(out, outLen, "--");
    } else {
      // Float arithmetic, like formatLevel: no int overflow on an out-of-range reading.
      const float feet = floorf(value / 12.0f);
      const float remIn = fmodf(value, 12.0f);
      if (feet > 0.0f) {
        snprintf(out, outLen, "%.0f' %.1f\"", feet, remIn);
      } else {
        snprintf(out, outLen, "%.1f\"", remIn);
      }
    }
  } else {
    snprintf(out, outLen, "%.1f %s", value, viewerPrintUnit(objectType, measurementUnit));
  }
}

// Writes the 24 h change ("+1.2\"" for feet-and-inches rows, "+1.2 psi" otherwise).
// Returns false when the row has no change line (a float switch has none).
static inline bool viewerPrintChangeLine(const char *objectType, const char *sensorType,
                                         const char *measurementUnit, float change,
                                         char *out, size_t outLen) {
  if (viewerPrintIsDigital(sensorType) || !isfinite(change)) return false;
  if (viewerPrintFeetInches(objectType, sensorType, measurementUnit)) {
    snprintf(out, outLen, "%+.1f\"", change);
  } else {
    snprintf(out, outLen, "%+.1f %s", change, viewerPrintUnit(objectType, measurementUnit));
  }
  return true;
}

// Writes the status: the alarm type with *ALARM* (and "(reminders snoozed)"), else
// "Sensor fault" while the reading is a fault, else "Normal".
static inline void viewerPrintStatusLine(bool alarmActive, const char *alarmType, bool snoozed,
                                         const char *fault, char *out, size_t outLen) {
  if (alarmActive) {
    snprintf(out, outLen, "%s  *ALARM*%s", viewerPrintHas(alarmType) ? alarmType : "ALARM",
             snoozed ? " (reminders snoozed)" : "");
  } else if (viewerPrintHas(fault)) {
    snprintf(out, outLen, "Sensor fault");
  } else {
    snprintf(out, outLen, "Normal");
  }
}

#endif  // TANKALARM_VIEWER_PRINT_H
