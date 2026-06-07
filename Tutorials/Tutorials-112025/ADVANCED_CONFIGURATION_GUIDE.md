# TankAlarm Advanced Configuration Guide

**Power User Settings and Optimization Techniques**

---

## Introduction

This guide covers advanced configuration options for experienced users who need to customize TankAlarm beyond standard deployments. Topics include multi-sensor configurations, custom object types, advanced sampling strategies, and system optimization for large fleets.

### Who This Guide Is For

- **Power users** with strong technical backgrounds
- **System integrators** deploying custom solutions
- **Fleet managers** overseeing 20+ devices
- **Developers** extending TankAlarm functionality

### Prerequisites

Before attempting advanced configuration:

- ✅ Completed basic installation ([Client](CLIENT_INSTALLATION_GUIDE.md) & [Server](SERVER_INSTALLATION_GUIDE.md))
- ✅ Familiar with JSON syntax
- ✅ Understand Blues Notehub routing
- ✅ Comfortable with serial monitor debugging
- ✅ Have working backup strategy ([Backup Guide](BACKUP_RECOVERY_GUIDE.md))

### What You'll Learn

- Multiple sensor types per client
- Custom object types and naming
- Advanced sampling strategies
- Event-based reporting optimization
- Watchdog and stability tuning
- Memory optimization for large fleets
- Custom alarm logic
- Performance tuning

---

## Multiple Sensor Types Per Client

### Beyond Tank Level Monitoring

Each client supports up to 8 analog channels (A0-A7 on Analog Expansion). You can monitor various asset types beyond tanks.

### Configuration Schema

**Standard (Single Tank Type):**

```json
{
  "objectType": "tank_level",
  "tanks": [
    {"id": "A", "name": "Diesel", "sensorType": "4-20mA", "channel": 0},
    {"id": "B", "name": "Propane", "sensorType": "4-20mA", "channel": 1}
  ]
}
```

**Advanced (Multiple Asset Types):**

```json
{
  "objectType": "multi_sensor",
  "assets": [
    {
      "type": "tank",
      "id": "A",
      "name": "Diesel Tank",
      "sensorType": "4-20mA",
      "channel": 0,
      "height": 96.0,
      "alarms": {...}
    },
    {
      "type": "engine",
      "id": "E1",
      "name": "Generator Coolant Temp",
      "sensorType": "0-10V",
      "channel": 2,
      "minValue": 0,
      "maxValue": 250,
      "unit": "°F",
      "alarmHigh": 220
    },
    {
      "type": "pump",
      "id": "P1",
      "name": "Transfer Pump Pressure",
      "sensorType": "4-20mA",
      "channel": 3,
      "minValue": 0,
      "maxValue": 100,
      "unit": "PSI",
      "alarmLow": 10,
      "alarmHigh": 90
    },
    {
      "type": "flow",
      "id": "F1",
      "name": "Water Flow Rate",
      "sensorType": "digital",
      "channel": 4,
      "pulsesPerGallon": 450,
      "totalizer": true
    }
  ]
}
```

### Custom Object Types

**Define Custom Objects:**

In `TankAlarm-112025-Common/src/ConfigSchema.h`:

```cpp
// Standard object types
#define OBJECT_TANK_LEVEL "tank_level"
#define OBJECT_MULTI_SENSOR "multi_sensor"

// Custom object types (add your own)
#define OBJECT_DAIRY_FARM "dairy_farm"
#define OBJECT_FUEL_DEPOT "fuel_depot"
#define OBJECT_WATER_TREATMENT "water_treatment"
```

**Custom Telemetry Structure:**

For a dairy farm monitoring milk tanks + feed silos + water:

```json
{
  "object": "dairy_farm",
  "site": "Green Acres Dairy",
  "deviceLabel": "Barn-01",
  "assets": {
    "milkTanks": [
      {"id": "M1", "level": 85.5, "temp": 38.2, "alarm": false},
      {"id": "M2", "level": 92.1, "temp": 38.5, "alarm": true}
    ],
    "feedSilos": [
      {"id": "F1", "level": 45.0, "type": "corn"},
      {"id": "F2", "level": 78.5, "type": "soy"}
    ],
    "waterTank": {
      "level": 55.0,
      "pressure": 45.2
    }
  },
  "timestamp": "2026-01-07T10:30:00Z"
}
```

