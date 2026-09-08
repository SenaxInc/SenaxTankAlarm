# Repository Review - 2026-09-08

## Findings at a Glance

**Recommendation: do not deploy the current dirty client/Common working tree until its removed functionality is reconciled.** All four primary sketches compile, but compilation does not detect the confirmed alarm, calibration, persistence, and interface defects below. This is a review, not a firmware repair or release.

| Priority | ID | Finding |
| --- | --- | --- |
| High | W-01 | Local changes remove the DAC loop-power schema and implementation |
| High | W-02 | Local changes restore offline alarm-discard gates |
| High | F-01 | Multipart daily report part 1 can clear active alarms |
| High | F-02 | Dashboard Clear Relay can address the wrong monitor |
| High | F-03 | Calibration splits real device UIDs incorrectly and submits the wrong identity |
| High | F-04 | Analog alarm debounce accepts nonconsecutive trigger/clear samples |
| High | F-05 | Alarm latching can suppress the only notification of an episode |
| High | F-06 | Pulse sampler is not serviced throughout its acquisition window |
| High | F-07 | Late clock synchronization leaves scheduled work permanently unarmed |
| High | F-08 | Deleting the last client is not durable; failed saves lose retry state |
| High | F-09 | Snooze is broadcast before saving; save failure still returns success |
| High | F-10 | Failed config ACK clears pending delivery; same-second revisions collide |
| High | F-11 | Delayed/backlogged notes can overwrite newer values and alarm state |
| High | F-12 | Fault validity is inconsistent across telemetry, daily, persistence, and viewer paths |
| High | F-13 | Restore cannot round-trip normal backup sizes or coherently reload runtime state |
| Medium | W-03 | Local changes defeat sensor recovery backoff and restore repeated setpoint probes |
| Medium | W-04 | Local changes remove on-demand telemetry, acquisition timestamps, and the OTA build guard |
| Medium | F-14 | Shared save timestamp can indefinitely starve metadata persistence |
| Medium | F-15 | Relay timeout overwrites the active alarm type and stops reminders |
| Medium | F-16 | Oversized buffered notes are silently discarded on replay |
| Medium | F-17 | FTP backup blocks normal server work for at least 8m40s in a complete FTPS pass |
| Medium | F-18 | Unknown battery voltage bypasses power-state transition side effects |
| Medium | F-19 | Historical sensor selection resets immediately; custom dates lack change handling |
| Medium | F-20 | History mixes physical quantities and labels all readings as inches |
| Medium | F-21 | Several website pages overflow phone viewports substantially |
| Medium | F-22 | Chart CDN failure discards usable history data and throws again in error handling |
| Medium | F-23 | Dashboard stale label disagrees with its 49-hour threshold |
| Medium | F-24 | Post-handler note deletion has no delivery deduplication; poison tracker is undersized |
| Medium | F-25 | Provisioning can report keys programmed without checking flash results |
| Medium | F-26 | OptaView accepts insufficiently validated Modbus responses and mis-scales signed current |
| Medium | F-27 | HTML editing and screenshot tooling do not validate the shipped web experience |
| Medium | F-28 | Invalid settings requests can mutate runtime configuration before returning 400 |
| Medium | F-29 | Dashboard's 24-hour delta can retain an arbitrarily old baseline |

High means plausible missed alarms, incorrect control/calibration, or loss of durable operational state under an identified trigger. Medium means a bounded correctness, reliability, usability, or efficiency defect. Findings are source-confirmed unless an executable reproduction is explicitly stated. No hardware failure rate or exploitability is inferred from source alone.

## Baseline and Coverage

- Repository: `SenaxInc/SenaxTankAlarm`, `master`, firmware v2.2.14, build sequence 301.
- Initial HEAD: `9c750a8`. Fetched and fast-forwarded to `939fa91`; the six upstream commits changed generated binaries, the Common archive, and preview timestamps, not the reviewed source behavior.
- Seven tracked files were already modified: the CI workflow, two older OTA review documents, the client sketch, and three Common source files. Three July review documents and two compile logs were already untracked. Those changes were preserved and are excluded from this review commit.
- W-series findings concern the pre-existing uncommitted code changes. F-series findings concern code present in the reviewed current sources and, except where explicitly noted, are not introduced by those local changes. Client line references refer to the current working copy; reconcile their positions when reading pristine `master`.
- Detailed review: production server/client/viewer sketches; Common storage, time, I2C, solar, battery, and OTA paths; server's 14 page routes; build/release/preview workflows; HTML editing utility; FTPS test helper; provisioning; OptaView.
- Diagnostic and historical material was sampled for consistency and maintenance hazards. Generated binaries, every archived review, and every bench sketch were not reverse-engineered or exhaustively line-reviewed. The StorageMigration directory is empty. A repository-wide review is not proof that no further defects exist.
- No deployed website was exercised, no board was flashed, no relay/partition/provisioning operation was executed, and no live SMS/email/Notehub request was sent. Browser writes terminated at a loopback fixture server with synthetic data.

## Existing Local Changes

### W-01 - DAC Loop Power Has Been Replaced by an Older PWM-Only Contract

High. [Client monitor configuration](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L699), [current-loop reader](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5440), [Common I2C implementation](../TankAlarm-112025-Common/src/TankAlarm_I2C.h).

The existing diff removes `loopPowerEnabled`, `loopPowerMode`, `sensorMinVoltage`, DAC initialization, bipolar DAC-loop conversion, and the soft-ramp helpers, replacing them with older `pwmGating*` fields. Current server configuration still emits the newer loop-power contract. A DAC-powered installation can receive a syntactically valid configuration that this working client ignores, then operate the wrong power path. The version still says v2.2.14, obscuring that behavioral regression.

