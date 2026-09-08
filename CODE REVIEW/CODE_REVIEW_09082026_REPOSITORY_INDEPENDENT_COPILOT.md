# Independent Repository Review — 2026-09-08

**Reviewer:** GitHub Copilot (Grok 4.6)  
**Date:** 2026-09-08  
**HEAD:** `6864d9c` (`master`, origin/master) — firmware **v2.2.14**, build sequence **301**  
**Method:** Independent static review of production firmware and the server website, plus a live loopback check of `/historical`. Prior reviews were used as leads to re-verify, not as source truth. No firmware was flashed and no live SMS/email/Notehub traffic was sent.

Companion documents consulted (not copied):

- [CODE_REVIEW_09082026_REPOSITORY_FULL_REVIEW.md](CODE_REVIEW_09082026_REPOSITORY_FULL_REVIEW.md)
- [CODE_REVIEW_07212026_SERVER_FULL_REVIEW.md](CODE_REVIEW_07212026_SERVER_FULL_REVIEW.md)
- [CODE_REVIEW_07202026_ISSUE_313_SERVER_WEBPAGE_BUGS.md](CODE_REVIEW_07202026_ISSUE_313_SERVER_WEBPAGE_BUGS.md)
- [CODE_REVIEW_07212026_SERVER_INDEPENDENT_COPILOT.md](CODE_REVIEW_07212026_SERVER_INDEPENDENT_COPILOT.md)

This is a review, not a firmware repair. Uncommitted client/Common/server edits already in the working tree were **not** included in this commit.

---

## 0. Recommendation

Do **not** treat compilation as proof of operational correctness. The four production sketches compile, but several confirmed sequencing bugs can still miss alarms, clear the wrong relay, drop durable state, or present a broken website on an isolated LAN.

Highest-priority work, in order:

1. Stop treating daily-report **part 1** as a first part (can clear live alarms).
2. Address Clear Relay by **sensorIndex**, not presentation order.
3. Make analog debounce require **consecutive** qualifying samples.
4. Persist empty registries and report save failure.
5. Re-arm server/viewer schedules after a late clock sync (the client already does this).
6. Repair History/Calibration identity, units, CSS tokens, and Chart.js fallback.

---

## 1. Findings at a Glance

| ID | Sev | Area | Finding |
|---|---|---|---|
| R-01 | High | Server daily | `isFirstPart = (part == 0 \|\| part == 1)` lets schema-1 part 1 clear alarms confirmed in part 0 |
| R-02 | High | Dashboard / client | Clear Relay sends presentation-order `sensorIdx`; client treats it as monitor-array index |
| R-03 | High | Client alarms | Analog debounce does not reset on a non-qualifying sample; nonconsecutive spikes can trip or clear |
| R-04 | High | Client alarms | Latch happens before `sendAlarm`; offline/`gNotecardAvailable` skip drops the only notification |
| R-05 | High | Persistence | `saveSensorRegistry` / `saveClientMetadataCache` no-op at count 0; callers clear dirty on failure |
| R-06 | High | Protocol | Dashboard **Update** posts `telemetry_request`; the client never polls that notefile |
| R-07 | High | Website | Calibration `split(':')` corrupts `dev:…` UIDs and can submit the wrong identity |
| R-08 | Med | Scheduling | Server daily email and viewer fetch stay unarmed if boot has no clock; client already re-arms |
| R-09 | Med | Config ACK | Matching `cv` clears `pendingDispatch` even when `st` is `failed`; same-second `_ts` can ACK the wrong revision |
| R-10 | Med | Snooze | Broadcast and HTTP `success:true` happen before a durable save that cannot report failure |
| R-11 | Med | Restore | FTP restore uses a 2 KiB buffer, partial-success, and does not reload registry/metadata into RAM |
| R-12 | Med | Persistence | Shared `gLastRegistrySaveMillis` can starve metadata saves |
| R-13 | Med | Alarms | `relay_timeout` overwrites `alarmType`, so reminders stop while the tank is still high |
| R-14 | Med | Pulse | `pollPulseSampler` runs only inside periodic `readPulseSensor`; edges between samples are unobserved |
| R-15 | Med | Power | Unknown voltage forces `POWER_STATE_NORMAL` and skips recovery side effects |
| R-16 | Med | History UI | Sensor filter resets; Chart.js CDN crash wipes data; units hardcoded inches; VIN series mixed |
| R-17 | Med | CSS / layout | Missing `--card-bg` / `--accent` / `--chart-grid`; `--radius:0` vs 8–10px cards; History/Calibration not in primary nav |
| R-18 | Med | Settings | `/api/server-settings` mutates RAM, then 400s on invalid `viewerNet` |
| R-19 | Med | Notes | Delete-after-handle with ignored delete failures; 12 poison slots for 13 inboxes |
| R-20 | Med | Backup | Synchronous FTPS backup ends the web listener and blocks the loop (~8m40s of inter-file waits) |
| R-21 | Med | Schema | Client comments say daily “schema 2+”; `NOTEFILE_SCHEMA_VERSION` is still `1` |
| R-22 | Low | README | Root README still advertises TankAlarm **v1.9.3** (June 11, 2026) |
| R-23 | Low | Login / session | 5s loading overlay; every page polls `/api/session/check` every 30s |
| R-24 | Low | Console | “Approve Deletion” still prompts for a PIN the handler no longer requires |

