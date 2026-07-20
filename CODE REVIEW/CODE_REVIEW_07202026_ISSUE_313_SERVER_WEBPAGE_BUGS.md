# CODE REVIEW — Issue #313: Server Webpage Errors and Bugs

**Date:** 2026-07-20
**Issue:** [SenaxInc/SenaxTankAlarm#313](https://github.com/SenaxInc/SenaxTankAlarm/issues/313) — "Server Webpage Errors and Bugs"
**Reviewed file (primary):** `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino` @ HEAD `bab0644` (v2.2.10)
**Live system used for verification:** server at `192.168.7.117` running v2.2.9 (fleet: 1 client `dev:860322068056545`, site "Silas", gas sensor "Cox Wellhead", sensorIndex 1)

Every bug below was **reproduced and root-caused against the live server**, not just by reading code. Where the live firmware (2.2.9) and repo HEAD (2.2.10) could differ, the relevant source was byte-compared and confirmed identical.

---

## 0. Executive Summary

| # | Issue-313 item | Verdict | Root cause (one line) | Severity |
|---|---|---|---|---|
| 1 | Dashboard trend graph "No trend data yet" | **CONFIRMED BUG** | Phantom `sensorIndex:0` history record + JS falsy-key collision `t.sensorIndex\|\|1` overwrites the real series with an empty one | High |
| 2a | /historical shows 0 sensors, empty Sites & Sensors | **CONFIRMED BUG** | Page requires `historicalData.sites` — the API **never emits a `sites` key** → `Object.keys(undefined)` throws → catch-all wipes the page to empty | High |
| 2b | "Enable FTP for full archive" despite FTP enabled | **CONFIRMED BUG** | `coldTierAvailable = gConfig.ftpEnabled && gHistorySettings.ftpArchiveEnabled`; `ftpArchiveEnabled` defaults `false` and has **no UI or API to turn it on** | Medium |
| 2c | VIN Voltage History empty | **CONFIRMED — collateral of 2a** | Live API has 131 voltage points; the `sites` crash resets the page before the chart renders | High |
| 3 | /client-console dropdown + Active Sites empty | **CONFIRMED BUG** | One extra `}` in `normalizeApiData` → **the entire page script fails to parse** (`SyntaxError: Unexpected token 'function'`). Broken since 2026-03-15 (commit `502bbcd`) | Critical |
| 4 | Contacts list bunched up, confusing "×" box | **CONFIRMED UX** | Card layout with inline alarm-association chips; the "×" is an unlabeled *remove-association* button | Low |
| 5 | Daily Report editor missing | **ALREADY EXISTS** — discoverability bug | `/email-format` page is implemented (and since v2.2.1 its settings are actually applied), but it is not linked from the Contacts "Daily Report" master card | Low |
| 6 | Settings: SMS recipients semantics, FTP save UX | **CONFIRMED UX + design gap** | Settings "Server SMS Alert Recipients" edits the *same* `smsAlertRecipients` list as the Contacts page; there is no dedicated server-events list. FTP section has Test/Backup/Restore but no adjacent Save/"Saved" feedback | Medium |
| 7 | Create this review document | This document | — | — |
| 8 | Viewer parity | Partial gap | Viewer got the v2.2.0 redesign (dashboard + contacts); server has since gained v2.2.1+ contact channels, opt-out badges, daily-schedule card | Low |

**Meta-finding:** items 1, 2a, 2c and 3 share two systemic causes — **(A)** hand-minified single-line JS inside PROGMEM strings with **no syntax validation**, and **(B)** frontend/backend JSON **contract drift** with all-or-nothing page rendering. See §9.

---

## 1. Bug 1 — Dashboard "Fleet Telemetry" trend graph always "No trend data yet"

### 1.1 Symptom (reproduced live)

Dashboard sparkline element renders `<span class="spark-label">No trend data yet</span>` for `dev:860322068056545|1` even though `/api/history?days=90` returns **90 readings** for that sensor.

### 1.2 Data flow

1. Dashboard JS fetches `/api/history?days=90` (DASHBOARD_HTML, ~line 2280) and builds a series map:
   ```js
   const map={};(data.sensors||[]).forEach(t=>{const key=(t.client||'')+'|'+(t.sensorIndex||1);map[key]=t.readings||[];});
   ```
2. Card HTML sets the lookup key the same way (~line 2298):
   ```js
   const sparkKey=escapeHtml(t._clientUid)+'|'+(t.sensorIndex||1);
   ```
3. `renderSparklines()` (~line 2285) draws only when `readings.length>=2`, else shows `No trend data yet` when `n===0`.

### 1.3 Root cause — verified live

The live `/api/history` returns **three** series:

```json
[{"client":"Client-112025","sensorIndex":1,"readings":25×Feb-27},
 {"client":"dev:860322068056545","sensorIndex":1,"readings":90×Jun-23→Jul-20},   ← the real sensor
 {"client":"dev:860322068056545","sensorIndex":0,"readings":47×Mar-27}]          ← PHANTOM
```

The phantom `sensorIndex:0` record comes from notes that arrived **without a `"k"` field** (a March 27 diagnostic/config burst — 47 notes in 20 seconds; cf. `CODE_REVIEW_07022026_POST_CONFIG_DIAG_BURST_AND_CONFIG_LOSS.md`). `handleTelemetry` defaults the missing key to 0:

```cpp
// line ~12391
uint8_t sensorIndex = doc["k"].as<uint8_t>();          // missing "k" -> 0
SensorRecord *rec = upsertSensorRecord(clientUid, sensorIndex);  // creates phantom record
```

The phantom was then **persisted forever** by the hot-tier snapshot (`/fs/history/hot_tier.json`, `saveHotTierSnapshot()` line ~8262 / `loadHotTierSnapshot()` line ~8325), so it survives every reboot.

In the dashboard JS, `t.sensorIndex||1` maps **0 → 1** (falsy coercion). The phantom appears *after* the real sensor in the array, so:

- key `dev:…545|1` is first set to the real 90 readings,
- then **overwritten** by the phantom's readings.
- With `?days=90`, the phantom's March readings are all older than the cutoff → `readings:[]` → `n===0` → **"No trend data yet"**.

This is a *last-writer-wins key collision*, not missing data. (Note: real client sensors are 1-based — `k` starts at 1 — so `k==0` never identifies a legitimate sensor.)

### 1.4 Fix (three layers — do all three)

**Fix 1a — server, stop creating phantom records (ingest guard).** In `handleTelemetry` (~line 12391):

```cpp
// BEFORE
uint8_t sensorIndex = doc["k"].as<uint8_t>();

// AFTER — reject notes that don't identify a sensor; k is 1-based on all clients
if (!doc["k"].is<int>() || doc["k"].as<int>() < 1) {
  addSerialLog("telemetry dropped: missing/invalid sensor index k");
  return;
}
uint8_t sensorIndex = doc["k"].as<uint8_t>();
```

(Same pattern already exists in spirit for system alarms: `handleAlarm`'s `isSystemAlarm` guard at line ~12681. `handleDaily` at ~line 13335 should get the same presence check.)

**Fix 1b — server, self-heal persisted phantoms on boot.** In `loadHotTierSnapshot()` (~line 8360), skip `k==0` entries when restoring:

```cpp
for (JsonObject sensorObj : arr) {
  if (gSensorHistoryCount >= MAX_HISTORY_SENSORS) break;
  uint8_t k = sensorObj["k"] | 0;
  if (k == 0) continue;   // Fix #313-1b: purge legacy phantom sensor-0 histories
  ...
}
```

The next `saveHotTierSnapshot()` rewrites the file without the phantom — no manual cleanup needed.

**Fix 1c — JS, remove the falsy collision (defense in depth).** In DASHBOARD_HTML, both key builders:

```js
// BEFORE (two places, ~offsets 366795 and 373994 in the file)
const key=(t.client||'')+'|'+(t.sensorIndex||1);
const sparkKey=escapeHtml(t._clientUid)+'|'+(t.sensorIndex||1);

// AFTER — 0 is a distinct (if invalid) index, never alias it onto 1
const key=(t.client||'')+'|'+(t.sensorIndex==null?1:t.sensorIndex);
const sparkKey=escapeHtml(t._clientUid)+'|'+(t.sensorIndex==null?1:t.sensorIndex);
```

**Optional Fix 1d — merge instead of overwrite** (protects against any future duplicate keys): `if(!map[key]||map[key].length<(t.readings||[]).length)map[key]=t.readings||[];`

---

## 2. Bug 2 — /historical page: 0 sensors, empty sites, no VIN history, wrong FTP banner

### 2.1 Symptom (reproduced live)

- "Total Sensors: 0", Sites & Sensors empty, Level Trends blank, VIN Voltage History empty.
- Browser console: `No historical data available: Cannot convert undefined or null to object`.
- Banner: `Data: RAM (730d) | Enable FTP for full archive` although FTP is enabled and connected (live `srv.ftp.enabled:true`, host reachable).

### 2.2 Root cause 2a — the API never emits `sites`, the page requires it

`loadHistoricalData()` (HISTORICAL_DATA_HTML, ~line 2180):

```js
const data=await res.json();
if(data.sensors&&data.sensors.length>0){historicalData=data;}   // <- adopts API object AS-IS
else{initEmptyData();}
...
updateStats();populateFilters();renderLevelChart();renderAlarmChart();renderVoltageChart();renderSites();
```

But `populateFilters`, `renderAlarmChart`, and `renderSites` all do:

```js
Object.keys(historicalData.sites)...        // historicalData.sites is UNDEFINED
```

`sendHistoryJson()` (line 16692→16905) emits only `sensors`, `alarms`, `voltage`, `settings`, `dataInfo` — **there is no `sites` key anywhere in the server response** (verified in source and live). Only `initEmptyData()` creates `sites:{}`:

```js
function initEmptyData(){historicalData={sites:{},sensors:[],alarms:[],voltage:[]};}
```

So the *success* path always throws at the first `Object.keys(historicalData.sites)`, the `catch` runs `initEmptyData()`, and the page renders as if the fleet were empty. **Every symptom in 2a and 2c is this single contract mismatch.** The live API actually holds 90 readings and 131 voltage points.

### 2.3 Fix 2a — derive `sites` client-side after load (smallest, no server RAM cost)

In `loadHistoricalData()` inside HISTORICAL_DATA_HTML:

```js
// BEFORE
if(data.sensors&&data.sensors.length>0){historicalData=data;}else{initEmptyData();}

// AFTER — normalize the shape the renderers expect
if(data.sensors&&data.sensors.length>0){
  historicalData=data;
  historicalData.sites={};
  historicalData.sensors.forEach(s=>{
    const site=s.site||'Unknown Site';
    (historicalData.sites[site]=historicalData.sites[site]||[]).push(s);
  });
  if(!historicalData.alarms)historicalData.alarms=[];
  if(!historicalData.voltage)historicalData.voltage=[];
}else{initEmptyData();}
```

This one change fixes: Total Sensors count, site filter dropdown, Sites & Sensors section, Level Trends, Alarm Frequency, **and** VIN Voltage History (2c) in a single stroke — they all render from the same object and currently die on the same line.

> Alternative (rejected): emitting a `sites` map from the server duplicates every sensor object in the JSON document and risks `doc.overflowed()` on the Opta. Deriving client-side is free.

### 2.4 Root cause 2b — `ftpArchiveEnabled` is an orphan flag

```cpp
// line 16875
dataInfo["coldTierAvailable"] = (gConfig.ftpEnabled && gHistorySettings.ftpArchiveEnabled);
```

- `gConfig.ftpEnabled` — **true** on the live server (Settings page FTP section).
- `gHistorySettings.ftpArchiveEnabled` — defaults **false** (line 812) and is only read/written via `/fs/history_settings.json` (`doc["ftpArchive"]`, lines 7866/7926). **No settings-page control, no POST endpoint, no /historical UI can set it** — routes only expose GET `/api/history`, `/compare`, `/archived`, `/yoy` (router lines 9320–9345). Live value confirmed: `settings.ftpArchiveEnabled:false`.

So the banner is *technically correct* ("history archival to FTP is off") but *operationally wrong* ("Enable FTP" — the operator already did, in the only place the UI offers).

Secondary issue: `coldTierAvailable` conflates "configured" with "working" — it never checks `lastFtpSyncEpoch`, unlike `warmTierAvailable` which checks `gWarmTierDataExists` (line 16874).

### 2.5 Fix 2b — expose the flag and make the banner honest

**(1) Auto-enable archive when FTP backup is enabled (recommended default-fix):** in `applyHistorySettingsFromJson`/`loadHistorySettings`, or simply at the daily archive check (line ~4723), treat "FTP enabled + never explicitly configured" as on. Simplest honest default — change the initializer and migrate:

```cpp
// HistorySettings initializer, line ~812
true,  // ftpArchiveEnabled — archive history whenever FTP is configured (was false)
```

**(2) Add the missing toggle + endpoint** so operators have explicit control:

```cpp
// Router (near line 9333):
} else if (method == "POST" && path == "/api/history/settings") {
  handleHistorySettingsPost(client, body);

// Handler:
static void handleHistorySettingsPost(EthernetClient &client, const String &body) {
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) { respondStatus(client, 400, F("Bad JSON")); return; }
  if (doc["ftpArchive"].is<bool>()) gHistorySettings.ftpArchiveEnabled = doc["ftpArchive"].as<bool>();
  if (doc["ftpSyncHour"].is<int>()) gHistorySettings.ftpSyncHour = constrain(doc["ftpSyncHour"].as<int>(), 0, 23);
  saveHistorySettings();
  JsonDocument out; populateHistorySettingsJson(out); respondJson(client, out);
}
```

UI: a toggle in the Settings FTP section (`<label class="toggle"><span>Archive history to FTP</span><input type="checkbox" id="ftpArchiveEnabled"></label>`) posted with the other FTP settings, **or** a small "Enable FTP archive" button directly on the /historical banner.

**(3) Make the banner state-aware** (replace the `else` branch in `loadHistoricalData`):

```js
if(data.dataInfo.coldTierAvailable){
  msg+=' + FTP Archive'+(data.settings.lastFtpSync>0?'':' (first sync pending)');
}else if(data.settings&&data.settings.ftpArchiveEnabled===false&&data.dataInfo.ftpConfigured){
  msg+=' | FTP connected — history archive OFF (enable in Settings)';
}else{
  msg+=' | Enable FTP for full archive';
}
```

with one extra server field: `dataInfo["ftpConfigured"] = gConfig.ftpEnabled;`

### 2.6 Bug 2c — VIN Voltage History

No separate defect: `renderVoltageChart()` returns early only when `historicalData.voltage` is empty, and the live API carries 131 voltage entries (59 fresh ones from the real sensor at 12.2–12.4 V). Fix 2a restores the chart.

**Improvement (user's suggestion — cards):** the voltage array interleaves clients (`voltObj["client"]` is emitted but unused by the page) and is not time-sorted across sensors. If the chart stays, group by client; better yet render **per-client cards** (current V, min/max 7d, sparkline):

```js
function renderVoltageCards(){
  const byClient={};
  (historicalData.voltage||[]).forEach(v=>{(byClient[v.client]=byClient[v.client]||[]).push(v);});
  const wrap=document.getElementById('voltageCards');wrap.innerHTML='';
  Object.keys(byClient).sort().forEach(uid=>{
    const pts=byClient[uid].sort((a,b)=>a.timestamp-b.timestamp);
    const cur=pts[pts.length-1],min=Math.min(...pts.map(p=>p.voltage)),max=Math.max(...pts.map(p=>p.voltage));
    const card=document.createElement('div');card.className='data-card';
    card.innerHTML=`<div class="dc-site">${escapeHtml(uid)}</div>
      <div class="dc-value">${cur.voltage.toFixed(2)} V</div>
      <div class="dc-meta">min ${min.toFixed(2)} / max ${max.toFixed(2)} V</div>
      <div class="dc-sparkline" data-volt-client="${escapeHtml(uid)}"></div>`;
    wrap.appendChild(card);
  });
}
```

(Reuses the dashboard's card CSS vocabulary; sparkline can reuse `drawMiniSparkline` already present on the page.)

---

## 3. Bug 3 — /client-console: empty client dropdown and Active Sites (CRITICAL)

### 3.1 Symptom (reproduced live)

Page chrome renders, but the client list shows nothing (not even "No clients available"), Active Sites is blank. Browser reports an **uncaught `SyntaxError: Unexpected token 'function'` at page load** — the entire main `<script>` block never executes: no fetch, no render, no error UI. Meanwhile `/calibration` works because it is a different page with valid JS (`/api/sensors`).

Live probe of all 13 pages confirmed **only /client-console** fails to parse:

```
/                OK | OK          /client-console  SYNTAX ERR [0] | OK
/contacts        OK | OK          /historical      OK | OK
/calibration     OK | OK          /server-settings OK | OK | OK
/site-config     OK | OK          /config-generator OK
/transmission-log OK | OK         /email-format    OK | OK
/serial-monitor  OK | OK          /email-setup     OK | OK      /sms-setup OK | OK
```

### 3.2 Root cause — one extra `}` in `normalizeApiData` (CLIENT_CONSOLE_HTML, ~line 2349)

Brace-depth analysis of the 924-char function (served bytes == repo bytes, verified): depth returns to 0 at char 793 — *before* `result.configs=...` — and goes **negative** at the final `}`:

```js
// BROKEN (current):
...measurementUnit:t.mu||''};})};})}};result.configs=(data.cfgs||[]).map(...);return result;}
//                              ^^^ one `}` too many — closes normalizeApiData here,
//                                  leaving `return result;` at top level → SyntaxError
```

Intended structure: `};})` closes the `t` map, `};})` closes the `c` map, then **one** `}` closes the `result` object literal and `;` ends the `var` statement.

Introduced by commit `502bbcd` "Rename monitor/tankNumber to sensorIndex" (**2026-03-15**) — a hand edit inside the single-line PROGMEM string. **The page has been dead for four months** without any build or test noticing, because C++ compiles string contents blindly.

### 3.3 Fix — remove the extra brace

Single-character fix inside CLIENT_CONSOLE_HTML:

```js
// FIND  (unique in file):
;})}};result.configs=

// REPLACE WITH:
;})};result.configs=
```

Resulting valid tail:

```js
...measurementUnit:t.mu||''};})};})};result.configs=(data.cfgs||[]).map(function(cfg){return{client:cfg.c||'',site:cfg.s||'',configJson:cfg.cj||''};});return result;}
```

### 3.4 Secondary defect on the same page — `cfgs` never arrives in summary mode

`normalizeApiData` maps `data.cfgs` and `populateClientList()` merges "Stored config" entries, but the page fetches `/api/clients?summary=1`, and `sendClientDataJson()` **returns before serializing `cfgs`** in summary mode (early return at line ~10499–10507; `cfgs` built at 10509+). Today this is masked: config-only clients still get a stub `cs[]` entry (uid/site/`u:0`, line ~10484) which the page renders as "Configured Client / Awaiting first report". So the dropdown will work after Fix 3.3 — but the `type:'config'` merge path and the `configJson` field are dead code.

**Options (pick one):**
- **(a) Slim the page** — drop `result.configs` and the `state.data.configs` merge (the `cs[]` stub covers the use case). Least flash, least contract surface.
- **(b) Honor the contract** — emit a *thin* `cfgs` in summary mode (uid/site only, no `cj` payload):

```cpp
if (summaryOnly) {
  JsonArray cfgsThin = doc["cfgs"].to<JsonArray>();
  for (uint8_t i = 0; i < gClientConfigCount; ++i) {
    if (gClientConfigs[i].uid[0] == '\0') continue;
    JsonObject e = cfgsThin.add<JsonObject>();
    e["c"] = gClientConfigs[i].uid;
    e["s"] = gClientConfigs[i].site;
  }
  if (doc.overflowed()) { ... }
  respondJson(client, doc);
  return;
}
```

Recommendation: **(a)** — the page never uses `configJson`, and thinner JSON matters on this platform.

---

## 4. Bug 4 — Contacts page layout ("bunched up", unclear "×")

### 4.1 Findings (CONTACTS_MANAGER_HTML, `renderContacts` ~line 2201)

- Each contact renders as a `.contact-card` with name+badges, stacked phone/email lines, SMS/Email checkboxes, Edit/Delete buttons, and *alarm-association chips* grouped by site.
- The confusing **"×" box** is the association chip's remove button:
  ```js
  `<div class="association-tag">${escapeHtml(alarm.label)}(${escapeHtml(alarm.type)})<button class="remove-tag" ...>&times;</button></div>`
  ```
  It **removes that alarm association from the contact** — with no tooltip, label, or confirmation. That is why "one of the sites shows in a confusing way with an x box next to it".

### 4.2 Fix — table layout with emoji actions (user's requested design)

Replace the card list with a CSS-grid table. Columns: **Name | Phone | Email | SMS | Email alerts | ✏️ | 🗑️**, associations demoted to a compact second row per contact.

```html
<style>
.contact-table{display:grid;grid-template-columns:1.4fr 1.1fr 1.6fr 70px 70px 44px 44px;gap:0;border:1px solid var(--card-border);border-radius:8px;overflow:hidden;}
.contact-table .ct-head{font-size:0.75rem;letter-spacing:0.5px;text-transform:uppercase;color:var(--muted);background:var(--chip);padding:8px 10px;}
.contact-table .ct-cell{padding:10px;border-top:1px solid var(--card-border);display:flex;align-items:center;gap:6px;min-width:0;}
.contact-table .ct-cell.center{justify-content:center;}
.ct-assoc{grid-column:1/-1;padding:0 10px 10px;display:flex;flex-wrap:wrap;gap:6px;}
.icon-btn{background:none;border:1px solid var(--card-border);border-radius:6px;padding:4px 8px;cursor:pointer;font-size:0.95rem;}
.icon-btn.danger:hover{background:#fee2e2;}
@media(max-width:720px){.contact-table{grid-template-columns:1fr 1fr;} .ct-head{display:none;}}
</style>
```

```js
function renderContacts(){
  const container=document.getElementById('contactsList');
  /* ...existing filtering unchanged... */
  let html='<div class="contact-table">'
    +'<div class="ct-head">Name</div><div class="ct-head">Phone</div><div class="ct-head">Email</div>'
    +'<div class="ct-head">SMS</div><div class="ct-head">Email alerts</div><div class="ct-head"></div><div class="ct-head"></div>';
  html+=filteredContacts.map(contact=>{
    const badges=(contact.cat==='viewer'?' <span class="badge">VIEWER</span>':'')
      +(contact.phone&&isOptedOut(contact.phone)?' <span class="badge badge-red">SMS OPT-OUT</span>':'')
      +(contact.email&&isEmailOptedOutJs(contact.email)?' <span class="badge badge-orange">EMAIL OPT-OUT</span>':'');
    const assoc=(contact.alarmAssociations||[]).map(id=>alarms.find(a=>a.id===id)).filter(Boolean)
      .map(a=>`<span class="association-tag">${escapeHtml(a.site)} — ${escapeHtml(a.label)}`
        +`<button class="remove-tag" title="Stop sending this alarm to ${escapeHtml(contact.name)}" aria-label="Remove alarm association"`
        +` data-contact-id="${escapeHtml(contact.id)}" data-alarm-id="${escapeHtml(a.id)}">&times;</button></span>`).join('');
    return `<div class="ct-cell">${escapeHtml(contact.name)}${badges}</div>`
      +`<div class="ct-cell">${escapeHtml(contact.phone||'—')}</div>`
      +`<div class="ct-cell">${escapeHtml(contact.email||'—')}</div>`
      +`<div class="ct-cell center"><input type="checkbox" class="chan-sms" data-contact-id="${escapeHtml(contact.id)}"${smsAlertRecipients.includes(contact.id)?' checked':''}${contact.phone?'':' disabled'}></div>`
      +`<div class="ct-cell center"><input type="checkbox" class="chan-email" data-contact-id="${escapeHtml(contact.id)}"${emailAlertRecipients.includes(contact.id)?' checked':''}${contact.email?'':' disabled'}></div>`
      +`<div class="ct-cell center"><button class="icon-btn" title="Edit contact" data-contact-id="${escapeHtml(contact.id)}" data-action="edit">&#x270F;&#xFE0F;</button></div>`
      +`<div class="ct-cell center"><button class="icon-btn danger" title="Delete contact" data-contact-id="${escapeHtml(contact.id)}" data-action="delete">&#x1F5D1;&#xFE0F;</button></div>`
      +(assoc?`<div class="ct-assoc">${assoc}</div>`:'');
  }).join('');
  container.innerHTML=html+'</div>';
  /* ...existing event wiring unchanged (selectors keep the same classes/data attrs)... */
}
```

Notes:
- Keep `&#x270F;&#xFE0F;` / `&#x1F5D1;&#xFE0F;` (pencil / trash) as HTML entities — raw emoji in the .ino risk encoding trouble in the single-line PROGMEM strings; entities are ASCII-safe (the codebase already uses `&#x26A0;` etc.).
- The `×` gets a `title` + `aria-label` and the chip now includes the site name, fixing "showing one of the sites in a confusing way".
- Event wiring is unchanged because classes and `data-` attributes are preserved.
- Remember: this HTML lives in a **single-line** raw string — author it multi-line, then re-join (see `build/_joinhtml.ps1` workflow from v2.2.0).

