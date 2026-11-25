/**
 * OptaViewNet.h
 *
 * How OptaView gets its network address, and the 'net' commands of the USB console.
 *
 * No Arduino dependency: the Ethernet port is reached through OvNetDriver, which the sketch
 * (OptaView.ino) implements with the Opta's Ethernet library and the host tests with a fake.
 * Nothing here blocks: begin() and poll() return at once, so the USB console works while the
 * network is still looking for an address.
 *
 * Three modes
 * -----------
 *   auto     DHCP first. When no address has arrived after 20 s, the fixed address is used.
 *            If the cable only goes in after that, DHCP gets one more try on the network the
 *            cable leads to, because the first try was made on no network at all. The same
 *            holds when an address from DHCP is lost: the cable may have moved to a laptop.
 *            The stack cannot tell "no cable" from "no DHCP server" while DHCP is waiting, but
 *            it can once a fixed address is set, so a cable that is only out for a while still
 *            ends up back on DHCP. That one more try needs the link to be seen missing 3 s or
 *            more after the fixed address was set. A cable that goes back in sooner looks like
 *            one that was in all along, and so does one that goes in while a long command (a
 *            scan, a sniff) holds the loop from before those 3 s are up: the link it finds then
 *            may have been there all along.
 *   dhcp     DHCP only, for as long as it takes. The mode for a network that always has DHCP:
 *            in auto mode a router that takes longer than 20 s to hand out addresses again, as
 *            after a power cut, leaves the Opta on the fixed address.
 *   static   the fixed address only.
 *
 * Nothing is stored: a mode typed at the console lasts until power-off. OptaViewConfig.h sets
 * what the Opta starts with.
 *
 * A note on the Opta's network stack (mbed OS 6.17, read from its source, not seen on hardware):
 * when a DHCP attempt is given up while no cable was ever in, the stack still starts DHCP when
 * the cable goes in later, on top of the fixed address it was given since. The policy here does
 * not rely on that and does not trust it either: in a fixed-address state it compares the
 * address in use with the one it asked for (verifyAddress).
 */

#ifndef OPTAVIEW_NET_H
#define OPTAVIEW_NET_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "OptaViewConsole.h"
#include "OptaViewHttp.h"
#include "OptaViewText.h"

namespace optaview {

struct OvIp4 {
  uint8_t b[4];
};

// The functions from here to ovNetConfigTextProblem are constexpr so that the sketch can check
// the settings of OptaViewConfig.h while it is compiled: a mistyped address then stops the build
// instead of leaving an Opta that nobody can find on the network.

constexpr OvIp4 ovIp4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) { return OvIp4{{a, b, c, d}}; }

constexpr bool ovIp4Equal(const OvIp4 &a, const OvIp4 &b) {
  return a.b[0] == b.b[0] && a.b[1] == b.b[1] && a.b[2] == b.b[2] && a.b[3] == b.b[3];
}
constexpr bool ovIp4IsZero(const OvIp4 &ip) { return (ip.b[0] | ip.b[1] | ip.b[2] | ip.b[3]) == 0; }
constexpr uint32_t ovIp4Bits(const OvIp4 &ip) {
  return ((uint32_t)ip.b[0] << 24) | ((uint32_t)ip.b[1] << 16) | ((uint32_t)ip.b[2] << 8) | ip.b[3];
}

// "192.168.1.16": four decimal numbers from 0 to 255, without signs, spaces or leading zeros.
constexpr bool ovParseIp4(const char *text, OvIp4 *out) {
  if (text == nullptr) {
    return false;
  }
  const char *p = text;
  for (int field = 0; field < 4; ++field) {
    if (*p < '0' || *p > '9') {
      return false;
    }
    if (p[0] == '0' && p[1] >= '0' && p[1] <= '9') {
      return false;  // "010" reads as 8 in some places and as 10 in others
    }
    unsigned value = 0;
    for (int digits = 0; *p >= '0' && *p <= '9'; ++p, ++digits) {
      value = value * 10u + (unsigned)(*p - '0');
      if (digits >= 3 || value > 255u) {
        return false;
      }
    }
    out->b[field] = (uint8_t)value;
    if (field < 3) {
      if (*p != '.') {
        return false;
      }
      ++p;
    }
  }
  return *p == '\0';
}

