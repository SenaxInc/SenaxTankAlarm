# Comprehensive Code Review: Recent Merged PRs #318-#321 & Follow-on PRs #322-#326

- Date: 2026-09-24
- Reviewer: GitHub Copilot
- Review baseline: `c9a9cceae06ae1879216bf07bb8db5bea3a3a644` (master, release v2.2.16)
- In-flight branches examined: `feat/opta-io-scaffold` (PR #322), `fix/stable-sensor-numbers` (PR #323), `fix/clear-relay-by-number-client` (PR #324), `fix/digital-display-server` (PR #325), `fix/display-number-emails` (PR #326)

---

## Executive Summary

This review provides an independent technical assessment of the changes introduced in the recent release group (PRs #318-#321, v2.2.16) as well as the immediate follow-on relay/float project PRs (#322-#326). 

The review analyzed:
1. Debounce and rate-window correctness in [TankAlarm-112025-Client-BluesOpta/TankAlarm_AlarmDebounce.h](TankAlarm-112025-Client-BluesOpta/TankAlarm_AlarmDebounce.h#L25) and [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L6440).
2. Warm history streaming, month rollups, and filesystem write semantics in [TankAlarm-112025-Server-BluesOpta/WarmTierStore.h](TankAlarm-112025-Server-BluesOpta/WarmTierStore.h#L690).
3. Deduplication in the Apps Script bridge on [TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2086).
4. Operator time formatting, timestamp truncation, and on-demand UI polling.
5. In-flight relay and digital/float changes across PRs #322-#326, including cross-PR dependencies, backward compatibility, and test portability.

Fourteen findings were identified (1 P1 defect, 12 P2 operational/data-integrity issues, and 1 P3 cross-platform test defect).

---

## Findings Summary

| ID | Priority | Scope | Area | Description |
| --- | --- | --- | --- | --- |
| R01 | P1 | PR #319 | Embedded File System | LittleFS atomic rename semantics can cause month file commit failure |
| R02 | P2 | PR #321 / #318 | Operator Dashboard / UI | Same-minute on-demand update responses remain stuck as "Update requested" |
| R03 | P2 | PR #319 | History Admission | Daily history admission gate discards legitimate zero-level readings |
| R04 | P2 | PR #319 | History Provenance | Voltage 1-hour age assumption allows prior-day readings into warm history |
| R05 | P2 | PR #319 | Warm Tier Rollup | Smaller retained sample sets discard legitimate late alarm count updates |
| R06 | P2 | PR #319 | History Ingestion | Legacy unverified entries block replay of verified readings into warm tier |
| R07 | P2 | PR #318 | Client Alarms | Digital float switch alarms bypass the 300 s minimum interval rate limiter |
| R08 | P2 | PR #324 | Relay Protocol | Client PR #324 ignores dashboard Clear Relay until unreleased Server S5 lands |
| R09 | P2 | PR #324 | Relay Actuation | Global 5 s relay cooldown drops commands across different sensors |
| R10 | P2 | PR #325 | Daily Reconciliation | Daily reconciliation ignores missed alarms on uncached/new sensors |
| R11 | P2 | PR #325 | Current Loop Ingestion | Stale $\ge 4.0\text{ mA}$ clamp in daily report ingestion zeroes live-zero current |
| R12 | P2 | PR #326 | Display Number | `handleAlarm` Display Number updates bypass sanitization and dirty tracking |
| R13 | P2 | PR #326 | Email Formatting | Daily email summary outputs `(0 mA)` for analog voltage and pulse sensors |
| R14 | P2 | PR #326 | Message Formatting | Snooze/resume SMS overflows 160-char buffer for digital float alarms |
| R15 | P3 | PR #323 | Host Test Portability | Generator numbers host test fails on Windows due to hardcoded Unix LF endings |

---

## Detailed Findings: Merged PRs #318-#321 (v2.2.16)

### R01 - P1: LittleFS atomic rename semantics can cause month file commit failure

- **Locations:** [TankAlarm-112025-Server-BluesOpta/WarmTierStore.h](TankAlarm-112025-Server-BluesOpta/WarmTierStore.h#L735-L755).
- **Classification:** Critical durability / persistence defect in embedded filesystem integration.

In `WarmAtomicWriter::commit()`, the final step executes `WARM_RENAME(tmp, path)`. When `moveAsideTo` is null (the standard rewrite path), the destination `path` already exists on flash.
Under standard desktop POSIX (where host tests execute), `rename(old, new)` atomically replaces an existing `new` file. However, under Mbed OS LittleFS on the Arduino Portenta/Opta core, `rename()` fails with error `EEXIST` or `ENOTEMPTY` if the destination path exists.
When `WARM_RENAME` returns non-zero:
```cpp
if (ok && WARM_RENAME(tmp, path) != 0) {
  if (moveAsideTo && WARM_RENAME(moveAsideTo, path) != 0) keepTmp = true;
  ok = false;
}
if (!ok) {
  failed = true;
  if (tmp[0] && !keepTmp) WARM_REMOVE(tmp);
}
```
The failure causes `commit()` to delete the temporary file and return `false`. Consequently, once an initial month file is created, subsequent daily rollups attempting to rewrite that month file will consistently fail to commit, discarding updated daily rows.

- **Suggestion:**
Explicitly handle destination replacement for LittleFS:
1. Verify behavior on physical Opta LittleFS with a dedicated sketch or bench test.
2. If `rename()` over an existing file is rejected by the underlying LittleFS block device, perform a staged rename: rename the existing file to a temporary backup (such as a backup file path), rename the temporary file to the destination path, and then unlink the backup file.

- **Regression coverage:**
Add a LittleFS mock test that simulates POSIX vs LittleFS `rename()` behavior when the destination exists.

---

### R02 - P2: Same-minute on-demand update responses remain stuck as "Update requested"

- **Locations:**
  - [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4385) (`noteEpochMinute`)
  - [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L6590) (`sendTelemetry`)
  - [TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2471) (`sensorUpdatePending`, `sensorUpdateExpired`)
  - [TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L10669) (`clientObj["ureq"]`)
  - [TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11327) (`meta->updateRequestedEpoch`)

- **Classification:** UX / operational edge case introduced by integer/minute timestamp truncation.

PR #318 and PR #321 introduced integer-minute truncation (`noteEpochMinute()`) on the wire for client readings, and integer-second truncation for server-side `u` in `/api/clients`.
When an operator clicks "Update" on the web dashboard:
1. The server records `meta->updateRequestedEpoch = now;` (where `now` is floating-point `currentEpoch()`, e.g. 12:34:25.400).
2. The server outputs `clientObj["ureq"] = meta->updateRequestedEpoch;` in `/api/clients`.
3. The client promptly services the request and returns a reading with `t` truncated to the minute: 12:34:00.
4. On the dashboard, `sensorUpdatePending(t)` checks:
   ```javascript
   const ref = t.lastUpdate || 0;
   if (ref > t._updateRequestedEpoch) return false;
   return (Date.now()/1000 - t._updateRequestedEpoch) <= maxUpdateWaitSec(t._isSolar);
   ```
For any update answered within the same calendar minute where the reading's timestamp represents the update time, $12:34:00 \le 12:34:25$. The card remains stuck displaying "⏱ Update requested" for the entire 10-minute (or 60-minute solar) window, and subsequently transitions to "Retry Update" despite having received fresh data.

- **Suggestion:**
Correlate update requests using a monotonically incremented request counter or explicit request ID in the telemetry request and response notefiles, or compare at integer-minute granularity ($ref \ge \lfloor t.\_updateRequestedEpoch / 60 \rfloor \times 60$) while verifying sample freshness.

- **Regression coverage:**
Simulate an on-demand update request initiated at :20s and serviced at :45s of the same minute; assert that the pending badge clears immediately upon receipt of the fresh telemetry note.

---

### R03 - P2: Daily history admission gate discards legitimate zero-level readings

- **Locations:** [TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L13681), [TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L7664).
- **Classification:** History data omission.

In [TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L13681), the daily report admission condition requires `newLevel > 0.0f`.
A valid measurement of exactly $0.0\text{ in}$ (empty tank), $0.0\text{ psi}$, or an inactive digital float switch ($0.0$) is discarded from historical rollup. On clients where delta-based telemetry is silenced and daily reports provide the primary data, a tank resting at zero level will register as an unrecorded gap in daily history rather than a verified zero reading.

- **Suggestion:**
Permit $0.0$ when accompanied by valid acquisition metadata (`!doc["ru"]` and `!doc["sf"]`), distinguishing genuine zero measurements from default/uninitialized states.

- **Regression coverage:**
Send daily notes with level $0.0$, valid $4.0\text{ mA}$, and digital float state $0.0$; verify that each creates an entry in the hot ring and warm rollup.

---

### R04 - P2: Voltage 1-hour age assumption allows prior-day readings into warm history

- **Locations:**
  - [TankAlarm-112025-Server-BluesOpta/WarmTierStore.h](TankAlarm-112025-Server-BluesOpta/WarmTierStore.h#L393) (`warmVinOnReadingDay`)
  - [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L8180)
- **Classification:** Data provenance integrity.

The server helper `warmVinOnReadingDay()` assumes that system voltage was acquired within 3,600 seconds prior to note composition. However, the client allows `solarCharger.pollIntervalSec` up to 3,600 seconds and retains cached readings across up to four consecutive failed polls while `isCommunicationOk()` remains true.
A voltage measured at 23:30 followed by three failed polls will still be attached to a note generated at 01:05 the next day. The server predicate evaluates the note as belonging to the new day and records yesterday's battery voltage into today's warm history row.

- **Suggestion:**
Explicitly timestamp the voltage measurement on the client, or attach voltage age in seconds, ensuring history records only same-day voltage.

- **Regression coverage:**
Feed a telemetry payload with a sample timestamp at 01:05 UTC containing voltage measured at 23:30 UTC of the prior day; verify rejection from today's warm summary.

---

### R05 - P2: Smaller retained sample sets discard legitimate late alarm count updates

- **Locations:**
  - [TankAlarm-112025-Server-BluesOpta/WarmTierStore.h](TankAlarm-112025-Server-BluesOpta/WarmTierStore.h#L595)
  - [TankAlarm-112025-Server-BluesOpta/WarmTierStore.h](TankAlarm-112025-Server-BluesOpta/WarmTierStore.h#L792)
- **Classification:** Rollup aggregation edge case.

In `warmDecideVisitor()`, an incoming row update is marked superseded whenever the previously stored sample count `n` exceeds the recomputed row's count `nr.n`.
If a day had $n = 48$ samples and $al = 0$, and subsequent samples overwrite older entries in the bounded RAM ring so only $n = 40$ remain for that day, the arrival of a delayed alarm marks the day dirty. When recomputed, the row has $n = 40, al = 1$. Because $48 > 40$, the entire recomputed row is rejected, and the stored row remains at $al = 0$.

- **Suggestion:**
Decouple alarm event reconciliation from sample aggregate replacement, allowing `al = max(stored.al, new.al)` without corrupting high/low/average level metrics.

- **Regression coverage:**
Simulate a day where ring capacity has partially evicted older samples; deliver a late alarm and verify that `al` increments in the stored month file.

---

### R06 - P2: Legacy unverified entries block replay of verified readings into warm tier

- **Locations:**
  - [TankAlarm-112025-Server-BluesOpta/WarmTierStore.h](TankAlarm-112025-Server-BluesOpta/WarmTierStore.h#L369)
  - [TankAlarm-112025-Server-BluesOpta/WarmTierStore.h](TankAlarm-112025-Server-BluesOpta/WarmTierStore.h#L618)
  - [TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L7704)
- **Classification:** Upgrade transition defect.

The v2.2.16 upgrade marks pre-upgrade snapshots as legacy (`legacyCount`) so they are charted but excluded from daily rollups. However, `warmRingHasAcquisition()` checks for duplicates across all snapshots without checking the legacy flag.
If a verified reading arrives whose acquisition timestamp matches a legacy entry, the duplicate check returns true and drops the reading. The entry remains classified as legacy and is never promoted into the warm tier.

- **Suggestion:**
Check the legacy status during duplicate scans: allow a verified fresh reading to replace or upgrade an unverified legacy entry.

- **Regression coverage:**
Populate the ring buffer with legacy entries, ingest a verified note with an identical acquisition epoch, and assert that the entry is promoted for rollup.

---

### R07 - P2: Digital float switch alarms bypass the 300 s minimum interval rate limiter

- **Locations:** [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L6670-L6700), [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L6765-L6780).
- **Classification:** Rate limiting inconsistency.

In `checkAlarmRateLimit()`, dedicated minimum interval checks (`minInterval = 300000UL`) exist for `"high"`, `"low"`, `"sensor-fault"`, and `"sensor-stuck"`.
However, digital float alarms emit `alarmType` as `"triggered"` or `"not_triggered"`. These types do not match any branch in the minimum interval gate, nor do they update `lastHighAlarmMillis`.
While digital alarms are subject to the hourly cap (`MAX_ALARMS_PER_HOUR`), they are completely exempt from the 5-minute spacing cooldown. A fluttering float switch will fire consecutive alarm notes on back-to-back sample cycles, exhausting the hourly quota within seconds.

- **Suggestion:**
Include `"triggered"` and `"not_triggered"` in the minimum interval check, updating and checking `state.lastHighAlarmMillis` (or a dedicated `state.lastDigitalAlarmMillis`).

- **Regression coverage:**
Trigger consecutive digital float alarm transitions at 10-second intervals; verify that notes after the first are held pending by rate limiting until 300 seconds elapse.

---

## Detailed Findings: Follow-on PRs #322-#326

### R08 - P2: Client PR #324 ignores dashboard Clear Relay until unreleased Server S5 lands

- **Locations:**
  - [TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11508)
  - [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L9470-L9520) (`processRelayCommand` on branch fix/clear-relay-by-number-client)
- **Classification:** Protocol backward-compatibility disconnect.

Client PR #324 switches Clear Relay handling to `relay_reset_sensor_number` (sensor identity $1..255$) and explicitly ignores the legacy key:
```cpp
case RELAY_CMD_RESET_LEGACY_IGNORED:
  logRelayClearLine("Clear Relay by list position (relay_reset_sensor) ignored - update the server");
  return;
```
However, the server on master still dispatches `relay_reset_sensor` with the array index in `handleRelayClearPost()`:
```cpp
JAddNumberToObject(body, "relay_reset_sensor", sensorIdx);
```
If client units are updated with PR #324 before server PR S5 is deployed, all operator "Clear Relay" commands dispatched from the web dashboard will be rejected by clients.

- **Suggestion:**
Deploy server PR S5 before or concurrently with client PR #324, or provide a temporary fallback in the client that honors `relay_reset_sensor` when `relay_reset_sensor_number` is absent.

---

### R09 - P2: Global 5 s relay cooldown drops commands across different sensors

- **Locations:** [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L9475-L9485).
- **Classification:** Command loss under multi-tank operations.

In `processRelayCommand()`, the rate limiter enforces:
```cpp
if (lastRelayCommandMillis != 0 && (now - lastRelayCommandMillis) < RELAY_COMMAND_COOLDOWN_MS) {
  Serial.println(F("Relay command rate-limited — too soon after last command"));
  return;
}
lastRelayCommandMillis = now;
```
This cooldown is device-global. If an operator clears alarms on Sensor #1 and then immediately clears Sensor #2, or if two relay control commands arrive in the same inbound Notecard sync batch, the second command is permanently dropped and popped from the relay notefile queue.

- **Suggestion:**
Key the cooldown to the specific relay mask or sensor number rather than enforcing a blanket device-wide freeze.

---

### R10 - P2: Daily reconciliation ignores missed alarms on uncached/new sensors

- **Locations:** [TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L13530-L13570) (on branch `fix/digital-display-server`).
- **Classification:** Alarm reconciliation omission.

In `handleDaily()`, `runAlarmReconcile` executes before the `sensors[]` loop. When inspecting `dailyAlarms` to recover missed alarms, it executes:
```cpp
SensorRecord *rec = nullptr;
for (uint8_t ri = 0; ri < gSensorRecordCount; ++ri) {
  if (strcmp(gSensorRecords[ri].clientUid, clientUid) == 0 && gSensorRecords[ri].sensorIndex == sensorIdx) {
    rec = &gSensorRecords[ri];
    break;
  }
}
if (rec && !rec->alarmActive) { ... }
```
If the server rebooted, or the sensor is reporting for the first time, `rec` is `nullptr`. The missed alarm is bypassed. The subsequent `sensors[]` loop calls `upsertSensorRecord()`, but never checks `dailyAlarms`, leaving the sensor unalarmed on the dashboard.

- **Suggestion:**
Swap the sequence in `handleDaily()` so `sensors[]` are ingested and upserted before running the alarm reconciliation pass, or call `upsertSensorRecord()` directly during alarm reconciliation.

---

### R11 - P2: Stale $\ge 4.0\text{ mA}$ clamp in daily report ingestion zeroes live-zero current

- **Locations:** [TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L13677) (on branch `fix/digital-display-server`).
- **Classification:** Telemetry regression / sensor data corruption.

In v2.0.52 (Fix 13), `handleTelemetry()` and `sendClientDataJson()` removed the legacy $\ge 4.0\text{ mA}$ clamp to allow diagnostic visibility into under-range currents ($3.6 - 3.9\text{ mA}$).
However, `handleDaily()` still retains:
```cpp
if (t["ma"]) {
  mA = t["ma"].as<float>();
  rec->sensorMa = (mA >= 4.0f) ? mA : 0.0f;
}
```
Whenever a daily report is processed, any legitimate live-zero reading below $4.0\text{ mA}$ is wiped to $0.0\text{ mA}$.

- **Suggestion:**
Remove `(mA >= 4.0f) ? mA : 0.0f` and store raw `mA` directly, matching `handleTelemetry()`.

---

### R12 - P2: `handleAlarm` Display Number updates bypass sanitization and dirty tracking

- **Locations:** [TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L13182-L13186) (on branch `fix/display-number-emails`).
- **Classification:** Validation bypass and persistence inconsistency.

PR #326 introduced `noteDisplayNumber()` in the new sensor name header and applied it across `handleTelemetry()`, `handleDaily()`, and `handleUnload()`.
However, `handleAlarm()` retains the legacy unvalidated extraction:
```cpp
if (doc.containsKey("un")) {
  rec->userNumber = doc["un"].as<uint8_t>();
}
```
1. An explicit JSON `null` sets `rec->userNumber = 0`, clearing the Display Number on an alarm note.
2. Malformed numbers ($> 255$) wrap modulo 256.
3. `handleAlarm()` fails to set `gSensorRegistryDirty = true`, so a Display Number learned via an alarm note is not committed to flash.

- **Suggestion:**
Use `noteDisplayNumber(!un.isNull(), un.is<int32_t>() ? un.as<int32_t>() : -1, false, rec->userNumber)` in `handleAlarm()` and flag `gSensorRegistryDirty = true` when changed.

---

### R13 - P2: Daily email summary outputs `(0 mA)` for analog voltage and pulse sensors

- **Locations:**
  - [TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L15340-L15355)
  - The email bridge script template in [TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L2086)
- **Classification:** Formatting defect in operator notifications.

In `sendDailyEmail()`, `obj["sensorMa"] = roundTo(gSensorRecords[i].sensorMa, 2)` is serialized for every sensor record, defaulting to $0.0$.
In the Apps Script email bridge:
```javascript
if (!digital && s.sensorMa !== undefined) l += ' (' + s.sensorMa + ' mA)';
```
Because `sensorMa` is present on all non-digital records, analog 0-10V sensors and pulse sensors (RPM, flow) print `(0 mA)` in daily email reports (e.g. `Silas Generator RPM: 1800 (0 mA)`).

- **Suggestion:**
Either emit `sensorMa` only when `strcmp(rec.sensorType, "currentLoop") == 0`, or update the email bridge script to check `s.sensorMa > 0` before appending current-loop units.

---

### R14 - P2: Snooze/resume SMS overflows 160-char buffer for digital float alarms

- **Locations:** [TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L15025-L15045) (on branch fix/display-number-emails).
- **Classification:** SMS string truncation of operator actions.

In `applyReminderSnooze()`, the snooze message tail includes:
`" reminders paused by <who>. Still in <type> alarm (<state>). Auto-resumes on recovery; reply UNSNOOZE to resume now."`
For a digital float with type `"not_triggered"` (13 bytes) and a 20-character operator username/email, the tail length reaches 138-141 bytes.
When prepended with `"SNOOZED: "` (9 bytes) and the mandatory minimum name allocation `SENSOR_NAME_MIN_ROOM` (16 bytes), the total required buffer is 163-166 bytes, which exceeds `char message[160]`.
`composeSensorText()` truncates the tail at 159 bytes, chopping off the trailing command instructions: `"... reply UNSNOOZE to resume n"`. Additionally, email alerts inherit the same 160-byte SMS-constrained buffer.

- **Suggestion:**
Shorten the SMS instruction tail (e.g. `" Auto-resumes or reply UNSNOOZE."`) and decouple email message generation from the 160-byte SMS buffer.

---

### R15 - P3: Generator numbers host test fails on Windows due to hardcoded Unix LF endings

- **Locations:** The generator numbers test script on branch fix/stable-sensor-numbers (line 247).
- **Classification:** Developer tooling / test suite cross-platform defect.

In the generator numbers test script, the test extracts `upsertSensorRecord()` using:
```javascript
const end = at < 0 ? -1 : sketch.indexOf('\n}\n', at);
```
On Windows workstations where `core.autocrlf = true`, files are checked out with CRLF (`\r\n`). `sketch.indexOf('\n}\n', at)` returns `-1`, causing three test assertions to fail immediately (`FAIL upsertSensorRecord found`, `FAIL registry refuses sensor 0`, `FAIL registry still caps the record count`).

- **Suggestion:**
Normalize line endings upon loading:
```javascript
const sketch = fs.readFileSync(SKETCH, 'utf8').replace(/\r\n/g, '\n');
```

---

## Verification Summary

| Suite / Verification Area | Result | Scope / Notes |
| --- | --- | --- |
| [tests/host/email_bridge/email_bridge_test.js](tests/host/email_bridge/email_bridge_test.js) | **137 checks, 0 failures** | Verified on Node.js v24 against master baseline |
| Generator numbers host test | **3 failures on Windows (CRLF)** | Reproduced finding R15; verified clean pass when normalized to LF |
| Viewer cards host test | **All checks passed** | Verified on Node.js against branch fix/digital-display-server |
| Web Page Syntax Validation | **18 HTML PROGMEM blocks checked** | No unbalanced script tags or unterminated template strings |
| Git Branch Merge Simulation | Clean line-merge | `git merge-tree` verified no syntactic conflicts between PR #323 and PR #326 |

---

## Actionable Recommendations & Implementation Plan

1. **Phase 1 (Critical Hotfixes for v2.2.16):**
   - Address R01 by verifying LittleFS rename behavior on physical Opta hardware and adding staged rename fallback in [TankAlarm-112025-Server-BluesOpta/WarmTierStore.h](TankAlarm-112025-Server-BluesOpta/WarmTierStore.h#L735).
   - Address R02 by adjusting the on-demand update fulfillment check on the dashboard.
   - Address R07 by adding `"triggered"` and `"not_triggered"` to `checkAlarmRateLimit()` in [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L6670).

2. **Phase 2 (Pre-Merge Fixes for PRs #322-#326):**
   - In PR #324, scope relay command cooldown to individual relays/monitors (R09) and synchronize deployment with Server S5 (R08).
   - In PR #325, re-order `handleDaily()` so `sensors[]` are ingested prior to running `runAlarmReconcile` (R10), and drop the legacy $\ge 4.0\text{ mA}$ gate (R11).
   - In PR #326, route `handleAlarm()` Display Number updates through `noteDisplayNumber()` with `gSensorRegistryDirty` tracking (R12), restrict `sensorMa` serialization to current-loop sensors (R13), and condense the snooze SMS tail (R14).
   - In PR #323, normalize line endings in the generator numbers test script (R15).
