/**
 * sunsaver-rs485-direct-serial2.ino
 *
 * Direct Opta Serial2/DE/RE SunSaver probe.
 * Bypasses ArduinoRS485 and ArduinoModbus to test whether the global RS485
 * object or its direction sequencing is hiding a valid MRC-1 response.
 */

static const uint32_t BAUD_RATE = 9600;
static const uint32_t LISTEN_WINDOW_MS = 900;
static const uint16_t POST_TX_DELAY_US = 1200;
static const uint8_t SLAVE_ID = 1;

static int gDePin = -1;
static int gRePin = -1;

extern int PinNameToIndex(PinName pinName);

struct SerialProfile {
  uint16_t config;
  const char *name;
};

struct RegProbe {
  uint16_t addr;
  const char *name;
};

static const SerialProfile kSerialProfiles[] = {
  { SERIAL_8N2, "8N2" },
  { SERIAL_8N1, "8N1" }
};

static const RegProbe kProbes[] = {
  { 0x0008, "adc_vb_f (batt V)" },
  { 0x0009, "adc_va_f (array V)" },
  { 0x000A, "adc_vl_f (load V)" },
  { 0x000B, "adc_ic_f (charge I)" },
  { 0x000C, "adc_il_f (load I)" }
};

static uint16_t crc16Modbus(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint16_t)data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      if (crc & 0x0001) {
        crc >>= 1;
        crc ^= 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

static void printHexByte(uint8_t value) {
  if (value < 0x10) Serial.print('0');
  Serial.print(value, HEX);
}

static void printHexBuf(const uint8_t *buf, uint16_t len) {
  for (uint16_t i = 0; i < len; ++i) {
    if (i > 0) Serial.print(' ');
    printHexByte(buf[i]);
  }
}

static void setTransmitMode() {
  digitalWrite(gRePin, HIGH);
  digitalWrite(gDePin, HIGH);
}

static void setReceiveMode() {
  digitalWrite(gDePin, LOW);
  digitalWrite(gRePin, LOW);
}

static void drainRx() {
  setReceiveMode();
  uint32_t deadline = millis() + 20UL;
  while ((int32_t)(millis() - deadline) < 0) {
    while (Serial2.available()) {
      Serial2.read();
    }
  }
}

static void buildRequest(uint8_t *frame, uint16_t addr) {
  frame[0] = SLAVE_ID;
  frame[1] = 0x04;
  frame[2] = (uint8_t)(addr >> 8);
  frame[3] = (uint8_t)(addr & 0xFF);
  frame[4] = 0x00;
  frame[5] = 0x01;
  uint16_t crc = crc16Modbus(frame, 6);
  frame[6] = (uint8_t)(crc & 0xFF);
  frame[7] = (uint8_t)(crc >> 8);
}

static void probeRegister(const SerialProfile &profile, const RegProbe &probe) {
  uint8_t tx[8];
  buildRequest(tx, probe.addr);

  drainRx();

  Serial.print(F("\n>>> direct "));
  Serial.print(profile.name);
  Serial.print(F(" slv=1 fc=0x04 reg=0x"));
  printHexByte((uint8_t)(probe.addr >> 8));
  printHexByte((uint8_t)(probe.addr & 0xFF));
  Serial.print(F(" "));
  Serial.print(probe.name);
  Serial.print(F(" TX ["));
  printHexBuf(tx, 8);
  Serial.println(F("]"));

  setTransmitMode();
  delayMicroseconds(50);
  Serial2.write(tx, sizeof(tx));
  Serial2.flush();
  delayMicroseconds(POST_TX_DELAY_US);
  setReceiveMode();

  uint8_t rx[96];
  uint16_t rxLen = 0;
  uint32_t deadline = millis() + LISTEN_WINDOW_MS;
  while ((int32_t)(millis() - deadline) < 0) {
    while (Serial2.available()) {
      int value = Serial2.read();
      if (value >= 0 && rxLen < sizeof(rx)) {
        rx[rxLen++] = (uint8_t)value;
      }
    }
  }

  Serial.print(F("<<< direct RX ("));
  Serial.print(LISTEN_WINDOW_MS);
  Serial.print(F("ms): "));
  if (rxLen == 0) {
    Serial.print(F("<none>"));
  } else {
    printHexBuf(rx, rxLen);
  }
  Serial.print(F("  ["));
  Serial.print(rxLen);
  Serial.println(F(" bytes]"));
}

void setup() {
  Serial.begin(115200);
  uint32_t startMs = millis();
  while (!Serial && millis() - startMs < 3000UL) delay(10);

  gDePin = PinNameToIndex(PB_14);
  gRePin = PinNameToIndex(PB_13);
  pinMode(gDePin, OUTPUT);
  pinMode(gRePin, OUTPUT);
  setReceiveMode();

  Serial.println();
  Serial.println(F("================================================"));
  Serial.println(F("SunSaver direct Serial2 RS485 probe"));
  Serial.print(F("  Baud: "));
  Serial.println(BAUD_RATE);
  Serial.print(F("  DE pin index: "));
  Serial.println(gDePin);
  Serial.print(F("  RE pin index: "));
  Serial.println(gRePin);
  Serial.println(F("  TX path: Serial2 + PB14/PB13, no ArduinoRS485"));
  Serial.println(F("================================================"));
}

void loop() {
  static uint32_t cycle = 0;
  static uint32_t lastCycle = 0;
  static uint32_t lastHb = 0;

  uint32_t now = millis();
  if (now - lastHb >= 1000UL) {
    lastHb = now;
    Serial.print(F("hb ms="));
    Serial.print(now);
    Serial.print(F(" cycle="));
    Serial.println(cycle + 1);
  }

  if (now - lastCycle < 3000UL) return;
  lastCycle = now;

  const SerialProfile &profile = kSerialProfiles[cycle % (sizeof(kSerialProfiles) / sizeof(kSerialProfiles[0]))];
  Serial2.end();
  delay(20);
  Serial2.begin(BAUD_RATE, profile.config);
  delay(20);

  Serial.println();
  Serial.println(F("------------------------------------------------"));
  Serial.print(F("Direct cycle "));
  Serial.print(cycle + 1);
  Serial.print(F(" using "));
  Serial.println(profile.name);
  Serial.println(F("------------------------------------------------"));

  for (uint8_t i = 0; i < sizeof(kProbes) / sizeof(kProbes[0]); ++i) {
    probeRegister(profile, kProbes[i]);
    delay(150);
  }

  ++cycle;
}