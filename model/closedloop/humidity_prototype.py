"""humidity_prototype.py -- the gh#84 package, prototyped in a separate build.

The firmware is not touched. drivers/ventModel/src is COPIED into
build/prototype/<floor>/, the copy is patched, and the closed loop is handed a
law compiled from the copy through run_closed_loop's law= argument. Every
patch is a text replacement that must match exactly once, so a change
upstream stops this script rather than prototyping against a different law.

The package (humidityControl.md, "What would have to change"):
  1. dryness never closes against heat, under every priority (gh#84);
  2. humidity may open the house on its own only at or above t_min;
  3. a humidity-only opening is capped at step 1 (M1);
  4. a live humidity vote holds at step 1 until the average is a step width
     (hyst_rh / 3) below rh_max -- the close guard the branch never had;
  5. cr_priority: 0 unchanged, 1 = humidity may open on its own (2-4
     apply), 2 = the same as 1.
Under rule 1 the dry vote can no longer change any decision: against heat
it loses, and with the temperature idle the house is shut either way. So
rh_min becomes inert under every priority, as it already is under 0, and no
hysteresis is prototyped for it.

t_min is a compile-time constant here, one build per floor: vent_in_t has no
t_min field, and adding one (interface 3) is the firmware session's change.
mode 2's law is not patched at all: graded calls the stepped law, so it
inherits the package, which is the point of the embedding.

    python model/closedloop/humidity_prototype.py rules   rule checks, fail-first (seconds)
    python model/closedloop/humidity_prototype.py loop    acceptance runs, closed loop (~30 min)
    python model/closedloop/humidity_prototype.py floor   where the floor sits, and its hysteresis (~18 min)
    python model/closedloop/humidity_prototype.py diff    the package as a diff of the stepped law
"""
import sys
import json
import bisect
import shutil
from argparse import Namespace
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ventmodel as vm
from ventmodel import (VentIn, VentModel, VENT_WIN_CLOSED, VENT_CAP_DIGITAL,
                       VENT_CAP_LINEAR)

PROTO_DIR = vm.BUILD_DIR / "prototype"
NONE = -1

# The floor under a humidity-only opening, as recommended after `floor`: it
# opens from t_min + MARGIN and a live opening holds down to t_min + MARGIN -
# HYST. At t_min itself the house still cooled below t_min at night (the
# reading lags, and an unheated house keeps cooling after M1 shuts).
T_MIN = (16, 14)                # 5C88's t_min, day and night
TOMATO_T_MIN = (18, 16)         # the manual's tomato row
MARGIN, HYST = 2, 1

# The versions the prototype reports (design/ventModelContract.md, "A law's
# name and version"): the package changes stepped's decisions, and graded
# embeds stepped, so both bump although vent_model_graded.cpp is untouched.
VERSIONS = {"stepped": 2, "graded": 2}


def package(t_min):
    """The recommended package for a unit with this t_min (day, night)."""
    return build(t_min[0] + MARGIN, t_min[1] + MARGIN, HYST)


# --------------------------------------------------------------------------
# The patches
# --------------------------------------------------------------------------

