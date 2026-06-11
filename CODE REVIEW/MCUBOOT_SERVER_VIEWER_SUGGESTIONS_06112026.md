# Server & Viewer MCUboot / Storage Suggestions

**Date:** June 11, 2026
**Author:** GitHub Copilot (Claude Opus 4.8)
**Context:** Follow-up to the Client MCUboot provisioning work and the QSPI storage-conflict investigation.
**Companion docs:** [MCUBOOT_QSPI_STORAGE_CONFLICT_06102026.md](MCUBOOT_QSPI_STORAGE_CONFLICT_06102026.md), [MCUBOOT_BOOTLOADER_OPTIONS.md](MCUBOOT_BOOTLOADER_OPTIONS.md), [../Tutorials/Tutorials-112025/CLIENT_MCUBOOT_PROVISIONING_GUIDE.md](../Tutorials/Tutorials-112025/CLIENT_MCUBOOT_PROVISIONING_GUIDE.md)

---

## 1. Background & Decision Recap

The Client was fixed to mount its LittleFS config store on **QSPI MBR partition 4** and to never reformat the whole device, reserving **partition 2** for MCUboot OTA staging. The product decision was:

- **Clients** are the only remotely located devices → they get the full MCUboot OTA + rollback treatment.
- **Server and Viewer** are locally accessible → a simple USB `.bin` install is acceptable; brick-proof OTA is **not required** for them.

This document records the resulting **inconsistencies** in the Server and Viewer firmware and offers concrete, low-risk suggestions to make the codebase match that decision safely.

---

## 2. Key Finding: Server/Viewer are still wired for MCUboot but not storage-safe

Two facts are currently in tension:

1. **All three roles are built with `-DTANKALARM_DFU_MCUBOOT`** (CI workflow + `build/release-build.ps1`) and call the MCUboot engine:
   - Server: `tankalarm_resolvePendingOta()` (~L4178), `tankalarm_markFirmwareHealthy()` (~L4264), `tankalarm_performMcubootUpdate()` (~L8436) via `enableDfuMode()`.
   - Viewer: `tankalarm_resolvePendingOta()` (~L328), `tankalarm_markFirmwareHealthy()` (~L365), `tankalarm_performMcubootUpdate()` (~L1298).
2. **The Server still reformats the entire QSPI** as one LittleFS volume in `initializeStorage()` (~L4628):
   ```cpp
   mbedBD = BlockDevice::get_default_instance();   // whole 16 MB device
   mbedFS = new LittleFileSystem("fs");
   if (mbedFS->mount(mbedBD)) mbedFS->reformat(mbedBD);   // wipes MBR + partition 2
   ```

**Consequence:** If a Server were ever provisioned for MCUboot and an OTA were attempted, the first boot of the Server app would reformat the whole QSPI and **destroy the OTA staging partition** — the exact failure that bricked the test Client's OTA path. The Server is wired to *attempt* MCUboot but its storage layer is incompatible with it.

The **Viewer** does **not** have this conflict: it keeps its config in RAM / compile-time defaults and does not mount a local QSPI filesystem. It only *reads* partition 2 during `tankalarm_resolvePendingOta()`.

---

## 3. Suggestions for the Server

### Goal
Make the Server consistent with "simple USB `.bin` updates, no MCUboot OTA" — without leaving a latent QSPI-wipe hazard in the code.

### Option A — Disable MCUboot for Server builds *(recommended, lowest effort)*
Stop compiling the Server with `-DTANKALARM_DFU_MCUBOOT`, so the MCUboot apply/stage/confirm/rollback paths are not active and the existing `#else` fallback (`dfu.status stop=true`) is used instead.

- **Changes:** Remove the `--build-property "build.extra_flags=-DTANKALARM_DFU_MCUBOOT"` from the Server compile steps in [.github/workflows/release-firmware-112025.yml](../.github/workflows/release-firmware-112025.yml) and [build/release-build.ps1](../build/release-build.ps1). Leave Client (and optionally Viewer) on.
- **Pros:** Removes the brick hazard entirely; matches the decision; whole-device LittleFS stays valid (no MBR needed); no risky storage refactor.
- **Cons:** Server `.slot.bin` artifacts become meaningless and should be dropped from the Server release set to avoid confusion.
- **Note:** The Server keeps its existing IAP/Notecard or USB update path; verify which path remains active with the macro off.

