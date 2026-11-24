/**
 * TankAlarm_SunSaverProgram.h
 *
 * Writing a LiFePO4 charging profile, the LVD and the LVR into a Morningstar SunSaver MPPT: what
 * may be asked for, which registers that changes and in which order, and the procedure that
 * writes them, restarts the controller and says how it went.
 *
 * It builds on TankAlarm_SunSaverSettings.h and, like it, has no Arduino dependency, no heap, no
 * printf and no exception, and keeps to C++14. The controller is reached through the Bus
 * interface, so the same code runs in the client (ArduinoModbus, TankAlarm_Solar.cpp) and in the
 * host tests against the simulated SunSaver (tests/host/sunsaver_program).
 *
 * The client compiles the procedure in only with TANKALARM_SUNSAVER_PROGRAMMING set to 1 (see
 * TankAlarm_Solar.h). The server needs this file either way, to read and show a client's result.
 *
 * What a run does
 * ---------------
 *   1. Reads every register of the charger check, and refuses the run when anything it relies on
 *      is missing or wrong (the Refusal bits).
 *   2. Makes the plan: 17 registers, each with the word it has to hold. A register that already
 *      holds its word is not written.
 *   3. Restarts the controller (coil 0x00FF), waits, and asks until it answers. A controller
 *      that refuses the command, or does not come back from it, is not written to.
 *   4. Writes one register at a time with function 6, and reads each one back.
 *   5. Restarts the controller again, which puts the new settings to work.
 *   6. Reads the registers once more, from the restarted controller.
 *
 * Before step 3 and before step 4 the run waits for go(). The caller records in its flash how
 * far the run has come first, so that a client that loses power part way knows at its next start
 * whether the controller may have been left waiting for a restart (see Programmer::startRecovery).
 *
 * What makes a write safe
 * -----------------------
 *   The scale. A voltage is written as a raw word, and which word that is depends on how this
 *     controller encodes voltages (see TankAlarm_SunSaverSettings.h). Nothing is written unless
 *     judgeSettingsScale() has proven the encoding from the setpoints the controller holds. Only
 *     multiples of 0.05 V can be asked for, so the proof still works on what a run leaves behind.
 *   Limits. validateProfile() holds the values to fixed ranges for a 12 V LiFePO4 battery and to
 *     their order (LVD below LVR, float below absorption, absorption below HVD). They are in
 *     this file and nowhere else, so no config can widen them.
 *   The restart. Every EEPROM write raises the controller's "EEPROM setting edit" fault, and it
 *     stays raised until the controller restarts. So the restart command is tried before
 *     anything is written, on the controller as the run found it. And once a write request has
 *     been sent, the run goes on to the restart command whatever happens to the writes after it.
 *   Read-back. The controller does not check what it is given, and an acknowledgement can be
 *     lost on the way back from a controller that stored the value. So the acknowledgement
 *     decides nothing: what the register reads afterwards does.
 *   No repeats. A register that does not take its value is written once more, and then the run
 *     stops. A whole run is never repeated by itself.
 *   The order. What a lithium battery must not get goes first (equalization, temperature
 *     compensation), then the limits, then the charge voltages, then the load thresholds. Within
 *     a pair that has to stay in order (LVD and LVR, say) the register that keeps the pair in
 *     order is written first. So a run that is cut short leaves settings that are no worse for
 *     the battery than the ones it found.
 *
 * What it does not do
 * -------------------
 *   It does not put old values back when a run stops part way: the report says what the
 *     controller holds, register by register, and the next charger check judges it.
 *   It does not change the DIP switches, which only a person at the controller can. The stored
 *     charge settings are used with switch 1 ON, the stored LVD and LVR with switch 2 ON.
 *   It does not program a 24 V system, or any battery but LiFePO4.
 *
 * The LOAD output
 * ---------------
 * The specification does not say what the LOAD output does while the controller waits for its
 * restart, and a restart may switch it off for a moment. A client that is powered from the LOAD
 * terminals could lose power part way. So a run is refused while current flows from them, unless
 * the request says that this is all right. The first restart comes before any write, so a client
 * that does lose power to it has written nothing. The run notes the load state when it begins,
 * while the controller waits and at its end, so that the first run on a real controller answers
 * the question.
 *
 * Copyright (c) 2025-2026 Senax Inc. All rights reserved.
 */

#ifndef TANKALARM_SUNSAVER_PROGRAM_H
#define TANKALARM_SUNSAVER_PROGRAM_H

#include "TankAlarm_SunSaverSettings.h"

