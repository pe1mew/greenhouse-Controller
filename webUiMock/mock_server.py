#!/usr/bin/env python3
"""
Greenhouse Controller — Web UI Mock Server
==========================================
Serves the static files from firmware/data/ and emulates all REST and
WebSocket endpoints that the real ESP32 firmware (web_server.cpp +
status_post/status_json.cpp) exposes.

Targets firmware 1.17.20 — canonical nested status-JSON shape, status-
website POST configuration (Web tab), and the OTA-version mismatch
diagnostic surfaces (manifest.json, asset_version, HTML version stamp).

Endpoints emulated
------------------
GET  /                  index.html from firmware/data/ (with ?v=<ver>
                        cache-busters injected on app.js / style.css)
GET  /style.css         stylesheet
GET  /app.js            JavaScript
GET  /manifest.json     {"asset_version": "<ver>", "checksum": ""}
GET  /api/whoami        check cookie validity → {ok, role}
POST /api/login         {role, pin} → {ok, role}
POST /api/logout        → {ok:true} + clears cookie
GET  /api/status        full status JSON (canonical nested shape;
                        same payload as WebSocket push)
GET  /api/config        all configuration parameters
GET  /api/config/limits min/max ranges per parameter (public, no auth)
POST /api/config        {ns, key, value} or {ns, key, str_value}
POST /api/wifi          {ssid, psk} or {ap_psk}
POST /api/pin           {role, pin}  (admin only)
GET  /api/web           current status-website settings (admin only)
POST /api/web           update status-website settings (admin only)
GET  /api/history       ?n=N  — last N synthetic sensor readings
GET  /api/sd/status     {mounted, free_mb, size_mb}
POST /api/sd/mount      mount SD card (admin only)
POST /api/sd/unmount    unmount SD card (admin only)
GET  /api/ota/status    {ok, state, progress, error}  (any logged-in role)
POST /api/ota/firmware  upload firmware .bin  (admin only) → {ok, rebooting}
POST /api/ota/assets    upload web assets .zip (admin only) → 202 + {ok, message}
GET  /api/log/files     {nvs_count, sd_files:[...]}  (admin only)
GET  /api/log/download  ?src=nvs  or  ?src=sd&file=NAME  (admin only) → CSV
WS   /ws                push status JSON every 2 s

Usage
-----
    cd webUiMock
    pip install flask flask-sock
    python mock_server.py
    # open http://localhost:5000
    # Farmer PIN: 1234   Admin PIN: 12345678
"""

import json
import math
import os
import secrets
import threading
import time
from datetime import datetime
from pathlib import Path

from flask import Flask, make_response, request, send_from_directory
from flask_sock import Sock

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
BASE_DIR = Path(__file__).resolve().parent
DATA_DIR = BASE_DIR.parent / "firmware" / "data"

app  = Flask(__name__, static_folder=None)
sock = Sock(app)

# ---------------------------------------------------------------------------
# In-memory state
# ---------------------------------------------------------------------------
pins = {"farmer": "1234", "admin": "12345678"}

sessions: dict[str, dict] = {}
sessions_lock = threading.Lock()

# NVS config state — mirrors build_config_json() in web_server.cpp.
# Default values track firmware/config/cfg_defaults.h so the GUI sees the
# same fresh-flash factory defaults whether it talks to the mock or to a
# real device.
cfg: dict = {
    "wifi_ssid":           "MyNetwork",
    "ap_ssid":             "Greenhouse-AABB",
    "t_max_day":           28,    # DEF_T_MAX_DAY
    "t_min_day":           16,    # DEF_T_MIN_DAY
    "t_max_ngt":           20,    # DEF_T_MAX_NGT
    "t_min_ngt":           14,    # DEF_T_MIN_NGT
    "rh_max_day":          75,    # DEF_RH_MAX_DAY
    "rh_min_day":          50,    # DEF_RH_MIN_DAY
    "rh_max_ngt":          80,    # DEF_RH_MAX_NGT
    "rh_min_ngt":          55,    # DEF_RH_MIN_NGT
    "hyst_t":               5,    # DEF_HYST_T
    "hyst_rh":             12,    # DEF_HYST_RH (1.16.31: anti-oscillation widening)
    "rh_ctrl_en":           1,
    "cr_priority":          0,
    "avg_win_t":            6,    # DEF_AVG_WIN_T
    "avg_win_rh":          10,    # DEF_AVG_WIN_RH (1.16.31)
    "v_max":                6,    # DEF_V_MAX
    "wind_prot_en":         1,
    "dir_excl_low":         0,
    "dir_excl_high":        0,
    "travel_s":            [21, 21, 171],     # MOTOR_M{1,2,3}_TRAVEL_S_DEFAULT
    "dwell_open_min":      [300, 300, 1500],  # DEF_DWELL_OPEN_M{1,2,3}_S (1.16.31: per-motor split, M3=1500)
    "dwell_close_min":     [  0,   0,  600],  # DEF_DWELL_CLOSE_M{1,2,3}_S (1.16.31: per-motor split, M3=600)
    "poll_interval_s":     30,    # DEF_POLL_INTERVAL_S
    "session_timeout_min":  5,    # DEF_SESSION_TIMEOUT_MIN
    "ap_timeout_min":      30,    # DEF_AP_TIMEOUT_MIN
    "lat_deg":             52,    # DEF_LAT_DEG
    "lat_frac":             0,    # DEF_LAT_FRAC
    "lon_deg":              5,    # DEF_LON_DEG
    "lon_frac":             0,    # DEF_LON_FRAC
    "tz_str":              "CET-1CEST,M3.5.0,M10.5.0/3",
    "fw_ver":              "1.17.20",
    # Status-website / Web tab settings (added in firmware 1.17.0).  Defaults
    # mirror DEF_STATUS_* / DEF_LOG_UPLOAD_* in firmware/config/cfg_defaults.h.
    "status_url":          "",
    "status_secret":       "",
    "status_interval_s":   120,
    "status_enable":         0,
    "status_expose":      0x3F,
    "log_upload_h":          3,
    "log_upload_m":         15,
    "log_upload_rot":        1,
    "log_last_up":          "",
}