**Benefits:**
- Group related sensors logically
- Custom dashboard layouts
- Industry-specific data models
- Simplified fleet management

### Implementation Example: Fuel Depot

**Scenario:** Fuel depot with 3 tanks + dispensing pump monitoring

**Configuration:**

```cpp
// In client firmware, define custom structure

struct FuelDepotConfig {
  // Diesel tank
  Tank tank_diesel;
  
  // Gasoline tanks
  Tank tank_regular;
  Tank tank_premium;
  
  // Pump monitoring
  struct {
    int channel;           // Analog input
    float pressureMin;     // PSI min
    float pressureMax;     // PSI max
    float alarmLowPSI;
    bool enabled;
  } dispenserPump;
  
  // Leak detection
  struct {
    int channel;
    float thresholdMV;     // Millivolts
    bool alarmOnDetect;
  } leakSensor;
};
```

**Telemetry:**

```json
{
  "object": "fuel_depot",
  "depot_id": "Location-42",
  "tanks": {
    "diesel": {"level": 78.5, "alarm": false},
    "regular": {"level": 45.2, "alarm": false},
    "premium": {"level": 92.1, "alarm": true}
  },
  "dispenser": {
    "pressure": 55.3,
    "alarm": false
  },
  "leak_detected": false
}
```

---

## Advanced Sampling Strategies

### Adaptive Sampling Rates

**Problem:** Fixed sampling wastes cellular data when tank stable, misses rapid changes during filling.

**Solution:** Dynamic sample rate based on activity.

**Implementation:**

```cpp
// In client firmware

int getSampleInterval() {
  static float lastLevel = 0.0;
  float currentLevel = readTankLevel();
  float change = abs(currentLevel - lastLevel);
  
  if (change > 5.0) {
    // Rapid change detected (filling/emptying)
    return 300;  // 5 minutes during activity
  } else if (change > 1.0) {
    // Moderate change
    return 900;  // 15 minutes
  } else {
    // Stable
    return 3600;  // 1 hour when stable
  }
}
```

**Configuration:**

```json
{
  "adaptiveSampling": {
    "enabled": true,
    "rapidInterval": 300,      // Seconds when changing fast
    "normalInterval": 1800,    // Normal rate
    "stableInterval": 3600,    // When very stable
    "changeThresholdRapid": 5.0,  // Inches
    "changeThresholdNormal": 1.0
  }
}
```

**Benefits:**
- Reduces cellular data usage by 60-80%
- Captures filling/delivery events with high resolution
- Automatic optimization without manual tuning

### Event-Based Reporting

**Send telemetry only when something happens:**

```json
{
  "eventReporting": {
    "enabled": true,
    "triggers": [
      "level_change",      // Level changed > threshold
      "alarm_state",       // Alarm triggered or cleared
      "unload_detected",   // Fill-and-empty cycle
      "relay_activated",   // Relay state changed
      "config_updated",    // New config received
      "error_occurred"     // System fault
    ],
    "reportThreshold": 2.0,   // Per-sensor, in the sensor's own unit
    "maxIntervalSeconds": 14400,   // Force update every 4 hours minimum
    "retryOnFailure": true
  }
}
```

**Normal Operation:**

```
Time    Event                    Action
10:00   Level stable @ 48.5"     No telemetry
10:30   Level stable @ 48.6"     No telemetry (< 2" change)
11:00   Level now 52.3"          SEND (3.8" change)
11:30   High alarm triggered     SEND (alarm event)
12:00   Level stable @ 92.1"     No telemetry
...
14:00   No events for 4 hours    SEND (max interval heartbeat)
```

**Data Savings:**

| Strategy | Daily Telemetry Events | Cellular Data (est.) |
|----------|------------------------|----------------------|
| Fixed 30 min | 48 | 96 KB |
| Adaptive | 15-25 | 30-50 KB |
| Event-based | 5-15 | 10-30 KB |

### Burst Reporting During Delivery

**Scenario:** Need high-resolution data during fuel delivery for billing.

