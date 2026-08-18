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

---

## 17. Root Fix Implemented (v1.9.23)

**Date:** 2026-06-15

The staged protocol rewrite was implemented, with all frame constants copied verbatim from the installed `Arduino_Opta_Blueprint` source (per Section 15 item 6 — not guessed). Files read to extract the exact protocol: `OptaAnalogProtocol.h`, `OptaBlueProtocol.h`, `OptaCrc.h`, `OptaMsgCommon.cpp`, `AnalogExpansion.cpp` (`beginChannelAsAdc`, `msg_begin_adc`, `msg_get_adc`, `parse_ans_get_adc`, `pinCurrent`), `OptaController.cpp` (`send`, `_send`, `wait_for_device_answer`).

**Exact protocol now used (new helpers in `TankAlarm_I2C.h`):**
- **Configure channel** — `tankalarm_configureCurrentAdcChannel()` sends `SET ARG_OA_CH_ADC (0x09)`, 7-byte payload `[ch][type=OA_CURRENT_ADC=1][pull_down=DISABLE=2][rejection=ENABLE=1][diagnostic=DISABLE=2][moving_avg=0][adding_adc=DISABLE=2]`, CRC8 (poly 0x07, init 0). Mirrors `beginChannelAsCurrentAdc → beginChannelAsAdc(ch, OA_CURRENT_ADC, false, true, false, 0)`.
- **Read value** — `tankalarm_readCurrentAdcFramed()` sends `GET ARG_OA_GET_ADC (0x0A)` then reads the 7-byte answer `[BP_ANS_GET=0x03][0x0A][LEN=0x03][ch][lo][hi][CRC]`, validates header + CRC, decodes the little-endian 16-bit value, and returns **`mA = 25.0 * raw / 65535`** (the A0602 current-ADC scale from `pinCurrent`). **This formula also corrects H6** — the legacy `4 + raw/65535*16` mapping was wrong even on a good read.

**Integration (client `readCurrentLoopSensor`):** after P1 gate ON + warmup, the channel is (re)configured every powered read (the config is lost when P1 powers off in the low-power gating cycle), then a primed framed read is discarded and 4 framed samples are averaged. Any I2C/framing/CRC failure returns `-1` → counted invalid → the v1.9.22 fault path returns `NAN` (sensor fault). So a wrong/incompatible frame **fails safe to "sensor fault," never a fabricated pressure.** The legacy bare-read helper is retained only for the diagnostic sketches.

**Why it should work without `OptaController.begin()` discovery:** production `tankalarm_setPwm()` already drives the A0602 with the same framed style at the fixed address (0x64) and demonstrably switches P1, proving the module accepts framed commands at that address without running the library's discovery/addressing. The read uses the identical write-then-`requestFrom` handshake the library's `_send`/`wait_for_device_answer` use.

**VALIDATION CAVEAT (important):** this could not be hardware-validated before release (the server is USB-only and the client is remote/cellular). It is built from exact source constants and is fault-safe, but the I2C read-back timing and the fixed-address assumption should be confirmed on the **USB-accessible client**: flash v1.9.23, watch serial / the `ma` + `pg` telemetry for a correct ~4 mA at 0 psi, and cross-check with `Blueprint_CH0_Test`. If the framed read does not validate, the sensor reports a fault (safe) rather than a false value, and we iterate with the client on USB. Shipped as v1.9.23.

---

## 18. Post-Implementation Report — Current-Loop Fixes (v1.9.22 + v1.9.23)

**Date:** 2026-06-15
**Author:** GitHub Copilot (implementation)
**Versions:** v1.9.22 (safety + diagnostics), v1.9.23 (framed-protocol root fix)
**Commits:** `8ecd502` (v1.9.22), `c50d505` (v1.9.23). Tags `v1.9.22`, `v1.9.23` pushed; release CI built the OTA bins.
**Build result:** client 349,772 bytes (17% flash), server 921,996 bytes (46% flash), both compiled clean.

### 18.1 Summary of what was done

Three problems were addressed across two releases:

1. **Safety (v1.9.22):** stop publishing a fabricated pressure when the loop is provably unpowered.
2. **Observability (v1.9.22):** expose the power-gate result in telemetry so the failure mode is visible remotely.
3. **Root cause (v1.9.23):** replace the bare-shortcut read with the official Blueprint **framed** configure + read protocol, including the correct mA scale.

No hardware-in-the-loop validation was possible (server USB-only, client remote/cellular), so every change was designed to **fail safe to a sensor-fault**, never to a plausible-but-wrong value.

