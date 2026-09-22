# CODE REVIEW — Server Full Review (v2.2.12)

**Date:** 2026-07-21
**Reviewed at:** HEAD `2c90c6d` (v2.2.12), primary file `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino` (~18,600 lines), with cross-inspection of the client and viewer code that interacts with the server.
**Method:** four subsystem passes (main loop & Notecard pipeline; HTTP/session layer; persistence & alerting; wire protocol across server/client/viewer), followed by **manual verification of every non-trivial claim against the source**. Findings below are marked **CONFIRMED** only when I re-read the code myself. Claims from the passes that turned out to be wrong are preserved in Appendix B so future reviewers don't rediscover them.

Companion docs: `CODE_REVIEW_07202026_ISSUE_313_SERVER_WEBPAGE_BUGS.md` (webpage layer — not re-reviewed here), `CODE_REVIEW_07062026_SMS_PIPELINE_END_TO_END.md`.

---

## 0. Executive summary

The server core is in good shape: persistence is atomic everywhere (a `posix_write_file` shim wraps the atomic writer), auth uses constant-time compares with exponential lockout, the Notecard pipeline has poison-note recovery and a future-schema gate, and long FTP transfers service the watchdog. The issues found are mostly **sequencing windows** (state mutated before validation, state alerted before persistence, restore not adopted into RAM) and **operational blind spots** (notes that can be silently re-processed or never seen). One defect was introduced by v2.2.12 itself (S-1).

| # | Finding | Severity | Introduced |
|---|---|---|---|
| S-1 | `/api/server-settings`: partial-apply-then-400 (validation mid-mutation) | **Medium** | v2.2.12 |
| S-2 | FTP restore-on-boot never reloads sensor registry / client metadata; periodic save then **overwrites the restored files with stale RAM** | **Medium** | long-standing |
| S-3 | At-least-once note processing: silent delete failures can re-run handlers (duplicate SMS) | **Medium** | long-standing |
| S-4 | 5-minute dirty-save window replays alert state on reboot (rate-limit + reminder anchors) | **Medium** | long-standing |
| S-5 | Single-threaded HTTP: one slow client can stall telemetry/notecard for ~10 s per request | **Medium** | long-standing |
| S-6 | `gPaused` gates inbound only — daily email, FTP backup, viewer summary still fire while "paused" | Low | long-standing |
| S-7 | Admin PIN stored plaintext in `/fs/server_config.json` (FTP creds are obfuscated) | Low | long-standing |
| S-8 | Single global session token; ADC-seeded entropy | Low | long-standing |
| S-9 | `health.qo` / `diag.qo` are Notehub-only — client diagnostics invisible to the dashboard | Low | by design, undocumented |
| S-10..S-17 | Smaller items (below) | Low/Info | — |

---

## 1. S-1 (Medium, **v2.2.12 regression — my own change**): `/api/server-settings` partial-apply-then-400

**CONFIRMED.** `handleServerSettingsPost` applies settings *as it parses them*: `productUid` (~L18140), `githubRouteAlias`, `serverFleet`, `sms*`, `serverDownSmsEnabled`, and `viewerEnabled` all mutate `gConfig` **before** the v2.2.12 `viewerNet` block runs its validation. The `viewerNet` block added the handler's **only** mid-mutation early returns:

```cpp
// L18208-18224 — five early returns AFTER gConfig mutations began
if (newIp[0] && !parseDottedQuad(newIp, octets)) {
  respondStatus(client, 400, "Invalid viewer IP address");
  return;   // <- productUid/smsPrimary/viewerEnabled etc. already changed in RAM,
}           //    saveConfig (L18348) never runs, browser is told "nothing saved"
```

Consequences: RAM config diverges from disk until some other path calls `saveConfig`; the UI reports failure while half the form actually applied; a reboot silently reverts the applied half. (The `viewerNet` block itself is safe — it validates into locals before touching `gConfig` — the problem is the *earlier* fields.)

**Fix (recommended): validate-before-mutate.** Hoist the `viewerNet` validation to the top of the handler, before any `gConfig` writes:

