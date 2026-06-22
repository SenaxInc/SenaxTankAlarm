# Code Review: Deferred Items from the 2026-06-22 Voltage/Alarm Review

## Date: 2026-06-22
## Scope
Companion to [CODE_REVIEW_06222026_VOLTAGE_ALARM.md](CODE_REVIEW_06222026_VOLTAGE_ALARM.md). That review's
code fixes shipped in **v2.0.0** (commit `2d8de11`). This document covers the items that were
**deliberately deferred** because they are hardware, configuration-default, or operational in nature
rather than straightforward code bugs:

| # | Item | Nature | Status after v2.0.0 |
|---|------|--------|---------------------|
| 11 | I2C bus to the A0602 current-loop module is the real `sensor-fault` driver | Hardware + observability | Open — needs bench work; code can only diagnose/mitigate |
| 4  | Config generator battery-type default is `agm` (enables battery monitor) | Config default | Low risk now (v2.0.0 gated the Notecard voltage source out) but still worth changing |
| 5 / 15 | One-shot clear of the stale dashboard value | Operational | Mostly self-healing via the v2.0.0 serve-gate; optional admin action remains useful |

**This document is review-only. No code was changed. All snippets below are suggestions.**

---

## 1. Fix 11 — The recurring `sensor-fault` is an I2C bus problem, not a firmware logic bug

### 1.1 What the code already does correctly (do NOT rewrite this)
The current-loop acquisition path is already hardened and is **reporting a genuine fault**, not
fabricating one. Verified at HEAD:

- `readCurrentLoopSensor()` ([client .ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5329)):
  - Enables the **P1 high-side power gate** (`tankalarm_setPwm`) with retry; if the gate fails to
    enable it logs `WARNING: Failed to enable sensor power gating`, drives P1 off, sets
    `sampleReused`, and **returns `NAN`** instead of reading a floating channel (v1.9.22).
  - **(Re)configures the A0602 ADC channel** via the framed Blueprint protocol on every powered
    read (v1.9.23), because the channel loses config when P1 is switched off — this killed the old
    "constant ~18 mA / 43.8 psi stale-register" symptom.
  - Does a **priming discard read** + a **300 ms settle floor** (`CURRENT_LOOP_GATED_SETTLE_MS`,
    [#L274](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L274))
    matching the proven `P1_Transistor_Gating_Test`, then averages 4 CRC-validated framed samples.
  - Returns `NAN` on `validSamples == 0` or under-range (`< CURRENT_LOOP_FAULT_MA`), which
    `validateSensorReading()` escalates to `sensor-fault` after `SENSOR_FAILURE_THRESHOLD` (5).
- `tankalarm_recoverI2CBus()` ([TankAlarm_I2C.h](../TankAlarm-112025-Common/src/TankAlarm_I2C.h#L69)):
  `Wire.end()` -> toggle SCL 16x -> STOP -> `Wire.begin()`, increments `gI2cBusRecoveryCount`.
- Escalation ladder in `sampleMonitors()`
  ([#L1856](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L1856)):
  dual (Notecard+sensor) failure and "all current-loop sensors failing" both call `recoverI2CBus()`
  and `logI2CRecoveryEvent()`.

**Conclusion:** the 1:1 pairing of `alarm.qo {y:sensor-fault}` with `diag.qo {ev:i2c-recovery}`
(each ~2.5–3 h, `count` climbing) means the framed reads are failing on the wire and the bus is being
reset every cycle. The fault is **physical / electrical**, and the highest-value work is a hardware
investigation plus better remote observability so it can be diagnosed without a site visit.

### 1.2 Hardware checklist (bench / field — no code)
1. **Pull-ups.** Confirm the SDA/SCL pull-ups are present and sized for the bus capacitance. With the
   Notecard **and** the A0602 expansion on the same bus plus cable length, weak/again-too-weak
   pull-ups cause marginal edges -> NACK/CRC failures. Try stronger pull-ups (e.g. 2.2k–4.7k) if the
   current values are 10k.
2. **Cable length / routing.** The A0602 ribbon/expansion run length and proximity to the 4-20 mA
   loop wiring and relays (EMI) matters. Keep I2C away from switched loads; add ground returns.
3. **Ground integrity.** A floating or noisy ground between the Opta and the expansion is a classic
   intermittent-NACK source.
4. **Address / contention.** Confirm `currentLoopI2cAddress` (default `0x64`) matches the snapped
   A0602 and does not collide; the Blueprint auto-addressing assigns expansion addresses, so a
   re-snap or firmware on the expansion can move it.
5. **Power.** The P1-gated transmitter inrush at warmup can sag a shared rail; verify the 24 V (or
   loop supply) holds under the gated load, and that warmup (`pwmGatingWarmup`) is long enough.
6. **Correlate `count` with environment.** Pull the `diag.qo` `count`/`i2c_errs` series against time
   of day / temperature / charging state. A thermal or solar-charge correlation points at a marginal
   connector or supply rather than software.

### 1.3 Suggested CODE changes — observability first (low risk, high diagnostic value)
The current `diag.qo` only carries `trigger`, `count`, `i2c_errs`, `t`
([logI2CRecoveryEvent](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5027)).
To diagnose remotely, enrich it and the per-sensor fault note so we can see *how* the read failed.

**Suggestion 11-A — richer `i2c-recovery` diag note.** Add the last current-loop read context:

```cpp
// in logI2CRecoveryEvent(), after doc["i2c_errs"] = gCurrentLoopI2cErrors;
doc["validSamples"] = gLastCurrentLoopValidSamples;   // new global, set in readCurrentLoopSensor()
doc["pwmOk"]        = gLastPwmEnableOk ? 1 : 0;        // already tracked per-monitor; surface last
doc["lastMa"]       = roundTo(gLastCurrentLoopMa, 2);  // last raw mA seen (even if rejected)
doc["clkHz"]        = (uint32_t)I2C_BUS_CLOCK_HZ;       // see 11-C
```

This makes the difference between "P1 never enabled" (wiring/power), "frames CRC-fail"
(signal integrity), and "under-range mA" (loop/transmitter) visible in Notehub without serial.

**Suggestion 11-B — count consecutive recoveries and escalate to a distinct alarm.** Today every
recovery is the same `sensor-fault`. A bus that needs recovery *every cycle* is a different
operational state than an occasional glitch. Track consecutive recoveries and, past a threshold,
emit a one-shot `y:"i2c-bus-fault"` (or set `od`/detail) so the dashboard can show "bus unstable"
distinctly from a single bad reading. Reset the counter on a clean read.

**Suggestion 11-C — make the I2C clock a tunable and consider 50 kHz on this bus.** The recovery
routine and `Wire.begin()` keep the default 100 kHz. On a long/marginal expansion bus, **lowering to
50 kHz** dramatically improves robustness at negligible throughput cost (these are tiny transfers).

```cpp
// TankAlarm_Config.h (new)
#ifndef I2C_BUS_CLOCK_HZ
#define I2C_BUS_CLOCK_HZ 100000UL   // try 50000UL on marginal A0602 buses
#endif

// after every Wire.begin() (setup + tankalarm_recoverI2CBus):
Wire.setClock(I2C_BUS_CLOCK_HZ);
```

Pair this with a config-pushable override so the rate can be dropped on a problem unit without a
re-flash.

**Suggestion 11-D — surface "P1 enable failed" as telemetry, not just serial.** The
`WARNING: Failed to enable sensor power gating` path
([#L5382](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5382))
returns `NAN` (correct) but only logs to serial. Since `buildSensorObject()` now emits `pg`
(power-gate result) and the v2.0.0 `sf`/`ru` flags, also count gate-enable failures into the diag
note so a remote operator can tell "transmitter never powered" from "bus noisy".

**Suggestion 11-E — do NOT mask the fault by widening acceptance.** Explicitly call out the
anti-pattern: it would be wrong to "fix" the dashboard by accepting under-range mA or reused levels.
v2.0.0 already prevents the *display* from lying (`sf`/`ru` flags, server level-trust gate); the
remaining work is making the bus reliable, not making the firmware less strict.

### 1.4 Bench diagnostics to run (existing tools, no new code)
- `firmware/.../P1_Transistor_Gating_Test` — validated reference; on the Silas unit it should read
  ~1.6 psi / ~4.5 mA when gating works. If it fails on the bench too, it is hardware.
- `Blueprint_CH0_Test` — cross-checks via the official `Arduino_Opta_Blueprint` `exp.pinCurrent()`.
- `I2C_Utility` / `tankalarm_scanI2CBus()` — confirm the A0602 answers reliably at its address under
  repeated scans (and that nothing unexpected shows up).
- Capture client serial @115200 during a fault window and look for `Failed to enable sensor power
  gating` vs `A0602 current-ADC channel config NACK` vs CRC failures — each points at a different
  layer.

---

## 2. Fix 4 — Config generator should not default the battery monitor on

### 2.1 Current behavior
The Client Console battery dropdown defaults to **AGM** and the collector treats anything but `none`
as "battery monitor enabled":

```html
<!-- server .ino #L1888 -->
<select id="batteryType" ...>
  <option value="none">None (no battery)</option>
  <option value="agm" selected>AGM (Sealed)</option>
  ...
```
```js
// server .ino #L1999 (collectConfig)
const btStr = document.getElementById('batteryType').value || 'agm';
batteryConfig: { enabled: btStr !== 'none', batteryType: btEnum, ... }
```
And the reverse-map fallback is also `agm`
([#L2019](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2019)).

### 2.2 Why it mattered, and why it is lower-risk now
Historically `batteryMonitor.enabled = true` is what let the client poll `card.voltage` and ingest the
~5 V Notecard rail (the 4.69 V / false-hibernate chain). **v2.0.0 compile-gated that source out on
Opta** in `getEffectiveBatteryVoltage()`, so an `agm` default can no longer resurrect the 5 V rail on
Opta hardware. This is therefore a **cleanliness / least-surprise** change, not a bug fix.

### 2.3 Suggested change
For an Opta deployment with no MPPT and no Vin divider, the honest default is **None**:

```html
<option value="none" selected>None (no battery)</option>
<option value="agm">AGM (Sealed)</option>
```
and change the two `|| 'agm'` fallbacks (`#L1902`, `#L1935`, `#L1999`) and the reverse-map default
(`#L2019`) to `'none'`. Keep the `validatePowerCombination()` guard so a solar/MPPT selection still
forces a sensible battery/Vin pairing.

**Caveat:** verify this does not regress the **solar-only** detection
(`isSolarOnly = btStr==='none' && psRaw.startsWith('solar')`) or the battery-failure-fallback logic —
defaulting to `none` changes `isSolarOnly` for a fresh form. Recommend defaulting battery to `none`
**and** power source to `grid` together so a blank form describes the simplest (grid, no battery)
install, and the operator opts in to solar/battery explicitly.

### 2.4 Operational note (no code)
For the live Silas unit specifically: confirm its pushed config and, if it has `batteryConfig.enabled`
true with no real measurement path, re-push with battery = None. With v2.0.0 this is optional (the
client no longer mis-reports), but it keeps the config honest.

---

## 3. Fix 5 / 15 — Clearing the stale dashboard value

### 3.1 Now mostly self-healing
v2.0.0's serve-gate in `sendClientDataJson()` only emits `vinVoltage` when it is **>= 6.0 V AND
< 48 h old**, and `handleDaily()` clears a stale `sensorMa` when a current-loop daily omits raw `ma`.
So the stale 4.69 V and 18.02 mA **age off the dashboard on their own** once the server runs v2.0.0 —
no manual action strictly required. (A server reboot still clears them instantly because the metadata
is in RAM and re-hydrated from cache.)

### 3.2 Optional: an explicit admin "clear voltage/mA" action
For operators who do not want to wait out the 48 h window, a small authenticated endpoint is cheap and
mirrors the existing `/api/ota/expect` pattern (session-auth via `requireValidPin`). Sketch:

```cpp
// route registration (near the other /api/... POST routes)
// POST /api/clients/clear-voltage  body: { pin, client }
static void handleClearVoltagePost(EthernetClient &client, const String &body) {
  JsonDocument req; if (deserializeJson(req, body)) { sendJsonError(client, 400, "bad json"); return; }
  if (!requireValidPin(req["pin"] | "")) { sendJsonError(client, 403, "bad pin"); return; }
  const char *uid = req["client"] | "";
  ClientMetadata *meta = findClientMetadata(uid);   // existing helper
  if (!meta) { sendJsonError(client, 404, "unknown client"); return; }
  meta->vinVoltage = 0.0f;
  meta->vinVoltageEpoch = 0.0;
  gClientMetadataDirty = true;
  // also clear stale raw mA on that client's sensor records
  for (uint8_t i = 0; i < gSensorRecordCount; ++i) {
    if (strcmp(gSensorRecords[i].clientUid, uid) == 0) gSensorRecords[i].sensorMa = 0.0f;
  }
  gSensorRegistryDirty = true;
  sendJsonOk(client);
}
```

Add a small "Clear VIN" button on the site-config client card (next to the existing Refresh / Remove
Client buttons) that POSTs to it. **Low priority** given the self-healing gate; include it only if the
operator asks for an instant override.

### 3.3 Optional: source tag persistence (stronger than the 6.0 V heuristic)
v2.0.0 trusts the client's `vs` tag at *ingest* but the *serve* gate falls back to the `>= 6.0 V`
plausibility heuristic (it does not persist the source). A genuinely low but real battery
(e.g. an MPPT pack at 5.8 V) would be ingested yet hidden at serve. If that edge case matters, add a
`char vinVoltageSource[12]` to `ClientMetadata`, store the `vs` on ingest, and at serve show the value
when `vinVoltageSource` is non-empty **and** recent (drop the `>= 6.0` floor for tagged sources).

**Caution:** `ClientMetadata` is persisted to LittleFS (`save/loadClientMetadataCache`). Adding a
field requires bumping/handling the cache format so an old on-disk cache does not misalign — do it the
same defensive way `loadConfigFromFlash` tolerates schema drift (keep present fields, default the new
one). Given the device has no battery-measurement hardware today, this is **optional / future**.

---

## 4. Summary of suggested changes (priority order)

| Priority | Item | Suggestion | Risk |
|---|---|---|---|
| **P0 (hardware)** | 11 | Bench-diagnose the A0602 I2C bus (pull-ups, cable, ground, power, address); run `P1_Transistor_Gating_Test` / `Blueprint_CH0_Test` | n/a (no code) |
| **P1** | 11-A/B/D | Enrich `diag.qo` (validSamples, pwmOk, lastMa, clkHz) + distinct `i2c-bus-fault` escalation so the cause is visible remotely | Low |
| **P1** | 11-C | Make I2C clock tunable; try 50 kHz on the marginal bus; `Wire.setClock()` after every `Wire.begin()` | Low |
| **P2** | 4 | Default config-generator battery to `None` (+ power source `grid`); fix `||'agm'` fallbacks | Low (verify solar-only logic) |
| **P3** | 5/15 | Optional `/api/clients/clear-voltage` admin action + button (self-heals already) | Low |
| **P3 (future)** | 7-ext | Persist `vinVoltageSource` for source-aware display instead of the `>=6.0 V` heuristic | Med (cache format) |

### Explicitly NOT recommended
- Do **not** loosen `readCurrentLoopSensor()` / `validateSensorReading()` acceptance to make the
  dashboard read non-zero — that would re-mask a real acquisition failure. v2.0.0 already stops the
  *display* from lying; the remaining work is bus reliability.
- Do **not** rebuild the I2C recovery or current-loop read path — they are correct and already encode
  the v1.9.19/1.9.22/1.9.23 lessons.

## 5. Verification before any of these are merged
1. Bench-confirm the A0602 bus behavior first (Section 1.2/1.4) — it determines whether 11-C (clock)
   is corrective or just defensive.
2. Any client change (11-A/B/C/D) is client-only: compile with `-DTANKALARM_DFU_MCUBOOT`, deploy via
   USB or OTA, watch `diag.qo` for the new fields.
3. Any server change (4, 5/15) is server-only: compile, flash via USB (COM3), confirm the dashboard
   and the existing post-DFU Ethernet relink.
4. Keep these as separate small commits (one per fix) rather than a bundle, so a regression is easy to
   bisect.
