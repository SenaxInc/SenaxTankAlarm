# A0602 Expansion Addressing — Findings & Code‑Change Test Plan

**Date:** 2026‑06‑24
**Author:** GitHub Copilot (AI)
**Status:** Findings + proposed changes to test on a **different computer** — **no production firmware changed**
**Scope:** Why the A0602 4‑20 mA current‑loop read is wrong/stuck, and the candidate code changes to validate.
**Companion docs:**
`CODE REVIEW/OPTA_A0602_USB_DIAGNOSTIC_PLAN_06242026.md` (USB test plan + decision tree),
`CODE REVIEW/OPTA_I2C_BUS_SEPARATION_FEASIBILITY_06242026.md` (the v2.0.46 contention plan that shipped and did **not** fix it),
`CODE REVIEW/CODE_REVIEW_06152026_CURRENT_LOOP_MA_READING.md` (original symptom write‑up).

---

## 0. TL;DR

- The production firmware talks to the A0602 over a **raw, hand‑rolled I²C** protocol and **never runs
  `OptaController`** (the official Arduino_Opta_Blueprint controller). As a result the A0602 is **never taken
  through the Blueprint bootstrap** — it is left **unmanaged**.
- The field‑observed **RED status LED on the A0602 means exactly that: "ready for address / unmanaged."** The
  healthy, controller‑managed state is **GREEN ("has address")**. This is grounded in the Blueprint library
  source, not inferred (see §1).
- **`0x64` is not a Blueprint address.** The Blueprint unmanaged default is **`0x0A`**, and assigned addresses
  start at **`0x0B`**. The client already carries `0x0A` as a fallback (`CURRENT_LOOP_I2C_ALT_ADDRESS_1`),
  which is a tell that the author anticipated talking to an unmanaged module.
- Therefore the v2.0.46 work — which treated this as **I²C bus *contention*** between the Notecard and the
  A0602 — was almost certainly aimed at the **wrong axis**, which is why it changed nothing in the field.
- **The fix to test:** drive/read the A0602 through the **official `OptaController` / `AnalogExpansion`
  managed API** (the same path the validated `Sensor_Utility` / `Blueprint_CH0_Test` use), so the module is
  addressed/managed (**GREEN**) and read through the validated current‑ADC path — instead of the raw shortcut
  against an unmanaged module. The candidate changes are listed in §4.
- We could **not** finish on this workstation: the unit is an **MCUboot** board (rejects unsigned sketches),
  and this host's **USB‑CDC is unstable** and the locally‑signed image was **rejected** (back to DFU). The
  next session should run on the **computer that already provisions/flashes this board** (§5 has the exact
  procedure and the ready‑to‑use diagnostic sketch).

---

## 1. The addressing findings (source‑grounded)

All citations are from the installed library
`C:\Users\<user>\Documents\Arduino\libraries\Arduino_Opta_Blueprint\src`.

### 1.1 The RED status LED = UNMANAGED

`OptaAnalog.cpp` drives the A0602's RGB **status** LED with three states:

| LED | Function (`OptaAnalog.cpp`) | RGB | Meaning |
|---|---|---|---|
| **GREEN** | `setStatusLedHasAddress()` (~L2604) | GREEN on | **Healthy** — a controller assigned it an address; module is **managed**. |
| **RED** | `setStatusLedReadyForAddress()` (~L2592) | RED on | Booted, **ready to receive** an address, but **none assigned** → **UNMANAGED**. |
| **BLUE** | `setStatusLedWaitingForAddress()` (~L2598) | BLUE on | Initial power‑up, signalling it wants an address. |

A module that has completed the Blueprint handshake shows **GREEN**. The field unit shows **RED** → it never
completed the handshake → **nothing is managing it.** This is the expected consequence of the production
firmware never calling `OptaController.begin()`.

### 1.2 `0x64` is not a Blueprint address; `0x0A` is the unmanaged default

`OptaBluePrintCfg.h`:
- `OPTA_DEFAULT_SLAVE_I2C_ADDRESS = 0x0A` — where an **unmanaged** module listens.
- `OPTA_CONTROLLER_FIRST_AVAILABLE_ADDRESS = 0x0B` — first address a controller **assigns**.

