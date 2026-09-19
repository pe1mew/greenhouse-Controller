"""
settings.py -- the controller's settings, and where the simulator applies each one.

Every key the controller stores is here: the rows of
firmware/config/cfg_desc.inc (53 as of 2.10.0), plus tz_str, a string and so
outside the descriptor. Keys, bounds, defaults and audit ids are read from the
firmware's own sources through the pre-commit checker's parser
(bin/check_cfg_desc.py). Nothing is restated here, so nothing can drift: a key
this module does not classify stops the simulator.

EFFECT says what the simulator does with each key. It is what the firmware
does with it, in the same place (design/ventModelContract.md §3a):

  law    passed to the control law, resolved for day or night (T6's caller)
  T5     an averaging window: the law receives only the average
  T3     wind safety: it closes every window and suspends the law
  T2     motor travel and dwell, enforced around the law
  M3     M3's wire sensor: with wpos_fitted_m3 = 1, M3 is linear (firmware.
         LinearChannel), and in mode 2 it takes targets, stopped and dropped
         within deadzone_m3. Observe-only in the firmware through 2.10.0
  T4     the site's coordinates: sunrise, sunset, and so day or night
  poll   the poll interval: when T5 samples and T6 decides
  clock  tz_str: no effect on control, since T4 decides day and night in UTC;
         the simulator uses it to read the log's local stamps
  none   no effect on anything the simulator models; the note says why

Where the values come from, later wins:
  1. the descriptor's defaults (cfg_defaults.h), then a unit's own base.
     5C88's is below, with the evidence for each value.
  2. the unit's SETPT audit rows: every change, with its time, so the
     settings in force follow the log.
  3. --config: a unit's GET /api/config JSON, which replaces 1 and 2.
  4. --set KEY=VALUE: wins over everything, at every moment.
Every value is clamped to its descriptor bounds, as T4 does on a write.
"""

from __future__ import annotations

import bisect
import glob
import json
import re
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
APP_TYPES_H = REPO / "firmware" / "src" / "types" / "app_types.h"
DEFAULTS_H = REPO / "firmware" / "config" / "cfg_defaults.h"

sys.path.insert(0, str(REPO / "bin"))
import check_cfg_desc as _cd  # noqa: E402   the pre-commit hook's descriptor parser


# --------------------------------------------------------------------------
# The keys, from the firmware's sources
# --------------------------------------------------------------------------

@dataclass(frozen=True)
class Key:
    name: str
    ns: str          # climate, wind, motor, wifi, system
    lo: int
    hi: int
    default: int
    param: int       # the LOG_PARAM_* number its SETPT rows carry; 0 = not audited
    channel: int     # the motor, 1..3, for the per-window keys; else 0


def _load_keys():
    rows = _cd.parse_desc(_cd.macro_values(), _cd.key_constants())
    defaults = _cd.default_values()
    problems = _cd._FATAL + _cd._ERRORS
    if problems:
        raise RuntimeError("firmware/config/cfg_desc.inc did not parse:\n  "
                           + "\n  ".join(problems))
    src = APP_TYPES_H.read_text(encoding="utf-8", errors="replace")
    params = {k: int(v) for k, v in re.findall(r"\b(LOG_PARAM_[A-Z0-9_]+)\s*=\s*(\d+)", src)}
    keys = {}
    for r in rows:
        tok = r["def_tok"]
        default = int(tok) if re.fullmatch(r"-?\d+", tok) else defaults[tok]
        keys[r["key"]] = Key(r["key"], r["ns"].replace("NVS_NS_", "").lower(), r["min"],
                             r["max"], default, params.get(r["param"], 0), r["channel"])
    return keys


KEYS = _load_keys()

DEF_TZ_STR = re.search(r'#define\s+DEF_TZ_STR\s+"([^"]+)"',
                       DEFAULTS_H.read_text(encoding="utf-8")).group(1)


# --------------------------------------------------------------------------
# Where each key takes effect
# --------------------------------------------------------------------------

_WIN = ("travel_m%d", "dwell_open_m%d", "dwell_close_m%d")

