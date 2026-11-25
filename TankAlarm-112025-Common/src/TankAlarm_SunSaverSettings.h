/**
 * TankAlarm_SunSaverSettings.h
 *
 * What a Morningstar SunSaver MPPT has stored (its charging profile, LVD and LVR) and how to
 * judge it: the registers to read, the test that proves how the controller encodes voltages,
 * the DIP switches, and whether the settings suit the battery that was selected for the site.
 *
 * Nothing here writes to the controller. Registers are read through the one-method Reader
 * interface, so the same code runs in the client (ArduinoModbus, TankAlarm_Solar.cpp) and in the
 * host tests against the simulated SunSaver (tests/host/sunsaver_settings). There is no Arduino
 * dependency, no heap, no printf and no exception, and the code keeps to C++14.
 *
 * Where the facts come from
 * -------------------------
 * Addresses, scales and bit meanings are those of the Morningstar SunSaver MPPT MODBUS
 * Specification v11 (3 April 2017), as written out in OptaView/profiles/sunsaver-mppt.json. A
 * host test compares the table below with that profile, so the two cannot drift apart. On our
 * bench only 0x0008 to 0x000C have been confirmed so far (2026-04-22 and 2026-07-03).
 *
 * The voltage scale
 * -----------------
 * The specification gives n * 100 / 32768 for voltages. The TankAlarm client has always decoded
 * the battery voltage with n * 96.667 / 32768, and which of the two a controller really uses
 * was never settled on the bench. A setting that is decoded with the wrong one is off by 3.3 %,
 * which is 0.45 V at 13.5 V. So nothing here assumes a scale:
 *
 *   Stored settings: people and factories choose round numbers (14.00, 13.70, 11.50). Decoded
 *     with the right scale the setpoints land on a 0.05 V grid, with the wrong one they do not.
 *     judgeSettingsScale() counts both ways and names a scale only when the count is clear. It
 *     is the test OptaView's 'scalecheck' runs, and a host test holds the two to the same verdict.
 *
 *   Live readings: the LVD in effect (V_lvd) is a live reading of a known value. With DIP switch
 *     2 OFF it is the built-in 11.50 V, with it ON it is the stored EV_lvd. judgeLiveScale()
 *     looks for the one scale that makes the reading come out as that value.
 *
 * Copyright (c) 2025-2026 Senax Inc. All rights reserved.
 */

#ifndef TANKALARM_SUNSAVER_SETTINGS_H
#define TANKALARM_SUNSAVER_SETTINGS_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "TankAlarm_Battery.h"

