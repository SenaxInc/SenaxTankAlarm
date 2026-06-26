/**
 * sunsaver-rs485-autoscan.ino
 *
 * Software-only Modbus autoscan for Opta -> MRC-1 -> SunSaver MPPT.
 *
 * Purpose:
 * - Keep current hardware unchanged.
 * - Sweep a focused set of serial profiles, slave IDs, and query targets.
 * - Validate any response with Modbus CRC before reporting success.
 *
 * This avoids repeating broad manual experiments while still probing the
 * remaining plausible protocol settings on the same wiring.
 */

#include <ArduinoRS485.h>

struct SerialProfile {
  uint32_t baud;
  uint16_t config;
  const char *name;
};

struct DelayProfile {
  int preUs;
  int postUs;
  const char *name;
};

struct QueryProfile {
  uint8_t fn;
  uint16_t addr;
  uint16_t count;
  const char *name;
};

static const SerialProfile kSerialProfiles[] = {
  { 9600,  SERIAL_8N2, "9600 8N2" },
  { 9600,  SERIAL_8N1, "9600 8N1" }
};

static const DelayProfile kDelayProfiles[] = {
  { 0,    1200, "pre0/post1200" },
  { 1000, 1200, "pre1000/post1200" },
  { 2000, 1200, "pre2000/post1200" },
  { 3500, 1200, "pre3500/post1200" },
  { 4000, 1200, "pre4000/post1200" }
};

static const uint8_t kSlaveIds[] = { 1 };

static const QueryProfile kQueryProfiles[] = {
  { 0x03, 0x0008, 1, "FC03 reg0008 (filtered batt V)" },
  { 0x04, 0x0008, 1, "FC04 reg0008 (filtered batt V)" }
};

struct ScanStats {
  uint32_t steps;
  uint32_t validResponse;
  uint32_t validException;
  uint32_t noValidFrame;
  uint32_t artifact00;
};

static ScanStats gStats = {0, 0, 0, 0, 0};

static const uint32_t kStepIntervalMs = 1400;
static const uint32_t kListenWindowMs = 900;
static const uint16_t kMaxRxBytes = 96;

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

static void buildRequest(uint8_t *frame, uint8_t slaveId, uint8_t fn,
                         uint16_t addr, uint16_t count) {
  frame[0] = slaveId;
  frame[1] = fn;
  frame[2] = (uint8_t)(addr >> 8);
  frame[3] = (uint8_t)(addr & 0xFF);
  frame[4] = (uint8_t)(count >> 8);
  frame[5] = (uint8_t)(count & 0xFF);
  uint16_t crc = crc16Modbus(frame, 6);
  frame[6] = (uint8_t)(crc & 0xFF);
  frame[7] = (uint8_t)(crc >> 8);
}

static bool parseValidReply(const uint8_t *rx, uint16_t rxLen, uint8_t expectedSlave,
                            uint8_t expectedFn, uint16_t &outReg, bool &outIsException,
                            uint8_t &outExCode, uint16_t &outFrameStart,
                            uint16_t &outFrameLen) {
  outReg = 0;
  outIsException = false;
  outExCode = 0;
  outFrameStart = 0;
  outFrameLen = 0;

  if (rxLen < 5) return false;

  for (uint16_t i = 0; i + 5 <= rxLen; ++i) {
    uint8_t slave = rx[i + 0];
    uint8_t fn = rx[i + 1];

    if (slave != expectedSlave) {
      continue;
    }

    uint16_t frameLen = 0;
    bool isException = false;

    if (fn == (uint8_t)(expectedFn | 0x80)) {
      frameLen = 5;
      isException = true;
    } else if (fn == expectedFn) {
      uint8_t byteCount = rx[i + 2];
      frameLen = (uint16_t)(5 + byteCount);
    } else {
      continue;
    }

    if (i + frameLen > rxLen) {
      continue;
    }

    uint16_t crcCalc = crc16Modbus(&rx[i], frameLen - 2);
    uint16_t crcWire = (uint16_t)rx[i + frameLen - 2] | ((uint16_t)rx[i + frameLen - 1] << 8);
    if (crcCalc != crcWire) {
      continue;
    }

    outFrameStart = i;
    outFrameLen = frameLen;
    outIsException = isException;

    if (isException) {
      outExCode = rx[i + 2];
      return true;
    }

    if (rx[i + 2] >= 2) {
      outReg = (uint16_t)((uint16_t)rx[i + 3] << 8) | (uint16_t)rx[i + 4];
    }

    return true;
  }

  return false;
}

