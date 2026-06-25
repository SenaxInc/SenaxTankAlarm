# CODE REVIEW — Remaining Issues After v2.0.50 Ship

**Date:** 2026-06-25
**Scope:** Where we stand after v2.0.47 (A0602 managed bootstrap, my ship), v2.0.48 (battery source rework), v2.0.49 (RS-485 MPPT hardening), and v2.0.50 (current-loop hardening) — what's confirmed working, what's deferred for explicit reasons, and what still needs investigation.

**Supersedes:** the earlier "post-v2.0.47" version of this doc, which was untracked. Several items from that doc are now resolved by v2.0.48–v2.0.50 (see §2). The remaining items plus the new deferrals from the three new releases are consolidated in §3.

**Related docs (all newly added in v2.0.48–v2.0.50):**
- [CODE_REVIEW_06252026_BATTERY_VOLTAGE_SAMPLING.md](CODE_REVIEW_06252026_BATTERY_VOLTAGE_SAMPLING.md) — the design source for v2.0.48 Fix 9/10/11.
- [CODE_REVIEW_06252026_RS485_SUNSAVER_COMMUNICATION.md](CODE_REVIEW_06252026_RS485_SUNSAVER_COMMUNICATION.md) — the design source for v2.0.49 Fix S1-S4.
- [CODE_REVIEW_06252026_CURRENT_LOOP_SENSORS.md](CODE_REVIEW_06252026_CURRENT_LOOP_SENSORS.md) — the design source for v2.0.50 Fix C1/C2.
- [A0602_ADDRESSING_FINDINGS_AND_FIX_TEST_PLAN_06242026.md](A0602_ADDRESSING_FINDINGS_AND_FIX_TEST_PLAN_06242026.md) — original A0602 plan.

**Release notes for context:** [v2.0.48](../release-notes/v2.0.48.md) · [v2.0.49](../release-notes/v2.0.49.md) · [v2.0.50](../release-notes/v2.0.50.md).

---

## 1. State of the bench unit (`dev:860322068056545`) as of 2026-06-25

- v2.0.50 flashed via USB DFU on COM5; Opta re-enumerated cleanly post-MCUboot-swap.
- A0602 status LED still GREEN (carryover from v2.0.47's `bootstrapA0602Managed()` — still the only path that drives the Blueprint controller; v2.0.50's Fix C1 explicitly does NOT call `bootstrapA0602Managed` from the recovery path, per the prohibition in its function header).
- v2.0.50's Fix C1 lightweight re-probe (`resolveCurrentLoopI2cAddress(prev)`) is wired at the two sensor-recovery call sites (≈L1987 and ≈L2066 of the current `.ino`), not inside `recoverI2CBus()` itself.
- v2.0.50's Fix C2 restructured `readCurrentLoopSensor()` into two burst windows (CONFIGURE before warmup → SAMPLE after warmup) so the AD74412R sense node is connected to the I/O pin during the entire 3 s loop-current stabilization.

---

## 2. Resolved vs still-open from the prior doc

Original 2026-06-24 items, re-evaluated:

| ID | Subject | Status as of v2.0.50 |
|---|---|---|
| 3.1 | Bench loop reads 0 mA — hardware/wiring | **Still open** — firmware can't manufacture loop current; transducer / 24 V loop power / channel wiring audit pending. v2.0.50's Fix C2 ensures the AD74412R sense node is connected during the warmup window, which removes one possible *firmware* explanation for 0 mA (sense-node not connected during transmitter electrical settling). If the bench still reads 0 mA after v2.0.50, the cause is definitively hardware. |
| 3.2 | Bench `gVinVoltage` reads 5.03 V → CRITICAL_HIBERNATE | **Partially resolved.** v2.0.48 Fix 9 changes `getEffectiveBatteryVoltage()` from `min()` to strict priority (MPPT → Vin divider). When MPPT is healthy (`scOk:1`), Vin divider can no longer drag the reading down. On the bench, MPPT is not reporting (`scOk:0`), so Vin divider is still primary and the 5.03 V reading still wins. Carrier wiring audit and/or pushing `vinMonitor.enabled=false` via the new Voltage Monitor dropdown remains the next step (see §3.B). |
| 3.3 | Field unit not yet OTA'd | **Superseded** — the OTA target is now v2.0.50, not v2.0.47. The v2.0.48–v2.0.50 chain bundles the A0602 fix plus battery-source rework plus current-loop hardening, all of which are field-relevant. (See §3.A.) |
| 3.4 | Telemetry of `a0602_addr` / `a0602_mgmt_ok` | **Still open** — Change 5 from the original A0602 plan. Not addressed by v2.0.48–v2.0.50. (See §3.C.) |
| 3.5 | P1 PWM gating still uses raw `tankalarm_setPwm` | **Still open and now lower-priority.** v2.0.50's Fix C2 confirmed the raw PWM-on/off works correctly when bracketed by the SPLIT burst windows; the raw path is now intentionally outside both 400 kHz windows. Migrating to `AnalogExpansion::beginChannelAsPwm()` would still be cleaner but is no longer on the critical path. (See §3.D.) |
| 3.6 | A0602 channel LEDs 2 & 4 stuck yellow through power-cycle | **Still open** — not investigated. |
| 3.7 | Working-tree drift caught this session | **Closed (process note only).** The current pull was clean — `git pull --ff-only` succeeded with no working-tree conflict. The lesson stands: `git status` + `git log` before edits. |
| 3.8 | `read_file` returned stale cached content | **Closed (tooling note only).** No code change. |

---

## 3. Still-open work, ranked

### §3.A — HIGH — Field-unit OTA upgrade: v2.0.50 (not v2.0.47)

**Why this matters most:** the bench has proven the firmware chain end-to-end *except* the actual non-zero mA reading. The field unit has a real loop with real current; deploying v2.0.50 is the only way to validate the chain against the real "stuck ~18 mA" symptom that drove the entire June 2026 effort.

**Operator action:**

1. Wait for the v2.0.50 GitHub Release CI artifact to publish if not yet present (the local `firmware/112025/client/TankAlarm-Client-secure-v2.0.50.slot.bin` was pulled with the working tree, but the official release artifact is what Notehub Host Firmware should consume).
2. Upload to Notehub → FIELD project → Settings → Firmware → Host.
3. Target the field device **specifically by Device ID**, not the whole `tankalarm-clients` fleet, until v2.0.50 is field-proven.
4. Watch for these signals in the field device's next telemetry cycle:
   - `telemetry.qo` `lvl` > 0 PSI on the real well-pressure transducer (this is the headline cure).
   - `_session.qo` opens/closes cleanly; no `alarm.qo` `y:"power"` `to:"CRITICAL_HIBERNATE"` storm.
   - No new `merr` spike in the next daily report (v2.0.49 Fix S1 dynamic DE delay must not have hurt the 9600 baud production path).
   - Optional: `_health.qo` `i2c_cl_err` counter low/zero, meaning v2.0.50 Fix C2 didn't introduce a new failure mode.

**Rollback plan:** v2.0.47–v2.0.49 slot images are committed in the repo at `firmware/112025/client/TankAlarm-Client-secure-v2.0.4{7,8,9}.slot.bin` and as GitHub Release artifacts. Push any earlier one as a Host Firmware target to roll back.

---

### §3.B — HIGH — Bench carrier voltage-divider wiring audit

v2.0.48 Fix 9 (strict priority MPPT > Vin) is the **correct** code-side fix. But it relies on MPPT (Source 1) reporting. On the bench, MPPT is not present (`scOk:0`), so Source 2 (Vin divider) remains primary. The Vin divider on this bench reads ~5 V, which:

- Trips `CRITICAL_HIBERNATE` if config thresholds aren't overridden.
- Makes the Server's new Voltage Monitor dropdown's "Voltage Divider" option look broken on this specific bench.

**Three orthogonal next steps (do at least one):**

