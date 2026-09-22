# Independent Server Code Review - v2.2.12

**Date:** 2026-07-21  
**Revision reviewed:** `2c90c6d4315fed74a50accb73da4892693fe5504` (`v2.2.12`)  
**Primary scope:** `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino`  
**Cross-check scope:** client firmware, viewer firmware, the common notefile definitions, and the Notehub route guide where they define a server-facing contract.  
**Method:** independent static review by subsystem, followed by direct source verification of every retained finding. No firmware was changed or flashed during this review.

The existing `CODE_REVIEW_07212026_SERVER_FULL_REVIEW.md` was treated as a set of claims to verify, not as source truth. A separate companion addendum records the detailed assessment of that review without modifying it.

---

## 1. Executive summary

The server has several good foundations: filesystem writes are atomic at the file level, inbound notes are normally processed peek-then-delete, config payloads are cached before dispatch, authentication comparisons are constant-time, and the viewer network revision is monotonic.

The largest remaining risks are not buffer overflows or basic parsing errors. They are state-transition problems:

1. A successful delete can be reported to the operator but not survive reboot when the last registry or metadata entry is removed.
2. FTP backup/restore cannot currently provide a complete, internally consistent recovery set.
3. A client ACK saying config persistence failed nevertheless clears the server's pending retry flag.
4. Metadata persistence can be starved indefinitely by normal sensor traffic.
5. A cold boot without Notecard time leaves daily email and viewer schedules permanently unarmed for that boot.
6. Synchronous FTPS backup can suspend alarm processing for at least 8 minutes 40 seconds before transfer and retry time.

| ID | Severity | Finding |
|---|---|---|
| IR-1 | **High** | Registry/metadata writers cannot persist an empty state, and callers clear dirty flags even after failed writes |
| IR-2 | **High** | FTP recovery is size-limited, partial-success based, stale in RAM, and does not reapply restored subsystem configuration |
| IR-3 | **Medium** | A matching `failed` config ACK clears `pendingDispatch` |
| IR-4 | **Medium** | The shared registry-save timestamp can starve client metadata persistence |
| IR-5 | **Medium** | Late time synchronization never re-arms daily/viewer schedules and is retried every loop |
| IR-6 | **Medium** | Synchronous FTPS backup blocks the main control loop for many minutes |
| IR-7 | **Medium** | Poison-note tracking has 12 slots for 13 files; delete failures can replay side effects repeatedly |
| IR-8 | **Medium** | Whole-second client config ordering can acknowledge but skip a newer same-second config |
| IR-9 | **Medium** | SMS limiter state is incomplete on disk and is committed before an alert is queued |
| IR-10 | **Medium** | `/api/server-settings` is not transactional across validation, persistence, and side effects |
| IR-11 | Low/Medium | HTTP timeouts block the loop and incomplete requests are treated as complete |
| IR-12 | Low | Sensor index and UID validation is inconsistent at inbound protocol boundaries |
| IR-13 | Low | PIN/session hardening opportunities remain; the alleged cross-origin token leak is not real |
| IR-14 | Low | Email-format POST reports `success:true` when persistence fails |
| IR-15 | Improvement | Diagnostics routing, schema handling, pause wording, and observability can be tightened |

No finding below depends on a speculative race between threads. The sketch is single-threaded; the failures arise from operation ordering, incomplete persistence contracts, or asynchronous delivery between devices.

---

## 2. Findings

### IR-1 - High: empty registry/metadata states are not durable, and failed saves are marked clean

**Source:** server `saveSensorRegistry`, `saveClientMetadataCache`, the periodic save block in `loop`, and `handleClientDeleteRequest`.

Both writers are `void` and return immediately when their count is zero:

```cpp
if (!mbedFS || gSensorRecordCount == 0) {
  return;
}
```

`saveClientMetadataCache` has the same early return for `gClientMetadataCount == 0`. Callers then clear the corresponding dirty flag without knowing whether a file was written:

```cpp
saveSensorRegistry();
saveClientMetadataCache();
gSensorRegistryDirty = false;
gClientMetadataDirty = false;
```

**Failure sequence:**