---

## 5. Bug 5 — "Daily Report editor" — it already exists; make it discoverable

### 5.1 Findings

- The editor is `/email-format` (EMAIL_FORMAT_HTML, line 1784) with ~30 layout options stored in `/fs/email_format.json`.
- Since v2.2.1 (EMAIL-1 fix), `sendDailyEmail()` actually **applies** these settings and attaches the whole `fmt` object to the outbound `email.qo` note for route templates — so the editor is functional, not vestigial.
- It is linked only from: Server Settings (small "Email formatting" text link, line 1761) and the /email-setup guide (line 1934). It is **not** linked from the Contacts page "Daily Report" master card, which is where issue 313 expected it (Contacts owns daily recipients/time/zone since v2.2.7).

### 5.2 Fix — one link + one title

In CONTACTS_MANAGER_HTML, inside the "Daily Report" card header/footer:

```html
<a class="pill secondary" href="/email-format" style="text-decoration:none;">Daily Report Editor</a>
```

And in EMAIL_FORMAT_HTML retitle the page (`<title>` + `<h1>/<h2>`) from "Email Format" to **"Daily Report Editor"** so operators searching for the issue-313 name find it. Optionally serve the same page at both routes:

```cpp
} else if (method == "GET" && (path == "/email-format" || path == "/daily-report-editor")) {
  serveFile(client, EMAIL_FORMAT_HTML);
```

