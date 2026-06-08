# Code Review: 112025 Client RS485 / Morningstar SunSaver MPPT

**Date:** June 8, 2026  
**Review Version:** Copilot v1.1  
**Reviewed Firmware:** Client v1.7.2  
**Reviewer:** GitHub Copilot  
**Scope:** Client-side Opta RS485 / Modbus RTU communication with the Morningstar SunSaver MPPT, including shared `SolarManager`, client polling, runtime config, alerting, daily reports, and power-management integration.

---

## Executive Summary

The Opta-to-SunSaver link is **functional for the verified production subset**. The client initializes Modbus RTU over RS485, uses `SERIAL_8N2`, applies an Opta-specific RS485 post-transmit delay, enforces a 500 ms timeout floor for the MRC-1 path, reads the verified realtime electrical block, and reads charge setpoints for battery chemistry / DIP-switch verification.

The system is **not complete as a full SunSaver MPPT diagnostic monitor**. In normal production builds, faults, alarms, charge state, temperature, daily amp-hours, daily watt-hours, and true charger daily min/max voltage are disabled behind `SOLAR_ENABLE_UNVERIFIED_REGISTERS`. That is a sensible safety choice because earlier bench work saw implausible values from those addresses, but the resulting field capability is voltage/current monitoring plus setpoint verification, not full Morningstar diagnostics.

The most important new code issue in this review is runtime config drift: server-pushed solar alert thresholds, alert flags, and poll interval are copied into `gConfig.solarCharger`, but the running `SolarManager::_config` is not refreshed unless enable state or transport parameters change. A remote config can therefore appear to apply and persist while the active Modbus poller continues using stale policy until reboot or transport reinit.

No firmware code changes, compile, or upload were performed as part of this review.

---

## Direct Answers

**Is the communication system between the Opta and charger functional?**  
Yes, for the verified production subset: battery voltage, array voltage, charge current, load current, and charge setpoints.

**Is it complete?**  
No. It is complete as a voltage/current monitor and chemistry cross-checker, but incomplete as a full SunSaver diagnostic monitor.

**Are we missing anything?**  
Yes: verified status/fault/temp/energy registers, software-derived daily min/max and no-charge detection from already trusted values, runtime config synchronization into `SolarManager`, partial-read validation, plausibility bounds, and stronger config range validation.

---

## Severity-Ranked Findings

### 1. High: Runtime solar config updates do not refresh `SolarManager` unless transport changes

**Locations:**

- Runtime config parsing in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4210)
- `SolarManager::setConfig()` in [../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L156)
- Poll interval and alert thresholds read from `_config` in [../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L161) and [../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L340)

`applyConfigUpdate()` updates `gConfig.solarCharger.pollIntervalSec`, voltage thresholds, `alertOnLow`, `alertOnFault`, and `alertOnCommFail`. It only restarts the manager when enable state, slave ID, baud, or timeout changes. If only alert thresholds, alert flags, or poll interval change, the already-running `SolarManager` keeps its old private `_config`.

Impact:

- Changing `alertOnCommFail` from false to true may not make communication failure alerts fire until reboot.
- Changing low/critical/high voltage thresholds may not affect `checkAlerts()` until reboot.
- Changing the solar poll interval may not affect actual RS485 polling until reboot.
- Saved config and runtime behavior can disagree, which is painful in field troubleshooting.

Recommended fix:

- After parsing any `solarCharger` block, call `gSolarManager.setConfig(gConfig.solarCharger)` for non-transport changes.
- If `pollIntervalSec` changes, consider resetting `_lastPollMillis` or forcing one immediate poll so the new cadence is visible.
- Keep the current `end()` / `begin()` path for enable, slave ID, baud, or timeout changes.

---

### 2. High: Full SunSaver diagnostics are intentionally disabled in production

**Locations:**