EFFECT = {
    "t_max_day":    ("law", "t_max_c10 by day"),
    "t_max_ngt":    ("law", "t_max_c10 by night"),
    "rh_max_day":   ("law", "rh_max_pct by day"),
    "rh_max_ngt":   ("law", "rh_max_pct by night"),
    "rh_min_day":   ("law", "rh_min_pct by day"),
    "rh_min_ngt":   ("law", "rh_min_pct by night"),
    "hyst_t":       ("law", "hyst_t_c, floored at 1"),
    "hyst_rh":      ("law", "hyst_rh_pct, floored at 1"),
    "cr_priority":  ("law", "which demand wins"),
    "rh_ctrl_en":   ("law", "off: humidity casts no vote"),
    "avg_win_t":    ("T5", "the law receives t_avg_*, never the window"),
    "avg_win_rh":   ("T5", "the law receives rh_avg_pct"),
    "avg_win_wind": ("T5", "the wind averages T3 judges and the law receives; "
                           "before 2.1.0 wind used avg_win_t"),
    "v_max":        ("T3", "set at avg >= v_max"),
    "wind_hyst":    ("T3", "clear below v_max - wind_hyst (2.3.0+)"),
    "dir_excl_low": ("T3", "direction exclusion arc; low == high disables it"),
    "dir_excl_high": ("T3", "direction exclusion arc"),
    "wind_prot_en": ("T3", "off: no wind override at all"),
    "lat_deg":      ("T4", "sunrise.cpp -> is_daytime -> the day or night set"),
    "lat_frac":     ("T4", "latitude = lat_deg + lat_frac/1000, as T4 adds them"),
    "lon_deg":      ("T4", "sunrise.cpp -> is_daytime"),
    "lon_frac":     ("T4", "longitude = lon_deg + lon_frac/1000"),
    "poll_interval": ("poll", "T5 samples and T6 decides every poll; each window "
                              "holds minutes*60/poll samples"),
    "t_min_day":    ("none", "T6 never reads it: a heating setpoint, and there is no "
                             "heating (contract §3a); LCD and GUI only"),
    "t_min_ngt":    ("none", "as t_min_day"),
    "deadzone_m3":  ("M3", "with a linear M3: m3_deadzone_x10 (over --m3-span-mm); T6 "
                           "drops a target this close to M3 at rest, T2 stops this close"),
    "wpos_fitted_m3": ("M3", "1: M3 linear, the law gets cap, pos and age; with a law "
                             "other than stepped, mode 2 (targets)"),
    "ap_enable":    ("none", "the WiFi access point"),
    "ap_timeout":   ("none", "the WiFi access point"),
    "session_timeout": ("none", "admin sessions are replayed from the log as they happened"),
    "led_day_brt":  ("none", "the status LED"),
    "led_nite_brt": ("none", "the status LED"),
    "led_nite_from": ("none", "the status LED"),
    "led_nite_to":  ("none", "the status LED"),
    "log_upload_h": ("none", "the SD log upload"),
    "log_upload_m": ("none", "the SD log upload"),
    "log_upload_rot": ("none", "the SD log upload"),
    "ota_check_h":  ("none", "remote updates; their reboots are replayed from the log"),
    "ota_enable":   ("none", "remote updates"),
    "ota_win_hi":   ("none", "remote updates"),
    "ota_win_lo":   ("none", "remote updates"),
    "status_enable": ("none", "the status post"),
    "status_expose": ("none", "the status post"),
    "status_intv_s": ("none", "the status post"),
    "tz_str":       ("clock", "no effect on control: T4 decides day and night in UTC. "
                              "The simulator reads the log's local stamps with it"),
}
for _i in (1, 2, 3):
    EFFECT[_WIN[0] % _i] = ("T2", "relay time = travel + the fixed 5 s margin")
    EFFECT[_WIN[1] % _i] = ("T2", "defers T6 only; T3 and the operator bypass it")
    EFFECT[_WIN[2] % _i] = ("T2", "defers T6 only")

_unclassified = sorted(set(KEYS) - set(EFFECT))
_stale = sorted(set(EFFECT) - set(KEYS) - {"tz_str"})
if _unclassified or _stale:
    raise RuntimeError("settings.py is out of step with cfg_desc.inc: unclassified %s, "
                       "no longer in the descriptor %s. Decide where each new key takes "
                       "effect before simulating with it." % (_unclassified, _stale))


