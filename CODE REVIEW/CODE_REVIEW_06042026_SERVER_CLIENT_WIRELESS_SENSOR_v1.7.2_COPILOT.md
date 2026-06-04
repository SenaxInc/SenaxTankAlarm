# Code Review: 112025 Server and 112025 Client

Date: June 4, 2026  
Review artifact version: v1.0  
Firmware reviewed: v1.7.2  
Notefile schema reviewed: 2  
Reviewer: GitHub Copilot

## Scope

Reviewed the current 112025 Server and Client firmware with emphasis on:

- Wireless Notecard messaging between server and clients.
- Client sensor collection, validation, local processing, alarm generation, and payload construction.
- Transmission and retry behavior for telemetry, alarm, daily, relay, config, serial, location, and sync notes.
- Server-side ingestion, calibration reconciliation, level/capacity processing, history snapshots, and config dispatch.

Primary source files reviewed:

- `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino`
- `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino`
- `TankAlarm-112025-Common/src/TankAlarm_Common.h`
- `TankAlarm-112025-Common/src/TankAlarm_I2C.h`
- `TankAlarm-112025-Common/src/TankAlarm_Solar.cpp`
- `TankAlarm-112025-Common/src/TankAlarm_Battery.h`
- `CODE REVIEW/CODE_REVIEW_06032026_IMPLEMENTATION_PLAN.md`

This was a static review. I did not modify firmware source and did not rerun a compile as part of this review.

## Executive Summary

The current v1.7.2 tree is materially stronger than the earlier v1.6.14 review baseline. The highest-risk data-integrity issues from the June 3 review appear fixed or architecturally removed:

- The common firmware version is now `1.7.2`, and the shared notefile schema is `2` in `TankAlarm-112025-Common/src/TankAlarm_Common.h:19` and `TankAlarm-112025-Common/src/TankAlarm_Common.h:25`.
- Client payload generation is now centralized in `buildSensorObject()` at `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:5333`, with `st`, raw value fields, `lvl`, `cap`, object type, measurement unit, and calibration version where available.
- The server now uses `resolveLevel()` at `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:13335`, trusting the client-computed `lvl` unless a current-loop calibration exists and the client's `cv` is stale.
- The previous analog-voltage reconversion bug is effectively removed by the new client-level/server-reconciliation model.
- The previous historical height bug is fixed in the current handlers: telemetry/alarm/daily now use client-reported `cap` instead of passing current level as capacity.
- `_sv` is stamped into the JSON before serialization in `publishNote()`, so buffered notes retain schema version.
- Current-loop under-range readings below the live-zero fault threshold are rejected before learned calibration can turn a disconnected sensor into a plausible level.

The remaining review findings are mostly reliability hardening and edge cases, not broad architectural failures. The top items to address are client inbound command validation, offline note buffer record sizing, and the server config snapshot capacity mismatch.

## Severity-Ranked Findings

### 1. Medium-high: Client command inboxes still trust Notehub routing too much

Source:

- Shared command route contract: `TankAlarm-112025-Common/src/TankAlarm_Common.h:153`, `TankAlarm-112025-Common/src/TankAlarm_Common.h:174`, `TankAlarm-112025-Common/src/TankAlarm_Common.h:191`, `TankAlarm-112025-Common/src/TankAlarm_Common.h:204`, `TankAlarm-112025-Common/src/TankAlarm_Common.h:217`, `TankAlarm-112025-Common/src/TankAlarm_Common.h:260`
- Client config inbox: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:3900`
- Client relay inbox: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:7874`
- Client relay command processing: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:7923`
- Client serial request inbox: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:8429`
- Client location request inbox: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:8573`
- Client sync request inbox: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:8642`
- Server sends `_target` and `_type` in command notes: `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:3580`, `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:10980`

The server deliberately sends all server-to-client commands through `command.qo`, with `_target`, `_type`, and `_sv` stamped into the body. Notehub routes then deliver those commands to `config.qi`, `relay.qi`, `serial_request.qi`, `location_request.qi`, or `sync_request.qi`.

The client still largely trusts the route result. The config handler applies the note without checking `_type`, `_target`, or `_sv`. Relay, serial, location, and sync handlers request the note with `delete=true`, so the note is consumed before the client proves the command is the expected type, targeted to this device, and valid enough to execute. `processRelayCommand()` checks `target` if present, but the server sends `_target`; if a route ever preserves the underscore field, the local target check is bypassed because `target` is absent.