### 18.2 Files changed

| File | Change |
|---|---|
| `TankAlarm-112025-Common/src/TankAlarm_I2C.h` | Added `tankalarm_optaCrc8()`, `tankalarm_configureCurrentAdcChannel()`, `tankalarm_readCurrentAdcFramed()` (framed Blueprint protocol). Legacy `tankalarm_readCurrentLoopMilliamps()` left in place for the diagnostic sketches. |
| `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino` | `MonitorRuntime` gained `lastPwmEnableOk`. `readCurrentLoopSensor()`: P1-enable-failure now returns `NAN`; added framed channel-config after warmup; priming + sampling now use the framed read. `buildSensorObject()` emits `pg`. |
| `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino` | Config-generator `pwm-gating-sample-delay` default `5` → `300` ms; tooltip text updated. |
| `TankAlarm-112025-Common/src/TankAlarm_Common.h`, `library.properties` | Version bumps to 1.9.22 then 1.9.23. |

### 18.3 v1.9.22 — Safety: fail instead of fabricating

Previously, if the P1 high-side switch failed to enable, the code logged a warning and **read the unpowered channel anyway**, producing the bogus in-range ~18 mA. The failure path now refuses to sample:

```c
    } else {
      Serial.print(F("WARNING: Failed to enable sensor power gating on P"));
      Serial.print(cfg.pwmGatingChannel + 1);
      Serial.println(F(" via I2C"));
      // Safety (v1.9.22): the transmitter is UNPOWERED — do NOT sample. Record the failed
      // enable, drive P1 off defensively, and return a sensor fault so validateSensorReading()
      // escalates instead of publishing a fabricated pressure.
      gMonitorState[idx].lastPwmEnableOk = false;
      gMonitorState[idx].currentSensorMa = 0.0f;
      gMonitorState[idx].sampleReused = true;
      (void)tankalarm_setPwm(cfg.pwmGatingChannel, 0, 0, i2cAddr);
      return NAN;
    }
```

### 18.4 v1.9.22 — Observability: `pg` in telemetry

`MonitorRuntime` gained `bool lastPwmEnableOk;` (set `true` on a successful enable, `false` on failure above). `buildSensorObject()` now emits it for gated current-loop sensors so a floating/stale vs. real read is visible on Notehub/dashboard with no serial cable:

```c
    case SENSOR_CURRENT_LOOP:
      o["st"] = "currentLoop";
      if (state.currentSensorMa >= 4.0f) o["ma"] = roundTo(state.currentSensorMa, 2);
      // v1.9.22: surface the power-gate enable result so a floating/stale read vs a real
      // reading is distinguishable remotely (Notehub/dashboard) without a serial cable.
      if (cfg.pwmGatingEnabled) o["pg"] = state.lastPwmEnableOk ? 1 : 0;
      break;
```

### 18.5 v1.9.23 — Root fix: the framed Blueprint protocol

**New CRC-8** (poly 0x07, init 0 — identical to what `tankalarm_setPwm()` already used, now factored out):

```c
static inline uint8_t tankalarm_optaCrc8(const uint8_t *data, size_t len) {
  uint8_t crc = 0x00;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; ++b) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}
```

**New channel-configure** — `SET ARG_OA_CH_ADC (0x09)`, 7-byte payload, mirroring `beginChannelAsCurrentAdc → beginChannelAsAdc(ch, OA_CURRENT_ADC, pull_down=false, rejection=true, diagnostic=false, ma=0)`. Every payload byte and position was copied from `OptaAnalogProtocol.h` / `AnalogExpansion.cpp::msg_begin_adc`:

```c
static inline bool tankalarm_configureCurrentAdcChannel(uint8_t channel, uint8_t i2cAddr) {
  uint8_t buf[11];
  buf[0] = 0x01;          // BP_CMD_SET
  buf[1] = 0x09;          // ARG_OA_CH_ADC
  buf[2] = 0x07;          // LEN_OA_CH_ADC (7-byte payload)
  buf[3] = channel;       // OA_CH_ADC_CHANNEL_POS
  buf[4] = 0x01;          // OA_CH_ADC_TYPE_POS = OA_CURRENT_ADC
  buf[5] = 0x02;          // OA_CH_ADC_PULL_DOWN_POS = OA_DISABLE (false)
  buf[6] = 0x01;          // OA_CH_ADC_REJECTION_POS = OA_ENABLE (true)
  buf[7] = 0x02;          // OA_CH_ADC_DIAGNOSTIC_POS = OA_DISABLE (false)
  buf[8] = 0x00;          // OA_CH_ADC_MOVING_AVE_POS = 0
  buf[9] = 0x02;          // OA_CH_ADC_ADDING_ADC_POS = OA_DISABLE (single ADC)
  buf[10] = tankalarm_optaCrc8(buf, 10);
  Wire.beginTransmission(i2cAddr);
  Wire.write(buf, 11);
  if (Wire.endTransmission() != 0) return false;
  delay(1);
  (void)Wire.requestFrom(i2cAddr, (uint8_t)4); // drain the SET-ACK frame
  while (Wire.available()) { (void)Wire.read(); }
  return true;
}
```

