/**
 * OptaView.ino
 *
 * OptaView 0.3: reads and programs a charge controller from an Arduino Opta, over the Opta's
 * RS-485 port. It talks to a Morningstar SunSaver MPPT through an MRC-1 adapter, so that the
 * field does not need a Windows laptop with MSView. There are two ways in, and both work at
 * the same time:
 *
 *   a console on the USB port     serial monitor at 115200 baud, "New Line" as the line ending;
 *                                 type 'probe', 'live' or 'help'
 *   a web page on the Ethernet    http://<address>/ in a browser; 'net' on the console shows the
 *   port                          address (192.168.1.16 when no DHCP server answers)
 *
 *   Board:      Arduino Opta RS485 or Opta WiFi, core "Arduino Mbed OS Opta Boards"
 *   Libraries:  ArduinoRS485, ArduinoJson (version 7); Ethernet comes with the board's core
 *   Settings:   optional, in OptaViewConfig.h (see OptaViewConfig.h.example)
 *
 * This file is only the glue between the Opta and the parts that do the work, which have no
 * Arduino dependency and are tested on a PC against a simulated SunSaver (tests/host/optaview):
 *
 *   OptaViewModbus.h          Modbus RTU master that checks every reply before it is believed
 *   OptaViewProfile.h         the register map as data (profiles/sunsaver-mppt.json)
 *   OptaViewDevice.h          block reads with fallback, probe, verified writes, reset
 *   OptaViewConsole.h         the commands
 *   OptaViewHttp.h            the little HTTP the page needs
 *   OptaViewNet.h             how the Opta gets its address
 *   OptaViewWeb.h             the web server: login, the API, the console over HTTP
 *   OptaViewBuiltinProfile.h  the profile, embedded by tools/embed_profile.py
 *   OptaViewWebPage.h         the page (web/index.html), embedded by tools/embed_web.py
 *
 * README.md has the wiring, a first session and what is still unconfirmed on hardware.
 */

#include <ArduinoJson.h>
#include <ArduinoRS485.h>
#include <PortentaEthernet.h>
#include <Ethernet.h>

#include "OptaViewBuiltinProfile.h"
#include "OptaViewConsole.h"
#include "OptaViewDevice.h"
#include "OptaViewHttp.h"
#include "OptaViewModbus.h"
#include "OptaViewNet.h"
#include "OptaViewProfile.h"
#include "OptaViewText.h"
#include "OptaViewWeb.h"
#include "OptaViewWebPage.h"

// What one installation sets for itself: a PIN, a fixed address. The file is optional and is
// not kept in the repository (OptaViewConfig.h.example is the template).
#if __has_include("OptaViewConfig.h")
#include "OptaViewConfig.h"
#endif
#ifndef OPTAVIEW_WEB_PIN
#define OPTAVIEW_WEB_PIN ""  // none: the first visitor of the page chooses it
#endif
#ifndef OPTAVIEW_NET_MODE
#define OPTAVIEW_NET_MODE "auto"
#endif
#ifndef OPTAVIEW_NET_ADDRESS
#define OPTAVIEW_NET_ADDRESS "192.168.1.16"
#endif
#ifndef OPTAVIEW_NET_MASK
#define OPTAVIEW_NET_MASK "255.255.255.0"
#endif
#ifndef OPTAVIEW_NET_GATEWAY
#define OPTAVIEW_NET_GATEWAY nullptr  // the usual one: the first address of the network (192.168.1.1)
#endif

// A mistake in the settings stops the build here. Found only when the Opta runs, it would leave
// an Opta that nobody can find on the network, or a page without the PIN that was meant.
static_assert(OPTAVIEW_WEB_PIN[0] == '\0' || optaview::ovIsPin(OPTAVIEW_WEB_PIN),
              "OPTAVIEW_WEB_PIN in OptaViewConfig.h must be 4 to 8 digits in quotes, like \"2468\"");
static_assert(optaview::ovIsNetModeName(OPTAVIEW_NET_MODE),
              "OPTAVIEW_NET_MODE in OptaViewConfig.h must be \"auto\", \"dhcp\" or \"static\"");
static_assert(!optaview::ovIsNetModeName(OPTAVIEW_NET_MODE) ||
                  optaview::ovNetConfigTextProblem(OPTAVIEW_NET_MODE, OPTAVIEW_NET_ADDRESS, OPTAVIEW_NET_MASK,
                                                   OPTAVIEW_NET_GATEWAY) == nullptr,
              "OPTAVIEW_NET_ADDRESS, OPTAVIEW_NET_MASK or OPTAVIEW_NET_GATEWAY in OptaViewConfig.h cannot be used: "
              "each is written like \"192.168.1.16\", the mask is usually \"255.255.255.0\", and the gateway is "
              "another address on the same network, or \"\" for none");

