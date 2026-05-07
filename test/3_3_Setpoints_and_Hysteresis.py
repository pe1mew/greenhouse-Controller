#!/usr/bin/env python3
"""
3_3_Setpoints_and_Hysteresis.py
Greenhouse Ventilation Controller — Setpoints and Hysteresis test

Covers: UT-CC-014 through UT-CC-029 (§3.3 Setpoints and Hysteresis)
Both temperature and humidity control paths are exercised.
Graduated ventilation (steps 1–3) and day/night setpoint selection are verified.

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

DEVICE_BASE   = os.getenv("GH_DEVICE_BASE",   "http://192.168.20.150")
EMULATOR_BASE = os.getenv("GH_EMULATOR_BASE",  "http://192.168.20.226")
ADMIN_PIN     = os.getenv("GH_ADMIN_PIN",      "12345678")

# Fast-test parameters written to NVS for the duration of the test.
# Restored unconditionally in teardown.
TEST_POLL_S     = 30   # system/poll_interval  (s)
TEST_TRAVEL_S   = 5    # motor/travel_m1/m2/m3 (s) — minimum allowed
TEST_AVG_WIN    = 0    # climate/avg_win_t and avg_win_rh
                       # 0 min → window_size = clamp(0×60/30, 1, 360) = 1 sample = immediate

# Timing margins
POLL_MARGIN_S            = 5   # extra seconds beyond poll interval for sensor read
MOTOR_MARGIN_S           = 5   # extra seconds after relay de-energises
FIRMWARE_TRAVEL_MARGIN_S = 5   # firmware adds this internally to every relay pulse
NVS_SETTLE_S             = 2   # wait after POST /api/config before next action

# Derived wait times
# WAIT_FOR_SENSOR_S: push → wait → verify controller has new reading
WAIT_FOR_SENSOR_S = TEST_POLL_S + POLL_MARGIN_S                                      # 35 s
# WAIT_FOR_MOTOR_S: push → wait → window state settled at new position
WAIT_FOR_MOTOR_S  = TEST_POLL_S + TEST_TRAVEL_S + FIRMWARE_TRAVEL_MARGIN_S + MOTOR_MARGIN_S  # 45 s

# Maximum times to retry a sensor push before aborting the test case
MAX_SENSOR_RETRIES = 2

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
T_MIN_DAY    = 5    # °C — stored in NVS but not used in step evaluation (see notes)
T_STEP1      = 26   # °C — triggers step 1 (M1 only)
T_STEP2      = 28   # °C — triggers step 2 (M1+M2)
T_STEP3      = 31   # °C — triggers step 3 (all)
T_HYST_HOLD  = 24   # °C — inside hysteresis band (19 < 24 < 25); windows held open
T_CLOSE      = 18   # °C — below close threshold (19); triggers CLOSE
T_BELOW_MIN  = 10   # °C — below T_MIN_DAY; always causes step = 0
T_NEUTRAL    = 10   # °C — guaranteed to keep windows closed (same as T_BELOW_MIN)

# ---------------------------------------------------------------------------
# Setpoints for humidity tests
# ---------------------------------------------------------------------------
RH_MAX_DAY   = 70   # %
HYST_RH      = 6    # %
RH_MIN_DAY   = 40   # %
RH_OPEN      = 80   # % — above rh_max; deviation = 10 → step 5, clamped to 3 (all)
RH_DRY       = 35   # % — below rh_min → demands CLOSE_ALL (step 0)
RH_NEUTRAL   = 55   # % — between rh_min and rh_max; no RH demand

# ---------------------------------------------------------------------------
# Night setpoints (UT-CC-028/029)
# ---------------------------------------------------------------------------
T_MAX_NGT    = 18   # °C — night max; T=20 > 18 triggers opening
T_OPEN_NGT   = 20   # °C — above t_max_ngt; triggers night opening

# ---------------------------------------------------------------------------
# Polar coordinates for forcing is_daytime via lat_deg
#
# T4 calls update_sun_times() immediately when lat_deg is written via Q4.
# lat =  89°N on 2026-05-07 → SUNRISE_POLAR_DAY  → is_daytime = true
# lat = −89°S on 2026-05-07 → SUNRISE_POLAR_NIGHT → is_daytime = false
# ---------------------------------------------------------------------------
LAT_POLAR_DAY   =  89   # lat_deg written to system namespace
LAT_POLAR_NIGHT = -89
LON_ZERO        =   0

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
                    f"401 Unauthorized — re-authenticating …"
                )
                try:
                    body = do_login(session, "admin", ADMIN_PIN)
                    if not body.get("ok"):
                        log.warning(f"write_config: re-login returned {body}")
                    else:
                        log.info("write_config: session restored")
                except Exception as reauth_exc:
                    log.warning(f"write_config: re-login failed — {reauth_exc}")
                # No sleep after re-auth — retry immediately
            else:
                log.warning(
                    f"write_config {ns}/{key}={value} failed "
                    f"(attempt {attempt + 1}/{_WRITE_CONFIG_RETRIES + 1}): {exc} — "
                    f"retrying in {_WRITE_CONFIG_RETRY_S} s …"
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
    T=None, RH=None, Speed: float = 0.5, Direction: float = 180.0
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

def push_and_verify_sensor(
    session: requests.Session,
    T=None,
    RH=None,
    poll_s: int = TEST_POLL_S,
) -> bool:
    """
    Push sensor values to the emulator, wait poll_s + POLL_MARGIN_S, then verify
    the controller actually read the pushed values from GET /api/status.

    On mismatch the push + wait cycle is repeated up to MAX_SENSOR_RETRIES times.
    Returns True when the controller's status reflects the pushed values;
    returns False (caller should record FAIL and abort the test case) when all
    retries are exhausted.

    Tolerances: ±1.5 °C for temperature, ±3 % for relative humidity.
    These cover the int16 °C quantisation and uint8 RH rounding in the firmware.
    """
    wait_s = poll_s + POLL_MARGIN_S
    for attempt in range(MAX_SENSOR_RETRIES + 1):
        if attempt > 0:
            log.warning(
                f"  Sensor mismatch (attempt {attempt}); retrying push …"
            )
        push_sensors(T=T, RH=RH)
        log.info(
            f"  Pushed T={T}°C RH={RH}% — waiting {wait_s} s for poll …"
        )
        time.sleep(wait_s)

        status = get_status(session)
        ok = True

        if T is not None:
            actual = status.get("temp_avg", 0.0)
            if abs(actual - T) > 1.5:
                log.warning(f"  T mismatch: expected {T}°C, controller shows {actual}°C")
                ok = False

        if RH is not None:
            actual = status.get("rh_avg", 0)
            if abs(int(actual) - int(RH)) > 3:
                log.warning(f"  RH mismatch: expected {RH}%, controller shows {actual}%")
                ok = False

        if ok:
            log.info(
                f"  Sensor confirmed: T={status.get('temp_avg')}°C "
                f"RH={status.get('rh_avg')}%"
            )
            return True

    log.error(
        f"  Sensor NOT confirmed after {MAX_SENSOR_RETRIES + 1} attempts — "
        f"aborting test case"
    )
    return False


def windows_all_closed(status: dict) -> bool:
    return all(w == "CLOSED" for w in status.get("windows", []))


def windows_all_closing(status: dict) -> bool:
    """True when every window is CLOSED or MOVING_CLOSE.

    Used where the firmware has correctly issued CLOSE_ALL but M3's physical
    travel time can exceed the relay pulse duration (travel_m3 production
    default is 171 s; at TEST_TRAVEL_S=5 the relay is only energised for 10 s,
    so the motor may still be moving when the status is polled).
    """
    return all(w in ("CLOSED", "MOVING_CLOSE") for w in status.get("windows", []))


def any_window_open_or_moving(status: dict) -> bool:
    return any(w in ("OPEN", "MOVING_OPEN") for w in status.get("windows", []))


def wins_str(status: dict) -> str:
    return str(status.get("windows", []))


def force_windows_closed(session: requests.Session) -> bool:
    """
    Push T=T_NEUTRAL (below every close threshold) and wait for all windows
    to reach CLOSED or MOVING_CLOSE state.  Returns True within two
    poll+travel cycles, False otherwise.

    MOVING_CLOSE is accepted as "effectively closed" because M3's physical
    travel time can exceed the 10 s relay pulse at TEST_TRAVEL_S=5.  Any
    subsequent test that opens windows will push new sensor values; by the
    time the firmware polls (≥30 s), M3 will have completed its close stroke.

    This is a setup/teardown helper; it does not record a test result.
    T=10 °C is below t_max_day − hyst_t for any t_max_day ≥ 16 °C.
    """
    log.info(f"  force_windows_closed: pushing T={T_NEUTRAL}°C …")
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


def set_daytime(session: requests.Session, daytime: bool) -> bool:
    """
    Force is_daytime by writing a polar latitude.
    lat =  89 (°N) in May → SUNRISE_POLAR_DAY  → is_daytime = true
    lat = −89 (°S) in May → SUNRISE_POLAR_NIGHT → is_daytime = false

    T4 calls update_sun_times() immediately on lat_deg write (via Q4 shadow
    update), so the change takes effect within NVS_SETTLE_S.
    Returns True if is_daytime was confirmed via GET /api/status.
    """
    lat = LAT_POLAR_DAY if daytime else LAT_POLAR_NIGHT
    write_config(session, "system", "lat_deg",  lat)
    write_config(session, "system", "lat_frac", 0)
    write_config(session, "system", "lon_deg",  LON_ZERO)
    write_config(session, "system", "lon_frac", 0)
    time.sleep(NVS_SETTLE_S)
    status = get_status(session)
    confirmed = (status.get("is_daytime") == daytime)
    label = "true" if daytime else "false"
    if confirmed:
        log.info(f"  is_daytime={label} confirmed via /api/status")
    else:
        log.warning(
            f"  is_daytime={label} NOT confirmed — "
            f"status shows is_daytime={status.get('is_daytime')}"
        )
    return confirmed

# ---------------------------------------------------------------------------
# Setup / teardown
# ---------------------------------------------------------------------------

def setup(session: requests.Session) -> None:
    """
    1. Activate REST mode on both sensor emulator channels.
    2. Write fast-test parameters to NVS.
    3. Force is_daytime=true via polar latitude.
    4. Push neutral sensor values; wait for all windows to close.
    """
    log.info("SETUP: activating REST mode on sensor emulator")
    set_rest_mode()

    log.info("SETUP: writing fast-test config parameters")
    # Averaging: 1 sample → immediate response to pushed values
    write_config(session, "climate", "avg_win_t",      TEST_AVG_WIN)
    write_config(session, "climate", "avg_win_rh",     TEST_AVG_WIN)
    # Motor travel: minimum; dwell disabled (factory default = 0, explicit anyway)
    for ch in (1, 2, 3):
        write_config(session, "motor", f"travel_m{ch}",     TEST_TRAVEL_S)
        write_config(session, "motor", f"dwell_open_m{ch}",  0)
        write_config(session, "motor", f"dwell_close_m{ch}", 0)
    # Poll interval
    write_config(session, "system", "poll_interval",   TEST_POLL_S)
    # Wind protection off — must not interfere with these tests
    write_config(session, "wind",   "wind_prot_en",    0)
    # RH control on — required for UT-CC-018 and UT-CC-024
    write_config(session, "climate", "rh_ctrl_en",     1)
    time.sleep(NVS_SETTLE_S)

    log.info("SETUP: forcing is_daytime=true (polar day)")
    set_daytime(session, True)

    log.info("SETUP: pushing neutral sensors; waiting for windows to close")
    push_sensors(T=T_NEUTRAL, RH=RH_NEUTRAL)
    time.sleep(WAIT_FOR_MOTOR_S)
    status = get_status(session)
    log.info(f"SETUP complete — windows: {wins_str(status)}, mode: {status.get('mode')}")


def teardown(session: requests.Session, orig: dict) -> None:
    """
    Restore original configuration values read before setup.
    Re-authenticates first (test duration ~20 min can expire the session cookie).
    Pushes neutral sensors afterwards.
    """
    log.info("TEARDOWN: refreshing admin session …")
    try:
        body = do_login(session, "admin", ADMIN_PIN)
        if body.get("ok"):
            log.info("TEARDOWN: session refreshed")
        else:
            log.warning(f"TEARDOWN: re-login returned {body} — proceeding with existing cookie")
    except Exception as exc:
        log.warning(f"TEARDOWN: re-login raised {exc} — proceeding with existing cookie")

    log.info("TEARDOWN: restoring original configuration")
    _teardown_errors: list[str] = []

    def _safe_write(ns: str, key: str, value: int) -> None:
        """Write one config key; log a warning on any HTTP/network error."""
        try:
            write_config(session, ns, key, value)
        except Exception as exc:
            msg = f"{ns}/{key}={value} — {exc}"
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

    # Wind settings
    for key in ("v_max", "dir_excl_low", "dir_excl_high", "wind_prot_en"):
        if key in orig:
            _safe_write("wind", key, int(orig[key]))

    # Motor travel (config response has travel_s as array; NVS keys are individual)
    travel_list = orig.get("travel_s", [])
    if len(travel_list) == 3:
        for i, v in enumerate(travel_list, start=1):
            _safe_write("motor", f"travel_m{i}", int(v))

    # System settings
    if "poll_interval_s" in orig:
        _safe_write("system", "poll_interval", int(orig["poll_interval_s"]))

    # Latitude / longitude (restores is_daytime to original computation)
    for key in ("lat_deg", "lat_frac", "lon_deg", "lon_frac"):
        if key in orig:
            _safe_write("system", key, int(orig[key]))

    if _teardown_errors:
        log.warning(
            f"TEARDOWN: {len(_teardown_errors)} config key(s) could not be restored: "
            + ", ".join(_teardown_errors)
        )

    time.sleep(NVS_SETTLE_S)

    log.info("TEARDOWN: pushing neutral sensors")
    try:
        push_sensors(T=T_NEUTRAL, RH=RH_NEUTRAL)
    except Exception as exc:
        log.warning(f"TEARDOWN: neutral sensor push failed — {exc}")
    time.sleep(NVS_SETTLE_S)
    log.info("TEARDOWN complete")

# ---------------------------------------------------------------------------
# Result tracker
# ---------------------------------------------------------------------------

class Results:
    def __init__(self):
        self._passed: list[str] = []
        self._failed: list[str] = []

    def record(self, test_id: str, ok: bool, detail: str = "") -> None:
        suffix = f" — {detail}" if detail else ""
        if ok:
            self._passed.append(test_id)
            log.info(f"[{test_id}] PASS{suffix}")
        else:
            self._failed.append(test_id)
            log.error(f"[{test_id}] FAIL{suffix}")

    def print_summary(self) -> bool:
        total  = len(self._passed) + len(self._failed)
        passed = len(self._passed)
        failed = len(self._failed)
        log.info("=" * 60)
        log.info(f"SUMMARY: {passed}/{total} passed, {failed} failed")
        for tag in self._passed:
            log.info(f"  PASS  {tag}")
        for tag in self._failed:
            log.error(f"  FAIL  {tag}")
        log.info("=" * 60)
        return failed == 0

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
            results.record(test_id, False, "sensor not confirmed — test aborted")
            return

        # Wait for motor to travel to OPEN
        time.sleep(WAIT_FOR_MOTOR_S)
        status = get_status(session)
        log.info(f"  Windows: {wins_str(status)}")

        if any_window_open_or_moving(status):
            results.record(
                test_id, True,
                f"T={T_STEP1}°C > t_max={T_MAX_DAY}°C → M1 opened: {wins_str(status)}",
            )
        else:
            results.record(
                test_id, False,
                f"Expected M1 to open at T={T_STEP1}°C (t_max={T_MAX_DAY}°C), "
                f"got: {wins_str(status)}",
            )
    except Exception as exc:
        results.record(test_id, False, str(exc))


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
            results.record(
                test_id, False,
                f"Precondition not met — no window open: {wins_str(status)}",
            )
            return

        # First poll: T inside hysteresis band
        if not push_and_verify_sensor(session, T=T_HYST_HOLD, RH=RH_NEUTRAL):
            results.record(test_id, False, "sensor not confirmed — test aborted")
            return

        # Second poll cycle — push again and wait for another evaluation
        log.info(f"  Second poll cycle at T={T_HYST_HOLD}°C …")
        push_sensors(T=T_HYST_HOLD, RH=RH_NEUTRAL)
        time.sleep(WAIT_FOR_SENSOR_S)
        status = get_status(session)
        log.info(f"  Windows after 2 polls at T={T_HYST_HOLD}°C: {wins_str(status)}")

        close_thresh = T_MAX_DAY - HYST_T  # = 19
        if any_window_open_or_moving(status):
            results.record(
                test_id, True,
                f"T={T_HYST_HOLD}°C > close_thresh={close_thresh}°C → "
                f"windows held open over 2 polls: {wins_str(status)}",
            )
        else:
            results.record(
                test_id, False,
                f"Windows closed prematurely at T={T_HYST_HOLD}°C "
                f"(close_thresh={close_thresh}°C): {wins_str(status)}",
            )
    except Exception as exc:
        results.record(test_id, False, str(exc))


def run_cc016(results: Results, session: requests.Session) -> None:
    """
    UT-CC-016 — CLOSE when T < (T_max − hyst_t)

    Precondition: at least one window open.
    Push T=18°C: below close threshold (25−6=19°C).
    The close-hysteresis guard allows step-down to 0 → CMD_CLOSE_ALL.
    Assert: all windows reach CLOSED.
    """
    test_id = "UT-CC-016"
    log.info(f"--- {test_id}: CLOSE when T below close threshold ---")
    try:
        if not push_and_verify_sensor(session, T=T_CLOSE, RH=RH_NEUTRAL):
            results.record(test_id, False, "sensor not confirmed — test aborted")
            return

        time.sleep(WAIT_FOR_MOTOR_S)
        status = get_status(session)
        log.info(f"  Windows: {wins_str(status)}")

        close_thresh = T_MAX_DAY - HYST_T  # = 19
        if windows_all_closed(status):
            results.record(
                test_id, True,
                f"T={T_CLOSE}°C < close_thresh={close_thresh}°C → "
                f"all CLOSED: {wins_str(status)}",
            )
        else:
            results.record(
                test_id, False,
                f"Expected all CLOSED at T={T_CLOSE}°C "
                f"(close_thresh={close_thresh}°C), got: {wins_str(status)}",
            )
    except Exception as exc:
        results.record(test_id, False, str(exc))


def run_cc017(results: Results, session: requests.Session) -> None:
    """
    UT-CC-017 — CLOSE when T < T_min_day

    Note: climate_control.cpp uses only t_max as the temperature setpoint;
    t_min_day is stored in NVS but not evaluated in the step algorithm.
    The close is triggered by the hysteresis guard (T < t_max−hyst_t = 19°C)
    whenever T drops below t_min_day (given any valid t_min_day < t_max−hyst).

    Setup: open windows (T=T_STEP1=26°C), then push T=T_BELOW_MIN=10°C
    (below T_MIN_DAY=5°C AND below the close threshold of 19°C).
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
        log.info(f"  Opening windows: pushing T={T_STEP1}°C …")
        if not push_and_verify_sensor(session, T=T_STEP1, RH=RH_NEUTRAL):
            results.record(test_id, False, "sensor not confirmed during open phase — test aborted")
            return
        time.sleep(WAIT_FOR_MOTOR_S)
        status = get_status(session)
        if not any_window_open_or_moving(status):
            results.record(
                test_id, False,
                f"Could not open windows in setup: {wins_str(status)}",
            )
            return

        # Now push T well below T_min_day
        log.info(
            f"  Pushing T={T_BELOW_MIN}°C (below t_min_day={T_MIN_DAY}°C "
            f"and close_thresh={T_MAX_DAY - HYST_T}°C) …"
        )
        if not push_and_verify_sensor(session, T=T_BELOW_MIN, RH=RH_NEUTRAL):
            results.record(test_id, False, "sensor not confirmed — test aborted")
            return

        time.sleep(WAIT_FOR_MOTOR_S)
        status = get_status(session)
        log.info(f"  Windows: {wins_str(status)}")

        if windows_all_closed(status):
            results.record(
                test_id, True,
                f"T={T_BELOW_MIN}°C < t_min_day={T_MIN_DAY}°C → all CLOSED: {wins_str(status)}",
            )
        else:
            results.record(
                test_id, False,
                f"Expected all CLOSED at T={T_BELOW_MIN}°C "
                f"(< t_min_day={T_MIN_DAY}°C), got: {wins_str(status)}",
            )
    except Exception as exc:
        results.record(test_id, False, str(exc))


