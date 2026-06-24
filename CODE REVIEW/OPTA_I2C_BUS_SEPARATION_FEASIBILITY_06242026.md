# Opta I²C Bus‑Separation Feasibility & Action Plan (A0602 vs Notecard)

**Date:** 2026‑06‑24
**Author:** GitHub Copilot (AI review)
**Status:** Design note / decision input — **no firmware changed**
**Hardware:** Arduino Opta **Lite** (STM32H747) + Blues "Wireless for Opta" Notecard + Arduino Pro Opta Ext **A0602**
**Hard constraint:** **NO hardware changes.** The solution must run on the **existing Opta AUX connector, the
Blues "Wireless for Opta" carrier, and the Opta Ext A0602 as wired today** — no rewiring, no added parts, no
carrier swap. This puts every form of physical bus separation off‑limits; the path is **firmware‑only** (§2–§5).
**Follows / corrects:** `OPTA_AUX_I2C_CONTENTION_REVIEW_06232026.md` (its recommendation **R‑1** assumed a free
second I²C bus — that assumption is **wrong on the Opta**; this note documents why and what is actually feasible).

---

## 0. Why this document exists (correction to the prior review)

The AUX contention review concluded — correctly — that the A0602 field failures are a **shared‑bus
architecture** problem and that the durable fix is **physical bus separation (R‑1)**, suggesting the A0602 be
moved to "`Wire1`/`Wire2`/`Wire3` at 400 kHz."

**That option does not exist on the Opta.** I verified the core's board variant. The Opta defines exactly **two**
I²C instances, and the second is **not** a free expansion bus — it is the **internal secure‑element bus**:

```c
// variants/OPTA/pins_arduino.h
#define WIRE_HOWMANY        2

#define I2C_SDA             (digitalPinToPinName(PIN_WIRE_SDA))   // D11 -> PH_8  = I2C3_SDA
#define I2C_SCL             (digitalPinToPinName(PIN_WIRE_SCL))   // D12 -> PH_7  = I2C3_SCL

#define I2C_SDA_INTERNAL    (PB_7)                                // = I2C1_SDA
#define I2C_SCL_INTERNAL    (PB_6)                                // = I2C1_SCL
#define I2C_SDA1            I2C_SDA_INTERNAL
#define I2C_SCL1            I2C_SCL_INTERNAL
#define CRYPTO_WIRE         Wire1                                 // <-- Wire1 is the on‑board crypto bus
```

So the corrected, verified topology is:

| Arduino object | STM32 peripheral | Pins | Where it goes | Free for A0602? |
|---|---|---|---|---|
| **`Wire`** | **I2C3** | PH_8 / PH_7 | **External AUX/expansion** — Notecard **0x17**, A0602 **0x64**, `OptaController` all live here | already the shared bus |
| **`Wire1`** | **I2C1** | PB_7 / PB_6 (`INTERNAL`) | **On‑board secure element** (`CRYPTO_WIRE`), not pinned out | **No** — internal + occupied |
| `Wire2`/`Wire3` | I2C2 / I2C4 | (not instantiated) | silicon exists, **pins not broken out** on Opta Lite | **No** |

**Conclusion up front:** a software‑only "move the A0602 to another `Wire`" is **not possible** on the Opta —
there is one external I²C bus, full stop. And because **no hardware changes are allowed**, the external‑hardware
separation paths (Notecard‑to‑UART, I²C mux/buffer) are **also off the table**. The **only** available path is
**disciplined single‑bus sharing in firmware** — extending what v2.0.45 already started (§3).

---

## 1. The hardware reality: one usable I²C bus

- The A0602 is **physically wired** to the AUX expansion connector, which is the Opta's `Wire` (I2C3, PH_8/PH_7).
  You cannot reassign it in firmware — its SDA/SCL are soldered to those pins through the expansion connector.
- The Blues "Wireless for Opta" carrier routes the **Notecard onto that same `Wire`** (per the Blues datasheet:
  "the Opta talks to the Wireless expansion/Notecard over I²C via the AUX expansion path"). Confirmed in code:
  `notecard.begin(NOTECARD_I2C_ADDRESS)` → `make_note_i2c(Wire)`.
- `Wire1` (I2C1, PB_6/PB_7) is the **secure‑element** bus. It is internal, not exposed on any user connector, and
  already has a device on it. It is **not** an option for the A0602.
- The remaining STM32H747 I²C controllers (I2C2, I2C4) are **not** instantiated by the Opta core and their pins
  are **not** broken out on the Lite. Reaching them would require board‑level rework that the Opta does not
  support as a product.

> Net: **the Opta gives you one usable I²C bus, and it is fully occupied** by the Notecard + A0602. Any physical
> "separation" would need external hardware (a second host interface or a mux) — which the no‑hardware‑change
> constraint forbids. So the bus stays shared and the fix must be **firmware discipline** on it.

---

## 2. The no‑hardware‑change requirement rules out physical separation

The system must use the **existing AUX connector, Blues "Wireless for Opta" carrier, and Opta Ext A0602 as wired
today**. That requirement eliminates **every** form of physical bus separation, because each one needs hardware:

| Separation approach | What it would require | Allowed under "no HW change"? |
|---|---|---|
| Move A0602 to a 2nd I²C (`Wire1`/`Wire2`/`Wire3`) | a second exposed I²C — **does not exist** (only the internal crypto bus, §0/§1) | **No** (and impossible anyway) |
| Move the **Notecard to a UART**, dedicate `Wire` to the A0602 @ 400 kHz | a UART‑capable carrier and/or rewiring the Notecard host interface | **No** |
| External **I²C mux / buffer / isolator** (TCA9548A, PCA9508, LTC4311) | added parts spliced into the AUX I²C | **No** |

These were the "true separation" options in earlier drafts; they are kept here only to record that they were
considered and are **explicitly excluded by the constraint**. The A0602 and the Notecard **will** continue to
share the one `Wire` (I2C3) bus.

> **So the entire solution space is firmware discipline on the shared bus.** The encouraging part: the field
> evidence (works standalone, fails in production — single master, so no *true* simultaneity) points at **temporal
> coupling**, which is exactly what firmware *can* attack. There is real headroom without touching hardware.

---

## 3. The only available path: harden the shared bus in firmware

This extends what v2.0.45 already shipped. Every item is firmware‑only and runs on the installed hardware as‑is.
**Shipped** = already in v2.0.45; **Add** = proposed next; **Bench‑gate** = must be validated on the full
Notecard + Modbus + A0602 harness before field rollout, because the Notecard is clock‑sensitive.

### 3.1 Transaction isolation / single‑owner discipline
- **(Shipped)** Defer `trimTelemetryOutbox()` until **after** sampling, and **drain the `Wire` RX buffer** right
  before the A0602 reads, so the heavy Notecard burst never sits immediately in front of an A0602 frame.
- **(Add)** Treat each A0602 read as an **atomic, owned section**: issue **no** Notecard request between the
  A0602 config‑write and its framed read. Sequence the loop so all Notecard I/O for the cycle completes first,
  then a short settle + WDT kick, then the A0602 burst.
- **(Add)** A small **inter‑device settle** (a few ms + WDT kick) when switching from a Notecard op to an A0602
  op, so a just‑finished Notecard transaction has fully released SDA/SCL before the A0602 frame starts.

