/**
 * OptaViewDevice.h
 *
 * One controller on the bus, seen through its profile: the link settings in use, reads that
 * fall back from block requests to single registers, the probe sweep, verified writes, the
 * reset sequence, the pair checks and the voltage scale check.
 *
 * Nothing here prints. The USB console (OptaViewConsole.h) decides what to show, and the web
 * page (OptaViewWeb.h) sits on the same calls. No Arduino dependency, so the host tests run this
 * code against a simulated SunSaver.
 *
 * More than one front end
 * -----------------------
 * The USB console and the web page work on the same controller at the same time. What one of
 * them learns or changes has to hold for the other, so it is kept here and in neither of them:
 * what 'scalecheck' found, whether settings are waiting for a restart, and a count of everything
 * that makes a question that is waiting for "yes" out of date (see generation()).
 *
 * Reads and the MRC-1
 * -------------------
 * The TankAlarm client reads the SunSaver one register per request, because block reads through
 * the MRC-1 adapter were never proven on the bench. OptaView asks for blocks first (they are much
 * faster) and falls back on its own:
 *   - a block gets no usable reply but a single register does  -> blocks are off for the session
 *   - a block is refused with an exception                      -> its registers are read one by
 *     one to find the register the controller objects to; if they all read, blocks are off
 * 'blocks on' and 'blocks off' in the console force either behaviour.
 */

#ifndef OPTAVIEW_DEVICE_H
#define OPTAVIEW_DEVICE_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "OptaViewModbus.h"
#include "OptaViewProfile.h"
#include "OptaViewText.h"

namespace optaview {

enum OvBlockMode : uint8_t { OV_BLOCKS_AUTO = 0, OV_BLOCKS_ON = 1, OV_BLOCKS_OFF = 2 };

inline const char *ovBlockModeName(uint8_t mode) {
  switch (mode) {
    case OV_BLOCKS_AUTO: return "auto";
    case OV_BLOCKS_ON: return "on";
    case OV_BLOCKS_OFF: return "off";
    default: return "?";
  }
}

struct OvLink {
  uint8_t slave;
  uint32_t baud;
  uint8_t format;        // MbFormat
  uint8_t readFunction;  // 3 or 4
};

// Word status for a register that was never asked for, because the link had stopped answering.
static const uint8_t OV_NOT_READ = 0xFF;

struct OvWordStatus {
  uint8_t status;     // MbStatus, or OV_NOT_READ
  uint8_t exception;  // exception code when status == MB_EXCEPTION
};

// "no reply", "exception 2 (illegal data address)", "not read"
inline void ovDescribeStatus(const OvWordStatus &s, char *out, size_t cap) {
  OvText text(out, cap);
  if (s.status == OV_NOT_READ) {
    text.add("not read");
  } else if (s.status == MB_EXCEPTION) {
    text.add("exception ");
    text.addU(s.exception);
    text.add(" (");
    text.add(mbExceptionText(s.exception));
    text.add(")");
  } else {
    text.add(mbStatusText(s.status));
  }
}

inline void ovDescribeResult(const MbResult &r, char *out, size_t cap) {
  OvWordStatus s;
  s.status = r.status;
  s.exception = r.exception;
  ovDescribeStatus(s, out, cap);
}

static const uint16_t OV_MAX_SPAN_WORDS = MB_MAX_READ_REGISTERS;
static const uint8_t OV_LINK_DOWN_AFTER = 3;      // failed requests in a row that end a batch
static const uint16_t OV_PROBE_TIMEOUT_MS = 400;  // reply wait during the probe sweep and scans
static const uint32_t OV_RESET_GIVE_UP_MS = 20000;
static const uint32_t OV_RESET_POLL_MS = 250;     // at most one request this often while a restart is awaited

// A run of registers at consecutive addresses.
struct OvSpan {
  size_t first;    // index of its first register in the profile
  size_t count;    // registers in it
  uint16_t addr;
  uint16_t words;
};

class OvAbortCheck {
 public:
  virtual ~OvAbortCheck() {}
  virtual bool aborted() = 0;
};

class OvProbeListener {
 public:
  virtual ~OvProbeListener() {}
  virtual void onProbeAttempt(const OvLink &, const MbResult &) {}
  virtual bool probeAborted() { return false; }
};

struct OvProbeOutcome {
  bool found;         // a register read was answered with data; `link` is now in use
  bool refused;       // a slave answered, but only with exceptions
  bool sawBytes;      // something arrived that was not a valid reply
  bool aborted;
  OvLink link;        // where it was found, or where the exception came from
  uint8_t exception;
  uint16_t raw;       // the probe register's value when found
  uint16_t attempts;
};

// What a write did to the register, as far as the acknowledgement and the read-back show. The
// acknowledgement alone cannot tell: it can be lost on the way back from a controller that has
// stored the value.
enum OvWriteEffect : uint8_t {
  OV_WRITE_NOT_TAKEN = 0,  // refused, or the register still reads what it held before
  OV_WRITE_TAKEN,          // acknowledged, or the register no longer reads what it held before
  OV_WRITE_UNKNOWN         // no acknowledgement, and the register could not be read afterwards
};

struct OvWriteOutcome {
  MbResult write;         // the write request; never repeated, a lost reply is not a lost write
  OvWordStatus readBack;  // reading the register again afterwards
  uint16_t readBackRaw;
  bool verified;          // the register now holds what was written
  uint8_t effect;         // OvWriteEffect
};

struct OvResetOutcome {
  bool sent;             // the profile has a reset coil and the command went out
  MbResult command;      // a missing reply is normal here: the controller may restart first
  bool wentAway;         // a request after the command went unanswered: the sign of a restart
  bool cameBack;         // ...and the controller answered again after that
  bool neverAway;        // it answered every request for the whole of the profile's resetWaitMs
  bool aborted;
  uint32_t backAfterMs;  // from the command to the answer that ended the wait
};

enum OvCheckResult : uint8_t {
  OV_CHECK_PASS = 0,
  OV_CHECK_FAIL,
  OV_CHECK_OFF,      // one of the two is 0 where 0 switches that feature off (zeroOk)
  OV_CHECK_ZERO,     // one of the two is 0 where 0 switches nothing off: that fails as well
  OV_CHECK_NOT_READ
};

// ---------------------------------------------------------------------------------------------
// Voltage scale check
//
// The SunSaver specification gives n*100/32768 for voltages; the TankAlarm client has always
// used n*96.667/32768. Which one a controller really uses is tested, not assumed. It is tested
// twice, because a controller could use one figure for what it measures and the other for what
// it is told, and the profile gives the two their own scales:
//
//   Stored settings
//     1. Round setpoints. People and factories pick round numbers (14.00, 13.70, 11.50), and
//        decoded with the scale they were written with the setpoints land on a 0.05 V grid. This
//        test only points, and decides nothing: settings that OptaView, or any tool with a
//        wrong scale, has written look round at that wrong scale, so the test would confirm the
//        very scale that wrote them.
//     2. A setting whose value the user knows from elsewhere (MSView shows it, say), typed in.
//        It decides.
//
//   Live readings
//     3. Regulation. In absorption, float or equalize the controller works to hold the battery at
//        its regulation target, and the two registers agree only if the battery's scale is right
//        relative to the target's. This test only points, and decides nothing: the battery need
//        not be at the target in those states (with too little sun for the load it stays below
//        it, and float goes on for a while below it, see Et_float_exit_cum), and a battery 3.3 %
//        below its target reads as on target at the other scale. When it points away from the
//        scale in use, the target's own scale may also be the one that is wrong.
//     4. A multimeter reading typed in by the user. It decides.
//
// Neither verdict is borrowed for the other scale: a multimeter says nothing about how settings
// are stored, and a setting says nothing about the readings.
// ---------------------------------------------------------------------------------------------

static const uint8_t OV_SCALE_CANDIDATES = 6;

// How many registers OvDevice remembers having written. Beyond that it takes every register to
// have been written.
static const uint8_t OV_WRITTEN_MAX = 48;

// The numerators one scale might have. [0] is the one in use now.
struct OvScaleCandidates {
  uint8_t count;
  double num[OV_SCALE_CANDIDATES];
};

struct OvSettingsScaleReport {
  bool tested;  // the profile sets this half up
  OvScaleCandidates candidates;