namespace sunsaver {

// ---------------------------------------------------------------------------------------------
// Reading a register
// ---------------------------------------------------------------------------------------------

enum IoStatus : uint8_t {
  SSIO_OK = 0,
  SSIO_REFUSED = 1,   // the controller answered with a Modbus exception
  SSIO_NO_REPLY = 2,  // nothing usable came back
  SSIO_NOT_READ = 3   // not asked
};

class Reader {
 public:
  virtual ~Reader() {}
  // Reads one register, in one request. *raw is only meaningful with SSIO_OK.
  virtual IoStatus readRegister(uint16_t addr, uint16_t *raw) = 0;
};

// ---------------------------------------------------------------------------------------------
// Scales
// ---------------------------------------------------------------------------------------------

enum ScaleId : uint8_t {
  SCALE_UNKNOWN = 0,
  SCALE_100 = 1,    // n * 100 / 32768, the specification's figure
  SCALE_96667 = 2   // n * 96.667 / 32768, the figure the TankAlarm client decodes readings with
};

static const uint8_t SCALE_CANDIDATES = 2;
static const double VOLTS_DENOMINATOR = 32768.0;
static const double SETPOINT_GRID_VOLTS = 0.05;

// The numerator of a scale, or 0 for SCALE_UNKNOWN.
inline double scaleNumerator(uint8_t scale) {
  switch (scale) {
    case SCALE_100: return 100.0;
    case SCALE_96667: return 96.667;
    default: return 0.0;
  }
}

// The candidates in the order they are tried: index 0 is SCALE_100, index 1 is SCALE_96667.
inline uint8_t candidateScale(uint8_t index) { return index == 0 ? (uint8_t)SCALE_100 : (uint8_t)SCALE_96667; }

inline const char *scaleName(uint8_t scale) {
  switch (scale) {
    case SCALE_100: return "x100";
    case SCALE_96667: return "x96.667";
    default: return "not proven";
  }
}

inline double voltsFromRaw(uint16_t raw, uint8_t scale) {
  return (double)raw * scaleNumerator(scale) / VOLTS_DENOMINATOR;
}

inline double ohmsFromRaw(uint16_t raw) { return (double)raw * 1.263 / 65536.0; }

// EV_tempcomp: the controller uses only its low byte, with the sign implied negative, so a word
// whose low byte is 0 is compensation off, whatever its high byte holds.
inline uint16_t tempcompWord(uint16_t raw) { return (uint16_t)(raw & 0x00FFu); }

// Distance from value to the nearest multiple of grid.
inline double gridDistance(double value, double grid) {
  const double nearest = floor(value / grid + 0.5) * grid;
  return value > nearest ? value - nearest : nearest - value;
}

// ---------------------------------------------------------------------------------------------
// The registers
// ---------------------------------------------------------------------------------------------

enum Kind : uint8_t {
  KIND_VOLT_SETTING = 0,  // a stored voltage, in the settings scale
  KIND_VOLT_LIVE,         // a measured voltage, in the live scale
  KIND_VOLT_TARGET,       // Vb_ref: n * 96.667 / 32768 in the specification
  KIND_AMPS,              // signed, n * 79.16 / 32768
  KIND_SECONDS,
  KIND_TENTHS,            // tenths of a second
  KIND_DAYS,
  KIND_TEMPCO,            // volts per degree C: n * 100 / 65536
  KIND_OHMS,              // n * 1.263 / 65536
  KIND_CELSIUS,           // signed low byte
  KIND_CHARGE_STATE,
  KIND_LOAD_STATE,
  KIND_ARRAY_FAULT,       // bits
  KIND_LOAD_FAULT,        // bits
  KIND_DIP                // bits: the four DIP switches
};

enum Group : uint8_t {
  GROUP_BANK1 = 0,  // custom charge settings, battery jumper in
  GROUP_BANK2,      // custom charge settings, battery jumper out
  GROUP_CHARGE,     // charge settings shared by both banks
  GROUP_LOAD,       // LVD, LVR and the load output
  GROUP_LED,        // battery LED thresholds
  GROUP_STATUS,     // what the controller measures and does now
  GROUP_COUNT
};

inline const char *groupTitle(uint8_t group) {
  switch (group) {
    case GROUP_BANK1: return "Charge settings, bank 1";
    case GROUP_BANK2: return "Charge settings, bank 2";
    case GROUP_CHARGE: return "Charge settings, both banks";
    case GROUP_LOAD: return "Load and LVD settings";
    case GROUP_LED: return "Battery LED thresholds";
    case GROUP_STATUS: return "Status";
    default: return "?";
  }
}

enum ItemFlag : uint8_t {
  ITEM_ZERO_IS_OFF = 0x01,  // 0 switches the feature off (not a reconnect: the battery never falls to 0 V)
  ITEM_WITNESS = 0x02       // a setpoint the scale test looks at
};

// Everything the check reads, in the order it is read and reported. The order is part of the
// report format (REPORT_LAYOUT): add new items at the end, and never move or remove one.
enum ItemIndex : uint8_t {
  I_EV_REG = 0,
  I_EV_FLOAT,
  I_ET_FLOAT,
  I_ET_FLOATLB,
  I_EV_FLOATLB_TRIP,
  I_EV_FLOAT_CANCEL,
  I_ET_FLOAT_EXIT_CUM,
  I_EV_EQ,
  I_ET_EQCALENDAR,
  I_ET_EQ_ABOVE,
  I_ET_EQ_REG,
  I_EV_REG2,
  I_EV_FLOAT2,
  I_ET_FLOAT2,
  I_ET_FLOATLB2,
  I_EV_FLOATLB_TRIP2,
  I_EV_FLOAT_CANCEL2,
  I_ET_FLOAT_EXIT_CUM2,
  I_EV_EQ2,
  I_ET_EQCALENDAR2,
  I_ET_EQ_ABOVE2,
  I_ET_EQ_REG2,
  I_EV_TEMPCOMP,
  I_EV_HVD,
  I_EV_HVR,
  I_EVB_REF_LIM,
  I_ETB_MAX,
  I_ETB_MIN,
  I_EV_LVD,
  I_EV_LVR,
  I_EV_LHVD,
  I_EV_LHVR,
  I_ER_ICOMP,
  I_ET_LVD_WARN,
  I_EV_SOC_Y2G,
  I_EV_SOC_G2Y,
  I_EV_SOC_Y2R0,
  I_EV_SOC_R2Y,
  I_ADC_VB_F,
  I_ADC_IL_F,
  I_T_BATT,
  I_T_RTS,
  I_CHARGE_STATE,
  I_ARRAY_FAULT,
  I_VB_REF,
  I_LOAD_STATE,
  I_LOAD_FAULT,
  I_V_LVD,
  I_DIP_SWITCH,
  ITEM_COUNT
};

static const uint8_t REPORT_LAYOUT = 1;                      // names the order above
static const uint8_t SETTING_ITEMS = (uint8_t)I_ADC_VB_F;    // items below this one are EEPROM settings

struct Item {
  uint16_t addr;
  uint8_t kind;      // Kind
  uint8_t group;     // Group
  uint8_t loVolts;   // a voltage setting that somebody chose lies in loVolts..hiVolts
  uint8_t hiVolts;
  uint8_t flags;     // ItemFlag
  const char *name;  // the specification's name
  const char *label;
};

inline const Item &item(uint8_t index) {
  static const Item kItems[ITEM_COUNT] = {
      {0xE000, KIND_VOLT_SETTING, GROUP_BANK1, 10, 17, ITEM_WITNESS, "EV_reg", "Absorption voltage at 25 C"},
      {0xE001, KIND_VOLT_SETTING, GROUP_BANK1, 10, 17, ITEM_WITNESS | ITEM_ZERO_IS_OFF, "EV_float",
       "Float voltage at 25 C"},
      {0xE002, KIND_SECONDS, GROUP_BANK1, 0, 0, 0, "Et_float", "Absorption time before float"},
      {0xE003, KIND_SECONDS, GROUP_BANK1, 0, 0, 0, "Et_floatlb", "Absorption time before float after a low battery"},
      {0xE004, KIND_VOLT_SETTING, GROUP_BANK1, 9, 17, ITEM_ZERO_IS_OFF, "EV_floatlb_trip",
       "Low-battery voltage that selects the longer absorption"},
      {0xE005, KIND_VOLT_SETTING, GROUP_BANK1, 9, 17, ITEM_ZERO_IS_OFF, "EV_float_cancel",
       "Voltage that cancels float"},
      {0xE006, KIND_SECONDS, GROUP_BANK1, 0, 0, 0, "Et_float_exit_cum", "Time below float that exits float"},
      {0xE007, KIND_VOLT_SETTING, GROUP_BANK1, 10, 17, ITEM_WITNESS | ITEM_ZERO_IS_OFF, "EV_eq",
       "Equalize voltage at 25 C"},
      {0xE008, KIND_DAYS, GROUP_BANK1, 0, 0, 0, "Et_eqcalendar", "Days between equalizations"},
      {0xE009, KIND_SECONDS, GROUP_BANK1, 0, 0, 0, "Et_eq_above",
       "Equalize time limit above the absorption voltage"},
      {0xE00A, KIND_SECONDS, GROUP_BANK1, 0, 0, 0, "Et_eq_reg", "Equalize time limit at the equalize voltage"},

      {0xE00D, KIND_VOLT_SETTING, GROUP_BANK2, 10, 17, ITEM_WITNESS, "EV_reg2", "Absorption voltage at 25 C"},
      {0xE00E, KIND_VOLT_SETTING, GROUP_BANK2, 10, 17, ITEM_WITNESS | ITEM_ZERO_IS_OFF, "EV_float2",
       "Float voltage at 25 C"},
      {0xE00F, KIND_SECONDS, GROUP_BANK2, 0, 0, 0, "Et_float2", "Absorption time before float"},
      {0xE010, KIND_SECONDS, GROUP_BANK2, 0, 0, 0, "Et_floatlb2", "Absorption time before float after a low battery"},
      {0xE011, KIND_VOLT_SETTING, GROUP_BANK2, 9, 17, ITEM_ZERO_IS_OFF, "EV_floatlb_trip2",
       "Low-battery voltage that selects the longer absorption"},
      {0xE012, KIND_VOLT_SETTING, GROUP_BANK2, 9, 17, ITEM_ZERO_IS_OFF, "EV_float_cancel2",
       "Voltage that cancels float"},
      {0xE013, KIND_SECONDS, GROUP_BANK2, 0, 0, 0, "Et_float_exit_cum2", "Time below float that exits float"},
      {0xE014, KIND_VOLT_SETTING, GROUP_BANK2, 10, 17, ITEM_WITNESS | ITEM_ZERO_IS_OFF, "EV_eq2",
       "Equalize voltage at 25 C"},
      {0xE015, KIND_DAYS, GROUP_BANK2, 0, 0, 0, "Et_eqcalendar2", "Days between equalizations"},
      {0xE016, KIND_SECONDS, GROUP_BANK2, 0, 0, 0, "Et_eq_above2",
       "Equalize time limit above the absorption voltage"},
      {0xE017, KIND_SECONDS, GROUP_BANK2, 0, 0, 0, "Et_eq_reg2", "Equalize time limit at the equalize voltage"},

      {0xE01A, KIND_TEMPCO, GROUP_CHARGE, 0, 0, ITEM_ZERO_IS_OFF, "EV_tempcomp",
       "Temperature compensation per C"},
      {0xE01B, KIND_VOLT_SETTING, GROUP_CHARGE, 10, 18, ITEM_WITNESS | ITEM_ZERO_IS_OFF, "EV_hvd",
       "Battery high voltage disconnect at 25 C"},
      {0xE01C, KIND_VOLT_SETTING, GROUP_CHARGE, 10, 18, ITEM_WITNESS, "EV_hvr",
       "Battery high voltage reconnect"},
      {0xE01D, KIND_VOLT_SETTING, GROUP_CHARGE, 10, 18, ITEM_ZERO_IS_OFF, "Evb_ref_lim",
       "Regulation ceiling, not compensated"},
      {0xE01E, KIND_CELSIUS, GROUP_CHARGE, 0, 0, 0, "ETb_max", "Highest temperature used for compensation"},
      {0xE01F, KIND_CELSIUS, GROUP_CHARGE, 0, 0, 0, "ETb_min", "Lowest temperature used for compensation"},

      {0xE022, KIND_VOLT_SETTING, GROUP_LOAD, 9, 15, ITEM_WITNESS, "EV_lvd", "Low voltage disconnect"},
      {0xE023, KIND_VOLT_SETTING, GROUP_LOAD, 9, 15, ITEM_WITNESS, "EV_lvr", "Low voltage reconnect"},
      {0xE024, KIND_VOLT_SETTING, GROUP_LOAD, 10, 18, ITEM_ZERO_IS_OFF, "EV_lhvd", "Load high voltage disconnect"},
      {0xE025, KIND_VOLT_SETTING, GROUP_LOAD, 10, 18, 0, "EV_lhvr", "Load high voltage reconnect"},
      {0xE026, KIND_OHMS, GROUP_LOAD, 0, 0, ITEM_ZERO_IS_OFF, "ER_icomp", "Load current compensation"},
      {0xE027, KIND_TENTHS, GROUP_LOAD, 0, 0, 0, "Et_lvd_warn", "LVD warning time"},

      {0xE030, KIND_VOLT_SETTING, GROUP_LED, 9, 15, ITEM_WITNESS, "EV_soc_y2g", "LED yellow to green (rising)"},
      {0xE031, KIND_VOLT_SETTING, GROUP_LED, 9, 15, ITEM_WITNESS, "EV_soc_g2y", "LED green to yellow (falling)"},
      {0xE032, KIND_VOLT_SETTING, GROUP_LED, 9, 15, ITEM_WITNESS, "EV_soc_y2r0",
       "LED yellow to blinking red (falling)"},
      {0xE033, KIND_VOLT_SETTING, GROUP_LED, 9, 15, ITEM_WITNESS, "EV_soc_r2y", "LED red to yellow (rising)"},

      {0x0008, KIND_VOLT_LIVE, GROUP_STATUS, 0, 0, 0, "adc_vb_f", "Battery voltage"},
      {0x000C, KIND_AMPS, GROUP_STATUS, 0, 0, 0, "adc_il_f", "Load current"},
      {0x000E, KIND_CELSIUS, GROUP_STATUS, 0, 0, 0, "T_batt", "Battery temperature (RTS, else ambient)"},
      {0x0010, KIND_CELSIUS, GROUP_STATUS, 0, 0, 0, "T_rts", "Remote temperature sensor"},
      {0x0011, KIND_CHARGE_STATE, GROUP_STATUS, 0, 0, 0, "charge_state", "Charge state"},
      {0x0012, KIND_ARRAY_FAULT, GROUP_STATUS, 0, 0, 0, "array_fault", "Array faults"},
      {0x0014, KIND_VOLT_TARGET, GROUP_STATUS, 0, 0, 0, "Vb_ref", "Regulation target (temperature compensated)"},
      {0x001A, KIND_LOAD_STATE, GROUP_STATUS, 0, 0, 0, "load_state", "Load state"},
      {0x001B, KIND_LOAD_FAULT, GROUP_STATUS, 0, 0, 0, "load_fault", "Load faults"},
      {0x001C, KIND_VOLT_LIVE, GROUP_STATUS, 0, 0, 0, "V_lvd", "LVD in effect (load-current compensated)"},
      {0x0025, KIND_DIP, GROUP_STATUS, 0, 0, 0, "dip_switch", "DIP switches"},
  };
  return kItems[index < ITEM_COUNT ? index : 0];
}

// Bits of the fault registers that say "settings were written and the controller has not
// restarted since". Every EEPROM write sets them, and only a restart clears them.
static const uint16_t ARRAY_FAULT_EEPROM_EDIT = 1u << 5;
static const uint16_t LOAD_FAULT_EEPROM_EDIT = 1u << 6;

// The low byte of T_rts reads this while no remote temperature sensor is connected.
static const uint16_t RTS_NOT_CONNECTED = 0x0080;

// The DIP switches, as the controller reports them.
static const uint16_t DIP_CUSTOM_CHARGE = 1u << 0;  // SW1 ON: the custom charge settings are in use
static const uint16_t DIP_CUSTOM_LVD = 1u << 1;     // SW2 ON: the custom LVD and LVR are in use
static const uint16_t DIP_EQUALIZE = 1u << 2;       // SW3 ON: automatic equalization
static const uint16_t DIP_MODBUS = 1u << 3;         // SW4 ON: MODBUS instead of MeterBus

// With DIP switch 2 OFF the load is disconnected at this voltage (12 V system; twice that at 24 V).
static const double PRESET_LVD_VOLTS = 11.50;

// ---------------------------------------------------------------------------------------------
// One pass over the registers
// ---------------------------------------------------------------------------------------------

struct Snapshot {
  uint16_t raw[ITEM_COUNT];
  uint8_t status[ITEM_COUNT];  // IoStatus
};

inline void clearSnapshot(Snapshot *snapshot) {
  for (uint8_t i = 0; i < ITEM_COUNT; ++i) {
    snapshot->raw[i] = 0;
    snapshot->status[i] = SSIO_NOT_READ;
  }
}

inline bool have(const Snapshot &snapshot, uint8_t index) {
  return index < ITEM_COUNT && snapshot.status[index] == SSIO_OK;
}

// Reads the registers a few at a time, so that the caller's loop keeps running in between: one
// register takes one request, and a request can take as long as the Modbus timeout.
//
// A register that gets no reply is asked for once more, in a later step. If it stays silent it
// is noted as such and the pass carries on: one register that this controller does not answer
// must not keep the rest from being judged. More than MISSES_PER_PASS requests without a reply
// end the pass as failed: the link is not fit to judge a charger by. A register the controller
// refuses (an older firmware may lack one) is noted as refused, without a second request.
class SnapshotReader {
 public:
  static const uint8_t ATTEMPTS_PER_REGISTER = 2;
  static const uint8_t MISSES_PER_PASS = 4;

