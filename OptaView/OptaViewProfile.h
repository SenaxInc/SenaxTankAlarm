/**
 * OptaViewProfile.h
 *
 * Device profiles: a controller's register map as data (JSON), not as C constants.
 *
 * The lesson from the TankAlarm SunSaver bring-up was that register addresses and scales
 * compiled into the firmware were wrong on the real hardware, and each fix needed a new build.
 * A profile carries every address, type, scale, unit, label, limit and "confirmed on our
 * bench" flag, so a correction is an edit to profiles/<name>.json.
 *
 * This file loads a profile with ArduinoJson (v7), copies what it needs into its own storage
 * and frees the JSON document. It also converts raw register words to engineering values and
 * back. No Arduino dependency other than ArduinoJson, so the host tests run the same code.
 */

#ifndef OPTAVIEW_PROFILE_H
#define OPTAVIEW_PROFILE_H

#include <ArduinoJson.h>

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "OptaViewModbus.h"
#include "OptaViewText.h"

// Every allocation of the loader goes through these two, so that the host tests can make any
// one of them fail and count what is still allocated.
#ifndef OV_ALLOC
#define OV_ALLOC(count, size) calloc((count), (size))
#define OV_FREE(pointer) free(pointer)
#endif

namespace optaview {

static const int OV_PROFILE_SCHEMA = 1;

enum OvType : uint8_t {
  OV_U16,   // one word, unsigned
  OV_I16,   // one word, two's complement
  OV_U8,    // low byte of one word, unsigned
  OV_I8,    // low byte of one word, two's complement
  OV_U32,   // two words
  OV_ENUM,  // one word, value names from a label set
  OV_BITS   // one or two words, bit names from a label set
};

struct OvScale {
  const char *name;
  double num;  // value = raw * num / den
  double den;
  const char *units;
  uint8_t decimals;
};

struct OvLabelSet {
  const char *name;
  uint8_t count;
  const char **items;  // index = enum value or bit number; nullptr = no name
};

struct OvRegister {
  uint16_t addr;
  uint8_t words;      // 1 or 2
  uint8_t type;       // OvType
  bool loFirst;       // two-word values: the low word is at the lower address
  bool writable;
  bool bench;         // confirmed on our hardware; false = from the specification only
  bool hasMin;
  bool hasMax;
  bool zeroOk;        // 0 is allowed outside min..max (it switches the feature off)
  bool hasSentinel;
  uint16_t sentinelRaw;
  uint8_t decimals;
  const char *name;
  const char *label;
  const char *group;
  const char *units;
  const char *note;
  const char *sentinelText;
  const OvScale *scale;
  const OvLabelSet *labels;
  double minValue;
  double maxValue;
};

struct OvCoil {
  uint16_t addr;
  bool trigger;  // write-only action; reads back 0
  const char *name;
  const char *label;
};

struct OvGroup {
  const char *name;
  const char *title;
};

// "below" must be lower than "above". Skipped while either one is 0 (switched off).
struct OvCheck {
  const OvRegister *below;
  const OvRegister *above;
  const char *message;
};

// How to find out which numerators the voltage scales really have. Stored settings and live
// readings each have a scale of their own and are tested apart: a controller could use one
// figure for what it measures and another for what it is told.
struct OvScaleCheck {
  bool present;                      // at least one of the two halves is set up
  double alternates[4];              // numerators either scale might really have
  uint8_t alternateCount;
  double grid;                       // settings are normally multiples of this many volts

  OvScale *settingsScale;            // scale of the stored voltage settings; nullptr = not tested
  double settingsFileNum;            // its numerator as the profile file has it
  const OvRegister *setpoints[24];
  uint8_t setpointCount;