1. The operator removes the only client through `DELETE /api/client`.
2. RAM counts become zero and both dirty flags are set.
3. The immediate save calls return without writing `[]` or removing the old files.
4. The handler clears both dirty flags and returns `200 Client removed`.
5. On reboot, the old files are loaded and the deleted client reappears.

The same API shape loses retry state after allocation, filesystem, or atomic-write failure. The periodic loop, stale-client path, DFU pre-save path, manual deletion path, and debug dedupe path all clear flags after calling a writer that cannot report success.

**Recommended fix:**

- Change both writers to return `bool`.
- Serialize and atomically write `[]` when the count is zero.
- Clear dirty flags only after `true` is returned.
- Keep failure counters/logging, but leave the object dirty so the next loop retries.
- Add a hash gate to the registry and metadata writers after correctness is fixed.

**Focused test:** create one client, reboot to prove it loads, delete it, reboot again, and verify `/api/clients` and `/api/sensors` remain empty. Repeat with an injected write failure and verify the dirty flag remains set.

---

### IR-2 - High: FTP backup/restore is not a complete recovery transaction

This is a group of directly related defects in the recovery contract.

#### A. Restore accepts only 2 KB per baseline file

Backup allocates `FTP_MAX_FILE_BYTES + 1` where `FTP_MAX_FILE_BYTES` is 24,576 bytes. Restore instead uses:

```cpp
char contents[2048];
```

That makes restore incapable of retrieving any baseline file larger than 2,047 bytes. Normal contacts, sensor registry, client metadata, calibration, and client-cache files can all exceed that size. Their normal local loaders accept 16 KB or 32 KB.

#### B. Any restored file makes the operation successful

`performFtpRestoreDetailed` sets:

```cpp
result.success = (result.filesProcessed > 0);
```

The `required` flag in `kBackupFiles` is not enforced by restore. One small optional file can therefore produce HTTP `200` and `ok:true` while `server_config.json`, `sensor_registry.json`, or most other files failed. Backup uses the same broad partial-success rule after recording required-file failures.

#### C. The recovered files are not fully adopted into RAM

After either boot-time or manual restore, the code reloads server config, client config snapshots, and calibration. It does not reload:

- sensor registry,
- client metadata,
- history settings.

For a manual restore, `gContactsCacheValid` can also cause every contact lookup to continue using the pre-restore cached JSON instead of the restored file.

The earlier review suggested reloading the hot-tier snapshot. That file is not in `kBackupFiles`, so it is not currently part of FTP backup/restore and reloading it would not repair this recovery path.

#### D. Restored configuration side effects are not applied

`setup` initializes the Notecard and Ethernet before restore-on-boot. Loading a restored `productUid`, fleet, DHCP/static profile, or FTP profile into `gConfig` does not reinitialize those subsystems. Manual restore has the same issue. A replacement server can therefore need an undocumented second reboot before the restored network and Notehub identity are actually active.

#### E. Backup does not flush dirty registry or metadata

`prepareLocalBackupFilesForFtp` flushes dirty server config and history settings, but not dirty sensor registry or client metadata. A backup can upload stale on-disk alarm, rate-limit, firmware, location, or OTA state even when newer state is present in RAM.

**Recommended fix:**

1. Retrieve to a static/global buffer sized to `FTP_MAX_FILE_BYTES + 1`, or stream each remote file to a temporary local file with a size limit.
2. Validate each JSON/text file before replacing its live file.
3. Stage the full set, require all required files, and expose partial restore as `ok:false` with per-file status.
4. Flush dirty registry and metadata before backup and abort if either flush fails.
5. On successful restore, invalidate/reload every RAM cache represented in the manifest.
6. Mark a safe reboot as required, or explicitly reinitialize Notecard/Ethernet after sending the HTTP response.
7. Add a manifest version plus per-file length/hash so mixed or corrupt generations are rejected.

**Focused test:** populate contacts and registry files above 2 KB, back up, alter local state, restore, and verify byte hashes plus every corresponding API response after one boot only.

---

### IR-3 - Medium: a failed client persistence ACK clears the pending config

The client sends `st:"failed"` with the matching config hash when `saveConfigToFlash` fails. In `handleConfigAck`, the server clears pending state solely on hash equality:

```cpp
if (version[0] != '\0' && strcmp(version, snap->configVersion) == 0) {
  snap->pendingDispatch = false;
  snap->dispatchAttempts = 0;
}
```

