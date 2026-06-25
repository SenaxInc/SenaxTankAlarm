# Code Review: RS-485 / SunSaver Follow-Up — Telemetry Observability

**Date:** June 25, 2026  
**Analyst:** GitHub Copilot  
**Scope:** Follow-up to [CODE_REVIEW_06252026_RS485_SUNSAVER_COMMUNICATION.md](CODE_REVIEW_06252026_RS485_SUNSAVER_COMMUNICATION.md) — specifically the question *"should we build in better error tracking into the telemetry?"* The test client has a live SunSaver wired up via the MRC-1 adapter, so any **ambiguous field error** must now be evaluated under the lens that the RS-485 link could itself be misbehaving — and the dashboard currently doesn't carry enough information to rule that in or out without waiting for the next daily report.

---

## 1. Executive summary

**Answer: yes, but narrowly.** The DAILY report block (`appendSolarDataToDaily`) already carries a rich diagnostic surface (`commOk`, `errs`, `merr`, `maddr`, `mms`, `merrTxt`, `scImpl`). The TELEMETRY payload — which is sent every poll interval and is what the operator actually watches — carries only two fields: `scOk` (boolean) and `scImpl` (CRC-valid-but-rejected flag). When `scOk:0` appears in telemetry, the operator currently has **no way to disambiguate**:

| Possible cause | Today's telemetry signature |
|---|---|
| SunSaver powered down / wire fault (timeout) | `scOk:0` |
| Wrong slave ID / wrong baud (illegal data address) | `scOk:0` |
| Noise glitch (CRC error) | `scOk:0` |
| Library returned a CRC-valid but obviously-wrong value | `scOk:0, scImpl:1` |
| `ModbusRTUClient.begin()` itself failed at boot | *no solar fields at all — looks like "solar disabled"* |
| Solar is genuinely disabled by configuration | *no solar fields at all — looks the same as init failure* |

All but the last two collapse to the same indicator. The operator either has to ssh into the client log or wait up to 24h for the daily report.

Fix S1–S4 (v2.0.49) **hardened the transport layer** (baud-scaled DE delay, daily-windowed `merr`, dead-bus fastFail, derived charge state). This review proposes a **complementary telemetry-observability layer** so the existing rich diagnostic data flows out per-poll, not per-day.

This review is **analysis only** — no implementation is included. A separate fix pass (Fix R1–R6) can ship the changes if approved.

---

## 2. Inventory of existing observability

### 2.1 Internal state (already captured, file-static globals in [TankAlarm-112025-Common/src/TankAlarm_Solar.cpp](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp))

| Symbol | Type | Meaning |
|---|---|---|
| `sSolarModbusErrorCount` | `uint32_t` | Lifetime read failures since boot or last `resetModbusErrorStats()` |
| `sSolarLastModbusError[40]` | `char[]` | Last `ModbusRTUClient.lastError()` text (e.g. `"Timeout"`, `"CRC error"`, `"Illegal data address"`) |
| `sSolarLastFailAddress` | `uint16_t` | Register address of the last failing read |
| `sSolarLastResponseMs` | `uint16_t` | Elapsed ms of the last read attempt (whether it succeeded or timed out) |
| `sSolarLastReadImplausible` | `bool` | The last CRC-valid frame was rejected by the plausibility clamp |
| `SolarData::communicationOk` | `bool` | 5-consecutive-success boolean (`SOLAR_COMM_FAILURE_THRESHOLD = 5`) |
| `SolarData::consecutiveErrors` | `uint8_t` | Runtime consecutive failure counter |
| `SolarData::lastReadMillis` | `uint32_t` | Board uptime ms of the last successful read (0 = never) |
| `SolarData::setpointsValid` | `bool` | The chemistry-setpoint registers were ever read OK at begin() |
| `_initialized` | `bool` (private) | `ModbusRTUClient.begin()` succeeded *and* the begin() block ran |
| `_cachedHoldingFC` | `uint8_t` (private) | 0 = neither FC ever worked; 3 = FC03 confirmed; 4 = FC04 confirmed |

### 2.2 Currently emitted in telemetry (`publishNote(TELEMETRY_FILE, ...)`)

Only emitted when `gSolarManager.isEnabled()` is true (which requires both `_config.enabled` AND `_initialized`):

```jsonc
{
  "scOk":   1 | 0,            // _data.communicationOk
  "scImpl": 1                 // only when scOk=0 AND wasLastReadImplausible()
}
```

### 2.3 Currently emitted in the daily report (`appendSolarDataToDaily`)

