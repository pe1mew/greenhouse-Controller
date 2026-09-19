"""
closed_loop.py -- the closed-loop greenhouse simulator: the calibrated plant,
the firmware's control chain, and the control law from drivers/ventModel.

    python model/closedloop/closed_loop.py gate-plant
    python model/closedloop/closed_loop.py gate-control [LOG ...] [--sweep KEY=A..B]
    python model/closedloop/closed_loop.py gate-sun
    python model/closedloop/closed_loop.py gate-wind
    python model/closedloop/closed_loop.py reproduce --start 2026-06-20 --end 2026-07-11
    python model/closedloop/closed_loop.py settings

Every controller setting can be applied: --config takes a unit's
GET /api/config, --set KEY=VALUE overrides one key (both on gate-control,
gate-sun, gate-wind, reproduce and settings), and without them the settings
are 5C88's own history, rebuilt from its SETPT audit rows. settings.py says
where each key acts -- the law, T5's averaging windows, T3, T2, T4 or the
poll interval -- and `settings` prints the table. T6 decides on T5's
averages, never on the raw reading: the SD log holds the raw 30-s reading
(data_manager.cpp), the plant is fitted and scored against that, and the
simulated controller averages the plant's reading as T5 does.

Why it exists
-------------
The plant model exists to verify control algorithms before they reach the
greenhouse: the binary (stepped) law 5C88 runs today, and the linear M3 law
(mode 2) once the new firmware and hardware can drive it. A simulator's
verdict is only evidence if the simulator first reproduces what the real
greenhouse did, so each layer has a gate, and the gates run in order:

  gate-plant    The plant is the calibrator's. Stepping the adopted artifact
                over the calibration input must reproduce
                calibrate_plant_constrained.simulate_c6() to 1e-9, and with it
                the published validation figures (T RMSE 1.19 degC on
                Jun 19-25).
  gate-control  The law and the firmware chain around it are the firmware's.
                Fed the logged sensor readings, the stepped law behind
                vent_model.h -- compiled from the firmware's own sources --
                must reproduce the logged decisions at least as well as
                vent_step_replay.py does: 96.8 % of 378 T-demands on the
                Jul 13-29 logs (design/ventModelContract.md s.5 item 3).
  gate-sun      T4 is the firmware's: sunrise.cpp itself, at the site's
                coordinates in force, must make every logged SUN row to the
                minute (92 of 92 over the summer).
  gate-wind     T3 is the firmware's: T5's wind averages and T3's state
                machine, at the settings in force and with 5C88's firmware
                versions, must make every logged wind override (8 of 8, onset
                and clear within 60 s).
  reproduce     The loop closed: the simulated controller drives the
                simulated plant through logged weather, and the outcome is
                compared with what 5C88 did -- window openings, M3 time open,
                temperatures, the limit cycle's period and swing. This is the
                test the model has to pass before its verdict on a new law
                means anything. It reports; the pass criterion is the
                operator's to set once the first numbers are in.

Nothing here talks to a unit: every input is a file already in the repo.
ASCII-only output (Windows console is cp1252 -- see memory/gotcha-log.md).
"""

from __future__ import annotations

import argparse
import copy
import json
import statistics
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
MODEL_DIR = HERE.parent
for p in (HERE, MODEL_DIR):
    if str(p) not in sys.path:
        sys.path.insert(0, str(p))

from firmware import (  # noqa: E402
    GH48_ON_5C88, PROFILE_CURRENT, SPAN_MM_PRODUCTION, SRC_T3, Actuator, Controller, LinearM3,
    RELAY_TO_CH, SafetyMonitor, SensorLayer, Settings, is_daytime, lroundf, profile_5c88,
    settings_5c88, sun_times_local, t17_poll_ms,
)
from logdata import BIT_WIND_OVERRIDE, CAMPAIGN, load_sd_logs  # noqa: E402
import settings as settings_mod  # noqa: E402
from plant import ADOPTED, Plant  # noqa: E402
import plant2  # noqa: E402
from plant2 import Plant2  # noqa: E402
from ventmodel import VENT_STEPS_MAX, VentModel, entry_temp_c  # noqa: E402

# The published figures the gates must reproduce.
PUBLISHED_VAL_T_RMSE = 1.19        # campaignResults_summer2026.md s.1, artifact _comment
REPLAY_BASELINE_PCT = 96.8         # campaignResults_summer2026.md F8, contract s.5 item 3
REPLAY_BASELINE_N = 378
DEFAULT_CONTROL_LOGS = str(CAMPAIGN / "2026-07-2*.log")
# T6 runs stepped in mode 1 and graded in mode 2 (plan §5c). Any law but
# stepped is run as mode 2's when M3 has its sensor.
MODE1_LAWS = ("stepped",)
PLANT_GATE_INPUT = CAMPAIGN / "calibration_input_2026-06-04_2026-07-04.csv"
EPOCH = datetime(2026, 1, 1)


def _ms(ts):
    """The simulator's monotonic millisecond clock -- deliberately UNBOUNDED.

    The firmware's clock wraps at 2^32 ms (49.7 days) and T2 compares
    deadlines wrap-safely, (int32_t)(now - deadline). firmware.Channel
    compares plain Python ints, so a wrapping clock here makes every deadline
    set just before a wrap look 49 days away: T6 keeps asking and the
    actuator keeps deferring. That happened on 2026-07-18 20:09 and silently
    froze M3 shut for the rest of every summer-long run until it was found
    (the 102-day tables before 2026-09-18 evening are void past that date).
    Only the value handed to the law is wrapped (Controller.cycle).
    """
    return int((ts - EPOCH).total_seconds() * 1000)


def _unix(ts):
    return int((ts - datetime(1970, 1, 1)).total_seconds())


# ==========================================================================
# settings
# ==========================================================================

LOG_POLL_S = 30        # the SD log's cadence: 5C88's poll_interval all summer

LEGEND = ("law = handed to the control law, resolved for day or night | T5 = an averaging "
          "window, the law gets the average | T3 = wind safety | T2 = motor travel and dwell"
          " | M3 = its wire sensor: a linear M3 | T4 = the site, so day or night | poll = when"
          " T5 samples and T6 decides | clock = reads the log's stamps | none = no effect on "
          "what is simulated")


def add_settings_args(p):
    p.add_argument("--config", metavar="JSON",
                   help="a unit's GET /api/config, instead of 5C88's logged history")
    p.add_argument("--set", action="append", metavar="KEY=VALUE",
                   help="override one controller setting at every moment (repeatable); "
                        "`closed_loop.py settings` lists them")


def schedule_from_args(args):
    """The settings in force: 5C88's history, or --config; --set wins over both."""
    sched = (settings_mod.schedule_from_config(args.config) if getattr(args, "config", None)
             else settings_mod.schedule_5c88())
    over = settings_mod.parse_set(getattr(args, "set", None))
    if over:
        over, notes = settings_mod.clamp(over)
        sched = sched.with_overrides(over)
        sched.notes = sched.notes + notes
    return sched


def describe_source(sched):
    s = sched.label
    if sched.overrides:
        s += "  |  --set " + ", ".join("%s=%s" % kv for kv in sorted(sched.overrides.items()))
    return s


