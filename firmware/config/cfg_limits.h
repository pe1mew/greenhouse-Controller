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
 *   CFG_MIN_POLL_S  = 30  faster polling gives no benefit for greenhouse dynamics
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

/* ── System ───────────────────────────────────────────────────────────────── */
#define CFG_MIN_POLL_S       30   /* below 30 s provides no benefit for greenhouse dynamics */
#define CFG_MAX_POLL_S      300   /* above 5 min climate response becomes too slow */
#define CFG_MIN_TIMEOUT_MIN   1
#define CFG_MAX_TIMEOUT_MIN 1440  /* 24 h */
#define CFG_MIN_AP_TIMEOUT    0   /* 0 = AP stays up indefinitely */

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