### 3.2 Timeout
- **(Add)** Widen the A0602 `Wire` timeout to **≥ 50 ms** around its transactions (match the Blueprint
  controller's own 50 ms; the bench utility used 100 ms). Today's `I2C_WIRE_TIMEOUT_MS = 25` is tighter than the
  controller it imitates and can trip when A0602 staging slips under bus load. Keep the normal 25 ms for Notecard
  ops (v2.0.45 already scopes the wider timeout around A0602 reads and restores it after).

### 3.3 Per‑op clock management (the highest‑leverage software lever)
- **(Add, Bench‑gate)** Raise `Wire` to **400 kHz only around the A0602 frames**, and restore **100 kHz before
  any Notecard op**. This 4× shrinks each A0602 bus window (back toward the speed the Blueprint expansion was
  designed for) **without** running the Notecard at 400 kHz — the exact reason global 400 kHz was removed.
  Implement as `Wire.setClock(400000)` → A0602 transaction → `Wire.setClock(100000)`, with the 100 kHz restore on
  a path that **always** runs (mirror the existing scoped‑timeout restore so an early return can't leave the bus
  at 400 kHz for the Notecard).
- **Why it matters most here:** with no hardware option, the bus‑window width is the main knob left; quartering
  the A0602's bus occupancy directly reduces the temporal overlap that corrupts its reads.

### 3.4 Bus recovery when a transaction wedges
- **(Add)** If the A0602 NACKs or SDA is held low after a partial Notecard transaction, run a guarded **I²C bus
  recovery**: clock out a stuck slave (toggle SCL up to 9 cycles), then `Wire.end()` / `Wire.begin()` and
  re‑`notecard.begin()`. The codebase already has `recoverI2CBus()` + a Notecard‑recovery threshold
  (`CODE_REVIEW_02262026.md`); extend the same recovery to the A0602 framed‑read failure path. Bench‑validate the
  recovery does not itself perturb the Notecard session.

### 3.5 Retry + accounting (observability)
- **(Shipped)** A0602 framed‑read failures bump `gCurrentLoopI2cErrors`; P1 OFF retried ×3; `scImpl`/`merr`
  taxonomy emitted.
- **(Add)** Bounded retry with a short backoff on the A0602 framed read itself (re‑drain RX between attempts), and
  publish a per‑cycle **A0602 success/NACK count** to telemetry so the field can show whether the discipline is
  working and whether failures still cluster right after a Notecard sync.

---

## 4. Measure to guide the tuning (free, do this first)

We are not building hardware, so the bench work exists only to **tell us which firmware knobs help** and to prove
the per‑op 400 kHz window is safe for the Notecard:
1. **Field telemetry (already available):** watch the v2.0.45 `gCurrentLoopI2cErrors` / `scImpl` / `merr` signals
   and whether A0602 failures **cluster immediately after a Notecard `hub.sync`/trim** → confirms temporal
   coupling and tells us the isolation/ordering changes (§3.1) are the right lever.
2. **Bench harness (Notecard + Modbus + A0602 together):** A/B test each §3 change. Most important: prove the
   **per‑op 400 kHz window (§3.3) restores reliable A0602 reads while the Notecard session stays healthy** (no
   sync failures). This is the gate that lets the clock change ship.
3. **Optional scope on SDA/SCL:** confirm reads fail because they are *not serviced in time* (temporal), not
   because the line is electrically marginal — the latter cannot be fixed without hardware anyway, so this mainly
   tells us how much headroom the clock/timeout changes have.

---

## 5. Recommendation / action plan (all firmware‑only)

1. **Keep** the v2.0.45 isolation already in place (deferred trim + RX drain).
2. **Add now (low risk):** widen the A0602 `Wire` timeout to **≥ 50 ms** (§3.2); make each A0602 read an owned
   section with Notecard I/O sequenced first + an inter‑device settle (§3.1); add bounded A0602 retry + a
   per‑cycle success/NACK counter to telemetry (§3.5).
3. **Add, bench‑gated:** the **per‑op 400 kHz window** around A0602 frames with a guaranteed 100 kHz restore
   (§3.3), and **A0602 bus recovery** on a wedged transaction (§3.4). Ship only after §4 proves the Notecard
   session is unaffected.
4. **Do not** attempt any `Wire` reassignment, UART move, or external mux — the first is impossible and the rest
   need hardware the constraint forbids. The shared bus stays; firmware discipline is the fix.

> **Honest expectation:** this will **materially improve** A0602 reliability by shrinking and isolating its bus
> window, and at minimum makes every remaining failure **observable** in telemetry. It cannot give the A0602 a
> truly private bus — that needs hardware — so if a residual failure rate persists after §3/§5 are bench‑proven
> and deployed, the conclusion is that the shared‑bus ceiling has been reached and any further gain would require
> the hardware change that is currently out of scope.

---

## 6. Verified facts (sources)

- **Opta has 2 I²C instances; the 2nd is the crypto bus.** `variants/OPTA/pins_arduino.h`: `WIRE_HOWMANY 2`,
  `Wire` = PH_8/PH_7 (I2C3, external), `Wire1` = PB_7/PB_6 (I2C1, `CRYPTO_WIRE`, internal). `Wire2/Wire3` not
  defined.
- **Everything shares `Wire` (I2C3).** `OptaController::begin()` → `Wire.begin(); Wire.setClock(400000)`
  (`Arduino_Opta_Blueprint/src/OptaController.cpp` L325‑326). `notecard.begin(NOTECARD_I2C_ADDRESS)` →
  `make_note_i2c(Wire)`. A0602 (0x64) raw frames on `Wire` (`TankAlarm_I2C.h`).
- **Production runs `Wire` at 100 kHz** (no `setClock`) deliberately, because the Notecard is unreliable at
  400 kHz on this Mbed/Opta port (`OPTA_I2C_COMMUNICATION.md`, `I2C_COMPREHENSIVE_ANALYSIS_02262026.md`).
- **The "Wireless for Opta" carrier connects the Notecard over the I²C/AUX path**
  (`CODE_REVIEW_06152026_OTA_VS_USB_UPDATE_FAILURE.md` L272) — i.e. moving the Notecard off I²C would require a
  hardware/carrier change, which is out of scope under the constraint.
- **A0602 = 0x64, Notecard = 0x17** (`client .ino` L245, `TankAlarm_Common.h`).

---

*No firmware was modified by this review. It is a feasibility/decision note. Given the **no‑hardware‑change**
constraint — the existing Opta AUX connector, Blues "Wireless for Opta" carrier, and Opta Ext A0602 must be used
as wired — the **only** available path is firmware discipline on the shared `Wire` bus (§3/§5). It also corrects
the "move to another `Wire`" assumption in `OPTA_AUX_I2C_CONTENTION_REVIEW_06232026.md`.*

---

## 7. AI Reviewer Suggestions & Outside-the-Box Thinking

### 7.1 Default Methods for Opta AUX Port
The standard method for utilizing the Opta's AUX expansion port is via the `OptaController` class in the Arduino Pro tier. By default, `OptaController::begin()` expects to own the I²C bus, forcefully initializing it to 400 kHz. When mixing `OptaController` (or A0602 direct polling) with other heavy I²C peripherals (like the Blues Notecard), the default abstraction breaks down because it assumes exclusive temporal bus access. Opta expansions were ostensibly designed under the assumption they would be the primary device on `Wire`.

### 7.2 Known Forum & Community Issues
Researching similar issues across Mbed / STM32H7 / Arduino forums highlights common architectural friction points:
- **Mbed OS I²C Mutex Limitations:** Mbed's underlying I²C drivers often lack robust concurrent mutexing for multi-threaded setups, meaning interlaced commands easily crash the state machine.
- **STM32H7 Clock Stretching Errata:** STM32 I²C silicon has known errata with heavy clock stretchers. The Blues Notecard enforces significant clock stretching. When an Opta expansion module (which does not stretch clocks as heavily) shares the bus, the H7 peripheral can occasionally miss a stop condition or lock the bus state.
- **Blues Notecard I²C Vulnerabilities:** The community frequently reports Notecard I²C instability on shared Mbed buses, which is why Blues heavily recommends UART operation for high-reliability applications.

### 7.3 What We've Tried That Failed
- **Moving the A0602 to a secondary I²C bus:** As thoroughly debunked in §0, `Wire1` is the internal crypto bus and `Wire2/Wire3` are not exposed.
- **Global 400 kHz I²C clock:** This was previously attempted but rolled back because the Blues Notecard drops frames and fails to sync reliably at 400 kHz on the Mbed/Opta port.
- **Silent Failures / No Retry:** Historically, I²C reads were dropping silently without retries or backoff, leaving the underlying cause obscured and leading to phantom stale data.

### 7.4 Outside-the-Box Ideas & Their Pitfalls

#### Idea A: Bit-Bang Software I²C (`SoftWire`)
- **Concept:** Temporarily commandeer two other GPIO pins on the Opta to run a bit-banged I²C master specifically for the A0602, entirely severing it from the Notecard's bus.
- **Pitfalls:** Violates the "no hardware change" constraint because the A0602 is physically headered to the AUX connector's hardware SDA/SCL pins. You would need to physically reroute the A0602 pins to free GPIOs. Additionally, bit-banging blocks interrupts and loop execution, further skewing RTOS timing.

#### Idea B: Subvert the RS485 Port for Analog Input
- **Concept:** Ditch the A0602 entirely and use an off-the-shelf Modbus RTU 4-20mA expansion module connected via the Opta's built-in RS485 block. 
- **Pitfalls:** Requires new hardware procurement, firmware rewriting to add Modbus RTU telemetry reading, and enclosure physical changes. Excluded under "no hardware changes".

#### Idea C: Asynchronous DMA I²C Transfers
- **Concept:** Bypass standard `Wire` blocking logic and use STM32H7 DMA commands to schedule A0602 and Notecard transactions independently, preventing blocking-wait timeouts.
- **Pitfalls:** Highly complex Mbed/STM32 HAL driver surgery. Maintaining synchronization between HAL DMA triggers and the Blues/Arduino high-level libraries introduces heavy technical debt and likely exacerbates state de-syncs without fixing the root bus contention.

#### Idea D: Force Notecard Sleep States Between Reads
- **Concept:** Issue an AT-style command to put the Notecard into deep sleep, service the A0602 exclusively, and wake the Notecard back up.
- **Pitfalls:** Waking the Notecard imposes significant latency and cellular network penalties. Synchronizing recurrent cellular network registration logs with a fast control loop reading analog inputs would destroy the PLC's timing determinism.

### 7.5 Final AI Thoughts
The temporal isolation + scoped 400 kHz strategy (Section 3) represents the absolute ceiling of what is possible in software without hardware modifications. If this mitigates 90% of faults but leaves a hanging 10% failure rate, the organization *must* reconsider the "no hardware changes" constraint. Moving the Blues Carrier to UART—which is natively supported by many carrier revisions—is the only enduring, architecturally sound solution to I²C contention on the Opta platform.

---

## 8. Second Reviewer's Assessment & Recommended Subset (GitHub Copilot, 2026‑06‑24)

I reviewed §3, §5 and §7 against the code, the installed core/libraries, the existing repo I²C analyses
(`FUTURE_3.3.3_WIRE_LIBRARY_TIMEOUT.md`, `FUTURE_3.2.1_I2C_TRANSACTION_TIMING.md`,
`CODE_REVIEW_06222026_DEFERRED_ITEMS.md`, `I2C_REVIEW_SUMMARY_02262026.md`) and the Blues **Wireless for Opta**
datasheet. The architectural framing is sound, but the research surfaced **two corrections that change the
priority order** and a **mechanism the plan under‑weights**. My net opinion: the biggest win here is *not* the
400 kHz clock trick — it is **transaction isolation plus guaranteeing the A0602's P1 power‑gate actually turns
on**, and the A0602 timeout should be made **shorter, not longer**.

### 8.1 Two corrections to this document's own proposals

- **§3.2 is backwards — shorten the A0602 timeout, don't widen it.** The repo's `FUTURE_3.3.3` (validated again
  here) shows the **A0602 does not clock‑stretch**; only the **Notecard** stretches SCL (≈5–15 ms typical, up to
  ~25 ms during sync, 100 ms+ during a Notecard FW update). The Wire timeout exists to ride out *clock
  stretching*, so the long timeout belongs to the **Notecard**, not the A0602. A long A0602 timeout merely
  **waits out** contention — adding loop latency and watchdog pressure while masking the fault. Better: give the
  A0602 a **short/moderate** Wire timeout (~10–25 ms) so a held bus **fails fast into the §3.4 recovery**, and
  reserve ≥50 ms for Notecard ops. v2.0.45's scoped **100 ms** for A0602 is conservative and should be revisited
  *downward* once §3.1 isolation is tight. (Note: this is the Wire *byte* timeout, which is distinct from the
  Blueprint request→answer **staging delay**; the staging delay stays as‑is.)
- **§3.3's clock *direction* is an unproven hypothesis, and there is a credible opposite.**
  `CODE_REVIEW_06222026_DEFERRED_ITEMS.md` (item 11‑C) proposes **lowering** the bus to **50 kHz** on a marginal
  A0602 bus — the reverse of the 400 kHz idea. Raising to 400 kHz shrinks the *temporal* window but **degrades
  signal integrity** (faster edges, less tolerance for cable capacitance / weak pull‑ups); on an electrically
  marginal field bus it could make things **worse**. These two levers fix *different* root causes, so the §4
  bench/scope test must pick the direction **before** either ships. Do not ship a clock change blind.

### 8.2 The residual gap worth closing (corrected after a code read)

The field signature is a **stuck/garbage value that has never been a valid reading** (the ~18–20 mA / `0xFFFF`
full‑scale signature), while the **same code reads ~4.5 mA correctly on the bench**. A purely *intermittent* bus
contention would produce intermittent *successes* — we see none.

**Correction — the obvious guard already exists.** I initially flagged "publishes a floating value when the P1
enable NACKs," but a read of `readCurrentLoopSensor()` (client `.ino` ~L5364) shows v2.0.45 (since v1.9.22)
**already fails safe**: if `tankalarm_setPwm()` ON NACKs all 3 attempts it logs, drives P1 off, and returns
`NAN` (it does **not** read); if every framed sample fails (`validSamples==0`) it returns `NAN`; and
`tankalarm_readCurrentAdcFramed()` rejects CRC/channel failures (negative return). So a *missing NACK guard* is
**not** the problem.

**The real residual: `setPwm` ACK ≠ rail‑up.** The code itself notes the I²C ACK only confirms the command was
*received*, not that the loop powered. A loop that ACKs the enable but doesn't physically source current can
still yield an **in‑range, CRC‑valid** value — most dangerously an all‑ones/`0xFFFF` read that scales to ~20 mA
and passes validation. The firmware levers (all detailed in §9): (a) **positively reject `0xFFFF`/CRC‑fail/
stuck‑identical samples** so a garbage read can never masquerade as ~20 mA; (b) cross‑check the
`GET_CHANNEL_FUNCTION` ext‑power state; (c) keep the enable inside the §3.1 clean window so it is far likelier to
genuinely take. A value that is *simultaneously* in‑range and CRC‑valid yet physically wrong is a **hardware**
condition (loop unpowered / transmitter disconnected) that no firmware can detect by value alone — that is
§8.5(3) / Phase 3.

### 8.3 My favorite proposals, in priority order

1. **§3.1 single‑owner transaction ordering + RX drain (mostly shipped — tighten it).** Highest value / lowest
   risk. Crucial insight the doc should state plainly: **the Notecard is an I²C *slave*** (addr 0x17) and **never
   masters the bus** — the Opta host is the sole master, so the host *already* has total control over when
   Notecard traffic happens. Disciplined ordering (all Notecard I/O first → drain RX → settle → A0602 burst) buys
   the A0602 a **guaranteed‑idle window essentially for free**. This is the backbone fix.
2. **Close the "in‑range garbage" gap (§8.2).** Reject `0xFFFF`/CRC‑fail/stuck‑identical samples and cross‑check
   the ext‑power channel function, so a powered‑but‑not‑sourcing loop can't masquerade as a ~20 mA reading. (The
   P1‑NACK fail‑safe already exists — this closes the residual.)