Impact:

- A Notehub route misconfiguration can turn into device action instead of a clean rejection.
- A wrong-type or wrong-device note can be deleted before a retry or diagnostic path can inspect it.
- Future schema changes are harder to diagnose because `_sv` is stamped but not enforced.
- The risk is mitigated by the route design, but the receiver still lacks defense in depth.

Suggested fix:

- Add a shared client helper such as `validateInboundCommand(doc, expectedType)`.
- Validate `_sv <= NOTEFILE_SCHEMA_VERSION` and log/reject unsupported future schema versions.
- Validate `_type` when present, and require it for schema 2+ command notes.
- Validate both `target` and `_target`; if either is present and non-empty, it must match `gDeviceUID`.
- Switch relay, serial, location, and sync inboxes to peek-then-delete, matching the safer config pattern.
- Delete a note only after successful handling or after a deliberate poison-note policy decision.

### 2. Medium-high: Offline note flush still cannot preserve notes larger than 1024 bytes

Source:

- Client payload serialization and heap fallback: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:7247`
- Client offline append format: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:7362`
- Client flush path: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:7396`
- Daily report builder: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:6938`
- Per-monitor daily object builder: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:7094`

`publishNote()` can serialize payloads larger than the 2048 byte static buffer by allocating a dynamic buffer. `bufferNoteForRetry()` then writes each pending note as one tab-delimited text line.

`flushBufferedNotes()` still reads pending lines into `char lineBuffer[1024]`. On the Mbed/POSIX path, an over-length line is detected, the rest of the line is consumed, and the note is skipped. On the LittleFS path, the fixed buffer can parse a truncated JSON prefix and then treat the remainder as a separate malformed line. Either way, the over-length buffered note is lost instead of preserved for retry.

Impact:

- During a cellular/Notecard outage, a larger daily report or diagnostic payload can be buffered successfully but lost when connectivity returns.
- This weakens the exact recovery path intended to protect weak-signal deployments.
- The current unified payload is more compact and better structured than before, but the storage format still cannot safely round-trip the maximum payload size that `publishNote()` accepts.

Suggested fix:

- Replace the tab-delimited line format with length-prefixed records, for example `fileName`, `syncFlag`, `payloadLength`, then exact payload bytes.
- Alternatively, size the flush buffer from the maximum accepted note payload plus filename/sync overhead.
- If a record is too large, preserve it and log a clear error rather than consuming and dropping it.
- Add a bench test that buffers a daily note larger than 1024 bytes, simulates Notecard recovery, and confirms the note is sent or remains queued.

### 3. Medium: Server config snapshot cache is still much smaller than the dispatch buffer

Source:

- Snapshot payload size: `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:983`
- Dispatch serialization buffer: `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:11583`
- Cache size rejection: `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:14639`

`dispatchClientConfig()` serializes the augmented config into an 8192 byte static buffer, including injected learned-calibration coefficients. It then calls `cacheClientConfigFromBuffer()` before dispatching. `ClientConfigSnapshot.payload` is still only 1536 bytes, and the cache function rejects payloads that do not fit.

Impact:

- A valid config that fits the dispatch serialization buffer can still be rejected before it is sent.
- More monitors, longer labels/site data, solar/battery fields, relay settings, and calibration fields all push toward this limit.
- The new architecture no longer relies on cached config for normal display conversion as heavily as v1.6.14 did, but the cache is still on the send path, so this remains a config-delivery blocker.

Suggested fix:

- Raise `ClientConfigSnapshot.payload` to match the dispatch maximum, if RAM budget allows.
- Preferably store each client config snapshot in its own LittleFS file and keep only UID/site/version/status metadata in RAM.
- Add a UI/API preflight estimate that warns when an edited config approaches the transport or cache limit.

### 4. Medium-low: Daily report can skip a monitor if one entry cannot fit even as a relaxed solo entry

Source:

- Daily report loop: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:6938`
- Per-monitor append sizing: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:7094`

When `appendDailyMonitor()` cannot fit the current monitor, `sendDailyReport()` retries with `DAILY_NOTE_PAYLOAD_LIMIT + 48`. If that still fails while no monitor has been added to the current part, the code logs `Daily report entry skipped; payload still exceeds limit` and increments `monitorCursor`.

Impact:

- The monitor is permanently skipped for that daily report.
- This is probably rare with the current compact payload, but it is silent at the server side and can matter for a verbose monitor combined with first-part metadata.
- Because daily reports are the backup path for missed telemetry/alarm notes, silent omission is undesirable.

Suggested fix:

- Do not advance `monitorCursor` until the monitor is actually published or explicitly marked unsendable.
- Retry an oversized monitor on a fresh part without first-part metadata.
- If it still cannot fit alone, publish a compact error note naming the monitor and keep a local log entry so the omission is visible.

### 5. Low-medium: `_sv` is stamped but not validated inbound

Source:

- Common schema version: `TankAlarm-112025-Common/src/TankAlarm_Common.h:25`
- Client stamps `_sv` before serialization: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:7247`
- Server stamps command schema version via `stampSchemaVersion()` before dispatch: `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:3580`, `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:10980`