def cmd_settings(args):
    sched = schedule_from_args(args)
    ts = _parse_local(args.at) if args.at else None
    print("=== settings: every controller key, and where the simulator applies it ===")
    print("  source : %s" % describe_source(sched))
    print("  at     : %s" % (ts or "the last logged change"))
    print("  (* = not the default; set = --set)")
    print()
    for line in settings_mod.describe(sched, ts):
        print(line)
    print("\n  acts: " + LEGEND)
    effective, cur = [], dict(sched.base)
    for t, k, v in sched.changes:
        if cur.get(k) != v and settings_mod.EFFECT[k][0] != "none":
            effective.append((t, k, cur.get(k), v))
        cur[k] = v
    if effective:
        print("\n  logged changes that act on the simulation (SETPT rows; the boot-time "
              "geolocation rewrites of an unchanged site are left out):")
        for t, k, old, v in effective:
            print("    %s  %-14s %s -> %s" % (t, k, old, v))
    for n in sched.notes:
        print("  note: %s" % n)
    ok = settings_mod.to_sim(settings_mod.defaults()) == Settings()
    print("\n  cfg_desc.inc keys classified: %d of %d, plus tz_str"
          % (len(settings_mod.KEYS), len(settings_mod.KEYS)))
    print("  firmware.Settings() defaults = cfg_defaults.h: %s"
          % ("yes" if ok else "NO -- update firmware.Settings"))
    return 0 if ok else 1


# ==========================================================================
# gate-plant
# ==========================================================================

def gate_plant(args):
    import numpy as np
    from calibrate_plant_constrained import simulate_c6
    from calibrate_plant_dynamic import (ah_from_rh, build_segments, ffill_bool,
                                         load_calibration_input, resample_to_grid,
                                         rmse_rh_stats, vent_mask_from_bitmask)

    with open(args.artifact) as fh:
        params = json.load(fh)
    print("=== gate-plant: is the step-wise plant the calibrator's? ===")
    print("  artifact : %s" % Path(args.artifact).name)
    print("  input    : %s" % Path(args.input).name)

    ts, T_in, RH_in, T_out, RH_out, lux, bm, valid = load_calibration_input(args.input)
    (grid, gT_in, gRH_in, gT_out, gRH_out, glux, gbm_f), data_mask = \
        resample_to_grid(ts, T_in, RH_in, T_out, RH_out, lux, bm.astype(float))
    vm = vent_mask_from_bitmask(gbm_f.astype(np.int32))
    seg_starts, seg_vent = build_segments(vm)

    s1 = (params["k_solar_w_per_lux"], params["c_eff_mj_per_c"],
          params["transpiration_kg_s"], params["ach_inf"], params["ach_m1"])
    T_ref, RH_ref = simulate_c6(s1, params["ach_m3"], gT_out, gRH_out, glux,
                                seg_starts, seg_vent)

    def run(hold):
        plant = Plant(params, hold_on_change=hold)
        plant.reset(gT_out[0], float(ah_from_rh(gRH_out[0], gT_out[0])))
        T = np.empty(len(grid))
        RH = np.empty(len(grid))
        for i in range(len(grid)):
            o = (float(vm[i] & 1), float((vm[i] >> 1) & 1), float((vm[i] >> 2) & 1))
            T[i], RH[i] = plant.step(o, gT_out[i], gRH_out[i], glux[i])
        return T, RH

    T_mine, RH_mine = run(hold=True)
    dT = float(np.max(np.abs(T_mine - T_ref)))
    dRH = float(np.max(np.abs(RH_mine - RH_ref)))
    print("\n  equivalence over %d steps (hold_on_change=True):" % len(grid))
    print("    max |T  - calibrator| = %.3g degC" % dT)
    print("    max |RH - calibrator| = %.3g %%" % dRH)
    eq_ok = dT < 1e-9 and dRH < 1e-9

    def parse(s):
        # calibrate_plant_constrained.py's own parse_utc(), so the validation
        # mask is the one its published figures were computed on
        return datetime.fromisoformat(s).replace(tzinfo=timezone.utc).timestamp()
    in_val = (grid >= parse(args.val_start)) & (grid < parse(args.val_end))
    val_mask = ffill_bool(ts, valid, grid) & data_mask & in_val
    r = rmse_rh_stats(T_mine, RH_mine, gT_in, gRH_in, val_mask)
    print("\n  validation week %s .. %s (%d valid rows):" % (args.val_start, args.val_end,
                                                          int(val_mask.sum())))
    print("    T RMSE %.2f degC (published %.2f)  95th %.2f  within +-1 %.1f %%  RH RMSE %.1f %%"
          % (r[0], PUBLISHED_VAL_T_RMSE, r[2], r[4], r[1]))
    pub_ok = abs(round(r[0], 2) - PUBLISHED_VAL_T_RMSE) < 1e-9

    T_cl, RH_cl = run(hold=False)
    r2 = rmse_rh_stats(T_cl, RH_cl, gT_in, gRH_in, val_mask)
    print("\n  without the calibrator's hold (the closed loop's setting):")
    print("    T RMSE %.2f degC  95th %.2f  within +-1 %.1f %%  RH RMSE %.1f %%"
          % (r2[0], r2[2], r2[4], r2[1]))
    print("    max |T difference| from the held version: %.3f degC"
          % float(np.max(np.abs(T_cl - T_mine))))

    ok = eq_ok and pub_ok
    print("\n  %s" % ("PASS -- the plant is the calibrator's, and reproduces its published fit"
                      if ok else "FAIL -- see the figures above"))
    return 0 if ok else 1


# ==========================================================================
# gate-control
# ==========================================================================

def replay_decisions(data, law, settings_at=settings_5c88):
    """Run T5 -> T4 -> T6 over the logged readings. Returns [(ts, Decision|None)].

    Resets as the firmware does: a BOOT empties T5's averages and T6's steps;
    an inhibit's onset resets T6's steps. Inhibits: the logged wind override,
    T/RH sensor faults, and LCD admin sessions (STANDBY).
    """
    s = settings_at(data.samples[0][0])
    sensor = SensorLayer(s)
    ctl = Controller(law, s)
    boots = list(data.boots)
    bi = 0
    out = []
    for ts, t_c10, rh in data.samples:
        booted = False
        while bi < len(boots) and boots[bi] <= ts:
            booted = True
            bi += 1
        s_now = settings_at(ts)
        if s_now != ctl.s:
            ctl.s = s_now
        if booted:
            sensor = SensorLayer(s_now)
            ctl.reset()
            ctl.prev_inhibited = False
        else:
            sensor.configure(s_now)             # T5 step 2: a changed window starts empty
        meas = sensor.push_register(t_c10, rh * 10)
        inhibited = data.override(ts) or data.tfault.contains(ts) or data.standby.contains(ts)
        d = ctl.cycle(_ms(ts), _unix(ts), data.is_day(ts), meas, inhibited, actuator=None)
        out.append((ts, d))
    return out


# A MODE row is stamped with dm_get_unix_time(), a cached clock that T4
# refreshes only when it re-reads the RTC, about once a minute
# (data_manager.cpp, read_rtc_and_seed_clock()). The row therefore belongs to
# one of the samples in the minute AFTER its timestamp: measured against the
# accurate RELAY clock over all 5C88 logs, 944 rows belong to the first
# sample at or after the stamp and 792 to the one after that.
MODE_CLOCK_WINDOW_S = 60


def score_decisions(decisions, modes):
    """Score the simulated decisions against the logged MODE rows.

    Two pairings, reported side by side:
      first  -- the row against the first cycle at or after its stamp: the
                rule vent_step_replay.py uses, kept so the T-demand figure is
                comparable with its 96.8 %;
      window -- the row against every cycle in the minute after its stamp,
                a hit if any of them made exactly that decision. This is the
                resolution the log's clock actually has.
    """
    import bisect
    ts_list = [t for t, _ in decisions]
    r = {"n": 0, "inhibited": 0, "first_t": 0, "first_row": 0, "first_rh": 0,
         "window_t": 0, "window_row": 0, "window_rh": 0, "misses": []}
    win = timedelta(seconds=MODE_CLOCK_WINDOW_S)
    for ts, step, step_t, step_rh in modes:
        i = bisect.bisect_left(ts_list, ts)
        if i >= len(ts_list):
            continue
        r["n"] += 1
        logged = (step, step_t, step_rh)
        d = decisions[i][1]
        if d is None:
            r["inhibited"] += 1
        else:
            r["first_t"] += d.step_t == step_t
            r["first_rh"] += d.step_rh == step_rh
            r["first_row"] += (d.step, d.step_t, d.step_rh) == logged
        cands = []
        j = i
        while j < len(ts_list) and ts_list[j] < ts + win:
            if decisions[j][1] is not None:
                cands.append(decisions[j][1])
            j += 1
        r["window_t"] += any(c.step_t == step_t for c in cands)
        r["window_rh"] += any(c.step_rh == step_rh for c in cands)
        hit = any((c.step, c.step_t, c.step_rh) == logged for c in cands)
        r["window_row"] += hit
        if not hit:
            r["misses"].append((ts, logged, [(c.step, c.step_t, c.step_rh) for c in cands]))
    return r


