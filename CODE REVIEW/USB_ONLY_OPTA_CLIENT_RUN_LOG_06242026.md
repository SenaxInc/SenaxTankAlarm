# USB-Only Opta Client Run Log (June 24, 2026)

Use with:
- CODE REVIEW/USB_ONLY_OPTA_CLIENT_DIAGNOSTIC_TODO_06242026.md
- CODE REVIEW/USB_ONLY_OPTA_CLIENT_COMMANDS_06242026.md

## Session metadata

- Date: 2026-06-24
- Operator: Copilot terminal session
- Device UID: not captured in this session
- Firmware start version: v2.0.46 (test context)
- Opta port: COM4 at baseline, COM5 after I2C Utility upload
- Notehub project: app:f0a8c2c9-6835-49ce-9f7f-1c9540754044 (FIELD)

## Logging setup

- Serial capture started: 2026-06-24 16:28:53 CDT on COM4
- Notehub Events tab started: yes, but full event rows were not retrievable in this session (fresh page load returned 401 Unauthorized)
- Timezone used for timestamps: CDT

## Phase A - Production baseline (v2.0.46)

Start: 2026-06-24 16:28:53 CDT
End: 2026-06-24 16:29:53 CDT

- i2c_cl_err start/end: not captured in serial window
- cl_ok start/end: not captured in serial window
- cl_fault samples: not captured in serial window
- cl_or start/end: not captured in serial window
- cl_dur_us samples: not captured in serial window
- i2c-recovery count: not observed in serial window
- sensor-fault count: 0 observed in serial window

LED correlation notes (timestamped):
- no new LED sample captured during this short baseline window

Serial highlights:
- [2026-06-24 16:28:53] Solar: Modbus communication failure (23 consecutive errors)
- [2026-06-24 16:29:53] Solar: Modbus communication failure (24 consecutive errors)

Verdict:
- [ ] PASS baseline acceptable
- [x] FAIL baseline reproduces issue

## Phase B - I2C Utility (Notecard path isolation)

Start: 2026-06-24 16:31 CDT
End: 2026-06-24 16:36 CDT

I2C scan results:
- addresses found: blocked, no utility command interaction possible

Command results:
- s: not executed, utility serial console unavailable
- a: not executed, utility serial console unavailable
- d: not executed, utility serial console unavailable
- y: not executed, utility serial console unavailable
- r (if used): not executed

USB/port observations during Phase B:
- I2C Utility compile succeeded.
- I2C Utility upload succeeded over DFU (dfu-util completed and reported new upload port).
- After upload, board enumerated as COM5 in arduino-cli board list.
- arduino-cli monitor on COM5 failed repeatedly with: "Invalid serial port".
- Raw .NET serial open on COM5 failed with: "A device attached to the system is not functioning."
- Windows mode COM5 status reported Data Bits = 0 and rejected DATA=8 configuration as unsupported.
- COM3 could be opened but is detected as Unknown and produced no utility interaction data.

Verdict:
- [ ] PASS Notecard path stable
- [x] FAIL Notecard path unstable

## Phase C1 - Sensor Utility (OptaBlue)

Start:
End:

- CH0 reading trend:
- all-channel snapshot notes:
- read failures observed:

Verdict:
- [ ] PASS standalone A0602 read stable
- [ ] FAIL standalone A0602 unstable

## Phase C2 - Blueprint_CH0_Test

Start:
End:

- CH0 observed range:
- CH0 stability notes:
- other channel anomalies:

Verdict:
- [ ] PASS blueprint path healthy
- [ ] FAIL blueprint path unhealthy

## Phase C3 - P1_Transistor_Gating_Test

Start:
End:

- ON success count / total:
- OFF success count / total:
- avg mA by cycle (summary):
- stuck-high signature seen:

Verdict:
- [ ] PASS gating path healthy
- [ ] FAIL gating path unhealthy

## Phase D - Production stress run

Start:
End:

Temporary config used:
- sample interval:
- report threshold:

Comparison versus Phase A:
- i2c_cl_err growth:
- cl_fault mix:
- i2c-recovery frequency:

Verdict:
- [ ] FAIL rate increased under stress
- [ ] FAIL rate unchanged
- [ ] PASS issue not reproduced

## Fault-code-first summary (dominant)

- Dominant cl_fault code:
- Secondary cl_fault code:

Interpretation:
- [ ] 1 PWM_NACK dominant
- [ ] 2 CONFIG_NACK dominant
- [ ] 3 FUNC_WRONG dominant
- [ ] 4 READ_FAIL dominant
- [ ] 5 OVER_RANGE dominant

## Decision

Selected next fix path:
- recover stable USB CDC serial endpoint first (COM5 open failure), then repeat Phase B before any firmware logic changes

Evidence supporting decision:
- DFU upload completed successfully but COM5 could not be opened by either arduino-cli monitor or raw serial API
- repeated COM5 open failures indicate transport/endpoint instability that invalidates deeper Notecard/A0602 conclusions

Do not proceed to firmware edit if stop condition hit:
- [ ] standalone A0602 tests consistently failing
- [x] Notecard I2C utility diagnostics unstable

## Attachments collected

- Serial logs: terminal captures from baseline monitor and Phase B upload/port checks
- Notehub screenshots/export: none collected in this session due authentication limitation on fresh page load
- Additional notes: board list shows COM5 (Opta) present while COM5 open fails with device-not-functioning error
