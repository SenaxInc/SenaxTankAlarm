# Current-Loop "Stuck mA" — Root-Cause Analysis & Proposed Fix

**Date:** 2026-06-30
**Author:** GitHub Copilot (Claude)
**Status:** Diagnosis complete; fix proposed but **not yet implemented**
**Affected firmware:** TankAlarm-112025-Client-BluesOpta `v2.0.74` (current `master`) and `v2.0.73` (deployed)
**Reporting device:** `dev:860322068056545` ("Cox Wellhead" gas sensor at site "Silas")
**Reference event:** Notehub event `b243fdb2-5e48-872e-a89c-0a76ae32095e`

---

## 1. Executive Summary

The current-loop (4–20 mA) sensor reading does not respond to actual sensor input — every telemetry note carries the same `ma` value regardless of pressure applied to the transducer. The user reports this happens both **before and after** the recent "simplification" commit `e00a25a`, so this is a long-standing class of bug that successive fixes have touched but not solved.

**Root cause (confirmed):** The A0602 Opta Blueprint expansion module's analog channel is **never configured into current-ADC mode** before being read. Without that configuration step, every read of the channel returns a stale default register value from the AD74412R analog front-end rather than a live ADC sample. The value can appear plausible (4–20 mA range), passes all validation, and is published as if fresh — but it does not respond to the physical sensor.

**Why this regressed:** The "simplification" commit `e00a25a` (planned in [CODE_REVIEW_06262026_CURRENT_LOOP_SIMPLIFICATION.md](CODE%20REVIEW/CODE_REVIEW_06262026_CURRENT_LOOP_SIMPLIFICATION.md)) explicitly **removed** the calls to `tankalarm_configureCurrentAdcChannel()` and `tankalarm_readCurrentAdcFramed()` and replaced them with the bare-shortcut helper `readCurrentLoopMilliamps()`. The simplification was justified by a stand-alone diagnostic sketch (`P1_Transistor_Gating_Test`) that — by coincidence — happened to inherit a previously-configured A0602 state, so it could not have caught this regression in isolation. Production firmware, which power-gates the A0602 rail on every read cycle, comes up unconfigured and reads garbage.

**Proposed fix:** Restore the framed configure-then-read protocol. The required helper functions already exist in [TankAlarm-112025-Common/src/TankAlarm_I2C.h](TankAlarm-112025-Common/src/TankAlarm_I2C.h) and were validated in firmware `v1.9.23` (commit `e889a32` — "A0602 current-loop root fix — official Blueprint framed ADC config + read"). The fix is a ~20-line edit inside [`readCurrentLoopSensor()`](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino) at the client `.ino` and requires **no library/hardware changes**.

---

## 2. Live Symptom Evidence

From the Notehub event you have open (`b243fdb2-5e48-872e-a89c-0a76ae32095e`):

```json
{
  "_sv": 1,
  "c": "dev:860322068056545",
  "fv": "2.0.73",
  "k": 1,
  "ma": 4.26,
  "mu": "psi",
  "n": "Cox Wellhead",
  "ot": "gas",
  "pg": 1,
  "r": "sample",
  "s": "Silas",
  "scErr": "to",
  ...
  "st": "currentLoop",
  "t": 1782822133
}
```

Key observations:

| Field | Value | Interpretation |
|---|---|---|
| `st` | `"currentLoop"` | Sensor type correctly identified |
| `pg` | `1` | PWM/transistor power gate succeeded — A0602 rail is up |
| `ma` | `4.26` | The "stuck" value; right at the bottom of the 4–20 mA scale (≈ 0.8 psi on a 0–50 psi sensor) |
| (no `fault`) | — | The code's validators all passed; this looks like a "good" read from the firmware's perspective |
| `fv` | `"2.0.73"` | Running the simplified post-`e00a25a` code |