def gate_control(args):
    logs = args.logs or [DEFAULT_CONTROL_LOGS]
    data = load_sd_logs(logs)
    law = VentModel(args.model)
    print("=== gate-control: does the library law, in the firmware chain, make 5C88's decisions? ===")
    print("  law      : %s v%d  (drivers/ventModel, via %s)" % (law.name, law.version,
                                                                law.lib.path.name))
    print("  logs     : %d files, %d readings, %d MODE rows, %d boots"
          % (len(data.files), len(data.samples), len(data.modes), len(data.boots)))
    print("  span     : %s .. %s" % (data.samples[0][0], data.samples[-1][0]))
    sched = schedule_from_args(args)
    s = sched.at(data.samples[-1][0])
    if s.poll_s != LOG_POLL_S:
        print("\n  poll_interval %d: the log was sampled every %d s, and a replay cannot "
              "resample it. Use reproduce." % (s.poll_s, LOG_POLL_S))
        return 2
    print("  settings : t_max %d/%d hyst_t %d avg_win_t %d | rh_max %d/%d rh_min %d/%d "
          "hyst_rh %d avg_win_rh %d cr_priority %d"
          % (s.t_max_day, s.t_max_ngt, s.hyst_t, s.avg_win_t, s.rh_max_day, s.rh_max_ngt,
             s.rh_min_day, s.rh_min_ngt, s.hyst_rh, s.avg_win_rh, s.cr_priority))
    custom = bool(getattr(args, "config", None) or getattr(args, "set", None))
    if custom:
        print("  source   : %s" % describe_source(sched))

    if args.sweep:
        return _sweep(args.sweep, data, law, sched)

    decisions = replay_decisions(data, law, settings_at=sched.at)
    r = score_decisions(decisions, data.modes)
    n = r["n"]
    sim_rows = sum(1 for _, d in decisions if d is not None and d.logged)

    def pct(k):
        return 100.0 * r[k] / n if n else 0.0
    t_pct = pct("first_t")

    print("\n  logged MODE rows scored : %d  (%d fell in an inhibit)" % (n, r["inhibited"]))
    print("  %-22s %8s %8s" % ("", "first", "window"))
    print("  %-22s %7.1f%% %7.1f%%   <- vent_step_replay.py: %.1f %% of %d, first rule"
          % ("T-demand reproduced", t_pct, pct("window_t"), REPLAY_BASELINE_PCT,
             REPLAY_BASELINE_N))
    print("  %-22s %7.1f%% %7.1f%%   (the log keeps RH in whole %%, T5 averages 0.1 %%)"
          % ("RH-demand reproduced", pct("first_rh"), pct("window_rh")))
    print("  %-22s %7.1f%% %7.1f%%   (step, step_t and step_rh all equal)"
          % ("whole row reproduced", pct("first_row"), pct("window_row")))
    print("  MODE rows written       : %d simulated vs %d logged" % (sim_rows, len(data.modes)))
    print("  (first = the cycle at or after the row's stamp; window = any cycle in the")
    print("   %d s after it, the resolution of the clock MODE rows are stamped with)"
          % MODE_CLOCK_WINDOW_S)
    if args.show_misses:
        print("\n  rows no cycle in their window reproduced (logged -> candidates):")
        for ts, lg, cands in r["misses"][:args.show_misses]:
            print("    %s  %s -> %s" % (ts, lg, cands if cands else "inhibited"))

    is_default = not args.logs and not custom
    ok = t_pct >= REPLAY_BASELINE_PCT if is_default else t_pct >= args.min_fit
    print("\n  %s" % ("PASS" if ok else "FAIL"), end="")
    if is_default:
        print(" -- at least as good as vent_step_replay.py on the same logs"
              if ok else " -- worse than vent_step_replay.py on the same logs")
    else:
        print(" (min %.1f %%)" % args.min_fit)
    return 0 if ok else 1


def _sweep(spec, data, law, sched):
    """--sweep KEY=A..B: which value of one setting makes the logged decisions.

    How 5C88's averaging windows were found (2026-09-19): avg_win_t 3 makes
    98.5 % of the summer's T-demands and the default 6 only 65 %.
    """
    key, _, rng = spec.partition("=")
    a, _, b = rng.partition("..")
    if not b:
        raise SystemExit("--sweep %s: expected KEY=A..B, e.g. avg_win_rh=3..12" % spec)
    name = next(iter(settings_mod.parse_set(["%s=%s" % (key, a)])))
    in_force = sched.values_at(data.samples[-1][0])[name]
    print("\n  %-14s  T-demand  RH-demand  whole row   (first-cycle rule, %d MODE rows)"
          % (name, len(data.modes)))
    for v in range(int(a), int(b) + 1):
        sc = sched.with_overrides({name: v})
        r = score_decisions(replay_decisions(data, law, settings_at=sc.at), data.modes)
        n = r["n"] or 1
        print("  %-14d  %7.1f%%  %8.1f%%  %8.1f%%%s"
              % (v, 100.0 * r["first_t"] / n, 100.0 * r["first_rh"] / n,
                 100.0 * r["first_row"] / n, "   <- in force" if v == in_force else ""))
    return 0


# ==========================================================================
# gate-sun, gate-wind: T4 and T3 against 5C88's own rows
# ==========================================================================

def _tz_self_check(tz_str):
    """The tz_str reader against lora_time's EU rule, every hour of 2026.
    None when tz_str is not the one lora_time knows."""
    if tz_str != settings_mod.DEF_TZ_STR:
        return None
    from lora_time import utc_to_local
    tzr = settings_mod.tz(tz_str)
    u = datetime(2026, 1, 1)
    bad = 0
    while u.year == 2026:
        loc = utc_to_local(u)
        back = tzr.local_to_utc(loc)
        # the autumn hour repeats locally: the reader takes summer time, by design
        if back != u and not (back == u - timedelta(hours=1) and loc.month == 10):
            bad += 1
        u += timedelta(hours=1)
    return bad


def gate_sun(args):
    """T4's sun times at the settings in force must equal every logged SUN row."""
    from datetime import time as dtime
    sched = schedule_from_args(args)
    data = load_sd_logs(args.logs or [str(CAMPAIGN / "*.log")])
    print("=== gate-sun: sunrise.cpp at the site's coordinates makes 5C88's SUN rows ===")
    print("  source : %s" % describe_source(sched))
    miss = []
    for day, (sr, ss) in sorted(data.sun.items()):
        ts = datetime.combine(day, dtime(12))          # the UTC date is the local date
        got = sun_times_local(ts, sched.at(ts))
        if got != (sr, ss):
            miss.append((day, (sr, ss), got))
    s = sched.at(data.samples[-1][0])
    print("  site   : lat %d + %d/1000, lon %d + %d/1000  |  tz_str %s"
          % (s.lat_deg, s.lat_frac, s.lon_deg, s.lon_frac, s.tz_str))
    n = len(data.sun)
    print("  logged SUN rows: %d days, %s .. %s" % (n, min(data.sun), max(data.sun)))
    print("  sunrise and sunset reproduced to the minute: %d of %d" % (n - len(miss), n))
    for day, lg, got in miss[:10]:
        print("    %s  logged %d / %d  computed %d / %d" % (day, lg[0], lg[1], got[0], got[1]))
    bad = _tz_self_check(s.tz_str)
    if bad is not None:
        print("  tz_str read as lora_time's EU rule, every hour of 2026: %s"
              % ("yes" if bad == 0 else "NO, %d hours differ" % bad))
    ok = not miss and not bad
    print("\n  %s" % ("PASS -- day and night are the controller's" if ok else "FAIL"))
    return 0 if ok else 1


