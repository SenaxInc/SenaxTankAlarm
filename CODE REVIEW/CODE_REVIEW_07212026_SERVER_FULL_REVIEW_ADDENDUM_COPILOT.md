# Addendum to Server Full Review v2.2.12

**Date:** 2026-07-21  
**Revision assessed:** `2c90c6d4315fed74a50accb73da4892693fe5504` (`v2.2.12`)  
**Review assessed:** `CODE_REVIEW_07212026_SERVER_FULL_REVIEW.md`  
**Independent review:** `CODE_REVIEW_07212026_SERVER_INDEPENDENT_COPILOT.md`

This is a companion document. The existing review was read but not edited.

---

## Overall assessment

The previous review is substantially better than a typical broad static review of this sketch. It identifies real state-ordering problems, explicitly records rejected claims, distinguishes verified behavior from speculation, and correctly recognizes several sound paths that are easy to misread in an 18,000-line sketch.

Its central conclusion is directionally right: the largest risks are sequencing, persistence, and operational observability rather than conventional memory-safety defects.

It should not be used unchanged as the implementation checklist, however. Several findings need narrower consequences or stronger root causes, two statements are materially wrong, and the independent pass found additional higher-value defects.

---

## Finding-by-finding assessment

### S-1 - Confirmed and should be broadened

The viewer-network `400` path does occur after earlier `gConfig` mutations. Hoisting that validation fixes the immediate v2.2.12 regression.

The deeper issue is that the whole handler mutates global state before persistence. A `saveConfig` failure also leaves RAM on the new values while disk remains on the old values. `archiveHistory` can be persisted separately before the main config save. The durable fix is a candidate-config transaction, not only moving the five viewer checks.

**Verdict:** confirmed; recommended fix is incomplete.

### S-2 - Confirmed direction, but incomplete and partly overstated

The restore paths do omit `loadSensorRegistry`, `loadClientMetadataCache`, and `loadHistorySettings`. Manual restore can also keep serving the old contacts because `gContactsCacheValid` takes precedence over disk.

Corrections and additions:

- `ensureConfigLoaded()` really does call `loadConfig(gConfig)` each time, so the restored server config is reloaded into RAM.
- `loadHotTierSnapshot()` should not be in this fix list because `/history/hot_tier.json` is not in `kBackupFiles` and is not restored.
- Immediate overwrite by the five-minute save is conditional on a dirty flag. It is very plausible during manual restore and after later telemetry, but it is not guaranteed on every boot restore.
- Restore's larger defect is the 2,048-byte receive buffer versus the 24,576-byte backup limit.
- Restore reports success when any one file succeeds and does not require its `required` files.
- Restored Product UID/fleet/network values are loaded after Notecard/Ethernet initialization and are not applied until another initialization or reboot.
- Backup does not flush dirty registry or metadata before uploading them.

**Verdict:** confirmed core defect; superseded by IR-2 in the independent review.

### S-3 - Confirmed, with narrower duplicate-SMS mechanics

The post-handler delete path ignores allocation failure, null response, and error responses. Reprocessing is real.

The consequence needs nuance. A duplicate ordinary high/low alarm is usually suppressed immediately by `checkSmsRateLimit`, because the limiter is updated before the send. Clear/recovery SMS bypass that limiter, and unload notifications/log entries have no equivalent dedupe, so those can repeat immediately and repeatedly. A persistent delete failure can also allow an ordinary alarm through again after the minimum interval.

The review also missed that a failed delete does not stop the current drain. The same head note can run up to ten times in one poll.

**Verdict:** confirmed; impact mechanism narrowed and strengthened for unload/clear.

### S-4 - Confirmed window, but the persisted hourly budget claim is wrong

The five-minute window can lose `lastSmsAlertEpoch`, alarm state, and reminder anchors. Eager persistence after a successfully queued alert is reasonable.

The registry does not persist `smsAlertTimestamps[]`. It stores only the last epoch and the count. After reboot the timestamp array is zero, and the next limiter cleanup reduces the restored count to zero. The hourly budget therefore does not survive reboot even after a successful registry save.

