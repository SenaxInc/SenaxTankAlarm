# TankAlarm Repository Review — 2026-09-08

**Baseline:** `SenaxInc/SenaxTankAlarm`, branch `master`, HEAD `4f30a7f`, firmware v2.2.14.
**Reviewed from:** a clean `git worktree` of HEAD, not the working tree (see section 1).
**Method:** three production sketches and the shared library compiled locally with warnings enabled; the 14 embedded server pages and 2 viewer pages extracted from the HEAD source and rendered in a real browser against a loopback fixture server whose responses were built from the server's own handler code plus a captured live-device response; 19 review lenses run in parallel over the sources, every candidate finding then re-checked by two independent adversarial verifiers reading the code afresh, with a third verifier on disagreement.

---

## 1. Read this first: the working tree holds June code that would revert three releases

This is not a finding in the codebase; it is a hazard in the checkout, and it is the single most consequential thing in this review.

Seven tracked files were modified in the working tree when the review started. Each modified file is **byte-identical to an older commit**, so these are reversions, not new work:

| Working-tree file | Identical to | Dated | What committing it would undo |
|---|---|---|---|
| `TankAlarm-112025-Client-BluesOpta.ino` | `e00a25a` | 2026-06-26 | v2.1.0 through v2.1.6: DAC loop power, the v2.1.2 milliamp scale fix, v2.1.3 clipping detection, v2.1.4 I2C recovery repair, v2.1.6 SMS pipeline hardening and offline alarm buffering |
| `TankAlarm-112025-Common/src/TankAlarm_I2C.h` | `fea0824` (v2.0.50) | 2026-06-25 | The DAC loop-power configure/read helpers, PWM ramp helpers, expansion LED control |
| `TankAlarm-112025-Common/src/TankAlarm_Solar.cpp` | `5b68a31` | 2026-06-26 | The v2.1.5 setpoint-probe give-up and the v2.1.3 nominal-bank classification |
| `TankAlarm-112025-Common/src/TankAlarm_Config.h` | `5768f5c` (v2.0.46) | 2026-06-24 | The current threshold documentation block |
| `.github/workflows/arduino-ci-112025.yml` | `dd7f5e3` | 2026-06-29 | Re-adds the FTPSclientOPTA overlay step; the `vendor/FTPSclientOPTA-overlay` directory it copies from was deleted in `4d1938d` when upstream shipped 0.3.0, so both jobs would now emit the "overlay missing" warning on every run |
| Two OTA review documents | one matches `cb20b3c`, one matches nothing | 2026-06-15 | Document content |

Compounding this: `C:\GitHub\Arduino\libraries\TankAlarm-112025-Common` is a **directory junction pointing at the repository's own Common folder**. Any local `arduino-cli` build therefore compiles whatever is in the working tree, not what is committed. The three review documents already in this folder dated 2026-09-08 describe these reversions as "existing local changes" (their W-01 through W-04) and at least one of their findings — the claim that the dashboard Update button has no client consumer — is an artifact of reading the June client, which predates the `telemetry_request.qi` handler that exists at HEAD.

**Recommendation:** do not `git add -A`. Decide deliberately whether these seven files should be restored from HEAD (`git checkout -- <paths>`) or whether some genuinely represent intended work. This review deliberately leaves them untouched and reviews HEAD instead.

---

## 2. Build verification

All three sketches compile clean at HEAD with `arduino-cli 1.4.1`, core `arduino:mbed_opta 4.5.0`, ArduinoJson 7.4.3, Common supplied via `--library`, `--warnings all`.

| Sketch | Flash | Static RAM | Result |
|---|---|---|---|
| Client (`-DTANKALARM_DFU_MCUBOOT`) | 384,940 B (19%) | 81,008 B (15%) | exit 0 |
| Server | 1,013,348 B (51%) | 360,720 B (**68%**, 162,904 B left for locals and heap) | exit 0 |
| Viewer | 318,000 B (16%) | 82,784 B (15%) | exit 0 |

Warnings worth acting on, all in project code:

- `Client:3161` `-Wtype-limits`: `(dur > 86400) ? (uint16_t)86400 : dur` where `dur` is `uint16_t` — the clamp is dead code. See M-04.
- `Client:3341` `-Wtype-limits`: `sc.modbusBaudRate != 115200` is always true because the field is `uint16_t`. See L-47.
- `Client:9559` `-Wextra`: enumeral and non-enumeral types mixed in a conditional expression.
- Dead code the linker keeps: `readCurrentLoopMilliamps` (Client:5418), `freeRam` (Client:10291, Viewer:382), `gStartupScanReported` (Client:1023), `dfuKickWatchdog` (Viewer:270), `sendChunk` (Server:10035), `tankalarm_performIapUpdate` and `firstSectorSize` (`TankAlarm_DFU.h:317`, `:436`).
- About 40 `containsKey()` deprecation warnings from ArduinoJson 7.4.3 (Server 10643-10718, 12486, 12843, 13252, 18372-18398; Client 3200-3202). These still compile but the API is slated for removal; `doc["k"].is<T>()` is the replacement.
- Two `-Wformat-truncation` warnings (`Server:12350`, `Viewer:1838`) are benign — both buffers are bounded and the truncation path is handled.

The server's 68% static RAM is the number to watch: several findings below (H-23, M-41, L-23) involve handlers that build a whole response, a whole month file, or a whole manifest inside the remaining 162 KB.

---

## 3. Findings at a glance

118 findings were confirmed by two independent verifiers each, and 7 more were established directly during this review from build warnings, live rendering, and the device data committed in this repository (marked with a dagger in section 6). Two candidates were refuted and two remained split (section 9); fifteen more were not reached before the verification run hit a usage limit and are listed as leads in Appendix A. Severity is the verifiers' independent rating, not the finder's.

- **High (27)** — plausible missed or false alarm, wrong physical output, loss of durable state, a security exposure, or a page control that does nothing.
- **Medium (49)** — bounded correctness, reliability or operator-facing defect, or a measurable efficiency win.
- **Low (49)** — minor, cosmetic, or defensive.

Items marked *(prior)* were reported in an earlier review in this folder and are confirmed still present at HEAD.

---

## 4. The firmware findings that matter most

The full High list is in section 6. Six of them deserve to be read in full, because they concern which physical pin the firmware drives, whether a device is visible at all, and data the operator can never get back.

### 4.1 Relay indices do not address the relays (H-12)

```cpp
static int getRelayPin(uint8_t relayIndex) {
#if defined(ARDUINO_OPTA)
  if (relayIndex < 4) {
    return LED_D0 + relayIndex;
  }
#endif
  return -1;
}
```

The expression assumes the four LED constants are consecutive. In `variants/OPTA/pins_arduino.h` they are **7, 9, 8, 153**, so `LED_D0 + relayIndex` yields 7, 8, 9, 10:

| relayIndex | computed pin | constant actually hit | intended |
|---|---|---|---|
| 0 (Relay 1) | 7 | `LED_D0` | `LED_D0` (7) — correct |
| 1 (Relay 2) | 8 | `LED_D2` | `LED_D1` (9) — **relay 3's pin** |
| 2 (Relay 3) | 9 | `LED_D1` | `LED_D2` (8) — **relay 2's pin** |
| 3 (Relay 4) | 10 | `D10` (PI_2), neither relay nor LED | `LED_D3` (153) — **never driven** |

Any config with relay-mask bit 1, 2 or 3 set is affected. An alarm that should energise Relay 2 energises Relay 3's output and vice versa; Relay 4 is dead while pin 10 is driven as an output. The local alarm path and the remote relay-command path both call `getRelayPin`, and `clearAllRelayAlarms` releases the same wrong pins, so the mis-mapping is self-consistent and never surfaces as an error. `RELAY_CONTROL.md` documents "Bit 1 (value 2) = Relay 2 (D1)", which is not what runs.

```cpp
static int getRelayPin(uint8_t relayIndex) {
#if defined(ARDUINO_OPTA)
  static const int kRelayPins[4] = { LED_D0, LED_D1, LED_D2, LED_D3 };
  if (relayIndex < 4) return kRelayPins[relayIndex];
#endif
  return -1;
}
```

One question to settle on a bench before shipping that patch. `RELAY_CONTROL.md` says the relays are D0-D3 "controlled via LED_D0-LED_D3 constants". On this variant `RELAY1..RELAY4` are `D0..D3` (0..3) and `LED_D0..LED_D3` are the status LEDs. If the coils are really driven through D0-D3 then the table should read `{ RELAY1, RELAY2, RELAY3, RELAY4 }` and relay control has never actuated anything. Confirm which table closes the contacts; the index arithmetic is wrong either way.

