#!/usr/bin/env python3
"""Checks an OptaView profile for the mistakes that are easy to make by hand.

    python3 OptaView/tools/check_profile.py [profile.json ...]

With no argument it checks every file in OptaView/profiles. The firmware's own loader
(OptaViewProfile.h) rejects a broken profile too; this script says more about what is wrong and
also enforces house rules the loader does not care about (ASCII text, addresses in order,
every group declared).
"""

import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
TYPES = {"u16", "i16", "u8", "i8", "u32", "enum", "bits"}
FORMATS = {"8N1", "8N2", "8E1", "8O1"}


def number(value, what, errors):
    """An address or raw value: an int, or a string such as "0xE022"."""
    if isinstance(value, bool):
        errors.append("%s: expected a number, got %r" % (what, value))
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        try:
            return int(value, 16) if value.lower().startswith("0x") else int(value, 10)
        except ValueError:
            pass
    errors.append("%s: expected a number such as 8 or \"0x0008\", got %r" % (what, value))
    return None


def check(path):
    errors = []
    with open(path, "rb") as handle:
        raw = handle.read()
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError as error:
        return ["not plain ASCII (byte %d): the console prints these strings as they are" % error.start]
    try:
        data = json.loads(text)
    except ValueError as error:
        return ["not valid JSON: %s" % error]

    if data.get("schema") != 1:
        errors.append("\"schema\" must be 1")
    if not data.get("profile"):
        errors.append("missing \"profile\" name")

    scales = data.get("scales", {})
    for name, scale in scales.items():
        if not (isinstance(scale.get("num"), (int, float)) and scale["num"] > 0 and
                isinstance(scale.get("den"), (int, float)) and scale["den"] > 0):
            errors.append("scale %s: \"num\" and \"den\" must be numbers above zero" % name)

    labels = data.get("labels", {})
    for name, items in labels.items():
        if not isinstance(items, list) or not 1 <= len(items) <= 32:
            errors.append("labels %s: must be a list of 1 to 32 names" % name)

    groups = [g.get("name") for g in data.get("groups", [])]
    if len(groups) != len(set(groups)):
        errors.append("two groups share a name")

    registers = data.get("registers", [])
    if not registers:
        errors.append("no registers")
    by_name = {}
    spans = []
    previous = -1
    for reg in registers:
        name = reg.get("name")
        if not isinstance(name, str) or not NAME.match(name):
            errors.append("register name %r: use letters, digits and _, not starting with a digit" % (name,))
            continue
        if name.lower() in by_name:
            errors.append("register %s: the name is used twice" % name)
        by_name[name.lower()] = reg
        addr = number(reg.get("addr"), "register %s addr" % name, errors)
        if addr is None:
            continue
        kind = reg.get("type", "u16")
        if kind not in TYPES:
            errors.append("register %s: unknown type %r" % (name, kind))
            continue
        words = 2 if kind == "u32" else reg.get("words", 1) if kind == "bits" else 1
        if words not in (1, 2):
            errors.append("register %s: \"words\" must be 1 or 2" % name)
            continue
        if not 0 <= addr <= 0xFFFF or addr + words > 0x10000:
            errors.append("register %s: address out of range" % name)
        if addr < previous:
            errors.append("register %s: keep the registers in address order" % name)
        previous = addr
        spans.append((addr, addr + words, name))
        if "scale" in reg and reg["scale"] not in scales:
            errors.append("register %s: unknown scale %r" % (name, reg["scale"]))
        if "scale" in reg and kind in ("enum", "bits"):
            errors.append("register %s: an enum or bits register cannot have a scale" % name)
        if "labels" in reg and reg["labels"] not in labels:
            errors.append("register %s: unknown labels %r" % (name, reg["labels"]))
        if kind in ("enum", "bits") and "labels" not in reg:
            errors.append("register %s: an enum or bits register needs \"labels\"" % name)
        if reg.get("group") not in groups:
            errors.append("register %s: group %r is not in \"groups\"" % (name, reg.get("group")))
        if reg.get("order", "hi-lo") not in ("hi-lo", "lo-hi"):
            errors.append("register %s: \"order\" must be \"hi-lo\" or \"lo-hi\"" % name)
        if reg.get("verified", "spec") not in ("spec", "bench"):
            errors.append("register %s: \"verified\" must be \"spec\" or \"bench\"" % name)
        if reg.get("writable") and (words != 1 or kind not in ("u16", "i16", "u8")):
            errors.append("register %s: only one-word u16, i16 and u8 registers can be writable" % name)
        if "min" in reg and "max" in reg and reg["min"] > reg["max"]:
            errors.append("register %s: \"min\" is above \"max\"" % name)
        if ("min" in reg or "max" in reg or "zeroOk" in reg) and not reg.get("writable"):
            errors.append("register %s: limits only mean something on a writable register" % name)
        if "sentinel" in reg:
            number(reg["sentinel"].get("raw"), "register %s sentinel raw" % name, errors)
            if not isinstance(reg["sentinel"].get("text"), str):
                errors.append("register %s: the sentinel needs \"text\"" % name)
    spans.sort()
    for (_, end, first), (start, _, second) in zip(spans, spans[1:]):
        if start < end:
            errors.append("registers %s and %s overlap" % (first, second))

    coils = {}
    for coil in data.get("coils", []):
        name = coil.get("name")
        if not isinstance(name, str) or not NAME.match(name):
            errors.append("coil name %r: use letters, digits and _" % (name,))
            continue
        if name.lower() in coils:
            errors.append("coil %s: the name is used twice" % name)
        coils[name.lower()] = coil
        number(coil.get("addr"), "coil %s addr" % name, errors)

    def register(name, what, numeric=False):
        reg = by_name.get(str(name).lower())
        if reg is None:
            errors.append("%s: no register called %r" % (what, name))
        elif numeric and reg.get("type", "u16") in ("enum", "bits"):
            errors.append("%s: %r is not a numeric register" % (what, name))
        return reg

    modbus = data.get("modbus", {})
    if not 1 <= modbus.get("slaveId", 1) <= 247:
        errors.append("modbus.slaveId must be 1 to 247")
    if modbus.get("readFunction", 4) not in (3, 4):
        errors.append("modbus.readFunction must be 3 or 4")
    if not 1 <= modbus.get("maxBlock", 16) <= 125:
        errors.append("modbus.maxBlock must be 1 to 125")
    if modbus.get("format", "8N2").upper() not in FORMATS:
        errors.append("modbus.format must be one of %s" % ", ".join(sorted(FORMATS)))
    if "probeRegister" in modbus:
        register(modbus["probeRegister"], "modbus.probeRegister")
    if "resetCoil" in modbus and str(modbus["resetCoil"]).lower() not in coils:
        errors.append("modbus.resetCoil: no coil called %r" % modbus["resetCoil"])

    for group in data.get("liveGroups", []):
        if group not in groups:
            errors.append("liveGroups: %r is not in \"groups\"" % group)

    for item in data.get("checks", []):
        register(item.get("below"), "check \"below\"", numeric=True)
        register(item.get("above"), "check \"above\"", numeric=True)
        if not item.get("message"):
            errors.append("a check needs a \"message\"")

    errors.extend(check_scale_check(data.get("scaleCheck"), scales, register))
    live_scale = ((data.get("scaleCheck") or {}).get("live") or {}).get("scale")
    for reg in registers:
        if live_scale is not None and reg.get("writable") and reg.get("scale") == live_scale:
            errors.append("register %s: a writable register cannot use scaleCheck.live.scale (%s): "
                          "a multimeter reading would decide how it is written" % (reg.get("name"), live_scale))
    return errors