**Configuration:**

```json
{
  "burstMode": {
    "enabled": true,
    "trigger": "unload_start",    // When filling begins
    "interval": 60,                // 1 min during delivery
    "duration": 3600,              // Max 1 hour burst
    "minChange": 0.5,              // Report every 0.5" change
    "returnToNormal": "auto"       // Auto-detect end
  }
}
```

**Behavior:**

```
Normal mode: Sample every 30 min

[10:00] Delivery truck arrives
[10:05] Level change detected (+5" in 5 min) → BURST MODE
[10:06] Report: 50.5"
[10:07] Report: 52.0"  (+1.5" in 1 min)
[10:08] Report: 54.3"
...
[10:45] Level stabilized (no change for 5 min) → NORMAL MODE
[11:15] Report: 95.2" (30 min schedule)
```

**Result:** High-resolution delivery data without constant high-rate sampling.

---

## Watchdog and Stability Tuning

### System Watchdog Configuration

**Purpose:** Automatically recover from firmware hangs or crashes.

**Default Settings:**

```cpp
// In TankAlarm-112025-Common/src/TankAlarm_Common.h
#define WATCHDOG_TIMEOUT_SECONDS 30  // 30-second hardware watchdog

// In TankAlarm-112025-Common/src/TankAlarm_Platform.h
// TANKALARM_WATCHDOG_AVAILABLE is auto-detected based on platform:
//   - Arduino Opta / Mbed OS: uses mbed::Watchdog (MbedWatchdogHelper class)
//   - STM32 (non-Mbed): uses IWatchdog library
```

**Customizing Timeout:**

To override the default 30-second timeout, define `WATCHDOG_TIMEOUT_SECONDS` before including the common header:

```cpp
// In your .ino file (before #include "TankAlarm_Common.h")
#define WATCHDOG_TIMEOUT_SECONDS 60  // Override: 60 seconds
```

**Platform-Agnostic Macros:**

The firmware provides macros that work on both Mbed and STM32 platforms:

```cpp
// In loop() — the firmware calls these automatically
TANKALARM_WATCHDOG_KICK(mbedWatchdog);   // Reset the watchdog timer
TANKALARM_WATCHDOG_START(mbedWatchdog, timeoutMs);  // Start watchdog
```

**Actual usage in firmware:**

```cpp
// In loop() and during long operations (FTP transfers, etc.)
#ifdef TANKALARM_WATCHDOG_AVAILABLE
  mbedWatchdog.kick();  // Reset timer during lengthy operations
#endif
```

**Diagnostic Logging:**

```
[10:30:00] Watchdog started (30000ms timeout)
[10:31:00] ERROR: Watchdog timeout! Resetting...
[10:31:05] System restarted (watchdog reset)
```

### Task Scheduling Optimization

**Problem:** Multiple slow tasks compete for CPU time.

**Solution:** Prioritized task scheduler.

**Implementation:**

```cpp
// Task priority system

enum TaskPriority {
  CRITICAL = 0,    // Watchdog feed, safety checks
  HIGH = 1,        // Sensor sampling, alarms
  MEDIUM = 2,      // Telemetry sending
  LOW = 3          // Logging, housekeeping
};

struct Task {
  void (*function)();
  TaskPriority priority;
  unsigned long intervalMs;
  unsigned long lastRun;
  bool enabled;
};

Task tasks[] = {
  {feedWatchdog, CRITICAL, 10000, 0, true},
  {sampleSensors, HIGH, 1800000, 0, true},
  {checkAlarms, HIGH, 30000, 0, true},
  {sendTelemetry, MEDIUM, 1800000, 0, true},
  {housekeeping, LOW, 3600000, 0, true}
};

void runScheduler() {
  unsigned long now = millis();
  
  // Run tasks in priority order
  for (int p = CRITICAL; p <= LOW; p++) {
    for (int i = 0; i < numTasks; i++) {
      if (tasks[i].priority == p && 
          tasks[i].enabled &&
          (now - tasks[i].lastRun) >= tasks[i].intervalMs) {
        
        tasks[i].function();
        tasks[i].lastRun = now;
        
        watchdog.feed();  // Feed between tasks
      }
    }
  }
}
```

