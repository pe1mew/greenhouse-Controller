#!/usr/bin/env python3
"""
plot_daily.py — Generate one PNG per UTC day from greenhouse SD logs.

Layout mirrors model/simulation.py's save_plot() four-panel design:
  Panel 1 — Temperature (indoor) + setpoint reference lines, night shading
  Panel 2 — Humidity (indoor) + setpoint reference lines, night shading
  Panel 3 — Wind override windows + spot wind-speed values from ALARM rows
            (Note: continuous wind speed is not logged in rc.1.3.x. The
            campaign firmware (LOG_SENSOR_HR) will add it; this panel reads
            what's available today: ALARM-event spot values + override
            intervals.)
  Panel 4 — Window position per channel (M1/M2/M3) on a state ladder
            CLOSED → MOVING_OPEN → OPEN → MOVING_CLOSE

Inputs:
  - temp/*.csv — SD log files (header: timestamp,type,initiator,ch,param,value_a,value_b)
  - temp/config.json — current controller config (for setpoint lines)

Outputs:
  - temp/plot_YYYY-MM-DD.png  — one per UTC day with ≥ 30 sensor samples
  - temp/plot_summary.txt     — per-day stats and a list of generated files
"""

from __future__ import annotations
import csv
import json
import sys
from collections import defaultdict
from datetime import datetime, timezone, timedelta
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
import matplotlib.gridspec as gridspec


HERE = Path(__file__).resolve().parent
LOG_GLOB = "*.log"


# ─── parse helpers ──────────────────────────────────────────────────────────

def parse_ts(s: str) -> datetime:
    """Parse an SD-log timestamp.

    The firmware writes timestamps with `localtime_r()` (see
    firmware/src/event_logger/event_logger.cpp:496 and the doc block above
    it), so the value in the CSV is **local time** (Europe/Amsterdam,
    typically CEST in May), NOT UTC. We return a naive datetime; downstream
    code treats every event as already being in the controller's local TZ.
    """
    return datetime.strptime(s, "%Y-%m-%dT%H:%M:%S")


# Per firmware/src/relay_controller/relay_controller.cpp:137 (ch_state_t).
# T2 logs the *internal* per-channel state, which extends window_state_t
# with two transient GAP states between travel direction changes.
RELAY_STATE_NAME = {
    0: "UNKNOWN",
    1: "CLOSED",
    2: "MOVING_OPEN",
    3: "OPEN",
    4: "MOVING_CLOSE",
    5: "GAP_TO_OPEN",     # transient (~2 s) — treat visually like MOVING_OPEN
    6: "GAP_TO_CLOSE",    # transient (~2 s) — treat visually like MOVING_CLOSE
}
RELAY_STATE_Y = {
    "UNKNOWN":      0.00,  # render as CLOSED-row baseline; rare in steady state
    "CLOSED":       0.00,
    "MOVING_OPEN":  0.33,
    "GAP_TO_OPEN":  0.33,
    "MOVING_CLOSE": 0.67,
    "GAP_TO_CLOSE": 0.67,
    "OPEN":         1.00,
}