// The processor's random number generator, as the core's TLS library reaches it.
extern "C" int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);

static const unsigned long USB_BAUD = 115200;
static const uint16_t WEB_PORT = 80;

static optaview::OvLineEditor gEditor;

// The Opta's RS-485 port as the Modbus master's transport.
class Rs485Transport : public optaview::MbTransport {
 public:
  Rs485Transport() : open_(false) {}

  bool configure(uint32_t baud, uint8_t format) override {
    uint16_t config;
    switch (format) {
      case optaview::MB_8N1: config = SERIAL_8N1; break;
      case optaview::MB_8N2: config = SERIAL_8N2; break;
      case optaview::MB_8E1: config = SERIAL_8E1; break;
      case optaview::MB_8O1: config = SERIAL_8O1; break;
      default: return false;
    }
    if (baud < 300 || baud > 115200) {
      return false;
    }
    if (open_) {
      RS485.noReceive();
      RS485.end();
      delay(20);
    }
    RS485.begin(baud, config);
    // The driver must stay on for one character (11 bits) after the UART is done, or the last
    // stop bit is cut short and the controller ignores the request. This is the TankAlarm
    // client's formula, 1195 microseconds at 9600 baud. It has no upper limit here, because
    // OptaView also offers baud rates below 9600.
    uint32_t postDelayUs = 11000000UL / baud + 50UL;
    if (postDelayUs < 150UL) {
      postDelayUs = 150UL;
    }
    RS485.setDelays(0, (int)postDelayUs);
    RS485.receive();
    open_ = true;
    delay(20);
    while (RS485.available() > 0) {
      RS485.read();
    }
    return true;
  }

  // The transmit bracket that the SunSaver bring-up arrived at in April 2026, unchanged: the
  // receiver is off while the driver is on, and the receiver is back on before the reply.
  void send(const uint8_t *data, size_t len) override {
    if (!open_) {
      return;
    }
    RS485.noReceive();
    RS485.beginTransmission();
    RS485.write(data, len);
    RS485.flush();
    delay(1);
    RS485.endTransmission();
    RS485.receive();
  }

  int read() override { return (open_ && RS485.available() > 0) ? RS485.read() : -1; }

  uint32_t nowMs() override { return millis(); }

  void idle() override { delay(1); }

  bool isOpen() const { return open_; }

 private:
  bool open_;
};

class SerialOut : public optaview::OvOut {
 public:
  void write(const char *text, size_t len) override { Serial.write((const uint8_t *)text, len); }
};

class SerialHost : public optaview::OvConsoleHost {
 public:
  // A key press stops a long command. So does closing the serial monitor: nobody is left to
  // press one.
  bool inputPending() override {
    // The line feed of the CR LF that started the command is not a key press.
    if (gEditor.expectsLineFeed() && Serial.peek() == '\n') {
      Serial.read();
      gEditor.lineFeedTaken();
    }
    return Serial.available() > 0 || !Serial;
  }

  // Takes the whole key press: Enter may be two characters, and the IDE's monitor sends a
  // full line at once.
  void discardInput() override {
    const uint32_t start = millis();
    do {
      while (Serial.available() > 0) {
        Serial.read();
      }
      delay(1);
    } while ((uint32_t)(millis() - start) < 20);
    gEditor.lineFeedTaken();
  }
};

class MillisClock : public optaview::OvClock {
 public:
  uint32_t nowMs() override { return millis(); }
  void idle() override { delay(1); }
};

// The Opta's Ethernet port, for the address policy in OptaViewNet.h. Nothing here waits: with
// a timeout of 0 the library starts the interface and returns.
class EthernetDriver : public optaview::OvNetDriver {
 public:
  void startDhcp() override { Ethernet.begin(static_cast<uint8_t *>(nullptr), 0UL, 0UL); }

  void startStatic(const optaview::OvIp4 &ip, const optaview::OvIp4 &mask, const optaview::OvIp4 &gateway) override {
    const IPAddress address(ip.b[0], ip.b[1], ip.b[2], ip.b[3]);
    const IPAddress subnet(mask.b[0], mask.b[1], mask.b[2], mask.b[3]);
    const IPAddress gate(gateway.b[0], gateway.b[1], gateway.b[2], gateway.b[3]);
    // OptaView never looks a name up, so the name server only has to be some address.
    Ethernet.begin(static_cast<uint8_t *>(nullptr), address, gate, gate, subnet, 0UL, 0UL);
  }

