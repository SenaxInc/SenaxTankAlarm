# Code Review: Coordinated ODFU Client Handshake (v1.8.5)
**Date:** June 9, 2026
**Reviewer:** GitHub Copilot
**Commit reviewed:** `81a8c0b` — *"Bump version to v1.8.5 and implement Coordinated ODFU update handshake on remote DIN-rail clients with zero hardware mods"*
**Components:**
- `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino` (`enableDfuMode()`, `initializeNotecard()`)
- `TankAlarm-112025-Common/src/TankAlarm_DFU.h` (shared IAP helpers, now partly dead in the client)
- Design docs: `CODE_REVIEW_ODFU_BRICK_PROOF_IMPLEMENTATION_PLAN_06092026.md`, `CODE_REVIEW_DFU_IAP_ANALYSIS_06092026.md`

---

## 1. Executive Summary

v1.8.5 reworks the **Client** firmware-update path to **Coordinated Outboard DFU (ODFU)**: the host quiesces the RS-485 bus and then asks the Notecard to reset/flash the STM32 via `card.dfu {"name":"stm32","on":true}`.

**Important context (see §1a):** ODFU is *not* a new, unproven mechanism on this hardware. Per git history, ODFU was the **original** update path and **was successfully updating devices in the field before April 2026**. The April `840bc64` (v1.3.0) commit replaced it with host-driven IAP and added a "ODFU not supported on Blues Wireless for Opta (no AUX pin routing)" comment — a claim that is **contradicted by the working pre-April field history**. v1.8.5 effectively **restores** the original (working) ODFU mechanism and layers a coordinated RS-485 quiescing handshake on top for admin-triggered updates.

This reframes two findings below: the "is ODFU even supported?" question (C3) is largely answered by history (it worked), and the infinite-hang (C2) is a **new v1.8.5 regression** — the original ODFU path did **not** block. The architectural goal — a brick-proof, hardware-managed recovery path — is sound. However, the v1.8.5 implementation introduces blocking defects and leaves stale, contradictory documentation/Server code.

**Recommendation:** Do **not** field-deploy the v1.8.5 client ODFU path until C1–C2 are fixed. Treat the original pre-April ODFU flow as the proven reference implementation.

| ID | Severity | Issue |
|----|----------|-------|
| C1 | 🔴 Critical | Bus-quiescing isolation code is dead (`PIN_RS485_*` never defined) |
| C2 | 🔴 Critical | **New regression:** infinite `while(true)` hang if the reset never arrives; watchdog deliberately starved (original ODFU did not block) |
| C3 | 🟠 High | Stale/contradictory docs: header + Server still say ODFU unsupported & still use IAP, though ODFU worked pre-April |
| H1 | 🟠 High | Detection uses IAP `user` state; application uses ODFU `stm32` — mismatched flows |
| H2 | 🟠 High | Auto-apply on `gDfuUpdateAvailable` with no confirmation / rollback (fleet risk) |
| M1 | 🟡 Moderate | RS-485 isolation logic floats `DE`/`RE` instead of holding them disabled |
| M2 | 🟡 Moderate | Watchdog-starvation regression vs. prior hardening work |
| M3 | 🟡 Moderate | Large dead IAP/FlashIAP path still compiled into the client (`-Wunused-function`) |
| N1–N4 | ⚪ Minor | Typo, restore-path inconsistency, stale "IAP" comments, un-kicked blocking window |

---

## 1a. History & Context: ODFU Was the Original, Working Mechanism

Reconstructed from git history:

| When | Commit / Version | Mechanism | Notes |
|------|------------------|-----------|-------|
| Before Apr 2026 | pre-v1.3.0 | **ODFU** (`card.dfu {"name":"stm32","on":true}`) | **Worked in the field** — updates succeeded |
| Apr 2, 2026 | `840bc64` / v1.3.0 | **Switched to IAP** | Commit msg: "remove reliance on outboard DFU (card.dfu)"; added "ODFU not supported … no AUX pin routing" comment |
| Apr–Jun 2026 | v1.3.0 → v1.8.4 | **IAP** (`dfu.get` + `FlashIAP`) | Host reads/writes its own flash |
| Jun 9, 2026 | `81a8c0b` / v1.8.5 | **Client back to ODFU** (Coordinated) | Server still on IAP |