namespace sunsaver {

// ---------------------------------------------------------------------------------------------
// Reaching the controller
// ---------------------------------------------------------------------------------------------

class Bus : public Reader {
 public:
  // Writes one register with Modbus function 6, in one request. SSIO_OK when the controller
  // acknowledged the write, SSIO_REFUSED when it answered with an exception. A bus need not check
  // which address and value an acknowledgement names (ArduinoModbus does not): the run reads
  // every register back and goes by that.
  virtual IoStatus writeRegister(uint16_t addr, uint16_t raw) = 0;
  // Switches a coil on with function 5.
  virtual IoStatus writeCoil(uint16_t addr) = 0;
};

// Restarts the controller. Its acknowledgement may never come: the controller may restart first.
static const uint16_t COIL_RESTART = 0x00FF;

// ---------------------------------------------------------------------------------------------
// What can be asked for
// ---------------------------------------------------------------------------------------------

// Voltages are in hundredths of a volt (1420 is 14.20 V), as figures for a 12 V battery.
struct Profile {
  uint16_t absorbCv;       // absorption voltage: EV_reg and EV_reg2
  uint16_t floatCv;        // float voltage: EV_float and EV_float2
  uint16_t absorbSeconds;  // time at the absorption voltage: Et_float and Et_floatlb, both banks
  uint16_t hvdCv;          // the charger stops charging above this: EV_hvd, and the regulation ceiling
  uint16_t hvrCv;          // and starts again below this: EV_hvr
  uint16_t lvdCv;          // the load is disconnected below this: EV_lvd
  uint16_t lvrCv;          // and reconnected above this: EV_lvr
};

// What this project recommends for a 12 V LiFePO4 battery such as the Optima ORANGETOP, which
// switches itself off above 14.8 V.
inline Profile lifepo4Profile() {
  const Profile profile = {1420, 1350, 900, 1445, 1360, 1310, 1325};
  return profile;
}

enum ProfileError : uint16_t {
  PROFILE_ABSORB = 1u << 0,
  PROFILE_FLOAT = 1u << 1,
  PROFILE_TIME = 1u << 2,
  PROFILE_HVD = 1u << 3,
  PROFILE_HVR = 1u << 4,
  PROFILE_LVD = 1u << 5,
  PROFILE_LVR = 1u << 6,
  PROFILE_GRID = 1u << 7,    // a voltage is not a multiple of 0.05 V
  PROFILE_MISSING = 1u << 8  // the request did not hold all seven values as numbers
};

static const uint8_t PROFILE_ERROR_BITS = 9;

static const uint16_t ABSORB_SECONDS_MIN = 60;
static const uint16_t ABSORB_SECONDS_MAX = 14400;
static const uint16_t HVD_ABOVE_ABSORB_CV = 10;  // the HVD is at least this far above the absorption voltage
static const uint16_t HVD_MAX_CV = 1500;
static const uint16_t HVR_MIN_CV = 1320;
static const uint16_t HVR_MAX_CV = 1440;
static const uint16_t HVR_BELOW_HVD_CV = 20;     // the reconnect is at least this far below the disconnect
static const uint16_t LVD_MIN_CV = 1000;
static const uint16_t LVD_MAX_CV = 1330;
static const uint16_t LVR_ABOVE_LVD_CV = 10;
static const uint16_t LVR_MAX_CV = 1360;
static const uint16_t GRID_CV = 5;               // SETPOINT_GRID_VOLTS, in hundredths of a volt

inline const char *profileErrorText(uint16_t error) {
  switch (error) {
    case PROFILE_ABSORB: return "Absorption voltage: 13.80 to 14.60 V.";
    case PROFILE_FLOAT: return "Float voltage: 13.20 to 13.90 V, and below the absorption voltage.";
    case PROFILE_TIME: return "Absorption time: 1 to 240 minutes.";
    case PROFILE_HVD:
      return "High voltage disconnect: at least 0.10 V above the absorption voltage, and 15.00 V at most.";
    case PROFILE_HVR:
      return "High voltage reconnect: 13.20 to 14.40 V, and at least 0.20 V below the disconnect.";
    case PROFILE_LVD: return "LVD: 10.00 to 13.30 V.";
    case PROFILE_LVR: return "LVR: at least 0.10 V above the LVD, and 13.60 V at most.";
    case PROFILE_GRID: return "Every voltage has to be a multiple of 0.05 V.";
    case PROFILE_MISSING: return "A value is missing from the request.";
    default: return "A limit this firmware has no text for.";
  }
}

inline uint16_t centivoltsOf(double volts) { return (uint16_t)(volts * 100.0 + 0.5); }

// The seven values of a profile, in the order of the struct, for a list a person reads.
static const uint8_t PROFILE_FIELDS = 7;

inline const char *profileFieldLabel(uint8_t field) {
  switch (field) {
    case 0: return "Absorption voltage";
    case 1: return "Float voltage";
    case 2: return "Absorption time";
    case 3: return "High voltage disconnect";
    case 4: return "High voltage reconnect";
    case 5: return "Low voltage disconnect (LVD)";
    case 6: return "Low voltage reconnect (LVR)";
    default: return "?";
  }
}

inline uint16_t profileField(const Profile &profile, uint8_t field) {
  switch (field) {
    case 0: return profile.absorbCv;
    case 1: return profile.floatCv;
    case 2: return profile.absorbSeconds;
    case 3: return profile.hvdCv;
    case 4: return profile.hvrCv;
    case 5: return profile.lvdCv;
    case 6: return profile.lvrCv;
    default: return 0;
  }
}

// "14.20 V", and "15 min" for the time.
inline void describeProfileField(const Profile &profile, uint8_t field, Text *out) {
  const uint16_t value = profileField(profile, field);
  if (field != 2) {
    out->addFixed((double)value / 100.0, 2).add(" V");
  } else if (value % 60u == 0) {
    out->addUnsigned(value / 60u).add(" min");
  } else {
    out->addUnsigned(value).add(" s");
  }
}

// What is wrong with a profile, as ProfileError bits. 0 means it may be written. The ranges of
// the absorption and float voltage are the ones the charger check judges a LiFePO4 battery's
// settings by, so what this allows the check accepts.
inline uint16_t validateProfile(const Profile &profile) {
  uint16_t errors = 0;
  VoltRange absorb = {0.0, 0.0};
  VoltRange floating = {0.0, 0.0};
  chargeRanges(CHEMISTRY_LIFEPO4, &absorb, &floating);
  if (profile.absorbCv < centivoltsOf(absorb.low) || profile.absorbCv > centivoltsOf(absorb.high)) {
    errors |= PROFILE_ABSORB;
  }
  if (profile.floatCv < centivoltsOf(floating.low) || profile.floatCv > centivoltsOf(floating.high) ||
      profile.floatCv >= profile.absorbCv) {
    errors |= PROFILE_FLOAT;
  }
  if (profile.absorbSeconds < ABSORB_SECONDS_MIN || profile.absorbSeconds > ABSORB_SECONDS_MAX) {
    errors |= PROFILE_TIME;
  }
  if ((uint32_t)profile.hvdCv < (uint32_t)profile.absorbCv + HVD_ABOVE_ABSORB_CV || profile.hvdCv > HVD_MAX_CV) {
    errors |= PROFILE_HVD;
  }
  if (profile.hvrCv < HVR_MIN_CV || profile.hvrCv > HVR_MAX_CV ||
      (uint32_t)profile.hvrCv + HVR_BELOW_HVD_CV > (uint32_t)profile.hvdCv) {
    errors |= PROFILE_HVR;
  }
  if (profile.lvdCv < LVD_MIN_CV || profile.lvdCv > LVD_MAX_CV) {
    errors |= PROFILE_LVD;
  }
  if ((uint32_t)profile.lvrCv < (uint32_t)profile.lvdCv + LVR_ABOVE_LVD_CV || profile.lvrCv > LVR_MAX_CV) {
    errors |= PROFILE_LVR;
  }
  const uint16_t volts[6] = {profile.absorbCv, profile.floatCv, profile.hvdCv,
                             profile.hvrCv,    profile.lvdCv,   profile.lvrCv};
  for (uint8_t i = 0; i < 6; ++i) {
    if (volts[i] % GRID_CV != 0) {
      errors |= PROFILE_GRID;
    }
  }
  return errors;
}

// The word that stores a voltage, for a controller that encodes with `scale`. 0 for SCALE_UNKNOWN.
inline uint16_t rawFromCentivolts(uint16_t centivolts, uint8_t scale) {
  const double numerator = scaleNumerator(scale);
  if (!(numerator > 0.0)) {
    return 0;
  }
  const double raw = (double)centivolts / 100.0 * VOLTS_DENOMINATOR / numerator + 0.5;
  return raw < 65535.0 ? (uint16_t)raw : (uint16_t)0xFFFF;
}

// ---------------------------------------------------------------------------------------------
// The plan: which registers, which words, in which order
// ---------------------------------------------------------------------------------------------

static const uint8_t PLAN_ENTRIES = 17;

enum EntryStatus : uint8_t {
  ENTRY_PENDING = 0,  // not reached: the run ended before this register
  ENTRY_UNCHANGED,    // it already held the word: not written
  ENTRY_WRITTEN,      // written, and read back as written
  ENTRY_NOT_TAKEN,    // written twice, and it still reads what it held before
  ENTRY_ALTERED,      // the controller holds another word than it was given
  ENTRY_REFUSED,      // the controller refused the write
  ENTRY_NO_ANSWER,    // it could not be read after the write
  ENTRY_LOST          // it held the word after the write, and no longer does after the restart
};

static const uint8_t ENTRY_STATUS_LAST = ENTRY_LOST;

inline const char *entryStatusName(uint8_t status) {
  switch (status) {
    case ENTRY_PENDING: return "not reached";
    case ENTRY_UNCHANGED: return "already set";
    case ENTRY_WRITTEN: return "written";
    case ENTRY_NOT_TAKEN: return "not taken";
    case ENTRY_ALTERED: return "altered by the charger";
    case ENTRY_REFUSED: return "refused";
    case ENTRY_NO_ANSWER: return "no answer";
    case ENTRY_LOST: return "lost after the restart";
    default: return "?";
  }
}

struct PlanEntry {
  uint8_t item;     // ItemIndex
  uint8_t status;   // EntryStatus
  uint16_t before;  // what the register held when the run began
  uint16_t target;  // what it has to hold
  uint16_t found;   // what it held when this run last read it
};

// The word a register of the plan has to hold.
inline uint16_t targetWord(uint8_t index, const Profile &profile, uint8_t scale) {
  switch (index) {
    case I_EV_REG:
    case I_EV_REG2:
      return rawFromCentivolts(profile.absorbCv, scale);
    case I_EV_FLOAT:
    case I_EV_FLOAT2:
      return rawFromCentivolts(profile.floatCv, scale);
    case I_ET_FLOAT:
    case I_ET_FLOATLB:
    case I_ET_FLOAT2:
    case I_ET_FLOATLB2:
      return profile.absorbSeconds;
    case I_EV_HVD:
    case I_EVB_REF_LIM:
      return rawFromCentivolts(profile.hvdCv, scale);
    case I_EV_HVR:
      return rawFromCentivolts(profile.hvrCv, scale);
    case I_EV_LVD:
      return rawFromCentivolts(profile.lvdCv, scale);
    case I_EV_LVR:
      return rawFromCentivolts(profile.lvrCv, scale);
    default:
      return 0;  // EV_eq, EV_eq2, EV_tempcomp and ER_icomp: off
  }
}

// The plan is made of steps: one register, or a pair in which `first` has to stay below `second`.
struct PlanStep {
  uint8_t first;
  uint8_t second;
};

static const uint8_t PLAN_SINGLE = 0xFF;
static const uint8_t PLAN_STEPS = 13;

// Both custom banks get the same settings: the battery jumper picks the bank, and it cannot be
// read over Modbus. The regulation ceiling (Evb_ref_lim) is set to the HVD, so that a ceiling
// somebody stored below the new absorption voltage cannot hold the charger under it.
inline const PlanStep &planStep(uint8_t index) {
  static const PlanStep kSteps[PLAN_STEPS] = {
      {I_EV_EQ, PLAN_SINGLE},       {I_EV_EQ2, PLAN_SINGLE},    {I_EV_TEMPCOMP, PLAN_SINGLE},
      {I_EV_HVR, I_EV_HVD},         {I_EVB_REF_LIM, PLAN_SINGLE},
      {I_EV_FLOAT, I_EV_REG},       {I_ET_FLOAT, PLAN_SINGLE},  {I_ET_FLOATLB, PLAN_SINGLE},
      {I_EV_FLOAT2, I_EV_REG2},     {I_ET_FLOAT2, PLAN_SINGLE}, {I_ET_FLOATLB2, PLAN_SINGLE},
      {I_EV_LVD, I_EV_LVR},         {I_ER_ICOMP, PLAN_SINGLE}};
  return kSteps[index < PLAN_STEPS ? index : 0];
}

// True for a register the plan writes.
inline bool inPlan(uint8_t index) {
  if (index >= ITEM_COUNT) {
    return false;  // PLAN_SINGLE is no register
  }
  for (uint8_t s = 0; s < PLAN_STEPS; ++s) {
    if (planStep(s).first == index || planStep(s).second == index) {
      return true;
    }
  }
  return false;
}

// Fills entries[PLAN_ENTRIES] in the order of writing and returns how many of them have to be
// written. `before` must hold every register of the plan.
inline uint8_t buildPlan(const Snapshot &before, const Profile &profile, uint8_t scale, PlanEntry *entries) {
  uint8_t count = 0;
  uint8_t toWrite = 0;
  for (uint8_t s = 0; s < PLAN_STEPS; ++s) {
    const PlanStep &step = planStep(s);
    uint8_t order[2] = {step.first, step.second};
    uint8_t items = 1;
    if (step.second != PLAN_SINGLE) {
      items = 2;
      // The upper register goes first when its new word is above the word the lower one holds
      // now: the pair is then in order after either write. Otherwise the lower one goes first.
      // Its new word is below the new upper one, which is then no higher than the old lower
      // one, which was below the old upper one.
      if (targetWord(step.second, profile, scale) > before.raw[step.first]) {
        order[0] = step.second;
        order[1] = step.first;
      }
    }
    for (uint8_t i = 0; i < items; ++i) {
      PlanEntry &entry = entries[count++];
      entry.item = order[i];
      entry.before = before.raw[order[i]];
      entry.target = targetWord(order[i], profile, scale);
      entry.found = entry.before;
      entry.status = entry.before == entry.target ? (uint8_t)ENTRY_UNCHANGED : (uint8_t)ENTRY_PENDING;
      if (entry.status == ENTRY_PENDING) {
        ++toWrite;
      }
    }
  }
  return toWrite;
}

// ---------------------------------------------------------------------------------------------
// How a run went
// ---------------------------------------------------------------------------------------------

enum Outcome : uint8_t {
  OUTCOME_NONE = 0,        // no run has ended yet
  OUTCOME_PROGRAMMED,      // the controller holds the settings, has restarted, and they were read again
  OUTCOME_UNCHANGED,       // it already held them: nothing was written
  OUTCOME_REFUSED,         // nothing was written: see the Refusal bits
  OUTCOME_NO_LINK,         // nothing was written: the controller did not answer well enough to begin
  OUTCOME_STOPPED,         // a register did not take its word, and the run stopped there
  OUTCOME_RESTART_FAILED,  // everything was written, but the restart could not be confirmed
  OUTCOME_VERIFY_FAILED,   // written and restarted, but a register no longer holds its word
  OUTCOME_INTERRUPTED      // the client restarted during a run (see Programmer::startRecovery)
};

static const uint8_t OUTCOME_LAST = OUTCOME_INTERRUPTED;

inline const char *outcomeName(uint8_t outcome) {
  switch (outcome) {
    case OUTCOME_PROGRAMMED: return "programmed";
    case OUTCOME_UNCHANGED: return "unchanged";
    case OUTCOME_REFUSED: return "refused";
    case OUTCOME_NO_LINK: return "no link";
    case OUTCOME_STOPPED: return "stopped";
    case OUTCOME_RESTART_FAILED: return "restart failed";
    case OUTCOME_VERIFY_FAILED: return "verify failed";
    case OUTCOME_INTERRUPTED: return "interrupted";
    default: return "none";
  }
}

inline const char *outcomeText(uint8_t outcome) {
  switch (outcome) {
    case OUTCOME_PROGRAMMED: return "Programmed: the charger holds the settings and has restarted.";
    case OUTCOME_UNCHANGED: return "Nothing to do: the charger already held these settings.";
    case OUTCOME_REFUSED: return "Refused: nothing was written.";
    case OUTCOME_NO_LINK:
      return "Not done: the charger did not answer well enough to begin. Nothing was written.";
    case OUTCOME_STOPPED: return "Stopped part way: a register did not take its value.";
    case OUTCOME_RESTART_FAILED: return "Written, but the charger's restart could not be confirmed.";
    case OUTCOME_VERIFY_FAILED: return "Written and restarted, but a register no longer holds its value.";
    case OUTCOME_INTERRUPTED: return "Interrupted: the client restarted during the run.";
    default: return "No run has ended yet.";
  }
}

// True for an outcome that leaves the controller as the request wanted it.
inline bool outcomeIsGood(uint8_t outcome) { return outcome == OUTCOME_PROGRAMMED || outcome == OUTCOME_UNCHANGED; }

// Why a run was refused. The first eight are found by the run itself: from what it read, and
// from how the controller took its first restart command. The rest are the client's, which
// refuses before a run starts.
enum Refusal : uint32_t {
  REFUSE_PROFILE = 1ul << 0,         // the values are outside the limits (see the ProfileError bits)
  REFUSE_SCALE_UNPROVEN = 1ul << 1,  // how the controller encodes stored voltages could not be proven
  REFUSE_NOT_12V = 1ul << 2,         // not a 12 V system
  REFUSE_NOT_READ = 1ul << 3,        // a register the run needs gave no value
  REFUSE_FAULT = 1ul << 4,           // the controller reports a fault
  REFUSE_BATTERY_LOW = 1ul << 5,     // the battery is below the new LVR
  REFUSE_LOAD_CURRENT = 1ul << 6,    // current flows from the LOAD terminals, and the request does not allow that
  REFUSE_NO_RESTART = 1ul << 7,      // the restart command does not work on this controller
  REFUSE_NOT_BUILT = 1ul << 8,       // the client's firmware was built without programming
  REFUSE_STALE = 1ul << 9,           // the request was too old when the client could act on it
  REFUSE_NOT_LIFEPO4 = 1ul << 10,    // the client's battery is not configured as 12 V LiFePO4
  REFUSE_NO_CHARGER = 1ul << 11,     // the charger is not enabled, or did not answer in time
  REFUSE_JOURNAL = 1ul << 12,        // the client could not record the run in its flash
  REFUSE_WITHDRAWN = 1ul << 13       // a newer config no longer asked for it while it waited
};

static const uint8_t REFUSAL_BITS = 14;

inline const char *refusalText(uint32_t refusal) {
  switch (refusal) {
    case REFUSE_PROFILE: return "The values asked for are outside the limits this firmware allows.";
    case REFUSE_SCALE_UNPROVEN:
      return "How this charger encodes its stored voltages could not be proven from its setpoints, so no "
             "voltage can be written safely.";
    case REFUSE_NOT_12V: return "This is not a 12 V system. Only 12 V systems are programmed.";
    case REFUSE_NOT_READ: return "A register that programming needs did not answer.";
    case REFUSE_FAULT: return "The charger reports a fault. Clear it first.";
    case REFUSE_BATTERY_LOW:
      return "The battery is below the new LVR, so the load could stay switched off after the charger "
             "restarts. Try again with a charged battery.";
    case REFUSE_LOAD_CURRENT:
      return "Current is flowing from the charger's LOAD terminals. If this Opta is powered from them it "
             "could lose power part way. Confirm in the request that it is not.";
    case REFUSE_NO_RESTART:
      return "The restart command, which every change of settings has to end with, did not work on this "
             "charger. Nothing was written.";
    case REFUSE_NOT_BUILT: return "This client's firmware was built without charger programming.";
    case REFUSE_STALE:
      return "The request was more than a day old when the client could act on it, or the client's clock "
             "could not tell.";
    case REFUSE_NOT_LIFEPO4: return "The battery configured for this client is not a 12 V LiFePO4 battery.";
    case REFUSE_NO_CHARGER:
      return "The charger is not enabled on RS-485 for this client, or it did not answer within an hour.";
    case REFUSE_JOURNAL: return "The client could not record the run in its flash memory, so it did not go on.";
    case REFUSE_WITHDRAWN:
      return "A newer config for this client no longer asked for these values while the request waited, so "
             "they were not written.";
    default: return "A reason this firmware has no text for.";
  }
}

// How the last restart command of a run went.
enum RestartStatus : uint8_t {
  RESTART_NOT_NEEDED = 0,  // no restart command was sent
  RESTART_DONE,            // the controller answers again and reports no pending edit
  RESTART_REFUSED,         // it refused the command
  RESTART_NOT_DONE,        // it answers, and still reports the pending edit after two commands
  RESTART_NO_ANSWER,       // it has not answered since the command
  RESTART_UNCONFIRMED      // it answers, but its fault registers could not be read
};

static const uint8_t RESTART_STATUS_LAST = RESTART_UNCONFIRMED;

inline const char *restartText(uint8_t restart) {
  switch (restart) {
    case RESTART_NOT_NEEDED: return "No restart command was sent to the charger.";
    case RESTART_DONE: return "The charger restarted.";
    case RESTART_REFUSED: return "The charger refused the restart command.";
    case RESTART_NOT_DONE:
      return "The charger took the restart command twice and still reports that it waits for a restart.";
    case RESTART_NO_ANSWER: return "The charger has not answered since the restart command.";
    case RESTART_UNCONFIRMED:
      return "The charger answers again, but its fault registers could not be read, so its restart is not "
             "confirmed.";
    default: return "?";
  }
}

// What else a person should know about a run.
enum Note : uint16_t {
  NOTE_SW1_OFF = 1u << 0,              // the stored charge settings are not in use
  NOTE_SW2_OFF = 1u << 1,              // the stored LVD and LVR are not in use
  NOTE_SW3_ON = 1u << 2,               // automatic equalization is switched on
  NOTE_WAS_PENDING = 1u << 3,          // the controller was waiting for a restart when the run began
  NOTE_EDIT_FAULT_NOT_SEEN = 1u << 4,  // no "EEPROM setting edit" fault after the writes
  NOTE_VERIFY_INCOMPLETE = 1u << 5,    // a register could not be read again after the restart
  NOTE_NOTHING_WRITTEN = 1u << 6,      // an interrupted run that had not begun to write
  NOTE_AT_FIRST_RESTART = 1u << 7      // the client restarted while the charger restarted for the first time
};

static const uint8_t NOTE_BITS = 8;

inline const char *noteText(uint16_t note) {
  switch (note) {
    case NOTE_SW1_OFF:
      return "DIP switch 1 is OFF: the charger does not use the stored charge settings until it is ON.";
    case NOTE_SW2_OFF:
      return "DIP switch 2 is OFF: the load is disconnected at the built-in 11.50 V, not at the stored LVD, "
             "until it is ON.";
    case NOTE_SW3_ON:
      return "DIP switch 3 is ON: automatic equalization is switched on. Set it OFF for a lithium battery.";
    case NOTE_WAS_PENDING: return "The charger was already waiting for a restart when the run began.";
    case NOTE_EDIT_FAULT_NOT_SEEN:
      return "The charger did not report the 'EEPROM setting edit' fault after the writes. The specification "
             "says it does, so nothing here shows that the restart command did its work.";
    case NOTE_VERIFY_INCOMPLETE:
      return "A register could not be read again after the restart. The next charger check shows what the "
             "charger holds.";
    case NOTE_NOTHING_WRITTEN: return "Nothing had been written.";
    case NOTE_AT_FIRST_RESTART:
      return "The client restarted at the moment the charger restarted. If this Opta is powered from the "
             "charger's LOAD terminals it cannot program the charger: power it from the battery.";
    default: return "A note this firmware has no text for.";
  }
}

// Which of the readings below a result holds.
enum Seen : uint8_t {
  SEEN_BATTERY = 1u << 0,
  SEEN_LOAD_CURRENT = 1u << 1,
  SEEN_DIP = 1u << 2,
  SEEN_FAULTS = 1u << 3  // both fault words
};

static const uint8_t LOAD_STATE_NOT_READ = 0xFF;
static const uint8_t LOAD_STATE_OTHER = 0xFE;  // a word that does not fit into a byte: no state the controller has

// More load current than this counts as "something is powered from the LOAD terminals".
static const double LOAD_CURRENT_LIMIT_AMPS = 0.05;

inline double ampsFromRaw(uint16_t raw) { return (double)(int16_t)raw * 79.16 / 32768.0; }

inline uint8_t loadStateOf(uint16_t raw) { return raw < LOAD_STATE_OTHER ? (uint8_t)raw : LOAD_STATE_OTHER; }

struct ProgramResult {
  uint8_t outcome;         // Outcome
  uint8_t restart;         // RestartStatus
  uint8_t scale;           // ScaleId the voltages were written with, SCALE_UNKNOWN when it was not proven
  uint8_t liveScale;       // ScaleId of batteryRaw, SCALE_UNKNOWN when the lower of the two readings was used
  uint8_t planned;         // entries[] in use: PLAN_ENTRIES, or 0 when the run ended before the plan was made
  uint8_t seen;            // Seen bits: which of the readings below were read
  uint8_t loadStateBefore; // load_state when the run began, or LOAD_STATE_NOT_READ
  uint8_t loadStateEdit;   // after the last write, while the controller waited for its restart
  uint8_t loadStateAfter;  // at the end of the run
  uint8_t probes;          // requests the controller left unanswered after the last restart command
  bool wroteAny;           // a write request was sent
  bool followUp;           // the run could not learn whether the controller restarted: the client owes it another
                           // look, and reports once more (see Programmer::controllerUnknown)
  uint16_t profileErrors;  // ProfileError bits
  uint16_t notes;          // Note bits
  uint32_t refusals;       // Refusal bits
  Profile profile;         // what was asked for
  uint16_t batteryRaw;     // adc_vb_f when the run began
  uint16_t loadCurrentRaw; // adc_il_f
  uint16_t dip;            // dip_switch
  uint16_t arrayFault;     // array_fault
  uint16_t loadFault;      // load_fault
  PlanEntry entries[PLAN_ENTRIES];
};

inline void clearResult(ProgramResult *result) {
  memset(result, 0, sizeof(*result));
  result->loadStateBefore = LOAD_STATE_NOT_READ;
  result->loadStateEdit = LOAD_STATE_NOT_READ;
  result->loadStateAfter = LOAD_STATE_NOT_READ;
}

inline uint8_t countEntries(const ProgramResult &result, uint8_t status) {
  uint8_t count = 0;
  for (uint8_t i = 0; i < result.planned && i < PLAN_ENTRIES; ++i) {
    if (result.entries[i].status == status) {
      ++count;
    }
  }
  return count;
}

// A result for a request the client refuses before a run starts.
inline void refusedResult(const Profile &profile, uint16_t profileErrors, uint32_t refusals, ProgramResult *result) {
  clearResult(result);
  result->outcome = OUTCOME_REFUSED;
  result->profile = profile;
  result->profileErrors = profileErrors;
  result->refusals = refusals;
}

// A result for a run that the client's own restart cut short before it sent anything that
// changes the controller.
inline void interruptedResult(ProgramResult *result) {
  clearResult(result);
  result->outcome = OUTCOME_INTERRUPTED;
  result->notes = NOTE_NOTHING_WRITTEN;
}

// The battery voltage a result holds, in volts: with the scale of the live readings when that
// was proven, and otherwise the lower of the two readings it could be.
inline double batteryVolts(const ProgramResult &result) {
  return voltsFromRaw(result.batteryRaw, result.liveScale != SCALE_UNKNOWN ? result.liveScale : (uint8_t)SCALE_96667);
}

// ---------------------------------------------------------------------------------------------
// The run
// ---------------------------------------------------------------------------------------------

static const uint16_t RESTART_WAIT_MS = 3000;  // the line is left alone for this long after a restart command
static const uint16_t PROBE_GAP_MS = 500;      // and for this long after a request the restarting controller missed
static const uint8_t BACK_PROBES = 20;         // such requests before the run gives up on the controller
static const uint8_t WRITE_ATTEMPTS = 2;       // writes of one register
static const uint8_t RESTART_ATTEMPTS = 2;     // restart commands that the controller took without restarting
static const uint8_t READ_ATTEMPTS = 2;        // requests for a register that stays silent
static const uint8_t VERIFY_MISSES = 3;        // registers in a row without a value end the last reading

// The two points at which a run waits for its caller before it goes on.
enum Commit : uint8_t {
  COMMIT_NONE = 0,
  COMMIT_RESTART,  // next is the first restart command: nothing has been sent that changes the controller
  COMMIT_WRITE     // next is the first write: the controller has restarted once, and nothing is written
};

// Runs one programming pass a few requests at a time, so that the caller's loop keeps running:
//
//   programmer.start(profile, loadCurrentAllowed);
//   loop:  ended = programmer.step(bus, 4);
//          if (programmer.awaiting() != COMMIT_NONE) { record that point in flash; programmer.go(); }
//          wait: one Modbus timeout if programmer.missed(), and at least programmer.pauseMs()
//   then:  programmer.result(), and programmer.controllerUnknown()
//
// It keeps no clock. It asks for the pauses it needs, and counts requests where it has to give up.
class Programmer {
 public:
  Programmer() { reset(); }