Schema version stamping is now in the right place for outbound notes, including buffered notes. However, neither side consistently rejects or warns on unsupported inbound schema versions.

Impact:

- A future schema 3 command could be accepted by a schema 2 client and processed partially.
- Old or malformed v1-style notes can continue through default paths in places where a hard migration decision would be clearer.

Suggested fix:

- For command/config inboxes, reject `_sv > NOTEFILE_SCHEMA_VERSION`.
- For server ingestion, accept schema 1/2 only where legacy handling is explicitly intended, otherwise log and reject.
- Add an operator-visible log when `_sv` is missing on a file that should now be schema 2.

### 6. Low-medium: Server trusts body-supplied client UID without source authentication

Source:

- Server ingestion dispatcher: `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:11766`
- Telemetry handler uses `doc["c"]` as client identity: `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:11863`
- Alarm handler uses `doc["c"]` as client identity: `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:12021`
- Daily handler uses `doc["c"]` as client identity: `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:12303`

The server creates or updates client/sensor records from the body-provided `c` value. This is normal for the current route architecture, but it means identity is a soft field inside the payload rather than a verified envelope or signature.

Impact:

- A compromised device or misrouted note could spoof another client's UID in telemetry/alarm/daily bodies.
- Spoofing could contaminate dashboard state, trigger SMS, or alter history for the wrong client.

Suggested fix:

- If Notehub route metadata exposes the originating device UID to the server-side note, compare it against body `c` before processing.
- If not, add a lightweight HMAC or per-device shared secret for client-originated notes.
- At minimum, enforce `isValidClientUid()` at the top of every server handler and log body UID anomalies.

### 7. Low: Sensor validation still allows some fragile edge cases

Source:

- `linearMap()` returns `outMin` for degenerate ranges: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:4580`
- Validation lower bound for native-range sensors is `-maxValid * 0.1f`: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:4629`

The new `isfinite()` validation gate and current-loop under-range guard are good fixes. Two smaller validation issues remain:

- `linearMap()` returns `outMin` when `inMin` and `inMax` are effectively equal, which can make a bad range configuration look like a valid minimum reading.
- Gas pressure validation still derives a negative lower bound from `-maxValid * 0.1f`. Current gas conversion paths clamp negative pressure to zero, so this is not usually observable today, but the validation rule is still loose for a pressure sensor.

Impact:

- Bad range configuration can produce plausible-looking readings instead of faulting.
- Future gas sensor paths that return raw pressure before clamping could allow negative pressure through validation.

Suggested fix:

- Return `NAN` from `linearMap()` for degenerate ranges and let `validateSensorReading()` raise a sensor fault.
- For gas monitors, use `minValid = max(0.0f, sensorRangeMin - tolerance)` rather than a negative percentage of maximum pressure.

## Wireless Messaging Review

What looks good:

- The `.qo`/`.qi` notefile model is centralized and well documented in `TankAlarm_Common.h`.
- Server-to-client command traffic is consolidated through `command.qo` with `_target`, `_type`, and `_sv` metadata.
- Client-to-server data traffic now carries self-describing sensor data: raw value, `lvl`, `cap`, `st`, object type, unit, and calibration version.
- Server `processNotefile()` uses a strong peek/process/delete pattern and has poison-note cleanup after repeated parse failures.
- Config ACK version matching is present, and unknown-client ACKs are logged in the current source.
- Config ACKs are deferred until client persistence succeeds, which protects against acknowledging a config that was not actually saved.