There is also no hash gate in `saveSensorRegistry`, and the writer cannot return success. Before adding eager saves, make the writer return `bool`, preserve dirty state on failure, and add the hash gate.

Finally, the limiter is committed before `sendSmsAlert`, whose return value is ignored. A failed queue attempt can be recorded as if an SMS was sent.

**Verdict:** confirmed basic window; persistence description and implementation order need correction.

### S-5 - Confirmed, with one already-implemented mitigation

Header and body phases can block the single loop for roughly ten seconds in aggregate. The request reader also returns success without proving that headers or the declared body completed.

`body.reserve(contentLength)` is already present in the reviewed source, so it is not an outstanding fix. The useful changes are completion tracking, a per-byte idle timeout, and consistent maximum-body handling.

The review underweights the much larger synchronous FTPS stall: the configured baseline inter-file waits alone total at least 8 minutes 40 seconds.

**Verdict:** confirmed; one recommendation already exists; FTPS is the higher-priority loop stall.

### S-6 - Useful product question, not a confirmed correctness bug

The implementation comment and server README define pause as pausing Notecard processing. Under that definition, continuing history maintenance, FTP, heartbeat, and outbound schedules is expected.

The UI says "Paused for maintenance," which implies broader behavior. Stale-client alerts and alarm reminders while inbound data is intentionally frozen may surprise an operator. This needs a named semantic decision, but gating history and FTP by default would change the documented behavior and may make maintenance less safe rather than more safe.

**Verdict:** retain as UX/semantics clarification; do not classify all listed activity as a bug.

### S-7 - Confirmed

`configPin` is plaintext in `server_config.json`, and that file is included in FTP backup. The risk is bounded by the local filesystem/FTP/LAN trust model, but the observation is correct.

Hashing needs a migration plan because the PIN currently participates in the reversible FTP credential key. A device-salted PIN hash can also be used as key material without retaining the plaintext PIN on disk.

**Verdict:** confirmed low-severity hardening item.

### S-8 - Mixed: session observations are valid; token-leak claim is false

Confirmed:

- one login invalidates the prior browser,
- there is no session expiry,
- token generation is locally seeded rather than backed by an explicit CSPRNG.

Correction: the cross-origin fetch wrapper does not send the real token. The real 16-hex token exists only in the `HttpOnly; SameSite=Strict` cookie. Login JSON returns the literal string `"cookie"`, which is what JavaScript stores and injects as `X-Session`. The unconditional wrapper can break an external request by triggering CORS preflight, but it does not leak the secret session value.

Scoping the wrapper to same-origin remains the correct cleanup for functionality and least privilege.

**Verdict:** partially confirmed; no token-exfiltration vulnerability from this wrapper.

### S-9 - Confirmed as a deliberate observability gap

There are no common `diag.qi`/`health.qi` definitions, Route #1 does not carry those files, and the server does not process them. Notehub-only visibility is consistent with the current route guide.

This should remain an improvement unless dashboard health ingestion is made a product requirement.

**Verdict:** confirmed by-design limitation.

### S-10 - Confirmed

The ten-note cap has no backlog/cap-hit metric. Add per-file counters and expose them with delete failures.

**Verdict:** confirmed low-severity observability item.

### S-11 - Technically correct but lower value than the timestamp collision

An eight-hex hash collision is theoretically possible. The more practical ordering bug is that two different configs dispatched in the same second have different hashes but can truncate to the same client-side `_ts`; the newer hash can then be ACKed while its payload is skipped.

**Verdict:** retain as informational; prioritize a monotonic revision over widening only the hash.

### S-12 - Confirmed

The diagnostic alarm skip list prevents daily high/low reconciliation from clearing `sensor-fault` and `sensor-stuck` latches.

**Verdict:** verified sound.

### S-13 - Finding is correct; stated consequence is wrong

`handleUnload` lacks a `k >= 1` guard. Contrary to the review text, it does call `upsertSensorRecord(clientUid, sensorIndex)` after logging/sending notifications. A missing `k` can therefore create sensor zero and persist it, not merely create an unload-log entry.

