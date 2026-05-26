#!/usr/bin/env python3
"""
logparser.py — Greenhouse Controller log parser

Converts NVS-export and SD-card CSV logs into human-readable text.

Usage
-----
    # Parse a single file:
    python logparser.py 20250607163022.csv

    # Parse all timestamp-named SD log files in the current directory,
    # concatenate them (date order) into one output file:
    python logparser.py *

See logparser.md for full documentation.
"""

import csv
import ctypes
import os
import re
import struct
import sys
from datetime import datetime, timezone

# ---------------------------------------------------------------------------
# Constants — match firmware app_types.h and relay_controller.cpp
# ---------------------------------------------------------------------------

_EVENT_TYPES = {
    "SENSOR":    "SENSOR",      # legacy single-row format — pre-rc.1.4.0 files only
    "SENSOR_HR": "SENSOR_HR",   # rc.1.4.0+ — high-resolution triplet (T+RH, wind, bitmask)
    "SUN":       "SUN",         # rc.1.4.0+ — sunrise/sunset record
    "RELAY":     "RELAY",
    "MODE":      "MODE",
    "SETPT":     "SETPT",
    "SESSION":   "SESSION",
    "ALARM":     "ALARM",
    "SYSTEM":    "SYSTEM",
}

_CH_STATE = {
    0: "UNKNOWN",
    1: "CLOSED",
    2: "MOVING_OPEN",
    3: "OPEN",
    4: "MOVING_CLOSE",
    5: "GAP_TO_OPEN",
    6: "GAP_TO_CLOSE",
}

_VENT_STEP = {
    0: "all closed",
    1: "M1 open",
    2: "M1+M2 open",
    3: "M1+M2+M3 open (ridge vent)",
}

# log_param_id_t (from app_types.h) — param → (name, unit)
#
# Sensitive fields (PIN, WiFi credentials, secrets) log a "changed" marker
# without exposing the value. Numeric fields log old → new the way the
# climate/wind setpoints do. The parser renders sensitive fields with
# value_a=1 as "<field> changed" (no value); numeric fields render as
# "<field>: <old> -> <new>" via the normal _decode_setpoint path.
_PARAM = {
    0:  ("none",            ""),
    # C1..C22 — original setpoint table from logAnalysis.md.
    1:  ("t_min_day",       "degC"),
    2:  ("t_max_day",       "degC"),
    3:  ("t_min_ngt",       "degC"),
    4:  ("t_max_ngt",       "degC"),
    5:  ("rh_min_day",      "%"),
    6:  ("rh_max_day",      "%"),
    7:  ("rh_min_ngt",      "%"),
    8:  ("rh_max_ngt",      "%"),
    9:  ("hyst_t",          "degC"),
    10: ("hyst_rh",         "%"),
    11: ("rh_ctrl_en",      ""),
    12: ("cr_priority",     ""),
    13: ("avg_win_t",       "samples"),
    14: ("avg_win_rh",      "samples"),
    15: ("v_max",           "m/s"),
    16: ("dir_excl_low",    "deg"),
    17: ("dir_excl_high",   "deg"),
    18: ("dwell_open",      "s"),
    19: ("dwell_close",     "s"),
    20: ("poll_interval",   "s"),
    21: ("lat/lon",         ""),
    22: ("cr_applied",      ""),

    # 2.0.0-a.6.35.5 — audit-trail entries for setting changes that
    # previously had no LOG_SETPOINT emission. Sensitive (no value):
    23: ("tz_str",          "(set)"),
    24: ("wifi_ssid",       "(set)"),
    25: ("wifi_psk",        "(set)"),
    26: ("wifi_ap_psk",     "(set)"),
    27: ("pin_farmer",      "(changed)"),
    28: ("pin_admin",       "(changed)"),
    29: ("status_url",      "(set)"),
    30: ("status_secret",   "(set)"),
    # Numeric (old → new):
    31: ("status_intv_s",   "s"),
    32: ("status_enable",   ""),
    33: ("status_expose",   "bitmask"),
    34: ("log_upload_h",    "h"),
    35: ("log_upload_m",    "min"),
    36: ("log_upload_rot",  ""),
    37: ("wind_prot_en",    ""),
}

# Param IDs whose value semantics are "field was set/changed" (value_a=1
# is a sentinel, not a real old/new pair). The decoder renders these with
# just the field name + "(set)" / "(changed)" rather than "1 -> 0".
_PARAM_SENTINEL_VALUE = frozenset({23, 24, 25, 26, 27, 28, 29, 30})

# Initiator strings shown in CSV → friendly label
_INITIATOR = {
    "SYS":    "System",
    "FARMER": "Farmer (LCD)",
    "ADMIN":  "Admin (LCD)",
    "MQTT":   "MQTT",
    "WEB":    "Web UI",
}

# Output column widths
_COL_TS   = 20   # "2025-06-07 14:30:22"
_COL_TYPE = 11   # "[SENSOR_HR ]"  (widest type string in rc.1.4.0+; was 8 pre-rc.1.4.0 for "[SENSOR  ]")
_COL_INIT = 14   # "System        "

# Regex that identifies a valid SD-card log filename: 14 digits + ".csv"
_SD_FILENAME_RE = re.compile(r"^\d{14}\.csv$", re.IGNORECASE)


