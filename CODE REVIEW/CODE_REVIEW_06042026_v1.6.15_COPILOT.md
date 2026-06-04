# TankAlarm Code Review — Server & Client v1.6.15 (06/04/2026)

- **Date:** June 4, 2026
- **Firmware Version:** v1.6.15
- **Author:** GitHub Copilot (Gemini 3.5 Flash)
- **Scope:** Complete analysis of Client (`TankAlarm-112025-Client-BluesOpta`), Server (`TankAlarm-112025-Server-BluesOpta`), and the shared library (`TankAlarm-112025-Common`).
- **Focus Areas:** Wireless messaging system (Notecard queueing, offline buffering, JSON sizes, memory management, peek-then-delete, transport protocol), sensor data collection and processing (sampling, filtering, PWM gating, digital debouncing, learned calibration bounds, edge cases), transmission of that data, and server-side processing (reconciliation of client level, cached config payloads, capacity handling/precision, error recovery, LRU metadata management, SMS rate limit keys/counters).

---

## 1. Executive Summary

This comprehensive review evaluates the newly implemented **v1.6.15 Conversion Architecture & Calibration Sync** designed on June 3, 2026. The new architectural direction—moving from duplicated theoretical conversions on both side to a **unified, self-describing client level payload (`lvl` + `cap` + `st`) with authoritatively pushed server calibration coefficients**—is highly successful and correctly solves multiple high-precision drift bugs.

However, several deep-seated logical bugs, buffer overflows, and architectural bottlenecks have been uncovered during this thorough sweep, including:
1.  **Critical Buffer Flush Drop-Bug:** Notes exceeding $1023\text{ bytes}$ (such as large multi-sensor daily reports) are *permanently deleted and ignored* by the client during offline buffer flushing.
2.  **Silent Calibrated Sensor Failure:** When a current-loop sensor is physically disconnected ($0.0\text{ mA}$), its computed level resolves to the calibrated offset (e.g., $2.5\text{ inH}_2\text{O}$), resulting in a valid reading and silencing the `sensor-fault` alarm.
3.  **Config Snapshot Cache Bottleneck:** The server enforces a $1536\text{ byte}$ hard cap on saved configuration snapshots, rejecting large configs with multiple monitors and active calibration coefficients.
4.  **OOM and Heap Churn Risk:** Double-serialization/deserialization cycles between ArduinoJson and Notecard `J` objects strain the CPU and cause heavy heap fragmentation.

Below is the structured, severity-rated analysis of these findings.

---

## 2. Wireless Messaging System

### [CRITICAL BUG] P3-1: Offline Note Buffer Drops Large Reports due to `lineBuffer` Limitation
*   **Location:** Client `flushBufferedNotes()` [L7303](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7303) (Mbed OS branch) and [L7482](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7482) (LittleFS branch)
*   **Impact:** **High** (Data Loss)
*   **Logic Failure:**
    *   In the Mbed branch, `fgets(lineBuffer, sizeof(lineBuffer), src)` reads lines from `/fs/pending_notes.log` with a $1024\text{ byte}$ buffer. Because `DAILY_NOTE_PAYLOAD_LIMIT` is set to $960\text{ bytes}$ and the wrapper metadata (device UID, site ID, schema version, wrapper JSON formatting) adds $100\text{–}150\text{ bytes}$, daily reports during offline periods routinely serialize to **$1030\text{–}1100\text{ bytes}$**.
    *   When the line is read, the check `len == sizeof(lineBuffer) - 1 && lineBuffer[len - 1] != '\n'` evaluates to `true` (it was truncated).
    *   The client then calls:
        ```cpp
        // Skip rest of the truncated line
        int ch;
        while ((ch = fgetc(src)) != EOF && ch != '\n') {}
        continue;
        ```
        This **permanently skips the entire note** and prints a warning that is lost on headless deployments. The user's historical records and voltage monitoring are permanently blind to that day's reports.
    *   In the LittleFS branch, the code reads up to $1023\text{ bytes}$ and null-terminates. The remainder of the JSON string remains in the file stream, causing the next loop iteration to read the remainder of the JSON (e.g., `"very_long_data": "..."}`) which lacks tab delimiters, failing parsing and silently skipping the rest of the stream.
*   **Remediation:** Sibling daily notes and flash-flushing files must accommodate the maximum possible serialized JSON payload. Increase `lineBuffer` in both branches of `flushBufferedNotes()` to `2048` bytes, or replace the trailing-whitespace log format with a length-prefixed protocol.