# Process start time — drives system.uptime_s in the canonical status JSON.
BOOT_TIME: float = time.monotonic()

sd: dict = {"mounted": True, "size_mb": 7500, "free_mb": 7100}

# ---------------------------------------------------------------------------
# Synthetic log data (mirrors log_entry_t / log_event_t from firmware)
# Fields: timestamp, event_type, initiator, channel, param_id, value_a, value_b
# event_type → string: 0=SENSOR 1=RELAY 2=MODE 3=SETPT 4=SESSION 5=ALARM 6=SYSTEM
# initiator  → string: 0=SYS 1=FARMER 2=ADMIN 3=MQTT 4=WEB
# ---------------------------------------------------------------------------
_EVT_TYPE  = ["SENSOR", "RELAY",   "MODE",  "SETPT", "SESSION", "ALARM", "SYSTEM"]
_EVT_INIT  = ["SYS",    "FARMER",  "ADMIN", "MQTT",  "WEB"]

def _nvs_log_entries() -> list[dict]:
    """Generate 64 synthetic NVS log entries spanning the last ~32 minutes."""
    now = int(time.time())
    entries = []
    for i in range(64):
        ts         = now - (64 - i) * 30
        etype      = [0, 0, 1, 0, 2, 0, 3, 0, 1, 0, 4, 0, 0, 6, 0, 1][i % 16]
        initiator  = [0, 0, 0, 0, 0, 4, 2, 0, 0, 0, 4, 0, 0, 0, 1, 0][i % 16]
        channel    = i % 3
        param_id   = i % 8
        value_a    = int(20.0 * 10 + 40 * math.sin(ts / 7200) * 10)   # °C × 10
        value_b    = int(60 * 10 + 100 * math.sin(ts / 10800 + 1.0))  # RH × 10
        entries.append({
            "ts":        ts,
            "type":      _EVT_TYPE[etype],
            "initiator": _EVT_INIT[initiator],
            "ch":        channel,
            "param":     param_id,
            "value_a":   value_a,
            "value_b":   value_b,
        })
    return entries

def _sd_log_files() -> list[str]:
    """Return 3 synthetic SD log filenames using local-time timestamp format."""
    now = time.time()
    files = []
    for hours_ago in (3, 2, 1):
        t = time.localtime(now - hours_ago * 3600)
        files.append(time.strftime("%Y%m%d%H%M%S", t) + ".csv")
    return files

def _sd_csv_content(filename: str) -> str:
    """Return synthetic CSV content for the given SD filename (ISO 8601 timestamps)."""
    # Derive time base from the filename timestamp (YYYYMMDDHHMMSS)
    try:
        base = time.mktime(time.strptime(filename[:14], "%Y%m%d%H%M%S"))
    except (ValueError, IndexError):
        base = time.time() - 3600
    lines = ["timestamp,type,initiator,ch,param,value_a,value_b"]
    for i in range(120):
        ts        = base + i * 30
        etype     = _EVT_TYPE[i % len(_EVT_TYPE)]
        initiator = _EVT_INIT[i % len(_EVT_INIT)]
        ch        = i % 3
        param     = i % 8
        va        = int(20.0 * 10 + 40 * math.sin(ts / 7200) * 10)
        vb        = int(60  * 10 + 100 * math.sin(ts / 10800 + 1.0))
        ts_str    = time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(ts))
        lines.append(f"{ts_str},{etype},{initiator},{ch},{param},{va},{vb}")
    return "\n".join(lines) + "\n"

# OTA simulation state
OTA_STATES = ["idle", "fw_writing", "fw_verifying", "fw_done",
              "assets_buffering", "assets_writing", "rebooting", "error"]
