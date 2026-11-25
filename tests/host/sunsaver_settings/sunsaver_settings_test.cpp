// Host tests for the charger check: TankAlarm-112025-Common/src/TankAlarm_SunSaverSettings.h
// (what a SunSaver MPPT has stored, and whether it suits the battery) and
// TankAlarm_SunSaverReport.h (the same as JSON). They run against the simulated SunSaver MPPT of
// the OptaView tests (../optaview/sim_sunsaver.h), through OptaView's Modbus master.
//
//   make test ARDUINOJSON_DIR=<ArduinoJson v7.4.3>/src
//
// The TankAlarm client and OptaView do not share code, so two tests here hold them together:
// the register table is compared with OptaView's SunSaver profile, entry by entry, and the scale
// test is run side by side with OptaView's on the same words.
//
// The code under test is compiled as C++14, the dialect of the Opta's compiler.

#include "test_support.h"  // ../optaview: the test macros, the simulator and the OptaView headers

#include "TankAlarm_SunSaverReport.h"
#include "TankAlarm_SunSaverSettings.h"

namespace ss = sunsaver;

// ============================================================================
// What the tests share
// ============================================================================

// The Reader of these tests: OptaView's Modbus master on the simulated controller, one register
// per request, as the client does it with ArduinoModbus.
struct BusReader : ss::Reader {
  ModbusMaster &master;
  uint8_t function;
  std::vector<uint16_t> asked;

  explicit BusReader(ModbusMaster &m, uint8_t fc = MB_FC_READ_INPUT) : master(m), function(fc) {}

  ss::IoStatus readRegister(uint16_t addr, uint16_t *raw) override {
    asked.push_back(addr);
    uint16_t word = 0;
    const MbResult result = master.readRegisters(1, function, addr, 1, &word);
    if (result.status == MB_OK) {
      *raw = word;
      return ss::SSIO_OK;
    }
    return result.status == MB_EXCEPTION ? ss::SSIO_REFUSED : ss::SSIO_NO_REPLY;
  }
};

// Layout 1 of the report: the registers in the order they are read and sent. A client and a
// server of different versions rely on it, so it is written out here a second time.
static const uint16_t kLayout1[] = {
    0xE000, 0xE001, 0xE002, 0xE003, 0xE004, 0xE005, 0xE006, 0xE007, 0xE008, 0xE009, 0xE00A,
    0xE00D, 0xE00E, 0xE00F, 0xE010, 0xE011, 0xE012, 0xE013, 0xE014, 0xE015, 0xE016, 0xE017,
    0xE01A, 0xE01B, 0xE01C, 0xE01D, 0xE01E, 0xE01F,
    0xE022, 0xE023, 0xE024, 0xE025, 0xE026, 0xE027,
    0xE030, 0xE031, 0xE032, 0xE033,
    0x0008, 0x000C, 0x000E, 0x0010, 0x0011, 0x0012, 0x0014, 0x001A, 0x001B, 0x001C, 0x0025};

// What the controller holds, taken straight from the simulator's memory.
static ss::Snapshot snapshotOf(const SimSunSaver &sim) {
  ss::Snapshot snapshot;
  ss::clearSnapshot(&snapshot);
  for (uint8_t i = 0; i < ss::ITEM_COUNT; ++i) {
    const std::map<uint16_t, uint16_t>::const_iterator it = sim.regs.find(ss::item(i).addr);
    if (it != sim.regs.end()) {
      snapshot.raw[i] = it->second;
      snapshot.status[i] = ss::SSIO_OK;
    } else {
      snapshot.status[i] = ss::SSIO_REFUSED;
    }
  }
  return snapshot;
}

static bool sameSnapshot(const ss::Snapshot &a, const ss::Snapshot &b) {
  for (uint8_t i = 0; i < ss::ITEM_COUNT; ++i) {
    if (a.raw[i] != b.raw[i] || a.status[i] != b.status[i]) {
      return false;
    }
  }
  return true;
}

static ss::Evaluation judge(const SimSunSaver &sim, uint8_t chemistry, uint8_t nominalVolts = 12,
                            uint8_t firmwareScale = ss::SCALE_UNKNOWN) {
  const ss::Expectation want = {chemistry, nominalVolts, firmwareScale};
  ss::Evaluation result;
  ss::evaluate(snapshotOf(sim), want, &result);
  return result;
}

// A controller that holds the LiFePO4 settings this project recommends in both banks, with the
// DIP switches set to use them.
static void makeLiFePO4(SimSunSaver &sim) {
  const uint16_t banks[2] = {0xE000, 0xE00D};
  for (int b = 0; b < 2; ++b) {
    sim.regs[banks[b]] = sim.setVolts(14.20);                  // EV_reg
    sim.regs[(uint16_t)(banks[b] + 1)] = sim.setVolts(13.50);  // EV_float
    sim.regs[(uint16_t)(banks[b] + 2)] = 900;                  // Et_float
    sim.regs[(uint16_t)(banks[b] + 7)] = 0;                    // EV_eq
  }
  sim.regs[0xE01A] = 0;                    // EV_tempcomp
  sim.regs[0xE01B] = sim.setVolts(14.45);  // EV_hvd
  sim.regs[0xE01C] = sim.setVolts(13.60);  // EV_hvr
  sim.regs[0xE022] = sim.setVolts(13.10);  // EV_lvd
  sim.regs[0xE023] = sim.setVolts(13.25);  // EV_lvr
  sim.regs[0x001C] = sim.volts(13.10);     // V_lvd
  sim.regs[0x0025] = 0x000B;               // SW1, SW2 and SW4 ON
}

static std::string textOf(uint8_t index, uint16_t raw, uint8_t settingsScale = ss::SCALE_100,
                          uint8_t liveScale = ss::SCALE_100) {
  char buffer[160];
  ss::Text text(buffer, sizeof(buffer));
  ss::describeValue(index, raw, settingsScale, liveScale, &text);
  CHECK(!text.truncated());
  return std::string(buffer);
}

static bool plainAscii(const char *text) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  for (const char *p = text; *p != '\0'; ++p) {
    if (*p < 0x20 || *p > 0x7E) {
      return false;
    }
  }
  return true;
}

static bool startsWith(const char *text, const char *start) { return strncmp(text, start, strlen(start)) == 0; }

// ============================================================================
// The register table against OptaView's profile
// ============================================================================
static void testTableAgainstProfile() {
  OvProfile profile;
  char error[200] = "";
  const bool loaded = profile.load(OV_BUILTIN_PROFILE, strlen(OV_BUILTIN_PROFILE), error, sizeof(error));
  CHECK_MSG(loaded, "the SunSaver profile did not load: %s", error);
  if (!loaded) {
    return;
  }

  // The layout of the report.
  CHECK_EQ(ss::REPORT_LAYOUT, 1);
  CHECK_EQ(ss::ITEM_COUNT, sizeof(kLayout1) / sizeof(kLayout1[0]));
  CHECK_EQ(ss::ITEM_COUNT, 49);
  CHECK_EQ(ss::SETTING_ITEMS, 38);
  for (uint8_t i = 0; i < ss::ITEM_COUNT; ++i) {
    CHECK_MSG(ss::item(i).addr == kLayout1[i], "item %d is 0x%04X, layout 1 has 0x%04X there", (int)i,
              (unsigned)ss::item(i).addr, (unsigned)kLayout1[i]);
    CHECK_EQ(i < ss::SETTING_ITEMS, ss::item(i).addr >= 0xE000);
  }
  CHECK_EQ(ss::item(ss::I_EV_REG).addr, 0xE000);
  CHECK_EQ(ss::item(ss::I_EV_REG2).addr, 0xE00D);
  CHECK_EQ(ss::item(ss::I_EV_TEMPCOMP).addr, 0xE01A);
  CHECK_EQ(ss::item(ss::I_EV_LVD).addr, 0xE022);
  CHECK_EQ(ss::item(ss::I_EV_LVR).addr, 0xE023);
  CHECK_EQ(ss::item(ss::I_ER_ICOMP).addr, 0xE026);
  CHECK_EQ(ss::item(ss::I_EV_SOC_Y2G).addr, 0xE030);
  CHECK_EQ(ss::item(ss::I_ADC_VB_F).addr, 0x0008);
  CHECK_EQ(ss::item(ss::I_ADC_IL_F).addr, 0x000C);
  CHECK_EQ(ss::item(ss::I_ARRAY_FAULT).addr, 0x0012);
  CHECK_EQ(ss::item(ss::I_LOAD_FAULT).addr, 0x001B);
  CHECK_EQ(ss::item(ss::I_V_LVD).addr, 0x001C);
  CHECK_EQ(ss::item(ss::I_DIP_SWITCH).addr, 0x0025);
  CHECK_EQ(ss::item(200).addr, 0xE000);  // an index past the end reads as the first item, not past the table

  // Every entry against the profile's entry for the same address.
  for (uint8_t i = 0; i < ss::ITEM_COUNT; ++i) {
    const ss::Item &entry = ss::item(i);
    const OvRegister *reg = profile.findRegisterAt(entry.addr);
    CHECK_MSG(reg != nullptr, "0x%04X is not in the profile", (unsigned)entry.addr);
    if (reg == nullptr) {
      continue;
    }
    CHECK_MSG(strcmp(reg->name, entry.name) == 0, "0x%04X is '%s' here and '%s' in the profile", (unsigned)entry.addr,
              entry.name, reg->name);
    CHECK_MSG(startsWith(reg->label, entry.label), "%s: label '%s', the profile has '%s'", entry.name, entry.label,
              reg->label);
    CHECK(plainAscii(entry.name));
    CHECK(plainAscii(entry.label));
    CHECK_EQ(reg->words, 1);

    const char *scale = reg->scale != nullptr ? reg->scale->name : "";
    const char *units = reg->units != nullptr ? reg->units : "";
    const char *labels = reg->labels != nullptr ? reg->labels->name : "";
    const char *wantScale = "";
    const char *wantUnits = nullptr;
    const char *wantLabels = "";
    uint8_t wantType = OV_U16;
    switch (entry.kind) {
      case ss::KIND_VOLT_SETTING: wantScale = "vset"; break;
      case ss::KIND_VOLT_LIVE: wantScale = "vlive"; break;
      case ss::KIND_VOLT_TARGET: wantScale = "vref"; break;
      case ss::KIND_AMPS: wantScale = "i79"; wantType = OV_I16; break;
      case ss::KIND_SECONDS: wantUnits = "s"; break;
      case ss::KIND_TENTHS: wantScale = "tenths"; break;
      case ss::KIND_DAYS: wantUnits = "days"; break;
      case ss::KIND_TEMPCO: wantScale = "tempco"; wantType = OV_U8; break;  // the low byte only
      case ss::KIND_OHMS: wantScale = "icomp"; break;
      case ss::KIND_CELSIUS: wantUnits = "C"; wantType = OV_I8; break;
      case ss::KIND_CHARGE_STATE: wantLabels = "charge_state"; wantType = OV_ENUM; break;
      case ss::KIND_LOAD_STATE: wantLabels = "load_state"; wantType = OV_ENUM; break;
      case ss::KIND_ARRAY_FAULT: wantLabels = "array_fault"; wantType = OV_BITS; break;
      case ss::KIND_LOAD_FAULT: wantLabels = "load_fault"; wantType = OV_BITS; break;
      case ss::KIND_DIP: wantLabels = "dip_switch"; wantType = OV_BITS; break;
      default: CHECK_MSG(false, "%s has a kind this test does not know", entry.name); break;
    }
    CHECK_MSG(strcmp(scale, wantScale) == 0, "%s: scale '%s' in the profile, '%s' by its kind", entry.name, scale,
              wantScale);
    CHECK_MSG(strcmp(labels, wantLabels) == 0, "%s: labels '%s' in the profile", entry.name, labels);
    CHECK_MSG(reg->type == wantType, "%s: type %d in the profile, %d by its kind", entry.name, (int)reg->type,
              (int)wantType);
    if (wantUnits != nullptr) {
      CHECK_MSG(strcmp(units, wantUnits) == 0, "%s: units '%s' in the profile", entry.name, units);
    }

    // A voltage setting has the profile's limits, and 0 switches off what it switches off there.
    if (entry.kind == ss::KIND_VOLT_SETTING) {
      CHECK_MSG(reg->hasMin && reg->hasMax && near(reg->minValue, entry.loVolts, 1e-9) &&
                    near(reg->maxValue, entry.hiVolts, 1e-9),
                "%s: limits %d to %d here, %.2f to %.2f in the profile", entry.name, (int)entry.loVolts,
                (int)entry.hiVolts, reg->minValue, reg->maxValue);
      CHECK_MSG(reg->zeroOk == ((entry.flags & ss::ITEM_ZERO_IS_OFF) != 0), "%s: zero-is-off differs", entry.name);
    } else {
      CHECK_EQ(entry.loVolts, 0);
      CHECK_EQ(entry.hiVolts, 0);
      CHECK_EQ(entry.flags & ss::ITEM_WITNESS, 0);
    }

    // The groups.
    const char *wantGroup = nullptr;
    switch (entry.group) {
      case ss::GROUP_BANK1: wantGroup = "charge1"; break;
      case ss::GROUP_BANK2: wantGroup = "charge2"; break;
      case ss::GROUP_CHARGE: wantGroup = "charge"; break;
      case ss::GROUP_LOAD: wantGroup = "load"; break;
      case ss::GROUP_LED: wantGroup = "led"; break;
      default: break;
    }
    if (wantGroup != nullptr) {
      CHECK_MSG(strcmp(reg->group, wantGroup) == 0, "%s: group '%s' in the profile", entry.name, reg->group);
      const OvGroup *group = profile.findGroup(wantGroup);
      CHECK(group != nullptr && startsWith(group->title, ss::groupTitle(entry.group)));
    } else {
      CHECK_EQ(entry.group, ss::GROUP_STATUS);
      CHECK(strcmp(reg->group, "live") == 0 || strcmp(reg->group, "status") == 0);
    }
  }
  for (uint8_t group = 0; group < ss::GROUP_COUNT; ++group) {
    CHECK(plainAscii(ss::groupTitle(group)));
    CHECK(strcmp(ss::groupTitle(group), "?") != 0);
  }
  CHECK(strcmp(ss::groupTitle(ss::GROUP_COUNT), "?") == 0);

  // The scales and the scale test's set-up.
  const OvScaleCheck &check = profile.scaleCheck();
  CHECK(check.present && check.settingsScale != nullptr && check.liveScale != nullptr);
  if (check.settingsScale != nullptr && check.liveScale != nullptr) {
    CHECK(near(check.settingsScale->num, ss::scaleNumerator(ss::SCALE_100), 1e-9));
    CHECK(near(check.liveScale->num, ss::scaleNumerator(ss::SCALE_100), 1e-9));
    CHECK(near(check.settingsScale->den, ss::VOLTS_DENOMINATOR, 1e-9));
    CHECK(near(check.liveScale->den, ss::VOLTS_DENOMINATOR, 1e-9));
  }
  CHECK_EQ(check.alternateCount, 1);
  CHECK(near(check.alternates[0], ss::scaleNumerator(ss::SCALE_96667), 1e-9));
  CHECK(near(check.grid, ss::SETPOINT_GRID_VOLTS, 1e-12));
  CHECK_EQ(ss::SCALE_CANDIDATES, 2);
  CHECK_EQ(ss::candidateScale(0), ss::SCALE_100);  // the profile's own figure comes first, as in OptaView
  CHECK_EQ(ss::candidateScale(1), ss::SCALE_96667);
  CHECK(near(ss::scaleNumerator(ss::SCALE_UNKNOWN), 0.0, 1e-12));
  const OvRegister *target = profile.findRegisterAt(0x0014);
  CHECK(target != nullptr && target->scale != nullptr &&
        near(target->scale->num, ss::scaleNumerator(ss::SCALE_96667), 1e-9));

  // The setpoints the scale test looks at are OptaView's, in the same order.
  uint8_t witnesses = 0;
  for (uint8_t i = 0; i < ss::ITEM_COUNT; ++i) {
    if ((ss::item(i).flags & ss::ITEM_WITNESS) == 0) {
      continue;
    }
    CHECK_MSG(witnesses < check.setpointCount && check.setpoints[witnesses]->addr == ss::item(i).addr,
              "%s is setpoint %d of the scale test here, but not in the profile", ss::item(i).name, (int)witnesses);
    ++witnesses;
  }
  CHECK_EQ(witnesses, check.setpointCount);
  CHECK_EQ(witnesses, 14);

  // State names, fault bits and DIP switches.
  const struct {
    uint16_t addr;
    const char *(*byValue)(uint16_t);
    const char *(*byBit)(uint8_t);
  } kNamed[] = {{0x0011, ss::chargeStateName, nullptr},
                {0x001A, ss::loadStateName, nullptr},
                {0x0012, nullptr, ss::arrayFaultName},
                {0x001B, nullptr, ss::loadFaultName}};
  for (size_t n = 0; n < sizeof(kNamed) / sizeof(kNamed[0]); ++n) {
    const OvRegister *reg = profile.findRegisterAt(kNamed[n].addr);
    CHECK(reg != nullptr && reg->labels != nullptr);
    if (reg == nullptr || reg->labels == nullptr) {
      continue;
    }
    for (uint8_t value = 0; value <= reg->labels->count; ++value) {
      const char *mine = kNamed[n].byValue != nullptr ? kNamed[n].byValue(value) : kNamed[n].byBit(value);
      const char *theirs = value < reg->labels->count ? reg->labels->items[value] : nullptr;
      CHECK_MSG((mine == nullptr) == (theirs == nullptr) && (mine == nullptr || strcmp(mine, theirs) == 0),
                "0x%04X, value or bit %d: '%s' here, '%s' in the profile", (unsigned)kNamed[n].addr, (int)value,
                mine != nullptr ? mine : "(none)", theirs != nullptr ? theirs : "(none)");
    }
  }
  CHECK(ss::chargeStateName(65535) == nullptr);
  CHECK(ss::loadStateName(65535) == nullptr);
  CHECK(ss::arrayFaultName(255) == nullptr);
  CHECK(ss::loadFaultName(255) == nullptr);
  CHECK(has(ss::arrayFaultName(5), "EEPROM setting edit"));
  CHECK_EQ(ss::ARRAY_FAULT_EEPROM_EDIT, 1u << 5);
  CHECK(has(ss::loadFaultName(6), "EEPROM setting edit"));
  CHECK_EQ(ss::LOAD_FAULT_EEPROM_EDIT, 1u << 6);

  const OvRegister *dip = profile.findRegisterAt(0x0025);
  CHECK(dip != nullptr && dip->labels != nullptr && dip->labels->count == 4);
  if (dip != nullptr && dip->labels != nullptr && dip->labels->count == 4) {
    CHECK(has(dip->labels->items[0], "SW1") && has(dip->labels->items[0], "custom battery"));
    CHECK(has(dip->labels->items[1], "SW2") && has(dip->labels->items[1], "custom LVD"));
    CHECK(has(dip->labels->items[2], "SW3") && has(dip->labels->items[2], "equalize"));
    CHECK(has(dip->labels->items[3], "SW4") && has(dip->labels->items[3], "MODBUS"));
  }
  CHECK_EQ(ss::DIP_CUSTOM_CHARGE, 1u << 0);
  CHECK_EQ(ss::DIP_CUSTOM_LVD, 1u << 1);
  CHECK_EQ(ss::DIP_EQUALIZE, 1u << 2);
  CHECK_EQ(ss::DIP_MODBUS, 1u << 3);

  const OvRegister *rts = profile.findRegisterAt(0x0010);
  CHECK(rts != nullptr && rts->hasSentinel && rts->sentinelRaw == ss::RTS_NOT_CONNECTED);
}

