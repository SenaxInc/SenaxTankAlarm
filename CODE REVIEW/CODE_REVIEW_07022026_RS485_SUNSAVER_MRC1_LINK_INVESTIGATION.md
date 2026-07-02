# RS-485 / SunSaver MPPT Link Investigation — History Review & Bench Test Results

**Date:** 2026-07-02
**Author:** GitHub Copilot (Claude)
**Status:** Investigation complete — failure localized; implementation decisions pending
**Hardware under test:** Bench client Opta (COM4, `dev:860322068056545`) ↔ Morningstar MRC-1 (MeterBus→EIA-485 adapter) ↔ SunSaver SS-MPPT-15L
**References:**
- Morningstar SunSaver MPPT Modbus spec: <https://www.morningstarcorp.com/wp-content/uploads/technical-doc-sunsaver-mppt-modbus-specification-en.pdf>
- Opta RS-485 tutorial: <https://opta.findernet.com/en/tutorial/getting-started-rs-485>
- Prior docs: `SUNSAVER_MODBUS_BRINGUP_FAILURE_04212026.md`, `MODBUS_DESIGN_NOTES_04182026.md`, `SOLAR_MODBUS_FUTURE_IMPROVEMENTS_04222026.md`, `CODE_REVIEW_06082026_CLIENT_RS485_SUNSAVER_MPPT_*.md`, `CODE_REVIEW_06252026_RS485_SUNSAVER_COMMUNICATION.md`, `CODE_REVIEW_06262026_DIAGNOSTICS_OPTAVIEW_AND_PRODUCTION_RESTORE.md`

---

## 1. Executive Summary

**The link is dead at the physical/adapter layer, and today's testing localizes the break to the
MRC-1's receive path** (with one explicitly-flagged caveat, §5.4). Everything on the Opta side
was re-verified live today:

| Layer | State today (2026-07-02) | Evidence |
|---|---|---|
| Client software stack (SolarManager, ArduinoModbus, timing fixes) | ✅ Healthy | CRC-valid frames on the wire, correct 8N2/9600/postDelay init |
| Opta RS-485 transceiver (ST3485, DE/RE sequencing) | ✅ Driving the bus | ~1 V AC measured across A–B during continuous TX stress |
| Wiring continuity Opta→MRC-1 (crossed A↔B per Morningstar convention) | ✅ Present | Same ~1 V AC at the far end; operator re-verified hookup |
| **MRC-1 data recognition** | ❌ **Sees nothing** | LED stays **solid green** ("power OK, no data") through 5+ minutes of continuous 960 B/s traffic at its own terminals |
| SunSaver reachability | ❌ Unreachable since ≥ 2026-06-26 | 0 bytes returned to every probe today and on 06-26 |

Key historical fact: **this exact link worked on 2026-04-22** — live CRC-valid data was captured
(`bv=12.23 V`) with the same firmware patterns still in production today, and the MRC-1's LED
visibly flickered with each poll. On 2026-06-26 the same historically-proven diagnostic returned
0 bytes and a passive sniff heard nothing. Today's tests reproduce the 06-26 result and add the
new discriminating datum: **the bus is being driven, and the MRC-1 still doesn't react.**

**Production firmware was never the problem** — the client code contains the complete, reviewed,
hardened Modbus stack (§3) and correctly reports the failure through its own diagnostics
(`scOk:0`, error-class buckets). It was restored to the bench Opta (v2.1.4) at the end of testing.

---

## 2. Timeline Reconstructed from Git + Review Docs + Repo Memory

