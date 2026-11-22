// What the OptaView host tests share: the allocation hooks for the profile loader, a minimal
// test framework, and rigs that put the code under test on a simulated SunSaver.
//
// Each test program (optaview_test.cpp, optaview_web_test.cpp) is one translation unit and
// includes this file once.

#ifndef OPTAVIEW_TEST_SUPPORT_H
#define OPTAVIEW_TEST_SUPPORT_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <unistd.h>
#endif

#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

// The profile loader allocates through OV_ALLOC and OV_FREE (see OptaViewProfile.h). Here they
// count what is live and can make any one allocation fail.
static long gAllocCalls = 0;
static long gAllocFailAt = -1;
static long gAllocLive = 0;

inline void *testAlloc(size_t count, size_t size) {
  if (gAllocCalls++ == gAllocFailAt) {
    return nullptr;
  }
  ++gAllocLive;
  return calloc(count, size);
}

inline void testFree(const void *pointer) {
  if (pointer != nullptr) {
    --gAllocLive;
    free(const_cast<void *>(pointer));
  }
}

#define OV_ALLOC(count, size) testAlloc((count), (size))
#define OV_FREE(pointer) testFree(pointer)

#include "OptaViewBuiltinProfile.h"
#include "OptaViewConsole.h"
#include "OptaViewDevice.h"
#include "OptaViewModbus.h"
#include "OptaViewProfile.h"
#include "OptaViewText.h"
#include "sim_sunsaver.h"

using namespace optaview;

// ============================================================================
// Minimal test framework
// ============================================================================
static long gChecks = 0;
static long gFailures = 0;
static std::string gTranscript;  // everything the console printed, checked for plain ASCII at the end

// A test that waits for something that never happens must not hold CI until the runner's own
// limit, which is hours. A wait inside the code under test ends on the simulated clock (see
// SimSunSaver::idle). This is for everything else: after ten minutes of real time the program
// says so and fails. OPTAVIEW_TEST_SECONDS sets another limit, and 0 switches it off, as for a
// debugger or for a tool that runs the tests many times slower. A value that is not a number of
// seconds, a day at most, changes nothing.
#if defined(__unix__) || defined(__APPLE__)
static void testTimeIsUp(int) {
  static const char text[] =
      "\noptaview: the tests did not finish within their time limit (OPTAVIEW_TEST_SECONDS, 600 if not set).\n"
      "One of them probably waits for something that never happens.\n";
  if (write(2, text, sizeof(text) - 1) < 0) {
    // Nothing to be done about that here.
  }
  _exit(124);
}

inline void startTestTimeLimit() {
  unsigned long seconds = 600;
  const char *text = getenv("OPTAVIEW_TEST_SECONDS");
  if (text != nullptr && text[0] != '\0') {
    char *end = nullptr;
    const unsigned long asked = strtoul(text, &end, 10);
    if (end != text && *end == '\0' && asked <= 86400UL) {
      seconds = asked;
    }
  }
  if (seconds > 0) {
    signal(SIGALRM, testTimeIsUp);
    alarm((unsigned)seconds);
  }
}
#else
inline void startTestTimeLimit() {}
#endif

