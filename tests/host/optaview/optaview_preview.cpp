// OptaView on a PC: the web page and the USB console of the firmware, with the simulated
// SunSaver (sim_sunsaver.h) in place of the RS-485 port and a TCP socket of this PC in place of
// the Opta's Ethernet port.
//
//   make preview ARDUINOJSON_DIR=/path/to/ArduinoJson/src
//   then open http://127.0.0.1:8080/ in a browser
//
// Everything above the hardware is the firmware's own code: the HTTP reader, the login, the
// API, the console and the page as it is built into the sketch, with its Content-Security-Policy.
// So this is where a change to web/index.html is tried out, and what the browser test
// (browser/page_test.js) drives. What it cannot show is the Opta's network stack; the address
// policy of OptaViewNet.h is not part of it either.
//
// The terminal is the USB console: type 'help', 'status', 'web pin 2468'. A line that starts
// with "sim" goes to the simulated controller instead:
//
//   sim power off|on       the controller loses or gets back its power
//   sim battery <volts>    what it measures at the battery terminals
//   sim refuse on|off      it refuses every write (Modbus exception 4)
//   sim delay <ms>         how long it takes to answer
//   sim drop <n>           its next n replies are lost on the way (it does what was asked)
//   sim skip <seconds>     this much time passes at once (a session ends after 30 minutes)
//   sim writes             how many requests that change something it has received
//   sim random off|on      the Opta's random number generator fails (the clock stands in for it),
//                          or works again
//   sim quit
//
// Options: --port <n> (default 8080; 0 takes any free port and prints it), --pin <digits>
// (start with this PIN, as OptaViewConfig.h does on the Opta).

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "OptaViewBuiltinProfile.h"
#include "OptaViewConsole.h"
#include "OptaViewDevice.h"
#include "OptaViewHttp.h"
#include "OptaViewModbus.h"
#include "OptaViewProfile.h"
#include "OptaViewText.h"
#include "OptaViewWeb.h"
#include "OptaViewWebPage.h"
#include "preview_terminal.h"
#include "sim_sunsaver.h"

// A send to a browser that has gone raises SIGPIPE, which main() ignores, so the flag that keeps
// one send from raising it is only a second guard. macOS has no MSG_NOSIGNAL.
#ifdef MSG_NOSIGNAL
#define OV_SEND_NO_SIGNAL MSG_NOSIGNAL
#else
#define OV_SEND_NO_SIGNAL 0
#endif

using namespace optaview;

static uint64_t realMs() {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000ULL + (uint64_t)(now.tv_nsec / 1000000L);
}

// The simulated controller on a clock that keeps step with the PC's, as millis() does on the
// Opta: a millisecond takes a millisecond, whether the firmware spends it waiting or working.
// (The tests run it on a clock that only counts.)
class LiveSunSaver : public SimSunSaver {
 public:
  unsigned writes;  // requests that change something: a coil (function 5) or a register (6)

  LiveSunSaver() : writes(0), startMs_(realMs()), skippedMs_(0) {}

  // Everything that asks for the time, or for what has arrived by now, first lets the time pass
  // that the firmware has spent working. A loop that never waits still has to see its time limit
  // come: ovHttpDrain() takes bytes for as long as they keep arriving, and for half a second.
  uint32_t nowMs() override {
    catchUp();
    return SimSunSaver::nowMs();
  }

  int read() override {
    catchUp();
    return SimSunSaver::read();
  }

  void send(const uint8_t *data, size_t len) override {
    catchUp();
    if (len > 1 && (data[1] == 0x05 || data[1] == 0x06)) {
      ++writes;
    }
    SimSunSaver::send(data, len);
    // The log of every frame is for the tests. A preview may run for days.
    requests.clear();
    requestTimes.clear();
  }

