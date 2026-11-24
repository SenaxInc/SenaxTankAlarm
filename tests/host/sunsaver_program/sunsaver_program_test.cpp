// Host tests for programming a SunSaver MPPT from the TankAlarm client:
// TankAlarm-112025-Common/src/TankAlarm_SunSaverProgram.h (what may be written, in which order,
// and the run that writes it and restarts the controller). They run against the simulated
// SunSaver MPPT of the OptaView tests (../optaview/sim_sunsaver.h), through OptaView's Modbus
// master, one register per request, as the client does it with ArduinoModbus.
//
//   make test ARDUINOJSON_DIR=<ArduinoJson v7.4.3>/src
//
// Three sweeps carry most of the weight. They take the run that goes well and disturb it at
// every single request in turn: one frame lost or damaged, two frames lost in a row, and the
// controller's power gone for good. Whatever the outcome, the controller must be left with
// settings that are in order, with no register changed that the plan does not name, and not
// waiting for a restart that nobody will send.
//
// The code under test is compiled as C++14, the dialect of the Opta's compiler.

#include "test_support.h"  // ../optaview: the test macros, the simulator and the OptaView headers

#include <functional>
#include <limits>

#include "TankAlarm_SunSaverProgram.h"
#include "TankAlarm_SunSaverReport.h"

namespace ss = sunsaver;

// CHECK_EQ compares numbers. This one compares two texts, and shows the one it got.
#define CHECK_TEXT(actual, expected)                                                              \
  do {                                                                                            \
    ++gChecks;                                                                                    \
    const std::string checkActual = (actual);                                                     \
    const std::string checkExpected = (expected);                                                 \
    if (checkActual != checkExpected)                                                             \
      reportFailure(__FILE__, __LINE__, #actual " is " #expected, "it is: \"" + checkActual + "\""); \
  } while (0)

// ============================================================================
// What the tests share
// ============================================================================

static const uint16_t A_ARRAY_FAULT = 0x0012;
static const uint16_t A_LOAD_STATE = 0x001A;
static const uint16_t A_LOAD_FAULT = 0x001B;
static const uint16_t A_V_LVD = 0x001C;
static const uint16_t A_COIL_RESTART = 0x00FF;
static const uint16_t EDIT_ARRAY = 1u << 5;
static const uint16_t EDIT_LOAD = 1u << 6;

// The registers a run may write, by address, in the order a factory-default controller gets
// them. Written out here a second time, from the specification's table.
static const uint16_t A_EV_REG = 0xE000, A_EV_FLOAT = 0xE001, A_ET_FLOAT = 0xE002, A_ET_FLOATLB = 0xE003;
static const uint16_t A_EV_EQ = 0xE007;
static const uint16_t A_EV_REG2 = 0xE00D, A_EV_FLOAT2 = 0xE00E, A_ET_FLOAT2 = 0xE00F, A_ET_FLOATLB2 = 0xE010;
static const uint16_t A_EV_EQ2 = 0xE014;
static const uint16_t A_EV_TEMPCOMP = 0xE01A, A_EV_HVD = 0xE01B, A_EV_HVR = 0xE01C, A_EVB_REF_LIM = 0xE01D;
static const uint16_t A_EV_LVD = 0xE022, A_EV_LVR = 0xE023, A_ER_ICOMP = 0xE026;

static const uint16_t kPlanAddrs[ss::PLAN_ENTRIES] = {
    A_EV_EQ,      A_EV_EQ2,    A_EV_TEMPCOMP, A_EV_HVD,      A_EV_HVR,   A_EVB_REF_LIM,
    A_EV_REG,     A_EV_FLOAT,  A_ET_FLOAT,    A_ET_FLOATLB,  A_EV_REG2,  A_EV_FLOAT2,
    A_ET_FLOAT2,  A_ET_FLOATLB2, A_EV_LVR,    A_EV_LVD,      A_ER_ICOMP};

// One request a test's bus sent.
struct Asked {
  char kind;  // 'r' a register read, 'w' a register written, 'c' a coil switched on
  uint16_t addr;
  uint16_t value;
  ss::IoStatus status;
  uint64_t at;   // the simulator's clock when it went out
  uint64_t end;  // and when its answer was in, or the master gave up on it
};

static bool wordAt(const SimSunSaver &sim, uint16_t addr, uint16_t *word) {
  const std::map<uint16_t, uint16_t>::const_iterator it = sim.regs.find(addr);
  if (it == sim.regs.end()) {
    return false;
  }
  *word = it->second;
  return true;
}

static uint16_t reg(const SimSunSaver &sim, uint16_t addr) {
  uint16_t word = 0;
  CHECK_MSG(wordAt(sim, addr, &word), "the simulator has no register 0x%04X", addr);
  return word;
}

// True while the lower register of a pair is below the upper one. A pair with a register the
// simulator does not have is not judged.
static bool pairInOrder(const SimSunSaver &sim, uint16_t lower, uint16_t upper) {
  uint16_t low = 0;
  uint16_t high = 0;
  return !wordAt(sim, lower, &low) || !wordAt(sim, upper, &high) || low < high;
}

static bool settingsInOrder(const SimSunSaver &sim) {
  return pairInOrder(sim, A_EV_HVR, A_EV_HVD) && pairInOrder(sim, A_EV_FLOAT, A_EV_REG) &&
         pairInOrder(sim, A_EV_FLOAT2, A_EV_REG2) && pairInOrder(sim, A_EV_LVD, A_EV_LVR);
}

static bool editPending(const SimSunSaver &sim) {
  return (reg(sim, A_ARRAY_FAULT) & EDIT_ARRAY) != 0 || (reg(sim, A_LOAD_FAULT) & EDIT_LOAD) != 0;
}

// While a controller waits for a restart its stored LVD may not be the one in effect, so the
// scale of its live readings is not proven (sunsaver::storedLvdInEffect()) and a run counts the
// lower of the two battery readings. 13.80 V with x100 is 13.34 V with x96.667: above the
// recommended profile's LVR of 13.25 V either way. The default 13.68 V is not (13.23 V).
static void chargeForPending(SimSunSaver &sim) { sim.regs[0x0008] = sim.volts(13.80); }

// The Bus of these tests. It keeps a list of what was asked, lets a test act just before a
// request goes out, and looks at the controller's memory before every request: the settings
// must be in order at every moment of a run, not only at its end.
struct TestBus : ss::Bus {
  SimSunSaver &sim;
  ModbusMaster &master;
  uint8_t function;
  std::vector<Asked> log;
  std::function<void(size_t index, char kind, uint16_t addr)> before;
  long disorder;
  uint8_t slave;  // the Modbus id the requests go to

  TestBus(SimSunSaver &s, ModbusMaster &m) : sim(s), master(m), function(MB_FC_READ_INPUT), disorder(0), slave(1) {}

  ss::IoStatus readRegister(uint16_t addr, uint16_t *raw) override {
    const uint64_t at = announce('r', addr);
    uint16_t word = 0;
    const MbResult result = master.readRegisters(slave, function, addr, 1, &word);
    if (result.status == MB_OK) {
      *raw = word;
    }
    return noted('r', addr, 0, at, result);
  }

  ss::IoStatus writeRegister(uint16_t addr, uint16_t raw) override {
    const uint64_t at = announce('w', addr);
    return noted('w', addr, raw, at, master.writeRegister(slave, addr, raw));
  }

  ss::IoStatus writeCoil(uint16_t addr) override {
    const uint64_t at = announce('c', addr);
    return noted('c', addr, 0xFF00, at, master.writeCoil(slave, addr, true));
  }

  // The simulator's clock is the client's here: a Station waits on it, and only on it.
  uint32_t millisNow() override { return (uint32_t)sim.elapsedMs(); }
  uint16_t timeoutMs() const override { return (uint16_t)master.responseTimeoutMs; }

  size_t count(char kind) const {
    size_t n = 0;
    for (size_t i = 0; i < log.size(); ++i) {
      n += log[i].kind == kind ? 1 : 0;
    }
    return n;
  }

  size_t count(char kind, uint16_t addr) const {
    size_t n = 0;
    for (size_t i = 0; i < log.size(); ++i) {
      n += (log[i].kind == kind && log[i].addr == addr) ? 1 : 0;
    }
    return n;
  }

  // The index of the n-th request of a kind (n from 0), or the log's size when there is none.
  size_t nth(char kind, size_t n) const {
    for (size_t i = 0; i < log.size(); ++i) {
      if (log[i].kind == kind && n-- == 0) {
        return i;
      }
    }
    return log.size();
  }

 private:
  uint64_t announce(char kind, uint16_t addr) {
    if (before) {
      before(log.size(), kind, addr);
    }
    if (!settingsInOrder(sim)) {
      ++disorder;
    }
    return sim.elapsedMs();
  }

  ss::IoStatus noted(char kind, uint16_t addr, uint16_t value, uint64_t at, const MbResult &result) {
    ss::IoStatus status = ss::SSIO_NO_REPLY;
    if (result.status == MB_OK) {
      status = ss::SSIO_OK;
    } else if (result.status == MB_EXCEPTION) {
      status = ss::SSIO_REFUSED;
    }
    const Asked asked = {kind, addr, value, status, at, sim.elapsedMs()};
    log.push_back(asked);
    return status;
  }
};

// A controller, the line to it, and a programmer, driven the way the client drives it: a few
// requests per step, a pause between the steps, a longer one after a request without a reply,
// and go() wherever the run waits for it.
struct Station {
  SimSunSaver sim;
  ModbusMaster master;
  TestBus bus;
  ss::Programmer programmer;

  uint8_t budget;
  uint32_t gapMs;
  uint8_t refuseAt;              // refuse(REFUSE_JOURNAL) at this Commit point, where go() would be called
  std::vector<uint8_t> commits;  // the Commit points the run waited at
  unsigned steps;
  bool finished;

  explicit Station(double voltsNumerator = 100.0, double settingsNumerator = 0.0)
      : sim(voltsNumerator, settingsNumerator),
        master(sim),
        bus(sim, master),
        budget(4),
        gapMs(250),
        refuseAt(ss::COMMIT_NONE),
        steps(0),
        finished(false) {
    sim.configure(9600, MB_8N2);
  }

  // Steps the programmer until no run is under way.
  void drive() {
    finished = false;
    for (unsigned guard = 0; guard < 4000 && !finished; ++guard) {
      const size_t had = bus.log.size();
      const bool ended = programmer.step(bus, budget);
      ++steps;
      const size_t made = bus.log.size() - had;
      CHECK_MSG(made <= budget, "a step made %u requests with a budget of %u", (unsigned)made, (unsigned)budget);
      // A request that got no reply ends its step, and the programmer says so.
      for (size_t i = had; i + 1 < bus.log.size(); ++i) {
        CHECK_MSG(bus.log[i].status != ss::SSIO_NO_REPLY, "request %u got no reply and the step went on", (unsigned)i);
      }
      CHECK_EQ(programmer.missed(), made > 0 && bus.log.back().status == ss::SSIO_NO_REPLY);
      if (programmer.awaiting() != ss::COMMIT_NONE) {
        CHECK(!ended);
        CHECK(programmer.active());
        commits.push_back(programmer.awaiting());
        // A run that waits makes no request, however often it is stepped.
        const size_t waitingAt = bus.log.size();
        CHECK(!programmer.step(bus, budget));
        CHECK_EQ(bus.log.size(), waitingAt);
        if (refuseAt == programmer.awaiting()) {
          programmer.refuse(ss::REFUSE_JOURNAL);
        } else {
          programmer.go();
        }
        continue;
      }
      if (ended) {
        finished = true;
        if (programmer.missed()) {
          sim.advance(master.responseTimeoutMs);  // the client's line rest, before anything else is sent
        }
        break;
      }
      uint32_t wait = programmer.missed() ? master.responseTimeoutMs : gapMs;
      if (programmer.pauseMs() > wait) {
        wait = programmer.pauseMs();
      }
      sim.advance(wait);
    }
    CHECK_MSG(finished, "the run did not end within %u steps", steps);
    CHECK(!programmer.active());
  }

  const ss::ProgramResult &run(const ss::Profile &profile = ss::lifepo4Profile(), bool loadCurrentAllowed = true) {
    programmer.start(profile, loadCurrentAllowed);
    drive();
    return programmer.result();
  }
};

// Every reply-less request is followed by silence for a Modbus timeout, and every restart
// command that was not refused by the wait the controller needs to come back. Both are counted
// from the end of the request: a request to a controller that is off ends after a whole timeout
// by itself, and one whose reply was garbage ends at once.
static void checkPausesIn(const std::vector<Asked> &log, uint32_t timeoutMs) {
  for (size_t i = 0; i + 1 < log.size(); ++i) {
    const uint64_t gap = log[i + 1].at - log[i].end;  // from the moment the master was done with it
    if (log[i].status == ss::SSIO_NO_REPLY) {
      CHECK_MSG(gap >= timeoutMs, "request %u got no reply, and the next one went out %llu ms later",
                (unsigned)i, (unsigned long long)gap);
    }
    if (log[i].kind == 'c' && log[i].status != ss::SSIO_REFUSED) {
      CHECK_MSG(gap >= ss::RESTART_WAIT_MS, "restart command at request %u, and the next request went out %llu ms later",
                (unsigned)i, (unsigned long long)gap);
    }
  }
}

static void checkPauses(const Station &station) { checkPausesIn(station.bus.log, station.master.responseTimeoutMs); }

// What a fresh controller of the same kind holds: for "nothing else was changed".
static bool onlyPlanChanged(const SimSunSaver &sim, const SimSunSaver &fresh) {
  bool same = true;
  for (std::map<uint16_t, uint16_t>::const_iterator it = fresh.regs.begin(); it != fresh.regs.end(); ++it) {
    const uint16_t addr = it->first;
    bool planned = false;
    for (uint8_t i = 0; i < ss::PLAN_ENTRIES; ++i) {
      planned = planned || kPlanAddrs[i] == addr;
    }
    // The fault words and the LVD in effect are the controller's own doing.
    if (planned || addr == A_ARRAY_FAULT || addr == A_LOAD_FAULT || addr == A_V_LVD) {
      continue;
    }
    uint16_t now = 0;
    if (!wordAt(sim, addr, &now) || now != it->second) {
      fprintf(stderr, "  register 0x%04X changed from 0x%04X to 0x%04X\n", addr, it->second, now);
      same = false;
    }
  }
  return same;
}

// The words a controller has to hold for a profile, from the simulator's own encoding.
static uint16_t wantedWord(const SimSunSaver &sim, uint16_t addr, const ss::Profile &profile) {
  switch (addr) {
    case A_EV_REG:
    case A_EV_REG2:
      return sim.setVolts(profile.absorbCv / 100.0);
    case A_EV_FLOAT:
    case A_EV_FLOAT2:
      return sim.setVolts(profile.floatCv / 100.0);
    case A_ET_FLOAT:
    case A_ET_FLOATLB:
    case A_ET_FLOAT2:
    case A_ET_FLOATLB2:
      return profile.absorbSeconds;
    case A_EV_HVD:
    case A_EVB_REF_LIM:
      return sim.setVolts(profile.hvdCv / 100.0);
    case A_EV_HVR:
      return sim.setVolts(profile.hvrCv / 100.0);
    case A_EV_LVD:
      return sim.setVolts(profile.lvdCv / 100.0);
    case A_EV_LVR:
      return sim.setVolts(profile.lvrCv / 100.0);
    default:
      return 0;
  }
}

static bool holdsProfile(const SimSunSaver &sim, const ss::Profile &profile) {
  for (uint8_t i = 0; i < ss::PLAN_ENTRIES; ++i) {
    if (reg(sim, kPlanAddrs[i]) != wantedWord(sim, kPlanAddrs[i], profile)) {
      return false;
    }
  }
  return true;
}

static void holdProfile(SimSunSaver &sim, const ss::Profile &profile) {
  for (uint8_t i = 0; i < ss::PLAN_ENTRIES; ++i) {
    sim.regs[kPlanAddrs[i]] = wantedWord(sim, kPlanAddrs[i], profile);
  }
  sim.regs[A_V_LVD] = sim.volts(profile.lvdCv / 100.0);  // the LVD in effect follows at a restart
}

// Every register of the plan holds what it held before the run or what the run wants: never a
// third word.
static bool beforeOrTarget(const SimSunSaver &sim, const SimSunSaver &fresh, const ss::Profile &profile) {
  for (uint8_t i = 0; i < ss::PLAN_ENTRIES; ++i) {
    const uint16_t now = reg(sim, kPlanAddrs[i]);
    if (now != reg(fresh, kPlanAddrs[i]) && now != wantedWord(sim, kPlanAddrs[i], profile)) {
      return false;
    }
  }
  return true;
}

static const ss::PlanEntry *entryFor(const ss::ProgramResult &result, uint16_t addr) {
  for (uint8_t i = 0; i < result.planned; ++i) {
    if (ss::item(result.entries[i].item).addr == addr) {
      return &result.entries[i];
    }
  }
  return nullptr;
}

static uint8_t statusOf(const ss::ProgramResult &result, uint16_t addr) {
  const ss::PlanEntry *entry = entryFor(result, addr);
  CHECK_MSG(entry != nullptr, "the result has no entry for 0x%04X", addr);
  return entry != nullptr ? entry->status : (uint8_t)0xFF;
}

static ss::Snapshot snapshotOf(const SimSunSaver &sim) {
  ss::Snapshot snapshot;
  ss::clearSnapshot(&snapshot);
  for (uint8_t i = 0; i < ss::ITEM_COUNT; ++i) {
    uint16_t word = 0;
    if (wordAt(sim, ss::item(i).addr, &word)) {
      snapshot.raw[i] = word;
      snapshot.status[i] = ss::SSIO_OK;
    } else {
      snapshot.status[i] = ss::SSIO_REFUSED;
    }
  }
  return snapshot;
}

template <typename Describe>
static std::string textFrom(Describe describe) {
  char buffer[400];
  ss::Text text(buffer, sizeof(buffer));
  describe(&text);
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

// The requests of a run on a factory-default controller in which nothing goes wrong. After each
// restart command the controller misses one request while it restarts.
typedef std::vector<std::pair<char, uint16_t> > Sequence;

static Sequence quietRun() {
  Sequence s;
  for (uint8_t i = 0; i < ss::ITEM_COUNT; ++i) {
    s.push_back(std::make_pair('r', ss::item(i).addr));
  }
  s.push_back(std::make_pair('c', A_COIL_RESTART));
  s.push_back(std::make_pair('r', A_ARRAY_FAULT));  // no answer
  s.push_back(std::make_pair('r', A_ARRAY_FAULT));
  s.push_back(std::make_pair('r', A_LOAD_FAULT));
  for (uint8_t i = 0; i < ss::PLAN_ENTRIES; ++i) {
    if (kPlanAddrs[i] == A_EV_EQ || kPlanAddrs[i] == A_ER_ICOMP) {
      continue;  // already 0 in a factory-default controller
    }
    s.push_back(std::make_pair('w', kPlanAddrs[i]));
    s.push_back(std::make_pair('r', kPlanAddrs[i]));
  }
  s.push_back(std::make_pair('r', A_LOAD_STATE));
  s.push_back(std::make_pair('r', A_ARRAY_FAULT));
  s.push_back(std::make_pair('r', A_LOAD_FAULT));
  s.push_back(std::make_pair('c', A_COIL_RESTART));
  s.push_back(std::make_pair('r', A_ARRAY_FAULT));  // no answer
  s.push_back(std::make_pair('r', A_ARRAY_FAULT));
  s.push_back(std::make_pair('r', A_LOAD_FAULT));
  for (uint8_t i = 0; i < ss::PLAN_ENTRIES; ++i) {
    s.push_back(std::make_pair('r', kPlanAddrs[i]));
  }
  s.push_back(std::make_pair('r', A_LOAD_STATE));
  return s;
}

static void checkSequence(const TestBus &bus, const Sequence &want) {
  CHECK_EQ(bus.log.size(), want.size());
  for (size_t i = 0; i < bus.log.size() && i < want.size(); ++i) {
    CHECK_MSG(bus.log[i].kind == want[i].first && bus.log[i].addr == want[i].second,
              "request %u was %c 0x%04X, expected %c 0x%04X", (unsigned)i, bus.log[i].kind, bus.log[i].addr,
              want[i].first, want[i].second);
  }
}

// ============================================================================
// What can be asked for
// ============================================================================
static void testProfile() {
  const ss::Profile good = ss::lifepo4Profile();
  CHECK_EQ(good.absorbCv, 1420);
  CHECK_EQ(good.floatCv, 1350);
  CHECK_EQ(good.absorbSeconds, 900);
  CHECK_EQ(good.hvdCv, 1445);
  CHECK_EQ(good.hvrCv, 1360);
  CHECK_EQ(good.lvdCv, 1310);
  CHECK_EQ(good.lvrCv, 1325);
  CHECK_EQ(ss::validateProfile(good), 0);
  // The battery it is meant for switches itself off above 14.8 V: the charger must give up first.
  CHECK(good.hvdCv < 1480);

  // Each limit: the last value inside passes, the first one outside sets its own bit and no other.
  struct Case {
    uint16_t ss::Profile::*field;
    uint16_t value;
    uint16_t errors;
  };
  const Case cases[] = {
      {&ss::Profile::absorbCv, 1380, 0},  // with the float voltage lowered below, see the loop
      {&ss::Profile::absorbCv, 1375, ss::PROFILE_ABSORB},
      {&ss::Profile::absorbCv, 1435, 0},
      {&ss::Profile::absorbCv, 1440, ss::PROFILE_HVD},  // closer than 0.10 V to the HVD of 14.45
      {&ss::Profile::floatCv, 1320, 0},
      {&ss::Profile::floatCv, 1315, ss::PROFILE_FLOAT},
      {&ss::Profile::floatCv, 1390, 0},
      {&ss::Profile::floatCv, 1395, ss::PROFILE_FLOAT},
      {&ss::Profile::absorbSeconds, 60, 0},
      {&ss::Profile::absorbSeconds, 59, ss::PROFILE_TIME},
      {&ss::Profile::absorbSeconds, 14400, 0},
      {&ss::Profile::absorbSeconds, 14401, ss::PROFILE_TIME},
      {&ss::Profile::absorbSeconds, 0, ss::PROFILE_TIME},
      {&ss::Profile::hvdCv, 1430, 0},
      {&ss::Profile::hvdCv, 1425, ss::PROFILE_HVD},
      {&ss::Profile::hvdCv, 1500, 0},
      {&ss::Profile::hvdCv, 1505, ss::PROFILE_HVD},
      {&ss::Profile::hvrCv, 1320, 0},
      {&ss::Profile::hvrCv, 1315, ss::PROFILE_HVR},
      {&ss::Profile::hvrCv, 1425, 0},
      {&ss::Profile::hvrCv, 1430, ss::PROFILE_HVR},  // closer than 0.20 V to the HVD
      {&ss::Profile::lvdCv, 1000, 0},
      {&ss::Profile::lvdCv, 995, ss::PROFILE_LVD},
      {&ss::Profile::lvdCv, 1315, 0},
      {&ss::Profile::lvdCv, 1320, ss::PROFILE_LVR},  // closer than 0.10 V to the LVR of 13.25
      {&ss::Profile::lvrCv, 1320, 0},
      {&ss::Profile::lvrCv, 1315, ss::PROFILE_LVR},
      {&ss::Profile::lvrCv, 1360, 0},
      {&ss::Profile::lvrCv, 1365, ss::PROFILE_LVR},
      {&ss::Profile::lvdCv, 1312, ss::PROFILE_GRID},
      {&ss::Profile::absorbCv, 1421, ss::PROFILE_GRID},
      {&ss::Profile::hvrCv, 1361, ss::PROFILE_GRID},
      {&ss::Profile::floatCv, 1351, ss::PROFILE_GRID},
      {&ss::Profile::hvdCv, 1446, ss::PROFILE_GRID},
      {&ss::Profile::lvrCv, 1327, ss::PROFILE_GRID},  // each of the six voltages is looked at
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    ss::Profile profile = good;
    if (cases[i].field == &ss::Profile::absorbCv && cases[i].value < 1400) {
      profile.floatCv = 1330;  // keeps the float voltage below a low absorption voltage
    }
    profile.*(cases[i].field) = cases[i].value;
    CHECK_MSG(ss::validateProfile(profile) == cases[i].errors, "case %u: errors 0x%X, expected 0x%X", (unsigned)i,
              (unsigned)ss::validateProfile(profile), (unsigned)cases[i].errors);
  }
  {
    // The limits that do not depend on another value, at their far ends.
    ss::Profile profile = good;
    profile.absorbCv = 1460;
    profile.hvdCv = 1470;
    CHECK_EQ(ss::validateProfile(profile), 0);
    profile.absorbCv = 1465;
    profile.hvdCv = 1475;
    CHECK_EQ(ss::validateProfile(profile), ss::PROFILE_ABSORB);
    profile = good;
    profile.hvdCv = 1500;
    profile.hvrCv = 1440;
    CHECK_EQ(ss::validateProfile(profile), 0);
    profile.hvrCv = 1445;
    CHECK_EQ(ss::validateProfile(profile), ss::PROFILE_HVR);
    profile = good;
    profile.lvdCv = 1330;
    profile.lvrCv = 1340;
    CHECK_EQ(ss::validateProfile(profile), 0);
    profile.lvdCv = 1335;
    profile.lvrCv = 1345;
    CHECK_EQ(ss::validateProfile(profile), ss::PROFILE_LVD);
    // The float voltage has to stay below the absorption voltage, wherever that is.
    profile = good;
    profile.absorbCv = 1385;
    profile.floatCv = 1385;
    CHECK_EQ(ss::validateProfile(profile), ss::PROFILE_FLOAT);
    profile.floatCv = 1380;
    CHECK_EQ(ss::validateProfile(profile), 0);
  }
  {
    // Everything wrong at once, and values that would wrap a 16-bit sum.
    const ss::Profile bad = {0, 0, 0, 0, 0, 0, 0};
    CHECK_EQ(ss::validateProfile(bad), ss::PROFILE_ABSORB | ss::PROFILE_FLOAT | ss::PROFILE_TIME | ss::PROFILE_HVD |
                                           ss::PROFILE_HVR | ss::PROFILE_LVD | ss::PROFILE_LVR);
    const ss::Profile huge = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    CHECK_EQ(ss::validateProfile(huge), ss::PROFILE_ABSORB | ss::PROFILE_FLOAT | ss::PROFILE_TIME | ss::PROFILE_HVD |
                                            ss::PROFILE_HVR | ss::PROFILE_LVD | ss::PROFILE_LVR);
    ss::Profile wrap = good;
    wrap.lvdCv = 0xFFFA;  // + 10 wraps to 4 in 16 bits
    wrap.lvrCv = 1325;
    CHECK((ss::validateProfile(wrap) & ss::PROFILE_LVR) != 0);
    wrap = good;
    wrap.absorbCv = 0xFFFA;
    CHECK((ss::validateProfile(wrap) & ss::PROFILE_HVD) != 0);
    wrap = good;
    wrap.hvrCv = 0xFFF0;
    CHECK((ss::validateProfile(wrap) & ss::PROFILE_HVR) != 0);
  }
  // What the limits allow, the charger check accepts for a LiFePO4 battery: the two do not drift apart.
  {
    ss::VoltRange absorb = {0.0, 0.0};
    ss::VoltRange floating = {0.0, 0.0};
    ss::chargeRanges(ss::CHEMISTRY_LIFEPO4, &absorb, &floating);
    for (uint16_t cv = 1300; cv <= 1500; cv = (uint16_t)(cv + 5)) {
      ss::Profile profile = good;
      profile.absorbCv = cv;
      profile.floatCv = 1320;
      profile.hvdCv = 1500;
      const bool allowed = (ss::validateProfile(profile) & ss::PROFILE_ABSORB) == 0;
      CHECK_EQ(allowed, cv / 100.0 >= absorb.low - 0.001 && cv / 100.0 <= absorb.high + 0.001);
      profile = good;
      profile.absorbCv = 1460;
      profile.hvdCv = 1500;
      profile.floatCv = cv;
      const bool floatAllowed = (ss::validateProfile(profile) & ss::PROFILE_FLOAT) == 0;
      CHECK_EQ(floatAllowed, cv / 100.0 >= floating.low - 0.001 && cv / 100.0 <= floating.high + 0.001);
    }
  }

  CHECK_EQ(ss::centivoltsOf(13.8), 1380);
  CHECK_EQ(ss::centivoltsOf(14.6), 1460);
  CHECK_EQ(ss::centivoltsOf(13.2), 1320);
  CHECK_EQ(ss::centivoltsOf(13.9), 1390);

  // The words, worked out by hand: V * 32768 / 100, and V * 32768 / 96.667.
  CHECK_EQ(ss::rawFromCentivolts(1310, ss::SCALE_100), 4293);
  CHECK_EQ(ss::rawFromCentivolts(1325, ss::SCALE_100), 4342);
  CHECK_EQ(ss::rawFromCentivolts(1420, ss::SCALE_100), 4653);
  CHECK_EQ(ss::rawFromCentivolts(1350, ss::SCALE_100), 4424);
  CHECK_EQ(ss::rawFromCentivolts(1445, ss::SCALE_100), 4735);
  CHECK_EQ(ss::rawFromCentivolts(1360, ss::SCALE_100), 4456);
  CHECK_EQ(ss::rawFromCentivolts(1310, ss::SCALE_96667), 4441);
  CHECK_EQ(ss::rawFromCentivolts(1325, ss::SCALE_96667), 4491);
  CHECK_EQ(ss::rawFromCentivolts(1420, ss::SCALE_96667), 4813);
  CHECK_EQ(ss::rawFromCentivolts(1350, ss::SCALE_96667), 4576);
  CHECK_EQ(ss::rawFromCentivolts(1445, ss::SCALE_96667), 4898);
  CHECK_EQ(ss::rawFromCentivolts(1360, ss::SCALE_96667), 4610);
  CHECK_EQ(ss::rawFromCentivolts(1310, ss::SCALE_UNKNOWN), 0);
  CHECK_EQ(ss::rawFromCentivolts(0, ss::SCALE_100), 0);
  CHECK_EQ(ss::rawFromCentivolts(0xFFFF, ss::SCALE_100), 0xFFFF);  // 655.35 V has no word: the highest one, not a wrapped one
  // Every voltage that can be asked for reads back as itself, to the hundredth of a volt, with the
  // scale it was written with, and is on the 0.05 V grid that proves that scale.
  for (uint16_t cv = 1000; cv <= 1500; cv = (uint16_t)(cv + 5)) {
    const uint8_t scales[2] = {ss::SCALE_100, ss::SCALE_96667};
    for (int s = 0; s < 2; ++s) {
      const uint16_t raw = ss::rawFromCentivolts(cv, scales[s]);
      const double back = ss::voltsFromRaw(raw, scales[s]);
      CHECK_MSG(near(back, cv / 100.0, 0.0016), "%u cV reads back as %.4f V", (unsigned)cv, back);
      CHECK(ss::gridDistance(back, ss::SETPOINT_GRID_VOLTS) < 0.0016);
    }
  }

  for (uint8_t bit = 0; bit < 16; ++bit) {
    const char *text = ss::profileErrorText((uint16_t)(1u << bit));
    CHECK(plainAscii(text));
    CHECK_EQ(has(text, "no text for"), bit >= ss::PROFILE_ERROR_BITS);
    for (uint8_t other = 0; other < bit && bit < ss::PROFILE_ERROR_BITS; ++other) {
      CHECK(strcmp(text, ss::profileErrorText((uint16_t)(1u << other))) != 0);
    }
  }
  CHECK(has(ss::profileErrorText(ss::PROFILE_LVD), "10.00 to 13.30 V"));
  CHECK(has(ss::profileErrorText(ss::PROFILE_GRID), "0.05 V"));
  CHECK(has(ss::profileErrorText(0), "no text for"));
  CHECK(has(ss::profileErrorText(3), "no text for"));
}

// ============================================================================
// The plan
// ============================================================================
static void testPlan() {
  const ss::Profile profile = ss::lifepo4Profile();
  // The plan's registers are these 17 and no others, each in exactly one step.
  {
    unsigned planned = 0;
    for (uint8_t i = 0; i < ss::ITEM_COUNT; ++i) {
      bool listed = false;
      for (uint8_t p = 0; p < ss::PLAN_ENTRIES; ++p) {
        listed = listed || kPlanAddrs[p] == ss::item(i).addr;
      }
      CHECK_MSG(ss::inPlan(i) == listed, "item %u (%s)", (unsigned)i, ss::item(i).name);
      unsigned steps = 0;
      for (uint8_t s = 0; s < ss::PLAN_STEPS; ++s) {
        steps += ss::planStep(s).first == i ? 1 : 0;
        steps += ss::planStep(s).second == i ? 1 : 0;
      }
      CHECK_EQ(steps, listed ? 1 : 0);
      planned += listed ? 1 : 0;
    }
    CHECK_EQ(planned, ss::PLAN_ENTRIES);
    CHECK(!ss::inPlan(ss::PLAN_SINGLE));
    CHECK(!ss::inPlan(ss::ITEM_COUNT));
    CHECK_EQ(ss::planStep(200).first, ss::planStep(0).first);  // out of range: the first step, not a wild read
    // A pair names two different voltage settings, the lower one first.
    for (uint8_t s = 0; s < ss::PLAN_STEPS; ++s) {
      const ss::PlanStep &step = ss::planStep(s);
      if (step.second != ss::PLAN_SINGLE) {
        CHECK(step.first != step.second);
        CHECK(ss::targetWord(step.first, profile, ss::SCALE_100) < ss::targetWord(step.second, profile, ss::SCALE_100));
        CHECK(ss::targetWord(step.first, profile, ss::SCALE_96667) < ss::targetWord(step.second, profile, ss::SCALE_96667));
      }
    }
  }

  // A factory-default controller: 15 registers to write, in this order, with these words.
  {
    SimSunSaver sim;
    ss::PlanEntry entries[ss::PLAN_ENTRIES];
    memset(entries, 0xEE, sizeof(entries));
    CHECK_EQ(ss::buildPlan(snapshotOf(sim), profile, ss::SCALE_100, entries), 15);
    const uint16_t words[ss::PLAN_ENTRIES] = {0,    0,   0,   4735, 4456, 4735, 4653, 4424, 900,
                                              900,  4653, 4424, 900, 900,  4342, 4293, 0};
    for (uint8_t i = 0; i < ss::PLAN_ENTRIES; ++i) {
      CHECK_MSG(ss::item(entries[i].item).addr == kPlanAddrs[i], "entry %u is %s", (unsigned)i,
                ss::item(entries[i].item).name);
      CHECK_EQ(entries[i].target, words[i]);
      CHECK_EQ(entries[i].before, reg(sim, kPlanAddrs[i]));
      CHECK_EQ(entries[i].found, entries[i].before);
      const bool unchanged = kPlanAddrs[i] == A_EV_EQ || kPlanAddrs[i] == A_ER_ICOMP;
      CHECK_EQ(entries[i].status, unchanged ? ss::ENTRY_UNCHANGED : ss::ENTRY_PENDING);
    }
    // The same controller with the other encoding gets other words for the same voltages.
    SimSunSaver other(96.667);
    CHECK_EQ(ss::buildPlan(snapshotOf(other), profile, ss::SCALE_96667, entries), 15);
    for (uint8_t i = 0; i < ss::PLAN_ENTRIES; ++i) {
      CHECK_EQ(entries[i].target, wantedWord(other, kPlanAddrs[i], profile));
    }
    CHECK_EQ(entries[15].target, 4441);  // EV_lvd: 13.10 V
  }

  // A controller that holds the profile: nothing to write.
  {
    SimSunSaver sim;
    holdProfile(sim, profile);
    ss::PlanEntry entries[ss::PLAN_ENTRIES];
    CHECK_EQ(ss::buildPlan(snapshotOf(sim), profile, ss::SCALE_100, entries), 0);
    for (uint8_t i = 0; i < ss::PLAN_ENTRIES; ++i) {
      CHECK_EQ(entries[i].status, ss::ENTRY_UNCHANGED);
    }
    // One register off by one count is one register to write.
    sim.regs[A_ET_FLOATLB2] = 901;
    CHECK_EQ(ss::buildPlan(snapshotOf(sim), profile, ss::SCALE_100, entries), 1);
  }

  // The order within a pair. Coming down from high thresholds the lower register goes first,
  // coming up from low ones the upper register does.
  {
    SimSunSaver sim;
    sim.regs[A_EV_LVD] = sim.setVolts(13.50);
    sim.regs[A_EV_LVR] = sim.setVolts(13.80);
    ss::PlanEntry entries[ss::PLAN_ENTRIES];
    ss::buildPlan(snapshotOf(sim), profile, ss::SCALE_100, entries);
    CHECK_EQ(ss::item(entries[14].item).addr, A_EV_LVD);
    CHECK_EQ(ss::item(entries[15].item).addr, A_EV_LVR);
    // The new upper word equal to the old lower one: writing the upper register first would
    // leave the two equal for a moment, so the lower one goes first.
    sim.regs[A_EV_LVD] = sim.setVolts(13.25);
    ss::buildPlan(snapshotOf(sim), profile, ss::SCALE_100, entries);
    CHECK_EQ(ss::item(entries[14].item).addr, A_EV_LVD);
    sim.regs[A_EV_LVD] = (uint16_t)(sim.setVolts(13.25) - 1);
    ss::buildPlan(snapshotOf(sim), profile, ss::SCALE_100, entries);
    CHECK_EQ(ss::item(entries[14].item).addr, A_EV_LVR);
  }
  // Whatever an ordered pair holds, and whatever ordered pair it is to hold: the pair is in
  // order after the first of its two writes as well.
  {
    long checked = 0;
    for (uint16_t low = 1000; low <= 1500; low = (uint16_t)(low + 25)) {
      for (uint16_t high = (uint16_t)(low + 5); high <= 1560; high = (uint16_t)(high + 35)) {
        for (uint16_t lvd = 1000; lvd <= 1330; lvd = (uint16_t)(lvd + 55)) {
          for (uint16_t lvr = (uint16_t)(lvd + 10); lvr <= 1360; lvr = (uint16_t)(lvr + 45)) {
            SimSunSaver sim;
            sim.regs[A_EV_LVD] = sim.setVolts(low / 100.0);
            sim.regs[A_EV_LVR] = sim.setVolts(high / 100.0);
            ss::Profile wanted = profile;
            wanted.lvdCv = lvd;
            wanted.lvrCv = lvr;
            ss::PlanEntry entries[ss::PLAN_ENTRIES];
            ss::buildPlan(snapshotOf(sim), wanted, ss::SCALE_100, entries);
            for (uint8_t i = 14; i <= 15; ++i) {
              if (entries[i].status == ss::ENTRY_PENDING) {
                sim.regs[ss::item(entries[i].item).addr] = entries[i].target;
              }
              CHECK_MSG(reg(sim, A_EV_LVD) < reg(sim, A_EV_LVR), "from %u/%u to %u/%u: out of order after write %u",
                        (unsigned)low, (unsigned)high, (unsigned)lvd, (unsigned)lvr, (unsigned)(i - 13));
            }
            CHECK_EQ(reg(sim, A_EV_LVD), sim.setVolts(lvd / 100.0));
            CHECK_EQ(reg(sim, A_EV_LVR), sim.setVolts(lvr / 100.0));
            ++checked;
          }
        }
      }
    }
    CHECK(checked > 1000);
  }
}

// ============================================================================
// A run in which nothing goes wrong
// ============================================================================
static void testQuietRun() {
  const ss::Profile profile = ss::lifepo4Profile();
  {
    Station station;
    const SimSunSaver fresh;
    const uint16_t batteryRaw = reg(station.sim, 0x0008);
    const ss::ProgramResult &result = station.run();

    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK(ss::outcomeIsGood(result.outcome));
    CHECK_EQ(result.restart, ss::RESTART_DONE);
    CHECK_EQ(result.scale, ss::SCALE_100);
    CHECK_EQ(result.liveScale, ss::SCALE_100);
    CHECK_EQ(result.planned, ss::PLAN_ENTRIES);
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_WRITTEN), 15);
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_UNCHANGED), 2);
    CHECK(result.wroteAny);
    CHECK_EQ(result.refusals, 0);
    CHECK_EQ(result.profileErrors, 0);
    // The factory-default controller has switch 1 OFF, and that is all there is to say about it.
    CHECK_EQ(result.notes, ss::NOTE_SW1_OFF);
    CHECK_EQ(result.seen, ss::SEEN_BATTERY | ss::SEEN_LOAD_CURRENT | ss::SEEN_DIP | ss::SEEN_FAULTS);
    CHECK_EQ(result.batteryRaw, batteryRaw);
    CHECK_EQ(result.loadCurrentRaw, SimSunSaver::amps(0.14));
    CHECK_EQ(result.dip, 0x000A);
    CHECK_EQ(result.arrayFault, 0);
    CHECK_EQ(result.loadFault, 0);
    CHECK_EQ(result.loadStateBefore, 1);
    CHECK_EQ(result.loadStateEdit, 1);
    CHECK_EQ(result.loadStateAfter, 1);
    CHECK_EQ(result.probes, 1);  // the controller missed one request while it restarted
    CHECK_EQ(result.profile.lvdCv, 1310);
    CHECK(near(ss::batteryVolts(result), 13.68, 0.005));
    for (uint8_t i = 0; i < ss::PLAN_ENTRIES; ++i) {
      CHECK_EQ(ss::item(result.entries[i].item).addr, kPlanAddrs[i]);
      CHECK_EQ(result.entries[i].before, reg(fresh, kPlanAddrs[i]));
      CHECK_EQ(result.entries[i].target, wantedWord(fresh, kPlanAddrs[i], profile));
      CHECK_EQ(result.entries[i].found, result.entries[i].target);
    }

    // What went over the line, request by request.
    checkSequence(station.bus, quietRun());
    CHECK_EQ(station.bus.log.size(), 108);
    CHECK_EQ(station.bus.count('w'), 15);
    CHECK_EQ(station.bus.count('c'), 2);
    for (size_t i = 0; i < station.bus.log.size(); ++i) {
      // The first request after each restart command finds the controller restarting.
      const bool restarting = i > 0 && station.bus.log[i - 1].kind == 'c';
      CHECK_EQ(station.bus.log[i].status, restarting ? ss::SSIO_NO_REPLY : ss::SSIO_OK);
      if (station.bus.log[i].kind == 'w') {
        CHECK_EQ(station.bus.log[i].value, wantedWord(fresh, station.bus.log[i].addr, profile));
      }
    }
    checkPauses(station);
    CHECK_EQ(station.bus.disorder, 0);
    // The run waited twice, and in this order.
    CHECK_EQ(station.commits.size(), 2);
    CHECK(station.commits.size() == 2 && station.commits[0] == ss::COMMIT_RESTART && station.commits[1] == ss::COMMIT_WRITE);

    // What the controller holds now.
    CHECK(holdsProfile(station.sim, profile));
    CHECK(onlyPlanChanged(station.sim, fresh));
    CHECK(!editPending(station.sim));
    CHECK_EQ(station.sim.resets, 2);
    CHECK_EQ(reg(station.sim, A_V_LVD), station.sim.volts(13.10));  // the new LVD is in effect
    CHECK(!station.programmer.controllerUnknown());
    CHECK(station.programmer.ended());
    CHECK(!station.programmer.resume());

    // The charger check now finds one thing wrong: switch 1, which only a person can set.
    const ss::Expectation want = {ss::CHEMISTRY_LIFEPO4, 12, ss::SCALE_UNKNOWN};
    ss::Evaluation evaluation;
    ss::evaluate(snapshotOf(station.sim), want, &evaluation);
    CHECK_EQ(evaluation.settingsScale.scale, ss::SCALE_100);  // the encoding can still be proven from what was written
    CHECK_EQ(evaluation.findings, ss::FINDING_PRESET_IN_USE);
    CHECK_EQ(evaluation.verdict, ss::VERDICT_MISMATCH);
    station.sim.regs[0x0025] = 0x000B;
    ss::evaluate(snapshotOf(station.sim), want, &evaluation);
    CHECK_EQ(evaluation.findings, 0);
    CHECK_EQ(evaluation.verdict, ss::VERDICT_OK);

    // The same request once more: nothing to do, nothing sent but the reads.
    const size_t had = station.bus.log.size();
    station.commits.clear();
    const ss::ProgramResult &again = station.run();
    CHECK_EQ(again.outcome, ss::OUTCOME_UNCHANGED);
    CHECK(ss::outcomeIsGood(again.outcome));
    CHECK_EQ(again.restart, ss::RESTART_NOT_NEEDED);
    CHECK_EQ(again.planned, ss::PLAN_ENTRIES);
    CHECK_EQ(ss::countEntries(again, ss::ENTRY_UNCHANGED), ss::PLAN_ENTRIES);
    CHECK(!again.wroteAny);
    CHECK_EQ(again.notes, 0);
    CHECK_EQ(station.bus.log.size() - had, ss::ITEM_COUNT);
    CHECK_EQ(station.bus.count('w'), 15);
    CHECK_EQ(station.bus.count('c'), 2);
    CHECK(station.commits.empty());
    CHECK_EQ(station.sim.resets, 2);
  }

  // However many requests a step may make, the run is the same.
  {
    const uint8_t budgets[] = {1, 2, 3, 7, 255};
    for (size_t b = 0; b < sizeof(budgets); ++b) {
      Station station;
      station.budget = budgets[b];
      const ss::ProgramResult &result = station.run();
      CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
      checkSequence(station.bus, quietRun());
      checkPauses(station);
      CHECK(holdsProfile(station.sim, ss::lifepo4Profile()));
    }
  }

  // With the other function code for the reads, as a client whose poll settled on it.
  {
    Station station;
    station.bus.function = MB_FC_READ_HOLDING;
    CHECK_EQ(station.run().outcome, ss::OUTCOME_PROGRAMMED);
    CHECK(holdsProfile(station.sim, profile));
  }

  // A controller that encodes with 96.667 gets other words, and the same voltages.
  {
    Station station(96.667);
    const SimSunSaver fresh(96.667);
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(result.scale, ss::SCALE_96667);
    CHECK(holdsProfile(station.sim, profile));
    CHECK_EQ(reg(station.sim, A_EV_LVD), 4441);
    CHECK_EQ(reg(station.sim, A_EV_REG), 4813);
    CHECK(onlyPlanChanged(station.sim, fresh));
    CHECK_EQ(ss::judgeSettingsScale(snapshotOf(station.sim)).scale, ss::SCALE_96667);
    CHECK_EQ(station.run().outcome, ss::OUTCOME_UNCHANGED);
  }
  // Settings with 96.667 and readings with 100, and the other way round: each is judged by itself.
  {
    Station station(100.0, 96.667);
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(result.scale, ss::SCALE_96667);
    CHECK_EQ(result.liveScale, ss::SCALE_100);
    CHECK_EQ(reg(station.sim, A_EV_LVR), 4491);
  }
  {
    Station station(96.667, 100.0);
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(result.scale, ss::SCALE_100);
    CHECK_EQ(result.liveScale, ss::SCALE_96667);
    CHECK_EQ(reg(station.sim, A_EV_LVR), 4342);
  }

  // Another profile within the limits, on a controller with all its switches set for it.
  {
    Station station;
    station.sim.regs[0x0025] = 0x000B;
    const ss::Profile other = {1440, 1360, 1800, 1470, 1380, 1200, 1280};
    CHECK_EQ(ss::validateProfile(other), 0);
    const ss::ProgramResult &result = station.run(other);
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(result.notes, 0);
    CHECK(holdsProfile(station.sim, other));
    CHECK_EQ(reg(station.sim, A_ET_FLOAT), 1800);
    CHECK_EQ(reg(station.sim, A_ET_FLOATLB2), 1800);
    CHECK_EQ(station.bus.disorder, 0);
    // And from there to the recommended one: thresholds that come down as well as go up.
    const ss::ProgramResult &back = station.run();
    CHECK_EQ(back.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK(holdsProfile(station.sim, profile));
    CHECK_EQ(station.bus.disorder, 0);
    CHECK_EQ(ss::countEntries(back, ss::ENTRY_UNCHANGED), 4);  // the two equalize voltages, the two compensations
  }

  // The notes about the switches.
  {
    Station station;
    station.sim.regs[0x0025] = 0x000C;  // switches 3 and 4
    station.sim.regs[A_V_LVD] = station.sim.volts(11.50);
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(result.notes, ss::NOTE_SW1_OFF | ss::NOTE_SW2_OFF | ss::NOTE_SW3_ON);
    CHECK_EQ(result.dip, 0x000C);
  }
}