def load_logs(temp_dir: Path):
    """Parse every CSV in temp_dir into typed event lists keyed by event_type.

    Skips the file-rotation header rows (SYSTEM value_a=11) at file start.
    Skips boot-marker SYSTEM rows.
    """
    events = {
        # SENSOR + SENSOR_HR are kept as separate streams so the plotter can
        # tell pre-rc.1.4.0 (legacy) from rc.1.4.0+ files cleanly. Both feed
        # the temperature/humidity panels; SENSOR_HR additionally feeds the
        # continuous wind panel + provides the bitmask sub-row that mirrors
        # the RELAY-derived window state.
        "SENSOR":      [],   # (dt, T_c_int, RH_pct)             — legacy (pre-rc.1.4.0)
        "SENSOR_HR_0": [],   # (dt, t_c10, rh_pct)               — rc.1.4.0+ ch=0
        "SENSOR_HR_1": [],   # (dt, wind_dms, wind_dir_deg)      — rc.1.4.0+ ch=1
        "SENSOR_HR_2": [],   # (dt, bitmask)                     — rc.1.4.0+ ch=2 (window state)
        "SUN":         [],   # (dt, sunrise_min, sunset_min)     — rc.1.4.0+ (local time)
        "RELAY":   [],   # (dt, channel 1/2/3, state_name)
        "MODE":    [],   # (dt, resolved_step, step_t, step_rh)
        "ALARM_W": [],   # (dt, kind, va, vb)  kind ∈ {"onset","clear","wsfault"}
        "ALARM_T": [],   # (dt, onset 1/0) — sensor-T fault (ch=4)
        "ALARM_W_FAULT": [],  # (dt, onset 1/0) — sensor-W fault (ch=5)
        "BOOT":    [],   # (dt, reset_reason)
        # rc.1.5.x — unit_id is emitted at boot via LOG_SYSTEM value_a=11,
        # value_b = unit_id (decimal of last 2 MAC bytes). Captured here so
        # the plot title can self-attribute regardless of which folder /
        # config.json the script runs against.
        "UNIT_ID": [],   # (dt, unit_id_decimal)
    }
    files_seen = []
    for path in sorted(temp_dir.glob(LOG_GLOB)):
        files_seen.append(path.name)
        with open(path, newline="") as f:
            reader = csv.reader(f)
            try:
                header = next(reader)
            except StopIteration:
                continue
            if header[:2] != ["timestamp", "type"]:
                # Not a log file
                continue
            for row in reader:
                if len(row) != 7:
                    continue
                try:
                    dt = parse_ts(row[0])
                except ValueError:
                    continue
                typ, init, ch, par, va, vb = row[1], row[2], int(row[3]), int(row[4]), int(row[5]), int(row[6])
                if typ == "SENSOR":
                    # Legacy pre-rc.1.4.0 single-row format.
                    events["SENSOR"].append((dt, va, vb))
                elif typ == "SENSOR_HR":
                    # rc.1.4.0+ triplet, discriminated by ch.
                    if ch == 0:
                        events["SENSOR_HR_0"].append((dt, va, vb))   # t_c10, rh_pct
                    elif ch == 1:
                        events["SENSOR_HR_1"].append((dt, va, vb))   # wind_dms, wind_dir_deg
                    elif ch == 2:
                        events["SENSOR_HR_2"].append((dt, va & 0xFFFF))  # bitmask
                elif typ == "SUN":
                    # rc.1.4.0+ — sunrise/sunset in local-time minutes-from-midnight.
                    events["SUN"].append((dt, va, vb))
                elif typ == "RELAY":
                    name = RELAY_STATE_NAME.get(va)
                    if name and ch in (1, 2, 3):
                        events["RELAY"].append((dt, ch, name))
                elif typ == "MODE":
                    # value_a = resolved_step
                    # value_b = packed: high byte = step_t (int8), low byte = step_rh (int8)
                    vb_unsigned = vb & 0xFFFF
                    step_t = (vb_unsigned >> 8) & 0xFF
                    if step_t & 0x80:
                        step_t -= 256
                    step_rh = vb_unsigned & 0xFF
                    if step_rh & 0x80:
                        step_rh -= 256
                    events["MODE"].append((dt, va, step_t, step_rh))
                elif typ == "ALARM":
                    if ch == 0:
                        # Wind override family. Per safety_monitor.h:
                        #   onset W1: va = wind_speed_avg_ms10, vb = v_max × 10
                        #   onset W2: va = wind_dir_avg_deg,    vb = dir_excl_low
                        #   onset sensor-fault: va = -1, vb = 0
                        #   clearance: va = wind_speed_avg_ms10 (or 0), vb = wind_dir_avg_deg (or 0)
                        # 2.3.0+ rows (gh#45) stamp the subtype into `param`
                        # (240 speed-SET, 241 dir-SET, 242 CLEAR, 243 fault-SET)
                        # → kind is exact. Legacy rows have par == 0 → kind None
                        # and infer_wind_override_intervals falls back to the
                        # value heuristic.
                        kind = {240: "onset", 241: "onset",
                                242: "clear", 243: "onset"}.get(par)
                        events["ALARM_W"].append((dt, kind, va, vb))
                    elif ch == 4:
                        events["ALARM_T"].append((dt, va))
                    elif ch == 5:
                        events["ALARM_W_FAULT"].append((dt, va))
                elif typ == "SYSTEM" and ch == 0 and par == 0 and init == "SYS":
                    if va == 5:
                        # Reset reason
                        events["BOOT"].append((dt, vb))
                    elif va == 11:
                        # Unit ID (decimal of last 2 MAC bytes; gh#17)
                        events["UNIT_ID"].append((dt, vb))
    # De-duplicate (downloaded files overlap in their rotation header rows).
    for k in events:
        events[k] = sorted(set((tuple(e) for e in events[k])))
    return events, files_seen