# ---------------------------------------------------------------------------
# Timestamp helpers
# ---------------------------------------------------------------------------

def _fmt_local(iso_str: str) -> str:
    """
    Return a fixed-width local-time timestamp string from an ISO 8601 string.

    Input:  "2026-05-19T13:30:22"
    Output: "2026-05-19 13:30:22"

    Since firmware 2.0.0-a.6.35.3 the CSV row timestamps are in **local
    time** (matches the SD filename convention which has always been local).
    Older log files emitted before that release have UTC timestamps inside
    but local-time filenames — the parser passes the string through
    unchanged in both cases since the visual format is identical. The
    column heading and the docs make the timezone explicit.
    """
    return iso_str.replace("T", " ")


# Backwards-compat alias for any external caller of the old name.
_fmt_utc = _fmt_local


def _date_from_sd_filename(name: str) -> str:
    """
    Extract the date portion (YYYYMMDD) from an SD card filename.
    "20250607163022.csv" → "20250607"
    """
    return os.path.splitext(os.path.basename(name))[0][:8]


# ---------------------------------------------------------------------------
# Event decoders — one function per event type
# ---------------------------------------------------------------------------

def _decode_sensor(row: dict) -> str:
    """SENSOR (legacy, pre-rc.1.4.0 files only): value_a=temp_°C  value_b=rh_%"""
    try:
        temp = int(row["value_a"])
        rh   = int(row["value_b"])
        return f"T={temp} degC   RH={rh} %"
    except (ValueError, KeyError):
        return f"raw: a={row.get('value_a')} b={row.get('value_b')}"


# rc.1.4.0 — public window_state_t encoding for the SENSOR_HR bitmask.
# Per model/logUpdatePlan.md §2.2: 0=CLOSED, 1=MOVING_OPEN, 2=OPEN, 3=MOVING_CLOSE.
# GAP states fold into the matching MOVING state in t2_get_window_states(),
# so the bitmask packer never produces GAP encodings.
_WIN_STATE_BITMASK = {
    0: "CLOSED",
    1: "MOVING_OPEN",
    2: "OPEN",
    3: "MOVING_CLOSE",
}

# Short 4-char rendering for the per-channel state shown on the LCD.
_WIN_STATE_SHORT = {
    0: "CLOS",
    1: "MOV>",
    2: "OPEN",
    3: "MOV<",
}


def _decode_sensor_hr(row: dict) -> str:
    """SENSOR_HR (rc.1.4.0+): three-row sensor triplet keyed by ch.

      ch=0 → T+RH        : value_a = t_c10 (deci-deg C), value_b = rh_pct
      ch=1 → wind        : value_a = wind speed × 10 (deci-m/s), value_b = wind_dir_deg
      ch=2 → window bits : value_a = packed 16-bit bitmask (see logUpdatePlan §2.2)

    See model/logUpdatePlan.md §2 + design/technicalSoftwareDesignSpecification.md §5.3.
    """
    try:
        ch = int(row["ch"])
        a  = int(row["value_a"])
        b  = int(row["value_b"])
    except (ValueError, KeyError):
        return f"raw: ch={row.get('ch')} a={row.get('value_a')} b={row.get('value_b')}"

    if ch == 0:
        # T+RH — T in 0.1 °C precision; RH in integer %.
        return f"T={a / 10:.1f} degC   RH={b} %"

    if ch == 1:
        # Wind — speed in deci-m/s, direction in degrees.
        return f"wind={a / 10:.1f} m/s  dir={b} deg"

    if ch == 2:
        # Window-state bitmask — pack-uint16 of 3×2-bit channel fields + 3 EG1 flags.
        mask = a & 0xFFFF
        m1 = _WIN_STATE_SHORT.get((mask     ) & 0x3, "?   ")
        m2 = _WIN_STATE_SHORT.get((mask >> 2) & 0x3, "?   ")
        m3 = _WIN_STATE_SHORT.get((mask >> 4) & 0x3, "?   ")
        flags = []
        if mask & (1 << 12): flags.append("WIND")
        if mask & (1 << 13): flags.append("ALARM")
        if mask & (1 << 14): flags.append("CAL")
        flag_str = f"  [{','.join(flags)}]" if flags else ""
        return f"M1={m1}  M2={m2}  M3={m3}{flag_str}  (0x{mask:04X})"

    return f"ch={ch} a={a} b={b}  (unknown SENSOR_HR sub-row)"


def _decode_sun(row: dict) -> str:
    """SUN (rc.1.4.0+): value_a=sunrise_min, value_b=sunset_min (local time).

    Both values are minutes-from-local-midnight (0..1439). See logUpdatePlan §3.
    """
    try:
        sr_min = int(row["value_a"])
        ss_min = int(row["value_b"])
        sr_hh, sr_mm = divmod(sr_min, 60)
        ss_hh, ss_mm = divmod(ss_min, 60)
        return f"sunrise={sr_hh:02d}:{sr_mm:02d} local  sunset={ss_hh:02d}:{ss_mm:02d} local"
    except (ValueError, KeyError):
        return f"raw: a={row.get('value_a')} b={row.get('value_b')}"


