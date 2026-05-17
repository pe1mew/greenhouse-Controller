/**
 * @file cfg_defaults.h
 * @brief Single source of truth for all NVS factory-default values.
 *
 * Companion to cfg_limits.h (which holds the min/max validation bounds).
 * Consumed by every layer that needs to know "what value should be written
 * to NVS the first time we boot, or read back if a key is missing":
 *
 *   1. data_manager.cpp / nvs_load_*()        — writes these to NVS on first
 *                                                boot via nvs_cfg_get_*_or_default().
 *   2. relay_controller.cpp / t2_init()       — same pattern; reads motor
 *                                                travel + dwell defaults if
 *                                                T2 boots before T4 has
 *                                                seeded NVS.
 *   3. ui_display.cpp / session-timeout calc  — fallback when the cfg shadow
 *                                                snapshot is unavailable.
 *
 * Anti-oscillation rationale (see simulation/simulationOptimisation.md and
 * the kas-2 calibration in simulation/new_settings_calibrated.json):
 *   DEF_HYST_T              = 5     wider band reduces window oscillation
 *   DEF_HYST_RH             = 12    wider RH dead band suppresses small-signal toggles
 *   DEF_AVG_WIN_T           = 6     smooths short thermal spikes
 *   DEF_AVG_WIN_RH          = 10    matched to the doubled-buffer 30 s poll
 *   DEF_POLL_INTERVAL_S     = 30    finer sampling, twice the smoothing depth in same time window
 *   DEF_DWELL_OPEN_M1/M2_S  = 300   5 min hold for the short-travel roof vents
 *   DEF_DWELL_OPEN_M3_S     = 1500  25 min hold for M3 specifically (171 s travel makes it the
 *                                   dominant slow-oscillation driver; long hold breaks the cycle)
 *   DEF_DWELL_CLOSE_M3_S    = 600   10 min closed-state hold on M3 — symmetric counterpart;
 *                                   prevents reopen-after-close micro-cycles
 *
 * NB: existing devices keep their NVS-stored values across firmware upgrades;
 * default changes only apply on a fresh flash or after factory-reset.
 */

#pragma once

/* ── Climate — temperature setpoints (°C) ───────────────────────────────── */
#define DEF_T_MIN_DAY      16   /**< Day heating setpoint (informational; future heating) */
#define DEF_T_MAX_DAY      28   /**< Day ventilation threshold: open above 28 °C */
#define DEF_T_MIN_NGT      14   /**< Night heating setpoint (informational; future heating) */
#define DEF_T_MAX_NGT      20   /**< Night ventilation threshold: open above 20 °C */

/* ── Climate — humidity setpoints (%) ───────────────────────────────────── */
#define DEF_RH_MIN_DAY     50   /**< Day RH floor: close windows below 50 % (avoid crop desiccation) */
#define DEF_RH_MAX_DAY     75   /**< Day RH ceiling: open above 75 % (disease pressure threshold) */
#define DEF_RH_MIN_NGT     55   /**< Night RH floor: close windows below 55 % */
#define DEF_RH_MAX_NGT     80   /**< Night RH ceiling: open above 80 % (condensation prevention) */

/* ── Climate — control flags and tuning ─────────────────────────────────── */
#define DEF_HYST_T          5   /**< T hysteresis: 5 °C dead band — wider band reduces window oscillation */
#define DEF_HYST_RH        12   /**< RH hysteresis: 12 % dead band — suppresses small-signal step toggles on humid days */
#define DEF_RH_CTRL_EN      1   /**< RH control enabled by default */
#define DEF_CR_PRIORITY     0   /**< Conflict resolution: 0 = CR_TEMP_FIRST (preserves T-floor protection on cool humid nights) */
#define DEF_AVG_WIN_T       6   /**< 6-min T averaging window: ~12 samples @ 30 s poll — smooths short thermal spikes */
#define DEF_AVG_WIN_RH     10   /**< 10-min RH averaging window: ~20 samples @ 30 s poll — extra smoothing for the M3 RH-driven scenarios */

/* ── Wind ───────────────────────────────────────────────────────────────── */
#define DEF_V_MAX           6   /**< Wind speed threshold (m/s) — Beaufort 4 onset + margin */
#define DEF_DIR_EXCL_LOW    0   /**< No exclusion zone by default */
#define DEF_DIR_EXCL_HIGH   0
#define DEF_WIND_PROT_EN    1   /**< Wind override enabled by default */

/* ── Motor — full-travel times (seconds) ────────────────────────────────── */
/* Written to NVS on first boot; adjustable via web GUI (FR-CF05, admin).
 * T2 reads the runtime values from T4 (MX4); these macros are only used as
 * the fallback default when nvs_cfg_get_i32_or_default() finds no NVS key. */