The ACK status is not considered.

**Failure sequence:**

1. Client receives and applies a config in RAM.
2. Client flash persistence fails and it sends a matching `failed` ACK.
3. Server records `lastAckStatus="failed"` but clears `pendingDispatch`.
4. Automatic retry stops.
5. Client reboots into the old config while the server has no pending delivery.

**Recommended fix:** clear pending only for an explicitly successful terminal status, currently `applied`. Keep `failed` pending, preserve a visible failure reason, and use a bounded retry/manual-action state so persistent flash faults do not retry forever.

**Focused test:** force `saveConfigToFlash` to return false, inject the resulting ACK, and verify the server still reports `pd:true` and retries or requests operator action.

---

### IR-4 - Medium: one timestamp can starve metadata persistence

The periodic blocks share `gLastRegistrySaveMillis`:

```cpp
if (gSensorRegistryDirty && now - gLastRegistrySaveMillis > interval) {
  gLastRegistrySaveMillis = now;
  saveSensorRegistry();
  gSensorRegistryDirty = false;
}
if (gClientMetadataDirty && now - gLastRegistrySaveMillis > interval) {
  saveClientMetadataCache();
  gClientMetadataDirty = false;
}
```

When both are dirty, the registry block advances the timestamp before the metadata condition. Metadata waits another five minutes. If any sensor telemetry arrives often enough to dirty the registry before each interval, this repeats indefinitely.

This is realistic fleet behavior: every telemetry update dirties the sensor registry, while firmware version, VIN, OTA state, signal, and system-alarm changes dirty client metadata.

**Impact:** metadata can remain RAM-only until a quiet five-minute interval, a stale-client scan that happens to force a save, or a clean shutdown/DFU path. A power loss can then revert firmware/OTA/location/signal state much further than the documented five-minute window.

**Recommended fix:** use independent `gLastSensorRegistrySaveMillis` and `gLastClientMetadataSaveMillis` values, and update each only after its own successful write.

---

### IR-5 - Medium: late time sync leaves schedules disabled and retries on every loop

At setup, `ensureTimeSync` is followed once by `scheduleNextDailyEmail` and, when enabled, `scheduleNextViewerSummary`. A normal Notecard `no-time` response leaves `currentEpoch()` at zero, so both schedules are set to zero.

The loop later calls `ensureTimeSync()` continuously. When time finally becomes valid, no code detects the zero-to-valid transition and re-runs either scheduler. Both features remain disabled until a settings change, restore, or reboot happens to schedule them again.

There is a second problem in the same path: while time is unavailable, `ensureTimeSync` issues `card.time` on every loop iteration with no retry interval or backoff. A disconnected or newly provisioned Notecard can therefore dominate the I2C bus and slow web/telemetry work.

**Recommended fix:** make time sync return a transition result, rate-limit failed attempts, and run all epoch-dependent scheduler initialization when time changes from unavailable to available:

```cpp
bool becameValid = ensureTimeSync();
if (becameValid || gNextDailyEmailEpoch <= 0.0) scheduleNextDailyEmail();
if (gConfig.viewerEnabled && (becameValid || gNextViewerSummaryEpoch <= 0.0)) {
  scheduleNextViewerSummary();
}
```

Use a bounded backoff such as 30 seconds, then several minutes after repeated `no-time` responses.

---

### IR-6 - Medium: synchronous FTPS backup suspends the control loop for many minutes

`FTP_BACKUP_INTER_FILE_DELAY_MS` is 65,000 ms. The baseline list has nine files, and the backup waits before every file after the first. Even before transfer and retry time, that is:

$$8 \times 65\text{ s} = 520\text{ s} = 8\text{ min }40\text{ s}$$

Both manual and pending auto-backup call `performFtpBackupDetailed` synchronously. The wait loop services the watchdog, but it does not run `pollNotecard`, `Ethernet.maintain`, stale/reminder checks, daily/viewer schedules, or normal web handling.

**Impact:** alarm and unload notes remain queued and alert delivery is delayed for many minutes. The device stays alive, which makes the delay look like healthy operation rather than a watchdog reset.