// ============================================================================
// Values as text, against OptaView's decoding of the same registers
// ============================================================================
static void testValuesAgainstOptaView() {
  OvProfile profile;
  char error[200] = "";
  if (!profile.load(OV_BUILTIN_PROFILE, strlen(OV_BUILTIN_PROFILE), error, sizeof(error))) {
    CHECK_MSG(false, "the SunSaver profile did not load: %s", error);
    return;
  }
  // Every 37th word and the corners. With both scales at the profile's x100 the two decoders
  // have to print the same number for every register.
  std::vector<uint16_t> words;
  for (uint32_t raw = 0; raw <= 0xFFFF; raw += 37) {
    words.push_back((uint16_t)raw);
  }
  const uint16_t corners[] = {1, 2, 59, 60, 61, 127, 128, 129, 255, 256, 0x7FFF, 0x8000, 0x8001, 0xFF80, 0xFFFF};
  words.insert(words.end(), corners, corners + sizeof(corners) / sizeof(corners[0]));

  long differences = 0;
  std::string firstDifference;
  for (uint8_t i = 0; i < ss::ITEM_COUNT; ++i) {
    const ss::Item &entry = ss::item(i);
    const OvRegister *reg = profile.findRegisterAt(entry.addr);
    if (reg == nullptr) {
      continue;  // testTableAgainstProfile reports it
    }
    for (size_t w = 0; w < words.size(); ++w) {
      const uint16_t raw = words[w];
      char mine[400];  // a word with every fault bit set makes a long line
      ss::Text text(mine, sizeof(mine));
      ss::describeValue(i, raw, ss::SCALE_100, ss::SCALE_100, &text);
      char theirs[400];
      // The controller uses only the low byte of EV_tempcomp, and both read only that: OptaView
      // gets the whole word too.
      const uint16_t pair[2] = {raw, 0};
      ovFormatValue(*reg, pair, theirs, sizeof(theirs));
      const uint16_t used = entry.kind == ss::KIND_TEMPCO ? (uint16_t)(raw & 0x00FFu) : raw;

      std::string expected = theirs;
      if ((entry.flags & ss::ITEM_ZERO_IS_OFF) != 0 && used == 0) {
        expected = "0 (off)";
      } else if (entry.kind == ss::KIND_SECONDS && raw >= 60) {
        expected = std::string(mine).substr(0, expected.size()) == expected ? std::string(mine) : expected;
        if (!has(mine, " min)")) {
          expected = "(minutes are missing)";
        }
      } else if (entry.kind == ss::KIND_DAYS && raw == 1) {
        expected = "1 day";
      } else if (entry.kind == ss::KIND_CHARGE_STATE || entry.kind == ss::KIND_LOAD_STATE) {
        // OptaView adds the number: "FLOAT (7)", "unknown (9)".
        const std::string number = fmtStr(" (%u)", (unsigned)raw);
        if (expected == "unknown" + number) {
          expected = fmtStr("state %u", (unsigned)raw);
        } else if (expected.size() > number.size()) {
          expected = expected.substr(0, expected.size() - number.size());
        }
      } else if (entry.kind == ss::KIND_DIP) {
        expected.clear();
        for (unsigned sw = 0; sw < 4; ++sw) {
          expected += fmtStr("%sSW%u %s", sw == 0 ? "" : ", ", sw + 1, (raw & (1u << sw)) != 0 ? "ON" : "OFF");
        }
        // And OptaView names exactly the switches that are ON.
        for (unsigned sw = 0; sw < 4; ++sw) {
          const std::string on = fmtStr("SW%u", sw + 1);
          if (has(theirs, on.c_str()) != ((raw & (1u << sw)) != 0)) {
            expected = "(OptaView reads the switches differently)";
          }
        }
      }
      if (expected != mine || text.truncated()) {
        if (differences++ == 0) {
          firstDifference = fmtStr("%s = 0x%04X: '%s' here, '%s' expected (OptaView prints '%s')", entry.name,
                                   (unsigned)raw, mine, expected.c_str(), theirs);
        }
      }
    }
  }
  CHECK_MSG(differences == 0, "%ld values differ, the first: %s", differences, firstDifference.c_str());

  // Some of them written out.
  CHECK(textOf(ss::I_EV_REG, 4588) == "14.00 V");
  CHECK(textOf(ss::I_EV_REG, 4588, ss::SCALE_96667) == "13.53 V");
  CHECK(textOf(ss::I_EV_REG, 4588, ss::SCALE_UNKNOWN) == "14.00 V if x100, 13.53 V if x96.667");
  CHECK(textOf(ss::I_EV_EQ, 0) == "0 (off)");
  CHECK(textOf(ss::I_EV_EQ, 0, ss::SCALE_UNKNOWN) == "0 (off)");
  CHECK(textOf(ss::I_EV_LVD, 0) == "0.00 V");  // the LVD has no "off"
  // Nor have the high voltage reconnects: the battery never falls to 0 V, so 0 there would keep
  // charging, or the load, off after a high voltage disconnect. 0 in the disconnect is "off".
  CHECK(textOf(ss::I_EV_HVR, 0) == "0.00 V");
  CHECK(textOf(ss::I_EV_LHVR, 0) == "0.00 V");
  CHECK(textOf(ss::I_EV_HVD, 0) == "0 (off)");
  CHECK(textOf(ss::I_EV_LHVD, 0) == "0 (off)");
  CHECK(textOf(ss::I_ET_FLOAT, 900) == "900 s (15 min)");
  CHECK(textOf(ss::I_ET_FLOAT, 90) == "90 s (1.5 min)");
  CHECK(textOf(ss::I_ET_FLOAT, 59) == "59 s");
  CHECK(textOf(ss::I_ET_FLOAT, 0) == "0 s");
  CHECK(textOf(ss::I_ET_EQCALENDAR, 28) == "28 days");
  CHECK(textOf(ss::I_ET_EQCALENDAR, 1) == "1 day");
  CHECK(textOf(ss::I_ET_EQCALENDAR, 0) == "0 days");
  CHECK(textOf(ss::I_EV_TEMPCOMP, 20) == "0.0305 V/C");
  CHECK(textOf(ss::I_EV_TEMPCOMP, 0) == "0 (off)");
  // Only the low byte is used.
  CHECK(textOf(ss::I_EV_TEMPCOMP, 0x0114) == "0.0305 V/C");
  CHECK(textOf(ss::I_EV_TEMPCOMP, 0x0100) == "0 (off)");
  CHECK(textOf(ss::I_EV_TEMPCOMP, 0xFF00) == "0 (off)");
  CHECK(textOf(ss::I_ER_ICOMP, 778) == "0.0150 ohm");
  CHECK(textOf(ss::I_ER_ICOMP, 0) == "0 (off)");
  CHECK(textOf(ss::I_ET_LVD_WARN, 600) == "60.0 s");
  CHECK(textOf(ss::I_ETB_MIN, 0x00E2) == "-30 C");
  CHECK(textOf(ss::I_T_BATT, 25) == "25 C");
  CHECK(textOf(ss::I_T_RTS, 0x0080) == "not connected");
  CHECK(textOf(ss::I_T_BATT, 0x0080) == "-128 C");  // only T_rts has that meaning
  CHECK(textOf(ss::I_ADC_IL_F, SimSunSaver::amps(0.14)) == "0.14 A");
  CHECK(textOf(ss::I_ADC_IL_F, 0xFFF0) == "-0.04 A");
  CHECK(textOf(ss::I_ADC_VB_F, 4483) == "13.68 V");
  CHECK(textOf(ss::I_ADC_VB_F, 4483, ss::SCALE_100, ss::SCALE_96667) == "13.23 V");
  CHECK(textOf(ss::I_ADC_VB_F, 4483, ss::SCALE_100, ss::SCALE_UNKNOWN) == "13.68 V if x100, 13.23 V if x96.667");
  CHECK(textOf(ss::I_V_LVD, 3898, ss::SCALE_UNKNOWN, ss::SCALE_96667) == "11.50 V");
  // Vb_ref has its own scale, whatever the two verdicts.
  CHECK(textOf(ss::I_VB_REF, 4644, ss::SCALE_UNKNOWN, ss::SCALE_UNKNOWN) == "13.70 V");
  CHECK(textOf(ss::I_CHARGE_STATE, 7) == "FLOAT");
  CHECK(textOf(ss::I_CHARGE_STATE, 99) == "state 99");
  CHECK(textOf(ss::I_LOAD_STATE, 1) == "LOAD_ON");
  CHECK(textOf(ss::I_ARRAY_FAULT, 0) == "none");
  CHECK(textOf(ss::I_ARRAY_FAULT, 0x0021) == "Overcurrent, EEPROM setting edit (reset required)");
  CHECK(textOf(ss::I_ARRAY_FAULT, 0x8000) == "bit 15");
  CHECK(textOf(ss::I_LOAD_FAULT, 0x0040) == "EEPROM setting edit (reset required)");
  CHECK(textOf(ss::I_DIP_SWITCH, 0x000A) == "SW1 OFF, SW2 ON, SW3 OFF, SW4 ON");
  CHECK(textOf(ss::I_DIP_SWITCH, 0xFFF5) == "SW1 ON, SW2 OFF, SW3 ON, SW4 OFF");

  // A buffer that is too small gets what fits, and says so.
  char small[8];
  ss::Text text(small, sizeof(small));
  ss::describeValue(ss::I_DIP_SWITCH, 0x000A, ss::SCALE_100, ss::SCALE_100, &text);
  CHECK(strcmp(small, "SW1 OFF") == 0);
  CHECK(text.truncated());
}

// ============================================================================
// Text
// ============================================================================
static void testText() {
  {
    char buffer[8];
    ss::Text text(buffer, sizeof(buffer));
    CHECK_EQ(buffer[0], '\0');
    CHECK_EQ(text.length(), 0);
    text.add("abc").addUnsigned(42);
    CHECK(strcmp(buffer, "abc42") == 0);
    CHECK_EQ(text.length(), 5);
    CHECK(!text.truncated());
    text.add("xy");  // seven characters and the terminator fill the buffer exactly
    CHECK(strcmp(buffer, "abc42xy") == 0);
    CHECK(!text.truncated());
    text.add("z");
    CHECK(strcmp(buffer, "abc42xy") == 0);
    CHECK(text.truncated());
    CHECK_EQ(text.length(), 7);
    text.add(nullptr);
    CHECK_EQ(text.length(), 7);
  }
  {
    // A buffer of one character holds the terminator only, and one of none is never written to.
    char guard[4] = {'#', '#', '#', '#'};
    ss::Text one(guard + 1, 1);
    one.add("x").addUnsigned(7).addHex16(0xABCD).addFixed(1.5, 1).addSigned(-3);
    CHECK(one.truncated());
    CHECK_EQ(one.length(), 0);
    CHECK(guard[0] == '#' && guard[1] == '\0' && guard[2] == '#' && guard[3] == '#');
    char untouched[2] = {'#', '#'};
    ss::Text none(untouched, 0);
    none.add("x").addUnsigned(7);
    CHECK(none.truncated());
    CHECK(untouched[0] == '#' && untouched[1] == '#');
  }
  {
    char buffer[64];
    ss::Text text(buffer, sizeof(buffer));
    text.addUnsigned(0).add(" ").addUnsigned(4294967295ul).add(" ").addSigned(0).add(" ");
    text.addSigned(-2147483647 - 1).add(" ").addSigned(2147483647);
    CHECK(strcmp(buffer, "0 4294967295 0 -2147483648 2147483647") == 0);
  }
  {
    char buffer[96];
    ss::Text text(buffer, sizeof(buffer));
    text.addFixed(14.0, 2).add(" ").addFixed(0.125, 2).add(" ").addFixed(-0.004, 2).add(" ");
    text.addFixed(-1.5, 0).add(" ").addFixed(2.5, 0).add(" ").addFixed(1.0, 9).add(" ").addFixed(-0.05, 1);
    CHECK_MSG(strcmp(buffer, "14.00 0.13 0.00 -2 3 1.000000 -0.1") == 0, "got '%s'", buffer);
  }
  {
    char buffer[64];
    ss::Text text(buffer, sizeof(buffer));
    text.addFixed(std::numeric_limits<double>::quiet_NaN(), 2).add(" ");
    text.addFixed(std::numeric_limits<double>::infinity(), 2).add(" ").addFixed(-5.0e9, 2).add(" ");
    text.addFixed(4.0e9, 0).add(" ").addHex16(0x00AF).add(" ").addHex16(0xFFFF);
    CHECK_MSG(strcmp(buffer, "? ? ? 4000000000 00AF FFFF") == 0, "got '%s'", buffer);
  }

  // The names.
  CHECK(strcmp(ss::scaleName(ss::SCALE_100), "x100") == 0);
  CHECK(strcmp(ss::scaleName(ss::SCALE_96667), "x96.667") == 0);
  CHECK(strcmp(ss::scaleName(ss::SCALE_UNKNOWN), "not proven") == 0);
  CHECK(strcmp(ss::scaleName(77), "not proven") == 0);
  CHECK(strcmp(ss::verdictName(ss::VERDICT_OK), "ok") == 0);
  CHECK(strcmp(ss::verdictName(ss::VERDICT_MISMATCH), "mismatch") == 0);
  CHECK(strcmp(ss::verdictName(ss::VERDICT_CANNOT_TELL), "cannot tell") == 0);
  CHECK(strcmp(ss::verdictName(ss::VERDICT_NOT_JUDGED), "not judged") == 0);
  CHECK(strcmp(ss::verdictName(200), "not judged") == 0);
  CHECK(strcmp(ss::chemistryName(ss::CHEMISTRY_LIFEPO4), "LiFePO4") == 0);
  CHECK(strcmp(ss::chemistryName(ss::CHEMISTRY_LI_ION), "Li-ion") == 0);
  CHECK(strcmp(ss::chemistryName(ss::CHEMISTRY_LEAD_SEALED), "sealed lead-acid") == 0);
  CHECK(strcmp(ss::chemistryName(ss::CHEMISTRY_LEAD_FLOODED), "flooded lead-acid") == 0);
  CHECK(strcmp(ss::chemistryName(ss::CHEMISTRY_NOT_JUDGED), "not judged") == 0);
  CHECK(ss::chemistryIsLithium(ss::CHEMISTRY_LIFEPO4) && ss::chemistryIsLithium(ss::CHEMISTRY_LI_ION));
  CHECK(!ss::chemistryIsLithium(ss::CHEMISTRY_LEAD_SEALED) && !ss::chemistryIsLithium(ss::CHEMISTRY_LEAD_FLOODED) &&
        !ss::chemistryIsLithium(ss::CHEMISTRY_NOT_JUDGED));
  // The battery types of the client config, as the client judges them and the server compares
  // them with a kept report. The numbers are those the config carries (batteryConfig.batteryType).
  {
    const int types[] = {BATTERY_TYPE_NONE, BATTERY_TYPE_AGM,     BATTERY_TYPE_FLOODED, BATTERY_TYPE_GEL,
                         BATTERY_TYPE_SLA,  BATTERY_TYPE_LIFEPO4, BATTERY_TYPE_LI_ION,  BATTERY_TYPE_LIPO,
                         BATTERY_TYPE_CUSTOM, 9, 255, -1};
    const int numbers[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 255, -1};
    const uint8_t want[] = {ss::CHEMISTRY_NOT_JUDGED, ss::CHEMISTRY_LEAD_SEALED, ss::CHEMISTRY_LEAD_FLOODED,
                            ss::CHEMISTRY_LEAD_SEALED, ss::CHEMISTRY_LEAD_SEALED, ss::CHEMISTRY_LIFEPO4,
                            ss::CHEMISTRY_LI_ION,      ss::CHEMISTRY_NOT_JUDGED,  ss::CHEMISTRY_NOT_JUDGED,
                            ss::CHEMISTRY_NOT_JUDGED,  ss::CHEMISTRY_NOT_JUDGED,  ss::CHEMISTRY_NOT_JUDGED};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
      CHECK_EQ(types[i], numbers[i]);
      CHECK_MSG(ss::chemistryForBatteryType(types[i]) == want[i], "battery type %d", types[i]);
    }
  }

  // Every finding has a sentence of its own.
  CHECK_EQ(ss::FINDING_BITS, 15);
  const char *fallback = ss::findingText(0);
  for (uint8_t bit = 0; bit < 32; ++bit) {
    const char *sentence = ss::findingText((uint32_t)1 << bit);
    CHECK(plainAscii(sentence));
    CHECK_MSG((strcmp(sentence, fallback) == 0) == (bit >= ss::FINDING_BITS), "finding bit %d", (int)bit);
    for (uint8_t other = 0; other < bit && bit < ss::FINDING_BITS; ++other) {
      CHECK(strcmp(sentence, ss::findingText((uint32_t)1 << other)) != 0);
    }
  }
  CHECK(strcmp(ss::findingText(ss::FINDING_PRESET_IN_USE | ss::FINDING_TEMPCOMP), fallback) == 0);

  // The scale lines.
  char line[160];
  {
    const ss::ScaleVerdict verdict = {13, {13, 1}, ss::SCALE_100};
    ss::Text text(line, sizeof(line));
    ss::describeSettingsScale(verdict, &text);
    CHECK_MSG(strcmp(line, "x100 (13 of 13 setpoints are round with it, 1 with x96.667)") == 0, "got '%s'", line);
  }
  {
    const ss::ScaleVerdict verdict = {13, {2, 12}, ss::SCALE_96667};
    ss::Text text(line, sizeof(line));
    ss::describeSettingsScale(verdict, &text);
    CHECK_MSG(strcmp(line, "x96.667 (12 of 13 setpoints are round with it, 2 with x100)") == 0, "got '%s'", line);
  }
  {
    const ss::ScaleVerdict verdict = {6, {3, 2}, ss::SCALE_UNKNOWN};
    ss::Text text(line, sizeof(line));
    ss::describeSettingsScale(verdict, &text);
    CHECK_MSG(strcmp(line, "not proven (of 6 setpoints 3 are round with x100 and 2 with x96.667)") == 0, "got '%s'",
              line);
  }
  {
    ss::Text text(line, sizeof(line));
    ss::describeLiveScale(ss::SCALE_UNKNOWN, ss::LIVE_SOURCE_NONE, 12, &text);
    CHECK(strcmp(line, "not proven") == 0);
  }
  {
    ss::Text text(line, sizeof(line));
    ss::describeLiveScale(ss::SCALE_100, ss::LIVE_SOURCE_PRESET_LVD, 12, &text);
    CHECK(strcmp(line, "x100 (the built-in 11.50 V LVD reads right with it)") == 0);
  }
  {
    // The built-in LVD of a 24 V system is twice that.
    ss::Text text(line, sizeof(line));
    ss::describeLiveScale(ss::SCALE_100, ss::LIVE_SOURCE_PRESET_LVD, 24, &text);
    CHECK_MSG(strcmp(line, "x100 (the built-in 23.00 V LVD reads right with it)") == 0, "got '%s'", line);
  }
  {
    // A report from a client that did not send the system names both.
    ss::Text text(line, sizeof(line));
    ss::describeLiveScale(ss::SCALE_96667, ss::LIVE_SOURCE_PRESET_LVD, 0, &text);
    CHECK_MSG(strcmp(line, "x96.667 (the built-in LVD, 11.50 V or 23.00 V on a 24 V system, reads right with it)") == 0,
              "got '%s'", line);
  }
  for (uint8_t system : {0, 12, 24}) {
    ss::Text text(line, sizeof(line));
    ss::describeLiveScale(ss::SCALE_96667, ss::LIVE_SOURCE_STORED_LVD, system, &text);
    CHECK(strcmp(line, "x96.667 (the stored LVD reads right with it)") == 0);
  }
  // The finding names the built-in LVD of both systems: it has no system to go by.
  CHECK(strstr(ss::findingText(ss::FINDING_PRESET_LVD), "11.50 V (23.00 V on a 24 V system)") != nullptr);

  // The client logs each finding and each scale line as an entry of its own, with the words it
  // puts before it, and an entry of its log holds 160 bytes: none may be cut.
  for (uint8_t bit = 0; bit < ss::FINDING_BITS; ++bit) {
    ss::Text text(line, sizeof(line));
    text.add("  - ").add(ss::findingText((uint32_t)1 << bit));
    CHECK_MSG(!text.truncated(), "finding bit %d", (int)bit);
  }
  {
    const ss::ScaleVerdict verdict = {255, {255, 255}, ss::SCALE_UNKNOWN};  // the longest
    ss::Text text(line, sizeof(line));
    text.add("  Stored voltages: ");
    ss::describeSettingsScale(verdict, &text);
    CHECK(!text.truncated());
  }
  for (uint8_t scale : {(uint8_t)ss::SCALE_100, (uint8_t)ss::SCALE_96667}) {
    for (uint8_t source : {(uint8_t)ss::LIVE_SOURCE_PRESET_LVD, (uint8_t)ss::LIVE_SOURCE_STORED_LVD}) {
      for (uint8_t system : {0, 12, 24}) {
        ss::Text text(line, sizeof(line));
        text.add("  Live readings: ");
        ss::describeLiveScale(scale, source, system, &text);
        CHECK(!text.truncated());
      }
    }
  }
  // And the test can tell: a sentence that does not fit is cut, and says so.
  {
    char small[8];
    ss::Text text(small, sizeof(small));
    text.add("  - ").add(ss::findingText(ss::FINDING_CHARGER_FAULT));
    CHECK(text.truncated());
  }
}

