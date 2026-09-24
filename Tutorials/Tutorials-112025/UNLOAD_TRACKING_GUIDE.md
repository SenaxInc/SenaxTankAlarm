# TankAlarm Unload Tracking Guide

**Monitoring Fill-and-Empty Tank Cycles for Delivery Tracking and Inventory Management**

---

## Introduction

Unload tracking is designed for tanks that follow a fill-and-empty cycle pattern - they gradually fill up over time, then are emptied (unloaded) in a single event. This feature automatically detects and logs these unload events, providing valuable data for delivery tracking, billing, and inventory management.

### What You'll Learn

- Understanding unload tracking vs continuous level monitoring
- Configuring unload detection parameters
- Setting up notifications for unload events
- Accessing and analyzing unload history
- Tuning detection for different tank types
- Troubleshooting false triggers or missed events

### Common Applications

**Ideal For:**
- ✅ Fuel delivery tanks (diesel, gasoline, propane)
- ✅ Milk collection tanks (dairy farms)
- ✅ Grain/feed storage bins
- ✅ Liquid fertilizer tanks
- ✅ Waste collection tanks

**Not Ideal For:**
- ❌ Process tanks with continuous in/out flow
- ❌ Balancing tanks with frequent fluctuations
- ❌ Buffer tanks with gradual consumption
- ❌ Water storage with steady draw

### Prerequisites

- [ ] Client device installed and operational
- [ ] Tank exhibits clear fill-and-empty pattern
- [ ] Accurate sensor calibration (see [Sensor Calibration Guide](SENSOR_CALIBRATION_GUIDE.md))
- [ ] Understanding of tank usage patterns
- [ ] Access to server dashboard

---

## How Unload Tracking Works

### The Fill-and-Empty Cycle

**Typical Pattern:**
```
100% ┤                    ╭─────────╮
     │                   ╱           ╲
 75% ┤                 ╱               ╲
     │               ╱                   ╲
 50% ┤             ╱                       ╲        ← UNLOAD
     │           ╱                           ╲      DETECTED
 25% ┤         ╱                               ╲
     │       ╱                                   ╲╲
  0% ┤─────╯                                       ╰────────
     └──────────────────────────────────────────────────────→
           Fill Phase        Peak      Empty Phase    Time
```

### Detection Algorithm

The system uses a **state-based peak tracking algorithm** with these steps:

#### Step 1: Peak Tracking Phase
```
When tank level rises above minimum threshold (default 12"):
  - Start tracking peak (highest) level
  - Record peak height and sensor reading
  - Update peak if level continues rising
  - Record timestamp of peak
```

#### Step 2: Unload Detection
```
When level drops significantly from peak:
  IF (drop_percentage > threshold) OR (drop_inches > threshold):
    - Require N consecutive low readings (debounce)
    - Calculate delivered amount: peak - current
    - Log unload event with details
    - Send notifications if configured
    - Reset to idle state
```

#### Step 3: Idle State
```
After unload detected:
  - Wait for tank to refill above minimum threshold
  - Prevents duplicate detections while tank stays empty
  - Returns to peak tracking when refill detected
```

### Smart Features

**Sensor Bottom Handling:**
- When level at/below sensor mount height, use configured `unloadEmptyHeight`
- Accounts for unreliable readings at very low levels
- Example: Sensor at 3", but configure 2" empty height for accuracy

**Debouncing:**
- Requires 3 consecutive readings below threshold
- Prevents false triggers from sensor noise
- Filters out brief level fluctuations (sloshing, waves)

**Rate Limiting:**
- After unload detected, won't trigger again until refill
- Prevents duplicate logging
- Saves cellular data usage

---

## Configuration

### Basic Configuration via Dashboard

**Step 1: Access Tank Configuration**

1. Open server dashboard: `http://<server-ip>/`
2. Select client device
3. Click "Configure" button
4. Navigate to target tank settings

**Step 2: Enable Unload Tracking**

```json
{
  "tanks": [
    {
      "id": "A",
      "name": "Diesel Fuel Tank",
      "enabled": true,
      
      // Standard level monitoring
      "highAlarm": 90.0,
      "lowAlarm": 10.0,
      
      // Unload tracking settings
      "trackUnloads": true,
      "unloadEmptyHeight": 2.0,
      "unloadDropPercent": 50.0,
      "unloadAlarmSms": true,
      "unloadAlarmEmail": true
    }
  ]
}
```

