/**
 * TankAlarm_SunSaverReport.h
 *
 * The charger check (TankAlarm_SunSaverSettings.h) as JSON: the compact report a client sends to
 * the server, which the server also keeps, and the readable form the server's pages show.
 *
 * This header needs ArduinoJson (v7), so the sketches include it themselves. TankAlarm_Common.h
 * does not, and neither does TankAlarm_Solar.cpp. Like the settings header it has no Arduino
 * dependency, and tests/host/sunsaver_settings runs it on a PC.
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

// A member as a small number. Anything that is not a whole number from 0 to `highest` reads as
// `fallback`: a report is input from the network.
inline uint8_t reportByte(JsonObjectConst in, const char *key, uint8_t highest, uint8_t fallback) {
  JsonVariantConst value = in[key];
  if (!value.is<uint32_t>()) {
    return fallback;
  }
  const uint32_t number = value.as<uint32_t>();
  return number <= highest ? (uint8_t)number : fallback;
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
    for (uint8_t digit = 0; digit < 4; ++digit) {
      const int8_t value = hexDigitValue(hex[(size_t)i * 4u + digit]);
      if (value < 0) {
        clearSnapshot(snapshot);
        return false;
      }
      raw = (uint16_t)((raw << 4) | (uint16_t)value);
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
// A client's clock may run ahead of the server's by this much.
static const double REPORT_CLOCK_AHEAD_SECONDS = 86400.0;

// The time of a check as the server takes it: the client's own, or 0 for "not known" when the
// client sent none or one that is plainly wrong. `serverNow` is the server's time, or anything
// below REPORT_EARLIEST_EPOCH when the server does not know the time either.
//
// "Not known" is never replaced by the time of arrival: that would let an old report that was
// held back pass for the newest one.
inline double reportCheckTime(double clientEpoch, double serverNow) {
  if (!(clientEpoch >= REPORT_EARLIEST_EPOCH)) {
    return 0.0;  // also for a value that is not a number
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

}  // namespace sunsaver

#endif  // TANKALARM_SUNSAVER_REPORT_H