ota: dict = {"state": "idle", "progress": 0, "error": "",
             "bank": "A", "accepted": True}
ota_lock = threading.Lock()

# ---------------------------------------------------------------------------
# NVS (namespace, key) → (cfg_field, array_index)
# Maps what POST /api/config sends to the in-memory cfg dict.
# ---------------------------------------------------------------------------
NVS_MAP: dict[tuple, tuple] = {
    ("climate", "t_max_day"):       ("t_max_day",           None),
    ("climate", "t_min_day"):       ("t_min_day",           None),
    ("climate", "t_max_ngt"):       ("t_max_ngt",           None),
    ("climate", "t_min_ngt"):       ("t_min_ngt",           None),
    ("climate", "rh_max_day"):      ("rh_max_day",          None),
    ("climate", "rh_min_day"):      ("rh_min_day",          None),
    ("climate", "rh_max_ngt"):      ("rh_max_ngt",          None),
    ("climate", "rh_min_ngt"):      ("rh_min_ngt",          None),
    ("climate", "hyst_t"):          ("hyst_t",              None),
    ("climate", "hyst_rh"):         ("hyst_rh",             None),
    ("climate", "avg_win_t"):       ("avg_win_t",           None),
    ("climate", "avg_win_rh"):      ("avg_win_rh",          None),
    ("climate", "rh_ctrl_en"):      ("rh_ctrl_en",          None),
    ("climate", "cr_priority"):     ("cr_priority",         None),
    ("wind",    "v_max"):           ("v_max",               None),
    ("wind",    "dir_excl_low"):    ("dir_excl_low",        None),
    ("wind",    "dir_excl_high"):   ("dir_excl_high",       None),
    ("wind",    "wind_prot_en"):    ("wind_prot_en",        None),
    ("motor",   "travel_m1"):       ("travel_s",               0),
    ("motor",   "travel_m2"):       ("travel_s",               1),
    ("motor",   "travel_m3"):       ("travel_s",               2),
    ("motor",   "dwell_open_m1"):   ("dwell_open_min",         0),
    ("motor",   "dwell_open_m2"):   ("dwell_open_min",         1),
    ("motor",   "dwell_open_m3"):   ("dwell_open_min",         2),
    ("motor",   "dwell_close_m1"):  ("dwell_close_min",        0),
    ("motor",   "dwell_close_m2"):  ("dwell_close_min",        1),
    ("motor",   "dwell_close_m3"):  ("dwell_close_min",        2),
    ("system",  "session_timeout"): ("session_timeout_min", None),
    ("system",  "ap_timeout"):      ("ap_timeout_min",      None),
    ("system",  "poll_interval"):   ("poll_interval_s",     None),
    ("system",  "lat_deg"):         ("lat_deg",             None),
    ("system",  "lat_frac"):        ("lat_frac",            None),
    ("system",  "lon_deg"):         ("lon_deg",             None),
    ("system",  "lon_frac"):        ("lon_frac",            None),
    ("system",  "tz_str"):          ("tz_str",              None),
    ("wifi",    "ssid"):            ("wifi_ssid",           None),
}

# Farmer-writable (ns, key) pairs — mirrors FARMER_KEYS / FARMER_WIND_KEYS in
# web_server.cpp.
FARMER_WRITABLE: set[tuple] = {
    ("climate", "t_max_day"),
    ("climate", "t_min_day"),
    ("climate", "t_max_ngt"),
    ("climate", "t_min_ngt"),
    ("climate", "rh_max_day"),
    ("climate", "rh_min_day"),
    ("climate", "rh_max_ngt"),
    ("climate", "rh_min_ngt"),
    ("climate", "rh_ctrl_en"),
    ("climate", "cr_priority"),
    ("wind",    "wind_prot_en"),
}

# ---------------------------------------------------------------------------
# Session helpers
# ---------------------------------------------------------------------------
def _session_create(role: str) -> str:
    token  = secrets.token_hex(8)
    expiry = time.time() + cfg["session_timeout_min"] * 60
    with sessions_lock:
        sessions[token] = {"role": role, "expiry": expiry}
    return token


def _session_find(token: str) -> str | None:
    if not token:
        return None
    with sessions_lock:
        s = sessions.get(token)
        if s:
            if s["expiry"] > time.time():
                return s["role"]
            del sessions[token]
    return None


def _session_destroy(token: str) -> None:
    with sessions_lock:
        sessions.pop(token, None)


def _get_role() -> str | None:
    """Return 'farmer' | 'admin' | None for the current request."""
    token = request.cookies.get("session", "")
    return _session_find(token)

