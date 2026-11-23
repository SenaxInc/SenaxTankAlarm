# Firmware Communication Architecture — TankAlarm 112025

## Introduction

This guide explains how the TankAlarm 112025 firmware communicates between Client, Server, and Viewer devices using Blues Notecard and Notehub. If you're modifying the firmware or debugging communication issues, start here.

### What You'll Learn

- How notefiles are named and why direction matters
- How the Route Relay pattern works in firmware
- What each notefile carries
- How `command.qo` consolidates server→client communication
- How the firmware reads inbound notes

---

## Notefile Naming Convention

Every notefile name has two parts: a **base name** and a **direction extension**.

```
telemetry.qo
─────────  ──
  base     direction
```

| Extension | Direction | Used With |
|-----------|-----------|-----------|
| `.qo` | **Outbound** — device sends to Notehub | `note.add` |
| `.qi` | **Inbound** — Notehub delivers to device | `note.get` |

### Golden Rules

1. **`note.add` only accepts `.qo`** (or `.qos`, `.db`, `.dbs`, `.dbx`)
2. **`note.get` reads `.qi`** (or `.qis`, `.db`, `.dbx`)
3. **Never use colons (`:`) in notefile names** — they're invalid and cause firmware errors
4. **Cross-device delivery requires Notehub Routes** — no `device:uid:` or `fleet:name:` prefixes

---

## Notefile Definitions

All notefiles are defined in `TankAlarm_Common.h` with clear outbox/inbox naming:

### Data Notefiles (Client → Server)

| Constant | Value | Sender | Receiver |
|----------|-------|--------|----------|
| `TELEMETRY_OUTBOX_FILE` | `"telemetry.qo"` | Client | — |
| `TELEMETRY_INBOX_FILE` | `"telemetry.qi"` | — | Server |
| `ALARM_OUTBOX_FILE` | `"alarm.qo"` | Client | — |
| `ALARM_INBOX_FILE` | `"alarm.qi"` | — | Server |
| `DAILY_OUTBOX_FILE` | `"daily.qo"` | Client | — |
| `DAILY_INBOX_FILE` | `"daily.qi"` | — | Server |
| `UNLOAD_OUTBOX_FILE` | `"unload.qo"` | Client | — |
| `UNLOAD_INBOX_FILE` | `"unload.qi"` | — | Server |

### Command Notefile (Server → Client)

| Constant | Value | Description |
|----------|-------|-------------|
| `COMMAND_OUTBOX_FILE` | `"command.qo"` | Server sends ALL commands through this single notefile |

The server adds `_target` (device UID) and `_type` (command type) fields to the body. The Notehub Route reads these and delivers to the correct client's `.qi` file.

### Command Types

| `_type` Value | Delivered To | Purpose |
|--------------|-------------|---------|
| `"config"` | `config.qi` | Push configuration to client |
| `"relay"` | `relay.qi` | Relay on/off commands |
| `"serial_request"` | `serial_request.qi` | Request serial log upload |
| `"location_request"` | `location_request.qi` | Request GPS location |

### Serial & Location Notefiles

| Constant | Value | Direction |
|----------|-------|-----------|
| `SERIAL_LOG_OUTBOX_FILE` | `"serial_log.qo"` | Client → Server |
| `SERIAL_LOG_INBOX_FILE` | `"serial_log.qi"` | Server reads |
| `SERIAL_REQUEST_FILE` | `"serial_request.qi"` | Client reads |
| `SERIAL_ACK_OUTBOX_FILE` | `"serial_ack.qo"` | Client → Server |
| `SERIAL_ACK_INBOX_FILE` | `"serial_ack.qi"` | Server reads |
| `LOCATION_RESPONSE_OUTBOX_FILE` | `"location_response.qo"` | Client → Server |
| `LOCATION_RESPONSE_INBOX_FILE` | `"location_response.qi"` | Server reads |
| `LOCATION_REQUEST_FILE` | `"location_request.qi"` | Client reads |

### Relay Forwarding & Config Acknowledgment Notefiles