# ─── wind-override interval inference ───────────────────────────────────────

def infer_wind_override_intervals(alarm_w_events):
    """Build (start_dt, end_dt) intervals where wind override was active.

    Wind onset rows have non-zero/specific va/vb. Clearance is the row that
    contains low speed values OR is followed by a normal MODE row.
    Simplification: any ALARM_W row toggles state; we treat consecutive
    onset-style rows as a continuous interval until the next clear-style row.

    2.3.0+ rows (gh#45) carry an exact kind ("onset"/"clear") decoded from the
    param discriminator — used directly, no guessing. Legacy rows (kind None)
    fall back to the value heuristic:

    Onset detection: va > 0 with vb > 0 (both populated indicate spot values),
                     OR va == -1 (sensor fault),
                     OR (va, vb) == (1, 0) or (2, 0) — code-only onset rows.
    Clearance detection: va == 0 with vb == 0,
                         OR rows with va > 0 and vb > 0 but speed (va/10) < threshold (vb/10)
                         OR fault clearance: previous was -1 and we see normal speed values.

    (Legacy caveat: pre-2.3.0 SD logs don't carry an explicit onset/clear bit,
    and a legacy speed-SET at exactly speed == v_max has va == vb — the
    va < vb clearance test correctly leaves that as onset.)
    """
    intervals = []
    active = False
    open_dt = None
    for dt, kind, va, vb in alarm_w_events:
        if kind is not None:
            # Exact subtype from the 2.3.0+ param discriminator.
            is_clear = (kind == "clear")
        else:
            # Clearance heuristic — both zero OR speed below threshold
            is_clear = (va == 0 and vb == 0)
            if not is_clear and va > 0 and vb > 0:
                # speed-with-threshold form. If va < vb the spot speed is below
                # threshold => almost certainly a clearance row.
                if va < vb:
                    is_clear = True
        if is_clear:
            if active and open_dt is not None:
                intervals.append((open_dt, dt))
                active = False
                open_dt = None
        else:
            if not active:
                open_dt = dt
                active = True
    if active and open_dt is not None:
        intervals.append((open_dt, alarm_w_events[-1][0]))
    return intervals


# ─── per-day partitioning ──────────────────────────────────────────────────

def bucket_by_day(events, key_for_dt):
    """Bucket each event list into dict[UTC date] -> list of events."""
    out = defaultdict(list)
    for ev in events:
        dt = key_for_dt(ev)
        out[dt.date()].append(ev)
    return out


# ─── plotting ──────────────────────────────────────────────────────────────

def is_daytime(dt: datetime, sunrise_min_local: int, sunset_min_local: int) -> bool:
    """Naive day/night check from sunrise/sunset minutes-from-local-midnight.

    Note: 'local' here is approximated as UTC for simplicity. Refining to
    cfg.tz_str is feasible but not material for the visual shading.
    """
    minute = dt.hour * 60 + dt.minute
    if sunrise_min_local <= sunset_min_local:
        return sunrise_min_local <= minute <= sunset_min_local
    # Wrap (won't happen at our latitude, but be safe)
    return minute >= sunrise_min_local or minute <= sunset_min_local


def shade_night(ax, day_start_dt, day_end_dt, sunrise_min, sunset_min):
    """Shade night periods (00:00→sunrise and sunset→24:00) for the day."""
    sr_dt = day_start_dt.replace(hour=sunrise_min // 60,
                                  minute=sunrise_min % 60, second=0)
    ss_dt = day_start_dt.replace(hour=sunset_min // 60,
                                  minute=sunset_min % 60, second=0)
    if sr_dt > day_start_dt:
        ax.axvspan(day_start_dt, sr_dt, color="#e8e8f0", alpha=0.5, zorder=0)
    if ss_dt < day_end_dt:
        ax.axvspan(ss_dt, day_end_dt, color="#e8e8f0", alpha=0.5, zorder=0)