  SnapshotReader() : next_(0), attempts_(0), misses_(0), state_(PASS_IDLE), missed_(false) {
    clearSnapshot(&snapshot_);
  }

  // Starts a new pass. What the last one read is dropped.
  void start() {
    clearSnapshot(&snapshot_);
    next_ = 0;
    attempts_ = 0;
    misses_ = 0;
    missed_ = false;
    state_ = PASS_READING;
  }

  // Gives up the pass that is under way.
  void cancel() {
    if (state_ == PASS_READING) {
      state_ = PASS_IDLE;
    }
  }

  // Reads up to `budget` registers. Returns true once no pass is under way any more: it is
  // done(), it failed(), or none was started.
  bool step(Reader &bus, uint8_t budget) {
    missed_ = false;
    if (state_ != PASS_READING) {
      return true;
    }
    while (budget > 0 && next_ < ITEM_COUNT) {
      --budget;
      uint16_t raw = 0;
      const IoStatus status = bus.readRegister(item(next_).addr, &raw);
      if (status != SSIO_OK && status != SSIO_REFUSED) {
        // No reply. The step ends here whatever follows, so that the line is quiet for a while
        // before the next request.
        missed_ = true;
        ++misses_;
        if (misses_ > MISSES_PER_PASS) {
          state_ = PASS_FAILED;
          return true;
        }
        ++attempts_;
        if (attempts_ < ATTEMPTS_PER_REGISTER) {
          return false;  // the same register again in the next step
        }
        attempts_ = 0;
        snapshot_.raw[next_] = 0;
        snapshot_.status[next_] = SSIO_NO_REPLY;
        ++next_;
        break;
      }
      attempts_ = 0;
      snapshot_.raw[next_] = status == SSIO_OK ? raw : (uint16_t)0;
      snapshot_.status[next_] = (uint8_t)status;
      ++next_;
    }
    if (next_ >= ITEM_COUNT) {
      state_ = PASS_DONE;
      return true;
    }
    return false;
  }

  bool active() const { return state_ == PASS_READING; }
  bool done() const { return state_ == PASS_DONE; }
  bool failed() const { return state_ == PASS_FAILED; }
  // The last step ended on a request that got no reply. A late reply may still be on its way:
  // leave the line alone for one Modbus timeout before the next step.
  bool missed() const { return missed_; }
  uint8_t progress() const { return next_; }
  // Complete only when done(). A register can be in it as refused or as without a reply.
  const Snapshot &snapshot() const { return snapshot_; }

 private:
  enum State : uint8_t { PASS_IDLE = 0, PASS_READING, PASS_DONE, PASS_FAILED };