inline size_t ovFormatIp4(const OvIp4 &ip, char *out, size_t cap) {
  OvText text(out, cap);
  for (int i = 0; i < 4; ++i) {
    if (i > 0) {
      text.addChar('.');
    }
    text.addU(ip.b[i]);
  }
  return text.length();
}

// A subnet mask is ones followed by zeros. Accepted from /8 to /30.
constexpr bool ovIsNetmask(const OvIp4 &mask) {
  const uint32_t bits = ovIp4Bits(mask);
  const uint32_t inverted = ~bits;
  if ((inverted & (inverted + 1u)) != 0) {
    return false;  // the zeros are not all at the end
  }
  return bits >= 0xFF000000UL && bits <= 0xFFFFFFFCUL;
}

// An address a device can have on the network that `mask` describes.
constexpr bool ovIsHostAddress(const OvIp4 &ip, const OvIp4 &mask) {
  if (ip.b[0] == 0 || ip.b[0] == 127 || ip.b[0] >= 224) {
    return false;
  }
  const uint32_t host = ovIp4Bits(ip) & ~ovIp4Bits(mask);
  return host != 0 && host != (~ovIp4Bits(mask));  // neither the network itself nor its broadcast
}

constexpr bool ovSameSubnet(const OvIp4 &a, const OvIp4 &b, const OvIp4 &mask) {
  return ((ovIp4Bits(a) ^ ovIp4Bits(b)) & ovIp4Bits(mask)) == 0;
}

// The gateway that is assumed when none is named: the first address of the network, which is
// where a router usually is. None (0.0.0.0) when that is the device's own address. Without a
// gateway the Opta still answers everything on its own network.
constexpr OvIp4 ovUsualGateway(const OvIp4 &ip, const OvIp4 &mask) {
  return ((ovIp4Bits(ip) & ovIp4Bits(mask)) | 1u) == ovIp4Bits(ip)
             ? ovIp4(0, 0, 0, 0)
             : ovIp4(ip.b[0] & mask.b[0], ip.b[1] & mask.b[1], ip.b[2] & mask.b[2], (ip.b[3] & mask.b[3]) | 1u);
}

// The Ethernet port.
class OvNetDriver {
 public:
  virtual ~OvNetDriver() {}
  // Each of these returns at once. Call stop() before starting the other way.
  virtual void startDhcp() = 0;
  virtual void startStatic(const OvIp4 &ip, const OvIp4 &mask, const OvIp4 &gateway) = 0;
  virtual void stop() = 0;
  // True while the link is up and an address is in use.
  virtual bool up() = 0;
  // The address in use, or 0.0.0.0.
  virtual OvIp4 address() = 0;
  // "A8:61:0A:..." or "" when it is not known yet.
  virtual void hardwareAddress(char *out, size_t cap) = 0;
};

enum OvNetMode : uint8_t { OV_NET_AUTO = 0, OV_NET_DHCP = 1, OV_NET_STATIC = 2 };

struct OvNetConfig {
  uint8_t mode;  // OvNetMode
  // The fixed address: used at once in static mode, as the fallback in auto mode.
  OvIp4 ip;
  OvIp4 mask;
  OvIp4 gateway;  // 0.0.0.0 = none
};

// Why a fixed address was refused, or nullptr when it is fine.
constexpr const char *ovNetConfigProblem(const OvNetConfig &config) {
  if (config.mode > OV_NET_STATIC) {
    return "The mode is auto, dhcp or static.";
  }
  if (!ovIsNetmask(config.mask)) {
    return "That is not a subnet mask (255.255.255.0 is the usual one).";
  }
  if (!ovIsHostAddress(config.ip, config.mask)) {
    return "That address cannot be given to a device on that network.";
  }
  if (!ovIp4IsZero(config.gateway) &&
      (!ovSameSubnet(config.ip, config.gateway, config.mask) || ovIp4Equal(config.ip, config.gateway) ||
       !ovIsHostAddress(config.gateway, config.mask))) {
    return "The gateway has to be another address on the same network (or leave it out).";
  }
  return nullptr;
}

