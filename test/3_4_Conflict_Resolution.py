#!/usr/bin/env python3
"""
3_4_Conflict_Resolution.py
Greenhouse Ventilation Controller — Conflict Resolution test

Covers: UT-CC-020, UT-CC-021, UT-CC-022, UT-CC-030, UT-CC-031
(softwareTestPlan.md §6.4), against the law in force since firmware 2.15.0:
`stepped` v2 (gh#84, drivers/ventModel/src/vent_model_stepped.cpp).

vent_resolve_conflict() in stepped v2, first match wins:
  1. RH casts no vote (inside its band, or RH control off) → step_t
  2. both want OPEN                    → max(step_t, step_rh), any priority  (CC-022a)
  3. both ask for the same step        → that step                           (CC-022b)
  4. T wants OPEN, RH votes 0 (too dry) → step_t under EVERY cr_priority:
     dryness never closes against heat, so rh_min is inert                   (CC-020, CC-030)
  5. what is left, RH alone wanting OPEN:
       cr_priority 0     → 0 (temperature first)                             (CC-021, CC-031f)
       cr_priority 1 / 2 → M1 at most, and only from t_min + 2 °C; an
                           opening already live holds down to t_min + 1 °C   (CC-031a-e)
A live humidity vote also holds at step 1 until RH <= rh_max − max(hyst_rh/3, 1),
the close guard. The host tests in drivers/ventModel (`pio test -e native`) pin
every rule, that one included; this script is the device-level view.

RUNNING IT CHANGES THE RIG, so it needs the operator's go. It rewrites about 30
settings (restored and read back at the end) and pushes values into the sensor
emulator, which Node-RED on Shuttle2 normally feeds with 5C88's live readings
every 60 s. Stop that feed for the run and restart it afterwards: the script
stops with exit code 2, before touching the unit's settings, if the emulator
does not hold a pushed value. Not during a soak.

See 3_4_Conflict_Resolution.md for full instructions.
Results written to 3_4_Conflict_Resolution.log.
"""

import os
import sys
import time
import logging
import requests

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

# The dev rig takes ONE of two swappable modules at a time, each on a fixed
# DHCP reservation, so the address says which module answers. Only the fitted
# one is powered: the other not answering is expected. GH_DEVICE_BASE, when
# set, is used as given instead of the search.
RIG_MODULES = (
    ("2344", "http://192.168.20.160"),
    ("FDA4", "http://192.168.20.169"),
)
PRODUCTION_UNIT = "5C88"   # never run against production

DEVICE_BASE   = os.getenv("GH_DEVICE_BASE",   "")   # resolved in main()
EMULATOR_BASE = os.getenv("GH_EMULATOR_BASE",  "http://192.168.20.226")
ADMIN_PIN     = os.getenv("GH_ADMIN_PIN",      "12345678")

# The law these expectations are written for (GET /api/status windows.law).
EXPECTED_LAW = "stepped v2"

# Fast-test parameters written to NVS for the duration of the test.
# Restored unconditionally in teardown.
TEST_POLL_S     = 30   # system/poll_interval  (s)
TEST_TRAVEL_S   = 5    # motor/travel_m1 and travel_m2 (s), the minimum allowed.
                       # travel_m3 is left alone: it belongs to the rig (13 s for
                       # the dev rig's test window), and a shorter value stops M3
                       # short of its end. The motor wait uses the longer of the two.
TEST_AVG_WIN    = 1    # climate/avg_win_t and avg_win_rh (min), the minimum
                       # allowed: cfg_limits.h clamps 0 to 1. At a 30 s poll the
                       # window holds 2 samples, so the first poll after a push
                       # reads (old + new) / 2; push_and_verify_sensor() pushes
                       # again until the average has converged on the new value.

# Timing margins
POLL_MARGIN_S            = 5    # extra seconds beyond poll interval for sensor read
MOTOR_MARGIN_S           = 5    # extra seconds after relay de-energises
FIRMWARE_TRAVEL_MARGIN_S = 5    # T2 adds this to every relay pulse (MOTOR_TRAVEL_MARGIN_S_DEFAULT)
NVS_SETTLE_S             = 2    # wait after POST /api/config before next action
MODE_LIMIT_S             = 150  # T6 re-decides M3's effective mode once per wake

# Derived wait times
# WAIT_FOR_SENSOR_S: push → wait → verify controller has new reading
WAIT_FOR_SENSOR_S = TEST_POLL_S + POLL_MARGIN_S                                      # 35 s


def motor_wait_s(travel_s: int) -> int:
    """Push → window state settled: a poll, the relay pulse and a margin."""
    return TEST_POLL_S + travel_s + FIRMWARE_TRAVEL_MARGIN_S + MOTOR_MARGIN_S


# WAIT_FOR_MOTOR_S: push → wait → window state settled at new position.
# Recomputed in main() from the unit's own travel_m3: 53 s on the dev rig.
WAIT_FOR_MOTOR_S = motor_wait_s(TEST_TRAVEL_S)

# Maximum times to retry a sensor push before giving up on the test case
MAX_SENSOR_RETRIES = 2

# The status carries the averages in 0.1 °C and whole %, so only a converged
# 2-sample window matches: a 1 °C step leaves the first poll 0.5 °C off.
T_TOL_C    = 0.3
RH_TOL_PCT = 1

# Every push carries this wind direction as a watermark. The unit reports the
# raw direction one poll later, and the Node-RED feed would replace it with
# 5C88's, so a unit reading anything else means something else is writing to
# the emulator and the reading is void (gotcha 2026-07-28).
WATERMARK_DIR = 312   # °
FEED_PERIOD_S = 60    # the Node-RED feed's interval

# ---------------------------------------------------------------------------
# Climate setpoints used for all conflict tests
# ---------------------------------------------------------------------------
#
# t_max_day = 25 °C,  hyst_t = 6 °C → step_width = hyst_t // NUM_VENT_STEPS = 2
#
# Temperature step (step_from_deviation):
#   T = 26 °C: deviation = 1,  ceil(1/2) = 1  → step_t = 1 (M1 only)
#   T = 28 °C: deviation = 3,  ceil(3/2) = 2  → step_t = 2 (M1+M2)
#   T ≤ 25 °C from closed      → step_t = 0 (an open step holds at 1 down to 19 °C)
#
# RH step (vent_step_required_rh):
#   RH = 80 %: above rh_max=70 → step_rh = 5, clamped to NUM_VENT_STEPS=3
#   RH = 35 %: below rh_min=40 → step_rh = 0, a "too dry" vote that since v2
#              changes no decision (rule 4)
#   RH = 55 %: inside [rh_min, rh_max] → no RH vote (VENT_STEP_NONE)
#
# Humidity floor (v2): t_min_day = 12 °C, so humidity may open M1 on its own
# from 14 °C, and an opening it made holds down to 13 °C. T6 compares whole
# degrees of the average (t_avg_c, rounded half away from zero).
#
T_MAX_DAY  = 25   # °C
HYST_T     = 6    # °C
T_MIN_DAY  = 12   # °C — the base of the humidity floor
T_OPEN_S1  = 26   # °C — step_t = 1
T_OPEN_S2  = 28   # °C — step_t = 2
T_CLOSE    = 10   # °C — step_t = 0, and below the humidity floor
T_NEUTRAL  = 10   # °C — same as T_CLOSE; with RH_NEUTRAL every window closes