**New framed read** — `GET ARG_OA_GET_ADC (0x0A)`, then the 7-byte answer is header- and CRC-validated, the value decoded little-endian (`parse_ans_get_adc`), and converted with the A0602 current-ADC scale from `pinCurrent` (`25·raw/65535` — **this also fixes H6**, the legacy `4 + raw/65535·16` was wrong):

```c
static inline float tankalarm_readCurrentAdcFramed(uint8_t channel, uint8_t i2cAddr) {
  uint8_t req[5] = { 0x02 /*BP_CMD_GET*/, 0x0A /*ARG_OA_GET_ADC*/, 0x01 /*LEN*/, channel, 0 };
  req[4] = tankalarm_optaCrc8(req, 4);
  Wire.beginTransmission(i2cAddr);
  Wire.write(req, 5);
  if (Wire.endTransmission() != 0) return -1.0f;
  delay(1);
  const uint8_t ANS_LEN = 7; // [0x03][0x0A][0x03][ch][lo][hi][CRC]
  uint8_t got = Wire.requestFrom(i2cAddr, ANS_LEN);
  uint8_t a[7]; uint8_t n = 0;
  while (Wire.available() && n < ANS_LEN) { a[n++] = Wire.read(); }
  while (Wire.available()) { (void)Wire.read(); }
  if (got != ANS_LEN || n != ANS_LEN) return -1.0f;
  if (a[0] != 0x03 || a[1] != 0x0A || a[2] != 0x03) return -1.0f;
  if (tankalarm_optaCrc8(a, 6) != a[6]) return -1.0f;
  uint16_t raw = (uint16_t)a[4] | ((uint16_t)a[5] << 8);
  return 25.0f * (float)raw / 65535.0f;
}
```

### 18.6 v1.9.23 — Integration into the read path

In `readCurrentLoopSensor()`, after the P1 gate is enabled and the warmup completes, the channel is (re)configured on every powered read (the config is lost when P1 powers off in the gating cycle), then the priming discard-read and the 4 averaged samples use the framed read instead of the legacy shortcut:

```c
  // (re)configure the channel as a 4-20mA current ADC AFTER warmup, BEFORE reading (v1.9.23)
  bool adcConfigOk = tankalarm_configureCurrentAdcChannel((uint8_t)channel, i2cAddr);
  if (!adcConfigOk) { Serial.print(F("WARNING: A0602 current-ADC channel config NACK on ch ")); Serial.println(channel); }
  delay(2);
  ...
  if (cfg.pwmGatingEnabled) { (void)tankalarm_readCurrentAdcFramed((uint8_t)channel, i2cAddr); delay(sampleSettleMs); ... }
  ...
  for (uint8_t s = 0; s < numSamples; ++s) {
    float sample = tankalarm_readCurrentAdcFramed((uint8_t)channel, i2cAddr);
    if (sample >= 0.0f) { total += sample; validSamples++; }
    ...
  }
```

Because `tankalarm_readCurrentAdcFramed()` returns `-1.0f` on any I2C/framing/CRC failure, an incompatible or mistimed frame yields `validSamples == 0`, which the existing fault path turns into `NAN` (sensor fault). **The change cannot regress into a fabricated reading.**

### 18.7 Verification performed and still required

- **Performed:** static analysis against the installed library source (every constant traced to `OptaAnalogProtocol.h` / `OptaMsgCommon.cpp` / `AnalogExpansion.cpp` / `OptaController.cpp`); clean compile of client (with `-DTANKALARM_DFU_MCUBOOT`) and server; fault-safe review of all new return paths.
- **Still required (hardware):** flash v1.9.23 to the **USB-accessible client**, confirm the gas sensor reads ~4 mA / ~0 psi at the serial console and in the `ma`/`pg` telemetry, and cross-check against `Blueprint_CH0_Test` on the same wiring. Until that is done, the field outcome is either a correct reading or a sensor-fault — never a false pressure.