The client (`TankAlarm-112025-Client-BluesOpta.ino`):
- `CURRENT_LOOP_I2C_ADDRESS = 0x64` (~L245) — the project convention it talks to.
- `CURRENT_LOOP_I2C_ALT_ADDRESS_1 = 0x0A` (~L249) — **exactly the unmanaged Blueprint default**.
- `resolveCurrentLoopI2cAddress()` (~L1452) probes `0x64` → `0x0A` and logs
  `I2C: current loop address override 0x64 -> 0x0A` when it falls back.

**Implication:** if the module is unmanaged it is at `0x0A`, not `0x64`. Whether the raw framed protocol even
works against an unmanaged module (which may only honor the address‑assignment command) is the open question
the managed‑vs‑raw test answers.

### 1.3 The channel LEDs are host‑set — and positions 2 & 4 persisted across a power cycle

`OptaAnalog.cpp` (~L2250) sets the 8 channel LEDs only from a host command:
`led_status = rx_buffer[OA_SET_LED_VALUE_POS]`. They are **not** auto‑lit by config/PWM/current in the code we
read. **Yet positions 2 & 4 stayed yellow through a full power cycle.** That is an anomaly worth resolving on
the bench, because it means one of:
1. those LEDs are driven by an A0602‑firmware/hardware condition we did not find in the controller‑side source
   (e.g., a per‑channel state/diagnostic), **or**
2. the A0602 was **not** actually power‑cycled (it can be back‑powered via the AUX bus / external supply), **or**
3. they reflect real per‑channel wiring/state on channels 2 and 4.

The managed read (§4, Change 1 / the diagnostic sketch) will report each channel's actual function and value,
which should explain it.

### 1.4 Production uses raw I²C, never the managed controller

Verified by search across the client + Common: there is **no** reference to `OptaController`,
`AnalogExpansion`, `beginChannelAsCurrentAdc`, or `updateAnalogInputs` in production. It uses the raw helpers in
`TankAlarm-112025-Common/src/TankAlarm_I2C.h`:
- `tankalarm_setPwm()` — P1 high‑side gate (raw Blueprint command frame).
- `tankalarm_configureCurrentAdcChannel()` — raw `SET ARG_OA_CH_ADC` frame.
- `tankalarm_readCurrentAdcFramed()` — raw `GET ARG_OA_GET_ADC` frame.
- `tankalarm_getAnalogChannelFunction()` — raw `GET_CHANNEL_FUNCTION (0x40)`.

These are called from `readCurrentLoopSensor()` (~L5358) in the client. By contrast, the validated
`TankAlarm-112025-Sensor_Utility.ino` reads correctly on the bench using the **official** API
(`OptaController.begin()` → `AnalogExpansion::beginChannelAsCurrentAdc()` → `exp.updateAnalogInputs()` →
`exp.pinCurrent()`).

---

## 2. Why v2.0.46 (contention) was the wrong axis

- The field signature is a **stable, repeatable, physically‑impossible, timing‑immune** value (~18 mA / 43.8 psi
  at ~0 psi). Bus *contention* produces **intermittent** corruption with **intermittent successes** — we never
  saw successes. A constant wrong value is the signature of **talking to a module that is not in a valid read
  state** (unmanaged / not in current‑ADC mode / wrong address), not of occasional collisions.
- v2.0.46 hardened **sequencing/timing** on the shared bus (scoped 25 ms timeout, per‑op 400 kHz window,
  `0xFFFF`/over‑range rejection, PWM‑NACK fail‑safe, `GET_CHANNEL_FUNCTION` confirm) and **did not change the
  reading**. A fix that fails is evidence against its hypothesis.
- The RED LED + the raw‑only/unmanaged path + the `0x0A` fallback all point at **management/addressing**, not
  contention.

> Note: keep the v2.0.46 defensive guards (they are correct and cheap), but stop tuning contention until the
> managed‑vs‑raw test (below) says the bus is actually the constraint.

---

## 3. The decisive test (already built, ready to run)

`TankAlarm-112025-A0602_Diagnostic/TankAlarm-112025-A0602_Diagnostic.ino` runs the discriminator in one boot:

1. **PHASE 1 — RAW before managed:** full bus scan, then the exact production sequence
   (`setPwm P1 on` → `configureCurrentAdcChannel` → `getAnalogChannelFunction` → `readCurrentAdcFramed`) at
   **0x64 and 0x0A**, dumping **raw bytes + CRC** for every transaction.
