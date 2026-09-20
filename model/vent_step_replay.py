#!/usr/bin/env python3
"""vent_step_replay.py — replay T6's ventilation-step logic against SD logs.

Answers two questions that config tuning discussions keep raising:

  1. HOW BAD is the M3 jitter really? (open/close counts, dwell distribution,
     mid-stroke reversals, motor duty)
  2. WHAT WOULD CHANGING `hyst_t` / `avg_win_t` DO? (replay the same days
     under candidate settings and compare)

Reads raw SD CSV directly (no logparser pass needed):

    timestamp,type,initiator,ch,param,value_a,value_b
    MODE       value_a = resolved step, value_b = packed (hi=step_t, lo=step_rh)
    SENSOR_HR  ch=0 -> value_a = t_c10 (temperature x10)
    SUN        value_a = sunrise_min, value_b = sunset_min (local)

USAGE
    python model/vent_step_replay.py <log.csv> [<log.csv> ...] \
        [--t-max-day 28] [--t-max-ngt 20] [--hyst-t 5] [--avg-win-t 3]

THE LAW IS THE LIBRARY'S, NOT A COPY
------------------------------------
Every decision is made by the stepped law in drivers/ventModel
(src/vent_model_stepped.cpp), the library T6 moves onto in 2.12.0, loaded
through closedloop/ventmodel.py as the closed-loop simulator loads it. This
script restates none of the law's arithmetic, so a change to the law reaches
the replay without an edit here. (Until 2026-09-19 it carried a Python port
of step_from_deviation() and vent_resolve_conflict(); on the summer's logs
the library reproduced the port's output exactly.) The first run compiles
the library, with the compiler the closed loop uses (see ventmodel.py).

The script supplies what T6's caller supplies: the averaged temperature, the
day or night threshold, the settings -- and humidity's vote. That vote is
not replayed but read from the log (the MODE row's step_rh, carried
forward), because hyst_t and avg_win_t cannot change it and the log holds it
exactly. It enters the law through the law's own humidity inputs, set so
that its humidity branch casts the logged vote (the inputs for each vote are
found by asking the law, and every call checks the vote came back), so the
law's own resolver weighs it against temperature's.

WHY A VALIDATION GATE
---------------------
The replay is only evidence if it reproduces what the unit actually did. The
script therefore first replays at the unit's CONFIGURED settings and reports
the percentage of logged T-demands it reproduces. Below --min-fit (default
90%) the projections are withheld rather than printed, because an unvalidated
model produces authoritative-looking numbers that are not evidence.

TWO CORRECTNESS TRAPS (both cost real debugging time; do not re-introduce)
-------------------------------------------------------------------------
1. T6 evaluates on EVERY sensor cycle (~poll_interval) and carries
   current_step statefully, but a MODE row is logged ONLY when the resolved
   step changes. Stepping the state machine only at MODE rows diverges badly
   (~50% fit). Evaluate at every sample.
2. `meas.t_avg_c` is the ROUNDED integer degC, not truncated. With hyst_t=5
   the step width is 5/3 = 1 degC, so a 1-degree rounding error is an entire
   ventilation step (fit collapses from ~97% to ~59%). The law takes the
   average ready-rounded (vent_in_t.t_avg_c), so the rounding is this
   script's job, as it is T5's in the firmware. It is approximate here:
   float64 and Python's half-to-even round(), where T5 uses float32 and
   lroundf(). closedloop/closed_loop.py gate-control emulates T5 exactly.

STRUCTURAL NOTE
---------------
`step_width = hyst_t / NUM_VENT_STEPS` is INTEGER division, so hyst_t is not
a smooth dial -- 3/4/5 all give width 1, 6/7/8 give width 2, 9/10/11 give
width 3. Only the regime changes matter.

Also note `hyst_t` only gates the step -> 0 transition (the law's
"close-hysteresis guard": step_from_deviation() in vent_model_stepped.cpp,
a copy of climate_control.cpp's); intermediate transitions such as 2<->3
(where M3 lives) have no dead band. That asymmetry is what gh#47
(`vent_hyst`) targets.

ASCII-only output (Windows console is cp1252 -- see memory/gotcha-log.md).
"""

import argparse
import csv
import sys
from collections import deque
from datetime import datetime, timedelta
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "closedloop"))
from ventmodel import (VENT_STEP_NONE, VENT_STEPS_MAX, VentIn,  # noqa: E402
                       VentModel, entry_temp_c)

