#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Verify the config key descriptor table (gh#64).

Replaces bin/check_cfg_tables.py, which cross-checked six hand-maintained key
lists against each other. There are no longer six lists: firmware/config/
cfg_desc.inc is the one place a config key is declared, and the write path,
the clamp, the audit id and GET /api/config/limits all read it. So the job
changes shape -- there is nothing left to cross-check the table AGAINST inside
the firmware, and the risk moves to the table's edges:

  1. A bound macro that does not exist in cfg_limits.h.
  2. A CFG_SH() field that does not exist in cfg_shadow_t, or whose width is
     not the 2 or 4 bytes the generic write handles. An offsetof() typo here
     writes the WRONG FIELD -- the one failure mode the old six-table drift
     could not produce, and the price of collapsing the ladder.
  3. A hand-written key list creeping back into a consumer.
  4. The published set drifting from the mock server (webUiMock), which is the
     one copy of the limits that still lives outside the firmware.
  5. The published payload drifting from what the pre-refactor firmware served,
     if a golden capture is supplied.
  6. A key the boot path does not restore. gh#64 counted six tables; the
     nvs_load_*() helpers are a SEVENTH, and they carry the defaults. A key
     missing there is accepted, clamped, audited and published, then silently
     reset to 0 on every reboot, with nothing logged.
  7. POST /api/web's inline bounds. That endpoint does NOT go through Q4 -- it
     writes NVS directly and reloads -- so it never meets cfg_clamp() and keeps
     its own copy of six keys' bounds. An EIGHTH place, three of them literals.

Checks 1, 2 and 3 are the ones that matter most: they are what the six-table
design could not get wrong and this one can.

Usage:
    python bin/check_cfg_desc.py                 # quiet unless something is wrong
    python bin/check_cfg_desc.py -v              # print the table
    python bin/check_cfg_desc.py --golden F.json # also diff against a capture
                                                 # of GET /api/config/limits

Exit code 0 = consistent, 1 = a check failed, 2 = a source could not be read
(treated as failure, never as a pass -- an unparseable table must not look clean).

