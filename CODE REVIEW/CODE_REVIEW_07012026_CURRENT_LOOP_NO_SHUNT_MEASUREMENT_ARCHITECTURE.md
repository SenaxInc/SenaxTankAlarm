# Current-Loop Measurement Architecture — Internally-Powered DAC Loop Without a Shunt Resistor

**Date:** 2026-07-01
**Status:** As-built description of the shipping implementation (firmware v2.1.2)
**Applies to:** TankAlarm-112025 Client + `TankAlarm_I2C.h` (Common), Arduino Opta + Opta Ext A0602 (AFX00007)
**Related:** [release-notes/v2.1.0.md](../release-notes/v2.1.0.md) (feature),
[release-notes/v2.1.1.md](../release-notes/v2.1.1.md) (apply-settle timing),
[release-notes/v2.1.2.md](../release-notes/v2.1.2.md) (bipolar mA scale),
[CODE_REVIEW_07012026_DAC_LOOP_POWER_INTEGRATION_PLAN.md](CODE_REVIEW_07012026_DAC_LOOP_POWER_INTEGRATION_PLAN.md)

---

## 1. Executive Summary

Since v2.1.0 the TankAlarm client powers and measures each 2-wire 4-20mA
transmitter from a **single A0602 channel terminal pair** (e.g. `I1+`/`I1-`):
the channel's DAC drives the loop at 11V while a current ADC — overlaid on the
same channel — measures the loop current. There is **no external shunt
resistor, no separate power channel, and no gating transistor** in the path.

This works because the Opta Analog expansion is not a traditional PLC analog
input. Its front-end is an Analog Devices **AD74412R** software-configurable
I/O (SWIO) chip, which measures loop current across a small internal precision
sense resistor instead of the classic 250Ω shunt, leaving almost all of the
11V supply available to the transmitter at tank-level operating currents.

Bench validation (Dwyer 626, 0-50 psi, ~1.5 psi water head): steady
**4.516mA** measured by the client, matching the theoretical 4.48mA and the
managed-library reference utility.

---

## 2. The Classic PLC Input vs. the Opta A0602

### 2.1 Traditional method: the 250Ω shunt

Almost every conventional PLC/RTU analog input converts 4-20mA to a voltage
with a **250Ω shunt resistor** (external or internal), producing the familiar
1-5V signal for a voltage ADC:

| Loop current | Voltage burned by a 250Ω shunt |
|---|---|
| 4mA | 1.0V |
| 20mA | **5.0V** |

That 5V burden is why traditional loops are engineered around 24V supplies —
after the shunt, wiring, and the transmitter's own minimum lift-off voltage,
a 11-12V supply would be starved at full scale.

### 2.2 The Opta's method: direct current measurement, no shunt

The A0602 never converts the loop to a 1-5V signal. The AD74412R routes the
loop current through a small internal precision sense resistor and digitizes
the drop directly with an on-chip 16-bit ADC:

- The ADC input mux is set to the internal sense resistor node —
  `CFG_ADC_INPUT_NODE_100OHM_R` (expansion firmware `OptaAnalog.cpp`, e.g.
  lines 710/1071/1167 of Arduino_Opta_Blueprint).
- The sense resistor is **100Ω** (AD74412R internal `RSENSE`). This is the
  key architectural number: it is verified three independent ways in §4.3.
- Burden voltage at 20mA: `0.020A × 100Ω = 2.0V` — **2.5× lighter than a
  250Ω shunt** (5.0V), though not the near-zero burden of a milliohm-class
  ammeter. There is no 250Ω anywhere in the path.