LAW = "stepped"            # mode 1's law in drivers/ventModel
M3_TRAVEL_S = 171          # M3 full stroke; dwell below this = mid-stroke reversal
SHORT_CYCLE_S = 900        # 15 min


# --------------------------------------------------------------------------
# The law: drivers/ventModel, loaded through closedloop/ventmodel.py
# --------------------------------------------------------------------------

# The humidity band that carries the logged votes into the law. Any band
# works: rh_vote_inputs() asks the law which reading casts which vote.
RH_MIN_PCT, RH_MAX_PCT, RH_HYST_PCT = 40, 60, 3


def law_input(hyst_t, cr_priority):
    """A vent_in_t with the settings in; simulate() fills the rest per sample.

    The stepped law reads the whole-degree average, the threshold, hyst_t,
    cr_priority and the humidity fields. Nothing else it is given matters to
    it, so the clock, the raw readings, the wind and the windows stay zero.
    """
    v = VentIn()
    v.t_valid = True
    v.rh_valid = True
    v.hyst_t_c = hyst_t
    v.cr_priority = cr_priority
    v.rh_min_pct = RH_MIN_PCT
    v.rh_max_pct = RH_MAX_PCT
    v.hyst_rh_pct = RH_HYST_PCT
    return v


def rh_vote_inputs():
    """{vote: (rh_ctrl_en, rh_avg_pct)} that make the law's humidity branch
    cast each vote: none (-1), close (0), or a step 1..3.

    Found by asking a fresh law at every humidity reading, rather than by
    restating its humidity rules here. The temperature settings do not matter.
    """
    law = VentModel(LAW)
    v = law_input(hyst_t=5, cr_priority=0)
    votes = {}
    for en, rh in [(False, 0)] + [(True, r) for r in range(101)]:
        law.reset()
        v.rh_ctrl_en, v.rh_avg_pct = en, rh
        votes.setdefault(int(law.step(v).step_rh), (en, rh))
    missing = [s for s in range(VENT_STEP_NONE, VENT_STEPS_MAX + 1) if s not in votes]
    if missing:
        raise RuntimeError("the %s law cannot be made to cast humidity vote(s) %s, so the "
                           "logged votes cannot be carried into it" % (LAW, missing))
    return votes


def m3_entry_temp(hyst_t, t_max_day):
    """Lowest t_avg_c at which the T branch demands step 3 (M3 in the mask)."""
    return entry_temp_c(VENT_STEPS_MAX, t_max_day, hyst_t, LAW)


# --------------------------------------------------------------------------
# Log loading
# --------------------------------------------------------------------------

def load(paths):
    """Return (temps, modes, sun) from raw SD CSV files."""
    temps, modes, sun = [], [], []
    for path in paths:
        try:
            with open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
                for row in csv.DictReader(fh):
                    try:
                        ts = datetime.strptime(row["timestamp"], "%Y-%m-%dT%H:%M:%S")
                        ch = int(row["ch"])
                        par = int(row.get("param", 0) or 0)
                        va = int(row["value_a"])
                        vb = int(row["value_b"])
                    except (ValueError, TypeError, KeyError):
                        continue
                    typ = row.get("type")
                    if typ == "SENSOR_HR" and ch == 0:
                        temps.append((ts, va / 10.0))
                    elif typ == "MODE":
                        # 2.6.0 (gh#54): param == 47 is a STANDBY transition
                        # from dm_set_standby_ex(), not a vent-step decision.
                        # This matters more here than anywhere else: the
                        # reproduction gate below refuses to project unless it
                        # reproduces >=90 % of the logged T-demands, and a
                        # STANDBY row injects a fabricated demand-free step --
                        # so it could fail the gate for a healthy config, or
                        # shift a projection. Rows from firmware before 2.6.0
                        # carry param 0 and cannot be separated.
                        if par == 47:
                            continue
                        u = vb & 0xFFFF
                        st = (u >> 8) & 0xFF
                        sr = u & 0xFF
                        if st & 0x80:
                            st -= 256
                        if sr & 0x80:
                            sr -= 256
                        modes.append((ts, va, st, sr))
                    elif typ == "SUN":
                        sun.append((ts, va, vb))
        except OSError as exc:
            print("  SKIP %s (%s)" % (path, exc))

    def dedup(seq):
        out, seen = [], set()
        for r in sorted(seq):
            if r[0] in seen:
                continue
            seen.add(r[0])
            out.append(r)
        return out

    return dedup(temps), dedup(modes), dedup(sun)