**Step 3: Send Configuration**

1. Click "Send Configuration to Device"
2. Wait for cellular transmission (5-10 minutes)
3. Verify in serial monitor: `"Unload tracking enabled for Tank A"`

### Configuration Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| **trackUnloads** | boolean | false | Enable/disable unload tracking |
| **unloadEmptyHeight** | float | 2.0 | Height to report when at/below sensor (inches) |
| **unloadDropThreshold** | float | 0.0 | Absolute drop to trigger (inches, 0 = use percent) |
| **unloadDropPercent** | float | 50.0 | Percentage drop from peak to trigger (%) |
| **unloadMinPeak** | float | 12.0 | Minimum peak height to start tracking (inches) |
| **unloadDebounceCount** | int | 3 | Consecutive low readings required |
| **unloadAlarmSms** | boolean | false | Send SMS notification on unload |
| **unloadAlarmEmail** | boolean | true | Include in daily email summary |

### Understanding Drop Thresholds

**Percentage-Based (Recommended):**
```
unloadDropPercent: 50.0
unloadDropThreshold: 0.0 (disabled)

Example:
  Peak: 80 inches
  Drop required: 80 × 50% = 40 inches
  Triggers when level ≤ 40 inches
```

**Absolute-Based (For Consistency):**
```
unloadDropPercent: 0.0 (disabled)
unloadDropThreshold: 60.0

Example:
  Peak: any height
  Drop required: 60 inches from peak
  Triggers when drop ≥ 60 inches
```

**Which to Use:**
- **Percentage**: When tank fill level varies (60-95%)
- **Absolute**: When consistent delivery amounts expected
- **Both disabled**: Uses default 50% percentage

---

## Tuning for Different Tank Types

### Small Tanks (<48" height)

**Challenges:**
- Small absolute drops even at 50%
- Sensor noise is proportionally larger
- May need tighter thresholds

**Recommended Settings:**
```json
{
  "trackUnloads": true,
  "unloadDropPercent": 40.0,    // Lower percentage
  "unloadMinPeak": 8.0,          // Lower minimum
  "unloadDebounceCount": 5,      // More debounce
  "unloadEmptyHeight": 1.0       // Match sensor mount
}
```

**Example: 36" Propane Tank**
- Peak typically: 30"
- Drop 40% = 12" → triggers at 18"
- Empty height: 1" (sensor at bottom)

### Large Tanks (>100" height)

**Challenges:**
- Percentage-based can trigger too early
- Want to detect full emptying
- Larger absolute values more reliable

**Recommended Settings:**
```json
{
  "trackUnloads": true,
  "unloadDropThreshold": 75.0,   // Absolute inches
  "unloadDropPercent": 0.0,      // Disable percentage
  "unloadMinPeak": 20.0,         // Higher minimum
  "unloadDebounceCount": 3,      // Standard debounce
  "unloadEmptyHeight": 3.0       // Sensor mount height
}
```

**Example: 120" Fuel Tank**
- Peak typically: 110"
- Drop 75" → triggers at 35"
- Detects 68% delivery (typical for fuel trucks)

### Partial Unload Tanks

**Some tanks are only partially emptied:**