- Register constants in [../TankAlarm-112025-Common/src/TankAlarm_Solar.h](../TankAlarm-112025-Common/src/TankAlarm_Solar.h#L36)
- Production zeroing branch in [../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L241)
- Alerts dependent on those fields in [../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L340)

The production build reads the verified `0x0008..0x000C` realtime ADC block and setpoints. It does not read temperature, charge state, faults, alarms, load state, daily Ah/Wh, or daily min/max unless `SOLAR_ENABLE_UNVERIFIED_REGISTERS` is defined. In the normal branch, those fields are forced to safe defaults every poll.

Impact:

- `SOLAR_ALERT_FAULT`, `SOLAR_ALERT_ALARM`, and `SOLAR_ALERT_HEATSINK_TEMP` cannot fire in production because their inputs are forced to zero.
- `getChargeStateDescription()` normally reports `Starting`, and derived `isCharging` / `isFullyCharged` remain false.
- The system cannot currently tell the operator about charger fault codes, RTS/temp sensor issues, true charge stage, or controller daily energy totals.

Assessment:

- This is not a transport failure. It is an intentional safety limitation because earlier bench reads returned implausible values from those addresses.
- It does mean the system is incomplete if the goal is full Morningstar SunSaver monitoring.

Recommended fix:

- Bench-verify the missing registers against the exact deployed SunSaver firmware revision and a known-good reference such as MSView or the official Morningstar Modbus/PDU document.
- Re-enable only proven registers, with sanity gates before feeding them into alerts.
- Keep fault/alarm alerts disabled by default until verification is complete.

---

### 3. Medium: Daily report publishes placeholder solar fields as if they were real measurements

**Locations:**

- Production daily fields forced to current/zero values in [../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L270)
- Daily report serialization in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5986)

`appendSolarDataToDaily()` correctly suppresses solar data when `communicationOk` is false. When communication is good, it emits `bvMin`, `bvMax`, and `ht`. In production, however, `batteryVoltageMinDaily` and `batteryVoltageMaxDaily` are overwritten to the latest instantaneous battery voltage every poll, and `heatsinkTemp` is forced to zero.

Impact:

- Daily reports can show `bvMin == bvMax == bv`, implying the battery never moved all day.
- `ht: 0` looks like a real 0 deg C heatsink reading, not an unavailable field.
- Server-side consumers cannot distinguish verified measurements from placeholders.

Recommended fix:

- Omit `ht` unless temperature registers are actually enabled and verified.
- Either omit `bvMin` / `bvMax` or compute them in firmware from the verified `batteryVoltage` reading.
- If fields are kept for compatibility, add a validity marker such as `htValid` or `dailyStatsDerived`.

---

### 4. Medium: Software-derived daily min/max and no-charge detection are missing

**Locations:**

- Verified live readings in [../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L193)
- `SOLAR_ALERT_NO_CHARGE` definition in [../TankAlarm-112025-Common/src/TankAlarm_Solar.h](../TankAlarm-112025-Common/src/TankAlarm_Solar.h#L196), description in [../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L392), and JSON mapping in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5920)

The client already reads enough verified data to derive useful daily and health signals without touching unverified registers. It can track daily min/max battery voltage locally, and it can infer charging from `chargeCurrent` and/or `arrayVoltage > batteryVoltage + margin`. Today the daily min/max are not accumulated, and `SOLAR_ALERT_NO_CHARGE` is unreachable because `checkAlerts()` never returns it.

Impact:

- Useful solar health information is left on the table even though the necessary trusted inputs are present.
- A panel/wiring failure with high array voltage and near-zero charge current can go unreported except indirectly through later battery voltage decline.

Recommended fix:

- Track daily `minBatteryVoltage` / `maxBatteryVoltage` inside `SolarManager` and reset them in `resetDailyStats()`.
- Add a sustained no-charge detector using verified live values, with a daylight/array-voltage gate and debounce window.
- Either wire `SOLAR_ALERT_NO_CHARGE` to that detector or remove the enum/reporting path until it is implemented.

---

### 5. Medium: Solar config accepts invalid or unsafe values without range validation

**Locations:**

- Flash load in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L2885)
- Runtime apply in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4210)
- `SolarManager::begin()` only clamps timeout low in [../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L77)