| Date | Event | Outcome |
|---|---|---|
| 2026-01-13/15 | Feasibility study; solar/battery monitoring added (`6a7ef6b`) | Code merged, unverified on hardware |
| 2026-04-21 | First bring-up campaign (`SUNSAVER_MODBUS_BRINGUP_FAILURE_04212026.md`) | **FAIL** — every listen window returned a single `0x00` artifact. 7 FCs × 247 slave IDs × parities × timings exhausted |
| 2026-04-22 AM | Morningstar support: **Terminal A = DATA− / B = DATA+** (inverted vs. modern convention) → wires crossed | MRC-1 LED went from steady-green to **flickering on each poll** — physical layer restored |
| 2026-04-22 PM | **BREAKTHROUGH** (`977ca70`): (1) `RS485.setDelays(0, 1200)` — Opta corrupts the last TX byte unless post-delay ≥ 1 char time; (2) register map fixed to `0x0008..0x000C` (adc_*_f block), scale 96.667/32768 | **CRC-valid live data captured**: `bv=12.23 V, av/ic/lc plausible`. `SolarPoll comm=OK err=0` |
| 2026-04-23 | Chemistry verification via charge setpoints (`2e0dc9b`, `33fbb7e`); config plumbing (`93ecf39`) | Working link assumed |
| 2026-06-08 | v1.8.4 hardening (`d8e8441`) + three comprehensive reviews (F-1…F-7 etc.) | Reviews describe the link as "functional for the verified production subset" |
| 2026-06-23..25 | v2.0.43–v2.0.53: individual-register reads, bounded retry, fastFail, signed-current fix, timeout cap, error-class buckets (`merrTo/merrCrc/merrIda/...`), baud-scaled post-delay (Fix S1) | Extensive **observability** added; no live-link re-verification recorded |
| **2026-06-26** | Deep-dive diagnostics session (`5b68a31`, `CODE_REVIEW_06262026_...RESTORE.md`): historical `sunsaver-rs485-raw` replayed **exactly**, autoscan sweeps, OptaView v0.1 interactive probe, passive sniff | **ALL SILENT — 0 bytes.** Verdict: "any lack of communication is occurring externally (loose physical contact, lack of MeterBus power, or unpowered MRC-1)" |
| 2026-06-26 → 07-02 | Bench rewired multiple times for DAC/PWM current-loop campaigns (v2.1.0–v2.1.4); P1 MOSFET fried in an earlier bench event | RS-485 wiring visually intact per operator |
| **2026-07-02** | **This session** (§5) | Break localized to MRC-1 receive path |

**The critical unknown between 04-23 and 06-26:** no captured evidence exists of the link working
after 2026-04-23. Somewhere in that window the physical path stopped answering, and no code change
in that window plausibly explains it (the 06-26 session proved that by replaying the exact
2026-04-22 firmware — still silent).

---

## 3. Production Code State (what's already implemented and correct)

The client already contains a complete, review-hardened implementation — **there is no missing
"working system" to port in.** [TankAlarm_Solar.cpp](../TankAlarm-112025-Common/src/TankAlarm_Solar.cpp) /
[TankAlarm_Solar.h](../TankAlarm-112025-Common/src/TankAlarm_Solar.h):

- **Transport:** `ModbusRTUClient.begin(sSolarRS485 /* Serial2, PB_10, PB_14, PB_13 */, baud, SERIAL_8N2)`;
  timeout clamped ≥ 500 ms (MRC-1 latency); **baud-scaled post-TX delay** `(11e6/baud)+50 µs`
  (1195 µs @ 9600) — the forum-thread #1421875 fix that made April's breakthrough work, later
  generalized for all baud rates (Fix S1, v2.0.49).
- **Reads:** verified register block only — `0x0008` battV, `0x0009` arrayV, `0x000B` chargeI,
  `0x000C` loadI (adc_*_f), scaled 96.667/32768 (V) and 79.16/32768 (A, **signed** — v2.0.45 fix);
  read **one register per transaction** (the MRC-1 was never proven to answer multi-register reads);
  FC03↔FC04 fallback with cached FC; bounded retry; `fastFail` skip on consecutive failures so a
  dead bus can't starve the pulse sampler; RX drain before each transaction; watchdog kicks.