---

## 6. Bug 6 — Server Settings: SMS list semantics, FTP save UX, autosave ambiguity

### 6.1 "Is SMS Alert Recipients a copy of the other lists?" — YES (verified)

- Settings-page `renderSmsRecipients()` renders `state.smsAlertRecipients` — the **same** `smsAlertRecipients` array in `/fs/contacts_config.json` that the Contacts page SMS checkboxes edit. Two UIs, one list (by design since v2.2.5/2.2.7, but nothing in the UI says so).
- Which triggers use it: `sendSmsAlert(message, alarmId)` resolves this list for **everything** — sensor alarms (association-filtered), reminders, unload SMS, *and* fleet/system alerts (OTA stalled, stale client, server-down, power-restore, test) which pass `alarmId=nullptr` and go to every recipient whose `alarmAssociations` is empty.
- Consequence the user intuited: **you cannot subscribe someone to server-health SMS without also subscribing them to every unassociated field alarm** (and vice versa: a field contact with no associations silently receives server-ops noise).

### 6.2 Fix — dedicated server-events recipient list

Add a third recipient array `serverSmsRecipients` (contacts doc), used only for server-origin events; fall back to the legacy behavior when empty so nothing breaks at upgrade:

```cpp
// New helper next to sendSmsAlert():
static uint8_t sendServerSmsAlert(const char *message) {
  // Resolve serverSmsRecipients from contacts config; if the array is missing or
  // empty, fall back to sendSmsAlert(message, nullptr) (legacy behavior).
  JsonDocument contactsDoc;
  if (!loadContactsConfig(contactsDoc) || contactsDoc["serverSmsRecipients"].size() == 0) {
    return sendSmsAlert(message, nullptr);
  }
  uint8_t queued = 0;
  for (JsonVariant idV : contactsDoc["serverSmsRecipients"].as<JsonArray>()) {
    const char *phone = resolveContactPhoneById(contactsDoc, idV.as<const char*>());
    if (!phone || !isRealPhoneNumber(phone) || isSmsOptedOut(phone)) continue;
    if (queueSingleSmsNote(message, phone)) queued++;      // same sms.qo {message,to} per-recipient note
    if (queued >= MAX_SMS_RECIPIENTS_PER_ALERT) break;
  }
  return queued;
}
```

