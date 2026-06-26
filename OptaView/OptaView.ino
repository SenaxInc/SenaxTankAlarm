/**
 * OptaView.ino
 *
 * OptaView: Morningstar MSView-on-Opta — First Standalone Demo v0.1
 * Fully compatible with Arduino Mbed Opta Core.
 *
 * Implements:
 * 1. Hardened TX Turnaround Bracket verbatim from April 2026 bring-up fixes:
 *    - RS485.setDelays(0, 1200) for stop-bit survival at 9600 baud.
 *    - noReceive() + beginTransmission() + write() + flush() + delay(1) + endTransmission() + receive()
 * 2. Automatic Slave ID, Baud rate, and Parity Probe Wizard.
 * 3. Interactive CLI Console over USB Serial (115200 baud) for remote configuration.
 * 4. Raw Modbus RTU Frame Generation & Parser with CRC-16 checks.
 * 5. SunSaver MPPT Live Data scaling and register layouts (FC04 0x0008-0x000C).
 * 6. Configuration updates using FC16 (Write Multiple Registers).
 */

#include <ArduinoRS485.h>

static const uint16_t LISTEN_WINDOW_DEFAULT_MS = 900;
static const uint16_t INTER_TRANSACTION_DELAY_MS = 150;

// Morningstar MPPT Default Register Maps
struct ControlRegister {
  uint16_t addr;
  const char *name;
  const char *units;
  float scaleNum;
  float scaleDen;
  bool isCurrent; // true for current scaling, false for voltage
};

static const ControlRegister kSunSaverRegs[] = {
  { 0x0008, "adc_vb_f (Battery Voltage)", "V", 96.667f, 32768.0f, false },
  { 0x0009, "adc_va_f (Solar Voltage)",   "V", 96.667f, 32768.0f, false },
  { 0x000A, "adc_vl_f (Load Voltage)",    "V", 96.667f, 32768.0f, false },
  { 0x000B, "adc_ic_f (Charge Current)",  "A", 79.16f,  32768.0f, true  },
  { 0x000C, "adc_il_f (Load Current)",    "A", 79.16f,  32768.0f, true  }
};
static const uint8_t kSunSaverRegCount = sizeof(kSunSaverRegs) / sizeof(kSunSaverRegs[0]);

// Modern vs Legacy Wire Swap Warning
static void printWireSwapWarning() {
  Serial.println(F("\n[!] CABLE / POLARITY IMPORTANT REMINDER:"));
  Serial.println(F("    Morningstar Terminal A = DATA- (Inverting)"));
  Serial.println(F("    Morningstar Terminal B = DATA+ (Non-Inverting)"));
  Serial.println(F("    Arduino Opta Terminal A = DATA+ | Opta Terminal B = DATA-"));
  Serial.println(F("    WIRING MUST BE CROSSED: Opta A <-> MRC-1 B | Opta B <-> MRC-1 A"));
}

