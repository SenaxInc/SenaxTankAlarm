# OptaView

OptaView turns an Arduino Opta into a field tool for Morningstar charge controllers: an open
replacement for the Windows-only MSView. It talks Modbus RTU over the Opta's RS-485 port to a
SunSaver MPPT through an MRC-1 adapter, and you drive it from any serial terminal over USB.

**Version 0.2** can find the controller, show its meters and status, list every documented
register, save a snapshot of all of them, and change stored settings such as the low voltage
disconnect, with a check before and a read-back after each write.

> **Status.** Version 0.2 is a rewrite of the June 2026 demo. Its logic is tested on a PC
> against a simulated SunSaver (`tests/host/optaview`), and the sketch is built in CI.
> **It has not been run against a real controller yet.** The section
> [What is confirmed on hardware](#what-is-confirmed-on-hardware) lists what that leaves open,
> and [First session](#first-session) starts with steps that only read.

## What you need

- An Arduino Opta with RS-485 (Opta RS485 or Opta WiFi).
- A Morningstar MRC-1 MeterBus adapter between the Opta and the SunSaver MPPT.
- Libraries: **ArduinoRS485** and **ArduinoJson** (version 7).

### Wiring

| Arduino Opta | MRC-1 | |
|---|---|---|
| A | terminal **B** | crossed on purpose: Morningstar's A is the inverting line, the Opta's A is not |
| B | terminal **A** | |
| GND | terminal G | |

Before suspecting the wiring, check that the MRC-1 power switch is ON (its LED is solid green
either way), that the RJ-11 cable is fully seated at both ends, and that the SunSaver's DIP
switch 4 selects MODBUS instead of MeterBus. The link is 9600 baud, 8N2, slave 1.

The console repeats this with the `wires` command, and `probe` prints it when nothing answers.

## Build and upload

```powershell
arduino-cli lib install ArduinoRS485 ArduinoJson
arduino-cli compile --upload -p COM4 --fqbn arduino:mbed_opta:opta:security=sien OptaView\OptaView.ino
```

`security=sien` builds the signed image that the project's Optas need. An Opta with the stock
bootloader takes the plain `arduino:mbed_opta:opta`.

Open the serial monitor at **115200 baud** and set the line ending to **New Line**. OptaView
greets each time a terminal opens the port. It echoes what you type, so switch local echo off in
terminal programs such as PuTTY.

## First session

The first four steps only read. Nothing is written to the controller until step 6.

1. `probe` looks for the controller: it tries the link settings in use, then the usual
   alternatives, and reports a find only when a register was actually read.
2. `live` shows the meters and the status registers. Compare the battery voltage with a
   multimeter.
3. `dump` reads every register in the profile. Lines marked `~` come from Morningstar's
   specification and have not been seen on our bench, so look for values that make no sense.
4. `snapshot` prints every raw value as JSON. Copy it out of the terminal and keep it: it is the
   record of how the controller was set before you changed anything.
5. `scalecheck 13.42` (with your own multimeter reading) settles the voltage scales. See
   [The voltage scales](#the-voltage-scales). Do this before writing any voltage.
6. `set EV_lvr 13.25`, then `set EV_lvd 13.10`: each shows the old and the new value and waits
   for `yes`. Raising the reconnect voltage first keeps it above the disconnect voltage at
   every step.
7. `reset` restarts the controller so that it uses the new settings, waits for it to come
   back and shows the status again. `dump load` then shows what is stored.

While you change settings, power the Opta from the battery and not from the SunSaver's LOAD
terminals: the specification does not say whether the load output stays on between an EEPROM
write and the reset. A custom LVD and LVR only apply with DIP switch 2 ON, and custom charge
settings only with DIP switch 1 ON.

This is what a session looks like. The text comes from the simulator, until there is a capture
from the bench:

```text
optaview> probe
Looking for Morningstar SunSaver MPPT (SS-MPPT-15L).
Each try reads adc_vb_f (0x0008). Press a key to stop.
  9600 8N2, slave 1, function 4: ok
Found it: slave 1, 9600 8N2, read function 4.
Battery voltage reads 13.68 V.
Identification: vendor "Morningstar Corp.", product "SS-MPPT", version "v01.01.01"
These settings are now in use. Try 'live'.
optaview> set EV_lvr 13.25
EV_lvr: Low voltage reconnect
  now  12.60 V       [0x1021]
  new  13.25 V       [0x10F6]
  Careful: the scale of stored settings (vset, x100/32768) is not confirmed on this controller.
    If 'now' is not the value you expect, stop here and run 'scalecheck'.
  Before you write:
  - Every EEPROM write raises the controller's 'EEPROM setting edit' fault until the controller is
    reset. The specification does not say whether charging or the load output stop in that state.
  - Power this Opta from the battery, not from the SunSaver LOAD terminals, while changing settings.
  - Custom charge settings apply only with DIP switch 1 ON (the battery jumper picks bank 1 or 2).
    Custom LVD and LVR apply only with DIP switch 2 ON.
Type yes to write it. Anything else cancels.
yes/no> yes
Written and read back: EV_lvr = 13.25 V [0x10F6]
The controller uses it after a restart: 'reset' when all your changes are in.
```

## Commands

| Command | What it does |
|---|---|
| `probe` | Try the usual link settings until the controller answers |
| `scan [first] [last]` | Look for slave addresses (1 to 16 unless told otherwise) |
| `id` | Ask the controller to identify itself |
| `wires` | Wiring and switch checklist |
| `live` | Meters and status |
| `watch [seconds]` | Repeat `live` until a key is pressed |
| `dump [group]` | Every register, or one group (`groups` lists them) |
| `get <name>` | One register by name, with its limits and notes |
| `get <address> [count]` | Raw registers from an address |
| `coils` | Coil states |
| `snapshot` | Every raw value as JSON |
| `scalecheck [volts]` | Test the voltage scales, with a multimeter reading if you have one |
| `set <name> <value>` | Write a setting, for example `set EV_lvd 13.10` |
| `set <name> raw <n>` | The same with a raw register value |
| `coil <name> on\|off` | Switch a coil |
| `reset` | Restart the controller so that new settings apply |
| `status` | Link settings and counters |
| `link <baud> [format]`, `slave <n>`, `fc <3\|4>`, `timeout <ms>` | Change the link |
| `blocks auto\|on\|off` | Read registers in blocks or one at a time |
| `scale [name] [x]` | Show the scales, or change one until OptaView restarts |
| `profile` | Where the register map comes from, and its notes |
| `trace on\|off` | Show every request and reply in hex |
| `sniff [ms]` | Listen on the bus without sending |
| `raw <slave> <fc> <address> <count\|value>` | One request as typed |
| `expert on\|off` | Allow `raw` to write |

Long commands stop when you press a key.

## What keeps a session safe

The June demo treated any reply with a valid CRC as success (review finding F-26). Version 0.2
keeps these rules, and the host tests check each of them:

- A reply counts only if its slave address, function, length and CRC all match the request,
  and a write only if the controller repeats the address and value. An exception reply is
  reported as an exception and never as data.
- `probe` reports a controller only after it has read a register. A controller that answers
  with an exception is reported as answering but refusing.
- Settings are written by name, inside limits from the profile, after the old and the new value
  were shown and `yes` was typed. Every write is read back.
- Pairs that must stay in order (reconnect above disconnect) are checked when you `set` one of
  them and again before `reset`.
- Writing to an arbitrary address needs `expert on` first and still asks for `yes`.
- Stored settings and live readings have a voltage scale each. `set` warns until the scale of
  the settings is confirmed, and a multimeter reading never changes how a setting is written.
- Currents are signed, so a small night-time back current reads as -0.04 A and not as 158 A.

## The voltage scales

Morningstar's specification gives every voltage as `raw x 100 / 32768`, except the regulation
target `Vb_ref`, which is `raw x 96.667 / 32768`. The TankAlarm client scales the battery,
array and load voltage by 96.667. The two differ by 3.3 %, which is 0.45 V at 13.5 V, and
that matters when you are about to write a disconnect voltage.

The bench has one comparison so far. On 2026-07-03 the TankAlarm client showed raw 0x1183 as
13.22 V (it is 13.68 V at x100), and that was judged to match what MSView had shown earlier
the same day, before the Opta's link was working. The MSView figure was not written down and
the two readings were not taken together, so this points to x96.667 for live readings without
settling it. Nothing has been measured for stored settings, and the figure need not be the
same one for what the controller measures and for what it stores. So the profile has three
voltage scales, and each can be corrected on its own:

| Scale | Used for | Starts as |
|---|---|---|
| `vlive` | Live readings: battery, array and load voltage, the LVD in effect, minimums and maximums | x100 |
| `vset` | Stored settings: every voltage that `set` can write | x100 |
| `vref` | The regulation target `Vb_ref` | x96.667 |

OptaView starts with the specification's figures and says so until `scalecheck` has settled
them. It tests the first two separately, each on its own evidence:

- **Stored settings.** The setpoints in the controller are round numbers (14.40 V, 11.50 V)
  only at the right scale. This needs no multimeter.
- **Live readings.** A multimeter reading at the controller's battery terminals, given as
  `scalecheck 13.42`, decides. Without one the check can still confirm the scale in use while
  the controller is in absorption or float, because it then holds the battery at `Vb_ref`.

A multimeter says nothing about how settings are stored, and round setpoints say nothing about
the readings, so neither verdict is taken from the other's evidence. This is the simulator
again, behaving as the specification says:

```text
optaview> scalecheck 13.68
Stored settings (scale vset, now x100/32768). Candidates: x100, x96.667.
  1. Setpoints: 13 in use (14 of 14 read). Round settings only look round at the right scale.
       at x100, 13 of 13 are multiples of 0.05 V
       at x96.667, 1 of 13 is a multiple of 0.05 V
     -> x100
Live readings (scale vlive, now x100/32768). Candidates: x100, x96.667.
  2. Regulation: the controller is holding the battery at its target (Vb_ref = 13.70 V).
       Vb_f reads 13.70 V at x100, 13.24 V at x96.667
     -> x100
  3. Multimeter: 13.68 V measured, which with the register value works out to x99.99.
       adc_vb_f reads 13.68 V at x100, 13.23 V at x96.667
     -> x100
Verdict
  Stored settings: x100, which is what OptaView uses now.
  Live readings:   x100, which is what OptaView uses now.
```

When a verdict differs from the scale in use, `scalecheck` prints the command that switches it
for the session, such as `scale vlive 96.667`, and the fix belongs in
`profiles/sunsaver-mppt.json`. If the specification turns out to be right about the live
readings, the TankAlarm client reads its three voltages 3.3 % low.

## What is confirmed on hardware

Confirmed on the project's SS-MPPT-15L through an MRC-1 (April and July 2026): reading
registers 0x0008 to 0x000C one at a time with function 4 at 9600 baud, 8N2, slave 1.

Not confirmed yet, and so the first things to try:

- OptaView 0.2 itself. Only the transmit sequence and the port settings are carried over
  unchanged from sketches that ran on the bench.
- Every register marked `~`, including all the EEPROM settings.
- Reading several registers in one request. OptaView tries it, and if the adapter does not
  answer blocks it falls back to single registers and says so.
- Writing settings with function 6, coils, the reset coil and device identification. They
  follow the specification. The April plan's function 16 is not in the SunSaver's
  specification, so OptaView does not use it.
- Which scales the voltages use (see above).
- Which position of DIP switch 4 is MODBUS. The repository's notes disagree, so the texts only
  say that it selects it.

## The profile

Everything OptaView knows about the SunSaver is data in
[`profiles/sunsaver-mppt.json`](profiles/sunsaver-mppt.json): addresses, types, scales, units,
labels, which registers `set` may write and within which limits, the pair checks, the link
defaults and the wiring notes. A wrong address or scale is a one-line change there.

The sketch carries the profile inside `OptaViewBuiltinProfile.h`. After editing the JSON:

```bash
python3 OptaView/tools/check_profile.py     # lint
python3 OptaView/tools/embed_profile.py     # regenerate OptaViewBuiltinProfile.h
```

The host tests fail when the header is out of date. A register entry looks like this:

```json
{ "addr": "0xE022", "name": "EV_lvd", "label": "Low voltage disconnect", "scale": "vset",
  "group": "load", "writable": true, "min": 9.0, "max": 15.0 }
```

`type` is `u16` (the default), `i16`, `u8`, `i8`, `u32`, `enum` or `bits`. `"verified": "bench"`
marks a register that was seen on our hardware; the others show `~` in the lists. Only
one-word `u16` and `i16` registers can be writable. The limits are OptaView's own sanity limits
for a 12 V system, not Morningstar's.

## Tests

```bash
git clone --depth 1 --branch v7.4.3 https://github.com/bblanchon/ArduinoJson.git /tmp/ArduinoJson
make -C tests/host/optaview test ARDUINOJSON_DIR=/tmp/ArduinoJson/src
```

The suite runs the same headers the sketch compiles against a simulated SunSaver that can drop,
cut, corrupt and delay replies, refuse blocks, answer at another address or speed, and restart.
Run `tests/host/optaview/build/optaview_test_o2` with `OPTAVIEW_TRANSCRIPT=session.txt` set to
keep everything the console printed, which shows what a session looks like without a controller
on the bench.

## Files

| File | |
|---|---|
| `OptaView.ino` | The glue: RS-485 port, USB serial, startup |
| `OptaViewModbus.h` | Modbus RTU master that checks every reply |
| `OptaViewProfile.h` | Profile loader, and raw values to units and back |
| `OptaViewDevice.h` | Block reads with fallback, probe, verified writes, reset, scale check |
| `OptaViewConsole.h` | The commands and the line editor |
| `OptaViewText.h` | Number formatting and parsing without `printf` |
| `OptaViewBuiltinProfile.h` | The profile as embedded text (generated) |
| `profiles/sunsaver-mppt.json` | The SunSaver MPPT register map |
| `tools/` | Profile lint and embedding |

Only `OptaView.ino` needs an Arduino. The headers have no Arduino dependency, which is what
lets the tests run them.

## What comes next

The plan is [`CODE REVIEW/OPTAVIEW_NEW_REPO_DESIGN_04222026.md`](../CODE%20REVIEW/OPTAVIEW_NEW_REPO_DESIGN_04222026.md).
Version 0.2 is its foundation under a serial console. Still to come from it:

1. A first run on the bench, and the corrections to the profile that it brings.
2. The web interface over WiFi or Ethernet: live monitor, settings editor, probe wizard.
3. Profiles and snapshots stored on the Opta, and restore from a snapshot with a diff to
   approve.
4. The logged data (the specification's record layout needs checking on hardware first).
5. Profiles for other Morningstar controllers.

The plan proposes a repository of its own. The folder is self-contained, tests included, so
moving it there later is a copy.