inline std::string fmtStr(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
inline std::string fmtStr(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return std::string(buf);
}

inline void reportFailure(const char *file, int line, const char *expr, const std::string &detail) {
  ++gFailures;
  if (gFailures <= 100) {
    fprintf(stderr, "%s:%d: CHECK failed: %s%s%s\n", file, line, expr, detail.empty() ? "" : "  -- ", detail.c_str());
  }
}

#define CHECK(cond)                                                            \
  do {                                                                         \
    ++gChecks;                                                                 \
    if (!(cond)) reportFailure(__FILE__, __LINE__, #cond, std::string());      \
  } while (0)

#define CHECK_MSG(cond, ...)                                                    \
  do {                                                                          \
    ++gChecks;                                                                  \
    if (!(cond)) reportFailure(__FILE__, __LINE__, #cond, fmtStr(__VA_ARGS__)); \
  } while (0)

#define CHECK_EQ(actual, expected)                                                                  \
  do {                                                                                              \
    ++gChecks;                                                                                      \
    const long long checkActual = (long long)(actual);                                              \
    const long long checkExpected = (long long)(expected);                                          \
    if (checkActual != checkExpected)                                                               \
      reportFailure(__FILE__, __LINE__, #actual " == " #expected,                                   \
                    fmtStr("got %lld (0x%llX), expected %lld (0x%llX)", checkActual,               \
                           (unsigned long long)checkActual, checkExpected, (unsigned long long)checkExpected)); \
  } while (0)

inline std::string excerpt(const std::string &text) {
  return text.size() > 700 ? text.substr(0, 700) + "..." : text;
}

inline bool has(const std::string &text, const char *needle) { return text.find(needle) != std::string::npos; }

// These two take the text once. It is usually `rig.run("...")`, and a check that failed must not
// run the command a second time to show what it printed.
#define CHECK_HAS(text, needle)                                                                          \
  do {                                                                                                   \
    ++gChecks;                                                                                           \
    const std::string checkText = (text);                                                                \
    if (!has(checkText, (needle)))                                                                       \
      reportFailure(__FILE__, __LINE__, "output contains " #needle, "output was:\n" + excerpt(checkText)); \
  } while (0)

#define CHECK_HASNT(text, needle)                                                                                \
  do {                                                                                                           \
    ++gChecks;                                                                                                   \
    const std::string checkText = (text);                                                                        \
    if (has(checkText, (needle)))                                                                                \
      reportFailure(__FILE__, __LINE__, "output does not contain " #needle, "output was:\n" + excerpt(checkText)); \
  } while (0)

inline bool near(double a, double b, double tolerance) { return a > b ? a - b <= tolerance : b - a <= tolerance; }

// The line of `text` that contains `needle`, or "" when there is none.
inline std::string lineWith(const std::string &text, const char *needle) {
  const size_t at = text.find(needle);
  if (at == std::string::npos) {
    return std::string();
  }
  size_t start = text.rfind('\n', at);
  start = start == std::string::npos ? 0 : start + 1;
  size_t end = text.find('\r', at);
  if (end == std::string::npos) {
    end = text.size();
  }
  return text.substr(start, end - start);
}

inline size_t countOf(const std::string &text, const char *needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
    ++count;
  }
  return count;
}

inline std::string replaced(std::string text, const char *from, const char *to) {
  const size_t at = text.find(from);
  if (at == std::string::npos) {
    fprintf(stderr, "test bug: '%s' is not in the profile text\n", from);
    exit(2);
  }
  text.replace(at, strlen(from), to);
  return text;
}

// ============================================================================
// Rigs
// ============================================================================
struct StringOut : OvOut {
  std::string text;
  void write(const char *data, size_t len) override {
    // A command that prints without end has to fail its test, not fill the memory.
    if (text.size() > 16u * 1024u * 1024u) {
      throw std::runtime_error("it has printed more than 16 MB and has not returned");
    }
    text.append(data, len);
  }
};

struct FakeHost : OvConsoleHost {
  SimSunSaver *sim;
  uint64_t keyAt;
  unsigned discards;
  FakeHost() : sim(nullptr), keyAt(std::numeric_limits<uint64_t>::max()), discards(0) {}
  bool inputPending() override { return sim->elapsedMs() >= keyAt; }
  void discardInput() override {
    keyAt = std::numeric_limits<uint64_t>::max();
    ++discards;
  }
  // The user presses a key `ms` from now.
  void pressKeyAfter(uint32_t ms) { keyAt = sim->elapsedMs() + ms; }
};

// The Modbus master alone on a simulated controller.
struct Bare {
  SimSunSaver sim;
  ModbusMaster master;
  Bare() : master(sim) { sim.configure(9600, MB_8N2); }
};

// The whole stack: simulator, master, built-in profile, device and console.
struct Rig {
  SimSunSaver sim;
  ModbusMaster master;
  OvProfile profile;
  OvDevice device;
  StringOut out;
  FakeHost host;
  OvConsole console;

  // The numerators say how the simulated controller encodes the voltages it measures, its
  // stored settings (0 = like the measured ones) and its regulation target.
  explicit Rig(double voltsNumerator = 100.0, const char *profileJson = OV_BUILTIN_PROFILE,
               double settingsNumerator = 0.0, double referenceNumerator = 96.667)
      : sim(voltsNumerator, settingsNumerator, referenceNumerator),
        master(sim),
        device(master, profile),
        console(out, host, device) {
    char error[200];
    const bool ok = profile.load(profileJson, strlen(profileJson), error, sizeof(error));
    CHECK_MSG(ok, "profile did not load: %s", error);
    device.useProfileDefaults();
    device.applyLink();
    host.sim = &sim;
  }

  std::string run(const std::string &line) {
    out.text.clear();
    try {
      console.handleLine(line.c_str());
    } catch (const std::runtime_error &stuck) {
      // The simulator's clock or the output gave up on a command that would not end. That is
      // one failed check, and the tests carry on.
      ++gChecks;
      reportFailure(__FILE__, __LINE__, ("the command '" + line + "' returns").c_str(), stuck.what());
      out.text.clear();
    }
    gTranscript += out.text;
    return out.text;
  }

  // Requests the master has sent with this function code.
  size_t sent(uint8_t function) const {
    size_t count = 0;
    for (size_t i = 0; i < sim.requests.size(); ++i) {
      if (sim.requests[i].size() > 1 && sim.requests[i][1] == function) {
        ++count;
      }
    }
    return count;
  }
};

// A small profile for the tests that need their own register map.
static const char kMiniProfile[] =
    "{\"schema\":1,\"profile\":\"mini\",\"title\":\"Mini\",\"version\":\"1\","
    "\"scales\":{\"v\":{\"num\":100,\"den\":32768,\"units\":\"V\",\"decimals\":2}},"
    "\"labels\":{\"st\":[\"A\",\"B\"]},"
    "\"groups\":[{\"name\":\"g\",\"title\":\"G\"}],"
    "\"registers\":["
    "{\"addr\":\"0xE010\",\"name\":\"a\",\"scale\":\"v\",\"group\":\"g\",\"writable\":true,\"min\":1,\"max\":20},"
    "{\"addr\":57361,\"name\":\"b\",\"type\":\"enum\",\"labels\":\"st\",\"group\":\"g\"},"
    "{\"addr\":\"0xE012\",\"name\":\"c\",\"type\":\"u32\",\"group\":\"g\"},"
    "{\"addr\":\"0xE014\",\"name\":\"d\",\"group\":\"h\"},"
    "{\"addr\":\"0xE015\",\"name\":\"e\",\"type\":\"u32\",\"order\":\"lo-hi\",\"group\":\"h\"}],"
    "\"coils\":[{\"addr\":\"0xFF\",\"name\":\"reset\",\"trigger\":true}],"
    "\"modbus\":{\"slaveId\":1,\"baud\":9600,\"format\":\"8N2\",\"readFunction\":4,\"maxBlock\":3,"
    "\"probeRegister\":\"a\",\"resetCoil\":\"reset\"},"
    "\"liveGroups\":[\"g\"],"
    "\"checks\":[{\"below\":\"a\",\"above\":\"d\",\"message\":\"m\"}]}";

inline std::vector<uint8_t> bytes(std::initializer_list<int> list) {
  std::vector<uint8_t> out;
  for (int value : list) {
    out.push_back((uint8_t)value);
  }
  return out;
}

#endif  // OPTAVIEW_TEST_SUPPORT_H
