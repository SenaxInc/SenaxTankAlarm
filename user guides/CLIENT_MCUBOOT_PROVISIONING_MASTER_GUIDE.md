# Client MCUboot Hardware Provisioning Master Guide

**Date:** June 10, 2026
**Latest Verified Firmware Version:** v1.9.1 (build 191)
**Scope:** One-time USB-based setup for brand-new or existing Client Opta devices before field deployment.

---

## 1. Safety & Architecture Overview (Read First)

Every Opta device must be provisioned **once via USB** to load structural MCUboot signature verification/decryption keys and format the QSPI external flash layout. 

### 11.1 The Critical Storage Partition Rule
The Arduino standard secure-boot process and external-flash OTA update mechanism **require** the flash memory to be partitioned into an MBR (Master Boot Record) format.
- **Partition 2 (OTA Staging)**: FAT filesystem holding `update.bin` (OTA slot copy target) and `scratch.bin` (journal swap sector).
- **Partition 4 (Application LittleFS)**: Config store where Client variables and logs are stored.

> 🛑 **CRITICAL ARCHITECTURE RULE:** Our Client firmware executes inside Partition 4. Custom configurations must be written only to that partition. **NEVER mount or call format/reformat on the entire raw QSPI device (`get_default_instance()`)**, or you will wipe out the MBR, destroying the OTA staging partitions. This permanently blocks future OTA updates on that device until physical re-provisioning occurs. The updated v1.9.1+ Client firmware enforces this partition boundary.

---

## 2. One-Time USB Provisioning Runbook

Follow these exact steps in order for every Client unit.

```mermaid
flowchart LR
    A[Step 1: Bootloader Update] --> B[Step 2: Key & Partition Setup]
    B --> C[Step 3: Secure App Flash]
    C --> D[Verified Ready ✓]
```

### Step 1: Flash MCUboot-capable Bootloader
1. Open the Arduino IDE on your computer.
2. Select: **File ▸ Examples ▸ STM32H747_System ▸ STM32H747_manageBootloader**.
3. Select your device port (e.g., `COM5`) and board structure (**Arduino Opta**). Ensure you are targeting the **M7 Core** (the compilation fails/refuses to run on the M4 Core).
4. Upload the sketch.
5. Open the Serial Monitor @ **115200 baud**.
6. **Press `Y`** to confirm when prompted:
   *(Note: On a fresh device, since the previous bootloader version is uninitialized, the program may describe the operation as a "downgrade to v25". This is expected behavior; press Y.)*
7. Let the process flash completely. When you see:
   `Bootloader update complete. It's now safe to reboot or disconnect your board.`
   it is safe to proceed.
8. **Verify:** Upload the sibling example **STM32H747_getBootloaderInfo** and open the Serial Monitor @ 115200. **The bootloader identifier MUST read `MCUboot Arduino`** (not `Arduino loader`) and the version must be **`25`** (or greater than `24`).

### Step 2: Run Key Tool & Partition Setup
1. Keep the Arduino IDE open.
2. Open the provisioning sketch: [TankAlarm-112025-KeyProvisioning/TankAlarm-112025-KeyProvisioning.ino](../TankAlarm-112025-KeyProvisioning/TankAlarm-112025-KeyProvisioning.ino).
3. Select your core FQBN (`arduino:mbed_opta:opta`) and port (e.g., `COM5`), then upload the sketch.
4. Open the Serial Monitor @ **115200 baud**. Assert the DTR pin to boot the sketch (opening the Serial Monitor normally does this automatically).
5. When the console prints:
   `Do you want to proceed and load the default keys? Y/[n]`
   **Press `Y`**.
6. The terminal will output:
   ```text
   Creating MBR partition table (p1 WiFi, p2 OTA, p3 KVStore, p4 user)...
   MBR partition table created.
   FAT Partition MBR2 reformatted successfully.
   Allocating scratch file ... (Flashed 100%)
   Allocating update file (1.92 MB) ... (Flashed 100%)
   Default Security Keys provisioned successfully.
   System provisioned. It's now safe to reboot or disconnect your board.
   ```
7. **Ensure there are NO errors** regarding partition creation in the console log. The keys and partitions matching the bootloader's mechanical swap structures are now active.

### Step 3: Flash MCUboot-Enabled signed application
The device now strictly accepts only **signed and encrypted** app binaries linked with our public verification key chain. Plain/unsigned uploads will be rejected at boot.