3. **§3.4 bus recovery + §3.5 observability counters.** Cheap, safe, and they convert an invisible failure into a
   *measured* one (per‑cycle A0602 success/NACK, and whether failures cluster right after a Notecard sync). Even
   if nothing else changes, ship these — they tell us if the rest is working.
4. **§3.3 per‑op clock window — only after §4 decides the direction.** Promising but conditional (see §8.1).

### 8.4 Pitfalls in the §7 ideas (assessment)

- **§7.2 needs three fixes.** (a) The *"Mbed multi‑threaded mutex"* worry is a **red herring** — this firmware is
  single‑threaded with one bus master; the coupling is **sequential residue**, not concurrent access. (b) The
  *"STM32H7 clock‑stretching errata"* is **uncited** and shouldn't be leaned on; the **documented, real** factor
  is the **Notecard's** clock stretching, and the mitigation (adequate Notecard timeout + bus recovery) is the
  same regardless of any erratum. (c) *"Blues heavily recommends UART for reliability"* is **not supported** — the
  Wireless‑for‑Opta datasheet states the Opta↔Notecard link is **over I²C**, and this project's own field
  experience is that **I²C @ 100 kHz is reliable** for the Notecard. Don't cite it as a reason.
- **Idea A (bit‑bang `SoftWire`)** — correctly rejected. Reinforce: the A0602's SDA/SCL are **headered to the AUX
  hardware I²C pins**, so you can't move it to GPIO without rewiring (HW change), and Mbed/RTOS preemption makes
  bit‑bang timing unreliable anyway.
- **Idea B (RS‑485 Modbus 4–20 mA module)** — correctly rejected, with an extra nail: the Opta's RS‑485
  (`Serial2`) is **already in use** for the SunSaver MPPT link, so the port isn't even free.
- **Idea C (DMA I²C)** — correctly rejected. Key point: DMA only offloads **byte copying**; it does **not** add a
  second master or remove **serialization**, so it cannot fix contention, while adding large Mbed/HAL complexity.
- **Idea D (sleep the Notecard between reads)** — reject on a *cleaner* basis than §7.4 gives: since the Notecard
  is a **slave that never initiates** bus traffic, the host obtains exclusive A0602 windows simply by **not
  issuing Notecard ops** during them (i.e. §3.1). Sleeping/waking the Notecard yields **zero** bus benefit and
  costs cellular re‑registration latency and data. It's not just risky — it's **unnecessary**.
- **§7.5 (UART as "the only enduring solution," "natively supported by many carrier revisions")** — partly agree,
  with caveats. UART *is* the architecturally clean long‑term answer **if** the no‑HW constraint is ever lifted,
  **but**: the datasheet specifies **I²C** for this module's Notecard link, and there is **no evidence** the
  Notecard's host UART is routed through to the Opta on the Wireless‑for‑Opta carrier. So even a future move would
  need schematic verification **and** a wiring/carrier change — it is not a drop‑in, and it remains out of scope
  here.

### 8.5 What I would ship, in order

1. **Now (no bench needed):** tighten §3.1 ordering; **close the in‑range‑garbage gap** (§8.2); add the §3.5
   success/NACK counters and §3.4 A0602 recovery; **rebalance the timeouts** — A0602 scope **down** to ~25 ms,
   Notecard global **up** to 50 ms (§8.1).
2. **Bench‑gated next:** run the §4 scope/AB test to decide the clock **direction** (50 kHz signal‑integrity vs
   per‑op 400 kHz window) and ship **whichever the data supports — one of them, not both, not blind.**
3. **In parallel (not firmware):** have the field tech confirm **loop supply, pull‑ups, and P1 wiring** on the
   actual A0602, because the "works on the bench, never in the field" signature is at least as consistent with a
   persistent physical/config delta as with bus contention. Firmware discipline raises the ceiling; it may not, by
   itself, be the whole fix.

> **One‑line verdict:** keep the §3 framework, but lead with **isolation + hardened read/recovery**, **rebalance
> the timeouts** (A0602 shorter, Notecard longer), and treat the **clock‑speed change as a measured, directional
> decision** rather than a foregone 400 kHz — and don't bank on the §7 "outside‑the‑box" ideas, which are
> correctly but sometimes imprecisely ruled out above.

---

## 9. Comprehensive Implementation Plan & Todo List

### 9.0 Ground rules
- **Firmware‑only**, on the hardware exactly as wired (per the §2 constraint). Client‑side only — no server change.
- **Phased.** Phase 1 = no bench required, ships as one client release. Phase 2 = the **single** clock change,
  **bench‑gated**. Phase 3 = physical checks (not firmware), run in parallel.
