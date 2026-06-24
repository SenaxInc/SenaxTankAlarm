# Opta AUX‑Port I²C Communication Review — Shared‑Bus Contention (Notecard + A0602)

**Date:** 2026‑06‑23
**Author:** GitHub Copilot (AI review)
**Firmware reviewed:** v2.0.45 (client) on `master`
**Hardware:** Arduino Opta Lite (STM32H747) + Blues "Wireless for Opta" Notecard + Arduino Pro Opta Ext **A0602**
**Scope:** the Opta AUX expansion connector I²C bus — what is on it, the relevant Arduino standard, and whether
bus contention explains the field A0602 current‑loop failures.
**Builds on:** `OPTA_I2C_COMMUNICATION.md` (2026‑02‑20), `I2C_COMPREHENSIVE_ANALYSIS_02262026.md`,
`I2C_REVIEW_SUMMARY_02262026.md`. This note brings those current to the v1.9.23 framed‑protocol / v2.0.45 era and
answers the three specific questions raised.

---

## Executive summary (the three questions answered)

1. **Is there an Arduino standard for the AUX port?** Yes — `Arduino_Opta_Blueprint` (`OptaController` +
   `AnalogExpansion`). It drives the AUX expansion on the **default `Wire`** bus at **400 kHz**, ISR‑served on
   the expansion side, with the controller as the **sole I²C master**. Production does **not** use it — it
   hand‑rolls the Blueprint wire protocol on raw `Wire` (deliberately, see below).

2. **Is the AUX bus shared?** **Yes — this is the core issue.** The Blues Notecard (**0x17**) and the A0602
   (**0x64**) are on the **same physical `Wire` bus**. Verified in code: `Notecard::begin()` →
   `make_note_i2c(Wire)`, and every A0602 access in `TankAlarm_I2C.h` uses `Wire`. There is only one `Wire`
   peripheral (one SDA/SCL pair), and per the Blues "Wireless for Opta" datasheet the carrier daisy‑chains the
   Notecard onto the **AUX connector's I²C** — the same connector the A0602 snaps onto.

3. **Are "too many messages using I²C at the same time"?** **Not literally simultaneously** — there is a single
   I²C master (the single‑threaded host loop), so transactions are **serialized**; there is no multi‑master
   collision. **But the bus is over‑subscribed**: two independent, mutually‑unaware protocols (Notecard
   `note‑arduino` and the hand‑rolled Blueprint) interleave on one bus running at the **slow 100 kHz**, with no
   bus lock or transaction isolation, while Notecard cellular operations can hold the bus for hundreds of ms to
   seconds. That timing coupling — not simultaneity — is what makes the A0602 reads fail in production while they
   succeed standalone on the bench.

> **Bottom line:** the A0602 problem is an **architecture/topology** issue (one shared, slow, un‑arbitrated bus
> carrying both a blocking cellular modem and a timing‑sensitive analog expansion), not a defect in the read
> code. The real fix is **bus separation**; everything else is mitigation.

---

## 1. What is physically on the AUX I²C bus

| Device | I²C addr | Library / access | Native speed | Role |
|---|---|---|---|---|
| **Blues Notecard** (Wireless for Opta) | **0x17** | `note‑arduino` → `make_note_i2c(Wire)` | reliable at **100 kHz** on this Mbed port | Cellular modem; **blocking** during sync |
| **Arduino Pro Opta Ext A0602** | **0x64** | **hand‑rolled** Blueprint frames on raw `Wire` (`TankAlarm_I2C.h`) | **400 kHz** (Blueprint design) | 4‑20 mA current‑loop ADC + P1 PWM gate |
| Host (Opta STM32H747) | master | `Wire` | — | single master, single‑threaded loop |

**One bus, two masters' worth of traffic.** Both devices share the single `Wire` peripheral. The STM32H747 has
four I²C peripherals (I2C1–I2C4), but the firmware only ever opens `Wire` — so the AUX expansion and the Notecard
are electrically and logically the **same bus**.

