# MCUboot Hardware Provisioning and Bench Testing Guide

> 🛑 **BLOCKER (2026-06-10): This runbook is paused at Step 3.** Bench provisioning of the first
> Client unit revealed that KeyProvisioning does **not** create the QSPI MBR partition (it only
> reformats an existing one), and — more importantly — the Client/Server application reformats the
> **entire** QSPI as a whole-device LittleFS, which is incompatible with MCUboot's requirement that
> the update image live in **MBR partition 2**. Booting the Client/Server app would destroy the OTA
> partition. **Do not proceed past Step 3** until the storage architecture is resolved. Full
> analysis: [MCUBOOT_QSPI_STORAGE_CONFLICT_06102026.md](MCUBOOT_QSPI_STORAGE_CONFLICT_06102026.md).
> Bootloader options/verification: [MCUBOOT_BOOTLOADER_OPTIONS.md](MCUBOOT_BOOTLOADER_OPTIONS.md).

This document outlines the step-by-step procedure for provisioning a brand-new or existing hardware unit with MCUboot, loading security keys, preparing the QSPI storage structures, and executing the validation testing matrix.

For architectural logic and design constraints, consult [CODE REVIEW/CODE_REVIEW_06092026_UPDATE_SYSTEM_v1.9.0_PROPOSED_FIXES.md](CODE%20REVIEW/CODE_REVIEW_06092026_UPDATE_SYSTEM_v1.9.0_PROPOSED_FIXES.md).

---

## 1. One-Time Hardware Provisioning Runbook

Every Arduino Opta device must be prepared **once via USB** before it is capable of receiving OTA updates via MCUboot. This step is a prerequisite prior to deployment in the field.

- [ ] **Step 1: Flash MCUboot-capable bootloader**
  - Upload the native Arduino sketch `STM32H747_manageBootloader` to the M7 core of the board.
  - Follow the prompt in the serial monitor to update the bootloader.
  - *Verify:* Ensure the installed bootloader version is greater than 24, and the board reports its identifier as "MCUboot Arduino".

- [ ] **Step 2: Upload Key Provisioning Sketch**
  - Compile and flash [TankAlarm-112025-KeyProvisioning/TankAlarm-112025-KeyProvisioning.ino](TankAlarm-112025-KeyProvisioning/TankAlarm-112025-KeyProvisioning.ino) to the block device.
  - Note: This sketch includes standard default signature/encryption headers. The default keys match `mcuboot_keys/` and are used to fulfill the bootloader's mechanical requirements.

- [ ] **Step 3: Run Keys & Storage Formatting**
  - Connect a serial monitor at `115200` baud rate to the provisioned board.
  - When prompted `Do you want to proceed and load the default keys? Y/[n]`, press **Y**.
  - The tool will:
    - Attempt to mount the QSPI partition `fs_ota`. If `/fs_ota/update.bin` and `/fs_ota/scratch.bin` are already intact, it will skip re-formatting to prevent write cycles wear.
    - ⚠️ **CORRECTION (verified 2026-06-10):** the current sketch does **NOT** create the MBR partition table. It constructs `MBRBlockDevice(&root, 2)` and calls `reformat()` on an *already-existing* partition 2; if the chip has no MBR (e.g. a unit previously running the whole-device-LittleFS app), this **fails** with `Error creating MCUboot FAT partition! Please partition QSPI manually first.` and `update.bin`/`scratch.bin` are never created. A full MBR must be created first (core `QSPIFormat` sketch, or a future KeyProvisioning enhancement). See [MCUBOOT_QSPI_STORAGE_CONFLICT_06102026.md](MCUBOOT_QSPI_STORAGE_CONFLICT_06102026.md) §4.
    - Pre-allocates a contiguous `/fs_ota/scratch.bin` (0x20000 bytes).
    - Pre-allocates a contiguous `/fs_ota/update.bin` (0x1E0000 bytes).
    - Programs the default public verification key to flash descriptor offset `SIGNING_KEY_ADDR 0x8000300`.
    - Programs the default private decryption key to flash descriptor offset `ENCRYPT_KEY_ADDR 0x8000400`.
  - *Verify:* Wait until the serial monitor outputs `Default Security Keys provisioned successfully.` and `System provisioned. It's now safe to reboot or disconnect your board.`. ⚠️ **Note:** these success messages print even if the QSPI partition step failed (key programming runs regardless). Confirm there was **no** `Error creating MCUboot FAT partition!` line earlier in the output.

- [ ] **Step 4: Initial Device Core Setup**
  - 🛑 **BLOCKED for Client/Server (2026-06-10):** flashing the Client or Server application at this
    point will reformat the entire QSPI as whole-device LittleFS and **destroy** the MCUboot OTA
    partition. Do not perform this step for those roles until the storage architecture is fixed
    (see [MCUBOOT_QSPI_STORAGE_CONFLICT_06102026.md](MCUBOOT_QSPI_STORAGE_CONFLICT_06102026.md) §7).
    The **Viewer** does not use local QSPI storage and is not subject to this conflict.
  - Flash the running application firmware matching the device's role (Client, Server, or Viewer) using local USB, compiled with the MCUboot flag on.
  - *Verify:* Upon boot, ensure the local console outputs `v1.9.1` and confirms memory initialization success.

---

## 2. Bench Testing Validation Matrix

