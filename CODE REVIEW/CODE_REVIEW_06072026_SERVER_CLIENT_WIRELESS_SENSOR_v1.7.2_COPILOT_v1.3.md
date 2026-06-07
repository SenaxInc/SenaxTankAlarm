# Code Review: 112025 Server and 112025 Client v1.7.2

**Date:** June 7, 2026
**Review Version:** v1.3
**Reviewed Firmware:** v1.7.2 (`FIRMWARE_VERSION` in [../TankAlarm-112025-Common/src/TankAlarm_Common.h](../TankAlarm-112025-Common/src/TankAlarm_Common.h#L19))
**Notefile Schema:** Schema Version 2 ([../TankAlarm-112025-Common/src/TankAlarm_Common.h](../TankAlarm-112025-Common/src/TankAlarm_Common.h#L25))
**Reviewer:** GitHub Copilot
**Scope:** Static source review of the 112025 Server and Client firmware, with emphasis on the wireless messaging system between the two devices, client sensor data collection and processing, payload transmission, and server-side ingestion/processing.

> No firmware changes, compile, or upload were performed as part of this review. This is a read-only analysis.

---

## Executive Summary

The v1.7.2 firmware is in good shape architecturally. The client builds every outbound sensor payload through one `buildSensorObject()` helper (telemetry, alarm, daily), so the wire contract no longer drifts between message types. The server's `resolveLevel()` correctly treats the client-sent `lvl`, raw `ma`/`vt`, `cap`, `st`, and calibration version `cv` as a self-describing contract and only re-derives a level from raw mA when its learned calibration is newer than the client's. The current-loop live-zero (`< 3.6 mA`) fault and the `isfinite()` gate in `validateSensorReading()` are solid production-hardening choices.

This review re-verified the ten findings from review v1.2 against the current source. **All ten are still present** — the relevant code paths are unchanged. They are summarized below (Part A) with current line numbers, followed by **five new findings** (Part B) discovered during this pass that were not in v1.1 or v1.2, primarily in alarm hysteresis logic, history pruning, the raw-mA emission boundary, sensor-index validation, and pulse/RPM measurement.

The single most important theme remains the wireless command path: the relay/serial/location/sync inboxes still delete notes before validating or executing them, and several server support paths still create state from body-supplied identity without the `isValidClientUid()` check that protects the main sensor table. The most important *new* finding is a logic bug in `evaluateAlarms()` where the alarm clear band can collapse for certain threshold/hysteresis combinations and latch an alarm permanently.

---

## Severity Overview

| ID | Severity | Area | Status |
|----|----------|------|--------|
| A1 | High | Wireless / command inbox | Still open (v1.2 #1) |
| A2 | High | Wireless / identity + schema validation | Still open (v1.2 #2) |
| A3 | Medium-High | Server config dispatch size | Still open (v1.2 #3) |
| A4 | Medium-High | Client offline replay size | Still open (v1.2 #4) |
| A5 | Medium | Transmission / daily partitioning | Still open (v1.2 #5) |
| A6 | Medium | Sensor collection / stale reuse | Still open (v1.2 #6) |
| A7 | Medium | Calibration temperature model | Still open (v1.2 #7) |
| A8 | Medium | Sensor collection / invalid-config zero | Still open (v1.2 #8) |
| A9 | Medium-Low | Server / daily alarm reconciliation | Still open (v1.2 #9) |
| A10 | Low | Wireless / relay replay | Still open (v1.2 #10) |
| **N1** | **Medium** | **Sensor processing / alarm hysteresis** | **New** |
| **N2** | **Low-Medium** | **Server / history timestamps + pruning** | **New** |
| **N3** | **Low** | **Transmission / raw-mA emission boundary** | **New** |
| **N4** | **Low** | **Server / sensor-index validation** | **New** |
| **N5** | **Low-Medium** | **Sensor collection / polled pulse undercount** | **New** |

---

## Implementation Status (applied this session — June 7, 2026)

Fixes were implemented autonomously after the review. Both firmwares compile clean for `arduino:mbed_opta:opta` (server: 47% flash / 67% RAM; client: 15% flash / 14% RAM; `arduino-cli compile` exit 0 for both). **No firmware was uploaded.**

| ID | Status | Implementation notes |
|----|--------|----------------------|
| A1 | Fixed | Relay/serial/location/sync inboxes converted to peek → validate → execute → delete; relay inbox also gates `_sv > NOTEFILE_SCHEMA_VERSION`. |
| A2 | Fixed | `isValidClientUid()` added to `handleAlarm()` (covers the system-alarm metadata path) and `handleDaily()`; central `_sv` schema gate added in `processNotefile()`. |
| A3 | Fixed | `ClientConfigSnapshot.payload` raised 1536 → 4096 via `CLIENT_CONFIG_SNAPSHOT_PAYLOAD_MAX`; the two save-side line buffers now track it via `CLIENT_CONFIG_CACHE_LINE_MAX` (load buffer already auto-scaled from `sizeof(payload)`). |
| A4 | Fixed | `flushBufferedNotes()` replay line buffer sized to `NOTE_REPLAY_LINE_MAX` (2304) to cover the publishable payload. |
| A5 | Fixed | `sendDailyReport()` no longer advances the cursor on a fit failure; it publishes the current part (a metadata-only part 0 is now valid) and retries the same monitor in a clean part. A genuinely oversized single monitor is still skipped to prevent an infinite loop. |
| A6 | Fixed | `readCurrentLoopSensor()` returns `NAN` (and clears raw mA) on total acquisition failure so `validateSensorReading()` escalates a `sensor-fault`. |
| A7 | Fixed | `resolveLevel()` keeps applying server-live temperature from raw mA for current-loop sensors with a non-zero learned temp coefficient, even when `cv` matches, removing the catch-up display jump. |
| A8 | Fixed | `readAnalogSensor()`, the current-loop invalid-range path, and the `readMonitorSensor()` unknown-interface default now return `NAN` instead of `0.0`. |
| A9 | Fixed | Client always emits `alarms: []` in part 0; server runs reconciliation when a schema-2+ daily omits the array (treats absence as "no active alarms"). |
| N1 | Fixed | `evaluateAlarms()` clear bands decoupled (high clears below `high − hyst`; low clears above `low + hyst`); hysteresis clamped non-negative. Removes the permanent-latch case. |
| N2 | Fixed | History snapshots are skipped before time sync (no `0.0` timestamps); `pruneHotTierIfNeeded()` now compacts survivors instead of count-only shrink. |
| N3 | Partially fixed | Stale-mA emission during current-loop outage is resolved by the A6 change (mA cleared on failure). The narrow [3.6, 4.0) mA emission-vs-fault boundary was left as-is (near-zero benefit; 4.0 mA nominal-zero convention retained). |
| N4 | Fixed | `upsertSensorRecord()` rejects sensor indices `>= MAX_SENSOR_RECORDS`. |
| A10 | Deferred | Durable relay replay protection needs a server-emitted command-id contract that does not exist yet; deferred rather than half-implemented. |
| N5 | Deferred | Moving pulse counting to hardware interrupts cannot be validated without the target hardware; left documented. |

> Verification performed: static review plus a clean compile of both sketches. The behavioral test cases in "Suggested Verification Tests" below have **not** been run on hardware.

---

# Part A — Prior Findings Re-Verified (Still Open)

These were documented in `CODE_REVIEW_06072026_SERVER_CLIENT_WIRELESS_SENSOR_v1.7.2_COPILOT_v1.2.md`. Each was re-checked against the current source and remains unchanged. Condensed here with refreshed line references; see v1.2 for the full discussion.

### A1 (High) — Client command inboxes delete messages before validation/execution

The config inbox uses the safe peek-then-delete pattern ([../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L3918)), but the relay, serial, location, and sync inboxes still issue `note.get` with `delete=true` *before* deserialization, target/schema validation, and successful execution:

- `pollForRelayCommands()` — [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7891)
- `pollForSerialRequests()` — [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L8446)
- `pollForLocationRequests()` — [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L8581)
- `pollForSyncRequests()` — [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L8647)

A malformed/transient-parse/handler failure permanently drops the command. `processRelayCommand()` validates `target` only *after* the note is already deleted ([../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7923)). **Fix:** convert all inboxes to peek → validate (`_target`/`_sv`/type) → execute → delete.

### A2 (High) — Server/client trust body-supplied identity with no uniform source/schema policy

`processNotefile()` hands parsed notes to handlers and deletes them with no central `_sv` schema gate ([../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11766)). `isValidClientUid()` exists ([../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L1343)) and protects `upsertSensorRecord()` ([../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12717)), but metadata/support paths bypass it:

- System alarms call `findOrCreateClientMetadata(clientUid)` with no UID check — [../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12030)
- `handleDaily()` checks only `strlen==0`, not format — [../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12304)
- `findOrCreateClientMetadata()` itself does not validate — [../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12239)

Malformed/spoofed UIDs can consume the 20-slot metadata store and evict legitimate clients. **Fix:** a shared `validateInboundNote(doc, expectedType)` applied before every handler, plus `isValidClientUid()` on every path that reads `c`/`client`/`target`, and reject `_sv > NOTEFILE_SCHEMA_VERSION`.

### A3 (Medium-High) — Config snapshot cache can block dispatch of valid large configs

`ClientConfigSnapshot.payload` is `char[1536]` ([../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L983)), but `dispatchClientConfig()` serializes into an 8192-byte buffer and `cacheClientConfigFromBuffer()` rejects anything `>= 1536` ([../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L14639)). A multi-sensor config with names + battery/solar settings + learned calibration coefficients can exceed 1535 bytes and never dispatch, so the client never receives updated calibration (`cv` stays mismatched). **Fix:** raise the snapshot to match the dispatch buffer, or store per-client configs in LittleFS and keep only metadata in RAM.

### A4 (Medium-High) — Offline note replay discards buffered notes over ~1024 bytes

`publishNote()` serializes through a 2048-byte static buffer with heap fallback ([../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7263)) and `bufferNoteForRetry()` writes full notes to flash ([../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7362)), but `flushBufferedNotes()` reads with `char lineBuffer[1024]` and *skips* truncated lines ([../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7397)). Large daily reports produced during an outage — exactly the backup data — are stored but lost on replay. **Fix:** size the replay line buffer to the maximum serializable payload, or store each pending note as its own length-prefixed file.

### A5 (Medium) — Daily report partitioning can permanently skip a monitor

When a monitor will not fit, `sendDailyReport()` retries with a relaxed limit and, on a second failure, logs the skip and still advances `monitorCursor` ([../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7044)); `appendDailyMonitor()` removes the over-limit object ([../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7100)). Part 0 is most exposed because it also carries VIN/solar/battery/power/signal/alarm metadata. The server's completeness bitmask cannot tell a monitor was dropped pre-serialization. **Fix:** do not advance the cursor on failure; flush the current part, start a clean part without part-0 metadata, retry the same monitor; include a monitor count so the server can flag incompleteness.

### A6 (Medium) — All-failed current-loop sample set reuses stale level without raising a fault

When every mA sample fails, `readCurrentLoopSensor()` sets `sampleReused = true` and returns the previous `currentInches` ([../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4969)). The timestamp handling is good (reused samples keep `lastReadingEpoch`, see [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5096)), but the stale value typically passes `validateSensorReading()`, so `consecutiveFailures`/`sensorFailed` never trip and no `sensor-fault` is sent. A disconnected current-loop module can leave alarms/relays evaluating against stale data. **Fix:** return `NAN` (or increment the non-finite failure path) on total acquisition failure, and/or emit a `stale`/`ageSec` flag.

### A7 (Medium) — Learned-calibration temperature compensation flips between server-live and client-static

While the client `cv` is stale, the server recomputes level from raw mA using live `getCachedTemperature()` ([../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L13335)). Once the client applies the pushed coefficients it uses the static `calTempF` snapshot ([../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4979)) and the server then trusts client `lvl` because `cv` matches — so the displayed level can step when the client catches up. This is a documented architectural tradeoff; **fix or document:** for `calTempCoef != 0` keep applying server-live temp from raw mA even when `cv` matches, or carry a temperature basis per note.

### A8 (Medium) — Invalid sensor configuration collapses into a valid-looking zero

`validateSensorReading()` rejects non-finite values, but several invalid-config paths still return `0.0f`, which is indistinguishable from a legitimately empty tank / inactive switch:

- `readAnalogSensor()` invalid range → `0.0f` — [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4825)
- `readCurrentLoopSensor()` invalid range → `0.0f` — [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4882)
- `readMonitorSensor()` unknown interface → `0.0f` — [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5075)

**Fix:** return `NAN` for invalid ranges/unknown interfaces so the existing non-finite path raises `sensor-fault`, and validate config when applying it.

### A9 (Medium-Low) — Daily reconciliation does not clear orphaned alarms when the client has no active alarms

The client only emits the `alarms` array when at least one alarm is latched ([../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7012)), and the server's reconciliation runs only inside `if (isFirstPart && dailyAlarms)` ([../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12377)). If a `clear` note was lost and the next daily has zero active alarms, the array is absent and the server keeps a stale alarm indefinitely. **Fix:** emit `alarms: []` in part 0 of every daily, or treat a missing array on a schema-2 first part as empty.

### A10 (Low) — Relay command cooldown is runtime-only

`processRelayCommand()` rate-limits via a static `lastRelayCommandMillis` ([../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7937)) that is cleared by any reboot/watchdog reset. **Fix:** include a command ID + creation timestamp and persist a short replay window.

---

# Part B — New Findings (This Review)

## N1 (Medium) — Alarm clear band can collapse and latch an alarm permanently

**Location:** `evaluateAlarms()` — [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5243)

The analog/current-loop hysteresis uses a single "clear band" between the two thresholds:

```cpp
float highClear = cfg.highAlarmThreshold - cfg.hysteresisValue;
float lowClear  = cfg.lowAlarmThreshold + cfg.hysteresisValue;
...
bool clearCondition = (state.currentInches < highClear) && (state.currentInches > lowClear);
```

A latched high *or* low alarm only clears when `clearCondition` is true. Two problems:

1. **Collapsed band.** If `highAlarmThreshold - lowAlarmThreshold <= 2 * hysteresisValue`, then `highClear <= lowClear` and `clearCondition` is `(x < highClear) && (x > lowClear)` — an empty/inverted interval that is **never** true. A high alarm that latches can never auto-clear through the hysteresis path. Example: `high=10`, `low=8`, `hysteresis=2` → `highClear=8`, `lowClear=10` → no value of `x` satisfies both. The server keeps showing the alarm until a *different* alarm transition overwrites it.

2. **Cross-coupling when low alarm is unused.** Even with a wide gap, the high-alarm clear depends on `lowClear = lowAlarmThreshold + hysteresisValue`. If the low alarm is effectively unconfigured (`lowAlarmThreshold == 0`), a tank that drains below `hysteresisValue` inches sits at `x <= lowClear`, so `clearCondition` is false and the high alarm stays latched while the tank is near-empty. In a normal slow drain the level passes through the band and clears on the way down, so the everyday case is fine; the failure shows up on a fast level drop, a sensor glitch, or the collapsed-band misconfiguration above.

**Impact:** A stuck latched alarm continues to suppress new same-type alarms via `checkAlarmRateLimit()` and keeps the server dashboard in an alarm state. There is no configuration validation preventing `hysteresisValue` from being set too large relative to the high/low gap.

**Suggested fix:**
- Validate config on apply: require `hysteresisValue < (highAlarmThreshold - lowAlarmThreshold) / 2` when both thresholds are active, and clamp/log otherwise.
- Decouple the two clear conditions: clear the high alarm on `currentInches < highClear` and clear the low alarm on `currentInches > lowClear`, rather than requiring a single shared middle band. This also removes the dependency of high-alarm clearing on the low threshold.

---

## N2 (Low-Medium) — History snapshot timestamps and count-based pruning are unsafe across non-monotonic time

**Locations:**
- `recordTelemetrySnapshot()` stamps `timestamp = 0.0` before the first time sync — [../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L7872)
- `pruneHotTierIfNeeded()` count-only prune — [../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L7929)

`recordTelemetrySnapshot()` writes `snap.timestamp = now`, where `now` is `0.0` if `gLastSyncedEpoch <= 0.0`. Telemetry/alarm/daily that arrives before the server's clock syncs is stored with timestamp `0.0`.

`pruneHotTierIfNeeded()` then prunes by **counting** survivors and shrinking `snapshotCount`, explicitly relying on the comment "timestamps are monotonically increasing, so all pruned entries are contiguous at the start":

```cpp
for (uint16_t j = 0; j < hist.snapshotCount; j++) {
  uint16_t idx = (hist.writeIndex - hist.snapshotCount + j + ...) % MAX_HOURLY_HISTORY_PER_SENSOR;
  if (hist.snapshots[idx].timestamp >= cutoffEpoch) newCount++; else pruned++;
}
hist.snapshotCount = newCount;   // assumes the failing entries are the oldest contiguous ones
```

If timestamps are **not** monotonic — a clock reset, an NTP/Notecard time correction, or `0.0` pre-sync entries interleaved with later real timestamps — the survivors are not the newest contiguous block, but the count reduction always drops entries from the logical *start*. The result is that valid recent snapshots get discarded while stale ones are retained, silently corrupting charts/sparklines.

**Impact:** Low probability (requires a clock discontinuity), but the corruption is silent and the buffer never self-heals because the logical start is permanently misaligned.

**Suggested fix:** compact survivors into a contiguous block (copy kept entries to a temp/in-place compaction and reset `writeIndex`) instead of only adjusting the count; and either skip recording snapshots until `gLastSyncedEpoch > 0` or backfill `0.0` timestamps once time syncs.

---

## N3 (Low) — Raw-mA emission boundary leaves a gap and can emit stale mA

**Locations:**
- `buildSensorObject()` emits `ma` only when `currentSensorMa >= 4.0f` — [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5354)
- `readCurrentLoopSensor()` faults only below `CURRENT_LOOP_FAULT_MA` (3.6) — [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4988)

Two boundary issues in the raw-value contract that feeds the server's `resolveLevel()` re-calibration:

1. **[3.6, 4.0) mA window.** A reading of, say, 3.8 mA is not treated as a fault (it is `>= 3.6`), so it converts to a (clamped) level and is sent as `lvl`. But `buildSensorObject()` omits `ma` because `3.8 < 4.0`. When such a client has a stale `cv`, the server *wants* to re-apply its coefficients to raw mA in `resolveLevel()` ([../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L13343)) but cannot, because `ma` is absent — so it falls back to trusting the un-recalibrated client `lvl`.

2. **Stale mA in daily reports.** When `validSamples == 0`, `readCurrentLoopSensor()` returns early **without** updating `currentSensorMa` ([../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4969)), so the field keeps its last good value. Telemetry is change-gated and usually won't fire, but the daily report sends unconditionally, so a current-loop outage can ship a stale `ma` alongside a stale `lvl` (compounding A6).

**Impact:** Minor accuracy/observability gaps at the fault boundary; the server cannot distinguish "no raw value" from "raw value below 4 mA," and daily payloads can carry stale raw mA during an outage.

**Suggested fix:** align the emission threshold with the fault threshold (emit `ma` when `>= CURRENT_LOOP_FAULT_MA`, or always emit the measured value and let the server validate), and clear/flag `currentSensorMa` when `validSamples == 0`.

---

## N4 (Low) — Missing `k` defaults sensor index to 0; sensor index is never range-validated

**Locations:**
- `handleTelemetry()` / `handleAlarm()` / `handleDaily()` use `doc["k"].as<uint8_t>()` — e.g. [../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11865)
- `upsertSensorRecord()` validates the UID but not the index — [../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12716)

A note that omits `k` deserializes to sensor index `0`, so a malformed payload silently writes/overwrites sensor 0 for that client. `upsertSensorRecord()` enforces `isValidClientUid()` but accepts any `uint8_t` index (0–255), so an out-of-range index also creates a record and consumes a slot toward `MAX_SENSOR_RECORDS`.

**Impact:** Low (UID validation already limits the blast radius to legitimately-prefixed devices), but a buggy/garbled client can corrupt sensor 0 or inflate the registry with phantom indices.

**Suggested fix:** require `k` to be present (reject the note if absent) and bound-check the index against a sane maximum (e.g., `MAX_MONITORS`) before `upsertSensorRecord()`.

---

## N5 (Low-Medium) — Polled pulse/RPM counting can systematically undercount

**Locations:**
- `pollPulseSampler()` counts edges only inside `PULSE_POLL_BURST_MS` bursts — [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L1117)
- Pulse-counting RPM uses the **full** sample duration as the denominator — [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L1230)

There is no `attachInterrupt()` anywhere in the client; `atomicIncrementPulses()` is called only from the polling loop ([../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L1138)), not from an ISR, despite the `volatile`/"atomic" naming. In pulse-counting mode, edges are only detected while the code is inside a `PULSE_POLL_BURST_MS` window each `loop()` pass, but the RPM is computed as:

```cpp
ctx.resultRpm = ((float)ctx.pulseCount * 60000.0f) / ((float)ctx.sampleDurationMs * (float)ctx.pulsesPerRev);
```

The denominator assumes continuous coverage over `sampleDurationMs`, while the numerator only accrues during burst windows. Any time `loop()` spends elsewhere — notably multi-second current-loop PWM warmups (`pwmGatingWarmup`, [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4920)) or blocking Notecard transactions — is a window where pulses are missed but still counted against the full duration, biasing RPM low. The accumulated mode divides by actual elapsed wall time, which fixes the *time base*, but the *count* is still polled and can miss edges during gaps.

**Impact:** Engine/pump RPM and flow readings can read low on devices that also run current-loop sensors with PWM warmups, or whenever the main loop has long blocking sections. High-frequency pulse trains are most affected.

**Suggested fix:** use a hardware interrupt (`attachInterrupt` on an interrupt-capable Opta input) to count edges continuously, leaving the loop free to measure elapsed time; if polling must remain, measure the *actual* covered sampling time and use that as the denominator, and document the maximum reliably-measurable pulse frequency.

---

## Focus-Area Notes (as requested)

**Wireless messaging system (client ↔ server).** The transport contract is consistent and self-describing (`buildSensorObject` + `resolveLevel`), and `_sv` now survives offline buffering. The open risks are all on the *trust and durability* side: command inboxes that delete-before-validate (A1), missing uniform identity/schema validation on the server (A2), the config snapshot size ceiling that can block dispatch (A3), and the offline replay buffer that drops large notes (A4). These four should be treated as the wireless-system priority set.

**Sensor data collection & processing (client).** The `validateSensorReading()` non-finite gate, current-loop live-zero fault, multi-sample averaging, and stuck/recovery detection are solid. Gaps: stale reuse on total current-loop failure does not raise a fault (A6), invalid configuration returns a valid-looking zero (A8), the alarm clear band can collapse (N1), and polled pulse counting can undercount (N5).

**Transmission of data (client).** The single-helper payload path is good and `publishNote()` handles oversize via heap fallback. Risks: daily partitioning can silently skip a monitor (A5), the offline replay path truncates large notes (A4), and the raw-mA emission boundary has a gap and can emit stale values (N3).

**Processing at the server (ingestion).** `resolveLevel()` reconciliation, capacity-based history snapshots, per-client SMS limiting, and daily completeness tracking are well-built. Risks: support paths bypass UID/schema validation (A2), daily reconciliation misses the all-clear case (A9), history pruning is unsafe under non-monotonic time (N2), and sensor index is unvalidated (N4).

---

## Recommended Priority Order

1. Convert all client command inboxes to peek → validate → execute → delete (A1).
2. Add shared inbound schema/source/UID validation on server and client (A2, N4).
3. Fix the alarm clear-band collapse and decouple high/low clearing; validate hysteresis vs. thresholds (N1).
4. Raise/redesign the server config snapshot storage so valid large configs dispatch (A3).
5. Redesign offline replay to handle the full publishable payload size (A4).
6. Stop advancing the daily cursor on append failure; mark daily completeness by monitor count (A5).
7. Treat total current-loop acquisition failure and invalid configuration as faults, not valid zero/stale values (A6, A8, N3).
8. Compact history on prune and avoid `0.0` pre-sync timestamps (N2).
9. Decide and document the calibration temperature model (A7).
10. Move pulse counting to hardware interrupts or correct the duty-cycle denominator (N5).
11. Treat a missing schema-2 daily `alarms` array as empty / always send `alarms: []` (A9).
12. Add durable relay command IDs/timestamps for replay protection (A10).

---

## Suggested Verification Tests

1. **Command inbox safety:** inject relay/serial/location/sync notes with wrong `_target`, unsupported `_sv`, invalid JSON, and valid payloads; verify wrong/future/bad notes are not executed and valid ones delete only after success.
2. **Server identity hygiene:** send system-alarm, daily, serial-log, location-response, config-ACK, and relay-forward notes with non-`dev:` UIDs; verify no metadata/serial/ACK/relay state is created.
3. **Alarm hysteresis edge:** configure `high=10, low=8, hysteresis=2`, drive the level above `high`, then hold it at `9` and at `<2`; verify the alarm still clears (it currently will not).
4. **Large config dispatch:** build a max-monitor config with long names + learned calibration; verify cache/dispatch/reboot-load/ACK/retry all succeed.
5. **Offline replay:** buffer daily reports above 1024/2048 bytes offline, restore the Notecard, verify every buffered note replays.
6. **Current-loop total failure:** force all four mA samples to fail; verify a `sensor-fault` is raised and stale data is flagged, not silently reused.
7. **History prune under clock change:** record snapshots, force a backward clock correction, run the prune, verify recent data survives and old data is dropped.
8. **Pulse accuracy:** feed a known-frequency square wave while a current-loop sensor with a multi-second PWM warmup runs; compare measured vs. expected RPM.
9. **Daily clear reconciliation:** drop a `clear` note, send a daily report with no active alarms, verify the server clears the orphaned alarm.

---

## Areas That Look Sound (re-confirmed)

- `buildSensorObject()` centralizes `ot`/`mu`/`st`/raw/`lvl`/`cap`/`cv`, eliminating telemetry/alarm/daily field drift ([../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5333)).
- `publishNote()` stamps `_sv` into the document before serialization, so buffered notes keep their schema version ([../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7256)).
- `resolveLevel()` re-applies server calibration only when the client `cv` is stale and raw mA is present ([../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L13335)).
- Current-loop live-zero detection returns `NAN` below 3.6 mA and `validateSensorReading()` catches non-finite values ([../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4629)).
- `processNotefile()` uses peek-then-delete with a 3-strike poison-note path for the server ingest loop ([../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11766)).
- Per-client system-alarm SMS limiting via `ClientMetadata.lastSystemSmsEpoch` (no longer fleet-wide) ([../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12046)).
- Capacity-based history (`cap`) is passed correctly by telemetry/alarm/daily instead of the current level.
- Alarm rate limiting guards unsigned-underflow under 1-hour uptime and checks the global cap before consuming the per-monitor budget ([../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5440)).
- Watchdog kicks wrap the long Notecard and current-loop/PWM-warmup operations.

---

*End of review v1.3.*