static void flushRxForMs(uint32_t ms) {
  RS485.receive();
  uint32_t deadline = millis() + ms;
  while ((int32_t)(millis() - deadline) < 0) {
    while (RS485.available()) {
      RS485.read();
    }
  }
  RS485.noReceive();
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);

  Serial.println();
  Serial.println(F("================================================"));
  Serial.println(F("SunSaver MPPT Modbus autoscan (Opta + MRC-1)"));
  Serial.println(F("  CRC-qualified response detection enabled"));
  Serial.print(F("  Serial profiles: "));
  Serial.println((int)(sizeof(kSerialProfiles) / sizeof(kSerialProfiles[0])));
  Serial.print(F("  Delay profiles: "));
  Serial.println((int)(sizeof(kDelayProfiles) / sizeof(kDelayProfiles[0])));
  Serial.print(F("  Slave IDs: "));
  Serial.println((int)(sizeof(kSlaveIds) / sizeof(kSlaveIds[0])));
  Serial.print(F("  Query profiles: "));
  Serial.println((int)(sizeof(kQueryProfiles) / sizeof(kQueryProfiles[0])));
  Serial.println(F("================================================"));
}

static void runStep(uint32_t step) {
  const uint8_t serialCount = (uint8_t)(sizeof(kSerialProfiles) / sizeof(kSerialProfiles[0]));
  const uint8_t delayCount = (uint8_t)(sizeof(kDelayProfiles) / sizeof(kDelayProfiles[0]));
  const uint8_t slaveCount = (uint8_t)(sizeof(kSlaveIds) / sizeof(kSlaveIds[0]));
  const uint8_t queryCount = (uint8_t)(sizeof(kQueryProfiles) / sizeof(kQueryProfiles[0]));

  uint32_t total = (uint32_t)serialCount * (uint32_t)delayCount * (uint32_t)slaveCount * (uint32_t)queryCount;
  uint32_t idx = step % total;

  uint8_t serialIdx = (uint8_t)(idx % serialCount);
  idx /= serialCount;
  uint8_t delayIdx = (uint8_t)(idx % delayCount);
  idx /= delayCount;
  uint8_t slaveIdx = (uint8_t)(idx % slaveCount);
  idx /= slaveCount;
  uint8_t queryIdx = (uint8_t)(idx % queryCount);

  const SerialProfile &sp = kSerialProfiles[serialIdx];
  const DelayProfile &dp = kDelayProfiles[delayIdx];
  uint8_t slaveId = kSlaveIds[slaveIdx];
  const QueryProfile &qp = kQueryProfiles[queryIdx];

  // Re-init per step so each tuple is tested in isolation.
  RS485.begin(sp.baud, sp.config);
  RS485.setDelays(dp.preUs, dp.postUs);

  // Clear any stale bytes from previous tuple.
  flushRxForMs(20);

  uint8_t tx[8];
  buildRequest(tx, slaveId, qp.fn, qp.addr, qp.count);

  Serial.println();
  Serial.print(F("STEP ")); Serial.print(step + 1);
  Serial.print(F(" tuple="));
  Serial.print(sp.name);
  Serial.print(F(", "));
  Serial.print(dp.name);
  Serial.print(F(", slave=")); Serial.print(slaveId);
  Serial.print(F(", "));
  Serial.println(qp.name);

  Serial.print(F("  TX: "));
  printHexBuf(tx, 8);
  Serial.println();

  RS485.beginTransmission();
  RS485.write(tx, 8);
  RS485.endTransmission();

  uint8_t rx[kMaxRxBytes];
  uint16_t rxLen = 0;

  RS485.receive();
  uint32_t deadline = millis() + kListenWindowMs;
  while ((int32_t)(millis() - deadline) < 0) {
    while (RS485.available()) {
      int b = RS485.read();
      if (b >= 0) {
        if (rxLen < kMaxRxBytes) {
          rx[rxLen++] = (uint8_t)b;
        }
      }
    }
  }
  RS485.noReceive();

  Serial.print(F("  RX("));
  Serial.print(kListenWindowMs);
  Serial.print(F("ms): "));
  if (rxLen == 0) {
    Serial.print(F("<none>"));
  } else {
    printHexBuf(rx, rxLen);
  }
  Serial.print(F("  [")); Serial.print(rxLen); Serial.println(F(" bytes]"));

  uint16_t reg = 0;
  bool isEx = false;
  uint8_t exCode = 0;
  uint16_t frameStart = 0;
  uint16_t frameLen = 0;
  bool ok = parseValidReply(rx, rxLen, slaveId, qp.fn, reg, isEx, exCode, frameStart, frameLen);

  if (ok) {
    if (isEx) {
      ++gStats.validException;
      Serial.print(F("  VALID MODBUS EXCEPTION frame start="));
      Serial.print(frameStart);
      Serial.print(F(" len="));
      Serial.print(frameLen);
      Serial.print(F(" exCode=0x"));
      printHexByte(exCode);
      Serial.println();
    } else {
      ++gStats.validResponse;
      Serial.print(F("  VALID MODBUS RESPONSE frame start="));
      Serial.print(frameStart);
      Serial.print(F(" len="));
      Serial.print(frameLen);
      Serial.print(F(" regValue=0x"));
      printHexByte((uint8_t)(reg >> 8));
      printHexByte((uint8_t)(reg & 0xFF));
      Serial.print(F(" ("));
      Serial.print(reg);
      Serial.println(F(")"));
    }
  } else {
    if (rxLen == 1 && rx[0] == 0x00) {
      ++gStats.artifact00;
      Serial.println(F("  Note: single 0x00 artifact (not valid Modbus frame)"));
    } else {
      ++gStats.noValidFrame;
      Serial.println(F("  No CRC-valid Modbus frame detected"));
    }
  }

  ++gStats.steps;
}