def replay_t3(data, sched, profile_at):
    """T5's wind averages and T3 over the logged wind: [(ts, active, event)].

    A boot empties T5's averages and clears T3, as the firmware does. A wind
    sensor fault (ALARM ch5) stops the averages and makes T3 safe-fail.
    """
    t0 = data.samples[0][0]
    sensor = SensorLayer(sched.at(t0), profile_at(t0))
    t3 = SafetyMonitor()
    boots, bi, out = list(data.boots), 0, []
    for ts, _t, _rh in data.samples:
        booted = False
        while bi < len(boots) and boots[bi] <= ts:
            booted = True
            bi += 1
        s, p = sched.at(ts), profile_at(ts)
        if booted:
            sensor = SensorLayer(s, p)
            t3.reset()
        else:
            sensor.configure(s, p)
        w = data.wind.at(ts, (0, 0))
        wf = data.wfault.contains(ts)
        meas = sensor.push_wind(w[0], w[1], ok=not wf)
        evt = t3.evaluate(meas, wf, s, p)
        out.append((ts, t3.active, evt, meas["wind_avg_ms10"], meas["wind_dir_avg_deg"], wf))
    return out


def _runs(ts_list, flags):
    out, start = [], None
    for t, f in zip(ts_list, flags):
        if f and start is None:
            start = t
        elif not f and start is not None:
            out.append((start, t))
            start = None
    if start is not None:
        out.append((start, ts_list[-1]))
    return out


def gate_wind(args):
    """T3 at the settings in force must make every logged wind override."""
    sched = schedule_from_args(args)
    prof = (lambda ts: PROFILE_CURRENT) if args.firmware == "current" else profile_5c88
    data = load_sd_logs(args.logs or [str(CAMPAIGN / "*.log")])
    print("=== gate-wind: T5's wind averages and T3 make 5C88's wind overrides ===")
    print("  source : %s  |  firmware: %s" % (describe_source(sched), args.firmware))
    sim = replay_t3(data, sched, prof)
    ts = [r[0] for r in sim]
    # T4 logs a sample before it wakes T3, so the bit logged with sample i is
    # T3's state after sample i-1: compare against the simulation shifted by one.
    logged = [bool(data.bitmask.at(t, 0) & BIT_WIND_OVERRIDE) for t in ts]
    shifted = [False] + [r[1] for r in sim[:-1]]
    agree = sum(a == b for a, b in zip(logged, shifted))
    lg_runs, sm_runs = _runs(ts, logged), _runs(ts, shifted)
    print("  samples: %d, %s .. %s  |  the override state agrees on %.4f %%"
          % (len(ts), ts[0], ts[-1], 100.0 * agree / len(ts)))
    print("  overrides: %d logged, %d simulated\n" % (len(lg_runs), len(sm_runs)))
    tol = timedelta(seconds=args.tolerance_s)
    used, fails = set(), 0
    print("  logged                                    simulated                                  "
          "onset / clear, s")
    for a, b in lg_runs:
        match = [k for k, (c, d) in enumerate(sm_runs)
                 if c <= b + tol and d >= a - tol and k not in used]
        if match:
            k = match[0]
            used.add(k)
            c, d = sm_runs[k]
            don, doff = (c - a).total_seconds(), (d - b).total_seconds()
            good = abs(don) <= args.tolerance_s and abs(doff) <= args.tolerance_s
            fails += not good
            print("  %s .. %s   %s .. %s   %+6.0f / %+6.0f%s"
                  % (a, b.time(), c, d.time(), don, doff, "" if good else "   <- outside tolerance"))
        else:
            fails += 1
            print("  %s .. %s   (none)  <- missed" % (a, b.time()))
    for k, (c, d) in enumerate(sm_runs):
        if k not in used:
            fails += 1
            print("  (none)                                    %s .. %s   <- not logged"
                  % (c, d.time()))
    ok = fails == 0
    print("\n  %s" % ("PASS -- every logged override, onset and clear within %d s"
                      % args.tolerance_s if ok else "FAIL -- %d override(s) differ" % fails))
    return 0 if ok else 1


# ==========================================================================
# reproduce
# ==========================================================================

STEP = timedelta(seconds=30)
WIND_VALID_FROM = datetime(2026, 6, 19, 12, 0, 0)   # vane commissioning (thermalProfileCampaign.md s.9.11)


def _parse_local(s, end=False):
    """'2026-07-01' (a whole day) or '2026-07-01T12:00'."""
    dt = datetime.fromisoformat(s)
    if end and len(s) == 10:
        dt += timedelta(days=1)
    return dt


