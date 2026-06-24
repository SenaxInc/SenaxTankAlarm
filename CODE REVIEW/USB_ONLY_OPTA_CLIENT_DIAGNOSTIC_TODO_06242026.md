# USB-Only Diagnostic Todo List for Test Opta Client (Post-v2.0.46)

Date: 2026-06-24
Owner: TankAlarm team
Prepared by: Copilot
Status: Ready to execute

## Goal

Diagnose why v2.0.46 did not resolve the A0602 current-loop issue, using only:
- USB flashing and serial monitor access
- Notehub event observation
- Existing repo test sketches and utilities

No electrical test equipment is required.

## Constraints

- Use the existing hardware stack as wired today (Opta + Blues Wireless for Opta + A0602).
- Do not rely on oscilloscope, DMM, or external bus analyzers.
- Keep all tests reproducible and timestamped.

## Key v2.0.46 signals to use

Track these fields in telemetry/health/diag notes:
- i2c_errs / i2c_cl_err
- cl_ok
- cl_fault
- cl_or
- cl_dur_us
- ev=i2c-recovery

cl_fault code map:
- 0 = last read OK
- 1 = P1 power-gate enable NACK
- 2 = ADC config NACK
- 3 = channel function wrong
- 4 = framed read failed (all samples failed)
- 5 = over-range or 0xFFFF-style garbage rejected

## Test references in this repo

Use these existing sketches and guides during execution:
- TankAlarm-112025-I2C_Utility/TankAlarm-112025-I2C_Utility.ino
- Tutorials/Tutorials-112025/I2C_UTILITY_GUIDE.md
- TankAlarm-112025-Sensor_Utility/TankAlarm-112025-Sensor_Utility.ino
- TankAlarm-112025-Sensor_Utility/Blueprint_CH0_Test/Blueprint_CH0_Test.ino
- TankAlarm-112025-Sensor_Utility/P1_Transistor_Gating_Test/P1_Transistor_Gating_Test.ino
- CODE REVIEW/CODE_REVIEW_06152026_CURRENT_LOOP_MA_READING.md
- CODE REVIEW/CODE_REVIEW_06222026_DEFERRED_ITEMS.md
- release-notes/v2.0.46.md

## Logging protocol (do this first)

- [ ] LP-1 Create a run folder for today logs (serial + screenshots + notes).
- [ ] LP-2 Use serial monitor at 115200 for every sketch.
- [ ] LP-2a After every upload, re-run board list and re-confirm the active COM port before opening monitor.
- [ ] LP-3 Prefix each manual note with timestamp and current sketch name.
- [ ] LP-4 In Notehub Events, keep one window open and capture event timestamps during each phase.
- [ ] LP-5 Record expansion LED state by timestamp (red/yellow pattern) for correlation only.

Note: LED color meaning for this exact red+yellow pattern is not yet confirmed in repo docs. Treat LEDs as correlation data, not root-cause proof.

## Phase A - Production baseline on test client (v2.0.46)

- [ ] A-1 Flash/confirm client firmware is v2.0.46.
- [ ] A-2 Reboot once and record first 5 minutes of serial output.
- [ ] A-3 Observe Notehub for 20-30 minutes with no config changes.
- [ ] A-4 Capture starting and ending values for i2c_cl_err, cl_ok, cl_fault, cl_or, cl_dur_us.
- [ ] A-5 Record count and timing of i2c-recovery events.

Pass/diagnostic criteria:
- Healthy direction: cl_ok rises, i2c_cl_err stable/slow, cl_fault mostly 0.
- Failure signature: cl_fault repeats non-zero and i2c-recovery clusters with sensor-fault.

## Phase B - Notecard path isolation (I2C Utility)

Goal: verify shared-bus Notecard behavior independently from production client logic.

- [ ] B-1 Flash TankAlarm-112025-I2C_Utility.
- [ ] B-2 Run command s (scan bus) and record addresses found.
- [ ] B-3 Run command a (auto-attach Notecard).
- [ ] B-4 Run command d (hub.get, card.version, card.wireless diagnostics).
- [ ] B-5 Run command y (hub.sync).
- [ ] B-6 If attach/diag fails, run command r (reset I2C address to default), then repeat B-2 to B-4.

