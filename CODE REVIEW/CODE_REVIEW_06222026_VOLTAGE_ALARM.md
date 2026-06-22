# Code Review: Voltage Reading and Sensor Alarm Status

## Date: 2026-06-22
## Overview
This code review addresses two reported bugs displayed on the Tank Alarm dashboard:
1. **Incorrect Vin Display (4.69V)**
2. **Erroneous "Sensor Alarm" ("sensor-fault") status**

## 1. Incorrect Vin 4.69V Reading
### Root Cause
The client firmware `TankAlarm-112025-Client-BluesOpta.ino` natively merges multiple voltage sources into the system's battery tracking in `getEffectiveBatteryVoltage()`. Included in this merge is "Source 2", which calls the Notehub API for `card.voltage`. 
While there is an `#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)` preprocessor block inside `pollBatteryVoltage()` intended to disable this on the Blues Wireless Opta Carrier (since `card.voltage` only measures the 5V DC-DC regulator rail, thus yielding ~4.6-5.0V), this protection is failing in your current compilation environment. 
Because `getEffectiveBatteryVoltage()` incorporates this ~4.69V reading, it gets serialized to Notehub in both standard telemetry and daily reports as `doc["v"] = 4.69`. The server parses this and propagates it to the dashboard.

### Suggested Fix
In `TankAlarm-112025-Client-BluesOpta.ino`:
1. **Remove `card.voltage` from battery logic entirely:** Since this system exclusively relies on the optional SunSaver MPPT (RS485) or the optional analog voltage divider, we recommend completely removing "Source 2" from `getEffectiveBatteryVoltage()`. 
2. Update `getEffectiveBatteryVoltage()` to only return a value if `gSolarManager.batteryVoltage` or `gVinVoltage` (analog divider) provides a valid >0 reading.

## 2. Erroneous "Sensor Alarm" Status
### Root Cause
The 4.69V voltage reading causes a severe cascading failure in the power management state machine. 
Because 4.69V is far below the `POWER_CRITICAL_ENTER_VOLTAGE` threshold (11.5V), `updatePowerState()` forces the system into `POWER_STATE_CRITICAL_HIBERNATE`. 
During `CRITICAL_HIBERNATE`:
1. The firmware explicitly forces all relays OFF (`setRelayState(i, false)`) to protect the battery.
2. If any analog sensors are powered through those relays, they lose power.
3. The analog pin begins floating or drops to 0V. This causes the reading to fall outside the `minValid` to `maxValid` bounds initialized in `validateSensorReading()`, or worse, results in out-of-range computation leading to `NaN`.
4. This instantly increments `state.consecutiveFailures` until the threshold is hit, publishing an `alarm.qo` note with `y: "sensor-fault"`.
5. The server sets `t.alarm = true` and `t.alarmType = "sensor-fault"`, which is rendered on the dashboard as `ALARM: sensor-fault` (or "sensor alarm").

### Suggested Fix
1. Fixing the 4.69V Vin issue (preventing the bogus `CRITICAL_HIBERNATE`) will inherently solve the false "sensor-fault" alarm. The sensors will maintain operational power and valid readings.
2. In `TankAlarm-112025-Server-BluesOpta.ino`, you may also optionally refine the `validateSensorReading` bounds logic, but strictly ensuring the system does not incorrectly hibernate is the priority.

## 3. Erroneous 0.0 PSI Display
### Root Cause
The 0.0 PSI reading is a direct secondary symptom of the same `CRITICAL_HIBERNATE` bug. When the system incorrectly enters the hibernate state, the relays de-energize and the sensor loses power, causing it to return 0V to the analog pin.
Additionally, unlike 4-20mA current loop sensors that have a `milliamps < CURRENT_LOOP_FAULT_MA` gate to return `NAN` on a broken loop, the `readAnalogSensor()` function lacks a "live-zero" fault gate for sensors with a raised minimum output (e.g., 1-5V). Instead of faulting on an unpowered 0V signal and generating a `sensor-fault`, it linearly maps 0V to a negative pressure and then silently clamps it:
`if (pressure < 0.0f) pressure = 0.0f;`
Because `0.0` is technically within the broad bounds checked by `validateSensorReading()` (`minValid` is allowed to dip negative, e.g. `-maxValid * 0.1f`), the system accepts the fabricated `0.0` PSI reading as an authentically valid data point and transmits it, masking the unpowered broken state.

### Suggested Fix
1. Applying the fix for the `CRITICAL_HIBERNATE` issue (Section 1) will restore sensor power and bring back the correct ~1 PSI readings automatically.
2. In `TankAlarm-112025-Client-BluesOpta.ino`, update `readAnalogSensor()` to implement a live-zero guard for analog sensors that declare an elevated minimum voltage. If `voltage` drops significantly below `cfg.analogVoltageMin` (e.g., `< 0.2V` when `min >= 1.0V`), the function should return `NAN` to correctly trip `validateSensorReading()` and escalate a `sensor-fault`, rather than silently returning a fabricated 0.0 value.

