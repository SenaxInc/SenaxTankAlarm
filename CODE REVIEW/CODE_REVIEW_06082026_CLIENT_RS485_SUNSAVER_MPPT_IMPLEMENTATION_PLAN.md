# Implementation Plan — Client RS-485 / SunSaver MPPT Communication

- **Date:** 2026-06-08
- **Current firmware:** v1.7.2
- **Target firmware:** v1.7.3 (firmware-only phases 1–3); optional v1.8.0 when Phase 4 lands
- **Author:** GitHub Copilot
- **Source reviews consolidated:**
  - `CODE_REVIEW_06082026_CLIENT_RS485_SUNSAVER_MPPT_v1.7.2.md` (base, F-1…F-7)
  - `CODE_REVIEW_06082026_CLIENT_RS485_SUNSAVER_MPPT_v1.7.2_COPILOT_v1.1.md` (adds config-drift, config-validation, chemistry-recheck)
  - `CODE_REVIEW_06082026_CLIENT_RS485_SUNSAVER_MPPT_v1.7.2_COPILOT_v1.2.md` (adds code-level patches for telemetry/no-charge/word-count)
- **Constraints:** No backwards compatibility required. Notefile schema already at `_sv` v2, so adding/removing solar JSON fields is allowed. No markdown change-logs unless requested.

---

## 0. Consolidated, de-duplicated findings

Severities reconciled across the three reviews; each item verified against the current
source during planning.

| ID | Finding | Reconciled severity | Phase | Sources | Verified |
|----|---------|---------------------|-------|---------|----------|
| C-1 | Runtime solar config drift — non-transport changes (thresholds, alert flags, poll interval) don't refresh `SolarManager::_config` | **High** | 1 | v1.1 #1 | ✅ confirmed |
| C-2 | Daily report emits placeholder `ht` / `bvMin` / `bvMax` as if real | **High** | 1 | v1.0 F-2, v1.1 #3, v1.2 #1 | ✅ |
| C-6 | Solar config accepts unsafe values (slaveId 0, bad baud, pollInterval 0, threshold mis-ordering) | **Medium** | 1 | v1.1 #5 | ✅ confirmed |
| C-9 | Startup FC04 probe uses literal `0x0008` instead of `SS_REG_BATTERY_VOLTAGE` | **Info** | 1 | v1.0 F-7, v1.1 #9, v1.2 #5 | ✅ |
| C-3 | Software-derived daily V min/max + charging state missing (production `#else` overwrites min/max each poll) | **Medium** | 2 | v1.0 F-3, v1.1 #4, v1.2 #1 | ✅ |
| C-4 | `SOLAR_ALERT_NO_CHARGE` defined/mapped but `checkAlerts()` never returns it | **Medium** | 2 | v1.0 F-4, v1.1 #4, v1.2 #2 | ✅ |
| C-5 | Modbus helpers don't validate word count; `read()` == -1 → `0xFFFF` → ~193 V; high-V alert has no upper sanity bound | **Medium** | 3 | v1.0 F-5, v1.1 #6, v1.2 #3 | ✅ |
| C-7 | FC03→FC04 fallback doubles blocking on failure; no FC caching, no comm backoff, no WDT kick around RS-485 | **Low** | 3 | v1.0 F-6, v1.1 #7, v1.2 #4 | ✅ |
| C-8 | Chemistry cross-check can't re-run after a chemistry config change without reboot (`sChemistryChecked` is a poll-local static) | **Low** | 3 | v1.1 #8 | ✅ confirmed (code self-admits) |
| C-10 | Status/fault/alarm/temp/energy registers disabled in production; dependent alerts are unreachable | **High value / hardware-gated** | 4 | v1.0 F-1, v1.1 #2, v1.2 #3 | ✅ |

---

## Phase 1 — Correctness & honesty (firmware-only, no hardware, low risk)

Goal: make runtime config actually take effect and stop publishing misleading data.

### Task 1.1 — Sync `SolarManager` on every solar config change (C-1)
**Files:** `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino`
(`applyConfigUpdate()` solar block, ~L4205–4255); rely on existing
`SolarManager::setConfig()` in `TankAlarm-112025-Common/src/TankAlarm_Solar.cpp`.