// ============================================================================
// One pass over the registers
// ============================================================================
static void testReader() {
  {
    Bare b;
    BusReader bus(b.master);
    ss::SnapshotReader reader;
    CHECK(!reader.active() && !reader.done() && !reader.failed());
    CHECK(reader.step(bus, 4));  // no pass was started
    CHECK_EQ(bus.asked.size(), 0);
    CHECK(!ss::have(reader.snapshot(), ss::I_EV_REG));

    reader.start();
    CHECK(reader.active());
    CHECK(!reader.step(bus, 0));  // nothing is asked without a budget
    CHECK_EQ(bus.asked.size(), 0);
    CHECK(!reader.step(bus, 4));
    CHECK_EQ(bus.asked.size(), 4);
    CHECK_EQ(reader.progress(), 4);
    CHECK(reader.active() && !reader.done());
    unsigned steps = 1;
    while (!reader.step(bus, 4) && steps < 100) {
      ++steps;
    }
    ++steps;
    CHECK_EQ(steps, 13);  // 49 registers, four at a time
    CHECK(reader.done() && !reader.active() && !reader.failed() && !reader.missed());
    CHECK_EQ(reader.progress(), ss::ITEM_COUNT);

    // Every register once, in the order of the layout, each in a request of its own.
    CHECK_EQ(bus.asked.size(), ss::ITEM_COUNT);
    CHECK_EQ(b.sim.requests.size(), ss::ITEM_COUNT);
    for (size_t i = 0; i < bus.asked.size() && i < ss::ITEM_COUNT; ++i) {
      CHECK_EQ(bus.asked[i], kLayout1[i]);
      const std::vector<uint8_t> &frame = b.sim.requests[i];
      CHECK(frame.size() == 8 && frame[0] == 1 && frame[1] == MB_FC_READ_INPUT && frame[4] == 0 && frame[5] == 1);
    }
    CHECK(sameSnapshot(reader.snapshot(), snapshotOf(b.sim)));
    CHECK(ss::have(reader.snapshot(), ss::I_DIP_SWITCH));
    CHECK_EQ(reader.snapshot().raw[ss::I_DIP_SWITCH], 0x000A);
    CHECK(!ss::have(reader.snapshot(), ss::ITEM_COUNT));
    CHECK(!ss::have(reader.snapshot(), 255));

    // A finished pass stays finished, and asks nothing more.
    CHECK(reader.step(bus, 4));
    CHECK_EQ(bus.asked.size(), ss::ITEM_COUNT);

    // The next pass drops what the last one read.
    b.sim.regs[0xE000] = 1234;
    reader.start();
    CHECK(reader.active() && !reader.done());
    CHECK_EQ(reader.progress(), 0);
    CHECK(!ss::have(reader.snapshot(), ss::I_EV_REG));
    CHECK(reader.step(bus, 255));  // all of it in one step
    CHECK(reader.done());
    CHECK_EQ(reader.snapshot().raw[ss::I_EV_REG], 1234);
    CHECK_EQ(bus.asked.size(), 2u * ss::ITEM_COUNT);
  }
  {
    // One lost reply: the step ends there, the register is asked again, and the pass completes.
    Bare b;
    BusReader bus(b.master);
    ss::SnapshotReader reader;
    reader.start();
    CHECK(!reader.step(bus, 2));
    b.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
    CHECK(!reader.step(bus, 4));
    CHECK(reader.missed());
    CHECK(reader.active());
    CHECK_EQ(reader.progress(), 2);
    CHECK_EQ(bus.asked.size(), 3);  // the rest of that step's budget was not used
    CHECK(!reader.step(bus, 4));
    CHECK(!reader.missed());
    CHECK_EQ(bus.asked[2], 0xE002);
    CHECK_EQ(bus.asked[3], 0xE002);
    CHECK_EQ(reader.progress(), 6);
    CHECK(reader.step(bus, 255));
    CHECK(reader.done());
    CHECK(sameSnapshot(reader.snapshot(), snapshotOf(b.sim)));
    CHECK_EQ(bus.asked.size(), ss::ITEM_COUNT + 1u);
  }
  {
    // The same register stays silent twice: that is noted, and the pass carries on without it.
    Bare b;
    BusReader bus(b.master);
    ss::SnapshotReader reader;
    reader.start();
    CHECK(!reader.step(bus, 3));
    b.sim.failNext(SimSunSaver::FAULT_NO_REPLY, 2);
    CHECK(!reader.step(bus, 4));
    CHECK(reader.missed() && reader.active());
    CHECK_EQ(reader.progress(), 3);
    CHECK(!reader.step(bus, 4));
    CHECK(reader.missed() && reader.active());  // this step ended on the miss as well
    CHECK_EQ(reader.progress(), 4);
    CHECK_EQ(bus.asked.size(), 5);
    CHECK_EQ(reader.snapshot().status[3], ss::SSIO_NO_REPLY);
    CHECK(!reader.step(bus, 4));
    CHECK(!reader.missed());
    CHECK_EQ(bus.asked[4], kLayout1[3]);
    CHECK_EQ(bus.asked[5], kLayout1[4]);  // not a third time
    CHECK_EQ(reader.progress(), 8);
    CHECK(reader.step(bus, 255));
    CHECK(reader.done() && !reader.failed());
    ss::Snapshot expected = snapshotOf(b.sim);
    expected.raw[3] = 0;
    expected.status[3] = ss::SSIO_NO_REPLY;
    CHECK(sameSnapshot(reader.snapshot(), expected));
    CHECK(!ss::have(reader.snapshot(), 3));
    CHECK(ss::have(reader.snapshot(), 2) && ss::have(reader.snapshot(), 4));
    CHECK_EQ(bus.asked.size(), ss::ITEM_COUNT + 1u);
  }
  {
    // Two registers may stay silent. The next request without a reply is the fifth of the pass,
    // and that ends it: the link is not fit to judge a charger by.
    Bare b;
    BusReader bus(b.master);
    ss::SnapshotReader reader;
    reader.start();
    b.sim.failNext(SimSunSaver::FAULT_NO_REPLY, 4);
    for (int miss = 0; miss < 4; ++miss) {
      CHECK(!reader.step(bus, 4));
      CHECK(reader.missed());
    }
    CHECK(reader.active());
    CHECK_EQ(reader.progress(), 2);
    CHECK_EQ(bus.asked.size(), 4);
    CHECK(!reader.step(bus, 4));
    CHECK(!reader.missed());
    CHECK_EQ(reader.progress(), 6);
    b.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
    CHECK(reader.step(bus, 4));
    CHECK(reader.failed() && !reader.done() && !reader.active());
    CHECK(reader.missed());
    CHECK_EQ(reader.progress(), 6);
    CHECK_EQ(bus.asked.size(), 9);
    CHECK(reader.step(bus, 4));  // and nothing more is asked
    CHECK(!reader.missed());
    CHECK_EQ(bus.asked.size(), 9);
    reader.cancel();  // a failed pass stays failed
    CHECK(reader.failed());
    // A new pass starts clean.
    reader.start();
    CHECK(reader.active() && !reader.failed());
    CHECK(reader.step(bus, 255));
    CHECK(reader.done());
    CHECK(sameSnapshot(reader.snapshot(), snapshotOf(b.sim)));
  }
  {
    // A controller that is switched off fails the pass at the fifth request.
    Bare b;
    b.sim.powered = false;
    BusReader bus(b.master);
    ss::SnapshotReader reader;
    reader.start();
    for (int miss = 0; miss < 4; ++miss) {
      CHECK(!reader.step(bus, 4));
      CHECK(reader.missed());
      CHECK_EQ(bus.asked.size(), (size_t)miss + 1u);  // one request a step
    }
    CHECK(reader.step(bus, 4));
    CHECK(reader.failed());
    CHECK_EQ(bus.asked.size(), 5);
    CHECK_EQ(reader.progress(), 2);
  }
  {
    // The last register stays silent: the pass is complete without it.
    Bare b;
    BusReader bus(b.master);
    ss::SnapshotReader reader;
    reader.start();
    CHECK(!reader.step(bus, (uint8_t)(ss::ITEM_COUNT - 1)));
    b.sim.failNext(SimSunSaver::FAULT_NO_REPLY, 2);
    CHECK(!reader.step(bus, 4));
    CHECK(reader.active() && reader.missed());
    CHECK(reader.step(bus, 4));
    CHECK(reader.done() && !reader.failed() && reader.missed());
    CHECK_EQ(reader.snapshot().status[ss::I_DIP_SWITCH], ss::SSIO_NO_REPLY);
    CHECK(!ss::have(reader.snapshot(), ss::I_DIP_SWITCH));
    CHECK(ss::have(reader.snapshot(), ss::I_V_LVD));
    CHECK_EQ(bus.asked.size(), ss::ITEM_COUNT + 1u);
  }
  {
    // Whatever is wrong with a reply, it is a miss. Four scattered misses are put up with.
    const SimSunSaver::Fault kinds[] = {SimSunSaver::FAULT_LOST_REPLY, SimSunSaver::FAULT_BAD_CRC,
                                        SimSunSaver::FAULT_TRUNCATE, SimSunSaver::FAULT_WRONG_SLAVE,
                                        SimSunSaver::FAULT_WRONG_FUNCTION, SimSunSaver::FAULT_EXTRA_REGISTER,
                                        SimSunSaver::FAULT_GARBAGE};
    for (size_t first = 0; first + 4 <= sizeof(kinds) / sizeof(kinds[0]); first += 3) {
      Bare b;
      BusReader bus(b.master);
      ss::SnapshotReader reader;
      reader.start();
      for (size_t k = first; k < first + 4; ++k) {
        CHECK(!reader.step(bus, 5));
        b.sim.failNext(kinds[k]);
        CHECK_MSG(!reader.step(bus, 5) && reader.missed(), "fault kind %d", (int)kinds[k]);
      }
      CHECK(reader.step(bus, 255));
      CHECK(reader.done());
      CHECK(sameSnapshot(reader.snapshot(), snapshotOf(b.sim)));
      CHECK_EQ(bus.asked.size(), ss::ITEM_COUNT + 4u);
    }
    // The fifth miss ends the pass, although that register was asked only once.
    Bare b;
    BusReader bus(b.master);
    ss::SnapshotReader reader;
    reader.start();
    for (int miss = 0; miss < 4; ++miss) {
      b.sim.failNext(SimSunSaver::FAULT_BAD_CRC);
      CHECK(!reader.step(bus, 1));
      CHECK(!reader.step(bus, 1));
    }
    CHECK_EQ(reader.progress(), 4);
    b.sim.failNext(SimSunSaver::FAULT_BAD_CRC);
    CHECK(reader.step(bus, 1));
    CHECK(reader.failed());
    CHECK_EQ(bus.asked.size(), 9);
    // The next pass starts with a clean count: it puts up with four misses of its own.
    reader.start();
    for (int miss = 0; miss < 4; ++miss) {
      b.sim.failNext(SimSunSaver::FAULT_BAD_CRC);
      CHECK(!reader.step(bus, 1));
      CHECK(!reader.step(bus, 1));
    }
    CHECK(reader.active());
    CHECK(reader.step(bus, 255));
    CHECK(reader.done());
  }
  {
    // A register the controller does not have is refused. That is noted, and the pass carries on.
    Bare b;
    b.sim.regs.erase(0xE026);
    b.sim.regs.erase(0x0010);
    BusReader bus(b.master);
    ss::SnapshotReader reader;
    reader.start();
    CHECK(reader.step(bus, 255));
    CHECK(reader.done());
    CHECK_EQ(bus.asked.size(), ss::ITEM_COUNT);  // asked once, not again
    CHECK_EQ(reader.snapshot().status[ss::I_ER_ICOMP], ss::SSIO_REFUSED);
    CHECK_EQ(reader.snapshot().raw[ss::I_ER_ICOMP], 0);
    CHECK_EQ(reader.snapshot().status[ss::I_T_RTS], ss::SSIO_REFUSED);
    CHECK(!ss::have(reader.snapshot(), ss::I_ER_ICOMP));
    CHECK(ss::have(reader.snapshot(), ss::I_ET_LVD_WARN));
    CHECK(sameSnapshot(reader.snapshot(), snapshotOf(b.sim)));
  }
  {
    // Giving up a pass.
    Bare b;
    BusReader bus(b.master);
    ss::SnapshotReader reader;
    reader.cancel();  // nothing to give up
    CHECK(!reader.active() && !reader.done() && !reader.failed());
    reader.start();
    CHECK(!reader.step(bus, 10));
    reader.cancel();
    CHECK(!reader.active() && !reader.done() && !reader.failed());
    CHECK(reader.step(bus, 10));
    CHECK_EQ(bus.asked.size(), 10);
    reader.start();
    CHECK(reader.step(bus, 255));
    reader.cancel();  // a finished pass keeps its result
    CHECK(reader.done());
  }
  {
    // A pass that starts while a register was waiting for its second request owes the old pass
    // nothing: its own first register gets its two requests.
    Bare b;
    BusReader bus(b.master);
    ss::SnapshotReader reader;
    reader.start();
    b.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
    CHECK(!reader.step(bus, 4));
    CHECK(reader.missed());
    reader.start();
    CHECK(!reader.missed());
    b.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
    CHECK(!reader.step(bus, 4));
    CHECK(reader.missed());
    CHECK_EQ(reader.progress(), 0);  // asked once in this pass: once more to come
    CHECK(reader.step(bus, 255));
    CHECK(reader.done());
    CHECK(sameSnapshot(reader.snapshot(), snapshotOf(b.sim)));
    CHECK_EQ(bus.asked.size(), ss::ITEM_COUNT + 2u);
  }
  {
    // What a reader leaves in the word of a register that gave no value is not kept.
    struct Scribbler : ss::Reader {
      ss::IoStatus readRegister(uint16_t addr, uint16_t *raw) override {
        *raw = 0xBEEF;
        if (addr == 0xE026) {
          return ss::SSIO_REFUSED;
        }
        return addr == 0x0010 ? ss::SSIO_NO_REPLY : ss::SSIO_OK;
      }
    };
    Scribbler bus;
    ss::SnapshotReader reader;
    reader.start();
    for (int steps = 0; steps < 10 && !reader.step(bus, 255); ++steps) {
    }
    CHECK(reader.done());
    CHECK_EQ(reader.snapshot().status[ss::I_ER_ICOMP], ss::SSIO_REFUSED);
    CHECK_EQ(reader.snapshot().raw[ss::I_ER_ICOMP], 0);
    CHECK_EQ(reader.snapshot().status[ss::I_T_RTS], ss::SSIO_NO_REPLY);
    CHECK_EQ(reader.snapshot().raw[ss::I_T_RTS], 0);
    CHECK(ss::have(reader.snapshot(), ss::I_EV_REG));
    CHECK_EQ(reader.snapshot().raw[ss::I_EV_REG], 0xBEEF);
  }
  {
    // The function code is the caller's business: a controller that answers function 3 only.
    Bare b;
    b.sim.readInput = false;
    BusReader bus(b.master, MB_FC_READ_HOLDING);
    ss::SnapshotReader reader;
    reader.start();
    CHECK(reader.step(bus, 255));
    CHECK(reader.done());
    CHECK(sameSnapshot(reader.snapshot(), snapshotOf(b.sim)));
  }
}

// ============================================================================
// The scale of the stored settings
// ============================================================================
static void testSettingsScale() {
  {
    // Only a register that gave a value counts as a setpoint, whatever its word holds. (The
    // reader and the decoder clear such a word. A snapshot put together by hand may not.)
    SimSunSaver sim;
    ss::Snapshot snapshot = snapshotOf(sim);
    const ss::ScaleVerdict whole = ss::judgeSettingsScale(snapshot);
    CHECK(snapshot.raw[ss::I_EV_LVD] != 0);
    snapshot.status[ss::I_EV_LVD] = ss::SSIO_NO_REPLY;
    const ss::ScaleVerdict without = ss::judgeSettingsScale(snapshot);
    CHECK_EQ(without.used + 1, whole.used);
    CHECK_EQ(without.fits[0] + 1, whole.fits[0]);
  }
  // The rule on words alone, with the cases OptaView's own test uses.
  CHECK(near(ss::gridDistance(14.0015, 0.05), 0.0015, 1e-9));
  CHECK(near(ss::gridDistance(13.98, 0.05), 0.02, 1e-9));
  CHECK(near(ss::gridDistance(0.0, 0.05), 0.0, 1e-12));
  CHECK(near(ss::voltsFromRaw(4588, ss::SCALE_100), 14.0015, 1e-4));
  CHECK(near(ss::voltsFromRaw(4588, ss::SCALE_96667), 13.5348, 1e-4));
  CHECK(near(ss::voltsFromRaw(4588, ss::SCALE_UNKNOWN), 0.0, 1e-12));
  {
    const uint16_t three[] = {4588, 4489, 3768};
    ss::ScaleVerdict verdict = ss::judgeSetpointWords(three, 3);
    CHECK_EQ(verdict.used, 3);
    CHECK_EQ(verdict.fits[0], 3);
    CHECK_EQ(verdict.scale, ss::SCALE_UNKNOWN);  // three setpoints are too few to call it
    const uint16_t truncated[] = {4587, 4489, 3768, 4128, 4292};
    verdict = ss::judgeSetpointWords(truncated, 5);
    CHECK_EQ(verdict.fits[0], 5);
    CHECK_EQ(verdict.scale, ss::SCALE_100);
    const uint16_t odd[] = {4594, 4496, 3775, 4135, 4300, 4391};
    verdict = ss::judgeSetpointWords(odd, 6);
    CHECK_EQ(verdict.scale, ss::SCALE_UNKNOWN);
    const uint16_t sixOfTen[] = {4588, 4489, 3768, 4129, 4293, 4719, 4496, 3775, 4299, 4725};
    verdict = ss::judgeSetpointWords(sixOfTen, 10);
    CHECK_EQ(verdict.fits[0], 6);
    CHECK_EQ(verdict.fits[1], 0);
    CHECK_EQ(verdict.scale, ss::SCALE_100);
    const uint16_t fiveOfTen[] = {4588, 4489, 3768, 4129, 4293, 4037, 4496, 3775, 4299, 4725};
    verdict = ss::judgeSetpointWords(fiveOfTen, 10);
    CHECK_EQ(verdict.fits[0], 5);
    CHECK_EQ(verdict.scale, ss::SCALE_UNKNOWN);
    const uint16_t twoToOne[] = {4588, 4489, 3768, 3932, 4424, 4915};
    verdict = ss::judgeSetpointWords(twoToOne, 6);
    CHECK_EQ(verdict.fits[0], 6);
    CHECK_EQ(verdict.fits[1], 3);
    CHECK_EQ(verdict.scale, ss::SCALE_100);
    const uint16_t tooClose[] = {4588, 4489, 3441, 3932, 4424, 4915};
    verdict = ss::judgeSetpointWords(tooClose, 6);
    CHECK_EQ(verdict.fits[0], 6);
    CHECK_EQ(verdict.fits[1], 4);
    CHECK_EQ(verdict.scale, ss::SCALE_UNKNOWN);
    verdict = ss::judgeSetpointWords(odd, 0);
    CHECK_EQ(verdict.used, 0);
    CHECK_EQ(verdict.scale, ss::SCALE_UNKNOWN);
    // The same setpoints stored with the other scale.
    uint16_t other[6];
    const double volts[6] = {14.00, 13.70, 11.50, 12.60, 13.10, 14.40};
    for (int i = 0; i < 6; ++i) {
      other[i] = (uint16_t)(volts[i] * 32768.0 / 96.667 + 0.5);
    }
    verdict = ss::judgeSetpointWords(other, 6);
    CHECK_EQ(verdict.fits[1], 6);
    CHECK_EQ(verdict.scale, ss::SCALE_96667);
  }

  // The same words through this rule and through OptaView's: counts and verdict agree every time.
  {
    uint32_t seed = 20261123u;
    long disagreements = 0;
    long verdicts[3] = {0, 0, 0};
    for (int round = 0; round < 30000; ++round) {
      uint16_t raws[16];
      seed = seed * 1664525u + 1013904223u;
      const uint8_t count = (uint8_t)((seed >> 8) % 15u);
      seed = seed * 1664525u + 1013904223u;
      const unsigned mix = (seed >> 8) % 5u;
      for (uint8_t i = 0; i < count; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const unsigned pick = mix == 0 ? 0u : mix == 1 ? 1u : (seed >> 8) % 3u;
        seed = seed * 1664525u + 1013904223u;
        const double volts = 9.0 + 0.05 * (double)((seed >> 8) % 161u);
        if (pick == 0) {
          raws[i] = (uint16_t)(volts * 32768.0 / 100.0 + 0.5);
        } else if (pick == 1) {
          raws[i] = (uint16_t)(volts * 32768.0 / 96.667 + 0.5);
        } else {
          raws[i] = (uint16_t)(2900u + (seed >> 12) % 3000u);
        }
      }
      const ss::ScaleVerdict mine = ss::judgeSetpointWords(raws, count);
      OvSettingsScaleReport theirs;
      memset(&theirs, 0, sizeof(theirs));
      ovAddCandidate(&theirs.candidates, ss::scaleNumerator(ss::candidateScale(0)));
      ovAddCandidate(&theirs.candidates, ss::scaleNumerator(ss::candidateScale(1)));
      ovJudgeSetpoints(&theirs, raws, count, ss::VOLTS_DENOMINATOR, ss::SETPOINT_GRID_VOLTS);
      // OptaView's round-setpoint test. There it only points: its verdict on the stored scale needs
      // a setting whose value the user knows from MSView.
      const uint8_t expected =
          theirs.roundVerdict < 0 ? (uint8_t)ss::SCALE_UNKNOWN : ss::candidateScale((uint8_t)theirs.roundVerdict);
      if (mine.scale != expected || mine.used != theirs.setpointsUsed || mine.fits[0] != theirs.fits[0] ||
          mine.fits[1] != theirs.fits[1]) {
        ++disagreements;
      }
      if (mine.scale < 3) {
        ++verdicts[mine.scale];
      }
    }
    CHECK_EQ(disagreements, 0);
    // The rounds reach every verdict often enough to mean something.
    CHECK_MSG(verdicts[0] > 3000 && verdicts[1] > 3000 && verdicts[2] > 3000, "verdicts: %ld %ld %ld", verdicts[0],
              verdicts[1], verdicts[2]);
  }

  // On a controller: which setpoints count.
  {
    SimSunSaver sim;  // settings stored with x100
    ss::ScaleVerdict verdict = ss::judgeSettingsScale(snapshotOf(sim));
    CHECK_EQ(verdict.used, 13);  // 14 setpoints, and EV_eq is 0
    CHECK_EQ(verdict.fits[0], 13);
    CHECK_EQ(verdict.scale, ss::SCALE_100);

    // OptaView's 'scalecheck' finds the same on that controller: its setpoints point at x100.
    // There they only point, since its verdict needs a setting read in MSView.
    Rig rig;
    const std::string out = rig.run("scalecheck");
    CHECK_HAS(out, "13 of 13");
    CHECK_HAS(out, "The setpoints point at x100, which OptaView uses now.");

    sim.regs[0xE01B] = 0xFFFF;  // a blank cell is not a setting somebody chose
    verdict = ss::judgeSettingsScale(snapshotOf(sim));
    CHECK_EQ(verdict.used, 12);
    sim.regs[0xE01C] = sim.setVolts(9.0);  // EV_hvr at 9 V: below what that register allows, under both scales
    verdict = ss::judgeSettingsScale(snapshotOf(sim));
    CHECK_EQ(verdict.used, 11);
    sim.regs[0xE01C] = (uint16_t)(10.2 * 32768.0 / 100.0);  // 10.2 V at x100 is allowed, 9.86 V at x96.667 is not: it counts
    verdict = ss::judgeSettingsScale(snapshotOf(sim));
    CHECK_EQ(verdict.used, 12);
    CHECK_EQ(verdict.fits[0], 12);
    sim.regs.erase(0xE022);  // a register that did not answer is not a witness
    sim.regs.erase(0xE023);
    verdict = ss::judgeSettingsScale(snapshotOf(sim));
    CHECK_EQ(verdict.used, 10);
    CHECK_EQ(verdict.scale, ss::SCALE_100);
    // A setting that is no witness changes nothing, however odd it is.
    sim.regs[0xE004] = 4001;
    sim.regs[0xE01D] = 4003;
    CHECK_EQ(ss::judgeSettingsScale(snapshotOf(sim)).used, 10);
    CHECK_EQ(ss::judgeSettingsScale(snapshotOf(sim)).fits[0], 10);
  }
  {
    SimSunSaver sim(100.0, 96.667);  // measures with x100, stores with x96.667
    const ss::ScaleVerdict verdict = ss::judgeSettingsScale(snapshotOf(sim));
    CHECK_EQ(verdict.used, 13);
    CHECK_EQ(verdict.fits[1], 13);
    CHECK_EQ(verdict.scale, ss::SCALE_96667);
  }
  {
    // Too few setpoints hold a setting.
    SimSunSaver sim;
    const uint16_t keep[] = {0xE000, 0xE001, 0xE022};
    for (uint8_t i = 0; i < ss::SETTING_ITEMS; ++i) {
      const uint16_t addr = ss::item(i).addr;
      if ((ss::item(i).flags & ss::ITEM_WITNESS) != 0 && addr != keep[0] && addr != keep[1] && addr != keep[2]) {
        sim.regs[addr] = 0;
      }
    }
    const ss::ScaleVerdict verdict = ss::judgeSettingsScale(snapshotOf(sim));
    CHECK_EQ(verdict.used, 3);
    CHECK_EQ(verdict.fits[0], 3);
    CHECK_EQ(verdict.scale, ss::SCALE_UNKNOWN);
  }
  {
    // Nothing was read at all.
    ss::Snapshot empty;
    ss::clearSnapshot(&empty);
    const ss::ScaleVerdict verdict = ss::judgeSettingsScale(empty);
    CHECK_EQ(verdict.used, 0);
    CHECK_EQ(verdict.scale, ss::SCALE_UNKNOWN);
  }
  {
    // Setpoints that somebody set to odd values prove nothing.
    SimSunSaver sim;
    for (uint8_t i = 0; i < ss::SETTING_ITEMS; ++i) {
      if ((ss::item(i).flags & ss::ITEM_WITNESS) != 0 && sim.regs[ss::item(i).addr] != 0) {
        sim.regs[ss::item(i).addr] = (uint16_t)(sim.regs[ss::item(i).addr] + 7);  // 0.02 V up
      }
    }
    const ss::ScaleVerdict verdict = ss::judgeSettingsScale(snapshotOf(sim));
    CHECK_EQ(verdict.used, 13);
    CHECK_EQ(verdict.scale, ss::SCALE_UNKNOWN);
  }
}

