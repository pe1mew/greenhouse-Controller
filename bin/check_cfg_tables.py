#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_cfg_tables.py -- keep the config-key tables in step (gh#64).

Six independent tables carry a config-key list, and they drift. Every gap so far
has shipped: gh#53 (the LCD WiFi-AP toggle went dead), gh#57 part 1 (eleven keys
stored any int32, four of them feeding the day/night setpoint choice), gh#57
part 2 (a published bound wider than the consumer's own clamp) and gh#51 Group B
(a param unparsed for three minors).

    A  cfg_key_kind()                  data_manager.cpp   accepted? SHADOW|NVS_ONLY
    B  shadow ladder in
       apply_config_update()           data_manager.cpp   which cfg_shadow_t field
    C  cfg_clamp()                     data_manager.cpp   the bounds ENFORCED
    D  ns_key_to_log_id()              data_manager.cpp   audited? which param_id
    E  LIMITS_JSON                     web_server.cpp     the bounds PUBLISHED
    F  CONFIG_LIMITS                   webUiMock/mock_server.py   the dev GUI's copy of E

A key is expected in all six unless this file says otherwise. EXPECTED_GAPS
below declares every deliberate omission with a reason, and the check is
BIDIRECTIONAL: an undeclared gap fails, and so does a declared gap that no
longer exists. That second half is what stops the allow-list rotting into a
list of excuses.

Also verifies, in the same pass, that LIMITS_JSON expands to valid JSON with
sane ranges, and that the mock agrees with the firmware key-for-key. Both have
bitten: the limits literal is assembled from stringified macros, so a
parenthesised #define or a stray comma yields JSON that only fails in a browser.

Usage:
    python bin/check_cfg_tables.py            # quiet unless something is wrong
    python bin/check_cfg_tables.py -v         # print the full matrix
    python bin/check_cfg_tables.py --list-gaps

Exit code 0 = consistent, 1 = a table disagrees, 2 = a table could not be read
(treated as failure, never as a pass -- an unparseable table must not look clean).

Stdlib only. ASCII output only (Windows consoles here are cp1252).
"""

import argparse
import io
import json
import os
import re
import sys

TABLES = ("A", "B", "C", "D", "E", "F")
TABLE_NAME = {
    "A": "cfg_key_kind()",
    "B": "shadow ladder",
    "C": "cfg_clamp()",
    "D": "ns_key_to_log_id()",
    "E": "LIMITS_JSON",
    "F": "mock CONFIG_LIMITS",
}

# ---------------------------------------------------------------------------
# Declared, deliberate omissions. key -> (tables it is absent from, why)
#
# Adding a line here is a reviewable act: it asserts "this key does not belong
# in that table". Removing a gap means deleting its line. Do NOT add a key here
# to silence a failure you have not understood -- gh#57 part 1's eleven keys
# were indistinguishable from these until someone read the consumers.
# ---------------------------------------------------------------------------
EXPECTED_GAPS = {
    # -- deliberately unaudited: ns_key_to_log_id() says so in its own docstring
    "ap_timeout": ("D", "Deliberately unaudited; no LOG_PARAM_* id assigned."),
    "session_timeout": ("D", "Deliberately unaudited; no LOG_PARAM_* id assigned."),
    "led_day_brt": ("D", "Deliberately unaudited (led_* carry no audit row)."),
    "led_nite_brt": ("D", "Deliberately unaudited (led_* carry no audit row)."),
    "led_nite_from": ("D", "Deliberately unaudited (led_* carry no audit row)."),
    "led_nite_to": ("D", "Deliberately unaudited (led_* carry no audit row)."),

    # -- owned by POST /api/web, never shown on the generic config form, so the
    #    generic limits endpoint does not advertise them
    "status_enable": ("EF", "Set via POST /api/web; not on the generic config form."),
    "status_expose": ("EF", "Set via POST /api/web; not on the generic config form."),
    "status_intv_s": ("EF", "Set via POST /api/web; not on the generic config form."),
    "log_upload_h": ("EF", "Set via POST /api/web; not on the generic config form."),
    "log_upload_m": ("EF", "Set via POST /api/web; not on the generic config form."),
    "log_upload_rot": ("EF", "Set via POST /api/web; not on the generic config form."),

    # -- the one NVS-only Q4 key: T10 polls NVS for it, so there is no shadow
    #    field to update and no audit row. gh#53 is why cfg_key_kind() is
    #    three-way rather than a boolean.
    "ap_enable": ("BDEF", "NVS-only (gh#53): T10 polls NVS, so no shadow field and no "
                          "audit row; set from the LCD System menu or /api/config."),

    # -- ROTA settings are owned by POST /api/ota/config, which REJECTS
    #    out-of-range values using the SAME CFG_* constants rather than
    #    clamping. They never reach Q4, so their cfg_clamp() entries are
    #    unreachable from that path and cfg_key_kind() does not accept them.
    "ota_enable": ("ABEF", "Owned by POST /api/ota/config, which rejects out-of-range "
                           "against the same CFG_* constants; never reaches Q4."),
    "ota_check_h": ("ABEF", "Owned by POST /api/ota/config (rejects, does not clamp)."),
    "ota_win_lo": ("ABEF", "Owned by POST /api/ota/config (rejects, does not clamp)."),
    "ota_win_hi": ("ABEF", "Owned by POST /api/ota/config (rejects, does not clamp)."),
}

DM_PATH = os.path.join("firmware", "src", "data_manager", "data_manager.cpp")
WS_PATH = os.path.join("firmware", "src", "web_server", "web_server.cpp")
MK_PATH = os.path.join("webUiMock", "mock_server.py")
LIM_PATH = os.path.join("firmware", "config", "cfg_limits.h")

_ERRORS = []
_FATAL = []


def err(msg):
    _ERRORS.append(msg)


def fatal(msg):
    _FATAL.append(msg)


def read(path):
    try:
        return io.open(path, encoding="utf-8", newline="").read()
    except Exception as exc:            # pragma: no cover - environment issue
        fatal("cannot read %s: %s" % (path, exc))
        return ""


def strip_c_comments(text):
    """Remove C comments. MUST run before splitting on any C token: a ';' or a
    '{' inside a comment truncates a naive extractor, which is exactly how the
    2.5.0 pre-flight check silently lost eleven JSON entries on its first run."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def fn_body(src, signature_re, label):
    """Return the brace-balanced body of the first function matching a regex."""
    m = re.search(signature_re, src)
    if not m:
        fatal("could not locate %s -- has it been renamed? (pattern: %s)"
              % (label, signature_re))
        return ""
    i = src.find("{", m.end() - 1)
    if i < 0:
        fatal("could not find the opening brace of %s" % label)
        return ""
    depth, j = 0, i
    while j < len(src):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
        j += 1
    fatal("unbalanced braces while reading %s" % label)
    return ""


def keys_referenced(text, kmap):
    """Keys named either through a K_* literal or compared as a bare string."""
    found = set()
    for macro in re.findall(r"\bK_[A-Z0-9_]+\b", text):
        if macro in kmap:
            found.add(kmap[macro])
    for lit in re.findall(r'strcmp\(\s*key(?:_str)?\s*,\s*"([^"]+)"', text):
        found.add(lit)
    return found


def collect():
    """Build {table letter: set of keys}. Fatal on anything unreadable."""
    dm = strip_c_comments(read(DM_PATH))
    ws = read(WS_PATH)
    mk = read(MK_PATH)
    if _FATAL:
        return {}

    kmap = dict(re.findall(
        r'static const char (K_[A-Z0-9_]+)\[\]\s*=\s*"([^"]+)"', dm))
    if len(kmap) < 30:
        fatal("only %d K_* key literals found in %s; the declaration style has "
              "probably changed" % (len(kmap), DM_PATH))
        return {}

    sets = {
        "A": keys_referenced(fn_body(dm, r"cfg_key_kind_t\s+cfg_key_kind\s*\(",
                                     "cfg_key_kind()"), kmap),
        "B": keys_referenced(fn_body(dm, r"bool\s+apply_config_update\s*\(",
                                     "apply_config_update()"), kmap),
        "C": keys_referenced(fn_body(dm, r"int32_t\s+cfg_clamp\s*\(",
                                     "cfg_clamp()"), kmap),
        "D": keys_referenced(fn_body(dm, r"log_param_id_t\s+ns_key_to_log_id\s*\(",
                                     "ns_key_to_log_id()"), kmap),
        "E": limits_json_keys(ws),
        "F": mock_limits_keys(mk),
    }
    for letter, s in sets.items():
        if not s:
            fatal("table %s (%s) came back EMPTY -- refusing to report a pass"
                  % (letter, TABLE_NAME[letter]))
    return sets


def limits_literal(ws):
    """The LIMITS_JSON initialiser, comments stripped BEFORE the ';' split."""
    marker = "static const char LIMITS_JSON[] ="
    if marker not in ws:
        fatal("could not find %s in %s" % (marker, WS_PATH))
        return ""
    tail = ws.split(marker, 1)[1]
    return strip_c_comments(tail).split(";", 1)[0]


def limits_json_keys(ws):
    body = limits_literal(ws)
    return set(re.findall(r'\\"([a-z0-9_]+)\\"\s*:', body))


def mock_limits_keys(mk):
    marker = "CONFIG_LIMITS: dict[str, list[int]] = "
    if marker not in mk:
        fatal("could not find %s in %s" % (marker, MK_PATH))
        return set()
    tail = mk.split(marker, 1)[1]
    depth, end = 0, None
    for i, ch in enumerate(tail):
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                end = i + 1
                break
    if end is None:
        fatal("unbalanced braces in %s CONFIG_LIMITS" % MK_PATH)
        return set()
    try:
        return set(eval(tail[:end], {"__builtins__": {}}, {}).keys())
    except Exception as exc:
        fatal("CONFIG_LIMITS in %s is not a literal dict: %s" % (MK_PATH, exc))
        return set()


def check_limits_payload():
    """Expand LIMITS_JSON the way the preprocessor does and json.loads it, then
    compare against the mock value-for-value."""
    lim = read(LIM_PATH)
    ws = read(WS_PATH)
    mk = read(MK_PATH)
    if _FATAL:
        return

    defs = dict(re.findall(
        r"#define\s+(CFG_(?:MIN|MAX)_[A-Z0-9_]+)\s+(\(?-?\d+\)?)", lim))
    body = limits_literal(ws)
    token = re.compile(r'"((?:[^"\\]|\\.)*)"' + r"|(CFG_(?:MIN|MAX)_[A-Z0-9_]+)")

    out = []
    for lit, macro in token.findall(body):
        if macro:
            if macro not in defs:
                err("LIMITS_JSON references %s, which is not #define'd in %s"
                    % (macro, LIM_PATH))
                out.append("0")
                continue
            val = defs[macro]
            if val.startswith("("):
                err("%s is parenthesised in %s, so it stringifies as %s and "
                    "makes LIMITS_JSON invalid JSON" % (macro, LIM_PATH, val))
            out.append(val)
        else:
            out.append(lit.replace('\\"', '"'))

    text = "".join(out)
    try:
        fw = json.loads(text)
    except ValueError as exc:
        err("LIMITS_JSON does not expand to valid JSON: %s" % exc)
        return

    for key, rng in sorted(fw.items()):
        if not (isinstance(rng, list) and len(rng) == 2
                and all(isinstance(v, int) for v in rng)):
            err("published limit for %s is not a two-integer range: %r" % (key, rng))
        elif rng[0] > rng[1]:
            err("published limit for %s is inverted: min %d > max %d"
                % (key, rng[0], rng[1]))

    marker = "CONFIG_LIMITS: dict[str, list[int]] = "
    tail = mk.split(marker, 1)[1]
    depth, end = 0, None
    for i, ch in enumerate(tail):
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                end = i + 1
                break
    try:
        mock = eval(tail[:end], {"__builtins__": {}}, {})
    except Exception:
        return
    for key in sorted(set(fw) & set(mock)):
        if list(fw[key]) != list(mock[key]):
            err("%s: firmware publishes %r but the mock says %r -- the dev GUI "
                "would validate against the wrong bound"
                % (key, fw[key], mock[key]))


def check_matrix(sets, verbose):
    universe = sorted(set().union(*sets.values()))

    if verbose:
        print("%-17s %s" % ("key", "  ".join(TABLES)))
        print("-" * 44)
        for key in universe:
            print("%-17s %s" % (key, "  ".join(
                "y" if key in sets[t] else "." for t in TABLES)))
        print()

    for key in universe:
        actual = "".join(t for t in TABLES if key not in sets[t])
        declared, why = EXPECTED_GAPS.get(key, ("", None))
        declared = "".join(t for t in TABLES if t in declared)

        for t in actual:
            if t not in declared:
                err("%-16s is MISSING from table %s (%s) and that gap is not "
                    "declared.\n%s  If deliberate, add it to EXPECTED_GAPS in "
                    "bin/check_cfg_tables.py with a reason. If not, this is the "
                    "gh#57 defect class: read the key's consumer and give it a "
                    "bound, an audit id or a published range."
                    % (key, t, TABLE_NAME[t], " " * 20))
        for t in declared:
            if t not in actual:
                err("%-16s IS now present in table %s (%s), but EXPECTED_GAPS "
                    "still declares it absent.\n%s  Delete that entry -- a stale "
                    "allow-list is how this check stops working.\n%s  Recorded "
                    "reason was: %s"
                    % (key, t, TABLE_NAME[t], " " * 20, " " * 20, why))

    for key in sorted(EXPECTED_GAPS):
        if key not in universe:
            err("EXPECTED_GAPS names %r, which appears in no table at all. Was "
                "the key removed? Delete the entry." % key)
    return universe


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="print the full key/table matrix")
    ap.add_argument("--list-gaps", action="store_true",
                    help="print the declared gaps and exit")
    args = ap.parse_args()

    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(here)

    if args.list_gaps:
        print("declared deliberate gaps (%d):\n" % len(EXPECTED_GAPS))
        for key in sorted(EXPECTED_GAPS):
            tabs, why = EXPECTED_GAPS[key]
            print("  %-16s absent from %-5s %s"
                  % (key, tabs, TABLE_NAME.get(tabs[0], "") if len(tabs) == 1 else ""))
            print("  %-16s %s" % ("", why))
        return 0

    sets = collect()
    if _FATAL:
        print("check_cfg_tables: CANNOT VERIFY")
        for m in _FATAL:
            print("  FATAL: %s" % m)
        print("\nA table that cannot be read is not a passing table. Fix the "
              "parser or the source before committing.")
        return 2

    universe = check_matrix(sets, args.verbose)
    check_limits_payload()

    print("check_cfg_tables: %d keys across %d tables (%s)"
          % (len(universe), len(TABLES),
             ", ".join("%s=%d" % (t, len(sets[t])) for t in TABLES)))
    print("                  %d declared gap(s)" % len(EXPECTED_GAPS))

    if _ERRORS:
        print("\n%d PROBLEM(S):\n" % len(_ERRORS))
        for m in _ERRORS:
            print("  * %s" % m)
        print("\nBackground: gh#64 (one descriptor table would make this class "
              "impossible rather than repeatedly fixable).")
        return 1

    print("                  OK -- every key is in every table it should be in")
    return 0


if __name__ == "__main__":
    sys.exit(main())
