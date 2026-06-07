# TankAlarm Client Installation Guide

**Setting Up Your TankAlarm Field Monitoring Device**

---

## Introduction

The TankAlarm 112025 Client is a field-deployable tank and pump monitoring device built on the Arduino Opta platform with cellular connectivity via the Blues Notecard. This guide will walk you through the complete installation process, from setting up your development environment to deploying devices in the field.

### What You'll Build

The TankAlarm Client monitors up to 8 tanks or pumps at remote locations and transmits data to a central server via cellular connection. It features:

- **Multi-Tank Monitoring** - Track up to 8 tanks per device
- **Automatic Alarms** - High/low level alerts via SMS
- **Daily Reports** - Scheduled status updates
- **Cellular Connectivity** - No WiFi required via Blues Notecard
- **Low Power Design** - Efficient sampling and transmission
- **Remote Configuration** - Update settings via server dashboard

### Required Materials

To complete this installation, you'll need:

**Hardware:**
- [Arduino Opta Lite](https://store-usa.arduino.cc/products/opta-lite) - Industrial controller platform
- [Blues Wireless for Opta](https://shop.blues.com/collections/accessories/products/wireless-for-opta) - Cellular module
- [Arduino Opta Ext A0602](https://store-usa.arduino.cc/products/opta-ext-a0602) - Analog expansion for sensors
- USB-C cable for programming
- 12-24V DC power supply for field deployment
- Compatible analog sensors (ultrasonic, pressure, float switches)

**Software:**
- Arduino IDE 2.0+ (or 1.8.19+)
- Active Blues Notehub account ([notehub.io](https://notehub.io))
- This repository's source code

### Suggested Reading

Before starting, familiarize yourself with these concepts:

- [What is Arduino?](https://learn.sparkfun.com/tutorials/what-is-an-arduino) - Arduino basics
- [I2C Communication](https://learn.sparkfun.com/tutorials/i2c) - Understanding the protocol
- [Blues Notecard Quickstart](https://dev.blues.io/quickstart/) - Cellular IoT basics
- [Arduino Opta Overview](https://docs.arduino.cc/hardware/opta) - Platform documentation

---

## Hardware Overview

### Arduino Opta Platform

The Arduino Opta is an industrial-grade Micro PLC with:

- **Processor**: STM32H747XI dual-core (Cortex-M7 @ 480 MHz + Cortex-M4 @ 240 MHz)
- **Memory**: 2 MB Flash, 1 MB RAM
- **I/O**: 8 digital inputs, 4 relay outputs, 4 analog inputs (expandable)
- **Connectivity**: Ethernet, USB-C, I2C, RS-485
- **Operating Voltage**: 12-24V DC industrial power
- **Temperature**: -20°C to +50°C operating range

![Arduino Opta](https://docs.arduino.cc/static/opta-pinout.png)

### Blues Notecard Cellular Module

The Blues Wireless for Opta provides:

- **Cellular**: LTE-M and NB-IoT global coverage
- **Prepaid Data**: 500 MB for 10 years (included)
- **No Monthly Fees**: Pay only for what you use
- **Low Power**: Deep sleep modes for battery operation
- **Secure**: TLS encryption, device authentication

### Analog Expansion Module

The Opta Ext A0602 adds:

- **6 Analog Inputs**: 0-10V or 4-20mA current loop
- **12-bit Resolution**: Precise measurements
- **Daisy-Chain**: Stack multiple expansions
- **Industrial Isolation**: Protects against electrical noise

### Pin Assignments

The TankAlarm Client uses these connections:

| Function | Pin/Port | Notes |
|----------|----------|-------|
| Notecard I2C | SDA/SCL | Default I2C bus |
| Analog In 0-3 | Built-in Opta | First 4 tanks |
| Analog In 4-9 | Ext A0602 | Additional tanks (requires expansion) |
| Digital In 0-7 | Built-in Opta | Float switches, relay feedback |
| Relay Out 0-3 | Built-in Opta | Pump control (optional) |

---

## Step 1: Install Arduino IDE

### Download and Install

1. Visit [arduino.cc/software](https://www.arduino.cc/en/software)
2. Download Arduino IDE for your operating system:
   - **Windows**: Windows 10 or later (Installer .exe)
   - **macOS**: macOS 10.14 Mojave or later (.dmg)
   - **Linux**: AppImage or ZIP archive
3. Run the installer and follow prompts
4. Launch Arduino IDE after installation

### First Launch Setup

When Arduino IDE starts for the first time:

1. It may prompt for permissions (Grant them)
2. Select "Allow" for automatic updates
3. Choose your preferred language (English recommended)

**Arduino IDE 2.0 Advantages:**
- Built-in serial plotter
- Auto-complete code suggestions
- Integrated library manager
- Faster compilation
- Better error messages

---

## Step 2: Install Board Support

### Add Arduino Opta to IDE

The Arduino Opta uses the Arduino Mbed OS core:

1. Open Arduino IDE
2. Navigate to **Tools → Board → Boards Manager**
3. In the search field, type: `Arduino Mbed OS Opta Boards`
4. Find **Arduino Mbed OS Opta Boards** by Arduino
5. Click the **Install** button
6. Wait for installation (may take 5-10 minutes - downloads ~400 MB)
7. Installation complete when you see "INSTALLED" badge

![Boards Manager](https://docs.arduino.cc/static/boards-manager.png)

### Verify Installation

To confirm the board installed correctly:

1. Go to **Tools → Board**
2. Expand **Arduino Mbed OS Opta Boards**
3. You should see **Arduino Opta** in the list

**Troubleshooting Tip**: If installation fails, check:
- Internet connection is stable
- Antivirus isn't blocking downloads
- Free disk space (need ~1 GB available)
- Try restarting Arduino IDE and retrying

---

## Step 3: Install Required Libraries

### Using the Library Manager

The TankAlarm Client requires these libraries:

1. Open **Tools → Manage Libraries** (or press Ctrl+Shift+I)
2. For each library below, search and install:

#### Required Libraries Table

| Library | Version | Purpose | Install Command |
|---------|---------|---------|-----------------|
| **ArduinoJson** | 7.0.0+ | Configuration & data serialization | Search "ArduinoJson" → Install |
| **Blues Wireless Notecard** | Latest | Cellular communication | Search "Notecard" → Install |
| **ArduinoRS485** | Latest | RS-485 hardware interface | Search "ArduinoRS485" → Install |
| **ArduinoModbus** | Latest | Modbus RTU protocol (solar monitoring) | Search "ArduinoModbus" → Install |
| **TankAlarm-112025-Common** | This repo | Shared constants/config (`TankAlarm_Common.h`) | Install from ZIP (recommended) |
| **LittleFS** | Built-in | File system (included with Mbed core) | No install needed |
| **Wire** | Built-in | I2C communication | No install needed |
| **IWatchdog** | Built-in | Watchdog timer (Mbed core) | No install needed |

### Step-by-Step Library Installation

#### 1. Install ArduinoJson

1. In Library Manager, type: `ArduinoJson`
2. Find **ArduinoJson by Benoit Blanchon**
3. **Important**: Install version **7.0.0 or later**
   - Version 7 has breaking changes from v6
   - Ensure dropdown shows 7.x
4. Click **Install**
5. Wait for "INSTALLED" confirmation

**Why ArduinoJson?** Handles JSON configuration files and data serialization with minimal memory footprint.

#### 2. Install Blues Wireless Notecard Library

1. In Library Manager, type: `Notecard`
2. Find **Blues Wireless Notecard by Blues Inc.**
3. Click **Install** (latest version)
4. Wait for installation

**Why Notecard Library?** Provides high-level API for cellular communication without AT commands.

#### 3. Install ArduinoRS485

1. In Library Manager, type: `ArduinoRS485`
2. Find **ArduinoRS485 by Arduino**
3. Click **Install** (latest version)
4. Wait for installation to complete

**Why ArduinoRS485?** Provides the low-level RS-485 hardware interface needed for Modbus communication with solar charge controllers.

#### 4. Install ArduinoModbus

1. In Library Manager, type: `ArduinoModbus`
2. Find **ArduinoModbus by Arduino**
3. Click **Install** (latest version)
4. Wait for installation to complete

**Why ArduinoModbus?** Implements the Modbus RTU protocol for reading solar charge controller data (battery voltage, charging current, etc.).

#### 5. Install TankAlarm-112025-Common (Custom Library)

TankAlarm uses a shared custom library included in this repository. Your sketches include it with:

```cpp
#include <TankAlarm_Common.h>
```

**Recommended: Install from ZIP**

1. Download `TankAlarm-112025-Common.zip` from the repository root
2. In Arduino IDE, go to **Sketch → Include Library → Add .ZIP Library...**
3. Select `TankAlarm-112025-Common.zip`
4. Restart Arduino IDE (recommended)

**Alternative: Install from Folder**

Copy the entire `TankAlarm-112025-Common/` folder to:

- Windows: `%USERPROFILE%\Documents\Arduino\libraries\`
- macOS: `~/Documents/Arduino/libraries/`
- Linux: `~/Arduino/libraries/`

#### 6. Verify Built-In Libraries

These come with the Arduino Mbed OS core:

- **LittleFS**: Flash file system for configuration storage
- **Wire**: I2C protocol implementation
- **IWatchdog**: Hardware watchdog for reliability

No installation needed - they're already available!

### Verify All Libraries Installed

1. Go to **Sketch → Include Library**
2. Confirm you see:
   - ArduinoJson
   - ArduinoModbus
   - ArduinoRS485
   - Notecard
   - TankAlarm-112025-Common
3. Go to **File → Examples**
4. You should see example sketches for both libraries

---

## Step 4: Set Up Blues Notehub

Before uploading firmware, configure your Blues Notehub account:

### Create Notehub Account

1. Visit [notehub.io](https://notehub.io)
2. Click **Sign Up** (free account available)
3. Verify your email address
4. Log in to Notehub

### Create a Product

1. Click **Create Product**
2. Fill in details:
   - **Product Name**: `TankAlarm Fleet`
   - **Description**: `Tank and pump monitoring system`
   - **Product Type**: `Device Fleet`
3. Click **Create**
4. **Note the Product UID** (format: `com.company.product:project`)

Example Product UID: `com.senax.tankalarm:production`

### Create Fleets

Fleets organize devices for routing:

1. In your product, go to **Fleets** tab
2. Create **three fleets**:

**Fleet 1: Clients**
- Name: `tankalarm-clients`
- Description: `Field monitoring devices`

**Fleet 2: Server**
- Name: `tankalarm-server`  
- Description: `Central data aggregation server`

**Fleet 3: Viewer**
- Name: `tankalarm-viewer`
- Description: `Read-only kiosk display devices`

### Provision Your Notecard

Your Blues Notecard needs to be associated with your product:

1. Connect Notecard to computer via USB (if using Notecarrier)
2. Visit [notehub.io](https://notehub.io)
3. Go to **Devices → Claim Device**
4. Follow the provisioning wizard
5. Assign device to `tankalarm-clients` fleet

**Alternative**: Notecards can self-provision on first connection if Product UID is set in firmware.

---

## Step 5: Configure and Upload Firmware

### Open the Client Sketch

1. Download this repository (or clone via Git)
2. Navigate to `TankAlarm-112025-Client-BluesOpta/`
3. Double-click `TankAlarm-112025-Client-BluesOpta.ino`
4. Arduino IDE opens with all project files loaded

### Update Product UID

Before compiling, configure your Product UID so the client can communicate with the server via Notehub. You have two options:

**Option A: Create `ClientConfig.h`** (recommended):

1. In the `TankAlarm-112025-Client-BluesOpta/` folder, copy `ClientConfig.h.example` to `ClientConfig.h`
2. Open `ClientConfig.h` and set your Product UID:
   ```cpp
   #define DEFAULT_PRODUCT_UID "com.your-company.your-product:your-project"
   ```
3. Save the file (Ctrl+S)

**Option B: Push config from server** (no recompile needed):

1. Flash the client with defaults (Product UID left empty)
2. On the server dashboard, go to the **Config Generator** page
3. The Product UID is auto-filled from server settings
4. Generate and push the config to your client

> ⚠️ **Critical**: The Product UID must be **exactly the same** on the client and server devices. If they don't match, the client will not be able to send telemetry to the server, and the server won't be able to push configuration updates to the client.
>
> **Tip**: You can verify the server's Product UID in the web dashboard under **Server Settings** → **Blues Notehub**. When using the **Config Generator** page on the server, the Product UID is automatically filled in from the server settings to ensure consistency.

> 📂 **Git Note**: `ClientConfig.h` is listed in `.gitignore` — your credentials stay out of version control. Only the `.example` template is committed.

### Select Board and Port

1. Connect Arduino Opta via USB-C cable
2. Go to **Tools → Board → Arduino Mbed OS Opta Boards → Arduino Opta**
3. Go to **Tools → Port**
4. Select the port showing `Arduino Opta` (e.g., COM3, /dev/ttyACM0)

### Verify/Compile Firmware

Test compilation before uploading:

1. Click the **Verify** button (✓ checkmark icon)
2. Wait for compilation (first compile takes 2-3 minutes)
3. Check bottom console for results

**Expected Output:**
```
Sketch uses 487320 bytes (62%) of program storage space. Maximum is 786432 bytes.
Global variables use 143256 bytes (27%) of dynamic memory.
Done compiling
```

**Memory Usage Guidelines:**
- **Flash**: 60-80% is normal (Opta has 2MB)
- **RAM**: 30-50% is typical (Opta has 1MB)
- **Warning**: If >90%, optimize code or reduce features

### Upload to Arduino Opta

1. Ensure Opta is connected via USB
2. Click the **Upload** button (→ right arrow icon)
3. Watch the progress bar
4. First upload takes longer (~1-2 minutes)
5. Success message: `Upload complete`

**Upload Process:**
1. Compiles firmware
2. Generates binary file
3. Resets Arduino Opta
4. Uploads via bootloader
5. Restarts with new firmware

---

## Step 6: Verify Operation

### Open Serial Monitor

1. Go to **Tools → Serial Monitor** (or press Ctrl+Shift+M)
2. Set baud rate to **115200** (dropdown at bottom)
3. Press **Reset** button on Arduino Opta
4. Watch for startup messages

### Expected Serial Output

```
TankAlarm 112025 Client - Blues Opta v1.1.2 (Feb 20 2026)
Initializing...
LittleFS initialized (524288 bytes free)
Loading configuration from /client_config.json
Configuration loaded successfully
Notecard initialization...
Notecard UID: dev:864475044123456
Product UID: com.senax.tankalarm:production
Assigned to fleet: tankalarm-clients
Hardware: Arduino Opta + Ext A0602 (10 analog channels)
Configured tanks: 3
  Tank A - Primary Tank (analog pin 0)
  Tank B - Secondary Tank (analog pin 1)  
  Tank C - Reserve Tank (analog pin 2)
Sample interval: 1800 seconds (30 minutes)
Starting main loop...
```

### Initial Configuration

On first boot, default configuration is created:

```json
{
  "site": "Site 1",
  "deviceLabel": "Client-01",
  "serverFleet": "tankalarm-server",
  "sampleSeconds": 1800,
  "tanks": [
    {
      "id": "A",
      "name": "Tank A",
      "number": 1,
      "sensor": "analog",
      "primaryPin": 0,
      "heightInches": 120.0,
      "highAlarm": 100.0,
      "lowAlarm": 20.0,
      "reportThreshold": 0
    }
  ]
}
```

**Configuration is stored in LittleFS** - survives reboots and power loss.

### Monitor First Telemetry

Watch serial monitor for first data transmission:

```
[12:30:45] Reading sensors...
  Tank A: 87.3 inches (72.8%)
  Tank B: 45.1 inches (37.6%)
  Tank C: 112.5 inches (93.8%)
[12:30:46] Sending telemetry to telemetry.qo
[12:30:48] Telemetry sent successfully
Next sample in 1800 seconds
```

> **Note:** Telemetry is sent via `telemetry.qo` (outbound queue). Notehub Routes
> handle delivery to the server device. See
> [NOTEHUB_ROUTES_SETUP.md](NOTEHUB_ROUTES_SETUP.md) for Route configuration.

---

## Step 7: Configure via Server

Once the server is running, configure clients remotely:

### Access Server Dashboard

1. Find server IP address from server's serial output or router
2. Open web browser
3. Navigate to `http://<server-ip>/`

### Update Client Configuration

1. Scroll to **"Update Client Config"** section
2. Select your client from dropdown (auto-populated from telemetry)
3. Update settings:
   - Site name and device label
   - Sample interval (seconds)
   - Level change threshold (inches for event-based reporting)
   - Tank configurations (up to 8 tanks)
4. Click **"Send Config to Client"**
5. Configuration pushed via Notehub
6. Client applies and saves config automatically

### Configuration Options

| Setting | Purpose | Typical Value |
|---------|---------|---------------|
| **Site Name** | Location identifier | "North Tank Farm" |
| **Device Label** | Short device name | "Tank-01" |
| **Server Fleet** | Target for data | "tankalarm-server" |
| **Product UID** | Fleet-wide identifier | Set in TankAlarm_Config.h |
| **Sample Interval** | Seconds between readings | 1800 (30 min) |
| **Report Time** | Daily report hour & minute | 05:00 (5 AM) |
| **Daily Email** | Report recipient | admin@company.com |
| **Power Source** | Power configuration | grid / solar / solar_mppt |

### Sensor Configuration

Each sensor (up to 8 on Analog Expansion) supports multiple monitor types:

#### Monitor Types
- **Tank Level** - Liquid level monitoring in tanks
- **Gas Pressure** - Propane/natural gas pressure monitoring
- **RPM Sensor** - Engine speed monitoring via Hall effect sensor

#### Sensor Types
- **Digital (Float Switch)** - Simple on/off switch
  - Modes: Normally-Open (NO) or Normally-Closed (NC)
  - Use Opta digital inputs (I1-I8)
- **Analog (0-10V)** - Voltage-based sensors
  - Use Opta analog inputs (A0-A7) or Expansion channels
- **4-20mA Current Loop** - Industrial sensors (most common)
  - Requires Analog Expansion (CH0-CH7)
  - Two modes: Pressure (bottom-mount) or Ultrasonic (top-mount)
- **Hall Effect RPM** - Engine speed detection
  - Pulses per revolution: 1-255

#### Per-Sensor Settings
- **Tank/Engine Number**: Numeric identifier (1-8)
- **Name**: Descriptive label ("Main Diesel Tank")
- **Contents**: Tank contents type (Diesel, Water, Propane)
- **Pin/Channel**: Hardware connection point
- **Max Height/Pressure/RPM**: Full-scale measurement value
- **Level Change Threshold**: Inches change for event reporting (tank sensors only)

#### Alarm Configuration (Optional)
Each sensor can have independent alarms:
- **High Alarm**: Upper threshold (inches/PSI/RPM)
- **Low Alarm**: Lower threshold (inches/PSI/RPM)
- **Digital Trigger**: "Activated" or "Not Activated" state

#### Relay Control (Optional)
Trigger relay outputs on alarm conditions:
- **Target Client UID**: Remote client with relay outputs
- **Trigger On**: Any/High/Low alarm
- **Relay Mode**: Momentary / Until Clear / Manual Reset
- **Relay Outputs**: Select R1-R4 outputs to activate

#### SMS Alerts (Optional)
Send text messages on alarm conditions:
- **Phone Numbers**: Comma-separated list (+15551234567)
- **Trigger On**: Any/High/Low alarm
- **Custom Message**: Optional alert text

### Physical Input Configuration

Configure physical buttons for manual control:
- **Clear All Relays**: Button to reset all active relay alarms
- **Pin Number**: Hardware input pin (0-99)
- **Input Mode**: Active LOW (pullup) or Active HIGH
- **Action**: clear_relays / none (future actions coming)

💡 **Example Use Case:** Emergency stop button to clear all relay-activated alarms across the fleet

---

## Troubleshooting

### Compilation Errors

#### Error: `ArduinoJson.h: No such file or directory`

**Cause**: ArduinoJson library not installed

**Solution:**
1. Open **Tools → Manage Libraries**
2. Search `ArduinoJson`
3. Install version 7.0.0 or later
4. Retry compilation

#### Error: `Notecard.h: No such file or directory`

**Cause**: Blues Notecard library missing

**Solution:**
1. Open **Tools → Manage Libraries**
2. Search `Notecard`
3. Install **Blues Wireless Notecard**
4. Restart Arduino IDE
5. Retry compilation

#### Error: `LittleFS.h: No such file or directory`

**Cause**: Arduino Mbed OS Opta Boards not installed

**Solution:**
1. Go to **Tools → Board → Boards Manager**
2. Search `Arduino Mbed OS Opta Boards`
3. Install the core package
4. Restart Arduino IDE
5. Select **Tools → Board → Arduino Opta**
6. Retry compilation

#### Error: `Sketch too big`

**Cause**: Wrong board selected or optimization issue

**Solution:**
1. Verify board: **Tools → Board → Arduino Opta**
2. Check compilation settings
3. Ensure no debug flags enabled
4. Arduino Opta has 2MB flash - sketch should fit

### Upload Errors

#### Error: `Port not found`

**Causes & Solutions:**

1. **Cable not connected**: Plug USB-C cable firmly into both ends
2. **Charge-only cable**: Use data-capable USB cable
3. **Driver issue (Windows)**: Install CH340 drivers if needed
4. **Permission (Linux)**: Add user to `dialout` group:
   ```bash
   sudo usermod -a -G dialout $USER
   # Log out and back in
   ```

#### Error: `Upload timeout`

**Solution:**
1. Press and hold **Reset** button on Opta
2. Click **Upload** in Arduino IDE
3. Release **Reset** when "Uploading..." appears
4. Bootloader activates and accepts upload

#### Error: `Verification failed`

**Cause**: Corrupted upload or interference

**Solution:**
1. Try a different USB cable
2. Connect directly to computer (not via hub)
3. Close other programs using serial ports
4. Retry upload

### Runtime Errors

#### Serial Output: `LittleFS init failed; halting`

**Cause**: Internal flash corruption or unformatted

**Solution:**
1. Re-upload firmware (formatting occurs automatically)
2. If persistent:
   ```cpp
   // Add to setup() before LittleFS.begin():
   LittleFS.format();
   ```
3. Upload, let it format, then remove format line
4. Upload again with normal code

#### Serial Output: `Failed to initialize Notecard`

**Causes & Solutions:**

1. **Notecard not seated**: Firmly press Blues module into socket
2. **I2C connection**: Check SDA/SCL pins
3. **Wrong I2C address**: Default is 0x17 (check code)
4. **Notecard firmware**: Update via Blues CLI if old version

**Verification Command:**
```cpp
{"req":"card.version"}
```

Expected response shows firmware version.

#### No Serial Output

**Causes & Solutions:**

1. **Wrong baud rate**: Set to **115200** in Serial Monitor
2. **Wrong port selected**: Re-select port in **Tools → Port**
3. **Upload failed**: Try uploading again
4. **USB cable**: Try different cable or port
5. **Press Reset**: Click reset button to restart

#### Configuration Not Loading

**Symptoms**: Default config persists after server update

**Causes:**
1. **Wrong device UID**: Verify UID matches in server dropdown
2. **Fleet mismatch**: Check `serverFleet` matches actual fleet name
3. **Notehub sync delay**: Wait 1-2 minutes for propagation
4. **Notecard offline**: Check cellular connection

**Verification:**
Check server's serial console for "Config sent via command.qo" message.
A Notehub Route relays this to the target client (see [NOTEHUB_ROUTES_SETUP.md](NOTEHUB_ROUTES_SETUP.md)).

### Cellular Connectivity

#### Notecard Won't Connect

**Debug Steps:**

1. **Check SIM activation**:
   - Log in to Notehub
   - Verify Notecard shows "Active" status
   - Check data usage isn't exhausted

2. **Check signal strength**:
   ```cpp
   {"req":"card.wireless"}
   ```
   Response includes `rssi` (signal strength):
   - `-70 to -85 dBm`: Good signal
   - `-86 to -100 dBm`: Weak signal
   - `-101 to -113 dBm`: Very weak (connectivity issues likely)

3. **Force reconnect**:
   ```cpp
   {"req":"card.restart"}
   ```

4. **Check antenna**: Ensure cellular antenna is attached to Notecard

#### Slow Data Sync

**Causes:**
- Weak cellular signal
- High network latency
- Notecard in power-saving mode

**Solutions:**
1. Relocate device for better signal
2. Use continuous mode (faster but more power):
   ```cpp
   {"req":"hub.set", "mode":"continuous"}
   ```
3. Check network congestion in area

---

## Advanced Configuration

### Custom Sensor Types

The client supports multiple sensor types:

#### Analog Voltage (0-10V)

```json
{
  "sensor": "analog",
  "primaryPin": 0,
  "heightInches": 120.0
}
```

Linear scaling from 0V (empty) to 10V (full).

#### Current Loop (4-20mA)

```json
{
  "sensor": "current_loop",
  "primaryPin": 4,
  "loopChannel": 0,
  "heightInches": 144.0
}
```

Requires Opta Ext A0602 configured for current input.

#### Float Switch (Digital)

```json
{
  "sensor": "float",
  "primaryPin": 0,
  "secondaryPin": 1
}
```

Two-level monitoring (high/low switches).

### Adjusting Sample Intervals

**Via Server (Recommended):**
- Update in web interface
- Change takes effect on next sync

**Via Serial Console:**
```cpp
// Query current config (delivered by Notehub Route)
{"req":"note.get", "file":"config.qi"}

// Direct config edit (advanced)
// Edit /client_config.json via LittleFS
```

**Recommended Intervals:**
- **Slow-changing tanks**: 3600s (1 hour)
- **Normal monitoring**: 1800s (30 minutes)  
- **Critical/fast-changing**: 600s (10 minutes)
- **Alarm mode**: 300s (5 minutes)

### Enabling Event-Based Reporting

Set a per-sensor `reportThreshold` to trigger reports on significant changes, in that sensor's own unit (inches, PSI, RPM, GPM):

```json
{
  "sensors": [
    { "id": "A", "reportThreshold": 6 }
  ]
}
```

Sends telemetry immediately if that sensor's reading changes by 6+ units (e.g. 6 inches) since the last report. `0` disables change-based telemetry for the sensor.

**Benefits:**
- Faster alarm response
- Detect leaks/fills quickly
- Reduce routine transmissions (saves data)

**Considerations:**
- Increases cellular usage during fills
- May trigger on sensor noise (set threshold appropriately)

### Watchdog Timer

The client uses hardware watchdog for reliability:

```cpp
#define WATCHDOG_TIMEOUT_SECONDS 600  // 10 minutes
```

If main loop hangs, Opta automatically resets.

**Adjusting Timeout:**
- Increase for slow operations
- Decrease for faster recovery
- Minimum: 60 seconds
- Maximum: 3600 seconds (1 hour)

---

## Field Deployment

### Installation Checklist

- [ ] **Power**: 12-24V DC supply connected and tested
- [ ] **Sensors**: Analog sensors wired and calibrated
- [ ] **Antenna**: Cellular antenna attached to Notecard
- [ ] **Notecard**: Activated in Notehub and assigned to fleet
- [ ] **Configuration**: Updated via server dashboard
- [ ] **Testing**: Verified telemetry reaching server
- [ ] **Enclosure**: Weatherproof if outdoor installation
- [ ] **Grounding**: Proper electrical grounding for safety
- [ ] **Documentation**: Location and tank IDs recorded

### Wiring Guidelines

**Power:**
- Use 18-22 AWG wire for 12-24V DC
- Polarity: Red (+), Black (-)
- Fuse: 2A recommended
- Wire runs: <50 feet for 22 AWG

**Sensors:**
- Shield analog signal wires if runs >10 feet
- Twisted pair for differential signals
- Avoid routing near high voltage wires
- Use DIN rail mounting for industrial environments

**Grounding:**
- Connect Opta ground to earth ground
- Use star grounding topology
- Isolate digital and analog grounds

### Environmental Considerations

**Operating Conditions:**
- Temperature: -20°C to +50°C
- Humidity: 10-90% RH (non-condensing)
- Altitude: Up to 2000m

**Protection:**
- IP20 enclosure minimum
- IP65 for outdoor/dusty environments
- Avoid direct sunlight on Opta
- Ventilation for heat dissipation

---

## Resources and Going Further

### Hardware Documentation

- [Arduino Opta Datasheet](https://docs.arduino.cc/resources/datasheets/ABX00064-datasheet.pdf)
- [Arduino Opta Getting Started](https://docs.arduino.cc/tutorials/opta/getting-started)
- [Opta Ext A0602 Guide](https://docs.arduino.cc/hardware/opta-ext)
- [Blues Notecard Datasheet](https://dev.blues.io/datasheets/notecard-datasheet/)
- [Blues for Opta Guide](https://dev.blues.io/hardware/opta-notecarrier/)

### Software Documentation

- [ArduinoJson Guide](https://arduinojson.org/v7/doc/)
- [Blues Notecard API](https://dev.blues.io/api-reference/notecard-api/introduction/)
- [Arduino Mbed OS API](https://os.mbed.com/docs/mbed-os/v6.16/apis/index.html)
- [LittleFS Documentation](https://github.com/littlefs-project/littlefs)

### Project Documentation

- [TankAlarm Server Installation](../../TankAlarm-112025-Server-BluesOpta/INSTALLATION.md)
- [Fleet Setup Guide](../../TankAlarm-112025-Server-BluesOpta/FLEET_SETUP.md)
- [Firmware Update Guide](../FIRMWARE_UPDATE_GUIDE.md)
- [Relay Control Guide](RELAY_CONTROL_GUIDE.md)
- [Migration from 092025](MIGRATION_GUIDE.md)

### Getting Help

**Technical Support:**
- **Arduino**: [Arduino Forum - Opta](https://forum.arduino.cc/c/hardware/opta/181)
- **Blues**: [Blues Community Forum](https://community.blues.io)
- **GitHub**: [Project Issues](https://github.com/SenaxInc/SenaxTankAlarm/issues)

**Useful Commands:**

Check Notecard status:
```cpp
{"req":"card.wireless"}
{"req":"hub.sync.status"}  
{"req":"card.version"}
```

Force immediate sync:
```cpp
{"req":"hub.sync"}
```

View configuration (delivered by Notehub Route):
```cpp
{"req":"note.get", "file":"config.qi"}
```

---

## Next Steps

After successful installation:

1. **[Install Server](../../TankAlarm-112025-Server-BluesOpta/INSTALLATION.md)** - Set up central aggregation
2. **[Configure Fleet](../../TankAlarm-112025-Server-BluesOpta/FLEET_SETUP.md)** - Organize devices in Notehub
3. **[Connect Sensors](SENSOR_CALIBRATION_GUIDE.md)** - Wire and calibrate tank sensors
4. **[Test Alarms](README.md)** - Verify high/low alerts work
5. **[Schedule Reports](README.md)** - Set up daily email summaries
6. **[Deploy to Field](README.md)** - Install at remote sites

---

*Installation Guide v1.1 | Last Updated: February 20, 2026*  
*Compatible with TankAlarm Client Firmware 1.1.2+*