#define MOTOR_M1_TRAVEL_S_DEFAULT       21   /**< M1 factory default full-travel: 21 s */
#define MOTOR_M2_TRAVEL_S_DEFAULT       21   /**< M2 factory default full-travel: 21 s */
#define MOTOR_M3_TRAVEL_S_DEFAULT      171   /**< M3 (ridge vent) factory default: 171 s */
#define MOTOR_TRAVEL_MARGIN_S_DEFAULT    5   /**< Fixed safety margin added to every relay pulse (s) */

/* ── Motor — dwell (seconds) ────────────────────────────────────────────── */
/* Per-motor: M3 (171 s travel ridge vent) needs a much longer post-open hold
 * to break the slow oscillation observed on humid days; M1 and M2 (21 s
 * travel roof vents) can use the standard 5 min hold. dwell_close on M3
 * adds a symmetric closed-state hold for the same anti-oscillation reason.
 * Unit: seconds — T2 reads the NVS value and multiplies by 1000 for ms. */
#define DEF_DWELL_OPEN_M1_S    300   /**< M1: 5 min post-open hold */
#define DEF_DWELL_OPEN_M2_S    300   /**< M2: 5 min post-open hold */
#define DEF_DWELL_OPEN_M3_S   1500   /**< M3: 25 min post-open hold (kas-2 calibrated, breaks slow RH oscillation) */
#define DEF_DWELL_CLOSE_M1_S     0   /**< M1: no mandatory closed-state hold */
#define DEF_DWELL_CLOSE_M2_S     0   /**< M2: no mandatory closed-state hold */
#define DEF_DWELL_CLOSE_M3_S   600   /**< M3: 10 min closed-state hold — symmetric anti-oscillation counterpart to the open hold */

/* ── System ─────────────────────────────────────────────────────────────── */
#define DEF_POLL_INTERVAL_S      30   /**< 30 s poll: doubles smoothing-buffer depth at same time-window without the firmware-revisit overhead of finer rates */
#define DEF_SESSION_TIMEOUT_MIN   5   /**< Idle session expiry (minutes) */
#define DEF_AP_TIMEOUT_MIN       30   /**< Soft-AP auto-stop (minutes); 0 = stay up */

/* ── Site location (Netherlands) ────────────────────────────────────────── */
/* Used by sunrise/sunset calculation and is_daytime selection.  Updated at
 * runtime by the geolocation sync (network_manager.cpp). */
#define DEF_LAT_DEG              52   /**< Netherlands default latitude */
#define DEF_LAT_FRAC              0
#define DEF_LON_DEG               5   /**< Netherlands default longitude */
#define DEF_LON_FRAC              0

/* ── LED brightness and night schedule ──────────────────────────────────── */
#define DEF_LED_DAY_BRT         200   /**< Daytime brightness (0..255) */
#define DEF_LED_NITE_BRT         20   /**< Night-time brightness (0..255) */
#define DEF_LED_NITE_FROM        22   /**< Night-mode start (local hour, 0..23) */
#define DEF_LED_NITE_TO           6   /**< Night-mode end   (local hour, 0..23) */

/* ── Timezone (POSIX TZ string) ─────────────────────────────────────────── */
/* Overwritten at runtime by geolocation sync or web/Q4 update. */
#define DEF_TZ_STR    "CET-1CEST,M3.5.0,M10.5.0/3"

/* ── Status website reporting (T14) ─────────────────────────────────────── */
/* All keys live in NVS_NS_SYSTEM. Empty URL or status_enable=0 disables the
 * feature. Defaults: feature off, expose mask = ALL six tiles, daily log
 * upload at 03:15 local, also upload on rotation. */
#define DEF_STATUS_URL          ""              /**< Endpoint URL (http:// or https://) */
#define DEF_STATUS_SECRET       ""              /**< Shared secret sent in sourceidentifier header */
#define DEF_STATUS_INTERVAL_S   240             /**< POST cycle (s); spec range 60–300. Default raised 120→240 in 1.20.3 (gh#23) to slow the mbedTLS-handshake heap-drop accumulation rate; with 240 s the supervisor's planned-reboot cadence extends from ~5.5 h to ~11 h. Operator can override via Web tab → Interval (s). */
#define DEF_STATUS_ENABLE       0               /**< 0 = disabled by default */
#define DEF_STATUS_EXPOSE       0x3F            /**< Bits 0..5 = climate/wind/windows/mode/sun/system */
#define DEF_LOG_UPLOAD_H        3               /**< Daily upload local hour (0–23) */
#define DEF_LOG_UPLOAD_M        15              /**< Daily upload local minute (0–59) */
#define DEF_LOG_UPLOAD_ROT      1               /**< Also upload on T9 rotation */
#define DEF_LOG_LAST_UP         ""              /**< Last uploaded filename (T14 owns) */