def run_closed_loop(ds, lo, hi, plant_kind, params, args, sched=None, law=None):
    """Close the loop over the dataset's samples [lo, hi). Returns per-step records.

    law: a VentModel, by default args.model from drivers/ventModel. Anything
    else with the same step()/reset() is a test double, not a law.

    Warm-up (args.warmup_h before lo): the logged world drives everything
    and the controller only listens, so T5's averages, T6's hysteresis state
    and T2's dwell timers are what they were when the loop closes. At lo the
    plant takes the measured state -- for the two-node plant, the structure
    node from an open-loop run over the logged history -- and from then on
    the simulated controller moves the simulated windows.

    Every controller setting acts where the firmware applies it (settings.py):
    the law's inputs, T5's windows, T2's travel and dwell, T3's wind safety,
    T4's day and night, and the poll interval. By default T3 is simulated from
    the logged wind (--t3 sim) and day or night comes from sunrise.cpp at the
    site's coordinates (--daynight sim); both reproduce 5C88's own rows
    (gate-wind, gate-sun). --t3 log / --daynight log take the logged override
    bit and SUN rows instead, as this simulator did before 2026-09-19.

    poll_interval: at 30 s, the log's cadence, the controller decides on every
    logged sample. At any other value it decides every poll_interval seconds,
    between the samples: the two-node plant is stepped to each decision, with
    the weather, the wind and the doors held from the sample that covers it.

    Taken from the log rather than simulated, because neither the plant nor
    the controller can change them: the outdoor weather, the doors, the wind,
    T/RH and wind sensor faults, boots, and LCD admin sessions -- during which
    the operator moved the windows by hand, so the windows follow the logged
    RELAY rows.

    A linear M3: with wpos_fitted_m3 = 1 at the start, M3 has its wire sensor
    (firmware.LinearChannel), and the law gets its capability, position and
    age. With stepped that is mode 1 and nothing that acts changes; with any
    other law it is mode 2, and M3 takes targets (see the README). It needs
    the two-node plant, which takes M3's airflow as proportional to its
    opening: unmeasured for a part-open M3 (plan §5c).
    """
    data = ds.log
    sched = sched or schedule_from_args(args)
    prof_at = ((lambda ts: PROFILE_CURRENT) if getattr(args, "firmware", "5c88") == "current"
               else profile_5c88)
    t3_sim = getattr(args, "t3", "sim") == "sim"
    day_sim = getattr(args, "daynight", "sim") == "sim"
    start, end = ds.t[lo], ds.t[hi - 1]
    law = law or VentModel(args.model)
    s = sched.at(start)
    m3 = None
    if s.wpos_fitted_m3:
        if plant_kind != "two":
            raise SystemExit("a linear M3 (wpos_fitted_m3 = 1) needs the two-node plant "
                             "(--plant2): the single-node plant sees a window open or shut")
        m3 = LinearM3(span_mm=getattr(args, "m3_span_mm", SPAN_MM_PRODUCTION),
                      min_move_s=getattr(args, "m3_min_move_s", 0),
                      mode2=law.name not in MODE1_LAWS)
    act = Actuator(s, prof_at(start), m3_linear=m3)
    sensor = SensorLayer(s, prof_at(start))
    ctl = Controller(law, s)
    t3 = SafetyMonitor()
    sub = s.poll_s != LOG_POLL_S
    if plant_kind == "two":
        plant = Plant2(params)
        Ts_open = plant2.run(params, ds)[1].copy()   # the structure's logged history
    else:
        if sub:
            raise SystemExit("poll_interval %d s: deciding between the logged samples needs "
                             "the two-node plant (--plant2)" % s.poll_s)
        plant = Plant(params, hold_on_change=args.calibrator_hold)

    import bisect
    warm = max(0, bisect.bisect_left(ds.t, start - timedelta(hours=args.warmup_h)))
    relay = [r for r in data.relay if ds.t[warm] - timedelta(hours=1) <= r[0] <= end]
    boots = [b for b in data.boots if ds.t[warm] <= b <= end]
    ri = bi = 0

    def apply_relay_until(t):
        nonlocal ri
        while ri < len(relay) and relay[ri][0] <= t:
            ts, ch, v = relay[ri]
            if v in RELAY_TO_CH:
                act.ch[ch].sync(RELAY_TO_CH[v], _ms(ts))
            ri += 1

    def boot_between(t0, t1):
        nonlocal bi
        hit = False
        while bi < len(boots) and boots[bi] <= t1:
            hit = hit or boots[bi] > t0
            bi += 1
        return hit

    def daytime(t, s):
        return is_daytime(t, s) if day_sim else data.is_day(t)

    def sample_wind(t):
        """T5's wind: the logged S200 reading, or its fault."""
        w = data.wind.at(t, (0, 0))
        wf = data.wfault.contains(t)
        return sensor.push_wind(w[0], w[1], ok=not wf), wf

    # ---- T5's wind and T3 since the last boot ----------------------------
    # The wind stays the logged wind all through the run, so its float32
    # running sum has to carry the firmware's history since boot: an average
    # that lands on x.5 rounds either way. On 2026-07-19 12:35:54 six samples
    # average 59.5 (0.1 m/s), lroundf() made 60 = v_max, and T3 fired; a sum
    # started six hours earlier gives 59 and misses it. T and RH need no such
    # history -- once the loop closes they are the plant's, not the log's.
    boot0 = max((b for b in data.boots if b <= ds.t[warm]), default=None)
    if boot0 is not None:
        for ts, _t, _rh in data.samples:
            if ts < boot0:
                continue
            if ts >= ds.t[warm]:
                break
            s, p = sched.at(ts), prof_at(ts)
            sensor.configure(s, p)
            wmeas, wf = sample_wind(ts)
            if t3_sim:
                t3.evaluate(wmeas, wf, s, p)

    # ---- warm-up: listen only --------------------------------------------
    prev = ds.t[warm] - STEP
    for i in range(warm, lo):
        ts = ds.t[i]
        apply_relay_until(ts)
        s, p = sched.at(ts), prof_at(ts)
        if boot_between(prev, ts):
            sensor = SensorLayer(s, p)
            ctl.reset()
            t3.reset()
        else:
            sensor.configure(s, p)
        ctl.s = s
        meas = sensor.push_register(int(round(ds.T_in[i] * 10)), int(ds.RH_in[i]) * 10)
        wmeas, wf = sample_wind(ts)
        meas.update(wmeas)
        if t3_sim:
            t3.evaluate(meas, wf, s, p)
            override = t3.active
        else:
            override = data.override(ts)
        inhibited = override or data.tfault.contains(ts) or data.standby.contains(ts)
        ctl.cycle(_ms(ts), _unix(ts), daytime(ts, s), meas, inhibited, actuator=None)
        prev = ts
    apply_relay_until(start)

    def init_plant(i):
        if plant_kind == "two":
            plant.reset(ds.T_in[i], ds.AH_in[i], Ts0=Ts_open[i])
        else:
            plant.reset(ds.T_in[i], ds.AH_in[i])

    prev_override = data.override(start)

    def decide(t, T, RH, i, standby):
        """One poll at time t: T5 samples, T3 then T6 decide (T3 runs first)."""
        nonlocal prev_override
        now = _ms(t)
        s, p = ctl.s, act.ch[0].profile
        rh_c10 = int(ds.RH_in[i]) * 10 if args.rh_from_log else lroundf(RH * 10)
        meas = sensor.push_register(lroundf(T * 10), rh_c10)
        wmeas, wf = sample_wind(t)
        meas.update(wmeas)
        if t3_sim:
            if t3.evaluate(meas, wf, s, p) == "set":
                act.close_all(now, SRC_T3)
            override = t3.active
        else:
            override = data.override(t)
            if override and not prev_override:
                act.close_all(now, SRC_T3)
            prev_override = override
        inhibited = override or data.tfault.contains(t) or standby
        return ctl.cycle(now, _unix(t), daytime(t, s), meas, inhibited, actuator=act), override

    def prepare(t):
        """The settings and firmware behaviour in force at t."""
        s, p = sched.at(t), prof_at(t)
        act.set_profile(p)
        ctl.s = s
        sensor.configure(s, p)
        return s

    # ---- closed loop -------------------------------------------------------
    init_plant(lo)
    act.openness("mean", _ms(start))            # start the openness integral here
    act.n0 = [copy.copy(c.n) for c in act.ch]   # the counters as the loop closes
    ctl.n0 = (ctl.dropped, ctl.deferred, ctl.model_errors)
    held_out_day0 = ds.t[0].date()
    poll = timedelta(seconds=s.poll_s)
    next_c, plant_t = start, start               # sub-stepping: next decision, plant clock
    T, RH = ds.T_in[lo], ds.RH_in[lo]
    d, override = None, prev_override
    recs = []
    prev = start - STEP
    for i in range(lo, hi):
        t = ds.t[i]
        now = _ms(t)
        booted = boot_between(prev, t)
        standby = data.standby.contains(t)
        if not standby:
            while ri < len(relay) and relay[ri][0] <= t:
                ri += 1                         # the simulation has the windows
        # Sub-stepping decides at earlier moments first, so the actuator is
        # only advanced to t here when nothing happens between the samples.
        gap = (ds.restart[i] and i > lo) or booted
        if not sub or gap:
            s = prepare(t)
            act.advance(now)
            if booted:
                sensor = SensorLayer(s, act.ch[0].profile)
                ctl.reset()
                t3.reset()
                act.close_all(now, SRC_T3)      # T2's boot CLOSE_ALL sweep
            if standby:
                apply_relay_until(t)            # the operator has the windows

        doors = ds.door1[i] + ds.door2[i]
        wdir = ds.wind_dir[i] if ds.wind_valid[i] else float("nan")   # as plant2.Prepared
        if ds.restart[i] and i > lo:            # an SD gap: no weather to simulate through
            init_plant(i)
            act.openness("mean", now)
            T, RH = ds.T_in[i], ds.RH_in[i]
            next_c, plant_t = t, t
        elif not sub:
            if plant_kind == "two":
                o = act.openness("mean", now)
                T, RH = plant.step(o, doors, wdir, ds.T_out[i], ds.AH_out[i], ds.lux[i],
                                   ds.dt[i])
            else:
                T, RH = plant.step(act.openness(args.openness, now), ds.T_out[i],
                                   ds.RH_out[i], ds.lux[i])
        else:
            if booted:
                next_c = t                      # T5 starts polling again
            # decisions between the samples: step the plant to each, decide there
            while next_c < t:
                c_now = _ms(next_c)
                if standby:
                    apply_relay_until(next_c)
                act.advance(c_now)
                if next_c > plant_t:
                    o = act.openness("mean", c_now)
                    T, RH = plant.step(o, doors, wdir, ds.T_out[i], ds.AH_out[i], ds.lux[i],
                                       (next_c - plant_t).total_seconds())
                    plant_t = next_c
                prepare(next_c)
                d, override = decide(next_c, T, RH, i, standby)
                next_c += poll
            if standby:
                apply_relay_until(t)
            act.advance(now)
            prepare(t)
            if t > plant_t:
                o = act.openness("mean", now)
                T, RH = plant.step(o, doors, wdir, ds.T_out[i], ds.AH_out[i], ds.lux[i],
                                   (t - plant_t).total_seconds())
                plant_t = t

        if not sub:
            d, override = decide(t, T, RH, i, standby)
        elif next_c == t:
            d, override = decide(t, T, RH, i, standby)
            next_c += poll

        rec = {
            "t": t, "T_sim": T, "RH_sim": RH, "T_log": float(ds.T_in[i]),
            "RH_log": float(ds.RH_in[i]), "wind_ms": float(ds.wind_ms[i]),
            "wind_dir": float(ds.wind_dir[i]), "T_out": float(ds.T_out[i]),
            "lux": float(ds.lux[i]), "door": bool(ds.door1[i] or ds.door2[i]),
            "bm_sim": act.bitmask(), "bm_log": int(ds.bm[i]) & 0x3F,
            "pos_m3": act.ch[2].pos, "standby": standby, "override": override,
            "held_out": (t.date() - held_out_day0).days % 4 == 3,
            "step": d.step if d else None, "step_t": d.step_t if d else None,
            "step_rh": d.step_rh if d else None,
        }
        if m3:
            rec.update({"o3_log": float(ds.o[i, 2]), "m3_x10": act.ch[2].reading,
                        "m3_target": act.ch[2].last_target})
        recs.append(rec)
        prev = t
    return recs, act, ctl