// True when `text` is `word`, which is given in lower case, in any mix of cases.
constexpr bool ovIsWord(const char *text, const char *word) {
  if (text == nullptr) {
    return false;
  }
  size_t i = 0;
  for (; word[i] != '\0'; ++i) {
    const char c = text[i];
    if ((c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c) != word[i]) {
      return false;
    }
  }
  return text[i] == '\0';
}

// "auto", "dhcp" or "static" as a mode. False, and `mode` untouched, for any other word.
constexpr bool ovNetModeFromName(const char *name, uint8_t *mode) {
  if (ovIsWord(name, "auto")) {
    *mode = OV_NET_AUTO;
  } else if (ovIsWord(name, "dhcp")) {
    *mode = OV_NET_DHCP;
  } else if (ovIsWord(name, "static")) {
    *mode = OV_NET_STATIC;
  } else {
    return false;
  }
  return true;
}

constexpr bool ovIsNetModeName(const char *name) {
  uint8_t mode = OV_NET_AUTO;
  return ovNetModeFromName(name, &mode);
}

// The configuration as OptaViewConfig.h spells it: a mode by its name and addresses as text.
// For the gateway, "" is none and nullptr is the usual one (ovUsualGateway). Fills `config` and
// returns nullptr, or says what is wrong and leaves `config` alone.
constexpr const char *ovNetConfigFromText(const char *mode, const char *ip, const char *mask, const char *gateway,
                                          OvNetConfig *config) {
  OvNetConfig wanted = {};
  if (!ovNetModeFromName(mode, &wanted.mode)) {
    return "The mode is auto, dhcp or static.";
  }
  if (!ovParseIp4(ip, &wanted.ip) || !ovParseIp4(mask, &wanted.mask) ||
      (gateway != nullptr && gateway[0] != '\0' && !ovParseIp4(gateway, &wanted.gateway))) {
    return "An address is four numbers from 0 to 255, like 192.168.1.16.";
  }
  if (gateway == nullptr) {
    wanted.gateway = ovUsualGateway(wanted.ip, wanted.mask);
  }
  const char *problem = ovNetConfigProblem(wanted);
  if (problem == nullptr) {
    *config = wanted;
  }
  return problem;
}

// The same check without a configuration to fill, for a static_assert.
constexpr const char *ovNetConfigTextProblem(const char *mode, const char *ip, const char *mask,
                                             const char *gateway) {
  OvNetConfig scratch = {};
  return ovNetConfigFromText(mode, ip, mask, gateway, &scratch);
}

static const uint32_t OV_NET_DHCP_WAIT_MS = 20000;  // auto mode: how long DHCP gets
static const uint32_t OV_NET_PLUG_MS = 3000;        // no link this long after a fixed address is set: no cable
static const uint32_t OV_NET_CHECK_MS = 1000;

class OvNet : public OvConsoleExtra {
 public:
  OvNet(OvNetDriver &driver, OvClock &clock)
      : driver_(driver),
        clock_(clock),
        state_(STATE_OFF),
        sinceMs_(0),
        checkedMs_(0),
        fallback_(false),
        dhcpWorked_(false),
        retried_(false),
        missed_(false),
        reapplied_(0),
        mismatch_(false),
        changed_(false) {
    config_.mode = OV_NET_AUTO;
    config_.ip = ovIp4(192, 168, 1, 16);
    config_.mask = ovIp4(255, 255, 255, 0);
    config_.gateway = ovIp4(192, 168, 1, 1);
    address_ = ovIp4(0, 0, 0, 0);
    lastAddress_ = address_;
  }