  OvScale *liveScale;                // scale of the live voltage readings; nullptr = not tested
  double liveFileNum;
  const OvRegister *meter;           // live battery voltage, for a multimeter comparison
  const OvRegister *battery;         // battery voltage to compare with the regulation target
  const OvRegister *reference;       // regulation target, on a scale of its own
  const OvRegister *state;           // charge state register
  uint16_t regulating[8];            // state values in which the battery is held at the target
  uint8_t regulatingCount;
};

struct OvModbusDefaults {
  uint8_t slaveId;
  uint32_t baud;
  uint8_t format;        // MbFormat
  uint8_t readFunction;  // 3 or 4
  uint8_t maxBlock;      // most registers to ask for in one request
  uint16_t responseTimeoutMs;
  uint16_t resetWaitMs;
  const OvRegister *probeRegister;
  const OvCoil *resetCoil;
};

// ---------------------------------------------------------------------------------------------
// Raw words <-> values
// ---------------------------------------------------------------------------------------------

inline uint32_t ovRawValue(const OvRegister &reg, const uint16_t *words) {
  if (reg.words == 2) {
    return reg.loFirst ? (((uint32_t)words[1] << 16) | words[0]) : (((uint32_t)words[0] << 16) | words[1]);
  }
  return words[0];
}

// The unscaled count with the register's signedness applied. Current registers are signed:
// reading 0xFFF0 as unsigned turned a small negative night-time current into 158 A (F-26).
inline double ovCount(const OvRegister &reg, const uint16_t *words) {
  switch (reg.type) {
    case OV_I16: return (double)(int16_t)words[0];
    case OV_U8: return (double)(uint8_t)(words[0] & 0xFF);
    case OV_I8: return (double)(int8_t)(uint8_t)(words[0] & 0xFF);
    default: return (double)ovRawValue(reg, words);
  }
}

inline double ovValue(const OvRegister &reg, const uint16_t *words) {
  const double count = ovCount(reg, words);
  return reg.scale != nullptr ? count * reg.scale->num / reg.scale->den : count;
}

inline bool ovIsSentinel(const OvRegister &reg, const uint16_t *words) {
  if (!reg.hasSentinel) {
    return false;
  }
  const uint16_t raw = (reg.type == OV_U8 || reg.type == OV_I8) ? (uint16_t)(words[0] & 0xFF) : words[0];
  return raw == reg.sentinelRaw;
}

inline bool ovIsNumeric(const OvRegister &reg) { return reg.type != OV_ENUM && reg.type != OV_BITS; }

// The bits of a one-word register that the controller uses: the low byte of a u8 or i8, all
// sixteen of anything else. Two words that agree in those bits hold the same value, whatever
// the rest holds.
inline uint16_t ovUsedBits(const OvRegister &reg) {
  return (reg.type == OV_U8 || reg.type == OV_I8) ? (uint16_t)0x00FF : (uint16_t)0xFFFF;
}

inline bool ovSameValue(const OvRegister &reg, uint16_t a, uint16_t b) {
  return ((uint16_t)(a ^ b) & ovUsedBits(reg)) == 0;
}

// The value as text: "13.68 V", "FLOAT (7)", "SW1: custom battery settings, SW2: custom LVD/LVR"
// (the bits that are set), "none".
inline void ovFormatValue(const OvRegister &reg, const uint16_t *words, char *out, size_t cap) {
  OvText text(out, cap);
  if (ovIsSentinel(reg, words)) {
    text.add(reg.sentinelText);
    return;
  }
  if (reg.type == OV_ENUM) {
    const uint16_t value = words[0];
    const char *name = (reg.labels != nullptr && value < reg.labels->count) ? reg.labels->items[value] : nullptr;
    text.add(name != nullptr ? name : "unknown");
    text.add(" (");
    text.addU(value);
    text.add(")");
    return;
  }
  if (reg.type == OV_BITS) {
    const uint32_t bits = ovRawValue(reg, words);
    const uint8_t width = (uint8_t)(reg.words * 16);
    bool any = false;
    for (uint8_t position = 0; position < width; ++position) {
      if ((bits & ((uint32_t)1 << position)) == 0) {
        continue;
      }
      if (any) {
        text.add(", ");
      }
      any = true;
      const char *name =
          (reg.labels != nullptr && position < reg.labels->count) ? reg.labels->items[position] : nullptr;
      if (name != nullptr) {
        text.add(name);
      } else {
        text.add("bit ");
        text.addU(position);
      }
    }
    if (!any) {
      text.add("none");
    }
    return;
  }
  text.addFixed(ovValue(reg, words), reg.decimals);
  if (reg.units != nullptr && reg.units[0] != '\0') {
    text.add(" ");
    text.add(reg.units);
  }
}

enum OvEncodeStatus : uint8_t {
  OV_ENCODE_OK = 0,
  OV_ENCODE_NOT_WRITABLE,
  OV_ENCODE_BELOW_MIN,
  OV_ENCODE_ABOVE_MAX,
  OV_ENCODE_OUT_OF_RANGE  // does not fit the register
};

// Engineering value -> the raw word to write. Applies the profile's limits. A u8 goes in the low
// byte, with 0 in the high byte, which the controller does not use.
inline OvEncodeStatus ovEncode(const OvRegister &reg, double value, uint16_t *raw) {
  if (!reg.writable || reg.words != 1 || (reg.type != OV_U16 && reg.type != OV_I16 && reg.type != OV_U8)) {
    return OV_ENCODE_NOT_WRITABLE;
  }
  if (value != value) {
    return OV_ENCODE_OUT_OF_RANGE;
  }
  if (!(reg.zeroOk && value == 0.0)) {
    if (reg.hasMin && value < reg.minValue) {
      return OV_ENCODE_BELOW_MIN;
    }
    if (reg.hasMax && value > reg.maxValue) {
      return OV_ENCODE_ABOVE_MAX;
    }
  }
  const double count = reg.scale != nullptr ? value * reg.scale->den / reg.scale->num : value;
  const double rounded = floor(count + 0.5);
  if (reg.type == OV_I16) {
    if (rounded < -32768.0 || rounded > 32767.0) {
      return OV_ENCODE_OUT_OF_RANGE;
    }
    *raw = (uint16_t)(int16_t)rounded;
  } else {
    if (rounded < 0.0 || rounded > (reg.type == OV_U8 ? 255.0 : 65535.0)) {
      return OV_ENCODE_OUT_OF_RANGE;
    }
    *raw = (uint16_t)rounded;
  }
  return OV_ENCODE_OK;
}

// ---------------------------------------------------------------------------------------------
// Numbers in a profile
// ---------------------------------------------------------------------------------------------

// ArduinoJson keeps a decimal of up to seven digits in single precision, so the 96.667 in a
// profile arrives as 96.66699982. That is harmless for a scale, but a limit of 0.3 would then
// refuse a typed "0.3". ovExactDecimal() undoes it: for a value that is exactly a
// single-precision number it returns the shortest decimal that single precision cannot tell
// from it, as the nearest double. A decimal of up to six significant digits comes back exactly
// as written and equal to the same text read by ovParseDecimal(); anything else moves by less
// than one part in two million, or not at all.
inline double ovExactDecimal(double value) {
  if (value != value) {
    return value;
  }
  const double magnitude = value < 0.0 ? -value : value;
  if (magnitude < 1.0e-7 || magnitude >= 1.0e15) {
    return value;  // zero, infinity, and sizes no profile uses
  }
  if ((double)(float)value != value) {
    return value;  // read in double precision already
  }
  const double tolerance = magnitude * 4.0e-7;
  double power = 1.0;
  for (int decimals = 0; decimals <= 15 && magnitude * power < 1.0e15; ++decimals, power *= 10.0) {
    const double candidate = floor(magnitude * power + 0.5) / power;
    const double gap = candidate > magnitude ? candidate - magnitude : magnitude - candidate;
    if (gap <= tolerance) {
      return value < 0.0 ? -candidate : candidate;
    }
  }
  return value;
}

inline double ovJsonNumber(JsonVariantConst value) { return ovExactDecimal(value.as<double>()); }

// ---------------------------------------------------------------------------------------------
// The profile
// ---------------------------------------------------------------------------------------------

class OvProfile {
 public:
  OvProfile() { zero(); }
  ~OvProfile() { clear(); }

