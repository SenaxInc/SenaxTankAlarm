# Master Repository Review - 09092026

Date: 2026-09-09. Status: consolidated review; no firmware fixes applied.

## Decision Summary

The highest-priority supported work is physical input/output mapping and relay ownership, configuration round-trip integrity, calibration identity, commissioning/discovery, alarm delivery state, and protection against history/restore data loss. Reconcile the dirty client/Common baseline before deploying any fix. Do not lead remediation with the earlier schema-2 multipart demonstration: that exact traffic is rejected by the currently shipped schema-1 receiver.

Main groups: [Server](#server), [Client](#client), [Viewer](#viewer), and [Shared Platform and Repository Tooling](#shared-platform-and-repository-tooling). Within them, the review separates website/operator workflows, acquisition/history, alarms/control and delivery, and persistence/power/maintenance. [Validity Corrections](#validity-corrections) and [Source Crosswalk](#source-crosswalk) explain why some earlier findings or proposed patches changed.

Coverage: four source reviews, 253 finding-list entries including repeated claims, consolidated into 46 issue groups. Every source ID has a disposition; 100 local source/document links and the issue cross-references validate. These counts measure coverage, not 253 independently confirmed defects.

## Scope and Sources

Current local baseline: `cf8896f`, branch `master`, firmware v2.2.14, build sequence 301, notefile schema 1. Existing uncommitted client/Common and CI changes are reviewed separately from committed behavior and are not modified.

The four recent review documents are inputs, not four independent confirmations of shared claims:

- FULL: [CODE_REVIEW_09082026_REPOSITORY_FULL_REVIEW.md](CODE_REVIEW_09082026_REPOSITORY_FULL_REVIEW.md).
- COPILOT: [CODE_REVIEW_09082026_REPOSITORY_REVIEW_COPILOT.md](CODE_REVIEW_09082026_REPOSITORY_REVIEW_COPILOT.md).
- INDEPENDENT: [CODE_REVIEW_09082026_REPOSITORY_INDEPENDENT_COPILOT.md](CODE_REVIEW_09082026_REPOSITORY_INDEPENDENT_COPILOT.md).
- CLAUDE: [CODE_REVIEW_09082026_REPOSITORY_REVIEW_CLAUDE.md](CODE_REVIEW_09082026_REPOSITORY_REVIEW_CLAUDE.md).

## How to Use This Review

Findings are grouped by the component that owns the fix, then by behavior. Cross-component issues are described once and referenced from the other owner. Source IDs identify provenance, not separate independent confirmations: FULL and COPILOT largely share findings; CLAUDE also contains duplicate rows.

- **Confirmed:** current source and its controlling path support the stated trigger; executable checks are identified separately.
- **Conditional:** the mechanism exists but depends on a future protocol change, particular deployment, hardware condition, or unmeasured capacity.
- **Corrected/rejected:** an earlier claim or proposed implementation conflicts with current evidence.
- **P1:** address before deploying the affected control, configuration, or data-retention workflow. **P2:** reliability/operability repair. **P3:** hardening, maintainability, or measured optimization.

Line links target the present workspace. Client/Common line numbers differ between the dirty working copy and committed source; function names and the baseline distinction are authoritative. Prior build/browser results are not relabeled as tests rerun on 09092026.

## Validity Corrections

### Daily Part Numbering: Conditional Hazard, Not a Current Schema-1 Reproduction

The current [schema constant](../TankAlarm-112025-Common/src/TankAlarm_Common.h#L29) is 1. The [inbound schema gate](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12213) rejects a note with a newer schema before invoking its handler. In [daily reconciliation](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L13163), a schema-1 part 1 without an `alarms` array does not reconcile. The earlier model using `_sv:2` bypassed that controlling gate and is not evidence that the shipped client clears alarms through this route.

`isFirstPart = (part == 0 || part == 1)` remains ambiguous and would activate the missing-array clear hazard after a schema change that admits schema 2. Resolve numbering and explicit alarm-summary semantics before such a rollout. Do not blindly bump the global schema: older receivers currently discard future-schema notes.

Source disposition: FULL/COPILOT F-01; INDEPENDENT R-01/R-21; CLAUDE prior-item appendix. **Conditional, not a present schema-1 alarm-clear reproduction.** An explicitly supplied alarm array on another part is a separate malformed/ambiguous-input case.

### Other Corrections That Change the Plan

| Earlier claim or suggestion | Consolidated decision |
| --- | --- |
| Client never re-arms its daily schedule after late time sync | **Rejected for the ordinary running-client path.** `loop()` calls `updateDailyScheduleIfNeeded()` after `ensureTimeSync()` outside critical hibernate. Server and viewer still have the gap. FULL/COPILOT F-07 narrowed; INDEPENDENT R-08 retained with this scope. |
| Dashboard Update has no client consumer | **Working-tree-only regression.** Committed `HEAD` has `pollForTelemetryRequests`; the dirty June-era client does not. INDEPENDENT R-06 is not a committed-code defect. |
| Offline sensor alarms are discarded at HEAD | **Working-tree-only for the reinstated availability gate.** HEAD `sendAlarm` calls the buffering publisher. Notification loss from rate limiting and the separate remote-relay publisher remains valid. |
| Zero deadband means publish every sample | **Rejected.** `thresholdEnabled = threshold > 0`, then `needBaseline || changeExceeded`, suppresses ordinary unchanged/change telemetry at zero. Baseline, daily, alarms, and explicit forced requests are separate paths. Do not enable more periodic data by claiming it saves data. |
| Restore four LED constants in a lookup table to repair relays | **Insufficient and wrong for coil control.** Core 4.5.0 distinguishes `RELAY1..4` / D0..D3 from `LED_D0..3` = 7,9,8,153. Fix output and input terminal mapping together; bench-verify contacts with non-production loads. |
| Reinterpret `relay_reset_sensor` as a sensor number and fall back to array index | **Reject ambiguous compatibility fallback.** Value 1 can mean sensor 1 or old array slot 1. Use a new explicit key/version/capability and resolve stable identity; never guess after a failed lookup. |
| Integer `_ts` reading simply truncates fractional numbers | **Correct mechanism:** ArduinoJson default operator first tests type compatibility. A floating `_ts` can select default 0; an integral `_ts` still works. The same-second collision remains after type repair. Avoid claims that every deployed client has always remained at zero without runtime evidence. |
| Custom history dates have no change handler | **Correct cause:** inline `onchange="renderLevelChart()"` exists, but the function is closure-local. The handler is present and inaccessible, not absent. |
| Snooze response precedes the save call / partly fixed since prior review | **Correct order:** mutate + broadcast, save attempt, response. The unresolved defect is broadcast before durability plus unobservable save failure, not response before the call. |
| Replay size mismatch is fixed by a 2,304-byte line buffer | **Not fully fixed.** The producer has a dynamic allocation path larger than the line cap. Prove producer/replay limits match; do not silently discard accepted data or merely move the drop earlier. |
| 50 ms loop warmup, >98% energy reduction, 8 microamp device sleep | **Unsupported deployment assumptions.** Current loop power is already duty-cycled. Retain validated settling and measure the complete Opta/carrier/sensor system. A 20 mA loop for 24 hours is 0.48 Ah at that loop voltage; 0.48 W at 24 V is 11.52 Wh/day, before losses. |
| Disable alarm sync, skip all nighttime Modbus, or keep unknown voltage in last state forever | **Needs safety policy.** Preserve urgent alert/remote-OFF latency, battery-recovery polling, and the daily OTA recovery window. Use bounded backoff and freshness-aware state transitions, not indefinite suppression. |
| Change the global radius/palette, hide all table overflow, or self-host Chart.js unconditionally | **Design/capacity decisions, not mandatory bug fixes.** Preserve the established design, repair missing tokens/classes and local overflow, and verify offline assets against flash/licensing budgets. Canvas colors need resolved color values, not just new CSS variable definitions. |
| Two-page agreement or a successful compile proves all claims | **Rejected.** Duplicated text, source-only reasoning, baseline differences, and fixtures have distinct evidentiary weight. Hardware outcomes, exact heap limits, timing estimates, and radio savings require measurement. |

Some COPILOT snippets use nonexistent helpers/fields, substitute API output keys `flt/l` for inbound `fault/lvl`, omit unsnooze's reminder-anchor update, return values from void functions, or hand-build JSON with a changed response schema. They are conceptual proposals, not approved patches. This master review specifies contracts and regression checks instead of endorsing those snippets wholesale.

## Server

### Website and Operator Workflows

#### S-W01 - Configuration Round-Trips Change Sensor and Alarm Meaning

**P1, confirmed and browser-reproduced.** [Configuration loader](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2121). Origins: CLAUDE H-15, H-16, H-17.

With the real config keys (`sensor:"current"` / `sensor:"digital"`), `loadConfig` restores the current-loop subtype before a later updater rebuilds its select; ultrasonic becomes pressure. Missing high/low thresholds leave template checkboxes enabled, so a low-only or high-only configuration acquires a second alarm threshold. The digital trigger selection is never restored, changing `not_activated` to `activated`.

Recommendation: construct interface-dependent controls first, then restore every explicit/absent field. Do not use truthiness to distinguish missing from zero. Regression gate: load -> collect -> reload must preserve ultrasonic/pressure, digital polarity, one-sided thresholds, zero thresholds, relay policy, and loop-power mode. These three round-trip defects were reproduced on 09092026 without posting a device config.

#### S-W02 - Closure-Local Functions Make Six Controls Inoperative

**P1, confirmed in browser.** [Calibration](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2184), [Transmission Log](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2210), [Site Configuration](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2381). Origins: CLAUDE H-18, H-20, H-21, M-24; FULL/COPILOT F-19; INDEPENDENT R-16.

`deleteClient`, `expectUpdate`, `resetCalibration`, `viewTankPoints`, `cancelPendingConfig`, and `renderLevelChart` are defined inside page closures but referenced by inline attributes. Each resolved as `undefined` on its page in the current fixture. Reset, removal, OTA expectation, config cancellation, and custom date changes can fail without a request.

Recommendation: bind events within the owning page scope, preferably with `addEventListener`; explicit `window` exports are a small compatibility repair. Test actual controls and error states, not only script parsing. Test dynamically created rows and confirmation/cancellation paths too.

#### S-W03 - Calibration Identity, Units, and Input Validation

**P1, confirmed.** [Key construction/parsing](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2181), [submit/backend](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L19465). Origins: FULL/COPILOT F-03; INDEPENDENT R-07; CLAUDE H-19, M-60.

Splitting `dev:<id>:<sensor>` at the first colon corrupts sensor selection, engineering-unit mode, submission, and filtering. Numeric device IDs are especially dangerous because a wrong sensor number may still parse. Backend validation also rejects a legitimate zero calibration reference through a boolean-style presence check, and does not require a valid/existing client-sensor pair before all work.

Recommendation: parse at the last separator or keep structured identity on the option; validate a nonempty sensor suffix and an integer range before narrowing; resolve the actual sensor server-side. Use `isNull()`/numeric-type checks for presence and `isfinite()`/domain bounds for value validation. Test real numeric UIDs, malformed keys, pressure units, and the zero/4 mA anchor. Do not copy nonexistent helper APIs from prior suggested snippets.

#### S-W04 - History Selection, Units, Offline Rendering, and Scope

**P2, confirmed; prior browser evidence retained.** [History page](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2188), [history API](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L17012). Origins: FULL/COPILOT F-19, F-20, F-22; INDEPENDENT R-16; CLAUDE M-24, M-25, M-56.

Reload recreates dropdowns without preserving selected identity. The sensor request never sends the supported `sensor=` filter. Pressure/RPM/flow and tank values share inches formatting and CSV labels; all clients' voltages are joined into one series. Date/site/range filters apply inconsistently. CDN failure throws, clears usable data, and retries the same unavailable renderer. Fix the inaccessible date handler through S-W02.

Recommendation: stable identity across refreshes, a quantity/unit/validity contract, separate voltage series per device, consistent filter scope, and a data-first fallback when charts are unavailable. Derive history coverage from returned timestamps/tier completeness rather than configuration wishes. A JSON transport failure and a chart-library failure must not both become an empty fleet.

#### S-W05 - Responsive Layout, Contacts Grid, and Shared Styling

**P2 for hidden/misaligned data; P3 for cosmetic consistency.** [Shared CSS](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L1630), [contacts rendering](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2216). Origins: FULL/COPILOT F-21; INDEPENDENT R-17; CLAUDE H-14, M-19, M-21, M-22, M-23, M-28, M-29, M-32, L-13, L-15, L-16, L-17, L-26, L-58.

Consolidated issues: unwrapped tables/code and chart minimum widths; `nth-child(7n+3)` hiding the wrong contact column after association rows add children; nowrap tooltips constrained to 300 px; offscreen-transform toasts that can leave a colored strip; inconsistent error toast treatment; referenced CSS variables/classes missing from the shared stylesheet; header/active-navigation drift; large sticky phone headers; and weak status contrast/keyboard semantics.

Recommendation: semantic per-column classes, bounded scroll wrappers, `min-width:0` where required, wrapping tooltips, visibility/opacity-based toast state, one navigation/component vocabulary, resolved chart colors, and keyboard-accessible controls. Do not apply global `overflow-x:hidden` or convert every table to a block without checking accessibility. Prior viewport measurements vary by data, first load versus resize, and viewport size: Calibration's approximately 723 px minimum is well supported; do not claim every other page always fits or always overflows. Re-test 320/375/390/768/1440 px with association chips, long labels, filled tables, dialogs, and desktop-to-phone resizing.

#### S-W06 - Operator Status Can Be Misleading

**P2, confirmed/source-supported.** [Dashboard mapping](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2310), [request state](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2338), [site mapper](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2381). Origins: FULL/COPILOT F-23; INDEPENDENT R-24; CLAUDE M-30, M-31, M-41, L-25, L-51.

Zero digital/tank values can become `--` in the unitless tank formatting branch; digital cards use tank/inches semantics. Client-level `a` aggregates all alarms while `at` can be overwritten by the newest normal sensor. Site Config drops `flt/ma` although its renderer expects them. Pending updates never expire, replacing the Update button and enabling indefinite 10-second polling. The stale calculation is 49 hours while the current template labels 25 hours. Outdated-version counts use inequality rather than older-than comparison. A deletion action still prompts for a PIN despite session-based authorization.

Recommendation: explicit digital state, valid zero, per-sensor alarm identity, complete mapper contracts, a retryable update timeout, one stale policy shared by text and calculation, ordered version comparisons, and session-consistent controls. Stale policy must account for actual report cadence; do not change 49 to 25 hours merely to match one label without agreeing on that policy.

#### S-W07 - Logs, Save Feedback, and Secondary API Usability

**P2.** [Transmission Log](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2208), [settings handler](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L18335), [location query](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L20242). Origins: CLAUDE M-26, M-27, M-57, M-58, M-61, L-18, L-19, L-20, L-21, L-22, L-24, L-42, L-49; INDEPENDENT R-23/R-24.

Retain: log filters miss emitted types/statuses (`error` is not `failed`), table/export search differs, notification logs omit useful identity, save UIs can ignore a returned `saved:false`, text fields truncate silently, location query fails to URL-decode `dev%3A...`, year-over-year history uses the first colon, initialization can duplicate settings fetches, and generated configs do not consistently enforce the eight-monitor cap. Unloaded contacts/schedule data can produce an unintended daily time; a new UID with no snapshot should enter commissioning rather than a generic load error. Login collapses lockout/network/setup errors into Invalid PIN.

Recommendation: derive filters from a stable shared vocabulary, share visible/export filtering, propagate durability status and validation errors, reject overlength fields, use the existing query decoder and stable key parser, and make initialization/commissioning states explicit. Treat the exact login-overlay delay as conditional: existing load handlers can hide it earlier, so a fixed five-second delay on every login is not established.

### Data Ingestion and History

#### S-D01 - Registration Is Dropped Before Client Discovery

**P1, confirmed.** [Telemetry entry guard](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12470). Origin: CLAUDE H-25.

The unconfigured client's intentional `{mc:0,r:"boot"|"heartbeat"}` note has no `k` and is rejected before metadata updates. This is different from the earlier fixture-induced false claim that every configured client is shown unconfigured. A metadata-only patch is also incomplete: `/api/clients` must enumerate registration-only clients, not just sensor records/config snapshots.

Recommendation: validate the UID first, handle registration as a client-lifecycle event with site/firmware/last-contact metadata, preserve the no-phantom-sensor guard for measurements, and include discoverable sensorless clients in the API. Test true commissioning through registration -> selection -> config -> first telemetry.

#### S-D02 - Freshness, Quality, and Event Ordering Need One Contract

**P1, confirmed.** [Telemetry mutation](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12647), [alarm mutation](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12905), [daily processing](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L13296). Origins: FULL/COPILOT F-11, F-12, F-29; CLAUDE M-45, M-46, M-48, M-49, L-37, L-38, L-39; related INDEPENDENT R-13.

Older arrivals can overwrite newer state; receipt and acquisition time are conflated. Fault-only or diagnostic `rd` notes can resolve to zero and replace last-good values. Daily ingestion does not mirror fault handling, persistence drops transient quality, and valid zero snapshots are excluded by positivity guards. The dashboard's 24-hour delta updates its baseline from gaps between consecutive arrivals, so frequent telemetry can preserve a much older baseline. Unload can replace the label with default Tank, omit dirtiness, and never mark SMS success; alarm/unload identity checks are weaker than telemetry/daily.

Recommendation: separate last-good acquisition, latest receipt, quality state, and alarm transition ordering. Historical insertion must not mutate current state. Match inbound `fault/lvl/ma/rd` semantics deliberately; `rd` is not automatically a valid measurement. Derive the 24-hour comparison from time-bracketed history; stamping an old value with a new baseline time is not a fix. Validate sensor identity and preserve absent optional labels. Test reorder/replay, faults, diagnostic-only events, daily recovery, zero, unload, and reboot.

#### S-D03 - Warm History and Archive Manifests Lose Data at Size Boundaries

**P1, confirmed.** [Daily rollup](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L8100), [archive manifest](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L16535). Origins: CLAUDE H-23, M-54, M-55, L-29.

An existing month of at least 8,192 bytes is not loaded and can be overwritten with only yesterday's rows; parse/allocation/read failure can cause the same outcome. The archived-client index is read through about 2 KiB, parse results are ignored, and replacement removes the previous file before rename. The exact number of archives before failure depends on entry sizes, not a universal seven-entry limit. Rolling up only yesterday also misses older unprocessed days still in hot storage.

Recommendation: distinguish absent from unreadable; abort without replacing any existing file on load failure. Use bounded per-day files or a streaming/indexed layout and atomic activation; make backfill idempotent. Do not discard oldest days inside the current month as an automatic response if retention promises require them. Test boundary sizes, long site names, partial reads, allocation failure, and multiple missed rollup days.

#### S-D04 - Retention, Monthly Completeness, and Peak Memory Are Overstated

**P2; allocation pattern confirmed, exact capacity failure requires measurement.** [History structures](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L758), [archive generation](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L7719), [history serialization](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L17012). Origins: CLAUDE H-26, M-39, M-56; FULL/COPILOT F-20 and performance recommendations.

Ninety snapshots are not 90/730 days; warm retention settings can exceed the hard-coded three-month prune. Month archival prefers whatever overlapping hot samples remain and consults warm summaries only when none remain, yielding partial-month archives. History/snapshot operations construct growing JSON plus serialized copies within approximately 163 KiB remaining after server static allocations.

Recommendation: report actual earliest/latest data, snapshot capacity, enforced retention, and complete/partial status. Prefer complete daily rollups for monthly archives, with explicit merging/deduplication. Stream bounded responses and avoid simultaneous whole-document copies. Check allocation/overflow before file replacement and instrument heap under full-fleet fixtures. Do not present an estimated 170 KiB allocation or mid-size-fleet crash as a measured result of this consolidation.

#### S-D05 - Calibration and Weather History Are Not Reliable Reference Data

**P2, source-supported.** [Weather reads](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L6442), [calibration data](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L19234). Origins: CLAUDE M-37, M-38, M-59, L-50.

NWS body reads stop at the first empty receive buffer instead of complete HTTP framing; response status/chunking/size and failure caching are inadequate. That can truncate multi-packet responses, but source alone does not prove every request always fails. Calibration reads/fits use the earliest bounded log entries while the file grows, and quality/range metrics are not restored/recomputed consistently after reboot. The weather averaging code also ignores the supplied calibration timestamp when selecting values, so it must not be represented as verified historical observations.

Recommendation: use a bounded, complete HTTP/JSON response path with failure TTL; distinguish forecast/reference data and actual sample time. Retain a deliberate recent/representative calibration window, persist or recompute quality metrics, and test log rollover, new calibrations after capacity, packet gaps, failed weather service, and boot restoration.

### Alarms, Delivery, and Remote Commands

#### S-T01 - Stable Relay Identity Requires an Explicit Protocol Migration

**P1, confirmed.** [Dashboard mapping](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2310), [command builder](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11197). Origins: FULL/COPILOT F-02; INDEPENDENT R-02; CLAUDE prior F-02/R-02.

Telemetry-array position is sent as the client's monitor-array index; reordered/sparse sensor arrivals select the wrong monitor. Retain stable sensor-number intent through UI -> API -> command -> client lookup. Use a new key such as `relay_reset_sensor_number` or an explicitly versioned command, with a capability-aware old-client path. Unknown identity must fail closed, never fall back to another interpretation. Test numbers 1 and 2 specifically, because they overlap valid legacy indices.

#### S-T02 - Notification Eligibility Must Belong to the Current Alarm Episode

**P1/P2 according to alert policy, confirmed.** [Alarm and rate-limit paths](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12905), [snooze/reminders](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L14566). Origins: FULL/COPILOT F-09, F-15; INDEPENDENT R-10/R-13; CLAUDE M-51, M-52, L-45, L-47.

Relay timeout overwrites the condition type, disabling reminders while an alarm remains active. A lifetime `lastSmsAlertEpoch` can enable reminders for a later excursion that did not request SMS. Snooze broadcasts before a save attempt whose failure is invisible. Duplicate registry merge can discard newer snooze/rate-limit fields. Daily-email failure can advance the schedule with only a serial error.

Recommendation: separate condition, relay event, episode eligibility, queued delivery, and durable reminder anchors. Preserve unsnooze's full-interval delay, idempotency, recipient scope, and automatic clear. Persist before confirming a durable state change; handle notice delivery separately so a successful save is not confused with SMS receipt. Do not roll back only the snooze boolean while leaving an unsnooze anchor or audit side effect changed. Test suppressed new episodes, timeout, duplicate notes, reboot, failed saves, and bounded failed-email retry.

#### S-T03 - Config Revision, ACK, and Retry Semantics Can Misreport Success

**P1, confirmed; deployed incidence unmeasured.** [Revision sender](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11914), [ACK handling](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L16255). Origins: FULL/COPILOT F-10; INDEPENDENT R-09; CLAUDE H-01, M-42, M-43.

Fractional `_ts` is incompatible with the client's integer default operator, disabling ordering for such notes; integral same-second revisions still collide. A matching `cv` clears pending even for failed status, while stale applied ACKs can prune against newer desired config. Hourly retries can mistake a long inbound cadence for loss and enqueue duplicates or declare failure before the client has polled.

Recommendation: persist a monotonic revision plus content identity; validate finite/range/type before narrowing; require matching applied-and-persisted ACK for completion/pruning. A content hash alone proves equality, not ordering. Account for client poll/power cadence when retrying, and distinguish queued, delivered, applied, and persisted. Test fractional/integer timestamps, equal-second different configs, duplicate hashes, stale ACK, storage failure, and long-sleep clients.

#### S-T04 - Note Consumption Needs Idempotency and Observable Failure

**P2, confirmed.** [Inbound processor](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12160). Origins: FULL/COPILOT F-24; INDEPENDENT R-19; CLAUDE M-47, L-35, L-36.

Handlers execute before a separate delete whose errors are ignored; replay can duplicate side effects, while void handlers can consume a failed write. Twelve poison slots cover thirteen inboxes. Daily reconciliation runs before new sensor upserts, so a missed alarm on a previously unknown sensor is not recovered on that pass. Several command builders leak the request on body-allocation failure; system-alarm composition lacks a safe message for unexpected `se:true` sunset/I2C types.

Recommendation: structured applied/retryable/rejected results, durable idempotency at the side-effect boundary, inbox-derived tracker sizing, validate/upsert before reconciliation, allocation cleanup, and exhaustive message initialization. A small RAM recent-ID ring is only a transient optimization: it does not prevent duplicate delivery after reboot and must not mark failed processing complete. Test all inboxes, malformed notes, allocation/delete failure, restart after queueing, and first-ever daily alarm discovery.

#### S-T05 - Contacts, Opt-Outs, and Delivery Metadata Need Consistent Rules

**P2/P3.** [Recipient resolution](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L14170), [SMS reply handling](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L13949), [daily email](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L14769). Origins: CLAUDE M-50, M-53, L-40, L-41, L-42, L-43, L-44; earlier recipient/persistence recommendations.

Retain: inconsistent phone canonicalization can miss STOP/SNOOZE matches; server-only SMS enrollment omits the welcome path; unknown START senders can generate replies; bounded opt-out eviction can lose real consent state; daily recipients lack equivalent dedupe/placeholder handling; UTC subject date and ignored format options disagree with local scheduling; stale-alert flags reset on reboot; delivery logs lack identity. Carrier STOP enforcement is an additional layer, not proof local opt-out state is correct.

Recommendation: validate/canonicalize at enrollment with an explicit country-code policy, retain consent durably without silent eviction, scope replies, dedupe channels, and expose queued versus delivered state. Persist episode-level stale/recovery notifications. Batch recipient queueing only if urgent alert latency and provider requirements remain satisfied.

### Persistence, Scheduling, and Maintenance

#### S-O01 - Save Contracts Lose Empty State and Starve Metadata

**P1, confirmed.** [Registry persistence](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L15612), [loop gates](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L4810). Origins: FULL/COPILOT F-08/F-14; INDEPENDENT R-05/R-12; CLAUDE M-35 and prior-item confirmations.

Zero-count saves no-op, so deleting the last client leaves old disk state. Void saves cause dirty flags to clear on failure. Registry timing resets the shared clock before metadata checks it, allowing starvation. Config save failure can retry every loop; not all critical alert-limit state is persisted. Raw pointers into a registry compacted by stale pruning can then refer to another sensor/site (CLAUDE L-48).

Recommendation: return outcomes, write empty arrays, preserve retryable dirtiness, schedule both owners independently or under one due decision, back off failures, and copy identifiers before compaction. Existing atomic helpers are useful; do not replace them with remove-then-rename. Test last-delete/reboot, failure injection, continuous registry traffic, pointer invalidation, and bounded flash retry.

#### S-O02 - Restore Is Partial, Capacity-Mismatched, and Not Coherently Activated

**P1, confirmed; exact socket failures hardware-dependent.** [Restore pipeline](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L7275), [client-cache restore](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L6816). Origins: FULL/COPILOT F-13; INDEPENDENT R-11; CLAUDE H-22, H-24, H-27.

Backup/restore size limits differ (about 24 KiB versus 2 KiB for baseline files). Any-file success masks required-file failure; per-client reconstruction can replace an intact cache with an empty/partial one and drops ACK/dispatch fields. Runtime owners/caches are not all reloaded, and restored network/Notecard settings are not coherently activated. Restore does not follow the backup's socket lifecycle/cooldown handling.

Recommendation: stage a versioned manifest, validate every required object and capacity, preserve untouched data on failure, activate as one coherent generation, then reinitialize/reload or reboot deliberately. Merely adding reload calls or clearing all dirty flags cannot make a partial restore transactional. Test missing client downloads, large manifests, invalid schemas, interrupted activation, and the physical socket limit.

#### S-O03 - Cooperative Maintenance, Scheduling, and HTTP Budgets

**P2, confirmed blocking structure; durations bounded by configuration/transport.** [Main loop](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L4509), [time sync](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L8667), [HTTP parser](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L9696). Origins: FULL/COPILOT F-07/F-17; INDEPENDENT R-08/R-20; CLAUDE M-34, M-40, M-44, L-27, L-30, L-31, L-32, L-33.

Server schedules remain zero after late first sync. Failed time requests lack backoff. Thirteen inbox peeks every five seconds, blocking external requests, and whole-file FTP work delay normal service. A complete nine-file FTPS pass includes eight 65-second waits (520 seconds), even before transfer time. Watchdog kicks do not service alarms. HTTP parsing permits incomplete headers/bodies to time out and still return success; body reads are bytewise. Some refresh callers request full config payloads unnecessarily; small pages are copied before streaming and CSS uses small writes.

Recommendation: explicit first-valid-clock arming; budgeted queue/drain and maintenance states; truthful asynchronous job status; complete HTTP framing and idle deadlines; bulk/streamed bounded I/O; summary-only responses for summary consumers. Retain the physical socket constraint when scheduling backup, and preserve safe watchdog budgets inside every blocking step. One file per loop is not sufficient if that file operation itself blocks for minutes. CSS already has a public one-hour cache policy; improve validators/versioning rather than reporting it wholly uncached.

#### S-O04 - Settings Transactions and Administrative Security

**P2, confirmed source paths; deployment exposure conditional.** [Settings](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L18335), [auth](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L9238). Origins: FULL/COPILOT F-28; INDEPENDENT R-18; CLAUDE M-20, M-33, M-36, L-14, L-23, L-28, L-34.

Validate all fields before mutating runtime config: invalid viewer addressing currently returns 400 after other changes. Apply length/type/range validation before string copies. Global login backoff can block authenticated admin actions. ADC/timing-seeded tokens and no explicit expiry are hardening concerns. Some HTML-attribute builders use text-only escaping; decoded export parameters can reach response headers; the login redirect check needs normalized same-origin validation. Backups contain the PIN and reversible credential encoding, and LAN HTTP provides no confidentiality.

Recommendation: stage/validate/persist/apply settings, separate login abuse controls from valid sessions, use platform randomness and bounded sessions, DOM properties/event listeners instead of mixed-context string escaping, CR/LF-safe filenames, and trusted network/TLS deployment. Validate malicious strings only in isolated fixtures. Preserve existing HttpOnly/SameSite and constant-time checks; the JS literal `cookie` is not the secret session token.

## Client

### Sensor Acquisition and Control

#### C-A01 - Map Physical Outputs and Input Terminals Explicitly

**P1, confirmed against committed source and installed Opta core 4.5.0; physical acceptance test required.** [Relay mapping](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L8693), [digital reader](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5457). Origins: CLAUDE H-04, H-11, H-12.

`getRelayPin` uses `LED_D0 + index`, producing 7/8/9/10. Core definitions identify relay coils as D0..D3 (0/1/2/3), and status LEDs as 7/9/8/153. This is not merely a relay-2/relay-3 swap. The configured I1..I8 values 0..7 are also passed directly to digital pin functions; they should resolve to the physical input terminals, not D0..D7. Analog channel mapping is a separate API contract.

Recommendation: explicit tables for physical inputs and relay coils, separate LED indication, strict terminal validation, and no generic `pin < 255` acceptance. Audit setup/reinitialization/pulse/clear-button paths, not only the main reader. Bench gate: verify each of four contacts and eight inputs individually without connected process loads. A software pin-table test is necessary but cannot certify wiring, polarity, or field safety.

#### C-A02 - Unify Relay Ownership, Modes, and Reconfiguration

**P1, confirmed logic; physical consequences depend on C-A01 and the target device.** [Alarm GPIO path](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L6316), [config mutation](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4774). Origins: CLAUDE H-02, H-05, H-07; related FULL/COPILOT F-05 and S-T01.

`activateLocalAlarm` writes GPIO directly outside relay-mode/runtime ownership, including a legacy monitor-index fallback when no mask is configured. A normal clear can bypass MANUAL_RESET semantics. Removing a monitor clears its sensor runtime but does not reliably release its local relay owner or send remote OFF using the old config. Remaining-monitor hardware reinitialization does not make removed remote outputs safe. First-sample persistent-relay restoration latches without the normal debounce/notification path.

Recommendation: one actuator state owner; indicator output separate from control; explicit deactivation/reassignment using the old configuration before replacing it; no implicit relay mapping for unconfigured monitors. Decide whether boot restoration is immediate safety action or debounced control, but still report the active condition. Test shared masks, removal/reorder, changed target, manual reset, until-clear, momentary timeout, and boot-in-alarm. Do not turn an LED-mapping repair into immediate field actuation without testing the now-exposed state logic.

#### C-A03 - Pulse Acquisition Must Observe the Whole Measurement Window

**P1 for pulse-dependent control, otherwise P2; confirmed.** [Pulse state machine](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L1268), [read entry](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5695). Origins: FULL/COPILOT F-06; INDEPENDENT R-14; CLAUDE M-02 and prior-item evidence.

The sampler is serviced from periodic sensor reads rather than continuously through its window. Incomplete acquisition returns a previous value; time-based mode can retain old RPM indefinitely when a machine stops. Calling the poller once at the top of the existing loop is not a complete fix: sleeps, Modbus, Notecard and replay still leave long blind intervals.

Recommendation: timer/interrupt-backed acquisition appropriate to the supported pulse rates, with minimal ISR work and atomic snapshots; independently schedule result publication. Define no-pulse timeout, freshness, and expected maximum rate. Test stationary/slow/fast input, long loop blocks, rollover, power modes, and zero speed. Preserve pulse capture while considering sleep changes.

#### C-A04 - Sensor Failure and Recovery Need Fresh, Sensor-Specific Evidence

**P2, confirmed; sensor-specific policy required.** [Validation](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5294), [sampling](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5748). Origins: CLAUDE H-03, M-09, M-11, L-06; FULL/COPILOT F-12.

Stuck detection can classify a legitimately steady float switch/stopped engine as failure when enabled. Recovery counters are not reset by every bad acquisition. One global current-loop fault code is reused across monitors, so deferred telemetry can report another sensor's reason. Validation can publish diagnostics during the acquisition phase despite its no-Notecard-I/O claim. These are state/sequence issues, not proof of physical simultaneous I2C-master contention: the Notecard is a slave.

Recommendation: per-sensor quality/reason/timestamps, consecutive independent recovery samples, sensor-appropriate stuck policy, and deferred diagnostic publication after the acquisition phase. Test mixed good/faulted channels and stable legitimate values. Keep known-good last measurements separately from current health.

#### C-A05 - Analog Scale, Acquisition Delays, and I2C Timeout Assumptions

**P2; scale magnitude requires bench verification.** [Analog/current readers](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5487), [I2C helpers](../TankAlarm-112025-Common/src/TankAlarm_I2C.h). Origins: CLAUDE M-10, L-07, L-56.

The analog conversion assumes 10 V equals full ADC scale; compare the board front-end/reference calibration before accepting the review's approximately 8% error as a universal value. In committed current-loop code, external-power sample delays occur without the loop-powered watchdog kicks and are not coherently capped. Core MbedI2C inherits Stream timeout configuration but its `requestFrom`/`endTransmission` directly call Mbed transfers; `Wire.setTimeout` is not the claimed hardware transaction deadline.

Recommendation: board-specific conversion with reference measurements, bounded settling/sample delays with watchdog service in every power mode, and verified driver-level transfer/recovery limits. Do not increase Stream timeout or change bus clock and claim that fixes hardware hangs. Preserve validated DAC bipolar versus externally powered unipolar conversion and settling sequences.

### Alarm Evaluation and Delivery

#### C-T01 - Consecutive Debounce and Durable Notification-Pending State

**P1, confirmed.** [Alarm evaluation](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5872), [rate limiter](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L6210). Origins: FULL/COPILOT F-04/F-05; INDEPENDENT R-03/R-04; CLAUDE H-06, M-12.

The analog counter-reset gaps admit nonconsecutive spikes and clears. Latching before a rate-limited send leaves no pending first notification. Boot timestamps clamped to zero can suppress the first five minutes. Prior function-derived probes demonstrated `[90,50,90,50,90]` latching with high=80 and `[70,78,70,78,70]` clearing with hysteresis=5, debounce=3.

Recommendation: independent trigger/clear counters reset on disqualification, explicit policy for stale samples, first-send handling, and a bounded pending-delivery record distinct from actuator state. Do not just add a pending boolean with no retry/clear lifecycle. At 30-minute sampling, three fresh samples imply roughly 60-90 minutes detection latency; agree on required alarm latency separately from upload frequency.

#### C-T02 - Remote Commands Need Delivery and Freshness Guarantees

**P1 for OFF/safety commands; confirmed.** [Relay polling/forwarding](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L8798). Origins: CLAUDE H-13, M-15, M-16; S-T01.

Remote relay forwarding uses direct Notecard calls and skips while unavailable even at committed HEAD. The receiver drains one command per inbound cycle, making ON/OFF sequences wait through configured cadence and power multipliers. Existing cooldown can consume a command without action; indiscriminately draining faster would expose that more often.

Recommendation: bounded, idempotent desired-state commands with command IDs, expiry, acknowledgments, and retry outcomes; prioritize valid OFF/safety transitions. Drain under time/quantity budgets and align cooldown with coalescing, not silent drop. Stale buffered ON commands should not execute after a newer OFF. Support the explicit stable-sensor protocol from S-T01 without ambiguous fallback.

#### C-T03 - Replay Order and Capacity Must Match the Publisher

**P2, confirmed.** [Publisher/replay](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L8164). Origins: FULL/COPILOT F-11/F-16; CLAUDE M-14; prior July reviews.

New live notes can enter the outbox before older buffered events. Dynamic publisher payloads can exceed the fixed 2,304-byte replay-line capacity; truncated lines are skipped. Count-bounded replay may still monopolize the loop for many blocking transfers. The larger line buffer corrected an older smaller cap, not the unbounded-producer mismatch.

Recommendation: a durable record format and unified limits, explicit oversize/split policy, sequence-aware replay/coalescing, and wall-time budgets. Preserve alarm/clear ordering and idempotency; do not simply replay a backlog of obsolete control commands. Collapse intermediate routine telemetry only when it does not erase transitions, compliance records, or required history.

### Configuration, Power, and OTA

#### C-P01 - Numeric Configuration Must Be Validated Before Narrowing

**P1/P2 by affected field; confirmed.** [Config parsing/application](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4774), [solar configuration types](../TankAlarm-112025-Common/src/TankAlarm_Solar.h#L253). Origins: CLAUDE H-01, M-06, M-08, M-42, L-19, L-57; FULL/COPILOT F-10.

The UI's 86,400 seconds cannot fit in `uint16_t` sample/duration fields; a clamp after narrowing is ineffective. 115200 baud cannot fit `uint16_t`. Float `_ts` with integer defaulting disables ordering for that input type. Too many sensors are silently truncated.

Recommendation: parse a wide numeric type, reject nonfinite/nonintegral/out-of-range values as appropriate, then narrow; align UI/backend/client constraints and persisted schemas. Widen seconds/baud where the supported range requires it. Preserve monotonic revision plus content identity from S-T03, not a nonexistent `gConfig.configVersion` field. Test 0, bounds, just-over-bounds, 24 hours, fractional values, 115200, and nine sensors through save/apply/reboot.

#### C-P02 - Battery Alerts Can Repeat Without a Bounded Episode Policy

**P1 when battery/data-constrained; confirmed with source-dependent conditions.** [Battery alert path](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L6958), [solar manager](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L758). Origins: CLAUDE H-08/H-09 (one issue), M-05, H-29, L-52.

Battery CRITICAL bypasses the interval every poll. This path is relevant to the Vin/battery monitor: it explicitly defers when MPPT is the source, so not every solar client follows it. Solar severity flapping bypasses same-type timing. BatteryConfig chemistry/pack scaling is not consistently carried into SolarConfig and fixed power thresholds; a 24 V bank can be evaluated against 12 V limits. Human battery labels also do not align with alert bands.

Recommendation: immediate first critical event plus bounded reminders, hysteresis and episode state, one chemistry/nominal-voltage authority with explicit overrides, and consistent labels. Test the low/critical gap above hibernate entry, two voltage sources, 12/24 V, supported chemistries, noise, and recovery. Do not lengthen critical-notification latency to claim energy savings.

#### C-P03 - Power Transitions Must Use Fresh Voltage and Complete Side Effects

**P2, confirmed.** [Power state machine](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7649). Origins: FULL/COPILOT F-18; INDEPENDENT R-15; CLAUDE H-10, M-13, L-05.

Debounce counts loop iterations on cached voltage. Unknown voltage or configuration reinitialization directly sets NORMAL, bypassing exit effects and potentially restarting transition notices. Battery-failure mode loads persisted state after setting its new flag; an older false flag can undo the transition and permit repeat announcements.

Recommendation: explicit fresh/unknown voltage and proposed-state tracking, a single transition function, restore-before-mutate ordering, and documented relay policy on uncertainty. Reusing one cached ADC sample three times is not three confirmations. Test source loss in critical state, fresh sample recovery, config apply during low battery, and persisted false/true fallback state. Do not hold hibernate forever on an unknown sensor and thereby break remote recovery.

#### C-P04 - Solar State Is Stored on the Wrong Logical Volume

**P1/P2 depending on solar-only recovery requirements; path mismatch confirmed.** [Solar state path](../TankAlarm-112025-Common/src/TankAlarm_Battery.h#L312), [state load/save](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7220). Origins: CLAUDE H-28/M-62 (one issue).

Client application storage uses `/cfg`, while `SOLAR_STATE_FILE` uses `/fs`, the OTA FAT volume. The LittleFS-oriented replacement helper is then used against that other filesystem. The exact claim that every save after the first fails depends on the mounted FAT rename behavior; the incorrect ownership/mount dependency does not.

Recommendation: store app state on its owning app partition and migrate any readable old state deliberately. Check close/write/rename outcomes; do not format a volume to repair a path mistake. Test first/repeated saves, OTA mount absent, migration, brownout, sunset state, and reboot.

#### C-P05 - Recovery, Health Polling, and Energy Budgets

**P2/P3, source-supported; measured savings pending.** [Loop](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L1923), [health helper](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4041), [solar stats](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp). Origins: CLAUDE M-03, M-04, M-07, L-02/L-03, L-04, L-08, L-09, L-10; working-tree W-03.

Retain: normal health timer only probes when already marked unavailable; `sync_request` is polled repeatedly during a two-second window; time acquisition lacks attempt backoff; routine trim probes can run without new payloads; software daily extrema are not consistently reset; power-state outbound multiplier constants do not change autonomous modem cadence. HEAD's real-read recovery watermark can be fooled by resetting that counter for daily reporting, while the dirty working copy has the larger unconditional-reset regression.

Recommendation: separate lifetime/acquisition evidence from resettable statistics, maintain one poll deadline per task, use slow health checks based on actual connectivity evidence, and budget modem/Modbus work by freshness requirements. Caution: moving the existing health helper onto an always-running timer without changing its four-hour "no successful note.add" stall heuristic can reboot a healthy modem when zero-deadband telemetry is intentionally quiet. Distinguish host queue acceptance from cloud delivery and legitimate silence. Inventory payload consumers, including Notehub diagnostics/external routes, before removing supposedly unused fields. Do not skip overnight battery-recovery polls.

#### C-P06 - OTA Trial State, Health Confirmation, and Build Guarantees

**P2, conditional interruption risk.** [OTA state machine](../TankAlarm-112025-Common/src/TankAlarm_DFU.h#L1282), [client startup](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L1565). Origins: CLAUDE M-63, L-55; FULL/COPILOT W-04 and provisioning recommendations.

Writing `pending_ota.json` as trial before successfully scheduling the swap creates a reset window that can be mistaken for rollback and blacklist a valid image. Early unconditional MCUboot confirmation is an explicit existing choice; it should not be described as a full post-peripheral health gate. The unused in-place IAP updater is not an active OTA failure solely because its code remains in a header.

Recommendation: distinguish prepared/swap-scheduled/trial/confirmed outcomes with durable checks and bootloader evidence, and test reset at each transition. Preserve the required MCUboot build flag and matching slot/link/keychain recipe. Any change to confirmation timing must avoid making normal offline startup roll back indefinitely. No provisioning, swap, or power-cut test was executed for this consolidation.

#### C-P07 - Existing Working-Tree Regressions Are a Separate Release Gate

**P1, confirmed difference from HEAD, not automatically authorized to restore.** Origins: FULL/COPILOT W-01..W-04; INDEPENDENT R-04/R-06; CLAUDE section 1.

The current working client/Common remove DAC loop-power fields/helpers/conversion, reinstate offline alarm gates, remove the actual-success recovery watermark and setpoint probe cap, remove telemetry-request polling and daily acquisition timestamps, and weaken the mandatory OTA build guard. The same firmware version then describes different behavior. Current server source matches committed HEAD.

Recommendation: reconcile each intended edit against HEAD with the owner, not `git add -A`, wholesale checkout/reset, or an unreviewed merge. Keep both baseline build results distinct. The conditional CI overlay step references a vendor directory no longer present; absence does not prove the upstream FTPS dependency lacks the required capability.

## Viewer

### Website and Display Fidelity

#### V-W01 - Match Measurement Quality, Staleness, Units, and Status

**P2, confirmed omissions.** [Viewer pages](../TankAlarm-112025-Viewer-BluesOpta/TankAlarm-112025-Viewer-BluesOpta.ino#L308), [server summary](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L14957). Origins: FULL/COPILOT F-12 and parity suggestions; CLAUDE L-46, L-58, L-59, L-62.

Viewer data omits current fault/snooze information and differs in voltage freshness rules; the UI lacks equivalent sensor-age warnings and has a separate style vocabulary. Printed reports format all readings as feet/inches and can print epoch-zero as 1970. Raw mA/voltage payload fields are sent but unused by the viewer.

Recommendation: share the semantic measurement/status contract with the server, include necessary quality metadata, and display actual quantity/unit and last acquisition. Derive theme/navigation tokens without forcing identical interaction density on a kiosk. Print unavailable time explicitly. Remove or expose unused raw fields only after confirming other consumers and compatibility.

#### V-W02 - Failed Contact Saves Leave Optimistic State Without Recovery

**P2, source-supported.** [Viewer contacts page](../TankAlarm-112025-Viewer-BluesOpta/TankAlarm-112025-Viewer-BluesOpta.ino#L310). Origin: CLAUDE L-60.

The save promise can reject without a recovery handler while the local displayed list has changed. Recommendation: staged edits, disabled duplicate submission, explicit queued/acknowledged/failed status, and rollback/refetch when delivery fails. Test network rejection, server-side rejection, duplicate save, and a later authoritative summary that differs from local edits.

### Summary Delivery, Contacts, and Networking

#### V-T01 - Viewer Writes Cross the Administrative Trust Boundary

**P1 on an untrusted/public LAN; confirmed unauthenticated mutation.** [Viewer routes](../TankAlarm-112025-Viewer-BluesOpta/TankAlarm-112025-Viewer-BluesOpta.ino#L969), [server contact adoption](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L15088). Origins: CLAUDE H-31/H-32 (one issue); FULL website/security recommendations.

Viewer POST routes can replace the viewer roster or request cellular updates without authentication. Server adoption replaces all viewer-category contacts and auto-enrolls recipients; an empty list removes them. The README's read-only/public-area claim is therefore false. Multiple viewers also share a last-writer-wins subset rather than explicit ownership.

Recommendation: choose and document trusted-LAN editing, authenticated scoped editing, admin-approved requests, or truly read-only viewer operation. Protect both write routes and enforce ownership/revision/consent server-side, not only in JavaScript. Authentication alone does not resolve multi-viewer lost updates. Preserve privacy and opt-out state across roster replacement.

#### V-T02 - Summary Schedules Can Stay Unarmed or Consistently Late

**P2; schedule gap confirmed, fixed 6-12-hour lag claim qualified.** [Viewer loop/scheduler](../TankAlarm-112025-Viewer-BluesOpta/TankAlarm-112025-Viewer-BluesOpta.ino#L697), [contact POST](../TankAlarm-112025-Viewer-BluesOpta/TankAlarm-112025-Viewer-BluesOpta.ino#L1298). Origins: INDEPENDENT R-08; CLAUDE M-65, M-66, L-61.

Late first clock sync does not arm a zero fetch schedule. Fetching at the same aligned time the server publishes can consistently retrieve the prior summary because delivery is asynchronous, but the actual lag depends on timing and interval; permanence is not established. Contact save does not arm the same bounded fast-poll window as Request Update.

Recommendation: first-valid-clock arming, bounded regular inbound checks or post-publication retry with generation tracking, and fast polling after queued edits until a matching acknowledgement/summary arrives. Do not let any unrelated old summary prematurely declare a new request complete. Test boot without time, delayed delivery, contact echo, and missing server replies.

#### V-T03 - Network Reconfiguration Needs Explicit Lifecycle and Recovery

**P2, confirmed missing lifecycle steps; exact Ethernet result hardware-dependent.** [Profile application](../TankAlarm-112025-Viewer-BluesOpta/TankAlarm-112025-Viewer-BluesOpta.ino#L522), [bring-up](../TankAlarm-112025-Viewer-BluesOpta/TankAlarm-112025-Viewer-BluesOpta.ino#L849). Origins: CLAUDE H-30, M-64.

Profiles call network initialization and server begin without explicitly closing the existing listener/interface. Repeated `Ethernet.begin` can fail to apply changed addressing; the installed core's begin loop can wait up to 60 seconds without application watchdog service. However, the static overload does call `set_network`, so "only stores the profile and can never apply it" is too absolute without the network stack/board result. Revision deduplication itself is present.

Recommendation: close/release the listener, disconnect/reconfigure/reconnect under bounded deadlines, verify the applied address, and restart serving or fall back explicitly. Preserve rollback/recovery access when a new address is invalid. Test DHCP absence, cable loss, static -> DHCP -> static, repeated identical revisions, failed persistence, and socket reuse.

#### V-T04 - GitHub Update Metadata Uses the Wrong Response Shape

**P2, confirmed against documented Notecard contract; live deployment behavior not tested.** [Viewer GitHub reader](../TankAlarm-112025-Viewer-BluesOpta/TankAlarm-112025-Viewer-BluesOpta.ino#L1614), [server equivalent](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L3642). Origin: CLAUDE M-67.

Both ask `JGetString(...,"body")` for a JSON object response. The [official web request reference](https://dev.blues.io/api-reference/notecard-api/web-requests/) documents object `body`, base64 `payload` for binary, proxy route/name, and `seconds` for timeout. The viewer's bare URL/`timeout` assumptions also require correction or firmware-specific verification. Version inequality is not a downgrade-safe update test; viewer direct installation is explicitly a stub.

Recommendation: parse the actual object or deliberately requested/decoded payload, use supported request members and a configured route, validate shape/errors/size, and compare versions monotonically. Test canned object/string/error/oversize responses and maintain the existing fallback policy without claiming direct installation works.

## Shared Platform and Repository Tooling

#### X-R01 - Reproducible Builds and Regression Tests

**P2/P3.** [CI](../.github/workflows/arduino-ci-112025.yml), [release](../.github/workflows/release-firmware-112025.yml), [preview automation](../.github/workflows/update-screenshots.yml), [HTML utility](../TankAlarm-112025-Server-BluesOpta/update_html.py). Origins: FULL/COPILOT F-27; CLAUDE M-01, M-17, L-01, L-12, L-53, L-63, L-64.

Pin core/library/external repository versions and keep CI/release options consistent. Test version/build-sequence synchronization, MCUboot image format/link target and supported release assets; FTPS Test has no matching published release asset despite the UI target. Do not publish an incompatible test image just to satisfy a selector.

The HTML utility leaves concatenated C++ delimiters in standalone extraction. Screenshot automation's auth markers, missing API fixtures, skip-on-missing behavior, and lack of runtime/mobile assertions are not adequate website tests. A proper generator can retain maintainable source and emit Arduino-compatible strings while checking per-page handler scope, contracts, identities, and round-trips. ZIP timestamp churn and competing auto-commits warrant reproducible packaging/concurrency handling; whether a ZIP guard can *never* fire is stronger than necessary. Prefer release/CI artifacts for generated binaries/screenshots, but do not rewrite repository history during this review.

#### X-R02 - Documentation, Published Artifacts, and Trust Model

**P3, with security implications where documentation drives deployment.** [Root guide](../README.md), [Common contract](../TankAlarm-112025-Common/src/TankAlarm_Common.h), [Pages deployment](../.github/workflows/deploy-gh-pages.yml). Origins: INDEPENDENT R-21/R-22; CLAUDE M-18, L-01, L-11, L-12, L-54; FULL documentation/security recommendations.

The root guide still advertises v1.9.3, obsolete RAM/client limits, incomplete dependencies, wrong notefile direction/name, and read-only viewer capabilities. Define one release/contract source of truth and mark old plans historical. Check what Jekyll actually publishes before declaring every tracked scratch capture publicly served; review its exclusion rules and resulting artifact. Do not reproduce live device data, secrets, or obsolete build recipes in the master.

Public Arduino default signing keys are an explicit project authenticity trade-off, not a newly discovered leaked secret. Document the difference between image integrity/rollback and authenticity. Keep protected networks/backups and distinguish local test credentials from production.

#### X-R03 - Provisioning and Diagnostic Utilities

**P2, confirmed source risks; never exercised on hardware here.** [Key provisioning](../TankAlarm-112025-KeyProvisioning/TankAlarm-112025-KeyProvisioning.ino#L215), [OptaView](../OptaView/OptaView.ino#L86), [FTPS helper](../TankAlarm-112025-FTPS_Server_Test/ftps_server.py). Origins: FULL/COPILOT F-25/F-26; CLAUDE prior appendix.

Key readiness ignores internal flash programming return values/readback and can rebuild more partition metadata than necessary. Do not use a proposed QSPI error message or an unsupported FlashIAP read method for the internal key region. OptaView needs exact Modbus length/slave/function/echo validation before decode and signed-current scaling only on signed current registers, not voltage. CRC alone is not a complete response contract. Probe configuration and write-readback should be explicit. The Python FTPS helper exposes all interfaces with known test credentials/full permissions by default; confine it to an isolated test network or explicit binding.

Recommendation: checked platform APIs, readback through the actual installed interface, conservative partition preservation and explicit destructive confirmation; shared validated transport/unit logic in diagnostic code where appropriate. The unused legacy in-place IAP function is maintenance debt, not an active production route unless a caller is introduced.

#### X-R04 - Suggestions Requiring Measurement or Further Evidence

**Conditional/open, not release blockers solely on these claims.** Origins: CLAUDE section 9 and efficiency sections; other review recommendations.

- The exact analog conversion error, Ethernet reconfiguration failure mode, FAT overwrite behavior, watchdog exposure of specific library calls, peak heap usage and fleet threshold, voltage debounce wall time, modem power, and traffic savings need their respective runtime/bench evidence.
- A viewer-summary size limit should be explicit, but actual maximum fleet payload versus supported Notecard size needs a serialized maximum-case test. Retain this as a sizing gate, not a proven real-fleet outage.
- Whether the CI product UID is present in every release artifact was disputed. Verify in controlled CI without printing the secret; no product-UID failure is asserted here.
- The rejected calibration-log concatenation claim is not reinstated. Truncation/retention should still have tests, but do not equate a bounded note string with proof that subsequent records concatenate.
- Compiler warnings merit triage: integer width and unsupported range warnings are actionable; bounded truncation can be intentional. Deprecation is not a present compile failure. An unused source symbol is not proof the linker retained it in the binary.
- Do not infer cellular data savings from HTTP/CSS compression on the browser-to-server LAN path. Do not infer energy savings from a CPU sleep interval while the modem, regulators, sensors, and relays remain powered.

## Priorities and Verification

### Recommended Work Order

1. **Establish the intended release baseline.** Reconcile C-P07 without discarding unrelated user edits; reproduce release builds against committed source, not the Common junction's dirty contents.
2. **Physical/control correctness.** C-A01/C-A02/S-T01, then C-A03/C-T01/C-T02. Bench-test all input/output identities and relay modes before connecting process loads.
3. **Prevent incorrect configuration and lost records.** S-W01/S-W02/S-W03, S-D01/S-D03, S-T03, S-O01/S-O02, C-P01/C-P04. Use failure injection and round-trip tests.
4. **Unify event/quality/delivery state.** S-D02/S-T02/S-T04, C-A04/C-T03/C-P02/C-P03, viewer trust and summary fidelity. Adopt the daily numbering correction before any schema increase.
5. **Operator visibility and bounded service.** Remaining website/history/log defects, late-clock scheduling, offline charts, cooperative maintenance, viewer networking and metadata.
6. **Measure then optimize.** Run peak-heap, loop/alert-latency, flash-write, Notehub-byte/sync, and whole-device current measurements. Fix demonstrable repeated work before changing safety-sensitive cadence or warmup.

### Evidence and Limits

**Performed on 09092026:** all four September reviews read; current HEAD and dirty-file differences captured read-only; installed Opta pin, Ethernet, I2C, and ArduinoJson implementation consulted; schema-aware daily reachability checked; client late-clock re-arm and committed request consumer verified; six missing web handlers and three real-schema config-loader regressions reproduced on loopback fixtures. Official Notecard `web.get` response/request semantics checked. Source/document links and provenance coverage are checked by the local master validator.

**Inherited evidence, not newly rerun builds:** FULL reported server/client/viewer/FTPS builds of the dirty tree and nine source-derived probes. Its schema-2 daily probe is superseded by the gate-aware correction; the other probes remain useful but are not native MCU execution. CLAUDE reports clean-HEAD builds with core 4.5.0, ArduinoJson 7.4.3, and warning triage. Those two client build sizes differ because the client sources differ. Compilation success does not establish that proposed fixes compile, or that firmware is safe to deploy.

**Browser evidence:** 09092026 tests used extracted current server HTML and synthetic API/config data, not live devices. September 8 layout/pixel results are retained with their respective fixture/viewport limits. Screenshots from FULL's environment were blank; CLAUDE reports separate rendering measurements. This master does not claim a fresh visual approval or new production screenshot pass.

**Not executed:** firmware changes, uploads, live SMS/email/Notehub commands, relay operation, provisioning, OTA swaps, power-loss injection, maximum-fleet heap tests, or whole-device energy profiling. Unique lower-risk source findings are retained with their cited evidence and scope; the source reviews' claim that all 163 rows were independently verified is not adopted as proof, nor are duplicate rows counted as distinct bugs.

Original reviews remain unchanged. This master is the current consolidation and validity record, not proof of remediation.

## Source Crosswalk

Every ID in the four recent findings lists is accounted for below. **Retain** means the recommendation is carried into the referenced issue with its stated scope/evidence; it does not claim an independent hardware reproduction. **Narrow** corrects cause, affected baseline, severity, or an absolute claim. **Conditional** retains a test/risk gate rather than a proven current failure. Duplicate rows map to one root issue. Older July/June review IDs cited by these sources are covered through the inherited findings, not a claim that every historical document was freshly re-reviewed.

### FULL and COPILOT

The following mapping applies to both documents' shared W/F identifiers. COPILOT's expanded example patches remain subject to the validity corrections above.

| Source ID | Disposition | Master issue |
| --- | --- | --- |
| W-01 | Working-tree-only schema/loop-power regression | C-P07, C-A05 |
| W-02 | Working-tree-only offline sensor-alarm gate; remote relay loss is separate | C-P07, C-T02 |
| W-03 | Working-tree regression; HEAD watermark also needs a lifetime counter | C-P07, C-P05 |
| W-04 | Working-tree-only missing consumer/timestamps/build guard | C-P07, C-P06 |
| F-01 | Conditional schema-change hazard; original model omitted ingress gate | Validity Corrections, S-T04 |
| F-02 | Retain; reject ambiguous old/new index fallback | S-T01, C-A02 |
| F-03 | Retain; validate before narrowing | S-W03 |
| F-04 | Retain | C-T01 |
| F-05 | Retain rate-limit/latch and first-send defects; separate fixed HEAD offline path | C-T01, C-A02, C-P07 |
| F-06 | Retain; loop polling alone is not adequate under blocking work | C-A03 |
| F-07 | Narrow to server/viewer scheduling; client re-arm exists | S-O03, V-T02 |
| F-08 | Retain | S-O01 |
| F-09 | Correct order: broadcast -> save attempt -> response; durability remains unresolved | S-T02 |
| F-10 | Retain; type-defaulting and same-second collisions both matter | S-T03, C-P01 |
| F-11 | Retain | S-D02, C-T03 |
| F-12 | Retain; use inbound keys, not API-output aliases | S-D02, V-W01 |
| F-13 | Retain; reload-only patch is incomplete | S-O02 |
| F-14 | Retain | S-O01 |
| F-15 | Retain | S-T02 |
| F-16 | Retain capacity mismatch; ordinary split daily notes are not all affected | C-T03 |
| F-17 | Retain blocking structure; complete-pass waits are 520 s, not a universal duration | S-O03 |
| F-18 | Retain; define bounded unknown-voltage policy | C-P03 |
| F-19 | Retain filter reset; correct date-handler scope diagnosis | S-W02, S-W04 |
| F-20 | Retain | S-W04, S-D04 |
| F-21 | Retain; viewport/data/resize conditions determine exact overflow | S-W05 |
| F-22 | Retain; preserve data independently of chart renderer | S-W04 |
| F-23 | Retain current label/calculation mismatch; stale policy needs explicit agreement | S-W06 |
| F-24 | Retain; RAM dedupe alone is not durable processing | S-T04 |
| F-25 | Retain; use actual internal-flash APIs | X-R03 |
| F-26 | Retain response validation and signed-current correction | X-R03 |
| F-27 | Retain; optional missing overlay is not itself a release failure | X-R01 |
| F-28 | Retain full transaction validation | S-O04 |
| F-29 | Retain; baseline-age patch alone does not compute a true 24-hour delta | S-D02 |

### INDEPENDENT

| Source ID | Disposition | Master issue |
| --- | --- | --- |
| R-01 | Conditional; schema-1 missing-array part 1 does not clear, schema 2 is rejected | Validity Corrections |
| R-02 | Retain stable identity; require explicit compatibility | S-T01 |
| R-03 | Retain | C-T01 |
| R-04 | Split: rate-limited event loss at HEAD; offline gate only in dirty client | C-T01, C-P07 |
| R-05 | Retain | S-O01 |
| R-06 | Reject at HEAD; retain working-tree regression and UI timeout | C-P07, S-W06 |
| R-07 | Retain | S-W03 |
| R-08 | Retain for server/viewer; client re-arm confirmed | S-O03, V-T02 |
| R-09 | Retain, including fractional type issue | S-T03 |
| R-10 | Retain durable-save/broadcast issue with corrected call order | S-T02 |
| R-11 | Retain | S-O02 |
| R-12 | Retain | S-O01 |
| R-13 | Retain | S-T02 |
| R-14 | Retain; do not rely on an occasionally running loop | C-A03 |
| R-15 | Retain; reject an indefinite hold without recovery policy | C-P03 |
| R-16 | Retain; current history sites synthesis is already fixed | S-W02, S-W04 |
| R-17 | Retain missing tokens/layout; radius/menu choice is design work | S-W05 |
| R-18 | Retain | S-O04 |
| R-19 | Retain | S-T04 |
| R-20 | Retain socket-aware cooperative maintenance | S-O03 |
| R-21 | Retain schema documentation mismatch; reject blind global schema bump | Validity Corrections, X-R02 |
| R-22 | Retain | X-R02 |
| R-23 | Retain redundant polling/error UX; fixed five-second overlay claim qualified | S-W07, S-O03 |
| R-24 | Retain unnecessary PIN prompt | S-W06 |

Additional unnumbered INDEPENDENT claims: the previous `sensorIndex || 1` sparkline-collision fix is present in the current sparkline-loading path; do not re-report its earlier form. Client epoch arming, the history `sites` synthesis, boot hash insertion, and session-cookie protections are present. The suggestion to suppress immediate alarm sync and the assumption that night means no need to poll battery voltage are not accepted without the latency/recovery gates in C-P05.

### CLAUDE High Rows

| Source ID | Disposition and qualification | Master issue |
| --- | --- | --- |
| H-01 | Retain float/default mismatch; reject "every config ever" and permanent-zero certainty | S-T03, C-P01 |
| H-02 | Retain removed-owner/remote OFF gap; physical result depends on pin and target path | C-A02 |
| H-03 | Retain when stuck detection is enabled on a legitimately steady sensor | C-A04 |
| H-04 | Retain input-terminal mapping defect | C-A01 |
| H-05 | Retain silent first-sample persistent-relay restoration | C-A02, C-T01 |
| H-06 | Retain | C-T01 |
| H-07 | Retain direct GPIO/bookkeeping conflict; address together with physical map | C-A02 |
| H-08, H-09 | Duplicate critical-repeat issue; scope to non-MPPT battery-monitor path | C-P02 |
| H-10 | Retain cached-voltage debounce; exact 300 ms depends on loop work | C-P03 |
| H-11, H-12 | One mapping defect; LEDs are not coils, so an LED table is not the fix | C-A01 |
| H-13 | Retain HEAD's direct remote-command loss | C-T02 |
| H-14 | Retain nowrap/width conflict; reduce to operator-usability priority, not physical-control severity | S-W05 |
| H-15 | Browser-reproduced with `sensor:"current"` | S-W01 |
| H-16 | Browser-reproduced one-sided threshold changes | S-W01 |
| H-17 | Browser-reproduced digital trigger reset | S-W01 |
| H-18 | Browser handler-scope check confirmed both names missing | S-W02 |
| H-19 | Retain | S-W03 |
| H-20 | Browser handler-scope check confirmed | S-W02 |
| H-21 | Browser handler-scope check confirmed both names missing | S-W02 |
| H-22 | Retain empty/partial cache rewrite and metadata loss | S-O02 |
| H-23 | Retain load-failure/size-boundary destructive replacement | S-D03 |
| H-24 | Retain missing lifecycle/budget protections; deterministic socket failure needs bench evidence | S-O02 |
| H-25 | Retain registration drop; metadata-only fix also needs API enumeration | S-D01 |
| H-26 | Retain allocation risk; exact fleet-size failure/heap bytes unmeasured here | S-D04 |
| H-27 | Retain | S-O02 |
| H-28 | Retain wrong mount; always-fails-after-first claim filesystem-dependent | C-P04 |
| H-29 | Retain separate threshold authorities; not all chemistries share identical error bands | C-P02 |
| H-30 | Narrow: missing disconnect/verification is real; static overload does call set_network | V-T03 |
| H-31, H-32 | Duplicate unauthenticated roster mutation; severity depends on viewer network trust | V-T01 |

### CLAUDE Medium Rows

| Source ID | Disposition and qualification | Master issue |
| --- | --- | --- |
| M-01 | Retain artifact-growth/process concern; historical size is not a fresh measurement | X-R01 |
| M-02 | Retain no-pulse stale-result issue | C-A03 |
| M-03 | Retain ordinary healthy-timer coverage gap; do not activate false stall restarts | C-P05 |
| M-04 | Retain resettable evidence-counter flaw | C-P05 |
| M-05 | Retain state-flapping repeat cost | C-P02 |
| M-06 | Retain 16-bit duration contract/clamp defect | C-P01 |
| M-07 | Retain ineffective modem power multipliers; change cadence only with recovery guarantees | C-P05 |
| M-08 | Retain missing numeric range/minimum validation | C-P01 |
| M-09 | Retain nonconsecutive recovery | C-A04 |
| M-10 | Retain scale/reference validation; exact approximately 8% magnitude bench-qualified | C-A05 |
| M-11 | Retain global fault code misattribution | C-A04 |
| M-12 | Retain | C-T01 |
| M-13 | Retain persisted-state overwrite of new transition | C-P03 |
| M-14 | Retain replay ordering | C-T03 |
| M-15, M-16 | Duplicate one-command-per-cycle latency issue; cooldown must be fixed with draining | C-T02 |
| M-17 | Retain missing build prerequisites/options | X-R01, X-R02 |
| M-18 | Retain obsolete stated capacity/specifications | X-R02 |
| M-19 | Retain toast concealment defect; measured strip size is viewport/content-specific | S-W05 |
| M-20 | Retain global login backoff affecting valid sessions | S-O04 |
| M-21 | Retain association-child stride bug | S-W05 |
| M-22 | Retain navigation consistency; do not make unused pause controls mandatory | S-W05 |
| M-23 | Retain local table overflow fix; avoid blanket page clipping | S-W05 |
| M-24 | Browser handler-scope check confirmed | S-W02, S-W04 |
| M-25 | Retain omitted server-side sensor filter | S-W04 |
| M-26 | Retain filter vocabulary mismatch | S-W07 |
| M-27 | Retain visible/export filtering inconsistency | S-W07 |
| M-28 | Retain missing semantic style classes | S-W05 |
| M-29 | Retain error/success toast inconsistency | S-W05 |
| M-30 | Retain digital semantics; `--` depends on unitless-tank formatting branch | S-W06 |
| M-31 | Retain no-expiry update state, even with HEAD consumer present | S-W06 |
| M-32 | Retain undefined card/accordion vocabulary | S-W05 |
| M-33 | Retain context-specific escaping risk; exploit surface depends on writable data path | S-O04 |
| M-34 | Retain blocking/WDT budget risk; exact 30-second claim is not a verified bound | S-O03, V-T04 |
| M-35 | Retain missing persistence-failure backoff | S-O01 |
| M-36 | Retain backup confidentiality concern, distinct from accepted public signing keys | S-O04, X-R02 |
| M-37 | Retain incomplete TCP-body read; reject claim every lookup necessarily fails | S-D05 |
| M-38 | Retain missing failure cache/backoff | S-D05 |
| M-39 | Retain partial hot-month preferred over complete rollups | S-D04 |
| M-40 | Retain | S-O03 |
| M-41 | Retain aggregate alarm/type mismatch | S-W06 |
| M-42 | Duplicate H-01 | S-T03, C-P01 |
| M-43 | Retain retry policy/poll-cadence mismatch | S-T03 |
| M-44 | Retain thirteen recurring peeks; quoted per-window timing is an estimate | S-O03 |
| M-45, M-46 | Duplicate diagnostic-to-zero overwrite | S-D02 |
| M-47 | Retain reconcile-before-upsert; next correction may arrive before 24 h via another note | S-D01, S-T04 |
| M-48, M-49 | Duplicate unload label replacement, plus dirtiness issue | S-D02 |
| M-50 | Retain canonicalization/enrollment validation issue | S-T05 |
| M-51, M-52 | Duplicate lifetime reminder-anchor/new-episode eligibility issue | S-T02 |
| M-53 | Retain reboot-sensitive stale/recovery notifications | S-T05 |
| M-54, M-55 | Duplicate small manifest/parse/replace failure; exact archive count depends on size | S-D03 |
| M-56 | Retain actual-capacity versus configured-retention mismatch | S-D04 |
| M-57 | Retain save-result contract/feedback mismatch | S-W07, S-O01 |
| M-58 | Retain overlength input truncation | S-W07, S-O04 |
| M-59 | Retain stale calibration window/log growth | S-D05 |
| M-60 | Retain valid-zero reference rejection | S-W03 |
| M-61 | Retain missing URL decoding | S-W07 |
| M-62 | Duplicate H-28; FAT-specific failure qualified | C-P04 |
| M-63 | Conditional reset-before-swap false rollback classification | C-P06 |
| M-64 | Retain lifecycle/WDT risk; exact network result needs hardware test | V-T03 |
| M-65 | Retain viewer late-sync scheduling gap | V-T02 |
| M-66 | Conditional timing race; not proven permanently one cycle behind | V-T02 |
| M-67 | Retain documented object-body mismatch in server and viewer | V-T04 |

### CLAUDE Low Rows

| Source ID | Disposition and qualification | Master issue |
| --- | --- | --- |
| L-01 | Retain artifact/privacy audit; check actual Pages output before asserting exposure | X-R02 |
| L-02, L-03 | Duplicate repeated sync-request poll window | C-P05 |
| L-04 | Retain time-attempt backoff; do not conflate with fixed client schedule re-arm | C-P05 |
| L-05 | Retain direct NORMAL reset/transition-side-effect issue | C-P03 |
| L-06 | Retain diagnostic publication inside acquisition phase | C-A04 |
| L-07 | Retain uncapped delay/unpowered-loop watchdog gap | C-A05 |
| L-08 | Retain since-boot versus daily extrema semantics | C-P05 |
| L-09 | Retain unnecessary trim probes; quantify actual returned-data cost | C-P05 |
| L-10 | Consumer audit/optimization, not permission to remove diagnostic/external-route fields | C-P05, X-R04 |
| L-11 | Retain incorrect notefile/direction documentation | X-R02 |
| L-12 | Retain release asset/tooling documentation drift | X-R01, X-R02 |
| L-13 | Retain mobile-header space recommendation; dimensions data/viewport-specific | S-W05 |
| L-14 | Retain randomness/expiry hardening; a validity endpoint alone proves no token break | S-O04 |
| L-15 | Retain missing CSS variables; Chart.js canvas needs resolved values | S-W05 |
| L-16 | Retain heading-role/style consistency recommendation | S-W05 |
| L-17 | Retain control font/hover mismatch, not unsupported equal-height claim | S-W05 |
| L-18 | Retain duplicate settings initialization/fetch | S-W07 |
| L-19 | Retain eight-monitor boundary validation | S-W07, C-P01 |
| L-20 | Retain unsafely defaulted schedule while directory unavailable | S-W07 |
| L-21 | Retain commissioning empty/error-state UX | S-W07, S-D01 |
| L-22, L-24 | Duplicate generic login-error feedback | S-W07 |
| L-23 | Retain normalized same-origin redirect validation | S-O04 |
| L-25 | Retain dropped fault/raw-value mapper fields | S-W06 |
| L-26 | Retain contrast/status semantic audit; exact ratios depend on backgrounds | S-W05 |
| L-27 | Retain unnecessary full response to summary consumers | S-O03 |
| L-28 | Retain header filename CR/LF validation | S-O04 |
| L-29 | Retain missed-day rollup backfill | S-D03 |
| L-30 | Retain chunk-size/validator opportunity; CSS already has max-age | S-O03 |
| L-31 | Retain HTML caching/versioning opportunity | S-O03 |
| L-32 | Retain avoidable small-page allocation/copy | S-O03 |
| L-33 | Retain bulk body-read opportunity plus framing validation | S-O03 |
| L-34 | Retain config type/range validation before string copies | S-O04 |
| L-35 | Retain allocation-failure request cleanup | S-T04 |
| L-36 | Conditional unsupported system-alarm se:true input; initialize/exhaustively compose | S-T04 |
| L-37 | Retain consistent sensor identity gates without breaking registration | S-D01, S-D02 |
| L-38 | Retain daily/telemetry raw-mA validity mismatch | S-D02 |
| L-39 | Retain unload notification outcome metadata | S-D02 |
| L-40 | Retain server-only recipient welcome/consent consistency | S-T05 |
| L-41 | Retain unsolicited reply/opt-out eviction risk; carrier enforcement is separate | S-T05 |
| L-42 | Retain log identity/diagnostic detail improvement | S-W07, S-T05 |
| L-43 | Retain daily recipient dedupe/placeholder consistency | S-T05 |
| L-44 | Retain subject date/timezone/format contract | S-T05 |
| L-45 | Retain observable/bounded daily-delivery failure handling | S-T02 |
| L-46 | Consumer audit: remove or expose raw viewer values deliberately | V-W01 |
| L-47 | Retain duplicate-registry merge state completeness | S-T02 |
| L-48 | Retain pointer invalidation during registry compaction | S-O01 |
| L-49 | Retain first-colon year-over-year key parsing | S-W07 |
| L-50 | Retain calibration quality restoration/recomputation | S-D05 |
| L-51 | Retain ordered version comparison | S-W06, V-T04 |
| L-52 | Retain battery label/alert band consistency | C-P02 |
| L-53 | Retain version/build-sequence CI invariant | X-R01 |
| L-54 | Retain stale comments; do not revive rejected rename/FAT subclaim | X-R02 |
| L-55 | Maintenance debt in inactive updater, not a demonstrated active flash path | X-R03, C-P06 |
| L-56 | Confirmed installed-driver limitation: Stream timeout is not an I2C deadline | C-A05 |
| L-57 | Retain width/115200 validation defect | C-P01 |
| L-58 | Retain shared design vocabulary recommendation, not compulsory visual identity | V-W01, S-W05 |
| L-59 | Retain viewer stale/snooze/quality parity | V-W01 |
| L-60 | Retain save rejection/error recovery | V-W02 |
| L-61 | Retain bounded post-save summary polling | V-T02 |
| L-62 | Retain quantity-aware print formatting and unavailable timestamp | V-W01 |
| L-63 | Retain reproducible ZIP and concurrent-push handling; qualify "never" | X-R01 |
| L-64 | Retain missing FTPS target release asset | X-R01 |

### Unnumbered and Disputed Material

CLAUDE's open product-UID and viewer-summary-size questions, rejected calibration-line concatenation, reported compiler warnings, runtime/performance estimates, and broad "found sound" statements are qualified in X-R04 and the evidence section. Its earlier S-9 health/diagnostic inbox observation is retained in C-P05's consumer audit: absence of a server handler does not make Notehub diagnostic data useless. Claims that note deletion, snooze durability, ACK semantics, or all atomic writes are wholly sound conflict with specific unresolved paths elsewhere in the same review; the path-specific findings take precedence.

FULL/COPILOT efficiency and security recommendations are retained by S-O03/S-O04, C-P05, V-T01, and X-R01..X-R04, subject to the validity corrections. The greater-than-98% duty-cycle saving, 50 ms warmup, whole-device sleep current, arbitrary deadband, global style reset, and unsafely simplified protocol/storage snippets are not approved implementation requirements.