---

## 4. Detailed Code Review Findings (Automated)

### 4.1 Notehub Event Log Analysis
**Device:** `dev:860322068056545` (client, site "Silas")  
**Server:** `dev:860322068056529`  
**Client Firmware:** v1.9.42  
**Server Firmware:** v1.9.29

**Observed events (2026-06-22, most recent 50):**
- Repeated `alarm.qo` with `{"y":"sensor-fault", "k":1, "s":"Silas"}` — at 05:39, 08:09 AM and continuing
- Corresponding `alarm.qi` events arriving at server device `dev:860322068056529`
- `diag.qo` events with `{"ev":"i2c-recovery", "trigger":1}` — i2c bus recovery attempts
- `_session.qo` open/close events for periodic inbound sync (hourly cadence)
- **No normal `data.qo` / `telemetry.qo` rows visible** in the recent event window
- A routed daily report did appear at 12:00 AM: `daily.qo` from client and matching `daily.qi` to server with body:
    ```json
    {"_sv":1,"alarms":[],"c":"dev:860322068056545","fv":"1.9.42","m":false,"p":0,"s":"Silas","sensors":[{"cap":50,"k":1,"lvl":0,"mu":"psi","n":"Cox Wellhead","ot":"gas","pg":1,"st":"currentLoop"}],"t":1782104400}
    ```

**Significance:** The absence of normal telemetry rows means routine sampled upload is currently suppressed, withheld, or not reaching Notehub in the visible window. The midnight `daily.qo` is important because client code skips daily reports while in `CRITICAL_HIBERNATE`; therefore the device was not simply locked in critical hibernate at midnight. The daily payload also omitted top-level `v`, which means the currently running client did **not** send the 4.69V Notecard rail in that daily report. However, the daily report did explicitly send `lvl:0` for the Cox Wellhead gas sensor, and later `alarm.qo` notes explicitly sent `sensor-fault`. This makes the dashboard symptoms a combination of: stale/previously accepted voltage on the server, a real client-reported 0.0 PSI daily value, and real client-generated sensor-fault alarms.

### 4.2 Server Voltage Display (Server Settings Page)
The server's own settings page at `/server-settings` displays:
> **Notecard Supply (V+ rail): NC: 4.68V**

This is the SERVER's own `gNotecardVoltage` reading (stored separately from client data). It independently confirms that the Opta carrier's DC-DC regulator outputs ~4.68V on the Notecard V+ rail — consistent with the 4.69V reported by the client.

### 4.3 Voltage Data Flow — Full Path Analysis

#### Client Side (sending)
| Code Location | Line | Function | What it does |
|---|---|---|---|
| [Client .ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7340) | ~7340 | `getEffectiveBatteryVoltage()` | Merges 3 sources: MPPT, Notecard, Vin divider — returns min |
| [Client .ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5937) | ~5937 | `sendTelemetry()` | Sets `doc["v"] = getEffectiveBatteryVoltage()` (TEMPORARY, added 2026-06-15) |
| [Client .ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7615) | ~7615 | `sendDailyReport()` | Sets `doc["v"] = getEffectiveBatteryVoltage()` in part 0 |
| [Client .ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L6836) | ~6836 | `appendBatteryDataToDaily()` | Adds `doc["battery"]["v"] = gBatteryData.voltage` (Notecard only) |

#### Server Side (receiving & displaying)
| Code Location | Line | Function | What it does |
|---|---|---|---|
| [Server .ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11624) | ~11624 | `handleTelemetry()` | Parses `doc["v"]` → stores in `ClientMetadata.vinVoltage` |
| [Server .ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L9874) | ~9874 | `sendClientDataJson()` | Serves `clientObj["v"] = meta->vinVoltage` to dashboard API |
| Dashboard JS | ~2240 | `renderTanks()` | Displays voltage in tank card |

#### The 4.69V Propagation Chain
1. `pollBatteryVoltage()` calls `card.voltage` on Notecard → returns ~4.69V
2. `gBatteryData.voltage = 4.69`, `gBatteryData.valid = true`
3. `getEffectiveBatteryVoltage()` Source 2 passes gate (`enabled && valid && > 0`)
4. Returns 4.69V (no other source to compare against; MPPT and Vin divider both inactive)
5. `sendTelemetry()` sets `doc["v"] = 4.69` → published as `data.qo`
6. Server `handleTelemetry()` stores `ClientMetadata.vinVoltage = 4.69`
7. Dashboard displays "4.69V" — value persists in server memory/disk indefinitely

### 4.4 Why the Preprocessor Guard May Be Failing