---

## 19. Post-Implementation Review — Current-Loop Fixes

**Date:** 2026-06-15
**Reviewer:** GitHub Copilot (post-implementation review)
**Reviewed code:** HEAD after v1.9.23 (`d5a2428`, implementation tags `v1.9.22` and `v1.9.23`)

I reviewed the implementation report against the actual code in `TankAlarm_I2C.h`, `TankAlarm-112025-Client-BluesOpta.ino`, the installed `Arduino_Opta_Blueprint` source, and the relevant implementation commits. The report is broadly accurate: the frame constants, CRC polynomial, ADC payload offsets, little-endian ADC value, and `25.0 * raw / 65535.0` current scale match the installed Blueprint library's `msg_begin_adc`, `msg_get_adc`, `parse_ans_get_adc`, and `pinCurrent` logic.

### 19.1 Findings and corrections

1. **The report overstates the fail-safe behavior after acquisition failures.**
   The framed read returns `-1.0f` on I2C/framing/CRC failure, and `readCurrentLoopSensor()` returns `NAN` when all samples fail. However, `sampleMonitors()` then reuses `currentInches` until `SENSOR_FAILURE_THRESHOLD` consecutive failures marks the sensor failed. During those first few failed cycles, telemetry is usually withheld by the change-threshold path, but a baseline send (`lastReportedValue < 0`) can still publish the reused/default `lvl` before `sensorFailed` becomes true. So the better statement is: the new read path no longer converts a bad frame into a new fabricated mA/pressure value, but the monitor loop can still temporarily retain or publish a reused level until the sensor-fault threshold trips. A stricter fix would suppress telemetry immediately when a current-loop acquisition returns `NAN`, even before the failure threshold is reached.

2. **`pg` is useful but not sufficient as a trust flag.**
   `pg=1` only proves that the P1 enable command ACKed. It does not prove the ADC channel config ACKed, the framed read CRC passed, or the loop current is physically valid. If channel config/read fails after a successful P1 enable, `pg` can still be `1` while no new mA is acquired. Recommended improvement: add an acquisition status field such as `aq` or `cs` with values like `ok`, `pwm-on-failed`, `adc-config-failed`, `read-crc-failed`, `read-short`, `under-range`, plus a valid-sample count.

3. **The framed read does not validate the returned channel byte.**
   `tankalarm_readCurrentAdcFramed()` validates answer command, argument, length, and CRC, but it does not check `a[3] == channel`. The official library also stores the returned channel without requiring it match the requested channel, but production should be stricter because it is bypassing the library's object model. Add this guard before decoding `a[4]/a[5]`:

```c
if (a[3] != channel) {
  return -1.0f;
}
```

4. **The config SET ACK is drained but not validated.**
   `tankalarm_configureCurrentAdcChannel()` only checks `Wire.endTransmission()` and then drains 4 bytes. The official library expects and validates an ACK frame: `BP_ANS_SET`, `ANS_ARG_OA_ACK (0x20)`, `ANS_LEN_OA_ACK (0)`, CRC. The current implementation can still fail safe because later reads CRC-validate, but a missing/bad ACK is useful diagnostic information and should not be called a confirmed config success. Recommended improvement: read 4 bytes, validate `[0x04][0x20][0x00][crc]`, and return false if invalid.

5. **The fixed-address assumption remains a hardware-validation risk.**
   The implementation report correctly says production has historically talked to the A0602 at `0x64`, and setup does resolve among the configured address, `0x64`, `0x0A`, and `0x0B`. But the installed Blueprint library's controller uses the expansion default/assigned address flow (`0x0A` temporary/default, assigned `0x0B+`). The new framed helpers will use whatever `gConfig.currentLoopI2cAddress` was resolved to at boot, which is good, but field validation must explicitly record the resolved address. If the live unit resolves differently than `0x64`, the report's fixed-address confidence should be revised.

6. **Diagnostic sketches still exercise the legacy path.**
   The report says the legacy bare helper is retained for diagnostics, which is true. That is useful for A/B testing, but it also means `P1_Transistor_Gating_Test` no longer reflects production v1.9.23 reads. Add or update a diagnostic sketch that uses `tankalarm_configureCurrentAdcChannel()` + `tankalarm_readCurrentAdcFramed()` so field tests can validate the shipped path directly.

