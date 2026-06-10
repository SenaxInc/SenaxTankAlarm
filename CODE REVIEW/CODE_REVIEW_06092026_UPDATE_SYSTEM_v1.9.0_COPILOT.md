# Code Review: v1.9.0 Update System and Associated Changes
**Date:** June 9, 2026  
**Reviewer:** GitHub Copilot  
**Version reviewed:** v1.9.0  
**Branch / range reviewed:** `master` at `076abe6`; primary delta from `v1.8.4..HEAD`, with v1.9.0 focus on commits `691bb5f`, `08af85f`, `cc1b497`, `81b5cf6`, and `eb536ea`.

---

## 1. Executive Summary

v1.9.0 introduces a new MCUboot-oriented update architecture on paper and in pieces of the build pipeline: release artifacts are now signed/formatted as MCUboot slot images, a key-provisioning sketch was added, a dormant `tankalarm_performMcubootUpdate()` helper exists, and the Client has an optional `MCUboot::confirmSketch()` health milestone.

The implementation is not yet an integrated MCUboot update system. The shipping build path does not define `TANKALARM_DFU_MCUBOOT`, no production caller invokes `tankalarm_performMcubootUpdate()`, the Client still auto-applies the coordinated ODFU path, and the Server still performs destructive in-place internal-flash updates for both Notehub IAP and GitHub Direct. This creates a dangerous expectation gap: v1.9.0 looks like it has moved to MCUboot/slot-based recovery, but the active runtime update paths remain mostly pre-MCUboot.

**Recommendation:** Do not treat v1.9.0 as a production-ready MCUboot update release. Keep automatic update policies disabled, do not deploy GitHub Direct auto-install, and do not assign MCUboot slot images to field units until one end-to-end path is selected, wired, bench-proven, and documented.

### Findings Overview

| ID | Severity | Issue |
|---|---|---|
| C1 | Critical | MCUboot update path is compiled out and never called by the active firmware |
| C2 | Critical | Client still auto-triggers ODFU and can hang forever while feeding the watchdog |
| C3 | Critical | Server GitHub Direct still erases/programs the running internal application flash |
| H1 | High | Secure slot artifacts are produced, but runtime discovery/install paths still target raw `.bin` assets |
| H2 | High | Committed private/default MCUboot keys provide no real firmware authenticity |
| H3 | High | MCUboot staging helper lacks validation and can mark partial/padded files as ready |
| H4 | High | MCUboot confirmation exists only for the Client and only behind a disabled macro |
| M1 | Moderate | Release and docs are version-split between 1.9.0 and 1.8.5 |
| M2 | Moderate | Client ODFU RS-485 isolation remains guarded by undefined pin macros |
| M3 | Moderate | Key provisioning can crash on file-open failures while formatting QSPI |

---

## 2. Critical Findings

### C1 - MCUboot update path is not wired into the active product

**Locations:**
- `TankAlarm-112025-Common/src/TankAlarm_DFU.h:658` and `:667` define the MCUboot helper only when `TANKALARM_DFU_MCUBOOT` is set.
- `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:29` and `:1661` guard the MCUboot include/confirm call behind the same macro.
- `.github/workflows/release-firmware-112025.yml` builds the normal sketches without defining `TANKALARM_DFU_MCUBOOT`.

The v1.9.0 release workflow adds signed MCUboot slot artifacts, and `TankAlarm_DFU.h` adds `tankalarm_performMcubootUpdate()`, but the production builds do not enable the macro and no active caller invokes the helper. In the actual Client loop, a detected update still calls `enableDfuMode()` (`Client.ino:2001-2003`), which triggers `card.dfu` ODFU (`Client.ino:3930`) rather than staging a file and calling `MCUboot::applyUpdate(false)`.

**Impact:** v1.9.0 appears to have an MCUboot architecture, but field devices will keep using the older update mechanisms. Operators may upload or release MCUboot slot images expecting rollback semantics, while the running firmware neither downloads those artifacts into the MCUboot slot nor confirms/rolls back them as a system. This is the central release-blocking mismatch.