The guard at line ~6619 is:
```cpp
#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  data.valid = false;
  return false;
#else
  // ... card.voltage polling ...
#endif
```

**Possible failure modes:**
1. **Board FQBN mismatch during compilation:** No `.vscode/arduino.json` or `sketch.json` exists in the client project directory. If the Arduino CLI command uses a FQBN that doesn't set `ARDUINO_OPTA` or `ARDUINO_ARCH_MBED` (e.g., a custom or generic board definition), the guard is bypassed entirely.
2. **The guard works but was added after the initial 4.69V was stored:** If `pollBatteryVoltage()` ran successfully in an earlier firmware version before the guard was added, the 4.69V stored in `ClientMetadata.vinVoltage` on the server persists indefinitely (the server never expires it). Once stored, the server dashboard continues displaying the stale value even if the client stops sending it.
3. **Configuration push enables batteryMonitor:** The server config generator sets `batteryConfig.enabled = (btStr !== 'none')`. If the battery type dropdown was set to anything other than "none" when the config was pushed, `batteryMonitor.enabled` becomes true on the client. However, even with `enabled=true`, the preprocessor guard should still prevent `gBatteryData.valid` from being set — so this alone doesn't explain the 4.69V unless the guard was missing.

### 4.5 Config Push Battery Monitor Interaction
The server's config generator web form (Client Console) includes a battery type dropdown:
```javascript
batteryConfig: {
  enabled: btStr !== 'none',   // true for any type except "none"
  batteryType: btEnum,
  nominalVoltage: bvNominal
}
```
The client's `applyConfigUpdate()` at line ~4566 applies this:
```cpp
gConfig.batteryMonitor.enabled = batEnabled && (bt != BATTERY_TYPE_NONE);
```
**Risk:** If a config was pushed with battery type = "agm" (the default), `batteryMonitor.enabled` becomes true. The client then calls `pollBatteryVoltage()` in its main loop. If the preprocessor guard is absent (scenario 4.4.1), the Notecard's ~4.69V reading enters the system.

### 4.6 Missing Live-Zero Guard in `readAnalogSensor()`
At line ~5276, `readAnalogSensor()` maps the raw voltage to pressure:
```cpp
float pressure = linearMap(voltage, cfg.analogVoltageMin, cfg.analogVoltageMax,
                           cfg.sensorRangeMin, cfg.sensorRangeMax);
if (cfg.objectType == OBJECT_GAS) {
    if (pressure < 0.0f) pressure = 0.0f;  // ← silently clamps to 0.0
    return pressure;
}
```
When the sensor is unpowered (0V), `linearMap(0.0, 1.0, 5.0, 0.0, 30.0)` returns a **negative** value (~-7.5 PSI for a 1-5V/0-30 PSI sensor), which is then clamped to `0.0`. This fabricated `0.0` passes `validateSensorReading()` because `minValid = -maxValid * 0.1f` allows values near zero.

**Contrast with current loop sensors:** `readCurrentLoopSensor()` has a live-zero fault at `milliamps < CURRENT_LOOP_FAULT_MA` (3.6 mA) that correctly returns `NAN` when the loop is broken or unpowered, triggering the fault path.

### 4.7 Server Voltage Persistence — No Expiration
`ClientMetadata.vinVoltage` is set in `handleTelemetry()` and `handleDaily()` but is **never expired or cleared**. Once set, it remains on the dashboard indefinitely, even if:
- The client stops sending voltage (e.g., enters CRITICAL_HIBERNATE and suppresses telemetry)
- The client firmware is updated to no longer include voltage in telemetry
- The voltage source becomes invalid

The `vinVoltageEpoch` timestamp exists but the dashboard does not use it to age-out stale values.

### 4.8 Current HEAD vs Deployed Runtime
The current repository code already contains a strong Opta guard in `pollBatteryVoltage()`:
```cpp
#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    data.valid = false;
    return false;
#else
    // card.voltage polling
#endif
```
If the client is compiled for the normal Opta FQBN (`arduino:mbed_opta:opta`), `ARDUINO_ARCH_MBED` should be defined and `gBatteryData.valid` should remain false. In that build, `getEffectiveBatteryVoltage()` still contains the Notecard source block, but that source is inert because `pollBatteryVoltage()` never marks the data valid on Opta/mbed.

**Review conclusion:** The active 4.69V dashboard display is more likely stale server metadata from an earlier firmware/config state, or a deployment built without the expected Opta/mbed macros, than a value produced by the current HEAD code path. For defense-in-depth, the Notecard source should still be removed or compile-gated directly inside `getEffectiveBatteryVoltage()` so a future code path cannot accidentally resurrect it.