  // Starts a run. loadCurrentAllowed says that current may flow from the LOAD terminals: the
  // person who asked has confirmed that the client is not powered from them.
  void start(const Profile &profile, bool loadCurrentAllowed) {
    reset();
    result_.profile = profile;
    loadAllowed_ = loadCurrentAllowed;
    reader_.start();
    phase_ = PHASE_READ_BEFORE;
  }

  // Starts a run that writes nothing, for a client that restarted during a run. It makes sure
  // the controller is not left waiting for a restart: it asks, and sends the restart command if
  // it is. beforeWrites says that the run had not been released to write (it stopped between
  // COMMIT_RESTART and COMMIT_WRITE). The outcome is OUTCOME_INTERRUPTED, or OUTCOME_NO_LINK
  // when the controller's fault registers could not be read.
  void startRecovery(bool beforeWrites) {
    reset();
    recovery_ = true;
    if (beforeWrites) {
      result_.notes = (uint16_t)(NOTE_NOTHING_WRITTEN | NOTE_AT_FIRST_RESTART);
    }
    phase_ = PHASE_RECOVER;
  }

  // Takes a run up again that ended without learning whether the controller restarted
  // (controllerUnknown()), once the link works again. It keeps what the run found and did, asks
  // the controller whether it still waits for a restart, sends the command if it does, reads the
  // registers once more and ends with the outcome the run would have had. Returns false, and
  // does nothing, when the last run is not such a run.
  bool resume() {
    if (recovery_ || !controllerUnknown()) {
      return false;
    }
    result_.notes = (uint16_t)(result_.notes & ~(uint16_t)NOTE_VERIFY_INCOMPLETE);
    result_.loadStateAfter = LOAD_STATE_NOT_READ;
    missed_ = false;
    pauseMs_ = 0;
    stage_ = 0;
    readTries_ = 0;
    phase_ = PHASE_RECOVER;
    return true;
  }