def _decode_relay(row: dict) -> str:
    """RELAY: ch=motor(1-3)  value_a=ch_state_t"""
    try:
        ch    = int(row["ch"])
        state = int(row["value_a"])
        ch_name    = f"M{ch}" if ch in (1, 2, 3) else f"ch{ch}"
        state_name = _CH_STATE.get(state, f"state#{state}")
        return f"{ch_name}: -> {state_name}"
    except (ValueError, KeyError):
        return f"raw: ch={row.get('ch')} a={row.get('value_a')}"


def _decode_mode(row: dict) -> str:
    """
    MODE (LOG_MODE_CHANGE from climate_control.cpp):
      value_a = resolved ventilation step (0–3)
      value_b = packed int16: high byte = step_t (int8), low byte = step_rh (int8)
                step = -1 means VENT_STEP_NEUTRAL (no demand from that sensor)
    """
    try:
        resolved = int(row["value_a"])
        packed   = int(row["value_b"])

        # Unpack the two signed int8 values from the int16
        packed_u = packed & 0xFFFF
        step_t  = ctypes.c_int8((packed_u >> 8) & 0xFF).value
        step_rh = ctypes.c_int8(packed_u & 0xFF).value

        step_name = _VENT_STEP.get(resolved, f"step {resolved}")

        def _step_label(v: int) -> str:
            if v == -1:
                return "neutral"
            return _VENT_STEP.get(v, f"step {v}")

        return (
            f"Vent step -> {resolved} ({step_name})"
            f"  [T-demand: {_step_label(step_t)}"
            f"  RH-demand: {_step_label(step_rh)}]"
        )
    except (ValueError, KeyError):
        return f"raw: a={row.get('value_a')} b={row.get('value_b')}"


def _decode_setpoint(row: dict) -> str:
    """
    SETPT (LOG_SETPOINT):
      param = log_param_id_t identifying the config key
      value_a = old value   value_b = new value
      ch = motor channel for dwell_open/dwell_close, 0 otherwise

    Since 2.0.0-a.6.35.5 the parameter space includes audit-only entries
    for sensitive admin operations (PIN changes, WiFi credentials, the
    TZ string, the status-website URL/secret). These use value_a=1 as a
    sentinel for "changed/set" — the actual value isn't loggable for
    security reasons. The parser surfaces these as "<field> (set)" or
    "<field> (changed)" rather than the numeric old/new pair.
    """
    try:
        param_id = int(row["param"])
        old_val  = int(row["value_a"])
        new_val  = int(row["value_b"])
        ch       = int(row["ch"])

        param_name, unit = _PARAM.get(param_id, (f"param#{param_id}", ""))

        # Sensitive / sentinel-value rows: value_a=1 = "set"/"changed".
        # Don't render as "1 -> 0" — that's misleading. unit string carries
        # the human-readable verb (e.g. "(set)", "(changed)").
        if param_id in _PARAM_SENTINEL_VALUE:
            if old_val == 1:
                return f"{param_name} {unit}".rstrip()
            # Defensive fallback for unexpected value_a — shouldn't happen
            # but won't be invisible if it does.
            return f"{param_name} {unit}  [raw a={old_val} b={new_val}]"

        # Motor-specific params include the channel
        ch_suffix = f" (M{ch})" if ch in (1, 2, 3) and param_id in (18, 19) else ""

        def _fmt(v: int) -> str:
            if param_id == 11:   # rh_ctrl_en — boolean
                return "enabled" if v else "disabled"
            if param_id == 32:   # status_enable — boolean
                return "enabled" if v else "disabled"
            if param_id == 36:   # log_upload_rot — boolean
                return "enabled" if v else "disabled"
            if param_id == 37:   # wind_prot_en — boolean
                return "enabled" if v else "disabled"
            if param_id == 33:   # status_expose — hex bitmask
                return f"0x{v:02X}"
            return f"{v} {unit}".strip()

        return f"{param_name}{ch_suffix}: {_fmt(old_val)} -> {_fmt(new_val)}"
    except (ValueError, KeyError):
        return f"raw: param={row.get('param')} a={row.get('value_a')} b={row.get('value_b')}"


def _decode_session(row: dict) -> str:
    """
    SESSION:
      value_a = session level (0=closed, 1=farmer, 2=admin)
    """
    try:
        level     = int(row["value_a"])
        initiator = row.get("initiator", "?").strip()
        by        = _INITIATOR.get(initiator, initiator)

        if level == 0:
            return f"Session closed [{by}]"
        elif level == 1:
            return f"Session opened: Farmer [{by}]"
        elif level == 2:
            return f"Session opened: Admin [{by}]"
        else:
            return f"Session level {level}  [{by}]"
    except (ValueError, KeyError):
        return f"raw: a={row.get('value_a')}"


