# Proposed Fixes: v1.9.0 Update System — All-In MCUboot Implementation Plan

**Date:** June 9, 2026
**Author:** GitHub Copilot (Claude Opus 4.8)
**Version targeted:** v1.9.0 → **v1.9.1 (MCUboot)**
**Repository:** SenaxTankAlarm
**Strategic decision (locked):** Go **all-in on MCUboot** for firmware updates. Use the **Arduino default signing/encryption keys**. Keys are used **only for MCUboot mechanical compatibility** — **device firmware security/authenticity is explicitly a non-goal**.

**Source reviews studied (all three):**
- `CODE REVIEW/CODE_REVIEW_06092026_UPDATE_SYSTEM_v1.9.0_COPILOT.md` (first pass)
- `CODE REVIEW/CODE_REVIEW_06092026_UPDATE_SYSTEM_v1.9.0_COPILOT_v2.md` (compiler-verified pass)
- `CODE REVIEW/CODE_REVIEW_06092026_UPDATE_SYSTEM_v1.9.0_COPILOT_FINAL.md` (consolidated pass)

This revision supersedes the earlier two-path (IAP-vs-MCUboot) proposal. The path is now decided: **MCUboot only**. Every fix below is framed for that goal, and the key-management findings are re-scoped to "make MCUboot work," not "make firmware secure."

---

## 0. What This Decision Changes (read first)

Choosing "all-in MCUboot + default keys, no security" **simplifies** the hardest findings from the reviews and **promotes** a few that were optional. The table maps each review finding to its new disposition.

