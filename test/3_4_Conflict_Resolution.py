#!/usr/bin/env python3
"""
3_4_Conflict_Resolution.py
Greenhouse Ventilation Controller — Conflict Resolution test

Covers: UT-CC-020, UT-CC-021, UT-CC-022, UT-CC-030, UT-CC-031 (§3.4 Conflict Resolution)
All four branches of vent_resolve_conflict() are exercised:
  Rule 1 — RH neutral, T decides (verified implicitly by every prior test)
  Rule 2 — both want OPEN → max(step_t, step_rh)           (CC-022a)
  Rule 3 — both want CLOSE → step_t == step_rh == 0         (CC-022b)
  Rule 4 — genuine conflict, outcome determined by cr_priority:
             CR_TEMP_FIRST (0): T wins                       (CC-020, CC-021)
             CR_RH_FIRST   (1): RH wins                      (CC-030)
             CR_DEVIATION  (2): max(step_t, step_rh) wins    (CC-031)

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
# Climate setpoints used for all conflict tests
# ---------------------------------------------------------------------------
#
# t_max_day = 25 °C,  hyst_t = 6 °C → step_width = hyst_t // NUM_VENT_STEPS = 2
#
# Temperature step table (step_from_deviation algorithm):
#   T = 26 °C: deviation = 1,  ceil(1/2) = 1  → step_t = 1 (M1 only)
#   T = 28 °C: deviation = 3,  ceil(3/2) = 2  → step_t = 2 (M1+M2)
#   T = 10 °C: below close threshold (25−6=19) → step_t = 0 (close)
#
# RH step table (step_from_rh algorithm):
#   RH = 80 %: above rh_max=70 → step_rh = 5, clamped to NUM_VENT_STEPS=3 (all open)
#   RH = 35 %: below rh_min=40 → step_rh = 0 (over-dry; CLOSE_ALL demanded)
#   RH = 55 %: inside [rh_min, rh_max] → step_rh = VENT_STEP_NEUTRAL (−1; no RH vote)
#
T_MAX_DAY  = 25   # °C
HYST_T     = 6    # °C
T_OPEN_S1  = 26   # °C — step_t = 1
T_OPEN_S2  = 28   # °C — step_t = 2
T_CLOSE    = 10   # °C — step_t = 0 (well below close threshold 19 °C)
T_NEUTRAL  = 10   # °C — same as T_CLOSE; VENT_STEP_NEUTRAL not applicable to T

RH_MAX_DAY = 70   # %
RH_MIN_DAY = 40   # %
HYST_RH    = 6    # %
RH_OPEN    = 80   # % — step_rh = 3 (all-open demand)
RH_DRY     = 35   # % — step_rh = 0 (over-dry; CLOSE demand)
RH_NEUTRAL = 55   # % — step_rh = VENT_STEP_NEUTRAL (inside band; no RH vote)

# cr_priority values (climate/cr_priority NVS key)
CR_TEMP_FIRST = 0   # Rule 4: step_t wins
CR_RH_FIRST   = 1   # Rule 4: step_rh wins
CR_DEVIATION  = 2   # Rule 4: max(step_t, step_rh) wins

# ---------------------------------------------------------------------------
# Polar coordinates for forcing is_daytime via lat_deg
#
# T4 calls update_sun_times() immediately when lat_deg is written via Q4.
# lat =  89°N on 2026-05-07 → SUNRISE_POLAR_DAY  → is_daytime = true
# All conflict tests use day setpoints; setup forces is_daytime = true.
# ---------------------------------------------------------------------------
LAT_POLAR_DAY   =  89
LAT_POLAR_NIGHT = -89
LON_ZERO        =   0

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


def windows_all_open(status: dict) -> bool:
    """True when every window is OPEN or MOVING_OPEN."""
    return all(w in ("OPEN", "MOVING_OPEN") for w in status.get("windows", []))


def any_window_open_or_moving(status: dict) -> bool:
    return any(w in ("OPEN", "MOVING_OPEN") for w in status.get("windows", []))


def wins_str(status: dict) -> str:
    return str(status.get("windows", []))


def force_windows_closed(session: requests.Session) -> bool:
    """
    Push T=T_NEUTRAL (below every close threshold) and RH=RH_NEUTRAL (inside
    the RH band → VENT_STEP_NEUTRAL → Rule 1 fires → step = step_t = 0).
    Waits for all windows to reach CLOSED or MOVING_CLOSE.

    Using RH=RH_NEUTRAL ensures this helper is safe regardless of the current
    cr_priority setting: step_rh = VENT_STEP_NEUTRAL triggers Rule 1 before
    cr_priority is consulted, so T's step=0 always wins.

    Returns True within two poll+travel cycles, False otherwise.
    MOVING_CLOSE is accepted as "effectively closed" (M3 physical travel can
    exceed the 10 s relay pulse at TEST_TRAVEL_S=5).
    """
    log.info(f"  force_windows_closed: pushing T={T_NEUTRAL}°C RH={RH_NEUTRAL}% …")
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
    3. Force is_daytime=true (day setpoints used throughout).
    4. Push neutral sensors; wait for all windows to close.
    """
    log.info("SETUP: activating REST mode on sensor emulator")
    set_rest_mode()

    log.info("SETUP: writing fast-test config parameters")
    # Averaging: 1 sample → immediate response to pushed values
    write_config(session, "climate", "avg_win_t",      TEST_AVG_WIN)
    write_config(session, "climate", "avg_win_rh",     TEST_AVG_WIN)
    # Motor travel: minimum; dwell disabled
    for ch in (1, 2, 3):
        write_config(session, "motor", f"travel_m{ch}",      TEST_TRAVEL_S)
        write_config(session, "motor", f"dwell_open_m{ch}",   0)
        write_config(session, "motor", f"dwell_close_m{ch}",  0)
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
    write_config(session, "climate", "hyst_t",         HYST_T)
    write_config(session, "climate", "rh_max_day",     RH_MAX_DAY)
    write_config(session, "climate", "rh_min_day",     RH_MIN_DAY)
    write_config(session, "climate", "hyst_rh",        HYST_RH)
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
    Re-authenticates first (test duration ~15 min can expire the session cookie).
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