Failure branch (`!data.communicationOk`):
```jsonc
{
  "solar": {
    "commOk":  0,
    "errs":    <consecutiveErrors>,
    "merr":    <sSolarModbusErrorCount, windowed daily>,
    "maddr":   <sSolarLastFailAddress>,
    "mms":     <sSolarLastResponseMs>,
    "merrTxt": "<sSolarLastModbusError text>",
    "scImpl":  1                                          // optional
  }
}
```

Success branch: voltages/currents + optional faults/alarms.

### 2.4 The gap

Everything in §2.1 EXCEPT the two fields in §2.2 is invisible to the dashboard between daily reports. That is the entire problem this review is asking about.

---

## 3. Findings

### Finding F1 — *(High, Observability)* Telemetry can't distinguish RS-485 failure modes

**Location:** [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L6393](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L6393)

When telemetry reports `scOk:0`, the operator has no idea whether:
1. The SunSaver is powered off (`Timeout`).
2. The slave ID is wrong (`Illegal data address`).
3. The bus is noisy (`CRC error`).
4. A specific register isn't implemented on this firmware revision (`Illegal data address` at a specific register).
5. The library returned data, but the value failed our plausibility clamp (already covered by `scImpl:1`).

Since the test client has a live SunSaver and the operator may also be debugging current-loop sensors in parallel, mislabelling a real RS-485 problem as a transient could lead to wasted effort chasing the wrong subsystem.

**Proposed fix (R1):** when telemetry emits `scOk:0`, also emit:
```jsonc
"scErr":   "to" | "crc" | "ida" | "ifu" | "?",   // taxonomized short tag from sSolarLastModbusError
"scResMs": <sSolarLastResponseMs>,               // ms the failing read took (0 ms = transport never engaged)
"scMaddr": <sSolarLastFailAddress>               // register address that failed
```

Bytes per telemetry note: ~30 extra when `scOk:0`, zero overhead when `scOk:1`. The taxonomization (`scErr`) is a one-line classifier on the existing `lastError()` string.

### Finding F2 — *(High, Observability)* No "never-communicated since boot" signal

**Location:** [TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L263-264](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L263)

`_data.communicationOk` is initialized `false` and only ever becomes `true` after a poll succeeds. After a reboot:

- A client that has been up for 5 minutes with no successful poll
- A client that was healthy for hours and just lost the bus 30 s ago

…produce the **identical** telemetry `scOk:0`. These are very different failure modes (the first usually means a hardware/wiring problem; the second usually means a transient power or noise event).

**Proposed fix (R2):** track and emit a "first successful poll" timestamp:

In `SolarData`:
```cpp
double firstOkEpoch;   // 0 = never communicated since boot
double lastOkEpoch;    // 0 = never communicated since boot
```

Both populated from `currentEpoch()` on success. Emit in telemetry whenever `scOk:0`:
```jsonc
"scOkEver": 0 | 1,      // 1 = link has succeeded at least once since boot
"scLastOk": <epoch>     // when the last successful poll happened (omitted if 0)
```

This lets the server compute "down for X seconds" and show e.g. *"RS-485 down 47 s"* rather than just *"RS-485 down"*.

### Finding F3 — *(High, Observability)* `_initialized=false` silently disables ALL solar telemetry