Call sites to switch to `sendServerSmsAlert(...)`: OTA stalled (`reconcileClientOta`), stale-client, server-down-recovery, power-restore, and `POST /api/sms/test` — i.e. exactly the "server errors itself" class from the issue. Sensor/unload/reminder paths keep `sendSmsAlert(msg, alarmId)`.

Persistence: add `serverSmsRecipients` to the merge-keys and prune-keys sets in `handleContactsPost` / `handleViewerContacts` (the CONTACT-1 merge pattern, 4→5 keys) so partial saves from other pages don't drop it.

UI: the existing settings-page picker then edits `serverSmsRecipients` instead of `smsAlertRecipients`, retitled **"Server Event SMS Recipients"**, with helper text: *"Server-health alerts only (power, OTA, stale clients). Field alarm recipients are managed on the Contacts page."* — this also removes the duplicated-list confusion.

### 6.3 FTP "Save" button + "Saved" feedback

Findings: the whole settings page is one `<form>` with a single bottom **Save Settings** submit; the FTP section's buttons are Test Connection / Backup Now / Restore Now. Two real problems:

1. **Test Connection tests the last-saved config, not what's typed** (the handler reads `gConfig`), so "edit host → Test" silently tests the old host.
2. No local save affordance / no "Saved" confirmation near the section.