  // Makes up to `budget` requests. Returns true once no run is under way any more.
  bool step(Bus &bus, uint8_t budget) {
    missed_ = false;
    pauseMs_ = 0;
    while (budget > 0 && active() && awaiting() == COMMIT_NONE && !missed_ && pauseMs_ == 0) {
      bool asked = false;
      switch (phase_) {
        case PHASE_READ_BEFORE: asked = stepReadBefore(bus); break;
        case PHASE_RECOVER: asked = stepRecover(bus); break;
        case PHASE_RESTART: asked = stepRestart(bus); break;
        case PHASE_WAIT_BACK: asked = stepWaitBack(bus); break;
        case PHASE_WRITE: asked = stepWrite(bus); break;
        case PHASE_OBSERVE: asked = stepObserve(bus); break;
        default: asked = stepVerify(bus); break;  // PHASE_VERIFY: the phases that wait are not stepped
      }
      if (asked) {
        --budget;
      }
    }
    return !active();
  }

  // COMMIT_RESTART or COMMIT_WRITE while the run waits for go(). The caller records that point
  // in its flash first, so that a client that loses power from here on knows at its next start
  // how far the run had come. If it cannot record it, it calls refuse().
  uint8_t awaiting() const {
    if (phase_ == PHASE_AWAIT_RESTART) {
      return COMMIT_RESTART;
    }
    return phase_ == PHASE_AWAIT_WRITE ? (uint8_t)COMMIT_WRITE : (uint8_t)COMMIT_NONE;
  }

  void go() {
    if (phase_ == PHASE_AWAIT_RESTART) {
      firstRestart_ = true;
      restartAttempts_ = 0;
      phase_ = PHASE_RESTART;
    } else if (phase_ == PHASE_AWAIT_WRITE) {
      entry_ = 0;
      readingBack_ = false;
      writeAttempts_ = 0;
      readTries_ = 0;
      phase_ = PHASE_WRITE;
    }
  }