// ============================================================================
// A controller that was left waiting for a restart
// ============================================================================
static void checkNothingSent(const Station &station, const SimSunSaver &fresh);

static void testPendingBefore() {
  const ss::Profile profile = ss::lifepo4Profile();
  // It holds the settings already, and still waits for the restart that puts them to work.
  {
    Station station;
    holdProfile(station.sim, profile);
    station.sim.regs[A_ARRAY_FAULT] = EDIT_ARRAY;
    station.sim.regs[A_LOAD_FAULT] = EDIT_LOAD;
    chargeForPending(station.sim);
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(result.restart, ss::RESTART_DONE);
    CHECK_EQ(result.notes, ss::NOTE_SW1_OFF | ss::NOTE_WAS_PENDING);
    CHECK_EQ(result.scale, ss::SCALE_100);
    CHECK_EQ(result.liveScale, ss::SCALE_UNKNOWN);  // the lower battery reading counted
    CHECK(!result.wroteAny);
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_UNCHANGED), ss::PLAN_ENTRIES);
    CHECK_EQ(result.arrayFault, EDIT_ARRAY);
    CHECK_EQ(result.loadFault, EDIT_LOAD);
    CHECK_EQ(result.loadStateEdit, ss::LOAD_STATE_NOT_READ);
    CHECK_EQ(result.loadStateAfter, 1);
    CHECK_EQ(station.bus.count('w'), 0);
    CHECK_EQ(station.bus.count('c'), 1);
    CHECK_EQ(station.sim.resets, 1);
    CHECK(!editPending(station.sim));
    // It waited once, before the restart command, and never to write.
    CHECK_EQ(station.commits.size(), 1);
    CHECK(station.commits.size() == 1 && station.commits[0] == ss::COMMIT_RESTART);
    // 49 reads, the command, a request the restarting controller misses, the two fault words,
    // the 17 registers again, the load state.
    CHECK_EQ(station.bus.log.size(), ss::ITEM_COUNT + 1 + 1 + 2 + ss::PLAN_ENTRIES + 1);
    CHECK(!station.programmer.controllerUnknown());
  }
  // Only one of the two fault words says so: that is enough.
  for (int which = 0; which < 2; ++which) {
    Station station;
    holdProfile(station.sim, profile);
    station.sim.regs[which == 0 ? A_ARRAY_FAULT : A_LOAD_FAULT] = which == 0 ? EDIT_ARRAY : EDIT_LOAD;
    chargeForPending(station.sim);
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(result.notes & ss::NOTE_WAS_PENDING, ss::NOTE_WAS_PENDING);
    CHECK_EQ(station.sim.resets, 1);
  }
  // It waits, and holds other settings: restarted, written, restarted.
  {
    Station station;
    station.sim.regs[A_ARRAY_FAULT] = EDIT_ARRAY;
    station.sim.regs[A_LOAD_FAULT] = EDIT_LOAD;
    chargeForPending(station.sim);
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(result.notes, ss::NOTE_SW1_OFF | ss::NOTE_WAS_PENDING);
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_WRITTEN), 15);
    CHECK_EQ(station.sim.resets, 2);
    CHECK(holdsProfile(station.sim, profile));
    checkSequence(station.bus, quietRun());
  }
  // It waits, and the restart command is acknowledged without doing anything: nothing is written.
  {
    Station station;
    SimSunSaver fresh;
    chargeForPending(fresh);
    station.sim.regs[A_ARRAY_FAULT] = EDIT_ARRAY;
    station.sim.regs[A_LOAD_FAULT] = EDIT_LOAD;
    chargeForPending(station.sim);
    station.sim.resetWorks = false;
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_REFUSED);
    CHECK_EQ(result.refusals, ss::REFUSE_NO_RESTART);
    CHECK_EQ(result.restart, ss::RESTART_NOT_DONE);
    CHECK(!result.wroteAny);
    CHECK_EQ(station.bus.count('w'), 0);
    CHECK_EQ(station.bus.count('c'), ss::RESTART_ATTEMPTS);
    CHECK(onlyPlanChanged(station.sim, fresh));
    CHECK(beforeOrTarget(station.sim, fresh, profile) && !holdsProfile(station.sim, profile));
    CHECK(!station.programmer.controllerUnknown());
    checkPauses(station);
  }
  // It waits, and the battery is above the new LVR with one scale only. The stored LVD may not
  // be the one in effect, so it proves no scale: the lower reading counts, and nothing is sent,
  // not even the restart the controller waits for.
  {
    Station station;
    holdProfile(station.sim, profile);
    station.sim.regs[A_ARRAY_FAULT] = EDIT_ARRAY;
    SimSunSaver same;
    holdProfile(same, profile);
    same.regs[A_ARRAY_FAULT] = EDIT_ARRAY;
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_REFUSED);
    CHECK_EQ(result.refusals, ss::REFUSE_BATTERY_LOW);
    CHECK_EQ(result.liveScale, ss::SCALE_UNKNOWN);
    CHECK_EQ(result.notes & ss::NOTE_WAS_PENDING, ss::NOTE_WAS_PENDING);
    checkNothingSent(station, same);
    CHECK(editPending(station.sim));
    const std::string text = textFrom([&](ss::Text *t) { ss::describeRefusal(ss::REFUSE_BATTERY_LOW, result, t); });
    CHECK_HAS(text, "(battery 13.23 V at the lower of its two possible readings, new LVR 13.25 V)");
    // Without the edit the same battery reads 13.68 V, and the run goes ahead.
    Station settled;
    holdProfile(settled.sim, profile);
    const ss::ProgramResult &done = settled.run();
    CHECK_EQ(done.outcome, ss::OUTCOME_UNCHANGED);
    CHECK_EQ(done.liveScale, ss::SCALE_100);
  }
}

// ============================================================================
// Runs that are refused: nothing is sent but reads
// ============================================================================

// A refused run has sent no write and no restart command, and has not waited for a go.
static void checkNothingSent(const Station &station, const SimSunSaver &fresh) {
  CHECK_EQ(station.bus.count('w'), 0);
  CHECK_EQ(station.bus.count('c'), 0);
  CHECK(station.commits.empty());
  CHECK_EQ(station.sim.resets, 0);
  CHECK(!station.programmer.result().wroteAny);
  CHECK(!station.programmer.controllerUnknown());
  for (uint8_t i = 0; i < ss::PLAN_ENTRIES; ++i) {
    uint16_t now = 0;
    uint16_t was = 0;
    if (wordAt(station.sim, kPlanAddrs[i], &now) && wordAt(fresh, kPlanAddrs[i], &was)) {
      CHECK_EQ(now, was);
    }
  }
}

static void testRefusals() {
  const ss::Profile profile = ss::lifepo4Profile();
  const SimSunSaver fresh;

  // Current flows from the LOAD terminals, and nobody said that this is all right.
  {
    Station station;
    const ss::ProgramResult &result = station.run(profile, false);
    CHECK_EQ(result.outcome, ss::OUTCOME_REFUSED);
    CHECK(!ss::outcomeIsGood(result.outcome));
    CHECK_EQ(result.refusals, ss::REFUSE_LOAD_CURRENT);
    CHECK_EQ(result.planned, 0);
    CHECK_EQ(result.restart, ss::RESTART_NOT_NEEDED);
    CHECK_EQ(station.bus.log.size(), ss::ITEM_COUNT);
    checkNothingSent(station, fresh);
    CHECK_HAS(textFrom([&](ss::Text *t) { ss::describeRefusal(ss::REFUSE_LOAD_CURRENT, result, t); }), "(0.14 A)");
  }
  // The limit is 0.05 A: 20 counts are below it, 21 above. A reading below zero is noise.
  {
    const struct {
      uint16_t raw;
      bool refused;
    } cases[] = {{0, false}, {17, false}, {20, false}, {21, true}, {25, true}, {0x7FFF, true}, {65528, false}, {0x8000, false}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      Station station;
      station.sim.regs[0x000C] = cases[i].raw;
      const ss::ProgramResult &result = station.run(profile, false);
      CHECK_MSG((result.refusals == ss::REFUSE_LOAD_CURRENT) == cases[i].refused, "load current word %u", (unsigned)cases[i].raw);
      CHECK_EQ(result.outcome, cases[i].refused ? ss::OUTCOME_REFUSED : ss::OUTCOME_PROGRAMMED);
      CHECK_EQ(result.loadCurrentRaw, cases[i].raw);
    }
  }

  // The battery is below the new LVR.
  {
    Station station;
    station.sim.regs[0x0008] = station.sim.volts(13.24);
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_REFUSED);
    CHECK_EQ(result.refusals, ss::REFUSE_BATTERY_LOW);
    checkNothingSent(station, fresh);
    const std::string text = textFrom([&](ss::Text *t) { ss::describeRefusal(ss::REFUSE_BATTERY_LOW, result, t); });
    CHECK_HAS(text, "(battery 13.24 V, new LVR 13.25 V)");
  }
  {
    // At the LVR itself it is not below it.
    Station station;
    station.sim.regs[0x0008] = station.sim.volts(13.25);
    CHECK_EQ(station.run().outcome, ss::OUTCOME_PROGRAMMED);
    // And a lower LVR in the request lets a lower battery through.
    Station lower;
    lower.sim.regs[0x0008] = lower.sim.volts(13.24);
    ss::Profile wanted = profile;
    wanted.lvrCv = 1320;
    CHECK_EQ(lower.run(wanted).outcome, ss::OUTCOME_PROGRAMMED);
  }
  {
    // How the controller encodes what it measures is not known here (it reports no LVD in
    // effect), so the reading could be 3.3 % lower than it looks: the lower one counts.
    Station station;
    station.sim.regs[A_V_LVD] = 0;
    station.sim.regs[0x0008] = station.sim.volts(13.70);  // 13.24 V if this controller measures with 96.667
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_REFUSED);
    CHECK_EQ(result.refusals, ss::REFUSE_BATTERY_LOW);
    CHECK_EQ(result.liveScale, ss::SCALE_UNKNOWN);
    CHECK_EQ(result.scale, ss::SCALE_100);
    checkNothingSent(station, fresh);
    const std::string text = textFrom([&](ss::Text *t) { ss::describeRefusal(ss::REFUSE_BATTERY_LOW, result, t); });
    CHECK_HAS(text, "(battery 13.24 V at the lower of its two possible readings, new LVR 13.25 V)");
    Station enough;
    enough.sim.regs[A_V_LVD] = 0;
    enough.sim.regs[0x0008] = enough.sim.volts(13.71);  // 13.25 V at the least
    CHECK_EQ(enough.run().outcome, ss::OUTCOME_PROGRAMMED);
  }

  // Not a 12 V system, or no battery voltage that makes sense.
  {
    const double volts[] = {27.2, 17.5, 5.0, 0.0};
    for (size_t i = 0; i < sizeof(volts) / sizeof(volts[0]); ++i) {
      Station station;
      station.sim.regs[0x0008] = station.sim.volts(volts[i]);
      const ss::ProgramResult &result = station.run();
      CHECK_EQ(result.outcome, ss::OUTCOME_REFUSED);
      CHECK_MSG((result.refusals & ss::REFUSE_NOT_12V) != 0, "battery at %.1f V", volts[i]);
      CHECK_EQ(result.refusals & ss::REFUSE_BATTERY_LOW, 0);  // one reason for one thing
      checkNothingSent(station, fresh);
    }
  }

  // The controller reports a fault. The pending edit alone is none (see testPendingBefore).
  {
    const struct {
      uint16_t addr;
      uint16_t word;
    } cases[] = {{A_ARRAY_FAULT, 0x0001}, {A_ARRAY_FAULT, 0x0400}, {A_LOAD_FAULT, 0x0002},
                 {A_LOAD_FAULT, 0x0080},  {A_ARRAY_FAULT, (uint16_t)(EDIT_ARRAY | 0x0010)}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      Station station;
      SimSunSaver same;
      station.sim.regs[cases[i].addr] = cases[i].word;
      same.regs[cases[i].addr] = cases[i].word;
      chargeForPending(station.sim);  // the case with the edit leaves the live scale unproven
      chargeForPending(same);
      const ss::ProgramResult &result = station.run();
      CHECK_EQ(result.outcome, ss::OUTCOME_REFUSED);
      CHECK_EQ(result.refusals, ss::REFUSE_FAULT);
      checkNothingSent(station, same);
    }
    Station station;
    station.sim.regs[A_ARRAY_FAULT] = 0x0001;
    station.sim.regs[A_LOAD_FAULT] = 0x0082;
    const ss::ProgramResult &result = station.run();
    const std::string text = textFrom([&](ss::Text *t) { ss::describeRefusal(ss::REFUSE_FAULT, result, t); });
    CHECK_HAS(text, "(array faults 0x0001, load faults 0x0082)");
  }

  // The encoding of the stored voltages cannot be proven: no voltage is written, and nothing else either.
  {
    Station station(100.0, 98.3);  // setpoints that are round with neither scale
    const SimSunSaver same(100.0, 98.3);
    station.sim.regs[0x0008] = station.sim.volts(14.30);
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_REFUSED);
    CHECK_EQ(result.refusals, ss::REFUSE_SCALE_UNPROVEN);
    CHECK_EQ(result.scale, ss::SCALE_UNKNOWN);
    checkNothingSent(station, same);
    // With it goes the proof of how the battery voltage is encoded (the stored LVD is the
    // yardstick), so a battery that is not clearly above the new LVR is named as well.
    Station lower(100.0, 98.3);
    CHECK_EQ(lower.run().refusals, ss::REFUSE_SCALE_UNPROVEN | ss::REFUSE_BATTERY_LOW);
  }

  // A register the run needs gives no value: the controller refuses it, or stays silent.
  {
    const uint16_t needed[] = {A_ER_ICOMP, A_EV_LVD, A_EV_EQ2, 0x0025, 0x000C, A_ARRAY_FAULT, A_LOAD_FAULT};
    for (size_t i = 0; i < sizeof(needed) / sizeof(needed[0]); ++i) {
      Station station;
      station.sim.regs[0x0008] = station.sim.volts(14.30);  // high enough with either scale
      station.sim.regs.erase(needed[i]);
      const ss::ProgramResult &result = station.run();
      CHECK_EQ(result.outcome, ss::OUTCOME_REFUSED);
      CHECK_MSG(result.refusals == ss::REFUSE_NOT_READ, "without 0x%04X the refusals are 0x%lX", needed[i],
                (unsigned long)result.refusals);
      checkNothingSent(station, fresh);
    }
    Station silent;
    silent.bus.before = [&](size_t, char, uint16_t addr) {
      if (addr == A_EV_LVR) {
        silent.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
      }
    };
    const ss::ProgramResult &result = silent.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_REFUSED);
    CHECK_EQ(result.refusals, ss::REFUSE_NOT_READ);
    CHECK_EQ(silent.bus.count('r', A_EV_LVR), 2);
    checkNothingSent(silent, fresh);
    checkPauses(silent);
  }
  {
    // Without the battery voltage there is neither a system voltage nor a level to compare.
    Station station;
    station.sim.regs.erase(0x0008);
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.refusals, ss::REFUSE_NOT_READ);
    CHECK_EQ(result.seen & ss::SEEN_BATTERY, 0);
  }
  {
    // A register the run does not rely on may be missing, as on a controller with older firmware.
    Station station;
    station.sim.regs.erase(0xE030);  // an LED threshold
    station.sim.regs.erase(0x000E);  // the battery temperature
    station.sim.regs.erase(A_LOAD_STATE);
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(result.loadStateBefore, ss::LOAD_STATE_NOT_READ);
    CHECK_EQ(result.loadStateEdit, ss::LOAD_STATE_NOT_READ);
    CHECK_EQ(result.loadStateAfter, ss::LOAD_STATE_NOT_READ);
  }

  // Values outside the limits.
  {
    Station station;
    ss::Profile wanted = profile;
    wanted.lvdCv = 900;
    wanted.absorbCv = 1480;
    const ss::ProgramResult &result = station.run(wanted);
    CHECK_EQ(result.outcome, ss::OUTCOME_REFUSED);
    CHECK_EQ(result.refusals, ss::REFUSE_PROFILE);
    CHECK_EQ(result.profileErrors, ss::PROFILE_LVD | ss::PROFILE_ABSORB | ss::PROFILE_HVD);
    CHECK_EQ(result.profile.lvdCv, 900);
    checkNothingSent(station, fresh);
  }

  // Several reasons at once are all named.
  {
    Station station;
    station.sim.regs[0x0008] = station.sim.volts(12.90);
    station.sim.regs[A_LOAD_FAULT] = 0x0001;
    ss::Profile wanted = profile;
    wanted.absorbSeconds = 5;
    const ss::ProgramResult &result = station.run(wanted, false);
    CHECK_EQ(result.refusals,
             ss::REFUSE_PROFILE | ss::REFUSE_BATTERY_LOW | ss::REFUSE_LOAD_CURRENT | ss::REFUSE_FAULT);
    CHECK_EQ(result.profileErrors, ss::PROFILE_TIME);
  }

  // The controller does not answer at all, or stops answering part way through the reading.
  {
    Station station;
    station.sim.powered = false;
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_NO_LINK);
    CHECK(!ss::outcomeIsGood(result.outcome));
    CHECK_EQ(result.refusals, 0);
    CHECK_EQ(result.planned, 0);
    CHECK_EQ(result.seen, 0);
    CHECK_EQ(station.bus.log.size(), ss::SnapshotReader::MISSES_PER_PASS + 1);
    checkPauses(station);
    station.sim.powered = true;
    checkNothingSent(station, fresh);
  }
  {
    Station station;
    station.bus.before = [&](size_t index, char, uint16_t) { station.sim.powered = index < 30; };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_NO_LINK);
    station.sim.powered = true;
    checkNothingSent(station, fresh);
  }

  // The caller cannot record the run: it ends there. Before the restart command nothing was sent.
  {
    Station station;
    station.refuseAt = ss::COMMIT_RESTART;
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_REFUSED);
    CHECK_EQ(result.refusals, ss::REFUSE_JOURNAL);
    CHECK_EQ(result.restart, ss::RESTART_NOT_NEEDED);
    CHECK_EQ(result.planned, ss::PLAN_ENTRIES);
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_PENDING), 15);
    CHECK_EQ(station.bus.count('c'), 0);
    CHECK_EQ(station.bus.count('w'), 0);
    CHECK_EQ(station.commits.size(), 1);
    CHECK(!station.programmer.controllerUnknown());
  }
  // Before the first write the controller has restarted once, and holds what it held.
  {
    Station station;
    station.refuseAt = ss::COMMIT_WRITE;
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_REFUSED);
    CHECK_EQ(result.refusals, ss::REFUSE_JOURNAL);
    CHECK_EQ(result.restart, ss::RESTART_DONE);
    CHECK(!result.wroteAny);
    CHECK_EQ(station.bus.count('c'), 1);
    CHECK_EQ(station.bus.count('w'), 0);
    CHECK_EQ(station.sim.resets, 1);
    CHECK_EQ(station.commits.size(), 2);
    CHECK(onlyPlanChanged(station.sim, fresh) && !holdsProfile(station.sim, profile));
    CHECK(!station.programmer.controllerUnknown());
    CHECK(!station.programmer.resume());
  }
}