T_FLOOR       = T_MIN_DAY + 2   # 14 °C — humidity alone may open M1 from here
T_FLOOR_HOLD  = T_MIN_DAY + 1   # 13 °C — a live humidity opening holds; a new one does not start
T_BELOW_FLOOR = T_MIN_DAY       # 12 °C — below the hold: the humidity opening ends

RH_MAX_DAY = 70   # %
RH_MIN_DAY = 40   # %
HYST_RH    = 6    # %
RH_OPEN    = 80   # % — step_rh = 3
RH_DRY     = 35   # % — step_rh = 0 (too dry)
RH_NEUTRAL = 55   # % — no RH vote (inside the band)

# cr_priority (climate/cr_priority): two choices since 2.15.0 (FRS FR-CR03)
CR_TEMP_FIRST  = 0   # temperature first: humidity never opens a window on its own
CR_RH_MAY_OPEN = 1   # humidity may also open M1, from t_min + 2 °C
CR_STORED_2    = 2   # the old "deviation-based": accepted, behaves exactly as 1

# ---------------------------------------------------------------------------
# Forcing is_daytime through the location (see daytime_candidates())
# ---------------------------------------------------------------------------
LAT_POLE         = 89   # in polar day or night for months, except near an equinox
LAT_NEAR_EQUATOR = 1    # never 0: lat 0 with lon 0 means "no location", read as day (FR-DN05)

_DIR     = os.path.dirname(os.path.abspath(__file__))
LOG_PATH = os.path.join(_DIR, "3_4_Conflict_Resolution.log")

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

