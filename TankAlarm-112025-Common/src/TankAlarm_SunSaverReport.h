/**
 * TankAlarm_SunSaverReport.h
 *
 * The charger check (TankAlarm_SunSaverSettings.h) as JSON: the compact report a client sends to
 * the server, which the server also keeps, and the readable form the server's pages show. And
 * the same for programming the charger (TankAlarm_SunSaverProgram.h): the values of a request
 * as the config carries them, the result of a run, and the client's journal.
 *
 * This header needs ArduinoJson (v7), so the sketches include it themselves. TankAlarm_Common.h
 * does not, and neither does TankAlarm_Solar.cpp. Like the other two headers it has no Arduino
 * dependency, and tests/host/sunsaver_settings and tests/host/sunsaver_program run it on a PC.
 *
 * The compact report
 * ------------------
 *   lay   the layout of "r": sunsaver::REPORT_LAYOUT
 *   r     every register of the check, four hex digits each, in sunsaver::ItemIndex order
 *   nr    indexes of the registers that gave no value: the controller refused them or did not
 *         answer (left out when there are none)
 *   ch    Chemistry the settings were judged against
 *   vd    Verdict
 *   fd    Finding bits
 *   ss    ScaleId of the stored settings
 *   sf    [setpoints round with x100, setpoints round with x96.667, setpoints used]
 *   ls    ScaleId of the live readings
 *   lsrc  LiveSource: what proved it
 *   sv    12 or 24 as measured, 0 when not known
 *
 * Items are only ever added at the end of ItemIndex, so a report from a newer client decodes on
 * an older server (the extra registers are ignored) and the other way round (the registers the
 * older client did not read count as not read).
 *
 * Copyright (c) 2025-2026 Senax Inc. All rights reserved.
 */

#ifndef TANKALARM_SUNSAVER_REPORT_H
#define TANKALARM_SUNSAVER_REPORT_H

#include <ArduinoJson.h>

#include "TankAlarm_SunSaverProgram.h"
#include "TankAlarm_SunSaverSettings.h"