> **Correction to earlier working notes:** a draft analysis assumed a 1-10Ω
> current-ADC input (≈0.2V burden, ≈10.8V at the sensor at 20mA). The
> expansion firmware source and the ADC scale math both prove the sense
> element is 100Ω, so the true full-scale burden is 2.0V, not 0.2V. The
> qualitative conclusion ("no 250Ω shunt → far more headroom than a classic
> PLC input") stands; the quantitative margin at 20mA does not (§5).

---

## 3. Why the Loop Supply Is 11V (and cannot be 12V or 13V)

The 11V is **not a firmware choice** — it is the AD74412R's voltage-output
hardware ceiling:

- The voltage-output DAC is 13-bit: codes **0-8191** map to **0-11.0V**.
  The managed library computes `volts = code × 11.0 / 8191`
  (`AnalogExpansion::pinVoltage()`), and `setDac()` clamps any code above
  8191. Our raw frame sends code `0x1FFF` (8191) — already the maximum.
- The output stage can only drive so close to the module's internal supply
  rail; 11V is the specified full-scale output of the chip's voltage
  function. No register value produces 12V or 13V.
- Firmware location: `tankalarm_configureDacLoopPowered()` in
  [TankAlarm-112025-Common/src/TankAlarm_I2C.h](../TankAlarm-112025-Common/src/TankAlarm_I2C.h)
  (`SET_DAC` step, raw 8191 = 11.0V, `limit_current = DISABLE` so the
  channel may source up to 25mA).

If a specific installation genuinely needs a 12-13V (battery float) or 24V
loop supply, the supported path is the **"Sensor Loop Power: Disabled
(Externally powered)"** config option — see §6.

---

## 4. The Special Mode: Power and Measure on the Same Terminal Pair

### 4.1 What the firmware configures

`tankalarm_configureDacLoopPowered(channel, i2cAddr)` sends three framed
Blueprint commands (each CRC-validated and ACK-checked, with a 300ms
apply-settle between them — see §4.4):

1. `SET CH_DAC` — channel becomes a **voltage DAC**, current limit disabled.
2. `SET DAC` — output driven to **11.0V** (code 8191).
3. `SET CH_ADC` with `adding_adc = ENABLE` — a **current ADC is overlaid**
   on the same channel without disturbing the DAC drive.

On the expansion side this triggers a dedicated special case
(`parse_setup_adc_channel()`, Arduino_Opta_Blueprint `OptaAnalog.cpp`): when a
current ADC is *added* to a channel already configured as a voltage DAC, the
firmware applies `CH_FUNC_CURRENT_INPUT_LOOP_POWER`, muxes the ADC to the
100Ω sense node, and — critically — selects the **bipolar ±2.5V ADC range**
(`CFG_ADC_RANGE_2_5V_BI`), unlike the unipolar `2_5V_RTD` range used by a
plain externally-powered current input.

### 4.2 The two ADC scales (root cause of the v2.1.2 hotfix)

| Channel mode | ADC range across 100Ω | raw (0-65535) → mA | zero current | sourced current |
|---|---|---|---|---|
| Plain current ADC (externally powered) | unipolar 0-2.5V | `25·raw/65535` | raw = 0 | n/a (sunk) |
| DAC + added ADC (internally powered) | **bipolar ±2.5V** | `(raw/65535)·50 − 25` | raw ≈ 0x8000 (mid-scale) | reads **negative** |

The client uses `tankalarm_readLoopPoweredCurrentAdcFramed()` (bipolar,
returns magnitude, rejects the 0x0000/0xFFFF rails) when loop power is
enabled, and the original unipolar `tankalarm_readCurrentAdcFramed()` when
the transmitter is externally powered. Applying the unipolar formula to the
bipolar mode was the v2.1.1 bug that transmitted a real 4.51mA as 10.245mA.

### 4.3 Why we are confident RSENSE = 100Ω

1. **Source code:** the expansion firmware muxes the ADC to
   `CFG_ADC_INPUT_NODE_100OHM_R` in every current-measurement mode.
2. **Scale math:** the library's conversions only produce correct milliamps
   if the measured element is 100Ω — `±2.5V ÷ 100Ω = ±25mA` (bipolar) and
   `2.5V ÷ 100Ω = 25mA` (unipolar) exactly match `pinCurrent()`'s formulas.
3. **Bench cross-check:** the same raw sample (26857) converts to 4.51mA on
   the bipolar/100Ω model — agreeing with the physical head-pressure
   calculation (~1.5 psi on 0-50 psi ⇒ 4.48mA) and the transmitter's
   zero-button calibration behavior. A 10Ω model cannot reproduce these
   numbers on any of the three formulas.

### 4.4 Timing law (v2.1.1)

The expansion **applies** each `SET` in its own main loop; the I2C ACK only
means *queued*. The three config frames are therefore spaced by
`TANKALARM_DAC_APPLY_SETTLE_MS` (300ms). Sent back-to-back, the 11V value is
processed before the channel has become a DAC and is silently dropped — the
loop then drives 0V and every read returns a valid frame of `raw = 0`.
Proven by A/B test on the same hardware
(`TankAlarm-112025-Sensor_Utility/A0602_DAC_RawFrame_Diag`).

---

## 5. Loop Voltage Budget (honest numbers)

Voltage available at the transmitter = `11.0V − I × (100Ω + R_wiring)`:

| Loop current | Burden (100Ω) | At transmitter (short wires) | Dwyer 626 spec (10-35 VDC) |
|---|---|---|---|
| 4.0mA (live zero) | 0.40V | 10.60V | OK ✓ |
| 4.5mA (present bench reading) | 0.45V | 10.55V | OK ✓ — **bench-proven** |
| 12mA (mid scale) | 1.20V | 9.80V | marginal ⚠ |
| 20mA (full scale) | 2.00V | **9.00V** | below spec floor ⚠ |

Interpretation for this product:

- **Tank-level service (this application):** a 0-50 psi transmitter watching
  a few feet of head lives in the 4-6mA region for its whole life. Margin is
  ~0.5V or better, and the method is bench-proven at the operating point.
- **Near full scale:** the transmitter would see less than its published
  10V minimum. Many transmitters still function below their spec floor, but
  it must not be relied upon.
- **Long cable runs** add `I × R_wire` on top of the 2V worst case and eat
  the remaining margin first.

**Deployment guidance:** for installations that will genuinely operate near
20mA, or with very long home runs, configure the sensor as **externally
powered** (§6) from the 12-13V battery-float rail (≥11V at the transmitter
even at 20mA) or a dedicated 24V supply — the industrial gold standard.

---

## 6. The Two Supported Wiring Modes (v2.1.x)

| | Internally powered (default) | Externally powered |
|---|---|---|
| Server UI | Sensor Loop Power: **Enabled (Powered from channel terminals)** | **Disabled (Externally powered)** |
| Wiring | transmitter + → `In+`, transmitter − → `In-` (2 wires total) | supply + → transmitter + ; transmitter − → `In+`; supply − → module GND |
| Loop supply | 11V from channel DAC | battery float 12-13V / 24V PSU |
| Power duty cycle | powered only during sampling bursts; channel returns to high-impedance between bursts | continuous |
| Pilot LED | channel LED lit while loop is powered | none |
| ADC scale | bipolar ±25mA (§4.2) | unipolar 0-25mA |
| Best for | short runs, tank-level currents, lowest power, simplest wiring | sustained high-scale currents, long runs |

Config keys: `loopPowerEnabled`, `loopPowerWarmup`, `loopPowerSampleDelay`
(legacy `pwmGating*` keys still parse; the retired `pwmGatingChannel` is
ignored — the P1-P4 transistor gating was removed in v2.1.0 after the P1
MOSFET failure investigation).

---

## 7. Sampling Pipeline (as shipped in v2.1.2)

Per sampling duty cycle in `readCurrentLoopSensor()`:

1. Configure DAC loop power (3 framed commands, 300ms settles, 3 attempts)
   — or, in externally-powered mode, re-apply the plain current-ADC config.
2. Pilot LED on (`ARG_OA_SET_LED`, bitmask of the channel).
3. Warmup (`loopPowerWarmup`, default 3000ms) chunked with watchdog kicks.
4. 5 framed `GET_ADC` samples, `loopPowerSampleDelay` (default 300ms) apart;
   each validated for header, CRC, channel echo, and rail values; converted
   on the mode's scale; averaged.
5. Serial diagnostic line: `CL ch0: 4.516 mA (n=5/5, loopPower=1)`.
6. Live-zero (<4mA ⇒ `under_range`) and over-range (>21mA) guards; faults
   reported in telemetry (`fault`, `ma_raw`), good reads as `ma` with `pg`
   = loop-power result.
7. Channel returned to high-impedance, LED off.

---

## 8. Bench Evidence Trail

| Observation | Value | Interpretation |
|---|---|---|
| Managed utility, open loop | 0.000mA | mid-scale bipolar raw ⇒ no loop current — a true fault, not fabricated |
| Managed utility after transmitter zero-button calibration | 4.004mA | healthy live zero |
| Managed utility with ~1.5 psi water head | 4.509-4.511mA | matches 4.48mA theory (0-50 psi span) |
| Raw-frame diag, same condition | raw 26857 | bipolar ⇒ −4.511mA (magnitude 4.511) ✓; unipolar ⇒ 10.245mA ✗ |
| v2.1.1 Notehub telemetry | `"ma":10.25` | the unipolar mis-scale escaping to production |
| v2.1.2 client (CI-signed) | `CL ch0: 4.516 mA (n=5/5)` steady | fix confirmed end-to-end |

---

## 9. References

- `TankAlarm-112025-Common/src/TankAlarm_I2C.h` —
  `tankalarm_configureDacLoopPowered()`, `tankalarm_disableDacLoopPowered()`,
  `tankalarm_readLoopPoweredCurrentAdcFramed()`, `tankalarm_readCurrentAdcFramed()`,
  `TANKALARM_DAC_APPLY_SETTLE_MS`.
- Arduino_Opta_Blueprint `OptaAnalog.cpp` — `parse_setup_adc_channel()`
  add-ADC special case; `CFG_ADC_INPUT_NODE_100OHM_R`; `CFG_ADC_RANGE_2_5V_BI`
  vs `CFG_ADC_RANGE_2_5V_RTD`.
- Arduino_Opta_Blueprint `AnalogExpansion.cpp` — `pinCurrent()` per-mode
  conversion branches; `pinVoltage()` 11.0V/8191 DAC scale; `setDac()` clamp.
- Analog Devices AD74412R datasheet — SWIO channel functions, 0-11V voltage
  output range, internal 100Ω current-sense resistor.
- `TankAlarm-112025-Sensor_Utility/A0602_DAC_Loop_Powered_Test` (managed
  reference / calibration utility) and `A0602_DAC_RawFrame_Diag` (raw-frame
  forensics, settle A/B).