```cpp
// After the PIN check, BEFORE any gConfig mutation:
uint8_t vnMode = 0; char vnIp[16] = "", vnGw[16] = "", vnSn[16] = "", vnDns[16] = "";
bool vnPresent = settings["viewerNet"].is<JsonObject>();
if (vnPresent) {
  JsonObject vn = settings["viewerNet"];
  uint8_t octets[4];
  vnMode = vn["m"] | 0; if (vnMode > 1) vnMode = 0;
  strlcpy(vnIp, vn["ip"] | "", sizeof(vnIp));  /* ...gw/sn/dns... */
  if (vnIp[0] && !parseDottedQuad(vnIp, octets)) { respondStatus(client, 400, "Invalid viewer IP address"); return; }
  if (vnMode == 1 && !vnIp[0])                  { respondStatus(client, 400, "Static viewer mode requires an IP address"); return; }
  /* ...gw/sn/dns checks... */
}
// ... existing mutation blocks unchanged ...
// where the old viewerNet block was: apply from the pre-validated locals (no returns).
```

The same convention should be adopted for any future validated field: **all 4xx exits before the first mutation.**

---

## 2. S-2 (Medium): FTP restore-on-boot restores files the server then ignores — and overwrites

**CONFIRMED.** Boot sequence (setup, ~L4383-4420):

```cpp
loadSensorRegistry();          // RAM <- local files
loadClientMetadataCache();
...
if (gConfig.ftpEnabled && gConfig.ftpRestoreOnBoot) {
  if (performFtpRestore(err, sizeof(err))) {
    ensureConfigLoaded();            // reloaded ✓
    loadClientConfigSnapshots();     // reloaded ✓
    loadCalibrationData();           // reloaded ✓
    scheduleNextDailyEmail();
    // loadSensorRegistry()        <- MISSING
    // loadClientMetadataCache()   <- MISSING
    // loadHistorySettings()/loadHotTierSnapshot() <- also not reloaded
  }
}
```

The restore downloads `sensor_registry.json` / client metadata to disk, but RAM keeps the **pre-restore** state. Worse: the periodic dirty-flag save (`REGISTRY_SAVE_INTERVAL_MS`, L~4803) will write that stale RAM state back to disk within minutes, **destroying the freshly restored files**. Net effect: "Restore on boot" (and "Restore Now" from the settings page, same helper) silently does nothing for sensors/alarms/OTA state — the two datasets an operator most wants back after replacing hardware.

**Fix:** after a successful restore, reload everything the restore can touch:

```cpp
if (performFtpRestore(err, sizeof(err))) {
  ensureConfigLoaded();
  loadClientConfigSnapshots();
  loadCalibrationData();
  loadSensorRegistry();          // adopt restored registry
  loadClientMetadataCache();     // adopt restored metadata
  loadHistorySettings();
  loadHotTierSnapshot();
  gSensorRegistryDirty = false;  // don't let the next interval save stomp the files
  gClientMetadataDirty = false;
  scheduleNextDailyEmail();
}
```

(Verify `handleFtpRestorePost` — the "Restore Now" button path — gets the same treatment.)

---

## 3. S-3 (Medium): at-least-once note processing with silent delete failures

**CONFIRMED** (`processNotefile`, L12133-12240). The design is delete-**after**-handle (correct: a crash mid-handler re-delivers rather than losing data), with a poison-note guard (3 consecutive parse failures → force delete ✓). Two gaps:

```cpp
// L12215-12221 — delete of a successfully processed note
J *delReq = notecard.newRequest("note.get");
if (delReq) {                       // NULL-safe ✓
  JAddStringToObject(delReq, "file", fileName);
  JAddBoolToObject(delReq, "delete", true);
  J *delRsp = notecard.requestAndResponse(delReq);
  if (delRsp) notecard.deleteResponse(delRsp);
  // <- if delReq alloc failed, or delRsp == NULL, or delRsp carries "err":
  //    NOTHING is logged and the note stays queued
}
```

1. **Silent delete failure** → the same note is re-read next poll (5 s later) and its handler **re-runs**: duplicate SMS/email for an alarm note, duplicate unload log entry, double-counted history snapshot (the dedupe in `recordTelemetrySnapshot` only catches identical adjacent timestamps). The failure mode is real on a flaky I2C bus — precisely when the Notecard is already misbehaving.
2. **No idempotency**: nothing records the last-processed note identity, so re-delivery after a crash (by design) also re-alerts.

**Fixes (incremental):**
- Cheap: check `delRsp` + its `err` field; on failure, log loudly and increment a `gNoteDeleteFailures` counter surfaced in `/api/notecard/status`.
- Better: per-file "last processed note epoch+uid" kept in RAM; skip handler side-effects (but still delete) when an identical `(clientUid, epoch, file)` tuple repeats back-to-back. That converts duplicate-SMS into a no-op while preserving at-least-once for genuinely new data.

