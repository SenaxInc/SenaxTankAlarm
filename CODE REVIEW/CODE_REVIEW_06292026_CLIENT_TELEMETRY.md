# Code Review: Client Telemetry Interval and Daily Report Behavior Analysis

**Date:** June 29, 2026  
**Analyst:** GitHub Copilot  
**Scope:** Thorough investigation of client-side telemetry schedule, the missing telemetry files issue, and the mechanics behind `daily.qo` reports.

---

## 1. Executive Summary

This code review addresses three distinct concerns raised regarding the client's transmission of telemetry and daily report files:

1. **How often should the client be sending telemetry files (`telemetry.qo`)?**  
   The client does **not** send `telemetry.qo` files on a regular time-based schedule (like every 30 minutes). Instead, it uses **change-based reporting**. It only publishes a new `telemetry.qo` note when a sensor reading has changed by more than or equal to the configured `reportThreshold` (since the last reported value), or immediately following a system boot as a baseline. If the measured value is completely stable — a quiescent tank level, a pressure sensor sitting at a steady PSI, an RPM input on a stopped engine, a flow meter reading 0 GPM, or a digital float switch that has not toggled — and there are no active alarms (which go to `alarm.qo`, not `telemetry.qo`), the client will **never** send telemetry files, resulting in long quiet periods between daily reports.

2. **Why hasn't a telemetry file been sent in a few days (only the `daily.qo` at midnight)?**  
   There are three potential reasons, with **Reason A/B** being the most critical:
   - **Critical Bug (Reason A):** If the monitor's `reportThreshold` parameter is set to `0.0` (which is the default `DEFAULT_LEVEL_CHANGE_THRESHOLD` and is documented in `TankAlarm_Config.h` comments as "send all readings"), an implementation bug in the change-exceeded check disables change-based telemetry entirely. This applies to **every sensor interface and object type** — level, pressure, RPM, flow, gas, digital — because the threshold check is shared logic.
   - **Stable Reading State (Reason B):** If `reportThreshold` is set to a non-zero value but the measured value is at steady-state and any fluctuations stay within the delta band, no change is detected and telemetry is suppressed. This includes: a tank level that has not been topped off or drawn down, a pressure sensor on a closed/sealed system, an engine sitting idle (RPM = 0), a flow meter on a closed valve, a gas regulator holding pressure, or a float switch that has not toggled.
   - **Hardware/Sensor Fault (Reason C):** If the sensor fails or power gating prevents stable reads, the change-exceeded check is bypassed entirely (the surrounding `!sensorFailed` guard short-circuits it), and telemetry is suppressed until successful sampling is restored.

3. **Is the client gathering a new sensor reading for the `daily.qo` report, or simply resending the last measurement from days before?**  
   The client **does not** perform an active, on-demand sensor acquisition inside the `sendDailyReport` routine. Instead, it serializes and packages whatever reading is currently cached in the per-monitor memory state. The cache field is named `currentInches` for historical reasons but holds *whatever the monitor measures* — inches for level, PSI for pressure, RPM for engines, GPM for flow, etc.
   - **Under Healthy Conditions:** The cached state is updated every 30 minutes (or 1 to 2 hours in power-saving modes) by the independent sampling loop. Thus, the daily report value is highly fresh (at most 30–120 minutes old).
   - **Under Failed Conditions:** If the sensor has experienced I2C bus locks, brownouts, or general read failures for days, the sampling loop will have failed to update the cache. The cached state will retain the *last successfully acquired reading* from days ago. In this case, the daily report **resends that identical old measurement**, but packages it with `sf: 1` (Sensor Failed) and `ru: 1` (Sample Reused/Stale) in the daily JSON model to warn the server that the reading is stale and the link is impaired.

---

## 2. Telemetry Generation Logic & Threshold Bug

### 2.1 The Implementation of Telemetry Checks
In [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5793-L5801) and [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5824-L5832), the client checks if it needs to dispatch a `telemetry.qo` payload:

```cpp
if (gConfig.monitors[i].enableServerUpload && !gMonitorState[i].sensorFailed) {
  const float threshold = gConfig.monitors[i].reportThreshold;
  const bool needBaseline = (gMonitorState[i].lastReportedValue < 0.0f);
  const bool thresholdEnabled = (threshold > 0.0f);
  const bool changeExceeded = thresholdEnabled && (fabs(inches - gMonitorState[i].lastReportedValue) >= threshold);
  if (needBaseline || changeExceeded) {
    sendTelemetry(i, "sample", false);
    gMonitorState[i].lastReportedValue = inches;
  }
}
```

This enforces that:
* Telemetry is **only** initiated if `enableServerUpload` is active and the sensor is currently marking a valid (non-failed) state.
* `needBaseline` triggers a single transmission immediately after boot / client reset.
* Non-baseline telemetry is strictly conditional on `changeExceeded`.

### 2.2 The Threshold Bug Mismatch
A significant mismatch exists between the documentation and the firmware logic:
* **The Config Comment:** In [TankAlarm-112025-Common/src/TankAlarm_Config.h](TankAlarm-112025-Common/src/TankAlarm_Config.h#L38), the threshold parameter is defined:
  ```cpp
  // Minimum level change threshold in inches (0 = send all readings)
  #ifndef DEFAULT_LEVEL_CHANGE_THRESHOLD
  #define DEFAULT_LEVEL_CHANGE_THRESHOLD 0.0f
  #endif
  ```
* **The Migration Guide:** In [TankAlarm-112025-Client-BluesOpta/MIGRATION_GUIDE.md](TankAlarm-112025-Client-BluesOpta/MIGRATION_GUIDE.md#L278), it suggests:
  ```
  Leave it at 0 to disable change-based telemetry for that sensor and rely on alarms/daily reports...
  ```
* **The Code Consequence:** Because of `thresholdEnabled = (threshold > 0.0f)`, setting the threshold to `0.0` means `thresholdEnabled` is evaluated as `false`. Thus, `changeExceeded` is always `false`. 
* **Impact:** Setting `reportThreshold` to `0.0` does **not** "send all readings" periodically as the `TankAlarm_Config.h` comment claims. It completely **shuts off change-based telemetry** for that monitor. The device will only send a single `telemetry.qo` note at bootup, then keep silent until the next scheduled `daily.qo` runs. The migration guide at [TankAlarm-112025-Client-BluesOpta/MIGRATION_GUIDE.md](TankAlarm-112025-Client-BluesOpta/MIGRATION_GUIDE.md#L278) describes the *actual* behavior correctly — only the inline header comment is wrong and needs to be fixed.
* **Universality:** This bug is not specific to liquid level sensors. The exact same code path runs for every `SensorInterface` (digital, analog, current-loop, pulse) and every `ObjectType` (tank, engine, pump, gas, flow). A `reportThreshold: 0` on an engine RPM monitor, a pressure-sensor pump monitor, or a gas-line pressure transducer behaves identically: the boot baseline note is published, then the device falls silent until the daily report.

### 2.3 Alarms Are Segregated
If a high-level or low-level alert occurs, the client publishes an event immediately. However, it does not write to `telemetry.qo`. Instead, it writes to `alarm.qo` (referenced using the compiled macro `ALARM_FILE` defined in [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L195)). This ensures system alerts override standard delta restrictions, but keeps standard telemetry logs separate.

---

## 3. Daily Report Acquisition Mechanics

### 3.1 Uncoupling of Daily Reports and Sensor Sampling
When the client reaches the scheduled hour/minute (standardly 5:00 AM local time per default configuration but often set or customized to midnight), the schedule triggers `sendDailyReport()`. 

Looking at the main loop schedule in [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L2140-L2152):
```cpp
if (gPowerState != POWER_STATE_CRITICAL_HIBERNATE) {
  if (now - gLastTelemetryMillis >= sampleInterval) {
    gLastTelemetryMillis = now;
    if (gConfig.monitorCount > 0) {
      sampleMonitors();
    }
  }
}
```
And the report execution in [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L2434-L2446):
```cpp
if (reportDue) {
  sendDailyReport();
  gSolarOnlyLastReportEpoch = currentEpoch();
  scheduleNextDailyReport();
}
```
The execution of `sendDailyReport` behaves as follows:
* It does **not** call `sampleMonitors()` or read any pins/buses directly on execution.
* It uses the helper [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L8011) (`appendDailyMonitor`), which calls `buildSensorObject` to fetch and serialize `gMonitorState[monitorIndex].currentInches`.

### 3.2 Age of the Daily Measurement Under Healthy Operation
Depending on the client's current power environment state, the sampling loop triggers reads at varying periods:
* **Normal Mode (Grid):** Every 30 minutes (`DEFAULT_SAMPLE_INTERVAL_SEC` = 1800s).
* **ECO Mode:** Every 60 minutes (`POWER_ECO_SAMPLE_MULTIPLIER` = 2x).
* **Low Power Mode:** Every 120 minutes (`POWER_LOW_SAMPLE_MULTIPLIER` = 4x).

Therefore, when the daily report packages the cached values, the reading is **at most** 30–120 minutes old, which represents a highly fresh and accurate daily measurement.

### 3.3 Age of the Daily Measurement Under Stale/Failed Conditions
If the sensor interface (e.g. the I2C-based A0602 Current Loop module) has failed, or if battery drops into critical voltage ranges causing sensor power gating to fail, the sampling loop in `sampleMonitors` behaves as follows:

```cpp
// Validate sensor reading
if (!validateSensorReading(i, inches)) {
  // Keep previous valid reading if sensor failed
  inches = gMonitorState[i].currentInches;
  gMonitorState[i].sampleReused = true;
} else {
  gMonitorState[i].currentInches = inches;
  if (!gMonitorState[i].sampleReused) {
    gMonitorState[i].lastReadingEpoch = sampleEpoch;
  }
}
```

* Because validation fails, `currentInches` stays locked to the last healthy value from days before (regardless of whether that value is inches, PSI, RPM, GPM, or a digital state).
* `sampleReused` is flagged as `true`.
* `lastReadingEpoch` (the hardware-derived acquisition timestamp) is frozen in time.
* When `sendDailyReport()` packages the variables via [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L6117-L6121) (`buildSensorObject`), it serializes:
  ```cpp
  if (state.sensorFailed) o["sf"] = 1;   // sensor currently in a failed/fault state
  if (state.sampleReused) o["ru"] = 1;   // this value was reused from a previous cycle
  ```
* **Conclusion:** If the sensor has been failed for days, the daily report indeed **resends the outdated measurement from days before**. The JSON payload explicitly tags `"ru": 1` and `"sf": 1`, but — critically — the daily report's top-level `t` field is the report *transmission* time, NOT the acquisition time, and the nested per-sensor objects emitted by `buildSensorObject` carry no `t` field at all. The frozen `lastReadingEpoch` is therefore **never transmitted in `daily.qo`** today; it only appears in `telemetry.qo`. This is the root cause of the orphaned-epoch problem analyzed in Section 5.

---

## 4. Diagnosis and Troubleshooting Workflow

To resolve lack of telemetry, follow this priority sequence:

1. **Check the Configured `reportThreshold`:**
   Ensure the threshold is set to a small positive value through the server dashboard instead of `0.0`. The threshold is **always in the monitor's own measurement unit** (`measurementUnit` field), so the right magnitude depends on what the sensor measures:
   * Liquid level (`inches`): `1.0`–`2.0` inches
   * Pressure (`psi`): `0.25`–`1.0` PSI (or coarser for high-pressure systems)
   * Engine speed (`rpm`): `25`–`100` RPM
   * Flow rate (`gpm`): `0.5`–`2.0` GPM
   * Gas pressure / other custom units: pick a value that represents "meaningful change" for the application
   
   A setting of `0.0` disables change-based telemetry entirely for any of the above.
2. **Observe `"ru"` and `"sf"` Flags in the Daily Report (`daily.qo`):**
   * If `"ru": 1` and `"sf": 1` are present, the hardware sampling is failing entirely. Proceed with local terminal bus-recovery checks (e.g., check connections on the A0602 module or expansion buses, validate Hall-effect wiring for pulse sensors, etc.).
   * If no fault or reuse flags are present and successive daily reports show only minor fluctuations, the hardware is fine and the reading has simply stayed within the delta/threshold band — the silence is correct and designed.

---

## 5. Time Elements & Daily Report Robustness Analysis

### 5.1 Telemetry vs. Daily Time Serialization
* **`telemetry.qo`:** This file contains an explicit acquisition timestamp field `t` inside its payload:
  ```cpp
  doc["t"] = (state.lastReadingEpoch > 0.0) ? state.lastReadingEpoch : currentEpoch();
  ```
  This is the precise UNIX epoch of when the physical reading was acquired.
* **`daily.qo`:** The daily report carries a top-level timestamp `t` representing the epoch of the daily report's creation/transmission (`reportEpoch` in `sendDailyReport()`). However, **individual sensor entries** in the nested `sensors` array **do not** include any individual timestamp elements.

### 5.2 The Robustness Gap (Orphaned Epoch Problem)
Because the daily report lacks individual acquisition timestamps inside the nested `sensors` array, the server-side processor in `handleDaily()` can only fall back to the top-level transmittal epoch when updating local entries:
```cpp
double now = (epoch > 0.0) ? epoch : currentEpoch();
...
rec->lastUpdateEpoch = now;
```
This design causes a critical tracking vulnerability under degraded/failed conditions:
1. When a sensor fails, the client marks `"sf": 1`, `"ru": 1` and continues to package the **cached stale reading** from days ago.
2. The server processes the daily report, sees `"ru": 1` and `"sf": 1`, and correctly avoids overwriting the last known-good measurement value (for current-loop sensors).
3. However, the server still updates the database's `lastUpdateEpoch` for that sensor to **the transmittal time (`now`)**. 
4. The dashboard is thus led to falsely report that the sensor had "updated successfully seconds/minutes ago" (since `lastUpdateEpoch` got bumped), even though the underlying data point is actually an un-acquired stale reading from days prior!

For other sensor interfaces (like analog or digital), the server does not have the same current-loop safe-guards and will overwrite the measurement with a stale reading, update `lastUpdateEpoch = now`, and completely wipe out the accurate timestamp history of when the real physical event originally happened.

---

## 6. Recommended Code Improvements

To address this gap and bring the durability of the daily report up to the telemetry standards, we should make the following targeted updates.

### 6.1 Client-Side: Transmit Acquisition Epoch for Nested Monitors
Add a nested acquisition timestamp `"t"` to each sensor object in the `sensors` array of the daily report.

**Recommended location:** inside `appendDailyMonitor()` at [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L8011) **after** the call to `buildSensorObject`, since the existing code path is `array.add<JsonObject>()` → manual fields → `buildSensorObject(t, monitorIndex)`. The nested JsonObject is bound to the local variable `t` (its name conveniently matches the JSON key we want to add):

```cpp
  JsonObject t = array.add<JsonObject>();
  t["n"] = cfg.name;
  t["k"] = cfg.sensorIndex;
  if (cfg.userNumber > 0) t["un"] = cfg.userNumber;

  // Self-describing payload: ot, mu, st, raw reading, lvl, cap, cv, sf, ru
  buildSensorObject(t, monitorIndex);

  // NEW: per-sensor acquisition epoch. Mirrors telemetry.qo's top-level `t`
  // so that the server can distinguish report transmission time from the
  // time the underlying reading was actually acquired. Critical when `ru`/`sf`
  // indicate the daily is republishing a stale cached value.
  if (state.lastReadingEpoch > 0.0) {
    t["t"] = (uint32_t)state.lastReadingEpoch;
  }
```

* **Wire Cost:** Roughly `15` characters per configured monitor (e.g. `"t":1782782400,`). Well within `DAILY_NOTE_PAYLOAD_LIMIT`; the existing `measureJson(doc) > payloadLimit` guard at the bottom of `appendDailyMonitor` already protects against overflow by rejecting the entry.
* **Backward Compatibility:** Older server deployments simply ignore unknown JSON fields, so this change is forward-compatible during a phased rollout.
* **Universality:** This works for every sensor interface and object type because `lastReadingEpoch` is updated by the shared `sampleMonitors` path for all sensors.

### 6.2 Server-Side: Read Nested Acquisition Epoch
Use the nested acquisition timestamp when the client emitted one.

In `handleDaily()` at [TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12524-L12549) (around the `for (JsonObject t : sensors)` loop):

```cpp
    // Resolve the display value (level/PSI/RPM/etc) using the client's
    // self-describing payload + the server's learned calibration.
    float newLevel = resolveLevel(clientUid, sensorIndex, rec->sensorType, t);

    // Prefer the per-sensor acquisition epoch the client transmitted; only
    // fall back to the top-level report epoch (or server-now) when absent.
    double sensorEpoch = t["t"] | 0.0;
    double now = (sensorEpoch > 0.0)
                 ? sensorEpoch
                 : ((epoch > 0.0) ? epoch : currentEpoch());

    // ... existing 22h rollover logic, trustLevel check, etc., unchanged ...

    rec->lastUpdateEpoch = now;
```

* This preserves the precise physical acquisition time in the server's `SensorRecord`, regardless of how delayed the daily reports or sync queues are.
* It completely resolves the "orphaned epoch" bug on the dashboard where stale metrics falsely appear as fresh — for **every** sensor interface, not just current-loop.

### 6.3 Fix the Misleading Comment in `TankAlarm_Config.h`
The inline comment at [TankAlarm-112025-Common/src/TankAlarm_Config.h](TankAlarm-112025-Common/src/TankAlarm_Config.h#L37-L40) directly contradicts the firmware behavior and the migration guide. Replace:

```cpp
// Minimum level change threshold in inches (0 = send all readings)
#ifndef DEFAULT_LEVEL_CHANGE_THRESHOLD
#define DEFAULT_LEVEL_CHANGE_THRESHOLD 0.0f
#endif
```

with:

```cpp
// Minimum change threshold (in each monitor's own measurement unit) required
// before a non-baseline telemetry.qo note is published. Applies to every
// sensor interface and object type — inches for liquid level, PSI for
// pressure, RPM for engines, GPM for flow, etc.
//
// 0 = change-based telemetry DISABLED for that monitor. The client will only
// publish the boot baseline note and then the scheduled daily.qo report;
// alarms still publish to alarm.qo unconditionally.
#ifndef DEFAULT_LEVEL_CHANGE_THRESHOLD
#define DEFAULT_LEVEL_CHANGE_THRESHOLD 0.0f
#endif
```

The `#define` name is left unchanged to preserve compatibility with downstream usages; only the comment is corrected.

---

## 7. Verification Summary & Implementation Plan

### 7.1 Verification Findings (Confirmed Against Live Code)

| Claim | Verified? | Evidence |
|---|---|---|
| `thresholdEnabled = (threshold > 0.0f)` shuts off change-based telemetry when threshold is `0.0` | ✅ | Lines 5793–5801 and 5824–5832 of the client `.ino`; logic appears in both Phase-A (non-current-loop) and Phase-B (current-loop) sampling paths. |
| `DEFAULT_LEVEL_CHANGE_THRESHOLD` default is `0.0f` | ✅ | [TankAlarm-112025-Common/src/TankAlarm_Config.h](TankAlarm-112025-Common/src/TankAlarm_Config.h#L38) |
| `sendDailyReport()` never re-samples; it uses cached state | ✅ | `sendDailyReport` (L7844) calls `appendDailyMonitor` (L8011), which calls `buildSensorObject` (L6056). None of these touch the I2C bus or analog pins. |
| `buildSensorObject` emits `sf`/`ru`/`lvl`/`cap`/`cv` but does **not** emit a per-sensor `t` | ✅ | L6056–L6126; `t` is set only by `sendTelemetry` after `buildSensorObject` returns, so it appears at the *top level of telemetry.qo* and nowhere in `daily.qo`'s nested sensor objects. |
| Server `handleDaily()` sets `rec->lastUpdateEpoch = now` where `now = (epoch > 0.0) ? epoch : currentEpoch()` | ✅ | Lines 12524–12549 of the server `.ino`. |
| The threshold bug affects ALL sensor types, not just liquid level | ✅ | The check is interface-agnostic and runs in the shared `sampleMonitors` loop. `currentInches` is the cache field name but holds whatever the sensor measures (`SensorInterface` x `ObjectType` combinations all flow through it). |
| The current-loop `trustLevel` guard at L12545 protects `levelInches` from stale overwrite, but `lastUpdateEpoch` is still bumped to `now` | ✅ | Confirmed: `rec->lastUpdateEpoch = now;` is outside the `if (trustLevel)` block. |

### 7.2 Implementation Plan

The fix consists of three small, independent edits. Each is backward-compatible and can be merged separately if desired.

#### Step 1 — Client: Emit Per-Sensor Acquisition Epoch (4 lines)
* **File:** [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L8011)
* **Function:** `appendDailyMonitor`
* **Change:** Add `if (state.lastReadingEpoch > 0.0) t["t"] = (uint32_t)state.lastReadingEpoch;` immediately after the `buildSensorObject(t, monitorIndex);` call and before the `measureJson(doc) > payloadLimit` size check.
* **Risk:** None to existing deployments (new optional field).

#### Step 2 — Server: Honor Per-Sensor Acquisition Epoch (2 lines)
* **File:** [TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12524-L12549)
* **Function:** `handleDaily`
* **Change:** Inside the `for (JsonObject t : sensors)` loop, replace `double now = (epoch > 0.0) ? epoch : currentEpoch();` with a three-tier preference: nested `t["t"]` → top-level `epoch` → `currentEpoch()`. Keep the existing 22h rollover, `trustLevel`, and snapshot logic untouched.
* **Risk:** None. Old clients (no nested `t`) take the existing top-level fallback path, behavior unchanged.

#### Step 3 — Fix Misleading Default Threshold Comment
* **File:** [TankAlarm-112025-Common/src/TankAlarm_Config.h](TankAlarm-112025-Common/src/TankAlarm_Config.h#L37-L40)
* **Change:** Replace the inline comment as shown in §6.3. No code or macro name changes; documentation-only fix to remove the contradiction with `MIGRATION_GUIDE.md`.
* **Risk:** None.

#### Step 4 — Rename the Three Headline Cache Fields (Sensor-Agnostic)

The pervasive use of `currentInches` / `levelInches` / `previousLevelInches` for values that may actually hold PSI, RPM, GPM, or digital state is a documentation-by-naming bug. Backward compatibility on the on-disk registry is preserved because persistence uses short keys (`l`, `pl`) — not these long names — so this is a pure code/identifier rename.

**Rename (exactly three identifiers, plus their JSON-output mirrors):**

| File / scope | From | To |
|---|---|---|
| Client `MonitorRuntime` struct + all references | `currentInches` | `currentValue` |
| Server `SensorRecord` struct + all references | `levelInches` | `currentValue` |
| Server `SensorRecord` struct + all references | `previousLevelInches` | `previousValue` |
| Viewer `SensorRecord` struct + all references | `levelInches` | `currentValue` |
| Server daily-email JSON output | `obj["levelInches"]` | `obj["currentValue"]` |
| Server dashboard JS property names inside `R"HTML(...)"` blocks | `levelInches`, `previousLevelInches` | `currentValue`, `previousValue` |

The established sibling fields already use the universal pattern (`lastReportedValue`, `lastDailySentValue` in `MonitorRuntime`); the rename simply pulls the headline field into the same convention.

**Explicitly do NOT rename** the following — these identifiers describe genuine liquid-tank geometry, not stored measurements:

* `MonitorConfig::sensorMountHeight` (physical mount geometry of an ultrasonic/pressure sensor above the tank bottom — always inches, meaningless for RPM/PSI-gauge/flow)
* `MonitorConfig::unloadEmptyHeight` and the entire unload tracking subsystem (`unloadPeakInches`, server `UnloadHistoryEntry::peakInches` / `emptyInches`) — gated by `trackUnloads`, liquid-only by design
* `SensorHistoryEntry::heightInches` — tank height used by `% capacity` rendering
* `verifiedLevelInches` (REST `/api/calibration` request body), `CalibrationCache::minLevelInches` / `maxLevelInches`, dashboard `id="levelInches"` HTML form input — these belong to the calibration UI which already mode-switches between tank feet/inches input and generic value+unit input; rename them in a follow-up calibration refactor, not here
* Local variables `levelInches` inside `readPressureSensor()` / `readUltrasonicSensor()` and `currentInches` inside `sendUnloadEvent()` / `evaluateUnload()` — these compute or describe values that are genuinely inches in their local scope

**Scope and risk:**

* Sites affected: ~30 in [TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino), ~20 + ~12 in [TankAlarm-112025-Server-BluesOpta.ino](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino), ~7 in [TankAlarm-112025-Viewer-BluesOpta.ino](TankAlarm-112025-Viewer-BluesOpta/TankAlarm-112025-Viewer-BluesOpta.ino), plus dashboard JS occurrences inside server R"HTML(...)" blocks.
* The persisted registry continues to read/write `obj["l"]` and `obj["pl"]` — zero migration required.
* Wire telemetry already uses short keys (`l`, `lvl`, `d`, `pl`) — zero client/server contract change.
* The daily-email JSON consumer (`obj["levelInches"]`) is the one breaking change; renaming it in lockstep with any downstream template consumer keeps email output coherent.
* Recommended approach: language-server rename per identifier (one symbol at a time, verify diff, build, then commit). This catches every site including string interpolations in serial logs the grep search may miss.

### 7.3 Out of Scope (Deliberately)

The following adjacent issues surface during this review but are **not** included in this plan and would need their own ticket/review:

* **Calibration UI / REST rename:** `verifiedLevelInches`, `minLevelInches`, `maxLevelInches`, dashboard `id="levelInches"` input — these touch a user-facing REST API surface and the calibration HTML form, and deserve a focused refactor pass.
* **Optional periodic telemetry mode:** If product policy wants "send a heartbeat sample every N hours regardless of threshold," that is a new feature, not a bug fix. It would require adding a separate `telemetryHeartbeatSec` field and scheduling path.
* **Per-sensor `t` in alarm.qo:** `sendAlarm`'s top-level `t` is already the transmission epoch and the body uses `buildSensorObject`. The alarm path could benefit from the same change but is a separate concern.

### 7.4 Validation Plan

After the four edits land:

1. **Compile all three sketches** (`Client`, `Server`, `Viewer`) with `arduino-cli compile` and confirm no warnings/errors. The Step 4 rename touches all three.
2. **Unit-test the client output:** boot a device with a known `lastReadingEpoch`, force a daily report (via the dev command), and inspect the captured `daily.qo` JSON in Notehub Events. Each entry in `sensors[]` should now have a `t` field equal to the last successful sample's epoch.
3. **Server side:** confirm `rec->lastUpdateEpoch` advances only when a real new acquisition happened. Force `sensorFailed`/`sampleReused` on a test sensor, fire a daily report, and verify the dashboard "last update" age now reflects the frozen acquisition time rather than the report time.
4. **Universality regression:** run the validation against at least one of each sensor interface (current-loop, analog voltage, digital float switch, pulse/RPM) to confirm the new field is emitted and the renamed fields hold the correct non-inch values for non-tank monitors.
5. **Registry persistence smoke test:** save the sensor registry on a server running the Step 4 rename, reboot, and verify all records reload with their previous values — confirming that short-key persistence (`l`, `pl`) survived the C++ rename unchanged.
6. **Daily-email regression:** verify the daily email body still renders sensor rows (now keyed by `currentValue`) without empty fields.