def _patches(floor_day, floor_ngt, floor_hyst=0):
    return [
        # the package's constants, next to the state layout they sit beside
        ("#define ST_STEP_T   0   /* last temperature step this model commanded  */",
         "/* PROTOTYPE (model/closedloop/humidity_prototype.py): the gh#84 package. */\n"
         "#define PROTO_FLOOR_DAY_C  %d   /* by day: humidity may open on its own from here */\n"
         "#define PROTO_FLOOR_NGT_C  %d   /* and by night */\n"
         "#define PROTO_RH_CAP       1    /* a humidity-only opening: M1 at most */\n"
         "#define ST_STEP_T   0   /* last temperature step this model commanded  */"
         % (floor_day, floor_ngt)),
        # rule 4: the close guard at rh_max
        ("""    if (rh_avg > rh_max) {
        int deviation = (int)rh_avg - (int)rh_max;
        return step_from_deviation(deviation, (int)hyst_rh, current_step);
    }

    if (rh_avg < rh_min) {
        return 0;
    }""",
         """    if (rh_avg > rh_max) {
        int deviation = (int)rh_avg - (int)rh_max;
        return step_from_deviation(deviation, (int)hyst_rh, current_step);
    }

    /* PROTOTYPE rule 4: a live vote holds at step 1 until the average is the
     * ladder's own step width below the ceiling. */
    {
        const int band = ((int)hyst_rh / VENT_STEPS_MAX < 1) ? 1
                       : (int)hyst_rh / VENT_STEPS_MAX;
        if (current_step > 0 && (int)rh_avg > (int)rh_max - band) {
            return 1;
        }
    }

    if (rh_avg < rh_min) {
        return 0;
    }"""),
        # rules 1, 2, 3 and 5: the resolver
        ("static int vent_resolve_conflict(int step_t, int step_rh, uint8_t cr_priority)",
         "static int vent_resolve_conflict(int step_t, int step_rh, uint8_t cr_priority,\n"
         "                                 bool floor_ok)"),
        ("""    switch (cr_priority) {
        case 0:  /* temperature first */
        default:
            return step_t;

        case 1:  /* humidity first */
            return step_rh;

        case 2:  /* the larger demand wins */
            return (step_t > step_rh) ? step_t : step_rh;
    }""",
         """    /* PROTOTYPE rule 1 (gh#84): dryness never closes against heat. */
    if (step_t > 0 && step_rh == 0) {
        return step_t;
    }
    /* What is left: the temperature is idle and humidity wants to open. */
    if (cr_priority != 1 && cr_priority != 2) {
        return step_t;                              /* 0: temperature first */
    }
    /* Rules 2 and 3, and 5: 1 and 2 alike -- above the floor, M1 at most. */
    if (!floor_ok) {
        return step_t;
    }
    return (step_rh < PROTO_RH_CAP) ? step_rh : PROTO_RH_CAP;"""),
        ("    const int resolved = vent_resolve_conflict(step_t, step_rh, in->cr_priority);",
         "    const int t_floor = in->daytime ? PROTO_FLOOR_DAY_C : PROTO_FLOOR_NGT_C;\n"
         "    /* The floor's own hysteresis: a humidity-only opening already live\n"
         "     * holds down to the floor minus PROTO_FLOOR_HYST (0: a plain floor). */\n"
         "    const bool alone_live = st->v[ST_STEP] > 0 && st->v[ST_STEP_T] == 0;\n"
         "    const bool floor_ok = (int)in->t_avg_c >= t_floor ||\n"
         "        (alone_live && (int)in->t_avg_c >= t_floor - PROTO_FLOOR_HYST);\n"
         "    const int resolved = vent_resolve_conflict(step_t, step_rh, in->cr_priority,\n"
         "                                               floor_ok);"),
        ("#define PROTO_RH_CAP       1    /* a humidity-only opening: M1 at most */",
         "#define PROTO_RH_CAP       1    /* a humidity-only opening: M1 at most */\n"
         "#define PROTO_FLOOR_HYST   %d    /* degC the floor holds a live opening below it */"
         % floor_hyst),
    ]


def _file_patches(floor_day, floor_ngt, floor_hyst=0):
    """{source file: [(anchor, replacement), ...]} for every file the package touches."""
    return {
        "vent_model_stepped.cpp": _patches(floor_day, floor_ngt, floor_hyst) + [
            ('    "stepped",\n    1,\n',
             '    "stepped",\n    %d,     /* PROTOTYPE: the gh#84 package */\n'
             % VERSIONS["stepped"]),
        ],
        "vent_model_graded.cpp": [
            ('    "graded",\n    1,\n',
             '    "graded",\n    %d,     /* PROTOTYPE: embeds stepped, so it inherits the package */\n'
             % VERSIONS["graded"]),
        ],
    }


def patched_sources(floor_day, floor_ngt, floor_hyst=0):
    """{file name: text} for every file the package changes."""
    out = {}
    for name, patches in _file_patches(floor_day, floor_ngt, floor_hyst).items():
        text = (vm.LIB_SRC / name).read_text(encoding="utf-8")
        for old, new in patches:
            n = text.count(old)
            if n != 1:
                raise SystemExit("patch anchor found %d times in %s -- the law changed "
                                 "upstream; update the prototype first:\n%s"
                                 % (n, name, old.splitlines()[0]))
            text = text.replace(old, new)
        out[name] = text
    return out


