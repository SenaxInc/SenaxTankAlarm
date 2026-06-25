# Code Review: Battery Voltage Sampling and Reporting Inconsistencies

## Date: June 25, 2026
## Analyst: GitHub Copilot

---

## 1. Executive Summary

This review analyzes the client firmware [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino) for architectural and logical inconsistencies regarding battery voltage sampling and reporting. 

Per the hardware specification, the system is deployed on the **Blues Wireless for Opta Carrier**, which contains a physical hardware limitation: the Notecard's V+ rail is supplied through the carrier's regulated 5V DC-DC converter. Consequently, the Notecard's internal `card.voltage` API reads this regulated 5V rail and **can never represent the actual 12V battery voltage**. Using `card.voltage` as a battery health source is architecturally invalid for this hardware and results in reporting a flat ~4.69V, leading to false critical-hibernate triggers, unpowered sensors, false alarms, and 0.0 PSI readings.

The product guidelines state that the battery voltage must **only be sampled by**:
1. The **SunSaver Solar Battery Charger** via Modbus RS-485 (`"mppt"` source).
2. An optional **Analog Vin Resistor Voltage Divider** on one of the Opta's analog pins (`"vin-divider"` source).

This code review documents five major inconsistencies/vulnerabilities identified in the client firmware and details the corrective actions required to fully align the codebase with the hardware architecture.

---

## 2. Inconsistencies & Vulnerabilities Identified

### Inconsistency A: Dead-Code Retainers for Notecard Voltage
**Location:** [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L9652) (declaration on [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L1391))

The function `readNotecardVinVoltage` remains fully implemented in the client code:
```cpp
static float readNotecardVinVoltage() {
  J *req = notecard.newRequest("card.voltage");
  ...
```
* **Issues:**
  1. It is **completely unused and dead code** in the client project.
  2. It contains **no compile guards** (such as `#if !defined(ARDUINO_OPTA)`) and will make real, uncalibrated, and erroneous Notecard I2C calls if resurrected.
  3. Its existence creates developer confusion and risks future accidental integration.
* **Remedy:** Completely delete both the declaration and the implementation of this function.

---

### Inconsistency B: Missing Compile Guard in `configureBatteryMonitoring`
**Location:** [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L6833) (called from [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L1696))

The boot sequence (`setup()`) calls `configureBatteryMonitoring()` if `gConfig.batteryMonitor.enabled` is true:
```cpp
static void configureBatteryMonitoring(const BatteryConfig &cfg) {
  J *req = notecard.newRequest("card.voltage");
  ...
```
* **Issues:**
  1. Although the corresponding poller `pollBatteryVoltage` is gated out with `#if defined(ARDUINO_OPTA)`, `configureBatteryMonitoring` **is not gated**.
  2. If a server configuration update enables `batteryMonitor` (which defaults to true when battery type is set to anything other than "none" on the dashboard), the client executes a redundant `card.voltage` config request to configure charge thresholds (`usb:%, high:%, ...`) on the 5V regulated rail.
  3. This is an unnecessary I2C bus write that risks bus contention.
* **Remedy:** Wrap the body or calls of `configureBatteryMonitoring` in a `#if !defined(ARDUINO_OPTA) && !defined(ARDUINO_ARCH_MBED)` compile guard.

---

### Inconsistency C: Redundant Daily Report Call to `appendBatteryDataToDaily`
**Location:** [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7891) (source in [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7091))

The function `sendDailyReport()` still invokes `appendBatteryDataToDaily(doc)`:
```cpp
      appendSolarDataToDaily(doc);
      appendBatteryDataToDaily(doc);  // Notecard battery voltage monitoring
```
* **Issues:**
  1. If `batteryMonitor` config is enabled, this function runs. It immediately realizes `!gBatteryData.valid` and calls `pollBatteryVoltage()`.
  2. Because `pollBatteryVoltage()` is compile-gated to return `false` on Opta, this call will always fail.
  3. However, calling `appendBatteryDataToDaily()` wastes CPU cycles, adds unnecessary execution logic, and maintains a conceptual link between Opta hardware and Notecard voltage reporting.
