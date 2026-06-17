# CODE REVIEW — OTA Update Failure: v1.9.30 → v1.9.31

**Date:** 2026-06-21
**Scope:** Root cause analysis of why the OTA firmware update from v1.9.30 to v1.9.31 is not applying to the test client Opta
**Companion:** [CODE_REVIEW_06152026_OTA_VS_USB_UPDATE_FAILURE.md](CODE_REVIEW_06152026_OTA_VS_USB_UPDATE_FAILURE.md) (prior analysis, same class of failure)

---

## 1. Executive Summary

The OTA update from v1.9.30 to v1.9.31 is failing because **v1.9.30 was compiled locally and almost certainly lacks the `-DTANKALARM_DFU_MCUBOOT` build flag**. Without this flag, the MCUboot staging code is compiled out entirely — the firmware detects the v1.9.31 OTA image, enters `enableDfuMode()`, but immediately hits the `#else` fallback (`bool ok = false;`), silently rejects the update, clears the Notecard DFU state, and stays on v1.9.30 forever.

There is no git tag for v1.9.30, confirming it was not built by CI. The CI workflow (`release-firmware-112025.yml`) is the only build path that includes both `--fqbn arduino:mbed_opta:opta:security=sien` and `-DTANKALARM_DFU_MCUBOOT`. A local `arduino-cli compile` without these arguments produces a binary that can run normally but **cannot perform MCUboot OTA updates**.

---

## 2. How the Update Was Deployed

| Step | What happened |
|------|--------------|
| 1 | OTA code fixes applied (commit `47d326e`) — sscanf bounds, goto cleanup, latch bug, `stopRefusedFirmware()` |
| 2 | Version bumped to `1.9.30`, compiled locally, installed to test client Opta via USB |
| 3 | Version bumped to `1.9.31`, pushed & tagged → CI build ran with `security=sien` + `-DTANKALARM_DFU_MCUBOOT` |
| 4 | `TankAlarm-Client-secure-v1.9.31.slot.bin` downloaded from GitHub Release and uploaded to Notehub |
| 5 | OTA applied to the test client device via Notehub Host Firmware |
| 6 | Telemetry continues to report v1.9.30 — update never applies |

---

## 3. Evidence: v1.9.30 Was Not Built by CI

Git tags in the `v1.9.2x`–`v1.9.3x` range:

```
v1.9.20  v1.9.21  v1.9.22  v1.9.23  v1.9.24
v1.9.26  v1.9.27  v1.9.28
                                     v1.9.31
```

**Missing:** v1.9.25, v1.9.29, **v1.9.30**. The CI workflow (`release-firmware-112025.yml`) only runs on tag push. No tag → no CI build → no `security=sien` → no `-DTANKALARM_DFU_MCUBOOT`.

---

## 4. Root Cause: The `#ifdef` Gate in `enableDfuMode()`

The client firmware's `enableDfuMode()` function contains this compile-time gate:

```cpp
#if defined(TANKALARM_DFU_MCUBOOT)
  bool ok = tankalarm_performMcubootUpdate(
      notecard, gDfuStatus, restoreMode, DEVICE_ROLE, dfuKickWatchdog);
#else
  bool ok = false;
  // MCUboot DFU support is not compiled in — stop the pending update so the
  // Client does not repeatedly attempt to apply it on every DFU check cycle.
  { ... stops the update on the Notecard ... }
#endif
```

**When `-DTANKALARM_DFU_MCUBOOT` is NOT defined:**
1. The device detects `mode="ready"` from `dfu.status` ✓
2. `checkForFirmwareUpdate()` sets `gDfuUpdateAvailable = true` ✓
3. `enableDfuMode()` is called ✓
4. `gDfuInProgress = true` is set ✓
5. **`bool ok = false;`** — the MCUboot staging path is completely skipped ✗
6. The code clears the Notecard DFU state (stops the pending update)
7. `gDfuInProgress = false` — the device resumes normal operation
8. The OTA update is **silently rejected** and the Notecard state is cleared

The device will never attempt this update again because the Notecard DFU state has been cleared. It continues running v1.9.30 indefinitely.

---

## 5. Additional Prerequisite Layers (Even If the Flag Were Present)

Even if v1.9.30 had been compiled with `-DTANKALARM_DFU_MCUBOOT`, there are three more prerequisites that must ALL be satisfied for OTA to succeed. These were identified in the [prior code review](CODE_REVIEW_06152026_OTA_VS_USB_UPDATE_FAILURE.md) as H1–H3:

### 5.1 MCUboot Bootloader Must Be Installed (Prior H1 — ★ LEADING)

The stock Arduino Opta ships with a standard bootloader ("Arduino loader / Bootloader version: 255"). MCUboot is a **separate** bootloader that must be flashed explicitly. Without MCUboot:
- `MCUboot::applyUpdate(false)` writes a swap trailer to QSPI that **nothing acts on**
- `NVIC_SystemReset()` reboots into the stock bootloader, which boots the old app
- USB flashing bypasses this entirely (uses STM32 ROM DFU)

A bench Opta read on 2026-06-10 showed `"Arduino loader / Bootloader version: 255"` — the **stock** bootloader. This must be verified on the actual test unit.

### 5.2 QSPI Must Be Provisioned (Prior H2 — ★ HIGH)

The MCUboot staging writes the firmware image to `/fs_ota/update.bin` on QSPI partition 2. If this partition was never created (via `TankAlarm-112025-KeyProvisioning`), the staging function fails immediately with:

```
MCUboot DFU: ERROR - /fs_ota/update.bin missing or failed to open
(QSPI partition not provisioned; run KeyProvisioning). Aborting.
```

### 5.3 Signing Key Alignment (Prior H3 — ★ HIGH)

The CI now builds with `security=sien`, which uses the **Arduino core's default MCUboot keychain** (bundled imgtool 1.8.0-arduino.2 with Arduino's signing/encryption keys). This was changed from the previous custom-key approach in commit `a9bacd1`:

> *"The prior approach produced a .slot.bin the v25 bootloader would not swap on the device."*

