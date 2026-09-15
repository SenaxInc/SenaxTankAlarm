# Website Pull Requests #314–#317 — What Changed, How It Was Verified, What Remains

**Date:** September 15, 2026
**Author:** Claude (Claude Code session on the review machine)
**Scope:** the four website pull requests that implement the website items of `CODE_REVIEW_09092026_MASTER_REPOSITORY_REVIEW.md` (S-W01, S-W02, S-W03, S-W05, S-W06, S-W07), the eleven Copilot review rounds on #315, and the merge into `master`.
**Baseline:** `master` at `99b5b34` (firmware 2.2.14) before the PRs; the PRs do not change `FIRMWARE_VERSION`, so no firmware binaries were rebuilt by CI.

---

## 1. Summary

| PR | Branch | Commits | Merge | Master-review items |
|---|---|---|---|---|
| [#314](https://github.com/SenaxInc/SenaxTankAlarm/pull/314) | `website-style-consistency` | 2 | `1b516d9` | S-W05 (style, layout, checkbox vocabulary) |
| [#315](https://github.com/SenaxInc/SenaxTankAlarm/pull/315) | `website-handlers-and-clamps` | 3 | `2aaecf1` | S-W02 (dead controls), UID hardening, config clamps, page checker + CI |
| [#316](https://github.com/SenaxInc/SenaxTankAlarm/pull/316) | `website-config-roundtrip-calibration` | 1 | `31a80bd` | S-W01 (config round-trip), S-W03 (calibration identity and validation) |
| [#317](https://github.com/SenaxInc/SenaxTankAlarm/pull/317) | `website-status-and-feedback` | 1 | `2e2c97c` | S-W06 (operator status), S-W07 (logs, feedback, secondary APIs) |

All changes are confined to `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino`, the new `TankAlarm-112025-Server-BluesOpta/check_web_pages.py`, and `.github/workflows/arduino-ci-112025.yml`. The client and viewer sketches are untouched.

**Why the PRs were stacked.** Each embedded page is a `PROGMEM` raw string that sits on a handful of 50–100 KB source lines, so any two branches that edit the same page conflict textually in git even when the edits are unrelated. The PRs were therefore built as a stack (#314 → #315 → #316 → #317) and merged in that order with merge commits. Every branch was rebuilt from the branch below it by re-applying its edits as literal, segment-break-tolerant text replacements (the pages are split by `)HTML" R"HTML(` markers at arbitrary offsets), never by `git rebase`. This is also the recommended way to rework any of these pages in the future: express edits as anchored text replacements against the `.ino` and assert the anchor count.

---

## 2. What each pull request changed

### 2.1 #314 — style and layout consistency (S-W05)

- One header and navigation strip on every authenticated page (Dashboard · Client Console · History · Contacts · Server Settings), with the active section marked; the header collapses to a horizontally scrolling pill strip on phones (212 px → 112 px at 375×812).
- Tokens the pages referenced but nothing defined (`--card-bg`, `--accent`, `--chart-grid`), rules for about 45 classes that were emitted unstyled, AA-contrast status tokens.
- Toast hidden by `opacity`/`visibility` instead of a translate that left a blue strip pinned in the corner; tooltips wrap inside their bubble; wide tables scroll inside their card below 720 px.
- Contacts rendered as a real `<table>` so the SMS and Email checkboxes share a column line; a site-wide checkbox vocabulary (`.check-box` / `.toggle` boxed fields the same height as inputs); the config generator's Stuck Sensor Detection and Calibration Learning as boxed fields.
- Dashboard stale label rendered from `STALE_MIN`; `<h2>` card titles on the SMS and Email setup pages; `/contacts` error toasts get the shared `isError` colouring.

Residual: about 60 inline status hex colours still bypass the `--ok/--warn/--bad` tokens; the viewer's separate stylesheet is untouched (tracked as V-W01).

### 2.2 #315 — dead controls, UID hardening, clamps, page checker (S-W02)

**Commit 1 — bind six dead controls programmatically, restrict client UIDs, clamp config inputs, add page checker to CI.**
Every page script runs inside an async IIFE, so a `function name(){}` declared there was invisible to the inline `onclick="name()"` attributes that called it: Expect Update and Remove Client on `/site-config`, Reset and the sensor-name link on `/calibration`, Cancel on `/transmission-log`, and the custom date range on `/historical` threw `ReferenceError` on every click. Those attributes also spliced client UIDs into JavaScript source, so a UID containing a quote would have executed code the moment the handlers worked. The controls now carry their UID or key in HTML-escaped `data-*` attributes and are handled by one delegated listener per page; the handler functions stay closure-local.
`isValidClientUid()` (telemetry, alarm and daily ingestion) additionally requires letters, digits, `:`, `_`, `-` or `.` after `dev:` and rejects the bare prefix; the same check is enforced in `findOrCreateClientMetadata()`, `findOrCreateClientSerialBuffer()`, `handleUnload()`, `handleConfigPost()` (which also answers a `config` without a `client` with 400 instead of a silent 200) and the operator handlers that read `clientUid` (calibration POST/DELETE and two more). Rejected UIDs are logged through a sanitising printer.
Config generator clamps: Sample Minutes max 1440 → 1092 (the client stores `sampleSeconds` in a `uint16_t`; CLAUDE M-08) and momentary relay durations max 86400 → 65535 (`relayMomentarySeconds[]` is a `uint16_t`; CLAUDE M-06), parsed with `Number()` so exponent input is read correctly.
`check_web_pages.py` and a `check-web-pages` CI job: see section 4.

**Commit 2 — bind the dashboard and client-console controls programmatically too.**
Update, Snooze Reminders, Clear Relay and Remove Client on the dashboard and Edit Config(uration) and Approve Deletion on the client console spliced UIDs the same way (`encodeURIComponent()` does not encode a single quote either); they use data attributes and delegated listeners now, and the client console's UID readout is HTML-escaped.

**Commit 3 — attribute-safe escaping on every page; validate persisted UIDs at boot.**
Six pages implemented `escapeHtml()` by serialising a text node, which escapes `& < >` but not quotes; every page now uses the five-character escaper the dashboard already had. Records persisted before the UID character set existed (sensor registry, calibrations and their summaries, calibration log entries served by `/api/calibration`, hot-tier history, FTP cache summaries, client config snapshots, client metadata) are validated on the complete stored value before it is copied into a fixed-size field, and skipped with a serial warning otherwise; a registry that dropped a record is marked dirty so it is re-saved clean. The config generator's cloud client picker, the last control that put a UID into an inline handler, was converted as well.

### 2.3 #316 — config round-trip and calibration identity (S-W01, S-W03)

- `loadConfig()` restored the 4-20 mA subtype before `updateSensorTypeFields()` rebuilt the select (ultrasonic came back as pressure), never unchecked the threshold a config did not carry (a low-only alarm re-acquired the template's high alarm of 100), and never restored the float-switch trigger (`not_activated` became `activated`). All three are fixed; `alarmsEnabled`, `alarmSms` (a new **SMS Alerts** checkbox in the alarm section) and `digitalTrigger` round-trip independently, and an explicit `alarmsEnabled:false` keeps the section closed.
- The calibration page split sensor keys on the first colon, so a `dev:…:1` key produced `clientUid:"dev"`. A `parseSensorKey()` helper splits at the last colon and requires a 1–255 integer suffix at all three sites; the form requires an explicit numeric level (a blank field is no longer submitted as 0, while an explicit 0 is allowed).
- `POST /api/calibration` tested presence with `!doc[key]`, which rejected a verified level of 0 in (the empty-tank / 4 mA anchor). Presence is now a type check (`is<float>()` accepts integer and float JSON in ArduinoJson 7; `is<uint8_t>()` is a type and range test), sensor indexes are 1-based so 0 is rejected, the level must be finite and non-negative, and the client/sensor pair must resolve to a sensor record before any NWS lookup or file write. `DELETE` returns 404 for a pair with neither a sensor record nor calibration data instead of rewriting the calibration log.

### 2.4 #317 — operator status and feedback (S-W06, S-W07)

- Dashboard: float switches read ON/OFF with no inches delta or sparkline; a level of exactly 0 in is a value, not `--`; an "Update requested" pill expires after the client's reply window and the button returns as **Retry Update** with polling back to normal; `/api/clients` keeps the alarming sensor's `at` instead of the newest sensor's; Site Config maps `flt`/`ma` so its FAULT badge and mA readout work; the outdated-client count uses `compareFirmwareVersions()`; Client Console deletion confirms instead of prompting for a PIN the server ignores.
- Transmission Log filter options are built from the vocabulary `logTransmission()` emits plus the loaded entries (`error`, `ota`, `alarm`, `applied`, … were unreachable) and the CSV export uses the table's filter including the detail search; Contacts and Server Settings show `saved:false` as an error; route alias / FTP user / FTP password are rejected above 31 characters with `maxlength` on the inputs; `GET /api/location` URL-decodes `client=`; year-over-year history splits the key at the last colon; login shows the 429/400 reason; the Config Generator starts a new configuration for an unknown UID, caps sensors at 8 in the page and in `handleConfigPost`, and refuses to derive the daily report time from a guessed 05:00 UTC; Server Settings loads its settings once per page open.

---

## 3. Verification

Every change was measured in a browser against a loopback fixture server that served the pages extracted from the branch under test with placeholder data (`Site A`, `Client A`, `dev:000000000001`); the numbers in the PR bodies are DOM measurements and captured request payloads, not estimates. Highlights:

- Each converted control runs its handler through the delegated listener with the expected payload (calibration Reset → DELETE with the row's UID and index; Expect Update → POST `/api/ota/expect`; Cancel → POST `/api/config/cancel`; dashboard Update/Snooze/Clear Relay/Remove Client → their four endpoints; client picker → `/api/client?uid=`). A fixture row whose UID contains `"><img src=x onerror=alert(1)>');alert(2);//` renders as inert text, the handler receives it verbatim, and `alert` is never called.
- Config round-trip: a load → download pass of ultrasonic/pressure/digital sensors with one-sided and zero thresholds, relay policy and loop-power mode reports no differences, and a second pass is byte-identical.
- Calibration: `dev:000000000001:1` posts `sensorIndex: 1`; a blank form posts nothing and shows an error; an explicit 0 in posts `verifiedLevelInches: 0`.
- Clamps: Sample Minutes `5000` → `sampleSeconds: 65520`; `1e3` → `60000`; relay durations `99999 / -5 / 3600 / abc` → `[65535, 0, 3600, 0]`.
- Every branch compiled at `arduino:mbed_opta 4.5.0` with `--warnings all`; the warning set matches `master` (one warning fewer from #316 onward, a variable that is now used). Final flash: #315 1,022,836 B, #316 1,025,028 B, #317 1,028,012 B (all 52 %); static RAM unchanged at 360,720 B.
- CI: `compile-check` and `check-web-pages` passed on every push of every PR; one `compile-check` failure on #316 was a transient library download error and passed on re-run.

The round-trip and handler checks are browser-driven, not CI tests; a headless version would need a DOM runtime the runner does not have.

---

## 4. The page checker (`check_web_pages.py`)

Extracts every `PROGMEM` page from the server and viewer sketches (joining the `)HTML" R"HTML(` segments), runs `node --check` on each `<script>` block, and asserts that every function called from an inline `on<event>` attribute resolves to something the page exposes at load time. Rules as of the final revision:

- attributes are matched case-insensitively, quoted or unquoted, with optional whitespace around `=`, including attributes inside JS template strings; every call in the value is checked after string literals, comments, regex literals and `${…}` splices (lexed, so a `{` in a string or regex cannot swallow the rest) are removed; method calls and callable browser globals are skipped;
- a `window.fn =` assignment counts only when it starts a statement, every enclosing function body is an IIFE that is a statement of its own or a `load`/`DOMContentLoaded` listener, no conditional block or unbraced conditional encloses it, nothing precedes it that exits the block unconditionally, and the assigned value is a function expression, an arrow, or the name of a function declared in that scope or an enclosing one;
- a depth-0 `function name(` counts only when it starts a statement (named function expressions do not);
- it fails closed: a missing `node` (scan and self-test), a missing sketch, or a sketch yielding no pages is a failure;
- `--selftest` runs 60 synthetic pages covering each rule.

The `check-web-pages` job runs the self-test and the scan on every push and pull request; `build-firmware` depends on it, and the workflow's `pull_request` trigger no longer filters on the base branch so stacked PRs get checks. Before #315 the scan reported exactly the six dead handlers; after it, 17 pages, 0 problems (the stylesheet page has nothing to check).

---

## 5. Copilot review rounds on #315

Copilot could not review #314, #316 or #317 (the single-line pages exceed its file limits). On #315 it produced eleven rounds of inline comments after successive pushes; every comment was either fixed or recorded, and the final verdict changed from "changes recommended" to "needs a closer look" with no new inline comments.

| Round | Comments | Outcome |
|---|---|---|
| 1 | regex after keywords, event allow-list, missing sketch/node/pages fail open, textual `window.x=` detection, `build-firmware` not gated | all fixed |
| 2 | inline handlers splice UIDs into JS, self-test not run in CI, only the first call checked, expression-bodied arrows | all fixed (delegated binding, UID character set) |
| 3 | `escapeHtml` does not escape quotes, `dev:` with empty suffix, conditional blocks, named function expressions | all fixed |
| 4 | validate before truncating copy, case-insensitive/unquoted attributes, self-test without node | all fixed |
| 5 | client picker still inline, empty ids counted, unbraced conditionals | all fixed |
| 6 | sensor registry not validated, rejected UIDs printed verbatim, conditionally invoked IIFEs | all fixed |
| 7 | Clear Relay targets the monitor by array position (pre-existing), snapshot log printed verbatim, `window.x = undefined` counted | first recorded as **S-T06**; the other two fixed |
| 8 | `parseInt` misreads exponent input, declaration scope for `window.x = f` | all fixed |
| 9 | `config` without `client` returns 200, splice lexing, exports after `return` | all fixed |
| 10 | `findOrCreateClientMetadata()` unvalidated, regex inside a splice | all fixed |
| 11 | calibration handlers accept any UID, `location` not callable; then serial-log/unload ingress, calls inside strings, case-sensitive prefilter | all fixed |
| 12 | (no inline comments; one suppressed note about a nested bare block after `return`) | left as is |

Two Copilot claims on #316 were refuted against the ArduinoJson 7.4.3 source: `is<float>()` accepts integer JSON (`Converter<float>::checkJson` → `isFloat()` → `NumberBit`), and `is<uint8_t>()` range-checks (`isInteger<T>()` → `canConvertNumber<T>()`). One claim was confirmed against the code and corrected in the PR: sensor indexes are 1-based (the client numbers its first monitor 1 and `handleTelemetry` rejects `k < 1`), so index 0 is rejected rather than treated as "the first sensor".

---

## 6. Left open (tracked in `TODO.md`)

- **S-T06** (new): the dashboard's Clear Relay sends the sensor's position in the `/api/clients` `ts[]` array, which the client applies as its monitor-array index; a client whose sensors report out of order can clear the wrong monitor. Needs a server/client contract change.
- **S-W05 residual**: inline status hex colours; viewer stylesheet parity (V-W01).
- **S-W06 residual**: the 49-hour stale policy is unchanged by design.
- **S-W07 residual**: L-42 (SMS/email log entries carry no site/client and truncate detail to 47 characters).
- **Client side**: the client's `as<uint16_t>()` parse with its dead `> 86400` guard (CLAUDE M-06) and the `sampleSeconds` lower bound (M-08) are firmware changes and stay with the client items.
- `FIRMWARE_VERSION` is still 2.2.14; a bump is needed before CI publishes new server binaries.

---

## 7. Working-tree note

The review machine's working tree still holds byte-identical late-June copies of the client sketch, two Common headers, `TankAlarm_Solar.cpp` and the CI workflow, plus three uncommitted 2026-07-21 review documents. Nothing was committed from that tree; every branch above was built in a clean `git worktree` from `origin`. See `TODO.md` under C-P07.