  Snapshot snapshot_;
  uint8_t next_;
  uint8_t attempts_;  // requests for the register at next_ that got no reply
  uint8_t misses_;    // requests in this pass that got no reply
  uint8_t state_;
  bool missed_;
};

// ---------------------------------------------------------------------------------------------
// The scale of the stored settings
// ---------------------------------------------------------------------------------------------

struct ScaleVerdict {
  uint8_t used;                    // setpoints that were read and hold a setting
  uint8_t fits[SCALE_CANDIDATES];  // how many of them are round with x100 and with x96.667
  uint8_t scale;                   // ScaleId: SCALE_UNKNOWN when the setpoints cannot tell
};

// A blank or damaged EEPROM cell (0xFFFF, say) is not a setting that somebody chose. A setpoint
// counts when at least one candidate puts it inside the range its register allows.
inline bool plausibleSetpoint(const Item &entry, uint16_t raw) {
  if (entry.loVolts == 0 && entry.hiVolts == 0) {
    return true;
  }
  for (uint8_t c = 0; c < SCALE_CANDIDATES; ++c) {
    const double volts = voltsFromRaw(raw, candidateScale(c));
    if (volts >= (double)entry.loVolts && volts <= (double)entry.hiVolts) {
      return true;
    }
  }
  return false;
}

// Judges the scale from setpoint words alone. Most of them must be round with one scale, and at
// least twice as many as with the other. Fewer than four cannot tell a pattern from luck.
inline ScaleVerdict judgeSetpointWords(const uint16_t *raws, uint8_t count) {
  ScaleVerdict verdict;
  verdict.used = count;
  verdict.scale = SCALE_UNKNOWN;
  for (uint8_t c = 0; c < SCALE_CANDIDATES; ++c) {
    const double lsb = scaleNumerator(candidateScale(c)) / VOLTS_DENOMINATOR;
    uint8_t fits = 0;
    for (uint8_t i = 0; i < count; ++i) {
      if (gridDistance((double)raws[i] * lsb, SETPOINT_GRID_VOLTS) <= lsb) {
        ++fits;
      }
    }
    verdict.fits[c] = fits;
  }
  if (count < 4) {
    return verdict;
  }
  const uint8_t best = verdict.fits[1] > verdict.fits[0] ? (uint8_t)1 : (uint8_t)0;
  const uint8_t other = best == 0 ? (uint8_t)1 : (uint8_t)0;
  if ((unsigned)verdict.fits[best] * 10u >= (unsigned)count * 6u &&
      (unsigned)verdict.fits[other] * 2u <= (unsigned)verdict.fits[best]) {
    verdict.scale = candidateScale(best);
  }
  return verdict;
}

inline ScaleVerdict judgeSettingsScale(const Snapshot &snapshot) {
  uint16_t raws[ITEM_COUNT];
  uint8_t used = 0;
  for (uint8_t i = 0; i < SETTING_ITEMS; ++i) {
    const Item &entry = item(i);
    if ((entry.flags & ITEM_WITNESS) == 0 || !have(snapshot, i)) {
      continue;
    }
    const uint16_t raw = snapshot.raw[i];
    if (raw != 0 && plausibleSetpoint(entry, raw)) {
      raws[used++] = raw;
    }
  }
  return judgeSetpointWords(raws, used);
}

// ---------------------------------------------------------------------------------------------
// The scale of the live readings
// ---------------------------------------------------------------------------------------------

enum LiveSource : uint8_t {
  LIVE_SOURCE_NONE = 0,
  LIVE_SOURCE_PRESET_LVD,  // DIP switch 2 OFF: V_lvd reads the built-in 11.50 V
  LIVE_SOURCE_STORED_LVD   // DIP switch 2 ON: V_lvd reads the stored EV_lvd
};

// The system that one reading of the battery voltage fits: 12, 24, or 0 for neither.
inline uint8_t systemVoltsFor(double volts) {
  if (volts >= 8.0 && volts <= 17.0) {
    return 12;
  }
  if (volts > 17.0 && volts <= 34.0) {
    return 24;
  }
  return 0;
}

// 12 or 24, from the battery voltage. 0 when it was not read, makes no sense, or fits another
// system with each scale. The scale of the live readings is not known here, and the two scales
// are 3.3 % apart: near a limit one battery voltage can fit both systems (raw 5627 is 16.60 V
// with x96.667 and 17.17 V with x100). So both scales have to give the same system, which they
// do from 8.28 to 17.00 V and from 17.59 to 34.00 V read with x100.
inline uint8_t systemVolts(const Snapshot &snapshot) {
  if (!have(snapshot, I_ADC_VB_F)) {
    return 0;
  }
  const uint8_t with100 = systemVoltsFor(voltsFromRaw(snapshot.raw[I_ADC_VB_F], SCALE_100));
  const uint8_t with96667 = systemVoltsFor(voltsFromRaw(snapshot.raw[I_ADC_VB_F], SCALE_96667));
  return with100 == with96667 ? with100 : (uint8_t)0;
}

// The LVD in effect is the wanted LVD less the load-current compensation, which the built-in
// profile sets to 15 mV per amp. At the 15 A the controller is rated for that is 0.23 V.
static const double LVD_COMPENSATION_ROOM_VOLTS = 0.30;
static const double LVD_READING_ROOM_VOLTS = 0.05;
static const double LOAD_RATED_AMPS = 15.0;
// The controller works the LVD in effect out in whole words, so at the full compensation its
// reading can be a word or two below the figure worked out here (0.30 V less at 0.0200 ohm and
// 15 A, from EV_lvd 3051 with x96.667: V_lvd 2949, which reads 8.6997 V against 8.7006 V). The
// room below takes in a few words more, so that the right scale is not left out there.
static const double LVD_ROUNDING_ROOM_VOLTS = 0.01;

// With switch 2 ON the compensation is the stored one (ER_icomp), which can be set far higher.
// The test below then holds only while that stays within the room at the rated load current,
// which is up to 0.02 ohm: a larger compensation can pull the LVD in effect so far down that
// the wrong scale reads as right, and one that was not read could be anything.
inline bool storedCompensationFits(const Snapshot &snapshot) {
  return have(snapshot, I_ER_ICOMP) &&
         ohmsFromRaw(snapshot.raw[I_ER_ICOMP]) * LOAD_RATED_AMPS <= LVD_COMPENSATION_ROOM_VOLTS;
}

// The controller takes its stored settings into use when it starts: the LVD, the charging
// voltages, the temperature compensation. One that was changed since is not the one in effect,
// and every write to its EEPROM raises the "EEPROM setting edit" fault in both fault words until
// it restarts. So the stored settings stand for those in effect only while both words were read
// and neither reports that edit: one that was not read could.
inline bool storedSettingsInEffect(const Snapshot &snapshot) {
  return have(snapshot, I_ARRAY_FAULT) && have(snapshot, I_LOAD_FAULT) &&
         (snapshot.raw[I_ARRAY_FAULT] & ARRAY_FAULT_EEPROM_EDIT) == 0 &&
         (snapshot.raw[I_LOAD_FAULT] & LOAD_FAULT_EEPROM_EDIT) == 0;
}

// Looks for the one scale that makes V_lvd read as the LVD the controller is known to use.
// settingsScale is the verdict of judgeSettingsScale(); the stored LVD is only known with it.
inline void judgeLiveScale(const Snapshot &snapshot, uint8_t settingsScale, uint8_t *scale, uint8_t *source) {
  *scale = SCALE_UNKNOWN;
  *source = LIVE_SOURCE_NONE;
  if (!have(snapshot, I_DIP_SWITCH) || !have(snapshot, I_V_LVD) || snapshot.raw[I_V_LVD] == 0) {
    return;
  }
  const uint8_t system = systemVolts(snapshot);
  if (system == 0) {
    return;
  }
  const double times = system == 24 ? 2.0 : 1.0;
  double wanted = 0.0;
  uint8_t from = LIVE_SOURCE_NONE;
  if ((snapshot.raw[I_DIP_SWITCH] & DIP_CUSTOM_LVD) == 0) {
    wanted = PRESET_LVD_VOLTS * times;
    from = LIVE_SOURCE_PRESET_LVD;
  } else if (settingsScale != SCALE_UNKNOWN && have(snapshot, I_EV_LVD) && snapshot.raw[I_EV_LVD] != 0 &&
             storedCompensationFits(snapshot) && storedSettingsInEffect(snapshot)) {
    wanted = voltsFromRaw(snapshot.raw[I_EV_LVD], settingsScale) * times;
    from = LIVE_SOURCE_STORED_LVD;
  } else {
    return;
  }
  uint8_t matches = 0;
  uint8_t match = SCALE_UNKNOWN;
  for (uint8_t c = 0; c < SCALE_CANDIDATES; ++c) {
    const double reading = voltsFromRaw(snapshot.raw[I_V_LVD], candidateScale(c));
    if (reading >= wanted - (LVD_COMPENSATION_ROOM_VOLTS + LVD_ROUNDING_ROOM_VOLTS) * times &&
        reading <= wanted + LVD_READING_ROOM_VOLTS * times) {
      ++matches;
      match = candidateScale(c);
    }
  }
  if (matches == 1) {
    *scale = match;
    *source = from;
  }
}

// ---------------------------------------------------------------------------------------------
// Do the settings suit the battery?
// ---------------------------------------------------------------------------------------------

enum Chemistry : uint8_t {
  CHEMISTRY_NOT_JUDGED = 0,  // no battery, or one this check has no rules for
  CHEMISTRY_LEAD_SEALED,     // AGM, gel, sealed lead-acid: no equalization
  CHEMISTRY_LEAD_FLOODED,
  CHEMISTRY_LIFEPO4,
  CHEMISTRY_LI_ION           // three cells in series: full at 12.6 V
};

inline const char *chemistryName(uint8_t chemistry) {
  switch (chemistry) {
    case CHEMISTRY_LEAD_SEALED: return "sealed lead-acid";
    case CHEMISTRY_LEAD_FLOODED: return "flooded lead-acid";
    case CHEMISTRY_LIFEPO4: return "LiFePO4";
    case CHEMISTRY_LI_ION: return "Li-ion";
    default: return "not judged";
  }
}

inline bool chemistryIsLithium(uint8_t chemistry) {
  return chemistry == CHEMISTRY_LIFEPO4 || chemistry == CHEMISTRY_LI_ION;
}

// The chemistry that a battery type of the client config (BatteryType, TankAlarm_Battery.h) is
// judged as. The client judges its charger against it, and the server tells from it whether the
// report it keeps was judged against the battery that the config names now.
inline uint8_t chemistryForBatteryType(int batteryType) {
  switch (batteryType) {
    case BATTERY_TYPE_AGM:
    case BATTERY_TYPE_GEL:
    case BATTERY_TYPE_SLA:
      return CHEMISTRY_LEAD_SEALED;
    case BATTERY_TYPE_FLOODED:
      return CHEMISTRY_LEAD_FLOODED;
    case BATTERY_TYPE_LIFEPO4:
      return CHEMISTRY_LIFEPO4;
    case BATTERY_TYPE_LI_ION:
      return CHEMISTRY_LI_ION;
    default:
      return CHEMISTRY_NOT_JUDGED;  // no battery, custom thresholds, or the LiPo cell
  }
}

enum Finding : uint32_t {
  FINDING_PRESET_IN_USE = 1ul << 0,     // SW1 OFF: a built-in lead-acid profile charges the battery
  FINDING_EQUALIZE_SWITCH = 1ul << 1,   // SW3 ON
  FINDING_EQUALIZE_STORED = 1ul << 2,   // an equalize voltage is stored in a custom bank
  FINDING_TEMPCOMP = 1ul << 3,          // temperature compensation is stored as on
  FINDING_ABSORB_RANGE = 1ul << 4,      // a stored absorption voltage is outside the battery's range
  FINDING_FLOAT_RANGE = 1ul << 5,       // a stored float voltage is outside the battery's range
  FINDING_SCALE_UNPROVEN = 1ul << 6,    // the stored voltages could not be judged, but for any of 0 V
  FINDING_NOT_READ = 1ul << 7,          // a register the judgement needs did not answer
  FINDING_RESET_PENDING = 1ul << 8,     // settings were written, the controller has not restarted
  FINDING_CHARGER_FAULT = 1ul << 9,     // the controller reports another fault
  FINDING_PACK_VOLTAGE = 1ul << 10,     // the battery is not the 12 V or 24 V pack that was configured
  FINDING_PRESET_LVD = 1ul << 11,       // SW2 OFF: the load disconnects at the built-in 11.50 V (23.00 V at 24 V)
  FINDING_LIVE_SCALE = 1ul << 12,       // the charger's own LVD reads wrong with this firmware's scale
  FINDING_SYSTEM_24V = 1ul << 13,       // a 24 V system: the stored voltages were not judged
  FINDING_SYSTEM_UNKNOWN = 1ul << 14    // the battery voltage tells no system: not judged either
};

static const uint8_t FINDING_BITS = 15;

inline const char *findingText(uint32_t finding) {
  switch (finding) {
    case FINDING_PRESET_IN_USE:
      return "DIP switch 1 is OFF: the charger uses a built-in lead-acid profile with temperature "
             "compensation, and ignores the custom charge settings.";
    case FINDING_EQUALIZE_SWITCH:
      return "DIP switch 3 is ON: automatic equalization is switched on.";
    case FINDING_EQUALIZE_STORED:
      return "An equalize voltage is stored in the custom charge settings.";
    case FINDING_TEMPCOMP:
      return "Temperature compensation is on in the custom charge settings.";
    case FINDING_ABSORB_RANGE:
      return "A stored absorption voltage is outside the range for this battery.";
    case FINDING_FLOAT_RANGE:
      return "A stored float voltage is outside the range for this battery.";
    case FINDING_SCALE_UNPROVEN:
      return "Stored voltages other than 0 V were not judged: how this charger encodes them could not "
             "be proven from its setpoints.";
    case FINDING_NOT_READ:
      return "A register that the judgement needs did not answer.";
    case FINDING_RESET_PENDING:
      return "Settings were written and the charger has not restarted since: it reports the 'EEPROM "
             "setting edit' fault.";
    case FINDING_CHARGER_FAULT:
      return "The charger reports a fault.";
    case FINDING_PACK_VOLTAGE:
      return "The battery voltage does not match the pack voltage that was configured.";
    case FINDING_PRESET_LVD:
      return "DIP switch 2 is OFF: the load is disconnected at the built-in 11.50 V (23.00 V on a 24 V "
             "system), not at the stored LVD.";
    case FINDING_LIVE_SCALE:
      return "The charger's own LVD reads wrong with the scale this firmware uses for voltages, so the "
             "battery voltage it reports is off by 3.3 %.";
    case FINDING_SYSTEM_24V:
      return "This is a 24 V system: the stored voltages were not judged.";
    case FINDING_SYSTEM_UNKNOWN:
      return "The battery voltage the charger reports does not tell whether this is a 12 V or a 24 V "
             "system: the stored voltages were not judged.";
    default:
      return "A finding this firmware has no text for.";
  }
}

enum Verdict : uint8_t {
  VERDICT_NOT_JUDGED = 0,  // no rules for this battery
  VERDICT_OK = 1,          // nothing wrong was found
  VERDICT_MISMATCH = 2,    // the settings do not suit the battery
  VERDICT_CANNOT_TELL = 3  // part of the judgement could not be made, and the rest found nothing
};

inline const char *verdictName(uint8_t verdict) {
  switch (verdict) {
    case VERDICT_OK: return "ok";
    case VERDICT_MISMATCH: return "mismatch";
    case VERDICT_CANNOT_TELL: return "cannot tell";
    default: return "not judged";
  }
}

// The findings that make the settings wrong for a battery. The rest are notes.
inline uint32_t problemFindings(uint8_t chemistry) {
  switch (chemistry) {
    case CHEMISTRY_LIFEPO4:
    case CHEMISTRY_LI_ION:
      return FINDING_PRESET_IN_USE | FINDING_EQUALIZE_SWITCH | FINDING_EQUALIZE_STORED | FINDING_TEMPCOMP |
             FINDING_ABSORB_RANGE | FINDING_FLOAT_RANGE;
    case CHEMISTRY_LEAD_SEALED:
      return FINDING_EQUALIZE_SWITCH | FINDING_EQUALIZE_STORED;
    default:
      return 0;
  }
}

// The findings that leave part of the judgement open.
static const uint32_t OPEN_FINDINGS =
    FINDING_SCALE_UNPROVEN | FINDING_NOT_READ | FINDING_SYSTEM_24V | FINDING_SYSTEM_UNKNOWN;

struct VoltRange {
  double low;
  double high;
};

// Where a stored absorption and float voltage may lie, as 12 V figures. The controller keeps
// its settings as 12 V figures in a 24 V system too.
inline bool chargeRanges(uint8_t chemistry, VoltRange *absorb, VoltRange *floating) {
  switch (chemistry) {
    case CHEMISTRY_LIFEPO4:
      absorb->low = 13.80;
      absorb->high = 14.60;
      floating->low = 13.20;
      floating->high = 13.90;
      return true;
    case CHEMISTRY_LI_ION:
      absorb->low = 12.00;
      absorb->high = 12.60;
      floating->low = 11.70;
      floating->high = 12.60;
      return true;
    default:
      return false;
  }
}

struct Expectation {
  uint8_t chemistry;      // Chemistry
  uint8_t nominalVolts;   // the pack voltage that was configured: 12 or 24, 0 when not known
  uint8_t firmwareScale;  // ScaleId this firmware decodes live voltages with, SCALE_UNKNOWN to skip the comparison
};

struct Evaluation {
  uint8_t chemistry;  // what the settings were judged against
  uint8_t verdict;    // Verdict
  uint32_t findings;  // Finding bits
  ScaleVerdict settingsScale;
  uint8_t liveScale;    // ScaleId the charger's own LVD points at, SCALE_UNKNOWN when it cannot tell
  uint8_t liveSource;   // LiveSource
  uint8_t systemVolts;  // 12 or 24 as measured, 0 when not known
};

// One stored voltage against its range. A tolerance of 0.005 V covers the rounding to a raw word.
inline bool outsideRange(uint16_t raw, uint8_t scale, const VoltRange &range) {
  const double volts = voltsFromRaw(raw, scale);
  return volts < range.low - 0.005 || volts > range.high + 0.005;
}

inline void evaluate(const Snapshot &snapshot, const Expectation &want, Evaluation *out) {
  memset(out, 0, sizeof(*out));
  out->chemistry = want.chemistry;
  out->settingsScale = judgeSettingsScale(snapshot);
  out->systemVolts = systemVolts(snapshot);
  judgeLiveScale(snapshot, out->settingsScale.scale, &out->liveScale, &out->liveSource);

  uint32_t findings = 0;

  // Notes that hold whatever the battery. Each fault word counts by itself: one that did not
  // answer takes nothing from what the other says.
  const uint16_t arrayFault = have(snapshot, I_ARRAY_FAULT) ? snapshot.raw[I_ARRAY_FAULT] : (uint16_t)0;
  const uint16_t loadFault = have(snapshot, I_LOAD_FAULT) ? snapshot.raw[I_LOAD_FAULT] : (uint16_t)0;
  if ((arrayFault & ARRAY_FAULT_EEPROM_EDIT) != 0 || (loadFault & LOAD_FAULT_EEPROM_EDIT) != 0) {
    findings |= FINDING_RESET_PENDING;
  }
  if ((arrayFault & (uint16_t)~ARRAY_FAULT_EEPROM_EDIT) != 0 || (loadFault & (uint16_t)~LOAD_FAULT_EEPROM_EDIT) != 0) {
    findings |= FINDING_CHARGER_FAULT;
  }
  if (want.nominalVolts != 0 && out->systemVolts != 0 && want.nominalVolts != out->systemVolts) {
    findings |= FINDING_PACK_VOLTAGE;
  }
  if (out->liveScale != SCALE_UNKNOWN && want.firmwareScale != SCALE_UNKNOWN && out->liveScale != want.firmwareScale) {
    findings |= FINDING_LIVE_SCALE;
  }

  const bool lithium = chemistryIsLithium(want.chemistry);
  const bool sealed = want.chemistry == CHEMISTRY_LEAD_SEALED;
  // What was read is judged by itself. A register that did not answer leaves the judgement open
  // (FINDING_NOT_READ), and takes nothing from one that did answer and is wrong.
  if (lithium || sealed) {
    const bool haveDip = have(snapshot, I_DIP_SWITCH);
    const uint16_t dip = haveDip ? snapshot.raw[I_DIP_SWITCH] : (uint16_t)0;
    const bool custom = (dip & DIP_CUSTOM_CHARGE) != 0;
    if (!haveDip) {
      findings |= FINDING_NOT_READ;
    } else {
      if ((dip & DIP_EQUALIZE) != 0) {
        findings |= FINDING_EQUALIZE_SWITCH;
      }
      if (lithium) {
        if (!custom) {
          findings |= FINDING_PRESET_IN_USE;
        }
        if ((dip & DIP_CUSTOM_LVD) == 0) {
          findings |= FINDING_PRESET_LVD;
        }
      }
    }
    // The battery jumper picks the bank and cannot be read, so both banks have to be right.
    // For a lithium battery the stored settings are judged whatever switch 1 says: they are
    // what the charger will use once the switch is set. For a sealed battery they count only
    // while they are known to be in use.
    if (lithium || custom) {
      const uint8_t equalize[2] = {I_EV_EQ, I_EV_EQ2};
      for (uint8_t bank = 0; bank < 2; ++bank) {
        if (!have(snapshot, equalize[bank])) {
          findings |= FINDING_NOT_READ;
        } else if (snapshot.raw[equalize[bank]] != 0) {
          findings |= FINDING_EQUALIZE_STORED;
        }
      }
    }
  }
  if (lithium) {
    if (!have(snapshot, I_EV_TEMPCOMP)) {
      findings |= FINDING_NOT_READ;
    } else if (tempcompWord(snapshot.raw[I_EV_TEMPCOMP]) != 0) {
      findings |= FINDING_TEMPCOMP;
    }
    VoltRange absorb = {0.0, 0.0};
    VoltRange floating = {0.0, 0.0};
    chargeRanges(want.chemistry, &absorb, &floating);
    // The stored voltages are judged as those of a 12 V system whose encoding is proven. Where
    // that cannot be said of this controller, they are not judged and the judgement stays open.
    // But a word of 0 is 0 V in either encoding, so on a 12 V system it is wrong for the battery
    // with the encoding unproven too: a float voltage of 0 means "no float", and the charger then
    // holds the battery at its absorption voltage. A 24 V system, or one that is not known, is
    // not judged even then.
    bool judged = false;
    if (!have(snapshot, I_ADC_VB_F)) {
      findings |= FINDING_NOT_READ;
    } else if (out->systemVolts == 24) {
      findings |= FINDING_SYSTEM_24V;
    } else if (out->systemVolts != 12) {
      findings |= FINDING_SYSTEM_UNKNOWN;
    } else if (out->settingsScale.scale == SCALE_UNKNOWN) {
      findings |= FINDING_SCALE_UNPROVEN;
    } else {
      judged = true;
    }
    const bool twelveVolts = out->systemVolts == 12;  // 0 when the battery voltage was not read
    const uint8_t setpoints[4] = {I_EV_REG, I_EV_REG2, I_EV_FLOAT, I_EV_FLOAT2};
    for (uint8_t i = 0; i < 4; ++i) {
      const bool isAbsorb = i < 2;
      if (!have(snapshot, setpoints[i])) {
        findings |= FINDING_NOT_READ;
        continue;
      }
      const uint16_t raw = snapshot.raw[setpoints[i]];
      if ((twelveVolts && raw == 0) ||
          (judged && outsideRange(raw, out->settingsScale.scale, isAbsorb ? absorb : floating))) {
        findings |= isAbsorb ? FINDING_ABSORB_RANGE : FINDING_FLOAT_RANGE;
      }
    }
  }

  out->findings = findings;
  if (want.chemistry == CHEMISTRY_NOT_JUDGED) {
    out->verdict = VERDICT_NOT_JUDGED;
  } else if ((findings & problemFindings(want.chemistry)) != 0) {
    out->verdict = VERDICT_MISMATCH;
  } else if ((findings & OPEN_FINDINGS) != 0) {
    out->verdict = VERDICT_CANNOT_TELL;
  } else {
    out->verdict = VERDICT_OK;
  }
}

// What a firmware that decodes voltages with firmwareScale reads for a battery, per volt, when the
// controller reports its live readings with liveScale. While liveScale is not known, the lower of
// the candidates: a voltage multiplied by it is never above what the firmware reads for a battery
// at that voltage. 1 for a firmwareScale that is not known, which then decodes as it pleases.
inline double readingFactor(uint8_t liveScale, uint8_t firmwareScale) {
  if (firmwareScale == SCALE_UNKNOWN) {
    return 1.0;
  }
  const double firmware = scaleNumerator(firmwareScale);
  if (liveScale != SCALE_UNKNOWN) {
    return firmware / scaleNumerator(liveScale);
  }
  double factor = 0.0;
  for (uint8_t c = 0; c < SCALE_CANDIDATES; ++c) {
    const double candidate = firmware / scaleNumerator(candidateScale(c));
    if (c == 0 || candidate < factor) {
      factor = candidate;
    }
  }
  return factor;
}

// The voltage at which the charger holds a full battery, as a firmware that decodes voltages
// with firmwareScale reads that battery. The client's no-charge alert compares the battery with
// it. 0 means "not known", and the caller falls back on a default.
//
// It is known only while the custom charge settings are in use (DIP switch 1 ON) with their
// scale proven and temperature compensation off: a built-in or compensated profile moves its
// float voltage with the temperature. And only while those stored are the ones in effect: after
// a write to the EEPROM the controller charges by the settings it started with until it restarts
// (see storedSettingsInEffect()). The battery jumper picks the bank and cannot be read, so the
// lower of the two banks counts. A bank without a float voltage holds the battery at its
// absorption voltage. The charger regulates to no more than its ceiling (Evb_ref_lim, which is not
// compensated; 0 is none), so one below that voltage holds the battery at the ceiling, and a
// ceiling that was not read could be anywhere.
//
// With the scale of the live readings proven the figure is exact. Without it the lower of the
// two possible readings is returned, so that a battery at its float voltage never reads as
// below it.
//
// Nor is it known while the measured system is not the pack that was configured
// (FINDING_PACK_VOLTAGE): a 12 V pack read at 18 V fits a 24 V system, and its 13.50 V float
// would become a 27 V reference. The caller then keeps the default of the configured pack.
inline double floatReferenceVolts(const Snapshot &snapshot, const Evaluation &evaluation, uint8_t firmwareScale) {
  const uint8_t scale = evaluation.settingsScale.scale;
  if (scale == SCALE_UNKNOWN || firmwareScale == SCALE_UNKNOWN || evaluation.systemVolts == 0) {
    return 0.0;
  }
  if ((evaluation.findings & FINDING_PACK_VOLTAGE) != 0) {
    return 0.0;
  }
  if (!have(snapshot, I_DIP_SWITCH) || (snapshot.raw[I_DIP_SWITCH] & DIP_CUSTOM_CHARGE) == 0) {
    return 0.0;
  }
  if (!have(snapshot, I_EV_TEMPCOMP) || tempcompWord(snapshot.raw[I_EV_TEMPCOMP]) != 0) {
    return 0.0;
  }
  if (!storedSettingsInEffect(snapshot)) {
    return 0.0;
  }
  if (!have(snapshot, I_EVB_REF_LIM)) {
    return 0.0;
  }
  const uint16_t ceiling = snapshot.raw[I_EVB_REF_LIM];
  const uint8_t absorbItems[2] = {I_EV_REG, I_EV_REG2};
  const uint8_t floatItems[2] = {I_EV_FLOAT, I_EV_FLOAT2};
  double lowest = 0.0;
  for (uint8_t bank = 0; bank < 2; ++bank) {
    if (!have(snapshot, absorbItems[bank]) || !have(snapshot, floatItems[bank])) {
      return 0.0;
    }
    const uint16_t raw = snapshot.raw[floatItems[bank]] != 0 ? snapshot.raw[floatItems[bank]] : snapshot.raw[absorbItems[bank]];
    const Item &entry = item(floatItems[bank]);
    double volts = voltsFromRaw(raw, scale);
    if (volts < (double)entry.loVolts || volts > (double)entry.hiVolts) {
      return 0.0;  // not a voltage a battery is held at
    }
    if (ceiling != 0 && raw > ceiling) {
      // Both are stored words of one scale. A ceiling below the voltages a battery is held at
      // says the settings are not what they seem, so nothing is known then either.
      volts = voltsFromRaw(ceiling, scale);
      if (volts < (double)entry.loVolts || volts > (double)entry.hiVolts) {
        return 0.0;
      }
    }
    if (bank == 0 || volts < lowest) {
      lowest = volts;
    }
  }
  if (evaluation.systemVolts == 24) {
    lowest *= 2.0;
  }
  return lowest * readingFactor(evaluation.liveScale, firmwareScale);
}

// A number that changes when the stored settings, the DIP switches or the judgement change, and
// not when the battery voltage, the load or the temperature do. The client reports to the server
// when it changes. (FNV-1a, 32 bits.)
inline uint32_t settingsFingerprint(const Snapshot &snapshot, const Evaluation &evaluation) {
  uint32_t hash = 2166136261ul;
  uint8_t bytes[8];
  for (uint8_t i = 0; i < ITEM_COUNT; ++i) {
    if (i >= SETTING_ITEMS && i != I_DIP_SWITCH) {
      continue;
    }
    bytes[0] = snapshot.status[i];
    bytes[1] = (uint8_t)(snapshot.raw[i] >> 8);
    bytes[2] = (uint8_t)(snapshot.raw[i] & 0xFF);
    for (uint8_t b = 0; b < 3; ++b) {
      hash = (hash ^ bytes[b]) * 16777619ul;
    }
  }
  bytes[0] = evaluation.chemistry;
  bytes[1] = evaluation.verdict;
  bytes[2] = evaluation.settingsScale.scale;
  bytes[3] = evaluation.liveScale;
  bytes[4] = (uint8_t)(evaluation.findings >> 24);
  bytes[5] = (uint8_t)(evaluation.findings >> 16);
  bytes[6] = (uint8_t)(evaluation.findings >> 8);
  bytes[7] = (uint8_t)(evaluation.findings & 0xFF);
  for (uint8_t b = 0; b < 8; ++b) {
    hash = (hash ^ bytes[b]) * 16777619ul;
  }
  return hash;
}

// ---------------------------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------------------------

// Builds a line in a caller's buffer. It never writes past the end and always leaves a
// terminated string. What does not fit is dropped, and truncated() says so.
class Text {
 public:
  Text(char *buffer, size_t capacity) : buffer_(buffer), capacity_(capacity), length_(0), truncated_(false) {
    if (capacity_ > 0) {
      buffer_[0] = '\0';
    }
  }