  // Waits a millisecond. Where the simulation is ahead of the PC's clock, as it is by the time
  // a frame takes to send, this waits until the PC's clock is there as well.
  void idle() override {
    catchUp();
    advance(1);
    const uint64_t now = realMs();
    const uint64_t due = startMs_ + (elapsedMs() - skippedMs_);
    if (due > now) {
      struct timespec wait;
      wait.tv_sec = (time_t)((due - now) / 1000ULL);
      wait.tv_nsec = (long)((due - now) % 1000ULL) * 1000000L;
      nanosleep(&wait, nullptr);
    }
  }

  // Time that passes without anybody waiting for it.
  void skip(uint32_t ms) {
    advance(ms);
    skippedMs_ += ms;
  }

 private:
  uint64_t startMs_;
  uint64_t skippedMs_;

  // Brings the simulation's clock up to the PC's where it has fallen behind.
  void catchUp() {
    const uint64_t byThePc = realMs() - startMs_ + skippedMs_;
    while (byThePc > elapsedMs()) {
      const uint64_t behind = byThePc - elapsedMs();
      advance((uint32_t)(behind > 0x40000000ULL ? 0x40000000ULL : behind));
    }
  }
};

// The web server's clock is the same one, so that a session's half hour and a reply's timeout
// are counted alike.
struct LiveClock : OvClock {
  LiveSunSaver &sim;
  explicit LiveClock(LiveSunSaver &s) : sim(s) {}
  uint32_t nowMs() override { return sim.nowMs(); }
  void idle() override { sim.idle(); }
};

// One browser connection.
class PcConn : public OvConn {
 public:
  PcConn() : fd_(-1) {}
  bool isFree() const { return fd_ < 0; }
  void open(int fd) { fd_ = fd; }

  int read() override {
    uint8_t byte;
    if (fd_ >= 0 && ::recv(fd_, &byte, 1, MSG_DONTWAIT) == 1) {
      return byte;
    }
    return -1;  // nothing now, or the browser has closed: the reader's time limits deal with both
  }

  // Like the Opta's Ethernet client with its socket timeout at 0: what fits goes, and nothing
  // waits. Unlike it, this one can tell a browser that is gone.
  int write(const uint8_t *data, size_t len) override {
    if (fd_ < 0) {
      return -1;
    }
    const ssize_t sent = ::send(fd_, data, len, MSG_DONTWAIT | OV_SEND_NO_SIGNAL);
    if (sent >= 0) {
      return (int)sent;
    }
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
  }

  void close() override {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  int fd_;
};

// False while "sim random off" stands for an Opta whose random number generator fails.
static bool gRandomWorks = true;

class PcWebHost : public OvWebHost {
 public:
  PcWebHost() : listener_(-1), port_(0) {}

  // Listens on this PC only: 127.0.0.1.
  bool listen(uint16_t port) {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) {
      return false;
    }
    const int on = 1;
    ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    socklen_t size = sizeof(address);
    if (::bind(listener_, (struct sockaddr *)&address, sizeof(address)) != 0 || ::listen(listener_, 8) != 0 ||
        ::getsockname(listener_, (struct sockaddr *)&address, &size) != 0 ||
        ::fcntl(listener_, F_SETFL, O_NONBLOCK) != 0) {
      ::close(listener_);
      listener_ = -1;
      return false;
    }
    port_ = ntohs(address.sin_port);
    return true;
  }

  uint16_t port() const { return port_; }

  OvConn *accept() override {
    for (size_t i = 0; i < sizeof(conns_) / sizeof(conns_[0]); ++i) {
      if (conns_[i].isFree()) {
        const int fd = ::accept(listener_, nullptr, nullptr);
        if (fd < 0) {
          return nullptr;
        }
        conns_[i].open(fd);
        return &conns_[i];
      }
    }
    return nullptr;
  }