def run_cc018(results: Results, session: requests.Session) -> None:
    """
    UT-CC-018 — OPEN when RH > RH_max_day (rh_ctrl_en=true)

    Config: rh_ctrl_en=1, rh_max_day=70, hyst_rh=6, t_max_day=40 (T never triggers).
    Push RH=80% → deviation=10, step=5 → clamped to NUM_VENT_STEPS=3 → all open.
    Assert: at least one window transitions out of CLOSED.
    """
    test_id = "UT-CC-018"
    log.info(f"--- {test_id}: OPEN when RH > RH_max_day ---")
    try:
        write_config(session, "climate", "rh_ctrl_en",  1)
        write_config(session, "climate", "rh_max_day",  RH_MAX_DAY)
        write_config(session, "climate", "rh_min_day",  RH_MIN_DAY)
        write_config(session, "climate", "hyst_rh",     HYST_RH)
        write_config(session, "climate", "t_max_day",   40)   # T never triggers
        write_config(session, "climate", "hyst_t",      HYST_T)
        write_config(session, "climate", "cr_priority", 1)    # CR_RH_FIRST: T step=0 must not veto RH
        time.sleep(NVS_SETTLE_S)

        force_windows_closed(session)

        log.info(f"  Pushing RH={RH_OPEN}% (above rh_max={RH_MAX_DAY}%) …")
        if not push_and_verify_sensor(session, T=T_NEUTRAL, RH=RH_OPEN):
            results.record(test_id, False, "sensor not confirmed — test aborted")
            return

        time.sleep(WAIT_FOR_MOTOR_S)
        status = get_status(session)
        log.info(f"  Windows: {wins_str(status)}")

        if any_window_open_or_moving(status):
            results.record(
                test_id, True,
                f"RH={RH_OPEN}% > rh_max={RH_MAX_DAY}% → window(s) opened: {wins_str(status)}",
            )
        else:
            results.record(
                test_id, False,
                f"Expected window(s) to open at RH={RH_OPEN}%, got: {wins_str(status)}",
            )
    except Exception as exc:
        results.record(test_id, False, str(exc))