Fix — add a Save button next to Test Connection and make both explicit:

```html
<div class="actions">
  <button type="button" id="ftpSaveBtn">Save FTP Settings</button>
  <button type="button" class="secondary" id="ftpTestNow">Test Connection</button>
  <button type="button" id="ftpBackupNow">Backup Now</button>
  <button type="button" class="secondary" id="ftpRestoreNow">Restore Now</button>
  <span id="ftpSaveStatus" style="font-size:0.85rem;color:var(--muted);"></span>
</div>
```

```js
async function saveFtpOnly(){
  els.ftpSaveStatus.textContent='Saving...';
  try{
    await saveSettingsImpl();                 // existing impl already collects the FTP fields
    els.ftpSaveStatus.textContent='\u2713 Saved';
    els.ftpSaveStatus.style.color='#059669';
  }catch(e){
    els.ftpSaveStatus.textContent='Save failed';
    els.ftpSaveStatus.style.color='#dc2626';
  }
  setTimeout(()=>{els.ftpSaveStatus.textContent='';},4000);
}
els.ftpSaveBtn.addEventListener('click',saveFtpOnly);
// Test should never test stale config:
els.ftpTestNow.addEventListener('click',async()=>{await saveFtpOnly();runFtpTest();});
```

(`saveSettingsImpl` currently returns `undefined` and toasts internally — refactor it to `return fetch(...)` so callers can await/chain; the existing submit handler keeps working.)