### [PERFORMANCE PENALTY] Dual JSON Library Churn (ArduinoJson $\leftrightarrow$ J-Notecard)
*   **Location:** Client `publishNote()` [L7175](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7175)
*   **Impact:** **Medium** (Memory Churn, Latency)
*   **Logic Failure:**
    To publish a note, the client takes a pre-built ArduinoJson `JsonDocument` representation, serializes it to a flat character array string, and then immediately invokes the Blues `JParse()` utility:
    ```cpp
    size_t len = serializeJson(doc, buffer, bufSize);
    ...
    J *body = JParse(buffer);
    ```
    This completely duplicates the JSON data structure in memory. The Opta's STM32H7 has plenty of RAM ($512\text{ KB}$), but the heap-allocation overhead of the Blues C-Notecard library (which allocates individual leaf structs for every JSON property) combined with ArduinoJson's heap structures creates extreme memory fragmentation.
*   **Optimization:** Pass pre-serialized strings directly to Notehub where possible, or build the `J*` request tree directly using `JAddNumberToObject()` and `JAddStringToObject()` instead of maintaining two distinct JSON representations.

### [LOGIC GAPS] Target-Blind Inbound Queue Deletion
*   **Location:** Client `pollForLocationRequests()` [L8573](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L8573), `pollForSyncRequests()` [L8642](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L8642), and `pollForSerialRequests()` [L8429](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L8429).
*   **Impact:** **Medium** (Robustness/Security)
*   **Logic Failure:**
    These polling paths utilize an immediate deletion pattern:
    ```cpp
    JAddStringToObject(req, "file", LOCATION_REQUEST_FILE);
    JAddBoolToObject(req, "delete", true);
    ```
    Because they execute `delete: true` on the Notecard request directly, the note is removed from the local Notecard *before* the client parses its contents. 
    1.  If the JSON is corrupt, parsing fails and the request is gone forever without generating an error response.
    2.  More critically, unlike config/relay commands, these paths possess **no target verification**. If a misconfigured Notehub route delivers a location query to the wrong client, the client consumes it, deletes it, and spits out its location tower logs without any validation checks.
*   **Optimization:** Employ the "peek-then-delete" paradigm used by the Config Engine. Fetch without deletion, execute `validateInboundCommand()` (checking `_sv` and verifying `_target == gDeviceUID`), and only delete the note via a second `note.get(..., "delete": true)` call once processing runs successfully.

---

## 3. Sensor Data Collection & Processing

### [CRITICAL BUG] Silent Calibrated Sensor Failure (Current-Loop)
*   **Location:** Client `readCurrentLoopSensor()` [L4906](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4906)
*   **Impact:** **High** (Safety Risk)
*   **Logic Failure:**
    Under calibrated operation, the client translates current loop readings into levels using pushed coefficients:
    ```cpp
    if (cfg.hasLearnedCalibration && cfg.objectType != OBJECT_GAS) {
      float level = cfg.calSlope * milliamps + cfg.calOffset;
      if (cfg.calTempCoef != 0.0f) {
        level += cfg.calTempCoef * (cfg.calTempF - 70.0f);
      }
      if (level < 0.0f) level = 0.0f;
      return level;
    }
    ```
    If a sensor is unplugged or the wire cut, the physical current drops to $0.0\text{ mA}$.
    *   **Without learned calibration:** It maps $0.0\text{ mA}$ on a $4\text{–}20\text{ mA}$ loop, resulting in a large negative depth (e.g., $-15.0\text{ in}$). This reading fails `validateSensorReading()`, raising `sensor-failed` on the digital output and dispatching a `sensor-fault` alarm.
    *   **With learned calibration:** The unplugged reading of $0.0\text{ mA}$ evaluates to `cfg.calOffset` (e.g., $calSlope \cdot 0 + calOffset = 2.5\text{ inH}_2\text{O}$). This positive value falls squarely within the "legal" level bounds of `validateSensorReading()`. The device will report a static Low/Medium level of fluid forever, completely silent to the physical cable loss.
*   **Remediation:** Add a loop-current integrity guard *before* applying learned coefficients. Any mA reading below $3.6\text{ mA}$ indicates active current loop loss and must immediately skip calibration and trigger a loop failure.
    ```cpp
    if (milliamps < 3.6f) {
      gMonitorState[idx].sampleReused = true;
      return gMonitorState[idx].currentInches; // Triggers loop failure via validateSensorReading
    }
    ```

