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
 *   3. Restarts the controller (coil 0x00FF) and asks until it is back. A controller that
 *      refuses the command, does not come back from it, or shows no sign of having restarted
 *      (it neither fell silent nor cleared a pending edit) is not written to.
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
 *     anything is written, on the controller as the run found it, and it has to show that it
 *     works: the controller falls silent and comes back, or clears an edit that was pending.
 *     And once a write request has been sent, the run goes on to the restart command whatever
 *     happens to the writes after it, and whatever its time limit says.
 *   Read-back. The controller does not check what it is given, and an acknowledgement can be
 *     lost on the way back from a controller that stored the value. So the acknowledgement
 *     decides nothing: what the register reads afterwards does.
 *   No repeats. A register that does not take its value is written once more, and then the run
 *     stops. A whole run is never repeated by itself.
 *   The order. Equalization goes off first. The limits on the charge voltage (the HVD and the
 *     regulation ceiling) that come down follow, then the charge voltages of both banks that come
 *     down, and the times. Temperature compensation goes off only then: on a warm battery it
 *     holds a charge voltage below its setting, so switched off before a setting came down it
 *     would let the battery charge to the old setting. The charge voltages that go up come after
 *     it, since on a cold battery it would add to them, and the limits that go up last of all,
 *     once every charge voltage holds its new value. The load thresholds come at the end, the
 *     load current compensation (which pulls the LVD in effect down under load) before the LVD.
 *     Within a pair that has to stay in order (LVD and LVR, say) the register that keeps the pair
 *     in order is written first. So wherever a run stops, at any battery temperature, each charge
 *     voltage in effect is no higher than the higher of the one the run found and the one it was
 *     to write, and the LVD in effect is no lower than the lower of the two.
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
  // The client's millis(): a run keeps to its time limit by it (RUN_LIMIT_MS).
  virtual uint32_t millisNow() = 0;
  // How long a request waits for its reply before it counts as unanswered. A run keeps the time
  // that its end may need by it.
  virtual uint16_t timeoutMs() const = 0;
  // The longest one call of readRegister(), writeRegister() or writeCoil() may hold the client,
  // whatever the controller sends back: with ArduinoModbus, modbusRequestLimitMs(), and more
  // while a read looks for its Modbus function (FunctionChoice::readLimitMs()). A run reads it at
  // its first step, and starts no request that could end past its time limit by it.
  virtual uint32_t requestLimitMs() const = 0;
};

// How long one request may hold the client with ArduinoModbus, which it talks through: the
// Modbus timeout for a reply to begin, and what libmodbus may add to it.
//   - A reply that comes whole but does not answer the request (another slave's, another
//     function's, or one whose length does not fit) costs one timeout more: with
//     MODBUS_ERROR_RECOVERY_PROTOCOL, which ModbusClient::begin() sets, libmodbus sleeps for a
//     whole response timeout before it flushes the line and gives up.
//   - A reply that stops part way costs up to MODBUS_PARTIAL_REPLY_MS more: after its first
//     bytes, libmodbus waits its byte timeout of 500 ms for the next part, the RS-485 stream its
//     read timeout of 1 s for the bytes that part lacks, and libmodbus 500 ms more before it
//     gives up.
// And the frames themselves, with the silence of 3.5 characters that the client's port keeps
// before each request of a run, a few tens of milliseconds at 9600 baud, the slowest a config
// allows. Bytes that came one at a time, each just inside those waits, could hold a request
// longer; a controller sends its reply at the speed of the line.
static const uint16_t MODBUS_PARTIAL_REPLY_MS = 2000;
static const uint16_t MODBUS_FRAMES_MS = 100;

inline uint32_t modbusRequestLimitMs(uint16_t timeoutMs) {
  const uint32_t timeout = timeoutMs;
  const uint32_t more = timeout > MODBUS_PARTIAL_REPLY_MS ? timeout : (uint32_t)MODBUS_PARTIAL_REPLY_MS;
  return timeout + more + MODBUS_FRAMES_MS;
}

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
  ENTRY_LOST,         // it held the word after the write, and no longer does after the restart
  ENTRY_UNREAD        // it held the word, and could not be read again after the restart
};

static const uint8_t ENTRY_STATUS_LAST = ENTRY_UNREAD;

// A register whose word the last reading of a run is to confirm.
inline bool awaitsReading(uint8_t status) {
  return status == ENTRY_WRITTEN || status == ENTRY_UNCHANGED || status == ENTRY_UNREAD;
}

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
    case ENTRY_UNREAD: return "not read after the restart";
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
// somebody stored below the new absorption voltage cannot hold the charger under it. The steps
// are in the order of writing of a run in which nothing goes up; the phases decide (planPhase()).
inline const PlanStep &planStep(uint8_t index) {
  static const PlanStep kSteps[PLAN_STEPS] = {
      {I_EV_EQ, PLAN_SINGLE},       {I_EV_EQ2, PLAN_SINGLE},
      {I_EV_HVR, I_EV_HVD},         {I_EVB_REF_LIM, PLAN_SINGLE},
      {I_EV_FLOAT, I_EV_REG},       {I_ET_FLOAT, PLAN_SINGLE},  {I_ET_FLOATLB, PLAN_SINGLE},
      {I_EV_FLOAT2, I_EV_REG2},     {I_ET_FLOAT2, PLAN_SINGLE}, {I_ET_FLOATLB2, PLAN_SINGLE},
      {I_EV_TEMPCOMP, PLAN_SINGLE}, {I_ER_ICOMP, PLAN_SINGLE},  {I_EV_LVD, I_EV_LVR}};
  return kSteps[index < PLAN_STEPS ? index : 0];
}

// The phases of a plan, in the order of writing. The charge voltage in effect is the lowest of
// the setting plus the temperature compensation, the HVD plus the same, and the ceiling, which
// is not compensated. The compensation is above 0 on a battery colder than 25 C and below 0 on
// a warmer one, and every run switches it off. So before it goes off only what comes down is
// written, and after it what goes up: each charge voltage in effect stays no higher than the one
// the run found until compensation goes off, and no higher than the one it is to leave from then
// on, at any temperature.
enum PlanPhase : uint8_t {
  PHASE_EQUALIZE = 0,  // equalization off, which takes a stage away
  PHASE_LIMITS_DOWN,   // the HVD (with its HVR) and the ceiling, where they come down or stay
  PHASE_BANKS,         // the charge voltages of both banks that come down or stay, and the times
  PHASE_TEMPCOMP,      // temperature compensation off
  PHASE_VOLTS_UP,      // the charge voltages that go up
  PHASE_LIMITS_UP,     // the HVD and the ceiling, where they go up
  PHASE_LOAD,          // the LVD's load current compensation, then the LVD and LVR
  PLAN_PHASES
};

// A limit on the charge voltage as a level to compare: 0 is no limit (the HVD off, no ceiling),
// above every word.
inline uint32_t limitLevel(uint16_t word) { return word == 0 ? 0x10000ul : (uint32_t)word; }

