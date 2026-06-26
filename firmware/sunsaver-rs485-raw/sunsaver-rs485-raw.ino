/**
 * sunsaver-rs485-raw.ino
 *
 * Raw RS-485 byte-level diagnostic for the SunSaver MPPT bring-up.
 *
 * Bypasses ArduinoModbus entirely. Manually crafts a Modbus RTU
 * "Read Holding Registers" query (function 0x03) for slave 1, register 0x0012
 * (battery voltage), and then dumps EVERY raw byte received from the bus
 * for ~800 ms. This isolates electrical/protocol issues from library issues.
 *
 * If we see bytes returned -> the SunSaver is alive and the wires work;
 * issue is framing/parity/timing in the Modbus library.
 * If we see ZERO bytes -> the request is not reaching the SunSaver, or
 * the SunSaver is not responding (slave ID, baud, wiring, or transceiver
 * direction all suspect).
 */

#include <ArduinoRS485.h>

static const uint32_t BAUD_RATE = 9600;
static const uint8_t  SLAVE_ID  = 1;
static const uint16_t REG_ADDR  = 0x0012;  // Battery voltage (slow filter, per legacy code)
static const uint16_t REG_COUNT = 1;
static const uint32_t POLL_INTERVAL_MS = 3000;
static const uint32_t LISTEN_WINDOW_MS = 800;

// Register sweep -- probe documented SunSaver MPPT registers to determine
// which actually carry live data on this unit. Two goals:
//
//   (1) Confirm the live-telemetry block at 0x0008..0x000C (adc_*_f) — these
//       are already in production after the 2026-04-22 fix.
//   (2) Identify which power-management candidate registers (filtered batt I,
//       hourmeter, daily Ah charged/loaded, status, faults, alarms) actually
//       respond on this unit. The 2026-04-22 production capture showed
//       cs=Unknown and faults=0x4235 (implausible), so the previously-assumed
//       status register addresses 0x002B/0x002C/0x002E are suspect on this
//       firmware revision and need bench verification.
//
// All probes use FC04 (input registers). Per the SunSaver MPPT PDU, holding
// vs input mapping is identical for read-only registers; FC04 has worked
// consistently on this unit so we stick with it.
struct RegProbe { uint16_t addr; uint8_t count; const char *name; };
static const RegProbe kProbes[] = {
  // --- Confirmed live (already in production) ---
  { 0x0008, 1, "adc_vb_f (batt V)"          },
  { 0x0009, 1, "adc_va_f (array V)"         },
  { 0x000A, 1, "adc_vl_f (load V)"          },
  { 0x000B, 1, "adc_ic_f (charge I)"        },
  { 0x000C, 1, "adc_il_f (load I)"          },

  // --- Power-management candidates (bench verify) ---
  { 0x000F, 1, "vb_f_1m (1-min batt V)"     },
  { 0x0010, 1, "Ib_f_1m (1-min batt I)"     },  // signed: net battery current
  { 0x0011, 1, "vb_min_legacy?"             },  // was wrong-mapped previously
  { 0x0015, 1, "Ahc_daily (Ah charged)"     },
  { 0x0016, 1, "Ahl_daily (Ah load)"        },
  { 0x0018, 2, "hourmeter (32-bit hours)"   },  // two contiguous regs, lo/hi
  { 0x002D, 1, "power_out (W)"              },

  // --- Status block (currently suspect on this firmware) ---
  { 0x001B, 1, "T_hs (heatsink C)"          },
  { 0x001C, 1, "T_batt (batt C from RTS)"   },
  { 0x002B, 1, "charge_state @ 0x2B"        },
  { 0x002C, 1, "faults @ 0x2C"              },
  { 0x002E, 1, "alarms @ 0x2E"              },
  { 0x002F, 1, "load_state @ 0x2F"          },
  { 0x0030, 1, "Ilh_max_daily (peak load)"  },

  // --- Daily-stats block alternate addresses ---
  { 0x003D, 1, "vb_min_daily @ 0x3D"        },
  { 0x003E, 1, "vb_max_daily @ 0x3E"        },
  { 0x0034, 1, "Ah_daily @ 0x34"            },

  // --- MPPT diagnostics ---
  { 0x0033, 1, "sweep_pmax (last MPPT W)"   },
  { 0x0034, 1, "sweep_vmp"                  }
};
static const uint8_t kProbeCount = sizeof(kProbes) / sizeof(kProbes[0]);

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