// ============================================================================
// The scale of the live readings
// ============================================================================
static void testLiveScale() {
  uint8_t scale = 99;
  uint8_t source = 99;

  // 12 or 24 V. Both scales have to give the same system, since the live scale is not known yet.
  {
    SimSunSaver sim;
    const double volts[] = {0.0, 7.95, 8.05, 8.30, 12.80, 16.95, 17.05, 17.30, 17.60, 26.40, 33.95, 34.05, 199.0};
    const int want[] = {0, 0, 0, 12, 12, 12, 0, 0, 24, 24, 24, 0, 0};
    for (size_t i = 0; i < sizeof(volts) / sizeof(volts[0]); ++i) {
      sim.regs[0x0008] = sim.volts(volts[i]);
      CHECK_MSG(ss::systemVolts(snapshotOf(sim)) == want[i], "%.2f V", volts[i]);
    }
    // The words at the ends: 2712 is 8.00 V with x96.667, 5570 is 17.00 V with x100, 5763 is
    // 17.00 V with x96.667 and 11141 is 34.00 V with x100. In between, each scale gives another
    // system.
    const uint16_t words[] = {2711, 2712, 5570, 5571, 5627, 5762, 5763, 11141, 11142};
    const int wantWord[] = {0, 12, 12, 0, 0, 0, 24, 24, 0};
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
      sim.regs[0x0008] = words[i];
      CHECK_MSG(ss::systemVolts(snapshotOf(sim)) == wantWord[i], "word %u", (unsigned)words[i]);
    }
    CHECK_EQ(ss::systemVoltsFor(8.00), 12);
    CHECK_EQ(ss::systemVoltsFor(7.99), 0);
    CHECK_EQ(ss::systemVoltsFor(17.00), 12);
    CHECK_EQ(ss::systemVoltsFor(17.01), 24);
    CHECK_EQ(ss::systemVoltsFor(34.00), 24);
    CHECK_EQ(ss::systemVoltsFor(34.01), 0);
    // The other scale puts a battery 3.3 % lower. Away from the ends that changes nothing.
    SimSunSaver other(96.667);
    other.regs[0x0008] = other.volts(12.80);
    CHECK_EQ(ss::systemVolts(snapshotOf(other)), 12);
    other.regs[0x0008] = other.volts(25.60);
    CHECK_EQ(ss::systemVolts(snapshotOf(other)), 24);
    // A 12 V battery at 16.60 V, as an x96.667 controller reports it: 17.17 V with x100.
    other.regs[0x0008] = other.volts(16.60);
    CHECK_EQ(other.regs[0x0008], 5627);
    CHECK_EQ(ss::systemVolts(snapshotOf(other)), 0);
    sim.regs.erase(0x0008);
    CHECK_EQ(ss::systemVolts(snapshotOf(sim)), 0);
  }
  {
    // Switch 2 ON: the LVD in effect is the stored one.
    SimSunSaver sim;
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_100, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_100);
    CHECK_EQ(source, ss::LIVE_SOURCE_STORED_LVD);
    // Without a verdict on the settings the stored LVD is not known, and so is the live scale.
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_UNKNOWN, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_UNKNOWN);
    CHECK_EQ(source, ss::LIVE_SOURCE_NONE);
    // Not known is not 0 V: a V_lvd next to zero proves nothing either. (33 counts in a 24 V
    // system are within reach of 0 V with x96.667 and out of it with x100.)
    SimSunSaver low;
    low.regs[0x0008] = low.volts(27.00);
    low.regs[0x001C] = 33;
    scale = source = 99;
    ss::judgeLiveScale(snapshotOf(low), ss::SCALE_UNKNOWN, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_UNKNOWN);
    CHECK_EQ(source, ss::LIVE_SOURCE_NONE);
    // The load current pulls the LVD in effect down a little.
    sim.regs[0x001C] = sim.volts(11.30);
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_100, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_100);
    sim.regs[0x001C] = sim.volts(11.10);  // more than the compensation can explain
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_100, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_UNKNOWN);
    CHECK_EQ(source, ss::LIVE_SOURCE_NONE);
    sim.regs[0x001C] = 0;
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_100, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_UNKNOWN);
    sim.regs[0x001C] = sim.volts(11.50);
    sim.regs[0xE022] = 0;
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_100, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_UNKNOWN);
  }
  {
    // A controller that measures with x96.667 and stores with x100: each verdict stands alone.
    SimSunSaver sim(96.667, 100.0);
    CHECK_EQ(ss::judgeSettingsScale(snapshotOf(sim)).scale, ss::SCALE_100);
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_100, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_96667);
    CHECK_EQ(source, ss::LIVE_SOURCE_STORED_LVD);
  }
  {
    // And the other way round.
    SimSunSaver sim(100.0, 96.667);
    CHECK_EQ(ss::judgeSettingsScale(snapshotOf(sim)).scale, ss::SCALE_96667);
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_96667, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_100);
    CHECK_EQ(source, ss::LIVE_SOURCE_STORED_LVD);
  }
  {
    // Switch 2 OFF: the built-in 11.50 V, whatever is stored and whatever is known about it.
    for (int which = 0; which < 2; ++which) {
      SimSunSaver sim(which == 0 ? 100.0 : 96.667, 100.0);
      sim.regs[0x0025] = 0x0008;
      sim.regs[0xE022] = sim.setVolts(13.10);
      sim.regs[0x001C] = sim.volts(11.50);
      ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_UNKNOWN, &scale, &source);
      CHECK_EQ(scale, which == 0 ? ss::SCALE_100 : ss::SCALE_96667);
      CHECK_EQ(source, ss::LIVE_SOURCE_PRESET_LVD);
      sim.regs[0x001C] = sim.volts(12.30);  // not the built-in value under either scale
      ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_100, &scale, &source);
      CHECK_EQ(scale, ss::SCALE_UNKNOWN);
      CHECK_EQ(source, ss::LIVE_SOURCE_NONE);
    }
  }
  {
    // A 24 V system: the controller doubles its 12 V figures.
    SimSunSaver sim;
    sim.regs[0x0008] = sim.volts(26.40);
    sim.regs[0x0025] = 0x0008;
    sim.regs[0x001C] = sim.volts(23.00);
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_UNKNOWN, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_100);
    CHECK_EQ(source, ss::LIVE_SOURCE_PRESET_LVD);
    sim.regs[0x0025] = 0x000A;  // the stored 11.50 V, doubled
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_100, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_100);
    CHECK_EQ(source, ss::LIVE_SOURCE_STORED_LVD);
    sim.regs[0x001C] = sim.volts(11.50);  // a 12 V figure in a 24 V system fits nothing
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_100, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_UNKNOWN);
  }
  {
    // With switch 2 ON the compensation is the stored one too, and that can be large: 13.10 V
    // less 0.10 ohm at 6 A is 12.50 V, which a controller that encodes with x96.667 reports as
    // a word that reads 12.93 V with x100, inside the room the test leaves for x100. Such a
    // reading decides nothing.
    SimSunSaver sim(96.667, 96.667);
    makeLiFePO4(sim);
    sim.regs[0x001C] = sim.volts(12.50);
    sim.regs[0xE026] = 5189;  // 0.1000 ohm
    CHECK_EQ(ss::judgeSettingsScale(snapshotOf(sim)).scale, ss::SCALE_96667);
    CHECK(ss::voltsFromRaw(sim.regs[0x001C], ss::SCALE_100) > 13.10 - ss::LVD_COMPENSATION_ROOM_VOLTS);
    scale = source = 99;
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_96667, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_UNKNOWN);
    CHECK_EQ(source, ss::LIVE_SOURCE_NONE);
    // So the page does not call this firmware's scale wrong, and the no-charge alert keeps to the
    // lower of the two figures.
    ss::Evaluation e = judge(sim, ss::CHEMISTRY_LIFEPO4, 12, ss::SCALE_96667);
    CHECK_EQ(e.liveScale, ss::SCALE_UNKNOWN);
    CHECK_EQ(e.findings & ss::FINDING_LIVE_SCALE, 0);
    CHECK_EQ(e.verdict, ss::VERDICT_OK);
    // Up to 0.02 ohm the room holds at the rated 15 A, and the reading decides.
    sim.regs[0x001C] = sim.volts(13.10);
    sim.regs[0xE026] = 1037;  // 0.0200 ohm
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_96667, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_96667);
    CHECK_EQ(source, ss::LIVE_SOURCE_STORED_LVD);
    sim.regs[0xE026] = 1038;
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_96667, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_UNKNOWN);
    CHECK_EQ(source, ss::LIVE_SOURCE_NONE);
    // Not read, it could be anything.
    sim.regs.erase(0xE026);
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_96667, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_UNKNOWN);
    // With switch 2 OFF the built-in compensation applies, whatever is stored or was not read.
    sim.regs[0x0025] = 0x0009;
    sim.regs[0x001C] = sim.volts(11.50);
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_96667, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_96667);
    CHECK_EQ(source, ss::LIVE_SOURCE_PRESET_LVD);
    sim.regs[0xE026] = 5189;
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_96667, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_96667);
    CHECK_EQ(source, ss::LIVE_SOURCE_PRESET_LVD);
  }
  {
    // The controller works the LVD in effect out in whole words, so at the full compensation the
    // word can read a little below the stored LVD less 0.30 V: EV_lvd 3051 (9.0006 V with
    // x96.667) less 0.0200 ohm at 15 A comes back as V_lvd 2949, which reads 8.6997 V, below
    // 8.7006 V. With x100 the same word reads 9.00 V, inside the room too: the reading decides
    // nothing. Without room for the rounding, x100 alone would match and be taken for the scale.
    SimSunSaver sim(96.667, 96.667);
    makeLiFePO4(sim);
    sim.regs[0xE022] = 3051;
    sim.regs[0xE026] = 1037;
    sim.regs[0x001C] = 2949;
    CHECK(ss::storedCompensationFits(snapshotOf(sim)));
    CHECK(ss::voltsFromRaw(2949, ss::SCALE_96667) < ss::voltsFromRaw(3051, ss::SCALE_96667) - ss::LVD_COMPENSATION_ROOM_VOLTS);
    scale = source = 99;
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_96667, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_UNKNOWN);
    CHECK_EQ(source, ss::LIVE_SOURCE_NONE);
    // At 13.10 V the two scales are further apart than the room, and the same compensation, worked
    // out in whole words, decides for x96.667.
    sim.regs[0xE022] = sim.setVolts(13.10);
    const double stored = ss::voltsFromRaw(sim.regs[0xE022], ss::SCALE_96667);
    sim.regs[0x001C] = (uint16_t)((stored - ss::ohmsFromRaw(1037) * ss::LOAD_RATED_AMPS) * 32768.0 / 96.667);
    CHECK(ss::voltsFromRaw(sim.regs[0x001C], ss::SCALE_96667) < stored - ss::LVD_COMPENSATION_ROOM_VOLTS);
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_96667, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_96667);
    CHECK_EQ(source, ss::LIVE_SOURCE_STORED_LVD);
    // The room is a few words, not more: 0.02 V further down is no LVD this compensation gives.
    sim.regs[0x001C] = (uint16_t)((stored - ss::LVD_COMPENSATION_ROOM_VOLTS - 0.02) * 32768.0 / 96.667);
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_96667, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_UNKNOWN);
  }
  {
    // A 24 V system doubles the figures, and the rounding with them: EV_lvd 3051 with the same
    // compensation can come back as V_lvd 5895 (twice 2947.5), 17.3906 V with x96.667, which is
    // within twice the room below 18.0012 V. With x100 it reads 17.99 V, inside too.
    SimSunSaver sim(96.667, 96.667);
    makeLiFePO4(sim);
    sim.regs[0xE022] = 3051;
    sim.regs[0xE026] = 1037;
    sim.regs[0x0008] = sim.volts(26.80);
    sim.regs[0x001C] = 5895;
    CHECK_EQ(ss::systemVolts(snapshotOf(sim)), 24);
    CHECK(ss::voltsFromRaw(5895, ss::SCALE_96667) <
          2.0 * ss::voltsFromRaw(3051, ss::SCALE_96667) - 2.0 * ss::LVD_COMPENSATION_ROOM_VOLTS - ss::LVD_ROUNDING_ROOM_VOLTS);
    uint8_t scale = 99, source = 99;
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_96667, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_UNKNOWN);
    CHECK_EQ(source, ss::LIVE_SOURCE_NONE);
  }
  {
    // A write to the EEPROM, and no restart yet. The controller keeps the LVD it started with,
    // and both fault words report the edit until it restarts. An x100 controller that still uses
    // 11.50 V after its stored LVD went to 11.30 V reports a word that reads 11.12 V with x96.667,
    // inside the room for 11.30 V, and 11.50 V with x100, outside it. Such a reading decides
    // nothing while either word reports the edit.
    Bare b;
    CHECK_EQ(b.master.writeRegister(1, 0xE022, b.sim.setVolts(11.30)).status, MB_OK);
    CHECK_EQ(b.sim.regs[0x001C], b.sim.volts(11.50));
    CHECK(ss::voltsFromRaw(b.sim.regs[0x001C], ss::SCALE_96667) > 11.30 - ss::LVD_COMPENSATION_ROOM_VOLTS);
    ss::Snapshot edited = snapshotOf(b.sim);
    CHECK_EQ(ss::judgeSettingsScale(edited).scale, ss::SCALE_100);
    CHECK(!ss::storedSettingsInEffect(edited));
    scale = source = 99;
    ss::judgeLiveScale(edited, ss::SCALE_100, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_UNKNOWN);
    CHECK_EQ(source, ss::LIVE_SOURCE_NONE);
    // So the page does not call this firmware's scale wrong, and the no-charge alert keeps to the
    // lower of the two figures.
    ss::Evaluation e = judge(b.sim, ss::CHEMISTRY_LEAD_SEALED, 12, ss::SCALE_100);
    CHECK_EQ(e.liveScale, ss::SCALE_UNKNOWN);
    CHECK_EQ(e.findings & ss::FINDING_LIVE_SCALE, 0);
    // Either word alone is enough, and so is a word that was not read.
    const uint8_t words[2] = {ss::I_ARRAY_FAULT, ss::I_LOAD_FAULT};
    const uint16_t bits[2] = {ss::ARRAY_FAULT_EEPROM_EDIT, ss::LOAD_FAULT_EEPROM_EDIT};
    for (int w = 0; w < 2; ++w) {
      ss::Snapshot one = edited;
      one.raw[words[1 - w]] = (uint16_t)(one.raw[words[1 - w]] & ~bits[1 - w]);
      CHECK((one.raw[words[w]] & bits[w]) != 0);
      ss::judgeLiveScale(one, ss::SCALE_100, &scale, &source);
      CHECK_EQ(scale, ss::SCALE_UNKNOWN);
      // Other fault bits are no edit.
      ss::Snapshot other = edited;
      other.raw[ss::I_ARRAY_FAULT] = (uint16_t)~ss::ARRAY_FAULT_EEPROM_EDIT;
      other.raw[ss::I_LOAD_FAULT] = (uint16_t)~ss::LOAD_FAULT_EEPROM_EDIT;
      other.raw[ss::I_V_LVD] = b.sim.volts(11.30);
      CHECK(ss::storedSettingsInEffect(other));
      ss::judgeLiveScale(other, ss::SCALE_100, &scale, &source);
      CHECK_EQ(scale, ss::SCALE_100);
      other.status[words[w]] = ss::SSIO_REFUSED;
      CHECK(!ss::storedSettingsInEffect(other));
      ss::judgeLiveScale(other, ss::SCALE_100, &scale, &source);
      CHECK_EQ(scale, ss::SCALE_UNKNOWN);
      CHECK_EQ(source, ss::LIVE_SOURCE_NONE);
    }
    // The restart puts the stored LVD to work and clears the edit: now the reading decides.
    CHECK_EQ(b.master.writeCoil(1, 0x00FF, true).status, MB_OK);
    CHECK_EQ(b.sim.resets, 1);
    const ss::Snapshot restarted = snapshotOf(b.sim);
    CHECK(ss::storedSettingsInEffect(restarted));
    ss::judgeLiveScale(restarted, ss::SCALE_100, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_100);
    CHECK_EQ(source, ss::LIVE_SOURCE_STORED_LVD);
    // With switch 2 OFF the stored LVD is not used, and an edit changes nothing.
    b.sim.advance(b.sim.resetDownMs);  // the controller is silent while it starts
    CHECK_EQ(b.master.writeRegister(1, 0xE022, b.sim.setVolts(12.00)).status, MB_OK);
    CHECK(!ss::storedSettingsInEffect(snapshotOf(b.sim)));
    b.sim.regs[0x0025] = 0x0008;
    b.sim.regs[0x001C] = b.sim.volts(11.50);
    ss::judgeLiveScale(snapshotOf(b.sim), ss::SCALE_100, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_100);
    CHECK_EQ(source, ss::LIVE_SOURCE_PRESET_LVD);
  }
  {
    // A low LVD, where the two scales are closer together than the room the test leaves: a
    // reading that both explain decides nothing.
    SimSunSaver sim;
    sim.regs[0xE022] = sim.setVolts(9.00);
    sim.regs[0x001C] = sim.volts(9.04);
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_100, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_UNKNOWN);
    CHECK_EQ(source, ss::LIVE_SOURCE_NONE);
    sim.regs[0x001C] = sim.volts(8.95);  // 8.65 V with x96.667: only one explains this
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_100, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_100);
  }
  {
    // What it needs and did not get.
    const uint16_t needed[] = {0x0025, 0x001C, 0x0008};
    for (size_t i = 0; i < sizeof(needed) / sizeof(needed[0]); ++i) {
      SimSunSaver sim;
      sim.regs.erase(needed[i]);
      scale = 99;
      source = 99;
      ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_100, &scale, &source);
      CHECK_EQ(scale, ss::SCALE_UNKNOWN);
      CHECK_EQ(source, ss::LIVE_SOURCE_NONE);
    }
    SimSunSaver sim;
    sim.regs.erase(0xE022);
    ss::judgeLiveScale(snapshotOf(sim), ss::SCALE_100, &scale, &source);
    CHECK_EQ(scale, ss::SCALE_UNKNOWN);
  }
}