Solar config fields are copied directly from JSON with casts to `uint8_t` / `uint16_t` and floats. There is no range validation for slave ID, baud rate, poll interval, or threshold ordering.

Impact:

- `pollIntervalSec = 0` can cause a solar poll attempt every loop after reboot or after the `setConfig()` fix above.
- Invalid baud rates can make `ModbusRTUClient.begin()` fail or behave unpredictably.
- Slave ID 0 is a Modbus broadcast address and should not be used for reads.
- Bad threshold ordering can suppress alerts or create alert flapping.

Recommended fix:

- Clamp slave ID to 1..247, poll interval to a conservative floor, and baud to supported values such as 9600 unless bench-verified otherwise.
- Enforce `critical < low < high` and sane absolute voltage bounds for the selected pack voltage.
- Log rejected config values and preserve the previous known-good setting.

---

### 6. Low-Medium: Modbus helpers do not verify that all requested words are available before reading

**Locations:**

- `readHoldingRegisters()` and `readInputRegisters()` in [../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L18)
- High-voltage alert check in [../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L376)

After `ModbusRTUClient.requestFrom()` succeeds, the helper reads `count` words without checking `ModbusRTUClient.available()`. If a partial response ever occurs and `read()` returns `-1`, casting to `uint16_t` produces `0xFFFF`. Scaled as voltage, that is an implausibly high value and can trip the high-voltage path, which has no upper sanity bound.

Impact:

- A partial Modbus read could become a false high-voltage reading.
- The same implausible value could feed power-state logic as a valid battery voltage through `getEffectiveBatteryVoltage()`.

Recommended fix:

- Check `available() >= count` before reading, and treat short reads as failed transactions.
- Add upper plausibility clamps to scaled battery and array voltages before storing them in `SolarData`.
- Add an upper bound to the high-voltage alert branch.

---

### 7. Low: FC03/FC04 fallback doubles worst-case blocking on failures

**Locations:**

- `readRegistersWithFallback()` in [../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L42)
- Client poll site in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L1890)

Each failed register group tries holding registers, then input registers. With the 500 ms timeout floor, a failed group can block for about one second. The current production poll is small enough that this is acceptable, but repeated failures still add loop jitter and power cost.

Recommended fix:

- Once the deployed register model is confirmed, remember the successful function code and skip fallback except during startup/probe mode.
- Consider a communication-failure backoff after consecutive failures so a broken RS485 link does not consume power at the normal interval.
- Kick the watchdog around long RS485 transactions if future register sets grow again.

---

### 8. Low: Chemistry verification cannot re-run after a battery chemistry config change without reboot

**Locations:**

- One-shot chemistry check static in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L1896)
- Config-change log notes reboot requirement in [../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4160)

The chemistry cross-check is useful, but `sChemistryChecked` is local-static inside the loop path. When battery chemistry changes through config, the firmware logs that a reboot is needed to refresh the check.

Impact:

- An installer changing battery chemistry in the field may not get an immediate re-check against the SunSaver DIP/setpoints.

Recommended fix:

- Move chemistry-check state out of the local static and reset it when `batteryConfig` changes.
- Optionally re-read setpoints on chemistry changes if the controller may have rebooted or DIP settings changed.

---

### 9. Informational: Startup input-register probe uses a literal address

**Location:** [../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L127)

The FC03 startup probe uses `SS_REG_BATTERY_VOLTAGE`, while the FC04 fallback probe uses literal `0x0008`. They are equivalent today.

Recommended fix:

- Use `SS_REG_BATTERY_VOLTAGE` in both probes to keep future address changes consistent.

---

## Completeness Matrix