  Text &add(const char *text) {
    if (text == nullptr) {
      return *this;
    }
    while (*text != '\0') {
      put(*text++);
    }
    return *this;
  }

  Text &addUnsigned(uint32_t value) {
    char digits[10];
    uint8_t count = 0;
    do {
      digits[count++] = (char)('0' + value % 10u);
      value /= 10u;
    } while (value != 0);
    while (count > 0) {
      put(digits[--count]);
    }
    return *this;
  }

  Text &addSigned(int32_t value) {
    if (value < 0) {
      put('-');
      return addUnsigned((uint32_t)(-(int64_t)value));
    }
    return addUnsigned((uint32_t)value);
  }

  // value with `decimals` digits after the point, rounded half away from zero.
  Text &addFixed(double value, uint8_t decimals) {
    if (!(value == value) || value > 4.0e9 || value < -4.0e9) {
      return add("?");
    }
    if (decimals > 6) {
      decimals = 6;
    }
    uint32_t factor = 1;
    for (uint8_t i = 0; i < decimals; ++i) {
      factor *= 10u;
    }
    const bool negative = value < 0.0;
    const double magnitude = negative ? -value : value;
    const uint64_t scaled = (uint64_t)(magnitude * (double)factor + 0.5);
    if (negative && scaled != 0) {
      put('-');
    }
    addUnsigned((uint32_t)(scaled / factor));
    if (decimals > 0) {
      put('.');
      uint32_t rest = (uint32_t)(scaled % factor);
      for (uint32_t place = factor / 10u; place > 0; place /= 10u) {
        put((char)('0' + rest / place));
        rest %= place;
      }
    }
    return *this;
  }