def build(floor_day, floor_ngt, floor_hyst=0):
    """The patched copy, compiled. Returns a VentLib; drivers/ is only read.

    As law_compare's variants do: the sources are patched in a temporary
    directory and only the DLL is kept, under build/ (where *.dll is ignored).
    It is named by a hash of every source it is built from, so an unchanged
    prototype reuses its DLL -- Windows will not overwrite a loaded one."""
    import hashlib
    import tempfile
    texts = patched_sources(floor_day, floor_ngt, floor_hyst)
    h = hashlib.sha1()
    for name in sorted(texts):
        h.update(texts[name].encode("utf-8"))
    for f in sorted(vm.LIB_SRC.glob("*")) + [vm.FFI_SRC]:
        if f.name not in texts:
            h.update(f.read_bytes())
    out = PROTO_DIR / ("ventmodel_proto_%s.dll" % h.hexdigest()[:10])
    if not out.exists():
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            for f in vm.LIB_SRC.iterdir():
                if f.is_file():
                    shutil.copy2(f, d / f.name)
            for name, text in texts.items():
                (d / name).write_text(text, encoding="utf-8")
            vm.build_dll(out, sorted(d.glob("*.cpp")) + [vm.FFI_SRC],
                         include_dirs=[d], force=True)
    return vm.VentLib(out)


def diff():
    """The package as a diff against the shipped law, for review."""
    import difflib
    texts = patched_sources(T_MIN[0] + MARGIN, T_MIN[1] + MARGIN, HYST)
    for name in sorted(texts, reverse=True):         # stepped first: that is the change
        old = (vm.LIB_SRC / name).read_text(encoding="utf-8").splitlines(True)
        sys.stdout.writelines(difflib.unified_diff(
            old, texts[name].splitlines(True), "drivers/ventModel/src/" + name,
            "prototype (t_min 16/14: floor 18/16, holding to 17/15)", n=2))


# --------------------------------------------------------------------------
# The rule checks, fail-first: each new expectation must fail on the shipped law
# --------------------------------------------------------------------------

def vin(t_c, rh, prio, day=True, t_max=28, t_max_ngt=20, rh_max=75, rh_max_ngt=80,
        rh_min=50, rh_min_ngt=55, m3_linear=False):
    v = VentIn()
    v.t_valid = v.rh_valid = v.rh_ctrl_en = True
    v.daytime = day
    v.t_avg_c = int(t_c + 0.5) if t_c >= 0 else int(t_c - 0.5)
    v.t_avg_c10 = v.t_c10 = int(round(t_c * 10))
    v.rh_pct = v.rh_avg_pct = rh
    v.t_max_c10 = (t_max if day else t_max_ngt) * 10
    v.rh_max_pct = rh_max if day else rh_max_ngt
    v.rh_min_pct = rh_min if day else rh_min_ngt
    v.hyst_t_c, v.hyst_rh_pct, v.cr_priority = 5, 12, prio
    v.m3_deadzone_x10 = 13
    for i in range(3):
        v.win[i].state, v.win[i].cap = VENT_WIN_CLOSED, VENT_CAP_DIGITAL
        v.win[i].pos_x10, v.win[i].last_target_x10 = -1, -1
        v.win[i].ms_since_move = 0xFFFFFFFF
    if m3_linear:
        v.win[2].cap, v.win[2].pos_x10, v.win[2].pos_age_ms = VENT_CAP_LINEAR, 0, 0
    return v


def seq(lib, name, inputs):
    """Resolved steps (and graded's M3 target) over a sequence, on one law's memory."""
    law = VentModel(name, lib=lib)
    law.reset()
    res = []
    for v in inputs:
        o = law.step(v)
        res.append((o.step, o.win[2].action, o.win[2].target_x10))  # read before the next step
    return res