def _decode_alarm(row: dict) -> str:
    """
    ALARM — three producers in the firmware:

    T2 relay_controller.cpp — motor alarm (channel = 0):
      onset:    value_a=1, value_b=0
      cleared:  value_a=0, value_b=0

    T3 safety_monitor.cpp — wind override (channel = 0):
      set (sensor fault):   value_a=-1,  value_b=0
      set (speed):          value_a=speed×10, value_b=v_max×10
      set (direction):      value_a=direction°, value_b=excl_low°
      cleared (disabled):   value_a=0,   value_b=0
      cleared (safe):       value_a=speed×10,  value_b=direction°

    T5 sensor_poll.cpp — sensor read fault (since 2.0.0-a.6.35.3):
      channel = 4 → T/RH sensor; channel = 5 → wind sensor
      value_a = 1 → fault triggered, value_a = 0 → fault cleared
      value_b = 0
      (Pre-a.6.35.3 these used `value_a = ±1, ±2` with `channel = 0`, which
      aliased to motor alarm and wind-override sensor-fault — the parser
      had no way to disambiguate. The new ch-based encoding lets T2/T3
      keep ch=0 and T5 own ch>=4.)

    Channel-based dispatch handles T5 first; everything else falls through
    to the legacy va-based disambiguation:
      value_a ==  1  → motor alarm onset
      value_a == -1  → wind override: sensor fault safe-fail
      value_a ==  0  → motor alarm cleared  OR  wind override cleared (disabled)
      value_a >   1  → wind override event (speed or direction)
    """
    try:
        ch = int(row["ch"])
        va = int(row["value_a"])
        vb = int(row["value_b"])

        # T5 sensor faults — channel carries the sensor kind (4 = T/RH, 5 = wind).
        if ch == 4:
            return ("T/RH sensor fault: "
                    + ("triggered (two consecutive read failures)"
                       if va else "cleared"))
        if ch == 5:
            return ("Wind sensor fault: "
                    + ("triggered (two consecutive read failures)"
                       if va else "cleared"))

        if va == 1 and vb == 0:
            return "MOTOR ALARM: triggered - all relays de-energised"

        if va == -1 and vb == 0:
            return "WIND OVERRIDE: SET - wind sensor fault safe-fail"

        if va == 0 and vb == 0:
            # Ambiguous: could be motor alarm cleared or wind override cleared
            # (disabled while active).  Report both possibilities.
            return "Motor alarm / wind override: CLEARED"

        if va > 1:
            # Three possible wind override events share va > 1:
            #
            #  A) SET speed:    va = measured_speed*10, vb = v_max*10
            #                   va ALWAYS > vb (triggered by exceeding)
            #                   vb is v_max*10: typically 30–200 (v_max 3–20 m/s)
            #
            #  B) SET direction: va = direction° (0–359), vb = excl_low° (0–359)
            #                   No size constraint between va and vb.
            #                   Both values are large (typically > 100).
            #
            #  C) CLEARED normal: va = current_speed*10 (now safe, small),
            #                     vb = current_direction° (0–359)
            #                   va is typically small (below v_max*10 = < 200)
            #                   vb is direction (0–359, often > va)
            #
            # Disambiguation heuristic (best-effort):
            #   1. va > vb AND vb <= 200  →  speed SET  (vb is v_max*10, max 20 m/s)
            #   2. va < 150  AND vb > va  →  CLEARED    (va is now-safe speed*10)
            #   3. otherwise              →  direction SET

            if vb == 0:
                # No direction reading available at clear time
                speed = va / 10.0
                return f"WIND OVERRIDE: CLEARED - speed now {speed:.1f} m/s"

            if va > vb and vb <= 200:
                # Speed SET: measured speed exceeded v_max
                speed = va / 10.0
                v_max = vb / 10.0
                return (
                    f"WIND OVERRIDE: SET - speed {speed:.1f} m/s"
                    f" >= v_max {v_max:.1f} m/s"
                )

            if va < 150 and vb > va:
                # CLEARED normal: speed is now safe (small value), direction reported
                speed = va / 10.0
                return (
                    f"WIND OVERRIDE: CLEARED - speed {speed:.1f} m/s,"
                    f" direction {vb} deg"
                )

            # Direction SET: va=direction, vb=excl_low (both angle-range)
            return (
                f"WIND OVERRIDE: SET - direction {va} deg"
                f" in exclusion zone (low bound {vb} deg)"
            )

        if va >= 0 and vb > 0:
            # va == 0, vb > 0: wind override cleared, direction only
            return f"WIND OVERRIDE: CLEARED - direction {vb} deg"

        return f"Alarm event: a={va} b={vb}"
    except (ValueError, KeyError):
        return f"raw: a={row.get('value_a')} b={row.get('value_b')}"


_ESP_RESET_REASON = {
    0:  "UNKNOWN",
    1:  "POWERON",
    2:  "EXT",
    3:  "SW (esp_restart)",
    4:  "PANIC",
    5:  "INT_WDT",
    6:  "TASK_WDT",
    7:  "WDT",
    8:  "DEEPSLEEP",
    9:  "BROWNOUT",
    10: "SDIO",
}