namespace sunsaver {

static const size_t REPORT_HEX_CHARS = (size_t)ITEM_COUNT * 4u;

// Writes the compact report into `out`. Members that `out` already holds are left alone.
inline void encodeReport(const Snapshot &snapshot, const Evaluation &evaluation, JsonObject out) {
  // A char array (not a pointer to const) makes every version of ArduinoJson 7 copy the text.
  char hex[REPORT_HEX_CHARS + 1];
  Text text(hex, sizeof(hex));
  for (uint8_t i = 0; i < ITEM_COUNT; ++i) {
    text.addHex16(have(snapshot, i) ? snapshot.raw[i] : (uint16_t)0);
  }
  out["lay"] = REPORT_LAYOUT;
  out["r"] = hex;
  bool anyMissing = false;
  JsonArray missing;
  for (uint8_t i = 0; i < ITEM_COUNT; ++i) {
    if (have(snapshot, i)) {
      continue;
    }
    if (!anyMissing) {
      missing = out["nr"].to<JsonArray>();
      anyMissing = true;
    }
    missing.add(i);
  }
  out["ch"] = evaluation.chemistry;
  out["vd"] = evaluation.verdict;
  out["fd"] = evaluation.findings;
  out["ss"] = evaluation.settingsScale.scale;
  JsonArray fits = out["sf"].to<JsonArray>();
  fits.add(evaluation.settingsScale.fits[0]);
  fits.add(evaluation.settingsScale.fits[1]);
  fits.add(evaluation.settingsScale.used);
  out["ls"] = evaluation.liveScale;
  out["lsrc"] = evaluation.liveSource;
  out["sv"] = evaluation.systemVolts;
}

// The value of a hex digit, or -1.
inline int8_t hexDigitValue(char c) {
  if (c >= '0' && c <= '9') {
    return (int8_t)(c - '0');
  }
  if (c >= 'A' && c <= 'F') {
    return (int8_t)(c - 'A' + 10);
  }
  if (c >= 'a' && c <= 'f') {
    return (int8_t)(c - 'a' + 10);
  }
  return -1;
}

// Four hex digits as a word. False when one of them is no hex digit, which the end of a text
// that is too short is not either.
inline bool hexWord(const char *digits, uint16_t *word) {
  uint16_t value = 0;
  for (uint8_t digit = 0; digit < 4; ++digit) {
    const int8_t nibble = hexDigitValue(digits[digit]);
    if (nibble < 0) {
      return false;
    }
    value = (uint16_t)((value << 4) | (uint16_t)nibble);
  }
  *word = value;
  return true;
}

// A value as a small number. Anything that is not a whole number from 0 to `highest` reads as
// `fallback`: a report is input from the network.
inline uint8_t variantByte(JsonVariantConst value, uint8_t highest, uint8_t fallback) {
  if (!value.is<uint32_t>()) {
    return fallback;
  }
  const uint32_t number = value.as<uint32_t>();
  return number <= highest ? (uint8_t)number : fallback;
}

// A value as a word: 0 to 65535, and `fallback` for anything else.
inline uint16_t variantWord(JsonVariantConst value, uint16_t fallback) {
  if (!value.is<uint32_t>()) {
    return fallback;
  }
  const uint32_t number = value.as<uint32_t>();
  return number <= 0xFFFFu ? (uint16_t)number : fallback;
}

// The same for a member of an object.
inline uint8_t reportByte(JsonObjectConst in, const char *key, uint8_t highest, uint8_t fallback) {
  return variantByte(in[key], highest, fallback);
}

// Reads a compact report. Returns false, with *snapshot empty and *evaluation zero, when `in` is
// not a report this code can read. What a report says about a scale, a verdict or a battery this
// code does not know reads as "not proven" or "not judged".
inline bool decodeReport(JsonObjectConst in, Snapshot *snapshot, Evaluation *evaluation) {
  clearSnapshot(snapshot);
  memset(evaluation, 0, sizeof(*evaluation));
  if (in.isNull() || !in["lay"].is<uint32_t>() || in["lay"].as<uint32_t>() != REPORT_LAYOUT) {
    return false;
  }
  const char *hex = in["r"].as<const char *>();
  if (hex == nullptr) {
    return false;
  }
  const size_t length = strlen(hex);
  if (length == 0 || length % 4u != 0) {
    return false;
  }
  const size_t sent = length / 4u;
  const uint8_t items = sent < ITEM_COUNT ? (uint8_t)sent : (uint8_t)ITEM_COUNT;
  for (uint8_t i = 0; i < items; ++i) {
    uint16_t raw = 0;
    if (!hexWord(hex + (size_t)i * 4u, &raw)) {
      clearSnapshot(snapshot);
      return false;
    }
    snapshot->raw[i] = raw;
    snapshot->status[i] = SSIO_OK;
  }
  JsonArrayConst missing = in["nr"].as<JsonArrayConst>();
  for (JsonVariantConst entry : missing) {
    if (entry.is<uint32_t>() && entry.as<uint32_t>() < items) {
      const uint8_t index = (uint8_t)entry.as<uint32_t>();
      snapshot->raw[index] = 0;
      snapshot->status[index] = SSIO_REFUSED;
    }
  }

  evaluation->chemistry = reportByte(in, "ch", CHEMISTRY_LI_ION, CHEMISTRY_NOT_JUDGED);
  evaluation->verdict = reportByte(in, "vd", VERDICT_CANNOT_TELL, VERDICT_NOT_JUDGED);
  evaluation->findings = in["fd"].is<uint32_t>() ? in["fd"].as<uint32_t>() : 0;
  evaluation->settingsScale.scale = reportByte(in, "ss", SCALE_96667, SCALE_UNKNOWN);
  JsonArrayConst fits = in["sf"].as<JsonArrayConst>();
  if (fits.size() == 3) {
    const uint8_t values[3] = {
        (uint8_t)(fits[0].is<uint32_t>() && fits[0].as<uint32_t>() <= ITEM_COUNT ? fits[0].as<uint32_t>() : 0),
        (uint8_t)(fits[1].is<uint32_t>() && fits[1].as<uint32_t>() <= ITEM_COUNT ? fits[1].as<uint32_t>() : 0),
        (uint8_t)(fits[2].is<uint32_t>() && fits[2].as<uint32_t>() <= ITEM_COUNT ? fits[2].as<uint32_t>() : 0)};
    evaluation->settingsScale.fits[0] = values[0];
    evaluation->settingsScale.fits[1] = values[1];
    evaluation->settingsScale.used = values[2];
  }
  evaluation->liveScale = reportByte(in, "ls", SCALE_96667, SCALE_UNKNOWN);
  evaluation->liveSource = reportByte(in, "lsrc", LIVE_SOURCE_STORED_LVD, LIVE_SOURCE_NONE);
  if (evaluation->liveScale == SCALE_UNKNOWN || evaluation->liveSource == LIVE_SOURCE_NONE) {
    evaluation->liveScale = SCALE_UNKNOWN;
    evaluation->liveSource = LIVE_SOURCE_NONE;
  }
  const uint8_t system = reportByte(in, "sv", 24, 0);
  evaluation->systemVolts = (system == 12 || system == 24) ? system : (uint8_t)0;
  return true;
}

// ---------------------------------------------------------------------------------------------
// Which report the server keeps
// ---------------------------------------------------------------------------------------------
//
// A client's report can arrive late: the client keeps it while it cannot reach its Notecard,
// and the Notecard keeps it until it syncs. The server therefore orders the reports of a client
// by the time of the check, which the client sends as "epoch", and not by their arrival.

// 2020-09-13. A clock that reads less was never set.
static const double REPORT_EARLIEST_EPOCH = 1600000000.0;
// 2106-02-07, the last second a uint32_t holds: the server keeps and serves the time as one.
static const double REPORT_LATEST_EPOCH = 4294967295.0;
// A client's clock may run ahead of the server's by this much.
static const double REPORT_CLOCK_AHEAD_SECONDS = 86400.0;

// The time of a check as the server takes it: the client's own, or 0 for "not known" when the
// client sent none or one that is plainly wrong. `serverNow` is the server's time, or anything
// below REPORT_EARLIEST_EPOCH when the server does not know the time either.
//
// "Not known" is never replaced by the time of arrival: that would let an old report that was
// held back pass for the newest one.
//
// The server also runs the time of the report it keeps through this, against its clock now:
// one taken while its clock was not set, and more than a day ahead of it once it is, would
// otherwise rank above every report that comes after it.
inline double reportCheckTime(double clientEpoch, double serverNow) {
  // Also for a value that is not a number, or one past what the server keeps: an infinite or
  // huge time is plainly wrong whether or not the server knows the time.
  if (!(clientEpoch >= REPORT_EARLIEST_EPOCH && clientEpoch <= REPORT_LATEST_EPOCH)) {
    return 0.0;
  }
  if (serverNow >= REPORT_EARLIEST_EPOCH && clientEpoch > serverNow + REPORT_CLOCK_AHEAD_SECONDS) {
    return 0.0;
  }
  return clientEpoch;
}

// Whether a report that arrives takes the place of the one the server keeps. Both times are
// reportCheckTime()s. An older report does not replace a newer one, and one whose time is not
// known replaces only another one whose time is not known. The same report arriving twice
// replaces itself.
inline bool reportReplaces(bool haveKept, double keptCheckTime, double newCheckTime) {
  return !haveKept || newCheckTime >= keptCheckTime;
}

// The report for a person: what was found, and every register with its value as text.
//
//   vd, verdict     the Verdict and its name
//   battery         the battery the settings were judged against
//   system          12 or 24 as measured, 0 when not known
//   settingsScale   one line on how the stored voltages are encoded
//   liveScale       the same for the live readings
//   findings        [{"t": text, "p": true when it makes the settings wrong for this battery}]
//   groups          [{"title": ..., "rows": [[name, label, value, raw], ...]}]
//
// A register that an older client did not read is left out, and one the controller refused or
// did not answer shows "no answer".
inline void describeReport(const Snapshot &snapshot, const Evaluation &evaluation, JsonObject out) {
  char line[160];
  out["vd"] = evaluation.verdict;
  out["verdict"] = verdictName(evaluation.verdict);
  out["battery"] = chemistryName(evaluation.chemistry);
  out["system"] = evaluation.systemVolts;
  {
    Text text(line, sizeof(line));
    describeSettingsScale(evaluation.settingsScale, &text);
    out["settingsScale"] = line;
  }
  {
    Text text(line, sizeof(line));
    describeLiveScale(evaluation.liveScale, evaluation.liveSource, evaluation.systemVolts, &text);
    out["liveScale"] = line;
  }

  JsonArray findings = out["findings"].to<JsonArray>();
  const uint32_t problems = problemFindings(evaluation.chemistry);
  for (uint8_t bit = 0; bit < 32; ++bit) {
    const uint32_t finding = (uint32_t)1 << bit;
    if ((evaluation.findings & finding) == 0) {
      continue;
    }
    JsonObject entry = findings.add<JsonObject>();
    entry["t"] = findingText(finding);
    entry["p"] = (problems & finding) != 0;
  }

  JsonArray groups = out["groups"].to<JsonArray>();
  for (uint8_t group = 0; group < GROUP_COUNT; ++group) {
    JsonArray rows;
    bool opened = false;
    for (uint8_t i = 0; i < ITEM_COUNT; ++i) {
      const Item &entry = item(i);
      if (entry.group != group || snapshot.status[i] == SSIO_NOT_READ) {
        continue;
      }
      if (!opened) {
        JsonObject section = groups.add<JsonObject>();
        section["title"] = groupTitle(group);
        rows = section["rows"].to<JsonArray>();
        opened = true;
      }
      JsonArray row = rows.add<JsonArray>();
      row.add(entry.name);
      row.add(entry.label);
      if (have(snapshot, i)) {
        Text value(line, sizeof(line));
        describeValue(i, snapshot.raw[i], evaluation.settingsScale.scale, evaluation.liveScale, &value);
        row.add(line);
        Text raw(line, sizeof(line));
        raw.add("0x").addHex16(snapshot.raw[i]);
        row.add(line);
      } else {
        row.add("no answer");
        row.add("");
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Programming: the request in a client's config
// ---------------------------------------------------------------------------------------------
//
// "solarCharger.program" in a config:
//   absorbV, floatV, hvdV, hvrV, lvdV, lvrV   volts, with two decimals at most
//   absorbMin                                 whole minutes
//   loadOk                                    true: current may flow from the LOAD terminals during the run
//   req                                       the id of the request: the server's time when a person asked for
//                                             the values to be written. Without it they are only kept.
// The server sets "req", and takes out one that came from a browser.

// One voltage of a request, in hundredths of a volt. A number that could be no voltage becomes
// one that every limit refuses.
inline uint16_t readCentivolts(JsonVariantConst value, uint16_t *centivolts) {
  *centivolts = 0;
  if (!value.is<double>()) {
    return PROFILE_MISSING;
  }
  const double scaled = value.as<double>() * 100.0;
  if (!(scaled > 0.0)) {
    return 0;  // 0 V, less, or not a number: outside every range
  }
  if (scaled >= 65535.5) {
    *centivolts = 0xFFFF;  // more than a word holds
    return 0;
  }
  const uint16_t rounded = (uint16_t)(scaled + 0.5);
  *centivolts = rounded;
  // More than two decimals. ArduinoJson keeps a number that fits a float as a float, and 655.3
  // comes back from it 0.0012 hundredths off: the margin is wider than that and far narrower
  // than a digit a person could type.
  const double off = scaled - (double)rounded;
  return (off > 0.01 || off < -0.01) ? (uint16_t)PROFILE_GRID : (uint16_t)0;
}

// Reads the seven values. Returns ProfileError bits: PROFILE_MISSING when one of them is not
// there or is no number, and otherwise what validateProfile() finds. 0: the profile may be
// written.
inline uint16_t readProfile(JsonObjectConst in, Profile *profile) {
  memset(profile, 0, sizeof(*profile));
  uint16_t errors = 0;
  errors |= readCentivolts(in["absorbV"], &profile->absorbCv);
  errors |= readCentivolts(in["floatV"], &profile->floatCv);
  errors |= readCentivolts(in["hvdV"], &profile->hvdCv);
  errors |= readCentivolts(in["hvrV"], &profile->hvrCv);
  errors |= readCentivolts(in["lvdV"], &profile->lvdCv);
  errors |= readCentivolts(in["lvrV"], &profile->lvrCv);
  JsonVariantConst minutes = in["absorbMin"];
  if (!minutes.is<double>()) {
    errors |= PROFILE_MISSING;
  } else {
    // Anything but a whole number of minutes becomes a time the limits refuse.
    const double value = minutes.as<double>();
    const uint32_t whole = (value >= 0.0 && value <= 1000.0) ? (uint32_t)value : 0;
    profile->absorbSeconds = (double)whole == value ? (uint16_t)(whole * 60u) : (uint16_t)0;
  }
  if ((errors & PROFILE_MISSING) == 0) {
    errors |= validateProfile(*profile);
  }
  return errors;
}

inline bool readLoadAllowed(JsonObjectConst in) { return in["loadOk"].is<bool>() && in["loadOk"].as<bool>(); }

// The id of the request, or 0 when the config asks for nothing to be written.
inline uint32_t readRequestId(JsonObjectConst in) { return in["req"].is<uint32_t>() ? in["req"].as<uint32_t>() : 0; }

// When a request was asked for: the server's time then ("at"), apart from its id, which goes on
// from the last one when the server's clock went back (see requestIsFresh()). 0 for a request
// without it, whose id is that time.
inline uint32_t readRequestAt(JsonObjectConst in) { return in["at"].is<uint32_t>() ? in["at"].as<uint32_t>() : 0; }

// ---------------------------------------------------------------------------------------------
// Programming: the result of a run
// ---------------------------------------------------------------------------------------------
//
// The compact result
//   play  the layout of this result: PROGRAM_LAYOUT
//   o     Outcome
//   rs    RestartStatus
//   fu    true: the client owes the controller another look, and reports once more (left out otherwise)
//   w     true: a write request was sent (left out otherwise)
//   sc    [ScaleId the voltages were written with, ScaleId of the battery reading]
//   rf    Refusal bits
//   pe    ProfileError bits
//   nt    Note bits
//   pb    requests the controller left unanswered after the last restart command
//   p     what was asked for: [absorption, float, absorption seconds, HVD, HVR, LVD, LVR], the
//         voltages in hundredths of a volt
//   sn    Seen bits: which of the words in "m" were read
//   m     five words, four hex digits each: battery voltage, load current, DIP switches, array
//         faults, load faults
//   ld    the load state before the run, while the controller waited for its restart, and at
//         the end. 255: not read
//   en    the plan in the order of writing, 16 hex digits per register: ItemIndex and
//         EntryStatus (two digits each), the word before, the word wanted, the word found.
//         Left out when the run ended before it made a plan.

static const uint8_t PROGRAM_LAYOUT = 1;
static const size_t PROGRAM_ENTRY_HEX_CHARS = 16;
static const size_t PROGRAM_READING_WORDS = 5;

// Writes the compact result into `out`. Members that `out` already holds are left alone.
inline void encodeProgramResult(const ProgramResult &result, JsonObject out) {
  out["play"] = PROGRAM_LAYOUT;
  out["o"] = result.outcome;
  out["rs"] = result.restart;
  if (result.followUp) {
    out["fu"] = true;
  }
  if (result.wroteAny) {
    out["w"] = true;
  }
  JsonArray scales = out["sc"].to<JsonArray>();
  scales.add(result.scale);
  scales.add(result.liveScale);
  out["rf"] = result.refusals;
  out["pe"] = result.profileErrors;
  out["nt"] = result.notes;
  out["pb"] = result.probes;
  JsonArray asked = out["p"].to<JsonArray>();
  for (uint8_t field = 0; field < PROFILE_FIELDS; ++field) {
    asked.add(profileField(result.profile, field));
  }
  out["sn"] = result.seen;
  {
    // A char array (not a pointer to const) makes every version of ArduinoJson 7 copy the text.
    char hex[PROGRAM_READING_WORDS * 4u + 1];
    Text text(hex, sizeof(hex));
    text.addHex16(result.batteryRaw).addHex16(result.loadCurrentRaw).addHex16(result.dip);
    text.addHex16(result.arrayFault).addHex16(result.loadFault);
    out["m"] = hex;
  }
  JsonArray load = out["ld"].to<JsonArray>();
  load.add(result.loadStateBefore);
  load.add(result.loadStateEdit);
  load.add(result.loadStateAfter);
  if (result.planned != 0) {
    char hex[(size_t)PLAN_ENTRIES * PROGRAM_ENTRY_HEX_CHARS + 1];
    Text text(hex, sizeof(hex));
    for (uint8_t i = 0; i < PLAN_ENTRIES; ++i) {
      const PlanEntry &entry = result.entries[i];
      text.addHex16((uint16_t)(((uint16_t)entry.item << 8) | entry.status));
      text.addHex16(entry.before).addHex16(entry.target).addHex16(entry.found);
    }
    out["en"] = hex;
  }
}

// Reads a compact result. Returns false, with *result cleared, when `in` is not a result this
// code can read: another layout, or an outcome, a restart status or a register it has no name
// for. It shows nothing rather than something it would have to guess at.
inline bool decodeProgramResult(JsonObjectConst in, ProgramResult *result) {
  clearResult(result);
  if (in.isNull() || !in["play"].is<uint32_t>() || in["play"].as<uint32_t>() != PROGRAM_LAYOUT) {
    return false;
  }
  ProgramResult read;
  clearResult(&read);
  read.outcome = reportByte(in, "o", OUTCOME_LAST, OUTCOME_NONE);
  read.restart = reportByte(in, "rs", RESTART_STATUS_LAST, 0xFF);
  if (read.outcome == OUTCOME_NONE || read.restart == 0xFF) {
    return false;
  }
  read.followUp = in["fu"].is<bool>() && in["fu"].as<bool>();
  read.wroteAny = in["w"].is<bool>() && in["w"].as<bool>();
  JsonArrayConst scales = in["sc"].as<JsonArrayConst>();
  if (scales.size() == 2) {
    read.scale = variantByte(scales[0], SCALE_96667, SCALE_UNKNOWN);
    read.liveScale = variantByte(scales[1], SCALE_96667, SCALE_UNKNOWN);
  }
  read.refusals = in["rf"].is<uint32_t>() ? in["rf"].as<uint32_t>() : 0;
  read.profileErrors = variantWord(in["pe"], 0);
  read.notes = variantWord(in["nt"], 0);
  read.probes = reportByte(in, "pb", 0xFF, 0);

  JsonArrayConst asked = in["p"].as<JsonArrayConst>();
  if (asked.size() != PROFILE_FIELDS) {
    return false;
  }
  uint16_t fields[PROFILE_FIELDS];
  for (uint8_t field = 0; field < PROFILE_FIELDS; ++field) {
    if (!asked[field].is<uint32_t>() || asked[field].as<uint32_t>() > 0xFFFFu) {
      return false;
    }
    fields[field] = variantWord(asked[field], 0);
  }
  const Profile profile = {fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], fields[6]};
  read.profile = profile;

  const char *readings = in["m"].as<const char *>();
  if (readings != nullptr) {
    uint16_t words[PROGRAM_READING_WORDS];
    if (strlen(readings) != PROGRAM_READING_WORDS * 4u) {
      return false;
    }
    for (uint8_t i = 0; i < PROGRAM_READING_WORDS; ++i) {
      if (!hexWord(readings + (size_t)i * 4u, &words[i])) {
        return false;
      }
    }
    read.seen = (uint8_t)((in["sn"].is<uint32_t>() ? in["sn"].as<uint32_t>() : 0) &
                          (SEEN_BATTERY | SEEN_LOAD_CURRENT | SEEN_DIP | SEEN_FAULTS));
    read.batteryRaw = words[0];
    read.loadCurrentRaw = words[1];
    read.dip = words[2];
    read.arrayFault = words[3];
    read.loadFault = words[4];
  }
  JsonArrayConst load = in["ld"].as<JsonArrayConst>();
  if (load.size() == 3) {
    read.loadStateBefore = variantByte(load[0], 0xFF, LOAD_STATE_NOT_READ);
    read.loadStateEdit = variantByte(load[1], 0xFF, LOAD_STATE_NOT_READ);
    read.loadStateAfter = variantByte(load[2], 0xFF, LOAD_STATE_NOT_READ);
  }

  const char *entries = in["en"].as<const char *>();
  if (entries != nullptr) {
    if (strlen(entries) != (size_t)PLAN_ENTRIES * PROGRAM_ENTRY_HEX_CHARS) {
      return false;
    }
    for (uint8_t i = 0; i < PLAN_ENTRIES; ++i) {
      const char *at = entries + (size_t)i * PROGRAM_ENTRY_HEX_CHARS;
      uint16_t head = 0;
      PlanEntry &entry = read.entries[i];
      if (!hexWord(at, &head) || !hexWord(at + 4, &entry.before) || !hexWord(at + 8, &entry.target) ||
          !hexWord(at + 12, &entry.found)) {
        return false;
      }
      entry.item = (uint8_t)(head >> 8);
      entry.status = (uint8_t)(head & 0xFFu);
      if (!inPlan(entry.item) || entry.status > ENTRY_STATUS_LAST) {
        return false;
      }
    }
    read.planned = PLAN_ENTRIES;
  }
  *result = read;
  return true;
}

// ---------------------------------------------------------------------------------------------
// Programming: the client's journal
// ---------------------------------------------------------------------------------------------
//
// The journal as the client keeps it in flash (sunsaver::Journal):
//   req, ph, ctl   the last run that reached a commit point, its JournalPhase, and the Modbus id
//                  of its controller (0: not known)
//   fc             the Modbus function that run read its controller with (3 or 4; 0: not known)
//   ans            the newest request the client has answered or taken on
//   wait           the request that waits (left out when none does)
//   rep            the result that has yet to go to the server (left out when there is none): the
//                  compact result of encodeProgramResult(), with "rq", the request it answers,
//                  "ep", when it came about (0: not known), and "out", the time it went out with
//                  (left out while it has not gone out)
//   look           the look for no request that has yet to go to the server (left out when there
//                  is none): the same, without "rq"
//   srq, sep       the request of the last result that went out, or goes out now, and the time
//                  it went out with (left out while none has)
//   lep            the time the last look went out with (left out while none has)
//   kt             the check value of a journal kept aside that this one took in (left out for
//                  none: see the client's takeKeptChargerJournal())
//   hf             the time from which its record of the requests answered holds them all, or 1
//                  while that time is not known (left out when it holds them all: see
//                  cautiousJournal())
// It is written whole, as a new file that takes the place of the old one, and is no longer than
// JOURNAL_JSON_MAX.

static const size_t JOURNAL_JSON_MAX = 1240;

inline void encodeJournal(const Journal &journal, JsonObject out) {
  out["req"] = journal.requestId;
  out["ph"] = journal.phase;
  out["ctl"] = journal.controller;
  out["fc"] = journal.readFunction;
  out["ans"] = journal.answeredId;
  if (journal.waitingId != 0) {
    out["wait"] = journal.waitingId;
  }
  if (journal.reportDue) {
    JsonObject report = out["rep"].to<JsonObject>();
    report["rq"] = journal.reportId;
    report["ep"] = journal.reportEpoch;
    if (journal.reportOut != 0) {
      report["out"] = journal.reportOut;
    }
    encodeProgramResult(journal.report, report);
  }
  if (journal.lookDue) {
    JsonObject look = out["look"].to<JsonObject>();
    look["ep"] = journal.lookEpoch;
    if (journal.lookOut != 0) {
      look["out"] = journal.lookOut;
    }
    encodeProgramResult(journal.look, look);
  }
  if (journal.sentEpoch != 0) {
    out["srq"] = journal.sentId;
    out["sep"] = journal.sentEpoch;
  }
  if (journal.lookSentEpoch != 0) {
    out["lep"] = journal.lookSentEpoch;
  }
  if (journal.keptTaken != 0) {
    out["kt"] = journal.keptTaken;
  }
  if (journal.historyFrom != 0) {
    out["hf"] = journal.historyFrom;
  }
}

// A file that is no journal reads as cautiousJournal(): the client may owe the controller a
// restart, and finds out by asking it. A result in it that this code cannot read is left out:
// the rest of the journal still holds. A record of the requests answered ("ans", "hf") that does
// not read is lost as the cautious journal's is: the start that reads it stands for the time.
inline Journal decodeJournal(JsonObjectConst in) {
  if (in.isNull() || !in["req"].is<uint32_t>() || !in["ph"].is<uint32_t>() ||
      in["ph"].as<uint32_t>() > JOURNAL_PHASE_LAST) {
    return cautiousJournal();
  }
  Journal journal = emptyJournal();
  journal.requestId = in["req"].as<uint32_t>();
  journal.phase = (uint8_t)in["ph"].as<uint32_t>();
  journal.controller = reportByte(in, "ctl", 247, 0);  // Modbus ids go from 1 to 247
  const uint8_t function = reportByte(in, "fc", 4, 0);
  journal.readFunction = function == 3 || function == 4 ? function : (uint8_t)0;
  journal.answeredId = in["ans"].is<uint32_t>() ? in["ans"].as<uint32_t>() : 0;
  if (!in["ans"].is<uint32_t>() || (!in["hf"].isNull() && !in["hf"].is<uint32_t>())) {
    journal.historyFrom = HISTORY_FROM_UNKNOWN;
  } else {
    journal.historyFrom = in["hf"].is<uint32_t>() ? in["hf"].as<uint32_t>() : 0;
  }
  journal.waitingId = in["wait"].is<uint32_t>() ? in["wait"].as<uint32_t>() : 0;
  JsonObjectConst report = in["rep"].as<JsonObjectConst>();
  if (!report.isNull() && report["rq"].is<uint32_t>() && decodeProgramResult(report, &journal.report)) {
    journal.reportDue = true;
    journal.reportId = report["rq"].as<uint32_t>();
    journal.reportEpoch = report["ep"].is<uint32_t>() ? report["ep"].as<uint32_t>() : 0;
    journal.reportOut = report["out"].is<uint32_t>() ? report["out"].as<uint32_t>() : 0;
  } else {
    clearResult(&journal.report);
  }
  JsonObjectConst look = in["look"].as<JsonObjectConst>();
  if (!look.isNull() && decodeProgramResult(look, &journal.look)) {
    journal.lookDue = true;
    journal.lookEpoch = look["ep"].is<uint32_t>() ? look["ep"].as<uint32_t>() : 0;
    journal.lookOut = look["out"].is<uint32_t>() ? look["out"].as<uint32_t>() : 0;
  } else {
    clearResult(&journal.look);
  }
  if (in["srq"].is<uint32_t>() && in["sep"].is<uint32_t>()) {
    journal.sentId = in["srq"].as<uint32_t>();
    journal.sentEpoch = in["sep"].as<uint32_t>();
  }
  journal.lookSentEpoch = in["lep"].is<uint32_t>() ? in["lep"].as<uint32_t>() : 0;
  journal.keptTaken = in["kt"].is<uint32_t>() ? in["kt"].as<uint32_t>() : 0;
  return journal;
}

// ---------------------------------------------------------------------------------------------
// Programming: the result the server keeps, and the result for a person
// ---------------------------------------------------------------------------------------------

// Whether a result of the same request as the one the server keeps, or a look when it keeps a
// look, came after it. A client sends each one with a later time ("epoch") than the last one it
// sent (Supervisor::sendEpoch()), whatever its clock did, and has that time in flash before the
// result goes out (Supervisor::reportSending()). One it sends again, after a restart that came
// before the Notecard's answer was in flash, goes out with the time it went out with: never
// later than a result that followed it, which may have gone out while flash took nothing, so the
// one sent again does not take that one's place. That time as it came, `keptSent` and `newSent`,
// orders the two while the one kept came with a time the server took: a later one may come with
// a time reportCheckTime() does not take, as when the server's clock went back by more than a
// day, and it came later all the same. When the one kept came with a time the server did not take,
// or either time is missing (a file written before the server kept it), the times the server took
// decide, `keptTime` and `newTime`, as they do for charger reports (reportReplaces()): a time that
// was never right then does not hold back the results that follow it.
inline bool resultFollows(double keptTime, uint32_t keptSent, double newTime, uint32_t newSent) {
  if (keptTime > 0.0 && keptSent != 0 && newSent != 0) {
    return newSent >= keptSent;
  }
  return newTime >= keptTime;
}

// Whether a result that arrives takes the place of the one the server keeps for a client. The
// result of a newer request does, and so does a later result of the same request
// (resultFollows()): a client reports twice when it had to look at the controller again. The
// times are reportCheckTime()s, and the times the client sent them with. The server keeps a
// result for request 0 apart, by resultFollows() alone: a look that a client made when it could
// not tell from its flash what was owed answers no request, and takes the place of none.
inline bool programResultReplaces(bool haveKept, uint32_t keptRequestId, double keptTime, uint32_t keptSent,
                                  uint32_t newRequestId, double newTime, uint32_t newSent) {
  if (!haveKept || newRequestId > keptRequestId) {
    return true;
  }
  return newRequestId == keptRequestId && resultFollows(keptTime, keptSent, newTime, newSent);
}

// The result for a person.
//
//   o, outcome   the Outcome and its name
//   good         true when the charger is left as the request wanted it
//   text         one sentence on how the run went
//   summary      "programmed: 15 of 17 registers written, 2 already set"
//   followUp     true when the client looks at the charger again and reports once more
//   restart      how the restart went (left out when no restart command was sent)
//   battery      the battery voltage when the run began (left out when it was not read)
//   load         what the LOAD output did during the run (left out when it was not read)
//   reasons      why nothing was written: the refusals with the figures behind them, and what is
//                wrong with the values
//   notes        what else a person should know
//   asked        [[label, value], ...]: the values of the request (left out for a result that
//                holds none, as after a client's restart)
//   registers    [[name, label, before, wanted, how it went], ...] in the order of writing
inline void describeProgramResult(const ProgramResult &result, JsonObject out) {
  char line[320];
  out["o"] = result.outcome;
  out["outcome"] = outcomeName(result.outcome);
  out["good"] = outcomeIsGood(result.outcome);
  out["text"] = outcomeText(result.outcome);
  {
    Text text(line, sizeof(line));
    describeRun(result, &text);
    out["summary"] = line;
  }
  if (result.followUp) {
    out["followUp"] = true;
  }
  if (result.restart != RESTART_NOT_NEEDED) {
    out["restart"] = restartText(result.restart);
  }
  if ((result.seen & SEEN_BATTERY) != 0) {
    Text text(line, sizeof(line));
    if (result.liveScale != SCALE_UNKNOWN) {
      text.addFixed(batteryVolts(result), 2).add(" V");
    } else {
      text.addFixed(voltsFromRaw(result.batteryRaw, SCALE_96667), 2).add(" V or ");
      text.addFixed(voltsFromRaw(result.batteryRaw, SCALE_100), 2);
      text.add(" V: how this charger encodes its live readings is not proven");
    }
    out["battery"] = line;
  }
  {
    Text text(line, sizeof(line));
    describeLoad(result, &text);
    if (line[0] != '\0') {
      out["load"] = line;
    }
  }

  JsonArray reasons = out["reasons"].to<JsonArray>();
  for (uint8_t bit = 0; bit < 32; ++bit) {
    const uint32_t refusal = (uint32_t)1 << bit;
    if ((result.refusals & refusal) == 0) {
      continue;
    }
    Text text(line, sizeof(line));
    describeRefusal(refusal, result, &text);
    reasons.add(line);
  }
  for (uint8_t bit = 0; bit < 16; ++bit) {
    const uint16_t error = (uint16_t)(1u << bit);
    if ((result.profileErrors & error) != 0) {
      reasons.add(profileErrorText(error));
    }
  }
  JsonArray notes = out["notes"].to<JsonArray>();
  for (uint8_t bit = 0; bit < 16; ++bit) {
    const uint16_t note = (uint16_t)(1u << bit);
    if ((result.notes & note) != 0) {
      notes.add(noteText(note));
    }
  }

  bool anyAsked = false;
  for (uint8_t field = 0; field < PROFILE_FIELDS; ++field) {
    anyAsked = anyAsked || profileField(result.profile, field) != 0;
  }
  if (anyAsked) {
    JsonArray asked = out["asked"].to<JsonArray>();
    for (uint8_t field = 0; field < PROFILE_FIELDS; ++field) {
      JsonArray row = asked.add<JsonArray>();
      row.add(profileFieldLabel(field));
      Text text(line, sizeof(line));
      describeProfileField(result.profile, field, &text);
      row.add(line);
    }
  }

  JsonArray registers = out["registers"].to<JsonArray>();
  for (uint8_t i = 0; i < result.planned && i < PLAN_ENTRIES; ++i) {
    const PlanEntry &entry = result.entries[i];
    JsonArray row = registers.add<JsonArray>();
    row.add(item(entry.item).name);
    row.add(item(entry.item).label);
    {
      Text text(line, sizeof(line));
      describeValue(entry.item, entry.before, result.scale, SCALE_UNKNOWN, &text);
      row.add(line);
    }
    {
      Text text(line, sizeof(line));
      describeValue(entry.item, entry.target, result.scale, SCALE_UNKNOWN, &text);
      row.add(line);
    }
    {
      Text text(line, sizeof(line));
      text.add(entryStatusName(entry.status));
      if (entry.status == ENTRY_ALTERED || entry.status == ENTRY_LOST) {
        text.add(": it holds ");
        describeValue(entry.item, entry.found, result.scale, SCALE_UNKNOWN, &text);
      }
      row.add(line);
    }
  }
}

}  // namespace sunsaver

#endif  // TANKALARM_SUNSAVER_REPORT_H
