#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""AT for gh#73: the "M3 position sensor fitted" setting (`motor/wpos_fitted_m3`).

WHAT IT CHECKS
--------------
Before 2.9.0 a unit could not tell "no sensor fitted" from "a fitted sensor that
does not answer". A unit without one re-probed address 40 every 30 s for ever,
and its Bus card and hourly log rows showed a failing slave. The setting
decides. Default 0, not fitted.

Run the stages in order, on a unit that has just taken 2.9.0 and has the encoder
connected (the dev rig). Each stage checks only what the public status and the
config API can show, because a release build has no diagnostic routes.

  unfitted  The state after an update: the setting reads 0, address 40 is not in
            the bus array, M3 has no position keys and no fault flag, for the
            whole watch.
  on        Switch the setting on. The sensor must show up within seconds, and
            its since-boot counters must be SMALL. That is the proof that T17
            sent nothing while the unit was unfitted: counters run from boot, and
            a T17 still reading at rest would have ~2 transactions per 30 s of
            uptime by now. Only meaningful if the unit booted unfitted.
  unplug    Operator stage. With the setting on, unplug the encoder's bus
            connector: the fault flag must appear within ~70 s of the first
            failed read, with no position keys. Plug it back in: the flag must
            clear. The script waits; it cannot see the connector.
  off       Switch the setting off at run time: the address-40 row and the
            position keys go, no flag appears. After the watch the setting goes
            back on, and the counters must have grown only by the switch-on's own
            few transactions.
  log       Read the newest SD log: the mode rows (param 248) with their
            reasons, the setting's audit rows (param 49), whether any position
            row was written while unfitted, and the hourly bus rows (LOG_SYSTEM
            31) for address 40 against the fitted state at that hour.

USAGE
-----
    python bin/at_wpos_fitted.py --host 192.168.20.160 unfitted
    python bin/at_wpos_fitted.py --host 192.168.20.160 on
    python bin/at_wpos_fitted.py --host 192.168.20.160 unplug
    python bin/at_wpos_fitted.py --host 192.168.20.160 off [--watch 300]
    python bin/at_wpos_fitted.py --host 192.168.20.160 log [--since "2026-09-17 13:00"]