`CURRENT_LOOP_I2C_ADDRESS 0x64` ([client .ino L245](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L245)),
`NOTECARD_I2C_ADDRESS 0x17` ([Common.h](../TankAlarm-112025-Common/src/TankAlarm_Common.h)).

---

## 2. The Arduino standard — and why production deliberately deviates

### 2.1 The standard: `Arduino_Opta_Blueprint`
The vendor library `OptaController::begin()` does exactly this (verified in the installed source,
`OptaController.cpp` L325‑326):

```cpp
Wire.begin();
Wire.setClock(400000);   // 400 kHz Fast Mode — the Blueprint design point
```

- The expansion side answers in an **I²C onReceive ISR** (`OptaBlueModule.cpp`), so the controller can read the
  reply with near‑zero staging delay — but only because it owns the bus and runs at 400 kHz.
- The controller polls for an answer up to ~50 ms (`OPTA_CONTROLLER_WAIT_REQUEST_TIMEOUT`).
- The library implicitly assumes it is the **sole master** on that bus.

### 2.2 Why production does NOT use it (the deliberate trade‑off)
This trade‑off is documented in the prior I²C analyses and is **intentional**:

- `OptaController.begin()` forces `Wire.setClock(400000)`. **The Blues Notecard is unreliable at 400 kHz on this
  Mbed/Opta port** — debugging (`OPTA_I2C_COMMUNICATION.md`) found **100 kHz is the only reliable speed for the
  Notecard**. So adopting `OptaController` would clobber the bus to 400 kHz and break cellular.
- Therefore production **never calls `Wire.setClock(400000)`** and instead **re‑implements the Blueprint frames
  by hand** on raw `Wire` (`tankalarm_configureCurrentAdcChannel` / `tankalarm_readCurrentAdcFramed` /
  `tankalarm_setPwm` / `tankalarm_getAnalogChannelFunction` in
  [TankAlarm_I2C.h](../TankAlarm-112025-Common/src/TankAlarm_I2C.h)).
- Net effect: the A0602 is driven at the **default 100 kHz** (`Wire.begin()` with no `setClock`,
  [client .ino L1495‑1496](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L1495)),
  i.e. **¼ of its design speed**, so every A0602 transaction occupies the bus ~4× longer than the library intends.

> **This is the crux of the standard‑vs‑reality tension:** the Notecard wants 100 kHz, the A0602 (Blueprint)
> wants 400 kHz, and they are on the **same bus** — so one of them must run off‑spec. Production chose 100 kHz
> (favoring the Notecard) and hand‑rolled the A0602 to avoid the clock clobber. **There is no Arduino standard
> for co‑hosting a Notecard and a Blueprint expansion on one I²C bus** — the libraries each assume they own it.

---

## 3. Contention analysis — "too many messages at the same time?"

### 3.1 The precise answer
**No true simultaneity.** One master, one core servicing `Wire` from the main loop ⇒ I²C transactions are
strictly **serialized**. There is no bus collision or multi‑master arbitration failure.

**But the bus is over‑subscribed and un‑isolated.** The failure mode is *timing/ state coupling* between two
protocols sharing a slow bus, not concurrent access:

| Pressure | Detail |
|---|---|
| **Slow clock** | 100 kHz (not the A0602's 400 kHz) ⇒ each framed transaction holds the bus ~4× longer, widening every contention window. |
| **Blocking peer** | Notecard cellular ops (`hub.sync`, `note.add`, `note.changes` + N×`note.delete` in `trimTelemetryOutbox()`) can occupy the bus context for **100s of ms to seconds**, and `note‑arduino` reads in chunked segments with inter‑segment delays. |
| **No isolation** | The hand‑rolled Blueprint reads and the Notecard reads run back‑to‑back in the same loop pass with **no bus mutex, no `Wire.end()/begin()` boundary, and (pre‑v2.0.45) no RX drain**. A timed‑out / partial Notecard transaction can leave SDA held or stray bytes in the `Wire` RX buffer that **NACK or corrupt the next A0602 framed answer**. |
| **Tight timeout** | `I2C_WIRE_TIMEOUT_MS = 25` ([TankAlarm_Config.h]) is tighter than the Blueprint controller's own 50 ms and the bench utility's 100 ms — a slow A0602 staging under bus load can trip it. |
| **ISR assumption** | The Blueprint expansion stages its reply in an onReceive ISR expecting a clean framed request; a fragment from a prior Notecard transaction violates that assumption. |

### 3.2 Why it works on the bench but not in the field
The bench sketches (`P1_Transistor_Gating_Test`, `Blueprint_CH0_Test`) read **~4.5 mA / ~1.6 psi correctly** —
because they run the A0602 **standalone**: no Notecard, no Modbus, no `trimTelemetryOutbox()`, nothing else on
`Wire`. The production firmware chains the Notecard + Modbus + A0602 on one bus. **Same read code, different bus
environment** ⇒ the differentiator is the shared bus, not the read protocol. (This matches the field evidence:
the A0602 has never produced a valid reading in production, only `0`/`sensor‑fault` or a stuck floating value.)

### 3.3 What v2.0.45 already does about it (mitigation, not cure)
- `sampleMonitors()` **defers `trimTelemetryOutbox()`** until after sampling on current‑loop devices, and
  **drains the `Wire` RX buffer** before the A0602 reads — removes the heavy Notecard burst from the immediate
  pre‑read window.
- A0602 transactions use a **scoped 100 ms `Wire.setTimeout`**, restored to 25 ms before every return.
- **`GET_CHANNEL_FUNCTION` config‑verify** + **framed‑failure accounting** (`gCurrentLoopI2cErrors`) so a
  contention‑induced failure is no longer silent.

These reduce the contention window but **cannot make one slow shared bus behave like two**.

---

## 4. Recommendations (ranked)

> **⚠️ Correction (2026‑06‑24):** R‑1 below originally assumed the A0602 could be moved to a free second I²C
> (`Wire1`/`Wire2`/`Wire3`) in firmware. **Verified false on the Opta** — the board has only **one** external
> I²C (`Wire` = I2C3, PH_8/PH_7); the second instance (`Wire1` = I2C1, PB_6/PB_7) is the **internal
> secure‑element bus** (`CRYPTO_WIRE`), not pinned out. There is **no** software `Wire` reassignment available.
> See **`OPTA_I2C_BUS_SEPARATION_FEASIBILITY_06242026.md`** for the corrected, hardware‑grounded analysis.
>
> **⛔ Constraint (2026‑06‑24): NO hardware changes** — the existing Opta AUX connector, Blues "Wireless for
> Opta" carrier, and Opta Ext A0602 must be used **as wired**. This **excludes R‑1 and R‑3 entirely** (and the
> Notecard→UART / external‑mux ideas), because all of them require hardware/rewiring. The **only** applicable
> recommendations are the firmware‑only ones: **R‑2** (harden single‑bus isolation: ≥50 ms A0602 timeout,
> single‑owner ordering, bench‑gated per‑op 400 kHz window, bus recovery) and **R‑4** (quantify in telemetry).
> R‑1/R‑3 are retained below only as rejected alternatives.

### R‑1 — Separate the buses (the real fix)
Give the Blueprint expansion its own I²C bus so each device runs at its design speed and each library gets the
sole‑master ownership it assumes:
- Put the **A0602 on a second I²C peripheral** (`Wire1`/`Wire2`/`Wire3` on the STM32H747) at **400 kHz**, and
  keep the **Notecard on `Wire` @ 100 kHz**. Then the official `OptaController` can be adopted for the A0602
  (R‑3) with no clock conflict.