### 4.9 Dashboard Voltage Acceptance and Display Gaps
Server ingestion is still permissive:
- `handleTelemetry()` accepts any positive `doc["v"]` and stores it as `meta->vinVoltage`.
- `handleDaily()` accepts any positive top-level `doc["v"]` unless `solar.bv` is present and positive.
- `sendClientDataJson()` emits any positive `meta->vinVoltage` with no age, source, or plausibility check.
- The main dashboard card renders any positive `_vinVoltage` as `VIN: x.xxV`, while the site-config page has a separate UI guard that hides client voltage below 6.0V.

This inconsistency explains why 4.69V can remain visible on the main dashboard even after newer client firmware stops sending that value. It also means any old or misbuilt client can re-poison the server metadata by sending `v:4.69` once.

### 4.10 Daily Report Can Publish a Faulted/Stale 0.0 PSI
The Notehub daily payload for Silas is `st:"currentLoop"`, not `analog`, and it includes `lvl:0`, `pg:1`, and **no `ma` field**. In current client code, `buildSensorObject()` always emits `lvl = state.currentInches`, but only emits `ma` when `state.currentSensorMa >= 4.0f`. Also, `sampleMonitors()` reuses the previous `state.currentInches` when `validateSensorReading()` rejects a sample.

This creates a data-quality gap: a current-loop read can fail or under-range, leave `currentInches` at its previous/default 0.0 value, omit raw `ma`, and still publish `lvl:0` in the daily report. The server then trusts `lvl` via `resolveLevel()` and updates the dashboard to 0.0 PSI. The existing current-loop live-zero guard is correct, but the daily-report path can still serialize the reused 0.0 state as if it were a valid measurement.

**Review conclusion:** The analog live-zero guard is still a good defense-in-depth fix for analog 1-5V sensors, but the current Silas evidence points more directly at current-loop fault masking in the daily report path.

### 4.11 Daily Alarm Reconciliation Can Clear Diagnostic Faults
Client daily reports intentionally emit `alarms: []` as a high/low alarm reconciliation signal. However, the client currently only adds high/low latched alarms to that array. It does not include diagnostic states such as `sensor-fault` or `sensor-stuck`.

On the server, `handleDaily()` treats a missing sensor from the daily `alarms` array as proof that the server should clear most non-system alarms. The skip list only excludes `solar`, `battery`, and `power`, so diagnostic alarms like `sensor-fault` can be incorrectly cleared by a daily report that was never designed to represent diagnostic fault state.

**Risk:** A daily report with `alarms: []` can temporarily clear a real `sensor-fault` dashboard alarm until the next `alarm.qo` relatches it. That can make the sensor alarm status flicker or appear inconsistent with Notehub.

### 4.12 Sensor Alarm Status Is Not a Dashboard-Only Bug
The visible Notehub rows show outbound `alarm.qo` from `dev:860322068056545` and routed inbound `alarm.qi` to `dev:860322068056529`, both with `y:"sensor-fault"`. The server's `handleAlarm()` path sets `rec->alarmActive = true` and `rec->alarmType = "sensor-fault"`, and the dashboard renders that as `ALARM: sensor-fault`.

**Review conclusion:** The dashboard is accurately displaying a real alarm note. The fix should focus on why the client is generating `sensor-fault` and why daily reports publish 0.0 PSI without raw `ma` or fault context, rather than suppressing the dashboard alarm text.

---

## 5. Consolidated Suggested Fixes

### Fix 1: Remove Notecard `card.voltage` as a battery source (Client — Primary Fix)
**File:** `TankAlarm-112025-Client-BluesOpta.ino`, function `getEffectiveBatteryVoltage()` (~line 7340)

Remove Source 2 (Notecard `card.voltage`) entirely from `getEffectiveBatteryVoltage()`. This hardware exclusively uses the SunSaver MPPT (RS485) and/or the analog Vin voltage divider. The Notecard reading is architecturally wrong on the Opta carrier and provides no useful data on any Opta deployment.

**Before:**
```cpp
// Source 2: Notecard card.voltage
if (gConfig.batteryMonitor.enabled && gBatteryData.valid && gBatteryData.voltage > 0.0f) {
    if (!hasVoltage) {
        voltage = gBatteryData.voltage;
        hasVoltage = true;
    } else {
        voltage = min(voltage, gBatteryData.voltage);
    }
}
```

**After:** Remove the entire Source 2 block, or wrap it in an additional `#if !defined(ARDUINO_OPTA) && !defined(ARDUINO_ARCH_MBED)` guard as defense-in-depth.

### Fix 2: Add live-zero guard to `readAnalogSensor()` (Client — Defense-in-Depth)
**File:** `TankAlarm-112025-Client-BluesOpta.ino`, function `readAnalogSensor()` (~line 5276)