  // Ends a run that waits for go(), as refused for `reasons` (Refusal bits). Nothing is written.
  void refuse(uint32_t reasons) {
    if (awaiting() == COMMIT_NONE) {
      return;
    }
    result_.refusals |= reasons;
    finish(OUTCOME_REFUSED);
  }

  bool active() const { return phase_ != PHASE_IDLE && phase_ != PHASE_ENDED; }
  bool ended() const { return phase_ == PHASE_ENDED; }
  // The last step ended on a request that got no reply. A late reply may still be on its way:
  // leave the line alone for one Modbus timeout before the next step.
  bool missed() const { return missed_; }
  // Leave the line alone for at least this long before the next step.
  uint16_t pauseMs() const { return pauseMs_; }

  // Complete once ended().
  const ProgramResult &result() const { return result_; }

  // After a run has ended: the controller may still be waiting for a restart, and the run could
  // not find out. The caller owes it another look once the link works again: resume() after a
  // run that wrote, startRecovery() after one that was itself a recovery.
  // The result says the same as followUp, for the server.
  bool controllerUnknown() const { return phase_ == PHASE_ENDED && result_.followUp; }

  // The registers as the run found them. Complete once the plan is made.
  const Snapshot &before() const { return reader_.snapshot(); }

 private:
  enum Phase : uint8_t {
    PHASE_IDLE = 0,
    PHASE_READ_BEFORE,    // every register of the charger check
    PHASE_RECOVER,        // a recovery, or a run taken up again: the two fault words
    PHASE_AWAIT_RESTART,
    PHASE_RESTART,        // the restart command
    PHASE_WAIT_BACK,      // asking until the controller answers, then its fault words
    PHASE_AWAIT_WRITE,
    PHASE_WRITE,          // one register after the other: write, read back
    PHASE_OBSERVE,        // the load state and the fault words while the controller waits for its restart
    PHASE_VERIFY,         // the plan's registers once more, and the load state
    PHASE_ENDED
  };

  enum Fetch : uint8_t { FETCH_AGAIN = 0, FETCH_VALUE, FETCH_NONE };

  SnapshotReader reader_;
  ProgramResult result_;
  uint8_t phase_;
  bool recovery_;
  bool loadAllowed_;
  bool missed_;
  bool firstRestart_;  // the restart command under way is the one before the writes
  bool stopped_;       // a register did not take its word
  bool readingBack_;   // the register at entry_ has been written and is to be read
  IoStatus lastWrite_;
  uint8_t entry_;
  uint8_t stage_;
  uint8_t writeAttempts_;
  uint8_t restartAttempts_;
  uint8_t readTries_;
  uint8_t verifyMisses_;
  uint8_t toWrite_;
  uint16_t pauseMs_;
  uint16_t heldFault_;  // array_fault, kept until load_fault has been read too

  void reset() {
    clearResult(&result_);
    reader_.cancel();
    phase_ = PHASE_IDLE;
    recovery_ = false;
    loadAllowed_ = false;
    missed_ = false;
    firstRestart_ = false;
    stopped_ = false;
    readingBack_ = false;
    lastWrite_ = SSIO_OK;
    entry_ = 0;
    stage_ = 0;
    writeAttempts_ = 0;
    restartAttempts_ = 0;
    readTries_ = 0;
    verifyMisses_ = 0;
    toWrite_ = 0;
    pauseMs_ = 0;
    heldFault_ = 0;
  }

  void finish(uint8_t outcome) {
    result_.outcome = outcome;
    // A restart command went out and nothing shows that it worked. That matters when this run
    // wrote, and for a recovery, which exists to settle exactly this.
    const bool restartOpen = result_.restart == RESTART_NO_ANSWER || result_.restart == RESTART_UNCONFIRMED;
    result_.followUp = recovery_ ? (restartOpen || outcome == OUTCOME_NO_LINK) : (restartOpen && result_.wroteAny);
    phase_ = PHASE_ENDED;
  }

  static bool editPending(uint16_t arrayFault, uint16_t loadFault) {
    return (arrayFault & ARRAY_FAULT_EEPROM_EDIT) != 0 || (loadFault & LOAD_FAULT_EEPROM_EDIT) != 0;
  }

  // One request for a register. FETCH_VALUE with *raw set, FETCH_AGAIN when it was silent and is
  // to be asked once more (the step ends there), FETCH_NONE when it gives no value: the
  // controller refused it, or it was silent READ_ATTEMPTS times.
  Fetch fetch(Bus &bus, uint16_t addr, uint16_t *raw) {
    uint16_t word = 0;
    const IoStatus status = bus.readRegister(addr, &word);
    if (status == SSIO_OK) {
      readTries_ = 0;
      *raw = word;
      return FETCH_VALUE;
    }
    if (status != SSIO_REFUSED) {
      missed_ = true;
      if (++readTries_ < READ_ATTEMPTS) {
        return FETCH_AGAIN;
      }
    }
    readTries_ = 0;
    return FETCH_NONE;
  }

  bool stepReadBefore(Bus &bus) {
    const bool passEnded = reader_.step(bus, 1);
    missed_ = reader_.missed();
    if (passEnded) {
      if (reader_.done()) {
        decide();
      } else {
        finish(OUTCOME_NO_LINK);
      }
    }
    return true;
  }

  // The registers are read: refuse the run, find that there is nothing to do, or make the plan.
  void decide() {
    const Snapshot &snapshot = reader_.snapshot();
    const ScaleVerdict settings = judgeSettingsScale(snapshot);
    uint8_t liveScale = SCALE_UNKNOWN;
    uint8_t liveSource = LIVE_SOURCE_NONE;
    judgeLiveScale(snapshot, settings.scale, &liveScale, &liveSource);
    result_.scale = settings.scale;
    result_.liveScale = liveScale;

    uint32_t refusals = 0;
    bool pending = false;
    result_.profileErrors = validateProfile(result_.profile);
    if (result_.profileErrors != 0) {
      refusals |= REFUSE_PROFILE;
    }
    for (uint8_t i = 0; i < ITEM_COUNT; ++i) {
      const bool needed = inPlan(i) || i == I_DIP_SWITCH || i == I_ADC_VB_F || i == I_ADC_IL_F ||
                          i == I_ARRAY_FAULT || i == I_LOAD_FAULT;
      if (needed && !have(snapshot, i)) {
        refusals |= REFUSE_NOT_READ;
      }
    }
    if (settings.scale == SCALE_UNKNOWN) {
      refusals |= REFUSE_SCALE_UNPROVEN;
    }
    if (have(snapshot, I_ADC_VB_F)) {
      result_.seen |= SEEN_BATTERY;
      result_.batteryRaw = snapshot.raw[I_ADC_VB_F];
      if (systemVolts(snapshot) != 12) {
        refusals |= REFUSE_NOT_12V;
      } else if (batteryVolts(result_) * 100.0 + 0.0001 < (double)result_.profile.lvrCv) {
        // After its restart the controller may wait for the reconnect voltage before it switches
        // the load on. With the scale of the live readings not proven, the lower reading counts.
        refusals |= REFUSE_BATTERY_LOW;
      }
    }
    if (have(snapshot, I_ADC_IL_F)) {
      result_.seen |= SEEN_LOAD_CURRENT;
      result_.loadCurrentRaw = snapshot.raw[I_ADC_IL_F];
      if (!loadAllowed_ && ampsFromRaw(result_.loadCurrentRaw) > LOAD_CURRENT_LIMIT_AMPS) {
        refusals |= REFUSE_LOAD_CURRENT;
      }
    }
    if (have(snapshot, I_ARRAY_FAULT) && have(snapshot, I_LOAD_FAULT)) {
      result_.seen |= SEEN_FAULTS;
      result_.arrayFault = snapshot.raw[I_ARRAY_FAULT];
      result_.loadFault = snapshot.raw[I_LOAD_FAULT];
      if ((result_.arrayFault & (uint16_t)~ARRAY_FAULT_EEPROM_EDIT) != 0 ||
          (result_.loadFault & (uint16_t)~LOAD_FAULT_EEPROM_EDIT) != 0) {
        refusals |= REFUSE_FAULT;
      }
      if (editPending(result_.arrayFault, result_.loadFault)) {
        pending = true;
        result_.notes |= NOTE_WAS_PENDING;
      }
    }
    if (have(snapshot, I_DIP_SWITCH)) {
      result_.seen |= SEEN_DIP;
      result_.dip = snapshot.raw[I_DIP_SWITCH];
      if ((result_.dip & DIP_CUSTOM_CHARGE) == 0) {
        result_.notes |= NOTE_SW1_OFF;
      }
      if ((result_.dip & DIP_CUSTOM_LVD) == 0) {
        result_.notes |= NOTE_SW2_OFF;
      }
      if ((result_.dip & DIP_EQUALIZE) != 0) {
        result_.notes |= NOTE_SW3_ON;
      }
    }
    if (have(snapshot, I_LOAD_STATE)) {
      result_.loadStateBefore = loadStateOf(snapshot.raw[I_LOAD_STATE]);
    }

    if (refusals != 0) {
      result_.refusals = refusals;
      finish(OUTCOME_REFUSED);
      return;
    }
    toWrite_ = buildPlan(snapshot, result_.profile, settings.scale, result_.entries);
    result_.planned = PLAN_ENTRIES;
    if (toWrite_ == 0 && !pending) {
      finish(OUTCOME_UNCHANGED);
      return;
    }
    phase_ = PHASE_AWAIT_RESTART;
  }