def _make_logger() -> logging.Logger:
    log = logging.getLogger("conflict_test")
    log.setLevel(logging.DEBUG)
    fmt = logging.Formatter(
        "%(asctime)s  %(levelname)-5s  %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )
    fh = logging.FileHandler(LOG_PATH, mode="w", encoding="utf-8")
    fh.setFormatter(fmt)
    ch = logging.StreamHandler(sys.stdout)
    ch.setFormatter(fmt)
    log.addHandler(fh)
    log.addHandler(ch)
    return log

log = _make_logger()

# ---------------------------------------------------------------------------
# Device REST helpers
# ---------------------------------------------------------------------------

def do_login(session: requests.Session, role: str, pin: str) -> dict:
    """POST /api/login; returns the parsed JSON body."""
    r = session.post(
        f"{DEVICE_BASE}/api/login",
        json={"role": role, "pin": pin},
        timeout=10,
    )
    r.raise_for_status()
    return r.json()


_WRITE_CONFIG_RETRIES   = 3   # maximum retry attempts on transient HTTP errors
_WRITE_CONFIG_RETRY_S   = 3   # seconds to wait between retries


def write_config(session: requests.Session, ns: str, key: str, value: int) -> None:
    """POST /api/config (integer); raises RuntimeError on rejection.

    Retries up to _WRITE_CONFIG_RETRIES times on transient HTTP errors (5xx /
    connection errors) before propagating the exception.  Application-level
    rejections (ok=false in the response body) are never retried.
    On 401 Unauthorized the session is silently re-authenticated and the
    write retried immediately (no sleep) — this handles the ~20-minute session
    expiry that occurs when only unauthenticated GET /api/status calls are made
    between setup writes and later test writes.
    """
    last_exc: Exception | None = None
    for attempt in range(_WRITE_CONFIG_RETRIES + 1):
        try:
            r = session.post(
                f"{DEVICE_BASE}/api/config",
                json={"ns": ns, "key": key, "value": value},
                timeout=10,
            )
            r.raise_for_status()
            body = r.json()
            if not body.get("ok"):
                raise RuntimeError(f"Config write {ns}/{key}={value} rejected: {body}")
            return  # success
        except RuntimeError:
            raise  # application rejection — do not retry
        except Exception as exc:
            last_exc = exc
            if attempt >= _WRITE_CONFIG_RETRIES:
                break  # all retries exhausted
            # Special handling for 401: re-authenticate then retry immediately
            is_401 = (
                isinstance(exc, requests.exceptions.HTTPError)
                and exc.response is not None
                and exc.response.status_code == 401
            )
            if is_401:
                log.warning(
                    f"write_config {ns}/{key}={value} failed "
                    f"(attempt {attempt + 1}/{_WRITE_CONFIG_RETRIES + 1}): "
                    f"401 Unauthorized - re-authenticating ..."
                )
                try:
                    body = do_login(session, "admin", ADMIN_PIN)
                    if not body.get("ok"):
                        log.warning(f"write_config: re-login returned {body}")
                    else:
                        log.info("write_config: session restored")
                except Exception as reauth_exc:
                    log.warning(f"write_config: re-login failed - {reauth_exc}")
                # No sleep after re-auth — retry immediately
            else:
                log.warning(
                    f"write_config {ns}/{key}={value} failed "
                    f"(attempt {attempt + 1}/{_WRITE_CONFIG_RETRIES + 1}): {exc} - "
                    f"retrying in {_WRITE_CONFIG_RETRY_S} s ..."
                )
                time.sleep(_WRITE_CONFIG_RETRY_S)
    raise last_exc  # type: ignore[misc]


def get_status(session: requests.Session) -> dict:
    """GET /api/status; returns the parsed JSON body."""
    r = session.get(f"{DEVICE_BASE}/api/status", timeout=10)
    r.raise_for_status()
    return r.json()


def get_config(session: requests.Session) -> dict:
    """GET /api/config; returns the parsed JSON body."""
    r = session.get(f"{DEVICE_BASE}/api/config", timeout=10)
    r.raise_for_status()
    return r.json()


def get_admin_session() -> requests.Session:
    """Create a new session and authenticate as admin."""
    s = requests.Session()
    body = do_login(s, "admin", ADMIN_PIN)
    if not body.get("ok"):
        raise RuntimeError(f"Admin login failed: {body}")
    log.info("Admin session established")
    return s

# ---------------------------------------------------------------------------
# Canonical status accessors
#
# GET /api/status is build_canonical_status_json() (status_json.cpp): nested
# blocks, not the flat 1.x payload this test was first written against.
#   climate.temp_c / temp_avg_c (0.1 °C), climate.rh_pct / rh_avg_pct (whole %)
#   wind.direction_deg (raw), sun.is_daytime, mode.current,
#   system.unit_id / fw_ver / ts_unix
#   windows: an OBJECT keyed M1/M2/M3, plus M3_ctrl_mode, law, M3_ctrl_reason,
#   M3_pos_gate and, while M3's position is trusted, M3_percent_x10 and more.
#   Iterating it yields key names, so only the three state keys are read.
# ---------------------------------------------------------------------------

WINDOW_KEYS = ("M1", "M2", "M3")
OPENING     = ("OPEN", "MOVING_OPEN")
CLOSING     = ("CLOSED", "MOVING_CLOSE")


def block(status: dict, name: str) -> dict:
    """One nested block of the status; {} when it is absent."""
    b = status.get(name)
    return b if isinstance(b, dict) else {}


def win_states(status: dict) -> dict:
    """{"M1": state, "M2": state, "M3": state}; a missing key reads UNKNOWN."""
    w = block(status, "windows")
    return {k: str(w.get(k, "UNKNOWN")) for k in WINDOW_KEYS}


def is_daytime(status: dict):
    return block(status, "sun").get("is_daytime")


def mode_current(status: dict) -> str:
    return str(block(status, "mode").get("current", "?"))

# ---------------------------------------------------------------------------
# Sensor emulator helpers
# ---------------------------------------------------------------------------

def set_rest_mode() -> None:
    """Configure both sensors for REST/emulator mode (mode=3)."""
    for sensor in ("fg6485a", "s200"):
        r = requests.post(
            f"{EMULATOR_BASE}/config/sensor",
            json={"sensor": sensor, "mode": 3},
            timeout=10,
        )
        r.raise_for_status()
    log.info("Sensor emulator: fg6485a and s200 set to REST mode")


def push_sensors(
    T=None, RH=None, Speed: float = 0.5, Direction: float = WATERMARK_DIR
) -> dict:
    """POST /api/data to sensor emulator; returns response JSON."""
    body: dict = {"Speed": float(Speed), "Direction": float(Direction)}
    if T   is not None: body["T"]  = float(T)
    if RH  is not None: body["RH"] = float(RH)
    r = requests.post(f"{EMULATOR_BASE}/api/data", json=body, timeout=10)
    r.raise_for_status()
    return r.json()

# ---------------------------------------------------------------------------
# Test helpers
# ---------------------------------------------------------------------------

def mismatches(status: dict, T=None, RH=None, averaged: bool = True) -> list[str]:
    """What the unit reads that differs from the last push; [] when it matches.

    averaged=True compares the sliding averages, which are what T6 decides on;
    False compares the raw readings, which follow a push within one poll
    whatever the averaging window is. The wind direction is always the raw one:
    it is the watermark (WATERMARK_DIR).
    """
    c = block(status, "climate")
    out = []
    if T is not None:
        t = c.get("temp_avg_c" if averaged else "temp_c")
        if t is None or abs(float(t) - T) > T_TOL_C:
            out.append(f"T {t} C (pushed {T})")
    if RH is not None:
        rh = c.get("rh_avg_pct" if averaged else "rh_pct")
        if rh is None or abs(int(rh) - int(RH)) > RH_TOL_PCT:
            out.append(f"RH {rh} % (pushed {RH})")
    d = block(status, "wind").get("direction_deg")
    if d is None or abs(int(d) - WATERMARK_DIR) > 1:
        out.append(f"wind direction {d} (pushed {WATERMARK_DIR}: is something else "
                   f"writing to the emulator?)")
    return out


def push_and_verify_sensor(
    session: requests.Session,
    T=None,
    RH=None,
    poll_s: int = TEST_POLL_S,
    averaged: bool = True,
) -> bool:
    """
    Push sensor values to the emulator, wait poll_s + POLL_MARGIN_S, then verify
    the controller actually read the pushed values from GET /api/status.

    On mismatch the push + wait cycle is repeated up to MAX_SENSOR_RETRIES times;
    after a change the first attempt normally mismatches (see TEST_AVG_WIN).
    Returns True when the controller's status reflects the pushed values;
    returns False (the caller records the case INCONCLUSIVE) when all retries
    are exhausted.

    Tolerances: T_TOL_C and RH_TOL_PCT, tight enough that only a converged
    average passes, because T6 rounds the average to whole degrees.
    """
    wait_s = poll_s + POLL_MARGIN_S
    for attempt in range(MAX_SENSOR_RETRIES + 1):
        if attempt > 0:
            log.info(f"  Not converged yet (attempt {attempt}); pushing again ...")
        push_sensors(T=T, RH=RH)
        log.info(f"  Pushed T={T} C RH={RH} % - waiting {wait_s} s for a poll ...")
        time.sleep(wait_s)

        status = get_status(session)
        off = mismatches(status, T=T, RH=RH, averaged=averaged)
        if not off:
            c = block(status, "climate")
            log.info(
                f"  Sensor confirmed: T_avg={c.get('temp_avg_c')} C "
                f"RH_avg={c.get('rh_avg_pct')} %"
            )
            return True
        log.info("  Unit reads " + "; ".join(off))

    log.error(
        f"  Sensor NOT confirmed after {MAX_SENSOR_RETRIES + 1} attempts - "
        f"giving up on this case"
    )
    return False


def windows_all_closed(status: dict) -> bool:
    return all(s == "CLOSED" for s in win_states(status).values())


def windows_all_closing(status: dict) -> bool:
    """True when every window is CLOSED or MOVING_CLOSE: the close was commanded."""
    return all(s in CLOSING for s in win_states(status).values())


def windows_exactly(status: dict, *open_keys: str) -> bool:
    """True when the windows named are OPEN or MOVING_OPEN and every other
    one is CLOSED."""
    w = win_states(status)
    return all((w[k] in OPENING) if k in open_keys else (w[k] == "CLOSED")
               for k in WINDOW_KEYS)


def wins_str(status: dict) -> str:
    return " ".join(f"{k}={v}" for k, v in win_states(status).items())


def force_windows_closed(session: requests.Session) -> bool:
    """
    Push T=T_NEUTRAL (below every close threshold) and RH=RH_NEUTRAL (inside
    the RH band → no RH vote → rule 1 → step = step_t = 0).
    Waits for all windows to reach CLOSED or MOVING_CLOSE.

    Using RH=RH_NEUTRAL makes this helper safe under any cr_priority: with no
    RH vote rule 1 decides before cr_priority is consulted. It also ends a
    live humidity opening, so the next case starts with nothing to hold.

    Returns True within two poll+travel cycles, False otherwise.
    """
    log.info(f"  force_windows_closed: pushing T={T_NEUTRAL} C RH={RH_NEUTRAL} % ...")
    push_sensors(T=T_NEUTRAL, RH=RH_NEUTRAL)
    time.sleep(WAIT_FOR_MOTOR_S)
    status = get_status(session)
    if windows_all_closed(status):
        log.info(f"  All windows CLOSED: {wins_str(status)}")
        return True
    if windows_all_closing(status):
        log.info(f"  All windows CLOSED/MOVING_CLOSE: {wins_str(status)}")
        return True
    # One extra cycle in case a motor was still travelling
    push_sensors(T=T_NEUTRAL, RH=RH_NEUTRAL)
    time.sleep(WAIT_FOR_MOTOR_S)
    status = get_status(session)
    if windows_all_closing(status):
        log.info(f"  All windows CLOSED/MOVING_CLOSE after extra cycle: {wins_str(status)}")
        return True
    log.warning(f"  Windows not fully closed: {wins_str(status)}")
    return False


def daytime_candidates(daytime: bool, unit_ts: int) -> list[tuple[int, int]]:
    """(lat_deg, lon_deg) locations that should give the wanted is_daytime, best first.

    T4 derives is_daytime from the location and ITS clock (sunrise.cpp), and
    clamps sunrise and sunset to the UTC day instead of wrapping them:
      - night: 1° N at the longitude where it is solar midnight now. At any
        date and hour that stays night for at least five hours.
      - day: the pole in polar day (the north one from the March equinox to
        the September one), which holds for months. Within about a day of an
        equinox neither pole is, so 1° N at solar noon follows: day until the
        UTC day ends.
    The old fixed 89° N (day) / 89° S (night) was right from March to September
    only: on 2026-09-27 89° N had a 2.7 h day around 12:00 UTC.
    """
    now = time.gmtime(unit_ts if unit_ts > 0 else None)
    utc_h = now.tm_hour + now.tm_min / 60.0

    def lon_for(solar_h: float) -> int:
        """Longitude where the local solar time is solar_h right now."""
        return int(round(((solar_h - utc_h) * 15.0 + 180.0) % 360.0 - 180.0))

    north_summer = (3, 21) <= (now.tm_mon, now.tm_mday) <= (9, 22)
    summer_pole = LAT_POLE if north_summer else -LAT_POLE
    if daytime:
        return [(summer_pole, 0), (LAT_NEAR_EQUATOR, lon_for(12.0))]
    return [(LAT_NEAR_EQUATOR, lon_for(0.0)), (-summer_pole, 0)]


def set_daytime(session: requests.Session, daytime: bool) -> bool:
    """
    Force is_daytime by writing a location (daytime_candidates()), trying each
    candidate until GET /api/status confirms it.

    T4 calls update_sun_times() on a lat/lon write (via the Q4 shadow update),
    so the change shows within a few seconds.
    Returns True if is_daytime was confirmed via GET /api/status.
    """
    label = "true" if daytime else "false"
    unit_ts = int(block(get_status(session), "system").get("ts_unix") or 0)
    for lat, lon in daytime_candidates(daytime, unit_ts):
        write_config(session, "system", "lat_deg",  lat)
        write_config(session, "system", "lat_frac", 0)
        write_config(session, "system", "lon_deg",  lon)
        write_config(session, "system", "lon_frac", 0)
        for _ in range(3):
            time.sleep(NVS_SETTLE_S)
            if is_daytime(get_status(session)) == daytime:
                log.info(f"  is_daytime={label} confirmed at lat {lat}, lon {lon}")
                return True
        log.info(f"  lat {lat}, lon {lon} does not give is_daytime={label}; next candidate")
    log.warning(f"  is_daytime={label} NOT confirmed at any candidate location")
    return False

# ---------------------------------------------------------------------------
# Target, preconditions, setup, teardown
# ---------------------------------------------------------------------------

def resolve_device() -> tuple[str, dict]:
    """The base URL of the unit to test and its status; exits 2 if there is none.

    GH_DEVICE_BASE is used as given. Otherwise each rig module's address is
    tried, and exactly one must answer: two answering means the fitted module
    cannot be told from a module powered elsewhere, so set GH_DEVICE_BASE.
    Refuses production.
    """
    override = os.getenv("GH_DEVICE_BASE")
    candidates = [("", override.rstrip("/"))] if override else list(RIG_MODULES)
    found = []
    for module, base in candidates:
        try:
            r = requests.get(f"{base}/api/status", timeout=5)
            r.raise_for_status()
            status = r.json()
        except Exception as exc:
            log.info(f"  {base}: no answer ({type(exc).__name__})")
            continue
        found.append((module, base, status))
    if not found:
        log.critical("No unit answers at " + ", ".join(b for _, b in candidates))
        sys.exit(2)
    if len(found) > 1:
        log.critical("More than one unit answers (" + ", ".join(b for _, b, _ in found)
                     + "): set GH_DEVICE_BASE to the fitted module")
        sys.exit(2)
    module, base, status = found[0]
    uid = str(block(status, "system").get("unit_id", "?"))
    if uid == PRODUCTION_UNIT:
        log.critical(f"{base} is {PRODUCTION_UNIT}, the production unit: refusing to run")
        sys.exit(2)
    if module and uid != module:
        log.warning(f"  {base} answers as unit {uid}, not the expected {module}")
    return base, status


def check_firmware(status: dict) -> bool:
    """Log what is under test; False when it predates the law expected here."""
    sysb = block(status, "system")
    w = block(status, "windows")
    log.info(
        f"Unit     : {sysb.get('unit_id')}  fw {sysb.get('fw_ver')}  "
        f"law {w.get('law')}  M3 {w.get('M3_ctrl_mode')}"
    )
    if w.get("law") is None:
        log.critical(
            "GET /api/status has no windows.law, so the firmware predates 2.15.0. "
            f"These expectations are {EXPECTED_LAW}'s (gh#84)."
        )
        return False
    return True


def emulator_holds(session: requests.Session, poll_s: int) -> bool:
    """True when the emulator keeps serving what this script pushes.

    The rig's emulator normally runs in REST, fed every 60 s by Node-RED on
    Shuttle2 with 5C88's live readings, so a pushed value survives at most one
    feed period (gotcha 2026-07-28). Push the neutral values, confirm the unit
    reads them raw, then wait for a poll after at least one feed period and
    confirm it still does. Touches the emulator only, never the unit's settings.
    """
    log.info("PRE-CHECK: does the emulator hold a pushed value?")
    set_rest_mode()
    if not push_and_verify_sensor(session, T=T_NEUTRAL, RH=RH_NEUTRAL,
                                  poll_s=poll_s, averaged=False):
        return False
    hold_s = max(FEED_PERIOD_S, poll_s) + poll_s + POLL_MARGIN_S
    log.info(f"  Holding {hold_s} s, past one feed period and a poll ...")
    time.sleep(hold_s)
    off = mismatches(get_status(session), T=T_NEUTRAL, RH=RH_NEUTRAL, averaged=False)
    if off:
        log.critical("  The emulator was overwritten: " + "; ".join(off))
        return False
    log.info("  The emulator held the pushed values")
    return True


def wait_for_preconditions(session: requests.Session) -> bool:
    """AUTOMATIC, the expected law, M3 timed and every window closed.

    ctrl_mode_m3 = 0 takes effect at T6's next wake and the neutral push closes
    whatever was open, so allow MODE_LIMIT_S for both.
    """
    t0 = time.time()
    while True:
        status = get_status(session)
        w = block(status, "windows")
        why = []
        if mode_current(status) != "AUTOMATIC":
            why.append(f"mode {mode_current(status)}")
        if w.get("law") != EXPECTED_LAW:
            why.append(f"law {w.get('law')} (expected {EXPECTED_LAW})")
        if w.get("M3_ctrl_mode") != "TIMED":
            why.append(f"M3_ctrl_mode {w.get('M3_ctrl_mode')}")
        if not windows_all_closed(status):
            why.append(wins_str(status))
        if not why:
            log.info(f"SETUP complete - AUTOMATIC, law {EXPECTED_LAW}, M3 TIMED, "
                     f"{wins_str(status)}")
            return True
        if time.time() - t0 > MODE_LIMIT_S:
            log.critical(f"SETUP: preconditions not met after {MODE_LIMIT_S} s: "
                         + ", ".join(why))
            return False
        time.sleep(5)


def setup(session: requests.Session) -> bool:
    """
    Establish every precondition the cases rely on; False when one cannot be.

    1. Write fast-test parameters, the conflict setpoints and the humidity floor.
    2. Put M3 on the timed law (ctrl_mode_m3 = 0): these cases pin `stepped`,
       mode 1's law. Mode 2 (Lineair, the default since 2.13.0 where a position
       sensor is fitted) runs `graded`: M3 opens only from step 2, by at most
       25 % per drive and not within 10 min of its last one.
    3. Force is_daytime=true (day setpoints used throughout).
    4. Push neutral sensors; wait until AUTOMATIC, law stepped v2, M3 TIMED and
       every window closed.
    """
    log.info("SETUP: writing fast-test config parameters")
    # Averaging: 1 min = 2 samples at a 30 s poll (see TEST_AVG_WIN)
    write_config(session, "climate", "avg_win_t",      TEST_AVG_WIN)
    write_config(session, "climate", "avg_win_rh",     TEST_AVG_WIN)
    # Motor travel: minimum on M1 and M2 (never M3, see TEST_TRAVEL_S); dwell disabled
    for ch in (1, 2):
        write_config(session, "motor", f"travel_m{ch}",       TEST_TRAVEL_S)
    for ch in (1, 2, 3):
        write_config(session, "motor", f"dwell_open_m{ch}",   0)
        write_config(session, "motor", f"dwell_close_m{ch}",  0)
    # M3 on the timed law: stepped decides all three windows
    write_config(session, "motor",  "ctrl_mode_m3",    0)
    # Poll interval
    write_config(session, "system", "poll_interval",   TEST_POLL_S)
    # Wind protection off — must not interfere with climate tests
    write_config(session, "wind",   "wind_prot_en",    0)
    # RH control on — required for all conflict tests
    write_config(session, "climate", "rh_ctrl_en",     1)
    # Default cr_priority (each test overwrites this for its specific scenario)
    write_config(session, "climate", "cr_priority",    CR_TEMP_FIRST)
    # Setpoints shared by all test cases
    write_config(session, "climate", "t_max_day",      T_MAX_DAY)
    write_config(session, "climate", "t_min_day",      T_MIN_DAY)
    write_config(session, "climate", "hyst_t",         HYST_T)
    write_config(session, "climate", "rh_max_day",     RH_MAX_DAY)
    write_config(session, "climate", "rh_min_day",     RH_MIN_DAY)
    write_config(session, "climate", "hyst_rh",        HYST_RH)
    time.sleep(NVS_SETTLE_S)

    log.info("SETUP: forcing is_daytime=true")
    if not set_daytime(session, True):
        log.critical("SETUP: is_daytime is not true, so the day setpoints written "
                     "here would not apply")
        return False

    log.info("SETUP: pushing neutral sensors; waiting for windows to close")
    if not push_and_verify_sensor(session, T=T_NEUTRAL, RH=RH_NEUTRAL):
        log.critical("SETUP: the unit does not read the neutral values")
        return False
    return wait_for_preconditions(session)


def verify_restore(session: requests.Session, orig: dict, fields: list[str]) -> None:
    """Read the restored settings back and compare them with the sample.

    POST /api/config only queues a write (T4 applies it a loop later), so its
    {"ok":true} is not evidence that the value is back.
    """
    drift = ["no read-back"]
    for attempt in range(3):
        time.sleep(NVS_SETTLE_S + 3 * attempt)
        try:
            now = get_config(session)
        except Exception as exc:
            log.warning(f"TEARDOWN: read-back failed - {exc}")
            continue
        drift = [f"{k} {now.get(k)} (was {orig.get(k)})"
                 for k in fields if now.get(k) != orig.get(k)]
        if not drift:
            log.info(f"TEARDOWN: read-back matches the start for all {len(fields)} settings")
            return
    log.warning("TEARDOWN: NOT back to the start: " + "; ".join(drift))


def teardown(session: requests.Session, orig: dict) -> None:
    """
    Restore original configuration values read before setup, then read them back.
    Re-authenticates first (a ~30 min run can expire the session cookie).
    Pushes neutral sensors afterwards.
    """
    log.info("TEARDOWN: refreshing admin session ...")
    try:
        body = do_login(session, "admin", ADMIN_PIN)
        if body.get("ok"):
            log.info("TEARDOWN: session refreshed")
        else:
            log.warning(f"TEARDOWN: re-login returned {body} - proceeding with existing cookie")
    except Exception as exc:
        log.warning(f"TEARDOWN: re-login raised {exc} - proceeding with existing cookie")

    log.info("TEARDOWN: restoring original configuration")
    _teardown_errors: list[str] = []
    restored: list[str] = []   # GET /api/config fields put back, for the read-back

    def _safe_write(ns: str, key: str, value: int) -> None:
        """Write one config key; log a warning on any HTTP/network error."""
        try:
            write_config(session, ns, key, value)
        except Exception as exc:
            msg = f"{ns}/{key}={value} - {exc}"
            log.warning(f"TEARDOWN: restore failed for {msg}")
            _teardown_errors.append(msg)

    # Climate setpoints
    for key in (
        "t_max_day", "t_min_day", "t_max_ngt", "t_min_ngt",
        "rh_max_day", "rh_min_day", "rh_max_ngt", "rh_min_ngt",
        "hyst_t", "hyst_rh", "rh_ctrl_en", "cr_priority",
        "avg_win_t", "avg_win_rh",
    ):
        if key in orig:
            _safe_write("climate", key, int(orig[key]))
            restored.append(key)

    # Wind settings
    for key in ("v_max", "dir_excl_low", "dir_excl_high", "wind_prot_en"):
        if key in orig:
            _safe_write("wind", key, int(orig[key]))
            restored.append(key)

    # Motor travel and dwells: GET /api/config gives each as an [M1, M2, M3]
    # array; the NVS keys are per channel. travel_m3 was never changed (see
    # TEST_TRAVEL_S), so it is not written back either; the read-back still
    # checks it.
    for field, prefix, channels in (("travel_s",      "travel_m",      (1, 2)),
                                    ("dwell_open_s",  "dwell_open_m",  (1, 2, 3)),
                                    ("dwell_close_s", "dwell_close_m", (1, 2, 3))):
        values = orig.get(field, [])
        if len(values) == 3:
            for i in channels:
                _safe_write("motor", f"{prefix}{i}", int(values[i - 1]))
            restored.append(field)

    # M3's control mode (the effective mode follows at T6's next wake)
    if "ctrl_mode_m3" in orig:
        _safe_write("motor", "ctrl_mode_m3", int(orig["ctrl_mode_m3"]))
        restored.append("ctrl_mode_m3")

    # System settings
    if "poll_interval_s" in orig:
        _safe_write("system", "poll_interval", int(orig["poll_interval_s"]))
        restored.append("poll_interval_s")

    # Latitude / longitude (restores is_daytime to original computation)
    for key in ("lat_deg", "lat_frac", "lon_deg", "lon_frac"):
        if key in orig:
            _safe_write("system", key, int(orig[key]))
            restored.append(key)

    if _teardown_errors:
        log.warning(
            f"TEARDOWN: {len(_teardown_errors)} config key(s) could not be restored: "
            + ", ".join(_teardown_errors)
        )

    verify_restore(session, orig, restored)

    log.info("TEARDOWN: pushing neutral sensors")
    try:
        push_sensors(T=T_NEUTRAL, RH=RH_NEUTRAL)
    except Exception as exc:
        log.warning(f"TEARDOWN: neutral sensor push failed - {exc}")
    time.sleep(NVS_SETTLE_S)
    log.info("TEARDOWN complete - the emulator serves T 10 C / RH 55 % until the "
             "Node-RED feed is restarted")

# ---------------------------------------------------------------------------
# Result tracker
# ---------------------------------------------------------------------------

class Results:
    def __init__(self):
        self._passed: list[str] = []
        self._failed: list[str] = []
        self._inconclusive: list[str] = []

    def record(self, test_id: str, ok: bool, detail: str = "") -> None:
        suffix = f" - {detail}" if detail else ""
        if ok:
            self._passed.append(test_id)
            log.info(f"[{test_id}] PASS{suffix}")
        else:
            self._failed.append(test_id)
            log.error(f"[{test_id}] FAIL{suffix}")

    def inconclusive(self, test_id: str, detail: str = "") -> None:
        """The case could not be judged: neither a PASS nor a FAIL."""
        suffix = f" - {detail}" if detail else ""
        self._inconclusive.append(test_id)
        log.warning(f"[{test_id}] INCONCLUSIVE{suffix}")

    def print_summary(self) -> int:
        """Log the summary and return the exit code: 0 all passed, 1 any failed,
        2 none failed but some could not be judged."""
        passed = len(self._passed)
        failed = len(self._failed)
        unjudged = len(self._inconclusive)
        total  = passed + failed + unjudged
        log.info("=" * 60)
        log.info(f"SUMMARY: {passed}/{total} passed, {failed} failed, "
                 f"{unjudged} inconclusive")
        for tag in self._passed:
            log.info(f"  PASS  {tag}")
        for tag in self._failed:
            log.error(f"  FAIL  {tag}")
        for tag in self._inconclusive:
            log.warning(f"  INCONCLUSIVE  {tag}")
        log.info("=" * 60)
        return 1 if failed else (2 if unjudged else 0)

# ---------------------------------------------------------------------------
# Test-case building blocks
# ---------------------------------------------------------------------------

def expect_open(results: Results, session: requests.Session, test_id: str,
                T, RH, open_keys: tuple[str, ...], why: str) -> None:
    """Push T and RH, wait for the motors, and PASS when exactly the windows in
    open_keys are OPEN or MOVING_OPEN and every other one is CLOSED
    (open_keys empty: all CLOSED)."""
    try:
        log.info(f"  Pushing T={T} C + RH={RH} % - {why} ...")
        if not push_and_verify_sensor(session, T=T, RH=RH):
            results.inconclusive(test_id, "sensor not confirmed - the case could not be judged")
            return
        time.sleep(WAIT_FOR_MOTOR_S)
        status = get_status(session)
        log.info(f"  Windows: {wins_str(status)}")
        want = ("+".join(open_keys) + " open, the rest CLOSED") if open_keys else "all CLOSED"
        results.record(test_id, windows_exactly(status, *open_keys),
                       f"{why} -> expected {want}; got {wins_str(status)}")
    except Exception as exc:
        results.inconclusive(test_id, f"{type(exc).__name__}: {exc}")


def expect_closed_2polls(results: Results, session: requests.Session, test_id: str,
                         T, RH, why: str) -> None:
    """Push T and RH, then watch a second poll cycle: PASS when every window is
    still CLOSED, so a late opening is caught too."""
    try:
        log.info(f"  Poll 1: pushing T={T} C + RH={RH} % - {why} ...")
        if not push_and_verify_sensor(session, T=T, RH=RH):
            results.inconclusive(test_id, "sensor not confirmed - the case could not be judged")
            return
        log.info(f"  Poll 2: second cycle at T={T} C RH={RH} % ...")
        push_sensors(T=T, RH=RH)
        time.sleep(WAIT_FOR_SENSOR_S)
        status = get_status(session)
        log.info(f"  Windows after 2 polls: {wins_str(status)}")
        results.record(test_id, windows_all_closed(status),
                       f"{why} -> expected all CLOSED over 2 polls; got {wins_str(status)}")
    except Exception as exc:
        results.inconclusive(test_id, f"{type(exc).__name__}: {exc}")


def set_priority(session: requests.Session, prio: int) -> None:
    write_config(session, "climate", "cr_priority", prio)
    time.sleep(NVS_SETTLE_S)

# ---------------------------------------------------------------------------
# Test cases
# ---------------------------------------------------------------------------

def run_cc020(results: Results, session: requests.Session) -> None:
    """
    UT-CC-020 — cr_priority 0: T demands OPEN, RH votes CLOSE → T's step → M1 opens

    Scenario: T=26°C (step_t=1, above t_max=25) vs RH=35% (step_rh=0, below
    rh_min=40, a "too dry" vote).  cr_priority=0 (temperature first).

    Rule 4 fires (T wants open, RH votes 0): the temperature's step, under
    every cr_priority since v2 — under 0 this is also what v1 did.
    step=1 → CMD_OPEN M1 only.

    Assert: M1 opens (OPEN or MOVING_OPEN), M2 and M3 remain CLOSED.

    Same inputs under priority 1: UT-CC-030a, which since 2.15.0 has the same
    outcome.
    """
    test_id = "UT-CC-020"
    log.info(f"--- {test_id}: priority 0 - T open (step 1) against RH too dry (step 0) ---")
    try:
        set_priority(session, CR_TEMP_FIRST)
        force_windows_closed(session)
    except Exception as exc:
        results.inconclusive(test_id, f"{type(exc).__name__}: {exc}")
        return
    expect_open(results, session, test_id, T_OPEN_S1, RH_DRY, ("M1",),
                f"prio {CR_TEMP_FIRST}, step_t 1 vs step_rh 0: T's step")


def run_cc021(results: Results, session: requests.Session) -> None:
    """
    UT-CC-021 — cr_priority 0: T demands nothing (step 0), RH demands OPEN (step 3) → stays CLOSED

    Scenario: T=10°C (step_t=0, well below the close threshold 19°C) vs RH=80%
    (step_rh=3, above rh_max=70).  cr_priority=0 (temperature first).

    Rule 5 (humidity alone wants to open): priority 0 refuses it. 10°C is
    also below the humidity floor (t_min_day + 2 = 14°C), so the floor would
    refuse it under priority 1 as well; UT-CC-031f checks priority 0 above it.

    Two consecutive poll cycles are observed, so a late opening is caught too.

    Assert: all windows CLOSED after 2 polls.
    """
    test_id = "UT-CC-021"
    log.info(f"--- {test_id}: priority 0 - T idle (step 0) against RH open (step 3) ---")
    try:
        set_priority(session, CR_TEMP_FIRST)
        force_windows_closed(session)
    except Exception as exc:
        results.inconclusive(test_id, f"{type(exc).__name__}: {exc}")
        return
    expect_closed_2polls(results, session, test_id, T_CLOSE, RH_OPEN,
                         f"prio {CR_TEMP_FIRST}, humidity alone wants step 3: refused")


def run_cc022(results: Results, session: requests.Session) -> None:
    """
    UT-CC-022 — No conflict when both demand the same action

    Two sub-cases covering the two "no conflict" rules:

    UT-CC-022a — Rule 2: both demand OPEN (step_t > 0 AND step_rh > 0)
      T=26°C (step_t=1) + RH=80% (step_rh=3) → both positive → Rule 2 fires.
      Return max(step_t, step_rh) = max(1, 3) = 3 → CMD_OPEN all three windows.
      v2 keeps this: its M1 cap applies to humidity ALONE, and here the
      temperature is venting too.
      Assert: all windows OPEN or MOVING_OPEN.

    UT-CC-022b — Rule 3: both demand CLOSE (step_t == step_rh == 0)
      (Windows open from CC-022a.)
      T=10°C (step_t=0) + RH=35% (step_rh=0) → equal → Rule 3 fires.
      Return step_t = 0 → every open window closes.
      Assert: all windows CLOSED or MOVING_CLOSE.

    cr_priority is irrelevant for both sub-cases (Rules 2 and 3 fire before
    cr_priority is consulted).
    """
    # --- UT-CC-022a: Rule 2 — both want OPEN → max ---
    test_id = "UT-CC-022a"
    log.info(f"--- {test_id}: Rule 2 - both demand OPEN -> max(step_t, step_rh) = 3 ---")
    try:
        set_priority(session, CR_TEMP_FIRST)   # irrelevant; set for consistency
        log.info("  Closing all windows before Rule 2 test ...")
        force_windows_closed(session)
    except Exception as exc:
        results.inconclusive(test_id, f"{type(exc).__name__}: {exc}")
    else:
        expect_open(results, session, test_id, T_OPEN_S1, RH_OPEN, WINDOW_KEYS,
                    "Rule 2: step_t 1 and step_rh 3 both open -> max = 3")

    # --- UT-CC-022b: Rule 3 — both want CLOSE → step_t == step_rh == 0 ---
    test_id = "UT-CC-022b"
    log.info(f"--- {test_id}: Rule 3 - both demand CLOSE -> step_t == step_rh == 0 ---")
    try:
        # Precondition: windows open from CC-022a (no force_close needed)
        # If CC-022a failed, this will start from whatever state; still valid.
        log.info(
            f"  Pushing T={T_CLOSE} C (step_t 0) + RH={RH_DRY} % (step_rh 0) - "
            f"Rule 3: equal (both 0) -> step 0 -> close ..."
        )
        if not push_and_verify_sensor(session, T=T_CLOSE, RH=RH_DRY):
            results.inconclusive(test_id, "sensor not confirmed - the case could not be judged")
        else:
            time.sleep(WAIT_FOR_MOTOR_S)
            status = get_status(session)
            log.info(f"  Windows: {wins_str(status)}")
            results.record(
                test_id, windows_all_closing(status),
                f"Rule 3: step_t 0 == step_rh 0 -> step 0 -> expected all "
                f"CLOSED/MOVING_CLOSE; got {wins_str(status)}",
            )
    except Exception as exc:
        results.inconclusive(test_id, f"{type(exc).__name__}: {exc}")


def run_cc030(results: Results, session: requests.Session) -> None:
    """
    UT-CC-030 — Dryness never closes against heat (2.15.0; was "CR_RH_FIRST: humidity wins")

    T demands OPEN and RH votes CLOSE (below rh_min): the temperature's step
    is issued under every cr_priority (v2 rule 4). Priority 0 is UT-CC-020.

    UT-CC-030a — cr_priority 1: T=26°C (step_t=1) vs RH=35% (step_rh=0)
      → M1 opens, M2 and M3 stay CLOSED. Under v1 the dry vote won and every
      window stayed closed: this is the case v2 changed.

    UT-CC-030b — cr_priority 2: T=28°C (step_t=2) vs RH=35% (step_rh=0)
      → M1 and M2 open, M3 stays CLOSED: the temperature's whole step, not
      capped at M1. (This was UT-CC-031b under v1's "deviation-based"
      priority, with the same outcome.)
    """
    for test_id, prio, t, open_keys in (
        ("UT-CC-030a", CR_RH_MAY_OPEN, T_OPEN_S1, ("M1",)),
        ("UT-CC-030b", CR_STORED_2,    T_OPEN_S2, ("M1", "M2")),
    ):
        step_t = len(open_keys)
        log.info(f"--- {test_id}: priority {prio} - T open (step {step_t}) against "
                 f"RH too dry (step 0) -> T's step ---")
        try:
            set_priority(session, prio)
            force_windows_closed(session)
        except Exception as exc:
            results.inconclusive(test_id, f"{type(exc).__name__}: {exc}")
            continue
        expect_open(results, session, test_id, t, RH_DRY, open_keys,
                    f"prio {prio}, step_t {step_t} vs step_rh 0: dryness never "
                    f"closes against heat")


def run_cc031(results: Results, session: requests.Session) -> None:
    """
    UT-CC-031 — Humidity may open M1 on its own, above the floor
    (2.15.0; was "CR_DEVIATION: higher step wins")

    T below t_max (no temperature demand) and RH=80% (step_rh=3), with
    t_min_day=12°C: humidity alone may open M1 from 14°C, and an opening it
    made holds down to 13°C (v2 rules 2, 3 and 5). One sequence under
    priority 1, then priority 2 and priority 0:

      031a  prio 1  from closed, T=13 (t_min + 1) → stays CLOSED: below the
                                                    floor, and nothing live to hold
                                                    (v1 opened all three)
      031b  prio 1  T=14 (t_min + 2)              → M1 opens; M2 and M3 stay
                                                    CLOSED (the M1 cap)
      031c  prio 1  T=13 (t_min + 1)              → M1 stays open: the live
                                                    opening holds
      031d  prio 1  T=12 (t_min)                  → M1 closes
      031e  prio 2  from closed, T=14             → M1 only: priority 2 = 1
      031f  prio 0  from closed, T=14             → stays CLOSED: temperature first

    031a and 031c push the same temperature and expect opposite outcomes: that
    is the floor's hysteresis. Each temperature is held until the unit's
    average has converged on it (T_TOL_C), because T6 rounds the average to
    whole degrees and a half-converged 13.5 reads as 14.
    """
    try:
        set_priority(session, CR_RH_MAY_OPEN)
        log.info("  Closing all windows before UT-CC-031 ...")
        force_windows_closed(session)
    except Exception as exc:
        for sub in "abcdef":
            results.inconclusive(f"UT-CC-031{sub}", f"{type(exc).__name__}: {exc}")
        return

    # --- UT-CC-031a: below the floor, nothing live → stays closed ---
    test_id = "UT-CC-031a"
    log.info(f"--- {test_id}: prio 1, T={T_FLOOR_HOLD} C (t_min + 1) from closed "
             f"-> stays CLOSED ---")
    expect_closed_2polls(results, session, test_id, T_FLOOR_HOLD, RH_OPEN,
                         f"prio 1, T {T_FLOOR_HOLD} below the floor {T_FLOOR}, "
                         f"no opening live")

    # --- UT-CC-031b: at the floor → M1 only ---
    test_id = "UT-CC-031b"
    log.info(f"--- {test_id}: prio 1, T={T_FLOOR} C (t_min + 2) -> M1 only ---")
    expect_open(results, session, test_id, T_FLOOR, RH_OPEN, ("M1",),
                f"prio 1, T {T_FLOOR} at the floor: step_rh 3 capped at M1")

    # --- UT-CC-031c/d: the live opening holds at t_min + 1, ends at t_min ---
    for test_id, t, open_keys, why in (
        ("UT-CC-031c", T_FLOOR_HOLD, ("M1",),
         f"prio 1, T {T_FLOOR_HOLD} (t_min + 1): the live opening holds"),
        ("UT-CC-031d", T_BELOW_FLOOR, (),
         f"prio 1, T {T_BELOW_FLOOR} (t_min): below the hold, M1 closes"),
    ):
        log.info(f"--- {test_id}: {why} ---")
        try:
            status = get_status(session)
        except Exception as exc:
            results.inconclusive(test_id, f"{type(exc).__name__}: {exc}")
            continue
        if not windows_exactly(status, "M1"):
            results.inconclusive(test_id, f"precondition not met - needs M1 open from "
                                          f"the step before; got {wins_str(status)}")
            continue
        expect_open(results, session, test_id, t, RH_OPEN, open_keys, why)

    # --- UT-CC-031e: priority 2 behaves as 1 ---
    test_id = "UT-CC-031e"
    log.info(f"--- {test_id}: prio 2, T={T_FLOOR} C from closed -> M1 only ---")
    try:
        set_priority(session, CR_STORED_2)
        force_windows_closed(session)
    except Exception as exc:
        results.inconclusive(test_id, f"{type(exc).__name__}: {exc}")
    else:
        expect_open(results, session, test_id, T_FLOOR, RH_OPEN, ("M1",),
                    f"prio 2, T {T_FLOOR} at the floor: as priority 1, M1 only")

    # --- UT-CC-031f: priority 0 refuses humidity alone, even above the floor ---
    test_id = "UT-CC-031f"
    log.info(f"--- {test_id}: prio 0, T={T_FLOOR} C from closed -> stays CLOSED ---")
    try:
        set_priority(session, CR_TEMP_FIRST)
        force_windows_closed(session)
    except Exception as exc:
        results.inconclusive(test_id, f"{type(exc).__name__}: {exc}")
    else:
        expect_closed_2polls(results, session, test_id, T_FLOOR, RH_OPEN,
                             f"prio 0, T {T_FLOOR} at the floor: temperature first")

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    global DEVICE_BASE, WAIT_FOR_MOTOR_S

    log.info("=" * 60)
    log.info("Greenhouse Controller - Conflict Resolution Test")
    log.info("Test cases: UT-CC-020, UT-CC-021, UT-CC-022, UT-CC-030, UT-CC-031 "
             "(softwareTestPlan.md 6.4)")
    log.info(f"Emulator : {EMULATOR_BASE}")

    DEVICE_BASE, status = resolve_device()
    log.info(f"Device   : {DEVICE_BASE}")
    if not check_firmware(status):
        sys.exit(2)

    results = Results()

    try:
        session = get_admin_session()
    except Exception as exc:
        log.critical(f"Cannot establish admin session: {exc}")
        sys.exit(2)

    log.info("Reading original configuration ...")
    try:
        orig = get_config(session)
    except Exception as exc:
        log.critical(f"Cannot read original config: {exc}")
        sys.exit(2)

    travel = orig.get("travel_s") or [TEST_TRAVEL_S] * 3
    WAIT_FOR_MOTOR_S = motor_wait_s(max(TEST_TRAVEL_S, int(travel[2])))
    log.info(
        f"Test params: poll={TEST_POLL_S}s  travel M1/M2={TEST_TRAVEL_S}s "
        f"M3={travel[2]}s (the unit's own)  avg_win={TEST_AVG_WIN}"
    )
    log.info(
        f"Timing: sensor_wait={WAIT_FOR_SENSOR_S}s  "
        f"motor_wait={WAIT_FOR_MOTOR_S}s"
    )
    log.info("=" * 60)

    try:
        holds = emulator_holds(session, int(orig.get("poll_interval_s", TEST_POLL_S)))
    except Exception as exc:
        log.critical(f"PRE-CHECK failed: {type(exc).__name__}: {exc}")
        holds = False
    if not holds:
        log.critical("The emulator does not hold a pushed value, so no result would "
                     "mean anything. Stop the Node-RED feed on Shuttle2 and run again. "
                     "The unit's settings were not touched.")
        sys.exit(2)

    ready = False
    try:
        try:
            ready = setup(session)
        except Exception as exc:
            log.critical(f"SETUP failed: {type(exc).__name__}: {exc}")
        if ready:
            run_cc020(results, session)
            run_cc021(results, session)
            run_cc022(results, session)
            run_cc030(results, session)
            run_cc031(results, session)

    finally:
        teardown(session, orig)

    if not ready:
        log.critical("Setup could not establish its preconditions - no case was run")
        sys.exit(2)

    code = results.print_summary()
    log.info(f"Log written to: {LOG_PATH}")
    sys.exit(code)


if __name__ == "__main__":
    main()