After reading the voltage and before calling `linearMap()`, add:
```cpp
// Live-zero fault guard: if the sensor declares an elevated minimum voltage
// (e.g., 1.0V for a 1-5V transducer) and the actual reading is far below it,
// the sensor is unpowered or disconnected — return NAN to trigger sensor-fault.
if (cfg.analogVoltageMin >= 0.5f && voltage < cfg.analogVoltageMin * 0.2f) {
    return NAN;
}
```

### Fix 3: Server-side voltage expiration (Server — Optional)
**File:** `TankAlarm-112025-Server-BluesOpta.ino`, function `sendClientDataJson()` (~line 9874)

Before including `clientObj["v"]`, check the age of `vinVoltageEpoch`. If the voltage data is older than a configured threshold (e.g., 48 hours), omit it or mark it stale:
```cpp
if (meta && meta->vinVoltage > 0.0f) {
    double age = currentEpoch() - meta->vinVoltageEpoch;
    if (age < 172800.0) {  // 48 hours
        clientObj["v"] = meta->vinVoltage;
        clientObj["ve"] = meta->vinVoltageEpoch;
    }
}
```

### Fix 4: Config generator battery type default (Server — Config)
Verify that the client configuration push for "Silas" does not have `batteryConfig.enabled = true` with a battery type other than "none". In the Client Console config generator, set battery type to "none" for Opta deployments that lack a dedicated battery voltage measurement path (no MPPT, no Vin divider).

### Fix 5: Clear stale client voltage on server (Server — Immediate Action)
After deploying the client fix, the server's stored `ClientMetadata.vinVoltage = 4.69` will persist until a new valid voltage is received. Either:
- Reboot the server (clears in-memory metadata)
- Or add a manual clear endpoint / admin action to reset vinVoltage for a specific client

### Fix 6: Reject stale or implausible client voltage before dashboard display (Server — Recommended)
The server should not expose `clientObj["v"]` just because `meta->vinVoltage > 0`. Recommended minimum guard:
```cpp
if (meta && meta->vinVoltageEpoch > 0.0) {
    double now = currentEpoch();
    double age = (now > 0.0) ? (now - meta->vinVoltageEpoch) : 0.0;
    bool recent = (age >= 0.0 && age < 172800.0);  // 48h
    bool plausibleBattery = (meta->vinVoltage >= 6.0f);  // reject Opta 5V rail values
    if (recent && plausibleBattery) {
        clientObj["v"] = meta->vinVoltage;
        clientObj["ve"] = meta->vinVoltageEpoch;
    }
}
```
This does not replace source validation, but it prevents the current dashboard from continuing to show a stale or physically impossible battery value. The main dashboard and site-config page should use the same rule so one page cannot show 4.69V while another hides it.

### Fix 7: Tag voltage source in client payloads (Client + Server — Recommended)
To meet the product requirement exactly, the client should identify voltage source when it sends `v`:
- `vs:"mppt"` when sourced from SunSaver MPPT / RS485
- `vs:"vin-divider"` when sourced from the analog divider
- no `v` at all when neither optional measurement path is available

The server should store/display voltage only when `vs` is one of the allowed sources. This is stronger than inferring from voltage magnitude because a very low real battery voltage and a 5V Notecard rail can overlap numerically.

### Fix 8: Prevent daily reports from publishing faulted current-loop values as valid 0.0 PSI (Client + Server — Recommended)
For current-loop sensors, a daily report should not emit a bare `lvl:0` when the raw mA is absent/invalid or the sample was reused after a failed validation. Options:
- Client: add status fields such as `ok:false`, `sf:true`, `reused:true`, and `rt`/`lastReadingEpoch` to `buildSensorObject()`.
- Client: omit `lvl` for faulted current-loop samples, or include the previous level only with an explicit stale/reused flag.
- Server: when `st:"currentLoop"` and no valid `ma` is present, do not overwrite `rec->levelInches` from daily `lvl` unless the client explicitly marks the sample as valid.

This directly addresses the observed midnight daily note that reported `lvl:0` with no `ma`.

### Fix 9: Scope daily alarm reconciliation to represented alarm types (Server — Recommended)
In `handleDaily()`, only clear server alarms from the daily `alarms` array if the existing server alarm type is one represented by that array (`high`, `low`, and digital trigger types if added there). Do **not** clear `sensor-fault`, `sensor-stuck`, or other diagnostic alarms merely because they are absent from the high/low daily array.

Alternative: extend the daily schema to include diagnostic sensor health, e.g. `faults:[{"k":1,"y":"sensor-fault"}]`, and reconcile diagnostics explicitly.

### Fix 10: Update stale comments and documentation (Client — Cleanup)
Several comments and README sections still describe `batteryMonitor` / `card.voltage` as a battery health source. On the Blues Wireless for Opta carrier, that is not true. Updating those comments matters because future reviews may otherwise re-enable the same incorrect path.