  // A recovery, or a run taken up again: is the controller waiting for a restart?
  bool stepRecover(Bus &bus) {
    uint16_t raw = 0;
    const Fetch got = fetch(bus, item(stage_ == 0 ? I_ARRAY_FAULT : I_LOAD_FAULT).addr, &raw);
    if (got == FETCH_AGAIN) {
      return true;
    }
    if (got == FETCH_NONE) {
      if (recovery_) {
        finish(OUTCOME_NO_LINK);
      } else {
        conclude();  // it still does not know
      }
      return true;
    }
    if (stage_ == 0) {
      heldFault_ = raw;
      stage_ = 1;
      return true;
    }
    const bool pending = editPending(heldFault_, raw);
    if (recovery_) {
      result_.seen |= SEEN_FAULTS;
      result_.arrayFault = heldFault_;
      result_.loadFault = raw;
      if (pending) {
        result_.notes |= NOTE_WAS_PENDING;
      }
    }
    if (pending) {
      firstRestart_ = false;
      restartAttempts_ = 0;
      phase_ = PHASE_RESTART;
    } else {
      if (!recovery_) {
        result_.restart = RESTART_DONE;  // it answers, and no longer waits
      }
      beginVerify();
    }
    return true;
  }

  bool stepRestart(Bus &bus) {
    ++restartAttempts_;
    result_.probes = 0;
    const IoStatus status = bus.writeCoil(COIL_RESTART);
    if (status == SSIO_REFUSED) {
      restartSettled(RESTART_REFUSED);
      return true;
    }
    if (status != SSIO_OK) {
      missed_ = true;  // a controller may restart before it acknowledges
    }
    // A controller that restarts is silent for a while. One that did not restart answers the
    // first request after the wait, and its fault words say so.
    pauseMs_ = RESTART_WAIT_MS;
    stage_ = 0;
    readTries_ = 0;
    phase_ = PHASE_WAIT_BACK;
    return true;
  }

  bool stepWaitBack(Bus &bus) {
    if (stage_ == 0) {
      // Asked until it answers: a restarting controller misses as many requests as it takes.
      uint16_t word = 0;
      const IoStatus status = bus.readRegister(item(I_ARRAY_FAULT).addr, &word);
      if (status == SSIO_OK) {
        heldFault_ = word;
        stage_ = 1;
        readTries_ = 0;
      } else if (status == SSIO_REFUSED) {
        restartSettled(RESTART_UNCONFIRMED);
      } else {
        missed_ = true;
        ++result_.probes;
        if (result_.probes >= BACK_PROBES) {
          restartSettled(RESTART_NO_ANSWER);
        } else {
          pauseMs_ = PROBE_GAP_MS;
        }
      }
      return true;
    }
    uint16_t raw = 0;
    const Fetch got = fetch(bus, item(I_LOAD_FAULT).addr, &raw);
    if (got == FETCH_AGAIN) {
      return true;
    }
    if (got == FETCH_NONE) {
      restartSettled(RESTART_UNCONFIRMED);
    } else if (!editPending(heldFault_, raw)) {
      restartSettled(RESTART_DONE);
    } else if (restartAttempts_ < RESTART_ATTEMPTS) {
      phase_ = PHASE_RESTART;  // it answers and still waits: the command did not do its work
    } else {
      restartSettled(RESTART_NOT_DONE);
    }
    return true;
  }

  // A restart command has come to its end, one way or the other.
  void restartSettled(uint8_t status) {
    result_.restart = status;
    if (!firstRestart_) {
      if (status == RESTART_NO_ANSWER) {
        conclude();
      } else {
        beginVerify();
      }
      return;
    }
    // The restart before the writes. Nothing is written yet, and nothing will be unless this
    // controller has just shown that it takes the command.
    if (status == RESTART_DONE) {
      firstRestart_ = false;
      restartAttempts_ = 0;
      if (toWrite_ > 0) {
        phase_ = PHASE_AWAIT_WRITE;
      } else {
        beginVerify();  // it held the settings already, and only had a restart to make up for
      }
    } else if (status == RESTART_REFUSED || status == RESTART_NOT_DONE) {
      result_.refusals |= REFUSE_NO_RESTART;
      finish(OUTCOME_REFUSED);
    } else {
      finish(OUTCOME_NO_LINK);
    }
  }

  // The register at entry_: written, then read back. What it reads decides, not the
  // acknowledgement.
  bool stepWrite(Bus &bus) {
    while (entry_ < PLAN_ENTRIES && result_.entries[entry_].status != ENTRY_PENDING) {
      ++entry_;
    }
    if (entry_ >= PLAN_ENTRIES) {
      beginObserve();
      return false;
    }
    PlanEntry &entry = result_.entries[entry_];
    const uint16_t addr = item(entry.item).addr;
    if (!readingBack_) {
      ++writeAttempts_;
      result_.wroteAny = true;
      lastWrite_ = bus.writeRegister(addr, entry.target);
      readingBack_ = true;
      readTries_ = 0;
      if (lastWrite_ != SSIO_OK && lastWrite_ != SSIO_REFUSED) {
        missed_ = true;  // its acknowledgement may still come: not into the read that follows
      }
      return true;
    }
    uint16_t raw = 0;
    const Fetch got = fetch(bus, addr, &raw);
    if (got == FETCH_AGAIN) {
      return true;
    }
    readingBack_ = false;
    if (got == FETCH_NONE) {
      entry.status = ENTRY_NO_ANSWER;
      stopWrites();
      return true;
    }
    entry.found = raw;
    if (raw == entry.target) {
      entry.status = ENTRY_WRITTEN;
      writeAttempts_ = 0;
      ++entry_;
    } else if (raw != entry.before) {
      entry.status = ENTRY_ALTERED;
      stopWrites();
    } else if (lastWrite_ == SSIO_REFUSED) {
      entry.status = ENTRY_REFUSED;
      stopWrites();
    } else if (writeAttempts_ >= WRITE_ATTEMPTS) {
      entry.status = ENTRY_NOT_TAKEN;
      stopWrites();
    }
    // Otherwise the register still reads what it held, and it is written once more.
    return true;
  }

  void stopWrites() {
    stopped_ = true;
    beginObserve();
  }

  void beginObserve() {
    stage_ = 0;
    readTries_ = 0;
    phase_ = PHASE_OBSERVE;
  }

  // What the controller does while it waits for its restart: the load state, and whether it
  // reports the pending edit as the specification says. None of it changes what the run does.
  bool stepObserve(Bus &bus) {
    static const uint8_t kItems[3] = {I_LOAD_STATE, I_ARRAY_FAULT, I_LOAD_FAULT};
    uint16_t raw = 0;
    const Fetch got = fetch(bus, item(kItems[stage_]).addr, &raw);
    if (got == FETCH_AGAIN) {
      return true;
    }
    if (got == FETCH_VALUE) {
      if (stage_ == 0) {
        result_.loadStateEdit = loadStateOf(raw);
      } else if (stage_ == 1) {
        heldFault_ = raw;
      } else if (!editPending(heldFault_, raw)) {
        result_.notes |= NOTE_EDIT_FAULT_NOT_SEEN;
      }
    } else if (stage_ == 1) {
      stage_ = 2;  // without the array faults the load faults cannot settle the question
    }
    if (++stage_ >= 3) {
      firstRestart_ = false;
      restartAttempts_ = 0;
      phase_ = PHASE_RESTART;
    }
    return true;
  }

  void beginVerify() {
    entry_ = 0;
    readTries_ = 0;
    verifyMisses_ = 0;
    phase_ = PHASE_VERIFY;
  }

  // The registers once more, as the controller holds them now: a word that was only in its
  // working memory would be gone after the restart. Then the load state.
  bool stepVerify(Bus &bus) {
    while (entry_ < result_.planned && result_.entries[entry_].status != ENTRY_WRITTEN &&
           result_.entries[entry_].status != ENTRY_UNCHANGED) {
      ++entry_;
    }
    uint16_t raw = 0;
    if (entry_ < result_.planned) {
      PlanEntry &entry = result_.entries[entry_];
      const Fetch got = fetch(bus, item(entry.item).addr, &raw);
      if (got == FETCH_AGAIN) {
        return true;
      }
      ++entry_;
      if (got == FETCH_NONE) {
        result_.notes |= NOTE_VERIFY_INCOMPLETE;
        if (++verifyMisses_ >= VERIFY_MISSES) {
          conclude();  // the controller has stopped answering: the rest would only take time
        }
        return true;
      }
      verifyMisses_ = 0;
      entry.found = raw;
      if (raw != entry.target) {
        entry.status = ENTRY_LOST;
      }
      return true;
    }
    const Fetch got = fetch(bus, item(I_LOAD_STATE).addr, &raw);
    if (got == FETCH_AGAIN) {
      return true;
    }
    if (got == FETCH_VALUE) {
      result_.loadStateAfter = loadStateOf(raw);
    }
    conclude();
    return true;
  }

