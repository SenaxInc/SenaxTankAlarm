# CODE REVIEW — OTA Update Failure: v1.9.34 → v1.9.35 (MCUboot Swap Never Takes Effect)

**Date:** 2026-06-18
**Scope:** Root-cause analysis of why the OTA update from v1.9.34 to v1.9.35 downloaded and staged successfully but never swapped — the device reverted to v1.9.34 and blacklisted v1.9.35.
**Companions:**
- [CODE_REVIEW_06212026_OTA_V1930_TO_V1931_FAILURE.md](CODE_REVIEW_06212026_OTA_V1930_TO_V1931_FAILURE.md) — empty-version silent-skip (fixed in v1.9.34)
- [CODE_REVIEW_06152026_OTA_VS_USB_UPDATE_FAILURE.md](CODE_REVIEW_06152026_OTA_VS_USB_UPDATE_FAILURE.md) — the five OTA preconditions
- [CODE_REVIEW_06162026_OTA_SILENT_CHECK_FAILURE.md](CODE_REVIEW_06162026_OTA_SILENT_CHECK_FAILURE.md) — silent check path

---

## 1. Executive Summary

**This is progress, not a regression.** The v1.9.34 fix (recover the firmware version from the OTA filename when `dfu.status` returns no `version`) **worked**. The host now detects, downloads, CRC-verifies, and stages the image, then triggers the MCUboot swap and reboots. The failure has moved **one full stage deeper** in the pipeline — from *"the host never picks up the ready image"* (the v1.9.33 symptom) to *"the host stages and triggers the swap, but the MCUboot bootloader never performs the A/B swap."*

The device's own Notecard `dfu.user` object is unambiguous:

```json
"user": {
  "file":   "TankAlarm-Client-secure-v1.9.35.slot$20260618000502.bin",
  "length": 487741,
  "mode":   "completed",
  "status": "rollback detected - trial crashed - reverted to 1.9.34",
  "dfu_started": 1,
  "dfu_completed": 2,
  "read":   487741,
  "dl_complete": true
}
```

`read: 487741` (= full image length) and `dl_complete: true` prove the host pulled the **entire** image and staged it. The `status` string is generated verbatim by our own client code in `tankalarm_resolvePendingOta()` — meaning the device rebooted, came back up **still running v1.9.34**, saw that the staged target sequence (225) did not match the running build sequence (224), and declared a rollback.

**The MCUboot A/B swap (Phase D) did not happen.** Through direct inspection of the device's Notecard state, the Arduino mbed_opta 4.5.0 core, the MCUboot library source, the CI signing path, and a byte/hash comparison of every signing and encryption key, this review **rules out** key mismatch, slot-geometry mismatch, staging-location mismatch, host staging-logic bugs, encryption-unsupported, and the old imgtool monkeypatch. The remaining, highest-probability cause is that **the firmware currently running on the device is not a `security=sien` signed MCUboot image** (or the MCUboot bootloader is not the one performing boot), so the bootloader does not engage the swap of the signed secondary-slot image. This must be confirmed with a **serial boot capture**, which is the single most valuable next step.

A secondary but important finding: **the rollback/blacklist logic cannot tell "the bootloader ignored the swap" apart from "the trial image booted and crashed."** Both look identical (running sequence ≠ target sequence), so the client blacklists the target version on the *first* failure. With the swap broken for an external reason, **every version number we try gets permanently burned** on the device until `pending_ota.json` is cleared.

---

## 2. How Far the Update Got (Pipeline Stage Map)

| Phase | Step | v1.9.33 attempt | v1.9.35 attempt |
|------|------|----------------|----------------|
| A | Notehub → Notecard download | ✅ `ready` | ✅ `dl_complete:true` |
| B | Host detects update (`dfu.status`) | ❌ empty version → **silent skip** | ✅ version recovered from filename |
| C | Host enters DFU, pulls chunks (`dfu.get`) | — never ran | ✅ `read: 487741` (full) |
| D | Host verifies MCUboot magic + CRC32 | — | ✅ magic `0x96f3b83d` + CRC OK (no abort) |
| E | Host writes `update.bin`, pads slot | — | ✅ staged |
| F | Host writes `pending_ota.json` (trial) | — | ✅ (rollback path read it back) |
| G | `MCUboot::applyUpdate(false)` + reset | — | ✅ device rebooted |
| **H** | **Bootloader verifies + swaps slots** | — | ❌ **did not swap → booted old app** |
| I | New image confirms itself | — | ❌ never reached |
| — | Client rollback detection | — | ✅ fired → **v1.9.35 blacklisted** |

The v1.9.34 fix carried the process from Phase B all the way to Phase G. **Phase H — which runs entirely inside the MCUboot bootloader, with no application code involved — is where it now fails.**

---

## 3. Evidence Collected (Live Device + Toolchain)

### 3.1 Notehub device state (`dev:860322068056545`, "Silas")
- `dfu.user.file` = `TankAlarm-Client-secure-v1.9.35.slot$20260618000502.bin`
- `dfu.user.mode` = `completed`, `status` = `rollback detected - trial crashed - reverted to 1.9.34`
- `read` = `length` = `487741`, `dl_complete` = `true`, `dfu_started:1`, `dfu_completed:2`
- Subsequent telemetry/alarm/diag notes all report `fv:"1.9.34"`.

### 3.2 The rollback string is ours
`tankalarm_resolvePendingOta()` in [TankAlarm_DFU.h](../TankAlarm-112025-Common/src/TankAlarm_DFU.h) builds exactly:
```cpp
snprintf(err_buf, sizeof(err_buf),
         "rollback detected - trial crashed - reverted to %s", FIRMWARE_VERSION);
```
So the device booted v1.9.34, read back `pending_ota.json` (`target_seq:225, status:"trial"`), compared it to the running `FIRMWARE_BUILD_SEQ` (224), and — because `224 != 225` — concluded a rollback and reported it to Notehub.

---

## 4. What This Review Rules Out (with proof)

Each of these was previously listed as a "suspect/prerequisite" in earlier reviews. They are now **eliminated**:

### 4.1 ❌ Signing / encryption key mismatch — RULED OUT (hash-proven)
The CI signs the slot image by compiling with `--fqbn arduino:mbed_opta:opta:security=sien`, which uses the Arduino core's bundled imgtool **1.8.0-arduino.2** and the keychain at `…/libraries/MCUboot/default_keys/` (per `boards.txt`). The device bootloader is provisioned by `TankAlarm-112025-KeyProvisioning` using `mcuboot_keys/`.

A SHA-256 comparison of the **actual signing key files** is identical:

```
core security=sien signing key : 2CFF04B18AFF76CFCC9470968458C5FD618F68CE2F5F0F188EF29FD6CD65614B
repo mcuboot_keys  signing key : 2CFF04B18AFF76CFCC9470968458C5FD618F68CE2F5F0F188EF29FD6CD65614B
```

The **signing public key** and **encryption public key** were also compared **byte-for-byte** between `mcuboot_keys/*.pem` (CI) and the provisioning headers `TankAlarm-112025-KeyProvisioning/ecdsa-p256-*-key.h` (bootloader) — both identical:
- Signing pub EC point: `04 d5 16 35 26 … 18 49 05`
- Encrypt pub EC point: `04 6a c9 20 4c … e0 37 b0`

