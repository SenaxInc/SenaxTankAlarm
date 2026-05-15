# Analysis of Proposed Improvements (May 14, 2026)

This document expands on the proposed codebase improvements for the Tank Alarm server, following the fixes to the FTP backup file synchronization issue.

## 1. Move FTP Backup Off the Main Loop (Asynchronous Backups)
Currently, `performFtpBackup()` runs synchronously and blocks for up to ~65 seconds per file due to LWIP `TIME_WAIT` delays on the Opta.
*   **Problem:** This blocks the main `loop()`, shutting down the web server (`gWebServer.end()`) and preventing the dashboard and Modbus comms from functioning.
*   **Proposed Solution:** Implement a state machine or ticker for the FTP backup. Instead of blocking, a single file could be processed every loop cycle or triggered via a background timer. This also ensures the hardware watchdog is serviced properly.

## 2. Standardize Error Propagation and Synchronous Saves
We updated `handleServerSettingsPost` to synchronously save the configuration and issue an HTTP 500 error upon failure.
*   **Problem:** `/api/contacts` and `/api/email-format` already save synchronously, but they still return HTTP 200 with `success:true` even when persistence fails (`saved:false`). Other internal save paths such as sensor registry, client metadata, hot tier snapshots, and calibration data still use `void` saves, so failures cannot be propagated cleanly.
*   **Proposed Solution:** 
    *   Update the remaining `void` save functions (`saveSensorRegistry`, `saveClientMetadataCache`, `saveClientConfigSnapshots`, `saveCalibrationData`, `saveHotTierSnapshot`) to return `bool`.
    *   Update handlers that already return `saved:false` to use an accurate HTTP 5xx status on persistence failure.
    *   Keep the dirty flags set when a periodic save fails so the next loop can retry instead of silently discarding the pending change.

## 3. Flash Wear Reduction
The Opta uses LittleFS, which relies on erase-write cycles that degrade flash memory over time.
*   **Problem:** Before initiating an FTP backup, `prepareLocalBackupFilesForFtp()` forces a rewrite of `server_config.json` and `history_settings.json` regardless of whether they have actually been modified in memory.
*   **Proposed Solution:** Gate the file writing logic during backup preparation. Only save files back to the filesystem if their respective dirty flags (`gConfigDirty`, `gHistoryDirty`, etc.) are set.

## 4. Duplicate Arduino Library Resolution
During the compilation step, the `arduino-cli` flagged duplicate libraries in the local development environment (`TankAlarm-112025-Common` v1.2.3 vs 1.6.3, and `ArduinoOPTA-FTPS` vs `FTPSclientOPTA`).
*   **Problem:** Having multiple library versions is a prevalent source of undefined behavior, linking errors, and inconsistent builds compared to the CI pipeline.
*   **Proposed Solution:** Run `arduino-cli lib uninstall` locally to purge the outdated/duplicate libraries ensuring the configuration exactly matches the CI `arduino-libraries` working directory.

## 5. Buffer Size vs Large File Scaling
The per-file read buffer was expanded to a static array of size `FTP_MAX_FILE_BYTES + 1` (~24 KB).
*   **Problem:** While using `.bss` memory is safe on the Opta, dynamic caches like `client_config_cache.txt` could theoretically grow beyond 24 KB.
*   **Proposed Solution:** Instead of buffering the entire file at once, consider streaming the file chunks directly to the FTP socket. Alternatively, put in strict size constraints and log descriptive "File Too Large" errors avoiding silent truncation.

## 6. Symmetrical Validation for File Restoration
*   **Problem:** Just as the backup can fail due to FS issues or missing required files, the FTPS restore process could pull down corrupted, empty, or incomplete files.
*   **Proposed Solution:** Apply similar readiness checks on the FTPS download side. Download the file as a temporary `.tmp` payload, check its integrity/size, and atomic-rename it to replace the true config file, reverting on errors to prevent bricking the server configuration.

---

## Pros & Cons of Each Proposal

### 1. Asynchronous FTP Backup (State Machine)
**Pros**
*   Web UI / dashboard remain responsive during the multi-minute backup window (currently `gWebServer.end()` is called at line 4489 to free LWIP PCBs).
*   Watchdog is serviced naturally each loop iteration — no risk of starvation if a file STOR hangs.
*   Modbus/Solar polling and Notecard heartbeats keep flowing during the 65 s `FTP_BACKUP_INTER_FILE_DELAY_MS` waits.
*   Enables progress reporting to the dashboard ("Uploading 3/8 …").