# --------------------------------------------------------------------------
# Values: defaults, a unit's GET /api/config, --set
# --------------------------------------------------------------------------

def defaults():
    """Every key at its descriptor default, plus tz_str."""
    v = {k: key.default for k, key in KEYS.items()}
    v["tz_str"] = DEF_TZ_STR
    return v


# GET /api/config field -> key (web_server.cpp config_get_handler())
_API_RENAMED = {"deadzone_m3_mm": "deadzone_m3", "poll_interval_s": "poll_interval",
                "session_timeout_min": "session_timeout", "ap_timeout_min": "ap_timeout"}
_API_ARRAYS = {"travel_s": _WIN[0], "dwell_open_s": _WIN[1], "dwell_close_s": _WIN[2]}
# Before 2.7.0 (gh#63) the dwell arrays were also sent as *_min -- carrying SECONDS.
_API_LEGACY = {"dwell_open_min": ("dwell_open_s", _WIN[1]),
               "dwell_close_min": ("dwell_close_s", _WIN[2])}
_API_INFO = ("wifi_ssid", "ap_ssid", "fw_ver")


def load_api_config(path):
    """A unit's GET /api/config JSON -> ({key: value}, notes).

    Fails on a field it does not know: that is a firmware newer than this
    module, and the new setting must be classified first.
    """
    raw = json.loads(Path(path).read_text(encoding="utf-8"))
    vals, notes = {}, []
    for field, v in raw.items():
        if field in _API_ARRAYS:
            for i, x in enumerate(v, 1):
                vals[_API_ARRAYS[field] % i] = int(x)
        elif field in _API_LEGACY:
            newer, pattern = _API_LEGACY[field]
            if newer in raw:
                continue
            for i, x in enumerate(v, 1):
                vals[pattern % i] = int(x)
            notes.append("%s read as SECONDS: the name says minutes, the value never "
                         "was (gh#63, dropped in 2.7.0)" % field)
        elif field in _API_RENAMED:
            vals[_API_RENAMED[field]] = int(v)
        elif field in KEYS:
            vals[field] = int(v)
        elif field == "tz_str":
            vals["tz_str"] = str(v)
        elif field in _API_INFO:
            notes.append("%s = %s" % (field, v))
        else:
            raise ValueError("%s: unknown field %r -- a firmware newer than settings.py?"
                             % (path, field))
    return vals, notes


def parse_set(items):
    """--set KEY=VALUE items -> {key: value}. GET /api/config names work too."""
    out = {}
    for item in items or ():
        k, sep, v = item.partition("=")
        k = _API_RENAMED.get(k.strip(), k.strip())
        if not sep:
            raise ValueError("--set %s: expected KEY=VALUE" % item)
        if k == "tz_str":
            out[k] = v.strip()
        elif k in KEYS:
            out[k] = int(v)
        else:
            raise ValueError("--set %s: no such setting (see `closed_loop.py settings`)" % k)
    return out


def clamp(vals):
    """Clamp every value to its descriptor bounds, as T4 does. -> (vals, notes)."""
    out, notes = dict(vals), []
    for k, v in vals.items():
        key = KEYS.get(k)
        if key is None:
            continue
        c = min(max(int(v), key.lo), key.hi)
        if c != v:
            notes.append("%s %d clamped to %d (%d..%d), as T4 does" % (k, v, c, key.lo, key.hi))
        out[k] = c
    return out, notes