Exit 0 = pass, 1 = fail, 2 = could not run. Stdlib only, ASCII output.
"""

import argparse
import csv
import http.client
import io
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.append(os.path.join(HERE, "..", "log"))

from at_wp_ramp import Unit, DEFAULT_PIN                    # noqa: E402
from at_wp_teach_standby import public_status, flags        # noqa: E402
import logparser                                             # noqa: E402

KEY = "wpos_fitted_m3"
ADDR = 40
FAULT_FLAG = "sensor_fault_position"
# Transactions a switch-on costs before its first 30 s idle interval: 6. Four
# are the identify probe, the probe's position read (which since 2.9.1 also
# serves the orphan check), the first idle read and its restart check. The other
# two are commission_refresh() reading the device config and a position when the
# gate opens -- measured on 2344, 2026-09-17.
#
# gh#77 (2026-09-20): those two used to be a BENCH-ONLY cost, and a release
# build showed 4. Commissioning is compiled into every build now, so both builds
# cost 6 and this limit no longer distinguishes them. Six is still well below
# what idle reading adds: ~4 a minute.
SWITCH_ON_TXN_MAX = 6

FAILS = []


def say(msg):
    print("  %s  %s" % (time.strftime("%H:%M:%S"), msg))
    sys.stdout.flush()


def check(label, cond):
    say(("PASS  " if cond else "FAIL  ") + label)
    if not cond:
        FAILS.append(label)
    return cond


def banner(lines):
    print("")
    print("  " + "=" * 72)
    for ln in lines:
        print("  >>> " + ln)
    print("  " + "=" * 72)
    sys.stdout.flush()


# ------------------------------------------------------------------ reading
def status(host):
    st = public_status(host)
    if st is None:
        return None
    return st


def bus_row(st):
    for b in (st or {}).get("bus") or []:
        if b.get("a") == ADDR:
            return b
    return None


def has_pos(st):
    return "M3_percent_x10" in ((st or {}).get("windows") or {})


def txn(row):
    return (row or {}).get("ok", 0) + (row or {}).get("err", 0)


def describe(st):
    row = bus_row(st)
    return ("uptime %ss, addr40 %s, M3 %s%s, flags %s"
            % (st["system"]["uptime_s"],
               ("ok %(ok)s err %(err)s busy %(busy)s max %(max)s" % row) if row else "absent",
               st["windows"].get("M3"),
               (" %.1f %%" % (st["windows"]["M3_percent_x10"] / 10.0)) if has_pos(st) else "",
               flags(st)))


def fitted(a):
    u = Unit(a.host, a.pin)
    try:
        cfg = u.cfg()
    finally:
        u.logout()
    if not isinstance(cfg, dict) or KEY not in cfg:
        sys.exit("GET /api/config has no %s: this firmware predates gh#73 (2.9.0)" % KEY)
    return cfg[KEY]


def set_fitted(a, value):
    u = Unit(a.host, a.pin)
    try:
        sc = u.post_cfg("motor", KEY, value)
        if sc != 200:
            sys.exit("POST %s=%s answered HTTP %s" % (KEY, value, sc))
        # POST /api/config is asynchronous (Q4): wait for the read-back.
        if not u.settle_cfg(KEY, value):
            sys.exit("%s did not read back as %s" % (KEY, value))
    finally:
        u.logout()
    say("%s set to %s (read back)" % (KEY, value))


def identify(a):
    st = status(a.host)
    if st is None:
        sys.exit("no answer from %s" % a.host)
    s = st["system"]
    say("unit %s at %s: fw %s, assets %s" % (s["unit_id"], a.host, s["fw_ver"], s["asset_version"]))
    return st


def poll_until(host, pred, limit_s, every=0.5):
    """(seconds, status) when pred(status) first holds, or (None, last status)."""
    t0 = time.time()
    last = None
    while time.time() - t0 < limit_s:
        st = status(host)
        if st is not None:
            last = st
            if pred(st):
                return time.time() - t0, st
        time.sleep(every)
    return None, last


def watch_quiet(host, secs, label):
    """Every public status for `secs` shows no addr 40, no position, no flag."""
    end = time.time() + secs
    n = bad = 0
    while time.time() < end:
        st = status(host)
        if st is not None:
            n += 1
            if bus_row(st) or has_pos(st) or FAULT_FLAG in flags(st):
                bad += 1
                say("  seen: " + describe(st))
        time.sleep(10)
    check("%s: %d readings over %d s, none with addr 40, a position or the flag"
          % (label, n, secs), n > 0 and bad == 0)


# ------------------------------------------------------------------- stages
def stage_unfitted(a):
    st = identify(a)
    check("%s reads 0 (not fitted)" % KEY, fitted(a) == 0)
    say("now: " + describe(st))
    check("address 40 is not in the bus array", bus_row(st) is None)
    check("M3 has no position keys", not has_pos(st))
    check("no %s flag" % FAULT_FLAG, FAULT_FLAG not in flags(st))
    watch_quiet(a.host, a.watch, "unfitted")


def stage_on(a):
    st = identify(a)
    if fitted(a) != 0:
        sys.exit("%s is already 1; this stage starts from not fitted" % KEY)
    up = st["system"]["uptime_s"]
    say("before: " + describe(st))
    if bus_row(st) is not None:
        say("note: address 40 already has a row, so it was fitted earlier in this boot;"
            " the counter check below is then not conclusive")
    set_fitted(a, 1)
    dt, st = poll_until(a.host, lambda s: bus_row(s) is not None and has_pos(s), 15)
    check("sensor seen within 15 s of the read-back (%s s)" % ("%.1f" % dt if dt else "never"),
          dt is not None)
    time.sleep(3)
    st = status(a.host) or st
    say("after: " + describe(st))
    row = bus_row(st)
    would = 2 * up // 30
    check("addr 40 since-boot transactions %d <= %d (a T17 still reading at rest "
          "would have ~%d after %d s up)" % (txn(row), SWITCH_ON_TXN_MAX, would, up),
          row is not None and txn(row) <= SWITCH_ON_TXN_MAX)
    check("no failed transactions at addr 40", row is not None and row.get("err") == 0)
    check("no %s flag" % FAULT_FLAG, FAULT_FLAG not in flags(st))
    if would <= SWITCH_ON_TXN_MAX * 2:
        say("INCONCLUSIVE counter check: the unit has not been up long enough "
            "(%d s) for idle reads to stand out" % up)


def stage_unplug(a):
    st = identify(a)
    if fitted(a) != 1 or not has_pos(st):
        sys.exit("needs the setting on and the sensor answering: " + describe(st))
    row0 = bus_row(st)
    banner(["UNPLUG the encoder's bus connector now.",
            "This script waits for the controller to notice (up to %d s)." % a.limit])
    t_err, st = poll_until(a.host, lambda s: (bus_row(s) or {}).get("err", 0) > row0["err"],
                           a.limit, every=1)
    if t_err is None:
        check("a failed read at address 40 within %d s" % a.limit, False)
        return
    say("first failed read seen: " + describe(st))
    t_flag, st = poll_until(a.host, lambda s: FAULT_FLAG in flags(s), 90, every=1)
    check("fault flag within 90 s of the first failed read (%s s)"
          % ("%.0f" % t_flag if t_flag is not None else "never"), t_flag is not None)
    if t_flag is not None:
        say("flagged: " + describe(st))
        check("no position keys while flagged", not has_pos(st))
        check("address 40 still listed (a fitted sensor stays visible)", bus_row(st) is not None)
    banner(["PLUG the encoder back in now.",
            "This script waits for the flag to clear (up to %d s)." % a.limit])
    t_ok, st = poll_until(a.host, lambda s: FAULT_FLAG not in flags(s) and has_pos(s),
                          a.limit, every=1)
    check("flag cleared and position back (%s s after the prompt)"
          % ("%.0f" % t_ok if t_ok is not None else "never"), t_ok is not None)
    if st is not None:
        say("after: " + describe(st))


def stage_off(a):
    st = identify(a)
    if fitted(a) != 1 or not has_pos(st):
        sys.exit("needs the setting on and the sensor answering: " + describe(st))
    before = txn(bus_row(st))
    say("before: " + describe(st))
    set_fitted(a, 0)
    dt, st = poll_until(a.host, lambda s: bus_row(s) is None and not has_pos(s), 15)
    check("address 40 and the position gone within 15 s (%s s)"
          % ("%.1f" % dt if dt else "never"), dt is not None)
    check("no %s flag" % FAULT_FLAG, FAULT_FLAG not in flags(st or {}))
    watch_quiet(a.host, a.watch, "switched off")
    set_fitted(a, 1)
    dt, st = poll_until(a.host, lambda s: bus_row(s) is not None and has_pos(s), 15)
    time.sleep(3)
    st = status(a.host) or st
    say("switched on again: " + describe(st))
    grown = txn(bus_row(st)) - before
    check("addr 40 transactions grew by %d over %d s off (<= %d: only the switch-on's own)"
          % (grown, a.watch, SWITCH_ON_TXN_MAX), 0 <= grown <= SWITCH_ON_TXN_MAX)


# ---------------------------------------------------------------------- log
def newest_log(a):
    """(name, rows) of the newest SD log file. The card is shared by the rig's
    modules, so the newest file overall, not this unit's newest."""
    u = Unit(a.host, a.pin)
    try:
        sc, files = u._req("GET", "/api/log/files")
        names = files.get("sd_files") if isinstance(files, dict) else None
        if sc != 200 or not names:
            sys.exit("no SD log listed (HTTP %s)" % sc)
        newest = max(names, key=lambda n: re.sub(r"\D", "", n)[-14:])
        c = http.client.HTTPConnection(a.host, 80, timeout=120)
        try:
            c.request("GET", "/api/log/download?file=" + newest, headers={"Cookie": u.cookie})
            r = c.getresponse()
            raw = r.read().decode("utf-8", "replace")
            if r.status != 200:
                sys.exit("download of %s answered HTTP %s" % (newest, r.status))
        finally:
            c.close()
    finally:
        u.logout()
    return newest, list(csv.DictReader(io.StringIO(raw)))