**Cons**
*   Significant refactor: must persist FTP session/socket state across loops, handle TLS session expiration, and gracefully abort if Ethernet drops mid-cycle.
*   Re-introduces concurrency between web requests (which may mutate config files) and an in-flight backup — needs a write lock or snapshot strategy.
*   The `gWebServer.end()` workaround exists *because* of the Opta LWIP 4-PCB pool limit; an async design must still solve PCB exhaustion, possibly by gating new web connections during the data transfer phase only.
*   Higher risk of subtle bugs (partial uploads, half-closed sockets) than the current "stop-the-world" model.

**Recommendation:** High value, but defer until #2 and #4 land — it's the largest change and benefits from a clean baseline.

---

### 2. Standardize Error Propagation (`bool` Saves + Synchronous POST)
**Pros**
*   Matches the pattern we just shipped for `/api/server-settings` — consistent UX and observability.
*   Surfaces filesystem failures (mount loss, ENOSPC, atomic-rename failure) at the HTTP boundary instead of silently dropping data.
*   Makes the dirty-flag system honest: today periodic saves such as `saveSensorRegistry()` and `saveClientMetadataCache()` clear their dirty flags even though the save functions cannot report failure.
*   Fixes current API ambiguity: contacts and email format can report `success:true` with `saved:false`, which forces the UI to infer a partial failure from a secondary field.
*   Easy to unit-test individual save functions once they return `bool`.

**Cons**
*   Touches many call sites — `saveSensorRegistry()` alone is invoked from `loop()` (line 4440) and from sensor-registration paths (line 8237); each caller must decide how to react to a `false` return.
*   POST handlers become slightly slower (synchronous flash write before responding ~50–200 ms on LittleFS).
*   Risk of double-saves if both the handler and the deferred dirty-flag path run — must remove the redundant `gXxxDirty=true` assignments.
*   Some caches are intentionally best-effort. Turning every write failure into a user-visible HTTP 500 could make low-priority persistence failures look more severe than they are.

**Recommendation:** High value, low risk. Best next step.

---

### 3. Flash Wear Reduction in `prepareLocalBackupFilesForFtp()`
**Pros**
*   Eliminates 1–2 unnecessary erase/rewrite cycles per backup. With "backup on change" enabled and frequent settings tweaks, this compounds quickly on LittleFS.
*   Cheap to implement (one-line guard per save call).

**Cons**
*   Relies on dirty flags being 100% accurate — if any code path mutates `gConfig` without setting `gConfigDirty`, the backup will upload a stale on-disk file.
*   The current "force save" behavior is defensive and self-healing; gating it removes that safety net.
*   Marginal benefit if backups run only a few times per day — LittleFS wear leveling already absorbs most of the cost.

**Recommendation:** Implement, but pair with an audit that every `gConfig.* =` assignment in the codebase also sets `gConfigDirty = true`.

---

### 4. Resolve Duplicate Local Libraries
**Pros**
*   Eliminates "works on CI but not locally" (or vice versa) class of bugs.
*   Faster `arduino-cli compile` (no library-resolution warnings).
*   Future contributors won't inherit the inconsistent environment.

**Cons**
*   Purely a developer-environment change — zero firmware impact.
*   Risk of breaking *other* sketches in your Arduino libraries folder that depend on the older versions; verify nothing else uses `ArduinoOPTA-FTPS@0.2.0` or `TankAlarm-112025-Common@1.2.3` before uninstalling.

**Recommendation:** Quick win. Document the "blessed" library versions in the repo (e.g., a `library_versions.txt` or update `ARDUINO_COMPILATION_GUIDE.md`).

---

### 5. Streaming vs Buffered FTP Uploads
**Pros**
*   Removes the hard 24 KB ceiling — `client_config_cache.txt` can already exceed this in fleets >50 clients.
*   Frees ~24 KB of `.bss` permanently (currently always reserved even when no backup runs).
*   Aligns with how FTP/FTPS naturally works — chunked send loop instead of one-shot STOR.

**Cons**
*   Requires touching `FTPSclientOPTA` API usage; current helper assumes a contiguous buffer.
*   Mid-stream errors are harder to recover from (partial file on the remote).
*   TLS record framing means small chunks have higher overhead than one big buffer; need to pick a sensible chunk size (4–8 KB).

**Recommendation:** Worthwhile *if* `FTP_MAX_FILE_BYTES` becomes a real constraint. For now, expanding the cap and emitting a clear "file too large" log line is the smaller, safer change.

---