  uint8_t setpointsRead;  // answered
  uint8_t setpointsUsed;  // answered and holding a setting (not 0, not a blank cell)
  uint8_t fits[OV_SCALE_CANDIDATES];
  int8_t roundVerdict;  // the candidate the round setpoints point at, or -1

  const OvRegister *known;  // the setting the user knows, or nullptr when none was given
  double knownGivenVolts;   // what the user says it holds
  bool knownRead;           // the register answered
  bool knownPlausible;      // and held a setting: not 0, and inside its limits at a candidate
  bool knownCompared;       // so the comparison was made
  uint16_t knownRaw;
  double knownVolts[OV_SCALE_CANDIDATES];
  double impliedNumerator;
  uint8_t knownMatches;  // candidates that decode the register to the user's value
  int8_t knownVerdict;   // the candidate, when exactly one does

  int8_t verdict;  // candidate index, or -1 when the tests cannot tell
  bool conflict;   // the setting and the round setpoints point at different candidates
};

struct OvLiveScaleReport {
  bool tested;  // the profile sets this half up
  OvScaleCandidates candidates;

  bool regulationRead;  // the state, battery and target registers all answered
  bool regulating;
  uint16_t stateRaw;
  uint16_t batteryRaw;
  uint16_t referenceRaw;
  double referenceVolts;
  double batteryVolts[OV_SCALE_CANDIDATES];
  uint8_t regulationMatches;  // candidates that put the battery at the target
  int8_t regulationVerdict;   // the candidate, when exactly one does

  bool meterGiven;     // the user typed a multimeter reading
  bool meterRead;      // the register to compare it with answered
  bool meterCompared;  // and held a voltage, so the comparison was made
  uint16_t meterRaw;
  double meterGivenVolts;
  double meterVolts[OV_SCALE_CANDIDATES];
  double impliedNumerator;
  uint8_t meterMatches;  // candidates that put the register at the multimeter's value
  int8_t meterVerdict;   // the candidate, when exactly one does

