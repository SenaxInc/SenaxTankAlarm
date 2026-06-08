# Code Review — Client RS-485 / SunSaver MPPT Communication

- **Date:** 2026-06-08
- **Firmware version:** v1.7.2
- **Reviewer:** GitHub Copilot
- **Scope:** Morningstar SunSaver MPPT monitoring over Modbus RTU / RS-485, from transport
  init through telemetry/alert publishing.

## Files reviewed

- `TankAlarm-112025-Common/src/TankAlarm_Solar.cpp`
- `TankAlarm-112025-Common/src/TankAlarm_Solar.h`
- `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino`
  (setup init, loop poll, `sendSolarAlarm()`, `appendSolarDataToDaily()`,
  `logSolarPollSnapshot()`, chemistry verification)

---

## Executive summary

**Is the link functional?** Yes — for the registers that have been bench-verified.
The transport (8N2 framing, post-TX DE delay, MRC-1 timing, holding/input fallback,
timeout clamp) is solid and field-tuned, and the live electrical block (battery V,
array V, charge I, load I) plus the charge setpoints are read and scaled correctly.

**Is it complete?** No. The communication *plumbing* is complete, but the **monitored
data set is intentionally reduced to a safe subset.** Charge state, fault bitfield,
alarm bitfield, heatsink/battery temperature, daily amp-hours, and true daily battery
V min/max are all **disabled in production** (guarded behind
`SOLAR_ENABLE_UNVERIFIED_REGISTERS`) because their register addresses were not
confirmed for the controller's firmware revision and produced false faults on the
bench (see `TankAlarm_Solar.cpp` readRegisters policy block, dated 2026-04-22).

As a result, several advertised features are currently **dead** in the field:
fault alerts, alarm alerts, heatsink-overtemp alerts, charge-state reporting, and the
"no charge during daylight" alert can never fire. This is the headline answer to
"are we missing anything?" — the system monitors **voltage and current only**.

The good news: the prior review (`CODE_REVIEW_04182026_CLIENT_MODBUS_SOLAR_COMMUNICATION.md`)
raised five defects and **all five are resolved** in v1.7.2 (see Appendix A).

---

## Findings

### F-1 — Rich telemetry disabled in production; dependent alerts are dead code
**Severity: High (completeness, not a crash)**

In `readRegisters()` the production build compiles the `#else` branch, which forces:

```cpp
nextData.heatsinkTemp = 0;
nextData.batteryTemp  = 0;
nextData.chargeState  = CHARGE_STATE_START;
nextData.faults       = 0;
nextData.alarms       = 0;
nextData.loadOn       = false;
nextData.ampHoursDaily = 0.0f;
nextData.batteryVoltageMinDaily = nextData.batteryVoltage;
nextData.batteryVoltageMaxDaily = nextData.batteryVoltage;
```

Downstream consequences:

- `checkAlerts()` can return `SOLAR_ALERT_FAULT`, `SOLAR_ALERT_ALARM`, and
  `SOLAR_ALERT_HEATSINK_TEMP` **only** when `faults`/`alarms`/`heatsinkTemp` are
  non-zero — but those are pinned to 0, so these three alert classes never fire.
- `getChargeStateDescription()` always returns `"Starting"`.
- `updateHealthStatus()` derives `isCharging`/`isFullyCharged` from `chargeState`,
  so both are always false; `solarHealthy` collapses to
  `batteryHealthy && communicationOk`.

**Impact:** The SunSaver's most valuable diagnostics (hardware faults, RTS/temperature
sensor alarms, charge stage) are not observable. An operator reading the daily report
or alarms would have no visibility into a charger fault.

**Recommendation:** Resolve the register map for the deployed SunSaver MPPT firmware
revision against the official *SunSaver MPPT MODBUS Specification*, validate against a
known-good MSView read, then enable the verified addresses. Until then, see F-2/F-3 for
software-derived substitutes that need no unverified registers.

---

### F-2 — Daily report emits placeholder fields as if they were real measurements
**Severity: Medium (misleading telemetry)**

`appendSolarDataToDaily()` unconditionally writes:

```cpp
solar["bvMin"] = roundTo(data.batteryVoltageMinDaily, 2);
solar["bvMax"] = roundTo(data.batteryVoltageMaxDaily, 2);
...
solar["ht"]    = data.heatsinkTemp;  // always 0 in production
```

Because of F-1, `batteryVoltageMinDaily == batteryVoltageMaxDaily == batteryVoltage`
(the latest instantaneous reading) and `heatsinkTemp == 0`. The report therefore
publishes `bvMin == bvMax == bv` and a constant `ht: 0`, which a consumer will read as
"heatsink is 0 °C" and "battery never moved all day."

**Recommendation:**
- Omit `ht` entirely while the temperature register is unverified (mirrors how `ah` is
  already omitted when zero).
- Either omit `bvMin`/`bvMax` or, better, compute them in firmware (F-3).

---

### F-3 — Daily min/max and charge-state could be derived from verified registers
**Severity: Medium (lost capability, recoverable without hardware risk)**

The firmware already reads trustworthy `batteryVoltage`, `arrayVoltage`,
`chargeCurrent`, and `loadCurrent` every poll. Most of the lost functionality can be
reconstructed in software without touching the unverified addresses:

- **Daily V min/max:** track running min/max of `batteryVoltage` across polls; reset in
  `resetDailyStats()` (already called at report time). Today the production `#else`
  branch *overwrites* min/max each poll, which actively defeats this — change it to
  accumulate instead of overwrite.
- **Charge / no-charge inference:** `chargeCurrent > ~0.1 A` (or
  `arrayVoltage > batteryVoltage + margin`) is a reliable "charging" signal; sustained
  `arrayVoltage` high with `chargeCurrent ≈ 0` during daylight is the classic
  panel/wiring fault that `SOLAR_ALERT_NO_CHARGE` was meant to cover.

**Recommendation:** Implement software daily-stat tracking and a derived charging state
from the verified live block. This restores daily extremes and re-activates
`SOLAR_ALERT_NO_CHARGE` (see F-4) using data we already trust.

---

### F-4 — `SOLAR_ALERT_NO_CHARGE` is defined and mapped but never emitted
**Severity: Low (dead feature)**

`SOLAR_ALERT_NO_CHARGE` has an enum value, a `getAlertDescription()` case, and a
`sendSolarAlarm()` JSON mapping (`"no_charge"`), but `checkAlerts()` never returns it.
It is unreachable today.

**Recommendation:** Either wire it up using the F-3 derived charging signal, or remove
the enum/strings to avoid implying a capability that does not exist.

---

### F-5 — Modbus reads don't validate the returned word count before reading
**Severity: Low (robustness)**

```cpp
if (!ModbusRTUClient.requestFrom(slaveId, HOLDING_REGISTERS, startAddress, count)) {
  return false;
}
for (uint8_t index = 0; index < count; ++index) {
  buffer[index] = (uint16_t)ModbusRTUClient.read();   // read() returns -1 if empty
}
```

If the library ever returns success but fewer than `count` words are available, the
trailing `read()` calls return `-1`, which casts to `0xFFFF`. Through
`scaleVoltage(0xFFFF)` that is ≈ 193 V — a value that would also slip past the
`batteryVoltage > batteryHighVoltage` check in `checkAlerts()` (that branch has no
upper-sanity guard, unlike the low/critical branches which require `> 0`).

**Recommendation:** Guard with `if (ModbusRTUClient.available() < count) return false;`
before the read loop, and/or add an upper plausibility clamp on scaled voltages.

---

### F-6 — Holding/input fallback doubles worst-case blocking on failure
**Severity: Low (latency)**

`readRegistersWithFallback()` tries FC03 then FC04, each subject to the (clamped
≥ 500 ms) timeout. A failing realtime read therefore blocks ≈ 1 s; the first poll that
also probes setpoints can block longer. The main loop only kicks the watchdog at the
top of each iteration, so this adds loop jitter (well within the 30 s WDT, but it
delays Notecard servicing and relay handling).

**Recommendation:** Once the device's register model (holding vs input) is confirmed,
drop the fallback to halve worst-case blocking. If the synchronous design stays,
consider kicking the watchdog around the RS-485 transaction, consistent with the
watchdog-feeding pattern used elsewhere in the client.

---

