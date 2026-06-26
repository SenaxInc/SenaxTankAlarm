/**
 * sunsaver-modbus-test.ino
 *
 * Minimal SunSaver MPPT Modbus RTU bring-up test for Arduino Opta.
 *
 * Hardware:
 * - Arduino Opta WiFi (AFX00002) or Opta RS485 (AFX00001)
 * - Morningstar MRC-1 MeterBus to RS-485 adapter
 * - SunSaver MPPT (slave ID 1, 9600 baud, 8N2)
 *
 * Wiring (Opta -> MRC-1):
 *   Opta A(-) -> MRC-1 B(-)
 *   Opta B(+) -> MRC-1 A(+)
 *   Opta GND  -> MRC-1 G
 *
 * Output:
 *   Continuous serial heartbeat at 115200 so it is obvious the sketch is alive,
 *   plus one Modbus poll every 3 seconds reporting raw + scaled register values.
 */

#include <ArduinoRS485.h>
#include <ArduinoModbus.h>

// SunSaver MPPT register addresses (0-based for ArduinoModbus).
// Live filtered ADC values are at 0x0008..0x000C (adc_*_f), NOT 0x0010..0x0013.
// Verified by direct bench capture 2026-04-22.
#define SS_REG_BATTERY_VOLTAGE  0x0008  // adc_vb_f: filtered battery voltage
#define SS_REG_ARRAY_VOLTAGE    0x0009  // adc_va_f: filtered array voltage
#define SS_REG_CHARGE_CURRENT   0x000B  // adc_ic_f: filtered charge current
#define SS_REG_LOAD_CURRENT     0x000C  // adc_il_f: filtered load current
#define SS_REG_HEATSINK_TEMP    0x001B  // T_hs   : heatsink temperature (signed)
#define SS_REG_CHARGE_STATE     0x002B  // charge_state
#define SS_REG_FAULTS           0x002C  // faults bitfield
#define SS_REG_ALARMS           0x002E  // alarms bitfield

// Scaling factors per SunSaver MPPT PDU (12V version): V_PU=96.667, I_PU=79.16.
static const float SS_SCALE_VOLTAGE = 96.667f;
static const float SS_SCALE_CURRENT = 79.16f;
static const float SS_SCALE_DIVISOR = 32768.0f;

static const uint8_t  SLAVE_ID  = 1;
static const uint32_t BAUD_RATE = 9600;
static const uint16_t MODBUS_TIMEOUT_MS = 500;
static const uint32_t POLL_INTERVAL_MS  = 3000;

static RS485Class gOptaRS485(Serial2, PB_10, PB_14, PB_13);

static bool gModbusReady = false;
static uint32_t gPollCount = 0;
static uint32_t gOkCount = 0;
static uint32_t gFailCount = 0;

static void printBanner() {
  Serial.println();
  Serial.println(F("============================================"));
  Serial.println(F("SunSaver MPPT Modbus RTU bring-up test"));
  Serial.print  (F("  Slave ID: "));   Serial.println(SLAVE_ID);
  Serial.print  (F("  Baud: "));       Serial.println(BAUD_RATE);
  Serial.print  (F("  Timeout (ms): ")); Serial.println(MODBUS_TIMEOUT_MS);
  Serial.print  (F("  Poll interval (ms): ")); Serial.println(POLL_INTERVAL_MS);
  Serial.println(F("============================================"));
}

static bool readSingleRegister(uint16_t address, uint16_t &outValue) {
  if (!ModbusRTUClient.requestFrom(SLAVE_ID, HOLDING_REGISTERS, address, 1)) {
    return false;
  }
  if (!ModbusRTUClient.available()) {
    return false;
  }
  outValue = (uint16_t)ModbusRTUClient.read();
  return true;
}

static void printScaled(const char *label, uint16_t raw, float scale) {
  float value = ((float)raw * scale) / SS_SCALE_DIVISOR;
  Serial.print(label);
  Serial.print(F(" raw=0x"));
  Serial.print(raw, HEX);
  Serial.print(F(" ("));
  Serial.print(raw);
  Serial.print(F(") -> "));
  Serial.println(value, 3);
}

static const char *chargeStateName(uint8_t cs) {
  switch (cs) {
    case 0: return "START";
    case 1: return "NIGHT_CHECK";
    case 2: return "DISCONNECT";
    case 3: return "NIGHT";
    case 4: return "FAULT";
    case 5: return "BULK";
    case 6: return "ABSORPTION";
    case 7: return "FLOAT";
    case 8: return "EQUALIZE";
    default: return "UNKNOWN";
  }
}