### 6. Symmetrical Restore Validation
**Pros**
*   Prevents a corrupt FTPS download from bricking the server (today, a half-downloaded `server_config.json` would replace the good copy and fail to parse on next boot).
*   Reuses the existing `tankalarm_posix_write_file_atomic` helper — minimal new code.
*   Provides natural rollback: keep the previous file as `.bak` until the new one parses successfully.

**Cons**
*   Adds latency to each restored file (write to `.tmp`, fsync, parse-validate, rename).
*   Parse-validation requires per-file knowledge (JSON schema vs raw text); generic byte-length checks miss semantic corruption.

**Recommendation:** Implement at least the "download to `.tmp` + atomic rename" half; defer schema validation to a later pass.

---

## Additional Ideas

### 7. Backup Coalescing / Debounce
The current auto-backup arm logic at line 4477 waits 20 s of quiet before triggering, but every settings POST re-arms it. A user clicking "Save" repeatedly (or bulk-importing contacts) can still trigger back-to-back multi-minute backups.
*   **Suggestion:** Add a `gLastBackupCompletedMs` and a `MIN_BACKUP_INTERVAL_MS` (e.g., 5 minutes). Skip new backups inside that window, but mark `gPendingFtpBackup` so the *next* eligible window picks them up.
*   **Pros:** Caps worst-case FTP load; protects the upstream FTPS server from accidental DOS.
*   **Cons:** Slight delay between "Save" and remote backup visibility — needs UI affordance ("Backup queued, will run at HH:MM").

### 8. Accurate HTTP Status from `/api/ftp-backup`
The current manual backup handler now responds with HTTP 200 on success and HTTP 500 on outright failure. That is better than the original behavior, but the endpoint still collapses distinct failure classes into a generic 500.
*   **Suggestion:** Map `FtpResult` to more specific status codes — 200 (all uploaded), 207 (partial success: some required files failed), 409 (backup already in progress), 502 (FTP/FTPS connection or TLS failure), 503 (filesystem unavailable or no local required files), and 507 (local storage full where detectable).
*   **Pros:** Makes the API REST-correct and trivially monitorable from external tools.
*   **Cons:** The dashboard JS already checks `res.ok`, but any external caller expecting only 200/500 would need to handle the richer status range. HTTP 207 is also less commonly handled by lightweight clients.

### 9. Expose Per-File Backup Result in JSON Response
`FtpResult` currently exposes counts plus a comma-separated `failedFiles` string. It does not track a full per-file upload/skipped matrix.
*   **Suggestion:** Add a compact per-file result list with `name`, `required`, `status`, and optional `reason`. Always include it in the response so the UI can render `server_config.json: uploaded`, `history_settings.json: uploaded`, `contacts_config.json: skipped optional`, or `client_config_cache.txt: too large`.
*   **Pros:** Closes the loop on the original bug — users see *why* an optional file was skipped instead of a generic error.
*   **Cons:** Requires a little RAM for the per-file result list. On Opta, keep this as fixed-size structs or short strings instead of large dynamic JSON arrays.

### 10. Re-entrancy / Rate-limit Guard for `gPendingFtpBackup`
Right now, `gPendingFtpBackup` is a simple `bool`. If a settings save lands during an in-progress backup (`gBackupInProgress==true`), the flag is set and another backup fires immediately on completion — even if nothing meaningful changed.
*   **Suggestion:** Track a `gBackupRequestEpoch` counter; only re-run if epoch advanced *and* the cooldown from #7 has elapsed.
*   **Pros:** Removes the "two backups back-to-back" footgun.
*   **Cons:** None significant — pure logic change.

### 11. FTP Credential Update Semantics
The password path is mostly safe today: the UI leaves the password input blank and only sends `ftp.pass` when the user typed a value, while the server only updates the password if the key is present. The username, host, path, and fingerprint are always sent by the UI, so blank fields still clear those values.
*   **Suggestion:** Keep the existing "missing password means leave unchanged" behavior, add an explicit "clear saved FTP password" action, and consider applying the same explicit-clear pattern to sensitive FTPS certificate/fingerprint values.
*   **Pros:** Prevents accidental credential loss and lets the UI avoid ever echoing the password back to the browser.
*   **Cons:** Slightly more UI/state complexity; users need a visible way to intentionally clear stale credentials.

### 12. Regression Test in `TankAlarm-112025-FTPS_Server_Test`
The existing test harness already spins up a local FTP server with `ftp_root/` fixtures. Adding a case that:
1. Wipes `/fs`,
2. POSTs to `/api/server-settings` with `ftpBackupOnChange:true`,
3. Asserts HTTP 200 + verifies `server_config.json` and `history_settings.json` land on the fixture FTP root,