### 6.4 "Which settings auto-save?" — none on this page; adopt one convention

Audit result: the settings page has **no** auto-save (`addEventListener('change'` count: 0 for persistence; only the Save submit persists). Pages that *do* save immediately: Contacts (checkbox toggles call `saveData()`), config-generator message-contacts picker, calibration entries. The inconsistency is real but is *between pages*, not within this one.

Recommended convention (site-wide, cheap):

1. **Toggle-switch styling for instant-apply controls, checkboxes for form fields.** The CSS is ~10 lines and touch-friendly:

```css
.switch{position:relative;display:inline-block;width:40px;height:22px;flex:none;}
.switch input{opacity:0;width:0;height:0;}
.switch .slider{position:absolute;inset:0;background:#cbd5e1;border-radius:22px;transition:.15s;cursor:pointer;}
.switch .slider:before{content:"";position:absolute;height:16px;width:16px;left:3px;top:3px;background:#fff;border-radius:50%;transition:.15s;}
.switch input:checked + .slider{background:var(--accent);}
.switch input:checked + .slider:before{transform:translateX(18px);}
```

```html
<label class="toggle"><span>Enable FTP</span>
  <span class="switch"><input type="checkbox" id="ftpEnabled"><span class="slider"></span></span>
</label>
```

2. **Every instant-apply control fires a shared `savedFlash()`** (small "✓ Saved" toast) so behavior is self-evident.
3. Sections that require the Save button get a single muted footer line: *"Changes in this section apply when you press Save Settings."*

---

## 7. Issue item 8 — Viewer webpage parity

Verified state (viewer .ino @ `d5f8c0c`, constants `VIEWER_DASHBOARD_HTML` L269, `VIEWER_CONTACTS_HTML` L271):

| Feature | Server | Viewer | Assessment |
|---|---|---|---|
| Dashboard: stats row, active-alarm section, site-grouped cards | ✓ | ✓ (v2.2.0 redesign) | OK |
| Request Update button | n/a | ✓ | OK |
| GitHub update banner | ✓ | ✓ | OK |
| Trend sparklines | ✓ (`/api/history`) | ✗ | **Acceptable gap** — viewer's summary note (~KB over cellular) can't carry 90 readings/sensor; if desired, embed a downsampled 8-point series per sensor in `publishViewerSummary()` |
| VIN / signal display | ✓ | ✗ | Small gap — `vc` summary could carry `v`/`sig` per client cheaply |
| Contacts page | ✓ name/phone/email + SMS/email channel checkboxes + opt-out badges + daily schedule | ✓ name/phone/email add/remove (max 12, E.164) — **no channel checkboxes, no opt-out badges** | Real gap but by design: viewer contacts auto-enroll both channels server-side (v2.2.1); document this on the viewer page rather than duplicating the channel UI |
| Login/session auth | ✓ | ✗ (LAN-trust) | Intentional (documented model) |
| Historical / calibration / config pages | ✓ | ✗ | Intentional — viewer is a display terminal |

**Recommended parity actions (small):**
1. Viewer contacts page: add a note *"Alert channels and opt-outs are managed on the server Contacts page; contacts added here receive SMS and email alerts automatically."*
2. Add `v` (VIN) per client to `publishViewerSummary()` and render it in the viewer card meta (one field, negligible payload).
3. Defer sparklines unless requested — cellular cost.

