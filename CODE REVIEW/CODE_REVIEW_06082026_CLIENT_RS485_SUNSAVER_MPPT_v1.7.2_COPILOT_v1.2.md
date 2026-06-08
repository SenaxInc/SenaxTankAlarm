# Code Review: Client RS-485 / SunSaver MPPT Solar Charger Communication System

* **Date:** June 8, 2026  
* **Review Version:** v1.2  
* **Reviewed Firmware:** v1.7.2 / v1.7.3  
* **Hardware Scope:** Arduino Opta RS-485 with Morningstar MeterBus EIA-485 Adapter (MRC-1)  
* **Reviewer:** GitHub Copilot  

---

## Executive Summary

This code review evaluates the execution, timing, logic correctness, and robustness of the Solar Charger Communication engine implemented for the Morningstar SunSaver MPPT charger. The primary objective is to answer whether the communication subsystem between the Arduino Opta and the charger is functional, complete, and if there are any lingering logical or structural gaps.

### Summary Assessment
1. **Is it Functional?** **Yes.** The basic Modbus RTU serial plumbing, including custom UART parameters (8N2 framing), hardware-specific Tx/RX DE/RE driver timing delays, and scaled registers for the real-time electrical telemetry block (Battery Voltage, Panel/Array Voltage, Charge Current, and Load Current) is fully functional and bench-verified.
2. **Is it Complete?** **No, it is incomplete.** While the transport-level communication engine works, the software functionality is severely constrained. In production, we are currently bypassing/disabling the unverified register maps due to address inconsistencies on certain controller firmware revisions. This disables highly valuable diagnostics: faults, hardware battery temperature alerts, heatsink warning alerts, charge state reporting, and the daily Ah throughput stats are all disabled.
3. **Are we missing anything?** Yes. We are missing:
   * **Active Inbound Register Map Verification:** Verification of the unverified register addresses block against a live MSView serial parser to safely re-enable true hardware indicators like RTS sensors or heatsink overtemperature alerts.
   * **Failsafe Telemetry Logic:** Derived logic to dynamically calculate running Daily Voltage Minimums, Daily Voltage Maximums, and Solar Charging States using safe, pre-validated live readings in firmware, preventing placeholder leaks in daily reporting.
   * **Receiver Guarding:** Robust checking of words returned by the Modbus library to prevent cast truncation or -1 conversion voltage spikes.
   * **Loop Latency Shield:** Non-blocking and watchdog-aware fallbacks during multi-query failures.

---

## Source Artifacts Reviewed

The following files represent the scope of this review:
* Common definition schema: [TankAlarm_Solar.h](TankAlarm-112025-Common/src/TankAlarm_Solar.h)
* Charger transport implementation: [TankAlarm_Solar.cpp](TankAlarm-112025-Common/src/TankAlarm_Solar.cpp)
* Client polling and reporting: [TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino)

---

## Severity-Ranked Findings and Code Gaps

### 1. High: Placeholder Telemetry Leak in Daily Reports
* **File Details:** Located in the [appendSolarDataToDaily block](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L6001-L6043).
* **Discussion:**
  Because the real-time daily stats registers are disabled in production and forced to safe defaults in [TankAlarm_Solar.cpp](TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L250-L260), the values written to the daily report are static placeholders:
  * `batteryVoltageMinDaily` and `batteryVoltageMaxDaily` are pinned to the instantaneous current voltage read during the single poll when the report is compiled.
  * `heatsinkTemp` is set to a flat zero.
* **Impact:**
  The server-side database records a flat battery voltage range for the entire day, stating that the battery never rose during peak solar hours and never dropped during the night. Furthermore, logging `ht: 0` implies a freezing heatsink temperature in warm field enclosures.
