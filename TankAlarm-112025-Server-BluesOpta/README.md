# TankAlarm 112025 Server - Blues Opta

Central data aggregation server for TankAlarm system using Arduino Opta with Blues Wireless Notecard and Ethernet connectivity.

## Overview

The TankAlarm 112025 Server receives tank level data from multiple clients, displays real-time status on a web dashboard, sends SMS/email alerts, and enables remote client configuration. Key features include:

- **Web dashboard** - Real-time tank monitoring via Ethernet
- **Remote configuration** - Update clients from web interface
- **Alert distribution** - SMS and email notifications for alarms
- **Daily reports** - Scheduled email summaries
- **Fleet management** - Centralized control of multiple clients
- **Persistent storage** - LittleFS internal flash (no SD card needed)
- **Blues Notehub integration** - Cellular data aggregation

## Hardware

- **Arduino Opta Lite** - Industrial controller
- **Blues Wireless for Opta** - Cellular Notecard carrier
- **Ethernet connection** - RJ45 cable for network connectivity
- **12-24V DC power supply** - For continuous operation

## Quick Start

### 1. Hardware Setup
1. Install Blues Wireless for Opta carrier on Arduino Opta
2. Activate Notecard with Blues Wireless (see [blues.io](https://blues.io))
3. Connect Ethernet cable to local network
4. Connect 12-24V DC power supply

### 2. Software Setup
1. Install Arduino IDE 2.x or later
2. Install Arduino Mbed OS Opta Boards core via Boards Manager
3. Install required libraries:
   - **ArduinoJson** (version 7.x or later)
   - **Blues Wireless Notecard** (latest)
   - **Ethernet** (built-in)
   - LittleFS (built into Mbed core)
4. Open `TankAlarm-112025-Server-BluesOpta.ino` in Arduino IDE
5. Compile and upload to Arduino Opta
6. Access the web dashboard and set your Blues Notehub **Product UID** in **Server Settings**

**For detailed step-by-step instructions, see [INSTALLATION.md](INSTALLATION.md)**

**For Notecard I2C diagnostics/recovery, see [../Tutorials/Tutorials-112025/I2C_UTILITY_GUIDE.md](../Tutorials/Tutorials-112025/I2C_UTILITY_GUIDE.md)**

### 3. Blues Notehub Setup
1. Create account at [notehub.io](https://notehub.io)
2. Create a product for your tank alarm system
3. Create three fleets:
   - `tankalarm-server` - For the server device
   - `tankalarm-clients` - For all client devices
   - `tankalarm-viewer` - For viewer device(s)
4. Assign server Notecard to server fleet
5. Assign client Notecards to client fleet
6. Claim all Notecards into the product

**For detailed fleet setup, see [FLEET_SETUP.md](FLEET_SETUP.md)**

### 4. Access Web Dashboard
1. Power on the server and wait for Ethernet connection
2. Check serial monitor (115200 baud) for IP address
3. From a computer on the same network, navigate to: `http://<server-ip>/`
4. The dashboard displays real-time tank levels from all clients

## Web Dashboard Features

### Real-Time Monitoring
- **Tank Levels**: Current levels from all connected clients
- **Alarm Status**: Visual indicators for high/low alarms
- **Last Update**: Timestamp of most recent data from each client
- **System Status**: Server and client connectivity

### Client Configuration
Update client settings remotely via the dashboard:
- Site name and device label
- Server fleet name
- Sample interval and level change threshold
- Tank configurations (up to 8 per client)
- Individual tank alarms and calibration

### Server Configuration
Configure server behavior:
- Server name
- Client fleet name (for routing)
- SMS phone numbers (primary, secondary, tertiary)
- Email recipients for daily reports
- Daily report schedule (time of day)

### API Endpoints

The server exposes a simple REST API:

- `GET /` - Web dashboard (HTML)
- `GET /client-console` - Client configuration console (HTML)
- `GET /serial-monitor` - Server + client serial log viewer (HTML)
- `GET /calibration` - Calibration dashboard (HTML)
- `GET /api/tanks` - Tank records (JSON)
- `GET /api/clients` - Client metadata + cached configs (JSON)
- `GET /api/calibration` - Calibration status (JSON)
- `GET /api/serial-logs?...` - Fetch serial logs (JSON)
- `GET /api/serial-export?...` - Download serial logs (CSV)
- `POST /api/pin` - Set/verify/change admin PIN
- `POST /api/config` - Update server settings and/or push client configuration (PIN required)
- `POST /api/refresh` - Trigger manual refresh of a client (PIN required)
- `POST /api/relay` - Send relay command to a client (PIN required)
- `POST /api/relay/clear` - Clear a relay alarm on a client tank (PIN required if PIN configured)
- `POST /api/pause` - Pause/resume Notecard processing (PIN required if PIN configured)
- `POST /api/ftp-backup` - Backup configs to FTP (PIN required)
- `POST /api/ftp-restore` - Restore configs from FTP (PIN required)

Example: Push config to client
```bash
curl -X POST http://server-ip/api/config \
  -H "Content-Type: application/json" \
  -d '{
    "client": "dev:xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
    "config": {
      "sampleSeconds": 1800,
      "tanks": [
        {
          "id": "A",
          "name": "North Tank",
          "highAlarm": 110.0,
          "lowAlarm": 18.0
        }
      ]
    }
  }'
```

## Configuration

### Server Configuration

Stored in `/server_config.json` on LittleFS. Default values:

```json
{
  "serverName": "Tank Alarm Server",
  "clientFleet": "tankalarm-clients",
  "smsPrimary": "+15555555555",
  "smsSecondary": "+15555555555",
  "smsTertiary": "",
  "emailPrimary": "alerts@example.com",
  "emailSecondary": "",
  "dailyReportHour": 7,
  "dailyReportMinute": 0
}
```

### Client Configuration Cache

The server caches configuration for each client in `/client_configs/<device-uid>.json`. This allows the dashboard to display current client settings.

## Operation

### Data Flow

1. **Clients → Server**:
   - Clients send telemetry, alarms, and daily reports
   - Data routed via Blues Notehub using fleet targeting
   - Server receives notes in `.qi` inbox files

2. **Server Processing**:
   - Polls Notecard for incoming notes every 5 seconds
   - Processes telemetry to update dashboard
   - Handles alarms and triggers SMS/email
   - Aggregates daily reports

3. **Server → Clients**:
   - Configuration updates pushed from web interface
   - Routed via Blues Notehub using device-specific targeting
   - Clients receive and apply configuration immediately

### Alert Processing

When an alarm is received:
1. Server detects alarm in incoming `alarm.qi` note
2. Composes SMS message with tank details
3. Queues SMS for delivery via Blues Notehub
4. Updates dashboard to show alarm status
5. Logs alarm in system memory

### Daily Reports

The server generates daily reports:
1. Aggregates data from all clients at scheduled time
2. Formats email with current levels and changes
3. Queues email for delivery via Blues Notehub
4. Includes alarm history and status summary

## Communication

### Notes Received from Clients

1. **Telemetry** (`telemetry.qi`)
   - Tank levels and sensor data
   - Timestamp and client identifier
   - Used to update dashboard

2. **Alarms** (`alarm.qi`)
   - Immediate notification of threshold breach
   - Tank ID, name, level, threshold
   - Triggers SMS/email alerts

3. **Daily Reports** (`daily.qi`)
   - Once per day summary
   - All tanks and status
   - Used for email reporting

### Notes Sent to Clients

1. **Configuration** (via `command.qo`)
   - Pushed from web interface
   - Sent to `command.qo` with `_target` (device UID) and `_type: "config"` in body
   - ServerToClientRelay route delivers as `config.qi` on the target client
   - Contains updated settings for client

2. **SMS Requests** (`sms.qo`)
   - Outbound SMS via Blues Notehub integration
   - Can be routed to Twilio or other SMS provider

3. **Email Requests** (`email.qo`)
   - Outbound email via Blues Notehub integration
   - Can be routed to SendGrid or other email provider
   - Each note carries a message `id` (left out only if the note would not fit with it); the
     Apps Script bridge on `/email-setup` uses it and the Notehub event ID to skip route
     retries of an email it has already sent

## Troubleshooting

### Cannot Access Dashboard

**Check Network Connection:**
- Verify Ethernet cable is connected
- Check link lights on Ethernet port
- Ensure server obtained IP via DHCP
- Check serial monitor for IP address

**Check Firewall:**
- Allow port 80 through firewall
- Verify computer is on same network/subnet
- Try accessing from different device

**Check Server Status:**
- Serial monitor should show "Server listening on port 80"
- Try pinging the IP address
- Verify power supply is adequate

### Dashboard Shows No Data

**Verify Client Communication:**
- Check that clients have checked in (sample interval)
- Verify fleet configuration in Blues Notehub
- Check Blues Notehub Events for note traffic
- Confirm clients are configured with correct server fleet

**Check Server Processing:**
- Serial monitor shows "Polling for client data..."
- Verify Notecard is connected and syncing
- Check that server is in correct fleet

### Cannot Update Client Config

**Verify Client UID:**
- Copy UID directly from Blues Notehub device page
- Format: `dev:xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx`
- Check for typos or truncation

**Check Notehub Configuration:**
- Verify client is assigned to correct fleet
- Check that client is online and syncing
- Review Events for config note delivery

**Check Network:**
- Ensure server can communicate with Notecard
- Verify I2C connection to Notecard
- Check serial monitor for error messages

### SMS/Email Not Sending

**Configure Downstream Routes:**
- SMS and email require additional Blues Notehub routes
- Route `sms.qo` to Twilio or SMS provider
- Route `email.qo` to SendGrid or email provider
- See Blues Notehub documentation for route setup

**Verify Phone Numbers/Emails:**
- Check format in server configuration
- Phone: +1XXXXXXXXXX format
- Email: valid email addresses
- Test with known good numbers/addresses

## Development

### Serial Console Output
Monitor at 115200 baud for diagnostic information:
- Server startup and IP assignment
- Notecard connection status
- Incoming note processing
- Configuration updates
- Alarm processing
- Error messages

### Memory Usage
- Flash: ~70-85% (server is larger than client)
- RAM: ~50-70% during operation
- LittleFS: <5KB for all configuration files

### Network Configuration

**Default: DHCP**
The server obtains an IP address automatically.

**Static IP (if needed):**
Modify the code before `Ethernet.begin()`:
```cpp
IPAddress ip(192, 168, 1, 100);
IPAddress dns(192, 168, 1, 1);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
Ethernet.begin(mac, ip, dns, gateway, subnet);
```

## Security Considerations

**Intranet Use Only:**
- Read-only pages and `GET` endpoints have no authentication
- Control endpoints are protected by a 4-digit admin PIN once configured
- Designed for trusted local networks only
- Do not expose port 80 to internet
- Use firewall rules to restrict access

**For Internet Access:**
- Implement authentication (not included) and avoid exposing the Opta directly
- Use HTTPS with valid certificates
- Consider VPN for remote access
- Add rate limiting and input validation

## Documentation

- **[INSTALLATION.md](INSTALLATION.md)** - Complete setup guide with Arduino IDE and library installation
- **[FLEET_SETUP.md](FLEET_SETUP.md)** - Simplified fleet-based configuration
- **[WEBSITE_PREVIEW.md](WEBSITE_PREVIEW.md)** - Dashboard features and screenshots

## Additional Resources

- [Arduino Opta Documentation](https://docs.arduino.cc/hardware/opta)
- [Blues Wireless Developer Portal](https://dev.blues.io)
- [ArduinoJson Documentation](https://arduinojson.org/)
- [Arduino Ethernet Library](https://www.arduino.cc/reference/en/libraries/ethernet/)
- [TankAlarm 112025 Client](../TankAlarm-112025-Client-BluesOpta/)

## Support

For issues or questions:
1. Check [INSTALLATION.md](INSTALLATION.md) troubleshooting section
2. Review serial console output at 115200 baud
3. Verify network connectivity (ping test)
4. Check Blues Notehub note traffic
5. Consult Blues Wireless documentation
6. Check GitHub issues in this repository

## License

See [LICENSE](../LICENSE) file in repository root.