**Location:** [TankAlarm-112025-Common/src/TankAlarm_Solar.h#L288](../TankAlarm-112025-Common/src/TankAlarm_Solar.h#L288) — `isEnabled() = _config.enabled && _initialized`

If `ModbusRTUClient.begin()` fails at boot (Modbus library returns false), `_initialized=false`. Since `isEnabled()` returns the AND of both, the telemetry guard `if (gSolarManager.isEnabled()) { doc["scOk"] = ...; }` at [TankAlarm-112025-Client-BluesOpta.ino#L6395](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L6395) **emits nothing**. The dashboard then looks identical to "user disabled solar in config" — a totally different state.

This was *probably* what happened on early Modbus bring-up days. Today it would be very rare on Opta but is *exactly* the kind of silent ambiguity the user is asking about.

**Proposed fix (R3):** decouple the two states in the telemetry guard. Drop `_initialized` from the emit check; rely on `_config.enabled` only, and let a new `scInit` flag tell the operator the library state:

```cpp
if (gConfig.solarCharger.enabled) {                      // configured by operator
  doc["scInit"] = gSolarManager.isInitialized() ? 1 : 0; // 0 = library failed to init
  if (gSolarManager.isInitialized()) {
    doc["scOk"]  = gSolarManager.isCommunicationOk() ? 1 : 0;
    // ...
  }
}
```

New public accessor `bool SolarManager::isInitialized() const { return _initialized; }` (one-line helper). Telemetry overhead: 1 field, 6 bytes, only when solar is configured.

### Finding F4 — *(Medium, Observability)* Setpoint-readback success is never surfaced

**Location:** [TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L466-476](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L466) and [.h#L194-198](../TankAlarm-112025-Common/src/TankAlarm_Solar.h#L194)

`SolarData::setpointsValid` becomes `true` if the chemistry setpoint registers (`V_reg`, `V_float`, `V_eq`) were ever successfully read at startup. This is **the strongest possible evidence** that the SunSaver and the Opta are speaking Modbus correctly on this physical bus — it implies the wiring, baud, slave ID, parity, post-TX delay, and library are ALL correct.

Currently `setpointsValid` is used only internally to enable chemistry-derived float thresholding. It is invisible to the dashboard. When a field operator looks at `scOk:0` they cannot tell whether the link has *ever* worked since boot at the Modbus protocol level.

**Proposed fix (R4):** emit `scSpv` (setpoints valid) in the same telemetry block:
```jsonc
"scSpv": 0 | 1     // 1 = chemistry setpoint registers read successfully at startup
```

This is essentially a binary "Modbus link is real at the protocol level, even if real-time registers are currently failing." Six bytes, sent every poll when solar is configured. A field shipper can quickly read it as: `scInit:1 scSpv:1 scOk:0 scErr:"to"` = "boot OK, registers read OK on startup, current poll timing out" → probably power glitch or wire intermittent. Versus `scInit:1 scSpv:0 scOk:0 scErr:"to"` = "library OK but never decoded a single SunSaver frame" → wiring / baud / slave / wrong device.

### Finding F5 — *(Medium, Observability)* Per-error-type counters would classify failure modes

**Location:** [TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L29-32](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L29) — a single `sSolarModbusErrorCount`.

Today's `merr` is a single monotonic counter (now windowed daily). A daily total of 14 errors is ambiguous between:
- 14 timeouts → wire / power / slave-down
- 14 illegal-data-address → register map / firmware revision mismatch / wrong device
- 14 CRC errors → noise / termination / cable run length
- A mix of all three → genuinely flaky link

**Proposed fix (R5):** add per-bucket counters that **sum to** `merr`:
```cpp
static uint32_t sSolarErrTimeout    = 0;  // "Timeout"
static uint32_t sSolarErrCrc        = 0;  // "CRC error" / "CRC mismatch"
static uint32_t sSolarErrIllegalAddr = 0; // "Illegal data address"
static uint32_t sSolarErrIllegalFunc = 0; // "Illegal function"
static uint32_t sSolarErrOther      = 0;  // catch-all
```

Increment via the existing `captureSolarModbusError()` classifier, reset in `resetModbusErrorStats()`. Emit in the daily report's failure block only (telemetry stays thin):
```jsonc
"merrTo":   3,
"merrCrc":  1,
"merrIda":  10,
"merrIfu":  0,
"merrOth":  0
```

Daily payload growth: ~50 bytes, only when the link has had failures.

### Finding F6 — *(Low, Reliability vs. Observability tradeoff)* `SOLAR_COMM_FAILURE_THRESHOLD = 5` causes scOk to lag reality

**Location:** [TankAlarm-112025-Common/src/TankAlarm_Solar.h#L240](../TankAlarm-112025-Common/src/TankAlarm_Solar.h#L240) and [.cpp#L578](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L578)

`SOLAR_COMM_FAILURE_THRESHOLD = 5` means `communicationOk` only flips false after 5 consecutive failures. At a 60 s poll interval that is up to 5 minutes of "looks healthy on the dashboard while actually broken." This threshold is **correct for the SMS alarm pipeline** (avoids SMS spam on one bad poll) but **wrong for the dashboard observability layer**.

**Proposed fix (R6):** keep the threshold for the SMS escalation path but add a **per-poll boolean** that the dashboard can read:

In `SolarData`:
```cpp
bool lastPollOk;          // mirrors success of the most recent poll, no smoothing
```

Telemetry emits both:
```jsonc
"scOk":     0 | 1,        // 5-poll smoothed (current behavior, drives SMS)
"scLast":   0 | 1         // last single poll succeeded (no smoothing) — new
```

When `scOk:1, scLast:0` appears, the operator sees "the latest poll just failed but the link is still considered up." When the pattern persists, `scOk` will catch up. This is the cheapest path to an honest realtime indicator. ~6 bytes per telemetry.

### Finding F7 — *(Speculative, future)* Modbus library hang detection

`ArduinoModbus::requestFrom()` is synchronous and *should* honor the configured timeout, but a wedged USART peripheral could in theory block longer. Today's mitigations:
- Fix S3 fastFail caps consecutive blocking
- Watchdog kicks around long reads at the caller level

This is mentioned for completeness but **NO action is recommended** in this pass. If a hard hang is ever observed in the field, a soft watchdog wrapping each `requestFrom()` could be added. Not a recommended preventive fix today.

### Cross-coupling note — the original concern

The user's hypothesis was: *if ambiguous error signals appear, RS-485 misbehavior could be a contributing cause.* The RS-485 hardware (USART + DE pin) is **electrically distinct** from the I2C bus serving the A0602 current-loop chip, so RS-485 cannot directly corrupt I2C signaling. However, the two subsystems DO share:

1. **Main loop scheduling.** A blocking SunSaver poll up to ~1 s on a dead bus (post-S3) defers all other polled work. The pulse sampler is the most timing-sensitive consumer (Fix S3 specifically addressed this). The current-loop sample window is short (<200 ms post-Fix-C2) and runs at its own cadence; delaying it by 1 s skips a sample but does not corrupt one.
2. **`getEffectiveBatteryVoltage` priority.** When RS-485 fails, the battery voltage source silently falls back from `"mppt"` to `"vin-divider"`. The source label `vs` IS in telemetry, but the dashboard cards may not surface it prominently — a separate, smaller observability item.
3. **Serial log noise.** Diagnostic prints from the Solar Manager clutter the serial log when chasing other bugs. Low priority.

None of these is a smoking-gun cross-coupling for current-loop confusion, but #2 deserves a server-side display improvement (already planned).

---

## 4. Recommended deliverables

A future implementation pass — call it **Fix R1–R6** for traceability — would deliver:

| ID | Where | Change | Telemetry size | Risk |
|---|---|---|---|---|
| R1 | Common + Client | Emit `scErr`/`scResMs`/`scMaddr` in telemetry when `scOk:0` | ~30 B when failed | Very low |
| R2 | Common + Client | Track `firstOkEpoch`/`lastOkEpoch`; emit `scOkEver`/`scLastOk` when `scOk:0` | ~20 B when failed | Very low |
| R3 | Common + Client | Add `isInitialized()` accessor; emit `scInit` even when library failed to init | ~6 B always when configured | Low — touches the emit guard |
| R4 | Common + Client | Expose `setpointsValid` as `scSpv` | ~6 B always when configured | Very low |
| R5 | Common + Client | Per-error-type bucket counters in daily | ~50 B in daily when failed | Very low |
| R6 | Common + Client | Add `lastPollOk`; emit `scLast` | ~6 B always when configured | Low |

Total telemetry growth on the **happy path** (link OK): about **12 B** per telemetry note (R3 + R4 + R6, all always-on). On the **failed path**: about **62 B** (R1 + R2 added on top).

For a typical telemetry note (~150 B today), this is a ~8–40% growth depending on link state. Well within Notecard payload limits.

## 5. What this review explicitly does NOT recommend

- Adding new alarm types or new SMS triggers. SMS escalation is governed by `alertOnCommFailure` and the 5-poll threshold; that pipeline is correct as-is.
- Polling more frequently. Existing 60 s interval is appropriate.
- Switching to a non-blocking Modbus library. ArduinoModbus is stable; the cost/benefit of a replacement is poor right now.
- Adding any field that requires `SOLAR_ENABLE_UNVERIFIED_REGISTERS` (faults/alarms/heatsink temp). Those registers are still bench-unverified per the bring-up memory; do not surface them until a real-hardware verification pass closes that gap.

## 6. Recommendation

**Yes, build R1–R4 into the telemetry**, *if* the user agrees with the per-poll byte cost (~12 B happy-path / ~62 B failed-path). R5 (per-error buckets in daily) and R6 (`scLast`) are nice-to-haves that add little risk.

R1–R4 directly answer the user's original concern: *"if we were getting ambiguous error signals, we should take into account that the RS-485 signals were not being sent or received or processed properly."* After R1–R4, the dashboard can tell — in real time, from a single telemetry note — whether:
- the Modbus library initialized at all (R3 `scInit`),
- the SunSaver was ever decoded successfully at the protocol level (R4 `scSpv`),
- the current poll failed transport-side or data-side (R1 `scErr`/`scResMs`/`scMaddr`),
- the link has succeeded at any point since boot (R2 `scOkEver`/`scLastOk`).

That is the minimum required to take RS-485 off the suspect list when chasing current-loop or other sensor anomalies on the same client.

---

## 7. Open questions for the user

1. **Telemetry size budget**: any constraint we should respect? (`MAX_PAYLOAD_SIZE` is not currently exceeded but worth confirming.)
2. **Ship as v2.0.53** *(client-only, since these are emit-side changes)* or batch with the next change set?
3. **Server-side rendering**: this review proposes payload additions only. A complementary server-side render-side pass (Fix 14?) would show these fields in the dashboard — otherwise R1–R6 will sit in the Notehub events stream where only operators with the Notehub console will see them.