- **Additive & reversible.** Client OTA goes through the MCUboot trial‑boot pipeline (auto‑rollback on a bad
  boot), so a regression cannot brick the field unit.
- **One logical change per commit**; bump `FIRMWARE_VERSION`/`FIRMWARE_BUILD_SEQ` + `library.properties`; add
  `release-notes/vX.Y.Z.md`; compile **client (`security=sien`) and server** clean; **tag `vX.Y.Z`** to trigger
  the CI signed‑slot build + magic‑byte guard. Target Phase 1 = **v2.0.46**.

### 9.1 Current‑state audit — already shipped in v2.0.45 (do NOT redo)
A read of `readCurrentLoopSensor()` (client `.ino` ~L5345) and `sampleMonitors()` confirms much of §3 already
exists. Phase 1 is **refinement**, not new infrastructure:

| Already present | Where | Effect |
|---|---|---|
| P1‑enable NACK → `NAN` (no floating read) | ~L5364–5402 | `setPwm` ON fails 3× → log, P1 off, return fault |
| Total read failure → `NAN` (`validSamples==0`) | ~L5544 | every framed sample failed → fault + `gCurrentLoopI2cErrors++` |
| Framed read rejects CRC/channel failures | `tankalarm_readCurrentAdcFramed()` (<0 return) | garbage frames not averaged |
| `GET_CHANNEL_FUNCTION` (0x40) ext‑power verify | ~L5419–5460 | rejects a positively‑wrong channel function |
| Scoped A0602 Wire timeout 100 ms, restored to 25 ms | L5409 / L5446 / L5542 | A0602 ops off the Notecard default |
| Defer `trimTelemetryOutbox()` + drain Wire RX pre‑A0602 | `sampleMonitors()` | removes the Notecard burst from the pre‑read window |
| Error counter in telemetry | `doc["i2c_errs"]` (L5056), `gCurrentLoopI2cErrors` (L901) | A0602 failures already visible |

### 9.2 Phase 1 — firmware, no bench (target v2.0.46)

**T1 — Rebalance the per‑device Wire timeouts (the §8.1 correction).**
- *Change:* lower the scoped A0602 timeout from `Wire.setTimeout(100)` (L5409) to a named
  `A0602_WIRE_TIMEOUT_MS` (default **25 ms**) — the A0602 does not clock‑stretch, so fail fast into recovery (T3).
  Separately raise the Notecard/global `I2C_WIRE_TIMEOUT_MS` (`TankAlarm_Config.h` L146) from **25 → 50 ms** so
  legitimate Notecard clock stretches (≈5–15 ms, up to ~25 ms on sync) have margin (`FUTURE_3.3.3`). Keep the
  three existing restore points pointing at `I2C_WIRE_TIMEOUT_MS`.
- *Accept:* an A0602 read on a wedged bus fails within ~25 ms and triggers T3; Notecard sync no longer trips a
  spurious 25 ms timeout.
- *Risk:* Low — the Notecard 50 ms is the documented "conservative, no‑risk" value; the A0602 reduction only
  changes *failure latency*, not success behaviour, once T2 guarantees a clean bus.

