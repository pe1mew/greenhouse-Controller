#!/usr/bin/env python3
"""Check every vent-step decision in SD logs against stepped v2's resolver.

WHY
---
A soak's counters say nothing about the climate law: they judge M3's drives.
This reads the decisions T6 logged (MODE param 0: step, step_t, step_rh) and
judges each against the resolver of `stepped` v2 (gh#84, 2.15.0;
drivers/ventModel/src/vent_model_stepped.cpp), under the cr_priority in force
at that moment. `graded` v2 takes its step from `stepped` v2, so its rows are
judged the same way. Written for the 2.15.0 soak (2026-09-27), where it found
42 decisions and none against the rules.

**It restates v2's rules, so it is tied to that version.** The log's own law
row (MODE param 56, 2.15.0 on) says which law and version decided; the check
refuses a log whose law in force is not stepped/graded v2, instead of judging
it by the wrong rules. When the law changes, change RULES_VERSION and the rules
here in the same commit.

WHAT IT CHECKS (exact, from the MODE rows alone)
  no vote     step_rh == -1                  -> step == step_t
  both open   step_t > 0 and step_rh > 0     -> step == max(step_t, step_rh)
  agree       step_t == step_rh              -> step == step_t
  rule 1      step_t > 0 and step_rh == 0    -> step == step_t, every priority
  alone       step_t == 0 and step_rh > 0    -> priority 0: step == 0;
                                                priority 1/2: step 0 or 1
The priority, t_min_day/t_min_ngt and the law are tracked through the log
itself (SETPT params 12, 1 and 3; MODE param 56), from the first row of the
first file, so rows before --since still set the state the window starts in.
**A decision is judged only when the law behind it is known.** T6 writes the
law row at its first cycle, before its first decision, and a boot (SYSTEM
value_a 5) forgets it. A decision with no law row since its boot, or with none
anywhere in the logs, may come from firmware before 2.15.0 and the old law, so
it is counted and NOT judged, unless --assume-v2 says otherwise.

THE FLOOR (approximate, needs --law-watch)
T6 acts on T5's averaged temperature, which the log does not carry;
bin/law_watch.py records it once a minute. MODE rows are edge-triggered on the
resolved step, so a humidity-only step-1 row always starts a NEW opening, and
must meet the full floor: round(T_avg) >= t_min + 2. A humidity-only step-0 row
that ends such an opening must have dropped below the hold: round(T_avg) <
t_min + 1. Rows are stamped with T4's cached clock, up to a minute early, so
each is paired with the first watch sample in the 150 s after its stamp. A
floor finding is a pointer to read by hand, never a verdict.

USAGE
-----
    python bin/law_conformance.py LOG.csv [LOG2.csv ...] [--since ISO] [--until ISO]
        [--law-watch law_watch.csv] [--t-min-day 16] [--t-min-ngt 14] [--priority0 N]
        [--assume-v2]

Give the SD logs oldest first. Exit 0 = every judged decision conforms, 1 = a
decision against the rules, 2 = the log's law is not the one these rules
describe, 3 = nothing judged (no decision with a known law in the window).
"""
import argparse
import csv
import math
import sys
from datetime import datetime, timedelta

RULES_VERSION = 2                    # stepped v2 / graded v2
LAWS = {1: "stepped", 2: "graded"}   # MODE param 56 value_a (climate_control.cpp law_log_id())


def int8(v):
    v &= 0xFF
    return v - 256 if v > 127 else v


def lround(x):
    """C lroundf(): half away from zero (Python's round() is half-to-even)."""
    return int(math.copysign(math.floor(abs(x) + 0.5), x))


def load_rows(paths):
    rows = []
    for p in paths:
        with open(p, encoding="utf-8", errors="replace", newline="") as fh:
            for r in csv.DictReader(fh):
                try:
                    rows.append((r.get("timestamp") or "", (r.get("type") or "").strip(),
                                 int(r.get("param") or 0), int(r.get("value_a")),
                                 int(r.get("value_b"))))
                except (TypeError, ValueError):
                    continue
    return rows


