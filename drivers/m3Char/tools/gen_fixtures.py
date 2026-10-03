#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Generate test/test_m3_char/fixtures.h from the rig's archived raw data.

The host tests must reproduce what the bench harnesses PRINTED for these runs,
so this script carries two things into C for every run in test/data/:
  - the raw data the harness analysed (its CSV, or the resting positions it
    logged), as the library will receive it on the unit: widths in us,
    displacements and positions in 0.1 mm or 0.1 %;
  - the figures the harness printed (parsed from its log), as the expected
    values.
Nothing is recomputed here: an expected value is a printed one, so a test that
passes means the C arithmetic agrees with the harness, not with this script.

One harness detail is carried rather than guessed: the noise at rest. Before
2026-10-02 ~12:10 the harness took sigma over all six readings, and from run
1002b on it dropped the first, which can catch the leaf still settling. Which
rule a run used is decided here by which one reproduces its printed sigma; a
run that matches neither is an error.

Usage
  python drivers/m3Char/tools/gen_fixtures.py          # from anywhere
"""
import csv
import io
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DATA = os.path.join(ROOT, "test", "data")
OUT = os.path.join(ROOT, "test", "test_m3_char", "fixtures.h")

MINMOVE_RUNS = [
    ("1002", "minmove_1002",
     "first sweep, 12:05, no take-up pulses: its short widths measured the rope's slack, "
     "so it is the reversal data, not floor 2"),
    ("1002b", "minmove_1002b",
     "12:16, INVALID: the wind safe-fail closed M3 and the harness pulsed it against its end "
     "switch -- kept as the run nothing should be derived from"),
    ("1002d", "minmove_1002d",
     "12:34, the recorded run (plan 3.6): take-up pulses, 2 passes x 5 per width"),
]
WP02_RUNS = [
    ("cuts_run1", "wp02_cuts_run1", "2026-10-01 ~18:50, step 1 stop log, the old stop rule"),
    ("cuts_run2", "wp02_cuts_run2", "the same build, second run"),
    ("ff8192", "wp02_ff8192", "fail-first bit 8192: the old rule on the step-2 build"),
    ("extrap_run1", "wp02_extrap_run1", "step 2, the 420 ms default lead, leads still learning"),
    ("extrap_run2", "wp02_extrap_run2", "step 2, the 420 ms default, leads learned"),
    ("final", "wp02_final", "step 2 with the 250 ms default, from a boot (2.15.1's code)"),
]


def die(msg):
    sys.exit("gen_fixtures: " + msg)


def x10(v):
    return int(round(float(v) * 10.0))


def x100(v):
    return int(round(float(v) * 100.0))


def sd(xs):
    if len(xs) < 2:
        return 0.0
    m = sum(xs) / float(len(xs))
    return math.sqrt(sum((x - m) ** 2 for x in xs) / (len(xs) - 1))


def read_pulses(path):
    out = []
    with io.open(path, newline="") as f:
        for r in csv.DictReader(f):
            disp = r.get("disp_mm", "")
            valid = disp not in ("", "None")
            out.append({
                "req_ms": int(r["req_ms"]),
                "width_us": int(round(float(r["width_ms"] or 0) * 1000.0)),
                "disp_x10": x10(disp) if valid else 0,
                "opening": r["opening"] == "True",
                "takeup": r.get("takeup", "False") == "True",
                "valid": valid,
            })
    return out


ROW_RE = re.compile(r"^\s+(\d+)\s+([\d.]+)\s+(\d+)\s+(-?[\d.]+) /\s+(-?[\d.]+) /\s+(-?[\d.]+)"
                    r"\s+(-?[\d.]+)\s+(\d+)/(\d+)\s*$")
FIT_RE = re.compile(r"fit over (\d+) pulses: ([\d.]+) mm/s .*dead time (-?\d+) ms")
F2_RE = re.compile(r"floor 2: (\d+) ms commanded \(([\d.]+) ms real\).*: ([\d.]+) mm,")
REV_RE = re.compile(r"^\s+(open|close)\s+(\d+) ms: after a reversal\s+(-?[\d.]+) mm \(n (\d+)\)"
                    r"(?:, same direction\s+(-?[\d.]+) mm \(n (\d+)\): ([+-][\d.]+) mm"
                    r"|; no same-direction)")
NOISE_RE = re.compile(r"noise at rest: sigma ([\d.]+) mm .* more than ([\d.]+) mm")
REST_RE = re.compile(r"at rest: (.*) mm\s*$")


def read_minmove_log(path):
    with io.open(path) as f:
        lines = f.read().splitlines()
    rest, noise, thresh = None, None, None
    rev, sect, exp = [], None, {}
    for ln in lines:
        m = REST_RE.search(ln)
        if m and rest is None:
            rest = [float(v) for v in m.group(1).split(",")]
            continue
        m = NOISE_RE.search(ln)
        if m:
            noise, thresh = float(m.group(1)), float(m.group(2))
            continue
        m = REV_RE.match(ln)
        if m:
            rev.append({
                "opening": m.group(1) == "open", "req_ms": int(m.group(2)),
                "rev": x100(m.group(3)), "n_rev": int(m.group(4)),
                "has_same": m.group(5) is not None,
                "same": x100(m.group(5)) if m.group(5) else 0,
                "n_same": int(m.group(6)) if m.group(6) else 0,
                "diff": x100(m.group(7)) if m.group(7) else 0,
            })
            continue
        if ln.startswith("OPEN:") or ln.startswith("CLOSE:"):
            sect = ln.startswith("OPEN:")
            exp[sect] = {"rows": [], "fit": None, "floor2": None}
            continue
        if sect is None:
            continue
        m = ROW_RE.match(ln)
        if m:
            exp[sect]["rows"].append({
                "req_ms": int(m.group(1)), "real_x10": x10(m.group(2)), "n": int(m.group(3)),
                "mean_x100": x100(m.group(4)), "moved": int(m.group(8)),
            })
            if int(m.group(9)) != int(m.group(3)):
                die("%s: a row's n and moved-of differ" % path)
            continue
        m = FIT_RE.search(ln)
        if m:
            exp[sect]["fit"] = (int(m.group(1)), x10(m.group(2)), int(m.group(3)))
            continue
        m = F2_RE.search(ln)
        if m:
            exp[sect]["floor2"] = (int(m.group(1)), x10(m.group(2)), x100(m.group(3)))
    if rest is None or noise is None:
        die("%s: no rest readings or noise line" % path)
    # Which noise rule did this run use? The one that reproduces its sigma.
    drop = None
    for d in (0, 1):
        if abs(round(sd(rest[d:]), 2) - noise) < 0.005:
            drop = d
            break
    if drop is None:
        die("%s: neither noise rule gives the printed sigma %.2f" % (path, noise))
    if True not in exp or False not in exp:
        die("%s: missing the OPEN or CLOSE table" % path)
    return {"rest": [x10(v) for v in rest], "drop": drop, "noise_x100": x100(noise),
            "thresh_x10": x10(thresh), "rev": rev, "exp": exp}


APPROACH_RE = re.compile(r"approach\s+(\d+) from (below|above):\s+([\d.]+) %")
MEAN_RE = re.compile(r"mean\s+: ([\d.]+) %\s+\(target ([\d.]+) %, error ([+-][\d.]+) %\)")
SPREAD_RE = re.compile(r"spread\s+: ([\d.]+) %")
HYST_RE = re.compile(r"hysteresis\s+: ([+-][\d.]+) %")
VERDICT_RE = re.compile(r"AT-WP02\s+(PASS|FAIL)")


def read_wp02_log(path):
    with io.open(path) as f:
        text = f.read()
    ap = [(m.group(2) == "below", x10(m.group(3))) for m in APPROACH_RE.finditer(text)]
    mm, sp, hy, vd = (MEAN_RE.search(text), SPREAD_RE.search(text), HYST_RE.search(text),
                      VERDICT_RE.search(text))
    if len(ap) != 10 or not (mm and sp and hy and vd):
        die("%s: expected ten approaches and the four result lines" % path)
    return {"ap": ap, "mean_x10": x10(mm.group(1)), "target_x10": x10(mm.group(2)),
            "err_x10": x10(mm.group(3)), "spread_x10": x10(sp.group(1)),
            "hyst_x10": x10(hy.group(1)), "pass": vd.group(1) == "PASS"}


def c_bool(b):
    return "true" if b else "false"


def emit():
    o = []
    w = o.append
    w("/* GENERATED by drivers/m3Char/tools/gen_fixtures.py from test/data/ -- do not edit.")
    w(" * Regenerate after changing the data or the script. Expected values are the")
    w(" * figures bin/at_wp_minmove.py and bin/at_wp02.py PRINTED for these runs. */")
    w("#ifndef M3C_FIXTURES_H")
    w("#define M3C_FIXTURES_H")
    w("")
    w("#include <stddef.h>")
    w("#include \"m3_char.h\"")
    w("")
    w("typedef struct { uint16_t req_ms; uint8_t n; uint8_t moved; uint16_t real_x10; "
      "int32_t mean_x100; } fx_row_t;")
    w("typedef struct {")
    w("    bool     opening;")
    w("    uint8_t  n_rows;")
    w("    fx_row_t rows[M3C_MAX_WIDTHS];")
    w("    bool     has_fit;   uint16_t fit_n; uint16_t speed_x10; int16_t dead_ms;")
    w("    bool     has_floor2; uint16_t floor2_ms; uint16_t floor2_real_x10; "
      "int32_t floor2_disp_x100;")
    w("} fx_minmove_expect_t;")
    w("typedef struct { bool opening; uint16_t req_ms; uint8_t n_rev; int32_t rev_x100; "
      "bool has_same; uint8_t n_same; int32_t same_x100; int32_t diff_x100; } fx_rev_t;")
    w("typedef struct {")
    w("    const char *name;")
    w("    const char *what;")
    w("    const m3c_pulse_t *pulses; size_t n_pulses;")
    w("    const int32_t *rest_x10; size_t n_rest; size_t rest_drop;")
    w("    uint16_t noise_x100;  /* printed sigma, 0.01 mm */")
    w("    uint16_t thresh_x10;  /* printed threshold, 0.1 mm */")
    w("    fx_minmove_expect_t expect[2];  /* [0] OPEN, [1] CLOSE */")
    w("    const fx_rev_t *rev; size_t n_rev;")
    w("} fx_minmove_run_t;")
    w("typedef struct {")
    w("    const char *name;")
    w("    const char *what;")
    w("    int16_t rest_x10[10]; bool below[10];")
    w("    int16_t target_x10; int16_t mean_x10; int16_t err_x10; int16_t spread_x10;")
    w("    int16_t hyst_x10; bool pass;")
    w("} fx_wp02_run_t;")
    w("")

    runs = []
    for tag, base, what in MINMOVE_RUNS:
        pulses = read_pulses(os.path.join(DATA, base + ".csv"))
        lg = read_minmove_log(os.path.join(DATA, base + ".log"))
        runs.append((tag, what, pulses, lg))
        w("/* %s.csv: %d pulses. %s */" % (base, len(pulses), what))
        w("static const m3c_pulse_t MM_%s_PULSES[] = {" % tag.upper())
        for p in pulses:
            w("    { %d, %d, %d, %s, %s, %s, false }," % (
                p["req_ms"], p["width_us"], p["disp_x10"], c_bool(p["opening"]),
                c_bool(p["takeup"]), c_bool(p["valid"])))
        w("};")
        w("static const int32_t MM_%s_REST[] = { %s };" % (
            tag.upper(), ", ".join(str(v) for v in lg["rest"])))
        if lg["rev"]:
            w("static const fx_rev_t MM_%s_REV[] = {" % tag.upper())
            for r in lg["rev"]:
                w("    { %s, %d, %d, %d, %s, %d, %d, %d }," % (
                    c_bool(r["opening"]), r["req_ms"], r["n_rev"], r["rev"],
                    c_bool(r["has_same"]), r["n_same"], r["same"], r["diff"]))
            w("};")
        w("")

    w("static const fx_minmove_run_t MM_RUNS[] = {")
    for tag, what, pulses, lg in runs:
        w("    {")
        w("        \"%s\", \"%s\"," % (tag, what.replace("\"", "'")))
        w("        MM_%s_PULSES, sizeof(MM_%s_PULSES) / sizeof(MM_%s_PULSES[0])," % (
            tag.upper(), tag.upper(), tag.upper()))
        w("        MM_%s_REST, %d, %d, %d, %d," % (tag.upper(), len(lg["rest"]), lg["drop"],
                                                lg["noise_x100"], lg["thresh_x10"]))
        w("        {")
        for opening in (True, False):
            e = lg["exp"][opening]
            rows = ", ".join("{ %d, %d, %d, %d, %d }" % (r["req_ms"], r["n"], r["moved"],
                                                       r["real_x10"], r["mean_x100"])
                             for r in e["rows"])
            fit = e["fit"] or (0, 0, 0)
            f2 = e["floor2"] or (0, 0, 0)
            w("            { %s, %d, { %s }," % (c_bool(opening), len(e["rows"]), rows))
            w("              %s, %d, %d, %d, %s, %d, %d, %d }," % (
                c_bool(e["fit"] is not None), fit[0], fit[1], fit[2],
                c_bool(e["floor2"] is not None), f2[0], f2[1], f2[2]))
        w("        },")
        if lg["rev"]:
            w("        MM_%s_REV, sizeof(MM_%s_REV) / sizeof(MM_%s_REV[0])," % (
                tag.upper(), tag.upper(), tag.upper()))
        else:
            w("        NULL, 0,")
        w("    },")
    w("};")
    w("")

    w("static const fx_wp02_run_t WP02_RUNS[] = {")
    for tag, base, what in WP02_RUNS:
        r = read_wp02_log(os.path.join(DATA, base + ".log"))
        w("    { \"%s\", \"%s\"," % (tag, what))
        w("      { %s }," % ", ".join(str(p) for _, p in r["ap"]))
        w("      { %s }," % ", ".join(c_bool(b) for b, _ in r["ap"]))
        w("      %d, %d, %d, %d, %d, %s }," % (r["target_x10"], r["mean_x10"], r["err_x10"],
                                            r["spread_x10"], r["hyst_x10"], c_bool(r["pass"])))
    w("};")
    w("")
    w("#endif /* M3C_FIXTURES_H */")
    return "\n".join(o) + "\n"


def main():
    text = emit()
    with io.open(OUT, "w", newline="\n") as f:
        f.write(text)
    print("wrote %s (%d lines)" % (os.path.relpath(OUT, ROOT), text.count("\n")))


if __name__ == "__main__":
    main()