### Option B — Make Server storage partition-aware (future-proof, higher effort)
Mirror the Client fix: mount the Server's LittleFS on **partition 4** and never reformat the whole device, then provision Servers with the same KeyProvisioning MBR layout.

- **Pros:** Server could later support MCUboot OTA; one consistent storage model across roles.
- **Cons:** Real refactor; **must verify the 7 MB partition 4 is large enough for the Server's hot+warm history tiers** (up to 3 months of daily summaries) before adopting — see §5. Existing Servers lose their whole-device LittleFS contents on migration.

### Recommendation
**Option A now**, because the Server is locally accessible and OTA isn't required for it. Revisit **Option B** only if remote Server updates become a requirement, and only after confirming the history footprint fits partition 4.

---

## 4. Suggestions for the Viewer

### Goal
Decide whether the Viewer should support MCUboot OTA; it is already storage-compatible either way.

### Observations
- The Viewer has **no local QSPI filesystem**, so there is **no storage conflict** — it will not wipe an OTA partition.
- It is wired for MCUboot (resolve/confirm/apply) and built with the macro.
- A Viewer that was never provisioned simply has no MBR; `tankalarm_resolvePendingOta()` handles the failed mount gracefully (logs and returns).

### Options
- **Option V1 — Keep MCUboot enabled for Viewer.** Because there is no storage conflict, the Viewer is the **safest role to validate the end-to-end MCUboot swap/rollback mechanism** on the bench (bootloader + keys + staging), decoupled from any app-storage concerns. Recommended if you want to prove the OTA pipeline at all.
- **Option V2 — Disable MCUboot for Viewer** (mirror Server Option A) if Viewers are always co-located with the Server and will only ever be updated over USB. Simplest operationally.

### Recommendation
If Viewers are sometimes remote (kiosks at separate sites), keep **V1** and provision them (bootloader + keys only — no partition-4 concern). If they are always local, **V2** for consistency with the Server. Either is safe; pick based on deployment topology.

---

## 5. Open Item: Server History Footprint vs Partition 4 (only relevant for Server Option B)

Before any Server MCUboot adoption, measure the worst-case on-device storage the Server consumes:

- Hot-tier ring buffer snapshots (`MAX_HOURLY_HISTORY_PER_SENSOR`).
- Warm-tier LittleFS daily summaries (`MAX_DAILY_SUMMARY_MONTHS = 3`).
- Client config snapshots, sensor registry, client metadata cache, calibration data.

If the realistic maximum exceeds the **7 MB** partition 4, adjust the partition map (e.g. shrink the unused partition 1/partition 3 and grow partition 4) in the provisioning sketch before committing to Option B.

---

## 6. Suggested Cleanup Tasks (independent of the above)

1. **Artifact hygiene:** If Server/Viewer drop MCUboot (Options A/V2), remove their `.slot.bin` lines from the release workflow's `files:` set so only OTA-capable roles publish slot images.
2. **Build-flag matrix doc:** Add a short table to the release runbook stating which roles compile with `-DTANKALARM_DFU_MCUBOOT` and why, so the inconsistency never silently returns.
3. **`enableDfuMode()` guarding:** For any role with MCUboot disabled, confirm the `#else` fallback cleanly reports "not supported" and stops the pending update (already present in Client/Server/Viewer) so a stray Notehub image cannot loop.
4. **Provisioning guard (defense in depth):** Consider having the Server/Viewer `initializeStorage()` detect an existing MBR (partition table) and refuse to reformat the whole device even if MCUboot is off — this prevents a future Server from wiping a deliberately-partitioned chip.

---

## 7. Summary Matrix

| Role | Remote? | Local QSPI FS? | Storage conflict today | Suggested update path | MCUboot macro |
|------|---------|----------------|------------------------|-----------------------|---------------|
| **Client** | Yes | Yes (now **partition 4**) | ✅ Fixed | MCUboot OTA + rollback | **On** |
| **Server** | No | Yes (**whole device**) | ⚠️ Yes (latent) | USB `.bin` (Option A) | **Off** (recommended) |
| **Viewer** | Maybe | No | ✅ None | USB `.bin`, or MCUboot if remote | On (V1) / Off (V2) |

**Bottom line:** The Client is correct and field-ready. The highest-value follow-up is resolving the Server's latent QSPI-wipe hazard — most cheaply by turning the MCUboot build flag **off** for the Server (Option A), since the Server does not need remote OTA. The Viewer is safe either way and is the best candidate for validating the OTA mechanism on the bench if desired.