Action: reconcile the local edits against committed v2.2.14 before deployment; preserve schema migration and the separately validated DAC/PWM conversions. Do not blindly restore the entire file or discard the user's work. Test saved and pushed configs for external, DAC, and PWM power modes; bench-verify the selected physical output and current conversion.

### W-02 - Offline Alarms Again Bypass the Existing Retry Buffer

High. [sendAlarm](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L6354), [publishNote](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L8164).

The local diff reinstates `gNotecardAvailable` gates in alarm, unload, solar/battery, power-transition, and sunset paths. `publishNote` already buffers when the Notecard is unavailable. Skipping it loses the event instead of buffering it; an alarm can remain latched after communications recover without resending its initial notification. This is a host-to-Notecard outage issue, distinct from ordinary cellular outages while the Notecard remains accessible.

Action: restore unconditional invocation of the buffering publisher for alarm-class events, while retaining availability guards for direct Notecard I/O. Test alarm and clear transitions during a simulated Notecard outage and verify ordered replay.

### W-03 - Recovery Backoff and Setpoint Probe Limits Have Been Removed

Medium. [Sensor-only recovery](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L2052), [solar setpoint polling](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L505).

The removed successful-read watermark matters: recovery itself zeroes `consecutiveFailures`, causing the next loop to reset backoff and total attempts without a real successful reading. The circuit breaker therefore repeatedly restarts. The diff also removes the rejection limit for CRC-valid but implausible setpoints, restoring three unnecessary register reads per poll on incompatible controller revisions. At one poll/minute, that is about 4,320 extra Modbus transactions/day before any function-code retries. The measured nominal-voltage classification was also removed while the field remains declared.

Action: reset recovery budgets only on a proven acquisition, not a software counter reset; distinguish transport failures from genuine under/over-range sensor values. Keep a bounded setpoint capability probe and explicitly re-arm on controller/configuration changes.

### W-04 - On-Demand Updates, Freshness Metadata, and OTA Build Protection Regress

Medium. [Client inbound polling](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L2175), [daily sensor serialization](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L8010), [client build guard](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L40).

The local diff removes `pollForTelemetryRequests` although the dashboard still queues those requests. It removes each daily sensor's acquisition `t`, so the server falls back to report time for reused readings. It also changes the missing-MCUboot build error into a warning, allowing an apparently successful USB build that cannot apply future OTA updates. Some old VS Code build tasks omit that flag and use stale copied libraries.

Action: preserve the command consumer, acquisition timestamps, and explicit non-OTA opt-out guard. Add a request-update round-trip test and a negative compile test for accidental non-OTA client builds. These removals should not be bundled into a review-only commit.

## High-Priority Correctness Findings

### F-01 - Daily Part 1 Can Clear an Active Alarm

[Server first-part detection and reconciliation](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L13080), [client daily producer](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7844).

The client uses zero-based parts and includes `alarms` only in part 0. The server accepts both `part == 0` and `part == 1` as first parts for legacy compatibility, then interprets absent `alarms` on schema-2+ first parts as no active alarms. In a multipart report, part 1 therefore clears alarms that part 0 just confirmed, including their reminder snooze state.

Reproduction: part 0 `{p:0,_sv:2,alarms:[{k:1,hi:true,lo:false}]}` followed by part 1 `{p:1,_sv:2}` leaves sensor 1 clear in the source-matched model. This does not require packet reordering.

Suggested correction: distinguish legacy numbering using a protocol/version rule, not `0 || 1` for every sender. For the current schema, reconcile only part 0 with an explicit alarm-summary contract. Test metadata-only part 0, ordinary multipart reports, missing/reordered parts, and legacy reports.

### F-02 - Clear Relay Uses Presentation Order as Device Identity

[Dashboard mapper](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2310), [button serialization](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2323), [server forwarding](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11197), [client interpretation](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L8896).

`buildSiteModel` sets `sensorIdx` from the array position in `cs[].ts[]`. Those records follow server registry/arrival order, not the client's configuration order. The command is forwarded unchanged as `relay_reset_sensor`, which the client uses as a monitor-array index.

Executed reproduction: registry order `[k:2,k:1]` produces reset position 0 for sensor 2, although sensor 2 occupies configuration position 1. Sparse reports or a reboot backlog can create this ordering. This is a wrong-control-target risk, not merely a display issue.

Action: make the API carry the stable sensor number and resolve it against `gConfig.monitors[].sensorIndex` on the client. Provide a versioned compatibility path for existing index-based commands. Test reordered, missing, and deleted sensors; never infer identity from a rendered card's position.

### F-03 - Calibration Corrupts Device/Sensor Keys

[Dropdown and unit lookup](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2181), [submission and log filter](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2185), [backend validation](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L19465).

Keys are constructed as `clientUid:sensorIndex`, but `split(':')` takes the first two fields. A real key such as `dev:860000000000001:1` becomes client `dev` and sensor `860000000000001`, not the intended UID and sensor 1. A numeric device ID may then be rejected/coerced to an unrelated 8-bit sensor. Backend validation checks nonempty UID, not valid/existing sensor identity.

Browser reproduction with an alphanumeric test UID: selecting a GAS sensor kept feet/inches inputs visible, and submission produced `{"clientUid":"dev","sensorIndex":null,"verifiedLevelInches":12,...}`. The real handler rejects the null sensor; the mock's success response was not treated as backend success. Log filtering has the same parsing error.

Suggested shared page helper:

```js
function parseSensorKey(value) {
	const separator = value.lastIndexOf(':');
	const clientUid = value.slice(0, separator);
	const sensorIndex = Number(value.slice(separator + 1));
	if (separator < 1 || !clientUid.startsWith('dev:') ||
			!Number.isInteger(sensorIndex) || sensorIndex < 1 || sensorIndex > 255) {
		throw new Error('Invalid sensor selection');
	}
	return { clientUid, sensorIndex };
}
```