- **Caveat — hardware:** the Blues carrier daisy‑chains the Notecard onto the **AUX connector's** I²C, and the
  A0602 snaps onto that same AUX connector, so they may be physically tied together. Check whether the Opta
  exposes a **second, independent I²C** (e.g. a Qwiic/other header mapped to a different STM32 I2C peripheral)
  that the A0602 could move to. This likely needs a **wiring change**, but it is the only structurally clean
  solution. **Verify the Opta + Blues + A0602 pinout before committing.**

### R‑2 — If the single bus is unavoidable, harden the isolation
- Keep the v2.0.45 deferred‑trim + RX‑drain.
- **Widen the A0602 timeout to ≥ 50 ms** around its transactions (match the Blueprint controller; the bench used
  100 ms). 25 ms is too tight under bus load.
- Add an explicit **software bus guard/mutex** around A0602 vs Notecard transactions; if logs show wedging, a
  guarded `Wire.end()/Wire.begin()` boundary between the Notecard and A0602 domains (bench‑validate first — it
  can disturb the Notecard).
- **Bench‑test a per‑op clock bump:** raise to **400 kHz only around the A0602 transactions** and restore
  100 kHz before any Notecard op. This shrinks the A0602 bus window 4× **without** running the Notecard at
  400 kHz. **Must be bench‑proven on the field harness** — prior debugging removed global 400 kHz for a reason.

### R‑3 — Adopt `OptaController` for the A0602 — but only after R‑1
The official library is the Arduino standard and is ISR‑robust, but it forces 400 kHz, so it is **only safe once
the A0602 is on its own bus**. Do not adopt it on the shared bus.

### R‑4 — Quantify the contention in the field (cheap, do now)
- The v2.0.45 `i2c_cl_err` / framed‑failure counter and the solar `merr` taxonomy already surface failures;
  add a **bus‑health metric** (e.g., A0602 NACK count vs successful reads per cycle, and whether failures
  cluster right after Notecard sync) to telemetry so contention can be measured remotely rather than inferred.

---

## 5. Honest conclusion

- **Arduino standard:** `Arduino_Opta_Blueprint` (Wire @ 400 kHz, ISR, sole master). Production correctly avoids
  it on the shared bus because it would force 400 kHz and break the Notecard — there is **no standard pattern**
  for the Notecard‑plus‑expansion‑on‑one‑bus topology this product uses.
- **Contention:** not simultaneous access (single master serializes everything), but a **slow, shared,
  un‑arbitrated bus** where a blocking cellular modem and a timing‑sensitive analog expansion interfere. That is
  consistent with "works standalone, fails in production."
- **Path forward:** the durable fix is **physical bus separation (R‑1)**; absent that, the v2.0.45 mitigations
  plus a wider A0602 timeout and a bench‑validated per‑op clock bump (R‑2) are the best software can do. The read
  protocol itself is sound.

---

## References
- `CODE REVIEW/OPTA_I2C_COMMUNICATION.md` — original Opta I²C reference (100 kHz‑for‑Notecard decision, debugging log).
- `CODE REVIEW/I2C_COMPREHENSIVE_ANALYSIS_02262026.md` — raw‑I²C‑vs‑OptaBlue, 400 kHz clobber risk.
- `CODE REVIEW/REMAINING_PROBLEMS_AND_ATTEMPTS_06232026.md` — A0602 framed‑protocol analysis, v2.0.45 mitigations, bench‑vs‑field.
- Installed library: `Arduino_Opta_Blueprint/src/OptaController.cpp` (L325‑326 `Wire.begin()` + `setClock(400000)`), `OptaBlueModule.cpp` (onReceive ISR), `AnalogExpansion.cpp`.
- `note‑arduino` — `Notecard::begin()` → `make_note_i2c(Wire)` (Notecard on `Wire`).
- Code: [TankAlarm_I2C.h](../TankAlarm-112025-Common/src/TankAlarm_I2C.h) (A0602 raw‑Wire frames), [client .ino ~L1495/L5409](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L1495) (`Wire.begin()`, no `setClock`; scoped 100 ms timeout).