def to_sim(v):
    """{key: value}, every key present -> firmware.Settings."""
    from firmware import Settings
    return Settings(
        t_max_day=v["t_max_day"], t_max_ngt=v["t_max_ngt"],
        rh_max_day=v["rh_max_day"], rh_min_day=v["rh_min_day"],
        rh_max_ngt=v["rh_max_ngt"], rh_min_ngt=v["rh_min_ngt"],
        hyst_t=v["hyst_t"], hyst_rh=v["hyst_rh"], rh_ctrl_en=bool(v["rh_ctrl_en"]),
        cr_priority=v["cr_priority"],
        avg_win_t=v["avg_win_t"], avg_win_rh=v["avg_win_rh"], avg_win_wind=v["avg_win_wind"],
        poll_s=v["poll_interval"],
        travel_s=tuple(v[_WIN[0] % i] for i in (1, 2, 3)),
        dwell_open_s=tuple(v[_WIN[1] % i] for i in (1, 2, 3)),
        dwell_close_s=tuple(v[_WIN[2] % i] for i in (1, 2, 3)),
        v_max=v["v_max"], wind_hyst=v["wind_hyst"], dir_excl_low=v["dir_excl_low"],
        dir_excl_high=v["dir_excl_high"], wind_prot_en=bool(v["wind_prot_en"]),
        lat_deg=v["lat_deg"], lat_frac=v["lat_frac"], lon_deg=v["lon_deg"],
        lon_frac=v["lon_frac"], tz_str=v["tz_str"],
        t_min_day=v["t_min_day"], t_min_ngt=v["t_min_ngt"],
        deadzone_m3_mm=v["deadzone_m3"], wpos_fitted_m3=bool(v["wpos_fitted_m3"]),
    )


# --------------------------------------------------------------------------
# A unit's history: SETPT rows -> the settings in force
# --------------------------------------------------------------------------

# T10's geolocation posts the four coordinates at every boot, in this order,
# all on LOG_PARAM_LAT_LON with channel 0: a same-second group of four rows
# is decoded by position. A single edited coordinate is matched by its old value.
_LATLON = ("lat_deg", "lat_frac", "lon_deg", "lon_frac")


def changes_from_setpts(setpts, values_before=None):
    """The log's SETPT rows -> ([(ts, key, new)], [(ts, key, old)], notes).

    The second list holds each change's logged OLD value, which says what the
    unit ran before it -- the check on a base built from evidence.
    Rows of string settings (tz_str, WiFi, PINs, URLs) carry no value and are
    skipped, as are keys with no effect that are not in the descriptor.
    """
    by_param = {}
    for key in KEYS.values():
        if key.param:
            by_param.setdefault(key.param, []).append(key)
    cur = dict(values_before or {})
    changes, olds, notes = [], [], []
    rows = list(setpts)
    i = 0
    while i < len(rows):
        ts, _seq, _ini, ch, par, old, new = rows[i]
        group = [rows[i]]
        if par == 21:
            j = i + 1
            while j < len(rows) and rows[j][0] == ts and rows[j][4] == 21:
                group.append(rows[j])
                j += 1
            if len(group) == 4:
                for name, r in zip(_LATLON, group):
                    changes.append((ts, name, r[6]))
                    olds.append((ts, name, r[5]))
                    cur[name] = r[6]
                i = j
                continue
            group = [rows[i]]
        cands = by_param.get(par, [])
        if len(cands) > 1:
            cands = [k for k in cands if k.channel == ch] or cands
        if len(cands) > 1 and par == 21:
            cands = [k for k in cands if cur.get(k.name) == old] or cands
        if len(cands) == 1:
            name = cands[0].name
            changes.append((ts, name, new))
            olds.append((ts, name, old))
            cur[name] = new
        elif cands:
            notes.append("%s: SETPT param %d ch %d matches %s; skipped"
                         % (ts, par, ch, [k.name for k in cands]))
        i += 1
    return changes, olds, notes


class Schedule:
    """The settings in force over time: a base, dated changes, and overrides.

    at(ts) -> firmware.Settings; values_at(ts) -> {key: value}. Overrides win at
    every moment, so a what-if (--set) is not undone by a logged change.
    """

    def __init__(self, base, changes=(), overrides=None, label="", notes=()):
        self.label = label
        self.base = dict(base)
        self.changes = sorted(changes, key=lambda c: c[0])
        self.overrides = dict(overrides or {})
        self.notes = list(notes)
        self._ts = [c[0] for c in self.changes]
        self._cache = {}

    def _segment(self, i):
        if i not in self._cache:
            v = dict(self.base)
            for _, k, x in self.changes[:i]:
                v[k] = x
            v.update(self.overrides)
            v, _ = clamp(v)
            self._cache[i] = (v, to_sim(v))
        return self._cache[i]

    def values_at(self, ts):
        return self._segment(bisect.bisect_right(self._ts, ts))[0]

    def at(self, ts):
        return self._segment(bisect.bisect_right(self._ts, ts))[1]

    def with_overrides(self, overrides):
        o = dict(self.overrides)
        o.update(overrides)
        return Schedule(self.base, self.changes, o, self.label, self.notes)


