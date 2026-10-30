#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Watch the Modbus bus live while someone changes the wiring: a line per interval.

WHY
---
Bisecting a bus fault means changing one thing and seeing, within a minute,
whether the failure rate moved. `/api/status` carries each slave's counters in
`bus[]` (ok, err, busy, and `max`, the longest run of failures), but they are
totals since boot, so a fault that has run for an hour buries any change. This
prints what happened in the last interval only, per slave, on any build, with
no login: the public status is enough.

READING IT
----------
T5 reads addr 1 and addr 44 every `poll_interval_s` (30 s), with one retry after
a failure. A poll of addr 1 is one transaction and of addr 44 two, so a healthy
minute is 2 ok for addr 1 and 4 ok for addr 44. T17 reads addr 40 only when
`wpos_fitted_m3` is 1: every 30 s while it probes, every 30 s at rest once the
gate is ok, much faster while M3 moves.

A slave's `err` does not say why. Timeouts, CRC and framing errors are told
apart in the SD log (T5's ALARM rows carry the driver status in value_b) and on
a bench build's `GET /api/diag/windowpos`.

USAGE
-----
    python bin/bus_watch.py --host 192.168.1.104
    python bin/bus_watch.py --host 192.168.1.104 --every 60 --count 10

Ctrl-C stops it and prints the totals since it started.
"""

import argparse
import json
import sys
import time
import urllib.request


def status(host):
    with urllib.request.urlopen("http://%s/api/status" % host, timeout=10) as r:
        return json.load(r)


def bus(st):
    return dict((b["a"], b) for b in (st.get("bus") or []))


def pct(ok, err):
    return "  --" if ok + err == 0 else "%3d%%" % round(100.0 * ok / (ok + err))


def line(prev, now):
    """One column per slave: `ok/err pct` for this interval."""
    cols = []
    for a in sorted(now):
        n = now[a]
        p = prev.get(a, {"ok": 0, "err": 0})
        d_ok = n["ok"] - p["ok"]
        d_err = n["err"] - p["err"]
        cols.append("%2d: %3d ok %3d err %s" % (a, d_ok, d_err, pct(d_ok, d_err)))
    return "  |  ".join(cols)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="192.168.1.104")
    ap.add_argument("--every", type=float, default=60.0, help="seconds per line")
    ap.add_argument("--count", type=int, default=0, help="lines, 0 = until Ctrl-C")
    a = ap.parse_args()

    st = status(a.host)
    y = st["system"]
    print("%s fw %s, up %s s, gate %s, flags %s"
          % (y.get("unit_id"), y.get("fw_ver"), y.get("uptime_s"),
             (st.get("windows") or {}).get("M3_pos_gate"),
             ",".join((st.get("mode") or {}).get("flags") or []) or "-"), flush=True)
    start = prev = bus(st)
    n = 0
    try:
        while a.count == 0 or n < a.count:
            time.sleep(a.every)
            try:
                st = status(a.host)
            except Exception as e:                              # noqa: BLE001
                print("%s  status unreadable: %s" % (time.strftime("%H:%M:%S"), e), flush=True)
                continue
            now = bus(st)
            flags = [f.replace("sensor_fault_", "F:") for f in (st.get("mode") or {}).get("flags") or []
                     if f.startswith("sensor_fault") or f == "wind_override"]
            print("%s  %s  [%s]" % (time.strftime("%H:%M:%S"), line(prev, now), " ".join(flags) or "no faults"),
                  flush=True)
            prev = now
            n += 1
    except KeyboardInterrupt:
        pass
    print("since start: %s" % line(start, prev), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