**T2 — Lock in single‑owner transaction isolation (§3.1).**
- *Change:* ensure `sampleMonitors()` runs, in order: finish all Notecard I/O → **drain Wire RX**
  (`while (Wire.available()) Wire.read();`) → small **inter‑device settle** (`delay(2–5)` + WDT kick) → A0602
  burst, with the deferred `trimTelemetryOutbox()` **after** sampling. Assert (comment + code) that **no Notecard
  request is issued between `tankalarm_configureCurrentAdcChannel()` and the framed reads** (already true — lock
  it in so a future edit can't regress it).
- *Accept:* no `notecard.*` call interleaves an A0602 transaction; a fresh RX drain precedes the first frame.
- *Risk:* Very low (additive ordering).

**T3 — A0602 bus recovery on framed‑read failure (§3.4).**
- *Change:* in the `validSamples==0` branch (~L5544) and the config‑NACK branch (~L5440), after counting the
  error, call the existing `recoverI2CBus()` wrapper (→ `tankalarm_recoverI2CBus(gDfuInProgress, kick)`: deinit
  Wire, 16 SCL clocks, STOP, re‑begin) then `tankalarm_ensureNotecardBinding(notecard)`. Gate with **bounded
  backoff** (mirror the existing sensor‑only exponential backoff 1×→2×→4×… capped) so a hard‑down A0602 doesn't
  recover every cycle. Respect the existing `gDfuInProgress` guard.
- *Accept:* a wedged A0602 transaction is followed by one recovery (subject to backoff); the Notecard re‑binds
  cleanly; no watchdog reset.
- *Risk:* Medium — recovery toggles the shared bus and must re‑bind the Notecard; bench‑sanity even though it's
  "Phase 1." The mechanism is proven on the server path (`server .ino` L4480).

**T4 — Strengthen observability (§3.5).**
- *Change:* add `gCurrentLoopReadsOk` and a `lastClFaultReason` enum (none / pwm‑nack / config‑nack / func‑wrong
  / read‑fail / recovered) beside `gCurrentLoopI2cErrors`; emit `doc["cl_ok"]` and `doc["cl_fault"]` next to the
  existing `doc["i2c_errs"]`. (Optional) publish a lightweight recovery diag note — but the server has **no diag
  inbox** today, so keep it telemetry‑embedded unless a route is added.
- *Accept:* telemetry shows ok‑vs‑fail counts and the dominant fault reason per device, and whether failures
  cluster after a Notecard sync.
- *Risk:* Low (additive fields) — watch the JSON size budget.

**T5 — Close the "in‑range garbage" gap (§8.2 residual).**
- *Change:* in `tankalarm_readCurrentAdcFramed()` (or its caller), explicitly **reject an all‑ones/`0xFFFF` raw
  sample** and any CRC/length mismatch by returning <0 (so it is never averaged into ~20 mA). Optionally reject a
  **run of identical raw samples** across the 4‑sample window as a stuck signature. Keep the existing
  `validSamples==0 → NAN` escalation.
- *Accept:* a forced `0xFFFF`/garbage frame yields `NAN`/fault, never a ~20 mA reading; stuck‑identical reads are
  flagged.
- *Risk:* Low–Medium — must **not** reject a *legitimate* full‑scale 20 mA: distinguish "raw exactly
  `0xFFFF` / CRC‑fail" (garbage) from "raw near full‑scale but CRC‑valid" (real). Bench‑confirm the boundary.

### 9.3 Phase 2 — the one clock change, BENCH‑GATED (separate release)

**T6 — Build the bench harness + measure (§4).** Notecard + SunSaver/Modbus + A0602 on the real wiring; scope
SDA/SCL; reproduce the field load. Classify the failure: **temporal** (reads not serviced in time) vs
**electrical** (marginal edges / weak pull‑ups). Capture A0602 ok/NACK with the T4 counters.

**T7 — Implement ONE lever, per the data.**
- If **temporal** → per‑op **400 kHz window** (§3.3): `Wire.setClock(400000)` around A0602 frames with a
  guaranteed `Wire.setClock(100000)` restore on **every** return path (mirror the T1 timeout restore).
- If **electrical/marginal** → **lower** the global bus to **50 kHz** via a tunable `I2C_BUS_CLOCK_HZ`
  (`CODE_REVIEW_06222026_DEFERRED_ITEMS` 11‑C), applied after every `Wire.begin()`.
- **Ship only the winner.** Not both. Not blind.
- *Accept:* on the full harness, A0602 reads reliable **and** the Notecard session stays healthy (no sync
  failures) for ≥ N cycles.
- *Risk:* Medium — clock changes affect the shared Notecard; the bench gate is mandatory (global 400 kHz was
  previously removed for breaking the Notecard).

### 9.4 Phase 3 — physical verification (NOT firmware, parallel)

**T8 — Field/bench hardware check.** Confirm the 4–20 mA **loop supply actually energises when P1 is ON**
(measure the current), the **pull‑ups** and cable length/capacitance are in spec, the **P1 wiring** matches
`P1_Transistor_Gating_Test`, and the specific A0602 unit reads correctly standalone. Because the symptom is
"works on the bench, never in the field," a persistent physical/config delta is at least as likely as contention.

### 9.5 Validation & rollout (each firmware phase)
1. Compile **client** (`arduino:mbed_opta:opta:security=sien` + `-DTANKALARM_DFU_MCUBOOT`) and **server**; both
   clean (client ≈ 18 %, server ≈ 46 %); confirm the client `*.ino.bin` is the ~476 KB signed slot (magic
   `3d b8 f3 96`).
2. Bump `FIRMWARE_VERSION` + `FIRMWARE_BUILD_SEQ` (`Common.h`) + `library.properties`; write
   `release-notes/vX.Y.Z.md`.
3. Commit (one logical change) → `git pull --rebase` → push → **tag `vX.Y.Z`** → CI builds + signs + magic‑byte
   guard.
4. Assign `TankAlarm-Client-secure-vX.Y.Z.slot.bin` on Notehub to the field device; it self‑applies (trial‑boot
   → confirm).
5. Watch telemetry: `i2c_errs` ↓, `cl_ok` ↑, a real `ma`/`lvl` appears, `cl_fault` reason narrows — compare
   pre/post.

### 9.6 Risks & rollback
- All Phase‑1 items are additive; MCUboot trial‑boot **auto‑rolls back** a bad client image (no field brick).
- T3 (recovery) and T7 (clock) touch the shared bus → both must prove the **Notecard session survives** before
  field rollout.
- If post‑deploy `i2c_errs` stays high and no valid `ma` appears, conclude the cause is **physical (T8)** and
  stop tuning firmware — the shared‑bus ceiling has been reached (§5 honest expectation).

### 9.7 Consolidated todo checklist

**Phase 1 — firmware, no bench (target v2.0.46)**
- [ ] **T1** Add `A0602_WIRE_TIMEOUT_MS = 25`; change L5409 scope `100 → 25` ms; raise `I2C_WIRE_TIMEOUT_MS`
  `25 → 50` ms (`TankAlarm_Config.h` L146); keep all restore points on `I2C_WIRE_TIMEOUT_MS`.
- [ ] **T2** Lock `sampleMonitors()` order: Notecard I/O → drain Wire RX → settle + WDT → A0602 burst; trim after
  sampling; no Notecard op inside the config→read window.
- [ ] **T3** On config‑NACK (~L5440) and `validSamples==0` (~L5544): `recoverI2CBus()` +
  `tankalarm_ensureNotecardBinding()` with bounded backoff + `gDfuInProgress` guard.
- [ ] **T4** Add `gCurrentLoopReadsOk` + `lastClFaultReason`; emit `cl_ok`/`cl_fault` in telemetry.
- [ ] **T5** Reject `0xFFFF`/CRC‑fail/stuck‑identical framed samples; keep `validSamples==0 → NAN`.
- [ ] Compile client(sien)+server → bump version/seq/library → release‑notes → tag → OTA → verify telemetry.

**Phase 2 — bench‑gated clock (separate release)**
- [ ] **T6** Bench harness (Notecard+Modbus+A0602) + scope; classify temporal vs electrical.
- [ ] **T7** Implement the winning lever **only** (per‑op 400 kHz window **or** global 50 kHz); prove the Notecard
  stays healthy; ship.

**Phase 3 — physical (not firmware, parallel)**
- [ ] **T8** Verify loop supply energises on P1‑ON (measure current), pull‑ups/cable, P1 wiring, A0602 unit
  standalone.

---

*End of implementation plan. Phase 1 is self‑contained and shippable now; Phase 2 must not ship until the §4
/ T6 bench measurement picks the clock direction; Phase 3 proceeds independently and may prove to be the actual
fix if firmware discipline alone does not clear the field symptom.*

---

## 10. AI Assistant's Review & Additional Suggestions (GitHub Copilot, 2026-06-24)

Upon reviewing the implementation plan in Section 9, the phased approach is highly disciplined and prioritizes low-risk, high-value fixes correctly. Here are a few additional suggestions and refinements to consider incorporating into the plan:

### 10.1 Refinements to Phase 1 (Firmware, No Bench)
- **T2 (Transaction Ordering) Enhancements:** Consider wrapping the A0602 critical section in `noInterrupts()` / `interrupts()` (if the underlying Mbed I²C driver allows it without deadlocking) or taking an Mbed Mutex if the RTOS is multitasking. Even though the codebase is predominantly single-threaded, ensuring Mbed system ticks or peripheral interrupts don't preempt the short inter-device settle keeps the temporal window completely deterministic.
- **T3 (Bus Recovery) Depth:** Ensure that the `tankalarm_recoverI2CBus()` implementation completely clears the STM32 I²C peripheral HAL state. Sometimes `Wire.end()` and `Wire.begin()` leave residual errors/flags in the HAL struct on STM32 platforms. SCL clocking (bit-banging 16 pulses) is excellent for freeing the slave, but a full peripheral reset map register toggle (e.g., via `__HAL_RCC_I2C3_FORCE_RESET()`) is the gold standard for completely un-wedging the master.
- **T5 (Garbage Rejection) Tuning:** When rejecting "stuck-identical" samples, leverage the fact that real analog loops *always* have at least 1-2 LSBs of noise. If 4 consecutive raw samples are bit-for-bit identical (zero variance), it is a mathematical near-impossibility for a real 16-bit analog signal and guarantees a frozen or locked I²C buffer. Use this to aggressively filter out false-positives while preserving legitimate steady-state readings.

### 10.2 Refinements to Phase 2 (Bench-Gated)
- **Firmware-based Bus Profiling:** You don't necessarily need a physical scope to prove the temporal hypothesis. In T6, add a temporary `micros()` wrapper around the Notecard sync and the A0602 burst. Log the max durations to telemetry (e.g., `doc["cl_dur_us"]`). This turns every field unit into a low-speed logic analyzer, immediately confirming whether A0602 faults correlate with abnormally high transaction times or jitter.

### 10.3 Refinements to Phase 3 (Physical Verification)
- **Inrush Current Profiling:** When verifying the P1-ON loop supply (T8), use a high-speed scope to check for voltage droop or inrush current spikes on the 24V/12V rail that powers the A0602. If powering the loop causes a brief brownout on the expansion module's local logic rails, it would immediately cause an I²C NACK or latch-up that is indistinguishable from bus contention on the software side.

---

## 11. Implementation Plan Addendum / Proposed Changes (GitHub Copilot, 2026-06-24)

After a targeted read of the Phase 1 code paths, I would keep the Section 9 plan but make the following changes before implementing v2.0.46.

### 11.1 Make T2 cover every Notecard publish inside sampling

The plan correctly defers `trimTelemetryOutbox()`, but `sampleMonitors()` can still call `sendTelemetry()` inside the monitor loop, and `sendTelemetry()` immediately calls `publishNote()`. Alarm/recovery paths can also publish notes while sampling. If there is more than one current-loop monitor, that means Notecard traffic can still occur between A0602 reads.

**Proposed change:** revise T2 from "defer trim" to "defer all Notecard writes produced by the sampling pass." The cleanest implementation is a two-phase sample pass on devices with current-loop monitors:

1. acquire and validate all monitor readings, with no `publishNote()` calls;
2. after all A0602 reads are complete, emit telemetry/alarm/unload notes, then run the deferred outbox trim.

If a full two-phase refactor is too much for v2.0.46, add a narrower `deferSamplePublishes` queue for threshold sample uploads and recovery diagnostics, and explicitly document any publish path that still remains inline.

### 11.2 Do not use `noInterrupts()` around Wire transactions

The Section 10 suggestion to consider `noInterrupts()` should not ship as written. On the Opta/Mbed stack, `Wire` transactions, timeouts, watchdog service, and RTOS timing may depend on interrupts or scheduler progress. Disabling interrupts around I2C is more likely to create a deadlock or watchdog problem than to improve bus ownership, especially since the firmware already has one host master and the real issue is sequencing of host-issued transactions.

**Proposed change:** keep the critical section logical, not interrupt-level: no Notecard API calls, drain RX, short settle, A0602 config/read burst, guaranteed timeout/clock restore. If future evidence proves another task can issue Notecard calls concurrently, introduce one shared software gate around Notecard/A0602 access rather than disabling interrupts.

### 11.3 Treat recovery as a scheduled shared-bus action

T3 is directionally right, but recovery should reuse the existing sensor-only recovery/backoff model where possible. Calling `recoverI2CBus()` is fine on a wedged bus, but publishing a recovery diagnostic or doing other Notecard work from inside the A0602 read path would undercut T2's isolation.

**Proposed change:** on config-NACK or `validSamples==0`, record the fault reason, restore the Wire timeout, power P1 off if needed, and mark an A0602 recovery as pending. Service the pending recovery immediately after the A0602 sampling burst, subject to the existing backoff/circuit-breaker constants, then call `tankalarm_ensureNotecardBinding(notecard)`. Publish the recovery diagnostic only in the post-sampling publish phase.

Also keep the HAL/peripheral-reset idea as a bench-only escalation. The current helper already does `Wire.end()`, SCL clocking, STOP, `Wire.begin()`, timeout restore, DFU guard, and watchdog kick. A direct STM32 reset macro would be board-core-specific and should not be added unless the existing helper demonstrably fails. Before any recovery-depth work, fix or re-check the stale pin comment in `TankAlarm_I2C.h`; the code uses board constants, which is good, but the comment should not contradict the Opta variant mapping.

### 11.4 Change T5 to return raw/status, not just milliamps

Rejecting `0xFFFF` is the right instinct, but the current `tankalarm_readCurrentAdcFramed()` returns only scaled milliamps. The caller cannot reliably distinguish "raw exactly `0xFFFF`" from a near-full-scale valid reading once the value has been converted to `float`.

**Proposed change:** add a raw/status API, for example `tankalarm_readCurrentAdcRawFramed(channel, addr, rawOut)`, and let the existing float helper wrap it. T5 should reject raw `0xFFFF` before scaling and should count CRC/length/channel failures separately from all-ones samples.

For stuck-identical detection, start as telemetry-only or require a very specific signature. Four identical raw samples can happen on a stable slow process, especially with the 300 ms gated settle cadence. Do not hard-fault ordinary zero-variance samples in v2.0.46 unless bench data proves the A0602 always has enough LSB noise. A safer Phase 1 rule is: reject exact `0xFFFF`; observe exact-repeat runs via `cl_fault`/debug counters; promote repeat rejection later if field data supports it.

### 11.5 Make the new telemetry small and reset-scoped

T4 should define counter scope before implementation. The existing `gCurrentLoopI2cErrors` is effectively a health-window counter; adding lifetime-style `cl_ok` beside it would make field interpretation muddy.

**Proposed change:** keep telemetry compact and windowed: `cl_ok`, `cl_fail`, `cl_fault`, and optionally `cl_rec` should reset on the same cadence as the current I2C health counters. Use small integer fault codes in the note payload, with the string mapping documented in the release notes, to avoid growing every telemetry note.

### 11.6 Tighten Phase 1/Phase 2 gating language

Phase 1 is "no scope/clock experiment required," not literally "no bench." T3 touches the shared bus and should at least get a bench-smoke test with the Notecard present: force an A0602 NACK/read failure, confirm recovery runs once under backoff, confirm Notecard binding survives, and confirm no watchdog reset.

For Phase 2, define the acceptance target before changing clock speed: number of sampling cycles, forced `hub.sync` cadence, zero Notecard request failures, zero watchdog resets, and the acceptable A0602 failure rate. Without a numeric gate, the 50 kHz-vs-400 kHz decision will be too easy to rationalize from a short successful run.

---

## 12. Third Reviewer's Assessment & Proposed Changes (GitHub Copilot, 2026‑06‑24)

I read the implementation plan (§9), its two prior addenda (§10, §11), and the actual code paths they reference
(`readCurrentLoopSensor()` L5340–5628, `sampleMonitors()` L5689–5750, `tankalarm_readCurrentAdcFramed()`
L453–496, `tankalarm_recoverI2CBus()` L69–95, `publishNote()` L8043+, `sendTelemetry()` L6016–6065). The §9
plan is well‑structured, correctly phased, and the §11 addendum catches the two most important sequencing gaps
(deferred publishes, recovery ordering). I agree with the overall priority order and the Phase 1/2/3 gates.
Below are concrete issues I found in the code that the plan should address, plus changes I'd make to the tasks.

### 12.1 T5 needs a code‑level correction: `0xFFFF` → 25.0 mA, not ~20 mA

The plan says reject "raw `0xFFFF` / ~20 mA." That's slightly wrong. The actual scaling in
`tankalarm_readCurrentAdcFramed()` (L496) is:

```cpp
uint16_t raw = (uint16_t)a[4] | ((uint16_t)a[5] << 8);
return 25.0f * (float)raw / 65535.0f;
```

So raw `0xFFFF` → **25.0 mA**, not ~20 mA. On a 4–20 mA loop that's clearly over‑range (the transmitter's
full‑scale is 20 mA). The existing live‑zero fault check (`< 3.6 mA → NAN`) catches under‑range, but there is
**no over‑range guard**: any raw value above ~52429 (≈ 20.0 mA) passes validation. The ~18–20 mA field
signature the document describes is **not** `0xFFFF` — it's a near‑full‑scale but CRC‑valid value, which means
the garbage isn't always all‑ones.