  int8_t verdict;  // candidate index, or -1 when the tests cannot tell
  bool conflict;   // the multimeter and the regulation test point at different candidates
};

struct OvScaleReport {
  OvSettingsScaleReport settings;
  OvLiveScaleReport live;
};

// Distance from value to the nearest multiple of grid.
inline double ovGridDistance(double value, double grid) {
  const double nearest = floor(value / grid + 0.5) * grid;
  return value > nearest ? value - nearest : nearest - value;
}

// Adds a candidate numerator unless it is already listed.
inline void ovAddCandidate(OvScaleCandidates *list, double num) {
  if (!(num > 0.0)) {
    return;
  }
  for (uint8_t c = 0; c < list->count; ++c) {
    const double gap = list->num[c] - num;
    if (gap < 0.001 && gap > -0.001) {
      return;
    }
  }
  if (list->count < OV_SCALE_CANDIDATES) {
    list->num[list->count++] = num;
  }
}

// Test 1. raws = the setpoint registers that answered and hold a setting.
inline void ovJudgeSetpoints(OvSettingsScaleReport *report, const uint16_t *raws, uint8_t count, double den,
                             double grid) {
  report->setpointsUsed = count;
  report->roundVerdict = -1;
  for (uint8_t c = 0; c < report->candidates.count; ++c) {
    const double lsb = report->candidates.num[c] / den;
    uint8_t fits = 0;
    for (uint8_t i = 0; i < count; ++i) {
      if (ovGridDistance((double)raws[i] * lsb, grid) <= lsb) {
        ++fits;
      }
    }
    report->fits[c] = fits;
  }
  if (count < 4) {
    return;  // too few setpoints to tell a pattern from luck
  }
  uint8_t best = 0;
  for (uint8_t c = 1; c < report->candidates.count; ++c) {
    if (report->fits[c] > report->fits[best]) {
      best = c;
    }
  }
  uint8_t runnerUp = 0;
  for (uint8_t c = 0; c < report->candidates.count; ++c) {
    if (c != best && report->fits[c] > runnerUp) {
      runnerUp = report->fits[c];
    }
  }
  // Most setpoints must fit, and at least twice as many as under any other candidate.
  if ((unsigned)report->fits[best] * 10u >= (unsigned)count * 6u &&
      (unsigned)runnerUp * 2u <= (unsigned)report->fits[best]) {
    report->roundVerdict = (int8_t)best;
  }
}

// Test 2. A candidate matches when it decodes the register to within 0.5 % of the value the user
// gave. A setting is stored to within a step of 0.003 V, so the margin is for a value that was
// read off a screen with fewer decimals; the usual candidates lie 3.4 % apart.
inline void ovJudgeKnown(OvSettingsScaleReport *report, double den) {
  report->knownVerdict = -1;
  report->knownMatches = 0;
  report->knownCompared = false;
  report->impliedNumerator = 0.0;
  if (report->knownRaw == 0 || !(report->knownGivenVolts > 0.0)) {
    return;
  }
  report->knownCompared = true;
  report->impliedNumerator = report->knownGivenVolts * den / (double)report->knownRaw;
  int8_t match = -1;
  uint8_t matches = 0;
  for (uint8_t c = 0; c < report->candidates.count; ++c) {
    report->knownVolts[c] = (double)report->knownRaw * report->candidates.num[c] / den;
    const double gap = report->knownVolts[c] - report->knownGivenVolts;
    const double allowed = report->knownGivenVolts * 0.005;
    if (gap <= allowed && gap >= -allowed) {
      match = (int8_t)c;
      ++matches;
    }
  }
  report->knownMatches = matches;
  if (matches == 1) {
    report->knownVerdict = match;
  }
}

// The verdict for stored settings: a setting the user knows decides, also when it fits no
// candidate (then the scale stays open). Without one there is no verdict, however round the
// setpoints are.
inline void ovJudgeSettings(OvSettingsScaleReport *report) {
  report->verdict = -1;
  report->conflict = false;
  if (report->knownCompared) {
    report->verdict = report->knownVerdict;
    report->conflict = report->knownVerdict >= 0 && report->roundVerdict >= 0 &&
                       report->roundVerdict != report->knownVerdict;
  }
}

// Test 3. Call only while the controller is regulating.
inline void ovJudgeRegulation(OvLiveScaleReport *report, double den) {
  report->regulationVerdict = -1;
  report->regulationMatches = 0;
  int8_t match = -1;
  uint8_t matches = 0;
  for (uint8_t c = 0; c < report->candidates.count; ++c) {
    report->batteryVolts[c] = (double)report->batteryRaw * report->candidates.num[c] / den;
    const double gap = report->batteryVolts[c] - report->referenceVolts;
    if (gap <= 0.15 && gap >= -0.15) {
      match = (int8_t)c;
      ++matches;
    }
  }
  report->regulationMatches = matches;
  if (matches == 1) {
    report->regulationVerdict = match;
  }
}

// Test 4. A candidate matches when it puts the register within 1.2 % of the multimeter.
inline void ovJudgeMeter(OvLiveScaleReport *report, double den) {
  report->meterVerdict = -1;
  report->meterMatches = 0;
  report->meterCompared = false;
  report->impliedNumerator = 0.0;
  if (report->meterRaw == 0 || !(report->meterGivenVolts > 0.0)) {
    return;
  }
  report->meterCompared = true;
  report->impliedNumerator = report->meterGivenVolts * den / (double)report->meterRaw;
  int8_t match = -1;
  uint8_t matches = 0;
  for (uint8_t c = 0; c < report->candidates.count; ++c) {
    report->meterVolts[c] = (double)report->meterRaw * report->candidates.num[c] / den;
    const double gap = report->meterVolts[c] - report->meterGivenVolts;
    const double allowed = report->meterGivenVolts * 0.012;
    if (gap <= allowed && gap >= -allowed) {
      match = (int8_t)c;
      ++matches;
    }
  }
  report->meterMatches = matches;
  if (matches == 1) {
    report->meterVerdict = match;
  }
}

// The verdict for live readings: a multimeter comparison decides, also when it fits no candidate
// (then the scale stays open). Without one there is no verdict, whatever the regulation test says.
inline void ovJudgeLive(OvLiveScaleReport *report) {
  report->verdict = -1;
  report->conflict = false;
  if (report->meterCompared) {
    report->verdict = report->meterVerdict;
    report->conflict = report->meterVerdict >= 0 && report->regulationVerdict >= 0 &&
                       report->regulationVerdict != report->meterVerdict;
  }
}

// ---------------------------------------------------------------------------------------------
// The device
// ---------------------------------------------------------------------------------------------

class OvDevice {
 public:
  OvDevice(ModbusMaster &master, OvProfile &profile)
      : blockMode(OV_BLOCKS_AUTO),
        retries(1),
        master_(master),
        profile_(profile),
        blocksFailed_(false),
        linkDown_(false),
        consecutiveFailures_(0),
        settingsFound_(0.0),
        liveFound_(0.0),
        settingsEpoch_(0),
        liveEpoch_(0),
        generation_(0),
        resetNeeded_(false),
        writeUnsettled_(false),
        unknownOpen_(false),
        unknownAddr_(0),
        unknownBefore_(0),
        unknownUsed_(0xFFFF),
        writtenCount_(0),
        writtenFull_(false) {
    memset(written_, 0, sizeof(written_));
    link.slave = 1;
    link.baud = 9600;
    link.format = MB_8N2;
    link.readFunction = MB_FC_READ_INPUT;
    settled_ = link;
  }

  OvLink link;
  uint8_t blockMode;  // OvBlockMode
  uint8_t retries;    // extra attempts for a read after a link failure

  ModbusMaster &master() { return master_; }
  OvProfile &profile() { return profile_; }

  // Takes the link settings from the profile. Call applyLink() afterwards.
  void useProfileDefaults() {
    const OvModbusDefaults &d = profile_.modbus();
    link.slave = d.slaveId;
    link.baud = d.baud;
    link.format = d.format;
    link.readFunction = d.readFunction;
    master_.responseTimeoutMs = d.responseTimeoutMs;
    blockMode = OV_BLOCKS_AUTO;
    blocksFailed_ = false;
    settleLink();
  }

