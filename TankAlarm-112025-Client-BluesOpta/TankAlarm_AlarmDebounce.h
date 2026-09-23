// TankAlarm_AlarmDebounce.h - pure alarm debounce and rate-window helpers for the client (C-T01).
//
// Includes only <stdint.h>: no Arduino, mbed or Notecard dependencies, so
// tests/host/alarm_debounce compiles this exact file with a PC compiler and exercises the same
// code the firmware runs (the millis() wrap cases cannot be reproduced on a bench). It lives in
// the client sketch folder, not Common, because only the client uses it.

#ifndef TANKALARM_ALARM_DEBOUNCE_H
#define TANKALARM_ALARM_DEBOUNCE_H

#include <stdint.h>

// Edge reported by alarmDebounceStep() for one latch channel.
#define ALARM_EDGE_NONE  0
#define ALARM_EDGE_ENTER 1
#define ALARM_EDGE_EXIT  2

// One latch channel. Only CONSECUTIVE qualifying samples count: a sample that does not qualify
// resets the active direction's counter, and the inactive direction's counter is held at 0.
// Unlatched, `trigger` samples count towards ENTER; latched, `release` samples count towards
// EXIT. `need` is the number of consecutive samples (0 behaves like 1). The caller owns the
// latch and applies the returned edge. A time-qualified or per-monitor variant can extend this
// without changing callers.
static inline uint8_t alarmDebounceStep(bool latched, bool trigger, bool release, uint8_t need,
                                        uint8_t &enterCount, uint8_t &exitCount) {
  if (!latched) {
    exitCount = 0;
    if (!trigger) {
      enterCount = 0;
      return ALARM_EDGE_NONE;
    }
    if (enterCount < 255) enterCount++;
    if (enterCount >= need) {
      enterCount = 0;
      return ALARM_EDGE_ENTER;
    }
    return ALARM_EDGE_NONE;
  }
  enterCount = 0;
  if (!release) {
    exitCount = 0;
    return ALARM_EDGE_NONE;
  }
  if (exitCount < 255) exitCount++;
  if (exitCount >= need) {
    exitCount = 0;
    return ALARM_EDGE_EXIT;
  }
  return ALARM_EDGE_NONE;
}

// Per-sample analog comparisons, identical to v2.2.15 evaluateAlarms(). A negative hysteresis
// is clamped to 0 (it would invert the clear bands). Each alarm clears on its own side of its
// own threshold: a shared mid-band clear condition was empty when (high - low) <= 2*hysteresis,
// so a latched alarm could never clear. A NaN reading makes every flag false, so callers must
// not pass invalid samples here (the client skips sampleReused samples).
struct AlarmAnalogConditions {
  bool high;         // x >= high threshold
  bool low;          // x <= low threshold
  bool highRelease;  // x <  high threshold - hysteresis
  bool lowRelease;   // x >  low threshold + hysteresis
};

static inline AlarmAnalogConditions alarmAnalogConditions(float x, float highThreshold,
                                                          float lowThreshold, float hysteresis) {
  const float hyst = (hysteresis > 0.0f) ? hysteresis : 0.0f;
  AlarmAnalogConditions c;
  c.high = (x >= highThreshold);
  c.low = (x <= lowThreshold);
  c.highRelease = (x < highThreshold - hyst);
  c.lowRelease = (x > lowThreshold + hyst);
  return c;
}

// Two-channel (high/low) analog evaluation: updates the latches and counters in place and
// reports each channel's edge. The caller notifies in the order high edge, then low edge, as
// v2.2.15 did. A sample inside both trigger zones (low threshold >= high threshold, a
// misconfiguration) is evidence for neither side, which keeps v2.2.15's never-latch behaviour
// there. Entering one side unlatches the other, as v2.2.15 did.
static inline void alarmAnalogEvaluate(const AlarmAnalogConditions &c, uint8_t need,
                                       bool &highLatched, bool &lowLatched,
                                       uint8_t &highEnterCount, uint8_t &highExitCount,
                                       uint8_t &lowEnterCount, uint8_t &lowExitCount,
                                       uint8_t &highEdge, uint8_t &lowEdge) {
  highEdge = alarmDebounceStep(highLatched, c.high && !c.low, c.highRelease, need,
                               highEnterCount, highExitCount);
  if (highEdge == ALARM_EDGE_ENTER) {
    highLatched = true;
    lowLatched = false;
  } else if (highEdge == ALARM_EDGE_EXIT) {
    highLatched = false;
  }
  lowEdge = alarmDebounceStep(lowLatched, c.low && !c.high, c.lowRelease, need,
                              lowEnterCount, lowExitCount);
  if (lowEdge == ALARM_EDGE_ENTER) {
    lowLatched = true;
    highLatched = false;
  } else if (lowEdge == ALARM_EDGE_EXIT) {
    lowLatched = false;
  }
}

// Retry decision for a latch edge whose note the rate limiter denied (the client's pending
// slot), taken after a valid sample's own edges.
#define ALARM_RETRY_DROP 0  // the edge's latch was released: forget the edge
#define ALARM_RETRY_HOLD 1  // still latched, but this sample is not in alarm: keep the edge
#define ALARM_RETRY_SEND 2  // still latched and this sample is in alarm: re-send the edge

// `latched` is the edge's latch after this sample; `inAlarm` says whether this sample still
// supports the edge: analog, not in that latch's release zone (the test that holds the latch,
// so the hysteresis band counts); digital, the alarm state. The latch alone is not enough: it
// holds through up to need - 1 release samples and its counters restart after a sensor
// failure, so re-sending from a released sample would be a false alarm (a deferred HIGH sent
// at 30 in after the tank was pumped down during an outage). HOLD keeps the edge, so one
// noisy sample never loses a real alarm; if the latch then clears, its clear supersedes it.
static inline uint8_t alarmPendingRetryAction(bool latched, bool inAlarm) {
  if (!latched) return ALARM_RETRY_DROP;
  return inAlarm ? ALARM_RETRY_SEND : ALARM_RETRY_HOLD;
}

// True when the millis() stamp `ts` lies within the last `windowMs` milliseconds of `now`.
// Modular subtraction keeps this correct across the 49.7-day millis() wrap.
static inline bool alarmWithinWindow(uint32_t now, uint32_t ts, uint32_t windowMs) {
  return (uint32_t)(now - ts) < windowMs;
}

#endif  // TANKALARM_ALARM_DEBOUNCE_H
