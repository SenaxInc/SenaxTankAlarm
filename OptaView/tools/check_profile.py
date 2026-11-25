#!/usr/bin/env python3
"""Checks an OptaView profile for the mistakes that are easy to make by hand.

    python3 OptaView/tools/check_profile.py [profile.json ...]
    python3 OptaView/tools/check_profile.py --selftest   # check that the member rules refuse what they should

With no argument it checks every file in OptaView/profiles. The firmware's own loader
(OptaViewProfile.h) rejects a broken profile too; this script says more about what is wrong and
also enforces house rules the loader does not care about (printable ASCII text, addresses in
order, every group declared, no member the loader would ignore).
"""

import copy
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# Used with fullmatch: with match and "$", a name ending in a newline would have passed.
NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
# An address or raw value written as text, as the firmware reads it (ovParseU32 in OptaViewText.h):
# decimal digits, or 0x and hex digits, and nothing else. int() would also take a sign, spaces and _.
NUMBER_TEXT = re.compile(r"0[xX][0-9A-Fa-f]+|[0-9]+")
TYPES = {"u16", "i16", "u8", "i8", "u32", "enum", "bits"}
FORMATS = {"8N1", "8N2", "8E1", "8O1"}
# What ArduinoJson, which reads the profile in the firmware, refuses where Python's json would not:
# a number of more than 63 characters (parseNumericValue reads at most 63) and lists and objects
# nested more than 10 deep (ARDUINOJSON_DEFAULT_NESTING_LIMIT). NaN and Infinity are not JSON.
NUMBER_TOKEN_MAX = 63
NESTING_MAX = 10
# The room the loader has for each list of the scale check (OvScaleCheck). It refuses a longer one.
ALTERNATES_MAX, SETPOINTS_MAX, REGULATING_MAX = 4, 24, 8

# The JSON type each member must have when it is there, as the firmware's loader requires
# (membersFit() in OptaViewProfile.h). Read with a default, a member of another type would fall
# back to that default without a word: "min": "0" would leave a setting without its lower limit,
# and "checks": {} would leave out every pair check. A member that is null counts as left out, as
# it does for the loader.
TEXT, NUMBER, WHOLE, FLAG = "text", "a number", "a whole number", "true or false"
LIST, OBJECT = "a list", "an object"
ROOT_MEMBERS = {"profile": TEXT, "title": TEXT, "version": TEXT, "source": TEXT, "verifiedBy": TEXT,
                "notes": LIST, "writeNotes": LIST, "wiring": LIST, "scales": OBJECT, "labels": OBJECT,
                "groups": LIST, "registers": LIST, "coils": LIST, "modbus": OBJECT, "liveGroups": LIST,
                "checks": LIST, "scaleCheck": OBJECT}
GROUP_MEMBERS = {"name": TEXT, "title": TEXT}
SCALE_MEMBERS = {"num": NUMBER, "den": NUMBER, "units": TEXT, "decimals": WHOLE}
REGISTER_MEMBERS = {"label": TEXT, "note": TEXT, "type": TEXT, "order": TEXT, "scale": TEXT, "labels": TEXT,
                    "units": TEXT, "group": TEXT, "verified": TEXT, "words": WHOLE, "decimals": WHOLE,
                    "min": NUMBER, "max": NUMBER, "writable": FLAG, "zeroOk": FLAG, "sentinel": OBJECT}
COIL_MEMBERS = {"label": TEXT, "note": TEXT, "trigger": FLAG}
MODBUS_MEMBERS = {"slaveId": WHOLE, "readFunction": WHOLE, "maxBlock": WHOLE, "baud": WHOLE,
                  "responseTimeoutMs": WHOLE, "resetWaitMs": WHOLE, "format": TEXT, "probeRegister": TEXT,
                  "resetCoil": TEXT}
CHECK_MEMBERS = {"below": TEXT, "above": TEXT, "message": TEXT}
SCALE_CHECK_MEMBERS = {"grid": NUMBER, "alternates": LIST, "settings": OBJECT, "live": OBJECT}
SETTINGS_MEMBERS = {"scale": TEXT, "setpoints": LIST}
LIVE_MEMBERS = {"scale": TEXT, "meter": TEXT, "battery": TEXT, "reference": TEXT, "state": TEXT,
                "regulating": LIST}


def fits(value, kind):
    """Whether a member's value has the JSON type `kind`."""
    if kind == TEXT:
        return isinstance(value, str)
    if kind == FLAG:
        return isinstance(value, bool)
    if kind == LIST:
        return isinstance(value, list)
    if kind == OBJECT:
        return isinstance(value, dict)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return False
    if kind == WHOLE:
        return isinstance(value, int) and -2 ** 31 <= value < 2 ** 31  # the loader reads an int
    # The loader's bound (memberFits): neither NaN nor infinite, nor beyond 1e300 either way. An
    # int is compared exactly, so one too long for a double is refused without an OverflowError.
    return -1e300 <= value <= 1e300