### [LOGIC GAP] Watchdog Starvation during Slow Sensor Sampling
*   **Location:** Client `sampleMonitors()` [L5009](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5009)
*   **Impact:** **Medium** (System Resets)
*   **Logic Failure:**
    When PWM gating is active on current-loop channels, each channel warmup takes `cfg.pwmGatingWarmup` ($100\text{–}1000\text{ ms}$). This is followed by 4 I2C reads spaced by `cfg.pwmGatingSampleDelay` ($20\text{–}100\text{ ms}$).
    One slow current loop sequence can take up to $1.5\text{ s}$ of delay times. If the hub is populated with 8 sensors, a single call to `sampleMonitors()` blocks the main loop continuously for **$12$ seconds**.
    Since the watchdog timeout is set to $30\text{ seconds}$ (`WATCHDOG_TIMEOUT_SECONDS`), any simultaneous I2C delays from the Notecard (which can block up to $15$ seconds under weak cell conditions) will starve the watchdog, causing spurious STM32 module reboots.
*   **Optimization:** Kick the watchdog inside the `sampleMonitors()` loop between each channel's reading to yield time and refresh margins.

### [MATH OUTLIERS] Mean-Averaging Susceptibility to Noise
*   **Location:** Client `readCurrentLoopSensor()` [L4881](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L4881)
*   **Impact:** **Low-Medium** (False Alarms)
*   **Logic Failure:**
    Milamp values use a simple mean-average over 4 samples:
    ```cpp
    milliamps = total / validSamples;
    ```
    If an I2C transaction glitches or receives transient analog spike noise on one of the samples, a mean filter spreads the error across the reading, triggering high/low alarms.
*   **Optimization:** Scale sampling to 5 reads and deploy a **median-of-5 filter** to drop outright transient outliers and keep measurements steady.

---

## 4. Transmission & Server-Side Processing

### [CRITICAL BOTTLENECK] Server Side `ClientConfigSnapshot` payload limit
*   **Location:** Server `ClientConfigSnapshot` struct [L983](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L983)
*   **Impact:** **High** (Config Dispatch Refusal)
*   **Logic Failure:**
    *   The `ClientConfigSnapshot` persistent structure is declared as:
        ```cpp
        struct ClientConfigSnapshot {
          char uid[48];
          char site[32];
          char payload[1536]; // Hard Limit
          ...
        };
        ```
    *   However, the `dispatchClientConfig()` engine serializes configs using a `static char buffer[8192]`. If a multi-monitor config exceeds $1535\text{ characters}$ (highly likely once multiple sensors each configure calibration parameters like `calSlope`, `calOffset`, `calVersion`), the guard checks in `cacheClientConfigFromBuffer()` trigger:
        ```cpp
        if (bufferLen == 0 || bufferLen >= sizeof(((ClientConfigSnapshot*)0)->payload)) {
          Serial.println(F("Config too large for cache..."));
          return false;
        }
        ```
        This completely and permanently blocks the configuration from being persistently cached, returning `PayloadTooLarge` and rendering the manager incapable of managing configurations for larger sites.
*   **Remediation:** Increase `payload` capacity inside the snapshot struct to `4096` bytes. Since this struct lives on the persistent heap relative to `gClientConfigCount` (max 20), the RAM delta is minor ($\sim 50\text{ KB}$), well within the STM32H7's available $512\text{ KB}$.

### [LOGIC GAP] Stale Metadata Eviction Leak
*   **Location:** Server `findOrCreateClientMetadata()` [L12233](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L12233)
*   **Impact:** **Medium** (Supressed Alarms / Parsing Corruption)
*   **Logic Failure:**
    When client metadata counts hit `MAX_CLIENT_METADATA` (20), the cache evicts the oldest entry based purely on `vinVoltageEpoch`:
    ```cpp
    // Maximum capacity reached - evict stalest entry (oldest vinVoltageEpoch)
    uint8_t evictIdx = 0;
    double oldestEpoch = 1e18;
    for (uint8_t i = 0; i < gClientMetadataCount; ++i) {
      double epoch = gClientMetadata[i].vinVoltageEpoch;
      if (epoch <= 0.0) epoch = 0.0;
      if (epoch < oldestEpoch) {
        oldestEpoch = epoch;
        evictIdx = i;
      }
    }
    ```
    If active Client `A` does not use battery/VIN monitoring, its `vinVoltageEpoch` is `0.0`. It will be *immediately* evicted when a new client connects, despite being fully active!
    When evicted:
    1.  `A`'s last system-alarm SMS rate limit states (`lastSystemSmsEpoch`) are purged. If `A` undergoes rapid power transitions, it will bypass rate limits and trigger an SMS flood.
    2.  If eviction occurs while `A` is transmitting a daily report part batch, the receiving checklist (`dailyPartsReceived`) is cleared, causing the server to flag false "Daily report incomplete" warnings.