Use it in unit selection, submission, and filtering. Backend must validate the UID and resolve an existing sensor before changing calibration. Test a real numeric `dev:` UID, multiple sensor numbers, a true zero reference, pressure units, and malformed keys.

### F-04 - Analog Alarm Debounce Does Not Require Consecutive Samples

[evaluateAlarms](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5872).

The high trigger counter is not reset when a normal sample falls below the high-clear band while no high alarm is latched. The high-clear counter is not reset by a sample in the hysteresis band. The symmetric low path has the same pattern. Invalid/reused acquisitions can also reach alarm evaluation before `sensorFailed` is set.

Executed the actual function after a mechanical type/literal conversion, with high=80, low=20, hysteresis=5, debounce=3. `[90,50,90,50,90]` emitted HIGH; starting high-latched, `[70,78,70,78,70]` emitted CLEAR. Neither sequence contains three consecutive qualifying samples.

Action: explicitly reset each trigger/clear counter on every disqualifying fresh sample; define reused/invalid sample semantics and prevent them from counting as independent evidence. Test all four counters, threshold equality, hysteresis-band excursions, and interleaved invalid readings.

### F-05 - Latch-Before-Send Can Lose the Only Alarm Notification

[Boot timestamp initialization](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L1745), [alarm evaluation](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5872), [rate limiter](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L6210).

`evaluateAlarms` sets the latch before `sendAlarm` checks notification limits. A suppressed send is not pending anywhere, so subsequent samples in the same condition do not retry. The boot initializer sets last-high/low/fault times to zero when uptime is below five minutes, but the comment claiming unsigned subtraction then wraps is wrong: at 20 seconds, `20000 - 0 < 300000`. With short sample intervals, the first alarm can be suppressed and remain unsent until another episode or a daily summary. The daily recovery path currently latches state without dispatching the missing initial alert.

Action: separate physical alarm state, actuator state, and notification-pending state. An unsent initial event should remain pending under a bounded retry policy. Use an explicit never-sent flag or wrap-safe expired timestamp for the first notification. Test boot-in-alarm, rate-limited re-entry, offline buffering, and queue failure; keep local actuation independent of notification limits.

### F-06 - Pulse Acquisition Is Only Polled at Telemetry Sample Time

[Pulse sampler](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L1268), [result retrieval](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L1397), [only caller](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5695).

The nonblocking sampler requires repeated `pollPulseSampler` calls, but its only call is inside `readPulseSensor`, invoked by periodic sampling. There is no loop service or interrupt-backed capture that observes the intervening pulse edges. With a 30-minute reporting interval and a seconds-long measurement window, it counts only short bursts separated by long unobserved gaps, then divides by a duration that does not describe the observation. A pending sample returns an old reading without marking it reused.

Action: capture edges with an appropriate hardware timer/interrupt-backed counter and service the state machine independently of transmission cadence. Mark incomplete samples invalid/reused. Test known pulse trains, no pulses, low/high frequencies, long main-loop stalls, and the supported count range. Do not assume an occasional polling burst implements continuous accumulation.

### F-07 - Late Time Sync Does Not Arm Schedules