High = plausible missed alarm, wrong control target, or loss of durable operational state. Medium = bounded correctness, reliability, or operator-facing defect.

---

## 2. Coverage and Baseline

Reviewed in detail:

- [TankAlarm-112025-Server-BluesOpta.ino](../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino) — note pipeline, persistence, HTTP, 14 PROGMEM pages, `STYLE_CSS`
- [TankAlarm-112025-Client-BluesOpta.ino](../TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino) — sampling, alarms, daily, inbound commands, power
- [TankAlarm-112025-Viewer-BluesOpta.ino](../TankAlarm-112025-Viewer-BluesOpta/TankAlarm-112025-Viewer-BluesOpta.ino) — time sync and summary schedule
- Common headers: version, notefile schema, time helpers
- Live loopback: `http://127.0.0.1:60636/historical` (Chart.js failed; page showed zeros / empty filters)

Not exhaustively line-reviewed: generated binaries, every archived bench sketch, OptaView internals beyond prior Modbus notes, KeyProvisioning flash verify (already documented).

Working-tree noise excluded from this commit: client/Common/server source edits, CI workflow, older OTA review edits, compile logs.

---

## 3. Logic and Sequencing

### R-01 — Daily part 1 can clear an active alarm (High)

Client daily parts are **0-based**. Part 0 carries `alarms`; later parts do not.

```12991:12995:TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino
  // Check if this is part 0 (new) or part 1 (legacy) of the daily report
  uint8_t part = doc["p"].as<uint8_t>();
  bool isFirstPart = (part == 0 || part == 1);
```

```13157:13163:TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino
  int dailySchema = doc["_sv"] | 0;
  bool runAlarmReconcile = isFirstPart && (dailyAlarms || dailySchema >= 2);
```

For a current multipart report `{p:0, alarms:[…]}` then `{p:1, _sv:1}`:

1. Part 0 reconciles and confirms the high alarm.
2. Part 1 is also `isFirstPart`.
3. `dailyAlarms` is missing. If `_sv >= 2`, orphan-clear runs against an empty set and **clears** the alarm, including reminder snooze via `clearAlarmEvent`.

Even at `_sv == 1`, VIN/signal from part 0 can be overwritten by untrusted part-1 fields, and any future schema bump without fixing `isFirstPart` activates the clear.

```cpp
uint8_t part = doc["p"].as<uint8_t>();
int dailySchema = doc["_sv"] | 0;
// Modern clients (schema >= 1) are 0-based. Only unversioned legacy is 1-based.
bool isFirstPart = (dailySchema >= 1) ? (part == 0) : (part == 0 || part == 1);
bool runAlarmReconcile = isFirstPart && (dailyAlarms || dailySchema >= 2);
```

Also bump `NOTEFILE_SCHEMA_VERSION` if empty-`alarms` is now a required contract (R-21).