1. Create a `ClientConfig.h` file in the sketch folder [TankAlarm-112025-Client-BluesOpta/](../TankAlarm-112025-Client-BluesOpta/) (starting from `ClientConfig.h.example`) and edit your target fleet Product UID setting:
   ```cpp
   #define DEFAULT_PRODUCT_UID "com.senaxinc.james:field"
   ```
   *(This file is listed in `.gitignore` to prevent committing customer configs).*
2. Open a command prompt/PowerShell terminal in the workspace root directory.
3. Run the following single command to compile the Client firmware using the security sub-menu options:
   ```powershell
   & "C:\GITHUB\SenaxTankAlarm\arduino-cli.exe" compile --fqbn "arduino:mbed_opta:opta:security=sien" --library "C:\GITHUB\SenaxTankAlarm\TankAlarm-112025-Common" --build-property "build.extra_flags=-DTANKALARM_DFU_MCUBOOT" --build-property "build.version=1.9.1+191" --output-dir "build\secure-client" "TankAlarm-112025-Client-BluesOpta\TankAlarm-112025-Client-BluesOpta.ino"
   ```
   *(This compiles the sketch, resolves includes to our local common library, enables the MCUboot update engine, signs and encrypts the output binary with default keys, and pads it to the MCUboot layout size).*
4. Put the board into DFU mode by **double-tapping the physical RESET button** on the Opta.
5. Scan for the DFU port (re-enumerated as `COM6` or similar with device PID `0x0364` under `arduino-cli board list`).
6. Flash the signed application binary to execution slot `0x08040000` (DFU Alternate Interface 0):
   ```powershell
   & "C:\Users\dorkm\AppData\Local\Arduino15\packages\arduino\tools\dfu-util\0.10.0-arduino1\dfu-util.exe" --device 2341:0364 -a 0 --dfuse-address=0x08040000:leave -D "build\secure-client\TankAlarm-112025-Client-BluesOpta.ino.bin"
   ```
7. **Verify:** When upload completed successfully, **Press the physical RESET button ONCE**.
   - The device will reboot, verify the signed image signature, and run.
   - The application console output at **COM5** @ 115200 baud will report:
     `Mbed OS LittleFileSystem initialized (QSPI partition 4)`
     verifying the partitioned config storage is healthy and active.

---

## 3. Bench Testing Validation Matrix

Before shipping a provisioned Client to the field, perform the following validation matrix to verify staging and swap logic:

1. **Staging Verification**: Upload Client MCUboot package to the developer fleet on Notehub. Allow the device to download the `.slot.bin` image into the Notecard cache and auto-detect. Ensure the stage stream completes cleanly and writes `"status": "trial"` inside `/fs_ota/pending_ota.json`.
2. **A/B Swap & Rollback**: Let the system trigger an auto-restart. Ensure the bootloader successfully mounts the new image and confirmation logic registers without watchdog timeouts.
3. **Invalid/Corrupted Rejection**: Upload a corrupted binary. Staging must detect CRC32 mismatch, refuse update execution, and safely return to normal execution without reboots.
4. **Identity Mismatch Rejection**: Stage a Server `.slot.bin` to the Client. The Client's `strstr` role token guard must abort assembly, logging: `MCUboot DFU: ERROR - firmware source does not match device role: Client`.
5. **Stability Rollback**: Stage a bad trial firmware (containing an intentional memory crash). The board must auto-reset on watchdog timeout within 30 seconds, revert the flash, boot back into the previous release, parse `/fs_ota/pending_ota.json`, log `ROLLBACK DETECTED!`, and blacklist the bad version locally.

---

## 4. Troubleshooting Reference

### QSPI Partition Wiped (DFU Alt 2 Upload Fails)
- **Symptom:** `dfuse_download: libusb_control_transfer returned -9` / `ERASE_PAGE` error during a `dfu-util` Alternate Interface 2 (QSPI) flash.
- **Cause:** The QSPI partition layout table was destroyed (likely by an older, un-partitioned firmware running on the unit). Secure uploads targeting the QSPI staging region cannot resolve block locations.
- **Resolution:** Double-tap RESET to enter DFU mode. Flash the unsigned `KeyProvisioning` sketch directly to Alternate Interface 0 (internal flash) at `0x08040000`. Once uploaded, press RESET once to boot it. This will execute in internal flash, cleanly partition the QSPI, restore `fs_ota` Partition 2, and program keys. Future `security=sien` uploads will now resolve the QSPI block references without errors.

---

*This master guide represents verified production behavior for Senax TankAlarm field units.*
