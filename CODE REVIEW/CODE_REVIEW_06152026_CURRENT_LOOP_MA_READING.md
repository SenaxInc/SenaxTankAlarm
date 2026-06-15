# Code Review — Incorrect 4‑20 mA Current‑Loop Reading (Cox Wellhead Gas Sensor)

**Date:** 2026‑06‑15
**Author:** AI code review (for further human review)
**Firmware at time of writing:** Client v1.9.20 (live device `dev:860322068056545`), Server v1.9.21 (built; pending power‑cycle)
**Affected sensor:** Site "Silas" → "Cox Wellhead" → gas pressure, 4‑20 mA current loop on A0602 channel 0 (P1 gated)
**Status:** UNRESOLVED — root cause not yet confirmed. This document collects evidence and ranks hypotheses; it does **not** make a code change.

---

## 1. Executive Summary

The dashboard reports **43.8 psi** for the Cox Wellhead gas sensor when the true pressure should be near **0 psi**. The dashboard value is a faithful decode of a raw current reading of **~18.02 mA**, when a healthy 4‑20 mA transmitter at zero pressure should read **~4 mA**.

Two firmware changes already shipped specifically to fix this (v1.9.11 gas loop‑fault gate, v1.9.19 read‑cadence/settle fix) and **neither changed the reading**. The value is also **suspiciously stable** (18.02 mA across reads taken 42 h apart, and again after the v1.9.20 update). A stable, repeatable, physically‑implausible value that is immune to timing changes is the signature of a **stale or default register value**, not a noisy live measurement.

The single most important code finding in this review:

> **The production firmware reads current from the A0602 using a custom raw‑I²C shortcut (`write 1 channel byte → read 2 bytes`) and never configures the A0602 input channel into 4‑20 mA current‑ADC mode.** The validated diagnostic sketch (`Blueprint_CH0_Test`) instead uses the **official Arduino_Opta_Blueprint API**, which *does* configure each channel (`beginChannelAsCurrentAdc`) before reading.

This protocol/configuration mismatch is the **leading hypothesis** for why production reads a fixed ~18 mA regardless of the sensor, while bench tests with the official API have read correctly. It is directly and cheaply testable (Section 9).

---

## 2. Precise Symptom

| Quantity | Observed | Expected (≈0 psi) |
|---|---|---|
| Raw current | **18.02 mA** | ~4.0 mA |
| Decoded pressure (0–50 psi range) | **43.8 psi** | ~0 psi |
| Stability | Constant 18.02 mA across many reads, days apart, across firmware versions | Small noise around 4 mA |
| Reaction to timing fix (v1.9.19) | **No change** | n/a |
| Reaction to gas loop‑fault gate (v1.9.11) | Not triggered (18 mA is in‑range, so no fault raised) | n/a |

**Decode math (confirms the dashboard is not the problem):**

```
mA   = 4.0 + (raw / 65535) * 16.0
psi  = (mA - 4.0) * (rangeMax - rangeMin) / 16.0      // rangeMin=0, rangeMax=50

18.02 mA → raw ≈ (18.02 - 4.0)/16 * 65535 ≈ 57,424 (0xE050)
18.02 mA → (18.02 - 4.0) * 50/16 = 43.8 psi   ✓ matches dashboard exactly
```

The server, decode formula, and dashboard are all **correct**. The error is upstream of the formula — at the mA acquisition.

---

## 3. The mA Signal Path (end to end)

