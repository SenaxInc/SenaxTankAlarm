# Code Review: 112025 Server and 112025 Client (Wireless + Sensor Pipeline)

Date: June 3, 2026
Review version: v1.1
Firmware version reviewed: 1.6.14 (`FIRMWARE_VERSION` in `TankAlarm-112025-Common/src/TankAlarm_Common.h`)
Reviewer: GitHub Copilot

## Scope and Relationship to Prior Review

This pass focuses on the same subsystems the requester asked about:

- Wireless Notecard messaging between client and server.
- Client sensor data collection and processing.
- Telemetry/alarm/daily transmission payloads.
- Server-side ingestion, conversion, and history processing.

Files examined:

- `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino`
- `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino`
- `TankAlarm-112025-Common/src/TankAlarm_Common.h`

A prior review from the same day, `CODE_REVIEW_06032026_SERVER_CLIENT_WIRELESS_SENSOR_v1.0_COPILOT.md`, already documents eight findings (analog conversion mismatch, `heightInches` corruption, inbound command validation, offline buffer flush sizing, non-self-describing daily reports, PWM gate-failure handling, config snapshot cache size, gas negative validation). **This v1.1 does not repeat those write-ups in full.** Instead it:

1. Independently re-verifies the two highest-severity v1.0 findings against the current source (Section A).
2. Adds new findings that v1.0 did not cover (Section B).

This was a static review. No firmware source was modified and no compile was rerun.

---

## Section A — Independent Confirmation of Prior High-Severity Findings

### A1. CONFIRMED — Historical `heightInches` is inconsistent across the three ingest paths

The capacity field used for percent-full math is written three different (wrong) ways depending on which note arrives:

- Telemetry: [TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11973) reads `doc["h"]` and defaults to `48.0f`. The client never sends `h` in telemetry (`sendTelemetry`, [client L5191](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5191)), so this is **always** 48.0.
- Alarm: [server L12141](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12141) passes `rec->levelInches` as the height argument — i.e. the value just assigned from the current reading.
- Daily: [server L12508](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12508) also passes `rec->levelInches` as the height argument, immediately after setting `rec->levelInches = newLevel`.

`recordTelemetrySnapshot()` stores whatever it is given straight into `hist->heightInches` ([L7856](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L7856)), and that field is exported to the dashboard as `heightInches`/`h` ([L8014](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L8014), [L8625](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L8625)) where the UI uses it for percent-of-capacity.

Net effect on a single sensor's history: telemetry snapshots claim capacity 48.0, while alarm and daily snapshots claim capacity == current level (so percent-full reads ~100%). The last writer wins for the shared `hist->heightInches`, so the displayed capacity jumps around based on which note type arrived most recently. This remains the most impactful data-quality bug.

Recommended fix is as in v1.0: derive the true height from the cached client config (or have the client send `h`), and never pass `rec->levelInches` as the height. See also B1 below for a deeper structural note on why a single `hist->heightInches` is the wrong shape.

### A2. CONFIRMED — Server analog voltage conversion does not mirror the client

`readAnalogSensor()` on the client ([L4741](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4741)) maps voltage to native pressure, then applies pressure-unit conversion and specific gravity to produce inches of fluid, with a separate raw-pressure path for `OBJECT_GAS`. The client transmits only the raw voltage `vt`.

`convertVoltageToLevel()` on the server ([L13379](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L13379)) only does `rangeMin + fraction*(rangeMax-rangeMin) + mountHeight`. It does **not** apply `sensorRangeUnit` pressure conversion, does **not** divide by specific gravity, and has **no** gas/liquid branch — unlike its current-loop sibling `convertMaToLevelWithTemp()` ([L13259](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L13259)) which does all three. Confirmed still present. Fix: make the voltage path mirror the mA path.

---

## Section B — New Findings (not in v1.0)

### B1. Medium-high: A sensor's history capacity is a single scalar, so mixed note types fight over it

Beyond the call-site bug in A1, the underlying data model stores exactly one `heightInches` per `SensorHourlyHistory` ([L688](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L688)) and overwrites it on every snapshot ([L7856](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L7856)). Even after the A1 call-sites are fixed, any path that passes a different height (e.g. a future per-reading height, or gas full-scale vs. tank inches) will retroactively change the capacity used to render *all* stored snapshots for that sensor, because percent-full is computed against the single current `heightInches`, not the height that was in effect when each snapshot was taken.

