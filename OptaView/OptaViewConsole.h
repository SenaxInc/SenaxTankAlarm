/**
 * OptaViewConsole.h
 *
 * The OptaView console: one line of text in, text out. OptaView.ino feeds it from USB Serial;
 * the host tests feed it strings and read what it prints.
 *
 * Rules the console keeps (review findings F-26 / X-R03 on the v0.1 demo):
 *   - It only calls a controller "found" when a register read came back with data.
 *   - Settings are written by name, inside the profile's limits, after showing the old and the
 *     new value and after a typed "yes". Every write is read back.
 *   - Writing to an arbitrary address (raw function 5, 6 or 16) needs "expert on" first.
 *   - Long commands stop when a key is pressed.
 */

#ifndef OPTAVIEW_CONSOLE_H
#define OPTAVIEW_CONSOLE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "OptaViewDevice.h"
#include "OptaViewModbus.h"
#include "OptaViewProfile.h"
#include "OptaViewText.h"

namespace optaview {

static const char OV_VERSION[] = "0.2.0";
static const size_t OV_LINE_MAX = 160;
static const int OV_MAX_ARGS = 24;
static const size_t OV_VALUE_TEXT = 200;
static const size_t OV_WRAP = 100;  // longest line the console prints from a profile's free text

// What the console needs from its surroundings besides output.
class OvConsoleHost {
 public:
  virtual ~OvConsoleHost() {}
  // True when the user has typed something since the command started: long commands stop.
  virtual bool inputPending() = 0;
  // Throws away what was typed to stop a command.
  virtual void discardInput() = 0;
};

// Collects typed characters into a line, with echo and backspace, for a terminal that sends
// each key as it is pressed. Lines may end with CR, LF or CR LF.
class OvLineEditor {
 public:
  OvLineEditor() : length_(0), skipLineFeed_(false) { line_[0] = '\0'; }

  // Takes one character and echoes what the terminal should show. Returns true when it ended
  // a line: line() holds that line until the next character arrives.
  bool feed(int key, OvOut &echo) {
    if (key == '\n' && skipLineFeed_) {
      skipLineFeed_ = false;  // the second half of a CR LF
      return false;
    }
    skipLineFeed_ = (key == '\r');
    if (key == '\r' || key == '\n') {
      line_[length_] = '\0';
      length_ = 0;
      echo.println();
      return true;
    }
    if (key == '\b' || key == 0x7F) {
      if (length_ > 0) {
        --length_;
        echo.print("\b \b");
      }
      return false;
    }
    if (key < 0x20 || key > 0x7E) {
      return false;
    }
    if (length_ + 1 < sizeof(line_)) {  // a longer line loses its end and shows it: the echo stops
      line_[length_++] = (char)key;
      echo.printChar((char)key);
    }
    return false;
  }

  const char *line() const { return line_; }

  // Forgets a half-typed line (the terminal was closed and opened again).
  void clear() {
    length_ = 0;
    skipLineFeed_ = false;
    line_[0] = '\0';
  }

  // True right after a line that ended with CR. A line feed that arrives now belongs to the
  // same Enter key: it is not the key press that stops a long command. Whoever takes it away
  // calls lineFeedTaken().
  bool expectsLineFeed() const { return skipLineFeed_; }
  void lineFeedTaken() { skipLineFeed_ = false; }

 private:
  char line_[OV_LINE_MAX];
  size_t length_;
  bool skipLineFeed_;
};

class OvConsole : public MbTraceSink, public OvProbeListener, public OvAbortCheck {
 public:
  OvConsole(OvOut &out, OvConsoleHost &host, OvDevice &device)
      : out_(out),
        host_(host),
        device_(device),
        expert_(false),
        trace_(false),
        stopped_(false),
        blocksNoticeShown_(false),
        writeNotesShown_(false),
        resetPending_(false) {
    memset(&pending_, 0, sizeof(pending_));
    memset(&settingsScale_, 0, sizeof(settingsScale_));
    memset(&liveScale_, 0, sizeof(liveScale_));
  }

  void begin() {
    OvProfile &profile = device_.profile();
    out_.println();
    out_.print("OptaView ");
    out_.print(OV_VERSION);
    out_.println(": a Modbus console for charge controllers");
    out_.print("Profile: ");
    out_.print(profile.title());
    out_.print(", ");
    out_.println(profile.version());
    out_.print("Link: ");
    printLink();
    out_.println();
    out_.println("'probe' looks for the controller, 'live' reads it, 'help' lists everything.");
  }

  void prompt() { out_.print(pending_.kind != PENDING_NONE ? "yes/no> " : "optaview> "); }

  bool awaitingConfirmation() const { return pending_.kind != PENDING_NONE; }

  // Drops a question that is waiting for "yes" (the terminal went away). Returns true if one was.
  bool cancelPending() {
    const bool was = pending_.kind != PENDING_NONE;
    pending_.kind = PENDING_NONE;
    return was;
  }

  void handleLine(const char *line) {
    char buffer[OV_LINE_MAX];
    ovCopy(buffer, sizeof(buffer), line);
    char *argv[OV_MAX_ARGS];
    const int argc = split(buffer, argv, OV_MAX_ARGS);

    device_.beginBatch();
    stopped_ = false;
    settingsScale_.shown = false;
    liveScale_.shown = false;

    if (argc == 0) {
      return;  // an empty line is not an answer either: the question stays open
    }
    if (pending_.kind != PENDING_NONE) {
      const Pending action = pending_;
      pending_.kind = PENDING_NONE;
      if (argc == 1 && is(argv[0], "yes")) {
        runPending(action);
      } else {
        out_.println("Cancelled. Nothing was sent.");
      }
      return;
    }
    const char *cmd = argv[0];
    if (is(cmd, "help") || is(cmd, "?")) {
      cmdHelp();
    } else if (is(cmd, "status")) {
      cmdStatus();
    } else if (is(cmd, "probe")) {
      cmdProbe();
    } else if (is(cmd, "scan")) {
      cmdScan(argc, argv);
    } else if (is(cmd, "id")) {
      printIdentification();
    } else if (is(cmd, "wires")) {
      printWiring();
    } else if (is(cmd, "live") || is(cmd, "read")) {
      cmdLive();
    } else if (is(cmd, "watch")) {
      cmdWatch(argc, argv);
    } else if (is(cmd, "dump")) {
      cmdDump(argc, argv);
    } else if (is(cmd, "groups")) {
      cmdGroups();
    } else if (is(cmd, "get")) {
      cmdGet(argc, argv);
    } else if (is(cmd, "coils")) {
      cmdCoils();
    } else if (is(cmd, "snapshot")) {
      cmdSnapshot();
    } else if (is(cmd, "scalecheck")) {
      cmdScaleCheck(argc, argv);
    } else if (is(cmd, "scale")) {
      cmdScale(argc, argv);
    } else if (is(cmd, "set")) {
      cmdSet(argc, argv);
    } else if (is(cmd, "coil")) {
      cmdCoil(argc, argv);
    } else if (is(cmd, "reset")) {
      cmdReset(argc, argv);
    } else if (is(cmd, "link")) {
      cmdLink(argc, argv);
    } else if (is(cmd, "slave")) {
      cmdSlave(argc, argv);
    } else if (is(cmd, "fc")) {
      cmdFunction(argc, argv);
    } else if (is(cmd, "timeout")) {
      cmdTimeout(argc, argv);
    } else if (is(cmd, "blocks")) {
      cmdBlocks(argc, argv);
    } else if (is(cmd, "profile")) {
      cmdProfile();
    } else if (is(cmd, "trace")) {
      cmdTrace(argc, argv);
    } else if (is(cmd, "expert")) {
      cmdExpert(argc, argv);
    } else if (is(cmd, "sniff")) {
      cmdSniff(argc, argv);
    } else if (is(cmd, "raw")) {
      cmdRaw(argc, argv);
    } else if (is(cmd, "yes") || is(cmd, "no")) {
      out_.println("Nothing is waiting for a yes or a no.");
    } else {
      out_.print("Unknown command ");
      printQuoted(cmd);
      out_.println(". 'help' lists the commands.");
    }
  }

  // --- MbTraceSink ---
  void onTransaction(const uint8_t *tx, size_t txLen, const uint8_t *rx, size_t rxLen,
                     const MbResult &result) override {
    printExchange(tx, txLen, rx, rxLen, result);
  }

  // --- OvProbeListener ---
  void onProbeAttempt(const OvLink &tried, const MbResult &result) override {
    out_.print("  ");
    out_.printU(tried.baud);
    out_.printChar(' ');
    out_.print(mbFormatName(tried.format));
    out_.print(", slave ");
    out_.printU(tried.slave);
    out_.print(", function ");
    out_.printU(tried.readFunction);
    out_.print(": ");
    describe(result);
    out_.println();
  }
  bool probeAborted() override { return stopRequested(); }

  // --- OvAbortCheck ---
  bool aborted() override { return stopRequested(); }

 private:
  enum PendingKind : uint8_t { PENDING_NONE = 0, PENDING_SET, PENDING_COIL, PENDING_RESET, PENDING_RAW_WRITE };

  struct Pending {
    uint8_t kind;
    const OvRegister *reg;
    const OvCoil *coil;
    bool on;
    uint8_t slave;
    uint8_t function;
    uint16_t addr;
    uint16_t value;
  };

  OvOut &out_;
  OvConsoleHost &host_;
  OvDevice &device_;
  Pending pending_;
  bool expert_;
  bool trace_;
  bool stopped_;
  // What this session knows about one of the two voltage scales that 'scalecheck' tests. The
  // stored settings and the live readings have a scale each, and each is settled on its own
  // evidence.
  struct ScaleState {
    double found;  // the numerator 'scalecheck' found on this controller; 0 = nothing yet
    bool shown;    // the command that is running printed a value that uses it
    bool noticed;  // the user has been told that it is not confirmed
  };
  ScaleState settingsScale_;
  ScaleState liveScale_;
  bool blocksNoticeShown_;
  bool writeNotesShown_;
  bool resetPending_;