  // (Re)opens the serial port with link.baud and link.format, and gives the master the pauses
  // that go with that baud rate.
  bool applyLink() { return master_.configure(link.baud, link.format); }

  uint16_t maxBlock() const {
    const uint8_t n = profile_.modbus().maxBlock;
    return n < 1 ? 1 : n;
  }
  // True once automatic mode has given up on block reads.
  bool blocksFailed() const { return blocksFailed_; }
  bool blocksInUse() const {
    return blockMode == OV_BLOCKS_ON || (blockMode == OV_BLOCKS_AUTO && !blocksFailed_);
  }
  void setBlockMode(uint8_t mode) {
    blockMode = mode;
    blocksFailed_ = false;
  }

  // Call before each user command. After OV_LINK_DOWN_AFTER failed requests in a row the rest
  // of a batch is not attempted (a 94-register dump would otherwise wait out 94 timeouts).
  void beginBatch() {
    linkDown_ = false;
    consecutiveFailures_ = 0;
  }
  bool linkDown() const { return linkDown_; }

  // -------------------------------------------------------------------------------------------
  // What the front ends share
  // -------------------------------------------------------------------------------------------

  // The numerator that 'scalecheck' found for a voltage scale on the controller at the other end
  // of the link, or 0 when nothing has been found (set 0 to take a finding back).
  double settingsScaleFound() const { return settingsFound_; }
  double liveScaleFound() const { return liveFound_; }
  void setSettingsScaleFound(double numerator) {
    settingsFound_ = numerator;
    ++settingsEpoch_;
  }
  void setLiveScaleFound(double numerator) {
    liveFound_ = numerator;
    ++liveEpoch_;
  }

  // A scale is confirmed while it has the numerator that was found on this controller. Changing
  // it by hand to anything else makes it unconfirmed again. "Open": the profile wants the scale
  // tested, and it is not confirmed.
  bool settingsScaleConfirmed() const {
    return confirmed(profile_.scaleCheck().settingsScale, settingsFound_);
  }
  bool liveScaleConfirmed() const { return confirmed(profile_.scaleCheck().liveScale, liveFound_); }
  bool settingsScaleOpen() const {
    return profile_.scaleCheck().settingsScale != nullptr && !settingsScaleConfirmed();
  }
  bool liveScaleOpen() const { return profile_.scaleCheck().liveScale != nullptr && !liveScaleConfirmed(); }

  // These count up whenever somebody who has been told "this scale is not confirmed" has to be
  // told again: the link may lead to another controller, a check found something, or the scale
  // was changed by hand. A front end remembers the count at which it gave its reminder.
  uint32_t settingsScaleEpoch() const { return settingsEpoch_; }
  uint32_t liveScaleEpoch() const { return liveEpoch_; }

  // A scale's numerator was changed by hand ('scale <name> <x>'). A value that was shown with
  // the old figure no longer means the same raw word.
  void scaleChanged(const OvScale *scale) {
    const OvScaleCheck &check = profile_.scaleCheck();
    if (scale == check.settingsScale) {
      ++settingsEpoch_;
    }
    if (scale == check.liveScale) {
      ++liveEpoch_;
    }
    ++generation_;
  }

  // A register that OptaView has sent a write to since it started, whatever the outcome and the
  // link. What it holds now came from OptaView, with the scale that was in use, so its value
  // cannot tell whether that scale is right.
  bool wroteRegister(uint16_t addr) const {
    if (writtenFull_) {
      return true;
    }
    for (uint8_t i = 0; i < writtenCount_; ++i) {
      if (written_[i] == addr) {
        return true;
      }
    }
    return false;
  }
  // For a register write that did not pass through writeRegister() ('raw').
  void noteRegisterWritten(uint16_t addr) {
    if (wroteRegister(addr)) {
      return;
    }
    if (writtenCount_ < OV_WRITTEN_MAX) {
      written_[writtenCount_++] = addr;
    } else {
      writtenFull_ = true;
    }
  }

  // Settings were written and the controller has not restarted since: it does not use them yet.
  bool resetNeeded() const { return resetNeeded_; }
  void noteSettingsWritten() { resetNeeded_ = true; }

  // A write went out and nothing has shown whether the controller took it: its acknowledgement
  // did not come, and the register could not be read afterwards. If it was taken, a restart is
  // due as well. The next read of that register settles it: it either still holds what it held
  // before the write, or settings were written.
  bool resetMaybeNeeded() const { return !resetNeeded_ && (unknownOpen_ || writeUnsettled_); }
  // For a write of unknown outcome that did not pass through writeRegister(). No read settles it.
  void noteWriteUnknown() { writeUnsettled_ = true; }

  // Counts everything that can make a question that is waiting for "yes" out of date: a write,
  // a restart, other link settings, another scale. A front end notes the count when it asks and
  // sends nothing if the count has moved by the time the answer comes, because the other front
  // end has changed what the question was about.
  uint32_t generation() const { return generation_; }
  // For a request that went to the controller without passing through this class.
  void noteControllerChanged() { ++generation_; }

  // Call after every command. 'scalecheck' judged the controller that was at the other end of
  // the link at the time; another slave address or other serial settings may reach another
  // controller, so what was found there does not carry over. Returns true when something that
  // had been found was dropped.
  bool settleLink() {
    if (link.slave == settled_.slave && link.baud == settled_.baud && link.format == settled_.format) {
      settled_ = link;
      return false;
    }
    settled_ = link;
    if (unknownOpen_) {
      // What that register reads on another controller says nothing about the write.
      unknownOpen_ = false;
      writeUnsettled_ = true;
    }
    const bool had = settingsFound_ > 0.0 || liveFound_ > 0.0;
    settingsFound_ = 0.0;
    liveFound_ = 0.0;
    ++settingsEpoch_;
    ++liveEpoch_;
    ++generation_;
    return had;
  }