**Key correction to the v1.8.5 premise and the April commit's premise:** the intent in April was understood to be *adding admin-triggered update capability*, **not** removing a working ODFU path. The "AUX pins not routed / ODFU unsupported" justification in `840bc64` and in the current `TankAlarm_DFU.h` header is **not consistent** with ODFU having worked before April. This stale claim is what produced the apparent "contradiction" in C3 and should be removed/corrected.

### The original ODFU `enableDfuMode()` (pre-April) — the proven reference
```cpp
// ODFU already enabled at boot via initializeNotecard().
J *req = notecard.newRequest("card.dfu");
JAddStringToObject(req, "name", "stm32");
JAddBoolToObject(req, "on", true);
J *rsp = notecard.requestAndResponse(req);
// ...check err, deleteResponse...
gDfuInProgress = true;
// Force sync to accelerate the firmware download
J *syncReq = notecard.newRequest("hub.sync");
if (syncReq) notecard.sendRequest(syncReq);
// >>> THEN RETURNS — does NOT block. Notecard resets/flashes in background. <<<
```
Note what the original did **not** do: it never entered a `while(true)` kick loop, and it never starved the watchdog. The device continued running its normal loop until the Notecard asserted the hardware reset. **v1.8.5 should adopt this non-blocking return pattern** instead of the new infinite loop (C2). The genuinely *new* and valuable v1.8.5 contribution is the RS-485 bus-quiescing step (for healthy, actively-polling devices) — that should be layered onto the proven non-blocking flow, not onto an indefinite hang.

---

## 2. Critical Findings

### C1 — Bus-quiescing isolation is dead code 🔴
**Location:** `TankAlarm-112025-Client-BluesOpta.ino`, `enableDfuMode()` (~L3893–L3905)

```cpp
#if defined(PIN_RS485_DE) && defined(PIN_RS485_RE)
    pinMode(PIN_RS485_DE, OUTPUT);
    digitalWrite(PIN_RS485_DE, LOW);
    ...
    pinMode(PIN_RS485_DE, INPUT);
    pinMode(PIN_RS485_RE, INPUT);
#endif
#if defined(PIN_RS485_TX) && defined(PIN_RS485_RX)
    pinMode(PIN_RS485_TX, INPUT);
    pinMode(PIN_RS485_RX, INPUT);
#endif
```

`PIN_RS485_DE`, `PIN_RS485_RE`, `PIN_RS485_TX`, and `PIN_RS485_RX` are **never `#define`d anywhere in the workspace** — they appear only inside these `#if defined(...)` guards. Both blocks therefore compile to nothing.

**Impact:** The "Execute Bus Quiescence Protocol" step that the implementation plan (§5.2) identifies as *the* core mechanism for updating a healthy, actively-polling device is **silently absent**. Only `gSolarManager.end()` and `RS485.end()` actually run. The RS-485 transceiver pins are left in whatever state the library/HAL leaves them, so the exact bootloader auto-baud collision the plan is built to avoid is **not** prevented. On a live device this is precisely the condition that produces `{odfu-fail}` on Notehub.

**Fix:** Define the pins from the Opta/Wireless-for-Opta variant (the on-board RS-485 DE/RE and `Serial`/USART mapping), or call the transceiver's documented disable API. Add a compile-time `#error`/`#warning` if the pins are not defined so the isolation can never be silently skipped again. Verify with a logic analyzer that the lines are actually static before `card.dfu` is sent.

---

### C2 — Infinite hang waiting for a reset that may never come 🔴 (new regression)
**Location:** `enableDfuMode()` success branch (~L3935–L3941)

```cpp
Serial.println(F("ODFU Handshake: Reset command accepted. MCU restarting..."));
// The Notecard will pull the hardware NRST low and reset us.
while (true) {
  #ifdef TANKALARM_WATCHDOG_AVAILABLE
    mbedWatchdog.kick();
  #endif
  delay(100);
}
```

This blocking loop is **new in v1.8.5**. The original, field-proven ODFU `enableDfuMode()` (see §1a) re-asserted `card.dfu on`, forced a `hub.sync`, set `gDfuInProgress = true`, and then **simply returned** — the device kept running its normal loop while the Notecard performed the hardware reset/flash in the background. It never blocked and never starved the watchdog.

