# Code Review — Field Client Diagnostics (MPPT Voltage, Current‑Loop Sensor, Config Delivery)

**Date:** 2026‑06‑23
**Author:** GitHub Copilot (AI review)
**Scope:** `TankAlarm-112025-Client-BluesOpta`, `TankAlarm-112025-Common` (solar/voltage/sensor paths),
`TankAlarm-112025-Server-BluesOpta` (config generator), `.github/workflows/release-firmware-112025.yml`
**Subject device:** `dev:860322068056545` — site **"Silas"**, sensor **"Cox Wellhead"** (gas, current‑loop), field/cellular
**Firmware context:** client OTA'd 1.9.42 → **2.0.41** successfully; **2.0.42** (observability) released and pending OTA.

---

## Executive summary

The OTA delivery pipeline is now healthy (see Issue 5 — resolved this session). Two field problems remain, and a
third (config delivery confirmation) is an open question rather than a confirmed bug:

| # | Issue | Most likely layer | Confidence | Blocking a firmware fix? |
|---|-------|-------------------|-----------|--------------------------|
| 1 | No battery voltage on dashboard (MPPT) | **Config** (poller not enabled) → then wiring/driver | Medium‑high | No — verify config first |
| 2 | Current‑loop sensor reads `0` / `sensor-fault` | **Hardware** (A0602 I²C bus) | High | No — bench/HW issue (deferred "Fix 11") |
| 3 | Config "applied" confirmation not seen yet | Async delivery / client inbound sync | Open | No |
| 4 | RS‑485 driver review (watch items, no blocking bug) | Firmware (minor) | — | — |
| 5 | OTA slot image build regression | CI (**RESOLVED** 2.0.41 + guard 2.0.42) | Done | — |

**Key theme:** for both open field issues, the firmware logic is currently *correct* — it is reporting the
problem accurately (a reused/stale sensor value; no voltage because no source is configured/answering). The
work is to (a) confirm the device's **applied config**, (b) make the failure **observable remotely** (done in
2.0.42), and (c) only then chase wiring/driver details if needed.

---

## Issue 1 — No battery voltage reported (SunSaver MPPT / RS‑485)

### Symptom
Dashboard shows no VIN/battery voltage for the client. The 2.0.41 telemetry carries **no** `v`, `vs`,
`solar`, or `bv` fields; the only "voltage" anywhere in the Notehub event window is the Notecard's
`_health.qo` `voltage_mode:"usb"` (~4.6 V) — which on the Blues "Wireless for Opta" carrier is the **regulated
~5 V rail** and is *always* present regardless of field power. It is **not** a battery reading and is
deliberately excluded from the voltage logic.

### Code path (verified)
- `sendTelemetry()` — [client .ino ~L5951](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5951): adds `doc["v"] = getEffectiveBatteryVoltage()` **only if > 0** (so voltage is sent every cycle when a source exists, not daily‑only).
- `sendDailyReport()` — [client .ino ~L7636](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7636): same gate.
- `getEffectiveBatteryVoltage()` — [client .ino ~L7362](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7362): returns > 0 **only** from Source 1 (SunSaver MPPT: `gSolarManager.isEnabled() && isCommunicationOk()`) or Source 3 (Vin divider: `vinMonitor.enabled && gVinVoltage > 0.5`). Notecard Source 2 is compile‑gated out on Opta.
- Poller is started **only if** `gConfig.solarCharger.enabled` — [client .ino ~L1636](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L1636).

### Root‑cause hypotheses (ranked)
1. **The client's *applied* config is still grid (poller never starts).** The cached config snapshot for this
   device (`client_config_cache.txt`, ~March 2026) reads `powerSource:"grid"`, `solarCharger:{enabled:false}`,
   `vinMonitor:{enabled:false}`. If that is still the live config, `getEffectiveBatteryVoltage()` correctly
   returns 0 and no code change helps. The operator selected "Solar + Modbus MPPT (RS‑485)" in the generator,
   but selection ≠ pushed‑and‑applied. **This is the cheapest thing to confirm and the most likely cause.**
2. **Config‑generator trap.** In the server generator
   ([server .ino ~L1999](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L1999)):
   `solarCharger.enabled = (powerSource === 'solar_modbus_mppt')` **only**, while
   `mpptEnabled = powerSource.includes('mppt')`. So **"Solar + Non‑Monitored MPPT"** sets `mpptEnabled=true`
   but leaves `solarCharger.enabled=false` → the RS‑485 poller stays off. Only the **"Modbus MPPT (RS‑485)"**
   option actually enables it (and the generator requires a non‑"none" battery type).
3. **Config applied, but RS‑485 link silently failing.** If `solarCharger.enabled=true` but the SunSaver/MRC‑1
   doesn't answer, the result is *identical* to "disabled" because the failure was silent (see Issue 4). 2.0.42
   makes this distinguishable via `scOk`.

### How to start exploring
1. **Verify applied config on the server** (you have it on the bench): load **Cox Wellhead** in the generator;
   confirm Power Source = "Solar + Modbus MPPT (RS‑485)" + a battery type; confirm a `config_ack` shows it
   **applied** (not merely selected). If it's still grid → **re‑push** — likely the whole fix.