// ============================================================================
// The restart before the writes
// ============================================================================
static void testFirstRestart() {
  const ss::Profile profile = ss::lifepo4Profile();
  const SimSunSaver fresh;

  // A controller that refuses the command is not written to.
  {
    Station station;
    station.sim.coils.erase(A_COIL_RESTART);
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_REFUSED);
    CHECK_EQ(result.refusals, ss::REFUSE_NO_RESTART);
    CHECK_EQ(result.restart, ss::RESTART_REFUSED);
    CHECK(!result.wroteAny);
    CHECK_EQ(result.planned, ss::PLAN_ENTRIES);
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_PENDING), 15);
    CHECK_EQ(station.bus.count('w'), 0);
    CHECK_EQ(station.bus.count('c'), 1);
    CHECK_EQ(station.bus.log.size(), ss::ITEM_COUNT + 1);  // nothing is asked after the refusal
    CHECK_EQ(station.bus.log.back().status, ss::SSIO_REFUSED);
    CHECK_EQ(station.commits.size(), 1);
    CHECK(onlyPlanChanged(station.sim, fresh) && beforeOrTarget(station.sim, fresh, profile));
    CHECK(!holdsProfile(station.sim, profile));
    CHECK(!station.programmer.controllerUnknown());
    CHECK(!station.programmer.resume());
  }

  // It restarts without acknowledging the command: that is no refusal.
  {
    Station station;
    station.sim.resetAcknowledged = false;
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(station.sim.resets, 2);
    CHECK_EQ(station.bus.log[ss::ITEM_COUNT].kind, 'c');
    CHECK_EQ(station.bus.log[ss::ITEM_COUNT].status, ss::SSIO_NO_REPLY);
    checkPauses(station);
  }

  // It takes longer to come back than the run waits at first: asked until it answers.
  {
    Station station;
    station.sim.resetDownMs = 9000;
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(result.restart, ss::RESTART_DONE);
    CHECK(result.probes >= 4 && result.probes <= 5);  // those of the last restart: 9 s, about 2 s apart
    CHECK_EQ(station.sim.resets, 2);
    checkPauses(station);
    // The requests it missed were for the array fault word, one per step.
    const size_t command = station.bus.nth('c', 0);
    size_t missed = 0;
    for (size_t i = command + 1; station.bus.log[i].status == ss::SSIO_NO_REPLY; ++i) {
      CHECK_EQ(station.bus.log[i].kind, 'r');
      CHECK_EQ(station.bus.log[i].addr, A_ARRAY_FAULT);
      ++missed;
    }
    CHECK(missed >= 4 && missed <= 5);
  }
  // After each probe the restarting controller missed, the run asks for a pause of its own, so
  // that a client whose rest after a miss is shorter than PROBE_GAP_MS still leaves it that long.
  {
    Station station;
    station.sim.resetDownMs = 9000;
    station.programmer.start(ss::lifepo4Profile(), true);
    unsigned missedProbes = 0;
    for (unsigned guard = 0; guard < 4000 && station.programmer.active(); ++guard) {
      const size_t had = station.bus.log.size();
      station.programmer.step(station.bus, station.budget);
      if (station.programmer.awaiting() != ss::COMMIT_NONE) {
        station.programmer.go();
        continue;
      }
      if (station.bus.log.size() > had && station.bus.count('c') > 0 && station.bus.log.back().kind == 'r' &&
          station.bus.log.back().addr == A_ARRAY_FAULT && station.bus.log.back().status == ss::SSIO_NO_REPLY) {
        ++missedProbes;
        CHECK_EQ(station.programmer.pauseMs(), ss::PROBE_GAP_MS);
      }
      const uint32_t rest = station.programmer.missed() ? station.master.responseTimeoutMs : station.gapMs;
      station.sim.advance(station.programmer.pauseMs() > rest ? station.programmer.pauseMs() : rest);
    }
    CHECK(!station.programmer.active());
    CHECK_EQ(station.programmer.result().outcome, ss::OUTCOME_PROGRAMMED);
    CHECK(missedProbes >= 4);  // two restarts, each with at least two
  }

  // It never comes back from the command. Nothing is written, and the run owes it nothing. (A
  // short timeout, so that BACK_PROBES ends the wait and not the time limit: see testTimeLimit().)
  {
    Station station;
    station.master.responseTimeoutMs = 300;
    station.bus.before = [&](size_t, char kind, uint16_t) {
      if (kind == 'c') {
        station.sim.resetDownMs = 3600UL * 1000UL;
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_NO_LINK);
    CHECK_EQ(result.restart, ss::RESTART_NO_ANSWER);
    CHECK_EQ(result.refusals, 0);
    CHECK_EQ(result.probes, ss::BACK_PROBES);
    CHECK_EQ(result.notes & ss::NOTE_TIME_LIMIT, 0);
    CHECK(!result.wroteAny);
    CHECK_EQ(station.bus.count('w'), 0);
    CHECK_EQ(station.bus.count('c'), 1);
    CHECK_EQ(station.bus.log.size(), ss::ITEM_COUNT + 1 + ss::BACK_PROBES);
    CHECK_EQ(station.commits.size(), 1);
    CHECK(!station.programmer.controllerUnknown());
    CHECK(!station.programmer.resume());
    checkPauses(station);
  }

  // It comes back, and its fault words cannot be read: the run does not know that it is ready.
  {
    const uint16_t gone[] = {A_ARRAY_FAULT, A_LOAD_FAULT};
    for (int which = 0; which < 2; ++which) {
      Station station;
      station.bus.before = [&](size_t, char kind, uint16_t) {
        if (kind == 'r' && station.bus.count('c') == 1) {
          station.sim.regs.erase(gone[which]);
        }
      };
      const ss::ProgramResult &result = station.run();
      CHECK_EQ(result.outcome, ss::OUTCOME_NO_LINK);
      CHECK_EQ(result.restart, ss::RESTART_UNCONFIRMED);
      CHECK(!result.wroteAny);
      CHECK_EQ(station.bus.count('w'), 0);
      CHECK(!station.programmer.controllerUnknown());
    }
    // The load fault word stays silent twice, and answers on no third request.
    Station station;
    station.bus.before = [&](size_t index, char kind, uint16_t addr) {
      if (index > ss::ITEM_COUNT && kind == 'r' && addr == A_LOAD_FAULT) {
        station.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_NO_LINK);
    CHECK_EQ(result.restart, ss::RESTART_UNCONFIRMED);
    // The command, the probe it missed while it restarted, the array fault word, and the load fault word twice.
    CHECK_EQ(station.bus.log.size(), ss::ITEM_COUNT + 1 + 1 + 1 + ss::READ_ATTEMPTS);
    CHECK_EQ(station.bus.count('w'), 0);
  }

  // The command is acknowledged and nothing restarts. With no edit pending, only a controller
  // that goes away for a moment shows that it restarted: this one goes on answering, so nothing
  // shows that the command works, and nothing is written.
  {
    Station station;
    station.sim.resetWorks = false;
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_REFUSED);
    CHECK_EQ(result.refusals, ss::REFUSE_NO_RESTART);
    CHECK_EQ(result.restart, ss::RESTART_UNPROVEN);
    CHECK(!result.wroteAny);
    CHECK_EQ(station.bus.count('w'), 0);
    CHECK_EQ(station.bus.count('c'), 1);
    CHECK_EQ(station.sim.resets, 0);
    CHECK(onlyPlanChanged(station.sim, fresh));
    CHECK(!editPending(station.sim));
    // Asked QUIET_PROBES times, PROBE_GAP_MS apart, as a restart may not begin at once; then
    // its load fault word.
    const size_t command = station.bus.nth('c', 0);
    CHECK_EQ(station.bus.log.size() - command, 1 + ss::QUIET_PROBES + 1);
    for (size_t i = command + 1; i < station.bus.log.size(); ++i) {
      const bool probe = i + 1 < station.bus.log.size();
      CHECK_EQ(station.bus.log[i].status, ss::SSIO_OK);
      CHECK_EQ(station.bus.log[i].addr, probe ? A_ARRAY_FAULT : A_LOAD_FAULT);
      if (probe && i > command + 1) {
        CHECK(station.bus.log[i].at - station.bus.log[i - 1].end >= ss::PROBE_GAP_MS);
      }
    }
    CHECK(!station.programmer.controllerUnknown());
    CHECK(!station.programmer.resume());
    checkPauses(station);
  }
  // The same controller with an edit pending: that it is still pending shows the command did
  // nothing. Asked twice, refused, and nothing written.
  {
    Station station;
    station.sim.resetWorks = false;
    station.sim.regs[A_LOAD_FAULT] = EDIT_LOAD;
    chargeForPending(station.sim);
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_REFUSED);
    CHECK_EQ(result.refusals, ss::REFUSE_NO_RESTART);
    CHECK_EQ(result.restart, ss::RESTART_NOT_DONE);
    CHECK_EQ(station.bus.count('c'), ss::RESTART_ATTEMPTS);
    CHECK_EQ(station.bus.count('w'), 0);
    CHECK(editPending(station.sim));
  }
  // A controller that restarts without missing a request: the pending edit it cleared shows the
  // restart. It is asked a few seconds first, as it would be if its restart were yet to come.
  {
    Station station;
    station.sim.resetDownMs = 0;
    station.sim.regs[A_ARRAY_FAULT] = EDIT_ARRAY;
    chargeForPending(station.sim);
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(result.restart, ss::RESTART_DONE);
    CHECK_EQ(station.sim.resets, 2);
    const size_t command = station.bus.nth('c', 0);
    for (size_t i = 1; i <= ss::QUIET_PROBES; ++i) {
      CHECK_EQ(station.bus.log[command + i].status, ss::SSIO_OK);
      CHECK_EQ(station.bus.log[command + i].addr, A_ARRAY_FAULT);
    }
    CHECK_EQ(station.bus.log[command + ss::QUIET_PROBES + 1].addr, A_LOAD_FAULT);
    CHECK_EQ(station.bus.log[command + ss::QUIET_PROBES + 2].kind, 'w');
  }
  // One that restarts a moment after the command: it answers the first looks, then misses one
  // while it restarts. The miss shows the restart.
  {
    Station station;
    station.bus.before = [&](size_t index, char kind, uint16_t) {
      if (kind == 'c') {
        station.sim.resetWorks = station.bus.count('c') > 0;  // the first one takes effect late: below
      }
      if (station.bus.count('c') == 1 && index == station.bus.nth('c', 0) + 4) {
        station.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(result.restart, ss::RESTART_DONE);
    const size_t command = station.bus.nth('c', 0);
    for (size_t i = command + 1; i < command + 4; ++i) {
      CHECK_EQ(station.bus.log[i].status, ss::SSIO_OK);
    }
    CHECK_EQ(station.bus.log[command + 4].status, ss::SSIO_NO_REPLY);
    CHECK_EQ(station.bus.log[command + 5].addr, A_ARRAY_FAULT);  // asked again until it is back
    CHECK_EQ(station.bus.log[command + 6].addr, A_LOAD_FAULT);
    CHECK_EQ(station.bus.log[command + 7].kind, 'w');
  }
  // The first restart is shown, the second is not: the controller goes on answering after it,
  // and reports no pending edit before or after (it never does). The values are written, and
  // nothing shows they are in effect.
  {
    Station station;
    station.bus.before = [&](size_t, char kind, uint16_t) {
      station.sim.regs[A_ARRAY_FAULT] = 0;
      station.sim.regs[A_LOAD_FAULT] = 0;
      if (kind == 'c' && station.bus.count('c') == 1) {
        station.sim.resetWorks = false;
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_RESTART_FAILED);
    CHECK_EQ(result.restart, ss::RESTART_UNPROVEN);
    CHECK(!ss::outcomeIsGood(result.outcome));
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_WRITTEN), 15);
    CHECK_EQ(station.bus.count('c'), 2);  // a pending edit it could clear was never seen: one command
    CHECK_EQ(station.sim.resets, 1);
    CHECK(!station.programmer.controllerUnknown());
    checkPauses(station);
  }
  // The pending edit is not seen while the controller waits (its load fault word is silent then),
  // and the last restart command does nothing: the controller answers on, and then reports the
  // edit. That shows what the second command has to clear, and its clearing shows the restart,
  // although this controller restarts without missing a request.
  {
    Station station;
    station.bus.before = [&](size_t, char kind, uint16_t addr) {
      if (kind == 'r' && addr == A_LOAD_FAULT && station.bus.count('w') > 0 && station.bus.count('c') == 1) {
        station.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
      }
      if (kind == 'c' && station.bus.count('c') == 1) {
        station.sim.resetWorks = false;
      } else if (kind == 'c' && station.bus.count('c') == 2) {
        station.sim.resetWorks = true;
        station.sim.resetDownMs = 0;
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(result.restart, ss::RESTART_DONE);
    CHECK_EQ(station.bus.count('c'), 3);
    CHECK_EQ(station.sim.resets, 2);
    CHECK(!editPending(station.sim));
    for (size_t i = station.bus.nth('c', 2) + 1; i < station.bus.log.size(); ++i) {
      CHECK_EQ(station.bus.log[i].status, ss::SSIO_OK);  // it missed nothing after the last command
    }
  }
}

// ============================================================================
// The writes
// ============================================================================

// A hook that injects `fault` into the request with index `at`.
static void failAt(Station &station, size_t at, SimSunSaver::Fault fault, unsigned times = 1) {
  station.bus.before = [&station, at, fault, times](size_t index, char, uint16_t) {
    if (index == at) {
      station.sim.failNext(fault, times);
    }
  };
}

static void testWrites() {
  const ss::Profile profile = ss::lifepo4Profile();
  const SimSunSaver fresh;
  const Sequence quiet = quietRun();
  const size_t firstWrite = ss::ITEM_COUNT + 4;  // after the reads, the command, a missed probe and the two fault words
  CHECK_EQ(quiet[firstWrite].first, 'w');
  CHECK_EQ(quiet[firstWrite].second, A_EV_EQ2);

  // The acknowledgement of a write is lost, or damaged, on its way back. The register holds the
  // value, the read says so, and it is not written again.
  {
    const SimSunSaver::Fault faults[] = {SimSunSaver::FAULT_LOST_REPLY, SimSunSaver::FAULT_WRONG_ECHO,
                                         SimSunSaver::FAULT_BAD_CRC, SimSunSaver::FAULT_TRUNCATE,
                                         SimSunSaver::FAULT_GARBAGE};
    for (size_t f = 0; f < sizeof(faults) / sizeof(faults[0]); ++f) {
      Station station;
      failAt(station, firstWrite + 4, faults[f]);  // the write of EV_hvd
      const ss::ProgramResult &result = station.run();
      CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
      CHECK_EQ(station.bus.log[firstWrite + 4].status, ss::SSIO_NO_REPLY);
      CHECK_EQ(station.bus.count('w', A_EV_HVD), 1);
      CHECK_EQ(station.bus.count('w'), 15);
      CHECK_EQ(statusOf(result, A_EV_HVD), ss::ENTRY_WRITTEN);
      checkPauses(station);
      CHECK_EQ(station.bus.disorder, 0);
    }
  }
  // The write itself is lost. The register reads what it held, and is written once more.
  {
    Station station;
    failAt(station, firstWrite + 4, SimSunSaver::FAULT_NO_REPLY);
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(station.bus.count('w', A_EV_HVD), 2);
    CHECK_EQ(station.bus.count('w'), 16);
    CHECK_EQ(statusOf(result, A_EV_HVD), ss::ENTRY_WRITTEN);
    CHECK(holdsProfile(station.sim, profile));
    checkPauses(station);
  }
  // It is lost twice: the run stops there, writes nothing after it, and still restarts the controller.
  {
    Station station;
    station.bus.before = [&](size_t, char kind, uint16_t addr) {
      if (kind == 'w' && addr == A_EV_HVD) {
        station.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_STOPPED);
    CHECK(!ss::outcomeIsGood(result.outcome));
    CHECK_EQ(result.restart, ss::RESTART_DONE);
    CHECK_EQ(statusOf(result, A_EV_HVD), ss::ENTRY_NOT_TAKEN);
    CHECK_EQ(entryFor(result, A_EV_HVD)->found, reg(fresh, A_EV_HVD));
    CHECK_EQ(station.bus.count('w', A_EV_HVD), ss::WRITE_ATTEMPTS);
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_WRITTEN), 2);    // EV_eq2 and EV_tempcomp
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_UNCHANGED), 2);
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_PENDING), 12);   // everything after it
    CHECK_EQ(station.bus.count('w'), 2 + ss::WRITE_ATTEMPTS);
    CHECK_EQ(station.bus.count('c'), 2);
    CHECK_EQ(station.sim.resets, 2);
    CHECK(!editPending(station.sim));
    CHECK(onlyPlanChanged(station.sim, fresh) && beforeOrTarget(station.sim, fresh, profile));
    CHECK_EQ(reg(station.sim, A_EV_EQ2), 0);
    CHECK_EQ(reg(station.sim, A_EV_LVD), reg(fresh, A_EV_LVD));
    CHECK_EQ(station.bus.disorder, 0);
    CHECK(!station.programmer.controllerUnknown());
    // The registers that were written are read once more after the restart, the others are not.
    const size_t lastCommand = station.bus.nth('c', 1);
    CHECK_EQ(station.bus.log.size() - lastCommand, 1 + 1 + 2 + 4 + 1);
    checkPauses(station);
  }
  // The controller refuses to store anything.
  {
    Station station;
    station.sim.refuseWrites = true;
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_STOPPED);
    CHECK_EQ(statusOf(result, A_EV_EQ2), ss::ENTRY_REFUSED);
    CHECK_EQ(station.bus.count('w'), 1);  // refused is refused: not asked again
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_PENDING), 14);
    CHECK(result.wroteAny);
    CHECK_EQ(result.restart, ss::RESTART_DONE);
    CHECK_EQ(station.sim.resets, 2);
    CHECK(onlyPlanChanged(station.sim, fresh));
    CHECK(!holdsProfile(station.sim, profile));
    // It refused the write and reports no pending edit, which the note says.
    CHECK_EQ(result.notes, ss::NOTE_SW1_OFF | ss::NOTE_EDIT_FAULT_NOT_SEEN);
  }
  // It refuses, and holds the value all the same: the read decides.
  {
    Station station;
    station.bus.before = [&](size_t, char kind, uint16_t addr) {
      if (kind == 'w' && addr == A_EV_EQ2) {
        station.sim.refuseWrites = true;
        station.sim.regs[A_EV_EQ2] = 0;
      } else {
        station.sim.refuseWrites = false;
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(statusOf(result, A_EV_EQ2), ss::ENTRY_WRITTEN);
    CHECK_EQ(station.bus.log[firstWrite].status, ss::SSIO_REFUSED);
  }
  // It stores another word than it was given.
  {
    Station station;
    station.sim.storeMask = 0xFFF0;
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_STOPPED);
    // The two zeroes pass the mask. EV_hvd (4735 = 0x127F) does not.
    CHECK_EQ(statusOf(result, A_EV_EQ2), ss::ENTRY_WRITTEN);
    CHECK_EQ(statusOf(result, A_EV_TEMPCOMP), ss::ENTRY_WRITTEN);
    CHECK_EQ(statusOf(result, A_EV_HVD), ss::ENTRY_ALTERED);
    CHECK_EQ(entryFor(result, A_EV_HVD)->found, 0x1270);
    CHECK_EQ(station.bus.count('w', A_EV_HVD), 1);  // not written again: it is not a lost write
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_PENDING), 12);
    CHECK_EQ(result.restart, ss::RESTART_DONE);
    CHECK(!editPending(station.sim));
    CHECK_HAS(textFrom([&](ss::Text *t) { ss::describeEntry(*entryFor(result, A_EV_HVD), result.scale, t); }),
              "EV_hvd: 15.20 V -> 14.45 V, altered by the charger: it holds 14.40 V");
  }
  // The register cannot be read after its write.
  {
    Station station;
    station.bus.before = [&](size_t index, char kind, uint16_t addr) {
      if (index > firstWrite && kind == 'r' && addr == A_EV_HVD) {
        station.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_STOPPED);
    CHECK_EQ(statusOf(result, A_EV_HVD), ss::ENTRY_NO_ANSWER);
    CHECK_EQ(station.bus.count('w', A_EV_HVD), 1);
    CHECK_EQ(station.bus.count('r', A_EV_HVD), 1 + ss::READ_ATTEMPTS);  // once before the run, twice after the write
    CHECK_EQ(result.restart, ss::RESTART_DONE);
    CHECK(!editPending(station.sim));
    checkPauses(station);
  }
  {
    // It answers the second request: one lost reply stops nothing.
    Station station;
    failAt(station, firstWrite + 5, SimSunSaver::FAULT_NO_REPLY);  // the read after the write of EV_hvd
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(station.bus.count('w', A_EV_HVD), 1);
    checkPauses(station);
  }
  {
    // The controller refuses to let it be read.
    Station station;
    station.bus.before = [&](size_t index, char kind, uint16_t addr) {
      if (index > firstWrite && kind == 'r' && addr == A_EV_HVD) {
        station.sim.regs.erase(A_EV_HVD);
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_STOPPED);
    CHECK_EQ(statusOf(result, A_EV_HVD), ss::ENTRY_NO_ANSWER);
    CHECK_EQ(station.bus.count('r', A_EV_HVD), 2);  // refused is refused: not asked again
  }

  // The controller does not report the pending edit that the specification promises.
  {
    Station station;
    station.bus.before = [&](size_t, char, uint16_t) {
      station.sim.regs[A_ARRAY_FAULT] = 0;
      station.sim.regs[A_LOAD_FAULT] = 0;
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(result.notes, ss::NOTE_SW1_OFF | ss::NOTE_EDIT_FAULT_NOT_SEEN);
    CHECK_EQ(station.bus.count('c'), 2);  // the command is sent all the same
  }
  {
    // One of the two words is enough to see it, and a word that cannot be read settles nothing.
    for (int which = 0; which < 4; ++which) {
      Station station;
      station.bus.before = [&](size_t index, char, uint16_t) {
        if (index <= firstWrite) {
          return;
        }
        if (which == 0) {
          station.sim.regs[A_ARRAY_FAULT] = 0;
        } else if (which == 1) {
          station.sim.regs[A_LOAD_FAULT] = 0;
        } else if (station.bus.count('c') < 2) {
          station.sim.regs.erase(which == 2 ? A_ARRAY_FAULT : A_LOAD_FAULT);
        } else {
          station.sim.regs[A_ARRAY_FAULT] = 0;
          station.sim.regs[A_LOAD_FAULT] = 0;
        }
      };
      const ss::ProgramResult &result = station.run();
      CHECK_EQ(result.notes & ss::NOTE_EDIT_FAULT_NOT_SEEN, 0);
      CHECK_EQ(ss::countEntries(result, ss::ENTRY_WRITTEN), 15);
      // With the array fault word gone, the load fault word is not asked for while it waits.
      if (which == 2) {
        const size_t lastWrite = station.bus.nth('w', 14);
        CHECK_EQ(station.bus.log[lastWrite + 2].addr, A_LOAD_STATE);
        CHECK_EQ(station.bus.log[lastWrite + 3].addr, A_ARRAY_FAULT);
        CHECK_EQ(station.bus.log[lastWrite + 4].kind, 'c');
      }
    }
  }

  // What the LOAD output does while the controller waits, and afterwards.
  {
    Station station;
    station.bus.before = [&](size_t index, char, uint16_t) {
      if (index <= firstWrite) {
        return;
      }
      station.sim.regs[A_LOAD_STATE] = station.bus.count('c') < 2 ? 4 : 0;  // FAULT, then START
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(result.loadStateBefore, 1);
    CHECK_EQ(result.loadStateEdit, 4);
    CHECK_EQ(result.loadStateAfter, 0);
    CHECK_TEXT(textFrom([&](ss::Text *t) { ss::describeLoad(result, t); }),
             std::string("LOAD output: LOAD_ON before the run, FAULT while the charger waited for its restart, "
                         "START at the end."));
  }
  {
    // A word that is no state the controller has is kept apart from "not read".
    Station station;
    station.sim.regs[A_LOAD_STATE] = 0x1234;
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.loadStateBefore, ss::LOAD_STATE_OTHER);
    CHECK_EQ(result.loadStateAfter, ss::LOAD_STATE_OTHER);
    station.sim.regs[A_LOAD_STATE] = 0x00FF;
    CHECK_EQ(station.run().loadStateBefore, ss::LOAD_STATE_OTHER);
    station.sim.regs[A_LOAD_STATE] = 0x00FD;
    CHECK_EQ(station.run().loadStateBefore, 0xFD);
  }
}

// ============================================================================
// The restart after the writes, and taking a run up again
// ============================================================================
static void testFinalRestart() {
  const ss::Profile profile = ss::lifepo4Profile();
  const SimSunSaver fresh;

  // The command is lost once: the controller answers, still waits, and gets it again.
  {
    Station station;
    station.bus.before = [&](size_t, char kind, uint16_t) {
      if (kind == 'c' && station.bus.count('c') == 1) {
        station.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(result.restart, ss::RESTART_DONE);
    CHECK_EQ(station.bus.count('c'), 3);
    CHECK_EQ(station.sim.resets, 2);
    CHECK(!editPending(station.sim));
    checkPauses(station);
  }
  // It is acknowledged once without doing anything: the same.
  {
    Station station;
    station.bus.before = [&](size_t, char kind, uint16_t) {
      if (kind == 'c') {
        station.sim.resetWorks = station.bus.count('c') != 1;
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(station.bus.count('c'), 3);
    CHECK_EQ(station.sim.resets, 2);
  }
  // The controller refuses it now, although it took it before the writes.
  {
    Station station;
    station.bus.before = [&](size_t, char kind, uint16_t) {
      if (kind == 'w') {
        station.sim.coils.erase(A_COIL_RESTART);
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_RESTART_FAILED);
    CHECK_EQ(result.restart, ss::RESTART_REFUSED);
    CHECK_EQ(result.refusals, 0);  // a refusal is for a run that wrote nothing
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_WRITTEN), 15);
    CHECK_EQ(station.bus.count('c'), 2);  // refused is refused: not sent again
    CHECK(editPending(station.sim));
    CHECK(holdsProfile(station.sim, profile));
    CHECK(!station.programmer.controllerUnknown());
    CHECK(!station.programmer.resume());
    // The registers are read again all the same: 17 and the load state.
    CHECK_EQ(station.bus.log.size() - station.bus.nth('c', 1), 1 + ss::PLAN_ENTRIES + 1);
    CHECK_EQ(result.loadStateAfter, 1);
  }

  // The controller does not come back from it. The run ends not knowing, and is taken up again.
  // (A short timeout, so that BACK_PROBES ends the wait and not the time limit.)
  {
    Station station;
    station.master.responseTimeoutMs = 300;
    station.bus.before = [&](size_t, char kind, uint16_t) {
      if (kind == 'c' && station.bus.count('c') == 1) {
        station.sim.resetDownMs = 3600UL * 1000UL;
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_RESTART_FAILED);
    CHECK_EQ(result.restart, ss::RESTART_NO_ANSWER);
    CHECK_EQ(result.probes, ss::BACK_PROBES);
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_WRITTEN), 15);
    CHECK_EQ(result.loadStateAfter, ss::LOAD_STATE_NOT_READ);
    CHECK(station.programmer.controllerUnknown());
    CHECK(station.programmer.ended());
    // Nothing is asked after the last unanswered request.
    CHECK_EQ(station.bus.log.size() - station.bus.nth('c', 1), 1 + ss::BACK_PROBES);
    checkPauses(station);

    // Taken up while it is still gone: two requests, and it still does not know.
    size_t had = station.bus.log.size();
    CHECK(station.programmer.resume());
    CHECK(station.programmer.active());
    CHECK(!station.programmer.controllerUnknown());  // not while it is looking
    station.drive();
    CHECK_EQ(station.bus.log.size() - had, ss::READ_ATTEMPTS);
    CHECK_EQ(result.outcome, ss::OUTCOME_RESTART_FAILED);
    CHECK_EQ(result.restart, ss::RESTART_NO_ANSWER);
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_WRITTEN), 15);
    CHECK(station.programmer.controllerUnknown());

    // It is back, and has restarted in the meantime: no command, the registers once more.
    station.sim.advance(3600UL * 1000UL);
    had = station.bus.log.size();
    CHECK(station.programmer.resume());
    station.drive();
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(result.restart, ss::RESTART_DONE);
    CHECK_EQ(result.loadStateAfter, 1);
    CHECK_EQ(station.bus.log.size() - had, 2 + ss::PLAN_ENTRIES + 1);
    CHECK_EQ(station.bus.count('c'), 2);
    CHECK_EQ(station.bus.count('w'), 15);
    CHECK(!station.programmer.controllerUnknown());
    CHECK(!station.programmer.resume());
    CHECK(station.commits.size() == 2);  // taking a run up again waits for no go
  }
  // The line went dead before the command reached the controller, which still waits when it is back.
  {
    Station station;
    station.bus.before = [&](size_t, char kind, uint16_t) {
      if (kind == 'c' && station.bus.count('c') == 1) {
        station.sim.powered = false;
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_RESTART_FAILED);
    CHECK_EQ(result.restart, ss::RESTART_NO_ANSWER);
    CHECK(station.programmer.controllerUnknown());
    station.bus.before = nullptr;
    station.sim.powered = true;
    CHECK(editPending(station.sim));
    CHECK(station.programmer.resume());
    station.drive();
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(result.restart, ss::RESTART_DONE);
    CHECK_EQ(station.bus.count('c'), 3);
    CHECK_EQ(station.sim.resets, 2);
    CHECK(!editPending(station.sim));
    CHECK(holdsProfile(station.sim, profile));
    CHECK(!station.programmer.controllerUnknown());
    checkPauses(station);
  }

  // It comes back and its fault words cannot be read: not confirmed. The registers are read all
  // the same, and the run is taken up again later.
  {
    const uint16_t gone[] = {A_ARRAY_FAULT, A_LOAD_FAULT};
    for (int which = 0; which < 2; ++which) {
      Station station;
      station.bus.before = [&](size_t, char kind, uint16_t) {
        if (kind == 'r' && station.bus.count('c') == 2) {
          station.sim.regs.erase(gone[which]);
        }
      };
      const ss::ProgramResult &result = station.run();
      CHECK_EQ(result.outcome, ss::OUTCOME_RESTART_FAILED);
      CHECK_EQ(result.restart, ss::RESTART_UNCONFIRMED);
      CHECK_EQ(result.loadStateAfter, 1);
      CHECK_EQ(result.notes & ss::NOTE_VERIFY_INCOMPLETE, 0);
      CHECK(station.programmer.controllerUnknown());
      // Still unreadable: the same again.
      CHECK(station.programmer.resume());
      station.drive();
      CHECK_EQ(result.outcome, ss::OUTCOME_RESTART_FAILED);
      CHECK_EQ(result.restart, ss::RESTART_UNCONFIRMED);
      CHECK(station.programmer.controllerUnknown());
      station.bus.before = nullptr;
      station.sim.regs[gone[which]] = 0;
      CHECK(station.programmer.resume());
      station.drive();
      CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
      CHECK_EQ(result.restart, ss::RESTART_DONE);
      CHECK(!station.programmer.controllerUnknown());
    }
  }

  // A register no longer holds its word after the restart.
  {
    Station station;
    station.bus.before = [&](size_t, char kind, uint16_t) {
      if (kind == 'r' && station.bus.count('c') == 2) {
        station.sim.regs[A_EV_LVD] = reg(fresh, A_EV_LVD);
        station.sim.regs[A_EV_EQ] = 0x1300;  // one that was not written, too
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_VERIFY_FAILED);
    CHECK(!ss::outcomeIsGood(result.outcome));
    CHECK_EQ(result.restart, ss::RESTART_DONE);
    CHECK_EQ(statusOf(result, A_EV_LVD), ss::ENTRY_LOST);
    CHECK_EQ(statusOf(result, A_EV_EQ), ss::ENTRY_LOST);
    CHECK_EQ(entryFor(result, A_EV_LVD)->found, reg(fresh, A_EV_LVD));
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_LOST), 2);
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_WRITTEN), 14);
    CHECK_HAS(textFrom([&](ss::Text *t) { ss::describeEntry(*entryFor(result, A_EV_LVD), result.scale, t); }),
              "EV_lvd: 11.50 V -> 13.10 V, lost after the restart: it holds 11.50 V");
    CHECK(!station.programmer.controllerUnknown());
  }

  // Only one register has lost its word: the same.
  {
    Station station;
    station.bus.before = [&](size_t, char kind, uint16_t) {
      if (kind == 'r' && station.bus.count('c') == 2) {
        station.sim.regs[A_EV_LVD] = reg(fresh, A_EV_LVD);
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_VERIFY_FAILED);
    CHECK_EQ(statusOf(result, A_EV_LVD), ss::ENTRY_LOST);
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_LOST), 1);
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_WRITTEN), 14);
  }

  // A register cannot be read again after the restart: the run cannot say that the charger holds
  // what it asked for, and says so.
  {
    Station station;
    station.bus.before = [&](size_t, char kind, uint16_t) {
      if (kind == 'r' && station.bus.count('c') == 2) {
        station.sim.regs.erase(A_ET_FLOAT);
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_UNVERIFIED);
    CHECK(!ss::outcomeIsGood(result.outcome));
    CHECK_EQ(result.restart, ss::RESTART_DONE);
    CHECK_EQ(result.notes, ss::NOTE_SW1_OFF | ss::NOTE_VERIFY_INCOMPLETE);
    CHECK_EQ(statusOf(result, A_ET_FLOAT), ss::ENTRY_UNREAD);
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_UNREAD), 1);
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_WRITTEN), 14);
    CHECK_EQ(result.loadStateAfter, 1);
    CHECK_EQ(station.bus.log.size(), quietRun().size());
    CHECK(!station.programmer.controllerUnknown());
    // It was written, and read back, before the restart.
    CHECK_TEXT(textFrom([&](ss::Text *t) { ss::describeRun(result, t); }),
               std::string("unverified: 15 of 17 registers written, 2 already set, 1 not read after the restart"));
  }
  // Two registers in a row miss one request each: each is asked once more, and both are read.
  {
    Station station;
    station.bus.before = [&](size_t index, char kind, uint16_t) {
      if (kind == 'r' && station.bus.count('c') == 2) {
        const size_t firstRead = station.bus.nth('c', 1) + 4;  // after the missed probe and the two fault words
        if (index == firstRead || index == firstRead + 2) {
          station.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
        }
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(result.notes & ss::NOTE_VERIFY_INCOMPLETE, 0);
    CHECK_EQ(station.bus.log.size(), quietRun().size() + 2);
    const size_t firstRead = station.bus.nth('c', 1) + 4;
    CHECK_EQ(station.bus.log[firstRead + 1].addr, station.bus.log[firstRead].addr);
    CHECK_EQ(station.bus.log[firstRead + 3].addr, station.bus.log[firstRead + 2].addr);
    CHECK(station.bus.log[firstRead + 2].addr != station.bus.log[firstRead].addr);
    CHECK_EQ(station.bus.log[firstRead + 3].status, ss::SSIO_OK);
    checkPauses(station);
  }
  // The controller stops answering after its restart was seen: three registers are asked for,
  // twice each, and then the run ends without asking for the other fourteen. None of the 17 is
  // confirmed.
  {
    Station station;
    station.bus.before = [&](size_t index, char, uint16_t) {
      if (station.bus.count('c') == 2 && index >= station.bus.nth('c', 1) + 4) {
        station.sim.powered = false;
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_UNVERIFIED);
    CHECK_EQ(result.restart, ss::RESTART_DONE);
    CHECK_EQ(result.notes, ss::NOTE_SW1_OFF | ss::NOTE_VERIFY_INCOMPLETE);
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_UNREAD), ss::PLAN_ENTRIES);
    CHECK_EQ(result.loadStateAfter, ss::LOAD_STATE_NOT_READ);
    CHECK_EQ(station.bus.log.size() - station.bus.nth('c', 1), 1 + 1 + 2 + ss::VERIFY_MISSES * ss::READ_ATTEMPTS);
    CHECK(!station.programmer.controllerUnknown());
    checkPauses(station);
  }
  {
    // Two that cannot be read, then one that can: the count starts again, and the run reads on.
    Station station;
    station.bus.before = [&](size_t, char kind, uint16_t) {
      if (kind == 'r' && station.bus.count('c') == 2) {
        station.sim.regs.erase(A_EV_EQ);
        station.sim.regs.erase(A_EV_EQ2);
        station.sim.regs.erase(A_EV_HVD);
        station.sim.regs.erase(A_EV_HVR);
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_UNVERIFIED);
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_UNREAD), 4);
    CHECK_EQ(result.loadStateAfter, 1);  // read at the very end: the run went through all 17
    CHECK_EQ(station.bus.log.size(), quietRun().size());
  }

  // The power goes while the run writes. It stops, cannot restart the controller, and is taken up
  // again when the controller is back: the outcome stays "stopped", the controller is restarted.
  {
    Station station;
    station.bus.before = [&](size_t, char kind, uint16_t addr) {
      if (kind == 'w' && addr == A_EV_REG) {
        station.sim.powered = false;
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_STOPPED);
    CHECK_EQ(result.restart, ss::RESTART_NO_ANSWER);
    CHECK_EQ(statusOf(result, A_EV_REG), ss::ENTRY_NO_ANSWER);
    CHECK_EQ(statusOf(result, A_EVB_REF_LIM), ss::ENTRY_WRITTEN);
    CHECK_EQ(statusOf(result, A_EV_FLOAT), ss::ENTRY_PENDING);
    CHECK_EQ(result.loadStateEdit, ss::LOAD_STATE_NOT_READ);
    CHECK(station.programmer.controllerUnknown());
    checkPauses(station);
    station.bus.before = nullptr;
    station.sim.powered = true;
    CHECK(editPending(station.sim));
    CHECK(station.programmer.resume());
    station.drive();
    CHECK_EQ(result.outcome, ss::OUTCOME_STOPPED);
    CHECK_EQ(result.restart, ss::RESTART_DONE);
    CHECK_EQ(statusOf(result, A_EV_REG), ss::ENTRY_NO_ANSWER);
    CHECK_EQ(statusOf(result, A_EVB_REF_LIM), ss::ENTRY_WRITTEN);
    CHECK(!editPending(station.sim));
    CHECK(!station.programmer.controllerUnknown());
    CHECK(onlyPlanChanged(station.sim, fresh) && beforeOrTarget(station.sim, fresh, profile));
    CHECK_EQ(station.bus.disorder, 0);
  }

  // A run is taken up again only when it ended not knowing.
  {
    ss::Programmer idle;
    CHECK(!idle.resume());
    CHECK(!idle.controllerUnknown());
    Station station;
    station.programmer.start(profile, true);
    CHECK(!station.programmer.resume());  // not while it runs
    CHECK(!station.programmer.step(station.bus, 4));
    CHECK(!station.programmer.resume());
    CHECK_EQ(station.bus.log.size(), 4);
    station.drive();
    CHECK_EQ(station.programmer.result().outcome, ss::OUTCOME_PROGRAMMED);
    CHECK(!station.programmer.resume());
    CHECK(station.programmer.ended());
  }
}