  // Starts the network. Returns false, and starts nothing, when the configuration is not usable.
  bool begin(const OvNetConfig &config) {
    if (ovNetConfigProblem(config) != nullptr) {
      return false;
    }
    config_ = config;
    restart();
    return true;
  }

  const OvNetConfig &config() const { return config_; }

  // Call from loop().
  void poll() {
    if (state_ == STATE_OFF) {
      return;
    }
    const uint32_t now = clock_.nowMs();
    const bool up = driver_.up();
    switch (state_) {
      case STATE_DHCP_WAIT:
        if (up) {
          dhcpWorked_ = true;
          cameUp(STATE_DHCP_UP, now);
        } else if (config_.mode == OV_NET_AUTO && (uint32_t)(now - sinceMs_) >= OV_NET_DHCP_WAIT_MS) {
          // Also after an address from DHCP was lost: the cable may lead to a laptop now. If it
          // is only out, the fixed address waits for a link, and DHCP gets one more try when the
          // cable goes back in (STATE_STATIC_WAIT).
          driver_.stop();
          startStatic(true);
        }
        break;
      case STATE_STATIC_WAIT:
        if (!up) {
          if ((uint32_t)(now - sinceMs_) >= OV_NET_PLUG_MS) {
            missed_ = true;  // no cable: a link from here on means that it has gone in since
          }
          break;
        }
        if (fallback_ && !retried_ && missed_) {
          // The cable has gone in since the fixed address was set, so the DHCP attempt before
          // that was made on no network at all: try this one. Only a link that was seen missing
          // counts. A link seen for the first time long after the fixed address was set may have
          // been there all along, while a long command held the loop.
          retried_ = true;
          driver_.stop();
          startDhcp();
          break;
        }
        retried_ = true;
        cameUp(STATE_STATIC_UP, now);
        break;
      case STATE_DHCP_UP:
      case STATE_STATIC_UP:
        if (!up) {
          lastAddress_ = address_;
          address_ = ovIp4(0, 0, 0, 0);
          if (state_ == STATE_DHCP_UP) {
            // DHCP waits from here, with auto mode's fallback and its one extra try as at the start.
            retried_ = false;
          }
          state_ = state_ == STATE_DHCP_UP ? STATE_DHCP_WAIT : STATE_STATIC_WAIT;
          sinceMs_ = now;
          changed_ = true;
        } else if ((uint32_t)(now - checkedMs_) >= OV_NET_CHECK_MS) {
          checkedMs_ = now;
          verifyAddress();
        }
        break;
      default:
        break;
    }
  }

  // True while the web page can be reached at address().
  bool up() const { return state_ == STATE_DHCP_UP || state_ == STATE_STATIC_UP; }
  OvIp4 address() const { return address_; }

  // True once after the network came up, went down or changed its address.
  bool takeChange() {
    const bool was = changed_;
    changed_ = false;
    return was;
  }

  // --- OvConsoleExtra ---

  bool command(int argc, char **argv, OvOut &out) override {
    if (!ovEqualsIgnoreCase(argv[0], "net")) {
      return false;
    }
    if (argc == 1) {
      status(out);
      printDetails(out);
      return true;
    }
    OvNetConfig wanted = config_;
    if (ovEqualsIgnoreCase(argv[1], "auto") && argc == 2) {
      wanted.mode = OV_NET_AUTO;
    } else if (ovEqualsIgnoreCase(argv[1], "dhcp") && argc == 2) {
      wanted.mode = OV_NET_DHCP;
    } else if (ovEqualsIgnoreCase(argv[1], "static") && argc >= 3 && argc <= 5) {
      wanted.mode = OV_NET_STATIC;
      wanted.mask = ovIp4(255, 255, 255, 0);
      if (!ovParseIp4(argv[2], &wanted.ip) || (argc > 3 && !ovParseIp4(argv[3], &wanted.mask)) ||
          (argc > 4 && !ovParseIp4(argv[4], &wanted.gateway))) {
        out.println("An address is four numbers from 0 to 255, like 192.168.1.16.");
        return true;
      }
      if (argc <= 4) {
        wanted.gateway = ovUsualGateway(wanted.ip, wanted.mask);
      }
    } else {
      out.println("Usage: net   |   net auto   |   net dhcp   |   net static <address> [mask] [gateway]");
      return true;
    }
    const char *problem = ovNetConfigProblem(wanted);
    if (problem != nullptr) {
      out.println(problem);
      return true;
    }
    config_ = wanted;
    restart();
    status(out);
    out.println("          A browser that was connected has to connect again. This lasts until power-off;");
    out.println("          OptaViewConfig.h sets what OptaView starts with.");
    return true;
  }