// Low-Level CRC-16 Modbus Utility
static uint16_t crc16Modbus(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint16_t)data[i];
    for (uint8_t b = 0; b < 8; ++b) {
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

// Low-Level Print Utilities
static void printHexByte(uint8_t b) {
  if (b < 0x10) Serial.print('0');
  Serial.print(b, HEX);
}

static void printHexBuf(const uint8_t *buf, uint16_t len) {
  for (uint16_t i = 0; i < len; ++i) {
    if (i > 0) Serial.print(' ');
    printHexByte(buf[i]);
  }
}

// Hardened Modbus Transaction Core
static bool executeTransaction(uint8_t *txFrame, uint16_t txLen, uint8_t *rxFrame, uint16_t &rxLen, uint32_t listenMs) {
  // Clear any stray garbage in the hardware RX buffer first
  RS485.noReceive();
  while (RS485.available()) {
    RS485.read();
  }

  // Bracket transmission to prevent stop-bit chopping
  RS485.beginTransmission();
  RS485.write(txFrame, txLen);
  RS485.flush();
  delay(1); // survival turnaround guard
  RS485.endTransmission();

  // Listen window
  RS485.receive();
  uint32_t deadline = millis() + listenMs;
  rxLen = 0;

  while ((int32_t)(millis() - deadline) < 0) {
    while (RS485.available()) {
      int b = RS485.read();
      if (b >= 0) {
        if (rxLen < 96) {
          rxFrame[rxLen++] = (uint8_t)b;
        }
      }
    }
  }
  RS485.noReceive();

  if (rxLen < 4) {
    return false; // too short to even consist of slave + fc + crc
  }

  // Validate CRC16
  uint16_t crcWire = (uint16_t)rxFrame[rxLen - 2] | ((uint16_t)rxFrame[rxLen - 1] << 8);
  uint16_t crcCalc = crc16Modbus(rxFrame, rxLen - 2);
  if (crcWire != crcCalc) {
    Serial.print(F(" [CRC Fail Calc="));
    Serial.print(crcCalc, HEX);
    Serial.print(F(" Wire="));
    Serial.print(crcWire, HEX);
    Serial.print(F("] "));
    return false;
  }

  return true;
}

// Perform automated probe sweep
static void runProbeWizard() {
  Serial.println(F("\n================================================"));
  Serial.println(F("              PROBE WIZARD STARTING             "));
  Serial.println(F("================================================"));
  Serial.println(F("Sweeping physical link profiles..."));

  uint16_t configs[] = { SERIAL_8N2, SERIAL_8N1 };
  const char *configNames[] = { "8N2 (Spec Default)", "8N1" };
  uint8_t slaves[] = { 1, 247, 2, 16 };
  uint32_t baudRates[] = { 9600, 19200 };

  uint8_t tx[8];
  uint8_t rx[96];
  uint16_t rxLen = 0;

  bool found = false;

  for (size_t bIdx = 0; bIdx < sizeof(baudRates)/sizeof(baudRates[0]); ++bIdx) {
    uint32_t targetBaud = baudRates[bIdx];
    for (size_t cIdx = 0; cIdx < sizeof(configs)/sizeof(configs[0]); ++cIdx) {
      uint16_t targetCfg = configs[cIdx];
      const char *cfgName = configNames[cIdx];

      // Re-init core RS485
      RS485.end();
      delay(20);
      RS485.begin(targetBaud, targetCfg);
      RS485.setDelays(0, 1200); // lock post-delay turnaround for 9600/19200 baud
      delay(20);

      for (size_t sIdx = 0; sIdx < sizeof(slaves); ++sIdx) {
        uint8_t slave = slaves[sIdx];
        
        Serial.print(F("Trying Baud="));
        Serial.print(targetBaud);
        Serial.print(F(" Parity="));
        Serial.print(cfgName);
        Serial.print(F(" SlaveID="));
        Serial.print(slave);
        Serial.print(F(" ... "));

        // Build simple FC04 (Read Input Register) for Battery Voltage (0x0008)
        tx[0] = slave;
        tx[1] = 0x04; // read input registers
        tx[2] = 0x00;
        tx[3] = 0x08; // adc_vb_f register
        tx[4] = 0x00;
        tx[5] = 0x01; // count = 1
        uint16_t crc = crc16Modbus(tx, 6);
        tx[6] = (uint8_t)(crc & 0xFF);
        tx[7] = (uint8_t)(crc >> 8);

        if (executeTransaction(tx, 8, rx, rxLen, LISTEN_WINDOW_DEFAULT_MS)) {
          Serial.println(F("\nSUCCESS! SENSE FRAME CAPTURED EXCELLENTLY."));
          Serial.print(F("  Response Hex: "));
          printHexBuf(rx, rxLen);
          Serial.println();
          
          if (rx[1] == 0x04 && rx[2] == 2) {
            uint16_t rawVal = ((uint16_t)rx[3] << 8) | rx[4];
            float voltage = (float)rawVal * (96.667f / 32768.0f);
            Serial.print(F("  Battery voltage reads: "));
            Serial.print(voltage, 4);
            Serial.println(F(" V"));
          }
          found = true;
          break;
        } else {
          if (rxLen > 0) {
            Serial.print(F("Received corrupted/non-matching bytes ["));
            printHexBuf(rx, rxLen);
            Serial.println(F("]"));
          } else {
            Serial.println(F("Timeout. No Response."));
          }
        }
        delay(INTER_TRANSACTION_DELAY_MS);
      }
      if (found) break;
    }
    if (found) break;
  }

  if (!found) {
    Serial.println(F("\n================================================"));
    Serial.println(F("         PROBE COMPLETED WITH NO MODULE          "));
    Serial.println(F("================================================"));
    Serial.println(F("TROUBLESHOOTING RECOMMENDATIONS:"));
    Serial.println(F("1. Check MRC-1 LED indicator:"));
    Serial.println(F("   - OFF: No 12V from charge controller MeterBus cable."));
    Serial.println(F("   - SOLID GREEN: Powered, but completely idle. Wires not reaching."));
    Serial.println(F("   - SOLID AMBER: A/B Polarity Reversed. Reverse A/B wires at Opta interface."));
    printWireSwapWarning();
  } else {
    Serial.println(F("================================================"));
    Serial.println(F("               PROBE WIZARD COMPLETED           "));
    Serial.println(F("================================================"));
  }
}

// Live monitor of telemetry
static void runLiveMonitor(uint8_t slaveId) {
  Serial.print(F("\nPolling Live Telemetry Registers (Filtered) on Slave "));
  Serial.println(slaveId);

  uint8_t tx[8];
  uint8_t rx[96];
  uint16_t rxLen = 0;

  // Build sequential reads to SunSaver live region (0x0008 to 0x000C)
  tx[0] = slaveId;
  tx[1] = 0x04; // Read Input Registers
  tx[2] = 0x00;
  tx[3] = 0x08; // start address
  tx[4] = 0x00;
  tx[5] = 0x05; // read all 5 contiguous telemetry registers
  uint16_t crc = crc16Modbus(tx, 6);
  tx[6] = (uint8_t)(crc & 0xFF);
  tx[7] = (uint8_t)(crc >> 8);

  Serial.print(F(">>> poll TX: "));
  printHexBuf(tx, 8);
  Serial.println();

  if (executeTransaction(tx, 8, rx, rxLen, LISTEN_WINDOW_DEFAULT_MS)) {
    Serial.print(F("<<< poll RX: "));
    printHexBuf(rx, rxLen);
    Serial.println();

    // Verify response structure: slave, fc, byteCount (must be 10), data..., crc2
    if (rx[1] == 0x04 && rx[2] == 10) {
      Serial.println(F("\n--- Live Data Reads ---"));
      for (uint8_t i = 0; i < kSunSaverRegCount; ++i) {
        uint16_t raw = ((uint16_t)rx[3 + (i * 2)] << 8) | rx[4 + (i * 2)];
        float scaled = (float)raw * (kSunSaverRegs[i].scaleNum / kSunSaverRegs[i].scaleDen);
        Serial.print(F("  "));
        Serial.print(kSunSaverRegs[i].name);
        Serial.print(F(": 0x"));
        printHexByte((uint8_t)(raw >> 8));
        printHexByte((uint8_t)(raw & 0xFF));
        Serial.print(F(" ("));
        Serial.print(raw);
        Serial.print(F(") -> "));
        Serial.print(scaled, 4);
        Serial.print(F(" "));
        Serial.println(kSunSaverRegs[i].units);
      }
    } else {
      Serial.println(F("Error: Unexpected or mis-framed response."));
    }
  } else {
    Serial.println(F("Error: Modbus transaction failed (Timeout or CRC verification failed)."));
  }
}

// Raw frame command utility
static void sendRawFrame(uint8_t slave, uint8_t fc, uint16_t address, uint16_t count) {
  uint8_t tx[8];
  uint8_t rx[96];
  uint16_t rxLen = 0;

  tx[0] = slave;
  tx[1] = fc;
  tx[2] = (uint8_t)(address >> 8);
  tx[3] = (uint8_t)(address & 0xFF);
  tx[4] = (uint8_t)(count >> 8);
  tx[5] = (uint8_t)(count & 0xFF);
  uint16_t crc = crc16Modbus(tx, 6);
  tx[6] = (uint8_t)(crc & 0xFF);
  tx[7] = (uint8_t)(crc >> 8);

  Serial.println();
  Serial.print(F(">>> Raw Frame TX: "));
  printHexBuf(tx, 8);
  Serial.println();

  if (executeTransaction(tx, 8, rx, rxLen, LISTEN_WINDOW_DEFAULT_MS)) {
    Serial.print(F("<<< Raw Frame RX: "));
    printHexBuf(rx, rxLen);
    Serial.print(F("  ["));
    Serial.print(rxLen);
    Serial.println(F(" bytes]"));
  } else {
    if (rxLen > 0) {
      Serial.print(F("<<< Fail Raw Frame RX (Bad CRC/Format): "));
      printHexBuf(rx, rxLen);
      Serial.println();
    } else {
      Serial.println(F("<<< Fail Raw Frame RX: TIMEOUT"));
    }
  }
}

// Configuration write utility (FC16 Write Multiple Registers)
static void writeRegisterValue(uint8_t slave, uint16_t address, uint16_t value) {
  // Morningstar utilizes FC16 (Write Multiple Registers, 0x10) to write EEPROM parameters.
  // Frame structure: Slave, FC 0x10, AddressHI, AddressLO, QuantityHI (0), QuantityLO (1),
  // ByteCount (2), ValueHI, ValueLO, CRCLO, CRCHI
  uint8_t tx[11];
  uint8_t rx[96];
  uint16_t rxLen = 0;

  tx[0] = slave;
  tx[1] = 0x10; // FC16 Write Multiple Registers
  tx[2] = (uint8_t)(address >> 8);
  tx[3] = (uint8_t)(address & 0xFF);
  tx[4] = 0x00;
  tx[5] = 0x01; // qty = 1
  tx[6] = 0x02; // byte count = 2
  tx[7] = (uint8_t)(value >> 8);
  tx[8] = (uint8_t)(value & 0xFF);
  uint16_t crc = crc16Modbus(tx, 9);
  tx[9] = (uint8_t)(crc & 0xFF);
  tx[10] = (uint8_t)(crc >> 8);

  Serial.println();
  Serial.print(F(">>> Write Parameter EEPROM TX: "));
  printHexBuf(tx, 11);
  Serial.println();

  if (executeTransaction(tx, 11, rx, rxLen, LISTEN_WINDOW_DEFAULT_MS)) {
    Serial.print(F("<<< Write Parameter EEPROM RX: "));
    printHexBuf(rx, rxLen);
    Serial.println();
    if (rx[1] == 0x10) {
      Serial.println(F("SUCCESS: Register write confirmed by module."));
    }
  } else {
    Serial.println(F("Error: Failed to write parameter. Timeout or CRC mismatch."));
  }
}

// Sniff physical EIA485 bus
static void runPassiveSniff(uint32_t listenMs) {
  Serial.print(F("\nStarting Passive RX Sniffer for "));
  Serial.print(listenMs);
  Serial.println(F(" ms. Bus is passive (no Opta transmission allowed)."));

  RS485.receive();
  uint32_t deadline = millis() + listenMs;
  uint32_t byteCount = 0;

  while ((int32_t)(millis() - deadline) < 0) {
    if (RS485.available()) {
      int b = RS485.read();
      if (b >= 0) {
        if (byteCount > 0 && byteCount % 16 == 0) {
          Serial.println();
        } else if (byteCount > 0) {
          Serial.print(' ');
        }
        printHexByte((uint8_t)b);
        byteCount++;
      }
    }
  }
  RS485.noReceive();

  Serial.println();
  Serial.print(F("Sniff complete. Captured "));
  Serial.print(byteCount);
  Serial.println(F(" total bytes."));
}

void printHelp() {
  Serial.println(F("\n================= OPTAVIEW DEMO CLI ================="));
  Serial.println(F("Available Commands:"));
  Serial.println(F("  status                     - Print configuration and stats"));
  Serial.println(F("  probe                      - Run the slave ID & baud probe wizard"));
  Serial.println(F("  read [slave]               - Parse live charge controller values (default slave=1)"));
  Serial.println(F("  write <addr> <val> [slave] - Update a config register using FC16 (0x10)"));
  Serial.println(F("  raw <slave> <fc> <addr> <qty>"));
  Serial.println(F("                             - Send manual raw frame to controller"));
  Serial.println(F("  sniff <ms>                 - Passive bus RX monitoring (no queries sent)"));
  Serial.println(F("  wires                      - Print the Morningstar terminal polarity table"));
  Serial.println(F("  help                       - Displays this helper overview"));
  Serial.println(F("====================================================="));
}

void setup() {
  Serial.begin(115200);
  uint32_t s0 = millis();
  while (!Serial && millis() - s0 < 3000) delay(10);

  // Initialize transceiver interface
  RS485.begin(9600, SERIAL_8N2);
  RS485.setDelays(0, 1200); // the formula post-delay fix
  delay(20);

  Serial.println();
  Serial.println(F("====================================================="));
  Serial.println(F("             Welcome to OptaView Demo v0.1           "));
  Serial.println(F("        Hardened Modbus Turnaround Implementation    "));
  Serial.println(F("====================================================="));
  printHelp();
}

void parseCommand(const char *cmdLine) {
  char line[80];
  strlcpy(line, cmdLine, sizeof(line));

  char *argv[8];
  int argc = 0;
  char *token = strtok(line, " \t\r\n");
  while (token != NULL && argc < 8) {
    argv[argc++] = token;
    token = strtok(NULL, " \t\r\n");
  }

  if (argc == 0) return;

  const char *cmd = argv[0];

  if (strcmp(cmd, "help") == 0) {
    printHelp();
  }
  else if (strcmp(cmd, "status") == 0) {
    Serial.println(F("\n--- Console Parameters ---"));
    Serial.println(F("  Active Device: Opta (EIA-485 via Serial2)"));
    Serial.println(F("  Pre-delay: 0 us"));
    Serial.println(F("  Post-delay turnaround: 1200 us (locked for stop-bit survival)"));
    Serial.println(F("  Default Profile: SunSaver MPPT 12V (Morningstar)"));
  }
  else if (strcmp(cmd, "wires") == 0) {
    printWireSwapWarning();
  }
  else if (strcmp(cmd, "probe") == 0) {
    runProbeWizard();
  }
  else if (strcmp(cmd, "read") == 0) {
    uint8_t slave = 1;
    if (argc > 1) {
      slave = (uint8_t)strtol(argv[1], NULL, 0);
    }
    runLiveMonitor(slave);
  }
  else if (strcmp(cmd, "write") == 0) {
    if (argc < 3) {
      Serial.println(F("Error: Syntax 'write <addr> <val> [slave]'"));
      return;
    }
    uint16_t addr = (uint16_t)strtol(argv[1], NULL, 0);
    uint16_t val = (uint16_t)strtol(argv[2], NULL, 0);
    uint8_t slave = 1;
    if (argc > 3) {
      slave = (uint8_t)strtol(argv[3], NULL, 0);
    }
    writeRegisterValue(slave, addr, val);
  }
  else if (strcmp(cmd, "raw") == 0) {
    if (argc < 5) {
      Serial.println(F("Error: Syntax 'raw <slave> <fc> <addr> <qty>'"));
      return;
    }
    uint8_t slave = (uint8_t)strtol(argv[1], NULL, 0);
    uint8_t fc = (uint8_t)strtol(argv[2], NULL, 0);
    uint16_t addr = (uint16_t)strtol(argv[3], NULL, 0);
    uint16_t qty = (uint16_t)strtol(argv[4], NULL, 0);
    sendRawFrame(slave, fc, addr, qty);
  }
  else if (strcmp(cmd, "sniff") == 0) {
    uint32_t ms = 5000;
    if (argc > 1) {
      ms = (uint32_t)strtol(argv[1], NULL, 0);
    }
    runPassiveSniff(ms);
  }
  else {
    Serial.print(F("Command not recognized: '"));
    Serial.print(cmd);
    Serial.println(F("'. Type 'help' for command overview."));
  }
}

void loop() {
  static char buffer[80];
  static int bufIndex = 0;

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (bufIndex > 0) {
        buffer[bufIndex] = '\0';
        parseCommand(buffer);
        bufIndex = 0;
      }
    } else {
      if (bufIndex < (int)sizeof(buffer) - 1) {
        buffer[bufIndex++] = c;
      }
    }
  }

  // Brief loop yield to support Core OS scheduling
  delay(1);
}