  bool random(uint8_t *out, size_t len) override {
    if (!gRandomWorks) {
      // What the Opta falls back on is the timing of events; here the clock stands in for it.
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      uint64_t mix = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
      for (size_t i = 0; i < len; ++i) {
        mix = mix * 6364136223846793005ULL + 1442695040888963407ULL;
        out[i] = (uint8_t)(mix >> 56);
      }
      return false;
    }
    FILE *source = fopen("/dev/urandom", "rb");
    if (source == nullptr || fread(out, 1, len, source) != len) {
      fprintf(stderr, "optaview_preview: /dev/urandom cannot be read\n");
      exit(1);
    }
    fclose(source);
    return true;
  }

  bool address(char *out, size_t cap) override {
    snprintf(out, cap, "127.0.0.1:%u", (unsigned)port_);
    return true;
  }

 private:
  int listener_;
  uint16_t port_;
  PcConn conns_[OV_WEB_SLOTS];  // the server holds no more than this many at a time
};

// The terminal as the USB console's output. What is typed there comes in through
// PreviewTerminal (preview_terminal.h), which also answers "has a key been pressed".
class TerminalOut : public OvOut {
 public:
  void write(const char *text, size_t len) override { fwrite(text, 1, len, stdout); }
};

static bool isOnOff(const std::string &word, bool *on) {
  if (word == "on" || word == "off") {
    *on = word == "on";
    return true;
  }
  return false;
}

// A whole number in decimal digits and nothing else, at most `most`. strtol would take "8080typo"
// as 8080, and "nope" or "" as 0, which for --port means any free port.
static bool parseWhole(const char *text, unsigned long most, unsigned long *out) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  unsigned long value = 0;
  for (const char *p = text; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') {
      return false;
    }
    value = value * 10UL + (unsigned long)(*p - '0');
    if (value > most) {
      return false;
    }
  }
  *out = value;
  return true;
}

