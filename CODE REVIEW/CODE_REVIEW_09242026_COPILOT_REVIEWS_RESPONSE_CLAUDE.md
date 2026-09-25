# Copilot reviews of 2026-09-24: verdicts and actions

**Date:** 2026-09-24
**Reviews answered:** three Copilot documents dated 2026-09-24, all on baseline `c9a9cce` (v2.2.16)
**PR heads checked:** #322 `79dbe3e`, #323 `b272d85`, #324 `1d3779b`, #325 `1e1b13a`, #326 `c8135ec` (none merged)
**Author:** Claude (Opus 5.5) with the repository owner

This records how each finding in the three Copilot reviews was checked, which findings were real, what was fixed and in which commit, what was rejected and why, and what stays open. Every claim was traced in the code on `master` and on the PR branches, not taken from the review text. For R01, R02, R03, CR-2, CR-3 and CR-8, where the documents disagree or the stakes were higher, two or three verifiers traced the claim independently. A second pass then checked the verdicts for gaps. The deferred items are in [TODO.md](TODO.md).

## The three documents

| Doc | File | Commit | Scope | Findings |
|---|---|---|---|---|
| 1 | [CODE_REVIEW_09242026_PRS_318_321_HISTORY_COPILOT.md](CODE_REVIEW_09242026_PRS_318_321_HISTORY_COPILOT.md) | `a1a2c16` | Merged PRs #318–#321 (v2.2.16), mostly history | R1–R4, four Other Suggestions, three Ruled-Out entries |
| 2 | [CODE_REVIEW_09242026_PRS_322_326_COPILOT.md](CODE_REVIEW_09242026_PRS_322_326_COPILOT.md) | `b8e6a90` | Open PRs #322–#326 | CR-1 to CR-10 |
| 3 | [CODE_REVIEW_09242026_PRS_318_321_COPILOT.md](CODE_REVIEW_09242026_PRS_318_321_COPILOT.md) | `7b4c8be` | Both groups | R01–R15 |

How they relate:

- **Doc 3 replaced doc 1.** It was committed under doc 1's file name about 18 minutes after doc 1, so doc 1 was no longer in the tree. This branch restores doc 1 byte for byte under its own name, with one added line that says where it came from. Docs 2 and 3 are left as written; their corrections are recorded here.
- **Doc 3 repeats most of docs 1 and 2:**
  - R03–R06 are doc 1's R1–R4.
  - R08 is CR-1, and R09 is CR-9.
  - R14 is the snooze half of CR-8.
  - Doc 3's own findings are R01, R02, R07, R10–R13 and R15.