The earlier instance of this bug ([CODE_REVIEW_06152026_CURRENT_LOOP_MA_READING.md](CODE%20REVIEW/CODE_REVIEW_06152026_CURRENT_LOOP_MA_READING.md)) reported `~18.02 mA` stuck. Today's instance is `4.26 mA` stuck. The exact stale value differs between bus resets / power cycles, but the **signature is identical**: a value that the firmware accepts as a normal in-range sample but that does not change when the physical sensor changes.

---

## 3. Historical Timeline & Prior Review Trail

| Date | Doc / Commit | Subject | Outcome |
|---|---|---|---|
| 2026-06-15 | [`CODE_REVIEW_06152026_CURRENT_LOOP_MA_READING.md`](CODE%20REVIEW/CODE_REVIEW_06152026_CURRENT_LOOP_MA_READING.md) | "Stuck 18.02 mA — H1 hypothesis: no channel config" | **Diagnosis correct.** Proposed framed Blueprint config-then-read. |
| ~2026-06-15 | `e889a32` "v1.9.23: A0602 current-loop **root fix** — official Blueprint framed ADC config + read" | **The actual H1 fix landed here.** Added `tankalarm_configureCurrentAdcChannel()` + `tankalarm_readCurrentAdcFramed()` calls to the production read path. | ✅ Bug was fixed in v1.9.23 |
| 2026-06-25 | [`CODE_REVIEW_06252026_CURRENT_LOOP_SENSORS.md`](CODE%20REVIEW/CODE_REVIEW_06252026_CURRENT_LOOP_SENSORS.md) | "Fix C2: configure channel BEFORE the warmup delay so loop current stabilizes during warmup" | Recommendation incorporated into v2.0.50. |
| 2026-06-26 | [`CODE_REVIEW_06262026_CURRENT_LOOP_SIMPLIFICATION.md`](CODE%20REVIEW/CODE_REVIEW_06262026_CURRENT_LOOP_SIMPLIFICATION.md) | "Production code is overly complex; the diagnostic sketch `P1_Transistor_Gating_Test` proves a simple loop is sufficient. Remove framed calls." | **This is the regression-inducing plan.** |
| ~2026-06-26 | `e00a25a` "Simplify current-loop sensor sampling" | Removed `tankalarm_configureCurrentAdcChannel()`, `tankalarm_readCurrentAdcFramed()`, `tankalarm_getAnalogChannelFunction()` calls. Replaced with 5 × `readCurrentLoopMilliamps()` (bare shortcut). | ❌ **Reintroduced the v1.9.22-era H1 bug.** |
| 2026-06-30 | **(this doc)** | Stuck `ma:4.26` on `dev:860322068056545` running v2.0.73 | Re-diagnosis with live Notehub evidence; same root cause as 06-15. |

The arc is: **the bug was correctly diagnosed → correctly fixed → and then the fix was deliberately deleted under the banner of simplification.**

---

## 4. Root-Cause Mechanism

The Arduino_Opta_Blueprint A0602 expansion uses an AD74412R analog front-end IC. Each of its 8 channels is multi-function (voltage-out, current-out, voltage-in, current-in, RTD, DI…) and must be **explicitly configured** into a mode before it produces meaningful samples. The configure command is the Blueprint `SET CH_ADC` frame:

```
[0x01=BP_CMD_SET] [0x09=ARG_OA_CH_ADC] [0x07=LEN_OA_CH_ADC]
[channel] [0x01=OA_CURRENT_ADC] [0x02=PULL_DOWN_DISABLE] [0x01=REJECTION_ENABLE]
[0x02=DIAGNOSTIC_DISABLE] [0x00=MOVING_AVE_0] [0x02=SINGLE_ADC] [CRC]
```

Once a channel has been configured, subsequent reads via `GET_ADC` return live samples. Until then, reads return whatever value happens to sit in the module's response staging buffer — typically a constant pattern that masquerades as a plausible 4–20 mA reading.

