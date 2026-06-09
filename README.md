# TankAlarm v1.8.5 - Industrial Tank Monitoring System

**Release Date:** April 18, 2026  
**Version:** 1.8.5  
**Platform:** Arduino Opta + Blues Wireless Notecard

A production-ready industrial monitoring system for remote tank level monitoring, alarm management, and fleet coordination using cellular IoT connectivity.

---

## 🎯 Overview

TankAlarm is a complete industrial monitoring solution designed for reliable, remote tank level monitoring with SMS/email alerts. Built on the Arduino Opta industrial controller platform with Blues Wireless cellular connectivity, it provides enterprise-grade reliability without requiring WiFi or wired network infrastructure at remote sites.

### System Architecture

The system consists of three components:

- **Client** - Remote monitoring device (deployed at tank sites)
- **Server** - Central data aggregation and alerting hub (office/headquarters)
- **Viewer** - Read-only monitoring device (optional, for remote viewing)

```
       ┌─────────────┐                  ┌─────────────┐
       │   CLIENT    │                  │   SERVER    │
       │  (Remote)   │                  │ (HQ/Office) │
       │ • Monitors  │                  │ • Dashboard │
       │ • Sensors   │                  │ • Alerts    │
       └──────▲──────┘                  └──────▲──────┘
              │                                │
              │ Cellular                       │ Cellular
              ▼                                ▼
       ┌──────────────────────────────────────────────┐
       │            BLUES NOTEHUB (Cloud)             │
       │     (Data Exchange & Device Management)      │
       └──────────────────────┬───────────────────────┘
                              │
                              │ Cellular/WiFi
                              ▼
                       ┌─────────────┐
                       │   VIEWER    │
                       │ (Read-Only) │
                       └──────┬──────┘
                              │
                         Ethernet LAN
                        (User Display)
```

---

## ✨ Key Features

### Client (Remote Monitoring)
- **Multi-Sensor Support** - Monitor up to 8 objects per device
  - Analog voltage (0-10V)
  - 4-20mA current loop (pressure sensors, level transmitters)
  - Digital on/off (float switches)
  - Pulse counting (flow meters, RPM sensors)
- **Flexible Object Types** - Tanks, engines, pumps, gas systems, flow meters
- **Intelligent Alarming** - Configurable high/low thresholds with hysteresis and debouncing
- **Rate Limiting** - Prevents alert flooding during transient conditions
- **Relay Control** - Automated shutoff with multiple modes (momentary, until_clear, manual_reset)
- **Remote Configuration** - Update all settings via server web interface
- **Persistent Storage** - LittleFS internal flash (no SD card required)
- **Watchdog Protection** - Automatic recovery from system hangs
- **Low Power Design** - Optimized for battery/solar operation

### Server (Central Hub)
- **Web Dashboard** - Real-time monitoring of all clients
- **SMS & Email Alerts** - Immediate notifications for alarm conditions
- **Daily Reports** - Scheduled email summaries
- **Remote Client Management** - Configure any client from web interface
- **RESTful API** - Programmatic access for automation
- **Fleet Management** - Centralized control via Blues Notehub
- **FTP Backup/Restore** - Configuration archiving
- **Client Console** - Real-time serial log viewing for troubleshooting
- **PIN Authentication** - Secure access with constant-time comparison and rate limiting
- **Configuration Import/Export** - JSON-based backup and templating
- **Config Comparison** - Visual diff before deploying changes
- **Unsaved Changes Warning** - Prevents accidental data loss
- **Audit Log** - Track configuration changes with timestamps
- **Contextual Tooltips** - Inline help for complex configuration options

### Viewer (Read-Only Display)
- **Dashboard-Only** - No configuration access
- **Stale Data Warnings** - Visual indicators for outdated readings
- **Minimal Attack Surface** - Secure deployment for public areas
- **Notecard Sync** - Automatic data updates from server

---

## 🔧 Hardware Requirements