**Recommended fix:** convert backup to a loop-driven state machine. Transfer one file per eligible pass, store the next-attempt time for TIME_WAIT/retry pacing, and return to normal loop work between states. At minimum, service inbound alarm processing during the 65-second waits if FTPS socket constraints permit it.

---

### IR-7 - Medium: poison-note coverage is undersized and delete failure replays handlers

`pollNotecard` processes 13 distinct inbound files. `notefileParseFailureCounter` allocates 12 tracker slots. Once 12 distinct files have consumed trackers, a malformed note in the remaining file receives no counter and can never reach the three-strike poison deletion. That file remains blocked behind its first bad note.

For successfully parsed notes, the post-handler delete path ignores allocation failure, null response, and an error-bearing response. Processing then continues, so the same undeleted head note can run up to ten times in one poll and again every five seconds.

Immediate high/low alarm duplicates are partly suppressed by the per-sensor limiter. The dangerous replay cases are still real:

- clear/recovery alerts bypass that limiter,
- unload events have no equivalent dedupe,
- unload log entries are appended again,
- handler state can be recomputed repeatedly.

**Recommended fix:**

- Define the inbound files in one table and derive tracker count from that table.
- Check delete response errors and stop draining that file after a failed delete.
- Expose delete-failure and backlog counters in `/api/notecard/status`.
- Add idempotency keys for alert/unload/config side effects, preferably a note UID; otherwise use a persisted bounded tuple such as file, client, event epoch, and event type.

---

### IR-8 - Medium: same-second configs can be skipped but acknowledged as applied

The server stores `lastDispatchEpoch` as a fractional `double` and sends it as `_ts`. The client reads `_ts` into a `uint32_t` and rejects any value less than or equal to its persisted `configEpoch`.

**Failure sequence:**

1. Config A is dispatched and routed to the client.
2. Config B is saved for the same client within the same wall-clock second.
3. The server hashes A and B differently, but both fractional timestamps truncate to the same client-side second.
4. Client applies A and stores that second.
5. Client receives B, treats `_ts <= configEpoch` as obsolete, and sends a successful ACK carrying B's hash.
6. Server sees the matching B hash and clears pending state even though B was never applied.

Purging the server outbox reduces the window but cannot recall A after Route delivery.

**Recommended fix:** use a monotonic integer config revision independent of wall-clock resolution. Persist both active revision and active config hash on the client. A same-revision/different-hash input should be treated as a conflict, not as successfully applied.

---

### IR-9 - Medium: SMS accounting is neither complete on disk nor tied to queue success

Three related issues weaken alert guarantees:

1. `checkSmsRateLimit` updates `lastSmsAlertEpoch` and appends the hourly timestamp before `sendSmsAlert` is called.
2. `handleAlarm` ignores the queued-recipient return value from `sendSmsAlert`.
3. Registry persistence stores only `se` (last epoch) and `sa` (count), not `smsAlertTimestamps[]`. After reboot the array is zeroed, so the next cleanup reduces the restored count to zero. The two-per-hour budget does not actually survive reboot.

`ClientMetadata.lastSystemSmsEpoch` is also not serialized, so the per-client system-alarm limiter resets on reboot.

**Failure sequence:** the Notecard is unavailable or all contacts are filtered out, an alarm arrives, the limiter records a send, no SMS note is queued, and the source note is deleted as processed. A repeated alarm is then suppressed for five minutes, and the reminder engine can treat the nonzero epoch as proof that an original SMS was sent.

**Recommended fix:** separate `canSendAlert` from `recordQueuedAlert`. Commit limiter state only when at least one SMS note was queued, persist the timestamp ring and system SMS epoch, and eagerly save the small alert-state change after successful queueing. If no recipient was queued, retain a bounded pending-alert record rather than spinning every reminder sweep.

---

### IR-10 - Medium: server settings are not transactional

The previously reported viewer-network validation defect is confirmed: product, fleet, SMS, and viewer fields mutate `gConfig` before malformed viewer-network fields return `400`.

The same underlying problem also exists on persistence failure. Nearly every field is changed in global RAM before `saveConfig(gConfig)`. If the atomic file write fails, the old disk file remains valid but runtime behavior uses the new values despite an HTTP `500`. `archiveHistory` is written to a separate history file during the mutation phase, before the main config commit.