**Scenario: Milk Tank (80" height)**
- Fills to 75"
- Milk truck removes ~60" (80% of volume)
- Leaves 15" as heel/buffer

**Recommended Settings:**
```json
{
  "trackUnloads": true,
  "unloadDropPercent": 70.0,     // 70% of peak
  "unloadMinPeak": 15.0,         // Must fill above heel
  "unloadEmptyHeight": 15.0,     // Heel height
  "unloadAlarmSms": false,       // Daily only (frequent)
  "unloadAlarmEmail": true
}
```

### Variable Fill Tanks

**Tanks with inconsistent fill levels:**

**Scenario: Waste Collection**
- Sometimes fills to 90%, sometimes 60%
- Empty on schedule, not threshold

**Recommended Settings:**
```json
{
  "trackUnloads": true,
  "unloadDropThreshold": 50.0,   // Absolute minimum drop
  "unloadMinPeak": 10.0,         // Any meaningful fill
  "unloadDebounceCount": 10,     // Heavy debounce
  "unloadAlarmSms": true,        // Important to track
  "unloadAlarmEmail": true
}
```

---

## Notifications and Reporting

### SMS Notifications

**When enabled** (`unloadAlarmSms: true`):

**Message Format:**
```
North Farm Diesel Fuel Tank unloaded: 78.5 in delivered (peak 85.0, now 6.5)
```

The sensor is named by its site and label, plus `#N` only when it has a Display Number
(`North Farm Diesel Fuel Tank #2 unloaded: ...`). The internal sensor number is never printed.

**Delivery Time:**
- Sent immediately when unload detected
- Routed via SMS contact list on server
- Subject to cellular network delays (30s-5min)

**Rate Limiting:**
- One SMS per unload event
- No duplicates even if multiple checks
- Included in daily SMS quota

### Email Summary

**When enabled** (`unloadAlarmEmail: true`):

**Daily Report Includes:**
```
=== UNLOAD EVENTS (Last 24 Hours) ===

Site: North Farm
Tank: Diesel Fuel Tank
Peak: 85.0 in (Jan 6, 2:45 PM)
Emptied: 78.5 in delivered
Final: 6.5 in (Jan 6, 4:20 PM)
Sensor: 19.2mA → 4.8mA

Site: South Field
Tank: Propane Storage
Peak: 68.0 in (Jan 6, 9:15 AM)
Emptied: 64.5 in delivered
Final: 3.5 in (Jan 6, 9:30 AM)
Sensor: 17.8mA → 4.1mA
```

### Accessing Unload History

**Via Server Dashboard:**

1. Navigate to **"Unload History"** page
2. View table of recent unloads (last 50 events)
3. Filter by client, tank, or date range
4. Export to CSV for further analysis

**Table Columns:**
- Date/Time
- Site Name
- Tank Label
- Peak Level (in)
- Empty Level (in)
- Delivered Amount (in)
- Duration (peak to empty)
- Sensor Readings (mA)

**Via API:**

```bash
GET http://<server-ip>/api/unloads

Response:
{
  "count": 15,
  "unloads": [
    {
      "timestamp": 1704672000,
      "site": "North Farm",
      "tank": "Diesel Fuel Tank",
      "peak": 85.0,
      "empty": 6.5,
      "delivered": 78.5,
      "peakTime": 1704658800,
      "duration": 13200
    },
    // ... more events
  ]
}
```

---

## Practical Applications

### Application 1: Fuel Delivery Billing

**Scenario:** Verify delivered gallons match invoice

**Setup:**
```json
{
  "trackUnloads": true,
  "unloadDropPercent": 0.0,
  "unloadDropThreshold": 60.0,
  "unloadAlarmEmail": true
}
```

**Workflow:**
1. Fuel truck delivers
2. Unload event triggers automatically
3. Email shows: "78.5 inches delivered"
4. Convert to gallons using tank geometry
5. Compare to delivery ticket

**Tank Geometry (Cylindrical):**
```
Delivered: 78.5 inches
Tank diameter: 72 inches (6 feet)
Area: π × (3 ft)² = 28.27 ft²
Volume: 28.27 ft² × (78.5/12 ft) = 185 ft³
Gallons: 185 × 7.48 gal/ft³ = 1384 gallons
```

**Verification:**
- Delivery ticket: 1400 gallons
- Calculated: 1384 gallons
- Difference: 16 gallons (1.1% - acceptable)

### Application 2: Milk Collection Tracking

**Scenario:** Track daily milk production

**Setup:**
```json
{
  "trackUnloads": true,
  "unloadDropPercent": 75.0,
  "unloadMinPeak": 10.0,
  "unloadEmptyHeight": 8.0,
  "unloadAlarmSms": false,
  "unloadAlarmEmail": true
}
```

**Analysis:**
```
Daily Email Summary:
  Jan 6: 72.0 in collected (Peak 80.0)
  Jan 5: 68.5 in collected (Peak 76.5)
  Jan 4: 71.2 in collected (Peak 79.2)
  
  7-Day Average: 70.5 inches = ~420 gallons/day
  Trend: Stable production
```

### Application 3: Scheduled Maintenance Detection

**Scenario:** Alert when waste tank is emptied (maintenance performed)

**Setup:**
```json
{
  "trackUnloads": true,
  "unloadDropThreshold": 40.0,
  "unloadAlarmSms": true,
  "unloadAlarmEmail": true
}
```

**Use:**
- SMS confirms maintenance crew emptied tank
- Timestamp verifies scheduled service
- Tracks maintenance frequency
- Audit trail for compliance

---

## Troubleshooting

### Unloads Not Being Detected

**Symptoms:**
- Tank clearly emptied but no unload logged
- Email reports show no events
- Dashboard unload history empty

**Diagnostic Steps:**

**1. Verify Configuration:**
```
Check via Dashboard or Serial Monitor:
  ✓ trackUnloads: true
  ✓ unloadDropPercent or unloadDropThreshold set
  ✓ unloadMinPeak appropriate for tank
```

**2. Check Peak Tracking:**
```
Serial Monitor Output:
  "Peak updated: Tank A = 82.5 in (19.1mA)"
  
If no peak messages:
  - Tank may not reach unloadMinPeak threshold
  - Lower unloadMinPeak value
```

**3. Monitor Level During Empty:**
```
Watch dashboard during known unload:
  Before: 85.0 inches
  During: Dropping... 60, 45, 30, 15...
  After: 5.0 inches
  
If drop < threshold:
  - Lower unloadDropPercent
  - Or set absolute unloadDropThreshold
```

**4. Check Debounce:**
```
Default requires 3 consecutive low readings:
  Sample 1: 50 in (above threshold) ✗
  Sample 2: 45 in (above threshold) ✗
  Sample 3: 35 in (below threshold) ✓ count=1
  Sample 4: 33 in (below threshold) ✓ count=2
  Sample 5: 32 in (below threshold) ✓ count=3 → TRIGGER
  
If rapidly emptying:
  - Reduce sample interval temporarily
  - Or reduce debounceCount to 2
```

**Solutions:**

```json
{
  // If tank doesn't reach high enough:
  "unloadMinPeak": 8.0,  // Lower from 12.0
  
  // If threshold too strict:
  "unloadDropPercent": 40.0,  // Lower from 50.0
  
  // If emptying too fast for debounce:
  "unloadDebounceCount": 2,  // Lower from 3
  
  // If using percentage when absolute needed:
  "unloadDropThreshold": 50.0,  // Set absolute
  "unloadDropPercent": 0.0      // Disable percentage
}
```

### False Unload Detections

**Symptoms:**
- Unload events logged when tank NOT actually emptied
- Multiple false triggers per day
- Nuisance SMS notifications

**Common Causes:**

**1. Sensor Noise:**
```
Level fluctuates due to:
  - Electrical interference
  - Poor grounding
  - Loose connections
  - Sensor malfunction
  
Solution:
  - Increase debounceCount to 5-10
  - Check sensor wiring
  - Add filtering capacitor
  - See Sensor Calibration Guide
```

**2. Normal Level Fluctuations:**
```
Tank level varies naturally:
  - Temperature changes (thermal expansion)
  - Sloshing from wind/vibration
  - Gradual consumption (not true unload)
  
Solution:
  - Increase unloadDropThreshold
  - Use absolute threshold vs percentage
  - Increase debounceCount
```

**3. Threshold Too Low:**
```
Configuration:
  unloadDropPercent: 30.0  ← Too sensitive
  
Tank pattern:
  Peak: 90 in
  Natural variation: ±15 in
  Drop to 75 in triggers false unload
  
Solution:
  unloadDropPercent: 60.0  ← More conservative
```

**Recommended Fix:**
```json
{
  "trackUnloads": true,
  "unloadDropPercent": 60.0,     // More conservative
  "unloadDropThreshold": 0.0,
  "unloadMinPeak": 15.0,         // Higher minimum
  "unloadDebounceCount": 7,      // Heavy filtering
  "unloadAlarmSms": false,       // Disable until stable
  "unloadAlarmEmail": true       // Keep email only
}
```

### Delayed or Missing Notifications

**Symptoms:**
- Unload detected (visible in dashboard history)
- But SMS not received
- Or email doesn't include unload

**Check SMS Configuration:**

**Server Settings:**
```json
{
  "smsPrimary": "+15555555555",
  "smsSecondary": "+15555555555",
  "smsTertiary": "+15555555555"
}
```

**Verify:**
- Phone numbers correct
- At least one contact configured
- Server Notecard has SMS capability

**Check Email Configuration:**
```json
{
  "dailyEmail": "alerts@company.com",
  "dailyHour": 6,
  "dailyMinute": 0
}
```

**Check Server Logs:**
```
Serial Monitor Output:
  "Unload detected for Tank A"
  "Sending SMS to +15555555555: Fail - no SMS credits"
  
or:
  "SMS sent successfully to +15555555555"
  
or:
  "Email queued for next daily report"
```

---

## Best Practices

### Configuration Guidelines

**Start Conservative:**
1. Begin with higher thresholds
2. Monitor for false triggers
3. Gradually lower if needed
4. Document final settings

**Test Before Production:**
1. Enable `trackUnloads: true`
2. Set `unloadAlarmSms: false` initially
3. Watch several real unload cycles
4. Verify detection accuracy
5. Enable SMS once stable

**Match Tank Behavior:**
- **Regular schedule**: Use consistent thresholds
- **Variable fills**: Use absolute thresholds
- **Partial unloads**: Account for heel/buffer
- **Frequent cycles**: Disable SMS, use email

### Maintenance

**Monthly:**
- Review unload history for patterns
- Check for false triggers
- Verify sensor calibration hasn't drifted

**Quarterly:**
- Compare unload data to actual delivery records
- Adjust thresholds if needed
- Update `unloadEmptyHeight` if sensor moved

**Annually:**
- Full system calibration
- Review all unload configurations
- Optimize based on year's data

### Data Management

**Export Regularly:**
```bash
# Download unload history CSV
GET /api/unloads?format=csv&days=30

# Import into spreadsheet for analysis
```

**Backup Configuration:**
- Document all unload settings
- Include in site documentation
- Version control configuration files

---

## Advanced Topics

### Custom Unload Detection Logic

**For complex scenarios, firmware customization may be needed:**

**Example: Two-Stage Partial Unload**
```cpp
// Detect partial unloads (milk tank example)
if (currentLevel < (peakLevel * 0.7) && currentLevel > 15.0) {
  logPartialUnload();  // First pickup
}
if (currentLevel < 10.0) {
  logFullUnload();  // Final emptying
}
```

**Example: Time-Based Filtering**
```cpp
// Only detect unloads during business hours
time_t now = rtc.getEpoch();
if (hour(now) >= 8 && hour(now) <= 17) {
  // Normal detection
} else {
  // Ignore level drops outside hours
}
```

### Integration with Billing Systems

**Export API for automated invoicing:**

```bash
# Fetch unloads for specific date range
GET /api/unloads?start=2026-01-01&end=2026-01-31&tank=A

# Process results:
for unload in response.unloads:
  delivered_inches = unload.delivered
  delivered_gallons = convertToGallons(delivered_inches, tankGeometry)
  
  createInvoice(
    customer = unload.site,
    date = unload.timestamp,
    quantity = delivered_gallons,
    unit_price = currentFuelPrice
  )
```

### Multiple Sensor Validation

**For critical applications, cross-check with multiple sensors:**

```cpp
// Require both sensors to agree
float sensor1 = readAnalog(CH0);
float sensor2 = readAnalog(CH1);

if (abs(sensor1 - sensor2) < 2.0) {  // Within 2 inches
  float avgLevel = (sensor1 + sensor2) / 2.0;
  checkForUnload(avgLevel);
} else {
  logError("Sensor mismatch - unload detection suspended");
}
```

---

## Resources

### Related Guides

- [Sensor Calibration Guide](SENSOR_CALIBRATION_GUIDE.md) - Ensure accurate readings
- [Advanced Configuration Guide](ADVANCED_CONFIGURATION_GUIDE.md) - Complex setups
- [Dashboard Guide](DASHBOARD_GUIDE.md) - Viewing unload history
- [Dashboard Guide](DASHBOARD_GUIDE.md) - Viewing data on the server web UI

### External References

- [Tank Volume Calculations](https://www.engineeringtoolbox.com/tank-volume-d_1820.html)
- [Level Sensor Best Practices](https://www.omega.com/en-us/resources/level-measurement)

---

*Unload Tracking Guide v1.1 | Last Updated: February 20, 2026*  
*Compatible with TankAlarm Firmware 1.1.2+*