def plot_day(date_key, events, cfg, out_path: Path, dawn_dusk):
    """Generate one PNG for a single UTC date."""
    # Naive local-time day bounds (matches the naive timestamps from parse_ts).
    day_start = datetime.combine(date_key, datetime.min.time())
    day_end   = day_start + timedelta(days=1)

    # Slice events to this day
    def in_day(dt):
        return day_start <= dt < day_end

    # Merge legacy SENSOR rows with rc.1.4.0+ SENSOR_HR_0 (T+RH) sub-rows into
    # one unified series. Each tuple is (dt, T_value, RH_pct, t_is_c10) — the
    # boolean flags whether T_value is in 0.1 °C units (True for SENSOR_HR_0)
    # or whole °C (False for legacy SENSOR). Used downstream to format/scale
    # the temperature trace per source.
    sensor_legacy = [(d, t, rh, False) for d, t, rh in events["SENSOR"] if in_day(d)]
    sensor_hr     = [(d, t, rh, True)  for d, t, rh in events["SENSOR_HR_0"] if in_day(d)]
    sensor = sorted(sensor_legacy + sensor_hr)
    n_legacy = len(sensor_legacy)
    n_hr     = len(sensor_hr)

    # Continuous wind (rc.1.4.0+ only — SENSOR_HR_1).
    wind_hr = [(d, va, vb) for d, va, vb in events["SENSOR_HR_1"] if in_day(d)]

    relay  = [(d, c, s) for d, c, s in events["RELAY"]  if in_day(d)]
    modev  = [(d, r, t, rh) for d, r, t, rh in events["MODE"] if in_day(d)]
    aw     = [(d, k, va, vb) for d, k, va, vb in events["ALARM_W"] if in_day(d)]
    boots  = [(d, rsn) for d, rsn in events["BOOT"] if in_day(d)]

    if len(sensor) < 30:
        return None  # Skip near-empty days

    # ── Build figure ────────────────────────────────────────────────────────
    # Source-of-format banner. Files prior to rc.1.4.0 produce only legacy
    # SENSOR rows (integer °C); rc.1.4.0+ produce only SENSOR_HR_0 (0.1 °C
    # precision); a day spanning an upgrade boundary shows both kinds at the
    # corresponding times. The banner reflects what's actually on the page.
    if n_hr == 0:
        fmt_tag = "rc.1.3.x (legacy SENSOR, integer °C)"
    elif n_legacy == 0:
        fmt_tag = "rc.1.4.0+ (SENSOR_HR, 0.1 °C precision)"
    else:
        fmt_tag = f"mixed format ({n_legacy} legacy + {n_hr} HR rows)"

    # Unit ID for the title. Take the most-recent UNIT_ID row in the dataset
    # (every boot emits one; later boots win if the file spans a re-flash).
    # Fall back to "????" if no UNIT_ID row exists — which can only happen on
    # pre-gh#17 firmware (< 1.18.3) datasets, of which we have none in soak.
    if events.get("UNIT_ID"):
        unit_id_hex = f"{events['UNIT_ID'][-1][1]:04X}"
    else:
        unit_id_hex = "????"

    fig = plt.figure(figsize=(16, 12))
    fig.suptitle(
        f"Greenhouse — {date_key.isoformat()} local   ({fmt_tag} on unit 0x{unit_id_hex})",
        fontsize=13, fontweight="bold")
    gs = gridspec.GridSpec(4, 1, figure=fig, hspace=0.38)

    # rc.1.4.0+ SUN rows — per-day lookup. Prefer the day's own SUN row
    # (whichever fires last in the local-midnight window covers the date's
    # actual sun geometry). Falls back to dawn_dusk (caller's defaults,
    # typically live /api/status snapshot) for days without a SUN row —
    # e.g. days before rc.1.4.0 shipped.
    sun_today = [(d, sr, ss) for d, sr, ss in events["SUN"] if in_day(d)]
    if sun_today:
        # Use the latest SUN row in the day — captures both the boot-time
        # emit and the midnight-rollover emit; the midnight one is the
        # canonical "today's" value.
        _, sunrise_min, sunset_min = sun_today[-1]
        sun_source = f"SUN row (in-log, {len(sun_today)} this day)"
    else:
        sunrise_min, sunset_min = dawn_dusk
        sun_source = "fallback (caller-provided defaults)"

    fmt = mdates.DateFormatter("%H:%M")
    loc = mdates.HourLocator(interval=2)

    # ── Panel 1: Temperature ───────────────────────────────────────────────
    ax1 = fig.add_subplot(gs[0])
    shade_night(ax1, day_start, day_end, sunrise_min, sunset_min)
    # Per-sample scaling: SENSOR_HR_0 carries t_c10 (0.1 °C precision); legacy
    # SENSOR carries integer °C. Divide by 10 only for the HR-origin samples
    # so the plot reads in °C regardless of source. Per-day plots may show
    # the precision step-up across an upgrade boundary as a visible smoothing
    # of the trace.
    dts = [d for d, _, _, _ in sensor]
    Ts  = [(t / 10.0) if hr else float(t) for _, t, _, hr in sensor]
    ax1.plot(dts, Ts, "r-", lw=1.6, label=f"Indoor T ({fmt_tag})")
    ax1.axhline(cfg["t_max_day"], color="#c0392b", ls=":", lw=1.2,
                label=f"t_max_day = {cfg['t_max_day']} °C")
    ax1.axhline(cfg["t_min_day"], color="#27ae60", ls=":", lw=1.2,
                label=f"t_min_day = {cfg['t_min_day']} °C")
    ax1.axhline(cfg["t_max_ngt"], color="#e67e22", ls=":", lw=1.0,
                label=f"t_max_ngt = {cfg['t_max_ngt']} °C")
    ax1.axhline(cfg["t_min_ngt"], color="#16a085", ls=":", lw=1.0,
                label=f"t_min_ngt = {cfg['t_min_ngt']} °C")
    ax1.set_ylabel("Temperature [°C]")
    ax1.legend(fontsize=7, loc="upper right", ncol=3)
    ax1.grid(True, alpha=0.25)
    ax1.xaxis.set_major_locator(loc)
    ax1.xaxis.set_major_formatter(fmt)
    ax1.set_xlim(day_start, day_end)

    # ── Panel 2: Humidity ──────────────────────────────────────────────────
    ax2 = fig.add_subplot(gs[1], sharex=ax1)
    shade_night(ax2, day_start, day_end, sunrise_min, sunset_min)
    # RH is integer % in both legacy SENSOR and rc.1.4.0+ SENSOR_HR_0.
    RHs = [rh for _, _, rh, _ in sensor]
    ax2.plot(dts, RHs, "b-", lw=1.6, label="Indoor RH (integer %)")
    ax2.axhline(cfg["rh_max_day"], color="#2980b9", ls=":", lw=1.2,
                label=f"rh_max_day = {cfg['rh_max_day']} %")
    ax2.axhline(cfg["rh_min_day"], color="#27ae60", ls=":", lw=1.2,
                label=f"rh_min_day = {cfg['rh_min_day']} %")
    ax2.axhline(cfg["rh_max_ngt"], color="#8e44ad", ls=":", lw=1.0,
                label=f"rh_max_ngt = {cfg['rh_max_ngt']} %")
    ax2.set_ylabel("Relative Humidity [%]")
    ax2.set_ylim(0, 105)
    ax2.legend(fontsize=7, loc="upper right", ncol=3)
    ax2.grid(True, alpha=0.25)

    # ── Panel 3: Wind ──────────────────────────────────────────────────────
    # Two rendering modes depending on what's in the log:
    #   (a) rc.1.4.0+ continuous trace from SENSOR_HR_1 (preferred — actual
    #       per-poll wind speed sampled at the same cadence as T/RH)
    #   (b) rc.1.3.x spot values from ALARM rows (fallback — only fires at
    #       wind-override onset/clearance; sparse)
    # Both modes overlay the wind-override intervals (orange shading) and the
    # configured v_max threshold line.
    ax3 = fig.add_subplot(gs[2], sharex=ax1)
    shade_night(ax3, day_start, day_end, sunrise_min, sunset_min)
    intervals = infer_wind_override_intervals(aw)
    interval_handle = None
    for s, e in intervals:
        h = ax3.axvspan(max(s, day_start), min(e, day_end),
                        color="orange", alpha=0.30, zorder=1)
        interval_handle = h

    # Mode (a): continuous trace from rc.1.4.0+ SENSOR_HR_1 rows.
    wind_dts  = [d for d, _, _ in wind_hr]
    wind_ms   = [va / 10.0 for _, va, _ in wind_hr]   # va is wind speed × 10
    max_ms_seen = max(wind_ms) if wind_ms else 0.0
    if wind_dts:
        ax3.plot(wind_dts, wind_ms, color="#34495e", lw=1.4, zorder=2,
                 label=f"Wind speed (rc.1.4.0+ SENSOR_HR, {len(wind_dts)} samples)")

    # Mode (b): legacy ALARM-row spot values (only when no continuous trace).
    spot_dts, spot_ms = [], []
    if not wind_dts:
        for d, _k, va, vb in aw:
            if va == -1:
                continue
            if va > 0 and vb > 0 and va < 2000:
                spot_dts.append(d)
                spot_ms.append(va / 10.0)
        if spot_dts:
            ax3.scatter(spot_dts, spot_ms, c="#34495e", s=30, zorder=3,
                        label=f"Spot wind speed at ALARM tx ({len(spot_dts)} points)")
        max_ms_seen = max(max_ms_seen, max(spot_ms) if spot_ms else 0.0)

    # v_max threshold line.
    ax3.axhline(cfg["v_max"], color="#e67e22", ls=":", lw=1.2,
                label=f"v_max = {cfg['v_max']} m/s")
    ax3.set_ylabel("Wind speed [m/s]" if wind_dts
                   else "Wind speed [m/s]\n(spot values at\nALARM rows only)")
    ax3.set_ylim(0, max(cfg["v_max"] + 5, max_ms_seen + 2))

    # Wind DIRECTION on a secondary y-axis (0–360°, labelled N/E/S/W). Rendered
    # as a scatter — not a line — so the trace does not draw spurious vertical
    # strokes when the bearing wraps 360°↔0°. rc.1.4.0+ SENSOR_HR_1 carries the
    # direction in vb (degrees). (Reminder: wind is only valid from
    # 2026-06-19 12:00 — the vane's commissioning; earlier bearings are garbage.)
    ax3b = None
    if wind_dts:
        wind_dir = [vb % 360 for _, _, vb in wind_hr]
        ax3b = ax3.twinx()
        ax3b.scatter(wind_dts, wind_dir, s=4, c="#8e44ad", alpha=0.35, zorder=1,
                     label=f"Wind direction ({len(wind_dir)} samples)")
        ax3b.set_ylim(0, 360)
        ax3b.invert_yaxis()   # N at top, reading N -> E -> S -> W -> N downward
        ax3b.set_yticks([0, 90, 180, 270, 360])
        ax3b.set_yticklabels(["N", "E", "S", "W", "N"])
        ax3b.set_ylabel("Wind direction", color="#8e44ad")
        ax3b.tick_params(axis="y", labelcolor="#8e44ad")
        ax3b.set_xlim(day_start, day_end)

    handles = []
    labels = []
    if interval_handle is not None:
        handles.append(interval_handle)
        labels.append(f"Wind override active ({len(intervals)} intervals)")
    h_axes, l_axes = ax3.get_legend_handles_labels()
    handles.extend(h_axes)
    labels.extend(l_axes)
    if ax3b is not None:
        h_dir, l_dir = ax3b.get_legend_handles_labels()
        handles.extend(h_dir)
        labels.extend(l_dir)
    if handles:
        ax3.legend(handles, labels, fontsize=7, loc="upper right")
    ax3.grid(True, alpha=0.25)

    # When no continuous wind is available (pre-rc.1.4.0 file), call it out
    # explicitly so the reader knows the panel is sparse by data limitation,
    # not by a quiet day.
    if not wind_dts:
        ax3.text(0.01, 0.97,
                 "Continuous wind not in this file (legacy rc.1.3.x format). "
                 "rc.1.4.0+ adds LOG_SENSOR_HR ch=1 wind sub-row at the 30 s "
                 "sample rate.",
                 transform=ax3.transAxes, fontsize=7, va="top",
                 color="#7f8c8d", style="italic")

    # ── Panel 4: Window position per channel ───────────────────────────────
    ax4 = fig.add_subplot(gs[3], sharex=ax1)
    shade_night(ax4, day_start, day_end, sunrise_min, sunset_min)
    names = ["M1 (window 1)", "M2 (window 2)", "M3 (window 3)"]
    colors = ["#e67e22", "#27ae60", "#2980b9"]
    track_h = 1.3

    # Per channel, build (dt, y_value) step trace.
    # Initial state at day_start = the last state from the previous day's events.
    # We reconstruct by looking at all RELAY events sorted by time and tracking.
    last_state = {1: "CLOSED", 2: "CLOSED", 3: "CLOSED"}
    # Walk ALL relay events up to day_start to seed
    for d, c, s in events["RELAY"]:
        if d >= day_start:
            break
        last_state[c] = s

    for i, (name, color) in enumerate(zip(names, colors)):
        ch = i + 1
        # Build the per-channel time-series for this day
        ts = [day_start]
        ys = [RELAY_STATE_Y[last_state[ch]] + i * track_h]
        for d, c, s in relay:
            if c != ch:
                continue
            # Push the prior state up to this event then step
            ts.append(d)
            ys.append(RELAY_STATE_Y[last_state[ch]] + i * track_h)
            last_state[ch] = s
            ts.append(d)
            ys.append(RELAY_STATE_Y[s] + i * track_h)
        # Close trace at day end
        ts.append(day_end)
        ys.append(RELAY_STATE_Y[last_state[ch]] + i * track_h)
        ax4.plot(ts, ys, color=color, lw=1.8, label=name)

    ax4.set_ylabel("Window state\nC ↑ ↓ O")
    yticks, ylabels = [], []
    for i in range(3):
        for s, y in (("C", 0.00), ("↑", 0.33), ("↓", 0.67), ("O", 1.00)):
            yticks.append(y + i * track_h)
            ylabels.append(s)
    ax4.set_yticks(yticks)
    ax4.set_yticklabels(ylabels, fontsize=7)
    ax4.set_ylim(-0.1, 2 * track_h + 1.1)
    ax4.set_xlabel("Time (local — Europe/Amsterdam)")
    ax4.legend(fontsize=7, loc="upper right")
    ax4.grid(True, alpha=0.25, axis="x")
    ax4.xaxis.set_major_locator(loc)
    ax4.xaxis.set_major_formatter(fmt)

    # Boot/reset markers across all panels
    for d, rsn in boots:
        for ax in (ax1, ax2, ax3, ax4):
            ax.axvline(d, color="red", ls="--", lw=1.0, alpha=0.6, zorder=2)
            if ax is ax1:
                ax.text(d, ax.get_ylim()[1] * 0.97,
                        f" BOOT (reason={rsn})",
                        rotation=90, fontsize=7, color="red",
                        va="top", ha="left")

    plt.xticks(rotation=20, ha="right")
    plt.savefig(out_path, dpi=120, bbox_inches="tight")
    plt.close(fig)

    return {
        "samples": len(sensor),
        "n_legacy": n_legacy,
        "n_hr": n_hr,
        "wind_hr_samples": len(wind_dts),
        "sun_rows": len(sun_today),
        "sun_source": sun_source,
        "t_min": min(Ts),
        "t_max": max(Ts),
        "rh_min": min(RHs),
        "rh_max": max(RHs),
        "wind_override_intervals": len(intervals),
        "wind_alarm_rows": len(aw),
        "boots": len(boots),
        "mode_changes": len(modev),
    }