def _code(bm, ch):
    return (bm >> (2 * ch)) & 3


def day_metrics(recs, key, t_m3):
    """Window activity and temperature character of one run's records."""
    out = {}
    n = len(recs)
    for ch, name in ((0, "M1"), (1, "M2"), (2, "M3")):
        opens = 0
        prev = _code(recs[0][key], ch)
        open_ts = []
        for r in recs[1:]:
            c = _code(r[key], ch)
            if prev == 0 and c in (1, 2):
                opens += 1
                open_ts.append(r["t"])
            prev = c
        out[name + "_opens"] = opens
        out[name + "_open_h"] = sum(1 for r in recs if _code(r[key], ch) != 0) * STEP.total_seconds() / 3600
        if ch == 2:
            out["m3_open_ts"] = open_ts
    Tk = "T_sim" if key == "bm_sim" else "T_log"
    temps = [r[Tk] for r in recs]
    out["T_max"] = max(temps)
    out["T_mean"] = sum(temps) / n
    out["h_above"] = sum(1 for x in temps if x >= t_m3) * STEP.total_seconds() / 3600
    # limit-cycle character: the gaps between successive M3 openings, and the
    # temperature swing inside each gap
    gaps, swings = [], []
    ots = out["m3_open_ts"]
    for a, b in zip(ots, ots[1:]):
        gap_min = (b - a).total_seconds() / 60
        if gap_min <= 180:
            gaps.append(gap_min)
            seg = [r[Tk] for r in recs if a <= r["t"] < b]
            if seg:
                swings.append(max(seg) - min(seg))
    out["cycle_min"] = statistics.median(gaps) if gaps else None
    out["swing"] = statistics.median(swings) if swings else None
    return out


def summarise(days):
    """Totals and the limit cycle's character: all days, fitted days, held-out days."""
    import numpy as np

    def block(label, sel):
        if not sel:
            return
        lg = [d[2] for d in sel]
        sm = [d[3] for d in sel]
        o_l = np.array([x["M3_opens"] for x in lg], float)
        o_s = np.array([x["M3_opens"] for x in sm], float)
        r = np.corrcoef(o_l, o_s)[0, 1] if o_l.std() > 0 and o_s.std() > 0 else float("nan")

        def med(xs, key):
            v = [x[key] for x in xs if x[key] is not None]
            return (statistics.median(v), len(v)) if v else (None, 0)
        cl, ncl = med(lg, "cycle_min")
        cs, ncs = med(sm, "cycle_min")
        wl, _ = med(lg, "swing")
        ws, _ = med(sm, "swing")

        def f(x, fmt):
            return fmt % x if x is not None else "-"
        print("  %-14s %3d days | M3 openings %4d / %4d (day by day r = %.2f) | M3 open h %5.0f / %5.0f"
              " | h>=M3 entry %5.0f / %5.0f | cycle %s / %s min on %d / %d days | swing %s / %s degC"
              % (label, len(sel), o_l.sum(), o_s.sum(), r,
                 sum(x["M3_open_h"] for x in lg), sum(x["M3_open_h"] for x in sm),
                 sum(x["h_above"] for x in lg), sum(x["h_above"] for x in sm),
                 f(cl, "%.0f"), f(cs, "%.0f"), ncl, ncs, f(wl, "%.1f"), f(ws, "%.1f")))

    print("\n  summary, logged / simulated (cycle = median gap between M3 openings; swing = median"
          " T range inside a gap):")
    block("all days", days)
    block("fitted days", [d for d in days if not d[1]])
    block("held-out days", [d for d in days if d[1]])


def swing_split(days):
    """The swing by wind and doors (NS-10): where a plant's limit cycle is too shallow.

    North wind: at least half of the day's logged M3-open time had wind from
    315-45 deg, M3's side. Doors: open for at least a fifth of the day.
    """
    def med(sel, i):
        v = [d[i]["swing"] for d in sel if d[i]["swing"] is not None]
        return (statistics.median(v), len(v)) if v else (None, 0)

    print()
    print("  swing by condition, logged / simulated (median of the per-day swings):")
    for label, sel in (
            ("north wind", [d for d in days if d[6] is not None and d[6] >= 50]),
            ("other wind", [d for d in days if d[6] is not None and d[6] < 50]),
            ("doors shut", [d for d in days if d[5] < 20]),
            ("a door open", [d for d in days if d[5] >= 20])):
        (wl, nl), (ws, ns) = med(sel, 2), med(sel, 3)
        if nl and ns:
            print("  %-12s %4.1f / %4.1f degC   (on %d / %d days)" % (label, wl, ws, nl, ns))


def linear_summary(recs, act, ctl):
    """A linear M3's run: what T6 did with the law's targets, and the motor's
    work against the log's (contract §7: report starts per day)."""
    n, n0 = act.ch[2].n, act.n0[2]
    dropped, deferred, errors = (ctl.dropped - ctl.n0[0], ctl.deferred - ctl.n0[1],
                                 ctl.model_errors - ctl.n0[2])
    days = max((recs[-1]["t"] - recs[0]["t"]).total_seconds() / 86400.0, 1e-9)
    codes = [_code(r["bm_log"], 2) for r in recs]
    log_drives = sum(1 for a, b in zip(codes, codes[1:]) if b in (1, 3) and b != a)
    pos = [r["pos_m3"] for r in recs]
    # at rest part-open: packed as OPEN (code 2) with the leaf short of the end
    part = sum(1 for r in recs if _code(r["bm_sim"], 2) == 2 and r["pos_m3"] < 0.995) / len(recs)
    print("  sim M3, linear: %d targets moved it or its stop point (%d of them a stop point),"
          " %d dropped in the deadband, %d deferred by the minimum interval, %d timeouts,"
          " %d taken over by T3 or the operator, %d model errors"
          % (n.targets - n0.targets, n.retargets - n0.retargets, dropped, deferred,
             n.timeouts - n0.timeouts, n.aborts - n0.aborts, errors))
    print("  M3 drives per day: logged %.1f, simulated %.1f  |  mean opening: logged %.1f %%,"
          " simulated %.1f %%  |  simulated at rest part-open %.0f %% of the time"
          % (log_drives / days, (n.starts - n0.starts) / days,
             100.0 * sum(r["o3_log"] for r in recs) / len(recs),
             100.0 * sum(pos) / len(pos), 100.0 * part))