  // One read request for `count` words, repeated after a link failure. The master lets one more
  // timeout pass before the repeat, so that a reply that is merely late is not taken for its
  // answer; every attempt therefore counts towards giving up on the link.
  MbResult readWords(uint16_t addr, uint16_t count, uint16_t *out) {
    MbResult r = readOnce(addr, count, out);
    for (uint8_t attempt = 0; attempt < retries && mbIsLinkFailure(r.status) && !linkDown_; ++attempt) {
      r = readOnce(addr, count, out);
    }
    return r;
  }

  // Reads `count` consecutive words into words[] and tells how each one went in status[].
  void readSpan(uint16_t addr, uint16_t count, uint16_t *words, OvWordStatus *status) {
    uint16_t done = 0;
    while (done < count) {
      if (linkDown_) {
        for (; done < count; ++done) {
          words[done] = 0;
          status[done].status = OV_NOT_READ;
          status[done].exception = 0;
        }
        return;
      }
      const uint16_t at = (uint16_t)(addr + done);
      uint16_t n = 1;
      if (blocksInUse()) {
        n = (uint16_t)(count - done);
        if (n > maxBlock()) {
          n = maxBlock();
        }
      }
      if (n == 1) {
        readOne(at, words + done, status + done);
        ++done;
        continue;
      }
      MbResult r;
      if (blockMode != OV_BLOCKS_AUTO) {
        r = readWords(at, n, words + done);  // forced on: repeated like any other read
      } else {
        // In automatic mode a block that gets no usable reply is not simply asked for again.
        // First its first register is asked alone, to see whether the controller answers at
        // all. Only if it does is the block tried once more, and a block that is lost a second
        // time means that blocks are the problem. Repeating the block first would use up two of
        // the OV_LINK_DOWN_AFTER failures: one lost reply to the single read after them would
        // then end the batch on a controller that is there.
        r = readOnce(at, n, words + done);
        if (mbIsLinkFailure(r.status)) {
          readOne(at, words + done, status + done);
          const bool answered = status[done].status == MB_OK || status[done].status == MB_EXCEPTION;
          if (answered && retries > 0) {
            r = readOnce(at, n, words + done);  // a failed read leaves the word read alone as it is
          }
          if (!answered || mbIsLinkFailure(r.status)) {
            if (answered) {
              blocksFailed_ = true;
            }
            ++done;  // this word has its status from being read alone
            continue;
          }
          // The second try of the block was answered, with data or with an exception: carry on.
        }
      }
      if (r.status == MB_OK) {
        for (uint16_t i = 0; i < n; ++i) {
          status[done + i].status = MB_OK;
          status[done + i].exception = 0;
        }
        done = (uint16_t)(done + n);
        continue;
      }
      if (r.status == MB_EXCEPTION) {
        // Refused. Ask for each register alone to find the one the controller objects to.
        bool allRead = true;
        for (uint16_t i = 0; i < n; ++i) {
          if (!readOne((uint16_t)(at + i), words + done + i, status + done + i)) {
            allRead = false;
          }
        }
        if (allRead && blockMode == OV_BLOCKS_AUTO) {
          blocksFailed_ = true;  // nothing wrong with the registers: it is blocks it refuses
        }
        done = (uint16_t)(done + n);
        continue;
      }
      // No usable reply, and blocks are forced on: the whole block is lost.
      for (uint16_t i = 0; i < n; ++i) {
        words[done + i] = 0;
        status[done + i].status = r.status;
        status[done + i].exception = r.exception;
      }
      done = (uint16_t)(done + n);
    }
  }

  // The status of a register: that of its first word that was not read.
  static OvWordStatus combine(const OvWordStatus *status, uint8_t words) {
    for (uint8_t i = 0; i < words; ++i) {
      if (status[i].status != MB_OK) {
        return status[i];
      }
    }
    OvWordStatus ok;
    ok.status = MB_OK;
    ok.exception = 0;
    return ok;
  }

  // Reads one register (both words of a two-word one). True when it was read. `words` must
  // have room for two; the second is 0 for a one-word register.
  bool readRegister(const OvRegister &reg, uint16_t *words, OvWordStatus *status) {
    OvWordStatus each[2];
    uint16_t buffer[2] = {0, 0};
    const uint8_t n = reg.words > 2 ? 2 : reg.words;
    readSpan(reg.addr, n, buffer, each);
    words[0] = buffer[0];
    words[1] = buffer[1];
    *status = combine(each, n);
    return status->status == MB_OK;
  }

  // Finds the next run of registers at consecutive addresses, searching from *cursor (an index
  // into the profile's registers, 0 to start). `group` limits the search to one group; nullptr
  // takes every register. A run is at most maxBlock() words and never splits a register.
  bool nextSpan(const char *group, size_t *cursor, OvSpan *span) const {
    const size_t total = profile_.registerCount();
    size_t i = *cursor;
    while (i < total && !inGroup(profile_.reg(i), group)) {
      ++i;
    }
    if (i >= total) {
      *cursor = total;
      return false;
    }
    const uint16_t limit = maxBlock();
    span->first = i;
    span->count = 1;
    span->addr = profile_.reg(i).addr;
    span->words = profile_.reg(i).words;
    size_t j = i + 1;
    while (j < total) {
      const OvRegister &next = profile_.reg(j);
      if (!inGroup(next, group) || (uint32_t)next.addr != (uint32_t)span->addr + span->words ||
          (uint32_t)span->words + next.words > limit) {
        break;
      }
      span->words = (uint16_t)(span->words + next.words);
      ++span->count;
      ++j;
    }
    *cursor = j;
    return true;
  }