### 4.2 Digital and pulse inputs address the relay drivers, not the input terminals (H-04)

The Config Generator's pin dropdown is `optaPins=[{value:0,label:'Opta I1'},…,{value:7,label:'Opta I8'}]` and stores the raw 0-7 index as `primaryPin` / `rpmPin`.

The analog path is correct: `analogRead(channel)` resolves through `analogPinToPinName`, which adds the `A0` offset for indices below 15, so channel 0 reads A0 = terminal I1.

The digital and pulse paths use the same index with no offset:

```cpp
int pin = (cfg.primaryPin >= 0 && cfg.primaryPin < 255) ? cfg.primaryPin : (2 + idx);
pinMode(pin, INPUT_PULLUP);
int level = digitalRead(pin);
```

`digitalPinToPinName` applies no offset, so index 0-7 means D0-D7. Choosing "Opta I1" for a float switch or flow meter makes the firmware `pinMode(D0, INPUT_PULLUP)` and `digitalRead(D0)` — and D0 is `RELAY1`, the relay 1 coil drive. Indices 1, 2 and 3 are the relay 2, 3 and 4 drives. Index 7 is D7, numerically the same pin `getRelayPin(0)` uses for relay 1.

On a unit with a float switch on I1-I4 the switch is never read (the alarm is decided from the device's own output line), and `pinMode(..., INPUT_PULLUP)` reconfigures that relay's coil pin as an input on every sample, fighting the relay control path.

```cpp
// Map a configured terminal index (0..7 = I1..I8) to the Opta pin for digital use.
static int optaTerminalPin(int idx) {
#if defined(ARDUINO_OPTA)
  static const uint8_t kTerm[8] = { A0, A1, A2, A3, A4, A5, A6, A7 };  // I1..I8
  if (idx >= 0 && idx < 8) return kTerm[idx];
#endif
  return -1;
}
```

Use it everywhere a configured terminal index becomes a pin for `pinMode`/`digitalRead`, keep `analogRead(channel)` as it is, and reject out-of-range indices instead of the current `primaryPin < 255` test — server-side too, so a bad config cannot reach a device.

### 4.3 A client that has never been configured is invisible to the server (H-24)

`sendRegistration()` publishes to the telemetry notefile with no sensor index, by design:

```cpp
doc["mc"] = 0;  // Signals server: no monitors configured
publishNote(TELEMETRY_FILE, doc, true);
```

`handleTelemetry` rejects it on the first guard, before any client bookkeeping:

```cpp
if (!doc["k"].is<int>() || doc["k"].as<int>() < 1) {
  Serial.println(F("Telemetry dropped: missing/invalid sensor index k"));
  return;
}
```

That guard is the issue-313 fix for phantom sensor-0 records and is right for sensor telemetry, but it also swallows the one note built deliberately without a sensor: the boot registration, and the six-hourly heartbeat a client with `monitorCount == 0` sends through the same function. Because it returns before `noteClientFirmwareAndReconcile(...)`, the server never learns the device exists, its site, or its firmware version. A newly deployed client never appears under "New Sites (Unconfigured)" on the Client Console, so the documented commissioning flow cannot start; the client keeps announcing itself every six hours and the server keeps logging "Telemetry dropped".

```cpp
static void handleTelemetry(JsonDocument &doc, double epoch) {
  const char *clientUid = doc["c"] | "";
  // Registration / heartbeat carries no sensor payload by design (mc == 0, "r" gives the reason).
  if (!doc["k"].is<int>() && doc["mc"].is<int>() && doc["mc"].as<int>() == 0) {
    noteClientFirmwareAndReconcile(clientUid, doc["fv"] | "", epoch);
    upsertClientMetadata(clientUid, doc["s"] | "", epoch);   // site + last-seen only
    return;
  }
  if (!doc["k"].is<int>() || doc["k"].as<int>() < 1) { ... }
```

### 4.4 A month of history is destroyed the first time the file reaches 8 KB (H-23)

```cpp
if (sz > 0 && sz < 8192) {
  ...
  deserializeJson(monthDoc, buf);
}
fclose(existing);
...
if (!monthDoc.is<JsonArray>()) {
  monthDoc.to<JsonArray>();
```

When the month file has reached 8192 bytes the read is skipped with no log and no error, `monthDoc` is still empty, it is re-initialised as a fresh array, and the rollup writes the file back holding only the day it just summarised. Every earlier day in that month is gone, and because the threshold caps the file, the loss repeats each month once the fleet is large enough to reach it — the file grows by one entry per sensor per day.

```cpp
bool loaded = false;
if (sz > 0 && sz < (long)MAX_MONTH_FILE_BYTES) {
  ... loaded = (deserializeJson(monthDoc, buf) == DeserializationError::Ok);
}
if (existing && !loaded) {
  Serial.println(F("Daily rollup: month file unreadable/oversized - skipping to avoid data loss"));
  logTransmission("", "", "archive", "error", "daily rollup skipped: month file too large");
  fclose(existing);
  return;          // never rewrite the file with a single day
}
```

Then prune within the month (drop the oldest days) rather than dropping the file.

### 4.5 The viewer's API has no authentication and edits the fleet's alarm roster (H-27)

The viewer's request handler contains no PIN, session, token or origin check anywhere; routes dispatch straight from method and path, including `POST /api/contacts` and `POST /api/request-update`. The server treats that input as authoritative by design — its own comment reads:

> The viewer sends its FULL contact list in viewer_contacts.qo; the server stores them in the shared contact directory tagged cat:"viewer" (full-subset replace, last writer wins), auto-enrolls their phone numbers as SMS alert recipients, and echoes the authoritative list back in the next viewer summary.

So any host that can reach the viewer on the LAN can add its own number to the fleet's SMS alarm recipients (the server then texts that number a welcome message), remove every viewer-managed recipient by posting an empty list, or force an unscheduled cellular sync. The root README describes this device as a "Read-only monitoring device" with "Minimal Attack Surface — Secure deployment for public areas" and a "Web Server: HTTP (port 80) - read-only". None of that is true at HEAD.

Gate both POST routes behind the same session middleware the server uses, or make the viewer genuinely read-only and move contact editing to the server UI. If viewer-side editing stays, treat `viewer_contacts.qo` as a request needing admin approval rather than an authoritative replace. The README needs correcting either way.

### 4.6 The client's protection against stale configs has never been active (H-01)

The server injects a creation timestamp so the client can reject an out-of-order queued config:

```cpp
JAddNumberToObject(body, "_ts", snap->lastDispatchEpoch > 0.0 ? snap->lastDispatchEpoch : currentEpoch());
```

The client reads it back and guards on it:

```cpp
uint32_t inboundTs = doc["_ts"] | (uint32_t)0;
if (inboundTs > 0 && inboundTs <= gConfig.configEpoch) {
  // ignore obsolete queued config
} else {
  if (inboundTs > 0) { gConfig.configEpoch = inboundTs; }
  applyConfigUpdate(doc);
```

Both halves are correct in isolation, and the chain between them is not.

`currentEpoch()` returns `gLastSyncedEpoch + (millis() - gLastSyncMillis)/1000.0`, a double that is fractional except by coincidence. The Notecard's cJSON printer emits an integer only when the value is exactly integral — `if (vnum != (JNUMBER)vint) JNtoA(vnum, nbuf, -1); else JItoA(vint, nbuf);` — so `_ts` goes on the wire as a decimal number. The client parses that with ArduinoJson, which stores it as `VariantType::Float`, and `VariantData::isInteger<T>()` has no `Float` case: it falls through to `default: return false`.

So `doc["_ts"] | (uint32_t)0` returns the fallback **0** on every config the client has ever received. `inboundTs > 0` is false, so the obsolete-config check never runs, and `gConfig.configEpoch` is never assigned — it stays 0 permanently, which would keep the guard disabled even if an integral `_ts` arrived later. The "Config bypassed: newer version already exists" acknowledgement can never be sent.

The practical exposure is the hourly re-dispatch: a config that was queued, superseded, and then delivered late is applied over the newer one, silently changing alarm thresholds and relay behaviour. Note this is not the previously rejected claim that the server fails to inject `_ts` — it does inject it; the client cannot read it.

```cpp
// Client: accept the timestamp whether it arrives as an integer or a float.
double inboundTsD = doc["_ts"] | 0.0;
uint32_t inboundTs = (inboundTsD > 0.0) ? (uint32_t)inboundTsD : 0;
```
```cpp
// Server: stamp a whole second so the wire format is unambiguous.
JAddNumberToObject(body, "_ts",
    (JNUMBER)(uint32_t)(snap->lastDispatchEpoch > 0.0 ? snap->lastDispatchEpoch : currentEpoch()));
```

Fixing only the client is enough to restore the guard; doing both makes the contract explicit. Whichever way, bear in mind that every deployed client has `configEpoch == 0`, so the first config after the fix will always be accepted.

---

## 5. The server website, rendered and measured

Every page in this section was extracted from the HEAD source, served over a loopback fixture server, and opened in a real browser at 1280x900 and at 375x812. Fixture responses were built by reading each API handler's own JSON emitter, plus a captured live-device response committed in `build/`. Where a page defect below is stated as "measured", it was observed in the rendered DOM, not inferred from source.

### 5.1 Six page controls do nothing at all (H-21, H-18, H-20, M-18)

This is one root cause with six symptoms, and it is the most user-visible defect in the product.

Every server page wraps its script in an async IIFE:

```js
(async ()=>{ ... })();
```

Functions declared inside with `function foo(){}` are local to that closure. Inline HTML attributes (`onclick="foo()"`) are evaluated in **global** scope, so they only resolve if the function was explicitly published to `window`. `DASHBOARD_HTML` does exactly that (`window.clearRelays=`, `window.deleteClient=`, `window.logout=`, `window.refreshSensor=`, `window.requestUpdate=`, `window.snoozeAlarm=`) and `CONFIG_GENERATOR_HTML` publishes all 19 of its handlers. Four pages publish none of theirs.

Measured in the browser as `typeof window[handler]`:

| Page | Control | Handler | Result |
|---|---|---|---|
| `/site-config` | **Remove Client** | `deleteClient` | `undefined` |
| `/site-config` | **Expect Update** | `expectUpdate` | `undefined` |
| `/calibration` | per-sensor **Reset** | `resetCalibration` | `undefined` |
| `/calibration` | sensor-name link in Calibration Status | `viewTankPoints` | `undefined` |
| `/transmission-log` | per-row **Cancel** on a queued config | `cancelPendingConfig` | `undefined` |
| `/historical` | **Custom Range** start and end date inputs | `renderLevelChart` | `undefined` |

Clicking any of them throws `Uncaught ReferenceError` and produces no toast, no request, and no visible failure. The operator loses: decommissioning a client, arming an expected OTA version, resetting a bad calibration, cancelling a stuck config dispatch, and the custom date range on the history chart. Because `/api/calibration` DELETE has no other caller, that endpoint is unreachable from the UI entirely.

```js
// add before each page IIFE's closing })();
// SITE_CONFIG_HTML
window.expectUpdate = expectUpdate; window.deleteClient = deleteClient;
// CALIBRATION_HTML
window.resetCalibration = resetCalibration; window.viewTankPoints = viewTankPoints;
// TRANSMISSION_LOG_HTML
window.cancelPendingConfig = cancelPendingConfig;
// HISTORICAL_DATA_HTML
window.renderLevelChart = renderLevelChart;
```

The class of defect is worth a guard, because the pages are hand-minified single-line JavaScript inside PROGMEM string literals and nothing checks them. The extraction the screenshot workflow already performs can assert that every `on*="fn("` attribute in a page has a matching `window.fn =` in that page's script, and fail CI when it does not.

### 5.2 The Transmission Log's filters cannot select most of what the server logs (M-20)

Source-verified from every `logTransmission(...)` call site.

| | Emitted by the server | Offered by the page |
|---|---|---|
| Types | `sms, email, config, relay, relay_clear, viewer_summary, telemetry_request, sync_request, ota, stale, alarm, archive` | `sms, email, config, relay, relay_clear, viewer_summary, serial_request, location_request` |
| Statuses | `error, outbox, inbound, applied, cancelled, failed, pruned, auto-removed, snooze, unsnooze, optout, optin, stalled, sent`, plus a dynamic OTA state | `outbox, sent, failed, cancelled` |

Two filter options (`serial_request`, `location_request`) are never emitted. Six emitted types (`ota`, `stale`, `alarm`, `archive`, `telemetry_request`, `sync_request`) cannot be selected, so OTA and stale-client history cannot be isolated at all. Worst: SMS and email failures are logged with status `error`, which is not in the status list, so **filtering by "Failed" hides every message delivery failure** — the exact thing an operator opens this page to find.

Build both dropdowns from the values actually present in the fetched entries, or align the lists and fold `error` into Failed.

### 5.3 The History page advertises 730 days of RAM history; the ring holds 90 snapshots (M-42)

`MAX_HOURLY_HISTORY_PER_SENSOR` is 90, and the settings struct comment says `hotTierRetentionDays` has "max = MAX_HOURLY_HISTORY_PER_SENSOR snapshots". Nothing enforces it:

```cpp
gHistorySettings.hotTierRetentionDays = doc["hotDays"] | 90;   // no clamp
```

The device backup committed in this repository, `TankAlarm-112025-FTPS_Server_Test/ftp_root/dev_860322068056529/history_settings.json`, contains `"hotDays":730`, and the captured live response in `build/_api_history.json` reports `settings.hotTierDays: 730`, `dataInfo.maxRangeWithoutFtp: 1450`, `"maxRangeLabel":"~1450 days"`.

Rendered result, measured: the banner reads **"Data: RAM (730d)"** and the range selector offers up to Last 2 Years, while each sensor actually retains 90 snapshots — roughly 3.7 days at hourly cadence, 1.9 days at the 30-minute default. Because `maxDays > maxHotDays` is false for any range up to 730 days, the honest "Requested range exceeds hot tier" note never appears. Age-based pruning at `now - hotTierRetentionDays*86400` also never fires.

The same overstatement exists on the flash tier: `warmTierRetentionMonths` defaults to 24 and is what the banner prints as "+ Flash (24mo)", while `pruneDailySummaryFiles()` deletes everything older than `MAX_DAILY_SUMMARY_MONTHS`, which is 3.

```cpp
static uint16_t clampHotDays(uint32_t d) {
  const uint16_t maxDays = MAX_HOURLY_HISTORY_PER_SENSOR;  // one snapshot/day upper bound
  if (d == 0) return 1;
  return (d > maxDays) ? maxDays : (uint16_t)d;
}
gHistorySettings.hotTierRetentionDays = clampHotDays(doc["hotDays"] | 90);
// and report real capacity so the banner can say "90 snapshots", not "730 days":
settings["hotTierSnapshots"] = MAX_HOURLY_HISTORY_PER_SENSOR;
settings["warmTierMonthsEnforced"] = MAX_DAILY_SUMMARY_MONTHS;
```

### 5.4 A float switch reads "--", labelled TANK LEVEL, with an inches delta (M-24)

`formatValue(val,mu,ot)` falls back to `formatLevel(val)` whenever `mu` is empty and `ot` is `tank`, and `formatLevel` returns `'--'` for any value `<= 0`. A digital float switch reports `st:"digital"` with `l:0` for the normal OFF state. Measured on the rendered dashboard with a digital sensor in the fleet, the card shows the type label **TANK LEVEL**, the value **`--`**, the note *"No level data yet"*, and a **`+0.0 in/24h`** delta. OFF is indistinguishable from "never reported", and a genuinely empty tank reads identically.

```js
function formatValue(val,mu,ot,st){
  if(st==='digital') return (val>0?'ON':'OFF');
  if(typeof val!=='number'||!isFinite(val)) return '--';
  if(!mu&&(!ot||ot==='tank')) return formatLevel(val);
  return val.toFixed(1);
}
```

Suppress the 24-hour delta and the "No level data yet" hint for digital sensors too; the summary already carries `st`.

### 5.5 Client-level alarm flag and alarm type can contradict each other (M-30)

`sendClientsJson` sets the client-level alarm from any alarming sensor, then overwrites the type from whichever sensor reported most recently, alarming or not:

```cpp
if (rec.alarmActive) { clientObj["a"] = true; clientObj["at"] = rec.alarmType; }
...
if (rec.lastUpdateEpoch > previousUpdate) {
  ...
  clientObj["at"] = rec.alarmType;      // unconditional
}
```

A client whose sensor 1 is in `high` alarm (older update) and whose sensor 2 is normal (newer update) is published as `a:true, at:"clear"`. `/site-config` and `/client-console` both read exactly that pair (`alarm:!!c.a, alarmType:c.at||''`), so a site card shows an alarm with a contradictory or blank type. The same block leaves the client-level `n/k/l/ma/mu/ot` describing only the newest sensor, which is why the console's "Latest:" line can print one sensor's value with another sensor's unit.

```cpp
if (rec.lastUpdateEpoch > previousUpdate) {
  ...
  if (rec.alarmActive) { clientObj["at"] = rec.alarmType; }
  else if (!clientObj["a"].as<bool>()) { clientObj["at"] = rec.alarmType; }
}
```

### 5.6 Layout and style

Measured at 375x812 unless noted.

- **The toast is never fully hidden (M-13).** `#toast` hides itself with `transform:translateY(150%)` against a `bottom` offset of 36px. Full concealment needs the box to be at least 72px tall; an empty toast is 26px. Measured on the dashboard and on `/contacts`: top 877, bottom 903 in a 900px viewport — a 23px blue strip parked in the corner of every server page, about 13px after a message has sized it. Replace the transform trick with `opacity`/`visibility`.
- **`/calibration` forces a 723px layout viewport on phones (M-17).** Its two tables (10 columns and 6 columns) have a min-content width of 661px with no scroll container, so the card, `main` and body all stretch and the browser zooms the whole page out. Every other page fits 375px; `/historical` overflows by 1px. Fix with `.card table{display:block;overflow-x:auto;max-width:100%}` and `.card{min-width:0}`.
- **The contacts table hides a different column on every phone row (M-15).** The mobile rule is `.contact-table .ct-cell:nth-child(7n+3){display:none}`, which assumes each row contributes exactly seven children. A contact with alarm associations emits an extra full-width `.ct-assoc` child, shifting the stride for every row after it. Measured with six contacts, two carrying association chips: row 1 hid its email cell (correct), row 2 hid its phone cell (wrong). A misaligned row then places 7 cells into a 6-column grid and visually breaks apart. Give the column its own class and hide that instead of counting positions.
- **The sticky header eats 26% of a phone screen (L-10).** Measured 212px tall at 375x812, because the mobile media query stacks the bar into a column and wraps five pills onto three rows, all `position:sticky`. A horizontally scrolling action strip, or a non-sticky header below 720px, returns a quarter of the viewport.
- **Undefined CSS custom properties (L-11).** Pages reference `--card-bg`, `--accent` and `--chart-grid`; `STYLE_CSS :root` defines none of them. Each falls back to the browser's initial value, so those surfaces are transparent or black rather than themed.
- **Dead class vocabulary (M-26).** Client Console and Historical Data render into roughly 20 class names `STYLE_CSS` no longer defines, so those cards and grids are unstyled block elements. `/contacts` and the Server Settings recipient modal use a separate `.btn-*` / `.form-field` / `.filter-*` family that also does not exist.
- **Tooltips are unreadable (H-14).** The tooltip bubble sets `white-space:nowrap` with `max-width:300px`, so any tooltip longer than about 45 characters spills white text out of the dark bubble onto the page background. The Config Generator's help text is mostly longer than that.
- **Per-page header drift (M-16).** The header is copy-pasted per page and has diverged: Email Format shows 3 nav links, Config Generator 5, and five pages mark no active item at all. History, Calibration, Serial Monitor, Transmission Log, Config Generator and Site Config are reachable only from in-page links or by typing the URL. Nav pills are `<a>` on some pages and `<button>` on others, so they differ in height, font inheritance and hover treatment.
- **The viewer does not mirror the server (L-48).** It ships its own hand-written CSS block rather than `STYLE_CSS`: underline-tab navigation instead of pills, ~10px rounded cards against the server's `--radius:0`, 4px rounded buttons and inputs, and a bottom-centre dark toast instead of the server's bottom-right blue one. Every page also overrides toast colour inline with hard-coded hex values, and `/contacts` sets no colour at all, so its toast keeps whatever colour the previous toast left behind.


---

## 6. Complete findings list

Every entry below was confirmed by two independent verifiers reading the code at HEAD, except the seven established directly during this review (build warnings, live rendering, and the committed device data), which are marked with a dagger.


### High (27)

| ID | Area | Location | Finding |
|---|---|---|---|
| H-01 | data-contract | `Client:4736` | The obsolete-config guard never engages: the server stamps _ts as a fractional double, ArduinoJson reports is<uint32_t>() false for a Float, so `doc["_ts"] \| 0` yields 0 and gConfig.configEpoch stays 0 forever † |
| H-02 | logic | `Client:5165` | Removing a monitor via config leaves its alarm relays energized forever (no deactivate, no remote OFF, owner index orphaned) |
| H-03 | logic | `Client:5531` | Stuck-sensor detection flags a steady float switch (and a stopped engine) as failed after 10 samples, suppressing alarms *(prior)* |
| H-04 | logic | `Client:5600` | Digital/pulse inputs use the raw 0-7 index as an Arduino pin number, hitting D0-D7 (relay drivers) instead of the Opta I1-I8 terminals (A0-A7 = 15-22) |
| H-05 | logic | `Client:6358` | Alarm present on the first sample after boot/config with an until_clear or manual_reset relay is latched silently — no alarm note, no debounce |
| H-06 | logic | `Client:6381` | Analog high/low trigger debounce counters are reset only for readings inside the hysteresis band, so non-consecutive excursions accumulate toward the alarm *(prior)* |
| H-07 | logic | `Client:6766` | activateLocalAlarm() drives relay pins directly from sendAlarm(), bypassing relay-mode/bookkeeping: MANUAL_RESET relays drop on alarm clear, and monitors with no relay config toggle another monitor's relay |
| H-08 | logic | `Client:7387` | Client battery CRITICAL alert bypasses the 1-hour rate limiter, re-publishing an se:true alarm (forced hub.sync) on every battery poll while voltage sits in the 11.5–11.8 V band |
| H-09 | logic | `Client:7387` | Battery CRITICAL alert is re-published (with SMS escalation) on every battery poll while voltage sits between POWER_CRITICAL_ENTER_VOLTAGE and the chemistry critical threshold |
| H-10 | logic | `Client:8099` | Power-state debounce counts loop iterations against a cached voltage, so POWER_STATE_DEBOUNCE_COUNT=3 is satisfied by a single ADC sample in ~300 ms |
| H-11 | logic | `Client:9093` | getRelayPin() drives LED_D0+idx (pins 7,8,9,10 = status LEDs) instead of the relay outputs D0-D3 |
| H-12 | logic | `Client:9093` | getRelayPin() computes LED_D0+index, but the Opta LED constants are 7/9/8/153: relay 2 and relay 3 are transposed and relay 4 drives an unrelated GPIO † |
| H-13 | logic | `Client:9353` | Remote relay commands (relay_forward.qo) are dropped, not buffered, when the Notecard is offline — remote relay never switched or left ON |
| H-14 | layout-style | `Server:1698` | Tooltip bubble uses white-space:nowrap with max-width:300px, so every tooltip longer than ~45 characters spills white text outside the dark bubble onto the page background |
| H-15 | logic | `Server:2132` | Config Generator: loadConfig() restores currentLoopType before updateSensorTypeFields() rebuilds the select, silently downgrading 'ultrasonic' tanks to 'pressure' on re-save |
| H-16 | logic | `Server:2132` | Config Generator: loadConfig() never unchecks the absent alarm threshold, so a High-only (or Low-only) config re-acquires the template default lowAlarm 20 / highAlarm 100 on re-save |
| H-17 | logic | `Server:2132` | Config Generator: loadConfig() never restores .digital-trigger-state, so a 'not_activated' float-switch alarm silently inverts to 'activated' on re-save |
| H-18 | website-bug | `Server:2184` | Calibration page: 'Reset' button and sensor-name links throw ReferenceError (handlers not exported to window); /api/calibration DELETE is unreachable from the UI |
| H-19 | logic | `Server:2185` | Confirm at HEAD: calibration page still splits sensor keys on ':' (prior F-03/R-07) *(prior)* |
| H-20 | website-bug | `Server:2210` | Transmission Log per-row Cancel button is dead: inline onclick calls cancelPendingConfig, which only exists inside the page IIFE |
| H-21 | website-bug | `Server:2381` | Site Config page: 'Remove Client' and 'Expect Update' buttons are dead (handlers not exported to window) |
| H-22 | sequencing | `Server:6903` | ftpRestoreClientConfigs() unconditionally rewrites /fs/client_config_cache.txt from the manifest, discarding ACK/dispatch fields and writing an empty file when no per-client JSON is fetched |
| H-23 | logic | `Server:8114` | rollupDailySummaries() ignores an existing daily_YYYYMM.json once it reaches 8192 bytes and then overwrites it with only yesterday's entries |
| H-24 | data-contract | `Server:12475` | Fix #313-1a k-gate in handleTelemetry drops the client registration note, so unconfigured clients never appear and the Client Console "New Sites (Unconfigured)" panel is permanently empty |
| H-25 | logic | `TankAlarm_Battery.h:312` | SOLAR_STATE_FILE is "/fs/solar_state.json" but the client's LittleFS is mounted at "/cfg" — solar-only state is written to the MCUboot OTA FAT volume, where every save after the first fails at rename() |
| H-26 | data-contract | `TankAlarm_Solar.h:143` | SolarConfig battery thresholds are fixed 12V lead-acid values and are never derived from BatteryConfig chemistry/nominal voltage — a 24V bank (or LiFePO4) monitored over RS-485 raises a permanent false 'battery_high' (or late 'low') alarm |
| H-27 | security | `Viewer:985` | Viewer's HTTP API is completely unauthenticated: any LAN host can replace the viewer contact list, which the server adopts as SMS/email alarm recipients and welcome-texts |

### Medium (49)

| ID | Area | Location | Finding |
|---|---|---|---|
| M-01 | logic | `Client:1991` | checkNotecardHealth() only runs when the Notecard is already marked down, so card.wireless is never issued on a healthy client — signal telemetry, modem-stall recovery and deferred UID resolution are unreachable in normal operation |
| M-02 | logic | `Client:2184` | Daily-report reset of gCurrentLoopReadsOk fakes a 'read succeeded' event, re-arming the v2.1.4 current-loop recovery backoff and circuit breaker every 24 h |
| M-03 | logic | `Client:2326` | Solar alert type has no hysteresis: BATTERY_LOW <-> BATTERY_CRITICAL flapping bypasses the 1-hour solar-alarm rate limit, forcing a sync every poll and an SMS every 5 minutes |
| M-04 | data-contract | `Client:3161` | relayMomentarySeconds is parsed as uint16_t against an 86400 UI maximum, so any momentary duration above 65535 s becomes 0 and silently falls back to the 30-minute default (clamp is dead code, -Wtype-limits) † |
| M-05 | energy | `Client:4051` | POWER_*_OUTBOUND_MULTIPLIER are dead constants and hub.set is never re-issued on a power-state change, so the Notecard keeps its 60 min inbound / 6 h outbound modem cadence in ECO, LOW_POWER and CRITICAL_HIBERNATE |
| M-06 | data-contract | `Client:4942` | sampleSeconds is stored as uint16_t with no lower bound; an out-of-range value from the server becomes 0 and the client samples sensors on every 100 ms loop pass *(prior)* |
| M-07 | logic | `Client:5643` | Analog 0-10 V conversion assumes 10 V == ADC full scale; Opta I1-I8 front-end divider makes readings ~8% low |
| M-08 | data-contract | `Client:6472` | gLastClFaultReason is a single global shared by all current-loop monitors, so telemetry/daily `fault` and bus-recovery gating use the wrong monitor's code |
| M-09 | logic | `Client:6819` | A rate-limited alarm edge is dropped permanently: evaluateAlarms latches once and sendAlarm returns before publishNote, with no pending-notification retry *(prior)* |
| M-10 | logic | `Client:8207` | Battery-failure fallback calls loadSolarStateFromFlash() right after setting gSolarOnlyBatteryFailed=true, letting a persisted batteryFailed:false clear the flag and re-fire the se:true battery_failure alarm on every hibernate loop pass |
| M-11 | sequencing | `Client:8667` | publishNote() appends the new note to the outbox before replaying the flash backlog, so a fresh "clear" can be delivered ahead of the buffered "high" it resolves (client-side half of F-11) *(prior)* |
| M-12 | sequencing | `Client:9195` | relay.qi is drained one command per inbound poll (10 min grid / 60 min solar, x4/x12 in ECO/LOW_POWER), so a queued OFF behind an ON waits a full interval |
| M-13 | layout-style | `STYLE_CSS:0` | #toast hides itself with translateY(150%) against a 36px offset, leaving a 13-23px coloured strip permanently visible in the corner of every server page † |
| M-14 | security | `Server:1588` | Global (not per-IP) login back-off is re-checked inside requireValidPin, so unauthenticated failed logins 429 every admin action of the logged-in operator |
| M-15 | layout-style | `Server:1729` | Contacts mobile rule `.ct-cell:nth-child(7n+3)` counts all grid children, so one `.ct-assoc` row shifts the stride and later contacts lose Phone or Name instead of Email |
| M-16 | layout-style | `Server:1808` | Per-page copy-pasted headers drift: Email Format has 3 nav links, Config Generator 5, five pages mark no active item, the pause button is absent on 6 pages, and two pages render it without the .paused class *(prior)* |
| M-17 | layout-style | `Server:2173` | Wide tables (Calibration 10 columns, Transmission Log nowrap dates, Site Config UID table) have no horizontal-scroll wrapper, so the page body scrolls sideways on phones *(prior)* |
| M-18 | website-bug | `Server:2188` | Historical page: the Custom Range date inputs are dead — their inline onchange="renderLevelChart()" targets a function that is not global (ReferenceError), and no addEventListener replaces it *(prior)* |
| M-19 | performance | `Server:2204` | Historical page never sends the /api/history `sensor=` filter, so the server's documented single-sensor path is dead and the sensor dropdown only triggers a redundant refetch *(prior)* |
| M-20 | data-contract | `Server:2208` | Transmission Log type/status filter options do not match the vocabulary logTransmission emits (Failed misses all SMS/email errors; two type options match nothing) |
| M-21 | logic | `Server:2211` | Transmission Log CSV export omits the Detail clause the table's search applies, so the download can be a subset of the visible rows |
| M-22 | layout-style | `Server:2216` | /contacts and the Server Settings recipient modal use a `.btn-*` / `.form-field` / `.filter-*` / `.daily-report-*` vocabulary STYLE_CSS never defines, so Cancel and every 'Remove' render as primary blue *(prior)* |
| M-23 | layout-style | `Server:2220` | /contacts is the only server page whose showToast() takes no isError flag and never sets a colour, so error toasts look like successes and inherit whatever colour the shared pause toast last painted |
| M-24 | website-bug | `Server:2229` | Digital float switches render as TANK LEVEL with value '--' and an inches-per-24h delta, so the normal OFF state is indistinguishable from no data † |
| M-25 | performance | `Server:2338` | Dashboard "Update requested" pill never expires: no client-side or server-side timeout on updateRequestedEpoch, so an unreachable client pins the dashboard to a 10 s refresh and permanently replaces the Update button |
| M-26 | layout-style | `Server:2366` | Client Console and Historical Data render into ~20 class names that STYLE_CSS no longer defines: cards/grids are unstyled and the Historical site accordion is inert (always expanded) |
| M-27 | security | `Server:2380` | DOM-based escapeHtml does not escape quotes but is used inside quoted HTML attributes with device-reported strings |
| M-28 | security | `Server:5586` | FTP/FTPS backup uploads server_config.json containing the admin PIN in clear text plus FTP credentials XOR-'obfuscated' with a key built from productUid and that same PIN |
| M-29 | logic | `Server:7719` | archiveMonthToFtp() computes the monthly cold-tier summary from the 90-snapshot hot ring and only falls back to the whole-month warm tier when the hot ring has nothing in that month |
| M-30 | data-contract | `Server:10479` | /api/clients top-level `at` is overwritten by the newest-reporting sensor regardless of its alarm state, so Client Console and Site Config can render "ALARM: clear" for a multi-sensor client |
| M-31 | logic | `Server:12934` | handleAlarm zeroes the sensor's stored level on diagnostic and config-reload notes that carry no reading |
| M-32 | data-contract | `Server:12934` | Diagnostic alarm notes carry the reading as `rd`; server ignores `rd`, resolves level to 0 and overwrites the sensor's current value |
| M-33 | sequencing | `Server:13163` | handleDaily runs the missed-alarm reconcile before the sensors[] upsert loop, so a lost alarm on a not-yet-registered sensor is ignored for 24 h |
| M-34 | data-contract | `Server:13506` | handleUnload overwrites the sensor's configured label with the literal "Tank" on every unload event, and omits the registry dirty flag |
| M-35 | data-contract | `Server:13506` | handleUnload unconditionally overwrites the sensor registry label with the literal "Tank" because the client's unload note has no `n` |
| M-36 | data-contract | `Server:13771` | samePhoneNumber() treats '+' as a significant character and the server contacts API enforces no E.164 format, so STOP opt-outs and SNOOZE replies silently miss contacts entered without a leading '+' |
| M-37 | logic | `Server:14654` | Reminder engine re-texts a new excursion whose own alarm was configured NOT to SMS (lastSmsAlertEpoch persists across excursions) |
| M-38 | logic | `Server:14654` | Reminder engine anchors on the lifetime lastSmsAlertEpoch: reminder SMS/email fire for later alarms the client explicitly did not want texted, and the first one fires within a minute |
| M-39 | logic | `Server:16088` | staleAlertSent is runtime-only and force-cleared on load: every server reboot re-texts "Client stale" for each long-offline client, and "Client recovered" is lost across a reboot |
| M-40 | logic | `Server:16535` | archiveClientToFtp() reads archived_clients.json through a 2 KB stack buffer and rewrites it with remove()-then-rename(), so manifest entries are lost once the file passes 2047 bytes |
| M-41 | logic | `Server:16535` | Archived-clients manifest is read through a fixed char buf[2048]; past ~7 archives GET /api/history/archived returns an empty list and each new archive corrupts/drops the newest manifest entries |
| M-42 | data-contract | `Server:17184` | Hot- and warm-tier retention are reported as configured wishes, not capacity: the History banner reads 'RAM (730d)' against a 90-snapshot ring, and '+ Flash (24mo)' against a 3-month prune † |
| M-43 | sequencing | `Server:17808` | FTP restore does not invalidate the contacts RAM cache or reload registry/metadata/history, so restored files are ignored and then overwritten by stale RAM *(prior)* |
| M-44 | data-contract | `Server:18554` | FTP password/user and GitHub route alias are silently truncated to 31 chars, with an unconditional 'Settings saved successfully' |
| M-45 | logic | `Server:19319` | /api/calibration returns the OLDEST 50 log lines and recalculateCalibration() fits only the first 100 matching points, while calibration_log.txt is never trimmed |
| M-46 | logic | `Server:19491` | POST /api/calibration rejects a verified reading of 0 (empty tank / 4 mA anchor) as "Missing verifiedLevelInches" |
| M-47 | website-bug | `Server:20252` | GET /api/location parses `client=` without urlDecode, so the Client Console location readout always shows "Not yet received" for real dev: UIDs |
| M-48 | logic | `TankAlarm_Battery.h:312` | SOLAR_STATE_FILE still points at the MCUboot FAT mount "/fs" instead of the app LittleFS mount "/cfg", so every solar-state save after the first fails at rename() and the state never updates |
| M-49 | sequencing | `TankAlarm_DFU.h:1282` | pending_ota.json is written with status 'trial' before boot_set_pending() and two blocking Notecard round-trips — a reset in that window is misread as a rollback and permanently blacklists the target version |

### Low (49)

| ID | Area | Location | Finding |
|---|---|---|---|
| L-01 | logic | `Client:1392` | Time-based pulse mode never reports 0 RPM: a stopped engine keeps returning the previous RPM indefinitely |
| L-02 | energy | `Client:2264` | sync_request.qi is polled on every loop pass for 2 s after each config check instead of once |
| L-03 | energy | `Client:4366` | Before the first successful time sync, ensureTimeSync() issues a card.time Notecard I2C transaction on every loop pass (~10/s in NORMAL) *(prior)* |
| L-04 | sequencing | `Client:5291` | Config apply resets gPowerState to NORMAL with no exit actions, producing a spurious power-state alarm and sync a few hundred ms later |
| L-05 | sequencing | `Client:5458` | validateSensorReading() publishes sensor-fault/stuck/recovered notes from inside sampleMonitors Phase A, breaking the v2.0.46 'no Notecard I/O between A0602 reads' isolation |
| L-06 | logic | `Client:5559` | Sensor recovery requires 3 "consecutive" good readings but recoveryCount is never reset by a bad reading |
| L-07 | logic | `Client:5940` | Watchdog kick inside the 5-sample current-loop burst is gated on loopPowerEnabled, leaving externally powered loops unkicked for 4 x loopPowerSampleDelay (unclamped uint16) |
| L-08 | data-contract | `Client:8397` | Software 'daily' solar min/max are never reset: bvMin/bvMax in the daily report are since-boot extremes |
| L-09 | data-usage | `Client:13093` | Daily solar/battery/power sub-objects (and several telemetry/alarm keys) are server-side dead data — only solar.bv is consumed |
| L-10 | layout-style | `STYLE_CSS:0` | Sticky header measures 212px at 375x812 (26% of the viewport) because five pills wrap to three rows below 720px † |
| L-11 | layout-style | `Server:1630` | STYLE_CSS :root defines --card but pages reference --card-bg, --accent and --chart-grid, which are never defined anywhere *(prior)* |
| L-12 | layout-style | `Server:1651` | Global `h3` is styled as a grey uppercase sub-label, but SMS/Email Setup cards and Historical site headers use `<h3>` as their primary title |
| L-13 | layout-style | `Server:1659` | button-pills inherit the UA control font and the global button:hover lift/shadow, so Logout/Unpause differ from the anchor pills next to them (the height claim is wrong) |
| L-14 | performance | `Server:1801` | Server Settings page calls loadSettings() twice per page open (duplicate /api/clients and /api/contacts fetches) |
| L-15 | data-contract | `Server:2062` | Config Generator allows unlimited sensor cards and neither handleConfigPost nor dispatchClientConfig validates the count, so the client silently discards everything past MAX_MONITORS = 8 |
| L-16 | website-bug | `Server:2106` | collectConfig() derives the client's daily report time from a hard-coded 05:00 UTC when the /api/contacts directory has not loaded (or failed to load) |
| L-17 | website-bug | `Server:2139` | Config Generator: opening ?uid= for a client with no stored snapshot shows a red 'Error loading config' toast and renders zero sensor cards |
| L-18 | website-bug | `Server:2214` | Login page prints the literal "Invalid PIN" for every non-2xx response, so the server's 429 lockout countdown is never shown |
| L-19 | security | `Server:2214` | Login redirect sanitizer can be bypassed with a backslash (open redirect after login) |
| L-20 | website-bug | `Server:2214` | Login page reports 'Invalid PIN' for rate-limit (429) and first-login (400) responses |
| L-21 | data-contract | `Server:2381` | Site Config: the ts[] mapper drops `flt` and `ma`, so the page's FAULT badge and mA readout are dead code and a current-loop fault renders as a normal numeric level |
| L-22 | layout-style | `Server:2381` | Status colour vocabulary is hard-coded per page and several status texts fail WCAG contrast (green #10b981 'Normal' 2.5:1, #f59e0b 2.15:1, .pin-chip 3.1:1) |
| L-23 | performance | `Server:2597` | POST /api/refresh returns the full non-summary client document (every cached config payload copied into the JSON doc) although neither caller reads cfgs |
| L-24 | security | `Server:2729` | HTTP response-header injection in /api/serial-export: unvalidated 'client' (and 'source') query params are spliced into Content-Disposition after URL-decoding |
| L-25 | logic | `Server:8089` | Daily rollup only ever summarizes 'yesterday'; days missed during an outage are never rolled up even though the hot tier still holds them |
| L-26 | logic | `Server:10646` | handleConfigPost's "server" branch passes JsonVariants straight to strlcpy (NULL source for null/number/bool) and does not clamp dailyHour/dailyMinute |
| L-27 | maintainability | `Server:11153` | Four command.qo builders leak the note.add request J object when the body allocation fails (sendRelayCommand, sendRelayClearCommand, sendLocationRequest, requestClientSerialLogs) |
| L-28 | logic | `Server:12820` | System-alarm SMS path sends an uninitialized 160-byte buffer for solar_sunset / i2c-error-rate when se:true |
| L-29 | data-contract | `Server:12836` | handleAlarm/handleUnload lack the k>=1 gate that handleTelemetry/handleDaily have; upsertSensorRecord only rejects k>=MAX_SENSOR_RECORDS, so a k-less non-system alarm creates and latches a phantom sensor-0 record |
| L-30 | data-contract | `Server:13301` | handleDaily still applies the legacy >=4.0 mA gate that Fix 13 removed from handleTelemetry/handleAlarm, so a valid under-live-zero reading (3.60-3.99 mA) is stored as 0 by the daily and as the raw value by telemetry |
| L-31 | data-contract | `Server:13481` | Unload log never records that an SMS was sent (smsNotified is always false) |
| L-32 | logic | `Server:13863` | Welcome/opt-in SMS never reaches contacts enrolled only in serverSmsRecipients — the server-event list bypasses the compliance message the alarm list gets |
| L-33 | security | `Server:13949` | handleSmsInbound honors START/YES and STOP from any sender: strangers get a welcome SMS, and 32 STOPs silently FIFO-evict genuine opt-out entries so the /contacts page shows blocked contacts as subscribed |
| L-34 | maintainability | `Server:14320` | Transmission log entries for SMS/email carry no site/client and truncate the detail to 47 characters of message |
| L-35 | data-contract | `Server:14769` | sendDailyEmail() builds its recipient list without the de-duplication and placeholder filter that sendEmailAlert() applies |
| L-36 | logic | `Server:14821` | Daily-report subject {date} is rendered from UTC instead of the configured local send time (and always ISO, ignoring the dateFormat setting) |
| L-37 | logic | `Server:14877` | sendDailyEmail() drops the daily report with only a Serial print on serialize-overflow or JsonDocument allocation failure, and the caller still advances the schedule to tomorrow |
| L-38 | logic | `Server:15733` | loadSensorRegistry's duplicate-merge branch omits lastSmsAlertEpoch/smsAlertsInLastHour/reminderSnoozeEpoch, so a newer duplicate's SMS rate-limit and snooze state is discarded (the fresh-record path and the in-memory dedup both preserve them) |
| L-39 | logic | `Server:16064` | checkStaleClients holds a raw pointer into gSensorRecords[] across pruneStaleOrphanSensors(), which compacts the array before the pointer is used in the stale/recovery SMS |
| L-40 | logic | `Server:17458` | handleHistoryYearOverYear splits ?sensor= on the FIRST ':', so every real "dev:<imei>:<idx>" key resolves to client "dev" and the single-sensor branch always returns found=false |
| L-41 | data-contract | `Server:18168` | Contacts and Server Settings pages toast 'saved' when the server reports saved:false |
| L-42 | logic | `Server:19234` | Calibration range/quality metrics are neither persisted nor recomputed at boot, so every calibrated sensor shows a 0.0-0.0 mA range and a false 'Narrow sensor range' warning after a reboot |
| L-43 | logic | `TankAlarm_Battery.h:443` | getBatteryStateDescription() names the band ABOVE each threshold, so its "low"/"critical" labels never line up with the bands checkBatteryAlerts() alarms on |
| L-44 | ci-docs | `TankAlarm_Common.h:332` | Two stale Common-header comments: DFU_CHECK_INTERVAL_MS claims per-sketch overrides that no sketch defines, and the 24V battery scaling example cites voltages initBatteryConfig() never produces (the rename/FAT sub-claim does not hold) |
| L-45 | maintainability | `TankAlarm_DFU.h:317` | tankalarm_performIapUpdate() is dead code that would erase and reprogram the running application region in place *(prior)* |
| L-46 | logic | `TankAlarm_I2C.h:120` | Wire.setTimeout(I2C_WIRE_TIMEOUT_MS) resolves to Stream::setTimeout and is inert on MbedI2C — the documented I2C hang guard (and the v2.0.46 25->50 ms change) configures nothing; A0602_WIRE_TIMEOUT_MS is dead |
| L-47 | data-contract | `TankAlarm_Solar.h:253` | SolarConfig.modbusBaudRate is uint16_t, so the allow-listed 115200 baud silently truncates to 49664 and is reset to 9600 *(prior)* |
| L-48 | layout-style | `Viewer:308` | Viewer serves its own hand-written CSS block instead of the server's STYLE_CSS: nav, buttons, inputs and toast use different radii, colours and scale from the server UI |
| L-49 | website-bug | `Viewer:310` | Viewer contacts page: save() promise has no rejection handler, so a failed POST leaves phantom changes on screen |
---

## 7. Improvements: speed, energy, data

These are efficiency opportunities rather than defects. Each cites code confirmed at HEAD; expected benefit is stated rather than assumed.

### 7.1 Client energy

| What | Where | Why it costs | Change |
|---|---|---|---|
| Power states never re-issue `hub.set` | `Client:4051` (M-05) | `POWER_ECO_OUTBOUND_MULTIPLIER` and its siblings are declared and never read. Entering ECO or LOW_POWER stretches the sample interval but leaves the modem on its full-power outbound/inbound cadence, so the largest energy consumer ignores the power state entirely. | Re-issue `hub.set` with multiplied outbound/inbound minutes on each power-state transition, and once at boot. |
| A `card.time` transaction every loop pass before the first sync | `Client:4366` (L-03) | `ensureTimeSync()` issues a Notecard I2C round trip on every iteration until the clock is first set, roughly 10 per second on a cold boot with no cellular. | Back off: attempt once, then at 1 s, 5 s, 30 s, capped at the sync interval. |
| `sync_request.qi` polled every pass for 2 s after each config check | `Client:2264` (L-02) | About 20 `note.get` transactions where one suffices. | Poll once per config check. |
| Battery CRITICAL alert re-sent every poll | `Client:7387` (H-08) | Not just noise: each repeat sets `se:true`, forcing a `hub.sync` over cellular on a battery that is already critical. See H-08 — the fix is also the largest single energy win on a failing client. |
| `checkNotecardHealth()` only runs when the Notecard is already marked down | `Client:1991` (M-01) | Signal strength and modem-stall detection never run on a healthy-looking but degraded link, so a client can burn power retrying at 1 bar without the watchdog noticing. | Run the health check on a slow timer regardless of the last-known state. |

### 7.2 Cellular data

- **Dead payload.** The daily solar, battery and power sub-objects, plus several telemetry and alarm keys, are transmitted and never read by any server handler (L-09). The viewer summary carries per-sensor `ma` and `vt` that the viewer never reads. Removing a key costs nothing and every note carries it.
- **Change thresholds are off by default.** `DEFAULT_LEVEL_CHANGE_THRESHOLD` is `0.0f`, which means "publish every sample". With a 30-minute cadence that is 48 notes per sensor per day where a 0.5-inch deadband would typically send a handful. This is a deployment default worth revisiting, not a defect.
- **Replay ordering.** `publishNote()` appends the new note to the outbox before replaying the flash backlog (M-11), so a fresh "clear" can be delivered ahead of the older "alarm" it clears. Replay oldest-first.

### 7.3 Server speed and RAM

Static RAM is already at 68%, leaving 162 KB for locals and heap. The handlers most likely to press on that:

- `sendHistoryJson` builds an auto-growing `JsonDocument` and then a full `String` copy of it before writing a byte to the socket. At capacity (20 sensors x 90 snapshots plus voltage and alarm series) both live at once. Stream the JSON to the socket instead.
- `POST /api/refresh` returns the full non-summary client document, copying every cached config payload into the response (L-23), for a button whose only job is to refresh one card.
- The Server Settings page calls `loadSettings()` twice on every open (L-14), duplicating `/api/clients` and `/api/contacts`.
- The dashboard's "Update requested" pill never expires (M-25), so a single click leaves the page polling `/api/clients` every 10 seconds indefinitely on a single-threaded HTTP server.
- `/style.css` already sends `Cache-Control: public, max-age=3600`, but the URL is unversioned, so a firmware update cannot bust it. `/style.css?v=2.2.14` costs one character per page.
- The History page fetches the entire unbounded `/api/history` when a single sensor is selected (split finding, section 9) because it sets `apiDays=0` and never passes `sensor=`. Pass the sensor and the range.

---

## 8. Prior review items re-checked at HEAD

The three review documents in this folder dated 2026-09-08 were written partly against the stale working tree. Two of their headline items resolve differently when read against HEAD.

**F-01 / R-01, "daily report part 1 can clear an active alarm" — armed but not currently firing.** The mechanism is real and still present:

```cpp
bool isFirstPart = (part == 0 || part == 1);
bool runAlarmReconcile = isFirstPart && (dailyAlarms || dailySchema >= 2);
```

The client numbers parts from 0 and emits the `alarms` array only in part 0, so in part 1 `dailyAlarms` is null. The reconcile therefore depends entirely on `dailySchema >= 2`. `NOTEFILE_SCHEMA_VERSION` is **1** at HEAD, and `publishNote` stamps that value on every note, so `runAlarmReconcile` is false for part 1 today and the alarm-clearing pass does not run. The moment the schema constant is bumped to 2 — which the surrounding comment anticipates ("schema-2 clients always include the array when any alarm is active") — part 1 of every multi-part daily report will clear every active alarm the server holds for that client. Fix it now, while it is cheap: drop `part == 1` from `isFirstPart`, or gate the reconcile on the part that actually carries the array.

**R-06, "dashboard Update has no client consumer" — not present at HEAD.** The client has a `telemetry_request.qi` handler. That finding was derived from the June client sitting in the working tree.

Confirmed still present at HEAD, with IDs in section 6: F-03/R-07 calibration key splitting (H-19), F-04/R-03 non-consecutive analog debounce (H-06), F-15/R-13 relay timeout overwriting `alarmType`, F-17/R-20 blocking FTPS backup, F-21/R-17 mobile overflow and undefined CSS tokens (M-15, M-17, L-11), F-22 Chart.js fallback, R-21 daily schema comment versus `NOTEFILE_SCHEMA_VERSION 1`, R-23 login overlay and 30-second session polling, R-24 the Approve Deletion PIN prompt.

**R-22, README v1.9.3 — still present and now further out of date.** The root README is headed "TankAlarm v1.9.3 — Release Date: June 11, 2026" while Common declares 2.2.14. Beyond the version, its viewer section is wrong in a way that matters (see H-27).

---

## 9. Investigated and not confirmed

Recorded so they are not re-reported.

- **Calibration log entry truncation.** `saveCalibrationEntry` does write through a `char entry[256]`, so a long note is truncated — but both verifiers established the claimed consequence (a truncated line gluing onto the next record) is unreachable, because the reader's own buffer and the newline handling bound it.
- **`BLUES_PRODUCT_UID` treated as a CI secret but baked into public binaries.** The workflow does write the secret into the generated config headers, but a verifier parsed the four committed `.ino.bin` artifacts and found the value absent, so the stated exposure does not hold at HEAD.
- **Split, one verifier each way (open questions, not findings):** session-token entropy — both verifiers agree the generator is a 64-bit MMIX LCG seeded from `micros()`, `millis()` and four `analogRead()` values with no expiry, and disagree only on whether that is exploitable on a trusted management LAN; and the viewer summary carrying per-sensor `ma`/`vt` that the viewer never reads — real, but the wasted bytes are few.

Previously rejected claims from earlier reviews were kept out of scope and did not resurface: atomic-write concerns for `posix_write_file`, `checkStaleClients` watchdog risk, the viewer network-profile ordering guard, config `_ts` injection, and the "leaked session token" reading of the literal string `cookie`.

---

## 10. Coverage

Reviewed in detail: the three production sketches end to end; every Common header and `TankAlarm_Solar.cpp`; the note ingestion pipeline and every notefile contract in both directions; persistence, FTP backup and restore, and the tiered history system; all 14 server page routes and both viewer pages, rendered; PIN/session authentication and every mutating API route; the six GitHub workflows, `update_html.py`, and the README and per-component documentation.

Areas the verifiers explicitly checked and found sound include: the note peek-then-delete order with its schema gate, poison counter and per-poll cap; the sensor hash table (djb2 with linear probing, size 128 against a 64-record maximum, with static assertions and every removal path covered); `currentEpoch()` rollover arithmetic; the alarm ring-buffer index math; the v2.2.13 snooze state machine including its one-shot guard and SMS reply path; the daily multipart bitmask and its 30-minute batch reset; `handleConfigAck` version matching; relay-forward and location-response range checks; atomic file writes throughout; the constant-time PIN comparison and lockout backoff; and PROGMEM chunked streaming for pages above 32 KB.

Not covered: no hardware was flashed, no relay actuated, no live SMS, email or Notehub transaction sent, and no deployed website exercised — the browser work terminated at a loopback fixture server with synthetic data derived from the server's own code. Generated binaries, the archived review documents, and the bench sketches under `TankAlarm-112025-Sensor_Utility` were not line-reviewed. The four lenses covering server loop timing, CI and documentation, and the two quantified efficiency sweeps were interrupted by a usage limit before completing; section 7 reflects what the completed lenses established rather than a full measurement pass. A repository-wide review is not proof that no further defects exist.

## 11. Suggested order of work

1. **Physical correctness first.** H-12 and H-04 decide which pin the firmware actually drives and read. Settle the relay constant question on a bench, fix both index maps, and add a server-side range check on configured pin indices.
2. **Alarm integrity.** H-02, H-07, H-08, H-13, H-09 and H-06: first-sample latching, the direct-pin path that bypasses relay bookkeeping, the battery-critical repeat storm, offline relay commands, and non-consecutive debounce.
3. **Stop losing state.** H-22, H-23, H-25 and M-31: FTP restore rewriting the config cache, the 8 KB month-file reset, the solar state file written to the wrong mount, and diagnostic notes zeroing the last good reading.
4. **The website in one pass.** Export the six trapped handlers (H-21, H-18, H-20, M-18), fix the tooltip (H-14), the toast (M-13), the contacts grid stride (M-15), table overflow (M-17), and the undefined CSS tokens (L-11). Add the CI check that every inline handler resolves.
5. **Tell the truth about retention and delivery.** M-42 and M-20: clamp the tiers and report capacity, and make the transmission log's filters match what is logged.
6. **Close the viewer.** H-27, then correct the README's read-only claims and the v1.9.3 heading.
7. **Then efficiency.** Section 7, measuring before and after rather than assuming.


---

## Appendix A. Reported but not verified

The verification run was interrupted by a usage limit before these candidates were independently checked. They are recorded as leads, not findings. Two were verified by hand and appear in the main list instead; the rest need a reader before they are acted on.

| Area | Location | Candidate |
|---|---|---|
| sequencing | `Viewer:817` | Viewer fetches the summary at the exact instant the server publishes it, so the dashboard is always one 6-hour cycle stale |
| logic | `Viewer:926` | DHCP-first/static fallback (v2.2.12) cannot take effect: failed DHCP leaves the mbed interface in CONNECTING and the static Ethernet.begin() is refused |
| logic | `Viewer:561` | Live re-init after a server-pushed net profile either does nothing (interface already up) or blocks 60 s and trips the 30 s watchdog |
| sequencing | `Viewer:599` | Confirmed at HEAD: summary fetch schedule is never armed if the clock is unavailable at boot |
| logic | `Viewer:2006` | Daily print job formats every sensor as feet/inches regardless of measurement unit and prints 1970 dates for sensors without data |
| layout-style | `Viewer:308` | Viewer dashboard never flags stale sensors or snoozed reminders, unlike the server dashboard it claims to mirror |
| sequencing | `Viewer:1363` | Contacts POST does not arm the fast inbox poll, so the server's immediate echo (and any admin edits) are not shown for up to a full cycle |
| data-contract | `Server:15000` | Viewer summary note has no size guard; large fleets exceed the Notecard's per-note body limit and the viewer silently starves |
| logic | `Viewer:1636` | GitHub release check reads web.get 'body' as a string, but the Notecard returns a JSON object, so the update banner can never populate |
| sequencing | `Server:12062` | Hourly config re-dispatch until ACK sends duplicate configs to slow-polling clients and falsely auto-cancels after 5 h |
| sequencing | `Client:9211` | relay.qi is drained one note per inbound cycle, so multi-relay and ON->OFF sequences apply 10 min to 12 h apart |
| energy | `Client:2264` | sync_request.qi is polled on every loop iteration for 2 s after each config check (about 20 note.get calls per cycle) |
| logic | `Server:19858` | 'Outdated client' banner counts any version different from GitHub latest, including clients running newer firmware |


---

*Prepared 2026-09-08 against `master` at `4f30a7f`, firmware v2.2.14. Line numbers are from that
commit and drift with edits: anchor searches on the quoted code, not the numbers. All three sketches
were compiled; no hardware was flashed and no live message was sent.*
