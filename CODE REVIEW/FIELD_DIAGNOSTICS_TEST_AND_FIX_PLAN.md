# Field Diagnostics Test and Fix Plan

**Date:** 2026-06-23
**Scope:** Client and Server firmware configuration, monitoring, and communication fixes based on `CODE_REVIEW_06232026_FIELD_DIAGNOSTICSv2.md`.

## 1. Issues 1 & 3: Configuration Delivery and Verification
**Problem:** The client is not reporting battery voltage, likely due to an unapplied config (the RS-485 poller isn't running). 
**Action Plan:**
1. **Push Configuration:** Ensure the target field client (Cox Wellhead) is re-pushed with the `Solar + Modbus MPPT (RS-485)` configuration. Monitor for the `config_ack.qo`.
2. **Config Delivery Visibility:** Enhance the Server Management View to display the client's last applied configuration timestamp. This provides immediate "Applied" vs "Pending" states, identifying inbound sync staleness.
3. **Solar State Dashboard Badge:** Add a visual indicator to the Server UI ("Solar State: Disabled" / "Ok" / "Failing") powered by the new `scOk` flags in the v2.0.42 telemetry.
**Validation:** Verify that once the configuration is processed via the Notes pipeline, the dashboard accurately reflects the MPPT's heartbeat instead of silence.

## 2. Issue 4: RS-485 / SunSaver Driver Enhancements
**Problem:** Modbus communications are experiencing silent failures and potential transient timeouts on the RS-485 serial bus.
**Action Plan:**
1. **Enable Hardware Retry Loops:** Provide isolated, hardware-retry blocks inside the client's `TankAlarm_Solar.cpp` loop. E.g., Implement 3 consecutive retries 100ms apart before accepting a NACK / timeout and raising a `SOLAR_ALERT_COMM_FAILURE`.
2. **Enable Communication Alerts:** Transition `alertOnCommFailure` to default `true` whenever `solarCharger.enabled` holds true, giving the operator real-time feedback on link drops.
3. **Validation:** Use `firmware/sunsaver-rs485-windowed-probe` to sweep delays. Induce artificial electrical noise into the RS-485 bench bus to check that the retry logic correctly intercepts unformatted frames and suppresses false alarms.

## 3. Issue 2: A0602 Current-Loop Sensor Safeguards
**Problem:** A persistent hardware I2C issue results in `sensor-faults`. The firmware handles this by safely reusing the last good reading (`ru: 1`), but masks absolute failures from the daily logs and persists old faults on the Server dashboard. 
**Action Plan:**
1. **Daily Report Fault Injection:** Modify the daily roll-up logic in the client's `appendSensorDataToDaily`. If a sample interval consisted of 100% missed readings (`validSamples == 0`), explicitly record `0.0f` (or equivalent sentinel logic) to expose the outage instead of continuing to transmit a stale snapshot.
2. **Server-Side Automatic Unlatching:** Update the Server's anomaly engine (`TankAlarm-112025-Server-BluesOpta.ino`) to automatically clear a stale `sensor-fault` whenever fresh (`ru: 0`) and valid telemetry points arrive, replicating the existing `sensor-stuck` self-clearing.
3. **Validation:** Disconnect the A0602 I2C sensor on the bench configuration, verify the Daily Report sends clear failure data. Reconnect the harness, ensure valid data arrives with `ru: 0`, and check that the server resolves the `sensor-fault` immediately. 

## 4. End-to-End Release Pipeline 
- Patch, build, and deploy the new Server firmware updates handling the Solar Badge and the unlatching algorithms.
- Complete the A0602 trace analysis, resolving the raw I2C NACKs.
- Flash the client bench node, verify Modbus RS-485 logic.
- Conduct live regression trials comparing device events on Notehub versus Server dashboard visuals to confirm parity.

## 5. Copilot Review Notes and Suggestions
**Review date:** 2026-06-23

### Configuration Delivery / Applied-vs-Pending Visibility
- The basic config ACK plumbing already exists. The Client sends `config_ack.qo` with `c`, `st`, `cv`, `message`, and `epoch`; the Server stores `pendingDispatch`, `configVersion`, `lastAckEpoch`, `lastAckStatus`, `dispatchAttempts`, and `lastDispatchEpoch` in `ClientConfigSnapshot`.
- The most useful next step is UI/API visibility, not a new delivery protocol. Surface the existing snapshot fields in the Server Management / Config Generator flow so operators can see: `Pending Dispatch`, `Last Dispatch`, `Last ACK`, `ACK Status`, and whether the ACK `cv` matches the currently cached `configVersion`.
- `/api/client-config` already returns a `dispatch` object for a selected client, but `/api/clients?summary=1` skips config metadata. If the dashboard or site-management view needs Applied/Pending state without fetching each full config, add a compact per-client `cfg` summary to the thin response.
- The Client already refreshes non-transport solar config changes at runtime with `gSolarManager.setConfig()` and only restarts the RS-485 transport when enable/slave/baud/timeout changes. Keep this behavior; it directly addresses the older stale-runtime-solar-config risk.
- For `Solar + Modbus MPPT (RS-485)`, make the generated config explicit. The Server currently emits `solarCharger: { enabled: true }` from the UI, and the Client defaults missing `alertOnCommFail` to `false`. If comm-failure alerting should be enabled for MPPT installs, set `alertOnCommFail: true` in the generated config or make the Client parser default it to true when `solarCharger.enabled` is true and the field is absent.

### Solar / RS-485 Driver and Dashboard State
- The solar driver is already carrying several important hardware fixes: 9600/8N2 framing, `RS485.setDelays(0, 1200)`, timeout floor at 500 ms, FC03/FC04 fallback with cache, realtime register plausibility checks, and `SOLAR_COMM_FAILURE_THRESHOLD` before declaring the link down.
- Add retries carefully. A blanket 3x retry around every register block can multiply worst-case Modbus blocking time, especially with FC03/FC04 fallback. Prefer a bounded retry around the verified realtime block (`0x0008..0x000C`) or around the whole poll attempt, with watchdog kicks and a short inter-retry delay. Do not retry the unverified status/fault/daily register blocks in production builds.
- Coordinate the retry count with `SOLAR_COMM_FAILURE_THRESHOLD`. For example, three immediate retries per poll plus five failed polls before `communicationOk=false` gives up to fifteen low-level attempts before the operator sees failure. That may be right for noise immunity, but it should be intentional.
- The Client already emits `scOk` in telemetry when the SolarManager is enabled, and daily reports include `solar.commOk`. The Server does not currently consume `scOk` into the dashboard model. Add a small solar-state field to `ClientMetadata` or the sensor summary model so the dashboard badge can be driven from both telemetry (`scOk`) and daily (`solar.commOk`).
- Recommended badge logic: `Disabled` when the cached config has no `solarCharger.enabled`; `Ok` when enabled and recent telemetry/daily reports `scOk=1` or `solar.commOk=1`; `Failing` when enabled and recent telemetry/daily reports `scOk=0` or `solar.commOk=0`; `Unknown/Stale` when enabled but no solar state has arrived within a reasonable multiple of the configured poll interval.
- The bench probe exists at `firmware/sunsaver-rs485-windowed-probe/sunsaver-rs485-windowed-probe.ino`. Capture and keep the tuple that passes: baud/framing, slave ID, function code, register address, RX window, and delay profile. A good pass criterion is repeated valid Modbus CRC frames under 9600 8N2 and the production post-delay, not merely "not silent".
- Hardware notes worth preserving in the validation checklist: use the Morningstar MRC-1, confirm the RJ-11 MeterBus cable is fully seated, connect signal ground, and do not trust A/B labels alone. The Client README records Morningstar's inverted A/B convention; the wiring docs should be reconciled so the field instructions use one polarity convention consistently.
- For noise testing, avoid destructive bus faults. Prefer controlled, reversible perturbations: longer cable/stub, temporary termination/bias changes, adjacent noisy load, or current-limited injected disturbance. Record whether the retry layer suppresses transient failures without hiding a truly disconnected MRC-1.

### A0602 / Current-Loop Fault Visibility
- The current Client code already does more than the plan implies. `readCurrentLoopSensor()` uses the framed A0602 Blueprint protocol, validates response framing/CRC/channel, clears stale `currentSensorMa`, marks `sampleReused`, and returns `NAN` when `validSamples == 0`. `validateSensorReading()` then escalates non-finite readings into `sensor-fault` after the existing failure threshold.
- Because of that, avoid using a bare `0.0f` level as the daily-report sentinel. A numeric zero can look like a real empty tank and can corrupt 24-hour deltas and charts. Prefer explicit data-quality flags: keep `sf: 1`, `ru: 1`, omit `ma`, and optionally add a dedicated `dq: "fault"` or `miss: 1` field if the Server/dashboard needs a stronger signal.
- `buildSensorObject()` already includes `sf` and `ru`, and daily reports call `appendDailyMonitor()`, so the daily path can carry fault quality today. The Server should read those flags before updating history or clearing alarms.
- Add a shared Server helper such as `isFreshValidSensorSample(src, rec)` and use it in both `handleTelemetry()` and `handleDaily()`. It should require no `sf`, no `ru`, finite `lvl`, and for current-loop sensors a present, valid `ma >= 4.0` when raw mA is expected. Use that helper before recording history snapshots.
- Auto-unlatch stale diagnostic alarms only on fresh valid data. If `rec->alarmActive` and `rec->alarmType` is `sensor-fault` or `sensor-stuck`, a fresh valid sample should clear it, call `clearAlarmEvent()`, and mark the registry dirty. Do not clear on reused/stale daily rows.
- The existing Server self-clears `sensor-stuck` only in the narrow case where stuck detection has been disabled in config. That is useful, but it does not solve stale `sensor-fault` after a real reconnect. The planned automatic unlatch is still needed.

### Release / Test Additions
- Before field deployment, run one forced config push and verify all three states: Server dispatch becomes pending, Client ACK arrives with matching `cv`, and subsequent telemetry/daily shows the expected solar state.
- Add a regression where the Client reports `sf=1`/`ru=1` daily data while the A0602 is disconnected; the Server should display the fault and avoid recording a healthy-looking history point.
- Add a reconnect regression where three or more fresh valid current-loop samples arrive after the A0602 is restored; the Client should send `sensor-recovered`, and the Server should also self-clear any stale `sensor-fault` if the recovery note was lost.
- Treat release readiness as both firmware and operations: update the config generator defaults, dashboard/API summary, bench probe result, wiring instructions, and release notes together so a field technician can diagnose "not applied", "applied but solar failing", and "sensor failed but safely stale" from the UI alone.

## 6. Independent Code Review — Additional Findings and Recommendations
**Reviewer:** Copilot (Claude Opus 4.6)
**Date:** 2026-06-23
**Method:** Full source analysis of Client `.ino`, `TankAlarm_Solar.cpp/.h`, `TankAlarm_I2C.h`, and Server `.ino`.

### 6.1 Issue 1/3 — Config Delivery: Confirmed Gaps in Summary API

The plan correctly identifies that the config ACK plumbing is complete, but the `/api/clients?summary=1` endpoint (`sendClientDataJson()`, Server L9758) genuinely omits all config dispatch metadata. The summary path returns `cs[]` with sensor state, VIN voltage, firmware version, OTA state, and signal strength — but no `pd` (pendingDispatch), `cv` (configVersion), `ae` (lastAckEpoch), or `as` (lastAckStatus). These fields are only emitted in the full `/api/clients` response inside the `cfgs[]` array (Server L10056–10088).

**Concrete recommendation:** Rather than adding a full `cfgs[]` block to the summary response (which doubles its size), add a compact per-client `cfg` object inside each `cs[]` entry in summary mode:
```cpp
// Inside the per-client clientObj block, after OTA state:
if (cfgSnap) {
  JsonObject cfgSummary = clientObj["cfg"].to<JsonObject>();
  cfgSummary["pd"] = cfgSnap->pendingDispatch;
  if (cfgSnap->lastAckEpoch > 0.0) {
    cfgSummary["ae"] = cfgSnap->lastAckEpoch;
    cfgSummary["as"] = cfgSnap->lastAckStatus;
  }
  if (cfgSnap->configVersion[0] != '\0') {
    cfgSummary["cv"] = cfgSnap->configVersion;
  }
}
```
The `cfgSnap` pointer is already being looked up at Server L9877 for site name resolution, so this adds no extra lookup cost. This would be approximately 4–6 extra JSON fields per client in the summary — well within the existing `JsonDocument` budget.

### 6.2 Issue 4 — RS-485 Retry: Timing Risks with FC03/FC04 Fallback

The plan proposes 3 retries × 100ms between attempts. After reviewing `readRegistersWithFallback()` (Solar.cpp L62–97), there is a compounding concern:

- On a **cache miss** (`cachedFC == 0`), the function already performs two Modbus transactions (FC03 then FC04) back-to-back.
- On a **cached FC failure**, it clears the cache and tries the opposite FC on the same call — also two transactions.
- The Modbus timeout is floored at 500ms (Solar.cpp L148–149).

**Worst-case blocking time per retry:** 2 × 500ms = 1000ms (two failed FC attempts). With 3 retries on the realtime block (registers 0x0008–0x000C): 3 × 1000ms = **3 seconds** of blocking. The poll function already reads three register blocks (realtime, setpoints, and conditionally unverified status), so total worst-case could reach **9 seconds** per poll cycle.

The current watchdog timer is fed during the poll loop, but verify the watchdog window is > 9 seconds. If the Opta's WDT is 8 seconds (common default), the retry logic could trigger a reset on a fully unresponsive bus.

**Concrete recommendation:**
1. Only retry the realtime register block (0x0008–0x000C). The setpoint registers (0x0033, 0x0035, 0x0036) are static and don't need retries — a miss is harmless.
2. Feed the watchdog between retries, not just between register blocks.
3. Set `maxRetries = 2` (not 3) to keep worst-case blocking under 6 seconds.
4. Do NOT retry when `cachedFC == 0` (discovery phase) — the fallback logic is already effectively a retry.

### 6.3 Issue 4 — alertOnCommFailure Default: Config Generator Already Emits `false`

The config generator JS (Server L1980) builds `solarCharger: { enabled: true }` when the power source is `solar_modbus_mppt`, but **does not emit** `alertOnCommFail` at all. The Client parser (Client L3140) defaults missing `alertOnCommFail` to `false`:
```cpp
cfg.solarCharger.alertOnCommFailure =
  solarCfg["alertOnCommFail"].is<bool>()
    ? solarCfg["alertOnCommFail"].as<bool>()
    : false;
```

**Two options to fix this:**

- **Client-side default flip** (simpler, affects all future installs): Change the default in the Client parser from `false` to `true` when `solarCharger.enabled` is true. This is a one-line change but is a behavior change for all existing configs that omit the field.
- **Config generator explicit emission** (safer): Add `alertOnCommFail: true` to the generated config JSON when power source is `solar_modbus_mppt`. This makes the intent explicit and doesn't change behavior for devices with existing saved configs.

Recommend the config generator approach — it's explicit and doesn't silently change behavior for re-applied existing configs.

### 6.4 Issue 2 — sensor-fault Auto-Unlatch: The Real Gap

The plan is correct that the Server only auto-clears `sensor-stuck` when stuck detection is disabled (Server L11634–11670). But the actual gap is narrower than it appears:

The Client **already sends `sensor-recovered`** (Client L5086–5150) after 3 consecutive valid readings following a `sensor-fault`. The Server's `handleAlarm()` (Server L11814–11832) correctly processes this: it sets `rec->alarmActive = false`, calls `clearAlarmEvent()`, and preserves the `sensor-recovered` label. Additionally, the daily alarm reconciliation (Server L12018–12095) catches orphaned alarms by checking the daily report's alarm array against server-side state.

**The only unprotected scenario** is when:
1. The Client sends `sensor-recovered` via Notecard
2. The recovery note is lost (cellular gap, Notehub routing failure)
3. The next daily report's alarm array is also lost or arrives without the alarms section
4. No further alarm state change occurs

This is a real but narrow window. The proposed server-side auto-unlatch on fresh valid telemetry is the right fix. Specifically:

```cpp
// In handleTelemetry(), after the existing sensor-stuck auto-clear block (L11670):
if (rec->alarmActive && strcmp(rec->alarmType, "sensor-fault") == 0) {
  // Fresh valid telemetry with no sf/ru flags = sensor is healthy again.
  // The client would normally send sensor-recovered, but if that note was
  // lost, clear the stale fault here.
  bool hasSf = body["sf"] | 0;
  bool hasRu = body["ru"] | 0;
  float maVal = body["ma"] | 0.0f;
  bool isFreshValid = !hasSf && !hasRu && isfinite(rec->levelInches);
  // For current-loop sensors, also require valid mA
  if (strcmp(rec->sensorType, "currentLoop") == 0) {
    isFreshValid = isFreshValid && (maVal >= 4.0f);
  }
  if (isFreshValid) {
    rec->alarmActive = false;
    strlcpy(rec->alarmType, "clear", sizeof(rec->alarmType));
    clearAlarmEvent(clientUid, sensorIndex);
    markRegistryDirty();
  }
}
```

**Important:** The `sf` and `ru` flags are emitted by `buildSensorObject()` in telemetry but the Server's `handleTelemetry()` does not currently extract them. The telemetry handler reads `"ma"`, `"vt"`, `"lvl"`, `"st"`, etc., but skips `"sf"` and `"ru"`. These fields need to be read from the telemetry body for the auto-unlatch guard to work.

### 6.5 Issue 2 — Daily Report Sentinel: Why `0.0f` Is Dangerous

The plan (Section 3.1) proposes recording `0.0f` when `validSamples == 0`. The existing code already handles this better:

1. `readCurrentLoopSensor()` returns `NAN` when `validSamples == 0` (Client L5493–5496)
2. `validateSensorReading()` rejects `NAN` and increments `consecutiveFailures`
3. `sampleReused = true` is set; `currentInches` retains the last good value
4. `buildSensorObject()` emits `ru: 1` and `sf: 1` (if threshold crossed)
5. `currentSensorMa` is cleared to `0.0f`, so `ma` is omitted from the payload (the `ma >= 4.0f` guard in `buildSensorObject()` blocks it)

A `0.0f` level reading would be interpreted by the Server as "tank is empty," potentially triggering low-level alarms, corrupting 24-hour delta calculations, and producing false delivery indicators. The current approach of preserving the last valid reading with quality flags is correct.

**If you need a stronger fault signal in the daily report**, add a `miss` or `dq` integer field to the sensor object in `appendDailyMonitor()` rather than zeroing the level. For example:
```cpp
if (state.sensorFailed) {
  sensorObj["dq"] = 0;  // data quality: 0 = fault, 1 = valid
}
```

### 6.6 Solar Dashboard Badge — Server Implementation Path

The Server currently does not persist `scOk` from telemetry anywhere. `handleTelemetry()` (Server L11499–11750) does not extract the `scOk` field, and `ClientMetadata` has no solar state field.

**Minimum changes needed:**

1. **Add to `ClientMetadata`** (Server L482):
```cpp
int8_t solarCommOk;      // -1=unknown, 0=failing, 1=ok
double solarStateEpoch;   // when last updated
```

2. **Extract in `handleTelemetry()`** — after metadata lookup:
```cpp
if (body.containsKey("scOk")) {
  meta->solarCommOk = body["scOk"] | 0;
  meta->solarStateEpoch = epoch;
}
```

3. **Extract in `handleDaily()`** — from the `solar` sub-object:
```cpp
if (body.containsKey("solar")) {
  JsonObject solar = body["solar"];
  meta->solarCommOk = solar["commOk"] | 0;
  meta->solarStateEpoch = epoch;
}
```

4. **Emit in summary API** — inside the per-client `clientObj` block (Server L9880+):
```cpp
if (meta && meta->solarCommOk >= 0) {
  clientObj["sc"] = meta->solarCommOk;
  clientObj["sce"] = meta->solarStateEpoch;
}
```

5. **Dashboard JS** — derive badge from `sc` field presence and value, cross-referenced with the client's cached config to know if solar is even expected.

### 6.7 Risk: `readRegistersWithFallback()` Cache Invalidation on Noise

Under the current FC03/FC04 fallback logic (Solar.cpp L62–97), a single noise-induced Modbus CRC error on a cached-FC read will **clear the cache** (`cachedFC = 0`) and immediately try the opposite function code. If the device genuinely only supports one FC (e.g., the SunSaver MPPT uses FC03 for holding registers), this means:

- Noisy read on FC03 → cache cleared → FC04 attempted → FC04 fails (unsupported) → next poll starts from scratch with FC03 → FC03 succeeds → cache restored

This is one wasted poll cycle per noise event, which is acceptable. But with the proposed retry loop wrapping this function, a single noise event could trigger 2–3 unnecessary FC04 probes before FC03 succeeds on retry. Retries should be placed **outside** `readRegistersWithFallback()`, not inside it, so the cache has a chance to recover naturally.

### 6.8 A0602 I2C: PWM Gating Retry Already Exists

The plan mentions PWM power gating issues. The Client already retries `tankalarm_setPwm()` 3 times with 5ms delays (Client L5370–5391). If all 3 fail, it sets `lastPwmEnableOk = false`, `sampleReused = true`, and returns `NAN`. This is correct behavior, but the `pg: 0` field in telemetry is the only visibility into PWM failures. Consider adding a counter (`pwmFailCount`) to `ClientMetadata` on the Server so the dashboard can show "PWM gating has failed N times" — this would help diagnose chronic I2C bus issues vs. transient failures.

### 6.9 Test Plan Additions

Based on the code review, add these test cases:

1. **FC cache thrashing test**: On the bench, inject a single bad CRC response (e.g., by briefly disconnecting the RS-485 B line for < 100ms). Verify the FC cache recovers on the next poll without requiring a full restart. Count the number of wasted FC04 probes.

2. **Watchdog survival under retries**: With the MRC-1 powered off (worst case: all Modbus reads time out), verify the Client does not reset. Measure the actual blocking time per poll cycle with retries enabled. The WDT window must exceed this.

3. **Config push with solar badge visibility**: Push a `solar_modbus_mppt` config → verify the dashboard shows "Solar: Unknown" → wait for first telemetry with `scOk` → verify badge transitions to "Ok" or "Failing". Then power off the SunSaver → wait for `SOLAR_COMM_FAILURE_THRESHOLD` (5) failed polls → verify badge transitions to "Failing".

4. **Daily report with `sf=1` + `ru=1`**: Disconnect the A0602 sensor. Wait for the `sensor-fault` threshold (5 consecutive failures). Verify the daily report includes `sf: 1`, `ru: 1`, and **does not** include `ma` in the sensor object. On the Server, verify the history snapshot is **not** recorded as a healthy data point.

5. **Sensor-fault auto-unlatch race**: Disconnect the A0602 → wait for `sensor-fault` on Server → reconnect → block the `sensor-recovered` note (e.g., put the Notecard in airplane mode) → send a manual telemetry note with valid data (`sf: 0`, `ru: 0`, `ma: 12.5`) → verify the Server auto-clears the `sensor-fault` alarm.

6. **Daily alarm reconciliation coverage**: After the A0602 recovers and the Client's alarm state is clear, verify the next daily report's alarm array omits the sensor-fault entry and the Server reconciliation clears any orphaned alarm (Server L12018–12095).

### 6.10 Priority Ordering

Based on field impact and implementation complexity:

| Priority | Item | Effort | Risk if Deferred |
|----------|------|--------|-----------------|
| **P0** | Config push to Cox Wellhead + verify ACK | Ops only | Battery goes unreported indefinitely |
| **P1** | Server auto-unlatch `sensor-fault` on fresh valid telemetry | ~50 LOC | Stale dashboard alarms persist until manual clear |
| **P1** | Config generator: emit `alertOnCommFail: true` for MPPT configs | ~5 LOC | Silent RS-485 failures in the field |
| **P2** | Summary API: add per-client config status | ~15 LOC | Operator can't see Applied/Pending without full API call |
| **P2** | Solar dashboard badge (server metadata + API + JS) | ~80 LOC | No visibility into solar health without serial access |
| **P3** | RS-485 retry logic (realtime block only, 2 retries) | ~30 LOC | Transient noise causes missed polls (existing threshold absorbs this) |
| **P3** | Daily report `dq` field for sensor fault | ~10 LOC | Fault visibility exists via `sf`/`ru`; `dq` is incremental |
| **P4** | `isFreshValidSensorSample()` shared helper | ~40 LOC | Duplicated validation logic across telemetry/daily handlers |

## 7. Independent Verification & Additional Review
**Reviewer:** Copilot (Claude Opus 4.8)
**Date:** 2026-06-23
**Method:** Independent re-verification of every code claim in Sections 5–6 against the current working tree (Client `.ino`, Server `.ino`, `TankAlarm_Solar.cpp/.h`, `TankAlarm_Common.h`). Line numbers below reflect the tree as inspected today.

### 7.1 Verification Summary — the plan's foundations are accurate

Every substantive code claim in Sections 5 and 6 checks out against the live source. Confirmed:

| Claim | Status | Anchor (current tree) |
|-------|--------|-----------------------|
| Client emits `sf`/`ru`/`scOk`/`pg`, `ma` only when ≥4.0 | ✅ Confirmed | `buildSensorObject` Client L5874–5923; `scOk` set in `sendTelemetry` Client L5964 |
| `readCurrentLoopSensor()` returns `NAN` when `validSamples==0` | ✅ Confirmed | Client L5484–5495 |
| `CURRENT_LOOP_FAULT_MA = 3.6f`, escalates non-finite | ✅ Confirmed | Client L271, L5509 |
| PWM gate retried 3× with `lastPwmEnableOk` flag | ✅ Confirmed | Client L5363–5396 |
| Client runtime solar refresh via `setConfig()` for non-transport changes | ✅ Confirmed | Client L4708–4716 (I-24 fix) |
| Client config parser defaults `alertOnCommFail` to `false` | ✅ Confirmed | Client L3140; default also `false` at L2589 |
| Solar driver: 8N2, `setDelays(0,1200)`, 500 ms floor, FC03/FC04 fallback, plausibility clamps | ✅ Confirmed | `TankAlarm_Solar.cpp` L62–97, L135–169, L257–283 |
| Server `handleTelemetry()` does **not** read `sf`/`ru`/`scOk`/`pg` | ✅ Confirmed | Server L11499–11688 (only `ma`/`vt`/`lvl`/`st`/`un`/`cn`/`ot`/`mu`/`cap`) |
| Server self-clears `sensor-stuck` **only** when stuck detection disabled | ✅ Confirmed | Server L11634–11688 |
| Server has **no** `scOk`/`solarCommOk` consumer anywhere | ✅ Confirmed | No matches in Server `.ino` |
| Config generator emits `solarCharger:{enabled:…}` with **no** `alertOnCommFail` | ✅ Confirmed | Server L1980 |
| Summary API `cs[]` omits config-dispatch metadata | ✅ Confirmed | Client-side mapping at Server L2280 reads `c,n,s,k,a,at,u,l,ma,v,ve,fv,os,efv,od,tc,ts` — no `pd/cv/ae/as` |
| Client daily report carries `solar.commOk` | ✅ Confirmed | Client L6560 (`=0` fail branch), L6566 (`=1` healthy branch) |

**Bottom line:** the diagnosis is sound and the recommended direction (consume the data-quality flags the client already sends; surface config/solar state in the API and dashboard) is the right one. The corrections below are refinements, not reversals.

### 7.2 Corrections to Section 6 — risk assessments that need adjusting

**(a) Watchdog is 30 s, not 8 s — Section 6.2's "could trigger a reset" is overstated.**
`WATCHDOG_TIMEOUT_SECONDS` is `30` (`TankAlarm_Common.h` L119) and the Client arms the mbed watchdog at exactly that value: `mbedWatchdog.start(WATCHDOG_TIMEOUT_SECONDS * 1000)` → 30 000 ms (Client L1542–1543). On the STM32H747 the IWDG hard-caps near 32.7 s, so 30 s is the effective window. Even Section 6.2's *over*-estimated 9 s retry storm sits comfortably inside 30 s. Bounding retries is still good hygiene, but it is not a brick-risk mitigation at the current watchdog setting.

**(b) Default Modbus timeout is 1000 ms, not 200 ms — and the worst-case math should use it.**
`SOLAR_DEFAULT_TIMEOUT_MS = 1000` (`TankAlarm_Solar.h` L238). The inline "Default: 200ms" comment at Client L2582 is **stale** and should be corrected. `begin()` still floors to 500 ms, but the *default* a field config inherits is 1000 ms. So a failed transaction blocks for 1000 ms, not the 500 ms Section 6.2 assumed — doubling its per-retry estimate.

**(c) …but production only reads the realtime block (and setpoints once), so the 9 s figure does not apply to shipped firmware.**
In production builds `SOLAR_ENABLE_UNVERIFIED_REGISTERS` is **off**, so the status/fault/daily register blocks are `#ifdef`'d out (`TankAlarm_Solar.cpp` L290–319). Each poll reads at most **two** blocks: the realtime block (always) and the setpoints block (only while `!setpointsValid`, i.e. until the first good poll). Steady state is **one** block. Net effect of (b)+(c): a 3× retry wrapped around the realtime block is `3 × 2 × 1000 ms = 6 s` worst case (cache-miss doing FC03+FC04), not 9 s — and only on a fully dead bus. With `maxRetries = 2` and a watchdog kick between attempts it is ~4 s. **Recommendation stands** (retry realtime only, 2 attempts, kick between), but the justification is noise immunity, not avoiding resets.

**(d) Watchdog kick placement.** The current code kicks before the realtime read and after a *successful* read (`TankAlarm_Solar.cpp` L253–256, L262–266) but **not** between the internal FC03→FC04 fallback transactions, nor on the failure path. Any retry loop must add a kick on the failure/inter-retry path specifically, since that is the only path that can chain multiple full timeouts.

### 7.3 Bug in the proposed server code — null-metadata dereference

Section 6.6 step 2 proposes `meta->solarCommOk = body["scOk"] | 0;` "after metadata lookup." In `handleTelemetry()` the lookup is `ClientMetadata *meta = findClientMetadata(clientUid);` (Server L11675) — the **non-creating** variant, used immediately under an `if (meta && meta->vinVoltage > 0)` guard precisely because it can return `nullptr`. The proposed `scOk` extraction has no such guard and will dereference null on the **first** telemetry note from any client whose metadata has not yet been created. Same hazard for the `pg` counter in Section 6.8. Fix: use `findOrCreateClientMetadata(clientUid)` for the solar/PWM state, or wrap the writes in a null check. This matters most in exactly the field scenario this plan targets (a freshly (re)provisioned client reporting solar state before anything else has created its metadata).

### 7.4 The keystone change — extract the quality flags once

Three of the plan's items (6.4 auto-unlatch, 6.5 history gating, 6.6 solar badge, plus 6.8 PWM diagnostics) all depend on the **same** missing step: `handleTelemetry()` currently extracts none of `sf`, `ru`, `scOk`, `pg`. Do this **once**, early in the handler, into locals and into `ClientMetadata` (created via the find-or-create variant). Everything else composes on top:

```cpp
// Near the top of handleTelemetry(), after upsertSensorRecord succeeds:
const bool qSf   = doc["sf"] | 0;        // sensor in fault state
const bool qRu   = doc["ru"] | 0;        // value reused from prior cycle
const bool qFresh = !qSf && !qRu;        // authoritative acquisition
const bool hasScOk = doc["scOk"].is<int>();
// pg is per-sensor PWM-gate health; scOk is device-global solar link health.
```

Treat `scOk` as **device-global** (it rides on every per-sensor note but describes the one RS-485 link) and store it in `ClientMetadata`; treat `pg` and `sf`/`ru` as **per-sensor**. Storing `scOk` redundantly from each sensor's note is harmless (idempotent overwrite).

### 7.5 History gating is stronger than a `dq` field (refines 6.5)

Section 6.5 correctly rejects the `0.0f` sentinel. But note the concrete failure path: at the end of `handleTelemetry()` the server calls `recordTelemetrySnapshot(clientUid, siteName, sensorIndex, recordHeight, newLevel, vinVoltage)` (Server L11686) using the level returned by `resolveLevel()`, which **trusts the client's `lvl`** even when `ru:1`/`sf:1` are present. So today a reused/faulted reading is written into the history tier as a normal point, producing the flat stale line the plan describes. The cleanest fix is not (only) a `dq` flag the dashboard must learn to interpret — it is to **skip the snapshot entirely when `!qFresh`**:

```cpp
if (qFresh) {
  recordTelemetrySnapshot(clientUid, siteName, sensorIndex, recordHeight, newLevel, vinVoltage);
}
// else: leave a gap in the chart rather than a stale flat line.
```

A chart gap is self-explanatory to an operator; a flat line is actively misleading. Add the `dq` field as a secondary signal if desired, but gating the write is the high-value half.

### 7.6 Applied-vs-Pending — make the predicate explicit (refines 6.1 / Issue 1&3)

The ACK round-trip is fully wired: client sends `config_ack.qo` with `st`/`cv`/`epoch` (Client L4630–4660), server stores `pendingDispatch`/`lastAckEpoch`/`lastAckStatus`/`configVersion`/`lastDispatchEpoch` in `ClientConfigSnapshot` (Server L1017–1019 et al.). What's missing is a single derived state the UI can show. Define it once, server-side, so the dashboard and the management view agree:

- **Pending** when `pendingDispatch == true` **or** `lastAckEpoch < lastDispatchEpoch` (a newer config went out than the last ACK acknowledges).
- **Applied** when `lastAckStatus == "applied"` **and** the ACK's `cv` equals the cached `configVersion`.
- **Failed** when `lastAckStatus == "failed"`.
- **Unknown** when no dispatch has occurred yet.

Surface that one enum plus `lastAckEpoch` in the compact `cs[]` summary entry (per 6.1) so operators get Applied/Pending without the full `/api/clients` payload.

### 7.7 `alertOnCommFail` default — endorse the generator fix, and note the three client paths

Concur with Section 6.3: change the **config generator** (Server L1980) to emit `alertOnCommFail: true` for `solar_modbus_mppt`. Reason to prefer it over the client-side default flip: the client reads this field in **three** places — defaults (Client L2589), flash-load parser (L3140), and the runtime apply path (L4686) — and a client-side flip risks changing behavior for already-saved configs that omit the field. The generator change is one line, explicit, and only affects newly generated configs. Optionally expose it as a UI checkbox defaulted-on for MPPT so an operator can still opt out of a noisy link.

### 7.8 Hardware corroboration — SunSaver register map and MeterBus path

I verified the firmware's verified-register choices against the Morningstar SunSaver MPPT MODBUS model, and they line up:

- The "verified" realtime block is addressed correctly: `SS_REG_BATTERY_VOLTAGE = 0x0008` is `adc_vb_f`, `0x0009 = adc_va_f`, `0x000B = adc_ic_f` (`TankAlarm_Solar.h` L47–50) — these are the filtered live ADC logical addresses in Morningstar's published register map, and the in-code scaling (`V = raw·100/32768`, `I = raw·79.16/32768`) matches the documented 12 V SunSaver MPPT scaling constants. Confidence in the realtime/setpoint reads is well-founded; the team's caution about the status/fault/daily addresses (`0x001B`, `0x002B`, `0x0034`, `0x003D`) is the correct posture until bench-confirmed for the specific firmware revision.
- The transport assumptions in the driver are consistent with the MRC-1 MeterBus→RS-485 path: 9600 8N2, a full-character post-TX delay before DE drop, and a ≥500 ms reply window. These are exactly the symptoms an RJ-11 MeterBus bridge produces (slow reply assembly, last-byte corruption on too-short DE hold). One field-doc reconciliation item the plan already flags is worth repeating: **Morningstar's A/B labeling is inverted** relative to most RS-485 gear, so wiring instructions must commit to one polarity convention and state it explicitly.
- For the noise/retry bench work, the existing `firmware/sunsaver-rs485-windowed-probe` sketch is the right tool; capture and freeze the passing tuple (baud/framing, slave ID, function code, register, RX window, delay profile) as a regression fixture so a future driver change that breaks framing is caught without hardware.

### 7.9 Revised priority/sequencing

One reorder: promote the **field-flag extraction** (7.4) to the top of the firmware work because it is the shared dependency for auto-unlatch, history gating, the solar badge, and PWM diagnostics. Doing it first means each downstream item is a small composition rather than its own extraction.

| Priority | Item | Note vs. Section 6.10 |
|----------|------|------------------------|
| **P0** | Config push to Cox Wellhead + verify ACK | Unchanged (ops-only) |
| **P1** | **Extract `sf`/`ru`/`scOk`/`pg` in `handleTelemetry()` (find-or-create metadata)** | New keystone; unblocks P1–P2 below |
| **P1** | Gate `recordTelemetrySnapshot()` on `qFresh` (7.5) | Strengthens 6.5; prevents stale history points |
| **P1** | Server auto-unlatch `sensor-fault` on fresh valid telemetry | Now a small add on top of the keystone |
| **P1** | Config generator: emit `alertOnCommFail: true` for MPPT | Unchanged |
| **P2** | Summary API: per-client config status + explicit Applied/Pending enum (7.6) | Adds the derived-state predicate |
| **P2** | Solar dashboard badge (driven by `ClientMetadata.solarCommOk` + daily `solar.commOk`) | Fix null-deref (7.3) |
| **P3** | RS-485 retry: realtime block only, **2** retries, kick between attempts | Use 1000 ms default in the timing budget (7.2b); kick on failure path (7.2d) |
| **P3** | Fix stale "Default: 200ms" comment at Client L2582 | Doc-only accuracy fix |
| **P4** | `isFreshValidSensorSample()` shared helper | Becomes trivial once 7.4 lands |

### 7.10 Additional test cases

7. **First-telemetry null-metadata test:** From a brand-new (or freshly reprovisioned) client that has never reported, send a telemetry note carrying `scOk`. Verify the server stores solar state without crashing — this exercises the find-or-create fix from 7.3 and would have caught the proposed null-deref.
8. **Telemetry/daily solar-state agreement:** With the link healthy, confirm `scOk=1` on telemetry and `solar.commOk=1` on the daily agree, and that the badge does not flap when the two arrive close together. Then drop the link and confirm both converge to "Failing" without oscillation.
9. **History-gap (not stale-line) test:** Disconnect the A0602, let `ru:1`/`sf:1` flow, and verify the history tier shows a **gap** for that interval rather than a flat continuation of the last value (validates 7.5). Reconnect and confirm fresh points resume and the gap is bounded to the outage.
10. **Watchdog budget with real defaults:** Run the dead-bus retry test (Section 6.9 #2) using the **actual** 1000 ms default timeout and the realtime-only steady-state path, and record measured worst-case blocking. Confirm it stays well under the 30 s window with kicks between retries.
