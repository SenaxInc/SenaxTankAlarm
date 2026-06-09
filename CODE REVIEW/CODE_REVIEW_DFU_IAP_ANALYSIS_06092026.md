# Code Review: Over-The-Air DFU and In-Application Programming (IAP) Analysis
**Date:** June 9, 2026  
**Status:** Completed and Documented  
**Component:** `TankAlarm-112025-Common` / `TankAlarm_DFU.h`  
**Target:** Arduino Opta + Blues Notecard Carrier  

---

## 1. Executive Summary

Over-the-Air (OTA) firmware updates are the most critical, high-risk part of remote industrial setups. On the Arduino Opta controller with the Blues Wireless carrier board, we have two primary update mechanisms provided by Notehub.io:
1. **Notecard-initiated (Out-of-Band / ODFU)**
2. **Host-initiated (In-Application Programming / IAP)**

This analysis reviews the architectural differences, pros, cons, and performance trade-offs of both approaches under real-world industrial environments (especially solar-powered setups with transient electrical factors and Modbus RTU/RS-485 line loads).

Additionally, we identify **three moderate-risk logic and memory management vulnerabilities** in the existing `TankAlarm_DFU.h` code and outline precise, step-by-step remediation snippets to safeguard field clients against memory leaks, potential bricking, and corruption during flash writes.

---

## 2. In-Depth Comparison: ODFU vs. IAP

### Notecard-Initiated Out-of-Band DFU (ODFU)
In this mode, Notehub instructs the Notecard to handle the flashing process autonomously. The Notecard directly resets and bootstraps the Opta MCU via dedicated physical lines (`NRST` and `BOOT0`).

* **Pros:**
  * **Bricking-Proof:** Because the microcontroller is put into its native ROM bootloader by hardware resets, it completely bypasses the user sketch. If the running program is frozen, hanging, or corrupt, the update will still succeed.
  * **No Flash or Memory Footprint:** The host application requires zero flash space or RAM to download, decode, or write the bytes—entirely preserving the MCU's internal memory.
* **Cons:**
  * **Critical Process Disruption:** The reset is triggered by the Notecard immediately upon download completion, which can suddenly shut down safety-critical relays, active pumps, or interrupt Modbus controller polls.
  * **Serial Port Collisions:** During bootloader handshake negotiation, the debug serial lines are occupied, temporarily locking out active local terminal or USB diagnostic links.
  * **Bus Interference susceptibility:** If nearby RS-485/Modbus lines or other transient sources pull down or bias the shared communication lines, the hardware DFU handshake fails and times out with `{odfu-fail}`.

### Host-Initiated In-Application Programming (IAP)
In this mode, the Notecard acts purely as the storage sandbox. The active host sketch programmatically polls, downloads chunks in base64, decodes them in RAM, and erases/writes its own flash sectors.

* **Pros:**
  * **Subsystem Safety Gating:** The Client sketch controls exactly when to apply the flash (e.g., waiting for active alarms to clear, local relays to de-energize, and solar batteries to exceed a safe 12.8V threshold).
  * **Zero Extraneous Handshakes:** Relies entirely on normal I2C messaging; does not require raw rom-level bootstrap Handshakes over UART which can easily get corrupted.
* **Cons:**
  * **High Bricking Vulnerability:** If you push a firmware that has an immediate crash-on-boot bug, the MCU will restart before it can connect to the Notecard. Since the application code cannot launch, the device cannot pull any rollback binary, permanently bricking it in the field and requiring a physical USB site visit to recover.
  * **Large Code Footprint:** Requires bringing in Mbed's target-specific `FlashIAP` libraries, local Base64 decoders, and CRC-32 arithmetic modules.

---

## 3. Code Review & Vulnerability Analysis

A source-level secure audit of the existing **`TankAlarm-112025-Common/src/TankAlarm_DFU.h`** implementation revealed the following structural issues:

### Findings Overview
1. **Memory Leak in DFU Failure Paths (Moderate Risk)**
2. **Strict Base64 Decoding Boundary Rejection (Low/Moderate Risk)**
3. **Implicit Sector Alignment Assumption (Low Risk)**

---

### Finding 1: Memory Leak in Failure Paths (Moderate Risk)
#### **Location:** `TankAlarm_DFU.h` inside `tankalarm_performIapUpdate()` on lines 369–415

