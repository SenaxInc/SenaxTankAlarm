# A0602 Current‑Loop — USB‑Only Diagnostic Plan (post‑v2.0.46 failure)

**Date:** 2026‑06‑24
**Author:** GitHub Copilot (AI)
**Status:** Test plan / TODO — **no firmware changed**
**Trigger:** The v2.0.46 I²C *contention* fix did **not** correct the field symptom. We are about to
connect to the **test Opta client** to diagnose over **USB only** (no scope, no multimeter, no electrical
test gear). New field observation: the **A0602 status LED is RED** with **2 channel LEDs lit (yellow)**.
**Follows:** `OPTA_I2C_BUS_SEPARATION_FEASIBILITY_06242026.md` (the §9–§13 v2.0.46 plan that shipped and
did not fix the issue) and `CODE_REVIEW_06152026_CURRENT_LOOP_MA_READING.md` (original symptom write‑up).

---

## 0. TL;DR — the reframe, and the one test that settles it

Everything we shipped through **v2.0.46 assumed the root cause was I²C bus *contention*** between the
Notecard (0x17) and the A0602 (0x64). We added scoped timeouts, a per‑op 400 kHz window, `0xFFFF` /
over‑range rejection, PWM‑NACK fail‑safe, and channel‑function confirmation. **It changed nothing in the
field.** Per our own methodology, *a fix that fails is evidence against its hypothesis.* Contention is
probably **not** the axis.

The **RED status LED is the new, decisive clue**, and it is **grounded in the Blueprint library source**
(not a guess — see §1). On an Opta analog expansion:

> **RED status LED = "ready for address" = the A0602 is UNMANAGED** (no controller ever assigned it an
> address this power cycle). The **healthy, controller‑managed state is GREEN** ("has address").

Our **production firmware never runs `OptaController`** — it talks **raw framed I²C** to the module. So the
A0602 is **never driven through the Blueprint bootstrap** and sits unmanaged (RED). That is a *different and
more fundamental* problem than contention, and it lines up with the original symptom (a **stable,
physically‑impossible, never‑valid** reading — the signature of talking to a module that isn't in a valid
read state, not of occasional bus collisions).

**The single decisive, USB‑only, equipment‑free test:** flash the existing **`Sensor_Utility`** sketch
(which uses the official `OptaController` / `OptaBlue` API) and watch the A0602:

- **If the status LED turns GREEN, the expansion enumerates, and it reads ≈4 mA correctly** → the hardware,
  bus, power and module are all fine; **the production raw‑I²C / unmanaged path is the bug.** Fix = adopt the
  managed `OptaController` path (or bootstrap the module before raw reads). This also explains why v2.0.46
  did nothing.
- **If it stays RED/BLUE and `OptaController` cannot enumerate it** → the Blueprint bootstrap itself fails;
  the problem is the **AUX/expansion bus or module power/health**, not firmware contention.

Everything below builds the evidence to land on one of those two branches without any test equipment.

---

## 1. What the A0602 LEDs actually mean (source‑grounded)

Verified in the installed library `C:\Users\dorkm\Documents\Arduino\libraries\Arduino_Opta_Blueprint\src`:

| LED state | Library function (`OptaAnalog.cpp`) | RGB drive | Meaning |
|---|---|---|---|
| **GREEN** | `setStatusLedHasAddress()` (L2604) | GREEN on, RED/BLUE off | **Healthy** — controller assigned an address; module is **managed**. |
| **RED** | `setStatusLedReadyForAddress()` (L2592) | RED on, GREEN/BLUE off | Booted and **ready to receive** an address, but **none assigned yet** → **UNMANAGED**. |
| **BLUE** | `setStatusLedWaitingForAddress()` (L2598) | BLUE on, RED/GREEN off | Initial power‑up, signalling it wants an address. |

Addressing constants (`OptaBluePrintCfg.h`):

- `OPTA_DEFAULT_SLAVE_I2C_ADDRESS = 0x0A` — where an **unmanaged** module listens.
- `OPTA_CONTROLLER_FIRST_AVAILABLE_ADDRESS = 0x0B` — first address a controller **assigns**.
- **`0x64` is *not* a Blueprint address.** It is a TankAlarm convention. Tellingly, the client's
  `CURRENT_LOOP_I2C_ALT_ADDRESS_1 = 0x0A` is **exactly the unmanaged default** — the firmware author already
  anticipated the module sitting unmanaged and added 0x0A as a fallback (`resolveCurrentLoopI2cAddress()`).