Pass/diagnostic criteria:
- Notecard stable if attach and d pass repeatedly.
- If Notecard is unstable here, shared-bus reliability is suspect before touching A0602 logic.

## Phase C - A0602 standalone behavior without production loop

Goal: determine whether A0602 path itself is bad, or failure appears only in full production contention.

### C1: Sensor Utility (official OptaBlue path)
- [ ] C1-1 Flash TankAlarm-112025-Sensor_Utility.
- [ ] C1-2 Run s (scan expansions).
- [ ] C1-3 Run c (configure all channels current ADC).
- [ ] C1-4 Set focus channel to CH0 (f).
- [ ] C1-5 Log 5 minutes of CH0 readings.
- [ ] C1-6 Run a (all channels) and capture one snapshot.

### C2: Blueprint_CH0_Test
- [ ] C2-1 Flash Blueprint_CH0_Test.
- [ ] C2-2 Log 5 minutes of CH0-CH7 output (1 second cadence).
- [ ] C2-3 Capture whether CH0 is in expected active range and stable.

### C3: P1_Transistor_Gating_Test
- [ ] C3-1 Flash P1_Transistor_Gating_Test.
- [ ] C3-2 Run at least 10 full gating cycles.
- [ ] C3-3 Record ON success count, OFF success count, and averaged mA per cycle.
- [ ] C3-4 Record whether mA is plausible near known low-pressure expectation or stuck high.

Interpretation checkpoint:
- If C1/C2/C3 all fail: likely hardware/module/wiring state issue independent of production scheduler.
- If C1/C2/C3 pass but Phase A fails: likely shared-bus temporal coupling in production path.

## Phase D - Production stress run (contention amplification)

Goal: test whether failure rate scales with host-initiated bus activity.

- [ ] D-1 Reflash production v2.0.46 client firmware.
- [ ] D-2 Use a temporary test config that increases normal traffic (short sample interval and low report threshold).
- [ ] D-3 Run for 30 minutes with Notehub and serial capture active.
- [ ] D-4 Compare cl_fault distribution and i2c-recovery rate against Phase A baseline.

Interpretation checkpoint:
- If failure rate materially increases under stress, contention remains active despite 2.0.46 isolation.
- If failure pattern is unchanged from idle, likely deterministic per-read fault (config/function/power-gate path).

## Phase E - Fault-code-first triage matrix

Use dominant cl_fault to select likely next fix path:

- [ ] E-1 Dominant cl_fault=1 (PWM_NACK): prioritize P1 gating command/verification path and gate telemetry.
- [ ] E-2 Dominant cl_fault=2 (CONFIG_NACK): prioritize ADC config write timing/ack handling and retry sequencing.
- [ ] E-3 Dominant cl_fault=3 (FUNC_WRONG): prioritize channel-function verification path and channel mode ownership.
- [ ] E-4 Dominant cl_fault=4 (READ_FAIL): prioritize framed read robustness, bus recovery cadence, and shared-bus scheduling.
- [ ] E-5 Dominant cl_fault=5 (OVER_RANGE): prioritize raw-frame garbage rejection diagnostics and over-range context capture.

## Required artifacts before coding next fix

- [ ] R-1 Serial logs from Phase A through D saved with sketch name and timestamps.
- [ ] R-2 Notehub event export/screenshot set showing cl_* and i2c-recovery progression.
- [ ] R-3 One-page summary table of each phase outcome (pass/fail + key metrics).
- [ ] R-4 Dominant fault mode selected from Phase E with evidence.

## One-page result summary template

Device UID:
Firmware:
Date/time window:

Phase A baseline:
- i2c_cl_err start/end:
- cl_ok start/end:
- dominant cl_fault:
- i2c-recovery count:

Phase B I2C Utility:
- addresses found:
- diagnostics pass/fail:

Phase C standalone A0602:
- Sensor Utility verdict:
- Blueprint_CH0 verdict:
- P1 gating test verdict:

Phase D production stress:
- failure rate vs baseline:

Selected next fix path:
- rationale:

## Stop conditions

Stop and escalate before further firmware edits if either is true:
- Standalone A0602 tests are consistently failing on the USB bench.
- Notecard I2C utility diagnostics are unstable in repeated runs.

Those indicate platform/harness instability and can invalidate software-only conclusions.