**Benefits:**
- Critical tasks never starved
- Predictable timing
- Easier debugging
- Better watchdog interaction

---

## Memory Optimization for Large Fleets

### LittleFS Storage Management

**Challenge:** Server stores configs for all clients, fills up over time.

**Storage Breakdown:**

```
LittleFS Capacity: 1.5 MB

Usage:
├─ /server_config.json          5 KB
├─ /clients/ (100 devices)      500 KB
├─ /history/recent.json         400 KB
├─ /history/monthly_202601.json 300 KB
├─ /alarms/log.json            100 KB
├─ /unloads/log.json            50 KB
├─ Overhead & fragmentation    150 KB
└─────────────────────────────────────
Total:                         1.5 MB (100% full!)
```

**Optimization Strategies:**

**1. Tiered Historical Data**

```cpp
// Automatic data archival

if (littleFS.usedSpace() > 0.8 * littleFS.totalSpace()) {
  // 80% full - start archiving
  
  // Keep last 7 days detailed
  archiveOldDetailedData(7);
  
  // Compress older data (hourly averages only)
  compressData(30);  // Days 8-30
  
  // Delete very old data
  deleteOldData(90);  // >90 days
}
```

**2. Client Config Compression**

```cpp
// Store only delta from default

// Instead of full config:
{
  "site": "Farm 1",
  "deviceLabel": "Tank-01",
  "sampleSeconds": 1800,
  "tanks": [...],
  // ... 50 more fields
}

// Store delta:
{
  "_base": "default_v1",
  "site": "Farm 1",
  "deviceLabel": "Tank-01",
  "tanks": [
    {"id": "A", "highAlarm": 85.0}  // Only non-default values
  ]
}
```

**Savings:** 80% reduction per client config.

**3. Circular Buffer for Logs**

```cpp
// Fixed-size alarm log (newest overwrites oldest)

#define MAX_ALARM_ENTRIES 100

AlarmEntry alarmLog[MAX_ALARM_ENTRIES];
int alarmLogIndex = 0;

void addAlarmEntry(AlarmEntry entry) {
  alarmLog[alarmLogIndex] = entry;
  alarmLogIndex = (alarmLogIndex + 1) % MAX_ALARM_ENTRIES;
}
```

**4. FTP Offload**

Configure automatic backup to external FTP server:

```json
{
  "storage": {
    "autoArchive": true,
    "archiveThreshold": 0.75,  // At 75% full
    "ftpServer": "backup.company.com",
    "ftpPath": "/tankalarm/",
    "retentionDays": 30         // Delete local after upload
  }
}
```

### RAM Optimization

**Challenge:** Arduino Opta has limited RAM (~250 KB usable).

**Common RAM Hogs:**

```cpp
// BAD: Large global buffers
char jsonBuffer[10000];           // 10 KB always allocated
String largeString = "";          // Grows unpredictably

// GOOD: Dynamic allocation as needed
JsonDocument doc;                     // Only when needed
doc.garbageCollect();             // Free when done
```

**Optimization Techniques:**

**1. Use PROGMEM for Constants**

```cpp
// BAD: Strings in RAM
const char* errorMessages[] = {
  "Sensor reading out of range",
  "Configuration parse error",
  "Network communication timeout"
};

// GOOD: Strings in flash (PROGMEM)
const char errorMsg1[] PROGMEM = "Sensor reading out of range";
const char errorMsg2[] PROGMEM = "Configuration parse error";
const char errorMsg3[] PROGMEM = "Network communication timeout";

const char* const errorMessages[] PROGMEM = {
  errorMsg1, errorMsg2, errorMsg3
};
```

**2. Stream Processing**

```cpp
// BAD: Load entire file into RAM
File f = LittleFS.open("/history/data.json", "r");
String contents = f.readString();  // Could be 100+ KB!
f.close();
parseJSON(contents);

// GOOD: Stream processing
File f = LittleFS.open("/history/data.json", "r");
JsonDocument filter;
filter["timestamp"] = true;
filter["level"] = true;

while (f.available()) {
  DeserializationError error = deserializeJson(doc, f, filter);
  if (!error) {
    processEntry(doc);
  }
}
f.close();
```

