# TankAlarm Firmware Update Guide

**Updating Your TankAlarm System Over-The-Air Using Blues Notecard DFU**

---

## Introduction

> ℹ️ **v1.9.1+ update model (important):** As of v1.9.1, the **Client** receives over-the-air
> updates through the Arduino **MCUboot** bootloader, which adds atomic A/B image swap and automatic
> rollback. Each Client must be **provisioned once over USB** before it can accept OTA updates — see
> the [Client MCUboot Provisioning Guide](CLIENT_MCUBOOT_PROVISIONING_GUIDE.md). The **Server** and
> **Viewer** are updated **locally over USB** with their raw `.bin` (they are not OTA targets), since
> both are physically accessible. The general DFU concepts below still apply to how firmware reaches
> the Client through Notehub.

The TankAlarm 112025 system uses the Blues Notecard's Device Firmware Update (DFU) feature to deliver firmware over the cellular link. Update support by component:

- **Client** - Field monitoring devices → **OTA via MCUboot** (provisioned, signed `.slot.bin`)
- **Server** - Central aggregation and web dashboard → **USB `.bin`** (local)
- **Viewer** - Read-only kiosk displays → **USB `.bin`** (local)

With OTA, you can deploy Client firmware updates in the field without physical access. Updates are delivered via cellular connection through Blues Notehub, and devices automatically check for updates every hour.

### Why Use OTA Updates?

- **Remote Deployment** - Update devices in the field without site visits
- **Fleet Management** - Deploy to multiple devices simultaneously
- **Rollback Support** - Revert to previous firmware if needed
- **Cost Savings** - Eliminate truck rolls for firmware updates
- **Minimal Downtime** - Devices update and restart automatically

### What You'll Need

