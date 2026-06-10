# Code Review: v1.9.0 Update System and Associated Changes (Independent Verification Pass)

**Date:** June 9, 2026
**Reviewer:** GitHub Copilot (Claude Opus 4.8)
**Version reviewed:** v1.9.0
**Branch / range:** `master` @ `076abe6`; delta `v1.8.4..HEAD`, focus on v1.9.0 commits `691bb5f` (Track B CI + Key Provisioning), `08af85f` (`performMcubootUpdate`), `cc1b497` (`confirmSketch` milestone), `81b5cf6` (default-key mechanical fallback), `eb536ea` (version bump to 1.9.0).
**Relationship to prior pass:** This is an independent second-pass review that re-verifies the earlier `CODE_REVIEW_06092026_UPDATE_SYSTEM_v1.9.0_COPILOT.md` against the actual source and the real Arduino toolchain. It **confirms** the earlier high-level verdict (not production-ready) and **corrects one material claim**: the MCUboot path does **not** in fact compile from the workspace source (see C1).

---

## 1. Methodology

This review did not rely on reading alone. Each load-bearing claim was checked against the source and, where it mattered, against the project's actual compiler:

- Read the full v1.9.0 diff of `TankAlarm_DFU.h`, the Client, the Server, `TankAlarm-112025-KeyProvisioning.ino`, the release workflow, and the `mcuboot_keys/` material.
- Reproduced the MCUboot helper's control-flow skeleton and compiled it with the project's own cross-compiler (`arm-none-eabi-g++ 7-2017q4`, `-std=gnu++14`, the variant `cxxflags.txt`, and `-w`).
- Ran two full Client builds with `-DTANKALARM_DFU_MCUBOOT`: one default (which silently used a stale installed copy of the Common library) and one forced onto the workspace source with `--library`.

The compiler evidence is summarized in the Appendix.

---

## 2. Executive Summary

v1.9.0 is an *architecture-in-progress* commit, not a finished update system. It adds the right building blocks — QSPI staging, MCUboot trial/confirm semantics, a key-provisioning sketch, signed slot artifacts in CI, and genuinely good watchdog-hardening of the IAP path — but the runtime update paths that actually ship are still the older ones, and the new MCUboot code is non-compiling dead code.

The most important conclusions:

1. **The MCUboot update path does not build from the workspace source.** `tankalarm_performMcubootUpdate()` contains a `goto` that jumps over the initialization of `FILE* fp`, which is ill-formed C++ and a hard error with the project's own toolchain. A forced-workspace build fails with `exit status 1`. The reason this was previously believed to compile is that `arduino-cli` silently substituted a *stale installed copy* of the Common library that does not contain the function.
2. **The Client's only wired auto-update path is outboard ODFU**, which the repository's own hardware notes establish is physically impossible on a stock Opta + "Wireless for Opta," and it ends in an infinite watchdog-fed loop. Meanwhile the Client downloads firmware over the *working* IAP channel and never calls the hardened `tankalarm_performIapUpdate()` that the Server uses correctly.
3. **The Server's GitHub Direct install still erases and programs the live application flash before full verification, over a TLS connection with certificate verification disabled.**
4. **The signing/provisioning key chain is unproven and likely broken**: the device is provisioned with Arduino *default* keys (headers not even present in the repo) while CI signs with *separate* repo-committed keys.

**Recommendation:** Do not tag or field v1.9.0 as an MCUboot update release. Keep automatic update policies disabled. Pick one update path (IAP is the proven one on this hardware), wire it consistently across Client/Server/Viewer, and bench-prove it before re-introducing MCUboot.

### Findings Overview

