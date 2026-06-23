# Code Review — OTA "Stuck on Old Version": Silent & Fragile Update-Check Path

**Date:** 2026‑06‑16
**Author:** GitHub Copilot (for human review)
**Component:** `TankAlarm-112025-Client-BluesOpta` — host‑pull MCUboot OTA check/apply loop
**Trigger:** A v1.9.28 image was downloaded to the Notecard and sat **"Ready"** for ~4.5 hours, but the client kept running **v1.9.27** and never attempted to apply it. The device was alive and un‑rebooted the whole time.
**Status:** Diagnosis + proposed fixes. **No code changed by this document.** The companion file `CODE_REVIEW_06152026_OTA_VS_USB_UPDATE_FAILURE.md` (§16–§17) covers the *CI signing* fix and the confirmed end‑to‑end OTA success; **this** document covers why the **on‑device check** silently failed to apply a ready image.

> Honesty note: the *signing/format* path is proven working (we OTA‑swapped 1.9.27 → 1.9.28 on the bench). The findings below explain why the **automatic** check did not fire during the field‑like window. The single strongest, fully‑provable finding is the **lack of observability** (Findings 1). The exact silent exit that fired during the 4.5‑hour window cannot be confirmed post‑hoc precisely *because* of that missing logging — which is itself the point.

---

## 1. Executive Summary

The OTA **delivery** worked (Notehub → Notecard: "successfully downloaded"). The OTA **apply** never started: Notehub shows no host‑side `staged for MCUboot` or `firmware update failed` status, and the device boot log shows no staging/rollback. That means `enableDfuMode()` never ran — the device never detected the ready image during its hourly checks.

The update‑check path has several **silent early‑exits** and **fragility points** that, on a bench unit with a flaky I²C bus, can cause every hourly check to miss the ready image while emitting **zero diagnostics** to serial or Notehub. The result is an OTA that "just doesn't happen," with nothing to look at.

The actionable conclusions:

1. **Observability is the root problem.** Every branch of the check that decides *not* to apply is silent. Fix this first; it makes everything else diagnosable.
2. **The check is fragile and slow:** one un‑retried `dfu.status` per hour, a timer that resets even when the check is skipped, a full‑hour first‑check delay, and a hard dependency on Notecard health that has no OTA‑specific recovery.
3. **The bench environment makes it worse:** an A0602 current‑loop with no sensor NACKs on the shared I²C bus (`i2c-recovery count: 7`), increasing the odds that the once‑per‑hour query coincides with bus trouble.

---

## 2. Evidence

From the device's notes on Notehub during/after the window:

| Note | Key fields | What it proves |
|---|---|---|
| `_health.qo` | `"method":"dfu"`, `"text":"DFU host firmware ready: successfully downloaded"`, `voltage 4.68`, `voltage_mode "usb"` | Notecard **has** the image; bench is USB‑powered (4.68 V is the Notecard rail, not a battery). |
| `alarm.qo` | `fv:"1.9.27"`, `y:"sensor-fault"`, ~2.4 h **after** ready | Device **alive on 1.9.27**, no reboot, well into the window. |
| `diag.qo` | `ev:"i2c-recovery"`, `count:7`, `i2c_errs:0`, `trigger:1` | The device has been **recovering its I²C bus** repeatedly. |
| `telemetry.qo` | `fv:"1.9.28"` | Post‑test: the OTA we forced succeeded (see companion §17). |
| Notehub Host‑FW lifecycle | `DFU Initiated → Pending sync → Download started → Downloading → Ready "Successfully downloaded"` — **and nothing after** | The **host never reported** a staging attempt → `enableDfuMode()` never ran. |

Boot log (bench): `Battery voltage monitoring enabled` but **no** `Vin monitor enabled:` line, and solar Modbus failing → all three voltage sources inactive (see Finding 4 analysis) → power state resolves to **NORMAL**, so power‑state gating is *not* the blocker.

Passive (no‑reset) serial capture: **0 lines in 150 s, no boot banner** → device is **stable, not reset‑looping**.

---

## 3. Findings

Severity: **HIGH** = should fix; **MEDIUM** = fix recommended; **LOW** = minor/corner.

### Finding 1 — OTA check path is effectively invisible (HIGH, observability) ★ primary

Every "do not apply" decision in the periodic check is silent:

- `checkForFirmwareUpdate()` returns with **no log** when the `dfu.status` query fails:
  ```c
  // checkForFirmwareUpdate() ~ client .ino L4012
  if (!tankalarm_checkDfuStatus(notecard, dfuStatus)) {
    return;  // Notecard communication failure  ← silent
  }
  ```
  [TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4012)
- The periodic block silently skips when the Notecard is marked offline or power is low, and prints nothing about *why*:
  ```c
  // loop() periodic DFU check ~ L2097
  if (gPowerState <= POWER_STATE_ECO) {
    ...
    if (now - gLastDfuCheckMillis > dfuInterval) {
      gLastDfuCheckMillis = now;
      if (!gDfuInProgress && gNotecardAvailable) {   // ← false ⇒ silent skip
        checkForFirmwareUpdate();
        ...
      }
    }
  }
  ```
  [loop() L2097‑2113](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L2097)
