#!/usr/bin/env python3
"""
3_3_Setpoints_and_Hysteresis.py
Greenhouse Ventilation Controller — Setpoints and Hysteresis test

Covers: UT-CC-014 through UT-CC-029 (softwareTestPlan.md §6.3)
Both temperature and humidity control paths are exercised.
Graduated ventilation (steps 1–3) and day/night setpoint selection are verified.

The expectations are the law in force since firmware 2.15.0, `stepped` v2
(gh#84, drivers/ventModel/src/vent_model_stepped.cpp). Only the humidity cases
changed: humidity opens a window on its own only under cr_priority 1 or 2, only
M1, and only from t_min + 2 °C (UT-CC-018, UT-CC-024).

RUNNING IT CHANGES THE RIG, so it needs the operator's go. It rewrites about 30
settings (restored and read back at the end) and pushes values into the sensor
emulator, which Node-RED on Shuttle2 normally feeds with 5C88's live readings
every 60 s. Stop that feed for the run and restart it afterwards: the script
stops with exit code 2, before touching the unit's settings, if the emulator
does not hold a pushed value. Not during a soak.

See 3_3_Setpoints_and_Hysteresis.md for full instructions.
Results written to 3_3_Setpoints_and_Hysteresis.log.
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
TEST_AVG_WIN    = 1    # climate/avg_win_t and avg_win_rh — minimum allowed.
                       # cfg_clamp() (v1.16.25) enforces avg_win min=1, so 0 is silently
                       # clamped.  At avg_win=1 min and poll_interval=30 s the rolling
                       # window holds 2 samples, so the FIRST poll after a sensor push
                       # reads (prev+new)/2 and will not match the pushed value within
                       # tolerance.  push_and_verify_sensor() retries: the second push
                       # fills both slots with the new value, avg matches, verification
                       # succeeds.  Cost: ~one extra poll cycle per test case.

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
# Setpoints for temperature tests
# ---------------------------------------------------------------------------
#
# t_max_day = 25 °C,  hyst_t = 6 °C → step_width = hyst // NUM_VENT_STEPS = 2
#
# Graduated ventilation thresholds (step_from_deviation algorithm):
#   Step 1 (M1 only):   T = 26  →  deviation = 1  →  ceil(1/2) = 1
#   Step 2 (M1+M2):     T = 28  →  deviation = 3  →  ceil(3/2) = 2
#   Step 3 (M1+M2+M3):  T = 31  →  deviation = 6  →  ceil(6/2) = 3
#
# Close-hysteresis threshold: t_max − hyst = 25 − 6 = 19 °C
#   T = 18 °C: deviation = −7 ≤ −hyst → step allowed to drop to 0 → close
#   T = 24 °C: deviation = −1 > −hyst  → step held at ≥ 1 → no close (hysteresis)
#
T_MAX_DAY    = 25   # °C
HYST_T       = 6    # °C
T_MIN_DAY    = 15   # °C — not used by the temperature step; since 2.15.0 it is
                    # the base of the humidity floor (see T_RH_ONLY and the notes)
T_STEP1      = 26   # °C — triggers step 1 (M1 only)
T_STEP2      = 28   # °C — triggers step 2 (M1+M2)
T_STEP3      = 31   # °C — triggers step 3 (all)
T_HYST_HOLD  = 24   # °C — inside hysteresis band (19 < 24 < 25); windows held open
T_CLOSE      = 18   # °C — below close threshold (19); triggers CLOSE
T_BELOW_MIN  = 14   # °C — below T_MIN_DAY, and so below the close threshold (19)
T_NEUTRAL    = 10   # °C — guaranteed to keep windows closed

# ---------------------------------------------------------------------------
# Setpoints for humidity tests
# ---------------------------------------------------------------------------
RH_MAX_DAY   = 70   # %
HYST_RH      = 6    # %
RH_MIN_DAY   = 40   # %
RH_OPEN      = 80   # % — above rh_max; deviation = 10 → step 5, clamped to 3
RH_DRY       = 35   # % — below rh_min: a "too dry" vote (0), inert since 2.15.0
RH_NEUTRAL   = 55   # % — between rh_min and rh_max; no RH demand

# Humidity may open a window on its own only under cr_priority 1 or 2, only M1,
# and only from t_min + 2 °C (stepped v2, gh#84). UT-CC-018 and UT-CC-024 push
# this temperature with RH_OPEN: at or above that floor, far below t_max_day=40.
CR_RH_MAY_OPEN = 1    # climate/cr_priority: humidity may also open M1
T_RH_ONLY      = 20   # °C — ≥ T_MIN_DAY + 2 = 17 °C
assert T_RH_ONLY >= T_MIN_DAY + 2, "UT-CC-018/024 must push T at or above the floor"

# ---------------------------------------------------------------------------
# Night setpoints (UT-CC-028/029)
# ---------------------------------------------------------------------------
T_MAX_NGT    = 18   # °C — night max; T=20 > 18 triggers opening
T_OPEN_NGT   = 20   # °C — above t_max_ngt; triggers night opening

# ---------------------------------------------------------------------------
# Forcing is_daytime through the location (see daytime_candidates())
# ---------------------------------------------------------------------------
LAT_POLE         = 89   # in polar day or night for months, except near an equinox
LAT_NEAR_EQUATOR = 1    # never 0: lat 0 with lon 0 means "no location", read as day (FR-DN05)

_DIR     = os.path.dirname(os.path.abspath(__file__))
LOG_PATH = os.path.join(_DIR, "3_3_Setpoints_and_Hysteresis.log")

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

def _make_logger() -> logging.Logger:
    log = logging.getLogger("setpoints_test")
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
    """True when every window is CLOSED or MOVING_CLOSE: the close was commanded
    (per-channel CMD_CLOSE for climate-control step→0 transitions, or
    CMD_CLOSE_ALL only on safety events)."""
    return all(s in CLOSING for s in win_states(status).values())


def any_window_open_or_moving(status: dict) -> bool:
    return any(s in OPENING for s in win_states(status).values())


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
    Push T=T_NEUTRAL (below every close threshold) and RH=RH_NEUTRAL (no RH
    vote), and wait for all windows to reach CLOSED or MOVING_CLOSE state.
    Returns True within two poll+travel cycles, False otherwise.

    This is a setup/teardown helper; it does not record a test result.
    T=10 °C is below t_max_day − hyst_t for any t_max_day ≥ 16 °C.
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

    1. Write fast-test parameters to NVS.
    2. Put M3 on the timed law (ctrl_mode_m3 = 0): these cases pin `stepped`,
       mode 1's law. Mode 2 (Lineair, the default since 2.13.0 where a position
       sensor is fitted) runs `graded`: M3 opens only from step 2, by at most
       25 % per drive and not within 10 min of its last one.
    3. Force is_daytime=true.
    4. Push neutral sensors; wait until AUTOMATIC, law stepped v2, M3 TIMED and
       every window closed.
    """
    log.info("SETUP: writing fast-test config parameters")
    # Averaging: 1 min × 60s ÷ 30s poll = 2 samples (cfg_clamp min); see TEST_AVG_WIN
    write_config(session, "climate", "avg_win_t",      TEST_AVG_WIN)
    write_config(session, "climate", "avg_win_rh",     TEST_AVG_WIN)
    # Motor travel: minimum on M1 and M2 (never M3, see TEST_TRAVEL_S); dwell disabled
    for ch in (1, 2):
        write_config(session, "motor", f"travel_m{ch}",      TEST_TRAVEL_S)
    for ch in (1, 2, 3):
        write_config(session, "motor", f"dwell_open_m{ch}",  0)
        write_config(session, "motor", f"dwell_close_m{ch}", 0)
    # M3 on the timed law: stepped decides all three windows
    write_config(session, "motor",  "ctrl_mode_m3",    0)
    # Poll interval
    write_config(session, "system", "poll_interval",   TEST_POLL_S)
    # Wind protection off — must not interfere with these tests
    write_config(session, "wind",   "wind_prot_en",    0)
    # RH control on — required for UT-CC-018 and UT-CC-024
    write_config(session, "climate", "rh_ctrl_en",     1)
    time.sleep(NVS_SETTLE_S)

    log.info("SETUP: forcing is_daytime=true")
    if not set_daytime(session, True):
        log.critical("SETUP: is_daytime is not true, so the day setpoints the cases "
                     "write would not apply")
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
    Re-authenticates first (a ~45 min run can expire the session cookie).
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