def present(item):
    """The members of a JSON object that are there, null ones left out; {} for anything else."""
    if not isinstance(item, dict):
        return {}
    return {key: value for key, value in item.items() if value is not None}


def wrong_members(item, members, where):
    """An error for each member of `item` that has another type than `members` gives it."""
    return ["%s: \"%s\" must be %s, got %r" % (where, key, kind, item[key])
            for key, kind in members.items() if key in item and not fits(item[key], kind)]


def unknown_members(item, members, where, known=()):
    """An error for each member of `item` that is neither in `members` nor in `known`. The loader
    ignores a member it does not know, so a misspelt "mn" would leave a setting without its
    lower limit, and nothing would say so."""
    return ["%s: \"%s\" is not a member the firmware reads, so it would be ignored" % (where, key)
            for key in item if key not in members and key not in known]


def number(value, what, errors):
    """An address or raw value: a whole number, or text such as "0xE022" or "57378"."""
    if isinstance(value, bool):
        errors.append("%s: expected a number, got %r" % (what, value))
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, str) and NUMBER_TEXT.fullmatch(value):
        return int(value, 16) if value[1:2] in ("x", "X") else int(value, 10)
    errors.append("%s: expected a number such as 8 or \"0x0008\", got %r" % (what, value))
    return None


def nesting(value):
    """How many levels of lists and objects deep `value` goes: 1 for [1], 2 for [[1]]."""
    deepest, stack = 0, [(value, 1)]
    while stack:
        item, level = stack.pop()
        if isinstance(item, (dict, list)):
            deepest = max(deepest, level)
            stack.extend((child, level + 1) for child in (item.values() if isinstance(item, dict) else item))
    return deepest


def load_json(text):
    """The profile as the firmware's JSON parser reads it, or ValueError where that parser would
    refuse it: NaN and Infinity, a number of more than 63 characters, or nesting beyond 10."""
    def constant(name):
        raise ValueError("%s is not a JSON number" % name)

    def short(parse):
        def read(token):
            if len(token) > NUMBER_TOKEN_MAX:
                raise ValueError("a number of %d characters, where the firmware reads at most %d"
                                 % (len(token), NUMBER_TOKEN_MAX))
            return parse(token)
        return read

    try:
        data = json.loads(text, parse_constant=constant, parse_int=short(int), parse_float=short(float))
    except RecursionError:
        raise ValueError("lists and objects nested more than %d deep" % NESTING_MAX)
    if nesting(data) > NESTING_MAX:
        raise ValueError("lists and objects nested more than %d deep" % NESTING_MAX)
    return data


def not_printable(data):
    """An error for each text in the profile, member names included, that holds a character
    other than printable ASCII. A \\u escape gets past the file's own ASCII check."""
    errors, stack = [], [data]
    while stack:
        item = stack.pop()
        texts = []
        if isinstance(item, dict):
            texts = list(item)
            stack.extend(item.values())
        elif isinstance(item, list):
            stack.extend(item)
        elif isinstance(item, str):
            texts = [item]
        for text in texts:
            if any(not " " <= char <= "~" for char in text):
                errors.append("%r: not printable ASCII; the console prints text as it is" % text)
    return errors


def check(path):
    with open(path, "rb") as handle:
        return check_text(handle.read())


def check_text(raw):
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError as error:
        return ["not plain ASCII (byte %d): the console prints these strings as they are" % error.start]
    try:
        data = load_json(text)
    except ValueError as error:
        return ["not valid JSON: %s" % error]
    return check_data(data)