  // Tries link settings until the profile's probe register is read: the settings in use first,
  // then the profile's, then the usual alternatives. An exception reply proves that a slave is
  // listening but is not a find. On success the settings found stay in use.
  OvProbeOutcome probe(OvProbeListener *listener) {
    OvProbeOutcome out;
    memset(&out, 0, sizeof(out));
    const OvModbusDefaults &defaults = profile_.modbus();
    if (defaults.probeRegister == nullptr) {
      return out;
    }
    const OvLink original = link;
    const uint16_t originalTimeout = master_.responseTimeoutMs;

    uint32_t bauds[4];
    size_t baudCount = 0;
    addBaud(bauds, &baudCount, link.baud);
    addBaud(bauds, &baudCount, defaults.baud);
    addBaud(bauds, &baudCount, 9600);
    addBaud(bauds, &baudCount, 19200);

    uint8_t formats[5];
    size_t formatCount = 0;
    addByte(formats, &formatCount, link.format);
    addByte(formats, &formatCount, defaults.format);
    addByte(formats, &formatCount, MB_8N2);
    addByte(formats, &formatCount, MB_8N1);
    addByte(formats, &formatCount, MB_8E1);

    uint8_t slaves[3];
    size_t slaveCount = 0;
    addByte(slaves, &slaveCount, link.slave);
    addByte(slaves, &slaveCount, defaults.slaveId);
    addByte(slaves, &slaveCount, 1);

    const uint8_t functions[2] = {
        link.readFunction,
        (uint8_t)(link.readFunction == MB_FC_READ_INPUT ? MB_FC_READ_HOLDING : MB_FC_READ_INPUT)};

    bool done = false;
    for (size_t b = 0; b < baudCount && !done; ++b) {
      for (size_t f = 0; f < formatCount && !done; ++f) {
        if (!master_.configure(bauds[b], formats[f])) {
          continue;
        }
        for (size_t s = 0; s < slaveCount && !done; ++s) {
          uint8_t refusals = 0;
          for (size_t k = 0; k < 2 && !done; ++k) {
            if (listener != nullptr && listener->probeAborted()) {
              out.aborted = true;
              done = true;
              break;
            }
            // The first attempt gets the full timeout; the sweep after it a shorter one.
            master_.responseTimeoutMs =
                (out.attempts == 0 || originalTimeout < OV_PROBE_TIMEOUT_MS) ? originalTimeout : OV_PROBE_TIMEOUT_MS;
            OvLink tried;
            tried.slave = slaves[s];
            tried.baud = bauds[b];
            tried.format = formats[f];
            tried.readFunction = functions[k];
            uint16_t raw = 0;
            const MbResult r =
                master_.readRegisters(tried.slave, tried.readFunction, defaults.probeRegister->addr, 1, &raw);
            ++out.attempts;
            if (listener != nullptr) {
              listener->onProbeAttempt(tried, r);
            }
            if (r.status == MB_OK) {
              out.found = true;
              out.link = tried;
              out.raw = raw;
              done = true;
            } else if (r.status == MB_EXCEPTION) {
              ++refusals;
              if (!out.refused) {
                out.refused = true;
                out.link = tried;
                out.exception = r.exception;
              }
            } else if (r.status != MB_TIMEOUT) {
              out.sawBytes = true;
            }
          }
          if (refusals == 2) {
            done = true;  // a slave is here and refuses both read functions: no point sweeping on
          }
        }
      }
    }

    master_.responseTimeoutMs = originalTimeout;
    link = original;
    if (out.found) {
      link = out.link;
      blocksFailed_ = false;
    } else if (out.refused) {
      // The serial settings and the address are proven; only the read is refused.
      link.slave = out.link.slave;
      link.baud = out.link.baud;
      link.format = out.link.format;
    }
    applyLink();
    return out;
  }

  // Writes one register with function 06 and reads it back. `before` is what the register held
  // when it was last read: it tells a write that was not taken from one that was taken with
  // another value, when the acknowledgement does not arrive.
  OvWriteOutcome writeRegister(const OvRegister &reg, uint16_t raw, uint16_t before) {
    OvWriteOutcome out;
    memset(&out, 0, sizeof(out));
    out.write = master_.writeRegister(link.slave, reg.addr, raw);
    ++generation_;
    noteRegisterWritten(reg.addr);
    beginBatch();
    uint16_t stored = 0;
    readOne(reg.addr, &stored, &out.readBack);
    out.readBackRaw = stored;
    const bool read = out.readBack.status == MB_OK;
    out.verified = read && ovSameValue(reg, stored, raw);
    if (out.verified || out.write.status == MB_OK || (read && !ovSameValue(reg, stored, before))) {
      out.effect = OV_WRITE_TAKEN;
      resetNeeded_ = true;
    } else if (read || !mbIsLinkFailure(out.write.status)) {
      out.effect = OV_WRITE_NOT_TAKEN;  // it reads as before, or the controller said no
    } else {
      out.effect = OV_WRITE_UNKNOWN;
      if (!unknownOpen_ || unknownAddr_ != reg.addr) {
        if (unknownOpen_) {
          writeUnsettled_ = true;  // one register is remembered; the earlier one stays open for good
        }
        unknownOpen_ = true;
        unknownAddr_ = reg.addr;
        unknownBefore_ = before;
        unknownUsed_ = ovUsedBits(reg);
      }
      // Otherwise this is a second write in doubt to the same register. What the register held
      // before the first of them stays the value to compare with: if the first was taken, the
      // register differs from it, whatever the caller took it to hold before the second.
    }
    return out;
  }

  MbResult readCoil(const OvCoil &coil, bool *on) {
    uint8_t value = 0;
    MbResult r = master_.readBits(link.slave, MB_FC_READ_COILS, coil.addr, 1, &value);
    for (uint8_t attempt = 0; attempt < retries && mbIsLinkFailure(r.status); ++attempt) {
      r = master_.readBits(link.slave, MB_FC_READ_COILS, coil.addr, 1, &value);
    }
    *on = value != 0;
    return r;
  }

  MbResult writeCoil(const OvCoil &coil, bool on) {
    ++generation_;
    return master_.writeCoil(link.slave, coil.addr, on);
  }