SENSOR_NOT_CONFIRMED = "sensor not confirmed - the case could not be judged"


def raised(exc: Exception) -> str:
    return f"{type(exc).__name__}: {exc}"

# ---------------------------------------------------------------------------
# Test cases
# ---------------------------------------------------------------------------

def run_cc014(results: Results, session: requests.Session) -> None:
    """
    UT-CC-014 — OPEN when T > T_max_day (is_daytime=true)

    Config: t_max_day=25, hyst_t=6, rh_ctrl_en=0.
    Push T=26°C → deviation=1, step=1 → CMD_OPEN for M1.
    Assert: at least one window transitions out of CLOSED.
    """
    test_id = "UT-CC-014"
    log.info(f"--- {test_id}: OPEN when T > T_max_day ---")
    try:
        write_config(session, "climate", "t_max_day",  T_MAX_DAY)
        write_config(session, "climate", "hyst_t",     HYST_T)
        write_config(session, "climate", "rh_ctrl_en", 0)
        time.sleep(NVS_SETTLE_S)

        if not push_and_verify_sensor(session, T=T_STEP1, RH=RH_NEUTRAL):
            results.inconclusive(test_id, SENSOR_NOT_CONFIRMED)
            return

        # Wait for motor to travel to OPEN
        time.sleep(WAIT_FOR_MOTOR_S)
        status = get_status(session)
        log.info(f"  Windows: {wins_str(status)}")

        if any_window_open_or_moving(status):
            results.record(
                test_id, True,
                f"T={T_STEP1} C > t_max={T_MAX_DAY} C -> M1 opened: {wins_str(status)}",
            )
        else:
            results.record(
                test_id, False,
                f"Expected M1 to open at T={T_STEP1} C (t_max={T_MAX_DAY} C), "
                f"got: {wins_str(status)}",
            )
    except Exception as exc:
        results.inconclusive(test_id, raised(exc))