def load_watch(path):
    out = []
    with open(path, encoding="utf-8", newline="") as fh:
        for r in csv.DictReader(fh):
            try:
                out.append((datetime.strptime(r["iso"], "%Y-%m-%d %H:%M:%S"),
                            float(r["temp_avg_c"]), r["day"] == "True"))
            except (KeyError, ValueError):
                continue
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--since", default="")
    ap.add_argument("--until", default="")
    ap.add_argument("--law-watch", default="")
    ap.add_argument("--t-min-day", type=int, default=16, help="until a SETPT 1 row says otherwise")
    ap.add_argument("--t-min-ngt", type=int, default=14, help="until a SETPT 3 row says otherwise")
    ap.add_argument("--priority0", type=int, default=None,
                    help="cr_priority until the first SETPT 12 row (default: unknown)")
    ap.add_argument("--assume-v2", action="store_true",
                    help="judge decisions with no law row before them as stepped v2 anyway")
    a = ap.parse_args()
    watch = load_watch(a.law_watch) if a.law_watch else []

    pri = a.priority0
    t_min = {True: a.t_min_day, False: a.t_min_ngt}
    law = None                       # (law id, version) from the latest MODE param 56 row
    boot_ts = None                   # the last boot, which forgets the law
    unknown = 0                      # decisions with no law row since their boot
    laws_seen = []
    cases = {}
    problems = []
    floor_notes = []
    n = alone_open = alone_refused = 0
    prev = None                      # the previous judged row's (step, step_t, step_rh)

    for ts, typ, par, va, vb in load_rows(a.logs):
        if typ == "SYSTEM" and va == 5:
            law, boot_ts = None, ts
            continue
        if typ == "SETPT" and par == 12:
            pri = vb
            continue
        if typ == "SETPT" and par in (1, 3):
            t_min[par == 1] = vb
            continue
        if typ == "MODE" and par == 56:
            law = (va, vb)
            if (not a.since or ts >= a.since) and (not a.until or ts <= a.until):
                laws_seen.append((ts, va, vb))
            continue
        if typ != "MODE" or par != 0:
            continue
        if (a.since and ts < a.since) or (a.until and ts > a.until):
            prev = (va, int8(vb >> 8), int8(vb))
            continue
        step, st, sr = va, int8(vb >> 8), int8(vb)
        if law is None and not a.assume_v2:
            unknown += 1                 # no law row since the boot, or at all: not judged
            prev = (step, st, sr)
            continue
        if law is not None and not (law[0] in LAWS and law[1] == RULES_VERSION):
            print("REFUSED: at %s the law in force is %s v%s; these rules are stepped/graded v%d"
                  % (ts, LAWS.get(law[0], "law #%d" % law[0]), law[1], RULES_VERSION))
            return 2
        n += 1
        if sr == -1:
            case, ok = "no vote", step == st
        elif st > 0 and sr > 0:
            case, ok = "both open", step == max(st, sr)
        elif st == sr:
            case, ok = "agree", step == st
        elif st > 0 and sr == 0:
            case, ok = "rule 1 (dry against heat)", step == st
        elif st == 0 and sr > 0:
            case = "humidity alone, priority %s" % ("unknown" if pri is None else pri)
            ok = (step == 0) if pri == 0 else (step in (0, 1))
            if step == 1:
                alone_open += 1
            else:
                alone_refused += 1
            if watch and pri in (1, 2):
                t0 = datetime.strptime(ts, "%Y-%m-%dT%H:%M:%S")
                near = [w for w in watch if t0 <= w[0] <= t0 + timedelta(seconds=150)]
                if near:
                    _, t_avg, day = near[0]
                    tm = t_min[day]
                    if step == 1:
                        good = lround(t_avg) >= tm + 2
                        what = "opening needs round(T) >= %d" % (tm + 2)
                    else:
                        was_alone = prev is not None and prev[0] == 1 and prev[1] == 0
                        limit = tm + 1 if was_alone else tm + 2
                        good = lround(t_avg) < limit
                        what = "refusal needs round(T) < %d" % limit
                    floor_notes.append((ts, t_avg, day, what, good))
        else:
            case, ok = "other", False
        k = cases.setdefault(case, [0, 0])
        k[0] += 1
        if not ok:
            k[1] += 1
            problems.append("%s  step %d  step_t %d  step_rh %d  priority %s  [%s]"
                            % (ts, step, st, sr, pri, case))
        prev = (step, st, sr)

    window = ""
    if a.since or a.until:
        window = " (%s .. %s)" % (a.since or "start", a.until or "end")
    print("%d vent-step decisions judged against stepped v%d%s" % (n, RULES_VERSION, window))
    if unknown:
        print("%d more NOT judged: no law row (MODE param 56) since their boot%s, so firmware"
              " before 2.15.0 may have decided them by the old law; --assume-v2 judges them anyway"
              % (unknown, "" if boot_ts else " or anywhere in these logs"))
    if laws_seen:
        print("law rows in the window: " + ", ".join("%s %s v%d" % (t, LAWS.get(i, "#%d" % i), v)
                                                    for t, i, v in laws_seen))
    elif law is not None:
        print("law in force (from before the window): %s v%d" % (LAWS.get(law[0], "#%d" % law[0]), law[1]))
    else:
        print("no law row (MODE param 56) in these logs: firmware before 2.15.0, or no boot or mode"
              " change in them%s." % (" (judged as stepped v%d: --assume-v2)" % RULES_VERSION
                                     if a.assume_v2 else ""))
    for case, (k, bad) in sorted(cases.items()):
        print("  %-34s %5d rows, %d against the rules" % (case, k, bad))
    for must in ("rule 1 (dry against heat)", "both open"):
        if must not in cases:
            print("  %-34s NOT EXERCISED in this window" % must)
    print("humidity alone: %d openings (M1 at most), %d refused" % (alone_open, alone_refused))
    if floor_notes:
        low = [f for f in floor_notes if not f[4]]
        print("floor, from the law watch: %d humidity-only rows paired, %d to read by hand"
              % (len(floor_notes), len(low)))
        for ts, t_avg, day, what, good in floor_notes:
            print("  %s  T_avg %.1f (%s)  %s  %s" % (ts, t_avg, "day" if day else "night", what,
                                                   "ok" if good else "READ BY HAND"))
    elif a.law_watch:
        print("floor: no humidity-only row under priority 1/2 overlaps the law watch")
    if problems:
        print("\nAGAINST THE RULES:")
        for p in problems:
            print("  " + p)
    if n == 0:
        print("\nNOTHING JUDGED: no decision in this window has a known law")
        return 3
    print("\n%s" % ("CONFORMS: every decision follows stepped v%d's resolver" % RULES_VERSION
                    if not problems else "%d DECISION(S) AGAINST THE RULES" % len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
