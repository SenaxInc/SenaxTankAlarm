# Review of Recent PRs #318-#321

- Date: 2026-09-24
- Reviewer: GitHub Copilot
- Review baseline: `c9a9cceae06ae1879216bf07bb8db5bea3a3a644` (`master`, v2.2.16).
- Restored: this is the review first committed as `CODE_REVIEW_09242026_PRS_318_321_COPILOT.md` in `a1a2c16`, restored byte for byte under this name on 2026-09-24 because `7b4c8be` reused that file name for a different review. Only this line was added.

## Findings

Three P2 findings and one P3 finding follow. P2 means a functional/data-correctness problem worth scheduling soon; P3 is a narrower follow-up. None is presented as a demonstrated safety-critical failure. The main remaining risk in this PR group is the integration between history admission, cached readings, and the new rollup logic, rather than JavaScript syntax or the isolated debounce helper.

| ID | Priority | PR relationship | Finding |
| --- | --- | --- | --- |
| R1 | P2 | Existing check retained in #319 | Valid zero readings disappear from daily history |
| R2 | P2 | New provenance check in #319 | A supported poll configuration can put yesterday's voltage in today's history |
| R3 | P2 | New merge policy in #319 | Late alarm counts are discarded when fewer samples remain in the hot ring |
| R4 | P3 | New upgrade/deduplication interaction in #319 | A legacy entry can prevent a trustworthy replay from entering the warm tier |

### R1 - P2: Daily history still drops valid zero readings

**Locations:** [daily-report admission gate](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L13681), [snapshot admission](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L7664).

**Classification:** Remaining defect in a path changed by #319, not a newly introduced zero-value check.

The daily snapshot condition is still `trustLevel && dailyMaInRange && newLevel > 0.0f`. A fresh, timestamped zero-pressure, empty-tank, or inactive digital reading is therefore discarded even when its sensor data is valid. Telemetry admission does not impose this positive-only restriction. With change-based telemetry disabled, the daily report is the intended source of ongoing history, so zero-valued days can remain blank and transitions to zero can be absent from daily minima and closing values.

**Suggestion:** Validate that an actual, finite measurement is present, retain the acquisition-time and sensor-quality checks, and admit zero as data. Do not admit `resolveLevel()`'s missing-data default merely by removing the comparison.

**Regression coverage:** Exercise `handleDaily()` with a fresh zero reading, a valid current-loop live-zero sample, an inactive digital input, and missing/invalid measurement data. Compare admission with `handleTelemetry()`, including change-based reporting disabled.

### R2 - P2: The one-hour voltage-age assumption is not enforced by the client

