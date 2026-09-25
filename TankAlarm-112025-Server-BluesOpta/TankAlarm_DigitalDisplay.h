// TankAlarm_DigitalDisplay.h - how the server shows digital (float switch) sensors (C-A04 B4-B8,
// relay/float project S3). Pure: ArduinoJson and C headers only, host-tested in
// tests/host/digital_display.
//
// A float's reading is 1.0 (activated) or 0.0 (not activated); the dashboard shows it as ON/OFF
// with the threshold of formatSwitch (value > 0.5). Its alarm types are "triggered" and
// "not_triggered", both carried on the client's high latch.

#ifndef TANKALARM_DIGITAL_DISPLAY_H
#define TANKALARM_DIGITAL_DISPLAY_H

#include <stdint.h>
#include <string.h>
#include <ArduinoJson.h>

static inline bool isDigitalSensorType(const char *sensorType) {
  return sensorType != nullptr && strcmp(sensorType, "digital") == 0;
}

// Same threshold as the dashboard's formatSwitch() (value > 0.5). This is for SMS and email text,
// where a NaN reads as OFF; the pages never get NaN (ArduinoJson writes it as null) and show '-'
// for a missing value.
static inline const char *digitalStateText(float value) {
  return (value > 0.5f) ? "ON" : "OFF";
}

// Sensor type to classify a missed alarm with in the daily reconcile: the first non-empty of
// reportSt (the "st" of this report's sensors[] entry for the same k), configSensor (the sensor's
// "sensor" in the cached config) and recordType (the server's record, which can be empty or stale
// until the report's sensors[] loop refreshes it). Only its digital-ness is used: the config and
// "st" spell some analog types differently ("current" vs "currentLoop").
static inline const char *dailyReconcileSensorType(const char *reportSt, const char *configSensor,
                                                   const char *recordType) {
  if (reportSt != nullptr && reportSt[0] != '\0') return reportSt;
  if (configSensor != nullptr && configSensor[0] != '\0') return configSensor;
  return (recordType != nullptr) ? recordType : "";
}

// Alarm type to record when the daily report shows an alarm the server missed.
// 1. The note type ("y", added by CL-5) is trusted when the sensor type is digital or not known
//    yet, whichever channel latched: a conforming client sends "y" only on a float's alarms[]
//    entry, and a float latches only on the high channel (for "triggered" and "not_triggered"
//    alike), so "y" always comes with hi:true. Gating "y" on highLatched would only change the
//    impossible lo:true case, and would then record a float as "low". A stray float type on a
//    known non-digital sensor is ignored and falls through to high/low.
// 2. With no usable "y" (older clients), a digital sensor's high latch takes its type from
//    configTrigger, the sensor's "digitalTrigger" in the client's cached config: "not_activated"
//    gives "not_triggered", anything else (including an unknown config) "triggered", the client
//    default. This fallback needs highLatched because a lone low latch is never a float.
// 3. Anything else keeps high/low.
static inline const char *dailyReconcileAlarmType(const char *y, const char *sensorType, bool highLatched,
                                                  const char *configTrigger) {
  const bool typeUnknown = (sensorType == nullptr || sensorType[0] == '\0');
  if (y != nullptr && (strcmp(y, "triggered") == 0 || strcmp(y, "not_triggered") == 0) &&
      (typeUnknown || isDigitalSensorType(sensorType))) {
    return y;
  }
  if (highLatched && isDigitalSensorType(sensorType)) {
    return (configTrigger != nullptr && strcmp(configTrigger, "not_activated") == 0) ? "not_triggered"
                                                                                    : "triggered";
  }
  return highLatched ? "high" : "low";
}

// R03: whether a daily report's sensors[] entry is filed in history. The test is that a value is
// present, never the value itself: a real 0 (empty tank at 4.00 mA, 0 psi, float OFF, engine at
// 0 rpm) is data and is admitted; a missing reading is a gap and stays out (history is never
// filled). hasT: the entry carries its own acquisition time "t" (no fallback to the report time).
// trustLevel: handleDaily's Fix 8 check (not a faulted/reused current-loop value). A current-loop
// entry needs a raw mA ("ma"/"sensorMa") within 4-20 mA, as in handleTelemetry (a failed read
// carries "fault" and no "ma"); any other type needs "lvl", "fl" or "rm". Re-filing an old value
// is prevented by the per-sensor "t" and the history ring's acquisition-time dedupe.
static inline bool dailyReadingAdmissible(const char *sensorType, bool hasT, bool trustLevel,
                                          bool maPresent, bool maInRange, bool valuePresent) {
  if (!hasT || !trustLevel) return false;
  if (sensorType != nullptr && strcmp(sensorType, "currentLoop") == 0) return maPresent && maInRange;
  return valuePresent;
}

// True when an alarm note carries a reading. Diagnostic, sensor-recovered and config-push clear
// notes carry none; resolveLevel() returns 0 for them, which must not overwrite the last value.
static inline bool alarmNoteCarriesValue(JsonObjectConst note) {
  return !note["lvl"].isNull() || !note["fl"].isNull() || !note["rm"].isNull() ||
         !note["ma"].isNull() || !note["sensorMa"].isNull();
}

// For the stale "sensor-stuck" self-clear: stuck detection counts as disabled for this config
// sensor when it is a float (floats are exempt: a float that holds its state is normal) or when the
// flag is off. A missing flag keeps today's meaning (disabled).
static inline bool configSensorStuckDisabled(JsonObjectConst sensorCfg) {
  const char *sensor = sensorCfg["sensor"] | "";
  if (isDigitalSensorType(sensor)) return true;
  return !(sensorCfg["stuckDetection"] | false);
}

#endif  // TANKALARM_DIGITAL_DISPLAY_H
