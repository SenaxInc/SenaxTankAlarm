# Independent review of PRs #322–#326

**Date:** 2026-09-24
**Reviewer:** GitHub Copilot
**Baseline:** `master` at `c9a9cce` (v2.2.16)
**Method:** static review of the five open pull requests after #318–#321. Each finding below was checked against the PR head, not against the PR description. No firmware was changed, flashed, or committed except this document.

| PR | Head | Base | What it is |
|---|---|---|---|
| [#322](https://github.com/SenaxInc/SenaxTankAlarm/pull/322) | `79dbe3e` | `master` | CL-1 Opta I/O tables, input plan, host tests, bench sketch |
| [#323](https://github.com/SenaxInc/SenaxTankAlarm/pull/323) | `0974dab` | `master` | S-T01/S1 stable sensor numbers on the server |
| [#324](https://github.com/SenaxInc/SenaxTankAlarm/pull/324) | `1d3779b` | `master` | CL-4 Clear Relay by sensor number, client only |
| [#325](https://github.com/SenaxInc/SenaxTankAlarm/pull/325) | `0ffe804` | `master` | S3 float ON/OFF on server, viewer, and email |
| [#326](https://github.com/SenaxInc/SenaxTankAlarm/pull/326) | `00a9e01` | #325 | SMS/email names: site + label + Display Number |

#322, #323, #324, and #325 are independent branches off `master`. #326 is stacked on #325. None of them are merged.

## Recommendation

Do not merge #324 onto a server that still sends `relay_reset_sensor`, and do not merge #323 and #325/#326 without a rebase. Those two are the items that can cause a wrong operational result. The rest are identity and naming holes that should be fixed before the relay/float work depends on them, not reasons to reject the design.

| ID | Severity | PR | Finding |
|---|---|---|---|
| CR-1 | **High** | #324 | Clear Relay fail-closes against today's server. The dashboard still returns 200. |
| CR-2 | **High** | #323, #325, #326 | #323 conflicts with #325 and #326 in the server sketch. |
| CR-3 | **Medium** | #323, #326 | A blank sensor name embeds the internal number, and #326 then prints that name. |
| CR-4 | **Medium** | #323 | A missing or out-of-range number can be reattached to a retired identity with no toast. |
| CR-5 | **Medium** | #323 | `snh` is not in the client flash schema, so a device round-trip drops the high-water mark. |
| CR-6 | **Low** | #323 | Notes with `k` 64–255 now consume registry slots. Not an overflow. |
| CR-7 | **Low** | #326 | An unload note with a missing `k` is still turned into sensor 0 and can be texted. |
| CR-8 | **Low** | #326 | SMS and the daily email do not apply the same naming rule. |
| CR-9 | **Low** | #324 | A Clear Relay that arrives inside the 5 s cooldown is deleted and not retried. |
| CR-10 | **Note** | #325, #322 | Smaller display and bench items. Not defects in the code that shipped in these diffs. |

---

## CR-1 — High: #324 disables Clear Relay until the server half exists

**Source:** client `processRelayCommand` on `1d3779b`; server `sendRelayClearCommand` on `master` (unchanged by any of these PRs).

The client change is internally consistent. `relayClassifyCommand` accepts only `relay_reset_sensor_number` in 1–255, logs any other present value, and logs `relay_reset_sensor` without acting. A by-number command is resolved on the owning client and is not forwarded. `relayClearSensorNumber` is the host-tested step. The `_target` check is in the right place: a note routed to another UID is ignored before the cooldown is consumed.

The server half is not in this set. The dashboard still does this:

- The card's `data-idx` is the position in `/api/clients` `ts[]` (`sensorIdx: idx`), not `k`.
- `clearRelays()` POSTs that position as `sensorIdx`.
- `handleRelayClearPost` does not check it.
- `sendRelayClearCommand` writes `relay_reset_sensor` and `_target`, then the handler returns 200.

After #324, that note is deleted from `relay.qi` and logged as `legacy-ignored`. The operator sees "Relay clear command sent". Nothing on the Opta changes.

That is the fail-closed behavior the PR describes, and it is the right behavior for a wrong key. It is not safe to flash this client onto a unit whose server has not been updated to send `relay_reset_sensor_number`. No relays are in the field yet, so this is a merge and flash order constraint, not a live outage. It becomes an outage the day a relay is commissioned against a mixed pair.

#323 makes the old key worse if #324 is not flashed with it. Once numbers are no longer positions, the current server/client pair clears a different monitor more often. Ship the server command and the client reader together.

Also still true, and not introduced here: `resetRelayForMonitor` sends a remote OFF only when the local active mask is non-zero. `relayClearSensorNumber` still calls `release` when the mask is 0. The log then says `none-active`. A remote coil that is actually ON, but not in that mask, stays ON. C-A02 is the right place to fix the mask. The log should not read as success for a remote binding with an empty mask.

## CR-2 — High: #323 does not merge with #325 or #326

`git merge-tree` of #323 with #325, and of #323 with #326, reports `changed in both` for `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino`.

#322 merges cleanly with #323, #324, and #325. #324 merges cleanly with #325 and #326. #326 is already based on #325, so that pair is a fast-forward of the server work.

The conflict is the usual one for this sketch: both sides edit the same large page strings and nearby C++. A textual merge can drop one side's include, or split a `R"HTML(` page. Rebase #323 onto #326 (after #326 is merged, or onto that branch) by re-applying the generator and `handleConfigPost` edits as anchored replacements. Do not resolve the `.ino` by taking one side of the conflict.

Suggested merge order: #322 anytime, #325 then #326, rebase #323, then #324 only with the server Clear Relay change.

## CR-3 — Medium: a blank name puts the internal number into SMS and email

**Source:** Config Generator `collectConfig` on #323; `formatSensorName` / `composeSensorText` on #326.

#323 fills an empty name as:

- `Tank ${userNum || cardSensorNumber(card) || index+1}`
- `Gas System ...` and `Engine ...` use the same expression

`userNum` is the Display Number. `cardSensorNumber` is the stable internal number. #326 then prints site + label, and adds ` #N` again when a Display Number is set.

Two results that violate the 2026-09-24 naming rule ("the internal sensor number is never printed"):

1. Display Number blank, name blank, sensor number 4. The stored name becomes `Tank 4`. The SMS is `Silas Cox Tank 4 high alarm ...`. The internal number is in the label.
2. Display Number 7, name blank. The stored name becomes `Tank 7`, and the SMS appends ` #7`, so the operator sees `Tank 7 #7`.

Leave the auto-name as a type word with no number (`Tank`, `Engine`, `Gas System`). The Display Number belongs only in the ` #N` suffix. Do not write either number into `name`.

## CR-4 — Medium: some bad numbers are repaired onto a retired identity, with no toast

**Source:** `loadSensorNumbers` in the Config Generator on #323. The host test locks the behavior in: `missing number is the position`, `300 is the position, like the client`.

A 0 or a duplicate is renumbered past `snh` and the load toast names it. A missing number, a non-integer, or a number outside 0–255 is different. The helper substitutes `position + 1` when that value is free, does not add a `repaired` entry, and does not look at `snh`.

Example: stored `snh` is 5, the only sensor has `"number": 300`. The card becomes #1, `high` stays 5, and there is no toast. Sending that config reuses sensor 1. History, learned calibration, and alarm-contact id `uid_1` belong to that number. The server check in `sensorNumbersCheck` does not save this, because the page has already rewritten 300 into 1 before POST.

The comment in `TankAlarm_SensorNumbers.h` is why this is not the same as the client's flash load. A live `applyConfigUpdate` does not reset the monitor first, so a missing number on the device keeps the previous slot's number. The page's position fallback is not what the device will do on a live update, and it is not what the identity rule asked for.

Treat missing, non-integer, and out-of-range the same as 0: allocate past `snh`, and toast every repair, including the `reused` case at 255. The 255 fallback (lowest free number, `reused: true`) is tested and toasted. That toast is the only thing standing between the operator and a reused identity. It should say not to send until the contacts and history have been checked, which the current toast does. Keep that, and do not silently take the other path.

## CR-5 — Medium: `snh` dies if the config is round-tripped through the client

**Source:** generator `cfg.snh = Math.max(sensorNumberHigh, ...sensorNums)` on #323; client `saveConfigToFlash` on `master`, which #323 does not change.

`snh` is what stops the newest removed number from being reused. `nextSensorNumber` is max(numbers still on the page, `snh`) + 1. Without `snh`, removing #4 from {1, 3, 4} and adding a sensor yields #4 again.

The server snapshot keeps `snh`, because `dispatchClientConfig` caches the posted JSON. Load from Cloud is fine. The client does not. `saveConfigToFlash` writes a fixed field list and never writes `snh`. The client ACK is not a copy of the config, so the cloud copy is not overwritten today. An import of a flash dump, or any later path that replaces the snapshot with the device's copy, drops the high-water mark.

`snh` has to live in the client schema, or the server must keep it beside the snapshot and put it back onto any config read from the device. A comment in the generator is not enough. The number is the identity; the high-water mark is part of that identity.

The POST check itself is in the right place: `handleConfigPost` calls `sensorNumbersCheck` on the raw `sensors` value before `dispatchClientConfig`, so a bad POST is not cached. Calibration re-dispatch trusts the cache. That is fine for configs that already passed the check.

`upsertSensorRecord` rejecting `k == 0` instead of `k >= 64` is required once numbers pass 63. The hash is `(uid, k)`, not `records[k]`. This is not an overflow. See CR-6.

## CR-6 — Low: garbage `k` values 64–255 can now evict a live sensor

**Source:** `upsertSensorRecord` on #323.

`MAX_SENSOR_RECORDS` is 64. Before this PR, `k >= 64` was rejected and did not take a slot. After it, any `k` in 1–255 is accepted, and a full table evicts the stalest record. A garbled note can no longer be ignored just because its `k` is large. One bad value takes one slot, as the comment says, but there are now 192 values that used to be refused.

This is acceptable for a small fleet if every `k` comes from a real config. It is not acceptable as the only filter. Prefer: accept `k` only when it is in that client's cached `sensors[].number` (and allow a short grace for a note that arrives before the config). Unknown `k` should be logged and dropped, not inserted.

## CR-7 — Low: #326 still texts an unload whose `k` is missing

**Source:** `handleUnload` on `00a9e01`.

Telemetry already drops a note whose `k` is missing or less than 1. The unload path, which this PR edits, still does `uint8_t sensorIndex = doc["k"].as<uint8_t>()`. A missing key, a bool, or a fraction becomes 0. The function then looks up sensor 0, falls back to the label `Tank`, and can send the SMS and email. #323's new `k == 0` reject in `upsertSensorRecord` does not stop that send; the text is built before the registry update matters.

Use the same guard as telemetry: no `k` in 1–255, no SMS, no email, no registry write.

The label fix beside it is right: an unload note does not carry `n`, and the old `doc["n"] | "Tank"` overwrote a real label. Keeping the record's label unless the note has one is the correct direction. The remaining hole is only the missing `k`, and the empty-label case that still stores the placeholder `Tank` when the record has no name yet. Telemetry that carries `n` will replace that placeholder. Telemetry that does not will leave `Tank` in place, which is the old default.

## CR-8 — Low: SMS and the daily email do not name the sensor the same way

**Source:** `TankAlarm_SensorName.h` on #326; the Apps Script in `/email-setup`; the SendGrid JSONata in `NOTEHUB_ROUTES_SETUP.md`.

The owner rule is site + label, trailing spaces removed, blank parts omitted, `Sensor` if both are blank, ` #N` only for Display Number 1–255.

The C++ path does more than that, and the email path does less:

- `SENSOR_NAME_PART_MAX` is 23 bytes. A site longer than that is cut in SMS, reminders, snooze, and unload text. The Apps Script prints the full site and label. The same sensor will not match across the two channels.
- C++ removes trailing spaces only, and only after the UTF-8 cut. The script uses `String.trim()`, so a leading space survives in SMS and disappears in the daily email.
- `composeSensorText` protects the tail, which is the right priority. A snooze tail with a long operator name can still drop `reply UNSNOOZE` off the end. Cap `who` before it is copied into the tail. A normal phone number fits.
- The script's `s.userNumber > 0` and the float `> 0.5` ON/OFF test match the server. Alarm and reminder emails use `b.message`, so they pick up the C++ name, not the script's name. Only the daily report uses the script. That split is why the two formatters have to match.
- The JSONata template uses `$trim` and `$name :=`. The live route is the Apps Script bridge, not SendGrid. Paste that expression into Notehub's tester before anyone saves it. Do not assume the documented template is valid on Notehub's JSONata.

`noteDisplayNumber` is the right rule for telemetry and daily entries (a missing `un` means the box was cleared). The call site is weaker than #324's `relayJsonUint`: it uses `!variant.isNull()` and `is<int32_t>()`. A JSON integer from the current client works. A `7.0` that survived a double round-trip is treated as invalid and the old number is kept, which is safe but will not track a cleared or changed number. A client build that never sends `un` will clear a stored Display Number on the next telemetry. Current clients send `un` when it is set. Do not deploy #326 against a client that omits `un` even when the box is set.

Alarm notes are not used to update `un`. A sensor first seen on an alarm can be texted without ` #N` until telemetry or the daily report. The site fill in `handleAlarm` (`rec->site` empty, copy `s`) is correct and closes the "Sensor" name for that case. The label can still be empty on that first note.

## CR-9 — Low: a rate-limited Clear Relay is discarded

**Source:** the `relay.qi` reader sets `consume = true` after `processRelayCommand` returns, including the cooldown return. #324 did not change that reader.

The new `#error` on `GRID_INBOUND_INTERVAL_MS` is real: `ClientConfig.h` is included just above it, and `RELAY_COMMAND_COOLDOWN_MS` (5000) is visible from `TankAlarm_Config.h`. A bench poll faster than 10 s will not compile. That does not help a field unit. Any relay note within 5 s of the previous one is deleted. A Clear Relay behind a `relay/state` note is lost, and the only record is the serial line. The server has already returned 200.

When the server half is written, one clear should be one note, and the client should not delete a note it refused for cooldown. Leave it queued, or have the server retry. Do not treat 200 as delivery.

## CR-10 — Notes, not defects

**#325 is the right shape.** `TankAlarm_DigitalDisplay.h` is small and host-tested. The field sensor-fault, sensor-stuck, and sensor-recovered notes send `rd`, not `lvl` / `ma` / `fl`, so `alarmNoteCarriesValue` keeps the last reading for the notes that were blanking the dashboard to 0. A config-push clear still goes through `buildSensorObject` and can carry `lvl`; that is not the blank-to-zero bug. The stuck clear runs after telemetry copies `st` onto the record, so a float note clears a stale sensor-stuck alarm even with no cached config. A lone low latch is not relabeled as a float. The viewer card hides the 24 h change and prints ON/OFF. It still titles the card from the object type (`Tank`), while the server dashboard says `Float Switch`. Worth aligning, not a functional miss.

`dailyReconcileAlarmType` trusts `y` of `triggered` / `not_triggered` before it checks that the sensor is digital. No current client sends `y` on the daily alarm part. Gate it on a digital type before CL-5, so a stray `y` cannot relabel an analog latch.

**#322 should land as-is.** The header does not touch hardware. Coil pins are `0..3`, LED pins are `7, 9, 8, 153`, and the v2.2.16 `LED_D0 + r` helper is named legacy and is not for firmware. `static_assert`s run only under `ARDUINO_OPTA`. CI compiles the bench sketch and the job still exits 1 on failure (`continue-on-error` is only so the issue step runs). `TankAlarm-112025-Common.zip` on this branch contains `TankAlarm_OptaIo.h`. The input-plan rules match the comments: exclusive claims conflict, analog may share, a class change sticks as RESTART, and a pull-only change produces a pin op. `out` may alias `prev`; `usedClass` is copied before the clear.

Do not copy `OPTA_LED_ON_LEVEL = 1` into firmware until bench A2 measures it. The bench `x` command still calls `pinMode` / `digitalWrite` on pin 10 for relay 4. "Changes nothing" is the expected meter result, not proof that pin 10 is unused. CL-2 must keep `usedClass` for the whole boot and must not call `optaLegacyRelayPin`.

**#324's parser is the one to reuse.** `relayJsonUint` rejects bool, null, fractions, and non-integral doubles, and accepts `3.0`. The sensor-number and Display Number call sites should use that, or the same rule, instead of `is<uint8_t>()` / `is<int32_t>()` alone.

## What not to rework

- The consecutive-conflict and RESTART rules in `optaBuildIoPlan`. They match the core's analog/digital driver split.
- Rejecting `k == 0` in the registry. Phantom sensor 0 was the old bug. Widening the legal range is required; the missing piece is "unknown `k` does not take a slot" (CR-6), not a return to `k < 64`.
- Ignoring `relay_reset_sensor` on the new client. That is correct. The missing piece is the server key, shipped in the same release (CR-1).
- Keeping the last displayed value when an alarm note has no reading. The field fault notes do not carry `lvl` or `ma`.
- Printing ON/OFF at `value > 0.5`. That matches the dashboard `formatSwitch`.
- Not printing `sensorIndex` from the new SMS helpers. The leak is the auto-name (CR-3), not `composeSensorText`.

## Suggested order

1. Rebase #323 onto #326 and fix CR-3 and CR-4 in that rebase. Add `snh` to the client schema, or document and enforce that the server snapshot is the only copy that counts (CR-5).
2. Land #325 and #326 together. Update the Apps Script in the field before expecting the daily email to change. The server SMS texts change with the server firmware; the daily email does not, until the script is redeployed as a new version of the same Web App.
3. Land #322 whenever. It changes no client, server, or viewer behavior.
4. Do not flash #324 until the server sends `relay_reset_sensor_number` and the dashboard posts `k`, not the card index. Until then, leave the field client on the v2.2.16 reader if a bench unit must clear relays.