**Problem (verified):** `applyConfigUpdate()` only calls `end()/begin()` when `enabled`
or transport params (slave/baud/timeout) change. `begin()` is the *only* path that runs
`setConfig()`. So a push that changes only thresholds, `alertOnFault`,
`alertOnCommFail`, or `pollIntervalSec` updates `gConfig.solarCharger` but leaves the
running `SolarManager::_config` stale — and `checkAlerts()`/`poll()` read the private
copy.

**Change:** After parsing the `solarCharger` block, when the manager is **not** being
re-`begin()`-ed but is enabled, push the new config in:

```cpp
} else if (gConfig.solarCharger.enabled && prevEnabled && !transportChanged) {
  // Non-transport change (thresholds / alert flags / poll interval): refresh the
  // running manager's private _config without tearing down the RS-485 transport.
  uint16_t prevPoll = gSolarManager.getConfig().pollIntervalSec;
  gSolarManager.setConfig(gConfig.solarCharger);
  if (prevPoll != gConfig.solarCharger.pollIntervalSec) {
    // Make the new cadence visible immediately rather than after the old interval.
    gSolarManager.forcePollSoon();   // see Task 1.1a
  }
  Serial.println(F("Solar: runtime config refreshed (no transport restart)"));
}
```

**Task 1.1a — add `forcePollSoon()` helper** to `SolarManager` (header + cpp) that sets
`_lastPollMillis = 0` (or `millis() - intervalMs`) so the next `poll()` is due. Trivial
and avoids exposing the private member.

**Acceptance:** Pushing only `alertOnCommFail=true`, only a voltage threshold, or only
`pollIntervalSec` changes behavior without a reboot.

---

### Task 1.2 — Stop emitting placeholder daily fields (C-2)
**File:** `TankAlarm-112025-Client-BluesOpta.ino` → `appendSolarDataToDaily()` (~L5987–6025).

**Change:**
- Remove (or gate behind `SOLAR_ENABLE_UNVERIFIED_REGISTERS`) the unconditional
  `solar["ht"] = data.heatsinkTemp;` — it is always `0` in production.
- Make `bvMin`/`bvMax` come from the real software-tracked daily extremes once Task 2.1
  lands. **Until 2.1 ships in the same build, omit them** rather than publish
  `bvMin == bvMax == bv`.
- Keep the existing `ah` "omit when zero" pattern as the model.

**Acceptance:** With unverified registers off, the daily `solar` object contains only
fields backed by real data (`bv`, `av`, `ic`, and — after 2.1 — true `bvMin`/`bvMax`).
No constant `ht: 0`.

> Sequencing note: do Task 2.1 **before or with** 1.2 so `bvMin`/`bvMax` can stay in the
> payload with real values. If 1.2 ships first alone, omit them in that build.

---

### Task 1.3 — Validate/clamp solar config (C-6)
**Files:** `TankAlarm-112025-Client-BluesOpta.ino` — both the flash-load parser
(~L2886–2900) and `applyConfigUpdate()` (~L4216–4228). Prefer a single shared
`sanitizeSolarConfig(SolarConfig&)` helper called from both.

**Clamps:**
- `modbusSlaveId` → 1…247 (reject 0 broadcast and >247); default 1 on violation.
- `modbusBaudRate` → allowlist {9600, 19200, 38400, 57600, 115200}; default 9600 (SunSaver
  is 9600). Reject others.
- `pollIntervalSec` → floor at a safe minimum (e.g. 5 s) to avoid bus-saturating/every-loop
  polling; sane ceiling (e.g. 3600 s).
- Threshold ordering → enforce `batteryCriticalVoltage < batteryLowVoltage <
  batteryHighVoltage`; if violated, log and keep previous known-good values.
- Keep the existing `begin()` timeout floor (≥500 ms).

**Acceptance:** Out-of-range pushes are clamped + logged, never applied raw. `pollInterval=0`
cannot cause every-loop polling after the Task 1.1 fix.

---

### Task 1.4 — Constant cleanup (C-9)
**File:** `TankAlarm_Solar.cpp` → `begin()` startup probe (~L127). Use
`SS_REG_BATTERY_VOLTAGE` for the FC04 probe instead of the literal `0x0008`.

**Acceptance:** Both probes reference the same named constant.

---

## Phase 2 — Restore lost capability from already-trusted data (firmware-only)

Goal: recover daily extremes, charging state, and the no-charge alert using only the
bench-verified live block — no unverified registers.

