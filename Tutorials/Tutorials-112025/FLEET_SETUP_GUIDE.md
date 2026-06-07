# TankAlarm Fleet Setup Guide

**Deploying and Managing Multiple Tank Monitoring Devices**

---

## Introduction

The TankAlarm 112025 system uses **fleet-organized devices** with **Notehub Route Relay** for device-to-device communication. Devices send to plain `.qo` notefiles (no colons, no fleet prefixes), and Notehub Routes handle cross-device delivery. This guide walks you through deploying a complete fleet of client devices that communicate with a central server via Routes.

### What You'll Learn

This guide covers:

- Creating and managing fleets in Blues Notehub
- Understanding device-to-device communication
- Deploying multiple client devices efficiently
- Scaling from pilot to full production
- Troubleshooting fleet-wide issues
- Best practices for large deployments

### Fleet Architecture Overview

```
┌─────────────────────────────────────────────────────┐
│              Blues Notehub Cloud                     │
│                                                       │
│  ┌──────────────┐           ┌──────────────┐        │
│  │    Fleet:    │           │    Fleet:    │        │
│  │tankalarm-    │           │tankalarm-    │        │
│  │  clients     │           │   server     │        │
│  │              │           │              │        │
│  │  • Client 1  │           │  • Server 1  │        │
│  │  • Client 2  │           │              │        │
│  │  • Client 3  │           │              │        │
│  │  • ...       │           │              │        │
│  └──────────────┘           └──────────────┘        │
│         ↓                          ↑                 │
│         └──────── notes ───────────┘                 │
└─────────────────────────────────────────────────────┘

Route Relay Pattern:
• Clients send to: telemetry.qo → ClientToServerRelay route → server
• Server sends to: command.qo + _target UID → ServerToClientRelay route → client
• Server broadcasts: command.qo + _target="*" → route delivers to all clients
```

### Prerequisites

Before starting fleet setup:

- [ ] Blues Notehub account created
- [ ] Product created in Notehub
- [ ] Product UID noted
- [ ] At least one server installed and running
- [ ] Client devices ready for deployment

---

## Quick Start (5 Minutes)

For those who want to get started immediately:

### 1. Create Fleets (2 minutes)