Impact: historical percent-full can shift retroactively whenever capacity/config changes or a differing height is written.

Suggested fix: treat `heightInches` as immutable per sensor (set once from config, updated only on an explicit config change), and compute it from config rather than from any inbound note value. If per-reading capacity is ever needed, store it on the snapshot, not on the parent.

### B2. Medium: Alarm notes omit `st`, so server conversion depends on a prior telemetry having run

`sendAlarm()` transmits raw `ma`/`vt`/`fl`/`rm` plus thresholds, but does **not** include the sensor-interface type `st` ([client L5493-5507](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5493)). The server's `handleAlarm()` decides how to decode the raw field purely from `rec->sensorType` ([server L12084-12097](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12084)):

```cpp
bool isCurrentLoop = (strcmp(rec->sensorType, "currentLoop") == 0);
...
if (isCurrentLoop && mA >= 4.0f) { level = convertMaToLevelWithTemp(...); }
else if (isAnalog && voltage > 0.0f) { ... }
```

If an alarm is the first note the server sees for a sensor (registry was cleared/evicted, server rebooted and lost the registry, or the sensor alarmed before its first telemetry/daily established `sensorType`), then `rec->sensorType` is empty, none of the branches match, and `level` stays `0.0`. The alarm is still recorded, but with level 0 and a level-0 history snapshot is suppressed by the `if (level > 0.0f)` guard ([L12138](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12138)).

This is the alarm-path analogue of v1.0's finding #5 (which only addressed daily). Alarms are the most safety-relevant note type, so making them self-describing matters more here.

Suggested fix: include `st` (and ideally `h`) in alarm payloads, and in `handleAlarm()` fall back to the cached config snapshot to resolve `sensorType` when `rec->sensorType` is empty (the telemetry path already does a config fallback for `objectType`).

### B3. Medium: Daily report can permanently drop a monitor whose entry plus part-0 header exceeds the size limit

In `sendDailyReport()` ([client L6810](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L6810)) the part-0 document is pre-loaded with VIN, solar, battery, power, signal, and an active-alarm summary before any sensor is appended. The inner append loop is:

```cpp
if (!addedMonitor) {
  if (appendDailyMonitor(doc, sensors, monitorIdx, DAILY_NOTE_PAYLOAD_LIMIT + 48U)) { ... }
  else {
    Serial.println(F("Daily report entry skipped; payload still exceeds limit"));
    ++monitorCursor;   // monitor consumed but NOT added to any part
  }
  break;
}
```

If the part-0 metadata is large and the very first monitor still does not fit even at the `+48` fallback, the monitor cursor is advanced and the loop breaks with `addedMonitor == false`. The outer loop then hits `if (!addedMonitor) continue;` without incrementing `part`, so that monitor is never emitted in any part — its daily data is silently lost. Because the bulky metadata only lives in part 0, the same monitor would have fit fine in part 1.

Impact: rare, but a config with many part-0 extras (solar+battery+power+signal+multiple active alarms) plus one verbose monitor entry can drop that monitor from the daily report entirely.

Suggested fix: if `addedMonitor` is false because of the part-0 header, finalize/skip the header and retry the same monitor on a fresh (header-less) part instead of consuming and discarding it; or only attach the part-0 metadata after at least one monitor has been placed.

### B4. Low-medium: `flushBufferedNotes()` does not re-stamp `_sv`, so retried notes differ from live notes

`publishNote()` stamps `_sv = NOTEFILE_SCHEMA_VERSION` onto the body after re-parsing the serialized payload ([client L7220](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7220)). However, the payload that gets written to the offline buffer is the pre-`_sv` serialization, and `flushBufferedNotes()` re-sends it via `JParse(payload)` + `note.add` **without** adding `_sv` ([client L7368-7400](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7368) and the LittleFS branch at [L7482](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7482)).

Impact today is cosmetic: the server's `processNotefile()` does not currently read or require `_sv`, so notes flushed after an outage simply lack the version stamp. But this becomes a real interop hazard the moment the server (or v1.0 finding #3's proposed `_sv` validation) starts gating on schema version — buffered notes from before the upgrade would be treated as version 0.

Suggested fix: add `JAddNumberToObject(body, "_sv", NOTEFILE_SCHEMA_VERSION)` in both `flushBufferedNotes()` branches, mirroring `publishNote()`. Cleanest is to stamp `_sv` into the document *before* serialization so the buffered copy already carries it.

### B5. Low: With `minLevelChangeInches == 0`, there is no periodic telemetry heartbeat