Stdlib only. ASCII output only (Windows consoles here are cp1252).
"""

import argparse
import io
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

DESC_PATH = os.path.join(ROOT, "firmware", "config", "cfg_desc.inc")
LIM_PATH = os.path.join(ROOT, "firmware", "config", "cfg_limits.h")
DM_C_PATH = os.path.join(ROOT, "firmware", "src", "data_manager", "data_manager.cpp")
DM_H_PATH = os.path.join(ROOT, "firmware", "src", "data_manager", "data_manager.h")
WS_PATH = os.path.join(ROOT, "firmware", "src", "web_server", "web_server.cpp")
MOCK_PATH = os.path.join(ROOT, "webUiMock", "mock_server.py")

_ERRORS = []
_FATAL = []


def err(msg):
    _ERRORS.append(msg)


def fatal(msg):
    _FATAL.append(msg)


def read(path):
    try:
        with io.open(path, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except Exception as exc:                                  # noqa: BLE001
        fatal("cannot read %s: %s" % (os.path.relpath(path, ROOT), exc))
        return ""


def strip_c_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


# ---------------------------------------------------------------- sources ---

def macro_values():
    """{CFG_MIN_X: int} from cfg_limits.h, expanded as the preprocessor would."""
    out = {}
    for name, val in re.findall(
            r"#define\s+(CFG_(?:MIN|MAX)_[A-Z0-9_]+)\s+(\(?-?(?:0[xX][0-9a-fA-F]+|\d+)\)?)",
            read(LIM_PATH)):
        out[name] = int(val.strip("()"), 0)
    return out


def key_constants():
    """{K_CONSTANT: "key_string"} from data_manager.cpp."""
    return dict(re.findall(
        r'static const char (K_[A-Z0-9_]+)\[\]\s*=\s*"([a-z0-9_]+)"',
        strip_c_comments(read(DM_C_PATH))))


def shadow_fields():
    """{field: (width_bytes, array_len or None)} for cfg_shadow_t's scalars.

    Widths are what the generic write in apply_config_update() switches on, so
    a field of any other width silently would not be written at all."""
    src = strip_c_comments(read(DM_H_PATH))
    m = re.search(r"typedef struct\s*\{(.*?)\}\s*cfg_shadow_t\s*;", src, re.S)
    if not m:
        fatal("could not find cfg_shadow_t in data_manager.h")
        return {}
    width = {"int8_t": 1, "uint8_t": 1, "int16_t": 2, "uint16_t": 2,
             "int32_t": 4, "uint32_t": 4, "bool": 1, "char": 1}
    out = {}
    for typ, name, arr in re.findall(
            r"\b(int8_t|uint8_t|int16_t|uint16_t|int32_t|uint32_t|bool|char)\s+"
            r"(\w+)\s*(?:\[\s*(\d+)\s*\])?\s*;", m.group(1)):
        out[name] = (width[typ], int(arr) if arr else None)
    return out


ROW_RE = re.compile(
    r"\{\s*(NVS_NS_[A-Z]+)\s*,\s*(K_[A-Z0-9_]+|\"[a-z0-9_]+\")\s*,"
    r"\s*(CFG_KEY_\w+)\s*,\s*(\d+)u\s*,\s*([^,]+?)\s*,\s*(LOG_PARAM_\w+)\s*,"
    r"\s*([^,]+?)\s*,\s*([^,]+?)\s*,\s*([^,]+?)\s*,"
    r"\s*(CFG_SH\(\s*([A-Za-z0-9_\[\]]+)\s*\)|CFG_NOSH)\s*\}", re.S)


def parse_desc(macros, kconsts):
    """Parse cfg_desc.inc into rows, resolving macros and key constants."""
    src = strip_c_comments(read(DESC_PATH))
    rows = []
    for m in ROW_RE.finditer(src):
        (ns, kc, kind, chan, flags, pid, lo, hi, dflt, sh, fld) = m.groups()
        key = kc.strip('"') if kc.startswith('"') else kconsts.get(kc)
        if key is None:
            err("row references %s, which is not a key constant in "
                "data_manager.cpp" % kc)
            continue

        def resolve(tok):
            tok = tok.strip()
            if re.fullmatch(r"-?(?:0[xX][0-9a-fA-F]+|\d+)", tok):
                return int(tok, 0)
            if tok not in macros:
                err("%s: bound %s is not defined in cfg_limits.h" % (key, tok))
                return None
            return macros[tok]

        rows.append({
            "ns": ns, "key": key, "kconst": kc, "kind": kind,
            "channel": int(chan),
            "published": "CFG_F_PUB" in flags,
            "sun": "CFG_F_SUN" in flags,
            "param": pid,
            "min": resolve(lo), "max": resolve(hi),
            "def_tok": dflt.strip(),
            "web": "CFG_F_WEB" in flags,
            "shadow": fld if sh.startswith("CFG_SH") else None,
        })
    if not rows:
        fatal("no descriptor rows parsed from %s"
              % os.path.relpath(DESC_PATH, ROOT))
    return rows


# ----------------------------------------------------------------- checks ---

def check_bounds(rows):
    for r in rows:
        if r["min"] is None or r["max"] is None:
            continue                       # already reported by resolve()
        if r["min"] > r["max"]:
            err("%s: min %d is above max %d" % (r["key"], r["min"], r["max"]))


def check_shadow(rows, fields):
    """Every CFG_SH() field must exist and be 2 or 4 bytes wide.

    This is the check the six-table design never needed. Collapsing the ladder
    traded 51 explicit assignments for one offsetof-driven write, and the new
    way to be wrong is to name the wrong field -- which compiles, and writes
    a real but different setting."""
    if not fields:
        return
    for r in rows:
        fld = r["shadow"]
        # kind and shadow-field presence are related but NOT equivalent, and
        # conflating them is what made the first cut of this table give the four
        # ota_* keys CFG_NOSH when they have real cfg_shadow_t fields:
        #   SHADOW   must have one -- the Q4 generic write needs a destination.
        #   NVS_ONLY must not      -- ap_enable has none by design (T10 polls NVS).
        #   NOT_Q4   may have one  -- all four ota_* do; only the boot loader
        #                             writes them, /api/ota/config owns the rest.
        if r["kind"] == "CFG_KEY_SHADOW" and fld is None:
            err("%s: kind is CFG_KEY_SHADOW but has no shadow field -- the Q4 "
                "write would have nowhere to put it" % r["key"])
        if r["kind"] == "CFG_KEY_NVS_ONLY" and fld is not None:
            err("%s: kind is CFG_KEY_NVS_ONLY but names shadow field '%s' -- "
                "NVS-only means the consumer reads NVS, not the shadow"
                % (r["key"], fld))
        if fld is None:
            continue
        m = re.match(r"^(\w+)(?:\[(\d+)\])?$", fld)
        base, idx = m.group(1), m.group(2)
        if base not in fields:
            err("%s: cfg_shadow_t has no field '%s'" % (r["key"], base))
            continue
        wide, arrlen = fields[base]
        if wide not in (2, 4):
            err("%s: field '%s' is %d bytes; the generic write handles only "
                "2 and 4, so this key would never be applied"
                % (r["key"], base, wide))
        if idx is None and arrlen is not None:
            err("%s: field '%s' is an array but is referenced without an index"
                % (r["key"], base))
        if idx is not None:
            if arrlen is None:
                err("%s: field '%s' is not an array but is indexed"
                    % (r["key"], base))
            elif int(idx) >= arrlen:
                err("%s: field '%s'[%s] is past the end of a [%d] array -- "
                    "this would write over a neighbouring setting"
                    % (r["key"], base, idx, arrlen))

    # A field claimed by two keys means one of them writes the wrong setting.
    seen = {}
    for r in rows:
        if r["shadow"] is None:
            continue
        if r["shadow"] in seen:
            err("%s and %s both write cfg_shadow_t.%s -- one of them is wrong"
                % (seen[r["shadow"]], r["key"], r["shadow"]))
        seen[r["shadow"]] = r["key"]


def check_no_ladders():
    """No consumer may grow its own key list again.

    The whole property being bought is 'one place'. A K_* reference inside any
    of these functions means a second list has started forming, which is how
    all six drifted the first time."""
    src = strip_c_comments(read(DM_C_PATH))
    for sig, label in (
            (r"static int32_t\s+cfg_clamp\s*\(", "cfg_clamp()"),
            (r"static log_param_id_t\s+ns_key_to_log_id\s*\(", "ns_key_to_log_id()"),
            (r"static cfg_key_kind_t\s+cfg_key_kind\s*\(", "cfg_key_kind()"),
            (r"static bool\s+apply_config_update\s*\(", "apply_config_update()"),
            # The boot loaders were the seventh list. They are one-liners over
            # the descriptor now; a K_* creeping back means someone started
            # rebuilding the ladder by hand. K_TZ_STR and the status/ota string
            # keys are exempt -- strings are not in the descriptor by design.
            (r"static void\s+nvs_load_climate\s*\(", "nvs_load_climate()"),
            (r"static void\s+nvs_load_wind\s*\(", "nvs_load_wind()"),
            (r"static void\s+nvs_load_motor\s*\(", "nvs_load_motor()"),
            (r"static void\s+nvs_load_system\s*\(", "nvs_load_system()"),
            (r"static void\s+nvs_load_web\s*\(", "nvs_load_web()")):
        m = re.search(sig, src)
        if not m:
            fatal("could not find %s in data_manager.cpp" % label)
            continue
        i = src.index("{", m.end() - 1)
        depth, j = 0, i
        while j < len(src):
            if src[j] == "{":
                depth += 1
            elif src[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        body = src[i:j]
        # String-valued keys are legitimately named in the loaders: they never
        # travel through Q4 and are deliberately absent from the descriptor.
        STRING_KEYS = ("K_TZ_STR", "K_STATUS_URL", "K_STATUS_SECRET",
                       "K_LOG_LAST_UP", "K_OTA_URL", "K_OTA_SECRET")
        stray = sorted(set(re.findall(r"\bK_[A-Z0-9_]+\b", body)) - set(STRING_KEYS))
        if stray:
            err("%s references key constants directly (%s) -- the descriptor "
                "table is meant to be the only key list" % (label, ", ".join(stray)))


DEF_PATH = os.path.join(ROOT, "firmware", "config", "cfg_defaults.h")

LOADERS = ("cfg_load_group", "nvs_load_climate", "nvs_load_wind",
           "nvs_load_motor", "nvs_load_system", "nvs_load_web")


def default_values():
    """{DEF_X: int} from cfg_defaults.h. Hex-aware: DEF_STATUS_EXPOSE is 0x3F."""
    out = {}
    for name, val in re.findall(
            r"#define\s+((?:DEF|MOTOR)_[A-Z0-9_]+)\s+"
            r"(\(?-?(?:0[xX][0-9a-fA-F]+|\d+)\)?)\s*(?:/\*|$)",
            read(DEF_PATH), re.M):
        out[name] = int(val.strip("()"), 0)
    return out


def check_defaults(rows, defaults):
    """Every key with a home in the shadow must carry a resolvable default.

    gh#64 counted six tables; the nvs_load_*() helpers were a SEVENTH, carrying
    the key list AND the factory defaults. They are collapsed onto the
    descriptor now, so the risk is no longer a missing key -- cfg_load_group()
    walks the table, so it cannot miss one -- but a default that does not exist
    or sits outside the key's own clamp, which would put the unit somewhere the
    write path would have refused to put it.
    """
    for r in rows:
        if r["shadow"] is None:
            continue                     # ap_enable: NVS-only, nothing to restore
        tok = r["def_tok"]
        if re.fullmatch(r"-?(?:0[xX][0-9a-fA-F]+|\d+)", tok):
            val = int(tok, 0)
        elif tok in defaults:
            val = defaults[tok]
        else:
            err("%s: default %s is not defined in cfg_defaults.h" % (r["key"], tok))
            continue
        if r["min"] is not None and not (r["min"] <= val <= r["max"]):
            err("%s: factory default %s (%s) is outside its own clamp [%s, %s] -- "
                "a fresh unit would boot to a value POST /api/config would reject"
                % (r["key"], val, tok, r["min"], r["max"]))


# POST /api/web local variable -> the config key it validates.
WEB_VARS = {"interval": "status_intv_s", "log_h": "log_upload_h",
            "log_m": "log_upload_m", "enable": "status_enable",
            "log_rot": "log_upload_rot", "expose": "status_expose"}


def check_web_handler_bounds(rows, macros):
    """POST /api/web restates these six keys' bounds inline -- check they agree.

    /api/web does NOT go through Q4. It calls nvs_cfg_set_i32() directly and
    then dm_reload_web_cfg(), so it never reaches cfg_clamp() and never sees the
    descriptor. It validates instead, with its own copy of each bound written
    into the handler -- an EIGHTH place a bound lives.

    Three of the six are literals there (`enable > 1`, `log_rot > 1`,
    `expose > 0x3F`), so they cannot even drift together with cfg_limits.h the
    way the macro-valued ones would. The endpoint also REJECTS rather than
    clamping -- a deliberate difference, verified on hardware, and not what this
    checks; only the numbers have to match.
    """
    src = strip_c_comments(read(WS_PATH))
    m = re.search(r"static esp_err_t\s+web_post_handler\s*\(", src)
    if not m:
        fatal("could not find web_post_handler in web_server.cpp")
        return
    body = src[m.end():m.end() + 12000]
    by_key = {r["key"]: r for r in rows}

    # Hex alternative FIRST. Regex alternation is ordered, so `-?\d+` ahead of
    # it matches just the leading 0 of `0x3F` and status_expose reads as 0..0 --
    # which is the same mistake that hid status_expose's bounds from the
    # generator, made twice.
    _num = r"(-?0[xX][0-9a-fA-F]+|-?\d+|CFG_[A-Z0-9_]+)"
    seen = {}
    for var, lo, hi in re.findall(
            r"\b(\w+)\s*<\s*" + _num + r"\s*\|\|\s*\1\s*>\s*" + _num, body, re.S):
        key = WEB_VARS.get(var)
        if not key:
            continue

        def val(tok):
            if re.fullmatch(r"-?(?:0[xX][0-9a-fA-F]+|\d+)", tok):
                return int(tok, 0)
            return macros.get(tok)

        seen[key] = (val(lo), val(hi))

    for var, key in sorted(WEB_VARS.items()):
        row = by_key.get(key)
        if row is None:
            err("POST /api/web validates '%s', which is not a descriptor key" % key)
            continue
        got = seen.get(key)
        if got is None:
            err("POST /api/web writes %s but no bounds check for it was found "
                "-- it would store any int32 the way gh#57 part 1 did" % key)
        elif got != (row["min"], row["max"]):
            err("POST /api/web bounds %s for '%s' disagree with the descriptor's %s"
                % (list(got), key, [row["min"], row["max"]]))


def published_json(rows):
    """The payload GET /api/config/limits will emit, rendered as the firmware
    renders it: descriptor order, CFG_F_PUB rows only."""
    return [(r["key"], [r["min"], r["max"]]) for r in rows if r["published"]]


def mock_limits():
    """{key: [min, max]} from webUiMock/mock_server.py's CONFIG_LIMITS."""
    src = read(MOCK_PATH)
    m = re.search(r"CONFIG_LIMITS[^=]*=\s*\{(.*?)\n\}", src, re.S)
    if not m:
        fatal("could not find CONFIG_LIMITS in mock_server.py")
        return {}
    body = re.sub(r"#[^\n]*", "", m.group(1))
    out = {}
    for k, lo, hi in re.findall(
            r'"([a-z0-9_]+)"\s*:\s*\[\s*(-?\d+)\s*,\s*(-?\d+)\s*\]', body):
        out[k] = [int(lo), int(hi)]
    return out