1. Log into [notehub.io](https://notehub.io)
2. Navigate to your TankAlarm product
3. Click **Fleets** tab → **Create Fleet**
4. Create **Fleet 1**:
   - Name: `tankalarm-server`
   - Description: `Central data aggregation servers`
5. Create **Fleet 2**:
   - Name: `tankalarm-clients`
   - Description: `Field monitoring devices`
6. Create **Fleet 3**:
   - Name: `tankalarm-viewer`
   - Description: `Read-only kiosk displays`

### 2. Assign Devices (2 minutes)

1. Go to **Devices** tab
2. Select your server Notecard → Assign to `tankalarm-server`
3. Select each client Notecard → Assign to `tankalarm-clients`
4. Select each viewer Notecard → Assign to `tankalarm-viewer`

### 3. Configure Clients (2 minutes per client)

#### New Provisioning Workflow

1. Access server dashboard: `http://<server-ip>/`
2. Look for **"New Sites (Unconfigured)"** section
3. Unconfigured clients appear automatically with:
   - ⚠️ Warning indicator
   - Client UID and firmware version
   - Smart "last seen" timestamp ("● Active now" or "Last seen 5 mins ago")
   - **"Configure →"** button

#### Configuration Steps

**For each new client:**

1. Click **"Configure →"** button
2. Fill required fields:
   - Site Name* (e.g., "North Tank Farm")
   - Device Label* (e.g., "Tank-01")
   - Product UID (auto-filled, fleet-wide setting)

3. Add sensors using **"+ Add Sensor"** button:
   - Choose monitor type (Tank/Gas/RPM)
   - Select sensor type (Digital/Analog/4-20mA/Hall Effect)
   - Configure pin/channel assignments
   - Set max values and thresholds
   - Add alarms, relays, SMS alerts as needed

4. **Validate before sending:**
   - System checks for pin conflicts
   - Validates required fields
   - Confirms sensor configurations
   - Shows detailed error messages if issues found

5. **Download backup** (recommended):
   - Click **"Download JSON"** button
   - Saves timestamped config file (e.g., `TankAlarm_NorthFarm_abc123_2026-02-05.json`)
   - Keep for disaster recovery

6. **Send configuration:**
   - Click **"Send Configuration"** button
   - Watch status messages:
     - 🟡 "Validating configuration..."
     - 🟡 "Sending to server..."
     - ✅ "Configuration queued for delivery"
   - Client receives within ~5 minutes

💡 **Pro Tip:** Start with empty sensor list for truly new sites. Click "+ Add Sensor" only for sensors you've physically connected.

**Done!** Two primary Notehub Routes (ClientToServerRelay + ServerToClientRelay) plus ServerToViewerRelay handle all device-to-device communication automatically. See [NOTEHUB_ROUTES_SETUP.md](NOTEHUB_ROUTES_SETUP.md) for all 5 routes.

---

## Understanding Fleet-Based Communication

### What Are Fleets?

Fleets are **logical groups of devices** in Blues Notehub that enable:

- **Device organization** (clients vs. servers)
- **Route targeting** (Notehub Routes use fleet membership to select destinations)
- **Simplified scaling** (add devices to a fleet without reconfiguring Routes)
- **Broadcast capabilities** (Routes can target all devices in a fleet)

### How Fleet Routing Works

#### Client to Server Communication

When a client sends telemetry:

```cpp
// Client code — plain .qo file, no fleet prefix or colons
J *req = notecard.newRequest("note.add");
JAddStringToObject(req, "file", "telemetry.qo");
JAddItemToObject(req, "body", telemetryData);
notecard.sendRequest(req);
```

**What happens:**
1. Client sends note to `telemetry.qo` (plain outbound notefile — no colons allowed)
2. Notehub syncs the note, triggering the **ClientToServerRelay** route
3. Route delivers the note as `telemetry.qi` on the server Notecard
4. Server reads note via `note.get` and processes data

#### Server to Specific Client

When server configures a client:

```cpp
// Server code — single command.qo with _target for routing
J *req = notecard.newRequest("note.add");
JAddStringToObject(req, "file", "command.qo");
JAddStringToObject(configData, "_target", clientUID);  // device UID for routing
JAddStringToObject(configData, "_type", "config");      // command type
JAddItemToObject(req, "body", configData);               // config payload
notecard.sendRequest(req);
```

**What happens:**
1. Server sends note to `command.qo` with `_target` = client UID in the body
2. Notehub syncs the note, triggering the **ServerToClientRelay** route
3. Route reads `_target` and `_type`, delivers to the appropriate `.qi` file on the target client (e.g., `config.qi`, `relay.qi`)
4. Client reads the specific `.qi` file via `note.get` and applies settings

#### Server Broadcast to All Clients

For fleet-wide updates:

```cpp
// Server code (broadcast) — command.qo with _target="*" for all clients
J *req = notecard.newRequest("note.add");
JAddStringToObject(req, "file", "command.qo");
JAddStringToObject(broadcastData, "_target", "*");       // "*" = all clients
JAddStringToObject(broadcastData, "_type", "broadcast");  // command type
JAddItemToObject(req, "body", broadcastData);
notecard.sendRequest(req);
```

**What happens:**
1. Server sends to `command.qo` with `_target` = `"*"` (wildcard = all clients)
2. **ServerToClientRelay** route delivers to the appropriate `.qi` file on every client in the fleet
3. Each client reads the specific `.qi` file and processes the broadcast message

---

## Step-by-Step Fleet Deployment

### Phase 1: Notehub Preparation

#### 1. Create Product (If Not Already Done)

1. Log into [notehub.io](https://notehub.io)
2. Click **Create Product**
3. Fill in:
   - **Name**: `TankAlarm Fleet`
   - **Description**: `Multi-site tank monitoring system`
   - **Type**: Device Fleet
4. Click **Create**
5. **Save Product UID** (e.g., `com.senax.tankalarm:production`)

#### 2. Create Fleets

**Server Fleet:**
1. In your product, click **Fleets** tab
2. Click **Create Fleet**
3. Enter:
   - **Name**: `tankalarm-server`
   - **Description**: `Central data aggregation servers`
   - **Purpose**: Target for client telemetry, alarms, reports
4. Click **Create**

**Client Fleet:**
1. Click **Create Fleet** again
2. Enter:
   - **Name**: `tankalarm-clients`
   - **Description**: `Field monitoring devices`
   - **Purpose**: Target for server configuration updates
3. Click **Create**

**Viewer Fleet:**
1. Click **Create Fleet** again
2. Enter:
   - **Name**: `tankalarm-viewer`
   - **Description**: `Read-only kiosk display devices`
   - **Purpose**: Target for viewer summary data
3. Click **Create**

**Fleet Naming Best Practices:**
- Use lowercase, no spaces
- Include project name for multi-project accounts
- Be descriptive but concise
- Examples:
  - `tankalarm-server-prod` (production)
  - `tankalarm-server-test` (testing environment)
  - `tankalarm-clients-north` (regional subdivision)

### Phase 2: Server Deployment

#### 1. Provision Server Notecard

**Option A: Manual Provisioning (via Notehub)**
1. Go to **Devices** → **Claim Device**
2. Enter Notecard Serial Number or scan QR code
3. Assign to product
4. Assign to `tankalarm-server` fleet
5. Notecard activates and connects

**Option B: Automatic Provisioning (via Firmware)**
1. Set `DEFAULT_SERVER_PRODUCT_UID` in server firmware (via `ServerConfig.h`)
2. Upload firmware to server Opta
3. Server Notecard self-provisions on first connection
4. Manually assign to `tankalarm-server` fleet in Notehub

#### 2. Verify Server Connectivity

1. In Notehub, go to **Devices**
2. Find your server Notecard
3. Check status shows **"Active"**
4. Verify **Last Seen** is recent
5. Click device to view details
6. Confirm **Fleet** shows `tankalarm-server`

#### 3. Configure Server

1. Access server dashboard: `http://<server-ip>/`
2. Navigate to **Server Settings** page
3. Under **Blues Notehub**, verify or set the **Product UID**
4. Set **Client Fleet** to: `tankalarm-clients`
5. Configure SMS/email contacts
6. Set daily report time
7. Save configuration

> ⚠️ **Important**: The Product UID must be **identical** on the server and all client devices. When deploying clients, ensure you use the same Product UID. The **Config Generator** page automatically fills in the Product UID from the server settings to help ensure consistency.

### Phase 3: Client Deployment

#### Pilot Deployment (1-3 Clients)

**Purpose**: Validate system before full rollout

**Steps:**
1. **Provision First Client:**
   - Power on client Opta
   - Wait for Notecard to connect (green LED)
   - In Notehub, go to **Devices**
   - Notecard appears automatically (if Product UID set)
   - Assign to `tankalarm-clients` fleet

2. **Configure via Server:**
   - Wait for first telemetry (~30 min default)
   - Client appears in server dashboard dropdown
   - Select client and configure:
     - Site name: "Pilot Site 1"
     - Device label: "Tank-01"
     - Tank configurations
   - Send configuration to client

3. **Verify Data Flow:**
   - Watch server dashboard for telemetry updates
   - Check Notehub **Events** tab for note traffic
   - Verify alarms trigger correctly
   - Test SMS delivery

4. **Monitor for 24-48 Hours:**
   - Ensure stable operation
   - Verify daily reports
   - Check cellular data consumption
   - Confirm battery/power stability

#### Staged Rollout (4-20 Clients)

**Purpose**: Expand to small fleet, validate scaling

**Preparation:**
1. Document pilot learnings
2. Create deployment checklist
3. Prepare site-specific configurations
4. Schedule deployment windows

**Deployment Process:**

**Day 1: Deploy 5 clients**
1. Provision Notecards in Notehub
2. Assign all to `tankalarm-clients` fleet
3. Install at sites
4. Power on and verify connectivity
5. Configure via server dashboard

**Day 2-3: Monitor**
1. Watch for telemetry from all clients
2. Verify configurations applied
3. Check for connectivity issues
4. Test alarms at each site

**Day 4-7: Deploy remaining clients**
1. Repeat deployment process
2. Stagger installations to manage workload
3. Configure in batches
4. Document any site-specific issues

#### Full Production (20+ Clients)

**Purpose**: Scale to production fleet

**Best Practices:**

**1. Regional Batching**
Deploy by geographic area:
- Easier troubleshooting (common tower/carrier)
- Efficient site visits if issues arise
- Test regional cellular coverage

**2. Configuration Templates**
Create standard configs for common scenarios:
- **Standard Tank Farm**: 3 tanks, hourly sampling
- **Critical Monitoring**: 8 tanks, 15-min sampling
- **Remote Sites**: 2 tanks, daily reports only

**3. Bulk Provisioning**
1. Provision multiple Notecards at once in Notehub
2. Use CSV import for bulk fleet assignment
3. Pre-label Notecards with site names
4. Track deployments in spreadsheet

**4. Testing Checklist**
For each site:
- [ ] Power LED illuminates
- [ ] Notecard connects (green LED)
- [ ] Appears in Notehub devices
- [ ] Assigned to client fleet
- [ ] First telemetry received
- [ ] Configuration sent and applied
- [ ] Sensors reading correctly
- [ ] Alarms tested (if possible)

---

## Fleet Management

### Monitoring Fleet Health

#### Notehub Dashboard

**Key Metrics:**
1. **Device Activity**:
   - Last Seen timestamps
   - Connection frequency
   - Data consumption

2. **Event Stream**:
   - View all note traffic
   - Filter by device or note type
   - Monitor for errors

3. **Fleet Overview**:
   - Total devices per fleet
   - Active vs inactive devices
   - Cellular coverage map

#### Server Dashboard

**Monitoring Features:**
1. **Client Status Panel**:
   - All connected clients
   - Last update times
   - Connection indicators (green/yellow/red)

2. **Alarm Summary**:
   - Active alarms across fleet
   - Alarm history
   - SMS delivery status

3. **System Health**:
   - Memory usage
   - Network connectivity
   - Notecard status

### Organizing Large Fleets

#### Using Subfleets

For deployments >50 devices, consider subfleets:

**Geographic Subdivision:**
- `tankalarm-clients-north`
- `tankalarm-clients-south`
- `tankalarm-clients-east`
- `tankalarm-clients-west`

**Functional Subdivision:**
- `tankalarm-clients-production`
- `tankalarm-clients-testing`
- `tankalarm-clients-maintenance`

**Advantages:**
- Targeted firmware updates
- Regional configuration changes
- Easier troubleshooting
- Better organization

#### Device Naming Conventions

**Recommended Format:**
```
Site-Role-Number
Examples:
- NorthFarm-Tank-01
- Warehouse3-Pump-02
- Distribution-Tank-A1
```

**Benefits:**
- Easy identification
- Alphabetical sorting
- Clear purpose indication

### Configuration Management

#### Batch Configuration Updates

**Scenario**: Update sample interval for all clients

**Method 1: Individual Updates (Small Fleet)**
1. Access server dashboard
2. For each client:
   - Select from dropdown
   - Update sample interval
   - Send configuration

**Method 2: Broadcast Update (Large Fleet)**
1. Implement broadcast handler in client firmware
2. Server sends fleet-wide update:
   ```json
   {
     "type": "config_update",
     "sampleSeconds": 3600
   }
   ```
3. All clients receive and apply change

**Method 3: Server-Pushed Configuration (Recommended)**
1. Configure update via server web dashboard
2. Server sends via `command.qo` with `_type: "config"`
3. Client receives as `config.qi` and applies settings

#### Configuration Backup

**Server Automatic Backup:**
- Stores all client configs in LittleFS
- Survives server restarts
- Export via FTP (if enabled)

**Manual Backup:**
1. Access server dashboard
2. Record all client configurations
3. Export to spreadsheet or JSON
4. Store in version control

### Firmware Updates

#### Fleet-Wide OTA Updates

Using Blues Notecard DFU:

**Preparation:**
1. Test new firmware on 1-2 devices
2. Monitor for 24-48 hours
3. Verify no issues

**Staged Rollout:**
1. **Day 1**: Update 10% of fleet
2. **Day 3**: Update 30% if no issues
3. **Day 5**: Update remaining 60%

**Execution:**
1. Compile firmware
2. Upload to Notehub → Firmware
3. Target specific fleet or devices
4. Devices auto-update on next check (hourly)

**Rollback Plan:**
- Keep previous firmware in Notehub
- Re-deploy if issues found
- Target affected devices only

See [Firmware Update Guide](FIRMWARE_UPDATE_GUIDE.md) for details.

---

## Scaling Considerations

### Network Capacity

**Cellular Data Usage:**
- **Per Client**: ~5-10 MB/month (default settings)
- **100 Clients**: ~500 MB - 1 GB/month total
- **Notehub Prepaid**: 500 MB for 10 years included

**Optimization Tips:**
- Use event-based reporting (level changes only)
- Increase sample intervals for stable tanks
- Disable daily reports for non-critical tanks
- Compress JSON payloads

### Notehub Limits

**Free Tier:**
- 500 events/month per device
- 1 GB total data/month
- Unlimited devices

**Commercial Plans:**
- Higher event limits
- Priority routing
- Advanced analytics
- Custom integrations

**Monitor Usage:**
1. Notehub → Billing → Usage
2. Track events per device
3. Set up alerts for overages

### Server Capacity

**Single Server Limits:**
- **Clients**: 100-200 (tested)
- **HTTP Connections**: 10 simultaneous
- **Memory**: ~50% usage typical
- **CPU**: <20% under normal load

**When to Add Servers:**
- >200 clients
- Multiple geographic regions
- Redundancy requirements
- Load balancing needs

**Multi-Server Architecture:**
```
Region A Clients → Server A (Fleet: tankalarm-server-a)
Region B Clients → Server B (Fleet: tankalarm-server-b)
```

Each region operates independently with its own fleet.

---

## Troubleshooting Fleet Issues

### Common Scenarios

#### Multiple Clients Not Sending Data

**Symptoms:**
- Several clients missing from dashboard
- No recent telemetry from subset of devices

**Causes:**
1. **Regional cellular outage**
2. **Wrong fleet assignment**
3. **Configuration error in batch**
4. **Notecard firmware bug**

**Diagnostic Steps:**

1. **Check Notehub Events:**
   - Are clients connecting at all?
   - Any error messages?
   - Session logs show connectivity?

2. **Verify Fleet Assignment:**
   - Notehub → Devices
   - Confirm all in `tankalarm-clients` fleet
   - Re-assign if needed

3. **Test Individual Clients:**
   - Serial monitor on one affected client
   - Watch for telemetry send attempts
   - Check Notecard responses

4. **Regional Patterns:**
   - Are all affected clients in same area?
   - Could indicate tower/carrier issue
   - Check Blues status page for outages

#### Server Not Receiving from Specific Clients

**Symptoms:**
- Most clients working
- Specific devices not showing up

**Causes:**
1. **Wrong `serverFleet` in client config**
2. **Client Notecard not connected**
3. **Client firmware issue**
4. **Note queue backed up**

**Solutions:**

1. **Verify Client Configuration:**
   ```json
   {
     "serverFleet": "tankalarm-server"  // Must match!
   }
   ```

2. **Check Notecard Connection:**
   - Serial monitor: `{"req":"hub.sync.status"}`
   - Should show "completed"

3. **Force Sync:**
   ```cpp
   {"req":"hub.sync"}
   ```

4. **Clear Note Queue (if stuck):**
   ```cpp
   {"req":"note.changes", "delete":true}
   ```

#### Configuration Not Reaching Clients

**Symptoms:**
- Server shows "Config sent"
- Client never receives/applies

**Causes:**
1. **Wrong device UID**
2. **Client not polling for configs**
3. **Notehub routing issue**
4. **Client firmware not handling configs**

**Solutions:**

1. **Verify Device UID:**
   - Notehub → Devices → copy exact UID
   - Server → ensure exact match in dropdown

2. **Check Client Polling:**
   - Client should query for specific `.qi` files (e.g., `config.qi`, `relay.qi`) delivered by ServerToClientRelay route
   - Verify in client firmware: `note.get`

3. **Monitor Notehub Events:**
   - Watch for `command.qo` note with matching `_target` UID in the body
   - Verify the ServerToClientRelay route delivery status

4. **Client Serial Monitor:**
   - Should show "Configuration updated"
   - If not, check note parsing logic

### Performance Optimization

#### Reducing Cellular Data

**Techniques:**
1. **Event-Based Reporting:**
   - Set a per-sensor `reportThreshold` > 0
   - Only report on significant changes
   - Can reduce traffic by 50-70%

2. **Longer Sample Intervals:**
   - Slow-changing tanks: 3600s (1 hour)
   - Critical tanks: 1800s (30 min)
   - Stable tanks: 7200s (2 hours)

3. **Selective Daily Reports:**
   - Disable `daily` flag for non-critical tanks
   - Reduces morning data spike

4. **JSON Compression:**
   - Use shorter key names (already done)
   - Remove unnecessary fields
   - Example: `l` instead of `levelInches`

#### Improving Response Time

**Alarm Response:**
1. **Continuous Mode** (faster but more power):
   ```cpp
   {"req":"hub.set", "mode":"continuous"}
   ```
   - Checks for inbound every ~60 seconds
   - Good signal required

2. **Event-Based Triggers:**
   - Level change detected → immediate send
   - Don't wait for sample interval

**Dashboard Updates:**
1. **Reduce Refresh Interval:**
   - Default: 6 hours
   - Set to 1-2 hours for active monitoring
   - Balance freshness vs battery

2. **Client-Side Caching:**
   - Browser caches static assets
   - Only fetch new data via API

---

## Best Practices

### Deployment Planning

**Pre-Deployment:**
1. **Site Survey**:
   - Verify cellular coverage
   - Plan power sources
   - Identify sensor locations
   - Note any access restrictions

2. **Equipment Prep**:
   - Pre-provision all Notecards
   - Assign to fleets before shipping
   - Pre-configure common settings
   - Label devices clearly

3. **Installation Kit**:
   - Power supplies
   - Mounting hardware
   - Ethernet cables (for server)
   - Tools (screwdrivers, wire strippers)
   - Multimeter for troubleshooting

**During Deployment:**
1. Follow installation checklist
2. Test connectivity before leaving site
3. Document any deviations
4. Take photos for reference

**Post-Deployment:**
1. Monitor for 24-48 hours
2. Verify first telemetry received
3. Test alarm delivery
4. Schedule follow-up check

### Documentation

**Fleet Inventory:**
Maintain spreadsheet with:
- Device UID
- Site name
- Installation date
- GPS coordinates
- Contact person
- Tank IDs monitored
- Special notes

**Configuration Database:**
- Server IP address
- Static IP assignments
- SMS contact list
- Email recipients
- Notehub Product UID
- Fleet names

**Maintenance Log:**
- Firmware updates applied
- Configuration changes
- Issues encountered
- Resolutions implemented

### Security

**Fleet-Wide Security:**
1. **Notecard Security** (automatic):
   - TLS encryption
   - Device authentication
   - Secure provisioning

2. **Server Security**:
   - Firewall rules
   - Access control (future: authentication)
   - VLAN isolation
   - VPN for remote access

3. **API Security**:
   - PIN protection for sensitive operations
   - Rate limiting (future enhancement)
   - Input validation

**Audit Trail:**
- Notehub logs all note traffic
- Server logs configuration changes
- Review logs periodically

---

## Advanced Scenarios

### Multi-Region Deployment

For deployments spanning multiple geographic regions:

**Architecture:**
```
Region A: Fleet "tankalarm-clients-regionA" → Server A
Region B: Fleet "tankalarm-clients-regionB" → Server B
```

**Benefits:**
- Reduced latency (regional servers)
- Fault isolation
- Easier regulatory compliance
- Regional carrier optimization

**Implementation:**
1. Create regional fleets in Notehub
2. Deploy regional servers
3. Configure clients for appropriate server fleet
4. Aggregate data at central location (optional)

### Redundant Servers

For high-availability deployments:

**Active-Passive:**
```
Primary Server: Fleet "tankalarm-server-primary"
Backup Server: Fleet "tankalarm-server-backup"

Clients: Send telemetry.qo once — Routes handle delivery to both servers
```

**Implementation:**
```cpp
// Client sends telemetry ONCE — no fleet prefix, no colons
J *req = notecard.newRequest("note.add");
JAddStringToObject(req, "file", "telemetry.qo");
JAddItemToObject(req, "body", telemetryData);
notecard.sendRequest(req);

// Two Notehub Routes handle redundancy:
// 1. ClientToServerRelay-Primary → delivers telemetry.qi to primary server
// 2. ClientToServerRelay-Backup  → delivers telemetry.qi to backup server
```

**Considerations:**
- Client code is simple (single `telemetry.qo` write)
- Redundancy is configured entirely in Notehub Routes
- Failover: disable one route, enable the other

### Custom Routing

For advanced scenarios beyond standard fleets:

**Blues Notehub Routes:**
1. Create route: Notehub → External API
2. Transform note data
3. Forward to custom backend
4. Process/store in external database

**Use Cases:**
- Integration with existing systems
- Advanced analytics
- Long-term data warehousing
- Third-party monitoring services

**Setup:**
1. Notehub → Routes → Create Route
2. Select source: Inbound notes
3. Configure transform (JSONata)
4. Set destination: HTTP endpoint
5. Test with sample data

---

## Resources and Going Further

### Notehub Documentation

- [Blues Notehub Overview](https://dev.blues.io/notehub/notehub-walkthrough/)
- [Fleet Management Guide](https://dev.blues.io/notehub/fleet-management/)
- [Device-to-Device Messaging](https://dev.blues.io/guides-and-tutorials/notecard-guides/understanding-device-to-device-communications/)
- [Routes and Transforms](https://dev.blues.io/notehub/notehub-walkthrough/#routing-data-with-notehub)

### Related Guides

- [Client Installation Guide](CLIENT_INSTALLATION_GUIDE.md)
- [Server Installation Guide](SERVER_INSTALLATION_GUIDE.md)
- [Firmware Update Guide](FIRMWARE_UPDATE_GUIDE.md)
- [Quick Start Guide](QUICK_START_GUIDE.md)

### Community and Support

- **Blues Community**: [community.blues.io](https://community.blues.io)
- **GitHub Issues**: [SenaxInc/SenaxTankAlarm](https://github.com/SenaxInc/SenaxTankAlarm/issues)
- **Arduino Forum**: [forum.arduino.cc/opta](https://forum.arduino.cc/c/hardware/opta/181)

### Video Tutorials

- [Blues Notehub Walkthrough](https://www.youtube.com/blues_wireless)
- [Fleet Setup Demo](https://www.youtube.com/blues_wireless)
- [TankAlarm Deployment Guide](coming soon)

---

## Appendix: Fleet Configuration Reference

### Client Configuration Fields

```json
{
  "site": "string",              // Site name
  "deviceLabel": "string",       // Device identifier
  "serverFleet": "string",       // Target fleet (must match server)
  "sampleSeconds": number,       // Sample interval
  "reportHour": number,          // Daily report hour (0-23)
  "reportMinute": number,        // Daily report minute (0-59)
  "dailyEmail": "string",        // Email for reports
  "sensors": [ /* array of sensor configs; each may set "reportThreshold": number in its own unit (0 = off) */ ]
}
```

### Server Configuration Fields

```json
{
  "serverName": "string",        // Server display name
  "clientFleet": "string",       // Target fleet for broadcasts
  "smsPrimary": "string",        // Primary SMS contact
  "smsSecondary": "string",      // Secondary SMS contact
  "smsTertiary": "string",       // Tertiary SMS contact
  "dailyEmail": "string",        // Daily report recipient
  "dailyHour": number,           // Report hour (0-23)
  "dailyMinute": number,         // Report minute (0-59)
  "webRefreshSeconds": number,   // Dashboard refresh interval
  "useStaticIp": boolean         // Static vs DHCP
}
```

### Notecard API Quick Reference

**Check fleet assignment:**
```cpp
{"req":"hub.get"}
// Response includes "fleet" field
```

**Force sync:**
```cpp
{"req":"hub.sync"}
```

**Check inbound queue:**
```cpp
{"req":"note.get", "file":"config.qi", "delete":true}
```

**Send telemetry (client → server via Route):**
```cpp
{"req":"note.add", "file":"telemetry.qo", "body":{...}}
```

**Send command to specific client (server → client via Route):**
```cpp
{"req":"note.add", "file":"command.qo", "body":{"_target":"dev:123456", "_type":"config", ...}}
```

---

*Fleet Setup Guide v1.1 | Last Updated: February 20, 2026*  
*Compatible with TankAlarm Firmware 1.1.2+*