2. **PHASE 2 — MANAGED:** `OptaController.begin()` (A0602 status LED should turn **GREEN**), enumerate type +
   assigned address, configure channels as current ADC, read `pinCurrent()`/`getAdc()`.
3. **PHASE 3 — RAW after managed:** repeats the raw path at 0x64, 0x0A, and the **assigned** address.

It prints a **Branch ①/②/③ verdict**:
- **① managed reads ≈4 mA, raw‑before did not** → the production **raw/unmanaged path is the bug** (do §4 Change 1).
- **② managed enumerates but reads wrong/READ FAIL** → loop power / channel / wiring (limited further over USB).
- **③ `OptaController` does not enumerate / LED never green** → AUX bus or module power/health.

This sketch already builds clean (plain `arduino:mbed_opta:opta`) and also builds **signed** (`security=sien`,
MCUboot magic `3d b8 f3 96` verified). Run it **first** on the other computer to lock the branch before changing
production code.

---

## 4. Code changes to test (the list)

> Validate **Change 1** first; it is the hypothesis‑confirming fix. The rest are dependent/supporting. Each item
> says **where**, **what**, **why**, **how to test**, **risk**.

### Change 1 — Migrate current‑loop acquisition to the official managed API (PRIMARY)
- **Where:** `readCurrentLoopSensor()` (client `.ino` ~L5358) + setup()/loop(); add `#include "OptaBlue.h"`.
- **What:**
  - In `setup()` (after `Wire.begin()`): call `OptaController.begin()` once; call `OptaController.update()` in
    `loop()` (or immediately before a read).
  - In `readCurrentLoopSensor()`: replace the raw `configureCurrentAdcChannel()` + `readCurrentAdcFramed()`
    pair with `AnalogExpansion exp = OptaController.getExpansion(devIdx);`
    `AnalogExpansion::beginChannelAsCurrentAdc(OptaController, devIdx, ch);` `exp.updateAnalogInputs();`
    `float ma = exp.pinCurrent(ch, false);`.
  - Resolve `devIdx` from `OptaController.getExpansionNum()` / `getExpansionType()` (analog), not from `0x64`.
- **Why:** drives the module to **GREEN/managed**, assigns a real address, and reads through the validated
  current‑ADC path that already works on the bench (`Sensor_Utility`). Removes the unmanaged/raw/`0x64`
  assumptions entirely.
- **How to test:** flash the candidate; confirm the A0602 LED goes **GREEN**; confirm `ma ≈ 4` at ~0 psi on CH0
  and tracks pressure; confirm telemetry `ma`/`lvl` are sane.
- **Risk:** `OptaController.begin()` re‑addresses the module and **sets `Wire` to 400 kHz** (see Change 3); it
  coexists on the shared bus with the Notecard — must verify the Notecard still syncs (§Change 3).

### Change 2 — If keeping the raw read: manage the module first, then read at the assigned address (FALLBACK)
- **Where:** client `setup()` + `resolveCurrentLoopI2cAddress()`.
- **What:** call `OptaController.begin()`/`update()` at startup **only** to assign the module an address (→ GREEN),
  capture `getExpansionI2Caddress()`, and point the raw read path at that assigned address instead of `0x64`.
- **Why:** smaller diff than Change 1 if the raw framed reads work once the module is *managed*; the diagnostic
  sketch's "raw‑after‑managed at the assigned address" phase tells us whether this is viable.
- **How to test:** confirm LED GREEN after boot; confirm the raw framed read returns sane `ma` at the assigned
  address; compare against Change 1.
- **Risk:** mixing raw frames with a controller‑managed module may fight `OptaController.update()`; generally
  Change 1 is cleaner. Treat this as a fallback only.

### Change 3 — Notecard coexistence: restore `Wire` to 100 kHz around managed ops
- **Where:** wherever `OptaController.begin()` / `update()` runs (`OptaController::begin()` calls
  `Wire.setClock(400000)`).
- **What:** after any OptaController call, `Wire.setClock(I2C_NORMAL_CLOCK_HZ /*100000*/)` before the next
  Notecard transaction (mirror the existing scoped‑clock pattern in `readCurrentLoopSensor()`).