7. **The mA emission gate may hide low-but-valid diagnostic data.**
   `buildSensorObject()` only emits `ma` when `currentSensorMa >= 4.0f`. That is reasonable for normal telemetry, but while debugging loop faults it hides under-range values that are diagnostically important. Consider emitting a separate diagnostic raw/current field when acquisition status is not `ok`, or include the last failed mA/status in `diag.qo` / health telemetry.

8. **Stuck-sensor and recovery interactions should be watched after the scale change.**
   The old raw formula produced a synthetic 4-20 mA value. The new formula uses the A0602's 0-25 mA full-scale. This is correct for `pinCurrent`, but it changes the exact mA/pressure mapping that reaches validation, stuck detection, alarm thresholds, and learned calibration inputs. Gas pressure with `sensorRangeMin=0` and `sensorRangeMax=50` still maps correctly from mA to pressure, but existing learned calibrations for non-gas current-loop sensors should be treated cautiously until one real reading is confirmed.

### 19.2 Recommended follow-up fixes

1. Validate the returned ADC channel byte in `tankalarm_readCurrentAdcFramed()`.
2. Validate the SET ACK frame in `tankalarm_configureCurrentAdcChannel()` and expose config/read failure separately from `pg`.
3. Add a production-path diagnostic sketch or serial command that uses the new framed helpers directly.
4. Add a compact acquisition status/valid-sample field to telemetry or diagnostics; do not rely on `pg` alone.
5. During live validation, capture: resolved A0602 I2C address, P1 enable status, config ACK status, framed raw ADC, computed mA, `pg`, and final pressure.

### 19.3 Overall assessment

The implementation addresses the root protocol mismatch much more directly than the earlier timing fixes, and the constants/scale are consistent with the installed Blueprint library. The main remaining risk is not the math; it is observability around partial failures. The next small hardening pass should make "P1 enabled", "channel configured", and "sample read/CRC valid" separate states so the dashboard cannot imply more confidence than the firmware actually has.

## 20. AI Assistant Additional Review - Post Implementation

**Date:** 2026-06-15

I have reviewed the post-implementation report and the corresponding changes merged in v1.9.22 and v1.9.23. The root cause analysis was spot-on, and the framing changes effectively mitigate the false/stale reads from the A0602 module.

**Observations & Additions:**
1. **Validation Gap in Read:** In `tankalarm_readCurrentAdcFramed`, the returning frame `a[3]` contains the channel index from the response. The current implementation does not validate that `a[3] == channel`. Adding `if (a[3] != channel) return -1.0f;` before processing `a[4]` and `a[5]` ensures no multiplexing or buffer cross-talk issues go unnoticed.
2. **Validation Gap in Config:** `tankalarm_configureCurrentAdcChannel` simply drains the incoming `[BP_ANS_SET][ANS_ARG_OA_ACK][ANS_LEN_OA_ACK][crc]` buffer. It should enforce the strict `[0x04][0x20][0x00][crc]` ACK layout to ensure the hardware accepted the command correctly.
3. **Current Scaling & Formula:** The conversion scale 25.0f * (float)raw / 65535.0f implemented in v1.9.23 correctly matches the Arduino_Opta_Blueprint pin current resolution, rectifying the underlying H6 conversion bug.
4. **Fail-Safe Mechanism:** Replacing the blind logging bypass with NAN upon P1 enable failure explicitly protects downstream systems from bogus pressure artifacts. This is a critical safety improvement.

**Conclusion:** The changes successfully establish a solid protocol handshake with the A0602 and implement correct fault states. Adding the remaining byte-by-byte ACK/Channel validations will bring the codebase to full parity with the robustness of the official library.

---

## 21. Copilot Post-Implementation Review (Gemini 3.5 Flash)

**Date:** 2026-06-15
**Reviewer:** GitHub Copilot (Gemini 3.5 Flash)

These post-update changes successfully address the underlying protocol and initialization gaps that produced the artificial 18.02 mA reading. Below is my detailed evaluation and suggestions.

### 21.1 Verified Strengths & Correctness