2. **OTA v2.0.42**, then read telemetry: **no `scOk`** = not enabled in applied config; **`scOk:0`** = enabled
   but SunSaver not answering; **`scOk:1`** = link healthy.
3. If `scOk:0`: this is when (and only when) it becomes a wiring/driver hunt — see Issue 4.

### Status
Observability shipped in 2.0.42. Root cause pending the config verification + 2.0.42 OTA. **No driver change
made** (the code is sound; changing bench‑tuned timing on speculation is riskier than diagnosing first).

---

## Issue 2 — Current‑loop sensor reads `0` / recurring `sensor-fault` (A0602)

### Symptom / evidence (from the Notehub event log)
- Newest 2.0.41 telemetry: `{"st":"currentLoop","lvl":0,"ru":1,"pg":1, ... }` — i.e. **level 0, `ru:1`
  (reused/stale), no raw `ma` field**. The client could not obtain a fresh valid milliamp reading, so it
  reused the last value (0) and flagged it. `pg:1` shows the power‑gating transistor is enabled.
- Diagnostics: `diag.qo` `i2c-recovery` events (paired 1:1 with `sensor-fault`), all carrying **`fv:"1.9.42"`**
  (pre‑OTA). **2.0.41 raised no new `sensor-fault`** (count 0) — it reused instead.
- The `sensor-fault` alarm on the dashboard is therefore a **stale latch** from the 1.9.42 era, not a fresh fault.

### Root cause
The A0602 current‑loop module's **I²C bus is unreliable** — the recurring `i2c-recovery` + the inability to read
a valid `ma` is a **hardware/bus problem**, not a firmware logic bug. This is the previously‑deferred **"Fix 11"**
(see `CODE_REVIEW_06222026_DEFERRED_ITEMS.md`). v2.0.x changes made the dashboard report the condition
*accurately* (`ru:1`, no fabricated `0.0`) but cannot repair the physical bus.

### Two distinct sub‑problems
1. **The sensor itself isn't reading** (hardware) — needs bench investigation.
2. **The stale latched `sensor-fault` alarm** won't clear on its own while the sensor keeps faulting (and the
   client, reusing the value, isn't emitting a fresh fault/recovery transition). Server self‑clear logic exists
   for `sensor-stuck` ([server .ino ~L11787](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11787))
   but `sensor-fault` clears only on a client recovery note or manual clear.

### How to start exploring
1. **Hardware first (the real driver):** with the A0602 on the bench, run the validated probes already in the
   repo — `firmware/.../P1_Transistor_Gating_Test` and `Blueprint_CH0_Test` — and watch for the documented
   `~4.5 mA / ~1.6 PSI` healthy read. Check I²C wiring/pull‑ups, the P1 gating transistor, connector seating,
   and whether the A0602 NACKs under production I²C contention (Notecard + Modbus + sensor on the bus).
2. **Capture client serial** at a sample cycle (when bench‑accessible) for `"Failed to enable sensor power
   gating"` or current‑loop read errors.
3. **Alarm hygiene (optional firmware follow‑up):** consider clearing a latched `sensor-fault` on the server
   when fresh non‑fault telemetry arrives for that sensor (mirror the `sensor-stuck` self‑clear), so a recovered
   sensor doesn't keep a dead alarm lit. Only do this **after** the hardware read is restored, so we don't mask
   a genuine fault.

### Status
Deferred hardware issue (Fix 11). No firmware regression. Code is reporting it correctly.

---

## Issue 3 — Config "applied" confirmation not yet observed

### Observation
After pushing the Modbus‑MPPT config, no newer `config_ack.qo` from `dev:…545` was visible (last readable
events were the 9:14 PM OTA acks).

### Why this may be expected (not a bug)
Config delivery is **asynchronous**: server publishes → Notehub **queues** it for the client (via the
`ServerToClientRelay` route) → the client applies it on its **next inbound sync** → emits `config_ack.qo`. For a
field cellular client the inbound sync timing governs when the ack appears.

### What to verify
- **Server outbound** config note (`dev:…529`) confirms it was *sent*.
- **Client `config_ack.qo`** (`dev:…545`) confirms it was *applied* — this is the real confirmation.
- **Pending notes** on the client device page: if the config sits in *pending*, the client hasn't pulled it yet.

### Watch item (possible latent issue)
The **server** previously suffered a stale Notecard inbound‑sync condition (reported "connected" but didn't pull
queued notes — resolved by `sync:true` + periodic `hub.sync`, see build notes). If the **client** never acks
after a reasonable window, suspect the analogous client‑side inbound‑sync staleness, not the config content.

---

## Issue 4 — RS‑485 / SunSaver driver review (no blocking bug; watch items)

`TankAlarm_Solar.h` / `TankAlarm_Solar.cpp` are **mature** and already incorporate prior bench fixes:
`SERIAL_8N2`, `setTimeout` clamped ≥ 500 ms, `RS485.setDelays(0, 1200)` (post‑delay fix), bench‑verified live
ADC registers `0x0008–0x000C`, scaling `96.667 / 79.16`, FC03↔FC04 fallback with caching, plausibility clamps,
comm‑failure threshold. **No blocking defect found.** Watch items if Issue 1 resolves to `scOk:0`:

1. **`setDelays(0, 1200)` pre‑delay = 0.** Post‑delay was the documented fix; the **pre‑delay (DE‑assert →
   first byte) is 0**. For some transceivers/the MRC‑1 path this can clip the first byte. First knob to sweep —
   your `firmware/sunsaver-rs485-windowed-probe` already sweeps pre/post delays.
2. **`alertOnCommFailure` defaults `false`** (config generator doesn't set it). Combined with the (now‑fixed)
   silent daily omission, this is *why* the failure was invisible. Consider defaulting it **on** when
   `solarCharger.enabled` (or surface comm state on the dashboard) so field links self‑report.
3. **Silent‑failure gap — partially fixed in 2.0.42.** `sendTelemetry` now emits `scOk`, and
   `appendSolarDataToDaily` now emits `{commOk:0,errs:N}` on failure instead of omitting the block. Remaining
   follow‑up: have the **server** parse/display `scOk`/`commOk` on the dashboard (currently visible only in raw
   note bodies / event log).
4. **No raw‑frame diagnostic.** If we reach a true link hunt, add a one‑shot client diagnostic that dumps the
   exact Modbus request bytes and any response (or timeout) for `SS_REG_BATTERY_VOLTAGE`, so we can see what the
   SunSaver returns without a site visit.

---

## Issue 5 — OTA slot‑image build regression (RESOLVED this session, for context)

Documented here so the timeline is complete:
- **Bug:** commit `2926b11` (a "release notes" commit) silently reverted the client OTA build from the core
  `security=sien` path back to a plain build + external imgtool + custom keys. The resulting `.slot.bin` was
  linked for the wrong address (`0x08040000` vs the MCUboot slot `0xA0000000`) and signed with keys the Opta
  bootloader doesn't trust → MCUboot reverted every trial boot ("trial boot reverted by MCUboot").
- **Fix:** restored the `security=sien` build step (v2.0.41), re‑released as 2.0.41 (2.0.40 got blacklisted
  on‑device after the revert).
- **Guard:** v2.0.42 CI adds a **"Validate client OTA slot image (MCUboot magic)"** step that fails the release
  if the slot `.bin` doesn't start with `3d b8 f3 96` (MCUboot `IMAGE_MAGIC`). A plain build starts with the
  STM32 vector table (`00 00 08 24`).
- **Prevention (process):** `git pull --rebase` before editing/committing; one logical change per commit; review
  the full diff (including files you didn't mean to touch); consider not committing `build/*.bin` + the Common
  `.zip` (stale‑copy surface); branch protection + PR review on master. The root mechanism was a **stale
  single‑file copy** committed on a linear history (no merge conflict to flag it).

---

## Prioritized exploration plan (start here)

1. **Confirm the client's *applied* config** on the server (Cox Wellhead → Power Source = Solar + Modbus MPPT
   (RS‑485) + AGM). If still grid → **re‑push**. *(Cheapest; likely resolves Issue 1.)*
2. **Confirm the config ack** (`config_ack.qo` from `dev:…545`). If none after a reasonable window → suspect
   client inbound‑sync staleness (Issue 3 watch item), not the config.
3. **OTA v2.0.42** and read `scOk` in telemetry to classify Issue 1 precisely: not‑enabled / not‑answering /
   healthy.
4. If `scOk:0` → RS‑485 link hunt (Issue 4): sweep pre‑delay, verify MRC‑1 power + A/B polarity + slave ID/baud,
   add the raw‑frame diagnostic.
5. **Issue 2 (sensor) is a parallel hardware track** — run the A0602 bench probes; it does not block the voltage
   work and is not a firmware bug.

---

## Appendix — file/line reference map

| Concern | Location |
|---|---|
| `getEffectiveBatteryVoltage()` (voltage sources) | [client .ino ~L7362](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7362) |
| Telemetry voltage + `scOk` (2.0.42) | [client .ino ~L5951](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5951) |
| Daily voltage + solar block (`commOk`, 2.0.42) | [client .ino ~L6539](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L6539), [~L7636](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7636) |
| Solar poller start / poll | [client .ino ~L1636](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L1636), [~L2029](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L2029) |
| Analog live‑zero guard (Fix 2) | [client .ino ~L5305](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5305) |
| Sensor data‑quality flags `sf`/`ru` | [client .ino ~L5921](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5921) |
| SunSaver driver (`begin`/`readRegisters`) | [TankAlarm_Solar.cpp](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp), [TankAlarm_Solar.h](../TankAlarm-112025-Common/src/TankAlarm_Solar.h) |
| Config generator (`mpptEnabled` vs `solarCharger.enabled`) | [server .ino ~L1999](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L1999) |
| Daily alarm reconcile skip‑list (Fix 9) | [server .ino ~L12261](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12261) |
| `sensor-stuck` self‑clear | [server .ino ~L11787](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11787) |
| OTA slot‑image guard | [release-firmware-112025.yml](../.github/workflows/release-firmware-112025.yml) |