[Server startup](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L4397), [server clock/scheduler](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L8667), [client scheduler](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4224), [viewer scheduler](../TankAlarm-112025-Viewer-BluesOpta/TankAlarm-112025-Viewer-BluesOpta.ino#L807), [shared sync helper](../TankAlarm-112025-Common/src/TankAlarm_Notecard.h#L28).

At startup without time, scheduling stores zero. Later successful `ensureTimeSync` updates only the clock. Loop due checks require a nonzero next epoch and therefore never trigger the scheduling function again. The standard client has a no-time 24-hour fallback, but that fallback stops applying once time becomes valid, leaving its zero schedule stranded. Server daily email and viewer schedules have analogous gaps.

Failed time synchronization also retries on every loop because only successful sync updates its timestamp, bypassing the intent of Notecard health backoff.

Action: on the transition from invalid to valid time, arm all enabled zero schedules; track an independent last-attempt time and backoff while offline. Test startup with no time followed by synchronization, never-synchronized operation, and time correction after a long outage.

### F-08 - Last-Client Deletion Is Not Persisted; Save Failures Lose Dirty State

[Registry save](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L15612), [metadata save](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L15815), [delete handler](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L16650), [periodic saves](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L4810).

Both save functions return early when their count is zero. Deleting the final client therefore leaves the old nonempty files on disk; reboot reloads the removed records. Save functions return `void`, and the delete/periodic callers clear dirty flags even on unavailable storage, allocation failure, or write failure. Atomic writing is already present; the defect is the surrounding contract, not a missing rename strategy.

Action: persist empty arrays and return success/failure. Clear dirty flags only on success, retaining bounded retries and visible persistence errors. Test deleting the last versus one of several clients, reboot, and injected failures. Historical data retention after deletion should be an explicit separate policy.

### F-09 - Snooze Notice and Success Precede Durable State

[applyReminderSnooze](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L14566), [HTTP handler](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L14591), [registry save](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L15612).

`applyReminderSnooze` changes RAM and broadcasts before its caller saves. `saveSensorRegistry` cannot report failure, yet the handler returns `success: true`. An operator can receive a paused notice and success, then lose that snooze after reboot. Even with healthy storage, blocking notification transactions enlarge the pre-save interruption window. The SMS path also applies/broadcasts before its eventual batch save.

Action: stage/mutate, durably persist, then notify and acknowledge. Specify rollback or an explicit applied-but-not-persisted error on save failure. Preserve idempotent no-op behavior and avoid advancing unrelated rate-limit buckets. Test interruption between each stage, offline recipients, duplicate commands, unsnooze interval anchoring, and recovery auto-reset. The v2.2.14 boot hash insertion fix is present and should be preserved.

### F-10 - Config Failure Can Be Acknowledged as Complete

[ACK handler](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L16255), [client revision gate](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4594).

The server clears pending state for a matching `cv` regardless of `st`; `st:"failed"` ends automatic retry. The client truncates `_ts` to whole seconds and skips `inboundTs <= configEpoch`, acknowledging the offered version as applied even when a different configuration was generated within the same second. A later ACK can thus claim a configuration that never took effect. Orphan pruning additionally checks `status == applied` without requiring the ACK to match the current snapshot version.

Action: require an applied/persisted status and the expected revision to clear pending or prune. Retain explicit failure state. Use a monotonic revision with stored content/version identity so retries are idempotent and two same-second changes remain distinguishable. Test failed persistence, stale ACKs, two updates in one second, and crash between RAM apply and persistence.

### F-11 - Note Arrival Order Can Regress Sensor and Alarm State

[Telemetry update](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12647), [alarm update](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12905), [daily reconciliation](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L13154), [client replay ordering](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L8250).

The ingestion paths do not reject an older event before mutating current state. Telemetry/alarm freshness uses the note-envelope time while history can use the acquisition `t`; daily uses per-sensor `t` where available. Different notefiles are drained separately, so arrival order is not event order. The client also queues a new successful live note before flushing old buffered notes, enabling old alarm/clear messages to arrive afterward.

Action: store separate acquisition, receipt, and alarm-transition epochs or per-client sequence numbers. Older valid samples may enrich history but must not overwrite the current snapshot or reverse a later alarm transition. Reconcile daily alarms only when the report is newer than the latest relevant alarm state. Test high/clear messages in both orders, delayed daily reports, and buffered replay after a current note.

### F-12 - Invalid Data Can Be Presented or Stored as Valid

[Telemetry validity handling](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12622), [diagnostic alarm handling](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12905), [daily validity](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L13296), [viewer summary](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L15002).

- Fault-only telemetry still runs `resolveLevel`, assigns the result, and records history. Without `ma/lvl/fl/rm`, the result is zero. The main card may say FAULT, but a zero can still enter trends.
- Daily ingestion does not update `sensorFault`, so a prior fault can persist after a good daily sample, or a daily-only fault can be absent from the display. Its raw-mA gate also differs from telemetry.
- Diagnostic fault/recovery notes produced by `validateSensorReading` contain no normal sensor payload. `handleAlarm` still assigns the resolved zero and updates freshness.
- `sensorFault` is deliberately not persisted, and viewer summaries omit it; reboot or viewing through the viewer can turn a known invalid value into an apparently numeric observation.
- Daily/alarm snapshots require `newLevel > 0` or `level > 0`, excluding valid empty-tank/zero-pressure observations. With change telemetry disabled, zeros can disappear from the trend entirely.

Action: make measurement validity explicit and common to all ingestion/export paths. Preserve last-good value/time separately from latest fault/receipt time. Never use positivity as a proxy for validity; zero is valid for these sensors. Persist the necessary quality metadata and send it to the viewer. Test good -> fault -> daily recovery -> reboot, zero values, reused data, and all sensor interfaces.

### F-13 - FTP Restore Is Not a Reliable Round Trip

[Restore implementation](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L7275), [boot reload sequence](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L4426), [backup size limit](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L206).

Backup accepts roughly 24 KiB per baseline file, but restore retrieves each into a 2 KiB stack buffer. Ordinary contacts/config/registry files can exceed that. Both backup and restore call the operation successful when any file succeeded, even if required files failed. Restored files are applied one by one, without an all-files validation/activation boundary. Boot restore reloads config snapshots/calibration but not the already loaded registry, metadata, history settings, hot history, or contacts cache, and does not coherently reinitialize networking/Notecard from restored settings. A subsequent dirty save can overwrite restored files from stale RAM.

Action: stage a manifest with sizes, schemas, checksums, and required-file status; validate the complete set; activate coherently and reload all affected owners or deliberately reboot. Use compatible capacities or streaming. Test >2 KiB files, missing required files, malformed JSON, interrupted restore, stale cache replacement, and reboot. Do not call a partial restore complete.

## Additional Correctness and Reliability

### F-14 - Metadata Save Can Starve Behind Registry Save

[Loop persistence gates](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L4810).

The registry branch sets `gLastRegistrySaveMillis = now` before the metadata branch checks that same interval. If registry dirtiness recurs at each interval, metadata never saves. A 12-cycle source-matched model performs zero metadata saves. Persist both owners under one due decision, or use independent timestamps, with per-write success tracking.

```cpp
const bool persistenceDue = now - lastPersistenceAttemptMs >= saveIntervalMs;
if (persistenceDue) {
	lastPersistenceAttemptMs = now;
	if (gSensorRegistryDirty && saveSensorRegistry()) gSensorRegistryDirty = false;
	if (gClientMetadataDirty && saveClientMetadataCache()) gClientMetadataDirty = false;
}
```

This sketch assumes the proposed boolean save APIs; it is not a drop-in patch. Alert anchors currently also have a five-minute reboot replay window, and the hourly timestamp ring/system limiter state is not fully persisted. Define durability and retry guarantees explicitly rather than saving every sensor sample synchronously.

### F-15 - Relay Timeout Disables an Active Alarm's Reminders

[Timeout branch](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12927), [reminder type gate](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L14659).

Timeout keeps `alarmActive` true but replaces `alarmType` with `relay_timeout`. Reminders only accept high/low/digital types, so a still-high tank stops reminding after this operational event. Preserve the condition's type and store/log the relay event separately. Test high -> relay timeout -> reminder due -> clear, including a snoozed episode.

### F-16 - Replay Capacity Is Smaller Than Accepted Publish Capacity

[Dynamic publisher](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L8181), [replay line limit and discard](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L8319).

`publishNote` accepts dynamically allocated payloads larger than 2,048 bytes, but `NOTE_REPLAY_LINE_MAX` is 2,304 including metadata. Longer buffered lines are skipped, not retained, and the skip warning is debug-only. This is a capacity mismatch; the current daily splitter usually limits daily parts, so not every daily report is affected.

Action: use one enforced payload contract, or length-prefixed/streamed replay that can retain every accepted note. Record rejected/oversized notes visibly. Test payloads at the boundary and oversized serial/diagnostic payloads, not only ordinary telemetry. Bound replay by elapsed service time as well as its current 20-note count.

### F-17 - FTPS Backup Makes the Server Unresponsive for Minutes

[Nine-file manifest](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L5585), [inter-file waits](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L7011), [loop backup wrapper](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L4861).

Eight 65-second waits total 520 seconds (8m40s), before connection/transfer time, for a full FTPS manifest pass. The delay occurs before checking whether each next file exists. The loop closes its web listener and calls backup synchronously; watchdog kicks prevent reset but do not service telemetry or alarms. Normal web requests also permit roughly five seconds for headers plus five for bodies, and neither parser requires a complete body before returning success.

Action: implement a cooperative job with transfer/wait states, return 202 plus job status to the browser, and service alarms between bounded steps. Keep the measured socket/TIME_WAIT constraint; do not blindly reduce the delay. Move absent-file checks ahead of waits. Add complete-body validation and short idle timeouts. Test backup under active notes/web polling and a client that stops sending mid-request.

### F-18 - Loss of Voltage Data Bypasses Power-State Recovery Logic

[updatePowerState](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7649).

When no source is available, the function directly sets NORMAL and returns. If the prior state was CRITICAL, it bypasses relay restoration, state-change logging/notification, transition timestamps, and debounce reset. Losing RS-485 data can therefore look like battery recovery while leaving side effects inconsistent. Debounce also counts loop iterations against cached voltage, not necessarily independent battery samples.

Action: represent voltage as known/fresh versus unknown. Choose an explicit fail-safe unknown-voltage policy and funnel all transitions through one side-effect path. Only independent fresh measurements should advance voltage debounce. Test CRITICAL -> source loss -> recovery, stale MPPT values, and Vin fallback. Confirm relay safety policy with the installation owner before changing it.

### F-19 - Historical Filters Do Not Survive Their Own Reload

[Filters and load path](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2197), [event handlers](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2205).

Sensor selection calls `loadHistoricalData`, which rebuilds both dropdowns without restoring their selected values. Browser test: selecting sensor 0 immediately returns to `all` with both datasets still plotted. A refresh/range change also loses the site choice. Custom date inputs have no change listener to redraw when edited. The single-sensor request path sets unlimited days but does not send the API's supported stable `sensor=UID:NUMBER` filter.

Action: key selection by stable sensor identity, preserve valid choices during population, pass the server filter, and redraw on date edits. Test reload, changed sensor order, deleted sensors, range changes, and timezone boundaries.

### F-20 - History Units and Series Identity Are Incorrect

[Chart/CSV formatting](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2199), [history serializer](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L17012).

The history API omits object type/unit, while the page hardcodes `Level (inches)`, feet/inches cards, inch deltas, and a CSV inches column. A pressure sensor displays in PSI on the dashboard but is plotted/exported as inches. The voltage chart concatenates all clients into one line rather than grouping by client. Sensor/range filtering is not consistently applied to alarm and voltage charts.

Action: include stable identity, physical quantity, unit, and quality in history. Group incompatible quantities, label/convert intentionally, and make one voltage series per client. CSV should include value and unit columns. The main history endpoint reads hot snapshots only despite warm/cold availability messaging; implement the requested tier retrieval or explicitly indicate the actual returned coverage.

### F-21 - Mobile Layout and Styling Are Inconsistent

[Shared CSS](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L1630), [Calibration](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2173), [History](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2188), [Site Configuration](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2380).

All 14 server page routes were checked with synthetic populated data at 320, 390, 768, and 1,440 CSS pixels. Representative document widths at a 390-pixel viewport:

| Page | Document width | Evidence |
| --- | ---: | --- |
| Dashboard / Contacts / Settings / Client Console | 375 | No document overflow at this width; scrollbar consumes space |
| Calibration | 723 | Wide table/content forces page width |
| Site Configuration | 593 | Header/actions and reference table force minimum width |
| Transmission Log | 516 | Table/control minimum width |
| Email Setup | about 781-796 | Code block/content minimum width |
| SMS Setup | about 767-782 | Code block/content minimum width |
| History after desktop-to-phone resize | 1,180 | Chart canvas/container retains oversized width; confirmed after two animation frames |

At 320 pixels, configuration/settings/email-format fields also reached 346 pixels and serial controls 373 pixels. Some first-load History cases fit better, so resize behavior must be tested separately. Headings, statistics (`stat-card`, `stat-box`, `stat-value`), buttons (`pill`, regular, small, inline-sized), and nested section cards vary between pages. Site Config uses a large H1 while neighboring operational pages use H2. Many controls and site-status dots are not keyboard-operable buttons.

Action: preserve the existing visual language, but consolidate spacing/type/button/status tokens, navigation, and page headings. Use `min-width:0` in grid/flex children, bounded chart containers, wrapping action rows, local scroll containers for wide tables/code, and stacked mobile data rows. Avoid hiding information with page-level `overflow-x:hidden`. Use semantic buttons with labels, focus styles, sufficiently large targets, and text in addition to status colors.

Illustrative starting point, to be verified per page rather than pasted globally:

```css
.content-column, .chart-container { min-width: 0; }
.chart-container { position: relative; width: 100%; height: 320px; }
.table-scroll, pre { max-width: 100%; overflow-x: auto; }
.actions { display: flex; flex-wrap: wrap; gap: 8px; }
```

Screenshot capture in this environment returned blank compositor images. Those images are excluded from the deliverable. Layout conclusions above come from DOM geometry and chart pixel inspection, not a claimed visual screenshot approval.

### F-22 - CDN Loss Breaks Local History Even with Valid API Data

[External script imports](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2188), [catch path](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2204).

Browser reproduction blocked jsDelivr while allowing all local requests. `Chart` was absent; render threw, the catch reset valid historical data to empty, and its second chart render threw `ReferenceError: Chart is not defined`. A local industrial dashboard should not report zero sensors because the browser lacks internet access.

Action: keep data loading separate from optional chart rendering. Show tables/export and an explicit chart-unavailable status when the library is unavailable. Consider an approved locally served/preinstalled chart bundle subject to flash budget and licensing, or a lightweight existing offline-capable renderer. Pin dependencies and add integrity checks where applicable. Test disconnected browser internet with a reachable local server.

### F-23 - Stale Threshold and Labels Disagree

[Dashboard stale constant and logic](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2273).

The label says `Stale (>25h)`, but `STALE_MIN = 2940` means 49 hours. A 26-48-hour-old reading is not counted stale despite the label. The dashboard also hardcodes its refresh interval rather than consistently honoring configured web refresh, and a never-fulfilled update request displays ETA zero indefinitely while retaining faster polling.

Action: expose thresholds/cadences from one API contract, derive the text from those values, distinguish receipt age from acquisition age, and expire update requests into a retryable failed state. Test 24h/25h/26h/48h/49h, no timestamp, clock skew, and request timeouts. Transmission Log similarly promises 100 entries while the server ring holds 50.

### F-24 - At-Least-Once Note Handling Lacks Idempotent Side Effects

[Notefile processing](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12160), [poison tracker](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12139).

Handlers run before a separate delete request whose failure is ignored. If delete fails or the MCU resets after sending, the same note can dispatch alerts again; clear notifications bypass the minimum interval. The handler returns no success result, so storage failures do not prevent consumption either. There are 13 inboxes but only 12 parse-failure tracker slots; a malformed note in the untracked file cannot reach the three-failure deletion policy.

Action: use durable event IDs/idempotency keys for notification side effects, observe delete errors, and distinguish applied/retryable/rejected handler results. Size trackers from the inbox registry. Prefer a bounded dead-letter record for unsupported/malformed notes over silently losing them. Test duplicate note IDs, delete errors, restart after send, and malformed notes in every inbox.

### F-25 - Key Provisioning Does Not Verify Key Writes

[applyUpdate](../TankAlarm-112025-KeyProvisioning/TankAlarm-112025-KeyProvisioning.ino#L215).

`flash.init`, both `flash.program` calls, and deinitialization results are ignored, but the summary always says keys were programmed. A QSPI-successful run can report overall readiness despite a failed key write. The reprovisioning fallback also rebuilds the whole MBR when OTA files are missing/wrong-sized, which is broader than repairing partition 2.

Action: check every flash return, read back the exact key regions, and base readiness on both key and storage verification. Preserve a valid partition table when only OTA files need repair; clearly confirm any broader destructive operation. No provisioning command was run. The repository explicitly accepts public Arduino signing keys for mechanical integrity, not authenticity; that documented trust choice is not presented here as an accidental secret leak.

### F-26 - OptaView's Modbus Success Checks Are Too Weak

[Transaction validation](../OptaView/OptaView.ino#L86), [probe](../OptaView/OptaView.ino#L136), [telemetry decode](../OptaView/OptaView.ino#L242), [write acknowledgement](../OptaView/OptaView.ino#L353).

CRC validity alone is treated as transaction success. Slave ID, expected function, exact frame length, byte count, and write address/count echoes are not validated consistently. A valid exception can stop the probe as a found device; a truncated-but-CRC-valid read can lead to reading uninitialized bytes. Live current scaling ignores `isCurrent` and interprets two's-complement values as unsigned (e.g. `0xFFF0` becomes about 158 A instead of a small negative current). Probe-selected settings are not clearly persisted/reported for subsequent default-slave commands.

Action: reuse the proven production transport/register helpers where appropriate, validate full request/response correspondence before decoding, distinguish exception responses, and cast signed current through `int16_t`. Use single-register/fallback behavior where the MRC-1 requires it. Add frame fixtures, negative-current cases, and explicit write validation/readback. Keep arbitrary register writes bench-only.

### F-27 - Website Tooling Can Produce Misleading Results

[HTML utility](../TankAlarm-112025-Server-BluesOpta/update_html.py#L20), [screenshot workflow](../.github/workflows/update-screenshots.yml#L44), [release workflow](../.github/workflows/release-firmware-112025.yml#L65), [CI workflow](../.github/workflows/arduino-ci-112025.yml#L67).

The HTML utility slices from the first raw-string opening to the last closing without joining intervening C++ literals. An executed Python test with mocked I/O preserved `)HTML" R"HTML(` inside the extracted script, so that standalone HTML is not the shipped page. The screenshot workflow seeds only `tankalarm_token`, whereas current pages also require `tankalarm_session`; it opens `file://` pages without local API fixtures, checks only a subset, skips missing HTML, and never asserts page errors, data contracts, or mobile overflow. A successful image job is not a website smoke test.

Build core/libraries and the external FTPS repository are not version-pinned, so rebuilding the same source later can differ. The uncommitted FTPS overlay step exists only in CI, not release; it is conditional on a vendor directory absent in this checkout. Confirm dependency capabilities rather than assuming that overlay was applied. Do not infer a release failure solely from the missing optional overlay: local production builds passed.

Action: share one deterministic literal-aware extractor/generator; validate every page script and relative route; serve fixtures over HTTP; seed the current dummy-auth contract; fail on missing pages/console errors and layout regressions. Pin the core/library/external Git revisions used by CI and releases. Verify MCUboot slot headers, size, link target, and bootloader compatibility, not just the first four magic bytes. Keep generated screenshots/binaries as CI/release artifacts where possible rather than frequent commits to `master`.

### F-28 - Rejected Settings Requests Partially Apply in RAM

[Settings mutation and validation order](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L18335).

Product UID, fleet, SMS policy, and viewer-enabled fields mutate `gConfig` before the handler validates `viewerNet`. An invalid viewer IP/gateway/subnet/DNS returns 400 after those mutations, skipping final persistence and device reinitialization. The website says the save failed, but some running settings have changed; the Notecard may still use the previous profile while RAM holds the new one.

Action: parse and validate into a candidate config first, then commit/persist it and apply required side effects under an explicit success contract. Test a request containing both a changed SMS/product setting and an invalid viewer address; rejection must leave RAM, persisted config, and device configuration unchanged. Apply the same principle to any multi-field handler with late validation.

### F-29 - The Dashboard Delta Is Not Reliably a 24-Hour Change

[Telemetry baseline update](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12653), [daily baseline update](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L13328), [displayed delta](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L10537).

The baseline advances when the gap from the immediately previous update is at least 22 hours, or initializes once if absent. With hourly or change-triggered updates, that gap never reaches 22 hours, so `previousValue` can remain the first baseline indefinitely while the UI labels the difference `/24h`. The history endpoint already contains a bracketing/interpolation approach that is closer to the intended semantics.

Action: derive the delta from timestamped history bracketing 24 hours ago, or maintain a time-based baseline independent of the latest update gap. Expose unavailable/insufficient history instead of a falsely precise delta. Test hourly reporting over several days, irregular sampling, a reporting gap, and reboot restoration.

## Performance, Energy, and Data Recommendations

| Priority | Improvement | Expected benefit and verification |
| --- | --- | --- |
| 1 | Decouple sensing, alert decisions, queued delivery, and maintenance | Alarm service latency remains bounded during backup, replay, configuration, and web load; measure worst-case loop/alert latency, not only average throughput |
| 1 | Preserve change thresholds and on-demand updates while adding explicit delivery state | Avoid repeated unchanged cellular notes without losing a failed baseline/alarm; compare queued bytes and delivery outcomes under outages |
| 1 | Keep bounded Modbus capability probes and real-success recovery backoff | Avoid approximately 4,320 unnecessary setpoint reads/day on unsupported maps and repeated recovery bursts; measure bus time/current on target hardware |
| 2 | Queue recipient notes first, then request sync once per alert batch where delivery policy permits | Up to 10 recipient notes currently each request sync, with retries; fewer redundant host commands, but actual modem-session savings must be measured because Notecard can coalesce syncs |
| 2 | Stream bounded history JSON and paginate/downsample by requested range | Server static RAM is 68%; `sendHistoryJson` builds an auto-growing document and a second full String. At 20 x 90 snapshots plus voltage/alarms this can exhaust remaining heap; measure peak free heap and response sizes at capacity |
| 2 | Cache parsed display/config data per client version | `/api/clients` repeatedly parses the same config for individual sensors and scans arrays; avoid repeated JSON allocation/work without duplicating authoritative state |
| 2 | Use conditional requests and static asset caching | HTML/CSS are repeatedly transferred with no-cache behavior; versioned shared assets/ETags reduce LAN traffic and MCU work. This is not automatically a cellular saving |
| 2 | Suspend or slow hidden-tab polling; share a refresh coordinator | Session checks, dashboard, serial logs, and history timers run separately. Avoid polling indefinitely at 10 seconds for expired update requests; retain rapid alarm refresh for visible users |
| 2 | Bound flash writes and record outcomes | Coalesce telemetry metadata, use durable event journals for critical changes, and skip unchanged files. Measure writes/day and preserve power-loss semantics; do not eliminate required durability to save wear |
| 2 | Make battery-state changes sample-driven, with explicit unknown/fresh states | Prevent repeated evaluation of cached values and spurious state changes; instrument acquisition epochs, transition counts, and real current draw |
| 3 | Keep alarms fast while normal telemetry is sparse | Three debounce samples at the default 30-minute cadence can delay an alarm roughly 60-90 minutes; low-power multipliers extend this. Agree on maximum detection latency and sample/confirm alarms independently of upload interval |
| 3 | Give long-running operations explicit jobs and progress | Backup/restore/config delivery should show queued, running, persisted, applied, or failed instead of conflating success with acceptance. Avoid blind automatic retries for non-idempotent sends |

Additional boundaries to test: the configuration UI allows up to 1,440 sample minutes while the client stores seconds in `uint16_t`; 24 hours does not fit. Clamp consistently or widen the field and multiplication path. The history ring holds 90 snapshots, not inherently 90 days; at an hourly snapshot cadence that is 3.75 days. Match retention labels and downsampling to actual capacity. Do not promise battery-life improvement from `safeSleep` alone: MCU RTOS sleep does not turn off the modem, regulator, sensors, or relay coils. Profile complete hardware power before introducing deeper sleep, and preserve watchdog, pulse capture, and the daily OTA recovery window.

## Website and Security Improvements

- Standardize one navigation/header, heading hierarchy, form spacing, table treatment, action sizing, and status vocabulary. Keep the dashboard work-focused; group settings by operational task and avoid decorative nested section cards.
- Put active alarms and communication failures ahead of passive readings. Distinguish active-but-snoozed, sensor fault, stale measurement, stale device contact, and offline server. Preserve last-good content on a failed refresh with an explicit age/error instead of replacing the whole view.
- Give invalid fields inline errors and preserve edits on failed saves. Use stable IDs for list items and a revision/ETag for shared contact/config edits so two open pages cannot silently overwrite one another.
- Expose reminder interval and delivery policy alongside snooze controls. Current reminders share SMS-oriented gates with email, so email-only behavior and settings labels need an explicit policy and end-to-end tests.
- Align viewer display quality, stale voltage rules, configured labels, and snooze indicators with the server. Viewer contacts are mutable without viewer authentication, and a viewer update replaces the entire `cat:"viewer"` subset. Confirm the single-trusted-viewer assumption; multiple or untrusted viewers need ownership, authorization, and revision checks. The root README still describes the viewer as read-only.
- Session middleware, constant-time comparisons, HttpOnly cookie, and SameSite=Strict are present. The JS session marker is the literal `cookie`, not the secret token; do not report the fetch wrapper as leaking that token cross-origin. Nonetheless, restrict the wrapper to same-origin requests and support concurrent user sessions if operationally needed.
- LAN HTTP provides no transport confidentiality for PIN/session traffic. Keep the system on a trusted management network or use a supported TLS reverse proxy. Replace ADC/timing-seeded LCG session generation with a platform cryptographic random source; a 64-bit state is not proof of 64 bits of entropy. Avoid a shared default administrative PIN and protect backups containing the plaintext PIN and reversibly obfuscated credentials.
- The FTPS Python helper defaults to all interfaces with known test credentials and full file permissions. That is acceptable only as an isolated test fixture; require explicit exposure or local binding for general use. Its TLS requirements are correctly enabled for both channels. Do not run it unchanged on an untrusted LAN.
- The root README advertises v1.9.3 and obsolete memory/capability details while Common declares v2.2.14. Update the release/version source of truth, viewer capabilities, and build recipes. Keep archived design decisions clearly marked so older OTA/loop-power guidance is not mistaken for the current deployment procedure.

## Suggested Remediation Order

1. Reconcile W-01 through W-04 without reverting unrelated local work. Add a behavior/schema regression gate before building another client release.
2. Fix F-01/F-02/F-03 first: multipart alarm clearing, stable relay identity, and calibration identity/units. Add deterministic protocol/UI tests before deployment.
3. Separate alarm state from notification delivery; fix debounce, early-boot delivery, pulse acquisition, and event-order handling. Bench-test actuators and sensor timing with non-production loads.
4. Make save/restore and config-ACK contracts explicit and failure-aware; test restart at each commit point. Add the invalid-to-valid clock scheduling transition.
5. Unify validity/units across telemetry, history, emails, and viewer; repair filter state and mobile/offline rendering.
6. Introduce cooperative backup/replay and measure peak RAM, loop latency, flash writes, modem syncs, and energy before tuning intervals. Pin the verified toolchain and automate the website fixtures.

## Verification and Limits

| Check | Result |
| --- | --- |
| Server compile, Opta FQBN | PASS: 1,013,348 bytes flash (51%); 360,720 bytes static RAM (68%) |
| Client compile, Opta with `-DTANKALARM_DFU_MCUBOOT` | PASS: 379,940 bytes flash (19%); 80,928 bytes static RAM (15%) |
| Viewer compile, Opta | PASS: 318,000 bytes flash (16%); 82,784 bytes static RAM (15%) |
| FTPS test sketch compile, Opta | PASS: 481,512 bytes flash (24%); 78,976 bytes static RAM (15%) |
| C++ literal-aware extraction | 17 server/viewer HTML/CSS resources extracted |
| Embedded inline JavaScript syntax | All 18 script blocks parse successfully |
| Source-derived deterministic checks | Nine confirmed branch/contract reproductions; these are not native MCU tests |
| Server website | 14 routes exercised at 320/390/768/1440 CSS pixels with synthetic API data |
| Calibration workflow | GAS input mismatch and malformed POST captured against the local fixture |
| History workflow | Filter reset and inches axis reproduced; CDN-blocked failure reproduced |
| Canvas check | History canvas contains 18,970 nontransparent pixels in the measured populated case; oversized width confirmed after resize |
| Python HTML extraction helper | Executed mocked-I/O test confirms leaked C++ concatenation markers; source files were not rewritten |
| Editor diagnostics on review helpers/document | No relevant diagnostics at validation time |
| Hardware, power-loss injection, radio delivery, real relay operation | Not executed; required before deploying corresponding fixes |
| Secure slot/bootloader lifecycle | Build recipe and source reviewed; a new signed slot was not flashed or trial-booted |
| Screenshot approval | Unavailable: browser compositor captures were blank; excluded from the review commit |

Builds used the installed local toolchain/libraries and the existing dirty working copy, not a clean-room reconstruction of the released binaries. The server was also compiled successfully through the existing VS Code task. Transient terminal input/PATH issues were worked around; only completed builds with recorded zero exit results are listed as passing.

Review-local extraction, fixture, logic probes, and build logs are retained under the ignored build directory, not shipped as production tests. Reproductions and expected regression checks are described above so they can be promoted into maintainable CI tests. In particular, the logic probe mechanically translates the selected alarm function and separately models several branch sequences; it does not simulate Mbed, Notecard, flash timing, or hardware interrupts.

Excluded false positives: corrected synthetic summary fields eliminated the initial apparent unconfigured-client classification issue; it is not a finding. The present code already has atomic file replacement, fixed boot registry hash insertion, future-schema gating, and corrected semantic-version comparisons. Public Arduino signing keys are an explicitly documented authenticity trade-off, not an undisclosed credential incident. No claim is made that every page is visually approved merely because its JavaScript parses.