// ============================================================================
// After the client itself restarted during a run
// ============================================================================
static void testRecovery() {
  // The controller does not wait for anything: two fault words, the load state, no command.
  for (int before = 0; before < 2; ++before) {
    Station station;
    station.programmer.startRecovery(before == 1);
    CHECK(station.programmer.active());
    station.drive();
    const ss::ProgramResult &result = station.programmer.result();
    CHECK_EQ(result.outcome, ss::OUTCOME_INTERRUPTED);
    CHECK(!ss::outcomeIsGood(result.outcome));
    CHECK_EQ(result.restart, ss::RESTART_NOT_NEEDED);
    CHECK_EQ(result.notes, before == 1 ? (ss::NOTE_NOTHING_WRITTEN | ss::NOTE_AT_FIRST_RESTART) : 0);
    CHECK_EQ(result.planned, 0);
    CHECK_EQ(result.seen, ss::SEEN_FAULTS);
    CHECK_EQ(result.loadStateAfter, 1);
    CHECK_EQ(result.loadStateBefore, ss::LOAD_STATE_NOT_READ);
    CHECK(!result.wroteAny);
    CHECK_EQ(result.refusals, 0);
    CHECK(station.commits.empty());  // it waits for no go: there is nothing to record
    Sequence want;
    want.push_back(std::make_pair('r', A_ARRAY_FAULT));
    want.push_back(std::make_pair('r', A_LOAD_FAULT));
    want.push_back(std::make_pair('r', A_LOAD_STATE));
    checkSequence(station.bus, want);
    CHECK(!station.programmer.controllerUnknown());
    CHECK(!station.programmer.resume());
  }

  // It waits for a restart: it gets one.
  for (int which = 0; which < 3; ++which) {
    Station station;
    if (which != 1) {
      station.sim.regs[A_ARRAY_FAULT] = EDIT_ARRAY;
    }
    if (which != 0) {
      station.sim.regs[A_LOAD_FAULT] = EDIT_LOAD;
    }
    station.programmer.startRecovery(false);
    station.drive();
    const ss::ProgramResult &result = station.programmer.result();
    CHECK_EQ(result.outcome, ss::OUTCOME_INTERRUPTED);
    CHECK_EQ(result.restart, ss::RESTART_DONE);
    CHECK_EQ(result.notes, ss::NOTE_WAS_PENDING);
    CHECK_EQ(result.arrayFault, which != 1 ? EDIT_ARRAY : 0);
    CHECK_EQ(result.loadFault, which != 0 ? EDIT_LOAD : 0);
    CHECK_EQ(station.bus.count('c'), 1);
    CHECK_EQ(station.bus.count('w'), 0);
    CHECK_EQ(station.sim.resets, 1);
    CHECK(!editPending(station.sim));
    CHECK_EQ(result.loadStateAfter, 1);
    CHECK(!station.programmer.controllerUnknown());
    checkPauses(station);
    CHECK_EQ(station.bus.log.size(), 2 + 1 + 1 + 2 + 1);  // with the probe it missed while it restarted
  }

  // It waits, and the command does not work: said, and nothing to find out later.
  {
    Station station;
    station.sim.regs[A_ARRAY_FAULT] = EDIT_ARRAY;
    station.sim.coils.erase(A_COIL_RESTART);
    station.programmer.startRecovery(false);
    station.drive();
    CHECK_EQ(station.programmer.result().outcome, ss::OUTCOME_INTERRUPTED);
    CHECK_EQ(station.programmer.result().restart, ss::RESTART_REFUSED);
    CHECK_EQ(station.programmer.result().refusals, 0);
    CHECK(!station.programmer.controllerUnknown());
    Station ignored;
    ignored.sim.regs[A_LOAD_FAULT] = EDIT_LOAD;
    ignored.sim.resetWorks = false;
    ignored.programmer.startRecovery(false);
    ignored.drive();
    CHECK_EQ(ignored.programmer.result().outcome, ss::OUTCOME_INTERRUPTED);
    CHECK_EQ(ignored.programmer.result().restart, ss::RESTART_NOT_DONE);
    CHECK_EQ(ignored.bus.count('c'), ss::RESTART_ATTEMPTS);
    CHECK(!ignored.programmer.controllerUnknown());
  }

  // The controller does not answer: the recovery is owed again.
  {
    Station station;
    station.sim.powered = false;
    station.programmer.startRecovery(true);
    station.drive();
    const ss::ProgramResult &result = station.programmer.result();
    CHECK_EQ(result.outcome, ss::OUTCOME_NO_LINK);
    CHECK_EQ(result.seen, 0);
    CHECK_EQ(station.bus.log.size(), ss::READ_ATTEMPTS);
    CHECK(station.programmer.controllerUnknown());
    CHECK(!station.programmer.resume());  // a recovery is started anew, not taken up
    checkPauses(station);
    // The second word is the one that does not answer.
    Station second;
    second.sim.regs.erase(A_LOAD_FAULT);
    second.programmer.startRecovery(false);
    second.drive();
    CHECK_EQ(second.programmer.result().outcome, ss::OUTCOME_NO_LINK);
    CHECK_EQ(second.programmer.result().seen, 0);
    CHECK_EQ(second.bus.count('c'), 0);
    CHECK(second.programmer.controllerUnknown());
  }
  // It waits, gets the command, and does not come back: owed again as well.
  {
    Station station;
    station.sim.regs[A_ARRAY_FAULT] = EDIT_ARRAY;
    station.bus.before = [&](size_t, char kind, uint16_t) {
      if (kind == 'c') {
        station.sim.powered = false;
      }
    };
    station.programmer.startRecovery(false);
    station.drive();
    CHECK_EQ(station.programmer.result().outcome, ss::OUTCOME_INTERRUPTED);
    CHECK_EQ(station.programmer.result().restart, ss::RESTART_NO_ANSWER);
    CHECK(station.programmer.controllerUnknown());
    // The next recovery finds it.
    station.bus.before = nullptr;
    station.sim.powered = true;
    station.programmer.startRecovery(false);
    station.drive();
    CHECK_EQ(station.programmer.result().outcome, ss::OUTCOME_INTERRUPTED);
    CHECK_EQ(station.programmer.result().restart, ss::RESTART_DONE);
    CHECK(!editPending(station.sim));
    CHECK(!station.programmer.controllerUnknown());
  }

  // A recovery after a run leaves nothing of that run in its result.
  {
    Station station;
    CHECK_EQ(station.run().outcome, ss::OUTCOME_PROGRAMMED);
    station.programmer.startRecovery(false);
    station.drive();
    const ss::ProgramResult &result = station.programmer.result();
    CHECK_EQ(result.outcome, ss::OUTCOME_INTERRUPTED);
    CHECK_EQ(result.planned, 0);
    CHECK(!result.wroteAny);
    CHECK_EQ(result.profile.absorbCv, 0);
    CHECK_EQ(result.notes, 0);
    // And a run after a recovery is a run like any other.
    station.sim.regs[A_EV_LVD] = station.sim.setVolts(12.00);
    station.sim.regs[A_V_LVD] = station.sim.volts(12.00);
    station.commits.clear();
    const ss::ProgramResult &again = station.run();
    CHECK_EQ(again.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(ss::countEntries(again, ss::ENTRY_WRITTEN), 1);
    CHECK_EQ(station.commits.size(), 2);
  }
}

// ============================================================================
// The sweeps: the quiet run, disturbed at every request in turn
// ============================================================================

// What must hold however a run went.
static void checkLeftSafe(const Station &station, const SimSunSaver &fresh, const ss::Profile &profile, const char *what,
                          size_t at) {
  const ss::ProgramResult &result = station.programmer.result();
  CHECK_MSG(station.bus.disorder == 0, "%s at request %u: the settings were out of order at some moment", what,
            (unsigned)at);
  CHECK_MSG(onlyPlanChanged(station.sim, fresh), "%s at request %u: a register outside the plan changed", what,
            (unsigned)at);
  CHECK_MSG(beforeOrTarget(station.sim, fresh, profile), "%s at request %u: a register holds a third word", what,
            (unsigned)at);
  // What the result says about a register is what the controller holds.
  for (uint8_t i = 0; i < result.planned; ++i) {
    const ss::PlanEntry &entry = result.entries[i];
    const uint16_t now = reg(station.sim, ss::item(entry.item).addr);
    if (entry.status == ss::ENTRY_WRITTEN || entry.status == ss::ENTRY_UNCHANGED) {
      CHECK_MSG(now == entry.target, "%s at request %u: %s is reported as %s and holds 0x%04X", what, (unsigned)at,
                ss::item(entry.item).name, ss::entryStatusName(entry.status), now);
    } else if (entry.status == ss::ENTRY_PENDING || entry.status == ss::ENTRY_NOT_TAKEN ||
               entry.status == ss::ENTRY_REFUSED) {
      CHECK_MSG(now == entry.before, "%s at request %u: %s is reported as %s and holds 0x%04X", what, (unsigned)at,
                ss::item(entry.item).name, ss::entryStatusName(entry.status), now);
    }
  }
  if (ss::outcomeIsGood(result.outcome)) {
    CHECK_MSG(holdsProfile(station.sim, profile) && !editPending(station.sim), "%s at request %u: reported as %s", what,
              (unsigned)at, ss::outcomeName(result.outcome));
  }
  if (!result.wroteAny) {
    CHECK_MSG(station.bus.count('w') == 0, "%s at request %u: wrote, and says it did not", what, (unsigned)at);
  }
  // Not left waiting for a restart without the run knowing that something is open.
  if (editPending(station.sim)) {
    CHECK_MSG(station.programmer.controllerUnknown() || result.restart == ss::RESTART_REFUSED ||
                  result.restart == ss::RESTART_NOT_DONE,
              "%s at request %u: the controller waits for a restart and the run ended as %s", what, (unsigned)at,
              ss::outcomeName(result.outcome));
  }
}

static void testSweeps() {
  const ss::Profile profile = ss::lifepo4Profile();
  const SimSunSaver fresh;
  const size_t length = quietRun().size();

  // One frame lost or damaged, at any request: the run still ends as programmed.
  {
    const struct {
      SimSunSaver::Fault fault;
      const char *name;
    } kinds[] = {{SimSunSaver::FAULT_NO_REPLY, "a lost request"},
                 {SimSunSaver::FAULT_LOST_REPLY, "a lost reply"},
                 {SimSunSaver::FAULT_BAD_CRC, "a damaged reply"},
                 {SimSunSaver::FAULT_TRUNCATE, "a reply cut short"},
                 {SimSunSaver::FAULT_WRONG_SLAVE, "another slave's reply"},
                 {SimSunSaver::FAULT_WRONG_FUNCTION, "a reply for another function"},
                 {SimSunSaver::FAULT_EXTRA_REGISTER, "a reply that is too long"},
                 {SimSunSaver::FAULT_WRONG_ECHO, "a wrong echo"},
                 {SimSunSaver::FAULT_GARBAGE, "noise for a reply"}};
    for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); ++k) {
      for (size_t at = 0; at < length; ++at) {
        Station station;
        failAt(station, at, kinds[k].fault);
        const ss::ProgramResult &result = station.run();
        CHECK_MSG(result.outcome == ss::OUTCOME_PROGRAMMED, "%s at request %u: the run ended as %s", kinds[k].name,
                  (unsigned)at, ss::outcomeName(result.outcome));
        CHECK_MSG(result.restart == ss::RESTART_DONE && !editPending(station.sim), "%s at request %u: restart %u",
                  kinds[k].name, (unsigned)at, (unsigned)result.restart);
        CHECK_MSG(ss::countEntries(result, ss::ENTRY_WRITTEN) == 15, "%s at request %u", kinds[k].name, (unsigned)at);
        CHECK(!station.programmer.controllerUnknown());
        checkLeftSafe(station, fresh, profile, kinds[k].name, at);
        checkPauses(station);
      }
    }
  }

  // Two requests lost in a row. The run may end in several ways, and in none of them is the
  // controller left in a worse state, or waiting.
  {
    long outcomes[ss::OUTCOME_LAST + 1] = {0};
    for (size_t at = 0; at < length; ++at) {
      Station station;
      failAt(station, at, SimSunSaver::FAULT_NO_REPLY, 2);
      station.run();
      if (station.programmer.controllerUnknown()) {
        CHECK_MSG(station.programmer.resume(), "two lost requests at %u: not taken up again", (unsigned)at);
        station.drive();
      }
      const ss::ProgramResult &result = station.programmer.result();
      CHECK(result.outcome <= ss::OUTCOME_LAST);
      ++outcomes[result.outcome <= ss::OUTCOME_LAST ? result.outcome : 0];
      CHECK_MSG(!editPending(station.sim), "two lost requests at %u: the controller still waits for a restart", (unsigned)at);
      CHECK(!station.programmer.controllerUnknown());
      checkLeftSafe(station, fresh, profile, "two lost requests", at);
      checkPauses(station);
    }
    CHECK_EQ(outcomes[ss::OUTCOME_NONE], 0);
    CHECK_EQ(outcomes[ss::OUTCOME_UNCHANGED], 0);
    CHECK_EQ(outcomes[ss::OUTCOME_INTERRUPTED], 0);
    CHECK_EQ(outcomes[ss::OUTCOME_VERIFY_FAILED], 0);
    CHECK_EQ(outcomes[ss::OUTCOME_RESTART_FAILED], 0);  // taken up again, it ends as programmed
    // A needed register silent twice before the run: refused. A register silent twice after its
    // write: stopped. The fault word silent twice after the first restart: no link.
    CHECK(outcomes[ss::OUTCOME_REFUSED] >= 17);
    CHECK_EQ(outcomes[ss::OUTCOME_STOPPED], 15);
    CHECK_EQ(outcomes[ss::OUTCOME_NO_LINK], 1);
    CHECK(outcomes[ss::OUTCOME_PROGRAMMED] > 40);
  }

  // The controller loses its power at any request, for good as far as the run can tell. When it
  // is back, a run that wrote is taken up again, and the controller is restarted if it waits.
  for (size_t at = 0; at < length; ++at) {
    Station station;
    station.bus.before = [&station, at](size_t index, char, uint16_t) {
      if (index >= at) {
        station.sim.powered = false;
      }
    };
    station.run();
    const ss::ProgramResult &result = station.programmer.result();
    CHECK_MSG(result.outcome != ss::OUTCOME_UNCHANGED && result.outcome != ss::OUTCOME_NONE, "power lost at %u", (unsigned)at);
    checkPauses(station);
    const bool owed = station.programmer.controllerUnknown();
    if (owed) {
      // Still without power: the look is owed again.
      CHECK(station.programmer.resume());
      station.drive();
      CHECK_MSG(station.programmer.controllerUnknown(), "power lost at %u: gave up on the controller", (unsigned)at);
    }
    station.bus.before = nullptr;
    station.sim.powered = true;
    if (editPending(station.sim)) {
      CHECK_MSG(owed, "power lost at %u: the controller waits for a restart and the run does not know", (unsigned)at);
    }
    if (owed) {
      CHECK(station.programmer.resume());
      station.drive();
    }
    CHECK_MSG(!editPending(station.sim), "power lost at %u: the controller still waits for a restart", (unsigned)at);
    CHECK_MSG(!station.programmer.controllerUnknown(), "power lost at %u: still not known", (unsigned)at);
    if (owed) {
      CHECK_MSG(result.restart == ss::RESTART_DONE, "power lost at %u: restart %u after it was taken up", (unsigned)at,
                (unsigned)result.restart);
      CHECK_EQ(result.notes & ss::NOTE_VERIFY_INCOMPLETE, 0);
    }
    checkLeftSafe(station, fresh, profile, "power lost", at);
  }

  // The client itself is gone at any request, and the client that starts afterwards runs a
  // recovery: whatever the first one had done, the controller does not stay waiting.
  for (size_t at = 0; at <= length; ++at) {
    Station station;
    station.budget = 1;
    station.programmer.start(profile, true);
    // Driven by hand, so that it can be dropped after exactly `at` requests.
    for (unsigned guard = 0; guard < 4000 && station.bus.log.size() < at; ++guard) {
      const bool ended = station.programmer.step(station.bus, 1);
      if (station.programmer.awaiting() != ss::COMMIT_NONE) {
        station.programmer.go();
        continue;
      }
      if (ended) {
        break;
      }
      const uint32_t wait = station.programmer.pauseMs() > 250 ? station.programmer.pauseMs() : 250;
      station.sim.advance(wait);
    }
    const size_t wrote = station.bus.count('w');
    station.sim.advance(20000);  // the client restarts
    station.programmer = ss::Programmer();
    CHECK(!station.programmer.active());
    station.programmer.startRecovery(wrote == 0);
    station.drive();
    const ss::ProgramResult &result = station.programmer.result();
    CHECK_MSG(result.outcome == ss::OUTCOME_INTERRUPTED, "client gone at %u: the recovery ended as %s", (unsigned)at,
              ss::outcomeName(result.outcome));
    CHECK_MSG(!editPending(station.sim), "client gone at %u: the controller still waits for a restart", (unsigned)at);
    CHECK_MSG((result.restart == ss::RESTART_DONE) == ((result.notes & ss::NOTE_WAS_PENDING) != 0), "client gone at %u",
              (unsigned)at);
    CHECK_EQ(station.bus.count('w'), wrote);  // a recovery writes nothing
    CHECK(!station.programmer.controllerUnknown());
    CHECK_MSG(station.bus.disorder == 0, "client gone at %u: out of order", (unsigned)at);
    CHECK_MSG(onlyPlanChanged(station.sim, fresh) && beforeOrTarget(station.sim, fresh, profile), "client gone at %u",
              (unsigned)at);
  }
}

// ============================================================================
// A request in a config, and the client's journal
// ============================================================================

static const uint32_t kRequestId = 1795500000u;  // the server's clock when a person asked
static const double kClientNow = 1795500030.0;   // the client's clock half a minute later

static JsonDocument parsed(const std::string &json) {
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, json);
  CHECK_MSG(error == DeserializationError::Ok, "the test's JSON does not parse: %s", json.c_str());
  return doc;
}

// "solarCharger.program" as the config generator sends it for the recommended values, with one
// member replaced, or left out when `replacement` is null.
static std::string requestJson(const char *replaceKey = nullptr, const char *replacement = nullptr) {
  static const char *const kMembers[][2] = {{"absorbV", "14.2"}, {"floatV", "13.5"}, {"absorbMin", "15"},
                                            {"hvdV", "14.45"},   {"hvrV", "13.6"},   {"lvdV", "13.1"},
                                            {"lvrV", "13.25"},   {"loadOk", "true"}, {"req", "1795500000"}};
  std::string json = "{";
  for (size_t i = 0; i < sizeof(kMembers) / sizeof(kMembers[0]); ++i) {
    const char *value = kMembers[i][1];
    if (replaceKey != nullptr && strcmp(replaceKey, kMembers[i][0]) == 0) {
      if (replacement == nullptr) {
        continue;
      }
      value = replacement;
    }
    if (json.size() > 1) {
      json += ",";
    }
    json += std::string("\"") + kMembers[i][0] + "\":" + value;
  }
  return json + "}";
}

static uint16_t profileErrorsOf(const std::string &json, ss::Profile *profile = nullptr) {
  JsonDocument doc = parsed(json);
  ss::Profile read;
  memset(&read, 0x5A, sizeof(read));
  const uint16_t errors = ss::readProfile(doc.as<JsonObjectConst>(), &read);
  if (profile != nullptr) {
    *profile = read;
  }
  return errors;
}

static bool sameProfile(const ss::Profile &a, const ss::Profile &b) {
  for (uint8_t field = 0; field < ss::PROFILE_FIELDS; ++field) {
    if (ss::profileField(a, field) != ss::profileField(b, field)) {
      return false;
    }
  }
  return true;
}

static bool sameResult(const ss::ProgramResult &a, const ss::ProgramResult &b);

static void testRequestJson() {
  // The recommended values, as a browser writes them.
  {
    ss::Profile profile;
    CHECK_EQ(profileErrorsOf(requestJson(), &profile), 0);
    CHECK(sameProfile(profile, ss::lifepo4Profile()));
    JsonDocument doc = parsed(requestJson());
    CHECK(ss::readLoadAllowed(doc.as<JsonObjectConst>()));
    CHECK_EQ(ss::readRequestId(doc.as<JsonObjectConst>()), kRequestId);
  }

  // A value that is not there, or is no number: said as such, and as nothing else.
  static const char *const kValues[] = {"absorbV", "floatV", "absorbMin", "hvdV", "hvrV", "lvdV", "lvrV"};
  static const char *const kNoNumbers[] = {nullptr, "\"14.2\"", "true", "null", "[14.2]", "{\"v\":14.2}"};
  for (size_t v = 0; v < sizeof(kValues) / sizeof(kValues[0]); ++v) {
    for (size_t n = 0; n < sizeof(kNoNumbers) / sizeof(kNoNumbers[0]); ++n) {
      ss::Profile profile;
      CHECK_MSG(profileErrorsOf(requestJson(kValues[v], kNoNumbers[n]), &profile) == ss::PROFILE_MISSING, "%s as %s",
                kValues[v], kNoNumbers[n] != nullptr ? kNoNumbers[n] : "(left out)");
      CHECK_EQ(ss::profileField(profile, (uint8_t)v), 0);  // the members come in the order of the struct
    }
  }
  {
    // No object at all.
    ss::Profile profile;
    CHECK_EQ(profileErrorsOf("[1,2,3]", &profile), ss::PROFILE_MISSING);
    CHECK(sameProfile(profile, ss::Profile()));
    JsonDocument none;
    memset(&profile, 0x5A, sizeof(profile));
    CHECK_EQ(ss::readProfile(none.as<JsonObjectConst>(), &profile), ss::PROFILE_MISSING);
    CHECK(sameProfile(profile, ss::Profile()));
    CHECK(!ss::readLoadAllowed(none.as<JsonObjectConst>()));
    CHECK_EQ(ss::readRequestId(none.as<JsonObjectConst>()), 0);
  }
  // A value that is missing hides what else is wrong, except what reading alone finds.
  CHECK_EQ(profileErrorsOf(requestJson("lvdV", nullptr).replace(1, 14, "\"absorbV\":99.9")), ss::PROFILE_MISSING);
  CHECK_EQ(profileErrorsOf(requestJson("lvdV", nullptr).replace(1, 14, "\"absorbV\":14.204")),
           ss::PROFILE_MISSING | ss::PROFILE_GRID);

  // A voltage, read through the LVD. With an LVR of 13.25 V it may be 10.00 to 13.15 V.
  {
    const struct {
      const char *text;
      uint16_t centivolts;
      uint16_t errors;
    } cases[] = {{"13.1", 1310, 0},
                 {"13.10", 1310, 0},
                 {"1.31e1", 1310, 0},
                 {"13", 1300, 0},
                 {"10", 1000, 0},
                 {"10.0", 1000, 0},
                 {"13.15", 1315, 0},
                 {"13.100001", 1310, 0},  // nearer to two decimals than a person can type
                 {"13.104", 1310, ss::PROFILE_GRID},
                 {"13.096", 1310, ss::PROFILE_GRID},
                 {"13.1049", 1310, ss::PROFILE_GRID},
                 {"13.1005", 1310, ss::PROFILE_GRID},  // half a hundredth is more than two decimals
                 {"13.10005", 1310, 0},                // a twentieth of one is within the margin for floats
                 {"13.12", 1312, ss::PROFILE_GRID},
                 {"9.95", 995, ss::PROFILE_LVD},
                 {"13.2", 1320, ss::PROFILE_LVR},
                 {"13.35", 1335, ss::PROFILE_LVD | ss::PROFILE_LVR},
                 {"0", 0, ss::PROFILE_LVD},
                 {"0.0", 0, ss::PROFILE_LVD},
                 {"-13.1", 0, ss::PROFILE_LVD},
                 {"-0.001", 0, ss::PROFILE_LVD},
                 {"655.3", 65530, ss::PROFILE_LVD | ss::PROFILE_LVR},
                 {"655.34", 65534, ss::PROFILE_LVD | ss::PROFILE_LVR | ss::PROFILE_GRID},
                 {"655.354", 65535, ss::PROFILE_LVD | ss::PROFILE_LVR | ss::PROFILE_GRID},
                 {"655.35", 65535, ss::PROFILE_LVD | ss::PROFILE_LVR},
                 {"655.36", 65535, ss::PROFILE_LVD | ss::PROFILE_LVR},
                 {"1e9", 65535, ss::PROFILE_LVD | ss::PROFILE_LVR},
                 {"1e300", 65535, ss::PROFILE_LVD | ss::PROFILE_LVR}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      ss::Profile profile;
      const uint16_t errors = profileErrorsOf(requestJson("lvdV", cases[i].text), &profile);
      CHECK_MSG(errors == cases[i].errors, "an LVD of %s gives the errors 0x%X", cases[i].text, (unsigned)errors);
      CHECK_MSG(profile.lvdCv == cases[i].centivolts, "an LVD of %s reads as %u", cases[i].text, (unsigned)profile.lvdCv);
      CHECK_EQ(profile.absorbCv, 1420);  // the others are read as they are
      CHECK_EQ(profile.lvrCv, 1325);
    }
  }
  // Every voltage is read from its own member.
  {
    static const char *const kVolts[] = {"absorbV", "floatV", "hvdV", "hvrV", "lvdV", "lvrV"};
    static const uint8_t kField[] = {0, 1, 3, 4, 5, 6};
    for (size_t v = 0; v < 6; ++v) {
      ss::Profile profile;
      profileErrorsOf(requestJson(kVolts[v], "12.34"), &profile);
      for (uint8_t field = 0; field < ss::PROFILE_FIELDS; ++field) {
        const uint16_t want = field == kField[v] ? (uint16_t)1234 : ss::profileField(ss::lifepo4Profile(), field);
        CHECK_MSG(ss::profileField(profile, field) == want, "%s set, and field %u reads %u", kVolts[v], (unsigned)field,
                  (unsigned)ss::profileField(profile, field));
      }
    }
  }

  // The time: whole minutes.
  {
    const struct {
      const char *text;
      uint16_t seconds;
      uint16_t errors;
    } cases[] = {{"15", 900, 0},        {"15.0", 900, 0},       {"1.5e1", 900, 0},
                 {"1", 60, 0},          {"240", 14400, 0},      {"241", 14460, ss::PROFILE_TIME},
                 {"0", 0, ss::PROFILE_TIME},    {"-15", 0, ss::PROFILE_TIME},  {"15.5", 0, ss::PROFILE_TIME},
                 {"0.999", 0, ss::PROFILE_TIME}, {"1000", 60000, ss::PROFILE_TIME}, {"1001", 0, ss::PROFILE_TIME},
                 {"1e12", 0, ss::PROFILE_TIME}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      ss::Profile profile;
      const uint16_t errors = profileErrorsOf(requestJson("absorbMin", cases[i].text), &profile);
      CHECK_MSG(errors == cases[i].errors, "%s minutes give the errors 0x%X", cases[i].text, (unsigned)errors);
      CHECK_MSG(profile.absorbSeconds == cases[i].seconds, "%s minutes read as %u s", cases[i].text,
                (unsigned)profile.absorbSeconds);
    }
  }

  // "Current may flow from the LOAD terminals" is true and nothing else.
  {
    static const char *const kNotTrue[] = {nullptr, "false", "1", "\"true\"", "\"yes\"", "null", "[true]"};
    for (size_t i = 0; i < sizeof(kNotTrue) / sizeof(kNotTrue[0]); ++i) {
      JsonDocument doc = parsed(requestJson("loadOk", kNotTrue[i]));
      CHECK_MSG(!ss::readLoadAllowed(doc.as<JsonObjectConst>()), "loadOk as %s", kNotTrue[i] != nullptr ? kNotTrue[i] : "(left out)");
    }
  }
  // The id of a request is a whole number that fits 32 bits. Anything else asks for nothing.
  {
    const struct {
      const char *text;
      uint32_t id;
    } cases[] = {{nullptr, 0},          {"1795500000", 1795500000u}, {"1", 1},        {"0", 0},
                 {"-5", 0},             {"1.5", 0},                  {"true", 0},     {"\"1795500000\"", 0},
                 {"4294967295", 4294967295u}, {"4294967296", 0},     {"null", 0},     {"[1795500000]", 0}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      JsonDocument doc = parsed(requestJson("req", cases[i].text));
      CHECK_MSG(ss::readRequestId(doc.as<JsonObjectConst>()) == cases[i].id, "req as %s", cases[i].text != nullptr ? cases[i].text : "(left out)");
    }
  }

  // How old a request may be.
  CHECK(ss::requestIsFresh(kRequestId, kClientNow));
  CHECK(ss::requestIsFresh(kRequestId, (double)kRequestId));
  CHECK(ss::requestIsFresh(kRequestId, (double)kRequestId + 86400.0));
  CHECK(!ss::requestIsFresh(kRequestId, (double)kRequestId + 86401.0));
  CHECK(ss::requestIsFresh(kRequestId, (double)kRequestId - 3600.0));
  CHECK(!ss::requestIsFresh(kRequestId, (double)kRequestId - 3601.0));
  CHECK(!ss::requestIsFresh(kRequestId, 0.0));
  CHECK(!ss::requestIsFresh(1, 0.0));
  CHECK(!ss::requestIsFresh(0, 0.0));
  CHECK(!ss::requestIsFresh(1599999999u, 1599999999.0));  // a clock that was never set judges nothing
  CHECK(ss::requestIsFresh(1600000000u, 1600000000.0));
  CHECK(!ss::requestIsFresh(kRequestId, std::numeric_limits<double>::quiet_NaN()));
  CHECK(ss::CLOCK_SET_EPOCH == ss::REPORT_EARLIEST_EPOCH);
  CHECK_EQ(ss::REQUEST_WAIT_MS, 3600000u);
  CHECK_EQ(ss::DUTY_RETRY_MS, 600000u);
  CHECK_EQ(ss::DUTY_SLOW_RETRY_MS, 86400000u);
  // A controller that answers right after a restart command is asked ten times more, half a
  // second apart, before its answers count (the documents say so).
  CHECK_EQ(ss::QUIET_PROBES, 10u);
  CHECK_EQ(ss::PROBE_GAP_MS, 500u);

  // The journal.
  {
    const ss::Journal empty = ss::emptyJournal();
    CHECK(empty.requestId == 0 && empty.phase == ss::JOURNAL_IDLE && empty.controller == 0 && empty.answeredId == 0 &&
          empty.waitingId == 0 && !empty.reportDue);
    CHECK_EQ(ss::JOURNAL_IDLE, 0);
    CHECK_EQ(ss::JOURNAL_RESTARTING, 1);
    CHECK_EQ(ss::JOURNAL_WRITING, 2);
    CHECK_EQ(ss::JOURNAL_PHASE_LAST, ss::JOURNAL_WRITING);
    const ss::Journal cautious = ss::cautiousJournal();
    CHECK(cautious.phase == ss::JOURNAL_WRITING && cautious.requestId == 0 && cautious.controller == 0 &&
          cautious.answeredId == 0 && cautious.waitingId == 0 && !cautious.reportDue);

    ss::Journal journal = ss::emptyJournal();
    journal.requestId = kRequestId;
    journal.phase = ss::JOURNAL_WRITING;
    journal.controller = 1;
    journal.answeredId = kRequestId + 5;
    JsonDocument doc;
    ss::encodeJournal(journal, doc.to<JsonObject>());
    std::string wire;
    serializeJson(doc, wire);
    CHECK_TEXT(wire, std::string("{\"req\":1795500000,\"ph\":2,\"ctl\":1,\"ans\":1795500005}"));

    // Every member, through the file and back.
    ss::ProgramResult refused;
    ss::refusedResult(ss::lifepo4Profile(), ss::PROFILE_LVD, ss::REFUSE_PROFILE, &refused);
    for (uint8_t phase = 0; phase <= ss::JOURNAL_PHASE_LAST; ++phase) {
      for (int due = 0; due < 2; ++due) {
        ss::Journal out = ss::emptyJournal();
        out.requestId = kRequestId + phase;
        out.phase = phase;
        out.controller = (uint8_t)(phase == 2 ? 247 : phase);
        out.answeredId = kRequestId + 100 + phase;
        out.waitingId = due == 1 ? kRequestId + 200 : 0u;
        out.reportDue = due == 1;
        if (out.reportDue) {
          out.reportId = kRequestId + 300;
          out.reportEpoch = phase == 1 ? 0u : (uint32_t)kClientNow;
          out.report = refused;
        }
        JsonDocument file;
        ss::encodeJournal(out, file.to<JsonObject>());
        std::string text;
        serializeJson(file, text);
        CHECK_EQ(text.find("wait") != std::string::npos, due == 1);  // left out when nothing waits
        CHECK_EQ(text.find("rep") != std::string::npos, due == 1);   // and when nothing is due
        JsonDocument back = parsed(text);
        const ss::Journal in = ss::decodeJournal(back.as<JsonObjectConst>());
        CHECK_MSG(in.requestId == out.requestId && in.phase == out.phase && in.controller == out.controller &&
                      in.answeredId == out.answeredId && in.waitingId == out.waitingId && in.reportDue == out.reportDue,
                  "the journal changed on its way: %s", text.c_str());
        if (due == 1) {
          CHECK_EQ(in.reportId, out.reportId);
          CHECK_EQ(in.reportEpoch, out.reportEpoch);
          CHECK(sameResult(in.report, refused));
        }
      }
    }

    // The longest journal there can be fits the client's buffer.
    {
      ss::Journal longest;
      longest.requestId = 0xFFFFFFFFu;
      longest.phase = ss::JOURNAL_WRITING;
      longest.controller = 247;
      longest.answeredId = 0xFFFFFFFFu;
      longest.waitingId = 0xFFFFFFFFu;
      longest.reportDue = true;
      longest.reportId = 0xFFFFFFFFu;
      longest.reportEpoch = 0xFFFFFFFFu;
      ss::ProgramResult &result = longest.report;
      ss::clearResult(&result);
      result.outcome = ss::OUTCOME_LAST;
      result.restart = ss::RESTART_STATUS_LAST;
      result.followUp = true;
      result.wroteAny = true;
      result.scale = ss::SCALE_96667;
      result.liveScale = ss::SCALE_96667;
      result.refusals = 0xFFFFFFFFul;
      result.profileErrors = 0xFFFF;
      result.notes = 0xFFFF;
      result.probes = 0xFF;
      const ss::Profile widest = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
      result.profile = widest;
      result.seen = 0xFF;
      result.loadStateBefore = 0xFF;
      result.loadStateEdit = 0xFF;
      result.loadStateAfter = 0xFF;
      const SimSunSaver fresh;
      ss::buildPlan(snapshotOf(fresh), ss::lifepo4Profile(), ss::SCALE_100, result.entries);
      result.planned = ss::PLAN_ENTRIES;
      for (uint8_t i = 0; i < ss::PLAN_ENTRIES; ++i) {
        result.entries[i].status = ss::ENTRY_STATUS_LAST;
      }
      JsonDocument file;
      ss::encodeJournal(longest, file.to<JsonObject>());
      CHECK(!file.overflowed());
      const size_t length = measureJson(file);
      CHECK_MSG(length <= ss::JOURNAL_JSON_MAX, "the longest journal is %zu characters", length);
      CHECK_MSG(length + 64 > ss::JOURNAL_JSON_MAX, "the longest journal is %zu characters: the bound is loose", length);
    }

    // A file that is no journal: the client may owe the controller a restart.
    static const char *const kNoJournals[] = {"{}",
                                              "{\"req\":5}",
                                              "{\"ph\":1}",
                                              "{\"req\":\"5\",\"ph\":1}",
                                              "{\"req\":5,\"ph\":\"1\"}",
                                              "{\"req\":5,\"ph\":3}",
                                              "{\"req\":5,\"ph\":-1}",
                                              "{\"req\":5,\"ph\":1.5}",
                                              "{\"req\":-5,\"ph\":1}",
                                              "{\"req\":4294967296,\"ph\":0}",
                                              "[5,1]",
                                              "null",
                                              "\"journal\""};
    for (size_t i = 0; i < sizeof(kNoJournals) / sizeof(kNoJournals[0]); ++i) {
      JsonDocument file = parsed(kNoJournals[i]);
      const ss::Journal in = ss::decodeJournal(file.as<JsonObjectConst>());
      CHECK_MSG(in.phase == ss::JOURNAL_WRITING && in.requestId == 0, "%s reads as phase %u of request %u", kNoJournals[i],
                (unsigned)in.phase, (unsigned)in.requestId);
    }
    JsonDocument idle = parsed("{\"req\":0,\"ph\":0,\"more\":true}");
    CHECK_EQ(ss::decodeJournal(idle.as<JsonObjectConst>()).phase, ss::JOURNAL_IDLE);
  }

  // The values as a person reads them.
  {
    const ss::Profile profile = ss::lifepo4Profile();
    static const char *const kWant[ss::PROFILE_FIELDS][2] = {
        {"Absorption voltage", "14.20 V"},      {"Float voltage", "13.50 V"},
        {"Absorption time", "15 min"},          {"High voltage disconnect", "14.45 V"},
        {"High voltage reconnect", "13.60 V"},  {"Low voltage disconnect (LVD)", "13.10 V"},
        {"Low voltage reconnect (LVR)", "13.25 V"}};
    for (uint8_t field = 0; field < ss::PROFILE_FIELDS; ++field) {
      CHECK_TEXT(std::string(ss::profileFieldLabel(field)), std::string(kWant[field][0]));
      CHECK_TEXT(textFrom([&](ss::Text *t) { ss::describeProfileField(profile, field, t); }), std::string(kWant[field][1]));
      CHECK(plainAscii(ss::profileFieldLabel(field)));
    }
    CHECK_TEXT(std::string(ss::profileFieldLabel(ss::PROFILE_FIELDS)), std::string("?"));
    CHECK_EQ(ss::profileField(profile, ss::PROFILE_FIELDS), 0);
    ss::Profile odd = profile;
    odd.absorbSeconds = 901;
    CHECK_TEXT(textFrom([&](ss::Text *t) { ss::describeProfileField(odd, 2, t); }), std::string("901 s"));
    odd.absorbSeconds = 60;
    CHECK_TEXT(textFrom([&](ss::Text *t) { ss::describeProfileField(odd, 2, t); }), std::string("1 min"));
    odd.absorbSeconds = 0;
    CHECK_TEXT(textFrom([&](ss::Text *t) { ss::describeProfileField(odd, 2, t); }), std::string("0 min"));
    odd.lvdCv = 5;
    CHECK_TEXT(textFrom([&](ss::Text *t) { ss::describeProfileField(odd, 5, t); }), std::string("0.05 V"));
  }
}

// ============================================================================
// The result of a run as JSON
// ============================================================================

static bool sameResult(const ss::ProgramResult &a, const ss::ProgramResult &b) {
  if (a.outcome != b.outcome || a.restart != b.restart || a.scale != b.scale || a.liveScale != b.liveScale ||
      a.planned != b.planned || a.seen != b.seen || a.loadStateBefore != b.loadStateBefore ||
      a.loadStateEdit != b.loadStateEdit || a.loadStateAfter != b.loadStateAfter || a.probes != b.probes ||
      a.wroteAny != b.wroteAny || a.followUp != b.followUp || a.profileErrors != b.profileErrors || a.notes != b.notes ||
      a.refusals != b.refusals || !sameProfile(a.profile, b.profile) || a.batteryRaw != b.batteryRaw ||
      a.loadCurrentRaw != b.loadCurrentRaw || a.dip != b.dip || a.arrayFault != b.arrayFault || a.loadFault != b.loadFault) {
    return false;
  }
  for (uint8_t i = 0; i < ss::PLAN_ENTRIES; ++i) {
    const ss::PlanEntry &x = a.entries[i];
    const ss::PlanEntry &y = b.entries[i];
    if (x.item != y.item || x.status != y.status || x.before != y.before || x.target != y.target || x.found != y.found) {
      return false;
    }
  }
  return true;
}

static std::string wireOf(const ss::ProgramResult &result) {
  JsonDocument doc;
  ss::encodeProgramResult(result, doc.to<JsonObject>());
  CHECK(!doc.overflowed());
  std::string wire;
  serializeJson(doc, wire);
  return wire;
}

static bool decodes(const std::string &wire, ss::ProgramResult *result) {
  JsonDocument doc = parsed(wire);
  return ss::decodeProgramResult(doc.as<JsonObjectConst>(), result);
}

// The way of a result: from the client in a note, into the server's file, and out of it again.
static void checkRoundTrip(const ss::ProgramResult &result, const char *what) {
  const std::string wire = wireOf(result);
  CHECK_MSG(plainAscii(wire.c_str()), "%s", what);
  ss::ProgramResult decoded;
  memset(&decoded, 0x5A, sizeof(decoded));
  CHECK_MSG(decodes(wire, &decoded), "%s: its own result does not decode: %s", what, wire.c_str());
  CHECK_MSG(sameResult(decoded, result), "%s: the result changed on its way: %s", what, wire.c_str());
  CHECK_MSG(wireOf(decoded) == wire, "%s: encoded again it reads differently", what);
}