---

## 8. Other bugs found during this review (not in issue 313)

| # | Finding | Location | Suggested action |
|---|---|---|---|
| O-1 | **`hotTierSnapshots` only inspects slot 0** — `(gSensorHistoryCount > 0 && gSensorHistory[0].snapshotCount > 0)`; wrong if slot 0 is an empty/phantom sensor | line 16873 | `bool any=false; for(...) if(gSensorHistory[i].snapshotCount){any=true;break;}` |
| O-2 | **Bench/default-UID pollution**: `Client-112025` (compile-default UID) occupies a history slot and a registry record forever | live data; `upsertSensorRecord` | Reject or quarantine records whose UID equals the firmware default (`Client-112025`) unless explicitly allowed; add a "Delete history for this sensor" admin action (see O-3) |
| O-3 | **No cleanup API for history records** — phantoms (k=0, stale bench clients) persist in `/fs/history/hot_tier.json` forever; the only remedy is reflash+file wipe | — | `POST /api/history/delete {client,sensorIndex,pin}` → remove `gSensorHistory` entry + `saveHotTierSnapshot()`; wire a small 🗑️ next to each series in the /historical Sites & Sensors list |
| O-4 | **`voltage` array is not time-sorted and interleaves clients**; chart code draws it as one series → sawtooth artifacts once 2a is fixed and a second client reports | line 16827-16843 + page chart | Sort by timestamp server-side or group per client in JS (see §2.6 cards) |
| O-5 | **Range UI over-promises**: hot ring is `MAX_HOURLY_HISTORY_PER_SENSOR = 90` snapshots (≈2–27 days depending on cadence) while the page offers "Last 2 Years" and `hotTierDays` reports 730 | lines 746-773 vs HISTORICAL_DATA_HTML range select | Either surface *actual* oldest-snapshot age in `dataInfo` (`doc["oldestEpoch"]`) and grey out impossible ranges, or raise the ring for small fleets. Warm tier (daily summaries) covers the long ranges only after `flushDailySummariesToWarmTier` runs — banner should say which tier served the data (`dataInfo["source"]` is hardcoded `"hot"`) |
| O-6 | **`dataInfo["source"]` is hardcoded `"hot"`** even when warm/cold data was merged | line 16872 | Set it from the actual code path when warm/FTP merge is implemented for the main endpoint |
| O-7 | **Duplicate snapshots with identical timestamp+level** exist in the live ring (e.g. two `t:1782203479, level:0`) — dedupe at `recordTelemetrySnapshot` only compares the immediately-previous write | line ~7475 | Acceptable; optional: scan last N snapshots or key dedupe on epoch only |
| O-8 | **`/api/clients?summary=1` early-return omits `cfgs`** that two page scripts still map (client-console, site-config) — dead code paths / latent contract drift | line 10499-10509 | Resolve per §3.4 (a) or (b) |
| O-9 | Contacts page association chip shows `label(type)` but omits the site inside the chip when grouped under a site header — after the §4 table redesign, chips are flat, so include the site in the chip text (done in §4 example) | renderContacts | Included in §4 fix |
| O-10 | `/api/history` correctly requires a session (verified 401 unauthenticated) — **no action**; recorded because the sparkline fetch omits credentials-sensitive headers and works only due to the global fetch wrapper. Keep the wrapper pattern when adding pages | — | None |

---

## 9. Commonalities — why these bugs happened, and how to stop the class

### 9.1 Common cause A: hand-minified single-line JS inside PROGMEM strings, never syntax-checked

Bug 3 (extra `}`) shipped in a *rename* commit and survived **4 months** across ~40 releases because nothing parses the JS: the C++ compiler sees an opaque string; there are no browser tests. The v2.2.6 post-mortem ("escapeHtml missing in page scope") is the same class: **each page is a separate JS scope with no static validation.**

**Recommended guard (cheap, catches the whole class):** a repo script that extracts every `<script>` block from every `*_HTML` PROGMEM constant and syntax-checks it with Node (`new Function` semantics, same engine family as the field browsers). Run locally before commit and as a CI step:

```powershell
# build/check_html_js.ps1 — extract PROGMEM HTML, join raw-string segments, node --check each script block
$ino = Get-Content "TankAlarm-112025-Server-BluesOpta\TankAlarm-112025-Server-BluesOpta.ino" -Raw
$fail = $false
# Join the R"HTML(...)HTML" segment splits, then walk each constant
$pattern = 'static const char (\w+_HTML)\[\] PROGMEM = (R"HTML\(.*?\)HTML";)'
foreach ($m in [regex]::Matches($ino, $pattern, 'Singleline')) {
  $name = $m.Groups[1].Value
  $html = $m.Groups[2].Value -replace '\)HTML"\s*R"HTML\(', '' -replace '^R"HTML\(', '' -replace '\)HTML";$', ''
  $i = 0
  foreach ($s in [regex]::Matches($html, '<script>(.*?)</script>', 'Singleline')) {
    $js = $s.Groups[1].Value
    $tmp = New-TemporaryFile
    Set-Content $tmp.FullName $js -Encoding utf8
    node --check $tmp.FullName 2>$null   # node --check accepts top-level return? no → wrap:
    if ($LASTEXITCODE -ne 0) {
      node -e "try{new Function(require('fs').readFileSync('$($tmp.FullName.Replace('\','/'))','utf8'))}catch(e){console.error(e.message);process.exit(1)}"
      if ($LASTEXITCODE -ne 0) { Write-Host "SYNTAX ERROR in $name script[$i]"; $fail = $true }
    }
    Remove-Item $tmp.FullName; $i++
  }
}
if ($fail) { exit 1 } else { Write-Host "All PROGMEM page scripts parse OK" }
```

