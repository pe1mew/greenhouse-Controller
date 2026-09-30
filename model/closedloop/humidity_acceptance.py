"""humidity_acceptance.py -- the gh#84 acceptance table, against the law as shipped.

humidity_prototype.py `loop` produced the acceptance table by comparing a
PATCHED copy of the v1 law (the prototype) with v1 itself. Since firmware
2.15.0 the library IS v2: the prototype's patches no longer apply ("patch
anchor found 0 times"), and vm.load() returns v2. This re-runs the same table
with the real libraries:

  v1  the pre-gh#84 law: vent_model_stepped.cpp and vent_model_graded.cpp
      from commit V1_REV, compiled against TODAY's interface-3 header and
      FFI shim. v1 never reads t_min_c10, so it decides exactly as before.
  v2  drivers/ventModel/src as it is (vm.load()). Its humidity floor comes
      from the t_min_day / t_min_ngt settings (interface 3), not from a
      compiled-in constant: 16/14 for 5C88, 18/16 for the manual's tomato row,
      the same floors the prototype compiled in.

Everything else is the prototype's `loop`, called unchanged through
humidity_prototype._run_configs(): the primary plant, 2026-06-05..09-16, RH
from the log, the same metrics. The acceptance rows are the prototype's A1-A4
plus A5: at priority 0 v2 decides exactly as v1.

**Run 2026-09-30 against 2.15.0: all ten rows PASS**, and v2's priority-1
figures equal the prototype's to the digit (mode 1 / mode 2 / tomato): hours
>= 31 C 2.3 / 2.4 / 2.0, night hours below t_min 1.3 / 1.3 / 3.8,
humidity-only venting 8.5 / 8.5 / 3.9 h a day, M1+M2 drives 15.5 / 15.3 /
15.1 a day. Recorded in bin/2.15.0/release-notes.md.

When the law changes again: move V1_REV to the commit before the change (and
rename the baseline), and replace PROTO with the figures the change is meant
to reproduce.

    python model/closedloop/humidity_acceptance.py check   build v1, print both libraries' laws
    python model/closedloop/humidity_acceptance.py run     the nine closed-loop runs (~30 min)

Exit 0 = every acceptance row passes, 1 = one fails.
"""
import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
import ventmodel as vm                                      # noqa: E402

V1_REV = "b393901"                 # the last commit before gh#84 (stepped v1, graded v1)
T_MIN_5C88 = ["t_min_day=16", "t_min_ngt=14"]
T_MIN_TOMATO = ["t_min_day=18", "t_min_ngt=16"]            # the manual's tomato row
# The prototype's figures at priority 1 (humidityControl.md, "The package,
# prototyped"): mode 1 / mode 2 / tomato.
PROTO = {"hours >= 31 degC a day": (2.3, 2.4, 2.0),
         "night hours below t_min": (1.3, 1.3, 3.8),
         "humidity-only venting, h a day": (8.5, 8.5, 3.9),
         "M1+M2 drives a day": (15.5, 15.3, 15.1)}
TOL = 0.1


def build_v1():
    """V1_REV's two law sources + today's header and shim, compiled once.

    Named by a hash of every source, like humidity_prototype.build(), so an
    unchanged baseline reuses its DLL (Windows will not overwrite a loaded
    one); under build/, where *.dll is git-ignored."""
    srcs = {}
    for name in ("vent_model_stepped.cpp", "vent_model_graded.cpp"):
        srcs[name] = subprocess.run(["git", "show", "%s:drivers/ventModel/src/%s" % (V1_REV, name)],
                                    cwd=REPO, capture_output=True, text=True, encoding="utf-8",
                                    check=True).stdout
    h = hashlib.sha1()
    for name in sorted(srcs):
        h.update(srcs[name].encode("utf-8"))
    h.update((vm.LIB_SRC / "vent_model.h").read_bytes())
    h.update(vm.FFI_SRC.read_bytes())
    out = vm.BUILD_DIR / ("ventmodel_v1_%s.dll" % h.hexdigest()[:10])
    if not out.exists():
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / "vent_model.h").write_bytes((vm.LIB_SRC / "vent_model.h").read_bytes())
            for name, text in srcs.items():
                (d / name).write_text(text, encoding="utf-8")
            vm.build_dll(out, sorted(d.glob("*.cpp")) + [vm.FFI_SRC], include_dirs=[d], force=True)
    return vm.VentLib(out)