- No breadcrumb is emitted to **Notehub** when an update is detected‑but‑deferred or when a check is skipped, so the failure is invisible both on serial and in the cloud.

**Impact:** This is why the 4.5‑hour failure could not be root‑caused with certainty. It also means a field unit that silently fails to update gives operators nothing to act on.

**Proposed fix:** Log every decision point (see §4.1). At minimum: "DFU check: Notecard offline, skipping", "DFU check: dfu.status query failed", "DFU check: mode=<x> version=<v>", "DFU update available v<v> → applying", and emit a `diag.qo`/`_log.qo` breadcrumb when an update is detected. Rate‑limit to avoid log spam (e.g., only log state changes).

---

### Finding 2 — Check timer resets even when the check is skipped (MEDIUM, logic)

In the periodic block, `gLastDfuCheckMillis = now;` is assigned **before** the `!gDfuInProgress && gNotecardAvailable` guard:

```c
if (now - gLastDfuCheckMillis > dfuInterval) {
  gLastDfuCheckMillis = now;                    // ← timer reset unconditionally
  if (!gDfuInProgress && gNotecardAvailable) {  // ← may skip the actual check
    checkForFirmwareUpdate();
    ...
  }
}
```
[L2100‑2113](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L2100)

If `gNotecardAvailable` is momentarily false at the exact tick (e.g., mid‑I²C‑recovery), the check is skipped **and** the clock is reset, so the device waits **another full `dfuInterval`** (1 hour on solar) before trying again — even if the Notecard recovers one second later.

**Impact:** A brief Notecard hiccup that coincides with the hourly tick costs a whole hour. Over a flaky window this can repeatedly defer the check.

**Proposed fix:** Only advance `gLastDfuCheckMillis` when a check actually ran (move the assignment inside the guard), or use a shorter "retry soon" interval when the guard fails:
```c
if (now - gLastDfuCheckMillis > dfuInterval) {
  if (!gDfuInProgress && gNotecardAvailable) {
    gLastDfuCheckMillis = now;          // only reset when we actually check
    checkForFirmwareUpdate();
    ...
  } else {
    // retry in 1 min instead of a full interval
    gLastDfuCheckMillis = now - dfuInterval + 60000UL;
    Serial.println(F("DFU check deferred (Notecard offline/busy) — retrying soon"));
  }
}
```

---

### Finding 3 — Single un‑retried `dfu.status` per interval (MEDIUM, robustness)

`checkForFirmwareUpdate()` issues exactly **one** `dfu.status` request and gives up silently on failure (Finding 1). There is no retry and no forced `hub.sync` to refresh the cached status. On solar units the inbound sync cadence is also hourly, so the host both *checks* rarely and *acts on stale cache*.

**Impact:** Any transient I²C/Notecard contention at check time (likely on this bench — `i2c-recovery count:7`, A0602 NACK storm) silently burns that interval's only attempt.

**Proposed fix:** Add a small bounded retry (e.g., 2–3 attempts with short backoff) inside `checkForFirmwareUpdate()` before giving up, and log the outcome. Optionally, when an update is *expected*, force a `hub.sync` and re‑query.

---

### Finding 4 — First solar DFU check is a full hour after boot (MEDIUM, latency)

`gLastDfuCheckMillis` initializes to `0` and is never seeded, so the first periodic check fires only after a full `dfuInterval`:

```c
static unsigned long gLastDfuCheckMillis = 0;   // L858
...
if (now - gLastDfuCheckMillis > dfuInterval) {  // first fire at now ≈ dfuInterval
```
[L858](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L858)

For a solar unit (`SOLAR_INBOUND_INTERVAL_MINUTES = 60`) that is **one hour** after every boot. The low‑power "daily" safety net does fire at boot + 2 min, but only when `gPowerState > POWER_STATE_ECO` ([L2120‑2165](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L2120)); the *normal* path has no early check.

**Impact:** A device that reboots shortly after an image goes ready waits up to an hour before its first look. (Not the cause of the 4.5‑hour case — that unit had long uptime — but it compounds perceived "stuck" behavior and slows every validation.)

**Proposed fix:** Seed the first normal‑path check to ~2 min after boot, mirroring `DAILY_DFU_BOOT_GRACE_MS`, e.g. initialize `gLastDfuCheckMillis` so the first check is due shortly after boot, or add a one‑shot early check guarded by a boot‑grace timer.

---

### Finding 5 — `enableDfuMode()` can busy‑loop with `firmwareLength == 0` (LOW, corner)

`checkForFirmwareUpdate()` sets `gDfuUpdateAvailable = true` regardless of whether `firmwareLength` was parsed, but `enableDfuMode()` bails when length is 0 **without** clearing the available flag or stopping the Notecard DFU:

```c
// enableDfuMode() L4151
if (gDfuFirmwareLength == 0) {
  Serial.println(F("ERROR: No firmware length — cannot apply update"));
  return;                       // gDfuUpdateAvailable stays true ⇒ retried next cycle
}
```
[L4156‑4159](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4156)

