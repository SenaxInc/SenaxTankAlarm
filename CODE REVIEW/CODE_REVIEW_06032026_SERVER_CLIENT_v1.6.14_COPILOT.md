# TankAlarm Code Review — Server & Client (11/2025 Generation)

- **Date:** June 3, 2026
- **Firmware Version Reviewed:** v1.6.14 (`FIRMWARE_VERSION` in [TankAlarm-112025-Common/src/TankAlarm_Common.h](TankAlarm-112025-Common/src/TankAlarm_Common.h#L19))
- **Reviewer:** GitHub Copilot (synthesizing four parallel focused review passes)
- **Scope:**
  - [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino) (~8,845 LOC)
  - [TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino) (~17,773 LOC)
  - [TankAlarm-112025-Common/src/](TankAlarm-112025-Common/src) shared library
- **Focus areas (per request):**
  1. Wireless (device-to-device) messaging between Client and Server
  2. Sensor data collection and processing on the Client
  3. Server-side reception, processing, and cloud relay of that data
  4. Cross-cutting concerns in the shared Common library

---

## Executive Summary

The 11/2025 firmware family is **well-architected and largely production-ready**. The Common library follows good header-only patterns, the Notecard route-based relay is a clean replacement for raw radio, and the sensor pipeline has solid validation/debouncing primitives. The most consequential issues fall in three clusters:

1. **Time/uptime arithmetic** — `currentEpoch()` on the Server does not explicitly guard against `millis()` rollover at ~49 days, which can corrupt every time-based decision (rate limiters, history timestamps, schedules).
2. **Trust boundary on inbound notes** — the Server treats the JSON-claimed `clientUid` (`"c"` field) as authoritative without any signature or cross-check against the Notecard route metadata. Combined with global (not per-client) SMS rate limiting on system alarms, this creates spoofing and alert-suppression risks.
3. **Silent-failure paths in sensor ingestion** — integer-before-cast in 4–20 mA scaling, `Wire.read()` `-1` interpreted as `0xFF`, no `isfinite()` check on derived levels, and unenforced `analogReadResolution(12)` can each produce confident-but-wrong readings that flow all the way to SMS alerts.

None of the findings are blocking for current field deployments, but the items marked **Critical / High** should be scheduled into the next maintenance release.

### Findings by severity

| Severity     | Count | Areas |
|--------------|-------|-------|
| Critical     | 2     | Server time-base rollover; missing NULL-guard pattern on JSON parsing |
| High         | 3     | UID spoofing trust boundary; per-client SMS rate-limit scope; SMS site-name truncation |
| Medium       | 14    | Sensor numeric edge cases, Modbus timeouts, eviction loss, payload size validation, etc. |
| Low          | 6     | Memory-safety nits, style/consistency, documentation gaps |
| Improvements | 25+   | See sections below |

---

## 1. Wireless Messaging (Client ↔ Server)

### 1.1 Architecture

The system does **not** use LoRa or direct radio. Wireless transport is **Blues Notecard cellular + Notehub Route Relay**:

- **Client → Server:** Client serializes telemetry / alarm / daily / unload events into JSON and calls `publishNote()` ([Client line 7150](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7150)), which appends to `.qo` outbox files (`telemetry.qo`, `alarm.qo`, `daily.qo`, …). A Notehub **ClientToServerRelay** route mirrors those notes into the Server's matching `.qi` inbox files.
- **Server → Client:** Server publishes to a single `command.qo` outbox, tagging each note with `_target` (client device UID) and `_type` (command kind). A **ServerToClientRelay** route delivers the body to the correct client's `config.qi`, `relay.qi`, `location_request.qi`, etc.
- **Payload encoding:** JSON with deliberately abbreviated keys (`c`=clientUid, `k`=sensorIndex, `s`=site, `l`=level, `ma`=milliamps, `vt`=voltage, `fv`/`fwv`=firmware version, `_sv`=schema version) to keep cellular cost down.
- **Security model:** Inherits TLS + fleet membership from Notehub. There is **no application-layer signing or encryption**, and the Server trusts the `"c"` field that the body itself supplies.

Notefile names are centralized in [TankAlarm-112025-Common/src/TankAlarm_Common.h](TankAlarm-112025-Common/src/TankAlarm_Common.h#L119) so both sides stay in sync.

### 1.2 Bugs

| # | Severity | File:Line | Finding |
|---|----------|-----------|---------|
| W-1 | **Critical** | [Server 11742–11750](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11742) | The `JGetObject(rsp, "body")` pattern is inconsistently NULL-checked across handlers. `processNotefile()` does check it, but several downstream `JGetString` / `JGetNumber` calls inside `handleTelemetry`, `handleAlarm`, `handleDaily` operate on objects whose parent was never re-validated. **Fix:** introduce `SAFE_JGET_*` wrappers and apply them uniformly; reject the note if `body` is missing rather than continuing with defaults. |
| W-2 | **High** | [Server `handleTelemetry` 11806–11860](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11806) | No source authentication: the server upserts records keyed on the body-supplied `"c"` (clientUid) with no cross-check against Notehub's routing metadata. Any party with project credentials (or a forged note) can poison records for another device, raise phantom alarms, or suppress real ones. **Fix:** add HMAC-SHA256 of the canonical body using a per-project shared secret, or pull and compare the routed `device` header from the Notecard envelope before trusting `"c"`. |
| W-3 | **High** | [Server 12011–12028](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12011) | `static double sLastSystemSmsSentEpoch` is a **single global** rate limiter for system alarms (solar/battery/power) across all clients. With ≥2 clients alarming inside `MIN_SMS_ALERT_INTERVAL_SECONDS`, only one SMS goes out and the rest are silently dropped. **Fix:** move the timestamp into `ClientMetadata` and index per `(clientUid, alarmType)`. |
| W-4 | **High** | [Server 12021–12028](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12021) | System-alarm SMS uses a fixed 160-byte buffer with `snprintf("%s Solar: %s ...", siteName, …)`. A long `siteName` silently truncates without checking the return value, losing the actual alarm text. **Fix:** check `snprintf` return; if `>= sizeof(buf)`, fall back to a compact pre-truncated site label. |
| W-5 | **Medium** | [Client `processRelayCommand` 7818–7848](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7818) | Target validation uses `strcmp(targetUid, gDeviceUID)`. Before the Notecard hub-sync completes, `gDeviceUID` falls back to the configured label (e.g. `"Tank01"`). A note crafted with that label gets accepted. **Fix:** require `strncmp(gDeviceUID, "dev:", 4) == 0` before processing any relay command. |
| W-6 | **Medium** | [Server 11765–11795](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11765) | Poison-note deletion only triggers after 3 consecutive parse failures *for the entire notefile*, not per note. A single corrupt note can stall an inbox indefinitely while burning I2C cycles every poll. **Fix:** raise the count and add exponential backoff, or attach the failure counter to the note ID. |
| W-7 | **Medium** | [Client 7837](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7837) | `RELAY_COMMAND_COOLDOWN_MS` is referenced without a header-guard `#error` ensuring its definition is in scope. If a future refactor drops the include, the cooldown silently becomes 0. **Fix:** add `#if !defined(RELAY_COMMAND_COOLDOWN_MS) #error … #endif` at file top. |

### 1.3 Improvements

1. **Add a per-message sequence number** (`_seq`, persisted in client config) and a server-side `(clientUid, _seq)` dedup set. Notehub retries can currently double-fire SMS, daily-report rows, or relay commands.
2. **Add an "age" guard** in `processNotefile()`: reject notes whose `time` field is more than ~24 h behind `currentEpoch()`, so a backlogged queue can't trigger stale alarms.
3. **Enforce `isValidClientUid()` at the top of every handler** (`handleTelemetry`, `handleAlarm`, `handleDaily`, `handleUnload`, command-forwarding paths). Today it is only called in a few spots ([Server 1340](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L1340)).
4. **Cap inbound payload size** before `deserializeJson()` (e.g. `MAX_NOTE_PAYLOAD_BYTES = 32 KB`) to bound parser cost from a malicious or runaway client.
5. **Optionally AES-encrypt config bodies** delivered through `sendConfigViaNotecard` ([Server 3056](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L3056)) using a key derived from productUid + deviceUid, for defense in depth.
6. **Warn on `strlcpy` truncation** of important string fields (site, label, phone numbers) instead of silently losing characters.
7. **Document the `.qo` ↔ `.qi` mapping table** at the top of [TankAlarm_Common.h](TankAlarm-112025-Common/src/TankAlarm_Common.h#L119) so the Notehub route names are auditable from source.

### 1.4 Key symbols

| Symbol | File:Line | Role |
|--------|-----------|------|
| `publishNote()` | [Client 7150](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7150) | Core outbound send |
| `bufferNoteForRetry()` | [Client ~7260](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7260) | Persists notes on transient failures |
| `processNotefile()` | [Server 11708](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11708) | Drains a `.qi` inbox |
| `handleTelemetry / Alarm / Daily / Unload` | [Server 11806 / 11983 / 12256 / 12568](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11806) | Per-notefile handlers |
| `processRelayCommand()` | [Client 7818](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7818) | Inbound relay control |
| `sendConfigViaNotecard()` | [Server 3056](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L3056) | Outbound config push |
| `isValidClientUid()` | [Server 1340](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L1340) | UID format check |

---

## 2. Client Sensor Data Collection & Processing

### 2.1 Pipeline

`sampleMonitors()` ([Client 4945](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4945)) iterates monitor configs and dispatches via `readMonitorSensor()` ([Client 4926](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4926)) to:

- `readDigitalSensor()` — NO/NC float switch via `INPUT_PULLUP` ([Client 4713](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4713))
- `readAnalogSensor()` — 8-sample average of 0–10 V into 12-bit ADC ([Client 4743](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4743))
- `readCurrentLoopSensor()` — A0602-gated 4-sample 4–20 mA loop ([Client 4811](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4811))
- Ancillary: battery via Notecard `card.voltage`, solar via Modbus over RS-485 ([TankAlarm_Solar.cpp](TankAlarm-112025-Common/src/TankAlarm_Solar.cpp)), Vin divider ADC.

Raw → unit conversion (incl. fluid specific gravity) → `validateSensorReading()` ([Client 4571](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4571)) → `evaluateAlarms()` (debounced) → rate-limited telemetry upload.

### 2.2 Bugs

| # | Severity | File:Line | Finding |
|---|----------|-----------|---------|
| C-1 | **Medium** | [TankAlarm_I2C.h L359](TankAlarm-112025-Common/src/TankAlarm_I2C.h#L359) | `raw / 65535.0f` performs the division at integer width before promotion in some compiler paths, losing precision near 4 mA. **Fix:** `(float)raw / 65535.0f`. |
| C-2 | **Medium** | [TankAlarm_I2C.h L225](TankAlarm-112025-Common/src/TankAlarm_I2C.h#L225) | `uint16_t raw = ((uint16_t)Wire.read() << 8) \| Wire.read();` — `Wire.read()` returns `int` (`-1` on error). A `-1` cast to `uint16_t` becomes `0xFFFF`, producing a confident full-scale 20 mA reading. **Fix:** read into two `int` locals, check `>= 0` before combining. |
| C-3 | **Medium** | [TankAlarm_I2C.h L325–340](TankAlarm-112025-Common/src/TankAlarm_I2C.h#L325) | After the `Wire.available() < 2` guard, a transient may leave exactly 1 byte; the subsequent two `Wire.read()` calls then return one valid byte + `-1`. Same effect as C-2 with a different trigger. **Fix:** re-check `Wire.available() == 2` immediately before combining. |
| C-4 | **Medium** | [Client 1372–1376](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L1372) | The comment promises 12-bit ADC (`/4095.0f`), but no runtime `analogReadResolution(12)` call enforces it. Mbed defaults vary across cores; a default of 10-bit silently reads 4× high. **Fix:** call `analogReadResolution(12)` in `setup()` and log the actual resolution. |
| C-5 | **Medium** | [Client `linearMap` 4562–4569](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4562) | `if (fabs(inMax - inMin) < 0.0001f) return outMin;` masks a malformed config with a silent "always min" result. **Fix:** return `NAN` and let `validateSensorReading()` flag a fault. |
| C-6 | **Medium** | Throughout sensor pipeline; check should live in [validateSensorReading 4571](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4571) | No `isfinite()` gate on derived inches/percent. A NaN (e.g. SG=0, mismapped config) compares false in every alarm threshold check yet still serializes into telemetry as `NaN` → invalid JSON / server parse failure. **Fix:** reject `!isfinite(value)` early. |
| C-7 | **Medium** | [TankAlarm_Solar.cpp 96–103](TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L96) | `modbusTimeoutMs` clamped to minimum 500 ms while SunSaver-over-MRC-1 commonly responds in 300–600 ms; intermittent timeouts cause false LOW_BATTERY alerts driven by stale data. **Fix:** allow 250 ms minimum, document empirical timings. |

### 2.3 Improvements

1. **Median-of-5 filter** in `readCurrentLoopSensor` and `readAnalogSensor` instead of mean, to discard a single bad I2C/ADC sample without skewing the result.
2. **Cache `effectiveSpecificGravity`** on `MonitorRuntime` instead of recomputing it inside every sample call ([getEffectiveSpecificGravity Client 2065](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L2065)).
3. **Software debounce digital float switches** (3–5 reads ~1 ms apart) — currently a single GPIO read can latch a spurious edge.
4. **Bound cumulative sampling time** — 8 sensors × 8 samples × 2 ms ≈ 128 ms per cycle blocks the loop. Kick the watchdog and yield between sensors.
5. **Track "age of last valid reading"** per monitor. Today a failed validation reuses `currentInches` indefinitely, so a permanently-failed sensor can report a value that is hours old without ever escalating to a sensor-fault alert.
6. **Log a warning + counter** when `currentLoopChannel` out-of-range silently falls back to channel 0 ([Client 4805–4810](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4805)).
7. **Add `static_assert` ordering** for battery thresholds (`critical < low < normal < high`) in [TankAlarm_Battery.h](TankAlarm-112025-Common/src/TankAlarm_Battery.h#L600) to catch copy-paste swaps at compile time.
8. **Optional temperature compensation** for pressure sensors (~0.1–0.3 %/°C drift) using SunSaver or Notecard temperature.
9. **Document A0602 endianness** at the read site (it is big-endian; verified per datasheet Fig. 3) to keep future maintainers from "fixing" the byte order.

---

## 3. Server Reception, Processing & Cloud Relay

### 3.1 Architecture

- **Reception:** `processNotefile()` drains each `.qi` inbox bounded to `MAX_NOTES_PER_FILE_PER_POLL = 10` ([Server 346](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L346)).
- **Parsing/state:** Handlers upsert into `gSensorRecords[]` (MAX 64) and `gClientMetadata[]` (MAX 20), with LRU eviction keyed on `vinVoltageEpoch`.
- **History:** `recordTelemetrySnapshot()` writes ring buffers; daily summaries persist to LittleFS; optional FTP archival.
- **Alerts:** SMS via `sms.qo`, Email via `email.qo`; Viewer summary published periodically.
- **Time base:** Notecard epoch synced every 6 h; intra-window time computed by `currentEpoch()` from `millis()` delta.
- **Web API:** Ethernet REST endpoints behind a 4-digit PIN session.

### 3.2 Bugs

| # | Severity | File:Line | Finding |
|---|----------|-----------|---------|
| S-1 | **Critical** | [Server `currentEpoch` 8973–8979](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L8973) | `(millis() - gLastSyncMillis)` is computed without explicit rollover handling. At ~49 days uptime, `millis()` wraps and the delta becomes ~4 B ms (~136 years). Every consumer (`checkSmsRateLimit`, history timestamps, schedule comparisons) then sees an epoch jump. **Fix:** force `uint32_t` subtraction (which is well-defined modulo 2³²) and immediately re-sync from Notecard when the delta exceeds e.g. 24 h; add a unit test. |
| S-2 | **High** | Static `sLastSystemSmsSentEpoch` ([Server 12011](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12011)) | Same finding as W-3 — global scope across all clients & all system-alarm types. |
| S-3 | **Medium** | [Server `findOrCreateClientMetadata` 12207–12230](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12207) | LRU eviction can drop a `ClientMetadata` entry that still holds an active `lastSystemAlarmType` / `lastSystemAlarmEpoch`. After eviction the alarm state is gone, and the next message from that device starts fresh. **Fix:** include "has active alarm" as a pinning factor in the LRU score, or persist the alarm state to LittleFS. |
| S-4 | **Medium** | [Server `checkSmsRateLimit` 12173](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12173) | `bypassMinimumInterval=true` for "clear" events lets a flapping sensor (bouncing at the hysteresis boundary) generate unbounded SMS. **Fix:** still apply a short floor (e.g. 60 s) on clears, or require N consistent clear samples first. |
| S-5 | **Medium** | [Server `sendDailyEmail` 12960–12980](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12960) | Overflow is checked *after* `serializeJson`; oversized days are silently dropped with no retry queue. **Fix:** pre-estimate per-sensor row size and split into multiple emails before serialization, or persist the failed payload for the next cycle. |
| S-6 | **Medium** | [Server 11806–11980](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11806) | No payload size cap before `deserializeJson`. Same finding as W-4. |
| S-7 | **Low-Med** | [Server 1055](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L1055) | `gBackupInProgress` is declared `volatile` but read/written non-atomically. Non-fatal but can race in status reporting. |
| S-8 | **Medium** | [Server PIN-protected POST endpoints 1651–1663](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L1651) | `/api/pin`, `/api/server-settings`, `/api/relay` accept POSTs with only the `X-Session` header — no anti-CSRF nonce. **Fix:** issue a one-shot nonce on session creation and require it on every state-changing POST. |
| S-9 | **Low** | [Server 1034 / 1037](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L1034) | Repeated `strncat` building `failedFiles` is heap-thrashing on long retry loops. **Fix:** pre-size into a fixed `char[]` with `snprintf` accumulation. |

### 3.3 Improvements

1. **Add a `safeSleep(1)` / watchdog kick between `processNotefile()` calls** so a backlog can't block the web server and starve the watchdog.
2. **Reject empty `clientUid`** outright in `findOrCreateClientMetadata` ([Server 12207](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12207)). Today empty strings can create phantom slots.
3. **Prioritized notefile draining** — give `alarm.qi`/`daily.qi` higher weight than `telemetry.qi` so bulk telemetry can't starve a high-priority alarm.
4. **Per-alarm-type and per-client SMS rate limits.** Same root cause as S-2 / W-3.
5. **Periodic epoch reconciliation.** Compare `currentEpoch()` to a fresh `card.time` every 24 h (not just 6 h) and log/correct drift.
6. **Persist `ClientMetadata` snapshots to LittleFS** so an eviction or reboot doesn't lose alarm/version/signal context.

---

## 4. Common Library & Cross-Cutting Concerns

### 4.1 Architecture

Header-only static-inline pattern — good for ODR safety and aggressive inlining; works because each .ino is a single translation unit.

### 4.2 Bugs

| # | Severity | File:Line | Finding |
|---|----------|-----------|---------|
| K-1 | Medium | [TankAlarm_I2C.h 225](TankAlarm-112025-Common/src/TankAlarm_I2C.h#L225) | Same `Wire.read() == -1 → 0xFF` issue as C-2 (the canonical site). Fix once here. |
| K-2 | Medium | [TankAlarm_I2C.h 39](TankAlarm-112025-Common/src/TankAlarm_I2C.h#L39) | `extern uint32_t gCurrentLoopI2cErrors;` etc. must be defined in every sketch including this header. A future sketch that forgets it will link with a weak/zero symbol on some toolchains. **Fix:** provide weak definitions in the header, or move them into a `.cpp` in the library. |
| K-3 | Medium | [TankAlarm_Battery.h ~600](TankAlarm-112025-Common/src/TankAlarm_Battery.h#L600) | No ordering enforcement on `critical < low < normal < high`. **Fix:** add `static_assert` on the chemistry constants, plus a runtime check in `initBatteryConfig()`. |
| K-4 | Medium | [TankAlarm_DFU.h 406, 419](TankAlarm-112025-Common/src/TankAlarm_DFU.h#L406) | Multiple `free(progBuf)` exit paths interact awkwardly with `goto iap_restore_hub`; while not currently a double-free, restructuring to a single exit point with `if (progBuf) free(progBuf);` is much easier to audit. |
| K-5 | Low | [TankAlarm_DFU.h 327](TankAlarm-112025-Common/src/TankAlarm_DFU.h#L327) | Redundant `free(NULL)` after malloc failure. Harmless but suggests confusion. |
| K-6 | Low | [TankAlarm_Utils.h 57](TankAlarm-112025-Common/src/TankAlarm_Utils.h#L57) | Fallback `strlcpy` guard is `#if !defined(ARDUINO_ARCH_MBED) && !defined(strlcpy)` — replace with just `#if !defined(strlcpy)` to also cover non-Opta STM32 targets. |

### 4.3 Improvements

1. **`static_assert`s in [TankAlarm_Config.h](TankAlarm-112025-Common/src/TankAlarm_Config.h)** for watchdog timeout bounds, recovery threshold positivity, and enum ordering.
2. **`#error` guards** in [TankAlarm_DFU.h](TankAlarm-112025-Common/src/TankAlarm_DFU.h) requiring `DFU_IAP_CHUNK_SIZE` and `DFU_IAP_MODE_TIMEOUT_MS` to be defined before include.
3. **Mark diagnostic counters `volatile`** where they cross ISR ↔ main-loop boundaries; document atomicity assumptions for 32-bit ARM.
4. **Watchdog kick inside the 16-iteration SCL toggle loop** of `tankalarm_recoverI2CBus` to prevent starvation under bus contention.
5. **Standardize integer-literal suffixes** (`UL`) on time/size constants in `TankAlarm_Config.h` so arithmetic never silently promotes to `int`.
6. **Document the DFU `dfuInProgress` guard parameter** at the top of [TankAlarm_I2C.h](TankAlarm-112025-Common/src/TankAlarm_I2C.h#L127) — its semantics aren't obvious from the call sites.

---

## 5. Recommended Roadmap

Priority order for the next maintenance release:

1. **Fix `currentEpoch()` rollover** (S-1) and add a periodic Notecard time re-sync sanity check.
2. **Make system-alarm SMS rate limiting per-client and per-alarm-type** (W-3 / S-2).
3. **Harden inbound JSON parsing** with `SAFE_JGET_*` wrappers and uniform `isValidClientUid()` checks (W-1, W-2, S-6).
4. **Repair the I2C / 4-20 mA numeric path** (C-1, C-2, C-3, C-6) — these are subtle and produce confident-but-wrong sensor readings.
5. **Enforce `analogReadResolution(12)` at boot** and log the actual resolution (C-4).
6. **Gate relay commands on a confirmed `dev:` UID** (W-5).
7. **Add CSRF nonces to PIN-protected POST endpoints** (S-8).
8. **Compile-time ordering checks for battery thresholds + watchdog timeouts** (K-3, improvements 4.3.1).
9. **Per-message `_seq` dedup** to eliminate duplicate SMS / relay re-execution from Notehub retries.
10. **Persist `ClientMetadata` and active-alarm pinning** so LRU eviction never silently loses alarm state.

---

## 6. Overall Assessment

- **Code quality:** Good and consistent. Naming, file structure, abbreviated wire format, and the Common-library factoring all show deliberate engineering.
- **Reliability:** Most failure paths are handled, but several edge cases (timer wrap, NaN, `-1` from `Wire.read()`) bypass them in a way that produces *confident wrong answers* rather than detectable errors — the most dangerous failure mode for an alarm system. These should be the top priority.
- **Security:** Adequate for a closed Notehub fleet today, but the lack of body-level authentication means anyone with project credentials can spoof alarms or telemetry. Application-layer HMAC is a worthwhile defense-in-depth investment.
- **Maintainability:** The shared Common library and dated review trail in `CODE REVIEW/` make this an unusually easy codebase to onboard onto.

No findings warrant rolling back the v1.6.14 release; all are appropriate for a planned v1.6.15 / v1.7.0 maintenance window.