…would have caught the original bug pre-merge. Worth adding alongside the fixes from this round.
*   **Pros:** Locks in the fix; future refactors can't regress without a CI failure.
*   **Cons:** Requires maintaining the test harness; modest one-time setup cost.

### 13. Watchdog Accounting During Inter-File Sleep
The 65 s `FTP_BACKUP_INTER_FILE_DELAY_MS` wait loop does call `serviceTransferWatchdog()` during the delay. The remaining risk is future transfer/retry loops or client-snapshot backup paths that may add blocking waits without the same call.
*   **Suggestion:** Treat watchdog service as a checklist item for any new FTP/FTPS wait, retry, Notecard purge, or long filesystem loop.
*   **Pros:** Keeps a solved reliability issue from returning during later refactors.
*   **Cons:** None significant, but too many scattered watchdog calls can make it harder to see where blocking behavior still exists.

### 14. Backup Manifest With Firmware/Schema Metadata
Right now, the remote FTP directory contains individual files, but no authoritative manifest describing which firmware produced them, which files are required, and whether each file was complete.
*   **Suggestion:** After a successful backup, upload a small `backup_manifest.json` containing firmware version, schema version, server UID/name, timestamp/epoch if available, list of uploaded/skipped files, byte lengths, and optional CRC32/SHA-256 checksums.
*   **Pros:** Restore can validate compatibility before applying files. Support/debugging becomes much easier because the remote folder tells you exactly what was backed up.
*   **Cons:** Requires one extra FTP upload and one more file to keep backward-compatible. Cryptographic hashes cost CPU/RAM; CRC32 may be a better embedded compromise.

### 15. Filesystem Health and Capacity Preflight
Several proposed fixes depend on distinguishing "FTP failed" from "local storage is missing/full/unhealthy".
*   **Suggestion:** Add a small filesystem health helper that reports mounted/not mounted, free bytes if available, last save error, and last successful save timestamp. Use it before backup/restore and expose it in the dashboard/system status response.
*   **Pros:** Turns mysterious `check /fs` failures into actionable diagnostics. Also helps decide whether to return 503 vs 507 from API handlers.
*   **Cons:** LittleFS free-space APIs may be limited under Mbed, so the first version may only report mount/read/write probe status rather than exact capacity.

### 16. Restore-on-Boot Safety Controls
`ftpRestoreOnBoot` runs automatically after Ethernet and the web server start. That is convenient, but repeated restore failures or a bad remote config can keep the device in an unhealthy startup pattern.
*   **Suggestion:** Track restore-on-boot attempts and last failure reason. Disable or pause automatic restore after N consecutive failures, while leaving manual restore available from the dashboard.
*   **Pros:** Prevents a bad remote backup or network outage from creating repeated long boot delays. Gives the web UI a chance to tell the operator what happened.
*   **Cons:** Adds persistent state and another recovery path to document; users need a clear way to re-enable restore-on-boot after fixing the FTP server.

### 17. Config Change Detection Before Auto-Backup
The current server-settings handler queues a backup after any successful save when FTP backup-on-change is enabled, even if the POST body writes the same values that were already stored.
*   **Suggestion:** Compute a lightweight `settingsChanged` flag while parsing the POST, and only queue `gPendingFtpBackup` if at least one persisted field actually changed.
*   **Pros:** Reduces unnecessary FTP load and flash/remote churn without delaying legitimate backups.
*   **Cons:** Adds comparison logic throughout a long handler; easy to miss fields unless helper setters are introduced.

---

## Flash Wear and Durability Strategy

This section consolidates the analysis of how the Opta's flash storage holds up under this firmware's write patterns, and what to do about it.

### Hardware reality
*   **Storage medium:** On-board QSPI NOR flash (Macronix MX25L12835F-class, ~16 MB) hosting LittleFS, mounted at `/fs`.
*   **Per-sector endurance:** ~100,000 erase cycles datasheet typical; commonly 10× that before bit errors emerge in practice.
*   **Wear leveling:** LittleFS spreads block erases across the entire free area. Active config files total well under 200 KB, so the *effective* endurance for the working set is hundreds of millions of logical writes.
*   **Atomic write cost:** Each `tankalarm_posix_write_file_atomic` call writes a full new file to `*.tmp`, then renames. That is one full erase/program cycle of the file's blocks plus the directory blocks.