| Constant | Value | Direction |
|----------|-------|-----------|
| `RELAY_FORWARD_OUTBOX_FILE` | `"relay_forward.qo"` | Client → Server |
| `RELAY_FORWARD_INBOX_FILE` | `"relay_forward.qi"` | Server reads |
| `CONFIG_ACK_OUTBOX_FILE` | `"config_ack.qo"` | Client → Server |
| `CONFIG_ACK_INBOX_FILE` | `"config_ack.qi"` | Server reads |

`config_ack.qo` carries more than config acknowledgments. The server tells the notes apart by the start of their status field `st`, so none of them needs a route of its own:

| `st` | What it is |
|------|------------|
| `applied`, `failed` | Acknowledgment of a config the server sent (`cv` names the config version) |
| `ota-applied`, `ota-reverted`, `ota-stage-failed` | Outcome of a firmware update |
| `chg-check` | Charger check of a client with a SunSaver MPPT: the registers it read and its verdict (see `TankAlarm_SunSaverReport.h`). The client sends it when the charger's stored settings or the verdict have changed, once after it starts, and once a week. |

A server that does not know a status keeps it as the last acknowledgment status of that client and does nothing else with it, so update the server before the clients.

### Viewer Summary

| Constant | Value | Direction |
|----------|-------|-----------|
| `VIEWER_SUMMARY_OUTBOX_FILE` | `"viewer_summary.qo"` | Server → Viewer |
| `VIEWER_SUMMARY_INBOX_FILE` | `"viewer_summary.qi"` | Viewer reads |

---

## Client Firmware: Sending Notes

The client uses short alias defines in its `.ino` file to map outbox names:

```cpp
// Client perspective — outbound data uses .qo
#define TELEMETRY_FILE TELEMETRY_OUTBOX_FILE   // "telemetry.qo"
#define ALARM_FILE     ALARM_OUTBOX_FILE       // "alarm.qo"
#define DAILY_FILE     DAILY_OUTBOX_FILE       // "daily.qo"
#define UNLOAD_FILE    UNLOAD_OUTBOX_FILE      // "unload.qo"
```

### publishNote() — The Core Sending Function

```cpp
static void publishNote(const char *fileName, const JsonDocument &doc, bool syncNow) {
    // fileName is already a plain .qo name (e.g., "telemetry.qo")
    // No fleet: or device: prefix — Route handles cross-device delivery

    J *req = notecard.newRequest("note.add");
    JAddStringToObject(req, "file", fileName);  // Just the plain .qo name
    if (syncNow) {
        JAddBoolToObject(req, "sync", true);
    }
    J *body = JParse(buffer);
    JAddItemToObject(req, "body", body);
    notecard.sendRequest(req);
}
```

### Client-to-Client Relay Forwarding (Server-Mediated)

When a client alarm triggers relays on another client, it sends a `relay_forward.qo` note. The server receives this via Route #1, then re-dispatches to the target client via `command.qo` → Route #2:

```cpp
// Client sends relay forward request to server
JAddStringToObject(req, "file", RELAY_FORWARD_OUTBOX_FILE);  // "relay_forward.qo"
JAddStringToObject(body, "target", targetClientUid);   // Target client UID
JAddStringToObject(body, "client", gDeviceUID);        // Source client UID
JAddNumberToObject(body, "relay", relayNum);
JAddBoolToObject(body, "state", true);
JAddStringToObject(body, "source", "client-alarm");
```

The flow is: Client → `relay_forward.qo` → Route #1 → Server `relay_forward.qi` → Server `handleRelayForward()` → `command.qo` → Route #2 → Target client `relay.qi`.

---

## Server Firmware: Sending Commands

The server consolidates all outbound commands through `command.qo`:

### sendRelayCommand()

```cpp
JAddStringToObject(req, "file", COMMAND_OUTBOX_FILE);  // "command.qo"
JAddStringToObject(body, "_target", clientUid);
JAddStringToObject(body, "_type", "relay");
JAddNumberToObject(body, "relay", relayNum);
JAddBoolToObject(body, "state", state);
```

### sendConfigViaNotecard()