def run_cc015(results: Results, session: requests.Session) -> None:
    """
    UT-CC-015 — Stay open when T > (T_max − hyst_t) — hysteresis prevents premature close

    Precondition: at least one window open (from UT-CC-014).
    Push T=24°C: above close threshold (25−6=19°C) but below t_max=25°C.
    Wait two consecutive poll cycles; assert windows remain open.
    """
    test_id = "UT-CC-015"
    log.info(f"--- {test_id}: Stay open inside hysteresis band ---")
    try:
        status = get_status(session)
        if not any_window_open_or_moving(status):
            results.inconclusive(
                test_id,
                f"precondition not met - no window open (see UT-CC-014): {wins_str(status)}",
            )
            return

        # First poll: T inside hysteresis band
        if not push_and_verify_sensor(session, T=T_HYST_HOLD, RH=RH_NEUTRAL):
            results.inconclusive(test_id, SENSOR_NOT_CONFIRMED)
            return

        # Second poll cycle — push again and wait for another evaluation
        log.info(f"  Second poll cycle at T={T_HYST_HOLD} C ...")
        push_sensors(T=T_HYST_HOLD, RH=RH_NEUTRAL)
        time.sleep(WAIT_FOR_SENSOR_S)
        status = get_status(session)
        log.info(f"  Windows after 2 polls at T={T_HYST_HOLD} C: {wins_str(status)}")

        close_thresh = T_MAX_DAY - HYST_T  # = 19
        if any_window_open_or_moving(status):
            results.record(
                test_id, True,
                f"T={T_HYST_HOLD} C > close_thresh={close_thresh} C -> "
                f"windows held open over 2 polls: {wins_str(status)}",
            )
        else:
            results.record(
                test_id, False,
                f"Windows closed prematurely at T={T_HYST_HOLD} C "
                f"(close_thresh={close_thresh} C): {wins_str(status)}",
            )
    except Exception as exc:
        results.inconclusive(test_id, raised(exc))


def run_cc016(results: Results, session: requests.Session) -> None:
    """
    UT-CC-016 — CLOSE when T < (T_max − hyst_t)

    Precondition: at least one window open.
    Push T=18°C: below close threshold (25−6=19°C).
    The close-hysteresis guard allows step-down to 0 → per-channel CMD_CLOSE
    (v1.16.22: apply_step_delta() no longer emits CMD_CLOSE_ALL for normal
    climate transitions; CMD_CLOSE_ALL is reserved for safety events only).
    Assert: all windows reach CLOSED.
    """
    test_id = "UT-CC-016"
    log.info(f"--- {test_id}: CLOSE when T below close threshold ---")
    try:
        if not push_and_verify_sensor(session, T=T_CLOSE, RH=RH_NEUTRAL):
            results.inconclusive(test_id, SENSOR_NOT_CONFIRMED)
            return

        time.sleep(WAIT_FOR_MOTOR_S)
        status = get_status(session)
        log.info(f"  Windows: {wins_str(status)}")

        close_thresh = T_MAX_DAY - HYST_T  # = 19
        if windows_all_closed(status):
            results.record(
                test_id, True,
                f"T={T_CLOSE} C < close_thresh={close_thresh} C -> "
                f"all CLOSED: {wins_str(status)}",
            )
        else:
            results.record(
                test_id, False,
                f"Expected all CLOSED at T={T_CLOSE} C "
                f"(close_thresh={close_thresh} C), got: {wins_str(status)}",
            )
    except Exception as exc:
        results.inconclusive(test_id, raised(exc))