// The phase of a register of the plan. The HVD decides for its HVR, so the two stay a pair.
inline uint8_t planPhase(uint8_t index, const Snapshot &before, const Profile &profile, uint8_t scale) {
  switch (index) {
    case I_EV_EQ:
    case I_EV_EQ2:
      return PHASE_EQUALIZE;
    case I_EV_HVR:
    case I_EV_HVD:
    case I_EVB_REF_LIM: {
      const uint8_t limit = index == I_EV_HVR ? (uint8_t)I_EV_HVD : index;
      return limitLevel(targetWord(limit, profile, scale)) > limitLevel(before.raw[limit]) ? (uint8_t)PHASE_LIMITS_UP
                                                                                          : (uint8_t)PHASE_LIMITS_DOWN;
    }
    case I_EV_REG:
    case I_EV_FLOAT:
    case I_EV_REG2:
    case I_EV_FLOAT2:
      return targetWord(index, profile, scale) > before.raw[index] ? (uint8_t)PHASE_VOLTS_UP : (uint8_t)PHASE_BANKS;
    case I_EV_TEMPCOMP:
      return PHASE_TEMPCOMP;
    case I_ER_ICOMP:
    case I_EV_LVD:
    case I_EV_LVR:
      return PHASE_LOAD;
    default:
      return PHASE_BANKS;  // the times
  }
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

// Adds one register to the plan.
inline void addPlanEntry(uint8_t index, const Snapshot &before, const Profile &profile, uint8_t scale,
                         PlanEntry *entries, uint8_t *count, uint8_t *toWrite) {
  PlanEntry &entry = entries[(*count)++];
  entry.item = index;
  entry.before = before.raw[index];
  entry.target = targetWord(index, profile, scale);
  entry.found = entry.before;
  entry.status = entry.before == entry.target ? (uint8_t)ENTRY_UNCHANGED : (uint8_t)ENTRY_PENDING;
  if (entry.status == ENTRY_PENDING) {
    ++*toWrite;
  }
}

// Fills entries[PLAN_ENTRIES] in the order of writing and returns how many of them have to be
// written. `before` must hold every register of the plan.
//
// Phase by phase, and within a phase in the order of the steps. When both registers of a pair
// are in one phase, the upper one goes first when its new word is above the word the lower one
// holds now: the pair is then in order after either write. Otherwise the lower one goes first.
// Its new word is below the new upper one, which is then no higher than the old lower one, which
// was below the old upper one. When a pair's charge voltages are in two phases, the one that
// comes down goes first, and the pair is in order after either write as well.
inline uint8_t buildPlan(const Snapshot &before, const Profile &profile, uint8_t scale, PlanEntry *entries) {
  uint8_t count = 0;
  uint8_t toWrite = 0;
  for (uint8_t phase = 0; phase < PLAN_PHASES; ++phase) {
    for (uint8_t s = 0; s < PLAN_STEPS; ++s) {
      const PlanStep &step = planStep(s);
      const bool first = planPhase(step.first, before, profile, scale) == phase;
      const bool second = step.second != PLAN_SINGLE && planPhase(step.second, before, profile, scale) == phase;
      if (first && second && targetWord(step.second, profile, scale) > before.raw[step.first]) {
        addPlanEntry(step.second, before, profile, scale, entries, &count, &toWrite);
        addPlanEntry(step.first, before, profile, scale, entries, &count, &toWrite);
        continue;
      }
      if (first) {
        addPlanEntry(step.first, before, profile, scale, entries, &count, &toWrite);
      }
      if (second) {
        addPlanEntry(step.second, before, profile, scale, entries, &count, &toWrite);
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
  OUTCOME_INTERRUPTED,     // the client restarted during a run (see Programmer::startRecovery)
  OUTCOME_UNVERIFIED       // written and restarted, but registers could not be read again to confirm it
};

static const uint8_t OUTCOME_LAST = OUTCOME_UNVERIFIED;

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
    case OUTCOME_UNVERIFIED: return "unverified";
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
    case OUTCOME_UNVERIFIED:
      return "Written and restarted, but not every register could be read again to confirm it. The next "
             "charger check shows what the charger holds.";
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
  REFUSE_NOT_12V = 1ul << 2,         // the battery voltage does not show a 12 V system (systemVolts())
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
  REFUSE_WITHDRAWN = 1ul << 13,      // a newer config no longer asked for it while it waited, or named another controller
  REFUSE_CONFIG_UNSAVED = 1ul << 14,  // the client could not save the config that carried it in its flash
  REFUSE_HISTORY_LOST = 1ul << 15     // asked for before a start that lost the record of the requests answered (cautiousJournal())
};

static const uint8_t REFUSAL_BITS = 16;

inline const char *refusalText(uint32_t refusal) {
  switch (refusal) {
    case REFUSE_PROFILE: return "The values asked for are outside the limits this firmware allows.";
    case REFUSE_SCALE_UNPROVEN:
      return "How this charger encodes its stored voltages could not be proven from its setpoints, so no "
             "voltage can be written safely.";
    case REFUSE_NOT_12V: return "The battery voltage does not show a 12 V system. Only 12 V systems are programmed.";
    case REFUSE_NOT_READ: return "A register that programming needs did not answer.";
    case REFUSE_FAULT: return "The charger reports a fault. Clear it first.";
    case REFUSE_BATTERY_LOW:
      return "The battery is below the new LVR, so the load could stay switched off after the charger "
             "restarts. Try again with a charged battery.";
    case REFUSE_LOAD_CURRENT:
      return "Current is flowing from the charger's LOAD terminals. If this Opta is powered from them it "
             "could lose power part way. Confirm in the request that it is not.";
    case REFUSE_NO_RESTART:
      return "The restart command, which every change of settings has to end with, could not be shown to "
             "work on this charger. Nothing was written.";
    case REFUSE_NOT_BUILT: return "This client's firmware was built without charger programming.";
    case REFUSE_STALE:
      return "The request was more than a day old when the client could act on it, or the client's clock "
             "could not tell.";
    case REFUSE_NOT_LIFEPO4: return "The battery configured for this client is not a 12 V LiFePO4 battery.";
    case REFUSE_NO_CHARGER:
      return "The charger is not enabled on RS-485 for this client, or it did not answer within an hour.";
    case REFUSE_JOURNAL: return "The client could not record the run in its flash memory, so it did not go on.";
    case REFUSE_WITHDRAWN:
      return "A newer config for this client no longer asked for these values while the request waited, or "
             "named another charger, so they were not written.";
    case REFUSE_CONFIG_UNSAVED:
      return "The client could not save the config that asked for these values in its flash memory, so they "
             "were not written: after a restart it would go by the config it had before.";
    case REFUSE_HISTORY_LOST:
      return "The client could not read its record of the requests it had carried out when it last started, "
             "and this request was asked for before that start, or too soon after it to tell, so it may have "
             "been carried out already. Check the charger's settings, and ask again if they still need to "
             "change.";
    default: return "A reason this firmware has no text for.";
  }
}

// How the last restart command of a run went. A restart counts as done only with something to
// show for it: the controller reported a pending edit before the command and no longer does, or
// it fell silent after the command and answers again. A controller that answers all along and
// had no pending edit may have ignored the command (RESTART_UNPROVEN).
enum RestartStatus : uint8_t {
  RESTART_NOT_NEEDED = 0,  // no restart command was sent
  RESTART_DONE,            // the controller restarted, and reports no pending edit
  RESTART_REFUSED,         // it refused the command
  RESTART_NOT_DONE,        // it answers, and still reports the pending edit after two commands (or one, at the time limit)
  RESTART_NO_ANSWER,       // it has not answered since the command
  RESTART_UNCONFIRMED,     // it answers, but its fault registers could not be read
  RESTART_UNPROVEN         // it answered all along and had no pending edit: nothing shows that it restarted
};

static const uint8_t RESTART_STATUS_LAST = RESTART_UNPROVEN;

inline const char *restartText(uint8_t restart) {
  switch (restart) {
    case RESTART_NOT_NEEDED: return "No restart command was sent to the charger.";
    case RESTART_DONE: return "The charger restarted.";
    case RESTART_REFUSED: return "The charger refused the restart command.";
    case RESTART_NOT_DONE:
      return "The charger took the restart command and still reports that it waits for a restart.";
    case RESTART_NO_ANSWER: return "The charger has not answered since the restart command.";
    case RESTART_UNCONFIRMED:
      return "The charger answers again, but its fault registers could not be read, so its restart is not "
             "confirmed.";
    case RESTART_UNPROVEN:
      return "The charger kept answering after the restart command and had no pending edit to clear, so "
             "nothing shows that it restarted.";
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
  NOTE_VERIFY_INCOMPLETE = 1u << 5,    // registers could not be read again after the restart
  NOTE_NOTHING_WRITTEN = 1u << 6,      // an interrupted run that had not begun to write
  NOTE_AT_FIRST_RESTART = 1u << 7,     // the client restarted while the charger restarted for the first time
  NOTE_TIME_LIMIT = 1u << 8,           // the run reached its time limit and was cut short
  NOTE_NOT_ANSWERED = 1u << 9,         // a look after a run that was cut short could not read the controller
  NOTE_NO_LINK = 1u << 10              // a look could not ask the controller: the client's link did not start
};

static const uint8_t NOTE_BITS = 11;

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
      return "Not every register could be read again after the restart. The next charger check shows what the "
             "charger holds.";
    case NOTE_NOTHING_WRITTEN: return "Nothing had been written.";
    case NOTE_AT_FIRST_RESTART:
      return "The client restarted at the moment the charger restarted. If this Opta is powered from the "
             "charger's LOAD terminals it cannot program the charger: power it from the battery.";
    case NOTE_TIME_LIMIT:
      return "The charger answered so slowly that the run reached its time limit and was cut short. Past it "
             "nothing more is written. A charger that was written to is still sent its restart command, "
             "unless a reply so slow left no time even for that: then the look that follows sends it if the "
             "charger still waits for it.";
    case NOTE_NOT_ANSWERED:
      return "The charger did not answer when the client looked at it after its own restart, so it is not "
             "known whether the charger waits for a restart.";
    case NOTE_NO_LINK:
      return "The client could not start its RS-485 link, so it could not ask the charger whether it waits "
             "for a restart. The client tries again later.";
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

static const uint16_t RESTART_WAIT_MS = 500;   // the line is left alone for this long after a restart command
static const uint16_t PROBE_GAP_MS = 500;      // and between two requests that look for the controller after it
static const uint8_t BACK_PROBES = 20;         // requests it may leave unanswered before the run gives up on it
static const uint8_t QUIET_PROBES = 10;        // requests it answers without a pause before the run takes it that
                                               // it did not restart
static const uint8_t WRITE_ATTEMPTS = 2;       // writes of one register
static const uint8_t RESTART_ATTEMPTS = 2;     // restart commands that the controller took without restarting
static const uint8_t READ_ATTEMPTS = 2;        // requests for a register that stays silent
static const uint8_t VERIFY_MISSES = 3;        // registers in a row without a value end the last reading
static const uint32_t RUN_LIMIT_MS = 60000;    // no run takes longer than this (see Programmer::step())
static const uint8_t RUN_END_TIMEOUTS = 7;     // Modbus timeouts that the shortest safe end of a run may take
static const uint16_t RUN_END_SLACK_MS = 2000; // and the requests that are answered on its way

// The longest the caller leaves the line alone between two requests of a run: a Modbus timeout
// after a request without a reply, and the pauses the run asks for.
inline uint32_t runRestMs(uint16_t timeoutMs) {
  uint32_t rest = timeoutMs;
  if (rest < RESTART_WAIT_MS) {
    rest = RESTART_WAIT_MS;
  }
  return rest < PROBE_GAP_MS ? (uint32_t)PROBE_GAP_MS : rest;
}

// The time after its first step at which a run turns to its end (see Programmer). That is when
// only the time is left that the shortest safe end needs while its requests are answered or time
// out: the request under way may take a timeout and the rest after it, and the end that follows
// at most five timeouts more (a write's read-back and the restart command, each with its rest,
// and one for the requests that look for the controller after that command). And it is no later
// than the last moment at which a request of the writes still leaves the last restart command
// room, however long the bus takes for each (requestLimitMs).
inline uint32_t runCutMs(uint16_t timeoutMs, uint32_t requestLimitMs) {
  const uint32_t end = (uint32_t)RUN_END_TIMEOUTS * timeoutMs + RUN_END_SLACK_MS;
  const uint32_t room = 2 * requestLimitMs + runRestMs(timeoutMs);
  const uint32_t reserve = end > room ? end : room;
  return reserve < RUN_LIMIT_MS ? RUN_LIMIT_MS - reserve : 0;
}

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
// It asks for the pauses it needs, and counts requests where it has to give up. It reads the
// bus's clock for one thing only, its time limit: no run takes longer than RUN_LIMIT_MS, a
// minute. Once only the time is left that the shortest safe end may need (runCutMs()), the run
// takes that end: nothing more is written, a register gets one request where it would get two,
// nothing is read that the end does not need, and the run ends unless it has written. Then the
// controller still gets one restart command, and when the run cannot see it restart, the client
// owes it a look (controllerUnknown()). A run on a line that answers takes about 10 seconds.
//
// A request can take longer than the Modbus timeout (Bus::requestLimitMs()). So no request
// starts that could end past the limit, and while a run that writes still owes its last restart
// command, no other request starts that could leave that command no room: from there a register
// just written is not read back, and the run goes on to the command. A request that takes longer
// still than the bus says one may (a line that sends a byte at a time) holds a run past the limit
// by that much, once: the run then ends at once, and leaves a restart command it has no time for
// to a later look.
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
  // COMMIT_RESTART and COMMIT_WRITE). The outcome is OUTCOME_INTERRUPTED, which speaks for the
  // run that was cut short. It holds NOTE_NOT_ANSWERED, and a follow-up, when the controller's
  // fault registers could not be read: that run may have written, so this is no OUTCOME_NO_LINK.
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
  // registers once more and ends with the outcome the run would have had. Its time limit starts
  // again. A look that cannot read the controller's fault words settles nothing: the result
  // stays as the run left it, a restart command the time limit left unsent is still owed, and so
  // is the look. Returns false, and does nothing, when the last run is not such a run.
  bool resume() {
    if (recovery_ || !controllerUnknown()) {
      return false;
    }
    missed_ = false;
    pauseMs_ = 0;
    stage_ = 0;
    readTries_ = 0;
    timed_ = false;
    overdue_ = false;
    phase_ = PHASE_RECOVER;
    return true;
  }

  // Makes up to `budget` requests. Returns true once no run is under way any more.
  bool step(Bus &bus, uint8_t budget) {
    missed_ = false;
    pauseMs_ = 0;
    while (budget > 0 && active() && awaiting() == COMMIT_NONE && !missed_ && pauseMs_ == 0) {
      const uint32_t now = bus.millisNow();
      if (!timed_) {
        timed_ = true;
        startMs_ = now;
        requestMs_ = bus.requestLimitMs();
        if (requestMs_ < bus.timeoutMs()) {
          requestMs_ = bus.timeoutMs();
        }
        restMs_ = runRestMs(bus.timeoutMs());
        cutMs_ = runCutMs(bus.timeoutMs(), requestMs_);
      }
      const uint32_t elapsed = (uint32_t)(now - startMs_);
      if (!overdue_ && elapsed >= cutMs_) {
        overdue_ = true;
        result_.notes |= NOTE_TIME_LIMIT;
      }
      if (!roomForRequest(elapsed)) {
        outOfTime();  // the phase comes to its end without the request it would make
        continue;
      }
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
    // When no request could end within the time limit after the rest that is to come, the rest
    // would only hold the client longer: the run ends here, without the requests its phases would
    // make. While a run that wrote still owes its last restart command, that happens only after a
    // request took longer than the bus said one may: the writes leave that command room otherwise.
    // The command is then left to a later look (restartCut_).
    if (timed_ && active() && awaiting() == COMMIT_NONE) {
      uint32_t rest = pauseMs_;
      if (missed_ && rest < bus.timeoutMs()) {
        rest = bus.timeoutMs();
      }
      const uint32_t elapsed = (uint32_t)(bus.millisNow() - startMs_);
      if (elapsed > RUN_LIMIT_MS || rest + requestMs_ > RUN_LIMIT_MS - elapsed) {
        for (uint8_t guard = 0; guard < 8 && active(); ++guard) {
          outOfTime();
        }
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
      beginRestart(true, pendingAtStart_);
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
    PHASE_WAIT_BACK,      // asking until the controller is back, then its fault words
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
  bool firstRestart_;    // the restart command under way is the one before the writes
  bool stopped_;         // a register did not take its word, or the time limit stopped the writes
  bool readingBack_;     // the register at entry_ has been written and is to be read
  bool pendingAtStart_;  // the controller reported a pending edit when the run read it
  bool editSeen_;        // and after the writes
  bool sawPending_;      // it reported a pending edit when the restart command under way went out
  bool silent_;          // it has missed a request since that command: it went away to restart
  bool timed_;           // startMs_, cutMs_, requestMs_ and restMs_ are set: the run has made its first step
  bool overdue_;         // the run has reached cutMs_, and takes the shortest safe way to its end
  bool restartCut_;      // the time limit left a second restart command unsent
  IoStatus lastWrite_;
  uint8_t entry_;
  uint8_t stage_;
  uint8_t writeAttempts_;
  uint8_t restartAttempts_;
  uint8_t readTries_;
  uint8_t verifyMisses_;
  uint8_t quiet_;        // requests the controller answered since the restart command, before it went away
  uint8_t toWrite_;
  uint16_t pauseMs_;
  uint16_t heldFault_;   // array_fault, kept until load_fault has been read too
  uint32_t startMs_;
  uint32_t cutMs_;       // from startMs_ to the moment the run turns to its end
  uint32_t requestMs_;   // the longest one request may take (Bus::requestLimitMs())
  uint32_t restMs_;      // and the longest rest between two (runRestMs())

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
    pendingAtStart_ = false;
    editSeen_ = false;
    sawPending_ = false;
    silent_ = false;
    timed_ = false;
    overdue_ = false;
    restartCut_ = false;
    lastWrite_ = SSIO_OK;
    entry_ = 0;
    stage_ = 0;
    writeAttempts_ = 0;
    restartAttempts_ = 0;
    readTries_ = 0;
    verifyMisses_ = 0;
    quiet_ = 0;
    toWrite_ = 0;
    pauseMs_ = 0;
    heldFault_ = 0;
    startMs_ = 0;
    cutMs_ = 0;
    requestMs_ = 0;
    restMs_ = 0;
  }

  void finish(uint8_t outcome) {
    result_.outcome = outcome;
    // A restart command went out and nothing shows that it worked, or the time limit left its
    // second try unsent. That matters when this run wrote, and for a recovery, which exists to
    // settle exactly this.
    const bool restartOpen =
        result_.restart == RESTART_NO_ANSWER || result_.restart == RESTART_UNCONFIRMED || restartCut_;
    result_.followUp =
        recovery_ ? (restartOpen || (result_.notes & NOTE_NOT_ANSWERED) != 0) : (restartOpen && result_.wroteAny);
    phase_ = PHASE_ENDED;
  }

  // Whether a request may start `elapsed` after the run's first step: it has to end within the
  // time limit however long the bus takes for it. A request of the writes, or of the look at the
  // controller that follows them, has to leave room for the last restart command as well, and
  // for the rest between the two.
  bool roomForRequest(uint32_t elapsed) const {
    if (elapsed > RUN_LIMIT_MS) {
      return false;
    }
    uint32_t need = requestMs_;
    if (phase_ == PHASE_WRITE || phase_ == PHASE_OBSERVE) {
      need += restMs_ + requestMs_;
    }
    return need <= RUN_LIMIT_MS - elapsed;
  }

  // The request this phase would make does not fit in the time that is left (roomForRequest()):
  // the phase comes to its end without it, as it does past the cut when a request goes
  // unanswered. That leads to the end of the run, or to the last restart command, which the
  // writes left room for.
  void outOfTime() {
    if (!overdue_) {
      overdue_ = true;
      result_.notes |= NOTE_TIME_LIMIT;
    }
    switch (phase_) {
      case PHASE_READ_BEFORE:
        finish(OUTCOME_NO_LINK);  // nothing has been sent that changes the controller
        break;
      case PHASE_RECOVER:
        if (recovery_) {
          notAnswered();
        } else {
          conclude();  // it still does not know
        }
        break;
      case PHASE_RESTART:
        if (firstRestart_) {
          finish(OUTCOME_NO_LINK);  // nothing is written yet
        } else {
          restartCut_ = true;  // a later look sends it
          conclude();
        }
        break;
      case PHASE_WAIT_BACK:
        restartSettled(stage_ == 0 && (silent_ || quiet_ == 0) ? (uint8_t)RESTART_NO_ANSWER
                                                               : (uint8_t)RESTART_UNCONFIRMED);
        break;
      case PHASE_WRITE:
        if (readingBack_) {
          readingBack_ = false;
          result_.entries[entry_].status = ENTRY_NO_ANSWER;  // written, and not read back
          stopWrites();
        } else {
          stopped_ = true;
          if (result_.wroteAny) {
            beginRestart(false, false);
          } else {
            finish(OUTCOME_NO_LINK);
          }
        }
        break;
      case PHASE_OBSERVE:
        beginRestart(false, editSeen_);
        break;
      default:  // PHASE_VERIFY
        leaveUnread();
        conclude();
        break;
    }
  }

  static bool editPending(uint16_t arrayFault, uint16_t loadFault) {
    return (arrayFault & ARRAY_FAULT_EEPROM_EDIT) != 0 || (loadFault & LOAD_FAULT_EEPROM_EDIT) != 0;
  }

  // One request for a register. FETCH_VALUE with *raw set, FETCH_AGAIN when it was silent and is
  // to be asked once more (the step ends there), FETCH_NONE when it gives no value: the
  // controller refused it, or it was silent READ_ATTEMPTS times, or once past the time limit.
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
      if (++readTries_ < READ_ATTEMPTS && !overdue_) {
        return FETCH_AGAIN;
      }
    }
    readTries_ = 0;
    return FETCH_NONE;
  }

  bool stepReadBefore(Bus &bus) {
    if (overdue_) {
      finish(OUTCOME_NO_LINK);  // nothing has been sent that changes the controller
      return false;
    }
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
        pendingAtStart_ = true;
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
    if (toWrite_ == 0 && !pendingAtStart_) {
      finish(OUTCOME_UNCHANGED);
      return;
    }
    phase_ = PHASE_AWAIT_RESTART;
  }

  // A recovery, or a run taken up again: is the controller waiting for a restart?
  bool stepRecover(Bus &bus) {
    if (overdue_) {
      if (recovery_) {
        notAnswered();
      } else {
        conclude();  // it still does not know
      }
      return false;
    }
    uint16_t raw = 0;
    const Fetch got = fetch(bus, item(stage_ == 0 ? I_ARRAY_FAULT : I_LOAD_FAULT).addr, &raw);
    if (got == FETCH_AGAIN) {
      return true;
    }
    if (got == FETCH_NONE) {
      if (recovery_) {
        notAnswered();
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
    if (!recovery_) {
      restartCut_ = false;  // the look settles it: the command goes out now, or is not needed
    }
    if (pending) {
      beginRestart(false, true);
    } else {
      if (!recovery_) {
        result_.restart = RESTART_DONE;  // it answers, and no longer waits
      }
      beginVerify();
    }
    return true;
  }

  // A recovery that could not read the controller: it is owed again. The run it is about may
  // have written, so its result does not say that nothing was.
  void notAnswered() {
    result_.notes |= NOTE_NOT_ANSWERED;
    finish(OUTCOME_INTERRUPTED);
  }

  // A restart command is to go out. `first`: the one before the writes. `pending`: the
  // controller reported a pending edit, so its clearing shows that the command did its work.
  void beginRestart(bool first, bool pending) {
    firstRestart_ = first;
    sawPending_ = pending;
    restartAttempts_ = 0;
    phase_ = PHASE_RESTART;
  }

  // The restart command goes out even past the time limit when the run has written: then it is
  // the one request the controller must get. The one before the writes does not.
  bool stepRestart(Bus &bus) {
    if (overdue_ && firstRestart_) {
      finish(OUTCOME_NO_LINK);  // nothing has been sent that changes the controller
      return false;
    }
    ++restartAttempts_;
    result_.probes = 0;
    silent_ = false;
    quiet_ = 0;
    const IoStatus status = bus.writeCoil(COIL_RESTART);
    if (status == SSIO_REFUSED) {
      restartSettled(RESTART_REFUSED);
      return true;
    }
    if (status != SSIO_OK) {
      missed_ = true;   // a controller may restart before it acknowledges
      silent_ = true;
    }
    pauseMs_ = RESTART_WAIT_MS;
    stage_ = 0;
    readTries_ = 0;
    phase_ = PHASE_WAIT_BACK;
    return true;
  }

  // Asks after the command until the controller is back: a controller that restarts misses the
  // requests that come while it does, as many as it takes. One that keeps answering is asked a
  // few seconds longer, as a restart may not begin at once. Then its fault words decide.
  bool stepWaitBack(Bus &bus) {
    if (stage_ == 0) {
      uint16_t word = 0;
      const IoStatus status = bus.readRegister(item(I_ARRAY_FAULT).addr, &word);
      if (status == SSIO_REFUSED) {
        restartSettled(RESTART_UNCONFIRMED);
      } else if (status != SSIO_OK) {
        missed_ = true;
        silent_ = true;
        ++result_.probes;
        if (result_.probes >= BACK_PROBES || overdue_) {
          restartSettled(RESTART_NO_ANSWER);
        } else {
          pauseMs_ = PROBE_GAP_MS;
        }
      } else if (!silent_ && ++quiet_ < QUIET_PROBES && !overdue_) {
        pauseMs_ = PROBE_GAP_MS;  // it answers: its restart may be yet to come
      } else {
        heldFault_ = word;
        stage_ = 1;
        readTries_ = 0;
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
    } else if (editPending(heldFault_, raw)) {
      if (restartAttempts_ < RESTART_ATTEMPTS && !overdue_) {
        sawPending_ = true;
        phase_ = PHASE_RESTART;  // it answers and still waits: the command did not do its work
      } else {
        restartCut_ = restartAttempts_ < RESTART_ATTEMPTS;  // a later look may send the second
        restartSettled(RESTART_NOT_DONE);
      }
    } else {
      restartSettled(sawPending_ || silent_ ? RESTART_DONE : RESTART_UNPROVEN);
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
      if (toWrite_ > 0) {
        phase_ = PHASE_AWAIT_WRITE;
      } else {
        beginVerify();  // it held the settings already, and only had a restart to make up for
      }
    } else if (status == RESTART_REFUSED || status == RESTART_NOT_DONE || status == RESTART_UNPROVEN) {
      result_.refusals |= REFUSE_NO_RESTART;
      finish(OUTCOME_REFUSED);
    } else {
      finish(OUTCOME_NO_LINK);
    }
  }

  // The register at entry_: written, then read back. What it reads decides, not the
  // acknowledgement. Past the time limit no register is written any more: a run that has
  // written goes on to the restart without looking at the controller first, one that has not
  // ends.
  bool stepWrite(Bus &bus) {
    if (overdue_ && !readingBack_) {
      stopped_ = true;
      if (result_.wroteAny) {
        beginRestart(false, false);
      } else {
        finish(OUTCOME_NO_LINK);
      }
      return false;
    }
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
    editSeen_ = false;
    phase_ = PHASE_OBSERVE;
  }

  // What the controller does while it waits for its restart: the load state, and whether it
  // reports the pending edit as the specification says. Past the time limit the restart comes
  // at once.
  bool stepObserve(Bus &bus) {
    if (overdue_) {
      beginRestart(false, editSeen_);
      return false;
    }
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
      } else if (editPending(heldFault_, raw)) {
        editSeen_ = true;
      } else {
        result_.notes |= NOTE_EDIT_FAULT_NOT_SEEN;
      }
    } else if (stage_ == 1) {
      stage_ = 2;  // without the array faults the load faults cannot settle the question
    }
    if (++stage_ >= 3) {
      beginRestart(false, editSeen_);
    }
    return true;
  }

  // The registers are read again from here, so what a run before this one could not read again,
  // and the load state it saw, give way to what this reading finds (a run taken up again).
  void beginVerify() {
    entry_ = 0;
    readTries_ = 0;
    verifyMisses_ = 0;
    result_.notes = (uint16_t)(result_.notes & ~(uint16_t)NOTE_VERIFY_INCOMPLETE);
    result_.loadStateAfter = LOAD_STATE_NOT_READ;
    phase_ = PHASE_VERIFY;
  }

  // The registers from entry_ on that were not read again keep no confirmed word.
  void leaveUnread() {
    for (uint8_t i = entry_; i < result_.planned && i < PLAN_ENTRIES; ++i) {
      if (awaitsReading(result_.entries[i].status)) {
        result_.entries[i].status = ENTRY_UNREAD;
        result_.notes |= NOTE_VERIFY_INCOMPLETE;
      }
    }
  }

  // The registers once more, as the controller holds them now: a word that was only in its
  // working memory would be gone after the restart. Then the load state.
  bool stepVerify(Bus &bus) {
    if (overdue_) {
      leaveUnread();
      conclude();
      return false;
    }
    while (entry_ < result_.planned && !awaitsReading(result_.entries[entry_].status)) {
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
        entry.status = ENTRY_UNREAD;
        result_.notes |= NOTE_VERIFY_INCOMPLETE;
        if (++verifyMisses_ >= VERIFY_MISSES) {
          leaveUnread();
          conclude();  // the controller has stopped answering: the rest would only take time
        }
        return true;
      }
      verifyMisses_ = 0;
      entry.found = raw;
      if (raw != entry.target) {
        entry.status = ENTRY_LOST;
      } else {
        entry.status = entry.before == entry.target ? (uint8_t)ENTRY_UNCHANGED : (uint8_t)ENTRY_WRITTEN;
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
    } else if (result_.restart != RESTART_DONE || restartCut_) {
      finish(OUTCOME_RESTART_FAILED);
    } else if (countEntries(result_, ENTRY_LOST) != 0) {
      finish(OUTCOME_VERIFY_FAILED);
    } else if (countEntries(result_, ENTRY_UNREAD) != 0) {
      finish(OUTCOME_UNVERIFIED);
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
// moment. The Supervisor decides what is done about a request and when. It keeps a journal in
// the client's flash, so that a restart of the client loses nothing that the controller or a
// person waits for:
//
//   - the newest request the client has answered or taken on: none that is not newer is acted on;
//   - a request that waits: if the client restarts first, it is answered as interrupted;
//   - how far the last run came that reached the controller, and which controller that was:
//       JOURNAL_IDLE         nothing is owed to the controller
//       JOURNAL_RESTARTING   the first restart command may have gone out; nothing has been written
//       JOURNAL_WRITING      registers may have been written: the controller may wait for a restart
//   - the result that has yet to go to the server, and apart from it the look that answers no
//     request (request 0, after a start that could not tell from its flash what was owed): the
//     server keeps the two apart as well (programResultReplaces());
//   - the time the last result of a request went out with, and the last look: a later result of
//     the same request, or a later look, goes out with a later time, whatever the client's clock
//     did in between (sendEpoch()). That time is in flash before the result goes out
//     (reportSending());
//   - which journal kept aside at an earlier start it has taken in, if any (the client's
//     takeKeptChargerJournal()), so that a later start does not take that one in again.
//
// Each of these is in flash before the client goes on: a request before the config that carries
// it is acknowledged, a commit point before the run passes it, a result before the journal
// stops showing what it answers.
//
// A client that starts with RESTARTING or WRITING in its journal owes the controller a look, and
// the restart command if it waits (Programmer::startRecovery). So does a client whose run ended
// without learning whether the controller restarted (Programmer::resume). That duty comes before
// any new request. It is tried as soon as the charger answers, then every 10 minutes, and once a
// day after three tries, and always on the controller that its run talked to.
//
// A run holds the client for as long as it takes: about 10 seconds on a line that answers, and
// never longer than RUN_LIMIT_MS, a minute, whatever the Modbus timeout (see Programmer). Nothing
// else talks to the controller meanwhile, and the time between the first write and the restart
// that ends the run is as short as the line allows.

enum JournalPhase : uint8_t { JOURNAL_IDLE = 0, JOURNAL_RESTARTING, JOURNAL_WRITING };

static const uint8_t JOURNAL_PHASE_LAST = JOURNAL_WRITING;

// Journal::historyFrom of a record of the requests answered that a start lost, while the client
// has yet to learn the time of that start.
static const uint32_t HISTORY_FROM_UNKNOWN = 1;

struct Journal {
  uint32_t requestId;    // the request of the last run that reached a commit point
  uint8_t phase;         // JournalPhase: how far that run came
  uint8_t controller;    // the Modbus id of the controller that run talked to, 0 when not known
  uint8_t readFunction;  // the Modbus function it read that controller with (3 or 4), 0 when not known
  uint32_t answeredId;   // the newest request this client has answered or taken on
  uint32_t waitingId;    // the request that waits, or runs and has not reached a commit point; 0 for none
  bool reportDue;        // `report` has yet to go to the server
  uint32_t reportId;     // the request it answers
  uint32_t reportEpoch;  // when it came about, in whole seconds; while the client's clock was not set,
                         // 0 until it goes out, and then the time it went out with (reportSending())
  ProgramResult report;
  uint32_t reportOut;    // the time `report` went out with, 0 while it has not gone out: a restart
                         // before the Notecard has it sends it again with that time (Supervisor::sendEpoch())
  bool lookDue;          // `look` has yet to go to the server: the result of a look for request 0
  uint32_t lookEpoch;    // when it came about, as reportEpoch
  ProgramResult look;
  uint32_t lookOut;      // the time `look` went out with, as reportOut
  uint32_t sentId;       // the request of the last result that went out (not a look), or goes out now; 0 for none
  uint32_t sentEpoch;    // the time that result went out with, 0 for none
  uint32_t lookSentEpoch;  // the time the last look went out with, 0 for none
  uint32_t keptTaken;    // the check value of a journal kept aside that this one took in, 0 for none
  uint32_t historyFrom;  // the record of the requests answered holds only those asked for from this
                         // time on, less HISTORY_MARGIN_SECONDS: 0 when it holds them all,
                         // HISTORY_FROM_UNKNOWN while the time is not known (see cautiousJournal())
};

// The journal of a client that owes nothing and has answered nothing.
inline Journal emptyJournal() {
  Journal journal;
  memset(&journal, 0, sizeof(journal));
  return journal;
}

// The journal of a client whose journal cannot be read: it may owe the controller a restart, and
// it has lost its record of the requests it answered. A request asked for before this start (or
// within HISTORY_MARGIN_SECONDS after it, for the difference between the server's clock and the
// client's) may have been answered already, and is refused with REFUSE_HISTORY_LOST; one asked
// for later, and every duty, go on as before. The journal keeps the start (Journal::historyFrom),
// as a time once the clock has one (Supervisor::service()), so that later starts refuse the same
// requests: the record they read holds none of those either. A start that reads
// HISTORY_FROM_UNKNOWN stands for the start that lost the record, and is later. A request is fresh
// for a day (requestIsFresh()), so the time holds back no request asked for after it.
inline Journal cautiousJournal() {
  Journal journal = emptyJournal();
  journal.phase = JOURNAL_WRITING;
  journal.historyFrom = HISTORY_FROM_UNKNOWN;
  return journal;
}

// A journal that a start could not read and kept aside (the client's keepUnreadChargerJournal()),
// which reads at a later start, taken into `journal`, the one written since. What it holds that
// `journal` does not comes in:
//   - the duty to the controller its run talked to, where `journal` owes none, or owes a look to
//     no controller it names: the cautious look that stands for this kept journal, which went to
//     the controller the config names. (A cautious look that stands for another journal, one that
//     does not read at this start or holds no journal, the client gives the controller the config
//     names before it calls this: its loadChargerJournal().) A duty that `journal` owes a
//     known controller stays: it is newer, and a journal holds one. When that is another
//     controller, the kept duty is left out (keptDutyLeftOut()): the client keeps the kept file,
//     and takes its duty up once the one owed now is settled (Supervisor::takeKeptDuty());
//   - the requests it answered or took on, so that none of them is acted on again;
//   - a request that waited, newer than any that `journal` answered: begin() answers it as
//     interrupted, unless `journal` has one that waits;
//   - its result that had yet to go out, unless `journal` holds one of the same request or a
//     newer one, or sent one; and its look, while `journal` has none and sent none; each with the
//     time it went out with, if it had gone out (Journal::reportOut);
//   - the times its last result and its last look went out with, where they are later: a later
//     result of the same request, and a later look, go out later still (Supervisor::sendEpoch());
//   - the time from which its record of the requests answered is whole, where it is later: the
//     record is whole only from the later of the two (Journal::historyFrom).
// `keptTaken` stays as `journal` has it: the caller sets it to the check value of the kept file.
inline Journal mergeKeptJournal(Journal journal, const Journal &kept) {
  uint32_t answered = journal.answeredId;
  if (answered < journal.requestId) {
    answered = journal.requestId;
  }
  if (answered < journal.waitingId) {
    answered = journal.waitingId;
  }
  if (kept.phase != JOURNAL_IDLE && kept.controller != 0 &&
      (journal.phase == JOURNAL_IDLE || journal.controller == 0)) {
    journal.requestId = kept.requestId;
    journal.phase = kept.phase;
    journal.controller = kept.controller;
    journal.readFunction = kept.readFunction;
  }
  if (kept.waitingId > answered && journal.waitingId == 0) {
    journal.waitingId = kept.waitingId;
  }
  if (kept.reportDue && kept.reportId > journal.sentId && !(journal.reportDue && journal.reportId >= kept.reportId)) {
    journal.reportDue = true;
    journal.reportId = kept.reportId;
    journal.reportEpoch = kept.reportEpoch;
    journal.report = kept.report;
    journal.reportOut = kept.reportOut;
  }
  if (kept.sentId > journal.sentId || (kept.sentId == journal.sentId && kept.sentEpoch > journal.sentEpoch)) {
    journal.sentId = kept.sentId;
    journal.sentEpoch = kept.sentEpoch;
  }
  if (kept.lookDue && !journal.lookDue && journal.lookSentEpoch == 0) {
    journal.lookDue = true;
    journal.lookEpoch = kept.lookEpoch;
    journal.look = kept.look;
    journal.lookOut = kept.lookOut;
  }
  if (kept.lookSentEpoch > journal.lookSentEpoch) {
    journal.lookSentEpoch = kept.lookSentEpoch;
  }
  if (journal.historyFrom != HISTORY_FROM_UNKNOWN &&
      (kept.historyFrom == HISTORY_FROM_UNKNOWN || kept.historyFrom > journal.historyFrom)) {
    journal.historyFrom = kept.historyFrom;
  }
  const uint32_t keptIds[] = {answered, kept.answeredId, kept.requestId, kept.waitingId, kept.reportId};
  for (uint8_t i = 0; i < sizeof(keptIds) / sizeof(keptIds[0]); ++i) {
    if (journal.answeredId < keptIds[i]) {
      journal.answeredId = keptIds[i];
    }
  }
  return journal;
}

// Whether mergeKeptJournal() leaves the duty of `kept` out: `journal` owes a duty to another
// controller, and both know which one. A duty owed to the same controller covers it: the look
// goes to that controller, and asks it what it holds.
inline bool keptDutyLeftOut(const Journal &journal, const Journal &kept) {
  return kept.phase != JOURNAL_IDLE && kept.controller != 0 && journal.phase != JOURNAL_IDLE &&
         journal.controller != 0 && journal.controller != kept.controller;
}

// Where the client keeps the journal.
class JournalStore {
 public:
  // Writes the journal to the client's flash. True only once it is there, whole.
  virtual bool saveJournal(const Journal &journal) = 0;
};

// What a run needs from the client, besides the controller.
class Port : public Bus {
 public:
  // Leaves the line alone between two requests of a run: for at least `ms`, and for one Modbus
  // timeout when the last request got no reply. Keeps the client's watchdog fed.
  virtual void rest(uint16_t ms, bool afterMiss) = 0;
  // The Modbus id of the controller this port talks to.
  virtual uint8_t controller() const = 0;
  // The Modbus function it reads registers with: 3 or 4, or 0 while it has yet to find one that
  // the controller answers (see FunctionChoice).
  virtual uint8_t readFunction() const = 0;
  // False for a port whose link did not start (NoLinkPort): nothing it sends reaches the line.
  virtual bool linkStarted() const { return true; }
};

// The port of a pass in which the client could not start its link to the controller, for a duty
// whose turn has come (Supervisor::wantsLine()). The duty is tried on it all the same: every
// request ends at once without a reply, as on a line where the controller does not answer, so
// the try counts, its result goes out with NOTE_NO_LINK, and the next try comes on the duty's
// timetable (DUTY_RETRY_MS), not in the next pass. Nothing was sent, so the line does not rest
// after it.
class NoLinkPort : public Port {
 public:
  NoLinkPort(uint8_t controller, uint8_t readFunction, uint32_t nowMs)
      : controller_(controller), readFunction_(readFunction), nowMs_(nowMs) {}

  IoStatus readRegister(uint16_t, uint16_t *raw) override {
    if (raw != nullptr) {
      *raw = 0;
    }
    return SSIO_NO_REPLY;
  }
  IoStatus writeRegister(uint16_t, uint16_t) override { return SSIO_NO_REPLY; }
  IoStatus writeCoil(uint16_t) override { return SSIO_NO_REPLY; }
  uint32_t millisNow() override { return nowMs_; }  // no request takes any time
  uint16_t timeoutMs() const override { return 0; }
  uint32_t requestLimitMs() const override { return 0; }
  void rest(uint16_t, bool) override {}
  uint8_t controller() const override { return controller_; }
  uint8_t readFunction() const override { return readFunction_; }
  bool linkStarted() const override { return false; }

 private:
  uint8_t controller_;
  uint8_t readFunction_;
  uint32_t nowMs_;
};

// Reads one register in one request, with Modbus function 3 (holding registers) or 4 (input
// registers): what a FunctionChoice reads through.
class FunctionReader {
 public:
  virtual ~FunctionReader() {}
  virtual IoStatus readWith(uint8_t function, uint16_t addr, uint16_t *raw) = 0;
};

// The Modbus function a port reads its controller with. The poll finds the one that the
// controller the config names answers, and the journal keeps the one that a run's controller
// answered (Supervisor::readFunctionToUse()). When neither speaks for the controller, the port
// finds out: it reads with function 3, and a register the controller refuses that way is asked
// once more with function 4, which is kept from then on when the controller answers it. Once a
// register has answered, the function is known, and a refusal is the controller's answer.
class FunctionChoice {
 public:
  // `known`: 3 or 4, or 0 when it is to be found out.
  explicit FunctionChoice(uint8_t known = 0) : function_(known == 4 ? 4 : 3), known_(known == 3 || known == 4) {}

  IoStatus read(FunctionReader &reader, uint16_t addr, uint16_t *raw) {
    IoStatus status = reader.readWith(function_, addr, raw);
    if (!known_ && status == SSIO_REFUSED) {
      // A refusal is a reply, so nothing late can follow it: the other function may ask at once.
      const IoStatus other = reader.readWith(4, addr, raw);
      if (other == SSIO_OK) {
        function_ = 4;
      }
      if (other != SSIO_REFUSED) {
        status = other;  // its answer, or none: then the line rests as after any miss
      }
    }
    if (status == SSIO_OK) {
      known_ = true;
    }
    return status;
  }

  // 3 or 4, or 0 while no register has answered.
  uint8_t function() const { return known_ ? function_ : 0; }

  // The longest one read() may take when one request may take `requestMs`: while the function
  // is not known, a register refused with function 3 is asked once more, and a refusal, a whole
  // reply, comes within the Modbus timeout `timeoutMs` and its frames.
  uint32_t readLimitMs(uint32_t requestMs, uint16_t timeoutMs) const {
    return known_ ? requestMs : requestMs + timeoutMs + MODBUS_FRAMES_MS;
  }

 private:
  uint8_t function_;  // 3 while the function is not known
  bool known_;
};

// What the client knows of the line at a pass of its loop: see Supervisor::mayTalk().
struct Line {
  bool free;           // nothing keeps the client off the line in this pass: no firmware update is
                       // being staged, no poll took the pass, and the line does not rest after a miss
  bool polling;        // the client polls the charger: it is on in the config, and its link started
  bool pollAnswered;   // and the controller the config names answered the last poll
  uint8_t configured;  // the Modbus id the config names
};

// Who has the line in a pass of the client's loop. The poll, the charger check and charger
// programming all talk to the charger, so a pass talks for one of them only, and the poll comes
// first. A short poll interval with the long sleep of ECO or LOW_POWER makes every pass due to
// poll. So after a poll the others have the next pass in which the line is free, when they have
// something to do (a check under way, programming with something due), and the poll waits for
// the pass after. They keep that turn while the line rests after a request that got no reply:
// a poll is due again by the time the rest ends when its interval is short and its controller
// does not answer, and it would take every free pass. Once per pass of the client's loop:
//
//   const bool polled = !turns.pollWaits(othersWant) && poll();
//   turns.afterPoll(polled, lineResting());
class LineTurns {
 public:
  LineTurns() : othersNext_(false) {}
  // Whether the poll is to leave this pass to the others. `othersWant`: a check is under way, or
  // programming has something due (Supervisor::wantsLine()).
  bool pollWaits(bool othersWant) const { return othersNext_ && othersWant; }
  // After the poll: `polled`, it talked to the charger in this pass; `lineResting`, the line
  // rests now.
  void afterPoll(bool polled, bool lineResting) {
    if (polled) {
      othersNext_ = true;
    } else if (!lineResting) {
      othersNext_ = false;  // this pass has the line free: it is theirs, if they want it
    }
  }

 private:
  bool othersNext_;
};

static const uint32_t REQUEST_WAIT_MS = 3600000ul;        // a request waits this long for the clock and the charger
static const double REQUEST_MAX_AGE_SECONDS = 86400.0;    // and may be this old when it runs
static const double REQUEST_MAX_AHEAD_SECONDS = 3600.0;   // or this far ahead of the client's clock
static const double CLOCK_SET_EPOCH = 1600000000.0;       // 2020-09-13: a clock that reads less was never set
static const double HISTORY_MARGIN_SECONDS = 600.0;       // see cautiousJournal()
static const uint32_t DUTY_RETRY_MS = 600000ul;           // between two tries of a duty
static const uint32_t DUTY_SLOW_RETRY_MS = 86400000ul;    // and after DUTY_FAST_TRIES of them
static const uint8_t DUTY_FAST_TRIES = 3;
static const uint32_t JOURNAL_RETRY_MS = 600000ul;        // between two tries to write a journal that flash did not take
static const uint16_t RUN_STEP_LIMIT = 2000;              // a run has under 400 steps: no run goes on beyond this

// True for a request asked for at `askedAt`, the server's time then, that a client with the time
// `epochNow` may still act on. That time travels with the request ("at", readRequestAt()), apart
// from its id: the id only orders the requests, and goes on from the last one when the server's
// clock went back. A request without it has its time as its id.
inline bool requestIsFresh(uint32_t askedAt, double epochNow) {
  const double asked = (double)askedAt;
  return epochNow >= CLOCK_SET_EPOCH && asked >= epochNow - REQUEST_MAX_AGE_SECONDS &&
         asked <= epochNow + REQUEST_MAX_AHEAD_SECONDS;
}

class Supervisor {
 public:
  explicit Supervisor(uint16_t stepLimit = RUN_STEP_LIMIT) : store_(nullptr), stepLimit_(stepLimit) {
    begin(emptyJournal(), nullptr, 0);
  }

  // Once, when the client starts, with the journal from its flash (emptyJournal() when there is
  // none, cautiousJournal() when there is no telling what it held: the file cannot be read, there
  // is no flash, or the flash lost its files at this start) and the store that keeps it. A
  // request that waited is answered now as interrupted, and a result that had yet to go out is
  // due again.
  void begin(const Journal &journal, JournalStore *store, uint32_t nowMs) {
    store_ = store;
    programmer_ = Programmer();
    journal_ = journal;
    if (journal_.phase > JOURNAL_PHASE_LAST) {
      journal_.phase = JOURNAL_WRITING;  // a phase of a newer firmware: the cautious reading
    }
    if (journal_.answeredId < journal_.requestId) {
      journal_.answeredId = journal_.requestId;
    }
    if (journal_.answeredId < journal_.waitingId) {
      journal_.answeredId = journal_.waitingId;
    }
    journalUnsaved_ = false;
    journalTryMs_ = nowMs;
    memset(&waitingProfile_, 0, sizeof(waitingProfile_));
    waitingLoadAllowed_ = false;
    waitingAskedAt_ = 0;
    waitingSinceMs_ = nowMs;
    runId_ = 0;
    dutyForced_ = dutyOwed();
    dutyTries_ = 0;
    dutyTryMs_ = nowMs;
    reportMs_ = nowMs;
    lookMs_ = nowMs;
    startMs_ = nowMs;
    if (journal_.waitingId != 0) {
      // The client restarted while the request waited, or before its run reached the controller.
      const uint32_t requestId = journal_.waitingId;
      journal_.waitingId = 0;
      ProgramResult result;
      interruptedResult(&result);
      post(requestId, result, 0.0, nowMs);
      saveJournal(nowMs);
    }
  }

  // The duty of a journal kept aside, which the client's start did not take in because the
  // journal owed a duty to another controller then (keptDutyLeftOut()). Taken once no duty is
  // owed: a look at the controller the kept journal's run talked to, at the next chance, which
  // asks that controller what it holds. `check` is the check value of the kept file
  // (Journal::keptTaken), which the journal holds from then on. Returns false, and does nothing,
  // while a duty is owed, or when `kept` owes none.
  bool takeKeptDuty(const Journal &kept, uint32_t check, uint32_t nowMs) {
    if (dutyOwed() || kept.phase == JOURNAL_IDLE) {
      return false;
    }
    journal_.requestId = kept.requestId;
    journal_.phase = kept.phase > JOURNAL_PHASE_LAST ? (uint8_t)JOURNAL_WRITING : kept.phase;
    journal_.controller = kept.controller;
    journal_.readFunction = kept.readFunction;
    if (journal_.answeredId < kept.requestId) {
      journal_.answeredId = kept.requestId;
    }
    journal_.keptTaken = check;
    programmer_ = Programmer();  // no run of this start is taken up for it
    dutyForced_ = true;
    dutyTries_ = 0;
    dutyTryMs_ = nowMs;
    saveJournal(nowMs);
    return true;
  }

  // A config carried a request. `profileErrors` is what readProfile() found, `clientRefusals`
  // the Refusal bits the client found by itself (REFUSE_NOT_BUILT, REFUSE_NOT_LIFEPO4,
  // REFUSE_NO_CHARGER). `askedAt` is the server's time when it was asked for (readRequestAt()),
  // 0 for a request whose id is that time. A request that is not newer than every one this client
  // has answered or taken on is ignored: a config is sent again when its acknowledgement is lost.
  // A new request takes the place of one that still waits. The request is in flash, or answered,
  // on return. Returns false for a request that is ignored, true for one that waits now or was
  // refused.
  bool request(uint32_t requestId, const Profile &profile, uint16_t profileErrors, uint32_t clientRefusals,
               bool loadCurrentAllowed, double epochNow, uint32_t nowMs, uint32_t askedAt = 0) {
    if (requestId == 0 || requestId <= journal_.answeredId) {
      return false;
    }
    journal_.answeredId = requestId;
    uint32_t refusals = clientRefusals;
    if (profileErrors != 0) {
      refusals |= REFUSE_PROFILE;
    }
    if (refusals == 0) {
      journal_.waitingId = requestId;
      waitingProfile_ = profile;
      waitingLoadAllowed_ = loadCurrentAllowed;
      waitingAskedAt_ = askedAt != 0 ? askedAt : requestId;
      waitingSinceMs_ = nowMs;
      if (saveJournal(nowMs)) {
        dutyForced_ = true;  // a duty that is owed is tried now, and not at its next turn
        return true;
      }
      refusals = REFUSE_JOURNAL;  // flash does not keep it: a restart of the client would lose it
    }
    journal_.waitingId = 0;
    ProgramResult result;
    refusedResult(profile, profileErrors, refusals, &result);
    post(requestId, result, epochNow, nowMs);
    saveJournal(nowMs);
    return true;
  }

  // Once per pass of the client's loop, with what the client knows of the line (see mayTalk()).
  // `port` talks to controllerToUse(line.configured). Returns true when it talked to the
  // controller: that held the caller for a whole run.
  bool service(Port &port, double epochNow, const Line &line) { return service(port, epochNow, mayTalk(port, line)); }

  // The same, with `ready` decided by the caller: the controller may be talked to now.
  // `epochNow` is the client's clock, or anything below CLOCK_SET_EPOCH while it is not set.
  bool service(Port &port, double epochNow, bool ready) {
    const uint32_t nowMs = port.millisNow();
    maintain(nowMs);
    if (journal_.historyFrom != 0 && epochNow >= CLOCK_SET_EPOCH) {
      const uint32_t from = (uint32_t)historyStart(epochNow, nowMs);
      if (from != journal_.historyFrom) {
        journal_.historyFrom = from;  // the start as a time, taken once: millis() may come round later
        saveJournal(nowMs);
      }
    }
    if (dutyOwed()) {
      if (ready && dutyTurn(nowMs)) {
        // The run the duty is about is taken up again, if this client still holds it. After a
        // restart of the client, or after a recovery, it holds none: a recovery asks the controller.
        runId_ = journal_.requestId;
        if (!programmer_.resume()) {
          programmer_.startRecovery(journal_.phase == JOURNAL_RESTARTING);
        }
        drive(port);
        settle(port, epochNow, nowMs);
        return true;
      }
      if (journal_.waitingId != 0 && waitOver(nowMs)) {
        refuseWaiting(REFUSE_NO_CHARGER, epochNow, nowMs);  // the duty comes first, and the charger did not let it be done
      }
      return false;
    }
    if (journal_.waitingId == 0) {
      return false;
    }
    // Both limits hold up to the moment the run starts.
    const bool clockSet = epochNow >= CLOCK_SET_EPOCH;
    if (clockSet && !requestIsFresh(waitingAskedAt_, epochNow)) {
      refuseWaiting(REFUSE_STALE, epochNow, nowMs);
      return false;
    }
    if (clockSet && askedBeforeHistory(waitingAskedAt_, epochNow, nowMs)) {
      refuseWaiting(REFUSE_HISTORY_LOST, epochNow, nowMs);
      return false;
    }
    if (waitOver(nowMs)) {
      // An hour has gone by: waiting for the clock (which may have come just now), or for the charger.
      refuseWaiting(clockSet && !ready ? REFUSE_NO_CHARGER : REFUSE_STALE, epochNow, nowMs);
      return false;
    }
    if (!clockSet || !ready) {
      return false;
    }
    runId_ = journal_.waitingId;
    programmer_.start(waitingProfile_, waitingLoadAllowed_);
    drive(port);
    settle(port, epochNow, nowMs);
    return true;
  }

  // A result waits to be sent to the server: the request it answers (0 for a look, which answers
  // none), the client's millis() when it came about, and that moment as a time when the clock
  // was set then (0 otherwise, until it goes out: reportSending(); before that, the time of
  // reportMillis() is the best that is known, and after a restart of the client, that is the
  // moment of the restart). A newer result of the same request takes the place of one that was
  // not sent yet, and so does the result of a newer request. A result of an older request never
  // does: the server keeps the newest request's anyway. A look has a place of its own, as at the
  // server, and only a newer look takes it. When both wait, the one that came about first goes
  // first: these speak of that one, until reportTaken().
  bool reportDue() const { return journal_.reportDue || journal_.lookDue; }
  const ProgramResult &report() const { return lookFirst() ? journal_.look : journal_.report; }
  uint32_t reportRequestId() const { return lookFirst() ? 0u : journal_.reportId; }
  uint32_t reportMillis() const { return lookFirst() ? lookMs_ : reportMs_; }
  // The same moment for each of the two, whichever goes first: the result of a request, and the
  // look. A client that counts the age of a result while it has no time counts both in every
  // pass, the one that waits behind the other too.
  uint32_t resultMillis() const { return reportMs_; }
  uint32_t lookMillis() const { return lookMs_; }
  uint32_t reportEpoch() const { return lookFirst() ? journal_.lookEpoch : journal_.reportEpoch; }
  // The time to send that result with, given `epoch`, the time it came about: later than the
  // time the last result of the same request went out with (the last look, for a look). The
  // server orders the results of a request by their time, and the looks by theirs: a recovery
  // that settles a run must not read as older than the run's result because the client's clock
  // was set back in between. A result that has gone out once (Journal::reportOut, lookOut) goes
  // out again with the time it went out with, and not a second later: a later result of the same
  // request went out a second later than it at least, possibly while flash took nothing, and with
  // the same time the server would take the older one back.
  uint32_t sendEpoch(uint32_t epoch) const {
    const uint32_t out = lookFirst() ? journal_.lookOut : journal_.reportOut;
    if (out != 0) {
      return out;
    }
    const uint32_t last = lookFirst() ? journal_.lookSentEpoch
                                      : (journal_.sentId == journal_.reportId ? journal_.sentEpoch : 0u);
    return last != 0 && last != 0xFFFFFFFFu && epoch <= last ? last + 1 : epoch;
  }
  // That result is about to go out with the time `sentEpoch` (sendEpoch()). The time goes into the
  // journal first, as the time the last result of its request (the last look, for a look) went
  // out with, and as the time this result went out with (Journal::reportOut, lookOut): the
  // Notecard may take the result and the client restart before reportTaken() is in flash, and a
  // later result of the same request, or a later look, must still go out with a later time. A
  // newer result that takes the place of this one before it goes out goes out later than that
  // time too. The result stays due until reportTaken(). A result that came about while the clock
  // was not set has no time of its own (reportEpoch() is 0): from here on that time is its own,
  // in the journal with it. A restart before reportTaken() then sends it again with the time it
  // went out with (sendEpoch()), and not with the time of that restart: a later result of the
  // same request, which went out meanwhile a second later at least and which flash did not
  // take, stays the later one at the server. Returns false when flash did not take the journal:
  // a restart may then lose the time, as it would lose the result. Nothing is written when the
  // journal in flash holds that time already.
  bool reportSending(uint32_t sentEpoch, uint32_t nowMs) {
    if (lookFirst()) {
      if (journal_.lookSentEpoch == sentEpoch && journal_.lookOut == sentEpoch && journal_.lookEpoch != 0 &&
          !journalUnsaved_) {
        return true;
      }
      journal_.lookSentEpoch = sentEpoch;
      journal_.lookOut = sentEpoch;
      if (journal_.lookEpoch == 0) {
        journal_.lookEpoch = sentEpoch;
      }
    } else if (journal_.reportDue) {
      if (journal_.sentId == journal_.reportId && journal_.sentEpoch == sentEpoch && journal_.reportOut == sentEpoch &&
          journal_.reportEpoch != 0 && !journalUnsaved_) {
        return true;
      }
      journal_.sentId = journal_.reportId;
      journal_.sentEpoch = sentEpoch;
      journal_.reportOut = sentEpoch;
      if (journal_.reportEpoch == 0) {
        journal_.reportEpoch = sentEpoch;
      }
    } else {
      return true;
    }
    return saveJournal(nowMs);
  }
  // The Notecard has that result, sent with the time `sentEpoch`. Not the client's note buffer:
  // that drops its oldest records once it grows too large, and the journal keeps the result until
  // the Notecard takes it.
  void reportTaken(uint32_t sentEpoch, uint32_t nowMs) {
    if (lookFirst()) {
      journal_.lookDue = false;
      journal_.lookOut = 0;
      journal_.lookSentEpoch = sentEpoch;
    } else if (journal_.reportDue) {
      journal_.reportDue = false;
      journal_.reportOut = 0;
      journal_.sentId = journal_.reportId;
      journal_.sentEpoch = sentEpoch;
    } else {
      return;
    }
    saveJournal(nowMs);
  }

  // The client gives up the request that waits: a newer config no longer asks for it, or names
  // another controller (REFUSE_WITHDRAWN), or the client can no longer take it on
  // (REFUSE_NOT_LIFEPO4, REFUSE_NO_CHARGER). It is refused with `refusals` as it stands, and its
  // result goes to the server. A duty that is owed stays owed. Returns false, and does nothing,
  // when no request waits or `refusals` holds no bit.
  bool withdraw(uint32_t refusals, double epochNow, uint32_t nowMs) {
    if (journal_.waitingId == 0 || refusals == 0) {
      return false;
    }
    refuseWaiting(refusals, epochNow, nowMs);
    return true;
  }

  // What service() does besides talking to the controller, for a client that does not call
  // service(): its firmware cannot talk to the controller (built without programming, or with no
  // RS-485). Once per pass of its loop. A journal that flash did not take is written again,
  // JOURNAL_RETRY_MS after the last try, so that a result that waits to go out and the requests
  // answered outlast a restart once the flash takes it. service() calls it too.
  void maintain(uint32_t nowMs) {
    if (journalUnsaved_ && (uint32_t)(nowMs - journalTryMs_) >= JOURNAL_RETRY_MS) {
      saveJournal(nowMs);
    }
  }
  // Flash did not take the journal as it is here: a restart would lose what changed since the
  // last journal that it took.
  bool journalUnsaved() const { return journalUnsaved_; }
  // A result waits to go out and is in RAM alone: flash did not take the journal that holds it,
  // and the Notecard does not have it yet. The client acknowledges no config while this holds, so
  // that the server sends it again, with the request it may carry, as long as it waits for the
  // acknowledgement: a client that restarted in that time takes the request on again.
  // A look is not counted: no config brings it back, and a client that restarts before flash
  // takes it owes the controller the look again (its duty is in flash until a journal without it is).
  bool resultOnlyInRam() const { return journal_.reportDue && journalUnsaved_; }

  const Journal &journal() const { return journal_; }
  // A request waits for the clock, the charger or a duty.
  bool requestWaiting() const { return journal_.waitingId != 0; }
  // The id of the request that waits, 0 when none does.
  uint32_t waitingRequestId() const { return journal_.waitingId; }
  // Whether service() would talk to the controller now if it were ready: a duty whose turn has
  // come, or a request that can run (a fresh one with the clock set, within its hour). The
  // client gives such a pass to programming when every pass of its loop is due to poll. It never
  // says no where service() would talk, nor yes where service() would not.
  bool wantsLine(double epochNow, uint32_t nowMs) const {
    if (dutyOwed()) {
      return dutyTurn(nowMs);
    }
    return journal_.waitingId != 0 && requestIsFresh(waitingAskedAt_, epochNow) &&
           !askedBeforeHistory(waitingAskedAt_, epochNow, nowMs) && !waitOver(nowMs);
  }
  // The controller may wait for a restart, and the client has yet to settle that.
  bool dutyOwed() const { return journal_.phase == JOURNAL_RESTARTING || journal_.phase == JOURNAL_WRITING; }
  // Whether service() may talk to the controller of `port` in a pass with this line. A request
  // waits for a poll that shows the link works, since its run talks to the controller the config
  // names. So does a duty owed to that controller while the client polls: a dead link costs it no
  // try. A duty owed to another controller (the config names a new id now), or one that is owed
  // while no poll runs (the charger is off in the config), does without: the poll says nothing of
  // its controller, and a controller that waits for its restart may not charge. Its first request
  // asks the controller, and a miss leaves the duty owed until its next turn (dutyTurn()). So does
  // a link that the client starts for the duty and that does not start: the try is made on a
  // NoLinkPort.
  bool mayTalk(const Port &port, const Line &line) const { return mayTalkTo(port.controller(), line); }
  // The same for a port that talks to the controller `controller`.
  bool mayTalkTo(uint8_t controller, const Line &line) const {
    if (!line.free) {
      return false;
    }
    if (line.polling && line.pollAnswered) {
      return true;
    }
    return dutyOwed() && (!line.polling || controller != line.configured);
  }
  // Whether the client is to give programming a pass that a poll would take: wantsLine(), in a
  // pass in which service() may talk with `line` as it stands (mayTalk(), for the controller
  // controllerToUse() names). A request, and a duty owed to the controller the config names, wait
  // for a poll that answered: a pass given to them before that goes unused, and holds back the
  // poll that may bring that answer, such as the one that tells that the battery recovered in
  // hibernation. Whether the line is free in this pass is not asked: that is for LineTurns.
  bool wantsPass(double epochNow, uint32_t nowMs, Line line) const {
    line.free = true;
    return wantsLine(epochNow, nowMs) && mayTalkTo(controllerToUse(line.configured), line);
  }
  // The Modbus id to talk to: the controller a duty is owed to, when the journal knows which
  // one that is, and otherwise `configured`, the one the client's config names.
  uint8_t controllerToUse(uint8_t configured) const {
    return dutyOwed() && journal_.controller != 0 ? journal_.controller : configured;
  }
  // The Modbus function to read controllerToUse(configured) with (FunctionChoice). For the
  // controller the config names, `configuredFunction`: the one its poll found, when the poll
  // answered (0 otherwise). For the controller a duty is owed to, the one its run read it with.
  // 0 when neither is known: the port finds out.
  uint8_t readFunctionToUse(uint8_t configured, uint8_t configuredFunction) const {
    const uint8_t target = controllerToUse(configured);
    if (target == configured && configuredFunction != 0) {
      return configuredFunction;
    }
    return dutyOwed() && journal_.controller == target ? journal_.readFunction : (uint8_t)0;
  }

 private:
  Programmer programmer_;
  Journal journal_;
  JournalStore *store_;
  bool journalUnsaved_;  // flash did not take the journal as it is here
  uint32_t journalTryMs_;
  Profile waitingProfile_;
  bool waitingLoadAllowed_;
  uint32_t waitingAskedAt_;  // when the request that waits was asked for (requestIsFresh())
  uint32_t waitingSinceMs_;
  uint32_t runId_;       // the request the run under way answers
  bool dutyForced_;      // try the duty at the next chance, whatever its timer says
  uint8_t dutyTries_;
  uint32_t dutyTryMs_;
  uint32_t reportMs_;
  uint32_t lookMs_;
  uint32_t startMs_;  // the client's millis() at this start (historyStart())
  uint16_t stepLimit_;

  bool waitOver(uint32_t nowMs) const { return (uint32_t)(nowMs - waitingSinceMs_) >= REQUEST_WAIT_MS; }

  // The time from which the record of the requests answered holds them all (Journal::historyFrom),
  // in whole seconds, from the clock now: this start for HISTORY_FROM_UNKNOWN, and for a time that
  // no clock gives or that is later than the clock now, which is no start's.
  double historyStart(double epochNow, uint32_t nowMs) const {
    const double kept = (double)journal_.historyFrom;  // HISTORY_FROM_UNKNOWN is a time no clock gives
    if (kept >= CLOCK_SET_EPOCH && kept <= epochNow) {
      return kept;
    }
    double at = epochNow - (double)(uint32_t)(nowMs - startMs_) / 1000.0;
    at = at < CLOCK_SET_EPOCH ? CLOCK_SET_EPOCH : at > 4294967295.0 ? 4294967295.0 : at;
    return (double)(uint32_t)at;
  }
  // A request asked for at `askedAt` (the server's time) that a record of the requests answered
  // that a start lost may have answered (cautiousJournal()).
  bool askedBeforeHistory(uint32_t askedAt, double epochNow, uint32_t nowMs) const {
    return journal_.historyFrom != 0 && (double)askedAt < historyStart(epochNow, nowMs) + HISTORY_MARGIN_SECONDS;
  }

  // The duty that is owed may be tried now: at once after a request or a client's start, and then
  // DUTY_RETRY_MS apart, DUTY_SLOW_RETRY_MS after DUTY_FAST_TRIES tries.
  bool dutyTurn(uint32_t nowMs) const {
    const uint32_t gap = dutyTries_ >= DUTY_FAST_TRIES ? DUTY_SLOW_RETRY_MS : DUTY_RETRY_MS;
    return dutyForced_ || (uint32_t)(nowMs - dutyTryMs_) >= gap;
  }

  // Of the two results that wait, the look goes first when it came about first.
  bool lookFirst() const {
    return journal_.lookDue && (!journal_.reportDue || (int32_t)(lookMs_ - reportMs_) < 0);
  }

  void post(uint32_t requestId, const ProgramResult &result, double epochNow, uint32_t nowMs) {
    const uint32_t epoch = epochNow >= CLOCK_SET_EPOCH ? (uint32_t)epochNow : 0u;
    if (requestId == 0) {
      // A look for no request: a start that could not tell from its flash what was owed. The
      // server keeps it apart from the results of requests, and so does the journal.
      journal_.look = result;
      journal_.lookEpoch = epoch;
      journal_.lookOut = 0;  // it has yet to go out
      journal_.lookDue = true;
      lookMs_ = nowMs;
      return;
    }
    if (journal_.reportDue && journal_.reportId > requestId) {
      return;  // the result of a newer request has yet to go out
    }
    journal_.report = result;
    journal_.reportId = requestId;
    journal_.reportEpoch = epoch;
    journal_.reportOut = 0;  // it has yet to go out
    journal_.reportDue = true;
    reportMs_ = nowMs;
  }

  bool saveJournal(uint32_t nowMs) {
    journalTryMs_ = nowMs;
    journalUnsaved_ = store_ == nullptr || !store_->saveJournal(journal_);
    return !journalUnsaved_;
  }

  void refuseWaiting(uint32_t reasons, double epochNow, uint32_t nowMs) {
    const uint32_t requestId = journal_.waitingId;
    journal_.waitingId = 0;
    ProgramResult result;
    refusedResult(waitingProfile_, 0, reasons, &result);
    post(requestId, result, epochNow, nowMs);
    saveJournal(nowMs);
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
      next.requestId = runId_;
      next.phase = commit == COMMIT_RESTART ? (uint8_t)JOURNAL_RESTARTING : (uint8_t)JOURNAL_WRITING;
      next.controller = port.controller();
      next.readFunction = port.readFunction();
      if (next.waitingId == runId_) {
        next.waitingId = 0;  // from here on the journal speaks for the request
      }
      if (store_ != nullptr && store_->saveJournal(next)) {
        journal_ = next;
        journalUnsaved_ = false;
        programmer_.go();
      } else {
        programmer_.refuse(REFUSE_JOURNAL);
      }
    }
  }

  // A run or a duty has come to its end: its result goes out, and the journal says what is owed.
  // `epochAtStart` and `startMs` are the client's clock and millis() when it began.
  void settle(Port &port, double epochAtStart, uint32_t startMs) {
    const uint32_t nowMs = port.millisNow();
    const double epochNow =
        epochAtStart >= CLOCK_SET_EPOCH ? epochAtStart + (double)(uint32_t)(nowMs - startMs) / 1000.0 : epochAtStart;
    dutyForced_ = false;
    dutyTryMs_ = nowMs;
    ProgramResult result = programmer_.result();
    if (!programmer_.ended()) {
      // The step limit: the run is given up as it stands. What it may have left behind is a
      // duty, and a recovery settles it: resume() takes up no run that has not ended.
      result.outcome = OUTCOME_STOPPED;
      result.followUp = dutyOwed();
    }
    if (!port.linkStarted()) {
      // Nothing reached the controller: the result says that the client could not ask it, and
      // not that it did not answer.
      result.notes = (uint16_t)((result.notes & ~(uint16_t)NOTE_NOT_ANSWERED) | NOTE_NO_LINK);
    }
    if (journal_.waitingId == runId_) {
      journal_.waitingId = 0;  // a run that ended before its first commit point
    }
    post(runId_, result, epochNow, nowMs);
    if (result.followUp) {
      if (dutyTries_ < 0xFF) {
        ++dutyTries_;
      }
    } else {
      dutyTries_ = 0;
      journal_.phase = JOURNAL_IDLE;
    }
    saveJournal(nowMs);
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

// One line that sums a run up: "programmed: 14 of 17 registers written, 3 already set". A
// register that held its word and was not read after the restart counts as it went before it:
// written when it needed the word, already set when it held it.
inline void describeRun(const ProgramResult &result, Text *out) {
  out->add(outcomeName(result.outcome));
  if (result.planned == 0) {
    return;
  }
  uint8_t written = countEntries(result, ENTRY_WRITTEN);
  uint8_t set = countEntries(result, ENTRY_UNCHANGED);
  uint8_t unread = 0;
  for (uint8_t i = 0; i < result.planned && i < PLAN_ENTRIES; ++i) {
    const PlanEntry &entry = result.entries[i];
    if (entry.status == ENTRY_UNREAD) {
      ++unread;
      if (entry.before == entry.target) {
        ++set;
      } else {
        ++written;
      }
    }
  }
  out->add(": ").addUnsigned(written).add(" of ").addUnsigned(result.planned);
  out->add(" registers written, ").addUnsigned(set).add(" already set");
  if (unread != 0) {
    out->add(", ").addUnsigned(unread).add(" not read after the restart");
  }
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