static void sendQueryAndDump(const char *label, uint8_t slaveId, uint8_t fn,
                             uint16_t addr, uint16_t count) {
  uint8_t frame[8];
  frame[0] = slaveId;
  frame[1] = fn;
  frame[2] = (uint8_t)(addr >> 8);
  frame[3] = (uint8_t)(addr & 0xFF);
  frame[4] = (uint8_t)(count >> 8);
  frame[5] = (uint8_t)(count & 0xFF);
  uint16_t crc = crc16Modbus(frame, 6);
  // Modbus CRC is little-endian on the wire
  frame[6] = (uint8_t)(crc & 0xFF);
  frame[7] = (uint8_t)(crc >> 8);

  Serial.print(F("\n>>> ")); Serial.print(label);
  Serial.print(F(" TX ["));
  for (uint8_t i = 0; i < 8; ++i) {
    if (i) Serial.print(' ');
    printHexByte(frame[i]);
  }
  Serial.println(F("]"));

  // Forum-validated TX bracket (Arduino forum thread 1421875 post #18):
  //   noReceive + beginTransmission + write + flush + delay(1) + endTransmission + receive
  // The flush() ensures the UART software buffer drains, and the post-delay
  // configured via RS485.setDelays() guarantees DE stays high until the last
  // byte has physically left the transceiver.
  RS485.noReceive();
  RS485.beginTransmission();
  RS485.write(frame, 8);
  RS485.flush();
  delay(1);
  RS485.endTransmission();

  // Listen for any bytes
  Serial.print(F("<<< RX ("));
  Serial.print(LISTEN_WINDOW_MS);
  Serial.print(F("ms): "));
  RS485.receive();
  uint32_t deadline = millis() + LISTEN_WINDOW_MS;
  uint16_t rxCount = 0;
  while (millis() < deadline) {
    if (RS485.available()) {
      int b = RS485.read();
      if (b >= 0) {
        if (rxCount > 0) Serial.print(' ');
        printHexByte((uint8_t)b);
        ++rxCount;
      }
    }
  }
  RS485.noReceive();
  Serial.print(F("  [")); Serial.print(rxCount); Serial.println(F(" bytes]"));
}

void setup() {
  Serial.begin(115200);
  uint32_t s = millis();
  while (!Serial && millis() - s < 3000) delay(10);

  Serial.println();
  Serial.println(F("==============================================="));
  Serial.println(F("SunSaver MPPT raw RS-485 byte diagnostic v2"));
  Serial.print  (F("  Baud: ")); Serial.println(BAUD_RATE);
  Serial.println(F("  Sweep: slave IDs 1, 2; FC 0x03 and 0x04"));
  Serial.println(F("  Sweep: parity 8N2 then 8N1"));
  Serial.println(F("==============================================="));
}

static const uint16_t kSerialCfgs[] = { SERIAL_8N2, SERIAL_8N1 };
static const char *   kSerialNames[]= { "8N2",       "8N1"       };
static const uint8_t  kCfgCount     = sizeof(kSerialCfgs) / sizeof(kSerialCfgs[0]);

void loop() {
  static uint32_t cycle = 0;
  static unsigned long lastCycle = 0;
  static unsigned long lastHb = 0;
  unsigned long now = millis();

  if (now - lastHb >= 1000UL) {
    lastHb = now;
    Serial.print(F("hb ms=")); Serial.println(now);
  }

  if (now - lastCycle < POLL_INTERVAL_MS) return;
  lastCycle = now;
  ++cycle;

  // Pick the parity for this cycle (rotate every cycle)
  uint8_t cfgIdx = (cycle - 1) % kCfgCount;
  uint16_t cfg = kSerialCfgs[cfgIdx];
  const char *cfgName = kSerialNames[cfgIdx];

  Serial.println();
  Serial.println(F("------------------------------------------------"));
  Serial.print(F("Cycle ")); Serial.print(cycle);
  Serial.print(F(" using ")); Serial.println(cfgName);
  Serial.println(F("------------------------------------------------"));

  // Re-init RS-485 with the new framing
  RS485.end();
  delay(20);
  RS485.begin(BAUD_RATE, cfg);
  // Forum-validated post-delay = one full character time (forum thread 1421875 post #18).
  // For 9600 baud:
  //   8N1 = 10 bits/char => 10 / 9600 * 1e6 = 1042 us
  //   8N2 = 11 bits/char => 11 / 9600 * 1e6 = 1146 us
  // We use 1200 us as a safe upper bound that covers both framings without
  // having to recompute per-cycle.
  RS485.setDelays(0, 1200);
  delay(20);

  for (uint8_t i = 0; i < kProbeCount; ++i) {
    char label[48];
    snprintf(label, sizeof(label), "%s slv=1 fc=0x04 reg=0x%04X %s",
             cfgName, (unsigned)kProbes[i].addr, kProbes[i].name);
    sendQueryAndDump(label, 1, 0x04, kProbes[i].addr, kProbes[i].count);
    delay(150);
  }
}
