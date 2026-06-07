# Code Review: 112025 Server and 112025 Client v1.7.2

**Date:** June 7, 2026  
**Review Version:** v1.2  
**Reviewed Firmware:** v1.7.2  
**Notefile Schema:** Schema Version 2  
**Reviewer:** GitHub Copilot  
**Scope:** Static source review of the 112025 Server and Client firmware, focused on wireless messaging, sensor collection and processing, payload transmission, and server-side ingestion.

---

## Executive Summary

The v1.7.2 firmware has several strong architectural improvements over the older server/client split. The client now uses a centralized `buildSensorObject()` payload path for telemetry, alarms, and daily reports, and the server's `resolveLevel()` model correctly treats the client-sent `lvl`, raw sensor values, `cap`, `st`, and calibration version `cv` as a self-describing data contract. The current-loop live-zero guard and `isfinite()` validation are also important production hardening improvements.

The remaining risks are concentrated around wireless command intake, identity/schema validation, and payload size boundaries. The highest priority fixes are to stop destructively consuming command notes before validation, apply a consistent inbound schema/source policy on the server and client, raise or redesign the config/offline-buffer payload limits, and close several cases where failed or invalid sensor acquisition can still look like valid data.

No firmware changes, compile, or upload were performed as part of this review.

---

## Severity-Ranked Findings

### 1. High: Client command inboxes delete messages before validation or successful execution

**Locations:**

- `pollForRelayCommands()` in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7874)
- `pollForSerialRequests()` in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L8429)
- `pollForLocationRequests()` in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L8573)
- `pollForSyncRequests()` in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L8642)

These inboxes call `note.get` with `delete=true` before deserialization, target/schema validation, and successful command execution. The config inbox uses the safer peek-then-delete pattern, but relay, serial, location, and sync commands do not.

Impact:

- A malformed JSON command, transient parse failure, or handler failure permanently drops the command.
- Relay, serial, location, and sync commands mostly trust route delivery rather than validating `_target`, `target`, `_type`, and `_sv` in firmware.
- A route misconfiguration or manual test injection can cause a device to consume a note intended for another device and delete it before the intended device sees it.

Recommended fix:

- Change all command inboxes to peek first and delete only after successful validation and execution.
- Add a shared inbound validator that checks schema (`_sv <= NOTEFILE_SCHEMA_VERSION`), expected command type, and target UID when present.
- Keep malformed-but-not-owned notes in the queue or route them to a quarantine/dead-letter path rather than silently erasing them.

---

### 2. High: Server and client trust body-supplied identity without a uniform source/auth/schema policy

**Locations:**

- `processNotefile()` parses and dispatches without `_sv` validation in [../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11766)
- `isValidClientUid()` exists in [../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L1343), and `upsertSensorRecord()` uses it in [../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12716)
- Metadata-only paths bypass the UID validator: system alarms in [../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12021), daily reports in [../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12303), serial logs/ACKs in [../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L3417), location responses in [../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L3517), and config ACKs in [../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L13988)
- Relay forwarding accepts body-supplied target/source UIDs in [../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L3478)

The server correctly validates UIDs before inserting sensor records, but several support paths create metadata, serial buffers, relay commands, or ACK state from `doc["c"]` or `doc["client"]` without the same validation. Inbound note schema is also not checked centrally; parsed notes are handed to handlers and then deleted.

Impact:

- Malformed or spoofed UIDs can consume limited metadata slots (`MAX_CLIENT_METADATA` is 20) and serial-log slots (`MAX_CLIENT_SERIAL_LOGS` is 5).
- A forged config ACK with a matching body UID/version can clear pending config dispatch state for a real client.
- Relay-forward notes can ask the server to send commands to arbitrary body-supplied targets if the route accepts the note.
- Future schema notes can be accepted and deleted even when the receiving firmware does not understand the payload semantics.

Recommended fix:

- Add a central `validateInboundNote(doc, expectedType, requireValidUid)` check before handler execution.
- Apply `isValidClientUid()` to every server path that reads `c`, `client`, `target`, or `_target`, not only sensor records.
- Reject or quarantine `_sv > NOTEFILE_SCHEMA_VERSION` on both server and client.
- For stronger source assurance, include a per-device shared HMAC or validate Notehub-provided route/device envelope metadata before trusting body-supplied client IDs.

