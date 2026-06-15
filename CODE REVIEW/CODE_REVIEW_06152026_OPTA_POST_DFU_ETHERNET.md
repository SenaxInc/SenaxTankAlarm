# Code Review — Opta Loses Network Connectivity After Firmware Updates

**Date:** 2026‑06‑15
**Author:** AI code review (for further human review)
**Affected unit:** Server Opta (`192.168.7.117`, DHCP) — recurs on most USB DFU flashes
**Firmware at time of writing:** Server built v1.9.21 (flashed but currently unreachable, pending power‑cycle)
**Status:** RECURRING / UNRESOLVED in firmware. Reliable workaround = physical power cycle. This document analyzes why and proposes fixes. **No code was changed.**

---

## 1. Executive Summary

After a USB DFU firmware flash, the Arduino Opta **server frequently fails to bring its Ethernet link back up**. The board is healthy on USB (every upload succeeds and it re‑enumerates on COM3), but `192.168.7.117` does not ping and port 80 is unreachable. The behavior is **nondeterministic**: sometimes the link returns in ~2.5 s, sometimes after a second/third re‑flash, and sometimes only after a **physical power cycle** (unplug/replug).

There are two distinct, compounding root causes:

1. **Hardware/PHY (primary):** `NVIC_SystemReset()` and the USB‑DFU reset are **soft resets** of the STM32H747. They do **not** power‑cycle the external **LAN8742 Ethernet PHY**. If the PHY is left in a partially‑negotiated or latched state, the Mbed network stack's re‑init during `Ethernet.begin()` can fail to (re)establish the link. A real power‑on reset clears the PHY, which is why power cycling always works.

2. **Firmware (secondary, makes it worse and unrecoverable):** Network is initialized **once** in `setup()`. The main `loop()` only calls `Ethernet.maintain()` and *reads* `linkStatus()` for logging — it **never re‑runs `Ethernet.begin()`, never resets the PHY, and never reboots on sustained link‑down.** So when cause #1 happens, the firmware has **no recovery path**; it logs "Network link lost!" forever and waits for a human.

A third, separate issue can masquerade as the same symptom: the **DHCP→static fallback uses a wrong‑subnet IP** (`192.168.1.200` on a `192.168.7.x` network) **and the serial message misreports it as `192.168.7.200`**. If DHCP fails during the brief post‑reset window, the server can come up reachable‑by‑nobody on the wrong subnet.

---

## 2. Precise Symptom

| Observation | Detail |
|---|---|
| USB DFU upload | **Succeeds every time** (board accepts flash, re‑enumerates COM3) |
| Ethernet after flash | Often **down**: `192.168.7.117` no ping, port 80 unreachable |
| Recovery | Nondeterministic — sometimes a re‑flash, often a **power cycle** |
| Board health | Fine on USB; only the network is affected |
| Serial (when observed) | Prints "Network link lost!" / no IP, or comes up on an unexpected address |

**Historical flash log (from session notes):**

| Version | Post‑flash Ethernet outcome |
|---|---|
| v1.9.9 | Self‑recovered in ~2.5 s |
| v1.9.10 | Needed a second re‑flash |
| v1.9.17 | Two flashes (1st: port 80 open briefly then down) |
| v1.9.18 | Two flashes to relink |
| v1.9.20 | Relinked on the **first** try |
| v1.9.21 | **Did NOT relink after ~4 flashes — needs power cycle** |

The spread (2.5 s → never‑without‑power‑cycle) is the signature of a **hardware initialization race / latched PHY state**, not a deterministic software bug.

---

## 3. Hardware Context