# ---------------------------------------------------------------------------
# Sensor / status data generators
# ---------------------------------------------------------------------------
def _build_status() -> dict:
    """Canonical nested status JSON — matches build_canonical_status_json() in
    firmware/src/status_post/status_json.cpp as of firmware 1.17.20.  Every
    key the local app.js and the public dashboard consume must appear here
    in the exact same shape, otherwise the mock and the real device diverge
    silently and the GUI breaks against one of them."""
    t = time.time()
    temp     = 20.0 + 4.0  * math.sin(t / 7200)
    rh       = 60   + 10   * math.sin(t / 10800 + 1.0)
    wind     = max(0.0, 2.5 + 2.0 * math.sin(t / 900 + 2.0))
    wind_dir = int((180 + 90 * math.sin(t / 3600 + 3.0)) % 360)

    now      = datetime.now()
    cur_mins = now.hour * 60 + now.minute
    # Sun times in LOCAL minutes-from-midnight (firmware 1.17.1+ converts
    # UTC→local in dm_status_snapshot using gmtoff so the public dashboard
    # renders the value verbatim as HH:MM).
    sunrise  = 6 * 60 + 30     # 06:30 local
    sunset   = 20 * 60 + 30    # 20:30 local
    is_day   = sunrise <= cur_mins <= sunset

    # Active climate setpoints — pick the day or night value from the cfg
    # dict to mirror dm_status_snapshot()'s is_daytime branch.
    t_max_active  = cfg["t_max_day"]  if is_day else cfg["t_max_ngt"]
    rh_max_active = cfg["rh_max_day"] if is_day else cfg["rh_max_ngt"]
    rh_min_active = cfg["rh_min_day"] if is_day else cfg["rh_min_ngt"]
    rh_ctrl_on    = (cfg["rh_ctrl_en"] != 0)

    # Wind-direction variation — synthetic ±20° wobble around the average
    # plus a slow swelling component so the value moves visibly between
    # ~30° (steady) and ~110° (shifting). The real firmware computes this
    # as the smallest arc containing every direction sample in the sliding
    # window (see dir_avg_variation() in firmware/src/sensor_poll/).
    wind_var = int(40 + 35 * (1 + math.sin(t / 1800 + 0.7)))

    return {
        "type": "status",
        # Mock serves the local-UI consumer path, so it ALWAYS includes the
        # RH-setpoint values (the GUI dims the rows itself when rh_ctrl is
        # off via the rh_ctrl_enabled flag).  rh_ctrl_enabled is always
        # emitted so consumers know which mode is active.  The real-device
        # T14 → public-dashboard path differs — see firmware
        # status_post/status_json.cpp `include_disabled_setpoints=false`.
        "climate": {
            "temp_c":           round(temp, 1),
            "temp_avg_c":       round(temp, 1),
            "rh_pct":           int(round(rh)),
            "rh_avg_pct":       int(round(rh)),
            "temp_max_active":  t_max_active,
            "rh_max_active":    rh_max_active,
            "rh_min_active":    rh_min_active,
            "rh_ctrl_enabled":  rh_ctrl_on,
        },
        "wind": {
            "speed_ms":                round(wind, 1),
            "speed_avg_ms":            round(wind, 1),
            "direction_deg":           wind_dir,
            "direction_avg_deg":       wind_dir,
            "direction_variation_deg": wind_var,
        },
        "windows": {
            "M1": "CLOSED",
            "M2": "CLOSED",
            "M3": "CLOSED",
        },
        "mode": {
            "current": "AUTOMATIC",
            "flags":   [],
        },
        "sun": {
            "is_daytime":   is_day,
            "sunrise_min":  sunrise,
            "sunset_min":   sunset,
        },
        "system": {
            "wifi_ip":       "192.168.1.100",
            "wifi_rssi_dbm": -65,
            "ntp_synced":    True,
            "fw_ver":        cfg["fw_ver"],
            # asset_version is what /manifest.json reports — in the mock
            # they're paired since we don't simulate the real-device case
            # where they can drift via a half-completed OTA.  A MISMATCH
            # badge therefore never fires here; that's by design.
            "asset_version": cfg["fw_ver"],
            "uptime_s":      int(time.monotonic() - BOOT_TIME),
            "ts_unix":       int(t),
            "time_iso":      now.strftime("%Y-%m-%dT%H:%M:%S"),
            "eg1":           0,
        },
        "update_interval_s": cfg.get("status_interval_s", 120),
    }