def reproduce(args):
    import dataset as dsmod
    start = _parse_local(args.start)
    end = _parse_local(args.end, end=True)
    ds = dsmod.build()
    if start < ds.t[0] or end > ds.t[-1] + timedelta(minutes=5):
        print("The dataset covers %s .. %s; fetch the outdoor and door data past that "
              "first (fetch_lora_data.py)." % (ds.t[0], ds.t[-1]))
        return 2
    import bisect
    lo, hi = bisect.bisect_left(ds.t, start), bisect.bisect_left(ds.t, end)

    if args.plant2:
        with open(args.plant2) as fh:
            params = json.load(fh)["params"]
        kind, plant_name = "two", Path(args.plant2).name
    else:
        with open(args.artifact) as fh:
            params = json.load(fh)
        kind, plant_name = "single", Path(args.artifact).name

    if args.m3_span_mm <= 0 or args.m3_min_move_s < 0:
        raise SystemExit("--m3-span-mm must be above 0 and --m3-min-move-s not below 0")
    sched = schedule_from_args(args)
    recs, act, ctl = run_closed_loop(ds, lo, hi, kind, params, args, sched)
    s = sched.at(start)
    # where the stepped law opens M3: the yardstick for any law, as the log's is
    t_m3 = entry_temp_c(VENT_STEPS_MAX, s.t_max_day, s.hyst_t)
    lin = act.m3_linear

    if lin is None:
        print("=== reproduce: the binary law, closed over the plant ===")
    else:
        print("=== reproduce: the law with a linear M3 (mode %d), closed over the plant ==="
              % (2 if lin.mode2 else 1))
    print("  %s .. %s  |  law %s v%d  |  plant %s (%s-node)%s%s"
          % (ds.t[lo], ds.t[hi - 1], ctl.law.name, ctl.law.version, plant_name,
             "two" if kind == "two" else "single",
             "" if kind == "two" else "  |  openness=%s" % args.openness,
             "  |  RH from the log" if args.rh_from_log else ""))
    print("  settings: %s" % describe_source(sched))
    print("  T3 %s  |  day/night %s  |  poll %d s  |  firmware: %s"
          % ("simulated" if args.t3 == "sim" else "the logged override",
             "sunrise.cpp" if args.daynight == "sim" else "the logged SUN rows", s.poll_s,
             "5C88's versions, as it ran them" if args.firmware == "5c88" else "today's"))
    if args.firmware == "5c88":
        print("  firmware: the gh#48 guard from %s (2.3.1); T6 may reverse mid-stroke before"
              % GH48_ON_5C88)
    if lin is not None:
        print("  M3: wire sensor fitted (wpos_fitted_m3 = 1), span %d mm, deadband %d mm = %.1f %%,"
              " a reading every %d ms while it moves and every 30 s at rest"
              % (lin.span_mm, s.deadzone_m3_mm, lin.deadzone_x10(s.deadzone_m3_mm) / 10.0,
                 t17_poll_ms(s.travel_s[2])))
        print("  M3: " + ("mode 2 -- T2 takes targets; the open dwell gives way to a minimum "
                          "interval of %d s between moves" % lin.min_move_s if lin.mode2 else
                          "mode 1 -- driven on its timer with its dwell, as without the sensor"))
        if lin.min_move_s * 1000 > 0xFFFF:
            print("  note: vent_in_t.m3_min_move_ms is a uint16, so the law is told 65.5 s;"
                  " T6 enforces the whole %d s" % lin.min_move_s)
        print("  the plant takes M3's airflow as proportional to its opening: unmeasured for a "
              "part-open M3 (plan s.5c)")
    print("  'h>=%d' = hours at or above the M3 entry temperature (t_avg_c %d with "
          "t_max %d, hyst_t %d)" % (t_m3, t_m3, s.t_max_day, s.hyst_t))
    print("  'N%%' = share of the logged M3-open time with wind from 315-45 deg (M3's windward"
          " side; wind valid from 2026-06-19 12:00)")
    print("  '*' = a day the two-node fit held out (every 4th day)")
    print()
    hdr = ("date         door  standby  N%%  | M3 opens  | M3 open h   | M1 opens | T max       "
           "| h>=%d      | T RMSE bias  | M3 cycle min | swing degC" % t_m3)
    print(hdr)
    print("             %     min          | log  sim  | log   sim   | log sim  | log   sim   "
          "| log   sim  |              | log   sim    | log  sim")
    by_day = {}
    for r in recs:
        by_day.setdefault(r["t"].date(), []).append(r)
    days = []
    for day, rs in sorted(by_day.items()):
        if len(rs) < 2000:          # partial day
            continue
        lg = day_metrics(rs, "bm_log", t_m3)
        sm = day_metrics(rs, "bm_sim", t_m3)
        err = [r["T_sim"] - r["T_log"] for r in rs]
        rmse = (sum(e * e for e in err) / len(err)) ** 0.5
        bias = sum(err) / len(err)
        door = 100.0 * sum(r["door"] for r in rs) / len(rs)
        sb = sum(r["standby"] for r in rs) * 0.5
        m3_open = [r for r in rs if _code(r["bm_log"], 2) != 0]
        n_pct = (100.0 * sum(1 for r in m3_open if r["wind_dir"] >= 315 or r["wind_dir"] < 45)
                 / len(m3_open)) if m3_open and day >= WIND_VALID_FROM.date() else None
        windward = "%3.0f" % n_pct if n_pct is not None else "  -"
        held = rs[0]["held_out"]
        days.append((day, held, lg, sm, rmse, door, n_pct))

        def f(x, fmt="%5.0f"):
            return fmt % x if x is not None else "    -"
        print("%s%s  %4.0f  %5.0f   %s  | %3d  %3d  | %5.1f %5.1f | %3d %3d  | %5.1f %5.1f | %4.1f  %4.1f | %4.2f %+5.2f | %s %s   | %s %s"
              % (day, "*" if held else " ", door, sb, windward, lg["M3_opens"], sm["M3_opens"],
                 lg["M3_open_h"], sm["M3_open_h"],
                 lg["M1_opens"], sm["M1_opens"], lg["T_max"], sm["T_max"],
                 lg["h_above"], sm["h_above"], rmse, bias,
                 f(lg["cycle_min"]), f(sm["cycle_min"]),
                 f(lg["swing"], "%4.1f"), f(sm["swing"], "%4.1f")))
    summarise(days)
    swing_split(days)
    if args.t3 == "sim":
        # the logged bit trails T3 by one sample (gate-wind); compare it so
        ts = [r["t"] for r in recs]
        logged = [bool(ds.log.bitmask.at(t, 0) & BIT_WIND_OVERRIDE) for t in ts]
        shifted = [False] + [bool(r["override"]) for r in recs[:-1]]
        print("  T3 simulated: %d wind overrides against %d logged; the override state agrees "
              "on %.4f %% of samples" % (len(_runs(ts, shifted)), len(_runs(ts, logged)),
                                         100.0 * sum(a == b for a, b in zip(logged, shifted))
                                         / len(ts)))
    for i, name in enumerate(("M1", "M2", "M3")):
        n = act.ch[i].n
        print("  sim %s: %d drives, %d reversals, %d dwell deferrals, %d in-travel deferrals"
              % (name, n.starts, n.reversals, n.dwell_defers, n.travel_defers))
    if lin is not None:
        linear_summary(recs, act, ctl)

    if args.csv:
        import csv
        with open(args.csv, "w", newline="") as fh:
            wr = csv.DictWriter(fh, fieldnames=list(recs[0].keys()))
            wr.writeheader()
            wr.writerows(recs)
        print("\n  wrote %s" % args.csv)
    if args.plot:
        plot_run(recs, args.plot, t_m3)
        print("  wrote %s" % args.plot)
    return 0


