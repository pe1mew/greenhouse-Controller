"""
dataset.py -- one aligned record per SD sample, for fitting and replaying the plant.

Built from 5C88's SD logs and the raw LoRa exports (fetch_lora_data.py), NOT
from the merged calibration input: that file carries the outdoor data as
10-minute forward-filled steps, which a plant with a fast air node cannot
tolerate, and it ends on 2026-07-12.

Per sample (the SD's 30 s SENSOR_HR cadence):

  T_in, RH_in, AH_in    indoor, as logged (AH in kg/m3, the calibrator's psychrometrics)
  T_out, RH_out, lux    lht65-20, linearly interpolated between uplinks;
                        stale = the bracketing uplinks are more than
                        MAX_OUTDOOR_GAP_S apart (or the sample is outside them)
  o1, o2, o3            each window's MEAN openness over the step into this
                        sample, 0..1, reconstructed from the RELAY rows' exact
                        transition times and the physical traverse (M1/M2
                        26 s, M3 176 s -- travel + the 5 s margin, CLAUDE.md)
  door1, door2          lds01-5 / lds01-6, forward-filled (event-driven sensors);
  door1_known           false after lds01-5's last report (2026-08-16 07:56,
                        "closed"), when "closed" is an assumption, not a reading
  wind_ms, wind_dir     the S200, as logged (valid from 2026-06-19 12:00)
  restart               the gap before this sample exceeds MAX_SAMPLE_GAP_S,
                        so a simulation must restart from the measurement

The window reconstruction is checked against the logged bitmask: see
Dataset.window_check().
"""

from __future__ import annotations

import bisect
import csv
import math
from dataclasses import dataclass
from datetime import datetime, timedelta
from pathlib import Path

import numpy as np

from firmware import CH_CLOSED, CH_MOVING_CLOSE, CH_MOVING_OPEN, CH_OPEN, RELAY_TO_CH
from logdata import CAMPAIGN, load_sd_logs
from plant import ah_from_rh

EPOCH = datetime(2026, 1, 1)
LHT = CAMPAIGN / "lht65_20_2026-06-04_2026-09-17.csv"
DOOR1 = CAMPAIGN / "lds01_5_2026-06-01_2026-09-17.csv"
DOOR2 = CAMPAIGN / "lds01_6_2026-06-01_2026-09-17.csv"

MAX_OUTDOOR_GAP_S = 40 * 60     # 4 missed 10-min uplinks
MAX_SAMPLE_GAP_S = 300          # restart a simulation across longer SD gaps
TRAVERSE_S = (26.0, 26.0, 176.0)
WIND_VALID_FROM = datetime(2026, 6, 19, 12, 0, 0)


def _sec(dt):
    return (dt - EPOCH).total_seconds()


def _read_lora(path, cols):
    out = []
    with open(path, newline="", encoding="utf-8") as fh:
        for r in csv.DictReader(fh):
            try:
                t = datetime.strptime(r["dateTime"], "%Y-%m-%d %H:%M:%S")
                out.append((t,) + tuple(float(r[c]) for c in cols))
            except (ValueError, KeyError, TypeError):
                continue
    out.sort()
    return out


def _interp_outdoor(t_s, lora):
    """Linear interpolation of (T_out, RH_out, lux); stale where uplinks are far apart."""
    lt = np.array([_sec(r[0]) for r in lora])
    vals = np.array([r[1:] for r in lora])
    idx = np.searchsorted(lt, t_s, side="right")
    lo = np.clip(idx - 1, 0, len(lt) - 1)
    hi = np.clip(idx, 0, len(lt) - 1)
    span = lt[hi] - lt[lo]
    w = np.where(span > 0, (t_s - lt[lo]) / np.where(span > 0, span, 1.0), 0.0)
    out = vals[lo] + (vals[hi] - vals[lo]) * w[:, None]
    stale = (span > MAX_OUTDOOR_GAP_S) | (t_s < lt[0]) | (t_s > lt[-1])
    return out[:, 0], out[:, 1], np.maximum(out[:, 2], 0.0), stale


def _ffill_events(t_s, events):
    """Forward-fill (t, value) events onto t_s; 0 before the first."""
    et = [_sec(e[0]) for e in events]
    ev = [e[1] for e in events]
    out = np.zeros(len(t_s))
    for i, t in enumerate(t_s):
        k = bisect.bisect_right(et, t) - 1
        out[i] = ev[k] if k >= 0 else 0.0
    return out


