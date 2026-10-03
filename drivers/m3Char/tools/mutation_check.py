#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Break each rule of the library in a copy, and check the host tests notice.

The host tests replay the rig's archived data, and a replay is evidence only
for the cases its data contains. On 2026-10-03 the first suite reproduced every
figure the harnesses printed, and still passed with two rules broken: no
archived displacement sits exactly on a threshold, and no shorter width ever
succeeded after a longer one failed. This script is how that was found, and it
keeps the suite honest when the library or its tests change.

For each mutation below: copy src/m3_char.cpp with ONE rule broken, compile it
with the unchanged tests, run, and require the run to FAIL. A mutation that
survives is a rule no test checks -- add a test, do not delete the mutation.

Needs: the Code::Blocks MinGW toolchain (as set_compiler.py), and Unity from
one `pio test -e native` run (.pio/libdeps/native/Unity). Builds in a
temporary directory; the repository is not touched.

Usage
  python drivers/m3Char/tools/mutation_check.py
"""
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.dirname(HERE)
MINGW = [r"C:\Program Files\CodeBlocks\MinGW\bin",
         r"C:\Program Files (x86)\CodeBlocks\MinGW\bin",
         r"C:\CodeBlocks\MinGW\bin"]

# (what the mutation breaks, the exact source text, its broken replacement)
MUTATIONS = [
    ("moved: more than the threshold becomes at least",
     "* 100 > (int64_t)thresh_x1000", "* 100 >= (int64_t)thresh_x1000"),
    ("floor 2: no longer an unbroken run",
     "ok_so_far = ok_so_far && (moved == cnt);", "ok_so_far = (moved == cnt);"),
    ("reversals: a pulse that measured nothing sets the direction",
     "if (p[i].valid) { prev = p[i].opening; }", "prev = p[i].opening;"),
    ("AT-WP02: a 2.0 % spread fails",
     "out->pass        = (mx - mn) <= M3C_WP02_PASS_X10;",
     "out->pass        = (mx - mn) < M3C_WP02_PASS_X10;"),
    ("noise: population sigma",
     "sqrt(ss / (double)(n - 1u))", "sqrt(ss / (double)n)"),
    ("fit: the commanded width, not the real one",
     "return (double)((p[i].width_us != 0u) ? p[i].width_us",
     "return (double)((false) ? p[i].width_us"),
    ("reversal loss: take-up pulses left out",
     "if (!p[i].valid || p[i].opening != opening || p[i].req_ms != req_ms) { continue; }",
     "if (!p[i].valid || p[i].takeup || p[i].opening != opening "
     "|| p[i].req_ms != req_ms) { continue; }"),
    ("hysteresis: sign flipped",
     "((double)sum_b / (double)n_b - (double)sum_a / (double)n_a)",
     "((double)sum_a / (double)n_a - (double)sum_b / (double)n_b)"),
    ("band candidate: rounded down",
     "((int64_t)best + 99) / 100", "((int64_t)best) / 100"),
    ("band check: a raise not rounded up",
     "M3C_CHECK_FACTOR_PCT + 99u) / 100u", "M3C_CHECK_FACTOR_PCT) / 100u"),
    ("speed: the whole move, not its middle 60 %",
     "return (double)s[i].pos_x10 >= lo && (double)s[i].pos_x10 <= hi;",
     "(void)lo; (void)hi; return true;"),
    ("width search: one midpoint only",
     "s->refined >= 2u", "s->refined >= 1u"),
]


def find_mingw():
    for p in MINGW:
        if os.path.exists(os.path.join(p, "g++.exe")):
            return p
    sys.exit("mutation_check: Code::Blocks MinGW not found (see set_compiler.py)")


def main():
    mingw = find_mingw()
    env = dict(os.environ)
    env["PATH"] = mingw + os.pathsep + env.get("PATH", "")
    unity = os.path.join(LIB, ".pio", "libdeps", "native", "Unity", "src")
    if not os.path.exists(os.path.join(unity, "unity.c")):
        sys.exit("mutation_check: run `pio test -e native` once first (it fetches Unity)")
    src = io.open(os.path.join(LIB, "src", "m3_char.cpp"), encoding="utf-8").read()
    test = os.path.join(LIB, "test", "test_m3_char", "test_m3_char.cpp")
    out = tempfile.mkdtemp(prefix="m3char_mut_")
    try:
        subprocess.check_call([os.path.join(mingw, "gcc.exe"), "-c", "-O1", "-I", unity,
                               os.path.join(unity, "unity.c"), "-o",
                               os.path.join(out, "unity.o")], env=env)

        def run(code):
            cpp = os.path.join(out, "m3_char.cpp")
            io.open(cpp, "w", encoding="utf-8").write(code)
            exe = os.path.join(out, "t.exe")
            subprocess.check_call([os.path.join(mingw, "g++.exe"), "-std=c++17", "-O1",
                                   "-DUNIT_TEST", "-I", os.path.join(LIB, "src"), "-I", unity,
                                   "-I", os.path.dirname(test), test, cpp,
                                   os.path.join(out, "unity.o"), "-o", exe], env=env)
            p = subprocess.run([exe], env=env, capture_output=True, text=True)
            return p.returncode, re.findall(r":(test_\w+):FAIL", p.stdout)

        rc, fails = run(src)
        if rc != 0:
            sys.exit("mutation_check: the UNMUTATED library fails: %s" % ", ".join(fails))
        survived = 0
        for what, a, b in MUTATIONS:
            if src.count(a) != 1:
                sys.exit("mutation_check: '%s' -- its source text is no longer unique "
                         "(or gone); update the mutation" % what)
            rc, fails = run(src.replace(a, b))
            if rc == 0:
                survived += 1
            print("%-58s %s  %s" % (what, "caught  " if rc else "SURVIVED",
                                    ", ".join(fails[:3])))
        print("\n%d of %d mutations caught" % (len(MUTATIONS) - survived, len(MUTATIONS)))
        sys.exit(1 if survived else 0)
    finally:
        shutil.rmtree(out, ignore_errors=True)


if __name__ == "__main__":
    main()