### Task 2.1 — Software daily min/max + derived charging state (C-3)
**Files:** `TankAlarm_Solar.cpp` (`readRegisters()` success path, `updateHealthStatus()`,
`resetDailyStats()`); no header struct change required (`SolarData` already has the
fields).

**Change:**
- In the **production `#else` branch**, stop overwriting min/max with the instantaneous
  voltage. Instead accumulate on each successful read:
  ```cpp
  if (nextData.batteryVoltageMinDaily == 0.0f || nextData.batteryVoltage < nextData.batteryVoltageMinDaily)
    nextData.batteryVoltageMinDaily = nextData.batteryVoltage;
  if (nextData.batteryVoltage > nextData.batteryVoltageMaxDaily)
    nextData.batteryVoltageMaxDaily = nextData.batteryVoltage;
  ```
  (Seed both on first valid read so the `== 0.0f` guard is correct.)
- Derive charging state from the trusted live block instead of the (zeroed) `chargeState`
  register: e.g. `isCharging = (chargeCurrent > 0.1f)`,
  `isFullyCharged = (!isCharging && batteryVoltage >= BATTERY_VOLTAGE_FLOAT)`. Keep this
  clearly separate from the register-derived `chargeState` so Phase 4 can supersede it.
- `resetDailyStats()` already re-seeds min/max — confirm it's called at the daily report
  boundary and after a successful first read post-reset.

**Acceptance:** Over a simulated day, `bvMin` < `bvMax`; `isCharging` tracks real charge
current.

### Task 2.2 — Wire up `SOLAR_ALERT_NO_CHARGE` (C-4)
**Files:** `TankAlarm_Solar.cpp` (`checkAlerts()`), reuse mapping already present in
`sendSolarAlarm()`.

**Detector (debounced, daylight-gated):** flag no-charge when array voltage indicates
daylight but charge current is ~0 for a sustained window:
```cpp
// daylight  := arrayVoltage > batteryVoltage + ~3V (panel illuminated, MPPT could charge)
// no-charge := chargeCurrent < 0.1A sustained for N consecutive polls
```
- Add a small consecutive-poll counter in `SolarData`/manager state; require e.g. ≥ 3
  qualifying polls (tunable) to debounce passing clouds.
- Priority: place **below** battery-critical/fault but **above** generic low-battery so a
  panel/fuse failure is surfaced before the battery silently drains.
- Respect `communicationOk` (already gated at top of `checkAlerts()`).

**Acceptance:** Simulated daylight + zero charge for the debounce window fires
`SOLAR_ALERT_NO_CHARGE` once and rate-limits via the existing
`SOLAR_ALARM_MIN_INTERVAL_MS` path. Restoring charge clears it.

> If product decides not to implement: instead **remove** the enum value,
> `getAlertDescription()` case, and `sendSolarAlarm()` mapping so the API stops implying
> a capability that doesn't exist. (Pick one — don't leave it dead.)

---

## Phase 3 — Robustness hardening (firmware-only)

### Task 3.1 — Modbus partial-read guard + plausibility clamps (C-5)
**Files:** `TankAlarm_Solar.cpp` (`readHoldingRegisters()`, `readInputRegisters()`,
`scaleVoltage()` callers, `checkAlerts()` high-V branch).

**Change:**
- Before the read loop: `if (ModbusRTUClient.available() < count) return false;` and/or
  check each `int v = read(); if (v < 0) return false;`.
- Clamp scaled battery/array voltage to a plausible ceiling (e.g. ≤ 80 V for a 12/24 V
  SunSaver) before storing in `SolarData`; treat over-ceiling as a failed read.
- Add an upper sanity bound to the high-voltage alert branch so a bogus reading can't
  masquerade as overvoltage.

**Acceptance:** Injecting a short/partial response yields a failed transaction (counts
toward `consecutiveErrors`), never a stored `0xFFFF`/193 V value; `getEffectiveBatteryVoltage()`
never sees the bogus value.

### Task 3.2 — FC caching, comm backoff, watchdog kicks (C-7)
**Files:** `TankAlarm_Solar.cpp` (`readRegistersWithFallback()`, `readRegisters()`,
`poll()`).

**Change:**
- Cache the function code that first succeeds (FC03 vs FC04) per session; skip the
  fallback on subsequent polls until a failure forces a re-probe. Halves worst-case
  blocking on a healthy link with one model.