  bool loaded() const { return loaded_; }

  // Parses a profile. On failure returns false, leaves the profile empty and explains why in
  // `error`.
  bool load(const char *json, size_t len, char *error, size_t errorCap) {
    clear();
    if (errorCap > 0) {
      error[0] = '\0';
    }
    const bool ok = parse(json, len, error, errorCap);
    if (!ok) {
      clear();
    }
    loaded_ = ok;
    return ok;
  }

  void clear() {
    if (labelSets_ != nullptr) {
      for (size_t i = 0; i < labelSetCount_; ++i) {
        OV_FREE(labelSets_[i].items);
      }
    }
    OV_FREE(arena_);
    OV_FREE(scales_);
    OV_FREE(labelSets_);
    OV_FREE(groups_);
    OV_FREE(registers_);
    OV_FREE(coils_);
    OV_FREE(checks_);
    OV_FREE(notes_);
    OV_FREE(writeNotes_);
    OV_FREE(wiring_);
    OV_FREE(liveGroups_);
    zero();
  }

  const char *id() const { return id_; }
  const char *title() const { return title_; }
  const char *version() const { return version_; }
  const char *source() const { return source_; }
  const char *verifiedBy() const { return verifiedBy_; }
  const OvModbusDefaults &modbus() const { return modbus_; }
  const OvScaleCheck &scaleCheck() const { return scaleCheck_; }

  size_t noteCount() const { return noteCount_; }
  const char *note(size_t i) const { return notes_[i]; }
  size_t writeNoteCount() const { return writeNoteCount_; }
  const char *writeNote(size_t i) const { return writeNotes_[i]; }
  // Wiring and switch checklist, shown when nothing answers.
  size_t wiringCount() const { return wiringCount_; }
  const char *wiring(size_t i) const { return wiring_[i]; }

  size_t registerCount() const { return registerCount_; }
  const OvRegister &reg(size_t i) const { return registers_[i]; }
  const OvRegister *findRegister(const char *name) const {
    for (size_t i = 0; i < registerCount_; ++i) {
      if (ovEqualsIgnoreCase(registers_[i].name, name)) {
        return &registers_[i];
      }
    }
    return nullptr;
  }
  // The register that starts at addr.
  const OvRegister *findRegisterAt(uint16_t addr) const {
    for (size_t i = 0; i < registerCount_; ++i) {
      if (registers_[i].addr == addr) {
        return &registers_[i];
      }
    }
    return nullptr;
  }

  size_t groupCount() const { return groupCount_; }
  const OvGroup &group(size_t i) const { return groups_[i]; }
  const OvGroup *findGroup(const char *name) const {
    for (size_t i = 0; i < groupCount_; ++i) {
      if (ovEqualsIgnoreCase(groups_[i].name, name)) {
        return &groups_[i];
      }
    }
    return nullptr;
  }
  size_t liveGroupCount() const { return liveGroupCount_; }
  const char *liveGroup(size_t i) const { return liveGroups_[i]; }

  size_t coilCount() const { return coilCount_; }
  const OvCoil &coil(size_t i) const { return coils_[i]; }
  const OvCoil *findCoil(const char *name) const {
    for (size_t i = 0; i < coilCount_; ++i) {
      if (ovEqualsIgnoreCase(coils_[i].name, name)) {
        return &coils_[i];
      }
    }
    return nullptr;
  }

  size_t checkCount() const { return checkCount_; }
  const OvCheck &check(size_t i) const { return checks_[i]; }

  size_t scaleCount() const { return scaleCount_; }
  const OvScale &scale(size_t i) const { return scales_[i]; }
  // For a per-session override while the true scale is being settled on the bench.
  OvScale *findScale(const char *name) {
    for (size_t i = 0; i < scaleCount_; ++i) {
      if (ovEqualsIgnoreCase(scales_[i].name, name)) {
        return &scales_[i];
      }
    }
    return nullptr;
  }

 private:
  OvProfile(const OvProfile &);
  OvProfile &operator=(const OvProfile &);