`card.dfu {"on":true}` returning without an `err` only means the **Notecard accepted the request** — it does not guarantee the physical reset latency. With the new loop, if the reset is delayed or doesn't arrive, the device spins here **forever**, and because it **actively kicks the watchdog** it also disables the platform's one automatic recovery. The unit becomes a silent brick: no polling, no alarms, no SMS — on a remote DIN-rail client with no local access. That is the opposite of the "brick-proof" goal.

**Fix (preferred):** Restore the original **non-blocking return** pattern — after a successful `card.dfu on` + `hub.sync`, return and let the main loop continue; the Notecard will reset the MCU when ready. If a post-trigger guard window is desired, make it a **bounded** wait that, on timeout, **stops kicking the watchdog** (let it reset) and re-enables RS-485/SolarManager so the device resumes monitoring rather than hanging.

---

### C3 — Stale, contradictory docs & a split-brain Server 🟠
The client now uses ODFU, but two places in the repo still assert ODFU is impossible on this carrier — a claim **disproven by the pre-April working history** (§1a):

1. `TankAlarm-112025-Common/src/TankAlarm_DFU.h` header comment (file top):
   > "The Blues Wireless for Opta carrier does **NOT** route the AUX pins needed for Outboard DFU (ODFU). This module implements IAP DFU instead…"
2. `TankAlarm-112025-Server-BluesOpta.ino` (~L8192):
   > "Wireless for Opta uses IAP DFU, not ODFU." — and the Server still calls `tankalarm_performIapUpdate()` (~L8432).

So **Server and Client now use different update mechanisms on identical hardware**, and the shared header still claims the client's (proven) mechanism can't work. This is stale documentation plus an unfinished migration — not a genuine hardware limitation.

**Fix:** Make ODFU the single source of truth: correct/remove the "ODFU not supported" comment in `TankAlarm_DFU.h`, update the Server comment, and migrate the Server back to the same coordinated ODFU flow so the fleet is consistent. A quick bench confirmation that `card.dfu {"name":"stm32","on":true}` still resets/flashes the Opta is worthwhile, but the burden of proof has flipped: history says it works.

---

## 3. High Findings

### H1 — Detection (IAP `user`) and application (ODFU `stm32`) are different flows 🟠
`checkForFirmwareUpdate()` still calls `tankalarm_checkDfuStatus()` → `dfu.status {"name":"user"}` and sets `gDfuUpdateAvailable` when `mode == "ready"`. That readiness reflects the **host firmware staged for IAP**. But `enableDfuMode()` then triggers `card.dfu {"name":"stm32"}`, which is the Notecard's **outboard STM32** flow. The availability signal that gates the update is not the same artifact that ODFU flashes. At minimum, confirm that "`user` firmware ready" reliably implies "`stm32` ODFU image staged and triggerable"; otherwise the device can isolate its bus and reset on a target that isn't actually staged.

`initializeNotecard()` compounds this by enabling **both** at boot: `tankalarm_enableIapDfu(notecard)` (`dfu.status {"on":true,"name":"user"}`) **and** `card.dfu {"name":"stm32","off":true}`. Clarify which mechanism is authoritative.

### H2 — Auto-apply without confirmation or rollback 🟠
In `loop()`, when `gPowerState <= POWER_STATE_ECO` and `gDfuUpdateAvailable` is set, `enableDfuMode()` is invoked automatically. With ODFU this performs an **immediate hardware reset into the bootloader** the moment a firmware is pushed to Notehub. A single bad/incompatible push can reset the **entire client fleet** with no staged rollout and no rollback bank. This matches the previously filed concern *H5 Auto-DFU Executes Without User Confirmation* (`CODE_REVIEW_02192026_COMPREHENSIVE_CLAUDE.md`). Recommend a `dfu_auto_enable` config flag (default false) and/or staged rollout.

---

## 4. Moderate Findings

### M1 — Isolation logic floats `DE`/`RE` rather than holding them disabled 🟡
The code drives `DE`/`RE` `LOW` and then immediately switches them to `INPUT`:

```cpp
pinMode(PIN_RS485_DE, OUTPUT); digitalWrite(PIN_RS485_DE, LOW);
pinMode(PIN_RS485_RE, OUTPUT); digitalWrite(PIN_RS485_RE, LOW);
pinMode(PIN_RS485_DE, INPUT);   // <-- now floating
pinMode(PIN_RS485_RE, INPUT);   // <-- now floating
```