| Capability | Status | Notes |
|---|---:|---|
| RS485 physical/framing setup | Complete | Uses Opta RS485, `SERIAL_8N2`, and tuned post-TX delay. |
| Modbus read transport | Functional | FC03/FC04 fallback works, with partial-read robustness caveat. |
| Battery voltage | Functional | Verified realtime register. |
| Array voltage | Functional | Verified realtime register. |
| Charge current | Functional | Verified realtime register. |
| Load current | Functional | Read from verified block, not currently surfaced in daily report. |
| Charge setpoint verification | Functional | Useful for checking SunSaver DIP/chemistry against UI config. |
| Communication failure alert | Intended functional | Alert path exists, but runtime config changes may not reach `SolarManager` until reboot. |
| Low/critical/high voltage alerts | Functional with caveats | Threshold runtime-update drift and upper-sanity gap remain. |
| Charge state | Not complete | Forced to `Starting` unless unverified registers are enabled. |
| Fault and alarm bitfields | Not complete | Forced to zero in production. |
| Heatsink / battery temperature | Not complete | Forced to zero in production; daily report should not emit as real. |
| Daily Ah / Wh | Not complete | Unverified controller registers are disabled. |
| Daily battery V min/max | Misleading/incomplete | Currently mirrors instantaneous voltage; should be derived or omitted. |
| No-charge alert | Not complete | Enum/JSON mapping exists, but `checkAlerts()` never emits it. |

---

## What Looks Sound

- The production code wisely avoids trusting suspect status/fault registers that produced false data in bench testing.
- The realtime block is read as one contiguous group, reducing bus transactions versus older per-register polling.
- `poll()` returns true when a poll attempt occurs, so communication-failure alert evaluation is reachable after repeated failures.
- `appendSolarDataToDaily()` suppresses solar data when `communicationOk` is false, avoiding stale-data daily reports after a link failure.
- Startup logging distinguishes RS485 transport initialization from initial Modbus read success.
- The chemistry/setpoint verification is a strong commissioning feature for a device whose chemistry is selected physically by DIP switches.
- Solar alarm SMS escalation now uses low-battery policy for battery-critical and fault policy for fault/alarm alerts.
- `getEffectiveBatteryVoltage()` conservatively uses the lower of valid SunSaver, Notecard, and analog Vin readings.

---

## Recommended Fix Order

1. Refresh `SolarManager` on non-transport solar config changes using `setConfig()`.
2. Stop daily reports from emitting placeholder `ht`, `bvMin`, and `bvMax` as real values.
3. Add software-derived daily min/max and a debounced no-charge detector from verified voltage/current registers.
4. Add Modbus word-count validation and upper plausibility clamps.
5. Add range validation for solar config fields.
6. Bench-verify fault/alarm/status/temp/energy registers before enabling full SunSaver diagnostics.
7. Add FC03/FC04 mode caching or failure backoff if RS485 failures cause power or latency issues.
8. Reset chemistry verification when battery config changes.

---

## Suggested Verification Tests

1. **Runtime config drift:** Enable solar monitoring, then push only `alertOnCommFail=true`, only `pollIntervalSec`, and only voltage-threshold changes. Verify `SolarManager` behavior changes without reboot.
2. **Partial Modbus read guard:** Mock or instrument a successful `requestFrom()` with too few available words. Verify the read fails rather than storing `0xFFFF` scaled values.
3. **Daily payload honesty:** With `SOLAR_ENABLE_UNVERIFIED_REGISTERS` undefined, verify daily reports omit or mark unavailable `ht`, and derived `bvMin` / `bvMax` move over a simulated day.
4. **No-charge detector:** Simulate array voltage above battery voltage with near-zero charge current for the debounce window. Verify `SOLAR_ALERT_NO_CHARGE` fires once and rate-limits correctly.
5. **Register-map bench:** Compare fault/status/temp/energy register reads against MSView or official Morningstar tooling before enabling those fields in production.
6. **Communication failure:** Unplug RS485 or change slave ID. Verify `communicationOk` flips false after the configured threshold and a comm-failure alert is emitted only when `alertOnCommFailure` is enabled.