  bool loaded_;
  char *arena_;
  size_t arenaCap_;
  size_t arenaUsed_;
  const char *id_;
  const char *title_;
  const char *version_;
  const char *source_;
  const char *verifiedBy_;
  OvScale *scales_;
  size_t scaleCount_;
  OvLabelSet *labelSets_;
  size_t labelSetCount_;
  OvGroup *groups_;
  size_t groupCount_;
  size_t groupCap_;
  OvRegister *registers_;
  size_t registerCount_;
  OvCoil *coils_;
  size_t coilCount_;
  OvCheck *checks_;
  size_t checkCount_;
  const char **notes_;
  size_t noteCount_;
  const char **writeNotes_;
  size_t writeNoteCount_;
  const char **wiring_;
  size_t wiringCount_;
  const char **liveGroups_;
  size_t liveGroupCount_;
  OvModbusDefaults modbus_;
  OvScaleCheck scaleCheck_;

  void zero() {
    loaded_ = false;
    arena_ = nullptr;
    arenaCap_ = 0;
    arenaUsed_ = 0;
    id_ = title_ = version_ = source_ = verifiedBy_ = "";
    scales_ = nullptr;
    scaleCount_ = 0;
    labelSets_ = nullptr;
    labelSetCount_ = 0;
    groups_ = nullptr;
    groupCount_ = 0;
    groupCap_ = 0;
    registers_ = nullptr;
    registerCount_ = 0;
    coils_ = nullptr;
    coilCount_ = 0;
    checks_ = nullptr;
    checkCount_ = 0;
    notes_ = nullptr;
    noteCount_ = 0;
    writeNotes_ = nullptr;
    writeNoteCount_ = 0;
    wiring_ = nullptr;
    wiringCount_ = 0;
    liveGroups_ = nullptr;
    liveGroupCount_ = 0;
    memset(&modbus_, 0, sizeof(modbus_));
    memset(&scaleCheck_, 0, sizeof(scaleCheck_));
  }

  // Copies a string into the arena. Every kept string comes from the JSON text, so the arena
  // (sized to that text) cannot run out; the bound is checked anyway.
  const char *keep(const char *text) {
    if (text == nullptr) {
      return "";
    }
    const size_t n = strlen(text) + 1;
    if (arena_ == nullptr || arenaUsed_ + n > arenaCap_) {
      return "";
    }
    char *at = arena_ + arenaUsed_;
    memcpy(at, text, n);
    arenaUsed_ += n;
    return at;
  }

  static bool fail(char *error, size_t cap, const char *what, const char *which = nullptr) {
    OvText text(error, cap);
    text.add(what);
    if (which != nullptr && which[0] != '\0') {
      text.add(": ");
      text.add(which);
    }
    return false;
  }

  static bool jsonU32(JsonVariantConst v, uint32_t *out) {
    if (v.is<const char *>()) {
      return ovParseU32(v.as<const char *>(), out);
    }
    if (v.is<uint32_t>()) {
      *out = v.as<uint32_t>();
      return true;
    }
    return false;
  }

  static bool parseType(const char *text, uint8_t *type) {
    static const struct {
      const char *name;
      uint8_t type;
    } kTypes[] = {{"u16", OV_U16}, {"i16", OV_I16}, {"u8", OV_U8},    {"i8", OV_I8},
                  {"u32", OV_U32}, {"enum", OV_ENUM}, {"bits", OV_BITS}};
    for (size_t i = 0; i < sizeof(kTypes) / sizeof(kTypes[0]); ++i) {
      if (strcmp(text, kTypes[i].name) == 0) {
        *type = kTypes[i].type;
        return true;
      }
    }
    return false;
  }

  static bool parseFormat(const char *text, uint8_t *format) {
    for (uint8_t f = MB_8N1; f <= MB_8O1; ++f) {
      if (ovEqualsIgnoreCase(text, mbFormatName(f))) {
        *format = f;
        return true;
      }
    }
    return false;
  }

  // A name must start with a letter or underscore, so it can never be mistaken for an address.
  static bool validName(const char *name) {
    if (name == nullptr || !(isalpha((unsigned char)name[0]) || name[0] == '_')) {
      return false;
    }
    for (const char *p = name; *p != '\0'; ++p) {
      if (!(isalnum((unsigned char)*p) || *p == '_')) {
        return false;
      }
    }
    return true;
  }

  const OvScale *scaleByName(const char *name) const {
    for (size_t i = 0; i < scaleCount_; ++i) {
      if (strcmp(scales_[i].name, name) == 0) {
        return &scales_[i];
      }
    }
    return nullptr;
  }

  const OvLabelSet *labelsByName(const char *name) const {
    for (size_t i = 0; i < labelSetCount_; ++i) {
      if (strcmp(labelSets_[i].name, name) == 0) {
        return &labelSets_[i];
      }
    }
    return nullptr;
  }

  // Returns the stored name of a group, adding the group when it is new.
  const char *internGroup(const char *name, const char *title) {
    for (size_t i = 0; i < groupCount_; ++i) {
      if (strcmp(groups_[i].name, name) == 0) {
        return groups_[i].name;
      }
    }
    if (groupCount_ >= groupCap_) {
      return nullptr;
    }
    groups_[groupCount_].name = keep(name);
    groups_[groupCount_].title = title != nullptr ? keep(title) : groups_[groupCount_].name;
    return groups_[groupCount_++].name;
  }

  bool loadStringList(JsonArrayConst list, const char ***items, size_t *count) {
    *count = 0;
    *items = nullptr;
    if (list.isNull() || list.size() == 0) {
      return true;
    }
    *items = (const char **)OV_ALLOC(list.size(), sizeof(const char *));
    if (*items == nullptr) {
      return false;
    }
    for (JsonVariantConst item : list) {
      const char *text = item.as<const char *>();
      if (text != nullptr) {
        (*items)[(*count)++] = keep(text);
      }
    }
    return true;
  }

