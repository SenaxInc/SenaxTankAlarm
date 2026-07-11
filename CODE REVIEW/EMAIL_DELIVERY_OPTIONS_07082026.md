# Email Delivery Options — Full Method Catalog

**Date:** 2026-07-08
**Applies to:** TankAlarm v2.2.1 (server email pipeline: `email.qo` → Notehub Route #5)
**Companion docs:** `CODE_REVIEW_07062026_SMS_PIPELINE_END_TO_END.md`, `Tutorials/Tutorials-112025/NOTEHUB_ROUTES_SETUP.md` (Step 7)

---

## 0. Executive Summary

There are four families of ways this system can get an email out the door:

| Family | Where email is actually sent | On-device TLS? | Works offline (store & forward)? | Verdict |
|---|---|---|---|---|
| **A. Notehub route relay** (current) | Notehub cloud → email provider | No | **Yes** (notes queue on Notecard) | ✅ **Recommended — keep** |
| **B. Direct from Opta over LAN Ethernet** | Opta itself, via site internet | Yes (mbedTLS) | No (must hand-roll queue) | ⚠️ Possible, not advised |
| **C. Notecard proxy web request** (`web.post`) | Opta-initiated, TLS by Notehub | No | **No** (requires live session) | ⚠️ Niche fallback only |
| **D. Opta as an actual mail server (MTA)** | Opta direct to recipient MX | Yes | No | ❌ **Not viable — do not pursue** |
| **E. Own modem + SIM gateway node** (SMS) | Dedicated cellular board at server site | No (AT commands) | No (single radio, DIY queue) | ⚠️ Independence fallback only |

**Direct answer to "can the Opta server run a small email service?"** — It can run a small email *client* (an SMTP submission agent that hands mail to an authenticated relay like SendGrid/Gmail/an office Exchange server). It **cannot practically run an email *service*** (a mail server that delivers directly to recipient mailboxes or receives mail): modern anti-spam infrastructure (port-25 egress blocking, IP reputation, PTR/SPF/DKIM/DMARC requirements, no static IP) makes direct delivery from an embedded device on a business LAN or cellular link essentially undeliverable, independent of how good the code is. Details in §5.

---

## 1. Current Pipeline Recap (baseline for comparison)

```
Server Opta ──sendEmailAlert()/sendDailyEmail()──▶ email.qo note on Notecard
    │  {to:"a@x.com,b@y.com", subject, message, type:"alarm"}   (alarm shape)
    │  {to, subject, sensors:[...], fmt:{...}}                  (daily-report shape)
    ▼
Notecard ──cellular, store-and-forward──▶ Notehub
    ▼
Route #5: General HTTP/HTTPS → SendGrid v3 /mail/send
    (Bearer auth header + JSONata transform splits the comma-joined `to`
     and branches on body.type — see NOTEHUB_ROUTES_SETUP.md Step 7)
    ▼
SendGrid → recipient mailboxes (SPF/DKIM/DMARC handled by provider)
```

Properties worth preserving:
- **Zero TLS/crypto burden on the Opta.** No certificates to manage or expire on-device.
- **Store-and-forward.** If cellular is down, `email.qo` notes queue on the Notecard and flush when connectivity returns. Nothing to build.
- **Deliverability outsourced.** SendGrid's IPs carry the sender reputation; SPF/DKIM live in DNS, not firmware.
- **One provider swap = route edit, not firmware.** Changing email vendors touches Notehub only.

---

## 2. Family A — Notehub Route Relay (device stays as-is)

All of these keep the firmware unchanged; only the Notehub route (and possibly a small cloud component) differs. Verified: **Notehub has no native SMTP/email route type**, so every route-based option here is HTTP-shaped.

### A0. Notehub's own built-in email: **Alert Monitors** (what Blues suggests as its default)
Notehub itself can send email from exactly one place: the **Alerts → Alert Monitors** feature. Email is the first-listed notification channel (alongside Slack and Twilio SMS) for both monitor types:

| Monitor type | What it watches | Plan requirement |
|---|---|---|
| **Heartbeat Monitor** | Device silence — fires when a device hasn't communicated for N days/hours/minutes | **1 free on the Essentials plan**; more require Enterprise |
| **Event Monitor** | A field inside note bodies crossing a threshold (`>`, `<`, `>=`, `<=`, `!=`), optionally aggregated (avg/min/max/sum/count over a rolling window, per-device or fleet-wide) | **Enterprise plan only** |

Why this is *not* a replacement for our Route #5 pipeline:
- Recipients are a **fixed list typed into the Notehub UI** — no per-contact opt-in, no alarm associations, no viewer-managed contacts, none of the server's contact-directory logic.
- Event Monitors are simple **threshold comparisons on one field** — they cannot reproduce composed alarm messages, reminders, unload reports, or the daily report.
- Event Monitors are **paywalled behind Enterprise**.

Where it **is** genuinely useful to us — as a free backstop:
- The **one free Heartbeat Monitor** can email the admin when the *server Opta itself* goes silent (e.g., "no events for 26 hours"). That is a dead-server detector that **no firmware on the server can provide** (a dead server can't report its own death), and today nothing covers this gap. Configure once in Notehub, zero code.
- Related: fleet **Watchdog Events** (`_watchdog.qo`) emit device-silence as routable events — those could even be routed through our existing SMS/email routes, though the Heartbeat Monitor is simpler for a single server device.

For routing data generally, Blues' first-listed, tutorial-default route type is **General HTTP/HTTPS** ("Notehub is able to route data to virtually any provider") — i.e., exactly the shape our Route #5 uses. Blues publishes partner guides for Twilio (SMS) and Slack, but **no email-provider guide**; email delivery of note data is left to the General HTTP route + your chosen provider.

### A1. General HTTP route → transactional email API — **CURRENT**
Route #5 as documented. Provider choices, all drop-in with only URL/header/JSONata changes:

| Provider | API style | Free tier | Notes |
|---|---|---|---|
| **SendGrid** (current) | REST `v3/mail/send`, Bearer key | 100/day | Documented in tutorial Step 7 |
| **Mailgun** | REST, form-encoded or JSON, Basic auth | trial/limited | Simple API, good logs |
| **Postmark** | REST JSON, `X-Postmark-Server-Token` | 100/mo | Best-in-class deliverability for transactional |
| **SMTP2GO** | REST JSON `/email/send` | 1,000/mo | Generous free tier |
| **Brevo (Sendinblue)** | REST JSON, `api-key` header | 300/day | Largest free daily quota |
| **AWS SES (HTTP API)** | SigV4-signed REST | pay-per-use (~$0.10/1k) | SigV4 signing is awkward from a JSONata-only route — prefer A2 with Lambda if SES is required |

- **Effort:** none (already built). Provider swap ≈ 30 minutes of route editing.
- **Risk:** provider API key expiry/rotation; JSONata errors only visible in Notehub route logs.

### A2. Cloud-function route → anything (including real SMTP)
Notehub natively routes to **AWS Lambda**, **Azure Functions**, and **Google Cloud Functions**. The function receives the note JSON and can then:
- send via any provider SDK (SES, SendGrid, Graph API, Gmail API), or
- open a **genuine SMTP session** to a corporate mail server (e.g., the company's Exchange/365 connector or an internal Postfix smart host).

This is the **only clean path to "must go through our corporate SMTP server"** requirements, because the TLS+AUTH SMTP conversation happens in the cloud function, not on the Opta.

- **Effort:** small (30–80 lines of Python/Node) + cloud account plumbing.
- **Cost:** effectively $0 at this volume (free tiers dwarf tank-alarm traffic).
- **Risk:** one more deployed artifact to maintain; cold-start latency (irrelevant for email).

### A3. Low-code webhook → Gmail/Outlook (Zapier, Make, Power Automate, IFTTT)
Point Route #5 (General HTTP) at a Zapier/Make/Power Automate webhook; the automation's "Send Gmail/Outlook email" step does the rest. Power Automate is attractive if the operator already lives in Microsoft 365 — the mail comes *from the operator's real mailbox*.

- **Effort:** minutes, no code.
- **Limits:** free tiers are task-capped (Zapier ~100/mo); an alarm storm could burn the quota; another SaaS dependency.
- **Fit:** good for a small deployment or a proof of concept; weaker for fleet scale.

### A4. Adjacent (not email, but same wiring)
- **Slack route:** Notehub has a first-class Slack route type — alarm notes → Slack channel. Zero email infrastructure.
- **Email-to-SMS gateways:** because we already send email, carrier gateways (`5551234567@vtext.com`, `@txt.att.net`, `@tmomail.net`) can serve as a **free SMS backup path** — just add the gateway address as a contact email. Deliverability is best-effort and carriers throttle; do not replace Twilio with this, but it is a useful belt-and-suspenders trick.

### A5. Google Workspace (Gmail) integration paths
If the operator's company runs on Google Workspace and wants alerts to come **from their real company mailbox**, there are four workable methods — verified against Google's Workspace admin docs (updated 2026-07). One constraint first: **a Notehub route cannot talk to Gmail directly.** Notehub has no SMTP route type, and the Gmail REST API requires OAuth 2.0 tokens that expire hourly — a static route header can't refresh them. Every Gmail path therefore needs a small bridge (or must originate from the Opta itself):

| # | Method | How | Limits / auth | Fit |
|---|---|---|---|---|
| G1 | **Apps Script Web App bridge** (zero-infra favorite) | ~15-line Google Apps Script deployed as a Web App; Notehub Route #5 (General HTTP) POSTs the note JSON to the script URL; `doPost()` calls `MailApp.sendEmail()` | 1,500 recipients/day (Workspace quota); secure via unguessable URL + shared-secret field check; no OAuth code (deployment handles it) | ✅ **Best Workspace path** — free, no servers, mail from the real mailbox, keeps store-and-forward |
| G2 | Cloud function → `smtp.gmail.com:587` (STARTTLS) with an **app password** | A2 pattern; function does SMTP AUTH as the Workspace user | 2,000 msgs/day; requires 2-Step Verification + app password (plain passwords are dead — Google ended "less secure apps" May 2025; app passwords remain the sanctioned device path, though admins can disable them and OAuth/XOAUTH2 is the long-term direction) | ✅ Fine if a cloud function exists anyway |
| G3 | **Workspace SMTP relay** `smtp-relay.gmail.com:25/465/587` — Google's *recommended* option for devices/apps | Admin console → Gmail → Routing → SMTP relay; authenticates by **allowlisted static IP** (or SMTP AUTH); admin may waive the TLS requirement | 10,000 recipients/day/user; needs static egress IP; send to anyone | ✅ For corporate sites: usable from a cloud function, a LAN smart host (B2), or even the Opta directly (see below) |
| G4 | **Restricted Gmail SMTP** `aspmx.l.google.com:25` — **no TLS, no auth** | Plain SMTP from an allowlisted IP + SPF record | Delivers **only to addresses in your own Workspace domain**; port 25 egress must be open | ⚠️ Niche, but see below — the only Google path with zero crypto |

**The on-Opta angle (why G3/G4 are interesting):** both accept **plain, unencrypted SMTP on port 25 from an allowlisted static IP** — meaning the Opta could speak to Google *directly* with a ~150-line no-TLS SMTP client (no mbedTLS, no certificates, minimal RAM — the same tiny footprint as B2, but with Google as the smart host and no on-site relay box). Preconditions are strict: site LAN internet egress, a static public IP registered in the admin console, outbound port 25 not blocked by the ISP (frequently is), and for G4, all recipients inside the company domain. Where those hold — e.g., alarm emails that only go to `@company.com` staff from an office-connected server — this is the most realistic version of "the Opta sends its own email." It still sacrifices the Notecard's offline queue, so it belongs in the same "only if the Blues path is forbidden" bucket as the rest of Family B.

### A6. SMS via Google Workspace — investigated and **rejected**
Question examined: could Google Voice for Workspace (or anything else in the Google stack) replace/augment Twilio for SMS alerts? Findings, verified against Google's current help docs:

1. **Google Voice for Workspace has no API — at all.** There is no REST endpoint, no Apps Script service, no Admin SDK message-send call. SMS can only be sent by a human in the voice.google.com UI or the mobile apps. Nothing for Notehub, a cloud function, or the Opta to call. (The Admin SDK only manages Voice *administration* — number assignment, not messaging.)
2. **Its terms explicitly prohibit our use case.** Google Voice messaging "is intended only for interactive conversations. It's not intended for bulk messaging, which can result in messages being reported as spam and blocked" — automated alarm/notification traffic is precisely what gets a number blocked. Google's own docs tell even teachers sending announcements to use email instead.
3. **Paid-subscription (Workspace) Voice SMS is US-only**, no short codes, group texts capped at 8 participants — hobbled even for the manual case.
4. **Google has no SMS product anywhere else either.** Google Cloud offers no SMS API (its marketplace refers customers to Twilio/Vonage); Firebase phone-auth SMS is OTP-only; RCS Business Messaging requires carrier-approved verified agents and RCS-capable handsets — not SMS and not practical here.
5. **The only Google-flavored path to a text message is the one we already have:** Workspace Gmail → carrier **email-to-SMS gateways** (`5551234567@vtext.com` etc., §A4) — e.g., via the G1 Apps Script bridge. Still email under the hood, still best-effort delivery, still backup-only.

**Verdict: Twilio (Route #4) remains the SMS channel.** Google Workspace contributes nothing for programmatic SMS beyond the email-gateway trick already cataloged.

### A7. If Twilio rejects the account — SMS provider fallback ladder
Twilio's compliance vetting can reject account or campaign applications at its discretion. The good news: **the firmware contract is provider-neutral** — `sms.qo` notes carry `{message, to}` and any provider with a static-credential REST API can replace Twilio by editing Route #4 (Notehub's dedicated Twilio route type becomes a General HTTP route + JSONata). No firmware change, no contact-directory change.

Requirements for a drop-in candidate: fixed endpoint URL + **static auth** (Basic or bearer/API-key header — Notehub routes cannot run OAuth token refresh), JSON or form body buildable from JSONata + placeholders.

| Rank | Provider | API / auth | Notehub fit | Notes |
|---|---|---|---|---|
| 1 | **Telnyx** | `POST /v2/messages`, `Authorization: Bearer <key>`, JSON `{from,to,text}` | General HTTP route, trivial JSONata | Self-serve signup, developer-oriented, runs its own carrier network, typically ~half Twilio's per-message price. Usual first pick after a Twilio rejection. |
| 2 | **Vonage (Nexmo)** | `POST rest.nexmo.com/sms/json`, `api_key`/`api_secret` params | General HTTP route | Long-established, simple API, straightforward vetting. |
| 3 | **Plivo** | `POST /v1/Account/{id}/Message/`, Basic auth, JSON `{src,dst,text}` | General HTTP route | Near-clone of Twilio's model; migration docs literally target Twilio switchers. |
| 4 | **AWS SNS / End User Messaging** | SigV4-signed — **not** static-header compatible | Via Notehub's **AWS Lambda route type** (~20-line function) | An AWS account is effectively never "rejected," making this the vetting-proof floor. Costs the Lambda hop. |
| 5 | **ClickSend / Sinch / Infobip** | Basic auth / API-key REST | General HTTP route | Fine, but smaller dev ecosystems (ClickSend) or enterprise sales motion (Sinch/Infobip). |
| 6 | **Email→SMS carrier gateways** (§A4) | via existing email channel | Already built | Free last resort; best-effort delivery, no delivery receipts, carrier throttling. |

**The caveat nobody escapes:** US A2P regulations follow the *traffic*, not the vendor. Every provider above requires an approved origination identity for US SMS — either **10DLC campaign registration** (same TCR vetting ecosystem where Twilio rejections originate) or a **verified toll-free number**. For a low-volume industrial alarm system, **toll-free verification is the gentler path** (single-use-case form, no campaign registry, typically days to approve) and is offered by Telnyx, Vonage, and Plivo alike. So the practical playbook after a Twilio rejection: *Telnyx + toll-free verified number*; if that vetting also fails, *AWS SNS via a Lambda route*; and the email→SMS gateway remains the zero-vetting emergency channel.

Switch cost estimate: ~1 hour (new Route #4 URL/headers/JSONata + one Send Test SMS round-trip). Worth noting in the operator runbook that `From` number, body encoding (form vs JSON), and status-callback semantics are the only things that differ. For the fully self-hosted variant — our own modem and SIM at the server site — see **Family E (§6)**.

---

## 3. Family B — Direct Send from the Opta over LAN Ethernet

These make the Opta itself speak to the outside world through the **site LAN's internet connection** (not the Notecard — the Notecard is not a general-purpose IP interface for the host). Precondition that must be verified per site: **the LAN the server Opta sits on actually has internet egress.** Remote tank sites frequently do not, which is why the Notecard exists.

### B1. SMTP submission client on the Opta (port 587 STARTTLS / 465 implicit TLS)
The Opta authenticates to a **relay** — SendGrid's SMTP endpoint, `smtp.gmail.com` with an app password (see A5/G2 for the Workspace auth rules), or the office mail server — which then handles world-facing delivery. This is the legitimate version of "the Opta sends its own email."

Feasibility on `arduino:mbed_opta:opta`:
- **Transport:** mbed OS `TCPSocket` + `TLSSocket` (mbedTLS is in the core). Workable.
- **Protocol:** SMTP submission is simple enough to hand-roll (~200–350 lines: EHLO, STARTTLS, AUTH LOGIN/PLAIN base64, MAIL FROM, RCPT TO per recipient, DATA with dot-stuffing, QUIT).
- **Library situation:** **thin.** The popular Arduino email libraries (`EMailSender`, `ESP-Mail-Client`, `ReadyMail`) target ESP32/ESP8266/AVR network stacks and do **not** support the mbed Opta core out of the box. Expect to write and own the client.
- **RAM:** a mbedTLS session costs roughly 40–60 KB. Current server build already uses **359,616 B / 68 % of RAM**; ~164 KB headroom means one TLS session fits, but it will coexist tightly with the web server, Notecard I2C, and contact/config buffers.
- **Certificates:** must embed and *maintain* a root CA bundle (or pin a cert that will eventually rotate). A CA change in 2–3 years silently breaks email until a firmware update — exactly the failure mode the current design avoids.
- **Blocking & watchdog:** SMTP+TLS handshake over a slow link can take multiple seconds; every wait loop needs WDT kicks and timeouts, in the same main loop that services HTTP clients.
- **Offline behavior:** none for free. A power/ISP outage during an alarm loses the email unless we build a flash-backed retry queue — re-implementing what the Notecard already gives us.

**Verdict:** genuinely possible; roughly 1–2 weeks of careful firmware work + permanent cert/RAM/maintenance liability, to duplicate something the Notehub route already does more robustly. Only worth it if a hard requirement appears that email must flow *without* Blues/cellular *and* the site has reliable LAN internet.

### B2. Plain SMTP (port 25, no TLS) to an **internal LAN relay / smart host**
The best on-device variant *if the site has IT infrastructure*: the Opta speaks trivial, unencrypted SMTP to a LAN-local relay (office Exchange connector, a Postfix box, or even a Raspberry Pi running a 20-line Python `aiosmtpd`→provider bridge). The relay does TLS, auth, and queuing.

- **Firmware cost:** tiny (~150 lines, no TLS, no certs, minimal RAM).
- **Trade:** requires a maintained relay host at the site and mail-flow rules that allow an unauthenticated LAN client. Availability of email now depends on that host.
- **Fit:** office/plant deployments with an IT department. Not fits-in-a-cabinet remote sites.

### B3. HTTPS REST call from the Opta directly to an email API
Same idea as B1 but speaking HTTPS to SendGrid's REST API instead of SMTP. Slightly simpler protocol layer, identical TLS/RAM/cert/queueing burdens. No advantage over B1 except familiarity; strictly worse than A1 in every operational dimension.

### B4. (Variant) LAN HTTP to a local bridge
Opta POSTs plain HTTP JSON to a Raspberry Pi/PC on the LAN, which forwards to a provider. Equivalent to B2 with HTTP instead of SMTP; same pros/cons. Mentioned for completeness because it needs no TLS or SMTP code on the Opta at all.

---

## 4. Family C — Notecard Proxy Web Requests (`web.post`)

The Notecard supports device-initiated `web.*` transactions through a Notehub **Proxy Route**: the Opta calls `web.post`, Notehub performs the HTTPS call to the configured URL (e.g., SendGrid) and returns the response. TLS is Notehub's problem, like Family A.

Why it is *not* better than the current design for email:
- Requires the Notecard to be in **continuous** connection mode with a live session at call time — **no store-and-forward**. A cellular hiccup during an alarm = failed email, with retry logic pushed back into firmware.
- Payload caps (~8 KB) and per-call latency.
- We would still keep `email.qo` for the daily report anyway, so it fragments the pipeline.

**Legitimate niche:** a synchronous "Send Test Email and show me the provider's HTTP status right now" diagnostic. Even that is currently served well enough by the `/api/email/test` button + Notehub route-log inspection.

---

## 5. Family D — Running a Real Email Service on the Opta (the direct question)

Split "email service" into its three possible meanings:

### D1. Submission client (sender via authenticated relay)
**Yes, feasible** — this is exactly B1/B2 above. If someone says "the Opta sends its own email," this is the only version that can work, and it works by *not* being a mail server — it's a client of one.

### D2. Full MTA — direct-to-MX delivery (a true "small email server")
Technically codable (DNS MX lookup, SMTP to port 25 of `mx.recipient.com`, even DKIM signing — mbedTLS can do RSA-SHA256). **Practically undeliverable**, for reasons no firmware can fix:

1. **Port 25 egress is blocked** by most business ISPs and effectively all cellular carriers. The connection usually cannot even open. (The Notecard path offers no raw sockets at all, so cellular is out regardless.)
2. **IP reputation:** mail from a dynamic/business-DSL IP with no history is dropped or spam-foldered by Gmail/Outlook on arrival. Blocklists (Spamhaus PBL) preemptively list dynamic ranges.
3. **Reverse DNS (PTR)** must match HELO; not controllable on typical site connections.
4. **SPF/DKIM/DMARC:** sending as `alerts@company.com` requires the site's egress IP in the company SPF record and a published DKIM key — a DNS-administration coupling per site that operations will not sustain.
5. **No retry infrastructure:** real MTAs queue for days across greylisting and deferrals; the Opta would need a flash-backed multi-day queue with backoff per recipient domain.
6. **Resource cost for nothing:** all of the above burns flash/RAM (already 48 %/68 %) to achieve *worse* deliverability than a free-tier API.

**Verdict: ❌ do not build.** This is not an embedded-capability problem; it is how the modern email ecosystem treats unknown senders.

### D3. Receiving mail / mailbox service (inbound)
Would require a static public IP, an MX record pointing at the site, an always-listening port-25 daemon, spam filtering, and storage. No TankAlarm use case exists, and inbound port 25 to a control-network device is a security anti-pattern (expands attack surface on the same box that switches relays). **❌ Out of scope permanently.** If "reply to an alarm email to acknowledge it" is ever wanted, do it with an inbound-parse webhook at the provider (SendGrid Inbound Parse → Notehub inbound API or cloud function) — never on the device.

---

## 6. Family E — Self-Hosted SMS Gateway Hardware (own modem + own SIM)

**The question:** add a second Arduino-class board with a cellular/SMS shield and a minimal SIM subscription, and send alarm texts from *our own phone hardware* — no Twilio, no vetting, no per-message API pricing.

**We have been here before.** The retired legacy generation — **TankAlarm-092025 (Arduino MKR NB 1500 + Hologram SIM)** — was precisely this architecture, and it was replaced by the Blues design this repo is built on. Any revival should answer why it was retired: IoT-SIM SMS pricing (Hologram-class SIMs charge ~$0.19 *per SMS* or lack SMS entirely), modem babysitting (registration drops, signal, AT-command edge cases), no store-and-forward, and a single radio as a single point of failure.

### E1. What a 2026 revival would look like (done right)
- **Modem: LTE Cat-1, not a legacy shield.** 2G/3G shields (SIM800L/SIM900) are **dead in the US** — carrier sunsets killed them; do not buy one. A SIM7600A or Quectel EC25 (Cat-1) speaks standard SMS on any carrier with a plain consumer SIM. Cat-M1 boards (SIM7000/7080, the MKR NB 1500's SARA-R410M) also work but only on plans with SMS provisioned — exactly the trap the Hologram era fell into. (The Blues Notecard itself **cannot** do this: its SIM is data-only and the API exposes no SMS send.)
- **SIM: consumer/MVNO prepaid, not an IoT SIM.** A ~$5–6/mo unlimited-text MVNO plan (Tello, US Mobile, etc.) is the "minimal subscription" that actually includes real SMS. Caveat: putting a consumer plan in a modem can violate the MVNO's own ToS (IMEI/handset checks); some tolerate it, some suspend.
- **Integration:** a small gateway node (the modem board) on the server's LAN exposing `POST /sms {to, message}`; the server gains an alternate SMS transport (config switch: `notehub-route` vs `lan-gateway`, ~100 lines) while keeping `sms.qo` note shapes intact. The gateway firmware is its own real mini-project: AT-command send, delivery-report parsing, retry queue, watchdog, antenna/power/enclosure at the site.

### E2. The honest risk assessment
| Factor | Reality |
|---|---|
| **Vetting** | ✅ None — the entire attraction. No account to reject. |
| **Carrier AUP ("grey route")** | ⚠️ **The big one.** Automated/application traffic on a consumer line violates carrier AUPs and CTIA guidelines — the same A2P regime as 10DLC, enforced at the SIM instead of the API account. Low-volume person-style alerts to a handful of known numbers usually pass, but carriers run ML filtering: repetitive identical texts → **silent filtering or SIM suspension**, with no delivery receipts and no support channel. "Silently stops delivering" is the worst possible failure mode for an alarm system. |
| **Reliability** | ⚠️ Single radio, single SIM, single site. No store-and-forward beyond what we build. The current path rides the Notecard queue + Twilio's delivery infra. |
| **Cost** | ⚠️ Hardware $50–100 + $5–15/mo SIM + real engineering time. Twilio at our volume is ~$1–5/mo — the DIY route is *more* expensive in most months. The motivation is independence, not savings. |
| **Two-way SMS** | ✅ Genuine feature gain — operators could text back "ACK" to a real number. (More firmware scope if pursued.) |
| **Coverage** | Neutral — the server site already proves cellular coverage via its Notecard. |

### E3. Verdict
**Viable as the independence/last-resort option — ranked below every hosted provider in §A7, above nothing except doing without.** If every provider in the A7 ladder rejects us (unlikely — AWS SNS effectively cannot), build it with an LTE Cat-1 modem + tolerant MVNO SIM, keep volume low and message text varied, and treat silent filtering as an accepted risk. Do not rebuild the retired MKR NB 1500/Hologram stack. A hybrid is also possible: keep the Notehub→Twilio path primary and the LAN gateway as a cold-standby transport behind a config flag.

---

## 7. Comparison Matrix

| # | Method | Firmware change | New infra | TLS/certs on Opta | Survives cell/ISP outage | Deliverability | Est. effort | Recommendation |
|---|---|---|---|---|---|---|---|---|
| A0 | Notehub Alert Monitor (built-in email) | none | none | none | n/a (watches silence/thresholds) | ✅ Notehub sends it | minutes | ✅ **Free Heartbeat backstop**; Event Monitors ❌ (Enterprise, too simple) |
| A1 | Notehub → SendGrid HTTP (current) | none | none | none | ✅ notes queue | ✅ provider-grade | done | ✅ **Primary** |
| A1' | Swap provider (Postmark/SMTP2GO/Brevo) | none | none | none | ✅ | ✅ | ~30 min route edit | ✅ Fallback provider |
| A2 | Notehub → cloud function → SMTP/API | none | 1 small function | none | ✅ | ✅ | hours | ✅ **If corporate SMTP mandated** |
| A3 | Notehub → Zapier/Power Automate → mailbox | none | SaaS account | none | ✅ | ✅ (real mailbox) | minutes | ⚠️ Small fleets / demos |
| A5/G1 | Notehub → Apps Script → Workspace Gmail | none | 1 Apps Script | none | ✅ | ✅ (real mailbox, 1,500/day) | ~1 hour | ✅ **Best if operator is on Workspace** |
| A5/G3′ | Opta plain SMTP:25 → `smtp-relay.gmail.com` (IP auth) | small | static IP + admin config | none | ❌ LAN-dependent | ✅ (Google's) | days | ⚠️ Corporate sites w/ static IP + open port 25 only |
| A4 | Email→SMS carrier gateways | none | none | none | ✅ | best-effort | minutes | ⚠️ Backup trick only |
| C | Notecard `web.post` proxy | moderate | proxy route | none | ❌ live session needed | ✅ | days | ⚠️ Niche diagnostics only |
| B2 | Plain SMTP → LAN smart host | small | relay host at site | none | ❌ LAN-dependent | ✅ (relay's) | days | ⚠️ Only where site IT exists |
| E | Own modem + SIM gateway node (SMS) | moderate | gateway board + MVNO SIM | none | ❌ single radio | ⚠️ grey-route filtering risk | 1–2 wks | ⚠️ **Independence fallback only** (§6) |
| B1 | On-Opta SMTP submission (587/465) | large | none | **yes** | ❌ unless queue built | ✅ (relay's) | 1–2 wks + upkeep | ❌ Not advised |
| B3 | On-Opta HTTPS → email API | large | none | **yes** | ❌ | ✅ | 1–2 wks + upkeep | ❌ Dominated by A1 |
| D2 | On-Opta full MTA (direct-to-MX) | very large | DNS/IP per site | yes | ❌ | ❌ **fails** | weeks, futile | ❌ **Never** |
| D3 | On-Opta inbound mail service | very large | static IP + MX | yes | ❌ | n/a | weeks, futile | ❌ **Never** |

---

## 8. Recommendations

1. **Keep A1** (email.qo → General HTTP → SendGrid) as the production path. It is the only option that simultaneously gives store-and-forward, zero on-device crypto, and provider-grade deliverability.
2. **Document A1' as the contingency:** if SendGrid's free tier or terms become a problem, SMTP2GO (1,000/mo) or Brevo (300/day) are 30-minute route swaps; Postmark if deliverability ever becomes critical. No firmware release needed.
3. **Adopt A2 only on demand:** if a customer requires "mail must originate from our corporate mail system," insert an Azure Function / Lambda between Notehub and their SMTP connector. Keep the note format unchanged.
4. **Reject B1/B3/D outright** unless a requirement appears that explicitly forbids the Blues path *and* guarantees site LAN internet — then choose **B2 (LAN smart host)** first, because it keeps TLS and queuing off the microcontroller.
5. **Optional cheap win:** mention the email→SMS carrier-gateway trick (A4) in the operator docs as a free secondary text path using the existing email checkbox — no code change required.
6. **Free backstop worth configuring today (A0):** set up the one free Notehub **Heartbeat Monitor** to email the admin if the server Opta goes silent for >24 h. It covers the one failure mode firmware can never report — the server itself dying — and costs nothing but two minutes in the Notehub UI.
7. **Own-hardware SMS (Family E) stays on the shelf** unless the entire §A7 provider ladder fails: it re-creates the retired TankAlarm-092025 architecture, costs more than Twilio at our volume, and its failure mode (silent carrier filtering of a consumer SIM) is the worst fit for alarm traffic. If ever built: LTE Cat-1 modem + tolerant MVNO SIM, as a cold-standby transport behind a config flag — never the primary.

---

*Prepared as an options analysis; no code changes accompany this document.*