---

### R-02 — Clear Relay uses card order as device identity (High)

Dashboard cards stamp `sensorIdx` from `cs[].ts[]` array position, not `t.k`:

```js
sensors.forEach((t,idx)=>{ cl.sensors.push({ … sensorIndex:t.k||'', sensorIdx:idx, … }); });
// …
onclick="clearRelays('…', ${t.sensorIdx||0})"
```

The server forwards that number unchanged:

```11116:11140:TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino
static bool sendRelayClearCommand(const char *clientUid, uint8_t sensorIdx) {
  // …
  JAddNumberToObject(body, "relay_reset_sensor", sensorIdx);
```

The client indexes the **monitor array**:

```8896:8902:TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino
  // Command format: { "relay_reset_sensor": 0-7 }
  if (!doc["relay_reset_sensor"].isNull()) {
    uint8_t sensorIdx = doc["relay_reset_sensor"].as<uint8_t>();
    if (sensorIdx < MAX_MONITORS) {
      resetRelayForMonitor(sensorIdx);
```

If registry order is `[k:2, k:1]`, the card for sensor 2 sends `0` and resets monitor 0 (sensor 1). Snooze already passes `Number(t.sensorIndex)||1` — Clear Relay should do the same, and the client should match `monitors[i].sensorIndex`.

```cpp
uint8_t target = doc["relay_reset_sensor"].as<uint8_t>();
for (uint8_t i = 0; i < gConfig.monitorCount; ++i) {
  if (gConfig.monitors[i].sensorIndex == target) {
    resetRelayForMonitor(i);
    return;
  }
}
```

---

### R-03 — Analog debounce is not consecutive (High)

Digital path resets the trigger count when `!shouldAlarm`. Analog high/low do not:

```5983:6004:TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino
  if (highCondition && !state.highAlarmLatched) {
    state.highAlarmDebounceCount++;
    // …
  } else if (state.highAlarmLatched && highClearCondition) {
    state.highClearDebounceCount++;
    // …
  } else if (!highCondition && !highClearCondition) {
    state.highAlarmDebounceCount = 0;
  }
```

A sample fully below `highClear` while **not** latched hits none of those branches, so `highAlarmDebounceCount` is kept. Sequence `[90, 50, 90, 50, 90]` with debounce 3 can still latch HIGH. The latched clear path has the symmetric hysteresis-band hole.

Reset the relevant counter on every non-qualifying sample:

```cpp
if (!state.highAlarmLatched) {
  if (highCondition) {
    if (++state.highAlarmDebounceCount >= ALARM_DEBOUNCE_COUNT) { /* latch + send */ }
  } else {
    state.highAlarmDebounceCount = 0;
  }
} else if (highClearCondition) {
  if (++state.highClearDebounceCount >= ALARM_DEBOUNCE_COUNT) { /* clear + send */ }
} else {
  state.highClearDebounceCount = 0;
}
```

---

### R-04 — Latch-before-send plus offline skip can lose the only alert (High)

`evaluateAlarms` latches, then calls `sendAlarm`. `sendAlarm` may return in `checkAlarmRateLimit` with no pending retry. Separately:

```6427:6430:TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino
  if (gNotecardAvailable) {
    // build doc … publishNote(ALARM_FILE, doc, true);
  } else {
    Serial.print(F("Network offline - local alarm only …
```

`publishNote` already buffers when the Notecard is down. Skipping it drops the note. Boot timestamp init still clamps `expired = 0` when uptime is below the minimum interval; `(now - 0) < interval` then suppresses the first high/low/fault. The comment claiming wraparound is therefore wrong.

Always call `publishNote` for alarm-class events. Keep a `notificationPending` flag when rate-limited. Initialize last-alarm millis with unsigned wrap (`bootNow - (interval + 1)`), never clamp to 0.

---

### R-05 — Last-client delete is not durable (High)

```15612:15617:TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino
static void saveSensorRegistry() {
  if (!mbedFS || gSensorRecordCount == 0) {
    return;
  }
```

