// TankAlarm_SensorName.h - how the server keeps a sensor's Display Number ("un", the "Display
// Number" box on the client config page) in step with the client. Pure: C headers only,
// host-tested in tests/host/sensor_name.
//
// The client stamps "un" on every telemetry note and on every daily-report sensors[] entry while
// the sensor's Display Number is set (1-255), and omits it when the box is blank (0):
//   sendTelemetry():      if (cfg.userNumber > 0) doc["un"] = cfg.userNumber;
//   appendDailyMonitor(): if (cfg.userNumber > 0) t["un"] = cfg.userNumber;
// So on those notes a missing "un" means "no Display Number", and a Display Number that was set
// and later cleared must go back to 0 on the server. Writing only when "un" is present left the
// old number in the registry (and in "#N" on emails and the dashboard) forever.
//
// Alarm and unload notes do not always carry "un" in the same way, so their handlers keep the
// old rule (write only when present) and do not use this helper.

#ifndef TANKALARM_SENSOR_NAME_H
#define TANKALARM_SENSOR_NAME_H

#include <stddef.h>
#include <stdint.h>

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

#endif  // TANKALARM_SENSOR_NAME_H