// ============================================================================
// Do the settings suit the battery?
// ============================================================================
static void testEvaluate() {
  const uint32_t kNone = 0;

  // The controller as it comes: lead-acid settings, the custom charge settings not in use.
  {
    SimSunSaver sim;
    ss::Evaluation e = judge(sim, ss::CHEMISTRY_LIFEPO4);
    CHECK_EQ(e.chemistry, ss::CHEMISTRY_LIFEPO4);
    CHECK_EQ(e.findings, ss::FINDING_PRESET_IN_USE | ss::FINDING_EQUALIZE_STORED | ss::FINDING_TEMPCOMP);
    CHECK_EQ(e.verdict, ss::VERDICT_MISMATCH);
    CHECK_EQ(e.systemVolts, 12);
    CHECK_EQ(e.settingsScale.scale, ss::SCALE_100);
    CHECK_EQ(e.settingsScale.used, 13);
    CHECK_EQ(e.liveScale, ss::SCALE_100);
    CHECK_EQ(e.liveSource, ss::LIVE_SOURCE_STORED_LVD);

    // A sealed lead-acid battery on the built-in profile: nothing to object to, and what is
    // stored in the custom banks does not matter while switch 1 is OFF.
    e = judge(sim, ss::CHEMISTRY_LEAD_SEALED);
    CHECK_EQ(e.findings, kNone);
    CHECK_EQ(e.verdict, ss::VERDICT_OK);
    sim.regs[0x0025] = 0x000E;  // switch 3 ON
    e = judge(sim, ss::CHEMISTRY_LEAD_SEALED);
    CHECK_EQ(e.findings, ss::FINDING_EQUALIZE_SWITCH);
    CHECK_EQ(e.verdict, ss::VERDICT_MISMATCH);
    sim.regs[0x0025] = 0x000B;  // switch 1 ON: bank 2 holds an equalize voltage
    e = judge(sim, ss::CHEMISTRY_LEAD_SEALED);
    CHECK_EQ(e.findings, ss::FINDING_EQUALIZE_STORED);
    CHECK_EQ(e.verdict, ss::VERDICT_MISMATCH);
    sim.regs[0xE014] = 0;
    e = judge(sim, ss::CHEMISTRY_LEAD_SEALED);
    CHECK_EQ(e.findings, kNone);
    CHECK_EQ(e.verdict, ss::VERDICT_OK);
    sim.regs[0xE007] = sim.setVolts(14.80);  // and bank 1 counts as well
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LEAD_SEALED).findings, ss::FINDING_EQUALIZE_STORED);
    sim.regs.erase(0xE007);
    e = judge(sim, ss::CHEMISTRY_LEAD_SEALED);
    CHECK_EQ(e.findings, ss::FINDING_NOT_READ);
    CHECK_EQ(e.verdict, ss::VERDICT_CANNOT_TELL);
    // The temperature compensation and the voltages are right for lead-acid whatever they are.
    sim.regs[0xE007] = 0;
    sim.regs.erase(0xE01A);
    sim.regs.erase(0xE000);
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LEAD_SEALED).verdict, ss::VERDICT_OK);
  }
  {
    // A flooded battery may be equalized: there is nothing this check objects to.
    SimSunSaver sim;
    sim.regs[0x0025] = 0x000F;
    ss::Evaluation e = judge(sim, ss::CHEMISTRY_LEAD_FLOODED);
    CHECK_EQ(e.findings, kNone);
    CHECK_EQ(e.verdict, ss::VERDICT_OK);
    sim.regs.erase(0x0025);
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LEAD_FLOODED).verdict, ss::VERDICT_OK);
    // And with no battery selected nothing is judged, though the scales are still worked out.
    SimSunSaver other;
    e = judge(other, ss::CHEMISTRY_NOT_JUDGED);
    CHECK_EQ(e.findings, kNone);
    CHECK_EQ(e.verdict, ss::VERDICT_NOT_JUDGED);
    CHECK_EQ(e.settingsScale.scale, ss::SCALE_100);
    CHECK_EQ(e.liveScale, ss::SCALE_100);
    makeLiFePO4(other);
    other.regs[0x0025] = 0x000F;
    CHECK_EQ(judge(other, ss::CHEMISTRY_NOT_JUDGED).verdict, ss::VERDICT_NOT_JUDGED);
    CHECK_EQ(judge(other, 77).verdict, ss::VERDICT_OK);  // a battery this code has no rules for
    CHECK_EQ(judge(other, 77).findings, kNone);
  }

  // A controller that was programmed for LiFePO4, whatever scales it uses.
  const double scales[4][2] = {{100.0, 100.0}, {96.667, 100.0}, {100.0, 96.667}, {96.667, 96.667}};
  for (int s = 0; s < 4; ++s) {
    SimSunSaver sim(scales[s][0], scales[s][1]);
    makeLiFePO4(sim);
    const ss::Evaluation e = judge(sim, ss::CHEMISTRY_LIFEPO4);
    CHECK_MSG(e.findings == kNone && e.verdict == ss::VERDICT_OK, "scales %d: findings 0x%lX, verdict %d", s,
              (unsigned long)e.findings, (int)e.verdict);
    CHECK_EQ(e.settingsScale.used, 12);
    CHECK_EQ(e.settingsScale.scale, scales[s][1] > 99.0 ? ss::SCALE_100 : ss::SCALE_96667);
    CHECK_EQ(e.liveScale, scales[s][0] > 99.0 ? ss::SCALE_100 : ss::SCALE_96667);
    CHECK_EQ(e.liveSource, ss::LIVE_SOURCE_STORED_LVD);
  }

  // One thing wrong at a time.
  {
    struct Case {
      uint16_t addr;
      double volts;  // a voltage setting, or a negative number to use `raw`
      uint16_t raw;
      uint32_t findings;
      uint8_t verdict;
    };
    const Case cases[] = {
        {0x0025, -1, 0x000A, ss::FINDING_PRESET_IN_USE, ss::VERDICT_MISMATCH},
        {0x0025, -1, 0x000F, ss::FINDING_EQUALIZE_SWITCH, ss::VERDICT_MISMATCH},
        {0xE007, 14.60, 0, ss::FINDING_EQUALIZE_STORED, ss::VERDICT_MISMATCH},
        {0xE014, 14.60, 0, ss::FINDING_EQUALIZE_STORED, ss::VERDICT_MISMATCH},
        {0xE007, -1, 1, ss::FINDING_EQUALIZE_STORED, ss::VERDICT_MISMATCH},
        {0xE01A, -1, 5, ss::FINDING_TEMPCOMP, ss::VERDICT_MISMATCH},
        {0xE01A, -1, 0x0105, ss::FINDING_TEMPCOMP, ss::VERDICT_MISMATCH},
        {0xE01A, -1, 0x0100, kNone, ss::VERDICT_OK},  // only the low byte is used
        {0xE01A, -1, 0xFF00, kNone, ss::VERDICT_OK},
        {0xE000, 14.70, 0, ss::FINDING_ABSORB_RANGE, ss::VERDICT_MISMATCH},
        {0xE00D, 14.70, 0, ss::FINDING_ABSORB_RANGE, ss::VERDICT_MISMATCH},
        {0xE000, 13.70, 0, ss::FINDING_ABSORB_RANGE, ss::VERDICT_MISMATCH},
        {0xE000, 14.65, 0, ss::FINDING_ABSORB_RANGE, ss::VERDICT_MISMATCH},
        {0xE000, 13.75, 0, ss::FINDING_ABSORB_RANGE, ss::VERDICT_MISMATCH},
        {0xE000, 14.60, 0, kNone, ss::VERDICT_OK},  // the ends of the range are inside it
        {0xE00D, 13.80, 0, kNone, ss::VERDICT_OK},
        {0xE001, 14.00, 0, ss::FINDING_FLOAT_RANGE, ss::VERDICT_MISMATCH},
        {0xE00E, 13.10, 0, ss::FINDING_FLOAT_RANGE, ss::VERDICT_MISMATCH},
        {0xE001, 13.95, 0, ss::FINDING_FLOAT_RANGE, ss::VERDICT_MISMATCH},
        {0xE001, 13.15, 0, ss::FINDING_FLOAT_RANGE, ss::VERDICT_MISMATCH},
        {0xE001, 13.90, 0, kNone, ss::VERDICT_OK},
        {0xE00E, 13.20, 0, kNone, ss::VERDICT_OK},
        {0xE001, -1, 0, ss::FINDING_FLOAT_RANGE, ss::VERDICT_MISMATCH},  // no float: held at the absorption voltage
        // What does not matter to the battery is not a finding.
        {0xE002, -1, 14400, kNone, ss::VERDICT_OK},
        {0xE01B, 15.20, 0, kNone, ss::VERDICT_OK},
        {0xE022, 11.50, 0, kNone, ss::VERDICT_OK},
        {0xE026, -1, 778, kNone, ss::VERDICT_OK},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
      for (int s = 0; s < 2; ++s) {
        SimSunSaver sim(100.0, s == 0 ? 100.0 : 96.667);
        makeLiFePO4(sim);
        sim.regs[cases[c].addr] = cases[c].volts < 0 ? cases[c].raw : sim.setVolts(cases[c].volts);
        if (cases[c].addr == 0xE022) {
          sim.regs[0x001C] = sim.volts(cases[c].volts);  // the LVD in effect follows the stored one
        }
        const ss::Evaluation e = judge(sim, ss::CHEMISTRY_LIFEPO4);
        CHECK_MSG(e.findings == cases[c].findings && e.verdict == cases[c].verdict,
                  "case %d, scale %d: findings 0x%lX, verdict %d", (int)c, s, (unsigned long)e.findings,
                  (int)e.verdict);
      }
    }
  }
  {
    // Switch 2 OFF is a note, not a mismatch: the load terminals may not be in use at all.
    SimSunSaver sim;
    makeLiFePO4(sim);
    sim.regs[0x0025] = 0x0009;
    sim.regs[0x001C] = sim.volts(11.50);
    const ss::Evaluation e = judge(sim, ss::CHEMISTRY_LIFEPO4);
    CHECK_EQ(e.findings, ss::FINDING_PRESET_LVD);
    CHECK_EQ(e.verdict, ss::VERDICT_OK);
    CHECK_EQ(e.liveSource, ss::LIVE_SOURCE_PRESET_LVD);
    // For a lead-acid battery the built-in LVD is what it is for.
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LEAD_SEALED).findings, kNone);
  }
  {
    // Several at once, and the stored settings are judged although switch 1 is OFF.
    SimSunSaver sim;
    makeLiFePO4(sim);
    sim.regs[0x0025] = 0x000C;  // SW3 and SW4
    sim.regs[0x001C] = sim.volts(11.50);
    sim.regs[0xE000] = sim.setVolts(14.80);
    sim.regs[0xE00E] = sim.setVolts(14.10);
    sim.regs[0xE01A] = 20;
    const ss::Evaluation e = judge(sim, ss::CHEMISTRY_LIFEPO4);
    CHECK_EQ(e.findings, ss::FINDING_PRESET_IN_USE | ss::FINDING_EQUALIZE_SWITCH | ss::FINDING_TEMPCOMP |
                             ss::FINDING_ABSORB_RANGE | ss::FINDING_FLOAT_RANGE | ss::FINDING_PRESET_LVD);
    CHECK_EQ(e.verdict, ss::VERDICT_MISMATCH);
  }
  {
    // A Li-ion pack of three cells has its own range.
    SimSunSaver sim;
    makeLiFePO4(sim);
    ss::Evaluation e = judge(sim, ss::CHEMISTRY_LI_ION);
    CHECK_EQ(e.findings, ss::FINDING_ABSORB_RANGE | ss::FINDING_FLOAT_RANGE);
    CHECK_EQ(e.verdict, ss::VERDICT_MISMATCH);
    sim.regs[0xE000] = sim.regs[0xE00D] = sim.setVolts(12.60);
    sim.regs[0xE001] = sim.regs[0xE00E] = sim.setVolts(12.40);
    e = judge(sim, ss::CHEMISTRY_LI_ION);
    CHECK_EQ(e.findings, kNone);
    CHECK_EQ(e.verdict, ss::VERDICT_OK);
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LIFEPO4).findings, ss::FINDING_ABSORB_RANGE | ss::FINDING_FLOAT_RANGE);
  }

  // What leaves the judgement open.
  {
    // The setpoints do not prove a scale: the voltages are not judged, with either.
    SimSunSaver sim;
    makeLiFePO4(sim);
    for (uint8_t i = 0; i < ss::SETTING_ITEMS; ++i) {
      if ((ss::item(i).flags & ss::ITEM_WITNESS) != 0 && sim.regs[ss::item(i).addr] != 0) {
        sim.regs[ss::item(i).addr] = (uint16_t)(sim.regs[ss::item(i).addr] + 7);
      }
    }
    sim.regs[0xE000] = sim.setVolts(16.02);  // far outside the range, and nobody can say so
    ss::Evaluation e = judge(sim, ss::CHEMISTRY_LIFEPO4);
    CHECK_EQ(e.settingsScale.scale, ss::SCALE_UNKNOWN);
    CHECK_EQ(e.findings, ss::FINDING_SCALE_UNPROVEN);
    CHECK_EQ(e.verdict, ss::VERDICT_CANNOT_TELL);
    CHECK_EQ(e.liveScale, ss::SCALE_UNKNOWN);  // the stored LVD is not known either
    // A mismatch that needs no voltage is still a mismatch.
    sim.regs[0xE01A] = 20;
    e = judge(sim, ss::CHEMISTRY_LIFEPO4);
    CHECK_EQ(e.findings, ss::FINDING_SCALE_UNPROVEN | ss::FINDING_TEMPCOMP);
    CHECK_EQ(e.verdict, ss::VERDICT_MISMATCH);
    // A sealed battery's judgement needs no voltage at all.
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LEAD_SEALED).verdict, ss::VERDICT_OK);
  }
  {
    // A register the judgement needs did not answer.
    const uint16_t needed[] = {0x0025, 0xE007, 0xE014, 0xE01A, 0xE000, 0xE00D, 0xE001, 0xE00E};
    for (size_t i = 0; i < sizeof(needed) / sizeof(needed[0]); ++i) {
      SimSunSaver sim;
      makeLiFePO4(sim);
      sim.regs.erase(needed[i]);
      const ss::Evaluation e = judge(sim, ss::CHEMISTRY_LIFEPO4);
      CHECK_MSG(e.findings == ss::FINDING_NOT_READ && e.verdict == ss::VERDICT_CANNOT_TELL,
                "without 0x%04X: findings 0x%lX, verdict %d", (unsigned)needed[i], (unsigned long)e.findings,
                (int)e.verdict);
    }
    // One that it does not need may be missing.
    SimSunSaver sim;
    makeLiFePO4(sim);
    sim.regs.erase(0xE026);
    sim.regs.erase(0x0010);
    sim.regs.erase(0xE002);
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LIFEPO4).verdict, ss::VERDICT_OK);
    // Nothing was read at all.
    ss::Snapshot empty;
    ss::clearSnapshot(&empty);
    const ss::Expectation want = {ss::CHEMISTRY_LIFEPO4, 12, ss::SCALE_96667};
    ss::Evaluation e;
    ss::evaluate(empty, want, &e);
    CHECK_EQ(e.findings, ss::FINDING_NOT_READ);
    CHECK_EQ(e.verdict, ss::VERDICT_CANNOT_TELL);
    CHECK_EQ(e.systemVolts, 0);
  }
  {
    // What was read is judged by itself: a register that did not answer leaves the judgement
    // open, and takes nothing from one that did answer and is wrong.
    const uint16_t setpoints[4] = {0xE000, 0xE00D, 0xE001, 0xE00E};  // EV_reg, EV_reg2, EV_float, EV_float2
    for (int missing = 0; missing < 4; ++missing) {
      for (int wrong = 0; wrong < 4; ++wrong) {
        if (wrong == missing) {
          continue;
        }
        SimSunSaver sim;
        makeLiFePO4(sim);
        sim.regs.erase(setpoints[missing]);
        sim.regs[setpoints[wrong]] = sim.setVolts(16.00);
        const ss::Evaluation e = judge(sim, ss::CHEMISTRY_LIFEPO4);
        const uint32_t expected =
            ss::FINDING_NOT_READ | (wrong < 2 ? ss::FINDING_ABSORB_RANGE : ss::FINDING_FLOAT_RANGE);
        CHECK_EQ(e.settingsScale.scale, ss::SCALE_100);
        CHECK_MSG(e.findings == expected && e.verdict == ss::VERDICT_MISMATCH,
                  "without 0x%04X, 0x%04X wrong: findings 0x%lX, verdict %d", (unsigned)setpoints[missing],
                  (unsigned)setpoints[wrong], (unsigned long)e.findings, (int)e.verdict);
      }
    }
    // The same for the two equalize voltages, for a lithium battery and for a sealed one whose
    // custom settings are in use.
    const uint16_t equalize[2] = {0xE007, 0xE014};
    const uint8_t batteries[2] = {ss::CHEMISTRY_LIFEPO4, ss::CHEMISTRY_LEAD_SEALED};
    for (int missing = 0; missing < 2; ++missing) {
      for (int battery = 0; battery < 2; ++battery) {
        SimSunSaver sim;
        makeLiFePO4(sim);
        sim.regs.erase(equalize[missing]);
        ss::Evaluation e = judge(sim, batteries[battery]);
        CHECK_EQ(e.findings, ss::FINDING_NOT_READ);
        CHECK_EQ(e.verdict, ss::VERDICT_CANNOT_TELL);
        sim.regs[equalize[1 - missing]] = sim.setVolts(14.60);
        e = judge(sim, batteries[battery]);
        CHECK_MSG(e.findings == (ss::FINDING_NOT_READ | ss::FINDING_EQUALIZE_STORED) &&
                      e.verdict == ss::VERDICT_MISMATCH,
                  "without 0x%04X, battery %d: findings 0x%lX, verdict %d", (unsigned)equalize[missing],
                  (int)batteries[battery], (unsigned long)e.findings, (int)e.verdict);
      }
    }
    // The DIP switches did not answer. For a lithium battery what is stored is judged whatever
    // they say. For a sealed one it counts only while it is known to be in use.
    {
      SimSunSaver sim;
      makeLiFePO4(sim);
      sim.regs.erase(0x0025);
      sim.regs[0xE014] = sim.setVolts(14.60);
      sim.regs[0xE001] = sim.setVolts(14.10);
      ss::Evaluation e = judge(sim, ss::CHEMISTRY_LIFEPO4);
      CHECK_EQ(e.findings, ss::FINDING_NOT_READ | ss::FINDING_EQUALIZE_STORED | ss::FINDING_FLOAT_RANGE);
      CHECK_EQ(e.verdict, ss::VERDICT_MISMATCH);
      e = judge(sim, ss::CHEMISTRY_LEAD_SEALED);
      CHECK_EQ(e.findings, ss::FINDING_NOT_READ);
      CHECK_EQ(e.verdict, ss::VERDICT_CANNOT_TELL);
      // A word that was not read is not looked at, whatever its place in the snapshot holds.
      ss::Snapshot snapshot = snapshotOf(sim);
      snapshot.raw[ss::I_DIP_SWITCH] = 0x000B;
      const ss::Expectation want = {ss::CHEMISTRY_LEAD_SEALED, 12, ss::SCALE_UNKNOWN};
      ss::evaluate(snapshot, want, &e);
      CHECK_EQ(e.findings, ss::FINDING_NOT_READ);
      CHECK_EQ(e.verdict, ss::VERDICT_CANNOT_TELL);
    }
    // Everything that can be wrong together with everything that can be missing.
    {
      SimSunSaver sim;
      makeLiFePO4(sim);
      sim.regs[0x0025] = 0x000C;               // SW1 OFF, SW3 ON
      sim.regs[0x001C] = sim.volts(11.50);
      sim.regs[0xE007] = sim.setVolts(14.60);  // EV_eq
      sim.regs.erase(0xE014);                  // EV_eq2
      sim.regs.erase(0xE01A);                  // EV_tempcomp
      sim.regs[0xE000] = sim.setVolts(14.80);  // EV_reg
      sim.regs.erase(0xE00D);                  // EV_reg2
      sim.regs.erase(0xE001);                  // EV_float
      sim.regs[0xE00E] = sim.setVolts(13.00);  // EV_float2
      const ss::Evaluation e = judge(sim, ss::CHEMISTRY_LIFEPO4);
      CHECK_EQ(e.findings, ss::FINDING_NOT_READ | ss::FINDING_PRESET_IN_USE | ss::FINDING_PRESET_LVD |
                               ss::FINDING_EQUALIZE_SWITCH | ss::FINDING_EQUALIZE_STORED |
                               ss::FINDING_ABSORB_RANGE | ss::FINDING_FLOAT_RANGE);
      CHECK_EQ(e.verdict, ss::VERDICT_MISMATCH);
    }
  }
  {
    // The battery voltage says which system this is. Without it nobody can say that this is not
    // a 24 V system, whose stored voltages are not judged: the judgement stays open.
    SimSunSaver sim;
    makeLiFePO4(sim);
    sim.regs.erase(0x0008);
    ss::Evaluation e = judge(sim, ss::CHEMISTRY_LIFEPO4);
    CHECK_EQ(e.systemVolts, 0);
    CHECK_EQ(e.findings, ss::FINDING_NOT_READ);
    CHECK_EQ(e.verdict, ss::VERDICT_CANNOT_TELL);
    sim.regs[0xE000] = sim.setVolts(16.00);  // not judged
    e = judge(sim, ss::CHEMISTRY_LIFEPO4);
    CHECK_EQ(e.findings, ss::FINDING_NOT_READ);
    CHECK_EQ(e.verdict, ss::VERDICT_CANNOT_TELL);
    sim.regs[0xE01A] = 20;                   // judged: it needs no voltage
    e = judge(sim, ss::CHEMISTRY_LIFEPO4);
    CHECK_EQ(e.findings, ss::FINDING_NOT_READ | ss::FINDING_TEMPCOMP);
    CHECK_EQ(e.verdict, ss::VERDICT_MISMATCH);
    // A sealed battery's judgement needs no voltage at all.
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LEAD_SEALED).findings, kNone);
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LEAD_SEALED).verdict, ss::VERDICT_OK);
  }
  {
    // A battery voltage that fits neither system, or each scale another: the same, under its own
    // name.
    const double odd[] = {0.0, 5.00, 7.99, 8.10, 17.05, 17.40, 34.02, 60.00};
    for (size_t i = 0; i < sizeof(odd) / sizeof(odd[0]); ++i) {
      SimSunSaver sim;
      makeLiFePO4(sim);
      sim.regs[0x0008] = sim.volts(odd[i]);
      ss::Evaluation e = judge(sim, ss::CHEMISTRY_LIFEPO4);
      CHECK_EQ(e.systemVolts, 0);
      CHECK_MSG(e.findings == ss::FINDING_SYSTEM_UNKNOWN && e.verdict == ss::VERDICT_CANNOT_TELL,
                "battery at %.2f V: findings 0x%lX, verdict %d", odd[i], (unsigned long)e.findings, (int)e.verdict);
      sim.regs[0xE000] = sim.setVolts(16.00);  // not judged
      sim.regs.erase(0xE00E);                  // and still counted as missing
      e = judge(sim, ss::CHEMISTRY_LIFEPO4);
      CHECK_EQ(e.findings, ss::FINDING_SYSTEM_UNKNOWN | ss::FINDING_NOT_READ);
      CHECK_EQ(e.verdict, ss::VERDICT_CANNOT_TELL);
      sim.regs[0xE007] = sim.setVolts(14.60);  // judged: it needs no voltage
      e = judge(sim, ss::CHEMISTRY_LIFEPO4);
      CHECK_EQ(e.findings, ss::FINDING_SYSTEM_UNKNOWN | ss::FINDING_NOT_READ | ss::FINDING_EQUALIZE_STORED);
      CHECK_EQ(e.verdict, ss::VERDICT_MISMATCH);
      CHECK_EQ(judge(sim, ss::CHEMISTRY_LEAD_FLOODED).findings, kNone);
      CHECK_EQ(judge(sim, ss::CHEMISTRY_NOT_JUDGED).findings, kNone);
    }
    // Just inside the ends of the two systems.
    const double fits[] = {8.30, 13.30, 16.99, 17.60, 26.60, 33.99};
    for (size_t i = 0; i < sizeof(fits) / sizeof(fits[0]); ++i) {
      SimSunSaver sim;
      makeLiFePO4(sim);
      sim.regs[0x0008] = sim.volts(fits[i]);
      const ss::Evaluation e = judge(sim, ss::CHEMISTRY_LIFEPO4, 0);
      CHECK_EQ(e.systemVolts, fits[i] > 17.00 ? 24 : 12);
      CHECK_MSG((e.findings & ss::FINDING_SYSTEM_UNKNOWN) == 0, "battery at %.2f V", fits[i]);
      CHECK_EQ(e.findings & ss::FINDING_SYSTEM_24V, fits[i] > 17.00 ? ss::FINDING_SYSTEM_24V : kNone);
    }
  }
  {
    // A 24 V system: the stored voltages are left alone, the rest is judged.
    SimSunSaver sim;
    makeLiFePO4(sim);
    sim.regs[0x0008] = sim.volts(26.60);
    sim.regs[0x001C] = sim.volts(26.20);
    ss::Evaluation e = judge(sim, ss::CHEMISTRY_LIFEPO4, 24);
    CHECK_EQ(e.systemVolts, 24);
    CHECK_EQ(e.findings, ss::FINDING_SYSTEM_24V);
    CHECK_EQ(e.verdict, ss::VERDICT_CANNOT_TELL);
    CHECK_EQ(e.liveScale, ss::SCALE_100);
    sim.regs[0xE000] = sim.setVolts(16.00);  // not judged
    sim.regs[0xE01A] = 20;                   // judged
    e = judge(sim, ss::CHEMISTRY_LIFEPO4, 24);
    CHECK_EQ(e.findings, ss::FINDING_SYSTEM_24V | ss::FINDING_TEMPCOMP);
    CHECK_EQ(e.verdict, ss::VERDICT_MISMATCH);
    sim.regs[0xE01A] = 0;
    sim.regs.erase(0xE00D);  // a voltage that did not answer is missing all the same
    e = judge(sim, ss::CHEMISTRY_LIFEPO4, 24);
    CHECK_EQ(e.findings, ss::FINDING_SYSTEM_24V | ss::FINDING_NOT_READ);
    CHECK_EQ(e.verdict, ss::VERDICT_CANNOT_TELL);
  }

  // Notes: they are reported and change no verdict.
  {
    SimSunSaver sim;
    makeLiFePO4(sim);
    sim.regs[0x0012] = 1u << 5;
    ss::Evaluation e = judge(sim, ss::CHEMISTRY_LIFEPO4);
    CHECK_EQ(e.findings, ss::FINDING_RESET_PENDING);
    CHECK_EQ(e.verdict, ss::VERDICT_OK);
    sim.regs[0x0012] = 0;
    sim.regs[0x001B] = 1u << 6;
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LIFEPO4).findings, ss::FINDING_RESET_PENDING);
    sim.regs[0x001B] = (1u << 6) | (1u << 1);
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LIFEPO4).findings, ss::FINDING_RESET_PENDING | ss::FINDING_CHARGER_FAULT);
    sim.regs[0x001B] = 0;
    sim.regs[0x0012] = 1u << 3;
    e = judge(sim, ss::CHEMISTRY_LIFEPO4);
    CHECK_EQ(e.findings, ss::FINDING_CHARGER_FAULT);
    CHECK_EQ(e.verdict, ss::VERDICT_OK);
    CHECK_EQ(judge(sim, ss::CHEMISTRY_NOT_JUDGED).findings, ss::FINDING_CHARGER_FAULT);
    CHECK_EQ(judge(sim, ss::CHEMISTRY_NOT_JUDGED).verdict, ss::VERDICT_NOT_JUDGED);
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LEAD_FLOODED).verdict, ss::VERDICT_OK);
    // Each fault word counts by itself: one that did not answer hides nothing the other says.
    sim.regs.erase(0x001B);
    e = judge(sim, ss::CHEMISTRY_LIFEPO4);
    CHECK_EQ(e.findings, ss::FINDING_CHARGER_FAULT);
    CHECK_EQ(e.verdict, ss::VERDICT_OK);
    sim.regs[0x0012] = 1u << 5;
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LIFEPO4).findings, ss::FINDING_RESET_PENDING);
    sim.regs[0x0012] = (1u << 5) | (1u << 3);
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LIFEPO4).findings, ss::FINDING_RESET_PENDING | ss::FINDING_CHARGER_FAULT);
    sim.regs[0x0012] = 0;
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LIFEPO4).findings, kNone);
    sim.regs.erase(0x0012);
    sim.regs[0x001B] = 1u << 6;
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LIFEPO4).findings, ss::FINDING_RESET_PENDING);
    sim.regs[0x001B] = 1u << 1;
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LIFEPO4).findings, ss::FINDING_CHARGER_FAULT);
    sim.regs[0x001B] = 0;
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LIFEPO4).findings, kNone);
    sim.regs.erase(0x001B);
    e = judge(sim, ss::CHEMISTRY_LIFEPO4);
    CHECK_EQ(e.findings, kNone);  // neither answered: there is nothing to say, and nothing is open
    CHECK_EQ(e.verdict, ss::VERDICT_OK);
    // And only as far as a word was read, whatever it holds.
    for (int unread = 0; unread < 2; ++unread) {
      SimSunSaver held;
      makeLiFePO4(held);
      held.regs[0x0012] = unread == 0 ? (uint16_t)((1u << 5) | (1u << 3)) : (uint16_t)0;
      held.regs[0x001B] = unread == 1 ? (uint16_t)((1u << 6) | (1u << 1)) : (uint16_t)0;
      ss::Snapshot snapshot = snapshotOf(held);
      snapshot.status[unread == 0 ? ss::I_ARRAY_FAULT : ss::I_LOAD_FAULT] = ss::SSIO_NO_REPLY;
      const ss::Expectation want = {ss::CHEMISTRY_LIFEPO4, 12, ss::SCALE_UNKNOWN};
      ss::Evaluation evaluation;
      ss::evaluate(snapshot, want, &evaluation);
      CHECK_MSG(evaluation.findings == kNone, "the unread %s fault word was looked at: findings 0x%lX",
                unread == 0 ? "array" : "load", (unsigned long)evaluation.findings);
    }
  }
  {
    // The pack voltage that was configured against the one that is there.
    SimSunSaver sim;
    makeLiFePO4(sim);
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LIFEPO4, 24).findings, ss::FINDING_PACK_VOLTAGE);
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LIFEPO4, 24).verdict, ss::VERDICT_OK);
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LIFEPO4, 0).findings, kNone);  // not configured: not compared
    CHECK_EQ(judge(sim, ss::CHEMISTRY_NOT_JUDGED, 24).findings, ss::FINDING_PACK_VOLTAGE);
    sim.regs[0x0008] = sim.volts(26.60);
    sim.regs[0x001C] = sim.volts(26.20);
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LIFEPO4, 12).findings, ss::FINDING_PACK_VOLTAGE | ss::FINDING_SYSTEM_24V);
    // Not measured: not compared. (Without it a lithium battery's judgement stays open.)
    sim.regs.erase(0x0008);
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LEAD_SEALED, 24).findings, kNone);
    CHECK_EQ(judge(sim, ss::CHEMISTRY_NOT_JUDGED, 24).findings, kNone);
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LIFEPO4, 24).findings, ss::FINDING_NOT_READ);
  }
  {
    // The charger's own LVD against the scale this firmware decodes voltages with.
    SimSunSaver sim;  // measures with x100
    makeLiFePO4(sim);
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LIFEPO4, 12, ss::SCALE_100).findings, kNone);
    ss::Evaluation e = judge(sim, ss::CHEMISTRY_LIFEPO4, 12, ss::SCALE_96667);
    CHECK_EQ(e.findings, ss::FINDING_LIVE_SCALE);
    CHECK_EQ(e.verdict, ss::VERDICT_OK);
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LIFEPO4, 12, ss::SCALE_UNKNOWN).findings, kNone);
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LEAD_FLOODED, 12, ss::SCALE_96667).findings, ss::FINDING_LIVE_SCALE);
    sim.regs[0x001C] = sim.volts(12.00);  // the LVD proves nothing now
    CHECK_EQ(judge(sim, ss::CHEMISTRY_LIFEPO4, 12, ss::SCALE_96667).findings, kNone);
    SimSunSaver other(96.667, 100.0);
    makeLiFePO4(other);
    CHECK_EQ(judge(other, ss::CHEMISTRY_LIFEPO4, 12, ss::SCALE_96667).findings, kNone);
    CHECK_EQ(judge(other, ss::CHEMISTRY_LIFEPO4, 12, ss::SCALE_100).findings, ss::FINDING_LIVE_SCALE);
  }

  // Which findings count against which battery.
  CHECK_EQ(ss::problemFindings(ss::CHEMISTRY_LIFEPO4),
           ss::FINDING_PRESET_IN_USE | ss::FINDING_EQUALIZE_SWITCH | ss::FINDING_EQUALIZE_STORED |
               ss::FINDING_TEMPCOMP | ss::FINDING_ABSORB_RANGE | ss::FINDING_FLOAT_RANGE);
  CHECK_EQ(ss::problemFindings(ss::CHEMISTRY_LI_ION), ss::problemFindings(ss::CHEMISTRY_LIFEPO4));
  CHECK_EQ(ss::problemFindings(ss::CHEMISTRY_LEAD_SEALED), ss::FINDING_EQUALIZE_SWITCH | ss::FINDING_EQUALIZE_STORED);
  CHECK_EQ(ss::problemFindings(ss::CHEMISTRY_LEAD_FLOODED), kNone);
  CHECK_EQ(ss::problemFindings(ss::CHEMISTRY_NOT_JUDGED), kNone);
  CHECK_EQ(ss::OPEN_FINDINGS, ss::FINDING_SCALE_UNPROVEN | ss::FINDING_NOT_READ | ss::FINDING_SYSTEM_24V |
                                  ss::FINDING_SYSTEM_UNKNOWN);
  for (uint8_t c = 0; c <= ss::CHEMISTRY_LI_ION; ++c) {
    CHECK_EQ(ss::problemFindings(c) & ss::OPEN_FINDINGS, kNone);  // a finding is one or the other
  }
  ss::VoltRange absorb = {1.0, 1.0};
  ss::VoltRange floating = {1.0, 1.0};
  CHECK(ss::chargeRanges(ss::CHEMISTRY_LIFEPO4, &absorb, &floating));
  CHECK(near(absorb.low, 13.80, 1e-9) && near(absorb.high, 14.60, 1e-9));
  CHECK(near(floating.low, 13.20, 1e-9) && near(floating.high, 13.90, 1e-9));
  CHECK(ss::chargeRanges(ss::CHEMISTRY_LI_ION, &absorb, &floating));
  CHECK(near(absorb.high, 12.60, 1e-9) && near(floating.high, 12.60, 1e-9));
  CHECK(!ss::chargeRanges(ss::CHEMISTRY_LEAD_SEALED, &absorb, &floating));
  CHECK(!ss::chargeRanges(ss::CHEMISTRY_NOT_JUDGED, &absorb, &floating));
}