### Where the real risk lives
*   The hardware itself is robust. Risk is dominated by **write cadence**, not raw endurance.
*   The single highest-frequency persistent writer is `saveHotTierSnapshot()`. At "save every 5 minutes" cadence, that is ~105k saves/year. With LittleFS wear leveling over 16 MB, that still maps to decades of life — but only if the snapshot path stays throttled and the data actually changed.
*   The realistic failure mode is a regression that starts hot-writing flash. Examples that would have to be guarded against:
    *   A loop or handler that sets `gConfigDirty = true` on every iteration or every telemetry packet.
    *   A periodic save that runs unconditionally instead of gating on a dirty flag.
    *   A retry loop that rewrites the same file after every transient FTP failure.
*   The current 5-minute throttle for sensor registry / client metadata, the dirty-flag gating on `gConfig`, and the atomic write helper are the actual safety mechanisms keeping wear within budget.

### Should backups skip local flash and go straight to FTP?
**No.** FTP/FTPS is appropriate as the off-device durable tier, not as primary storage.
*   **Boot availability:** The device must serve dashboard, Modbus, and SMS alarm logic before Ethernet — let alone an FTP server — is reachable. Local flash is the only thing that survives a cold boot with current state in place.
*   **Speed:** Each FTPS STOR on this hardware costs roughly 60–90 seconds (TLS handshake + LWIP TIME_WAIT). Hot-path telemetry cannot be checkpointed at that rate.
*   **Reliability:** Power blips, switch reboots, ISP outages are all common. Without a local cache, any outage during a write window equals data loss.
*   **Bootstrap dependency:** FTP credentials themselves cannot live only on FTP.
*   **Safety adjacency:** Alarm and SMS logic should not depend on a network round-trip for state it needs locally.

The right model is the one already in place, just tightened:
*   **RAM** — working state, mutate freely.
*   **Flash** — durable local cache, written only when state actually changed and on a throttle.
*   **FTP/FTPS** — off-device durable backup, periodic and debounced, asymmetric required-vs-optional.
*   **Notecard** — transient queues and cellular fallback.

### RAM reality and current budget
The flash-wear picture is good, but RAM deserves equal attention. The recent Opta server build reports roughly **276-277 KB of global/static memory** out of a **523,624-byte dynamic-memory region**, leaving about **246-247 KB for stack, heap, TLS buffers, `String`, `JsonDocument`, and `malloc` allocations**. That is healthy headroom, but it is not a free-for-all.

Important RAM observations from the current sketch:
*   **Do not assume 8 MB SDRAM is available to this sketch.** The Opta build target exposes a ~523 KB dynamic-memory region. Any external memory assumption should be verified on the exact board/core and backed by an explicit allocator/section before being used in the design. Treat ~247 KB free stack/heap as the real budget.
*   **Fixed pools are predictable and mostly appropriate.** Examples include `gSensorHistory[MAX_HISTORY_SENSORS]` (20 sensors × 90 snapshots), `gSensorRecords`, `gClientConfigs[MAX_CLIENT_CONFIG_SNAPSHOTS]`, serial log rings, alarm/unload/transmission logs, and client metadata. These cost BSS, but they do not fragment the heap.
*   **The 24 KB static FTP file buffer is acceptable today.** It moved a previous stack-risk buffer into BSS and the build still has ~52% global RAM use. The danger is repeating that pattern without a budget; every new static scratch buffer permanently reduces stack/heap room.
*   **Transient heap spikes are the real RAM risk.** Several flows temporarily hold the same information multiple times: HTTP request body `String`, ArduinoJson `JsonDocument`, serialized output `String`/`malloc` buffer, and sometimes a cached copy. Hot-tier snapshot save/load is the clearest example: in-memory ring buffer + dynamic JSON tree + serialized buffer up to 65 KB.
*   **`String` use is manageable but should stay bounded.** `readHttpRequest()` reserves the Content-Length and body caps are enforced, which is good. The main risk is repeated large `String` allocation/reallocation in contacts, email format, HTML injection, archive responses, and error/status messages.
*   **TLS/FTPS adds hidden memory pressure.** The compile report does not show runtime TLS/socket heap use. Backup/restore should continue to avoid overlapping with other large heap users.

### RAM handling principles
*   Prefer **bounded static pools** for long-lived state and **small stack buffers** for short-lived formatting.
*   Prefer **streaming** for large file/JSON operations instead of building full JSON trees plus full serialized buffers.
*   Keep HTTP body caps endpoint-specific. Do not raise `MAX_HTTP_BODY_BYTES` globally without measuring heap headroom under FTPS and JSON parsing.
*   Treat `/fs/*.tmp` files as durability tools, not RAM scratch space. Temporary files on LittleFS still cost flash erase/write cycles.
*   Avoid overlapping peak-memory operations: large POST body parsing, hot-tier snapshot save/load, FTP/FTPS transfer, and archive JSON generation should not run concurrently.