The **default MCUboot bootloader** (from the Arduino BSP) trusts the **default Arduino keys**. So the CI `.slot.bin` is correctly signed for the stock MCUboot bootloader.

However, if the device was provisioned via `TankAlarm-112025-KeyProvisioning` with **custom keys** from `mcuboot_keys/`, those keys will NOT match the Arduino defaults → the bootloader will reject the image.

**Key chain alignment table:**

| Bootloader keys | CI signing keys | Result |
|----------------|----------------|--------|
| Arduino default | Arduino default (`security=sien`) | ✓ Match |
| Custom (`mcuboot_keys/`) | Arduino default (`security=sien`) | ✗ **Mismatch** |
| Arduino default | Custom (`mcuboot_keys/` via pip imgtool) | ✗ **Mismatch** |
| Custom (`mcuboot_keys/`) | Custom (`mcuboot_keys/` via pip imgtool) | ✓ Match (but abandoned — image divergence) |

---

## 6. The `security=sien` FQBN Variant — What It Does

When the Arduino core compiles with `--fqbn arduino:mbed_opta:opta:security=sien`:

1. **Linker script changes:** The application binary is laid out with a MCUboot header (0x20000 offset), different from the standard layout
2. **Post-build signing:** The core's bundled imgtool automatically signs and encrypts the output `.bin` using the default Arduino MCUboot keychain
3. **MCUboot compatibility:** The resulting binary can ONLY be booted by the MCUboot bootloader (not the stock bootloader)

**Critical implication:** A binary compiled WITHOUT `security=sien` and flashed via USB runs with the stock bootloader layout. A binary compiled WITH `security=sien` for OTA has a DIFFERENT memory layout. Both can coexist if the device has been properly bootstrapped — USB-flash the first `security=sien` image (which installs MCUboot as part of the `*.with_bootloader.bin`), then all subsequent OTA updates use MCUboot for the swap.

---

## 7. The Blues IAP DFU Flow — Confirmed Working Design