  Text &addHex16(uint16_t value) {
    static const char kDigits[] = "0123456789ABCDEF";
    for (int8_t shift = 12; shift >= 0; shift = (int8_t)(shift - 4)) {
      put(kDigits[(value >> shift) & 0x0F]);
    }
    return *this;
  }

  size_t length() const { return length_; }
  bool truncated() const { return truncated_; }

 private:
  char *buffer_;
  size_t capacity_;
  size_t length_;
  bool truncated_;

  void put(char c) {
    if (capacity_ == 0 || length_ + 1 >= capacity_) {
      truncated_ = true;
      return;
    }
    buffer_[length_++] = c;
    buffer_[length_] = '\0';
  }
};

inline const char *chargeStateName(uint16_t raw) {
  static const char *const kNames[] = {"START", "NIGHT_CHECK", "DISCONNECT", "NIGHT", "FAULT",
                                       "BULK",  "ABSORPTION",  "FLOAT",      "EQUALIZE"};
  return raw < sizeof(kNames) / sizeof(kNames[0]) ? kNames[raw] : nullptr;
}

inline const char *loadStateName(uint16_t raw) {
  static const char *const kNames[] = {"START", "LOAD_ON", "LVD_WARNING", "LVD", "FAULT", "DISCONNECT"};
  return raw < sizeof(kNames) / sizeof(kNames[0]) ? kNames[raw] : nullptr;
}

inline const char *arrayFaultName(uint8_t bit) {
  static const char *const kNames[] = {"Overcurrent",
                                       "FETs shorted",
                                       "Software bug",
                                       "Battery HVD",
                                       "Array HVD",
                                       "EEPROM setting edit (reset required)",
                                       "RTS shorted",
                                       "RTS was valid, now disconnected",
                                       "Local temperature sensor failed"};
  return bit < sizeof(kNames) / sizeof(kNames[0]) ? kNames[bit] : nullptr;
}

inline const char *loadFaultName(uint8_t bit) {
  static const char *const kNames[] = {"External short circuit",
                                       "Overcurrent",
                                       "FETs shorted",
                                       "Software bug",
                                       "HVD",
                                       "Heatsink over-temperature",
                                       "EEPROM setting edit (reset required)"};
  return bit < sizeof(kNames) / sizeof(kNames[0]) ? kNames[bit] : nullptr;
}

// What a raw word means, as text: "14.00 V", "900 s (15 min)", "FLOAT", "SW1 ON, SW2 OFF, ...".
// A voltage whose scale is not known is shown both ways. settingsScale and liveScale are ScaleIds.
inline void describeValue(uint8_t index, uint16_t raw, uint8_t settingsScale, uint8_t liveScale, Text *out) {
  const Item &entry = item(index);
  if (entry.kind == KIND_TEMPCO) {
    raw = tempcompWord(raw);
  }
  if ((entry.flags & ITEM_ZERO_IS_OFF) != 0 && raw == 0) {
    out->add("0 (off)");
    return;
  }
  switch (entry.kind) {
    case KIND_VOLT_SETTING:
    case KIND_VOLT_LIVE: {
      const uint8_t scale = entry.kind == KIND_VOLT_SETTING ? settingsScale : liveScale;
      if (scale != SCALE_UNKNOWN) {
        out->addFixed(voltsFromRaw(raw, scale), 2).add(" V");
      } else {
        out->addFixed(voltsFromRaw(raw, SCALE_100), 2).add(" V if x100, ");
        out->addFixed(voltsFromRaw(raw, SCALE_96667), 2).add(" V if x96.667");
      }
      break;
    }
    case KIND_VOLT_TARGET:
      out->addFixed(voltsFromRaw(raw, SCALE_96667), 2).add(" V");
      break;
    case KIND_AMPS:
      out->addFixed((double)(int16_t)raw * 79.16 / 32768.0, 2).add(" A");
      break;
    case KIND_SECONDS:
      out->addUnsigned(raw).add(" s");
      if (raw >= 60) {
        out->add(" (").addFixed((double)raw / 60.0, raw % 60 == 0 ? 0 : 1).add(" min)");
      }
      break;
    case KIND_TENTHS:
      out->addFixed((double)raw / 10.0, 1).add(" s");
      break;
    case KIND_DAYS:
      out->addUnsigned(raw).add(raw == 1 ? " day" : " days");
      break;
    case KIND_TEMPCO:
      // The word is the size of the coefficient, whose sign is implied negative: the charger
      // lowers its voltages as the battery warms.
      if (raw != 0) {
        out->add("-");
      }
      out->addFixed((double)raw * 100.0 / 65536.0, 4).add(" V/C");
      break;
    case KIND_OHMS:
      out->addFixed(ohmsFromRaw(raw), 4).add(" ohm");
      break;
    case KIND_CELSIUS:
      if (index == I_T_RTS && (raw & 0xFF) == RTS_NOT_CONNECTED) {
        out->add("not connected");
      } else {
        out->addSigned((int8_t)(raw & 0xFF)).add(" C");
      }
      break;
    case KIND_CHARGE_STATE:
    case KIND_LOAD_STATE: {
      const char *name = entry.kind == KIND_CHARGE_STATE ? chargeStateName(raw) : loadStateName(raw);
      if (name != nullptr) {
        out->add(name);
      } else {
        out->add("state ").addUnsigned(raw);
      }
      break;
    }
    case KIND_ARRAY_FAULT:
    case KIND_LOAD_FAULT: {
      if (raw == 0) {
        out->add("none");
        break;
      }
      bool first = true;
      for (uint8_t bit = 0; bit < 16; ++bit) {
        if ((raw & (1u << bit)) == 0) {
          continue;
        }
        const char *name = entry.kind == KIND_ARRAY_FAULT ? arrayFaultName(bit) : loadFaultName(bit);
        if (!first) {
          out->add(", ");
        }
        first = false;
        if (name != nullptr) {
          out->add(name);
        } else {
          out->add("bit ").addUnsigned(bit);
        }
      }
      break;
    }
    case KIND_DIP:
      for (uint8_t sw = 0; sw < 4; ++sw) {
        out->add(sw == 0 ? "SW" : ", SW").addUnsigned(sw + 1u).add((raw & (1u << sw)) != 0 ? " ON" : " OFF");
      }
      break;
    default:
      out->add("0x").addHex16(raw);
      break;
  }
}

// One line that says what the scale test found: "x100 (12 of 14 setpoints are round with it, 1 with x96.667)".
inline void describeSettingsScale(const ScaleVerdict &verdict, Text *out) {
  if (verdict.scale == SCALE_UNKNOWN) {
    out->add("not proven (of ").addUnsigned(verdict.used).add(" setpoints ").addUnsigned(verdict.fits[0]);
    out->add(" are round with x100 and ").addUnsigned(verdict.fits[1]).add(" with x96.667)");
    return;
  }
  const uint8_t best = verdict.scale == SCALE_100 ? 0 : 1;
  const uint8_t other = best == 0 ? 1 : 0;
  out->add(scaleName(verdict.scale)).add(" (").addUnsigned(verdict.fits[best]).add(" of ").addUnsigned(verdict.used);
  out->add(" setpoints are round with it, ").addUnsigned(verdict.fits[other]).add(" with ");
  out->add(scaleName(candidateScale(other))).add(")");
}

// systemVolts is Evaluation::systemVolts: the built-in LVD is 11.50 V on a 12 V system and twice
// that on a 24 V one.
inline void describeLiveScale(uint8_t scale, uint8_t source, uint8_t systemVolts, Text *out) {
  if (scale == SCALE_UNKNOWN) {
    out->add("not proven");
    return;
  }
  out->add(scaleName(scale));
  if (source != LIVE_SOURCE_PRESET_LVD) {
    out->add(" (the stored LVD reads right with it)");
  } else if (systemVolts == 24) {
    out->add(" (the built-in 23.00 V LVD reads right with it)");
  } else if (systemVolts == 12) {
    out->add(" (the built-in 11.50 V LVD reads right with it)");
  } else {
    out->add(" (the built-in LVD, 11.50 V or 23.00 V on a 24 V system, reads right with it)");
  }
}

}  // namespace sunsaver

#endif  // TANKALARM_SUNSAVER_SETTINGS_H