Main improvements:

- Add receiver-side `_type`, `_target`/`target`, and `_sv` validation to all client command inboxes.
- Convert relay, serial, location, and sync request handling to peek-then-delete.
- Add source authentication or route-envelope UID checks for server ingestion.
- Replace line-based offline buffering with a length-prefixed record format.

## Sensor Collection and Client Processing Review

What looks good:

- `buildSensorObject()` has removed most per-path payload drift between telemetry, alarm, and daily reporting.
- Client analog and current-loop conversions remain the authoritative local conversion path for uncalibrated readings.
- Learned current-loop calibration coefficients are now pushed from the server to the client, so client alarm thresholds and server display can agree.
- Current-loop under-range readings below `CURRENT_LOOP_FAULT_MA` now return `NAN` before calibration is applied.
- `validateSensorReading()` rejects non-finite readings before range checks.
- Digital/current-loop/analog/pulse sensor paths are still cleanly separated.
- I2C current-loop reading now checks request/availability before using returned bytes.
- Watchdog kicks are present in longer sampling/flush loops.

Main improvements:

- Return `NAN` for degenerate map ranges instead of reporting `outMin`.
- Tighten gas pressure lower-bound validation.
- Preserve and surface oversized daily/report notes instead of skipping or dropping them.

## Server Ingestion and Processing Review

What looks good:

- The old duplicate-converter architecture has been replaced by `resolveLevel()`.
- `resolveLevel()` correctly preserves learned current-loop calibration authority when a client's `cv` is stale.
- Telemetry, alarm, and daily handlers all use the self-describing payload fields and store `cap` for history.
- `handleAlarm()` and `handleDaily()` recover `sensorType` from `st`, so registry/cache loss no longer forces level decode to zero.
- System alarm SMS rate limiting is now per-client rather than one fleet-wide static timestamp.
- `processNotefile()` peeks first, parses, invokes the handler, and deletes only after successful processing.
- Calibration injection into outbound config is present before serialization/caching.

Main improvements:

- Fix the config snapshot capacity mismatch.
- Add route-envelope or HMAC identity checks before trusting body `c`.
- Add clear logging/metrics for schema mismatches and rejected source identity.

## Suggested Fix Order

1. Add client inbound command validation plus peek-then-delete for relay, serial, location, and sync requests.
2. Replace the offline pending-note line format or resize it to safely preserve maximum accepted payloads.
3. Increase or externalize the server client-config snapshot cache.
4. Fix the daily-report oversized-monitor cursor advance so a monitor is not silently skipped.
5. Add inbound `_sv` policy and logging for schema mismatches.
6. Add server-side source authentication or route-envelope UID verification.
7. Tighten `linearMap()` and gas validation edge cases.

## Suggested Tests

Recommended focused tests:

- Misrouted `command.qo` body delivered to each client inbox with wrong `_type`, wrong `_target`, missing `_sv`, and future `_sv`.
- Relay, serial, location, and sync command notes retained when handling fails before execution.
- Buffered daily/telemetry note larger than 1024 bytes, followed by Notecard recovery and flush.
- Worst-case 8-monitor config with long names, solar/battery/relay settings, and learned calibration coefficients enabled.
- Daily report with one intentionally oversized monitor entry; verify it is retried or reported, not silently skipped.
- Calibrated current-loop sensor where client `cv` matches server, then where client `cv` is stale.
- Empty server registry/cache followed by alarm and daily notes only; verify `lvl`, `cap`, and `st` recover display state correctly.
- Degenerate sensor range configuration; verify it produces a sensor fault rather than a plausible minimum value.

## Closing Assessment

The v1.7.2 server/client pair is in much better shape than the earlier v1.6.14 review target. The biggest data correctness problems around analog conversion, history capacity, self-describing daily/alarm payloads, schema stamping, and calibration synchronization are either fixed or removed by the new architecture. The remaining work is mostly about making the wireless layer less dependent on perfect Notehub routing and making offline persistence capable of preserving every payload the live path can produce.