1. **DMM-probe the configured `vinMonitor.analogPin`** while the device is running. Compare:
   - Voltage on the ADC pin (should be in 0–3.3 V range, scaled by R1/R2).
   - The `gVinVoltage` reported in telemetry (`v` field, with `vs` confirming the source).
   - The actual rail the divider is physically attached to.
2. **Push a Notehub `data.qi`** with `{"vinMonitor":{"enabled":false}}`. With both Source 1 and Source 2 disabled, `getEffectiveBatteryVoltage()` returns 0 and `updatePowerState()` forces NORMAL. This makes the bench match the post-v2.0.48 contract for "device without battery monitoring".
3. **Use the new Voltage Monitor dropdown** in the Server's `/config-generator`: set to **None** to mean "no voltage source", or **RS485** if the bench is supposed to grow a SunSaver later. The new dropdown is the documented way to express this; pushing legacy `vinMonitor`/`solarCharger` fields directly is deprecated.

**Firmware enhancement still worth considering (DEFERRED):** add an absolute lower-bound plausibility gate. If a 12 V system's only-reporting source claims < 7 V *and* the bench has been running on USB-or-similar for the last N samples (low confidence the battery is real), demote the source to "stale" rather than degrading power state. This would catch wired-to-wrong-rail divider mistakes without masking real low-battery events. Not in v2.0.50 because every fix has been narrow and review-driven.

---

### §3.C — MEDIUM — Surface A0602 bootstrap state in telemetry (Change 5)

Still not in v2.0.48–v2.0.50. The bench works only because we can visually see the GREEN LED; a field deployment in a sealed enclosure has no such signal.

**Recommended fields (Daily Health note minimally; per-sample optionally):**

- `a0602_addr` (uint8) ← `gA0602ManagedAddress`. Hex-formatted display on the dashboard.
- `a0602_mgmt_ok` (bool) ← `gOptaControllerReady`. `false` means the bootstrap failed and the firmware fell back to legacy `resolveCurrentLoopI2cAddress` probing — the operator needs to know which case they're in.
- v2.0.50 specifics worth exposing too:
  - `a0602_reprobe_hits` (uint16) — count of times Fix C1 re-probe found a different address than `gA0602ManagedAddress` (i.e., the module had been externally power-cycled and ended up unmanaged at `0x0A`).
  - `a0602_reprobe_at_0x0a` (uint16) — count of those re-probes that landed on the unmanaged default. If this is non-zero in production, the deferred "maintenance-window re-management" path (§3.E) becomes a priority.

**Estimated effort:** ~25 lines + a release-notes mention; bundle into the next small Client release.

---

### §3.D — LOW — Managed P1 PWM gating (Change 4 from original plan)

Skip until §3.B / §3.E surface a reason. Raw `tankalarm_setPwm()` works at the discovered address. The risk of mapping `AnalogExpansion::beginChannelAsPwm()` to the wrong channel/register and silently failing to power the loop is higher than the cost of leaving this alone.

---

### §3.E — MEDIUM — True A0602 re-management after external power-cycle (deferred by v2.0.50)