def one_word(reg):
    return reg.get("type", "u16") != "u32" and reg.get("words", 1) == 1


def check_scale_check(scale_check, scales, register):
    """The voltage scale check: stored settings and live readings, each on its own scale."""
    if scale_check is None:
        return []
    errors = []
    for key in scale_check:
        if key not in ("alternates", "grid", "settings", "live"):
            # The loader ignores entries it does not know, so a misspelt one would switch a test off.
            errors.append("scaleCheck.%s: not a scaleCheck entry (\"alternates\", \"grid\", \"settings\", \"live\")" % key)
    for half, known in (("settings", ("scale", "setpoints")),
                        ("live", ("scale", "meter", "battery", "reference", "state", "regulating"))):
        for key in scale_check.get(half) or {}:
            if key not in known:
                errors.append("scaleCheck.%s.%s: not an entry of scaleCheck.%s" % (half, key, half))
    for alternate in scale_check.get("alternates", []):
        if isinstance(alternate, bool) or not isinstance(alternate, (int, float)) or alternate <= 0:
            errors.append("scaleCheck.alternates: %r is not a number above zero" % (alternate,))
    grid = scale_check.get("grid", 0.05)
    if isinstance(grid, bool) or not isinstance(grid, (int, float)) or grid <= 0:
        errors.append("scaleCheck.grid must be a number above zero")

    settings = scale_check.get("settings")
    live = scale_check.get("live")
    if settings is None and live is None:
        errors.append("scaleCheck: needs \"settings\", \"live\" or both")

    if settings is not None:
        scale = settings.get("scale")
        if scale not in scales:
            errors.append("scaleCheck.settings.scale: no scale called %r" % scale)
        setpoints = settings.get("setpoints", [])
        if len(setpoints) < 4:
            errors.append("scaleCheck.settings.setpoints: the test needs at least 4 setpoints to tell anything")
        for name in setpoints:
            reg = register(name, "scaleCheck.settings.setpoints")
            if reg is None:
                continue
            if reg.get("scale") != scale:
                errors.append("scaleCheck.settings.setpoints: %s does not use scale %s" % (name, scale))
            if not one_word(reg):
                errors.append("scaleCheck.settings.setpoints: %s is not a one-word register" % name)
            if "min" not in reg or "max" not in reg:
                # The limits are how the check tells a setting from a blank EEPROM cell.
                errors.append("scaleCheck.settings.setpoints: %s needs \"min\" and \"max\"" % name)

    if live is not None:
        scale = live.get("scale")
        if scale not in scales:
            errors.append("scaleCheck.live.scale: no scale called %r" % scale)
        if settings is not None and scale == settings.get("scale"):
            errors.append("scaleCheck.live.scale and scaleCheck.settings.scale must be two scales: "
                          "a multimeter reading must not decide how settings are written")
        if "meter" not in live:
            # The regulation test only points, so without it nothing could confirm the scale.
            errors.append("scaleCheck.live.meter: needed, because only a multimeter reading settles the live scale")
        for key in ("meter", "battery"):
            if key in live:
                reg = register(live[key], "scaleCheck.live.%s" % key)
                if reg is not None and (reg.get("scale") != scale or not one_word(reg)):
                    errors.append("scaleCheck.live.%s must be a one-word register that uses scale %s" % (key, scale))
        if "reference" in live:
            reg = register(live["reference"], "scaleCheck.live.reference")
            if reg is not None and (reg.get("scale") in (None, scale) or not one_word(reg)):
                errors.append("scaleCheck.live.reference must be a one-word register with a scale of its own: "
                              "on the scale under test it would agree with the battery at any numerator")
        if "state" in live:
            reg = register(live["state"], "scaleCheck.live.state")
            if reg is not None and not one_word(reg):
                errors.append("scaleCheck.live.state must be a one-word register")
        regulation = [key for key in ("battery", "reference", "state") if key in live]
        if regulation and len(regulation) != 3:
            errors.append("scaleCheck.live: the regulation test needs \"battery\", \"reference\" and \"state\" together")
        if len(regulation) == 3 and not live.get("regulating"):
            errors.append("scaleCheck.live.regulating: list the state values in which the battery is held at the target")
        for value in live.get("regulating", []):
            if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 0xFFFF:
                errors.append("scaleCheck.live.regulating: %r is not a state value" % (value,))
    return errors


def main(argv):
    paths = argv or sorted(glob.glob(os.path.join(HERE, "..", "profiles", "*.json")))
    if not paths:
        print("no profiles found")
        return 1
    failed = False
    for path in paths:
        errors = check(path)
        label = os.path.relpath(path)
        if errors:
            failed = True
            print("%s: %d problem(s)" % (label, len(errors)))
            for error in errors:
                print("  - " + error)
        else:
            print("%s: ok" % label)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