All four sources (CI `security=sien` default_keys, repo `mcuboot_keys/`, KeyProvisioning `.h`, and the core's `examples/enableSecurity/*.h`) are the **same Arduino default keychain**. CI signs with exactly the key the bootloader is provisioned to trust. **Keys are not the problem.**

### 4.2 ❌ Slot-geometry mismatch — RULED OUT
The core's [`mcuboot_config.h`](file) (mbed_opta 4.5.0) and the app's `TankAlarm_MCUbootConfig.h` agree exactly:

| Parameter | Core bootloader lib | App / CI |
|----------|--------------------|----------|
| Slot size | `MCUBOOT_SLOT_SIZE 0x1E0000` | `TANKALARM_MCUBOOT_SLOT_SIZE 0x1E0000` / imgtool `--slot-size 0x1E0000` |
| Scratch | `MCUBOOT_SCRATCH_SIZE 0x20000` | `0x20000` |
| Header | (app at primary+0x20000) | `--header-size 0x20000` |
| Primary start | `MCUBOOT_PRIMARY_SLOT_START_ADDR 0x8020000` (app @ `0x8040000`) | upload addr `0x08040000` |
| Max align | `MCUBOOT_BOOT_MAX_ALIGN 32` | `--align 32 --max-align 32` |
| Sign alg | `MCUBOOT_SIGN_EC256` | EC P-256 keys |
| Swap | `MCUBOOT_SWAP_USING_SCRATCH 1` | scratch.bin provisioned |

### 4.3 ❌ Staging-location mismatch — RULED OUT
The bootloader reads the **secondary slot** through a FAT file. From the core's `flash_map_backend/secondary_bd.cpp`:
```cpp
static mbed::MBRBlockDevice mbr(raw, 2);
static mbed::FATFileSystem fs("fs");
static mbed::FileBlockDevice secondary_bd("/fs/update.bin", "rb+", MCUBOOT_SLOT_SIZE, …);
static mbed::FileBlockDevice scratch_bd ("/fs/scratch.bin", "rb+", MCUBOOT_SCRATCH_SIZE, …);
```
The app stages to `/fs_ota/update.bin` on the same **MBR partition 2**. The mount-prefix difference (`fs` vs `fs_ota`) is only an in-application label — **physically the same `update.bin` file in the root of partition 2's FAT volume.** The bootloader and the app target the identical bytes.

### 4.4 ❌ Host staging logic — RULED OUT (it works)
`tankalarm_performMcubootUpdate()` correctly: switches the Notecard to `dfu` mode → pulls chunks via `dfu.get` → verifies the MCUboot magic on the first chunk → writes `update.bin` → pads the remainder of the slot to `0x1E0000` with `0xFF` → verifies the staged CRC32 against the Notecard `body.crc32` → writes `pending_ota.json` → `dfu.status {stop}` → `MCUboot::applyUpdate(false)` → `NVIC_SystemReset()`. The `dfu.user` telemetry (`read == length`, `dl_complete:true`, no failure status from the `mcuboot_restore_hub` path) confirms every one of these steps ran to completion.

### 4.5 ❌ Bootloader lacks encryption support — RULED OUT
The prebuilt core bootloader `bootloaders/OPTA/bootloader.elf` (4.5.0) contains the encryption symbols `boot_enc`, `boot_encrypt`, `boot_enc_decrypt`, `boot_enc_load`, `enc_key`, and `ecies` — it can decrypt EC256-encrypted images (*if* the device actually runs this bootloader; see §5).

### 4.6 ❌ The old imgtool monkeypatch — ALREADY REMOVED
A prior approach signed with pip `imgtool` patched via `self.header_size & 0xFFFF` (which masks `0x20000`→`0`). That was abandoned in commit `a9bacd1`. The **current** workflow (which built v1.9.35) compiles with `security=sien` and uses the Arduino-bundled imgtool. The workflow comment is explicit: the old method *"produced a subtly divergent image the bootloader would not swap."* So the image format is now the Arduino-canonical one.

> ⚠️ **Tooling note for future investigators:** the editor's file-read returned a **stale** copy of the workflow (showing the old imgtool step). `git show HEAD:…` and `Get-Content` are the ground truth — always verify the workflow via the terminal/git, not a cached read.

---

## 5. Leading Hypotheses for Why the Swap Doesn't Happen

With keys, geometry, staging, host logic, encryption, and signing format all eliminated, the failure is isolated to **Phase H inside the bootloader**. Ranked by probability:

### H-1 (★ LEADING) — The running firmware is not a `security=sien` signed image, so the MCUboot swap path is not engaged
Per the repo's own [CLIENT_MCUBOOT_PROVISIONING_GUIDE.md](../Tutorials/Tutorials-112025/CLIENT_MCUBOOT_PROVISIONING_GUIDE.md):
> *"After this step the device requires **signed** application images — the bootloader will refuse to boot a plain/unsigned sketch."*
> *Step 3: "The Client application must now be compiled in **Signature + Encryption** mode (`security=sien`) and flashed over DFU."*

The OTA artifact is a `security=sien` signed slot image. The MCUboot swap+verify chain is designed around the device **already running** a signed image whose primary slot the bootloader manages. Every USB install we have done in this campaign used the **plain** `TankAlarm-Client-v1.9.3x.bin` (`--fqbn …:opta`, i.e. `security=none`), **not** the signed `security=sien` image. If the running image is the plain build, the device is not in the MCUboot-managed secure-boot state that the staged-signed-secondary swap expects, and the pending swap does not stick.

**Important tension to resolve on the bench:** the guide says a fully provisioned device should *refuse* to boot a plain sketch — yet this device boots v1.9.34 and reports telemetry. That means one of:
- (a) the device is **not** fully/strictly enforcing signatures (partial or non-secure provisioning), **or**
- (b) the v1.9.34 it is running actually **is** a signed image and the swap fails for another reason (H-2/H-3).

Only a **serial boot capture** (or `STM32H747_getBootloaderInfo`) can disambiguate.

### H-2 — The bootloader on the unit is not the MCUboot `v25` loader (or an incompatible version)
`KeyProvisioning` only writes keys/partitions if it reads `"MCUboot Arduino"` version `> 24`; otherwise it FATALs. Partition 2 exists (staging mounted and CRC-passed), which *suggests* provisioning completed at least once. But the bootloader could have been changed since, or the unit could be running an image flashed directly to `0x08040000` while the bootloader is an older/stock loader that doesn't service the QSPI secondary slot. A bench read on 2026-06-10 showed `"Arduino loader / Bootloader version: 255"` (the stock loader) on *a* unit — this must be re-verified on **this** unit.

### H-3 — Genuine MCUboot validation failure during swap (format/version/encryption edge)
Less likely given §4, but not yet observed directly. The bootloader could reject the secondary image for a reason only its serial log reveals (e.g., `boot_swap_type` resolving to none, an image-version/`ih_ver` check, a TLV/area-size edge with the 4.5.0 `--max-align 32` layout, or an encryption-TLV handling difference). The bootloader logs at `MCUBOOT_LOG_LEVEL 4` (INFO) — a capture will state the exact reason.

### H-4 — Swap happened, the trial image crashed before confirming, MCUboot reverted
Effectively impossible to distinguish from H-1/H-2 *without serial*, but **unlikely** because v1.9.35 is a **version-only bump** — byte-for-byte identical logic to the v1.9.34 that boots fine. If v1.9.34 runs, a swapped v1.9.35 would run too, and `MCUboot::confirmSketch()` is called **early and unconditionally** in `setup()`. (If this *were* the cause, it would point at the swap itself corrupting the image.)

---

## 6. Secondary Finding — The Rollback/Blacklist Logic Mis-classifies "Swap Never Happened"

This is a real code-level problem that is **compounding** the situation and burning version numbers.

`tankalarm_resolvePendingOta()` decides "rollback" purely from `FIRMWARE_BUILD_SEQ != target_seq`:

```cpp
if (FIRMWARE_BUILD_SEQ == target_seq) {            // trial succeeded
    … write "confirmed" …
} else {                                            // ANY other reason
    … "rollback detected - trial crashed - reverted" …
    … write "failed_rollback"  → tankalarm_isVersionBlacklisted() will refuse it forever …
}
```

This conflates **two physically different failures**:

| Reality | Running seq after reboot | Code's conclusion | Correct conclusion |
|--------|--------------------------|-------------------|--------------------|
| Bootloader **ignored** the swap (image is fine) | 224 (unchanged) | "trial crashed → blacklist 1.9.35" ❌ | "swap never happened — fix the bootloader, image is OK" |
| Swap happened, trial **crashed**, MCUboot reverted | 224 | "trial crashed → blacklist" ✅ | same |

Because the current OTA failure is the **first** row (external bootloader/provisioning issue, not a bad image), blacklisting is **wrong and harmful**: it permanently marks each attempted version as `failed_rollback` on the device. The operator must then either clear `/fs_ota/pending_ota.json` over USB or burn a brand-new version number on every single attempt — which is exactly what we are now forced to do (1.9.35 is dead on this device).

**There is no on-device, remote way to clear this blacklist today.**

---

## 7. Recommended Next Steps (Diagnostic-First)

The cheapest path to certainty is a **serial capture**, because Phase H is entirely in the bootloader and prints its decision.

### Step 1 — Capture the bootloader's own boot log during an OTA (DEFINITIVE)
1. Connect the client Opta over USB; open serial at **115200**.
2. Trigger/await an OTA of a **fresh** version (see Step 4).
3. Capture the reboot. Look for MCUboot bootloader lines, e.g.:
   - `Swap type: test/perm/revert/none`
   - `Image in the secondary slot is not valid!`
   - signature/hash/`boot_image_validate` errors
   - or **no bootloader output at all** (→ strongly implies a non-MCUboot / stock loader, H-2).

### Step 2 — Read the bootloader identity (one sketch, decisive for H-2)
Flash **`File ▸ Examples ▸ STM32H747_System ▸ STM32H747_getBootloaderInfo`** and confirm over serial:
- Identifier = **`MCUboot Arduino`** (not `Arduino loader`)
- Version = **`25`** (or `> 24`)

If it reads `Arduino loader` / `255`, the unit never had (or lost) the MCUboot bootloader → that alone explains every OTA failure. Re-run the provisioning sequence (manageBootloader → KeyProvisioning).

### Step 3 — Verify the running image is actually `security=sien` (decisive for H-1)
Re-flash the client with the **signed** image per the provisioning guide, instead of the plain `.bin`:
```powershell
arduino-cli compile --fqbn "arduino:mbed_opta:opta:security=sien" `
  --build-property "build.extra_flags=-DTANKALARM_DFU_MCUBOOT" `
  --build-property "build.version=1.9.36+226" `
  --libraries ".\TankAlarm-112025-Common" `
  --output-dir "build\secure-client" `
  ".\TankAlarm-112025-Client-BluesOpta\TankAlarm-112025-Client-BluesOpta.ino"

# double-tap RESET to enter DFU, then:
dfu-util --device 2341:0364 -a 0 --dfuse-address=0x08040000:leave `
  -D "build\secure-client\TankAlarm-112025-Client-BluesOpta.ino.bin"
```
A device running a confirmed-signed image is the correct baseline for MCUboot OTA. If OTA then swaps, H-1 was the cause.

### Step 4 — Use a FRESH version for every retry, and clear the blacklist
- v1.9.35 is now blacklisted on this unit. The next OTA test must use a **new** version (e.g. **v1.9.36 / seq 226**).
- If staying on a number is required, the blacklist must be cleared by deleting `/fs_ota/pending_ota.json` over USB (or re-running KeyProvisioning, which reformats partition 2).
- Ensure the Notehub host-firmware version string stays plain (`1.9.36`), per the prior review.

### Step 5 — (If Steps 1–3 pass but swap still fails) compare image layout
Confirm the `security=sien` output `.bin` begins with the MCUboot header magic `0x96f3b83d` (it does, or staging would have aborted) **and** that its `ih_ver` matches the `--build.version` (`1.9.36+226`). A bootloader that enforces monotonic image versions could reject a secondary whose embedded version isn't strictly greater than the primary's.

---

## 8. Recommended Code / Process Improvements

These do not require the device and harden the system regardless of the H-1/H-2/H-3 outcome.

### 8.1 Distinguish "swap never happened" from "trial crashed" before blacklisting
Add a **boot-attempt counter** to `pending_ota.json` so a single failed swap does not permanently blacklist a version:
```jsonc
{"target_seq":226,"target_v":"1.9.36","status":"trial","attempts":1}
```
- On a `trial` boot where `running_seq != target_seq`: increment `attempts`; only transition to `failed_rollback` after, say, **2–3** attempts. A bootloader that *ignores* the swap will leave the secondary pending, so a re-`applyUpdate()` + reset can be retried instead of immediately blacklisting an image that is actually valid.
- Log the distinction over serial: `"OTA: target staged but running seq unchanged after reset — swap did NOT occur (bootloader/provisioning), NOT a crashed trial."`

### 8.2 Surface bootloader identity + secure-mode in the OTA self-check
Extend `tankalarm_otaSelfCheck()` (already printed at boot) to also report the bootloader identifier/version (read from `0x8000000 + 0x2F0` / `+0x1F001`, exactly as KeyProvisioning does) and whether the running image is signed. This makes "stock loader / non-secure image" visible in the boot banner and (optionally) in telemetry, so the operator never again has to guess remotely.

### 8.3 Provide a remote/serial blacklist-recovery path
Add a guarded mechanism (serial maintenance command, or a pushed-config flag like `clearOtaBlacklist`) that deletes `/fs_ota/pending_ota.json`, so a falsely-blacklisted version can be retried after the bootloader/provisioning is corrected — without a USB visit.

### 8.4 Pin a deterministic USB-install artifact for "OTA-capable baseline"
Document and standardize that the **client must be USB-installed with the `security=sien` signed image** (not the plain `.bin`) to be OTA-capable. Consider publishing the `security=sien` `.bin` (and/or `.with_bootloader.bin`) as a release asset labelled "USB-install / OTA-capable," so field installs don't accidentally use the non-secure build.

### 8.5 Add a CI assertion that the signed slot image is well-formed
In the release workflow, after the `security=sien` build, assert the output begins with MCUboot magic `0x96f3b83d` and that `imgtool verify` (Arduino-bundled) passes against `mcuboot_keys/`. This catches any future signing-path drift before it ships.

---

## 9. What We Know vs. What Needs the Bench

| Question | Status |
|---------|--------|
| Did the host detect/download/stage/trigger? | ✅ **Yes** — proven by `dfu.user` (`read==length`, `dl_complete`, our rollback string) |
| Did the v1.9.34 filename-recovery fix work? | ✅ **Yes** — that's why we reached the swap stage at all |
| Do the signing/encryption keys match? | ✅ **Yes** — SHA-256 + byte-identical |
| Do slot geometry / staging location match? | ✅ **Yes** — verified against core 4.5.0 source |
| Is the signed image format Arduino-canonical? | ✅ **Yes** — CI uses `security=sien` (monkeypatch removed) |
| Did the MCUboot bootloader swap the image? | ❌ **No** — booted old app |
| **Is the device running a `security=sien` signed image?** | ❓ **Unknown — needs serial / re-flash (H-1)** |
| **Is the unit's bootloader `MCUboot Arduino` v25?** | ❓ **Unknown — needs `getBootloaderInfo` (H-2)** |
| **What does the bootloader print at swap time?** | ❓ **Unknown — needs serial capture (H-3)** |

---

## 10. Summary of Findings

| # | Finding | Severity | Status |
|---|---------|----------|--------|
| 1 | v1.9.34 filename-version recovery fix **works**; host now stages + triggers swap | INFO | ✅ Confirmed progress |
| 2 | MCUboot **A/B swap never takes effect** → device reverts to v1.9.34 | **CRITICAL** | Root failure (Phase H) |
| 3 | Signing/encryption keys are byte/hash **identical** across CI + bootloader | INFO | ✅ Ruled out |
| 4 | Slot geometry / staging location / host staging logic all correct | INFO | ✅ Ruled out |
| 5 | CI signing already uses Arduino-canonical `security=sien` | INFO | ✅ Ruled out |
| 6 | **Running image is likely `security=none` (plain), not `security=sien` signed** | **HIGH** | ★ Leading hypothesis (H-1) |
| 7 | Bootloader identity/version on this unit unverified | **HIGH** | Needs `getBootloaderInfo` (H-2) |
| 8 | Rollback logic **can't distinguish "swap ignored" from "trial crashed"** → blacklists good images, burns version numbers | **HIGH** | Code fix proposed (§8.1) |
| 9 | No remote way to clear a false `failed_rollback` blacklist | MEDIUM | Code fix proposed (§8.3) |

**Bottom line:** the firmware-side OTA pipeline is now correct through staging and trigger; the break is in the **bootloader swap stage**, almost certainly because the device is **not running a `security=sien` signed image** and/or its **bootloader identity is unverified**. The next action is a **serial boot capture + `STM32H747_getBootloaderInfo`**, then re-flash the client with the **signed** image and retry OTA on a **fresh** version (v1.9.36). In parallel, fix the blacklist mis-classification so a broken swap stops permanently burning version numbers.

---

## 11. References

- Device Notecard state (Notehub): `dev:860322068056545` raw JSON `dfu.user`
- DFU implementation: [TankAlarm-112025-Common/src/TankAlarm_DFU.h](../TankAlarm-112025-Common/src/TankAlarm_DFU.h)
- MCUboot slot config: [TankAlarm-112025-Common/src/TankAlarm_MCUbootConfig.h](../TankAlarm-112025-Common/src/TankAlarm_MCUbootConfig.h)
- Provisioning sketch: [TankAlarm-112025-KeyProvisioning/TankAlarm-112025-KeyProvisioning.ino](../TankAlarm-112025-KeyProvisioning/TankAlarm-112025-KeyProvisioning.ino)
- Provisioning guide: [Tutorials/Tutorials-112025/CLIENT_MCUBOOT_PROVISIONING_GUIDE.md](../Tutorials/Tutorials-112025/CLIENT_MCUBOOT_PROVISIONING_GUIDE.md)
- Release workflow: [.github/workflows/release-firmware-112025.yml](../.github/workflows/release-firmware-112025.yml)
- Core MCUboot lib (4.5.0): `…/packages/arduino/hardware/mbed_opta/4.5.0/libraries/MCUboot/` (`MCUboot.cpp`, `mcuboot_config/mcuboot_config.h`, `flash_map_backend/secondary_bd.cpp`)
- Core bootloader: `…/mbed_opta/4.5.0/bootloaders/OPTA/bootloader.{bin,elf}`
- Arduino Opta MCUboot OTA (external): https://docs.arduino.cc/tutorials/opta/getting-started-ota-qspi/
- Blues IAP firmware update (external): https://dev.blues.io/notehub/host-firmware-updates/iap-firmware-update/

## 12. AI Agent Review and Recommendations

Upon reviewing the evidence, including the telemetry from Notehub (which confirms `dfu.user` successfully read and staged the full image length), it is clear that the OTA process is functioning correctly up until the handoff to the MCUboot bootloader. The rollback is triggered not because the new image crashes, but because the bootloader entirely ignores the staged image, rebooting into the unchanged existing primary slot.

The misclassification of "swap ignored" as "trial crashed" in `TankAlarm_DFU.h` is exacerbating the issue by burning versions (like v1.9.35) via a permanent local blacklist.

**Recommended Actions to Proceed:**
1. **Immediate Diagnostic (Serial Bootlog):** We absolutely need the serial capture during reset. This is the only way to peek inside the black box of Phase H. If the bootloader says `Image in the secondary slot is not valid!`, then we have an encryption/signature validation issue. If it prints nothing or skips the secondary slot, H-1 or H-2 is confirmed.
2. **Get Bootloader Info:** Run `STM32H747_getBootloaderInfo` to guarantee we are actually running `MCUboot Arduino v25+`.
3. **Baseline the Device (`security=sien`):** If not done already, re-flash the base v1.9.34 firmware over USB using `--fqbn arduino:mbed_opta:opta:security=sien`. If the device is currently running an unsigned image, it will not initiate secure swaps.
4. **Code Fix (`TankAlarm_DFU.h`):** Once physical diagnostics are complete, implement the "attempts" counter as proposed in §8.1 to stop the erroneous blacklisting of valid images.

For the very next OTA attempt, use a fresh version (e.g., `v1.9.36`) since `v1.9.35` is now stuck in the `failed_rollback` blacklist.

---

## 13. Deep-Dive Staging Code Review & Discovery of filesystem-Locking Conflict

During a thorough review of the host's OTA staging sequence in [TankAlarm-112025-Common/src/TankAlarm_DFU.h](TankAlarm-112025-Common/src/TankAlarm_DFU.h) alongside Arduino's underlying MCUboot library implementation (FileBlockDevice.cpp), a **critical block-level conflict** has been identified. This structural locking problem explains why the MCUboot swap trailer was never written on Phase G, forcing the bootloader to ignore the update.

### 13.1 The Conflict/Locking Mechanics

1. **Host-Side Lock (fs_ota):** In `tankalarm_performMcubootUpdate()` inside [TankAlarm-112025-Common/src/TankAlarm_DFU.h](TankAlarm-112025-Common/src/TankAlarm_DFU.h), the host application mounts QSPI MBR partition 2 as a FAT filesystem using `static mbed::FATFileSystem ota_data_fs("fs_ota")` to open /fs_ota/update.bin and stream the download chunks.
2. **Scoping Issue Prevents Cleanup:** Crucially, `ota_data_fs` is defined as a `static` local variable restricted **entirely inside a block scope** (Step 3 nested `{ ... }` block). Because of this block-scoping, its visibility ends outside that block, and `ota_data_fs.unmount()` is **never called** in either the success path or the `mcuboot_restore_hub` failure path. It remains mounted continuously under the fs_ota prefix.
3. **MCUboot Library Attempt (fs):** When `MCUboot::applyUpdate(false)` is subsequently called, it invokes `boot_set_pending()`. Under the hood, this attempts to open the secondary slot block device using Arduino's `mbed::FileBlockDevice("/fs/update.bin", "rb+", ...)` which tries to mount partition 2 under the prefix fs (via MCUboot's internal `get_filesystem()`).
4. **The Silent Failure:** In Mbed OS, a partition cannot be mounted simultaneously under two different FAT filesystem instances with different mount points. The second mounting attempt (`fs.mount(&mbr)`) fails internally with an error. As a result, `FileBlockDevice::init()` fails with `BD_ERROR_DEVICE_ERROR` because `fopen("/fs/update.bin", "rb+")` returns `NULL`.
5. **No Trailer Written:** Because the file cannot be opened, `applyUpdate(false)` is unable to write the trailer (magic number + swap flag) at the end of the slot. Since `applyUpdate` has a `void` return type, this failure is completely silent on the application side. The MCU resets without the swap trailer ever being written to the QSPI flash block.
6. **Rollback Misclassification:** On reboot, the bootloader mounts partition 2 (successfully now, since the host application isn't running to hold the lock), reads /fs/update.bin, sees no pending trailer, assumes there is no update, and boots the original image (v1.9.34). Upon booting, the original image detects `"trial"` in pending_ota.json, sees that the sequence didn't change, misclassifies this as a trial crash/rollback, and blacklists v1.9.35.

### 13.2 Suggested Code Fixes in [TankAlarm-112025-Common/src/TankAlarm_DFU.h](TankAlarm-112025-Common/src/TankAlarm_DFU.h)

To solve this, we must ensure that the QSPI FAT partition is completely unmounted before the MCUboot library is invoked.

1. **Move Declarations to Function Scope:**
   Move `qspi_root`, `ota_data`, and `ota_data_fs` out of the Step 3 inner block so that they are in function scope. Use a tracking boolean `bool otaMounted = false;` to record mount status.

   ```cpp
   // Move to function scope in tankalarm_performMcubootUpdate()
   static QSPIFBlockDevice qspi_root(QSPI_SO0, QSPI_SO1, QSPI_SO2, QSPI_SO3, QSPI_SCK, QSPI_CS, QSPIF_POLARITY_MODE_1, 40000000);
   static mbed::MBRBlockDevice ota_data(&qspi_root, 2);
   static mbed::FATFileSystem ota_data_fs("fs_ota");
   bool otaMounted = false;
   ```

2. **Trigger Unmount in Success Path:**
   Right before calling `MCUboot::applyUpdate(false)`, perform an explicit unmount:

   ```cpp
   // --- Step 4d complete, free filesystem before handoff ---
   if (otaMounted) {
     ota_data_fs.unmount();
     otaMounted = false;
     Serial.println(F("MCUboot DFU: Unmounted fs_ota partition to release lock."));
   }

   MCUboot::applyUpdate(false);
   ```

3. **Handle Failure Path Robustly:**
   In the `mcuboot_restore_hub:` block, ensure any active mount is cleaned up so we don't leave the block device locked:

   ```cpp
   mcuboot_restore_hub:
     Serial.println(F("MCUboot DFU: FAILED * restoring normal operation"));
     if (fp) {
       fclose(fp);
       fp = nullptr;
     }
     if (otaMounted) {
       ota_data_fs.unmount();
       otaMounted = false;
     }
   ...
   ```

### 13.3 Recommended Tactical Proceed Plan

1. **Keep Code Intact For Now:** As requested, do not apply any code changes yet.
2. **Review with Team:** Compare the newly discovered lockup mechanics with existing bench observations.
3. **Execute Bench Diagnostic:** Once ready, apply this unmount change to [TankAlarm-112025-Common/src/TankAlarm_DFU.h](TankAlarm-112025-Common/src/TankAlarm_DFU.h) and re-compile the Client using `--fqbn arduino:mbed_opta:opta:security=sien`.
4. **Clear false blacklists on the bench device:** Perform a re-partitioning or delete pending_ota.json, and run a clean OTA update test using version `v1.9.36`.

---

## 14. Independent Validation Addendum (Copilot, 2026-06-18)

This addendum reflects a fresh pass over the live Notehub **Recent** event stream, current repo source, and the Arduino MCUboot core implementation used by this project.

### 14.1 Event-Log Corroboration from Notehub "Recent"

The live device event stream adds strong support to the rollback interpretation already documented above:

1. `_health.qo` includes:
  - `method: "dfu"`
  - `text: "DFU host firmware completed: rollback detected - trial crashed - reverted to 1.9.34"`
2. `config_ack.qo` includes:
  - `st: "ota-reverted"`
  - `reason: "trial boot reverted by MCUboot"`
  - `from_v: "1.9.34"`
  - `target_v: "1.9.35"`

These two events independently confirm that the host considered the OTA cycle complete from its perspective, rebooted, then came back on the old image and classified the trial as reverted.

In the captured Recent snapshot, there were **no** matches for `ota-stage-failed`, `staged for MCUboot`, or `firmware update failed`. That is not absolute proof of absence across all historical events, but it is consistent with "staging succeeded, handoff/swap failed" rather than a pre-swap staging abort.

### 14.2 Independent Code-Level Confirmation of the Handoff Risk

After reviewing both app code and Arduino core code, the filesystem-handoff conflict in §13 appears technically credible and high-probability:

1. In `tankalarm_performMcubootUpdate()` in [TankAlarm-112025-Common/src/TankAlarm_DFU.h](../TankAlarm-112025-Common/src/TankAlarm_DFU.h), partition 2 is mounted as `fs_ota` and used for `/fs_ota/update.bin`.
2. That mount is not explicitly unmounted before `MCUboot::applyUpdate(false)`.
3. In core `secondary_bd.cpp` (mbed_opta 4.5.0), MCUboot uses a separate FAT object mounted as `fs` and accesses `/fs/update.bin`.
4. Core comment explicitly notes mount may fail if already mounted (`// Mount can fail if filesystem already mounted`).
5. In core `FileBlockDevice.cpp`, failure to open `/fs/update.bin` returns `BD_ERROR_DEVICE_ERROR`.
6. In core `bootutil_public.c`, `boot_set_pending()` returns nonzero on failure.
7. In core `MCUboot.cpp`, `MCUboot::applyUpdate()` is `void` and ignores that return value.

That chain creates a realistic **silent-failure path** where trailer write fails, no swap is scheduled, reboot returns to old firmware, and app logic later marks rollback.

### 14.3 Updated Hypothesis Ranking

Based on the fresh event evidence + source review, I recommend this ordering for next bench verification:

1. **H-A (Co-leading): fs_ota mount not released before `applyUpdate()`** prevents MCUboot `fs` mount/trailer write.
2. **H-B (Co-leading): bootloader/baseline state mismatch** (non-MCUboot loader and/or non-`security=sien` runtime baseline).
3. **H-C:** true bootloader validation rejection (signature/TLV/version edge) despite correct staging.

Interpretation: §13's lock conflict is now strong enough to treat as a co-leading hypothesis with H-1/H-2, not just a speculative side path.

### 14.4 Recommended No-Code-Change Validation Sequence (Next Bench Session)

Before changing firmware source, run this exact sequence to maximize diagnostic value:

1. Capture `STM32H747_getBootloaderInfo` output on the exact failing unit.
2. Run one OTA attempt with a fresh version (v1.9.36+) and capture full serial reboot output.
3. Immediately export Notehub Recent/JSON after the attempt.
4. Verify whether `ota-reverted` appears again without any `ota-stage-failed` marker.

If these reproduce exactly, then in the next review/patch phase apply only the minimal unmount fix from §13.2 and repeat the same test. A pass/fail delta on that single change will decisively confirm or falsify the lock hypothesis.

### 14.5 Proceed Recommendation

No source code changes in this pass (as requested). For the next implementation pass, the highest-value first patch remains:

1. Explicit unmount of `fs_ota` before `MCUboot::applyUpdate(false)`.
2. Guaranteed unmount in all failure/cleanup paths.
3. Optional logging around the handoff so the trailer-write stage is no longer opaque.

Combined with the blacklist-attempts refinement in section 8.1, this gives both a likely technical fix and a safer retry model if swap/handoff fails for environmental reasons.

---

## 15. Follow-Up Review After v1.9.35 Failure (Copilot, 2026-06-18)

This pass reviewed the live Notehub Recent page, the raw device JSON, the current working-tree OTA code, the installed Arduino `mbed_opta` 4.5.0 MCUboot implementation, the release workflow from terminal/git, and the Blues DFU documentation. No firmware source changes were made in this pass.

### 15.1 Event Log / Raw JSON Findings

The event log and device JSON continue to support the same conclusion: the host reached the end of the Notecard IAP pull, but the application did not boot into v1.9.35.

1. The live Recent stream contains `_health.qo` with:
  - `method: "dfu"`
  - `text: "DFU host firmware completed: rollback detected - trial crashed - reverted to 1.9.34"`
2. The live Recent stream contains `config_ack.qo` with:
  - `st: "ota-reverted"`
  - `reason: "trial boot reverted by MCUboot"`
  - `from_v: "1.9.34"`
  - `target_v: "1.9.35"`
3. The raw JSON `dfu.user` object shows:
  - `file: "TankAlarm-Client-secure-v1.9.35.slot$20260618000502.bin"`
  - `length: 487741`
  - `read: 487741`
  - `dl_complete: true`
  - `mode: "completed"`
  - `status: "rollback detected - trial crashed - reverted to 1.9.34"`
  - `dfu_started: 1`, `dfu_completed: 2`
4. Later `telemetry.qo`, `daily.qo`, `alarm.qo`, and `diag.qo` events still report `fv: "1.9.34"`.

Important interpretation: Notehub's `mode:"completed"` here does not prove MCUboot swapped. It proves the host reported completion to the Notecard/Notehub layer. The running firmware then generated our rollback text because the post-reset application was still v1.9.34.

### 15.2 Release Workflow Ground Truth

The terminal/git view of `.github/workflows/release-firmware-112025.yml` confirms the current workflow no longer uses the old pip `imgtool` monkeypatch path. The Client secure artifact is produced by compiling with:

```text
--fqbn arduino:mbed_opta:opta:security=sien
--build-property "build.extra_flags=-DTANKALARM_DFU_MCUBOOT"
--build-property "build.version=<version>+0"
```

That means the stale editor/read-cache hazard noted earlier is real. Future reviewers should continue using terminal/git output as the source of truth for this workflow.

### 15.3 Current Working-Tree Code Review: Partial Patch Is Helpful but Incomplete

The current working tree already contains a partial OTA cleanup attempt in `TankAlarm_DFU.h`:

1. `tankalarm_otaFsMount()` / `tankalarm_otaFsUnmount()` centralize `/fs_ota` access and use `mbed::BlockDevice::get_default_instance()` instead of constructing another `QSPIFBlockDevice` on the same pins. This is a good direction. It reduces the risk of multiple QSPI drivers fighting for the same physical flash.
2. The failure path now calls `tankalarm_otaFsUnmount()` before restoring normal operation. Also good.
3. `pending_ota.json` now carries an `attempts` counter, and rollback detection retries `MCUboot::applyUpdate(false)` before blacklisting. The concept is useful, but the placement is still unsafe.

The remaining gap is decisive: the success path still does not unmount `/fs_ota` before calling `MCUboot::applyUpdate(false)`. In the current code, after `pending_ota.json` is written, the FAT filesystem remains mounted at `fs_ota`; then the code sends `dfu.status`, restores `hub.set`, prints `STAGED * TRIGGERING SWAP`, and calls `MCUboot::applyUpdate(false)` while partition 2 is still mounted by the application.

The retry path has the same bug. In `tankalarm_resolvePendingOta()`, the code mounts `/fs_ota`, reads and rewrites `pending_ota.json`, then calls `MCUboot::applyUpdate(false)` and resets before reaching the normal `tankalarm_otaFsUnmount()` at the bottom of the function. If the root failure is the still-mounted partition, every retry repeats the same silent failure.

There is also a minor hygiene issue in the current serial string: the intended dash in `swap did NOT occur` renders as mojibake (`â€”`) in the terminal. Clean this to plain ASCII before committing the patch.

### 15.4 Why This Still Matches the Arduino Core Failure Mode

The installed Arduino core makes the silent-failure path concrete:

1. `MCUboot::applyUpdate(false)` is only:
  ```cpp
  boot_set_pending(0);
  ```
  It returns `void`, so the application cannot see a failure.
2. `boot_set_pending()` returns a nonzero error if it cannot open/read/write the secondary slot trailer, but Arduino's wrapper drops that result.
3. The core's `secondary_bd.cpp` opens the secondary slot through a separate mount prefix: `FATFileSystem fs("fs")` and `FileBlockDevice("/fs/update.bin", "rb+", ...)`.
4. The core comment says mount can fail if the filesystem is already mounted. The code then still returns the filesystem object, so the visible failure can be delayed until `FileBlockDevice::init()` tries to `fopen("/fs/update.bin", "rb+")` and returns `BD_ERROR_DEVICE_ERROR`.
5. Because the application has already closed `fp` but has not unmounted `fs_ota`, this is exactly the window where `boot_set_pending()` can fail without any application log.

This explains the observed pattern cleanly: `update.bin` is valid, `pending_ota.json` says `trial`, Notecard is told the host completed, the MCU resets, the bootloader sees no pending trailer, v1.9.34 boots again, and the application mislabels the unchanged sequence as a trial rollback.

### 15.5 Updated Fix Recommendation

For the next code pass, make the smallest patch that closes the handoff gap before changing anything else:

1. In `tankalarm_performMcubootUpdate()`, immediately after closing `pending_ota.json` successfully, call `tankalarm_otaFsUnmount()` before any `dfu.status {stop}` / `hub.set` / `MCUboot::applyUpdate(false)` handoff. Log that the partition was released.
2. In `tankalarm_resolvePendingOta()`, before the retry call to `MCUboot::applyUpdate(false)`, close any open file and call `tankalarm_otaFsUnmount()`. Do not reset while `/fs_ota` is still mounted.
3. In every early return from `tankalarm_resolvePendingOta()` after a successful mount, ensure `tankalarm_otaFsUnmount()` happens first. Most paths already do this, but the retry path currently does not.
4. Keep the attempts counter, but treat it as retry protection after the trailer scheduling path is fixed. It cannot repair a deterministic trailer-write failure if each retry repeats the same mounted-filesystem state.
5. If feasible, replace the Arduino `MCUboot::applyUpdate(false)` wrapper with a small local scheduling helper that calls `boot_set_pending(0)` directly and checks the return code. Only send `dfu.status {stop:true,status:"staged for MCUboot"}` after the trailer write succeeds. On nonzero return, report `ota-stage-failed` / `firmware update failed: boot_set_pending rc=<n>` and leave enough diagnostics to distinguish a scheduling failure from a bootloader rejection.

The fifth item is not required for the minimal patch, but it would remove the current blind spot entirely. The core already exposes the return code; the application is just not using it because Arduino's convenience wrapper hides it.

### 15.6 Recommended Bench Sequence After the Patch

Use a fresh version because v1.9.35 is already blacklisted on this device.

1. Apply only the unmount-before-schedule fix plus the retry-path unmount fix.
2. Build the Client with `security=sien`, `-DTANKALARM_DFU_MCUBOOT`, and a fresh version such as v1.9.36 or later.
3. Clear the false blacklist by deleting `/fs_ota/pending_ota.json` or by re-running provisioning if that is easier on the bench.
4. Capture `STM32H747_getBootloaderInfo` on the exact unit before the OTA attempt. This remains important because a stock/incorrect bootloader would produce the same final symptom after the trailer is correctly written.
5. Run the OTA while capturing serial at 115200 from before `STAGED * TRIGGERING SWAP` through the reboot.
6. Immediately export Notehub Recent and raw JSON after the attempt.

Expected result if the filesystem handoff is the root cause: after the unmount fix, the next fresh-version OTA should either boot the new firmware or produce a true MCUboot bootloader validation message on serial. If it still reboots silently into the old version, the leading hypothesis shifts back to bootloader identity / secure baseline state.

### 15.7 Bottom Line

The new evidence does not point to a bad v1.9.35 image. It points to the application still failing to schedule the MCUboot trial swap. The current partial patch improves filesystem ownership and retry handling, but it still calls `MCUboot::applyUpdate(false)` while `/fs_ota` is mounted in both the first staging path and the retry path. The next fix should be narrowly focused on releasing the FAT mount before trailer scheduling, then proving that single delta with a fresh OTA attempt and serial boot capture.

---

## 16. Independent Analysis — Code-Path Verification and Structural Issues (Copilot Claude Opus 4.6, June 19 2026)

This section provides an independent analysis performed by verifying the *installed* Arduino core source on disk, cross-referencing the application code, and tracing the exact failure chain. No code changes were made.

### 16.1 Arduino Core Source Verification (On-Disk Confirmed)

The following files were read directly from `%LOCALAPPDATA%\Arduino15\packages\arduino\hardware\mbed_opta\4.5.0\libraries\MCUboot\src\`:

**`MCUboot.cpp`** — `applyUpdate()` is `void`, silently discards the return code from `boot_set_pending()`:
```cpp
void MCUboot::applyUpdate(bool permanent) {
    boot_set_pending(permanent ? 1 : 0);  // returns int, but void wrapper drops it
}
```

**`flash_map_backend/secondary_bd.cpp`** — `get_filesystem()` attempts a second mount and knowingly ignores failure:
```cpp
mbed::FATFileSystem* get_filesystem(void) {
    mbed::BlockDevice* raw = mbed::BlockDevice::get_default_instance();
    static mbed::MBRBlockDevice mbr = mbed::MBRBlockDevice(raw, 2);
    static mbed::FATFileSystem fs("fs");
    int err = mbr.init();
    if(err) { return nullptr; }
    // "Mount can fail if filesystem already mounted"
    fs.mount(&mbr);              // <-- return value ignored
    return &fs;                  // <-- returns &fs even on mount failure
}
```

**`flash_map_backend/FileBlockDevice.cpp`** — `init()` fails if `fopen` returns NULL:
```cpp
int FileBlockDevice::init() {
    _fp = fopen(_path, _oflag);  // _path = "/fs/update.bin"
    if(_fp == NULL) {
        MCUBOOT_LOG_ERR("Cannot open file block device");
        return BD_ERROR_DEVICE_ERROR;
    }
    ...
}
```

**`bootutil/src/bootutil_public.c`** — `boot_set_pending_multi()` returns nonzero on any failure:
```cpp
int boot_set_pending_multi(int image_index, int permanent) {
    rc = flash_area_open(FLASH_AREA_IMAGE_SECONDARY(image_index), &fap);
    if (rc != 0) { return BOOT_EFLASH; }          // <-- this is the likely failure point
    rc = boot_read_swap_state_by_id(FLASH_AREA_IMAGE_SECONDARY(image_index), &state_secondary_slot);
    ...
    rc = boot_write_magic(fap);                    // writes trailer magic
    rc = boot_write_swap_info(fap, ...);           // writes swap type
    flash_area_close(fap);
    return 0;
}
```

The chain is confirmed: if `get_filesystem()` mount fails → `FileBlockDevice::init()` → fopen fails → `flash_area_open()` returns error → `boot_set_pending_multi()` returns `BOOT_EFLASH` → `MCUboot::applyUpdate()` silently drops the error → no swap trailer written → bootloader boots old image → app misclassifies as rollback → permanent blacklist.

### 16.2 The Three QSPI Driver Instances Problem

A previously under-emphasized structural issue: there are potentially **three separate `QSPIFBlockDevice` instances** all targeting the same physical QSPI flash bus:

| # | Owner | Created In | Mount Point |
|---|-------|-----------|-------------|
| 1 | `tankalarm_resolvePendingOta()` | `TankAlarm_DFU.h` L687 | `"fs_ota"` (static, function-scoped) |
| 2 | `tankalarm_performMcubootUpdate()` | `TankAlarm_DFU.h` L908 | `"fs_ota"` (static, block-scoped) |
| 3 | Arduino core `get_filesystem()` | `secondary_bd.cpp` | `"fs"` (static, function-scoped) |

Instance #1 and #2 each declare `static QSPIFBlockDevice qspi_root(QSPI_SO0, ...)` — these are **separate C++ objects**, each independently initializing the QSPI peripheral. Instance #3 comes from `BlockDevice::get_default_instance()`, which is the BSP-defined singleton.

Having multiple QSPI driver instances on the same physical bus is inherently hazardous. Mbed OS's `QSPIFBlockDevice::init()` configures the QSPI peripheral registers. If two instances both `init()` against the same hardware peripheral, one can corrupt the other's bus state. This could manifest as:
- Silent read/write failures on the flash
- Partial or corrupted sector writes
- SPI command interleaving if both are active concurrently

**In the failure scenario**, instance #2 is active (the app's block-scoped QSPI for `fs_ota`), and then instance #3 is created by the core's `get_filesystem()`. The core's `mbr.init()` → `raw->init()` would try to re-initialize the QSPI peripheral, potentially disrupting instance #2's cached state.

### 16.3 Dual `FATFileSystem("fs_ota")` Registration Conflict

Both `tankalarm_resolvePendingOta()` (L689) and `tankalarm_performMcubootUpdate()` (L909) declare:
```cpp
static mbed::FATFileSystem ota_data_fs("fs_ota");
```

These are **different static objects** (different function scopes) that both attempt to register the VFS mount point name `"fs_ota"`. In Mbed OS, the `FATFileSystem` constructor calls `FileSystem("fs_ota")` → `FileBase("fs_ota", ...)`, which adds the name to a global linked list.

If `tankalarm_resolvePendingOta()` is called first (e.g., during `setup()`), it registers `"fs_ota"` via its own static `FATFileSystem`. When `tankalarm_performMcubootUpdate()` later constructs its own static `FATFileSystem("fs_ota")`, the second registration may silently shadow or conflict with the first. This creates an ambiguous VFS state where `fopen("/fs_ota/update.bin", ...)` may resolve to the wrong `FATFileSystem` instance — or to a stale one that was mounted on a different block device hierarchy.

This is a latent defect independent of the primary mount conflict. It should be addressed in the fix by ensuring only ONE `FATFileSystem` instance manages partition 2 across the entire application lifecycle.

### 16.4 Block-Scope Static: Why the Fix Requires Refactoring

The block scope at `TankAlarm_DFU.h` L906–930:
```cpp
  {   // L906 — block scope opens
    static QSPIFBlockDevice qspi_root(...);
    static mbed::MBRBlockDevice ota_data(&qspi_root, 2);
    static mbed::FATFileSystem ota_data_fs("fs_ota");
    
    int err = ota_data_fs.mount(&ota_data);
    ...
    fp = fopen("/fs_ota/update.bin", "r+b");
    ...
  }   // L930 — block scope closes. ota_data_fs is UNREACHABLE but STILL MOUNTED.
```

After L930, the `FILE* fp` handle (declared outside the scope) remains valid, and the mount persists in the VFS because the `static` objects are never destroyed. But the symbol `ota_data_fs` is no longer in scope, so `ota_data_fs.unmount()` cannot be called between file close (L1063) and `MCUboot::applyUpdate(false)` (L1120).

This is a structural constraint: the fix cannot simply "add an unmount call before applyUpdate" without either (a) moving the static declarations to a wider scope, or (b) extracting the filesystem lifecycle into a separate function that returns a handle or (c) restructuring the block scopes.

### 16.5 QSPI Cache Flush Before Reset — A Secondary Concern

Even if the mount conflict is resolved and the trailer write succeeds, there is a timing concern at the reset boundary:

```
L1120:  MCUboot::applyUpdate(false);
L1121:  Serial.println("Rebooting now...");
L1122:  delay(500);
L1123:  NVIC_SystemReset();
```

`MCUboot::applyUpdate()` writes through `FileBlockDevice::program()`, which calls `fwrite()` + `fflush()`. But `fflush()` guarantees only the C runtime buffer is pushed to the filesystem layer, not that the QSPI flash's internal write buffer has fully committed. Between `applyUpdate()` and `NVIC_SystemReset()`, there is a 500ms `delay()`, which should be sufficient for QSPI flash programming latency (typically <10ms per page). However, the FAT filesystem metadata (updated cluster chain, directory entries) may have its own write-back cache in FatFs that isn't explicitly synced. And critically, the `FATFileSystem` is never unmounted, so its internal cache state is uncertain at reset time.

The safest fix sequence before `NVIC_SystemReset()` would be:
1. `fclose(fp)` — flush and close the update.bin file
2. `ota_data_fs.unmount()` — flush FAT metadata, release QSPI
3. `MCUboot::applyUpdate(false)` — core mounts its own "fs", writes trailer, closes
4. `NVIC_SystemReset()` — clean reset

This ordering ensures the core's `get_filesystem()` has exclusive access to the QSPI bus and partition.

### 16.6 The `mcuboot_restore_hub` Cleanup Gap

The error-handling goto target `mcuboot_restore_hub` (L1125+) restores the Notecard hub mode but **does not unmount** `ota_data_fs`. If any error occurs during staging and the function hits this cleanup path, the `"fs_ota"` mount persists in the VFS. If `tankalarm_performMcubootUpdate()` is called again in the same power cycle (e.g., a retry), the second call's `ota_data_fs.mount()` would fail because the mount point is already active from the first attempt.

This creates a "one-shot" behavior: the first OTA attempt can mount `"fs_ota"`, but any retry within the same power cycle silently fails at mount time. The serial log would show `"MCUboot DFU: Failed to mount MBR2 FAT filesystem"` on the retry, which could be confused with a provisioning issue.

### 16.7 Recommendations — Prioritized Fix List

Based on verified code-path analysis:

**Priority 1 — Eliminate the filesystem mount conflict (root cause fix)**
- Move the QSPI/MBR/FAT static declarations out of the block scope into function scope (or a module-level singleton).
- After `fclose(fp)` and writing `pending_ota.json`, explicitly call `ota_data_fs.unmount()` before `MCUboot::applyUpdate(false)`.
- This ensures the core's `get_filesystem()` → `fs.mount(&mbr)` → `fopen("/fs/update.bin", "rb+")` succeeds because no competing mount exists.

**Priority 2 — Eliminate the triple QSPI driver instances**
- All QSPI/MBR/FAT access across `resolvePendingOta`, `isVersionBlacklisted`, and `performMcubootUpdate` should use a shared singleton, not three separate `static QSPIFBlockDevice` declarations. This prevents QSPI bus contention and VFS name collisions.
- Consider using `BlockDevice::get_default_instance()` consistently (the same instance the Arduino core uses), wrapping it with a shared `MBRBlockDevice` for partition 2.

**Priority 3 — Add `applyUpdate` error detection**
- `MCUboot::applyUpdate()` is `void` in the Arduino core, so the return code from `boot_set_pending()` cannot be captured without modifying the core. Options:
  - (a) Verify the trailer was written by re-reading the secondary slot's last page after `applyUpdate()` and checking for the MCUboot magic bytes (`0x77c29504`). If not found, log an error and do NOT reset.
  - (b) Fork/patch the local Arduino MCUboot library to expose the return code (add a `bool tryApplyUpdate(bool permanent)` wrapper).
  - Option (a) is preferred because it works without core modifications.

**Priority 4 — Add retry counter to blacklist logic**
- `tankalarm_resolvePendingOta()` permanently blacklists a version on the first mismatch. It cannot distinguish "swap never happened" from "swap happened but app crashed during trial". A retry counter (e.g., allow 2–3 attempts before blacklisting) would prevent false-positive permanent blocks.
- Consider adding a `"retry_count"` field to `pending_ota.json` and only writing `"failed_rollback"` when retries are exhausted.

**Priority 5 — Add remote blacklist clearing mechanism**
- Once a version is blacklisted, there is currently no way to clear it without physical access (delete the file or re-provision). Adding a Notecard environment variable (e.g., `dfu_clear_blacklist=true`) that triggers deletion of the blacklist entry would allow remote recovery from false-positive blacklists.

### 16.8 Recommended Diagnostic — Zero-Code-Change Verification

Before implementing any fix, this bench test can confirm the hypothesis without modifying application code:

1. **Manually pre-mount test**: Write a minimal test sketch that:
   - Mounts partition 2 as `FATFileSystem("fs_ota")`
   - Calls `MCUboot::applyUpdate(false)` while mounted
   - Captures serial output including `MCUBOOT_LOG_ERR` messages
   - Does NOT reset — just logs the result
   
2. **Compare with clean mount test**: Same sketch but:
   - Mounts partition 2, writes a file, unmounts
   - Then calls `MCUboot::applyUpdate(false)`
   
3. **Read the trailer directly**: After each test, read the last 512 bytes of `/fs/update.bin` (or the equivalent raw QSPI offset at `update.bin offset + MCUBOOT_SLOT_SIZE - 512`) and check for the MCUboot magic bytes `0x77 0xC2 0x95 0x04`. Their presence/absence definitively proves whether the trailer was written.

4. **Bootloader identity verification**: Run `STM32H747_getBootloaderInfo()` on the exact failing unit ("Silas") and capture the version number and identifier string. If the bootloader is not MCUboot Arduino v25+, no amount of trailer writing will help — the bootloader simply doesn't know how to swap.

### 16.9 Summary Assessment

The v1.9.34→v1.9.35 OTA failure is almost certainly caused by the application's `FATFileSystem("fs_ota")` mount blocking the Arduino core's `FATFileSystem("fs")` mount during `MCUboot::applyUpdate()`. The core silently ignores the mount failure, `fopen("/fs/update.bin")` fails, `boot_set_pending()` returns an error, and the `void` wrapper discards it. No swap trailer is written. The bootloader has nothing to act on. The device reboots into the old firmware. The application's rollback-detection logic misinterprets this as a failed trial and permanently blacklists the version.

Three structural issues compound this: (1) block-scoped static declarations make unmounting impossible without refactoring, (2) three separate QSPI driver instances risk bus contention, and (3) dual `FATFileSystem("fs_ota")` registrations create VFS ambiguity across function boundaries.

The fix is narrow and well-defined: unmount `fs_ota` before calling `applyUpdate()`, consolidate QSPI access into a singleton, and verify the trailer was actually written before committing to a reset. The blacklist logic should also be hardened with a retry counter and remote clearing capability.

