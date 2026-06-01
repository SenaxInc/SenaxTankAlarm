/*
  TankAlarm-112025 Sensor Utility (OptaBlue mode)

  Purpose:
  - Read Opta Ext A0602 current-loop channels using Arduino_Opta_Blueprint API
  - Match official Arduino examples for channel activation/configuration
  - Show raw mA and derived PSI for a 0-50 PSI transmitter
*/

#include "OptaBlue.h"

using namespace Opta;

#ifndef SERIAL_BAUD
#define SERIAL_BAUD 115200
#endif

#ifndef READ_INTERVAL_MS
#define READ_INTERVAL_MS 1000UL
#endif

static int8_t gFocusChannel = 0;  // I1 = channel 0
static unsigned long gLastReadMs = 0;
static bool gControllerReady = false;

static void printBanner();
static void printMenu();
static void scanExpansions();
static void configureCurrentModeAllChannels();
static bool readChannelMilliamps(uint8_t channel, float &maOut);
static void debugChannelRaw(uint8_t channel);
static void printFocusReading();
static void printAllChannels();
static float milliampsToPsi(float milliamps);
static bool readLine(char *buffer, size_t bufferSize, unsigned long timeoutMs);

void setup() {
  Serial.begin(SERIAL_BAUD);
  while (!Serial && millis() < 3000) {
    delay(10);
  }

  printBanner();

  OptaController.begin();
  OptaController.update();
  gControllerReady = true;

  scanExpansions();
  configureCurrentModeAllChannels();
  printMenu();
}

void loop() {
  if (gControllerReady) {
    OptaController.update();
  }

  if (Serial.available()) {
    char cmd = (char)Serial.read();
    if (cmd != '\r' && cmd != '\n') {
      switch (cmd) {
        case 'h':
        case '?':
          printMenu();
          break;
        case 's':
          scanExpansions();
          break;
        case 'c':
          configureCurrentModeAllChannels();
          break;
        case 'd':
          debugChannelRaw((uint8_t)gFocusChannel);
          break;
        case 'a':
          printAllChannels();
          break;
        case 'f': {
          Serial.println(F("Enter focus channel 0-7:"));
          char line[16] = {0};
          if (!readLine(line, sizeof(line), 30000UL)) {
            Serial.println(F("Timed out."));
            break;
          }
          int ch = atoi(line);
          if (ch < 0 || ch > 7) {
            Serial.println(F("Invalid channel. Use 0-7."));
            break;
          }
          gFocusChannel = (int8_t)ch;
          Serial.print(F("Focus channel set to CH"));
          Serial.println(gFocusChannel);
          break;
        }
        default:
          Serial.print(F("Unknown command: "));
          Serial.println(cmd);
          printMenu();
          break;
      }
    }
  }

  unsigned long now = millis();
  if (now - gLastReadMs >= READ_INTERVAL_MS) {
    gLastReadMs = now;
    printFocusReading();
  }
}

static void printBanner() {
  Serial.println();
  Serial.println(F("=============================================="));
  Serial.println(F(" TankAlarm-112025 Sensor Utility"));
  Serial.println(F("=============================================="));
  Serial.println(F("Mode: Arduino_Opta_Blueprint (OptaBlue)"));
  Serial.println(F("Target: A0602 current ADC on I1..I6/O1/O2 (CH0..CH7)"));
  Serial.println();
}

static void printMenu() {
  Serial.println(F("Commands:"));
  Serial.println(F("  h or ?  - Help"));
  Serial.println(F("  s       - Scan detected expansions"));
  Serial.println(F("  c       - Configure all channels as Current ADC"));
  Serial.println(F("  d       - Debug focused channel (raw ADC + mA)"));
  Serial.println(F("  a       - Read all channels (0-7) now"));
  Serial.println(F("  f       - Set focus channel (0-7)"));
  Serial.println();
}

static void scanExpansions() {
  OptaController.update();

  int expansionCount = OptaController.getExpansionNum();
  Serial.print(F("Detected expansions: "));
  Serial.println(expansionCount);

  for (int i = 0; i < expansionCount; ++i) {
    Serial.print(F("  Expansion["));
    Serial.print(i);
    Serial.print(F("] type="));
    Serial.print((int)OptaController.getExpansionType(i));
    Serial.print(F(" i2c=0x"));
    uint8_t addr = OptaController.getExpansionI2Caddress(i);
    if (addr < 0x10) {
      Serial.print('0');
    }
    Serial.println(addr, HEX);
  }
}