In `sampleMonitors()` ([client L4980-4988](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4980)) telemetry is sent only when:

```cpp
const bool thresholdEnabled = (threshold > 0.0f);
const bool changeExceeded = thresholdEnabled && (fabs(inches - lastReportedInches) >= threshold);
if (needBaseline || changeExceeded) { sendTelemetry(...); }
```

When the change threshold is configured to 0 (change-based telemetry "disabled"), the only telemetry ever sent is the one-time baseline; after that, the sensor's live value reaches the server only via the once-daily report. The server's 24-hour delta tracking ([L11952](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11952)) and freshness/stale detection then operate on near-daily data only.

This may be intentional (the server comment at the daily snapshot explicitly says it exists "even when change-based telemetry is disabled"), but the behavior is easy to misconfigure into: a tank that is perfectly steady reports essentially once per day, and "last update" age on the dashboard can routinely approach 24h. Consider an optional maximum-telemetry-interval heartbeat (e.g. send at least every N hours regardless of change) so steady sensors still prove liveness more often than daily.

### B6. Low: History pruning assumes timestamps are monotonic, which pre-time-sync snapshots violate

`recordTelemetrySnapshot()` stamps `snap.timestamp = now`, where `now` is `0.0` if `gLastSyncedEpoch == 0.0` ([L7858-7864](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L7858)). `pruneHotTierIfNeeded()` ([L7877](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L7877)) counts how many snapshots are `>= cutoffEpoch` and then simply sets `snapshotCount = newCount`, relying on the comment's assertion that "timestamps are monotonically increasing, so all pruned entries are contiguous at the start."

That invariant holds for the normal case, but snapshots recorded before the first time sync carry `timestamp == 0.0`. If any are interleaved with real-time snapshots (e.g. a Notecard time loss/clock reset mid-run), the "kept == most recent newCount" assumption breaks and the wrong entries are logically discarded, because the function reduces the count without compacting against the actual surviving indices.

Suggested fix: either drop/skip recording snapshots while `now == 0.0` (no reliable time yet), or have the prune actually compact surviving entries by index rather than trusting positional monotonicity.

---

## What Looks Solid (re-verified this pass)

- `processNotefile()` peek → parse → handle → delete ordering with poison-note deletion after 3 parse failures ([L11709](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11709)).
- Client alarm rate limiting separates relay actuation (always runs) from Notecard transmission (rate-limited), with per-monitor + global hourly caps and unsigned-underflow guards ([client L5251](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5251)).
- `publishNote()` handles oversized payloads via heap fallback, captures Notecard `err`, and buffers on failure ([client L7150](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7150)).
- Current-loop and analog reads use multi-sample averaging and retain the previous reading on read failure (`sampleReused`) instead of emitting a spurious zero.
- Server current-loop conversion correctly mirrors the client including pressure-unit factors, specific gravity, ultrasonic, and gas handling.
- Alarm hysteresis/debounce and persistent-relay-after-reboot restoration are well structured.
- Daily part-loss detection (received-parts bitmask + completeness check) is a genuinely useful backup-path integrity check ([server L12515+](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12515)).

---

## Suggested Fix Order (this pass)

1. A1 + B1 — Stop writing `rec->levelInches` as history height; derive height from config and treat it as immutable per sensor. Highest data-quality impact.
2. A2 — Make `convertVoltageToLevel()` mirror the client analog pressure/SG/gas math.
3. B2 — Add `st`/`h` to alarm payloads and add a config fallback for `sensorType` in `handleAlarm()`.
4. B3 — Fix daily-report part-0 header crowding so no monitor is silently dropped.
5. B4 — Re-stamp `_sv` on flushed buffered notes (stamp before serialization).
6. B5 / B6 — Optional telemetry heartbeat; guard history against pre-time-sync / clock-reset timestamps.

## Closing Assessment

The wireless and sensor pipeline is well defended at the transport layer (health checks, buffering, poison handling, rate limiting). The residual risk is concentrated in *semantic* mismatches between producers and consumers: the client and server disagree on analog conversion (A2), the three server ingest paths disagree on what `heightInches` means (A1/B1), and the alarm/daily payloads are not self-describing enough for the server to decode them after state loss (B2, plus v1.0 #5). None are architectural rewrites; each is a localized correctness fix. Addressing items 1–4 above would resolve the data-integrity issues that most directly affect what operators see on the dashboard and in SMS alerts.