def run_cc017(results: Results, session: requests.Session) -> None:
    """
    UT-CC-017 — CLOSE when T < T_min_day

    Note: the temperature step uses only t_max and hyst_t; t_min is not a
    temperature threshold. (Since 2.15.0 it floors humidity-only venting,
    which rh_ctrl_en=0 switches off here.) The close is triggered by the
    hysteresis guard (T < t_max−hyst_t = 19°C), which covers T < t_min_day
    for any t_min_day ≤ t_max−hyst_t (FRS FR-C07).

    Setup: t_min_day=15; open windows (T=T_STEP1=26°C), then push
    T=T_BELOW_MIN=14°C (below t_min_day=15°C AND below the close threshold
    of 19°C), the test plan's values. Until 2026-09-27 this case pushed 10°C
    against t_min_day=5°C, so it never had T below t_min.
    Assert: all windows close.
    """
    test_id = "UT-CC-017"
    log.info(f"--- {test_id}: CLOSE when T < T_min_day ---")
    try:
        write_config(session, "climate", "t_min_day",  T_MIN_DAY)
        write_config(session, "climate", "t_max_day",  T_MAX_DAY)
        write_config(session, "climate", "hyst_t",     HYST_T)
        write_config(session, "climate", "rh_ctrl_en", 0)
        time.sleep(NVS_SETTLE_S)

        # Open windows first
        log.info(f"  Opening windows: pushing T={T_STEP1} C ...")
        if not push_and_verify_sensor(session, T=T_STEP1, RH=RH_NEUTRAL):
            results.inconclusive(test_id, SENSOR_NOT_CONFIRMED + " (open phase)")
            return
        time.sleep(WAIT_FOR_MOTOR_S)
        status = get_status(session)
        if not any_window_open_or_moving(status):
            results.record(
                test_id, False,
                f"Could not open windows in setup: {wins_str(status)}",
            )
            return

        # Now push T below t_min_day (and so below the close threshold)
        log.info(
            f"  Pushing T={T_BELOW_MIN} C (below t_min_day={T_MIN_DAY} C "
            f"and close_thresh={T_MAX_DAY - HYST_T} C) ..."
        )
        if not push_and_verify_sensor(session, T=T_BELOW_MIN, RH=RH_NEUTRAL):
            results.inconclusive(test_id, SENSOR_NOT_CONFIRMED)
            return

        time.sleep(WAIT_FOR_MOTOR_S)
        status = get_status(session)
        log.info(f"  Windows: {wins_str(status)}")

        if windows_all_closed(status):
            results.record(
                test_id, True,
                f"T={T_BELOW_MIN} C < t_min_day={T_MIN_DAY} C -> all CLOSED: "
                f"{wins_str(status)}",
            )
        else:
            results.record(
                test_id, False,
                f"Expected all CLOSED at T={T_BELOW_MIN} C "
                f"(< t_min_day={T_MIN_DAY} C), got: {wins_str(status)}",
            )
    except Exception as exc:
        results.inconclusive(test_id, raised(exc))


