/**
 * @file cfg_limits.h
 * @brief Single source of truth for all integer config parameter bounds.
 *
 * Consumed by three layers so the same numbers never need to be maintained
 * in more than one place:
 *
 *   1. data_manager.cpp  — cfg_clamp() enforces these before every NVS write
 *   2. ui_display.cpp    — param_def_t tables use them as the keypad edit range
 *   3. web_server.cpp    — GET /api/config/limits returns them as JSON; app.js
 *                          applies them to every <input> element on page load
 *
 * Anti-oscillation critical minimums (see simulation/simulationOptimisation.md):
 *   CFG_MIN_HYST_T  = 2   narrower dead band collapses step_width to 0
 *   CFG_MIN_V_MAX   = 1   0 would permanently assert wind override
 *   CFG_MIN_POLL_S  = 15  matches FR-S03 / FR-CF07 and T5's own SP_POLL_MIN_S
 */

#pragma once

/* ── Temperature thresholds (°C) ─────────────────────────────────────────── */
#define CFG_MIN_T_MAX_DAY    15
#define CFG_MAX_T_MAX_DAY    45
#define CFG_MIN_T_MIN_DAY     5
#define CFG_MAX_T_MIN_DAY    40
#define CFG_MIN_T_MAX_NGT    10
#define CFG_MAX_T_MAX_NGT    35
#define CFG_MIN_T_MIN_NGT     0
#define CFG_MAX_T_MIN_NGT    30

/* ── Relative humidity thresholds (%) ────────────────────────────────────── */
#define CFG_MIN_RH_MAX       40
#define CFG_MAX_RH_MAX       98
#define CFG_MIN_RH_MIN       20
#define CFG_MAX_RH_MIN       90

/* -- Conflict resolution (2.5.0, gh#57) ----------------------------------- */
/* vent_resolve_conflict() (drivers/ventModel/src/vent_model_stepped.cpp since
 * 2.12.0; climate_control.cpp before it) implements exactly 0/1/2, and
 * `case 0:` shares an arm with `default:` — so before 2.5.0 an out-of-range
 * cr_priority was stored verbatim and silently degraded to TEMP_FIRST.
 *   0 = CR_TEMP_FIRST   1 = CR_RH_FIRST   2 = CR_DEVIATION (higher step wins) */
#define CFG_MIN_CR_PRIORITY   0
#define CFG_MAX_CR_PRIORITY   2

/* ── Hysteresis ───────────────────────────────────────────────────────────── */
/* Must be ≥ 2: with NUM_VENT_STEPS=3, hyst/3 = step_width; below 2 it rounds
 * to 0 and the floor-to-1 gives a 1-unit effective dead band. */
#define CFG_MIN_HYST_T        2
#define CFG_MAX_HYST_T       15
#define CFG_MIN_HYST_RH       2
#define CFG_MAX_HYST_RH      20

/* ── Averaging window (minutes) ──────────────────────────────────────────── */
/* 1 = raw sample; handled safely by sensor_poll but gives no noise rejection. */
#define CFG_MIN_AVG_WIN       1
#define CFG_MAX_AVG_WIN      30

/* ── Wind ─────────────────────────────────────────────────────────────────── */
#define CFG_MIN_V_MAX         1   /* 0 permanently triggers wind override */
#define CFG_MAX_V_MAX        30
#define CFG_MIN_DIR           0
#define CFG_MAX_DIR         359
/* Speed-hysteresis dead band (2.3.0, gh#46). 0 = legacy single-threshold.
 * Static clamp; T3 additionally caps the effective value at v_max - 1 at
 * runtime so the override can always clear (belt + braces). */
#define CFG_MIN_WIND_HYST     0
#define CFG_MAX_WIND_HYST     5

/* ── Motor (seconds) ─────────────────────────────────────────────────────── */
#define CFG_MIN_TRAVEL_S      5   /* below 5 s motor cannot complete full stroke */
#define CFG_MAX_TRAVEL_S    300
#define CFG_MIN_DWELL_OPEN_S    0   /* 0 = no hold; higher values reduce oscillation */
#define CFG_MAX_DWELL_OPEN_S 1500   /* M3 may need up to 25 min hold to suppress slow RH oscillation (kas-2 calibration) */
#define CFG_MIN_DWELL_CLOSE_S   0
#define CFG_MAX_DWELL_CLOSE_S 1500  /* matched to dwell_open ceiling so M3 can run a symmetric closed-state hold */

/* ── Motor: linear-control deadband (millimetres) ─────────────────────────
 *
 * Minimum position error worth energising the motor for, under linear control.
 * The complement of dwell_*_s, not an alternative to it: dwell answers "how
 * often may this window move", deadband answers "is this correction worth
 * moving for" (integrateWindowPositionSensor.md 3.6).
 *
 * MIN is 1, not 0, deliberately: a zero deadband IS the chattering case, so
 * the floor must not be reachable by an operator dragging a slider down.
 * MAX 200 is ~13 % of the 1500 mm M3 window — coarse, but an operator fighting
 * a noisy installation should be able to get there without a firmware change.
 */
#define CFG_MIN_DEADZONE_MM     1
#define CFG_MAX_DEADZONE_MM   200