def run_cc020(results: Results, session: requests.Session) -> None:
    """
    UT-CC-020 — CR_TEMP_FIRST: T demands OPEN, RH demands CLOSE → T wins → M1 opens

    Scenario: T=26°C (step_t=1, above t_max=25) vs RH=35% (step_rh=0, below
    rh_min=40 → over-dry CLOSE demand).  cr_priority=0 (CR_TEMP_FIRST).

    Rule 4 fires (step_t=1 vs step_rh=0, genuine conflict).
    CR_TEMP_FIRST → step_t wins → step=1 → CMD_OPEN M1 only.

    Assert: M1 opens (OPEN or MOVING_OPEN), M2 and M3 remain CLOSED.

    Mirror test: UT-CC-030 uses the same sensor inputs with CR_RH_FIRST, where
    RH's step=0 wins and windows stay closed.
    """
    test_id = "UT-CC-020"
    log.info(f"--- {test_id}: CR_TEMP_FIRST — T open (step=1) wins over RH close (step=0) ---")
    try:
        write_config(session, "climate", "cr_priority", CR_TEMP_FIRST)
        time.sleep(NVS_SETTLE_S)

        force_windows_closed(session)

        log.info(
            f"  Pushing T={T_OPEN_S1}°C (step_t=1) + RH={RH_DRY}% (step_rh=0) — "
            f"cr_priority={CR_TEMP_FIRST} (CR_TEMP_FIRST) …"
        )
        if not push_and_verify_sensor(session, T=T_OPEN_S1, RH=RH_DRY):
            results.record(test_id, False, "sensor not confirmed — test aborted")
            return

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
                f"CR_TEMP_FIRST: T=step_t=1 vs RH=step_rh=0 → T wins → "
                f"M1 open, M2+M3 closed: {wins}",
            )
        elif not m1_open:
            results.record(
                test_id, False,
                f"M1 did not open — expected T step=1 to win; got: {wins}",
            )
        else:
            results.record(
                test_id, False,
                f"Expected M1 open / M2+M3 closed, got: {wins}",
            )
    except Exception as exc:
        results.record(test_id, False, str(exc))