def _build_history(n: int) -> list[dict]:
    """Return n synthetic readings from oldest → newest.

    Per-row schema mirrors firmware /api/history (1.17.22+): both raw and
    average for T, RH, wind speed, plus the current direction and the
    sliding-window direction variation.  Field names match the keys inside
    the canonical status JSON's `climate` and `wind` blocks so the same
    name carries the same number on /api/status and /api/history."""
    now      = time.time()
    interval = cfg.get("poll_interval_s", 30)
    rows     = []
    for i in range(n - 1, -1, -1):
        ts       = now - i * interval
        temp     = 20.0 + 4.0  * math.sin(ts / 7200)
        temp_avg = 20.0 + 4.0  * math.sin((ts - 60) / 7200)         # slight lag
        rh       = 60   + 10   * math.sin(ts / 10800 + 1.0)
        rh_avg   = 60   + 10   * math.sin((ts - 60) / 10800 + 1.0)
        wind     = max(0.0, 2.5 + 2.0 * math.sin(ts / 900 + 2.0))
        wind_avg = max(0.0, 2.5 + 2.0 * math.sin((ts - 60) / 900 + 2.0))
        wind_dir = int((180 + 90 * math.sin(ts / 3600 + 3.0)) % 360)
        wind_var = int(40 + 35 * (1 + math.sin(ts / 1800 + 0.7)))
        rows.append({
            "ts":                       int(ts),
            "temp_c":                   round(temp,     1),
            "temp_avg_c":               round(temp_avg, 1),
            "rh_pct":                   int(round(rh)),
            "rh_avg_pct":               int(round(rh_avg)),
            "speed_ms":                 round(wind,     1),
            "speed_avg_ms":             round(wind_avg, 1),
            "direction_deg":            wind_dir,
            "direction_variation_deg":  wind_var,
        })
    return rows

# ---------------------------------------------------------------------------
# Static file routes
# ---------------------------------------------------------------------------
@app.route("/")
def index():
    """Serve index.html with `?v=<fw_ver>` injected on app.js / style.css
    references — mirrors firmware/src/web_server/web_server.cpp::serve_lfs()
    behaviour added in 1.17.4 so browsers reliably revalidate static assets
    when the firmware version changes."""
    html = (DATA_DIR / "index.html").read_text(encoding="utf-8")
    ver  = cfg["fw_ver"]
    html = html.replace('app.js"',    f'app.js?v={ver}"')
    html = html.replace('style.css"', f'style.css?v={ver}"')
    resp = make_response(html)
    resp.headers["Content-Type"]  = "text/html; charset=utf-8"
    resp.headers["Cache-Control"] = "no-store"
    return resp


@app.route("/style.css")
def stylesheet():
    return send_from_directory(str(DATA_DIR), "style.css")


@app.route("/app.js")
def appjs():
    return send_from_directory(str(DATA_DIR), "app.js")


@app.route("/manifest.json")
def manifest():
    """Mirrors GET /manifest.json on the real device (added 1.17.8).  In the
    mock asset_version is always equal to the firmware version — there is
    no separate LittleFS partition to drift out of sync.  The endpoint is
    here so the same diagnostic tooling (curl, View Source `<!-- web-assets
    X.Y.Z -->`) works against the mock and the real controller."""
    return {
        "asset_version": cfg["fw_ver"],
        "checksum":      "",
    }

# ---------------------------------------------------------------------------
# Auth routes
# ---------------------------------------------------------------------------
@app.route("/api/whoami", methods=["GET"])
def whoami():
    role = _get_role()
    if not role:
        return {"ok": False}, 401
    return {"ok": True, "role": role}


@app.route("/api/login", methods=["POST"])
def login():
    body     = request.get_json(force=True, silent=True) or {}
    role_req = body.get("role", "farmer")
    pin      = body.get("pin",  "")

    if role_req not in pins:
        return {"ok": False, "locked": False}

    if pin != pins[role_req]:
        return {"ok": False, "locked": False}

    token = _session_create(role_req)
    resp  = make_response({"ok": True, "role": role_req})
    resp.set_cookie("session", token, httponly=True, samesite="Strict", path="/")
    return resp


@app.route("/api/logout", methods=["POST"])
def logout():
    token = request.cookies.get("session", "")
    _session_destroy(token)
    resp = make_response({"ok": True})
    resp.set_cookie("session", "", max_age=0, path="/")
    return resp

# ---------------------------------------------------------------------------
# Status & config routes
# ---------------------------------------------------------------------------
@app.route("/api/status", methods=["GET"])
def status():
    if not _get_role():
        return {"ok": False}, 401
    return _build_status()


# Mirrors the static LIMITS_JSON string in firmware/src/web_server/web_server.cpp
# (see firmware/config/cfg_limits.h — single source of truth).  app.js fetches
# this once at page load and applies min/max to every <input> element.
CONFIG_LIMITS: dict[str, list[int]] = {
    "t_max_day":      [15, 45],
    "t_min_day":      [ 5, 40],
    "t_max_ngt":      [10, 35],
    "t_min_ngt":      [ 0, 30],
    "rh_max_day":     [40, 98],
    "rh_min_day":     [20, 90],
    "rh_max_ngt":     [40, 98],
    "rh_min_ngt":     [20, 90],
    "hyst_t":         [ 2, 15],
    "hyst_rh":        [ 2, 20],
    "avg_win_t":      [ 1, 30],
    "avg_win_rh":     [ 1, 30],
    "v_max":          [ 1, 30],
    "dir_excl_low":   [ 0, 359],
    "dir_excl_high":  [ 0, 359],
    "travel_m1":      [ 5, 300],
    "travel_m2":      [ 5, 300],
    "travel_m3":      [ 5, 300],
    "dwell_open_m1":  [ 0, 1500],   # 1.16.31: ceiling raised so M3 default 1500 round-trips
    "dwell_open_m2":  [ 0, 1500],
    "dwell_open_m3":  [ 0, 1500],
    "dwell_close_m1": [ 0, 1500],   # 1.16.31: matched to dwell_open ceiling
    "dwell_close_m2": [ 0, 1500],
    "dwell_close_m3": [ 0, 1500],
    "poll_interval":  [30, 300],
    "session_timeout":[ 1, 1440],
    "ap_timeout":     [ 0, 1440],
}