- **MCU:** STM32H747XI (dual‑core) on the Arduino Opta Lite.
- **Ethernet MAC:** integrated in the STM32H747.
- **Ethernet PHY:** external **LAN8742** (RMII), with its own reset/power‑on behavior. The PHY's hardware reset is **not** software‑toggled in this firmware.
- **Network stack:** `PortentaEthernet.h` + `Ethernet.h` → Arduino's Ethernet API shimmed over **Mbed OS `EthernetInterface`** (lwIP). This is *not* the classic WIZnet `Ethernet` library; `Ethernet.begin()` here drives the Mbed/lwIP bring‑up of the on‑chip MAC + LAN8742.
- **Reset paths:**
  - **USB DFU flash** (arduino‑cli) → bootloader resets the MCU (soft reset, no PHY power cycle).
  - **`NVIC_SystemReset()`** (used after the server's GitHub‑direct self‑update) → soft reset, **no PHY power cycle**.
  - **Power cycle** (unplug) → true power‑on reset, **PHY fully reset**. Always works.
- **Watchdog:** `WATCHDOG_TIMEOUT_SECONDS = 30` (Mbed watchdog). It is kicked every loop **regardless of network state**, so a no‑network condition does **not** trip a watchdog reset (the loop is still running fine).

> **Key hardware fact:** On Portenta/Opta‑class boards, the LAN8742 is widely reported to **not reliably re‑link after a soft reset** because its reset pin isn't asserted by software — only a power cycle guarantees a clean PHY state. This matches the observed nondeterminism exactly.

---

## 4. Code Path — Where It's Initialized and Why It Can't Recover

### 4a. One‑time init in `setup()` → `initializeEthernet()`
```c
initializeEthernet();   // setup() ~line 4250
gWebServer.begin();
```
`initializeEthernet()` (≈ line 8582) does, in order:
1. `Ethernet.hardwareStatus()` guard (returns if `EthernetNoHardware`).
2. `Ethernet.MACAddress(gMacAddress)` — reads the genuine hardware MAC (good; avoids the dummy‑MAC DHCP problem).
3. Up to **4** `Ethernet.begin()` attempts, **1.5 s apart** (DHCP or static per `gConfig.useStaticIp`).
4. **DHCP↔static fallback** if the primary mode got `status == 0`.
5. `Ethernet.linkStatus()` check → logs link up/down, prints IP/GW/subnet.

This is reasonable for a **cold boot**. The problem is what happens *after* it, on a soft‑reset boot where the PHY is latched.

### 4b. `loop()` — maintenance only, **no recovery**
```c
// loop() ~line 4356
Ethernet.maintain();   // DHCP lease renewal ONLY

if (now - gLastLinkCheckMillis > 5000UL) {
  bool linkUp = (Ethernet.linkStatus() == LinkON);
  if (linkUp && !gLastLinkState)  { /* log "link established", print IP */ }
  else if (!linkUp && gLastLinkState) { Serial.println("WARNING: Network link lost!"); }
  gLastLinkState = linkUp;
}
```

> **This is the core firmware gap.** On a sustained link‑down the loop:
> - does **not** call `Ethernet.begin()` again,
> - does **not** reset/re‑init the PHY or the Mbed interface,
> - does **not** reboot the MCU,
> - does **not** escalate after N seconds of no link.
>
> It only flips a flag and prints a warning. `Ethernet.maintain()` renews a DHCP lease on an *already‑up* interface; it does **not** rebuild a downed PHY link. So if the PHY didn't come up at boot, the server stays dark until a human intervenes.

### 4c. The reset after a server self‑update
```c
// ~line 4141
Serial.println(F("GitHub Direct: UPDATE COMPLETE - REBOOTING"));
...
NVIC_SystemReset();    // soft reset — PHY NOT power-cycled
```
`NVIC_SystemReset()` resets the core but leaves the LAN8742 in whatever state it was in — the same latched‑PHY risk as the USB‑DFU reset.

---

## 5. The Wrong‑Subnet Fallback (separate but symptom‑identical) ★

```c
static IPAddress gStaticIp(192, 168, 1, 200);     // line 398
static IPAddress gStaticGateway(192, 168, 1, 1);
static IPAddress gStaticSubnet(255, 255, 255, 0);
...
// initializeEthernet() fallback branch:
Serial.println(F("DHCP failed after retries. Falling back to default static IP: 192.168.7.200"));
status = Ethernet.begin(gMacAddress, gStaticIp, gStaticDns, gStaticGateway, gStaticSubnet);
```

Two defects in one place:
1. **Subnet mismatch:** the live LAN is `192.168.7.x`, but the compiled fallback is `192.168.1.200` / gateway `192.168.1.1`. If DHCP fails during the brief post‑reset window (very possible if the PHY links late), the server comes up on `192.168.1.200` — **unreachable on a `192.168.7.x` network**, indistinguishable from "no link" to the operator.
2. **Misleading log:** the serial message says `192.168.7.200` while the code actually uses `192.168.1.200`. Anyone debugging from the serial log is sent to the wrong address.

This means some "unreachable after flash" episodes may not be PHY at all — they may be **DHCP‑missed‑the‑window → wrong‑subnet static**. The fix is different (Section 8), which is why separating the two matters.

---

## 6. What Has Worked (history)

- **Power cycle always works.** Unplug/replug recovers the link 100% of the time → confirms a clean **power‑on reset of the PHY** is the reliable cure (points squarely at PHY soft‑reset state).
- **A second/third re‑flash often works.** Each re‑flash adds another reset and a fresh `Ethernet.begin()`; sometimes one of them happens to catch the PHY in a linkable state.
- **`Ethernet.MACAddress()` before `begin()` works.** Reading the genuine hardware MAC (added earlier) fixed an earlier DHCP‑reservation problem and is correct.
- **The 4×/1.5 s spaced DHCP retry** improves cold‑boot reliability (gives the switch time to learn the port and respond to reservations).
- **DHCP normal path works on cold boot** — the server reliably comes up at `192.168.7.117` from a true power‑on.

## 7. What Has NOT Worked (history)

- **Re‑flashing as a recovery method is unreliable** — v1.9.21 didn't relink after ~4 flashes. Repeated soft resets don't guarantee a PHY power‑on.
- **There is no firmware self‑recovery** — the loop's link‑down branch only logs. No re‑`begin()`, no PHY reset, no reboot. (Primary firmware gap.)
- **The watchdog does not help** — it's kicked every loop regardless of link, so a no‑network state never trips it (by design the loop is healthy; only the PHY is down).
- **The static fallback can't help on this LAN** — even when it fires, `192.168.1.200` is off‑subnet, so it never restores reachability at `192.168.7.117`.

The throughline: **every existing mechanism assumes the PHY links at boot; none recovers when it doesn't.**

---

## 8. Root‑Cause Hypotheses (ranked)

### H1 — LAN8742 PHY not cleanly reset by soft reset (NVIC/USB‑DFU) ★ PRIMARY
A soft reset leaves the PHY powered and possibly mid‑/mis‑negotiated; Mbed's re‑init during `Ethernet.begin()` then fails to establish link. Only a power‑on reset clears it.
**Fits:** nondeterminism (2.5 s → never), "power cycle always works," "more flashes sometimes works."
**Fix:** assert a PHY/MAC reset or full Mbed interface re‑init at boot; failing that, detect no‑link and **self‑reboot** so the operator's "unplug" becomes automatic (still a soft reset, but combined with H2 recovery it converges; a true fix may need toggling the PHY reset line if exposed).

### H2 — No firmware recovery loop (link‑down is logged, never acted on) ★ PRIMARY (firmware)
Even when the PHY *could* re‑link with another `begin()`, the loop never tries.
**Fits:** "a re‑flash sometimes fixes it" (each flash = another `begin()`), but the running firmware itself never retries.
**Fix:** add a link‑watchdog: on sustained `LinkOFF` for N seconds, re‑run `Ethernet.begin()` (re‑DHCP), and if still down after M attempts, `NVIC_SystemReset()` (or a dedicated full‑reinit). This is the **highest‑value, lowest‑risk** change.

### H3 — DHCP misses the post‑reset window → wrong‑subnet static fallback
If the PHY links late, the 4×/1.5 s DHCP attempts can all fail, dropping to `192.168.1.200` (off‑subnet) — unreachable, looks like "no link."
**Fits:** episodes where serial shows an IP but the host is still unreachable.
**Fix:** correct the fallback to the actual subnet (or make it configurable/persisted), fix the misleading serial string, and prefer **retry‑DHCP‑forever‑with‑backoff** over a hard static fallback on a DHCP network.

### H4 — `Ethernet.begin()` called before PHY autonegotiation completes
Link negotiation can take 1–3 s; if `begin()`/DHCP runs before link‑up, it fails. The 1.5 s spacing helps but may be insufficient on a slow switch.
**Fix:** gate DHCP on `linkStatus() == LinkON` first (poll link up to a timeout), *then* `begin()`; increase attempts/backoff.

### H5 — Mbed/lwIP interface not fully torn down across soft reset
The Mbed `EthernetInterface` global state may not fully re‑initialize after a soft reset, leaving a half‑open stack.
**Fix:** explicit `disconnect()`/re‑`begin()` of the interface at startup; or a brief delay + `hardwareStatus()`/link poll before `begin()`.

### H6 — Power‑integrity / inrush on the PHY during DFU reset
Marginal 3.3 V rail or PHY power sequencing during the reset could leave the PHY unhappy until a true power cycle.
**Fix:** hardware‑side (decoupling, rail check); lower priority unless H1–H5 fixes don't converge.

**Ranking rationale:** H1 (hardware) + H2 (no recovery) together explain everything, and **H2 is fixable in firmware right now** to make the system self‑heal even while H1 persists. H3 is a real, separate, easily‑fixed defect that should be corrected regardless.

---

## 9. Diagnostic Plan

1. **Capture serial across a failing flash** (115200). Look for:
   - Which path ran: DHCP success? fallback to static? what IP printed?
   - `linkStatus()` transitions and whether "link established" ever prints.
   - This separates **H1/H2 (never links)** from **H3 (links but wrong subnet)**.
2. **After a failing flash, scan the LAN for `192.168.1.200`.** If it answers there → **H3 confirmed** (wrong‑subnet fallback), not PHY.
3. **Instrument a boot‑time link poll:** log `linkStatus()` every 250 ms for ~5 s before/after `begin()` to measure how long the PHY takes to link on soft vs. power‑on reset (tests H1/H4).
4. **Prototype the H2 recovery loop** (re‑`begin()` on sustained link‑down, reboot after M tries) and observe whether the server self‑heals without a human power cycle. This is the decisive firmware experiment.

---

## 10. Recommended Fixes (priority order)

1. **Add a link‑recovery state machine to `loop()` (H2 — do first).**
   - On `LinkOFF` continuously for `T_relink` (e.g., 10–15 s): re‑run `Ethernet.begin()` (re‑DHCP).
   - If still down after `N_relink` attempts (e.g., 3): `NVIC_SystemReset()` to force a clean re‑init (and, with the FTP/registry already persisted, a reboot is safe).
   - Cheap, low‑risk, and converts today's manual power‑cycle into automatic recovery.
2. **Fix the static fallback (H3 — do now, trivial).**
   - Correct `gStaticIp/gateway` to the real subnet *or* make them persisted/configurable; fix the serial message to print the actual IP; prefer DHCP‑retry‑with‑backoff over a hard off‑subnet static on a DHCP LAN.
3. **Gate `begin()` on link‑up + longer budget (H4).**
   - Poll `linkStatus()` up to a few seconds before attempting DHCP; widen retry count/backoff.
4. **Explicit interface re‑init at boot (H1/H5).**
   - Where the Portenta/Mbed API allows, disconnect/re‑initialize the `EthernetInterface` (and assert the PHY reset line if it is exposed on the Opta) so a soft‑reset boot starts the PHY from a known state.
5. **Operational mitigation (until firmware lands).**
   - Treat **power cycle** (not re‑flash) as the standard post‑flash recovery — it's deterministic. Consider flashing during a maintenance window where a power cycle is acceptable.

> Note: items 1–3 are firmware‑only and deployable on the next server flash. Item 1 specifically would have avoided the v1.9.21 dead‑end (the server would have rebooted itself into a linkable state instead of waiting for a human).

---

## 11. Open Questions for Review

1. Is the **LAN8742 hardware reset line** exposed/controllable on the Opta from firmware? If yes, asserting it at boot is the clean fix for H1.
2. When the server is unreachable post‑flash, does it ever answer at **`192.168.1.200`**? (Confirms/denies H3 quickly.)
3. Is the live network **always DHCP**? If so, the static fallback should be replaced with persistent DHCP‑retry rather than a fixed off‑subnet IP.
4. Acceptable behavior for the recovery loop: prefer **re‑`begin()`‑then‑reboot**, or reboot immediately on sustained link‑down? (Trade‑off: faster recovery vs. avoiding reboot loops if the cable is simply unplugged — gate the auto‑reboot on "was previously linked this boot" or on Notecard‑confirmed expectation of a network.)
5. Should auto‑reboot be **suppressed when the cable is legitimately absent** (e.g., link never came up this boot AND no DHCP server) to avoid boot‑looping a deliberately‑offline unit?

---

*Prepared for collaborative review. No firmware was modified as part of producing this document. Companion review: `CODE_REVIEW_06152026_CURRENT_LOOP_MA_READING.md`.*

## 12. AI Assistant Additional Review

**Date:** 2026-06-15

I have reviewed the initializeEthernet() and loop() implementations within TankAlarm-112025-Server-BluesOpta.ino.

**Observations & Additions:**
1. **Static IP Subnet Mismatch:** Confirmed true. gStaticIp is defined as IPAddress gStaticIp(192, 168, 1, 200) but the fallback serial log literally states 192.168.7.200. This will certainly strand the device on the wrong subnet if DHCP fails.
2. **Loop Recovery Lacking:** In the main loop(), around line 4356, the code checks Ethernet.linkStatus() == LinkON. It prints WARNING: Network link lost! but relies completely on Ethernet.maintain() (lease renewal mechanism) which is incapable of establishing a broken hardware PHY link. A manual intervention (power cycle) is the only path in current code.
3. **Proposed Implementation for H2/H1 Recovery:** We can add a non-blocking timeout counter specifically for link downtime.
   ```c
   static unsigned long lastLinkDownTime = 0;
   if (linkUp && !gLastLinkState) {
       lastLinkDownTime = 0; // Reset
   } else if (!linkUp) {
       if (lastLinkDownTime == 0) lastLinkDownTime = millis();
       if (millis() - lastLinkDownTime > 30000UL) {
           Serial.println(F("Link down for 30s! Resetting MCU..."));
           NVIC_SystemReset();
       }
   }
   ```
   While NVIC_SystemReset() doesn't power-cycle the PHY, calling it might re-trigger the DHCP sequence which sometimes works. For a truly robust fix, calling initializeEthernet() again in loop without resetting the MCU might just bring the stack back online. We must combine this with an interface disconnect command if provided by Mbed OS, or implement the PHY reset pin toggle (if hardware design provides it, which usually isn't standard on the Opta's external layout).

**Conclusion:** Subnet typo needs immediate correction (gStaticIp to 192.168.7.200). Adding an automatic re-initialization inside loop() for sustained network drop is critical for autonomous function and eliminates the necessity for manual power cycles.

---

## 13. Additional Review - Recovery Design and Static-IP Nuance

**Date:** 2026-06-15

I reviewed the current HEAD server code around the compiled static defaults, config load/save, `initializeEthernet()`, `loop()`, and the MCUBoot health gate. The original conclusions still hold: the server has no runtime Ethernet recovery, and the default static fallback is wrong for the observed `192.168.7.x` LAN. A few implementation details are worth tightening before a firmware fix is written.

### 13.1 The static fallback is configurable, but the compiled default is still dangerous

The code does persist static network settings in `/fs/server_config.json`: `loadConfig()` reads `staticIp`, `gateway`, `subnet`, and `dns`, and `saveConfig()` writes them back. That means the wrong-subnet risk is specifically highest on first boot, missing/corrupt config, older config files without those arrays, or any boot where DHCP fails before the operator has saved network settings.

The compiled defaults are still `192.168.1.200` / `192.168.1.1`, while the fallback log hard-codes `192.168.7.200`. That log is wrong in both directions: it is wrong for the compiled default, and it would also be wrong for any operator-configured static IP that is not `.7.200`.

Recommended fixes:

- Minimum fix: change compiled defaults to the actual deployment subnet or remove the hard-coded `.7.200` string and print `gStaticIp` directly.
- Better fix: make the fallback explicitly use persisted/static settings only when the operator enabled or confirmed them; otherwise keep retrying DHCP with backoff rather than silently moving to a hidden static IP.
- Best operational fix: persist the last successful DHCP lease/subnet and only offer a static fallback inside that known subnet, with a clear serial log of the actual IP used.

### 13.2 `initializeEthernet()` has retries, but not a true relink strategy

The function attempts the primary mode up to four times at 1.5 second spacing, then tries the alternate mode once. It does not wait for `LinkON` before DHCP, does not keep retrying once link negotiation finishes late, and does not run again from `loop()`.

This means a slow switch or a PHY that becomes sane after the initial DHCP window can still strand the server until another reset. A safer startup flow is:

1. Read the hardware MAC.
2. Poll/log `Ethernet.linkStatus()` for a bounded warmup window, such as 5-10 seconds.
3. Once link is up, run DHCP/static begin attempts.
4. If link never comes up, do not burn through DHCP then fall to an off-subnet static address; report `LinkOFF` and let the runtime recovery state machine keep trying.

If `linkStatus()` is unreliable before the first `Ethernet.begin()` on this Mbed stack, log that explicitly during testing and keep a short initial `begin()` attempt, then switch to the same bounded relink state machine.

### 13.3 Recovery should prefer re-begin over immediate reset

`NVIC_SystemReset()` is only a soft reset, so it is a probabilistic recovery for a latched LAN8742, not a cure. The first runtime recovery action should be to re-run the Ethernet bring-up path, not reboot immediately.

A robust non-blocking state machine should track:

- `linkDownSinceMs`
- `lastEthernetRecoverMs`
- `ethernetRecoverAttempts`
- `ethernetEverLinkedThisBoot`
- `lastEthernetBeginSucceeded`

Suggested behavior:

1. If `LinkON`, clear down timers and attempts, set `ethernetEverLinkedThisBoot = true`.
2. If `LinkOFF` continuously for 15-30 seconds, call an `ethernetRecover()` helper that re-runs the MAC/IP setup and then calls `gWebServer.begin()` again after a successful begin.
3. Rate-limit recovery attempts, for example every 30-60 seconds, with a maximum before escalation.
4. Only escalate to `NVIC_SystemReset()` after several failed re-begin attempts and only when rebooting is likely to help.

The last condition matters. If the cable is intentionally unplugged, the switch is down, or the DHCP server is absent, a reboot loop only makes the server worse and interrupts Notecard duties. Suppress auto-reboot when Ethernet has never linked this boot and the Notecard is otherwise healthy; keep retrying Ethernet with backoff instead.

### 13.4 Coordinate recovery with the firmware health gate

The loop currently marks MCUboot firmware healthy when storage is available and either the Notecard is available or Ethernet link is up. That is good: a post-flash Ethernet-only failure should not automatically roll back a healthy server if the Notecard is alive.

The Ethernet recovery logic should preserve that behavior:

- Do not force a reset in the first minute or two of boot, especially after an update.
- Prefer `Ethernet.begin()` recovery attempts before any reset escalation.
- If a reset escalation is used, make sure the firmware health marker has had a chance to be written when the Notecard is available.

Otherwise an Ethernet relink problem could accidentally interact with trial firmware confirmation and create a confusing rollback or repeated reboot pattern.

### 13.5 Reinitialization needs to restart dependent services

If Ethernet is reinitialized at runtime, call `gWebServer.begin()` again afterward and log the actual local IP, gateway, subnet, and mode used. Also ensure Ethernet-dependent operations such as FTP backup/restore remain gated on `LinkON` and do not start during a recovery attempt.

The Notecard polling path should keep running while Ethernet is down. The server can still process client telemetry/config ACKs through the Notecard even when the web dashboard is unreachable, so Ethernet recovery should not pause or starve that work.

### 13.6 Add network recovery observability

When Ethernet is down, the web dashboard is unavailable, so serial-only logs are not enough. Add a small persisted/Notecard-visible diagnostic state:

- Last Ethernet mode attempted (`dhcp`, `static`, `fallback-static`).
- Actual IP/gateway/subnet used, not a hard-coded string.
- Link-down duration and recovery attempt count.
- Last recovery action (`begin`, `phy-reset`, `soft-reset-requested`).
- Whether Ethernet ever linked this boot.

If the server's Notecard is available, publish a low-rate diagnostic note or include this in the server heartbeat/serial log stream so the operator can distinguish "server alive but Ethernet down" from a dead board.

### 13.7 Apply the same pattern to the Viewer if needed

The Viewer sketch also initializes Ethernet once and then calls only `Ethernet.maintain()` in `loop()`. If the Viewer is expected to survive the same soft-reset/PHY condition, the recovery helper should be shared or copied there after the server fix is proven.

### Revised priority order

1. Fix the static fallback defaults/logging: print actual `gStaticIp`, and avoid hidden off-subnet fallback.
2. Add a non-blocking Ethernet recovery state machine that re-runs begin before any reset.
3. Suppress reset escalation for legitimate cable/DHCP absence; keep Notecard processing alive.
4. Gate reset escalation around MCUboot health confirmation.
5. Add remote-visible Ethernet recovery diagnostics.
6. Investigate whether the Opta exposes a LAN8742 reset control; if yes, use it before resorting to soft reset.

The main change from the earlier review is nuance: auto-reboot can be useful as a last resort, but the first firmware fix should be a measured relink/re-begin state machine with correct static-IP handling and enough observability to prove which branch occurred.

---

## 14. Implementation Status (v1.9.22)

**Date:** 2026-06-15

The firmware-side recommendations (priority items 1-5) were implemented; the hardware investigation (item 6) remains open.

- **Implemented (v1.9.22):**
  - **Misleading log fixed (item 1):** the DHCP-fallback message now prints the actual `gStaticIp` instead of the hard-coded `192.168.7.200` string (the compiled default is `192.168.1.200`). Operators are no longer sent to a wrong, fabricated address.
  - **Autonomous recovery state machine (items 2-4):** `loop()` now tracks continuous link-down time. After `ETH_RELINK_GRACE_MS` (20 s) it re-runs `initializeEthernet()` + `gWebServer.begin()` (a real re-begin, replacing the manual re-flash), rate-limited to every `ETH_RECOVER_INTERVAL_MS` (30 s). It escalates to `NVIC_SystemReset()` only after `ETH_RECOVER_MAX_ATTEMPTS` (4) failed re-begins **and** only when the link worked earlier this boot (`gEverLinkedThisBoot`, so an unplugged cable / absent DHCP never boot-loops the unit) **and** past `ETH_RECOVER_BOOT_GRACE_MS` (2 min, so the MCUboot health marker is written first). Notecard polling keeps running throughout, so client telemetry is still processed while the web side recovers.
  - **Observability (item 5, lightweight):** recovery attempts log to serial and the server serial-log buffer (`addServerSerialLog`, source `network`).
- **Deferred:** the **wrong-subnet compiled default** (`192.168.1.200` vs the live `192.168.7.x`) was intentionally **not** hard-coded to the customer subnet — per Section 13.1 the right fix is to keep retrying DHCP and only use operator-persisted static settings, which the recovery loop now favors; the log fix removes the immediate confusion. Item 6 (whether the Opta exposes a controllable **LAN8742 reset line**, the only true cure for the hard-latch case) needs the hardware/datasheet investigation and is still open. The Viewer (Section 13.7) was not yet given the same pattern.

**Net effect now:** the common **soft-latch** post-DFU case should self-heal (re-begin, then a guarded soft reset) instead of requiring a human, and the off-subnet log lie is gone. The **hard-latch** case (only a true power-on clears the PHY) is still bounded by the boot-loop guards rather than cured — that awaits the LAN8742-reset investigation. Shipped as v1.9.22.