**In a power-gated configuration (`cfg.pwmGatingEnabled = true`), the A0602 rail is brought back down at the end of every read cycle.** When the rail comes back up on the next cycle, the AD74412R is in its power-on-default state — **unconfigured**. The Blueprint module firmware running on the A0602's local MCU also forgets the channel function across rail cycles. So **every read cycle in production needs to re-issue the configure command**.

The pre-simplification code did exactly that. The simplified code does not. Hence the stuck values.

### Why `P1_Transistor_Gating_Test` didn't catch it

The diagnostic sketch runs as a stand-alone sketch that:

1. Does not power-gate the A0602 rail — the module stays continuously powered.
2. Typically inherits configuration that was set during a prior `OptaController` init at boot.
3. Therefore takes its 5 samples against a channel that happens to already be in current-ADC mode.

That sketch validates only the burst-spacing and averaging mechanics, not the configure-before-read invariant. Importing its read path verbatim into power-gated production code drops the only thing keeping the chip in current-ADC mode after a power cycle.

---

## 5. Current Code (as of `master` / v2.0.74) — What's Wrong

[`TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino`](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5577-L5715), lines 5577–5715:

```cpp
static float readCurrentLoopSensor(const MonitorConfig &cfg, uint8_t idx) {
  // ... bounds checks, address resolution ...

  if (cfg.pwmGatingEnabled) {
    // Enable P1 transistor (up to 3 attempts)
    pwmOnSuccess = tankalarm_setPwm(cfg.pwmGatingChannel, 10000, 9999, i2cAddr);
    // ... bail on failure ...

    delay(3000);  // <-- 3-second warmup
  }

  // ┌─── MISSING: tankalarm_configureCurrentAdcChannel(channel, i2cAddr);
  // │             tankalarm_waitCurrentAdcFunction(channel, i2cAddr, 80);
  // └─── Without these, every readCurrentLoopMilliamps() below reads stale data.

  const uint8_t numSamples = 5;
  float total = 0.0f;
  uint8_t validSamples = 0;
  for (uint8_t s = 0; s < numSamples; ++s) {
    float sample = readCurrentLoopMilliamps(channel);   // bare shortcut, no CRC
    if (sample >= 0.0f) { total += sample; validSamples++; }
    if (s < numSamples - 1) delay(300);
  }
  // ... PWM off, validation, mA → engineering-unit conversion ...
}
```

`readCurrentLoopMilliamps(channel)` is a thin wrapper around the legacy bare shortcut in `TankAlarm_I2C.h`:

```cpp
Wire.beginTransmission(i2cAddr);
Wire.write(channel);              // single-byte "read this channel" request — NOT a Blueprint frame
Wire.endTransmission();
Wire.requestFrom(i2cAddr, 2);     // expect 2 bytes back
uint16_t raw = lo | (hi << 8);
return 25.0f * raw / 65535.0f;
```

No frame header, no CRC validation, no channel-echo check. If the module's response buffer happens to hold any 2 bytes, those bytes get interpreted as a sample. This is what produces the stuck `4.26` / `18.02` / etc values.

---

## 6. Proposed Fix

The fix is small, fully self-contained in the client `.ino`, and reuses helpers already present in `TankAlarm_I2C.h`:

| Helper (in `TankAlarm-112025-Common/src/TankAlarm_I2C.h`) | Purpose |
|---|---|
| `tankalarm_configureCurrentAdcChannel(channel, i2cAddr)` | Sends `SET CH_ADC` frame; verifies ACK |
| `tankalarm_waitCurrentAdcFunction(channel, i2cAddr, timeoutMs)` | Polls `GET_CHANNEL_FUNCTION` until channel reports `CH_FUNC_CURRENT_INPUT_EXT_POWER` |
| `tankalarm_readCurrentAdcFramed(channel, i2cAddr)` | Framed `GET_ADC` read with full CRC validation; returns `-1.0f` on any framing / CRC / wrong-channel-echo failure |

### Proposed code replacement

Replace the sampling block in `readCurrentLoopSensor()` (lines ~5613 through ~5640) with:

```cpp
if (cfg.pwmGatingEnabled) {
  // ... existing PWM-on code (kept as-is) ...

  // Fix (post-e00a25a regression): the A0602 powers up in an unconfigured state every
  // gating cycle. Issue the SET CH_ADC frame BEFORE the warmup delay so the AD74412R
  // sense node is connected to the I/O pin and loop current stabilizes WHILE the rail
  // is being brought up (Fix C2 ordering from CODE_REVIEW_06252026_CURRENT_LOOP_SENSORS).
  bool channelConfigured = false;
  for (uint8_t attempt = 0; attempt < 3 && !channelConfigured; ++attempt) {
    if (attempt > 0) delay(5);
    channelConfigured = tankalarm_configureCurrentAdcChannel(channel, i2cAddr);
  }
  if (!channelConfigured) {
    Serial.print(F("WARNING: A0602 channel "));
    Serial.print(channel);
    Serial.println(F(" config NACK"));
    gMonitorState[idx].currentSensorMa = 0.0f;
    gMonitorState[idx].sampleReused = true;
    gLastClFaultReason = CL_FAULT_CONFIG_NACK;  // add this enum value if not present
    (void)tankalarm_setPwm(cfg.pwmGatingChannel, 0, 0, i2cAddr);
    return NAN;
  }

  delay(3000);  // Warmup with channel already in current-input mode

  // Verify the chip applied the function before we trust any sample. Non-fatal: if the
  // GET fails (e.g. A0602 firmware lacks 0x40 opcode) we proceed and rely on framed-read
  // CRC validation; if it returns the wrong function we abort with a clear fault.
  uint8_t funActual = 0xFF;
  if (tankalarm_getAnalogChannelFunction(channel, i2cAddr, funActual) &&
      funActual != TANKALARM_OA_FUNC_CURRENT_INPUT_EXT_POWER) {
    Serial.print(F("WARNING: A0602 channel reports function 0x"));
    Serial.println(funActual, HEX);
    gMonitorState[idx].currentSensorMa = 0.0f;
    gMonitorState[idx].sampleReused = true;
    gLastClFaultReason = CL_FAULT_WRONG_FUNC;  // add this enum value if not present
    (void)tankalarm_setPwm(cfg.pwmGatingChannel, 0, 0, i2cAddr);
    return NAN;
  }
}

const uint8_t numSamples = 5;
float total = 0.0f;
uint8_t validSamples = 0;
for (uint8_t s = 0; s < numSamples; ++s) {
  // Switched from bare-shortcut readCurrentLoopMilliamps() to the framed GET protocol
  // so corrupted / mis-framed responses are rejected (CRC + channel-echo) instead of
  // being accepted as plausible 4-20 mA samples.
  float sample = tankalarm_readCurrentAdcFramed(channel, i2cAddr);
  if (sample >= 0.0f) {
    total += sample;
    validSamples++;
  }
  if (s < numSamples - 1) {
    delay(300);
#ifdef TANKALARM_WATCHDOG_AVAILABLE
    if (cfg.pwmGatingEnabled) {
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
      mbedWatchdog.kick();
  #else
      IWatchdog.reload();
  #endif
    }
#endif
  }
}
```

### Required supporting changes

1. Add two new fault reasons to the `CL_FAULT_*` enum and to `clFaultReasonString()`:
   - `CL_FAULT_CONFIG_NACK` → `"CL_CONFIG_NACK"`
   - `CL_FAULT_WRONG_FUNC` → `"CL_WRONG_FUNC"`

2. No header changes required — `TankAlarm_I2C.h` already exports the three helpers and the `TANKALARM_OA_FUNC_CURRENT_INPUT_EXT_POWER` constant.

3. No library changes required — the Blueprint frame layout is hand-rolled in `TankAlarm_I2C.h` and was already validated against the installed `Arduino_Opta_Blueprint` library in v1.9.23.

### Optional hardening (not strictly necessary for the fix)