def _decode_system(row: dict) -> str:
    """
    SYSTEM events. value_a categorises the SYSTEM-event subtype; value_b is
    the payload. Match the LOG_SYSTEM value_a table in
    ``firmware/src/event_logger/event_logger.h`` exactly.

    Producer key:
      WEB   = T14 status_post.cpp
      NET   = T10 network_manager.cpp
      T4    = task_data_manager() (post-RTC-seed)
      T9    = event_logger.cpp (force-rotate marker, Q3 drop synthetic)
      T1    = task_watchdog_heartbeat() (heap rows + corruption + largest-block)
      T2    = relay_controller.cpp (boot-cal skipped)

    Subtypes:
      a=-1, b=count                   Q3 drop-overflow (T9 synthetic)
      a=0,  b=0/1/2/3 (initiator=WEB) T14 outcome / diagnostic skip
      a=1,  b=0/1                     STA (WiFi client) connected/disconnected
      a=2,  b=0/1                     NTP timeout / synced
      a=3,  b=0/1                     AP stopped / started
      a=4,  b=1                       Geolocation success
      a=5,  b=esp_reset_reason 1..10  BOOT (since 1.17.27, T4-emitted since 1.17.31)
      a=6,  b=0                       Force-rotate marker (since 1.17.28)
      a=7,  b=KB                      HEAP internal free (since 1.17.29)
      a=8,  b=KB                      HEAP PSRAM free (since 1.17.29)
      a=9,  b=0                       HEAP corruption (since 1.17.29)
      a=10, b=0                       T2 boot-calibration skipped (since 1.17.36)
      a=11, b=uid16 (int16-cast)      Unit ID (since 1.18.3) — low 16 bits of WiFi-STA MAC
      a=12, b=KB                      HEAP internal largest contiguous (since 1.18.2)
      a=13, b=0                       T13 firmware-only fallback commit (since 2.0.0-a.6.34)

    The legacy "a=0 b=0 initiator=SYS = boot marker" form was retired in
    1.17.31; older logs that contain it are reported as such.
    """
    try:
        va        = int(row["value_a"])
        vb        = int(row["value_b"])
        initiator = row.get("initiator", "SYS").strip()

        # ---------------------------------------------------------------
        # Q3 drop overflow (T9 synthetic)
        # ---------------------------------------------------------------
        if va == -1:
            return f"Q3 queue overflow: {vb} event(s) dropped"

        # ---------------------------------------------------------------
        # T14 outcome / skip (initiator=WEB) — value_a=0 sub-codes
        # ---------------------------------------------------------------
        if va == 0 and initiator == "WEB":
            if vb == 0:
                return "T14 status POST: failed (streak transition)"
            if vb == 1:
                return "T14 log upload: failed"
            if vb == 2:
                return "T14 daily slot fired but no closed file on SD"
            if vb == 3:
                return ("T14 daily slot fired but precondition blocked it "
                        "(status disabled / URL empty / WiFi down / pre-NTP / OTA)")
            return f"T14 SYSTEM event: a=0 b={vb} (Web)"

        # T14 success outcomes share the value_b code (0=status POST, 1=log upload)
        if va == 1 and initiator == "WEB":
            if vb == 0:
                return "T14 status POST: success"
            if vb == 1:
                return "T14 log upload: success"
            return f"T14 SYSTEM event: a=1 b={vb} (Web)"

        # ---------------------------------------------------------------
        # value_a=1..4 — network events (initiator=SYS, posted by T10)
        # ---------------------------------------------------------------
        if va == 1 and initiator in ("SYS", ""):
            return ("STA WiFi client: " + ("connected" if vb else "disconnected"))

        if va == 2 and initiator in ("SYS", ""):
            return "NTP: " + ("synced" if vb else "timeout")

        if va == 3 and initiator in ("SYS", ""):
            return "WiFi AP: " + ("started" if vb else "stopped")

        if va == 4 and initiator in ("SYS", ""):
            if vb == 1:
                return "Geolocation: success"
            return f"Geolocation event: b={vb}"

        # ---------------------------------------------------------------
        # value_a=5 — boot reason (posted by T4 since 1.17.31)
        # ---------------------------------------------------------------
        if va == 5:
            reason = _ESP_RESET_REASON.get(vb, f"reason#{vb}")
            return f"Boot: esp_reset_reason = {vb} ({reason})"

        # ---------------------------------------------------------------
        # value_a=6 — T9 force-rotate marker (last entry in a rotated-out file)
        # ---------------------------------------------------------------
        if va == 6:
            return "T9 force-rotate marker (last entry before rotation)"

        # ---------------------------------------------------------------
        # value_a=7/8 — periodic free-heap snapshot (T1)
        # ---------------------------------------------------------------
        if va == 7:
            return f"Heap internal free: {vb} KB"

        if va == 8:
            return f"Heap PSRAM free: {vb} KB"

        # ---------------------------------------------------------------
        # value_a=9 — heap integrity check failure (T1)
        # ---------------------------------------------------------------
        if va == 9:
            return "Heap CORRUPTION detected (heap_caps_check_integrity_all)"

        # ---------------------------------------------------------------
        # value_a=10 — T2 boot-calibration skipped (gh#18 Phase 3)
        # ---------------------------------------------------------------
        if va == 10:
            return ("T2 boot calibration skipped — NVS-recovered window state "
                    "(all three channels CLOSED)")

        # ---------------------------------------------------------------
        # value_a=11 — Unit ID, low 16 bits of WiFi-STA MAC (since 1.18.3, gh#17).
        # value_b is int16 in the CSV; reinterpret as uint16 and render as
        # 4-char uppercase hex — matches the AP-SSID convention
        # "Greenhouse-XXXX" so the same 4 chars identify a unit everywhere.
        # ---------------------------------------------------------------
        if va == 11:
            uid = vb & 0xFFFF
            return f"Unit ID: {uid:04X} (AP SSID would be 'Greenhouse-{uid:04X}')"

        # ---------------------------------------------------------------
        # value_a=12 — largest contiguous internal-heap block (since 1.18.2)
        # ---------------------------------------------------------------
        if va == 12:
            return f"Heap internal largest block: {vb} KB"

        # ---------------------------------------------------------------
        # value_a=13 — T13 firmware-only fallback commit (since 2.0.0-a.6.34).
        # Fires when a firmware OTA was verified but no paired web-asset
        # upload arrived inside the 120 s window; T13 commits the firmware
        # alone via esp_ota_set_boot_partition + schedule_reboot. The row
        # is written ~3 s before the reboot, so the NEXT row in the file
        # (after the heap-row triple) is the BOOT entry with the new
        # esp_reset_reason. value_b is reserved; always 0 in this release.
        # ---------------------------------------------------------------
        if va == 13:
            return ("T13 firmware-only fallback commit "
                    "(no asset upload received within 120 s)")

        # ---------------------------------------------------------------
        # value_a=14..17 — OTA stage progress (since 2.0.0-a.6.35.3).
        # Previously emitted as post_log(0/1/2/-1), which collided with
        # T14 status outcome / T10 STA / T10 NTP / T9 Q3 drop-overflow
        # codes. Re-numbered into a dedicated OTA range that doesn't
        # overlap any other producer. Operator-visible meaning:
        #
        #   14  /api/ota/firmware started — bytes streaming to inactive bank
        #   15  /api/ota/firmware ended OK — bank verified, waiting for assets
        #   16  /api/ota/assets ended OK  — ZIP extracted to inactive LittleFS,
        #                                    reboot scheduled in 1 s
        #   17  Asset OTA failed         — ZIP extraction error, OTA aborted
        #
        # A normal OTA cycle produces rows 14 → 15 → 16 → BOOT.
        # An interrupted cycle (a.6.34 fallback) is 14 → 15 → 13 → BOOT.
        # ---------------------------------------------------------------
        if va == 14:
            return "OTA: firmware POST started (bytes streaming to inactive bank)"
        if va == 15:
            return "OTA: firmware verified OK — awaiting web-asset upload"
        if va == 16:
            return "OTA: asset ZIP extracted OK — reboot scheduled (1 s)"
        if va == 17:
            return "OTA: asset extraction FAILED — boot partition unchanged"

        # ---------------------------------------------------------------
        # value_a=18..20 — coredump-retrieval audit (since 2.0.0-a.6.35.6).
        # Initiator-gated to avoid mis-rendering pre-a.6.35.3 heartbeat
        # noise (the retired main.cpp heartbeat emitted value_a=uptime_s
        # with initiator=SYS, so historic CSVs from older firmware contain
        # value_a=18/19/20 rows with bogus value_b=heap_kb payloads).
        #
        #   18 (SYS, value_b = KB)   T4 detected a coredump from a previous
        #                             panic at boot. The dump is sitting in
        #                             the coredump partition; download via
        #                             GET /api/coredump/download.
        #   19 (WEB, value_b ≈ KB/4) Admin downloaded the coredump from the
        #                             GUI Log → Diagnostics panel.
        #   20 (WEB, value_b = 0)    Admin erased the coredump partition
        #                             after a confirmed offline decode.
        #
        # A normal post-panic cycle looks like:
        #   <PANIC reboot>
        #   BOOT (value_a=5, value_b=4=PANIC)
        #   value_a=18, value_b=N  ← T4 detected the dump
        #   ... operator opens the GUI ...
        #   value_a=19            ← Admin downloaded
        #   value_a=20            ← Admin erased (after offline decode)
        # ---------------------------------------------------------------
        if va == 18 and initiator == "SYS":
            return f"Coredump from previous panic detected in flash: ~{vb} KB"
        if va == 19 and initiator == "WEB":
            # value_b is bytes / 256; render the approximate KB.
            approx_kb = (vb * 256) // 1024 if vb >= 0 else "?"
            return f"Coredump downloaded by admin (~{approx_kb} KB transferred)"
        if va == 20 and initiator == "WEB":
            return "Coredump erased by admin (partition wiped)"

        # ---------------------------------------------------------------
        # Legacy / ambiguous: pre-1.17.31 boot marker
        # ---------------------------------------------------------------
        if va == 0 and vb == 0 and initiator in ("SYS", ""):
            return "Legacy boot marker (pre-1.17.31)"

        # Admin-initiated WiFi AP toggle (older firmware path)
        if initiator == "ADMIN" and vb == 0:
            state = "enabled" if va else "disabled"
            return f"WiFi AP {state}  [Admin]"

        # ---------------------------------------------------------------
        # Legacy main.cpp heartbeat (pre-2.0.0-a.6.35.3).
        #
        # The original alpha.6.6 main.cpp scaffold posted a synthetic
        # LOG_SYSTEM row every 5 s with:
        #     value_a = uptime_seconds  (5, 10, 15, 20, …)
        #     value_b = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024
        #     initiator = LOG_BY_SYSTEM, ch = 0, param = 0
        # to prove T9 was alive on the SD card.
        #
        # The emit was REMOVED in 2.0.0-a.6.35.3 — see the rationale
        # comment at `firmware/src/main.cpp` (the `heartbeat_task()`
        # function around lines 302-318). Reason: every uptime-second
        # value (5, 7, 8, 9, 10, 11, 12, …) collides with a documented
        # subtype in event_logger.h's LOG_SYSTEM table (5=BOOT,
        # 7/8/12=heap rows, 9=corruption, 10=T2-skip, 11=unit-id,
        # 13=T13-fallback, 14-17=OTA, 18=coredump-detected). The
        # heartbeat is INDISTINGUISHABLE from a real subtype event
        # for uptime values that overlap with documented codes; this
        # decoder will (correctly) prefer the documented interpretation
        # for those rows.
        #
        # value_a == 19 and value_a == 20 are documented as WEB-only
        # (coredump downloaded / erased — admin-initiated, fired by
        # T11). A row with value_a=19 or value_a=20 and initiator=SYS
        # has no documented producer, so it is unambiguously a legacy
        # heartbeat (uptime=19 s or 20 s). From uptime=21 s onward
        # there is no overlap at all.
        #
        # SD log files that span the a.6.35.3 transition (which on any
        # field unit is a one-time event) contain a finite block of
        # these rows from the pre-upgrade sessions. The decoder makes
        # those rows readable instead of falling through to the opaque
        # "System event: a=N b=M" generic.
        # ---------------------------------------------------------------
        if va >= 19 and initiator in ("SYS", ""):
            return (f"Legacy heartbeat (pre-a.6.35.3): "
                    f"uptime={va} s, free internal heap={vb} KB")

        return (f"System event: a={va} b={vb}  "
                f"[{_INITIATOR.get(initiator, initiator)}]")
    except (ValueError, KeyError):
        return f"raw: a={row.get('value_a')} b={row.get('value_b')}"