The same pattern exists in `saveClientMetadataCache`. Loop and delete handlers still set `gSensorRegistryDirty = false` after calling save. Deleting the last client leaves the old JSON on disk; reboot restores it. Write failures are silent.

Return `bool`, serialize `[]` when empty, and clear dirty **only** on success.

---

### R-06 — Dashboard Update has no client consumer (High)

Server `requestUpdate` writes `command.qo` / `_type: telemetry_request` into `telemetry_request.qi`. Client inbound polling covers config, relay, serial, location, and sync — **not** telemetry requests (`pollForTelemetryRequests` does not exist). The card shows “Update requested · ETA ≤ 10m/1h” forever.

Restore a bounded `note.get` on `TELEMETRY_REQUEST_FILE` on the inbound cadence, sample immediately, and publish.

---

### R-08 — Late clock sync: client re-arms, server/viewer do not (Medium)

Client:

```4501:4503:TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino
  if (gNextDailyReportEpoch <= 0.0 && gLastSyncedEpoch > 0.0) {
    scheduleNextDailyReport();
  }
```

Server `ensureTimeSync` only stores time. `scheduleNextDailyEmail` / `scheduleNextViewerSummary` ran once at boot with epoch 0 (`tankalarm_computeNextAlignedEpoch` returns 0). Loop conditions require `gNextDailyEmailEpoch > 0.0`. Failed `card.time` also retries **every loop** (no backoff). Viewer fetch has the same arming hole.

On first successful sync: set a `firstSync` flag, call the schedulers, and back off unsuccessful attempts (e.g. 30 s).

---

### R-09 — Failed config ACK ends retry (Medium)

```16280:16284:TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino
  if (version[0] != '\0' && strcmp(version, snap->configVersion) == 0) {
    snap->pendingDispatch = false;
    snap->dispatchAttempts = 0;
  }
```

Clear pending only for `st == "applied"`. On the client, do not treat `inboundTs <= configEpoch` as success when two configs share a truncated whole-second `_ts`.

---

### R-10 / R-13 — Snooze and relay timeout sequencing (Medium)

`applyReminderSnooze` broadcasts SMS/email, then the HTTP handler calls void `saveSensorRegistry()` and still returns `success: true`. Persist first; roll back RAM on failure; broadcast after.

`handleAlarm` keeps `alarmActive` on `relay_timeout` but overwrites `alarmType`. `checkAlarmReminders` only accepts high/low/triggered. Store timeout in a separate flag.

---

### R-11 / R-12 / R-19 / R-20 — Restore, metadata starvation, notes, FTPS (Medium)

- Restore reads into `char contents[2048]` while backup accepts ~24 KiB; success is “any file”; boot restore reloads config/calibration but **not** `loadSensorRegistry` / `loadClientMetadataCache`, so the next dirty save can smash restored files.
- Registry save stamps `gLastRegistrySaveMillis` before the metadata branch, so a dirty registry every 5 minutes can starve metadata forever.
- `processNotefile` handles then deletes; delete errors are ignored. Poison tracker is 12 slots vs 13 inbox files (`sms.qo` / `email` inbound included).
- `gWebServer.end()` then `performFtpBackup()` with 65 s inter-file waits (~8m40s) leaves alarms/HTTP unserved. Watchdog kicks are not a substitute for a cooperative backup state machine.

---

### R-14 / R-15 — Pulse sampling and power-state fallback (Medium)

`pollPulseSampler` is only invoked from `readPulseSensor` at the telemetry interval. A seconds-long window polled once per 30 minutes does not implement a pulse rate. Service it every `loop()` or use an interrupt/timer.

`updatePowerState()` sets `POWER_STATE_NORMAL` and returns when voltage is unknown, skipping relay restore, notifications, and debounce reset if the previous state was CRITICAL. Hold last known state and mark voltage invalid.

---

### R-18 — Settings 400 after mutation (Medium)

`handleServerSettingsPost` writes `productUid`, fleet, SMS flags, and `viewerEnabled` before `viewerNet` validation can `return` 400. Validate all dotted-quads first; mutate only after.

---

### R-21 — Schema comment vs constant