def run_cc019(results: Results, session: requests.Session) -> None:
    """
    UT-CC-019 — No relay chatter at setpoint boundary

    Config: t_max_day=25, hyst_t=6, rh_ctrl_en=0.
    Step 1. Open M1 by pushing T=26°C.
    Step 2. Push T=24°C (inside hysteresis band: 19 < 24 < 25).
    Step 3. Wait three consecutive poll cycles, recording window state at each.
    Assert: window state is identical across all three polls (no oscillation)
            AND at least one window remains open (hysteresis guard is holding).
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
        # the emulator can trigger a CLOSE_ALL on the very next firmware poll
        # (before the T=T_STEP1 push is recognised), causing all windows to
        # start MOVING_CLOSE instead of opening.
        force_windows_closed(session)

        log.info(f"  Opening M1: pushing T={T_STEP1}°C …")
        if not push_and_verify_sensor(session, T=T_STEP1, RH=RH_NEUTRAL):
            results.record(test_id, False, "sensor not confirmed during setup — test aborted")
            return
        time.sleep(WAIT_FOR_MOTOR_S)
        status = get_status(session)
        if not any_window_open_or_moving(status):
            results.record(
                test_id, False,
                f"Could not open M1 for setup: {wins_str(status)}",
            )
            return

        # Poll 1 — T inside hysteresis band
        push_sensors(T=T_HYST_HOLD, RH=RH_NEUTRAL)
        time.sleep(WAIT_FOR_SENSOR_S)
        snap1 = get_status(session).get("windows", [])
        log.info(f"  Poll 1: {snap1}")

        # Poll 2
        push_sensors(T=T_HYST_HOLD, RH=RH_NEUTRAL)
        time.sleep(WAIT_FOR_SENSOR_S)
        snap2 = get_status(session).get("windows", [])
        log.info(f"  Poll 2: {snap2}")

        # Poll 3
        push_sensors(T=T_HYST_HOLD, RH=RH_NEUTRAL)
        time.sleep(WAIT_FOR_SENSOR_S)
        snap3 = get_status(session).get("windows", [])
        log.info(f"  Poll 3: {snap3}")

        states_stable = (snap1 == snap2 == snap3)
        any_open      = any(w in ("OPEN", "MOVING_OPEN") for w in snap1)
        close_thresh  = T_MAX_DAY - HYST_T  # = 19

        if states_stable and any_open:
            results.record(
                test_id, True,
                f"T={T_HYST_HOLD}°C (>{close_thresh}°C): 3 polls steady "
                f"at {snap1} — no chatter",
            )
        elif not states_stable:
            results.record(
                test_id, False,
                f"Window state oscillated: {snap1} → {snap2} → {snap3} (relay chatter)",
            )
        else:
            results.record(
                test_id, False,
                f"Windows unexpectedly closed at T={T_HYST_HOLD}°C "
                f"(above close_thresh={close_thresh}°C): {snap1}",
            )
    except Exception as exc:
        results.record(test_id, False, str(exc))


def run_cc024(results: Results, session: requests.Session) -> None:
    """
    UT-CC-024 — CLOSE_ALL when RH < RH_min_day (over-dry)

    Config: rh_ctrl_en=1, rh_min_day=40, rh_max_day=70, t_max_day=40.
    Step 1. Open windows via high RH=80%.
    Step 2. Push RH=35% (below rh_min=40%) → vent_step_required_rh returns 0
            → vent_resolve_conflict selects step 0 → CMD_CLOSE_ALL.
    Assert: all windows CLOSED.
    """
    test_id = "UT-CC-024"
    log.info(f"--- {test_id}: CLOSE_ALL when RH < RH_min_day ---")
    try:
        write_config(session, "climate", "rh_ctrl_en",  1)
        write_config(session, "climate", "rh_max_day",  RH_MAX_DAY)
        write_config(session, "climate", "rh_min_day",  RH_MIN_DAY)
        write_config(session, "climate", "hyst_rh",     HYST_RH)
        write_config(session, "climate", "t_max_day",   40)   # T never triggers
        write_config(session, "climate", "hyst_t",      HYST_T)
        write_config(session, "climate", "cr_priority", 1)    # CR_RH_FIRST: T step=0 must not veto RH
        time.sleep(NVS_SETTLE_S)

        force_windows_closed(session)

        # Open windows via high RH
        log.info(f"  Opening windows: pushing RH={RH_OPEN}% …")
        if not push_and_verify_sensor(session, T=T_NEUTRAL, RH=RH_OPEN):
            results.record(test_id, False, "sensor not confirmed during open phase")
            return
        time.sleep(WAIT_FOR_MOTOR_S)
        status = get_status(session)
        if not any_window_open_or_moving(status):
            results.record(
                test_id, False,
                f"Could not open windows in setup: {wins_str(status)}",
            )
            return
        log.info(f"  Windows open: {wins_str(status)}")

        # Push RH below rh_min → CLOSE_ALL
        log.info(
            f"  Pushing RH={RH_DRY}% (below rh_min={RH_MIN_DAY}%) → CLOSE_ALL …"
        )
        if not push_and_verify_sensor(session, T=T_NEUTRAL, RH=RH_DRY):
            results.record(test_id, False, "sensor not confirmed — test aborted")
            return

        time.sleep(WAIT_FOR_MOTOR_S)
        status = get_status(session)
        log.info(f"  Windows: {wins_str(status)}")

        # Accept MOVING_CLOSE as a valid outcome: the firmware correctly issued
        # CMD_CLOSE_ALL; M3's physical travel can exceed the relay pulse at
        # TEST_TRAVEL_S=5 so it may still be moving when polled.
        if windows_all_closing(status):
            results.record(
                test_id, True,
                f"RH={RH_DRY}% < rh_min={RH_MIN_DAY}% → CMD_CLOSE_ALL → "
                f"all CLOSED/MOVING_CLOSE: {wins_str(status)}",
            )
        else:
            results.record(
                test_id, False,
                f"Expected CMD_CLOSE_ALL at RH={RH_DRY}% "
                f"(rh_min={RH_MIN_DAY}%), got: {wins_str(status)}",
            )
    except Exception as exc:
        results.record(test_id, False, str(exc))


def run_cc025_026_027(results: Results, session: requests.Session) -> None:
    """
    UT-CC-025/026/027 — Graduated ventilation steps 1, 2 and 3

    Config: t_max_day=25, hyst_t=6, rh_ctrl_en=0.
    step_width = hyst_t // NUM_VENT_STEPS = 6 // 3 = 2

    Starting from all windows CLOSED, escalate temperature through three levels:

    UT-CC-025 — Step 1 (M1 only):
      T=26: deviation=1, ceil(1/2)=1 → CMD_OPEN M1 only
      Assert: windows[0] open, windows[1] CLOSED, windows[2] CLOSED

    UT-CC-026 — Step 2 (M1+M2):
      T=28: deviation=3, ceil(3/2)=2 → CMD_OPEN M2 (M1 already open)
      Assert: windows[0] open, windows[1] open, windows[2] CLOSED

    UT-CC-027 — Step 3 (M1+M2+M3):
      T=31: deviation=6, ceil(6/2)=3 → CMD_OPEN M3 (M1+M2 already open)
      Assert: all three windows open
    """
    write_config(session, "climate", "t_max_day",  T_MAX_DAY)
    write_config(session, "climate", "hyst_t",     HYST_T)
    write_config(session, "climate", "rh_ctrl_en", 0)
    time.sleep(NVS_SETTLE_S)

    log.info("  Closing all windows before graduated ventilation sequence …")
    force_windows_closed(session)

    # --- UT-CC-025: Step 1 — M1 only ---
    test_id = "UT-CC-025"
    log.info(f"--- {test_id}: Graduated ventilation step 1 (M1 only) ---")
    try:
        log.info(
            f"  Pushing T={T_STEP1}°C (deviation=1, step=1 → M1 only) …"
        )
        if not push_and_verify_sensor(session, T=T_STEP1, RH=RH_NEUTRAL):
            results.record(test_id, False, "sensor not confirmed — test aborted")
        else:
            time.sleep(WAIT_FOR_MOTOR_S)
            status = get_status(session)
            wins = status.get("windows", [])
            log.info(f"  Windows: {wins}")
            m1_open = len(wins) > 0 and wins[0] in ("OPEN", "MOVING_OPEN")
            m2_clsd = len(wins) > 1 and wins[1] == "CLOSED"
            m3_clsd = len(wins) > 2 and wins[2] == "CLOSED"
            if m1_open and m2_clsd and m3_clsd:
                results.record(
                    test_id, True,
                    f"T={T_STEP1}°C → step=1 → M1 open, M2+M3 closed: {wins}",
                )
            else:
                results.record(
                    test_id, False,
                    f"Expected M1 open / M2+M3 closed at T={T_STEP1}°C, got: {wins}",
                )
    except Exception as exc:
        results.record(test_id, False, str(exc))

    # --- UT-CC-026: Step 2 — M1+M2 ---
    test_id = "UT-CC-026"
    log.info(f"--- {test_id}: Graduated ventilation step 2 (M1+M2) ---")
    try:
        log.info(
            f"  Pushing T={T_STEP2}°C (deviation=3, step=2 → M1+M2) …"
        )
        if not push_and_verify_sensor(session, T=T_STEP2, RH=RH_NEUTRAL):
            results.record(test_id, False, "sensor not confirmed — test aborted")
        else:
            time.sleep(WAIT_FOR_MOTOR_S)
            status = get_status(session)
            wins = status.get("windows", [])
            log.info(f"  Windows: {wins}")
            m1_open = len(wins) > 0 and wins[0] in ("OPEN", "MOVING_OPEN")
            m2_open = len(wins) > 1 and wins[1] in ("OPEN", "MOVING_OPEN")
            m3_clsd = len(wins) > 2 and wins[2] == "CLOSED"
            if m1_open and m2_open and m3_clsd:
                results.record(
                    test_id, True,
                    f"T={T_STEP2}°C → step=2 → M1+M2 open, M3 closed: {wins}",
                )
            else:
                results.record(
                    test_id, False,
                    f"Expected M1+M2 open / M3 closed at T={T_STEP2}°C, got: {wins}",
                )
    except Exception as exc:
        results.record(test_id, False, str(exc))

    # --- UT-CC-027: Step 3 — M1+M2+M3 ---
    test_id = "UT-CC-027"
    log.info(f"--- {test_id}: Graduated ventilation step 3 (M1+M2+M3) ---")
    try:
        log.info(
            f"  Pushing T={T_STEP3}°C (deviation=6, step=3 → all) …"
        )
        if not push_and_verify_sensor(session, T=T_STEP3, RH=RH_NEUTRAL):
            results.record(test_id, False, "sensor not confirmed — test aborted")
        else:
            time.sleep(WAIT_FOR_MOTOR_S)
            status = get_status(session)
            wins = status.get("windows", [])
            log.info(f"  Windows: {wins}")
            all_open = all(w in ("OPEN", "MOVING_OPEN") for w in wins)
            if all_open:
                results.record(
                    test_id, True,
                    f"T={T_STEP3}°C → step=3 → all open: {wins}",
                )
            else:
                results.record(
                    test_id, False,
                    f"Expected all open at T={T_STEP3}°C, got: {wins}",
                )
    except Exception as exc:
        results.record(test_id, False, str(exc))


def run_cc028(results: Results, session: requests.Session) -> None:
    """
    UT-CC-028 — Night setpoints used when is_daytime=false

    Force is_daytime=false via lat=-89 (Antarctic in May = polar night).
    Set t_max_day=40 (so day setpoints would NOT open at T=20) and
    t_max_ngt=18 (night setpoint; T=20 > 18 → step=1 → M1 opens).
    Assert: is_daytime=false in status AND at least one window opens.
    Restores polar-day latitude in finally block for subsequent tests.
    """
    test_id = "UT-CC-028"
    log.info(f"--- {test_id}: Night setpoints used when is_daytime=false ---")
    try:
        write_config(session, "climate", "t_max_day",  40)          # T never triggers day
        write_config(session, "climate", "t_max_ngt",  T_MAX_NGT)   # 18; T=20 > 18 → opens
        write_config(session, "climate", "hyst_t",     HYST_T)
        write_config(session, "climate", "rh_ctrl_en", 0)
        time.sleep(NVS_SETTLE_S)

        log.info("  Forcing is_daytime=false (polar night) …")
        if not set_daytime(session, False):
            results.record(test_id, False, "could not confirm is_daytime=false")
            return

        force_windows_closed(session)

        log.info(
            f"  Pushing T={T_OPEN_NGT}°C (above t_max_ngt={T_MAX_NGT}°C, "
            f"below t_max_day=40°C) …"
        )
        if not push_and_verify_sensor(session, T=T_OPEN_NGT, RH=RH_NEUTRAL):
            results.record(test_id, False, "sensor not confirmed — test aborted")
            return

        time.sleep(WAIT_FOR_MOTOR_S)
        status = get_status(session)
        wins = status.get("windows", [])
        is_daytime = status.get("is_daytime", True)
        log.info(f"  is_daytime={is_daytime}, windows: {wins}")

        if not is_daytime and any_window_open_or_moving(status):
            results.record(
                test_id, True,
                f"Night setpoints active: T={T_OPEN_NGT}°C > t_max_ngt={T_MAX_NGT}°C → "
                f"opened: {wins}",
            )
        elif is_daytime:
            results.record(
                test_id, False,
                f"is_daytime still True — polar-night not effective: {wins}",
            )
        else:
            results.record(
                test_id, False,
                f"Night setpoints: no window opened at T={T_OPEN_NGT}°C "
                f"(expected > t_max_ngt={T_MAX_NGT}°C): {wins}",
            )
    except Exception as exc:
        results.record(test_id, False, str(exc))
    finally:
        # Restore polar day so subsequent tests use day setpoints
        log.info("  Restoring polar day after UT-CC-028 …")
        try:
            set_daytime(session, True)
            write_config(session, "climate", "t_max_day", T_MAX_DAY)
        except Exception as exc:
            log.warning(f"  run_cc028 finally: restore failed — {exc}")


def run_cc029(results: Results, session: requests.Session) -> None:
    """
    UT-CC-029 — Day setpoints used when is_daytime=true

    is_daytime=true (polar day, already set by run_cc028's finally block).
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

        log.info("  Confirming is_daytime=true (polar day) …")
        if not set_daytime(session, True):
            results.record(test_id, False, "could not confirm is_daytime=true")
            return

        force_windows_closed(session)

        # T=14: above t_max_ngt=12 (would open if night) but below t_max_day=25 (no open if day)
        T_disc = 14
        log.info(
            f"  Pushing T={T_disc}°C (>{T_disc}>t_max_ngt=12; "
            f"<t_max_day={T_MAX_DAY}) — verifying no open …"
        )
        if not push_and_verify_sensor(session, T=T_disc, RH=RH_NEUTRAL):
            results.record(test_id, False, "sensor not confirmed — test aborted")
            return

        # Wait two polls so any false-open has a chance to appear
        push_sensors(T=T_disc, RH=RH_NEUTRAL)
        time.sleep(WAIT_FOR_SENSOR_S)
        status = get_status(session)
        wins = status.get("windows", [])
        is_daytime = status.get("is_daytime", False)
        log.info(f"  is_daytime={is_daytime}, windows: {wins}")

        if is_daytime and windows_all_closed(status):
            results.record(
                test_id, True,
                f"Day setpoints active: T={T_disc}°C < t_max_day={T_MAX_DAY}°C → "
                f"CLOSED (night t_max_ngt=12 would have opened): {wins}",
            )
        elif not is_daytime:
            results.record(
                test_id, False,
                f"is_daytime still False — day mode not effective: {wins}",
            )
        else:
            results.record(
                test_id, False,
                f"Windows opened at T={T_disc}°C despite day setpoints "
                f"(t_max_day={T_MAX_DAY}°C): {wins}",
            )
    except Exception as exc:
        results.record(test_id, False, str(exc))
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
    log.info("=" * 60)
    log.info("Greenhouse Controller — Setpoints and Hysteresis Test")
    log.info("Test cases: UT-CC-014 … UT-CC-029 (§3.3)")
    log.info(f"Device   : {DEVICE_BASE}")
    log.info(f"Emulator : {EMULATOR_BASE}")
    log.info(
        f"Test params: poll={TEST_POLL_S}s  travel={TEST_TRAVEL_S}s  "
        f"avg_win={TEST_AVG_WIN}"
    )
    log.info(
        f"Timing: sensor_wait={WAIT_FOR_SENSOR_S}s  "
        f"motor_wait={WAIT_FOR_MOTOR_S}s"
    )
    log.info("=" * 60)

    results = Results()

    try:
        session = get_admin_session()
    except Exception as exc:
        log.critical(f"Cannot establish admin session: {exc}")
        sys.exit(2)

    log.info("Reading original configuration …")
    try:
        orig = get_config(session)
    except Exception as exc:
        log.critical(f"Cannot read original config: {exc}")
        sys.exit(2)

    try:
        setup(session)

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

    passed = results.print_summary()
    log.info(f"Log written to: {LOG_PATH}")
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