# ─── main ───────────────────────────────────────────────────────────────────

def main():
    temp_dir = HERE
    try:
        cfg = json.load(open(temp_dir / "config.json"))
    except FileNotFoundError:
        print("[error] temp/config.json not found; fetch it via /api/config first.")
        sys.exit(2)

    events, files_seen = load_logs(temp_dir)
    # Accept either legacy SENSOR (pre-rc.1.4.0) or SENSOR_HR_0 (rc.1.4.0+).
    # The day-union at the next step already handles HR-only datasets; this
    # guard previously rejected them despite the comment to the contrary.
    if not events["SENSOR"] and not events["SENSOR_HR_0"]:
        print("[error] no SENSOR or SENSOR_HR rows found in temp/*.csv")
        sys.exit(2)

    # Day buckets
    # Days to plot — union of legacy SENSOR and rc.1.4.0+ SENSOR_HR_0 dates,
    # so an HR-only file (e.g. campaign data after rc.1.4.0 ships) is rendered
    # even when no legacy SENSOR rows exist.
    days = sorted({d.date() for d, _, _ in events["SENSOR"]}
                  | {d.date() for d, _, _ in events["SENSOR_HR_0"]})

    # Optional single-day mode: `plot_daily.py YYYY-MM-DD` renders just that day
    # (focused analysis, without re-rendering the whole campaign or clobbering
    # the full plot_summary.txt).
    single_day = None
    if len(sys.argv) > 1:
        try:
            single_day = datetime.strptime(sys.argv[1], "%Y-%m-%d").date()
        except ValueError:
            print(f"[error] bad date '{sys.argv[1]}' — expected YYYY-MM-DD")
            sys.exit(2)
        days = [d for d in days if d == single_day]
        if not days:
            print(f"[error] no sensor data for {sys.argv[1]}")
            sys.exit(2)
        print(f"[filter] single-day mode: {sys.argv[1]}")

    # Sunrise / sunset for night-shading.
    # Pulled live from temp/status.json (controller's /api/status snapshot) —
    # the values are minutes-from-local-midnight, which matches our naive
    # local-time timestamps exactly. Fall back to seasonal defaults for
    # 52° N late May (~05:30 / 21:30 local).
    try:
        status = json.load(open(temp_dir / "status.json"))
        sun = status.get("sun", {})
        sunrise_min = int(sun.get("sunrise_min", 5 * 60 + 30))
        sunset_min  = int(sun.get("sunset_min",  21 * 60 + 30))
    except (FileNotFoundError, ValueError, KeyError):
        sunrise_min = 5 * 60 + 30
        sunset_min  = 21 * 60 + 30
    dawn_dusk_default = (sunrise_min, sunset_min)
    print(f"[sun] using sunrise={sunrise_min//60:02d}:{sunrise_min%60:02d} "
          f"sunset={sunset_min//60:02d}:{sunset_min%60:02d} (local) for night-shading")

    summary_lines = []
    summary_lines.append(f"Source files: {', '.join(files_seen)}")
    summary_lines.append(f"Sensor samples total: legacy SENSOR={len(events['SENSOR'])}, "
                         f"rc.1.4.0+ SENSOR_HR_0={len(events['SENSOR_HR_0'])}")
    summary_lines.append(f"Continuous wind samples (SENSOR_HR_1): {len(events['SENSOR_HR_1'])}")
    summary_lines.append(f"SUN rows (rc.1.4.0+): {len(events['SUN'])}")
    summary_lines.append(f"Date range: {days[0].isoformat()} … {days[-1].isoformat()}")
    summary_lines.append("")
    summary_lines.append(f"{'date':<12} {'samples':>8} {'fmt':>8} {'T min/max':>12} {'RH min/max':>11} "
                         f"{'wind/d':>7} {'sun':>4} {'wind-ovr':>9} {'boots':>6} {'modes':>6} {'PNG':>4}")
    summary_lines.append("-" * 110)

    generated = []
    for day in days:
        out_png = temp_dir / f"plot_{day.isoformat()}.png"
        stats = plot_day(day, events, cfg, out_png, dawn_dusk_default)
        if stats is None:
            summary_lines.append(f"{day.isoformat():<12} (skipped — too few samples)")
            continue
        # 'fmt' column: legacy / HR / mixed depending on which row types fed the day.
        if stats['n_hr'] == 0:
            fmt_short = "legacy"
        elif stats['n_legacy'] == 0:
            fmt_short = "HR"
        else:
            fmt_short = "mixed"
        # 'sun' column: 'log' = day used its own SUN row(s); 'fb' = fell back to /api/status defaults.
        sun_short = "log" if stats['sun_rows'] > 0 else "fb"
        summary_lines.append(
            f"{day.isoformat():<12} {stats['samples']:>8} {fmt_short:>8} "
            f"{stats['t_min']:>5}/{stats['t_max']:<5} "
            f"{stats['rh_min']:>4}/{stats['rh_max']:<5} "
            f"{stats['wind_hr_samples']:>7} "
            f"{sun_short:>4} "
            f"{stats['wind_override_intervals']:>9} "
            f"{stats['boots']:>6} "
            f"{stats['mode_changes']:>6} "
            f"{out_png.name}"
        )
        generated.append(out_png)

    summary_path = temp_dir / "plot_summary.txt"
    if single_day is None:                       # don't clobber the full summary in single-day mode
        summary_path.write_text("\n".join(summary_lines) + "\n", encoding="utf-8")
    print("\n".join(summary_lines))
    print(f"\nGenerated {len(generated)} PNG(s) in {temp_dir}")


if __name__ == "__main__":
    main()