Client daily comments describe a schema-2 empty-`alarms` contract. [TankAlarm_Common.h](../TankAlarm-112025-Common/src/TankAlarm_Common.h) still has `#define NOTEFILE_SCHEMA_VERSION 1`. Receivers that key off `_sv >= 2` never see the intended behavior. Either bump `_sv` with a release note, or key the server off “alarms array present in part 0”.

---

## 4. Server Website — Bugs, Layout, Style

Live `/historical` on the shared loopback page: Chart.js failed to load (`Chart is not defined`), stats stayed 0, filters stayed “All Sites / All Sensors”, and `loadHistoricalData`’s catch path called `renderLevelChart()` again (uncaught `ReferenceError`).

### R-07 — Calibration identity (`dev:` UIDs)

Keys are `` `${t.client}:${t.sensorIndex}` ``. Submit and log filter use `split(':')`, so `dev:860322068056545:1` becomes client `dev` and sensor `860322068056545`. Backend checks nonempty UID, not “this sensor exists”.

```js
function parseSensorKey(value) {
  const sep = value.lastIndexOf(':');
  if (sep <= 0) return null;
  const clientUid = value.slice(0, sep);
  const sensorIndex = Number(value.slice(sep + 1));
  if (!clientUid || !Number.isInteger(sensorIndex) || sensorIndex < 1) return null;
  return { clientUid, sensorIndex };
}
```

### R-16 — History page

Issue #313 empty-`sites` crash is **fixed** (the loader now builds `sites` from `sensors`). Remaining defects:

| Symptom | Cause |
|---|---|
| Sensor dropdown jumps back to All | `change` → `loadHistoricalData()` → `populateFilters()` rebuilds options without restoring selection; values are array indexes |
| Custom range dates do not reload data | `onchange="renderLevelChart()"` only; no `loadHistoricalData` |
| PSI/RPM/GPM plotted as inches | API omits `ot`/`mu`; Y-axis and CSV hardcode `Level (inches)` |
| VIN chart is one line | `voltage` points from every client concatenated |
| CDN failure blanks the page | `catch` calls `initEmptyData()` then `new Chart` |
| “Sites & Sensors” does not expand | `.site-card` / `.site-content.expanded` are not in `STYLE_CSS` |

Guard Chart.js:

```js
if (typeof Chart === 'undefined') {
  document.getElementById('levelChart').parentElement.innerHTML =
    '<p class="empty-state">Charts unavailable (offline). Use Export CSV.</p>';
  return;
}
```

Preserve filter keys (`client:k` with `lastIndexOf`), include `mu`/`ot` in `/api/history`, and split VIN datasets by `client`.

### R-17 — Layout and style inconsistencies

`STYLE_CSS` `:root` defines `--card` but **not** `--card-bg`, `--accent`, or `--chart-grid`. Dashboard, Client Console, Site Config, and History still reference them (stat cards, spark fill, Chart.js grid, selected list row). `--radius:0` (square pills/cards) fights page-local `border-radius:8px` / `10px`.

Primary nav is Dashboard / Client Console / Contacts / Server Settings on most pages. Historical Data is a fifth pill **only on the dashboard**. Calibration is only from Client Console. Serial Monitor and Transmission Log are only from Settings. Contacts uses `btn btn-primary` with **no** `.btn-primary` rule (Add Contact falls back to the generic blue `button` plus a missing `.btn` class). Pause control is copy-pasted onto pages that are not the dashboard.

Phone issues: calibration/status tables have no wrapper overflow; Email/SMS setup `<pre>` blocks do not wrap; History canvases can keep a desktop width after resize.

Suggested shared tokens:

```css
:root {
  --accent: #2563eb;
  --card-bg: var(--card);
  --chart-grid: #e5e7eb;
  --radius: 6px;
}
.btn-primary { background: var(--primary); color: #fff; border: 1px solid var(--primary); }
.btn-danger { background: var(--danger); color: #fff; border-color: var(--danger); }
.table-scroll, .chart-container, pre {
  max-width: 100%;
  overflow-x: auto;
}
```

Add Historical + Calibration to the shared header (or a single overflow “More” menu) so operators are not hunting.

### Other UI notes