Per Blues documentation ([IAP Firmware Update](https://dev.blues.io/notehub/host-firmware-updates/iap-firmware-update/), [Notecard API Requests for DFU](https://dev.blues.io/notehub/host-firmware-updates/notecard-api-requests-for-dfu/)):

| Phase | Step | API / Action |
|-------|------|-------------|
| A | Notehub downloads firmware to Notecard | Automatic (background) |
| B | Host polls status | `dfu.status` → checks `mode` field |
| C | Host enters DFU mode | `hub.set mode=dfu` |
| D | Host retrieves firmware in chunks | `dfu.get` with `length` + `offset` |
| E | Host writes to update region | QSPI `/fs_ota/update.bin` |
| F | Host validates (CRC32) | Compare against `body.crc32` from `dfu.status` |
| G | Host triggers swap + reboot | `MCUboot::applyUpdate(false)` → `NVIC_SystemReset()` |
| H | Bootloader verifies + swaps | MCUboot verifies signature/encryption, swaps slots |
| I | New firmware confirms itself | `MCUboot::confirmSketch()` |

The code in `TankAlarm_DFU.h::tankalarm_performMcubootUpdate()` correctly implements Phases C–G. The MCUboot confirmation is done early in `setup()`. The code is sound — the issue is that **this entire code path is compiled out** without `-DTANKALARM_DFU_MCUBOOT`.

---

## 8. OTA Fixes Applied in Commit 47d326e — Review

The following fixes were applied before v1.9.30. All are correct and well-implemented:

| Fix | Status | Notes |
|-----|--------|-------|
| Removed `gDfuInProgress` latch bug | ✓ Correct | `downloading` state no longer sets the latch — was causing permanent OTA suppression |
| Added `stopRefusedFirmware()` | ✓ Correct | Clears Notecard DFU state for refused images (downgrade, blacklisted) so they don't re-trigger every cycle |
| Added downgrade guard | ✓ Correct | `tankalarm_compareSemver()` prevents downgrade unless forced |
| Added retry-in-1-min on deferred check | ✓ Correct | If DFU check is deferred (Notecard busy), retries in 60s instead of waiting for next full interval |
| Fixed `gNotecardAvailable` in wireless-error path | ✓ Correct | Notecard availability flag is now properly maintained |
| Added state cleanup on staging failure | ✓ Correct | `goto mcuboot_restore_hub` pattern ensures hub mode is restored on any failure path |

**These fixes are all present on `origin/master` and would be effective IF the firmware were compiled with `-DTANKALARM_DFU_MCUBOOT`.**

---

## 9. Diagnosis: What the Device Is Doing Right Now

Based on the code analysis, the test device running v1.9.30 (compiled locally without `-DTANKALARM_DFU_MCUBOOT`) is doing the following on each DFU check cycle:

```
1. checkForFirmwareUpdate()
   → dfu.status returns mode="ready", version="1.9.31"
   → Passes downgrade guard (1.9.31 > 1.9.30)
   → Passes blacklist check (skipped — TANKALARM_DFU_MCUBOOT not defined)
   → Sets gDfuUpdateAvailable = true

2. enableDfuMode()
   → gDfuInProgress = true
   → Hits #else branch: bool ok = false
   → Stops the pending update on the Notecard
   → gDfuInProgress = false

3. Result: Update cleared from Notecard, device stays on v1.9.30
```

**After the first failed attempt, the Notecard DFU state is cleared.** Subsequent DFU checks will see `mode="idle"` — no update available. The update must be re-applied from Notehub for each new attempt.

---

## 10. Fix — Step-by-Step

### Option A: Full OTA-Capable USB Flash (Recommended)

This is the one-time USB setup that makes the test device fully OTA-capable:

1. **Flash the MCUboot bootloader** (if not already installed):
   - Compile the client with `--fqbn arduino:mbed_opta:opta:security=sien` and `-DTANKALARM_DFU_MCUBOOT`
   - Upload the `*.with_bootloader.bin` variant via USB — this includes the MCUboot bootloader alongside the application
   - Alternatively, flash the MCUboot bootloader separately per `MCUBOOT_BOOTLOADER_OPTIONS.md`

2. **Provision QSPI** (if not already done):
   - Run `TankAlarm-112025-KeyProvisioning` over USB
   - This creates MBR partition 2 with pre-allocated `update.bin` (0x1E0000) and `scratch.bin` (0x20000)

3. **Flash v1.9.30 (or v1.9.31) with OTA support**:
   ```bash
   arduino-cli compile \
     --fqbn arduino:mbed_opta:opta:security=sien \
     --build-property "build.extra_flags=-DTANKALARM_DFU_MCUBOOT" \
     --libraries "<LIB_ROOT>" \
     TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino

   arduino-cli upload \
     --fqbn arduino:mbed_opta:opta:security=sien \
     --port <PORT> \
     TankAlarm-112025-Client-BluesOpta
   ```

4. **Re-apply the OTA update from Notehub**:
   - Upload `TankAlarm-Client-secure-v1.9.31.slot.bin` (or later) to Notehub
   - Apply it to the test device

5. **Verify via serial** (115200 baud):
   - Look for `"MCUboot DFU: STAGED * TRIGGERING SWAP"` followed by a reboot
   - After reboot: firmware version should report v1.9.31
   - Look for `"OTA self-check: confirmed"` in the boot logs

### Option B: Quick CI Tag for v1.9.30 (Alternative)

If the goal is just to get v1.9.30 running with OTA support without changing the version:

1. Tag the v1.9.30 commit: `git tag v1.9.30 <commit-sha>`
2. Push the tag: `git push origin v1.9.30`
3. Wait for CI to produce the release artifacts
4. USB-flash the `TankAlarm-Client-secure-v1.9.30.slot.bin` (which includes MCUboot compatibility)
5. Re-apply the v1.9.31 OTA from Notehub

**Note:** This still requires MCUboot bootloader + QSPI provisioning on the device.

---

## 11. Preventive Recommendations

### 11.1 Build Flag Safety Net

Add a compile-time warning or runtime diagnostic when `TANKALARM_DFU_MCUBOOT` is not defined:

```cpp
#if !defined(TANKALARM_DFU_MCUBOOT)
  #warning "TANKALARM_DFU_MCUBOOT is NOT defined — MCUboot OTA updates are DISABLED in this build"
#endif
```

And in the serial boot banner:

```cpp
#if defined(TANKALARM_DFU_MCUBOOT)
  Serial.println(F("  OTA: MCUboot ENABLED"));
#else
  Serial.println(F("  OTA: MCUboot DISABLED (no -DTANKALARM_DFU_MCUBOOT)"));
#endif
```

This makes it immediately obvious from serial output whether a binary supports OTA.

### 11.2 Tag Every Version That Goes to Hardware

Establish a rule: **never flash a version to any device without a git tag.** If a local build is needed for testing, create a pre-release tag (e.g., `v1.9.30-rc1`) so that:
- The CI produces artifacts with the correct build flags
- There is a record of what was deployed
- The build is reproducible

### 11.3 Document the OTA Prerequisites Checklist

Add a diagnostic checklist to the client boot sequence (or to a dedicated diagnostic command):

```
OTA Readiness:
  [ ] MCUboot compiled in: YES/NO
  [ ] MCUboot bootloader: INSTALLED/STOCK
  [ ] QSPI partition 2: MOUNTED/MISSING
  [ ] update.bin: EXISTS/MISSING (size: xxx)
  [ ] MCUboot keys: DEFAULT/CUSTOM
```

The existing `tankalarm_otaSelfCheck()` function prints some of this — extending it to cover all prerequisites would make field diagnosis trivial.

### 11.4 Version Reporting Enhancement

Add the build flags to the telemetry version string:

```cpp
#if defined(TANKALARM_DFU_MCUBOOT)
  #define FIRMWARE_BUILD_TYPE "mcuboot"
#else
  #define FIRMWARE_BUILD_TYPE "standard"
#endif
```

Include this in `_health.qo` or `_session.qo` so the dashboard shows whether a device's firmware supports OTA without requiring USB access.

---

## 12. Summary of Findings

| Finding | Severity | Status |
|---------|----------|--------|
| v1.9.30 compiled without `-DTANKALARM_DFU_MCUBOOT` (MCUboot staging compiled out) | **CRITICAL** | Root cause — confirmed by missing git tag |
| v1.9.30 compiled without `security=sien` (no MCUboot-compatible binary layout) | **CRITICAL** | Compounding factor — confirmed by missing git tag |
| MCUboot bootloader may not be installed on test device | **HIGH** | Prerequisite — unverified on field unit |
| QSPI may not be provisioned on test device | **HIGH** | Prerequisite — unverified on field unit |
| Key chain alignment uncertain (default vs custom) | **MEDIUM** | CI now uses default Arduino keys; device provisioning state unknown |
| OTA code fixes (47d326e) are correct and well-implemented | **INFO** | Verified — all fixes are sound |
| CI workflow `security=sien` change is correct | **INFO** | Verified — produces bootloader-compatible slot images |

**Bottom line:** The firmware running on the device cannot perform OTA updates because the OTA code path is compiled out. Fix requires a one-time USB visit to flash a properly-compiled binary (with the correct FQBN and build flags), verify MCUboot bootloader installation, and confirm QSPI provisioning.

---

## 13. AI Findings & Deep-Dive Validation

In this section, we review the code fixes implemented in commit `47d326e` and analyze why the OTA update from v1.9.30 to v1.9.31 failed, including other sequencing/logic gaps.

### 13.1 Review of Implemented Code Fixes (Commit 47d326e)

The code changes introduced in commit `47d326e` are highly robust, correct, and address key vulnerabilities in the client on-device check logic described in [CODE REVIEW/CODE_REVIEW_06162026_OTA_SILENT_CHECK_FAILURE.md](CODE%20REVIEW/CODE_REVIEW_06162026_OTA_SILENT_CHECK_FAILURE.md):
1. **DFU Timer Check (Finding 2):** Rescheduling the check timer (`gLastDfuCheckMillis = now;`) to only advance when a check actually runs (and back-dating it on deferred ticks to retry in ~1 min instead of waiting a full hour) ensures a single transient I/O exception does not stall updates for hours.
2. **Observability (Finding 1):** Detailed logging added to [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino) on skips, I/O errors, or status conditions ensures developers can diagnose updating behavior instantly.
3. **Loop-Trap Mitigation (Findings 8, 13, 17):** Preflight size/role failures now route through `mcuboot_restore_hub`, stopping the update and setting the DFU state to `stop` on the Notecard. This, combined with the new `stopRefusedFirmware()` and clearing update-available flags on staging failure, stops infinite retry loops on incompatible or blacklisted builds.
4. **Buffer Size Safety (Finding 18):** Adding size bounds (`%31[^\"]` and `%15[^\"]`) in sscanf calls inside [TankAlarm-112025-Common/src/TankAlarm_DFU.h](TankAlarm-112025-Common/src/TankAlarm_DFU.h#L793) prevents memory corruption and bootloops from malformed or corrupted QSPI files.
5. **Notecard Wireless Error Path (Finding 10):** Restoring the online state (`gNotecardAvailable = true`) on wireless-error responses where I/O remains healthy prevents temporary cellular drops from permanently disabling the update check.

### 13.2 Crucial Gaps & Structural Sequencing Failures Found

Through our analysis, we have located two hidden logic/sequencing issues that have caused or can cause the OTA system to completely fail under production-like conditions:

#### Finding A: The False Rollback & Blacklist Trap (High Severity)
If a test device running v1.9.30 (compiled with `-DTANKALARM_DFU_MCUBOOT`) does *not* have the proper MCUboot bootloader installed on its internal flash (e.g. it still carries the standard Arduino bootloader), or if there is an image signature/key-mismatch, the following failure sequence occurs silently:
1. The device successfully downloads the v1.9.31 binary, writes it to the secondary slot on QSPI `/fs_ota/update.bin`, writes a `pending_ota.json` file marking state as `'trial'`, triggers `MCUboot::applyUpdate(false)`, and reboots the motherboard.
2. Because the bootloader is standard/mismatched, it completely ignores the swap request and boots Slot 0 (which is the current v1.9.30) again.
3. Upon booting back up, v1.9.30's `setup()` calls the pending-OTA resolver `tankalarm_resolvePendingOta()` in [TankAlarm-112025-Common/src/TankAlarm_DFU.h](TankAlarm-112025-Common/src/TankAlarm_DFU.h#L765).
4. Since the running version is sequence `220` (v1.9.30) but the trial marker specifies target sequence `221` (v1.9.31), the device concludes that a **rollback has occurred** (trial image crashed).
5. It triggers a Notecard `dfu.status` stop request, reporting `rollback detected - trial crashed - reverted to 1.9.30` to Notehub, and updates the local state in `pending_ota.json` to `"status":"failed_rollback"`.
6. From this moment on, **v1.9.31 is blacklisted locally on the device.** The firmware will ignore and stop any future update requests for v1.9.31, preventing subsequent updates even if the operator corrects the bootloader or keys, until `/fs_ota/pending_ota.json` is physically deleted from the QSPI flash.

#### Finding B: Downgrade Check Null-Result on Leading 'v' Prefix (High Severity)
The downgrade guard compares the offered version against the running version via `tankalarm_versionToSeq()` in [TankAlarm-112025-Common/src/TankAlarm_DFU.h](TankAlarm-112025-Common/src/TankAlarm_DFU.h#L758).
```cpp
static inline uint32_t tankalarm_versionToSeq(const char *verStr) {
  if (!verStr || verStr[0] == '\0') return 0;
  int major = 0, minor = 0, patch = 0;
  sscanf(verStr, "%d.%d.%d", &major, &minor, &patch);
  return major * 100 + minor * 10 + patch;
}
```
If the version assigned to the update on Notehub starts with a leading `'v'` (e.g. `"v1.9.31"` because of typical Git tag conventions) instead of `"1.9.31"`, then:
- sscanf encounters `'v'` while looking for an integer, fails parsing, and returns `0` with all variables unparsed.
- `tankalarm_versionToSeq("v1.9.31")` returns `0`.
- The downgrade block in [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4115) evaluates:
  `if (0 <= (uint32_t)FIRMWARE_BUILD_SEQ)` which is always **true**.
- **Result:** The update is immediately and silently aborted as an obsolete downgrade and stopped on the Notecard: `DFU: Offered v1.9.31 is not newer than running v1.9.30 - ignoring (no downgrade), stopped.`

---

## 14. Actionable Implementation Suggestions & Code Fixes

### 14.1 Strip Leading 'v'/'V' inside Version Parsing
Overcoming Finding B requires supporting both pure semantic strings (`1.9.31`) and prefixed strings (`v1.9.31`). We recommend a robust, lightweight pointer shift:

```cpp
static inline uint32_t tankalarm_versionToSeq(const char *verStr) {
  if (!verStr || verStr[0] == '\0') return 0;
  const char *p = verStr;
  if (*p == 'v' || *p == 'V') {
    p++;
  }
  int major = 0, minor = 0, patch = 0;
  sscanf(p, "%d.%d.%d", &major, &minor, &patch);
  return major * 100 + minor * 10 + patch;
}
```

### 14.2 Compile-Time Safety Guard for Local Builds
To ensure developers do not accidentally compile local binaries and flash them over USB without OTA features compiled-in, add a preprocessor safety check in [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino):

```cpp
#if !defined(TANKALARM_DFU_MCUBOOT)
  #warning "TANKALARM_DFU_MCUBOOT is NOT defined — MCUboot OTA updates are DISABLED in this build!"
#endif
```

And in the serial boot banner layout:

```cpp
#if defined(TANKALARM_DFU_MCUBOOT)
  Serial.println(F("  OTA: MCUboot ENABLED"));
#else
  Serial.println(F("  OTA: MCUboot DISABLED (no -DTANKALARM_DFU_MCUBOOT)"));
#endif
```

### 14.3 Decoupling Blacklist state on Explicit New Offered Updates
To prevent the client from becoming permanently locked out of a blacklisted version if the rollback was a "false positive" (e.g., standard bootloader was on the board at testing time, and has now been updated to MCUboot), the downgrade check could purge the blacklist state whenever a genuinely newer version (or different version) is successfully received, or we can provide a recovery CLI tool / Notefile reset message that clears `/fs_ota/pending_ota.json` on the board.

---

## 15. References

- [External - Blues IAP Firmware Update Guide](https://dev.blues.io/notehub/host-firmware-updates/iap-firmware-update/)
- [External - Blues Notecard API Requests for DFU](https://dev.blues.io/notehub/host-firmware-updates/notecard-api-requests-for-dfu/)
- [External - Arduino Opta User Manual](https://docs.arduino.cc/tutorials/opta/user-manual/)
- Release Workflow: [.github/workflows/release-firmware-112025.yml](.github/workflows/release-firmware-112025.yml)
- DFU Implementation: [TankAlarm-112025-Common/src/TankAlarm_DFU.h](TankAlarm-112025-Common/src/TankAlarm_DFU.h)
- Client Firmware: [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino)
- Prior OTA Analysis: [CODE REVIEW/CODE_REVIEW_06152026_OTA_VS_USB_UPDATE_FAILURE.md](CODE%20REVIEW/CODE_REVIEW_06152026_OTA_VS_USB_UPDATE_FAILURE.md)
- Silent Check Analysis: [CODE REVIEW/CODE_REVIEW_06162026_OTA_SILENT_CHECK_FAILURE.md](CODE%20REVIEW/CODE_REVIEW_06162026_OTA_SILENT_CHECK_FAILURE.md)

---

## 16. Second-Pass Validation (Current Repo State)

This section is an independent re-validation against the **current repository source** and Blues documentation, focused on confirming the implemented fixes and identifying any additional sequencing/process gaps.

### 16.1 Re-Validation of Implemented OTA Code Fixes

The previously implemented client OTA fixes are present and correctly wired in source:

1. **`gDfuInProgress` latch bug fix is correctly in place**
   - `checkForFirmwareUpdate()` no longer sets `gDfuInProgress` in `downloading` mode.
   - This prevents the permanent "no more DFU checks" deadlock.

2. **Refused update suppression (`stopRefusedFirmware`) is implemented correctly**
   - Downgrade/equal-version and blacklist cases explicitly issue `dfu.status {stop:true,name:"user"}`.
   - This prevents repeatedly evaluating the same refused image.

3. **Failure cleanup path (`mcuboot_restore_hub`) is robust**
   - On staging failure, code closes files, unmounts QSPI FS, sends DFU stop status, and restores hub mode.
   - This avoids getting stranded in DFU mode or leaving stale host state.

4. **Rollback/blacklist persistence logic is present and functional**
   - `pending_ota.json` stores `trial`, `confirmed`, and `failed_rollback` states.
   - Client-side blacklist check and stop behavior are active for MCUboot builds.

**Conclusion:** the OTA fix set itself is technically solid and not the likely reason your specific v1.9.30 -> v1.9.31 attempt remained on v1.9.30.

### 16.2 Additional Findings (New)

#### Finding 16-A: Release Pipeline Drift from This Document's Assumptions (HIGH)

This review document states that CI uses:
- `--fqbn arduino:mbed_opta:opta:security=sien`, and
- Arduino-core default MCUboot signing path.

However, the current `.github/workflows/release-firmware-112025.yml` in this repo currently builds the client with:

```bash
arduino-cli compile --fqbn arduino:mbed_opta:opta \
  --build-property "build.extra_flags=-DTANKALARM_DFU_MCUBOOT" ...
```

and then performs **manual imgtool signing/encryption** using:

```bash
imgtool sign \
  --key mcuboot_keys/ecdsa-p256-signing-priv-key.pem \
  --encrypt mcuboot_keys/ecdsa-p256-encrypt-pub-key.pem ...
```

This is not inherently wrong, but it is a **process drift** from the `security=sien` assumption throughout Sections 1/3/5/6/10/12 above. It can create operator confusion and mis-diagnosis during field failures.

#### Finding 16-B: Leading `v` Version Parse Bug Is Still Live (HIGH)

`tankalarm_versionToSeq()` still parses only `%d.%d.%d` with no normalization and no parse-count validation:

```cpp
static inline uint32_t tankalarm_versionToSeq(const char *verStr) {
  if (!verStr || verStr[0] == '\0') return 0;
  int major = 0, minor = 0, patch = 0;
  sscanf(verStr, "%d.%d.%d", &major, &minor, &patch);
  return major * 100 + minor * 10 + patch;
}
```

So `"v1.9.31"` still evaluates to `0`, and the downgrade guard rejects it as "not newer." This can silently block OTA even when everything else is correct.

#### Finding 16-C: Rollback Status Stop in `tankalarm_resolvePendingOta()` Omits `name:"user"` (MEDIUM)

In the rollback-detected branch, `dfu.status` stop is issued without explicitly setting `name:"user"`.

Most other DFU stop calls in the code explicitly set `name:"user"`. For consistency and unambiguous Notecard behavior, this rollback-stop should also include `name:"user"`.

#### Finding 16-D: Blacklist Is Sticky with No In-Firmware Recovery Path (MEDIUM)

Once `pending_ota.json` is marked `failed_rollback`, the same target version remains blacklisted until the file is manually cleared/overwritten. This is safe for loops, but operationally brittle when rollback was due to temporary provisioning mismatch and later corrected.

### 16.3 Updated Root-Cause Position for Your Specific Incident

For the exact scenario described (USB-installed v1.9.30 local build, then OTA to v1.9.31):

1. **Most likely root cause remains unchanged:** v1.9.30 on the device likely lacked `-DTANKALARM_DFU_MCUBOOT`, so staging code was compiled out and update was stopped locally.
2. **Compounding risk remains:** MCUboot bootloader / QSPI provisioning / key alignment must all still be correct.
3. **Additional blocker that can independently reproduce this symptom:** if Notehub firmware version is prefixed (`v1.9.31`), downgrade guard rejects the update even on a fully provisioned board.

### 16.4 Recommended Fixes (Code + Process)

#### 16.4.1 Harden version parsing (implement now)

```cpp
static inline uint32_t tankalarm_versionToSeq(const char *verStr) {
  if (!verStr || verStr[0] == '\0') return 0;

  const char *p = verStr;
  if (*p == 'v' || *p == 'V') {
    ++p;
  }

  int major = 0, minor = 0, patch = 0;
  const int n = sscanf(p, "%d.%d.%d", &major, &minor, &patch);
  if (n != 3 || major < 0 || minor < 0 || patch < 0) {
    return 0;
  }

  return (uint32_t)(major * 100 + minor * 10 + patch);
}
```

#### 16.4.2 Make rollback stop explicit (`name:"user"`)

In rollback handling inside `tankalarm_resolvePendingOta()`:

```cpp
J *req = notecard.newRequest("dfu.status");
if (req) {
  JAddBoolToObject(req, "stop", true);
  JAddStringToObject(req, "name", "user");
  JAddStringToObject(req, "status", "rollback detected - trial crashed - reverted");
  J *rsp = notecard.requestAndResponse(req);
  if (rsp) notecard.deleteResponse(rsp);
}
```

#### 16.4.3 Add explicit blacklist recovery command/path

Provide one controlled mechanism (serial command, maintenance sketch, or protected API) that can clear `pending_ota.json` when operators intentionally want to retry a previously blacklisted target after provisioning/key fixes.

#### 16.4.4 Align and lock release strategy (choose one, document it, enforce it)

Pick exactly one release model and make docs/workflow consistent:

- **Model A:** `security=sien` Arduino-core signing path.
- **Model B:** manual `imgtool` signing path with `mcuboot_keys/`.

Then add CI assertions to prevent drift. Example check:

```bash
# Example: fail build if client compile or signing mode is not what this repo expects
grep -q -- '--build-property "build.extra_flags=-DTANKALARM_DFU_MCUBOOT"' .github/workflows/release-firmware-112025.yml
```

#### 16.4.5 Improve local build visibility

Keep/add compile-time and boot-banner diagnostics so operators can instantly see OTA capability from serial output:

```cpp
#if !defined(TANKALARM_DFU_MCUBOOT)
  #warning "TANKALARM_DFU_MCUBOOT is NOT defined - MCUboot OTA is disabled"
#endif
```

and:

```cpp
#if defined(TANKALARM_DFU_MCUBOOT)
  Serial.println(F("OTA: MCUboot ENABLED"));
#else
  Serial.println(F("OTA: MCUboot DISABLED"));
#endif
```

### 16.5 Practical Next Test Sequence (Fastest Confirmation Path)

1. USB flash a known-good client build that **definitely** includes `-DTANKALARM_DFU_MCUBOOT`.
2. Confirm boot logs show OTA enabled + OTA self-check pass (partition/files).
3. Ensure Notehub host firmware version string is plain `1.9.31` (not `v1.9.31`) until parser hardening is merged.
4. Re-apply OTA and capture serial logs around:
   - `FIRMWARE UPDATE AVAILABLE`
   - `MCUboot DFU: STAGED * TRIGGERING SWAP`
   - post-reboot version / confirmation lines.

If this still does not update, the remaining suspects are bootloader/key/provisioning mismatch on the physical board, not the staged OTA logic.

---

## 17. Final Review Verdict, Action Plan & Todo List

This section is the consolidated outcome of re-validating **every** prior section of this
document against the **current repository source** (`TankAlarm_DFU.h`, the Client `.ino`,
`TankAlarm_Common.h`, and `.github/workflows/release-firmware-112025.yml`). It answers the two
questions posed — *were the fixes implemented correctly?* and *are the appended reviews correct?* —
and then lays out the exact bump → push → USB → bump → push → Notehub → OTA test workflow.

### 17.1 Were the Commit `47d326e` Fixes Implemented Correctly? — YES ✓

All six fixes are present and correct in the current source. They are **not** the reason the device
stayed on v1.9.30; they are sound.

| Fix | Location (verified) | Verdict |
|-----|--------------------|---------|
| `gDfuInProgress` latch no longer set on Notecard `downloading` | [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4083) | ✓ Correct |
| `stopRefusedFirmware()` issues `dfu.status {stop,name:"user"}`, deduped by version | [...Client...ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4043) | ✓ Correct |
| Downgrade guard (`versionToSeq(offered) <= FIRMWARE_BUILD_SEQ` → refuse) | [...Client...ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4117) | ✓ Correct (but depends on the parser — see 17.2) |
| Blacklist check before apply | [...Client...ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4104) | ✓ Correct |
| `sscanf` field-width bounds (`%31[^"]`) in OTA state parsing | [TankAlarm-112025-Common/src/TankAlarm_DFU.h](TankAlarm-112025-Common/src/TankAlarm_DFU.h#L797) | ✓ Correct |
| `goto mcuboot_restore_hub` cleanup on staging failure | [TankAlarm-112025-Common/src/TankAlarm_DFU.h](TankAlarm-112025-Common/src/TankAlarm_DFU.h) | ✓ Correct |

**Conclusion:** the implemented fix set is technically solid. The failure lies elsewhere — in
residual parser/process gaps that the later review passes correctly identified but which remain
**unfixed in source.**

### 17.2 Are the Appended Reviews (§13–§16) Correct? — Re-validated Against Source

| Finding | Severity | Confirmed in current source? | Notes |
|---------|----------|------------------------------|-------|
| **16-B / B** — `tankalarm_versionToSeq()` does not strip leading `v` and does not validate parse count → `"v1.9.31"` returns `0` | **HIGH** | ✅ **LIVE** — [TankAlarm_DFU.h#L758](TankAlarm-112025-Common/src/TankAlarm_DFU.h#L758) still `sscanf("%d.%d.%d")` only | Single most likely **code-level** cause of the exact symptom (silent stay-on-1.9.30) **if** v1.9.30 carried the MCUboot flag. Must fix. |
| **16-A** — release pipeline drift: doc says CI uses `security=sien` + Arduino default keys | **HIGH (doc accuracy)** | ✅ Confirmed — CI uses `--fqbn arduino:mbed_opta:opta` + `-DTANKALARM_DFU_MCUBOOT` + **manual `imgtool` with custom `mcuboot_keys/`** | §1/3/5/6/10/12 and the key table in §5.3 are **inverted**: the device must be provisioned with the **custom** keys, and its MCUboot bootloader must trust them. |
| **16-C** — rollback `dfu.status` stop omits `name:"user"` | **MEDIUM** | ✅ Confirmed — [TankAlarm_DFU.h#L828](TankAlarm-112025-Common/src/TankAlarm_DFU.h#L828) | Every other stop sets `name:"user"`; this one does not. |
| **A / 16-D** — `failed_rollback` blacklist is sticky; USB flash does not clear it (QSPI p2 untouched) | **HIGH (operational)** | ✅ Confirmed — [TankAlarm_DFU.h#L839](TankAlarm-112025-Common/src/TankAlarm_DFU.h#L839) | **Critical for the test plan:** the OTA validation must target a *fresh* version number never attempted on the device, or a prior false-rollback of v1.9.31 will keep refusing it. |
| **18** — `sscanf %[^"]` without width | MEDIUM | ✅ Already addressed (`%31[^"]`) | No action needed. |
| **NEW (this pass)** — `library.properties` version is **stale** (`1.9.27`) vs `TankAlarm_Common.h` (`1.9.31`) | LOW | ✅ Confirmed — [TankAlarm-112025-Common/library.properties](TankAlarm-112025-Common/library.properties#L2) | CI validates the tag against `TankAlarm_Common.h` only, so it won't block release, but keep both in sync at each bump. |

### 17.3 Most-Likely Root Cause(s) for *This* Incident (ranked)

1. **Leading-`v` parser bug (16-B)** — if Notehub presented the image version as `v1.9.31`,
   `versionToSeq` returned `0`, the downgrade guard saw `0 <= 221` and **silently refused + stopped**
   the update. Clean to fix; highest-value change.
2. **v1.9.30 compiled without `-DTANKALARM_DFU_MCUBOOT`** (no git tag → not CI-built) — the MCUboot
   staging path was compiled out, so `enableDfuMode()` hit the `#else` and stopped the update.
   Mitigated by USB-flashing a **CI-built** binary (Phase 1 below).
3. **False-rollback blacklist trap (A / 16-D)** — if an earlier OTA of v1.9.31 staged, rebooted, and
   the bootloader ignored the swap (stock bootloader or key mismatch), the device blacklisted v1.9.31
   locally and refuses it every cycle. Avoided by testing with a **fresh** version (Phase 2).
4. **Compounding prerequisites** (unverifiable remotely): MCUboot bootloader installed, QSPI
   provisioned, and **custom-key alignment** (per the corrected 16-A understanding).

### 17.4 Code Fixes to Implement Before the Version Bump

> These ship in **Bump #1** so the USB-flashed CI binary is genuinely OTA-correct.

**Fix 1 — Harden `tankalarm_versionToSeq()`** (`TankAlarm_DFU.h`, ~L758). Keep the existing weighting
(`major*100 + minor*10 + patch`) — it is load-bearing for the trial/rollback exact-match — and only
add `v`-stripping plus parse validation:

```cpp
static inline uint32_t tankalarm_versionToSeq(const char *verStr) {
  if (!verStr || verStr[0] == '\0') return 0;
  const char *p = verStr;
  if (*p == 'v' || *p == 'V') p++;            // tolerate "v1.9.33" from Git/Notehub
  int major = 0, minor = 0, patch = 0;
  const int n = sscanf(p, "%d.%d.%d", &major, &minor, &patch);
  if (n != 3 || major < 0 || minor < 0 || patch < 0) return 0;  // malformed → refuse (safe)
  return (uint32_t)(major * 100 + minor * 10 + patch);
}
```

**Fix 2 — Add `name:"user"` to the rollback stop** (`TankAlarm_DFU.h`, rollback branch of
`tankalarm_resolvePendingOta()`, ~L828):

```cpp
J *req = notecard.newRequest("dfu.status");
if (req) {
  JAddBoolToObject(req, "stop", true);
  JAddStringToObject(req, "name", "user");   // ADD: target the host DFU channel explicitly
  char err_buf[128];
  snprintf(err_buf, sizeof(err_buf), "rollback detected - trial crashed - reverted to %s", FIRMWARE_VERSION);
  JAddStringToObject(req, "status", err_buf);
  J *rsp = notecard.requestAndResponse(req);
  if (rsp) notecard.deleteResponse(rsp);
}
```

**Fix 3 — Build-capability visibility** (Client `.ino`). A compile-time warning near the version
includes, plus a boot-banner line in `setup()` so serial instantly reveals OTA capability:

```cpp
#if !defined(TANKALARM_DFU_MCUBOOT)
  #warning "TANKALARM_DFU_MCUBOOT is NOT defined - MCUboot OTA updates are DISABLED in this build"
#endif
```
```cpp
#if defined(TANKALARM_DFU_MCUBOOT)
  Serial.println(F("  OTA: MCUboot ENABLED"));
#else
  Serial.println(F("  OTA: MCUboot DISABLED (no -DTANKALARM_DFU_MCUBOOT)"));
#endif
```

**Fix 4 (recommended, optional for the immediate test) — Blacklist recovery path.** Add a controlled
way to clear `/fs_ota/pending_ota.json` (e.g. a pushed-config flag `clearOtaBlacklist` or a serial
maintenance command) so a *previously* false-rolled-back target can be retried after the
bootloader/keys are corrected — without a USB visit. Not on the critical path because **Bump #2 uses
a fresh, never-blacklisted version**, but it removes the trap permanently.

**Fix 5 (housekeeping)** — bump `library.properties` `version=` in lockstep with
`TankAlarm_Common.h` at every version change.

### 17.5 Two-Phase Deployment & Test Plan

Current source is at **v1.9.31 / build seq 221**. The two bumps are therefore:

#### Phase 1 — Establish a known-good, OTA-capable baseline via **USB** (Bump #1 = **v1.9.32 / seq 222**)

Purpose: eliminate "missing flag / wrong binary" by flashing a **CI-built** image, and prove the
device is actually OTA-capable *before* trusting an OTA.

1. Apply Fixes 1–3 (and 4 if desired) above.
2. Bump `FIRMWARE_VERSION` `1.9.31 → 1.9.32`, `FIRMWARE_BUILD_SEQ` `221 → 222`, and
   `library.properties` `version` → `1.9.32`.
3. Compile Client (with `-DTANKALARM_DFU_MCUBOOT`), Server, and Viewer locally to confirm a clean build.
4. Commit → push to `master` → tag `v1.9.32` → push tag → CI builds & publishes the GitHub Release.
5. Download **`TankAlarm-Client-v1.9.32.bin`** (the raw app `.bin`) from the Release.
6. USB-flash it to the test Client Opta (`arduino-cli upload -p <PORT> --fqbn arduino:mbed_opta:opta ...`).
7. **Verification gate (serial @ 115200):** confirm `Firmware Version: 1.9.32`, the new
   **`OTA: MCUboot ENABLED`** banner, and the OTA self-check reporting QSPI **`OTA partition: READY`**
   (`update.bin` / `scratch.bin` sizes OK). If it shows `DISABLED` or `NOT READY`, **stop** and fix
   the bootloader/QSPI provisioning before Phase 2.

#### Phase 2 — Validate OTA end-to-end via **Notehub** (Bump #2 = **v1.9.33 / seq 223**)

Purpose: a *fresh* target that no downgrade guard or stale blacklist can refuse.

8. Bump `FIRMWARE_VERSION` `1.9.32 → 1.9.33`, `FIRMWARE_BUILD_SEQ` `222 → 223`, `library.properties` → `1.9.33` (a trivial bump is fine — its only job is to be a strictly-newer, never-attempted version).
9. Commit → push → tag `v1.9.33` → push tag → CI builds & publishes the Release.
10. Download **`TankAlarm-Client-secure-v1.9.33.slot.bin`** (the signed slot image — the OTA artifact).
11. Upload the `.slot.bin` to Notehub and assign it as **Host Firmware** to the test Client device.
12. Ensure the Notehub firmware **version string is plain `1.9.33`** (not `v1.9.33`) — belt-and-suspenders even with the hardened parser.
13. Trigger / await the OTA; capture serial around: `FIRMWARE UPDATE AVAILABLE: v1.9.33` →
    `MCUboot DFU: ... STAGED ... TRIGGERING SWAP` → reboot →
    `MCUboot: Trial boot of version 1.9.33 succeeded` → `OTA applied reported to server`.
14. Confirm `telemetry.qi` now reports `fv = 1.9.33`. ✅ OTA validated.

> **If Phase 2 still fails,** the Fix-3 diagnostics make the cause self-evident from serial: MCUboot
> DISABLED, QSPI NOT READY, "not newer" (parser), "blacklisted", or a staging/swap failure — each
> points to a specific remaining prerequisite (bootloader / keys / provisioning), not the staged OTA
> logic.

### 17.6 Todo List

**Code fixes (ship in Bump #1):**
- [ ] Fix 1: harden `tankalarm_versionToSeq()` (strip `v`/`V`, validate parse count) — `TankAlarm_DFU.h`
- [ ] Fix 2: add `name:"user"` to the rollback `dfu.status` stop — `TankAlarm_DFU.h`
- [ ] Fix 3: add `#warning` + boot-banner `OTA: MCUboot ENABLED/DISABLED` — Client `.ino`
- [ ] Fix 4 (optional): blacklist-recovery mechanism (clear `pending_ota.json`)
- [ ] Fix 5: sync `library.properties` `version` with `TankAlarm_Common.h`

**Phase 1 — USB baseline (v1.9.32 / seq 222):**
- [ ] Bump `FIRMWARE_VERSION`→1.9.32, `FIRMWARE_BUILD_SEQ`→222, `library.properties`→1.9.32
- [ ] Local compile of Client (+`-DTANKALARM_DFU_MCUBOOT`), Server, Viewer — clean
- [ ] Commit, push `master`, tag `v1.9.32`, push tag; wait for CI Release
- [ ] Download `TankAlarm-Client-v1.9.32.bin`; USB-flash the test Client
- [ ] Verify serial: `1.9.32`, `OTA: MCUboot ENABLED`, `OTA partition: READY` (gate to Phase 2)

**Phase 2 — OTA validation (v1.9.33 / seq 223):**
- [ ] Bump `FIRMWARE_VERSION`→1.9.33, `FIRMWARE_BUILD_SEQ`→223, `library.properties`→1.9.33
- [ ] Commit, push `master`, tag `v1.9.33`, push tag; wait for CI Release
- [ ] Download `TankAlarm-Client-secure-v1.9.33.slot.bin`; upload to Notehub; assign to device
- [ ] Confirm Notehub version string is plain `1.9.33` (no `v`)
- [ ] Dispatch OTA; capture serial (STAGED → swap → trial succeeded → applied)
- [ ] Confirm `telemetry.qi` reports `fv = 1.9.33` ✅

**Process / accuracy follow-ups:**
- [ ] Reconcile §1/3/5/6/10/12 with the **actual** signing model (manual `imgtool` + custom `mcuboot_keys/`, not `security=sien`); or pick one signing model and add a CI assertion to prevent drift (16-A / §16.4.4)
- [ ] (Field unit) verify MCUboot bootloader installed + QSPI provisioned with the matching custom keys