def compare(label, want, have):
    """Bidirectional: a key on either side and not the other is a failure.
    A one-way check is how 2.5.0 shipped eleven keys that were in the shadow
    ladder and in neither the clamp nor the published limits."""
    wk, hk = set(want), set(have)
    for k in sorted(wk - hk):
        err("%s: missing '%s' (the descriptor publishes it)" % (label, k))
    for k in sorted(hk - wk):
        err("%s: has '%s', which the descriptor does not publish" % (label, k))
    for k in sorted(wk & hk):
        if list(want[k]) != list(have[k]):
            err("%s: '%s' is %s, descriptor says %s"
                % (label, k, have[k], list(want[k])))


# ------------------------------------------------------------------- main ---

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="print the descriptor table")
    ap.add_argument("--golden", metavar="FILE",
                    help="a captured GET /api/config/limits to diff against")
    args = ap.parse_args()

    macros = macro_values()
    kconsts = key_constants()
    fields = shadow_fields()
    rows = parse_desc(macros, kconsts)

    if _FATAL:
        for m in _FATAL:
            sys.stderr.write("FATAL: %s\n" % m)
        return 2

    check_bounds(rows)
    check_shadow(rows, fields)
    check_no_ladders()
    check_defaults(rows, default_values())
    check_web_handler_bounds(rows, macros)

    pub = dict(published_json(rows))
    compare("webUiMock/mock_server.py CONFIG_LIMITS", pub, mock_limits())

    if args.golden:
        try:
            with io.open(args.golden, "r", encoding="utf-8") as fh:
                compare("golden capture %s" % os.path.basename(args.golden),
                        pub, json.load(fh))
        except Exception as exc:                              # noqa: BLE001
            fatal("cannot read golden %s: %s" % (args.golden, exc))

    if args.verbose:
        print("%-16s %-8s %-13s %-3s %-24s %-12s %s"
              % ("key", "ns", "kind", "pub", "param_id", "bounds", "shadow"))
        print("-" * 100)
        for r in rows:
            print("%-16s %-8s %-13s %-3s %-24s %-12s %s%s"
                  % (r["key"], r["ns"].replace("NVS_NS_", "").lower(),
                     r["kind"].replace("CFG_KEY_", ""),
                     "y" if r["published"] else ".", r["param"],
                     "%s..%s" % (r["min"], r["max"]),
                     r["shadow"] or "-", "  +sun" if r["sun"] else ""))
        print("")

    if _FATAL:
        for m in _FATAL:
            sys.stderr.write("FATAL: %s\n" % m)
        return 2
    if _ERRORS:
        sys.stderr.write("cfg descriptor check FAILED (%d):\n" % len(_ERRORS))
        for m in _ERRORS:
            sys.stderr.write("  - %s\n" % m)
        return 1

    print("cfg descriptor OK: %d keys, %d published; bounds, shadow fields, "
          "mock server%s all agree"
          % (len(rows), len(pub), " and golden" if args.golden else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