def _openness(t_s, relay_events, first_code, traverse):
    """Mean openness over each step (t[i-1], t[i]] from exact RELAY transitions.

    relay_events: sorted [(t_sec, ch_state)]. first_code: the logged bitmask
    code at the first sample (0 closed, 2 open), for the state before any row.
    """
    pos = {0: 0.0, 1: 0.5, 2: 1.0, 3: 0.5}.get(first_code, 0.0)
    state = {0: CH_CLOSED, 1: CH_MOVING_OPEN, 2: CH_OPEN, 3: CH_MOVING_CLOSE}.get(
        first_code, CH_CLOSED)
    rate = 1.0 / traverse

    def advance(t0, t1):
        """Integrate from t0 to t1 under the current state; return the area."""
        nonlocal pos
        if t1 <= t0:
            return 0.0
        if state == CH_MOVING_OPEN:
            t_full = t0 + (1.0 - pos) / rate
            if t1 <= t_full:
                p1 = pos + rate * (t1 - t0)
                area = (pos + p1) / 2 * (t1 - t0)
                pos = p1
            else:
                area = (pos + 1.0) / 2 * (t_full - t0) + 1.0 * (t1 - t_full)
                pos = 1.0
            return area
        if state == CH_MOVING_CLOSE:
            t_zero = t0 + pos / rate
            if t1 <= t_zero:
                p1 = pos - rate * (t1 - t0)
                area = (pos + p1) / 2 * (t1 - t0)
                pos = p1
            else:
                area = pos / 2 * (t_zero - t0)
                pos = 0.0
            return area
        return pos * (t1 - t0)

    mean = np.zeros(len(t_s))
    at = np.zeros(len(t_s))
    k = 0
    prev_t = t_s[0]
    for i, t in enumerate(t_s):
        area, t0 = 0.0, prev_t
        while k < len(relay_events) and relay_events[k][0] <= t:
            te, st = relay_events[k]
            if te > t0:
                area += advance(t0, te)
                t0 = te
            if st == CH_OPEN:
                pos = 1.0
            elif st == CH_CLOSED:
                pos = 0.0
            state = st      # gap and unknown states hold the leaf: advance() only moves it
            k += 1          # in MOVING_OPEN / MOVING_CLOSE

        area += advance(t0, t)
        span = t - prev_t
        mean[i] = area / span if span > 0 else pos
        at[i] = pos
        prev_t = t
    return mean, at


@dataclass
class Dataset:
    t: list
    t_s: np.ndarray
    dt: np.ndarray
    T_in: np.ndarray
    RH_in: np.ndarray
    AH_in: np.ndarray
    T_out: np.ndarray
    RH_out: np.ndarray
    AH_out: np.ndarray
    lux: np.ndarray
    stale: np.ndarray
    door1: np.ndarray
    door2: np.ndarray
    door1_known: np.ndarray
    o: np.ndarray            # (n, 3) mean openness over the step into each sample
    pos: np.ndarray          # (n, 3) position at each sample
    bm: np.ndarray           # logged bitmask at each sample
    wind_ms: np.ndarray
    wind_dir: np.ndarray
    wind_valid: np.ndarray
    restart: np.ndarray
    log: object              # the LogData it came from

    def __len__(self):
        return len(self.t)

    def day_index(self):
        return np.array([(d.date() - self.t[0].date()).days for d in self.t])

    def window_check(self):
        """Share of samples where the RELAY-derived state disagrees with the bitmask.

        A settled window (logged OPEN or CLOSED) must sit at position 1 or 0.
        Moving states are skipped: their position is legitimately in between.
        """
        out = []
        for ch in range(3):
            code = (self.bm >> (2 * ch)) & 3
            settled = (code == 0) | (code == 2)
            want = np.where(code == 2, 1.0, 0.0)
            bad = settled & (np.abs(self.pos[:, ch] - want) > 0.01)
            out.append((int(bad.sum()), int(settled.sum())))
        return out


def build(start=None, end=None, log_glob=None, lht=LHT, door1=DOOR1, door2=DOOR2, data=None):
    """Assemble the dataset for [start, end) (local datetimes; None = everything)."""
    data = data or load_sd_logs(log_glob or str(CAMPAIGN / "*.log"))
    samp = [s for s in data.samples
            if (start is None or s[0] >= start) and (end is None or s[0] < end)]
    t = [s[0] for s in samp]
    t_s = np.array([_sec(x) for x in t])
    dt = np.diff(t_s, prepend=t_s[0] - 30.0)
    T_in = np.array([s[1] / 10.0 for s in samp])
    RH_in = np.array([float(s[2]) for s in samp])

    lora = _read_lora(lht, ("T_out_C", "RH_out_pct", "lux"))
    T_out, RH_out, lux, stale = _interp_outdoor(t_s, lora)

    d1 = _read_lora(door1, ("doorStatus",))
    d2 = _read_lora(door2, ("doorStatus",))
    door1 = _ffill_events(t_s, d1)
    door2 = _ffill_events(t_s, d2)
    door1_known = t_s <= _sec(d1[-1][0]) if d1 else np.zeros(len(t_s), bool)

    bm = np.array([data.bitmask.at(x, 0) for x in t], dtype=np.int64)
    o = np.zeros((len(t), 3))
    pos = np.zeros((len(t), 3))
    for ch in range(3):
        ev = [(_sec(ts), RELAY_TO_CH[v]) for ts, c, v in data.relay if c == ch and v in RELAY_TO_CH]
        first = int((bm[0] >> (2 * ch)) & 3) if len(bm) else 0
        o[:, ch], pos[:, ch] = _openness(t_s, ev, first, TRAVERSE_S[ch])

    wind = [data.wind.at(x, (0, 0)) for x in t]
    wind_ms = np.array([w[0] / 10.0 for w in wind])
    wind_dir = np.array([float(w[1]) for w in wind])
    wind_valid = np.array([x >= WIND_VALID_FROM for x in t])

    restart = dt > MAX_SAMPLE_GAP_S
    if len(restart):
        restart[0] = True

    return Dataset(
        t=t, t_s=t_s, dt=np.minimum(dt, MAX_SAMPLE_GAP_S), T_in=T_in, RH_in=RH_in,
        AH_in=ah_from_rh(RH_in, T_in), T_out=T_out, RH_out=RH_out,
        AH_out=ah_from_rh(RH_out, T_out), lux=lux, stale=stale,
        door1=door1, door2=door2, door1_known=door1_known, o=o, pos=pos, bm=bm,
        wind_ms=wind_ms, wind_dir=wind_dir, wind_valid=wind_valid, restart=restart, log=data)