  void conclude() {
    if (recovery_) {
      finish(OUTCOME_INTERRUPTED);
    } else if (stopped_) {
      finish(OUTCOME_STOPPED);
    } else if (result_.restart != RESTART_DONE) {
      finish(OUTCOME_RESTART_FAILED);
    } else if (countEntries(result_, ENTRY_LOST) != 0) {
      finish(OUTCOME_VERIFY_FAILED);
    } else {
      finish(OUTCOME_PROGRAMMED);
    }
  }
};

// ---------------------------------------------------------------------------------------------
// Requests, and what a client owes the controller across its own restarts
// ---------------------------------------------------------------------------------------------
//
// A request reaches a client in its config, with an id: the server's time when a person asked.
// The client may get the same config again, may get it days late, and may lose power at any
// moment of a run. The Supervisor decides what is done about a request and when. It keeps a
// journal in the client's flash that says how far a run has come:
//
//   JOURNAL_IDLE         nothing is under way, and nothing is owed
//   JOURNAL_TAKEN        a request was taken on; nothing has been sent that changes the controller
//   JOURNAL_RESTARTING   the first restart command may have gone out; nothing has been written
//   JOURNAL_WRITING      registers may have been written: the controller may wait for a restart
//
// A client that starts with RESTARTING or WRITING in its journal owes the controller a look, and
// the restart command if it waits (Programmer::startRecovery). So does a client whose run ended
// without learning whether the controller restarted (Programmer::resume). That duty comes before
// any new request. It is tried as soon as the charger answers, then every 10 minutes, and once a
// day after three tries.
//
// A run holds the client for as long as it takes: about 20 seconds, a minute at most. Nothing
// else talks to the controller meanwhile, and the time between the first write and the restart
// that ends the run is as short as the line allows.

enum JournalPhase : uint8_t { JOURNAL_IDLE = 0, JOURNAL_TAKEN, JOURNAL_RESTARTING, JOURNAL_WRITING };

static const uint8_t JOURNAL_PHASE_LAST = JOURNAL_WRITING;

struct Journal {
  uint32_t requestId;  // the newest request this client took on
  uint8_t phase;       // JournalPhase: how far its run has come
};

// The journal of a client whose journal cannot be read: it may owe the controller a restart.
inline Journal cautiousJournal() {
  const Journal journal = {0, JOURNAL_WRITING};
  return journal;
}

// What the Supervisor needs from the client, besides the controller.
class Port : public Bus {
 public:
  // Writes the journal to the client's flash. True only once it is there, whole.
  virtual bool saveJournal(const Journal &journal) = 0;
  // Leaves the line alone between two requests of a run: for at least `ms`, and for one Modbus
  // timeout when the last request got no reply. Keeps the client's watchdog fed.
  virtual void rest(uint16_t ms, bool afterMiss) = 0;
  // The client's millis().
  virtual uint32_t millisNow() = 0;
};

static const uint32_t REQUEST_WAIT_MS = 3600000ul;        // a request waits this long for the clock and the charger
static const double REQUEST_MAX_AGE_SECONDS = 86400.0;    // and may be this old when it arrives
static const double REQUEST_MAX_AHEAD_SECONDS = 3600.0;   // or this far ahead of the client's clock
static const double CLOCK_SET_EPOCH = 1600000000.0;       // 2020-09-13: a clock that reads less was never set
static const uint32_t DUTY_RETRY_MS = 600000ul;           // between two tries of a duty
static const uint32_t DUTY_SLOW_RETRY_MS = 86400000ul;    // and after DUTY_FAST_TRIES of them
static const uint8_t DUTY_FAST_TRIES = 3;
static const uint32_t JOURNAL_RETRY_MS = 600000ul;        // between two tries to write a journal that flash did not take
static const uint16_t RUN_STEP_LIMIT = 2000;              // a run has under 400 steps: no run goes on beyond this

// True for a request id that a client with the time `epochNow` may still act on.
inline bool requestIsFresh(uint32_t requestId, double epochNow) {
  const double asked = (double)requestId;
  return epochNow >= CLOCK_SET_EPOCH && asked >= epochNow - REQUEST_MAX_AGE_SECONDS &&
         asked <= epochNow + REQUEST_MAX_AHEAD_SECONDS;
}

class Supervisor {
 public:
  explicit Supervisor(uint16_t stepLimit = RUN_STEP_LIMIT) : stepLimit_(stepLimit) {
    const Journal none = {0, JOURNAL_IDLE};
    begin(none, 0);
  }

  // Once, when the client starts, with the journal from its flash: cautiousJournal() when the
  // file is there and cannot be read, {0, JOURNAL_IDLE} when there is none.
  void begin(const Journal &journal, uint32_t nowMs) {
    programmer_ = Programmer();
    journal_ = journal;
    if (journal_.phase > JOURNAL_PHASE_LAST) {
      journal_.phase = JOURNAL_WRITING;  // a phase of a newer firmware: the cautious reading
    }
    journalUnsaved_ = false;
    journalTryMs_ = nowMs;
    waiting_ = false;
    taken_ = false;
    waitingId_ = 0;
    waitingLoadAllowed_ = false;
    waitingSinceMs_ = nowMs;
    memset(&waitingProfile_, 0, sizeof(waitingProfile_));
    refusedId_ = 0;
    dutyForced_ = false;
    dutyTries_ = 0;
    dutyTryMs_ = nowMs;
    reportDue_ = false;
    reportRequestId_ = 0;
    reportMs_ = nowMs;
    clearResult(&report_);
    if (journal_.phase == JOURNAL_TAKEN) {
      // The client restarted between taking the request on and its run.
      ProgramResult result;
      interruptedResult(&result);
      post(journal_.requestId, result, nowMs);
      journal_.phase = JOURNAL_IDLE;
      journalUnsaved_ = true;
      journalTryMs_ = nowMs - JOURNAL_RETRY_MS;  // due at the first service()
    } else if (dutyOwed()) {
      dutyForced_ = true;
    }
  }

  // A config carried a request. `profileErrors` is what readProfile() found, `clientRefusals`
  // the Refusal bits the client found by itself (REFUSE_NOT_BUILT, REFUSE_NOT_LIFEPO4,
  // REFUSE_NO_CHARGER). A request this client has dealt with is ignored: a config is sent again
  // when its acknowledgement is lost. A new request takes the place of one that still waits.
  // Returns false for a request that is ignored, true for one that waits now or was refused.
  bool request(uint32_t requestId, const Profile &profile, uint16_t profileErrors, uint32_t clientRefusals,
               bool loadCurrentAllowed, uint32_t nowMs) {
    if (requestId == 0 || requestId <= journal_.requestId || requestId == refusedId_ ||
        (waiting_ && requestId <= waitingId_)) {
      return false;
    }
    dropWaiting(nowMs);
    uint32_t refusals = clientRefusals;
    if (profileErrors != 0) {
      refusals |= REFUSE_PROFILE;
    }
    if (refusals != 0) {
      refusedId_ = requestId;
      ProgramResult result;
      refusedResult(profile, profileErrors, refusals, &result);
      post(requestId, result, nowMs);
      return true;
    }
    waiting_ = true;
    taken_ = false;
    waitingId_ = requestId;
    waitingProfile_ = profile;
    waitingLoadAllowed_ = loadCurrentAllowed;
    waitingSinceMs_ = nowMs;
    dutyForced_ = true;  // a duty that is owed is tried now, and not at its next turn
    return true;
  }

  // Once per pass of the client's loop. `epochNow` is the client's clock, or anything below
  // CLOCK_SET_EPOCH while it is not set. `ready` says that the controller can be talked to now:
  // it answered its last poll, and nothing forbids it (a firmware update that is being staged).
  // Returns true when it talked to the controller: that held the caller for a whole run.
  bool service(Port &port, double epochNow, bool ready) {
    const uint32_t nowMs = port.millisNow();
    if (journalUnsaved_ && (uint32_t)(nowMs - journalTryMs_) >= JOURNAL_RETRY_MS) {
      saveJournal(port, nowMs);
    }
    if (dutyOwed()) {
      if (ready && dutyTurn(nowMs)) {
        // The run the duty is about is taken up again, if this client still holds it. After a
        // restart of the client, or after a recovery, it holds none: a recovery asks the controller.
        if (!programmer_.resume()) {
          programmer_.startRecovery(journal_.phase == JOURNAL_RESTARTING);
        }
        drive(port);
        settle(port);
        return true;
      }
      if (waiting_ && waitOver(nowMs)) {
        refuseWaiting(REFUSE_NO_CHARGER, nowMs);  // the duty comes first, and the charger did not let it be done
      }
      return false;
    }
    if (!waiting_) {
      return false;
    }
    if (!taken_) {
      if (!(epochNow >= CLOCK_SET_EPOCH)) {
        if (waitOver(nowMs)) {
          refuseWaiting(REFUSE_STALE, nowMs);
        }
        return false;
      }
      if (!requestIsFresh(waitingId_, epochNow)) {
        refuseWaiting(REFUSE_STALE, nowMs);
        return false;
      }
      const Journal next = {waitingId_, JOURNAL_TAKEN};
      if (!port.saveJournal(next)) {
        refuseWaiting(REFUSE_JOURNAL, nowMs);
        return false;
      }
      journal_ = next;
      journalUnsaved_ = false;
      taken_ = true;
    }
    if (!ready) {
      if (waitOver(nowMs)) {
        refuseWaiting(REFUSE_NO_CHARGER, nowMs);
      }
      return false;
    }
    waiting_ = false;
    taken_ = false;
    programmer_.start(waitingProfile_, waitingLoadAllowed_);
    drive(port);
    settle(port);
    return true;
  }

  // A result waits to be sent to the server, with the request it answers and the client's
  // millis() when it came about. The newest takes the place of one that was not sent yet.
  bool reportDue() const { return reportDue_; }
  const ProgramResult &report() const { return report_; }
  uint32_t reportRequestId() const { return reportRequestId_; }
  uint32_t reportMillis() const { return reportMs_; }
  void reportTaken() { reportDue_ = false; }