---

## 4. S-4 (Medium): 5-minute dirty-save window replays alert state on reboot

**CONFIRMED.** `gSensorRegistryDirty` state (including `lastSmsAlertEpoch`, the hourly SMS budget array, and reminder anchors) is flushed at most every `REGISTRY_SAVE_INTERVAL_MS = 300000` (L~188, save loop L~4803). Sequence that double-sends:

1. Alarm SMS sent at T0 → `lastSmsAlertEpoch = T0` (RAM only).
2. Power loss at T0+2 min (before the interval save).
3. Reboot loads the registry with the **old** epoch; the client's alarm note is still queued (or re-fires) → rate limiter sees stale state → **second SMS** for the same excursion. Same mechanics re-fire a reminder that was just sent (`checkAlarmReminders` advances the anchor in RAM at L~14444, dirty-flagged but unsaved).

This is a deliberate flash-wear trade-off, but the wear argument is weak here: `saveSensorRegistry()` already exists and alarm sends are rare (rate-limited to 2/hr/sensor).

**Fix:** save the registry immediately after any *alert-emitting* state change (alarm SMS, reminder, stale flag), keeping the 5-minute interval for high-churn telemetry updates:

```cpp
// after sendSmsAlert(...) succeeds in handleAlarm / checkAlarmReminders:
gSensorRegistryDirty = true;
saveSensorRegistry();                 // hash-gate inside makes repeats cheap
gLastRegistrySaveMillis = millis();   // reset the interval so the next flush isn't immediate
gSensorRegistryDirty = false;
```

(If `saveSensorRegistry` is not yet hash-gated the way `saveHotTierSnapshot` is, add the same FNV-gate first — it makes "save eagerly" free when nothing changed.)

---

## 5. S-5 (Medium): one slow HTTP client stalls the whole device

**CONFIRMED** (`readHttpRequest`, L~9672-9820). Header phase and body phase each allow up to **5 s** of waiting (`millis() - start < 5000UL` with `safeSleep(1)` polling); a client that trickles bytes can hold the single-threaded loop ~10 s per request. During that window: no `pollNotecard()`, no alarm SMS, no relay handling. The 30 s watchdog is kicked via `safeSleep`, so it degrades rather than resets — but a hostile or broken LAN device can effectively mute the server by looping such requests (Opta LWIP also has only a handful of PCBs to pin).

Mitigations, in order of value:
1. **Per-chunk idle timeout**: abort if no byte arrives for ~1 s (instead of 5 s total wall time — a *fast* big body still completes):
```cpp
unsigned long lastByteMs = millis();
while (readBytes < contentLength && client.connected()) {
  if (client.available()) { ...read...; lastByteMs = millis(); }
  else if (millis() - lastByteMs > 1000UL) { break; }   // idle abort
  else safeSleep(1);
}
```
2. Reserve `String body` capacity from `Content-Length` up front (avoids the O(n²) `body += c` reallocation the byte loop currently does).
3. Consider processing at most one HTTP request per loop iteration *between* Notecard polls (already the structure) but add a "poll overdue" check inside long handlers (e.g., FTP test) — optional.

Exposure is LAN-only (no WAN listener), so Medium.

---

## 6. S-6 (Low): `gPaused` only pauses *inbound*

**CONFIRMED.** Pause gates `pollNotecard()` (L~4650) and the forced `hub.sync` (L~4661) — but **daily email** (L~4706), **viewer summary publishing** (L~4711), **FTP backup** (L~4860), stale-client checks, and alarm reminders all continue while paused. An operator pausing during maintenance will still generate outbound traffic and alerts computed from freezing data (e.g. a stale-client SMS *caused by* the pause).

**Recommendation:** decide the semantics and enforce them. Most useful: pause = "no outbound alerts + no inbound processing, web UI stays live":

```cpp
if (!gPaused) { checkStaleClients(); checkAlarmReminders(); }
if (!gPaused && gNextDailyEmailEpoch > 0.0 && ...) { sendDailyEmail(); ... }
```
…and surface "Paused — alerts suspended" on the dashboard banner so the trade-off is visible. At minimum, exclude `checkStaleClients` while paused (it *will* false-alarm after 49 h of pause).

---