**Locations:** [voltage provenance predicate](../TankAlarm-112025-Server-BluesOpta/WarmTierStore.h#L393), [allowed MPPT poll interval](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L3360), [cached voltage selection](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L8180), [failed-poll handling](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L674).

**Classification:** New validation gap in #319, reachable with a supported non-default configuration. The header notes that longer poll intervals are not covered, but the caller still admits their voltages as if the one-hour bound were proven.

`warmVinOnReadingDay()` assumes that the voltage was measured no more than 3,600 seconds before the note was built. The client permits `solarCharger.pollIntervalSec` up to 3,600 seconds, retains the last successful data through four failed polls, and `getEffectiveBatteryVoltage()` accepts that cached value while `isCommunicationOk()` remains true. There is no one-hour age check in that selection path.

**Reproduction:** Set a 30-minute MPPT poll interval. The last successful voltage is measured at 23:30; polls at 00:00, 00:30, and 01:00 fail. At 01:05, a valid sensor telemetry note still carries the 23:30 voltage. The server's assumed interval, 00:05 through 01:10, lies entirely on the new day, so the predicate accepts the voltage even though its actual acquisition was yesterday. This violates the explicit same-day history rule without requiring a corrupt timestamp or unsupported configuration.

**Suggestion:** Send the actual voltage acquisition epoch or age, independently of the level acquisition time, and use it for history admission. The MPPT data already retains `lastReadMillis`; the Vin-divider path likewise needs its own acquisition time. Until provenance is available, omit a voltage whose measurement day cannot be established. Increasing the guessed one-hour constant alone does not establish that guarantee.

**Regression coverage:** Extend the existing voltage provenance cases with 30- and 60-minute polls, retained data after failed polls, midnight crossings, and Vin-divider configurations. Include a fresh same-day control case. Do not assume default poll settings inside the admission tests.

### R3 - P2: A smaller retained sample set blocks a legitimate late alarm-count update

**Locations:** [merge decision](../TankAlarm-112025-Server-BluesOpta/WarmTierStore.h#L792), [row computation](../TankAlarm-112025-Server-BluesOpta/WarmTierStore.h#L595), [daily alarm count callback](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L8237), [existing late-alarm test](../tests/host/warm_store/warm_store_test.cpp#L2196).

**Classification:** New edge case in #319's late-alarm reconciliation. The existing test covers a late alarm only while the retained reading count is unchanged.

`warmDecideVisitor()` marks a new row superseded whenever the stored `n` is larger, regardless of the new `al`. That protects better level aggregates from a partial ring, but it also rejects an independently known increase in the alarm count.

**Reproduction:** A completed day is stored as `n=48, al=0`. Subsequent samples overwrite six of that day's entries in the bounded hot ring. A delayed alarm for that day arrives, marks it dirty, and produces a recomputed row with `n=42, al=1`. The `exN > nr.n` branch preserves the complete old row unchanged, including `al=0`. The dirty mark is consumed without recording the known alarm. If no readings remain for the day, `warmComputeRows()` emits no row at all, so an alarm-only update is also unable to reach the stored row.

**Suggestion:** Merge alarm metadata independently of replacing the level aggregates. Preserve the stored `n`, min/max/average/open/close when the ring is partial, while allowing a verified higher alarm count to update the existing row. Handle retained days with an existing summary but no remaining hot samples, without fabricating a level. Exact counts across log eviction/reboots would additionally require durable event identity/counting; taking a maximum alone cannot provide that broader guarantee.

**Regression coverage:** Extend the current late-alarm test with both partial and empty hot rings. Assert that the level statistics remain byte-equivalent, `al` increases, repeated delivery is idempotent, and the next unchanged tick performs no rewrite.

### R4 - P3: Legacy-only entries can suppress a newly verified replay

**Locations:** [legacy marking on load](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L8714), [ingest duplicate early return](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L7704), [whole-ring duplicate scan](../TankAlarm-112025-Server-BluesOpta/WarmTierStore.h#L369), [legacy exclusion from rollup](../TankAlarm-112025-Server-BluesOpta/WarmTierStore.h#L618).

**Classification:** New upgrade-specific interaction in #319. This is not a request to trust old unverified history wholesale.

The upgrade intentionally keeps older-format entries for charts but excludes them from daily rows using `legacyCount`. However, `warmRingHasAcquisition()` scans all entries without considering that mark. An independently valid incoming telemetry/daily copy whose acquisition time matches a legacy entry is therefore rejected before it can become trusted rollup input.

**Reproduction:** Restore a ring containing one legacy entry at a valid midday epoch, with `legacyCount=1`. Receive a valid, timestamped copy of that acquisition which passes the new admission checks. The duplicate scan returns true; the ring stays legacy-only, and the daily computation still has zero eligible samples. This can leave an upgrade-period gap even though a trustworthy source was received after the update.

**Suggestion:** Distinguish trusted from legacy duplicates. Either promote/replace the matching entry using the verified incoming payload, maintaining the contiguous legacy-prefix invariant, or deduplicate eligible rollup input only against other trusted entries. Do not clear the legacy mark on unrelated entries.

**Regression coverage:** Combine the existing legacy-upgrade and deduplication cases: replay a matching valid acquisition, verify one trusted contribution, then replay it again and verify no double count. Include wrapped rings and a save/reload cycle.

## Scope

The latest four merged PRs form the v2.2.16 change group. Review the merged result as well as each first-parent PR diff; later release/version commits are included in the baseline.

| PR | Merge | Area |
| --- | --- | --- |
| #318 | `df7195e` | Consecutive alarm debounce |
| #319 | `6706044` | Warm history and acquisition-time handling |
| #320 | `e5442a2` | Email bridge retry deduplication |
| #321 | `3fc9d22` | Operator time display and integer epoch serialization |

The first-parent baseline before these merges is `4d2a12a`. The fetched remote matched the review baseline. Earlier PRs #314-#317 were read only where needed for surrounding behavior, not independently re-reviewed.

The owner decisions recorded in the [previous release review](CODE_REVIEW_09232026_PRS_318_321_CLAUDE.md) remain constraints: whole-minute note times are intentional; unknown-day data must not fill history gaps; email delivery uses the Google Apps Script bridge. The suggestions above do not require restoring second-resolution operator displays or changing the email provider.

## Other Suggestions

1. **Add handler-level coverage to the existing host suites.** The isolated history helpers have substantial coverage, but they do not exercise `handleDaily()` admission or the complete legacy-load -> duplicate-check -> rollup path. R1 and R4 illustrate that boundary. Extract only the minimum hardware-free admission logic needed to test the actual implementation, rather than maintaining a separate approximate handler in the tests. Relevant homes: [host suites](../tests/host/README.md) and [warm-store tests](../tests/host/warm_store/warm_store_test.cpp).
2. **Describe #320 as best-effort duplicate suppression, not exactly-once delivery.** The bridge intentionally sends if the lock/cache fails or another send remains in flight after 20 seconds; the cache is limited to six hours. Preserve the alert-delivery-first decision, but distinguish those expected duplicates in logs and deployment acceptance criteria. Existing tests deliberately exercise this behavior; it is not a newly discovered regression. Also verify that the deployed Apps Script is updated: a firmware update alone cannot replace an already deployed bridge. See [email bridge tests](../tests/host/email_bridge/email_bridge_test.js#L514) and [route setup](../Tutorials/Tutorials-112025/NOTEHUB_ROUTES_SETUP.md).
3. **Automate syntax/scope smoke checks for all changed PROGMEM pages.** The email suite extracts the bridge, not every browser page. This review parsed the server/viewer scripts and checked the Client Console's changed `formatEpoch` dependency; make comparable checks a CI gate so later edits to long raw-string lines cannot silently disable a whole page. A small browser smoke test should cover changed controls as well as syntax.
4. **Keep the hardware acceptance checks open.** Host fault injection does not prove Opta LittleFS rename/ENOSPC behavior, power-loss durability, worst-case warm-tier latency, or physical relay behavior. The previous review already records those checks and deferred relay work. This review neither clears them nor reports those known deferred items as new PR regressions.

## Ruled-Out Suspicions

- Minute-truncated sensor `t` alone does **not** keep an on-demand telemetry response pending in the dashboard. [processNotefile()](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12491) passes the Notecard envelope's `time`, and `handleTelemetry()` uses that for live `lastUpdateEpoch`. The rounded acquisition time is used separately for history. A direct UI-only model using acquisition time was rejected after tracing the actual receive path.
- The changed Client Console location formatter does have a `formatEpoch()` definition in its own page scope; no missing-helper regression was found there.
- No additional confirmed defect was found in #318's consecutive-counter helper or #321's inspected display changes. This is not hardware certification of alarm actuation or an assertion that all surrounding legacy issues are resolved.

## Verification

| Check | Result and limits |
| --- | --- |
| Existing email bridge suite | `node tests/host/email_bridge/email_bridge_test.js`: **137 checks, 0 failures**; exercises the actual bridge extracted from the sketch |
| Embedded browser-script syntax | **18 HTML constants, 19 inline scripts, 0 syntax failures** across server/viewer; raw-string fragments joined in memory and C++ macro substitutions represented by `0`; not a live-browser or asset-loading test |
| Changed Client Console formatter | Page-local `formatEpoch()` and the changed `fetchLocationInfo()` call checked |
| Daily zero gate | Extracted condition rejects a valid zero and accepts a valid positive control; the prior merge parent confirms the positive-only condition was pre-existing |
| History edge cases | Source-checked in-memory JavaScript models reproduce R2-R4, including the equal-`n` control for R3; these are decision-level checks, **not execution of the C++ suites** |
| Timestamp suspicion | Source trace through Notecard ingress plus actual dashboard functions with the correct envelope time rejects the suspected same-minute pending regression |
| Native C++ suites / firmware builds | **Not run in this review.** No native host C++ compiler or installed WSL environment was available; no toolchain was installed. Firmware compilation was not rerun for this document-only change |
| Physical device / live email deployment | **Not exercised.** No firmware upload, live server mutation, email send, relay actuation, or Apps Script deployment |

All executable probes ran in memory and did not create repository test/build artifacts. Only this new review document is changed; no production code, configuration, existing documents, or test files are included in the review commit.