def plot_run(recs, path, t_m3):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.dates as mdates
    import matplotlib.pyplot as plt

    t = [r["t"] for r in recs]
    fig, ax = plt.subplots(3, 1, figsize=(15, 9), sharex=True,
                           gridspec_kw={"height_ratios": [3, 1.6, 1]})
    ax[0].plot(t, [r["T_log"] for r in recs], color="#1f77b4", lw=1.0, label="T_in logged")
    ax[0].plot(t, [r["T_sim"] for r in recs], color="#d62728", lw=1.0, label="T_in simulated")
    ax[0].plot(t, [r["T_out"] for r in recs], color="0.6", lw=0.8, label="T_out")
    ax[0].axhline(t_m3, color="#ff7f0e", lw=0.8, ls="--", label="M3 entry (%d degC)" % t_m3)
    ax[0].set_ylabel("degC")
    ax[0].legend(loc="upper left", fontsize=8, ncol=4)
    ax[0].grid(alpha=0.25)
    linear = "m3_x10" in recs[0]
    for ch, name in enumerate(("M1", "M2", "M3")):
        off = 2 - ch
        ax[1].step(t, [off + 0.4 * (_code(r["bm_log"], ch) != 0) for r in recs], where="post",
                   color="#1f77b4", lw=1.0, label="logged" if ch == 0 else None)
        if ch == 2 and linear:      # a linear M3: its opening, not open or shut
            ax[1].plot(t, [off + 0.45 * r["pos_m3"] + 0.02 for r in recs], color="#d62728",
                       lw=1.0)
            continue
        ax[1].step(t, [off + 0.45 * (_code(r["bm_sim"], ch) != 0) + 0.02 for r in recs],
                   where="post", color="#d62728", lw=1.0, label="simulated" if ch == 0 else None)
    ax[1].set_yticks([0.2, 1.2, 2.2])
    ax[1].set_yticklabels(["M3", "M2", "M1"])
    ax[1].legend(loc="upper left", fontsize=8, ncol=2)
    ax[1].grid(alpha=0.25, axis="x")
    ax[2].fill_between(t, 0, [r["lux"] / 1000 for r in recs], color="#e6a817", alpha=0.35,
                       step="post")
    door = [r["door"] for r in recs]
    ax[2].fill_between(t, 0, 1, where=door, transform=ax[2].get_xaxis_transform(),
                       color="#9467bd", alpha=0.15, label="a door open (not in the model)")
    ax[2].set_ylabel("lux (k)")
    ax[2].legend(loc="upper left", fontsize=8)
    loc = mdates.AutoDateLocator()
    ax[2].xaxis.set_major_locator(loc)
    ax[2].xaxis.set_major_formatter(mdates.ConciseDateFormatter(loc))
    fig.suptitle("Closed loop: %s over the calibrated plant, vs what 5C88 logged"
                 % ("the law with a linear M3" if linear else "the binary law"), fontsize=10)
    fig.tight_layout()
    fig.savefig(path, dpi=120)
    plt.close(fig)


# ==========================================================================

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("gate-plant", help="the plant is the calibrator's")
    p.add_argument("--artifact", default=str(ADOPTED))
    p.add_argument("--input", default=str(PLANT_GATE_INPUT))
    p.add_argument("--val-start", default="2026-06-18T22:00",
                   help="as calibrate_plant_constrained.py --val-start takes it")
    p.add_argument("--val-end", default="2026-06-25T22:00",
                   help="as calibrate_plant_constrained.py --val-end takes it")
    p.set_defaults(fn=gate_plant)

    p = sub.add_parser("gate-control", help="the law + firmware chain make the logged decisions")
    p.add_argument("logs", nargs="*", help="SD logs (default: the Jul 13-29 baseline set)")
    p.add_argument("--model", default="stepped")
    p.add_argument("--min-fit", type=float, default=90.0,
                   help="pass mark for logs other than the baseline set, or other settings")
    p.add_argument("--show-misses", type=int, default=0, metavar="N")
    p.add_argument("--sweep", metavar="KEY=A..B",
                   help="score every value of one setting against the logged decisions, "
                        "e.g. avg_win_rh=3..12: which value did the unit run?")
    add_settings_args(p)
    p.set_defaults(fn=gate_control)

    p = sub.add_parser("gate-sun", help="T4: sunrise.cpp makes the logged SUN rows")
    p.add_argument("logs", nargs="*", help="SD logs (default: the whole campaign)")
    add_settings_args(p)
    p.set_defaults(fn=gate_sun)

    p = sub.add_parser("gate-wind", help="T5's wind averages + T3 make the logged overrides")
    p.add_argument("logs", nargs="*", help="SD logs (default: the whole campaign)")
    p.add_argument("--firmware", choices=("5c88", "current"), default="5c88",
                   help="the firmware behaviour: 5C88's versions as it ran them (default), "
                        "or today's")
    p.add_argument("--tolerance-s", type=int, default=60,
                   help="onset and clear must agree within this (default 60 s: two samples)")
    add_settings_args(p)
    p.set_defaults(fn=gate_wind)

    p = sub.add_parser("settings", help="every controller setting, and where it acts")
    p.add_argument("--at", help="local datetime to show (default: after the last change)")
    add_settings_args(p)
    p.set_defaults(fn=cmd_settings)

    p = sub.add_parser("reproduce", help="close the loop over logged weather")
    p.add_argument("--start", required=True, help="local date or datetime, e.g. 2026-06-20")
    p.add_argument("--end", required=True, help="local date (inclusive) or datetime")
    p.add_argument("--warmup-h", type=float, default=6.0)
    p.add_argument("--model", default="stepped")
    p.add_argument("--artifact", default=str(ADOPTED),
                   help="the single-node plant (default: the adopted artifact)")
    p.add_argument("--plant2", metavar="JSON",
                   help="use this two-node plant (refit.py) instead of the single node")
    p.add_argument("--openness", choices=("state", "position"), default="state",
                   help="single node only: state = the calibrator's convention (default); "
                        "position = the leaf's travelled fraction")
    p.add_argument("--rh-from-log", action="store_true",
                   help="feed the controller the LOGGED humidity, to separate the "
                        "temperature loop from the plant's weak humidity model")
    p.add_argument("--calibrator-hold", action="store_true",
                   help="reproduce the calibrator's hold on every window change")
    p.add_argument("--csv", help="write every step's record to this CSV")
    p.add_argument("--plot", help="write a PNG of the run")
    p.add_argument("--t3", choices=("sim", "log"), default="sim",
                   help="wind safety: simulated from the logged wind at the settings in "
                        "force (default), or the logged override bit")
    p.add_argument("--daynight", choices=("sim", "log"), default="sim",
                   help="day or night: sunrise.cpp at the site's coordinates (default), or "
                        "the logged SUN rows")
    p.add_argument("--firmware", choices=("5c88", "current"), default="5c88",
                   help="the firmware behaviour: 5C88's versions as it ran them (default), "
                        "or today's (gh#48 guard, gh#79 pause, wind_hyst, own wind window)")
    p.add_argument("--m3-span-mm", type=int, default=SPAN_MM_PRODUCTION,
                   help="a linear M3 (--set wpos_fitted_m3=1): its taught span, which turns "
                        "deadzone_m3 into the law's percent (default %(default)s mm, "
                        "production's ~1.5 m window)")
    p.add_argument("--m3-min-move-s", type=int, default=0,
                   help="a linear M3 in mode 2: the least time between two M3 moves, which "
                        "T6 enforces (the linear dwell: no key yet, specified default 0)")
    add_settings_args(p)
    p.set_defaults(fn=reproduce)

    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
