# PRs #318–#321 (v2.2.16): history, alarm debounce, duplicate emails, times to the minute

**Date:** 2026-09-23
**Release:** v2.2.16 (build 303)
**Merged into `master`:** #318 `df7195e`, #319 `6706044`, #320 `e5442a2`, #321 `3fc9d22` (merge commits)
**Author:** Claude (Opus 5.5) with the repository owner

This records what the four pull requests changed, how they were verified, how the reviews went, and what was left open. The PR descriptions have the full detail. The user-facing summary is `release-notes/v2.2.16.md`.

## Owner decisions that shaped the work

- **Field state (2026-09-22):** no relays or digital/float inputs are in the field yet. They will be deployed soon. Field clients are analog only. So S-D03 and C-T01 went first, and the relay/float items (C-A01, C-A02, S-T01) come next.
- **History rule (2026-09-23):** history must never fill gaps with data from other days. A day with no readings stays blank. Every value in a daily row must come from fresh readings taken that UTC day, and a reading whose freshness or time is unknown is left out rather than guessed. Chart lines drawn across gaps are fine; the stored data is what must be accurate.
- **Time resolution (2026-09-23):** second-level precision does not matter ("things happen pretty slow concerning messages"). Note times are whole minutes, and operator displays drop seconds.
- **Email:** the EmailRoute uses the Google Apps Script bridge, not SendGrid.

## #318 — client alarms (C-T01)

- **Debounce:** strictly consecutive, through the new `TankAlarm_AlarmDebounce.h`. Invalid samples hold the counters. Evidence resets while a sensor is failed. In overlapping trigger zones a reading counts for neither alarm.
- **Rate limiter:** the boot clamp that suppressed alarms for the first 300 s is gone, and hourly pruning is wrap-safe.
- **Pending edge:** a 1-byte pending slot re-sends a rate-limited latch edge. It is publish-only and re-sent only while the current sample is in alarm; a clear replaces it.
- **Sensor recovery:** a latched level alarm is re-asserted after sensor-recovered, but only while the reading is still in alarm.
- **Relay restore:** a relay restored at boot now also publishes its alarm note.
- **Note times:** alarm notes carry the reading's acquisition time. Telemetry, daily-report and alarm `t` are whole minutes, truncated, written as `uint32`. Telemetry omits `t` when there has been no valid reading since boot.
- **Verification:**
  - Host tests: 1,228 checks, including an exhaustive digital equivalence test against a v2.2.15 port, a 20,000-vector property test and millis-wrap vectors.
  - Client builds: plain and `security=sien`, 386,196 B flash, RAM unchanged.

## #319 — server daily history (S-D03)