def check_data(data):
    errors = []
    if not isinstance(data, dict):
        return ["the profile must be a JSON object"]
    errors.extend(not_printable(data))
    schema = data.get("schema")
    if isinstance(schema, bool) or not isinstance(schema, int) or schema != 1:  # true == 1 in Python
        errors.append("\"schema\" must be 1")
    wrong = wrong_members(present(data), ROOT_MEMBERS, "the profile")
    if wrong:
        return errors + wrong  # the rest would be read from the wrong kind of thing
    errors.extend(unknown_members(present(data), ROOT_MEMBERS, "the profile", known=("schema",)))
    data = present(data)  # null members are left out, here as in the loader
    if not data.get("profile"):
        errors.append("missing \"profile\" name")
    for key in ("notes", "writeNotes", "wiring"):
        if any(not isinstance(line, str) for line in data.get(key, [])):
            errors.append("%s: a list of lines must hold only text" % key)

    scales = data.get("scales", {})
    for name, scale in scales.items():
        scale = present(scale)
        wrong = wrong_members(scale, SCALE_MEMBERS, "scale %s" % name)
        if wrong:
            errors.extend(wrong)
            continue
        errors.extend(unknown_members(scale, SCALE_MEMBERS, "scale %s" % name))
        if not ("num" in scale and scale["num"] > 0 and "den" in scale and scale["den"] > 0):
            errors.append("scale %s: \"num\" and \"den\" must be numbers above zero" % name)
        if not 0 <= scale.get("decimals", 0) <= 6:
            # The firmware shows at most 6 (ovFormatFixed), and its loader refuses more.
            errors.append("scale %s: \"decimals\" must be 0 to 6" % name)

    labels = data.get("labels", {})
    for name, items in labels.items():
        if not isinstance(items, list) or not 1 <= len(items) <= 32:
            errors.append("labels %s: must be a list of 1 to 32 names" % name)
        elif any(item is not None and not isinstance(item, str) for item in items):
            errors.append("labels %s: holds names, or null for a value without one" % name)

    groups = []
    for group in data.get("groups", []):
        group = present(group)
        wrong = wrong_members(group, GROUP_MEMBERS, "group %s" % group.get("name"))
        errors.extend(wrong)
        errors.extend(unknown_members(group, GROUP_MEMBERS, "group %s" % group.get("name")))
        if not wrong and not group.get("name"):
            errors.append("a group needs a \"name\"")
        if not wrong:
            groups.append(group.get("name"))
    if len(groups) != len(set(groups)):
        errors.append("two groups share a name")

    registers = data.get("registers", [])
    if not registers:
        errors.append("no registers")
    by_name = {}
    spans = []
    previous = -1
    for reg in registers:
        reg = present(reg)
        name = reg.get("name")
        if not isinstance(name, str) or not NAME.fullmatch(name):
            errors.append("register name %r: use letters, digits and _, not starting with a digit" % (name,))
            continue
        if name.lower() in by_name:
            errors.append("register %s: the name is used twice" % name)
        by_name[name.lower()] = reg
        wrong = wrong_members(reg, REGISTER_MEMBERS, "register %s" % name)
        if wrong:
            errors.extend(wrong)
            continue
        errors.extend(unknown_members(reg, REGISTER_MEMBERS, "register %s" % name, known=("name", "addr")))
        if not 0 <= reg.get("decimals", 0) <= 6:
            errors.append("register %s: \"decimals\" must be 0 to 6" % name)
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
            raw = number(reg["sentinel"].get("raw"), "register %s sentinel raw" % name, errors)
            if raw is not None and not 0 <= raw <= 0xFFFF:
                errors.append("register %s: the sentinel's raw value must be 0 to 65535" % name)
            if not isinstance(reg["sentinel"].get("text"), str):
                errors.append("register %s: the sentinel needs \"text\"" % name)
            errors.extend(unknown_members(present(reg["sentinel"]), ("raw", "text"), "register %s sentinel" % name))
    spans.sort()
    for (_, end, first), (start, _, second) in zip(spans, spans[1:]):
        if start < end:
            errors.append("registers %s and %s overlap" % (first, second))

    coils = {}
    for coil in data.get("coils", []):
        coil = present(coil)
        name = coil.get("name")
        if not isinstance(name, str) or not NAME.fullmatch(name):
            errors.append("coil name %r: use letters, digits and _" % (name,))
            continue
        if name.lower() in coils:
            errors.append("coil %s: the name is used twice" % name)
        coils[name.lower()] = coil
        addr = number(coil.get("addr"), "coil %s addr" % name, errors)
        if addr is not None and not 0 <= addr <= 0xFFFF:
            errors.append("coil %s: address out of range" % name)
        errors.extend(wrong_members(coil, COIL_MEMBERS, "coil %s" % name))
        errors.extend(unknown_members(coil, COIL_MEMBERS, "coil %s" % name, known=("name", "addr")))

    def register(name, what, numeric=False):
        reg = by_name.get(str(name).lower())
        if reg is None:
            errors.append("%s: no register called %r" % (what, name))
        elif numeric and reg.get("type", "u16") in ("enum", "bits"):
            errors.append("%s: %r is not a numeric register" % (what, name))
        return reg

    modbus = present(data.get("modbus", {}))
    wrong = wrong_members(modbus, MODBUS_MEMBERS, "modbus")
    errors.extend(wrong)
    errors.extend(unknown_members(modbus, MODBUS_MEMBERS, "modbus"))
    if not wrong:
        if not 1 <= modbus.get("slaveId", 1) <= 247:
            errors.append("modbus.slaveId must be 1 to 247")
        if modbus.get("readFunction", 4) not in (3, 4):
            errors.append("modbus.readFunction must be 3 or 4")
        if not 1 <= modbus.get("maxBlock", 16) <= 125:
            errors.append("modbus.maxBlock must be 1 to 125")
        if not 300 <= modbus.get("baud", 9600) <= 1000000:
            errors.append("modbus.baud must be 300 to 1000000")
        if not 50 <= modbus.get("responseTimeoutMs", 1000) <= 10000:
            errors.append("modbus.responseTimeoutMs must be 50 to 10000")
        if not 0 <= modbus.get("resetWaitMs", 3000) <= 60000:
            errors.append("modbus.resetWaitMs must be 0 to 60000")
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
        item = present(item)
        wrong = wrong_members(item, CHECK_MEMBERS, "check")
        if wrong:
            errors.extend(wrong)
            continue
        errors.extend(unknown_members(item, CHECK_MEMBERS, "check"))
        register(item.get("below"), "check \"below\"", numeric=True)
        register(item.get("above"), "check \"above\"", numeric=True)
        if not item.get("message"):
            errors.append("a check needs a \"message\"")

    errors.extend(check_scale_check(data.get("scaleCheck"), scales, register))
    live_scale = present(present(data.get("scaleCheck")).get("live")).get("scale")
    for reg in map(present, registers):
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
    scale_check = present(scale_check)
    errors = wrong_members(scale_check, SCALE_CHECK_MEMBERS, "scaleCheck")
    for half, members in (("settings", SETTINGS_MEMBERS), ("live", LIVE_MEMBERS)):
        if half in scale_check and not errors:
            scale_check[half] = present(scale_check[half])
            errors.extend(wrong_members(scale_check[half], members, "scaleCheck.%s" % half))
    if errors:
        return errors  # the rest would be read from the wrong kind of thing
    for key in scale_check:
        if key not in ("alternates", "grid", "settings", "live"):
            # The loader ignores entries it does not know, so a misspelt one would switch a test off.
            errors.append("scaleCheck.%s: not a scaleCheck entry (\"alternates\", \"grid\", \"settings\", \"live\")" % key)
    for half, known in (("settings", ("scale", "setpoints")),
                        ("live", ("scale", "meter", "battery", "reference", "state", "regulating"))):
        for key in scale_check.get(half) or {}:
            if key not in known:
                errors.append("scaleCheck.%s.%s: not an entry of scaleCheck.%s" % (half, key, half))
    # The loader has room for 4 numerators, 24 setpoints and 8 states, and refuses a longer list.
    if len(scale_check.get("alternates", [])) > ALTERNATES_MAX:
        errors.append("scaleCheck.alternates holds at most %d numerators" % ALTERNATES_MAX)
    for alternate in scale_check.get("alternates", []):
        if not fits(alternate, NUMBER) or alternate <= 0:
            errors.append("scaleCheck.alternates: %r is not a number above zero" % (alternate,))
    if scale_check.get("grid", 0.05) <= 0:  # a number: its type is looked at above
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
        if len(setpoints) > SETPOINTS_MAX:
            errors.append("scaleCheck.settings.setpoints names at most %d registers" % SETPOINTS_MAX)
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
        if len(live.get("regulating", [])) > REGULATING_MAX:
            errors.append("scaleCheck.live.regulating lists at most %d states" % REGULATING_MAX)
        for value in live.get("regulating", []):
            if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 0xFFFF:
                errors.append("scaleCheck.live.regulating: %r is not a state value" % (value,))
    return errors


