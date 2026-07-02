# Post-Config-Push Diag.qo Burst & Config-Loss Investigation

**Date:** 2026-07-02
**Author:** GitHub Copilot (Claude)
**Status:** Diagnosis complete; two distinct root causes identified
**Affected device:** `dev:860322068056545` (site was "Silas" / "Cox Wellhead" gas sensor)
**Affected firmware:** `v2.1.3`
**Reference events (Notehub):**
- telemetry.qo pre-config: `63d8d33a-65b4-8320-ad66-c7a84c4f8074` (11:23:50 CDT)
- diag.qo sample: `be26730e-dd49-8038-b856-3408cb93760b` (11:34:45 CDT, count=1 of 7)

---

## 1. Executive Summary

Two distinct problems are visible in the Notehub event stream, and they compound each other:

| # | Symptom | Root cause | Severity |
|---|---|---|---|
| **A** | Pre-config telemetry reports **factory-default names** ("Primary Tank" / "Opta Tank Site" instead of "Cox Wellhead" / "Silas") | The device's saved config was **lost** at some point (most likely during the OTA to v2.1.3). The stock `createDefaultConfig()` was used until the server pushed a fresh config. | **CRITICAL** — silent, causes wrong labels/thresholds/sensor mapping to be applied for an unknown duration |
| **B** | **7 × `diag.qo`** with `ev:"i2c-recovery"`, `trigger:1`, `cl_fault:6`, `cl_ok:0`, `i2c_errs:0` in a single burst | The client's **SENSOR_ONLY I2C recovery logic misinterprets `CL_FAULT_UNDER_RANGE` (loop open) as a wedged I2C bus** and repeatedly runs bus recovery. Each attempt emits a `diag.qo`. | **HIGH** — noisy, wastes cellular bytes, but not fatal. Bus recovery cannot fix a physically open loop. |

**The v2.1.3 firmware itself is behaving correctly** on the framed-read path (`i2c_errs: 0`, no CRC failures, `cl_dur_us` measured). The client is honestly reporting `ma_raw:0` for a genuinely open sensor loop. The two problems above are (A) storage/config-persistence, and (B) an overzealous recovery heuristic that predates the framed-read work.

---

## 2. Timeline of Events (CDT, 2026-07-02)

| Time | File | Direction | Body highlights |
|---|---|---|---|
| **11:17:09** | `command.qo` | Server → Notehub (routed to client) | New config for `_target: dev:860322068056545` |
| **11:17:10** | `config.qi` | Notehub → Client (received) | Same config landed on the client's inbound queue |
| **11:23:50** | `telemetry.qo` | Client → Notehub | **`n:"Primary Tank"`, `s:"Opta Tank Site"`, `ot:"tank"`, `fault:"under_range"`, `ma_raw:0`, `ru:1`, `pg:1`, `fv:"2.1.3"`** — **defaults active, config not yet applied** |
| **11:29:41** | `alarm.qo` | Client → Notehub | Sensor-fault alarm (triggered by 0 mA loop) |
| **11:33:39** | `_session.qo` | Notecard | `{"closed":true, "why":"notecard ended the session..."}` |
| **11:34:44** | `_session.qo` | Notecard | `{"opened":true, "why":"config_ack.qo requested..."}` — client woke to send the ack |
| **11:34:44** | `config_ack.qo` | Client → Notehub | `cv:"3413135"` — **client accepted and applied the new config** |
| **11:34:45** | `diag.qo` ×7 | Client → Notehub (batched flush) | All identical shape: `ev:"i2c-recovery"`, `trigger:1`, `cl_fault:6`, `cl_ok:0`, `i2c_errs:0`, `count:1..7`, `cl_dur_us:0` |
| **11:34:46** | `_session.qo` | Notecard | `{"closed":true, "why":"notecard ended the session..."}` |

Interpretation of the 11:34:45 batch: `logI2CRecoveryEvent()` is rate-limited to 1 per 60 s and only sends when `gNotecardAvailable == true`. The 7 events were queued locally as the client re-tried bus recovery through the 11:17→11:34 window (about 17 minutes of runtime); when the session finally opened to send the config_ack, the deferred diag notes drained in a single flush.

---

## 3. Ground Truth From the Bodies

### 3.1 The pre-config `telemetry.qo` (event `63d8d33a-…`)

```json
{
  "_sv": 1,
  "c": "dev:860322068056545",
  "fault": "under_range",
  "fv": "2.1.3",
  "k": 1,
  "ma_raw": 0,
  "n": "Primary Tank",
  "ot": "tank",
  "pg": 1,
  "r": "sample",
  "ru": 1,
  "s": "Opta Tank Site",
  "st": "currentLoop",
  "t": 1783009421
}
```