CHECKS = [
    # (what, law, inputs, expected resolved steps)
    ("gh#84, stepped: 30 C (step 2), RH 40 under rh_min, priority 1 vents",
     "stepped", [vin(30, 40, 1)], [2]),
    ("gh#84, stepped: the same under priority 2 (it did before too)",
     "stepped", [vin(30, 40, 2)], [2]),
    ("gh#84, graded: 31 C (step 3), RH 40, priority 1 vents, M3 aimed",
     "graded", [vin(31, 40, 1, m3_linear=True)], [3]),
    ("floor + cap: 20 C by day (T idle), RH 88 (step 3), priority 1 -> M1 only",
     "stepped", [vin(20, 88, 1)], [1]),
    ("floor: 12 C by night (under the floor), RH 90, priority 1 -> shut",
     "stepped", [vin(12, 90, 1, day=False)], [0]),
    ("priority 2 is priority 1: 20 C by day, RH 88 -> M1 only",
     "stepped", [vin(20, 88, 2)], [1]),
    ("priority 0 unchanged: 20 C by day, RH 88 -> shut",
     "stepped", [vin(20, 88, 0)], [0]),
    ("graded, humidity alone never opens M3: 20 C, RH 88, priority 1",
     "graded", [vin(20, 88, 1, m3_linear=True)], [1]),
    ("close guard: 20 C by day, priority 1, RH 80 -> 73 -> 71: held, then lapses",
     "stepped", [vin(20, 80, 1), vin(20, 73, 1), vin(20, 71, 1)], [1, 1, 0]),
    ("unchanged: humidity raises a live step, 29 C (step 1) and RH 84 (step 3)",
     "stepped", [vin(29, 84, 0)], [3]),
]


def rules():
    shipped = vm.load()
    proto = package(T_MIN)
    print("=== the package's rules: prototype against the shipped law ===")
    bad = 0
    for what, name, inputs, want in CHECKS:
        got_p = [r[0] for r in seq(proto, name, inputs)]
        got_s = [r[0] for r in seq(shipped, name, inputs)]
        ok_p = got_p == want
        new_rule = got_s != want                     # fail-first: new behaviour must be new
        print("  %-4s %-76s proto %-9s shipped %-9s %s"
              % ("ok" if ok_p else "FAIL", what, got_p, got_s,
                 "(fails on shipped: new)" if new_rule else "(unchanged)"))
        bad += 0 if ok_p else 1
    pv = dict((n, v) for n, (_, v) in proto.models().items())
    sv = dict((n, v) for n, (_, v) in shipped.models().items())
    ok_v = pv == VERSIONS
    print("  %-4s %-76s proto %-9s shipped %-9s %s"
          % ("ok" if ok_v else "FAIL", "versions: stepped and graded both bump (graded embeds stepped)",
             "v%d/v%d" % (pv["stepped"], pv["graded"]), "v%d/v%d" % (sv["stepped"], sv["graded"]),
             "(fails on shipped: new)" if sv != VERSIONS else "(unchanged)"))
    bad += 0 if ok_v else 1
    m3 = seq(proto, "graded", [vin(31, 40, 1, m3_linear=True)])[0]
    print("  graded M3 under gh#84 conditions: action %d target %d (TARGET 250: rate-limited towards open)"
          % (m3[1], m3[2]))
    print("rule checks: %d of %d pass on the prototype" % (len(CHECKS) + 1 - bad, len(CHECKS) + 1))
    return bad


# --------------------------------------------------------------------------
# The acceptance runs
# --------------------------------------------------------------------------