| ID | Severity | Issue |
|----|----------|-------|
| C1 | Critical | MCUboot update path does not compile from workspace source; prior "it compiles" was a stale-library artifact |
| C2 | Critical | Client auto-applies outboard ODFU and hangs forever while feeding the watchdog |
| C3 | Critical | Client downloads via IAP but applies via ODFU; the working `tankalarm_performIapUpdate()` is dead code in the Client |
| C4 | Critical | Server GitHub Direct rewrites live app flash before full verification, with TLS verification disabled |
| H1 | High | Provisioned (default) keys and CI signing keys are different, unproven key sets; default key headers absent from repo |
| H2 | High | MCUboot staging performs no image validation (no magic/header/digest/target/version check) |
| H3 | High | Private MCUboot keys are committed to the repo; "default keys" provide no authenticity |
| H4 | High | Release artifacts (raw `.bin` vs secure `.slot.bin`) do not match what the runtime installs; duplicate Viewer asset |
| H5 | High | `confirmSketch()` health milestone is Client-only, unconditional, and behind the disabled macro |
| M1 | Moderate | Version metadata split between 1.9.0 and 1.8.5 |
| M2 | Moderate | MCUboot block placed outside the `TANKALARM_DFU_H` include guard (multi-include / ODR risk) |
| M3 | Moderate | Client RS-485 isolation gated by undefined `PIN_RS485_*` macros, and floats DE/RE |
| M4 | Moderate | Key-provisioning `fopen()` results unchecked before `fwrite()` |
| M5 | Low | FAT mount-point naming inconsistency (`fs` vs `fs_ota`) between provisioning and updater |

---

## 3. Critical Findings

### C1 — The MCUboot update path does not compile from the workspace source

**Location:** `TankAlarm-112025-Common/src/TankAlarm_DFU.h:667` (`tankalarm_performMcubootUpdate`), with the offending jump at `:728` and the bypassed declaration at `:734`.

```cpp
    if (!dfuReady) {
      goto mcuboot_restore_hub;     // line 728  — taken on DFU-mode timeout
    }
  }
  Serial.println(F("MCUboot DFU: DFU mode active"));

  // --- Step 3: Mount FAT filesystem and open update file ---
  FILE* fp = nullptr;               // line 734  — scalar with a non-vacuous initializer
```

The label `mcuboot_restore_hub:` sits at the end of the function, where `fp` is in scope. Jumping to it from line 728 crosses the initialization of `fp`, which is ill-formed under standard C++ ([stmt.dcl]). With the project's own compiler this is a hard error, not a warning:

```
error: jump to label 'mcuboot_restore_hub' [-fpermissive]
note:   crosses initialization of 'FILE* fp'
```

**Why this was believed to compile.** A normal `arduino-cli` build of the Client with `-DTANKALARM_DFU_MCUBOOT` *succeeds*, but only because `arduino-cli` resolved the Common library to a **stale installed copy** (`…/OneDrive/Documents/Arduino/libraries/TankAlarm-112025-Common/src/TankAlarm_DFU.h`, 656 lines, which does **not** contain `tankalarm_performMcubootUpdate`) rather than the workspace copy (917 lines, which does). Passing `--libraries` pointed at the library folder itself, not its parent, so the workspace source was never compiled. When the build is forced onto the workspace source with `--library`, it fails with `exit status 1` on the error above. See Appendix A.

**Impact.** The MCUboot update path has never been compiled from the source under review. It is simultaneously (a) non-building when actually included with the macro and (b) dead code — no production translation unit calls it. Any claim that v1.9.0 "has" an MCUboot updater is not yet true at the source level.

**Fix.** Restructure the function so no `goto` crosses a variable initialization — declare `fp` (and any other crossed locals) before the first `goto`, or replace the `goto`/label cleanup with a small RAII/helper or a single `do { … } while(0)` with status flags. Then add the macro build to CI (forced onto the workspace library) so this can never regress silently. Verify the local toolchain uses the workspace `src/`, not an installed copy.

---

### C2 — Client auto-applies outboard ODFU and then hangs forever feeding the watchdog

**Locations:** `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino:2001-2003` (auto-trigger), `:3872` (`enableDfuMode`), `:3930-3932` (`card.dfu name=stm32 on=true`), `:3946-3952` (infinite loop).

When an update is detected and power state is `<= ECO`, the Client auto-calls `enableDfuMode()`, which requests outboard DFU and, on acceptance, enters:

```cpp
while (true) {
  #ifdef TANKALARM_WATCHDOG_AVAILABLE
    mbedWatchdog.kick();
  #endif
  delay(100);
}
```

Per the repository's own verified hardware notes, outboard ODFU (`card.dfu name=stm32`) cannot reset a stock Opta paired with "Wireless for Opta" because the AUX connector does not route NRST/BOOT0/USART1. The Notecard accepts the request but never resets the host, so the Client spins here permanently **while deliberately feeding the watchdog**, which disables the one recovery mechanism that would otherwise reboot it.

**Impact.** A remote DIN-rail client silently stops sampling, alarming, relaying, and reporting the first time Notehub offers an update — with no watchdog recovery. This is the same class of regression v1.8.6 set out to remove.

**Fix.** Remove the infinite loop and the outboard-ODFU apply path from the Client (see C3 for the correct path). At minimum, never feed the watchdog in a terminal wait, and gate any auto-apply behind an explicit policy.

---

### C3 — Client downloads via IAP but applies via ODFU; the working IAP path is dead code in the Client

**Locations:** Client `:3571` (`tankalarm_enableIapDfu`), `:3827` (`tankalarm_checkDfuStatus`), `:3930` (apply via outboard ODFU). Contrast Server `:8390` (`enableDfuMode`) → `:8432` (`tankalarm_performIapUpdate`).

The Client enables the *user/IAP* DFU channel at startup and detects updates through it — i.e. the Notecard downloads the host image and the Client could self-flash via `dfu.get` + FlashIAP. But the apply step calls outboard ODFU instead of `tankalarm_performIapUpdate()`. The Server does the same operation **correctly**: its `enableDfuMode()` calls `tankalarm_performIapUpdate(notecard, gDfuFirmwareLength, "continuous", dfuKickWatchdog)` and resumes normal operation on failure (no hang).

This is especially wasteful because v1.9.0 *hardened* `tankalarm_performIapUpdate()` (per-sector erase with watchdog kicks between sectors, `TankAlarm_DFU.h:384-405`) — that work is unreachable from the Client.

**Impact.** The Client uses the one path that cannot work on this hardware and ignores the one that does and that was just improved. Two sketches that share a function name (`enableDfuMode`) implement opposite behaviors, which is a maintenance hazard.

**Fix.** Make the Client's `enableDfuMode()` mirror the Server's: call `tankalarm_performIapUpdate()` with the watchdog callback, drop the `card.dfu name=stm32` trigger and the infinite loop, and keep the RS-485 quiescing only if/when it is actually needed by IAP (it is not).

---

### C4 — Server GitHub Direct rewrites the live application flash before full verification, with TLS verification disabled

**Locations:** `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino:3697` (`attemptGitHubDirectInstall`), `:3768` (`MBEDTLS_SSL_VERIFY_NONE`), `:3868` (`appStart = flashStart + 0x40000UL`), `:3889` (`flash.erase(appStart, …)`), `:3963` (`flash.program(...)` in the receive loop), `:4013` (download SHA-256 compared *after* programming).

`attemptGitHubDirectInstall()` opens the TLS socket with certificate verification disabled, then erases the running application region and streams the asset straight into it with `flash.program()`. The download digest is only compared to the expected SHA-256 *after* the live region has already been overwritten; the flash read-back verification happens later still.

**Impact.**
- *Integrity:* with `MBEDTLS_SSL_VERIFY_NONE`, a network attacker who can MITM the GitHub asset connection can serve arbitrary firmware. The only remaining gate is the expected SHA-256, which is itself obtained from GitHub metadata over equally unverified transport — so a MITM can supply both the malicious image and its matching digest (OWASP A02/A08).
- *Availability:* a power loss, socket stall, digest mismatch, or flash error mid-stream leaves the Server with a partially written, non-bootable application. Even on a clean digest mismatch the live flash has already been destroyed.

