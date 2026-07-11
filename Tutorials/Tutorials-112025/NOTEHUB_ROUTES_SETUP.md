# Notehub Routes Setup Guide — TankAlarm 112025 v1.1.2

## Introduction

Welcome! In this tutorial you'll set up the **Notehub Routes** that allow your TankAlarm devices to talk to each other through the Blues cloud. By the end, your Clients, Server, and Viewer nodes will all be communicating reliably — no SSL certificates needed on your Opta hardware.

### What You'll Learn

- How Blues Notecard notefile routing works
- How to create the 5 Notehub Routes that power TankAlarm
- How to verify each route is working
- How to troubleshoot common issues

### What You'll Need

| Item | Notes |
|------|-------|
| Notehub account | Free at [notehub.io](https://notehub.io) |
| TankAlarm project | Already created in Notehub |
| Notehub Personal Access Token | Created from your profile menu → API Access (for route authentication) |
| All devices provisioned | Client, Server, and Viewer Notecards already connected to Notehub |

> **Time to complete:** ~30 minutes

---

## Background: How Blues Notecard Routing Works

Before we set up routes, let's understand the key rules of Blues Notecard communication:

### Notefile Naming Rules

| Extension | Direction | API | Example |
|-----------|-----------|-----|---------|
| `.qo` | **Outbound** (device → Notehub) | `note.add` | `telemetry.qo` |
| `.qi` | **Inbound** (Notehub → device) | `note.get` | `config.qi` |

**Critical rules:**
- `note.add` **only** accepts `.qo`, `.qos`, `.db`, `.dbs`, or `.dbx` as the `file` parameter
- `note.get` reads from `.qi`, `.qis`, `.db`, or `.dbx` files
- **Colons (`:`) are never allowed** in notefile names — they will cause firmware errors
- A device cannot directly write to another device's `.qi` file — that's what Routes are for!

### The Route Relay Pattern

Since devices can't write directly to each other, we use **Notehub Routes** as the relay:

```
┌─────────┐   note.add     ┌──────────┐   Route (HTTP)   ┌──────────┐   note.get
│  Client  │ ──────────────>│  Notehub │ ────────────────>│  Notehub │ ──────────>│  Server  │
│  Device  │  telemetry.qo  │  Cloud   │  Device API POST │  Cloud   │  telemetry.qi
└─────────┘                 └──────────┘                  └──────────┘
```

1. The **Client** calls `note.add` with `file: "telemetry.qo"`
2. The note syncs to **Notehub** cloud
3. A **Route** catches the event and uses the Notehub Device API to add a `.qi` note to the **Server** device
4. The **Server** calls `note.get` with `file: "telemetry.qi"` and reads the data

This is the "Route Relay" pattern — and it's the foundation of TankAlarm's communication.

---

## Architecture Overview

TankAlarm uses **5 routes** total:

| # | Route Name | Purpose | Trigger | Target |
|---|-----------|---------|---------|--------|
| 1 | **ClientToServerRelay** | Delivers client telemetry/alarms (and viewer requests) to server | 11 specific `.qo` notefiles | Server device `.qi` inbox |
| 2 | **ServerToClientRelay** | Delivers server commands to clients | `command.qo` from server device | Target client's `.qi` inbox |
| 3 | **ServerToViewerRelay** | Delivers viewer summary to viewer devices | `viewer_summary.qo` from server | Viewer device `.qi` inbox |
| 4 | **SMSRoute** | Sends SMS alerts | `sms.qo` from server device (one note per recipient) | Twilio, To = `[body.to]` |
| 5 | **EmailRoute** | Sends email reports & alerts | `email.qo` from server device | General HTTP → email API (SendGrid) or Google Workspace Apps Script |

### Notefile Flow Diagram

```
CLIENT DEVICE                    NOTEHUB                         SERVER DEVICE
─────────────                    ───────                         ─────────────
telemetry.qo  ──> Route #1 ──>  telemetry.qi  ──> note.get
alarm.qo      ──> Route #1 ──>  alarm.qi      ──> note.get
daily.qo      ──> Route #1 ──>  daily.qi      ──> note.get
unload.qo     ──> Route #1 ──>  unload.qi     ──> note.get
serial_log.qo ──> Route #1 ──>  serial_log.qi ──> note.get
serial_ack.qo ──> Route #1 ──>  serial_ack.qi ──> note.get
config_ack.qo ──> Route #1 ──>  config_ack.qi ──> note.get
location_response.qo ─> #1 ──>  location_response.qi ──> note.get
relay_forward.qo ──> #1 ──────> relay_forward.qi ──> handleRelayForward()
                                                        │
                                                        ▼
                                 command.qo    <── note.add (server re-issues)
                                     │               ▲
                                     │               │
                                     │         Also used directly for:
                                     │         config, serial_request,
                                     │         location_request, relay
                                     │
                                     └──> Route #2 reads body._target and body._type
                                          │
CLIENT DEVICE                             │
─────────────                             ▼
config.qi          <──────────────  Delivers to target client
relay.qi           <──────────────  based on _type field
serial_request.qi  <──────────────
location_request.qi <─────────────

VIEWER DEVICE                    NOTEHUB                         SERVER DEVICE
─────────────                    ───────                         ─────────────
viewer_summary.qi <── Route #3   viewer_summary.qo <── note.add (server)
viewer_contacts.qo ──> Route #1 ──> viewer_contacts.qi ──> handleViewerContacts()
viewer_request.qo  ──> Route #1 ──> viewer_request.qi  ──> publishViewerSummary()

                                 sms.qo   ──> Route #4 ──> Twilio SMS
                                 email.qo ──> Route #5 ──> Email API (SendGrid etc.)
```

### Relay Forwarding Flow (Client → Server → Client)

When a client alarm triggers relays on a remote client, the command flows through the server:

```
CLIENT A (alarm)                 SERVER                          CLIENT B (target)
────────────────                 ──────                          ────────────────
alarm triggers            relay_forward.qi                       relay.qi
relay_forward.qo ──>      handleRelayForward()                   pollForRelayCommands()
  Route #1          ──>     reads target, relay, state    ──>     activates relay
                            re-issues via command.qo
                              Route #2 delivers
```

This ensures the server has full visibility into all relay commands and can log/audit them.

---

## Step 1: Create a Personal Access Token

Routes #1, #2, and #3 use the Notehub Device API to add notes to specific devices. This requires authentication via a **Personal Access Token (PAT)**.

1. Log in to [notehub.io](https://notehub.io)
2. Click your **profile icon** (top right) → **API Access**
3. Click **Create New Token**
4. Name it: `TankAlarm Route Relay`
5. Set the **expiration** to the longest available duration
6. Copy the token — you'll need it for the routes below

> **⚠️ Important: Do NOT use Programmatic API Access (OAuth).** The Client ID / Client Secret found under Project Settings → Programmatic API Access generates OAuth tokens that **expire every 30 minutes** — they are useless for routes that need to work 24/7. Always use a **Personal Access Token** for route authentication.

> **⚡ Tip:** Store this token securely. You can always regenerate it, but all routes using the old token will break. Set a calendar reminder before the token expires to create a replacement.

---

## Step 2: Note Your Device UIDs

You'll need the Notecard device UIDs for your Server and Viewer devices. Find them in Notehub:

1. Go to your **TankAlarm project** in Notehub
2. Click **Devices** in the left sidebar
3. Note the **Device UID** for each device (format: `dev:XXXXXXXXXXXX`)

> **⚠️ Case-sensitive:** Device UIDs must use lowercase `dev:` — not `Dev:` or `DEV:`. Notehub treats these as different devices. A route configured with `Dev:XXXXXXXXXXXX` will return a **404 "could not find device"** error even though `dev:XXXXXXXXXXXX` exists. Always copy-paste the UID directly from the Notehub Devices page to avoid capitalization mistakes.

Write them down:

| Role | Device UID | Fleet |
|------|-----------|-------|
| Server | `dev:_____________` | `tankalarm-server` |
| Viewer | `dev:_____________` | `tankalarm-viewer` |
| Client 1 | `dev:_____________` | `tankalarm-clients` (or your client fleet) |
| Client 2 | `dev:_____________` | `tankalarm-clients` (or your client fleet) |

> **⚡ Note:** The viewer firmware automatically joins the `tankalarm-viewer` fleet via `hub.set`. Make sure this fleet exists in your Notehub project before provisioning viewer devices.

---

## Step 3: Create Route #1 — ClientToServerRelay

This route catches all `.qo` events from client devices and delivers them as `.qi` notes to the server.

### 3a. Navigate to Routes

1. In your Notehub project, click **Routes** in the left sidebar
2. Click **Create Route**
3. Select **General HTTP/HTTPS Request/Response**

### 3b. Configure the Route

| Setting | Value |
|---------|-------|
| **Route Name** | `ClientToServerRelay` |
| **URL** | `https://api.notefile.net/v1/projects/YOUR_PROJECT_UID/devices/YOUR_SERVER_DEVICE_UID/notes/[filebase].qi` |
| **Additional Headers** | Header Name: `X-SESSION-TOKEN`   Header Value: `YOUR_API_TOKEN` |

Replace the placeholders:
- `YOUR_PROJECT_UID` → Your Notehub project UID (found in project settings, format: `app:XXXX`)
- `YOUR_SERVER_DEVICE_UID` → The server's device UID (e.g., `dev:123456789012`)
- `YOUR_API_TOKEN` → The Personal Access Token from Step 1 (**paste the raw token only — do not include `Bearer `**)

> **Headers Tip:** Leave the Headers dropdown set to **Additional Headers**. The default `Content-Type: application/json` is sent automatically — you only need to add the `X-SESSION-TOKEN` header.

### 3c. Configure the Data Transform

In the **Data** section, select **JSONata Expression** from the **Transform Data** dropdown, then enter:

```jsonata
{
    "body": body
}
```

This passes the entire event body through to the server's `.qi` notefile.

### 3d. Configure Event Filters

| Setting | Value |
|---------|-------|
| **Fleets** | Select your **client fleet** only |
| **Notefiles** | `telemetry.qo`, `alarm.qo`, `daily.qo`, `unload.qo`, `serial_log.qo`, `serial_ack.qo`, `config_ack.qo`, `location_response.qo`, `relay_forward.qo`, `viewer_contacts.qo`, `viewer_request.qo` |

> **Viewer notefiles (v2.2.0+):** `viewer_contacts.qo` (viewer-managed alarm contacts) and
> `viewer_request.qo` (dashboard "Request Update" button) originate from the **viewer**
> device, not a client. The route matches on notefile name, so simply adding them to this
> route's Selected Notefiles list is enough — but if you scoped this route to the client
> fleet, either widen it to all devices or duplicate it for the `tankalarm-viewer` fleet.

> **⚡ Tip:** The `[filebase]` placeholder variable is a reserved Blues Notehub string that strips the file extension and any characters after the first dot in the Notefile name. So `telemetry.qo` becomes `telemetry`, `relay_forward.qo` becomes `relay_forward`, and so on. The URL appends `.qi` — resulting in the note being added to (e.g.) `telemetry.qi` on the server. See the [Blues placeholder docs](https://dev.blues.io/notehub/notehub-walkthrough/#using-placeholder-variable-substitution-interpolation) for the full list of reserved placeholder strings.

> **⚡ Important:** Do not include `config.qo` — it does not exist. Config is delivered to clients via `command.qo` with `_type: config`, and clients acknowledge via `config_ack.qo`.

### 3e. Timeout and Retry Settings

Before saving, configure these two settings (they apply to **all 5 TankAlarm routes**):

- **Timeout:** Leave at the default (30 seconds). Routes #1–3 target `api.notefile.net` (Blues' own infrastructure) and respond in under a second. Routes #4–5 (Twilio/SMTP) are equally fast under normal conditions.
- **Automatic reroute on failure:** **Enable this** (check the box). If a route fails transiently (e.g., a brief 5XX from the server), Notehub will retry at 5-second, 1-minute, and 5-minute intervals (3 retries max). This prevents lost telemetry, dropped config pushes, or missed alarm SMS due to momentary glitches. The retry overhead is negligible at TankAlarm's event volume.

> **Note:** Automatic reroute only helps with transient failures (5XX, timeouts). It will **not** fix 4XX errors (bad API token, wrong device UID, malformed JSON) — those indicate a configuration problem. If you see 401 or 404 in Route Logs, fix the route settings rather than relying on retries.

### 3f. Save and Test

1. Click **Save Route**
2. Send a test telemetry note from a client device
3. Check the **Route Logs** to verify delivery
4. Check the server's **Events** page to see the `.qi` note appear

---

## Step 4: Create Route #2 — ServerToClientRelay

This is the most sophisticated route. The server sends ALL commands through a single `command.qo` notefile, and the route reads metadata in the body to determine which client and which `.qi` file to deliver to.

### 4a. Create the Route

1. Click **Routes** → **Create Route** → **General HTTP/HTTPS Request/Response**

| Setting | Value |
|---------|-------|
| **Route Name** | `ServerToClientRelay` |
| **URL** | See dynamic URL below |
| **Additional Headers** | Header Name: `X-SESSION-TOKEN`   Header Value: `YOUR_API_TOKEN` |

### 4b. Configure the Dynamic URL

The URL must be **dynamic** because each command targets a different client device and notefile. Notehub supports **placeholder variables** in the URL field — these are evaluated at route time using data from the event.

Paste this into the **URL** field:

```
https://api.notefile.net/v1/projects/YOUR_PROJECT_UID/devices/[.body._target]/notes/[.body._type].qi
```

Replace `YOUR_PROJECT_UID` with your actual project UID (e.g., `app:XXXXXXXXXXXX`).

**How it works:** The server firmware sets `_target` (device UID) and `_type` (command type) in every `command.qo` note. Notehub substitutes `[.body._target]` with the target device UID and `[.body._type]` with the command type, producing URLs like:
- `…/devices/dev:123456/notes/config.qi`
- `…/devices/dev:789012/notes/relay.qi`
- `…/devices/dev:123456/notes/serial_request.qi`

> **Placeholder syntax:** `[.body.fieldname]` reads a field from the event body. See the [Blues placeholder docs](https://dev.blues.io/notehub/notehub-walkthrough/#using-placeholder-variable-substitution-interpolation) for details.

### 4c. Configure the Data Transform

In the **Data** section, select **JSONata Expression** from the **Transform Data** dropdown. Use JSONata to strip the routing metadata and pass only the command payload:

```jsonata
{
    "body": $merge([body, {"_target": $nothing, "_type": $nothing}])
}
```

This removes `_target` and `_type` from the body before delivering to the client — the client doesn't need to see routing metadata.

### 4d. Configure Event Filters

| Setting | Value |
|---------|-------|
| **Fleets** | Select your **server fleet** only |
| **Notefiles** | `command.qo` |

### 4e. Save and Test

1. Click **Save Route**
2. From the server dashboard, send a config update to a client
3. Check **Route Logs** — you should see the dynamic URL targeting the correct client device
4. On the client, verify the `config.qi` note arrived

---

## Step 5: Create Route #3 — ServerToViewerRelay

This route delivers the viewer summary from the server to all viewer devices.

### 5a. Create the Route

| Setting | Value |
|---------|-------|
| **Route Name** | `ServerToViewerRelay` |
| **URL** | `https://api.notefile.net/v1/projects/YOUR_PROJECT_UID/devices/YOUR_VIEWER_DEVICE_UID/notes/viewer_summary.qi` |
| **Additional Headers** | Header Name: `X-SESSION-TOKEN`   Header Value: `YOUR_API_TOKEN` |

### 5b. Data Transform

In the **Data** section, select **JSONata Expression** from the **Transform Data** dropdown, then enter:

```jsonata
{
    "body": body
}
```

### 5c. Event Filters

| Setting | Value |
|---------|-------|
| **Fleets** | Select your **server fleet** only |
| **Notefiles** | `viewer_summary.qo` |

### 5d. Multiple Viewers

If you have multiple viewer devices, you need **one route per viewer** — duplicate this route for each viewer device UID, changing only the `YOUR_VIEWER_DEVICE_UID` in the URL.

> **Why not fleet-level delivery?** The Notehub Device API (`/v1/projects/{project}/devices/{device}/notes/{notefile}`) requires a specific device UID. There is no fleet-level broadcast endpoint. Each viewer needs its own route.

For most deployments (1–2 viewers), this is simple to manage. If you have many viewers, consider scripting route creation via the [Notehub API](https://dev.blues.io/api-reference/notehub-api/).

> **Fleet membership still matters:** All viewers should join the `tankalarm-viewer` fleet (the firmware does this automatically). This enables fleet-scoped DFU updates, device grouping in the Notehub UI, and future fleet-level features.

---

## Step 6: Create Route #4 — SMS Alerts (Optional)

If you're using SMS alerts, create a route for the `sms.qo` notefile using Notehub's **Twilio** route type.

### What the server sends

When an alarm warrants an SMS, the TankAlarm server (v2.1.6+) adds **one note per recipient**
to `sms.qo`, each with a scalar `to` field:

```json
{
  "message": "Silas #1 high alarm 43.8 psi",
  "to": "+15551234567"
}
```

Because each event carries exactly one destination, a **single Twilio route** using the
`[body.to]` placeholder delivers to every recipient — the dashboard's Contacts → SMS Alert
Recipients list (and each contact's alarm associations) fully controls delivery. No
per-recipient routes are needed.

> **Upgrading from firmware older than v2.1.6?** Those versions sent a single note with a
> `numbers[]` array that a Twilio route cannot fan out over — delivery went only to the
> route's static To Number. Update the route to the configuration below when you update the
> server firmware.

### Route configuration

| Setting | Value |
|---------|-------|
| **Route Name** | `SMSRoute` |
| **Route Type** | **Twilio** |
| **Account SID** | Your Twilio Account SID |
| **Auth Token** | Your Twilio Auth Token |
| **From Number** | Your Twilio phone number (E.164, e.g. `+18005551212`) |
| **To Number** | `[body.to]` — pulls the recipient from each note. **Required.** |
| **Message** | `[body.message]` — pulls the alert text from each note. **Required.** |
| **Notefiles** | Selected Notefiles → `sms.qo` |
| **Automatic reroute on failure** | Enable (retries at 5 s / 1 min / 5 min) |

> **Twilio trial accounts:** every recipient number must be a Verified Caller ID in Twilio,
> and all numbers must be E.164 formatted (`+1...`).

### Recipient management (server side)

- Recipients are managed on the server dashboard: Contacts → **SMS Alert Recipients**.
- A contact with **alarm associations** configured receives only those alarms (plus
  fleet-wide alerts such as OTA/stale-client/server notices); a contact with **no**
  associations receives everything.
- The server de-duplicates numbers and caps sends at **10 recipients per alert** as a cost
  guard.

### Test it

Use the **Send Test SMS** button on the server's Settings page (next to the SMS recipient
list) — it queues a real `sms.qo` note to every configured recipient. Then confirm: the
events appear in Notehub → Events (one per recipient), the route log shows a 2xx response
for each, and every phone receives the text. Route errors (invalid number, unverified trial
recipient) appear when you click the event's error icon.

### Inbound STOP/START/HELP + welcome messages (v2.2.3+)

- Every **newly enrolled SMS recipient** automatically receives a welcome text:
  *"You are now receiving alerts from TankAlarm. For help, reply HELP. To opt-out, reply STOP."*
- **STOP replies:** Twilio blocks the number at the platform level automatically (later
  sends fail with error 21610). The server *additionally* tracks opt-outs — an **OPTED
  OUT** badge and an **SMS Opt-Out List** section appear on the `/contacts` page, and all
  SMS to those numbers is suppressed — once you wire the inbound webhook: a small **Twilio
  Function** forwards each reply through the Notehub API into `sms_inbound.qi` on the
  server device (no new Notehub route needed).
- **START replies** remove the number from the list and re-send the welcome message.
- **HELP replies** are answered by Twilio — set the text under Console → Messaging →
  Opt-out management.
- Full step-by-step including the Function code: server dashboard → **Server Settings →
  SMS Setup Guide** (`/sms-setup`).

---

## Step 7: Create Route #5 — Email Reports & Alerts (Optional)

> **Important:** Notehub has **no native SMTP/email route type**. Email is delivered by a
> **General HTTP/HTTPS route** that calls an HTTP endpoint. Two supported options:
> **Option A — SendGrid** (transactional email API) or **Option B — Google Workspace**
> (a free Apps Script bridge that sends from your real company mailbox). Mailgun, Postmark,
> or any HTTP email API works like Option A with a different URL/payload.

### What the server sends

The server publishes **two body shapes** to `email.qo`:

**Daily report** (scheduled, one note, all recipients comma-joined):

```json
{
  "to": "ops@example.com,owner@example.com",
  "subject": "Daily Sensor Summary - 2026-07-08",
  "serverName": "Tank Alarm Server",
  "sensors": [ { "client": "dev:...", "site": "Silas", "label": "Cox Wellhead",
                 "sensorIndex": 1, "levelInches": 43.8, "sensorMa": 8.2,
                 "alarm": false, "alarmType": "clear" } ],
  "fmt": { "groupBySite": true, "fieldLevelInches": true, "...": "..." }
}
```

The `subject` comes from the **/email-format** page (`{date}` token supported), and the
page's field/summary toggles ride along in `fmt` so your route template can honor them.

**Alarm / unload / test alert** (event-driven, v2.2.1+, sent to contacts with the
**Email alerts** checkbox enabled on the /contacts page):

```json
{ "to": "ops@example.com", "subject": "TankAlarm Alert",
  "message": "Silas #1 high alarm 43.8 psi", "type": "alarm" }
```

Use `body.type = "alarm"` in your template to distinguish the two shapes.

### Option A — Route configuration (SendGrid)

| Setting | Value |
|---------|-------|
| **Route Name** | `EmailRoute` |
| **Route Type** | **General HTTP/HTTPS Request/Response** |
| **URL** | `https://api.sendgrid.com/v3/mail/send` |
| **HTTP Headers** | `Authorization: Bearer YOUR_SENDGRID_API_KEY` and `Content-Type: application/json` |
| **Notefiles** | Selected Notefiles → `email.qo` |
| **Transform Data** | **JSONata Expression** (below) |
| **Automatic reroute on failure** | Enable |

**JSONata expression** — converts both body shapes into a SendGrid v3 payload (splits the
comma-joined `to` list and builds a plain-text body from either the alarm `message` or the
daily `sensors` array):

```jsonata
{
  "personalizations": [{ "to": $map($split(body.to, ","), function($e) {{ "email": $trim($e) }}) }],
  "from": { "email": "alerts@yourdomain.com", "name": "TankAlarm" },
  "subject": body.subject,
  "content": [{
    "type": "text/plain",
    "value": body.type = "alarm"
      ? body.message
      : $join($map(body.sensors, function($s) {
          $s.site & " " & $s.label & " #" & $string($s.sensorIndex) & ": " &
          $string($s.levelInches) & ($s.alarm ? "  ** ALARM: " & $s.alarmType & " **" : "")
        }), "\n")
  }]
}
```

> **Provider notes:** the `from` address must be a verified sender in SendGrid. For richer
> formatting, point `content.type` at `text/html` and expand the template using the `fmt`
> toggles. Any other HTTP email API (Mailgun `/v3/<domain>/messages`, Postmark
> `/email`, etc.) works with the same pattern — swap the URL, auth header, and JSONata shape.

### Option B — Route configuration (Google Workspace via Apps Script)

Use this option to send alerts **from your real Google Workspace mailbox** (e.g.
`alerts@yourcompany.com`) with no third-party email account. A ~40-line Google Apps
Script deployed as a Web App receives the routed note and calls `MailApp.sendEmail()`.

> **Interactive version:** the server dashboard has a step-by-step guide with the full
> copy-paste script at **Server Settings → Email Setup Guide** (`/email-setup`, v2.2.2+).
> The steps below are the condensed reference.

**1. Create the Apps Script** — sign in at [script.google.com](https://script.google.com)
with the sending account, create a new project, and paste the bridge script (shown on the
server's `/email-setup` page). It authenticates requests with a `SECRET` query parameter,
parses the note `body`, handles both body shapes above (alarm `message` vs daily
`sensors[]`), and sends via `MailApp.sendEmail()`. Set `SECRET` to a long random string.

**2. Deploy as Web App** — Deploy → New deployment → type **Web app**, **Execute as: Me**,
**Who has access: Anyone**, authorize, and copy the `/exec` URL.

> To update the script later, use Deploy → **Manage deployments** → edit → **Version: New**
> so the URL is preserved. A brand-new deployment gets a *different* URL and breaks the route.

**3. Configure the route:**

| Setting | Value |
|---------|-------|
| **Route Name** | `EmailRoute` |
| **Route Type** | **General HTTP/HTTPS Request/Response** |
| **URL** | `https://script.google.com/macros/s/DEPLOYMENT_ID/exec?key=YOUR_SECRET` |
| **HTTP Headers** | none required |
| **Notefiles** | Selected Notefiles → `email.qo` |
| **Transform Data** | none — the script reads `.body` from the full event |
| **Automatic reroute on failure** | Enable |

**4. Expectations & limits:**

- The Notehub route log will show **HTTP 302** for successful deliveries — this is normal
  (Apps Script answers POSTs with a redirect). Real send results are in the Apps Script
  editor under **Executions**.
- Quotas: Workspace accounts ≈ 1,500 recipients/day via Apps Script (consumer Gmail: 100/day).
- The daily report's `fmt` object (from the server's `/email-format` page) is available to
  the script if you want to expand the sample renderer.

### Recipient management (server side)

- **Daily report** recipients: Contacts page → daily report list (or the settings page).
- **Alarm/unload emails**: per-contact **Email alerts** checkbox on the /contacts page.
  Contacts with alarm associations only receive their selected alarms.
- Use the **Send Test Email** button on the server settings page (`POST /api/email/test`)
  to verify the route end-to-end without waiting for a real alarm.
- **Opt-outs (v2.2.4+):** addresses on the /contacts **Email Opt-Out List** receive no
  email of any kind (including daily reports). Recipients can self-unsubscribe by replying
  STOP if you wire the optional Apps Script reply-forwarder documented on the server's
  `/email-setup` page (forwards replies into `email_inbound.qi` via the Notehub API — no
  new route needed).

---

## Verification Checklist

After setting up all routes, verify each one works:

### Route #1: ClientToServerRelay
- [ ] Client sends telemetry → Server receives `telemetry.qi`
- [ ] Client sends alarm → Server receives `alarm.qi`
- [ ] Client sends daily report → Server receives `daily.qi`
- [ ] Client sends unload event → Server receives `unload.qi`
- [ ] Client sends config ACK → Server receives `config_ack.qi`
- [ ] Client sends serial logs → Server receives `serial_log.qi`
- [ ] Client sends serial ACK → Server receives `serial_ack.qi`
- [ ] Client sends location response → Server receives `location_response.qi`
- [ ] Client sends relay forward → Server receives `relay_forward.qi`
- [ ] Route logs show 200 OK responses

### Route #2: ServerToClientRelay
- [ ] Server sends config → Client receives `config.qi`
- [ ] Server sends relay command → Client receives `relay.qi`
- [ ] Server sends serial request → Client receives `serial_request.qi`
- [ ] Server sends location request → Client receives `location_request.qi`
- [ ] Dynamic URL correctly targets different client devices
- [ ] Relay forward: Client A alarm → server `relay_forward.qi` → server re-issues via `command.qo` → Client B receives `relay.qi`

### Route #3: ServerToViewerRelay
- [ ] Server publishes viewer summary → Viewer receives `viewer_summary.qi`
- [ ] Viewer dashboard updates with new tank data
- [ ] Viewer "Request Update" button → server receives `viewer_request.qi` → fresh summary arrives within ~1 minute
- [ ] Viewer adds a contact → server receives `viewer_contacts.qi` → contact appears on the server's /contacts page with a VIEWER badge → echoed back to the viewer's Contacts page

### Route #4–5: SMS and Email
- [ ] Send Test SMS (server Settings page) → one `sms.qo` event per recipient, all routed 2xx
- [ ] SMS alerts arrive at every configured recipient's phone
- [ ] Send Test Email (server Settings page) → `email.qo` event routed 2xx, email arrives at every opted-in contact
- [ ] Daily report email arrives at configured addresses

---

## Troubleshooting

### "ClientToServerRelay failing after installing new firmware"

If `telemetry.qo` (and other client `.qo` files) appear in the Notehub event log but the ClientToServerRelay route fails for every event, the most likely cause is an **invalid URL placeholder** in the route configuration.

**Symptom:** Route logs show errors (4xx/5xx) for every client-originated event.

**Cause:** An earlier version of this guide incorrectly used `{{notefile_base}}` as the URL placeholder. That syntax is **not** recognized by Notehub. Notehub uses square-bracket placeholders (`[filebase]`), and any unrecognized `{{...}}` string is left as a literal in the URL, causing every route call to target a malformed endpoint.

**Fix:** Edit the ClientToServerRelay route URL and replace `{{notefile_base}}` with `[filebase]`:

| Wrong (causes failure) | Correct |
|------------------------|---------|
| `.../notes/{{notefile_base}}.qi` | `.../notes/[filebase].qi` |

Save the route and re-route a failed event from Route Logs to confirm the fix.

> **Why `[filebase]`?** It is a Blues-reserved placeholder that strips the file extension and any characters after the first dot. So `telemetry.qo` → `telemetry`, `relay_forward.qo` → `relay_forward`, `config_ack.qo` → `config_ack`, etc. See the [Blues placeholder docs](https://dev.blues.io/notehub/notehub-walkthrough/#using-placeholder-variable-substitution-interpolation) for the full list.

---

### "Route returned 4xx error"

| Error Code | Meaning | Fix |
|-----------|---------|-----|
| 401 | Unauthorized | Check your Personal Access Token is correct and not expired. Create PATs from your profile menu → API Access. Do **not** use OAuth tokens from Programmatic API Access (they expire every 30 minutes). For `X-SESSION-TOKEN`, paste the raw PAT only — **do not prefix with `Bearer `**. Also check that the header value has no extra whitespace. |
| 404 | Device not found | Verify the device UID is correct and the device exists in the project. Device UIDs are **case-sensitive** — `Dev:` ≠ `dev:` (must be lowercase). Also check that the URL placeholder is `[filebase]` (not `{{notefile_base}}`). |
| 400 | Bad request | Check the request body format — must be valid JSON with a `body` field |

### "Notes not appearing on target device"

1. **Check Route Logs** in Notehub — do you see the route firing?
2. **Check fleet assignment** — is the source device in the correct fleet for the route filter?
3. **Check notefile filter** — does the route's notefile filter match the exact `.qo` filename?
4. **Check the target device** — is it set to `continuous` or `periodic` mode? The device must sync to receive notes.

### "notefile names may not include a : character"

This error means your firmware still has old `device:uid:file.qi` or `fleet:name:file.qi` patterns. Update to the latest firmware — see the [Firmware Update Guide](FIRMWARE_UPDATE_GUIDE.md).

### "note.add file must be .qo, .qos, .db, .dbs, or .dbx"

Your firmware is trying to `note.add` with a `.qi` filename. Only `.qo` extensions work for outbound notes. Check that your notefile `#define` values use `.qo` for outbound files.

---

## Understanding the command.qo Pattern

The server uses a **single consolidated outbox** (`command.qo`) for all outbound commands. Each note includes routing metadata in the body:

```json
{
    "_target": "dev:123456789012",
    "_type": "config",
    "site": "North Tank Farm",
    "sampleSeconds": 1800,
    "tanks": [...]
}
```

| Field | Purpose | Values |
|-------|---------|--------|
| `_target` | Device UID of the target client | `dev:XXXXXXXXXXXX` |
| `_type` | Command type — determines which `.qi` file to deliver to | `config`, `relay`, `serial_request`, `location_request` |

The ServerToClientRelay Route reads these fields to construct the correct API URL.

### Why a single command.qo?

- **Fewer routes** — one route handles all server→client communication
- **Simpler firmware** — server always writes to the same notefile
- **Easier debugging** — all commands are visible in one place in Notehub
- **Relay forwarding** — when a client alarm triggers relays on another client, the request flows through the server via `relay_forward.qo` → Route #1 → `relay_forward.qi` (server) → `handleRelayForward()` re-issues via `command.qo` → Route #2 → target client `relay.qi`. This gives the server full visibility and audit capability.

### Serial Request/ACK Handshake

When the server requests serial logs from a client:

1. Server sends `serial_request` via `command.qo` → Route #2 → client `serial_request.qi`
2. Client receives request → sends `serial_ack.qo` with `{"status": "processing"}` → Route #1 → server `serial_ack.qi`
3. Client uploads logs via `serial_log.qo` → Route #1 → server `serial_log.qi`
4. Client sends `serial_ack.qo` with `{"status": "complete"}` → Route #1 → server `serial_ack.qi`

The server dashboard displays the ACK status in real time, providing feedback on whether the client has received and processed the request.

### Config ACK Protocol

When the server pushes config to a client, a version-tracked ACK handshake ensures delivery:

1. Server generates a config version hash (e.g., `"A3F2B1C0"`) and injects it as `_cv` in the config payload
2. Server sends config via `command.qo` → Route #2 → client `config.qi`
3. Client applies config → sends `config_ack.qo` with `{"client": "dev:...", "status": "applied", "cv": "A3F2B1C0"}` → Route #1 → server `config_ack.qi`
4. Server matches `cv` against the stored version — if they match, `pendingDispatch` is cleared and retries stop

If the ACK version doesn't match (e.g., client applied an older config), the server continues retrying until the correct version is acknowledged.

### Fleet Architecture

TankAlarm uses three fleets:

| Fleet | Devices | hub.set Fleet |
|-------|---------|---------------|
| `tankalarm-server` | Server nodes | Set in server's `hub.set` (configurable) |
| `tankalarm-clients` | Client nodes | Set via config push from server |
| `tankalarm-viewer` | Viewer nodes | Hardcoded in viewer's `hub.set` |

Fleet membership is used for:
- **Route source filtering** — Routes #1 and #2 fire only for events from specific fleets
- **DFU targeting** — firmware updates can be deployed per-fleet
- **Device management** — Notehub UI groups devices by fleet
- **Route #3 scoping** — the server fleet filter ensures only server-originated viewer summaries are routed

---

## Cost and Data Usage

| Route | Events/Day (typical) | Data per Event | Daily Total |
|-------|---------------------|---------------|-------------|
| ClientToServerRelay | ~50–60 per client (telemetry @ 1/30min + alarms, daily, unloads, config ACKs, serial, location) | ~200–500 bytes | ~15 KB/client |
| ServerToClientRelay | 1–5 (config/relay/serial request/location request) | ~500 bytes | ~2.5 KB |
| ServerToViewerRelay | 4 (every 6 hours) | ~2 KB | ~8 KB |
| SMS | 0–10 (alarm dependent) | ~160 bytes | ~1.6 KB |
| Email | 1 (daily report) | ~2 KB | ~2 KB |

**Total:** ~30 KB/day per client node. Well within Blues free tier (5,000 events/month).

---

## Next Steps

- [Client Installation Guide](CLIENT_INSTALLATION_GUIDE.md) — Set up your field sensor nodes
- [Server Installation Guide](SERVER_INSTALLATION_GUIDE.md) — Set up your central server
- [Fleet Setup Guide](FLEET_SETUP_GUIDE.md) — Configure fleets and device groups
- [Dashboard Guide](DASHBOARD_GUIDE.md) — Use the web dashboard
- [Troubleshooting Guide](TROUBLESHOOTING_GUIDE.md) — Common issues and fixes

---

*TankAlarm 112025 — Firmware v1.1.2+*
*Route Relay Architecture — No SSL certificates required on device hardware*