template <typename Change>
static std::string changed(const std::string &wire, Change change) {
  JsonDocument doc = parsed(wire);
  change(doc.as<JsonObject>());
  std::string out;
  serializeJson(doc, out);
  return out;
}

static void checkUnreadable(const std::string &wire, const char *what) {
  ss::ProgramResult decoded;
  memset(&decoded, 0x5A, sizeof(decoded));
  CHECK_MSG(!decodes(wire, &decoded), "%s: read as a result", what);
  ss::ProgramResult cleared;
  ss::clearResult(&cleared);
  CHECK_MSG(sameResult(decoded, cleared), "%s: not read, and something of it was left behind", what);
}

static std::string describedAs(const ss::ProgramResult &result, JsonDocument *page) {
  ss::describeProgramResult(result, page->to<JsonObject>());
  CHECK(!page->overflowed());
  std::string shown;
  serializeJson(*page, shown);
  CHECK(plainAscii(shown.c_str()));
  return shown;
}

static void testResultJson() {
  Station quiet;
  const ss::ProgramResult programmed = quiet.run();
  CHECK_EQ(programmed.outcome, ss::OUTCOME_PROGRAMMED);

  // The note a client sends.
  {
    JsonDocument note;
    JsonObject body = note.to<JsonObject>();
    body["c"] = "dev:860322068012345";
    body["st"] = "chg-prog";
    body["epoch"] = 1795500042u;
    body["req"] = kRequestId;
    ss::encodeProgramResult(programmed, body);
    body["_sv"] = 1;
    body["fv"] = "2.2.16";
    CHECK(!note.overflowed());
    std::string wire;
    serializeJson(note, wire);
    // The client's note buffer is 2048 bytes.
    CHECK_MSG(wire.size() < 640, "the note is %zu bytes", wire.size());
    CHECK(plainAscii(wire.c_str()));
    CHECK(body["c"] == "dev:860322068012345");  // what was there stays
    CHECK(body["st"] == "chg-prog");
    CHECK_HAS(wire, "\"play\":1,\"o\":1,\"rs\":1,\"w\":true,\"sc\":[1,1],\"rf\":0,\"pe\":0,\"nt\":1,\"pb\":");
    CHECK_HAS(wire, "\"p\":[1420,1350,900,1445,1360,1310,1325]");
    CHECK_HAS(wire, "\"sn\":15,\"m\":\"");
    CHECK_HAS(wire, "\"ld\":[1,1,1]");
    CHECK_HASNT(wire, "\"fu\"");
    const char *plan = body["en"].as<const char *>();
    CHECK(plan != nullptr && strlen(plan) == (size_t)ss::PLAN_ENTRIES * ss::PROGRAM_ENTRY_HEX_CHARS);
    // EV_eq comes first and was 0 already: item, status "already set", and three times 0.
    char first[17];
    snprintf(first, sizeof(first), "%02X%02X000000000000", (unsigned)ss::I_EV_EQ, (unsigned)ss::ENTRY_UNCHANGED);
    CHECK(plan != nullptr && strncmp(plan, first, 16) == 0);
    const char *readings = body["m"].as<const char *>();
    CHECK(readings != nullptr && strlen(readings) == 20);

    JsonDocument received = parsed(wire);
    ss::ProgramResult decoded;
    CHECK(ss::decodeProgramResult(received.as<JsonObjectConst>(), &decoded));
    CHECK(sameResult(decoded, programmed));
    CHECK_EQ(received["req"].as<uint32_t>(), kRequestId);
  }
  CHECK_EQ(ss::PROGRAM_LAYOUT, 1);

  // Every kind of result makes the round trip.
  checkRoundTrip(programmed, "programmed");
  {
    const ss::ProgramResult unchanged = quiet.run();
    CHECK_EQ(unchanged.outcome, ss::OUTCOME_UNCHANGED);
    checkRoundTrip(unchanged, "unchanged");
    CHECK_HASNT(wireOf(unchanged), "\"w\"");
  }
  {
    Station station;
    station.sim.regs[0x0008] = station.sim.volts(12.80);
    const ss::ProgramResult refused = station.run(ss::lifepo4Profile(), false);
    CHECK_EQ(refused.refusals, ss::REFUSE_BATTERY_LOW | ss::REFUSE_LOAD_CURRENT);
    checkRoundTrip(refused, "refused by the run");
    CHECK_HASNT(wireOf(refused), "\"en\"");  // it made no plan
  }
  {
    ss::ProgramResult result;
    ss::refusedResult(ss::lifepo4Profile(), ss::PROFILE_LVD | ss::PROFILE_GRID, ss::REFUSE_PROFILE | ss::REFUSE_NOT_BUILT, &result);
    checkRoundTrip(result, "refused by the client");
    ss::interruptedResult(&result);
    checkRoundTrip(result, "interrupted");
  }
  {
    // A recovery that found the controller waiting.
    Station station;
    station.sim.regs[A_ARRAY_FAULT] = EDIT_ARRAY;
    station.programmer.startRecovery(true);
    station.drive();
    checkRoundTrip(station.programmer.result(), "a recovery");
  }
  {
    // A run whose restart found no answer: the client owes the controller another look. (A short
    // timeout, so that BACK_PROBES ends the wait and not the time limit.)
    Station station;
    station.master.responseTimeoutMs = 300;
    station.bus.before = [&station](size_t, char kind, uint16_t) {
      if (kind == 'c' && station.bus.count('w') > 0) {
        station.sim.powered = false;
      }
    };
    const ss::ProgramResult open = station.run();
    CHECK(open.followUp);
    CHECK_EQ(open.outcome, ss::OUTCOME_RESTART_FAILED);
    CHECK_EQ(open.probes, ss::BACK_PROBES);
    checkRoundTrip(open, "restart not confirmed");
    CHECK_HAS(wireOf(open), "\"fu\":true");
    CHECK_HAS(wireOf(open), "\"pb\":20");
  }
  // Every value a field can take.
  for (uint8_t outcome = 1; outcome <= ss::OUTCOME_LAST; ++outcome) {
    for (uint8_t restart = 0; restart <= ss::RESTART_STATUS_LAST; ++restart) {
      ss::ProgramResult result = programmed;
      result.outcome = outcome;
      result.restart = restart;
      result.followUp = (outcome + restart) % 2 == 0;
      result.wroteAny = outcome % 2 == 0;
      result.scale = (uint8_t)(outcome % 3);
      result.liveScale = (uint8_t)(restart % 3);
      checkRoundTrip(result, "an outcome with a restart status");
    }
  }
  for (uint8_t status = 0; status <= ss::ENTRY_STATUS_LAST; ++status) {
    ss::ProgramResult result = programmed;
    for (uint8_t i = 0; i < ss::PLAN_ENTRIES; ++i) {
      result.entries[i].status = (uint8_t)((status + i) % (ss::ENTRY_STATUS_LAST + 1));
      result.entries[i].found = (uint16_t)(0xFFFF - i * 257u);
      result.entries[i].before = (uint16_t)(i * 4099u);
    }
    checkRoundTrip(result, "entries of every status");
  }
  {
    ss::ProgramResult result = programmed;
    result.refusals = 0xFFFFFFFFul;
    result.notes = 0xFFFF;
    result.profileErrors = 0xFFFF;
    result.seen = ss::SEEN_BATTERY | ss::SEEN_LOAD_CURRENT | ss::SEEN_DIP | ss::SEEN_FAULTS;
    result.probes = 0xFF;
    result.loadStateBefore = 0;
    result.loadStateEdit = ss::LOAD_STATE_OTHER;
    result.loadStateAfter = ss::LOAD_STATE_NOT_READ;
    const ss::Profile top = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    result.profile = top;
    result.batteryRaw = 0xFFFF;
    result.loadCurrentRaw = 0x8000;
    result.dip = 0x00FF;
    result.arrayFault = 0xABCD;
    result.loadFault = 0x0123;
    checkRoundTrip(result, "the highest values");
    ss::ProgramResult low;
    ss::clearResult(&low);
    low.outcome = ss::OUTCOME_NO_LINK;
    checkRoundTrip(low, "nothing but an outcome");
    // Each reading keeps its place.
    for (uint8_t word = 0; word < 5; ++word) {
      ss::ProgramResult one = low;
      one.seen = (uint8_t)(1u << (word < 3 ? word : 3));
      (word == 0 ? one.batteryRaw : word == 1 ? one.loadCurrentRaw : word == 2 ? one.dip : word == 3 ? one.arrayFault : one.loadFault) =
          (uint16_t)(0x1111u * (word + 1u));
      checkRoundTrip(one, "one reading");
    }
    for (uint8_t field = 0; field < ss::PROFILE_FIELDS; ++field) {
      ss::ProgramResult one = low;
      uint16_t *fields[ss::PROFILE_FIELDS] = {&one.profile.absorbCv, &one.profile.floatCv, &one.profile.absorbSeconds,
                                              &one.profile.hvdCv,    &one.profile.hvrCv,   &one.profile.lvdCv,
                                              &one.profile.lvrCv};
      *fields[field] = (uint16_t)(1000u + field);
      checkRoundTrip(one, "one value of the request");
      CHECK_EQ(ss::profileField(one.profile, field), 1000u + field);
    }
  }

  // What is no result is not read as one, and leaves nothing behind.
  const std::string wire = wireOf(programmed);
  {
    JsonDocument none;
    ss::ProgramResult decoded;
    memset(&decoded, 0x5A, sizeof(decoded));
    CHECK(!ss::decodeProgramResult(none.as<JsonObjectConst>(), &decoded));
    CHECK_EQ(decoded.outcome, ss::OUTCOME_NONE);
    CHECK_EQ(decoded.loadStateEdit, ss::LOAD_STATE_NOT_READ);
  }
  checkUnreadable("{}", "an empty object");
  checkUnreadable("[1,2]", "an array");
  checkUnreadable(changed(wire, [](JsonObject o) { o.remove("play"); }), "no layout");
  checkUnreadable(changed(wire, [](JsonObject o) { o["play"] = 2; }), "a newer layout");
  checkUnreadable(changed(wire, [](JsonObject o) { o["play"] = 0; }), "layout 0");
  checkUnreadable(changed(wire, [](JsonObject o) { o["play"] = "1"; }), "the layout as text");
  checkUnreadable(changed(wire, [](JsonObject o) { o.remove("o"); }), "no outcome");
  checkUnreadable(changed(wire, [](JsonObject o) { o["o"] = 0; }), "the outcome of no run");
  checkUnreadable(changed(wire, [](JsonObject o) { o["o"] = ss::OUTCOME_LAST + 1; }), "an outcome of a newer client");
  checkUnreadable(changed(wire, [](JsonObject o) { o["o"] = -1; }), "a negative outcome");
  checkUnreadable(changed(wire, [](JsonObject o) { o["o"] = "1"; }), "the outcome as text");
  checkUnreadable(changed(wire, [](JsonObject o) { o.remove("rs"); }), "no restart status");
  checkUnreadable(changed(wire, [](JsonObject o) { o["rs"] = ss::RESTART_STATUS_LAST + 1; }), "a restart status of a newer client");
  checkUnreadable(changed(wire, [](JsonObject o) { o.remove("p"); }), "no values");
  checkUnreadable(changed(wire, [](JsonObject o) { o["p"].remove(6); }), "six values");
  checkUnreadable(changed(wire, [](JsonObject o) { o["p"].add(1); }), "eight values");
  checkUnreadable(changed(wire, [](JsonObject o) { o["p"][3] = "1445"; }), "a value as text");
  checkUnreadable(changed(wire, [](JsonObject o) { o["p"][3] = 65536; }), "a value above a word");
  checkUnreadable(changed(wire, [](JsonObject o) { o["p"][3] = -1; }), "a negative value");
  checkUnreadable(changed(wire, [](JsonObject o) { o["p"][3] = 14.45; }), "a value with a fraction");
  checkUnreadable(changed(wire, [](JsonObject o) { o["m"] = "0123456789ABCDEF012"; }), "readings one digit short");
  checkUnreadable(changed(wire, [](JsonObject o) { o["m"] = "0123456789ABCDEF01234"; }), "readings one digit long");
  checkUnreadable(changed(wire, [](JsonObject o) { o["m"] = "0123456789ABCDEF012G"; }), "readings with a letter that is no digit");
  checkUnreadable(changed(wire, [](JsonObject o) { o["m"] = ""; }), "empty readings");
  for (size_t at = 0; at < (size_t)ss::PLAN_ENTRIES * ss::PROGRAM_ENTRY_HEX_CHARS; at += 7) {
    checkUnreadable(changed(wire, [at](JsonObject o) {
                      std::string plan = o["en"].as<std::string>();
                      plan[at] = 'x';
                      o["en"] = plan;
                    }),
                    "a plan with a letter that is no digit");
  }
  checkUnreadable(changed(wire, [](JsonObject o) { o["en"] = o["en"].as<std::string>().substr(1); }), "a plan one digit short");
  checkUnreadable(changed(wire, [](JsonObject o) { o["en"] = o["en"].as<std::string>() + "0"; }), "a plan one digit long");
  checkUnreadable(changed(wire, [](JsonObject o) { o["en"] = o["en"].as<std::string>().substr(16); }), "a plan one register short");
  checkUnreadable(changed(wire, [](JsonObject o) { o["en"] = ""; }), "an empty plan");
  {
    // A register the plan does not name, one this code does not know, and a status it has no name for.
    char pair[3];
    snprintf(pair, sizeof(pair), "%02X", (unsigned)ss::I_ADC_VB_F);
    const std::string notPlanned = pair;
    for (size_t entry = 0; entry < ss::PLAN_ENTRIES; entry += 4) {
      const size_t at = entry * ss::PROGRAM_ENTRY_HEX_CHARS;
      checkUnreadable(changed(wire, [at, &notPlanned](JsonObject o) {
                        std::string plan = o["en"].as<std::string>();
                        plan.replace(at, 2, notPlanned);
                        o["en"] = plan;
                      }),
                      "a plan with a register it does not name");
      checkUnreadable(changed(wire, [at](JsonObject o) {
                        std::string plan = o["en"].as<std::string>();
                        plan.replace(at, 2, "FF");
                        o["en"] = plan;
                      }),
                      "a plan with a register this code does not know");
      checkUnreadable(changed(wire, [at](JsonObject o) {
                        char unknown[3];
                        snprintf(unknown, sizeof(unknown), "%02X", (unsigned)(ss::ENTRY_STATUS_LAST + 1));
                        std::string plan = o["en"].as<std::string>();
                        plan.replace(at + 2, 2, unknown);
                        o["en"] = plan;
                      }),
                      "a plan with a status this code has no name for");
    }
  }

  // What a result leaves out, or says in a way this code does not know, reads as "not known".
  {
    ss::ProgramResult decoded;
    CHECK(decodes(changed(wire, [](JsonObject o) { o.remove("sc"); }), &decoded));
    CHECK_EQ(decoded.scale, ss::SCALE_UNKNOWN);
    CHECK_EQ(decoded.liveScale, ss::SCALE_UNKNOWN);
    CHECK_EQ(decoded.planned, ss::PLAN_ENTRIES);  // the rest is read
    CHECK(decodes(changed(wire, [](JsonObject o) { o["sc"].add(1); }), &decoded));
    CHECK_EQ(decoded.scale, ss::SCALE_UNKNOWN);
    CHECK(decodes(changed(wire, [](JsonObject o) { o["sc"][0] = 3; o["sc"][1] = "1"; }), &decoded));
    CHECK_EQ(decoded.scale, ss::SCALE_UNKNOWN);
    CHECK_EQ(decoded.liveScale, ss::SCALE_UNKNOWN);
    CHECK(decodes(changed(wire, [](JsonObject o) { o["sc"][0] = 2; o["sc"][1] = 1; }), &decoded));
    CHECK_EQ(decoded.scale, ss::SCALE_96667);
    CHECK_EQ(decoded.liveScale, ss::SCALE_100);

    CHECK(decodes(changed(wire, [](JsonObject o) { o.remove("m"); }), &decoded));
    CHECK_EQ(decoded.seen, 0);  // without the words nothing was seen, whatever "sn" says
    CHECK_EQ(decoded.batteryRaw, 0);
    CHECK(decodes(changed(wire, [](JsonObject o) { o["sn"] = 0xF5; }), &decoded));
    CHECK_EQ(decoded.seen, 0x05);
    CHECK_EQ(decoded.batteryRaw, programmed.batteryRaw);
    CHECK(decodes(changed(wire, [](JsonObject o) { o.remove("sn"); }), &decoded));
    CHECK_EQ(decoded.seen, 0);
    CHECK(decodes(changed(wire, [](JsonObject o) { o["sn"] = "15"; }), &decoded));
    CHECK_EQ(decoded.seen, 0);

    CHECK(decodes(changed(wire, [](JsonObject o) { o.remove("ld"); }), &decoded));
    CHECK_EQ(decoded.loadStateBefore, ss::LOAD_STATE_NOT_READ);
    CHECK_EQ(decoded.loadStateAfter, ss::LOAD_STATE_NOT_READ);
    CHECK(decodes(changed(wire, [](JsonObject o) { o["ld"][1] = 256; o["ld"][2] = "1"; }), &decoded));
    CHECK_EQ(decoded.loadStateBefore, 1);
    CHECK_EQ(decoded.loadStateEdit, ss::LOAD_STATE_NOT_READ);
    CHECK_EQ(decoded.loadStateAfter, ss::LOAD_STATE_NOT_READ);
    CHECK(decodes(changed(wire, [](JsonObject o) { o["ld"].add(1); }), &decoded));
    CHECK_EQ(decoded.loadStateBefore, ss::LOAD_STATE_NOT_READ);

    CHECK(decodes(changed(wire, [](JsonObject o) { o.remove("rf"); o.remove("pe"); o.remove("nt"); o.remove("pb"); }), &decoded));
    CHECK_EQ(decoded.refusals, 0);
    CHECK_EQ(decoded.profileErrors, 0);
    CHECK_EQ(decoded.notes, 0);
    CHECK_EQ(decoded.probes, 0);
    CHECK(decodes(changed(wire, [](JsonObject o) { o["rf"] = "1"; o["pe"] = 65536; o["nt"] = -1; o["pb"] = 256; }), &decoded));
    CHECK_EQ(decoded.refusals, 0);
    CHECK_EQ(decoded.profileErrors, 0);
    CHECK_EQ(decoded.notes, 0);
    CHECK_EQ(decoded.probes, 0);
    // Bits of a newer client are kept, and shown as "no text for".
    CHECK(decodes(changed(wire, [](JsonObject o) { o["rf"] = 0x80000001ul; o["pe"] = 0x8001; o["nt"] = 0x8001; }), &decoded));
    CHECK_EQ(decoded.refusals, 0x80000001ul);
    CHECK_EQ(decoded.profileErrors, 0x8001);
    CHECK_EQ(decoded.notes, 0x8001);

    CHECK(decodes(changed(wire, [](JsonObject o) { o["fu"] = 1; o["w"] = "true"; }), &decoded));
    CHECK(!decoded.followUp);
    CHECK(!decoded.wroteAny);
    CHECK(decodes(changed(wire, [](JsonObject o) { o["fu"] = true; o.remove("w"); }), &decoded));
    CHECK(decoded.followUp);
    CHECK(!decoded.wroteAny);
    CHECK(decodes(changed(wire, [](JsonObject o) { o.remove("en"); }), &decoded));
    CHECK_EQ(decoded.planned, 0);
    CHECK_EQ(decoded.entries[3].target, 0);
    // Members this code does not know are ignored.
    CHECK(decodes(changed(wire, [](JsonObject o) { o["zz"] = "new"; o["c"] = "dev:1"; }), &decoded));
    CHECK(sameResult(decoded, programmed));
  }

  // Which result a server keeps for a client.
  CHECK(ss::programResultReplaces(false, 0, 0.0, 0, 0.0));
  CHECK(ss::programResultReplaces(false, 500, 9.0e9, 1, 0.0));
  CHECK(ss::programResultReplaces(true, 500, 1700000000.0, 501, 1600000000.0));  // a newer request, whatever the times
  CHECK(ss::programResultReplaces(true, 500, 1700000000.0, 501, 0.0));
  CHECK(!ss::programResultReplaces(true, 500, 1700000000.0, 499, 1800000000.0));  // an older one, whatever the times
  CHECK(!ss::programResultReplaces(true, 500, 0.0, 499, 1800000000.0));
  CHECK(ss::programResultReplaces(true, 500, 1700000000.0, 500, 1700000600.0));  // the second report of a request
  CHECK(ss::programResultReplaces(true, 500, 1700000000.0, 500, 1700000000.0));  // the same report again
  CHECK(!ss::programResultReplaces(true, 500, 1700000600.0, 500, 1700000000.0)); // its first report, arriving late
  CHECK(!ss::programResultReplaces(true, 500, 1700000600.0, 500, 0.0));          // one without a time
  CHECK(ss::programResultReplaces(true, 500, 0.0, 500, 0.0));
  CHECK(ss::programResultReplaces(true, 0xFFFFFFFEu, 0.0, 0xFFFFFFFFu, 0.0));
  CHECK(!ss::programResultReplaces(true, 0xFFFFFFFFu, 0.0, 0, 9.0e9));

  // For a person: a run that went well.
  {
    JsonDocument page;
    const std::string shown = describedAs(programmed, &page);
    CHECK_MSG(shown.size() < 3200, "the readable result is %zu bytes", shown.size());
    CHECK_EQ(page["o"].as<int>(), ss::OUTCOME_PROGRAMMED);
    CHECK(page["outcome"] == "programmed");
    CHECK(page["good"] == true);
    CHECK(page["text"] == "Programmed: the charger holds the settings and has restarted.");
    CHECK(page["summary"] == "programmed: 15 of 17 registers written, 2 already set");
    CHECK(page["restart"] == "The charger restarted.");
    CHECK(page["followUp"].isNull());
    CHECK(page["battery"] == "13.68 V");
    CHECK(page["load"] == "LOAD output: LOAD_ON before the run, LOAD_ON while the charger waited for its restart, LOAD_ON at the end.");
    CHECK_EQ(page["reasons"].size(), 0);
    CHECK(page["reasons"].is<JsonArrayConst>());  // an empty list, not a missing one
    CHECK_EQ(page["notes"].size(), 1);
    CHECK(has(page["notes"][0].as<const char *>(), "DIP switch 1 is OFF"));
    JsonArrayConst asked = page["asked"];
    CHECK_EQ(asked.size(), ss::PROFILE_FIELDS);
    CHECK_HAS(shown, "\"asked\":[[\"Absorption voltage\",\"14.20 V\"],[\"Float voltage\",\"13.50 V\"],[\"Absorption time\",\"15 min\"],");
    CHECK_HAS(shown, "[\"Low voltage disconnect (LVD)\",\"13.10 V\"],[\"Low voltage reconnect (LVR)\",\"13.25 V\"]]");
    JsonArrayConst registers = page["registers"];
    CHECK_EQ(registers.size(), ss::PLAN_ENTRIES);
    for (size_t i = 0; i < registers.size(); ++i) {
      CHECK_EQ(registers[i].size(), 5);
      CHECK(registers[i][0] == ss::item(programmed.entries[i].item).name);
      CHECK(registers[i][1] == ss::item(programmed.entries[i].item).label);
    }
    CHECK_HAS(shown, "[\"EV_eq\",\"Equalize voltage at 25 C\",\"0 (off)\",\"0 (off)\",\"already set\"]");
    const std::string lvdRow = "[\"EV_lvd\",\"" + std::string(ss::item(ss::I_EV_LVD).label) + "\",\"11.50 V\",\"13.10 V\",\"written\"]";
    CHECK_HAS(shown, lvdRow.c_str());
    CHECK_HAS(shown, "\"900 s (15 min)\",\"written\"]");
  }
  // One that was refused, with the figures behind the reasons.
  {
    Station station;
    station.sim.regs[0x0008] = station.sim.volts(12.80);
    const ss::ProgramResult refused = station.run(ss::lifepo4Profile(), false);
    JsonDocument page;
    const std::string shown = describedAs(refused, &page);
    CHECK(page["outcome"] == "refused");
    CHECK(page["good"] == false);
    CHECK(page["summary"] == "refused");
    CHECK(page["restart"].isNull());
    CHECK(page["battery"] == "12.80 V");
    CHECK_EQ(page["reasons"].size(), 2);
    CHECK(has(page["reasons"][0].as<const char *>(), "(battery 12.80 V, new LVR 13.25 V)"));
    CHECK(has(page["reasons"][1].as<const char *>(), "(0.14 A)"));
    CHECK_EQ(page["registers"].size(), 0);
    CHECK(page["registers"].is<JsonArrayConst>());
    CHECK_EQ(page["asked"].size(), ss::PROFILE_FIELDS);
    CHECK(page["load"] == "LOAD output: LOAD_ON before the run.");
  }
  // Values that are outside the limits: the reason, then what is wrong with each.
  {
    ss::ProgramResult result;
    ss::Profile wanted = ss::lifepo4Profile();
    wanted.lvdCv = 900;
    wanted.absorbSeconds = 30;
    ss::refusedResult(wanted, ss::PROFILE_LVD | ss::PROFILE_TIME, ss::REFUSE_PROFILE, &result);
    JsonDocument page;
    const std::string shown = describedAs(result, &page);
    CHECK_EQ(page["reasons"].size(), 3);
    CHECK(page["reasons"][0] == ss::refusalText(ss::REFUSE_PROFILE));
    CHECK(page["reasons"][1] == ss::profileErrorText(ss::PROFILE_TIME));
    CHECK(page["reasons"][2] == ss::profileErrorText(ss::PROFILE_LVD));
    CHECK(page["battery"].isNull());
    CHECK(page["load"].isNull());
    CHECK_HAS(shown, "[\"Absorption time\",\"30 s\"]");
    CHECK_HAS(shown, "[\"Low voltage disconnect (LVD)\",\"9.00 V\"]");
    CHECK_EQ(page["notes"].size(), 0);
  }
  // A client that restarted: it holds no values, and says what it knows.
  {
    ss::ProgramResult result;
    ss::interruptedResult(&result);
    JsonDocument page;
    describedAs(result, &page);
    CHECK(page["outcome"] == "interrupted");
    CHECK(page["asked"].isNull());
    CHECK_EQ(page["notes"].size(), 1);
    CHECK(page["notes"][0] == "Nothing had been written.");
    CHECK_EQ(page["registers"].size(), 0);
    CHECK(page["restart"].isNull());
  }
  // The restart is not confirmed, a register lost its word, another was altered, and the
  // battery reading has no proven scale.
  {
    ss::ProgramResult result = programmed;
    result.outcome = ss::OUTCOME_RESTART_FAILED;
    result.restart = ss::RESTART_NO_ANSWER;
    result.followUp = true;
    result.liveScale = ss::SCALE_UNKNOWN;
    result.notes = ss::NOTE_VERIFY_INCOMPLETE | ss::NOTE_SW3_ON;
    result.entries[15].status = ss::ENTRY_LOST;  // EV_lvd
    result.entries[15].found = result.entries[15].before;
    result.entries[14].status = ss::ENTRY_ALTERED;  // EV_lvr
    result.entries[14].found = quiet.sim.setVolts(12.00);
    JsonDocument page;
    const std::string shown = describedAs(result, &page);
    CHECK(page["followUp"] == true);
    CHECK(page["good"] == false);
    CHECK(page["restart"] == "The charger has not answered since the restart command.");
    CHECK_TEXT(page["battery"].as<std::string>(), std::string("13.23 V or 13.68 V: how this charger encodes its live readings is not proven"));
    CHECK_EQ(page["notes"].size(), 2);
    CHECK(has(page["notes"][0].as<const char *>(), "DIP switch 3 is ON"));
    CHECK(has(page["notes"][1].as<const char *>(), "Not every register could be read again"));
    CHECK_HAS(shown, "\"11.50 V\",\"13.10 V\",\"lost after the restart: it holds 11.50 V\"]");
    CHECK_HAS(shown, "\"12.60 V\",\"13.25 V\",\"altered by the charger: it holds 12.00 V\"]");
    CHECK(page["summary"] == "restart failed: 13 of 17 registers written, 2 already set");
  }
  // Bits a newer client set are shown, as what they are.
  {
    ss::ProgramResult result = programmed;
    result.outcome = ss::OUTCOME_REFUSED;
    result.refusals = 0x80000000ul | ss::REFUSE_FAULT;
    result.profileErrors = 0x8000;
    result.notes = 0x8000;
    result.arrayFault = 0x0001;
    result.loadFault = 0x0002;
    JsonDocument page;
    describedAs(result, &page);
    CHECK_EQ(page["reasons"].size(), 3);
    CHECK(has(page["reasons"][0].as<const char *>(), "(array faults 0x0001, load faults 0x0002)"));
    CHECK(has(page["reasons"][1].as<const char *>(), "no text for"));
    CHECK(has(page["reasons"][2].as<const char *>(), "no text for"));
    CHECK_EQ(page["notes"].size(), 1);
    CHECK(has(page["notes"][0].as<const char *>(), "no text for"));
  }
  // The longest result there can be still fits its line buffer and a page.
  {
    ss::ProgramResult result = programmed;
    result.outcome = ss::OUTCOME_STOPPED;
    result.refusals = 0xFFFFFFFFul;
    result.notes = 0xFFFF;
    result.profileErrors = 0xFFFF;
    result.liveScale = ss::SCALE_UNKNOWN;
    result.scale = ss::SCALE_UNKNOWN;
    result.followUp = true;
    result.restart = ss::RESTART_UNCONFIRMED;
    for (uint8_t i = 0; i < ss::PLAN_ENTRIES; ++i) {
      result.entries[i].status = ss::ENTRY_ALTERED;
      result.entries[i].found = 0xFFFF;
      result.entries[i].before = 0xFFFE;
    }
    JsonDocument page;
    const std::string shown = describedAs(result, &page);
    CHECK_MSG(shown.size() < 12000, "the longest readable result is %zu bytes", shown.size());
    CHECK_EQ(page["reasons"].size(), 32 + 16);
    CHECK_EQ(page["notes"].size(), 16);
    // No sentence was cut short: each ends as its text does.
    const std::string low = textFrom([&](ss::Text *t) { ss::describeRefusal(ss::REFUSE_BATTERY_LOW, result, t); });
    CHECK(page["reasons"][5] == low.c_str());
    CHECK_HAS(low, "at the lower of its two possible readings, new LVR 13.25 V)");
    for (size_t i = 0; i < page["registers"].size(); ++i) {
      const std::string last = page["registers"][i][4].as<std::string>();
      CHECK_MSG(last.size() > 20 && last.size() < 300, "row %u ends with %zu characters", (unsigned)i, last.size());
    }
  }
}

// ============================================================================
// Requests, the journal, and a client that restarts in the middle of a run
// ============================================================================

// Thrown by a Desk to cut the client off in the middle of whatever it is doing.
struct PowerLoss {};

static const size_t kNever = (size_t)-1;
static const uint64_t kSimIdleMs = 5000;  // see Desk: longer than the longest Modbus timeout

struct Report {
  uint32_t requestId;
  uint32_t atMs;
  uint32_t epoch;  // Supervisor::reportEpoch()
  ss::ProgramResult result;
};

// A client as the Supervisor sees it: the controller on its line, a flash that holds the
// journal, a clock, and a loop that calls service() and sends what is due.
//
// The client has its own clock. The simulator's moves with it while a run waits on the line or
// for a reply, and by a few seconds at most while the client does other things: its clock
// refuses to pass a simulated day, and these tests wait for days. Every gap the simulator sees
// is still at least as long as the client waited, up to those few seconds.
static bool sameJournal(const ss::Journal &a, uint32_t requestId, uint8_t phase) {
  return a.requestId == requestId && a.phase == phase;
}

// The journal holds `requestId` as the request that waits: taken on, its run not yet at a
// commit point.
static bool holdsWaiting(const ss::Journal &a, uint32_t requestId) { return requestId != 0 && a.waitingId == requestId; }

// The journal of a client that has answered `requestId` and owes nothing more: no request
// waits, no result is due, and the controller waits for nothing from it.
static bool settledOn(const ss::Journal &a, uint32_t requestId) {
  return a.answeredId == requestId && a.phase == ss::JOURNAL_IDLE && a.waitingId == 0 && !a.reportDue;
}

// A journal as a client wrote it: the last run that reached a commit point and how far it came.
static ss::Journal journalOf(uint32_t requestId, uint8_t phase) {
  ss::Journal journal = ss::emptyJournal();
  journal.requestId = requestId;
  journal.phase = phase;
  journal.controller = 1;
  journal.answeredId = requestId;
  return journal;
}

struct Desk : ss::Port, ss::JournalStore {
  SimSunSaver sim;
  ModbusMaster master;
  TestBus bus;

  ss::Journal flash;                // what the client's flash holds
  std::vector<ss::Journal> saves;   // every journal the supervisor wanted written, in order
  std::vector<size_t> saveAt;       // and how many requests had gone out by then
  unsigned failSaves;               // this many of the next writes fail: flash keeps what it holds
  size_t failSave;                  // or the write with this index does
  size_t failFrom;                  // or every write from this index on
  size_t dieAtRequest;              // the client loses power as it is about to make this request
  size_t dieAtSave;                 // or to write this journal
  bool dieAfterSave;                // or just after flash took it
  std::function<void()> onDeath;    // what else happens at that moment

  uint64_t clockMs;                 // the client's clock: milliseconds since the test began
  uint64_t bootedAt;                // clockMs when the client started
  uint32_t millisAtBoot;            // what millis() reads at that moment: to cross its wrap
  bool clockSet;
  double epochBase;                 // the wall clock when the simulator's clock read 0
  bool ready;
  std::vector<Report> reports;

  Desk()
      : master(sim),
        bus(sim, master),
        failSaves(0),
        failSave(kNever),
        failFrom(kNever),
        dieAtRequest(kNever),
        dieAtSave(kNever),
        dieAfterSave(false),
        clockMs(0),
        bootedAt(0),
        millisAtBoot(0),
        clockSet(true),
        epochBase(kClientNow),
        ready(true) {
    sim.configure(9600, MB_8N2);
    flash = ss::emptyJournal();
  }

  // The client waits for each reply, or for its timeout: that time is the client's too.
  ss::IoStatus readRegister(uint16_t addr, uint16_t *raw) override {
    cut();
    const uint64_t began = sim.elapsedMs();
    const ss::IoStatus status = bus.readRegister(addr, raw);
    clockMs += sim.elapsedMs() - began;
    return status;
  }
  ss::IoStatus writeRegister(uint16_t addr, uint16_t raw) override {
    cut();
    const uint64_t began = sim.elapsedMs();
    const ss::IoStatus status = bus.writeRegister(addr, raw);
    clockMs += sim.elapsedMs() - began;
    return status;
  }
  ss::IoStatus writeCoil(uint16_t addr) override {
    cut();
    const uint64_t began = sim.elapsedMs();
    const ss::IoStatus status = bus.writeCoil(addr);
    clockMs += sim.elapsedMs() - began;
    return status;
  }
  uint8_t controller() const override { return bus.slave; }
  uint16_t timeoutMs() const override { return (uint16_t)master.responseTimeoutMs; }
  bool saveJournal(const ss::Journal &journal) override {
    const size_t index = saves.size();
    if (index == dieAtSave && !dieAfterSave) {
      die();
    }
    saves.push_back(journal);
    saveAt.push_back(bus.log.size());
    if (failSaves > 0 || index == failSave || (failFrom != kNever && index >= failFrom)) {
      failSaves -= failSaves > 0 ? 1 : 0;
      return false;
    }
    flash = journal;
    if (index == dieAtSave) {
      die();
    }
    return true;
  }
  void rest(uint16_t ms, bool afterMiss) override {
    uint32_t wait = ms > 20 ? ms : 20;
    if (afterMiss && wait < master.responseTimeoutMs) {
      wait = master.responseTimeoutMs;
    }
    sim.advance(wait);
    clockMs += wait;
  }
  uint32_t millisNow() override { return millisAtBoot + (uint32_t)(clockMs - bootedAt); }

  // Time passes while the client does other things.
  void advance(uint64_t ms) {
    clockMs += ms;
    const uint64_t last = bus.log.empty() ? 0 : bus.log.back().end;
    const uint64_t since = sim.elapsedMs() - last;
    if (since < kSimIdleMs) {
      const uint64_t step = ms < kSimIdleMs - since ? ms : kSimIdleMs - since;
      sim.advance((uint32_t)step);
    }
  }

  double clock() const { return clockSet ? epochBase + (double)clockMs / 1000.0 : 0.0; }

