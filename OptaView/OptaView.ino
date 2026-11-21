/**
 * OptaView.ino
 *
 * OptaView 0.2: a Modbus console for charge controllers on an Arduino Opta. It reads and
 * programs a Morningstar SunSaver MPPT through an MRC-1 adapter, so that the field does not
 * need a Windows laptop with MSView.
 *
 *   Board:      Arduino Opta RS485 or Opta WiFi, core "Arduino Mbed OS Opta Boards"
 *   Libraries:  ArduinoRS485, ArduinoJson (version 7)
 *   Use:        open the serial monitor at 115200 baud with "New Line" as the line ending,
 *               then type 'probe', 'live' or 'help'
 *
 * This file is only the glue between the Opta and the parts that do the work, which have no
 * Arduino dependency and are tested on a PC against a simulated SunSaver (tests/host/optaview):
 *
 *   OptaViewModbus.h          Modbus RTU master that checks every reply before it is believed
 *   OptaViewProfile.h         the register map as data (profiles/sunsaver-mppt.json)
 *   OptaViewDevice.h          block reads with fallback, probe, verified writes, reset
 *   OptaViewConsole.h         the commands
 *   OptaViewBuiltinProfile.h  the profile, embedded by tools/embed_profile.py
 *
 * README.md has the wiring, a first session and what is still unconfirmed on hardware.
 */

#include <ArduinoJson.h>
#include <ArduinoRS485.h>

#include "OptaViewBuiltinProfile.h"
#include "OptaViewConsole.h"
#include "OptaViewDevice.h"
#include "OptaViewModbus.h"
#include "OptaViewProfile.h"
#include "OptaViewText.h"

static const unsigned long USB_BAUD = 115200;

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

static Rs485Transport gTransport;
static optaview::ModbusMaster gMaster(gTransport);
static optaview::OvProfile gProfile;
static optaview::OvDevice gDevice(gMaster, gProfile);
static SerialOut gOut;
static SerialHost gHost;
static optaview::OvConsole gConsole(gOut, gHost, gDevice);

static bool gReady = false;      // the profile loaded
static char gProblem[160];       // why the profile did not load
static bool gConnected = false;  // a terminal has the USB port open

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
  if (gReady) {
    gDevice.useProfileDefaults();
    gDevice.applyLink();
  }
}

void loop() {
  const bool connected = (bool)Serial;  // on the Opta this also yields for a millisecond
  if (connected && !gConnected) {
    greet();
  }
  gConnected = connected;
  while (Serial.available() > 0) {
    if (gEditor.feed(Serial.read(), gOut)) {
      runLine();
    }
  }
}