def run_cc021(results: Results, session: requests.Session) -> None:
    """
    UT-CC-021 — CR_TEMP_FIRST: T demands CLOSE (step=0), RH demands OPEN (step=3) → T wins → stays CLOSED

    Scenario: T=10°C (step_t=0, well below close threshold 19°C) vs RH=80%
    (step_rh=3, above rh_max=70).  cr_priority=0 (CR_TEMP_FIRST).

    Rule 4 fires (step_t=0 vs step_rh=3, genuine conflict).
    CR_TEMP_FIRST → step_t wins → step=0 → CLOSE (windows remain closed despite
    RH demanding full-open).

    Two consecutive poll cycles are observed to confirm stability (no spurious
    opening on any cycle).

    Assert: all windows CLOSED after 2 polls.

    Mirror test: UT-CC-031a uses the same sensor inputs with CR_DEVIATION, where
    max(0, 3)=3 causes all windows to open.
    """
    test_id = "UT-CC-021"
    log.info(f"--- {test_id}: CR_TEMP_FIRST — T close (step=0) wins over RH open (step=3) ---")
    try:
        write_config(session, "climate", "cr_priority", CR_TEMP_FIRST)
        time.sleep(NVS_SETTLE_S)

        force_windows_closed(session)

        # Poll 1: push and verify sensor read; windows should remain closed
        log.info(
            f"  Poll 1: pushing T={T_CLOSE}°C (step_t=0) + RH={RH_OPEN}% (step_rh=3) — "
            f"cr_priority={CR_TEMP_FIRST} (CR_TEMP_FIRST) …"
        )
        if not push_and_verify_sensor(session, T=T_CLOSE, RH=RH_OPEN):
            results.record(test_id, False, "sensor not confirmed — test aborted")
            return

        # Poll 2: second cycle to confirm no delayed opening
        log.info(f"  Poll 2: second cycle at T={T_CLOSE}°C RH={RH_OPEN}% …")
        push_sensors(T=T_CLOSE, RH=RH_OPEN)
        time.sleep(WAIT_FOR_SENSOR_S)
        status = get_status(session)
        wins = status.get("windows", [])
        log.info(f"  Windows after 2 polls: {wins}")

        if windows_all_closed(status):
            results.record(
                test_id, True,
                f"CR_TEMP_FIRST: T=step_t=0 vs RH=step_rh=3 → T wins → "
                f"windows stayed CLOSED over 2 polls despite RH={RH_OPEN}%: {wins}",
            )
        else:
            results.record(
                test_id, False,
                f"Windows opened despite CR_TEMP_FIRST (step_t=0 should veto RH open): {wins}",
            )
    except Exception as exc:
        results.record(test_id, False, str(exc))


def run_cc022(results: Results, session: requests.Session) -> None:
    """
    UT-CC-022 — No conflict when both demand the same action

    Two sub-cases covering the two "no conflict" rules:

    UT-CC-022a — Rule 2: both demand OPEN (step_t > 0 AND step_rh > 0)
      T=26°C (step_t=1) + RH=80% (step_rh=3) → both positive → Rule 2 fires.
      Return max(step_t, step_rh) = max(1, 3) = 3 → CMD_OPEN all three windows.
      Assert: all windows OPEN or MOVING_OPEN.

    UT-CC-022b — Rule 3: both demand CLOSE (step_t == step_rh == 0)
      (Windows open from CC-022a.)
      T=10°C (step_t=0) + RH=35% (step_rh=0) → equal → Rule 3 fires.
      Return step_t = 0 → CMD_CLOSE_ALL.
      Assert: all windows CLOSED or MOVING_CLOSE.

    cr_priority is irrelevant for both sub-cases (Rules 2 and 3 fire before
    Rule 4 which checks cr_priority).
    """
    write_config(session, "climate", "cr_priority", CR_TEMP_FIRST)  # irrelevant; set for consistency
    time.sleep(NVS_SETTLE_S)

    # --- UT-CC-022a: Rule 2 — both want OPEN → max ---
    test_id = "UT-CC-022a"
    log.info(f"--- {test_id}: Rule 2 — both demand OPEN → max(step_t, step_rh) = 3 ---")
    try:
        log.info("  Closing all windows before Rule 2 test …")
        force_windows_closed(session)

        log.info(
            f"  Pushing T={T_OPEN_S1}°C (step_t=1) + RH={RH_OPEN}% (step_rh=3) — "
            f"Rule 2: both>0 → max(1,3)=3 → all open …"
        )
        if not push_and_verify_sensor(session, T=T_OPEN_S1, RH=RH_OPEN):
            results.record(test_id, False, "sensor not confirmed — test aborted")
        else:
            time.sleep(WAIT_FOR_MOTOR_S)
            status = get_status(session)
            wins = status.get("windows", [])
            log.info(f"  Windows: {wins}")

            if windows_all_open(status):
                results.record(
                    test_id, True,
                    f"Rule 2: step_t=1 + step_rh=3 (both>0) → max=3 → "
                    f"all open: {wins}",
                )
            else:
                results.record(
                    test_id, False,
                    f"Expected all open (Rule 2, max(1,3)=3), got: {wins}",
                )
    except Exception as exc:
        results.record(test_id, False, str(exc))

    # --- UT-CC-022b: Rule 3 — both want CLOSE → step_t == step_rh == 0 ---
    test_id = "UT-CC-022b"
    log.info(f"--- {test_id}: Rule 3 — both demand CLOSE → step_t == step_rh == 0 ---")
    try:
        # Precondition: windows open from CC-022a (no force_close needed)
        # If CC-022a failed, this will start from whatever state; still valid.
        log.info(
            f"  Pushing T={T_CLOSE}°C (step_t=0) + RH={RH_DRY}% (step_rh=0) — "
            f"Rule 3: equal (both 0) → step=0 → CLOSE_ALL …"
        )
        if not push_and_verify_sensor(session, T=T_CLOSE, RH=RH_DRY):
            results.record(test_id, False, "sensor not confirmed — test aborted")
        else:
            time.sleep(WAIT_FOR_MOTOR_S)
            status = get_status(session)
            wins = status.get("windows", [])
            log.info(f"  Windows: {wins}")

            if windows_all_closing(status):
                results.record(
                    test_id, True,
                    f"Rule 3: step_t=0 == step_rh=0 → step=0 → "
                    f"all CLOSED/MOVING_CLOSE: {wins}",
                )
            else:
                results.record(
                    test_id, False,
                    f"Expected all CLOSED/MOVING_CLOSE (Rule 3, step=0), got: {wins}",
                )
    except Exception as exc:
        results.record(test_id, False, str(exc))


