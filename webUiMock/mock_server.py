#!/usr/bin/env python3
"""
Greenhouse Controller — Web UI Mock Server
==========================================
Serves the static files from firmware/data/ and emulates all REST and
WebSocket endpoints that the real ESP32 firmware (web_server.cpp) exposes.

Endpoints emulated
------------------
GET  /                  index.html from firmware/data/
GET  /style.css         stylesheet
GET  /app.js            JavaScript
GET  /api/whoami        check cookie validity → {ok, role}
POST /api/login         {role, pin} → {ok, role}
POST /api/logout        → {ok:true} + clears cookie
GET  /api/status        full status JSON (same payload as WebSocket push)
GET  /api/config        all configuration parameters
POST /api/config        {ns, key, value} or {ns, key, str_value}
POST /api/wifi          {ssid, psk} or {ap_psk}
POST /api/pin           {role, pin}  (admin only)
GET  /api/history       ?n=N  — last N synthetic sensor readings
GET  /api/sd/status     {mounted, free_mb, size_mb}
POST /api/sd/mount      mount SD card (admin only)
POST /api/sd/unmount    unmount SD card (admin only)
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

# NVS config state — mirrors build_config_json() in web_server.cpp
cfg: dict = {
    "wifi_ssid":           "MyNetwork",
    "ap_ssid":             "Greenhouse-AABB",
    "t_max_day":           28,
    "t_min_day":           18,
    "t_max_ngt":           22,
    "t_min_ngt":           14,
    "rh_max_day":          80,
    "rh_min_day":          40,
    "rh_max_ngt":          85,
    "rh_min_ngt":          45,
    "hyst_t":               2,
    "hyst_rh":              5,
    "rh_ctrl_en":           1,
    "cr_priority":          0,
    "avg_win_t":            5,
    "avg_win_rh":           5,
    "v_max":                8,
    "wind_prot_en":         1,
    "dir_excl_low":         0,
    "dir_excl_high":        0,
    "travel_s":            [45, 45, 45],
    "dwell_open_min":      [ 5,  5,  5],
    "dwell_close_min":     [ 5,  5,  5],
    "poll_interval_s":     30,
    "session_timeout_min": 30,
    "ap_timeout_min":      10,
    "lat_deg":             52,
    "lat_frac":           100,
    "lon_deg":              4,
    "lon_frac":           300,
    "tz_str":              "CET-1CEST,M3.5.0,M10.5.0/3",
    "fw_ver":              "1.14.0",
}

sd: dict = {"mounted": True, "size_mb": 7500, "free_mb": 7100}

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
def _sensor_now() -> dict:
    t = time.time()
    temp     = 20.0 + 4.0  * math.sin(t / 7200)
    rh       = 60   + 10   * math.sin(t / 10800 + 1.0)
    wind     = max(0.0, 2.5 + 2.0 * math.sin(t / 900 + 2.0))
    wind_dir = int((180 + 90 * math.sin(t / 3600 + 3.0)) % 360)
    return {
        "temp_c":       round(temp, 1),
        "temp_avg":     round(temp, 1),
        "rh_pct":       int(round(rh)),
        "rh_avg":       int(round(rh)),
        "wind_ms":      round(wind, 1),
        "wind_dir":     wind_dir,
        "wind_avg":     round(wind, 1),
        "wind_avg_dir": wind_dir,
    }


def _build_status() -> dict:
    now       = datetime.now()
    cur_mins  = now.hour * 60 + now.minute
    sunrise   = 6 * 60 + 30    # 06:30 UTC
    sunset    = 20 * 60 + 30   # 20:30 UTC
    return {
        "type": "status",
        **_sensor_now(),
        "windows":     ["CLOSED", "CLOSED", "CLOSED"],
        "mode":        "AUTOMATIC",
        "is_daytime":  sunrise <= cur_mins <= sunset,
        "sunrise_utc": sunrise,
        "sunset_utc":  sunset,
        "eg1":         0,
        "time":        now.strftime("%Y-%m-%dT%H:%M:%S"),
        "ntp_synced":  True,
        "wifi_ip":     "192.168.1.100",
        "wifi_rssi":   -65,
        "fw_ver":      cfg["fw_ver"],
    }


def _build_history(n: int) -> list[dict]:
    """Return n synthetic readings from oldest → newest."""
    now      = time.time()
    interval = cfg.get("poll_interval_s", 30)
    rows     = []
    for i in range(n - 1, -1, -1):
        ts       = now - i * interval
        temp     = 20.0 + 4.0  * math.sin(ts / 7200)
        rh       = 60   + 10   * math.sin(ts / 10800 + 1.0)
        wind     = max(0.0, 2.5 + 2.0 * math.sin(ts / 900 + 2.0))
        wind_dir = int((180 + 90 * math.sin(ts / 3600 + 3.0)) % 360)
        rows.append({
            "ts":       int(ts),
            "temp_c":   round(temp, 1),
            "rh_pct":   int(round(rh)),
            "wind_ms":  round(wind, 1),
            "wind_dir": wind_dir,
        })
    return rows

# ---------------------------------------------------------------------------
# Static file routes
# ---------------------------------------------------------------------------
@app.route("/")
def index():
    return send_from_directory(str(DATA_DIR), "index.html")


@app.route("/style.css")
def stylesheet():
    return send_from_directory(str(DATA_DIR), "style.css")


@app.route("/app.js")
def appjs():
    return send_from_directory(str(DATA_DIR), "app.js")

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