| Finding | Old framing | New disposition under all-in MCUboot + default keys |
|---|---|---|
| C1 | Helper won't compile | **Mandatory blocker** — MCUboot is the only path, it must compile |
| C2/C3 | Client hangs on ODFU; IAP is the fix | **Client applies via MCUboot staging**; delete ODFU + infinite loop |
| C4 | GitHub Direct destroys live flash | **Route GitHub Direct through MCUboot staging** (or drop it) |
| H1 | Provisioned keys ≠ CI keys (critical) | **Largely already solved** — repo CI keys are byte-identical to Arduino defaults; just commit the default headers |
| H2 | No staging validation | **Still required**, and the *component/role* check matters more (shared keys can't tell roles apart) |
| H3 | Private keys committed (security risk) | **Intentional & accepted** — default keys are public; document "no authenticity"; keep them committed |
| H4 | Artifacts don't match runtime | **Publish/install `.slot.bin`**; Notehub gets the slot image; remove duplicate Viewer |
| H5 | Confirm is Client-only/premature | **Required for all three roles**, criteria-gated |
| M1–M5 | Mixed | Mostly unchanged; M2/M5 now mandatory; M3 still deleted |

### Verified facts behind this revision
These were checked directly against the toolchain and the installed Arduino core (`arduino:mbed_opta@4.5.0`):

1. **The repo's `mcuboot_keys/*.pem` are byte-identical (SHA-256 match) to the Arduino default keys** in `…/mbed_opta/4.5.0/libraries/MCUboot/default_keys/`. CI already signs/encrypts with the Arduino defaults.
2. **`TankAlarm-112025-KeyProvisioning.ino` is a direct adaptation of the core's `MCUboot/examples/enableSecurity`** — same key addresses (`SIGNING_KEY_ADDR 0x8000300`, `ENCRYPT_KEY_ADDR 0x8000400`) and same symbols. The default headers it needs (`ecdsa-p256-signing-key.h` defining `ecdsa_pub_key`, `ecdsa-p256-encrypt-key.h` defining `enc_priv_key`) exist in that example but are **not committed** to this repo.
3. **The key roles are already correct:** the device is provisioned with the signing **public** key + encrypt **private** key; CI signs with the signing **private** key + encrypt **public** key. Matching default keypairs ⇒ the bootloader will accept CI-built images.
4. **The slot geometry already lines up:** `imgtool --header-size 0x20000` + core `APPLICATION_SIZE 0x1C0000` = `--slot-size 0x1E0000`; provisioning pre-allocates `update.bin` = `0x1E0000` and `scratch.bin` = `0x20000`; `tankalarm_performMcubootUpdate()` caps/pads to `0x1E0000`. No resizing needed.
5. **MCUboot API present:** `MCUboot::confirmSketch()`, `MCUboot::applyUpdate(bool permanent)`, `MCUboot::bootDebug(bool)`.
6. **`TANKALARM_DFU_MCUBOOT` is currently OFF in CI.** Under this plan it must be **ON** for Client, Server, and Viewer.

**Net effect:** the cryptographic work the reviews called "critical/high" is mostly a non-issue here. The real work is (a) make the helper compile, (b) wire all three roles to the staging+swap path, (c) commit the default headers, (d) validate stages, (e) ship `.slot.bin`, and (f) one-time provision every device.

---

## 1. Security Posture Statement (make it explicit in the repo)

Because this is a deliberate, documented choice, write it down where future maintainers will see it (e.g., `mcuboot_keys/README.md` and the KeyProvisioning header):

> **MCUboot keys in this repository are the public Arduino default keys.** They exist solely so the Arduino MCUboot bootloader will accept our images and so we get **mechanical** benefits: image integrity (hash/magic) checks and **A/B rollback** on a bad boot. They provide **no firmware authenticity**: anyone can sign an image the bootloader accepts. This is an accepted trade-off — device firmware confidentiality/authenticity is explicitly out of scope for this product. Do **not** describe these signatures as a security control.

What we still get from MCUboot with default keys (all valuable, none security):
- **Corruption rejection:** a truncated/garbled image fails the image hash and is not booted.
- **A/B rollback:** a bad image that boots but fails its health gate is automatically reverted.
- **Atomic swap:** the live application is never partially overwritten (no mid-write brick).

What we explicitly do **not** get (and accept):
- Protection against a malicious actor substituting a validly-"signed" image (they can use the same public default key).

---

## 2. Critical Findings — Proposed Fixes

### C1 — MCUboot helper does not compile (`goto` crosses `FILE* fp` initialization)

`TankAlarm_DFU.h:728` jumps to `mcuboot_restore_hub:` crossing `FILE* fp` at `:734`. Hard error under `arm-none-eabi-g++ 7-2017q4 -std=gnu++14`. **Now a hard release blocker** (MCUboot is the only path).

**Option A — Hoist the crossed declaration (minimal).**
```cpp
  FILE* fp = nullptr;          // declare before any goto
  // ... Step 2 DFU-mode wait ...
  if (!dfuReady) { goto mcuboot_restore_hub; }   // legal now
```
- Pros: one-line move, lowest risk. Cons: keeps the `goto` idiom.

**Option B — `do { … } while(0)` + status flag**, single cleanup epilogue. Removes `goto`; larger diff.

**Option C — Extract `mcubootRestoreHub(notecard, hubMode)` and early-`return`.** Cleanest/testable; most refactoring; must close `fp`/free `progBuf` before each return.

**Recommendation:** **Option A** to unblock immediately; adopt **Option C** when the function is next reworked for H2 (validation) since you'll be editing the same body. **Mandatory companion:** the CI macro-build gate in §6 so this never regresses behind a stale installed library again.

---

### C2 / C3 — Client must apply via MCUboot staging (not ODFU), and stop hanging

The Client enables the IAP **download** channel (`tankalarm_enableIapDfu`, `Client.ino:3571`) — keep that; it is how bytes reach the Notecard for `dfu.get`. The defect is the **apply** step: `card.dfu name=stm32` (`:3930`) + `while(true){ kick; delay; }` (`:3946`). Replace the apply body with the MCUboot staging call.

**Chosen fix — Client `enableDfuMode()` → `tankalarm_performMcubootUpdate()`:**
```cpp
static void enableDfuMode() {
  if (gDfuInProgress) { Serial.println(F("DFU already in progress")); return; }
  if (gDfuFirmwareLength == 0) {
    Serial.println(F("ERROR: No firmware length — cannot apply update")); return;
  }
  gDfuInProgress = true;
  if (gConfigDirty) persistConfigIfDirty();           // save state first

  const char *restoreMode = gConfig.solarPowered ? "periodic" : "continuous";

  // Stages signed .slot.bin to QSPI /fs_ota/update.bin, validates, then
  // MCUboot::applyUpdate(false) + NVIC_SystemReset(). Returns only on failure.
  bool ok = tankalarm_performMcubootUpdate(
      notecard, gDfuFirmwareLength, restoreMode, dfuKickWatchdog);

  gDfuInProgress = false;                              // only reached on failure
  if (!ok) {
    Serial.println(F("MCUboot DFU failed — resuming normal operation"));
    if (gConfig.solarCharger.enabled) gSolarManager.begin(gConfig.solarCharger);
  }
}
```
- **Delete** the `card.dfu name=stm32` trigger, the infinite loop, **and** the RS-485 quiescing (M3 — not needed; MCUboot reboots into the bootloader, no bus collision).
- **Add a `dfuKickWatchdog()` wrapper** in the Client (it currently calls `mbedWatchdog.kick()` directly and has no callback; the Server already has `dfuKickWatchdog`):
```cpp
static void dfuKickWatchdog() {
#ifdef TANKALARM_WATCHDOG_AVAILABLE
  mbedWatchdog.kick();
#endif
}
```
- **Gate auto-apply behind a policy flag** so a Client never self-updates merely because Notehub has an image (mirror the Server's `updatePolicy`); default it Disabled until the bench matrix (§7) passes.

---

### C2.5 (new) — Migrate the **Server** apply path to MCUboot too

The reviews praised the Server's `tankalarm_performIapUpdate()` as "the good example." Under all-in MCUboot that is no longer the target: the Server's `enableDfuMode()` (`Server.ino:8390→8432`) must also switch to `tankalarm_performMcubootUpdate()` so the whole fleet uses one mechanism and gets rollback.

**Options:**
- **A (recommended):** Replace the Server's `tankalarm_performIapUpdate(...)` call with `tankalarm_performMcubootUpdate(notecard, gDfuFirmwareLength, "continuous", dfuKickWatchdog)`. Keep all the Server's pre-update state-saving (it already saves config/registry/snapshots — good).
- **B (transitional):** Keep `tankalarm_performIapUpdate()` available behind `#if !defined(TANKALARM_DFU_MCUBOOT)` as a legacy fallback for un-provisioned units, and use MCUboot when the macro is on. Useful only during fleet migration; remove once all devices are provisioned.

**Recommendation:** **A**, with **B** as a short-lived migration aid if some field units can't be provisioned immediately. Note: `tankalarm_performIapUpdate()` becomes dead code once migration completes — delete it then.

---

### C4 — Server GitHub Direct: stop destroying live flash; route through MCUboot staging

`attemptGitHubDirectInstall()` erases/programs the running app region (`:3868/:3889/:3963`) and verifies afterward (`:4013`), with `MBEDTLS_SSL_VERIFY_NONE` (`:3768`). Under all-in MCUboot the destructive write is the real problem; staging removes the brick risk regardless of TLS.

**Download-target options:**
- **A (recommended) — Stage GitHub asset to QSPI, then MCUboot swap.** Reuse the Notehub path's tail: write the downloaded `.slot.bin` to `/fs_ota/update.bin`, validate (H2), `MCUboot::applyUpdate(false)`, reset. The HTTPS download replaces only the byte-source; the staging+swap is shared.
- **B — Drop GitHub Direct entirely; Notehub-only.** Simplest; removes the whole HTTPS/TLS surface. Keep GitHub discovery as a dashboard **alert** only.

**TLS options (low priority given the stated no-security posture):**
- **T1 — Leave `VERIFY_NONE`.** Acceptable under the explicit decision; MCUboot's image-hash check still rejects a corrupted/garbled MITM payload (it just won't reject a *validly default-signed* malicious one — which the policy accepts).
- **T2 — Pin the GitHub/Fastly CA.** Optional hardening if ever desired; costs CA-bundle maintenance. Not required by this plan.

**Recommendation:** **A** if unattended GitHub updates are wanted, else **B**. Either way, **stop programming live flash from the socket.** TLS can stay `VERIFY_NONE` (**T1**) consistent with the no-security decision; note the residual risk in the security-posture statement (§1).

**Shared helper to factor out:** both Notehub and GitHub paths should converge on
```cpp
bool stageSlotToQspi(/* byte source */, uint32_t expectedLen, uint32_t expectedCrc);
bool commitMcubootSlot();   // validate + MCUboot::applyUpdate(false) + reset
```
so validation and swap live in exactly one place.

---

## 3. High Findings — Proposed Fixes

### H1 — Key chain: already on Arduino defaults; just make it compile & be explicit

Because the repo CI keys **are** the Arduino defaults and the provisioning roles are already correct (verified facts #1–#3), there is **no key mismatch to fix**. The only real gaps:

**Fix 1 — Commit the default key headers so KeyProvisioning compiles from a clean checkout.**
Copy the two headers from `…/mbed_opta/4.5.0/libraries/MCUboot/examples/enableSecurity/` into `TankAlarm-112025-KeyProvisioning/`:
- `ecdsa-p256-signing-key.h` (defines `ecdsa_pub_key[]` — the signing **public** key the device verifies with)
- `ecdsa-p256-encrypt-key.h` (defines `enc_priv_key[]` — the encrypt **private** key the device decrypts with)
- Options: **(A)** commit copies into the sketch folder (reproducible, recommended); **(B)** `#include` them from the core path (fragile across core versions/machines); **(C)** generate them in CI from `mcuboot_keys/*.pem` with a small script. **Recommend A**, with a comment noting their origin and that they correspond to `mcuboot_keys/`.

**Fix 2 — Pin/verify key provenance in CI.** Add a CI check that hashes `mcuboot_keys/*.pem` against the installed core's `default_keys/*.pem` (or a vendored copy) so a future core bump that changes defaults can't silently break the chain. Options: hard-fail on mismatch (recommended) vs warn.

**Fix 3 (caveat) — Component/role identity.** Because all three roles share the *same* default signing key, a signature check **cannot** tell a Server image from a Client image. Guard against cross-role installs by: **(A)** keeping each role on its own Notehub product/fleet so the wrong image is never pushed (recommended; operational); **(B)** GitHub Direct selecting by exact filename (`TankAlarm-Server-secure-v…`); and/or **(C)** embedding a role/version marker and checking it during staging (H2). **Recommend A+B**, add C if you want belt-and-suspenders.

**Encryption: keep or drop?** The Arduino default flow signs **and** encrypts. Options: **(A)** keep `--encrypt` with the default encrypt key (matches the default bootloader/provisioning as-is — recommended, least deviation); **(B)** drop `--encrypt` and provision signing-only (fewer moving parts, but only if the provisioned bootloader is configured for unencrypted images — needs bench confirmation). **Recommend A** unless bench testing shows the bootloader is happy signing-only.

---

### H2 — Strict staging validation before `applyUpdate()`

Even with the bootloader as a backstop, validate at stage time to avoid wasted reboots and to catch wrong-role images (which signatures won't, per H1 Fix 3).

**Option A — Full transaction (recommended).** Before `applyUpdate`:
1. Downloaded length == expected `dfu.status` length.
2. `downloadCrc`/SHA-256 == expected from `dfu.status`.
3. MCUboot header **magic** `0x96f3b83d` + sane `hdr_size`/`img_size`.
4. Role/version marker matches this device's role (H1 Fix 3C).
5. **Every** `fseek`/`fwrite` checked; on the pad loop, `goto mcuboot_restore_hub` (abort) instead of `break`-then-succeed.

**Option B — Minimum bar.** Length+CRC compare and fix the pad-loop fall-through; rely on the bootloader for the rest.

**Option C — Bootloader-only.** Fix just the pad-loop fall-through. Cheapest; every bad image costs a reboot and clears Notecard state.

**Recommendation:** **Option A.** **B** is the floor. The pad-loop `break`→abort fix is mandatory in all cases.

---

### H3 — Committed default keys are intentional (document, don't purge)

Under the no-security decision this is **not a vulnerability** — the keys are the public Arduino defaults, identical to what any Opta ships able to accept. Keeping them committed makes builds reproducible and avoids secret management.

**Options:**
- **A (recommended):** Keep `mcuboot_keys/*.pem` committed. Add `mcuboot_keys/README.md` with the §1 posture statement ("default/public keys, mechanical compatibility + rollback only, no authenticity"). Add a CI note that these are intentionally public.
- **B:** If you ever want authenticity later, that's a *future* project: generate a real keypair, move signing-private to CI secrets, rotate provisioned devices. Out of scope now.

**Recommendation:** **A.** Explicitly close H3 as "won't fix — by design," with the documentation in place so it isn't re-flagged.

---

### H4 — Ship and install the `.slot.bin`; fix the duplicate

Under all-in MCUboot the runtime must consume the signed slot image, not the raw `.bin`.

**Fixes:**
1. **Server GitHub discovery** (`Server.ino:3050`) selects `TankAlarm-Server-secure-v%s.slot.bin` (not the raw `.bin`).
2. **Notehub host firmware** upload must be the **`.slot.bin`** for each role (this is what `dfu.get` serves to `performMcubootUpdate`). Document this in the release runbook — it's a manual/automated Notehub step, separate from the GitHub release.
3. **Release `files:`** — remove the **duplicate** `TankAlarm-Viewer-v…bin` line. Keep raw `.bin` and `.with_bootloader.bin` **only** for USB recovery/initial flashing, in a clearly labeled "recovery artifacts" group; they are not OTA inputs.
4. **Optional:** validate target metadata before any install is enabled (ties to H2 role check).

**Options for raw artifacts:** **(A)** keep raw bins published but clearly segregated as "USB recovery only" (recommended — you need them for the one-time provisioning flash); **(B)** stop publishing raw bins entirely (forces all flashing through slot images; harder for bench recovery). **Recommend A.**

---

### H5 — Shared, criteria-gated health confirmation for all three roles

`MCUboot::confirmSketch()` must run only after a role is genuinely healthy, and must exist for Client, Server, **and** Viewer (today only the Client calls it, unconditionally at end of `setup()` — which would auto-rollback Server/Viewer and falsely confirm a half-broken Client).

**Option A — Shared helper, called from each `loop()` after a health gate (recommended).**
```cpp
// TankAlarm_DFU.h (inside the guard)
static inline void tankalarm_markFirmwareHealthy() {
#if defined(TANKALARM_DFU_MCUBOOT)
  static bool confirmed = false;
  if (!confirmed) {
    MCUboot::confirmSketch();
    confirmed = true;
    Serial.println(F("MCUboot: firmware confirmed healthy."));
  }
#endif
}
```
Per-role gate (call once it passes):
- **Client:** storage mounted, config loaded, relays in safe state, **first telemetry queued/acked**.
- **Server:** storage mounted, config loaded, Ethernet up (or queued), web stack serving.
- **Viewer:** storage mounted, config loaded, **first summary received/rendered**.

**Option B — Confirm after first telemetry ack / N stable loops.** Simpler trigger; weaker signal than explicit criteria but far better than end-of-`setup()`.

**Option C — Time-based (X minutes uptime).** Weakest; a broken-but-idle sketch self-confirms.

**Recommendation:** **Option A**, using **first-telemetry-ack** as the concrete trigger inside each gate. Remove the unconditional `setup()` confirm.

---

## 4. Moderate / Low Findings — Proposed Fixes

- **M1 — Version split (1.9.0 vs 1.8.5).** Bump `library.properties:2`, `README.md` (1, 4, 347, 367), `BOM.md:3`, regenerate `TankAlarm-112025-Common.zip` to the release version in one commit. Add the CI version-consistency guard (§6). Target **1.9.1** and bump everything together (including `FIRMWARE_VERSION`, currently `1.9.0`). **Recommend: fix now + CI guard.**
- **M2 — MCUboot block outside the include guard.** Move the `#if defined(TANKALARM_DFU_MCUBOOT) … #endif` block **above** `#endif // TANKALARM_DFU_H`. Also move the file-scope `static QSPIFBlockDevice qspi_root(...)` **inside** the function (or behind its own guard) so multi-inclusion can't double-instantiate the QSPI device. **Now mandatory** (the block is compiled in production). **Recommend A.**
- **M3 — RS-485 quiescing.** **Delete it** — MCUboot reboots into the bootloader; there is no `card.dfu` bus collision to avoid. Removed as part of the C2/C3 rewrite.
- **M4 — KeyProvisioning unchecked `fopen()`.** Check both `fopen()` results (`/fs_ota/scratch.bin`, `/fs_ota/update.bin`), print the failing path, abort before writing keys. Optionally factor a `bool preallocate(path, bytes)` helper. **Recommend: add the null checks.**
- **M5 — Mount-name inconsistency (`fs` vs `fs_ota`).** Standardize on **`fs_ota`** in **both** the provisioning sketch and the updater (they mount the same QSPI partition 2; the labels must agree to avoid confusion). Add a comment that both files must stay identical. **Now relevant** (MCUboot is the live path). **Recommend: standardize on `fs_ota`.**
- **M6 (new) — Verify `imgtool` version string + downgrade policy.** The workflow signs `--version "<ver>.0"` (e.g. `1.9.1.0`). Confirm `imgtool` accepts that 4-field form (it expects `major.minor.revision[+build]`); consider `--version "<ver>+0"` or just `<ver>`. Also confirm the Arduino MCUboot bootloader's **downgrade-prevention** behavior so re-installing the same version during testing isn't silently rejected (relevant since 1.9.0→1.9.1 is a small bump). **Recommend: verify on the bench; adjust the version flag if needed.**

---

## 5. One-Time Device Provisioning (fleet prerequisite — cannot OTA without this)

All-in MCUboot requires every unit to be prepared **once via USB** before any OTA will work. Put this in the deployment runbook:

1. **Flash the MCUboot-capable bootloader** (Arduino `STM32H747_manageBootloader`; provisioning requires bootloader version > 24 and identifier "MCUboot Arduino" — the sketch already checks this).
2. **Run `TankAlarm-112025-KeyProvisioning`** to: burn the default signing-public + encrypt-private keys, and format/partition QSPI (`scratch.bin` 0x20000 + `update.bin` 0x1E0000). This **erases QSPI config** — do it before field configuration.
3. **Flash the signed application** for the role (USB upload of a signed sketch, or the `.with_bootloader` recovery image), so the first running image is already MCUboot-formatted.
4. **Upload the role's `.slot.bin` to its Notehub product/fleet** as host firmware so OTA has something to serve.

**Options to ease this:** **(A)** a documented manual checklist (recommended first); **(B)** a single "provisioning" combined sketch/script that chains bootloader + keys + partition; **(C)** factory pre-provisioning before units ship. Pick per logistics; **A** is enough to start.

---

## 6. CI / Build Gates to Add

- **Turn `-DTANKALARM_DFU_MCUBOOT` ON** for Client, Server, and Viewer production builds (it is currently off).
- **Macro compile job:** build all three sketches with the macro using `--library "$WORKSPACE/TankAlarm-112025-Common"` (force the **workspace** copy — never an installed sketchbook library). This is the gate that would have caught C1.
- **Clean-checkout KeyProvisioning build:** compile `TankAlarm-112025-KeyProvisioning` from a fresh checkout to catch the missing default headers (H1 Fix 1).
- **Key-provenance job:** assert `mcuboot_keys/*.pem` match the vendored Arduino defaults (H1 Fix 2).
- **Artifact-policy job:** assert the release publishes the `.slot.bin` set, raw bins are only in the "recovery" group, and there are **no duplicate filenames** (H4).
- **Version-consistency job:** assert every location in `VERSION_LOCATIONS.md` equals `FIRMWARE_VERSION` (M1).
- **imgtool sanity job:** sign a 1-block dummy and confirm the version/flags parse (M6).

---

## 7. Bench Test Matrix (gate before enabling MCUboot OTA in the field)

Do not enable MCUboot OTA on any field unit until **all** pass on hardware:
1. **Provision → good image:** bootloader + keys + partition; install a CI-built signed `.slot.bin`; confirm boot and that `confirmSketch()` fires **only** after the role health gate.
2. **Corrupt/truncated image:** staging validation (H2) aborts before `applyUpdate`; Notecard state preserved for retry.
3. **Power loss during staging:** pull power mid-download → device still boots prior firmware (QSPI staging, not live flash).
4. **Power loss during swap:** pull power mid-swap → MCUboot recovers/rolls back to a bootable image.
5. **Bad-health rollback:** install an image whose `loop()` never passes its health gate → no confirm → automatic rollback on next boot.
6. **Wrong-role image:** push a Server `.slot.bin` to a Client (or stage it) → rejected by the H2 role check (since the shared default signature will *not* reject it).
7. **Real-artifact key check:** install an actual CI-built artifact (not a locally re-signed one) to prove the committed default keys == bootloader-accepted keys end to end.
8. **Same-version reinstall:** confirm downgrade-prevention doesn't silently block re-flashing the same version during testing (M6).

---

## 8. Implementation Order (single MCUboot track)

1. **C1 (A)** make the helper compile **+ §6 macro CI gate**.
2. **M2** move the MCUboot block inside the guard; relocate `qspi_root`.
3. **H1 Fix 1** commit the two default key headers; **M5** standardize mount name to `fs_ota`; **M4** null-check provisioning `fopen()`.
4. **C2/C3** rewrite Client `enableDfuMode()` → `performMcubootUpdate()`, add `dfuKickWatchdog`, delete loop + ODFU + RS-485 quiescing; **C2.5** migrate Server `enableDfuMode()` to MCUboot.
5. **H2 (A)** strict staging validation incl. role/version check; fix pad-loop abort.
6. **C4 (A or B)** route/de-risk GitHub Direct (stage-to-QSPI or drop); keep TLS per posture.
7. **H5 (A)** shared criteria-gated `tankalarm_markFirmwareHealthy()` in all three `loop()`s; remove the `setup()` confirm.
8. **H4** ship/install `.slot.bin`; Notehub gets the slot image; remove duplicate Viewer; segregate recovery artifacts.
9. **H3 (A)** add `mcuboot_keys/README.md` posture statement; **§1** posture text into KeyProvisioning header.
10. **M1** bump all version strings to 1.9.1; **M6** verify imgtool version/downgrade flags.
11. **§5** provision the bench unit; run the **§7 matrix**; only then enable OTA policy in the field.

---

## 9. Quick Reference

| Finding | Recommended fix (all-in MCUboot) | Effort | Blocker? |
|---|---|---|---|
| C1 | Hoist `fp` (A) + CI macro build | XS | **Yes** |
| C2/C3 | Client → `performMcubootUpdate`; delete loop/ODFU/RS-485 | S | **Yes** |
| C2.5 | Server → `performMcubootUpdate` | S | **Yes** |
| C4 | Stage GitHub Direct to QSPI (A) or drop (B); TLS optional | S–M | High |
| H1 | Commit default headers; keys already match defaults | XS | **Yes** (compile) |
| H2 | Strict staging validation + role check; pad-loop abort | M | **Yes** |
| H3 | Keep default keys; document "no authenticity" | XS | No (by design) |
| H4 | Publish/install `.slot.bin`; Notehub slot upload; de-dup Viewer | S | **Yes** |
| H5 | Shared criteria-gated confirm in all 3 roles | M | **Yes** |
| M1 | Unify versions → 1.9.1 + CI guard | XS | No |
| M2 | Move block inside include guard; relocate `qspi_root` | XS | **Yes** |
| M3 | Delete RS-485 quiescing | XS | No |
| M4 | Null-check provisioning `fopen()` | XS | No |
| M5 | Standardize mount name `fs_ota` | XS | High |
| M6 | Verify imgtool version/downgrade | XS | Verify |

**Bottom line:** the all-in MCUboot decision **removes** the hardest problems the reviews raised — the key chain is already on the Arduino defaults, so H1/H3 collapse into "commit two public headers and document the no-security stance." The remaining work is mechanical: make the helper compile (C1), wire **all three roles** to stage-to-QSPI + `MCUboot::applyUpdate` (C2/C3/C2.5/C4), validate stages including a **role check** (H2, because shared default signatures can't distinguish roles), confirm health per role (H5), ship `.slot.bin` (H4), and one-time-provision every device (§5). Gate the rollout on the §7 bench matrix.

---

## 10. Additional Strategic Suggestions (Independent Audit Contributions)

During our independent engineering verification of the "All-In on MCUboot" strategy, we have identified several latent edge cases and optimization opportunities. We strongly recommend appending these five measures to the implementation plan:

### 10.1 Persistent Trial & Rollback Self-Detection (Eliminating Infinite Re-Download Loops)
* **The Vulnerability:** Under MCUboot, if a trial image is buggy, crashes, or fails its health check, the bootloader automatically reverts the swap to the previous working version on the next reboot. However, the reverted firmware maintains no state tracking of the failed attempt. As soon as it boots, it will check Notehub, find that the new version is still marked as "available," stage it to QSPI, trigger the swap, reboot, fail, and roll back again. This results in an infinite data-transfer and reboot loop that depletes solar battery cells and burns cellular bandwidth.
* **The Solution:** Implement a transition state file on the QSPI FAT partition (e.g., `/fs_ota/pending_ota.json`) containing the target update version and trial state:
  1. **Pre-Swap Action:** Before calling `MCUboot::applyUpdate(false)` and resetting, the staging code in [TankAlarm-112025-Common/src/TankAlarm_DFU.h](TankAlarm-112025-Common/src/TankAlarm_DFU.h) must write `{ "target_v": "1.9.1", "status": "trial" }` to `/fs_ota/pending_ota.json`.
  2. **Boot-Time Resolution:** During `setup()` of any component (Client, Server, or Viewer):
     - Mount `/fs_ota` and check `/fs_ota/pending_ota.json`.
     - If a trial version is active:
       - **Success:** If current running `FIRMWARE_VERSION` matches `target_v`, we succeeded. Proceed to `setup()` complete, let the health gate verify network connectivity, and then write `"idle"` to the state file and call `MCUboot::confirmSketch()`.
       - **Rollback Detected:** If current running `FIRMWARE_VERSION` does *not* match `target_v`, **a rollback has occurred**. Increment a failure count, add `target_v` to a local temporary blacklist (preventing re-download), send a failed status `{"status": "rollback detected - trial crashed", "stop": true}` to Notehub to disable the update push, and clear the JSON file.

### 10.2 CI Automation: Single Source of Truth for Versions and Role MBR Boundaries
* **The Vulnerability:** Human error during rapid release cycles can lead to mismatches between the `FIRMWARE_VERSION` macro hard-coded in [TankAlarm-112025-Common/src/TankAlarm_Common.h](TankAlarm-112025-Common/src/TankAlarm_Common.h#L19) and the version passed as an argument to `imgtool` in [.github/workflows/release-firmware-112025.yml](.github/workflows/release-firmware-112025.yml). Furthermore, because all three roles (Client, Server, Viewer) share the same default public keys for signature verification, signature checks alone *cannot* prevent a Server slot.bin from being staged and accepted by a Client, potentially corrupting control signals.
* **The Solution:**
  1. Update [.github/workflows/release-firmware-112025.yml](.github/workflows/release-firmware-112025.yml) to automatically extract the release version directly from the source header before signing:
     ```bash
     VERSION=$(grep -oE '#define FIRMWARE_VERSION "([^"]+)"' TankAlarm-112025-Common/src/TankAlarm_Common.h | cut -d'"' -f2)
     echo "Target release version: $VERSION"
     ```
  2. Add an explicit "Component ID" (e.g. `Client = 1`, `Server = 2`, `Viewer = 3`) within the first few bytes of the staged file, and ensure [TankAlarm-112025-Common/src/TankAlarm_DFU.h](TankAlarm-112025-Common/src/TankAlarm_DFU.h) parses the image header and verifies the role matches the target device before marking the swap as ready.

### 10.3 Provisioning: Mount-Before-Format QSPI Wear Reduction
* **The Pitfall:** The `setupMCUBootOTAData()` routine in [TankAlarm-112025-KeyProvisioning/TankAlarm-112025-KeyProvisioning.ino](TankAlarm-112025-KeyProvisioning/TankAlarm-112025-KeyProvisioning.ino#L42) calls `ota_data_fs.reformat(&ota_data)` unconditionally. This triggers continuous, heavy block erasure on the QSPI chip every time the provisioning tool is uploaded, even if the flash disk is already formatted and partitioned correctly.
* **The Solution:** Modify [TankAlarm-112025-KeyProvisioning/TankAlarm-112025-KeyProvisioning.ino](TankAlarm-112025-KeyProvisioning/TankAlarm-112025-KeyProvisioning.ino) to attempt to mount the partition first:
  ```cpp
  int err = ota_data_fs.mount(&ota_data);
  if (err == 0) {
    // Check if update.bin and scratch.bin exist with correct sizes
    FILE* f_up = fopen("/fs/update.bin", "rb");
    FILE* f_sc = fopen("/fs/scratch.bin", "rb");
    if (f_up && f_sc) {
      Serial.println(F("QSPI partition already provisioned and healthy. Skipping format."));
      fclose(f_up);
      fclose(f_sc);
      return;
    }
    if (f_up) fclose(f_up);
    if (f_sc) fclose(f_sc);
  }
  // If mount fails or files are missing, proceed to reformat and pre-allocate
  ```
  This reduces board commissioning wear and preserves the 100k-minimum write endurance cycles of cell gates.

### 10.4 Segmented Staging Fallback Warn-Logging
* **The Pitfall:** In [TankAlarm-112025-Common/src/TankAlarm_DFU.h](TankAlarm-112025-Common/src/TankAlarm_DFU.h#L740), if opening `/fs_ota/update.bin` in `"r+b"` mode fails, the staging helper falls back to `"wb"` mode, erasing the pre-allocated region. This causes FAT filesystem allocation tables to fragment on subsequent chunks.
* **The Solution:** Add a prominent warning print to the fallback sector:
  ```cpp
  Serial.println(F("[WARNING] QSPI file allocation missing or invalid! Formatting and pre-allocating new spaces. Fragmentation risk alert. Run KeyProvisioning to restore contiguous sectors."));
  ```
  This immediately alerts developers and bench technicians that the QSPI filesystem is in a downgraded, un-commissioned state.

### 10.5 Watchdog Assertions During Dry-Run Boots
* **The Pitfall:** During a trial boot of a new image, developers must ensure that if the sketch locks up (e.g. while attempting DHCP initialization or awaiting an I2C component response), the hardware watchdog is *active* and *not fed* in failure paths, in order for the system to reset and roll back.
* **The Solution:**
  1. Symmetrize watchdog timeouts to a strict limit (e.g., 10 seconds maximum) during dry-runs.
  2. Enforce that no blocking loops in `setup()` (such as while executing `Ethernet.begin()` or waiting for the Notecard initialization) are permitted to spin endlessly. They must implement bounded counts and let the hardware watchdogs trip if unacknowledged.

---

## 11. GitHub Copilot Addendum: Implementation Guardrails and Corrections

After reviewing this proposed fixes document and checking the local Arduino Opta MCUboot core, the Blues Notecard DFU API reference, and the MCUboot `imgtool` documentation, I agree with the overall all-in MCUboot direction. The default-key premise is locally supported: the repository PEM files match the installed Arduino `mbed_opta@4.5.0` default PEMs, and the missing `enableSecurity` C headers exist in the core. The recommendations below are additions and small course-corrections to reduce implementation risk.

### 11.1 Do not modify the first bytes of a signed slot image for role metadata

Section 10.2 suggests adding a Component ID within the first few bytes of the staged file. Avoid doing that unless the ID is present before signing. A signed/encrypted MCUboot slot image is a sealed artifact: changing bytes after `imgtool sign` can break the image hash/signature and cause MCUboot to reject the update.

Use one of these safer role-identity options instead:

1. **Release filename and Notehub source check, mandatory:** require exact names such as `TankAlarm-Client-secure-v1.9.1.slot.bin`, `TankAlarm-Server-secure-v1.9.1.slot.bin`, and `TankAlarm-Viewer-secure-v1.9.1.slot.bin`. Extend `tankalarm_checkDfuStatus()` to capture `body.name` and `body.source`, then reject a ready update before `dfu.get` if the source/name does not match the running role.
2. **MCUboot protected custom TLV, preferred for in-image metadata:** use `imgtool sign --custom-tlv` to include role metadata in the signed image. Then parse that TLV from the staged file before `MCUboot::applyUpdate(false)`. This preserves signature validity because the metadata is added by `imgtool`, not patched afterward.
3. **Signed sidecar manifest, optional:** publish a small `TankAlarm-<Role>-vX.Y.Z.manifest.json` containing role, version, expected length, CRC/MD5/SHA, and artifact name. This is easiest for GitHub Direct, but Notehub IAP is cleaner if the needed metadata can come from `dfu.status body`.

Minimum rule: never edit the slot image after signing/encryption. Treat role identity as metadata to verify, not bytes to inject into the artifact later.

### 11.2 Promote Notecard DFU metadata into the shared DFU status struct

The Blues DFU API exposes more than the current code stores. `dfu.status` `body` can include `length`, `crc32`, `md5`, `name`, `source`, timestamps, notes, and type. The implementation plan should explicitly extend `TankAlarmDfuStatus` to carry at least:

```cpp
uint32_t firmwareLength;
uint32_t firmwareCrc32;
char firmwareMd5[33];
char firmwareName[96];
char firmwareSource[96];
```

Then make staging validation use those fields:

- `firmwareLength` must equal the total bytes written.
- `firmwareCrc32` should match the running CRC already computed by `tankalarm_performMcubootUpdate()`.
- `firmwareMd5` should match a staged-file MD5 if feasible, especially for GitHub/Notehub parity.
- `firmwareName`/`firmwareSource` must match the expected role-specific `.slot.bin` naming rule.

This gives the MCUboot path a practical validation baseline without inventing new metadata infrastructure first. Later, consider `dfu.get {"binary":true}` for larger chunks and per-chunk MD5 `status`; keep base64 for the first correctness pass if binary-buffer handling in the Notecard library would slow the implementation.

### 11.3 Split "staged", "trial", "confirmed", and "rollback" status reporting

The current helper clears Notecard DFU state with `dfu.status {"stop":true,"status":"staged for MCUboot"}` before calling `MCUboot::applyUpdate(false)`. That can be acceptable for freeing the Notecard's local image after QSPI staging, but it should not be treated as terminal success.

Add an explicit OTA state machine with terminal-state reporting:

| State | Local action | Notehub / diagnostic reporting |
|---|---|---|
| `staged` | QSPI file written and validated | report `staged`, not success |
| `trial` | `pending_ota.json` written, `applyUpdate(false)`, reset requested | report `trial pending` if possible |
| `confirmed` | role health gate passed, `confirmSketch()` called | report `completed/confirmed` and clear pending state |
| `rollback` | running version does not match pending target after reboot | report `rollback detected`, stop/clear failed update, block that version locally |

Important detail: only a health-confirmed image should be called "successful." If `stop:true` is used before reboot, pair it with local `/fs_ota/pending_ota.json` so the old firmware can still detect and report rollback after MCUboot reverts.

### 11.4 Keep QSPI OTA constants in one common header and verify against the core

The plan correctly observes that the current geometry lines up: header `0x20000`, application payload `0x1C0000`, slot `0x1E0000`, scratch `0x20000`. Do not leave those as repeated magic numbers across CI, provisioning, and `TankAlarm_DFU.h`.

Create a shared header such as `TankAlarm_MCUbootConfig.h`:

```cpp
#define TANKALARM_MCUBOOT_HEADER_SIZE  0x20000UL
#define TANKALARM_MCUBOOT_APP_SIZE     0x1C0000UL
#define TANKALARM_MCUBOOT_SLOT_SIZE    0x1E0000UL
#define TANKALARM_MCUBOOT_SCRATCH_SIZE 0x20000UL
```

Use the same values in:

- `TankAlarm_DFU.h`
- `TankAlarm-112025-KeyProvisioning.ino`
- `.github/workflows/release-firmware-112025.yml`
- CI size checks

Add a CI check that fails if a built raw binary exceeds `TANKALARM_MCUBOOT_APP_SIZE`, and a second check that the workflow `--header-size` and `--slot-size` match the shared constants. This catches image growth before a device sees an OTA package.

### 11.5 Treat encrypted-slot acceptance as a first bench gate

The repo/core key hashes match locally, but the real system still needs one hardware proof with the exact CI command line. Make the first bench acceptance test specifically use an encrypted, signed `*-secure-vX.Y.Z.slot.bin` artifact produced by the release workflow, not a locally rebuilt or manually adjusted image.

Pass criteria:

1. Provisioning sketch burns the default signing-public and encrypt-private keys.
2. CI-built encrypted slot image is staged to `/fs_ota/update.bin` unchanged.
3. `MCUboot::applyUpdate(false)` triggers a test swap.
4. The new image boots, reaches the role health gate, and confirms.
5. A deliberately bad health image rolls back.

If encrypted slots fail but signed-only images pass, do not paper over it in firmware. Decide explicitly whether to keep encryption and fix provisioning, or remove `--encrypt` from CI and document signed-only mechanical rollback.

### 11.6 Prefer fail-closed staging over "create update.bin in wb" fallback

Section 10.4 recommends warning when `/fs_ota/update.bin` cannot be opened in `r+b` and the updater falls back to `wb`. I would go one step stricter for production: fail closed by default.

Recommended behavior:

- **Production:** if `/fs_ota/update.bin` is missing or cannot open in `r+b`, abort the update and report `QSPI OTA partition not provisioned; run KeyProvisioning`.
- **Developer override:** allow `wb` recreation only behind a compile-time flag such as `TANKALARM_MCUBOOT_ALLOW_RECREATE_UPDATE_FILE`, with the warning from Section 10.4.

This prevents field units from silently switching from the carefully preallocated OTA layout to a fragmented or undersized FAT file.

### 11.7 Define an update policy for Clients, not only Servers

The proposed plan says to gate Client auto-apply behind a policy flag. Make that a concrete stored config field before enabling MCUboot on field Clients.

Suggested values:

```cpp
#define CLIENT_UPDATE_POLICY_DISABLED       0
#define CLIENT_UPDATE_POLICY_ALERT_ONLY     1
#define CLIENT_UPDATE_POLICY_AUTO_MCUBOOT   2
```

Default should be `DISABLED`. The Server web UI can push this value through the existing Client configuration path. Solar-powered Clients should also require an `okToUpdateNow()` gate that checks power state, battery/Vin thresholds, active alarms, relay state, and recent telemetry. MCUboot protects the firmware image, but it does not make an update operationally safe during an alarm or low-power window.

### 11.8 Add a "workspace library only" build rule to every Arduino command in docs and CI

The stale-library discovery is important enough to make a standing rule. Any compile command used for validation should force the workspace common library by passing the parent library directory or explicit `--library` path in the form known to compile the intended source, then print the resolved library path from verbose output in CI.

Add a CI guard that fails if the build log contains an installed sketchbook copy of `TankAlarm-112025-Common`, for example under `Documents/Arduino/libraries` or `OneDrive/Documents/Arduino/libraries`. This prevents the exact false-green that hid the MCUboot compile error.

### 11.9 Keep `MCUboot::bootDebug(true)` as a bench-only diagnostic switch

The local core exposes `MCUboot::bootDebug(bool)`, which toggles a backup-register debug flag. Add a temporary bench option such as `TANKALARM_MCUBOOT_BOOTDEBUG` to enable it during destructive validation. Do not leave it enabled in production unless the serial/debug output has been measured for boot-time and power impact.

Use it in the bench matrix to capture:

- pending/test swap decision,
- rollback decision,
- image validation failure reason,
- confirm marker state.

This will save a lot of guesswork when testing bad-health and power-loss scenarios.

### 11.10 Tighten the immediate implementation order

The current Section 8 order is mostly right. I would adjust the first implementation slice so it cannot accidentally create a half-wired release:

1. **Foundation PR:** constants header, include-guard fix, `goto` compile fix, default key headers, provisioning null checks, `fs_ota` standardization, workspace-library CI macro build.
2. **Staging PR:** metadata expansion in `TankAlarmDfuStatus`, strict QSPI staging validation, fail-closed `r+b` behavior, pending/rollback state file.
3. **Runtime PR:** Client/Server/Viewer all call MCUboot staging, no ODFU apply path remains, Client update policy added, per-role health confirm added.
4. **Release PR:** `.slot.bin` artifact selection, duplicate Viewer artifact removed, role naming/metadata checks, version consistency checks.
5. **Bench-only PR or branch:** bootDebug, test images, and the hardware validation scripts/checklist.

Do not turn on `-DTANKALARM_DFU_MCUBOOT` in the production release workflow until the Foundation and Staging PRs are both merged and passing. That keeps the repo buildable and prevents a brief period where production firmware points at an updater that still lacks validation.

---

## 12. Final Architecture & Resilience Recommendations (AI Review)

Building upon the Copilot Addendum, here are a few final resilience improvements to ensure devices remain remotely manageable even in edge-case failure scenarios.

### 12.1 Remote QSPI Recovery Mechanism
While Section 11.6 wisely advises failing closed if `/fs_ota/update.bin` is missing, a catastrophically corrupted FAT filesystem on the QSPI chip would render the device permanently unable to receive OTA updates, necessitating a physical truck roll for USB recovery.
**Recommendation:** Implement an emergency "Format QSPI" command via a Notehub Environment Variable (e.g., `_qspi_format_req`) or a secure remote command. This would allow an administrator to remotely trigger the same `KeyProvisioning` format/preallocation routine to resurrect a corrupted QSPI partition without physical access.

### 12.2 Notecard Flash Cache Eviction on Rollback
If a firmware update rolls back (as detected via `pending_ota.json` described in 11.3), the device must explicitly instruct the Notecard to discard the downloaded firmware payload. Otherwise, the Notecard will continue holding the failed image. 
**Recommendation:** When a rollback is detected, the firmware must send a specific `dfu.status` request containing `"stop": true` (and/or `"error": "rollback"`) to thoroughly clear the Notecard's internal DFU cache and prevent spurious update notifications on subsequent syncs.

### 12.3 Bootloader-Phase Hardware Watchdog
MCUboot itself takes time to validate signatures, verify hashes, and perform swapping. If the image is large and QSPI reads are slow (or if QSPI hardware enters a bad state), the bootloader might hang.
**Recommendation:** Verify that the Arduino `mbed_opta` default bootloader keeps the independent hardware watchdog (IWDG) fed during the image swap and validation process. If the bootloader does not feed the IWDG, ensure the watchdog timeout set by the application is long enough (e.g., 10-15 seconds) to allow the bootloader to finish a full `0x1E0000` byte swap and hash computation without causing a boot loop.

### 12.4 Expose "Previous Version" in Telemetry
For fleet observability, knowing an update failed is just as important as knowing why.
**Recommendation:** Once an update succeeds or rolls back, include a `previous_version` field in the next outbound telemetry sync. This allows the Notehub dashboard to clearly visualize the update trajectory (e.g., "rolled back from 1.9.1 to 1.9.0" or "updated from 1.9.0 to 1.9.1") rather than just showing the active version.

---

## 13. FINAL CONSOLIDATED IMPLEMENTATION PLAN (v1.9.1, MCUboot)

This section reconciles the original fixes (§1–§9) with the three appended suggestion sets (§10 independent audit, §11 Copilot addendum, §12 resilience review) into one authoritative, de-duplicated, PR-sliced plan. Where the appendices conflict, this section resolves it and states why. Items are grounded in facts re-verified against the repo and the installed `arduino:mbed_opta@4.5.0` core.

### 13.0 Disposition of the appended suggestions (§10–§12)

| # | Suggestion | Disposition | Rationale / where it lands |
|---|---|---|---|
| 10.1 | `pending_ota.json` trial/rollback self-detection | **ACCEPT** | Core of **WS-E**. Real infinite-loop risk on solar/cell. |
| 10.2 | Single-source version in CI | **ACCEPT** | Into **WS-B** (workflow already greps the header — formalize it). |
| 10.2 | Component ID in **first bytes** of staged image | **REJECT** | Breaks the signed image hash. Superseded by 11.1 (name/source + optional protected TLV). |
| 10.3 | Mount-before-format (QSPI wear) | **ACCEPT** | Into **WS-C**. |
| 10.4 | Warn-log on `wb` fallback | **MODIFY** | Subsumed by 11.6 fail-closed (warn is the *dev-override* branch). **WS-C/WS-D**. |
| 10.5 | Watchdog active in dry-run; no unbounded `setup()` loops | **ACCEPT** | Into **WS-I**. |
| 11.1 | Don't post-sign edit; use name/source (+ optional protected TLV) | **ACCEPT** | Role identity in **WS-D**. Name/source is primary; TLV is bench-gated. |
| 11.2 | Extend `TankAlarmDfuStatus` (length/crc32/md5/name/source) | **ACCEPT** | **WS-D**. Verified: today it parses **only `length`**. |
| 11.3 | staged/trial/confirmed/rollback state machine | **ACCEPT** | Merges with 10.1 → **WS-E**. |
| 11.4 | One QSPI/MCUboot constants header | **ACCEPT** | **WS-B**. |
| 11.5 | First bench gate = encrypted CI slot image | **ACCEPT** | **WS-L bench matrix**. |
| 11.6 | Fail-closed staging (no silent `wb`) | **ACCEPT** | **WS-D**. Overrides 10.4. |
| 11.7 | Concrete Client update-policy config field | **ACCEPT** | **WS-F**. |
| 11.8 | "Workspace-library-only" build rule + CI guard | **ACCEPT** | **WS-B**. This is the guard that catches C1's stale-library trap. |
| 11.9 | `bootDebug(true)` bench-only behind flag | **ACCEPT** | **WS-I/WS-L**. |
| 11.10 | PR-sliced order | **ACCEPT (as the spine)** | Becomes §13.2, extended with WS-E. |
| 12.1 | Remote QSPI re-format command | **DEFER (post-1.9.1)** | Useful resilience, but adds a remotely-triggered destructive op; land after the core path is bench-proven. **WS-K**. |
| 12.2 | Evict Notecard DFU cache on rollback | **ACCEPT** | Part of **WS-E** terminal states. |
| 12.3 | Confirm bootloader feeds IWDG during swap | **ACCEPT (bench-critical)** | **WS-I/WS-L**. App watchdog is **30 s**; see 13.3. |
| 12.4 | `previous_version` in telemetry | **ACCEPT** | **WS-E** observability. |

### 13.1 Two corrections this plan makes to the appendices

1. **Confirm-gate criteria must be achievable OFFLINE (correction to H5 / 10.1 / 11.3).** Several sections suggest confirming firmware health on *"first telemetry ack"*. Making network connectivity a **hard** confirm requirement is dangerous: a healthy unit at a poor-signal site that reboots (brownout) before it ever gets an ack would be **rolled back even though the firmware is fine**. **Resolution:** the confirm gate for v1.9.1 is **local health** — storage mounted, config loaded, safety relays in known-safe state, Notecard reachable over I2C — all reachable without the cell network. Treat a telemetry ack as a *bonus signal*, not a precondition. A separate, slower "networking-broke" watchdog (re-stage a known-good image) can be added later if needed; it should not gate the MCUboot confirm.
2. **Trial/rollback detection must not rely on the human `FIRMWARE_VERSION` string alone (refinement to 10.1/11.3).** Reflashing the *same* version during testing (1.9.1 over 1.9.1) makes "running == target" indistinguishable from a rollback. **Resolution:** key the `pending_ota.json` trial record on a **monotonic build/sequence number** (or the MCUboot image version from the header) in addition to the version string; for same-version bench reflashes, require manual confirm. This also ties into M6 (downgrade prevention).

### 13.2 PR-sliced delivery (the spine — extends §11.10)

**Do not set `-DTANKALARM_DFU_MCUBOOT` ON in the release workflow until PR1, PR2, and PR3 are merged and green.** That prevents a window where shipped firmware points at an updater lacking validation.

- **PR1 — Foundation (compile + structure + guards).** WS-A + WS-B + WS-C.
  - C1 hoist `fp`; M2 move MCUboot block inside the include guard and relocate `static qspi_root` into function scope.
  - H1 Fix 1: commit `ecdsa-p256-signing-key.h` / `ecdsa-p256-encrypt-key.h` into KeyProvisioning; M4 null-check `fopen`; M5 standardize `fs_ota`; 10.3 mount-before-format.
  - WS-B: `TankAlarm_MCUbootConfig.h` shared constants (11.4); CI **workspace-library-only** macro build for all three sketches + clean-checkout KeyProvisioning build (11.8); key-provenance hash check (H1 Fix 2); version single-source + consistency guard (10.2 + M1).
  - **Gate:** all sketches compile with the macro **against the workspace library**; CI fails on any installed-copy resolution.
- **PR2 — Staging & state (validate + don't brick + rollback).** WS-D + WS-E.
  - H2 strict staging transaction (length/crc32 compare, MCUboot magic `0x96f3b83d`, role check, every `fseek`/`fwrite` checked, pad-loop **abort not fall-through**); 11.6 fail-closed `r+b` (dev-override flag for `wb`).
  - 11.2 extend `TankAlarmDfuStatus` (length/crc32/md5/name/source) and populate from `dfu.status body`.
  - WS-E OTA state machine: `pending_ota.json` (trial record keyed on build/seq per 13.1·2); boot-time resolve → confirmed / rollback; on rollback evict Notecard DFU cache (`dfu.status {"stop":true}`) + persist a one-entry version blacklist; `previous_version` telemetry.
- **PR3 — Runtime apply migration (one mechanism, no ODFU).** WS-F + WS-G.
  - C2/C3 Client `enableDfuMode()` → `tankalarm_performMcubootUpdate()`, add `dfuKickWatchdog` wrapper, delete infinite loop + `card.dfu` trigger + RS-485 quiescing (M3).
  - C2.5 Server `enableDfuMode()` → MCUboot (keep state-saving); legacy IAP behind `#if !defined(TANKALARM_DFU_MCUBOOT)` only during migration.
  - Viewer apply path → MCUboot.
  - H5 shared `tankalarm_markFirmwareHealthy()` gated on **local** health (13.1·1), called from each `loop()`; remove the unconditional `setup()` confirm.
  - 11.7 Client update-policy config field (`DISABLED` default) + solar `okToUpdateNow()` gate (power/battery/alarm/relay/telemetry-recency).
- **PR4 — Release & artifacts.** WS-H.
  - H4 select/publish `.slot.bin`; Server GitHub discovery picks `TankAlarm-Server-secure-v%s.slot.bin`; remove duplicate Viewer line; segregate raw/`with_bootloader` bins as "USB recovery only"; document the Notehub `.slot.bin` host-firmware upload step.
  - C4 route GitHub Direct through the shared `stageSlotToQspi()` + `commitMcubootSlot()` (or drop GitHub Direct to alert-only); TLS stays `VERIFY_NONE` per posture, noted in §1.
  - M6 verify `imgtool --version` format + downgrade policy; H3 add `mcuboot_keys/README.md` posture statement.
  - **Only now flip `-DTANKALARM_DFU_MCUBOOT` ON in the workflow.**
- **PR5 — Bench/diagnostics (branch, not shipped ON).** WS-I + WS-L.
  - 11.9 `bootDebug` behind `TANKALARM_MCUBOOT_BOOTDEBUG`; 10.5 watchdog dry-run discipline; 12.3 bootloader-IWDG verification; run the full §13.3 matrix.
- **PR6 — Deferred resilience (post-1.9.1).** WS-K: 12.1 remote QSPI re-format command (gated/audited).

### 13.3 Bench verification matrix (must pass before enabling field OTA)

Extends §7 with the appendix-driven items:
1. Provision → install **CI-built encrypted** `.slot.bin` → boots → local-health confirm fires once (11.5).
2. Corrupt/truncated image → staging aborts before `applyUpdate`; Notecard state preserved (H2).
3. Power loss during **staging** → prior firmware still boots (QSPI staging, not live flash).
4. Power loss during **swap** → MCUboot resumes/rolls back to a bootable image. **Explicitly measure swap duration vs the 30 s app watchdog** and confirm whether the Arduino MCUboot bootloader kicks the IWDG during a full `0x1E0000` swap+hash (12.3). If it does **not** and swap approaches/exceeds 30 s, the fix is **bootloader-side IWDG-resume**, **not** a larger app timeout (the IWDG is already near its hardware ceiling — see §13.6). **This is the single most important unknown.**
5. Bad-health image (never passes local gate) → no confirm → auto rollback; `pending_ota.json` detects it; Notecard cache evicted; version blacklisted; `previous_version` reported (WS-E).
6. Wrong-role image (Server slot → Client) → rejected by name/source + role check (H2/11.1), since the shared default signature will **not** reject it.
7. Same-version reinstall → confirm downgrade-prevention + the build/seq trial key behave (13.1·2, M6).
8. If pursuing protected custom-TLV role metadata (11.1 opt 2): prove `imgtool` writes it **and** the Arduino MCUboot bootloader still accepts the image; otherwise stay on name/source only.

### 13.4 Definition of Done (v1.9.1)

- All three sketches build with `-DTANKALARM_DFU_MCUBOOT` against the **workspace** library in CI; KeyProvisioning builds from a clean checkout.
- No code path triggers `card.dfu name=stm32` apply or an infinite watchdog-fed wait; RS-485 quiescing removed.
- Client, Server, Viewer all apply via `tankalarm_performMcubootUpdate()` with strict staging validation and fail-closed QSPI.
- A bad image rolls back **and** the unit does not immediately re-download it (WS-E proven on hardware).
- Confirm gate is offline-achievable; no false rollback at a no-signal bench.
- Release publishes role-named `.slot.bin`, no duplicate artifacts, raw bins clearly marked recovery-only; Notehub host-firmware upload documented.
- All version strings = `1.9.1`; CI version/key/artifact guards green.
- §13.3 matrix passed on at least one unit per role, including the bootloader-IWDG measurement.

### 13.5 Repo facts re-verified for this plan

- App watchdog = **30 s** (`WATCHDOG_TIMEOUT_SECONDS`, `mbedWatchdog.start(timeoutMs)` in Client/Viewer) — grounds the 12.3 swap-window concern.
- `tankalarm_checkDfuStatus()` currently parses **only** `body.length` — grounds the 11.2 struct extension.
- Repo `mcuboot_keys/*.pem` are byte-identical (SHA-256) to the core `MCUboot/default_keys/*.pem`; the needed `ecdsa-p256-*-key.h` headers exist in `MCUboot/examples/enableSecurity` but are not committed.
- Slot geometry already consistent: header `0x20000` + app `0x1C0000` = slot `0x1E0000`; `update.bin` preallocated `0x1E0000`, `scratch.bin` `0x20000`.
- `MCUboot::confirmSketch()`, `applyUpdate(bool)`, `bootDebug(bool)` present in the core; `TANKALARM_DFU_MCUBOOT` currently OFF in CI.
- **Bootloader MCUboot config re-verified** (`…/libraries/MCUboot/src/mcuboot_config/mcuboot_config.h`): `MCUBOOT_WATCHDOG_FEED()` is defined as a **no-op** (`do {} while(0)`) — the bootloader does **not** feed any watchdog during swap; `MCUBOOT_SWAP_USING_SCRATCH 1` (stateful, resumable swap with rollback); `MCUBOOT_IMAGE_NUMBER 1` (one image, two slots); `MCUBOOT_SIGN_EC256` + `MCUBOOT_ENC_IMAGES` (signed **and** encrypted). These ground §14.

**Final word:** the plan is now a single MCUboot track delivered in 6 PRs, gated so production never points at an unvalidated updater. The two genuinely open risks are (1) **bootloader IWDG behavior during the swap** (13.3·4) and (2) **getting the confirm gate offline-safe** (13.1·1). Resolve those on the bench before flipping the macro on and pushing any field OTA.

### 13.6 Watchdog timer: should we change it? (analysis — for further review)

**Question raised:** since the MCUboot swap window is the top open risk (13.3·4), should we change the watchdog timeout?

**Short answer: No — keep it at ~30 s. Do not treat the watchdog timeout as the lever for the swap-window problem.** Verified facts and reasoning:

**1. There is a hard hardware ceiling (~32.77 s).** The Opta watchdog is the STM32H747 **IWDG**, clocked by the ~32 kHz LSI with max prescaler 256 and a 12-bit (4096) reload:

$$t_{max} = \frac{4096 \times 256}{32000} \approx 32.77\ \text{s}$$

The current `WATCHDOG_TIMEOUT_SECONDS = 30` (`TankAlarm_Common.h`, started via `mbedWatchdog.start(timeoutMs)` in Client/Viewer) is already **~92% of the ceiling**. We could nudge to ~32 s, but **cannot** reach 45/60 s on the IWDG — `mbed::Watchdog::start()` is bounded by `get_max_timeout()` and rejects/clamps larger values. A longer wall-clock guard would require a *different* mechanism (RTC alarm or software supervisor), not the IWDG.

**2. The swap runs in the bootloader, not the app — so the app timeout value is mostly irrelevant to it.** `MCUboot::applyUpdate(false)` → `NVIC_SystemReset()` hands control to MCUboot, which performs the slot swap **before** our sketch runs. Once started, the IWDG keeps counting into that bootloader phase and **cannot be stopped by software** (only a power-on reset clears it). So the real question is **"does the Arduino MCUboot bootloader service the IWDG while it swaps ~1 MB?"** — exactly the 13.3·4 unknown. No app-side timeout value changes that answer.

**3. A longer timeout would *hurt* rollback.** During the **trial boot**, the watchdog is what resets a hung new image so MCUboot can revert. Longer timeout = **slower rollback** on a bad image. For rollback responsiveness we want this *short*, not long.

**4. The real images are well under the window.** Current artifacts: Client ~303 KB, Server ~933 KB, Viewer ~287 KB. A swap is typically single-digit to low-teens of seconds, comfortably under 30 s — **but it must be measured**, because MCUboot swap is *resumable*: if the IWDG fires mid-swap and the bootloader doesn't kick it, we don't brick — we reboot and **restart** the swap, which can loop forever only if a swap can *never* finish within the window.

**What to do instead (do not change the timeout):**
- **Kick the watchdog immediately before `applyUpdate()` + reset.** Make the last action before `NVIC_SystemReset()` a `dfuKickWatchdog()` so the bootloader inherits a full ~30 s window rather than a partially-elapsed one. Cheap, strictly helpful. (Folds into WS-D/WS-E.)
- **Bench-measure swap duration** for all three roles (13.3·4) with `bootDebug` on (11.9).
- **Verify the bootloader's IWDG behavior.** If Arduino MCUboot kicks the IWDG during swap (likely, since they ship this OTA flow), 30 s is fine and **no change is needed**. If it does not and a swap can approach the window, the fix is **bootloader-side** (or accept the resumable-swap retry), never a bigger app timeout.
- **Optional micro-tweak only:** raising 30 → 32 s buys ~2 s of margin at the cost of slightly slower hang-recovery/rollback. Not worth it unless bench data shows a swap lands in the 28–32 s band — in which case the right move is bootloader IWDG-resume, not living on the 0.77 s ceiling margin.

**Recommendation:** Leave `WATCHDOG_TIMEOUT_SECONDS` at **30**. Treat it as a non-lever; add the kick-before-reset and resolve swap-duration + bootloader-IWDG on the bench (13.3·4) before enabling field OTA. **Open for review:** if we ever need a guaranteed >32 s protected window, that's a separate design (RTC/software supervisor) and a different discussion.

---

## 14. IWDG Deep Dive & Mitigation Strategies (AI Analysis)

Because the IWDG timeout risk is a critical bottleneck that spans across both application firmware and the bootloader runtime, we must define the precise mitigation strategies if the bootloader fails to service the watchdog during the swap phase. 

### 14.1 The Software-Reset Loophole (The Hardware Reality)
The STM32H747 allows the IWDG to be configured as either a *hardware watchdog* (starts automatically on power-on) or a *software watchdog* (started by user code).
* **The Reality:** By default, the Arduino Opta core configures the IWDG as a *software watchdog*.
* **The Implication (as originally proposed):** When `NVIC_SystemReset()` is called to trigger the MCUboot swap, this generates a **software system reset**. The proposal asserts that a software reset *stops* a software-configured IWDG, so the bootloader would run with **no watchdog** and the 30 s ceiling would not apply during swap.

> **⚠️ CORRECTION / VERIFICATION STATUS — do not rely on this loophole.** This premise is **unverified and contradicts the conservative position in §13.6** ("the IWDG keeps counting into the bootloader phase and cannot be stopped by software"). The two cannot both be true, and we must reconcile in favor of the safe assumption until proven otherwise:
> - **Documented STM32 behavior:** once the IWDG is started, it is **not** stopped by a software reset (`NVIC_SystemReset()` / SYSRESETREQ). Per RM0399, only specific reset sources clear it; a software reset is generally **not** one of them. So a started IWDG most likely **survives** into the bootloader.
> - **The bootloader does not feed it:** verified directly — `MCUBOOT_WATCHDOG_FEED()` in the core's `mcuboot_config.h` is a **no-op**. So if the IWDG is live, the bootloader will **not** service it.
> - **Normal OTA path makes this concrete:** the app calls `mbedWatchdog.start(30s)` long before `applyUpdate()`. The bootloader therefore most likely inherits a **live, partially-elapsed, unfed 30 s IWDG** — the *opposite* of "no watchdog."
> - **Optional `IWDG_SW` option-bit nuance:** whether the IWDG can ever be started in HW mode (auto-start, definitely unstoppable) depends on the `IWDG_SW` option byte; this should be read on the bench, not assumed.
>
> **Resolution for the plan:** treat 14.1 as a *hypothesis to disprove on the bench*, not a safety guarantee. Assume the IWDG is live and unfed during swap (the §13.6 position). Our safety actually comes from 14.2 (resumable swap) + measuring swap duration + kick-before-reset — see §14.4 — which is robust **whether or not** 14.1 turns out to be true.

### 14.2 MCUboot Resumable Swap Mechanism (The Failsafe)
Because the bootloader feed is a confirmed no-op (§13.5) and 14.1's "no watchdog" loophole is unverified, **this resumable-swap property is the *primary* safety net, not a secondary one.**
* **The Reality (verified):** the core is built with `MCUBOOT_SWAP_USING_SCRATCH`. The swap algorithm is stateful, operates sector-by-sector, and **journals its progress to flash** (the scratch area + status trailer).
* **The Implication:** if a reset (watchdog or power) interrupts the swap, the device is **not** bricked. On the next boot MCUboot reads the sector status and **resumes** where it left off.
* **Conclusion:** even if a swap took 40 s and a 30 s IWDG terminated it at ~75%, the next boot processes the remaining sectors and succeeds. The IWDG reset is merely an extra reboot, not a catastrophe.
* **Convergence caveat:** this only converges if **each** post-reset attempt makes ≥1 sector of *persisted* forward progress (true for scratch-swap) so it cannot loop forever doing the same work. Confirm on the bench that an interrupted swap actually advances rather than restarting from zero.
* **Performance caveat (now important):** `SWAP_USING_SCRATCH` is the **slowest** swap mode — it shuffles primary↔scratch↔secondary, so the Server's ~933 KB image moves several MB of flash total. This is exactly why the §13.3·4 swap-duration measurement matters: a slow scratch-swap is the scenario most likely to approach the IWDG window.

### 14.3 Custom Bootloader IWDG Injection (Last Resort)
If bench testing reveals that we *must* have watchdog coverage during the MCUboot phase (e.g., to prevent the bootloader itself from hanging if the QSPI chip fails), we cannot rely on the default core.
* **The Fix:** redefine the watchdog hook and **rebuild the MCUboot bootloader itself**, then reflash it.
* **The Implementation (corrected):** the hook to override is **`MCUBOOT_WATCHDOG_FEED()`** in `…/libraries/MCUboot/src/mcuboot_config/mcuboot_config.h` (currently `do {} while(0)`), **not** `os_watchdog_feed()`. Redefine it to call `HAL_IWDG_Refresh(&hiwdg)` so the bootloader pets the dog during hashing and flash writes.
* **Effort caveat (corrected):** this is **not** a tweak to the `manageBootloader` sketch. `manageBootloader` only flashes a **prebuilt** bootloader blob; to change the feed behavior you must **rebuild the Arduino MCUboot bootloader from its source project** and reflash it — with all the attendant key/partition/versioning risk. This is a significant undertaking.
* **Recommendation:** **Avoid for v1.9.1.** Rely on resumable scratch-swap (14.2) + swap-duration measurement + kick-before-reset (14.4). Only attempt a bootloader rebuild if bench metrics prove a swap cannot complete within the IWDG window across repeated resets.

### 14.4 Synthesis: the design that is safe regardless of the IWDG question

The key insight is that **we do not need to resolve the §14.1 silicon question to ship safely.** The following holds whether the IWDG is dead, live-and-unfed, or live-and-fed during the swap:

| Scenario during swap | What happens | Outcome |
|---|---|---|
| IWDG dead (14.1 true) | Swap runs uninterrupted | Succeeds |
| IWDG live + unfed (14.1 false, the conservative assumption) | IWDG may fire mid-swap → reset | **Resumable scratch-swap (14.2)** continues on next boot → succeeds after ≥1 extra reboot |
| Power loss mid-swap | Reset | Same as above → resumes → succeeds |

So the **mandatory v1.9.1 actions** (none require a bootloader fork) are:

1. **Kick-before-reset.** Make the last statement before `NVIC_SystemReset()` in `tankalarm_performMcubootUpdate()` a `dfuKickWatchdog()` so the bootloader inherits a **full** ~30 s window instead of a partially-elapsed one. (Folds into PR2/WS-E.)
2. **Measure scratch-swap duration per role** on the bench (Server ~933 KB is worst case) with `bootDebug` on. Record max swap time vs the 30 s window. (PR5/§13.3·4.)
3. **Prove resumable convergence:** force a reset mid-swap (e.g., power-cycle, or a deliberately short test watchdog) and confirm the next boot **resumes and completes** rather than restarting from zero or looping. This is the single test that retires the whole IWDG risk. (PR5/§13.3.)
4. **Keep `MCUBOOT_WATCHDOG_FEED()` modification out of scope** unless step 2 shows a swap that cannot converge within the window across repeated resets — then, and only then, escalate to §14.3.

**Bottom line:** the confirmed no-op `MCUBOOT_WATCHDOG_FEED()` makes 14.1's "no watchdog" comfort unreliable, but it does **not** change the plan, because resumable scratch-swap (14.2) already makes an interrupted swap recoverable. Treat 14.1 as a bench hypothesis, lean on 14.2 + measurement + kick-before-reset, and reserve the bootloader rebuild (14.3) as a documented last resort that we expect not to need.

---

## 15. Implementation Readiness & Closed Gaps (pre-flight)

This section closes the open questions from the readiness review so implementation can start without unknowns. Every item below was verified against the repo or the official Blues API; nothing here is assumed.

### 15.0 Readiness verdict

**PR1 (Foundation) is ready to start now** — zero open questions. **PR2/PR3 are ready** once the three additions below (build-seq, device-role, helper signature) are accepted into the plan; all three are designed concretely here. The only items that remain "verify on hardware" are the two already-documented bench gates (bootloader IWDG behavior §14, offline confirm-gate §13.1·1) — neither blocks writing code.

### 15.1 Correction: the Viewer already uses IAP (defect is Client-only)

Verified: `TankAlarm-112025-Viewer-BluesOpta.ino:1278` already calls `tankalarm_performIapUpdate(notecard, gDfuFirmwareLength, "continuous", dfuKickWatchdog)` — same as the Server. The infinite-watchdog-fed `card.dfu` hang (C2/C3) is **Client-only**. So:
- **Client** needs the *bug-ectomy* (delete ODFU + infinite loop) **and** migration to MCUboot.
- **Server and Viewer** need only the clean IAP→MCUboot *migration* (swap `performIapUpdate` → `performMcubootUpdate`).

This de-risks PR3 (C2.5 also covers the Viewer, not just the Server).

### 15.2 GAP CLOSED — monotonic build/sequence number (`FIRMWARE_BUILD_SEQ`)

**Problem:** §13.1·2 needs a value that increments on every release so "running == target" is unambiguous when reflashing the same `FIRMWARE_VERSION`. Today `TankAlarm_Common.h:18-33` has only `FIRMWARE_VERSION`, `NOTEFILE_SCHEMA_VERSION` (integer, currently 1 — but semantic, not a build counter), and `FIRMWARE_BUILD_DATE/TIME` (`__DATE__`/`__TIME__`, not monotonic).

**Design (one-line add, mirrors the existing pattern):**
```cpp
// TankAlarm_Common.h, next to FIRMWARE_VERSION
#ifndef FIRMWARE_BUILD_SEQ
#define FIRMWARE_BUILD_SEQ 191   // monotonic; bump every release (v1.9.1 = 191)
#endif
```
- **CI guard (WS-B):** assert `FIRMWARE_BUILD_SEQ` strictly increases vs the previous tag, and that it encodes the version (convention: `major*100 + minor*10 + patch`). One grep + compare in the version-consistency job.
- **Notehub carry:** when the app reports its version via `dfu.status {"version": …}`, use the **object** form (the API supports it) and set `ver_build` = `FIRMWARE_BUILD_SEQ`. That puts the monotonic value on the Notehub side too, for dashboards.
- **Use in WS-E:** `pending_ota.json` records `{ "target_seq": 191, "target_v": "1.9.1", "status": "trial" }`. Boot-time resolve compares the **running** `FIRMWARE_BUILD_SEQ` to `target_seq`. Same-version bench reflashes (seq unchanged) require manual confirm, exactly as §13.1·2 specifies.

### 15.3 GAP CLOSED — device-role constant + role marker

**Problem:** the H2 role check (so a Server image can't be staged on a Client) had nothing to compare against.

**Verified precedent:** `TankAlarm-112025-FTPS_Server_Test.ino:131-132` already defines `#define FIRMWARE_TARGET_SERVER 0` / `#define FIRMWARE_TARGET_FTPS_TEST 1` plus `firmwareTargetId()` → `"server"`/`"ftps-test"` and `firmwareTargetAssetNamingConvention()`. We mirror that exactly.

**Design (Common header + one define per sketch):**
```cpp
// TankAlarm_Common.h
#define TANKALARM_ROLE_CLIENT 1
#define TANKALARM_ROLE_SERVER 2
#define TANKALARM_ROLE_VIEWER 3
static inline const char* tankalarm_roleToken(uint8_t role) {
  switch (role) {
    case TANKALARM_ROLE_CLIENT: return "Client";
    case TANKALARM_ROLE_SERVER: return "Server";
    case TANKALARM_ROLE_VIEWER: return "Viewer";
    default: return "Unknown";
  }
}
```
```cpp
// top of each sketch (Client/Server/Viewer .ino), before including Common is fine via -D or here:
#define DEVICE_ROLE TANKALARM_ROLE_CLIENT   // (SERVER / VIEWER respectively)
```
**How the role check works (no in-image byte patching — per §11.1):** the device compares its compiled `tankalarm_roleToken(DEVICE_ROLE)` against the Notehub-reported `body.source`/`body.name` (see 15.5), rejecting a `ready` update whose source filename does not contain its role token. Because we publish role-named artifacts (`TankAlarm-Client-secure-v1.9.1.slot.bin`, etc.), `body.source` will contain `"Client"`/`"Server"`/`"Viewer"`. For GitHub Direct (Server), discovery already selects by exact filename, so the role is implicit in the asset name.

### 15.4 GAP CLOSED — `performMcubootUpdate()` / `TankAlarmDfuStatus` signatures

**Problem:** the helper is `tankalarm_performMcubootUpdate(Notecard&, uint32_t firmwareLength, const char* hubMode, void(*kickWatchdog)())` — no expected CRC/role in, so PR2's H2 validation had nothing to check. And `TankAlarmDfuStatus` only carries `firmwareLength` (verified: `tankalarm_checkDfuStatus` parses only `body.length` at `TankAlarm_DFU.h:144`).

**Design — extend the struct (populated from the now-verified `body` fields):**
```cpp
struct TankAlarmDfuStatus {
  bool     updateAvailable;
  bool     downloading;
  bool     error;
  uint32_t firmwareLength;     // body.length  (already parsed)
  uint32_t firmwareCrc32;      // body.crc32   (NEW — verified available)
  char     firmwareMd5[33];    // body.md5     (NEW — 32 hex + NUL)
  char     firmwareName[96];   // body.name    (NEW)
  char     firmwareSource[96]; // body.source  (NEW)
  char     version[32];
  char     mode[16];
  char     errorMsg[64];
};
```
**Design — new helper signature (pass the whole status + role so it can validate):**
```cpp
static bool tankalarm_performMcubootUpdate(
    Notecard &notecard,
    const TankAlarmDfuStatus &dfu,   // length + crc32 + name/source for H2
    const char *hubMode,
    uint8_t deviceRole,              // DEVICE_ROLE — for the role/source check
    void (*kickWatchdog)() = nullptr);
```
Inside, H2 validation becomes concrete: downloaded bytes `== dfu.firmwareLength`; running CRC32 `== dfu.firmwareCrc32` (the helper already computes a CRC32 — just compare it instead of discarding it); `strstr(dfu.firmwareSource, tankalarm_roleToken(deviceRole))` non-null; MCUboot header magic `0x96f3b83d` present. Only then write `pending_ota.json` and call `MCUboot::applyUpdate(false)`.

### 15.5 GAP CLOSED — `dfu.status body` fields are officially available

**Verified against the Blues Notecard API reference** (`dfu.status` response `body`). The documented example body is:
```json
"body": { "crc32": 2525287425, "created": 1599163431, "info": {}, "length": 42892,
          "md5": "5a3f73a7f1b4bc8917b12b36c2532969", "modified": 1599163431,
          "name": "stm32-new-firmware$20200903200351.bin", "notes": "...",
          "source": "stm32-new-firmware.bin", "type": "firmware" }
```

| Field | Type | Use in our plan |
|---|---|---|
| `length` | int | size check (already parsed) |
| `crc32` | int | **integrity** — compare to the helper's running CRC32 |
| `md5` | 32-hex | optional second integrity check / GitHub parity |
| `name` | string | role/source check (15.3) |
| `source` | string | role/source check (15.3) |
| `type` | string | sanity (`"firmware"`) |

**Consequence:** H2 validation is fully implementable **without a live device** — the spec is authoritative. (A 5-minute live `dfu.status` dump is still worth doing once to confirm Notehub populates `crc32`/`source` for *our* uploads, but it is no longer a blocker.) **Bonus:** `dfu.get {"binary":true}` returns COBS + an MD5 `status` per chunk and allows larger transfers — the optimization path noted in §11.2 is confirmed real for later.

### 15.6 GAP CLOSED — per-role **offline** confirm gates (concrete)

**Problem:** §13.1·1 requires the MCUboot confirm to be reachable without the cell network, but the exact per-role criteria weren't pinned to real signals.

**Verified signals (all exist today):** `gNotecardAvailable` is present in all three roles (`Client:832`, `Server:1146`, `Viewer:233`) and reflects **I2C** reachability (offline-safe, not network). Config load exists in all three (`loadConfig()` Server:4815; `gConfig` Client/Viewer). Client has `initializeRelays()` for safe outputs (`Client:1551`).

**Confirm gate per role (call `tankalarm_markFirmwareHealthy()` once these pass in `loop()`):**
- **Client:** QSPI/storage mounted **and** `loadConfig` succeeded **and** `initializeRelays()` left outputs in known-safe state **and** `gNotecardAvailable` (Notecard answered over I2C). *No* telemetry-ack requirement.
- **Server:** storage mounted **and** `loadConfig` succeeded **and** web/UI stack initialized **and** (`gNotecardAvailable` **or** Ethernet link up) — either transport proves "not bricked." *No* outbound-ack requirement.
- **Viewer:** storage mounted **and** `loadConfig` succeeded **and** `gNotecardAvailable`. *No* "summary received" requirement (that needs the network; treat it as a bonus signal only).

Rationale: every criterion is satisfiable at a no-signal bench, so a healthy unit that browns out before its first cell connection is **not** falsely rolled back (the failure mode §13.1·1 warned about).

### 15.7 Updated PR mapping (deltas from §13.2)

- **PR1 (Foundation):** also add `FIRMWARE_BUILD_SEQ` (15.2) and the `TANKALARM_ROLE_*` enum + `tankalarm_roleToken()` (15.3) to `TankAlarm_Common.h`, and `#define DEVICE_ROLE …` in each sketch. These are pure additions (no behavior change) and belong with the constants/guards work.
- **PR2 (Staging & state):** extend `TankAlarmDfuStatus` + populate the new fields in `tankalarm_checkDfuStatus()` (15.4/15.5); change the `performMcubootUpdate` signature (15.4); implement H2 using `crc32`/`source`; key `pending_ota.json` on `target_seq` (15.2).
- **PR3 (Runtime):** C2.5 now explicitly covers **Server and Viewer** (both already on IAP, 15.1); Client also gets the bug-ectomy. Per-role confirm gates use 15.6.
- **No change** to PR4/PR5/PR6.

### 15.8 Final readiness checklist

| Gap from readiness review | Status | Resolution |
|---|---|---|
| Build/sequence number missing | ✅ Closed | `FIRMWARE_BUILD_SEQ` design (15.2) |
| Device-role constant missing | ✅ Closed | `TANKALARM_ROLE_*` + token, mirrors FTPS precedent (15.3) |
| Helper can't validate (signature) | ✅ Closed | Extended struct + new signature (15.4) |
| `dfu.status` body fields unverified | ✅ Closed | Confirmed via official API: `crc32`/`md5`/`name`/`source` (15.5) |
| Per-role offline confirm gates undefined | ✅ Closed | Concrete gates on verified signals (15.6) |
| Viewer assumed broken like Client | ✅ Corrected | Viewer already on IAP; clean migration only (15.1) |
| Bootloader IWDG during swap | ⏳ Bench | §14 — mitigated by resumable swap; test, don't block coding |
| Offline confirm-gate proof | ⏳ Bench | §13.1·1 / 15.6 — verify on hardware |

**Conclusion: no blocking unknowns remain for writing code.** Start PR1 immediately; PR2/PR3 proceed with the designs in 15.2–15.6. The only remaining work that *must* happen on hardware is the two bench gates (§14 swap/IWDG, §13.1·1 confirm), and both have a defined test and a fallback, so neither blocks implementation.