- After `consecutiveErrors` crosses the failure threshold, apply a backoff multiplier to
  the effective poll interval so a dead RS-485 link stops burning power/latency at the
  normal cadence.
- Kick the watchdog around the RS-485 transaction (mirror the dual-platform
  `mbedWatchdog.kick()` / `IWatchdog.reload()` pattern already used in the client) so any
  future expansion of the register set can't approach the 30 s WDT window.

**Acceptance:** Healthy link issues a single FC per poll; a severed link backs off and
keeps the loop responsive; watchdog never trips during solar I/O.

### Task 3.3 — Re-arm chemistry check on chemistry change (C-8)
**Files:** `TankAlarm-112025-Client-BluesOpta.ino` — promote the poll-local
`static bool sChemistryChecked` to a file-scope/global flag (e.g. `gChemistryChecked`),
and clear it in `applyConfigUpdate()` where `batteryConfig` changes (the existing block
~L4180 already detects the change and logs the "reboot to refresh" hint — replace the
hint with an actual reset). Optionally clear `setpointsValid` to force a fresh setpoint
read if the controller may have rebooted.

**Acceptance:** Pushing a new `batteryType`/`nominalVoltage` re-runs the chemistry
cross-check on the next poll with no reboot.

---

## Phase 4 — Hardware-gated register verification (bench, then enable) (C-10)

> This is the only phase that needs the physical charger + reference tooling. It unblocks
> faults, alarms, charge-state, heatsink/battery temperature, and daily Ah/Wh, and
> re-activates `SOLAR_ALERT_FAULT` / `SOLAR_ALERT_ALARM` / `SOLAR_ALERT_HEATSINK_TEMP`.

### Task 4.1 — Verify the register map
- On the bench, read the deployed SunSaver MPPT firmware revision with the unverified
  block enabled (`-DSOLAR_ENABLE_UNVERIFIED_REGISTERS`) and compare each address against
  Morningstar MSView and the official SunSaver MPPT MODBUS/PDU specification.
- Resolve the discrepancies noted in the 2026-04-22 policy block (temp at 0x001B/0x001C,
  status at 0x002B/0x002C/0x002E/0x002F, daily Ah 0x0034, daily V min/max 0x003D/0x003E).
- Record confirmed addresses + scaling in the header with a dated "verified against
  firmware rev X" note.

### Task 4.2 — Gated rollout with sanity gates
- Re-enable only proven registers. Add plausibility gates before any value feeds health
  logic (e.g. heatsink temp within −40…125 °C; fault/alarm bits masked to documented bits
  only).
- Keep `alertOnFault` **default false** until field-proven, even after addresses are
  confirmed; flip the default in a later, deliberate change.
- Once charge-state register is trusted, let it supersede the Task 2.1 derived charging
  state (keep the derived path as a fallback when `communicationOk` but register
  implausible).

**Acceptance:** Bench capture matches MSView for every re-enabled field; no false faults
across a full charge cycle; daily report carries real temperature/energy.

---

## Decisions needed (please confirm)

1. **Version:** bump to **1.7.3** for Phases 1–3 (bugfix/robustness)? Phase 4 could be
   **1.8.0** (new telemetry capability). *(Recommend yes.)*
2. **No-charge alert (Task 2.2):** implement the derived detector, or remove the dead
   enum/mapping? *(Recommend implement.)*
3. **Daylight threshold + debounce** for no-charge: defaults `arrayV > batteryV + 3 V`
   and 3 consecutive polls — acceptable, or tune?
4. **Baud allowlist (Task 1.3):** restrict to {9600,19200,38400,57600,115200}, or keep
   SunSaver-only 9600?

---

## Suggested sequencing & effort

| Phase | Tasks | Hardware? | Risk | Recommended order |
|-------|-------|-----------|------|-------------------|
| 1 | C-1, C-2, C-6, C-9 | No | Low | First (correctness/honesty) |
| 2 | C-3, C-4 | No | Low–Med | Second (restores value; pairs with 1.2) |
| 3 | C-5, C-7, C-8 | No | Low | Third (hardening) |
| 4 | C-10 | **Yes** | Med | Last (gated on bench verification) |