  void collect(ss::Supervisor &supervisor) {
    if (supervisor.reportDue()) {
      const Report report = {supervisor.reportRequestId(), supervisor.reportMillis(), supervisor.reportEpoch(),
                             supervisor.report()};
      reports.push_back(report);
      supervisor.reportTaken(millisNow());
      CHECK(!supervisor.reportDue());
    }
  }

  // The client's line rest: nothing is sent for one Modbus timeout after a request that got no
  // reply (SolarManager::lineResting()), and the client tells service() that it is not ready.
  bool lineResting() const {
    if (bus.log.empty() || bus.log.back().status != ss::SSIO_NO_REPLY) {
      return false;
    }
    return sim.elapsedMs() - bus.log.back().end < master.responseTimeoutMs;
  }

  // One pass of the client's loop. True when it talked to the controller.
  //
  // The client lets programming have a pass that a poll would take when wantsLine() says yes. So
  // with a line that is ready it must say yes exactly where service() talks to the controller.
  // Every test checks that here, in every pass.
  bool pass(ss::Supervisor &supervisor) {
    const bool wanted = supervisor.wantsLine(clock(), millisNow());
    const bool lineReady = ready && !lineResting();
    const bool ran = supervisor.service(*this, clock(), lineReady);
    CHECK_MSG(ran == (wanted && lineReady), "wantsLine() said %s, the line was %sready, and service() %s",
              wanted ? "yes" : "no", lineReady ? "" : "not ", ran ? "ran" : "did not run");
    collect(supervisor);
    return ran;
  }

  // The loop for `ms`, a pass every `everyMs`. Returns how many passes talked to the controller.
  unsigned loopFor(ss::Supervisor &supervisor, uint64_t ms, uint32_t everyMs = 1000) {
    unsigned ran = 0;
    const uint64_t until = clockMs + ms;
    while (clockMs < until) {
      ran += pass(supervisor) ? 1u : 0u;
      if (clockMs >= until) {
        break;  // a run takes its time
      }
      const uint64_t left = until - clockMs;
      advance(left < everyMs ? left : everyMs);
    }
    return ran;
  }

  // What Supervisor::request() returns: false for a request it ignores.
  bool ask(ss::Supervisor &supervisor, uint32_t requestId, const ss::Profile &profile = ss::lifepo4Profile(),
           uint16_t profileErrors = 0, uint32_t clientRefusals = 0, bool loadCurrentAllowed = true) {
    const bool taken = supervisor.request(requestId, profile, profileErrors, clientRefusals, loadCurrentAllowed, clock(),
                                          millisNow());
    collect(supervisor);
    return taken;
  }

  // The client starts: its millis() begins at 0 again, and it knows what its flash holds.
  void boot(ss::Supervisor &supervisor) {
    dieAtRequest = kNever;
    dieAtSave = kNever;
    bootedAt = clockMs;
    supervisor.begin(flash, this, millisNow());
    collect(supervisor);
  }

 private:
  void cut() {
    if (bus.log.size() == dieAtRequest) {
      die();
    }
  }
  void die() {
    if (onDeath) {
      onDeath();
    }
    throw PowerLoss();
  }
};

// The controller loses its power and starts again. That does what its restart command does: it
// clears the "EEPROM setting edit" faults and takes the LVD from its EEPROM (see sim_sunsaver.h).
static void powerCycle(SimSunSaver &sim) {
  sim.regs[A_ARRAY_FAULT] = (uint16_t)(sim.regs[A_ARRAY_FAULT] & ~EDIT_ARRAY);
  sim.regs[A_LOAD_FAULT] = (uint16_t)(sim.regs[A_LOAD_FAULT] & ~EDIT_LOAD);
  sim.regs[A_V_LVD] = sim.volts((double)sim.regs[A_EV_LVD] * sim.settingsNumerator / 32768.0);
}

static void testSupervisor() {
  const ss::Profile profile = ss::lifepo4Profile();
  const Sequence quiet = quietRun();
  const size_t firstRestart = ss::ITEM_COUNT;    // the request index of the first restart command
  const size_t firstWrite = ss::ITEM_COUNT + 4;  // and of the first write

  // Nothing asked: nothing done.
  {
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    CHECK(!supervisor.reportDue());
    CHECK(!supervisor.requestWaiting());
    CHECK(!supervisor.dutyOwed());
    CHECK_EQ(desk.loopFor(supervisor, 3 * 86400000ull, 600000), 0);
    CHECK(desk.bus.log.empty());
    CHECK(desk.saves.empty());
    CHECK(desk.reports.empty());
  }

  // A request on a client where nothing stands in its way.
  {
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.advance(5000);
    CHECK(desk.ask(supervisor, kRequestId));
    CHECK(supervisor.requestWaiting());
    CHECK(desk.reports.empty());
    // The request is in flash before the client acknowledges the config that carries it.
    CHECK_EQ(desk.saves.size(), 1);
    CHECK(holdsWaiting(desk.flash, kRequestId) && sameJournal(desk.flash, 0, ss::JOURNAL_IDLE));
    CHECK_EQ(desk.flash.answeredId, kRequestId);
    const uint64_t began = desk.clockMs;
    CHECK(desk.pass(supervisor));
    const uint64_t took = desk.clockMs - began;
    CHECK_MSG(took > 9000 && took < 13000, "the run took %llu ms", (unsigned long long)took);  // 10.7 s
    CHECK(!supervisor.requestWaiting());
    CHECK(!supervisor.dutyOwed());
    CHECK_EQ(desk.reports.size(), 1);
    if (desk.reports.size() == 1) {
      const Report &report = desk.reports[0];
      CHECK_EQ(report.requestId, kRequestId);
      CHECK_EQ(report.result.outcome, ss::OUTCOME_PROGRAMMED);
      CHECK(!report.result.followUp);
      CHECK_EQ(ss::countEntries(report.result, ss::ENTRY_WRITTEN), 15);
      CHECK_EQ(report.atMs, desk.millisNow());  // when the run ended, not when it began
      const double end = desk.clock();
      CHECK_MSG(report.epoch + 1.0 >= end && report.epoch <= end, "the result is dated %u, the run ended at %.3f",
                (unsigned)report.epoch, end);
    }
    checkSequence(desk.bus, quiet);
    checkPausesIn(desk.bus.log, desk.master.responseTimeoutMs);
    CHECK_EQ(desk.bus.disorder, 0);
    CHECK(holdsProfile(desk.sim, profile));
    CHECK(!editPending(desk.sim));
    // The journal says each thing before it is done, and keeps the result until it has gone out.
    CHECK_EQ(desk.saves.size(), 5);
    if (desk.saves.size() == 5) {
      CHECK(holdsWaiting(desk.saves[0], kRequestId) && sameJournal(desk.saves[0], 0, ss::JOURNAL_IDLE));
      CHECK(sameJournal(desk.saves[1], kRequestId, ss::JOURNAL_RESTARTING));
      CHECK(sameJournal(desk.saves[2], kRequestId, ss::JOURNAL_WRITING));
      CHECK(sameJournal(desk.saves[3], kRequestId, ss::JOURNAL_IDLE) && desk.saves[3].reportDue);
      CHECK_EQ(desk.saves[3].reportId, kRequestId);
      CHECK_EQ(desk.saves[3].report.outcome, ss::OUTCOME_PROGRAMMED);
      CHECK_EQ(desk.saves[3].reportEpoch, desk.reports[0].epoch);
      CHECK(sameJournal(desk.saves[4], kRequestId, ss::JOURNAL_IDLE) && !desk.saves[4].reportDue);
      for (size_t i = 0; i < desk.saves.size(); ++i) {
        CHECK_EQ(desk.saves[i].answeredId, kRequestId);
        CHECK_EQ(desk.saves[i].waitingId, i == 0 ? kRequestId : 0u);
        CHECK_EQ(desk.saves[i].controller, i == 0 ? 0 : 1);  // the controller the run talked to
      }
      CHECK_EQ(desk.saveAt[0], 0);
      CHECK_EQ(desk.saveAt[1], firstRestart);
      CHECK_EQ(desk.saveAt[2], firstWrite);
      CHECK_EQ(desk.saveAt[3], quiet.size());
      CHECK_EQ(desk.saveAt[4], quiet.size());
      CHECK_EQ(quiet[firstRestart].first, 'c');
      CHECK_EQ(quiet[firstWrite].first, 'w');
    }
    CHECK(settledOn(desk.flash, kRequestId) && sameJournal(desk.flash, kRequestId, ss::JOURNAL_IDLE));
    CHECK(settledOn(supervisor.journal(), kRequestId) && sameJournal(supervisor.journal(), kRequestId, ss::JOURNAL_IDLE));

    // The same config again, and an older one: nothing.
    CHECK(!desk.ask(supervisor, kRequestId));
    CHECK(!desk.ask(supervisor, kRequestId - 1));
    CHECK(!desk.ask(supervisor, 0));
    CHECK(!supervisor.requestWaiting());
    CHECK_EQ(desk.loopFor(supervisor, 7200000, 60000), 0);
    CHECK_EQ(desk.bus.log.size(), quiet.size());
    CHECK_EQ(desk.saves.size(), 5);
    CHECK_EQ(desk.reports.size(), 1);

    // A new request for the same values, two hours later: a run that finds nothing to do. It
    // comes to no commit point, so the journal still names the run before it.
    CHECK(desk.ask(supervisor, kRequestId + 7200));
    CHECK(desk.pass(supervisor));
    CHECK_EQ(desk.reports.size(), 2);
    CHECK_EQ(desk.reports.back().requestId, kRequestId + 7200);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_UNCHANGED);
    CHECK_EQ(desk.bus.log.size(), quiet.size() + ss::ITEM_COUNT);
    CHECK_EQ(desk.saves.size(), 8);
    if (desk.saves.size() == 8) {
      CHECK(holdsWaiting(desk.saves[5], kRequestId + 7200) && sameJournal(desk.saves[5], kRequestId, ss::JOURNAL_IDLE));
      CHECK(desk.saves[6].reportDue && desk.saves[6].reportId == kRequestId + 7200 && desk.saves[6].waitingId == 0);
      CHECK(settledOn(desk.saves[7], kRequestId + 7200) && sameJournal(desk.saves[7], kRequestId, ss::JOURNAL_IDLE));
    }
    CHECK_EQ(desk.sim.resets, 2);  // and no third restart
  }
  // The request says whether current may flow from the LOAD terminals.
  {
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.ask(supervisor, kRequestId, profile, 0, 0, false);
    CHECK(desk.pass(supervisor));
    CHECK_EQ(desk.reports.size(), 1);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_REFUSED);
    CHECK_EQ(desk.reports.back().result.refusals, ss::REFUSE_LOAD_CURRENT);
    CHECK_EQ(desk.bus.count('c') + desk.bus.count('w'), 0);
    CHECK_EQ(desk.saves.size(), 3);  // taken on, answered, and sent: the run never came to a commit point
    CHECK(settledOn(desk.flash, kRequestId) && sameJournal(desk.flash, 0, ss::JOURNAL_IDLE));
    // And which values it wants.
    ss::Profile wanted = profile;
    wanted.lvdCv = 1250;
    desk.ask(supervisor, kRequestId + 1, wanted);
    CHECK(desk.pass(supervisor));
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(desk.reports.back().result.profile.lvdCv, 1250);
    CHECK(holdsProfile(desk.sim, wanted));
  }

  // Refused as it arrives: the values, or what the client knows about itself. Nothing is sent to
  // the controller. Flash keeps the request's id, and its result until that has gone out.
  {
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    ss::Profile wanted = profile;
    wanted.lvdCv = 900;
    CHECK(desk.ask(supervisor, kRequestId, wanted, ss::PROFILE_LVD));
    CHECK(!supervisor.requestWaiting());
    CHECK_EQ(desk.reports.size(), 1);
    if (desk.reports.size() == 1) {
      CHECK_EQ(desk.reports[0].requestId, kRequestId);
      CHECK_EQ(desk.reports[0].result.outcome, ss::OUTCOME_REFUSED);
      CHECK_EQ(desk.reports[0].result.refusals, ss::REFUSE_PROFILE);
      CHECK_EQ(desk.reports[0].result.profileErrors, ss::PROFILE_LVD);
      CHECK_EQ(desk.reports[0].result.profile.lvdCv, 900);
      CHECK_EQ(desk.reports[0].atMs, desk.millisNow());
    }
    CHECK(!desk.ask(supervisor, kRequestId, wanted, ss::PROFILE_LVD));  // the config again: no second answer
    CHECK_EQ(desk.reports.size(), 1);
    CHECK(desk.ask(supervisor, kRequestId + 1, profile, 0, ss::REFUSE_NOT_LIFEPO4));
    CHECK_EQ(desk.reports.size(), 2);
    CHECK_EQ(desk.reports.back().result.refusals, ss::REFUSE_NOT_LIFEPO4);
    CHECK_EQ(desk.reports.back().result.profileErrors, 0);
    desk.ask(supervisor, kRequestId + 2, wanted, ss::PROFILE_LVD | ss::PROFILE_GRID, ss::REFUSE_NOT_BUILT | ss::REFUSE_NO_CHARGER);
    CHECK_EQ(desk.reports.size(), 3);
    CHECK_EQ(desk.reports.back().result.refusals, ss::REFUSE_PROFILE | ss::REFUSE_NOT_BUILT | ss::REFUSE_NO_CHARGER);
    CHECK_EQ(desk.reports.back().result.profileErrors, ss::PROFILE_LVD | ss::PROFILE_GRID);
    CHECK_EQ(desk.loopFor(supervisor, 7200000, 60000), 0);
    CHECK(desk.bus.log.empty());
    CHECK_EQ(desk.saves.size(), 3 * 2);  // each with its result, then without it once it went out
    if (desk.saves.size() == 6) {
      CHECK(desk.saves[0].reportDue && desk.saves[0].reportId == kRequestId && desk.saves[0].answeredId == kRequestId);
      CHECK(settledOn(desk.saves[1], kRequestId));
    }
    CHECK_EQ(desk.reports.size(), 3);
    CHECK(sameJournal(supervisor.journal(), 0, ss::JOURNAL_IDLE));
    CHECK(settledOn(desk.flash, kRequestId + 2));
    // A request that was refused does not stand in the way of the next one.
    desk.ask(supervisor, kRequestId + 3);
    CHECK(desk.pass(supervisor));
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_PROGRAMMED);
  }

  // The client's clock is not set: the request waits for it, an hour at most.
  {
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.clockSet = false;
    desk.ask(supervisor, kRequestId);
    CHECK_EQ(desk.loopFor(supervisor, 1800000), 0);
    CHECK(supervisor.requestWaiting());
    CHECK_EQ(desk.saves.size(), 1);  // written once, not at every pass
    CHECK(holdsWaiting(desk.flash, kRequestId));
    CHECK(desk.reports.empty());
    desk.clockSet = true;  // half an hour later it knows the time
    CHECK(desk.pass(supervisor));
    CHECK_EQ(desk.reports.size(), 1);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_PROGRAMMED);
  }
  {
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.clockSet = false;
    desk.advance(777);
    desk.ask(supervisor, kRequestId);
    CHECK_EQ(desk.loopFor(supervisor, ss::REQUEST_WAIT_MS - 1), 0);
    CHECK(supervisor.requestWaiting());
    CHECK(desk.reports.empty());
    desk.advance(1);
    CHECK(!desk.pass(supervisor));
    CHECK(!supervisor.requestWaiting());
    CHECK_EQ(desk.reports.size(), 1);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_REFUSED);
    CHECK_EQ(desk.reports.back().result.refusals, ss::REFUSE_STALE);
    CHECK(sameProfile(desk.reports.back().result.profile, profile));
    CHECK_EQ(desk.reports.back().epoch, 0u);  // the client never knew the time
    CHECK_EQ(desk.saves.size(), 3);
    CHECK(settledOn(desk.flash, kRequestId));
    CHECK(desk.bus.log.empty());
    // The clock comes right after all: that request is over, and the config again changes nothing.
    desk.clockSet = true;
    desk.ask(supervisor, kRequestId);
    CHECK_EQ(desk.loopFor(supervisor, 60000), 0);
    CHECK_EQ(desk.reports.size(), 1);
  }

  {
    // A clock that runs but was never set: the request waits for the right time as it does for
    // a clock that does not run, and is not refused as too old.
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.epochBase = 86400.0;  // 1970-01-02
    desk.ask(supervisor, kRequestId);
    CHECK_EQ(desk.loopFor(supervisor, 600000), 0);
    CHECK(supervisor.requestWaiting());
    CHECK(desk.reports.empty());
    CHECK_EQ(desk.saves.size(), 1);
    desk.epochBase = kClientNow - (double)desk.clockMs / 1000.0;
    CHECK(desk.pass(supervisor));
    CHECK_EQ(desk.reports.size(), 1);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_PROGRAMMED);
  }

  // A request that is too old, or from a time the client has not reached.
  {
    const struct {
      double age;  // seconds between the request and the client's clock
      bool runs;
    } cases[] = {{0.0, true}, {86400.0, true}, {86401.0, false}, {-3600.0, true}, {-3601.0, false}, {40.0 * 86400.0, false}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      Desk desk;
      ss::Supervisor supervisor;
      desk.boot(supervisor);
      desk.epochBase = (double)kRequestId + cases[i].age - (double)desk.clockMs / 1000.0;
      desk.ask(supervisor, kRequestId);
      CHECK_EQ(desk.pass(supervisor), cases[i].runs);
      CHECK_EQ(desk.reports.size(), 1);
      if (desk.reports.size() == 1) {
        CHECK_MSG((desk.reports[0].result.outcome == ss::OUTCOME_PROGRAMMED) == cases[i].runs, "a request %.0f s old", cases[i].age);
        CHECK_EQ(desk.reports[0].result.refusals, cases[i].runs ? 0u : (uint32_t)ss::REFUSE_STALE);
      }
      // Taken on, at its two commit points, answered and sent; or taken on, refused and sent.
      CHECK_EQ(desk.saves.size(), cases[i].runs ? 5u : 3u);
      CHECK(settledOn(desk.flash, kRequestId));
      CHECK_EQ(desk.bus.log.empty(), !cases[i].runs);
      if (!cases[i].runs) {
        // It is not asked about twice, and it does not stand in the way of a newer request that is
        // in time. (The server's ids only grow: one that is older than an answered one is ignored.)
        CHECK(!desk.ask(supervisor, kRequestId));
        CHECK(!supervisor.requestWaiting());
        const uint32_t inTime = (uint32_t)desk.clock();
        if (inTime > kRequestId) {
          CHECK(desk.ask(supervisor, inTime));
          CHECK(supervisor.requestWaiting());
          CHECK(desk.pass(supervisor));
          CHECK_EQ(desk.reports.back().requestId, inTime);
          CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_PROGRAMMED);
        } else {
          CHECK(!desk.ask(supervisor, inTime));
          CHECK(!supervisor.requestWaiting());
        }
      }
    }
  }

  // The charger cannot be talked to: the request is taken on and waits, an hour at most.
  {
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.ready = false;
    desk.ask(supervisor, kRequestId);
    CHECK(!desk.pass(supervisor));
    CHECK(supervisor.requestWaiting());
    CHECK_EQ(desk.saves.size(), 1);
    CHECK(holdsWaiting(desk.flash, kRequestId));
    CHECK_EQ(desk.loopFor(supervisor, 1800000), 0);
    CHECK_EQ(desk.saves.size(), 1);  // written once, not at every pass
    CHECK(desk.reports.empty());
    desk.ready = true;
    CHECK(desk.pass(supervisor));
    CHECK_EQ(desk.reports.size(), 1);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK(settledOn(desk.flash, kRequestId) && sameJournal(desk.flash, kRequestId, ss::JOURNAL_IDLE));
  }
  {
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.ready = false;
    desk.advance(123456);
    desk.ask(supervisor, kRequestId);
    CHECK_EQ(desk.loopFor(supervisor, ss::REQUEST_WAIT_MS - 1), 0);
    CHECK(supervisor.requestWaiting());
    CHECK(desk.reports.empty());
    desk.advance(1);
    desk.ready = true;  // too late, even though it would answer now
    desk.sim.powered = true;
    desk.ready = false;
    CHECK(!desk.pass(supervisor));
    CHECK(!supervisor.requestWaiting());
    CHECK_EQ(desk.reports.size(), 1);
    CHECK_EQ(desk.reports.back().requestId, kRequestId);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_REFUSED);
    CHECK_EQ(desk.reports.back().result.refusals, ss::REFUSE_NO_CHARGER);
    CHECK(desk.bus.log.empty());
    // The journal no longer says that the request waits.
    CHECK(settledOn(supervisor.journal(), kRequestId));
    desk.ready = true;
    CHECK(!desk.pass(supervisor));
    CHECK_EQ(desk.saves.size(), 3);
    CHECK(settledOn(desk.flash, kRequestId));
    CHECK(!desk.ask(supervisor, kRequestId));  // the config again: it has had its answer
    CHECK(!supervisor.requestWaiting());
    CHECK_EQ(desk.loopFor(supervisor, 60000), 0);
    CHECK_EQ(desk.reports.size(), 1);
  }

  // A newer request takes the place of one that waits. The older one gets no answer.
  {
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.ready = false;
    desk.ask(supervisor, kRequestId);
    CHECK(!desk.pass(supervisor));
    desk.advance(1800000);
    ss::Profile wanted = profile;
    wanted.lvdCv = 1200;
    CHECK(desk.ask(supervisor, kRequestId + 1800, wanted));
    CHECK(!desk.ask(supervisor, kRequestId));         // the older config, delivered late: ignored
    CHECK(!desk.ask(supervisor, kRequestId + 1800));  // and the new one again
    CHECK(supervisor.requestWaiting());
    // The newer one has its own hour.
    CHECK_EQ(desk.loopFor(supervisor, ss::REQUEST_WAIT_MS - 1), 0);
    CHECK(supervisor.requestWaiting());
    CHECK(desk.reports.empty());
    desk.ready = true;
    CHECK(desk.pass(supervisor));
    CHECK_EQ(desk.reports.size(), 1);
    CHECK_EQ(desk.reports.back().requestId, kRequestId + 1800);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK(holdsProfile(desk.sim, wanted));
    CHECK(sameJournal(desk.flash, kRequestId + 1800, ss::JOURNAL_IDLE));
  }
  {
    // A newer request that is refused as it arrives still ends the one that waited.
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.ready = false;
    desk.ask(supervisor, kRequestId);
    CHECK(!desk.pass(supervisor));
    CHECK(holdsWaiting(desk.flash, kRequestId));
    desk.ask(supervisor, kRequestId + 5, profile, ss::PROFILE_HVD);
    CHECK(!supervisor.requestWaiting());
    CHECK_EQ(desk.reports.size(), 1);
    CHECK_EQ(desk.reports.back().requestId, kRequestId + 5);
    desk.ready = true;
    CHECK_EQ(desk.loopFor(supervisor, 7200000, 60000), 0);
    CHECK(desk.bus.log.empty());
    CHECK_EQ(desk.reports.size(), 1);
    CHECK(settledOn(desk.flash, kRequestId + 5));
  }

  // Flash does not take the journal.
  {
    // As the request is taken on: refused at once, before the config is acknowledged, and the
    // controller is not touched.
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.failSaves = 1;
    CHECK(desk.ask(supervisor, kRequestId));
    CHECK(!supervisor.requestWaiting());
    CHECK(!supervisor.wantsLine(desk.clock(), desk.millisNow()));
    CHECK_EQ(desk.reports.size(), 1);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_REFUSED);
    CHECK_EQ(desk.reports.back().result.refusals, ss::REFUSE_JOURNAL);
    CHECK(!desk.pass(supervisor));
    CHECK(desk.bus.log.empty());
    CHECK(settledOn(desk.flash, kRequestId) && sameJournal(desk.flash, 0, ss::JOURNAL_IDLE));
    CHECK(sameJournal(supervisor.journal(), 0, ss::JOURNAL_IDLE));
    CHECK(!desk.ask(supervisor, kRequestId));
    CHECK(!supervisor.requestWaiting());
    CHECK_EQ(desk.loopFor(supervisor, 60000), 0);
    CHECK_EQ(desk.saves.size(), 3);  // the request, which flash refused; its refusal; and that it went out
  }
  {
    // Before the first restart command: refused, and no command goes out.
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.failSave = 1;
    desk.ask(supervisor, kRequestId);
    CHECK(desk.pass(supervisor));
    CHECK_EQ(desk.reports.size(), 1);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_REFUSED);
    CHECK_EQ(desk.reports.back().result.refusals, ss::REFUSE_JOURNAL);
    CHECK_EQ(desk.bus.log.size(), ss::ITEM_COUNT);
    CHECK_EQ(desk.bus.count('c') + desk.bus.count('w'), 0);
    CHECK_EQ(desk.sim.resets, 0);
    CHECK_EQ(desk.saves.size(), 4);
    CHECK(settledOn(desk.flash, kRequestId) && sameJournal(desk.flash, 0, ss::JOURNAL_IDLE));
    CHECK(!supervisor.dutyOwed());
  }
  {
    // Before the first write: refused after the controller's first restart, and nothing written.
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.failSave = 2;
    desk.ask(supervisor, kRequestId);
    CHECK(desk.pass(supervisor));
    CHECK_EQ(desk.reports.size(), 1);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_REFUSED);
    CHECK_EQ(desk.reports.back().result.refusals, ss::REFUSE_JOURNAL);
    CHECK_EQ(desk.reports.back().result.restart, ss::RESTART_DONE);
    CHECK(!desk.reports.back().result.wroteAny);
    CHECK_EQ(desk.bus.log.size(), firstWrite);
    CHECK_EQ(desk.bus.count('w'), 0);
    CHECK_EQ(desk.sim.resets, 1);
    CHECK_EQ(desk.saves.size(), 5);
    CHECK(settledOn(desk.flash, kRequestId) && sameJournal(desk.flash, kRequestId, ss::JOURNAL_IDLE));
    CHECK(!supervisor.dutyOwed());
  }
  {
    // At the end of a run: the journal in flash still says "writing", both when the result is
    // due and once it has gone out. It is written again ten minutes later, and until then the
    // client itself knows better.
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.failFrom = 3;
    desk.ask(supervisor, kRequestId);
    CHECK(desk.pass(supervisor));
    desk.failFrom = kNever;
    CHECK_EQ(desk.reports.size(), 1);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(desk.saves.size(), 5);
    CHECK(sameJournal(desk.flash, kRequestId, ss::JOURNAL_WRITING));
    CHECK(settledOn(supervisor.journal(), kRequestId) && sameJournal(supervisor.journal(), kRequestId, ss::JOURNAL_IDLE));
    CHECK(!supervisor.dutyOwed());
    CHECK_EQ(desk.loopFor(supervisor, ss::JOURNAL_RETRY_MS - 1), 0);
    CHECK_EQ(desk.saves.size(), 5);
    desk.advance(1);
    CHECK(!desk.pass(supervisor));
    CHECK_EQ(desk.saves.size(), 6);
    CHECK(settledOn(desk.flash, kRequestId) && sameJournal(desk.flash, kRequestId, ss::JOURNAL_IDLE));
    CHECK_EQ(desk.loopFor(supervisor, 7200000, 60000), 0);
    CHECK_EQ(desk.saves.size(), 6);
    CHECK_EQ(desk.bus.log.size(), quiet.size());
  }
  {
    // A write that flash refuses is made good by the next one that it takes: here the one that
    // says the result has gone out.
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.failSave = 3;
    desk.ask(supervisor, kRequestId);
    CHECK(desk.pass(supervisor));
    CHECK_EQ(desk.saves.size(), 5);
    CHECK(settledOn(desk.flash, kRequestId));
    CHECK_EQ(desk.loopFor(supervisor, 2 * ss::JOURNAL_RETRY_MS, 60000), 0);
    CHECK_EQ(desk.saves.size(), 5);
  }
  {
    // And when flash keeps refusing: a try every ten minutes, no more.
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.ask(supervisor, kRequestId);
    desk.failFrom = 3;
    CHECK(desk.pass(supervisor));
    desk.loopFor(supervisor, 6 * ss::JOURNAL_RETRY_MS + 1000);
    desk.failFrom = kNever;
    CHECK_EQ(desk.saves.size(), 5 + 6);
    // A client that restarts in that state looks at the controller, finds nothing to do, and says so.
    desk.failSaves = 0;
    ss::Supervisor restarted;
    desk.boot(restarted);
    CHECK(restarted.dutyOwed());
    CHECK(desk.pass(restarted));
    CHECK_EQ(desk.reports.size(), 2);
    CHECK_EQ(desk.reports.back().requestId, kRequestId);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_INTERRUPTED);
    CHECK_EQ(desk.reports.back().result.restart, ss::RESTART_NOT_NEEDED);
    CHECK_EQ(desk.bus.count('w'), 15);
    CHECK_EQ(desk.sim.resets, 2);
    CHECK(sameJournal(desk.flash, kRequestId, ss::JOURNAL_IDLE));
  }

  // What a client finds in its flash when it starts.
  {
    // Nothing under way. The id of the last request is remembered across the restart.
    Desk desk;
    desk.flash = journalOf(kRequestId, ss::JOURNAL_IDLE);
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    CHECK(desk.reports.empty());
    CHECK(!supervisor.dutyOwed());
    desk.ask(supervisor, kRequestId);
    CHECK(!supervisor.requestWaiting());
    CHECK_EQ(desk.loopFor(supervisor, 60000), 0);
    CHECK(desk.saves.empty());
    desk.ask(supervisor, kRequestId + 1);
    CHECK(supervisor.requestWaiting());
  }
  for (int owed = 0; owed < 2; ++owed) {
    // A request it had taken on and not begun, waiting for the clock, the charger or a duty: it
    // says so, and that is the end of that request. A duty that was owed stays owed.
    Desk desk;
    desk.flash = journalOf(kRequestId - 50, owed == 1 ? ss::JOURNAL_WRITING : ss::JOURNAL_IDLE);
    desk.flash.waitingId = kRequestId;
    desk.flash.answeredId = kRequestId;
    desk.advance(4000);
    ss::Supervisor supervisor;
    desk.ready = false;
    desk.boot(supervisor);
    CHECK_EQ(desk.reports.size(), 1);
    if (desk.reports.size() == 1) {
      CHECK_EQ(desk.reports[0].requestId, kRequestId);
      CHECK_EQ(desk.reports[0].result.outcome, ss::OUTCOME_INTERRUPTED);
      CHECK_EQ(desk.reports[0].result.notes, ss::NOTE_NOTHING_WRITTEN);
      CHECK(!desk.reports[0].result.followUp);
      CHECK_EQ(desk.reports[0].atMs, 0);
      CHECK_EQ(desk.reports[0].epoch, 0u);  // the client's clock is not set yet when it starts
    }
    CHECK_EQ(supervisor.dutyOwed(), owed == 1);
    CHECK(!supervisor.requestWaiting());
    CHECK_EQ(desk.saves.size(), 2);  // the answer, and that it went out
    CHECK(desk.flash.waitingId == 0 && desk.flash.answeredId == kRequestId && !desk.flash.reportDue);
    CHECK(sameJournal(desk.flash, kRequestId - 50, owed == 1 ? ss::JOURNAL_WRITING : ss::JOURNAL_IDLE));
    desk.ready = true;
    CHECK_EQ(desk.loopFor(supervisor, 7200000, 60000), owed == 1 ? 1u : 0u);
    CHECK_EQ(desk.bus.log.empty(), owed == 0);
    CHECK_EQ(desk.reports.size(), owed == 1 ? 2u : 1u);
    CHECK_EQ(desk.reports.back().requestId, owed == 1 ? kRequestId - 50 : kRequestId);
    CHECK(!supervisor.dutyOwed());
    CHECK(settledOn(desk.flash, kRequestId));
    CHECK(!desk.ask(supervisor, kRequestId));
    CHECK(!supervisor.requestWaiting());
  }
  for (int which = 0; which < 4; ++which) {
    // A run that had come to its first restart command, or to its writes: the client asks the
    // controller whether it waits for a restart, as soon as it can be talked to.
    Desk desk;
    ss::Journal journal = which == 3 ? ss::cautiousJournal() : ss::emptyJournal();
    if (which < 3) {
      journal.requestId = which < 2 ? kRequestId : kRequestId + 9;
      journal.phase = which == 0 ? (uint8_t)ss::JOURNAL_RESTARTING : (which == 1 ? (uint8_t)ss::JOURNAL_WRITING : (uint8_t)9);
    }
    desk.flash = journal;
    if (which == 1) {
      desk.sim.regs[A_LOAD_FAULT] = EDIT_LOAD;
    }
    ss::Supervisor supervisor;
    desk.ready = false;
    desk.boot(supervisor);
    CHECK(supervisor.dutyOwed());
    CHECK(desk.reports.empty());
    CHECK_EQ(desk.loopFor(supervisor, 1800000), 0);
    CHECK(desk.bus.log.empty());
    desk.ready = true;
    CHECK(desk.pass(supervisor));
    CHECK_EQ(desk.reports.size(), 1);
    if (desk.reports.size() == 1) {
      const ss::ProgramResult &result = desk.reports[0].result;
      CHECK_EQ(desk.reports[0].requestId, journal.requestId);
      CHECK_EQ(result.outcome, ss::OUTCOME_INTERRUPTED);
      CHECK(!result.followUp);
      CHECK_EQ(result.notes, which == 0 ? (ss::NOTE_NOTHING_WRITTEN | ss::NOTE_AT_FIRST_RESTART)
                                        : (which == 1 ? (uint16_t)ss::NOTE_WAS_PENDING : (uint16_t)0));
      CHECK_EQ(result.restart, which == 1 ? ss::RESTART_DONE : ss::RESTART_NOT_NEEDED);
      CHECK_EQ(result.planned, 0);
    }
    CHECK_EQ(desk.bus.count('w'), 0);
    CHECK_EQ(desk.bus.count('c'), which == 1 ? 1 : 0);
    CHECK(!editPending(desk.sim));
    CHECK(!supervisor.dutyOwed());
    CHECK_EQ(desk.saves.size(), 2);  // the result, and that it went out
    CHECK(sameJournal(desk.flash, journal.requestId, ss::JOURNAL_IDLE));
    CHECK(settledOn(desk.flash, journal.requestId));
    CHECK_EQ(desk.loopFor(supervisor, 2 * 86400000ull, 600000), 0);
    CHECK_EQ(desk.reports.size(), 1);
  }

  // A run whose last restart command found no answer. The client says what it knows, keeps the
  // journal open, and takes the run up again: after 10 and 20 minutes, then once a day.
  {
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.bus.before = [&desk](size_t, char kind, uint16_t) {
      if (kind == 'c' && desk.bus.count('w') > 0) {
        desk.sim.powered = false;
      }
    };
    desk.ask(supervisor, kRequestId);
    CHECK(desk.pass(supervisor));
    const uint64_t firstTry = desk.clockMs;
    CHECK_EQ(desk.reports.size(), 1);
    if (desk.reports.size() == 1) {
      CHECK_EQ(desk.reports[0].result.outcome, ss::OUTCOME_RESTART_FAILED);
      CHECK_EQ(desk.reports[0].result.restart, ss::RESTART_NO_ANSWER);
      CHECK(desk.reports[0].result.followUp);
      CHECK(desk.reports[0].result.wroteAny);
    }
    CHECK(supervisor.dutyOwed());
    CHECK(sameJournal(desk.flash, kRequestId, ss::JOURNAL_WRITING));
    CHECK_EQ(desk.saves.size(), 5);  // the request, the two commit points, the result and that it went out
    CHECK(editPending(desk.sim));
    desk.bus.before = nullptr;

    // Not before its time, and not while the charger cannot be talked to.
    size_t asked = desk.bus.log.size();
    CHECK_EQ(desk.loopFor(supervisor, firstTry + ss::DUTY_RETRY_MS - 1 - desk.clockMs), 0);
    CHECK_EQ(desk.bus.log.size(), asked);
    desk.advance(1);
    desk.ready = false;
    CHECK(!desk.pass(supervisor));
    desk.ready = true;
    CHECK(desk.pass(supervisor));  // the second try: still no answer
    const uint64_t secondTry = desk.clockMs;
    CHECK_EQ(desk.bus.log.size(), asked + ss::READ_ATTEMPTS);
    CHECK_EQ(desk.reports.size(), 2);
    CHECK(desk.reports.back().result.followUp);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_RESTART_FAILED);
    CHECK_EQ(desk.reports.back().requestId, kRequestId);
    CHECK(supervisor.dutyOwed());
    CHECK_EQ(desk.saves.size(), 7);  // the result of the try, and that it went out: the phase stays
    CHECK(sameJournal(desk.flash, kRequestId, ss::JOURNAL_WRITING));

    CHECK_EQ(desk.loopFor(supervisor, secondTry + ss::DUTY_RETRY_MS - 1 - desk.clockMs), 0);
    desk.advance(1);
    CHECK(desk.pass(supervisor));  // the third
    const uint64_t thirdTry = desk.clockMs;
    CHECK_EQ(desk.reports.size(), 3);
    // From here on once a day.
    asked = desk.bus.log.size();
    CHECK_EQ(desk.loopFor(supervisor, thirdTry + ss::DUTY_SLOW_RETRY_MS - 1 - desk.clockMs, 60000), 0);
    CHECK_EQ(desk.bus.log.size(), asked);
    desk.advance(1);
    CHECK(desk.pass(supervisor));
    const uint64_t fourthTry = desk.clockMs;
    CHECK_EQ(desk.reports.size(), 4);
    CHECK_EQ(desk.loopFor(supervisor, fourthTry + ss::DUTY_SLOW_RETRY_MS - 1 - desk.clockMs, 60000), 0);

    // A request arrives: the duty comes first, and is tried at once. The charger still does not
    // answer, so the request waits, and after its hour it is refused. The duty stays.
    desk.ask(supervisor, kRequestId + 200000);
    CHECK(supervisor.requestWaiting());
    asked = desk.bus.log.size();
    CHECK(desk.pass(supervisor));
    CHECK_EQ(desk.bus.log.size(), asked + ss::READ_ATTEMPTS);
    CHECK_EQ(desk.reports.size(), 5);
    CHECK_EQ(desk.reports.back().requestId, kRequestId);  // the duty's report, not the new request's
    CHECK(supervisor.requestWaiting());
    CHECK(!desk.ask(supervisor, kRequestId + 200000));  // the config again does not force another try
    asked = desk.bus.log.size();
    CHECK_EQ(desk.loopFor(supervisor, ss::REQUEST_WAIT_MS + 60000, 30000), 0);
    CHECK_EQ(desk.bus.log.size(), asked);
    CHECK(!supervisor.requestWaiting());
    CHECK_EQ(desk.reports.size(), 6);
    CHECK_EQ(desk.reports.back().requestId, kRequestId + 200000);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_REFUSED);
    CHECK_EQ(desk.reports.back().result.refusals, ss::REFUSE_NO_CHARGER);
    CHECK(supervisor.dutyOwed());
    CHECK(sameJournal(desk.flash, kRequestId, ss::JOURNAL_WRITING));
    CHECK(sameJournal(supervisor.journal(), kRequestId, ss::JOURNAL_WRITING));

    // The controller is back. At its next turn the run is taken up again and ends as it would have.
    desk.sim.powered = true;
    unsigned ran = desk.loopFor(supervisor, ss::DUTY_SLOW_RETRY_MS, 60000);
    CHECK_EQ(ran, 1);
    CHECK_EQ(desk.reports.size(), 7);
    if (desk.reports.size() == 7) {
      const ss::ProgramResult &result = desk.reports.back().result;
      CHECK_EQ(desk.reports.back().requestId, kRequestId);
      CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
      CHECK_EQ(result.restart, ss::RESTART_DONE);
      CHECK(!result.followUp);
      CHECK_EQ(ss::countEntries(result, ss::ENTRY_WRITTEN), 15);
      CHECK(sameProfile(result.profile, profile));
    }
    CHECK(!editPending(desk.sim));
    CHECK(holdsProfile(desk.sim, profile));
    CHECK(!supervisor.dutyOwed());
    CHECK(settledOn(desk.flash, kRequestId + 200000) && sameJournal(desk.flash, kRequestId, ss::JOURNAL_IDLE));
    CHECK_EQ(desk.bus.count('w'), 15);  // nothing was written a second time
    CHECK_EQ(desk.bus.disorder, 0);
    checkPausesIn(desk.bus.log, desk.master.responseTimeoutMs);
    CHECK_EQ(desk.loopFor(supervisor, 3 * 86400000ull, 600000), 0);
    CHECK_EQ(desk.reports.size(), 7);

    // A later run whose restart finds no answer starts again with tries 10 minutes apart.
    desk.bus.before = [&desk](size_t, char kind, uint16_t) {
      if (kind == 'c' && desk.bus.count('w') > 15) {
        desk.sim.powered = false;
      }
    };
    ss::Profile wanted = profile;
    wanted.lvdCv = 1200;
    desk.ask(supervisor, (uint32_t)desk.clock(), wanted);
    CHECK(desk.pass(supervisor));
    CHECK_EQ(desk.reports.size(), 8);
    CHECK(desk.reports.back().result.followUp);
    const uint64_t again = desk.clockMs;
    desk.bus.before = nullptr;
    desk.sim.powered = true;
    CHECK_EQ(desk.loopFor(supervisor, again + ss::DUTY_RETRY_MS - 1 - desk.clockMs), 0);
    desk.advance(1);
    CHECK(desk.pass(supervisor));
    CHECK_EQ(desk.reports.size(), 9);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK(holdsProfile(desk.sim, wanted));
    CHECK(!editPending(desk.sim));
  }
  {
    // begin() forgets the run a supervisor held: a duty after it is settled by a recovery, as
    // after a restart of the client.
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.bus.before = [&desk](size_t, char kind, uint16_t) {
      if (kind == 'c' && desk.bus.count('w') > 0) {
        desk.sim.powered = false;
      }
    };
    desk.ask(supervisor, kRequestId);
    CHECK(desk.pass(supervisor));
    CHECK(supervisor.dutyOwed());
    desk.bus.before = nullptr;
    desk.sim.powered = true;
    desk.advance(20000);
    desk.boot(supervisor);
    CHECK(desk.pass(supervisor));
    CHECK_EQ(desk.reports.size(), 2);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_INTERRUPTED);
    CHECK_EQ(desk.reports.back().result.restart, ss::RESTART_DONE);
    CHECK(!editPending(desk.sim));
  }
  {
    // A request that arrives while a duty is owed, on a controller that answers again: the duty
    // is done at once, and the request runs in the pass after it.
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.bus.before = [&desk](size_t, char kind, uint16_t) {
      if (kind == 'c' && desk.bus.count('w') > 0) {
        desk.sim.powered = false;
      }
    };
    desk.ask(supervisor, kRequestId);
    CHECK(desk.pass(supervisor));
    desk.bus.before = nullptr;
    desk.sim.powered = true;
    CHECK(supervisor.dutyOwed());
    desk.advance(60000);
    CHECK(!desk.pass(supervisor));  // not its turn yet
    ss::Profile wanted = profile;
    wanted.lvdCv = 1200;
    desk.ask(supervisor, kRequestId + 60, wanted);
    CHECK(desk.pass(supervisor));
    CHECK_EQ(desk.reports.size(), 2);
    CHECK_EQ(desk.reports.back().requestId, kRequestId);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK(supervisor.requestWaiting());
    CHECK(!supervisor.dutyOwed());
    CHECK(desk.pass(supervisor));
    CHECK_EQ(desk.reports.size(), 3);
    CHECK_EQ(desk.reports.back().requestId, kRequestId + 60);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(ss::countEntries(desk.reports.back().result, ss::ENTRY_WRITTEN), 1);
    CHECK(holdsProfile(desk.sim, wanted));
    CHECK(settledOn(desk.flash, kRequestId + 60) && sameJournal(desk.flash, kRequestId + 60, ss::JOURNAL_IDLE));
    CHECK(!desk.pass(supervisor));
  }
  {
    // The client restarts while such a duty is owed: the run is gone with its memory, and a
    // recovery does what is left to do.
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.bus.before = [&desk](size_t, char kind, uint16_t) {
      if (kind == 'c' && desk.bus.count('w') > 0) {
        desk.sim.powered = false;
      }
    };
    desk.ask(supervisor, kRequestId);
    CHECK(desk.pass(supervisor));
    desk.bus.before = nullptr;
    desk.sim.powered = true;
    CHECK(editPending(desk.sim));
    ss::Supervisor restarted;
    desk.advance(30000);
    desk.boot(restarted);
    CHECK(restarted.dutyOwed());
    CHECK(desk.pass(restarted));
    CHECK_EQ(desk.reports.size(), 2);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_INTERRUPTED);
    CHECK_EQ(desk.reports.back().result.restart, ss::RESTART_DONE);
    CHECK_EQ(desk.reports.back().result.notes, ss::NOTE_WAS_PENDING);
    CHECK(!editPending(desk.sim));
    CHECK(holdsProfile(desk.sim, profile));
    CHECK(sameJournal(desk.flash, kRequestId, ss::JOURNAL_IDLE));
  }

  // A recovery that cannot reach the controller is owed again, on the same timetable.
  {
    Desk desk;
    desk.flash = journalOf(kRequestId, ss::JOURNAL_WRITING);
    desk.sim.regs[A_ARRAY_FAULT] = EDIT_ARRAY;
    desk.sim.powered = false;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    CHECK(desk.pass(supervisor));
    CHECK_EQ(desk.reports.size(), 1);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_NO_LINK);
    CHECK(desk.reports.back().result.followUp);
    CHECK(supervisor.dutyOwed());
    CHECK_EQ(desk.saves.size(), 2);
    CHECK(sameJournal(desk.flash, kRequestId, ss::JOURNAL_WRITING));
    const uint64_t firstTry = desk.clockMs;
    CHECK_EQ(desk.loopFor(supervisor, firstTry + ss::DUTY_RETRY_MS - 1 - desk.clockMs), 0);
    desk.sim.powered = true;
    desk.advance(1);
    CHECK(desk.pass(supervisor));
    CHECK_EQ(desk.reports.size(), 2);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_INTERRUPTED);
    CHECK_EQ(desk.reports.back().result.restart, ss::RESTART_DONE);
    CHECK(!desk.reports.back().result.followUp);
    CHECK(!editPending(desk.sim));
    CHECK(!supervisor.dutyOwed());
    CHECK(sameJournal(desk.flash, kRequestId, ss::JOURNAL_IDLE));
  }

  // The timers work across the wrap of millis().
  {
    Desk desk;
    ss::Supervisor supervisor;
    desk.millisAtBoot = 0xFFFFFFFFu - 1800000u;  // half an hour before the wrap
    desk.boot(supervisor);
    desk.ready = false;
    desk.ask(supervisor, kRequestId);
    CHECK_EQ(desk.loopFor(supervisor, ss::REQUEST_WAIT_MS - 1), 0);
    CHECK(supervisor.requestWaiting());
    CHECK(desk.millisNow() < 1800000u);  // it has wrapped
    desk.advance(1);
    CHECK(!desk.pass(supervisor));
    CHECK(!supervisor.requestWaiting());
    CHECK_EQ(desk.reports.back().result.refusals, ss::REFUSE_NO_CHARGER);
  }
  {
    Desk desk;
    ss::Supervisor supervisor;
    desk.millisAtBoot = 0xFFFFFFFFu - 300000u;  // the run ends five minutes before the wrap, or so
    desk.boot(supervisor);
    desk.bus.before = [&desk](size_t, char kind, uint16_t) {
      if (kind == 'c' && desk.bus.count('w') > 0) {
        desk.sim.powered = false;
      }
    };
    desk.ask(supervisor, kRequestId);
    CHECK(desk.pass(supervisor));
    desk.bus.before = nullptr;
    const uint64_t firstTry = desk.clockMs;
    CHECK(supervisor.dutyOwed());
    CHECK_EQ(desk.loopFor(supervisor, firstTry + ss::DUTY_RETRY_MS - 1 - desk.clockMs), 0);
    CHECK(desk.millisNow() < 600000u);
    desk.sim.powered = true;
    desk.advance(1);
    CHECK(desk.pass(supervisor));
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_PROGRAMMED);
  }

  // The step limit, which no run reaches: a run is given up where it stands, and what it may
  // have left behind is settled by a recovery.
  {
    // During the first reading: nothing was sent, nothing is owed.
    Desk desk;
    ss::Supervisor supervisor(30);
    desk.boot(supervisor);
    desk.ask(supervisor, kRequestId);
    CHECK(desk.pass(supervisor));
    CHECK_EQ(desk.bus.log.size(), 30);
    CHECK_EQ(desk.reports.size(), 1);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_STOPPED);
    CHECK(!desk.reports.back().result.followUp);
    CHECK(!supervisor.dutyOwed());
    CHECK(settledOn(desk.flash, kRequestId) && sameJournal(desk.flash, 0, ss::JOURNAL_IDLE));
    CHECK_EQ(desk.loopFor(supervisor, 2 * 86400000ull, 600000), 0);
  }
  {
    // In the middle of the writes: the controller waits for a restart, and gets it at the duty's turn.
    Desk desk;
    ss::Supervisor supervisor(70);
    desk.boot(supervisor);
    desk.ask(supervisor, kRequestId);
    CHECK(desk.pass(supervisor));
    CHECK(desk.bus.count('w') > 3 && desk.bus.count('w') < 15);
    CHECK_EQ(desk.reports.size(), 1);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_STOPPED);
    CHECK(desk.reports.back().result.followUp);
    CHECK(supervisor.dutyOwed());
    CHECK(sameJournal(desk.flash, kRequestId, ss::JOURNAL_WRITING));
    CHECK(editPending(desk.sim));
    const uint64_t stopped = desk.clockMs;
    const size_t wrote = desk.bus.count('w');
    CHECK_EQ(desk.loopFor(supervisor, stopped + ss::DUTY_RETRY_MS - 1 - desk.clockMs), 0);
    desk.advance(1);
    CHECK(desk.pass(supervisor));
    CHECK_EQ(desk.reports.size(), 2);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_INTERRUPTED);
    CHECK_EQ(desk.reports.back().result.restart, ss::RESTART_DONE);
    CHECK(!editPending(desk.sim));
    CHECK_EQ(desk.bus.count('w'), wrote);
    CHECK_EQ(desk.bus.disorder, 0);
    CHECK(!supervisor.dutyOwed());
    CHECK(sameJournal(desk.flash, kRequestId, ss::JOURNAL_IDLE));
  }
  CHECK(ss::RUN_STEP_LIMIT >= 5 * quiet.size());
}