### F-7 — Minor: startup input-probe uses a literal instead of the named constant
**Severity: Informational**

In `begin()` the FC04 startup probe uses `0x0008` directly while the FC03 probe uses
`SS_REG_BATTERY_VOLTAGE`. They're equal today; use the constant for both so a future
address change stays consistent.

---

## What is done well

- **Framing & timing:** Forcing `SERIAL_8N2` and the `RS485.setDelays(0, 1200)` post-TX
  delay are correct for Modbus RTU and the MRC-1, and reflect real bench tuning. The
  rationale comments are excellent and will save the next engineer hours.
- **Timeout clamp:** Raising sub-500 ms saved timeouts to 500 ms prevents the client
  giving up before the MeterBus bridge replies.
- **Stale-data discipline:** `checkAlerts()` suppresses data-driven branches when
  `communicationOk` is false, and the daily report now suppresses solar output entirely
  on comm failure — both correct.
- **Chemistry verification:** Reading back `V_reg`/`V_float`/`V_eq` and cross-checking
  against the UI-selected chemistry/pack voltage (including the lithium "did you load
  the MSView profile / is equalize dangerously enabled" checks) is a genuinely valuable
  safety feature built only on verified registers.
- **Scaling:** `raw * 96.667 / 32768` (V) and `raw * 79.16 / 32768` (A) match the
  documented SunSaver per-unit constants, and the 24 V chemistry path scales setpoints
  ×2 consistently.

---

## Recommended fix order

1. **F-2** (stop emitting `ht`/`bvMin`/`bvMax` placeholders) — quick, prevents
   misleading field data. Low risk.
2. **F-3** (software daily min/max + derived charging state) — restores most lost value
   with zero hardware risk.
3. **F-4** (activate or remove `SOLAR_ALERT_NO_CHARGE`) — follows naturally from F-3.
4. **F-5 / F-7** (read-count guard, constant cleanup) — small robustness wins.
5. **F-1** (verify and re-enable the real status/fault/temp registers) — highest value,
   but gated on confirming the register map against the Morningstar spec / MSView for
   the deployed firmware revision. Do this on the bench with `SOLAR_ENABLE_UNVERIFIED_REGISTERS`.
6. **F-6** (drop fallback / feed watchdog) — once the register model is pinned down.

---

## Answers to the specific questions

- **Is the communication system between the Opta and the charger functional?**
  Yes. The RS-485/Modbus transport works and reliably returns the verified live
  electrical block and setpoints.
- **Is it complete?** No. It is complete as a *voltage/current monitor*. Fault, alarm,
  charge-state, temperature, and energy telemetry are disabled pending register
  verification, which makes the corresponding alerts unreachable.
- **Are we missing anything?** Yes:
  1. Verified addresses for status/fault/alarm/temperature/energy registers (F-1).
  2. Honest daily-report fields — currently placeholder `ht`/`bvMin`/`bvMax` (F-2).
  3. Software-derived daily extremes and charging state that don't need those
     registers (F-3, F-4).
  4. A partial-read guard on the Modbus helpers (F-5).

---

## Appendix A — Status of the 2026-04-18 review findings (all resolved in v1.7.2)

| # | 2026-04-18 finding | Status in v1.7.2 | Evidence |
|---|--------------------|------------------|----------|
| 1 | Comm-failure alerts unreachable (alerts only checked on read success) | **Fixed** | `poll()` now returns `true` when a poll is *attempted*; client evaluates `checkAlerts()` on every attempt. |
| 2 | Fault SMS escalation gated by low-battery flag | **Fixed** | `sendSolarAlarm()` gates BATTERY_CRITICAL on `alertOnLowBattery` and FAULT/ALARM on `alertOnFault`. |
| 3 | Daily report could emit stale solar data | **Fixed** | `appendSolarDataToDaily()` returns early when `!communicationOk`. |
| 4 | Init reported success even when initial read failed | **Fixed** | Setup distinguishes transport-init success from `isCommunicationOk()` and logs the degraded state. |
| 5 | ~2.2 s per-poll blocking (11 sequential reads) | **Fixed/Mitigated** | Production reads only the 5-word live block (+ a one-time 4-word setpoint read); unverified blocks are compiled out. Residual latency tracked here as F-6. |