  // -------------------------------------------------------------------------------------------
  // Small helpers
  // -------------------------------------------------------------------------------------------

  static bool is(const char *a, const char *b) { return ovEqualsIgnoreCase(a, b); }

  static int split(char *line, char **argv, int maxArgs) {
    int argc = 0;
    char *p = line;
    while (*p != '\0' && argc < maxArgs) {
      while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        ++p;
      }
      if (*p == '\0') {
        break;
      }
      argv[argc++] = p;
      while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
        ++p;
      }
      if (*p != '\0') {
        *p++ = '\0';
      }
    }
    return argc;
  }

  static bool parseOnOff(const char *text, bool *on) {
    if (is(text, "on") || is(text, "1")) {
      *on = true;
      return true;
    }
    if (is(text, "off") || is(text, "0")) {
      *on = false;
      return true;
    }
    return false;
  }

  static bool parseFormat(const char *text, uint8_t *format) {
    for (uint8_t f = MB_8N1; f <= MB_8O1; ++f) {
      if (is(text, mbFormatName(f))) {
        *format = f;
        return true;
      }
    }
    return false;
  }

  // Two hex digits, with or without 0x.
  static bool parseHexByte(const char *text, uint8_t *out) {
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
      text += 2;
    }
    if (text[0] == '\0' || strlen(text) > 2) {
      return false;
    }
    unsigned value = 0;
    for (; *text != '\0'; ++text) {
      const char c = *text;
      unsigned digit;
      if (c >= '0' && c <= '9') {
        digit = (unsigned)(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = (unsigned)(c - 'a') + 10u;
      } else if (c >= 'A' && c <= 'F') {
        digit = (unsigned)(c - 'A') + 10u;
      } else {
        return false;
      }
      value = value * 16u + digit;
    }
    *out = (uint8_t)value;
    return true;
  }

  bool stopRequested() {
    if (!stopped_ && host_.inputPending()) {
      stopped_ = true;
      host_.discardInput();
    }
    return stopped_;
  }

  void describe(const MbResult &result) {
    char text[64];
    ovDescribeResult(result, text, sizeof(text));
    out_.print(text);
  }

  void describe(const OvWordStatus &status) {
    char text[64];
    ovDescribeStatus(status, text, sizeof(text));
    out_.print(text);
  }

  // Something the user typed, in quotes, cut short when it is long.
  void printQuoted(const char *text) {
    static const size_t kMost = 32;
    const size_t len = strlen(text);
    out_.printChar('\'');
    out_.write(text, len < kMost ? len : kMost);
    if (len > kMost) {
      out_.print("...");
    }
    out_.printChar('\'');
  }

  // A number without trailing zeros: 100, 96.667, 989.5
  void printNumber(double value) {
    char text[40];
    size_t n = ovFormatFixed(value, 4, text, sizeof(text));
    if (strchr(text, '.') != nullptr) {
      while (n > 0 && text[n - 1] == '0') {
        text[--n] = '\0';
      }
      if (n > 0 && text[n - 1] == '.') {
        text[--n] = '\0';
      }
    }
    out_.print(text);
  }

  // text, then spaces up to `width` columns, and never fewer than two.
  void printColumn(const char *text, size_t width) {
    const size_t len = strlen(text);
    out_.print(text);
    out_.spaces(len + 2 <= width ? width - len : 2);
  }

  // Free text, wrapped at OV_WRAP columns. `first` starts the first line, `rest` the others.
  void printWrapped(const char *first, const char *rest, const char *text) {
    out_.print(first);
    size_t column = strlen(first);
    bool lineStart = true;
    const char *p = text;
    while (*p == ' ') {
      ++p;
    }
    while (*p != '\0') {
      const char *end = p;
      while (*end != '\0' && *end != ' ') {
        ++end;
      }
      const size_t len = (size_t)(end - p);
      if (!lineStart && column + 1 + len > OV_WRAP) {
        out_.println();
        out_.print(rest);
        column = strlen(rest);
        lineStart = true;
      }
      if (!lineStart) {
        out_.printChar(' ');
        ++column;
      }
      out_.write(p, len);
      column += len;
      lineStart = false;
      p = end;
      while (*p == ' ') {
        ++p;
      }
    }
    out_.println();
  }

  // "x100/32768"
  void printScale(double num, double den) {
    out_.printChar('x');
    printNumber(num);
    out_.printChar('/');
    printNumber(den);
  }

  // "vset = x100/32768, not confirmed on this controller"
  void printScaleState(const OvScale &scale, const ScaleState &state) {
    out_.print(scale.name);
    out_.print(" = ");
    printScale(scale.num, scale.den);
    if (scaleConfirmed(state, scale)) {
      out_.println(", confirmed by 'scalecheck'");
    } else if (state.found > 0.0) {
      out_.print(", but 'scalecheck' found x");
      printNumber(state.found);
      out_.println(" on this controller");
    } else {
      out_.println(", not confirmed on this controller");
    }
  }

  // A value in the register's own units: "13.10 V"
  void printQuantity(const OvRegister &reg, double value) {
    out_.printFixed(value, reg.decimals);
    if (reg.units[0] != '\0') {
      out_.printChar(' ');
      out_.print(reg.units);
    }
  }

  void printLink() {
    out_.print("slave ");
    out_.printU(device_.link.slave);
    out_.print(", ");
    out_.printU(device_.link.baud);
    out_.printChar(' ');
    out_.print(mbFormatName(device_.link.format));
    out_.print(", read function ");
    out_.printU(device_.link.readFunction);
  }

  // "[0x1183]" or "[0x0000 0x1234]"
  void printRawWords(const uint16_t *words, uint8_t count) {
    out_.printChar('[');
    for (uint8_t i = 0; i < count; ++i) {
      if (i > 0) {
        out_.printChar(' ');
      }
      out_.print("0x");
      out_.printHex(words[i], 4);
    }
    out_.printChar(']');
  }

  void printExchange(const uint8_t *tx, size_t txLen, const uint8_t *rx, size_t rxLen, const MbResult &result) {
    out_.print("    > ");
    out_.printHexBytes(tx, txLen);
    out_.println();
    out_.print("    < ");
    if (rxLen == 0) {
      out_.print("(nothing)");
    } else {
      out_.printHexBytes(rx, rxLen);
    }
    out_.print("  (");
    describe(result);
    out_.print(", ");
    out_.printU(result.elapsedMs);
    out_.println(" ms)");
  }

  // One register on one line:
  //   0xE022 W~ EV_lvd                  11.50 V           Low voltage disconnect  [0x0EB8]
  void printRow(const OvRegister &reg, const uint16_t *words, const OvWordStatus &status) {
    out_.print("  0x");
    out_.printHex(reg.addr, 4);
    out_.printChar(' ');
    out_.printChar(reg.writable ? 'W' : ' ');
    out_.printChar(reg.bench ? ' ' : '~');
    out_.printChar(' ');
    printColumn(reg.name, 24);
    if (status.status != MB_OK) {
      out_.print("-- ");
      describe(status);
      out_.println();
      return;
    }
    char text[OV_VALUE_TEXT];
    ovFormatValue(reg, words, text, sizeof(text));
    printColumn(text, 18);
    out_.print(reg.label);
    out_.print("  ");
    printRawWords(words, reg.words);
    out_.println();
    if (reg.scale != nullptr) {
      const OvScaleCheck &check = device_.profile().scaleCheck();
      settingsScale_.shown = settingsScale_.shown || reg.scale == check.settingsScale;
      liveScale_.shown = liveScale_.shown || reg.scale == check.liveScale;
    }
  }

  // Reads and prints the registers of one group. Returns how many it printed.
  size_t printGroup(const OvGroup &group) {
    OvProfile &profile = device_.profile();
    size_t cursor = 0;
    size_t printed = 0;
    OvSpan span;
    uint16_t words[OV_MAX_SPAN_WORDS];
    OvWordStatus status[OV_MAX_SPAN_WORDS];
    while (device_.nextSpan(group.name, &cursor, &span)) {
      if (stopRequested()) {
        break;
      }
      if (printed == 0) {
        out_.println(group.title);
      }
      device_.readSpan(span.addr, span.words, words, status);
      uint16_t offset = 0;
      for (size_t k = 0; k < span.count; ++k) {
        const OvRegister &reg = profile.reg(span.first + k);
        printRow(reg, words + offset, OvDevice::combine(status + offset, reg.words));
        offset = (uint16_t)(offset + reg.words);
        ++printed;
      }
    }
    return printed;
  }

  void afterReads() {
    if (stopped_) {
      out_.println("Stopped.");
    }
    if (device_.linkDown()) {
      out_.println("The controller is not answering, so the rest was not read. 'probe' checks the link.");
    }
    if (device_.blocksFailed() && !blocksNoticeShown_) {
      blocksNoticeShown_ = true;
      out_.println("Note: block reads do not work on this link, so registers are now read one at a time.");
      out_.println("      'blocks auto' tries blocks again.");
    }
  }

  // A scale is confirmed while it has the numerator that 'scalecheck' found on this controller.
  // Changing it by hand to anything else makes it unconfirmed again.
  static bool scaleConfirmed(const ScaleState &state, const OvScale &scale) {
    const double gap = scale.num - state.found;
    return state.found > 0.0 && gap < 0.001 && gap > -0.001;
  }
  // "Open": the profile wants this scale tested, and it is not confirmed.
  bool settingsScaleOpen() const {
    const OvScale *scale = device_.profile().scaleCheck().settingsScale;
    return scale != nullptr && !scaleConfirmed(settingsScale_, *scale);
  }
  bool liveScaleOpen() const {
    const OvScale *scale = device_.profile().scaleCheck().liveScale;
    return scale != nullptr && !scaleConfirmed(liveScale_, *scale);
  }

  // After a command that printed voltages on a scale that nothing has confirmed: says so, once
  // for each scale. 'scalecheck' brings the reminder back when it finds a scale in use wrong.
  void scaleNotice() {
    const OvScaleCheck &check = device_.profile().scaleCheck();
    const bool live = liveScaleOpen() && liveScale_.shown && !liveScale_.noticed;
    const bool settings = settingsScaleOpen() && settingsScale_.shown && !settingsScale_.noticed;
    if ((!live && !settings) || device_.linkDown()) {
      return;
    }
    liveScale_.noticed = liveScale_.noticed || live;
    settingsScale_.noticed = settingsScale_.noticed || settings;
    const bool both = live && settings;
    out_.println(both ? "Note: voltages are shown with scales that are not confirmed on this controller"
                      : "Note: voltages are shown with a scale that is not confirmed on this controller");
    out_.print("      (");
    if (live) {
      out_.print("live readings ");
      printScale(check.liveScale->num, check.liveScale->den);
    }
    if (both) {
      out_.print(", ");
    }
    if (settings) {
      out_.print("stored settings ");
      printScale(check.settingsScale->num, check.settingsScale->den);
    }
    out_.println(both ? "). 'scalecheck' tests them." : "). 'scalecheck' tests it.");
  }

  void printWiring() {
    OvProfile &profile = device_.profile();
    for (size_t i = 0; i < profile.wiringCount(); ++i) {
      out_.println(profile.wiring(i));
    }
    if (profile.wiringCount() == 0) {
      out_.println("This profile has no wiring notes.");
    }
  }

  void printIdentification() {
    MbDeviceId id;
    const MbResult result = device_.master().readDeviceId(device_.link.slave, id);
    if (result.status != MB_OK) {
      out_.print("Identification: not available (");
      describe(result);
      out_.println(").");
      return;
    }
    out_.print("Identification:");
    for (uint8_t i = 0; i < id.count; ++i) {
      out_.print(i == 0 ? " " : ", ");
      switch (id.objects[i].id) {
        case 0: out_.print("vendor"); break;
        case 1: out_.print("product"); break;
        case 2: out_.print("version"); break;
        default:
          out_.print("object ");
          out_.printU(id.objects[i].id);
          break;
      }
      out_.print(" \"");
      out_.print(id.objects[i].text);
      out_.print("\"");
    }
    out_.println();
  }

  const OvRegister *findRegisterArg(const char *text) {
    OvProfile &profile = device_.profile();
    uint32_t addr = 0;
    if (ovParseU32(text, &addr)) {
      return addr <= 0xFFFF ? profile.findRegisterAt((uint16_t)addr) : nullptr;
    }
    return profile.findRegister(text);
  }

  void printLimits(const OvRegister &reg) {
    if (reg.hasMin && reg.hasMax) {
      out_.print("from ");
      printQuantity(reg, reg.minValue);
      out_.print(" to ");
      printQuantity(reg, reg.maxValue);
    } else if (reg.hasMin) {
      out_.print("at least ");
      printQuantity(reg, reg.minValue);
    } else if (reg.hasMax) {
      out_.print("at most ");
      printQuantity(reg, reg.maxValue);
    } else {
      out_.print("anything the register can hold");
    }
    if (reg.zeroOk) {
      out_.print(", or 0 to switch it off");
    }
  }

  // -------------------------------------------------------------------------------------------
  // Commands: finding the controller
  // -------------------------------------------------------------------------------------------

  void cmdHelp() {
    static const char *const kLines[] = {
        "Finding the controller",
        "  probe                   try the usual link settings until the controller answers",
        "  scan [first] [last]     look for slave addresses (1 to 16 unless told otherwise)",
        "  id                      ask the controller to identify itself",
        "  wires                   wiring and switch checklist",
        "Reading",
        "  live                    meters and status",
        "  watch [seconds]         repeat 'live' until a key is pressed (every 5 s)",
        "  dump [group]            every register, or one group ('groups' lists them)",
        "  get <name> | get <address> [count]",
        "                          one register by name, or raw registers from an address",
        "  coils                   coil states",
        "  snapshot                every raw value as JSON, to keep",
        "  scalecheck [volts]      test the voltage scales; add a multimeter reading if you have one",
        "Changing settings (each one asks for 'yes' first)",
        "  set <name> <value>      write a setting, for example: set EV_lvd 13.10",
        "  set <name> raw <n>      the same with a raw register value",
        "  coil <name> on|off      switch a coil",
        "  reset                   restart the controller so that new settings apply",
        "Link",
        "  status                  link settings and counters",
        "  link <baud> [format]    for example: link 9600 8N2",
        "  slave <n>, fc <3|4>, timeout <ms>, blocks <auto|on|off>",
        "  scale [name] [x]        show the scales, or change one until the next restart",
        "  profile                 where the register map comes from",
        "Diagnostics",
        "  trace on|off            show every request and reply in hex",
        "  sniff [ms]              listen without sending (5000 ms)",
        "  raw <slave> <fc> <address> <count|value>",
        "                          one request; function 5, 6 and 16 write and need 'expert on'",
        "  expert on|off",
        "In the lists, W marks a setting that 'set' can change and ~ a register taken from the",
        "specification that has not been confirmed on our bench."};
    for (size_t i = 0; i < sizeof(kLines) / sizeof(kLines[0]); ++i) {
      out_.println(kLines[i]);
    }
  }

  void cmdStatus() {
    OvProfile &profile = device_.profile();
    ModbusMaster &master = device_.master();
    out_.print("OptaView  ");
    out_.println(OV_VERSION);
    out_.print("Profile   ");
    out_.print(profile.title());
    out_.print(", ");
    out_.print(profile.version());
    out_.print(" (");
    out_.printU((uint32_t)profile.registerCount());
    out_.print(" registers, ");
    out_.printU((uint32_t)profile.coilCount());
    out_.println(" coils)");
    out_.print("Link      ");
    printLink();
    out_.print(", timeout ");
    out_.printU(master.responseTimeoutMs);
    out_.println(" ms");
    out_.print("Reads     blocks ");
    out_.print(ovBlockModeName(device_.blockMode));
    if (device_.blockMode == OV_BLOCKS_AUTO && device_.blocksFailed()) {
      out_.println(": gave up on blocks, reading one register at a time");
    } else if (device_.blocksInUse()) {
      out_.print(": up to ");
      out_.printU(device_.maxBlock());
      out_.println(" registers in a request");
    } else {
      out_.println(": one register in a request");
    }
    out_.print("Counters  sent ");
    out_.printU(master.stats.sent);
    out_.print(", ok ");
    out_.printU(master.stats.ok);
    out_.print(", no reply ");
    out_.printU(master.stats.timeouts);
    out_.print(", CRC errors ");
    out_.printU(master.stats.crcErrors);
    out_.print(", exceptions ");
    out_.printU(master.stats.exceptions);
    out_.print(", other errors ");
    out_.printU(master.stats.otherErrors);
    out_.println();
    const OvScaleCheck &check = profile.scaleCheck();
    if (check.settingsScale != nullptr) {
      out_.print("Voltage   stored settings: scale ");
      printScaleState(*check.settingsScale, settingsScale_);
    }
    if (check.liveScale != nullptr) {
      out_.print(check.settingsScale != nullptr ? "          live readings:   scale "
                                                : "Voltage   live readings: scale ");
      printScaleState(*check.liveScale, liveScale_);
    }
    if (settingsScaleOpen() || liveScaleOpen()) {
      out_.println("          'scalecheck' tests what is not confirmed.");
    }
    out_.print("Modes     expert ");
    out_.print(expert_ ? "on" : "off");
    out_.print(", trace ");
    out_.println(trace_ ? "on" : "off");
    if (resetPending_) {
      out_.println("Settings were changed in this session. 'reset' makes the controller use them.");
    }
  }

  void cmdProbe() {
    OvProfile &profile = device_.profile();
    const OvRegister *reg = profile.modbus().probeRegister;
    out_.print("Looking for ");
    out_.print(profile.title());
    out_.println(".");
    out_.print("Each try reads ");
    out_.print(reg->name);
    out_.print(" (0x");
    out_.printHex(reg->addr, 4);
    out_.println("). Press a key to stop.");
    const OvProbeOutcome outcome = device_.probe(this);
    blocksNoticeShown_ = false;
    if (outcome.found) {
      char text[OV_VALUE_TEXT];
      const uint16_t words[2] = {outcome.raw, 0};
      ovFormatValue(*reg, words, text, sizeof(text));
      out_.print("Found it: ");
      printLink();
      out_.println(".");
      out_.print(reg->label);
      out_.print(" reads ");
      out_.print(text);
      out_.println(".");
      printIdentification();
      out_.println("These settings are now in use. Try 'live'.");
      return;
    }
    if (outcome.aborted) {
      out_.println("Stopped.");
      return;
    }
    if (outcome.refused) {
      out_.print("A controller answers at slave ");
      out_.printU(outcome.link.slave);
      out_.print(", ");
      out_.printU(outcome.link.baud);
      out_.printChar(' ');
      out_.print(mbFormatName(outcome.link.format));
      out_.println(", but it refuses to read that register:");
      out_.print("exception ");
      out_.printU(outcome.exception);
      out_.print(" (");
      out_.print(mbExceptionText(outcome.exception));
      out_.println(").");
      out_.println("The link works. This register map may not be the right one for that controller.");
      return;
    }
    if (outcome.sawBytes) {
      out_.println("Bytes arrived, but never a valid reply. That points at the wrong baud rate or format,");
      out_.println("at A and B the wrong way round, or at another master on the bus ('sniff' shows it).");
    } else {
      out_.println("Nothing answered.");
    }
    printWiring();
  }

  void cmdScan(int argc, char **argv) {
    uint32_t first = 1;
    uint32_t last = 16;
    if (argc > 1) {
      if (!ovParseU32(argv[1], &first) || first < 1 || first > 247) {
        out_.println("Slave addresses run from 1 to 247. Usage: scan [first] [last]");
        return;
      }
      last = first;
    }
    if (argc > 2 && (!ovParseU32(argv[2], &last) || last < first || last > 247)) {
      out_.println("Slave addresses run from 1 to 247. Usage: scan [first] [last]");
      return;
    }
    ModbusMaster &master = device_.master();
    const OvRegister *reg = device_.profile().modbus().probeRegister;
    out_.print("Asking slaves ");
    out_.printU(first);
    out_.print(" to ");
    out_.printU(last);
    out_.print(" at ");
    out_.printU(device_.link.baud);
    out_.printChar(' ');
    out_.print(mbFormatName(device_.link.format));
    out_.println(". Press a key to stop.");
    const uint16_t savedTimeout = master.responseTimeoutMs;
    if (master.responseTimeoutMs > OV_PROBE_TIMEOUT_MS) {
      master.responseTimeoutMs = OV_PROBE_TIMEOUT_MS;
    }
    uint32_t answered = 0;
    for (uint32_t slave = first; slave <= last && !stopRequested(); ++slave) {
      uint16_t raw = 0;
      const MbResult result = master.readRegisters((uint8_t)slave, device_.link.readFunction, reg->addr, 1, &raw);
      if (result.status == MB_TIMEOUT) {
        continue;
      }
      out_.print("  slave ");
      out_.printU(slave);
      out_.print(": ");
      if (result.status == MB_OK) {
        out_.println("answers");
        ++answered;
      } else if (result.status == MB_EXCEPTION) {
        out_.print("answers, but refuses that register: ");
        describe(result);
        out_.println();
        ++answered;
      } else {
        describe(result);
        out_.println();
      }
    }
    master.responseTimeoutMs = savedTimeout;
    if (stopped_) {
      out_.println("Stopped.");
    }
    if (answered == 0) {
      out_.println("No slave answered.");
    } else {
      out_.printU(answered);
      out_.println(answered == 1 ? " slave answered. 'slave <n>' selects it."
                                 : " slaves answered. 'slave <n>' selects one.");
    }
  }

  // -------------------------------------------------------------------------------------------
  // Commands: reading
  // -------------------------------------------------------------------------------------------

  void printLiveGroups() {
    OvProfile &profile = device_.profile();
    for (size_t i = 0; i < profile.liveGroupCount() && !stopped_ && !device_.linkDown(); ++i) {
      const OvGroup *group = profile.findGroup(profile.liveGroup(i));
      if (group != nullptr) {
        printGroup(*group);
      }
    }
  }

  void cmdLive() {
    if (device_.profile().liveGroupCount() == 0) {
      out_.println("This profile names no live groups. Use 'dump'.");
      return;
    }
    printLiveGroups();
    afterReads();
    scaleNotice();
  }

  void cmdWatch(int argc, char **argv) {
    uint32_t seconds = 5;
    if (argc > 1 && (!ovParseU32(argv[1], &seconds) || seconds < 1 || seconds > 3600)) {
      out_.println("Usage: watch [seconds], from 1 to 3600");
      return;
    }
    if (device_.profile().liveGroupCount() == 0) {
      out_.println("This profile names no live groups. Use 'dump'.");
      return;
    }
    MbTransport &transport = device_.master().transport();
    out_.println("Press a key to stop.");
    for (;;) {
      device_.beginBatch();
      printLiveGroups();
      if (device_.linkDown()) {
        out_.println("  (no answer; trying again)");
      }
      const uint32_t start = transport.nowMs();
      while (!stopRequested() && (uint32_t)(transport.nowMs() - start) < seconds * 1000UL) {
        transport.idle();
      }
      if (stopped_) {
        break;
      }
      out_.println();
    }
    out_.println("Stopped.");
    scaleNotice();
  }

  void cmdDump(int argc, char **argv) {
    OvProfile &profile = device_.profile();
    if (argc > 1) {
      const OvGroup *group = profile.findGroup(argv[1]);
      if (group == nullptr) {
        out_.print("There is no group called ");
        printQuoted(argv[1]);
        out_.println(". 'groups' lists them.");
        return;
      }
      if (printGroup(*group) == 0 && !stopped_) {
        out_.println("That group has no registers.");
      }
    } else {
      for (size_t i = 0; i < profile.groupCount() && !stopped_ && !device_.linkDown(); ++i) {
        printGroup(profile.group(i));
      }
      out_.println("W = 'set' can change it.  ~ = from the specification, not confirmed on our bench.");
    }
    afterReads();
    scaleNotice();
  }

  void cmdGroups() {
    OvProfile &profile = device_.profile();
    for (size_t i = 0; i < profile.groupCount(); ++i) {
      const OvGroup &group = profile.group(i);
      uint32_t count = 0;
      for (size_t r = 0; r < profile.registerCount(); ++r) {
        if (strcmp(profile.reg(r).group, group.name) == 0) {
          ++count;
        }
      }
      out_.print("  ");
      out_.printPadded(group.name, 12);
      out_.print(group.title);
      out_.print(" (");
      out_.printU(count);
      out_.println(count == 1 ? " register)" : " registers)");
    }
  }

  void cmdGet(int argc, char **argv) {
    OvProfile &profile = device_.profile();
    if (argc < 2) {
      out_.println("Usage: get <name>   or   get <address> [count]");
      return;
    }
    uint32_t addr = 0;
    if (!ovParseU32(argv[1], &addr)) {
      const OvRegister *reg = profile.findRegister(argv[1]);
      if (reg == nullptr) {
        out_.print("There is no register called ");
        printQuoted(argv[1]);
        out_.println(". 'dump' lists them all.");
        return;
      }
      uint16_t words[2] = {0, 0};
      OvWordStatus status;
      device_.readRegister(*reg, words, &status);
      printRow(*reg, words, status);
      if (reg->writable) {
        out_.print("    'set' accepts ");
        printLimits(*reg);
        out_.println(".");
      }
      if (reg->note[0] != '\0') {
        printWrapped("    ", "    ", reg->note);
      }
      afterReads();
      scaleNotice();
      return;
    }
    uint32_t count = 1;
    if (argc > 2 && (!ovParseU32(argv[2], &count) || count < 1 || count > OV_MAX_SPAN_WORDS)) {
      out_.println("The count runs from 1 to 125.");
      return;
    }
    if (addr > 0xFFFF || addr + count > 0x10000UL) {
      out_.println("Addresses run from 0 to 0xFFFF.");
      return;
    }
    uint16_t words[OV_MAX_SPAN_WORDS];
    OvWordStatus status[OV_MAX_SPAN_WORDS];
    device_.readSpan((uint16_t)addr, (uint16_t)count, words, status);
    for (uint32_t i = 0; i < count; ++i) {
      out_.print("  0x");
      out_.printHex(addr + i, 4);
      out_.print("  ");
      if (status[i].status != MB_OK) {
        out_.print("-- ");
        describe(status[i]);
        out_.println();
        continue;
      }
      out_.print("0x");
      out_.printHex(words[i], 4);
      out_.print("  ");
      char number[12];
      ovFormatU64(words[i], number, sizeof(number));
      out_.printPadded(number, 7);
      // What the profile makes of it, when a register starts here and all of it was read.
      const OvRegister *reg = profile.findRegisterAt((uint16_t)(addr + i));
      if (reg != nullptr && i + reg->words <= count && OvDevice::combine(status + i, reg->words).status == MB_OK) {
        char text[OV_VALUE_TEXT];
        ovFormatValue(*reg, words + i, text, sizeof(text));
        out_.print(reg->name);
        out_.print(" = ");
        out_.print(text);
      }
      out_.println();
    }
    afterReads();
  }

  void cmdCoils() {
    OvProfile &profile = device_.profile();
    if (profile.coilCount() == 0) {
      out_.println("This profile has no coils.");
      return;
    }
    for (size_t i = 0; i < profile.coilCount() && !stopRequested(); ++i) {
      const OvCoil &coil = profile.coil(i);
      out_.print("  0x");
      out_.printHex(coil.addr, 4);
      out_.print("  ");
      out_.printPadded(coil.name, 24);
      if (coil.trigger) {
        out_.printPadded("(action)", 10);
      } else {
        bool on = false;
        const MbResult result = device_.readCoil(coil, &on);
        if (result.status != MB_OK) {
          out_.print("-- ");
          describe(result);
          out_.println();
          continue;
        }
        out_.printPadded(on ? "on" : "off", 10);
      }
      out_.println(coil.label);
    }
    if (stopped_) {
      out_.println("Stopped.");
    }
    out_.println("Coils come from the specification and have not been tried on our bench.");
  }

  void printJsonString(const char *text) {
    out_.printChar('"');
    for (const char *p = text; *p != '\0'; ++p) {
      const unsigned char c = (unsigned char)*p;
      if (c == '"' || c == '\\') {
        out_.printChar('\\');
        out_.printChar((char)c);
      } else if (c < 0x20) {
        out_.print("\\u00");
        out_.printHex(c, 2);
      } else {
        out_.printChar((char)c);
      }
    }
    out_.printChar('"');
  }

  void cmdSnapshot() {
    OvProfile &profile = device_.profile();
    out_.println("--- snapshot begins ---");
    out_.println("{");
    out_.print("  \"optaview\": ");
    printJsonString(OV_VERSION);
    out_.println(",");
    out_.print("  \"profile\": ");
    printJsonString(profile.id());
    out_.println(",");
    out_.print("  \"profileVersion\": ");
    printJsonString(profile.version());
    out_.println(",");
    out_.print("  \"link\": {\"slave\": ");
    out_.printU(device_.link.slave);
    out_.print(", \"baud\": ");
    out_.printU(device_.link.baud);
    out_.print(", \"format\": \"");
    out_.print(mbFormatName(device_.link.format));
    out_.print("\", \"readFunction\": ");
    out_.printU(device_.link.readFunction);
    out_.println("},");
    out_.println("  \"registers\": [");
    size_t cursor = 0;
    OvSpan span;
    uint16_t words[OV_MAX_SPAN_WORDS];
    OvWordStatus status[OV_MAX_SPAN_WORDS];
    bool first = true;
    uint32_t missing = 0;
    while (device_.nextSpan(nullptr, &cursor, &span)) {
      if (stopRequested()) {
        // Count what is left as missing.
        missing += (uint32_t)(profile.registerCount() - span.first);
        break;
      }
      device_.readSpan(span.addr, span.words, words, status);
      uint16_t offset = 0;
      for (size_t k = 0; k < span.count; ++k) {
        const OvRegister &reg = profile.reg(span.first + k);
        const OvWordStatus regStatus = OvDevice::combine(status + offset, reg.words);
        if (!first) {
          out_.println(",");
        }
        first = false;
        out_.print("    {\"addr\": \"0x");
        out_.printHex(reg.addr, 4);
        out_.print("\", \"name\": ");
        printJsonString(reg.name);
        if (regStatus.status == MB_OK) {
          char text[OV_VALUE_TEXT];
          ovFormatValue(reg, words + offset, text, sizeof(text));
          out_.print(", \"raw\": [");
          for (uint8_t w = 0; w < reg.words; ++w) {
            if (w > 0) {
              out_.print(", ");
            }
            out_.printU(words[offset + w]);
          }
          out_.print("], \"text\": ");
          printJsonString(text);
        } else {
          char text[64];
          ovDescribeStatus(regStatus, text, sizeof(text));
          out_.print(", \"error\": ");
          printJsonString(text);
          ++missing;
        }
        out_.print("}");
        offset = (uint16_t)(offset + reg.words);
      }
    }
    out_.println();
    out_.println("  ],");
    out_.print("  \"missing\": ");
    out_.printU(missing);
    out_.println();
    out_.println("}");
    out_.println("--- snapshot ends ---");
    if (missing > 0) {
      out_.printU(missing);
      out_.println(missing == 1 ? " register could not be read; see its \"error\"."
                                : " registers could not be read; see their \"error\".");
    }
    afterReads();
  }

  void printCandidate(double num) {
    out_.printChar('x');
    printNumber(num);
  }

  // "Stored settings (scale vset, now x100/32768). Candidates: x100, x96.667."
  void printScaleHeading(const char *what, const OvScale &scale, const OvScaleCandidates &candidates) {
    out_.print(what);
    out_.print(" (scale ");
    out_.print(scale.name);
    out_.print(", now ");
    printScale(scale.num, scale.den);
    out_.print("). Candidates:");
    for (uint8_t c = 0; c < candidates.count; ++c) {
      out_.print(c == 0 ? " " : ", ");
      printCandidate(candidates.num[c]);
    }
    out_.println(".");
  }

  // "       Vb_f reads 13.70 V at x100, 13.24 V at x96.667"
  void printReadings(const OvRegister &reg, const double *volts, const OvScaleCandidates &candidates) {
    out_.print("       ");
    out_.print(reg.name);
    out_.print(" reads");
    for (uint8_t c = 0; c < candidates.count; ++c) {
      out_.print(c == 0 ? " " : ", ");
      out_.printFixed(volts[c], 2);
      out_.print(" V at ");
      printCandidate(candidates.num[c]);
    }
    out_.println();
  }

  // What one test says: "     -> x100", or why it cannot tell.
  void printTestResult(const OvScaleCandidates &candidates, int8_t verdict, const char *otherwise) {
    out_.print("     -> ");
    if (verdict < 0) {
      out_.println(otherwise);
    } else {
      printCandidate(candidates.num[verdict]);
      out_.println();
    }
  }

  void printSettingsTests(const OvScaleCheck &check, const OvSettingsScaleReport &report) {
    printScaleHeading("Stored settings", *check.settingsScale, report.candidates);
    out_.print("  1. Setpoints: ");
    out_.printU(report.setpointsUsed);
    out_.print(" in use (");
    out_.printU(report.setpointsRead);
    out_.print(" of ");
    out_.printU(check.setpointCount);
    out_.println(" read). Round settings only look round at the right scale.");
    for (uint8_t c = 0; c < report.candidates.count; ++c) {
      out_.print("       at ");
      printCandidate(report.candidates.num[c]);
      out_.print(", ");
      out_.printU(report.fits[c]);
      out_.print(" of ");
      out_.printU(report.setpointsUsed);
      out_.print(report.fits[c] == 1 ? " is a multiple of " : " are multiples of ");
      out_.printFixed(check.grid, 2);
      out_.println(" V");
    }
    printTestResult(report.candidates, report.verdict, "too few round setpoints to tell");
  }

  void printLiveTests(const OvScaleCheck &check, const OvLiveScaleReport &report) {
    printScaleHeading("Live readings", *check.liveScale, report.candidates);

    out_.print("  2. Regulation: ");
    if (check.battery == nullptr || check.reference == nullptr || check.state == nullptr) {
      out_.println("not set up in this profile.");
    } else if (!report.regulationRead) {
      out_.println("the registers could not be read.");
    } else if (!report.regulating) {
      char text[OV_VALUE_TEXT];
      const uint16_t words[2] = {report.stateRaw, 0};
      ovFormatValue(*check.state, words, text, sizeof(text));
      out_.print("the controller is in ");
      out_.print(text);
      out_.println(", not holding the battery at its target.");
      out_.println("     -> cannot tell now; try again in absorption or float");
    } else if (report.batteryRaw == 0 || report.referenceRaw == 0) {
      out_.println("the battery or the target register reads 0.");
      out_.println("     -> cannot tell");
    } else {
      out_.print("the controller is holding the battery at its target (");
      out_.print(check.reference->name);
      out_.print(" = ");
      out_.printFixed(report.referenceVolts, 2);
      out_.println(" V).");
      printReadings(*check.battery, report.batteryVolts, report.candidates);
      printTestResult(report.candidates, report.regulationVerdict,
                      report.regulationMatches == 0 ? "no candidate matches the target"
                                                    : "more than one candidate matches the target");
      if (report.regulationVerdict > 0) {
        out_.print("       (That holds only if the target's own scale, ");
        out_.print(check.reference->scale->name);
        out_.println(", is right. A multimeter reading tells.)");
      }
    }

    out_.print("  3. Multimeter: ");
    if (check.meter == nullptr) {
      out_.println("not set up in this profile.");
    } else if (!report.meterGiven) {
      out_.println("no reading given.");
      out_.println("     -> measure at the controller's battery terminals, then: scalecheck <volts>");
    } else if (!report.meterRead) {
      out_.println("the register could not be read.");
    } else if (!report.meterCompared) {
      out_.print(check.meter->name);
      out_.println(" reads 0, so there is nothing to compare the reading with.");
    } else {
      out_.printFixed(report.meterGivenVolts, 2);
      out_.print(" V measured, which with the register value works out to x");
      out_.printFixed(report.impliedNumerator, 2);
      out_.println(".");
      printReadings(*check.meter, report.meterVolts, report.candidates);
      printTestResult(report.candidates, report.meterVerdict,
                      report.meterMatches == 0 ? "no candidate is within 1.2 % of the multimeter"
                                               : "more than one candidate is within 1.2 % of the multimeter");
    }
  }

  // "  Stored settings: x100, which is what OptaView uses now.", or how to switch to the scale
  // that was found. `fileNum` is the numerator in the profile file, which OptaView starts with.
  void printFoundScale(const char *label, const OvScale &scale, double fileNum, const OvScaleCandidates &candidates,
                       int8_t verdict) {
    const double found = candidates.num[verdict];
    const double gap = found - fileNum;
    const bool fileHasIt = gap < 0.001 && gap > -0.001;
    out_.print(label);
    printCandidate(found);
    if (verdict == 0) {
      out_.println(", which is what OptaView uses now.");
      if (!fileHasIt) {
        out_.print("     The profile file still has ");
        printCandidate(fileNum);
        out_.println(": correct it there, or OptaView starts with that again.");
      }
      return;
    }
    out_.print(", not the ");
    printCandidate(candidates.num[0]);
    out_.println(" that OptaView uses now.");
    out_.print("     'scale ");
    out_.print(scale.name);
    out_.printChar(' ');
    printNumber(found);
    out_.println(fileHasIt ? "' switches it back to the profile's figure."
                           : "' switches it until OptaView restarts. Correct the profile file too.");
  }

  // The multimeter and the regulation test point at different candidates. If the multimeter is
  // right, the battery register is right too, and it is the target that is read with a wrong
  // scale. While the controller regulates, the target equals the battery voltage, which says
  // what the target's scale would have to be.
  void printTargetScaleHint(const OvScaleCheck &check, const OvLiveScaleReport &live) {
    const OvScale &target = *check.reference->scale;
    const double batteryVolts = live.batteryVolts[live.verdict];
    double wanted = batteryVolts * target.den / (double)live.referenceRaw;
    bool exact = false;
    for (uint8_t c = 0; c < live.candidates.count; ++c) {
      const double gap = live.candidates.num[c] - wanted;
      if (gap <= wanted * 0.012 && gap >= -wanted * 0.012) {
        wanted = live.candidates.num[c];  // a known figure, not a number worked out from one reading
        exact = true;
        break;
      }
    }
    out_.print("     The regulation test disagrees. If the multimeter is right, the target's own scale (");
    out_.print(target.name);
    out_.println(")");
    out_.print("     is the one that is off: 'scale ");
    out_.print(target.name);
    out_.printChar(' ');
    if (exact) {
      printNumber(wanted);
    } else {
      out_.printFixed(wanted, 2);
    }
    out_.print("' would put ");
    out_.print(check.reference->name);
    out_.println(" at the battery voltage.");
  }

  // Keeps what a test found. That either confirms the scale in use or shows it to be the wrong
  // one; then the reminder after reads comes back until the user switches the scale.
  void takeVerdict(ScaleState *state, const OvScaleCandidates &candidates, int8_t verdict) {
    state->found = candidates.num[verdict];
    if (verdict > 0) {
      state->noticed = false;
    }
  }

  void cmdScaleCheck(int argc, char **argv) {
    OvProfile &profile = device_.profile();
    const OvScaleCheck &check = profile.scaleCheck();
    if (!check.present) {
      out_.println("This profile has no voltage scale check.");
      return;
    }
    double meterVolts = 0.0;
    if (argc > 1 && (!ovParseDecimal(argv[1], &meterVolts) || !(meterVolts > 0.0))) {
      out_.println("Usage: scalecheck [volts], for example: scalecheck 13.42");
      return;
    }
    OvScaleReport report;
    device_.scaleCheck(meterVolts, &report);
    if (device_.linkDown()) {
      afterReads();
      return;
    }
    const OvSettingsScaleReport &settings = report.settings;
    const OvLiveScaleReport &live = report.live;

    if (settings.tested) {
      printSettingsTests(check, settings);
    }
    if (live.tested) {
      printLiveTests(check, live);
    } else if (meterVolts > 0.0) {
      out_.println("This profile has no test for live readings, so the multimeter reading was not used.");
    }

    // Each scale gets its own verdict from its own tests. A multimeter does not decide how
    // settings are stored, and round setpoints do not decide what the readings mean.
    out_.println("Verdict");
    if (settings.tested) {
      static const char kLabel[] = "  Stored settings: ";
      if (settings.verdict >= 0) {
        printFoundScale(kLabel, *check.settingsScale, check.settingsFileNum, settings.candidates, settings.verdict);
        takeVerdict(&settingsScale_, settings.candidates, settings.verdict);
      } else {
        out_.print(kLabel);
        out_.println("cannot tell from these setpoints.");
        out_.println("     If you know what one of them was set to, "
                     "'get <name>' shows whether this scale reads it right.");
      }
    }
    if (live.tested) {
      static const char kLabel[] = "  Live readings:   ";
      if (live.verdict >= 0) {
        printFoundScale(kLabel, *check.liveScale, check.liveFileNum, live.candidates, live.verdict);
        takeVerdict(&liveScale_, live.candidates, live.verdict);
        if (live.conflict) {
          printTargetScaleHint(check, live);
        }
      } else if (live.meterCompared && live.meterMatches == 0) {
        out_.print(kLabel);
        out_.println("the multimeter fits none of the candidates, so this stays open.");
        out_.print("     If the reading is right, the scale is x");
        out_.printFixed(live.impliedNumerator, 2);
        out_.print(": 'scale ");
        out_.print(check.liveScale->name);
        out_.printChar(' ');
        out_.printFixed(live.impliedNumerator, 2);
        out_.println("' uses that.");
      } else if (live.meterCompared) {
        out_.print(kLabel);
        out_.println("the multimeter cannot separate candidates this close together.");
      } else {
        out_.print(kLabel);
        out_.println("cannot tell yet. A multimeter reading settles it: scalecheck <volts>");
      }
    }
    if (settings.verdict >= 0 && live.verdict >= 0) {
      const double gap = settings.candidates.num[settings.verdict] - live.candidates.num[live.verdict];
      if (gap > 0.001 || gap < -0.001) {
        out_.println("  The two differ on this controller. "
                     "Each has its own scale in the profile, so both can be right.");
      }
    }
  }

  void cmdScale(int argc, char **argv) {
    OvProfile &profile = device_.profile();
    if (argc == 1) {
      for (size_t i = 0; i < profile.scaleCount(); ++i) {
        const OvScale &scale = profile.scale(i);
        out_.print("  ");
        out_.printPadded(scale.name, 10);
        printScale(scale.num, scale.den);
        if (scale.units[0] != '\0') {
          out_.printChar(' ');
          out_.print(scale.units);
        }
        out_.println();
      }
      return;
    }
    OvScale *scale = profile.findScale(argv[1]);
    double num = 0.0;
    if (scale == nullptr) {
      out_.print("There is no scale called ");
      printQuoted(argv[1]);
      out_.println(". 'scale' lists them.");
      return;
    }
    if (argc < 3 || !ovParseDecimal(argv[2], &num) || !(num > 0.0)) {
      out_.println("Usage: scale <name> <x>, where x is the multiplier. 'scale' alone lists the names.");
      return;
    }
    scale->num = num;
    out_.print(scale->name);
    out_.print(" is now ");
    printScale(scale->num, scale->den);
    out_.println(" until OptaView restarts. The profile file is unchanged.");
    // One of the two tested scales, set to something that 'scalecheck' has not found on this
    // controller: the reminders about it start again.
    ScaleState *state = scale == profile.scaleCheck().settingsScale ? &settingsScale_
                        : scale == profile.scaleCheck().liveScale   ? &liveScale_
                                                                    : nullptr;
    if (state != nullptr && !scaleConfirmed(*state, *scale)) {
      state->noticed = false;
      out_.println("'scalecheck' has not confirmed that figure on this controller.");
    }
  }

  // -------------------------------------------------------------------------------------------
  // Commands: changing settings
  // -------------------------------------------------------------------------------------------

  void explainEncode(const OvRegister &reg, OvEncodeStatus status) {
    if (status == OV_ENCODE_OUT_OF_RANGE) {
      out_.println("That value does not fit the register.");
      return;
    }
    out_.print(status == OV_ENCODE_BELOW_MIN ? "That is below" : "That is above");
    out_.print(" what this profile allows for ");
    out_.print(reg.name);
    out_.print(": ");
    printLimits(reg);
    out_.println(".");
  }

  void printWriteNotes() {
    OvProfile &profile = device_.profile();
    if (profile.writeNoteCount() == 0) {
      return;
    }
    if (writeNotesShown_) {
      out_.println("  ('profile' repeats the notes on writing.)");
      return;
    }
    writeNotesShown_ = true;
    out_.println("  Before you write:");
    for (size_t i = 0; i < profile.writeNoteCount(); ++i) {
      printWrapped("  - ", "    ", profile.writeNote(i));
    }
  }

  // Warns when the value about to be written breaks a pair check.
  void previewChecks(const OvRegister &reg, uint16_t raw) {
    OvProfile &profile = device_.profile();
    for (size_t i = 0; i < profile.checkCount(); ++i) {
      const OvCheck &check = profile.check(i);
      if (check.below != &reg && check.above != &reg) {
        continue;
      }
      double below = 0.0;
      double above = 0.0;
      if (device_.runCheck(check, &reg, raw, &below, &above) != OV_CHECK_FAIL) {
        continue;
      }
      printWrapped("  Careful: ", "    ", check.message);
      out_.print("    After this write, ");
      out_.print(check.below->name);
      out_.print(" = ");
      printQuantity(*check.below, below);
      out_.print(" and ");
      out_.print(check.above->name);
      out_.print(" = ");
      printQuantity(*check.above, above);
      out_.println(". Put the other one right before you 'reset'.");
    }
  }

  void cmdSet(int argc, char **argv) {
    if (argc < 3) {
      out_.println("Usage: set <name> <value>   or   set <name> raw <number>");
      return;
    }
    const OvRegister *reg = findRegisterArg(argv[1]);
    if (reg == nullptr) {
      out_.print("There is no register called ");
      printQuoted(argv[1]);
      out_.println(". 'dump' lists them all.");
      return;
    }
    if (!reg->writable) {
      out_.print(reg->name);
      out_.println(" is read-only in this profile.");
      if (reg->note[0] != '\0') {
        printWrapped("  ", "  ", reg->note);
      }
      return;
    }
    uint16_t raw = 0;
    if (is(argv[2], "raw")) {
      uint32_t number = 0;
      if (argc < 4 || !ovParseU32(argv[3], &number) || number > 0xFFFF) {
        out_.println("A raw value is a whole number from 0 to 65535, or 0x0000 to 0xFFFF.");
        return;
      }
      raw = (uint16_t)number;
      // The limits apply to what the raw value means.
      const uint16_t words[2] = {raw, 0};
      uint16_t unused = 0;
      const OvEncodeStatus status = ovEncode(*reg, ovValue(*reg, words), &unused);
      if (status != OV_ENCODE_OK) {
        explainEncode(*reg, status);
        return;
      }
    } else {
      double value = 0.0;
      if (!ovParseDecimal(argv[2], &value)) {
        printQuoted(argv[2]);
        out_.println(" is not a number. Example: set EV_lvd 13.10");
        return;
      }
      const OvEncodeStatus status = ovEncode(*reg, value, &raw);
      if (status != OV_ENCODE_OK) {
        explainEncode(*reg, status);
        return;
      }
    }

    uint16_t now[2] = {0, 0};
    OvWordStatus status;
    if (!device_.readRegister(*reg, now, &status)) {
      out_.print("Could not read ");
      out_.print(reg->name);
      out_.print(" first (");
      describe(status);
      out_.println("), so nothing was written.");
      return;
    }
    char text[OV_VALUE_TEXT];
    const uint16_t next[2] = {raw, 0};
    out_.print(reg->name);
    out_.print(": ");
    out_.println(reg->label);
    ovFormatValue(*reg, now, text, sizeof(text));
    out_.print("  now  ");
    out_.printPadded(text, 14);
    printRawWords(now, 1);
    out_.println();
    ovFormatValue(*reg, next, text, sizeof(text));
    out_.print("  new  ");
    out_.printPadded(text, 14);
    printRawWords(next, 1);
    out_.println();
    if (now[0] == raw) {
      out_.println("It already holds that value. Nothing to write.");
      return;
    }
    const OvScaleCheck &check = device_.profile().scaleCheck();
    if (settingsScaleOpen() && reg->scale == check.settingsScale) {
      out_.print("  Careful: the scale of stored settings (");
      out_.print(check.settingsScale->name);
      out_.print(", ");
      printScale(check.settingsScale->num, check.settingsScale->den);
      out_.println(") is not confirmed on this controller.");
      out_.println("    If 'now' is not the value you expect, stop here and run 'scalecheck'.");
    }
    previewChecks(*reg, raw);
    printWriteNotes();
    out_.println("Type yes to write it. Anything else cancels.");
    pending_.kind = PENDING_SET;
    pending_.reg = reg;
    pending_.value = raw;
  }

  void runSet(const Pending &action) {
    const OvRegister &reg = *action.reg;
    const OvWriteOutcome outcome = device_.writeRegister(reg, action.value);
    char text[OV_VALUE_TEXT];
    const uint16_t readBack[2] = {outcome.readBackRaw, 0};
    ovFormatValue(reg, readBack, text, sizeof(text));
    if (outcome.verified) {
      if (outcome.write.status == MB_OK) {
        out_.print("Written and read back: ");
      } else {
        out_.print("The controller did not acknowledge the write (");
        describe(outcome.write);
        out_.print("), but it took effect: ");
      }
      out_.print(reg.name);
      out_.print(" = ");
      out_.print(text);
      out_.print(" ");
      printRawWords(readBack, 1);
      out_.println();
      resetPending_ = true;
      if (device_.profile().modbus().resetCoil != nullptr) {
        out_.println("The controller uses it after a restart: 'reset' when all your changes are in.");
      }
      return;
    }
    if (outcome.write.status == MB_OK) {
      out_.print("The controller acknowledged the write, ");
    } else {
      out_.print("The write failed (");
      describe(outcome.write);
      out_.print("), ");
    }
    if (outcome.readBack.status == MB_OK) {
      out_.print(outcome.write.status == MB_OK ? "but " : "and ");
      out_.print(reg.name);
      out_.print(" reads ");
      out_.print(text);
      out_.print(" ");
      printRawWords(readBack, 1);
      out_.println(outcome.write.status == MB_OK ? ", not what was written." : ".");
    } else {
      out_.print("and reading it back failed too (");
      describe(outcome.readBack);
      out_.println(").");
      out_.print("'get ");
      out_.print(reg.name);
      out_.println("' shows what it holds now.");
    }
  }

  void cmdCoil(int argc, char **argv) {
    OvProfile &profile = device_.profile();
    bool on = false;
    if (argc < 3 || !parseOnOff(argv[2], &on)) {
      out_.println("Usage: coil <name> on|off   ('coils' lists them)");
      return;
    }
    const OvCoil *coil = profile.findCoil(argv[1]);
    if (coil == nullptr) {
      out_.print("There is no coil called ");
      printQuoted(argv[1]);
      out_.println(". 'coils' lists them.");
      return;
    }
    if (coil == profile.modbus().resetCoil) {
      out_.println("Use 'reset' for that: it waits for the controller to come back.");
      return;
    }
    out_.print(coil->name);
    out_.print(": ");
    out_.println(coil->label);
    out_.print("  Switch it ");
    out_.print(on ? "on" : "off");
    out_.print(" (coil 0x");
    out_.printHex(coil->addr, 4);
    out_.println(")? Coils have not been tried on our bench.");
    out_.println("Type yes to send it. Anything else cancels.");
    pending_.kind = PENDING_COIL;
    pending_.coil = coil;
    pending_.on = on;
  }

  void runCoil(const Pending &action) {
    const OvCoil &coil = *action.coil;
    const MbResult result = device_.writeCoil(coil, action.on);
    if (result.status != MB_OK) {
      out_.print("The controller did not accept it: ");
      describe(result);
      out_.println(".");
      return;
    }
    out_.print("Sent and acknowledged.");
    if (!coil.trigger) {
      bool now = false;
      const MbResult readBack = device_.readCoil(coil, &now);
      if (readBack.status == MB_OK) {
        out_.print(" ");
        out_.print(coil.name);
        out_.print(" now reads ");
        out_.print(now ? "on" : "off");
        out_.print(".");
      }
    }
    out_.println();
  }

  void cmdReset(int argc, char **argv) {
    OvProfile &profile = device_.profile();
    if (profile.modbus().resetCoil == nullptr) {
      out_.println("This profile has no reset command. Power-cycle the controller instead.");
      return;
    }
    const bool anyway = argc > 1 && is(argv[1], "anyway");
    bool problems = false;
    for (size_t i = 0; i < profile.checkCount() && !device_.linkDown(); ++i) {
      const OvCheck &check = profile.check(i);
      double below = 0.0;
      double above = 0.0;
      const OvCheckResult result = device_.runCheck(check, nullptr, 0, &below, &above);
      if (result == OV_CHECK_FAIL) {
        problems = true;
        printWrapped("  Problem: ", "    ", check.message);
        out_.print("    Now ");
        out_.print(check.below->name);
        out_.print(" = ");
        printQuantity(*check.below, below);
        out_.print(" and ");
        out_.print(check.above->name);
        out_.print(" = ");
        printQuantity(*check.above, above);
        out_.println(".");
      } else if (result == OV_CHECK_NOT_READ) {
        problems = true;
        out_.print("  Could not check ");
        out_.print(check.below->name);
        out_.print(" against ");
        out_.print(check.above->name);
        out_.println(": a register did not read.");
      }
    }
    if (device_.linkDown()) {
      afterReads();
      return;
    }
    if (problems && !anyway) {
      out_.println("Put that right first, or use 'reset anyway'.");
      return;
    }
    out_.print("This restarts the controller (coil 0x");
    out_.printHex(profile.modbus().resetCoil->addr, 4);
    out_.println("). Charging and the load output may drop out for a moment.");
    out_.println("Type yes to restart it. Anything else cancels.");
    pending_.kind = PENDING_RESET;
  }

  void runReset() {
    out_.println("Restarting the controller. Press a key to stop waiting.");
    const OvResetOutcome outcome = device_.resetController(this);
    if (!outcome.sent) {
      out_.println("The reset command could not be sent.");
      return;
    }
    if (outcome.command.status == MB_EXCEPTION) {
      out_.print("The controller refused the reset command: ");
      describe(outcome.command);
      out_.println(".");
      return;
    }
    if (outcome.command.status == MB_OK) {
      out_.println("Reset acknowledged.");
    } else {
      out_.print("No acknowledgement (");
      describe(outcome.command);
      out_.println("). A controller may restart before it answers, so waiting anyway.");
    }
    if (outcome.aborted) {
      out_.println("Stopped waiting.");
      return;
    }
    if (!outcome.cameBack) {
      out_.print("The controller has not answered for ");
      out_.printU((uint32_t)(outcome.backAfterMs / 1000UL));
      out_.println(" s. Check it, then 'probe'.");
      return;
    }
    out_.print("The controller answers again after ");
    out_.printFixed((double)outcome.backAfterMs / 1000.0, 1);
    out_.println(" s.");
    resetPending_ = false;
    printLiveGroups();
    out_.println("If a fault above still asks for a reset, the controller did not restart: try again or");
    out_.println("power-cycle it.");
    afterReads();
  }

  void runPending(const Pending &action) {
    switch (action.kind) {
      case PENDING_SET:
        runSet(action);
        break;
      case PENDING_COIL:
        runCoil(action);
        break;
      case PENDING_RESET:
        runReset();
        break;
      case PENDING_RAW_WRITE:
        runRawWrite(action);
        break;
      default:
        break;
    }
  }

  // -------------------------------------------------------------------------------------------
  // Commands: link
  // -------------------------------------------------------------------------------------------

  void cmdLink(int argc, char **argv) {
    if (argc > 1) {
      uint32_t baud = 0;
      uint8_t format = device_.link.format;
      if (!ovParseU32(argv[1], &baud) || baud < 300 || baud > 1000000UL) {
        out_.println("Usage: link <baud> [8N1|8N2|8E1|8O1], for example: link 9600 8N2");
        return;
      }
      if (argc > 2 && !parseFormat(argv[2], &format)) {
        out_.println("The format is one of 8N1, 8N2, 8E1 and 8O1.");
        return;
      }
      const OvLink before = device_.link;
      device_.link.baud = baud;
      device_.link.format = format;
      if (!device_.applyLink()) {
        device_.link = before;
        device_.applyLink();
        out_.println("The serial port cannot do that. The link is unchanged.");
      }
      blocksNoticeShown_ = false;
    }
    out_.print("Link: ");
    printLink();
    out_.println();
  }

  void cmdSlave(int argc, char **argv) {
    if (argc > 1) {
      uint32_t slave = 0;
      if (!ovParseU32(argv[1], &slave) || slave < 1 || slave > 247) {
        out_.println("Slave addresses run from 1 to 247.");
        return;
      }
      device_.link.slave = (uint8_t)slave;
    }
    out_.print("Link: ");
    printLink();
    out_.println();
  }

  void cmdFunction(int argc, char **argv) {
    if (argc > 1) {
      uint32_t function = 0;
      if (!ovParseU32(argv[1], &function) || (function != MB_FC_READ_HOLDING && function != MB_FC_READ_INPUT)) {
        out_.println("Registers are read with function 3 or 4.");
        return;
      }
      device_.link.readFunction = (uint8_t)function;
    }
    out_.print("Link: ");
    printLink();
    out_.println();
  }

  void cmdTimeout(int argc, char **argv) {
    ModbusMaster &master = device_.master();
    if (argc > 1) {
      uint32_t ms = 0;
      if (!ovParseU32(argv[1], &ms) || ms < 50 || ms > 10000) {
        out_.println("The timeout runs from 50 to 10000 ms.");
        return;
      }
      master.responseTimeoutMs = (uint16_t)ms;
    }
    out_.print("Reply timeout: ");
    out_.printU(master.responseTimeoutMs);
    out_.println(" ms");
  }

  void cmdBlocks(int argc, char **argv) {
    if (argc > 1) {
      if (is(argv[1], "auto")) {
        device_.setBlockMode(OV_BLOCKS_AUTO);
      } else if (is(argv[1], "on")) {
        device_.setBlockMode(OV_BLOCKS_ON);
      } else if (is(argv[1], "off")) {
        device_.setBlockMode(OV_BLOCKS_OFF);
      } else {
        out_.println("Usage: blocks auto|on|off");
        return;
      }
      blocksNoticeShown_ = false;
    }
    out_.print("Block reads: ");
    out_.print(ovBlockModeName(device_.blockMode));
    if (device_.blockMode == OV_BLOCKS_AUTO && device_.blocksFailed()) {
      out_.print(" (gave up on blocks; reading one register at a time)");
    }
    out_.println();
  }

  void cmdProfile() {
    OvProfile &profile = device_.profile();
    out_.print(profile.title());
    out_.print(" (");
    out_.print(profile.id());
    out_.print(", ");
    out_.print(profile.version());
    out_.println(")");
    printWrapped("Source: ", "  ", profile.source());
    printWrapped("Confirmed on hardware: ", "  ", profile.verifiedBy());
    for (size_t i = 0; i < profile.noteCount(); ++i) {
      printWrapped("- ", "  ", profile.note(i));
    }
    if (profile.writeNoteCount() > 0) {
      out_.println("Before writing settings:");
      for (size_t i = 0; i < profile.writeNoteCount(); ++i) {
        printWrapped("- ", "  ", profile.writeNote(i));
      }
    }
  }

  // -------------------------------------------------------------------------------------------
  // Commands: diagnostics
  // -------------------------------------------------------------------------------------------

  void cmdTrace(int argc, char **argv) {
    bool on = trace_;
    if (argc > 1 && !parseOnOff(argv[1], &on)) {
      out_.println("Usage: trace on|off");
      return;
    }
    trace_ = on;
    device_.master().trace = trace_ ? this : nullptr;
    out_.print("Trace is ");
    out_.println(trace_ ? "on: every request (>) and reply (<) is shown." : "off.");
  }

  void cmdExpert(int argc, char **argv) {
    bool on = expert_;
    if (argc > 1 && !parseOnOff(argv[1], &on)) {
      out_.println("Usage: expert on|off");
      return;
    }
    expert_ = on;
    out_.print("Expert mode is ");
    out_.println(expert_ ? "on: 'raw' can now write to any address. The profile's limits do not apply there."
                         : "off.");
  }

  void cmdSniff(int argc, char **argv) {
    uint32_t ms = 5000;
    if (argc > 1 && (!ovParseU32(argv[1], &ms) || ms < 100 || ms > 600000UL)) {
      out_.println("Usage: sniff [ms], from 100 to 600000");
      return;
    }
    MbTransport &transport = device_.master().transport();
    out_.print("Listening for ");
    out_.printU(ms);
    out_.println(" ms without sending. Press a key to stop.");
    const uint32_t start = transport.nowMs();
    uint8_t burst[64];
    size_t count = 0;
    uint32_t burstStart = start;
    uint32_t lastByte = start;
    uint32_t total = 0;
    for (;;) {
      const uint32_t now = transport.nowMs();
      bool got = false;
      int b;
      while ((b = transport.read()) >= 0) {
        if (count == sizeof(burst)) {
          printBurst((uint32_t)(burstStart - start), burst, count);
          count = 0;
        }
        if (count == 0) {
          burstStart = now;
        }
        burst[count++] = (uint8_t)b;
        lastByte = now;
        got = true;
        ++total;
      }
      if (!got && count > 0 && (uint32_t)(now - lastByte) >= 5) {
        printBurst((uint32_t)(burstStart - start), burst, count);
        count = 0;
      }
      if ((uint32_t)(now - start) >= ms || stopRequested()) {
        break;
      }
      transport.idle();
    }
    if (count > 0) {
      printBurst((uint32_t)(burstStart - start), burst, count);
    }
    if (stopped_) {
      out_.println("Stopped.");
    }
    out_.print("Heard ");
    out_.printU(total);
    out_.println(total == 1 ? " byte." : " bytes.");
    if (total == 0) {
      out_.println("A quiet bus is normal when OptaView is the only master.");
    }
  }

  void printBurst(uint32_t atMs, const uint8_t *data, size_t count) {
    out_.print("  +");
    out_.printU(atMs);
    out_.print(" ms  ");
    out_.printHexBytes(data, count);
    if (mbCrcOk(data, count)) {
      out_.print("  (a complete Modbus frame)");
    }
    out_.println();
  }

  void rawUsage() {
    out_.println("Usage: raw <slave> <function> <address> <count|value>");
    out_.println("       functions 1 to 4 read; 5, 6 and 16 write one value and need 'expert on'");
    out_.println("       raw <slave> <function> <hex byte>...   sends any other function as typed (expert)");
  }

  void printLastExchange(const MbResult &result) {
    if (trace_) {
      return;  // the trace has shown it already
    }
    ModbusMaster &master = device_.master();
    printExchange(master.lastTx(), master.lastTxLen(), master.lastRx(), master.lastRxLen(), result);
  }

  void cmdRaw(int argc, char **argv) {
    ModbusMaster &master = device_.master();
    uint32_t slave = 0;
    uint32_t function = 0;
    if (argc < 3 || !ovParseU32(argv[1], &slave) || slave < 1 || slave > 247 || !ovParseU32(argv[2], &function) ||
        function < 1 || function > 127) {
      rawUsage();
      return;
    }
    const bool reads = function >= MB_FC_READ_COILS && function <= MB_FC_READ_INPUT;
    const bool writes =
        function == MB_FC_WRITE_COIL || function == MB_FC_WRITE_REGISTER || function == MB_FC_WRITE_REGISTERS;
    if (!reads && !expert_) {
      out_.println("Only functions 1 to 4 are open here. 'expert on' unlocks the rest.");
      return;
    }
    if (!reads && !writes) {
      // Any other function: the bytes after the function code, as typed.
      uint8_t body[OV_MAX_ARGS];
      size_t length = 0;
      body[length++] = (uint8_t)slave;
      body[length++] = (uint8_t)function;
      for (int i = 3; i < argc; ++i) {
        if (!parseHexByte(argv[i], &body[length])) {
          printQuoted(argv[i]);
          out_.println(" is not a hex byte (00 to FF).");
          return;
        }
        ++length;
      }
      const MbResult result = master.rawRequest(body, length);
      printLastExchange(result);
      return;
    }
    uint32_t addr = 0;
    uint32_t value = 0;
    if (argc < 5 || !ovParseU32(argv[3], &addr) || addr > 0xFFFF || !ovParseU32(argv[4], &value) ||
        value > 0xFFFF) {
      rawUsage();
      return;
    }
    if (writes) {
      out_.print("Write 0x");
      out_.printHex(value, 4);
      out_.print(" to address 0x");
      out_.printHex(addr, 4);
      out_.print(" on slave ");
      out_.printU(slave);
      out_.print(" with function ");
      out_.printU(function);
      out_.println("? No limits apply and nothing is checked first.");
      out_.println("Type yes to send it. Anything else cancels.");
      pending_.kind = PENDING_RAW_WRITE;
      pending_.slave = (uint8_t)slave;
      pending_.function = (uint8_t)function;
      pending_.addr = (uint16_t)addr;
      pending_.value = (uint16_t)value;
      return;
    }
    if (function == MB_FC_READ_HOLDING || function == MB_FC_READ_INPUT) {
      uint16_t words[MB_MAX_READ_REGISTERS];
      if (value < 1 || value > MB_MAX_READ_REGISTERS) {
        out_.println("The count runs from 1 to 125.");
        return;
      }
      const MbResult result =
          master.readRegisters((uint8_t)slave, (uint8_t)function, (uint16_t)addr, (uint16_t)value, words);
      printLastExchange(result);
      if (result.status == MB_OK) {
        for (uint32_t i = 0; i < value; ++i) {
          out_.print("  0x");
          out_.printHex(addr + i, 4);
          out_.print("  0x");
          out_.printHex(words[i], 4);
          out_.print("  ");
          out_.printU(words[i]);
          out_.println();
        }
      }
      return;
    }
    uint8_t bits[64];
    if (value < 1 || value > sizeof(bits)) {
      out_.println("The count runs from 1 to 64.");
      return;
    }
    const MbResult result =
        master.readBits((uint8_t)slave, (uint8_t)function, (uint16_t)addr, (uint16_t)value, bits);
    printLastExchange(result);
    if (result.status == MB_OK) {
      for (uint32_t i = 0; i < value; ++i) {
        out_.print("  0x");
        out_.printHex(addr + i, 4);
        out_.println(bits[i] != 0 ? "  on" : "  off");
      }
    }
  }

  void runRawWrite(const Pending &action) {
    ModbusMaster &master = device_.master();
    MbResult result;
    if (action.function == MB_FC_WRITE_REGISTERS) {
      result = master.writeRegisters(action.slave, action.addr, 1, &action.value);
    } else {
      result = master.writeSingle(action.slave, action.function, action.addr, action.value);
    }
    printLastExchange(result);
    if (result.status == MB_OK) {
      out_.println("Acknowledged.");
      resetPending_ = true;
    } else {
      out_.print("Not accepted: ");
      describe(result);
      out_.println(".");
    }
  }
};

}  // namespace optaview

#endif  // OPTAVIEW_CONSOLE_H
