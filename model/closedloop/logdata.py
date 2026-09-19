"""
logdata.py -- what the simulator reads from 5C88's SD logs. The outdoor and
door data are joined in dataset.py, from the raw LoRa exports.

SD logs (raw CSV: timestamp,type,initiator,ch,param,value_a,value_b). The
files overlap, so identical rows are de-duplicated.

  SENSOR_HR ch0  value_a = t_c10, which is the FG6485A register itself
                 (lroundf(t * 10) of regs/10.0f), value_b = RH, whole %
  SENSOR_HR ch1  value_a = wind m/s x10, value_b = direction, deg
  SENSOR_HR ch2  value_a = window bitmask: 2 bits per channel (0 CLOSED,
                 1 MOVING_OPEN, 2 OPEN, 3 MOVING_CLOSE), bit 12 WIND_OVERRIDE,
                 bit 13 MOTOR_ALARM, bit 14 CALIBRATING
  MODE           value_a = resolved step, value_b = step_t << 8 | step_rh,
                 both int8; param 47 is a STANDBY row (2.6.0+) and is skipped
  RELAY          ch = motor 1..3, value_a = ch_state_t (see firmware.RELAY_TO_CH)
  SUN            value_a = sunrise, value_b = sunset, minutes after local midnight
  SYSTEM         value_a 5 = BOOT
  ALARM          ch 4 = T/RH sensor fault, ch 5 = wind sensor fault:
                 value_a 1 onset, 0 clear
  SETPT          one row per setting change: param = the key's audit id
                 (LOG_PARAM_*, the channel picks the motor), value_a old,
                 value_b new. settings.py turns them into the settings in force
  SESSION        initiator ADMIN: value_a 2 = start, 0 = end. On the firmware
                 5C88 ran, an LCD admin session is the manual-motor menu,
                 which holds STANDBY for its whole length

Timestamps are local (Europe/Amsterdam) and naive, as the logs write them.

Two traps, both from memory/gotcha-log.md 2026-07-28 and this module's own
testing:
  * MODE rows carry a clock that lags the sample that produced them by up to
    one poll (a row stamped 10:19:03 belongs to the 10:19:29 sample). A MODE
    row is matched with the FIRST SAMPLE AT OR AFTER its timestamp -- the
    rule vent_step_replay.py uses.
  * OPEN is 2 in the bitmask but 3 in a RELAY row.
"""

from __future__ import annotations

import bisect
import csv
import glob
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from pathlib import Path

MODEL_DIR = Path(__file__).resolve().parent.parent
CAMPAIGN  = MODEL_DIR / "campaign-summer-2026"

BIT_WIND_OVERRIDE = 1 << 12
BIT_MOTOR_ALARM   = 1 << 13
BIT_CALIBRATING   = 1 << 14

TS_FMT = "%Y-%m-%dT%H:%M:%S"


def _int8(b):
    b &= 0xFF
    return b - 256 if b > 127 else b


class Series:
    """A time-ordered (ts, value) list with forward-fill lookup."""

    def __init__(self, pairs):
        pairs = sorted(pairs, key=lambda p: p[0])
        self.ts = [p[0] for p in pairs]
        self.val = [p[1] for p in pairs]

    def at(self, ts, default=None):
        """The last value at or before ts."""
        i = bisect.bisect_right(self.ts, ts) - 1
        return self.val[i] if i >= 0 else default

    def between(self, t0, t1):
        """Values with t0 < ts <= t1, in order."""
        i = bisect.bisect_right(self.ts, t0)
        j = bisect.bisect_right(self.ts, t1)
        return list(zip(self.ts[i:j], self.val[i:j]))

    def __len__(self):
        return len(self.ts)


class Intervals:
    """Closed-open [start, end) intervals, from onset/clear edges."""

    def __init__(self, spans):
        self.spans = sorted(spans)
        self.starts = [s for s, _ in self.spans]

    def contains(self, ts):
        i = bisect.bisect_right(self.starts, ts) - 1
        return i >= 0 and self.spans[i][0] <= ts < self.spans[i][1]

    def overlap_s(self, t0, t1):
        tot = 0.0
        for s, e in self.spans:
            lo, hi = max(s, t0), min(e, t1)
            if hi > lo:
                tot += (hi - lo).total_seconds()
        return tot


def _edges_to_intervals(edges, open_values, close_values, end, min_len_s=0):
    """edges: ((ts, file_seq), value), sorted by time, then by file order.

    min_len_s: the shortest span an onset/clear pair can stand for. ALARM
    rows are stamped with the same minute-stale clock as MODE rows, so a
    T/RH fault whose onset and clear share a stamp still lasted at least one
    failed poll -- during which T6 was inhibited and its steps were reset.
    """
    spans, cur = [], None
    for (ts, _seq), v in sorted(edges, key=lambda e: e[0]):
        if v in open_values and cur is None:
            cur = ts
        elif v in close_values and cur is not None:
            stop = max(ts, cur + timedelta(seconds=min_len_s))
            if stop > cur:
                spans.append((cur, stop))
            cur = None
    if cur is not None:
        spans.append((cur, end))
    return Intervals(spans)