---

### 3. Medium-High: Server config dispatch can be blocked by the 1536-byte config snapshot cache

**Locations:**

- `ClientConfigSnapshot.payload[1536]` in [../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L983)
- `dispatchClientConfig()` serializes into an 8192-byte buffer in [../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11570)
- `cacheClientConfigFromBuffer()` rejects payloads that do not fit the 1536-byte snapshot in [../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L14639)

The server can build a config payload up to almost 8 KB, including learned calibration coefficients, but it must cache that payload into a 1536-byte `ClientConfigSnapshot` before dispatch. If the cache write fails, dispatch returns `PayloadTooLarge` and the config is never sent.

Impact:

- Larger real-world configurations with multiple sensors, names, battery/solar settings, and calibration coefficients can be impossible to sync.
- Calibration version mismatch can persist because the client never receives the updated coefficients.
- Retry machinery cannot help because the config fails before it reaches the Notecard outbox.

Recommended fix:

- Increase the snapshot payload to match the largest allowed dispatch payload, or move per-client cached configs to LittleFS files and keep only metadata in RAM.
- Add a web/API warning when serialized config size approaches the configured limit.
- Add a regression test that builds a max-monitor config with calibration fields and verifies dispatch/cache/load survives reboot.

---

### 4. Medium-High: Offline note recovery discards buffered notes over 1024 bytes

**Locations:**

- `publishNote()` supports a 2048-byte static buffer and heap fallback in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7247)
- `bufferNoteForRetry()` writes full serialized notes to `/fs/pending_notes.log` in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7362)
- `flushBufferedNotes()` reads with `char lineBuffer[1024]` in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7396)

Live publishing can serialize notes larger than 1024 bytes, but the offline recovery path cannot replay them. On Mbed/Opta, a truncated line is detected, the remainder is drained, and the note is skipped. The non-Mbed branch also uses a 1024-byte fixed line buffer and can truncate payloads.

Impact:

- Large daily reports produced during outages can be stored successfully but lost when connectivity returns.
- The data most likely to exceed the buffer is exactly the backup data intended to recover from weak cellular conditions: daily reports with sensor payloads, power state, signal, battery, solar, and alarm summary.

Recommended fix:

- Use a replay buffer at least as large as the maximum serialized payload that `publishNote()` can generate, or store each pending note as a separate length-prefixed file.
- Log a persistent diagnostic note when a buffered note cannot be replayed.
- Add an offline replay test with payloads above 1024, 2048, and the chosen maximum size.

---

### 5. Medium: Daily report partitioning can permanently skip a monitor

**Locations:**

- `DAILY_NOTE_PAYLOAD_LIMIT` is 960 bytes in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L844)
- `sendDailyReport()` increments `monitorCursor` after both normal and relaxed append failures in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L6938)
- `appendDailyMonitor()` removes the failed monitor object when `measureJson(doc) > payloadLimit` in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7094)

When a monitor does not fit in the current daily-report part, the code tries a relaxed payload limit. If that also fails, it logs the skip and still advances `monitorCursor`. Part 0 is especially vulnerable because it carries VIN, solar, battery, power, signal, and alarm metadata before sensor entries.

Impact:

- The skipped sensor is never sent in that daily report.
- The server's daily report completeness bitmask can still show all parts present because it does not know a monitor was skipped before serialization.
- This weakens the daily report as the backup path for missed telemetry or alarm notes.

Recommended fix:

- Do not advance `monitorCursor` when a monitor fails to fit.
- Publish the current non-empty part, start a clean next part without bulky part-0 metadata, and retry the same monitor.
- Include an explicit monitor count or skipped-monitor counter so the server can flag incomplete daily content.

---

### 6. Medium: All-failed current-loop sample sets reuse stale levels without raising a sensor fault

**Locations:**

- `readCurrentLoopSensor()` returns `gMonitorState[idx].currentInches` when `validSamples == 0` in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4875)
- `sampleMonitors()` validates the returned value and only marks `sampleReused` after a validation failure in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5096)
- Telemetry timestamps use `lastReadingEpoch` when available in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5376)

