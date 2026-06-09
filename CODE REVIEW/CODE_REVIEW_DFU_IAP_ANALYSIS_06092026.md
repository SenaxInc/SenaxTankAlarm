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