Documented in [the v2.0.50 release notes](../release-notes/v2.0.50.md#what-is-intentionally-not-in-this-release) under "True A0602 re-management after external reset". The setup-time `bootstrapA0602Managed()` is the only path that drives an unmanaged A0602 from RED (waiting for address) back to GREEN (assigned at 0x0B). If a field unit's A0602 brownouts (or its loop power is cycled), the C1 re-probe finds it at `0x0A`, **emits a WARNING log line**, and continues — but the module stays unmanaged for the rest of the device's uptime.

**Failure mode in production:** the C1 re-probe will keep finding `0x0A` after every recovery. The framed read may or may not work against an unmanaged A0602 (depends on the module's firmware — undocumented). The `WARNING: A0602 at unmanaged default 0x0A` line is the operator's only signal.

**Three possible paths, listed in order of risk:**

1. **Watchdog-triggered Opta soft-reboot after N consecutive `0x0A` re-probes.** Lowest engineering risk; restarts the device's `setup()` which re-runs `bootstrapA0602Managed()`. Cost: ~30 s of downtime per occurrence. Probably acceptable for a sensor with 30-minute sample interval.
2. **Maintenance-window re-management.** Allow `bootstrapA0602Managed()` to run again at a config-defined time-of-day (e.g., 3 AM local) but ONLY if `gA0602ManagedAddress == 0x0A` (proves the module needs it). Cost: design overhead, complexity, and the prohibition against running `OptaController.begin()` outside `setup()` needs to be carefully re-examined.
3. **Surfacing the gap only.** Add §3.C's `a0602_reprobe_at_0x0a` counter, gather a few weeks of production data, and decide whether 1 or 2 is needed based on actual incidence. **This is the recommended next step** because it costs almost nothing and tells us whether the problem is real.

---

### §3.F — MEDIUM — Activate SunSaver fault/alarm register reads (deferred by v2.0.49)

v2.0.49's "Inconsistency E — documentation only" notes that the hardware fault/alarm registers at `0x002B`, `0x002C`, `0x002E` returned `faults=0x4235` on a physically healthy unit during 2026-04-22 bench-up. Activating those reads without verifying the address map would generate false-alarm SMS to every field install.

The release added a `BENCH-VERIFICATION TODO` in `TankAlarm_Solar.cpp` listing the candidate addresses to probe (`0x0021`, `0x0023`, `0x002F`, `0x0032`) and the bench conditions to induce. **Still requires hands-on bench work** before activation.

Until then, the "Known gaps in fault detection" subsection in the Client README is the operator-facing documentation of which failure modes are NOT currently surfaced (DIP tampering, EEPROM corruption, RTS wiring, battery sense wiring, calibration drift, heatsink overtemp).

---

### §3.G — MEDIUM — v2.0.50 bench-validation TODO (carry forward, per the release notes themselves)

The [v2.0.50 release notes](../release-notes/v2.0.50.md#bench-validation-requested-before-tagging-the-next-release) explicitly request four bench captures before tagging the next release:

1. First three `cl_dur_us` values before/after — expect the same range (warmup excluded from the accumulator, per Fix C2 §9-2).
2. One healthy `mA` sample at no-fluid baseline — expect ~4 mA, not "stuck 18 mA".
3. Address probe log around a forced `recoverI2CBus()` — confirm `WARNING: A0602 at unmanaged default 0x0A` fires when the module is held in reset.
4. Serial capture of all four early-return paths (PWM-NACK, CONFIG-NACK, FUNC-WRONG, READ-FAIL) — confirm all five side-effects fire on each (`gCurrentLoopI2cErrors++`, `gLastClFaultReason`, `currentSensorMa=0`, `sampleReused=true`, retried PWM-off at 100 kHz).

**Status:** none of these captures has been recorded yet on this session. The bench USB-CDC instability on this host (documented in [the A0602 plan doc §5](A0602_ADDRESSING_FINDINGS_AND_FIX_TEST_PLAN_06242026.md#5-how-to-build--flash--capture-on-the-other-computer)) makes serial capture flaky; the Notehub telemetry channel is more reliable for #1 and #2, but #3 and #4 need serial.

**Recommended:** carry these captures forward to the next bench session on a host without the USB-CDC instability.

---

### §3.H — MEDIUM — Server-side and Viewer-side surfacing of the voltage source / re-probe events

v2.0.48 explicitly defers:

- **Server-side dashboard surfacing of the active voltage source.** `ClientMetadata` already stores the voltage via `vinVoltage`/`vinVoltageEpoch` from the top-level `v`/`vs` telemetry fields, but does NOT persist the source string (`"mppt"` / `"vin-divider"`). Adding `char vinSource[16]` to `ClientMetadata` and exposing it on the dashboard would let the operator see the active source at a glance.
- **Viewer source-tag pass-through.** The Viewer ingests `item["v"]` but does not echo `item["vs"]` through its API.

v2.0.50 explicitly defers:

- **Server-side surfacing of the C1 WARNING events / `y:"sensor-degraded"` telemetry note.** Currently the C1 re-probe at `0x0A` only emits a serial WARNING — invisible in production. A formal telemetry event would make this dashboard-observable; combined with §3.C's `a0602_reprobe_at_0x0a` daily counter, the operator would have both a long-term trend signal and per-event alert.

**Estimated effort:** small per item; bundle 2–3 of these together into a Server release.

---

### §3.I — LOW — Multi-day voltage trend statistics (lost with the Notecard `card.voltage` removal in v2.0.48)

v2.0.48 Fix 11 deleted the Notecard `card.voltage` path entirely; with it went the Notecard's built-in 24h / 7d / 30d voltage-change windows. The `dailyChange` / `weeklyChange` / `monthlyChange` fields remain in `BatteryData` as reserved fields populated with 0.

If multi-day trend data on voltage is operationally useful, it has to be reconstructed by the **server** from the per-sample `v`+`vs` history (a SQL query on the telemetry log) or by the **client** maintaining its own rolling windows in flash. The release notes don't commit to either; this is "noted as missing, not yet decided".

---

### §3.J — LOW — Other docs / process items

- **Channel LEDs 2 & 4 stuck yellow through power-cycle.** Not investigated. Out-of-band item for the next bench session with a unit exhibiting the symptom.
- **`read_file` cache staleness.** Confirmed again in this session: cross-check with terminal `Get-Content` for version-bump decisions. No code change.
- **Working-tree hygiene.** This session's pull was clean. The earlier stash entry from a prior session is still in `git stash list` and should be reviewed/dropped during the next housekeeping pass.

---

## 4. Suggested order of operations

1. **Watch the bench** for the next telemetry sample now that v2.0.50 is flashed (Notehub events page). Confirm:
   - Power state stays out of CRITICAL_HIBERNATE (we proved the firmware path with v2.0.47; v2.0.48 Fix 9 hardens it further but only if MPPT is in the picture).
   - `lvl` reading is consistent with whatever the loop is doing (still 0 if no transducer; non-zero if hardware finally cooperates).
   - No new `i2c_cl_err` spike or `merr` spike.
2. **OTA the field unit to v2.0.50** (§3.A) — the only test that matters for the original symptom.
3. **Carrier rail audit on the bench** (§3.B) — DMM-probe the `vinMonitor` pin OR push `vinMonitor.enabled=false`.
4. **Telemetry follow-up** (§3.C + §3.H) — bundle into the next small Client + Server release.
5. **Decide §3.E path** based on whether §3.C surfaces `a0602_reprobe_at_0x0a > 0` in production.

---

## 5. References

- v2.0.47 commit: [b589d95](https://github.com/SenaxInc/SenaxTankAlarm/commit/b589d95) (my ship)
- v2.0.48 tag: [v2.0.48](https://github.com/SenaxInc/SenaxTankAlarm/releases/tag/v2.0.48) — battery source rework
- v2.0.49 tag: [v2.0.49](https://github.com/SenaxInc/SenaxTankAlarm/releases/tag/v2.0.49) — RS-485 MPPT hardening
- v2.0.50 tag: [v2.0.50](https://github.com/SenaxInc/SenaxTankAlarm/releases/tag/v2.0.50) — current-loop hardening
- Release notes: [v2.0.48.md](../release-notes/v2.0.48.md) · [v2.0.49.md](../release-notes/v2.0.49.md) · [v2.0.50.md](../release-notes/v2.0.50.md)
- Code reviews: [BATTERY_VOLTAGE_SAMPLING](CODE_REVIEW_06252026_BATTERY_VOLTAGE_SAMPLING.md) · [RS485_SUNSAVER_COMMUNICATION](CODE_REVIEW_06252026_RS485_SUNSAVER_COMMUNICATION.md) · [CURRENT_LOOP_SENSORS](CODE_REVIEW_06252026_CURRENT_LOOP_SENSORS.md)
- Original A0602 plan: [A0602_ADDRESSING_FINDINGS_AND_FIX_TEST_PLAN_06242026.md](A0602_ADDRESSING_FINDINGS_AND_FIX_TEST_PLAN_06242026.md)
- Notehub events (bench): https://notehub.io/project/app:f0a8c2c9-6835-49ce-9f7f-1c9540754044/events