The timestamp handling is good: reused samples do not get a fresh acquisition time. The problem is that a total current-loop I2C read failure returns the previous valid level, which usually passes `validateSensorReading()` and therefore does not increment `consecutiveFailures` or set `sensorFailed`.

Impact:

- A disconnected or unreachable current-loop expansion module can leave the client evaluating alarms and relay state against stale data.
- The server may not receive a `sensor-fault` alarm even though acquisition has stopped.
- If the stale level remains within thresholds, the field condition can drift while the dashboard appears stable.

Recommended fix:

- When `validSamples == 0`, return `NAN` or explicitly increment the same failure path used by non-finite readings.
- If stale reuse is desired for display continuity, send a diagnostic field such as `stale=true`, `ageSec`, or `sampleReused=true` so the server can mark the reading as stale.
- Consider separate counters for acquisition failure and physical-range failure.

---

### 7. Medium: Learned calibration temperature compensation can switch between live server temperature and stale client temperature

**Locations:**

- Client applies server-pushed learned calibration and static `calTempF` in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4975)
- Server injects calibration temperature into config in [../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11557)
- Server `resolveLevel()` trusts client `lvl` when `clientCv == serverCv` in [../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L13335)

When a client has stale calibration, the server recalculates level from raw mA using live `getCachedTemperature()`. After the client receives the calibration, it applies the pushed `calTempF`, which is the temperature at config dispatch time. The server then trusts the client `lvl` because `cv` matches.

Impact:

- The same physical sensor can jump when the client catches up because the server changes from live temperature compensation to the client's static temperature compensation.
- Accuracy degrades in locations with large ambient temperature swings if the config is not refreshed often.

Recommended fix:

- For current-loop sensors with non-zero `calTempCoef`, have the server continue applying live temperature compensation from raw mA even when `cv` matches.
- Alternatively, include a temperature/version field in each sensor note and only trust client `lvl` when the temperature basis is current enough.
- Document the intended tradeoff if static client compensation is an accepted field behavior.

---

### 8. Medium: Invalid sensor configuration can collapse into valid-looking zero readings

**Locations:**

- `linearMap()` returns `outMin` for degenerate input ranges in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4580)
- `readAnalogSensor()` returns `0.0f` for invalid sensor/voltage ranges in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4822)
- `readCurrentLoopSensor()` returns `0.0f` for invalid current-loop ranges in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4875)
- `readMonitorSensor()` returns `0.0f` for unknown sensor interfaces in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5062)

The newer `validateSensorReading()` correctly rejects `NAN` and infinities, but several invalid configuration paths still return `0.0f`. That value is plausible for an empty tank, inactive float switch, stopped engine, or zero flow.

Impact:

- Bad or corrupted config can be indistinguishable from a legitimate empty/zero reading.
- The server receives clean-looking `lvl` values and cannot infer that acquisition or configuration failed.
- Low-level alarms or relay actions could trigger from configuration defects rather than real fluid state.

Recommended fix:

- Return `NAN` for invalid ranges and unknown interfaces so the existing non-finite validation path raises `sensor-fault`.
- Include a config validation pass when applying server config; reject monitors with degenerate ranges before they enter the sampling loop.

---

### 9. Medium-Low: Daily alarm reconciliation does not clear orphaned alarms when the client has no active alarms

**Locations:**

- Client daily report only creates `alarms` when at least one alarm is active in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7011)
- Server daily reconciliation only runs inside `if (isFirstPart && dailyAlarms)` in [../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12377)

The intended daily backup path clears server-side orphaned alarms when a clear note was lost. However, if the client currently has no active alarms, it omits the `alarms` array entirely. The server interprets absence of the array as "do nothing" rather than "no active alarms".

Impact:

- If a clear note is lost and the next daily report has no active alarms, the server can keep showing a stale alarm indefinitely.
- The daily report backup path works for missed alarm-on notes better than for missed alarm-clear notes.

Recommended fix:

- Emit an empty `alarms: []` array in part 0 of every daily report, or have the server treat a missing `alarms` field on schema-2 first parts as an empty array.
- Add a schema/version gate before changing this behavior to avoid misinterpreting legacy daily reports.