// ============================================================================
// The fingerprint
// ============================================================================
static void testFingerprint() {
  SimSunSaver sim;
  makeLiFePO4(sim);
  const ss::Expectation want = {ss::CHEMISTRY_LIFEPO4, 12, ss::SCALE_UNKNOWN};
  ss::Snapshot snapshot = snapshotOf(sim);
  ss::Evaluation evaluation;
  ss::evaluate(snapshot, want, &evaluation);
  const uint32_t before = ss::settingsFingerprint(snapshot, evaluation);
  CHECK_EQ(ss::settingsFingerprint(snapshot, evaluation), before);

  // What the controller measures does not change it.
  sim.regs[0x0008] = sim.volts(12.95);
  sim.regs[0x000C] = SimSunSaver::amps(1.9);
  sim.regs[0x000E] = 3;
  sim.regs[0x0011] = 3;
  sim.regs[0x0014] = sim.refVolts(14.20);
  sim.regs[0x001A] = 2;
  sim.regs[0x001C] = sim.volts(13.07);
  snapshot = snapshotOf(sim);
  ss::evaluate(snapshot, want, &evaluation);
  CHECK_EQ(ss::settingsFingerprint(snapshot, evaluation), before);

  // A setting, a DIP switch, a register that stops answering, and the judgement do.
  std::vector<uint32_t> seen;
  seen.push_back(before);
  const struct {
    uint16_t addr;
    uint16_t raw;
  } changes[] = {{0xE000, 4654}, {0xE002, 901}, {0xE033, 1}, {0x0025, 0x000F}, {0x0025, 0x0003}, {0xE027, 601}};
  for (size_t c = 0; c < sizeof(changes) / sizeof(changes[0]); ++c) {
    SimSunSaver changed;
    makeLiFePO4(changed);
    changed.regs[changes[c].addr] = changes[c].raw;
    snapshot = snapshotOf(changed);
    ss::evaluate(snapshot, want, &evaluation);
    seen.push_back(ss::settingsFingerprint(snapshot, evaluation));
  }
  {
    SimSunSaver changed;
    makeLiFePO4(changed);
    changed.regs.erase(0xE026);  // refused, where it held 0 before
    snapshot = snapshotOf(changed);
    ss::evaluate(snapshot, want, &evaluation);
    seen.push_back(ss::settingsFingerprint(snapshot, evaluation));
  }
  {
    snapshot = snapshotOf(sim);
    const ss::Expectation sealed = {ss::CHEMISTRY_LEAD_SEALED, 12, ss::SCALE_UNKNOWN};
    ss::evaluate(snapshot, sealed, &evaluation);
    seen.push_back(ss::settingsFingerprint(snapshot, evaluation));  // judged against another battery
    sim.regs[0x0012] = 1u << 5;
    snapshot = snapshotOf(sim);
    ss::evaluate(snapshot, want, &evaluation);
    seen.push_back(ss::settingsFingerprint(snapshot, evaluation));  // a note appeared
    const ss::Expectation scaled = {ss::CHEMISTRY_LIFEPO4, 12, ss::SCALE_96667};
    sim.regs[0x0012] = 0;
    snapshot = snapshotOf(sim);
    ss::evaluate(snapshot, scaled, &evaluation);
    seen.push_back(ss::settingsFingerprint(snapshot, evaluation));  // and another one
  }
  for (size_t a = 0; a < seen.size(); ++a) {
    for (size_t b = a + 1; b < seen.size(); ++b) {
      CHECK_MSG(seen[a] != seen[b], "fingerprints %d and %d are the same", (int)a, (int)b);
    }
  }

  // Every part of the judgement counts by itself, each finding included.
  snapshot = snapshotOf(sim);
  ss::evaluate(snapshot, want, &evaluation);
  const uint32_t base = ss::settingsFingerprint(snapshot, evaluation);
  for (uint8_t bit = 0; bit < 32; ++bit) {
    ss::Evaluation changed = evaluation;
    changed.findings ^= (uint32_t)1 << bit;
    CHECK_MSG(ss::settingsFingerprint(snapshot, changed) != base, "finding bit %d is not in the fingerprint", (int)bit);
  }
  {
    ss::Evaluation changed = evaluation;
    changed.chemistry = ss::CHEMISTRY_LI_ION;
    CHECK(ss::settingsFingerprint(snapshot, changed) != base);
    changed = evaluation;
    changed.verdict = ss::VERDICT_CANNOT_TELL;
    CHECK(ss::settingsFingerprint(snapshot, changed) != base);
    changed = evaluation;
    changed.settingsScale.scale = ss::SCALE_96667;
    CHECK(ss::settingsFingerprint(snapshot, changed) != base);
    changed = evaluation;
    changed.liveScale = ss::SCALE_UNKNOWN;
    CHECK(ss::settingsFingerprint(snapshot, changed) != base);
    // How many setpoints were round is detail, not judgement.
    changed = evaluation;
    changed.settingsScale.fits[1] = 9;
    CHECK_EQ(ss::settingsFingerprint(snapshot, changed), base);
  }
}