**3. Memory Leak Detection**

```cpp
// Monitor free heap regularly

void checkMemoryLeaks() {
  static size_t lastFreeHeap = 0;
  size_t currentFree = freeMemory();
  
  if (lastFreeHeap > 0 && currentFree < (lastFreeHeap - 5000)) {
    Serial.printf("WARNING: Memory leak detected! Lost %d bytes\n", 
                  lastFreeHeap - currentFree);
  }
  
  lastFreeHeap = currentFree;
}

// Call periodically in loop
if (millis() % 60000 == 0) {  // Every minute
  checkMemoryLeaks();
}
```

---

## Custom Alarm Logic

### Multi-Condition Alarms

**Beyond simple high/low thresholds:**

**Example 1: Rate-of-Change Alarm**

Trigger if level drops too quickly (leak detection):

```cpp
struct RateAlarmConfig {
  bool enabled;
  float maxDropPerMinute;  // Inches/minute
  int consecutiveReadings;  // Debounce
};

bool checkRateAlarm(float currentLevel, float previousLevel, 
                    unsigned long deltaTimeMs, RateAlarmConfig& config) {
  if (!config.enabled) return false;
  
  float deltaLevel = previousLevel - currentLevel;
  float deltaMinutes = deltaTimeMs / 60000.0;
  float rate = deltaLevel / deltaMinutes;
  
  if (rate > config.maxDropPerMinute) {
    consecutiveCount++;
    if (consecutiveCount >= config.consecutiveReadings) {
      return true;  // LEAK ALARM!
    }
  } else {
    consecutiveCount = 0;
  }
  
  return false;
}
```

**Configuration:**

```json
{
  "alarms": {
    "rateOfChange": {
      "enabled": true,
      "maxDropPerMinute": 2.0,     // >2 in/min = leak
      "consecutiveReadings": 3     // 3 samples to confirm
    }
  }
}
```

**Example 2: Time-Based Alarm**

Different thresholds for different times (e.g., must be full by 6 AM):

```cpp
struct TimeBasedAlarm {
  int hour;            // 24-hour format
  int minute;
  float requiredLevel;  // Minimum level at this time
  char* message;
};

TimeBasedAlarm morningAlarm = {
  .hour = 6,
  .minute = 0,
  .requiredLevel = 80.0,
  .message = "Tank not filled for morning milking!"
};

bool checkTimeBasedAlarm(float currentLevel, TimeBasedAlarm& alarm) {
  time_t now = getNotecard Time();
  struct tm* timeinfo = localtime(&now);
  
  if (timeinfo->tm_hour == alarm.hour && 
      timeinfo->tm_min == alarm.minute) {
    
    if (currentLevel < alarm.requiredLevel) {
      sendAlarmSMS(alarm.message);
      return true;
    }
  }
  
  return false;
}
```

**Example 3: Predictive Alarm**

Estimate when tank will run empty based on current usage rate:

```cpp
float predictTimeToEmpty(float currentLevel, float usageRatePerHour) {
  // Linear prediction (simple model)
  float hoursRemaining = currentLevel / usageRatePerHour;
  return hoursRemaining;
}

bool checkPredictiveAlarm(float currentLevel, float usageRate) {
  float hoursToEmpty = predictTimeToEmpty(currentLevel, usageRate);
  
  if (hoursToEmpty < 24.0) {
    char msg[100];
    sprintf(msg, "Tank will run empty in %.1f hours at current usage rate!", 
            hoursToEmpty);
    sendAlarmSMS(msg);
    return true;
  }
  
  return false;
}
```

### Compound Conditions

**Multiple sensors must agree for alarm:**

```cpp
// Example: Two level sensors for redundancy

struct RedundantSensorConfig {
  int primaryChannel;
  int secondaryChannel;
  float maxDisagreement;  // Max difference before fault
};

enum SensorAgreement {
  AGREE_NORMAL,
  AGREE_ALARM,
  DISAGREE_FAULT
};

SensorAgreement checkRedundantSensors(RedundantSensorConfig& config) {
  float primary = readSensor(config.primaryChannel);
  float secondary = readSensor(config.secondaryChannel);
  float difference = abs(primary - secondary);
  
  if (difference > config.maxDisagreement) {
    // Sensors disagree - fault condition
    return DISAGREE_FAULT;
  }
  
  // Use average of both
  float avgLevel = (primary + secondary) / 2.0;
  
  if (avgLevel > highAlarmThreshold || avgLevel < lowAlarmThreshold) {
    return AGREE_ALARM;
  }
  
  return AGREE_NORMAL;
}
```