def check():
    v1, v2 = build_v1(), vm.load()
    print("v1 library (%s): %s  %s" % (V1_REV, v1.path.name, v1.models()))
    print("v2 library (shipped): %s  %s" % (v2.path.name, v2.models()))
    return v1, v2


def run():
    import humidity_prototype as hp
    import humidity_closedloop as hc
    v1, v2 = check()
    M2, TOM = hc.MODE2, hc.TOMATO
    runs = [
        ("m1 v1 p0", "stepped", v1, T_MIN_5C88 + ["cr_priority=0"]),
        ("m1 v2 p0", "stepped", v2, T_MIN_5C88 + ["cr_priority=0"]),
        ("m1 v2 p1", "stepped", v2, T_MIN_5C88 + ["cr_priority=1"]),
        ("m2 v1 p0", "graded", v1, M2 + T_MIN_5C88 + ["cr_priority=0"]),
        ("m2 v2 p0", "graded", v2, M2 + T_MIN_5C88 + ["cr_priority=0"]),
        ("m2 v2 p1", "graded", v2, M2 + T_MIN_5C88 + ["cr_priority=1"]),
        ("m2 v2 p2", "graded", v2, M2 + T_MIN_5C88 + ["cr_priority=2"]),
        ("tom v1 p0", "graded", v1, M2 + TOM + T_MIN_TOMATO + ["cr_priority=0"]),
        ("tom v2 p1", "graded", v2, M2 + TOM + T_MIN_TOMATO + ["cr_priority=1"]),
    ]
    c = hp._run_configs(runs, "gh#84 acceptance: the shipped law (v2) against v1")

    def near(key_a, key_b, metric):
        return abs(c[key_a][metric] - c[key_b][metric]) <= TOL
    rows = [
        ("A1 heat, mode 1: v2 p1 hours >= 31 C within %.1f of v1 p0" % TOL,
         near("m1 v2 p1", "m1 v1 p0", "hours >= 31 degC a day")),
        ("A1 heat, mode 2", near("m2 v2 p1", "m2 v1 p0", "hours >= 31 degC a day")),
        ("A1 heat, tomato row", near("tom v2 p1", "tom v1 p0", "hours >= 31 degC a day")),
        ("A2 nights, mode 1: v2 p1 night hours < 14 C within %.1f of v1 p0" % TOL,
         near("m1 v2 p1", "m1 v1 p0", "night hours below 14 degC a day")),
        ("A2 nights, mode 2", near("m2 v2 p1", "m2 v1 p0", "night hours below 14 degC a day")),
        ("A2 nights, tomato row (below its own t_min, 16 C)",
         near("tom v2 p1", "tom v1 p0", "night hours below 16 degC a day")),
        ("A3 no dry close anywhere in v2",
         all(m["dry-close hours a day"] == 0 for k, m in c.items() if " v2 " in k)),
        ("A4 priority 2 behaves exactly as priority 1", c["m2 v2 p1"] == c["m2 v2 p2"]),
        ("A5 priority 0: v2 decides as v1, mode 1", c["m1 v2 p0"] == c["m1 v1 p0"]),
        ("A5 priority 0: v2 decides as v1, mode 2", c["m2 v2 p0"] == c["m2 v1 p0"]),
    ]
    print("\n-- acceptance --")
    for what, ok in rows:
        print("  %-4s %s" % ("PASS" if ok else "FAIL", what))

    print("\n-- against the prototype's figures (mode 1 / mode 2 / tomato, priority 1) --")
    cols = ("m1 v2 p1", "m2 v2 p1", "tom v2 p1")
    for key, proto in PROTO.items():
        if key == "night hours below t_min":
            got = (c[cols[0]]["night hours below 14 degC a day"],
                   c[cols[1]]["night hours below 14 degC a day"],
                   c[cols[2]]["night hours below 16 degC a day"])
        else:
            got = tuple(c[k][key] for k in cols)
        print("  %-32s v2: %-18s prototype: %s" % (key, " / ".join("%.1f" % g for g in got),
                                                   " / ".join("%.1f" % p for p in proto)))
    return 0 if all(ok for _, ok in rows) else 1


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "check"
    if what == "check":
        check()
    elif what == "run":
        sys.exit(run())
    else:
        raise SystemExit("usage: humidity_acceptance.py check|run")