def _run_configs(runs, title):
    """Each (label, law name, lib, settings) through the closed loop; both tables."""
    import closed_loop as cl
    import dataset
    import law_compare as lc
    import logdata
    import humidity_closedloop as hc

    plant = lc.PLANTS["primary"]
    ds = dataset.build()
    start, end = cl._parse_local("2026-06-05"), cl._parse_local("2026-09-16", end=True)
    lo, hi = bisect.bisect_left(ds.t, start), bisect.bisect_left(ds.t, end)
    log = logdata.load_sd_logs([str(HERE.parent / "campaign-summer-2026" / "*.log")])
    print("=== %s: primary plant, %s .. %s, RH from the log ===" % (title, ds.t[lo], ds.t[hi - 1]))

    cols, north = [], None
    for label, name, lib, sets in runs:
        args = Namespace(model=name, warmup_h=6.0, rh_from_log=True, calibrator_hold=False,
                         openness="state", t3="sim", daynight="sim", firmware="current",
                         config=None, set=list(sets), plant2=str(plant),
                         m3_span_mm=1500, m3_airflow_exp=1.0)
        params = json.loads(Path(plant).read_text())["params"]
        recs, act, ctl = cl.run_closed_loop(ds, lo, hi, "two", params, args,
                                            cl.schedule_from_args(args),
                                            law=VentModel(name, lib=lib))
        starts = [c.n.starts - b.starts for c, b in zip(act.ch, act.n0)]
        starts += [ctl.dropped - ctl.n0[0], ctl.deferred - ctl.n0[1],
                   (act.ch[2].n.run_ms - act.n0[2].run_ms) / 60000.0]
        for r in recs:
            r.setdefault("o3_log", 1.0 if cl._code(r["bm_log"], 2) != 0 else 0.0)
        if north is None:
            north = lc.north_days(lc.full_days(recs))
        m = lc.metrics(recs, starts, "bm_sim", hc.T_HOT, north)
        m.update(hc.by_day(recs))
        m.update(hc.by_night(recs, log.is_day))
        m.update(extra(recs, log.is_day))
        cols.append((label, m))
        print("  ran %-13s %-8s %s" % (label, name, " ".join(sets)), flush=True)

    hc.table(cols, ["hours >= 31 degC a day", "hours >= 35 degC a day",
                    "mean daily max, degC", "daytime mean, degC",
                    "dry-close hours a day", "humidity-only venting, h a day",
                    "  of it by night", "M1+M2 drives a day", "M3 drives a day",
                    "M3 open hours a day"], "-- by day --")
    hc.table(cols, ["mean nightly minimum, degC", "coldest night, degC",
                    "night hours below 16 degC a day", "night hours below 14 degC a day",
                    "night hours below 12 degC a day", "M3 open at night, h a day"],
             "-- by night --")
    return dict(cols)


def floor():
    """Where the floor must sit, and whether it needs a hysteresis of its own.

    The first acceptance run failed A2 with the floor at exactly t_min and
    passed it at t_min + 2, at the price of more M1/M2 drives -- the whole-
    degree average flickering across a floor that, unlike every other
    threshold here, had no hysteresis. Mode 2 only: humidity alone opens M1
    and nothing else, so mode 1 behaves the same (the acceptance run shows it).
    """
    import humidity_closedloop as hc
    shipped = vm.load()
    M2 = hc.MODE2
    runs = [
        ("ship p0", "graded", shipped, M2 + ["cr_priority=0"]),
        ("t_min", "graded", build(16, 14, 0), M2 + ["cr_priority=1"]),
        ("t_min h1", "graded", build(16, 14, 1), M2 + ["cr_priority=1"]),
        ("t_min+1 h1", "graded", build(17, 15, 1), M2 + ["cr_priority=1"]),
        ("t_min+2", "graded", build(18, 16, 0), M2 + ["cr_priority=1"]),
        ("t_min+2 h1", "graded", build(18, 16, 1), M2 + ["cr_priority=1"]),
    ]
    c = _run_configs(runs, "the floor, mode 2, 5C88's settings (t_min 16/14), priority 1")
    base = c["ship p0"]
    print("")
    print("-- the floor against the baseline (shipped, priority 0) --")
    print("  %-11s %22s %20s %14s" % ("", "night h < 14 C, delta", "M1+M2 drives, delta",
                                      "humidity h/d"))
    for k, m in c.items():
        if k == "ship p0":
            continue
        dn = m["night hours below 14 degC a day"] - base["night hours below 14 degC a day"]
        dd = m["M1+M2 drives a day"] - base["M1+M2 drives a day"]
        print("  %-11s %+22.2f %+20.1f %14.1f   A2 %s"
              % (k, dn, dd, m["humidity-only venting, h a day"],
                 "PASS" if abs(dn) <= 0.1 else "FAIL"))