  // Restarts the controller through the profile's reset coil and waits for it to come back. A
  // restart shows as a silence: asked all along, the controller misses a request and then answers
  // again. Only then do the settings written count as in use. A controller that answers every
  // request for the profile's resetWaitMs was not seen to restart (the command may not have
  // reached it, or it may not act on it), so whatever said that a restart is due still says so.
  OvResetOutcome resetController(OvAbortCheck *abort) {
    OvResetOutcome out;
    memset(&out, 0, sizeof(out));
    const OvModbusDefaults &defaults = profile_.modbus();
    if (defaults.resetCoil == nullptr || defaults.probeRegister == nullptr) {
      return out;
    }
    MbTransport &transport = master_.transport();
    out.command = master_.writeCoil(link.slave, defaults.resetCoil->addr, true);
    ++generation_;
    out.sent = out.command.status != MB_BAD_REQUEST;
    if (out.command.status == MB_EXCEPTION || out.command.status == MB_BAD_REQUEST) {
      return out;  // refused: it did not restart
    }
    const uint32_t start = transport.nowMs();
    uint32_t askedAt = start;
    bool asked = false;
    for (;;) {
      if (abort != nullptr && abort->aborted()) {
        out.aborted = true;
        break;
      }
      if (asked && (uint32_t)(transport.nowMs() - askedAt) < OV_RESET_POLL_MS) {
        transport.idle();  // a controller that answers at once is not asked without a pause
        continue;
      }
      asked = true;
      askedAt = transport.nowMs();
      uint16_t raw = 0;
      const MbResult r = master_.readRegisters(link.slave, link.readFunction, defaults.probeRegister->addr, 1, &raw);
      const uint32_t elapsed = (uint32_t)(transport.nowMs() - start);
      if (r.status != MB_OK && r.status != MB_EXCEPTION) {
        out.wentAway = true;
        if (elapsed >= OV_RESET_GIVE_UP_MS) {
          break;  // it has not come back
        }
      } else if (out.wentAway) {
        out.cameBack = true;
        resetNeeded_ = false;  // whatever was written, the controller uses it now
        writeUnsettled_ = false;
        unknownOpen_ = false;
        break;
      } else if (elapsed >= defaults.resetWaitMs) {
        out.neverAway = true;
        break;
      }
    }
    out.backAfterMs = (uint32_t)(transport.nowMs() - start);
    beginBatch();
    return out;
  }

  // Evaluates one pair check. `pending` and `pendingRaw` stand in for a value that is about to
  // be written (nullptr = judge what the controller holds now).
  OvCheckResult runCheck(const OvCheck &check, const OvRegister *pending, uint16_t pendingRaw, double *below,
                         double *above) {
    uint16_t belowWords[2] = {0, 0};
    uint16_t aboveWords[2] = {0, 0};
    OvWordStatus status;
    if (check.below == pending) {
      belowWords[0] = pendingRaw;
    } else if (!readRegister(*check.below, belowWords, &status)) {
      return OV_CHECK_NOT_READ;
    }
    if (check.above == pending) {
      aboveWords[0] = pendingRaw;
    } else if (!readRegister(*check.above, aboveWords, &status)) {
      return OV_CHECK_NOT_READ;
    }
    *below = ovValue(*check.below, belowWords);
    *above = ovValue(*check.above, aboveWords);
    const bool belowZero = ovRawValue(*check.below, belowWords) == 0;
    const bool aboveZero = ovRawValue(*check.above, aboveWords) == 0;
    // A 0 switches the check off only in a register whose 0 switches the setting off: with the
    // high voltage disconnect at 0 there is nothing for its reconnect to stay below. Anywhere
    // else a 0 is a voltage the battery never reaches: a reconnect at 0 V never reconnects, and
    // a disconnect at 0 V never disconnects.
    if ((belowZero && check.below->zeroOk) || (aboveZero && check.above->zeroOk)) {
      return OV_CHECK_OFF;
    }
    if (belowZero || aboveZero) {
      return OV_CHECK_ZERO;
    }
    return *below < *above ? OV_CHECK_PASS : OV_CHECK_FAIL;
  }

  // Reads what the scale check needs and judges it. meterVolts <= 0 means no multimeter value.
  // known is a setting whose value the user knows to be knownVolts, or nullptr; it must be a
  // one-word register on the scale of stored settings (the console checks that). Returns false
  // when the profile has no scale check.
  bool scaleCheck(double meterVolts, const OvRegister *known, double knownVolts, OvScaleReport *report) {
    memset(report, 0, sizeof(*report));
    report->settings.roundVerdict = -1;
    report->settings.knownVerdict = -1;
    report->settings.verdict = -1;
    report->live.regulationVerdict = -1;
    report->live.meterVerdict = -1;
    report->live.verdict = -1;
    const OvScaleCheck &check = profile_.scaleCheck();
    if (!check.present) {
      return false;
    }
    if (check.settingsScale != nullptr) {
      checkSettingsScale(check, known, knownVolts, &report->settings);
    }
    if (check.liveScale != nullptr) {
      checkLiveScale(check, meterVolts, &report->live);
    }
    return true;
  }

 private:
  ModbusMaster &master_;
  OvProfile &profile_;
  bool blocksFailed_;
  bool linkDown_;
  uint8_t consecutiveFailures_;
  OvLink settled_;          // the link settings that the findings below belong to
  double settingsFound_;
  double liveFound_;
  uint32_t settingsEpoch_;
  uint32_t liveEpoch_;
  uint32_t generation_;
  bool resetNeeded_;
  bool writeUnsettled_;     // a write of unknown outcome that no read can settle any more
  bool unknownOpen_;        // a write of unknown outcome that the next read of its register settles
  uint16_t unknownAddr_;    // that register
  uint16_t unknownBefore_;  // what it held before the write
  uint16_t unknownUsed_;    // the bits of it that the controller uses (ovUsedBits)
  uint16_t written_[OV_WRITTEN_MAX];  // the registers written since OptaView started
  uint8_t writtenCount_;
  bool writtenFull_;        // more than that: every register counts as written

  static bool confirmed(const OvScale *scale, double found) {
    if (scale == nullptr || !(found > 0.0)) {
      return false;
    }
    const double gap = scale->num - found;
    return gap < 0.001 && gap > -0.001;
  }

  // What a scale may really be: the numerator in use now ('scale' may have changed it), the one
  // in the profile file, and the profile's alternates.
  static void listCandidates(const OvScaleCheck &check, const OvScale &scale, double fileNum,
                             OvScaleCandidates *list) {
    ovAddCandidate(list, scale.num);
    ovAddCandidate(list, fileNum);
    for (uint8_t i = 0; i < check.alternateCount; ++i) {
      ovAddCandidate(list, check.alternates[i]);
    }
  }