  void help(OvOut &out) override {
    out.println("Network");
    out.println("  net                     the address and where it came from");
    out.println("  net auto | net dhcp     DHCP, with or without the fixed address as a fallback");
    out.println("  net static <address> [mask] [gateway]");
  }

  // One line: "Network   address 192.168.1.57 from DHCP"
  void status(OvOut &out) override {
    char text[20];
    out.print("Network   ");
    switch (state_) {
      case STATE_DHCP_UP:
        ovFormatIp4(address_, text, sizeof(text));
        out.print("address ");
        out.print(text);
        out.println(" from DHCP");
        break;
      case STATE_DHCP_WAIT:
        if (dhcpWorked_) {
          out.print("no link now, or no answer from DHCP");
          if (!ovIp4IsZero(lastAddress_)) {
            ovFormatIp4(lastAddress_, text, sizeof(text));
            out.print(" (the address was ");
            out.print(text);
            out.print(")");
          }
          out.print(". Is the cable in?");
          if (config_.mode == OV_NET_AUTO) {
            ovFormatIp4(config_.ip, text, sizeof(text));
            out.print(" After ");
            out.printU(OV_NET_DHCP_WAIT_MS / 1000UL);
            out.print(" s without an answer the fixed address ");
            out.print(text);
            out.print(" is used (");
            out.printU((uint32_t)((uint32_t)(clock_.nowMs() - sinceMs_) / 1000UL));
            out.print(" s so far).");
          }
          out.println();
        } else {
          out.print("waiting for an address from DHCP, ");
          out.printU((uint32_t)((uint32_t)(clock_.nowMs() - sinceMs_) / 1000UL));
          out.print(" s so far");
          if (config_.mode == OV_NET_AUTO) {
            ovFormatIp4(config_.ip, text, sizeof(text));
            out.print(" (after ");
            out.printU(OV_NET_DHCP_WAIT_MS / 1000UL);
            out.print(" s the fixed address ");
            out.print(text);
            out.print(" is used)");
          }
          out.println();
        }
        break;
      case STATE_STATIC_UP:
        ovFormatIp4(address_, text, sizeof(text));
        out.print("address ");
        out.print(text);
        if (mismatch_) {
          out.println(", which is not the fixed address that was asked for");
        } else {
          out.println(fallback_ ? ", the fixed one: DHCP did not answer" : ", fixed");
        }
        break;
      case STATE_STATIC_WAIT:
        ovFormatIp4(config_.ip, text, sizeof(text));
        out.print("fixed address ");
        out.print(text);
        out.print(fallback_ ? " (DHCP did not answer)" : "");
        out.println(", but no link. Is the cable in?");
        break;
      default:
        out.println("not started");
        break;
    }
  }

 private:
  enum State : uint8_t { STATE_OFF = 0, STATE_DHCP_WAIT, STATE_DHCP_UP, STATE_STATIC_WAIT, STATE_STATIC_UP };

  OvNetDriver &driver_;
  OvClock &clock_;
  OvNetConfig config_;
  uint8_t state_;
  uint32_t sinceMs_;    // when the state that is waiting began
  uint32_t checkedMs_;  // when the address in use was last looked at
  bool fallback_;       // the fixed address is in use because DHCP did not answer (auto mode)
  bool dhcpWorked_;     // DHCP has given an address since the mode was chosen
  bool retried_;        // the one extra DHCP try of auto mode is used up
  bool missed_;         // the link was missing OV_NET_PLUG_MS or more after the fixed address was set
  uint8_t reapplied_;   // times the fixed address had to be set again
  bool mismatch_;       // the address in use is not the fixed one, and setting it again did not help
  bool changed_;
  OvIp4 address_;       // in use now; 0.0.0.0 while the network is down
  OvIp4 lastAddress_;   // the one before the link went away