### Client Hardware
- **Arduino Opta Lite** - Industrial controller (STM32H747XI dual-core)
- **Blues Wireless for Opta** - Cellular Notecard carrier board
- **Arduino Opta Ext A0602** (optional) - 4-20mA analog expansion for sensors
- **Sensors** - Compatible with 0-10V, 4-20mA, digital, or pulse outputs
- **Power Supply** - 12-24V DC (solar + battery recommended for remote sites)

### Server Hardware
- **Arduino Opta Lite** - Industrial controller
- **Blues Wireless for Opta** - Cellular Notecard carrier board
- **Ethernet Connection** - RJ45 to local network
- **Power Supply** - 12-24V DC

### Viewer Hardware (Optional)
- **Arduino Opta Lite** - Industrial controller
- **Blues Wireless for Opta** - Cellular Notecard carrier board
- **Ethernet Connection** - RJ45 to local network
- **Power Supply** - 12-24V DC

---

## 📋 Sensor Compatibility

### Tested Sensors

| Type | Model | Interface | Application |
|------|-------|-----------|-------------|
| Pressure Transmitter | Dwyer 626-06-CB-P1-E5-S1 | 4-20mA | Tank level (0-5 PSI) |
| Float Switch | Dayton 3BY75 | Digital | High/low level detection |
| Hall Effect Sensor | Generic | Pulse | Engine RPM monitoring |
| Flow Meter | Generic | Pulse | Liquid flow rate |

### Sensor Interface Types

- **Digital (Float Switches)** - Binary on/off, 3.3V/5V compatible
- **Analog Voltage** - 0-10V (via Opta Ext A0602)
- **4-20mA Current Loop** - Pressure transmitters, level sensors
- **Pulse Counting** - Flow meters, RPM sensors (up to 10 kHz)

---

## 🚀 Quick Start

### 1. Hardware Setup