---

## Summary of Next Steps
1. **Verify compilation target:** Confirm that `ARDUINO_OPTA` or `ARDUINO_ARCH_MBED` is defined when building the client firmware by checking the Arduino CLI `--fqbn` flag in the build command.
2. **Apply Fix 1:** Remove Notecard `card.voltage` (Source 2) from `getEffectiveBatteryVoltage()`.
3. **Apply Fix 2:** Add live-zero guard to `readAnalogSensor()` for defense-in-depth.
4. **Check client config:** Verify the battery type in the pushed config for "Silas" and set to "none" if no hardware voltage measurement path exists.
5. **Flash updated firmware** to the client to exit CRITICAL_HIBERNATE and restore normal operation.
6. **Clear stale server data** by rebooting the server or deploying Fix 5 after the client is re-flashed.
7. **Optionally apply Fix 3:** Add server-side voltage expiration to prevent future stale readings.
8. **Apply server display guard:** Reject/age-out implausible client voltage before `/api/clients` exposes it to the main dashboard.
9. **Review daily-report sensor validity:** Prevent faulted/reused current-loop values from updating the dashboard as valid 0.0 PSI.
10. **Review daily alarm reconciliation:** Ensure `alarms: []` cannot clear diagnostic `sensor-fault` / `sensor-stuck` states.
11. Perform further reviews before any firmware code changes are made.

---

## 6. Live Review — GitHub Copilot (2026-06-22, ~11:09 AM CDT)

This section records a fresh review of the **live Notehub event log** and a **line-by-line re-verification of the current HEAD code**, performed after the analysis in Sections 1–5. Its purpose is to test the earlier theory against what the devices are *actually* transmitting right now. **No firmware code was changed** — this is review-only, per the request to perform more reviews before proceeding.

### 6.1 Live Notehub evidence (50 most recent events, `app:f0a8c2c9…`)

Notefile tally over the visible window (client `dev:860322068056545` "Silas", server `dev:860322068056529`, client fw **1.9.42**):

| Notefile | Meaning | Present? |
|---|---|---|
| `data.qo` / `telemetry.qo` | Routine sampled telemetry | **0 — none at all** |
| any body with a `"v"` field | Client-reported system voltage | **0 — none at all** |
| `alarm.qo` → `alarm.qi` | `sensor-fault` (k:1, Silas) | Yes, ~every 2.5–3 h |
| `diag.qo` | `{"ev":"i2c-recovery","trigger":1,"count":N}` | Yes, paired with each alarm |
| `daily.qo` → `daily.qi` | Midnight daily report | Yes, 1 (00:00) |
| `email.qo` | Server "Daily Sensor Summary" | Yes, 1 (00:00) |
| `_session.qo` | Hourly inbound sync open/close | Yes (periodic) |

**Exact pairing observed** (each `sensor-fault` is emitted ~1 s *before* an `i2c-recovery` diag, with a monotonically increasing `count`):

```
08:09:58 alarm.qo sensor-fault   ← 08:09:59 diag.qo i2c-recovery count:4
05:39:57 alarm.qo sensor-fault   ← 05:39:58 diag.qo i2c-recovery count:3
03:09:57 alarm.qo sensor-fault   ← 03:09:58 diag.qo i2c-recovery count:2
00:39:57 alarm.qo sensor-fault   ← 00:39:58 diag.qo i2c-recovery count:1
22:09:57 alarm.qo sensor-fault   ← 22:09:58 diag.qo i2c-recovery count:9
```

Midnight `daily.qo` body:
```json
{"_sv":1,"alarms":[],"c":"dev:860322068056545","fv":"1.9.42","m":false,"p":0,
 "s":"Silas","sensors":[{"cap":50,"k":1,"lvl":0,"mu":"psi","n":"Cox Wellhead",
 "ot":"gas","pg":1,"st":"currentLoop"}],"t":1782104400}
```

Server `email.qo` body (00:00):
```json
{"_sv":1,"sensors":[{"alarm":true,"alarmType":"sensor-fault",
 "client":"dev:860322068056545","label":"Cox Wellhead","levelInches":0,
 "sensorIndex":1,"sensorMa":18.02,"site":"Silas"}],
 "subject":"Daily Sensor Summary","to":"james@senaxinc.com"}
```

### 6.2 Finding A — The 4.69V is 100% STALE SERVER METADATA (the client is sending no voltage)

The live log contains **zero** notes carrying a `"v"` field — no `data.qo`, and the `daily.qo` omits top-level `v`. The running client is **1.9.42**, which already contains the strong Opta guard in `pollBatteryVoltage()` ([Client .ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L6619)). Because there is no MPPT and no Vin divider on this unit, `getEffectiveBatteryVoltage()` ([Client .ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7340)) returns `0.0`, so `sendTelemetry()` ([Client .ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5935)) and `sendDailyReport()` ([Client .ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7607)) both **skip** the `v` key (`if (voltage > 0.0f)`).