## 7. S-7 (Low): admin PIN stored plaintext at rest

**CONFIRMED.** `saveConfig` obfuscates FTP credentials (`encodeFtpCredential`, ~L5360) but writes `doc["configPin"] = cfg.configPin` in clear. Runtime compare is constant-time (`pinMatches`, L~1439 ✓). Anyone with the QSPI contents (or an FTP backup of `server_config.json`!) reads the PIN directly — note the FTP *backup* file inherits the plaintext, so the PIN also sits on the FTP server.

**Fix:** reuse the existing obfuscation helper for the PIN (device-salted XOR is already the accepted bar in this codebase), or store a salted hash and compare hashes (`pinMatches` then hashes the candidate — still constant-time on the 32-byte digest).

---

## 8. S-8 (Low): session model notes

**CONFIRMED live during this session (twice):**
- **Single global token** (`generateSessionToken()` on every login, L~9257): any new login logs out the previous browser. With one operator this is fine; with an operator + this agent (or two browsers) it causes surprise logouts mid-work. A tiny 4-slot token table (token + lastSeen, LRU eviction) removes the annoyance without meaningful RAM cost.
- **Entropy** (L~941-968): seed = `micros()` ⊕ `millis()` ⊕ 4 ADC reads. Fine for LAN; would be stronger mixed with the device UID and a persisted boot counter (defeats "same seed after identical cold boot" patterns).
- Token compare is constant-time ✓; auth failures have exponential backoff + lockout ✓; `X-Session` header is injected by a fetch wrapper into **all** fetches including cross-origin ones (observed breaking CORS preflight to api.github.com from a dashboard page). No page legitimately fetches cross-origin today, so this is only a latent token-leak footgun — scope the wrapper to same-origin URLs:

```js
const _F=window.fetch;window.fetch=function(u,o){const sameOrigin=(typeof u==='string')&&!/^https?:\/\//i.test(u);if(sameOrigin){/* inject X-Session */}return _F.call(window,u,o).then(...);};
```

---

## 9. S-9 (Low): `health.qo` / `diag.qo` never reach the server

**CONFIRMED.** Common.h defines `HEALTH_OUTBOX_FILE "health.qo"` (L305) and `DIAG_OUTBOX_FILE "diag.qo"` (L310); the client emits diag notes (e.g. the v2.0.46 `i2c-recovery` diagnostics). Route #1 forwards **11 notefiles** — health/diag are *not* among them (NOTEHUB_ROUTES_SETUP.md L218), there are no `.qi` counterparts in Common.h, and `pollNotecard()` (L12092-12105) has no handler. So client health/diag telemetry is visible **only** in the Notehub event console.

Not a bug (the routing doc is self-consistent), but the dashboard's client health story (i2c error counters, recovery events) ends at Notehub. **Improvement:** add `diag.qo` to Route #1 → `diag.qi`, a trivial `handleDiag` that appends to the client serial-log ring (infrastructure already exists via `addClientSerialLog`), and a `DIAG_INBOX_FILE` define. Cost: one route edit + ~20 lines.

---

## 10. Smaller confirmed findings