# For the self-test: one member of the SunSaver profile at a time is given a value of the wrong
# type, or out of its range, and the error that has to follow. ("register", name) or ("coil",
# name) picks an entry, ("scale", name) a scale, ("modbus",) the link defaults, ("check", n) a
# pair check, ("group", n) a group, ("labels",) the label sets, ("sentinel", name) a register's
# sentinel, ("scaleCheck",), ("settings",) and ("live",) the scale check, and () the top.
SELF_TESTS = [
    (("register", "ER_icomp"), "min", "0", 'register ER_icomp: "min" must be a number'),
    (("register", "ER_icomp"), "max", "0.2", 'register ER_icomp: "max" must be a number'),
    (("register", "EV_lvd"), "min", True, 'register EV_lvd: "min" must be a number'),
    (("register", "EV_lvd"), "max", [14], 'register EV_lvd: "max" must be a number'),
    (("register", "EV_lvd"), "max", float("inf"), 'register EV_lvd: "max" must be a number'),
    (("register", "EV_lvd"), "max", 10 ** 400, 'register EV_lvd: "max" must be a number'),
    (("register", "EV_lvd"), "writable", "true", 'register EV_lvd: "writable" must be true or false'),
    (("register", "EV_hvd"), "zeroOk", 1, 'register EV_hvd: "zeroOk" must be true or false'),
    (("register", "EV_lvd"), "scale", 1, 'register EV_lvd: "scale" must be text'),
    (("register", "EV_tempcomp"), "type", 8, 'register EV_tempcomp: "type" must be text'),
    (("register", "charge_state"), "labels", ["a"], 'register charge_state: "labels" must be text'),
    (("register", "Ahc_r"), "order", 1, 'register Ahc_r: "order" must be text'),
    (("register", "EV_lvd"), "group", 2, 'register EV_lvd: "group" must be text'),
    (("register", "EV_lvd"), "verified", True, 'register EV_lvd: "verified" must be text'),
    (("register", "EV_lvd"), "label", 3, 'register EV_lvd: "label" must be text'),
    (("register", "EV_lvd"), "note", {}, 'register EV_lvd: "note" must be text'),
    (("register", "EV_lvd"), "units", 1, 'register EV_lvd: "units" must be text'),
    (("register", "EV_lvd"), "decimals", 2.0, 'register EV_lvd: "decimals" must be a whole number'),
    (("register", "alarm"), "words", "2", 'register alarm: "words" must be a whole number'),
    (("register", "EV_lvd"), "sentinel", 0, 'register EV_lvd: "sentinel" must be an object'),
    (("coil", "clear_kwh"), "trigger", "true", 'coil clear_kwh: "trigger" must be true or false'),
    (("coil", "clear_kwh"), "note", 1, 'coil clear_kwh: "note" must be text'),
    (("coil", "clear_kwh"), "label", 0, 'coil clear_kwh: "label" must be text'),
    (("scale", "vset"), "num", True, 'scale vset: "num" must be a number'),
    (("scale", "vset"), "den", "32768", 'scale vset: "den" must be a number'),
    (("scale", "vset"), "units", 1, 'scale vset: "units" must be text'),
    (("scale", "vset"), "decimals", 2.5, 'scale vset: "decimals" must be a whole number'),
    (("modbus",), "slaveId", 2.0, 'modbus: "slaveId" must be a whole number'),
    (("modbus",), "baud", "9600", 'modbus: "baud" must be a whole number'),
    (("modbus",), "baud", 9600.0, 'modbus: "baud" must be a whole number'),
    (("modbus",), "readFunction", 4.0, 'modbus: "readFunction" must be a whole number'),
    (("modbus",), "maxBlock", True, 'modbus: "maxBlock" must be a whole number'),
    (("modbus",), "responseTimeoutMs", 2 ** 31, 'modbus: "responseTimeoutMs" must be a whole number'),
    (("modbus",), "resetWaitMs", "3000", 'modbus: "resetWaitMs" must be a whole number'),
    (("modbus",), "format", 82, 'modbus: "format" must be text'),
    (("modbus",), "probeRegister", 1, 'modbus: "probeRegister" must be text'),
    (("modbus",), "resetCoil", True, 'modbus: "resetCoil" must be text'),
    (("check", 0), "below", 1, 'check: "below" must be text'),
    (("check", 0), "above", ["EV_lvr"], 'check: "above" must be text'),
    (("check", 0), "message", 1, 'check: "message" must be text'),
    ((), "title", 1, 'the profile: "title" must be text'),
    ((), "profile", 1, 'the profile: "profile" must be text'),
    ((), "version", 1.0, 'the profile: "version" must be text'),
    ((), "source", False, 'the profile: "source" must be text'),
    ((), "verifiedBy", [], 'the profile: "verifiedBy" must be text'),
    # The lists and the sections, which the loader would otherwise read as empty.
    ((), "checks", {}, 'the profile: "checks" must be a list'),
    ((), "coils", {"name": "clear_kwh"}, 'the profile: "coils" must be a list'),
    ((), "registers", {}, 'the profile: "registers" must be a list'),
    ((), "groups", "live", 'the profile: "groups" must be a list'),
    ((), "liveGroups", "live", 'the profile: "liveGroups" must be a list'),
    ((), "notes", "one line", 'the profile: "notes" must be a list'),
    ((), "writeNotes", {}, 'the profile: "writeNotes" must be a list'),
    ((), "wiring", 1, 'the profile: "wiring" must be a list'),
    ((), "scales", [], 'the profile: "scales" must be an object'),
    ((), "labels", [["a"]], 'the profile: "labels" must be an object'),
    ((), "modbus", [1], 'the profile: "modbus" must be an object'),
    ((), "scaleCheck", [], 'the profile: "scaleCheck" must be an object'),
    ((), "writeNotes", ["Read it first.", 5], "writeNotes: a list of lines must hold only text"),
    (("labels",), "alarm", ["RTS open", 5], "labels alarm: holds names, or null for a value without one"),
    (("group", 0), "title", 1, 'group live: "title" must be text'),
    (("group", 0), "name", 1, 'group 1: "name" must be text'),
    (("scaleCheck",), "grid", "0.05", 'scaleCheck: "grid" must be a number'),
    (("scaleCheck",), "grid", float("inf"), 'scaleCheck: "grid" must be a number'),
    (("scaleCheck",), "alternates", 96.667, 'scaleCheck: "alternates" must be a list'),
    (("scaleCheck",), "alternates", ["96.667"], "scaleCheck.alternates: '96.667' is not a number above zero"),
    (("scaleCheck",), "alternates", [float("inf")], "scaleCheck.alternates: inf is not a number above zero"),
    (("scaleCheck",), "alternates", [True], "scaleCheck.alternates: True is not a number above zero"),
    (("scaleCheck",), "alternates", [0], "scaleCheck.alternates: 0 is not a number above zero"),
    (("scaleCheck",), "settings", ["vset"], 'scaleCheck: "settings" must be an object'),
    (("scaleCheck",), "live", "vlive", 'scaleCheck: "live" must be an object'),
    (("settings",), "setpoints", "EV_reg", 'scaleCheck.settings: "setpoints" must be a list'),
    (("settings",), "scale", 1, 'scaleCheck.settings: "scale" must be text'),
    (("live",), "regulating", 1, 'scaleCheck.live: "regulating" must be a list'),
    (("live",), "meter", 8, 'scaleCheck.live: "meter" must be text'),
    (("live",), "regulating", [1, 70000], "scaleCheck.live.regulating: 70000 is not a state value"),
    # The ranges the loader refuses as well.
    ((), "schema", True, '"schema" must be 1'),
    ((), "schema", 1.0, '"schema" must be 1'),
    (("modbus",), "baud", 299, "modbus.baud must be 300 to 1000000"),
    (("modbus",), "baud", 1000001, "modbus.baud must be 300 to 1000000"),
    (("modbus",), "responseTimeoutMs", 49, "modbus.responseTimeoutMs must be 50 to 10000"),
    (("modbus",), "responseTimeoutMs", 10001, "modbus.responseTimeoutMs must be 50 to 10000"),
    (("modbus",), "resetWaitMs", -1, "modbus.resetWaitMs must be 0 to 60000"),
    (("modbus",), "resetWaitMs", 60001, "modbus.resetWaitMs must be 0 to 60000"),
    (("coil", "clear_kwh"), "addr", 65536, "coil clear_kwh: address out of range"),
    (("coil", "clear_kwh"), "addr", -1, "coil clear_kwh: address out of range"),
    (("sentinel", "T_rts"), "raw", 65536, "register T_rts: the sentinel's raw value must be 0 to 65535"),
    # Names as the loader's validName() reads them: "$" would have let a final newline through.
    (("register", "adc_va_f"), "name", "adc_va_f\n", "register name 'adc_va_f\\n': use letters"),
    (("coil", "clear_kwh"), "name", "clear_kwh\n", "coil name 'clear_kwh\\n': use letters"),
    (("register", "adc_va_f"), "name", "adc va", "register name 'adc va': use letters"),
    # Numbers written as text, as ovParseU32() reads them: int() would take a sign, spaces and _.
    (("register", "EV_lvd"), "addr", "0xE0_22", "register EV_lvd addr: expected a number"),
    (("register", "EV_lvd"), "addr", "0x_E022", "register EV_lvd addr: expected a number"),
    (("register", "EV_lvd"), "addr", "57_378", "register EV_lvd addr: expected a number"),
    (("register", "EV_lvd"), "addr", "+57378", "register EV_lvd addr: expected a number"),
    (("register", "EV_lvd"), "addr", "-0", "register EV_lvd addr: expected a number"),
    (("register", "EV_lvd"), "addr", " 57378", "register EV_lvd addr: expected a number"),
    (("register", "EV_lvd"), "addr", "57378 ", "register EV_lvd addr: expected a number"),
    (("register", "EV_lvd"), "addr", "57378\n", "register EV_lvd addr: expected a number"),
    (("register", "EV_lvd"), "addr", "0x", "register EV_lvd addr: expected a number"),
    (("register", "EV_lvd"), "addr", "", "register EV_lvd addr: expected a number"),
    (("register", "EV_lvd"), "addr", "0xE022h", "register EV_lvd addr: expected a number"),
    (("register", "EV_lvd"), "addr", "".join(map(chr, (0xFF15, 0xFF17, 0xFF13, 0xFF17, 0xFF18))),
     "register EV_lvd addr: expected a number"),  # full-width digits, which int() reads as 57378
    (("register", "EV_lvd"), "addr", 57378.0, "register EV_lvd addr: expected a number"),
    (("coil", "clear_kwh"), "addr", "0x0_10", "coil clear_kwh addr: expected a number"),
    (("coil", "clear_kwh"), "addr", " 18", "coil clear_kwh addr: expected a number"),
    (("sentinel", "T_rts"), "raw", "+5", "register T_rts sentinel raw: expected a number"),
    (("sentinel", "T_rts"), "raw", "0x8_0", "register T_rts sentinel raw: expected a number"),
    # Lists longer than the loader has room for, which it refuses rather than cut short.
    (("scaleCheck",), "alternates", [90, 95, 96.667, 100, 105], "scaleCheck.alternates holds at most 4 numerators"),
    (("live",), "regulating", list(range(9)), "scaleCheck.live.regulating lists at most 8 states"),
    # Decimals the loader would otherwise have cut to 0 or 6 without a word.
    (("register", "EV_lvd"), "decimals", 7, 'register EV_lvd: "decimals" must be 0 to 6'),
    (("register", "EV_lvd"), "decimals", -1, 'register EV_lvd: "decimals" must be 0 to 6'),
    (("scale", "vset"), "decimals", 7, 'scale vset: "decimals" must be 0 to 6'),
    (("scale", "vset"), "decimals", -1, 'scale vset: "decimals" must be 0 to 6'),
    # A number beyond the loader's bound of 1e300, which a double can still hold.
    (("register", "EV_lvd"), "max", 1e301, 'register EV_lvd: "max" must be a number'),
    (("register", "EV_lvd"), "min", -1.5e300, 'register EV_lvd: "min" must be a number'),
    (("register", "EV_lvd"), "max", 10 ** 301, 'register EV_lvd: "max" must be a number'),
    (("scale", "vset"), "num", 2e300, 'scale vset: "num" must be a number'),
    (("scaleCheck",), "grid", 1e301, 'scaleCheck: "grid" must be a number'),
    (("scaleCheck",), "alternates", [1e301], "scaleCheck.alternates: 1e+301 is not a number above zero"),
    # Members the loader does not read, which it would ignore without a word.
    (("register", "EV_lvd"), "mn", 9.0, 'register EV_lvd: "mn" is not a member the firmware reads'),
    (("register", "EV_lvd"), "Max", 15.0, 'register EV_lvd: "Max" is not a member the firmware reads'),
    (("register", "EV_lvd"), "writeable", True, 'register EV_lvd: "writeable" is not a member the firmware reads'),
    ((), "scheme", 1, 'the profile: "scheme" is not a member the firmware reads'),
    ((), "check", [], 'the profile: "check" is not a member the firmware reads'),
    (("scale", "vset"), "decimal", 2, 'scale vset: "decimal" is not a member the firmware reads'),
    (("coil", "clear_kwh"), "triger", True, 'coil clear_kwh: "triger" is not a member the firmware reads'),
    (("modbus",), "slave", 2, 'modbus: "slave" is not a member the firmware reads'),
    (("check", 0), "messge", "x", 'check: "messge" is not a member the firmware reads'),
    (("group", 0), "titel", "x", 'group live: "titel" is not a member the firmware reads'),
    (("sentinel", "T_rts"), "txt", "x", 'register T_rts sentinel: "txt" is not a member the firmware reads'),
    # Text that is not printable ASCII, which an escape in the file gets past its own check.
    (("register", "EV_lvd"), "label", "caf" + chr(0xE9), "not printable ASCII"),
    (("register", "EV_lvd"), "note", "a bell" + chr(7), "not printable ASCII"),
    (("register", "EV_lvd"), "units", "V" + chr(0), "not printable ASCII"),
    (("register", "EV_lvd"), "units", "V" + chr(0x7F), "not printable ASCII"),
    (("coil", "clear_kwh"), "note", "line one\nline two", "not printable ASCII"),
    ((), "notes", ["a tab\there"], "not printable ASCII"),
    (("labels",), "alarm", ["RTS open" + chr(0x1B) + "[2J"], "not printable ASCII"),
    ((), "title" + chr(0xA0), "x", "not printable ASCII"),
]