The sensor branch of `handleAlarm` has the same missing-index issue after system alarms are filtered out.

**Verdict:** confirmed and more consequential than reported.

### S-14 - Correct retry observation, but misses same-second ordering

Retry timestamps are refreshed, so a lost ACK causes an idempotent reapply in ordinary cases. The client stores only whole seconds. Two distinct dispatches within one second can collide and cause the newer payload to be bypassed while its hash is acknowledged as applied.

**Verdict:** ordinary retry description confirmed; ordering analysis incomplete.

### S-15 - Mostly valid low-priority cleanup

`handleDebugSensors` still assembles JSON with `String` concatenation. `respondJson` double-walk and contacts-cache parse cost are optimization items, not current correctness defects.

The HTTP body already reserves `Content-Length`, so no action is needed there.

**Verdict:** retain as low-priority performance cleanup.

### S-16 - Confirmed current contract

Client snapshot overflow aborts before send. The 4,096-byte persistent snapshot limit remains a capacity constraint to monitor rather than a current orphaning bug.

**Verdict:** verified sound with a documented ceiling.

### S-17 - Confirmed improvement

The client config path has no future-schema check, while relay commands do. Current fields default safely, so this is compatibility hardening rather than a current high-severity defect.

Config ACK and OTA-report bodies also omit `_sv` because they bypass the common `publishNote` path.

**Verdict:** confirmed low-severity protocol hardening.

---

## Assessment of the verified-correct and rejected-claim sections

### Correct and valuable

- `posix_write_file` is an atomic-write shim.
- Config payloads are cached before send.
- Viewer network revisions are monotonic and mode is applied.
- `tankalarm_computeNextAlignedEpoch` safely returns zero for an invalid epoch.
- The stale-client scan size is far below the watchdog limit.
- Contact object fields are held in ArduinoJson rather than fixed per-field buffers.
- Daily reconciliation preserves diagnostic alarm latches.

### Important qualifications

1. **Atomic file writes do not imply durable state transitions.** Registry/metadata callers cannot observe write failure, clear dirty flags anyway, and cannot persist an empty array.
2. **The scheduler epoch guard is safe but incomplete.** A cold `no-time` boot does not calculate a bogus epoch, but later time synchronization never re-arms daily email or viewer summary.
3. **Contacts can fit in RAM yet still fail FTP restore.** The restore baseline buffer is only 2,048 bytes.
4. **The fetch wrapper does not contain the secret token.** Its cross-origin problem is unnecessary preflight/breakage, not credential exfiltration.

---

## Material findings missing from the previous review

The independent pass found the following issues that should be added to any repair plan:

1. Registry/metadata files cannot be written empty; deleting the last client can be undone by reboot.
2. Save failures are marked clean because the two writers return `void`.
3. Client metadata persistence can be indefinitely starved by the registry save ordering.
4. FTP restore has a 2 KB baseline receive limit and partial-success semantics.
5. Restored config side effects are not applied to already-initialized Ethernet/Notecard subsystems.
6. A matching client `failed` config ACK clears pending delivery.
7. Late time synchronization does not initialize daily/viewer schedules and retries `card.time` every loop.
8. FTPS backup blocks the main loop for at least 8 minutes 40 seconds of configured inter-file waits.
9. Poison-note tracking has 12 slots for 13 inbound files.
10. Same-second config dispatches can collide at the client's whole-second ordering boundary.
11. SMS hourly timestamps and system-SMS limiter epochs are not persisted.
12. SMS limiter state is consumed before queue success is known.

---

## Final thought

The previous review is a strong diagnostic document and is worth keeping. Its best contribution is the explicit separation of confirmed claims from rejected ones. The next iteration should apply that same standard to persistence return values and cross-device state machines, where a locally correct function can still produce an incorrect end-to-end transition.

The repair plan should start with IR-1 through IR-5 from the independent review. Those defects can cause operator-visible state to be reported as saved, restored, applied, or scheduled when the corresponding durable transition did not actually happen.