- **Doc 3 contradicts the earlier documents twice:** doc 1 on R02, and doc 2 on CR-2. Doc 1 was right about R02, and doc 3 was right about CR-2; see [Contradictions and wrong attributions](#contradictions-and-wrong-attributions).

## Summary

- **Fixed in the open PRs:**
  - #325 `1e1b13a`: R03, R10, R11, CR-10a and CR-10b.
  - #323 `1e31145` and `b272d85`: CR-3, CR-4 and CR-5.
  - #326 `c8135ec`: R12, R13, CR-7 and CR-8 (R14).
- **Release gate, not a defect:** R08/CR-1. #324 ships only together with S5.
- **Real but low, deferred** to planned waves or the backlog: R04, R05, R07, the remote-OFF half of CR-1, CR-6, CR-10c, and alarm/snooze emails cut to SMS length.
- **Refuted:** R01, R02, CR-2, R15, and the "lost command" claim in R09/CR-9.
- **Partly right, no change:** R06, a closed, one-time limitation of the upgrade (at most one pre-upgrade sample per non-current-loop sensor).
- **Second round:** Copilot's comments of 2026-09-25 on #323–#327 are answered in [Follow-up](#follow-up-copilot-comments-of-2026-09-25).

**Field impact.** The field has one analog client with one current-loop sensor, and no relays or floats. Two findings reach it today, and both are small:

- R03: an exact 0 in a daily report becomes a gap in history.
- R11: each daily report zeroes an under-range mA reading on the dashboard.

Both are fixed in #325.

## Verdicts: merged code (doc 3 R01–R07, doc 1 R1–R4)

| ID | Verdict | Severity | Field impact today | Decisive evidence | Action |
|---|---|---|---|---|---|
| **R01** | Refuted | none | None | The server mounts LittleFS v1.7 (`mbed::LittleFileSystem`). Opta core 4.6.0's `lfs.h:328-334` allows a rename over an existing file. The disassembled `lfs_rename` in the Opta `libmbed.a` replaces the entry through `lfs_dir_update` and returns 0; it has no EEXIST path. Config, registry and hot-tier saves have relied on this since `09788a6` (2026-02-19), as the comment in [TankAlarm_Platform.h](../TankAlarm-112025-Common/src/TankAlarm_Platform.h) says. | No code change. Do not add the suggested move-aside, rename and delete sequence: it opens a window where the month file does not exist. Power loss and full flash during a rewrite stay bench items. |
| **R02** (doc 1 Ruled-Out) | Refuted | none | None | The badge compares the click time with `u`. `u` is the Notehub envelope time of the reply note, via `processNotefile` (:12491), `handleTelemetry` (:12951/:12965) and `/api/clients` (:10813), and it is always later than the click. The minute-rounded `t` is used only for history. | No code change. The suggested minute-level comparison would count a reading taken earlier in the same minute as the answer. |
| **R03** (doc 1 R1) | Confirmed | low | Rare. The field sensor reads exactly 0 only at 4.00 mA with no mount offset, or through a calibration clamp. If no telemetry came that day, the daily copy is dropped and the day becomes a gap. It would hit every inactive float every day. | `handleDaily` required `newLevel > 0.0f`, and telemetry has no such test. The test dates from `3861d65` (2026-02-23), and #319 (`5146a41`) kept it. Missing readings are already kept out by the per-sensor `t` and, for current loop, by a missing `ma`. Doc 3's `!ru && !sf` test is the wrong test, because those flags do not mark missing data. | **Fixed in #325 `1e1b13a`.** `dailyReadingAdmissible()` in `TankAlarm_DigitalDisplay.h` needs the entry's own `t` and a trusted level. Current loop also needs a raw mA in 4–20; other types need `lvl`, `fl` or `rm`. Held for the relay/float release (owner). |
| **R04** (doc 1 R2) | Confirmed | low | Probably none. The page never sends a poll interval, so the client polls the MPPT every 60 s and the Vin divider every 300 s. | `warmVinOnReadingDay()` assumes the voltage is at most 3600 s old. The client allows `solarCharger.pollIntervalSec` up to 3600 s and keeps the last MPPT data through failed polls. `vinMonitor.pollIntervalSec` has no upper limit, which doc 3 missed. Only the row's voltage is affected; levels are not. | Backlog: the client sends the voltage's age next to `v`, and the server uses it instead of 3600 s. CS-1 clamps the Vin poll to 5–3600 s. Do not just raise `WARM_VIN_MAX_AGE_SEC`. Doc 1's interim option (drop a voltage of unknown day) is covered by the age field. TODO S-D06 and C-P01. |
| **R05** (doc 1 R3) | Confirmed | low | Practically unreachable. With default settings, history gets about one reading per day, so a day cannot be partly overwritten. | `warmDecideVisitor()` keeps the stored row whenever its `n` is larger (`WarmTierStore.h:792`), so a higher `al` is dropped. A day with no readings left emits no row. Only the warm-history alarm count is affected; levels, emails and texts are not. | Backlog (TODO S-D07): raise only `al` on an existing row, and never create a row from an alarm alone. Needs tests with partial and empty rings. |
| **R06** (doc 1 R4) | Partly | low | None going forward | The dedupe does ignore the legacy mark. But legacy entries are created only on the first v2.2.16 boot, so a match needs a client copy whose `t` equals a reading taken before the upgrade. The client keeps sending that `t` until its next valid acquisition: usually the next sample (30 min by default), but longer while the sensor stays faulted (`ru`), while the solar voltage gate skips sampling, or with a larger `sampleSeconds` (up to 65535 s). The bound is therefore not a length of time. For current loop (the field sensor), a failed read carries `fault` and `ma_raw` with no `ma`, so neither `handleDaily` nor telemetry (`freshReading`) files it, and the legacy entry changes nothing. For other types, one copy would be filed and every later copy would dedupe against it: at most one pre-upgrade sample per sensor, on its own day only, never a later day. The upgrade day's row leaves out pre-upgrade samples by design, and earlier days keep their v2.2.15 rows (`warmDecideVisitor`, `n` tie-break); only a day that never got a row could have gained a one-sample row. | No change. It is a closed, one-time limitation of the upgrade: at most one pre-upgrade sample per non-current-loop sensor, none for current loop. |
| **R07** | Partly | low | None; there are no floats | Float alarm types do skip the 300 s spacing check. But with the 3-sample debounce and the page's 1-minute sample minimum, a float alarms at most once per 6 minutes, not "the hourly cap in seconds". That minimum exists only because the page sends `sampleMinutes*60`; the client accepts any `sampleSeconds`, including 0. The code is the same in v2.2.15, so it is not from #318. | CL-5: add `triggered`/`not_triggered` to the spacing check (TODO C-T05). CS-1: a client minimum for `sampleSeconds` (TODO C-P01). |

## Verdicts: open PRs #322–#326 (doc 3 R08–R15, doc 2 CR-1–CR-10)

| ID | Verdict | Severity | Field impact today | Decisive evidence | Action |
|---|---|---|---|---|---|
| **R08 / CR-1** | Already handled (release gate); the remote-OFF point is confirmed | low | None. #324 is unreleased, and the field client has no relay bindings. | The `master` server still sends `relay_reset_sensor` with the card's position in `ts[]`, does not check it, and answers 200. #324's client logs and ignores that key on purpose; its PR text says "update the server first". Separately, `resetRelayForMonitor` sends a remote OFF only for the mask tracked in RAM, which is 0 after a reboot. That code is unchanged since before v2.2.16. | #324 ships only with S5 (see [Release gates](#release-gates)). Reject R08's legacy fallback, because it brings back the wrong-sensor clear (S-T01). C-A02 wave: remote OFF for the monitor's whole configured relay mask, and a clearer log line (TODO C-A02; S5 scope under S-T01). |
| **R09 / CR-9** | Refuted (no command is lost); doc 2's point about 200 stands | low | None | The client reads one `relay.qi` note per inbound poll: every 60 s while awaiting config, 10 min on grid and 60 min on solar. Two commands therefore never reach `processRelayCommand` within 5 s in a field build, and queued notes wait for the next poll. The only exception is a bench build with `GRID_INBOUND_INTERVAL_MS` near its 10 s minimum. | No change to #324. Optional hardening in the C-T02 wave (TODO C-T02). S5 covers the "do not treat 200 as delivery" point. |
| **R10** | Partly | low | None in practice | The part-0 reconcile only searched existing records. But the registry is saved and reloaded at boot, so an ordinary reboot does not lose it (doc 3 is wrong there). The case needs a lost registry and a lost alarm note, and the next day's daily report sets the flag anyway. The code is from `73d5006`, not #325. | **Fixed in #325 `1e1b13a`.** The reconcile now calls `upsertSensorRecord` and fills an empty site from the note. `handleDaily` is not reordered: that would miss sensors in later parts and change what #325's sensor-type check reads. |
| **R11** | Partly | low | Cosmetic, and live in v2.2.16. Each daily report sets a 3.6–3.99 mA reading's stored mA to 0 until the next telemetry. | `handleDaily` kept `(mA >= 4.0f) ? mA : 0.0f`, which telemetry dropped in v2.0.52 (Fix 13). It was already listed as L-38 in the 2026-09-08 review, and it is not from #325. | **Fixed in #325 `1e1b13a`.** The raw mA is stored in both branches, and "no `ma` means 0" is kept for current loop. Held for the relay/float release (owner). |
| **R12** | Partly | low | None. The client sends `un` only as a positive whole number. | `handleAlarm` did read `un` with `containsKey` and `as<uint8_t>()`, so `null` cleared it. But ArduinoJson range-checks the conversion, so an out-of-range value clears to 0; it does not wrap. `handleAlarm` already set `gSensorRegistryDirty`. The code is older than #326, which left it out when it converted the other handlers. | **Fixed in #326 `c8135ec`**: `noteDisplayNumber(..., false, rec->userNumber)`, with the dirty flag set on a change. |
| **R13** | Confirmed | low | None visible. The field sensor is current loop, where "(0 mA)" correctly means no valid reading. | `sendDailyEmail` always sent `sensorMa`, and the script prints the suffix whenever the field is present. The code is older than #326. | **Fixed in #326 `c8135ec`**, on the server. `sensorMa` is sent only for current-loop sensors, plus an untyped record that holds a reading. No script change is needed. A script-side `> 0` filter would hide the useful current-loop "(0 mA)". |
| **R14** | See CR-8 | | | | |
| **R15** | Refuted | none | None; host tests only | `.gitattributes` sets `*.ino text eol=lf`, so every sketch checks out with LF even with `core.autocrlf=true`. The suite passes on a real checkout (59/59 at `0974dab`). The three failures appear only on a copy converted to CRLF outside git. | No change needed. Optional: strip CR after reading the sketch in each suite, so a stray CRLF copy cannot make the negative checks pass without testing anything (TODO X-R01). |
| **CR-2** | Refuted | none | None | On the current heads, `git merge-tree --write-tree` merges all 10 PR pairs cleanly. The five-way merge (#326, then #323, #324, #322) also has no conflict. At the earlier heads, the merged tree compiled for the Opta (53% flash, 69% RAM) and passed the host tests. Doc 2 read the old merge-tree label "changed in both" as a conflict. | No conflict rebase is needed. #326 is stacked on #325: merge both with merge commits, or rebase #326 if #325 is squashed. The merge note in #326 that expects small conflicts with #323 can be dropped. |
| **CR-3** | Partly (mechanism right, blame wrong) | medium | None on v2.2.16, which does not print the label. But a sensor whose name was left blank is stored as "Tank 1". Once #326 ships, its texts would read "<site> Tank 1 …". | `master` already fills a blank name with `Tank ${userNum\|\|index+1}`; #323 only swapped in the stable number. #326 is what first prints the label. | **Fixed in #323 `1e31145`**: a blank name becomes the type word only (`Tank`, `Gas System`, `Engine`). #326's branch still has `master`'s line, but the merged tree takes #323's version (checked). Names that are already stored are not renamed: #323's PR text tells operators to rename or clear them. |
| **CR-4** | Confirmed | low | None. It is reachable only by importing hand-edited JSON, because the server rejects invalid numbers on save. | `loadSensorNumbers` gave a missing or invalid number `position + 1` with no warning and without looking at `snh`. A sensor with no number could also take #1 and push the real #1 past the high mark. | **Fixed in #323 `1e31145` and `b272d85`:**<br>- Explicit numbers are kept first.<br>- With `snh`, a bad number is repaired past the mark and reported.<br>- Without `snh`, an unusable number keeps its position when that position is free, and is reported. The client runs it as `index + 1`.<br>- A 0, a duplicate, or a taken position still goes past the mark. |
| **CR-5** | Partly | low | None. No path copies the device's config over the server's copy. | The real gap was different: posting a config whose `snh` was missing or lower, for example by importing an old JSON file, lowered the server's mark. | **Fixed in #323 `1e31145`**: `handleConfigPost` stores the largest of the posted `snh`, the cached snapshot's mark and the highest posted number. The client does not keep `snh`, by design.<br>The page side (after an import or a send, the page's own mark could stay below the server's) is fixed in #323 `a35b328`; see [Follow-up](#follow-up-copilot-comments-of-2026-09-25). |
| **CR-6** | Partly | low | None. There is one sensor, the registry holds 64 records, and a stray record is pruned within about 72 h. | #323 accepts any `k` from 1 to 255 into the registry. Transport is integrity-checked, so a bad `k` would come from a firmware bug. The suggested filter (accept only a `k` in the cached config) would drop real alarms from a client whose new config is still pending, and it cannot help a client with no cached config. | Filter rejected. Optional: log a warning for an unknown number, and evict unknown-number records first (TODO S-T09). |
| **CR-7** | Partly | low | None. The client always sends a valid `k`. | `handleUnload` read `doc["k"].as<uint8_t>()` with no guard, and the same code is on `master`. On #326 the text no longer said "sensor 0". Instead it would read "<site> Tank unloaded" and route contacts as `<uid>_0`. | **Fixed in #326 `c8135ec`.** `handleAlarm` (sensor path) and `handleUnload` drop a note whose `k` is missing, not an integer, or below 1, before any record, log, SMS or email work. The unload path no longer writes the "Tank" placeholder.<br>Still open, and noted in #326's PR text: `handleTelemetry` and `handleDaily` still write "Tank" into an empty label (TODO S-T08). |
| **CR-8 / R14** | Partly | low | None from #326, which is not deployed. On v2.2.16 the SNOOZED text can lose "reply UNSNOOZE" only with a sensor-fault alarm, a site of 16 or more characters and a contact name of about 23 characters. | Labels are at most 23 bytes, so the only length difference between SMS and the daily email is a site of 24–31 bytes. Leading spaces also differed, because the C++ code trimmed only trailing spaces. Nothing overflows: `composeSensorText` keeps the tail, and "reply UNSNOOZE" survives in R14's own example. The contact name is already capped at 23 characters. | **Fixed in #326 `c8135ec`.** Site and label are trimmed of leading and trailing spaces and tabs. `composeSnoozeText` falls back to " Reply UNSNOOZE to resume." when the long hint does not fit. The 23-byte part cap stays (owner). Alarm and snooze emails still use the 160-byte SMS text (backlog, TODO S-T08). |
| **CR-10** | Confirmed (notes) | low | None | (a) `dailyReconcileAlarmType` trusted `y` before checking the type.<br>(b) The viewer titled a float card "Tank Level" (not "Tank"), while the dashboard says "Float Switch". The mismatch is older than #325.<br>(c) Display Number reads use `is<int32_t>()`, so `7.0` is ignored and the stored number is kept. #324's `relayJsonUint` accepts `7.0`. No sender produces a double today.<br>(d) The bench `x` command drives pin 10 for relay 4 on purpose. `optaLegacyRelayPin` appears only in the header, the bench sketch and the host test.<br>(e) CI for page scripts already exists: `check_web_pages.py` runs as the `check-web-pages` job (since `9ee192d`). It checks syntax and whether inline handlers can reach their functions. It does not catch calls to undefined functions. | (a) and (b): **fixed in #325 `1e1b13a`**. `y` is trusted only for an empty or digital type, and the viewer says "Float Switch".<br>(c) Open, low (TODO S-T08). A shared whole-number rule applies to the Display Number only; sensor-number reads must keep matching the client's `is<uint8_t>()`.<br>(d) Carry into CL-2 (see TODO C-A01).<br>(e) No change. Whether the job is a required status check is for the owner to confirm. |

Doc 2's claims about #322 were re-checked on `79dbe3e`, and they hold:

- Coil pins are 0–3, and the LED pins are 7, 9, 8 and 153.
- The `static_assert`s are compiled only under `ARDUINO_OPTA`.
- The CI bench-sketch step has `continue-on-error`, but the "Fail workflow if compilation failed" step still exits 1.
- `TankAlarm-112025-Common.zip` contains the same `TankAlarm_OptaIo.h`.

## Doc 1's suggestions and ruled-out entries

| Item | Verdict | Where it stands |
|---|---|---|
| **Suggestion 1:** handler-level coverage | Agreed | Done for the daily admission rule. #325 moved it into `dailyReadingAdmissible()`. Its tests build the inputs from JSON the same way the sketch does, and a sketch-text pin checks the call site. The legacy-load path (R4/R06) needs nothing, because R06 needs no code change. |
| **Suggestion 2:** #320 is best-effort, and the deployed script must be updated | Agreed | Best-effort delivery is already recorded (TODO S-T07): an extra copy is possible, and an email is never lost. Updating the deployed script is a release gate below; a firmware or server update does not replace a deployed Apps Script. |
| **Suggestion 3:** CI checks for page scripts | Already in place | The `check-web-pages` job covers syntax and inline-handler reachability. There is no check for undefined function calls and no browser smoke test. |
| **Suggestion 4:** hardware checks stay open | Agreed | See [Bench items that stay open](#bench-items-that-stay-open). |
| **Ruled-Out entries** (same-minute badge, Client Console `formatEpoch`, #318/#321) | All three stand | The badge entry is confirmed by the R02 trace. Only telemetry answers an update request, and its `u` is the delivery time. A daily report's per-sensor `t` is the last acquisition minute, which can be at or before `ureq`: a sample in the click minute, a report queued before the request, or a reused reading. Before #325 `4faedae`, `handleDaily` wrote that `t` into `lastUpdateEpoch` unconditionally, and it runs after `handleTelemetry` in `pollNotecard`. It could therefore move `u` backwards and bring back a badge that telemetry had cleared (after the wait window, a false "Retry Update"). Since `4faedae`, `handleDaily` never lowers `lastUpdateEpoch`. The defect was in v2.2.16, not from #325. See [Follow-up](#follow-up-copilot-comments-of-2026-09-25). |

## Contradictions and wrong attributions

**Between the documents:**

1. **R02 (doc 3) against doc 1's Ruled-Out entry.** Doc 1 was right. The live `u` comes from Notehub's delivery time, not from the client's minute-rounded reading time.
2. **CR-2 (doc 2) against doc 3's merge check.** Doc 3 was right: the branches merge cleanly.
3. **R01 (doc 3) against the code's own comment** in `TankAlarm_Platform.h` and seven months of field saves. Doc 1 listed rename semantics as an open bench check, which was the right way to put it.
4. **R08 (doc 3) against #324's design and doc 2's "What not to rework".** A legacy fallback would bring back the wrong-sensor clear.
5. **Doc 2 CR-8 against doc 3 R12.** Doc 2 says alarm notes are not used to update `un`. The client's `publishAlarmNote` does send `un`, and `handleAlarm` stores it before building the text.

**Wrong or imprecise attributions:**

- R03 names #319, but the gate dates from `3861d65` (February 2026).
- R10 and R11 name #325. R10 is from `73d5006`, and R11 is L-38 from the 2026-09-08 review.
- R07 names #318, but v2.2.15 had the same code.
- R12 and R13 name #326, but both are on `master`.
- R02 attributes the badge to the #318/#321 minute rounding, which is used only for history.
- CR-3 blames #323, but `master` already auto-names "Tank <n>".
- CR-7's "sensor 0" wording applies only to `master`.
- CR-1's remote-OFF point is older than #324.
- R15 names #323's test, and the same pattern is in every suite that reads a sketch. None of them fails on a real checkout.

**Mechanisms overstated:**

- R07 says the hourly cap is used up "in seconds"; at the fastest page settings, a float alarms at most once per 6 minutes.
- R09/CR-9 say commands are lost; notes are read one per poll, at least 60 s apart.
- R10 says a server reboot is enough; the registry survives reboots.
- R12 says values "wrap" and are not saved; they clear to 0, and the save flag is set.
- R14 says the text "overflows"; nothing overflows.
- R15's failures came from a CRLF copy made outside git.

**Doc 3 on its own terms:**

- The Executive Summary says "Fourteen findings (1 P1, 12 P2, 1 P3)". The table lists fifteen: R01–R15, with 1 P1, 13 P2 and 1 P3.
- Its "Phase 1 (Critical Hotfixes for v2.2.16)" lists R01 and R02, which are refuted, and R07, which cannot occur without floats.
- Its links omit `../`, so they do not resolve from `CODE REVIEW/`.

## Owner decisions (2026-09-24)

- **Naming rule, made earlier the same day.** Every SMS and email names a sensor as site + label, followed by " #<Display Number>" only when one is set. The internal sensor number is never printed. This rule is behind #326, CR-3 and CR-7.
- **R03 and R11 wait for the release.** Both are held for the single relay/float release and ship in #325 (`1e1b13a`), not as a separate server patch from `master`.
- **The SMS name part cap stays at 23 bytes**, not 31. A site of 24–31 bytes is shortened in texts and printed in full in the daily email.
- **How the records are kept:**
  - Doc 1 is restored under its own name (`..._HISTORY_COPILOT.md`), from `a1a2c16`.
  - Docs 2 and 3 are not edited.
  - This document and the TODO entries record the corrections.
  - The records go on a docs branch, not in a direct push to `master`.

## Release gates

These apply to the single relay/float release:

1. **#324 ships only together with S5, with the server first.** S5 must include:
   - The dashboard posts the sensor number `k`, not the card index. Today `data-idx` is the position in `/api/clients` `ts[]`.
   - The server validates `k` before it sends `relay_reset_sensor_number`. Today `handleRelayClearPost` passes the value through unchecked.
   - The success message does not claim delivery. A 200 means the note was queued, not that the relay cleared.
2. **Operators re-paste the Apps Script.**
   - The script to paste is on `/email-setup`; its first line is `// TankAlarm email bridge v2.2.17 (Display Number, float ON/OFF)`.
   - Redeploy it as a new version of the same Web App. A new deployment changes the URL and breaks the route.
   - The script carries #320's duplicate suppression and #326's naming. A server update does not replace a deployed script.
   - Until then, the one-line edit in #326's PR text stops the daily email from printing the internal number: replace `' #' + s.sensorIndex` with `(s.userNumber > 0 ? ' #' + s.userNumber : '')`.
3. **Check the field sensor's stored name** in the Config Generator (CR-3). An auto-name such as "Tank 1" would appear in texts once #326 ships.
4. **SendGrid users only:** test the JSONata daily line in Notehub's tester before saving it, as #326's PR text says. The live route uses the Apps Script.
5. **Merge #325 and #326 with merge commits**, or rebase #326 if #325 is squashed.

## Bench items that stay open

Host tests and library disassembly do not settle these:

- **LittleFS power loss and full flash.** A power cut during a month-file rewrite, and the ENOSPC behaviour. R01 settled only the rename semantics.
- **Worst-case warm-tier latency and heap**, measured with `-DTANKALARM_WARM_SELFTEST`.
- **Physical relay behaviour:**
  - the coils;
  - remote OFF;
  - the restore after a power cut;
  - the release on a sensor fault.
- **`OPTA_LED_ON_LEVEL`.** The header's value of 1 is a placeholder. Bench A2 measures it, and no firmware uses it before then.

The rest of the bench list in the [v2.2.16 review](CODE_REVIEW_09232026_PRS_318_321_CLAUDE.md) stands.

## Verification of the fixes

- **#325 `1e1b13a`:**
  - The digital_display suite adds admission cases.
    - Admitted: 4.00 mA with level 0, float `fl` 0, analog `lvl` 0, pulse `rm` 0.
    - Not admitted: no `t`, `t` = 0, a current-loop fault with no `ma`, 3.8 mA, 20.5 mA, voltage only, and an untrusted level.
  - It also adds the CR-10a cases, and sketch-text pins for R03, R11 and R10.
  - `viewer_cards_test.js`: 9 checks.
- **#323 `b272d85`:**
  - `generator_numbers_test.js`: 90 checks.
  - The sensor_numbers C++ suite: 145/145 under MSVC, debug and `-O2`. It also passes an `arm-none-eabi-g++ -Werror` syntax check for gnu++14 and gnu++17.
  - All node suites, `check_web_pages.py` and `--selftest` pass.
  - The server compiles for the Opta.
- **#326 `c8135ec`**, re-run after the fix:
  - sensor_name, MSVC debug and `-O2`: 36,079 checks.
  - digital_display: 113 checks.
  - email_bridge: 160 checks, 0 failures.
  - viewer_cards: 9 checks.
  - `check_web_pages.py`: 17 pages, 0 problems. `--selftest`: 0 failures.
- **Merges:** all 10 pairs of the current heads, and the five-way merge, complete without conflicts.
- **Not run here:** the Linux CI build with ASan/UBSan runs when the PRs are pushed, and no firmware was flashed.

## Follow-up: Copilot comments of 2026-09-25

Copilot reviewed #323–#327 again after the commits above. Each comment was traced on the PR head in the same way. The follow-up work is in [TODO.md](TODO.md).

| PR | Comment | Verdict | Outcome |
|---|---|---|---|
| #323 | The Config Generator takes its sensor-number mark only from the loaded file, so after importing an older file, or after a successful send, Add Sensor can hand out a retired number. | Confirmed, low | **Fixed in `a35b328`.** Before numbering an imported file, the page reads the server's stored mark (`GET /api/client`, which already returns the effective `snh`) and uses it as a floor, so repairs made during the load also land above it. A send answered with 200, WARNING or 202 raises the page's mark. The mark is never lowered. This closes the import bullet of TODO S-T09. The same commit trims the sensor name when the config is built (`collectConfig`). `generator_numbers_test.js`: 160 checks (90 before). |
| #324 | `relayScopeOf` treats a blank relay target as LOCAL, but the config struct says "empty = none". | Partly, low | **Comment only, `082bc24`.** The comment now states the plan's rule R6: a blank target or the client's own UID is LOCAL; "no relay" is `relayMask` 0; the new page's "This client" option writes a blank target. It adds that v2.2.16's tracked activation and restore paths still skip a blank target until CL-6b, and that in CL-4 the scope only labels the Clear Relay log line. The logic and its tests are unchanged. Converting the guards is a CL-6b follow-up (TODO C-A02). |
| #324 | `released` is decided and logged before the release, and a remote OFF can fail without any sign. | Partly, low | **Wording, `082bc24`.** The `RELAY_CLEAR_RELEASED` comment and the `.ino` comment now say that local coils are off, while a remote OFF was only requested: queued to the server, not confirmed, and not retried until C-A02/C-T02. For a remote release the log line says `off-requested` (`relayClearLogWord`). The enum, the log/release order and `resetRelayForMonitor` are unchanged. The failure paths are v2.2.16 code, not from #324. |
| #325 | `y` is trusted even on a lone low latch, which contradicts the header comment. | Refuted | **No logic change.** Floats latch only on the high channel, and a conforming client sends `y` only on high-latched float entries. The lone-low-latch rule belongs to the no-`y` fallback that uses `configTrigger`. The `TankAlarm_DigitalDisplay.h` comment was reworded in `4faedae` to separate the two rules. |
| #325 (via #327 R02) | `handleDaily` can move `lastUpdateEpoch` backwards. | Confirmed, low | **Fixed in `4faedae` and `bc83c98`.** `handleDaily` writes `lastUpdateEpoch` only when the value is newer. The daily missed-alarm reconcile no longer stamps the report time on an existing record. A record created by the reconcile or by the loop takes the reading's own time. Sketch-text pins cover both. The mismatch between `currentValue` and `lastUpdateEpoch` (low) stays as a note in the PR text. |
| #326 | `handleUnload` does not mark the sensor registry dirty. | Confirmed, low | **Fixed in `84d2145`.** `gSensorRegistryDirty` is set after the record writes, and the Display Number comes from `rec->userNumber` after the upsert. A sketch pin covers it. |
| #326 | Overview: names can be cut inside a UTF-8 character, and the trim rules differ between paths. | Partly, low | **Fixed in `84d2145` and `9678cb1`.** The SMS name part drops an incomplete UTF-8 tail (`utf8CompleteLen`), and so do the daily email's site and label (`utf8CompleteCopy`). The header comments say what each path trims: C++ trims spaces and tabs, Apps Script `trim()` strips Unicode whitespace, and JSONata `$trim` also collapses inner runs. The split characters come from the client, which copies names by bytes (TODO S-T08). The `84d2145` message cannot be amended on the published branch; #326's PR text corrects it. #326 takes the #325 fixes through merge `599f871`. |
| #327 | Comment 4099312992: R06's "at most about 30 min" is not a real bound. | Partly | **Corrected in this document.** The R06 row now gives the bound as at most one pre-upgrade sample per non-current-loop sensor, on its own day, and none for current loop. The action stays "No change". |
| #327 | Comment 4099313046: the Ruled-Out row says a later daily report still clears the badge. | Confirmed | **Corrected in this document.** Only telemetry answers the request, and until `4faedae` a daily report could bring the badge back. The code fix is the #325 row above. |