def num(row, field):
    try:
        return int((row.get(field) or "").strip())
    except ValueError:
        return None


def stage_log(a):
    st = identify(a)
    unit = int(st["system"]["unit_id"], 16)
    name, rows = newest_log(a)
    say("%s: %d rows" % (name, len(rows)))

    # Keep this unit's rows only. The card is shared by the rig's modules, and
    # each boot row (SYSTEM 5) is followed by a unit-id row (SYSTEM 11) naming
    # the writer of everything up to the next boot. Rows before the first boot
    # in the file have no known writer and are left out.
    segs, cur = [], None
    for r in rows:
        kind = (r.get("type") or "").strip().upper()
        if kind == "SYSTEM" and num(r, "value_a") == 5:
            cur = {"unit": None, "rows": [r]}
            segs.append(cur)
            continue
        if cur is None:
            continue
        if kind == "SYSTEM" and num(r, "value_a") == 11 and cur["unit"] is None:
            cur["unit"] = (num(r, "value_b") or 0) & 0xFFFF
        cur["rows"].append(r)
    rows = [r for s in segs if s["unit"] == unit for r in s["rows"]]
    if a.since:
        rows = [r for r in rows if logparser._fmt_local(r.get("timestamp", "")) >= a.since]
    say("%d rows written by %s%s" % (len(rows), st["system"]["unit_id"],
                                     (" since " + a.since) if a.since else ""))

    # Two fitted states, because two tasks act on the setting:
    #  - t17: T17's own view, from its mode rows (param 248; reason 5 = not
    #    fitted). It decides whether position rows may be written, and it
    #    follows the setting up to one idle tick after the audit row.
    #  - cfg: the setting itself, from its audit rows (param 49). T4's hourly
    #    bus rows read it directly. After a boot it is inferred from T17's
    #    first mode row, which reflects the setting the unit booted with.
    t17 = cfg = None
    pos_rows_unfitted = 0
    kpi = []                                # (timestamp, addr, cfg state)
    for i, r in enumerate(rows):
        kind = (r.get("type") or "").strip().upper()
        ch, param = num(r, "ch"), num(r, "param")
        va, vb = num(r, "value_a"), num(r, "value_b")
        show = False
        if kind == "SYSTEM" and va == 5:
            t17 = cfg = None
            show = True
        elif kind == "SETPT" and param == 49:
            cfg = bool(vb)
            show = True
        elif kind == "ALARM" and ch == 6 and param == 248:
            t17 = (vb != 5)
            if cfg is None:
                cfg = t17
            show = True
        elif kind == "SENSOR_HR" and ch == 3 and t17 is False:
            pos_rows_unfitted += 1
        elif kind == "SYSTEM" and va == 31 and param == 50:
            kpi.append((r.get("timestamp", ""), ch, cfg))
        if show:
            print("    " + logparser._format_row(r, i))
    check("no position rows (SENSOR_HR ch 3) while not fitted (%d)" % pos_rows_unfitted,
          pos_rows_unfitted == 0)
    hours = {}
    for ts, addr, fit in kpi:
        hours.setdefault(ts[:16], {"addrs": set(), "fitted": fit})["addrs"].add(addr)
    if not hours:
        say("no hourly bus rows (LOG_SYSTEM 31) in range yet -- they start 2 h after boot")
    for hour in sorted(hours):
        h = hours[hour]
        has40 = ADDR in h["addrs"]
        if h["fitted"] is None:
            say("bus rows at %s: addresses %s (fitted state unknown here)"
                % (hour, sorted(h["addrs"])))
            continue
        check("bus rows at %s: addresses %s, fitted=%s -> address 40 %s"
              % (hour, sorted(h["addrs"]), h["fitted"], "present" if has40 else "absent"),
              has40 == h["fitted"])


STAGES = {"unfitted": stage_unfitted, "on": stage_on, "unplug": stage_unplug,
          "off": stage_off, "log": stage_log}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("stage", choices=sorted(STAGES))
    ap.add_argument("--host", required=True, help="the unit; the rig's modules are .169 and .160")
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("--watch", type=int, default=300,
                    help="seconds to watch in 'unfitted' and 'off' (default 300)")
    ap.add_argument("--limit", type=int, default=600,
                    help="seconds to wait for the operator in 'unplug' (default 600)")
    ap.add_argument("--since", help='log stage: only rows from this local time on, '
                                    '"YYYY-MM-DD HH:MM"')
    a = ap.parse_args()
    print("AT gh#73 -- stage '%s' on %s" % (a.stage, a.host))
    STAGES[a.stage](a)
    print("RESULT: %s" % ("PASS" if not FAILS else "FAIL (%d)" % len(FAILS)))
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