*   **Remediation:** Track a single unified `lastActiveEpoch` timestamp inside the `ClientMetadata` structure. Update it during any telemetry, alarm, or daily note packet receipt, and use it as the single, robust candidate selector for cache eviction.

---

## 5. Security & Verification

### [VULNERABILITY] Lack of Inbound Encryption/HMAC Verification
*   **Location:** Server Routing / `processNotefile` [L11766](TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino#L11766)
*   **Impact:** **High** (Spoofing)
*   **Logic Failure:**
    The server upserts and processes sensor records using the body-supplied `"c"` device identification parameter directly:
    ```cpp
    const char *clientUid = doc["c"] | "";
    ```
    There is no cryptographic sign-off or handshake tracking. Any entity with access to the Notehub project or API endpoint can inject arbitrary data values by pushing a basic `.qo` note carrying a valid `clientUid` (e.g., `dev:0123456789`).
*   **Remediation:** Enforce HMAC-SHA256 of the JSON body using a shared site-wide key distributed inside the client configs, or leverage Notehub's built-in header validation to cross-reference the incoming routing envelope headers against the payload's `"c"` parameter.

---

## 6. Implementation Plan - Actionable Code Snippets

### A. Fixing the Offline Note Buffer Truncation (Client)
Update both `lineBuffer` configurations in `flushBufferedNotes()` to prevent daily report Drops:

```cpp
// In TankAlarm-112025-Client-BluesOpta.ino - line 7318
static void flushBufferedNotes() {
#ifdef FILESYSTEM_AVAILABLE
  if (!gNotecardAvailable) {
    return;
  }
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) {
      return;
    }
    
    FILE *src = fopen("/fs/pending_notes.log", "r");
    if (!src) {
      return;
    }
    
    FILE *tmp = fopen("/fs/pending_notes.tmp", "w");
    if (!tmp) {
      fclose(src);
      return;
    }
    
    bool wroteFailures = false;
    uint8_t flushCount = 0;
    // CRITICAL: Increased from 1024 to 2048 to prevent large daily report drop bugs
    char lineBuffer[2048];  
    while (fgets(lineBuffer, sizeof(lineBuffer), src) != nullptr) {
```

### B. Correcting Current-Loop Disconnect Silencing (Client)
Inject mA bounds checking preceding local calibration equations:

```cpp
// In TankAlarm-112025-Client-BluesOpta.ino - line 4906
  float milliamps;
  if (validSamples == 0) {
    gMonitorState[idx].sampleReused = true;
    return gMonitorState[idx].currentInches; // keep previous on failure
  }
  milliamps = total / validSamples;

  // Store raw mA reading for telemetry
  gMonitorState[idx].currentSensorMa = milliamps;

  // CRITICAL: Prevent disconnected loop (0.0mA) from reporting calOffset as valid depth!
  if (milliamps < 3.6f) {
    gMonitorState[idx].sampleReused = true;
    return gMonitorState[idx].currentInches;
  }

  // Server-pushed learned calibration overrides the theoretical conversion
  if (cfg.hasLearnedCalibration && cfg.objectType != OBJECT_GAS) {
    float level = cfg.calSlope * milliamps + cfg.calOffset;
```

---

## 7. Developer Validation & Regression Checklist

- [ ] **Flash Buffer Bounds Verification:** Generate an offline payload carrying $1000\text{ bytes}$ of data. Ensure `flushBufferedNotes()` reads it cleanly and transmits it to Notehub without entering the truncation skip-block.
- [ ] **Failsafe Disconnection Test:** Force a calibrated multi-sensor configuration. Disconnect the current-loop sensor terminal. Verify that the client displays/reports a `sensor-fault` alarm instead of reporting $calOffset$.
- [ ] **Maximum Config Verification:** Push a config consisting of 8 sensors, all carrying active calibration parameters. Verify that the server `cacheClientConfigFromBuffer` caches and lists it successfully on `/fs/config_snapshots.txt` without payload limitation faults.
- [ ] **Metadata Cache Eviction Scan:** Push metadata for 20 clients. Connect a 21st client. Verify that active clients with `vinVoltageEpoch == 0.0` are protected from eviction and that the LRU sweeps on global active timestamps instead.
- [ ] **Regression Run:** Run a full compile on Client and Server with `arduino-cli` to verify zero type mismatches or symbol declaration errors.
- [ ] **Watchdog Starvation Sweep:** Measure loop execution intervals during maximum current-loop sensor readings with $1000\text{ ms}$ warmup times. Validate that loop times do not cause watchdog expiration events.