**Fix:** Pick one shipping update path and make it explicit:
1. If v1.9.0 is meant to be MCUboot, define the build flag in CI, replace Client/Server/Viewer apply paths with `tankalarm_performMcubootUpdate()`, and prove the QSPI `/fs_ota/update.bin` + `MCUboot::applyUpdate(false)` flow on hardware.
2. If MCUboot is still experimental, remove the v1.9.0 release claim/artifacts from the production workflow and keep the code behind an obviously experimental flag until the bench gate passes.

---

### C2 - Client auto-ODFU still has an indefinite watchdog-fed hang

**Locations:**
- `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:2001-2003`
- `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:3930-3955`

The Client automatically calls `enableDfuMode()` whenever `gDfuUpdateAvailable` is true and power state is no worse than ECO. That path triggers `card.dfu {"name":"stm32","on":true}` and then enters an infinite loop:

```cpp
while (true) {
  #ifdef TANKALARM_WATCHDOG_AVAILABLE
    mbedWatchdog.kick();
  #endif
  delay(100);
}
```

This is still the v1.8.5 coordinated ODFU issue: if the Notecard accepts the request but does not physically reset the Opta, the application stops monitoring permanently while also preventing the watchdog from recovering it.

**Impact:** A remote client can silently stop sampling, alarming, relaying, and reporting during an update attempt. Because the loop feeds the watchdog, normal recovery is deliberately disabled.

**Fix:** Remove the infinite loop. Restore the original non-blocking `card.dfu` trigger behavior, or use a bounded wait that restores RS-485/SolarManager and stops feeding the watchdog on timeout. Add a config/policy gate so clients do not auto-apply firmware solely because Notehub has a ready update.

---

### C3 - Server GitHub Direct still destructively rewrites the running app flash

**Locations:**
- `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:3868`
- `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:3889`
- `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:3963`

`attemptGitHubDirectInstall()` downloads the GitHub asset over TLS, but it erases and programs the internal application region starting at `flashStart + 0x40000UL` while the current Server firmware is still running. The SHA-256 check is performed as the stream is written, and the flash verification happens only after the image has already replaced the application area.

This is the exact single-image destructive pattern the v1.9.0 MCUboot/Track B work is supposed to avoid. It also verifies TLS with `MBEDTLS_SSL_VERIFY_NONE` (`Server.ino:3768`), so the content-integrity story depends entirely on release metadata being authentic and present.

**Impact:** A power loss, TLS/socket stall, digest mismatch, flash error, or execution from an erased/programmed sector can leave the Server without a valid application. Even on a digest mismatch, the code has already erased/programmed the live flash before it can decide not to reboot.

**Fix:** GitHub Direct should stage the complete asset to QSPI, verify the full digest before touching internal flash, and then either hand it to MCUboot or perform the shortest possible verified local copy. For the MCUboot design, it should consume the secure slot image and call the same shared staging/swap function as Notehub updates.

---

## 3. High Findings

### H1 - Release artifacts and runtime asset discovery disagree

**Locations:**
- `.github/workflows/release-firmware-112025.yml:160`, `:173`, `:186`, `:197-199`
- `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:3050`

The workflow publishes both raw images (`TankAlarm-Server-vX.Y.Z.bin`) and secure MCUboot slot images (`TankAlarm-Server-secure-vX.Y.Z.slot.bin`). The Server GitHub discovery path still searches for the raw `TankAlarm-Server-v%s.bin` asset and installs that raw image. It never selects the secure slot artifact, despite the v1.9.0 release workflow creating it.

**Impact:** Operators and automation now have two plausible artifacts per component. The active direct-install path chooses the one that bypasses the new MCUboot packaging. If the device has been provisioned for signed/encrypted MCUboot images, the raw asset is also the wrong artifact for that boot chain.

**Fix:** Publish only the artifacts that each runtime path can safely consume, or make the runtime explicitly choose and validate `*-secure-vX.Y.Z.slot.bin` for MCUboot-enabled devices. Add target/image metadata validation before any install button is enabled.

---

### H2 - Private/default MCUboot keys are committed to the repository