**Proposed change to T5:** add two guards inside `tankalarm_readCurrentAdcFramed()`, before the float conversion:

1. **Reject raw `0xFFFF` unconditionally** — this is a bus‑pulled‑high signature, never a real ADC sample.
   Return `−1.0f`.
2. **Add an over‑range guard in the caller** (`readCurrentLoopSensor()`): if the averaged milliamps exceeds a
   defined `CURRENT_LOOP_OVER_RANGE_MA` (e.g. 21.0 mA, providing some headroom above a legitimate 20.0 mA
   full‑scale), treat it as a fault — log, bump `gCurrentLoopI2cErrors`, return `NAN`. This catches the
   near‑full‑scale garbage the field is actually seeing, where the raw value isn't exactly `0xFFFF` but is
   still physically impossible for the installed transmitter.

The "stuck‑identical" detection should remain telemetry‑only in v2.0.46 per §11.4's recommendation — the 300 ms
gated settle cadence can legitimately produce identical samples on a stable process.

### 12.2 `sendTelemetry()` during sampling is the biggest remaining contention vector

§11.1 identified this but proposed a deferred queue as a "if the full two‑phase refactor is too much" fallback.
Having read the code, I believe it is **not** optional — it is the **primary remaining contention path**. Here
is the exact mechanism:

In `sampleMonitors()` (~L5737), after each monitor's `readMonitorSensor()` returns:
```cpp
if (gConfig.monitors[i].enableServerUpload && !gMonitorState[i].sensorFailed) {
  sendTelemetry(i, "sample", false);
}
```

`sendTelemetry()` → `publishNote()` → `notecard.requestAndResponse()`, which can hold the I²C bus for up to
**30 seconds**. If there are **two or more monitors** and the first is an A0602, the Notecard traffic from
monitor 0's `sendTelemetry()` lands **directly before** monitor 1's A0602 read — exactly the interleaving T2 is
supposed to prevent.

Even with a single monitor, `evaluateAlarms()` and `evaluateUnload()` may also publish Notecard notes (alarm
state transitions, unload events) between the A0602 read and the next cycle. The deferred‑trim only covers
`trimTelemetryOutbox()`; the inline publishes are unguarded.

**Proposed change:** T2 should explicitly require that `sendTelemetry()`, alarm publishes, and unload publishes
produced during the sampling `for` loop are **buffered and flushed after the loop exits** (before the deferred
trim). This is the §11.1 "two‑phase sample pass" and it should be a hard requirement for v2.0.46, not a
nice‑to‑have. The implementation is straightforward: accumulate `JsonDocument`s (or serialized strings) into a
small vector during the loop, then iterate and `publishNote()` after the loop and before `trimTelemetryOutbox()`.

### 12.3 Recovery pin comment in `TankAlarm_I2C.h` is misleading

`tankalarm_recoverI2CBus()` (L78–80) uses:
```cpp
const int I2C_SCL_PIN = PIN_WIRE_SCL;  // PB_8
const int I2C_SDA_PIN = PIN_WIRE_SDA;  // PB_9
```

But the Opta variant file (`variants/OPTA/pins_arduino.h`) maps `Wire` (I2C3) to **PH_7 / PH_8**, not PB_8/PB_9.
The `PIN_WIRE_SCL` / `PIN_WIRE_SDA` macros resolve correctly at compile time — the code works — but the inline
comments `// PB_8` / `// PB_9` are **wrong** and could confuse anyone using this document to trace pins. §11.3
mentioned this; T3 should include fixing the comments to `// PH_7 (I2C3_SCL)` / `// PH_8 (I2C3_SDA)` when that
code is touched.

### 12.4 T1 timeout rebalance needs a fourth restore point

The plan identifies three existing `Wire.setTimeout(I2C_WIRE_TIMEOUT_MS)` restore points. But the config‑NACK
early‑return (~L5448) and the func‑mismatch early‑return (~L5461) both call `Wire.setTimeout(I2C_WIRE_TIMEOUT_MS)`
**after** the A0602 scope was set at L5409. When T1 lowers the A0602 scope to 25 ms, those early‑return restores
are fine. However, the **PWM‑OFF retry loop** (~L5520–5535) runs at whatever timeout was set at L5409; if the bus
is wedged during A0602 reads and recovery (T3) runs, the restored timeout must be **explicitly set before the
PWM‑OFF attempts**, because the recovery calls `Wire.begin()` which resets the timeout to the library default
(1000 ms on Mbed). Add a `Wire.setTimeout(A0602_WIRE_TIMEOUT_MS)` after recovery and before PWM‑OFF, and the
normal `Wire.setTimeout(I2C_WIRE_TIMEOUT_MS)` after PWM‑OFF.

### 12.5 T4 counter scope: align with the existing daily‑reset cadence

`gCurrentLoopI2cErrors` is already reset daily (~L7886) and emitted in the daily report as `i2c_cl_err` (~L9767).
The new `gCurrentLoopReadsOk` and `lastClFaultReason` should follow the same pattern: emit in the daily report,
then reset. Do **not** emit them on every `sendTelemetry()` sample note — that would bloat every telemetry
payload for a diagnostic that only needs daily granularity. For real‑time debugging during the rollout, the
existing serial logging is sufficient; once the field is stable, the daily counters tell the story.

Exception: the first occurrence of a new fault reason *within* a daily window could be emitted as a one‑shot
diagnostic note (like the existing I2C recovery diag at L5056) so the field team sees it promptly, but keep it
gated to avoid flooding.

### 12.6 Phase 1 should include a compile‑time A0602 feature gate

Several of the §9 tasks (T1–T5) add logic that is only relevant to devices with an A0602 expansion module. Not
every deployed client has one. To keep the binary clean and reduce risk on non‑A0602 units, wrap the new A0602
recovery (T3) and the over‑range/`0xFFFF` rejection (T5) behind the existing `hasCurrentLoopMonitor()` runtime
check or, better, a `#ifdef TANKALARM_HAS_A0602` compile‑time guard that is defined alongside
`TANKALARM_DFU_MCUBOOT`. This prevents the recovery codepath from ever executing on a device that has no
expansion module — where `recoverI2CBus()` could gratuitously cycle the shared bus for the Notecard.

### 12.7 Summary of proposed changes to §9