  void stop() override { Ethernet.disconnect(); }

  // The library's "link on" means more than its name: the cable is in and an address is in use.
  bool up() override { return Ethernet.linkStatus() == LinkON; }

  optaview::OvIp4 address() override {
    const IPAddress ip = Ethernet.localIP();
    return optaview::ovIp4(ip[0], ip[1], ip[2], ip[3]);
  }

  void hardwareAddress(char *out, size_t cap) override {
    const String mac = Ethernet.macAddress();
    // All ff is the library's answer while the interface has not been started.
    optaview::ovCopy(out, cap, mac.equalsIgnoreCase("ff:ff:ff:ff:ff:ff") ? "" : mac.c_str());
    for (char *p = out; *p != '\0'; ++p) {
      *p = (char)toupper((unsigned char)*p);
    }
  }
};

// A client that is connected to nothing. Assigning it to a connection's client makes the
// library let go of what it held for a browser that has gone.
static EthernetClient gNoClient;

// One browser connection.
class EthernetConn : public optaview::OvConn {
 public:
  EthernetConn() : open_(false) {}

  bool isFree() const { return !open_; }

  void open(const EthernetClient &client) {
    client_ = client;
    // With its usual timeout of 1.5 s the library's write() waits for the browser, and starts
    // that wait again after every byte the browser takes, so a browser that reads slowly on
    // purpose could keep one call from returning. With 0 it hands over what fits into the
    // network stack's send buffer and returns. The waiting is OvHttpResponse's, with its limits.
    client_.setSocketTimeout(0);
    open_ = true;
  }

  int read() override { return open_ ? client_.read() : -1; }

  // The library answers 0 both when nothing fits right now and when the connection has
  // failed, so this cannot tell a browser that is gone from one that is slow. It does not have
  // to: OvHttpResponse gives up on a connection that takes nothing for a few seconds.
  int write(const uint8_t *data, size_t len) override {
    if (!open_) {
      return -1;
    }
    const size_t sent = client_.write(data, len);
    return sent <= len ? (int)sent : -1;
  }

  void close() override {
    if (open_) {
      client_.stop();  // closes the socket and ends the library's reader thread: up to 0.1 s
      client_ = gNoClient;
      open_ = false;
    }
  }

 private:
  EthernetClient client_;
  bool open_;
};

static EthernetServer gServer(WEB_PORT);
static bool gListening = false;

// Stirred with the time of things that nobody can time from outside to the microsecond: each
// key on the USB port, each connection. It is mixed into what the random number generator gives,
// as a second ingredient for a generator that answers with poor bytes, and it stands in alone for
// a generator that does not answer (see random()).
static uint64_t gJitter = 0x4F70746156696577ULL;

static void stir(uint32_t value) {
  gJitter ^= value;
  gJitter *= 0x9E3779B97F4A7C15ULL;
  gJitter ^= gJitter >> 29;
}

static MillisClock gClock;
static EthernetDriver gEthernet;
static optaview::OvNet gNet(gEthernet, gClock);

class EthernetWebHost : public optaview::OvWebHost {
 public:
  optaview::OvConn *accept() override {
    if (!gListening) {
      return nullptr;
    }
    for (size_t i = 0; i < sizeof(conns_) / sizeof(conns_[0]); ++i) {
      if (conns_[i].isFree()) {
        EthernetClient client = gServer.accept();
        if (!client) {
          return nullptr;
        }
        stir(micros());
        conns_[i].open(client);
        return &conns_[i];
      }
    }
    return nullptr;
  }

  // The processor's random number generator, asked a few times at most until it has given every
  // byte, with the jitter mixed in. False when it fails or falls short: the bytes it did not give
  // then come from the jitter alone. The web server uses them all the same, and the USB console
  // says that it had to.
  bool random(uint8_t *out, size_t len) override {
    size_t filled = 0;
    for (int tries = 0; tries < 4 && filled < len; ++tries) {
      size_t got = 0;
      if (mbedtls_hardware_poll(nullptr, out + filled, len - filled, &got) != 0) {
        break;
      }
      filled += got < len - filled ? got : len - filled;
    }
    memset(out + filled, 0, len - filled);
    for (size_t i = 0; i < len; ++i) {
      stir(micros());
      out[i] ^= (uint8_t)(gJitter >> 56);
    }
    return filled == len;
  }