- **Why:** the Notecard is documented‑unreliable at 400 kHz on this Mbed/Opta port; `OptaController` forces
  400 kHz. Without restoring 100 kHz, Change 1/2 could regress the Notecard.
- **How to test:** after the change, run several `hub.sync` / telemetry cycles and confirm no Notecard request
  failures.
- **Risk:** low; it is the same clock‑restore discipline v2.0.46 already uses for the A0602 burst.

### Change 4 — Migrate P1 gating to the managed API too (one abstraction)
- **Where:** `readCurrentLoopSensor()` P1 enable/disable (currently `tankalarm_setPwm()`).
- **What:** if Change 1 lands, gate the loop power through the same `AnalogExpansion` object (its PWM / high‑side
  output API) so input **and** output use one managed abstraction (review recommendation #9), instead of raw
  `setPwm` at `0x64` while reads go through the managed address.
- **Why:** avoids talking to two different addresses/abstractions for the same physical module; removes the last
  raw‑`0x64` dependency.
- **How to test:** confirm P1 still switches (transmitter powers during the read window) and the read is valid;
  confirm low‑power gating (P1 off between reads) still works.
- **Risk:** medium — must confirm the managed PWM/high‑side API maps to the same P1 terminal the wiring uses.

### Change 5 — Diagnostics: log managed enumeration + assigned address; compare to configured 0x64
- **Where:** client `setup()` after `OptaController.begin()`; telemetry/daily report.
- **What:** log `getExpansionNum()`, expansion type, and `getExpansionI2Caddress()`; emit a compact telemetry
  field (e.g., `a0602_addr`, `a0602_mgmt_ok`) so the field shows whether the module is managed and at what
  address. Keep the v2.0.46 `cl_ok`/`cl_fault` counters.
- **Why:** turns "is it managed?" into an observable fact in the field, not a guess.
- **How to test:** confirm the logged/telemetered address matches the GREEN‑state assigned address.
- **Risk:** low (additive).

### Change 6 — Keep the v2.0.46 guards (no change, just don't regress)
- `tankalarm_readCurrentAdcFramed()` rejects raw `0xFFFF`; caller rejects > `CURRENT_LOOP_OVER_RANGE_MA` (21.0);
  under‑range `< CURRENT_LOOP_FAULT_MA` (3.6) → `NAN`; PWM‑enable NACK → `NAN`; `cl_fault`/`cl_ok` daily counters.
- **Why:** these correctly prevent a bad frame from masquerading as a real reading; retain them under the managed
  path too (validate the managed `pinCurrent()` NaN/again maps into the same fault accounting).

---

## 5. How to build / flash / capture on the other computer

**This unit is the production MCUboot client.** Plain unsigned sketches flash but **will not boot** (the
bootloader rejects them → DFU). Two hard‑won facts:

1. **Build signed** (public default keys, same as production):
   ```powershell
   arduino-cli compile --fqbn "arduino:mbed_opta:opta:security=sien" `
     --build-property "build.extra_flags=-DTANKALARM_DFU_MCUBOOT" `
     --build-property "build.version=2.0.46+240" `
     --output-dir "build\<name>-secure" "<SketchDir>"
   ```
   The output `*.ino.bin` **must** start with the MCUboot magic **`3d b8 f3 96`** (else it is a plain build the
   bootloader will reject). The diagnostic sketch already includes the required
   `#if defined(TANKALARM_DFU_MCUBOOT)` `MCUboot::confirmSketch();` at the top of `setup()`.
2. **Flash over DFU** (board in DFU = double‑tap RESET; device `2341:0364`, app slot alt 0 @ `0x08000000`):
   ```powershell
   dfu-util --device 2341:0364 -a 0 --dfuse-address=0x08040000:leave -D "<signed>.ino.bin"
   ```
   Then **press RESET once** so MCUboot validates and boots the app.
3. **Open serial with DTR/RTS OFF** (`DtrEnable=$false; RtsEnable=$false`). **This is critical:** asserting DTR
   on connect tells the bootloader to abort and re‑enter DFU (it is what produced the "device not functioning"
   failures here). With DTR off, opening does not reset the app either, so trigger a run by sending `r`.

**Open blocker we hit on this host (carry forward):** even the correctly‑signed diagnostic (magic verified) went
**back to DFU after RESET** — MCUboot rejected it at boot. Most likely an **encryption/provisioning mismatch**
for *this* unit+host (`security=sien` = Signature **+ Encryption**; the device decrypts with its provisioned
key). The other computer — the one that already provisioned/flashed this board — should not have this problem.
If it persists there:
- Re‑run the **KeyProvisioning** flow (loads the default keys + QSPI partitions) per
  `Tutorials/Tutorials-112025/CLIENT_MCUBOOT_PROVISIONING_GUIDE.md`, then re‑flash the signed image; **or**
- Confirm the bench Arduino‑IDE setup that historically flashes this Client, and build the signed image there.

> The Server/Viewer are **not** MCUboot — but the A0602 lives on the **Client**, so testing must be on the
> Client. The validated bench sketches `Sensor_Utility` (managed reads) and `Blueprint_CH0_Test` are the
> fastest way to independently confirm the module reads correctly via the official API before/after Change 1.

---

## 6. Suggested test order on the other computer

1. **Flash + run `TankAlarm-112025-A0602_Diagnostic` (signed).** Read the Branch ①/②/③ verdict. Capture the full
   PHASE 1/2/3 serial dump. (Confirms the diagnosis before any production edit.)
2. If **Branch ①**: implement **Change 1** (+ Change 3) on a client branch; flash signed; confirm A0602 LED
   GREEN and `ma ≈ 4` at ~0 psi; verify Notecard still syncs.
3. Add **Change 5** (managed‑address telemetry) and, if needed, **Change 4** (managed P1 gating).
4. Keep **Change 6** intact. Bump version, write release notes, run the normal signed‑slot CI/tag flow.
5. If **Branch ② or ③**: stop and treat as hardware/loop‑power/AUX‑bus (limited further over USB) — see the
   companion USB plan §5 decision tree.

---

## 7. Verified facts & citations (for the next session)

- Blueprint status LED states — `OptaAnalog.cpp` `setStatusLedReadyForAddress` (RED, ~L2592) /
  `setStatusLedWaitingForAddress` (BLUE, ~L2598) / `setStatusLedHasAddress` (GREEN, ~L2604).
- Addresses — `OptaBluePrintCfg.h`: `OPTA_DEFAULT_SLAVE_I2C_ADDRESS 0x0A`,
  `OPTA_CONTROLLER_FIRST_AVAILABLE_ADDRESS 0x0B`. `0x64` is a project convention, not a Blueprint address.
- Channel LEDs host‑set — `OptaAnalog.cpp` ~L2250 `led_status = rx_buffer[OA_SET_LED_VALUE_POS]`.
- Production raw path — `TankAlarm_I2C.h` (`tankalarm_setPwm` / `configureCurrentAdcChannel` /
  `readCurrentAdcFramed` / `getAnalogChannelFunction`); caller `readCurrentLoopSensor()` (client ~L5358).
  `0xFFFF` rejected in `readCurrentAdcFramed()`; over‑range (`CURRENT_LOOP_OVER_RANGE_MA = 21.0`) + under‑range
  (`CURRENT_LOOP_FAULT_MA = 3.6`) in the caller; per‑op `A0602_PEROP_I2C_CLOCK_HZ = 400000` restored to
  `I2C_NORMAL_CLOCK_HZ = 100000`.
- Managed path (validated on bench) — `Sensor_Utility.ino`: `OptaController.begin()`,
  `AnalogExpansion::beginChannelAsCurrentAdc()`, `exp.updateAnalogInputs()`, `exp.pinCurrent()`,
  `getExpansionI2Caddress()`. `OptaController::begin()` calls `Wire.setClock(400000)`.
- Flash/boot — MCUboot v.25 bootloader; signed image magic `3d b8 f3 96`; app slot `0x08040000` alt 0;
  open serial **DTR/RTS off**; `dfu-util` at
  `…\Arduino15\packages\arduino\tools\dfu-util\0.10.0-arduino1\dfu-util.exe`.

---

*This document is findings + a change‑test plan only. No production firmware was modified. Start the next
session by running the signed `A0602_Diagnostic` sketch to confirm Branch ①, then implement Change 1.*