Atomic file replacement prevents a torn JSON file; it does not make the in-memory update transactional.

**Recommended fix:** copy `gConfig` and `gHistorySettings` into candidate values, parse and validate every supplied field into those candidates, persist all required files, then swap globals and apply side effects. No `4xx` or `5xx` path should leave runtime state changed.

---

### IR-11 - Low/Medium: HTTP read completion and loop latency

`readHttpRequest` allows up to five seconds for headers and another five seconds for a body. During that time the single-threaded loop does not poll inbound alarms. It also returns `true` when either phase times out without proving that the header terminator or full declared body was received. Handlers then see truncated JSON and usually return `400`.

There is also a boundary mismatch: a declared body larger than 16,384 bytes is rejected up front, but a body exactly 16,384 bytes sets `bodyTooLarge` after the final byte because the body loop checks `readBytes >= MAX_HTTP_BODY_BYTES`.

`body.reserve(contentLength)` is already present; it should not be listed as a missing optimization.

**Recommended fix:** track `headersComplete` and require `readBytes == contentLength`; use a short idle timeout reset by each received byte plus a bounded total timeout; make the maximum-size comparison consistent.

---

### IR-12 - Low: inconsistent inbound identity/index validation

Telemetry correctly rejects missing or zero `k`. The sensor branch of `handleAlarm` and `handleUnload` do not. `upsertSensorRecord` rejects indexes greater than or equal to `MAX_SENSOR_RECORDS` but accepts zero.

An unload note without `k` therefore logs an event and upserts sensor zero. A non-system alarm without `k` can also create/latch sensor zero. This directly contradicts the previous review's statement that unload does not upsert a sensor record.

`handleRelayForward` validates only a non-empty target, and `handleLocationResponse` creates metadata from a merely non-empty UID. Both should use the existing `isValidClientUid` boundary check.

**Recommended fix:** reject `sensorIndex == 0` centrally in `upsertSensorRecord`, with explicit guards in handlers that have valid sensorless message types. Apply `isValidClientUid` before logs, metadata creation, or outbound forwarding.

---

### IR-13 - Low: PIN/session hardening, with an important correction

Confirmed risks:

- `configPin` is stored in plaintext in `server_config.json`, which is also included in FTP backup.
- Login sends the four-digit PIN over plain HTTP, so the LAN is part of the security boundary.
- One global session has no inactivity/absolute expiry; a new login invalidates the prior browser.
- Session entropy is generated locally from timing and ADC samples rather than a hardware CSPRNG.

Important correction: the JavaScript fetch wrapper does **not** expose the real session token. The server sets the real token only in an `HttpOnly; SameSite=Strict` cookie. Login JSON returns `"session":"cookie"`, and JavaScript stores and forwards that literal marker as `X-Session`. A cross-origin fetch can cause a needless CORS preflight, but its header does not contain the secret token.

**Recommended improvements:** hash the PIN at rest with a device-specific salt, document or isolate the trusted management LAN, add session expiry and a small multi-session table if concurrent administration matters, and scope the fetch wrapper to same-origin requests to avoid external preflights.

---

### IR-14 - Low: email-format POST reports success when persistence fails

`handleEmailFormatPost` records the actual `saved` result but always emits:

```cpp
response["success"] = true;
response["saved"] = saved;
```

Clients that check the conventional `success` field can report success even though the change was not written. Return `success=saved` and an HTTP error status on persistence failure, or deliberately use a distinct `appliedInMemory` field if RAM-only behavior is supported.

---

### IR-15 - Improvements and explicit design decisions

These are not current release-blocking defects, but they should be resolved before the next protocol/schema expansion:

- `diag.qo` and `health.qo` are intentionally not routed to or consumed by the server. If dashboard-visible client health is desired, define matching inbox files, update Route #1, and add bounded handlers. Otherwise document Notehub-only visibility.
- Client config ingestion does not reject or warn on a future `_sv`, while relay ingestion does. Add a future-schema guard before `applyConfigUpdate`.
- Config ACK and OTA-report bodies sent directly through CJSON do not stamp `_sv`.
- Viewer summary ingestion warns on any schema mismatch but still applies the document. Define whether future versions should be rejected, partially applied, or accepted by a compatibility matrix.
- The server's pause endpoint is documented as "Pause/resume Notecard processing." Continuing history maintenance, FTP, and outbound schedules is therefore not itself a correctness bug. The UI phrase "Paused for maintenance" is broader than the implementation. Rename it to "Pause inbound processing," or explicitly define which alerts should also pause. At minimum, decide whether stale-client alerts should run while inbound processing is intentionally frozen.
- Log when a per-file Notecard drain reaches `MAX_NOTES_PER_FILE_PER_POLL`, and surface backlog/delete failures in status JSON.