---

## Performance Tuning

### Notecard Communication Optimization

**Reduce cellular usage:**

**1. Outbound Queue Consolidation**

```cpp
// Instead of sending each event immediately:
notecard.sendNote(tankAlevel);
notecard.sendNote(tankBlevel);
notecard.sendNote(tankClevel);

// Batch into single note:
JsonObject payload = doc["tanks"].to<JsonObject>();
payload["A"] = tankAlevel;
payload["B"] = tankBlevel;
payload["C"] = tankClevel;
notecard.sendNote(payload);
```

**Savings:** 3 cellular transmissions → 1 transmission

**2. Sync Interval Tuning**

```cpp
// Default: Sync every 15 minutes (aggressive)
notecard.setSyncMode("periodic", 900);

// Optimized: Sync hourly for stable systems
notecard.setSyncMode("periodic", 3600);

// Or: Manual sync only (most efficient)
notecard.setSyncMode("manual");
// Then manually trigger:
notecard.syncNow();  // When you have data to send
```

**3. Template-Based Notes**

Define payload structure once, reuse:

```cpp
// Define template (one-time)
J *req = notecard.newRequest("note.template");
JAddStringToObject(req, "file", "tank_level.qo");
J *body = JAddObjectToObject(req, "body");
JAddNumberToObject(body, "level_A", 14.2);
JAddNumberToObject(body, "level_B", 14.2);
JAddBoolToObject(body, "alarm", true);
notecard.sendRequest(req);

// Send data using template (compact)
req = notecard.newRequest("note.add");
JAddStringToObject(req, "file", "tank_level.qo");
body = JAddObjectToObject(req, "body");
JAddNumberToObject(body, "level_A", currentLevelA);
JAddNumberToObject(body, "level_B", currentLevelB);
notecard.sendRequest(req);
```

**Savings:** ~40% reduction in payload size.

### Sensor Reading Optimization

**Reduce ADC conversion time:**

```cpp
// Default: Multiple readings with delay
float total = 0.0;
for (int i = 0; i < 10; i++) {
  total += analogRead(channel);
  delay(10);  // 10ms between readings
}
float average = total / 10.0;
// Total time: 100ms

// Optimized: Hardware averaging (if supported)
analogReadResolution(12);  // 12-bit ADC
analogReadAveraging(16);   // Hardware averages 16 samples
float reading = analogRead(channel);
// Total time: ~5ms
```

### Ethernet Performance (Server)

**1. Connection Keep-Alive**

```cpp
// Enable TCP keep-alive for persistent connections
EthernetClient client = server.available();
if (client) {
  client.setConnectionTimeout(30000);  // 30 sec timeout
  // Process request
}
```

**2. Response Caching**

```cpp
// Cache frequently accessed data
static String cachedDashboard = "";
static unsigned long dashboardCacheTime = 0;

String getDashboard() {
  if (millis() - dashboardCacheTime > 5000) {
    // Regenerate after 5 seconds
    cachedDashboard = generateDashboardHTML();
    dashboardCacheTime = millis();
  }
  return cachedDashboard;
}
```

---

## Fleet Management at Scale

### Hierarchical Configuration

**For 100+ devices, use inheritance:**

**Base Template:**

```json
{
  "_template": "default_tank_client_v1",
  "sampleSeconds": 1800,
  "tanks": [
    {
      "sensorType": "4-20mA",
      "tankHeight": 96.0,
      "fullLevel": 90.0,
      "emptyLevel": 6.0,
      "highAlarm": 85.0,
      "lowAlarm": 10.0
    }
  ]
}
```

**Individual Clients:**