def run_cc030(results: Results, session: requests.Session) -> None:
    """
    UT-CC-030 — CR_RH_FIRST: RH demands CLOSE (step=0), T demands OPEN (step=1) → RH wins → stays CLOSED

    Scenario: T=26°C (step_t=1) vs RH=35% (step_rh=0).  cr_priority=1 (CR_RH_FIRST).

    Rule 4 fires (step_t=1 vs step_rh=0, genuine conflict).
    CR_RH_FIRST → step_rh wins → step=0 → CLOSE (windows remain closed despite
    T demanding opening).

    Two consecutive poll cycles confirm stability (no spurious opening).

    Assert: all windows CLOSED after 2 polls.

    This is the mirror of UT-CC-020 (same inputs, CR_RH_FIRST instead of
    CR_TEMP_FIRST, opposite outcome).
    """
    test_id = "UT-CC-030"
    log.info(f"--- {test_id}: CR_RH_FIRST — RH close (step=0) wins over T open (step=1) ---")
    try:
        write_config(session, "climate", "cr_priority", CR_RH_FIRST)
        time.sleep(NVS_SETTLE_S)

        force_windows_closed(session)

        # Poll 1: push and verify; windows should remain closed
        log.info(
            f"  Poll 1: pushing T={T_OPEN_S1}°C (step_t=1) + RH={RH_DRY}% (step_rh=0) — "
            f"cr_priority={CR_RH_FIRST} (CR_RH_FIRST) …"
        )
        if not push_and_verify_sensor(session, T=T_OPEN_S1, RH=RH_DRY):
            results.record(test_id, False, "sensor not confirmed — test aborted")
            return

        # Poll 2: confirm no delayed opening
        log.info(f"  Poll 2: second cycle at T={T_OPEN_S1}°C RH={RH_DRY}% …")
        push_sensors(T=T_OPEN_S1, RH=RH_DRY)
        time.sleep(WAIT_FOR_SENSOR_S)
        status = get_status(session)
        wins = status.get("windows", [])
        log.info(f"  Windows after 2 polls: {wins}")

        if windows_all_closed(status):
            results.record(
                test_id, True,
                f"CR_RH_FIRST: T=step_t=1 vs RH=step_rh=0 → RH wins → "
                f"windows stayed CLOSED over 2 polls despite T={T_OPEN_S1}°C: {wins}",
            )
        else:
            results.record(
                test_id, False,
                f"Windows opened despite CR_RH_FIRST (step_rh=0 should veto T open): {wins}",
            )
    except Exception as exc:
        results.record(test_id, False, str(exc))