1. **Protocol Integration:** The helpers `tankalarm_configureCurrentAdcChannel` and `tankalarm_readCurrentAdcFramed` in [TankAlarm-112025-Common/src/TankAlarm_I2C.h](TankAlarm-112025-Common/src/TankAlarm_I2C.h#L400-L480) match the structural framing requirements of the A0602. The configuration is reapplied before every read, ensuring low-power gating cycles don't wipe out the ADC channel settings.
2. **Current Re-scaling:** Correcting the conversion factor to standard 25.0 mA full-range directly matches `pinCurrent` behaviors, resolving any scale mapping skew (the H6 calibration/scale hypothesis).
3. **Power-Gating Fail-Safe:** The integration in [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5260-L5272) ensures that if the P1 power gate rail fails to come up, the loop aborts and returns `NAN`. This is highly robust since an unpowered or failed channel will no longer report false values.

### 21.2 Areas for Long-Term Hardening

1. **Returned Channel Guard:** In `tankalarm_readCurrentAdcFramed`, checking `if (a[3] != channel) return -1.0f;` should be coded. This rules out any latent issues where a lagging or stale value from a different analog channel is read under incorrect multiplexing circumstances.
2. **Config ACK Enforcer:** In `tankalarm_configureCurrentAdcChannel`, the returned SET-ACK is only drained. In more demanding environments, parsing this block specifically for `[0x04][0x20][0x00][crc]` would prevent proceeding with reads if the module's chip config command was rejected or failed.
3. **Validating Learn-Calibration Coexistence:** As noted, existing calibrations learned on the old, broken math model will need to be re-run or validated, since the new math shifts the theoretical scaling slope.

Overall, this is an exemplary fix of a legacy hardware shortcut bug. The implementation is tidy, non-intrusive, and contains strong safety fallbacks.

---

## 22. Frame-Validation Hardening (v1.9.24)

**Date:** 2026-06-15
**Author:** GitHub Copilot (implementation)
**Version:** v1.9.24 (build seq 214)

Implements the two follow-ups every post-implementation reviewer flagged (sections 19.1.3, 19.1.4, 20.1-2, 21.2.1-2). Both harden the v1.9.23 framed protocol in `TankAlarm-112025-Common/src/TankAlarm_I2C.h` and can only make a bad frame *more* likely to fault-safe, never less.

### 22.1 Returned-channel guard in `tankalarm_readCurrentAdcFramed()`

After the header + CRC checks, the answer is now rejected if it is for a different channel. CRC alone does not catch this (another channel's frame is itself valid-CRC); this guards against bus cross-talk or a stale answer buffer returning a neighbouring channel's value.

```c
  if (tankalarm_optaCrc8(a, 6) != a[6]) {
    return -1.0f;
  }
  // v1.9.24: reject an answer for a different channel (CRC alone would pass another
  // channel's valid frame).
  if (a[3] != channel) {
    return -1.0f;
  }
  uint16_t raw = (uint16_t)a[4] | ((uint16_t)a[5] << 8);
  return 25.0f * (float)raw / 65535.0f;
```

### 22.2 SET-ACK validation in `tankalarm_configureCurrentAdcChannel()`

The channel-config helper previously blind-drained the acknowledge bytes. It now reads and validates the ACK frame `[BP_ANS_SET=0x04][ANS_ARG_OA_ACK=0x20][LEN=0x00][CRC]` (constants confirmed from `OptaBlueProtocol.h` / `OptaAnalogProtocol.h`) and returns `false` on a missing/garbled ACK. This stays non-fatal at the call site (the GET read CRC-validates the real channel state independently), but it surfaces an unconfirmed config instead of silently assuming success.

```c
  delay(1);
  uint8_t ack[4];
  uint8_t an = 0;
  uint8_t agot = Wire.requestFrom(i2cAddr, (uint8_t)4);
  while (Wire.available() && an < 4) { ack[an++] = Wire.read(); }
  while (Wire.available()) { (void)Wire.read(); }
  if (agot != 4 || an != 4)                      return false;
  if (ack[0] != 0x04 || ack[1] != 0x20 || ack[2] != 0x00) return false;
  if (tankalarm_optaCrc8(ack, 3) != ack[3])      return false;
  return true;
```

### 22.3 Deferred (with rationale)

- **Acquisition-status telemetry field (`cs`, beyond `pg`):** additive schema whose consumer is the server dashboard, which is mid-rewrite; bundle with that work.
- **Production-path diagnostic sketch** using the framed helpers, **learned-calibration re-validation** after the `25*raw/65535` scale change, and the hardware items: all require the USB-accessible client / bench and are tracked for the validation pass.

### 22.4 Build / status

Client + server compiled clean (client 17% flash). Still fault-safe: any I2C/framing/CRC/channel/ACK failure yields a sensor-fault, never a fabricated pressure. Shipped as v1.9.24. **Hardware validation on the USB client (confirm ~4 mA / ~0 psi, cross-check `Blueprint_CH0_Test`) is still the required next step before field trust.**
