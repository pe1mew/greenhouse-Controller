"""humidity_audit.py -- what the humidity branch asks for, and what it gets.

Drives `drivers/ventModel`'s stepped law over every SENSOR_HR sample in the
campaign logs with 5C88's own settings, and counts what the two branches
demanded, what the conflict rule did with it, and what two rules the firmware
does not have -- a close guard on the RH branch, a temperature floor under a
humidity-only open -- would have changed.

It scores the shipped law, not a Python restatement of it: every sample goes
through the compiled library. The Python mirror below exists only so the
variants have something to vary; it is checked against the DLL on every
sample and the run says so. A non-zero mismatch count voids the variants.

Regenerates every table in humidityControl.md:

    python humidity_audit.py                    # both campaigns
    python humidity_audit.py "../campaign-fall-2026/*.log"

Scope. This is the demand layer only: the step the law asks for, before T2's
dwell, T3's wind override, the EG1 inhibit mask or a sensor fault get a say.
Ventilation hours here are what the law wanted, not what the motors did.
"""
import sys
import math
from collections import deque, Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import logdata
import ventmodel as vm
from ventmodel import VentIn, VENT_WIN_CLOSED, VENT_CAP_DIGITAL

# 5C88's settings, from model/campaign-*/config.json -- which are the shipped
# defaults in cfg_defaults.h, unchanged since the unit was commissioned.
CFG = dict(t_max_day=28, t_max_ngt=20, rh_max_day=75, rh_max_ngt=80,
           rh_min_day=50, rh_min_ngt=55, hyst_t=5, hyst_rh=12,
           t_min_day=16, t_min_ngt=14, avg_win_t=6, avg_win_rh=10)

NONE = -1                  # VENT_STEP_NONE: the branch abstains
MAXSTEP = 3
GAP_CAP_S = 120            # a sample stands for at most 2 min, so a log gap
                           # is not credited as measured time
GUARDS = (3, 4, 5, 12)     # candidate close-guard widths, % RH


# --------------------------------------------------------------------------
# The mirror: the same arithmetic in Python, so the variants can differ from
# it. Checked against the DLL sample by sample -- never trusted on its own.
# --------------------------------------------------------------------------

def whole(x):
    """T5 rounds to whole units the way lroundf() does -- not Python's
    round(), which is banker's and would put 30.5 at 30."""
    return int(math.floor(x + 0.5))