// What must hold once a client that lost its power in the middle of a request is back and has
// had a few passes of its loop.
static void checkAfterRestart(Desk &desk, const SimSunSaver &fresh, const ss::Profile &profile, const char *what, size_t at,
                              bool controllerRestarted) {
  const ss::Journal held = desk.flash;  // what flash held when the power went
  const size_t wrote = desk.bus.count('w');
  const bool waited = editPending(desk.sim);
  const size_t reported = desk.reports.size();
  desk.advance(20000);
  ss::Supervisor supervisor;
  desk.boot(supervisor);
  desk.loopFor(supervisor, 5000);

  CHECK_MSG(!editPending(desk.sim), "%s %u: the controller still waits for a restart", what, (unsigned)at);
  CHECK_MSG(!supervisor.dutyOwed() && desk.flash.phase == ss::JOURNAL_IDLE, "%s %u: the journal is left at phase %u", what,
            (unsigned)at, (unsigned)desk.flash.phase);
  CHECK_MSG(desk.bus.count('w') == wrote, "%s %u: the restarted client wrote", what, (unsigned)at);
  CHECK_MSG(desk.bus.disorder == 0, "%s %u: the settings were out of order at some moment", what, (unsigned)at);
  CHECK_MSG(onlyPlanChanged(desk.sim, fresh) && beforeOrTarget(desk.sim, fresh, profile), "%s %u: a register holds what nobody asked for",
            what, (unsigned)at);
  // What the restarted client says depends on what its journal held, and on nothing else.
  const size_t said = desk.reports.size() - reported;
  if (held.reportDue) {
    // A result that had not gone out, or had and flash did not know it yet, goes out now.
    CHECK_MSG(said == 1, "%s %u: %u reports after a restart with a result due", what, (unsigned)at, (unsigned)said);
    if (said == 1) {
      CHECK_EQ(desk.reports.back().requestId, held.reportId);
      CHECK_EQ(desk.reports.back().epoch, held.reportEpoch);
      CHECK_MSG(sameResult(desk.reports.back().result, held.report), "%s %u: the result changed across the restart", what,
                (unsigned)at);
      CHECK_MSG(ss::outcomeIsGood(held.report.outcome), "%s %u: a run that ended reported %s", what, (unsigned)at,
                ss::outcomeName(held.report.outcome));
    }
  } else if (held.phase == ss::JOURNAL_IDLE && held.waitingId == 0) {
    CHECK_MSG(said == 0, "%s %u: %u reports after a restart with nothing under way", what, (unsigned)at, (unsigned)said);
  } else {
    CHECK_MSG(said == 1, "%s %u: %u reports after the restart", what, (unsigned)at, (unsigned)said);
    if (said == 1) {
      const ss::ProgramResult &result = desk.reports.back().result;
      CHECK_EQ(desk.reports.back().requestId, kRequestId);
      CHECK_MSG(result.outcome == ss::OUTCOME_INTERRUPTED && !result.followUp, "%s %u: reported as %s", what, (unsigned)at,
                ss::outcomeName(result.outcome));
      CHECK_MSG(((result.notes & ss::NOTE_NOTHING_WRITTEN) != 0) == (held.phase != ss::JOURNAL_WRITING), "%s %u: notes 0x%X",
                what, (unsigned)at, (unsigned)result.notes);
      CHECK_MSG((result.restart == ss::RESTART_DONE) == (waited && !controllerRestarted), "%s %u: restart status %u", what,
                (unsigned)at, (unsigned)result.restart);
      if (held.phase != ss::JOURNAL_WRITING) {
        CHECK_MSG(wrote == 0, "%s %u: the journal says nothing was written, and %u writes went out", what, (unsigned)at,
                  (unsigned)wrote);
      }
    }
  }

  // The server sends the config again: a request this client had taken on is not run twice.
  const size_t asked = desk.bus.log.size();
  desk.ask(supervisor, kRequestId);
  desk.loopFor(supervisor, 3000);
  if (held.answeredId == kRequestId) {
    CHECK_MSG(desk.bus.log.size() == asked && desk.reports.size() == reported + said, "%s %u: the request ran a second time", what,
              (unsigned)at);
  } else {
    CHECK_MSG(desk.reports.size() == reported + 1 && desk.reports.back().result.outcome == ss::OUTCOME_PROGRAMMED,
              "%s %u: the request was lost with the power and did not run when it came again", what, (unsigned)at);
  }
  // And a new request finishes the job, whatever state the first one left.
  desk.ask(supervisor, kRequestId + 600);
  desk.loopFor(supervisor, 3000);
  CHECK_MSG(ss::outcomeIsGood(desk.reports.back().result.outcome) && desk.reports.back().requestId == kRequestId + 600,
            "%s %u: the next request ended as %s", what, (unsigned)at, ss::outcomeName(desk.reports.back().result.outcome));
  CHECK_MSG(holdsProfile(desk.sim, profile) && !editPending(desk.sim), "%s %u: the controller does not hold the profile", what,
            (unsigned)at);
  CHECK_MSG(desk.bus.disorder == 0, "%s %u: out of order after the next request", what, (unsigned)at);
  CHECK(settledOn(desk.flash, kRequestId + 600));
}

// Supervisor::wantsLine() in the plain cases. Desk::pass() checks it against service() in every
// pass of every other test.
static void testWantsLine() {
  {
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    CHECK(!supervisor.wantsLine(desk.clock(), desk.millisNow()));  // nothing asked, nothing owed
    desk.ask(supervisor, kRequestId);
    CHECK(supervisor.wantsLine(desk.clock(), desk.millisNow()));
    // A request runs only with the clock set and while it is fresh: not before, and not when it
    // is too old or too far ahead.
    CHECK(!supervisor.wantsLine(0.0, desk.millisNow()));
    CHECK(!supervisor.wantsLine(ss::CLOCK_SET_EPOCH - 1.0, desk.millisNow()));
    CHECK(supervisor.wantsLine((double)kRequestId + ss::REQUEST_MAX_AGE_SECONDS, desk.millisNow()));
    CHECK(!supervisor.wantsLine((double)kRequestId + ss::REQUEST_MAX_AGE_SECONDS + 1.0, desk.millisNow()));
    CHECK(supervisor.wantsLine((double)kRequestId - ss::REQUEST_MAX_AHEAD_SECONDS, desk.millisNow()));
    CHECK(!supervisor.wantsLine((double)kRequestId - ss::REQUEST_MAX_AHEAD_SECONDS - 1.0, desk.millisNow()));
    CHECK(desk.pass(supervisor));
    CHECK(!supervisor.wantsLine(desk.clock(), desk.millisNow()));  // done
  }
  {
    // Waiting for the charger: both limits still hold, up to the moment the run would start.
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.ready = false;
    desk.epochBase = (double)kRequestId + ss::REQUEST_MAX_AGE_SECONDS - 30.0;  // the request is a day old, nearly
    desk.ask(supervisor, kRequestId);
    CHECK(!desk.pass(supervisor));
    CHECK(holdsWaiting(desk.flash, kRequestId));
    CHECK(supervisor.wantsLine(desk.clock(), desk.millisNow()));
    CHECK(!supervisor.wantsLine(0.0, desk.millisNow()));  // and not without the clock
    desk.advance(30000);
    CHECK(supervisor.wantsLine(desk.clock(), desk.millisNow()));
    desk.advance(1000);
    CHECK(!supervisor.wantsLine(desk.clock(), desk.millisNow()));
    desk.ready = true;  // ready, a second too late
    CHECK(!desk.pass(supervisor));
    CHECK(desk.bus.log.empty());
    CHECK_EQ(desk.reports.size(), 1);
    CHECK_EQ(desk.reports.back().result.refusals, ss::REFUSE_STALE);
    CHECK(!supervisor.wantsLine(desk.clock(), desk.millisNow()));
  }
  {
    // The hour of waiting ends a moment before the charger is ready.
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.ready = false;
    desk.ask(supervisor, kRequestId);
    CHECK_EQ(desk.loopFor(supervisor, ss::REQUEST_WAIT_MS - 1, 600000), 0);
    CHECK(supervisor.wantsLine(desk.clock(), desk.millisNow()));
    desk.advance(1);
    CHECK(!supervisor.wantsLine(desk.clock(), desk.millisNow()));
    desk.ready = true;
    CHECK(!desk.pass(supervisor));
    CHECK(desk.bus.log.empty());
    CHECK_EQ(desk.reports.size(), 1);
    CHECK_EQ(desk.reports.back().result.refusals, ss::REFUSE_STALE);
  }
  {
    // Flash refuses the request: it is refused at once, and nothing wants the line.
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.failSaves = 1;
    desk.ask(supervisor, kRequestId);
    CHECK_EQ(desk.reports.size(), 1);
    CHECK_EQ(desk.reports.back().result.refusals, ss::REFUSE_JOURNAL);
    CHECK(!supervisor.wantsLine(desk.clock(), desk.millisNow()));
    CHECK(!desk.pass(supervisor));
  }
  {
    // A duty after a start with a journal that could not be read: at once, then at its turns.
    Desk desk;
    ss::Supervisor supervisor;
    supervisor.begin(ss::cautiousJournal(), &desk, desk.millisNow());
    CHECK(supervisor.dutyOwed());
    CHECK(supervisor.wantsLine(desk.clock(), desk.millisNow()));
    CHECK(supervisor.wantsLine(0.0, desk.millisNow()));  // a duty does not wait for the clock
    desk.sim.powered = false;
    CHECK(desk.pass(supervisor));  // no answer: still owed
    CHECK(supervisor.dutyOwed());
    CHECK(!supervisor.wantsLine(desk.clock(), desk.millisNow()));
    desk.advance(ss::DUTY_RETRY_MS - 1);
    CHECK(!supervisor.wantsLine(desk.clock(), desk.millisNow()));
    desk.advance(1);
    CHECK(supervisor.wantsLine(desk.clock(), desk.millisNow()));
    CHECK(desk.pass(supervisor));  // the second try: no answer again
    CHECK(!supervisor.wantsLine(desk.clock(), desk.millisNow()));
    // A request makes the duty due at once, and runs after it. (The line rests first after the
    // request that got no reply.)
    desk.sim.powered = true;
    desk.advance(desk.master.responseTimeoutMs);
    desk.ask(supervisor, kRequestId);
    CHECK(supervisor.wantsLine(desk.clock(), desk.millisNow()));
    CHECK(desk.pass(supervisor));  // the duty
    CHECK(!supervisor.dutyOwed());
    CHECK(supervisor.wantsLine(desk.clock(), desk.millisNow()));
    CHECK(desk.pass(supervisor));  // the request
    CHECK_EQ(desk.reports.back().requestId, kRequestId);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK(!supervisor.wantsLine(desk.clock(), desk.millisNow()));
  }
}

// The client gives up a request that waits when a newer config no longer carries it, or no
// longer suits it (Supervisor::withdraw()).
static void testWithdraw() {
  const ss::Profile profile = ss::lifepo4Profile();
  const SimSunSaver fresh;
  {
    // Waiting for the clock: withdrawn, refused with the reasons given, never taken on again.
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.clockSet = false;
    CHECK(!supervisor.withdraw(ss::REFUSE_WITHDRAWN, desk.clock(), desk.millisNow()));  // nothing waits
    CHECK_EQ(supervisor.waitingRequestId(), 0u);
    CHECK(desk.ask(supervisor, kRequestId));
    CHECK_EQ(supervisor.waitingRequestId(), kRequestId);
    CHECK(!desk.pass(supervisor));
    CHECK(!supervisor.withdraw(0, desk.clock(), desk.millisNow()));  // no reason: nothing to do
    CHECK(supervisor.requestWaiting());
    CHECK(supervisor.withdraw(ss::REFUSE_WITHDRAWN | ss::REFUSE_NOT_LIFEPO4, desk.clock(), desk.millisNow()));
    desk.collect(supervisor);
    CHECK(!supervisor.requestWaiting());
    CHECK_EQ(supervisor.waitingRequestId(), 0u);
    CHECK(!supervisor.withdraw(ss::REFUSE_WITHDRAWN, desk.clock(), desk.millisNow()));  // once
    CHECK_EQ(desk.reports.size(), 1);
    CHECK_EQ(desk.reports.back().requestId, kRequestId);
    const ss::ProgramResult &result = desk.reports.back().result;
    CHECK_EQ(result.outcome, ss::OUTCOME_REFUSED);
    CHECK_EQ(result.refusals, ss::REFUSE_WITHDRAWN | ss::REFUSE_NOT_LIFEPO4);
    CHECK_EQ(result.profileErrors, 0);
    CHECK_EQ(result.planned, 0);
    CHECK(!result.wroteAny);
    CHECK(result.profile.lvdCv == profile.lvdCv && result.profile.absorbCv == profile.absorbCv);
    // The config that carries it again changes nothing, and nothing goes to the controller.
    CHECK(!desk.ask(supervisor, kRequestId));
    desk.clockSet = true;
    CHECK_EQ(desk.loopFor(supervisor, 60000), 0u);
    CHECK(desk.bus.log.empty());
    CHECK_EQ(desk.saves.size(), 3);  // the request, its refusal, and that the refusal went out
    CHECK(settledOn(desk.flash, kRequestId));
    CHECK_EQ(desk.reports.size(), 1);
    CHECK(!supervisor.wantsLine(desk.clock(), desk.millisNow()));
    // A newer request is taken on, and runs.
    CHECK(desk.ask(supervisor, kRequestId + 1));
    CHECK_EQ(desk.loopFor(supervisor, 1000), 1u);
    CHECK_EQ(desk.reports.back().requestId, kRequestId + 1);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_PROGRAMMED);
  }
  {
    // Taken on (in the journal) and waiting for the charger: the journal goes back to idle at
    // the next pass, and nothing is sent.
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.ready = false;
    CHECK(desk.ask(supervisor, kRequestId));
    CHECK(!desk.pass(supervisor));
    CHECK(holdsWaiting(desk.flash, kRequestId));
    CHECK(supervisor.withdraw(ss::REFUSE_NO_CHARGER, desk.clock(), desk.millisNow()));
    CHECK(desk.flash.reportDue && !holdsWaiting(desk.flash, kRequestId));  // in flash before it goes out
    desk.collect(supervisor);
    CHECK_EQ(desk.reports.back().result.refusals, ss::REFUSE_NO_CHARGER);
    CHECK(settledOn(supervisor.journal(), kRequestId));
    desk.ready = true;
    CHECK(!desk.pass(supervisor));
    CHECK(settledOn(desk.flash, kRequestId));
    CHECK(desk.bus.log.empty());
    CHECK(!supervisor.dutyOwed());
    CHECK(!supervisor.wantsLine(desk.clock(), desk.millisNow()));
    // A restart now owes the controller nothing.
    desk.boot(supervisor);
    CHECK(!supervisor.dutyOwed());
    CHECK_EQ(desk.reports.size(), 1);
    CHECK(!desk.ask(supervisor, kRequestId));  // the journal holds it now
  }
  {
    // A duty is owed and a request waits behind it: the request goes, the duty stays and runs.
    Desk desk;
    ss::Supervisor supervisor;
    supervisor.begin(ss::cautiousJournal(), &desk, desk.millisNow());
    desk.ready = false;
    CHECK(desk.ask(supervisor, kRequestId));
    CHECK(supervisor.withdraw(ss::REFUSE_WITHDRAWN, desk.clock(), desk.millisNow()));
    desk.collect(supervisor);
    CHECK_EQ(desk.reports.size(), 1);
    CHECK_EQ(desk.reports.back().result.refusals, ss::REFUSE_WITHDRAWN);
    CHECK(supervisor.dutyOwed());
    desk.ready = true;
    CHECK(supervisor.wantsLine(desk.clock(), desk.millisNow()));
    CHECK(desk.pass(supervisor));
    CHECK(!supervisor.dutyOwed());
    CHECK_EQ(desk.reports.size(), 2);
    CHECK_EQ(desk.reports.back().requestId, 0u);  // the journal that could not be read named none
    CHECK_EQ(desk.bus.count('w'), 0);  // a look at a controller that waits for nothing writes nothing
    CHECK(onlyPlanChanged(desk.sim, fresh));
  }
  {
    // The text, and the result as the server shows it.
    ss::ProgramResult result;
    ss::refusedResult(profile, 0, ss::REFUSE_WITHDRAWN, &result);
    checkRoundTrip(result, "withdrawn");
    ss::ProgramResult decoded;
    CHECK(decodes(wireOf(result), &decoded));
    JsonDocument page;
    describedAs(decoded, &page);
    CHECK_EQ(page["reasons"].size(), 1);
    CHECK_HAS(std::string(page["reasons"][0].as<const char *>()), "no longer asked for these values");
  }
}

// What the journal keeps for the client across its restarts, and the one result it holds for
// the server.
static void testJournalKeeps() {
  const ss::Profile profile = ss::lifepo4Profile();

  // A result that had not gone out when the client restarted goes out after it, with the time it
  // came about.
  {
    Desk desk;
    desk.flash = journalOf(kRequestId, ss::JOURNAL_IDLE);
    desk.flash.reportDue = true;
    desk.flash.reportId = kRequestId;
    desk.flash.reportEpoch = (uint32_t)kClientNow;
    ss::refusedResult(profile, 0, ss::REFUSE_STALE, &desk.flash.report);
    desk.advance(3000);
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    CHECK_EQ(desk.reports.size(), 1);
    if (desk.reports.size() == 1) {
      CHECK_EQ(desk.reports[0].requestId, kRequestId);
      CHECK_EQ(desk.reports[0].epoch, (uint32_t)kClientNow);
      CHECK_EQ(desk.reports[0].atMs, 0u);  // the client's millis() when it started: the best it knows
      CHECK_EQ(desk.reports[0].result.refusals, ss::REFUSE_STALE);
    }
    CHECK_EQ(desk.saves.size(), 1);  // that it went out
    CHECK(settledOn(desk.flash, kRequestId));
    CHECK(!desk.ask(supervisor, kRequestId));
  }
  // The newest request it has answered, even one it refused, is remembered across the restart.
  {
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.ask(supervisor, kRequestId);
    CHECK(desk.pass(supervisor));
    CHECK(desk.ask(supervisor, kRequestId + 10, profile, ss::PROFILE_LVD));
    CHECK_EQ(desk.reports.size(), 2);
    ss::Supervisor restarted;
    desk.boot(restarted);
    CHECK_EQ(desk.reports.size(), 2);
    CHECK(!desk.ask(restarted, kRequestId + 10, profile, ss::PROFILE_LVD));
    CHECK(!desk.ask(restarted, kRequestId + 5));
    CHECK(!restarted.requestWaiting());
    CHECK_EQ(desk.reports.size(), 2);
    CHECK(desk.ask(restarted, kRequestId + 11));
  }

  // The slot holds one result. One of an older request never takes the place of one for a newer
  // request that has yet to go out: the server keeps the newest request's result anyway.
  {
    Desk desk;
    desk.flash = journalOf(kRequestId, ss::JOURNAL_WRITING);  // the older request owes a look
    ss::Supervisor supervisor;
    desk.ready = false;
    desk.boot(supervisor);
    CHECK(desk.reports.empty());
    // A newer request waits behind the duty and is withdrawn. Its refusal has not gone out yet.
    CHECK(supervisor.request(kRequestId + 100, profile, 0, 0, true, desk.clock(), desk.millisNow()));
    CHECK(supervisor.withdraw(ss::REFUSE_WITHDRAWN, desk.clock(), desk.millisNow()));
    CHECK(supervisor.reportDue() && supervisor.reportRequestId() == kRequestId + 100);
    // The duty runs and ends: its result for the older request is not kept.
    CHECK(supervisor.service(desk, desk.clock(), true));
    CHECK(!supervisor.dutyOwed());
    CHECK(supervisor.reportDue());
    CHECK_EQ(supervisor.reportRequestId(), kRequestId + 100);
    CHECK_EQ(supervisor.report().refusals, ss::REFUSE_WITHDRAWN);
    CHECK(desk.flash.reportDue && desk.flash.reportId == kRequestId + 100);  // and so does flash
    CHECK_EQ(desk.flash.phase, ss::JOURNAL_IDLE);
    desk.collect(supervisor);
    CHECK_EQ(desk.reports.size(), 1);
    CHECK_EQ(desk.reports.back().requestId, kRequestId + 100);
    CHECK(settledOn(desk.flash, kRequestId + 100));
  }
  {
    // A later result of the same request takes the place of one that has not gone out, and so
    // does a result of a newer request.
    Desk desk;
    desk.flash = journalOf(kRequestId, ss::JOURNAL_WRITING);
    desk.sim.powered = false;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    CHECK(supervisor.service(desk, desk.clock(), true));  // no answer: still owed
    CHECK(supervisor.dutyOwed());
    CHECK(supervisor.reportDue() && supervisor.reportRequestId() == kRequestId);
    CHECK_EQ(supervisor.report().outcome, ss::OUTCOME_NO_LINK);
    desk.sim.powered = true;
    desk.advance(ss::DUTY_RETRY_MS);
    CHECK(supervisor.service(desk, desk.clock(), true));
    CHECK(!supervisor.dutyOwed());
    CHECK_EQ(supervisor.reportRequestId(), kRequestId);
    CHECK_EQ(supervisor.report().outcome, ss::OUTCOME_INTERRUPTED);
    CHECK(supervisor.request(kRequestId + 1, profile, ss::PROFILE_HVD, 0, true, desk.clock(), desk.millisNow()));
    CHECK_EQ(supervisor.reportRequestId(), kRequestId + 1);
    CHECK_EQ(supervisor.report().refusals, ss::REFUSE_PROFILE);
    desk.collect(supervisor);
    CHECK_EQ(desk.reports.size(), 1);
  }

  // The journal names the controller its run talked to. A duty goes to that one, even when the
  // config names another now and after the client restarted; once it is settled, the config
  // decides again.
  {
    Desk desk;
    desk.bus.slave = 7;
    desk.sim.slaveId = 7;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    CHECK_EQ(supervisor.controllerToUse(3), 3);  // nothing owed: the config decides
    desk.bus.before = [&desk](size_t, char kind, uint16_t) {
      if (kind == 'c' && desk.bus.count('w') > 0) {
        desk.sim.powered = false;
      }
    };
    desk.ask(supervisor, kRequestId);
    CHECK(desk.pass(supervisor));
    CHECK(supervisor.dutyOwed());
    CHECK_EQ(desk.flash.controller, 7);
    CHECK_EQ(supervisor.controllerToUse(1), 7);
    desk.advance(20000);
    ss::Supervisor restarted;
    desk.boot(restarted);
    CHECK(restarted.dutyOwed());
    CHECK_EQ(restarted.controllerToUse(1), 7);
    desk.bus.before = nullptr;
    desk.sim.powered = true;
    CHECK(desk.pass(restarted));
    CHECK(!restarted.dutyOwed());
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_INTERRUPTED);
    CHECK_EQ(restarted.controllerToUse(1), 1);
    CHECK(!editPending(desk.sim));
  }
  {
    // A journal that names no controller, such as one that could not be read: the config decides.
    Desk desk;
    ss::Supervisor supervisor;
    supervisor.begin(ss::cautiousJournal(), &desk, 0);
    CHECK(supervisor.dutyOwed());
    CHECK_EQ(supervisor.controllerToUse(4), 4);
  }

  // A journal whose request that waits is newer than the newest it has answered (this code never
  // writes one): once that request is answered as interrupted, the same config does not bring
  // it back.
  {
    Desk desk;
    desk.flash = ss::emptyJournal();
    desk.flash.waitingId = kRequestId;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    CHECK_EQ(desk.reports.size(), 1);
    CHECK_EQ(desk.reports.back().requestId, kRequestId);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_INTERRUPTED);
    CHECK(!desk.ask(supervisor, kRequestId));
    CHECK(!supervisor.requestWaiting());
    CHECK(desk.ask(supervisor, kRequestId + 1));
    CHECK(supervisor.requestWaiting());
  }

  // A clock that reads less than CLOCK_SET_EPOCH was never set: a result made then has no time.
  {
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    CHECK(supervisor.request(kRequestId, profile, ss::PROFILE_HVD, 0, true, 1000.0, desk.millisNow()));
    CHECK(supervisor.reportDue());
    CHECK_EQ(supervisor.reportEpoch(), 0u);
    CHECK(supervisor.request(kRequestId + 1, profile, ss::PROFILE_HVD, 0, true, ss::CLOCK_SET_EPOCH + 5.5,
                             desk.millisNow()));
    CHECK_EQ(supervisor.reportEpoch(), (uint32_t)ss::CLOCK_SET_EPOCH + 5u);
  }

  // Without a store nothing can be kept, so nothing is taken on.
  {
    Desk desk;
    ss::Supervisor supervisor;
    CHECK(supervisor.request(kRequestId, profile, 0, 0, true, desk.clock(), desk.millisNow()));
    CHECK(!supervisor.requestWaiting());
    CHECK(supervisor.reportDue());
    CHECK_EQ(supervisor.report().refusals, ss::REFUSE_JOURNAL);
    CHECK(!supervisor.service(desk, desk.clock(), true));
    CHECK(desk.bus.log.empty());
  }
}