- **Unverified registers** (temps `0x001B/1C`, status `0x002B/2C/2E/2F`, daily stats) are
  compile-gated behind `SOLAR_ENABLE_UNVERIFIED_REGISTERS` because the 04-22 bench captured
  implausible values (`faults=0x4235`, `cs=Unknown`) at those addresses on this firmware revision.
- **Observability** (v2.0.42–v2.0.53): per-poll `scOk`, `scOkEver`, `scErr` tag (`to/crc/ida/ifu/?`)
  in telemetry; `merr` + per-class buckets (`merrTo/merrCrc/merrIda/merrIfu/merrOth`), `maddr`,
  `mms`, `merrTxt`, `scInit`, `scImpl` in the daily report. A field `scOk:0` is remotely classifiable.
- **Charge setpoint reads** (`0x0033/0x0035/0x0036`) for battery-chemistry/DIP verification —
  read-only by explicit decision (do NOT write SunSaver setpoints over Modbus).
- **Integration:** poll cadence from config (`pollIntervalSec`, 60 s default), 5-poll smoothed
  `communicationOk`, battery-voltage source priority MPPT → Vin divider, comm-failure alert plumbed
  (`alertOnCommFailure`, default false).

Current bench serial (v2.1.3/v2.1.4, config `solarCharger.enabled:true`) shows the expected honest
failure signature: `Solar: Modbus communication failure (N consecutive errors)` incrementing once
per ~60 s poll — the counter served as a device-uptime clock throughout today's session.

---

## 4. Reference-Doc Cross-Check

**Morningstar SunSaver MPPT Modbus spec** (PDF; not machine-extractable today — values below are
from the 04-22 bench-verified transcriptions already in the repo):
9600 baud, **8N2**, default slave ID 1, MODBUS protocol per spec §"Modbus Protocol"; filtered ADC
block at `0x0008..0x000C`; voltage scale `n·96.667/32768`, current scale `n·79.16/32768` (signed).
All match the production header [TankAlarm_Solar.h](../TankAlarm-112025-Common/src/TankAlarm_Solar.h).
**DIP switch 4 must be OFF for MODBUS** protocol mode (ON = MeterBus slave protocol) — reviewed in
April; user confirmed DIP4 state was validated with power cycles during the April campaign.

**Opta RS-485 tutorial (Finder):** recommends `setDelays(preDelay, postDelay)` computed as
3.5 char-times at the configured baud (≈ 3646 µs @ 9600 8N1). Production uses pre=0, post≈1195 µs
(1 char-time + margin). Note the tutorial's example is Opta↔Opta at 115200 and does **not** mention
the last-byte-corruption behavior; the 1-char-time post-delay was empirically proven sufficient in
April (live CRC-valid data). The tutorial confirms nothing about our failure — it's a getting-started
document — but it validates the general delay-bracket approach the code already uses.
A possible future experiment is the full 3.5-char pre+post delay per the tutorial, but April's
success with the current values makes this a low-probability fix (§7 T-6).

---

## 5. Today's Bench Test Results (2026-07-02)