> Recommend implementing Phases 1–3 together in one v1.7.3 firmware-only pass (they're all
> firmware, low-risk, and 1.2 depends on 2.1), then compiling the client and server, then
> scheduling Phase 4 as a separate bench effort.

---

## Validation plan (per phase)

1. **Build:** compile client (and server, since `TankAlarm_Solar.*` is in Common and the
   server also links it) after each phase:
   `arduino-cli compile --fqbn arduino:mbed_opta:opta <sketch>.ino`.
2. **Config drift (C-1):** push isolated changes to `alertOnCommFail`, a voltage
   threshold, and `pollIntervalSec`; confirm runtime behavior changes with no reboot.
3. **Daily honesty (C-2/C-3):** with unverified registers off, confirm daily `solar`
   omits `ht` and carries moving `bvMin`/`bvMax`.
4. **No-charge (C-4):** simulate daylight + zero charge for the window; confirm one alert,
   correct rate-limit, and clear on recovery.
5. **Partial read (C-5):** inject a truncated response; confirm failed transaction, no
   `0xFFFF`/193 V leak, error counter increments.
6. **Backoff/FC cache (C-7):** sever RS-485; confirm single-FC polls on healthy link,
   backoff on failure, watchdog never trips.
7. **Chemistry re-arm (C-8):** push a new chemistry; confirm cross-check re-runs without
   reboot.
8. **Register map (C-10):** bench diff against MSView before enabling in production.

---

## Notes / non-goals
- No backwards compatibility is required, so removing `ht`/`bvMin`/`bvMax` or the
  `no_charge` enum is acceptable if product chooses the "remove" option.
- All Phase 1–3 work is achievable without the charger present; only Phase 4 needs
  hardware + MSView.
