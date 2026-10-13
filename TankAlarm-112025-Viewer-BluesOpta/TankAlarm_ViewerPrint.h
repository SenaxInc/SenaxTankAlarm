// TankAlarm_ViewerPrint.h — value lines of the Viewer's printed daily report.
//
// The printout states each sensor's reading by the same rules as the dashboard cards
// (VIEWER_DASHBOARD_HTML renderDataCard): a fault replaces the value, a float switch
// (sensorType "digital") reads ON/OFF, a tank with no unit reads in feet and inches, and
// any other sensor reads its value with its unit ("inches" shortened to "in"). Plain C++,
// no Arduino dependencies, so tests/host/viewer_print can compile it.
#ifndef TANKALARM_VIEWER_PRINT_H
#define TANKALARM_VIEWER_PRINT_H

#include <math.h>
#include <stdio.h>
#include <string.h>

// Unit shown after a value: the measurement unit ("inches" -> "in"), else the object
// type's default unit (gas psi, rpm rpm, flow gpm, anything else in).
static inline const char *viewerPrintUnit(const char *measurementUnit, const char *objectType) {
  if (measurementUnit && measurementUnit[0]) {
    return strcmp(measurementUnit, "inches") == 0 ? "in" : measurementUnit;
  }
  if (objectType && strcmp(objectType, "gas") == 0) return "psi";
  if (objectType && strcmp(objectType, "rpm") == 0) return "rpm";
  if (objectType && strcmp(objectType, "flow") == 0) return "gpm";
  return "in";
}

static inline bool viewerPrintIsDigital(const char *sensorType) {
  return sensorType && strcmp(sensorType, "digital") == 0;
}

// True when the value prints in feet and inches: a tank (or untyped) sensor with no unit.
static inline bool viewerPrintFeetInches(const char *sensorType, const char *measurementUnit,
                                         const char *objectType) {
  return !viewerPrintIsDigital(sensorType) && !(measurementUnit && measurementUnit[0]) &&
         (!(objectType && objectType[0]) || strcmp(objectType, "tank") == 0);
}

// Writes the row label ("Level:", "Switch:" or "Value:", padded to 8) and the reading.
static inline void viewerPrintValueLine(const char *objectType, const char *sensorType,
                                        const char *measurementUnit, float value,
                                        const char *fault, char *label, size_t labelLen,
                                        char *out, size_t outLen) {
  const bool digital = viewerPrintIsDigital(sensorType);
  const bool tank = !(objectType && objectType[0]) || strcmp(objectType, "tank") == 0;
  snprintf(label, labelLen, "%s", digital ? "Switch: " : (tank ? "Level:  " : "Value:  "));
  if (fault && fault[0]) {
    snprintf(out, outLen, "FAULT (%s)", fault);
  } else if (!isfinite(value)) {
    snprintf(out, outLen, "--");
  } else if (digital) {
    snprintf(out, outLen, "%s", value > 0.5f ? "ON" : "OFF");
  } else if (viewerPrintFeetInches(sensorType, measurementUnit, objectType)) {
    if (value < 0.0f) {
      snprintf(out, outLen, "--");
    } else {
      int feet = (int)(value / 12.0f);
      float remIn = value - (float)(feet * 12);
      snprintf(out, outLen, "%d' %.1f\"", feet, remIn);
    }
  } else {
    snprintf(out, outLen, "%.1f %s", value, viewerPrintUnit(measurementUnit, objectType));
  }
}

// Writes the 24 h change ("+1.2\"" for feet-and-inches rows, "+1.2 psi" otherwise).
// Returns false when the row has no change line (a float switch has none).
static inline bool viewerPrintChangeLine(const char *objectType, const char *sensorType,
                                         const char *measurementUnit, float change,
                                         char *out, size_t outLen) {
  if (viewerPrintIsDigital(sensorType) || !isfinite(change)) return false;
  if (viewerPrintFeetInches(sensorType, measurementUnit, objectType)) {
    snprintf(out, outLen, "%+.1f\"", change);
  } else {
    snprintf(out, outLen, "%+.1f %s", change, viewerPrintUnit(measurementUnit, objectType));
  }
  return true;
}

#endif  // TANKALARM_VIEWER_PRINT_H