def sun_lookup(sun):
    """day -> (sunrise_min, sunset_min), falling back to the last known pair."""
    by_day = {}
    for ts, sr, ss in sun:
        by_day[ts.date()] = (sr, ss)
    return by_day


def is_day(dt, by_day, default=(355, 1299)):
    sr, ss = by_day.get(dt.date(), default)
    mins = dt.hour * 60 + dt.minute
    return sr <= mins < ss


# --------------------------------------------------------------------------
# Replay
# --------------------------------------------------------------------------

def simulate(temps, modes, by_day, hyst_t, t_max_day, t_max_ngt, avg_win_t,
             cr_priority=0):
    """Evaluate the law at every sample, carrying its state as T6 does, with
    humidity's vote taken from the log (carried forward between MODE rows,
    which are only written on change).

    Returns (resolved_timeline, fit_pairs) where fit_pairs is
    [(logged_t_demand, simulated_t_demand), ...] at each MODE row.
    """
    votes = rh_vote_inputs()
    law = VentModel(LAW)                  # fresh: T6's boot state
    v = law_input(hyst_t, cr_priority)
    win = timedelta(minutes=avg_win_t)
    dq = deque()
    total = 0.0
    resolved = []
    fit_pairs = []
    mi = 0
    rh_now = VENT_STEP_NONE

    for dt, tc in temps:
        dq.append((dt, tc))
        total += tc
        while dq and dt - dq[0][0] >= win:
            total -= dq.popleft()[1]
        # TRAP 2: firmware ROUNDS to integer degC
        v.t_avg_c = int(round(total / len(dq)))
        day = is_day(dt, by_day)
        v.daytime = day
        v.t_max_c10 = (t_max_day if day else t_max_ngt) * 10

        first = mi
        while mi < len(modes) and modes[mi][0] <= dt:
            rh_now = modes[mi][3]
            mi += 1
        if rh_now not in votes:
            raise RuntimeError("%s: the logged humidity vote %d is not one the law casts"
                               % (modes[mi - 1][0], rh_now))
        v.rh_ctrl_en, v.rh_avg_pct = votes[rh_now]

        out = law.step(v)
        if out.step_rh != rh_now:
            raise RuntimeError("%s: the law cast humidity vote %d, the log %d"
                               % (dt, out.step_rh, rh_now))
        for k in range(first, mi):
            fit_pairs.append((modes[k][2], out.step_t))
        resolved.append((dt, out.step))

    return resolved, fit_pairs


def m3_stats(resolved):
    """Crossings of the step-3 boundary (M3 enters/leaves the channel mask)."""
    opens = closes = 0
    dwells = []
    open_dt = None
    total_open = 0.0
    for i in range(1, len(resolved)):
        (_pdt, p), (ndt, n) = resolved[i - 1], resolved[i]
        if p < 3 and n == 3:
            opens += 1
            open_dt = ndt
        elif p == 3 and n < 3:
            closes += 1
            if open_dt:
                s = (ndt - open_dt).total_seconds()
                dwells.append(s)
                total_open += s
                open_dt = None
    return opens, closes, sorted(dwells), total_open


def observed_m3(modes):
    """Ground truth straight from the logged resolved steps."""
    return m3_stats([(dt, res) for dt, res, _t, _r in modes])