| Task | §9 as written | Proposed change |
|---|---|---|
| **T1** | 3 restore points | Add a 4th restore after T3 recovery / before PWM‑OFF (§12.4) |
| **T2** | Defer trim + drain RX | **Hard require** deferred publishes for `sendTelemetry()`, alarms, and unloads during the sampling loop — not optional (§12.2) |
| **T3** | Recovery in config‑NACK / `validSamples==0` | Keep §11.3's "record + service after burst" model; fix pin comments; gate behind `hasCurrentLoopMonitor()` or compile flag (§12.3, §12.6) |
| **T4** | `cl_ok` / `cl_fault` in telemetry | Emit in daily report only, not per‑sample; reset on the same daily cadence as `gCurrentLoopI2cErrors` (§12.5) |
| **T5** | Reject `0xFFFF` | Also add an **over‑range guard** (> 21 mA) in the caller; keep stuck‑identical as telemetry‑only (§12.1) |
| **New** | — | Feature‑gate A0602 recovery/rejection behind runtime or compile‑time check (§12.6) |

### 12.8 Items I agree with and would not change

- **Phase ordering** (1 → 2 → 3) and the bench gate on Phase 2: correct.
- **§11.2** (do not use `noInterrupts()`): correct; the Mbed RTOS scheduler and Wire driver depend on interrupts.
- **§11.3** (recovery as a post‑burst action, not inline): correct; keeps T2 isolation clean.
- **§11.4** (raw/status API for T5): directionally right, but the simpler approach is to add the `0xFFFF` check
  **inside** `tankalarm_readCurrentAdcFramed()` (it already has the raw value in hand) rather than refactoring
  the return type. The over‑range check belongs in the caller because it operates on the averaged float.
- **§8.1** (shorten A0602 timeout, lengthen Notecard): correct on the reasoning. 25 ms A0602 / 50 ms Notecard
  is the right starting point.
- **§8.3 priority order** (isolation → garbage guard → observability → clock): agree completely.

---

*End of third review. The §9 plan is sound. The changes above are refinements to close gaps visible in the code,
not architectural redirections. The most important one is §12.2: deferred publishes during the sampling loop are
a hard requirement, not a stretch goal, because `sendTelemetry()` calls `publishNote()` which can hold the
shared bus for up to 30 seconds — exactly the contention vector the plan exists to eliminate.*

---

## 13. FINAL Implementation Plan — proceed with this (GitHub Copilot, 2026‑06‑24)

This section **consolidates §9 (plan) + §10/§11/§12 (reviews) into the single plan we will execute.** Where the
sections conflicted, the decision is stated below with its rationale. Every code claim was **re‑verified against
the source on 2026‑06‑24** (line numbers current as of this date). **This section supersedes the §9 task details
where they differ.** Tasks are renamed **FT1–FT8** to avoid confusion with the §9 T‑numbers.

### 13.1 Decisions log (how the review conflicts were resolved)

| # | Question | Sources | **Decision** | Rationale |
|---|---|---|---|---|
| D1 | Defer only the outbox trim, or **all** Notecard publishes during sampling? | §9 T2 vs §11.1/§12.2 | **Defer ALL publishes (two‑phase pass). HARD requirement, #1 priority.** | `sendTelemetry(i,"sample",false)` runs **inside** the loop (L5737) → `publishNote()` can hold the bus ≤30 s right before the next A0602 read. Verified. |
| D2 | `noInterrupts()` / mutex around the A0602 section? | §10.1 vs §11.2 | **Reject `noInterrupts()`.** Use a *logical* critical section (no Notecard calls + RX drain + settle). | Mbed RTOS scheduler, `Wire`, timeouts and watchdog depend on interrupts; disabling them risks deadlock. Single master ⇒ sequencing, not masking, is the fix. |
| D3 | Recovery inline in the read, or scheduled after the burst? | §9 T3 vs §11.3/§12.8 | **Post‑burst, scheduled.** Record fault → service recovery after the A0602 burst, before Phase‑B publishes. | Keeps the A0602 owned‑section clean; avoids a mid‑read `Wire.begin()` (which resets the timeout). |
| D4 | Add an STM32 HAL peripheral reset (`__HAL_RCC_I2C3_FORCE_RESET`) to recovery? | §10.2 vs §11.3 | **Defer — bench‑only escalation.** Ship the existing `Wire.end()`+16×SCL+STOP+`Wire.begin()` helper. | Board‑core‑specific; only add if the existing helper demonstrably fails on the bench. Don't ship blind. |
| D5 | T5 garbage rejection: refactor the read to return raw/status, or guard in place? | §11.4 vs §12.1/§12.8 | **Guard in place.** Reject raw `0xFFFF` **inside** `tankalarm_readCurrentAdcFramed()`; add an **over‑range guard** in the caller. | The framed read already has the raw in hand; no return‑type refactor needed. Less churn. |
| D6 | Hard‑fault "stuck‑identical" samples? | §10.1 vs §11.4/§12.1 | **No — observe only in v2.0.46.** Count it; do not fault. | The 300 ms gated settle cadence can legitimately produce identical samples on a stable process. Promote later only if field data supports it. |
| D7 | New counters per‑sample or daily? | §9 T4 vs §11.5/§12.5 | **Daily‑scoped.** Emit in the daily report beside `i2c_cl_err` (L9767); reset with it (L7886). | Don't bloat every sample note; the per‑sample running count already exists as `i2c_errs` (L5056). |
| D8 | New compile‑time `TANKALARM_HAS_A0602` gate? | §12.6 | **Not needed.** Recovery/rejection live inside `readCurrentLoopSensor()`, which only runs for current‑loop monitors; the two‑phase pass is already gated by `hasCurrentLoopMonitor()` (L5699). | Avoid added config surface (YAGNI); the code path is already inherently scoped. |
| D9 | Is FT3 truly "no bench"? | §9 vs §11.6 | **Phase 1, but bench‑smoke‑gated.** | Recovery toggles the shared bus → must prove the Notecard session survives before field rollout. |
| D10 | A0602 timeout direction | §3.2 vs §8.1/§12.8 | **A0602 25 ms (shorter), Notecard 50 ms (longer).** | A0602 doesn't clock‑stretch; the Notecard does (≤25 ms sync). Verified. |

### 13.2 Verified code facts the plan relies on (source, 2026‑06‑24)
- `tankalarm_readCurrentAdcFramed()` (`TankAlarm_I2C.h` ~L453‑496): returns `25.0f*raw/65535.0f`, so raw `0xFFFF` = **25.0 mA**; returns `−1.0f` on endTransmission≠0 / short read / header / CRC / **wrong‑channel** mismatch. **No `0xFFFF` or over‑range guard.**
- `readCurrentLoopSensor()` (client `.ino` L5345‑5628): P1‑enable NACK→`NAN` (already), `validSamples==0`→`NAN` (already), **under‑range** `milliamps < CURRENT_LOOP_FAULT_MA → NAN` (L5571), **but no over‑range guard** — `linearMap(mA,4,20,…)` extrapolates a >20 mA reading into a fake level/pressure.
- `sampleMonitors()` (L5689+): `deferTrim = hasCurrentLoopMonitor()` (L5699); **`sendTelemetry(i,"sample",false)` is called inside the monitor loop (L5737)**.
- Counters: per‑sample `doc["i2c_errs"]` (L5056); daily `doc["i2c_cl_err"]` (L9767); reset daily (L7886). `hasCurrentLoopMonitor()` at L5680.
- `tankalarm_recoverI2CBus()` (`TankAlarm_I2C.h` ~L70‑95): `Wire.end()` → 16×SCL toggle → STOP → `Wire.begin()`, with `dfuInProgress` guard + optional WDT kick. **Comment says `PB_8/PB_9` but `Wire`=I2C3 is `PH_7`(SCL)/`PH_8`(SDA)** — fix the comment.
- `I2C_WIRE_TIMEOUT_MS = 25` (`TankAlarm_Config.h` L146); A0602 scoped to `Wire.setTimeout(100)` at L5409.

### 13.3 The final task set

#### Phase 1 — firmware, ships as **v2.0.46** (FT3 bench‑smoke‑gated; rest need no bench)