def run_cc018(results: Results, session: requests.Session) -> None:
    """
    UT-CC-018 — OPEN when RH > RH_max_day (rh_ctrl_en=true): M1 only

    Config: rh_ctrl_en=1, rh_max_day=70, hyst_rh=6, t_max_day=40 (T never
    triggers), cr_priority=1, t_min_day=15.
    Push RH=80% at T=20°C → step_rh=3 while the temperature asks for nothing.
    Since 2.15.0 (stepped v2) humidity alone opens M1 at most, only under
    cr_priority 1 or 2, and only from t_min + 2 = 17°C; under cr_priority 0 it
    would open nothing.
    Assert: M1 OPEN or MOVING_OPEN; M2 and M3 CLOSED.
    """
    test_id = "UT-CC-018"
    log.info(f"--- {test_id}: OPEN when RH > RH_max_day (humidity alone: M1 only) ---")
    try:
        write_config(session, "climate", "rh_ctrl_en",  1)
        write_config(session, "climate", "rh_max_day",  RH_MAX_DAY)
        write_config(session, "climate", "rh_min_day",  RH_MIN_DAY)
        write_config(session, "climate", "hyst_rh",     HYST_RH)
        write_config(session, "climate", "t_max_day",   40)   # T never triggers
        write_config(session, "climate", "t_min_day",   T_MIN_DAY)   # floor = 17 C
        write_config(session, "climate", "hyst_t",      HYST_T)
        write_config(session, "climate", "cr_priority", CR_RH_MAY_OPEN)
        time.sleep(NVS_SETTLE_S)

        force_windows_closed(session)

        log.info(f"  Pushing RH={RH_OPEN} % (above rh_max={RH_MAX_DAY} %) at "
                 f"T={T_RH_ONLY} C (floor {T_MIN_DAY + 2} C) ...")
        if not push_and_verify_sensor(session, T=T_RH_ONLY, RH=RH_OPEN):
            results.inconclusive(test_id, SENSOR_NOT_CONFIRMED)
            return

        time.sleep(WAIT_FOR_MOTOR_S)
        status = get_status(session)
        log.info(f"  Windows: {wins_str(status)}")

        results.record(
            test_id, windows_exactly(status, "M1"),
            f"RH={RH_OPEN} % > rh_max={RH_MAX_DAY} %, T {T_RH_ONLY} C at or above "
            f"the floor -> expected M1 open, M2+M3 CLOSED; got {wins_str(status)}",
        )
    except Exception as exc:
        results.inconclusive(test_id, raised(exc))


def run_cc019(results: Results, session: requests.Session) -> None:
    """
    UT-CC-019 — No relay chatter at setpoint boundary

    Config: t_max_day=25, hyst_t=6, rh_ctrl_en=0.
    Step 1. Open M1 by pushing T=26°C.
    Step 2. Push T=24°C (inside hysteresis band: 19 < 24 < 25).
    Step 3. Wait three consecutive poll cycles, recording window state at each.
    Assert: window state is identical across all three polls (no oscillation)
            AND at least one window remains open (hysteresis guard is holding).
    Only the three states are compared: the windows object also carries M3's
    measured position, which moves by a few tenths with no drive at all.
    """
    test_id = "UT-CC-019"
    log.info(f"--- {test_id}: No relay chatter at setpoint boundary ---")
    try:
        write_config(session, "climate", "t_max_day",  T_MAX_DAY)
        write_config(session, "climate", "hyst_t",     HYST_T)
        write_config(session, "climate", "rh_ctrl_en", 0)
        time.sleep(NVS_SETTLE_S)

        # Close any windows left open by the previous test before starting the
        # chatter check.  Without this, the prior test's stale T=T_NEUTRAL on
        # the emulator can trigger an all-close (step 0 → per-channel CMD_CLOSE)
        # on the very next firmware poll (before the T=T_STEP1 push is
        # recognised), causing all windows to start MOVING_CLOSE instead of
        # opening.
        force_windows_closed(session)

        log.info(f"  Opening M1: pushing T={T_STEP1} C ...")
        if not push_and_verify_sensor(session, T=T_STEP1, RH=RH_NEUTRAL):
            results.inconclusive(test_id, SENSOR_NOT_CONFIRMED + " (open phase)")
            return
        time.sleep(WAIT_FOR_MOTOR_S)
        status = get_status(session)
        if not any_window_open_or_moving(status):
            results.record(
                test_id, False,
                f"Could not open M1 for setup: {wins_str(status)}",
            )
            return

        snaps = []
        for poll in (1, 2, 3):
            push_sensors(T=T_HYST_HOLD, RH=RH_NEUTRAL)
            time.sleep(WAIT_FOR_SENSOR_S)
            status = get_status(session)
            snaps.append(win_states(status))
            log.info(f"  Poll {poll}: {wins_str(status)}")

        states_stable = (snaps[0] == snaps[1] == snaps[2])
        any_open      = any(s in OPENING for s in snaps[0].values())
        close_thresh  = T_MAX_DAY - HYST_T  # = 19
        shown = " -> ".join(" ".join(f"{k}={v}" for k, v in s.items()) for s in snaps)

        if states_stable and any_open:
            results.record(
                test_id, True,
                f"T={T_HYST_HOLD} C (>{close_thresh} C): 3 polls steady "
                f"at {shown.split(' -> ')[0]} - no chatter",
            )
        elif not states_stable:
            results.record(
                test_id, False,
                f"Window state oscillated: {shown} (relay chatter)",
            )
        else:
            results.record(
                test_id, False,
                f"Windows unexpectedly closed at T={T_HYST_HOLD} C "
                f"(above close_thresh={close_thresh} C): {shown.split(' -> ')[0]}",
            )
    except Exception as exc:
        results.inconclusive(test_id, raised(exc))