# Changes after which the profile must still pass: the edges of each rule above.
ALLOWED_TESTS = [
    (("register", "EV_lvd"), "addr", "0XE022"),
    (("register", "EV_lvd"), "addr", "0xe022"),
    (("register", "EV_lvd"), "addr", "057378"),
    (("register", "EV_lvd"), "addr", 57378),
    (("coil", "clear_kwh"), "addr", "18"),
    (("coil", "clear_kwh"), "addr", "0X12"),
    (("sentinel", "T_rts"), "raw", "0x0080"),
    (("sentinel", "T_rts"), "raw", "128"),
    (("sentinel", "T_rts"), "raw", 128),
    (("scaleCheck",), "alternates", [90, 95, 96.667, 100]),
    (("live",), "regulating", list(range(8))),
    (("register", "EV_lvd"), "decimals", 0),
    (("register", "EV_lvd"), "decimals", 6),
    (("scale", "vset"), "decimals", 0),
    (("scale", "vset"), "decimals", 6),
    (("register", "EV_lvd"), "max", 1e300),
    (("register", "EV_lvd"), "min", -1e300),
    (("register", "EV_lvd"), "note", "Printable: ~ { } \" \\ ' ` | < > & %"),
]

# Text the firmware's JSON parser refuses, and text it reads, to check load_json() against.
JSON_REFUSED = ["[NaN]", "[Infinity]", "[-Infinity]", "[" + "9" * 64 + "]", "[-" + "9" * 63 + "]",
                "[1." + "0" * 62 + "]", "[1e" + "0" * 62 + "]", "[" * 11 + "]" * 11,
                '{"a":' * 11 + "1" + "}" * 11, '{"a":[' * 6 + "1" + "]}" * 6, "[" * 100000 + "]" * 100000]