**FT1 — Two‑phase sampling pass (the #1 change; HARD requirement).**
- *Where:* `sampleMonitors()`, gated by the existing `hasCurrentLoopMonitor()`.
- *Change:* **Phase A** — acquire + validate every monitor reading with **no `publishNote()`**; buffer the
  `(monitorIndex, reason)` of monitors that need a `"sample"` upload, plus any alarm/unload events. **Phase B** —
  after the loop: one final `Wire` RX drain, then flush the buffered telemetry/alarm/unload notes, **then**
  `trimTelemetryOutbox()`. Inside `readCurrentLoopSensor()` keep the owned section (no Notecard call between
  `tankalarm_configureCurrentAdcChannel()` and the framed reads).
- *Sub‑task:* audit `evaluateAlarms()` / unload paths for inline publishes during sampling and route them through
  the Phase‑B buffer too.
- *Accept:* **zero `publishNote()` between the first and last A0602 read of a cycle** (instrument with a temporary
  assert/log during bring‑up).
- *Risk:* Medium (refactor) — but it removes the dominant contention vector. Non‑A0602 devices are unaffected
  (gate).

**FT2 — Rebalance the Wire timeouts.**
- *Change:* add `A0602_WIRE_TIMEOUT_MS = 25`; lower the L5409 scope `100 → 25` ms; raise `I2C_WIRE_TIMEOUT_MS`
  `25 → 50` ms (`TankAlarm_Config.h` L146). **Always set the intended timeout immediately after any
  `Wire.begin()`/recovery** (the FT3 recovery resets it to the Mbed default) and before the PWM‑OFF retries.
- *Accept:* an A0602 read on a wedged bus fails within ~25 ms; Notecard ops get 50 ms; no path leaves the bus at
  the wrong timeout.
- *Risk:* Low (the 50 ms Notecard value is the documented conservative setting).

**FT3 — Post‑burst A0602 bus recovery (bench‑smoke‑gated).**
- *Change:* on config‑NACK (~L5440) or `validSamples==0` (~L5544): record the fault reason, restore the timeout,
  drive P1 off, set `gA0602RecoveryPending`. **After** the A0602 burst (and before Phase‑B publishes), if pending
  **and** the existing sensor‑only exponential backoff/circuit‑breaker allows: `recoverI2CBus()` →
  `Wire.setTimeout(I2C_WIRE_TIMEOUT_MS)` → `tankalarm_ensureNotecardBinding(notecard)`. Honor `gDfuInProgress`.
  **Fix the `PB_8/PB_9` comment → `PH_7`(SCL)/`PH_8`(SDA)** while in `TankAlarm_I2C.h`.
- *Bench‑smoke gate (mandatory before field):* with the Notecard present, force an A0602 fault → confirm exactly
  one recovery under backoff, the Notecard binding survives, and there is no watchdog reset.
- *Do NOT* add the HAL `__HAL_RCC_I2C3_FORCE_RESET()` reset unless this helper demonstrably fails on the bench.
- *Risk:* Medium (touches the shared bus) — the bench‑smoke gate covers it.

**FT4 — Garbage / over‑range rejection.**
- *Change:* (a) in `tankalarm_readCurrentAdcFramed()`, **reject raw `0xFFFF`** (`return −1.0f` before scaling);
  (b) in `readCurrentLoopSensor()`, add an **over‑range guard next to the L5571 under‑range guard**: if averaged
  `milliamps > CURRENT_LOOP_OVER_RANGE_MA` (define **21.0 mA**, headroom above a legit 20 mA full‑scale) → log,
  `gCurrentLoopI2cErrors++`, `return NAN`; (c) count a **4‑sample bit‑identical run** into a diagnostic counter
  **only — do not fault** (D6).
- *Accept:* a CRC‑valid `0xFFFF`/25 mA frame and any >21 mA average both fault; a legitimate ≤20 mA passes; a
  stable real process is never falsely faulted.
- *Risk:* Low — keep the boundary at 21 mA so a real 20.00 mA full‑scale survives.

**FT5 — Daily‑scoped observability.**
- *Change:* add `gCurrentLoopReadsOk`, `gCurrentLoopRecoveries`, and `lastClFaultReason` (small **int code**:
  0 none / 1 pwm‑nack / 2 config‑nack / 3 func‑wrong / 4 read‑fail / 5 over‑range / 6 recovered). Emit in the
  **daily report** beside `i2c_cl_err` (L9767) and reset them with it (L7886). Do **not** add them to every
  per‑sample note. *(Optional)* a single one‑shot diag note on the first occurrence of a new fault code per
  window. Document the code mapping in `release-notes/v2.0.46.md`.
- *Accept:* the daily report shows ok / fail / recovery counts and the dominant fault code per device.
- *Risk:* Low (additive).

*Phase‑1 rollout:* compile **client (`security=sien` + `-DTANKALARM_DFU_MCUBOOT`)** and **server** clean (verify
the client `*.ino.bin` is the ~476 KB signed slot, magic `3d b8 f3 96`); bump `2.0.45 → 2.0.46` + `FIRMWARE_BUILD_SEQ`
+ `library.properties`; write `release-notes/v2.0.46.md` (incl. the FT5 fault‑code table); commit per logical
change; `git pull --rebase`; **tag `v2.0.46`**; OTA‑assign the signed slot; watch telemetry: `i2c_cl_err` ↓,
`cl_ok` ↑, a real `ma`/`lvl` appears, fault code narrows.

#### Phase 2 — the ONE clock change, BENCH‑GATED (separate release, e.g. **v2.0.47**)

**FT6 — Measure and define the gate.** Build the bench harness (Notecard + SunSaver/Modbus + A0602 on the real
wiring) + scope SDA/SCL **and** add temporary firmware profiling `doc["cl_dur_us"]` (`micros()` around the
Notecard sync and the A0602 burst) so field units also report timing/jitter. **Define the numeric acceptance gate
up front:** ≥ N sampling cycles, a forced `hub.sync` cadence, **zero** Notecard request failures, **zero**
watchdog resets, and an A0602 failure rate below a stated threshold. Classify the failure **temporal** vs
**electrical**.

**FT7 — Implement exactly one lever, per the data.** Temporal → per‑op **400 kHz window** around A0602 frames
with a guaranteed `Wire.setClock(100000)` restore on **every** return path. Electrical/marginal → **lower** the
global bus to **50 kHz** via a tunable `I2C_BUS_CLOCK_HZ`, set after every `Wire.begin()`. **Ship only the winner,
only after it meets the FT6 gate.** Not both. Not blind.

#### Phase 3 — physical verification (NOT firmware, parallel)

**FT8 — Hardware check on the actual install.** Confirm the 4–20 mA **loop supply energises when P1 is ON**
(measure the current); **scope the A0602 supply rail for inrush/droop/brownout** the instant P1 switches (a
brownout NACKs I²C indistinguishably from contention); verify **pull‑ups**, cable length/capacitance, the **P1
wiring** vs `P1_Transistor_Gating_Test`, and that the specific A0602 reads correctly standalone.

### 13.4 Final consolidated todo checklist

**Phase 1 — firmware → v2.0.46**
- [ ] **FT1** Two‑phase `sampleMonitors()` (gated `hasCurrentLoopMonitor()`): acquire‑all (no publishes) →
  drain RX → flush buffered telemetry/alarm/unload → `trimTelemetryOutbox()`. Audit alarm/unload inline publishes.
  Verify zero `publishNote()` between first/last A0602 read.
- [ ] **FT2** `A0602_WIRE_TIMEOUT_MS=25` (L5409 `100→25`); `I2C_WIRE_TIMEOUT_MS 25→50` (Config.h L146); re‑set
  timeout after every `Wire.begin()`/recovery and before PWM‑OFF.
- [ ] **FT3** Post‑burst recovery on config‑NACK / `validSamples==0` (record→service after burst) with backoff +
  `gDfuInProgress` guard + `ensureNotecardBinding()`; fix `PB_8/PB_9`→`PH_7/PH_8` comment. **Bench‑smoke test.**
- [ ] **FT4** Reject raw `0xFFFF` in `tankalarm_readCurrentAdcFramed()`; add `CURRENT_LOOP_OVER_RANGE_MA=21.0`
  guard by the L5571 under‑range check; count (don't fault) 4‑sample identical runs.
- [ ] **FT5** Add `gCurrentLoopReadsOk` / `gCurrentLoopRecoveries` / `lastClFaultReason` (int code); emit + reset
  in the **daily** report (L9767 / L7886); document codes in release notes.
- [ ] Compile client(sien)+server → bump version/seq/library → `release-notes/v2.0.46.md` → tag `v2.0.46` → OTA →
  verify telemetry deltas.

**Phase 2 — bench‑gated clock → v2.0.47**
- [ ] **FT6** Bench harness + scope + `cl_dur_us` profiling; **write the numeric acceptance gate**; classify
  temporal vs electrical.
- [ ] **FT7** Ship the one winning lever (per‑op 400 kHz window **or** global 50 kHz) only after it meets the gate.

**Phase 3 — physical (not firmware, parallel)**
- [ ] **FT8** Loop supply energises on P1‑ON (measure); scope rail for inrush/droop; pull‑ups/cable; P1 wiring;
  A0602 standalone.

### 13.5 What changed from the §9 plan (delta)
- **§9 T2 → FT1:** elevated to the #1 item and **hardened** from "defer trim" to a full **two‑phase deferred‑publish**
  pass (the verified `sendTelemetry`‑in‑loop at L5737 is the real residual contention vector).
- **§9 T3 → FT3:** reordered to **post‑burst** + a **bench‑smoke gate** + the `PB_8/PB_9`→`PH_7/PH_8` comment fix;
  HAL peripheral reset deferred to bench‑only.
- **§9 T5 → FT4:** corrected with verified scaling (`0xFFFF` = **25.0 mA**, not ~20) and an explicit **over‑range
  guard** (>21 mA); stuck‑identical is **observe‑only** in v2.0.46.
- **§9 T4 → FT5:** moved to **daily‑scoped** counters (not per‑sample), matching the existing `i2c_cl_err` pattern.
- **§9 T1 → FT2:** adds the **post‑recovery timeout re‑set** (4th restore) so `Wire.begin()` can't leave a stale
  timeout.
- **Rejected:** `noInterrupts()` (D2); shipping the HAL reset blind (D4); hard‑faulting stuck‑identical in v2.0.46
  (D6); a new compile‑time A0602 flag (D8).

---

*This Section 13 is the plan of record. Proceed Phase 1 → 2 → 3 in order; Phase 2 must not ship until FT6's bench
measurement picks the clock direction and its numeric gate is met; Phase 3 runs in parallel and may prove to be
the actual fix if firmware discipline alone does not clear the field symptom.*