* **Suggested Patch:**
  Introduce running software state tracking directly inside the [class definition](TankAlarm-112025-Common/src/TankAlarm_Solar.h#L280-L290). Let the software track daily min and max by updating them on every successful poll:
  ```cpp
  if (_data.batteryVoltage < _data.batteryVoltageMinDaily || _data.batteryVoltageMinDaily == 0.0f) {
    _data.batteryVoltageMinDaily = _data.batteryVoltage;
  }
  if (_data.batteryVoltage > _data.batteryVoltageMaxDaily) {
    _data.batteryVoltageMaxDaily = _data.batteryVoltage;
  }
  ```
  Reset these running measurements cleanly in [resetDailyStats](TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L508).

---

### 2. High: Derived No-Charge Alerts are Dead Code
* **File Details:** Enum definition in [TankAlarm_Solar.h](TankAlarm-112025-Common/src/TankAlarm_Solar.h#L190) and evaluation in [checkAlerts](TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L350).
* **Discussion:**
  The important safety warning `SOLAR_ALERT_NO_CHARGE` (designed to detect physical solar panel theft, disconnected array cables, or blown fuses during daylight hours) is never returned by `checkAlerts()`.
* **Impact:**
  Operators cannot monitor solar panel disconnects or wiring issues. Even if the sun is at high noon and the battery drops, no alarm triggers until the battery reaches its critical low shutoff threshold, wasting valuable response time.
* **Suggested Patch:**
  Since real-time array voltage and battery voltage are fully pre-validated, derive this state in software inside the poll cycle:
  ```cpp
  // If array voltage indicates daylight (> 15V) but charge current is flat zero, flag alert
  if (_data.arrayVoltage > 15.0f && _data.chargeCurrent < 0.05f) {
    // Raise a potential panel or fuse fault warning
  }
  ```

---

### 3. Medium: Missing Response Word-Count Verification
* **File Details:** Read routine in [readHoldingRegisters](TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L20-L30).
* **Discussion:**
  The Modbus helpers request a specific register byte count and proceed to read the values sequentially:
  ```cpp
  if (!ModbusRTUClient.requestFrom(slaveId, HOLDING_REGISTERS, startAddress, count)) {
    return false;
  }
  for (uint8_t index = 0; index < count; ++index) {
    buffer[index] = (uint16_t)ModbusRTUClient.read();
  }
  ```
  If `requestFrom()` returns true but the internal serial buffer contains fewer than `count` words (such as due to a framing glitch or sudden link drop), the subsequent `read()` calls will return `-1`. This casts directly to raw `0xFFFF` on the receiving buffer.
* **Impact:**
  A raw reading of `0xFFFF` translates via scaling factors to a voltage exceeding 193V. Because the client has no upper-sanity validation clamp on solar battery measurements, this false reading cascades through `checkAlerts()` and triggers high-voltage alarms or database corruption.
* **Suggested Patch:**
  Add a strict buffer capacity guard before processing the read data:
  ```cpp
  if (ModbusRTUClient.available() < count) {
    return false;
  }
  ```

---

### 4. Medium: Blocking Fallback During Serial Faults Increases Loop Latency
* **File Details:** Falling back with input registers in [readRegistersWithFallback](TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L45-L55).
* **Discussion:**
  To support legacy/alternative register maps, `readRegistersWithFallback` first attempts holding registers using FC03, and on failure tries input registers using FC04.
  
  Each attempt is subject to a hard-clamped 500ms timeout value. When an RS-485 cable is physically severed, a loose wiring connection develops, or a solar unit loses power, every polling cycle forces the client to block matching the cumulative timeout delay (at least 1000ms).
* **Impact:**
  This blocks the single-threaded client main loop for over a second. This delays critical task routines, disrupts time-sensitive Modbus I2C commands, delays emergency digital alarm captures, and could starve the physical watchdog if multiple consecutive Modbus devices fail on the same bus.
* **Suggested Patch:**
  1. Once the model's register map configuration is verified during site startup, cache the working function code (FC03 vs FC04) so subsequent polls run a single, optimized request instead of forcing dual timeout delay loops.
  2. Implement local watchdog kicks around the transaction block inside `readRegisters()` to maintain system health.

---

### 5. Low: Hardcoded Initialization Literals
* **File Details:** Startup verification in [SolarManager::begin](TankAlarm-112025-Common/src/TankAlarm_Solar.cpp#L125).
* **Discussion:**
  While the holding register probe correctly uses the named alias `SS_REG_BATTERY_VOLTAGE`, the input register backup verification hardcodes a raw literal `0x0008` value.
* **Impact:**
  Minor code inconsistency; if register addresses are adjusted, the backup probe code path becomes desynchronized and breaks during startup.
* **Suggested Patch:**
  Standardize both blocks to use the central alias defined in [TankAlarm_Solar.h](TankAlarm-112025-Common/src/TankAlarm_Solar.h#L38).

---

## Highly Optimized Verification Test Plan

To verify that the communication engine runs correctly, execute these bench tests:

1. **Simulated Zero-Response Test (Severed Bus Line):**
   * Physically disconnect the `MRC-1` Rx/B connection. Verify that the client raises a communication failure alert after exactly `SOLAR_COMM_FAILURE_THRESHOLD` consecutive errors, does not publish corrupted variables to the server, and returns the loop delay back to normal.
2. **Buffer Truncation and Framing Check:**
   * Feed truncated bytes into the Rx line. Ensure that the `ModbusRTUClient.available()` guard successfully rejects malformed payloads and returns `false` without storing `0xFFFF` values.
3. **Chemistry Safety Test:**
   * Configure the client web UI for lithium batteries (`BATTERY_TYPE_LIFEPO4`) but toggle the physical DIP switches on the SunSaver to GEL or Sealed. Verify that the client identifies the mismatch and logs a clean warning describing that equalization is disabled or voltages are mismatched.

---

## Conclusion & Action Plan

The RS-485 serial communication system between the Arduino Opta and the Morningstar SunSaver MPPT charger is **partially functional but incomplete.** The physical transport layer and basic electrical block reading are solid, but software alerts and rich telemetry are currently disabled.

### Recommended Implementation Steps:
1. **Apply Failsafe Software Metrics:** Track daily battery voltage maximums and minimums in memory rather than reading the unverified registers.
2. **Re-activate the Panel Alert:** Use array voltage to derive a charging status in software, reviving `SOLAR_ALERT_NO_CHARGE`.
3. **Verify Register Addresses on the Bench:** Hook up the charger to a desktop running MSView, confirm the registers for temperature and alarm bitfields under the current firmware, and re-enable `SOLAR_ENABLE_UNVERIFIED_REGISTERS`.
4. **Implement Modbus Read Guards:** Add word-count verification to prevent bad readings on framing errors.