  bool parse(const char *json, size_t len, char *error, size_t errorCap) {
    if (json == nullptr || len == 0) {
      return fail(error, errorCap, "the profile is empty");
    }
    JsonDocument doc;
    const DeserializationError parseError = deserializeJson(doc, json, len);
    if (parseError) {
      return fail(error, errorCap, "the profile is not valid JSON", parseError.c_str());
    }
    JsonObjectConst root = doc.as<JsonObjectConst>();
    if (root.isNull()) {
      return fail(error, errorCap, "the profile must be a JSON object");
    }
    if ((root["schema"] | 0) != OV_PROFILE_SCHEMA) {
      return fail(error, errorCap, "unsupported profile schema (this firmware reads schema 1)");
    }

    arenaCap_ = len + 16;
    arena_ = (char *)OV_ALLOC(arenaCap_, 1);
    if (arena_ == nullptr) {
      return fail(error, errorCap, "out of memory");
    }
    arenaUsed_ = 0;

    id_ = keep(root["profile"] | "");
    title_ = keep(root["title"] | "");
    version_ = keep(root["version"] | "");
    source_ = keep(root["source"] | "");
    verifiedBy_ = keep(root["verifiedBy"] | "");
    if (id_[0] == '\0') {
      return fail(error, errorCap, "the profile has no \"profile\" name");
    }
    if (!loadStringList(root["notes"].as<JsonArrayConst>(), &notes_, &noteCount_) ||
        !loadStringList(root["writeNotes"].as<JsonArrayConst>(), &writeNotes_, &writeNoteCount_) ||
        !loadStringList(root["wiring"].as<JsonArrayConst>(), &wiring_, &wiringCount_)) {
      return fail(error, errorCap, "out of memory");
    }

    // --- scales ---
    JsonObjectConst scales = root["scales"].as<JsonObjectConst>();
    if (!scales.isNull() && scales.size() > 0) {
      scales_ = (OvScale *)OV_ALLOC(scales.size(), sizeof(OvScale));
      if (scales_ == nullptr) {
        return fail(error, errorCap, "out of memory");
      }
      for (JsonPairConst pair : scales) {
        JsonVariantConst v = pair.value();
        OvScale &s = scales_[scaleCount_];
        s.name = keep(pair.key().c_str());
        if (!v["num"].is<double>() || !v["den"].is<double>()) {
          return fail(error, errorCap, "a scale needs \"num\" and \"den\"", s.name);
        }
        s.num = ovJsonNumber(v["num"]);
        s.den = ovJsonNumber(v["den"]);
        if (!(s.num > 0.0) || !(s.den > 0.0)) {
          return fail(error, errorCap, "a scale's \"num\" and \"den\" must be above zero", s.name);
        }
        s.units = keep(v["units"] | "");
        const int decimals = v["decimals"] | 0;
        s.decimals = (uint8_t)(decimals < 0 ? 0 : (decimals > 6 ? 6 : decimals));
        ++scaleCount_;
      }
    }

    // --- label sets (enum values and bit names) ---
    JsonObjectConst labels = root["labels"].as<JsonObjectConst>();
    if (!labels.isNull() && labels.size() > 0) {
      labelSets_ = (OvLabelSet *)OV_ALLOC(labels.size(), sizeof(OvLabelSet));
      if (labelSets_ == nullptr) {
        return fail(error, errorCap, "out of memory");
      }
      for (JsonPairConst pair : labels) {
        JsonArrayConst items = pair.value().as<JsonArrayConst>();
        OvLabelSet &set = labelSets_[labelSetCount_];
        set.name = keep(pair.key().c_str());
        if (items.isNull() || items.size() == 0 || items.size() > 32) {
          return fail(error, errorCap, "a label set must be a list of 1 to 32 names", set.name);
        }
        set.items = (const char **)OV_ALLOC(items.size(), sizeof(const char *));
        ++labelSetCount_;  // counted now so clear() frees items on a later failure
        if (set.items == nullptr) {
          return fail(error, errorCap, "out of memory");
        }
        for (JsonVariantConst item : items) {
          const char *text = item.as<const char *>();
          set.items[set.count++] = text != nullptr ? keep(text) : nullptr;
        }
      }
    }

    // --- registers ---
    JsonArrayConst registers = root["registers"].as<JsonArrayConst>();
    if (registers.isNull() || registers.size() == 0) {
      return fail(error, errorCap, "the profile has no registers");
    }
    JsonArrayConst groups = root["groups"].as<JsonArrayConst>();
    groupCap_ = (groups.isNull() ? 0 : groups.size()) + registers.size() + 1;
    groups_ = (OvGroup *)OV_ALLOC(groupCap_, sizeof(OvGroup));
    registers_ = (OvRegister *)OV_ALLOC(registers.size(), sizeof(OvRegister));
    if (groups_ == nullptr || registers_ == nullptr) {
      return fail(error, errorCap, "out of memory");
    }
    if (!groups.isNull()) {
      for (JsonVariantConst g : groups) {
        const char *name = g["name"].as<const char *>();
        if (name == nullptr || name[0] == '\0') {
          return fail(error, errorCap, "a group needs a \"name\"");
        }
        internGroup(name, g["title"].as<const char *>());
      }
    }

    for (JsonVariantConst v : registers) {
      OvRegister &r = registers_[registerCount_];
      const char *name = v["name"].as<const char *>();
      if (!validName(name)) {
        return fail(error, errorCap, "a register needs a \"name\" made of letters, digits and _",
                    name != nullptr ? name : "");
      }
      uint32_t addr = 0;
      if (!jsonU32(v["addr"], &addr) || addr > 0xFFFF) {
        return fail(error, errorCap, "bad or missing \"addr\" on register", name);
      }
      r.addr = (uint16_t)addr;
      r.name = keep(name);
      r.label = v["label"].is<const char *>() ? keep(v["label"].as<const char *>()) : r.name;
      r.note = keep(v["note"] | "");

      r.type = OV_U16;
      if (v["type"].is<const char *>() && !parseType(v["type"].as<const char *>(), &r.type)) {
        return fail(error, errorCap, "unknown \"type\" on register", name);
      }
      r.words = 1;
      if (r.type == OV_U32) {
        r.words = 2;
      } else if (r.type == OV_BITS) {
        const int words = v["words"] | 1;
        if (words != 1 && words != 2) {
          return fail(error, errorCap, "\"words\" must be 1 or 2 on register", name);
        }
        r.words = (uint8_t)words;
      }
      if ((uint32_t)r.addr + r.words > 0x10000UL) {
        return fail(error, errorCap, "register runs past address 0xFFFF", name);
      }
      const char *order = v["order"] | "hi-lo";
      if (strcmp(order, "lo-hi") == 0) {
        r.loFirst = true;
      } else if (strcmp(order, "hi-lo") != 0) {
        return fail(error, errorCap, "\"order\" must be \"hi-lo\" or \"lo-hi\" on register", name);
      }

      if (v["scale"].is<const char *>()) {
        r.scale = scaleByName(v["scale"].as<const char *>());
        if (r.scale == nullptr) {
          return fail(error, errorCap, "unknown \"scale\" on register", name);
        }
        if (r.type == OV_ENUM || r.type == OV_BITS) {
          return fail(error, errorCap, "an enum or bits register cannot have a scale", name);
        }
      }
      if (v["labels"].is<const char *>()) {
        r.labels = labelsByName(v["labels"].as<const char *>());
        if (r.labels == nullptr) {
          return fail(error, errorCap, "unknown \"labels\" on register", name);
        }
      }
      r.units = v["units"].is<const char *>() ? keep(v["units"].as<const char *>())
                                              : (r.scale != nullptr ? r.scale->units : "");
      int decimals = r.scale != nullptr ? r.scale->decimals : 0;
      decimals = v["decimals"] | decimals;
      r.decimals = (uint8_t)(decimals < 0 ? 0 : (decimals > 6 ? 6 : decimals));

      const char *groupName = v["group"] | "other";
      r.group = internGroup(groupName, nullptr);
      if (r.group == nullptr) {
        return fail(error, errorCap, "too many groups");
      }

      const char *verified = v["verified"] | "spec";
      if (strcmp(verified, "bench") == 0) {
        r.bench = true;
      } else if (strcmp(verified, "spec") != 0) {
        return fail(error, errorCap, "\"verified\" must be \"bench\" or \"spec\" on register", name);
      }

      r.writable = v["writable"] | false;
      r.zeroOk = v["zeroOk"] | false;
      if (v["min"].is<double>()) {
        r.hasMin = true;
        r.minValue = ovJsonNumber(v["min"]);
      }
      if (v["max"].is<double>()) {
        r.hasMax = true;
        r.maxValue = ovJsonNumber(v["max"]);
      }
      if (r.hasMin && r.hasMax && r.minValue > r.maxValue) {
        return fail(error, errorCap, "\"min\" is above \"max\" on register", name);
      }
      if (r.writable && (r.words != 1 || (r.type != OV_U16 && r.type != OV_I16 && r.type != OV_U8))) {
        return fail(error, errorCap, "only one-word u16, i16 and u8 registers can be writable", name);
      }

      JsonVariantConst sentinel = v["sentinel"];
      if (!sentinel.isNull()) {
        uint32_t raw = 0;
        if (!jsonU32(sentinel["raw"], &raw) || raw > 0xFFFF || !sentinel["text"].is<const char *>()) {
          return fail(error, errorCap, "a \"sentinel\" needs \"raw\" and \"text\" on register", name);
        }
        r.hasSentinel = true;
        r.sentinelRaw = (uint16_t)raw;
        r.sentinelText = keep(sentinel["text"].as<const char *>());
      } else {
        r.sentinelText = "";
      }
      ++registerCount_;
    }

    // Sort by address (insertion sort; the list is short and usually sorted already).
    for (size_t i = 1; i < registerCount_; ++i) {
      const OvRegister moving = registers_[i];
      size_t j = i;
      for (; j > 0 && registers_[j - 1].addr > moving.addr; --j) {
        registers_[j] = registers_[j - 1];
      }
      registers_[j] = moving;
    }
    for (size_t i = 0; i < registerCount_; ++i) {
      if (i + 1 < registerCount_ &&
          (uint32_t)registers_[i].addr + registers_[i].words > registers_[i + 1].addr) {
        return fail(error, errorCap, "two registers share an address", registers_[i + 1].name);
      }
      for (size_t j = i + 1; j < registerCount_; ++j) {
        if (ovEqualsIgnoreCase(registers_[i].name, registers_[j].name)) {
          return fail(error, errorCap, "two registers share a name", registers_[i].name);
        }
      }
    }

    // --- coils ---
    JsonArrayConst coils = root["coils"].as<JsonArrayConst>();
    if (!coils.isNull() && coils.size() > 0) {
      coils_ = (OvCoil *)OV_ALLOC(coils.size(), sizeof(OvCoil));
      if (coils_ == nullptr) {
        return fail(error, errorCap, "out of memory");
      }
      for (JsonVariantConst v : coils) {
        OvCoil &c = coils_[coilCount_];
        const char *name = v["name"].as<const char *>();
        uint32_t addr = 0;
        if (!validName(name)) {
          return fail(error, errorCap, "a coil needs a \"name\" made of letters, digits and _",
                      name != nullptr ? name : "");
        }
        if (!jsonU32(v["addr"], &addr) || addr > 0xFFFF) {
          return fail(error, errorCap, "bad or missing \"addr\" on coil", name);
        }
        if (findCoil(name) != nullptr) {
          return fail(error, errorCap, "two coils share a name", name);
        }
        c.addr = (uint16_t)addr;
        c.name = keep(name);
        c.label = v["label"].is<const char *>() ? keep(v["label"].as<const char *>()) : c.name;
        c.trigger = v["trigger"] | false;
        ++coilCount_;
      }
    }

    // --- link defaults ---
    JsonVariantConst modbus = root["modbus"];
    const int slaveId = modbus["slaveId"] | 1;
    const int readFunction = modbus["readFunction"] | 4;
    const int maxBlock = modbus["maxBlock"] | 16;
    const long baud = modbus["baud"] | 9600L;
    const int timeoutMs = modbus["responseTimeoutMs"] | 1000;
    const int resetWaitMs = modbus["resetWaitMs"] | 3000;
    if (slaveId < 1 || slaveId > 247) {
      return fail(error, errorCap, "modbus.slaveId must be 1 to 247");
    }
    if (readFunction != MB_FC_READ_HOLDING && readFunction != MB_FC_READ_INPUT) {
      return fail(error, errorCap, "modbus.readFunction must be 3 or 4");
    }
    if (maxBlock < 1 || maxBlock > (int)MB_MAX_READ_REGISTERS) {
      return fail(error, errorCap, "modbus.maxBlock must be 1 to 125");
    }
    if (baud < 300 || baud > 1000000L) {
      return fail(error, errorCap, "modbus.baud is out of range");
    }
    if (timeoutMs < 50 || timeoutMs > 10000 || resetWaitMs < 0 || resetWaitMs > 60000) {
      return fail(error, errorCap, "modbus.responseTimeoutMs or modbus.resetWaitMs is out of range");
    }
    modbus_.slaveId = (uint8_t)slaveId;
    modbus_.readFunction = (uint8_t)readFunction;
    modbus_.maxBlock = (uint8_t)maxBlock;
    modbus_.baud = (uint32_t)baud;
    modbus_.responseTimeoutMs = (uint16_t)timeoutMs;
    modbus_.resetWaitMs = (uint16_t)resetWaitMs;
    modbus_.format = MB_8N2;
    if (modbus["format"].is<const char *>() &&
        !parseFormat(modbus["format"].as<const char *>(), &modbus_.format)) {
      return fail(error, errorCap, "modbus.format must be 8N1, 8N2, 8E1 or 8O1");
    }
    modbus_.probeRegister = &registers_[0];
    if (modbus["probeRegister"].is<const char *>()) {
      modbus_.probeRegister = findRegister(modbus["probeRegister"].as<const char *>());
      if (modbus_.probeRegister == nullptr) {
        return fail(error, errorCap, "modbus.probeRegister names no register");
      }
    }
    modbus_.resetCoil = nullptr;
    if (modbus["resetCoil"].is<const char *>()) {
      modbus_.resetCoil = findCoil(modbus["resetCoil"].as<const char *>());
      if (modbus_.resetCoil == nullptr) {
        return fail(error, errorCap, "modbus.resetCoil names no coil");
      }
    }

    // --- which groups 'live' shows ---
    JsonArrayConst liveGroups = root["liveGroups"].as<JsonArrayConst>();
    if (!liveGroups.isNull() && liveGroups.size() > 0) {
      liveGroups_ = (const char **)OV_ALLOC(liveGroups.size(), sizeof(const char *));
      if (liveGroups_ == nullptr) {
        return fail(error, errorCap, "out of memory");
      }
      for (JsonVariantConst g : liveGroups) {
        const OvGroup *group = findGroup(g.as<const char *>());
        if (group == nullptr) {
          return fail(error, errorCap, "liveGroups names an unknown group", g.as<const char *>());
        }
        liveGroups_[liveGroupCount_++] = group->name;
      }
    }

    // --- pair checks ---
    JsonArrayConst checks = root["checks"].as<JsonArrayConst>();
    if (!checks.isNull() && checks.size() > 0) {
      checks_ = (OvCheck *)OV_ALLOC(checks.size(), sizeof(OvCheck));
      if (checks_ == nullptr) {
        return fail(error, errorCap, "out of memory");
      }
      for (JsonVariantConst v : checks) {
        OvCheck &c = checks_[checkCount_];
        c.below = findRegister(v["below"].as<const char *>());
        c.above = findRegister(v["above"].as<const char *>());
        if (c.below == nullptr || c.above == nullptr || !ovIsNumeric(*c.below) || !ovIsNumeric(*c.above)) {
          return fail(error, errorCap, "a check needs \"below\" and \"above\" naming numeric registers");
        }
        c.message = keep(v["message"] | "");
        ++checkCount_;
      }
    }

    // --- voltage scale check ---
    JsonVariantConst sc = root["scaleCheck"];
    if (!sc.isNull()) {
      OvScaleCheck &check = scaleCheck_;
      for (JsonVariantConst alt : sc["alternates"].as<JsonArrayConst>()) {
        if (check.alternateCount < sizeof(check.alternates) / sizeof(check.alternates[0]) &&
            alt.is<double>() && alt.as<double>() > 0.0) {
          check.alternates[check.alternateCount++] = ovJsonNumber(alt);
        }
      }
      check.grid = sc["grid"].is<double>() ? ovJsonNumber(sc["grid"]) : 0.05;
      if (!(check.grid > 0.0)) {
        return fail(error, errorCap, "scaleCheck.grid must be above zero");
      }

      // Stored settings: the setpoints whose roundness is judged.
      JsonVariantConst settings = sc["settings"];
      if (!settings.isNull()) {
        check.settingsScale = findScale(settings["scale"] | "");
        if (check.settingsScale == nullptr) {
          return fail(error, errorCap, "scaleCheck.settings.scale names no scale");
        }
        check.settingsFileNum = check.settingsScale->num;
        for (JsonVariantConst item : settings["setpoints"].as<JsonArrayConst>()) {
          const OvRegister *reg = findRegister(item.as<const char *>());
          if (reg == nullptr || reg->scale != check.settingsScale || reg->words != 1) {
            return fail(error, errorCap,
                        "scaleCheck.settings.setpoints must name one-word registers that use scaleCheck.settings.scale",
                        item.as<const char *>());
          }
          if (check.setpointCount < sizeof(check.setpoints) / sizeof(check.setpoints[0])) {
            check.setpoints[check.setpointCount++] = reg;
          }
        }
      }

      // Live readings: the registers behind the regulation and multimeter tests.
      JsonVariantConst live = sc["live"];
      if (!live.isNull()) {
        check.liveScale = findScale(live["scale"] | "");
        if (check.liveScale == nullptr) {
          return fail(error, errorCap, "scaleCheck.live.scale names no scale");
        }
        if (check.liveScale == check.settingsScale) {
          // One scale for both would let a multimeter reading decide how settings are written.
          return fail(error, errorCap, "scaleCheck.live.scale and scaleCheck.settings.scale must be two scales");
        }
        check.liveFileNum = check.liveScale->num;
        // The same rule for every setting, whether or not it is one of the setpoints above.
        for (size_t i = 0; i < registerCount_; ++i) {
          if (registers_[i].writable && registers_[i].scale == check.liveScale) {
            return fail(error, errorCap, "a writable register cannot use scaleCheck.live.scale", registers_[i].name);
          }
        }
        if (!namedRegister(live["meter"], &check.meter)) {
          return fail(error, errorCap, "scaleCheck.live.meter names no register");
        }
        if (check.meter == nullptr) {
          // The regulation test only points (ovJudgeLive), so without a multimeter comparison
          // nothing could ever confirm this scale.
          return fail(error, errorCap, "scaleCheck.live.meter is needed: only a multimeter reading settles this scale");
        }
        if (!namedRegister(live["battery"], &check.battery)) {
          return fail(error, errorCap, "scaleCheck.live.battery names no register");
        }
        if (!namedRegister(live["reference"], &check.reference)) {
          return fail(error, errorCap, "scaleCheck.live.reference names no register");
        }
        if (!namedRegister(live["state"], &check.state)) {
          return fail(error, errorCap, "scaleCheck.live.state names no register");
        }
        if (check.meter != nullptr && (check.meter->scale != check.liveScale || check.meter->words != 1)) {
          return fail(error, errorCap,
                      "scaleCheck.live.meter must be a one-word register that uses scaleCheck.live.scale");
        }
        if (check.battery != nullptr && (check.battery->scale != check.liveScale || check.battery->words != 1)) {
          return fail(error, errorCap,
                      "scaleCheck.live.battery must be a one-word register that uses scaleCheck.live.scale");
        }
        // The regulation test compares the battery with the target. If both were on the scale
        // under test they would agree at any numerator, and the test would prove nothing.
        if (check.reference != nullptr && (check.reference->scale == nullptr ||
                                           check.reference->scale == check.liveScale || check.reference->words != 1)) {
          return fail(error, errorCap, "scaleCheck.live.reference must be a one-word register with a scale of its own");
        }
        if (check.state != nullptr && check.state->words != 1) {
          return fail(error, errorCap, "scaleCheck.live.state must be a one-word register");
        }
        for (JsonVariantConst value : live["regulating"].as<JsonArrayConst>()) {
          if (check.regulatingCount < sizeof(check.regulating) / sizeof(check.regulating[0]) &&
              value.is<uint16_t>()) {
            check.regulating[check.regulatingCount++] = value.as<uint16_t>();
          }
        }
      }
      check.present = check.settingsScale != nullptr || check.liveScale != nullptr;
    }
    return true;
  }

  // Looks up the register that a profile entry names. A missing entry is fine and leaves *out
  // at nullptr; a name that matches no register is a mistake.
  bool namedRegister(JsonVariantConst value, const OvRegister **out) const {
    *out = nullptr;
    if (value.isNull()) {
      return true;
    }
    *out = findRegister(value.as<const char *>());
    return *out != nullptr;
  }
};

}  // namespace optaview

#endif  // OPTAVIEW_PROFILE_H