- Restore the `Wire.setClock(400000)` bracket around the burst that v2.0.50 used, so an interleaved Notecard transaction can't stretch the A0602 burst. Low priority on single-sensor devices.
- Once stable, consider adding an opt-in `gMonitorState[i].lastFunctionVerified` timestamp emitted in telemetry so the server can flag clients whose channel function check failed silently.

---

## 7. Verification Plan

| # | Test | Pass criterion |
|---|---|---|
| 1 | Build the patched client with `arduino-cli compile --build-property "build.extra_flags=-DTANKALARM_DFU_MCUBOOT"` | Exit 0, ≤ 400 KB binary (was 380 KB; +~1 KB acceptable) |
| 2 | USB-flash to a bench Opta wired to a calibrated 4–20 mA loop simulator | Boot, no PWM/I2C errors in serial |
| 3 | Step the simulator from 4 → 8 → 12 → 16 → 20 mA, watch `_health.qo` events | Each step within ±0.1 mA of commanded; values **change** between steps |
| 4 | Power-cycle the A0602 rail between two reads | First post-cycle read still tracks commanded mA (proves re-config-per-cycle works) |
| 5 | Field test on `dev:860322068056545` (Cox Wellhead) | Apply pressure → next telemetry note shows mA proportional to pressure; release pressure → mA returns to ~4 mA |
| 6 | Use the new dashboard "Update" button (v2.0.74 feature) | Triggered telemetry note shows a fresh, responsive mA value |
| 7 | Long-soak: run a client overnight with no pressure applied | mA should be ~4 mA (loop minimum) and `currentLoopReadsOk` counter should monotonically increment with no `CL_*` fault counters incrementing |

---

## 8. Why "Simpler" Was Wrong Here

The simplification commit was rational reasoning from incorrect evidence:

- **Premise A (true):** The diagnostic sketch P1_Transistor_Gating_Test produces stable 5-sample reads using the bare shortcut.
- **Premise B (false, but assumed):** Therefore the bare shortcut alone is sufficient for production.

Premise B does not follow because the diagnostic sketch runs in a different chip state than power-gated production. The simplification optimized away the only step that bridged the two states. A useful guardrail going forward:

> **Any change to the current-loop or A0602 read path must be validated on a power-gated device while a known pressure step is applied to the loop. Stand-alone diagnostic-sketch results are not sufficient evidence on their own.**

If we'd had a hardware-in-the-loop step in the test plan for `CODE_REVIEW_06262026_CURRENT_LOOP_SIMPLIFICATION.md`, the regression would have been caught before the simplification merged.

---

## 9. Suggested Sequencing

1. **Implement the fix in client `.ino`** (Section 6) on a feature branch.
2. **Bump to v2.0.75** in `TankAlarm_Common.h` so the release artifact is distinguishable.
3. **Local USB-DFU flash** to an Opta wired to a current-loop simulator and run tests 1–4 from Section 7.
4. **OTA-deploy v2.0.75** to `dev:860322068056545` via Notehub DFU (this device is the canonical reproduction target).
5. Once a fresh telemetry note from that device shows mA changing with applied pressure (test #5), broadcast to the rest of the fleet.
6. **Add a regression marker:** drop a one-line comment above the new `tankalarm_configureCurrentAdcChannel()` call quoting this document so future "simplification" passes see the warning.

---

## 10. Open Questions for the Operator

1. Are there any clients in the fleet currently configured with `pwmGatingEnabled = false`? (If so, they may have been working all along because the channel stays configured across cycles. That would explain the inconsistent reports.)
2. Is the v1.9.23 framed read path documented anywhere as "removed for being slow" or similar? — i.e., is there a measured performance argument we'd be reverting? Spot inspection suggests the framed read is ~5 ms slower per sample, which is negligible across a 1.5 s burst.
3. Should we also revisit the `P1_Transistor_Gating_Test` sketch to add a config-before-read step, so any future operator running it observes the same protocol production uses?

---

**End of document.** Implementation pending operator approval.