  // The client gives up the request that waits: a newer config no longer asks for it
  // (REFUSE_WITHDRAWN), or the client can no longer take it on (REFUSE_NOT_LIFEPO4,
  // REFUSE_NO_CHARGER). It is refused with `refusals` as it stands, its result goes to the
  // server, and the same id is not taken on again. A duty that is owed stays owed. Returns false,
  // and does nothing, when no request waits or `refusals` holds no bit.
  bool withdraw(uint32_t refusals, uint32_t nowMs) {
    if (!waiting_ || refusals == 0) {
      return false;
    }
    refuseWaiting(refusals, nowMs);
    return true;
  }

  const Journal &journal() const { return journal_; }
  // A request waits for the clock, the charger or a duty.
  bool requestWaiting() const { return waiting_; }
  // The id of the request that waits, 0 when none does.
  uint32_t waitingRequestId() const { return waiting_ ? waitingId_ : 0; }
  // Whether service() would talk to the controller now if it were ready: a duty whose turn has
  // come, or a request that can run (one in the journal, or a fresh one with the clock set). The
  // client gives such a pass to programming when every pass of its loop is due to poll. It cannot
  // tell that flash will refuse the journal, so it may say yes where service() then refuses the
  // request without a run. It never says no where service() would talk.
  bool wantsLine(double epochNow, uint32_t nowMs) const {
    if (dutyOwed()) {
      return dutyTurn(nowMs);
    }
    return waiting_ && (taken_ || requestIsFresh(waitingId_, epochNow));
  }
  // The controller may wait for a restart, and the client has yet to settle that.
  bool dutyOwed() const { return journal_.phase == JOURNAL_RESTARTING || journal_.phase == JOURNAL_WRITING; }

 private:
  Programmer programmer_;
  Journal journal_;
  bool journalUnsaved_;  // flash did not take the journal as it is here
  uint32_t journalTryMs_;
  bool waiting_;
  bool taken_;           // the waiting request is in the journal
  uint32_t waitingId_;
  Profile waitingProfile_;
  bool waitingLoadAllowed_;
  uint32_t waitingSinceMs_;
  uint32_t refusedId_;   // the last request that was refused without a run
  bool dutyForced_;      // try the duty at the next chance, whatever its timer says
  uint8_t dutyTries_;
  uint32_t dutyTryMs_;
  bool reportDue_;
  uint32_t reportRequestId_;
  uint32_t reportMs_;
  ProgramResult report_;
  uint16_t stepLimit_;

  bool waitOver(uint32_t nowMs) const { return (uint32_t)(nowMs - waitingSinceMs_) >= REQUEST_WAIT_MS; }

  // The duty that is owed may be tried now: at once after a request or a client's start, and then
  // DUTY_RETRY_MS apart, DUTY_SLOW_RETRY_MS after DUTY_FAST_TRIES tries.
  bool dutyTurn(uint32_t nowMs) const {
    const uint32_t gap = dutyTries_ >= DUTY_FAST_TRIES ? DUTY_SLOW_RETRY_MS : DUTY_RETRY_MS;
    return dutyForced_ || (uint32_t)(nowMs - dutyTryMs_) >= gap;
  }

  void post(uint32_t requestId, const ProgramResult &result, uint32_t nowMs) {
    report_ = result;
    reportRequestId_ = requestId;
    reportMs_ = nowMs;
    reportDue_ = true;
  }

  void saveJournal(Port &port, uint32_t nowMs) {
    journalTryMs_ = nowMs;
    journalUnsaved_ = !port.saveJournal(journal_);
  }

  // The waiting request is given up. If it was in the journal, the journal says so.
  void dropWaiting(uint32_t nowMs) {
    if (waiting_ && taken_ && journal_.phase == JOURNAL_TAKEN) {
      journal_.phase = JOURNAL_IDLE;
      journalUnsaved_ = true;
      journalTryMs_ = nowMs - JOURNAL_RETRY_MS;
    }
    waiting_ = false;
    taken_ = false;
  }

  void refuseWaiting(uint32_t reasons, uint32_t nowMs) {
    refusedId_ = waitingId_;
    ProgramResult result;
    refusedResult(waitingProfile_, 0, reasons, &result);
    post(waitingId_, result, nowMs);
    dropWaiting(nowMs);
  }

  // Runs what programmer_ was started on to its end, with the two commit points in the journal.
  void drive(Port &port) {
    uint16_t steps = 0;
    while (!programmer_.step(port, 1)) {
      if (++steps >= stepLimit_) {
        return;  // cannot happen: settle() deals with a run that has not ended
      }
      const uint8_t commit = programmer_.awaiting();
      if (commit == COMMIT_NONE) {
        port.rest(programmer_.pauseMs(), programmer_.missed());
        continue;
      }
      Journal next = journal_;
      next.phase = commit == COMMIT_RESTART ? (uint8_t)JOURNAL_RESTARTING : (uint8_t)JOURNAL_WRITING;
      if (port.saveJournal(next)) {
        journal_ = next;
        journalUnsaved_ = false;
        programmer_.go();
      } else {
        programmer_.refuse(REFUSE_JOURNAL);
      }
    }
  }

  // A run or a duty has come to its end: its result goes out, and the journal says what is owed.
  void settle(Port &port) {
    const uint32_t nowMs = port.millisNow();
    dutyForced_ = false;
    dutyTryMs_ = nowMs;
    ProgramResult result = programmer_.result();
    if (!programmer_.ended()) {
      // The step limit: the run is given up as it stands. What it may have left behind is a
      // duty, and a recovery settles it: resume() takes up no run that has not ended.
      result.outcome = OUTCOME_STOPPED;
      result.followUp = dutyOwed();
    }
    const bool owed = result.followUp;
    post(journal_.requestId, result, nowMs);
    if (owed) {
      if (dutyTries_ < 0xFF) {
        ++dutyTries_;
      }
      return;
    }
    dutyTries_ = 0;
    if (journal_.phase != JOURNAL_IDLE) {
      journal_.phase = JOURNAL_IDLE;
      saveJournal(port, nowMs);
    }
  }
};

// ---------------------------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------------------------

// One register of a run: "EV_lvd: 11.50 V -> 13.10 V, written".
inline void describeEntry(const PlanEntry &entry, uint8_t scale, Text *out) {
  out->add(item(entry.item).name).add(": ");
  describeValue(entry.item, entry.before, scale, SCALE_UNKNOWN, out);
  if (entry.status != ENTRY_UNCHANGED) {
    out->add(" -> ");
    describeValue(entry.item, entry.target, scale, SCALE_UNKNOWN, out);
  }
  out->add(", ").add(entryStatusName(entry.status));
  if (entry.status == ENTRY_ALTERED || entry.status == ENTRY_LOST) {
    out->add(": it holds ");
    describeValue(entry.item, entry.found, scale, SCALE_UNKNOWN, out);
  }
}

// A refusal with the figures behind it, where the result holds them.
inline void describeRefusal(uint32_t refusal, const ProgramResult &result, Text *out) {
  out->add(refusalText(refusal));
  if (refusal == REFUSE_BATTERY_LOW && (result.seen & SEEN_BATTERY) != 0) {
    out->add(" (battery ").addFixed(batteryVolts(result), 2).add(" V");
    if (result.liveScale == SCALE_UNKNOWN) {
      out->add(" at the lower of its two possible readings");
    }
    out->add(", new LVR ").addFixed((double)result.profile.lvrCv / 100.0, 2).add(" V)");
  } else if (refusal == REFUSE_LOAD_CURRENT && (result.seen & SEEN_LOAD_CURRENT) != 0) {
    out->add(" (").addFixed(ampsFromRaw(result.loadCurrentRaw), 2).add(" A)");
  } else if (refusal == REFUSE_FAULT && (result.seen & SEEN_FAULTS) != 0) {
    out->add(" (array faults 0x").addHex16(result.arrayFault).add(", load faults 0x").addHex16(result.loadFault);
    out->add(")");
  }
}

// One line that sums a run up: "programmed: 14 of 17 registers written, 3 already set".
inline void describeRun(const ProgramResult &result, Text *out) {
  out->add(outcomeName(result.outcome));
  if (result.planned == 0) {
    return;
  }
  out->add(": ").addUnsigned(countEntries(result, ENTRY_WRITTEN)).add(" of ").addUnsigned(result.planned);
  out->add(" registers written, ").addUnsigned(countEntries(result, ENTRY_UNCHANGED)).add(" already set");
}

inline void describeLoadState(uint8_t state, Text *out) {
  const char *name = loadStateName(state);
  if (name != nullptr) {
    out->add(name);
  } else {
    out->add("state ").addUnsigned(state);
  }
}

// What the LOAD output did during a run, which the specification leaves open:
// "LOAD output: LOAD_ON before the run, FAULT while the charger waited for its restart, LOAD_ON at the end."
// It lists the states the run read, and adds nothing when it read none.
inline void describeLoad(const ProgramResult &result, Text *out) {
  const uint8_t states[3] = {result.loadStateBefore, result.loadStateEdit, result.loadStateAfter};
  static const char *const kWhen[3] = {" before the run", " while the charger waited for its restart", " at the end"};
  bool any = false;
  for (uint8_t i = 0; i < 3; ++i) {
    if (states[i] == LOAD_STATE_NOT_READ) {
      continue;
    }
    out->add(any ? ", " : "LOAD output: ");
    describeLoadState(states[i], out);
    out->add(kWhen[i]);
    any = true;
  }
  if (any) {
    out->add(".");
  }
}

}  // namespace sunsaver

#endif  // TANKALARM_SUNSAVER_PROGRAM_H