### Lower-wear write strategies worth considering
*   **RAM-first, flash-deferred (extend current pattern).** Set a dirty flag on mutation and let a single timer do the actual save. Already used for sensor registry and client metadata; should be applied to any new persistent state.
*   **Coalesce multi-file flushes.** One periodic tick that walks every dirty flag and writes only those files in a single pass, instead of multiple independent intervals each writing one file.
*   **Hash-gate hot writes.** For high-frequency writers (most importantly the hot-tier snapshot), hash the in-memory representation and skip the write if the hash matches the previous successful save. This eliminates "timer fired but nothing actually changed" cycles.
*   **Append-only journal for time series.** Replace full-rewrite snapshots with a circular append-only log. LittleFS only re-erases when a block fills, not on every record. Trade-off is more code complexity and a slower restore at boot.
*   **Use RAM as the working buffer, but stay inside the verified budget.** Keep large live structures (history, recent telemetry) in RAM, and only checkpoint to flash on shutdown, scheduled intervals, or before risky operations. Do not assume external SDRAM unless the board/core exposes it explicitly.
*   **Push transient data to the Notecard outbox.** The Notecard manages its own wear-leveled flash for queued notes. Anything that can be expressed as a Note instead of an `/fs` file costs zero Opta flash wear.
*   **Keep LittleFS block tuning at defaults.** Larger blocks reduce metadata churn but increase per-save erase cost. Default tuning is fine for this workload — only worth touching if profiling shows a hotspot.

### Implementation ideas, ordered by leverage

These are the concrete steps that would have the biggest impact for the least disruption:

1. **Audit every `gXxxDirty = true` and unconditional save site.** ~20 sites today. Confirm none fire on every loop iteration, every telemetry packet, or every retry. This is the single most effective defense against a wear regression.
2. **Add a write-rate observability counter.** Maintain `gFlashWritesSinceBoot` (and ideally a per-file counter) and expose it in `/api/system-status` and the dashboard. A regression that starts hot-writing flash becomes immediately visible instead of silently destructive. Cost: a few bytes of RAM and one increment per save.
3. **Add RAM observability next to the flash counter.** On Mbed targets, use guarded heap/stack statistics if available (for example `mbed_stats_heap_get()` / stack stats) and expose: current free heap, largest allocatable block if available, minimum-ever free heap, and last allocation failure location. If those APIs are unavailable, still expose compile-time static budgets and handler-level high-water estimates.
4. **Print a boot-time memory budget table.** Log `sizeof(gSensorHistory)`, `sizeof(gClientConfigs)`, `sizeof(gSensorRecords)`, serial log buffers, `FTP_MAX_FILE_BYTES`, and the current global memory usage from the build notes. This makes future BSS growth visible during normal serial review.
5. **Hash-gate `saveHotTierSnapshot()` and any other periodic full-file rewrite.** Compare a CRC32 (or even a simple length+rolling sum) of the serialized payload against the last-saved value; skip the write on a match. CRC32 is cheap on Cortex-M7.
6. **Stream hot-tier snapshot writes.** Instead of building a full `JsonDocument`, measuring it, allocating a second full buffer, and then writing, stream JSON directly to a temp file in chronological order. This reduces peak heap and avoids a second full copy of the hot tier.
7. **Stream hot-tier snapshot reads if feasible.** Current load caps the file at 65 KB and reads it into a `malloc` buffer before deserializing. A file/stream reader wrapper would avoid the full buffer, though it may be a larger ArduinoJson integration change.
8. **Gate `prepareLocalBackupFilesForFtp()` on dirty flags (item #3 in the main proposals).** Avoid the "force save before backup" cycle when nothing changed.
9. **Add a write-rate alarm threshold.** If `gFlashWritesSinceBoot` exceeds an expected ceiling (e.g., > 200/hour sustained), log a warning and surface it in the dashboard. Catches future regressions that slip past code review.
10. **Add endpoint memory budgets.** Record expected max request body, expected JSON parse size, and expected response size for `/api/config`, `/api/contacts`, `/api/email-format`, `/api/history`, and FTP backup/restore. Refuse requests early when they would exceed the budget.
11. **Reduce duplicate contact-cache allocations.** Contacts currently pass through body `String` → `JsonDocument` → serialized `String` → `gContactsCache`. Prefer reusing/reserving the cache buffer, or only serializing once after validation.
12. **Move FTP uploads toward streaming.** This removes the permanent 24 KB BSS upload buffer and also removes the 24 KB file-size ceiling. This is lower priority than hot-tier streaming because the current static buffer is safe, but it is the right long-term shape.
13. **Document a wear and RAM budget in `ARDUINO_COMPILATION_GUIDE.md` or a future memory budget document.** Record expected write cadence per persistent file plus expected RAM cost per major pool, so a future contributor adding a new save path or static buffer has a number to compare against.
14. **(Larger change) Move hot-tier history to an append-only journal.** Reduces snapshot rewrites by an order of magnitude. Worth the complexity only if observability data shows the snapshot path dominates wear.

### Review of the proposals with optional FTP in mind
FTP/FTPS is an **optional** off-device tier. Many deployments will never configure an FTP host, which means **local flash is the only durable store**. That single fact reshapes how the earlier proposals should be ranked and implemented:

*   **Flash wear is the dominant durability concern, not FTP throughput.** When FTP is disabled, every persistent change lives and dies on the on-board QSPI NOR. The earlier proposals that protect flash (dirty-flag audit, hash-gating, write-rate counter, hot-tier streaming, append-only journal) move from "nice to have" to **load-bearing**. The proposals that improve FTP semantics (async state machine, manifest, richer status codes) only matter for the subset of deployments that opt in.
*   **Any code path must behave correctly with FTP fully disabled.** Audit every call site that conditionally runs backup logic and confirm it is a true no-op (no stub connection attempt, no spurious dirty-flag set, no log spam) when `gConfig.ftp.host` is empty or `ftpEnabled` is false. The "backup on change" auto-arm logic at line 4477 in particular must not queue work that will never run.
*   **Do not let "FTP will catch it" become an excuse for higher local write rates.** Several proposals (e.g., #7 backup coalescing, #10 re-entrancy guard) implicitly assume backups are the bottleneck. They should not relax local save throttles. Local throttle (5-minute registry/metadata cadence, dirty-flag gating, hash-gate hot-tier) is what actually protects the device.
*   **Restore-on-boot (#16) needs a "no FTP configured" branch.** Today's logic should already short-circuit, but verify it does so before touching Ethernet or DNS. Otherwise FTP-less devices pay a startup penalty for a feature they do not use.
*   **Backup manifest (#14) and restore validation (#6) remain valuable, but only inside the FTP-enabled subset.** Do not add manifest writes to the local flash path; that is pure wear with no durability gain.
*   **Filesystem health (#15) becomes more important, not less, when FTP is absent.** With no off-device copy, the only signal that flash is failing is local — surface mount status, free bytes if available, and consecutive save failures in `/api/system-status` and the dashboard.
*   **The hot-tier snapshot is the highest-leverage target either way.** It is the highest-frequency writer, so hash-gating (#5 in the implementation list) and streaming (#6/#7) reduce wear on every device regardless of FTP configuration. Prioritize these ahead of any FTP-side work.
*   **`saveHotTierSnapshot()`, `saveSensorRegistry()`, and `saveClientMetadataCache()` are the wear budget.** When FTP is disabled, these three are essentially the entire write story. A regression in any of them is the regression that matters.

#### Best methods to preserve the hardware (FTP-optional ranking)
1.  **Hash-gate every periodic full-file rewrite.** CRC32 the serialized payload, compare to last saved, skip on match. Single biggest wear reduction on devices that never see FTP.
2.  **Audit every `gXxxDirty = true` site.** Confirm none fire per loop iteration, per packet, per retry. This is the regression-prevention step.
3.  **Add `gFlashWritesSinceBoot` plus per-file counters and surface them.** Without this, a wear regression on an FTP-less device is invisible until the flash fails.
4.  **Stream hot-tier snapshot writes** to avoid a second full copy of the data in heap, and to keep the on-disk file's blocks the only thing erased per save.
5.  **Keep the existing 5-minute throttle, the dirty-flag pattern, and the atomic-write helper.** These are the working safety net.
6.  **Document expected write cadence per file.** Future contributors need a number to compare against; "is one save per minute OK?" must be answerable.

#### Best methods to manage the data when FTP is unavailable
*   **RAM is the working tier; flash is the durable tier; Notecard is the optional offload.** When FTP is absent, the Notecard outbox and SMS path are the only off-device channels. Use them for *transient* notifications and queued events — not as a substitute for durable config.
*   **Write only what changed, only when it changed.** Dirty flags + hash-gating cover this.
*   **Keep large live structures (history, recent telemetry) in RAM** within the verified ~247 KB budget; checkpoint to flash on a throttle, on graceful shutdown, and before risky operations (firmware update, FTP restore, manual reboot).
*   **Cap retention by RAM, not by flash.** History depth, fleet size, and serial log length should be sized to the RAM budget so flash sees a bounded snapshot, not a growing file.
*   **Atomic write + temp file remains the right pattern.** It costs one extra erase/write but prevents a half-written file from bricking the device — and on FTP-less devices there is no remote copy to recover from.
*   **Expose a manual "export config" path that does not require FTP.** A dashboard button that streams `server_config.json`, `history_settings.json`, `contacts_config.json`, and `email_format.json` as a single download gives FTP-less operators a real backup story without adding flash writes.
*   **Consider a manual "import config" upload that mirrors the export.** Same atomic-write + parse-validate + rename discipline as proposal #6, applied to an HTTP upload instead of an FTP download. Fully offline, fully optional, and reuses existing helpers.
*   **Treat the Notecard as a soft backup channel for the smallest critical fields only** (e.g., FTP credentials are not appropriate; alarm thresholds and contact list could be). Do not turn it into a config mirror — it is metered and not designed for that workload.

#### Refined implementation priority (FTP-optional aware)
The original 14-step order is still broadly correct, but these adjustments better serve the FTP-less common case:

*   **Promote** "Flash/RAM safety net" (currently step 5) to **step 2**, immediately after #4 library cleanup. It benefits every device.
*   **Promote** hot-tier streaming (currently step 9) to **step 4**. Highest-leverage wear reduction.
*   **Add** a new step: implement a local **export/import config over HTTP** path (no FTP required). Slot it after restore-side hardening (#6/#16) so it shares the validation helpers.
*   **Demote** FTP-only items (#1 async, #14 manifest, #6 restore validation, #16 restore-on-boot safety) below the flash-protection items, since they only help opt-in deployments.
*   **Keep** #2 (error propagation) and #15 (filesystem health) early — both improve diagnostics on every device.

### What not to do
*   Do not move primary state off local flash to "save wear." The bootstrap and offline-availability cost is much larger than the wear savings.
*   Do not lower the existing 5-minute throttle on registry/metadata saves; that is the throttle protecting the highest-frequency writer.
*   Do not add "save just in case" calls outside the dirty-flag pattern. Defensive saves are exactly the regression vector that turns a 30-year flash budget into a 30-day one.
*   Do not raise fleet limits, history retention, `MAX_HTTP_BODY_BYTES`, or `FTP_MAX_FILE_BYTES` without checking both compile-time BSS and runtime heap high-water marks.
*   Do not gate durability features behind FTP being configured. Atomic writes, dirty-flag throttling, hash-gating, and the local export path must all work on a device that will never have FTP credentials entered.
*   Do not use the Notecard as a config mirror to "make up for" missing FTP. It is metered cellular, not durable storage.
*   Do not treat LittleFS temp files as a wear-free scratch mechanism. They are safer for atomicity, not cheaper than RAM.
*   Do not add large local stack arrays in web/FTP/JSON handlers. Prefer static only when the buffer is shared and budgeted, or heap only when failure is handled cleanly.

---

## Suggested Implementation Order
1. **#4** (library cleanup) — 5 minutes, zero firmware risk.
2. **#2** — standardize persistence error propagation and HTTP failure responses.
3. **#9** + **#8** — richer FTP response model and more specific status codes.
4. **#10** + **#7** + **#17** — small logic guards around backup request coalescing.
5. **Flash/RAM safety net** — implement steps 1–5 from "Flash Wear and Durability Strategy" (dirty-flag audit, flash write counter, RAM observability, memory budget table, hash-gate hot-tier snapshot) before broader changes that touch persistence.
6. **#3** — flash wear, after auditing dirty-flag coverage.
7. **#13** — keep watchdog coverage verified as FTP loops evolve.
8. **#15** — filesystem health helper to improve diagnostics across save/backup/restore.
9. **Hot-tier streaming** — reduce peak heap in snapshot save/load before increasing history retention or fleet limits.
10. **#12** — regression test to lock in the settings-save + backup behavior.
11. **#14** — backup manifest, especially before deeper restore validation.
12. **#6** + **#16** — restore-side hardening and boot safety.
13. **#1** — async backup state machine (largest change, do last).
14. **#5** — only if/when buffer ceiling becomes a real-world problem.