Do not push any MCUboot firmware packages to active hulls in Notehub until all of the following diagnostic procedures pass successfully on hardware.

### [ ] Test 1: Baseline Signed/Encrypted Swap Check
- **Setup:** Device is fully provisioned and is running v1.9.0 (or a developmental build with build sequence less than `191`).
- **Execution:** 
  1. Upload the signed and encrypted `.slot.bin` file of version `1.9.1` (build seq `191`) for the matching role on the Notehub developer fleet.
  2. Let the device poll, download the file into Notecard memory, and auto-detect.
  3. Ensure the device enters staging mode and streams the chunks to `/fs_ota/update.bin`.
- **Expected Outcome:** 
  - Staging logs confirm `MCUboot magic verified` inside block 0.
  - File write completes, `CRC32 link integrity verified` matches the target package, and `/fs_ota/pending_ota.json` is written with status `"trial"`.
  - Watchdog reload is triggered, and `applyUpdate(false)` triggers a system reset.
  - Bootloader executes the swap, mounts the new image, verifies the local health gate, and writes status `"confirmed"`.

### [ ] Test 2: Watchdog Feed During Swap Duration
- **Setup:** Target a large server image (~933 KB) to maximize copy latency. Connect an oscilloscope, logic analyzer, or keep serial print logs open.
- **Execution:**
  1. Perform a swap check (Test 1).
  2. Measure the wall-clock time between the last staged debug log and the new image setup banner.
- **Expected Outcome:**
  - The maximum swap latency must be documented.
  - Ensure the independent hardware watchdog (IWDG) does not fire mid-swap, or confirm that MCUboot's `SWAP_USING_SCRATCH` successfully resumes the journaled sector swap across any watchdog-induced resets without bricking or looping.

### [ ] Test 3: Link Integrity CRC Fail-Closed Check
- **Setup:** Modify an update payload to corrupt a byte or introduce truncation.
- **Execution:**
  1. Stage the corrupted update through the Notecard.
  2. Let the staging loop process the file.
- **Expected Outcome:**
  - Staging code must detect CRC32 mismatch once the stream ends.
  - The device must abort update execution, log `ERROR - CRC32 link integrity mismatch!`, clean the file handles, trigger `dfu.status` `"stop":true` with error status, and return to normal operation without rebooting or resetting.

### [ ] Test 4: Role-Identity Mismatch Prevention
- **Setup:** A provisioned Client device is running v1.9.1.
- **Execution:** Collect a Server `.slot.bin` file and upload it to the Client's Notehub update path.
- **Expected Outcome:**
  - The Client's `strstr` checks on `dfuStatus.firmwareSource` must detect that `"Server"` is present instead of `"Client"`.
  - The staging must be refused beforehand with the error: `MCUboot DFU: ERROR - firmware source does not match device role: Client`.

### [ ] Test 5: Dry-Run Stability & Crash Rollback Checks
- **Setup:** Compile and sign an update image containing a synthetic crash (e.g. `while(true){}`) or memory lockup in its `setup()` block.
- **Execution:**
  1. Stage and trigger the swap of the crashing image.
  2. Hand control over to the bootloader.
- **Expected Outcome:**
  - The board boots the bad image, locks up, and fails to kick the hardware watchdog.
  - Within 30 seconds, the hardware watchdog fires and resets the MCU.
  - MCUboot bootloader detects that the image was in a trial phase and was never confirmed.
  - Bootloader rolls back the swap, restoring the stable v1.9.1 image on the next boot cycle.
  - Stable firmware mounts `fs_ota`, parses `/fs_ota/pending_ota.json`, logs `ROLLBACK DETECTED!`, sends `stop:true` status to clear the Notecard cache, and writes `"failed_rollback"` to prevent downloading that bad version again.

### [ ] Test 6: Offline-Safe Boot Confirmation
- **Setup:** Place the device inside a Faraday cage, metal shielding, or unplug cellular antenna / Ethernet cables to force offline status.
- **Execution:**
  1. Complete a successful swap under these conditions.
  2. Let the newly swapped image boot.
- **Expected Outcome:**
  - The local health gates in `loop()` of [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino) (checking storage available and Notecard I2C ready) must succeed offline.
  - The code must call `tankalarm_markFirmwareHealthy()` and finalize boot status.
  - The device must **not** roll back on next power cycle, even if network telemetry takes hours or days to materialize.

---

## 3. Maintenance File Mapping Reference

When updating, validating, or restoring parts of the update architecture, refer to these source structures:

- **Geometric Constants:** [TankAlarm-112025-Common/src/TankAlarm_MCUbootConfig.h](TankAlarm-112025-Common/src/TankAlarm_MCUbootConfig.h).
- **Core Update Engine:** [TankAlarm-112025-Common/src/TankAlarm_DFU.h](TankAlarm-112025-Common/src/TankAlarm_DFU.h) (contains `tankalarm_performMcubootUpdate`, `tankalarm_resolvePendingOta`, and `tankalarm_markFirmwareHealthy`).
- **Shared Parameters & Versioning:** [TankAlarm-112025-Common/src/TankAlarm_Common.h](TankAlarm-112025-Common/src/TankAlarm_Common.h).
- **Commissioning Helper:** [TankAlarm-112025-KeyProvisioning/TankAlarm-112025-KeyProvisioning.ino](TankAlarm-112025-KeyProvisioning/TankAlarm-112025-KeyProvisioning.ino).
