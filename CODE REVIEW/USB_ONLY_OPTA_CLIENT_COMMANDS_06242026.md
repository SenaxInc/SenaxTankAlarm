# USB-Only Opta Client Diagnostic Commands (June 24, 2026)

This is the command companion for:
- CODE REVIEW/USB_ONLY_OPTA_CLIENT_DIAGNOSTIC_TODO_06242026.md

## Environment snapshot from this workspace

Detected with Arduino CLI:
- Opta board: arduino:mbed_opta:opta
- Current Opta port: COM4

If port changes, re-run:

```powershell
arduino-cli board list
```

## Variables (PowerShell)

```powershell
$repo = "C:\Users\dorkm\Documents\GitHub\ArduinoSMSTankAlarm"
$fqbn = "arduino:mbed_opta:opta"
$port = "COM4"
```

Important: after each upload, re-run `arduino-cli board list` and update `$port`.
Opta can re-enumerate to a new COM number after DFU upload.

## Optional preflight

```powershell
arduino-cli version
arduino-cli board list
arduino-cli monitor --help
```

## Monitor command (115200)

Open serial monitor with timestamps:

```powershell
arduino-cli monitor -p $port --config 115200 --timestamp
```

Alternative explicit baud key form:

```powershell
arduino-cli monitor -p $port --config baudrate=115200 --timestamp
```

## Phase A / D: Production client firmware (v2.0.46)

### Compile

```powershell
arduino-cli compile --fqbn $fqbn --build-property "build.extra_flags=-DTANKALARM_DFU_MCUBOOT" --libraries "$repo\TankAlarm-112025-Common" "$repo\TankAlarm-112025-Client-BluesOpta\TankAlarm-112025-Client-BluesOpta.ino"
```

### Upload

```powershell
arduino-cli upload -p $port --fqbn $fqbn "$repo\TankAlarm-112025-Client-BluesOpta\TankAlarm-112025-Client-BluesOpta.ino"
```

## Phase B: I2C Utility

### Compile

```powershell
arduino-cli compile --fqbn $fqbn --libraries "$repo\TankAlarm-112025-Common" "$repo\TankAlarm-112025-I2C_Utility\TankAlarm-112025-I2C_Utility.ino"
```

### Upload

```powershell
arduino-cli upload -p $port --fqbn $fqbn "$repo\TankAlarm-112025-I2C_Utility\TankAlarm-112025-I2C_Utility.ino"
```

### Utility interactive commands

In Serial Monitor, use:
- s = scan I2C bus
- a = auto-attach Notecard
- d = diagnostics (hub.get, card.version, card.wireless)
- y = hub.sync
- r = reset Notecard I2C address to default

Reference:
- Tutorials/Tutorials-112025/I2C_UTILITY_GUIDE.md

## Phase C1: Sensor Utility (OptaBlue mode)

### Compile

```powershell
arduino-cli compile --fqbn $fqbn "$repo\TankAlarm-112025-Sensor_Utility\TankAlarm-112025-Sensor_Utility.ino"
```

### Upload

```powershell
arduino-cli upload -p $port --fqbn $fqbn "$repo\TankAlarm-112025-Sensor_Utility\TankAlarm-112025-Sensor_Utility.ino"
```

### Sensor Utility interactive commands

In Serial Monitor, use:
- s = scan expansions
- c = configure all channels as current ADC
- f = set focus channel (use CH0)
- a = print all channels

## Phase C2: Blueprint_CH0_Test

### Compile

```powershell
arduino-cli compile --fqbn $fqbn "$repo\TankAlarm-112025-Sensor_Utility\Blueprint_CH0_Test\Blueprint_CH0_Test.ino"
```

### Upload

```powershell
arduino-cli upload -p $port --fqbn $fqbn "$repo\TankAlarm-112025-Sensor_Utility\Blueprint_CH0_Test\Blueprint_CH0_Test.ino"
```

## Phase C3: P1_Transistor_Gating_Test

### Compile

```powershell
arduino-cli compile --fqbn $fqbn --libraries "$repo\TankAlarm-112025-Common" "$repo\TankAlarm-112025-Sensor_Utility\P1_Transistor_Gating_Test\P1_Transistor_Gating_Test.ino"
```

### Upload

```powershell
arduino-cli upload -p $port --fqbn $fqbn "$repo\TankAlarm-112025-Sensor_Utility\P1_Transistor_Gating_Test\P1_Transistor_Gating_Test.ino"
```

## Quick capture workflow per phase

1. Upload target sketch.
2. Start monitor command with timestamps.
3. Capture 5-30 minute run depending on phase.
4. Save manual notes in:
   - CODE REVIEW/USB_ONLY_OPTA_CLIENT_RUN_LOG_06242026.md

## Notehub events checklist per phase

Capture in Notehub Events for the test device:
- i2c_cl_err / i2c_errs
- cl_ok
- cl_fault
- cl_or
- cl_dur_us
- ev=i2c-recovery

## Troubleshooting commands

Re-detect board/port:

```powershell
arduino-cli board list
```

Quick raw serial-open sanity check for current port:

```powershell
$sp = New-Object System.IO.Ports.SerialPort $port,115200,None,8,one
try { $sp.Open(); 'SERIAL_OPEN_OK'; $sp.Close() } catch { 'SERIAL_OPEN_FAIL ' + $_.Exception.Message }
```

Describe monitor port settings:

```powershell
arduino-cli monitor -p $port --describe
```

If COM4 is busy, close all serial terminals and retry upload.

## Important note

In this Copilot terminal environment, some compile commands may appear with delayed/no textual output. On your local machine, rely on process completion and exit status in your own terminal session.