static void configureCurrentModeAllChannels() {
  OptaController.update();
  int expansionCount = OptaController.getExpansionNum();

  if (expansionCount <= 0) {
    Serial.println(F("No expansions detected. Check AUX and external 12-24V power."));
    return;
  }

  bool configuredAny = false;
  for (int i = 0; i < expansionCount; ++i) {
    AnalogExpansion exp = OptaController.getExpansion(i);
    if (!exp) {
      continue;
    }

    configuredAny = true;
    Serial.print(F("Configuring Analog Expansion "));
    Serial.print(exp.getIndex());
    Serial.println(F(" channels as OA_CURRENT_ADC..."));

    for (int ch = 0; ch < OA_AN_CHANNELS_NUM; ++ch) {
      AnalogExpansion::beginChannelAsCurrentAdc(OptaController, i, ch);
      delay(2);
    }
  }

  if (!configuredAny) {
    Serial.println(F("No A0602 analog expansion object found."));
    return;
  }

  Serial.println(F("Current ADC configuration complete."));
}

static bool readChannelMilliamps(uint8_t channel, float &maOut) {
  if (channel > 7) {
    return false;
  }

  OptaController.update();
  int expansionCount = OptaController.getExpansionNum();
  for (int i = 0; i < expansionCount; ++i) {
    AnalogExpansion exp = OptaController.getExpansion(i);
    if (!exp) {
      continue;
    }

    exp.updateAnalogInputs();
    maOut = exp.pinCurrent(channel, false);
    if (!isnan(maOut)) {
      return true;
    }
  }

  return false;
}

static void debugChannelRaw(uint8_t channel) {
  if (channel > 7) {
    Serial.println(F("Invalid debug channel."));
    return;
  }

  OptaController.update();
  int expansionCount = OptaController.getExpansionNum();
  if (expansionCount <= 0) {
    Serial.println(F("No expansions detected for debug."));
    return;
  }

  for (int i = 0; i < expansionCount; ++i) {
    AnalogExpansion exp = OptaController.getExpansion(i);
    if (!exp) {
      continue;
    }

    exp.updateAnalogInputs();
    uint16_t adc = exp.getAdc(channel, false);
    float ma = exp.pinCurrent(channel, false);

    Serial.print(F("DBG exp="));
    Serial.print(exp.getIndex());
    Serial.print(F(" ch="));
    Serial.print(channel);
    Serial.print(F(" adc="));
    Serial.print(adc);
    Serial.print(F(" mA="));
    Serial.println(ma, 4);
  }
}

static float milliampsToPsi(float milliamps) {
  float psi = (milliamps - 4.0f) * (50.0f / 16.0f);
  if (psi < 0.0f) {
    psi = 0.0f;
  }
  if (psi > 50.0f) {
    psi = 50.0f;
  }
  return psi;
}

static void printFocusReading() {
  float ma = 0.0f;
  bool ok = readChannelMilliamps((uint8_t)gFocusChannel, ma);

  Serial.print(F("CH"));
  Serial.print(gFocusChannel);
  Serial.print(F(" -> "));

  if (!ok) {
    Serial.println(F("READ FAIL"));
    return;
  }

  Serial.print(ma, 3);
  Serial.print(F(" mA | "));
  Serial.print(milliampsToPsi(ma), 2);
  Serial.println(F(" PSI (0-50 scale)"));
}

static void printAllChannels() {
  Serial.println(F("All A0602 channels (0-7):"));
  for (int ch = 0; ch < 8; ++ch) {
    float ma = 0.0f;
    bool ok = readChannelMilliamps((uint8_t)ch, ma);

    Serial.print(F("  CH"));
    Serial.print(ch);
    Serial.print(F(": "));

    if (!ok) {
      Serial.println(F("READ FAIL"));
      continue;
    }

    Serial.print(ma, 3);
    Serial.print(F(" mA | "));
    Serial.print(milliampsToPsi(ma), 2);
    Serial.println(F(" PSI"));
  }
  Serial.println();
}

static bool readLine(char *buffer, size_t bufferSize, unsigned long timeoutMs) {
  if (buffer == nullptr || bufferSize == 0) {
    return false;
  }

  memset(buffer, 0, bufferSize);
  size_t idx = 0;
  unsigned long start = millis();

  while ((millis() - start) < timeoutMs) {
    while (Serial.available()) {
      char c = (char)Serial.read();
      if (c == '\r' || c == '\n') {
        if (idx == 0) {
          continue;
        }
        buffer[idx] = '\0';
        return true;
      }
      if (idx < (bufferSize - 1)) {
        buffer[idx++] = c;
      }
    }
    delay(10);
  }

  return false;
}