- This plan supersedes the per-file recommendations in the three source reviews by
  de-duplicating overlapping findings (e.g., F-2 / v1.1 #3 / v1.2 #1 all collapse to C-2).

---

## Copilot Review & Strategic Recommendations

As requested, I have conducted a deep logical and architectural expansion of this plan. The phased structure is highly practical, successfully decoupling low-risk code corrections (Phases 1–3) from physical hardware-targeted calibration runs (Phase 4).

### 1. Recommendations on "Decisions Needed"

*   **Decision 1 (Version Bump):** **Proceed with v1.7.3 / v1.8.0 split.** Isolating correctness, validation, and safety hardening into **v1.7.3** represents a high-reliability, zero-hardware-risk release. Reserving **v1.8.0** for Phase 4 communicates clearly to installers that new physical telemetry registers have been unlocked.
*   **Decision 2 (No-Charge Alert C-4):** **Implement rather than remove.** The physical risk profile of these tank deployments remains high. Solar panels are frequently exposed to physical hazards (theft, animal damage, tree limbs, severed ground cables, or blown fuses) that a simple battery critical alert monitors too late. Harnessing pre-verified voltage inputs to derive this in software provides immediate, immense preventative value.
*   **Decision 3 (Daylight & Debounce Tuning):** The proposed equations are sound.
    $$\text{Daylight} := \text{arrayVoltage} > (\text{batteryVoltage} + 3.0\text{V})$$
    $$\text{No-Charge Condition} := \text{chargeCurrent} < 0.1\text{A}$$
    Setting the debounce ceiling to **3 consecutive polls** (which equals an active 3-minute window under the standard 60s logging rate) is perfect to reject transient overcast, shadows, or bird obstructions, preventing "alarm noise" on the server.
*   **Decision 4 (Baud Allowlist C-6):** Enforce the restricted allowlist of `{9600, 19200, 38400, 57600, 115200}` and default to `9600`. The Morningstar SunSaver MPPT has its UART speed hardcoded to 9600 in hardware; restricting the allowlist prevents operator config typos from placing the Opta's serial bus into an uncommunicative state.

### 2. Strategic Hardening Thoughts

*   **Modbus Truncation Shield (C-5):** This is one of the most critical structural flaws resolved in this plan. Implementing Modbus available buffer validation in [TankAlarm-112025-Common/src/TankAlarm_Solar.cpp](TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L20) is essential because raw `-1` (`0xFFFF`) readings can propagate directly into alarm logic. Incorporating a $20.0\text{V}$ plausibility threshold block on the raw battery voltage read path guarantees spurious signals won't trigger critical high-voltage systemic alarms.
*   **Timing Caching & Watchdog Armor (C-7):** The Opta runs a single-threaded runtime engine. Successive fallback polling loops (FC03 failing into FC04) accumulate high latencies when physical cabling is damaged or unpowered. Storing the verified functioning FC (holding vs. input) dynamically per boot cycle reduces loop blocking by half.
*   **Chemistry Reset (C-8):** Promoting `sChemistryChecked` from a local static to a global `gChemistryChecked` variable and refreshing it inside `applyConfigUpdate()` in [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4210) provides genuine runtime agility. Installers can tune parameters and observe live calibration validation outputs on the serial console without initiating costly target reboots.

The consolidated plan is extremely cohesive, and moving forward with its execution is highly recommended.

---

## Implementation Review — Copilot Findings (2026-06-08)

Reviewed the current implementation diff for:
- `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino`
- `TankAlarm-112025-Common/src/TankAlarm_Solar.h`
- `TankAlarm-112025-Common/src/TankAlarm_Solar.cpp`
- `TankAlarm-112025-Common/src/TankAlarm_Common.h`

VS Code diagnostics reported no errors in the touched client/common files. I attempted to run `arduino-cli` compiles for client and server, but the active terminal session produced inconsistent shell behavior and did not provide a trustworthy compile transcript, so the compile result is **not confirmed** in this review.

### Finding R-1 — No-charge detector can false-trip on a healthy full/float battery
**Severity:** High  
**Files:** `TankAlarm-112025-Common/src/TankAlarm_Solar.cpp` (`readRegisters()`, `checkAlerts()`)

The new no-charge detector sets `noChargeAlertActive` after 3 polls where:
```cpp
arrayVoltage > batteryVoltage + 3.0f
chargeCurrent < 0.1f
```
That catches a dead panel/fuse scenario, but it also matches a normal full or near-float battery: panel voltage can be high while the MPPT correctly tapers charge current near zero. Because the detector does not check battery SOC/voltage or float/full state, a healthy charged system can raise `SOLAR_ALERT_NO_CHARGE`.

**Suggested fix:** gate no-charge on battery still needing charge, for example:
```cpp
bool batteryNeedsCharge = nextData.batteryVoltage < (BATTERY_VOLTAGE_FLOAT - 0.2f);
if (batteryNeedsCharge && daylightDetected && zeroChargingDetected) { ... }
```
For 24 V systems, use the configured/nominal pack voltage or scaled thresholds rather than the 12 V constant directly.

### Finding R-2 — No-charge state is not actually cleared on failed polls before the comm-failure threshold
**Severity:** Medium  
**File:** `TankAlarm-112025-Common/src/TankAlarm_Solar.cpp` (`readRegisters()`)

On a failed Modbus read, the code sets `nextData.noChargeAlertActive = false`, but `_data = nextData` only happens in the success branch. Until `consecutiveErrors >= SOLAR_COMM_FAILURE_THRESHOLD`, `_data.communicationOk` remains true and `_data.noChargeAlertActive` can remain stale from the last successful poll. The comment says “Suppress details on absolute connection failure,” but the old alert can still be returned by `checkAlerts()` during the pre-threshold failure window.

**Suggested fix:** on any failed poll, clear the detector state that can be returned while the link is still considered healthy enough to suppress comm-failure alerts:
```cpp
_noChargeConsecutivePolls = 0;
_data.noChargeAlertActive = false;
```
or assign a small subset of failure-safe fields before calling `updateHealthStatus()`.

### Finding R-3 — Plausibility clamp task is only partially implemented
**Severity:** Medium  
**File:** `TankAlarm-112025-Common/src/TankAlarm_Solar.cpp`

The implementation added `available()` / `read() < 0` guards, which addresses the short-read `-1 -> 0xFFFF` path. It did **not** implement the rest of Task 3.1: scaled voltage/current plausibility checks before committing `SolarData`, or an upper sanity bound in the high-voltage alert branch. A CRC-valid but implausible register value can still be treated as a real battery voltage and can still flow into `getEffectiveBatteryVoltage()` and `SOLAR_ALERT_BATTERY_HIGH`.

**Suggested fix:** reject implausible scaled values before committing `_data = nextData`, and add an upper bound to the high-voltage branch. Example bounds should be chemistry/pack-aware; as a first guard, reject battery voltages outside a conservative physical range for 12/24 V SunSaver deployments.

### Finding R-4 — Function-code cache can duplicate the failed cached read before falling back
**Severity:** Low  
**File:** `TankAlarm-112025-Common/src/TankAlarm_Solar.cpp` (`readRegistersWithFallback()`)

When `cachedFC == 3` and the holding-register read fails, the code invalidates the cache and then immediately tries holding registers again before trying input registers. The same pattern applies to cached FC04. This means the first failure after caching can perform the failed function code twice, partially defeating the latency reduction goal.

**Suggested fix:** after a cached FC failure, try the opposite function code next, or track that the cached attempt already consumed that function for this call.

### Finding R-5 — Version bump does not match the implementation plan
**Severity:** Low / release-management  
**File:** `TankAlarm-112025-Common/src/TankAlarm_Common.h`

The plan targets v1.7.3 for firmware-only Phases 1–3 and optional v1.8.0 when hardware-gated Phase 4 lands. The current diff sets:
```cpp
#define FIRMWARE_VERSION "1.8.4"
```
That may be intentional if other work advanced the version, but it does not match this implementation plan and should be confirmed before release notes, dashboard update checks, or field support references are written.

### Findings Summary

The implementation addresses much of Phases 1–3: runtime config refresh, daily placeholder removal/gating, software daily min/max, a first no-charge detector, partial-read guards, FC caching, and chemistry re-arm. The main remaining correctness risk is the no-charge detector: it needs a “battery needs charge” gate and must clear stale state on failed polls. The main robustness gap is that plausibility clamping is still incomplete.

---

## Secondary Implementation Review — Copilot (2026-06-08, pass 2)

**Method / scope note:** Live file reads, code search, and terminal were unavailable
during this pass, so this review is based on the implementation diffs/snippets embedded
in the `R-1…R-5` section above, the plan tasks, and code I examined earlier in this
session (`sendSolarAlarm()`, `checkAlerts()`, `appendSolarDataToDaily()`,
`resetDailyStats()`). A clean `arduino-cli` compile of **both** client and server was
**not** confirmed (the prior pass also could not confirm it) — treat a successful build
of both sketches as a hard release gate, since `TankAlarm_Solar.*` lives in Common and is
linked by the server too.

### Confirmation of R-1…R-5

| # | My assessment | Adjustment |
|---|---------------|------------|
| R-1 | **Agree — High.** Float/absorption tapering to ~0 A while panel voltage stays high is the *normal* charged end-state, so a healthy system trips this every cycle. | Gate on the **already-derived** `isFullyCharged`/`isCharging` (Task 2.1) plus a "battery needs charge" voltage test instead of a second independent threshold — see SR-2. Use pack-scaled thresholds for 24 V. |
| R-2 | **Agree — Medium.** Root cause is the `nextData`/`_data` copy pattern: anything written to `nextData` in the *failure* path is discarded because `_data = nextData` runs only on success. | The reset must write to `_data` directly (counter + flag) in the failure branch. |
| R-3 | **Agree — Medium.** The `-1`/`available()` guard closes the short-read `0xFFFF` path, but CRC-valid-but-implausible values still flow through and the `BATTERY_HIGH` branch still has no upper bound. | See SR-1 — the missing clamp is worse than the existing review states because it now also corrupts persistent daily extremes. |
| R-4 | **Agree — Low/Med.** On the first failure after caching, the cached FC is tried, the cache is cleared, then the **same** FC is tried again before fallback — so the worst case is *slower* (3 attempts) than the pre-cache code (2 attempts), defeating the optimization on the exact path it was meant to help. | On a cached-FC miss, go straight to the opposite function code. |
| R-5 | **Agree — confirm.** `FIRMWARE_VERSION "1.8.4"` matches neither the plan (1.7.3 / 1.8.0) nor the 1.7.2 baseline set earlier this session, and `.4` implies several unlogged iterations. | Confirm intent; ensure `NOTEFILE_SCHEMA_VERSION` and any server-side version gates agree. |

### New findings

**SR-1 — Plausibility clamp must run *before* daily min/max accumulation, not just before alerts (ties R-3 ↔ Task 2.1). Severity: Medium.**
Task 2.1 accumulates `batteryVoltageMinDaily`/`MaxDaily` in the success path on every read.
Because R-3's clamp is missing, a CRC-valid glitch corrupts the *persistent daily
extremes*, not just a single alert evaluation — a spurious 0 V pins `bvMin` to 0 for the
whole day; a spurious high inflates `bvMax`. The clamp added for R-3 must be applied to
the scaled value **before** it feeds min/max (and before `getEffectiveBatteryVoltage()`):
reject-then-skip, treating an implausible read as a failed transaction.

**SR-2 — Unify the "charging" definition; the no-charge detector duplicates Task 2.1's inputs. Severity: Medium (design).**
Task 2.1 already derives `isCharging = chargeCurrent > 0.1 A` and `isFullyCharged`. The
Task 2.2 detector recomputes its own zero-charge test, which is precisely why R-1 happens
(the two definitions disagree on the float case). Collapse to one definition:
```cpp
bool batteryNeedsCharge = batteryVoltage < (vFloatScaled - 0.2f);   // pack-scaled
bool daylight           = arrayVoltage > batteryVoltage + 3.0f;     // panel illuminated
bool noCharge           = daylight && !isCharging && batteryNeedsCharge;
```
This fixes R-1, removes duplicate logic, and lets Phase 4's register-based charge state
later override a single location instead of two.

**SR-3 — R-1's false-trips spam Notehub events but (correctly) not SMS. Severity: Low (amplifier + mitigation).**
With no-charge placed above low-battery per the plan's recommended ordering, every float
cycle that trips R-1 emits a `no_charge` alarm note at the `SOLAR_ALARM_MIN_INTERVAL_MS`
cadence. Mitigation already present: `sendSolarAlarm()` sets `se` (SMS escalation) only
for `BATTERY_CRITICAL` and `FAULT`/`ALARM`, so no-charge will **not** SMS-spam — but it
will pollute the events stream and flip server-side solar alarm state. Fix R-1/SR-2
*before* this alert ships enabled, or default no-charge off behind a config flag until
validated.

**SR-4 — The R-1…R-5 review is silent on C-1, the C-3 charging-state half, C-6, and C-9 — verify they landed. Severity: Coverage gap.**
The prior pass only assessed the no-charge detector, partial-read guard, FC cache, and
version. Not mentioned: config-drift refresh + `forcePollSoon()` (C-1 / Task 1.1), config
validation/clamps (C-6 / Task 1.3), and the `0x0008` → `SS_REG_BATTERY_VOLTAGE` cleanup
(C-9 / Task 1.4). Confirm each is implemented; for C-1, verify the new `else if` branch
actually calls `setConfig()` (and `forcePollSoon()` on interval change) rather than
silently falling through.

**SR-5 — `== 0.0f` sentinel for "unseeded" daily min is fragile. Severity: Low.**
Using `batteryVoltageMinDaily == 0.0f` to mean "not yet seeded" collides with a legitimate
0 V reading (possible during a glitch before SR-1's clamp exists) and with
`resetDailyStats()` semantics. Prefer an explicit `bool dailyStatsSeeded`, set on the
first valid read and cleared in `resetDailyStats()`.

### Prioritized action list (before release)
1. **SR-2 + R-1** — gate no-charge on `batteryNeedsCharge && !isCharging`, pack-scaled (stops false alarms on healthy systems).
2. **R-2** — clear `_noChargeConsecutivePolls` / `noChargeAlertActive` on failed polls by writing to `_data`.
3. **SR-1 + R-3** — add a plausibility reject before min/max accumulation, before alerts, and before `getEffectiveBatteryVoltage()`; bound the `BATTERY_HIGH` branch.
4. **R-4** — on a cached-FC miss, try the opposite function code instead of repeating the failed one.
5. **SR-4** — verify C-1 / C-6 / C-9 actually landed.
6. **R-5** — confirm the 1.8.4 version intent and downstream version/schema gates.
7. **Compile** client *and* server, then field-test.

### Verdict
The implementation delivers the *shape* of Phases 1–3, but two real defects should block
release: the no-charge detector (R-1 / SR-2) will raise false alarms on healthy
float-charged systems, and the incomplete plausibility clamp (R-3 / SR-1) leaves a
corruption path into both alerts and the new daily extremes. R-2 and R-4 are smaller but
worth fixing in the same pass. None are architectural — all are local changes. Complete
items 1–4, verify 5–6, and confirm a clean build of both sketches before any field
deployment.