# --------------------------------------------------------------------------
# Report
# --------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+", help="raw SD CSV log file(s)")
    ap.add_argument("--t-max-day", type=int, default=28)
    ap.add_argument("--t-max-ngt", type=int, default=20)
    ap.add_argument("--hyst-t", type=int, default=5, help="the unit's CONFIGURED value")
    ap.add_argument("--avg-win-t", type=int, default=3, help="the unit's CONFIGURED value")
    ap.add_argument("--cr-priority", type=int, default=0)
    ap.add_argument("--min-fit", type=float, default=90.0)
    args = ap.parse_args(argv)
    # The law takes these as bytes: a value out of range would wrap silently.
    if not 1 <= args.hyst_t <= 255:
        ap.error("--hyst-t must be 1..255")
    if not 0 <= args.cr_priority <= 2:
        ap.error("--cr-priority must be 0, 1 or 2")

    temps, modes, sun = load(args.logs)
    by_day = sun_lookup(sun)
    if not temps or not modes:
        print("No usable SENSOR_HR / MODE rows found. Are these raw SD CSV logs?")
        return 1

    law = VentModel(LAW)
    span_days = max(1.0, (temps[-1][0] - temps[0][0]).total_seconds() / 86400.0)
    print("=== input ===")
    print("  %d temperature samples, %d MODE decisions" % (len(temps), len(modes)))
    print("  %s .. %s  (%.1f days)" % (temps[0][0], temps[-1][0], span_days))
    print("  configured: hyst_t=%d avg_win_t=%d t_max day/ngt=%d/%d"
          % (args.hyst_t, args.avg_win_t, args.t_max_day, args.t_max_ngt))
    print("  law: %s v%d (drivers/ventModel, via %s)"
          % (law.name, law.version, law.lib.path.name))

    # ---- observed severity (no model involved) --------------------------
    o, c, dw, tot = observed_m3(modes)
    print()
    print("=== OBSERVED M3 behaviour (from the logged resolved step -- not modelled) ===")
    print("  openings              : %d  (%.1f per day)" % (o, o / span_days))
    print("  total M3 open time    : %.1f h" % (tot / 3600.0))
    if dw:
        print("  dwell median          : %.0f s (%.1f min)" % (dw[len(dw) // 2], dw[len(dw) // 2] / 60))
        print("  below M3 travel %3ds  : %d   <-- mid-stroke reversals" % (M3_TRAVEL_S, sum(1 for s in dw if s < M3_TRAVEL_S)))
        print("  %ds - 5 min          : %d" % (M3_TRAVEL_S, sum(1 for s in dw if M3_TRAVEL_S <= s < 300)))
        print("  5 - 15 min            : %d" % sum(1 for s in dw if 300 <= s < SHORT_CYCLE_S))
        print("  over 15 min           : %d" % sum(1 for s in dw if s >= SHORT_CYCLE_S))
    duty_h = o * M3_TRAVEL_S * 2 / 3600.0
    print("  motor run time        : %.1f h total = %.0f min/day" % (duty_h, duty_h * 60 / span_days))

    # ---- validation -----------------------------------------------------
    _, fit = simulate(temps, modes, by_day, args.hyst_t, args.t_max_day,
                      args.t_max_ngt, args.avg_win_t, args.cr_priority)
    pct = 100.0 * sum(1 for lg, sm in fit if lg == sm) / len(fit) if fit else 0.0
    print()
    print("=== VALIDATION (replay at the configured settings vs logged T-demand) ===")
    print("  reproduces %.1f%% of %d logged T-demands" % (pct, len(fit)))
    if pct < args.min_fit:
        print("  MODEL NOT VALIDATED (< %.0f%%) -- projections withheld." % args.min_fit)
        print("  Check that --hyst-t / --avg-win-t match the unit's real config;")
        print("  a wrong avg_win_t is the usual cause (see the module docstring).")
        return 0
    print("  MODEL VALIDATED (>= %.0f%%)" % args.min_fit)

    # ---- levers ---------------------------------------------------------
    print()
    print("=== LEVER: hyst_t (moves the step thresholds; note integer step_width) ===")
    print("  %-8s %-11s %8s %9s %11s" % ("hyst_t", "M3 opens at", "openings", "<15min", "M3 open h"))
    for h in sorted({3, 5, 6, 7, 9, args.hyst_t}):
        res, _ = simulate(temps, modes, by_day, h, args.t_max_day,
                          args.t_max_ngt, args.avg_win_t, args.cr_priority)
        oo, _cc, dd, tt = m3_stats(res)
        tag = "  <-- configured" if h == args.hyst_t else ""
        print("  %-8d %-8d degC %8d %9d %11.1f%s"
              % (h, m3_entry_temp(h, args.t_max_day), oo,
                 sum(1 for s in dd if s < SHORT_CYCLE_S), tt / 3600.0, tag))

    print()
    print("=== LEVER: avg_win_t (smooths the input; thresholds UNCHANGED) ===")
    print("  %-10s %8s %9s %11s" % ("avg_win_t", "openings", "<15min", "M3 open h"))
    for aw in sorted({3, 6, 10, 15, args.avg_win_t}):
        res, _ = simulate(temps, modes, by_day, args.hyst_t, args.t_max_day,
                          args.t_max_ngt, aw, args.cr_priority)
        oo, _cc, dd, tt = m3_stats(res)
        tag = "  <-- configured" if aw == args.avg_win_t else ""
        print("  %-10d %8d %9d %11.1f%s"
              % (aw, oo, sum(1 for s in dd if s < SHORT_CYCLE_S), tt / 3600.0, tag))

    print()
    print("  Read the trade-off, not just the jitter column: both levers buy fewer")
    print("  openings by giving up M3 open time. gh#47 (vent_hyst) exists because")
    print("  neither can damp the 2<->3 transition without moving its threshold.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