For most half-duplex transceivers the driver-enable (`DE`) must be **held LOW** to keep the driver off. Floating it (INPUT) leaves the disable state to external pull resistors and leakage; if `DE` drifts/pulls high, the driver re-enables and re-introduces chatter on the very bus we are trying to silence. Recommended: keep `DE` a **push-pull OUTPUT held LOW** (driver disabled) for the duration; only the host TX/RX pins benefit from being floated to high-Z. (Note this is moot until C1 is fixed, since the block is currently dead.)

### M2 — Watchdog-starvation regression 🟡
Prior hardening (`BUGFIX_WATCHDOG_STARVATION_02262026.md`, `CODE_REVIEW_ISSUE247_02262026.md`) carefully ensured the watchdog could still fire during long/blocking operations. The new `while(true){ kick(); }` deliberately defeats the watchdog's recovery role (see C2). Any indefinite wait on hardware should *stop* feeding the watchdog so it can recover the device.

### M3 — Dead IAP/FlashIAP path still compiled into the client 🟡
The client no longer calls `tankalarm_performIapUpdate()` or `dfuKickWatchdog()` (the latter was removed), yet `TankAlarm_DFU.h` defines `tankalarm_performIapUpdate()` as a non-inline `static` function plus `static` `b64decode`/`crc32` helpers. Included but unused in the client translation unit, these will trigger GCC `-Wunused-function` ("defined but not used") and pull in the `FlashIAP` machinery for no reason. Gate the IAP code behind a macro (e.g., `TANKALARM_DFU_USE_IAP`) so each sketch compiles only the mechanism it uses.

---

## 5. Minor / Nits

- **N1 (typo):** Banner reads `"ENABLING COORDINATED HOSTAT ODFU MODE"` — "HOSTAT" should be "HOST" (or removed).
- **N2 (restore inconsistency):** The stop path uses `gSolarManager.isEnabled()`, but the failure-restore path keys off `gConfig.solarCharger.enabled` before calling `gSolarManager.begin(...)`. Use a single source of truth so a partially-initialized manager is restored consistently.
- **N3 (stale comments):** A few client log strings/comments still say "IAP" after the switch to ODFU naming; align them to avoid confusing field logs.
- **N4 (un-kicked window):** Between `RS485.end()`, `delay(1000)`, and the blocking `card.dfu` `requestAndResponse()`, the watchdog is not kicked. With the 30 s `WATCHDOG_TIMEOUT_SECONDS` this is comfortably within budget today, but if the Notecard transaction stalls it could trip; consider a kick before the request for margin.

---

## 6. What's Good
- The high-level design (Modbus "Silent Master" failsafe for dead units + software-gated quiescence for healthy units) is well reasoned and clearly documented.
- Stopping `SolarManager`/`RS485` before reset and saving dirty config (`persistConfigIfDirty()`) before the handshake are correct, sensible steps.
- `card.dfu {"off":true}` at boot to gate autonomous resets until the host authorizes is the right default posture.
- Error/restore branches for the `card.dfu` request creation and no-response cases are present (they re-enable Solar/Modbus), showing the failure paths were considered — they just need the same treatment on the *success-but-no-reset* path (C2).

---

## 7. Suggested Remediation Order
1. **C2** — Restore the original **non-blocking return** pattern from the pre-April ODFU flow (§1a). Remove the infinite kick loop; if a guard window is kept, make it bounded with a watchdog-release/restore fallback. *(Highest priority: this is what can brick a remote unit.)*
2. **C1 / M1** — Implement real RS-485 isolation with defined pins; add a compile-time guard so it can't be silently skipped; hold `DE` LOW rather than float it.
3. **C3** — Make ODFU the single source of truth: correct the stale "ODFU not supported" comment in `TankAlarm_DFU.h`, fix the Server comment, and migrate the Server back to coordinated ODFU. Quick bench re-confirm of `card.dfu` reset is worthwhile but no longer the gating question.
4. **H1 / H2** — Reconcile `user` vs `stm32` flows; gate auto-apply behind config + staged rollout.
5. **M3 / N1–N4** — Cleanup: macro-gate the IAP code, fix typo/comments, unify restore predicate.

> **Note:** The simplest correct fix for C2 may be to diff v1.8.5's `enableDfuMode()` against the pre-April version (`git show 840bc64~1:.../TankAlarm-112025-Client-BluesOpta.ino`) and reuse that proven flow, adding only the RS-485 quiescing step ahead of the `card.dfu on` call.