The **8 channel LEDs are host‑set only** (`OptaAnalog.cpp` L2250: `led_status = rx_buffer[OA_SET_LED_VALUE_POS]`).
They are **never** auto‑lit by channel configuration, PWM, or current flow. Therefore the **2 lit (yellow)
channel LEDs are leftover state from a prior *managed* session** (e.g., a previous `Sensor_Utility` /
Blueprint run that set them), persisting because the module has not been power‑cycled. Production raw‑I²C
never sets them. **This is itself a clue** (see Phase D): a Blueprint sketch has touched this module since
its last cold boot, but the production firmware has not re‑managed it.

> **Net:** RED + leftover channel LEDs is the picture of *"a Blueprint tool managed this module earlier, then
> production firmware took over with raw I²C and left it unmanaged."* That is the suspect.

---

## 2. Why this de‑prioritizes the v2.0.46 contention work

- v2.0.46 hardened *sequencing and timing on a shared bus.* If the module is unmanaged / not in a valid read
  state, **no amount of sequencing discipline produces a valid reading** — exactly what we observed.
- The original symptom (`CODE_REVIEW_06152026...`) was a **stable, repeatable, timing‑immune** wrong value.
  Contention produces **intermittent** corruption with **intermittent successes**; we never saw successes.
- Our memory/methodology rule: *when a fix for hypothesis X fails, that failure pre‑falsifies X — pivot
  rather than tune X harder.* We are pivoting from "contention" to "**is the module even managed / addressed /
  in current‑ADC mode the way the official path requires?**"

**Do not** ship more contention tuning until the §0 decisive test says the bus is actually the constraint.

---

## 3. Pre‑flight — get a *reliable* USB serial link first

We hit this exact wall earlier today (see repo memory 2026‑06‑24): after a DFU upload the Opta
**re‑enumerates COM ports** (e.g. COM4→COM5) and a raw serial open can fail with *"A device attached to the
system is not functioning" / "Invalid serial port"*. Treat serial‑transport health as **its own gate** before
trusting any on‑device result.

- [ ] **P0.1 — Identify the port.** `arduino-cli board list` → note the COM port reported as Opta. Re‑run
  after every upload (the port number changes on re‑enumeration).
- [ ] **P0.2 — Prove the CDC link is alive *before* drawing conclusions.** Open the monitor
  (`arduino-cli monitor -p COMx -c baudrate=115200`) and confirm you see the firmware banner. If the port
  opens but is silent or errors, **unplug/replug the USB**, re‑enumerate, and retry **before** concluding
  anything about the Notecard/A0602.
- [ ] **P0.3 — Note power.** The A0602 needs its **external field supply (12–24 V)** to power the analog
  front‑end *and the loop*. Confirm the test rig has it; an unpowered expansion will mislead every test below.
  (Observation only — no meter needed; just confirm the supply is connected and the module’s power LED is on.)

---

## 4. The diagnostic TODO list (USB‑only, ordered by diagnostic power)

### Phase A — Capture the *production* device's own story (no reflash)

The shipped v2.0.46 firmware already prints most of what we need. Just read it.