* **Remedy:** Gated compile-out of `appendBatteryDataToDaily()` on Opta platforms, or completely deprecate the `battery` block on daily serialization when compiled for Opta/mbed.

---

### Inconsistency D: The "Lower-Of" Selection Hazard in `getEffectiveBatteryVoltage`
**Location:** [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L7596)

When multiple voltage sources are present, `getEffectiveBatteryVoltage()` prioritizes them but selects the **lower** of the readings:
```cpp
  // Source 1: SunSaver MPPT via Modbus RS-485
  if (gSolarManager.isEnabled() && gSolarManager.isCommunicationOk()) {
    const SolarData &solar = gSolarManager.getData();
    if (solar.batteryVoltage > 0.0f) {
      voltage = solar.batteryVoltage;
      hasVoltage = true;
      source = "mppt";
    }
  }
...
  // Source 3: Analog Vin voltage divider
  if (gConfig.vinMonitor.enabled && gVinVoltage > 0.5f) {
    if (!hasVoltage) {
      voltage = gVinVoltage;
      hasVoltage = true;
      source = "vin-divider";
    } else if (gVinVoltage < voltage) {
      voltage = gVinVoltage;
      source = "vin-divider";
    }
  }
```
* **The Hazard:**
  1. The **SunSaver MPPT** (Source 1) is a specialized, industrial, digital charge controller measuring the battery directly and reporting over RS-485 via Modbus. It is highly accurate.
  2. The **Vin Voltage Divider** (Source 3) is a pair of resistors connected to the Opta's 12-bit ADC pin. It is highly susceptible to component tolerances, supply noise, temperature coefficients, and calibration drift.
  3. Under the current `min()` logic, if a noisy/uncalibrated Vin divider registers a reading **lower** than the pristine MPPT reading (e.g., noise dips it to 11.9V while the charger reads a solid 12.3V), the system will **override the MPPT value with the noisy Vin divider reading**.
  4. This can trigger premature ECO mode, low-power mode, or even a bogus **CRITICAL_HIBERNATE** transition solely due to ADC noise or calibration tolerance.
* **Remedy:** Modify the selection logic to enforce strict priority without a minimum override if the higher priority source is healthy. Since MPPT is the gold standard, if MPPT is available, its voltage should never be overridden by the less accurate Vin divider.

---

### Inconsistency E: Missing "Live-Zero" Fault Gating in `readAnalogSensor`
**Location:** [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L5304)

Unpowered analog transducers (e.g., 1-5V analog pressure sensors de-energized during sleep or hibernate) drop to 0.0V.
* **Issues:**
  1. `readAnalogSensor` maps the raw voltage lineary using `linearMap` and then clamps negative calculations to 0.0.
  2. Because `0.0` is accepted as a valid reading by `validateSensorReading()`, the unpowered sensor is reported to the dashboard as a valid "0.0 PSI" rather than a sensor fault.
  3. This is inconsistent with current-loop sensors which use `milliamps < CURRENT_LOOP_FAULT_MA` (3.6mA) to return `NAN` when disconnected or unpowered, correctly triggering a `"sensor-fault"`.
* **Remedy:** Implement a live-zero guard in `readAnalogSensor` for analog transducers with declared elevated minimums.

---

## 3. Recommended Remediation & Source Adjustments

The following code corrections are suggested to resolve these inconsistencies.

### Corrective Fix 1: Deprecate Notecard Voltage Sources and Config from Setup and Loop
In [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L1695), compile-gate the call to `configureBatteryMonitoring`:
```cpp
#if !defined(ARDUINO_OPTA) && !defined(ARDUINO_ARCH_MBED)
  if (gConfig.batteryMonitor.enabled) {
    configureBatteryMonitoring(gConfig.batteryMonitor);
    memset(&gBatteryData, 0, sizeof(BatteryData));
    pollBatteryVoltage(gBatteryData, gConfig.batteryMonitor);
  }
#endif
```