@app.route("/api/config/limits", methods=["GET"])
def config_limits_get():
    """Public endpoint (no auth) — same as the firmware web_server."""
    return CONFIG_LIMITS


@app.route("/api/config", methods=["GET"])
def config_get():
    if not _get_role():
        return {"ok": False}, 401
    return cfg


@app.route("/api/config", methods=["POST"])
def config_post():
    role = _get_role()
    if not role:
        return {"ok": False}, 401

    body    = request.get_json(force=True, silent=True) or {}
    ns      = body.get("ns",  "")
    key     = body.get("key", "")
    has_int = "value" in body
    has_str = "str_value" in body

    if not ns or not key or (not has_int and not has_str):
        return {"ok": False, "err": "bad request"}, 400

    if role == "farmer" and (ns, key) not in FARMER_WRITABLE:
        return {"ok": False, "err": "forbidden"}, 403

    mapping = NVS_MAP.get((ns, key))
    if mapping:
        cfg_key, idx = mapping
        value = body["str_value"] if has_str else body["value"]
        if idx is None:
            cfg[cfg_key] = value
        else:
            cfg[cfg_key][idx] = value
    # Keys not in NVS_MAP are silently accepted (forward compatibility)
    return {"ok": True}


@app.route("/api/wifi", methods=["POST"])
def wifi():
    if _get_role() != "admin":
        return {"ok": False, "err": "admin only"}, 403
    body = request.get_json(force=True, silent=True) or {}
    if "ssid" in body:
        cfg["wifi_ssid"] = body["ssid"]
    # psk / ap_psk accepted but not persisted in mock (write-only by design)
    return {"ok": True}


@app.route("/api/pin", methods=["POST"])
def pin_change():
    if _get_role() != "admin":
        return {"ok": False, "err": "admin only"}, 403
    body     = request.get_json(force=True, silent=True) or {}
    role_req = body.get("role", "farmer")
    new_pin  = body.get("pin",  "")
    if role_req not in pins or not new_pin:
        return {"ok": False, "err": "invalid"}
    pins[role_req] = new_pin
    return {"ok": True}

# ---------------------------------------------------------------------------
# Status-website (Web tab) routes — added in firmware 1.17.0.
#
# Validation rules mirror firmware/src/web_server/web_server.cpp::/api/web
# POST handler EXACTLY so the GUI gets identical 400 errors against the
# mock and the real device.
# ---------------------------------------------------------------------------
@app.route("/api/web", methods=["GET"])
def web_get():
    if _get_role() != "admin":
        return {"ok": False, "err": "admin only"}, 403
    # Secret is intentionally never echoed (matches firmware).  The UI uses
    # an empty input + "send empty to keep" idiom.
    return {
        "url":         cfg["status_url"],
        "interval_s":  cfg["status_interval_s"],
        "enable":      cfg["status_enable"],
        "expose":      cfg["status_expose"],
        "log_h":       cfg["log_upload_h"],
        "log_m":       cfg["log_upload_m"],
        "log_rot":     cfg["log_upload_rot"],
        "last_post":   "",  # T14 last-attempt indicator; no T14 in mock
        "last_log_up": "",
        "log_last_up": cfg["log_last_up"],
    }