**Locations:**
- `mcuboot_keys/ecdsa-p256-signing-priv-key.pem`
- `mcuboot_keys/ecdsa-p256-encrypt-priv-key.pem`
- `.github/workflows/release-firmware-112025.yml:151`, `:164`, `:177`

The release workflow signs images using private keys stored in the repo. The provisioning sketch explicitly describes these as baseline/default keys.

**Impact:** This can still be useful as a mechanical rollback experiment, but it should not be described as secure firmware authenticity. Anyone with access to the repository can sign and encrypt an image that the provisioned bootloader accepts.

**Fix:** For production authenticity, move signing private keys to protected CI secrets or an offline release process, provision only the public verification material to devices, and rotate field devices away from committed/default keys. If the project intentionally wants public/default keys for recovery testing, label the mode as non-authenticating rollback only.

---

### H3 - MCUboot staging helper does not validate what it stages

**Locations:**
- `TankAlarm-112025-Common/src/TankAlarm_DFU.h:667-890`

`tankalarm_performMcubootUpdate()` streams bytes from `dfu.get` into `/fs_ota/update.bin`, pads the rest of the 0x1E0000 slot with `0xFF`, and calls `MCUboot::applyUpdate(false)`. The helper calculates `downloadCrc`, but does not compare it to any expected CRC or SHA. It does not validate the MCUboot header, magic, target type, signature/encryption metadata, image version, or component identity before requesting the swap. The padding loop also breaks on write failure but continues into the success path.

**Impact:** Once the macro is enabled and the helper is called, a truncated, wrong-target, raw, corrupt, or partially padded file can be marked for MCUboot testing. MCUboot may reject it, but the application has already cleared Notecard DFU state as `staged for MCUboot`, making diagnostics and retries harder.

**Fix:** Treat staging as a strict transaction: validate expected length, SHA-256/digest, MCUboot magic/header, component target, version policy, and every `fseek`/`fwrite` result. Only call `MCUboot::applyUpdate(false)` after a complete file-level verification passes.

---

### H4 - Health confirmation is Client-only and disabled by default

**Location:** `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:1661-1666`

The only `MCUboot::confirmSketch()` call is in the Client sketch and only when `TANKALARM_DFU_MCUBOOT` is defined. The release workflow signs Client, Server, and Viewer slot images, but Server and Viewer have no equivalent confirmation milestone.

**Impact:** If MCUboot is later enabled uniformly, Server and Viewer trial images may never be confirmed. If MCUboot is not enabled, the signed artifacts are currently decorative rather than operational.

**Fix:** Implement a shared per-sketch health-milestone hook for Client, Server, and Viewer. Confirmation should happen only after each role reaches its real safe-running point: storage mounted, config loaded, safety outputs initialized, network/Notecard health established or queued telemetry accepted.

---

## 4. Moderate Findings

### M1 - Version metadata is split between 1.9.0 and 1.8.5

**Locations:**
- `TankAlarm-112025-Common/src/TankAlarm_Common.h:19`
- `TankAlarm-112025-Common/library.properties:2`
- `README.md:1`, `README.md:4`, `README.md:367`
- `TankAlarm-112025-BillOfMaterials.md:3`

The firmware header is bumped to `1.9.0`, but the common library metadata, README, and BOM still say `1.8.5`. This repo has historically treated both `TankAlarm_Common.h` and `library.properties` as version sources, so this split is likely accidental.

**Impact:** Release validation uses the header, but local library packaging, `TankAlarm-112025-Common.zip`, user docs, and deployment instructions can all advertise the wrong version.

**Fix:** Update `library.properties`, top-level docs, BOM, and generated common ZIP in the same version bump commit as `FIRMWARE_VERSION`.

---

### M2 - Client RS-485 isolation is still mostly dead code

**Location:** `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:3905-3916`

The ODFU quieting sequence drives `PIN_RS485_DE`, `PIN_RS485_RE`, `PIN_RS485_TX`, and `PIN_RS485_RX` only if those macros are defined. I did not find definitions for those macros in the workspace. Even if they are later defined, the code drives DE/RE low and then immediately changes them to inputs, which can float the transceiver enables.