def loop():
    import humidity_closedloop as hc

    libs = {"5c88": package(T_MIN), "tomato": package(TOMATO_T_MIN)}
    shipped = vm.load()
    MODE2 = hc.MODE2
    TOM = hc.TOMATO
    RUNS = [
        # label, law name, lib, settings
        ("m1 ship p0", "stepped", shipped, ["cr_priority=0"]),
        ("m1 prot p0", "stepped", libs["5c88"], ["cr_priority=0"]),
        ("m1 prot p1", "stepped", libs["5c88"], ["cr_priority=1"]),
        ("m2 ship p0", "graded", shipped, MODE2 + ["cr_priority=0"]),
        ("m2 prot p0", "graded", libs["5c88"], MODE2 + ["cr_priority=0"]),
        ("m2 prot p1", "graded", libs["5c88"], MODE2 + ["cr_priority=1"]),
        ("m2 prot p2", "graded", libs["5c88"], MODE2 + ["cr_priority=2"]),
        ("tom ship p0", "graded", shipped, MODE2 + TOM + ["cr_priority=0"]),
        ("tom prot p1", "graded", libs["tomato"], MODE2 + TOM + ["cr_priority=1"]),
    ]
    c = _run_configs(RUNS, "the recommended package in the closed loop (floor t_min + %d, "
                           "holding to t_min + %d)" % (MARGIN, MARGIN - HYST))
    accept(c)


def extra(recs, is_day):
    """Humidity venting on its own, and the tomato row's night floor."""
    step_h = (recs[1]["t"] - recs[0]["t"]).total_seconds() / 3600.0
    span = (recs[-1]["t"] - recs[0]["t"]).total_seconds() / 86400.0
    alone = alone_n = below16 = 0.0
    for r in recs:
        st, s = r.get("step_t"), r.get("step")
        night = not is_day(r["t"])
        if st == 0 and s is not None and s > 0:
            alone += step_h
            if night:
                alone_n += step_h
        if night and r["T_sim"] < 16:
            below16 += step_h
    return {"humidity-only venting, h a day": alone / span,
            "  of it by night": alone_n / span,
            "night hours below 16 degC a day": below16 / span}


def accept(c):
    """The acceptance criteria, stated before the numbers were seen."""
    tol = 0.1
    rows = [
        ("A1 heat, mode 1: prototype p1 hours >= 31 C within %.1f of shipped p0" % tol,
         abs(c["m1 prot p1"]["hours >= 31 degC a day"] - c["m1 ship p0"]["hours >= 31 degC a day"]) <= tol),
        ("A1 heat, mode 2", abs(c["m2 prot p1"]["hours >= 31 degC a day"]
                                - c["m2 ship p0"]["hours >= 31 degC a day"]) <= tol),
        ("A1 heat, tomato row", abs(c["tom prot p1"]["hours >= 31 degC a day"]
                                    - c["tom ship p0"]["hours >= 31 degC a day"]) <= tol),
        ("A2 nights, mode 1: prototype p1 night hours < 14 C within %.1f of shipped p0" % tol,
         abs(c["m1 prot p1"]["night hours below 14 degC a day"]
             - c["m1 ship p0"]["night hours below 14 degC a day"]) <= tol),
        ("A2 nights, mode 2", abs(c["m2 prot p1"]["night hours below 14 degC a day"]
                                  - c["m2 ship p0"]["night hours below 14 degC a day"]) <= tol),
        ("A2 nights, tomato row (below its own t_min, 16 C)",
         abs(c["tom prot p1"]["night hours below 16 degC a day"]
             - c["tom ship p0"]["night hours below 16 degC a day"]) <= tol),
        ("A3 no dry close anywhere in the prototype",
         all(m["dry-close hours a day"] == 0 for k, m in c.items() if "prot" in k)),
        ("A4 priority 2 behaves exactly as priority 1",
         c["m2 prot p1"] == c["m2 prot p2"]),
    ]
    print("")
    print("-- acceptance --")
    for what, ok in rows:
        print("  %-4s %s" % ("PASS" if ok else "FAIL", what))


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "rules"
    if what == "rules":
        sys.exit(1 if rules() else 0)
    elif what == "loop":
        loop()
    elif what == "floor":
        floor()
    elif what == "diff":
        diff()
    else:
        raise SystemExit("usage: humidity_prototype.py rules|loop|floor|diff")