- **Store:** `WarmTierStore.h`, header-only and host-tested.
- **Month files:** streamed and merged row by row. The result is fail-closed (a file that can't be read is never emptied), written through `.tmp` + rename, and an unparseable file is kept as `.bad`.
- **Rollup:** bounded per tick. It backfills missed days that have readings, re-rolls a day when a late snapshot arrives, and runs before the hot-tier prune. The prune never removes days the rollup hasn't reached.
- **Other fixes:**
  - Readers accept files of any size.
  - Archived-client manifest: fixed (M-54/M-55).
  - `?file=` accepts only files listed in the manifest.
  - Flash-write path pointer: fixed.
  - `warmTier` diagnostics added.
- **What enters history**, per the owner rule:
  - Fresh readings only: no `ru`, `sf`, fault, current-loop outside 4–20 mA, or pre-2.2.16 on-demand placeholders.
  - Acquisition time only; no receive-time fallback.
  - Integer `hot_tier.json` times, because ArduinoJson stored float-exact doubles with 7 digits.
  - Ring-wide dedupe.
  - Voltage only from the same measurement's day.
  - Alarm count by event time; late alarms re-roll their day.
  - v2.2.15 hot-tier snapshots are charted but never rolled up.
  - The midnight-rounding rule was removed once seconds were declared irrelevant.
- **Verification:**
  - Host tests: 10,038 checks, covering calendar, splitter grammar, the truncation sweep, a differential test against v2.2.15, merge, quarantine, the scheduler, per-operation and per-allocation fault injection, H-23 replay, the manifest and provenance.
  - Server build: 1,040,876 B flash on the branch.

## #320 — duplicate emails

- **Cause:** Notehub reroutes a route call that doesn't answer within 30 s. The Apps Script bridge sent on every call, and the daily report is the largest and slowest message.
- **Server:** every `email.qo` body gets an `id`, which stays the same across the server's own retry. An alert or daily report too large with the id is sent without it.
- **Bridge:**
  - It records the Notehub event ID and the message id under the script lock, as `sending` and then `sent`, for 6 h.
  - A repeat of a sent email answers `duplicate`.
  - A repeat that arrives during a send waits, timed by the clock, and sends only if that send failed or is still running.
  - If the lock or cache fails, it sends anyway.
- **Docs:** the Notehub retry schedule is corrected to 30 s / 1 min / 5 min, per Blues.
- **Verification:** node test of the real script extracted from the page, 137 checks.

## #321 — operator times without seconds

- **Pages:** seconds are removed from the Transmission Log, History tooltips (date-fns `tooltipFormat`), Server Settings, the Client Console location time, the Email Format preview, the dashboard ETA and the viewer's printed report.
- **Integer epochs:** displayed epochs are written as integers: sensor `u`/`pe`, unload `t`/`pt`, the viewer `u`, `locationEpoch`/`le` and calibration timestamps.
- **Kept at seconds:** serial logs (the `since` cursor), CSV exports, config `_ts`, email ids, schedules and `millis()` timing.

## Review process

- **Design:** each PR started from a judged design, with three independent designs per item. Implementation was followed by independent review rounds with different lenses: spec correctness, embedded safety and regressions, and tests/CI.
- **History audit:** for the history rule, every path into history was audited. There were 18 claimed leaks, each adversarially verified, followed by three further review rounds.
- **Copilot:** reviewed each push. Every inline comment got a reply, and the summary-only points got one comment per PR.
  - **Fixed:** stale pending edge after recovery; the pruning, temp-file and oversized-row cases in the store; the in-flight email repeat; the failure-streak carry-over; midnight handling, which was later removed with seconds.
  - **Declined with reasons:** pulse freshness (C-A03), `recoveryCount` (C-A04 M-09), ArduinoJson pinning for firmware CI (policy), the `ru`/`sf` gate on daily reports (their values are real readings at their real times), and trailing bytes after the month array.
  - **Final pass:** no new findings. It noted a few rare edge cases in code these PRs did not change; they are listed below.

## Open after this release

Tracked in `CODE REVIEW/TODO.md`:

- **Relay/float project (next):**
  - C-A01: relay coils driven via LED pins; inputs read from the relay pins.
  - C-A02: relay ownership, modes and reconfiguration, including the config-push latch reset (NEW-A) and the stuck-disable re-assertion.
  - S-T01: stable relay identity; S-T06 is closed as its duplicate.
  - C-A04: stuck detection defaults on for digital inputs; the I2C silent `sensorFailed` clear.
- **Rare edge cases (Copilot final pass):**
  - Alarm rate limiter at the ~49.7-day `millis()` wrap: the first alarm of a type could be held 5 minutes, and a stale hourly budget could survive on a long-quiet device.
  - Email bridge three-way race: an extra duplicate, never a loss.
- **Bench checks** in each PR description. The ones that matter most:
  - LittleFS `stat`/`rename`/ENOSPC semantics.
  - Warm-tier timing and heap with `-DTANKALARM_WARM_SELFTEST`.
  - Power cut during a month rewrite.
  - Alarm debounce and deferral on a 0–10 V input.
  - The Apps Script update.
- **Smaller items:**
  - The dashboard "24 h change" is computed across gaps.
  - The committed viewer `.bin` is stale; the release asset is current.
  - Month-scale epochs are still written as doubles.
  - The History custom-range default date is a UTC date.