---

## 3. Verified sound areas

- `posix_write_file` delegates to `serverWriteFileAtomic`; file replacement itself is temp-write plus rename.
- The config payload is cached before Notecard dispatch, and cache overflow aborts the send.
- Config ACK matching uses the current payload hash, preventing an ordinary late ACK for a different payload from clearing pending state.
- Viewer network configuration uses a monotonic revision guard and persists the applied profile.
- The real web session token is held in an `HttpOnly; SameSite=Strict` cookie and compared in constant time.
- PIN comparison is constant-time and failed logins have exponential backoff/lockout.
- Request header length/count and HTTP body size are bounded.
- Large JSON response builders generally check `doc.overflowed()`.
- Future-schema server inbound notes are centrally detected; the delete-vs-retain policy is explicit.
- FTP transfer waits service the watchdog. The remaining defect is control-loop latency, not watchdog starvation.
- Daily diagnostic alarm reconciliation excludes `sensor-fault` and `sensor-stuck` as intended.
- Viewer `net.m` is applied, and out-of-order network revisions are ignored.

---

## 4. Prioritized repair order

| Priority | Work item | Findings |
|---|---|---|
| P0 | Make registry/metadata writes return success, persist `[]`, keep dirty state on failure, and split save clocks | IR-1, IR-4 |
| P0 | Redesign FTP restore sizing/result semantics and reload/reapply every restored subsystem | IR-2 |
| P0 | Make config ACK completion status-aware | IR-3 |
| P1 | Re-arm epoch schedules after late sync and add time-sync backoff | IR-5 |
| P1 | Convert FTPS backup into a non-blocking state machine | IR-6 |
| P1 | Size poison trackers from the inbound table and make note deletion observable/idempotent | IR-7 |
| P1 | Replace second-resolution config ordering with a monotonic revision | IR-8 |
| P1 | Commit/persist SMS limiter state only after queue success | IR-9 |
| P1 | Make settings validation/persistence/apply transactional | IR-10 |
| P2 | Tighten HTTP completion, sensor/UID guards, and email-format result semantics | IR-11, IR-12, IR-14 |
| P3 | Apply security and protocol observability improvements | IR-13, IR-15 |

---

## 5. Focused regression plan

1. **Persistence empty/failure:** remove the last client, reboot, and inject a failed write.
2. **FTP round-trip:** restore files at 1 KB, 3 KB, 16 KB, and 24 KB; verify hashes, APIs, contacts, Notecard identity, and network mode after one boot.
3. **Config failure ACK:** force client flash-save failure and verify pending state remains actionable.
4. **Config ordering:** dispatch two different payloads in the same second and deliver both in order and reverse order.
5. **Cold no-time boot:** return `no-time` for several minutes, then provide time; verify retry cadence plus daily/viewer schedules.
6. **FTPS responsiveness:** run a full nine-file backup while injecting an alarm note; measure alarm-to-SMS-queue latency.
7. **Poison/delete:** exercise malformed notes in all 13 files and force an error on the post-handler delete.
8. **SMS delivery:** fail both `note.add` attempts, reboot inside an hour, and verify limiter/reminder state reflects queued messages rather than attempts.
9. **Settings transaction:** submit invalid viewer networking and inject config/history write failures; verify RAM and disk remain on the old complete configuration.
10. **HTTP slow/incomplete:** trickle headers/body and send exactly 16,384 bytes; verify bounded latency and deterministic 400/408/413 behavior.

---

## 6. Review limitations

This was a static source review. It did not exercise the physical Opta, Notecard, FTP/FTPS server, Notehub routes, or browser. The focused tests above are designed to falsify the sequencing claims with minimal instrumentation. Line numbers refer to `2c90c6d` and will drift after edits; use the named functions as anchors.