# What 5C88 ran before its first logged change, summer 2026. Every other key
# is the descriptor's default, and every later change is in its SETPT rows.
BASE_5C88 = {
    # vent_step_replay.py and gate-control reproduce 97-98 % of the logged
    # T-demands with 3, and 64 % with the default 6. 3 was the default before
    # v1.16.23, and 5C88 kept it (gotcha-log 2026-07-28).
    "avg_win_t": 3,
    # The default. UNCONFIRMED: the summer's logged RH-demands reproduce
    # 92.6-93.0 % with 5 to 7 and 88.3 % with 10 (gate-control --sweep), and 5
    # was the default before 1.16.31 -- 5C88 may have kept that too
    # (closedloop/README.md, "Settings").
    "avg_win_rh": 10,
    # t_max 28/20 and hyst_t 5 are defaults; the replay above confirms them.
    # v_max 6 is the default, and the logged wind ALARM rows carry 6 x 10.
}


def _campaign_setpts():
    """Every SETPT row in the campaign's SD logs, deduplicated, in file order."""
    from logdata import CAMPAIGN
    seen, rows = set(), []
    for path in sorted(glob.glob(str(CAMPAIGN / "*.log"))):
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if ",SETPT," not in line:
                    continue
                p = line.strip().split(",")
                try:
                    r = (datetime.strptime(p[0], "%Y-%m-%dT%H:%M:%S"), p[2], int(p[3]),
                         int(p[4]), int(p[5]), int(p[6]))
                except (ValueError, IndexError):
                    continue
                if r not in seen:
                    seen.add(r)
                    rows.append((r[0], len(rows), r[1], r[2], r[3], r[4], r[5]))
    return sorted(rows, key=lambda r: (r[0], r[1]))


_SCHEDULE_5C88 = None


def schedule_5c88():
    """5C88's settings over the campaign: BASE_5C88 on the defaults, then its log."""
    global _SCHEDULE_5C88
    if _SCHEDULE_5C88 is None:
        base = defaults()
        base.update(BASE_5C88)
        changes, olds, notes = changes_from_setpts(_campaign_setpts(), base)
        # A key's first logged OLD value is what the unit ran before it: it
        # wins over the base, and a disagreement is reported.
        first = {}
        for ts, k, old in olds:
            first.setdefault(k, (ts, old))
        for k, (ts, old) in sorted(first.items()):
            if base.get(k) != old:
                notes.append("%s: base %s, but its first SETPT row (%s) says the unit ran %s "
                             "-- the log wins" % (k, base.get(k), ts, old))
                base[k] = old
        _SCHEDULE_5C88 = Schedule(base, changes, label="5C88 (base + its SETPT rows)",
                                  notes=notes)
    return _SCHEDULE_5C88


def schedule_from_config(path):
    """One unit's GET /api/config, constant in time, on the defaults."""
    base = defaults()
    vals, notes = load_api_config(path)
    base.update(vals)
    missing = sorted(set(KEYS) - set(vals))
    if missing:
        notes.append("not in the file, so at their defaults: %s" % ", ".join(missing))
    return Schedule(base, label="config %s" % Path(path).name, notes=notes)


# --------------------------------------------------------------------------
# tz_str: the POSIX TZ rules the controller's clock runs on
# --------------------------------------------------------------------------