/* 2.12.0 — the linear dwell: the least time from the end of one M3 drive to
 * the start of the next, in mode 2 (contract §7, "mode 2 replaces the open
 * dwell with a minimum interval between moves"). 0 is off, and is the
 * SPECIFIED default (plan §10 decision 10) — unlike the deadband, a zero here
 * is a legitimate choice while the law's own hold governs. The maximum matches
 * CFG_MAX_DWELL_OPEN_S: the interval REPLACES that dwell, so it must be able
 * to express the same 25 minutes. */
#define CFG_MIN_MIN_INTV_S      0
#define CFG_MAX_MIN_INTV_S   1500

/* ── System ───────────────────────────────────────────────────────────────── */
/* 2.5.1 (gh#57 part 2) — was 30..300, which contradicted every other statement
 * of this range in the project and was the newest of them:
 *   FR-S03 and FR-CF07 (both "Must")        15..120, default 30
 *   TSDS, five places, citing FR-CF07       15..120, default 30
 *   sensor_poll.cpp SP_POLL_MIN_S/MAX_S     15..120  <- the ACTUAL poller
 *   sensor_poll.cpp file header              15..120
 *   beheerderHandleiding 12.4 advice        15..30 short / 60..120 long
 * T5 clamps to [SP_POLL_MIN_S, SP_POLL_MAX_S] on every loop pass immediately
 * before the vTaskDelay that sets the cadence, and has done so since before
 * this file existed (cfg_limits.h was created whole in v1.16.25, 2026-05-07;
 * SP_POLL_MIN_S = 15 is present in that commit's parent). So a stored 300 was
 * silently polled at 120, and worse: T5 sizes the averaging window from the
 * RAW shadow value, not the clamped one, so a 6-minute window at a stored 300
 * became (6*60)/300 = 1 sample, i.e. no averaging at all. Narrowing here makes
 * the two variables identical over the whole legal range and removes that
 * divergence by construction. Tracked separately for legacy stored values,
 * which this change does NOT retro-clamp: nvs_load_system() reads the key with
 * nvs_cfg_get_i32_or_default() and does not clamp on load. */
#define CFG_MIN_POLL_S       15   /* FR-S03 / FR-CF07; _Static_assert'd == SP_POLL_MIN_S */
#define CFG_MAX_POLL_S      120   /* FR-S03 / FR-CF07; _Static_assert'd == SP_POLL_MAX_S */
#define CFG_MIN_TIMEOUT_MIN   1
#define CFG_MAX_TIMEOUT_MIN 1440  /* 24 h */
#define CFG_MIN_AP_TIMEOUT    0   /* 0 = AP stays up indefinitely */

/* -- Geolocation (2.5.0, gh#57) -------------------------------------------- */
/* update_sun_times() assembles `lat_deg + lat_frac / 1000.0f`, and the result
 * drives s_cfg.is_daytime, which T6 uses to pick day vs night setpoints. An
 * absurd latitude therefore changes greenhouse behaviour, so these are clamped
 * server-side and not only by the GUI's own min/max attributes.
 * NOTE the fraction is unsigned in this encoding: for a negative degree the
 * fraction moves the value TOWARD zero (lat_deg=-52, lat_frac=500 -> -51.5).
 * Correct for the northern hemisphere, which is where the encoding is used;
 * changing it would be a payload change, so it is documented, not altered. */
#define CFG_MIN_LAT_DEG     -90
#define CFG_MAX_LAT_DEG      90
#define CFG_MIN_LON_DEG    -180
#define CFG_MAX_LON_DEG     180
#define CFG_MIN_COORD_FRAC    0   /* thousandths of a degree */
#define CFG_MAX_COORD_FRAC  999

/* -- Status LED: no bounds here since 2.14.0 (gh#67). Its brightness and night
 * window are compile-time constants in watchdog.cpp, not config keys, so there
 * is nothing to clamp. CFG_MIN_HOUR/MAX_HOUR remain: log_upload_h, ota_win_lo
 * and ota_win_hi use them. */

/* ── Status website reporting (T14) ──────────────────────────────────────── */
#define CFG_MIN_STATUS_INTERVAL_S   60   /* spec floor; faster wastes bandwidth */
#define CFG_MAX_STATUS_INTERVAL_S  300   /* spec ceiling; slower drops dashboard freshness */
#define CFG_MIN_HOUR                 0
#define CFG_MAX_HOUR                23
#define CFG_MIN_MINUTE               0
#define CFG_MAX_MINUTE              59
#define CFG_MIN_SECRET_LEN          16   /* below this gives weak shared-secret protection */
#define CFG_MAX_URL_LEN            128
#define CFG_MAX_SECRET_LEN          64

/* ── Internet-pull OTA (ROTA, T16) — rota_tds.md §2.7 R-F01 ───────────────── */
#define CFG_MIN_OTA_CHECK_H          1   /* hourly is the fastest sensible check cadence */
#define CFG_MAX_OTA_CHECK_H        168   /* weekly (7×24) */
/* ota_win_lo/hi use CFG_MIN_HOUR..CFG_MAX_HOUR (0–23); ota_url/ota_secret
 * reuse CFG_MAX_URL_LEN / CFG_MIN_SECRET_LEN / CFG_MAX_SECRET_LEN. */