**Impact:** The Client may not actually silence the RS-485 bus before `card.dfu`, preserving the bootloader collision mode that coordinated ODFU was meant to prevent.

**Fix:** Use documented Opta RS-485 control APIs or define verified pin names with a compile-time guard. Hold DE disabled for the entire handoff instead of floating it.

---

### M3 - Provisioning file creation lacks null checks

**Locations:**
- `TankAlarm-112025-KeyProvisioning/TankAlarm-112025-KeyProvisioning.ino:57`
- `TankAlarm-112025-KeyProvisioning/TankAlarm-112025-KeyProvisioning.ino:76`

`setupMCUBootOTAData()` calls `fopen()` for `/fs/scratch.bin` and `/fs/update.bin` and then writes immediately. If `fopen()` fails after a bad format, exhausted QSPI, or mount issue, the sketch will pass a null `FILE*` to `fwrite()`.

**Impact:** A provisioning failure can crash or hang without a clear serial diagnostic, which is exactly when operators need a precise error.

**Fix:** Check both `fopen()` results, print the failing path and errno-style code if available, and abort provisioning before writing keys if QSPI staging cannot be prepared.

---

## 5. Associated Change Notes

- The normal Client and Server sketches compiled locally with Arduino CLI 1.4.1 and `arduino:mbed_opta` 4.5.0.
- The Client also compiled locally with `-DTANKALARM_DFU_MCUBOOT`, and the new KeyProvisioning sketch compiled locally.
- There is currently no `v1.9.0` tag in the local tag list; `v1.8.4` is the latest tag present. The release workflow will not run the tag validation path until `v1.9.0` is tagged.
- The Server update policy defaults to disabled (`UPDATE_POLICY_DISABLED`), which is a good safety default while these paths are unsettled.
- The workflow validates tag version against `FIRMWARE_VERSION`, which is good, but it does not appear to validate that the MCUboot image format is accepted by the runtime updater or provisioned bootloader.

---

## 6. What Is Good

- The direction is right: staging to QSPI and using MCUboot trial/confirm semantics is the right family of solution for reducing field-update bricking risk.
- The release workflow now produces explicit component-named artifacts and tag/version validation.
- The Server GitHub Direct path includes size and SHA-256 checks, which are useful once the write target is changed from live app flash to a staging area.
- The provisioning sketch gives an explicit operator prompt before destroying QSPI contents.
- The normal build remains green locally, so remediation can be focused on behavior and release integration rather than basic compile repair.

---

## 7. Recommended Remediation Order

1. Freeze automatic installs for field deployments: keep Server `updatePolicy` disabled, remove Client auto-apply, and require manual staged pilots only.
2. Decide the v1.9.0 truth: either ship the existing IAP/ODFU paths honestly, or fully enable and wire the MCUboot path end to end.
3. Convert GitHub Direct and Notehub apply paths to a single shared staging transaction: QSPI write, full validation, `MCUboot::applyUpdate(false)`, reboot, confirm milestone.
4. Align artifacts: either publish only raw `.bin` files for legacy IAP/ODFU or only signed slot files for MCUboot-enabled devices. Do not leave both as equally plausible production inputs without metadata checks.
5. Replace committed/default signing material with production key handling, or explicitly document it as non-secure mechanical fallback only.
6. Add Server and Viewer health confirmation hooks before enabling MCUboot images for those components.
7. Update `library.properties`, README, BOM, generated common ZIP, and any release docs to `1.9.0` once the release story is accurate.
8. Add hardware bench evidence to the release checklist: provision QSPI, install a known-good slot image, install a deliberately bad image, confirm rollback, and recover from power loss during staging.

---

## 8. Release Readiness Verdict

**Not ready as an MCUboot update-system release.** v1.9.0 has useful building blocks, but the active firmware still uses old update paths and the new release artifacts are not the artifacts the runtime update code installs. The safest next release is either a clearly labeled legacy-update release with MCUboot disabled, or a smaller MCUboot pilot release that wires one component end to end and proves rollback before expanding to the fleet.