// A "sim ..." line: the word after "sim" and at most one value, each read whole. A word too many,
// a value cut short or one that is only partly a number changes nothing and gets the list of
// commands, so "sim battery 12 .4" cannot leave the battery at 12 V. Every change is confirmed
// with one line that starts with "sim:", which the browser test waits for.
static void simCommand(LiveSunSaver &sim, const std::string &line) {
  std::vector<std::string> words;
  size_t at = 0;
  while (at < line.size()) {
    const size_t begin = line.find_first_not_of(" \t", at);
    if (begin == std::string::npos) {
      break;
    }
    const size_t end = line.find_first_of(" \t", begin);
    words.push_back(line.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
    at = end == std::string::npos ? line.size() : end;
  }
  const std::string what = words.size() > 1 ? words[1] : std::string();
  const bool bare = words.size() == 2;                           // "sim writes"
  const char *value = words.size() == 3 ? words[2].c_str() : "";  // "sim battery 12.4", or none
  bool on = false;
  double number = 0.0;
  unsigned long whole = 0;
  const bool decimal = ovParseDecimal(value, &number);
  if (what == "power" && isOnOff(value, &on)) {
    sim.powered = on;
    printf("sim: the controller has %s\n", on ? "power" : "no power");
  } else if (what == "battery" && decimal && number >= 0.0 && number <= 40.0) {
    sim.setBattery(number);
    printf("sim: the battery is at %.2f V\n", number);
  } else if (what == "refuse" && isOnOff(value, &on)) {
    sim.refuseWrites = on;
    printf("sim: writes are %s\n", on ? "refused" : "accepted");
  } else if (what == "delay" && parseWhole(value, 10000UL, &whole)) {
    sim.responseDelayMs = (uint32_t)whole;
    printf("sim: replies take %u ms\n", (unsigned)sim.responseDelayMs);
  } else if (what == "drop" && parseWhole(value, 20UL, &whole) && whole >= 1UL) {
    sim.failNext(SimSunSaver::FAULT_LOST_REPLY, (unsigned)whole);
    printf("sim: the next %u replies are lost\n", (unsigned)whole);
  } else if (what == "skip" && decimal && number >= 0.0 && number <= 86400.0) {
    sim.skip((uint32_t)(number * 1000.0));
    printf("sim: %s s have passed\n", value);
  } else if (what == "writes" && bare) {
    printf("sim: %u writes so far\n", sim.writes);
  } else if (what == "random" && isOnOff(value, &on)) {
    gRandomWorks = on;
    printf("sim: the random number generator %s\n", on ? "works" : "fails");
  } else if (what == "quit" && bare) {
    printf("sim: bye\n");
    fflush(stdout);
    exit(0);
  } else {
    printf("sim: power off|on, battery <volts>, refuse on|off, delay <ms>, drop <replies>, skip <seconds>, writes, "
           "random off|on, quit\n");
  }
}

int main(int argc, char **argv) {
  unsigned long port = 8080;
  const char *pin = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      if (!parseWhole(argv[++i], 65535UL, &port)) {
        fprintf(stderr, "optaview_preview: a port is a whole number from 0 to 65535, not '%s'\n", argv[i]);
        return 2;
      }
    } else if (strcmp(argv[i], "--pin") == 0 && i + 1 < argc) {
      pin = argv[++i];
    } else {
      fprintf(stderr, "usage: optaview_preview [--port <n>] [--pin <4 to 8 digits>]\n");
      return 2;
    }
  }
  signal(SIGPIPE, SIG_IGN);

  static LiveSunSaver sim;
  static ModbusMaster master(sim);
  static OvProfile profile;
  static OvDevice device(master, profile);
  static LiveClock clock(sim);
  static PcWebHost host;
  static TerminalOut out;
  static PreviewTerminal terminal(STDIN_FILENO);
  static OvConsole console(out, terminal, device);
  static const OvWebPage page = {OV_WEB_PAGE, OV_WEB_PAGE_LENGTH, OV_WEB_PAGE_HEADERS};
  static OvWeb web(device, host, clock, page);

  char problem[160];
  if (!profile.load(OV_BUILTIN_PROFILE, sizeof(OV_BUILTIN_PROFILE) - 1, problem, sizeof(problem))) {
    fprintf(stderr, "optaview_preview: the built-in profile did not load (%s)\n", problem);
    return 1;
  }
  device.useProfileDefaults();
  device.applyLink();
  if (pin != nullptr && !web.setPin(pin)) {
    fprintf(stderr, "optaview_preview: a PIN is 4 to 8 digits\n");
    return 2;
  }
  if (!host.listen((uint16_t)port)) {
    fprintf(stderr, "optaview_preview: cannot listen on 127.0.0.1:%lu (%s)\n", port, strerror(errno));
    return 1;
  }
  console.addExtra(&web);

  printf("OptaView preview with a simulated SunSaver: http://127.0.0.1:%u/\n", (unsigned)host.port());
  printf("This terminal is its USB console. 'sim' lists what changes the simulated controller.\n");
  console.begin();
  console.prompt();
  fflush(stdout);

  for (;;) {
    // One line at most, and then the web server has its turn, as in the Opta's loop(): lines
    // that wait (a paste) must not keep the page waiting until the last of them has run.
    std::string line;
    if (terminal.takeLine(&line)) {
      // A NUL would end the line early for everything that reads it as text ("sim delay 100",
      // then "ms" left out), and a line as long as the terminal keeps of one is longer than any
      // command. Either goes to the console whatever it starts with: the console refuses it
      // whole, and as the answer to a question it cancels it.
      const bool plain = line.find('\0') == std::string::npos && line.size() < OV_LINE_MAX;
      if (plain && line.compare(0, 3, "sim") == 0 && (line.size() == 3 || line[3] == ' ' || line[3] == '\t')) {
        simCommand(sim, line);
      } else {
        console.handleLine(line.c_str(), line.size());
      }
      console.prompt();
      fflush(stdout);
    }
    web.poll();
    sim.idle();
  }
}