static void doModbusPoll() {
  gPollCount++;
  Serial.println();
  Serial.print(F("--- Poll #")); Serial.print(gPollCount);
  Serial.print(F(" ms=")); Serial.println(millis());

  uint16_t batt = 0, arr = 0, ic = 0, lc = 0, hs = 0, cs = 0, faults = 0, alarms = 0;
  bool okBatt   = readSingleRegister(SS_REG_BATTERY_VOLTAGE, batt);
  bool okArr    = readSingleRegister(SS_REG_ARRAY_VOLTAGE,   arr);
  bool okIc     = readSingleRegister(SS_REG_CHARGE_CURRENT,  ic);
  bool okLc     = readSingleRegister(SS_REG_LOAD_CURRENT,    lc);
  bool okHs     = readSingleRegister(SS_REG_HEATSINK_TEMP,   hs);
  bool okCs     = readSingleRegister(SS_REG_CHARGE_STATE,    cs);
  bool okFaults = readSingleRegister(SS_REG_FAULTS,          faults);
  bool okAlarms = readSingleRegister(SS_REG_ALARMS,          alarms);

  bool allOk = okBatt && okArr && okIc && okLc && okHs && okCs && okFaults && okAlarms;
  if (allOk) {
    gOkCount++;
    Serial.println(F("  Modbus: OK (all 8 registers read)"));
    printScaled("  Battery V    ", batt, SS_SCALE_VOLTAGE);
    printScaled("  Array V      ", arr,  SS_SCALE_VOLTAGE);
    printScaled("  Charge A     ", ic,   SS_SCALE_CURRENT);
    printScaled("  Load A       ", lc,   SS_SCALE_CURRENT);
    Serial.print(F("  Heatsink C   raw=")); Serial.println((int16_t)hs);
    Serial.print(F("  Charge state raw=")); Serial.print(cs & 0xFF);
    Serial.print(F(" -> ")); Serial.println(chargeStateName(cs & 0xFF));
    Serial.print(F("  Faults       0x")); Serial.println(faults, HEX);
    Serial.print(F("  Alarms       0x")); Serial.println(alarms, HEX);
  } else {
    gFailCount++;
    Serial.print(F("  Modbus: FAIL (per-register ok flags) batt=")); Serial.print(okBatt);
    Serial.print(F(" arr="));    Serial.print(okArr);
    Serial.print(F(" ic="));     Serial.print(okIc);
    Serial.print(F(" lc="));     Serial.print(okLc);
    Serial.print(F(" hs="));     Serial.print(okHs);
    Serial.print(F(" cs="));     Serial.print(okCs);
    Serial.print(F(" faults=")); Serial.print(okFaults);
    Serial.print(F(" alarms=")); Serial.println(okAlarms);
    Serial.print(F("  Last Modbus error: "));
    Serial.println(ModbusRTUClient.lastError());
  }

  Serial.print(F("  Cumulative: ok="));
  Serial.print(gOkCount);
  Serial.print(F(" fail="));
  Serial.println(gFailCount);
}

void setup() {
  Serial.begin(115200);
  // Wait briefly for USB-CDC enumeration
  unsigned long startWait = millis();
  while (!Serial && (millis() - startWait) < 3000) {
    delay(10);
  }

  printBanner();

  // SunSaver MPPT uses 9600 8N2 (2 stop bits) per Morningstar spec.
  // ArduinoModbus default is SERIAL_8N1 which causes silent framing errors.
  Serial.println(F("Calling ModbusRTUClient.begin(9600, SERIAL_8N2)..."));
  if (!ModbusRTUClient.begin(gOptaRS485, BAUD_RATE, SERIAL_8N2)) {
    Serial.println(F("FATAL: ModbusRTUClient.begin() failed"));
    gModbusReady = false;
  } else {
    ModbusRTUClient.setTimeout(MODBUS_TIMEOUT_MS);
    // Opta RS485 post-TX delay must be >= one full character time at this
    // baud, or the last byte of the query is corrupted on the wire and the
    // SunSaver silently rejects it. See Arduino forum thread #1421875 post #18.
    // 1042 us = 10/9600 * 1e6 (8N1); 1146 us = 11/9600 * 1e6 (8N2).
    // Use 1200 us as a safe upper bound covering both framings.
    gOptaRS485.setDelays(0, 1200);
    Serial.println(F("ModbusRTUClient.begin() OK (8N2, postDelay=1200us)"));
    gModbusReady = true;
  }
}

void loop() {
  static unsigned long lastHeartbeat = 0;
  static unsigned long lastPoll = 0;
  unsigned long now = millis();

  if (now - lastHeartbeat >= 1000UL) {
    lastHeartbeat = now;
    Serial.print(F("hb ms="));
    Serial.print(now);
    Serial.print(F(" modbus="));
    Serial.println(gModbusReady ? F("ready") : F("DOWN"));
  }

  if (gModbusReady && (now - lastPoll >= POLL_INTERVAL_MS)) {
    lastPoll = now;
    doModbusPoll();
  }
}