  bool address(char *out, size_t cap) override {
    if (!gNet.up()) {
      return false;
    }
    optaview::ovFormatIp4(gNet.address(), out, cap);
    return true;
  }

 private:
  // The web server holds no more than this many connections at a time, and the Opta's network
  // stack has four sockets in all: these two, the listener, and one to spare.
  EthernetConn conns_[optaview::OV_WEB_SLOTS];
};

static Rs485Transport gTransport;
static optaview::ModbusMaster gMaster(gTransport);
static optaview::OvProfile gProfile;
static optaview::OvDevice gDevice(gMaster, gProfile);
static SerialOut gOut;
static SerialHost gHost;
static optaview::OvConsole gConsole(gOut, gHost, gDevice);

static EthernetWebHost gWebHost;
static const optaview::OvWebPage gPage = {optaview::OV_WEB_PAGE, optaview::OV_WEB_PAGE_LENGTH,
                                          optaview::OV_WEB_PAGE_HEADERS};
static optaview::OvWeb gWeb(gDevice, gWebHost, gClock, gPage);

static bool gReady = false;      // the profile loaded
static char gProblem[160];       // why the profile did not load
static bool gConnected = false;  // a terminal has the USB port open
static bool gAnnounce = false;   // the network has changed and the terminal has not been told

// Where the page is, for a terminal on the USB port.
static void printNetwork() {
  gNet.status(gOut);
  gWeb.status(gOut);
}

// Greets a terminal that has just opened the port.
static void greet() {
  gEditor.clear();
  if (!gReady) {
    Serial.println();
    Serial.print("OptaView cannot start: its built-in profile did not load (");
    Serial.print(gProblem);
    Serial.println(").");
    return;
  }
  // A question the last visitor left open is not this visitor's to answer.
  gConsole.cancelPending();
  gConsole.begin();
  if (!gTransport.isOpen()) {
    Serial.println("The RS-485 port did not open with these link settings. 'link <baud> [format]' sets others.");
  }
  printNetwork();
  gAnnounce = false;
  Serial.println("Each command ends with Enter. In the Arduino serial monitor choose 'New Line'.");
  gConsole.prompt();
}

// Runs the line the editor has just completed.
static void runLine() {
  if (!gReady) {
    greet();
    return;
  }
  gConsole.handleLine(gEditor.line());
  gConsole.prompt();
}

void setup() {
  Serial.begin(USB_BAUD);
  gReady = gProfile.load(optaview::OV_BUILTIN_PROFILE, sizeof(optaview::OV_BUILTIN_PROFILE) - 1, gProblem,
                         sizeof(gProblem));
  if (!gReady) {
    return;
  }
  gDevice.useProfileDefaults();
  gDevice.applyLink();

  gConsole.addExtra(&gNet);
  gConsole.addExtra(&gWeb);
  if (OPTAVIEW_WEB_PIN[0] != '\0') {
    gWeb.setPin(OPTAVIEW_WEB_PIN);
  }
  // These settings were checked when the sketch was compiled (the static_asserts at the top).
  optaview::OvNetConfig config = gNet.config();
  optaview::ovNetConfigFromText(OPTAVIEW_NET_MODE, OPTAVIEW_NET_ADDRESS, OPTAVIEW_NET_MASK, OPTAVIEW_NET_GATEWAY,
                                &config);
  gNet.begin(config);
}

void loop() {
  const bool connected = (bool)Serial;  // on the Opta this also yields for a millisecond
  if (connected && !gConnected) {
    greet();
  }
  gConnected = connected;
  while (Serial.available() > 0) {
    stir(micros());
    if (gEditor.feed(Serial.read(), gOut)) {
      runLine();
    }
  }
  if (!gReady) {
    return;
  }

  gNet.poll();
  if (gNet.up() && !gListening) {
    // Once, and never ended: the listener is bound to the port, not to an address, so it
    // outlives every change of address. Starting a second one would use up a socket.
    gServer.begin();
    gListening = true;
  }
  gAnnounce = gNet.takeChange() || gAnnounce;
  if (gAnnounce && gConnected && gEditor.empty()) {
    // Not in the middle of a line that is being typed.
    gAnnounce = false;
    Serial.println();
    printNetwork();
    gConsole.prompt();
  }
  gWeb.poll();
}