---

### 10. Low: Relay command cooldown is runtime-only and does not prevent replay after reboot

**Locations:**

- `processRelayCommand()` uses a static `lastRelayCommandMillis` cooldown in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7923)

The relay command cooldown is useful during normal runtime, but it is not persisted. A reboot, watchdog reset, or power cycle clears the cooldown. Because command inboxes currently delete on read, this is less likely for the same note, but replayed or duplicated route deliveries after reboot would not be rejected by cooldown state.

Impact:

- Remote relay commands can execute sooner than intended after a reset.
- Manual or route-level duplicate command injection has no durable command ID/timestamp dedupe.

Recommended fix:

- Include a command ID and creation timestamp in relay commands.
- Persist a short replay cache or last-command timestamp per relay.
- Reject stale commands older than a conservative window.

---

## Areas That Look Sound

- `FIRMWARE_VERSION` is consistently set to `1.7.2`, and `NOTEFILE_SCHEMA_VERSION` is `2` in [../TankAlarm-112025-Common/src/TankAlarm_Common.h](../TankAlarm-112025-Common/src/TankAlarm_Common.h#L18).
- Client `publishNote()` stamps `_sv` before serialization, so offline-buffered notes retain the schema version.
- `buildSensorObject()` centralizes `ot`, `mu`, `st`, raw sensor values, `lvl`, `cap`, and `cv`, preventing telemetry/alarm/daily field drift.
- Server `resolveLevel()` correctly re-applies server calibration when the client `cv` is stale and raw mA is present.
- Current-loop live-zero detection (`CURRENT_LOOP_FAULT_MA`) correctly returns `NAN` for under-range non-gas transmitters, and `validateSensorReading()` catches non-finite values.
- Config ACKs are deferred until after `saveConfigToFlash()` succeeds, and the server keeps pending dispatch until a matching ACK version arrives.
- Server sensor records are protected by `isValidClientUid()` through `upsertSensorRecord()`, so the main telemetry table is better guarded than metadata/support paths.
- Watchdog kicks are present around long Notecard and current-loop operations.

---

## Recommended Priority Order

1. Convert all client command inboxes to peek-validate-execute-delete.
2. Add shared inbound schema/source/UID validation on server and client.
3. Raise or redesign server config snapshot storage so valid large configs can dispatch.
4. Redesign offline note replay to handle the same payload size that live publishing supports.
5. Fix daily report partition retry so monitors are never skipped silently.
6. Treat all current-loop acquisition failure and invalid sensor configuration paths as faults, not valid zero/stale values.
7. Decide the calibration temperature model: always server-live, client-current-temperature, or explicitly static and documented.
8. Treat missing schema-2 daily `alarms` as empty or always send `alarms: []`.
9. Add relay command IDs/timestamps for durable replay protection.

---

## Suggested Verification Tests

1. **Command inbox safety:** Inject relay, serial, location, and sync notes with wrong `_target`, unsupported `_sv`, invalid JSON, and valid payloads. Verify wrong/future/bad notes are not executed and valid notes are deleted only after success.
2. **Server UID hygiene:** Send system alarm, daily, serial log, location response, config ACK, and relay-forward notes with non-`dev:` UIDs. Verify no metadata, serial buffer, ACK, or relay command state is created.
3. **Large config dispatch:** Build a max-monitor config with long names, battery/solar settings, and learned calibration fields. Verify cache, dispatch, reboot load, ACK, and retry all work.
4. **Offline replay:** Force the client offline, buffer daily reports above 1024 bytes, restore the Notecard, and verify every buffered note is replayed.
5. **Current-loop total failure:** Simulate all four mA samples failing. Verify the client raises `sensor-fault`, includes stale/acquisition status, and does not evaluate relay/alarm transitions from stale data without marking it stale.
6. **Daily clear reconciliation:** Drop a `clear` note, send a daily report with no active alarms, and verify the server clears the orphaned alarm.
7. **Temperature compensation:** Compare server-displayed levels before and after a calibration config ACK when NWS temperature changes materially from the pushed `calTempF`.