**Impact:** If a `ready` status ever lacks `body.length`, the device logs an error every check cycle forever and never progresses. (Did not occur here — would have printed — but it's a latent trap.)

**Proposed fix:** If `mode == ready` but `firmwareLength == 0`, treat it as a malformed status: log once, clear `gDfuUpdateAvailable`, and re‑query next cycle (or `dfu.get` the length explicitly).

---

### Finding 6 — Bench I²C contention raises miss probability (LOW, environmental)

The boot log shows `WARNING: A0602 current-ADC channel config NACK on ch 0`, and the device emits `i2c-recovery` diagnostics. The A0602 current‑loop ADC and the Notecard share the I²C bus (`0x0A` and `0x17`). With no real sensor attached on the bench, the ADC NACKs and triggers recovery, periodically disturbing Notecard comms.

**Impact:** Increases the chance that the once‑per‑hour `dfu.status` query (Finding 3) coincides with bus trouble and silently fails (Finding 1). Field units with a real sensor wired should see far less of this — so this is partly bench‑specific, but it is exactly the kind of real‑world flakiness the check should tolerate.

**Proposed fix:** None to firmware strictly required, but Findings 1–3 make the check resilient to it. Optionally, de‑prioritize A0602 polling while a DFU apply is imminent.

---

### Finding 7 — Positive: the apply mechanism itself is correct (INFO)

When the check *does* detect the ready image, the staging + swap path works end‑to‑end: validated by forcing a fast check and observing `1.9.27 → 1.9.28` over cellular with `Firmware change detected (stored seq 217 -> running seq 218)` (companion §17). The F1–F7 fixes (shared `fs_ota` mount, latch fix, downgrade guard, self‑check) behave as designed on hardware.

---

## 4. Recommendations (priority order)

### 4.1 Make the check observable (do first — addresses Finding 1)
Add concise, state‑change‑gated logging to the periodic DFU block and `checkForFirmwareUpdate()`:
- why a check was skipped (`power state`, `Notecard offline`, `DFU in progress`),
- the raw `dfu.status` outcome (`mode`, `version`, or query‑failed),
- the apply decision (`available → applying`, `not newer → ignoring`, `blacklisted`).
Emit a Notehub breadcrumb (`_log.qo`/`diag.qo`) when an update is detected or when a check fails repeatedly, so the cloud shows the host side, not just the Notecard side.

### 4.2 Make the check robust (Findings 2, 3)
- Only reset `gLastDfuCheckMillis` when a check actually runs; use a short retry interval when skipped.
- Add a bounded retry around `dfu.status`; optionally force a `hub.sync` when an update is expected.

### 4.3 Make the check prompt (Finding 4)
- Seed the first normal‑path DFU check to ~2 min after boot (reuse `DAILY_DFU_BOOT_GRACE_MS`).

### 4.4 Harden corner cases (Finding 5)
- Handle `mode == ready && firmwareLength == 0` explicitly instead of looping.

### 4.5 Verify in the field, not just the bench (Finding 6)
- Re‑run an OTA on a unit with a real current‑loop sensor and the 4.1 logging in place; confirm the check detects and applies without a forced interval.

> All of the above are low‑risk, fail‑safe changes (logging + timing + a guard reorder). None touch the proven staging/swap engine.

---

## 5. What was already validated (so it isn't re‑investigated)

- **CI signing/format:** fixed and proven (companion §16–§17). The 1.9.28 slot image is the correct core `security=sien` format and swaps on hardware.
- **Keys:** signing + encryption keys are content‑identical across repo, core defaults, and the device's provisioned keys (companion §16.2).
- **Downgrade guard / version math:** `versionToSeq("1.9.28") = 218 > 217` — passes; not a blocker.
- **`gDfuInProgress` latch:** clears on failure (`enableDfuMode()` L4198); not a permanent latch.
- **`gNotecardAvailable`:** self‑heals on the next successful `card.wireless`; not a permanent latch.
- **Power‑state gating:** bench resolves to NORMAL (no active voltage source), so it did not gate the check in this case.

---

## 6. AI Agent Code Review Additions (06/16/2026)

Based on an additional deep-dive review of `TankAlarm-112025-Client-BluesOpta.ino` and `TankAlarm_DFU.h`, the following findings and recommendations are appended to the earlier manual analysis:

### Finding 8 — Rejected old updates leave Notecard in "ready" mode indefinitely (HIGH, logic trap)
In `checkForFirmwareUpdate()`, if the detected update is rejected due to the downgrade guard or blacklist, the code logs the rejection, sets `gDfuUpdateAvailable = false`, and immediately returns. 
**However, it does not send `stop` to the Notecard.**
Because the active update is never cleared via a `dfu.status` `"stop": true` request or `"err"` attribute, the Notecard continues to advertise `mode: "ready"` indefinitely.
**Impact:** On every check interval, the client queries `dfu.status`, sees "ready", runs the downgrade check, and silently skips it again. This creates a permanent logic trap that wastes I²C bandwidth and prevents the Notehub status from clearing to "failed" or "completed", which may permanently suppress *future* valid updates if Notehub believes it has already staged an update.
**Proposed fix:** When rejecting an update for downgrade/blacklist reasons, issue a `dfu.status` request to the Notecard with `"stop": true` and `"err": "downgrade rejected"` to clear the ready state and inform Notehub.

### Finding 9 — `gNotecardAvailable` I²C flapping causes full-interval skips (MEDIUM, sequencing)
In `loop()`, the DFU check block is guarded by `gNotecardAvailable`. 
`gNotecardAvailable` is set to `false` if `gNotecardFailureCount >= NOTECARD_FAILURE_THRESHOLD` during health checks. If the device suffers intermittent I²C failure (like Finding 6, where the A0602 ADC causes I²C recovery), `gNotecardAvailable` may rapidly flap.
Crucially, if the DFU check is skipped due to `gNotecardAvailable` being temporarily `false`, the timer logic (see Finding 2) assigns `gLastDfuCheckMillis = now;` before skipping. The device inherently waits a full 1-hour interval before trying again instead of waiting for the I²C bus to recover a few seconds later.
**Proposed fix:** Align with Section 4.2 recommendations. When skipping the check due to `gNotecardAvailable`, use a fast-retry backoff (e.g., 60 seconds) rather than accepting a full interval wait. Also avoid checking `gNotecardAvailable` until the timer has elapsed, ensuring the clock isn't reset unjustly on I²C failures.

### 6.1 Actionable Additions to Recommendations
- **Fix the Downgrade / Blacklist Trap:** Update `checkForFirmwareUpdate()` so any explicit `return` resulting from invalid firmware data sets a failure state on the Notecard DFU via `dfu.status {"stop": true, "err": "<reason>"}` before returning.

---

*Prepared 2026‑06‑16. Companion: `CODE_REVIEW_06152026_OTA_VS_USB_UPDATE_FAILURE.md` (CI fix + end‑to‑end OTA success). This document is analysis only; implement §4 and §6 changes under a version bump with serial validation.*

---

## 7. AI Agent Follow-up Additions (06/16/2026, pass 2)

Second-pass code review focused on OTA sequencing across `checkForFirmwareUpdate()`, `checkNotecardHealth()`, and `tankalarm_performMcubootUpdate()` identified the following additional risks.

### Finding 10 — `card.wireless` error path can keep `gNotecardAvailable` false (HIGH, recovery sequencing)

In `checkNotecardHealth()`, a non-empty `card.wireless.err` returns early:

```c
const char *wirelessErr = JGetString(rsp, "err");
if (wirelessErr && wirelessErr[0] != '\0') {
  ...
  return true;  // Notecard is responding (I2C OK), just wireless issue
}
```

But the code that restores `gNotecardAvailable = true` runs *after* that return path.

References:
- [TankAlarm-112025-Client-BluesOpta.ino#L3845](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L3845)
- [TankAlarm-112025-Client-BluesOpta.ino#L3883](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L3883)

**Impact:** If the device entered offline mode due I2C failures, then starts responding again with a wireless-layer error, OTA checks can remain blocked by `gNotecardAvailable == false` even though Notecard I2C is healthy.

**Proposed fix:** Treat any successful `card.wireless` response as transport recovery. Move `gNotecardAvailable = true; gNotecardFailureCount = 0;` above the wireless error return, and track wireless/connectivity health in a separate flag/log path.

---

### Finding 11 — Stale DFU state can drive apply attempts after failed status checks (HIGH, state sequencing)

`checkForFirmwareUpdate()` returns immediately on status-query failure, status error, and blacklist reject without clearing prior DFU candidate state. The loop then applies whenever `gDfuUpdateAvailable` is true:

```c
if (!tankalarm_checkDfuStatus(notecard, dfuStatus)) {
  return;
}
...
if (dfuStatus.error) {
  return;
}
...
if (tankalarm_isVersionBlacklisted(dfuStatus.version)) {
  return;
}
...
if (gDfuUpdateAvailable) {
  enableDfuMode();
}
```

References:
- [TankAlarm-112025-Client-BluesOpta.ino#L4012](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4012)
- [TankAlarm-112025-Client-BluesOpta.ino#L4035](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4035)
- [TankAlarm-112025-Client-BluesOpta.ino#L4045](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4045)
- [TankAlarm-112025-Client-BluesOpta.ino#L2106](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L2106)

**Impact:** After one failed staging attempt (or other stale-flag state), later transient `dfu.status` failures can trigger repeated apply attempts using stale metadata instead of a fresh accepted status snapshot.

**Proposed fix:** Make `checkForFirmwareUpdate()` return an explicit decision enum (`NoCheck`, `NoUpdate`, `UpdateReady`, `Rejected`, `Error`) and only call `enableDfuMode()` on `UpdateReady`. Also clear stale DFU candidate fields on error/failure return paths.

---

### Finding 12 — `versionToSeq()` ordering is not semver-safe (MEDIUM, downgrade guard correctness)

Current mapping:

```c
return major * 100 + minor * 10 + patch;
```

Reference:
- [TankAlarm_DFU.h#L758](../TankAlarm-112025-Common/src/TankAlarm_DFU.h#L758)

**Impact:** The downgrade guard can misorder versions when minor/patch width grows (e.g., `1.10.0` vs `1.9.20`) or when version strings include non-numeric prefixes (`v1.9.28`) that parse to zero. This can incorrectly reject valid upgrades or accept invalid ordering.

**Proposed fix:** Parse into integer tuple `(major, minor, patch)` with strict validation and compare tuple-wise. Reject malformed strings explicitly with a logged reason, not as implicit zero.

---

### Finding 13 — MCUboot preflight reject paths return before clearing Notecard DFU state (MEDIUM, ready-state trap)

In `tankalarm_performMcubootUpdate()`, preflight checks for invalid length or role/source mismatch return `false` immediately:

```c
if (firmwareLength == 0 || firmwareLength > TANKALARM_MCUBOOT_SLOT_SIZE) {
  return false;
}
if (dfu.firmwareSource[0] != '\0' && !strstr(...)) {
  return false;
}
```

The `dfu.status {"stop":true}` clear/report is only performed in later failure paths (`mcuboot_restore_hub`).

References:
- [TankAlarm_DFU.h#L980](../TankAlarm-112025-Common/src/TankAlarm_DFU.h#L980)
- [TankAlarm_DFU.h#L988](../TankAlarm-112025-Common/src/TankAlarm_DFU.h#L988)
- [TankAlarm_DFU.h#L1269](../TankAlarm-112025-Common/src/TankAlarm_DFU.h#L1269)

**Impact:** A preflight rejection can leave Notecard reporting `ready`, causing repeated re-detection/retry loops and stale OTA lifecycle status in Notehub.

**Proposed fix:** Route *all* preflight failures through a common fail-closed reporter that sends `dfu.status stop + status/err` before returning.

---

### Finding 14 — Low-power daily DFU path can miss post-sync readiness and defer 24 hours (MEDIUM, timing race)

In the daily low-power override, `gLastDailyDfuCheckMillis` is set before forcing `hub.sync`, and `checkForFirmwareUpdate()` runs immediately after:

```c
gLastDailyDfuCheckMillis = now;
...
hub.sync
checkForFirmwareUpdate();
```

Reference:
- [TankAlarm-112025-Client-BluesOpta.ino#L2127](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L2127)

**Impact:** If the sync has not yet completed when status is queried, the device can miss a newly-ready image and then wait until the next daily window (24h) while in LOW/CRITICAL states.

**Proposed fix:** Do not advance the daily timer until a real check executes against a confirmed sync point; or schedule a short follow-up retry (e.g., +1 to +5 minutes) when the immediate post-sync check sees no update.

---

### 7.1 Recommendations Addendum (for Findings 10–14)

1. **Split transport health from cellular health:** Set Notecard availability based on I2C request success, not `card.wireless` cloud status.
2. **Make DFU apply decision explicit and fresh:** Gate `enableDfuMode()` on an explicit per-cycle `UpdateReady` result; clear stale candidate state on failed status reads.
3. **Replace sequence math with tuple compare:** Use strict semver parsing + compare to avoid downgrade-guard misclassification.
4. **Fail-closed on all OTA rejects:** Any local reject path (downgrade, blacklist, role mismatch, malformed metadata) should report and clear Notecard DFU state.
5. **Add short retry after forced daily sync:** Prevent a single sync/status race from pushing low-power devices to a 24-hour deferral.

---

## 8. AI Agent Deep-Dive Additions (06/16/2026, pass 3)

Third-pass review focused on end-to-end data flow through `checkForFirmwareUpdate()` → `enableDfuMode()` → `tankalarm_performMcubootUpdate()`, with attention to error-path state consistency, buffer safety, and Notecard mode recovery.

### Finding 15 — `hub.set mode=dfu` lost-response can strand Notecard in DFU mode (HIGH, recovery)

In `tankalarm_performMcubootUpdate()`, Step 1 sends `hub.set mode=dfu` and returns `false` if the response is null:

```c
J *req = notecard.newRequest("hub.set");
if (!req) return false;                       // ← early return #1
JAddStringToObject(req, "mode", "dfu");
J *rsp = notecard.requestAndResponse(req);
if (rsp) {
  notecard.deleteResponse(rsp);
} else {
  return false;                               // ← early return #2
}
```

References:
- [TankAlarm_DFU.h#L1008](../TankAlarm-112025-Common/src/TankAlarm_DFU.h#L1008)
- [TankAlarm_DFU.h#L1016](../TankAlarm-112025-Common/src/TankAlarm_DFU.h#L1016)

Both early returns bypass `mcuboot_restore_hub` (which issues `hub.set mode=<hubMode>` to restore normal operation). If `requestAndResponse()` returns null because the I²C response was lost — but the Notecard *did* process the request — the Notecard is now in DFU mode. In DFU mode the Notecard **does not sync** with Notehub: no inbound notes (config), no outbound notes (alarms/telemetry), no OTA downloads.

The caller (`enableDfuMode()`) sets `gDfuInProgress = false` and resumes normal operation, but the Notecard stays in DFU mode silently. The only eventual recovery is the modem-stall detector in `checkNotecardHealth()` which fires after **4 hours** of no successful `note.add` — it issues `card.restart`, which resets the modem but may or may not clear a latched `hub.set mode=dfu`.

**Impact:** A single lost I²C response during OTA staging can silently disable all cellular connectivity for 4+ hours, with no log or diagnostic indicating the Notecard is stuck in DFU mode.

**Proposed fix:** Route all early returns through `mcuboot_restore_hub` (or a shared cleanup block), so `hub.set mode=<hubMode>` is always sent on failure — even if the original `hub.set mode=dfu` may not have succeeded. Restoring hub mode on a Notecard that was never put in DFU mode is harmless.

---

### Finding 16 — `checkForFirmwareUpdate()` error-path preserves stale `gDfuUpdateAvailable`, enabling apply with stale metadata (HIGH, state sequencing)

When `dfuStatus.error` is true, `checkForFirmwareUpdate()` logs and returns **without clearing `gDfuUpdateAvailable`**:

```c
if (dfuStatus.error) {
  Serial.print(F("DFU error: "));
  Serial.println(dfuStatus.errorMsg);
  return;                                  // ← gDfuUpdateAvailable unchanged
}
```

Reference:
- [TankAlarm-112025-Client-BluesOpta.ino#L4035](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4035)

The calling code in `loop()` unconditionally checks the flag after the function returns:

```c
checkForFirmwareUpdate();
if (gDfuUpdateAvailable) {       // ← still true from a PREVIOUS check
  enableDfuMode();               // ← called with stale gDfuStatus / gDfuFirmwareLength
}
```

Reference:
- [TankAlarm-112025-Client-BluesOpta.ino#L2103](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L2103)

**Impact:** If the Notecard transitions from `mode:"ready"` to an error state (e.g., CRC failure during download of a replacement image, or Notehub-side cancellation), the host will attempt to stage the now-invalid or absent image using cached metadata from the prior `ready` status. `tankalarm_performMcubootUpdate()` will enter DFU mode on the Notecard and then fail at `dfu.get`, consuming a full DFU timeout window (60 seconds) before falling through to the error path.

**Proposed fix:** Clear `gDfuUpdateAvailable`, `gDfuFirmwareLength`, and `gDfuVersion` on every non-ready return path (`error`, `downloading`, communication failure`) so stale state never drives an apply attempt:

```c
if (dfuStatus.error) {
  Serial.print(F("DFU error: "));
  Serial.println(dfuStatus.errorMsg);
  gDfuUpdateAvailable = false;
  gDfuFirmwareLength = 0;
  gDfuVersion[0] = '\0';
  return;
}
```

---

### Finding 17 — Preflight-rejected updates are re-detected and re-rejected every check cycle (MEDIUM, waste + I²C load)

When `tankalarm_performMcubootUpdate()` rejects an update at the preflight stage (invalid length or role/source mismatch), it returns `false` without sending `dfu.status stop` (Finding 13). The caller `enableDfuMode()` logs the failure and resumes. However, it does **not** clear `gDfuUpdateAvailable`.

On the next DFU check interval:
1. `checkForFirmwareUpdate()` queries `dfu.status` → still `mode:"ready"` (never cleared)
2. Blacklist/downgrade guards pass (role mismatch is not a blacklist or downgrade)
3. `gDfuUpdateAvailable` is set to `true` again (same version, same metadata)
4. `enableDfuMode()` is called → `tankalarm_performMcubootUpdate()` hits the same preflight → returns `false`
5. Repeat indefinitely every check interval

Each cycle mounts and unmounts the QSPI FAT filesystem (for blacklist check), issues `dfu.status` over I²C, and may attempt `hub.set mode=dfu` before failing — all for an update that can never succeed.

References:
- [TankAlarm_DFU.h#L980](../TankAlarm-112025-Common/src/TankAlarm_DFU.h#L980) (length check)
- [TankAlarm_DFU.h#L992](../TankAlarm-112025-Common/src/TankAlarm_DFU.h#L992) (role check)
- [TankAlarm-112025-Client-BluesOpta.ino#L4195](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4195) (`enableDfuMode()` failure path)

**Impact:** Permanent retry loop that wastes I²C bandwidth, QSPI wear, and (for solar devices) energy — and blocks detection of a *corrected* firmware push until the stale `ready` state is manually cleared from Notehub.

**Proposed fix:** When `enableDfuMode()` fails, explicitly stop the Notecard DFU and clear the update-available flag so the device does not retry a permanently-failing update:

```c
if (!ok) {
  gDfuUpdateAvailable = false;
  gDfuVersion[0] = '\0';
  gDfuFirmwareLength = 0;
  // ...existing failure logging...
}
```

Combined with Finding 13's recommendation to have `tankalarm_performMcubootUpdate()` send `dfu.status stop` on all failure paths, this breaks the loop and lets Notehub know the update was rejected.

---

### Finding 18 — `sscanf` with `%[^"]` in OTA state file parsing has no field-width limit (MEDIUM, robustness)

Multiple functions parse `pending_ota.json` from the QSPI filesystem using `sscanf` with unbounded character-class specifiers:

```c
char target_v[32];
char status[32];
sscanf(buf, "{\"target_seq\":%u,\"target_v\":\"%[^\"]\",\"status\":\"%[^\"]\"}", 
       &target_seq, target_v, status);
```

References (all have the same pattern):
- [TankAlarm_DFU.h#L791](../TankAlarm-112025-Common/src/TankAlarm_DFU.h#L791) — `tankalarm_resolvePendingOta()`
- [TankAlarm_DFU.h#L879](../TankAlarm-112025-Common/src/TankAlarm_DFU.h#L879) — `tankalarm_isVersionBlacklisted()`
- [TankAlarm_DFU.h#L923](../TankAlarm-112025-Common/src/TankAlarm_DFU.h#L923) — `tankalarm_peekOtaReport()`
- [TankAlarm_DFU.h#L938](../TankAlarm-112025-Common/src/TankAlarm_DFU.h#L938) — dedupe block in `tankalarm_peekOtaReport()`

The `%[^"]` specifier writes as many bytes as it finds before a `"` delimiter — there is no width limit. The destination buffers are 16–32 bytes. If `pending_ota.json` is corrupted (QSPI flash wear, power loss during write, partial write from a failed staging attempt), a missing `"` delimiter would cause `sscanf` to write up to 255 bytes of file content into a 32-byte stack buffer.

**Impact:** Stack buffer overflow causing undefined behavior — most likely a hard fault and watchdog reset, but potentially a silent stack corruption. Since this runs on every boot (`tankalarm_resolvePendingOta`) and on every DFU check cycle (`tankalarm_isVersionBlacklisted`), a corrupted file creates a **boot loop**.

**Proposed fix:** Add width specifiers matching the destination buffer sizes, e.g.:

```c
sscanf(buf, "{\"target_seq\":%u,\"target_v\":\"%31[^\"]\",\"status\":\"%31[^\"]\"}", 
       &target_seq, target_v, status);
```

Or switch to a small JSON parser (the Notecard library's `JGetString` pattern) for robustness against malformed files.

---

### Finding 19 — Normal-path DFU check does not kick watchdog during QSPI filesystem operations (MEDIUM, reliability)

The periodic DFU check in `loop()` calls `checkForFirmwareUpdate()`, which in turn calls `tankalarm_isVersionBlacklisted()`. The blacklist check mounts the QSPI FAT filesystem, reads and parses a file, and unmounts — all without any watchdog kick.

The watchdog is kicked once at the top of `loop()` (L1782), but the DFU check block runs much later. On a device with a degraded QSPI (slow erase/read cycles), the filesystem mount+read+unmount sequence could approach the watchdog window.

```c
// loop() — top
mbedWatchdog.kick();           // ← only kick before the DFU check block

// ... many operations ...

// loop() — DFU check (L2096+)
if (gPowerState <= POWER_STATE_ECO) {
  if (now - gLastDfuCheckMillis > dfuInterval) {
    gLastDfuCheckMillis = now;
    if (!gDfuInProgress && gNotecardAvailable) {
      checkForFirmwareUpdate();    // ← mounts QSPI, reads file, no watchdog kick
    }
  }
}
```

References:
- [TankAlarm-112025-Client-BluesOpta.ino#L1782](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L1782)
- [TankAlarm-112025-Client-BluesOpta.ino#L2103](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L2103)
- [TankAlarm_DFU.h#L858](../TankAlarm-112025-Common/src/TankAlarm_DFU.h#L858) (`tankalarm_isVersionBlacklisted` → `tankalarm_otaFsMount`)

Note: this is in contrast to the daily DFU check path, which explicitly kicks the watchdog before and after the sync + check (L2133–2148). The daily path correctly anticipates the latency.

**Impact:** On a device with slow QSPI, the normal-path DFU check could trigger a watchdog reset, creating an intermittent reboot that looks unrelated to OTA.

**Proposed fix:** Add a watchdog kick before `checkForFirmwareUpdate()` in the normal periodic path, mirroring the daily path:

```c
if (!gDfuInProgress && gNotecardAvailable) {
  #ifdef TANKALARM_WATCHDOG_AVAILABLE
    mbedWatchdog.kick();
  #endif
  checkForFirmwareUpdate();
  // ...
}
```

---

### Finding 20 — Unconditional early `confirmSketch()` removes MCUboot's rollback safety net (MEDIUM, OTA safety)

In `setup()`, `MCUboot::confirmSketch()` is called **unconditionally** before any peripheral initialization or Notecard communication:

```c
#if defined(TANKALARM_DFU_MCUBOOT)
  MCUboot::confirmSketch();
  Serial.println(F("MCUboot: sketch confirmed (early, unconditional)"));
#endif
```

Reference:
- [TankAlarm-112025-Client-BluesOpta.ino#L1456](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L1456)

The comment explains the intent: prevent rollback when QSPI storage or Notecard is unavailable. However, this completely defeats MCUboot's trial-boot safety mechanism. MCUboot is designed to automatically roll back to the previous known-good image if the new image fails to confirm — for example, if the new firmware crashes during Notecard initialization, I²C setup, or sensor bring-up.

By confirming before any of those subsystems are tested, a broken OTA image that passes basic boot but fails to communicate (e.g., corrupted Notecard I²C driver, broken sensor polling causing watchdog timeout in `loop()`) becomes **permanently installed** with no automatic recovery path.

**Impact:** If a firmware update introduces a regression that crashes after `setup()` completes but before stable `loop()` execution (e.g., a null pointer in `checkNotecardHealth()`, a hang in sensor polling), MCUboot cannot roll it back because the image was already confirmed. The only recovery is physical USB access.

**Proposed fix:** Delay `confirmSketch()` until after a meaningful health gate — for example, after one successful `checkNotecardHealth()` cycle and one successful `loop()` iteration, or after a configurable timeout (e.g., 5 minutes of stable operation). Use the existing `tankalarm_markFirmwareHealthy()` in `loop()` as the sole confirmation point:

```c
// In setup(): remove MCUboot::confirmSketch()
// In loop() (already present):
tankalarm_markFirmwareHealthy();  // called every loop(), but confirms only once
```

Guard the confirmation inside `tankalarm_markFirmwareHealthy()` with a check that the Notecard has been successfully contacted at least once, ensuring the core communication path works before committing.

> **Trade-off note:** The current approach prioritizes availability (never brick via failed confirmation) over correctness (always roll back broken firmware). The right balance depends on how common USB-flashed units with empty QSPI are vs. how likely a regression-bearing OTA is. A middle ground: confirm immediately **only** if no `pending_ota.json` with `status:"trial"` exists (i.e., this wasn't an OTA-delivered image), and defer confirmation for OTA-delivered images until the health gate passes.

---

### Finding 21 — `tankalarm_performMcubootUpdate()` does not verify `dfu.get` chunk ordering or detect duplicate offsets (LOW, data integrity)

The download loop in `tankalarm_performMcubootUpdate()` tracks `offset` as a running counter, but never verifies that the Notecard returned data for the requested offset. If a Notecard firmware bug or I²C corruption causes a `dfu.get` response to contain data for the wrong offset, the host writes it at the wrong position without detection:

```c
JAddNumberToObject(req, "offset", (int)offset);
// ...
// Response payload is written at `offset` without verifying the Notecard
// echoed back the requested offset
fseek(fp, offset, SEEK_SET);
fwrite(progBuf, 1, decoded, fp);
```

Reference:
- [TankAlarm_DFU.h#L1106](../TankAlarm-112025-Common/src/TankAlarm_DFU.h#L1106)

The CRC-32 check at the end (Step 4c) would catch this only if the Notecard provides a correct `crc32` in the `dfu.status` body, which is Notehub-computed and may not match a Notecard-side re-encoding error. The running `downloadCrc` would also be wrong since it accumulates over misplaced data.

**Impact:** Low probability, but if triggered, the staged firmware would be silently corrupted. MCUboot's own header/signature check at swap time is the final safety net, but depending on the corruption location, the image could pass MCUboot validation and then crash at runtime (stuck in rollback loop until blacklisted).

**Proposed fix:** If the Notecard `dfu.get` response includes an `offset` field, verify it matches the requested offset before writing. This is a defense-in-depth measure.

---

### 8.1 Recommendations Addendum (for Findings 15–21)

1. **Route all `tankalarm_performMcubootUpdate()` failure paths through a common cleanup label** that restores hub mode and stops the Notecard DFU. Early returns before `hub.set mode=dfu` need at minimum a `dfu.status stop` to clear the ready state. Early returns after `hub.set mode=dfu` must also restore hub mode (Finding 15).

2. **Clear all DFU candidate state on any non-`ready` status return** from `checkForFirmwareUpdate()`. The `error`, `downloading`, and communication-failure paths must all set `gDfuUpdateAvailable = false` so stale flags never drive `enableDfuMode()` (Finding 16).

3. **Clear `gDfuUpdateAvailable` in `enableDfuMode()` on staging failure** and have the preflight reject paths send `dfu.status stop` to break the perpetual retry loop (Finding 17, reinforces Finding 13).

4. **Add `sscanf` field-width limits** (`%31[^"]`) to all OTA state file parsing to prevent stack buffer overflow from corrupted QSPI data. Consider adding a file-size sanity check before parsing (Finding 18).

5. **Add watchdog kick before `checkForFirmwareUpdate()`** in the normal periodic path, consistent with the daily path (Finding 19).

6. **Defer `MCUboot::confirmSketch()` for OTA-delivered images** until a health gate passes (e.g., one successful Notecard communication cycle). Keep the unconditional early confirm only for USB-flashed images where no `pending_ota.json` trial marker exists (Finding 20).

7. **Verify `dfu.get` response offset** when available, as defense-in-depth against silent data misplacement (Finding 21).

> Findings 15–17 are the highest priority: they represent silent failure modes where the device either loses connectivity or enters infinite retry loops with no diagnostic output. Findings 18–19 are reliability hardening. Findings 20–21 are defense-in-depth improvements to the OTA safety model.
