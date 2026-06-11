# TankAlarm Client MCUboot Provisioning Guide

**One-Time USB Setup to Enable Secure Over-The-Air Updates on Field Clients**

---

## Introduction

The TankAlarm 112025 Client uses the Arduino Opta's **MCUboot** bootloader to perform secure, rollback-protected over-the-air (OTA) firmware updates. Before a Client can receive OTA updates in the field, it must be prepared **once over USB**. This guide walks you through that one-time provisioning process: installing the MCUboot bootloader, loading the signing/encryption keys, partitioning the QSPI flash, and flashing the first signed application.

Because Clients are the only remotely located devices in the system, getting this provisioning right is what makes truck-free firmware updates possible for the rest of the device's service life.

### What You'll Do

- Install the MCUboot-capable bootloader on the Opta's M7 core
- Load the default signing + encryption keys and build the QSPI partition layout
- Flash the first **signed** Client application
- Validate the device with a bench-test matrix before field deployment

### Required Materials

**Hardware:**
- [Arduino Opta](https://docs.arduino.cc/hardware/opta) (Lite or WiFi) with Blues Wireless for Opta
- USB-C cable for programming
- A workstation running the Arduino IDE and this repository

**Software:**
- Arduino IDE 2.0+ with the **Arduino Mbed OS Opta** core installed (`arduino:mbed_opta`)
- This repository checked out locally (provides `arduino-cli.exe`, the sketches, and `mcuboot_keys/`)

### Suggested Reading

Before starting, familiarize yourself with these companion documents:

- [Firmware Update Guide](FIRMWARE_UPDATE_GUIDE.md) — How OTA delivery works through Notehub
- [Client Installation Guide](CLIENT_INSTALLATION_GUIDE.md) — Full Client hardware setup
- [MCUboot Bootloader Options](../../CODE%20REVIEW/MCUBOOT_BOOTLOADER_OPTIONS.md) — Bootloader decision background
- [MCUboot QSPI Storage Conflict](../../CODE%20REVIEW/MCUBOOT_QSPI_STORAGE_CONFLICT_06102026.md) — Why the partition layout matters

---

## How It Works

### The Provisioning Flow

```mermaid
flowchart LR
    A[Step 1: Bootloader Update] --> B[Step 2: Keys & Partitions]
    B --> C[Step 3: Signed App Flash]
    C --> D[Bench Test ✓]
    D --> E[Field Ready]
```

Provisioning is a **one-time, per-device** procedure performed over USB. Once complete, the device only ever receives new firmware as signed OTA packages.

### The QSPI Partition Layout

The Opta's 16 MB QSPI external flash is divided into an MBR (Master Boot Record) partition table. MCUboot and the TankAlarm updater both depend on this layout:

| Partition | Purpose | Size |
|-----------|---------|------|
| 1 | WiFi firmware/certs (unused by TankAlarm) | 1 MB |
| **2** | **MCUboot OTA staging** (`update.bin` + `scratch.bin`) | 5 MB |
| 3 | Provisioning KVStore (unused by TankAlarm) | 1 MB |
| **4** | **Application config store** (LittleFS) | 7 MB |

> 🛑 **Critical architecture rule:** The Client firmware stores its configuration on **Partition 4 only**. It must **never** mount or reformat the entire raw QSPI device (`get_default_instance()`) — doing so destroys the MBR and the Partition 2 OTA staging area, which permanently blocks OTA updates until the device is physically re-provisioned. The v1.9.1+ Client firmware enforces this boundary automatically.

### Security Posture

The keys loaded during provisioning are the **public Arduino default keys**. They provide MCUboot's mechanical benefits — image-integrity checks, atomic A/B swap, and automatic rollback — but **not** firmware authenticity. This is an accepted, documented trade-off for this product; do not treat these signatures as a security control.

---

## Step 1: Install the MCUboot Bootloader

A factory Opta ships with the legacy `Arduino loader` bootloader, which cannot perform OTA swaps. You must replace it with the `MCUboot Arduino` bootloader.

1. Open the Arduino IDE.
2. Select **File ▸ Examples ▸ STM32H747_System ▸ STM32H747_manageBootloader**.
3. Choose **Tools ▸ Board → Arduino Opta** and select the device's serial port. Confirm you are targeting the **M7 core** (the sketch refuses to run on M4).
4. Upload the sketch.
5. Open the Serial Monitor at **115200 baud**.
6. When prompted, **press `Y`** to confirm.

   > ℹ️ **Expected "downgrade" wording:** On a fresh device the current bootloader version reads as uninitialized (`255`), so the tool frames the operation as a *"downgrade to v25"*. This is normal — the bundled image is the correct `MCUboot Arduino` loader. Press `Y`.

7. Wait for the write to finish:
   ```text
   Bootloader update complete. It's now safe to reboot or disconnect your board.
   ```

   > ⚠️ **Do not disconnect power or USB during this write.** This is the only step with brick risk if interrupted.

8. **Verify:** Upload **File ▸ Examples ▸ STM32H747_System ▸ STM32H747_getBootloaderInfo**, open the Serial Monitor at 115200, and confirm:
   - Identifier reads **`MCUboot Arduino`** (not `Arduino loader`)
   - Version is **`25`** (or greater than `24`)

---

## Step 2: Load Keys and Build the Partitions

This step loads the signing/encryption keys and creates the full QSPI partition layout, including the Partition 2 OTA staging files.

1. Open the provisioning sketch: [TankAlarm-112025-KeyProvisioning.ino](../../TankAlarm-112025-KeyProvisioning/TankAlarm-112025-KeyProvisioning.ino).
2. Select the board (`arduino:mbed_opta:opta`) and the device's serial port, then upload.
3. Open the Serial Monitor at **115200 baud** (opening it asserts DTR and boots the sketch).
4. When prompted, **press `Y`**:
   ```text
   Do you want to proceed and load the default keys? Y/[n]
   ```
5. Watch for a clean run with **no errors**:
   ```text
   Creating MBR partition table (p1 WiFi, p2 OTA, p3 KVStore, p4 user)...
   MBR partition table created.
   FAT Partition MBR2 reformatted successfully.
   Allocating scratch file ... (Flashed 100%)
   Allocating update file (1.92 MB) ... (Flashed 100%)
   Default Security Keys provisioned successfully.
   System provisioned. It's now safe to reboot or disconnect your board.
   ```

   > ⚠️ **Confirm there is no `Error creating MCUboot FAT partition!` line.** The "System provisioned" message prints even if partitioning failed, so always scroll up and verify the partition table and file allocation succeeded.

After this step the device requires **signed** application images — the bootloader will refuse to boot a plain/unsigned sketch.

---

## Step 3: Flash the First Signed Application

The Client application must now be compiled in **Signature + Encryption** mode and flashed over DFU.

### 3a. Set the Product UID

Create a `ClientConfig.h` file in [TankAlarm-112025-Client-BluesOpta/](../../TankAlarm-112025-Client-BluesOpta/) (copy from `ClientConfig.h.example`) and set your fleet's Product UID:

```cpp
#define DEFAULT_PRODUCT_UID "com.your-company.your-product:your-project"
```

> ℹ️ `ClientConfig.h` is listed in `.gitignore`, so your credentials are never committed.

### 3b. Compile a Signed Image

From a terminal in the repository root, compile with the `security=sien` FQBN. This signs and encrypts the image with the default keys and pads it to the MCUboot slot size:

```powershell
& ".\arduino-cli.exe" compile `
  --fqbn "arduino:mbed_opta:opta:security=sien" `
  --library ".\TankAlarm-112025-Common" `
  --build-property "build.extra_flags=-DTANKALARM_DFU_MCUBOOT" `
  --build-property "build.version=1.9.1+191" `
  --output-dir "build\secure-client" `
  ".\TankAlarm-112025-Client-BluesOpta\TankAlarm-112025-Client-BluesOpta.ino"
```

> ℹ️ The explicit `build.version` overrides the core's default of `1.2.3+4`. Keep it in sync with `FIRMWARE_VERSION` / `FIRMWARE_BUILD_SEQ`.

### 3c. Flash Over DFU

1. Put the board into DFU mode by **double-tapping the RESET button**.
2. Find the DFU port — it re-enumerates with device ID `2341:0364`:
   ```powershell
   & ".\arduino-cli.exe" board list
   ```
3. Flash the signed binary to the application slot at `0x08040000` (DFU alternate interface 0):
   ```powershell
   & "dfu-util.exe" --device 2341:0364 -a 0 --dfuse-address=0x08040000:leave `
     -D "build\secure-client\TankAlarm-112025-Client-BluesOpta.ino.bin"
   ```
   *(`dfu-util.exe` ships with the core under `…\packages\arduino\tools\dfu-util\`.)*
4. **Press RESET once.** The bootloader verifies the signature and boots the app.
5. **Verify:** Open the Serial Monitor at **115200 baud**. A healthy boot reports:
   ```text
   Mbed OS LittleFileSystem initialized (QSPI partition 4)
   ```
   This confirms the config store mounted on Partition 4 without disturbing the OTA partition.

---

## Step 4: Bench-Test Before Field Deployment

Validate staging, swap, and rollback on the bench before shipping the unit:

1. **Staging verification** — Upload the Client `.slot.bin` to your Notehub developer fleet. The device should download, stage cleanly, and write `"status": "trial"` to `/fs_ota/pending_ota.json`.
2. **A/B swap & rollback** — Let it auto-restart; the bootloader should mount and confirm the new image without watchdog timeouts.
3. **Corrupted image rejection** — Stage a corrupted binary; staging must detect the CRC32 mismatch and return to normal operation without rebooting.
4. **Role-mismatch rejection** — Stage a Server `.slot.bin` to the Client; it must abort with:
   ```text
   MCUboot DFU: ERROR - firmware source does not match device role: Client
   ```
5. **Stability rollback** — Stage a deliberately crashing image. The watchdog should reset within ~30 s, the bootloader rolls back, and the restored firmware logs `ROLLBACK DETECTED!` and blacklists the bad version.

---

## Troubleshooting

### QSPI staging upload fails (`ERASE_PAGE` / `LIBUSB_ERROR_PIPE`)

- **Symptom:** `dfuse_download: libusb_control_transfer returned -9` or `Error during special command "ERASE_PAGE"` during a secure (`security=sien`) upload to the QSPI region.
- **Cause:** The QSPI MBR partition table is missing or destroyed — usually because an older, un-partitioned firmware reformatted the whole device. Secure uploads to the QSPI staging region cannot resolve block locations without it.
- **Resolution:**
  1. Double-tap RESET to enter DFU mode.
  2. Flash the **unsigned** `KeyProvisioning` build directly to internal flash (alt interface 0) at `0x08040000`.
  3. Press RESET once to boot it.
  4. It runs from internal flash, rebuilds the QSPI MBR, restores Partition 2, and reloads the keys.
  5. Resume from **Step 3** — secure uploads now resolve correctly.

### Board only re-appears in DFU mode (`2341:0364`) after flashing

- **Cause:** An **unsigned** image was flashed after keys were loaded; the secure bootloader refuses to boot it.
- **Resolution:** Re-flash using the `security=sien` signed image (Step 3). After keys are loaded, **every** application flash must be signed.

### Console prints "QSPI partition 4 not found"

- **Cause:** The board booted a Client app but was never provisioned with the MBR layout (Step 2 not completed).
- **Resolution:** Run Step 2 (KeyProvisioning). The Client intentionally runs without persistence rather than reformatting the whole device, to protect the OTA partition.

---

## Conclusion

Provisioning prepares a Client for a lifetime of secure, truck-free firmware updates. By installing the MCUboot bootloader, loading keys, building the QSPI partition layout, and flashing a signed application, you give the device atomic A/B swap and automatic rollback protection in the field.

**Key Takeaways:**
- Provisioning is a one-time, per-device USB procedure
- The bootloader update is the only brick-risk step — never interrupt it
- After keys are loaded, every application flash must be **signed** (`security=sien`)
- The Client config store lives on **Partition 4**; the OTA staging area is **Partition 2** — never reformat the whole QSPI device
- Always run the bench-test matrix before field deployment

Happy provisioning! 🚀

---

*Last Updated: June 11, 2026*  
*Firmware Version: 1.9.1+ (build 191)*  
*Compatible with: TankAlarm-112025-Client (Arduino Opta + Blues Notecard)*