@app.route("/api/web", methods=["POST"])
def web_post():
    if _get_role() != "admin":
        return {"ok": False, "err": "admin only"}, 403
    body = request.get_json(force=True, silent=True) or {}

    # URL: empty disables the feature; non-empty must be http(s)://, must not
    # contain ? or #, and must end with "api.php".  Mirrors firmware checks.
    if "url" in body:
        url = (body.get("url") or "").strip()
        if url:
            if not (url.startswith("http://") or url.startswith("https://")):
                return {"ok": False, "err": "URL must start with http:// or https://"}, 400
            if "?" in url or "#" in url:
                return {"ok": False, "err": "URL must not contain ? or #"}, 400
            if not url.endswith("api.php"):
                return {"ok": False, "err": 'URL must end with "api.php"'}, 400
        cfg["status_url"] = url

    # Secret: empty = unchanged.  Non-empty must be ≥ 16 chars
    # (CFG_MIN_SECRET_LEN in firmware/config/cfg_limits.h).
    if "secret" in body:
        secret = body.get("secret") or ""
        if secret:
            if len(secret) < 16:
                return {"ok": False, "err": "secret too short"}, 400
            cfg["status_secret"] = secret

    if "interval_s" in body:
        try:
            iv = int(body["interval_s"])
        except (TypeError, ValueError):
            return {"ok": False, "err": "interval out of range"}, 400
        if iv < 60 or iv > 300:
            return {"ok": False, "err": "interval out of range"}, 400
        cfg["status_interval_s"] = iv

    if "enable" in body:
        v = int(body["enable"])
        if v not in (0, 1):
            return {"ok": False, "err": "bounds"}, 400
        cfg["status_enable"] = v

    if "expose" in body:
        v = int(body["expose"])
        if v < 0 or v > 0x3F:
            return {"ok": False, "err": "bounds"}, 400
        cfg["status_expose"] = v

    if "log_h" in body:
        v = int(body["log_h"])
        if v < 0 or v > 23:
            return {"ok": False, "err": "bounds"}, 400
        cfg["log_upload_h"] = v

    if "log_m" in body:
        v = int(body["log_m"])
        if v < 0 or v > 59:
            return {"ok": False, "err": "bounds"}, 400
        cfg["log_upload_m"] = v

    if "log_rot" in body:
        v = int(body["log_rot"])
        if v not in (0, 1):
            return {"ok": False, "err": "bounds"}, 400
        cfg["log_upload_rot"] = v

    return {"ok": True}

# ---------------------------------------------------------------------------
# History route
# ---------------------------------------------------------------------------
@app.route("/api/history", methods=["GET"])
def history():
    if not _get_role():
        return {"ok": False}, 401
    n = min(int(request.args.get("n", 60)), 60)
    return {"rows": _build_history(n)}

# ---------------------------------------------------------------------------
# SD card routes
# ---------------------------------------------------------------------------
@app.route("/api/sd/status", methods=["GET"])
def sd_status():
    if not _get_role():
        return {"ok": False}, 401
    return {
        "mounted": sd["mounted"],
        "free_mb": sd["free_mb"] if sd["mounted"] else 0,
        "size_mb": sd["size_mb"] if sd["mounted"] else 0,
    }


@app.route("/api/sd/mount", methods=["POST"])
def sd_mount():
    if _get_role() != "admin":
        return {"ok": False, "err": "admin only"}, 403
    sd["mounted"] = True
    return {"ok": True}


@app.route("/api/sd/unmount", methods=["POST"])
def sd_unmount():
    if _get_role() != "admin":
        return {"ok": False, "err": "admin only"}, 403
    sd["mounted"] = False
    return {"ok": True}

# ---------------------------------------------------------------------------
# Log routes
# ---------------------------------------------------------------------------
@app.route("/api/log/files", methods=["GET"])
def log_files():
    if _get_role() != "admin":
        return {"ok": False, "err": "admin only"}, 403
    entries   = _nvs_log_entries()
    sd_files  = _sd_log_files() if sd["mounted"] else []
    return {"nvs_count": len(entries), "sd_files": sd_files}


@app.route("/api/log/download", methods=["GET"])
def log_download():
    if _get_role() != "admin":
        return {"ok": False, "err": "admin only"}, 403

    src = request.args.get("src", "")

    if src == "nvs":
        entries = _nvs_log_entries()
        lines   = ["timestamp,type,initiator,ch,param,value_a,value_b"]
        for e in entries:
            ts_str = time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(e["ts"]))
            lines.append(
                f"{ts_str},{e['type']},{e['initiator']},"
                f"{e['ch']},{e['param']},{e['value_a']},{e['value_b']}"
            )
        csv_text = "\n".join(lines) + "\n"
        resp = make_response(csv_text)
        resp.headers["Content-Type"]        = "text/csv; charset=utf-8"
        resp.headers["Content-Disposition"] = 'attachment; filename="nvs_log.csv"'
        return resp

    if src == "sd":
        filename = request.args.get("file", "")
        # Path-traversal guard (mirrors firmware)
        if not filename or "/" in filename or ".." in filename:
            return {"ok": False, "err": "invalid filename"}, 400
        if not sd["mounted"]:
            return {"ok": False, "err": "SD not mounted"}, 503
        if filename not in _sd_log_files():
            return {"ok": False, "err": "file not found"}, 404
        csv_text = _sd_csv_content(filename)
        resp = make_response(csv_text)
        resp.headers["Content-Type"]        = "text/csv; charset=utf-8"
        resp.headers["Content-Disposition"] = f'attachment; filename="{filename}"'
        return resp

    return {"ok": False, "err": "invalid src"}, 400