def run_cc024(results: Results, session: requests.Session) -> None:
    """
    UT-CC-024 — Close when RH < RH_min_day (over-dry)

    Config: rh_ctrl_en=1, rh_min_day=40, rh_max_day=70, t_max_day=40,
    cr_priority=1, t_min_day=15.
    Step 1. Open M1 via high RH=80% at T=20°C: humidity alone, above the floor
            (t_min + 2 = 17°C), so M1 only (stepped v2's cap).
    Step 2. Push RH=35% (below rh_min=40%) → the humidity vote ends and the
            temperature asks for nothing → step 0 → apply_step_delta() issues
            per-channel CMD_CLOSE for every open channel.  (v1.16.22 reserved
            CMD_CLOSE_ALL for safety events: wind override, motor alarm,
            calibration.)
            Since 2.15.0 this close does not depend on rh_min: a live humidity
            vote ends once the average is at or below rh_max − max(hyst_rh/3, 1)
            = 68 % (v2's close guard), and a "too dry" vote changes no decision
            (FRS FR-C08). M1 would close the same at RH=55%. The case stays as
            the check that a humidity-only opening ends.
    Assert: all windows CLOSED (or MOVING_CLOSE).
    """
    test_id = "UT-CC-024"
    log.info(f"--- {test_id}: Close when RH < RH_min_day ---")
    try:
        write_config(session, "climate", "rh_ctrl_en",  1)
        write_config(session, "climate", "rh_max_day",  RH_MAX_DAY)
        write_config(session, "climate", "rh_min_day",  RH_MIN_DAY)
        write_config(session, "climate", "hyst_rh",     HYST_RH)
        write_config(session, "climate", "t_max_day",   40)   # T never triggers
        write_config(session, "climate", "t_min_day",   T_MIN_DAY)   # floor = 17 C
        write_config(session, "climate", "hyst_t",      HYST_T)
        write_config(session, "climate", "cr_priority", CR_RH_MAY_OPEN)
        time.sleep(NVS_SETTLE_S)

        force_windows_closed(session)

        # Open M1 via high RH
        log.info(f"  Opening M1: pushing RH={RH_OPEN} % at T={T_RH_ONLY} C ...")
        if not push_and_verify_sensor(session, T=T_RH_ONLY, RH=RH_OPEN):
            results.inconclusive(test_id, SENSOR_NOT_CONFIRMED + " (open phase)")
            return
        time.sleep(WAIT_FOR_MOTOR_S)
        status = get_status(session)
        if not windows_exactly(status, "M1"):
            results.record(
                test_id, False,
                f"Open phase: humidity alone should open M1 only, got: {wins_str(status)}",
            )
            return
        log.info(f"  M1 open: {wins_str(status)}")

        # Push RH below rh_min → step 0 → per-channel CMD_CLOSE
        log.info(
            f"  Pushing RH={RH_DRY} % (below rh_min={RH_MIN_DAY} %) -> step 0 -> CMD_CLOSE ..."
        )
        if not push_and_verify_sensor(session, T=T_RH_ONLY, RH=RH_DRY):
            results.inconclusive(test_id, SENSOR_NOT_CONFIRMED)
            return

        time.sleep(WAIT_FOR_MOTOR_S)
        status = get_status(session)
        log.info(f"  Windows: {wins_str(status)}")

        # Accept MOVING_CLOSE: the close was commanded
        if windows_all_closing(status):
            results.record(
                test_id, True,
                f"RH={RH_DRY} % -> humidity vote ended -> step 0 -> per-channel "
                f"CMD_CLOSE -> all CLOSED/MOVING_CLOSE: {wins_str(status)}",
            )
        else:
            results.record(
                test_id, False,
                f"Expected all CLOSED/MOVING_CLOSE at RH={RH_DRY} % "
                f"(rh_min={RH_MIN_DAY} %), got: {wins_str(status)}",
            )
    except Exception as exc:
        results.inconclusive(test_id, raised(exc))