| # | Sev | Finding + fix sketch |
|---|---|---|
| S-10 | Low | `processNotefile` drains at most `MAX_NOTES_PER_FILE_PER_POLL` (10) notes per 5 s poll with **no backlog logging**. A 100-note burst (fleet reboot) takes ~1 min to drain silently. Log when the cap is hit; optionally expose per-file backlog in `/api/notecard/status`. |
| S-11 | Low | `handleConfigAck` matches on an 8-hex djb2 hash of the payload (L~11996-12002). If dispatch B replaces pending A and a late ACK for A arrives, `strcmp` fails (different hash) ✓ — but if A and B hash-collide (~1 in 4 × 10⁹) or an ACK races the snapshot rewrite, `pendingDispatch` clears for the wrong config. Acceptable risk; if ever touched, add the dispatch epoch to the ACK echo. |
| S-12 | Info | `handleDaily` alarm reconciliation only evaluates hi/lo — **already guarded**: the diagnostic-type skip-list (`sensor-fault`, `sensor-stuck`, v2.0.0 Fix 9) prevents daily reports from clearing latched diagnostics. No action; documenting so it isn't "rediscovered" as a bug. |
| S-13 | Low | `handleUnload` (L~13335) still reads `doc["k"].as<uint8_t>()` without the v2.2.11 `k >= 1` presence guard added to telemetry/daily. It doesn't upsert sensor records (no phantom risk), but a missing `k` yields unload log entries for "sensor 0". Add the same 3-line guard for consistency. |
| S-14 | Info | Config `_ts` semantics: dispatch stamps `lastDispatchEpoch = currentEpoch()` *before* sending (L12004 → send L12020 ✓), and retries re-stamp (L12076). A retry after a *lost ACK* therefore carries a newer `_ts` and the client re-applies an already-applied config — harmless (idempotent apply + fresh ACK) but worth knowing: "applied" ACKs can arrive twice. |
| S-15 | Low | Perf niceties: `handleDebugSensors` builds its JSON via `String +=` chains (only String-concat responder left — convert to `JsonDocument`); `respondJson(doc)` walks the doc twice (`measureJson` + `serializeJson`) — fine for small payloads, avoid for `/api/history`; `sendSmsAlert` re-parses the contacts JSON per call (mitigated by `gContactsCache`, first call after boot hits disk). |
| S-16 | Info | Client config payloads are hard-capped at 4096 B by the snapshot cache (`cacheClientConfigFromBuffer` rejects, dispatch aborts BEFORE sending and surfaces `PayloadTooLarge` to the UI ✓). Multi-sensor configs with learned calibration approach this. When it's hit for real, raise `CLIENT_CONFIG_SNAPSHOT_PAYLOAD_MAX` (RAM allows) rather than splitting the protocol. |
| S-17 | Low | Schema-version (`_sv`) checks are inconsistent: enforced for inbound future-schema notes on the server (`processNotefile` gate ✓), checked with a warning on the viewer summary ✓, checked on the client relay path ✓ — but the client's `config.qi` apply path does **not** look at `_sv`, and neither do unload/location/sync handlers. All fields currently default safely; add `_sv` warnings (not rejections) to the client config path before the next schema bump. |

---

## 11. Client/viewer interaction — verified sound

Cross-checks that came back **correct** (worth recording):

- **Telemetry/alarm/daily field contracts** match between client emit and server parse, including the solar `sc*` mirror, `v`/`vs` voltage gating, and the 1-based `k` guards (v2.2.11) on telemetry + daily.
- **Config round-trip**: `_cv` hash + `_ts` epoch are injected on every dispatch (L11885-11887); snapshot is cached **before** the Notecard send so a Notecard failure can't orphan an un-cached dispatch; client defers its ACK until flash persistence succeeds; server clears `pendingDispatch` only on hash match.
- **Viewer summary round-trip** (v2.2.12 `net` object included): server emits `net{m,ip,gw,sn,dns,rev}` only when `rev > 0`; the viewer's `applyServerNetConfig` has a **monotonic revision guard** (`rev <= gConfig.netConfigRev → ignore`), persists to QSPI, applies mode 0 and 1 both, and only re-inits Ethernet after first bring-up. Out-of-order summaries cannot regress the profile.
- **Relay commands**: client validates `_target` UID and `_sv`; duplicate delivery is bounded by client-side cooldown + hardware state.
- **Viewer contacts**: server-authoritative echo (`vc`) with pruning; `gViewerContactsSyncedEpoch` on the viewer is currently write-only (dead field — harmless; remove or use for a "last synced" UI hint).

---

## 12. Verified-correct list (for coverage)

- Atomic persistence everywhere: `posix_write_file` **is** `serverWriteFileAtomic` (L1382-1384) — registry, metadata, contacts, config, hot tier all tmp+rename. Load-order in setup is state-before-network ✓.
- Poison-note recovery (3 strikes → force delete, L12222-12228) and future-`_sv` rejection with logging.
- Constant-time PIN + token compares; exponential auth backoff/lockout; PIN brute-force rate limiting.
- WDT serviced through FTP transfers (`serviceTransferWatchdog` per file), SMS sends (`dfuKickWatchdog` per Notecard txn), and `safeSleep` in wait loops. `checkStaleClients` worst case is 20 × 64 trivial iterations — no WDT risk (subagent claim rejected, Appendix B).
- `doc.overflowed()` checked on all large JSON responders (`sendSensorJson`, `sendClientDataJson`, summary-mode clients JSON).
- 32 KB/16 KB/4 KB load-size ceilings on registry/metadata/config files; `strlcpy` used consistently; `snprintf` truncation fallbacks on SMS composition (W-4 pattern).
- `scheduleNextDailyEmail` guards unsynced clock; `tankalarm_computeNextAlignedEpoch` returns 0.0 for `epoch <= 0` so the viewer-summary scheduler is also safe (subagent claim rejected).
- PROGMEM chunked streaming for >32 KB pages; static 512 B chunk buffer.