def run_cc031(results: Results, session: requests.Session) -> None:
    """
    UT-CC-031 — CR_DEVIATION: higher step wins (max of the two step values)

    Two sub-cases:

    UT-CC-031a — RH step higher: T=10°C (step_t=0) vs RH=80% (step_rh=3)
      Rule 4 fires, CR_DEVIATION → max(step_t, step_rh) = max(0, 3) = 3.
      All three windows open.
      Assert: all windows OPEN or MOVING_OPEN.
      (Mirror of UT-CC-021: same inputs, CR_DEVIATION instead of CR_TEMP_FIRST.)

    UT-CC-031b — T step higher: T=28°C (step_t=2) vs RH=35% (step_rh=0)
      Rule 4 fires, CR_DEVIATION → max(step_t, step_rh) = max(2, 0) = 2.
      M1 + M2 open; M3 stays closed.
      Assert: M1 and M2 open, M3 closed.
      (This matches the UT-CC-031 scenario described in §3.4 of the test plan.)
    """
    write_config(session, "climate", "cr_priority", CR_DEVIATION)
    time.sleep(NVS_SETTLE_S)

    # --- UT-CC-031a: RH step higher → all open ---
    test_id = "UT-CC-031a"
    log.info(f"--- {test_id}: CR_DEVIATION — max(0, 3) = 3 → all windows open ---")
    try:
        log.info("  Closing all windows before CC-031a …")
        force_windows_closed(session)

        log.info(
            f"  Pushing T={T_CLOSE}°C (step_t=0) + RH={RH_OPEN}% (step_rh=3) — "
            f"cr_priority={CR_DEVIATION} (CR_DEVIATION) → max(0,3)=3 …"
        )
        if not push_and_verify_sensor(session, T=T_CLOSE, RH=RH_OPEN):
            results.record(test_id, False, "sensor not confirmed — test aborted")
        else:
            time.sleep(WAIT_FOR_MOTOR_S)
            status = get_status(session)
            wins = status.get("windows", [])
            log.info(f"  Windows: {wins}")

            if windows_all_open(status):
                results.record(
                    test_id, True,
                    f"CR_DEVIATION: step_t=0 vs step_rh=3 → max=3 → "
                    f"all open: {wins}",
                )
            else:
                results.record(
                    test_id, False,
                    f"Expected all open (CR_DEVIATION, max(0,3)=3), got: {wins}",
                )
    except Exception as exc:
        results.record(test_id, False, str(exc))

    # --- UT-CC-031b: T step higher → M1+M2 open, M3 closed ---
    test_id = "UT-CC-031b"
    log.info(f"--- {test_id}: CR_DEVIATION — max(2, 0) = 2 → M1+M2 open, M3 closed ---")
    try:
        log.info("  Closing all windows before CC-031b …")
        force_windows_closed(session)

        log.info(
            f"  Pushing T={T_OPEN_S2}°C (step_t=2) + RH={RH_DRY}% (step_rh=0) — "
            f"cr_priority={CR_DEVIATION} (CR_DEVIATION) → max(2,0)=2 …"
        )
        if not push_and_verify_sensor(session, T=T_OPEN_S2, RH=RH_DRY):
            results.record(test_id, False, "sensor not confirmed — test aborted")
        else:
            time.sleep(WAIT_FOR_MOTOR_S)
            status = get_status(session)
            wins = status.get("windows", [])
            log.info(f"  Windows: {wins}")

            m1_open = len(wins) > 0 and wins[0] in ("OPEN", "MOVING_OPEN")
            m2_open = len(wins) > 1 and wins[1] in ("OPEN", "MOVING_OPEN")
            # Accept MOVING_CLOSE: step=2 correctly commanded M3 to close;
            # M3's physical travel can exceed the relay pulse at TEST_TRAVEL_S=5,
            # so the FSM may still be in MOVING_CLOSE when the status is polled.
            m3_clsd = len(wins) > 2 and wins[2] in ("CLOSED", "MOVING_CLOSE")

            if m1_open and m2_open and m3_clsd:
                results.record(
                    test_id, True,
                    f"CR_DEVIATION: step_t=2 vs step_rh=0 → max=2 → "
                    f"M1+M2 open, M3 CLOSED/MOVING_CLOSE: {wins}",
                )
            else:
                results.record(
                    test_id, False,
                    f"Expected M1+M2 open / M3 CLOSED or MOVING_CLOSE "
                    f"(CR_DEVIATION, max(2,0)=2), got: {wins}",
                )
    except Exception as exc:
        results.record(test_id, False, str(exc))

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    log.info("=" * 60)
    log.info("Greenhouse Controller — Conflict Resolution Test")
    log.info("Test cases: UT-CC-020, UT-CC-021, UT-CC-022, UT-CC-030, UT-CC-031 (§3.4)")
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

        run_cc020(results, session)
        run_cc021(results, session)
        run_cc022(results, session)
        run_cc030(results, session)
        run_cc031(results, session)

    finally:
        teardown(session, orig)

    passed = results.print_summary()
    log.info(f"Log written to: {LOG_PATH}")
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