# Dispatch table
_DECODERS = {
    "SENSOR":    _decode_sensor,      # legacy pre-rc.1.4.0
    "SENSOR_HR": _decode_sensor_hr,   # rc.1.4.0+ — three-row triplet
    "SUN":       _decode_sun,         # rc.1.4.0+ — sunrise/sunset record
    "RELAY":     _decode_relay,
    "MODE":      _decode_mode,
    "SETPT":     _decode_setpoint,
    "SESSION":   _decode_session,
    "ALARM":     _decode_alarm,
    "SYSTEM":    _decode_system,
}


# ---------------------------------------------------------------------------
# Row formatter
# ---------------------------------------------------------------------------

def _format_row(row: dict, line_no: int) -> str:
    """
    Format one CSV row as a human-readable line.

    Output format:
        2026-05-19 13:30:22  [SENSOR ]  System          T=23 °C   RH=65 %
    """
    ts        = _fmt_local(row.get("timestamp", "?").strip())
    etype     = row.get("type", "?").strip().upper()
    initiator = row.get("initiator", "?").strip()

    decoder     = _DECODERS.get(etype)
    description = decoder(row) if decoder else (
        f"a={row.get('value_a')} b={row.get('value_b')} "
        f"ch={row.get('ch')} param={row.get('param')}"
    )

    type_col = f"[{etype:<9}]"  # 9-char inner field accommodates "SENSOR_HR"
    init_col = f"{_INITIATOR.get(initiator, initiator):<{_COL_INIT}}"

    return f"{ts}  {type_col}  {init_col}  {description}"