Also apply the segment-split-tolerant grep lesson (v2.2.6): the `funct)HTML" R"HTML(ion` splits mean plain-text searches for `function foo` miss definitions — always regex with optional split, or run this checker which joins segments first.

### 9.2 Common cause B: frontend/backend JSON contract drift with all-or-nothing rendering

Three independent instances in this one issue: `sites` (never emitted), `cfgs` (dropped by a later `summary=1` optimization), `sensorIndex` semantics (server emits 0-valued keys the JS coerces away). The pages adopt whole API objects and one missing key kills everything downstream (`initEmptyData()` wipe; page-wide try/catch).

**Recommendations:**
1. **Normalize at the boundary, render from the normalized shape** — every page should have exactly one `normalizeApiData()`-style function that fills defaults for *every* key the renderers touch (`sites:{}`, `alarms:[]`, `voltage:[]`, …). The /historical fix in §2.3 is this pattern.
2. **Per-section try/catch** instead of page-wide: a broken VIN chart should not blank the sensors list. Pattern:
   ```js
   [['updateStats',updateStats],['populateFilters',populateFilters],['renderLevelChart',renderLevelChart],
    ['renderAlarmChart',renderAlarmChart],['renderVoltageChart',renderVoltageChart],['renderSites',renderSites]]
     .forEach(([name,fn])=>{try{fn();}catch(e){console.error(name+' failed:',e);}});
   ```
3. **Write the contract down once:** a `API_CONTRACTS.md` table (endpoint → keys → consumer pages) so a server-side key rename forces a grep of listed consumers. The `cs`/`cfgs`/`sensors` shapes are already stable enough to document in an afternoon.

### 9.3 Common cause C: sentinel/default values standing in for "absent"

- `doc["k"].as<uint8_t>()` silently manufactures sensor 0 (bug 1's phantom).
- `t.sensorIndex||1` silently merges 0 into 1 (bug 1's collision).
- `Client-112025` default UID pollutes registries (O-2).

**Rule to adopt:** at every ingest boundary, use ArduinoJson presence checks (`doc["k"].is<int>()`) before defaulting, and in JS never use `||` on numeric fields (`==null ? default :`). Both fixes in §1.4 are instances.

### 9.4 Common cause D: features gated by flags with no UI (dark flags)

`gHistorySettings.ftpArchiveEnabled` (bug 2b) is settable only by hand-editing a JSON file over the filesystem. Same smell previously: `smsReminderHours` shipped API-only (v2.1.6 note). **Rule:** a persisted flag ships in the same release as its settings-page control, or it defaults to the permissive value.

---

## 10. Prioritized fix plan

| P | Fix | Effort | Files |
|---|---|---|---|
| **P0** | §3.3 client-console extra `}` (one char) | trivial | Server .ino (CLIENT_CONSOLE_HTML) |
| **P0** | §2.3 /historical derive `sites` client-side | small | Server .ino (HISTORICAL_DATA_HTML) |
| **P0** | §1.4a+b+c phantom guard + hot-tier purge + `||1` fix | small | Server .ino |
| **P1** | §2.5 `ftpArchiveEnabled` toggle + honest banner (+ default true) | small | Server .ino |
| **P1** | §6.3 FTP Save button + save-before-test | small | Server .ino (SERVER_SETTINGS_HTML) |
| **P1** | §9.1 PROGMEM JS syntax checker in CI | small | new script + workflow step |
| **P2** | §6.2 `serverSmsRecipients` split | medium | Server .ino (contacts config + call sites) |
| **P2** | §4.2 contacts table layout + labeled × | medium | Server .ino (CONTACTS_MANAGER_HTML) |
| **P2** | §5.2 Daily Report Editor link/title | trivial | Server .ino |
| **P2** | §2.6 VIN per-client cards | medium | Server .ino (HISTORICAL_DATA_HTML) |
| **P3** | §7 viewer parity notes + VIN in summary | small | Viewer .ino + server `publishViewerSummary` |
| **P3** | §8 O-1..O-6 hygiene items | small each | Server .ino |

Deployment note: all fixes are server/viewer-side — **no client firmware or Notehub route changes**. A single server version bump + USB reflash (or release) covers everything; watch the post-DFU Ethernet-relink flakiness documented in the build notes.

---

## 11. Verification appendix (what was actually tested live, 2026-07-20)

1. `POST /api/login {pin}` → session; `GET /api/clients?summary=1` → `cs[]` contains the live client with fresh telemetry (`u=1.7845e9`, `l=33`, `ma=14.56`) — dashboard and client-console consume identical data.
2. `GET /api/history?days=90` → 3 series: `Client-112025|1` (0 in window), `dev:…545|1` (**90 readings**), `dev:…545|0` (**phantom**, 0 in window); `voltage:84–131` entries; `alarms:0`.
3. Dashboard in browser: sparkline key `dev:…545|1` → "No trend data yet"; replicating the page's own map code in-page shows the phantom overwriting the real series (`apiKeys: [.. ['dev:…545|1', 0]]`).
4. /client-console in browser: `pageerror SyntaxError: Unexpected token 'function'`; served script block byte-identical to repo; brace-depth scan pinpoints the extra `}` (§3.2); `git log -S` dates it to `502bbcd` (2026-03-15).
5. /historical in browser: console warning `Cannot convert undefined or null to object`; `statTotalTanks=0`; banner text and colors as reported; API `settings.ftpArchiveEnabled:false` while `srv.ftp.enabled:true`.
6. All 13 pages' script blocks parse-checked in-browser: only /client-console fails.
7. `GET /api/history` without session → **401** (auth coverage confirmed).

---

*Prepared for issue #313. Companion documents: `CODE_REVIEW_07022026_POST_CONFIG_DIAG_BURST_AND_CONFIG_LOSS.md` (source of the March diag burst that created the phantom), `CODE_REVIEW_07062026_SMS_PIPELINE_END_TO_END.md` (recipient-list architecture referenced in §6).*