  // A blank or damaged EEPROM cell (0xFFFF, say) is not a setting that somebody chose. A setpoint
  // counts when at least one candidate puts it inside the limits the profile gives its register.
  static bool plausibleSetpoint(const OvRegister &reg, uint16_t raw, const OvScaleCandidates &candidates,
                                double den) {
    if (!reg.hasMin && !reg.hasMax) {
      return true;
    }
    for (uint8_t c = 0; c < candidates.count; ++c) {
      const double value = (double)raw * candidates.num[c] / den;
      if ((!reg.hasMin || value >= reg.minValue) && (!reg.hasMax || value <= reg.maxValue)) {
        return true;
      }
    }
    return false;
  }

  // Tests 1 and 2: how round the stored setpoints are, and a setting the user knows.
  void checkSettingsScale(const OvScaleCheck &check, const OvRegister *known, double knownVolts,
                          OvSettingsScaleReport *report) {
    report->tested = true;
    const double den = check.settingsScale->den;
    listCandidates(check, *check.settingsScale, check.settingsFileNum, &report->candidates);

    uint16_t raws[sizeof(check.setpoints) / sizeof(check.setpoints[0])];
    uint8_t used = 0;
    for (uint8_t i = 0; i < check.setpointCount; ++i) {
      uint16_t words[2] = {0, 0};
      OvWordStatus status;
      if (!readRegister(*check.setpoints[i], words, &status)) {
        continue;
      }
      ++report->setpointsRead;
      if (words[0] != 0 && plausibleSetpoint(*check.setpoints[i], words[0], report->candidates, den)) {
        raws[used++] = words[0];
      }
    }
    ovJudgeSetpoints(report, raws, used, den, check.grid);

    if (known != nullptr && knownVolts > 0.0 && known->scale == check.settingsScale && known->words == 1) {
      uint16_t words[2] = {0, 0};
      OvWordStatus status;
      report->known = known;
      report->knownGivenVolts = knownVolts;
      if (readRegister(*known, words, &status)) {
        report->knownRead = true;
        report->knownRaw = words[0];
        // A blank or damaged cell is no setting, whatever the user believes it holds.
        report->knownPlausible = words[0] != 0 && plausibleSetpoint(*known, words[0], report->candidates, den);
        if (report->knownPlausible) {
          ovJudgeKnown(report, den);
        }
      }
    }

    ovJudgeSettings(report);
  }

  // Tests 3 and 4: the battery against the regulation target, and against a multimeter.
  void checkLiveScale(const OvScaleCheck &check, double meterVolts, OvLiveScaleReport *report) {
    report->tested = true;
    const double den = check.liveScale->den;
    listCandidates(check, *check.liveScale, check.liveFileNum, &report->candidates);
    OvWordStatus status;

    if (check.battery != nullptr && check.reference != nullptr && check.state != nullptr) {
      uint16_t state[2] = {0, 0};
      uint16_t battery[2] = {0, 0};
      uint16_t reference[2] = {0, 0};
      if (readRegister(*check.state, state, &status) && readRegister(*check.battery, battery, &status) &&
          readRegister(*check.reference, reference, &status)) {
        report->regulationRead = true;
        report->stateRaw = state[0];
        report->batteryRaw = battery[0];
        report->referenceRaw = reference[0];
        report->referenceVolts = ovValue(*check.reference, reference);
        for (uint8_t i = 0; i < check.regulatingCount; ++i) {
          if (check.regulating[i] == state[0]) {
            report->regulating = true;
          }
        }
        if (report->regulating && battery[0] != 0 && reference[0] != 0) {
          ovJudgeRegulation(report, den);
        }
      }
    }

    if (meterVolts > 0.0 && check.meter != nullptr) {
      uint16_t words[2] = {0, 0};
      report->meterGiven = true;
      report->meterGivenVolts = meterVolts;
      if (readRegister(*check.meter, words, &status)) {
        report->meterRead = true;
        report->meterRaw = words[0];
        ovJudgeMeter(report, den);
      }
    }

    ovJudgeLive(report);
  }

  static bool inGroup(const OvRegister &reg, const char *group) {
    return group == nullptr || reg.group == group || strcmp(reg.group, group) == 0;
  }

  static void addBaud(uint32_t *list, size_t *count, uint32_t value) {
    for (size_t i = 0; i < *count; ++i) {
      if (list[i] == value) {
        return;
      }
    }
    list[(*count)++] = value;
  }

  static void addByte(uint8_t *list, size_t *count, uint8_t value) {
    for (size_t i = 0; i < *count; ++i) {
      if (list[i] == value) {
        return;
      }
    }
    list[(*count)++] = value;
  }

  void note(const MbResult &r) {
    if (mbIsLinkFailure(r.status)) {
      if (++consecutiveFailures_ >= OV_LINK_DOWN_AFTER) {
        linkDown_ = true;
      }
    } else {
      consecutiveFailures_ = 0;
    }
  }

  // One read request, sent once.
  MbResult readOnce(uint16_t addr, uint16_t count, uint16_t *out) {
    const MbResult r = master_.readRegisters(link.slave, link.readFunction, addr, count, out);
    note(r);
    if (r.status == MB_OK && unknownOpen_ && unknownAddr_ >= addr && (uint32_t)unknownAddr_ < (uint32_t)addr + count &&
        link.slave == settled_.slave && link.baud == settled_.baud && link.format == settled_.format) {
      // The register of a write whose outcome was unknown has been read, on the same controller.
      unknownOpen_ = false;
      if (((uint16_t)(out[unknownAddr_ - addr] ^ unknownBefore_) & unknownUsed_) != 0) {
        resetNeeded_ = true;
      }
    }
    return r;
  }

  // Reads one word. True when it was read.
  bool readOne(uint16_t addr, uint16_t *word, OvWordStatus *status) {
    if (linkDown_) {
      *word = 0;
      status->status = OV_NOT_READ;
      status->exception = 0;
      return false;
    }
    const MbResult r = readWords(addr, 1, word);
    status->status = r.status;
    status->exception = r.exception;
    if (r.status != MB_OK) {
      *word = 0;
    }
    return r.status == MB_OK;
  }
};

}  // namespace optaview

#endif  // OPTAVIEW_DEVICE_H