---

## 13. Prioritized recommendations

| P | Action | Ref |
|---|---|---|
| P0 | Validate-before-mutate in `/api/server-settings` (fixes the v2.2.12 regression) | S-1 |
| P0 | Reload registry + metadata (+history) after FTP restore; clear dirty flags | S-2 |
| P1 | Log + count note-delete failures; consider last-note idempotency for alarm handler | S-3 |
| P1 | Eager registry save after alert-emitting state changes (hash-gated) | S-4 |
| P1 | Per-chunk idle timeout + `body.reserve()` in `readHttpRequest` | S-5 |
| P2 | Define `gPaused` semantics; gate stale checks/reminders/daily email while paused | S-6 |
| P2 | Obfuscate/hash the PIN at rest (it currently also lands in FTP backups) | S-7 |
| P2 | Scope the JS fetch wrapper's `X-Session` injection to same-origin | S-8 |
| P2 | Route + ingest `diag.qo` so client diagnostics reach the dashboard | S-9 |
| P3 | `k>=1` guard in `handleUnload`; backlog logging in `processNotefile`; `handleDebugSensors` → JsonDocument; `_sv` warning on client config apply; multi-session token table | S-13/S-10/S-15/S-17/S-8 |

All P0/P1 items are server-only changes (one server flash); none require client firmware, viewer firmware, or Notehub route changes except S-9 (one route edit).

---

## Appendix A — review coverage map

| Subsystem | Coverage |
|---|---|
| loop() ordering, watchdog, pause, schedulers | Full pass + manual verification of scheduler guards and loop gates |
| Notecard poll/process/delete, inbound handlers | Full pass + manual read of `processNotefile` delete/poison paths |
| HTTP parse, session, auth, response building | Full pass + manual read of `readHttpRequest` timeouts, `requireValidPin`, token entropy |
| Persistence (config/registry/metadata/contacts/hot tier/history), FTP backup/restore | Full pass + manual verification of atomicity shim and restore reload gap |
| Alerting (SMS/email/reminders/stale/OTA reconcile), daily email schedule | Full pass; rate-limit interplay traced |
| Wire protocol server↔client↔viewer (incl. v2.2.12 `net`) | Field-by-field cross-check; discrepancies resolved by manual reads |
| Web page JS layer | Not re-reviewed (covered by the 07/20 issue-313 review) |

## Appendix B — claims investigated and **rejected** (do not re-report)

1. *"Sensor registry / client metadata writes are non-atomic (`posix_write_file`)"* — *wrong*: `posix_write_file` is a shim over `serverWriteFileAtomic` (L1382-1384).
2. *"`checkStaleClients` nested loop can exceed the watchdog"* — *wrong scale*: bounded by `MAX_CLIENT_METADATA 20` × `MAX_SENSOR_RECORDS 64` trivial iterations.
3. *"`scheduleNextViewerSummary` lacks an epoch guard"* — the guard lives inside `tankalarm_computeNextAlignedEpoch` (returns 0.0 for epoch ≤ 0; loop gate requires > 0).
4. *"Server never injects `_ts` into config dispatches"* — it does (L11887), stamped fresh before send (L12004 → L12020).
5. *"Configs 4-8 KB send but fail to cache, orphaning dispatch"* — cache failure **aborts before sending** (L11963-11966).
6. *"Viewer net-profile revision has no ordering guard"* — `applyServerNetConfig` ignores `rev <= gConfig.netConfigRev` (monotonic) and persists the revision.
7. *"Viewer receives `net.m` but never applies the mode"* — `useStaticIp = wantStatic` is applied, persisted, and Ethernet re-initialized.
8. *"Contact fields can overflow fixed buffers"* — contacts live in ArduinoJson documents end-to-end (bounded by the 16 KB body cap + 100-contact cap), no fixed per-field buffers on the storage path.
9. *"handleDaily reconcile breaks latched diagnostic alarms"* — the v2.0.0 Fix 9 skip-list already excludes `sensor-fault`/`sensor-stuck`.

*Prepared 2026-07-21 against v2.2.12. Line numbers drift with edits — anchor searches on the quoted code, not the numbers.*
