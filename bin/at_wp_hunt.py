#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Did any targeted M3 stop get answered by a correction back to the same target?

Plan §5e's soak criterion: "no targeted stop answered by an opposite correction
to the same target". A stop that comes to rest outside the band around its
target is re-commanded by the law, and T2 then drives back: hunting. With the
band derived by the characterisation run in force, a stop should come to rest
inside it, so nothing is ever corrected back.

It reads the stop log a soak collects (bin/at_wp_cuts.py --save cuts_soak.json,
fetched hourly, merged by (unit, boot, n)), so it needs a bench build's soak.
Two stops that follow each other (n, n + 1, same boot), with the same target
(within --same, 0.1 %), opposite directions, and the second starting where the
first came to rest, are a hunt. That last condition matters: AT-WP02 approaches
one target from alternate ends, and the full moves to the ends are not in the
stop log.

It also prints where each stop came to rest against its target, in mm, signed
in the direction of travel (+ = past it), next to the band given with --band.

Usage
  python bin/at_wp_hunt.py cuts_soak.json [--band 32] [--same 2]
Exit 0 when there is no hunt, 1 when there is, 2 when there are no stops.
"""
import argparse
import json
import statistics
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("store")
    ap.add_argument("--band", type=float, default=0.0, help="the band in force, mm")
    ap.add_argument("--same", type=int, default=2, help="targets this close are the same, 0.1 %%")
    a = ap.parse_args()

    recs = json.load(open(a.store)).get("records", [])
    if not recs:
        print("no stops in %s" % a.store)
        sys.exit(2)
    hunts = []
    for p, q in zip(recs, recs[1:]):
        if (p["unit"], p["boot"]) != (q["unit"], q["boot"]) or q["n"] != p["n"] + 1:
            continue
        # A hunt starts where the stop it answers came to rest. AT-WP02's
        # alternating approaches to one target start from an END each time,
        # reached by a full move the stop log does not record: not a hunt.
        started_there = p.get("rested") and abs(q["from"] - p["rest"]) <= a.same * 3
        if abs(q["want"] - p["want"]) <= a.same and q["o"] != p["o"] and started_there:
            hunts.append((p, q))

    past = []
    for r in recs:
        if not r.get("rested") or not r.get("window_mm"):
            continue
        sgn = 1 if r["o"] else -1
        want_mm = r["want"] * float(r["window_mm"]) / 1000.0
        past.append(sgn * (r["rest_mm"] / 10.0 - want_mm))

    print("%d targeted stops in %s (%s boot(s))"
          % (len(recs), a.store, len(set((r["unit"], r["boot"]) for r in recs))))
    if past:
        out = [p for p in past if a.band and abs(p) > a.band]
        print("  rest against the target: median %+.1f mm, from %+.1f to %+.1f mm%s"
              % (statistics.median(past), min(past), max(past),
                 ("; outside the %.0f mm band: %d" % (a.band, len(out))) if a.band else ""))
    print("  answered by an opposite correction to the same target: %d" % len(hunts))
    for p, q in hunts:
        print("    stop %d (%s to %.1f %%, rested %.1f %%) -> stop %d (%s to %.1f %%)"
              % (p["n"], "open" if p["o"] else "close", p["want"] / 10.0, p["rest"] / 10.0,
                 q["n"], "open" if q["o"] else "close", q["want"] / 10.0))
    sys.exit(1 if hunts else 0)


if __name__ == "__main__":
    main()