def ladder(dev, hyst, cur):
    """step_from_deviation(), vent_model_stepped.cpp:128."""
    w = max(hyst // MAXSTEP, 1)
    raw = 0 if dev <= 0 else (dev + w - 1) // w
    raw = max(0, min(MAXSTEP, raw))
    if cur > 0 and raw == 0 and dev > -hyst:
        return 1                      # the close guard: hold M1, not the step
    return raw


def rh_branch(rh, rh_max, rh_min, hyst_rh, cur, guard=0):
    """vent_step_required_rh(), plus a switch the firmware does not have.

    `guard` is a close hysteresis in % RH below rh_max. At guard=0 this is the
    shipped branch exactly: the ladder is entered only above rh_max, so the
    guard inside ladder() is unreachable and the vote lapses the moment RH is
    back at the ceiling.
    """
    if rh > rh_max:
        return ladder(rh - rh_max, hyst_rh, cur if cur != NONE else 0)
    if guard and cur is not None and cur > 0 and rh > rh_max - guard:
        return 1
    if rh < rh_min:
        return 0                      # a close demand, not an abstention
    return NONE


def resolve(st, srh, prio):
    """vent_resolve_conflict(), vent_model_stepped.cpp:213."""
    if srh == NONE:
        return st
    if st > 0 and srh > 0:
        return max(st, srh)
    if st == srh:
        return st
    return st if prio == 0 else (srh if prio == 1 else max(st, srh))


class Avg:
    """T5's rolling time-window mean over the raw samples."""

    def __init__(self, minutes):
        self.win = minutes * 60
        self.q = deque()
        self.sum = 0.0

    def push(self, ts, v):
        self.q.append((ts, v))
        self.sum += v
        while len(self.q) > 1 and (ts - self.q[0][0]).total_seconds() > self.win:
            self.sum -= self.q.popleft()[1]
        return self.sum / len(self.q)


def fresh_in():
    """A vent_in_t with the windows shut: the demand layer does not depend on
    them, but the law reads the array, so it must be a defined one."""
    v = VentIn()
    v.t_valid = True
    v.rh_valid = True
    v.rh_ctrl_en = True
    for i in range(3):
        v.win[i].state = VENT_WIN_CLOSED
        v.win[i].cap = VENT_CAP_DIGITAL
        v.win[i].pos_x10 = -1
        v.win[i].last_target_x10 = -1
        v.win[i].ms_since_move = 0xFFFFFFFF
    return v


# --------------------------------------------------------------------------
# The run
# --------------------------------------------------------------------------

def run(patterns):
    """One pass through the law. Returns (rows, weights, log, mismatches).

    rows: (ts, t_whole, rh_whole, step_t, step_rh, step, step_at_prio2,
           daytime, t_floor, {guard: rh vote})
    """
    log = logdata.load_sd_logs(patterns)
    law = vm.VentModel("stepped")
    vin = fresh_in()
    at, arh = Avg(CFG["avg_win_t"]), Avg(CFG["avg_win_rh"])

    prev_rh = dict((g, NONE) for g in (0,) + GUARDS)
    prev_t = NONE
    rows, weights, mismatches = [], [], 0
    samples = log.samples

    for i, (ts, t10, rh_raw) in enumerate(samples):
        t_avg = at.push(ts, t10 / 10.0)
        rh_avg = arh.push(ts, rh_raw)
        day = log.is_day(ts)
        t_max = CFG["t_max_day"] if day else CFG["t_max_ngt"]
        rh_max = CFG["rh_max_day"] if day else CFG["rh_max_ngt"]
        rh_min = CFG["rh_min_day"] if day else CFG["rh_min_ngt"]
        t_floor = CFG["t_min_day"] if day else CFG["t_min_ngt"]
        t_w, rh_w = whole(t_avg), whole(rh_avg)

        vin.t_avg_c = t_w
        vin.t_avg_c10 = int(round(t_avg * 10))
        vin.t_c10 = t10
        vin.rh_pct = max(0, min(255, rh_raw))
        vin.rh_avg_pct = max(0, min(255, rh_w))
        vin.daytime = day
        vin.t_max_c10 = t_max * 10
        vin.rh_max_pct = rh_max
        vin.rh_min_pct = rh_min
        vin.hyst_t_c = CFG["hyst_t"]
        vin.hyst_rh_pct = CFG["hyst_rh"]
        vin.cr_priority = 0
        out = law.step(vin)
        d_step, d_t, d_rh = out.step, out.step_t, out.step_rh

        m_t = ladder(t_w - t_max, CFG["hyst_t"], prev_t if prev_t != NONE else 0)
        m_rh = rh_branch(rh_w, rh_max, rh_min, CFG["hyst_rh"], prev_rh[0])
        if (m_t, m_rh, resolve(m_t, m_rh, 0)) != (d_t, d_rh, d_step):
            mismatches += 1
        prev_t = m_t
        for g in prev_rh:
            prev_rh[g] = rh_branch(rh_w, rh_max, rh_min, CFG["hyst_rh"],
                                   prev_rh[g], guard=g)

        dt = GAP_CAP_S
        if i + 1 < len(samples):
            dt = min(GAP_CAP_S, (samples[i + 1][0] - ts).total_seconds())
        weights.append(max(0.0, dt) / 3600.0)
        rows.append((ts, t_w, rh_w, d_t, d_rh, d_step, resolve(m_t, m_rh, 2),
                     day, t_floor, dict(prev_rh), t_avg, rh_avg))

    return rows, weights, log, mismatches


# --------------------------------------------------------------------------
# Would venting have dried the house? Needs the outdoor LoRa sensor.
# --------------------------------------------------------------------------

def _magnus(t_c, rh_pct):
    """(absolute humidity g/m3, dewpoint degC) from T and RH."""
    rh = max(1.0, min(100.0, rh_pct))
    es = 6.112 * math.exp(17.62 * t_c / (243.12 + t_c))       # hPa
    e = es * rh / 100.0
    ah = 216.7 * e / (273.15 + t_c)
    alpha = math.log(rh / 100.0) + 17.62 * t_c / (243.12 + t_c)
    return ah, 243.12 * alpha / (17.62 - alpha)


def outdoor(rows, w, days, path, stale_s=1800):
    """The indoor/outdoor gradient during the refused hours.

    The LoRa export is stamped UTC and the SD log is local time -- the join
    that the 2026-09-18 erratum caught. lora_time.utc_to_local() is the one
    conversion in the repo, so it is the one used here.
    """
    import csv
    sys.path.insert(0, str(HERE.parent))
    from lora_time import utc_to_local
    from datetime import datetime, timedelta

    out = []
    with open(path, newline="", encoding="utf-8") as fh:
        for r in csv.DictReader(fh):
            try:
                ts = utc_to_local(datetime.strptime(r["dateTime"], "%Y-%m-%d %H:%M:%S"))
                out.append((ts, float(r["T_out_C"]), float(r["RH_out_pct"])))
            except (ValueError, KeyError, TypeError):
                continue
    out.sort()
    print("")
    print("-- would venting have dried the house? outdoor lht65-20, %d rows, %s .. %s --"
          % (len(out), out[0][0], out[-1][0]))

    acc = Counter()
    grad, dtemp, ddew = [], [], []
    j = 0
    for i, r in enumerate(rows):
        ts, _, _, d_t, d_rh, _, _, day, _, _, t_avg, rh_avg = r
        if not (d_rh >= 1 and d_t <= 0):
            continue                                   # refused hours only
        acc["refused"] += w[i]
        while j + 1 < len(out) and out[j + 1][0] <= ts:
            j += 1
        while j > 0 and out[j][0] > ts:
            j -= 1
        if out[j][0] > ts or (ts - out[j][0]) > timedelta(seconds=stale_s):
            acc["unmatched"] += w[i]
            continue
        _, t_out, rh_out = out[j]
        ah_in, td_in = _magnus(t_avg, rh_avg)
        ah_out, td_out = _magnus(t_out, rh_out)
        part = "day" if day else "night"
        acc["matched"] += w[i]
        acc["matched_" + part] += w[i]
        if ah_out < ah_in:
            acc["drier_out"] += w[i]
            acc["drier_out_" + part] += w[i]
        if td_out < td_in:
            acc["dew_lower"] += w[i]
        grad.append(ah_in - ah_out)
        dtemp.append(t_avg - t_out)
        ddew.append(td_in - td_out)

    def med(v):
        v = sorted(v)
        return v[len(v) // 2] if v else float("nan")

    m = acc["matched"]
    if not m:
        print("  no overlap between the logs and the outdoor export")
        return
    print("  refused hours %.1f h, matched %.1f h (%.0f %%), unmatched %.1f h "
          "(outside the export or stale)"
          % (acc["refused"], m, 100.0 * m / acc["refused"], acc["unmatched"]))
    print("  outside air holds LESS water   %5.1f %% of matched hours "
          "(day %4.1f %%, night %4.1f %%)"
          % (100.0 * acc["drier_out"] / m,
             100.0 * acc["drier_out_day"] / max(acc["matched_day"], 1e-9),
             100.0 * acc["drier_out_night"] / max(acc["matched_night"], 1e-9)))
    print("  outside dewpoint is lower      %5.1f %% of matched hours"
          % (100.0 * acc["dew_lower"] / m))
    print("  median AH_in - AH_out          %6.2f g/m3" % med(grad))
    print("  median dewpoint drop offered   %6.2f K" % med(ddew))
    print("  median T_in - T_out (paid)     %6.2f K" % med(dtemp))


# --------------------------------------------------------------------------
# The tables
# --------------------------------------------------------------------------

def header(rows, log, mismatches):
    print("files %d   samples %d   %s .. %s"
          % (len(log.files), len(rows), rows[0][0], rows[-1][0]))
    print("climate SETPT rows in the period: %d"
          % len([r for r in log.setpts if 1 <= r[4] <= 11]))
    print("mirror mismatches against the compiled law: %d  (a non-zero count "
          "voids every variant below)" % mismatches)


def distribution(rows):
    print("")
    print("-- RH as measured (10-min average, whole %) --")
    for name, want in (("day", True), ("night", False)):
        v = sorted(r[2] for r in rows if r[7] is want)
        if not v:
            continue
        q = lambda p: v[int(p * (len(v) - 1))]
        print("  %-6s n=%6d   p10 %2d   median %2d   p90 %2d   at/above 90 %%: %4.1f %%"
              % (name, len(v), q(.10), q(.50), q(.90),
                 100.0 * sum(1 for x in v if x >= 90) / len(v)))


def demand(rows, w, days, total):
    h = Counter()
    for i, r in enumerate(rows):
        t_w, d_t, d_rh, day, t_floor = r[1], r[3], r[4], r[7], r[8]
        part = "day" if day else "night"
        if d_rh >= 1:
            h["rh_high"] += w[i]
            h["rh_high_" + part] += w[i]
            if d_t <= 0:
                h["refused"] += w[i]
                h["refused_" + part] += w[i]
                h["refused_step%d" % d_rh] += w[i]
                if t_w >= t_floor:
                    h["refused_above_floor"] += w[i]
                    h["refused_above_floor_" + part] += w[i]
                else:
                    h["refused_below_floor"] += w[i]
            elif d_rh > d_t:
                h["raised_step"] += w[i]
        if d_rh == 0 and d_t > 0:
            h["dry_vs_heat"] += w[i]
            h["dry_vs_heat_" + part] += w[i]
        if d_t > 0:
            h["t_vents"] += w[i]
            h["t_vents_step%d" % d_t] += w[i]

    print("")
    print("-- %.1f days (%.0f h) of 5C88, its own settings --" % (days, total))
    def row(label, key):
        print("  %-42s %7.1f h   %5.1f %%   %5.2f h/day"
              % (label, h[key], 100.0 * h[key] / total, h[key] / days))
    row("temperature asks to vent", "t_vents")
    for s in (1, 2, 3):
        row("   at step %d" % s, "t_vents_step%d" % s)
    row("RH above its ceiling", "rh_high")
    row("   by day", "rh_high_day")
    row("   at night", "rh_high_night")
    row("humidity asks to vent, temperature idle (REFUSED)", "refused")
    row("   by day", "refused_day")
    row("   at night", "refused_night")
    row("   asking step 1 (M1)", "refused_step1")
    row("   asking step 2 (M1+M2)", "refused_step2")
    row("   asking step 3 (everything)", "refused_step3")
    row("   at or above t_min", "refused_above_floor")
    row("      by day (the narrowest useful authority)", "refused_above_floor_day")
    row("      at night", "refused_above_floor_night")
    row("   below t_min (a floor would refuse these too)", "refused_below_floor")
    row("what humidity does today: raises a live step", "raised_step")
    row("dry house while temperature wants to vent", "dry_vs_heat")
    row("   by day", "dry_vs_heat_day")
    row("   at night", "dry_vs_heat_night")
    return h


def temperature_bands(rows, w, days):
    print("")
    print("-- the temperature during the refused demand --")
    bands = (("below 10", -99, 10), ("10-13", 10, 14), ("14-15", 14, 16),
             ("16-19", 16, 20), ("20-23", 20, 24), ("24 and up", 24, 99))
    acc = Counter()
    for i, r in enumerate(rows):
        if r[4] >= 1 and r[3] <= 0:
            for name, lo, hi in bands:
                if lo <= r[1] < hi:
                    acc[name] += w[i]
                    break
    for name, _, _ in bands:
        print("  T_avg %-10s %7.1f h   %5.2f h/day" % (name, acc[name], acc[name] / days))


def monthly(rows, w):
    print("")
    print("-- month by month: refused demand at or above t_min --")
    per, span = Counter(), Counter()
    for i, r in enumerate(rows):
        m = r[0].strftime("%Y-%m")
        span[m] += w[i]
        if r[4] >= 1 and r[3] <= 0 and r[1] >= r[8]:
            per[m] += w[i]
    for m in sorted(span):
        d = span[m] / 24.0
        print("  %s   %5.1f days   %6.1f h   %5.2f h/day"
              % (m, d, per[m], per[m] / d if d else 0.0))


def guards(rows, w, days):
    print("")
    print("-- a close guard on the RH branch, which the law does not have --")
    print("  %-9s %9s %9s %11s %11s" % ("guard", "flips", "per day",
                                        "vote open", "held below"))
    for g in (0,) + GUARDS:
        flips, openh, held, prev = 0, 0.0, 0.0, None
        for i, r in enumerate(rows):
            day = r[7]
            rh_max = CFG["rh_max_day"] if day else CFG["rh_max_ngt"]
            v = r[9][g] >= 1
            if v:
                openh += w[i]
                if r[2] <= rh_max:
                    held += w[i]
            if prev is not None and v != prev:
                flips += 1
            prev = v
        label = "shipped" if g == 0 else "%d %%" % g
        print("  %-9s %9d %9.2f %9.1f h %9.1f h" % (label, flips, flips / days,
                                                    openh, held))


def main(patterns, outdoor_csv=None):
    rows, w, log, mismatches = run(patterns)
    total = sum(w)
    days = total / 24.0
    header(rows, log, mismatches)
    distribution(rows)
    demand(rows, w, days, total)
    temperature_bands(rows, w, days)
    monthly(rows, w)
    guards(rows, w, days)
    if outdoor_csv and Path(outdoor_csv).exists():
        outdoor(rows, w, days, outdoor_csv)


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--outdoor=")]
    csvs = [a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--outdoor=")]
    if not args:
        args = [str(HERE.parent / "campaign-summer-2026" / "*.log"),
                str(HERE.parent / "campaign-fall-2026" / "*.log")]
    default_csv = HERE.parent / "campaign-summer-2026" / "lht65_20_2026-06-04_2026-09-17.csv"
    main(args, csvs[0] if csvs else (str(default_csv) if default_csv.exists() else None))
