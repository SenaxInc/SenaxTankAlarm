/**
 * OptaViewDevice.h
 *
 * One controller on the bus, seen through its profile: the link settings in use, reads that
 * fall back from block requests to single registers, the probe sweep, verified writes, the
 * reset sequence, the pair checks and the voltage scale check.
 *
 * Nothing here prints. The USB console (OptaViewConsole.h) decides what to show, and a web UI
 * can later sit on the same calls. No Arduino dependency, so the host tests run this code
 * against a simulated SunSaver.
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
static const uint8_t OV_LINK_DOWN_AFTER = 2;      // failed requests in a row that end a batch
static const uint16_t OV_PROBE_TIMEOUT_MS = 400;  // reply wait during the probe sweep and scans
static const uint32_t OV_RESET_GIVE_UP_MS = 20000;

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

struct OvWriteOutcome {
  MbResult write;         // the write request; never repeated, a lost reply is not a lost write
  OvWordStatus readBack;  // reading the register again afterwards
  uint16_t readBackRaw;
  bool verified;          // the register now holds what was written
};

struct OvResetOutcome {
  bool sent;             // the profile has a reset coil and the command went out
  MbResult command;      // a missing reply is normal here: the controller may restart first
  bool cameBack;         // the controller answered again afterwards
  bool aborted;
  uint32_t backAfterMs;  // from the command to that answer
};

enum OvCheckResult : uint8_t {
  OV_CHECK_PASS = 0,
  OV_CHECK_FAIL,
  OV_CHECK_OFF,      // one of the two is 0, which switches that feature off
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
//     1. Setpoints. People and factories pick round numbers (14.00, 13.70, 11.50). Decoded with
//        the right scale the setpoints land on a 0.05 V grid; with the wrong one they do not.
//
//   Live readings
//     2. Regulation. In absorption, float or equalize the battery sits at the regulation target.
//        The two registers only agree if the battery's scale is right relative to the target's.
//        So this test can confirm the scale in use, but it cannot overturn it: when it points
//        elsewhere, the target's own scale may be the one that is wrong.
//     3. A multimeter reading typed in by the user. It decides.
//
// Neither verdict is borrowed for the other scale: a multimeter says nothing about how settings
// are stored, and round setpoints say nothing about the readings.
// ---------------------------------------------------------------------------------------------

static const uint8_t OV_SCALE_CANDIDATES = 6;

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
  int8_t verdict;  // candidate index, or -1 when the setpoints cannot tell
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
  report->verdict = -1;
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
    report->verdict = (int8_t)best;
  }
}

// Test 2. Call only while the controller is regulating.
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

// Test 3. A candidate matches when it puts the register within 1.2 % of the multimeter.
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

// The verdict for live readings. A multimeter comparison decides, also when it fits no
// candidate: then the scale stays open, whatever the regulation test says. Without one, the
// regulation test counts only when it confirms the scale in use (candidate 0).
inline void ovJudgeLive(OvLiveScaleReport *report) {
  report->verdict = -1;
  report->conflict = false;
  if (report->meterCompared) {
    report->verdict = report->meterVerdict;
    report->conflict = report->meterVerdict >= 0 && report->regulationVerdict >= 0 &&
                       report->regulationVerdict != report->meterVerdict;
  } else if (report->regulationVerdict == 0) {
    report->verdict = 0;
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
        consecutiveFailures_(0) {
    link.slave = 1;
    link.baud = 9600;
    link.format = MB_8N2;
    link.readFunction = MB_FC_READ_INPUT;
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
  }

  // (Re)opens the serial port with link.baud and link.format.
  bool applyLink() { return master_.transport().configure(link.baud, link.format); }

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

  // One read request for `count` words, repeated after a link failure.
  MbResult readWords(uint16_t addr, uint16_t count, uint16_t *out) {
    MbResult r = master_.readRegisters(link.slave, link.readFunction, addr, count, out);
    for (uint8_t attempt = 0; attempt < retries && mbIsLinkFailure(r.status); ++attempt) {
      r = master_.readRegisters(link.slave, link.readFunction, addr, count, out);
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
      const MbResult r = readWords(at, n, words + done);
      note(r);
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
      if (blockMode == OV_BLOCKS_AUTO) {
        // No usable reply to a block. If one register still answers, blocks are the problem.
        readOne(at, words + done, status + done);
        if (status[done].status == MB_OK || status[done].status == MB_EXCEPTION) {
          blocksFailed_ = true;
        }
        ++done;
        continue;
      }
      // Blocks are forced on: the whole block is lost.
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
        if (!master_.transport().configure(bauds[b], formats[f])) {
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

  // Writes one register with function 06 and reads it back.
  OvWriteOutcome writeRegister(const OvRegister &reg, uint16_t raw) {
    OvWriteOutcome out;
    memset(&out, 0, sizeof(out));
    out.write = master_.writeRegister(link.slave, reg.addr, raw);
    beginBatch();
    uint16_t stored = 0;
    readOne(reg.addr, &stored, &out.readBack);
    out.readBackRaw = stored;
    out.verified = out.readBack.status == MB_OK && stored == raw;
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

  MbResult writeCoil(const OvCoil &coil, bool on) { return master_.writeCoil(link.slave, coil.addr, on); }

  // Restarts the controller through the profile's reset coil, waits, then asks until it answers.
  OvResetOutcome resetController(OvAbortCheck *abort) {
    OvResetOutcome out;
    memset(&out, 0, sizeof(out));
    const OvModbusDefaults &defaults = profile_.modbus();
    if (defaults.resetCoil == nullptr || defaults.probeRegister == nullptr) {
      return out;
    }
    MbTransport &transport = master_.transport();
    out.command = master_.writeCoil(link.slave, defaults.resetCoil->addr, true);
    out.sent = out.command.status != MB_BAD_REQUEST;
    if (out.command.status == MB_EXCEPTION || out.command.status == MB_BAD_REQUEST) {
      return out;  // refused: it did not restart
    }
    const uint32_t start = transport.nowMs();
    while ((uint32_t)(transport.nowMs() - start) < defaults.resetWaitMs) {
      if (abort != nullptr && abort->aborted()) {
        out.aborted = true;
        return out;
      }
      transport.idle();
    }
    for (;;) {
      uint16_t raw = 0;
      const MbResult r = master_.readRegisters(link.slave, link.readFunction, defaults.probeRegister->addr, 1, &raw);
      if (r.status == MB_OK || r.status == MB_EXCEPTION) {
        out.cameBack = true;
        break;
      }
      if ((uint32_t)(transport.nowMs() - start) >= OV_RESET_GIVE_UP_MS) {
        break;
      }
      if (abort != nullptr && abort->aborted()) {
        out.aborted = true;
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
    if (ovRawValue(*check.below, belowWords) == 0 || ovRawValue(*check.above, aboveWords) == 0) {
      return OV_CHECK_OFF;
    }
    return *below < *above ? OV_CHECK_PASS : OV_CHECK_FAIL;
  }

  // Reads what the scale check needs and judges it. meterVolts <= 0 means no multimeter value.
  // Returns false when the profile has no scale check.
  bool scaleCheck(double meterVolts, OvScaleReport *report) {
    memset(report, 0, sizeof(*report));
    report->settings.verdict = -1;
    report->live.regulationVerdict = -1;
    report->live.meterVerdict = -1;
    report->live.verdict = -1;
    const OvScaleCheck &check = profile_.scaleCheck();
    if (!check.present) {
      return false;
    }
    if (check.settingsScale != nullptr) {
      checkSettingsScale(check, &report->settings);
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

  // Test 1: how round the stored setpoints are.
  void checkSettingsScale(const OvScaleCheck &check, OvSettingsScaleReport *report) {
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
  }

  // Tests 2 and 3: the battery against the regulation target, and against a multimeter.
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

  // Reads one word. True when it was read.
  bool readOne(uint16_t addr, uint16_t *word, OvWordStatus *status) {
    if (linkDown_) {
      *word = 0;
      status->status = OV_NOT_READ;
      status->exception = 0;
      return false;
    }
    const MbResult r = readWords(addr, 1, word);
    note(r);
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