```json
{
  "_base": "default_tank_client_v1",
  "_overrides": {
    "site": "Farm 42",
    "deviceLabel": "North Tank",
    "tanks": [
      {"id": "A", "name": "Diesel"}  // Only what's different
    ]
  }
}
```

**Benefits:**
- Update 100 devices by changing template
- Individual customization still possible
- Reduces configuration storage
- Easier fleet-wide standardization

### Bulk Operations

**Update multiple clients simultaneously:**

```bash
# Via API - update all clients in array
curl -X POST http://server-ip/api/config/bulk \
  -H "Content-Type: application/json" \
  -d '{
    "pin": "1234",
    "clients": [
      "dev:864475044012345",
      "dev:864475044056789",
      "dev:864475044098765"
    ],
    "config": {
      "sampleSeconds": 3600
    }
  }'
```

**Rolling Updates:**

Stagger updates to avoid overwhelming network:

```cpp
// Update 10 clients per hour
for (int i = 0; i < clientList.size(); i++) {
  updateClient(clientList[i], newConfig);
  delay(360000);  // 6 minutes between updates
}
```

### Fleet Health Monitoring

**Dashboard for fleet overview:**

```
Fleet Status: 98 clients

┌─ Health Overview ──────────────────────────┐
│  ✅ Online:      95 (97%)                   │
│  ⚠️  Stale Data:  2 (2%)                    │
│  ❌ Offline:      1 (1%)                    │
│  🔴 Alarms:       5 (5%)                    │
└────────────────────────────────────────────┘

Recent Activity:
├─ 10:30 - Farm 12 Tank A: HIGH ALARM
├─ 10:25 - Farm 45 Tank B: Config updated
├─ 10:20 - Farm 78: Came online after 2 days
└─ 10:15 - Farm 03 Tank C: Unload detected
```

**Automated Health Checks:**

```cpp
void fleetHealthCheck() {
  for (Client& client : allClients) {
    unsigned long timeSinceLastSeen = millis() - client.lastSeenTimestamp;
    
    if (timeSinceLastSeen > 86400000) {  // 24 hours
      sendAlertEmail("Client offline for 24+ hours: " + client.site);
    }
    
    if (client.alarmActive && !client.alarmAcknowledged) {
      escalateAlarm(client);
    }
    
    if (client.firmwareVersion != latestVersion) {
      scheduleOTAUpdate(client);
    }
  }
}
```

---

## Troubleshooting Advanced Configurations

### Common Issues

**1. Memory Exhaustion**

**Symptoms:**
- Random crashes
- Config changes don't save
- Telemetry failures

**Diagnosis:**

```cpp
Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
// If <10KB, you have a problem
```

**Solutions:**
- Reduce history retention
- Use PROGMEM for constants
- Implement streaming instead of buffering
- Check for memory leaks

**2. Watchdog Resets**

**Symptoms:**
- Device resets unexpectedly
- Serial shows "Watchdog reset"

**Solutions:**
- Increase watchdog timeout
- Add `watchdog.feed()` in long loops
- Profile slow functions
- Reduce task load

**3. Configuration Conflicts**

**Symptoms:**
- Settings don't apply as expected
- Alarms trigger incorrectly

**Diagnosis:**

```bash
# Retrieve current config via API
curl http://server-ip/api/client/dev:123/config

# Compare with intended config
diff intended.json actual.json
```

**Solutions:**
- Validate JSON syntax
- Check for duplicate keys
- Verify no conflicting settings

---

## Resources

### Related Guides

- [Client Installation Guide](CLIENT_INSTALLATION_GUIDE.md)
- [Server Installation Guide](SERVER_INSTALLATION_GUIDE.md)
- [Sensor Calibration Guide](SENSOR_CALIBRATION_GUIDE.md)
- [Troubleshooting Guide](TROUBLESHOOTING_GUIDE.md)

### External References

- [Arduino Opta Technical Specs](https://docs.arduino.cc/hardware/opta)
- [Blues Notecard API Reference](https://dev.blues.io/api-reference/notecard-api/introduction/)
- [ArduinoJson Documentation](https://arduinojson.org/)

---

*Advanced Configuration Guide v1.1 | Last Updated: February 20, 2026*  
*Compatible with TankAlarm Firmware 1.1.2+*