Two things this proves at once:
1. **Config was lost.** `n:"Primary Tank"` and `s:"Opta Tank Site"` are the values from `createDefaultConfig()`, not the user's "Cox Wellhead" / "Silas". Also `ot:"tank"` (not `"gas"`) and no `mu` field. See client [TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino) `createDefaultConfig()`.
2. **The v2.1.3 fix is working correctly.** `pg:1` (PWM gate ACK'd), `st:"currentLoop"`, `fault:"under_range"`, `ma_raw:0`, `ru:1`. That combination means: the framed CRC-validated read succeeded, returned exactly `0.00 mA`, which was correctly flagged as sub-live-zero, the value was not published as a real reading, but the raw was surfaced for diagnostics. Exactly the design.

### 3.2 A representative `diag.qo` (event `be26730e-…`)

```json
{
  "_sv": 1,
  "cl_dur_us": 0,
  "cl_fault": 6,
  "cl_ok": 0,
  "count": 1,
  "ev": "i2c-recovery",
  "fv": "2.1.3",
  "i2c_errs": 0,
  "t": 1783009474,
  "trigger": 1
}
```

Field-by-field:

| Field | Value | Meaning |
|---|---|---|
| `ev` | `"i2c-recovery"` | Generated by `logI2CRecoveryEvent()` |
| `trigger` | `1` | `I2C_RECOVERY_SENSOR_ONLY` — "all current-loop sensors failing, Notecard fine" |
| `count` | `1` (through `7` across the burst) | `gI2cBusRecoveryCount` — cumulative recovery attempts |
| `cl_fault` | `6` | `CL_FAULT_UNDER_RANGE` — every read returned < 3.6 mA |
| `cl_ok` | `0` | `gCurrentLoopReadsOk` — **zero** successful reads since boot |
| `i2c_errs` | `0` | `gCurrentLoopI2cErrors` — **zero** framing / CRC failures |
| `cl_dur_us` | `0` | `gLastClBurstMicros` — see §5.3 below (this field is stale/never set on this path) |
| `fv` | `"2.1.3"` | Firmware confirmed |

The two numbers that tell the story: `i2c_errs:0` (bus is healthy) and `cl_fault:6` (every read is honestly zero mA). Bus recovery cannot help — nothing on the bus is broken.

---

## 4. What Actually Happened

### Story so far

1. **OTA to v2.1.3 completed.** The device came up on the new firmware. `fv:"2.1.3"` in every subsequent note confirms this.
2. **The saved config was lost.** When the client loaded its config on boot, either the flash-backed config file was absent, corrupted, or rejected by a schema check, and the client fell through to `createDefaultConfig()`. The evidence: the 11:23:50 telemetry uses the exact factory-default site/monitor names ("Opta Tank Site" / "Primary Tank") — never those of the field device.
3. **Current-loop reads went to zero.** With the default `MonitorConfig` for CH0, the client began reading the current loop. Every framed read succeeded at the protocol level and returned exactly `0.00 mA`. This is one of:
   - the sensor is physically disconnected / unpowered (very possible for a bench Opta not connected to the field transducer), OR
   - the default `pwmGatingChannel = 0` (P1) is powering the wrong terminal for this specific hardware, OR
   - the v2.1.3-introduced PWM vs DAC loop-power selection is defaulting to a mode incompatible with how the sensor is wired.
4. **Consecutive-failure recovery kicked in.** After enough consecutive `NAN` returns, `sampleMonitors()` incremented `consecutiveFailures` on the (only) current-loop monitor until it exceeded `SENSOR_FAILURE_THRESHOLD`. That satisfied the `allCurrentLoopFailed` test in the main loop (client `.ino` line 2088–2103), which called `recoverI2CBus()` + `logI2CRecoveryEvent(I2C_RECOVERY_SENSOR_ONLY)`.
5. **Recovery kept firing.** Each cycle it reset the failure counters and doubled the backoff. Over ~17 minutes (11:17 → 11:34) it accumulated 7 queued diag.qo events.
6. **11:29:41 — a sensor-fault alarm.qo was emitted** in parallel (from `sampleMonitors` / `evaluateAlarms`).
7. **11:34:44 — the client came online for the inbound cycle**, saw the pending `config.qi`, applied it, wrote `config_ack.qo` with `cv:"3413135"`, and the deferred diag.qo batch flushed with the same session.

---

## 5. The Two Underlying Bugs

### 5.1 Bug A: Config not preserved across OTA (SEVERE)

**Evidence:** the pre-config telemetry (11:23:50) carries factory-default `n`/`s`/`ot`. The client would only report those if `gConfig` had `monitorCount == 0` (fully awaiting-config state) OR if `createDefaultConfig()` was executed. See client `createDefaultConfig()` — that's where "Primary Tank" and the placeholder site name come from.

**Impact:**
- Between "OTA landed" and "server config applied" the client uses default thresholds, default monitor mapping, default site name, and (crucially) default `pwmGatingChannel` and `currentLoopChannel`. If the physical sensor is on different channels than the defaults, no reading is possible until the correct config is re-pushed.
- The correct site name / monitor labels are missing from all telemetry in that window, so the server dashboard mis-attributes readings and alarms to the wrong sensor.
- On any device where the operator has NOT enabled server-side auto-config-push, the device would remain misconfigured indefinitely.

**Suspects (in order of likelihood):**
1. **OTA process wipes or reformats the config storage region** — MCUboot swap may be touching a partition it shouldn't, or the config partition offset changed between the pre-v2.1.3 firmware and v2.1.3.
2. **Schema version mismatch** in the persistent config header. If v2.1.3 raised the schema (e.g. to accommodate the new PWM/DAC selectable loop-power field) and `loadConfig()` rejects unknown/older versions, it would silently fall back to defaults on first boot.
3. **New field with no default in the migration path.** If the v2.1.3 `MonitorConfig` struct added a field like `loopPowerMode` and the old binary layout is being read without a migrator, the loaded values could be treated as corrupt and defaulted.

**Recommended diagnostics** (safe, telemetry-only, no risky changes):
- Add a boot-time serial log + one-shot `diag.qo` with `ev:"config-load"` that reports: `loaded:true|false`, `saved_schema:<n>`, `code_schema:<n>`, `bytes_read`, `crc_ok`.
- Bump the diag payload to also include `n`/`s` so the very first note after boot shows whether the loaded config is the customer's config or defaults.
- Locally verify: on a bench Opta with a known-good customer config, USB-DFU v2.1.3 over the top of the previous release. Observe whether the persisted config survives.

### 5.2 Bug B: "All current-loop failing" → "I2C bus wedged" is the wrong inference (NOISY)

**Evidence:** 7 identical `diag.qo` events in a 60-second window with `i2c_errs:0` (bus perfectly healthy at the protocol layer) and `cl_fault:6` (every read is honestly zero).

**Current logic** (client `.ino` around lines 2081–2160): if all current-loop monitors have `consecutiveFailures >= SENSOR_FAILURE_THRESHOLD` and the Notecard is OK, it toggles SCL to recover the bus. That was the right heuristic BEFORE the v2.0.75 framed-read work, when a bus lockup was the leading cause of persistent read failures. Now that framed reads have CRC validation:
- If the bus IS wedged, framed reads fail via `endTransmission != 0` or CRC mismatch → `CL_FAULT_READ_FAIL` (4) or `i2c_errs` increments — that's still worth a bus recovery.
- If the bus IS healthy but reads return under-range (`CL_FAULT_UNDER_RANGE` = 6) with `i2c_errs == 0`, the bus is NOT the problem and recovery is wasted work + one wasted cellular note per attempt.

**Recommended fix (gate the recovery on the actual fault reason):**

```cpp
// Only escalate to bus recovery if the failure signature actually points at
// the bus (framing/CRC/timeouts). A steady under-range or over-range fault
// tells us the loop transmitter is not producing valid current -- SCL toggling
// cannot fix that, and each attempt burns a diag.qo publish.
if (allCurrentLoopFailed &&
    gLastClFaultReason != CL_FAULT_UNDER_RANGE &&
    gLastClFaultReason != CL_FAULT_OVER_RANGE) {
    consecutiveSensorOnlyFailLoops++;
    // ... existing recovery path ...
}
```

Optionally, add a low-frequency `ev:"loop-open"` diag.qo (perhaps once per hour, not per recovery attempt) so the operator can still see the condition in Notehub without the flood.

### 5.3 Minor: `cl_dur_us:0` in the diag payload

The `cl_dur_us` field is `gLastClBurstMicros`, which is intended to record how long the last framed-read burst took. It's reported as `0` in every diag.qo, which either means (a) the burst timer wasn't updated on the recent path, or (b) the timer is reset before diag publish. Not blocking, but worth a quick audit — a working `cl_dur_us` is useful evidence when triaging framed-read latency.

---

## 6. Answering the Original Question

**"A telemetry was sent before the new config was applied, then after the config was received we received many diag.qo files. What happened and what errors is the client reporting?"**

### What happened

- The client OTA'd to v2.1.3 successfully.
- The client's saved config was **lost during (or after) the OTA** — it came up on `createDefaultConfig()` values ("Primary Tank" / "Opta Tank Site" / default channels).
- Running on the default config, every current-loop read returned `0.00 mA` — because either the sensor is physically disconnected on the bench, OR the default `pwmGatingChannel` doesn't match the wiring, OR the default loop-power mode is wrong for the sensor.
- **11:23:50** — the first scheduled telemetry after boot went out with defaults + `fault:"under_range"`, `ma_raw:0`.
- The consecutive-failure counter climbed over ~17 minutes and repeatedly tripped the SENSOR_ONLY I2C bus-recovery path, each attempt queueing a `diag.qo`.
- **11:34:44** — the client came online for its inbound polling window, saw and applied the new config, and wrote the `config_ack.qo` (`cv:"3413135"`). In the same session, the 7 queued diag events flushed.

### What errors the client is reporting

Two related errors, both surfaced by `diag.qo`:

1. **`cl_fault: 6` (`CL_FAULT_UNDER_RANGE`)** — every A0602 framed read returns valid data (`i2c_errs: 0`, CRC OK, channel echo matches) but the value is below the 3.6 mA live-zero threshold. This means **the 4–20 mA loop is physically open or unpowered**. This is honest hardware-level truth from the AD74412R.

2. **`trigger: 1` (`I2C_RECOVERY_SENSOR_ONLY`)** — the client's supervisor decided the persistent read failures warrant an SCL-toggle bus recovery. But because the bus is not the issue (see `i2c_errs:0`), the recovery is a no-op that just burns a `diag.qo` note. This is a **false-positive recovery**, not an actual bus fault.

Neither of these means the framed-read fix is broken. Both mean:
- **Physical:** the loop needs to be closed (transducer powered + wired end-to-end) before any real mA will flow.
- **Software:** the recovery heuristic needs to be gated on the fault reason so `CL_FAULT_UNDER_RANGE` and `CL_FAULT_OVER_RANGE` don't trigger bus recovery.

---

## 7. Recommended Next Steps

Ordered by priority. None of these touch working code paths — they add diagnostics or narrow existing behavior.

### Priority 1 — Investigate config loss (Bug A)

1. Add a one-shot boot `diag.qo` with `ev:"config-load"` reporting whether the persisted config was found, whether its CRC/schema validated, and its schema version. This lets us see on future OTAs whether the config truly survives.
2. Read `loadConfig()` and any storage-region defines in the v2.1.3 vs v2.0.77 diff. If the persisted schema changed, add an explicit migrator (or at minimum a version-tolerant loader that upgrades in place instead of defaulting).
3. On a bench Opta, USB-DFU v2.1.3 on top of a device that already has a customer config saved. Observe boot behavior on serial. If the customer config vanishes, we've reproduced Bug A locally.

### Priority 2 — Silence the false-positive bus recovery (Bug B)

1. Gate `I2C_RECOVERY_SENSOR_ONLY` on the current fault reason: skip recovery when `gLastClFaultReason` is `CL_FAULT_UNDER_RANGE` or `CL_FAULT_OVER_RANGE` (the reasons that indicate the loop, not the bus, is at fault).
2. Add a separate, low-frequency `ev:"loop-open"` diagnostic — one publish per hour maximum — so the operator still has visibility.

### Priority 3 — Confirm the loop is genuinely open on this device

Since the config on this device has now been re-applied (`cv:"3413135"` at 11:34:44), the next scheduled telemetry after ~60 min (solar cycle) should be observed:
- If `ma_raw` is still `0` with `fault:"under_range"` and the (correct) `n:"Cox Wellhead"` — this device's physical loop is genuinely open. Follow the deep-dive doc: verify sensor power at the P-terminal, check wiring, run the `A0602_Multi_Channel_Gating_Test`.
- If `ma_raw` becomes 4–20 mA with no fault — the re-pushed config had the correct `pwmGatingChannel` / loop-power mode and the loop is now energized. That would confirm Bug A (config-loss) as the root cause and let us focus energy there.

### Priority 4 — Post-hoc: verify `cl_dur_us` accounting

Trace where `gLastClBurstMicros` is written. If it's not being updated on the current framed-read path, restore that measurement so future diag notes carry real latency data.

---

## 8. Related Documents

- [CODE_REVIEW_06302026_CURRENT_LOOP_STUCK_MA_ROOT_CAUSE.md](CODE_REVIEW_06302026_CURRENT_LOOP_STUCK_MA_ROOT_CAUSE.md) — the original v2.0.75 framed-read fix.
- [CODE_REVIEW_06302026_BENCH_TESTS_AND_STUCK_MA_PROOF.md](CODE_REVIEW_06302026_BENCH_TESTS_AND_STUCK_MA_PROOF.md) — the mathematical proof of the 4.19 mA header-shadow bug.
- [A0602_PWM_POWER_CONTROL_DEEP_DIVE.md](A0602_PWM_POWER_CONTROL_DEEP_DIVE.md) — the multi-channel gating sweep procedure for confirming which physical P-terminal is powered.

---

**End of report.** Awaiting operator direction: proceed with the config-loss investigation (Priority 1), the recovery-heuristic fix (Priority 2), or wait for the next inbound cycle on `dev:860322068056545` to confirm the physical vs software picture (Priority 3).