**Fix.** Stage the complete asset to QSPI, verify the full digest (and, ideally, a signature) *before* touching internal flash, and only then perform a verified swap (MCUboot once C1 is resolved, or a minimal verified local copy). Enable proper TLS certificate validation (pin the GitHub/Fastly chain) so the digest is not the sole integrity control.

---

## 4. High Findings

### H1 — Provisioned keys and CI signing keys are different, unproven key sets; default headers are missing

**Locations:** `TankAlarm-112025-KeyProvisioning/TankAlarm-112025-KeyProvisioning.ino:8-9` (includes `ecdsa-p256-encrypt-key.h` / `ecdsa-p256-signing-key.h`), `:99-100` (burns `enc_priv_key` + `ecdsa_pub_key`); `.github/workflows/release-firmware-112025.yml` (signs with `mcuboot_keys/ecdsa-p256-signing-priv-key.pem`, encrypts with `mcuboot_keys/ecdsa-p256-encrypt-pub-key.pem`).

For MCUboot to accept an image, the device's provisioned **signing-public** key must be the counterpart of CI's **signing-private** key, and the device's **encrypt-private** key must be the counterpart of CI's **encrypt-public** key. The device is provisioned from Arduino *default* key headers, while CI signs with *separate* repo-committed PEMs. There is no evidence in the repo that these two independently sourced key sets are the same key pairs. If they differ, **every signed/encrypted OTA image will be rejected** by the provisioned bootloader (signature/decrypt failure), producing silent non-updates or trial-then-revert loops.