# ---------------------------------------------------------------------------
# OTA routes
# ---------------------------------------------------------------------------
def _ota_simulate_firmware(content_length: int) -> None:
    """Simulate firmware upload: fw_writing -> fw_verifying -> fw_done (no reboot yet)."""
    steps = 20
    delay = max(0.05, min(0.3, content_length / (steps * 200_000)))
    for i in range(1, steps + 1):
        time.sleep(delay)
        with ota_lock:
            if ota["state"] == "error":
                return
            ota["state"]    = "fw_writing"
            ota["progress"] = int(i * 100 / steps)
    with ota_lock:
        ota["state"]    = "fw_verifying"
        ota["progress"] = 100
    time.sleep(0.3)
    with ota_lock:
        ota["state"]    = "fw_done"
        ota["progress"] = 100
    # Stay in fw_done; a real device waits up to 120 s for assets.
    # Mock idles back after 5 s if no assets upload follows.
    time.sleep(5)
    with ota_lock:
        if ota["state"] == "fw_done":
            ota["state"]    = "idle"
            ota["progress"] = 0


def _ota_simulate_assets(content_length: int) -> None:
    """Simulate asset upload: assets_buffering -> assets_writing -> rebooting -> idle."""
    steps = 20
    delay = max(0.05, min(0.3, content_length / (steps * 200_000)))
    for i in range(1, steps + 1):
        time.sleep(delay)
        with ota_lock:
            if ota["state"] == "error":
                return
            ota["state"]    = "assets_buffering"
            ota["progress"] = int(i * 100 / steps)
    with ota_lock:
        ota["state"]    = "assets_writing"
        ota["progress"] = 0
    for i in range(1, 11):
        time.sleep(0.2)
        with ota_lock:
            if ota["state"] == "error":
                return
            ota["progress"] = i * 10
    with ota_lock:
        ota["state"]    = "rebooting"
        ota["progress"] = 100
    time.sleep(1)
    with ota_lock:
        # Simulate bank flip and brief "not yet accepted" window
        ota["bank"]     = "B" if ota["bank"] == "A" else "A"
        ota["accepted"] = False
        ota["state"]    = "idle"
        ota["progress"] = 0
    time.sleep(5)   # simulate 30 s healthy-boot window (compressed to 5 s for mock)
    with ota_lock:
        ota["accepted"] = True


@app.route("/api/ota/status", methods=["GET"])
def ota_status():
    if not _get_role():
        return {"ok": False}, 401
    with ota_lock:
        return {"ok": True, "state": ota["state"],
                "progress": ota["progress"], "error": ota["error"],
                "bank": ota["bank"], "accepted": ota["accepted"]}


@app.route("/api/ota/firmware", methods=["POST"])
def ota_firmware():
    if _get_role() != "admin":
        return {"ok": False, "err": "admin only"}, 403
    content_length = request.content_length or 512 * 1024
    with ota_lock:
        if ota["state"] not in ("idle", "error"):
            return {"ok": False, "err": "OTA already in progress"}, 409
        ota["state"]    = "fw_begin"
        ota["progress"] = 0
        ota["error"]    = ""
    # Consume request body so Flask doesn't complain
    _ = request.get_data()
    threading.Thread(
        target=_ota_simulate_firmware,
        args=(content_length,),
        daemon=True,
    ).start()
    return {"ok": True, "rebooting": False, "awaiting_assets": True}


@app.route("/api/ota/assets", methods=["POST"])
def ota_assets():
    if _get_role() != "admin":
        return {"ok": False, "err": "admin only"}, 403
    content_length = request.content_length or 128 * 1024
    with ota_lock:
        if ota["state"] not in ("idle", "error", "fw_done"):
            return {"ok": False, "err": "OTA already in progress"}, 409
        ota["state"]    = "assets_buffering"
        ota["progress"] = 0
        ota["error"]    = ""
    _ = request.get_data()
    threading.Thread(
        target=_ota_simulate_assets,
        args=(content_length,),
        daemon=True,
    ).start()
    resp = make_response(
        {"ok": True, "message": "extracting — poll GET /api/ota/status"}, 202
    )
    return resp

# ---------------------------------------------------------------------------
# WebSocket
# ---------------------------------------------------------------------------
@sock.route("/ws")
def ws_push(ws):
    """Push a status JSON payload every 2 seconds until the client disconnects."""
    while True:
        payload = json.dumps(_build_status())
        try:
            ws.send(payload)
        except Exception:
            break
        time.sleep(2)

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    if not DATA_DIR.exists():
        print(f"\nERROR: firmware/data/ not found at expected path:\n  {DATA_DIR}")
        print("Run mock_server.py from the webUiMock/ directory, or ensure the")
        print("firmware/data/ directory exists relative to the project root.\n")
        raise SystemExit(1)

    print(f"\nGreenhouse Controller — Web UI Mock Server")
    print(f"  Serving static files from : {DATA_DIR}")
    print(f"  Listening on              : http://0.0.0.0:5000")
    print(f"  Farmer PIN                : {pins['farmer']}")
    print(f"  Admin  PIN                : {pins['admin']}")
    print(f"\nOpen http://localhost:5000 in your browser.\nPress Ctrl+C to stop.\n")

    app.run(host="0.0.0.0", port=5000, debug=False)