class PosixTZ:
    """A POSIX TZ string of the form ESP-IDF uses, e.g. DEF_TZ_STR
    "CET-1CEST,M3.5.0,M10.5.0/3": offsets west of UTC, DST by Mm.w.d rules.

    local_to_utc() reads a local stamp as the controller wrote it; the hour
    that repeats in autumn is read as summer time.
    """

    _RE = re.compile(r"^(<[^>]+>|[A-Za-z]{3,})([-+]?\d{1,2}(?::\d{2}){0,2})"
                     r"(?:(<[^>]+>|[A-Za-z]{3,})([-+]?\d{1,2}(?::\d{2}){0,2})?"
                     r",M(\d+)\.(\d)\.(\d)(?:/([-+]?\d+(?::\d{2}){0,2}))?"
                     r",M(\d+)\.(\d)\.(\d)(?:/([-+]?\d+(?::\d{2}){0,2}))?)?$")

    def __init__(self, s):
        m = self._RE.match(s.strip())
        if not m:
            raise ValueError("tz_str %r: only STDoff[DST[off],Mm.w.d[/t],Mm.w.d[/t]] "
                             "is supported" % s)
        g = m.groups()
        self.std = -self._secs(g[1])                 # UTC offset in standard time, s
        self.has_dst = g[2] is not None
        if self.has_dst:
            self.dst = -self._secs(g[3]) if g[3] else self.std + 3600
            self.start = (int(g[4]), int(g[5]), int(g[6]), self._secs(g[7]) if g[7] else 7200)
            self.end = (int(g[8]), int(g[9]), int(g[10]), self._secs(g[11]) if g[11] else 7200)

    @staticmethod
    def _secs(t):
        sign = -1 if t.startswith("-") else 1
        parts = [int(x) for x in t.lstrip("+-").split(":")]
        parts += [0] * (3 - len(parts))
        return sign * (parts[0] * 3600 + parts[1] * 60 + parts[2])

    @staticmethod
    def _rule_day(year, month, week, wday):
        """Day of the month of the week-th wday (0 = Sunday); week 5 = the last."""
        first = datetime(year, month, 1)
        d = 1 + (wday - (first.weekday() + 1) % 7) % 7
        d += 7 * (week - 1)
        nxt = datetime(year + (month == 12), month % 12 + 1, 1)
        while datetime(year, month, 1) + timedelta(days=d - 1) >= nxt:
            d -= 7
        return d

    def utcoffset(self, local):
        """UTC offset in seconds for a naive local time."""
        if not self.has_dst:
            return self.std
        y = local.year
        m1, w1, d1, t1 = self.start
        m2, w2, d2, t2 = self.end
        on = datetime(y, m1, self._rule_day(y, m1, w1, d1)) + timedelta(seconds=t1)
        # the end rule is given in summer time; fold the repeated hour into it
        off = datetime(y, m2, self._rule_day(y, m2, w2, d2)) + timedelta(seconds=t2)
        dst = on <= local < off if on < off else not (off <= local < on)
        return self.dst if dst else self.std

    def local_to_utc(self, local):
        return local - timedelta(seconds=self.utcoffset(local))


_TZ_CACHE = {}


def tz(s):
    if s not in _TZ_CACHE:
        _TZ_CACHE[s] = PosixTZ(s)
    return _TZ_CACHE[s]


# --------------------------------------------------------------------------
# A table for people
# --------------------------------------------------------------------------

def describe(sched, ts=None):
    """Lines: every setting, its value, bounds, default, and where it acts."""
    ts = ts or (sched.changes[-1][0] if sched.changes else datetime.max)
    v = sched.values_at(ts)
    dflt = defaults()
    out = ["  %-16s %8s %12s %8s  %-6s %s" % ("key", "value", "bounds", "default", "acts", "how")]
    order = {"law": 0, "T5": 1, "T3": 2, "T2": 3, "M3": 4, "T4": 5, "poll": 6, "clock": 7,
             "none": 8}
    names = sorted(EFFECT, key=lambda k: (order[EFFECT[k][0]], k))
    for k in names:
        where, how = EFFECT[k]
        key = KEYS.get(k)
        mark = "" if v[k] == dflt[k] else " *"
        if k in sched.overrides:
            mark = " set"
        if key is None:                      # tz_str: a string, on its own line
            out.append("  %-16s %8s %12s %8s  %-6s %s%s"
                       % (k, "(below)", "string", "(below)", where, how, mark))
            out.append("  %-16s value %s, default %s" % ("", v[k], dflt[k]))
            continue
        out.append("  %-16s %8s %12s %8s  %-6s %s%s"
                   % (k, v[k], "%d..%d" % (key.lo, key.hi), dflt[k], where, how, mark))
    return out