def run_cc025_026_027(results: Results, session: requests.Session) -> None:
    """
    UT-CC-025/026/027 — Graduated ventilation steps 1, 2 and 3

    Config: t_max_day=25, hyst_t=6, rh_ctrl_en=0.
    step_width = hyst_t // NUM_VENT_STEPS = 6 // 3 = 2

    Starting from all windows CLOSED, escalate temperature through three levels:

    UT-CC-025 — Step 1 (M1 only):
      T=26: deviation=1, ceil(1/2)=1 → CMD_OPEN M1 only
      Assert: M1 open, M2 CLOSED, M3 CLOSED

    UT-CC-026 — Step 2 (M1+M2):
      T=28: deviation=3, ceil(3/2)=2 → CMD_OPEN M2 (M1 already open)
      Assert: M1 open, M2 open, M3 CLOSED

    UT-CC-027 — Step 3 (M1+M2+M3):
      T=31: deviation=6, ceil(6/2)=3 → CMD_OPEN M3 (M1+M2 already open)
      Assert: all three windows open
    """
    try:
        write_config(session, "climate", "t_max_day",  T_MAX_DAY)
        write_config(session, "climate", "hyst_t",     HYST_T)
        write_config(session, "climate", "rh_ctrl_en", 0)
        time.sleep(NVS_SETTLE_S)

        log.info("  Closing all windows before graduated ventilation sequence ...")
        force_windows_closed(session)
    except Exception as exc:
        for test_id in ("UT-CC-025", "UT-CC-026", "UT-CC-027"):
            results.inconclusive(test_id, raised(exc))
        return

    for test_id, t, deviation, open_keys in (
        ("UT-CC-025", T_STEP1, 1, ("M1",)),
        ("UT-CC-026", T_STEP2, 3, ("M1", "M2")),
        ("UT-CC-027", T_STEP3, 6, WINDOW_KEYS),
    ):
        step = len(open_keys)
        log.info(f"--- {test_id}: Graduated ventilation step {step} "
                 f"({'+'.join(open_keys)}) ---")
        try:
            log.info(f"  Pushing T={t} C (deviation={deviation}, step={step}) ...")
            if not push_and_verify_sensor(session, T=t, RH=RH_NEUTRAL):
                results.inconclusive(test_id, SENSOR_NOT_CONFIRMED)
                continue
            time.sleep(WAIT_FOR_MOTOR_S)
            status = get_status(session)
            log.info(f"  Windows: {wins_str(status)}")
            results.record(
                test_id, windows_exactly(status, *open_keys),
                f"T={t} C -> step={step} -> expected {'+'.join(open_keys)} open, "
                f"the rest CLOSED; got {wins_str(status)}",
            )
        except Exception as exc:
            results.inconclusive(test_id, raised(exc))


def run_cc028(results: Results, session: requests.Session) -> None:
    """
    UT-CC-028 — Night setpoints used when is_daytime=false

    Force is_daytime=false via the location (daytime_candidates()).
    Set t_max_day=40 (so day setpoints would NOT open at T=20) and
    t_max_ngt=18 (night setpoint; T=20 > 18 → step=1 → M1 opens).
    Assert: is_daytime=false in status AND at least one window opens.
    Restores daytime in the finally block for subsequent tests.
    """
    test_id = "UT-CC-028"
    log.info(f"--- {test_id}: Night setpoints used when is_daytime=false ---")
    try:
        write_config(session, "climate", "t_max_day",  40)          # T never triggers day
        write_config(session, "climate", "t_max_ngt",  T_MAX_NGT)   # 18; T=20 > 18 → opens
        write_config(session, "climate", "hyst_t",     HYST_T)
        write_config(session, "climate", "rh_ctrl_en", 0)
        time.sleep(NVS_SETTLE_S)

        log.info("  Forcing is_daytime=false ...")
        if not set_daytime(session, False):
            results.inconclusive(test_id, "could not confirm is_daytime=false")
            return

        force_windows_closed(session)

        log.info(
            f"  Pushing T={T_OPEN_NGT} C (above t_max_ngt={T_MAX_NGT} C, "
            f"below t_max_day=40 C) ..."
        )
        if not push_and_verify_sensor(session, T=T_OPEN_NGT, RH=RH_NEUTRAL):
            results.inconclusive(test_id, SENSOR_NOT_CONFIRMED)
            return

        time.sleep(WAIT_FOR_MOTOR_S)
        status = get_status(session)
        day = is_daytime(status)
        log.info(f"  is_daytime={day}, windows: {wins_str(status)}")

        if day is False and any_window_open_or_moving(status):
            results.record(
                test_id, True,
                f"Night setpoints active: T={T_OPEN_NGT} C > t_max_ngt={T_MAX_NGT} C -> "
                f"opened: {wins_str(status)}",
            )
        elif day is not False:
            results.inconclusive(
                test_id,
                f"is_daytime is {day} again - the night forcing did not hold: {wins_str(status)}",
            )
        else:
            results.record(
                test_id, False,
                f"Night setpoints: no window opened at T={T_OPEN_NGT} C "
                f"(expected > t_max_ngt={T_MAX_NGT} C): {wins_str(status)}",
            )
    except Exception as exc:
        results.inconclusive(test_id, raised(exc))
    finally:
        # Restore daytime so subsequent tests use day setpoints
        log.info("  Restoring daytime after UT-CC-028 ...")
        try:
            set_daytime(session, True)
            write_config(session, "climate", "t_max_day", T_MAX_DAY)
        except Exception as exc:
            log.warning(f"  run_cc028 finally: restore failed - {exc}")