```
[Dwyer-style 4-20mA gas transmitter]
        │  (2-wire current loop, needs loop supply ~12-24V)
        ▼
[A0602 P1 high-side switch] ── gated ON by tankalarm_setPwm(0,10000,9999,0x64)
        │  (loop power applied only during a read, then OFF for low power)
        ▼
[A0602 analog input CH0] ── senses loop current, 16-bit ADC
        │  raw I2C: Wire.write(channel) → Wire.requestFrom(0x64, 2) → 2 bytes
        ▼
[tankalarm_readCurrentLoopMilliamps()]  mA = 4 + raw/65535*16
        ▼
[readCurrentLoopSensor()]  averages 4 samples, applies live-zero fault gate,
                           maps mA → psi via sensor range
        ▼
[buildSensorObject()]  emits "ma" and "lvl" in telemetry.qo
        ▼
[Server handleTelemetry]  stores reading
        ▼
[Dashboard]  43.8 psi, "Gas Pressure / Cox Wellhead"
```

Every stage from the formula rightward is verified correct. The fault is at the **A0602 CH0 → I²C read** stage.

---

## 4. Hardware Context

- **Controller:** Arduino Opta Lite (STM32H747XI).
- **Analog expansion:** Arduino Opta Ext **A0602** (AFX00007), 4‑20 mA capable, on the Opta **AUX/expansion I²C bus**, default address **0x64**.
- **Cellular:** Blues "Wireless for Opta" Notecard on the **same I²C bus** at address **0x17**.
- **Sensor:** 2‑wire 4‑20 mA gas‑pressure transmitter, 0–50 psi range, on **CH0**.
- **Power gating:** A0602 terminal **P1** used as a high‑side switch to power the transmitter only during a read (low‑power duty cycling). P1 = PWM/output channel 0; CH0 = analog input channel 0. (Both index 0 but are different subsystems on the module.)
- **Site power:** solar + SunSaver **MPPT over RS‑485 (Modbus)** — i.e., there is **also** Modbus serial traffic in production.

**Bus‑contention note:** In production, the I²C bus is shared by the **Notecard (0x17)** and the **A0602 (0x64)**, and the MCU is concurrently servicing **Modbus RS‑485** and **Ethernet/cellular**. The validated bench sketches run essentially standalone. This is the biggest *environmental* difference between "works on the bench" and "wrong in production" (see H3).

---

## 5. The Two I²C Protocols In Use (the smoking gun)

The production firmware talks to the A0602 **two different ways**, and a third way exists in the validated test:

### 5a. Output gating — `tankalarm_setPwm()` → uses a proper Blueprint command frame
```c
buf[0]=0x01;  // BP_CMD_SET
buf[1]=0x13;  // ARG_OA_SET_PWM
buf[2]=0x09;  // payload length
buf[3]=ch; ... period(4) ... pulse(4) ...
buf[12]=CRC8(poly 0x07);          // proper CRC
Wire.beginTransmission(0x64); Wire.write(buf,13); Wire.endTransmission();
```
This is a **structured, CRC‑protected command**. It demonstrably **works** (the transistor switches; gating is confirmed). That also proves the A0602 is running standard Blueprint firmware that understands command frames.

### 5b. Input current read — `tankalarm_readCurrentLoopMilliamps()` → raw shortcut, no frame, no CRC, no channel config
```c
Wire.beginTransmission(0x64);
Wire.write((uint8_t)channel);     // a single bare byte — NOT a BP_CMD_GET frame
Wire.endTransmission();
delay(1);
Wire.requestFrom(0x64, 2);        // read 2 bytes
raw = (read<<8) | read;
return 4.0 + raw/65535.0*16.0;
```
This is **not** a Blueprint GET command frame. It writes a single channel index and reads two bytes. There is **no CRC check** on the response and, critically, **no prior command that configures CH0 as a 4‑20 mA current ADC.**

### 5c. Validated bench read — `Blueprint_CH0_Test.ino` → official API (configures the channel!)
```c
OptaController.begin();
for (ch=0..7) AnalogExpansion::beginChannelAsCurrentAdc(OptaController, 0, ch); // CONFIG
...
exp.updateAnalogInputs();
float ma = exp.pinCurrent(ch, false);   // official, framed, CRC-checked
```
This is the **official Arduino_Opta_Blueprint** path. It explicitly puts each channel into current‑ADC mode before reading.