#### **Analysis:**
The program allocates the page-aligned write buffer `progBuf` on the heap when entering the write phase:
```cpp
    // Page-align the program buffer size
    uint32_t alignedBufSize = ((chunkSize + pageSize - 1) / pageSize) * pageSize;
    uint8_t *progBuf = (uint8_t *)malloc(alignedBufSize);

    if (!progBuf) {
      Serial.println(F("IAP DFU: Failed to allocate program buffer"));
      free(progBuf); // freeing NULL is a no-op
      flash.deinit();
      goto iap_restore_hub;
    }
```
However, if a chunk read fails (e.g., if there's an I2C NACK after several retries), the function exits prematurely via `goto iap_restore_hub;`:
```cpp
      if (!chunkOk) {
        Serial.print(F("IAP DFU: Failed to read chunk at offset "));
        Serial.print(offset);
        Serial.println(F(" after retries"));
        free(progBuf); // This clean exit free is called if chunkOk is false...
        flash.deinit();
        goto iap_restore_hub;
      }
```
**The Bug:** If check-bounds or decompression failures occur *inside* the retry loop, the `goto iap_restore_hub;` statement is jumped to **directly without freeing `progBuf`**:
```cpp
        if ((uint32_t)decoded > thisChunk || (uint32_t)decoded > remaining) {
          Serial.print(F("IAP DFU: Decoded size "));
          ...
          free(progBuf); // <-- This is freed
          flash.deinit();
          goto iap_restore_hub;
        }
```
**Wait, what about the verify/CRC failure path?**
If reading back from flash memory blocks reveals a **CRC Mismatch**, the code executes this path on lines 472–478:
```cpp
      if (downloadCrc != readbackCrc) {
        Serial.println(F("IAP DFU: *** CRC MISMATCH — firmware corrupted during flash write ***"));
        Serial.println(F("IAP DFU: Aborting update, device will NOT reboot"));
        flash.deinit();
        goto iap_restore_hub; // <-- progBuf is NOT freed on this path!
      }
```
Since the `progBuf` allocation is successful but we abort due to a CRC failure, `progBuf` remains persistently locked on the heap. If the device repeatedly tries and fails the update under a poor cellular link, the heap memory will leak progressively, eventually triggering an Out-Of-Memory (OOM) crash.

---

### Finding 2: Base64 Decoded Boundary Edge Failures (Low/Moderate Risk)
#### **Location:** `TankAlarm_DFU.h` on line 389

#### **Analysis:**
```cpp
int decoded = tankalarm_b64decode(progBuf, payload, alignedBufSize);
```
Standard Base64 strings can terminate with padding characters (`=`). If the string has a small space padding or trailing invalid character from the Notecard stream, the in-place decoder `tankalarm_b64decode` strictly returns `-1` if the output offset hits `alignedBufSize` exactly, even if the actual payload inside is completely healthy. 

#### **Risk:**
This rejection triggers unnecessary sector write aborts.

---

### Finding 3: Hardcoded Flash Sector Size & Layout (Low Risk)
#### **Location:** `TankAlarm_DFU.h` lines 341–362

#### **Analysis:**
The routine defines:
```cpp
    uint32_t appStart = flashStart + 0x40000; // 256KB Bootloader
```
On typical Portenta/Opta H7 chips with dual-bank Flash memory, this matches the standard boot segment partition correctly. However, if the bootloader layout is configured differently (e.g., if a system integrator uses an asymmetrical partition scheme or a smaller custom boot loader), erasing from `0x40000` is risky.

---

## 4. Remediation & Hardened Implementation

To fully immunize the IAP update sequence on the client, we recommend applying the following code enhancements to `TankAlarm_DFU.h`.

### Step 1: Fix Memory Leaks in the Exit Path
Modify the failure path to ensure `progBuf` is ALWAYS freed symmetrically, regardless of where the failure occurred. This is cleanest by tracking a failure cleanup sequence in the `iap_restore_hub` label block:

```cpp
iap_restore_hub:
  // Symmetrically free the program buffer if we aborted prematurely
  if (progBuf != nullptr) {
    free(progBuf);
    progBuf = nullptr;
  }
  
  // Failure path: restore hub mode so device continues normal operation
  Serial.println(F("IAP DFU: FAILED — restoring normal operation"));
```

This simple fix prevents memory leaks on CRC mismatches, check-bounds failures, and internal loop breaks.

---

## 5. Summary Recommendation for Field Deployments

| Scenario / Deploy Choice | Recommendation | Key Steps to Success |
|---|---|---|
| **Office / Lab Testing (USB connected)** | Use **IAP** or **ODFU** interchangeably | Keep serial monitors closed when trigger/reset occurs. |
| **Clean Field LAN / Grid Power** | Use **ODFU (Out-of-Band)** | Safe, automatic, robust, and completely protects against sketch locks. |
| **Remote Solar Setup (Weak Signal/Modbus)** | Use **IAP (Host-initiated)** | Highly reliable because the MCU has absolute control over power state gating and can pause Modbus transactions before flashing. |

---

# 6. Addendum — Full Review of the Update Logic Under Blocking Events
**Date appended:** June 9, 2026  
**Reviewer focus:** Blocking-event behavior of the IAP update path — watchdog interaction, RS-485 / Modbus bus state, relay/pump safe-states, and the erase-before-download bricking window.  
**Code reviewed (as built):**
- `TankAlarm-112025-Common/src/TankAlarm_DFU.h` — `tankalarm_performIapUpdate()`, `tankalarm_checkDfuStatus()`
- `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino` — `loop()` DFU gate (lines ~1971–1990), `enableDfuMode()`/`checkForFirmwareUpdate()` (lines ~3799–3939), `dfuKickWatchdog()`
- `TankAlarm-112025-Common/src/TankAlarm_Solar.cpp` — synchronous Modbus RTU reads over RS-485
- `TankAlarm-112025-Common/src/TankAlarm_Common.h` — `WATCHDOG_TIMEOUT_SECONDS 30`

> **Threat model for this section:** The IAP update is a *long, single-threaded, blocking* routine executed from `loop()`. For its multi-minute duration the device performs **no** sensor sampling, alarm evaluation, relay logic, or Modbus polling. Every concern below stems from that fact.

---

## 6.1 Watchdog Interaction (HIGH — can reset the MCU mid-flash)

The watchdog is configured at **30 s** (`WATCHDOG_TIMEOUT_SECONDS`), which is near the STM32H7 IWDG ceiling (~32.7 s). The IAP routine relies *entirely* on the optional `kickWatchdog` callback to survive. There are several windows where no kick occurs:

### Finding 6.1-A: Single un-kicked `flash.erase()` of the entire application region (HIGH)
In `tankalarm_performIapUpdate()` the erase is one atomic, blocking call:
```cpp
    if (kickWatchdog) kickWatchdog();
    flashResult = flash.erase(eraseAddr, eraseSize);   // <-- whole app region, no kick inside
    ...
    if (kickWatchdog) kickWatchdog();
```
`eraseSize` is rounded up to cover the *entire* firmware (up to the 1536 KB cap = ~12× 128 KB sectors). On an H7 a 128 KB sector erase is typically ~150–600 ms but is heavily dependent on `Vcore`/voltage scaling and bus contention. In a brown-out-prone solar setup the erase of the full region can approach or exceed the 30 s window. Because the kick happens **before** the call and only resumes **after** it returns, a slow erase = watchdog reset **with the application region already wiped** → guaranteed brick (IAP cannot self-recover; see §6.4).
- **Fix:** Erase sector-by-sector in a loop and kick between each sector:
  ```cpp
  uint32_t a = eraseAddr, end = eraseAddr + eraseSize;
  while (a < end) {
    if (kickWatchdog) kickWatchdog();
    uint32_t ss = flash.get_sector_size(a);
    if (flash.erase(a, ss) != 0) { /* abort */ }
    a += ss;
  }
  if (kickWatchdog) kickWatchdog();
  ```

### Finding 6.1-B: Un-kicked Notecard I2C transactions inside the DFU-mode wait loop (MEDIUM)
```cpp
    while ((millis() - start) < DFU_IAP_MODE_TIMEOUT_MS) {
      if (kickWatchdog) kickWatchdog();
      delay(2000);
      J *req = notecard.newRequest("dfu.get");
      ...
      J *rsp = notecard.requestAndResponse(req);   // <-- blocking I2C, NO kick around it
```
The kick fires, then `delay(2000)` + a blocking `requestAndResponse()` run with no further kick. If the Notecard is mid-sync, busy, or the I2C transaction stalls near its library timeout, the combined gap can approach the 30 s window. The same pattern exists for the per-chunk `dfu.get` in step 4 (kick is at the top of the outer `while`, but each retry performs a blocking `requestAndResponse()` plus `delay(500)` with no kick — three retries × ~(I2C timeout + 500 ms) can be large).
- **Fix:** Kick *immediately before and after* every blocking `requestAndResponse()` and after every `delay()` inside retry loops, not just once per outer iteration.

### Finding 6.1-C: Step 1 `hub.set mode=dfu` has no kick (LOW)
The very first `hub.set` (entering DFU mode) executes a blocking `requestAndResponse()` before the wait loop's first kick. Add a kick before it.

### Finding 6.1-D: Default callback is `nullptr` (MEDIUM, latent)
`tankalarm_performIapUpdate(..., void (*kickWatchdog)() = nullptr)` defaults to no kicking. The client correctly passes `dfuKickWatchdog`, but any future caller (or a refactor) that omits it will brick on the first erase if the watchdog is running. Recommend making the callback **non-optional**, or asserting it is non-null when `TANKALARM_WATCHDOG_AVAILABLE` is defined.

---

## 6.2 RS-485 / Modbus Bus State During the Update (MEDIUM)

The solar/charger telemetry path (`TankAlarm_Solar.cpp`) drives the shared **RS-485** transceiver via synchronous `ModbusRTUClient.requestFrom(...)` calls. Because the whole system is single-threaded and the IAP routine runs to completion inside `loop()`, there is **no concurrent Modbus access** during DFU — that part is safe and is the key reason IAP is preferred over ODFU on noisy buses (IAP moves bytes over **I2C to the Notecard**, not over the RS-485/UART bootloader handshake).

However, two real issues remain:

### Finding 6.2-A: RS-485 driver/DE-RE pin left in its last state (MEDIUM)
Nothing quiesces the RS-485 PHY before the update. Whatever DE/RE (driver-enable) state the **last** Modbus transaction left is held for the entire multi-minute update and across the `NVIC_SystemReset()` glitch. If the last transaction left the driver **enabled**, the node keeps asserting the bus, which can:
- corrupt other masters/slaves sharing the segment, and
- bias the differential pair during the reset window.
- **Fix:** Before erase, explicitly release the bus: end any Modbus session and force the transceiver into receive/high-impedance (`RS485.end()` / drive DE low). Re-init on the post-reboot path.

### Finding 6.2-B: "Pause Modbus before flashing" is claimed but **not implemented** (MEDIUM — doc/code mismatch)
§5 of this document and the IAP "Pros" state the host "can pause Modbus transactions before flashing" and waits for "active alarms to clear, local relays to de-energize, and solar batteries to exceed a safe 12.8 V threshold." The shipped `enableDfuMode()` does **none** of this. The only gate is in `loop()`:
```cpp
  if (gPowerState <= POWER_STATE_ECO) { ... if (gDfuUpdateAvailable) { enableDfuMode(); } }
```
i.e. a coarse power-state check. There is no alarm-active check, no relay-de-energize step, and no explicit Modbus pause. This is a gap between documented intent and behavior and should either be implemented or the claims softened.

---

## 6.3 Relay / Pump Safe-State Across Reboot (MEDIUM–HIGH for a safety device)

On success the routine does:
```cpp
  Serial.flush();
  delay(500);
  NVIC_SystemReset();
```
Outputs (alarm relays, pump-control relays) are **not driven to a defined safe state** before the reset. During `NVIC_SystemReset()` + bootloader re-init + sketch `setup()`, Opta relay GPIOs pass through a floating/default phase and may **glitch** — momentarily energizing or de-energizing a pump or sounder. For a tank-alarm controller this is a process-safety concern, not just cosmetics.
- **Fix:** Immediately before `NVIC_SystemReset()`, command all relays to their fail-safe state (per site policy — typically alarm-asserted/pump-off), then reset. Mirror this de-energize at the very top of `setup()` so the safe state is held continuously through the reboot.
- The same applies to the **failure path** (`iap_restore_hub:`) which resumes `loop()` after potentially minutes of frozen outputs — confirm the first post-DFU loop re-evaluates alarms and relay logic promptly.

---

## 6.4 The Erase-Before-Download Bricking Window (HIGH — the dominant field risk)

The current sequence is **destructive-first**:
1. `flash.erase(appStart, eraseSize)` wipes the entire running application, **then**
2. chunks are pulled one-at-a-time over cellular/I2C (the slowest, least reliable link), **then**
3. CRC-32 is verified.

Between (1) and the end of (2) the device has **no valid application**. Any power loss, cellular stall that outlasts the retries, or watchdog reset (see §6.1-A) in that window leaves a partially-written or empty app. Because IAP recovery **requires the application to boot and reach the Notecard**, a corrupt app cannot pull a rollback — this is the textbook IAP bricking case requiring a physical USB site visit.

Mitigations, in order of preference:
- **Best:** Exploit the Opta/STM32H747 **dual-bank flash** — write/verify the new image into the *inactive* bank, then flip the boot bank (`FLASH_OB` bank-swap) atomically. A failed/partial write never touches the running bank; a bad image can be reverted by swapping back.
- **Good:** Stage the full image to an external/QSPI region first, CRC-verify it **before** erasing the app bank, then do a fast block copy. Shrinks the destructive window from "minutes of cellular download" to "seconds of local copy."
- **Minimum:** Do not erase until the *entire* image has been successfully pulled and buffered/verified. (Not feasible to fully buffer 1.5 MB in RAM, but per-bank staging makes it possible.)

> Note: the **server** GitHub-direct path (`TankAlarm-112025-FTPS_Server_Test.ino`) already does SHA-256 verification of both the download and the flash read-back — stronger than the IAP path's CRC-32. It still shares the same erase-first window, so the dual-bank recommendation applies there too.

---

## 6.5 Authenticity vs. Integrity (MEDIUM, security)

The IAP path validates only a **CRC-32** (integrity against random corruption). It does **not** verify authenticity — there is no signature/hash check on the binary content itself; trust is delegated entirely to the Notehub/Blues transport. The server GitHub path is stronger (SHA-256 against a pinned expected digest). For a remotely-updatable field controller, an unauthenticated image accepted purely on transport trust is an OWASP-class "Software and Data Integrity Failure" (A08). Recommend verifying a signed digest (or at minimum a pinned SHA-256 supplied out-of-band) before booting the new image on the IAP path as well.

---

## 6.6 Retry / Re-arm Behavior After a Failed Update (MEDIUM)

On failure, `enableDfuMode()` clears `gDfuInProgress` but **leaves `gDfuUpdateAvailable == true`**:
```cpp
  gDfuInProgress = false;
  Serial.println(F("IAP DFU failed — resuming normal operation"));
```
On the next DFU interval the `loop()` gate sees `gDfuUpdateAvailable` still true and **immediately re-attempts** — re-entering DFU mode and re-erasing the app bank. A *deterministic* failure (e.g. a genuine CRC mismatch from a bad build, or a chronically weak link) therefore loops forever: repeated full-region erases (flash wear) and repeated multi-minute offline windows during which **no alarms are delivered**.
- **Fix:** Add a per-version failure counter with exponential backoff; after N consecutive failures for the same `gDfuVersion`, latch "do not auto-apply this version" and surface it via health telemetry for operator action.
- Also note the client auto-apply path does **not** compare versions for "is newer" before applying (it only checks `version != last-seen`), so a mistaken Notehub assignment could trigger a **downgrade** loop. Add a `compareFirmwareVersions()` "strictly newer" guard before auto-applying.

---

## 6.7 Availability Gap: No Alarm Delivery While in DFU Mode (MEDIUM)

`hub.set mode=dfu` stops sync, and the routine then blocks for the full download. For a tank-alarm safety system this is a multi-minute window where a real high-level/leak alarm **cannot be transmitted**. Combined with §6.6's retry loop this can become a significant cumulative outage.
- **Fix:** Bound the total DFU wall-clock time (`DFU_IAP_MODE_TIMEOUT_MS` only covers mode entry, not the whole download). Before entering DFU mode, force a final alarm/state flush, and prefer scheduling auto-DFU only when no alarm is active (ties into §6.2-B).

---

## 6.8 Summary of Addendum Findings

| # | Finding | Severity | Core Risk |
|---|---|---|---|
| 6.1-A | Whole-region `flash.erase()` with no kick inside | **HIGH** | Watchdog reset mid-erase → brick |
| 6.1-B | Notecard I2C calls in wait/retry loops un-kicked | MEDIUM | Watchdog reset during download |
| 6.1-C | First `hub.set mode=dfu` un-kicked | LOW | Edge-case reset on entry |
| 6.1-D | `kickWatchdog` defaults to `nullptr` | MEDIUM | Future caller bricks silently |
| 6.2-A | RS-485 DE/RE left in last state across update+reset | MEDIUM | Bus contention / corruption |
| 6.2-B | "Pause Modbus / wait for safe conditions" not implemented | MEDIUM | Doc/code mismatch |
| 6.3 | Relays/pumps not driven to safe state before reset | MEDIUM–HIGH | Output glitch on reboot |
| 6.4 | Erase-before-download destructive window | **HIGH** | Brick on power loss / stall |
| 6.5 | CRC-32 only (no authenticity) on IAP path | MEDIUM | Unauthenticated firmware (A08) |
| 6.6 | `gDfuUpdateAvailable` not cleared on failure → retry loop | MEDIUM | Flash wear + repeated outage |
| 6.7 | No alarm delivery during DFU; total time unbounded | MEDIUM | Safety availability gap |

### Top three priorities
1. **Make erase watchdog-safe and non-destructive** — sector-by-sector erase with kicks (6.1-A) and move to dual-bank staged write (6.4). Together these eliminate the dominant brick scenarios.
2. **Define output/bus safe-states** — de-energize relays and release the RS-485 driver before `NVIC_SystemReset()`, and re-assert at the top of `setup()` (6.2-A, 6.3).
3. **Add failure backoff + "strictly newer" guard** so a bad image or weak link cannot loop forever erasing flash and starving alarm delivery (6.6, 6.7).

---

# 7. Implementation Plan

This plan sequences the §6 findings into four phases ordered by risk-reduction-per-effort. Phases 1–2 are *defensive* (low blast radius, no flash-layout changes) and should ship first. Phase 3 is the *structural* dual-bank rework. Phase 4 is *hardening*. Each task lists the target file, the exact insertion point, and an acceptance check.

> **Guiding principle:** Land the cheap, high-value safety guards (watchdog-safe erase, safe-states, failure backoff) *before* the larger dual-bank refactor, so field units are protected even if Phase 3 slips.

---

## Phase 0 — Pre-work: Pre-Update Hooks (enables everything else)

Introduce a small, sketch-supplied callback bundle so `tankalarm_performIapUpdate()` can quiesce the system without the common library depending on sketch internals (relays, Modbus). This keeps the library decoupled while fixing 6.2 and 6.3.

**File:** `TankAlarm-112025-Common/src/TankAlarm_DFU.h`
- Extend the signature with optional callbacks (keep `kickWatchdog` mandatory in practice — see Phase 1):
  ```cpp
  struct TankAlarmDfuHooks {
    void (*kickWatchdog)();        // REQUIRED when watchdog is enabled
    void (*enterSafeState)();      // de-energize relays/pumps, release RS-485
    bool (*okToUpdateNow)();       // returns false if an alarm is active
  };
  static bool tankalarm_performIapUpdate(Notecard &notecard, uint32_t firmwareLength,
                                         const char *hubMode, const TankAlarmDfuHooks &hooks);
  ```
- Keep a thin backward-compatible overload that wraps the old `void(*)()` kick-only signature so the server test sketch still compiles during migration.

**Files (callers):** client `enableDfuMode()` and `TankAlarm-112025-FTPS_Server_Test.ino` — populate the hook struct.

**Acceptance:** both sketches compile; `okToUpdateNow()` defaults to `true` if not supplied.

---

## Phase 1 — Watchdog-Safe Blocking (fixes 6.1-A/B/C/D)

**Task 1.1 — Sector-by-sector erase with kicks (6.1-A).**  
`TankAlarm_DFU.h`, Step 3 erase block. Replace the single `flash.erase(eraseAddr, eraseSize)` with the per-sector loop from §6.1-A, kicking before each sector. Apply the *same* change to the server GitHub-direct path in `TankAlarm-112025-FTPS_Server_Test.ino`.
- **Acceptance:** with the watchdog at 30 s, a full ~1.5 MB erase completes without reset on the bench (instrument with a per-sector `Serial` tick).

**Task 1.2 — Kick around every blocking Notecard call (6.1-B, 6.1-C).**  
`TankAlarm_DFU.h`, Step 1 (`hub.set mode=dfu`), Step 2 (DFU-mode wait loop), and Step 4 (per-chunk retry loop). Add `hooks.kickWatchdog()` immediately **before and after** each `requestAndResponse()` and after each `delay()`.
- **Acceptance:** induce a stalled Notecard (pull SDA briefly / unplug) and confirm no watchdog reset within one retry cycle.

**Task 1.3 — Make the kick non-optional when the watchdog is armed (6.1-D).**  
With the `TankAlarmDfuHooks` struct, treat `kickWatchdog == nullptr` as a hard error when `TANKALARM_WATCHDOG_AVAILABLE` is defined: log and **abort to `iap_restore_hub`** before erasing, rather than proceeding into a guaranteed brick.
- **Acceptance:** a unit/compile-time guard or an early runtime check that refuses to erase without a kick callback.

---

## Phase 2 — Output & Bus Safe-States + Failure Backoff (fixes 6.2, 6.3, 6.6, 6.7)

**Task 2.1 — `enterSafeState()` before erase (6.2-A, 6.3).**  
Implement in the client sketch: drive alarm/pump relays to the site-defined fail-safe, then `RS485.end()` / force DE low. Call `hooks.enterSafeState()` in `tankalarm_performIapUpdate()` *immediately before* the erase, and again on the `iap_restore_hub:` path's exit so the failure path also resumes from a known state.
- Mirror the relay safe-state at the **very top of `setup()`** so it is held continuously through `NVIC_SystemReset()`.
- **Acceptance:** scope/log relay GPIO and DE pin: no spurious energize during update or across reboot.

**Task 2.2 — `okToUpdateNow()` alarm gate (6.2-B, 6.7).**  
In the client `loop()` DFU gate, AND the existing `gPowerState <= POWER_STATE_ECO` check with `!anyAlarmActive()`. Pass the same predicate as `hooks.okToUpdateNow` so the library re-checks right before committing.
- Force a final alarm/state flush (`hub.sync` of the outbox) before `hub.set mode=dfu`.
- **Acceptance:** with a simulated active alarm, auto-DFU is deferred; the alarm note is delivered before DFU entry.

**Task 2.3 — Failure backoff + clear stale availability (6.6).**  
In client `enableDfuMode()` failure tail: do **not** leave `gDfuUpdateAvailable` latched. Add `gDfuFailCountForVersion` keyed on `gDfuVersion`; after `DFU_MAX_ATTEMPTS` (e.g. 3) consecutive failures for the same version, set `gDfuVersionBlocked` and stop auto-applying until a *different* version appears or an operator clears it. Surface the blocked state in health telemetry.
- **Acceptance:** a deterministically-failing image stops retrying after N attempts; a subsequent new version still updates.

**Task 2.4 — "Strictly newer" guard (6.6).**  
Before auto-applying, require `compareFirmwareVersions(gDfuVersion, FIRMWARE_VERSION) > 0`. Reuse the existing `compareFirmwareVersions()` already present in the server test sketch (promote it to `TankAlarm_Utils.h` for shared use).
- **Acceptance:** an equal or older advertised version does not trigger an auto-downgrade.

---

## Phase 3 — Non-Destructive Dual-Bank Update (fixes 6.4; the structural change)

The STM32H747 on Opta is dual-bank (2× 1 MB). Stage into the inactive bank and swap, so a failed/partial write never touches the running image.

**Task 3.1 — Bank topology helper.**  
Add `TankAlarm_FlashBank.h` in common: detect current boot bank, expose `inactiveBankStart()`, and wrap the `FLASH->OPTCR` `BFB2`/`SWAP_BANK` option-byte flip behind a single `commitBankSwap()` that performs the option-byte program + reset.
- **Acceptance:** read-back confirms boot bank toggles after `commitBankSwap()` on the bench.

**Task 3.2 — Retarget IAP write to the inactive bank.**  
In `tankalarm_performIapUpdate()`: erase + program into `inactiveBankStart()` instead of `flashStart + 0x40000`. Keep the running app untouched throughout download and CRC verify.
- **Acceptance:** during download the device still runs valid code (pull power mid-download → reboots into the *old* working app, not a brick).

**Task 3.3 — Verify-then-swap commit.**  
Only after the read-back CRC (Phase 4 upgrades this to SHA-256) passes against the *inactive* bank, call `commitBankSwap()` → reset. On any failure, simply restore hub mode and keep running the current bank — no rollback binary needed because the old bank was never erased.
- **Acceptance:** corrupt the staged image deliberately → device declines swap and continues on current firmware.

**Task 3.4 — Apply the same dual-bank flow to the server GitHub-direct path** in `TankAlarm-112025-FTPS_Server_Test.ino` (it already has SHA-256; only the target address + commit step change).

---

## Phase 4 — Authenticity Hardening (fixes 6.5)

**Task 4.1 — Upgrade IAP integrity check to SHA-256.**  
Replace the CRC-32 in `TankAlarm_DFU.h` with the same `mbedtls_sha256` streaming verification already used on the server path (factor it into a shared helper).

**Task 4.2 — Pinned/-signed digest.**  
Require an expected digest delivered out-of-band (Notehub env var / config note for IAP; release manifest for GitHub) and refuse `commitBankSwap()` unless the staged image matches. Optionally verify an Ed25519 signature over the digest for full authenticity.
- **Acceptance:** an image whose digest does not match the pinned value is rejected before swap (negative test).

---

## 7.1 Sequencing, Effort & Dependencies

| Phase | Fixes | Relative Effort | Risk if Deferred | Depends On |
|---|---|---|---|---|
| 0 | Hook plumbing | S | — (enabler) | — |
| 1 | 6.1-A/B/C/D | S–M | **HIGH** (active brick risk) | 0 |
| 2 | 6.2, 6.3, 6.6, 6.7 | M | MEDIUM–HIGH (safety/outage) | 0 |
| 3 | 6.4 | **L** | **HIGH** (brick window) | 1 |
| 4 | 6.5 | M | MEDIUM (security/A08) | 3 |

**Recommended release cut:** ship **Phase 1 + Phase 2** as a single safety patch (`vX.Y` minor — no flash-layout change, fully backward compatible), then land **Phase 3** behind a build flag (`DFU_DUAL_BANK`) for staged field validation, and finally enable **Phase 4** once Notehub/release tooling supplies the pinned digest.

## 7.2 Test Matrix (per phase, on hardware)

| Scenario | Expected (post-fix) |
|---|---|
| Power loss during erase | Phase 1: reset is survivable only post-Phase 3; Phase 3: reboots into old bank |
| Watchdog stall (Notecard unplugged mid-download) | No MCU reset; clean abort to `iap_restore_hub` |
| Active alarm at update time | Update deferred; alarm delivered first |
| Deterministically corrupt image | Declined after N attempts; current firmware retained; telemetry flags it |
| Older version advertised | No downgrade |
| Tampered image (wrong digest) | Phase 4: rejected before swap |
| Relay/DE pins during update+reboot | No spurious energize; bus released |

## 7.3 Out-of-Scope / Follow-ups
- ODFU (hardware AUX) remains unavailable on Wireless-for-Opta and is not part of this plan.
- A/B persistent "boot-confirmed" flag (mark new bank *confirmed* only after it boots and reaches Notehub once; auto-revert on next boot if unconfirmed) is a strong future addition layered on Phase 3.

---

## 8. Reviewer Thoughts and Advisory Suggestions
The proposed multi-phase implementation plan starting from **Phase 0** and working progressively through **Phase 4** is exceptionally thorough, highly structured, and targets the direct risks associated with field-level IAP updates. Specifically:
* Splitting the work into a lightweight, backward-compatible, change-free **Phase 1 & 2** (Watchdog and Safe-state fixes) and reserving the deep-cutting dual-bank flash logic for **Phase 3** represents the gold standard of defensive field engineering. It minimizes the development blast radius while immediately preventing the leading causes of actual field-unit brick resets.

Below are strategic advisory suggestions to enhance and extend this plan:

### 1. ⚠️ The "Zombie Rollback" Pitfall (Highly Critical for Phase 3)
When implementing **Phase 3 (Non-Destructive Dual-Bank Update)**, we boot atomically into the alternate bank using `commitBankSwap()`. 
* **The Risk:** If the newly flashed image has a subtle bug that compiles cleanly but causes a HardFault or an immediate stack crash 3 seconds *after* boot or if it enters a crash-reboot loop, it will never be able to execute any new DFU commands or restore its previous state. The unit enters a "Zombie Loop" where the watchdog continuously resets, boots the bad bank, crashes, and resets.
* **Advisory Suggestion:** You should companion **Phase 3** with a **Hardware Boot Confirmation State Machine (A/B Boot Latching)**:
  1. Upon bank swap, the bootloader (or early `setup()`) marks the new bank as "unconfirmed" in a dedicated, retained register or flash offset.
  2. If the application boots, successfully registers with the Notecard, and obtains its first valid `hub.sync` back to the cloud, the application marks the bank as "confirmed" (stable).
  3. If the MCU resets (by watchdog or panic) *before* reaching this confirmation threshold, the board's early boot sequence detects that an unconfirmed bank failed. It immediately executes an automatic bank rollback (`commitBankSwap()` back to the old, known-working bank) before initializing the bad application. This creates a true, bullet-proof self-healing industrial system.

### 2. ⚡ Brownout Management Gating (Phase 2 Enhancement)
During **Phase 2 (Output & Bus Safe-States)**:
* **The Risk:** Flashing is a high-power operation due to flash sector charge-pump activity. If voltage fluctuates on a solar client, even if the primary starting voltage was >12.8V, the continuous current draw of maintaining a cellular download *plus* continuous flash erasure can sag a batteries-under-load capacity below the brown-out threshold (BOR).
* **Advisory Suggestion:** 
  1. Add an active **Voltage Trend Check** in `okToUpdateNow()`. Refuse DFU if the battery voltage slope is negative or falling rapidly, indicating that the unit is currently in high-draw or discharging conditions with low solar supply.
  2. Perform the update sequentially only during peak charging hours (e.g., gating updates so they only fire between 10:00 AM and 2:00 PM local timezone when solar generation is historically most active).

### 3. 🛡️ SHA-256 Signature Padding Gaps (Phase 4 Enhancement)
During **Phase 4 (Authenticity Hardening)**, when migrating from CRC-32 to SHA-256 streaming verification:
* **The Risk:** Flash memory sectors are padded to page alignments (completed with default `0x00` or `0xFF` erase values). If you perform a read-back SHA-256 verification of the final compiled length, any page-boundary padding bytes written to the tail of the flash memory block must be symmetrically padded or trimmed from your reference binary’s SHA-256 digest, or the hashes will mismatch.
* **Advisory Suggestion:** Embed the exact **Expected Application Cargo Size** (not rounded block size) inside a fixed struct header at a predefined offset of the binary image (or pass it out-of-band via Notehub envelopes). Ensure your SHA-256 read-back routine bounds itself strictly to the *exact* original binary length (leaving out the page-erased tail padding) to ensure absolute hash equivalence.

### 4. 📈 Flash wear-leveling considerations (Ongoing advisory)
The STM32H747XI internal flash is high-end, but typical sector erasure life on internal flash is rated for roughly **10,000 to 100,000 program/erase cycles**.
* **The Risk:** A deterministic update loop (detailed in Finding 6.6) can wear out the flash sector gate oxides in a matter of weeks if left unchecked.
* **Advisory Suggestion:** Enforce an unbypassable **minimum 24-hour cool-down period** between DFU retries, regardless of whether a new version is requested, unless manual intervention is triggered locally via the physical front-panel USER button. Record persistent failure counts in the Opta's Backup SRAM or OTP (One-Time Programmable) area so they survive power losses.

---

## 9. Second Reviewer Notes on the Implementation Plan

The Phase 0-4 plan is directionally correct and captures the right order of operations: first prevent watchdog/safe-state failures, then remove the destructive flash window, then harden authenticity. I would keep that sequencing, but tighten several implementation details before coding so the fix does not create a new class of field failures.

### 9.1 Promote A/B boot confirmation into Phase 3, not a follow-up

Section 8's boot-confirmation warning is important enough to become a **Phase 3 acceptance requirement**. A bank swap without a boot-confirmed latch protects against interrupted writes, but it does **not** protect against a validly written image that crashes immediately after reset. The dual-bank work should therefore include these states from the first implementation:
- `current_bank_confirmed`
- `pending_bank`
- `pending_version`
- `boot_attempt_count`
- `last_reset_reason`

The new bank should only be marked confirmed after the application reaches a real health threshold, not merely after `setup()` starts. A practical threshold for this product is: relays initialized to the site-safe state, Notecard reachable, time/config loaded enough to evaluate alarms, and at least one successful outbound health/update-status note queued or synced. If that threshold is not reached within N watchdog resets or one boot deadline, early boot should swap back before normal application logic proceeds.

### 9.2 Validate the Opta flash map before assuming dual-bank drop-in support

The current updater writes to `flashStart + 0x40000`, matching the Arduino application start assumption. Phase 3 should start with a small proof-of-concept that proves the actual Opta/Mbed/Arduino boot chain can boot both banks safely. Specifically verify:
- the linker script and vector table location for the application image,
- whether the Arduino bootloader expects the sketch at a fixed offset,
- whether option-byte bank swap preserves the bootloader/application relationship,
- whether `SCB->VTOR` and interrupt vectors are correct after swap,
- the true maximum image size per inactive bank.

The current `1536 KB` application limit is not automatically compatible with a simple `2 x 1 MB` A/B layout. If the bootloader occupies part of bank 1 and the inactive bank is only one physical bank, the usable A/B image size may be closer to the smaller bank budget. If the current firmware can grow beyond that, the safer architecture may be a tiny immutable boot stub plus external/QSPI staging rather than direct bank swap from the application.

### 9.3 Split safe-state hooks into enter, restore, and pre-reset hooks

The proposed `enterSafeState()` hook is the right decoupling idea, but it should not be the only lifecycle hook. Calling the same hook on the failure path could leave the RS-485/Modbus stack stopped and relays held in a service state after the application resumes. Prefer a lifecycle bundle such as:
```cpp
struct TankAlarmDfuHooks {
  void (*kickWatchdog)();
  bool (*okToUpdateNow)();
  bool (*prepareForUpdate)();       // final alarm flush, stop Modbus, safe relays
  void (*restoreAfterAbort)();      // restart Modbus, force immediate sensor/alarm pass
  void (*prepareForReset)();        // final relay/bus state just before NVIC_SystemReset()
  void (*onProgress)(uint32_t done, uint32_t total);
};
```
For the client, `prepareForUpdate()` should call the existing `SolarManager::end()` path instead of reaching directly into low-level RS-485 pins where possible. `restoreAfterAbort()` should re-run the solar/Modbus initialization with the saved config and schedule an immediate sensor/alarm evaluation so the device does not wait for the normal polling interval after a failed update.

### 9.4 Watchdog kicks do not fix a permanently blocking I2C call

Kicking immediately before and after `requestAndResponse()` is necessary, but it is not sufficient if the Notecard or I2C driver can block longer than the watchdog window. Phase 1 should also define bounded transport behavior:
- configure/request a Notecard transaction timeout if the library exposes one,
- set an I2C/Wire timeout when supported by the platform,
- add an I2C bus recovery path for stuck SDA/SCL before retrying,
- count total DFU wall-clock time, not only DFU-mode-entry time,
- treat failure to send `dfu.status stop` or restore `hub.set` as a reportable degraded state.

Every blocking call in the success and failure cleanup paths should use the same watchdog discipline. That includes `dfu.status stop`, `hub.set`, final `hub.sync`, TLS reads in the GitHub-direct path, and any alarm-flush operation added before entering DFU mode.

### 9.5 Preserve the server path's existing SHA-256 strengths

The FTPS/GitHub-direct path already computes SHA-256 over the advertised asset size during download and again over the same byte count from flash. That is the right padding behavior: page-alignment bytes are written as erase-value padding, but they are not part of the expected digest. When this is factored into common helpers, preserve the explicit `(expectedDigest, expectedLength)` pairing so future callers cannot accidentally hash the rounded program size.

One related security note: the direct TLS download currently disables certificate verification and relies on the digest fetched from GitHub release metadata. That is acceptable only if the digest source is trusted independently of the asset transport. Phase 4 should make this explicit by requiring the expected digest from a pinned/signed manifest, Notehub environment/config channel, or release metadata fetched over a trusted path. Do not fetch the binary and its trust anchor from the same unauthenticated channel.

### 9.6 Use durable, wear-aware state for retry and boot metadata

The 24-hour DFU retry cooldown is a good addition, but the suggested storage targets need refinement. Backup SRAM may survive watchdog resets but is not reliable across full power loss unless the VBAT domain is maintained. OTP is not appropriate for counters because it is one-time programmable and cannot support repeated retry bookkeeping.

Use a small wear-aware record instead: LittleFS with a two-slot journal, a dedicated flash settings page with sequence numbers, or a Notehub/Notecard state note mirrored locally. The stored record should include version, target firmware type, attempt count, first-failure time, next-allowed-at time, last failure reason, and whether the failure happened before or after flash erase/swap.

### 9.7 Make `okToUpdateNow()` stricter than `!anyAlarmActive()`

The update gate should check more than active alarm thresholds. For this product, a safe auto-update gate should include:
- no active high/low/leak alarm,
- no manual relay command currently holding a relay on,
- no unacknowledged critical alarm note queued locally,
- battery voltage above threshold with non-negative trend,
- charger/solar telemetry recent enough to trust,
- no config update currently pending or partially applied,
- a local service override only when deliberately requested.

The peak-solar-hours suggestion is useful, but it should be implemented as a policy input rather than hard-coded local time. If time sync is stale, the device should either defer or fall back to a conservative power-only rule.

### 9.8 Re-audit cleanup scope before implementing the shared failure label

If the cleanup label is expanded to free buffers and deinitialize flash, be careful with C++ object scope and `goto`. The current `progBuf` allocation is inside the flash block, and the label is outside that block. A central cleanup label cannot safely reference block-scoped locals unless the variables are hoisted or wrapped in a small RAII helper. Prefer simple RAII-style wrappers or a single `DfuContext` struct over many `goto` exits.

Also re-check the current code before treating the original CRC-path leak as still present: in the current `TankAlarm_DFU.h`, `progBuf` is freed before read-back CRC verification. The cleanup improvement is still worthwhile, but it should be implemented as defensive simplification rather than assuming that exact leak path still exists.

### 9.9 Add field telemetry for every DFU terminal state

The plan should require a compact DFU telemetry note for success, defer, abort, blocked, rollback, and failed-restore states. Minimum fields: current version, target version, target type, firmware length, phase, elapsed seconds, reset reason, failure reason, attempt count, free heap minimum, battery voltage/trend, and active alarm summary. This will make field failures diagnosable without USB access and will confirm whether the new gates are too conservative.

### 9.10 Recommended adjustment to the release cut

I would still ship Phase 1 + Phase 2 first, but include these additions in that same safety patch:
- total DFU wall-clock timeout,
- transport timeout/bus recovery notes,
- durable failure backoff record,
- explicit restore-after-abort hook,
- DFU terminal-state telemetry.

Then gate Phase 3 behind `DFU_DUAL_BANK_EXPERIMENTAL` until the flash-map proof, bank-swap proof, and boot-confirmation rollback all pass on hardware. The dual-bank feature should not be considered field-ready until a deliberately crashing image automatically rolls back to the previous working firmware.