# ---------------------------------------------------------------------------
# File parser
# ---------------------------------------------------------------------------

def parse_csv(path: str) -> list[str]:
    """
    Read a greenhouse controller CSV log file and return a list of
    human-readable lines (one per event row, plus a header block).
    """
    lines = []
    basename = os.path.basename(path)

    # Header block
    lines.append("=" * 80)
    lines.append(f"  Source file : {basename}")
    lines.append(f"  Parsed      : {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S')} UTC")
    lines.append("=" * 80)
    lines.append(
        f"{'Timestamp (local)':<{_COL_TS}}  "
        f"{'Type':<{_COL_TYPE+2}}  "
        f"{'Initiator':<{_COL_INIT}}  "
        f"Description"
    )
    lines.append("-" * 80)

    try:
        with open(path, newline="", encoding="utf-8", errors="replace") as fh:
            reader = csv.DictReader(fh)

            # Validate expected header
            expected = {"timestamp", "type", "initiator", "ch", "param",
                        "value_a", "value_b"}
            if reader.fieldnames is None:
                lines.append("  [ERROR] File is empty or has no header row.")
                return lines

            actual = {f.strip().lower() for f in reader.fieldnames}
            missing = expected - actual
            if missing:
                lines.append(
                    f"  [WARNING] Missing CSV columns: {', '.join(sorted(missing))}"
                )
                lines.append("")

            count = 0
            for line_no, row in enumerate(reader, start=2):
                # Normalise keys to lower-case in case of capitalisation drift
                row = {k.strip().lower(): v for k, v in row.items()}
                formatted = _format_row(row, line_no)
                lines.append(formatted)
                count += 1

            lines.append("-" * 80)
            lines.append(f"  {count} event(s) in {basename}")

    except FileNotFoundError:
        lines.append(f"  [ERROR] File not found: {path}")
    except PermissionError:
        lines.append(f"  [ERROR] Permission denied: {path}")
    except Exception as exc:  # pylint: disable=broad-except
        lines.append(f"  [ERROR] {exc}")

    lines.append("")
    return lines


# ---------------------------------------------------------------------------
# Output file helpers
# ---------------------------------------------------------------------------