> **Key inconsistency:** Production uses 5b. The known‑good bench path is 5c. They are **different protocols**, and only 5c configures the input channel. The production firmware contains **no** reference to `OptaController`, `AnalogExpansion`, `beginChannelAsCurrentAdc`, or `updateAnalogInputs` (verified by search across client + Common).

---

## 6. What Has Worked (history)

- **Power gating works.** `tankalarm_setPwm()` reliably switches P1 (confirmed by the LED and by the user's recollection that "the sensor and new power gating were working in a previous test"). The output‑side command frame is correct.
- **The decode/formula is correct and unchanged** since commit `93a7338`. `4 + raw/65535*16` and the psi mapping reproduce 43.8 psi exactly from 18.02 mA.
- **The Blueprint API path has read plausibly** in diagnostics (`Blueprint_CH0_Test`, official `pinCurrent`). The P1 sketch header documents an expected "1.61 psi (4.516 mA)".
- **Server‑side handling is correct.** Live‑zero fault gating (v1.9.11) correctly rejects <3.6 mA; telemetry/decoding/labels are right.
- **Fault philosophy is sound.** When acquisition fails entirely, the code returns `NAN` and withholds telemetry rather than reporting a fake 0.

## 7. What Has NOT Worked (history)

- **v1.9.11 gas loop‑fault gate** — removed the gas exemption so <3.6 mA faults. *No effect here* because 18 mA is **in range**, not under‑range. (Correct fix for a different failure mode.)
- **v1.9.19 read‑cadence/settle fix** — added a priming read and floored inter‑sample settle to 300 ms (later the device config used 5000 ms warmup + 5000 ms sample delay, i.e., **very** generous). *No effect.* A stale register value does not change with more settle time. **This effectively rules out "ADC needs more time" as the cause.**
- **Re‑pushing config / reboots** — config is applied (`lastAckStatus: applied`), gating enabled, range correct. *No effect.*
- **Updating the client (eventually to v1.9.20)** — confirmed running, fresh telemetry, *still 18.02 mA.*

The pattern across all of these: **anything that changes timing, faulting, or server handling has zero effect on the 18.02 mA value.** That points away from timing/conversion and toward the *acquisition protocol* or the *physical loop*.

---

## 8. Device Config (ground truth, from `GET /api/client?uid=dev:860322068056545`)

```json
{ "monitorType":"gas", "sensor":"current", "loopChannel":0,
  "sensorRangeMin":0, "sensorRangeMax":50, "sensorRangeUnit":"PSI",
  "pwmGatingEnabled":true, "pwmGatingChannel":0,
  "pwmGatingWarmup":5000, "pwmGatingSampleDelay":5000,
  "stuckDetection":false }
"dispatch": { "lastAckStatus":"applied", "configVersion":"3DCDA81D" }
```
Gating **is** enabled on P1, channel 0, with generous warmup. Config was applied. Nothing in the config explains 18 mA. (Side note: `pwmGatingSampleDelay: 5000` makes each read ~25 s — almost certainly a typo for a small value; not the root cause, but worth correcting.)

---

## 9. Root‑Cause Hypotheses (ranked)

### H1 — A0602 input channel is never configured for 4‑20 mA; raw read returns a default/stale value ★ LEADING
**Why it fits:** Production never sends a channel‑configuration command (no `beginChannelAsCurrentAdc` equivalent, no Blueprint GET frame). Standard Blueprint firmware expects channels to be configured and read via framed GET commands. A bare `write channel / read 2 bytes` against an unconfigured channel can return a **fixed default / last‑buffer** value → constant ~18 mA. **Explains the stability, the immunity to the timing fix, and why the official‑API bench path reads correctly.**
**Against:** Requires that the raw protocol "worked before" actually came from the Blueprint sketch, not production. Plausible but unproven.
**Decisive test:** Run `Blueprint_CH0_Test` on the device with live wiring. If it reads ~4 mA while production reads ~18 mA → **confirmed**.

### H2 — Raw read returns a stale I²C output buffer (protocol shortcut invalid on this firmware)
Closely related to H1. Even if a channel is nominally configured, the `write 1 byte → read 2 bytes` shortcut may not be a valid read transaction for this A0602 firmware, so `requestFrom` returns whatever is latched in the module's output register (e.g., residue from the last `setPwm` ACK or a status word) — a stable wrong number.
**Decisive test:** same as H1; also, capture production serial — repeated identical raw bytes regardless of sensor disconnect strongly implies a stale buffer.

### H3 — I²C bus contention (Notecard 0x17 + A0602 0x64 + Modbus/Ethernet load) corrupts enable or read
The bench sketches run nearly standalone; production juggles Notecard, Modbus RS‑485, and Ethernet. Contention could cause the P1 enable to silently fail (then we read an unpowered loop) or corrupt the read.
**Why partly against:** an *unpowered* current loop should read **low/under‑range (≤4 mA)**, not 18 mA — unless the A0602 input floats high when open (topology‑dependent, unknown). Also, if the enable failed we would expect the serial warning `"Failed to enable sensor power gating on P1"`.
**Decisive test:** capture production serial during a read; look for the NACK / enable‑failure warnings and any I²C error counters.

### H4 — Transistor enable "succeeds" (ACK) but the loop is not actually powered in production
The I²C ACK only confirms the command was received, not that the rail came up. A power‑state/voltage difference (solar vs. USB bench) could mean P1 doesn't actually deliver loop voltage. But again this should read **low**, not high, unless the input floats high.
**Decisive test:** measure loop voltage/current at the transmitter with P1 commanded ON in production; or scope the P1 terminal.

### H5 — Genuine sensor/wiring fault (the transmitter really outputs ~18 mA)
A miswired, damaged, or over‑pressured transmitter could actually source ~18 mA. The user reports wiring unchanged since a working test, which argues against this, but a sensor can drift/fault over time.
**Decisive test:** `Blueprint_CH0_Test` (independent of our protocol). If the **official API also reads ~18 mA**, the problem is physical (sensor/wiring/loop), not firmware.

### H6 — ADC scale/format mismatch in the decode (`/65535` assumes 4‑20→0‑65535)
If the A0602 actually returns 0–24 mA (or 0–25 mA) full‑scale, or a left/right‑justified value, the `raw/65535*16+4` mapping would be wrong. This is **internally consistent** within our codebase (P1 test uses the same formula) so it can't alone explain a divergence from the Blueprint API — but it could compound H1/H2.
**Decisive test:** compare the raw `adc` value and `pinCurrent` from `Blueprint_CH0_Test` against the raw 2 bytes our shortcut returns for the same channel/sensor.

**Ranking rationale:** H1/H2 best explain *all* observations simultaneously — stability, immunity to the timing fix, and the bench‑vs‑production split — and are the cheapest to test. H3/H4 remain plausible environmental contributors. H5/H6 are less likely but must be excluded by the same single test.

---

## 10. Decisive Diagnostic Plan (cheap → expensive)

The client is currently USB‑accessible. One test discriminates most hypotheses:

1. **Run `Blueprint_CH0_Test` on the device, live wiring, serial @115200.** (Official API, configures the channel.)
   - Reads **~4 mA** → H1/H2 confirmed (our raw protocol/channel‑config is the bug). Fix = adopt the Blueprint API (Section 11).
   - Reads **~18 mA** → H5/H6 (physical sensor/wiring/loop or scale). Move to hardware checks.
2. **Capture production client serial during a sensor read.** Look for:
   - `WARNING: Failed to enable sensor power gating on P1 via I2C` → H3/H4 (enable failing).
   - `I2C NACK from 0x64` / `I2C short read` / `I2C buffer underrun` → bus/protocol issues.
   - Identical raw value with the sensor **disconnected** → stale buffer (H1/H2).
3. **Run `P1_Transistor_Gating_Test` (raw protocol) standalone.** If raw‑protocol standalone reads ~18 mA but Blueprint reads ~4 mA on the same wiring → protocol mismatch (H1/H2) nailed down independent of contention.
4. **Bench meter:** with P1 commanded ON, measure actual loop current at the transmitter. Ground truth for H4/H5.

---

## 11. Potential Fixes (mapped to hypotheses)

**If H1/H2 (protocol/channel‑config) — most likely:**
- **Adopt the official Arduino_Opta_Blueprint API for reads** (and ideally outputs): `OptaController` + `AnalogExpansion::beginChannelAsCurrentAdc()` at init, then `updateAnalogInputs()` + `pinCurrent()`. This is the configuration the validated bench sketch uses. Highest‑confidence fix.
- *Minimal alternative:* if we must keep the lightweight raw protocol, send the **proper Blueprint channel‑configuration command** (current‑ADC mode) at startup, and use the **framed GET command with CRC validation** for reads instead of the bare `write/read` shortcut. Validate the response CRC and reject stale/garbled frames.

**If H3 (bus contention):**
- Serialize A0602 transactions against Notecard/Modbus access (a single I²C mutex / quiet window); pause Notecard polling and Modbus during the gated read window; add explicit bus recovery and verify the existing retry/error counters actually fire.

**If H4 (enable not powering loop):**
- Treat a failed/again‑unverified P1 enable as a **sensor fault** (return `NAN`) rather than reading the channel anyway; add a post‑enable verification (e.g., expect a current rise) before trusting the read. Surface the gating‑enable result in telemetry for remote visibility.

**If H5 (hardware):**
- Field service: inspect wiring/polarity, loop supply voltage, burden resistor, and swap/zero the transmitter.

**If H6 (scale/format):**
- Re‑derive the raw→mA mapping from the Blueprint `adc` vs `pinCurrent` correspondence; correct the formula for the true full‑scale.

**Cross‑cutting, regardless of cause (recommended now):**
- **Add the gating‑enable result and raw mA into telemetry/diagnostics** so this is visible **remotely** (no serial cable needed) on the dashboard. This turns a multi‑trip field problem into a one‑glance status.
- **Return a sensor fault instead of a plausible‑but‑wrong 43.8 psi** when the loop can't be confirmed powered — an honest "sensor fault" is safer than a fabricated pressure for an alarming system.

---

## 12. Recommended Immediate Next Step

Run **`Blueprint_CH0_Test`** on the live device (it is USB‑accessible now) and capture the serial output, **then** capture the production client's serial during one read cycle. Those two captures will almost certainly collapse the hypothesis set to one branch:

- Blueprint ≈ 4 mA, production ≈ 18 mA → **firmware protocol/channel‑config (H1/H2)** → adopt the Blueprint API.
- Both ≈ 18 mA → **hardware/sensor (H5)** → field service.

No further speculative firmware change should be made until one of these captures is in hand — the last two timing‑based guesses (v1.9.11 path, v1.9.19) did not move the reading, and the evidence now points at the acquisition **protocol**, not timing.

---

## 13. Open Questions for Review

1. When the sensor read correctly "in a previous test," was that via `Blueprint_CH0_Test` (official API) or via production/`P1_Transistor_Gating_Test` (raw protocol)? This single fact would confirm or eliminate H1/H2 immediately.
2. Does the A0602 input float **high** when the loop is open, or read **low**? (Determines whether 18 mA can mean "unpowered.")
3. What loop supply voltage powers the transmitter through P1, and is it the same on the bench as in the field?
4. Is `pwmGatingSampleDelay: 5000` intentional (25 s reads) or a typo for ~300 ms?

---

*Prepared for collaborative review. No firmware was modified as part of producing this document.*

## 14. AI Assistant Additional Review

**Date:** 2026-06-15

I have reviewed the `tankalarm_readCurrentLoopMilliamps()` implementation in `TankAlarm_I2C.h`. The function indeed simply writes the channel index to I2C and reads 2 bytes:

```c
    Wire.beginTransmission(i2cAddr);
    Wire.write((uint8_t)channel); // No Blueprint command frame!
    uint8_t err = Wire.endTransmission();
    // ...
    if (Wire.requestFrom(i2cAddr, (uint8_t)2) != 2) { ... }
    uint16_t raw = ((uint16_t)Wire.read() << 8) | Wire.read();
    return 4.0f + (raw / 65535.0f) * 16.0f;
```

**Observations & Additions:**
1. **Lack of Initialization:** The analog input is never configured. The official Arduino_Opta_Blueprint library configures channels with a proper command frame (e.g. BP_CMD_SET, ARG_OA_SET_ADC_CONFIG).
A bare write without this configuration can lead to reading a default/diagnostic register (which explains the stable 18.02 mA).
2. **Immediate Fix:** If we want to continue using our lightweight protocol without bringing in the entire Arduino_Opta_Blueprint library, we need to mimic the BP_CMD_SET frames to properly configure the ADC mode in setup(), and use the BP_CMD_GET / ARG_OA_GET_ADC_DATA to get the raw data.
3. **Formula Validation:** The formula 4.0f + (raw / 65535.0f) * 16.0f is mathematically sound for a 16-bit DAC correctly scaled, but if the A0602 natively returns data inside a framed structure, raw bytes obtained using the shortcut might actually be framing headers or dummy bytes rather than ADC values.

**Conclusion:** Hypothesis 1 (H1) is practically verified by the source code. A software rewrite using the framed Blueprint commands or the official `pinCurrent()` API will immediately address the problem. Field debugging is still recommended just in case, but firmware update is necessary regardless.

---

## 15. Additional Review - Implementation Risks, Test Ordering, and Fix Details

**Date:** 2026-06-15

I reviewed the current HEAD code paths in `TankAlarm_I2C.h`, `TankAlarm-112025-Client-BluesOpta.ino`, `TankAlarm-112025-Sensor_Utility.ino`, `Blueprint_CH0_Test.ino`, and `P1_Transistor_Gating_Test.ino`. The earlier conclusion remains directionally correct: the production read path is still a bare `Wire.write(channel)` / `requestFrom(2)` shortcut, while the known-good diagnostic path explicitly configures the A0602 channels with `AnalogExpansion::beginChannelAsCurrentAdc()` and reads through `pinCurrent()`.

### Additional Findings

1. **Run raw-before-config and configured reads in a controlled order.**
   `Blueprint_CH0_Test` may configure CH0 into current-ADC mode and leave the expansion in that state until the A0602 is reset or power-cycled. If the raw `P1_Transistor_Gating_Test` is run after Blueprint, the raw shortcut might appear to work only because Blueprint already performed the missing channel configuration. The cleanest diagnostic is a single temporary sketch that does this in one boot: power-cycle/reset the expansion stack, run the raw helper before any Blueprint channel configuration, then configure via Blueprint, then run both Blueprint and raw reads again. If the raw value changes only after Blueprint configuration, missing channel config is confirmed. If Blueprint reads ~4 mA and raw still reads ~18 mA, the raw transaction itself is invalid or reading a stale buffer.

2. **Production continues sampling after P1 enable failure.**
   In `readCurrentLoopSensor()`, `tankalarm_setPwm(...ON...)` is retried three times, but if all attempts fail the code only prints a warning and then continues into the current-read loop. That is unsafe for this symptom: a stale/floating in-range value like 18.02 mA bypasses both the I2C failure path and the under-range live-zero gate. If `pwmGatingEnabled` is true and P1 enable fails, set `currentSensorMa = 0`, mark `sampleReused = true`, publish/log a diagnostic, turn P1 off defensively, and return `NAN` without sampling.

3. **P1 disable failure is a power/safety diagnostic, not just a serial warning.**
   If the OFF command fails, the transmitter may remain powered, defeating low-power operation and possibly biasing later reads. Retry the OFF command, publish a diagnostic, and include the last OFF status in daily/health telemetry.

4. **Current telemetry does not expose enough acquisition state.**
   Telemetry includes `ma` only when the mA value is `>= 4.0`. Health telemetry can include I2C counters, but the health feature is disabled by default, and the counters do not catch a plausible stale value. Add a compact current-loop diagnostic block while this issue is unresolved: A0602 address, channel, P-output channel, `pwmOnOk`, `pwmOffOk`, raw 16-bit value, valid sample count, averaged mA, and read status (`ok`, `pwm-on-failed`, `short-read`, `nack`, `underrange`, `stale-suspect`).

5. **`diag.qo` exists on the client, but the server currently does not poll a diagnostic inbox.**
   The client can publish `diag.qo` events for I2C recovery, but the server `pollNotecard()` list handles telemetry, alarm, daily, unload, serial, relay, location, and config ACK files; it does not handle a `diag.qi` inbox. If diagnostics are meant to be visible on the server dashboard, add `DIAG_INBOX_FILE`, a Notehub route, and a small server handler, or deliberately send these events through an already-routed/handled notefile.

6. **Do not implement guessed Blueprint command constants.**
   Section 14 mentions command names such as `ARG_OA_GET_ADC_DATA`. Before writing a lightweight replacement for the official library, inspect the installed `Arduino_Opta_Blueprint` source and copy the real command IDs, payload layout, byte order, and CRC/response rules. A partially guessed frame would be worse than the current shortcut because it would look intentional while still returning stale or misdecoded data.

7. **Clamp or reinterpret `pwmGatingSampleDelay`.**
   Current defaults are now 300 ms, but persisted/server-pushed values can still be very large. The live config's 5000 ms setting makes a single 4-sample read take roughly 25 seconds after warmup. The watchdog is kicked in the long waits, but the setting is operationally expensive and easy to confuse with transmitter warmup. Consider separate fields for transmitter warmup and ADC settle, with a bounded settle range such as 50-1000 ms unless an advanced override is explicitly enabled.

8. **The raw formula is only valid after the protocol is verified.**
   `4.0f + raw / 65535.0f * 16.0f` is internally consistent with the current dashboard math, but it assumes the two bytes are a 16-bit 4-20 mA normalized ADC value. If those bytes are a status/header/stale buffer, the formula will confidently produce a plausible but false pressure. The decisive comparison is still raw shortcut bytes versus Blueprint `getAdc()` and `pinCurrent()` on the same physical loop.

9. **Use one A0602 abstraction for inputs and outputs.**
   Production currently mixes a handcrafted Blueprint-style command frame for PWM output with a bare raw shortcut for input. The durable fix should avoid half-migrating: use `Arduino_Opta_Blueprint` / `OptaBlue` for A0602 discovery, channel configuration, input update, and current reading, or implement the exact same official framed protocol for both configuration and reads. If PWM gating remains handcrafted, explicitly test coexistence with `OptaController.update()` on the same expansion and I2C bus.

10. **Align config UI defaults with the client safety floor.**
    The client defaults now use `pwmGatingSampleDelay = 300`, and gated reads are floored to `CURRENT_LOOP_GATED_SETTLE_MS = 300`. The server config UI still presents the sample settling field with a visible `value="5"` and tooltip/example language around 5 ms. That invites confusing configs even though the client floors gated reads. Change the UI default/example to 300 ms, and warn or clamp excessive multi-second settle values. The live `5000` ms value makes each 4-sample read very slow and does not add useful protection once the 300 ms floor is exceeded.

11. **Track acquisition validity separately from the numeric mA.**
    `currentSensorMa` is just a float, and telemetry emits `ma` when it is >= 4.0. This bug demonstrates why that is not enough: 18.02 mA is numerically valid but operationally untrusted. Add fields such as `lastPwmEnableOk`, `lastPwmDisableOk`, `lastCurrentLoopProtocol`, and `lastCurrentLoopFault` (`none`, `pwm-enable-failed`, `i2c-nack`, `short-read`, `under-range`, `stale-suspect`) so dashboard and diagnostics can show "measurement suspect" without inferring trust from the mA value alone.

### Recommended Fix Sequence

1. **Short-term safety fix:** fail the sensor read when P1 enable fails, and publish the P1/read status remotely.
2. **Diagnostic fix:** add server-visible diagnostic handling for client `diag.qo` or route current-loop diagnostics through an existing handled notefile.
3. **Root fix:** replace production reads with the official `Arduino_Opta_Blueprint` API, or implement the exact official channel-config/read frames with CRC validation after confirming constants from the library source.
4. **UI/config fix:** align the server sample-settle defaults with the client's 300 ms gated-read floor.
5. **Regression test:** after any root fix, run raw-before-config and configured-read diagnostics on a freshly reset A0602 so the test itself does not mask missing initialization.

The central conclusion is unchanged: timing/fault-gate fixes are exhausted as explanations. The next firmware change should make acquisition trustworthy, not merely slower.

---

## 16. Implementation Status (v1.9.22)

**Date:** 2026-06-15

The Blueprint library source was inspected (`OptaAnalogProtocol.h`). It **confirms H1 from the source**: a correct read requires two framed commands the production code never sends — `ARG_OA_CH_ADC` (0x09, configure channel as ADC: type/pulldown/rejection) and `ARG_OA_GET_ADC` (0x0A, a framed request whose answer is a CRC'd frame with the value at byte offset 4). Production instead does a bare `Wire.write(channel)` + `requestFrom(2)`, which is neither — so it reads a stale module buffer. By contrast `tankalarm_setPwm()` uses a proper `0x01/0x13` SET frame, which is why output gating works.

Following the Section 15 fix sequence, and because the protocol rewrite cannot be hardware-validated right now (server down, client remote-cellular) and the official stack also re-addresses the expansion, the **safe items were implemented now** and the **root rewrite is staged**:

- **Implemented (v1.9.22):**
  - **Safety fix (item 1):** `readCurrentLoopSensor()` now returns `NAN` (sensor fault) when the P1 gate enable fails after retries — it no longer samples the floating/unpowered channel and reports a fabricated pressure. It records the failed enable, drives P1 off defensively, and lets `validateSensorReading()` escalate.
  - **Diagnostic (item 2, lightweight):** telemetry now emits `pg` (power-gate ok 1/0) for gated current-loop sensors, so a floating/stale read versus a real reading is distinguishable **remotely** (Notehub/dashboard) without a serial cable. This is the missing datum to confirm H1 on the live device.
  - **UI/config fix (item 4):** the config-generator sample-settle default is now 300 ms (was 5 ms), tooltip updated.
- **Staged (needs hardware-in-the-loop validation, item 3 + item 5):** replace the production read with the framed `ARG_OA_CH_ADC` configure + `ARG_OA_GET_ADC` read (with CRC validation), or adopt the official `Arduino_Opta_Blueprint` API. Must be validated on a freshly-reset A0602 (raw-before-config vs configured-read) and checked for coexistence with the handcrafted PWM path and expansion addressing. The new `pg` telemetry + the on-device `Blueprint_CH0_Test` capture should confirm H1 before this lands.

**Net effect now:** the system can no longer report a confident-but-false 43.8 psi when the loop isn't powered (it faults instead), and we gain the remote signal needed to finish the root fix safely. Shipped as v1.9.22.
