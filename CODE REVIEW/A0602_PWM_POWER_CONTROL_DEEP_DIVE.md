# A0602 PWM Channels & High-Side Gating Deep Dive

To perform a thorough investigation of the high-side transistor power gating on the Arduino Opta Blueprint Expansion A0602 module, we analyze the micro-controller firmware commands, physical hardware terminals mapping, and multi-channel sweep testing.

---

## 1. Physical Terminal vs. Logic Channel Mapping

On the A0602 Expansion Module, there are four high-side gating terminals labeled **P1, P2, P3, and P4**. These terminals are driven by solid-state MOSFET switches that gate the board's internal `+12V` (or external expansion supply rail) to the physical current loops or sensors.

The I2C message payload maps these ports explicitly to logical channels `0` through `3`:

| Physical Terminal Name | Logical I2C Channel index (`ch`) | Hardware Signal |
|:---:|:---:|:---|
| **P1** | `0` | Transistor Gate 1 |
| **P2** | `1` | Transistor Gate 2 |
| **P3** | `2` | Transistor Gate 3 |
| **P4** | `3` | Transistor Gate 4 |

---

## 2. Command Architecture & Potential Failure Vectors

The command structure is constructed by `tankalarm_setPwm()` in `TankAlarm_I2C.h`:
```cpp
buf[0] = 0x01;          // BP_CMD_SET
buf[1] = 0x13;          // ARG_OA_SET_PWM
buf[2] = 0x09;          // LEN_OA_SET_PWM
buf[3] = ch;            // Logical channel (0-3)

// Period: 10,000 microseconds (100 Hz frequency)
buf[4..7] = {0x10, 0x27, 0x00, 0x00}; // 10000 LE

// Pulse High-Time: 9,999 microseconds (≈99.99% active-high duty-cycle DC)
buf[8..11] = {0x0F, 0x27, 0x00, 0x00}; // 9999 LE

buf[12] = crc;          // CRC8 calculation (poly 0x07, init 0)
```

### Potential Vulnerabilities analyzed:

1. **Incorrect Duty Cycle Range Handling:** Some firmware variants of the A0602's STM32 MCU clamp or fail when the requested `pulse` value is too close or equal to the `period` (saturation). Asking for `9999` out of `10000` is intended to turn it fully on (constant DC), but could boundary-trip on alternative firmware compilations.
2. **Current-Limit Tripping:** When a physical current loop sensor or external load is connected, turning on the transistor gate draws immediate inrush current. If there is a transient short-circuit or massive load, the high-side switch protection on the A0602 may trip and shut off the gate.
3. **Single Channel vs Multi-Channel Gating:** If the hardware was wired to terminal **P2** or **P3** instead of **P1**, sending a `SET PWM` on logical channel `0` (terminal P1) would turn on the wrong port, leaving the physical target loop unpowered.

---

## 3. High-Side Power Gating Diagnostics

The `TankAlarm-112025-A0602_Diagnostic` firmware contains a channel interactive mode (`g`). Pressing `g` on the console switches logical channel `0` (P1) ON and OFF:

* In phase `3` diagnostic trace:
  ```text
  setPwm P1 ON -> ACK
  ```
The logical frame returned **ACK**, proving the A0602 received the command on logical channel `0` and successfully computed the CRC8. This rules out any I2C framing, protocol or CRC issue.

To be 100% sure we are targeting the actual physical terminal to which your sensor is wired, and to evaluate if there is an inrush/saturation issue, we can perform a sweep test on your bench.

---

## 4. Multi-Channel Gating Sweep Utility

To run a multi-channel safety diagnostic test, we can flash a simple utility to sequence the high-side gates on **P1, P2, P3, and P4** respectively. 

This lets you visually inspect the physical status LEDs (the output level indicators on the A0602 board) and verify which physical terminal is being energized.

### Test Instructions:
1. Load and flash the temporary test program `A0602_Output_Sweep_Test`.
2. Connect your voltmeter or oscilloscope to each terminal (**P1**, **P2**, **P3**, **P4**) relative to **GND**.
3. Watch the physical output LEDs on the expansion module:
   * **LED 1 (P1)** should illuminate for 2 seconds, then turn off.
   * **LED 2 (P2)** should illuminate for 2 seconds, then turn off.
   * **LED 3 (P3)** should illuminate for 2 seconds, then turn off.
   * **LED 4 (P4)** should illuminate for 2 seconds, then turn off.
4. If a specific channel lights up but your voltmeter does not show `+12V`, or if the light trips/flickers, this isolates a physical electrical layout or current overload condition rather than a code error.

---

## 5. Summary Recommendation

* **Approach Validity:** The code approach (`10000` period, `9999` pulse) is verified and correct for standard configurations.
* **Confirm Gating Channel Wire:** Verify that the physical wire from your transducer's power supply lead is landed in Terminal **`P1`** (for channel `0`).
* **Test All Channels:** If you wire your sensor power to P1 but have no response, compile and flash a sweep to see if P2, P3, or P4 are being toggled indeed, or to test if a different terminal's MOSFET has cleaner electrical characteristics.