Additionally, the two default headers (`ecdsa-p256-signing-key.h`, `ecdsa-p256-encrypt-key.h`) are **not present anywhere in the repository**, so `TankAlarm-112025-KeyProvisioning.ino` will not compile from a clean checkout. (The repo's `mcuboot_keys/*.pem` are at least internally consistent: each private PEM embeds the matching public point.)

**Fix.** Make the key chain a single source of truth: provision the public counterpart of the exact key CI signs with, and the private counterpart of the exact key CI encrypts with. Commit (or document the provenance of) the default key headers, and add a CI/bench check that signs a tiny image and confirms the provisioned bootloader accepts it before declaring MCUboot usable.

### H2 — MCUboot staging performs no image validation

**Location:** `TankAlarm_DFU.h:758-862`.

`tankalarm_performMcubootUpdate()` streams `dfu.get` bytes into `/fs_ota/update.bin`, pads the remainder of the `0x1E0000` slot with `0xFF`, and calls `MCUboot::applyUpdate(false)`. It computes a running `downloadCrc` (`:828`, finalized `:858`) but never compares it to an expected value — unlike the IAP path, which does compare CRC against a flash read-back (`:584`). Nothing validates the MCUboot magic/header, target/component identity, version policy, signature, or encryption metadata before requesting the swap. The pad loop also `break`s on a write failure and then proceeds into the success/“staged” path:

```cpp
while(offset < 0x1E0000) {
  ...
  if(fwrite(progBuf, 1, padAmt, fp) != padAmt) break;  // failure falls through to success
  offset += padAmt;
}
```

**Impact.** Once enabled, a truncated, wrong-target, raw (unsigned), or partially written file can be marked “staged for MCUboot,” after which Notecard DFU state is cleared, making diagnosis and retry harder. MCUboot may reject it on boot, but the application has already declared success.

**Fix.** Treat staging as a strict transaction: verify expected length, digest, MCUboot magic/header, component target, and version; check every `fseek`/`fwrite`; and only call `applyUpdate(false)` after a complete file-level verification.

### H3 — Private MCUboot keys are committed; "default keys" are non-authenticating

**Locations:** `mcuboot_keys/ecdsa-p256-signing-priv-key.pem`, `mcuboot_keys/ecdsa-p256-encrypt-priv-key.pem`; provisioning comment "baseline Arduino Public Signing and Private Encrypt keys."

Signing private keys live in the repository, and the device is intentionally provisioned with well-known default keys. This is acceptable as a *mechanical rollback* experiment but provides no firmware authenticity: anyone with repo access (or knowledge of the public defaults) can produce an image the bootloader accepts.

**Fix.** Move signing private keys to protected CI secrets / an offline release process, provision only public verification material to devices, and clearly label any default-key mode as "non-authenticating rollback only."

### H4 — Release artifacts do not match what the runtime installs; duplicate Viewer asset

**Locations:** `.github/workflows/release-firmware-112025.yml` (publishes both `TankAlarm-<role>-v<ver>.bin` and `TankAlarm-<role>-secure-v<ver>.slot.bin`; the Viewer raw `.bin` is listed **twice** in `files:`); Server discovery `TankAlarm-112025-Server-BluesOpta.ino:3050` (installs the raw `TankAlarm-Server-v%s.bin`).

CI now produces signed slot images, but the Server's direct-install path selects the raw `.bin`, and nothing in the runtime consumes the `*.slot.bin`. Operators are presented with two plausible artifacts per component, and the active path chooses the one that bypasses the new MCUboot packaging.

**Fix.** Publish only what each runtime path can safely consume, or make the runtime explicitly select and validate the secure slot artifact for MCUboot-enabled devices. Remove the duplicate Viewer entry.

### H5 — Health confirmation is Client-only, unconditional, and behind the disabled macro

**Location:** Client `:1659-1666`.

`MCUboot::confirmSketch()` is called unconditionally at the end of `setup()` (so any boot that merely reaches end-of-setup is "confirmed," even if Notecard telemetry failed), only in the Client, and only when `TANKALARM_DFU_MCUBOOT` is defined. Server and Viewer have no confirmation milestone.

**Fix.** Provide a shared per-role health hook that confirms only after the role reaches a real safe-running point (storage mounted, config loaded, safety outputs initialized, network/Notecard healthy or telemetry queued), for Client, Server, and Viewer.

---

## 5. Moderate / Low Findings

- **M1 — Version split.** `TankAlarm_Common.h:19` is `1.9.0`, but `TankAlarm-112025-Common/library.properties:2`, `README.md` (lines 1, 4, 347, 367), and `TankAlarm-112025-BillOfMaterials.md:3` still say `1.8.5`. The release workflow validates the tag against `FIRMWARE_VERSION` only, so the packaged library and docs can ship the wrong version. Update them in the same bump commit.
- **M2 — MCUboot block outside the include guard.** In `TankAlarm_DFU.h`, the `#if defined(TANKALARM_DFU_MCUBOOT) … #endif` block is placed *after* `#endif // TANKALARM_DFU_H`. Because it is not protected by the include guard, any translation unit that includes the header twice would redefine `qspi_root` / `tankalarm_performMcubootUpdate`. Move it inside the guard.
- **M3 — RS-485 isolation is effectively dead and floats the bus.** Client `:3905-3916` drives `PIN_RS485_DE/RE/TX/RX`, but those macros are not defined anywhere in the workspace, so the block compiles to nothing; even when defined, it drives DE/RE low and then immediately sets them to inputs, floating the transceiver enables. (Moot if C3 is fixed, since IAP needs no bus quiescing.)
- **M4 — Provisioning `fopen()` unchecked.** `TankAlarm-112025-KeyProvisioning.ino:57` and `:76` pass a possibly-null `FILE*` to `fwrite()` after a failed format/mount. Check both opens and abort with a clear diagnostic.
- **M5 — Mount-point naming inconsistency.** Provisioning mounts the FAT partition as `fs` and writes `/fs/update.bin`; the updater mounts the same partition as `fs_ota` and writes `/fs_ota/update.bin`. They resolve to the same underlying file, so this is not a bug, but the inconsistent naming is confusing and invites a future real mismatch. Standardize the mount point.

---

## 6. What Is Good

- The IAP path hardening is genuinely valuable: per-sector erase with watchdog kicks between sectors (`TankAlarm_DFU.h:384-405`) directly addresses the long-erase brick window.
- The Server implements Notehub updates correctly via `tankalarm_performIapUpdate()` with graceful failure handling — this is the model the Client should follow.
- Server GitHub Direct includes Content-Length, expected-size, download SHA-256, and flash read-back SHA-256 checks — all useful once the write target moves off the live region.
- `updatePolicy` defaults to Disabled, a sensible safety default while these paths are unsettled.
- The release workflow adds tag-vs-`FIRMWARE_VERSION` validation and component-named artifacts.
- The provisioning sketch prompts before destroying QSPI contents.
- The overall direction — stage to QSPI, use MCUboot trial/confirm — is the right family of solution.

---

## 7. Recommended Remediation Order

1. **Freeze field auto-installs:** keep Server `updatePolicy` disabled and remove the Client auto-apply (C2/C3).
2. **Decide the v1.9.0 truth:** ship the proven IAP/ODFU story honestly, or fully wire MCUboot — not a mix that looks like MCUboot but runs neither.
3. **Fix the Client to use IAP** exactly like the Server (C3); delete the outboard-ODFU apply + infinite loop (C2).
4. **Make the MCUboot helper compile** from the workspace source and add a forced-`--library` macro build to CI so it can't silently rot (C1, M2).
5. **Convert GitHub Direct and Notehub apply to a single staged, fully-verified swap** with TLS verification enabled (C4).
6. **Prove the key chain end to end** (provisioned key == CI key) and move private keys out of the repo (H1, H3); add the missing default key headers or document provenance.
7. **Add real staging validation** (digest/magic/target/version) and per-role confirm hooks (H2, H5).
8. **Align artifacts and versions** (H4, M1) and add bench evidence to the release checklist: provision QSPI, install a good slot image, install a deliberately bad one, confirm rollback, and recover from power loss during staging.

---

## 8. Release Readiness Verdict

**Not ready as an MCUboot update-system release, and not ready to enable automatic updates on the Client.** The new MCUboot code does not compile from the workspace source, the Client's wired auto-update path cannot work on this hardware and hangs, the Server's direct-install path is destructive and unauthenticated, and the signing/provisioning key chain is unproven. The IAP path (correct on the Server, hardened in v1.9.0) is the realistic basis for the next release: wire it into the Client, keep automatic policies off, and gate any MCUboot re-introduction behind a compiling, bench-proven, single component pilot.

---

## Appendix A — Compiler Evidence

All tests used the toolchain `arduino:mbed_opta@4.5.0` ships: `arm-none-eabi-g++ 7-2017q4`, `-std=gnu++14`, the variant `cxxflags.txt` (`-Wall -Wextra`, **no** `-fpermissive`), plus inline `-w`.

1. **Isolated skeleton of the helper's control flow** — the early `goto` + later `FILE* fp = nullptr;`:
   - `… -std=gnu++14` → `error: jump to label 'mcuboot_restore_hub' … crosses initialization of 'FILE* fp'` (exit 1)
   - `… -std=gnu++14 -w` → still **exit 1** (`-w` suppresses warnings, not this error)
   - `… @cxxflags.txt -w` → still **exit 1**
   - `… -std=gnu++14 -fpermissive -w` → **exit 0** (only `-fpermissive` demotes it)
2. **Full Client build, default library resolution**, `--libraries …/TankAlarm-112025-Common`, `-DTANKALARM_DFU_MCUBOOT` → **succeeds** (`Sketch uses 332268 bytes`). The verbose log shows the include path resolving to the *installed* copy `…/OneDrive/Documents/Arduino/libraries/TankAlarm-112025-Common/src` (656 lines, no `tankalarm_performMcubootUpdate`), so the buggy function was never compiled.
3. **Full Client build, workspace library forced**, `--library …/TankAlarm-112025-Common`, `-DTANKALARM_DFU_MCUBOOT` → **fails**: `error: jump to label 'mcuboot_restore_hub' … crosses initialization of 'FILE* fp'` … `Error during build: exit status 1`.

Conclusion: the workspace MCUboot path does not build under the project's own toolchain; prior "it compiles with the macro" results were produced against a stale installed copy of the Common library.