**Client (Remote Site):**
1. Install Blues Wireless for Opta carrier on Arduino Opta
2. Activate Notecard at [blues.io](https://blues.io)
3. Connect sensors to appropriate inputs
4. Connect 12-24V DC power (solar recommended)

**Server (Office/HQ):**
1. Install Blues Wireless for Opta carrier on Arduino Opta
2. Activate Notecard at [blues.io](https://blues.io)
3. Connect Ethernet cable to local network
4. Connect 12-24V DC power supply

### 2. Software Installation

**Prerequisites:**
- Arduino IDE 2.x or later
- Arduino Mbed OS Opta Boards core (via Boards Manager)

**Required Libraries:**
- ArduinoJson v7.x or later
- Blues Wireless Notecard (latest)
- LittleFS (built into Mbed core)
- Ethernet (built-in)

**Installation:**
1. Clone this repository
2. Install TankAlarm-112025-Common library:
   ```powershell
   # Windows PowerShell (as Admin)
   New-Item -ItemType Junction -Path "$env:USERPROFILE\Documents\Arduino\libraries\TankAlarm-112025-Common" -Target "C:\path\to\ArduinoSMSTankAlarm\TankAlarm-112025-Common"
   ```
3. Open the appropriate sketch in Arduino IDE:
   - Client: `TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino`
   - Server: `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino`
   - Viewer: `TankAlarm-112025-Viewer-BluesOpta/TankAlarm-112025-Viewer-BluesOpta.ino`
4. Update `PRODUCT_UID` to match your Blues Notehub project
5. Compile and upload

### 3. Blues Notehub Configuration

1. Create account at [notehub.io](https://notehub.io)
2. Create a new product for your TankAlarm system
3. Create three fleets:
   - `tankalarm-server` - For server device(s)
   - `tankalarm-clients` - For all client devices
   - `tankalarm-viewer` - For viewer device(s)
4. Claim all Notecards into the product
5. Assign each Notecard to the appropriate fleet

**Detailed Instructions:**
- Client Setup: [TankAlarm-112025-Client-BluesOpta/README.md](TankAlarm-112025-Client-BluesOpta/README.md)
- Server Setup: [TankAlarm-112025-Server-BluesOpta/README.md](TankAlarm-112025-Server-BluesOpta/README.md)
- Fleet Configuration: [TankAlarm-112025-Server-BluesOpta/FLEET_SETUP.md](TankAlarm-112025-Server-BluesOpta/FLEET_SETUP.md)

### 4. Access Web Dashboard

1. Power on the server and wait for network connection
2. Check serial monitor (115200 baud) for IP address
3. Navigate to: `http://<server-ip>/`
4. Configure server settings and SMS/email recipients
5. Configure clients remotely via the dashboard

---

## 📊 Technical Specifications

### Client Specifications
- **Lines of Code:** 5,488
- **Memory Footprint:** ~150KB flash, ~30KB RAM
- **Monitors per Device:** Up to 8
- **Sample Interval:** Configurable (default: 30 minutes)
- **Alarm Hysteresis:** Configurable per monitor
- **Watchdog Timeout:** 30 seconds
- **Cellular Connectivity:** Blues Wireless Notecard (LTE-M/NB-IoT)
- **Data Persistence:** LittleFS internal flash
- **Power Requirements:** 12-24V DC, ~2W typical

### Server Specifications
- **Lines of Code:** 10,328
- **Memory Footprint:** ~300KB flash, ~100KB RAM
- **Max Clients:** 32 (expandable)
- **Max Tanks:** 64 total across all clients
- **Web Server:** HTTP (port 80)
- **API Endpoints:** 15+ RESTful endpoints
- **Network:** Ethernet (10/100 Mbps)
- **Data Persistence:** LittleFS internal flash
- **Power Requirements:** 12-24V DC, ~3W typical

### Viewer Specifications
- **Lines of Code:** 763
- **Memory Footprint:** ~100KB flash, ~20KB RAM
- **Web Server:** HTTP (port 80) - read-only
- **Network:** Ethernet (10/100 Mbps)
- **Power Requirements:** 12-24V DC, ~2W typical

---

## 📡 Communication Architecture

### Fleet-Based Routing (Blues Notehub)

The system uses Blues Notehub fleet-based routing for device-to-device communication:

1. **Client → Server:** Telemetry and alarms routed to `tankalarm-server` fleet
2. **Server → Client:** Configuration updates routed to specific client device IDs
3. **Server → Viewer:** Status updates sent for dashboard display

**Advantages:**
- No hardcoded device IDs in firmware
- Simplified fleet management
- Automatic routing via Notehub
- Scalable to hundreds of devices

### Data Flow

```
CLIENT                      BLUES NOTEHUB              SERVER
  │                               │                       │
  ├─── telemetry.qo ──────────────┤                       │
  │         (level data)          │                       │
  │                               ├─── fleet routing ────▶│
  │                               │                       │
  ├─── alarm.qo ──────────────────┤                       │
  │      (alarm event)            │                       │
  │                               ├─── fleet routing ────▶│
  │                               │                       │
  │                               │                       ├─ SMS/Email
  │                               │                       │  Alerts
  │                               │                       │
  │                               │◀─── config_push.qi ───┤
  │                               │   (new config)        │
  │◀──── route: device_id ────────┤                       │
  │     (config received)         │                       │
```

---

## 🛡️ Security Features

### Authentication
- **PIN-based Access Control** - Protect configuration endpoints
- **Constant-Time PIN Comparison** - Prevents timing attack analysis
- **Authentication Rate Limiting** - Exponential backoff with lockout after 5 failures
- **Optional PIN** - Can be disabled for trusted networks
- **Session-based** - PIN verified per-session

### Input Validation
- All POST endpoints validate JSON structure
- Buffer overflow protection on string inputs
- Range checking on numeric values
- Client UID length validation with diagnostic logging
- Hash table bounds checking to prevent out-of-bounds access
- HTTP 400/401/429/500 status codes for error conditions

### Network Security
- **HTTP Only** - Deploy on trusted local networks
- **No Internet Exposure** - Server should not be internet-facing
- **Ethernet Isolation** - Physical network segmentation recommended

**Note:** HTTPS/TLS not currently supported. For internet-facing deployments, use VPN or reverse proxy with TLS termination.

---

## 📖 Documentation

### Installation & Setup
- **Client:** [TankAlarm-112025-Client-BluesOpta/README.md](TankAlarm-112025-Client-BluesOpta/README.md)
- **Server:** [TankAlarm-112025-Server-BluesOpta/README.md](TankAlarm-112025-Server-BluesOpta/README.md)
- **Fleet Setup:** [TankAlarm-112025-Server-BluesOpta/FLEET_SETUP.md](TankAlarm-112025-Server-BluesOpta/FLEET_SETUP.md)

### Advanced Features
- **Relay Control:** [TankAlarm-112025-Client-BluesOpta/RELAY_CONTROL.md](TankAlarm-112025-Client-BluesOpta/RELAY_CONTROL.md)
- **Unload Tracking:** [TankAlarm-112025-Client-BluesOpta/UNLOAD_TRACKING.md](TankAlarm-112025-Client-BluesOpta/UNLOAD_TRACKING.md)
- **Device-to-Device API:** [TankAlarm-112025-Client-BluesOpta/DEVICE_TO_DEVICE_API.md](TankAlarm-112025-Client-BluesOpta/DEVICE_TO_DEVICE_API.md)
- **Migration Guide:** [TankAlarm-112025-Client-BluesOpta/MIGRATION_GUIDE.md](TankAlarm-112025-Client-BluesOpta/MIGRATION_GUIDE.md)

### Security & Architecture
- **Security Fixes (Feb 2026):** [CODE REVIEW/SECURITY_FIXES_02062026.md](CODE%20REVIEW/SECURITY_FIXES_02062026.md)
- **Communication Architecture:** [CODE REVIEW/COMMUNICATION_ARCHITECTURE_VERDICT_02192026.md](CODE%20REVIEW/COMMUNICATION_ARCHITECTURE_VERDICT_02192026.md)
- **Data Usage Analysis:** [CODE REVIEW/DATA_USAGE_ANALYSIS_02192026.md](CODE%20REVIEW/DATA_USAGE_ANALYSIS_02192026.md)
- **Common Header Audit:** [CODE REVIEW/COMMON_HEADER_AUDIT_02192026.md](CODE%20REVIEW/COMMON_HEADER_AUDIT_02192026.md)

### Code Reviews & Release History
- **v1.1.8 Release Notes:** [CODE REVIEW/V1.1.8_RELEASE_NOTES.md](CODE%20REVIEW/V1.1.8_RELEASE_NOTES.md)
- **v1.1.7 Release Notes:** [CODE REVIEW/V1.1.7_RELEASE_NOTES.md](CODE%20REVIEW/V1.1.7_RELEASE_NOTES.md)
- **v1.1.6 Release Notes:** [CODE REVIEW/V1.1.6_RELEASE_NOTES.md](CODE%20REVIEW/V1.1.6_RELEASE_NOTES.md)
- **v1.1.5 Release Notes:** [CODE REVIEW/V1.1.5_RELEASE_NOTES.md](CODE%20REVIEW/V1.1.5_RELEASE_NOTES.md)
- **v1.1.4 Release Notes:** [CODE REVIEW/V1.1.4_RELEASE_NOTES.md](CODE%20REVIEW/V1.1.4_RELEASE_NOTES.md)
- **v1.1.1 Release Notes:** [CODE REVIEW/V1.1.1_RELEASE_NOTES.md](CODE%20REVIEW/V1.1.1_RELEASE_NOTES.md)
- **v1.0 Release Summary:** [CODE REVIEW/V1.0_RELEASE_SUMMARY.md](CODE%20REVIEW/V1.0_RELEASE_SUMMARY.md)
- **v1.0.1 Release Notes:** [CODE REVIEW/V1.0.1_RELEASE_NOTES.md](CODE%20REVIEW/V1.0.1_RELEASE_NOTES.md)
- **Advanced Features (Feb 2026):** [CODE REVIEW/ADVANCED_FEATURES_IMPLEMENTATION_02052026.md](CODE%20REVIEW/ADVANCED_FEATURES_IMPLEMENTATION_02052026.md)
- **Historical Data Architecture:** [TankAlarm-112025-Server-BluesOpta/HISTORICAL_DATA_ARCHITECTURE.md](TankAlarm-112025-Server-BluesOpta/HISTORICAL_DATA_ARCHITECTURE.md)
- **Console Restrictions:** [TankAlarm-112025-Server-BluesOpta/CONSOLE_RESTRICTIONS.md](TankAlarm-112025-Server-BluesOpta/CONSOLE_RESTRICTIONS.md)

---

## 🧪 Testing & Deployment

### Pre-Deployment Testing Checklist

- [ ] **Hardware Validation**
  - [ ] All sensors reading correctly
  - [ ] Relay control functioning
  - [ ] Cellular connectivity stable
  - [ ] Ethernet connectivity stable
  
- [ ] **Software Validation**
  - [ ] Firmware version 1.8.5 confirmed
  - [ ] All clients reporting to server
  - [ ] Alarms triggering correctly
  - [ ] SMS/email alerts delivering
  - [ ] Web dashboard accessible
  
- [ ] **Network Testing**
  - [ ] Blues Notehub routing configured
  - [ ] Fleet assignments verified
  - [ ] Server IP address documented
  - [ ] Firewall rules configured (if applicable)
  
- [ ] **Burn-In Testing**
  - [ ] 48-hour continuous operation test
  - [ ] Watchdog recovery test
  - [ ] Power cycle recovery test
  - [ ] Network failover test

### Deployment Checklist

1. Flash all devices with v1.8.5 firmware
2. Configure Blues Notehub fleet assignments
3. Set server IP address and network configuration
4. Configure SMS/email recipients
5. Test alarm notifications
6. Test relay control
7. Verify web dashboard access
8. Document device serial numbers and locations
9. Establish backup schedule (FTP recommended)

---

## 🔮 Roadmap

### v1.2 (Planned)
- [ ] Enhanced email formatting (HTML, attachments)
- [ ] Historical data logging (30-day retention)
- [ ] Graphical trend charts
- [ ] Common header consolidation (centralize duplicated constants)
- [ ] Event-driven Notecard polling (`file.changes` + change trackers)

### v2.0 (Future)
- [ ] Advanced analytics and reporting
- [ ] Predictive maintenance alerts

---

## 📜 License

See [LICENSE](LICENSE) file for details.

---

## 🤝 Support

For technical support, bug reports, or feature requests, please open an issue on GitHub.

### Common Resources
- **Blues Wireless Documentation:** [dev.blues.io](https://dev.blues.io)
- **Arduino Opta Documentation:** [docs.arduino.cc](https://docs.arduino.cc/hardware/opta)
- **Bill of Materials:** [TankAlarm-112025-BillOfMaterials.md](TankAlarm-112025-BillOfMaterials.md)

---

## 🏗️ Repository Structure

```
SenaxTankAlarm/
├── TankAlarm-112025-Client-BluesOpta/    # Remote monitoring client
├── TankAlarm-112025-Server-BluesOpta/    # Central server & dashboard
├── TankAlarm-112025-Viewer-BluesOpta/    # Read-only viewer
├── TankAlarm-112025-Common/              # Shared library
├── CODE REVIEW/                          # Code reviews & documentation
├── RecycleBin/                           # Archived versions
├── Tutorials/                            # Getting started guides
├── TankAlarm-112025-BillOfMaterials.md    # Hardware BOM
└── README.md                             # This file
```

---

---

## 📋 Changelog

### v1.1.8 (March 16, 2026)
- **Data Integrity:** Save all dirty data (registry, metadata, hot tier, history settings) before DFU/reboot — previously only saved config
- **Dedup Daily Summaries:** Persist last rollup date and remove duplicates on reboot to prevent double-counted daily entries
- **FTP Backup Expansion:** Added `tank_registry.json`, `client_metadata.json`, and `history_settings.json` to nightly FTP backup list
- **Crash Recovery:** Added `hot_tier.json` and `archived_clients.json` to orphaned `.tmp` file recovery
- **Warm Tier Fallback:** FTP monthly archives now aggregate warm tier daily summaries when hot tier has no data for the target month
- **SMS Rate-Limit Persistence:** `lastSmsAlertEpoch` and `smsAlertsInLastHour` now survive reboots via tank registry
- **FTP Sync Hour:** `ftpSyncHour` setting now actually controls when nightly FTP archive runs (was previously ignored)
- **History Slots Warning:** Rate-limited warning logged when all 20 tank history slots are exhausted
- **Default Fix:** `hotTierRetentionDays` default corrected from 730 to 90 to match actual ring buffer capacity
- **Stale Sensor Pruning:** 3-layer auto-pruning system — config-based periodic, per-sensor orphan (72h), dead client removal (7d)
- **FTP Archive Before Removal:** Clients active >30 days archived to FTP with date-stamped naming before deletion
- **Archived Clients API:** New `/api/history/archived` endpoint and Historical Data page section for browsing archived client data

### v1.1.7 (March 15, 2026)
- **Naming Refactor:** Complete elimination of tank-centric numbering from data model — `tankNumber` → `sensorIndex` with new optional `userNumber` display field
- **API Consistency:** All JSON API keys, C++ locals, JS properties, and URL parameters updated from `"tank"` to `"sensorIndex"`/`"sensor"`
- **Backward Compatibility Removal:** ~50 dual-key JSON parsing fallbacks removed across all three firmware files
- **Bug Fix:** Client `saveConfigToFlash()` now serializes `monitorType` (was silently dropped)
- **Bug Fix:** Client `serializeConfig()` fixed to emit `sensorIndex` correctly
- **Documentation:** 8 docs updated to reflect new naming conventions
- **URL Param Rename:** `/api/history?tank=` → `?sensor=`, `/api/history/yoy?tank=` → `?sensor=`

### v1.1.1 (February 20, 2026)
- **Viewer Fleet:** Viewer devices now join dedicated `tankalarm-viewer` fleet for fleet-scoped DFU and routing
- **Relay Forwarding:** New `relay_forward.qo`/`.qi` protocol for client-to-client relay commands via server
- **Serial ACKs:** Client emits `serial_ack.qo` processing/complete status during serial log requests
- **Config ACK Enhancements:** Config version hash (`cv`) tracked through dispatch → ACK cycle
- **ArduinoJson v7:** Full migration to auto-sizing `JsonDocument` in Server and Viewer (removed all capacity constants)
- **Notecard Hardening:** NULL-safe `card.uuid` handling across all three sketches
- **Watchdog Fix:** Corrected macro name to `TANKALARM_WATCHDOG_AVAILABLE` across all components
- **Memory Safety:** Use `JDelete` for unsent request cleanup; fix relay body allocation leak
- **Dead Code Removal:** ~300 lines of unused helpers removed from Common headers
- **Documentation:** 14 tutorial guides updated for three-fleet architecture

### v1.1.0 (February 19, 2026)
- **Security:** Constant-time PIN comparison to prevent timing attacks
- **Security:** Authentication rate limiting with exponential backoff and lockout
- **Security:** Hash table bounds checking to prevent out-of-bounds memory access
- **Security:** Client UID length validation with diagnostic logging
- **Security:** Buffer boundary fix for FTP response parsing
- **Server Console:** Configuration import/export (JSON backup & templating)
- **Server Console:** Unsaved changes warning with browser prompt
- **Server Console:** Configuration comparison with visual diff before deploy
- **Server Console:** Audit log tracking last 50 configuration changes
- **Server Console:** Contextual tooltips for complex configuration fields
- **Architecture:** Communication architecture review and documentation
- **Architecture:** Data usage analysis for standard vs. proxy patterns
- **Architecture:** Common header audit identifying constants to centralize
- **UI:** Centralized CSS with browser caching for faster page loads
- **Stability:** JavaScript null-safety hardening across dashboard pages
- **Stability:** UTF-8 BOM and struct compilation fixes

### v1.0.1 (January 13, 2026)
- Centralized CSS into single cached stylesheet
- UI header standardization (Action Bar layout)
- JavaScript null-safety hardening
- Compilation fixes (UTF-8 BOM, struct definitions)

### v1.0.0 (December 2025)
- Initial production release
- Client, Server, and Viewer firmware
- Blues Wireless fleet-based routing
- Web dashboard with SMS/email alerts
- LittleFS persistent storage
- Remote client configuration

---

**Built with ❤️ for industrial IoT applications**

*Last Updated: March 13, 2026*