Similarly, in [TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino](TankAlarm-112025-Client-BluesOpta/TankAlarm-112025-Client-BluesOpta.ino#L2125), compile-gate out the periodic `pollBatteryVoltage` and `checkBatteryAlerts`:
```cpp
#if !defined(ARDUINO_OPTA) && !defined(ARDUINO_ARCH_MBED)
  if (gConfig.batteryMonitor.enabled && gNotecardAvailable) {
    unsigned long batteryPollInterval = (unsigned long)gConfig.batteryMonitor.pollIntervalSec * 1000UL;
    if (gPowerState >= POWER_STATE_LOW_POWER) {
      batteryPollInterval *= 2;
    }
    if (now - gLastBatteryPollMillis >= batteryPollInterval) {
      gLastBatteryPollMillis = now;
      if (pollBatteryVoltage(gBatteryData, gConfig.batteryMonitor)) {
        if (gPowerState != POWER_STATE_CRITICAL_HIBERNATE) {
          checkBatteryAlerts(gBatteryData, gConfig.batteryMonitor);
        }
      }
    }
  }
#endif
```

---

### Corrective Fix 2: Clean up strict priority in `getEffectiveBatteryVoltage`
Rewrite `getEffectiveBatteryVoltage` to strictly prioritize digital MPPT measurements and never let the noisier ADC resistor divider drag down a healthy charge-controller reading.

```cpp
static float getEffectiveBatteryVoltage() {
  float voltage = 0.0f;
  bool hasVoltage = false;
  const char *source = nullptr;
  
  // Source 1 (Highest Priority): SunSaver MPPT via Modbus RS-485
  if (gSolarManager.isEnabled() && gSolarManager.isCommunicationOk()) {
    const SolarData &solar = gSolarManager.getData();
    if (solar.batteryVoltage > 0.1f) {
      voltage = solar.batteryVoltage;
      hasVoltage = true;
      source = "mppt";
    }
  }
  
  // Source 2 (Medium Priority): Analog Vin voltage divider (only if MPPT is unavailable)
  if (!hasVoltage && gConfig.vinMonitor.enabled && gVinVoltage > 0.5f) {
    voltage = gVinVoltage;
    hasVoltage = true;
    source = "vin-divider";
  }
  
  // Source 3 (Lowest Priority): Notecard (non-Opta ONLY, compile-gated)
#if !defined(ARDUINO_OPTA) && !defined(ARDUINO_ARCH_MBED)
  if (!hasVoltage && gConfig.batteryMonitor.enabled && gBatteryData.valid && gBatteryData.voltage > 0.1f) {
    voltage = gBatteryData.voltage;
    hasVoltage = true;
    source = "notecard";
  }
#endif
  
  gEffectiveVoltageSource = hasVoltage ? source : nullptr;
  return hasVoltage ? voltage : 0.0f;
}
```

---

### Corrective Fix 3: Add Live-Zero Guard to `readAnalogSensor`
Ensures that if an analog pressure sensor is unpowered (under 0.2V when minimum is `>=0.5V`), it returns `NAN` to force a correct `"sensor-fault"` instead of a false `"0.0 PSI"`.

```cpp
  // Map voltage to sensor's native pressure units
  // Live-zero fault guard: if the sensor declares an elevated minimum voltage
  // (e.g., 1.0V for a 1-5V transducer) and the actual reading is far below it,
  // the sensor is unpowered or disconnected — return NAN to trigger sensor-fault.
  if (cfg.analogVoltageMin >= 0.5f && voltage < cfg.analogVoltageMin * 0.2f) {
    return NAN;
  }
```

---

## 4. Conclusion & Action Items

1. **Purge the Dead Code:** Delete `readNotecardVinVoltage` to clean up the code.
2. **Apply Compile Guards and Priority Restructuring:** Deploy Corrective Fixes 1, 2, and 3 to ensure `getEffectiveBatteryVoltage` and other system loops strictly honor hardware limits and accuracy ranking.
3. **Verify Configuration Templates:** Review server-side configuration templates on Notehub to ensure that for all Opta installations, `batteryConfig.enabled` is forced to `false` (or set battery type to `"none"`), relying exclusively on `solarCharger` or `vinMonitor` blocks.