static void testSupervisorSweeps() {
  const ss::Profile profile = ss::lifepo4Profile();
  const SimSunSaver fresh;
  const size_t length = quietRun().size();

  // The client loses its power as it is about to make any one request of a run, or to write
  // any one journal, or just after flash took it. With the controller still powered, and with
  // the controller on the same supply, so that it restarts as well.
  for (int both = 0; both < 2; ++both) {
    for (size_t at = 0; at <= length; ++at) {
      Desk desk;
      ss::Supervisor supervisor;
      desk.boot(supervisor);
      desk.dieAtRequest = at;
      if (both == 1) {
        desk.onDeath = [&desk]() { powerCycle(desk.sim); };
      }
      bool died = false;
      try {
        desk.ask(supervisor, kRequestId);
        desk.pass(supervisor);
      } catch (const PowerLoss &) {
        died = true;
      }
      CHECK_EQ(died, at < length);
      if (!died) {
        CHECK_EQ(desk.reports.size(), 1);
        continue;  // the run that nothing cut short
      }
      CHECK(desk.reports.empty());  // a result is only said once its run has ended
      checkAfterRestart(desk, fresh, profile, both == 1 ? "client and controller gone at request" : "client gone at request",
                        at, both == 1);
    }
    // The journal is written five times: the request, the two commit points, the result, and
    // that the result has gone out.
    for (size_t save = 0; save < 5; ++save) {
      for (int after = 0; after < 2; ++after) {
        Desk desk;
        ss::Supervisor supervisor;
        desk.boot(supervisor);
        desk.dieAtSave = save;
        desk.dieAfterSave = after == 1;
        if (both == 1) {
          desk.onDeath = [&desk]() { powerCycle(desk.sim); };
        }
        bool died = false;
        try {
          desk.ask(supervisor, kRequestId);
          desk.pass(supervisor);
        } catch (const PowerLoss &) {
          died = true;
        }
        CHECK(died);
        CHECK_EQ(desk.reports.size(), save == 4 ? 1u : 0u);  // the last write follows the result's way out
        checkAfterRestart(desk, fresh, profile, after == 1 ? "client gone after journal write" : "client gone at journal write",
                          save, both == 1);
      }
    }
  }

  // The same with a controller that was waiting for a restart before the run began.
  for (size_t at = 0; at < length; at += 3) {
    Desk desk;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.sim.regs[A_ARRAY_FAULT] = EDIT_ARRAY;
    chargeForPending(desk.sim);
    desk.dieAtRequest = at;
    bool died = false;
    try {
      desk.ask(supervisor, kRequestId);
      desk.pass(supervisor);
    } catch (const PowerLoss &) {
      died = true;
    }
    if (!died) {
      continue;
    }
    const ss::Journal held = desk.flash;
    const bool waited = editPending(desk.sim);
    desk.advance(20000);
    ss::Supervisor restarted;
    desk.boot(restarted);
    desk.loopFor(restarted, 5000);
    if (held.phase == ss::JOURNAL_IDLE) {
      // Before the first commit point the client had sent nothing, and owes nothing: the
      // controller waits as it did before, and the next run restarts it first.
      CHECK_MSG(editPending(desk.sim) == waited, "pending before, client gone at request %u", (unsigned)at);
    } else {
      CHECK_MSG(!editPending(desk.sim), "pending before, client gone at request %u: still waiting", (unsigned)at);
    }
    CHECK_MSG(!restarted.dutyOwed() && desk.flash.phase == ss::JOURNAL_IDLE, "pending before, client gone at request %u", (unsigned)at);
    desk.ask(restarted, kRequestId + 600);
    desk.loopFor(restarted, 3000);
    CHECK_MSG(ss::outcomeIsGood(desk.reports.back().result.outcome) && holdsProfile(desk.sim, profile) && !editPending(desk.sim),
              "pending before, client gone at request %u: the next request ended as %s", (unsigned)at,
              ss::outcomeName(desk.reports.back().result.outcome));
    CHECK_EQ(desk.bus.disorder, 0);
  }
}

// ============================================================================
// The time limit
// ============================================================================

// The moment from a run's first step at which only its shortest safe end is left.
static uint32_t cutFor(uint16_t timeoutMs) {
  return ss::RUN_LIMIT_MS - ss::RUN_END_TIMEOUTS * (uint32_t)timeoutMs - ss::RUN_END_SLACK_MS;
}

// A hook that moves the clock on to the cut just before the request with index `at` goes out.
static void cutAt(Station &station, size_t at) {
  station.bus.before = [&station, at](size_t index, char, uint16_t) {
    if (index == at) {
      station.sim.advance(cutFor((uint16_t)station.master.responseTimeoutMs));
    }
  };
}

// The client's loop is held for the whole of a run: never longer than RUN_LIMIT_MS, whatever the
// Modbus timeout and however the line behaves. Past the cut only the shortest safe end is left:
// nothing more is written, a controller that was written to still gets its restart command, and
// one that may still wait for its restart is owed a later look.
static void testTimeLimit() {
  const ss::Profile profile = ss::lifepo4Profile();
  const SimSunSaver fresh;
  CHECK_EQ(ss::RUN_LIMIT_MS, 60000u);
  CHECK_EQ(cutFor(4000), 30000u);  // the longest timeout a client's config allows leaves half a minute
  CHECK_EQ(cutFor(1000), 51000u);

  // The run of the finding: a 4 s timeout, and a controller that is gone after the restart
  // command that follows the writes. Twenty probes, each with its rest, would take 160 s.
  {
    Desk desk;
    desk.master.responseTimeoutMs = 4000;
    ss::Supervisor supervisor;
    desk.boot(supervisor);
    desk.bus.before = [&desk](size_t, char kind, uint16_t) {
      if (kind == 'c' && desk.bus.count('w') > 0) {
        desk.sim.powered = false;
      }
    };
    desk.ask(supervisor, kRequestId);
    const uint64_t began = desk.clockMs;
    CHECK(desk.pass(supervisor));
    const uint64_t took = desk.clockMs - began;
    CHECK_MSG(took > cutFor(4000) && took <= ss::RUN_LIMIT_MS, "the run took %llu ms", (unsigned long long)took);
    CHECK_EQ(desk.reports.size(), 1);
    if (desk.reports.size() == 1) {
      const ss::ProgramResult &result = desk.reports[0].result;
      CHECK_EQ(result.outcome, ss::OUTCOME_RESTART_FAILED);
      CHECK_EQ(result.restart, ss::RESTART_NO_ANSWER);
      CHECK(result.followUp);
      CHECK((result.notes & ss::NOTE_TIME_LIMIT) != 0);
      CHECK(result.probes < ss::BACK_PROBES);
      CHECK_EQ(ss::countEntries(result, ss::ENTRY_WRITTEN), 15);
    }
    CHECK(supervisor.dutyOwed());
    // Its later look has a minute of its own, and ends the run as it would have ended.
    desk.bus.before = nullptr;
    desk.sim.powered = true;
    CHECK_EQ(desk.loopFor(supervisor, ss::DUTY_RETRY_MS + 1000, 60000), 1u);
    CHECK_EQ(desk.reports.back().result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK((desk.reports.back().result.notes & ss::NOTE_TIME_LIMIT) != 0);  // it tells how the run went
    CHECK(!supervisor.dutyOwed());
    CHECK(!editPending(desk.sim));
  }

  // The controller never comes back from the first restart command: at a 2 s timeout the time
  // limit ends the wait before BACK_PROBES does. Nothing was written.
  {
    Station station;
    station.master.responseTimeoutMs = 2000;
    station.bus.before = [&](size_t, char kind, uint16_t) {
      if (kind == 'c') {
        station.sim.resetDownMs = 3600UL * 1000UL;
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_NO_LINK);
    CHECK_EQ(result.restart, ss::RESTART_NO_ANSWER);
    CHECK((result.notes & ss::NOTE_TIME_LIMIT) != 0);
    CHECK(result.probes < ss::BACK_PROBES);
    CHECK(!result.wroteAny);
    CHECK(!result.followUp);
    CHECK_EQ(station.bus.count('w'), 0);
    const uint64_t took = station.bus.log.back().end - station.bus.log.front().at;
    CHECK_MSG(took > cutFor(2000) && took <= ss::RUN_LIMIT_MS, "the run took %llu ms", (unsigned long long)took);
  }

  // The cut comes as the first restart command is due: it is not sent, since nothing has been
  // written that needs it.
  {
    Station station;
    cutAt(station, ss::ITEM_COUNT - 1);
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_NO_LINK);
    CHECK_EQ(result.restart, ss::RESTART_NOT_NEEDED);
    CHECK((result.notes & ss::NOTE_TIME_LIMIT) != 0);
    CHECK_EQ(station.bus.count('c'), 0);
    CHECK_EQ(station.bus.log.size(), ss::ITEM_COUNT);
    CHECK_EQ(station.sim.resets, 0);
    CHECK(!result.followUp);
    CHECK(onlyPlanChanged(station.sim, fresh));
  }
  // The cut comes during the first reading: the run ends there.
  {
    Station station;
    cutAt(station, 10);
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_NO_LINK);
    CHECK((result.notes & ss::NOTE_TIME_LIMIT) != 0);
    CHECK_EQ(station.bus.log.size(), 11u);
    CHECK_EQ(station.bus.count('c') + station.bus.count('w'), 0);
  }

  // The cut comes as the third register is written: its read-back follows, then the restart
  // command, and nothing more is written. A probe that the restarting controller misses ends
  // the wait, so the run owes it a look.
  {
    const size_t thirdWrite = ss::ITEM_COUNT + 4 + 4;  // two writes with their read-backs before it
    Station station;
    cutAt(station, thirdWrite);
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(station.bus.log[thirdWrite].kind, 'w');
    CHECK_EQ(result.outcome, ss::OUTCOME_STOPPED);
    CHECK((result.notes & ss::NOTE_TIME_LIMIT) != 0);
    CHECK_EQ(station.bus.count('w'), 3);
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_WRITTEN), 3);
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_PENDING), ss::PLAN_ENTRIES - 3 - 2);
    CHECK_EQ(station.bus.log[thirdWrite + 1].kind, 'r');  // the read-back
    CHECK_EQ(station.bus.log[thirdWrite + 1].addr, station.bus.log[thirdWrite].addr);
    CHECK_EQ(station.bus.log[thirdWrite + 2].kind, 'c');
    CHECK_EQ(station.bus.log.size(), thirdWrite + 4);  // and one probe
    CHECK_EQ(result.restart, ss::RESTART_NO_ANSWER);
    CHECK(result.followUp);
    CHECK(station.programmer.controllerUnknown());
    CHECK_EQ(station.bus.disorder, 0);
    checkPauses(station);
    // Taken up again, with a time limit of its own: the controller had restarted.
    station.bus.before = nullptr;
    station.sim.advance(5000);
    CHECK(station.programmer.resume());
    station.drive();
    CHECK_EQ(result.outcome, ss::OUTCOME_STOPPED);
    CHECK_EQ(result.restart, ss::RESTART_DONE);
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_WRITTEN), 3);
    CHECK(!editPending(station.sim));
    CHECK(!station.programmer.controllerUnknown());
    CHECK(onlyPlanChanged(station.sim, fresh) && beforeOrTarget(station.sim, fresh, profile));
  }

  // The same, and the read-back of that register gets no reply: past the cut it is not asked
  // again, and the restart command follows.
  {
    const size_t thirdWrite = ss::ITEM_COUNT + 4 + 4;
    Station station;
    station.bus.before = [&station, thirdWrite](size_t index, char, uint16_t) {
      if (index == thirdWrite) {
        station.sim.advance(cutFor((uint16_t)station.master.responseTimeoutMs));
      } else if (index == thirdWrite + 1) {
        station.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_STOPPED);
    CHECK((result.notes & ss::NOTE_TIME_LIMIT) != 0);
    CHECK_EQ(station.bus.log[thirdWrite].kind, 'w');
    CHECK_EQ(station.bus.log[thirdWrite + 1].kind, 'r');
    CHECK_EQ(station.bus.log[thirdWrite + 1].status, ss::SSIO_NO_REPLY);
    CHECK_EQ(station.bus.log[thirdWrite + 2].kind, 'c');
    CHECK_EQ(station.bus.count('w'), 3);
    CHECK(result.followUp);
    checkPauses(station);
  }

  // A timeout so long that no safe end fits in RUN_LIMIT_MS (no config allows one): neither a
  // run nor a look at the controller sends anything, and the look stays owed.
  {
    Station station;
    station.master.responseTimeoutMs = 9000;
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_NO_LINK);
    CHECK((result.notes & ss::NOTE_TIME_LIMIT) != 0);
    CHECK(station.bus.log.empty());
    Station looking;
    looking.master.responseTimeoutMs = 9000;
    looking.programmer.startRecovery(false);
    looking.drive();
    CHECK_EQ(looking.programmer.result().outcome, ss::OUTCOME_NO_LINK);
    CHECK(looking.programmer.result().followUp);
    CHECK(looking.bus.log.empty());
  }

  // The cut comes while the controller waits for its restart: the command goes out at once.
  {
    Station station;
    station.bus.before = [&](size_t, char kind, uint16_t addr) {
      if (kind == 'r' && addr == A_LOAD_STATE && station.bus.count('w') == 15 && station.bus.count('c') == 1) {
        station.sim.advance(cutFor((uint16_t)station.master.responseTimeoutMs));
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK((result.notes & ss::NOTE_TIME_LIMIT) != 0);
    CHECK_EQ(station.bus.count('c'), 2);
    const size_t lastWrite = station.bus.nth('w', 14);
    CHECK_EQ(station.bus.log[lastWrite + 2].addr, A_LOAD_STATE);
    CHECK_EQ(station.bus.log[lastWrite + 3].kind, 'c');  // and not the two fault words first
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_WRITTEN), 15);
  }

  // The cut comes as the registers are read again after the restart: those not read are
  // unconfirmed, and the run says so.
  {
    Station station;
    station.bus.before = [&](size_t index, char, uint16_t) {
      if (station.bus.count('c') == 2 && index == station.bus.nth('c', 1) + 6) {
        station.sim.advance(cutFor((uint16_t)station.master.responseTimeoutMs));
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_UNVERIFIED);
    CHECK_EQ(result.restart, ss::RESTART_DONE);
    CHECK((result.notes & ss::NOTE_TIME_LIMIT) != 0);
    CHECK((result.notes & ss::NOTE_VERIFY_INCOMPLETE) != 0);
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_UNREAD), ss::PLAN_ENTRIES - 3);
    CHECK_EQ(result.loadStateAfter, ss::LOAD_STATE_NOT_READ);
    CHECK(!result.followUp);
    CHECK_EQ(station.bus.log.size(), station.bus.nth('c', 1) + 7);
  }

  // The cut comes after a restart command that did nothing: there is no time for the second
  // command, so the run leaves it to a later look.
  {
    Station station;
    station.bus.before = [&](size_t, char kind, uint16_t) {
      if (kind == 'c' && station.bus.count('c') == 1) {
        station.sim.resetWorks = false;
        station.sim.advance(cutFor((uint16_t)station.master.responseTimeoutMs));
      }
    };
    const ss::ProgramResult &result = station.run();
    CHECK_EQ(result.outcome, ss::OUTCOME_RESTART_FAILED);
    CHECK_EQ(result.restart, ss::RESTART_NOT_DONE);
    CHECK((result.notes & ss::NOTE_TIME_LIMIT) != 0);
    CHECK_EQ(station.bus.count('c'), 2);
    CHECK(result.followUp);
    CHECK(station.programmer.controllerUnknown());
    CHECK(editPending(station.sim));
    // Past the cut the controller that answers is not asked ten times more: one look at each of
    // its fault words, and the run ends.
    CHECK_EQ(station.bus.log.size(), station.bus.nth('c', 1) + 3);
    // The later look sends the command it could not send then, and leaves nothing owed.
    station.bus.before = nullptr;
    station.sim.resetWorks = true;
    CHECK(station.programmer.resume());
    station.drive();
    CHECK_EQ(result.restart, ss::RESTART_DONE);
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(station.bus.count('c'), 3);
    CHECK(!editPending(station.sim));
    CHECK(!result.followUp);
    CHECK(!station.programmer.controllerUnknown());
  }

  // Every way the line can go dead, at every request of a run, at the shortest and the longest
  // timeout, with replies that come at once and replies that come just in time. Whatever happens,
  // the run holds the client no longer than RUN_LIMIT_MS, writes nothing past the cut, and leaves
  // the client owing a look to a controller that may wait for its restart.
  const uint16_t timeouts[] = {500, 1000, 4000};
  unsigned cut = 0;
  for (size_t t = 0; t < sizeof(timeouts) / sizeof(timeouts[0]); ++t) {
    for (int slow = 0; slow < 2; ++slow) {
      for (size_t dead = 0;; ++dead) {
        Desk desk;
        desk.master.responseTimeoutMs = timeouts[t];
        if (slow == 1) {
          desk.sim.responseDelayMs = timeouts[t] - 150;
        }
        ss::Supervisor supervisor;
        desk.boot(supervisor);
        desk.bus.before = [&desk, dead](size_t index, char, uint16_t) {
          if (index == dead) {
            desk.sim.powered = false;
          }
        };
        desk.ask(supervisor, kRequestId);
        const uint64_t began = desk.clockMs;
        CHECK(desk.pass(supervisor));
        const uint64_t took = desk.clockMs - began;
        CHECK_MSG(took <= ss::RUN_LIMIT_MS, "timeout %u, %s replies, dead from request %u: the run took %llu ms",
                  (unsigned)timeouts[t], slow == 1 ? "slow" : "quick", (unsigned)dead, (unsigned long long)took);
        const uint64_t start = desk.bus.log.empty() ? 0 : desk.bus.log.front().at;
        for (size_t i = 0; i < desk.bus.log.size(); ++i) {
          if (desk.bus.log[i].kind == 'w') {
            CHECK_MSG(desk.bus.log[i].at - start < cutFor(timeouts[t]), "timeout %u: a write %llu ms into the run",
                      (unsigned)timeouts[t], (unsigned long long)(desk.bus.log[i].at - start));
          }
        }
        CHECK_EQ(desk.reports.size(), 1);
        if (editPending(desk.sim)) {
          CHECK_MSG(supervisor.dutyOwed(), "timeout %u, %s replies, dead from request %u: the controller waits, and no look is owed",
                    (unsigned)timeouts[t], slow == 1 ? "slow" : "quick", (unsigned)dead);
        }
        CHECK(desk.bus.disorder == 0);
        if (!desk.reports.empty() && (desk.reports.back().result.notes & ss::NOTE_TIME_LIMIT) != 0) {
          ++cut;
        }
        if (dead >= desk.bus.log.size()) {
          break;  // the line stayed up for the whole run
        }
      }
    }
  }
  CHECK(cut > 100);  // the cut was reached in many of those runs, not only in a few
}

// ============================================================================
// Text
// ============================================================================
static void testText() {
  // Every name and sentence is plain ASCII, and no two values share one.
  for (uint8_t outcome = 0; outcome <= ss::OUTCOME_LAST; ++outcome) {
    CHECK(plainAscii(ss::outcomeName(outcome)));
    CHECK(plainAscii(ss::outcomeText(outcome)));
    for (uint8_t other = 0; other < outcome; ++other) {
      CHECK(strcmp(ss::outcomeName(outcome), ss::outcomeName(other)) != 0);
      CHECK(strcmp(ss::outcomeText(outcome), ss::outcomeText(other)) != 0);
    }
    CHECK_EQ(ss::outcomeIsGood(outcome), outcome == ss::OUTCOME_PROGRAMMED || outcome == ss::OUTCOME_UNCHANGED);
  }
  CHECK_TEXT(std::string(ss::outcomeName(ss::OUTCOME_LAST + 1)), std::string("none"));
  CHECK_TEXT(std::string(ss::outcomeText(ss::OUTCOME_LAST + 1)), std::string(ss::outcomeText(ss::OUTCOME_NONE)));
  CHECK(!ss::outcomeIsGood(ss::OUTCOME_LAST + 1));
  CHECK_TEXT(std::string(ss::outcomeName(ss::OUTCOME_PROGRAMMED)), std::string("programmed"));
  CHECK_TEXT(std::string(ss::outcomeName(ss::OUTCOME_RESTART_FAILED)), std::string("restart failed"));

  for (uint8_t status = 0; status <= ss::ENTRY_STATUS_LAST; ++status) {
    CHECK(plainAscii(ss::entryStatusName(status)));
    CHECK(strcmp(ss::entryStatusName(status), "?") != 0);
    for (uint8_t other = 0; other < status; ++other) {
      CHECK(strcmp(ss::entryStatusName(status), ss::entryStatusName(other)) != 0);
    }
  }
  CHECK_TEXT(std::string(ss::entryStatusName(ss::ENTRY_STATUS_LAST + 1)), std::string("?"));

  for (uint8_t restart = 0; restart <= ss::RESTART_STATUS_LAST; ++restart) {
    CHECK(plainAscii(ss::restartText(restart)));
    CHECK(strcmp(ss::restartText(restart), "?") != 0);
    for (uint8_t other = 0; other < restart; ++other) {
      CHECK(strcmp(ss::restartText(restart), ss::restartText(other)) != 0);
    }
  }
  CHECK_TEXT(std::string(ss::restartText(ss::RESTART_STATUS_LAST + 1)), std::string("?"));

  for (uint8_t bit = 0; bit < 32; ++bit) {
    const char *text = ss::refusalText(1ul << bit);
    CHECK(plainAscii(text));
    CHECK_EQ(has(text, "no text for"), bit >= ss::REFUSAL_BITS);
    for (uint8_t other = 0; other < bit && bit < ss::REFUSAL_BITS; ++other) {
      CHECK(strcmp(text, ss::refusalText(1ul << other)) != 0);
    }
  }
  CHECK(has(ss::refusalText(0), "no text for"));
  for (uint8_t bit = 0; bit < 16; ++bit) {
    const char *text = ss::noteText((uint16_t)(1u << bit));
    CHECK(plainAscii(text));
    CHECK_EQ(has(text, "no text for"), bit >= ss::NOTE_BITS);
    for (uint8_t other = 0; other < bit && bit < ss::NOTE_BITS; ++other) {
      CHECK(strcmp(text, ss::noteText((uint16_t)(1u << other))) != 0);
    }
  }
  CHECK(has(ss::noteText(0), "no text for"));
  // The bits are where a server of another version expects them.
  CHECK_EQ(ss::REFUSE_PROFILE, 1u);
  CHECK_EQ(ss::REFUSE_NO_RESTART, 0x80u);
  CHECK_EQ(ss::REFUSE_NOT_BUILT, 0x100u);
  CHECK_EQ(ss::REFUSE_JOURNAL, 0x1000u);
  CHECK_EQ(ss::NOTE_AT_FIRST_RESTART, 0x80u);
  CHECK_EQ(ss::OUTCOME_INTERRUPTED, 8);
  CHECK_EQ(ss::ENTRY_LOST, 7);
  CHECK_EQ(ss::RESTART_UNCONFIRMED, 5);

  // One line per register.
  {
    Station station;
    const ss::ProgramResult &result = station.run();
    const char *want[ss::PLAN_ENTRIES] = {
        "EV_eq: 0 (off), already set",
        "EV_eq2: 14.90 V -> 0 (off), written",
        "EV_tempcomp: 0.0305 V/C -> 0 (off), written",
        "EV_hvd: 15.20 V -> 14.45 V, written",
        "EV_hvr: 14.00 V -> 13.60 V, written",
        "Evb_ref_lim: 15.00 V -> 14.45 V, written",
        "EV_reg: 14.00 V -> 14.20 V, written",
        "EV_float: 13.70 V -> 13.50 V, written",
        "Et_float: 9000 s (150 min) -> 900 s (15 min), written",
        "Et_floatlb: 10800 s (180 min) -> 900 s (15 min), written",
        "EV_reg2: 14.40 V -> 14.20 V, written",
        "EV_float2: 13.70 V -> 13.50 V, written",
        "Et_float2: 9000 s (150 min) -> 900 s (15 min), written",
        "Et_floatlb2: 10800 s (180 min) -> 900 s (15 min), written",
        "EV_lvr: 12.60 V -> 13.25 V, written",
        "EV_lvd: 11.50 V -> 13.10 V, written",
        "ER_icomp: 0 (off), already set"};
    for (uint8_t i = 0; i < ss::PLAN_ENTRIES; ++i) {
      CHECK_TEXT(textFrom([&](ss::Text *t) { ss::describeEntry(result.entries[i], result.scale, t); }), std::string(want[i]));
    }
    CHECK_TEXT(textFrom([&](ss::Text *t) { ss::describeRun(result, t); }),
             std::string("programmed: 15 of 17 registers written, 2 already set"));
    CHECK_TEXT(textFrom([&](ss::Text *t) { ss::describeLoad(result, t); }),
             std::string("LOAD output: LOAD_ON before the run, LOAD_ON while the charger waited for its restart, "
                         "LOAD_ON at the end."));
    // A register the run did not reach, and one that holds a word no state has.
    ss::PlanEntry entry = result.entries[15];
    entry.status = ss::ENTRY_PENDING;
    CHECK_TEXT(textFrom([&](ss::Text *t) { ss::describeEntry(entry, ss::SCALE_100, t); }),
             std::string("EV_lvd: 11.50 V -> 13.10 V, not reached"));
    entry.status = ss::ENTRY_NOT_TAKEN;
    CHECK_TEXT(textFrom([&](ss::Text *t) { ss::describeEntry(entry, ss::SCALE_100, t); }),
             std::string("EV_lvd: 11.50 V -> 13.10 V, not taken"));
    // The same words read with the other scale.
    CHECK_TEXT(textFrom([&](ss::Text *t) { ss::describeEntry(entry, ss::SCALE_96667, t); }),
             std::string("EV_lvd: 11.12 V -> 12.66 V, not taken"));
  }
  {
    ss::ProgramResult result;
    ss::clearResult(&result);
    CHECK_TEXT(textFrom([&](ss::Text *t) { ss::describeLoad(result, t); }), std::string(""));
    result.loadStateAfter = 3;
    CHECK_TEXT(textFrom([&](ss::Text *t) { ss::describeLoad(result, t); }), std::string("LOAD output: LVD at the end."));
    result.loadStateBefore = ss::LOAD_STATE_OTHER;
    result.loadStateAfter = ss::LOAD_STATE_NOT_READ;
    CHECK_TEXT(textFrom([&](ss::Text *t) { ss::describeLoad(result, t); }),
             std::string("LOAD output: state 254 before the run."));
    result.loadStateBefore = 5;
    result.loadStateEdit = 2;
    CHECK_TEXT(textFrom([&](ss::Text *t) { ss::describeLoad(result, t); }),
             std::string("LOAD output: DISCONNECT before the run, LVD_WARNING while the charger waited for its restart."));
    result.outcome = ss::OUTCOME_REFUSED;
    CHECK_TEXT(textFrom([&](ss::Text *t) { ss::describeRun(result, t); }), std::string("refused"));
    // A refusal without the figures behind it is its sentence and nothing else.
    for (uint8_t bit = 0; bit < ss::REFUSAL_BITS; ++bit) {
      CHECK_TEXT(textFrom([&](ss::Text *t) { ss::describeRefusal(1ul << bit, result, t); }),
               std::string(ss::refusalText(1ul << bit)));
    }
    // The figures of one refusal are not added to another.
    result.seen = ss::SEEN_BATTERY | ss::SEEN_LOAD_CURRENT | ss::SEEN_FAULTS;
    for (uint8_t bit = 0; bit < ss::REFUSAL_BITS; ++bit) {
      const std::string text = textFrom([&](ss::Text *t) { ss::describeRefusal(1ul << bit, result, t); });
      const uint32_t refusal = 1ul << bit;
      const bool figures = refusal == ss::REFUSE_BATTERY_LOW || refusal == ss::REFUSE_LOAD_CURRENT || refusal == ss::REFUSE_FAULT;
      CHECK_EQ(text != std::string(ss::refusalText(refusal)), figures);
    }
    // A load current below zero is shown as such.
    result.loadCurrentRaw = 65528;
    CHECK_HAS(textFrom([&](ss::Text *t) { ss::describeRefusal(ss::REFUSE_LOAD_CURRENT, result, t); }), "(-0.02 A)");
  }
}

// ============================================================================
// Small things
// ============================================================================
static void testSmallThings() {
  // A programmer nothing was started on.
  {
    Station station;
    CHECK(!station.programmer.active());
    CHECK(!station.programmer.ended());
    CHECK_EQ(station.programmer.awaiting(), ss::COMMIT_NONE);
    CHECK(station.programmer.step(station.bus, 4));
    CHECK(!station.programmer.missed());
    CHECK_EQ(station.programmer.pauseMs(), 0);
    station.programmer.go();
    station.programmer.refuse(ss::REFUSE_JOURNAL);
    CHECK(station.programmer.step(station.bus, 4));
    CHECK(station.bus.log.empty());
    CHECK_EQ(station.programmer.result().outcome, ss::OUTCOME_NONE);
    CHECK_EQ(station.programmer.result().refusals, 0);
    CHECK_EQ(station.programmer.result().loadStateBefore, ss::LOAD_STATE_NOT_READ);
  }
  // go() and refuse() do nothing to a run that does not wait for them, and nothing after its end.
  {
    Station station;
    station.programmer.start(ss::lifepo4Profile(), true);
    station.programmer.go();
    station.programmer.refuse(ss::REFUSE_JOURNAL);
    CHECK(station.programmer.active());
    CHECK(station.bus.log.empty());
    CHECK(!station.programmer.step(station.bus, 0));  // no budget: no request, and not ended
    CHECK(station.bus.log.empty());
    station.drive();
    const size_t had = station.bus.log.size();
    CHECK_EQ(station.programmer.result().outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(station.programmer.result().refusals, 0);
    station.programmer.go();
    station.programmer.refuse(ss::REFUSE_JOURNAL);
    CHECK(station.programmer.step(station.bus, 4));
    CHECK_EQ(station.bus.log.size(), had);
    CHECK_EQ(station.programmer.result().outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(station.programmer.result().refusals, 0);
    // The registers as the run found them stay available.
    CHECK_EQ(station.programmer.before().raw[ss::I_EV_LVD], 3768);  // 11.50 V
    CHECK(ss::have(station.programmer.before(), ss::I_DIP_SWITCH));
  }
  // A run started while another one is under way begins anew.
  {
    Station station;
    station.programmer.start(ss::lifepo4Profile(), true);
    CHECK(!station.programmer.step(station.bus, 10));
    ss::Profile wanted = ss::lifepo4Profile();
    wanted.lvdCv = 1250;
    const ss::ProgramResult &result = station.run(wanted);
    CHECK_EQ(result.outcome, ss::OUTCOME_PROGRAMMED);
    CHECK_EQ(result.profile.lvdCv, 1250);
    CHECK_EQ(station.bus.log.size(), 10 + quietRun().size());
    CHECK(holdsProfile(station.sim, wanted));
  }

  {
    ss::ProgramResult result;
    memset(&result, 0x5A, sizeof(result));
    ss::clearResult(&result);
    CHECK_EQ(result.outcome, ss::OUTCOME_NONE);
    CHECK_EQ(result.planned, 0);
    CHECK_EQ(result.loadStateBefore, ss::LOAD_STATE_NOT_READ);
    CHECK_EQ(result.loadStateEdit, ss::LOAD_STATE_NOT_READ);
    CHECK_EQ(result.loadStateAfter, ss::LOAD_STATE_NOT_READ);
    CHECK(!result.wroteAny);
    CHECK_EQ(result.entries[ss::PLAN_ENTRIES - 1].target, 0);

    const ss::Profile profile = ss::lifepo4Profile();
    memset(&result, 0x5A, sizeof(result));
    ss::refusedResult(profile, ss::PROFILE_LVD, ss::REFUSE_PROFILE | ss::REFUSE_NOT_BUILT, &result);
    CHECK_EQ(result.outcome, ss::OUTCOME_REFUSED);
    CHECK_EQ(result.profileErrors, ss::PROFILE_LVD);
    CHECK_EQ(result.refusals, ss::REFUSE_PROFILE | ss::REFUSE_NOT_BUILT);
    CHECK_EQ(result.profile.hvdCv, 1445);
    CHECK_EQ(result.planned, 0);
    CHECK_EQ(result.notes, 0);
    CHECK_EQ(result.restart, ss::RESTART_NOT_NEEDED);

    memset(&result, 0x5A, sizeof(result));
    ss::interruptedResult(&result);
    CHECK_EQ(result.outcome, ss::OUTCOME_INTERRUPTED);
    CHECK_EQ(result.notes, ss::NOTE_NOTHING_WRITTEN);
    CHECK_EQ(result.refusals, 0);
    CHECK_EQ(result.planned, 0);

    // Counting stops at the entries a result can hold, whatever it says it holds.
    ss::clearResult(&result);
    result.planned = 200;
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_PENDING), ss::PLAN_ENTRIES);
    result.planned = 3;
    result.entries[1].status = ss::ENTRY_WRITTEN;
    result.entries[5].status = ss::ENTRY_WRITTEN;
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_WRITTEN), 1);
    CHECK_EQ(ss::countEntries(result, ss::ENTRY_PENDING), 2);

    result.batteryRaw = 4483;
    result.liveScale = ss::SCALE_100;
    CHECK(near(ss::batteryVolts(result), 13.681, 0.001));
    result.liveScale = ss::SCALE_96667;
    CHECK(near(ss::batteryVolts(result), 13.225, 0.001));
    result.liveScale = ss::SCALE_UNKNOWN;
    CHECK(near(ss::batteryVolts(result), 13.225, 0.001));
  }
  CHECK(near(ss::ampsFromRaw(58), 0.140, 0.001));
  CHECK(near(ss::ampsFromRaw(65528), -0.019, 0.001));
  CHECK(near(ss::ampsFromRaw(0x7FFF), 79.158, 0.001));
  CHECK_EQ(ss::loadStateOf(0), 0);
  CHECK_EQ(ss::loadStateOf(5), 5);
  CHECK_EQ(ss::loadStateOf(0xFD), 0xFD);
  CHECK_EQ(ss::loadStateOf(0xFE), ss::LOAD_STATE_OTHER);
  CHECK_EQ(ss::loadStateOf(0xFF), ss::LOAD_STATE_OTHER);
  CHECK_EQ(ss::loadStateOf(0x0100), ss::LOAD_STATE_OTHER);
  CHECK_EQ(ss::loadStateOf(0xFFFF), ss::LOAD_STATE_OTHER);
  CHECK_EQ(ss::COIL_RESTART, 0x00FF);
}

int main() {
  startTestTimeLimit();
  testProfile();
  testPlan();
  testQuietRun();
  testPendingBefore();
  testRefusals();
  testFirstRestart();
  testWrites();
  testFinalRestart();
  testRecovery();
  testSweeps();
  testText();
  testSmallThings();
  testRequestJson();
  testResultJson();
  testSupervisor();
  testWantsLine();
  testWithdraw();
  testJournalKeeps();
  testSupervisorSweeps();
  testTimeLimit();
  CHECK_EQ(gAllocLive, 0);

  if (gFailures != 0) {
    fprintf(stderr, "sunsaver_program: %ld of %ld checks FAILED\n", gFailures, gChecks);
    return 1;
  }
  printf("sunsaver_program: all %ld checks passed\n", gChecks);
  return 0;
}
