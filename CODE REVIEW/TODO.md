# TankAlarm Master TODO List

> **Current Version:** 2.2.14 (review baseline `4f30a7f`, September 8, 2026)  
> **Last Updated:** September 15, 2026 (website PRs #314 → #315 → #316 → #317 merged into `master` as `1b516d9`, `2aaecf1`, `31a80bd`, `2e2c97c`; S-W01/02/03 done, S-W05/06/07 merged with residuals; see `CODE_REVIEW_09152026_WEBSITE_PRS_314_317_CLAUDE.md`)  
> **Purpose:** Comprehensive tracker for all unimplemented changes identified in code reviews and logic reviews. Update after every new review or commit.

---

## Table of Contents

- [Legend](#legend)
- [2026-09 Repository Review (v2.2.14)](#2026-09-repository-review-v2214)
- [Critical Bugs — Fix Immediately](#critical-bugs--fix-immediately)
- [High-Priority Issues](#high-priority-issues)
- [Moderate-Priority Issues](#moderate-priority-issues)
- [Minor / Cosmetic Issues](#minor--cosmetic-issues)
- [Common Header Centralization](#common-header-centralization)
- [Future Work — Planned Enhancements](#future-work--planned-enhancements)
- [Deferred / Dropped Items](#deferred--dropped-items)
- [Completed Items (Archive)](#completed-items-archive)
- [Review Sources](#review-sources)

---

## Legend

| Symbol | Meaning |
|--------|---------|
| `[ ]` | Not started |
| `[~]` | In progress |
| `[x]` | Completed |
| `[D]` | Deferred to future release |
| `[X]` | Dropped / Won't fix |
| **(S)** | Server component |
| **(C)** | Client component |
| **(V)** | Viewer component |
| **(A)** | All / Common |

---

## 2026-09 Repository Review (v2.2.14)

> Imported 2026-09-14 from `CODE_REVIEW_09092026_MASTER_REPOSITORY_REVIEW.md`, which consolidates the four 2026-09-08 reviews (FULL, COPILOT, INDEPENDENT, CLAUDE) against `master` at `4f30a7f` (firmware 2.2.14). IDs and priorities are the master review's; each row lists the source findings it consolidates, and the master review's Source Crosswalk maps every CLAUDE H-/M-/L- row. Status reflects `master` plus open pull requests where noted. P1 = physical/control correctness, missed or false alarms, lost durable state, or a security exposure; P2 = bounded defect or operator-facing error; P3 = documentation.

### Server — Website and Operator Workflows

- [x] **S-W01 (P1) (S)** Configuration Round-Trips Change Sensor and Alarm Meaning — _CLAUDE H-15, H-16, H-17_
  - **Done — PR #316 merged 2026-09-15 (`31a80bd`)**: `loadConfig()` restores the 4-20 mA subtype after the select is rebuilt (H-15), unchecks the threshold a config does not carry so one-sided and zero thresholds survive (H-16), and restores or derives the float-switch trigger (H-17). Browser round-trip of ultrasonic/pressure/digital sensors: no differences, second pass identical. The round-trip check is browser-driven, not a CI test.
- [x] **S-W02 (P1) (S)** Closure-Local Functions Make Six Controls Inoperative — _CLAUDE H-18, H-20, H-21, M-24; FULL/COPILOT F-19; INDEPENDENT R-16_
  - **Done — PR #315 merged 2026-09-15 (`2aaecf1`, three commits)**: the six controls carry their UID/key in HTML-escaped `data-*` attributes and are bound by delegated listeners inside the page IIFE (the old inline attributes spliced UIDs into JavaScript source), and `isValidClientUid()` restricts UIDs to letters, digits, `:`, `_`, `-`, `.` at ingestion and in `handleConfigPost`; `TankAlarm-112025-Server-BluesOpta/check_web_pages.py` (node --check per script block + inline-handler reachability, every call in an attribute) runs as the `check-web-pages` CI job and flagged exactly these six before the fix; after review it tokenises regex literals after keywords, matches any `on<event>` attribute, only counts `window.fn =` assignments that execute on load, fails closed on a missing node/sketch/page, carries a `--selftest`, and gates `build-firmware`. Also clamps Sample Minutes to 1092 and momentary relay durations to 65535 (client `uint16_t` limits, CLAUDE M-06/M-08). The dashboard and client-console controls (`requestUpdate`, `snoozeAlarm`, `clearRelays`, `deleteClient`, `approveDeletion`, the Edit Config links) got the same data-attribute treatment in the second commit; no page renders a client UID into JavaScript source any more. The third commit gives every page a quote-escaping `escapeHtml()` (six pages escaped only `& < >`), rejects the bare `dev:` prefix, and validates persisted UIDs (calibrations, history, FTP cache, config snapshots, metadata) when they load at boot.
- [x] **S-W03 (P1) (S)** Calibration Identity, Units, and Input Validation — _FULL/COPILOT F-03; INDEPENDENT R-07; CLAUDE H-19, M-60_
  - **Done — PR #316 merged 2026-09-15 (`31a80bd`)**: `parseSensorKey()` splits at the last colon with a 0–255 integer suffix at all three sites (submit used to POST `clientUid:"dev"` for a `dev:…:1` key); POST/DELETE `/api/calibration` use `is<uint8_t>()` / `is<float>()` presence checks so a verified level of 0 in is accepted (indexes are 1-based; 0 is rejected), require a finite non-negative level, and resolve the client/sensor pair before any work (404 otherwise). Review follow-ups (2026-09-14): blank calibration fields are no longer submitted as 0; an SMS Alerts checkbox makes `alarmSms` round-trip and `alarmsEnabled`/`digitalTrigger` open the alarm section on load.
- [ ] **S-W04 (P2) (S)** History Selection, Units, Offline Rendering, and Scope — _FULL/COPILOT F-19, F-20, F-22; INDEPENDENT R-16; CLAUDE M-24, M-25, M-56_
- [~] **S-W05 (P2) (S)** Responsive Layout, Contacts Grid, and Shared Styling — _FULL/COPILOT F-21; INDEPENDENT R-17; CLAUDE H-14, M-19, M-21, M-22, M-23, M-28, M-29, M-32, L-13, L-15, L-16, L-17, L-26, L-58_
  - **Merged — PR #314 (`1b516d9`, 2026-09-15)**: one header/nav on every page, missing `--card-bg`/`--accent`/`--chart-grid` tokens defined, rules for ~45 unstyled classes, toast hidden by opacity/visibility, wrapping tooltips, tables scroll inside cards on phones, contacts rendered as a real table, boxed checkbox fields matching input height, site-wide checkbox sizing, phone header as a scrolling strip (212px → 112px). Residual: ~60 inline status hex colours still bypass the new `--ok/--warn/--bad` tokens (CLAUDE L-26 partial); viewer stylesheet parity is tracked under V-W01.
- [~] **S-W06 (P2) (S)** Operator Status Can Be Misleading — _FULL/COPILOT F-23; INDEPENDENT R-24; CLAUDE M-30, M-31, M-41, L-25, L-51_
  - PR #314 renders the dashboard stale label from `STALE_MIN` (was a hard-coded ">25h").
  - **Merged — PR #317 (`2e2c97c`, 2026-09-15)**: float switches read ON/OFF on the dashboard and Site Config with no delta/sparkline and 0 in is a value (M-30); update requests expire after their window with a Retry Update button and polling drops back (M-31); `/api/clients` keeps the alarming sensor's `at` (M-41); Site Config maps `flt`/`ma` (L-25); the outdated count uses `compareFirmwareVersions()` (L-51); Client Console deletion confirms instead of prompting for a PIN. Residual: the 49 h stale policy itself is unchanged by design.
- [~] **S-W07 (P2) (S)** Logs, Save Feedback, and Secondary API Usability — _CLAUDE M-26, M-27, M-57, M-58, M-61, L-18, L-19, L-20, L-21, L-22, L-24, L-42, L-49; INDEPENDENT R-23/R-24_
  - PR #314 gives `/contacts` error toasts the shared `isError` colouring.
  - **Merged — PR #317 (`2e2c97c`, 2026-09-15)**: Transmission Log filters built from the emitted vocabulary and export sharing the table filter incl. detail search (M-26, M-27); `saved:false` shown as an error on Contacts and Server Settings (M-57); route alias / FTP user / FTP password rejected above 31 chars with `maxlength` on the inputs (M-58); `/api/location` URL-decodes `client=` (M-61); year-over-year key at the last colon (L-49); login shows the 429/400 reason (L-22/L-24); Config Generator commissioning path for an unknown UID, 8-sensor cap in page and `handleConfigPost`, no daily time from a guessed 05:00 UTC (L-19, L-20, L-21); single settings load (L-18). Residual: L-42 (SMS/email log entries without site/client, 47-char detail) is open.

### Server — Data Ingestion and History

- [ ] **S-D01 (P1) (S)** Registration Is Dropped Before Client Discovery — _CLAUDE H-25_
- [ ] **S-D02 (P1) (S)** Freshness, Quality, and Event Ordering Need One Contract — _FULL/COPILOT F-11, F-12, F-29; CLAUDE M-45, M-46, M-48, M-49, L-37, L-38, L-39; related INDEPENDENT R-13_
- [ ] **S-D03 (P1) (S)** Warm History and Archive Manifests Lose Data at Size Boundaries — _CLAUDE H-23, M-54, M-55, L-29_
- [ ] **S-D04 (P2) (S)** Retention, Monthly Completeness, and Peak Memory Are Overstated — _CLAUDE H-26, M-39, M-56; FULL/COPILOT F-20 and performance recommendations_
- [ ] **S-D05 (P2) (S)** Calibration and Weather History Are Not Reliable Reference Data — _CLAUDE M-37, M-38, M-59, L-50_

### Server — Alarms, Delivery, and Remote Commands

- [ ] **S-T01 (P1) (S)** Stable Relay Identity Requires an Explicit Protocol Migration — _FULL/COPILOT F-02; INDEPENDENT R-02; CLAUDE prior F-02/R-02_
- [ ] **S-T02 (P1) (S)** Notification Eligibility Must Belong to the Current Alarm Episode — _FULL/COPILOT F-09, F-15; INDEPENDENT R-10/R-13; CLAUDE M-51, M-52, L-45, L-47_
- [ ] **S-T03 (P1) (S)** Config Revision, ACK, and Retry Semantics Can Misreport Success — _FULL/COPILOT F-10; INDEPENDENT R-09; CLAUDE H-01, M-42, M-43_
- [ ] **S-T04 (P2) (S)** Note Consumption Needs Idempotency and Observable Failure — _FULL/COPILOT F-24; INDEPENDENT R-19; CLAUDE M-47, L-35, L-36_
- [ ] **S-T05 (P2) (S)** Contacts, Opt-Outs, and Delivery Metadata Need Consistent Rules — _CLAUDE M-50, M-53, L-40, L-41, L-42, L-43, L-44; earlier recipient/persistence recommendations_
- [ ] **S-T06 (P2) (S)** Dashboard Clear Relay targets the client monitor by dashboard array position — _Copilot review of #315 (2026-09-14)_
  - `renderDataCard` sends `t.sensorIdx`, the position of the sensor in `/api/clients` `ts[]` (registry order), and the client applies `relay_reset_sensor` as its configured monitor-array index, so a client whose sensors report as `[2, 1]` can clear the wrong monitor. Pre-existing (the inline handler did the same); #315 only moved the value into a data attribute. Fix: send the persistent sensor identity (`sensorIndex`, 1-based) and resolve it to the monitor on the client, or have the server translate it.

### Server — Persistence, Scheduling, and Maintenance

- [ ] **S-O01 (P1) (S)** Save Contracts Lose Empty State and Starve Metadata — _FULL/COPILOT F-08/F-14; INDEPENDENT R-05/R-12; CLAUDE M-35 and prior-item confirmations_
- [ ] **S-O02 (P1) (S)** Restore Is Partial, Capacity-Mismatched, and Not Coherently Activated — _FULL/COPILOT F-13; INDEPENDENT R-11; CLAUDE H-22, H-24, H-27_
- [ ] **S-O03 (P2) (S)** Cooperative Maintenance, Scheduling, and HTTP Budgets — _FULL/COPILOT F-07/F-17; INDEPENDENT R-08/R-20; CLAUDE M-34, M-40, M-44, L-27, L-30, L-31, L-32, L-33_
- [ ] **S-O04 (P2) (S)** Settings Transactions and Administrative Security — _FULL/COPILOT F-28; INDEPENDENT R-18; CLAUDE M-20, M-33, M-36, L-14, L-23, L-28, L-34_

### Client — Sensor Acquisition and Control

- [ ] **C-A01 (P1) (C)** Map Physical Outputs and Input Terminals Explicitly — _CLAUDE H-04, H-11, H-12_
- [ ] **C-A02 (P1) (C)** Unify Relay Ownership, Modes, and Reconfiguration — _CLAUDE H-02, H-05, H-07; related FULL/COPILOT F-05 and S-T01_
- [ ] **C-A03 (P1) (C)** Pulse Acquisition Must Observe the Whole Measurement Window — _FULL/COPILOT F-06; INDEPENDENT R-14; CLAUDE M-02 and prior-item evidence_
- [ ] **C-A04 (P2) (C)** Sensor Failure and Recovery Need Fresh, Sensor-Specific Evidence — _CLAUDE H-03, M-09, M-11, L-06; FULL/COPILOT F-12_
- [ ] **C-A05 (P2) (C)** Analog Scale, Acquisition Delays, and I2C Timeout Assumptions — _CLAUDE M-10, L-07, L-56_

### Client — Alarm Evaluation and Delivery

- [ ] **C-T01 (P1) (C)** Consecutive Debounce and Durable Notification-Pending State — _FULL/COPILOT F-04/F-05; INDEPENDENT R-03/R-04; CLAUDE H-06, M-12_
- [ ] **C-T02 (P1) (C)** Remote Commands Need Delivery and Freshness Guarantees — _CLAUDE H-13, M-15, M-16; S-T01_
- [ ] **C-T03 (P2) (C)** Replay Order and Capacity Must Match the Publisher — _FULL/COPILOT F-11/F-16; CLAUDE M-14; prior July reviews_

### Client — Configuration, Power, and OTA

- [ ] **C-P01 (P1) (C)** Numeric Configuration Must Be Validated Before Narrowing — _CLAUDE H-01, M-06, M-08, M-42, L-19, L-57; FULL/COPILOT F-10_
- [ ] **C-P02 (P1) (C)** Battery Alerts Can Repeat Without a Bounded Episode Policy — _CLAUDE H-08/H-09 (one issue), M-05, H-29, L-52_
- [ ] **C-P03 (P2) (C)** Power Transitions Must Use Fresh Voltage and Complete Side Effects — _FULL/COPILOT F-18; INDEPENDENT R-15; CLAUDE H-10, M-13, L-05_
- [ ] **C-P04 (P1) (C)** Solar State Is Stored on the Wrong Logical Volume — _CLAUDE H-28/M-62 (one issue)_
- [ ] **C-P05 (P2) (C)** Recovery, Health Polling, and Energy Budgets — _CLAUDE M-03, M-04, M-07, L-02/L-03, L-04, L-08, L-09, L-10; working-tree W-03_
- [ ] **C-P06 (P2) (C)** OTA Trial State, Health Confirmation, and Build Guarantees — _CLAUDE M-63, L-55; FULL/COPILOT W-04 and provisioning recommendations_
- [ ] **C-P07 (P1) (C)** Existing Working-Tree Regressions Are a Separate Release Gate — _FULL/COPILOT W-01..W-04; INDEPENDENT R-04/R-06; CLAUDE section 1_
  - Working tree on the review machine held byte-identical late-June copies of the client sketch, two Common headers, `TankAlarm_Solar.cpp` and the CI workflow; nothing was committed from it. See CLAUDE review section 1.

### Viewer

- [ ] **V-W01 (P2) (V)** Match Measurement Quality, Staleness, Units, and Status — _FULL/COPILOT F-12 and parity suggestions; CLAUDE L-46, L-58, L-59, L-62_
- [ ] **V-W02 (P2) (V)** Failed Contact Saves Leave Optimistic State Without Recovery — _CLAUDE L-60_
- [ ] **V-T01 (P1) (V)** Viewer Writes Cross the Administrative Trust Boundary — _CLAUDE H-31/H-32 (one issue); FULL website/security recommendations_
- [ ] **V-T02 (P2) (V)** Summary Schedules Can Stay Unarmed or Consistently Late — _INDEPENDENT R-08; CLAUDE M-65, M-66, L-61_
- [ ] **V-T03 (P2) (V)** Network Reconfiguration Needs Explicit Lifecycle and Recovery — _CLAUDE H-30, M-64_
- [ ] **V-T04 (P2) (V)** GitHub Update Metadata Uses the Wrong Response Shape — _CLAUDE M-67_

### Shared Platform and Repository Tooling

- [ ] **X-R01 (P2) (A)** Reproducible Builds and Regression Tests — _FULL/COPILOT F-27; CLAUDE M-01, M-17, L-01, L-12, L-53, L-63, L-64_
- [ ] **X-R02 (P3) (A)** Documentation, Published Artifacts, and Trust Model — _INDEPENDENT R-21/R-22; CLAUDE M-18, L-01, L-11, L-12, L-54; FULL documentation/security recommendations_
- [ ] **X-R03 (P2) (A)** Provisioning and Diagnostic Utilities — _FULL/COPILOT F-25/F-26; CLAUDE prior appendix_
- [ ] **X-R04 (open) (A)** Suggestions Requiring Measurement or Further Evidence — _CLAUDE section 9 and efficiency sections; other review recommendations_

---

## Critical Bugs — Fix Immediately

These items represent data loss, safety, or security risks.

### ~~C-5: UNTIL_CLEAR Relay Clear Unreachable **(C)** — SAFETY~~ ✅ FIXED
- [x] `sendAlarm()` now properly checks `cfg.relayMode == RELAY_MODE_UNTIL_CLEAR` and deactivates when the clearing alarm matches the trigger type. Logic corrected at Client line ~4591.
- **Source:** LOGICREVIEW-20260324 (multiple reviewers)
- **Verified:** March 24, 2026 — code inspection confirms fix in place.

### ~~C-6: Viewer DFU Enable Uses Wrong Notecard Request **(V)** — PROTOCOL~~ ✅ FIXED
- [x] `enableDfuMode()` now correctly sends `card.dfu` with `name:"stm32"` and `on:true` (matching Server pattern) instead of the status-check-only `dfu.status`. Function comment also corrected.
- **Source:** LOGICREVIEW-20260324
- **Fixed:** June 9, 2026 — Viewer `enableDfuMode()` updated.

### ~~C-2: SMS Rate Limiting Fails Open **(S)** — SECURITY/COST~~ ✅ FIXED
- [x] `checkSmsRateLimit()` now returns `false` (deny SMS) when time sync is unavailable (`now <= 0.0`). Prevents unbounded SMS flooding before clock synchronization.
- **Source:** LOGICREVIEW-20260324
- **Fixed:** June 9, 2026 — Server `checkSmsRateLimit()` updated.

### ~~C-3: Daily Email Shown as Local Time, Treated as UTC **(S)** — UX/LOGIC~~ ✅ FIXED
- [x] Web UI label now reads "Daily Email Time (HH:MM, UTC)" to clarify the timezone. Users are informed the time is UTC.
- **Source:** LOGICREVIEW-20260324
- **Fixed:** June 9, 2026 — Server settings HTML updated.

### ~~C-1: Stuck Sensor Detection False Positives **(C)**~~ ✅ FIXED
- [x] `stuckDetectionEnabled` now defaults to `false` (opt-in via config). Prevents false positive stuck-sensor alerts on idle tanks before explicit configuration.
- **Source:** LOGICREVIEW-20260324
- **Fixed:** June 9, 2026 — Client default config updated.

### ~~C-4: Viewer Ethernet Retry Near Watchdog Timeout **(V)** — WATCHDOG~~ ✅ FIXED
- [x] Ethernet retry `delay()` replaced with `safeSleep()` which feeds the watchdog in chunks. Prevents watchdog reset during 15-second retry delays.
- **Source:** LOGICREVIEW-20260324
- **Fixed:** June 9, 2026 — Viewer `initializeEthernet()` updated.

### ~~H4: Non-Atomic File Writes **(S)(C)(V)** — DATA LOSS~~ ✅ FIXED
- [x] All core save functions and archive manifest now use atomic writes. Server `archived_clients.json` updated to write-to-temp-then-rename pattern.
- **Source:** H4_ATOMIC_WRITE_REFACTOR_RECOMMENDATION.md, CODE_REVIEW_FIX_IMPLEMENTATION_02202026.md
- **Fixed:** June 9, 2026 — archive manifest write now atomic.

### ~~C-7: UNTIL_CLEAR Relay Not Re-Latched After Reboot **(C)** — SAFETY~~ ✅ FIXED
- [x] The first valid post-boot alarm sample now restores `UNTIL_CLEAR` and `MANUAL_RESET` relay outputs immediately, without waiting through the debounce window. This closes the extended post-reboot relay outage path.
- **Source:** CODE_REVIEW_04132026_COMPREHENSIVE.md (CR-H5), LOGIC_REVIEW_04132026.md (LR-5)
- **Fixed:** April 13, 2026 — Client `evaluateAlarms()` now restores persistent relays on the first qualifying sample.

---

## High-Priority Issues

### ~~I-10: Delete-Before-Accept Message Pattern **(A)** — ERROR HANDLING~~ ✅ FIXED
- [x] All three components now use two-step get-then-delete: `note.get` without `delete:true` (peek), process body, then separate `note.get` with `delete:true` to consume. If the device crashes during processing, the note survives for retry on next boot.
- **Source:** LOGICREVIEW-20260324
- **Fixed:** June 9, 2026 — Client `handleInboundConfig`, Server `processNotefile`, Viewer summary fetch all updated.

### ~~I-2: Viewer Summary Fetch Bootstrap Deadlock **(V)**~~ ✅ FIXED
- [x] `setup()` now calls `ensureTimeSync()` before `scheduleNextSummaryFetch()`, ensuring `gNextSummaryFetchEpoch` is set to a positive value. The bootstrap deadlock no longer occurs.
- **Source:** LOGICREVIEW-20260324
- **Verified:** March 24, 2026 — fix confirmed at Viewer lines ~246-250.

### I-5: Current-Loop Failures Return Previous Value **(C)** — DATA INTEGRITY — BY DESIGN
- [X] `readCurrentLoopSensor()` returns previous value on I2C failure. This is intentional: the stuck sensor detection system catches persistent failures (N identical readings → stuck alert). Single transient I2C failures are smoothed over, which is appropriate for slowly-changing tank levels. The `consecutiveFailures` counter and `communicationOk` flag provide visibility.
- **Source:** LOGICREVIEW-20260324
- **Reclassified:** June 9, 2026 — accepted as design choice; stuck detection is the backstop.

### ~~I-4: Solar Comm Failures Silently Suppressed **(C)**~~ ✅ CLARIFIED
- [x] `SolarManager::poll()` return value documented: returns `false` for both "not due" and "read failed". Callers should check `isCommunicationOk()` for failure state. The `consecutiveErrors` counter and `communicationOk` flag already surface failures internally and trigger alerts after threshold.
- **Source:** LOGICREVIEW-20260324
- **Fixed:** June 9, 2026 — added clarifying comments to `poll()` return paths.

### ~~I-3: Viewer Never Validates Schema Version **(V)** — FORWARD COMPAT~~ ✅ FIXED
- [x] `handleViewerSummary()` now checks the `_sv` field and logs a warning if the schema version is not 1. Future schema changes will produce visible warnings.
- **Source:** LOGICREVIEW-20260324
- **Fixed:** June 9, 2026 — Viewer `handleViewerSummary()` updated.

### ~~I-7: Server Overrides Per-Monitor SMS Intent **(S)(C)**~~ ✅ FIXED
- [x] SMS dispatch now requires BOTH client intent (`doc["se"]` flag) AND server policy (`smsOnHigh`/`smsOnLow`/`smsOnClear`). Previously server force-set `smsEnabled=true` for all sensor alarms, ignoring the client's per-monitor SMS preference.
- **Source:** LOGICREVIEW-20260324, CODE_REVIEW_04022026_COMPREHENSIVE.md (HIGH-7)
- **Fixed:** April 2, 2026 — Server `handleAlarm()` rewritten to use `clientWantsSms && smsAllowedByServer`.

### ~~I-12: Client Alarm Hourly Rate Limit Unsigned Underflow **(C)** — RATE LIMITING~~ ✅ FIXED
- [x] `checkAlarmRateLimit()`: Hourly pruning now guarded with `if (now >= 3600000UL)` for both per-monitor and global alarm budgets. When uptime < 1 hour, pruning is skipped (all timestamps are inherently recent).
- **Source:** CODE_REVIEW_04092026_COMPREHENSIVE.md (CR-H1)
- **Fixed:** April 9, 2026 — v1.6.2

### ~~I-13: `relay_timeout` Shares Rate Bucket With `sensor-fault` **(C)** — SAFETY~~ ✅ FIXED
- [x] Added explicit `relay_timeout` case in `checkAlarmRateLimit()` that returns `true` (always allowed). Safety timeout events are never rate-limited.
- **Source:** CODE_REVIEW_04092026_COMPREHENSIVE.md (CR-H2)
- **Fixed:** April 9, 2026 — v1.6.2

### ~~I-14: System Alarm SMS Not Rate-Limited **(S)** — COST/OPERATIONS~~ ✅ FIXED
- [x] Added `sLastSystemSmsSentEpoch` tracker to system alarm branch in `handleAlarm()`. Enforces `MIN_SMS_ALERT_INTERVAL_SECONDS` before sending system SMS.
- **Source:** CODE_REVIEW_04092026_COMPREHENSIVE.md (CR-H3), LOGIC_REVIEW_04092026.md (LR-4)
- **Fixed:** April 9, 2026 — v1.6.2

### ~~I-15: Server Settings POST Silently Ignores `false` / `0` Values **(S)** — CONFIG INTEGRITY~~ ✅ FIXED
- [x] `handleConfigPost()` now uses explicit field-presence checks for mutable numeric and boolean settings, so `false`, `0`, and midnight/top-of-hour values persist correctly instead of being treated as absent.
- **Source:** CODE_REVIEW_04132026_COPILOT.md (CR-M1), LOGIC_REVIEW_04132026_COPILOT.md (LR-M1)
- **Fixed:** April 13, 2026 — Server config POST path now honors explicit `false` / `0` payloads.

### ~~I-16: Sensor Alarm SMS Client-Intent Gate Regressed **(S)(C)** — NOTIFICATION POLICY~~ ✅ FIXED
- [x] Sensor alarm SMS now requires both the client-side intent flag and the server-side policy flag. The Server no longer force-enables SMS for sensor alarm categories.
- **Source:** CODE_REVIEW_04132026_COPILOT.md (CR-H2), LOGIC_REVIEW_04132026_COPILOT.md (LR-H1)
- **Fixed:** April 13, 2026 — Server `handleAlarm()` restored the dual-gate policy correctly.

### ~~I-17: Server Consumes Inbound Notes Even on Parse Failure **(S)** — DATA LOSS~~ ✅ FIXED
- [x] `processNotefile()` now peeks notes first, deletes only after successful parse/processing, and drops repeated poison notes after a bounded retry count instead of losing good data on first parse failure.
- **Source:** CODE_REVIEW_04132026_COPILOT.md (CR-H1), LOGIC_REVIEW_04132026_COPILOT.md (LR-C1)
- **Fixed:** April 13, 2026 — Server inbound note handling now uses delete-after-success semantics with poison-note cleanup.

### ~~I-18: Negative Hysteresis Value Not Validated **(C)** — INPUT VALIDATION~~ ✅ FIXED
- [x] Inbound monitor configs now clamp negative hysteresis to `0.0f`, preventing inverted clear thresholds and stuck alarm states from malformed payloads.
- **Source:** CODE_REVIEW_04132026_COMPREHENSIVE.md (CR-H1)
- **Fixed:** April 13, 2026 — Client config ingestion now floors hysteresis at zero.

### ~~I-19: FTP PASV Parser Corrupted by Status Line Digits **(S)** — CONNECTIVITY~~ ✅ FIXED
- [x] `ftpEnterPassive()` now starts parsing after the `(` in the FTP `227` response, so the passive IP and data port are assembled from the actual PASV tuple instead of the status code digits.
- **Source:** CODE_REVIEW_04132026_COMPREHENSIVE.md (CR-H2), LOGIC_REVIEW_04132026.md (LR-17)
- **Fixed:** April 13, 2026 — Server PASV parsing corrected.

### ~~I-20: FTP Credentials Stored and Transmitted in Plaintext **(S)** — SECURITY~~ ⚠ PARTIALLY FIXED / DEFERRED
- [D] FTP credentials are now obfuscated before being written to `server_config.json`, and legacy plaintext config files are auto-migrated on the next save. FTP control-channel transport is still plaintext on the wire because the current Ethernet/FTP stack does not yet implement FTPS.
- [D] The current secure-transport plan is **Explicit TLS FTPS only** for the Server backup/archive path. Implicit TLS is not part of the current implementation plan.
- [D] The wire-transport fix is tracked as **F-1 through F-13** under [FTPS Integration](#ftps-integration--migrate-server-ftp-to-explicit-ftps-resolves-i-20) in Future Work. The ArduinoOPTA-FTPS library now provides all required primitives (`connect`, `store`, `retrieve`, `mkd`, `size`, `quit`).
- **Source:** CODE_REVIEW_04132026_COMPREHENSIVE.md (CR-H4), FTPS_LIBRARY_INTEGRATION_STUDY_04152026.md
- **Fixed/Deferred:** April 13, 2026 — at-rest plaintext removed; secure transport upgrade tracked as F-1–F-13.

### ~~I-21: CRITICAL_HIBERNATE De-Energizes Relays Without Warning **(C)** — SAFETY~~ ✅ FIXED
- [x] The Client now publishes the power-state transition before de-energizing relays on entry to `CRITICAL_HIBERNATE`, so the Server receives an immediate critical-power event before pump/relay shutdown.
- **Source:** LOGIC_REVIEW_04132026.md (LR-10)
- **Fixed:** April 13, 2026 — `updatePowerState()` now sends the transition note before relay cutoff.

### ~~I-22: Sensor Registry Growth Not Bounded by Active Sensors **(S)** — PERFORMANCE~~
- [X] Current registry growth is already bounded by `MAX_SENSOR_RECORDS`, stale/orphan pruning, and stalest-record eviction. The intermittent-client scenario is operationally undesirable, but it is not a separate open defect worth tracking in the current implementation.
- **Source:** LOGIC_REVIEW_04132026.md (LR-13)
- **Validated:** April 13, 2026 - dropped after live-code audit.

### ~~I-23: Session Token Stored in localStorage — XSS-Accessible **(S)** — SECURITY~~ ✅ FIXED
- [x] The Server now sets the real session token in an `HttpOnly` cookie and request parsing prefers the cookie-backed session. The browser only retains a non-secret placeholder value for compatibility with existing page scripts.
- **Source:** LOGIC_REVIEW_04132026.md (LR-20)
- **Fixed:** April 13, 2026 — login/logout and request auth flow moved to cookie-backed sessions.

### ~~I-24: `applyConfigUpdate()` Ignores `solarCharger` Block **(C)** — CONFIG INTEGRITY~~ ✅ FIXED
- [x] `applyConfigUpdate()` now parses `doc["solarCharger"]` and re-initializes the `SolarManager` when `enabled`/`slaveId`/`baudRate`/`timeoutMs` change. Server-pushed solar charger settings now take effect at runtime instead of being silently dropped and overwritten on the next `saveConfigToFlash()`.
- **Source:** Self-audit during v1.6.12 bug review (April 23, 2026)
- **Fixed:** April 23, 2026 — v1.6.13.

### ~~I-25: `applyConfigUpdate()` Has No Save Path for `vinMonitor` **(C)** — DURABILITY~~ ✅ FIXED
- [x] `saveConfigToFlash()` now serializes the `vinMonitor` block (`enabled`/`pin`/`r1Kohm`/`r2Kohm`/`pollIntervalSec`/`includeInDaily`) so server-pushed Vin-divider config survives a reboot in offline-only deployments.
- **Source:** Self-audit during v1.6.12 bug review (April 23, 2026)
- **Fixed:** April 23, 2026 — v1.6.13.

### ~~I-26: `applyConfigUpdate()` Has No Save Path for `solarOnlyConfig` **(C)** — DURABILITY~~ ✅ FIXED
- [x] `saveConfigToFlash()` now serializes the `solarOnlyConfig` block so solar-only deployments retain their startup-debounce / sunset thresholds across a reboot when the server isn't around to re-push.
- **Source:** Self-audit during v1.6.12 bug review (April 23, 2026)
- **Fixed:** April 23, 2026 — v1.6.13.

### ~~I-6: Config Retry State Inconsistency **(S)** — STATE MACHINE~~ ✅ FIXED
- [x] Manual retry now keeps `pendingDispatch=true` and resets `dispatchAttempts=1` on successful Notecard send. ACK from client clears the pending flag (consistent with auto-retry behavior).
- **Source:** LOGICREVIEW-20260324
- **Fixed:** April 9, 2026 — v1.6.2 (also fixes M-10)

### ~~I-11: Config ACK Precedes Persistence Outcome **(C)** — DURABILITY~~ ✅ FIXED
- [x] Config ACK is now deferred until `persistConfigIfDirty()` completes. On success: ACK sent with "Config applied and persisted". On failure: ACK sent with "Flash persistence failed". Pending version tracked via `gPendingConfigAck` / `gPendingConfigAckVersion`.
- **Source:** LOGIC_REVIEW_04092026_COPILOT.md, CODE_REVIEW_04092026_COPILOT.md, LOGICREVIEW-20260324-1-GitHubCopilot-GPT5.3-Codex-v2.md
- **Fixed:** April 9, 2026 — v1.6.2

### ~~I-8: Missing Sensor Metadata in Alarms **(C)(S)**~~ ✅ FIXED
- [x] Alarm payloads now include object type (`ot`), measurement unit (`mu`), and sensor interface type (`st`) fields. Added at Client lines ~4330-4350.
- **Source:** LOGICREVIEW-20260324
- **Verified:** March 24, 2026 — all three metadata fields present in `sendAlarm()`.

### ~~I-9: Unbounded Viewer Cadence Inputs **(V)** — INPUT VALIDATION~~ ✅ FIXED
- [x] `refreshSeconds` clamped to 3600–86400 (1–24 hours). `baseHour` clamped to 0–23 (defaults to 6 if out of range). Prevents pathological fetch frequencies from malformed payloads.
- **Source:** LOGICREVIEW-20260324
- **Fixed:** June 9, 2026 — Viewer `handleViewerSummary()` updated with bounds clamping.

### ~~I2C Bus Recovery Missing **(C)(V)**~~ ✅ FIXED
- [x] `TankAlarm_I2C.h` now provides `tankalarm_recoverI2CBus()` with Wire.end(), 16x SCL-toggle, STOP condition, and Wire.begin() reinit. Used by Client (line ~1490), Server (line ~2703), and Viewer (line ~314) with watchdog integration.
- **Source:** I2C_COMPREHENSIVE_ANALYSIS_02262026.md
- **Verified:** March 24, 2026 — TankAlarm_I2C.h lines ~69-118.

### ~~V-1: Tank Array Overflow **(V)**~~ ✅ FIXED
- [x] Bounds check added: `if (gSensorRecordCount >= MAX_SENSOR_RECORDS) break;` at Viewer line ~856. `MAX_SENSOR_RECORDS` = 64.
- **Source:** CODE_REVIEW_03122026_COMPREHENSIVE.md
- **Verified:** March 24, 2026.

### ~~DEFAULT_PRODUCT_UID Collision **(C)**~~ ✅ FIXED
- [x] Client now uses conditional `#include "ClientConfig.h"` before the `#ifndef DEFAULT_PRODUCT_UID` guard, allowing per-deployment override. ClientConfig.h defines the UID first; fallback only applies if not already defined.
- **Source:** COMMON_HEADER_AUDIT_02192026.md (C5)
- **Verified:** March 24, 2026 — Client lines ~104-109.

### ~~Config ACK Notefile Routing Mismatch **(C)(S)**~~ ✅ FIXED
- [x] Routing now uses separate notefile pair: Client sends ACKs on `config_ack.qo` (CONFIG_ACK_OUTBOX_FILE), Server reads from `config_ack.qi` (CONFIG_ACK_INBOX_FILE). Both constants centralized in TankAlarm_Common.h lines ~139-144.
- **Source:** COMMON_HEADER_AUDIT_02192026.md (D1)
- **Verified:** March 24, 2026.

---

## Moderate-Priority Issues

### ~~M-12: Stuck Detection Interferes With Unload Tracking **(C)**~~ ✅ FIXED
- [x] Stuck detection now skips when `cfg.trackUnloads && state.unloadTracking` is true. Slow-emptying tanks no longer trigger false `sensor-stuck` alarms during active unloads.
- **Source:** CODE_REVIEW_04092026_COMPREHENSIVE.md (CR-M1), LOGIC_REVIEW_04092026.md (LR-3)
- **Fixed:** April 9, 2026 — v1.6.2

### ~~M-13: First Alarm After Boot Suppressed by Rate Limiter **(C)**~~ ✅ FIXED
- [x] Per-type alarm timestamps now initialized to `millis() - (MIN_ALARM_INTERVAL_SECONDS * 1000UL + 1)` at boot, so the first alarm is not suppressed by the minimum-interval check.
- **Source:** CODE_REVIEW_04092026_COMPREHENSIVE.md (CR-M2)
- **Fixed:** April 9, 2026 — v1.6.2

### ~~M-14: Viewer `respondJson()` Double-Buffers Entire JSON on Heap **(V)**~~ ✅ FIXED
- [x] `sendSensorJson()` now uses `measureJson(doc)` for Content-Length, then streams directly to the EthernetClient via `serializeJson(doc, client)`. No intermediate `String` allocation.
- **Source:** CODE_REVIEW_04092026_COMPREHENSIVE.md (CR-H4)
- **Fixed:** April 9, 2026 — v1.6.2

### ~~M-15: Viewer `readHttpRequest()` No Header Count Limit **(V)** — HARDENING~~ ✅ FIXED
- [x] Added `headerCount` counter in `readHttpRequest()`. Requests with more than 32 headers are rejected (`return false`).
- **Source:** CODE_REVIEW_04092026_COMPREHENSIVE.md (CR-M7)
- **Fixed:** April 9, 2026 — v1.6.2

### ~~M-16: SMS Message Truncation Silently Drops Alarm Info **(S)**~~ ✅ FIXED
- [x] Site name pre-truncated to 24 chars (`shortSite[24]`) before SMS construction in `handleAlarm()`. Ensures alarm type, sensor number, and level always fit within 160-byte SMS.
- **Source:** CODE_REVIEW_04092026_COMPREHENSIVE.md (CR-M8)
- **Fixed:** April 9, 2026 — v1.6.2

### M-17: Delayed Relay Commands May Actuate After Alarm Clears **(C)(S)** — ARCHITECTURE
- [D] Cross-device relay commands traverse 3 Notecard hops (~2-4 min). A relay ON command delayed past the alarm clear results in a relay activating without an active alarm. The relay safety timeout is the backstop.
- **Source:** LOGIC_REVIEW_04092026.md (LR-5)
- **Fix (deferred):** Add epoch timestamp to relay commands, validate freshness on receiving Client. Requires protocol change.

### ~~M-5: No "Client Recovered" SMS Alert **(S)**~~ ✅ FIXED
- [x] `checkStaleClients()` now sends a recovery SMS when `staleAlertSent` transitions from true to false: "Client recovered: {site} ({uid}) - reporting again."
- **Source:** LOGICREVIEW-20260324
- **Fixed:** April 9, 2026 — v1.6.2

### ~~M-7: Sensor-Recovered Alarms Blocked from SMS **(S)**~~ ✅ FIXED
- [x] `sensor-recovered` is now routed to SMS with explicit `isRecovery` flag handling at Server lines ~8347-8348.
- **Source:** LOGICREVIEW-20260324
- **Verified:** March 24, 2026.

### ~~M-2: Sensor Recovery Not Rate-Limited **(C)**~~ ✅ FIXED
- [x] Sensor recovery notifications now go through `checkAlarmRateLimit()` instead of bypassing rate limits. Both the normal recovery path and the button-clear recovery path are rate-limited.
- **Source:** LOGICREVIEW-20260324
- **Fixed:** June 9, 2026 — Client recovery paths updated.

### ~~M-3: Sensor Recovery Instantaneous (No Debounce) **(C)**~~ ✅ FIXED
- [x] Recovery now requires 3 consecutive valid readings before declaring sensor recovered (`recoveryCount` field added to `MonitorRuntime`). Prevents flaky sensors from rapidly toggling between failed/recovered states.
- **Source:** LOGICREVIEW-20260324
- **Fixed:** June 9, 2026 — Client `validateReading()` updated with debounce counter.

### M-4: I2C Thresholds Loop-Count-Based **(C)** — PARTIALLY FIXED
- [~] Still uses loop counts (not elapsed time), but now has exponential backoff multiplier (`sensorRecoveryBackoff`) at Client line ~1532. Variance between NORMAL and CRITICAL power states remains (~3,000x).
- **Source:** LOGICREVIEW-20260324, I2C_COMPREHENSIVE_ANALYSIS_02262026.md
- **Fix:** Use elapsed-time thresholds instead of loop counts.
- **Verified:** March 24, 2026 — loop-count-based with backoff.

### ~~M-6: Power Thresholds Accepted Without Validation **(C)**~~ ✅ FIXED
- [x] After receiving power threshold config, hysteresis ordering is validated: exit voltage must be greater than enter voltage for each tier (eco, low, critical). If violated, exit voltage is corrected to enter + 0.2V with a warning logged.
- **Source:** LOGICREVIEW-20260324
- **Fixed:** June 9, 2026 — Client `handleInboundConfig()` updated with cross-validation.

### ~~M-1: Current-Loop No Multi-Sample Averaging **(C)**~~ ✅ FIXED
- [x] `readCurrentLoopSensor()` now uses 4-sample averaging with 5ms inter-sample delay. Invalid (negative) samples are discarded. Falls back to previous reading if all samples fail.
- **Source:** LOGICREVIEW-20260324
- **Fixed:** April 9, 2026 — v1.6.2

### ~~M-8: Calibration Singular Matrix Detection Weak **(S)**~~ — ACCEPTABLE
- [x] Uses `fabs(det) < 0.0001f` threshold with fallback to simple regression at Server line ~12376. While the absolute threshold could miss some ill-conditioned matrices, the fallback path provides safety. Acceptable for current use case.
- **Source:** LOGICREVIEW-20260324
- **Verified:** March 24, 2026.

### ~~M-9: 24h Change Tracking Frozen at High Frequency **(S)**~~ ✅ FIXED
- [x] 24h change now uses linear interpolation between the two snapshots bracketing the 24h-ago mark, instead of the first snapshot inside the window. Much more accurate when snapshot intervals are wide.
- **Source:** LOGICREVIEW-20260324
- **Fixed:** April 9, 2026 — v1.6.2

### ~~M-10: Manual Config Retry Inconsistency **(S)**~~ ✅ FIXED
- [x] Unified with I-6 fix. Manual retry now keeps `pendingDispatch=true` and resets `dispatchAttempts=1`. ACK clears pending flag.
- **Source:** LOGICREVIEW-20260324
- **Fixed:** April 9, 2026 — v1.6.2 (merged with I-6 fix)

### ~~M-11: Config Generator Hides Dispatch / ACK Status **(S)** — OPERATIONS~~ ✅ FIXED
- [x] `GET /api/client?uid=` now returns `dispatch` object with: `pending`, `attempts`, `lastDispatchEpoch`, `lastAckEpoch`, `lastAckStatus`, `configVersion`. Config Generator JS can now populate `#configStatus`.
- **Source:** CODE_REVIEW_04092026_COPILOT.md, LOGICREVIEW-20260402-GitHubCopilot-GPT54.md
- **Fixed:** April 9, 2026 — v1.6.2 (API-side only; JS rendering is a follow-up)

### ~~Config Buffer Mismatch **(S)**~~ ✅ FIXED
- [x] Buffer sizes aligned: payload buffer 1536 bytes (line ~880), dispatch uses 8192-byte static buffer (line ~7851). No mismatch risk.
- **Source:** CODE_REVIEW_03122026_COMPREHENSIVE.md (S-4)
- **Verified:** March 24, 2026.

### ~~Server `respondHtml()` Triple-Copy Memory Pattern **(S)**~~ ✅ FIXED
- [x] Now uses streaming with `sendStringRange()` at Server lines ~6449-6510. Calculates `Content-Length` upfront, then writes chunks instead of building a massive String in memory.
- **Source:** CODE_REVIEW_RECOMMENDATIONS_02262026.md
- **Verified:** March 24, 2026.

### JSON API Builders Using String Concatenation **(S)** — PARTIALLY FIXED / DEFERRED
- [D] Uses `+=` incremental concatenation (Server lines ~13228-13232) instead of single massive concat, reducing peak memory. However, still uses `String` class rather than streaming JSON. Heap fragmentation risk remains but is reduced. Full migration to ArduinoJson deferred — large refactor with low urgency since inputs are from controlled sources.
- **Source:** CODE_REVIEW_RECOMMENDATIONS_02262026.md, CODE_REVIEW_04022026_COMPREHENSIVE.md (MED-13)
- **Verified:** March 24, 2026. Deferred April 2, 2026.

### ~~M-18: Login Redirect Parameter Not Sanitized **(S)** — SECURITY~~ ✅ FIXED
- [x] The login page now sanitizes `redirect` targets and accepts only safe in-app relative paths. Absolute URLs, protocol-relative targets, and non-app schemes are rejected.
- **Source:** CODE_REVIEW_04132026_COPILOT.md (CR-H3), LOGIC_REVIEW_04132026_COPILOT.md (LR-H2)
- **Fixed:** April 13, 2026 — login redirect flow hardened.

### ~~M-19: Narrow Hysteresis Causes Alarm Oscillation and SMS Flood **(C)(S)** — OPERATIONS~~ ✅ DOCUMENTED
- [x] The Config Generator UI now adds analog-sensor guidance recommending at least 1.0 inch hysteresis and noting that generated configs default to 2.0 inches.
- **Source:** LOGIC_REVIEW_04132026.md (LR-1)
- **Fixed:** April 13, 2026 — Config Generator now documents safe hysteresis guidance for analog sensors.

### ~~M-20: Clear/Recovery SMS Rate-Limited May Delay Operator Notice **(S)** — NOTIFICATION~~ ✅ FIXED
- [x] Clear and recovery SMS now bypass the minimum-interval suppression path while leaving normal alarm rate limiting intact, so operators receive the recovery notification promptly.
- **Source:** LOGIC_REVIEW_04132026.md (LR-3)
- **Fixed:** April 13, 2026 — Server SMS rate limiting now exempts clear/recovery notices.

### ~~M-21: Relay Momentary Timeout Imprecision During Blocking I/O **(C)** — PRECISION~~ ✅ DOCUMENTED
- [x] The Config Generator UI now explains that momentary relays turn off on the next main-loop pass after expiry and advises adding a safety margin when tight pulse width matters.
- **Source:** LOGIC_REVIEW_04132026.md (LR-7)
- **Fixed:** April 13, 2026 — Config Generator now documents momentary timing precision limits.

### ~~M-22: Config Dispatch Timeout Missing **(S)** — STATE MANAGEMENT~~
- [X] The Server already auto-cancels pending config dispatch after `MAX_CONFIG_DISPATCH_RETRIES` (currently 5). This is not an indefinite pending state in the current code.
- **Source:** LOGIC_REVIEW_04132026.md (LR-8)
- **Validated:** April 13, 2026 - dropped after live-code audit.

### ~~M-23: Daily Summary Deduplication Uses Exact Epoch **(S)** — DATA INTEGRITY~~
- [X] The claimed exact-epoch duplicate path was not verified in the current daily report flow. The live code groups report parts within a 30-minute batch window, and the warm-tier daily rollup already deduplicates by date.
- **Source:** LOGIC_REVIEW_04132026.md (LR-14)
- **Validated:** April 13, 2026 - dropped after live-code audit.

### ~~M-24: Viewer Poison Note Counter Resets Across Reboots **(V)** — RELIABILITY~~ ✅ FIXED
- [x] The Viewer now deletes malformed summary notes on the first parse/OOM failure instead of relying on an in-RAM failure counter that resets after reboot.
- **Source:** LOGIC_REVIEW_04132026.md (LR-16)
- **Fixed:** April 13, 2026 — Viewer summary fetch now drops poison notes immediately.

### ~~M-25: Config ACK Not Retried on Notecard Failure **(C)** — RELIABILITY~~ ✅ FIXED
- [x] Pending config ACK state is now retained until a successful send, and failed ACK transmissions retry from later loops with bounded backoff instead of being dropped permanently.
- **Source:** CODE_REVIEW_04132026_COMPREHENSIVE.md (CR-M1)
- **Fixed:** April 13, 2026 — Client config ACK flow now retries transient Notecard failures.

### ~~M-26: Viewer Responses Omit `Connection: close` **(V)** — HTTP COMPLIANCE~~ ✅ FIXED
- [x] Core Viewer responses now emit `Connection: close`, matching the existing deterministic socket-close behavior and preventing compliant clients from waiting on persistent HTTP/1.1 connections.
- **Source:** CODE_REVIEW_04132026_COMPREHENSIVE.md (CR-M6)
- **Fixed:** April 13, 2026 — Viewer response helpers updated.

### ~~M-27: Server Settings API Returns FTP Password **(S)** — SECURITY~~
- [X] The live `/api/server-settings` response already returns only `ftp.pset` to indicate whether a password is configured. The plaintext `ftpPass` path observed during audit is the config-file save path, which remains separately tracked as `I-20` (at-rest plaintext credentials).
- **Source:** CODE_REVIEW_04132026_COMPREHENSIVE.md (CR-M7)
- **Validated:** April 13, 2026 — dropped after re-checking the actual settings API payload.

---

## Minor / Cosmetic Issues

### m-11: `linearMap()` Not in Common Headers **(C)**
- [ ] `linearMap()` is defined only in the Client sketch. If Server or Viewer ever need inline sensor conversion, function must be duplicated. Consider moving to `TankAlarm_Utils.h`.
- **Source:** CODE_REVIEW_04092026_COMPREHENSIVE.md (CR-L1)

### m-12: `posix_read_file()` Silently Truncates Large Files **(S)**
- [ ] Files exceeding `bufSize - 1` are silently truncated with no error indication. Could corrupt JSON parsing for oversized `sensor_registry.json`.
- **Source:** CODE_REVIEW_04092026_COMPREHENSIVE.md (CR-L4)
- **Fix:** Return error code or log warning when truncation occurs.

### m-1: Stuck Tolerance Hard-Coded **(C)**
- [ ] `0.05` inches hard-coded threshold, not proportional to sensor range. High-resolution sensors (0–1 PSI) read within normal noise.
- **Source:** LOGICREVIEW-20260324

### m-5: No DFU Rollback on Crash-Loop **(C)**
- [ ] No boot-success flag to detect bad firmware. A bad DFU update causes permanent crash-loop.
- **Source:** LOGICREVIEW-20260324

### m-9: No Heartbeat in CRITICAL_HIBERNATE **(C)**
- [ ] Device sends zero telemetry in deepest sleep state. Server loses visibility entirely.
- **Source:** LOGICREVIEW-20260324

### ~~m-10: VinVoltage Not Rendered on Dashboard **(V)**~~ ✅ FIXED
- [x] Server dashboard and site/client views already render VIN voltage when it is present in telemetry metadata.
- **Source:** LOGICREVIEW-20260324
- **Verified:** April 9, 2026 — TODO entry was stale; current UI shows VIN voltage.

### ~~m-6: Negative Level Clamping Inconsistent **(S)**~~ ✅ FIXED
- [x] All fallback calculation paths in `convertMilliampsToLevel()` now clamp negative results to 0.0f. Previously only the calibrated path and one fallback had clamping.
- **Source:** LOGICREVIEW-20260324
- **Fixed:** June 9, 2026 — Server fallback returns updated.

### ~~m-7: FTP Archive Manifest Not Atomic **(S)**~~ ✅ FIXED
- [x] Archive manifest write now uses atomic write-to-temp-then-rename pattern (`/fs/archived_clients.json.tmp` → `/fs/archived_clients.json`). Part of H4 completion.
- **Source:** LOGICREVIEW-20260324
- **Fixed:** June 9, 2026 — merged with H4 fix.

### ~~m-8: Solar Alert Reports Stale Data **(C)**~~ ✅ FIXED
- [x] `SolarManager::checkAlerts()` now surfaces `SOLAR_ALERT_COMM_FAILURE` (or `NONE` if `alertOnCommFailure` is off) before any voltage/temp/fault branch when `_data.communicationOk` is false. Prevents stale `battery_critical`/`battery_low` alerts from firing on the last known voltage after the Modbus link drops mid-poll.
- **Source:** LOGICREVIEW-20260324
- **Fixed:** April 23, 2026 — v1.6.13 (`TankAlarm_Solar.cpp`).

### m-3: millis() Rollover at 49.7 Days **(C)**
- [ ] Rate limiter briefly fails at `millis()` rollover. Low risk; debounce catches on next cycle.
- **Source:** LOGICREVIEW-20260324

### m-2: Momentary Relay Duration Precedence Unclear **(C)**
- [ ] Multiple relays in mask with different durations: both deactivate at the minimum. Documented but unintuitive behavior.
- **Source:** LOGICREVIEW-20260324

### m-13: Config Changes Coalesce Without Audit Trail **(S)** — FORENSICS
- [ ] Rapid config changes between `persistConfig()` calls are coalesced — no log of which individual settings were changed. If a misconfiguration causes a missed alarm, forensic analysis cannot determine which change was responsible.
- **Source:** LOGIC_REVIEW_04132026.md (LR-9)
- **Fix:** Log each setting change (old → new value) to Serial output.

### m-14: Viewer Summary Fetch Misaligned After Large Time Correction **(V)** — TIMING
- [ ] After a large NTP or Notecard time correction (e.g., +24 hours), `gNextSummaryFetchEpoch` is not recalculated. May trigger an immediate out-of-schedule fetch.
- **Source:** LOGIC_REVIEW_04132026.md (LR-15)
- **Fix:** After each time sync, validate that `gNextSummaryFetchEpoch` is still in the future; if not, reschedule.

### ~~Dead `#ifndef` Redefinitions **(S)(C)**~~ ✅ FIXED
- [x] All deprecated local redefinitions have been removed. Server and Client now reference Common.h definitions directly.
- **Source:** COMMON_HEADER_AUDIT_02192026.md (E1)
- **Verified:** March 24, 2026.

---

## Common Header Centralization

Items from the COMMON_HEADER_AUDIT_02192026.md that need cleanup.

### HIGH
- [x] **(A1)** ~~`CONFIG_ACK_OUTBOX_FILE` + `CONFIG_ACK_INBOX_FILE`~~ ✅ Now centralized in TankAlarm_Common.h lines ~139-144.
- [x] **(C5)** ~~`DEFAULT_PRODUCT_UID` collision~~ ✅ Fixed via ClientConfig.h conditional include — see High-Priority Issues above.
- [x] **(D1)** ~~Config ACK notefile routing~~ ✅ Separate .qo/.qi pair centralized — see High-Priority Issues above.

### MEDIUM
- [~] **(C1)** `DEFAULT_SAMPLE_SECONDS` — Client uses alias `#define DEFAULT_SAMPLE_SECONDS DEFAULT_SAMPLE_INTERVAL_SEC` (line ~187). Functionally correct but maintains dual naming.
- [~] **(C2)** `DEFAULT_LEVEL_CHANGE_THRESHOLD_INCHES` — Same alias pattern: `#define DEFAULT_LEVEL_CHANGE_THRESHOLD_INCHES DEFAULT_LEVEL_CHANGE_THRESHOLD` (line ~188).
- [x] **(A2)** ~~`MAX_RELAYS`~~ ✅ Centralized in TankAlarm_Common.h lines ~58-59. Local definitions removed.
- [x] **(A3)** ~~`CLIENT_SERIAL_BUFFER_SIZE`~~ ✅ Centralized in TankAlarm_Common.h lines ~65-66. Local definitions removed.
- [x] **(A7)** ~~`VIEWER_SUMMARY_INTERVAL_SECONDS` / `VIEWER_SUMMARY_BASE_HOUR`~~ ✅ Now centralized in TankAlarm_Common.h. Server local definitions removed; Viewer local aliases now reference Common.h constants.

### LOW
- [x] **(E1)** ~~Dead `#ifndef` redefinitions~~ ✅ All removed.
- [ ] **(E2)** Platform defines (`POSIX_FS_PREFIX` etc.) — TankAlarm_Platform.h uses `TANKALARM_POSIX_FS_PREFIX`, but Server/Client maintain local `POSIX_FS_PREFIX`. Parallel naming conventions coexist.
- [ ] **(E3)** Debug macros — Client has full `#ifdef DEBUG_MODE` macros (lines ~85-95); Server/Viewer only use inline `#ifdef` without no-op macro stubs.
- [x] **(A4)** ~~`DFU_CHECK_INTERVAL_MS`~~ ✅ Centralized in TankAlarm_Common.h lines ~238-239. Redundant local definitions removed.
- [ ] **(A6)** `MAX_TANKS_PER_CLIENT` — still Client-local only, not in Common.h.

---

## Future Work — Planned Enhancements

### Tier 1 — Foundation (P1, prerequisite for later work)

- [x] **3.1.1 — Notecard `begin()` Idempotency Audit** ✅ COMPLETED  
  `tankalarm_ensureNotecardBinding()` wrapper created in TankAlarm_Notecard.h (line ~108). Used across 20+ callsites in all components.

- [x] **3.1.2 — Extract I2C Operations to Shared Header** ✅ COMPLETED  
  TankAlarm_I2C.h created (~300 lines) with `tankalarm_recoverI2CBus()`, `tankalarm_scanI2CBus()`, and `tankalarm_readCurrentLoopMilliamps()`. All 4 sketches now use shared functions.

- [x] **3.1.3 — Server/Viewer I2C Hardening** ✅ COMPLETED  
  Server and Viewer both use shared I2C recovery + bus scan with watchdog integration. Error counters and exponential backoff implemented.

### Tier 2 — Diagnostics & Reliability (P2)

- [ ] **3.2.1 — I2C Transaction Timing Telemetry** (4–6 hrs)  
  Instrument I2C calls to report min/avg/max timing data. Deferred pending field data collection.

- [x] **3.2.3 — Notecard Health Check Backoff** — COMPLETED
- [x] **3.2.4 — I2C Recovery Event Logging** — COMPLETED
- [x] **3.2.5 — Startup Scan in Health Telemetry** — COMPLETED

### Tier 3 — Advanced / Deferred to v2.0+

- [D] **3.3.1 — I2C Mutex Guard** — DROPPED (dual-core not planned)
- [D] **3.3.2 — I2C Address Conflict Detection** — DROPPED (superseded by 3.2.5)
- [D] **3.3.3 — Wire Library Timeout Configuration** — DEFERRED (pending 3.2.1 baseline data)
- [D] **3.3.4 — Flash-Backed I2C Log** — DROPPED (superseded by 3.2.4)
- [D] **3.3.5 — Opta Blueprint Disposition** — DEFERRED housekeeping
- [D] **3.3.6 — Integration Test Suite** — DEFERRED to v2.0 (16–40 hrs estimated)
- [D] **Notecard Optimization Templates** — DEFERRED (schemas still evolving)

### FTPS Integration — Migrate Server FTP to Explicit FTPS (resolves I-20)

> **Library:** [dorkmo/ArduinoOPTA-FTPS](https://github.com/dorkmo/ArduinoOPTA-FTPS) (CC0-1.0)  
> **Study:** FTPS_LIBRARY_INTEGRATION_STUDY_04152026.md  
> **Prerequisite:** Library must be validated on real Opta hardware against at least one reference server before server-side integration begins.

> **STATUS UPDATE — April 17, 2026:** FTPS transport is integrated and
> end-to-end multi-file backup is **verified live** against a pyftpdlib
> FTPS server: **8 files uploaded, 0 failed** in a single session
> (`proc=8 failed=0`, ~656 s total). Two integrator-side workarounds
> were required to live within Opta's hard LWIP socket-pool limit
> (`MBED_CONF_LWIP_SOCKET_MAX = 4`, baked into the precompiled
> `libmbed.a`) and the absence of `SO_LINGER` support
> (every close goes through ~60 s `TIME_WAIT`):
> 1. Release the listening web-server socket during backup
>    (`gWebServer.end()` before, `gWebServer.begin()` after).
> 2. Pace inter-file uploads with a ~65 s wait so the previous data
>    socket's `TIME_WAIT` drains before opening the next.
>
> **New follow-up documents (this folder):**
> - [MULTI_FILE_BACKUP_FOLLOWUPS_04172026.md](MULTI_FILE_BACKUP_FOLLOWUPS_04172026.md) — 7 ranked optimization opportunities.
> - [OPTA_LWIP_BACKUP_RECIPE_04172026.md](OPTA_LWIP_BACKUP_RECIPE_04172026.md) — integrator recipe / known-good pattern.
> - [PER_FILE_RETRY_PLAN_04172026.md](PER_FILE_RETRY_PLAN_04172026.md) — bounded per-file retry plan for `-3005` failures.
> - [NIGHTLY_BACKUP_PLAN_04172026.md](NIGHTLY_BACKUP_PLAN_04172026.md) — scheduled nightly backup plan.
>
> Items F-1 through F-13 below are landed in code but have not yet been
> individually re-audited and re-checked in this list. F-15 (pyftpdlib
> validation) is **complete**. F-16 (PR1400 NAS validation) is still
> outstanding.

#### Phase 1 — Config Schema & Persistence **(S)**
- [ ] **F-1: Add FTPS fields to `ServerConfig`** — `ftpsTrustMode` (fingerprint / imported-cert), `ftpsFingerprint[65]`, `ftpsTlsServerName[128]`, `ftpsRootCaPem[4097]`, `ftpsEnabled` (bool, opt-in gate to avoid breaking existing plain-FTP users).
- [ ] **F-2: Extend `server_config.json` schema** — Serialize/deserialize the new FTPS fields. Maintain backward compat: missing fields default to plain FTP (FTPS off).
- [ ] **F-3: Extend Notecard config sync** — Add shortened keys for FTPS fields in the Notecard serialization paths so config can be pushed/pulled remotely.

#### Phase 2 — Transport Replacement **(S)**
- [ ] **F-4: Add ArduinoOPTA-FTPS library dependency** — Add to `lib_deps` or local library checkout alongside existing dependencies.
- [ ] **F-5: Replace FTP helpers with `FtpsClient`** — Replace `ftpConnectAndLogin()`, `ftpStoreBuffer()`, `ftpRetrieveBuffer()`, `ftpQuit()`, `ftpEnterPassive()` with `FtpsClient::begin()` / `connect()` / `store()` / `retrieve()` / `quit()`. Use `mkd()` to create remote paths and `size()` for download preflight.
- [ ] **F-6: Remove redundant FTP helper code** — `ftpReadResponse()`, `ftpSendCommand()`, `ftpEnterPassive()`, `parsePasv()` can be removed once `FtpsClient` is the sole transport. Keep `FtpSession` struct or replace with `FtpsClient` instance.
- [ ] **F-7: Feed watchdog between `FtpsClient` calls** — The server backup loop iterates 9 files. Feed watchdog between each `store()` / `retrieve()` call. Individual transfers (max 24KB, 15s library timeout) should complete within a single watchdog window.

#### Phase 3 — Web UI & REST API **(S)**
- [ ] **F-8: Add FTPS settings to Web UI** — Add trust mode selector (Fingerprint / Imported Cert), fingerprint input, TLS server name input, PEM textarea, and FTPS enable toggle to the existing FTP settings form.
- [ ] **F-9: Update REST API endpoints** — Add FTPS fields to `GET /api/server-settings` response and `POST` handler. Respect existing `pset`-style password masking; do not expose fingerprint or PEM in full over the API unless saving.
- [ ] **F-10: Credential/cert obfuscation at rest** — Extend existing FTP credential obfuscation to cover the fingerprint and PEM fields in `server_config.json`.

#### Phase 4 — Validation & Cutover **(S)**
- [ ] **F-11: Dual-mode transition support** — Keep plain FTP as the default. FTPS activates only when `ftpsEnabled == true` and trust config is present. Allows field upgrade without breaking existing deployments.
- [ ] **F-12: Test against reference servers** — Validate against WD My Cloud PR4100, vsftpd, and FileZilla Server. Confirm backup (9 files), restore, client config manifests, and monthly archive flows all work over FTPS.
- [ ] **F-13: Error reporting integration** — Map `FtpsError` enum to existing FTP status UI (web dashboard, serial diagnostics). The library's `char* error` buffer provides human-readable messages at every call site.

#### Phase 5 — Execution Checklist (install + live validation) **(S)**
- [ ] **F-14: Deploy latest Server firmware to Opta** — Install the updated `TankAlarm-112025-Server-BluesOpta.ino` build on the operational server hardware and confirm clean boot, Ethernet link, Notecard online status, and web settings page access.
- [ ] **F-15: Validate against local Pyftpdlib FTPS test server**
  - [ ] Configure Server FTPS settings for local host, explicit TLS, fingerprint trust mode, and test path.
  - [ ] Run **Backup Now** and verify remote directory creation (base path, server UID folder, `clients/`, `history/` as needed).
  - [ ] Verify expected files transfer: baseline backup files, `clients_manifest.txt`, per-client config JSON files, and monthly archive JSON when triggered.
  - [ ] Run **Restore Now** and confirm restore path works without truncation/errors.
  - [ ] Capture serial/web diagnostics for pass/fail evidence.
- [ ] **F-16: Validate against operational PR1400 NAS FTPS server**
  - [ ] Manually configure NAS FTPS service, account permissions, and target path for TankAlarm.
  - [ ] Enter NAS FTPS settings in Server UI (host, port, user, FTPS enabled, trust settings).
  - [ ] Execute backup and restore validation on NAS, confirming folder creation and file presence match Pyftpdlib results.
  - [ ] Confirm history/archive data writes correctly on NAS and remains readable for restore/inspection.
  - [ ] Record final go-live status and any NAS-specific deviations.

### Other Future Enhancements

- [ ] **Pump Off Control Implementation** — Review and implement pump-off control options per PUMP_OFF_CONTROL_OPTIONS_03202026.md.
- [ ] **Modularization** — Break monolithic `.ino` files into modular compilation units. Blocked by Arduino IDE constraints. Consider PlatformIO migration.
- [ ] **Power State Machine Test Automation** — Execute the 7 documented test scenarios (POWER_STATE_TEST_COVERAGE.md). Currently test procedures exist, but execution status is unknown.
- [ ] **Pre-LVD Emergency Save/Sync Path (Client)** — After hardware validation confirms baseline behavior, add an optional pre-LVD path that triggers one-time state save + telemetry sync when battery/load indicators show imminent disconnect risk. Keep this deferred until code-hardware validation is complete.

---

## Deferred / Dropped Items

| Item | Reason | Source |
|------|--------|--------|
| 3.2.2 Current Loop Caching | DROPPED — No current need | FUTURE_3.2.2 |
| 3.3.1 I2C Mutex Guard | DROPPED — Dual-core not planned | FUTURE_3.3.1 |
| 3.3.2 Address Conflict Detection | DROPPED — Superseded by 3.2.5 | FUTURE_3.3.2 |
| 3.3.4 Flash-Backed I2C Log | DROPPED — Superseded by 3.2.4 | FUTURE_3.3.4 |
| Notecard Templates | DEFERRED — Schemas still evolving | FUTURE_OPTIMIZATION_TEMPLATES |
| Integration Test Suite | DEFERRED to v2.0 — 16-40 hrs | FUTURE_3.3.6 |
| Monolithic file refactor | DEFERRED — Arduino IDE constraint | MODULARIZATION_DESIGN_NOTE |

---

## Completed Items (Archive)

Items moved here after implementation. Include version number and date.

### Completed in v1.6.13 (April 23, 2026)
- [x] **I-24:** `applyConfigUpdate()` now parses `doc["solarCharger"]` and re-inits the `SolarManager` when `enabled`/`slaveId`/`baudRate`/`timeoutMs` change. Same bug class as the v1.6.12 `batteryConfig` regression — server-pushed solar charger settings now actually take effect at runtime (Client)
- [x] **I-25:** `saveConfigToFlash()` now serializes the `vinMonitor` block (was previously loaded and applied but never written back to flash) (Client)
- [x] **I-26:** `saveConfigToFlash()` now serializes the `solarOnlyConfig` block (same gap as I-25; broke offline-only deployments) (Client)
- [x] **m-8:** `SolarManager::checkAlerts()` now surfaces `COMM_FAILURE` (or `NONE` if opted out) before any voltage/temp/fault branch when `communicationOk` is false. Stops stale `battery_critical`/`battery_low` alerts from firing on the last cached voltage after the Modbus link drops (Common/Solar)

### Completed in v1.6.12 (April 23, 2026)
- [x] **BUG (Critical):** `applyConfigUpdate()` ignored the `batteryConfig` block — server-pushed chemistry/nominal-voltage changes were silently dropped, then `saveConfigToFlash()` overwrote flash with the stale in-memory value, so reboots did not recover. Parser added; mirrors `loadConfigFromFlash()` (Client)
- [x] **BUG (Medium):** Chemistry-mismatch message buffer `chemMsg[96]` truncated the worst-case lithium MISMATCH warning (~124 chars). Bumped to 160 (Client)
- [x] **Cleanup:** Dropped redundant `batteryTypeName` field from `saveConfigToFlash()` (loader uses int enum) (Client)

### Completed in v1.6.11 (April 23, 2026)
- [x] **REGRESSION FIX:** `saveConfigToFlash()` was still emitting the legacy `batteryMonitor` JSON block after the v1.6.9 cleanup removed the loader. Now writes `batteryConfig` (enabled/batteryType/nominalVoltage) matching the server schema, so chemistry/voltage selections survive reboot (Client)

### Completed in v1.6.10 (April 22, 2026)
- [x] **Tightened lithium chemistry verification:** A default Sealed-DIP SunSaver under a "LiFePO4" UI selection now flags MISMATCH instead of passing. `verifyChemistry()` lithium ranges narrowed; active V_eq with a lithium selection escalates to DANGER (Common/Solar)

### Completed in v1.6.9 (April 22, 2026)
- [x] **SunSaver chemistry cross-check:** Best-effort 4-register burst at `SS_REG_V_REG`/`V_FLOAT`/`V_EQ` reads SunSaver active setpoints over Modbus. New `SolarManager::verifyChemistry()` warns when the configured battery chemistry doesn't match the SunSaver DIP-derived setpoints. One-shot serial log on first valid poll. Plausibility-gated (8..32V) so absent/garbled reads safe-fail (Common/Solar, Client)
- [x] **Back-compat removal (decoupled battery):** `BatteryType` enum renumbered to chemistry-only (NONE=0, AGM=1, FLOODED=2, GEL=3, SLA=4, LIFEPO4=5, LI_ION=6, LIPO=7, CUSTOM=8). Legacy `LEAD_ACID_12V`/`LIFEPO4_12V` aliases removed. `derivePowerSource()` JS helper removed from server. Server `loadConfig()` `revMap` and client loader updated; no legacy fallback (Common/Battery, Server, Client)
- [x] **Caveat documented:** SunSaver Modbus does NOT expose chemistry presets remotely — DIP switch only handles lead-acid, lithium MUST be loaded via MSView (USB MeterBus) custom EEPROM profile. `validatePowerCombination()` warns on Modbus-MPPT + lithium combo with explicit MSView instruction (Server)
- [x] **Setpoint registers (BENCH-UNVERIFIED):** `SS_REG_V_REG=0x0033`, `SS_REG_V_FLOAT=0x0035`, `SS_REG_V_EQ=0x0036` — addresses still need bench confirmation against a real SunSaver before relying on the verification path in production (Common/Solar)

### Completed in v1.6.8 (April 22, 2026)
- [x] **Decouple battery chemistry from power source UI:** Replaced single combined dropdown with 4 decoupled controls (power source, battery chemistry, nominal pack voltage, monitoring enable). New `batteryConfig` JSON block; `initBatteryConfig(BatteryConfig*, BatteryType, uint8_t nominalVoltage=0)` scales 12V baselines by `nominalVoltage/12`. `BatteryConfig.nominalVoltage` is `uint8_t` (0 sentinel for LiPo, else 12 or 24). New `batteryIsLeadAcid()`, `batterySupportsEqualize()` (FLOODED only), `batteryTypeLabel()` helpers (Common/Battery, Server, Client)

### Completed in v1.6.7 (April 21, 2026)
- [x] **30-minute service window after boot:** Client honors a service-mode window after boot for installers (Client)

### Completed in v1.6.1 Implementation Review Fixes (April 8, 2026)
- [x] **CRITICAL:** `relay_timeout` re-activation loop — `sendAlarm()` now excludes `relay_timeout` from `isAlarm`, preventing infinite timeout→activate→timeout cycle (Client)
- [x] **HIGH:** Server clears alarm state on `relay_timeout` — separated into own branch that preserves `alarmActive` and does not call `clearAlarmEvent()` (Server)
- [x] **MEDIUM:** Ownerless manual relay duration ignored — `checkRelayMomentaryTimeout()` now handles `RELAY_SRC_MANUAL` relays with `customDurationSec` even without an owning monitor (Client)
- [x] **MEDIUM:** Duplicate `setRelayState()` in `processRelayCommand()` — removed bare GPIO call; unified helpers already call it internally (Client)
- [x] **LOW:** Unused `b64Buf` in DFU — removed allocation, null-check, and all `free(b64Buf)` calls (Common/TankAlarm_DFU.h)
- [x] **LOW:** Missing DFU decoded length bounds check — added abort if `decoded > thisChunk || decoded > remaining` (Common/TankAlarm_DFU.h)
- [x] **LOW:** `clearAllRelayAlarms()` stale `gRelayRuntime[]` — fallback loop now checks `.active` and zeros runtime struct (Client)
- [x] **FALSE POSITIVE:** DFU CRC padding mismatch (Gemini claim) — verified readback uses `firmwareLength`, not page-aligned size; no fix needed
- [x] **DESIGN DECISION:** Relay overlap validation warn-only — preserved; `findMonitorForRelay()` uses first-match
- [x] **DEFERRED:** DFU erase-before-download — known Phase 2 limitation, deferred to A/B partitioning

### Completed in v1.6.0 Implementation (April 8, 2026)
- [x] **PR-1:** Relay Runtime + Safety Timeout — `RelayRuntime` struct, `gRelayRuntime[]`, `relayMaxOnSeconds` config field, helper functions (Client)
- [x] **PR-2:** Debounce Split — separate high/low debounce counters per monitor (Client)
- [x] **PR-4:** DFU CRC Verification — running download CRC + flash readback CRC comparison (Common/TankAlarm_DFU.h)
- [x] **PR-6:** Documentation — V1.6.0 release notes, CONFIG_SCHEMA_VERSION bump to 2

### Completed in Code Review Implementation (April 2, 2026)
- [x] **CRITICAL-1:** Session token PRNG → STM32 hardware RNG with LCG fallback (Server)
- [x] **CRITICAL-2:** Relay actuation decoupled from rate limiting — relays activate even when SMS is rate-limited (Client)
- [x] **CRITICAL-3:** UNTIL_CLEAR relay clearing — fixed comparison that always evaluated false (Client)
- [x] **HIGH-2:** Security headers (`X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`) on all HTTP responses (Server)
- [x] **HIGH-6:** PIN digit-only validation added to first-login path (Server)
- [x] **HIGH-7 / I-7:** SMS dispatch requires both client intent AND server policy (Server)
- [x] **HIGH-8:** Unload event payload now includes `sms`/`email` notification flags (Client)
- [x] **MED-12:** Rate limit per-monitor timestamp committed only after both per-monitor and global checks pass (Client)
- [x] **MED-15:** Relay state restored on recovery from CRITICAL_HIBERNATE (Client)
- [x] **MED-17:** Hot-tier analytics (MoM + YoY) now filter snapshots by target month/year via `gmtime()` (Server)

### Completed in Code Review Pass (June 9, 2026)
- [x] **C-6:** Viewer DFU — `dfu.status` → `card.dfu` with `name:"stm32"` (Viewer)
- [x] **C-2:** SMS rate limit — fails closed (deny) when no time sync (Server)
- [x] **C-3:** Daily email time label — added "(UTC)" indicator (Server)
- [x] **C-1:** Stuck sensor detection — default changed to disabled/opt-in (Client)
- [x] **C-4:** Ethernet retry — `delay()` → `safeSleep()` for watchdog safety (Viewer)
- [x] **H4:** Archive manifest — atomic write-to-temp-then-rename (Server)
- [x] **I-10:** Delete-before-parse — two-step get-then-delete in all 3 components
- [x] **I-3:** Schema version — `_sv` field check added to Viewer summary handler
- [x] **I-9:** Viewer cadence — `refreshSeconds` clamped 3600–86400, `baseHour` clamped 0–23
- [x] **I-4:** Solar poll — return semantics documented; `isCommunicationOk()` for failure state
- [x] **I-5:** Current-loop stale data — reclassified as by-design (stuck detection is backstop)
- [x] **m-6:** Negative level clamping — all fallback paths now clamp to ≥ 0.0f (Server)
- [x] **m-7:** Archive manifest — merged with H4 atomic write fix (Server)
- [x] **M-2:** Sensor recovery rate limiting — now goes through `checkAlarmRateLimit()` (Client)
- [x] **M-3:** Sensor recovery debounce — requires 3 consecutive good readings (Client)
- [x] **M-6:** Power threshold validation — hysteresis cross-check added (Client)
- [x] **A7:** Viewer summary constants — centralized in TankAlarm_Common.h; Server/Viewer deduplicated

### Completed in v1.1.8 (March 16, 2026)
- [x] Save all dirty data before DFU/reboot
- [x] Deduplicate daily summaries on reboot
- [x] Persist SMS rate-limit state across power cycles
- [x] Expand FTP backup to 9 files
- [x] Warm tier fallback in FTP monthly archives
- [x] Wire `ftpSyncHour` into maintenance check
- [x] 3-layer stale sensor auto-pruning
- [x] FTP archival system with API + UI

### Completed in v1.1.7 (March 15, 2026)
- [x] Sensor-centric naming refactor (`tankNumber` → `sensorIndex`)
- [x] `userNumber` field added
- [x] Backward-compatibility dual-key JSON removed
- [x] `saveConfigToFlash()` serialization fix
- [x] `serializeConfig()` correct `sensorIndex` emission

### Completed in v1.1.6 (March 13, 2026)
- [x] **S-1:** Remove PIN from browser localStorage
- [x] **S-2:** Remove session tokens from URL query strings (header-based auth)
- [x] **S-3:** Require PIN enrollment on fresh install
- [x] **C-1:** Relay command target-UID verification
- [x] Single-session login with server-generated tokens + constant-time comparison
- [x] **S-4:** Config dispatch/cache buffer partial fix
- [x] DFU auto-recovery
- [x] Tank registry deduplication
- [x] Per-monitor stuck sensor detection (configurable toggle)
- [x] Notefile schema versioning (`_sv` field)
- [x] Boot-time `.tmp` orphan cleanup
- [x] Viewer metadata enrichment + deterministic MAC + Ethernet retry
- [x] 5s relay command cooldown
- [x] I2C sensor recovery max attempts cap
- [x] Solar sunset state persistence

### Completed in v1.1.5 (March 10, 2026)
- [x] Measurement unit & object type metadata in payloads
- [x] Orphaned tank record pruning
- [x] Auto-DFU re-enabled in client main loop
- [x] External antenna documentation

### Verified Fixed (March 24, 2026 audit)
- [x] **C-5:** UNTIL_CLEAR relay clear logic (Client)
- [x] **I-2:** Viewer summary fetch bootstrap deadlock (Viewer)
- [x] **I-7:** Server SMS override — confirmed as intentional design choice
- [x] **I-8:** Sensor metadata (`ot`, `mu`, `st`) now in alarm payloads (Client)
- [x] **I2C Bus Recovery:** `tankalarm_recoverI2CBus()` in shared TankAlarm_I2C.h
- [x] **V-1:** Tank array bounds check (Viewer)
- [x] **DEFAULT_PRODUCT_UID:** ClientConfig.h conditional include fix
- [x] **Config ACK Routing:** Separate `config_ack.qo`/`.qi` pair centralized
- [x] **M-7:** Sensor-recovered alarms now SMS-eligible (Server)
- [x] **M-8:** Calibration singular matrix with fallback — acceptable
- [x] **Config Buffer:** Dispatch/cache buffer sizes aligned (Server)
- [x] **respondHtml():** Now streaming with `sendStringRange()` (Server)
- [x] **H4 (partial):** Core save functions use atomic writes; archive manifest still non-atomic
- [x] Common Header — **A1:** CONFIG_ACK files centralized
- [x] Common Header — **A2:** MAX_RELAYS centralized
- [x] Common Header — **A3:** CLIENT_SERIAL_BUFFER_SIZE centralized
- [x] Common Header — **A4:** DFU_CHECK_INTERVAL_MS centralized
- [x] Common Header — **E1:** Dead #ifndef redefinitions removed

### Verified Fixed (April 9, 2026 audit)
- [x] **m-10:** VinVoltage now renders in the dashboard/site-config UI (Server)

### Completed (FUTURE work items)
- [x] **3.1.1** — Notecard `begin()` Idempotency Audit
- [x] **3.1.2** — Extract I2C Operations to Shared Header (TankAlarm_I2C.h)
- [x] **3.1.3** — Server/Viewer I2C Hardening
- [x] **3.2.3** — Notecard Health Check Backoff
- [x] **3.2.4** — I2C Recovery Event Logging
- [x] **3.2.5** — Startup Scan in Health Telemetry

---

## Review Sources

This TODO was compiled from the following documents, sorted by date:

> Three 2026-07-21 server review documents (`CODE_REVIEW_07212026_SERVER_FULL_REVIEW.md`, its Copilot addendum, and `CODE_REVIEW_07212026_SERVER_INDEPENDENT_COPILOT.md`) exist in the working tree but are not yet committed; they are listed in the 2026-09 master review's sources and should be committed with the next docs pass.

| Date | Document | Type |
|------|----------|------|
| 2026-09-15 | CODE_REVIEW_09152026_WEBSITE_PRS_314_317_CLAUDE.md | Website PRs #314-#317: changes, verification, Copilot rounds, residuals |
| 2026-09-09 | CODE_REVIEW_09092026_MASTER_REPOSITORY_REVIEW.md | Consolidated master review of the four 2026-09-08 reviews; tracked above |
| 2026-09-08 | CODE_REVIEW_09082026_REPOSITORY_FULL_REVIEW.md | Full repository review |
| 2026-09-08 | CODE_REVIEW_09082026_REPOSITORY_INDEPENDENT_COPILOT.md | Independent repository review (Copilot) |
| 2026-09-08 | CODE_REVIEW_09082026_REPOSITORY_REVIEW_CLAUDE.md | Full repository review, multi-agent verified (163 findings, prior-item status) |
| 2026-09-08 | CODE_REVIEW_09082026_REPOSITORY_REVIEW_COPILOT.md | Full repository review (Copilot) |
| 2026-08-18 | FEATURE_PLAN_08182026_REMINDER_SNOOZE.md | Feature plan |
| 2026-07-20 | CODE_REVIEW_07202026_ISSUE_313_SERVER_WEBPAGE_BUGS.md | Server webpage bugs (issue #313) |
| 2026-07-08 | EMAIL_DELIVERY_OPTIONS_07082026.md | Email delivery options |
| 2026-07-06 | CODE_REVIEW_07062026_SMS_PIPELINE_END_TO_END.md | SMS pipeline end-to-end review |
| 2026-07-02 | CODE_REVIEW_07022026_POST_CONFIG_DIAG_BURST_AND_CONFIG_LOSS.md | Post-config diagnostics and config loss |
| 2026-07-02 | CODE_REVIEW_07022026_RS485_SUNSAVER_MRC1_LINK_INVESTIGATION.md | RS-485 / SunSaver MPPT |
| 2026-07-01 | CODE_REVIEW_07012026_CURRENT_LOOP_NO_SHUNT_MEASUREMENT_ARCHITECTURE.md | Current-loop sensor |
| 2026-07-01 | CODE_REVIEW_07012026_DAC_LOOP_POWER_INTEGRATION_PLAN.md | Review |
| 2026-07-01 | CODE_REVIEW_07012026_MOSFET_GATING_FAILURE_AND_SOFT_RAMP_PROOF.md | Review |
| 2026-06-30 | CODE_REVIEW_06302026_BENCH_TESTS_AND_STUCK_MA_PROOF.md | Bench tests |
| 2026-06-30 | CODE_REVIEW_06302026_CURRENT_LOOP_STUCK_MA_ROOT_CAUSE.md | Current-loop sensor |
| 2026-06-29 | CODE_REVIEW_06292026_CLIENT_TELEMETRY.md | Client telemetry review |
| 2026-06-29 | FTPSCLIENTOPTA_UPSTREAM_CHANGES_06292026.md | FTPSclientOPTA upstream changes |
| 2026-06-26 | CODE_REVIEW_06262026_CURRENT_LOOP_SIMPLIFICATION.md | Current-loop sensor |
| 2026-06-26 | CODE_REVIEW_06262026_DIAGNOSTICS_OPTAVIEW_AND_PRODUCTION_RESTORE.md | Diagnostics |
| 2026-06-25 | CODE_REVIEW_06252026_BATTERY_VOLTAGE_SAMPLING.md | Battery voltage sampling |
| 2026-06-25 | CODE_REVIEW_06252026_CURRENT_LOOP_SENSORS.md | Current-loop sensor |
| 2026-06-25 | CODE_REVIEW_06252026_POST_V2050_REMAINING_ISSUES.md | Post-v2.0.50 remaining issues |
| 2026-06-25 | CODE_REVIEW_06252026_RS485_SUNSAVER_COMMUNICATION.md | RS-485 / SunSaver MPPT |
| 2026-06-25 | CODE_REVIEW_06252026_RS485_TELEMETRY_FOLLOWUP.md | Review |
| 2026-06-24 | A0602_ADDRESSING_FINDINGS_AND_FIX_TEST_PLAN_06242026.md | A0602 expansion |
| 2026-06-24 | OPTA_A0602_USB_DIAGNOSTIC_PLAN_06242026.md | Diagnostics |
| 2026-06-24 | OPTA_I2C_BUS_SEPARATION_FEASIBILITY_06242026.md | I2C bus |
| 2026-06-24 | USB_ONLY_OPTA_CLIENT_COMMANDS_06242026.md | USB-only client diagnostics |
| 2026-06-24 | USB_ONLY_OPTA_CLIENT_DIAGNOSTIC_TODO_06242026.md | Diagnostics |
| 2026-06-24 | USB_ONLY_OPTA_CLIENT_RUN_LOG_06242026.md | USB-only client diagnostics |
| 2026-06-23 | CODE_REVIEW_06232026_FIELD_DIAGNOSTICS.md | Field diagnostics |
| 2026-06-23 | CODE_REVIEW_06232026_FIELD_DIAGNOSTICSv2.md | Field diagnostics |
| 2026-06-23 | OPTA_AUX_I2C_CONTENTION_REVIEW_06232026.md | I2C bus |
| 2026-06-23 | REMAINING_PROBLEMS_AND_ATTEMPTS_06232026.md | Remaining problems log |
| 2026-06-22 | CODE_REVIEW_06222026_DEFERRED_ITEMS.md | Deferred items |
| 2026-06-22 | CODE_REVIEW_06222026_VOLTAGE_ALARM.md | Voltage alarm |
| 2026-06-21 | CODE_REVIEW_06212026_OTA_V1930_TO_V1931_FAILURE.md | OTA update failure analysis |
| 2026-06-18 | CODE_REVIEW_06182026_OTA_V1934_TO_V1935_SWAP_FAILURE.md | OTA update failure analysis |
| 2026-06-16 | CODE_REVIEW_06162026_OTA_SILENT_CHECK_FAILURE.md | OTA update failure analysis |
| 2026-06-15 | CODE_REVIEW_06152026_CURRENT_LOOP_MA_READING.md | Current-loop sensor |
| 2026-06-15 | CODE_REVIEW_06152026_OPTA_POST_DFU_ETHERNET.md | Review |
| 2026-06-15 | CODE_REVIEW_06152026_OTA_VS_USB_UPDATE_FAILURE.md | OTA update failure analysis |
| 2026-06-11 | MCUBOOT_SERVER_VIEWER_SUGGESTIONS_06112026.md | MCUboot |
| 2026-06-10 | MCUBOOT_QSPI_STORAGE_CONFLICT_06102026.md | MCUboot |
| 2026-06-09 | CODE_REVIEW_06092026_ODFU_CLIENT_HANDSHAKE_COPILOT.md | ODFU / bootloader |
| 2026-06-09 | CODE_REVIEW_06092026_UPDATE_SYSTEM_v1.9.0_COPILOT.md | Update system review |
| 2026-06-09 | CODE_REVIEW_06092026_UPDATE_SYSTEM_v1.9.0_COPILOT_FINAL.md | Update system review |
| 2026-06-09 | CODE_REVIEW_06092026_UPDATE_SYSTEM_v1.9.0_COPILOT_v2.md | Update system review |
| 2026-06-09 | CODE_REVIEW_06092026_UPDATE_SYSTEM_v1.9.0_PROPOSED_FIXES.md | Proposed fixes |
| 2026-06-09 | CODE_REVIEW_DFU_IAP_ANALYSIS_06092026.md | DFU IAP analysis |
| 2026-06-09 | CODE_REVIEW_DUAL_BANK_AB_IMPLEMENTATION_PLAN_06092026.md | Implementation plan |
| 2026-06-09 | CODE_REVIEW_ODFU_BRICK_PROOF_IMPLEMENTATION_PLAN_06092026.md | Implementation plan |
| 2026-06-08 | CODE_REVIEW_06082026_CLIENT_RS485_SUNSAVER_MPPT_IMPLEMENTATION_PLAN.md | Implementation plan |
| 2026-06-08 | CODE_REVIEW_06082026_CLIENT_RS485_SUNSAVER_MPPT_v1.7.2.md | RS-485 / SunSaver MPPT |
| 2026-06-08 | CODE_REVIEW_06082026_CLIENT_RS485_SUNSAVER_MPPT_v1.7.2_COPILOT_v1.1.md | RS-485 / SunSaver MPPT |
| 2026-06-08 | CODE_REVIEW_06082026_CLIENT_RS485_SUNSAVER_MPPT_v1.7.2_COPILOT_v1.2.md | RS-485 / SunSaver MPPT |
| 2026-06-07 | CODE_REVIEW_06072026_SERVER_CLIENT_WIRELESS_SENSOR_v1.7.2_COPILOT_v1.1.md | Wireless sensor review |
| 2026-06-07 | CODE_REVIEW_06072026_SERVER_CLIENT_WIRELESS_SENSOR_v1.7.2_COPILOT_v1.2.md | Wireless sensor review |
| 2026-06-07 | CODE_REVIEW_06072026_SERVER_CLIENT_WIRELESS_SENSOR_v1.7.2_COPILOT_v1.3.md | Wireless sensor review |
| 2026-06-04 | CODE_REVIEW_06042026_SERVER_CLIENT_WIRELESS_SENSOR_v1.7.2_COPILOT.md | Wireless sensor review |
| 2026-06-04 | CODE_REVIEW_06042026_v1.6.15_COPILOT.md | Review (Copilot) |
| 2026-06-03 | CODE_REVIEW_06032026_IMPLEMENTATION_PLAN.md | Implementation plan |
| 2026-06-03 | CODE_REVIEW_06032026_SERVER_CLIENT_WIRELESS_SENSOR_v1.0_COPILOT.md | Wireless sensor review |
| 2026-06-03 | CODE_REVIEW_06032026_SERVER_CLIENT_WIRELESS_SENSOR_v1.1_COPILOT.md | Wireless sensor review |
| 2026-06-03 | CODE_REVIEW_06032026_SERVER_CLIENT_v1.6.14_COPILOT.md | Review (Copilot) |
| 2026-06-03 | CODE_REVIEW_06032026_v1.6.14_COPILOT.md | Review (Copilot) |
| 2026-05-14 | CODE_REVIEW_IMPROVEMENTS_ANALYSIS_05142026.md | Improvements analysis |
| 2026-04-23 | v1.6.13 implementation pass (this conversation) | Closed I-24/I-25/I-26 (applyConfigUpdate solarCharger parser; vinMonitor+solarOnlyConfig flash save) and m-8 (stale solar alert ordering); both client+server compile clean |
| 2026-04-23 | v1.6.12 self-audit (this conversation) | Bug audit of v1.6.7–v1.6.11 SunSaver/battery work; shipped applyConfigUpdate batteryConfig parser, chemMsg buffer bump; logged I-24/I-25/I-26 applyConfigUpdate parity gaps |
| 2026-04-22 | v1.6.8–v1.6.10 SunSaver chemistry work | Decoupled battery UI, chemistry verification via Modbus setpoint readback, lithium MISMATCH detection, legacy enum aliases removed |
| 2026-04-21 | v1.6.7 service window | 30-min service-mode window after boot |
| 2026-04-15 | FTPS_LIBRARY_INTEGRATION_STUDY_04152026.md | ArduinoOPTA-FTPS library integration feasibility study; server-side FTPS migration tasks F-1 through F-13 |
| 2026-04-13 | FTPS planning note/checklist update | Narrowed the future secure-transport plan to Explicit TLS FTPS only; left Implicit TLS out of the current implementation scope |
| 2026-04-13 | Validated April 13 implementation pass | Implemented C-7, I-15 thru I-19, I-21, I-23, M-18 thru M-21, M-24 thru M-26, and FTP credential obfuscation at rest; revalidated M-27 as already fixed in API output; deferred secure FTP transport follow-up |
| 2026-04-13 | Live-code validation audit of April 13 review set | Accuracy check against current code. Dropped I-22, M-22, M-23 as unsupported; softened M-24 wording; added M-25 thru M-27 |
| 2026-04-13 | CODE_REVIEW_04132026_COMPREHENSIVE.md, LOGIC_REVIEW_04132026.md | Comprehensive code + logic review (Claude Opus 4.6). New: C-7, I-18 thru I-23, M-19 thru M-24, m-13, m-14 |
| 2026-04-13 | CODE_REVIEW_04132026_COPILOT.md, LOGIC_REVIEW_04132026_COPILOT.md | Targeted code + logic review update. New: I-15, I-16, I-17, M-18 |
| 2026-04-09 | CODE_REVIEW_04092026_COMPREHENSIVE.md, LOGIC_REVIEW_04092026.md | Full codebase code + logic review (Claude Opus 4.6). New: I-12 thru I-14, M-12 thru M-17, m-11, m-12 |
| 2026-04-09 | CODE_REVIEW_04092026_COPILOT.md, LOGIC_REVIEW_04092026_COPILOT.md | Code + logic review update (new findings I-11, M-11; verified m-10 fixed) |
| 2026-04-08 | FUTURE_WORK_04082026.md §12-13 | v1.6.0 implementation reviews (GPT-5.3-Codex, Gemini 3.1 Pro, GPT-5.4) + v1.6.1 fixes |
| 2026-04-02 | CODE_REVIEW_04022026_COMPREHENSIVE.md | Comprehensive review with peer cross-validation |
| 2026-04-02 | IMPLEMENTATION_PLAN_04022026.md | Implementation plan (11 of 16 findings implemented) |
| 2026-03-24 | LOGICREVIEW-20260324-* (9 files) | Logic review — multiple AI reviewers |
| 2026-03-24 | CODE_REVIEW_VIEWER_DECISION_LOGIC_03242026.md | Viewer decision logic review |
| 2026-03-21 | CODE_REVIEW_PUMP_OFF_CONTROL_03212026.md | Pump off control review |
| 2026-03-20 | PUMP_OFF_CONTROL_OPTIONS_03202026.md | Design options |
| 2026-03-16 | V1.1.8_RELEASE_NOTES.md | Release notes |
| 2026-03-15 | V1.1.7_RELEASE_NOTES.md | Release notes |
| 2026-03-13 | V1.1.6_RELEASE_NOTES.md | Release notes |
| 2026-03-12 | CODE_REVIEW_03122026_*.md (3 files) | Comprehensive code review |
| 2026-03-10 | V1.1.5_RELEASE_NOTES.md | Release notes |
| 2026-02-26 | NEXT_PRIORITIES_02262026.md | Priority roadmap |
| 2026-02-26 | CODE_REVIEW_RECOMMENDATIONS_02262026.md | Recommendations |
| 2026-02-26 | I2C_COMPREHENSIVE_ANALYSIS_02262026.md | I2C deep dive |
| 2026-02-26 | WATCHDOG_STARVATION_AUDIT_02262026.md | Watchdog audit |
| 2026-02-26 | BUGFIX_WATCHDOG_STARVATION_02262026.md | Watchdog bugfix |
| 2026-02-19 | COMMON_HEADER_AUDIT_02192026.md | Common header review |
| 2026-02-19 | CODE_REVIEW_02192026_COMPREHENSIVE*.md | Comprehensive review |
| 2026-02-20 | CODE_REVIEW_FIX_IMPLEMENTATION_02202026.md | Fix implementation tracking |
| 2026-02-06 | SECURITY_FIXES_02062026.md | Security hardening |
| Various | FUTURE_3.x.x_*.md (15 files) | Future work items |
| Various | H4_ATOMIC_WRITE_REFACTOR_RECOMMENDATION.md | Architecture recommendation |

---

## Maintenance Notes

- **Update frequency:** After every code review, logic review, or release commit.
- **Workflow:** When an item is completed, move it to the [Completed Items](#completed-items-archive) section with the version number and date. Do not delete items — archive them.
- **New findings:** Add new items under the appropriate severity section. Include source document, affected component(s), and a clear description of the issue and fix.
- **Priority reassessment:** Re-evaluate priorities after each release. Items may be promoted or demoted based on field experience.