// ============================================================================
// The float voltage for the no-charge alert
// ============================================================================
static void testFloatReference() {
  struct Rigged {
    // packVolts is the pack that was configured: 12, 24, or 0 for none.
    static double reference(const SimSunSaver &sim, uint8_t firmwareScale = ss::SCALE_96667, uint8_t packVolts = 12) {
      const ss::Snapshot snapshot = snapshotOf(sim);
      const ss::Expectation want = {ss::CHEMISTRY_LIFEPO4, packVolts, firmwareScale};
      ss::Evaluation evaluation;
      ss::evaluate(snapshot, want, &evaluation);
      return ss::floatReferenceVolts(snapshot, evaluation, firmwareScale);
    }
  };
  {
    // The controller measures with x100 and this firmware reads with x96.667: a battery held at
    // 13.50 V reads 13.05 V here.
    SimSunSaver sim;
    makeLiFePO4(sim);
    CHECK_MSG(near(Rigged::reference(sim), 13.05, 0.01), "got %.3f", Rigged::reference(sim));
    CHECK(near(Rigged::reference(sim, ss::SCALE_100), 13.50, 0.01));
    CHECK(near(Rigged::reference(sim, ss::SCALE_UNKNOWN), 0.0, 1e-12));
    // The LVD in effect proves nothing: the lower of the two possible readings.
    sim.regs[0x001C] = sim.volts(12.00);
    CHECK(near(Rigged::reference(sim), 13.05, 0.01));
    CHECK(near(Rigged::reference(sim, ss::SCALE_100), 13.50, 0.01));
  }
  {
    // The controller measures with x96.667, as this firmware reads: 13.50 V is 13.50 V.
    SimSunSaver sim(96.667, 100.0);
    makeLiFePO4(sim);
    CHECK(near(Rigged::reference(sim), 13.50, 0.01));
    CHECK(near(Rigged::reference(sim, ss::SCALE_100), 13.97, 0.01));
    sim.regs[0x001C] = sim.volts(12.00);  // not proven: the lower reading again
    CHECK(near(Rigged::reference(sim), 13.05, 0.01));
  }
  {
    // Settings stored with x96.667 decode to the same voltage.
    SimSunSaver sim(96.667, 96.667);
    makeLiFePO4(sim);
    CHECK(near(Rigged::reference(sim), 13.50, 0.01));
  }
  {
    // The lower bank counts, and a bank without float holds the battery at its absorption voltage.
    SimSunSaver sim(96.667, 100.0);
    makeLiFePO4(sim);
    sim.regs[0xE00E] = sim.setVolts(13.30);
    CHECK(near(Rigged::reference(sim), 13.30, 0.01));
    sim.regs[0xE00E] = 0;
    CHECK(near(Rigged::reference(sim), 13.50, 0.01));  // bank 2 holds 14.20 V, bank 1 floats at 13.50 V
    sim.regs[0xE001] = 0;
    CHECK(near(Rigged::reference(sim), 14.20, 0.01));
    sim.regs[0xE000] = 0;  // no absorption voltage either: not a setting to go by
    CHECK(near(Rigged::reference(sim), 0.0, 1e-12));
  }
  {
    // A 24 V system holds twice the stored 12 V figure, when a 24 V pack was configured or none
    // was.
    SimSunSaver sim(96.667, 100.0);
    makeLiFePO4(sim);
    sim.regs[0x0008] = sim.volts(27.00);
    sim.regs[0x001C] = sim.volts(26.20);
    CHECK(near(Rigged::reference(sim, ss::SCALE_96667, 24), 27.00, 0.02));
    CHECK(near(Rigged::reference(sim, ss::SCALE_96667, 0), 27.00, 0.02));
    // Not for a pack configured as 12 V: the measured system is not the pack (a pack voltage
    // finding), and the no-charge alert keeps the default of the configured pack.
    CHECK(near(Rigged::reference(sim, ss::SCALE_96667, 12), 0.0, 1e-12));
    // A 12 V battery at 16.60 V reads 17.17 V with x100. It is not taken for a 24 V system, which
    // would double the reference: no reference until the battery voltage tells.
    sim.regs[0x0008] = sim.volts(16.60);
    sim.regs[0x001C] = sim.volts(13.00);
    CHECK(near(Rigged::reference(sim), 0.0, 1e-12));
    sim.regs[0x0008] = sim.volts(13.30);
    CHECK(near(Rigged::reference(sim), 13.50, 0.01));
    CHECK(near(Rigged::reference(sim, ss::SCALE_96667, 0), 13.50, 0.01));
    // A pack configured as 24 V that reads as a 12 V system has no reference either: twice its
    // settings is not where it is held, and they alone are not where a 24 V pack is.
    CHECK(near(Rigged::reference(sim, ss::SCALE_96667, 24), 0.0, 1e-12));
  }
  {
    // A 12 V pack at 18 V (a charger out of regulation, or a charging source of its own) fits a
    // 24 V system with either scale. Its 13.50 V float would make a reference near 26.10 V, and
    // three daylight polls without charging current would then raise the no-charge alert, not
    // the high battery one. The measured system is not the configured pack, so there is none.
    SimSunSaver sim(100.0, 100.0);
    makeLiFePO4(sim);
    sim.regs[0x0008] = sim.volts(18.00);
    {
      const ss::Snapshot snapshot = snapshotOf(sim);
      const ss::Expectation want = {ss::CHEMISTRY_LIFEPO4, 12, ss::SCALE_96667};
      ss::Evaluation evaluation;
      ss::evaluate(snapshot, want, &evaluation);
      CHECK_EQ(evaluation.systemVolts, 24);
      CHECK((evaluation.findings & ss::FINDING_PACK_VOLTAGE) != 0);
      CHECK(near(ss::floatReferenceVolts(snapshot, evaluation, ss::SCALE_96667), 0.0, 1e-12));
      // The same snapshot judged with the finding taken away has the doubled reference: the
      // finding alone is what keeps it out.
      evaluation.findings &= ~(uint32_t)ss::FINDING_PACK_VOLTAGE;
      CHECK(ss::floatReferenceVolts(snapshot, evaluation, ss::SCALE_96667) > 26.0);
    }
    CHECK(near(Rigged::reference(sim, ss::SCALE_96667, 12), 0.0, 1e-12));
    CHECK(near(Rigged::reference(sim, ss::SCALE_100, 12), 0.0, 1e-12));
    CHECK(Rigged::reference(sim, ss::SCALE_96667, 0) > 26.0);
    // Back at 13.40 V it is the 12 V pack that was configured again.
    sim.regs[0x0008] = sim.volts(13.40);
    CHECK(near(Rigged::reference(sim, ss::SCALE_100, 12), 13.50, 0.01));
  }
  {
    // Not known: a built-in profile, temperature compensation, an unproven scale, a missing
    // register, a float voltage no battery is held at.
    SimSunSaver sim;
    CHECK(near(Rigged::reference(sim), 0.0, 1e-12));  // switch 1 OFF
    makeLiFePO4(sim);
    CHECK(Rigged::reference(sim) > 13.0);
    // Switch 1 alone says whether the charger uses the stored charge settings. Switch 2 is about
    // the LVD and has no say in it.
    sim.regs[0x0025] = 0x000A;  // SW2 and SW4 ON
    CHECK(near(Rigged::reference(sim), 0.0, 1e-12));
    sim.regs[0x0025] = 0x0009;  // SW1 and SW4 ON
    CHECK(Rigged::reference(sim) > 13.0);
    sim.regs[0x0025] = 0x000B;
    {
      // And only as far as the switches were read, whatever their word holds.
      ss::Snapshot snapshot = snapshotOf(sim);
      snapshot.status[ss::I_DIP_SWITCH] = ss::SSIO_NO_REPLY;
      const ss::Expectation want = {ss::CHEMISTRY_LIFEPO4, 12, ss::SCALE_96667};
      ss::Evaluation evaluation;
      ss::evaluate(snapshot, want, &evaluation);
      CHECK_EQ(evaluation.settingsScale.scale, ss::SCALE_100);
      CHECK(near(ss::floatReferenceVolts(snapshot, evaluation, ss::SCALE_96667), 0.0, 1e-12));
    }
    sim.regs[0xE01A] = 20;
    CHECK(near(Rigged::reference(sim), 0.0, 1e-12));
    const uint16_t needed[] = {0x0025, 0xE01A, 0xE000, 0xE00D, 0xE001, 0xE00E, 0x0008, 0xE01D};
    for (size_t i = 0; i < sizeof(needed) / sizeof(needed[0]); ++i) {
      SimSunSaver other;
      makeLiFePO4(other);
      other.regs.erase(needed[i]);
      CHECK_MSG(near(Rigged::reference(other), 0.0, 1e-12), "without 0x%04X", (unsigned)needed[i]);
    }
    SimSunSaver odd;
    makeLiFePO4(odd);
    odd.regs[0xE001] = odd.setVolts(9.50);
    CHECK(near(Rigged::reference(odd), 0.0, 1e-12));
    odd.regs[0xE001] = 0xFFFF;
    CHECK(near(Rigged::reference(odd), 0.0, 1e-12));
    SimSunSaver unproven;
    makeLiFePO4(unproven);
    for (uint8_t i = 0; i < ss::SETTING_ITEMS; ++i) {
      if ((ss::item(i).flags & ss::ITEM_WITNESS) != 0 && unproven.regs[ss::item(i).addr] != 0) {
        unproven.regs[ss::item(i).addr] = (uint16_t)(unproven.regs[ss::item(i).addr] + 7);
      }
    }
    CHECK(near(Rigged::reference(unproven), 0.0, 1e-12));
  }
  {
    // The charger regulates to no more than its ceiling (Evb_ref_lim), so a float voltage above
    // it holds the battery at the ceiling, as does an absorption voltage in a bank without float.
    // 0 is no ceiling; one that was not read could be anywhere. With the float at 13.50 V and the
    // ceiling at 13.00 V, a reference of 13.50 V would raise the no-charge alert for a battery
    // that the charger holds at 13.00 V as it should.
    SimSunSaver sim(96.667, 100.0);
    makeLiFePO4(sim);
    CHECK_EQ(sim.regs[0xE01D], sim.setVolts(15.00));
    CHECK(near(Rigged::reference(sim), 13.50, 0.01));
    sim.regs[0xE01D] = sim.setVolts(13.00);
    CHECK(near(Rigged::reference(sim), 13.00, 0.01));
    sim.regs[0xE01D] = sim.setVolts(13.50);
    CHECK(near(Rigged::reference(sim), 13.50, 0.01));
    // A bank under the ceiling keeps its own voltage, and the lower bank counts.
    sim.regs[0xE00E] = sim.setVolts(13.30);
    sim.regs[0xE01D] = sim.setVolts(13.40);
    CHECK(near(Rigged::reference(sim), 13.30, 0.01));
    sim.regs[0xE00E] = 0;
    sim.regs[0xE001] = 0;
    CHECK(near(Rigged::reference(sim), 13.40, 0.01));  // both banks held at 14.20 V, under 13.40 V
    sim.regs[0xE01D] = 0;
    CHECK(near(Rigged::reference(sim), 14.20, 0.01));
    sim.regs[0xE01D] = sim.setVolts(9.50);  // below any voltage a battery is held at
    CHECK(near(Rigged::reference(sim), 0.0, 1e-12));
    sim.regs.erase(0xE01D);
    CHECK(near(Rigged::reference(sim), 0.0, 1e-12));
    // The ceiling is a stored figure: a 24 V system doubles it with the rest.
    sim.regs[0xE01D] = sim.setVolts(13.40);
    sim.regs[0x0008] = sim.volts(27.00);
    sim.regs[0x001C] = sim.volts(26.20);
    CHECK(near(Rigged::reference(sim, ss::SCALE_96667, 24), 26.80, 0.02));
  }
  {
    // Only the low byte of EV_tempcomp is used: a word whose low byte is 0 is compensation off.
    SimSunSaver sim(96.667, 100.0);
    makeLiFePO4(sim);
    sim.regs[0xE01A] = 0x0100;
    CHECK(near(Rigged::reference(sim), 13.50, 0.01));
    sim.regs[0xE01A] = 0x0114;
    CHECK(near(Rigged::reference(sim), 0.0, 1e-12));
  }
  {
    // A write to the EEPROM, and no restart yet: the controller charges by the settings it
    // started with until it restarts, so the stored ones say nothing about where it holds the
    // battery. A float voltage raised from 13.30 V to 13.50 V would otherwise make a battery
    // held at 13.30 V look as if it were not charged.
    Bare b;
    makeLiFePO4(b.sim);
    b.sim.regs[0xE001] = b.sim.setVolts(13.30);
    b.sim.regs[0xE00E] = b.sim.setVolts(13.30);
    CHECK(near(Rigged::reference(b.sim, ss::SCALE_100), 13.30, 0.01));
    CHECK_EQ(b.master.writeRegister(1, 0xE001, b.sim.setVolts(13.50)).status, MB_OK);
    CHECK_EQ(b.master.writeRegister(1, 0xE00E, b.sim.setVolts(13.50)).status, MB_OK);
    CHECK(!ss::storedSettingsInEffect(snapshotOf(b.sim)));
    CHECK(near(Rigged::reference(b.sim, ss::SCALE_100), 0.0, 1e-12));
    CHECK(near(Rigged::reference(b.sim, ss::SCALE_96667), 0.0, 1e-12));
    // Either word alone is enough, and so is a word that was not read. Other fault bits are no
    // edit.
    const uint8_t words[2] = {ss::I_ARRAY_FAULT, ss::I_LOAD_FAULT};
    const uint16_t bits[2] = {ss::ARRAY_FAULT_EEPROM_EDIT, ss::LOAD_FAULT_EEPROM_EDIT};
    const ss::Expectation want = {ss::CHEMISTRY_LIFEPO4, 12, ss::SCALE_100};
    for (int w = 0; w < 2; ++w) {
      ss::Snapshot one = snapshotOf(b.sim);
      one.raw[words[1 - w]] = (uint16_t)(one.raw[words[1 - w]] & ~bits[1 - w]);
      CHECK((one.raw[words[w]] & bits[w]) != 0);
      ss::Evaluation evaluation;
      ss::evaluate(one, want, &evaluation);
      CHECK_EQ(evaluation.settingsScale.scale, ss::SCALE_100);
      CHECK(near(ss::floatReferenceVolts(one, evaluation, ss::SCALE_100), 0.0, 1e-12));
      ss::Snapshot other = one;
      other.raw[ss::I_ARRAY_FAULT] = (uint16_t)~ss::ARRAY_FAULT_EEPROM_EDIT;
      other.raw[ss::I_LOAD_FAULT] = (uint16_t)~ss::LOAD_FAULT_EEPROM_EDIT;
      ss::evaluate(other, want, &evaluation);
      CHECK(near(ss::floatReferenceVolts(other, evaluation, ss::SCALE_100), 13.50, 0.01));
      other.status[words[w]] = ss::SSIO_REFUSED;
      ss::evaluate(other, want, &evaluation);
      CHECK(near(ss::floatReferenceVolts(other, evaluation, ss::SCALE_100), 0.0, 1e-12));
    }
    // The restart puts the new settings to work and clears the edit.
    CHECK_EQ(b.master.writeCoil(1, 0x00FF, true).status, MB_OK);
    CHECK_EQ(b.sim.resets, 1);
    CHECK(ss::storedSettingsInEffect(snapshotOf(b.sim)));
    CHECK(near(Rigged::reference(b.sim, ss::SCALE_100), 13.50, 0.01));
  }
}