def run_cc029(results: Results, session: requests.Session) -> None:
    """
    UT-CC-029 — Day setpoints used when is_daytime=true

    is_daytime=true (already restored by run_cc028's finally block).
    Set t_max_day=25 (day) and t_max_ngt=12 (night, much lower).
    Discriminator temperature T=14°C:
      - Day  setpoints (t_max_day=25): 14 < 25 → step=0 → windows stay CLOSED
      - Night setpoints (t_max_ngt=12): 14 > 12 → step=1 → windows would OPEN
    Assert: windows remain CLOSED at T=14°C (confirms day setpoints are active).
    """
    test_id = "UT-CC-029"
    log.info(f"--- {test_id}: Day setpoints used when is_daytime=true ---")
    try:
        write_config(session, "climate", "t_max_day",  T_MAX_DAY)   # 25
        write_config(session, "climate", "t_max_ngt",  12)           # low discriminator
        write_config(session, "climate", "hyst_t",     HYST_T)
        write_config(session, "climate", "rh_ctrl_en", 0)
        time.sleep(NVS_SETTLE_S)

        log.info("  Confirming is_daytime=true ...")
        if not set_daytime(session, True):
            results.inconclusive(test_id, "could not confirm is_daytime=true")
            return

        force_windows_closed(session)

        # T=14: above t_max_ngt=12 (would open if night) but below t_max_day=25 (no open if day)
        T_disc = 14
        log.info(
            f"  Pushing T={T_disc} C (> t_max_ngt=12; "
            f"< t_max_day={T_MAX_DAY}) - verifying no open ..."
        )
        if not push_and_verify_sensor(session, T=T_disc, RH=RH_NEUTRAL):
            results.inconclusive(test_id, SENSOR_NOT_CONFIRMED)
            return

        # Wait two polls so any false-open has a chance to appear
        push_sensors(T=T_disc, RH=RH_NEUTRAL)
        time.sleep(WAIT_FOR_SENSOR_S)
        status = get_status(session)
        day = is_daytime(status)
        log.info(f"  is_daytime={day}, windows: {wins_str(status)}")

        if day is True and windows_all_closed(status):
            results.record(
                test_id, True,
                f"Day setpoints active: T={T_disc} C < t_max_day={T_MAX_DAY} C -> "
                f"CLOSED (night t_max_ngt=12 would have opened): {wins_str(status)}",
            )
        elif day is not True:
            results.inconclusive(
                test_id,
                f"is_daytime is {day} again - the day forcing did not hold: {wins_str(status)}",
            )
        else:
            results.record(
                test_id, False,
                f"Windows opened at T={T_disc} C despite day setpoints "
                f"(t_max_day={T_MAX_DAY} C): {wins_str(status)}",
            )
    except Exception as exc:
        results.inconclusive(test_id, raised(exc))
    finally:
        # Reset t_max_ngt to a sensible intermediate value before teardown restores
        try:
            write_config(session, "climate", "t_max_ngt", 20)
        except Exception:
            pass

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    global DEVICE_BASE, WAIT_FOR_MOTOR_S

    log.info("=" * 60)
    log.info("Greenhouse Controller - Setpoints and Hysteresis Test")
    log.info("Test cases: UT-CC-014 ... UT-CC-029 (softwareTestPlan.md 6.3)")
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
            run_cc014(results, session)
            run_cc015(results, session)
            run_cc016(results, session)
            run_cc017(results, session)
            run_cc018(results, session)
            run_cc019(results, session)
            run_cc024(results, session)
            run_cc025_026_027(results, session)
            run_cc028(results, session)
            run_cc029(results, session)

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