- [ ] **A1 — Capture a clean boot log** of the current v2.0.46 client over USB. Look specifically for:
  - the **I²C bus scan** block — is `A0602 Current Loop` reported **OK** or **NOT FOUND**, and **at which
    address**?
  - the line **`I2C: current loop address override 0x64 -> 0x0A`** — if present, the module is answering at
    the **unmanaged default 0x0A**, confirming §1. If absent and the scan shows OK at 0x64, it answers at 0x64.
  - `MCUboot: sketch confirmed`, firmware version `2.0.46`, build seq `240` (confirms what's actually running).
- [ ] **A2 — Capture a current‑loop sample cycle at runtime.** Let it run through a monitor read and look for:
  - `WARNING: Failed to enable sensor power gating on P1 via I2C` (PWM enable NACK), and/or
  - the framed‑read / channel‑function‑confirm diagnostics, and the `cl_fault` reason code.
  - whether a real `ma` ever appears or it stays at the stuck value.
- [ ] **A3 — Record the verdict:** does production see the A0602 at **0x64**, at **0x0A (override)**, or
  **not at all**? This one line drives everything else.

### Phase B — The decisive falsification: managed vs unmanaged

> **One‑flash tool (recommended): `TankAlarm-112025-A0602_Diagnostic.ino`.** This sketch runs the whole
> discriminator automatically on boot — **RAW (production protocol) before managed → MANAGED bootstrap
> (drives the LED toward GREEN, enumerates, reads `pinCurrent`) → RAW after managed**, at **0x64, 0x0A, and
> the assigned address**, dumping **raw bytes + CRC** for every transaction and printing a heuristic
> **Branch ①/②/③ verdict**. It covers Phase B *and* Phase C2/C3 in a single upload and never touches the
> Notecard. Flash it first; fall back to the two sketches below only if you want to isolate one path.

`TankAlarm-112025-Sensor_Utility.ino` uses the **official `OptaController`** path on its own. Flashing it will
run the Blueprint bootstrap and should drive a healthy module to **GREEN**.

- [ ] **B1 — Flash `Sensor_Utility`** (`arduino:mbed_opta:opta`, plain — *not* the `security=sien` production
  build). Re‑enumerate the port (P0.1) after upload.
- [ ] **B2 — Watch the A0602 status LED during/after boot.** **Does it turn GREEN?**
  - GREEN ⇒ the module **can** be managed and addressed → hardware/bus/power are OK; suspicion lands hard on
    the **production raw path**.
  - Stays RED/BLUE ⇒ the controller **cannot** assign it → bus/power/module problem.
- [ ] **B3 — `s` (scan expansions).** Expect `Detected expansions: 1` with an A0602 **type** and an **I²C
    address**. Record the address it reports (this is the *managed* address, distinct from 0x64).
- [ ] **B4 — `c` then `a` (configure current ADC, read all channels).** On the wellhead channel (CH0), do you
    get **≈4 mA** at ~0 psi (and sane values on others)? Use `d` for **raw ADC + mA** on the focus channel.
- [ ] **B5 — Verdict:**
  - **GREEN + enumerated + reads ≈4 mA** ⇒ **production raw/unmanaged path is the bug.** (→ §5 Branch ①.)
  - **GREEN + enumerated + READ FAIL / wrong mA** ⇒ module managed but front‑end/loop/channel issue (→ Branch ②).
  - **Not GREEN / not enumerated** ⇒ Blueprint bootstrap / bus / power problem (→ Branch ③).

### Phase C — Address & protocol disambiguation

- [ ] **C1 — Full bus scan.** Flash `TankAlarm-112025-I2C_Utility.ino` and press **`s`**. Its scan reports
    every ACK 0x08–0x77 (`UNEXPECTED device` for anything beyond the Notecard). **Record exactly which
    addresses ACK:** is anything at **0x64**? at **0x0A**? Is the Notecard at **0x17**? This nails the address
    question that A1/B3 hinted at.
- [ ] **C2 — Replicate the *production* transaction at the real address** — **already built** as
    `TankAlarm-112025-A0602_Diagnostic.ino`. It performs the **exact production sequence**
    (`tankalarm_setPwm(P1 on)` → `tankalarm_configureCurrentAdcChannel()` →
    `tankalarm_getAnalogChannelFunction()` → `tankalarm_readCurrentAdcFramed()`) **at 0x64, 0x0A, and the
    assigned address**, printing **raw bytes + CRC validity** so you can see whether the framed protocol works
    against the module **in its current (unmanaged) state** and at which address. Press `1` to re‑run just the
    RAW path on demand.
- [ ] **C3 — Raw‑before‑managed ordering test** — **automated** in the same sketch: PHASE 1 runs the raw read
    on a freshly power‑cycled module **before** the PHASE 2 managed bootstrap, and PHASE 3 repeats it after. If
    the raw read only succeeds **after** the managed session configured the channels, that proves production is
    relying on a state it never establishes itself.

### Phase D — LED state confirmation (cheap, do alongside the above)

- [ ] **D1 — Cold‑boot LED under production firmware.** Power‑cycle the A0602 with the **v2.0.46 production**
    firmware running and **no Blueprint sketch run since**. Expected per §1: status LED **RED/BLUE**, and the
    **2 yellow channel LEDs should be OFF** (production never sets them). If the 2 yellow LEDs are **still on
    after a true cold boot**, that contradicts "managed‑session residue" and needs explanation.
- [ ] **D2 — Managed‑session LED.** After `Sensor_Utility` (Phase B), confirm the status LED is **GREEN** and
    note which channel LEDs it lights. This documents the healthy reference state for the field tech.
- [ ] **D3 — Transition test.** Flash production again after B; confirm the LED **drops back from GREEN to
    RED** (production stops managing the module). That round‑trip directly demonstrates the architecture gap.

### Phase E — Notecard coexistence sanity (rule out a v2.0.46 regression)

v2.0.46 ships a **per‑op 400 kHz** window around A0602 reads. The Notecard was previously found unreliable at
400 kHz on this port. Confirm we didn't *introduce* a Notecard problem.

- [ ] **E1 — Notecard health over USB.** In `I2C_Utility`: `a`/`d` (attach + `card.version`, `hub.get`,
    `card.wireless`) and `y` (`hub.sync`). Confirm the Notecard answers cleanly. If the Notecard is now flaky,
    the scoped 400 kHz restore may be leaking — but that is a *secondary* issue to the A0602 management
    question above.

---

## 5. Decision tree — outcome → root cause → fix direction

```
Run Sensor_Utility (OptaController) over USB  ──►  A0602 status LED?
   │
   ├─ GREEN, enumerates, reads ≈4 mA  ──►  BRANCH ① : production RAW/UNMANAGED path is the bug
   │       • Hardware/bus/power/module all proven good by the managed read.
   │       • Fix direction: make production MANAGE the module —
   │         adopt OptaController/AnalogExpansion (beginChannelAsCurrentAdc + pinCurrent),
   │         OR run the Blueprint bootstrap so the module is addressed/GREEN before any raw read.
   │       • Explains why v2.0.46 contention tuning did nothing.
   │
   ├─ GREEN, enumerates, READ FAIL / wrong mA  ──►  BRANCH ② : managed but loop/front-end/channel
   │       • Module is reachable; the analog input or loop power (P1 gate) isn't delivering current.
   │       • USB-testable: try other channels, toggle P1 via utility, verify ext-power channel function,
   │         confirm field supply present. Remainder may be wiring/sensor (limited without a meter).
   │
   └─ NOT GREEN / not enumerated  ──►  BRANCH ③ : Blueprint bootstrap / AUX bus / module power
           • Controller can't even assign an address.
           • USB-testable: full bus scan (does anything ACK at 0x0A/0x64?), power-cycle behavior,
             try the module with nothing else on the bus. Likely hardware/power (beyond pure firmware).
```

**Most likely, based on the evidence so far:** Branch ① (or ③ if the bus/power is also marginal). The RED LED,
the raw‑I²C‑only production path, the `0x0A` fallback, and the contention‑fix no‑op all point at
*"the module was never managed."*

---

## 6. Command / tool quick reference

| Task | Command / keys |
|---|---|
| Find port (re‑run after each upload) | `arduino-cli board list` |
| Monitor serial | `arduino-cli monitor -p COMx -c baudrate=115200` |
| Build a diagnostic utility | `arduino-cli compile --fqbn arduino:mbed_opta:opta <SketchDir>` |
| Upload | `arduino-cli upload -p COMx --fqbn arduino:mbed_opta:opta <SketchDir>` |
| **`A0602_Diagnostic` keys** | runs full PHASE 1/2/3 on boot · `r` re‑run · `1` RAW only · `2` MANAGED only · `s` bus scan · `g` toggle P1 gate |
| `Sensor_Utility` keys | `s` scan expansions · `c` config current ADC · `a` read all · `d` raw ADC+mA · `f` focus ch |
| `I2C_Utility` keys | `s` full bus scan (shows UNEXPECTED) · `a` attach Notecard · `d` Notecard diag · `y` hub.sync |

> The diagnostic utilities build with the **plain** `arduino:mbed_opta:opta` FQBN. Only the **production
> client** needs `security=sien` + `-DTANKALARM_DFU_MCUBOOT`. Don't mix them up when reflashing the test unit.

---

## 7. Constraints honored / deliberately excluded

- **No electrical test gear:** every test above is **on‑device or over USB**. We do *not* require a scope,
  meter, or logic analyzer. (The few hardware facts we need — field supply present, module power LED on — are
  visual only.)
- **Branch ②/③ have a floor:** if the official managed path also can't read the loop, the residual is
  electrical (loop supply, wiring, the transmitter) and **cannot be fully closed over USB**. We will have
  *characterized* it precisely, which is the most USB can do.
- **No firmware changed by this plan.** The only optional code is the temporary **C2** replication sketch, and
  only if Phase B points at the production path.

---

## 8. Consolidated checklist

- [ ] **P0** Reliable USB link (port id, banner seen, replug if "not functioning"); field supply confirmed.
- [ ] **A1–A3** Production boot + runtime log: A0602 OK/NOT‑FOUND, `0x64 -> 0x0A` override?, `cl_fault`, stuck `ma`.
- [ ] **B1–B5** Flash `Sensor_Utility`: LED→GREEN? enumerates? reads ≈4 mA? → pick Branch ①/②/③.
- [ ] **C1** Full bus scan: which addresses ACK (0x64 / 0x0A / 0x17)?
- [ ] **C2–C3** (if Branch ①) replicate production raw transaction at the real address; raw‑before‑managed test.
- [ ] **D1–D3** LED cold‑boot vs managed vs transition (RED↔GREEN round trip).
- [ ] **E1** Notecard still healthy under v2.0.46 (no 400 kHz regression).
- [ ] **Decide fix direction** from §5 and write it up before touching firmware.

---

*This is a diagnosis plan, not a fix. Start with the §0 decisive test (Phase B). The strong prior is that the
A0602 has been running **unmanaged** (RED) and the production raw‑I²C path — not bus contention — is the real
fault, which is exactly why the v2.0.46 contention work had no effect.*