```cpp
JAddStringToObject(req, "file", COMMAND_OUTBOX_FILE);  // "command.qo"
// Parse the config JSON, then inject routing metadata
J *body = JParse(jsonPayload);
JAddStringToObject(body, "_target", clientUid);
JAddStringToObject(body, "_type", "config");
```

### sendLocationRequest()

```cpp
JAddStringToObject(req, "file", COMMAND_OUTBOX_FILE);  // "command.qo"
JAddStringToObject(body, "_target", clientUid);
JAddStringToObject(body, "_type", "location_request");
JAddStringToObject(body, "request", "get_location");
```

---

## Server Firmware: Reading Inbound Notes

The server reads `.qi` notefiles delivered by the ClientToServerRelay Route:

```cpp
static void pollNotecard() {
    processNotefile(TELEMETRY_INBOX_FILE, handleTelemetry);   // "telemetry.qi"
    processNotefile(ALARM_INBOX_FILE, handleAlarm);           // "alarm.qi"
    processNotefile(DAILY_INBOX_FILE, handleDaily);           // "daily.qi"
    processNotefile(UNLOAD_INBOX_FILE, handleUnload);         // "unload.qi"
    processNotefile(SERIAL_LOG_FILE, handleSerialLog);        // "serial_log.qi"
    processNotefile(SERIAL_ACK_FILE, handleSerialAck);        // "serial_ack.qi"
    processNotefile(LOCATION_RESPONSE_FILE, handleLocationResponse);
}
```

### processNotefile() — The Core Reading Function

```cpp
static void processNotefile(const char *fileName, void (*handler)(...)) {
    while (processed < MAX_NOTES_PER_FILE_PER_POLL) {
        J *req = notecard.newRequest("note.get");
        JAddStringToObject(req, "file", fileName);  // e.g., "telemetry.qi"
        JAddBoolToObject(req, "delete", true);       // Remove after reading
        J *rsp = notecard.requestAndResponse(req);
        // ... process the note body ...
    }
}
```

---

## Viewer Firmware: Reading Summary

The Viewer reads from `viewer_summary.qi`, delivered by the ServerToViewerRelay Route:

```cpp
#define VIEWER_SUMMARY_FILE VIEWER_SUMMARY_INBOX_FILE  // "viewer_summary.qi"

static void fetchViewerSummary() {
    J *req = notecard.newRequest("note.get");
    JAddStringToObject(req, "file", VIEWER_SUMMARY_FILE);
    JAddBoolToObject(req, "delete", true);
    // ... process summary data ...
}
```

---

## Complete Communication Flow

```
1. Client reads tank sensor
2. Client calls: note.add("telemetry.qo", body: {tank data})
3. Notecard syncs to Notehub
4. ClientToServerRelay Route fires:
   POST /v1/projects/.../devices/SERVER_UID/notes/telemetry.qi
   Body: {tank data}
5. Server's Notecard receives telemetry.qi note
6. Server calls: note.get("telemetry.qi", delete: true)
7. Server processes tank data, updates dashboard

8. If alarm detected, server calls:
   note.add("command.qo", body: {_target: "dev:CLIENT", _type: "relay", relay: 1, state: true})
9. ServerToClientRelay Route fires:
   POST /v1/projects/.../devices/CLIENT_UID/notes/relay.qi
   Body: {relay: 1, state: true}
10. Client's Notecard receives relay.qi note
11. Client calls: note.get("relay.qi", delete: true)
12. Client activates relay #1
```

---

## Migration from Old Patterns

If you're upgrading from pre-v1.0 firmware that used `fleet:` or `device:` prefixes:

| Old Pattern | New Pattern |
|------------|------------|
| `fleet:tankalarm-server:telemetry.qi` | `telemetry.qo` (plain name) |
| `device:dev:123456:config.qi` | `command.qo` with `_target` in body |
| `device:dev:123456:relay.qi` | `command.qo` with `_target` + `_type: "relay"` |

**Key changes:**
1. Remove ALL `fleet:` and `device:` prefixes from `note.add` calls
2. Use `.qo` for outbound, `.qi` for inbound
3. Use `command.qo` + `_target`/`_type` for targeted delivery
4. Set up Notehub Routes to handle cross-device delivery

---

*TankAlarm 112025 — Route Relay Architecture*
