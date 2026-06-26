# OptaView: Morningstar MSView-on-Opta (Demo v0.1)

This folder contains the **OptaView** first demo version, built as a field-portable replacement for Morningstar's Windows-only `MSView` configuration and diagnostic utility. It turns an Arduino Opta into a standalone Modbus RTU tool designed specifically to interface with the SunSaver MPPT (via the MRC-1 bridge) or any other Morningstar charge controller.

## 1. Directory Structure

```text
OptaView/
├── README.md                           # This documentation file
└── OptaView.ino                        # Self-contained OptaView firmware and interactive console
```

## 2. Key Features Implemented in Demo v0.1

### 2.1 The Hardened TX Bracket Layer
To address the infamous Arduino Opta RS-485 last-byte corruption issue (where the transceiver toggles direction before the last stop bit completes), OptaView enforces the forum-proven TX turnaround bracket:
1. **Explicit Delays**: Calls `RS485.setDelays(0, 1200)` at initialization. The 1200 microsecond post-delay ensures the driver stays active for at least one character time at 9600 baud.
2. **Specialized Bracket**:
   ```cpp
   RS485.noReceive();
   RS485.beginTransmission();
   RS485.write(frame, length);
   RS485.flush();
   delay(1);
   RS485.endTransmission();
   RS485.receive();
   ```

### 2.2 First-Time Probe Wizard
An automated sequence sweeping baud rates, parity, and slave IDs to identify any Morningstar device. It outputs the correct physical-layer guidelines if communication fails.

### 2.3 Interactive Command Console
Exposes a clean interactive CLI over USB Serial at **115200 baud** to operate the device without requiring a Windows PC or Morningstar's proprietary software. Supported commands:
* `status` - Show stats and current port parameters
* `probe` - Run first-time probe wizard to detect a controller
* `read` - Poll and display live telemetry registers (0x0008 down to 0x000C)
* `write <addr> <val>` - Write a configuration register using FC16 (Write Multiple Registers)
* `raw <slave> <fc> <addr> <count>` - Send a custom manual Modbus frame
* `sniff <ms>` - Put the transceiver in passive sniff mode to inspect bus traffic
* `help` - Print command help

---

## 3. Physical Interface Guidelines

When connecting the Arduino Opta to the Morningstar MRC-1:

### 3.1 Polarity Cross-over
Modern RS-485 adapters and the Arduino Opta use standard inverting/non-inverting markings. However, Morningstar uses legacy conventions. **You must cross wires A and B**:
* **Opta A (DATA+)** $\longleftrightarrow$ **MRC-1 Terminal B (DATA+)**
* **Opta B (DATA-)** $\longleftrightarrow$ **MRC-1 Terminal A (DATA-)**
* **Opta GND** $\longleftrightarrow$ **MRC-1 Terminal G**

### 3.2 Troubleshooting MRC-1 LED States
* **Steady Green:** Power is OK, but the bus is completely idle. No data is reaching the transceiver.
* **Steady Amber/Orange:** A/B line polarity is physically reversed. Swap links.
* **Flickering Amber/Green:** Active bidirectional communication on the bus.

---

## 4. Compilation and Flashing

To compile and upload the secure (signed) demo version to the Opta:

```powershell
arduino-cli compile --upload -p COM4 --fqbn arduino:mbed_opta:opta:security=sien OptaView\OptaView.ino
```