**Conclusion:** the dashboard's `VIN: 4.69V` is not being produced by the current client at all. It is a value the server stored earlier (in `ClientMetadata.vinVoltage`) and **never expires**. This confirms Section 4.8 conclusion (a) and Section 4.7 over Sections 1–3: the Opta/mbed compile guard is working, but the server's permissive ingest + non-expiring storage keep an old reading on screen indefinitely. `handleTelemetry()` stores any positive `v` with no source/age/plausibility check ([Server .ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11640)), and `sendClientDataJson()` re-serves any positive stored value ([Server .ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L10007)).

> Note: the `4.68V` on the server's own `/server-settings` page is a **separate** value — the server's local `gNotecardVoltage` rail (Section 4.2) — and is unrelated to the client's stale `vinVoltage`. They merely happen to be numerically similar because both are ~5V Notecard rails.

### 6.3 Finding B — `sensor-fault` is an I2C bus failure, NOT a CRITICAL_HIBERNATE cascade

The live evidence **contradicts the cascading-hibernate theory** in Sections 1–3:

1. **No voltage is being sent**, so the client cannot be driving its own power-state machine from a 4.69V Notecard reading. `updatePowerState()` returns early at `voltage <= 0.0f` and forces `POWER_STATE_NORMAL` ([Client .ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7391)).
2. **No `power` system-alarm notes** appear. Entering `CRITICAL_HIBERNATE` emits a `power` note with `se:true` ([Client .ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L6943)); none exist in the window.
3. **The daily report fired at 00:00.** Per Section 4.1, the client suppresses daily reports while in `CRITICAL_HIBERNATE`, so the device was demonstrably *not* hibernating.
4. **`pg:1` in the daily** means the P1 high-side power gate **enabled successfully** — the transmitter *is* being powered. The fault is therefore in *reading* the sensor, not in *powering* it.
5. **Every `sensor-fault` is paired with an `i2c-recovery` diag** (identical timestamps, incrementing `count`). The A0602 current-loop ADC is read over I2C via the framed Blueprint protocol in `readCurrentLoopSensor()` ([Client .ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5329)). When those framed reads fail (`validSamples == 0`) or return under-range, the function returns `NAN`, which trips `validateSensorReading()` ([Client .ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5083)) and publishes `sensor-fault`.

**Revised root cause for the alarm:** an **unstable I2C link to the A0602 current-loop expansion module** (bus recovery firing every cycle) is causing the framed current read to fail/under-range, which correctly escalates `sensor-fault`. The current-loop live-zero guard ([Client .ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5489)) is behaving as designed; it is reporting a genuine acquisition failure, not a fabricated value. The dashboard `ALARM: sensor-fault` is therefore **accurate**, and the priority is the **I2C hardware/bus reliability**, not the alarm display.

### 6.4 Finding C — 0.0 PSI is a reused daily sample colliding with a stale server `sensorMa`

Two facts combine on the dashboard:

- The `daily.qo` carries `lvl:0` with **no `ma`** and `st:"currentLoop"`. With `mA = 0` the calibration branch in `resolveLevel()` is skipped and the server trusts `lvl` directly, returning `0.0` ([Server .ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L13301)). The `lvl:0` itself is the **reused** value from `sampleMonitors()` after the failed validation ([Client .ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5617)); `buildSensorObject()` omits `ma` because `currentSensorMa < 4.0f` ([Client .ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5882)).
- Yet the server's own `email.qo` shows `sensorMa: 18.02`. The daily/alarm/diag notes in the window never carry `ma:18.02`, so this is a **stale** value the server stored from an older `data.qo` (now outside the window) and never cleared. 18.02 mA is the classic "pegged/stale register" symptom called out in the `readCurrentLoopSensor()` comments.

**Conclusion:** the dashboard shows `0.0 PSI` (from the reused daily `lvl:0`) while the server simultaneously holds an unrelated stale `sensorMa:18.02`. This is exactly the daily-report data-quality gap described in Section 4.10 — the faulted/reused sample is serialized as if valid, and the server has no mechanism to expire the old raw `ma`.

### 6.5 Re-verified current-HEAD code confirmations