- Active Blues Notehub account ([notehub.io](https://notehub.io))
- Your compiled firmware binary (`.bin` file)
- Device(s) online and connected to Notehub
- Arduino IDE or PlatformIO for compiling firmware

### Suggested Reading

Before starting, familiarize yourself with these concepts:

- [Blues Notecard Quickstart](https://dev.blues.io/quickstart/) - Understanding the Blues ecosystem
- [Arduino Compilation Guide](../../CODE%20REVIEW/ARDUINO_COMPILATION_GUIDE.md) - How to compile your firmware
- [Blues Notehub Basics](https://dev.blues.io/guides-and-tutorials/notecard-guides/understanding-notehub/) - Navigating the Notehub interface

---

## How DFU Works

### The Update Process

1. **Upload Firmware** - You upload a compiled `.bin` file to Notehub
2. **Target Devices** - Specify which devices should receive the update
3. **Automatic Discovery** - Devices check for updates every hour
4. **Secure Download** - Notecard downloads firmware over cellular
5. **Host MCU Reset** - Notecard resets the Arduino Opta to apply update
6. **Verification** - New firmware boots and reports version to Notehub

### Update States

During the update process, devices transition through these states:

| State | Description |
|-------|-------------|
| `idle` | No update available |
| `ready` | Update available, waiting for enablement |
| `downloading` | Notecard is downloading firmware from Notehub |
| `download-pending` | Download queued but not started |
| `completed` | Update successfully applied |

### Automatic vs. Manual Updates

**Automatic Mode (Default)**
- `DFU_AUTO_ENABLE` is `true` in TankAlarm_Common.h
- Devices automatically download and install updates
- Recommended for production deployments

**Manual Mode**
- `DFU_AUTO_ENABLE` is `false`
- Server provides web API for manual triggering
- Useful for controlled rollouts or testing

---

## Step 1: Prepare Your Firmware

### Version Your Firmware

Before compiling, update the version number in `TankAlarm-112025-Common/src/TankAlarm_Common.h`:

```cpp
#define FIRMWARE_VERSION "1.1.2"
#define FIRMWARE_BUILD_DATE __DATE__
```

**Version Naming Conventions:**
- Use semantic versioning: `MAJOR.MINOR.PATCH`
- Increment MAJOR for breaking changes
- Increment MINOR for new features
- Increment PATCH for bug fixes

Example progression: `1.0.0` → `1.0.1` → `1.1.0` → `1.1.2` → `2.0.0`

### Compile the Firmware

#### Using Arduino IDE

1. Open your `.ino` file (Client, Server, or Viewer)
2. Select **Sketch → Export Compiled Binary** (or press `Ctrl+Alt+S`)
3. Wait for compilation to complete
4. Find the `.bin` file in your sketch folder:
   ```
   TankAlarm-112025-Client-BluesOpta/
   ├── TankAlarm-112025-Client-BluesOpta.ino.bin
   ```

#### Using PlatformIO

1. Open your project in VS Code
2. Run the build command:
   ```bash
   pio run --target buildfs
   ```
3. Locate the binary in `.pio/build/opta/`:
   ```
   .pio/build/opta/firmware.bin
   ```

### Verify the Binary

**Important Checks:**
- File size should be reasonable (typically 200KB - 1MB)
- Verify version string is updated
- Test on a single device before fleet deployment

**Quick Test (Optional but Recommended):**
Upload to one device via USB and verify it boots correctly before deploying via DFU.

---

## Step 2: Upload Firmware to Notehub

### Access the Firmware Panel

1. Log in to [Notehub.io](https://notehub.io)
2. Navigate to your project
3. Click **Firmware** in the left sidebar

![Notehub Firmware Panel](https://dev.blues.io/assets/images/firmware-panel.png)

### Upload Your Binary

1. Click the **Upload Firmware** button (top right)
2. Fill in the form:

   | Field | Example | Notes |
   |-------|---------|-------|
   | **Firmware Type** | `host` | Always use `host` for Arduino Opta firmware |
   | **Version** | `1.1.2` | Must match `FIRMWARE_VERSION` in your code |
   | **Description** | `Bug fixes for sensor timeout` | Optional but recommended |
   | **Target** | `Product` | Update all devices, or select specific ones |
   | **File** | `TankAlarm-...-Client.ino.bin` | Your compiled binary |

3. Click **Upload Firmware**
4. Wait for upload to complete (progress bar will show 100%)

### Firmware Upload Tips

**Naming Convention:**
- Use descriptive filenames: `TankAlarm-Client-v1.1.2-2026-02-20.bin`
- Include component type (Client/Server/Viewer)
- Include version and date for tracking

**File Size Limits:**
- Maximum file size: 1.5 MB
- Compress images and optimize code if needed
- Remove debug symbols for production builds

---

## Step 3: Target Your Devices

### Deployment Strategies

#### Option 1: Product-Wide Deployment (All Devices)

Best for: Production updates that have been tested

1. In the firmware upload form, select **Target: Product**
2. All devices in your product will receive the update
3. Devices update on their next hourly check

#### Option 2: Specific Device(s)

Best for: Testing, staged rollouts, or individual fixes

1. Select **Target: Devices**
2. Enter Device UIDs (one per line):
   ```
   dev:864475044123456
   dev:864475044234567
   dev:864475044345678
   ```
3. Only listed devices will receive the update

#### Option 3: Fleet/Tag-Based Deployment

Best for: Multi-site deployments, regional updates

1. Organize devices using Notehub Fleets or Tags
2. Select **Target: Fleet** and choose your fleet
3. All devices in the fleet receive the update

### Device Targeting Best Practices

**Staged Rollout:**
1. Deploy to 1-2 test devices first
2. Monitor for 24 hours
3. Expand to 10% of fleet
4. Monitor for 48 hours
5. Deploy to remaining devices

**Site-by-Site:**
- Tag devices by location: `site:north-warehouse`
- Update one site at a time
- Verify functionality before next site

---

## Step 4: Monitor the Update

### Check Update Status

#### Via Notehub Dashboard

1. Go to **Devices** in Notehub
2. Click on a device to view details
3. Scroll to **Firmware** section
4. Check the **DFU Status** field:
   - `downloading` - Update in progress
   - `completed` - Update successful
   - `error` - Update failed (check logs)

#### Via Device Serial Monitor (If Accessible)

Connect to the device via USB and open serial monitor (115200 baud):

```
Checking for firmware update...
Firmware update available: 1.1.2
Auto-enabling DFU...
Enabling DFU mode for version: 1.1.2
DFU mode enabled. Device will reset after download.
```

#### Via Server Web Dashboard (Server Component Only)

The Server component provides web endpoints for DFU monitoring:

**Check DFU Status:**
```
GET http://your-server-ip/api/dfu/status
```

Response:
```json
{
  "available": true,
  "version": "1.1.2",
  "inProgress": false
}
```

### Update Timeline

Typical update timeline for a device:

| Time | Event |
|------|-------|
| T+0 | Firmware uploaded to Notehub |
| T+0-60 min | Device checks for update (hourly interval) |
| T+60-65 min | Notecard downloads firmware (~2-5 minutes) |
| T+65 min | Host MCU resets and boots new firmware |
| T+66 min | New firmware reports version to Notehub |

**Total Time:** Typically 60-70 minutes from upload to completion

### Monitoring Multiple Devices

**Notehub Events View:**
1. Go to **Events** tab
2. Filter by event type: `dfu.qo`
3. Monitor download progress across fleet
4. Look for `host.complete` events indicating success

**Expected Event Sequence:**
```
1. _dfu.qo (query firmware available)
2. dfu.status (report ready state)
3. dfu.mode (enable download)
4. host.complete (update finished)
```

---

## Step 5: Verify the Update

### Confirm New Version

After the update completes, verify the new firmware is running:

#### Method 1: Serial Monitor

Connect via USB and check startup messages:

```
Tank Alarm Client 112025 v1.1.2 (Feb 20 2026)
```

#### Method 2: Server Web Dashboard

For Server components, check the footer or system info page:

```html
<footer>
  TankAlarm Server v1.1.2 | Built: Feb 20 2026
</footer>
```

#### Method 3: Serial Monitor FIRMWARE_VERSION

The firmware version is compiled into each binary. Check the serial output at startup:

```
Firmware Version: 1.1.2
```

> **Note:** The firmware version is embedded at compile time via `FIRMWARE_VERSION` in `TankAlarm_Common.h` — it is not set via Notehub environment variables.

### Post-Update Health Checks

**For Client Devices:**
- ✓ Sensors reporting data
- ✓ Telemetry syncing to server
- ✓ Alarm states functioning
- ✓ No excessive restarts

**For Server:**
- ✓ Web dashboard accessible
- ✓ Receiving client data
- ✓ FTP backup/restore working
- ✓ API endpoints responding

**For Viewer:**
- ✓ Dashboard displaying data
- ✓ Auto-refresh working
- ✓ Summary fetching from Notecard
- ✓ Display rendering correctly

---

## Troubleshooting

### Update Not Starting

**Symptoms:** Device shows `idle` state, never transitions to `downloading`

**Possible Causes:**
1. Device not connected to Notehub
2. Firmware not targeted to this device
3. Hourly check hasn't occurred yet

**Solutions:**
- Verify cellular connection (check Notehub Events for recent activity)
- Confirm device UID matches target list
- Wait for next hourly check, or restart device to trigger immediate check
- Check that `DFU_CHECK_INTERVAL_MS` is set correctly (default: 3600000 = 1 hour)

### Download Fails or Stalls

**Symptoms:** Device stuck in `downloading` state for >10 minutes

**Possible Causes:**
1. Poor cellular signal
2. Large firmware file
3. Notecard buffer overflow

**Solutions:**
- Check cellular signal strength (RSSI) in Notehub
- Reduce firmware size by removing debug code
- Move device to location with better signal
- Restart device and retry

### Device Won't Boot After Update

**Symptoms:** Device resets continuously, no serial output

**Possible Causes:**
1. Corrupted firmware binary
2. Wrong board type selected during compilation
3. Missing dependencies

**Solutions:**
- Re-upload firmware via USB to recover
- Verify Arduino IDE board selection: **Arduino Opta**
- Check that all libraries are installed and up-to-date
- Compile with verbose output to check for errors

### Version Mismatch

**Symptoms:** Device reports old version after update

**Possible Causes:**
1. Forgot to update `FIRMWARE_VERSION` constant
2. Wrong binary uploaded
3. Update didn't actually complete

**Solutions:**
- Verify version string in source code matches Notehub
- Check Events log for `host.complete` confirmation
- Re-upload correct binary to Notehub

### Manual DFU Trigger Not Working (Server Only)

**Symptoms:** POST to `/api/dfu/enable` returns error

**Possible Causes:**
1. Missing or incorrect PIN
2. No update available
3. Update already in progress

**Solutions:**
- Verify PIN is correct (check `config.json` on Server)
- Call `/api/dfu/status` first to confirm update available
- Wait for current update to complete before retriggering

---

## Advanced Topics

### Rollback to Previous Firmware

If the new firmware has issues, you can rollback:

1. Go to **Firmware** panel in Notehub
2. Find the previous working version
3. Click **Re-deploy** next to that version
4. Target affected devices
5. Devices will download the older firmware on next check

**Rollback Timeline:** Same as regular update (~60-70 minutes)

### Disabling Automatic Updates

To prevent automatic updates (for testing or controlled rollouts):

1. Edit `TankAlarm-112025-Common/src/TankAlarm_Common.h`:
   ```cpp
   #define DFU_AUTO_ENABLE false  // Changed from true
   ```

2. Recompile and deploy firmware
3. Updates will require manual trigger via Server API

**Manual Trigger (Server Only):**
```bash
curl -X POST http://server-ip/api/dfu/enable \
     -H "Content-Type: application/json" \
     -d '{"pin": "your-admin-pin"}'
```

### Multi-Component Updates

When updating a system with Client, Server, and Viewer:

**Recommended Order:**
1. **Update Server first** - Ensures backward compatibility
2. **Update Clients** - May have new telemetry fields
3. **Update Viewers last** - Read-only, least critical

**Coordination:**
- Upload all three binaries to Notehub at once
- Use different version numbers if needed (e.g., `1.1.2-client`, `1.1.2-server`)
- Monitor Server update completion before starting Client updates

### Custom Update Intervals

Change hourly check frequency by editing `TankAlarm_Common.h`:

```cpp
// Default: 1 hour (3600000 ms)
#define DFU_CHECK_INTERVAL_MS 1800000UL  // 30 minutes

// For testing: 5 minutes
#define DFU_CHECK_INTERVAL_MS 300000UL
```

**Note:** Shorter intervals increase cellular data usage. For production, 1-4 hours is recommended.

### Firmware Signing and Security

Blues Notecard DFU includes built-in security features:

- **TLS Encryption** - All firmware downloads use HTTPS
- **Authenticated Sources** - Only Notehub can provide firmware
- **Device Validation** - Notecard verifies device UID before download
- **Secure Storage** - Firmware stored in Notecard's protected memory

**No Additional Configuration Required** - These features are automatic.

---

## Best Practices

### Development Workflow

1. **Local Testing First**
   - Always test new firmware on bench device via USB
   - Verify all features work before compiling for DFU
   - Run for 24+ hours to check stability

2. **Version Control**
   - Commit code before compiling release binary
   - Tag releases in Git: `git tag v1.1.2`
   - Keep binary archives with matching Git commits

3. **Staged Deployment**
   - Test device → Small pilot group → Full fleet
   - Wait 24-48 hours between stages
   - Monitor Notehub Events for errors

4. **Documentation**
   - Update CHANGELOG.md with changes
   - Document breaking changes or configuration updates
   - Note any required config.json changes

### Firmware Hygiene

**Before Every Release:**
- [ ] Update `FIRMWARE_VERSION` constant
- [ ] Update `FIRMWARE_BUILD_DATE` (auto-updated via `__DATE__`)
- [ ] Test compilation for all three components
- [ ] Verify binary size is reasonable (<1 MB)
- [ ] Check for compiler warnings
- [ ] Update relevant documentation

### Fleet Management

**Device Organization:**
- Use Notehub Fleets for geographic grouping
- Tag devices by role: `role:client`, `role:server`, `role:viewer`
- Document device assignments in spreadsheet

**Monitoring:**
- Set up Notehub Routes for update notifications
- Monitor cellular data usage (firmware downloads ~500KB each)
- Track update success rate per firmware version

---

## Resources and Going Further

### Related Documentation

- [Blues Notecard DFU Documentation](https://dev.blues.io/guides-and-tutorials/notecard-guides/device-firmware-update-dfu/) - Official Blues DFU guide
- [TankAlarm Security and Advanced Features](../../CODE%20REVIEW/SECURITY_AND_ADVANCED_FEATURES.md) - Full security documentation
- [Arduino Compilation Guide](../../CODE%20REVIEW/ARDUINO_COMPILATION_GUIDE.md) - Detailed compilation instructions

### Source Code

- [TankAlarm Client](../../TankAlarm-112025-Client-BluesOpta/) - Field monitoring firmware
- [TankAlarm Server](../../TankAlarm-112025-Server-BluesOpta/) - Central server firmware
- [TankAlarm Viewer](../../TankAlarm-112025-Viewer-BluesOpta/) - Kiosk display firmware
- [TankAlarm Common](../../TankAlarm-112025-Common/) - Shared library and version constants

### Support

**Technical Assistance:**
- Blues.io Community Forum: [community.blues.io](https://community.blues.io)
- GitHub Issues: [SenaxInc/SenaxTankAlarm](https://github.com/SenaxInc/SenaxTankAlarm/issues)

**Blues Resources:**
- [Notehub Documentation](https://dev.blues.io/reference/notehub-api/api-introduction/)
- [Notecard API Reference](https://dev.blues.io/api-reference/notecard-api/introduction/)
- [Blues Quickstart Tutorials](https://dev.blues.io/quickstart/)

---

## Appendix: Quick Reference Commands

### DFU Status Check (via Serial Console)

View current DFU state by sending this request manually:

```cpp
{"req":"dfu.status"}
```

Response when update available:
```json
{
  "mode": "ready",
  "body": "1.1.2",
  "on": false
}
```

### DFU Enable (Manual Trigger)

Force immediate download:

```cpp
{"req":"dfu.mode", "on":true}
```

### Check Current Firmware Version

Check serial monitor output at startup — the firmware version is compiled in:

```
Firmware Version: 1.1.2
```

### Server API Endpoints

**Get DFU Status:**
```bash
curl http://server-ip/api/dfu/status
```

**Enable DFU (requires PIN):**
```bash
curl -X POST http://server-ip/api/dfu/enable \
     -H "Content-Type: application/json" \
     -d '{"pin": "your-admin-pin"}'
```

---

## Conclusion

Blues Notecard DFU provides a robust, secure way to keep your TankAlarm fleet up-to-date without physical access. By following this guide, you can confidently deploy firmware updates to devices in the field, rollback if needed, and maintain a healthy, modern fleet.

**Key Takeaways:**
- Devices automatically check for updates every hour
- Updates take ~60-70 minutes from upload to completion
- Always test on a single device before fleet deployment
- Use staged rollouts for large deployments
- Monitor Notehub Events for update progress

Happy updating! 🚀

---

*Last Updated: February 20, 2026*  
*Firmware Version: 1.1.2+*  
*Compatible with: TankAlarm-112025-Client, Server, and Viewer*