@dataclass
class LogData:
    samples:  list                      # (ts, t_c10, rh) from SENSOR_HR ch0
    wind:     Series                    # (ws10, dir)
    bitmask:  Series                    # raw 16-bit value
    modes:    list                      # (ts, step, step_t, step_rh)
    relay:    list                      # (ts, channel 0..2, value_a)
    sun:      dict                      # date -> (sunrise_min, sunset_min)
    boots:    list                      # ts
    tfault:   Intervals                 # T/RH sensor fault, inhibits T6
    standby:  Intervals                 # LCD admin sessions (manual-motor menu)
    files:    list = field(default_factory=list)
    wfault:   Intervals = None          # wind sensor fault: T3 safe-fails, T5's wind averages stop
    setpts:   list = field(default_factory=list)   # SETPT audit rows, see settings.py

    def is_day(self, ts):
        """T4's is_daytime from the logged SUN pair (nearest earlier date)."""
        d = ts.date()
        pair = self.sun.get(d)
        if pair is None and self.sun:
            earlier = [k for k in self.sun if k <= d]
            pair = self.sun[max(earlier)] if earlier else self.sun[min(self.sun)]
        sr, ss = pair if pair else (355, 1299)
        mins = ts.hour * 60 + ts.minute
        return sr <= mins < ss

    def override(self, ts):
        bm = self.bitmask.at(ts, 0)
        return bool(bm & BIT_WIND_OVERRIDE)


def load_sd_logs(paths):
    """Parse raw SD CSV files into a LogData."""
    if isinstance(paths, (str, Path)):
        paths = [paths]
    files = []
    for p in paths:
        hits = sorted(glob.glob(str(p)))
        files.extend(hits if hits else [str(p)])

    # row -> its first position in file order. Order matters where the clock
    # cannot tell: a fault onset and its clear are often written within the
    # same second (2026-06-10 16:04:14, 2026-09-16 05:08:20), and sorting them
    # by value would put the clear first and leave a fault open for weeks.
    rows = {}
    for path in files:
        with open(path, newline="", encoding="utf-8", errors="replace") as fh:
            for r in csv.DictReader(fh):
                try:
                    key = (r["timestamp"], r["type"], r["initiator"], int(r["ch"]),
                           int(r.get("param") or 0), int(r["value_a"]), int(r["value_b"]))
                except (ValueError, TypeError, KeyError):
                    continue
                rows.setdefault(key, len(rows))

    samples, wind, bmask, modes, relay = {}, [], [], [], []
    sun, boots, fault_edges, session_edges = {}, [], [], []
    wfault_edges, setpts = [], []
    for (ts_s, typ, ini, ch, par, va, vb), seq in rows.items():
        try:
            ts = datetime.strptime(ts_s, TS_FMT)
        except ValueError:
            continue
        if typ == "SENSOR_HR":
            if ch == 0:
                samples[ts] = (va, vb)
            elif ch == 1:
                wind.append((ts, (va, vb)))
            elif ch == 2:
                bmask.append((ts, va))
        elif typ == "MODE":
            if par == 47:
                continue
            u = vb & 0xFFFF
            modes.append((ts, va, _int8(u >> 8), _int8(u)))
        elif typ == "RELAY":
            if 1 <= ch <= 3:
                relay.append((ts, ch - 1, va))
        elif typ == "SUN":
            sun[ts.date()] = (va, vb)
        elif typ == "SYSTEM" and va == 5:
            boots.append(ts)
        elif typ == "ALARM" and ch == 4:
            fault_edges.append(((ts, seq), va))
        elif typ == "ALARM" and ch == 5:
            wfault_edges.append(((ts, seq), va))
        elif typ == "SESSION" and ini == "ADMIN":
            session_edges.append(((ts, seq), va))
        elif typ == "SETPT":
            setpts.append((ts, seq, ini, ch, par, va, vb))

    samp = sorted((ts, t, rh) for ts, (t, rh) in samples.items())
    end = samp[-1][0] + timedelta(seconds=1) if samp else datetime.max
    return LogData(
        samples=samp,
        wind=Series(wind),
        bitmask=Series(bmask),
        modes=sorted(set(modes)),
        relay=sorted(set(relay)),
        sun=sun,
        boots=sorted(set(boots)),
        tfault=_edges_to_intervals(fault_edges, {1}, {0}, end, min_len_s=30),
        standby=_edges_to_intervals(session_edges, {2}, {0}, end),
        files=files,
        wfault=_edges_to_intervals(wfault_edges, {1}, {0}, end, min_len_s=30),
        setpts=sorted(setpts, key=lambda r: (r[0], r[1])),
    )