| Claim | Verified location | Status |
|---|---|---|
| Opta guard makes Notecard voltage inert | `pollBatteryVoltage()` [#L6619](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L6619) | ✅ present, returns `false` |
| Notecard still wired as Source 2 (defense-in-depth gap) | `getEffectiveBatteryVoltage()` [#L7357](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7357) | ⚠️ still present (inert only by build flag) |
| Telemetry `v` has **no source tag** | `sendTelemetry()` [#L5935](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5935) | ⚠️ confirmed |
| Analog sensor has **no** live-zero guard; clamps to 0.0 | `readAnalogSensor()` [#L5304](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5304) | ⚠️ confirmed (Fix 2 still valid) |
| Current-loop **has** live-zero guard | `readCurrentLoopSensor()` [#L5489](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5489) | ✅ working as designed |
| Server accepts any positive `v` (no source/age/plausibility) | `handleTelemetry()` [#L11640](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11640) | ⚠️ confirmed |
| Daily prefers `solar.bv` but still accepts bare `v` | `handleDaily()` [#L12131](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12131) | ⚠️ confirmed |
| `vinVoltage` never expired before display | `sendClientDataJson()` [#L10007](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L10007) | ⚠️ confirmed |
| Dashboard VIN threshold is **inconsistent** | data card has none [#L2216](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2216); client card uses `>=6.0` [#L2260](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2260) | ⚠️ confirmed |
| Daily reconcile skip-list omits `sensor-fault` | `handleDaily()` [#L12113](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12113) | ⚠️ confirmed (only solar/battery/power skipped) |

### 6.6 Revised conclusions (live evidence vs. Sections 1–3)

The three dashboard symptoms are **three independent issues**, not one cascade:

1. **`VIN 4.69V`** → stale, non-expiring server metadata from an earlier firmware/config state. The current client sends **no** voltage. *Not* a live Notecard read, and *not* driving any hibernate.
2. **`sensor-fault`** → a **real, recurring I2C bus failure** with the A0602 current-loop module (perfect correlation with `i2c-recovery` diags). The alarm is accurate; the fix belongs at the I2C/hardware reliability layer, not the dashboard.
3. **`0.0 PSI`** → the reused daily `lvl:0` being trusted by the server, alongside a stale `sensorMa:18.02` the server never cleared.

This **supersedes** the Section 1–3 hypothesis that 4.69V forces `CRITICAL_HIBERNATE` which then unpowers the sensor. The live data shows the sensor is powered (`pg:1`), the device is not hibernating (daily fired, no `power` notes), and no voltage is being transmitted.

### 6.7 Additional & revised suggested fixes (for discussion — not yet applied)

These augment Section 5; they do not replace the still-valid items (Fix 1, Fix 2, Fix 6, Fix 7, Fix 8, Fix 9).

- **Fix 11 — Investigate the I2C bus to the A0602 as the primary `sensor-fault` cause.** The `i2c-recovery` cadence is the real signal. Check wiring/pull-ups/cable length/ground, the `currentLoopI2cAddress`, and whether the recovery routine is masking a hard fault. Correlate `count` growth with environmental factors (temperature, power). This is the highest-value next step for the alarm.
- **Fix 12 — Expire stale server `vinVoltage` on ingest age.** Independent of the client fix, `sendClientDataJson()` and both dashboard renderers should age-out `vinVoltage` using the existing `vinVoltageEpoch` (e.g., 48 h) so a value the client stopped sending cannot persist forever. This is what would have removed the 4.69V on its own.
- **Fix 13 — Expire/clear stale `sensorMa` the same way.** The server keeps `rec->sensorMa` (18.02) with no age check, so the daily email and any raw-mA display can show a value hours/days old. Tie it to `lastUpdateEpoch` or clear it when a daily report omits `ma`.
- **Fix 14 — Make the dashboard VIN threshold consistent and source-aware.** The main data card ([#L2216](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2216)) shows any `>0`, while the client card uses `>=6.0` ([#L2260](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2260)). Unify on the same rule (and ideally the `vs` source tag from Fix 7) so one page cannot show 4.69V while another hides it. This directly satisfies the requirement to *only* display voltage measured by the Vin divider or SunSaver MPPT.
- **Fix 15 — One-shot clear of the existing stale value.** Because the client no longer sends `v`, the 4.69V will not self-correct until a fresh valid reading arrives. Reboot the server, or add an admin "clear voltage" action, after the display guard (Fix 12/14) is deployed.

### 6.8 Recommended verification before any code change

1. **Confirm the deployed client FQBN** for past builds (`arduino:mbed_opta:opta`) to establish whether 4.69V was ever transmitted by a mis-built binary, or is purely legacy stored state. The live log (no `v` today) suggests the latter, but the build history determines whether Fix 1 (remove Source 2) is corrective or purely defense-in-depth.
2. **Probe the I2C bus health** on the Silas unit (Fix 11) — this is the actual driver of the recurring `sensor-fault`/`0.0 PSI`, and no server/voltage change will resolve it.
3. **Decide the voltage product rule** (Fix 7): emit `vs:"mppt"` / `vs:"vin-divider"` and have the server display voltage *only* when tagged, which is more robust than the numeric `>=6.0` heuristic.