  // From the top, in the mode that config_ names.
  void restart() {
    if (state_ != STATE_OFF) {
      driver_.stop();
    }
    if (up()) {
      changed_ = true;
    }
    address_ = ovIp4(0, 0, 0, 0);
    lastAddress_ = address_;
    fallback_ = false;
    dhcpWorked_ = false;
    retried_ = false;
    reapplied_ = 0;
    mismatch_ = false;
    if (config_.mode == OV_NET_STATIC) {
      startStatic(false);
    } else {
      startDhcp();
    }
  }

  void startDhcp() {
    fallback_ = false;
    driver_.startDhcp();
    state_ = STATE_DHCP_WAIT;
    sinceMs_ = clock_.nowMs();
  }

  void startStatic(bool fallback) {
    fallback_ = fallback;
    missed_ = false;
    driver_.startStatic(config_.ip, config_.mask, config_.gateway);
    state_ = STATE_STATIC_WAIT;
    sinceMs_ = clock_.nowMs();
  }

  void cameUp(uint8_t state, uint32_t now) {
    state_ = state;
    checkedMs_ = now;
    address_ = driver_.address();
    changed_ = true;
    if (state == STATE_STATIC_UP) {
      verifyAddress();
    }
  }

  // Looks at the address that is really in use.
  void verifyAddress() {
    const OvIp4 inUse = driver_.address();
    if (!ovIp4Equal(inUse, address_)) {
      address_ = inUse;  // a new DHCP lease, for one
      changed_ = true;
    }
    if (state_ != STATE_STATIC_UP || ovIp4Equal(inUse, config_.ip)) {
      mismatch_ = false;
      return;
    }
    // A fixed address was asked for and another one is in use: DHCP has been at work behind it
    // (see the note at the top of this file).
    if (fallback_) {
      // Auto mode wanted DHCP in the first place, and here is the proof that it answers.
      dhcpWorked_ = true;
      driver_.stop();
      address_ = ovIp4(0, 0, 0, 0);
      startDhcp();
      return;
    }
    if (reapplied_ < 2) {
      ++reapplied_;
      driver_.stop();
      address_ = ovIp4(0, 0, 0, 0);
      startStatic(false);
      return;
    }
    mismatch_ = true;  // say so in 'net' rather than fight over it for ever
  }

  void printDetails(OvOut &out) {
    char text[20];
    out.print("          mode ");
    switch (config_.mode) {
      case OV_NET_AUTO:
        ovFormatIp4(config_.ip, text, sizeof(text));
        out.print("auto: DHCP first, then the fixed address ");
        out.print(text);
        out.print(" when nothing answers within ");
        out.printU(OV_NET_DHCP_WAIT_MS / 1000UL);
        out.println(" s");
        break;
      case OV_NET_DHCP:
        out.println("dhcp: DHCP only");
        break;
      default:
        out.print("static: ");
        ovFormatIp4(config_.ip, text, sizeof(text));
        out.print(text);
        out.print(", mask ");
        ovFormatIp4(config_.mask, text, sizeof(text));
        out.print(text);
        if (ovIp4IsZero(config_.gateway)) {
          out.println(", no gateway");
        } else {
          out.print(", gateway ");
          ovFormatIp4(config_.gateway, text, sizeof(text));
          out.println(text);
        }
        break;
    }
    char mac[24];
    driver_.hardwareAddress(mac, sizeof(mac));
    if (mac[0] != '\0') {
      out.print("          hardware address ");
      out.println(mac);
    }
  }
};

}  // namespace optaview

#endif  // OPTAVIEW_NET_H