- Dashboard `formatLevel` treats `inches <= 0` as `--`, so a real empty tank looks like no data.
- Sparkline map still uses `t.sensorIndex||1`, so a phantom `k:0` series can overwrite `k:1` (Issue #313-1). Prefer `t.sensorIndex == null ? 1 : t.sensorIndex`.
- `STALE_MIN = 2940` is 49 hours; the UI no longer prints “>25h”, but the threshold is still undocumented and far past a missed daily.
- Client Console “Approve Deletion” `prompt()`s a PIN; `requireValidPin` only checks the session.
- Login shows a 5-second overlay even after `load` hides it; `pattern="\d{4}"` rejects any future longer PIN.
- Every authenticated page hits `/api/session/check` every 30 s — extra Opta HTTP work on a single-threaded server.

---

## 5. Improvements — Speed, Energy, Data, Website

### Speed

- Cooperative FTPS: one file per loop pass; keep `gWebServer` listening; service `pollNotecard` and reminders during cooldown.
- Stream `/api/history` to the socket instead of one `JsonDocument` + `String` for 20 × 90 points.
- `Cache-Control: public, max-age=3600` already exists for `/style.css`; add a weak ETag / version query (`/style.css?v=2.2.14`) so firmware updates bust cache.
- Pause dashboard `/api/clients` polling when `document.hidden`.

### Energy (solar clients)

- Keep switched loop excitation (DAC or PWM). Do not leave 4–20 mA transmitters powered between samples.
- Night Modbus: if panel voltage is ~0, poll SunSaver less often; cache failed setpoint probes.
- Notecard `periodic` outbound/inbound already matches solar; do not add extra `hub.sync` from the client on every alarm if a short outbound window is configured — batch recipients then one sync.

### Cellular / Notehub data

- Deadband telemetry: skip routine notes when `|Δlevel| < 0.5` (or configured) and no alarm/fault.
- Replay: drop intermediate routine telemetry, keep transitions + latest.
- History page: do not fetch `days=0` (full RAM dump) for a single sensor unless asked; 90 days is enough for the chart.

### Website

- One shared `nav.html` fragment (or generator in `update_html.py`) so labels, active pill, and logout stay identical.
- Self-host Chart.js in PROGMEM or a local `/vendor/chart.umd.js` for air-gapped plants.
- Replace duplicated pause/session IIFE with one small `/app.js` if flash budget allows; otherwise a Python extractor that fails CI on `new Function` / `SyntaxError`.

---

## 6. Suggested Remediation Order

1. **Alarm integrity:** R-01, R-03, R-04, R-13.  
2. **Wrong target / missing command:** R-02, R-06, R-07.  
3. **Durable state:** R-05, R-08, R-09, R-10, R-12.  
4. **Website:** R-16, R-17, Chart.js fallback, nav, CSS tokens.  
5. **Ops efficiency:** R-11, R-20, streaming history, polling/energy.

---

## 7. Verification Notes

| Check | Result |
|---|---|
| Server `arduino-cli compile` (this workspace) | Pass (exit 0) |
| `isFirstPart` / daily part 0 vs 1 | Confirmed in source |
| Clear Relay index path | Confirmed dashboard → API → client |
| Analog debounce branches | Confirmed; digital path is stricter |
| `pollForTelemetryRequests` | Absent from client |
| History live page | Chart.js `ReferenceError`; empty stats |
| Calibration `split(':')` | Confirmed in `CALIBRATION_HTML` |
| CSS `:root` missing tokens | Confirmed |
| Client daily re-arm vs server | Client has `updateDailyScheduleIfNeeded`; server/viewer do not |

Hardware, SMS, FTPS, and OTA were not exercised in this pass.

---

## 8. Out of Scope / Already Improved

- Issue #313 Client Console extra `}` / `normalizeApiData` parse failure: **not present** in current `CLIENT_CONSOLE_HTML`.
- History `sites` key: loader now synthesizes `sites` from `sensors`.
- v2.2.14 hash-table boot insert order (snooze 404 after reboot): already released.
- Atomic `posix_write_file` wrapper: present; the surrounding count-0 / dirty-flag contract is the remaining bug.