void loop() {
  static uint32_t step = 0;
  static uint32_t lastStep = 0;
  static uint32_t lastHb = 0;
  static uint32_t completedCycles = 0;

  uint32_t now = millis();
  const uint8_t serialCount = (uint8_t)(sizeof(kSerialProfiles) / sizeof(kSerialProfiles[0]));
  const uint8_t delayCount = (uint8_t)(sizeof(kDelayProfiles) / sizeof(kDelayProfiles[0]));
  const uint8_t slaveCount = (uint8_t)(sizeof(kSlaveIds) / sizeof(kSlaveIds[0]));
  const uint8_t queryCount = (uint8_t)(sizeof(kQueryProfiles) / sizeof(kQueryProfiles[0]));
  const uint32_t totalStepsPerCycle = (uint32_t)serialCount * (uint32_t)delayCount * (uint32_t)slaveCount * (uint32_t)queryCount;

  if (now - lastHb >= 1000UL) {
    lastHb = now;
    Serial.print(F("hb ms="));
    Serial.print(now);
    Serial.print(F(" step="));
    Serial.println(step + 1);
  }

  if (now - lastStep < kStepIntervalMs) {
    return;
  }

  lastStep = now;
  runStep(step);
  ++step;

  if ((step % totalStepsPerCycle) == 0) {
    ++completedCycles;
    Serial.println();
    Serial.println(F("================ CYCLE SUMMARY ================"));
    Serial.print(F("cycle=")); Serial.println(completedCycles);
    Serial.print(F("steps=")); Serial.println(gStats.steps);
    Serial.print(F("valid_response=")); Serial.println(gStats.validResponse);
    Serial.print(F("valid_exception=")); Serial.println(gStats.validException);
    Serial.print(F("artifact_00=")); Serial.println(gStats.artifact00);
    Serial.print(F("no_valid_frame=")); Serial.println(gStats.noValidFrame);
    Serial.println(F("==============================================="));

    gStats.steps = 0;
    gStats.validResponse = 0;
    gStats.validException = 0;
    gStats.noValidFrame = 0;
    gStats.artifact00 = 0;
  }
}
