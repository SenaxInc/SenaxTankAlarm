# Code Review: 112025 Server and 112025 Client

Date: June 3, 2026  
Review version: v1.0  
Firmware version reviewed: 1.6.14 (`FIRMWARE_VERSION` in `TankAlarm-112025-Common/src/TankAlarm_Common.h`)  
Reviewer: GitHub Copilot

## Scope

Reviewed these firmware areas:

- `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino`
- `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino`
- `TankAlarm-112025-Common/src/TankAlarm_Common.h`
- `TankAlarm-112025-Common/src/TankAlarm_I2C.h`
- `TankAlarm-112025-Common/src/TankAlarm_Solar.cpp`
- `TankAlarm-112025-Common/src/TankAlarm_Battery.h`
- `TankAlarm-112025-Client-BluesOpta/DEVICE_TO_DEVICE_API.md`

Primary focus was wireless Notecard messaging between the server and clients, sensor data collection and processing on the client, transmission payloads, and server-side ingestion/conversion/history processing.

This was a static code review. I did not edit firmware source and did not rerun a compile as part of this review pass.

## Executive Summary

The overall system has a solid architecture: the client sends compact raw sensor payloads through standard `.qo` outboxes, the server polls `.qi` inboxes, command traffic is consolidated through `command.qo`, and there is meaningful defensive work around Notecard health, I2C recovery, config ACKs, relay rate limiting, stale clients, and history persistence.

The most important issues are data quality and wireless reliability problems:

1. Server analog voltage conversion does not mirror the client pressure-to-height conversion, so analog pressure sensors can be materially wrong on the server/dashboard.
2. Historical `heightInches` is often missing, defaulted, or replaced with current level, corrupting percentages and chart capacity for telemetry, alarm, and daily records.
3. Client command inboxes trust Notehub routing too much and often delete inbound command notes before validating `_type`, `_target`, schema version, or successful handling.
4. Offline note buffering can store payloads larger than the flush buffer can read, which can strand or drop larger daily reports after an outage.
5. Daily report sensor entries are not self-describing enough to be decoded reliably after server state loss.

## Findings

### 1. High: Server analog sensor conversion is wrong for pressure-based liquid level

Source:

- Client analog conversion: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:4741-4785`
- Client telemetry sends only raw voltage: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:5232-5235`
- Server analog conversion: `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:13384-13435`
- Server ingestion calls conversion from telemetry/daily/alarm paths: `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:11940-11958`, `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:12479-12497`

The client treats analog sensors as pressure/voltage sensors and converts native pressure to inches of fluid using `sensorRangeUnit`, `sensorMountHeight`, and specific gravity. Gas monitors are handled specially as raw pressure.

The server receives only `vt` for analog sensors and recomputes the value from the cached client config, but `convertVoltageToLevel()` only maps voltage into `sensorRangeMin..sensorRangeMax` and adds `sensorMountHeight`. It does not apply pressure unit conversion, specific gravity, or the gas/liquid distinction.

Impact:

- A 0-5 PSI analog pressure sensor on a liquid tank can be reported as about 0-5 inches plus mount height instead of about 0-138 inches for water, or adjusted by SG for diesel/propane/brine/etc.
- Dashboard values, SMS text, history, daily summaries, and calibration screens can be wrong for analog liquid sensors.
- The client computes a correct local value, but that value is not transmitted in telemetry/daily payloads, so the server's incorrect conversion wins.

Suggested fix:

- Make `convertVoltageToLevel()` mirror the client `readAnalogSensor()` logic:
  - map voltage to native pressure/value;
  - if `monitorType == "gas"`, return native pressure;
  - otherwise convert pressure units to inches of water and divide by fluid SG;
  - add `sensorMountHeight`;
  - clamp below zero.
- Add bench vectors for at least PSI, inH2O, bar/kPa, gas pressure, and one non-water SG case.

### 2. High: Historical height/capacity is missing or replaced with current level

Source:

- Client has an unused height helper: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:2104-2130`
- Client telemetry does not send `h`: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:5191-5249`
- Client daily sensor entries do not send `h`: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:6966-7014`
- Server telemetry defaults missing `h` to 48.0: `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:11973-11980`
- Server alarm path passes current level as height: `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:12138-12144`
- Server daily path passes current level as height: `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:12504-12510`
- `recordTelemetrySnapshot()` stores the height directly: `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:7836-7856`

The history layer expects `heightInches` to be the sensor/tank capacity used for percentages and charts. The client already has `getMonitorHeight()`, but it is only declared and defined, not used. As a result:

- telemetry history uses `doc["h"]` and falls back to 48.0 because the client does not send `h`;
- alarm history stores `rec->levelInches` as the height;
- daily history stores `rec->levelInches` as the height immediately after setting it to `newLevel`.

Impact:

- Percent-full math, sparklines, monthly summaries, and exported history can show incorrect capacity.
- Alarm and daily snapshots can make every reading look like 100 percent full because height equals level.
- Telemetry snapshots for tanks taller or shorter than 48 inches silently use the wrong capacity.

Suggested fix:

- Add `h = roundTo(getMonitorHeight(cfg), 1)` to client telemetry and daily sensor objects.
- On the server, add a helper to derive height from cached config when `h` is missing.
- In `handleAlarm()` and `handleDaily()`, pass the derived height to `recordTelemetrySnapshot()`, never `rec->levelInches`.
- Preserve gas/flow/engine semantics: for gas, use full-scale pressure; for digital, use 1.0; for pulse/flow, either omit height or use a type-appropriate full-scale value.

### 3. Medium-high: Client command inboxes do not validate `_type`, `_target`, or schema consistently

Source:

- Server stamps command metadata: `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:3589-3590`, `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:3654-3655`, `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:10932-10933`, `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:11077-11078`, `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:11498-11499`
- Client config receive path: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:3853-3920`
- Client relay receive path: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:7781-7834`
- Client serial request path: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:8337-8380`
- Client location request path: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:8472-8528`
- Client sync request path: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:8540-8588`

The server deliberately sends `command.qo` messages with `_target` and `_type`, relying on Notehub routes to deliver them to `config.qi`, `relay.qi`, `serial_request.qi`, `location_request.qi`, or `sync_request.qi`.

The client handlers mostly trust that route result:

- `pollForConfigUpdates()` applies any valid JSON in `config.qi` without requiring `_type == "config"` or checking `_target`.
- `processRelayCommand()` checks `target`, but the server sends `_target`; if Notehub preserves `_target`, this local check is bypassed.
- relay, serial, location, and sync handlers use `delete=true` before they know the command is valid or for the current device.
- `_sv` is stamped by senders but not validated by receivers.

Impact:

- A route misconfiguration or preserved metadata field can cause a client to process a command meant for another inbox or device.
- A malformed or wrong-type command can be consumed and deleted before a retry path can recover it.
- Future schema changes can fail silently because `_sv` is not checked.

Suggested fix:

- Add a shared client helper such as `validateInboundCommand(doc, expectedType)` that checks:
  - `_sv` is supported;
  - `_type` is absent only for legacy compatibility or equals the expected type;
  - `_target` or `target`, when present, equals `gDeviceUID`.
- Use peek-then-delete for relay, serial, location, and sync requests, matching the config path's safer pattern.
- In relay processing, read both `doc["_target"]` and `doc["target"]`.
- Consider logging and leaving unknown schema versions undeleted until a safe retry/poison-note threshold is reached.

### 4. Medium-high: Offline note buffering can lose larger payloads during flush

Source:

- Client can serialize notes into a 2048 byte static buffer and larger heap buffer: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:7156-7183`
- Buffered notes are appended as one tab-delimited line: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:7259-7276`
- Flush uses a 1024 byte line buffer and skips truncated lines: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:7303-7345`, `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:7454-7480`
- Daily report builder is designed to approach a payload limit and can exceed 1 KB with multiple monitors: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:6810-7015`

`publishNote()` can buffer notes larger than 1024 bytes, especially daily reports, but `flushBufferedNotes()` reads pending lines into `char lineBuffer[1024]`. On the Mbed/POSIX path, truncated lines are explicitly skipped; on the non-Mbed path, the fixed buffer can also fail to preserve long lines correctly.

Impact:

- After an outage, larger buffered daily reports can be skipped instead of sent.
- The file can appear to have buffered data, but those notes never reach the server.
- This undermines the retry system exactly when cellular or Notecard reliability is degraded.

Suggested fix:

- Store pending notes as length-prefixed records instead of tab-delimited lines, or allocate a flush buffer based on the line length.
- At minimum, make the flush line buffer at least as large as the maximum accepted payload plus filename/sync overhead.
- When a line is too large, preserve it and log a clear error rather than silently skipping it.

### 5. Medium: Daily report sensor entries are not self-describing after server state loss

Source:

- Telemetry includes `st`: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:5222-5239`
- Daily explicitly omits `st`: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:7012-7014`
- Server daily handler relies on `rec->sensorType`: `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:12479-12497`

Daily reports include raw readings (`ma`, `vt`, `fl`, `rm`) but do not include the sensor interface type `st`, and they also do not include height `h`. The server daily handler uses the existing sensor record's `sensorType` to decide how to decode the raw field.

Impact:

- If the server loses or evicts registry/config state, a daily report alone may not decode the sensor value and can leave `newLevel` at 0.0.
- Daily reports are intended as a backup path for weak signal and missed telemetry; not being self-contained weakens that backup.

Suggested fix:

- Add `st` and `h` to each daily sensor entry.
- Optionally add current display value `l` as a backward-compatible fallback so the server can show a reasonable value even if config conversion is unavailable.
- Keep raw values as canonical inputs for calibrated conversion, but make daily notes resilient enough to recover server state.

### 6. Medium: Current-loop power gating failure is logged but sampling continues

Source:

- `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:4812-4840`

When `cfg.pwmGatingEnabled` is true, `readCurrentLoopSensor()` attempts to enable sensor power through `tankalarm_setPwm()`. If the PWM/I2C command fails, it logs a warning but continues into the sample loop.

Impact:

- A sensor can be read while unpowered or partially powered.
- Depending on wiring and residual voltage, this can either reuse old readings, report failures, or produce misleading intermittent samples.
- This also obscures root cause because the measurement path proceeds after a known power-gate failure.

Suggested fix:

- Track a `pwmEnabled` flag.
- If power gating is required and the ON command fails, skip sampling, mark `sampleReused`, increment the appropriate I2C/power-gate failure counter, and leave/force the sensor state invalid for that sample.
- Still attempt PWM OFF cleanup if the ON command may have partially succeeded.

### 7. Medium: Config snapshot cache is much smaller than the dispatch buffer

Source:

- Snapshot payload buffer is 1536 bytes: `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:978-984`
- Dispatch serialization uses 8192 bytes: `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:11535-11539`
- Cache rejects payloads at the 1536 byte limit: `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:11544-11546`, `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:14686-14699`

`dispatchClientConfig()` can serialize up to 8 KB, but it must cache the config before sending. `ClientConfigSnapshot.payload` is only 1536 bytes, so larger multi-monitor configs fail before Notecard dispatch.

Impact:

- Larger client configurations can be rejected as "payload too large" even though the immediate send buffer could hold them.
- This can block fleet scaling to more monitors or richer per-monitor settings.

Suggested fix:

- Increase the snapshot payload capacity to match the dispatch buffer, or store each client config in its own LittleFS file and keep only metadata in RAM.
- Add a UI/API warning that estimates serialized config size before submission.
- Add tests for worst-case 8-monitor configs with relay, solar, battery, and calibration fields enabled.

### 8. Low-medium: Gas pressure validation allows negative values by construction

Source:

- `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:4587-4604`

For gas monitors, validation sets `maxValid = sensorRangeMax * 1.1f`, then falls through to `minValid = -maxValid * 0.1f`. The actual analog/current-loop gas conversion paths clamp negative pressure to 0.0, so this is not usually observable with current code. It is still a fragile validation rule for pressure sensors.

Impact:

- If a future sensor path returns raw pressure directly, negative pressure can pass validation.
- It weakens fault detection for gas monitors.

Suggested fix:

- For `OBJECT_GAS`, set `minValid` from the configured sensor range, usually `max(0.0f, sensorRangeMin - tolerance)`, and keep `maxValid = sensorRangeMax * 1.1f`.

## Wireless Messaging Review

What looks good:

- The notefile naming model is clear and centralized in `TankAlarm_Common.h`.
- Client-to-server files use standard `.qo` outboxes and server `.qi` inboxes.
- Server-to-client commands use one `command.qo` with `_target` and `_type`, which is the right shape for route-based delivery.
- Config delivery has ACK tracking through `_cv`, pending dispatch state, purge of stale config notes, and retry limits.
- Server `processNotefile()` peeks first, parses, calls the handler, and deletes only after processing. It also has poison-note deletion after repeated parse failures.
- Client publish path captures Notecard errors and buffers on failure.
- Notecard health checks include I2C bus recovery and modem stall restart logic.

Main improvements:

- Add receiver-side `_type`, `_target`, and `_sv` validation before acting on every command inbox.
- Use peek-then-delete for all inbound client commands, not only config.
- Fix the offline note buffer flush size/format so buffered daily reports actually survive outages.
- Add a small schema table in code or docs that lists each notefile, required fields, optional fields, producer, and consumer.

## Sensor Collection and Client Processing Review

What looks good:

- The client separates digital, analog, current-loop, and pulse sensor reading paths cleanly.
- Pressure-based liquid conversion correctly uses unit conversion and specific gravity on the client.
- Gas monitors are kept as pressure, not incorrectly converted to tank inches.
- Current-loop reads use multi-sample averaging and store raw mA for telemetry.
- Sensor validation includes range checks, consecutive failure thresholds, stuck-sensor detection, and recovery notification.
- Alarm evaluation includes hysteresis/debounce and separates relay actuation from Notecard rate limiting.
- I2C recovery logic in the common library uses timeouts, SCL toggling, STOP generation, and recovery counters.
- Solar and battery helpers are conservative, with verified register gating and chemistry-aware thresholds.

Main improvements:

- Treat PWM gate-on failure as a failed sample instead of continuing to read.
- Include `h` and `st` in daily payloads, and include `h` in telemetry.
- Consider including the client's computed display value as a non-authoritative fallback field for server recovery and diagnostics.
- Tighten gas validation lower bounds.

## Server Ingestion and Processing Review

What looks good:

- `processNotefile()` uses a safe peek/process/delete pattern.
- Sensor records use a hash table with fallback linear scan and rebuild, which is a good robustness measure.
- Server stores firmware versions, signal metadata, VIN voltage, object type, measurement unit, alarm state, and history snapshots.
- Current-loop server conversion mostly mirrors the client, including SG and gas handling.
- Daily alarm reconciliation is a strong backup path for lost alarm or clear notes.
- Stale client/orphan sensor handling and SMS rate limiting are thoughtful.

Main improvements:

- Fix `convertVoltageToLevel()` to mirror analog pressure conversion.
- Fix all `recordTelemetrySnapshot()` call sites to pass real height/capacity.
- Make daily reports self-describing enough to rebuild records after server cache loss.
- Consider making missing config snapshots visible in dashboard/system status, since raw mA/voltage conversion depends on them.

## Suggested Fix Order

1. Fix server analog conversion and add test vectors. This directly affects displayed sensor values.
2. Add `h` to telemetry/daily and fix `recordTelemetrySnapshot()` call sites. This protects historical charts and percentages.
3. Harden inbound command validation and switch all client command inboxes to peek-then-delete.
4. Replace or resize the offline pending note buffer format so larger daily reports flush correctly.
5. Add `st` to daily report sensor entries and optional fallback display value.
6. Increase or externalize the server client-config snapshot payload cache.
7. Treat PWM gating failure as a failed sample.
8. Tighten gas lower-bound validation.

## Notes on Possible Tests

Recommended focused tests:

- Analog pressure conversion parity between client and server for PSI, inH2O, kPa, bar, water SG, diesel SG, and gas mode.
- History snapshot height for telemetry, alarm, and daily inputs.
- Misrouted command notes in each client inbox with wrong `_type` and wrong `_target`.
- Buffered daily report larger than 1024 bytes, with Notecard unavailable, followed by recovery and flush.
- Daily report ingestion after clearing server sensor registry/config cache.
- Current-loop sensor with PWM gate ON returning false.

## Closing Assessment

The codebase shows many signs of active hardening, especially around Notecard health, I2C recovery, config ACKs, relay safety, and stale data handling. The highest-risk remaining issues are not architectural failures; they are mismatches between otherwise good components: client versus server conversion math, telemetry payload fields versus history expectations, and route metadata sent by the server versus validation performed by the client.
