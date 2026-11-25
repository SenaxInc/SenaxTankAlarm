# TankAlarm Dashboard Guide

**Using the Server Web Interface for Monitoring and Configuration**

---

## Introduction

The TankAlarm server provides a comprehensive web-based dashboard for real-time tank monitoring, alarm management, and remote client configuration. This guide covers all dashboard features and best practices for effective system management.

### What You'll Learn

- Accessing and navigating the dashboard
- Real-time tank monitoring
- Configuring clients remotely  
- Managing alarms and notifications
- Viewing historical data
- Controlling relays
- Customizing the dashboard
- Mobile browser optimization

### Dashboard Overview

**Access:** `http://<server-ip>/`

**Key Features:**
- 📊 Real-time tank level visualization
- 🔔 Alarm status indicators
- ⚙️ Remote configuration console
- 📈 Historical data graphs
- 🎛️ Relay control buttons
- 📱 Mobile-responsive design
- 🔒 PIN-protected admin functions

---

## Accessing the Dashboard

### Initial Setup

**Step 1: Determine Server IP Address**

**Method A: Serial Monitor**

1. Connect server Opta via USB-C
2. Open Arduino IDE serial monitor (115200 baud)
3. Look for startup messages:

```
Ethernet: CONNECTED
IP Address: 192.168.1.150  ← This is your dashboard URL
Subnet: 255.255.255.0
Gateway: 192.168.1.1
Web server started at: http://192.168.1.150/
```

**Method B: Router Admin Panel**

1. Log into your router (usually `192.168.1.1` or `192.168.0.1`)
2. Check DHCP leases or connected devices
3. Look for device named "Arduino-Opta" or with MAC starting with your Opta's ID

**Method C: Network Scanner**

```bash
# Windows
arp -a

# Mac/Linux
arp -a
# Or use nmap:
nmap -sn 192.168.1.0/24
```

**Step 2: Open Dashboard**