def _output_path(input_path: str, prefix: str = "parsed_") -> str:
    """
    Build the output path by prepending `prefix` to the basename and
    replacing the extension with `.txt`.

        "logs/20250607163022.csv" → "logs/parsed_20250607163022.txt"
        "nvs_log.csv"             → "parsed_nvs_log.txt"
    """
    directory = os.path.dirname(input_path) or "."
    stem      = os.path.splitext(os.path.basename(input_path))[0]
    return os.path.join(directory, f"{prefix}{stem}.txt")


def _wildcard_output_path(first_file: str, prefix: str = "parsed_") -> str:
    """
    Build the concatenated output path.  Only the date part (YYYYMMDD) of
    the first file's name is used.

        "20250607163022.csv" → "parsed_20250607.txt"
    """
    directory = os.path.dirname(first_file) or "."
    date_part = _date_from_sd_filename(first_file)
    return os.path.join(directory, f"{prefix}{date_part}.txt")


def _write_lines(path: str, lines: list[str]) -> None:
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
        fh.write("\n")


# ---------------------------------------------------------------------------
# Single-file mode
# ---------------------------------------------------------------------------

def process_single(input_path: str) -> None:
    """Parse one CSV file and write the output next to it."""
    out_path = _output_path(input_path)
    lines    = parse_csv(input_path)
    _write_lines(out_path, lines)
    print(f"  {os.path.basename(input_path)}  ->  {os.path.basename(out_path)}"
          f"  ({len(lines)-5} events)")


# ---------------------------------------------------------------------------
# Wildcard mode
# ---------------------------------------------------------------------------

def _find_sd_files(directory: str) -> list[str]:
    """
    Return all SD-card log files (matching \\d{14}\\.csv) in `directory`,
    sorted lexicographically (= chronological order because filenames are
    local-time timestamps).
    """
    try:
        entries = os.listdir(directory)
    except (FileNotFoundError, PermissionError) as exc:
        print(f"[ERROR] Cannot list directory '{directory}': {exc}", file=sys.stderr)
        return []

    matched = sorted(
        e for e in entries
        if _SD_FILENAME_RE.match(e)
    )
    return [os.path.join(directory, e) for e in matched]


def process_wildcard(directory: str = ".") -> None:
    """
    Find all timestamp-named SD log files in `directory`, parse them in
    chronological order, and write a single concatenated output file
    named parsed_YYYYMMDD.txt (date taken from the earliest file).
    """
    files = _find_sd_files(directory)

    if not files:
        print(f"[INFO] No SD log files (YYYYMMDDHHMMSS.csv) found in '{directory}'.")
        return

    print(f"Found {len(files)} SD log file(s) in '{directory}':")
    for f in files:
        print(f"  {os.path.basename(f)}")

    out_path = _wildcard_output_path(files[0])

    all_lines: list[str] = []

    # Preamble
    all_lines.append("=" * 80)
    all_lines.append("  Greenhouse Controller - combined SD log")
    all_lines.append(
        f"  Files       : {len(files)}  "
        f"({os.path.basename(files[0])} ... {os.path.basename(files[-1])})"
    )
    all_lines.append(
        f"  Parsed      : "
        f"{datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S')} UTC"
    )
    all_lines.append("=" * 80)
    all_lines.append("")

    total_events = 0
    for path in files:
        file_lines = parse_csv(path)
        # Count the data lines (skip the 6-line header block + 2 footer lines)
        data_count = sum(
            1 for ln in file_lines
            if ln and not ln.startswith(("=", "-", " ", "T"))
        )
        total_events += data_count
        all_lines.extend(file_lines)

    all_lines.append("=" * 80)
    all_lines.append(
        f"  Total: {total_events} event(s) across {len(files)} file(s)"
    )
    all_lines.append("=" * 80)

    _write_lines(out_path, all_lines)
    print(f"\nOutput -> {out_path}")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    if len(sys.argv) < 2:
        print(
            "Usage:\n"
            "  python logparser.py <file.csv>     - parse a single log file\n"
            "  python logparser.py *              — parse all SD log files in "
            "the current directory\n"
            "\nSee logparser.md for full documentation.",
            file=sys.stderr,
        )
        sys.exit(1)

    arg = sys.argv[1]

    if arg == "*":
        # Wildcard: scan the directory of the script (or cwd)
        search_dir = os.path.dirname(os.path.abspath(sys.argv[0]))
        # If called as `python logparser.py *` from a log directory, use cwd
        if os.getcwd() != os.path.dirname(os.path.abspath(sys.argv[0])):
            search_dir = os.getcwd()
        process_wildcard(search_dir)
    else:
        # Explicit path — allow glob expansion if the shell did not expand it
        # (e.g. on Windows where `*` is passed literally by some shells)
        if "*" in arg or "?" in arg:
            import glob
            matched = sorted(
                p for p in glob.glob(arg)
                if _SD_FILENAME_RE.match(os.path.basename(p))
            )
            if not matched:
                print(f"[ERROR] No matching SD log files for pattern: {arg}",
                      file=sys.stderr)
                sys.exit(1)
            directory = os.path.dirname(os.path.abspath(matched[0]))
            process_wildcard(directory)
        else:
            if not os.path.isfile(arg):
                print(f"[ERROR] File not found: {arg}", file=sys.stderr)
                sys.exit(1)
            process_single(arg)


if __name__ == "__main__":
    main()