Production v2.1.4 was temporarily replaced by diagnostic sketches (DFU, `security=sien`), then
restored. Operator (user) provided physical observations; all serial captures are in `build\`.

### 5.1 Test 1 — Production client polling (passive observation)
- v2.1.3/v2.1.4 client, `solarCharger.enabled:true` (Cox Wellhead config): every poll fails;
  `Solar: Modbus communication failure` counter incremented once per minute across the entire
  session (counter reached 128+ over ~2 h with zero successes).
- **Operator observation: MRC-1 LED solid green** (historically = "power OK, no data on bus").

### 5.2 Test 2 — `firmware/sunsaver-rs485-raw` (the April-proven register sweep)
- Flashed 17:45; captured 4 min (`build/monitor_v2100.txt` @ 17:47).
- TX frames verified byte-exact on serial, e.g. `01 04 00 2B 00 01 41 C2` (FC04, reg 0x002B, CRC
  valid), sweeping the full live+candidate register list under 8N2 and 8N1, slave 1, 800 ms listens.
- **Result: `[0 bytes]` on every single probe.**
- **Notable change vs April:** even the single-`0x00` DE-flip self-echo artifact (54 occurrences
  in April's 95 s capture) is **gone**. The RX path hears absolutely nothing — not even the Opta's
  own transceiver glitch. (Consistent with the 06-26 session's identical finding.)
- **Operator observation during active 3 s polling: MRC-1 LED still solid green** — no per-poll
  flick (April's working link produced two visible flashes per poll cycle).

### 5.3 Test 3 — `firmware/sunsaver-tx-stress` (continuous-drive discriminator)
- First upload attempt failed silently (dfu-util exit 74) — observations taken during that window
  were discarded; re-flashed successfully (upload EXIT 0).
- Serial heartbeat confirmed **continuous transmission**: `tx_bytes` climbing 1200 bytes per
  1.25 s heartbeat (≈ 960 B/s ≈ full 9600-baud utilization) for 5+ minutes (274,000 → 295,600+).
- **Operator measurement: ~1 V AC across A–B** with a basic multimeter while the stress traffic
  ran — consistent with a driven differential pair read by a bandwidth-limited meter (a 4.8 kHz
  ±2 V square wave under-reads on a 50/60 Hz meter). Reading was **the same at both measurement
  points** (repeat measurement), supporting end-to-end conductor continuity.
- **Operator observation: MRC-1 LED remained solid green throughout.** In April, far sparser
  traffic (one 10 s-cycle poll) produced visible LED flicker, and the original TX-stress run
  visibly changed the LED — that is how the TX path was first proven. Today the same unmissable
  stimulus produces no reaction.

### 5.4 Caveat (explicitly not yet falsified)
The ~1 V AC has **no idle-bus baseline measurement**. A floating/biased pair on a crude meter can
show phantom mains pickup. If an idle measurement (client restored, between polls — 59 of every
60 s) also reads ~1 V AC, then the "Opta is driving" conclusion weakens and a dead Opta
transceiver returns to the suspect list. **First follow-up action: idle baseline (§7 T-1).**

### 5.5 Verdict
With the caveat above, the failure localizes to: **the MRC-1 does not recognize (or cannot
receive) bus traffic that is demonstrably present at its terminals.** Combined with 06-26's
passive-sniff silence (nothing transmitted from the MRC-1 side either, ever), the adapter is
electrically present (LED lit, biasing likely intact) but functionally inert on its RS-485 side.

---

## 6. Root-Cause Candidates (ranked)

| # | Hypothesis | Consistent with | Against | Test that decides it |
|---|---|---|---|---|
| 1 | **MRC-1 RS-485 receive path damaged** (ESD/overvoltage during bench rewiring; the same bench killed a P1 MOSFET) | All of §5; LED power-only; worked in April, bench disturbed since | None known | Substitute adapter (T-4) or drive from server Opta (T-3) |
| 2 | **MRC-1 RS-485-side power rail off** (485 PWR switch / supply topology changed since April; April notes show the switch position mattered and was experimented with) | LED can be lit from MeterBus side while 485 side is unpowered | Operator verified hookup incl. switch ON | Re-verify switch + measure DC bias across A–B idle (T-2) |
| 3 | **Phantom AC reading; Opta transceiver actually dead** | Fried-P1 history proves damaging events occurred on this bench | 1 V AC at both ends; sketch heartbeat healthy | Idle-baseline AC (T-1); server-Opta swap (T-3) |
| 4 | **SunSaver/MeterBus side dead** (RJ-11 cable, SunSaver MeterBus port) | Would explain no replies | Does NOT explain MRC-1 ignoring bus-side traffic (LED should still flick on RS-485 activity) | Only matters after 1–3 cleared; MSView/RJ-11 test (T-5) |
| 5 | Polarity/bias/termination subtlety | — | Crossed wiring proven correct in April and re-verified; steady green ≠ the documented reversed-polarity signature (steady amber) | Included in T-3 by wiring to known-good conventions |

---

## 7. Recommended Next Tests (cheap → decisive)

- **T-1 — Idle AC baseline (2 min, closes §5.4):** with production client running (bus idle
  between polls), measure AC across A–B. **≈0 V** → today's 1 V was real drive → Opta exonerated,
  MRC-1 condemned. **≈1 V** → phantom pickup → rerun TX-stress and measure **DC** A-to-GND and
  B-to-GND instead (driving alternates both around mid-bias; a dead driver leaves fixed rails).
- **T-2 — MRC-1 DC bias check (2 min):** bus idle, DC across A–B at the MRC-1 terminals. A healthy
  biased RS-485 segment idles at roughly +0.2…+0.5 V (B−A per Morningstar naming). ~0 V hard →
  485-side of the adapter likely unpowered (supports #2).
- **T-3 — Server-Opta substitution (15 min, no purchases):** the bench server Opta has the same
  RS-485 port. Move the A/B/GND wires to it, flash `sunsaver-tx-stress` → LED reacts = client
  Opta's transceiver was the problem (#3). No reaction = MRC-1/downstream confirmed (#1/#2),
  since two independent known-good(ish) drivers then failed identically.
- **T-4 — Substitute the MRC-1 (definitive for #1):** second MRC-1 (~$40) — or Morningstar
  support/RMA referencing the April working capture and these findings.
- **T-5 — SunSaver-side check (after 1–4):** MSView + Morningstar's USB MeterBus adapter, or
  RJ-11 continuity + MeterBus supply voltage check, to verify the SunSaver's port separately.
- **T-6 — (low probability) tutorial-style 3.5-char pre/post delays:** only if everything
  electrical passes and frames still vanish; April's success without it argues no.

## 8. Implementation Options for Afterwards (deferred per user instruction)

1. **Fix the hardware, change no code** — the production stack is complete and was proven
   end-to-end in April; when the adapter path is restored it should resume with zero firmware work
   (the diagnostics will show `scOk:1` within one poll).
2. **Resilience if MPPT stays offline:** already handled — battery voltage falls back to the Vin
   divider; telemetry carries `scOk:0` + error class; no firmware change required. Optionally
   set `solarCharger.enabled:false` in the bench config until hardware is fixed to silence the
   per-minute failure logs (one config push).
3. **Consider `alertOnCommFailure:true` for field units** once the link is trustworthy, so a field
   MRC-1 failure like this one pages someone instead of being discovered months later. (The April
   note says a comm-failure alert exists but is default-off; CODE_REVIEW_04182026 flagged its
   reachability — re-review at implementation time.)
4. **OptaView** (`OptaView/OptaView.ino`, v0.1) remains available as the interactive bring-up
   console (probe/read/write/raw/sniff) once hardware is repaired.

---

## 9. Session Artifacts

- `build/monitor_v2100.txt` — rolling serial captures (raw sweep @ 17:47, TX-stress heartbeat @ 18:04)
- `build/ssraw_build.txt`, `build/sstx_build.txt` — diagnostic build/flash logs (note the silent
  dfu-util exit-74 failure on the first TX-stress attempt — always check `UPLOAD EXIT` before
  trusting hardware observations)
- `build/solar_git.txt` — git history extraction
- Production restored: CI-signed `TankAlarm-Client-secure-v2.1.4.slot.bin` re-flashed after testing;
  boot + config (`Cox Wellhead`, pwm@P2) verified retained.

**End of report.** Decision point for the user: order of T-1…T-5, and whether to disable bench
solar polling until the MRC-1 path is repaired.