1. From computer on same network
2. Open web browser (Chrome, Firefox, Edge, Safari)
3. Navigate to: `http://192.168.1.150/` (use your server's IP)
4. Bookmark for easy access

**Step 3: Set Admin PIN (First Time)**

1. Dashboard prompts: "Set Admin PIN"
2. Enter 4-digit PIN (e.g., `1234`)
3. Confirm PIN
4. Click **"Set PIN"**
5. PIN now required for configuration changes

---

## Dashboard Layout

### Main Dashboard (`/`)

**Top Navigation Bar:**
```
┌────────────────────────────────────────────────────────┐
│  TankAlarm Server  │  Last Update: 10:30:15 AM        │
│  192.168.1.150     │  Status: Connected ●              │
└────────────────────────────────────────────────────────┘
```

**Tank Status Grid:**
```
┌──────────────────┬──────────────────┬──────────────────┐
│  North Farm      │  South Farm      │  Main Storage    │
│  Tank A          │  Tank A          │  Tank A          │
│                  │                  │                  │
│  ████████░░      │  ███░░░░░░░      │  █████████░      │
│  82.5 in         │  31.2 in         │  95.3 in         │
│  (86% full)      │  (33% full)      │  (99% full)      │
│                  │  ⚠️ LOW ALARM    │  🔴 HIGH ALARM   │
│  Updated: 10:29  │  Updated: 10:28  │  Updated: 10:30  │
│  [Configure]     │  [Configure]     │  [Configure]     │
│  [Relay R1: ON]  │  [Relay R1: OFF] │  [Relay R1: OFF] │
└──────────────────┴──────────────────┴──────────────────┘
```

**Bottom Status Bar:**
```
┌────────────────────────────────────────────────────────┐
│  5 Clients Online │ 1 Alarm Active │ Last Sync: 10:30 │
│  [Server Config] │ [View History] │ [Serial Monitor]  │
└────────────────────────────────────────────────────────┘
```

### Components Explained

**Tank Card:**
- **Site Name**: Top label (e.g., "North Farm")
- **Tank ID**: Which tank (A-H)
- **Level Bar**: Visual fill indicator
- **Numeric Level**: Current height in inches (or cm)
- **Percentage**: Calculated from tank height
- **Alarm Indicators**: Color-coded status
  - 🟢 Green: Normal operation
  - 🟡 Yellow: Approaching threshold
  - 🔴 Red: Alarm active (high or low)
- **Timestamp**: When last updated
- **Actions**: Configure, Relay control buttons

**Alarm Color Codes:**

| Color | Meaning | Action Required |
|-------|---------|-----------------|
| Green background | Normal | None |
| Yellow background | Within 10% of threshold | Monitor |
| Red background | Alarm active | Investigate |
| Gray background | Stale data (>2 hours) | Check connectivity |

---

## Real-Time Monitoring

### Understanding Tank Display

**Level Visualization:**

```
Tank Height: 96 inches (full scale)
Current Level: 48 inches

Visual bar shows:
████████████░░░░░░░░░░░░░  50% filled
```

**Bar fills from bottom:**
- Empty (0%): Completely white/empty
- Half (50%): Half filled
- Full (100%): Completely filled
- Overfill (>100%): Special indicator

**Numeric Readout:**

```
48.5 in
(51% full)
```

- **48.5 in**: Actual measured height
- **(51% full)**: Percentage based on tank configuration
  - Calculated as: `(current - emptyLevel) / (fullLevel - emptyLevel) × 100`
  - Accounts for unusable heel and headspace

**Example Calculation:**
```
Tank config:
  tankHeight = 96 in (physical tank)
  fullLevel = 90 in (usable max, accounting for headspace)
  emptyLevel = 6 in (heel/unusable)

Current reading: 48 in

Percentage = (48 - 6) / (90 - 6) × 100
           = 42 / 84 × 100
           = 50%
```

### Data Freshness

**Timestamp Indicators:**

```
Updated: 10:30 AM  ← Fresh (green)
Updated: 9:45 AM   ← Recent (green)
Updated: 6:15 AM   ← Stale (yellow)
Updated: Yesterday ← Very stale (red)
```

**Auto-Refresh:**
- Dashboard polls server every 30 seconds
- New data appears automatically
- No manual refresh needed

**Manual Refresh:**
- Click **"Refresh"** button in navigation
- Or use browser refresh (F5)
- Force update: Ctrl+F5 (clears cache)

### Alarm Monitoring

**Active Alarms:**

Dashboard highlights in red:
```
┌──────────────────┐
│  🔴 HIGH ALARM   │
│  North Farm - A  │
│  95.3 in / 90.0  │
│  Triggered: 10:15│
└──────────────────┘
```

**Alarm Details:**
- **Threshold**: 95.3 in exceeds high alarm of 90.0 in
- **Duration**: How long in alarm state
- **Notification Status**: SMS/email sent indicator

**Alarm Summary:**

```
Active Alarms (3):
├─ North Farm Tank A: HIGH (95.3 > 90.0)
├─ South Farm Tank B: LOW (8.2 < 10.0)
└─ East Site Tank A: HIGH (112.0 > 110.0)
```

**Clearing Alarms:**
- Alarms clear automatically when level returns to normal
- Must cross hysteresis threshold
  - High alarm @ 90 with 5" hysteresis: Must drop to ≤85 to clear
  - Low alarm @ 10 with 5" hysteresis: Must rise to ≥15 to clear

---

## Client Configuration Console

Access via: `http://<server-ip>/client-console`

### Selecting a Client

**Client Dropdown:**
```
Select Client: [  North Farm Tank-01  ▼ ]
```

**Dropdown shows:**
- Site name + device label (if configured)
- Or device UID (if no label)
- Online status indicator (● green = online, ● gray = offline)

**After selecting:**
- Current configuration loads
- Editable form appears
- All settings visible

### Configuration Sections

**1. Device Identification**

```
┌─ Device Settings ───────────────────────────┐
│                                              │
│  Site Name:     [North Farm           ]     │
│  Device Label:  [Tank-01              ]     │
│  Location:      [40.7128, -74.0060    ]     │
│                  (Optional GPS)              │
│                                              │
│  Device UID:    dev:864475044012345          │
│  Last Seen:     10:30:15 AM (2 min ago)      │
│  Firmware:      v1.1.2                       │
└──────────────────────────────────────────────┘
```

**Fields:**
- **Site Name**: Friendly location name (e.g., "North Farm")
- **Device Label**: Specific identifier (e.g., "Diesel Tank")
- **Location**: GPS coordinates (optional, for mapping)
- **Device UID**: Read-only, unique identifier
- **Last Seen**: When last telemetry received
- **Firmware**: Client firmware version

**2. Sampling Configuration**

```
┌─ Sampling Settings ──────────────────────────┐
│                                              │
│  Sample Interval:  [1800] seconds (30 min)  │
│  Level Change Threshold: [1.0] inches        │
│                                              │
│  ☐ Event-based reporting (send on change)   │
│  ☑ Periodic reporting (send on schedule)    │
└──────────────────────────────────────────────┘
```

**Settings:**
- **Sample Interval**: How often sensors are read (seconds)
  - Minimum: 300 (5 min)
  - Default: 1800 (30 min)
  - Maximum: 86400 (24 hours)
- **Level Change Threshold**: Send telemetry if level changes by this amount
  - Enables event-based reporting
  - Saves cellular data when tank stable
- **Reporting Mode**:
  - Event-based: Only send when level changes significantly
  - Periodic: Send every sample regardless of change
  - Both: Send on change OR schedule (recommended)

**3. Tank Configuration**

Each client supports up to 8 tanks (A-H):

```
┌─ Tank A Configuration ───────────────────────┐
│                                              │
│  ☑ Enabled                                   │
│  Tank Name:        [Diesel             ]     │
│  Tank Height:      [96.0] inches             │
│  Full Level:       [90.0] inches (usable)    │
│  Empty Level:      [6.0] inches (heel)       │
│                                              │
│  Sensor Type:      [4-20mA          ▼ ]     │
│  Sensor Channel:   [CH0 (A0)        ▼ ]     │
│                                              │
│  ☑ High Alarm Enabled                        │
│    Threshold:      [85.0] inches             │
│    Hysteresis:     [5.0] inches              │
│  ☑ Low Alarm Enabled                         │
│    Threshold:      [10.0] inches             │
│    Hysteresis:     [5.0] inches              │
│                                              │
│  ☐ Track Unloads (fill-and-empty detection) │
│  ☐ Enable Relay Control                     │
│                                              │
│  [Calibration...] [Test Reading]             │
└──────────────────────────────────────────────┘
```

**Tank Settings:**

**Basic:**
- **Enabled**: Turn this tank on/off
- **Tank Name**: Friendly label (e.g., "Diesel", "Propane")
- **Tank Height**: Physical tank height (inches or cm)
- **Full Level**: Maximum usable level (accounting for headspace)
- **Empty Level**: Minimum level (unusable heel)

**Sensor:**
- **Sensor Type**: 4-20mA, 0-10V, Digital/SDI-12
- **Sensor Channel**: Which analog input (CH0-CH7 = A0-A7)

**Alarms:**
- **High Alarm**: Trigger when level exceeds threshold
  - **Threshold**: Alarm setpoint (inches)
  - **Hysteresis**: How far below threshold to clear alarm
- **Low Alarm**: Trigger when level falls below threshold
  - **Threshold**: Alarm setpoint
  - **Hysteresis**: How far above threshold to clear alarm

**Advanced:**
- **Track Unloads**: Enable fill-and-empty cycle detection
- **Relay Control**: Enable automatic relay activation on alarm

**Actions:**
- **Calibration**: Opens calibration wizard
- **Test Reading**: Immediate sensor sample (diagnostic)

**4. Notification Settings**

```
┌─ Notifications ──────────────────────────────┐
│                                              │
│  ☑ SMS Notifications                         │
│  ☑ Email Notifications                       │
│  ☑ Include in Daily Report                   │
│                                              │
└──────────────────────────────────────────────┘
```

**Options:**
- **SMS**: Send text messages on alarms
- **Email**: Send emails on alarms
- **Daily Report**: Include this client in daily summary

> **Who receives SMS?** Recipients are managed centrally on the server: Contacts → SMS
> Alert Recipients. Per-contact **alarm associations** control which alarms each contact
> receives. Use the **Send Test SMS** button on the server Settings page to verify
> delivery end-to-end. While an alarm stays active, the server re-sends a reminder SMS
> every 6 hours (configurable `smsReminderHours`, 0 disables) until the alarm clears.

### Charger Check (SunSaver MPPT)

A client that reads its SunSaver MPPT over RS-485 (**Voltage Monitor** set to **RS485**) also reads what the charger has stored: the charging profile of both banks, the low voltage disconnect and reconnect, and the DIP switches. It compares that with the **Battery** selected in its configuration and sends the result to the server. The check only reads; to have the client write a LiFePO4 profile into the charger, see [Charger Programming](#charger-programming-sunsaver-mppt) below.

**Where to find it:**
- On a site page (`/site-config?site=...`), each client shows a badge next to its RS-485 badge: **Charger settings OK**, **Charger settings WRONG**, **Charger: cannot tell** or **Charger: not judged**. Hover over it for the time of the check, and click it for the details.
- **Charger: check pending** means that the battery in the client's configuration has changed since its last check, so that check's verdict is not about the battery configured now. The client checks again once it runs the new configuration. The charger page says the same above the old check.
- In the configuration console, the hardware note under **Power Configuration** links to the same page for the client you are editing.
- The page itself is `/charger?uid=<device UID>`. It lists what was found and every register with its value.

**What the verdicts mean:**
- **Settings suit the battery**: nothing wrong was found.
- **Settings do not suit the battery**: for example a lithium battery on a charger whose DIP switch 1 is OFF (it then charges with a built-in lead-acid profile), a stored equalize voltage, or temperature compensation switched on.
- **Could not be judged in full**: a register did not answer, the charger's voltage encoding could not be told from its setpoints, or the stored voltages were left unjudged because this is a 24 V system or the battery voltage the charger reports does not tell whether this is a 12 V or a 24 V system. What could be checked showed nothing wrong: a setting that was read and is wrong gives **Settings do not suit the battery**, whatever else could not be read. That includes an absorption or float voltage of 0 on a 12 V system, which is wrong whatever the encoding (a float voltage of 0 is no float at all).
- **Not judged**: there are no rules for the battery that is configured (none, custom, LiPo).

The client checks after it starts, after a change to its battery or charger settings, after it has programmed the charger, and once a day. It sends a new report when something has changed, and once a week otherwise. A report carries the time of its check and the server keeps the newest, so a report waits for the client's clock: after a power cycle it goes out once the client has been online. It also waits while the client cannot reach its Notecard, and is tried again every 10 minutes. A page that says no check has arrived means the client has no SunSaver configured, has not reached it yet, or runs firmware from before the charger check.

### Charger Programming (SunSaver MPPT)

The SunSaver MPPT has no lithium preset: with DIP switch 1 OFF it charges with a built-in lead-acid profile. A client that reads its SunSaver over RS-485 can write a charging profile for a 12 V LiFePO4 battery, and the low voltage disconnect (LVD) and reconnect (LVR), into the charger's memory. It does so only when a person asks for it in the client's config, once for each request.

> **Firmware:** Charger programming is in client firmware built with `TANKALARM_SUNSAVER_PROGRAMMING` set to 1 (in `TankAlarm-112025-Common/src/TankAlarm_Solar.h`). It stays 0 until a run on a real SunSaver has shown how the charger takes the writes and the restart. A client built with 0 writes nothing, and its charger page says "This client's firmware was built without charger programming."

**To program a charger:**
1. Open the client's config in the configuration console.
2. Set **Battery** to **LiFePO4**, **Battery Pack Voltage** to **12V** and **Voltage Monitor** to **RS485**. The section **SunSaver Charging Profile (12 V LiFePO4)** appears under **Power Configuration**.
3. Check the values. The defaults suit a battery such as the Optima ORANGETOP, which switches itself off above 14.8 V: absorption 14.20 V for 15 minutes, float 13.50 V, high voltage disconnect 14.45 V and reconnect 13.60 V, LVD 13.10 V and LVR 13.25 V. Each field shows its limits; the server and the client refuse values outside them, and voltages that are not in steps of 0.05 V.
4. Tick **This Opta is not powered from the LOAD terminals of the charger** if that is so. The charger may switch its LOAD output off while it waits for its restart, so without the tick the client refuses to begin while current flows from the LOAD terminals.
5. Tick **Write these values to the charger** and save. The server refuses the save when the config is not for a 12 V LiFePO4 battery on RS-485, or when its own clock is not set yet. The box is unticked after each save, so a later save makes no new request. It does not cancel a request that still waits either: a later save with the same values keeps it, and the client may still write them, while a save with other values withdraws it (see **A request that waits** below).
6. Wait for the result on the client's charger page (see below). Once it says **Programmed** or **Unchanged**, set DIP switch 1 ON at the charger (it then uses the stored charge values), switch 2 ON (the stored LVD and LVR) and switch 3 OFF (no equalization). Until then leave switch 1 as it is: while the request waits, or after any other result, the charger's memory may still hold values that do not suit a LiFePO4 battery, and with switch 1 ON the charger would charge with them. After another result, ask again once its cause is dealt with: a charger that already holds the values gives **Unchanged**. The client cannot change the switches; the result says when one of them is set otherwise.

**What the client does:** It waits up to an hour for its clock and for the charger to answer, and refuses a request that is more than a day old by then. It reads the charger first and writes nothing when the charger reports a fault, when the battery voltage does not show a 12 V system, when the battery is below the new LVR, when current flows from the LOAD terminals and the request does not allow it, or when the way the charger encodes its voltages cannot be proven from its settings. Otherwise it restarts the charger before it writes anything, to be sure that the restart command works. It writes only once the charger has shown that it restarted: it stopped answering for a moment and came back, or a pending change it reported has cleared. A charger that keeps answering throughout gives no such sign, and the client then refuses to write. Then it writes the registers that differ, one at a time, and reads each one back; restarts the charger again, which puts the new settings to work; and reads them all once more. The run takes about 10 seconds, and the client does nothing else meanwhile. A charger that answers very slowly can make it longer, but never longer than a minute: at that limit the client writes nothing more, still sends the restart to a charger it has written to, and says so in the result. A run is never repeated by itself.

**Where to see the result:** on the client's charger page (`/charger?uid=<device UID>`), in the **Charger Programming** card, which comes first once a client has been asked. It says **Waiting** until the result arrives, and then:
- **Programmed**: the charger holds the values and has restarted. **Unchanged**: it held them already, and nothing was written.
- **Refused** or **No link**: nothing was written. The reasons say why.
- **Interrupted**: the client restarted during the run, or while the request waited. After a restart during a run, it looks at the charger that the run talked to, and restarts the charger if it waits for a restart. It does so even when the config now names another charger, or has the voltage monitor off, since a charger that waits for its restart may not charge. When the charger does not answer, or the client cannot start its RS-485 link to ask it, the card says so and the client looks again: 10 and 20 minutes later, then once a day. The charger check that follows shows what the charger holds. A client that started without its flash, or whose flash had to be formatted at the start and lost its files, cannot tell whether a run was cut short, so it looks at the charger the same way, and the result is reported for request 0. After lost files the look stays owed through further restarts until it is done. A new client whose flash holds nothing yet does not look, unless it lost power in the moment between formatting its flash at its first start and writing its first file: its next start cannot tell that from lost files, and looks.
- **Unverified**: the values were written and the charger restarted, but not every register answered when the client read them once more. The register table shows which ones; the charger check that follows shows what the charger holds.
- **Stopped**, **Restart failed** or **Verify failed**: values were written, but the run could not end as asked. **Restart failed** also covers a charger that gave no sign of its restart. The register table shows each register as it was before, the value wanted, and what it holds now.

The writes go in an order that, wherever a run stops and at any battery temperature, leaves the charger charging no higher than the higher of the values it had and the values asked for, and cutting off the load no lower than the lower of the two. A limit that is raised (the high voltage disconnect, say) is written only after the charge voltages it holds down have come down. Temperature compensation lowers the charge voltages on a warm battery and raises them on a cold one, so it is switched off after the charge voltages that come down and before those that go up. The client does not put the old values back. After every run the client checks the charger again and sends that report at once, so the charger check on the same page judges what the charger holds now.

**A request that waits:** a client may take a while to get its config or to reach its charger, and until it has answered, the card shows the request as waiting. Saving the config again with the same values, and the box of step 4 as it was, keeps the request, and the client may still write them. Saving it with other values, with that box ticked otherwise, with another battery or none, or with the voltage monitor off withdraws it, and the client reports it as refused. So does a config that names another charger (another Modbus id), which the configuration console does not send but a config made another way can. A client that restarts while a request waits reports it as interrupted, and does not run it. To write the values again, tick **Write these values to the charger** again and save.

### Saving Configuration

**Send to Device:**

1. Make changes in form
2. Scroll to bottom
3. Enter admin PIN
4. Click **"Send to Device"**

```
┌─ Save Configuration ─────────────────────────┐
│                                              │
│  Admin PIN: [****]                           │
│                                              │
│  [Cancel]  [Send to Device]                  │
└──────────────────────────────────────────────┘
```

**Process:**
1. Server validates changes
2. Packages configuration as JSON
3. Sends via Blues Notehub to client
4. Client receives and saves to LittleFS
5. Client applies new settings
6. Confirmation appears in dashboard

**Status Messages:**

```
✓ Configuration sent successfully
  Waiting for device acknowledgment...
✓ Device confirmed receipt
  Configuration applied

Or:

✗ Error: Could not reach device
  Device may be offline
```

**Verification:**

After sending:
1. Wait 1-2 minutes for confirmation
2. Check serial monitor on client (if connected):
   ```
   Incoming config from server
   Config saved to LittleFS
   Applied new configuration
   ```
3. Next telemetry will reflect new settings

---

## Calibration Dashboard

Access via: `http://<server-ip>/calibration`

### Overview

Calibration maps sensor readings (mA or V) to physical tank heights (inches or cm) for accurate level measurement.

**Calibration Screen:**

```
┌─ Calibration Dashboard ──────────────────────┐
│                                              │
│  Select Client: [North Farm Tank-01  ▼ ]    │
│  Select Tank:   [A - Diesel          ▼ ]    │
│                                              │
│  Current Calibration:                        │
│  ┌────────────┬──────────┬──────────┐        │
│  │ Point │ Sensor │ Height │ Action │        │
│  ├────────────┼──────────┼──────────┤        │
│  │   1   │  4.2 mA │  2.0 in │ [Delete] │    │
│  │   2   │ 12.3 mA │ 48.5 in │ [Delete] │    │
│  │   3   │ 19.8 mA │ 94.0 in │ [Delete] │    │
│  └────────────┴──────────┴──────────┘        │
│                                              │
│  [Export CSV] [Import CSV] [Clear All]       │
│                                              │
│  Add Calibration Point:                      │
│  Sensor Reading: [____] mA                   │
│  Tank Height:    [____] inches               │
│  [Add Point] [Read Current Sensor]           │
│                                              │
│  Visualization:                              │
│  │                                      •    │
│  │                                           │
│  │                            •              │
│  │                                           │
│  │              •                            │
│  │                                           │
│  │        •                                  │
│  └───────────────────────────────────────    │
│   4mA              12mA             20mA     │
└──────────────────────────────────────────────┘
```

### Adding Calibration Points

**Method 1: Manual Entry**

For known reference points:

1. Measure actual tank level with dipstick
2. Note current sensor reading (from dashboard or serial monitor)
3. Enter both values
4. Click **"Add Point"**

**Method 2: Live Sampling**

For active client:

1. Physically set tank to known level
2. Click **"Read Current Sensor"**
   - Requests immediate reading from client
   - Shows: "Current sensor: 12.34 mA"
3. Enter measured height
4. Click **"Add Point"**

**Best Practices:**

- Add at least 3 points (more is better)
- Spread across full tank range:
  - Near empty (10-20%)
  - Mid-range (40-60%)
  - Near full (80-95%)
- Use precise measurements
- Wait for tank to settle (no sloshing)

**Example:**

```
Tank: 96 inches tall

Calibration points:
1. Empty:    4.2 mA →  2.0 in  (minimum)
2. Quarter: 8.5 mA → 24.0 in
3. Half:   12.3 mA → 48.0 in
4. 3/4:    16.1 mA → 72.0 in
5. Full:   19.8 mA → 94.0 in  (maximum)
```

### Managing Calibration

**Export Calibration:**

Click **"Export CSV"**:
```csv
Site,Tank,Sensor_mA,Height_in,Date
North Farm,Diesel,4.2,2.0,2026-01-07
North Farm,Diesel,12.3,48.5,2026-01-07
North Farm,Diesel,19.8,94.0,2026-01-07
```

**Use exported CSV for:**
- Backup/documentation
- Applying to similar tanks
- Compliance records

**Import Calibration:**

1. Click **"Import CSV"**
2. Select CSV file
3. Preview points
4. Click **"Apply"**

**Useful for:**
- Restoring from backup
- Copying to multiple identical tanks
- Bulk calibration updates

**Clear All:**
- Removes all calibration points
- Reverts to factory default (linear 4-20mA = 0-100%)
- Requires confirmation

### Testing Calibration

**Verify Accuracy:**

1. Set tank to known level
2. Check dashboard reading
3. Compare to physical measurement
4. Should match within ±2%

**If inaccurate:**
- Add more calibration points
- Check for sensor drift (recalibrate)
- Verify sensor wiring
- See [Sensor Calibration Guide](SENSOR_CALIBRATION_GUIDE.md)

---

## Relay Control

### Manual Relay Control

**From Tank Card:**

Each client card shows relay buttons:

```
┌─ Client: North Farm Tank-01 ─────────────────┐
│  ...                                         │
│  [Relay R1: OFF] [Relay R2: OFF]             │
│  [Relay R3: OFF] [Relay R4: OFF]             │
└──────────────────────────────────────────────┘
```

**To activate relay:**

1. Click button (e.g., **"Relay R1: OFF"**)
2. Button changes to **"Relay R1: ON"** (green)
3. Command sent to client
4. Relay activates within 30-60 seconds
5. LED on client Opta illuminates

**To deactivate:**

1. Click **"Relay R1: ON"**
2. Changes back to **"Relay R1: OFF"** (gray)
3. Relay deactivates

**Status Indicator Colors:**

| Color | State | Meaning |
|-------|-------|---------|
| Gray | OFF | Relay open (no power to load) |
| Green | ON | Relay closed (powering load) |
| Yellow | PENDING | Command sent, awaiting confirmation |
| Red | FAULT | Relay failure or communication error |

### Automatic Relay Control

Configure in tank settings:

```
☑ Enable Relay Control
  Relay Assignment: [Relay 1 ▼]
  
  ☑ Activate on High Alarm
  ☑ Activate on Low Alarm
  
  Relay Behavior:
    ⦿ Latch (stays on until manually cleared)
    ○ Follow alarm (turns off when alarm clears)
```

**Modes:**

**Latch Mode:**
- Relay turns on when alarm triggers
- Stays on even after alarm clears
- Must manually turn off via dashboard
- **Use for:** Strobes, sirens (want continuous alert)

**Follow Mode:**
- Relay on during alarm
- Automatically off when alarm clears
- Tracks alarm state
- **Use for:** Pump control, automated valves

### Advanced: Cross-Client Relay Forwarding

Control one client's relay from another client's alarm (server-mediated):

**In Client A config:**
```json
{
  "tanks": [{
    "id": "A",
    "relayOnHigh": true,
    "relayMask": 1,  // Relay D0
    "relayTargetClient": "dev:864475044056789"  // Client B
  }]
}
```

**Behavior:**
- Client A's Tank A goes into high alarm
- Client A sends `relay_forward.qo` to server
- Server receives via Route #1, re-dispatches via `command.qo` to Client B
- Client B receives `relay.qi` and activates relay D0
- Enables cascade control across sites

**Example Use Cases:**
- Upstream tank fills → downstream pump starts
- Any tank alarms → master strobe activates
- Multiple tanks alarm → emergency shutdown

See [Relay Control Guide](RELAY_CONTROL_GUIDE.md) for full details.

---

## Historical Data

Access via: `http://<server-ip>/history` (if implemented)

### Viewing Trends

**Time Range Selector:**

```
┌─ Historical Data ────────────────────────────┐
│                                              │
│  Client:    [North Farm Tank-01  ▼]         │
│  Tank:      [A - Diesel          ▼]         │
│  Time Range: ⦿ Last 24 Hours                 │
│              ○ Last 7 Days                   │
│              ○ Last 30 Days                  │
│              ○ Custom Range                  │
│                                              │
│  [Generate Graph]                            │
└──────────────────────────────────────────────┘
```

**Graph Display:**

```
Level (inches)
100 ├─────────────────────────────────────────
 90 │                              ●●●●
 80 │                        ●●●●●●
 70 │                 ●●●●●●●
 60 │          ●●●●●●●
 50 │    ●●●●●●
 40 │●●●●
 30 │
 20 │
 10 │
  0 └─────────────────────────────────────────
    Mon  Tue  Wed  Thu  Fri  Sat  Sun
```

**Features:**
- Hover for exact values
- Zoom in on time range
- Show alarm thresholds (red/yellow lines)
- Overlay multiple tanks
- Export data as CSV

### Data Export

**Download Historical Data:**

1. Select client and tank
2. Choose time range
3. Click **"Export CSV"**

**CSV Format:**

```csv
Timestamp,Client,Tank,Level_in,Sensor_mA,Alarm
2026-01-07 00:00:00,dev:123,A,48.5,12.3,false
2026-01-07 00:30:00,dev:123,A,48.7,12.4,false
2026-01-07 01:00:00,dev:123,A,49.1,12.5,false
...
```

**Uses:**
- Compliance reporting
- Billing (delivery volumes)
- Trend analysis
- Troubleshooting
- Archive for long-term storage

---

## Serial Monitor Dashboard

Access via: `http://<server-ip>/serial-monitor`

### Real-Time Logs

**Combined serial output:**

```
┌─ Serial Monitor ─────────────────────────────┐
│                                              │
│  [Server Logs] [Client Logs]                 │
│                                              │
│  Filter: [_________] ☑ Auto-scroll           │
│                                              │
│  [SERVER] 10:30:00 - Received telemetry      │
│  [SERVER] 10:30:00 - Client dev:123 updated  │
│  [CLIENT dev:123] 10:30:05 - Sampled Tank A  │
│  [CLIENT dev:123] 10:30:05 - Level: 48.5in   │
│  [CLIENT dev:456] 10:30:10 - Config received │
│  [SERVER] 10:30:15 - Alarm check complete    │
│  ...                                         │
│                                              │
│  [Clear] [Export] [Pause]                    │
└──────────────────────────────────────────────┘
```

**Features:**

- **Real-time streaming**: Updates as events occur
- **Filtering**: Show only specific client or keywords
- **Auto-scroll**: Follow latest messages
- **Export**: Download as text file
- **Color coding**: Errors in red, warnings in yellow

**Use Cases:**
- Debugging configuration issues
- Monitoring system health
- Verifying communication
- Troubleshooting connectivity

---

## Mobile Optimization

### Accessing Dashboard on Mobile

**Mobile Browser Support:**
- iOS Safari
- Android Chrome
- Android Firefox
- Mobile Edge

**Responsive Layout:**

**Desktop (Wide Screen):**
```
┌──────────┬──────────┬──────────┬──────────┐
│  Tank 1  │  Tank 2  │  Tank 3  │  Tank 4  │
│          │          │          │          │
└──────────┴──────────┴──────────┴──────────┘
┌──────────┬──────────┬──────────┬──────────┐
│  Tank 5  │  Tank 6  │  Tank 7  │  Tank 8  │
└──────────┴──────────┴──────────┴──────────┘
```

**Mobile (Narrow Screen):**
```
┌──────────────────┐
│     Tank 1       │
│                  │
└──────────────────┘
┌──────────────────┐
│     Tank 2       │
│                  │
└──────────────────┘
┌──────────────────┐
│     Tank 3       │
│                  │
└──────────────────┘
```

**Mobile-Specific Features:**

- **Touch-friendly**: Large buttons for easy tapping
- **Collapsible sections**: Tap to expand details
- **Swipe navigation**: Swipe between tanks
- **Pull to refresh**: Drag down to update data
- **Portrait/landscape**: Adapts to orientation

### Creating Home Screen Shortcut

**iOS (Safari):**

1. Open dashboard in Safari
2. Tap Share icon (box with arrow)
3. Tap **"Add to Home Screen"**
4. Name: "TankAlarm"
5. Tap **"Add"**
6. Icon appears on home screen

**Android (Chrome):**

1. Open dashboard in Chrome
2. Tap menu (three dots)
3. Tap **"Add to Home screen"**
4. Name: "TankAlarm"
5. Tap **"Add"**
6. Confirm prompt

**Behaves like native app:**
- Opens in full screen
- No browser UI
- Fast access
- Custom icon

---

## Dashboard Customization

### Changing Appearance

**Basic Customization (HTML/CSS):**

The dashboard is served as HTML from the server. Advanced users can customize:

**Color Scheme:**

In `dashboard.h` (if implemented) or inline CSS:

```css
/* Default colors */
:root {
  --bg-color: #f0f0f0;
  --card-bg: #ffffff;
  --text-color: #333333;
  --alarm-high: #ff3333;
  --alarm-low: #ff9933;
  --normal: #33ff33;
}

/* Dark mode */
.dark-mode {
  --bg-color: #1a1a1a;
  --card-bg: #2a2a2a;
  --text-color: #e0e0e0;
}
```

**Layout:**

Adjust grid columns for tank cards:

```css
.tank-grid {
  display: grid;
  grid-template-columns: repeat(4, 1fr);  /* 4 columns */
  gap: 20px;
}

@media (max-width: 1200px) {
  .tank-grid {
    grid-template-columns: repeat(3, 1fr);  /* 3 on medium screens */
  }
}

@media (max-width: 768px) {
  .tank-grid {
    grid-template-columns: 1fr;  /* 1 column on mobile */
  }
}
```

**Custom Branding:**

Add company logo:

```html
<div class="header">
  <img src="data:image/png;base64,..." alt="Company Logo">
  <h1>TankAlarm Monitoring System</h1>
</div>
```

**Note:** Dashboard HTML is compiled into firmware. Requires recompiling and uploading new firmware to change.

### Adding Custom Features

**Example: Add "Fill Percentage" Button**

Modify dashboard HTML to include custom button:

```html
<button onclick="showFillPercentages()">Show Fill %</button>

<script>
function showFillPercentages() {
  // Calculate fill percentages
  // Display in modal or overlay
}
</script>
```

**Advanced:** Extend API with new endpoints for custom data.

---

## Troubleshooting Dashboard Issues

### Dashboard Won't Load

**Symptoms:** Blank page, timeout, "Unable to connect"

**Solutions:**
1. Verify server IP address (check serial monitor)
2. Ping server from computer: `ping 192.168.1.150`
3. Check Ethernet cable connected
4. Verify computer on same network
5. Try different browser
6. Check firewall settings

See [Troubleshooting Guide](TROUBLESHOOTING_GUIDE.md) for detailed network diagnostics.

### Data Not Updating

**Symptoms:** Levels show old timestamps, not refreshing

**Solutions:**
1. Check auto-refresh enabled (should update every 30 sec)
2. Verify clients sending telemetry (check serial monitor)
3. Check Blues Notehub routes active
4. Manually refresh: Ctrl+F5 (hard refresh)
5. Check for JavaScript errors (browser console: F12)

### Configuration Not Saving

**Symptoms:** Changes revert after clicking "Send to Device"

**Solutions:**
1. Verify correct admin PIN entered
2. Check client online (green indicator in dropdown)
3. Wait 1-2 minutes for confirmation
4. Check client serial monitor for "Config received" message
5. Verify Blues Notehub route enabled (clients ← server)

### Relay Buttons Not Working

**Symptoms:** Click relay button, nothing happens

**Solutions:**
1. Check client supports relay control (Opta has 4 relays)
2. Verify relay configuration enabled
3. Check admin PIN entered (if required)
4. Verify client online and connected
5. Check serial monitor for relay command received

### Mobile Display Issues

**Symptoms:** Layout broken, buttons too small, scrolling problems

**Solutions:**
1. Ensure using modern browser (Chrome/Safari)
2. Clear browser cache
3. Rotate device (portrait/landscape)
4. Zoom in/out to reset viewport
5. Update browser to latest version

---

## Best Practices

### Dashboard Usage

**DO:**
- ✅ Bookmark dashboard URL for quick access
- ✅ Create home screen shortcut on mobile
- ✅ Set strong admin PIN (not 0000 or 1234)
- ✅ Monitor alarm indicators regularly
- ✅ Export historical data for records
- ✅ Test relay controls before production use
- ✅ Verify configuration changes in serial monitor

**DON'T:**
- ❌ Share admin PIN with unauthorized users
- ❌ Leave browser logged in on public computers
- ❌ Ignore stale data warnings (gray cards)
- ❌ Make configuration changes without backups
- ❌ Control relays connected to critical equipment without testing
- ❌ Expose dashboard to public internet without VPN

### Security Considerations

**Admin PIN:**
- Change default PIN immediately
- Use 4-6 digit PIN (not sequential like 1234)
- Different PIN for each deployment
- Don't write PIN on equipment
- Limit who knows PIN

**Network Access:**
- Dashboard only accessible on local network by default
- To access remotely:
  - **Option 1:** VPN (recommended)
  - **Option 2:** Port forwarding (less secure, requires HTTPS)
  - **Option 3:** Blues Notehub proxy route (if implemented)

**HTTPS:**
- Default dashboard is HTTP (unencrypted)
- For production with remote access, add HTTPS
- Requires SSL certificate and modified firmware

---

## API Reference

### GET Endpoints

**`GET /`**
- Returns main dashboard HTML
- No authentication required
- Includes all CSS/JavaScript inline

**`GET /api/tanks`**
- Returns current tank levels (JSON)
- Example response:
  ```json
  {
    "tanks": [
      {
        "client": "dev:864475044012345",
        "site": "North Farm",
        "sensorIndex": "A",
        "level": 48.5,
        "sensor": 12.3,
        "alarm": false,
        "timestamp": "2026-01-07T10:30:00Z"
      }
    ]
  }
  ```

**`GET /api/clients`**
- Returns list of all known clients
- Includes last seen, firmware version, configuration summary

**`GET /api/charger?client=<device UID>`**
- Returns the client's last charger check (JSON): `c` (the device UID), `site` and `fv` (firmware version) when the server knows them, and `have` (false when no check has arrived: then no member of a check follows, though `program` may)
- With a check: `e` (its time in Unix seconds, 0 when not known), `vd` and `verdict` (the verdict: 0 `not judged`, 1 `ok`, 2 `mismatch`, 3 `cannot tell`), `battery` (the chemistry the settings were judged against: `LiFePO4`, `Li-ion`, `sealed lead-acid`, `flooded lead-acid` or `not judged`), `system` (12 or 24 as measured, 0 when not known), `settingsScale` and `liveScale` (one line each on how the stored voltages and the live readings are encoded), `findings` (text `t`, and `p` true when it makes the settings wrong for the battery) and `groups`: one object per group of registers, `{"title": ..., "rows": [[name, description, value, raw], ...]}`, with `value` "no answer" and `raw` empty for a register that did not answer
- `now` is added when the battery in the config the server holds for the client is judged as another chemistry than `battery` (named the same way): the check is pending, as on the badge
- `program` is there once the client was asked to program its charger, or has sent a result or a look. For the newest request sent: `asked` (its id), `askedAt` (the server's time when it was asked for), `answered` (true once the client has sent a result for it), `fresh` (true while it is under a day old, so that a client could still act on it, and false while the server's clock is not set), `loadOk` (it allowed current from the LOAD terminals during the run) and `values` as `[label, value]` (what it asked for), all left out when no request was sent. `result` is the last result of a request: `req` (the request it answers), `at` (when that was asked for, while it is the newest request sent), `e` (when the client made it, 0 when not known), `o` and `outcome`, `good`, `text`, `summary`, `followUp` (true when the client looks at the charger again and sends another result), `restart` (left out when no restart command was sent), `battery` and `load` (left out when they were not read), `reasons`, `notes`, `asked` (the values of the request as `[label, value]`, left out when the result holds none) and `registers` as `[name, description, before, wanted, how it went]`. `look` is the last look at the charger that answers no request (one the client made when it could not tell from its flash what it owed): `e`, and the members of a result from `o` on
- Answers 500 when the server runs out of memory for it, so that a verdict is never shown without its findings or a result as still to come, nor a verdict as current when the config could not be read
- Needs a session, like every `/api/` route

### POST Endpoints

**`POST /api/config`**
- Send configuration to client
- Requires admin PIN
- `config.solarCharger.program` holds the charger values (see [Charger Programming](#charger-programming-sunsaver-mppt)). With `"apply": true` in it the server makes them a request: it adds the request id `req` and leaves `apply` out of the config it sends. It answers 400 when the values are outside the limits, or the config is not for a 12 V LiFePO4 battery read over RS-485, and 503 when its clock is not set; the config is then not saved or sent
- Request body:
  ```json
  {
    "pin": "1234",
    "client": "dev:864475044012345",
    "config": {
      "sampleSeconds": 1800,
      "tanks": [...]
    }
  }
  ```

**`POST /api/relay`**
- Activate/deactivate relay
- Requires admin PIN
- Request body:
  ```json
  {
    "pin": "1234",
    "client": "dev:864475044012345",
    "relay": 1,  // 1-4
    "state": "on"  // "on" or "off"
  }
  ```

**`POST /api/refresh`**
- Request immediate telemetry from client
- Triggers out-of-band sample
- Useful for testing

See server README for complete API documentation.

---

## Resources

### Related Guides

- [Quick Start Guide](QUICK_START_GUIDE.md) - Initial setup
- [Server Installation Guide](SERVER_INSTALLATION_GUIDE.md) - Server deployment
- [Client Installation Guide](CLIENT_INSTALLATION_GUIDE.md) - Client setup
- [Sensor Calibration Guide](SENSOR_CALIBRATION_GUIDE.md) - Calibration procedures
- [Relay Control Guide](RELAY_CONTROL_GUIDE.md) - Automation setup
- [Troubleshooting Guide](TROUBLESHOOTING_GUIDE.md) - Diagnostic procedures

### Tools

- [Modern web browser](https://www.google.com/chrome/) (Chrome recommended)
- [Network scanner](https://www.advanced-ip-scanner.com/) (Find server IP)
- Browser dev tools (F12) - JavaScript debugging

---

*Dashboard Guide v1.1 | Last Updated: February 20, 2026*  
*Compatible with TankAlarm Server Firmware 1.1.2+*