// ============================================================================
// The report as JSON
// ============================================================================
static void testReport() {
  // From the bus to the server's page, the way the firmware does it. One register that no
  // judgement needs is refused.
  {
    Bare b;
    b.sim.regs.erase(0xE027);
    BusReader bus(b.master);
    ss::SnapshotReader reader;
    reader.start();
    while (!reader.step(bus, 4)) {
    }
    CHECK(reader.done());
    const ss::Expectation want = {ss::CHEMISTRY_LIFEPO4, 12, ss::SCALE_96667};
    ss::Evaluation evaluation;
    ss::evaluate(reader.snapshot(), want, &evaluation);
    CHECK_EQ(evaluation.verdict, ss::VERDICT_MISMATCH);

    JsonDocument note;
    JsonObject body = note.to<JsonObject>();
    body["c"] = "dev:860322068012345";
    body["st"] = "chg-check";
    ss::encodeReport(reader.snapshot(), evaluation, body);
    CHECK(!note.overflowed());
    std::string wire;
    serializeJson(note, wire);
    // The client's note buffer is 2048 bytes, and its other notes share the budget.
    CHECK_MSG(wire.size() < 420, "the report is %zu bytes", wire.size());
    CHECK(plainAscii(wire.c_str()));
    CHECK_HAS(wire, "\"lay\":1");
    CHECK_HAS(wire, "\"nr\":[33]");
    CHECK_HAS(wire, "\"ch\":3");
    CHECK_HAS(wire, "\"vd\":2");
    CHECK_HAS(wire, "\"ss\":1");
    CHECK_HAS(wire, "\"sf\":[13,");
    CHECK_HAS(wire, "\"ls\":1,\"lsrc\":2,\"sv\":12");
    CHECK(body["c"] == "dev:860322068012345");  // what was there stays
    const char *hex = body["r"].as<const char *>();
    CHECK(hex != nullptr && strlen(hex) == ss::REPORT_HEX_CHARS);
    CHECK(hex != nullptr && strncmp(hex, "11EC1189", 8) == 0);  // EV_reg 14.00 V, EV_float 13.70 V

    JsonDocument received;
    CHECK(deserializeJson(received, wire) == DeserializationError::Ok);
    ss::Snapshot snapshot;
    ss::Evaluation decoded;
    CHECK(ss::decodeReport(received.as<JsonObjectConst>(), &snapshot, &decoded));
    CHECK(sameSnapshot(snapshot, reader.snapshot()));
    CHECK_EQ(decoded.chemistry, evaluation.chemistry);
    CHECK_EQ(decoded.verdict, evaluation.verdict);
    CHECK_EQ(decoded.findings, evaluation.findings);
    CHECK_EQ(decoded.settingsScale.scale, evaluation.settingsScale.scale);
    CHECK_EQ(decoded.settingsScale.used, evaluation.settingsScale.used);
    CHECK_EQ(decoded.settingsScale.fits[0], evaluation.settingsScale.fits[0]);
    CHECK_EQ(decoded.settingsScale.fits[1], evaluation.settingsScale.fits[1]);
    CHECK_EQ(decoded.liveScale, evaluation.liveScale);
    CHECK_EQ(decoded.liveSource, evaluation.liveSource);
    CHECK_EQ(decoded.systemVolts, evaluation.systemVolts);
    CHECK_EQ(ss::settingsFingerprint(snapshot, decoded), ss::settingsFingerprint(reader.snapshot(), evaluation));

    // The server keeps the report by encoding it again: nothing is lost on the way.
    JsonDocument kept;
    ss::encodeReport(snapshot, decoded, kept.to<JsonObject>());
    std::string again;
    serializeJson(kept, again);
    JsonDocument first;
    ss::encodeReport(reader.snapshot(), evaluation, first.to<JsonObject>());
    std::string original;
    serializeJson(first, original);
    CHECK(again == original);

    // And for a person.
    JsonDocument page;
    ss::describeReport(snapshot, decoded, page.to<JsonObject>());
    CHECK(!page.overflowed());
    std::string shown;
    serializeJson(page, shown);
    CHECK_MSG(shown.size() < 6000, "the readable report is %zu bytes", shown.size());
    CHECK(plainAscii(shown.c_str()));
    CHECK_EQ(page["vd"].as<int>(), ss::VERDICT_MISMATCH);
    CHECK(page["verdict"] == "mismatch");
    CHECK(page["battery"] == "LiFePO4");
    CHECK_EQ(page["system"].as<int>(), 12);
    CHECK(page["settingsScale"] == "x100 (13 of 13 setpoints are round with it, 1 with x96.667)");
    CHECK(page["liveScale"] == "x100 (the stored LVD reads right with it)");
    JsonArrayConst findings = page["findings"];
    CHECK_EQ(findings.size(), 4);
    if (findings.size() == 4) {
      CHECK(has(findings[0]["t"].as<const char *>(), "DIP switch 1 is OFF") && findings[0]["p"] == true);
      CHECK(has(findings[1]["t"].as<const char *>(), "equalize voltage is stored") && findings[1]["p"] == true);
      CHECK(has(findings[2]["t"].as<const char *>(), "Temperature compensation is on") && findings[2]["p"] == true);
      CHECK(has(findings[3]["t"].as<const char *>(), "off by 3.3 %") && findings[3]["p"] == false);
    }
    JsonArrayConst groups = page["groups"];
    CHECK_EQ(groups.size(), ss::GROUP_COUNT);
    size_t rows = 0;
    for (size_t g = 0; g < groups.size(); ++g) {
      CHECK(groups[g]["title"] == ss::groupTitle((uint8_t)g));
      JsonArrayConst list = groups[g]["rows"];
      for (JsonArrayConst row : list) {
        CHECK_EQ(row.size(), 4);
        const ss::Item &entry = ss::item((uint8_t)rows);
        CHECK(row[0] == entry.name);
        CHECK(row[1] == entry.label);
        CHECK_EQ(entry.group, g);
        ++rows;
      }
    }
    CHECK_EQ(rows, ss::ITEM_COUNT);
    CHECK_HAS(shown, "[\"EV_reg\",\"Absorption voltage at 25 C\",\"14.00 V\",\"0x11EC\"]");
    CHECK_HAS(shown, "[\"EV_eq\",\"Equalize voltage at 25 C\",\"0 (off)\",\"0x0000\"]");
    CHECK_HAS(shown, "[\"ER_icomp\",\"Load current compensation\",\"0 (off)\",\"0x0000\"]");
    CHECK_HAS(shown, "[\"Et_lvd_warn\",\"LVD warning time\",\"no answer\",\"\"]");
    CHECK_HAS(shown, "[\"dip_switch\",\"DIP switches\",\"SW1 OFF, SW2 ON, SW3 OFF, SW4 ON\",\"0x000A\"]");
    CHECK_HAS(shown, "\"13.68 V\"");  // the live voltages use the scale the LVD proved
  }
  {
    // Nothing missing: no "nr" at all. And the scales that were not proven show both readings.
    SimSunSaver sim;
    sim.regs[0x001C] = sim.volts(12.00);
    const ss::Snapshot snapshot = snapshotOf(sim);
    const ss::Expectation want = {ss::CHEMISTRY_NOT_JUDGED, 0, ss::SCALE_UNKNOWN};
    ss::Evaluation evaluation;
    ss::evaluate(snapshot, want, &evaluation);
    JsonDocument note;
    ss::encodeReport(snapshot, evaluation, note.to<JsonObject>());
    CHECK(note["nr"].isNull());
    CHECK_EQ(note["ls"].as<int>(), ss::SCALE_UNKNOWN);
    ss::Snapshot back;
    ss::Evaluation decoded;
    CHECK(ss::decodeReport(note.as<JsonObjectConst>(), &back, &decoded));
    CHECK(sameSnapshot(back, snapshot));
    JsonDocument page;
    ss::describeReport(back, decoded, page.to<JsonObject>());
    std::string shown;
    serializeJson(page, shown);
    CHECK(page["verdict"] == "not judged");
    CHECK(page["liveScale"] == "not proven");
    CHECK_EQ(page["findings"].as<JsonArrayConst>().size(), 0);
    CHECK_HAS(shown, "\"13.68 V if x100, 13.23 V if x96.667\"");
    CHECK_MSG(shown.size() < 6000, "the readable report is %zu bytes", shown.size());
  }

  {
    // A 24 V system with DIP switch 2 OFF: its built-in LVD is 23.00 V, and the page says so.
    SimSunSaver sim;
    const ss::Snapshot snapshot = snapshotOf(sim);
    ss::Evaluation evaluation;
    memset(&evaluation, 0, sizeof(evaluation));
    evaluation.liveScale = ss::SCALE_100;
    evaluation.liveSource = ss::LIVE_SOURCE_PRESET_LVD;
    evaluation.systemVolts = 24;
    evaluation.findings = ss::FINDING_PRESET_LVD;
    JsonDocument note;
    ss::encodeReport(snapshot, evaluation, note.to<JsonObject>());
    ss::Snapshot back;
    ss::Evaluation decoded;
    CHECK(ss::decodeReport(note.as<JsonObjectConst>(), &back, &decoded));
    JsonDocument page;
    ss::describeReport(back, decoded, page.to<JsonObject>());
    CHECK(page["liveScale"] == "x100 (the built-in 23.00 V LVD reads right with it)");
    CHECK_EQ(page["findings"].as<JsonArrayConst>().size(), 1);
    CHECK(has(page["findings"][0]["t"].as<const char *>(), "11.50 V (23.00 V on a 24 V system)"));
    // A report without the system names both.
    note.remove("sv");
    CHECK(ss::decodeReport(note.as<JsonObjectConst>(), &back, &decoded));
    JsonDocument without;
    ss::describeReport(back, decoded, without.to<JsonObject>());
    CHECK(without["liveScale"] == "x100 (the built-in LVD, 11.50 V or 23.00 V on a 24 V system, reads right with it)");
  }

  {
    // A register without a value is sent as 0000 and named in "nr", whatever its word holds.
    SimSunSaver sim;
    ss::Snapshot snapshot = snapshotOf(sim);
    CHECK(snapshot.raw[ss::I_EV_REG] != 0);
    snapshot.status[ss::I_EV_REG] = ss::SSIO_REFUSED;
    const ss::Expectation want = {ss::CHEMISTRY_NOT_JUDGED, 0, ss::SCALE_UNKNOWN};
    ss::Evaluation evaluation;
    ss::evaluate(snapshot, want, &evaluation);
    JsonDocument note;
    ss::encodeReport(snapshot, evaluation, note.to<JsonObject>());
    const char *sent = note["r"].as<const char *>();
    CHECK(sent != nullptr && startsWith(sent, "0000"));
    CHECK_EQ(note["nr"].as<JsonArrayConst>().size(), 1);
    CHECK_EQ(note["nr"][0].as<int>(), ss::I_EV_REG);
  }

  // Reports from other versions, and input that is no report.
  {
    SimSunSaver sim;
    const ss::Snapshot snapshot = snapshotOf(sim);
    const ss::Expectation want = {ss::CHEMISTRY_LIFEPO4, 12, ss::SCALE_UNKNOWN};
    ss::Evaluation evaluation;
    ss::evaluate(snapshot, want, &evaluation);
    JsonDocument good;
    ss::encodeReport(snapshot, evaluation, good.to<JsonObject>());
    const std::string hex = good["r"].as<const char *>();
    ss::Snapshot back;
    ss::Evaluation decoded;

    // An older client that knew fewer registers: the rest counts as not read, and is not shown.
    {
      JsonDocument doc = good;
      doc["r"] = hex.substr(0, 40u * 4u);
      CHECK(ss::decodeReport(doc.as<JsonObjectConst>(), &back, &decoded));
      CHECK(ss::have(back, 39));
      CHECK_EQ(back.status[40], ss::SSIO_NOT_READ);
      CHECK_EQ(back.status[ss::I_DIP_SWITCH], ss::SSIO_NOT_READ);
      JsonDocument page;
      ss::describeReport(back, decoded, page.to<JsonObject>());
      size_t rows = 0;
      for (JsonObjectConst group : page["groups"].as<JsonArrayConst>()) {
        rows += group["rows"].as<JsonArrayConst>().size();
      }
      CHECK_EQ(rows, 40);
      doc["r"] = hex.substr(0, 4);
      CHECK(ss::decodeReport(doc.as<JsonObjectConst>(), &back, &decoded));
      CHECK(ss::have(back, 0) && !ss::have(back, 1));
      JsonDocument small;
      ss::describeReport(back, decoded, small.to<JsonObject>());
      CHECK_EQ(small["groups"].as<JsonArrayConst>().size(), 1);  // a group with no rows is left out
    }
    // A newer client that knows more: what this code knows is read, the rest ignored.
    {
      JsonDocument doc = good;
      doc["r"] = hex + "ABCD1234zzzz";
      JsonArray missing = doc["nr"].to<JsonArray>();
      missing.add(50);
      missing.add(2);
      CHECK(ss::decodeReport(doc.as<JsonObjectConst>(), &back, &decoded));
      CHECK_EQ(back.status[2], ss::SSIO_REFUSED);
      back.status[2] = ss::SSIO_OK;
      back.raw[2] = snapshot.raw[2];
      CHECK(sameSnapshot(back, snapshot));
    }
    // Lower-case digits are digits.
    {
      JsonDocument doc = good;
      std::string lower = hex;
      for (size_t i = 0; i < lower.size(); ++i) {
        lower[i] = (char)tolower((unsigned char)lower[i]);
      }
      doc["r"] = lower;
      CHECK(ss::decodeReport(doc.as<JsonObjectConst>(), &back, &decoded));
      CHECK(sameSnapshot(back, snapshot));
    }
    // What is no report leaves nothing behind.
    const char *const bad[] = {
        "{}",
        "[]",
        "null",
        "{\"lay\":1}",
        "{\"lay\":2,\"r\":\"11EC\"}",
        "{\"lay\":\"1\",\"r\":\"11EC\"}",
        "{\"lay\":-1,\"r\":\"11EC\"}",
        "{\"lay\":1.5,\"r\":\"11EC\"}",
        "{\"lay\":1,\"r\":\"\"}",
        "{\"lay\":1,\"r\":\"11E\"}",
        "{\"lay\":1,\"r\":\"11EC1\"}",
        "{\"lay\":1,\"r\":\"11EG\"}",
        "{\"lay\":1,\"r\":\"11EC 189\"}",
        "{\"lay\":1,\"r\":4588}",
        "{\"lay\":1,\"r\":[\"11EC\"]}",
        "{\"r\":\"11EC\"}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
      JsonDocument doc;
      CHECK(deserializeJson(doc, bad[i]) == DeserializationError::Ok);
      back.status[0] = ss::SSIO_OK;
      decoded.verdict = ss::VERDICT_OK;
      CHECK_MSG(!ss::decodeReport(doc.as<JsonObjectConst>(), &back, &decoded), "accepted %s", bad[i]);
      CHECK_EQ(back.status[0], ss::SSIO_NOT_READ);
      CHECK_EQ(decoded.verdict, ss::VERDICT_NOT_JUDGED);
    }
    {
      JsonDocument doc = good;
      std::string broken = hex;
      broken[broken.size() - 1] = 'x';  // the very last digit
      doc["r"] = broken;
      CHECK(!ss::decodeReport(doc.as<JsonObjectConst>(), &back, &decoded));
      CHECK_EQ(back.status[0], ss::SSIO_NOT_READ);
    }
    // The server reads its file of kept reports into a buffer, parses that and frees it before
    // it reads the reports out of the document (loadChargerReports). That is sound because a
    // document owns a copy of every string: ArduinoJson 7 has no zero-copy mode, also for a
    // buffer it may write to. Should that ever change, the address sanitizer build of this test
    // reads freed memory here.
    {
      SimSunSaver sim;
      makeLiFePO4(sim);
      sim.regs.erase(0xE026);
      const ss::Snapshot kept = snapshotOf(sim);
      const ss::Expectation want = {ss::CHEMISTRY_LIFEPO4, 12, ss::SCALE_96667};
      ss::Evaluation keptEvaluation;
      ss::evaluate(kept, want, &keptEvaluation);
      JsonDocument file;
      JsonArray array = file.to<JsonArray>();
      for (int i = 0; i < 3; ++i) {
        JsonObject entry = array.add<JsonObject>();
        entry["c"] = i == 0 ? "dev:860322068012345" : i == 1 ? "dev:860322068099999" : "dev:1";
        entry["e"] = 1795000000u + (unsigned)i;
        ss::encodeReport(kept, keptEvaluation, entry);
      }
      std::string text;
      serializeJson(file, text);
      char *buffer = (char *)malloc(text.size() + 1);
      CHECK(buffer != nullptr);
      if (buffer != nullptr) {
        memcpy(buffer, text.c_str(), text.size() + 1);
        JsonDocument doc;
        const DeserializationError error = deserializeJson(doc, buffer);  // a char *, as in the server
        memset(buffer, '#', text.size());
        free(buffer);
        CHECK(error == DeserializationError::Ok);
        int read = 0;
        for (JsonObjectConst entry : doc.as<JsonArrayConst>()) {
          const char *client = entry["c"] | "";
          CHECK(startsWith(client, "dev:"));
          CHECK_EQ(strlen(client), read == 2 ? 5 : 19);
          CHECK_EQ(entry["e"] | 0u, 1795000000u + (unsigned)read);
          ss::Snapshot loaded;
          ss::Evaluation loadedEvaluation;
          CHECK(ss::decodeReport(entry, &loaded, &loadedEvaluation));
          CHECK(sameSnapshot(loaded, kept));
          CHECK_EQ(loadedEvaluation.findings, keptEvaluation.findings);
          CHECK_EQ(ss::settingsFingerprint(loaded, loadedEvaluation), ss::settingsFingerprint(kept, keptEvaluation));
          ++read;
        }
        CHECK_EQ(read, 3);
      }
    }
    // A report whose other members are odd is read for what it holds.
    {
      JsonDocument doc;
      CHECK(deserializeJson(doc, "{\"lay\":1,\"r\":\"11EC1189\",\"nr\":[1,-1,99,\"0\",1.5,[0]],\"ch\":9,\"vd\":7,"
                                 "\"fd\":\"x\",\"ss\":3,\"sf\":[1,2],\"ls\":2,\"lsrc\":0,\"sv\":13}") ==
            DeserializationError::Ok);
      CHECK(ss::decodeReport(doc.as<JsonObjectConst>(), &back, &decoded));
      CHECK(ss::have(back, 0));
      CHECK_EQ(back.raw[0], 0x11EC);
      CHECK_EQ(back.status[1], ss::SSIO_REFUSED);
      CHECK_EQ(back.raw[1], 0);
      CHECK_EQ(back.status[2], ss::SSIO_NOT_READ);
      CHECK_EQ(decoded.chemistry, ss::CHEMISTRY_NOT_JUDGED);
      CHECK_EQ(decoded.verdict, ss::VERDICT_NOT_JUDGED);
      CHECK_EQ(decoded.findings, 0);
      CHECK_EQ(decoded.settingsScale.scale, ss::SCALE_UNKNOWN);
      CHECK_EQ(decoded.settingsScale.used, 0);  // "sf" needs its three numbers
      CHECK_EQ(decoded.settingsScale.fits[0], 0);
      CHECK_EQ(decoded.settingsScale.fits[1], 0);
      CHECK_EQ(decoded.liveScale, ss::SCALE_UNKNOWN);  // a scale with nothing that proved it is no verdict
      CHECK_EQ(decoded.liveSource, ss::LIVE_SOURCE_NONE);
      CHECK_EQ(decoded.systemVolts, 0);
      JsonDocument page;
      ss::describeReport(back, decoded, page.to<JsonObject>());
      CHECK(page["verdict"] == "not judged");
    }
    {
      JsonDocument doc;
      CHECK(deserializeJson(doc, "{\"lay\":1,\"r\":\"11EC\",\"ch\":3,\"vd\":3,\"fd\":4294967295,\"ss\":2,"
                                 "\"sf\":[200,3,14],\"ls\":1,\"lsrc\":1,\"sv\":24}") == DeserializationError::Ok);
      CHECK(ss::decodeReport(doc.as<JsonObjectConst>(), &back, &decoded));
      CHECK_EQ(decoded.chemistry, ss::CHEMISTRY_LIFEPO4);
      CHECK_EQ(decoded.verdict, ss::VERDICT_CANNOT_TELL);
      CHECK_EQ(decoded.findings, 0xFFFFFFFFul);
      CHECK_EQ(decoded.settingsScale.scale, ss::SCALE_96667);
      CHECK_EQ(decoded.settingsScale.fits[0], 0);  // more fits than there are registers: not a count
      CHECK_EQ(decoded.settingsScale.fits[1], 3);
      CHECK_EQ(decoded.settingsScale.used, 14);
      CHECK_EQ(decoded.liveScale, ss::SCALE_100);
      CHECK_EQ(decoded.liveSource, ss::LIVE_SOURCE_PRESET_LVD);
      CHECK_EQ(decoded.systemVolts, 24);
      // Findings from a newer client, which this code has no sentence for, are still listed.
      JsonDocument page;
      ss::describeReport(back, decoded, page.to<JsonObject>());
      JsonArrayConst findings = page["findings"];
      CHECK_EQ(findings.size(), 32);
      CHECK(findings.size() == 32 && has(findings[31]["t"].as<const char *>(), "no text for"));
    }
  }
}

// ============================================================================
// Which report the server keeps
// ============================================================================
static void testReportOrder() {
  const double now = 1795478400.0;  // 2026-11-24
  // The client's time is taken as it is, to the second.
  CHECK(ss::reportCheckTime(now - 3600.0, now) == now - 3600.0);
  CHECK(ss::reportCheckTime(now, now) == now);
  CHECK(ss::reportCheckTime(now - 400.0 * 86400.0, now) == now - 400.0 * 86400.0);  // a year in flash
  CHECK(ss::reportCheckTime(ss::REPORT_EARLIEST_EPOCH, now) == ss::REPORT_EARLIEST_EPOCH);
  CHECK(ss::reportCheckTime(1600000000.0, now) == 1600000000.0);  // 2020-09-13, written out
  CHECK(ss::reportCheckTime(1599999999.0, now) == 0.0);
  // A clock that was never set, or a value that is no time at all: not known.
  CHECK(ss::reportCheckTime(0.0, now) == 0.0);
  CHECK(ss::reportCheckTime(-5.0, now) == 0.0);
  CHECK(ss::reportCheckTime(86400.0, now) == 0.0);
  CHECK(ss::reportCheckTime(ss::REPORT_EARLIEST_EPOCH - 1.0, now) == 0.0);
  CHECK(ss::reportCheckTime(std::nan(""), now) == 0.0);
  // A clock that runs ahead of the server's: up to a day is taken, more is not known.
  CHECK(ss::reportCheckTime(now + 86400.0, now) == now + 86400.0);
  CHECK(ss::reportCheckTime(now + 86401.0, now) == 0.0);
  CHECK(ss::reportCheckTime(4102444800.0, now) == 0.0);  // 2100
  // The time a report arrived at never stands in for the time of its check.
  CHECK(ss::reportCheckTime(0.0, now) != now);
  // A server that does not know the time cannot call a client's clock wrong.
  CHECK(ss::reportCheckTime(now, 0.0) == now);
  CHECK(ss::reportCheckTime(now + 10.0 * 86400.0, 0.0) == now + 10.0 * 86400.0);
  CHECK(ss::reportCheckTime(now, 1000.0) == now);
  CHECK(ss::reportCheckTime(0.0, 0.0) == 0.0);
  // But one that the server could not keep is not known whether or not it knows the time: the
  // server keeps and serves the time as a uint32_t, and an infinite or huge time would rank above
  // every report after it.
  CHECK(ss::REPORT_LATEST_EPOCH == 4294967295.0);  // 2106-02-07, the last second a uint32_t holds
  for (double server : {0.0, 1000.0}) {
    CHECK(ss::reportCheckTime(ss::REPORT_LATEST_EPOCH, server) == ss::REPORT_LATEST_EPOCH);
    CHECK(ss::reportCheckTime(4294967295.0, server) == 4294967295.0);
    CHECK(ss::reportCheckTime(4294967295.5, server) == 0.0);
    CHECK(ss::reportCheckTime(4294967296.0, server) == 0.0);
    CHECK(ss::reportCheckTime(1e10, server) == 0.0);
    CHECK(ss::reportCheckTime(1e100, server) == 0.0);
    CHECK(ss::reportCheckTime(HUGE_VAL, server) == 0.0);
    CHECK(ss::reportCheckTime(-HUGE_VAL, server) == 0.0);
    CHECK(ss::reportCheckTime(std::nan(""), server) == 0.0);
  }
  CHECK(ss::reportCheckTime(1e100, now) == 0.0);
  CHECK(ss::reportCheckTime(HUGE_VAL, now) == 0.0);
  // What fits is what the server can keep: the cast back gives the same time.
  CHECK((double)(uint32_t)ss::reportCheckTime(ss::REPORT_LATEST_EPOCH, 0.0) == ss::REPORT_LATEST_EPOCH);
  // The server judges the time of the report it holds again, by its clock now. One that came
  // while it did not know the time, ten days ahead, is then not known, and the next report
  // replaces it. Judged only as it was kept, it would rank above every report for ten days.
  {
    const double held = ss::reportCheckTime(now + 10.0 * 86400.0, 0.0);
    CHECK(held == now + 10.0 * 86400.0);
    CHECK(!ss::reportReplaces(true, held, now + 60.0));
    CHECK(ss::reportCheckTime(held, now) == 0.0);
    CHECK(ss::reportReplaces(true, ss::reportCheckTime(held, now), now + 60.0));
    // A report held with a right time is judged the same again, and keeps its place.
    CHECK(ss::reportCheckTime(now - 60.0, now + 3600.0) == now - 60.0);
    CHECK(!ss::reportReplaces(true, ss::reportCheckTime(now - 60.0, now + 3600.0), now - 120.0));
  }

  // The first report is kept whatever its time.
  CHECK(ss::reportReplaces(false, 0.0, 0.0));
  CHECK(ss::reportReplaces(false, 0.0, now));
  CHECK(ss::reportReplaces(false, now, 0.0));  // the time of a report that is not there means nothing
  // A newer one replaces an older one, and not the other way round.
  CHECK(ss::reportReplaces(true, now - 60.0, now));
  CHECK(!ss::reportReplaces(true, now, now - 60.0));
  CHECK(!ss::reportReplaces(true, now, now - 1.0));
  CHECK(ss::reportReplaces(true, now, now));  // the same report again: nothing is lost by it
  // One without a time never replaces one that has a time, and is replaced by it.
  CHECK(!ss::reportReplaces(true, now, 0.0));
  CHECK(!ss::reportReplaces(true, ss::REPORT_EARLIEST_EPOCH, 0.0));
  CHECK(ss::reportReplaces(true, 0.0, now));
  CHECK(ss::reportReplaces(true, 0.0, 0.0));  // of two without a time, the one that arrived last

  // The case the rule is for. The client checks at 10:00 while its Notecard does not answer:
  // the report goes into the flash buffer. At 10:20 it checks again after a config change, and
  // that report gets through. The buffer is sent after it, so the 10:00 report arrives last.
  {
    const double first = ss::reportCheckTime(now, now + 1800.0);            // arrives at 10:30
    const double second = ss::reportCheckTime(now + 1200.0, now + 1205.0);  // arrives at 10:20
    bool have = false;
    double kept = 0.0;
    if (ss::reportReplaces(have, kept, second)) {
      have = true;
      kept = second;
    }
    CHECK(!ss::reportReplaces(have, kept, first));
    // In the order they were made, each is kept in turn.
    CHECK(ss::reportReplaces(false, 0.0, first));
    CHECK(ss::reportReplaces(true, first, second));
  }
}

int main() {
  startTestTimeLimit();
  testTableAgainstProfile();
  testValuesAgainstOptaView();
  testText();
  testReader();
  testSettingsScale();
  testLiveScale();
  testEvaluate();
  testFingerprint();
  testFloatReference();
  testReport();
  testReportOrder();
  CHECK_EQ(gAllocLive, 0);

  if (gFailures != 0) {
    fprintf(stderr, "sunsaver_settings: %ld of %ld checks FAILED\n", gFailures, gChecks);
    return 1;
  }
  printf("sunsaver_settings: all %ld checks passed\n", gChecks);
  return 0;
}