JSON_READ = ["[" + "9" * 63 + "]", "[-" + "9" * 62 + "]", "[1." + "0" * 61 + "]", "[" * 10 + "]" * 10,
             '{"a":' * 10 + "1" + "}" * 10, '{"a":[' * 5 + "1" + "]}" * 5, "[1e300, -1e300, 0.5, -0]"]


def member_owner(data, where):
    """The JSON object a self-test changes."""
    if not where:
        return data
    if where[0] == "modbus":
        return data["modbus"]
    if where[0] == "scale":
        return data["scales"][where[1]]
    if where[0] == "check":
        return data["checks"][where[1]]
    if where[0] == "group":
        return data["groups"][where[1]]
    if where[0] == "sentinel":
        return next(entry for entry in data["registers"] if entry["name"] == where[1])["sentinel"]
    if where[0] == "labels":
        return data["labels"]
    if where[0] == "scaleCheck":
        return data["scaleCheck"]
    if where[0] in ("settings", "live"):
        return data["scaleCheck"][where[0]]
    entries = data["registers"] if where[0] == "register" else data["coils"]
    return next(entry for entry in entries if entry["name"] == where[1])


def selftest():
    path = os.path.join(HERE, "..", "profiles", "sunsaver-mppt.json")
    with open(path, encoding="ascii") as handle:
        profile = json.load(handle)
    failures = ["the profile as it is: %s" % error for error in check_data(copy.deepcopy(profile))]

    def changed(where, key, value):
        data = copy.deepcopy(profile)
        member_owner(data, where)[key] = value
        return data

    def errors_of(run, argument):
        try:
            return run(argument)
        except Exception as error:  # a crash is no answer either
            return ["crashed: %r" % error]

    setpoints = profile["scaleCheck"]["settings"]["setpoints"] * 2
    refused = SELF_TESTS + [(("settings",), "setpoints", setpoints[:SETPOINTS_MAX + 1],
                             "scaleCheck.settings.setpoints names at most 24 registers")]
    for where, key, value, expected in refused:
        errors = errors_of(check_data, changed(where, key, value))
        if not any(expected in error for error in errors):
            failures.append("%s = %r: expected %r, got %r" % (key, value, expected, errors[:2]))
    # A null member counts as left out, as it does for the loader.
    allowed = ALLOWED_TESTS + [(("settings",), "setpoints", setpoints[:SETPOINTS_MAX])] + [
        (where, key, None) for where, key in ((("register", "EV_lvd"), "note"), ((), "wiring"),
                                              (("scaleCheck",), "grid"), ((), "notes"), (("group", 0), "title"),
                                              (("register", "EV_lvd"), "mn"))]
    for where, key, value in allowed:
        failures += ["%s = %r: %s" % (key, value, error) for error in errors_of(check_data, changed(where, key, value))]

    # The file as written: what the firmware's JSON parser refuses, whatever the member.
    def text_with(where, key, token):
        return json.dumps(changed(where, key, "@token@")).replace('"@token@"', token).encode("ascii")

    backslash = chr(92)
    texts = [((("register", "EV_lvd"), "max", "NaN"), "not valid JSON: NaN is not a JSON number"),
             ((("register", "EV_lvd"), "max", "-Infinity"), "not valid JSON: -Infinity is not a JSON number"),
             ((("register", "EV_lvd"), "mn", "Infinity"), "not valid JSON: Infinity is not a JSON number"),
             ((("scaleCheck",), "alternates", "[96.667" + "0" * 58 + "]"), "not valid JSON: a number of 64 characters"),
             ((("live",), "regulating", "[" * 8 + "6" + "]" * 8), "not valid JSON: lists and objects nested more than 10"),
             ((("register", "EV_lvd"), "label", '"caf' + backslash + 'u00e9"'), "not printable ASCII")]
    for (where, key, token), expected in texts:
        errors = errors_of(check_text, text_with(where, key, token))
        if not any(expected in error for error in errors):
            failures.append("%s as %s: expected %r, got %r" % (key, token[:20], expected, errors[:2]))
    longest = text_with(("scaleCheck",), "alternates", "[96.667" + "0" * 57 + "]")  # 63 characters
    failures += ["a number of 63 characters: %s" % error for error in errors_of(check_text, longest)]
    for text in JSON_REFUSED:
        try:
            load_json(text)
            failures.append("read %s..., which the firmware refuses" % text[:24])
        except ValueError:
            pass
        except Exception as error:
            failures.append("%s...: crashed: %r" % (text[:24], error))
    for text in JSON_READ:
        try:
            load_json(text)
        except Exception as error:
            failures.append("refused %s..., which the firmware reads: %s" % (text[:24], error))

    for failure in failures:
        print("check_profile.py self-test: " + failure)
    if failures:
        return 1
    print("check_profile.py self-test: %d wrong profiles refused, %d right ones passed, %d texts read as the "
          "firmware reads them" % (len(refused) + len(texts), len(allowed) + 2, len(JSON_REFUSED) + len(JSON_READ)))
    return 0


def main(argv):
    if argv == ["--selftest"]:
        return selftest()
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
