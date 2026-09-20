/* SPDX-License-Identifier: MIT */
/**
 * @file cfg_desc.inc
 * @brief The config key descriptor table -- ONE row per key (gh#64).
 *
 * ============================================================================
 *  GENERATED FILE -- do not edit by hand.
 *  Regenerate:  python bin/gen_cfg_desc.py --emit
 *  Verify:      python bin/gen_cfg_desc.py --check   (non-zero on drift)
 * ============================================================================
 *
 * Until 2.8.x the key set was written out six times -- cfg_key_kind(),
 * the apply_config_update() shadow ladder, cfg_clamp(), ns_key_to_log_id(),
 * LIMITS_JSON and the mock server -- and they drifted. Every drift so far
 * reached a release: gh#53 (a whole kind of key rejected), gh#57 part 1
 * (eleven keys accepting any int32), gh#57 part 2 (a clamp wider than the
 * published range), gh#51 group B (a motor key changing unaudited).
 *
 * The fix is not a seventh list. It is that there is only this one, and the
 * consumers read it. Adding a key is one row.
 *
 * Fields that are NOT here, on purpose:
 *   - The bounds themselves. Those stay in cfg_limits.h, referenced by macro,
 *     so a bound is still changed in exactly one place.
 *   - String-valued keys (tz_str, ota_url, status_secret, ...). They never
 *     travel through Q4; their endpoints validate and log them directly.
 *
 * CFG_KEY_NOT_Q4 rows are real keys with real bounds and a real audit id that
 * a DIFFERENT route owns -- the four ota_* belong to POST /api/ota/config.
 * They are listed so their bounds have one home, and cfg_key_kind() maps them
 * to CFG_KEY_UNKNOWN so Q4 still refuses them exactly as before.
 */

/* offsetof and sizeof are taken from the SAME field expression, so the two
 * cannot disagree with each other, nor with the name written beside the key. */
#define CFG_SH(f)   (uint16_t)offsetof(cfg_shadow_t, f), \
                    (uint8_t)sizeof(((cfg_shadow_t *)0)->f)
#define CFG_NOSH    0u, 0u

static const cfg_desc_t CFG_DESC[] = {
    /* ---- NVS_NS_CLIMATE ---- */
    { NVS_NS_CLIMATE, K_AVG_WIN_RH, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_AVG_WIN_RH,
      CFG_MIN_AVG_WIN, CFG_MAX_AVG_WIN, DEF_AVG_WIN_RH, CFG_SH(avg_win_rh) },
    { NVS_NS_CLIMATE, K_AVG_WIN_T, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_AVG_WIN_T,
      CFG_MIN_AVG_WIN, CFG_MAX_AVG_WIN, DEF_AVG_WIN_T, CFG_SH(avg_win_t) },
    { NVS_NS_CLIMATE, K_CR_PRIORITY, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_CR_PRIORITY,
      CFG_MIN_CR_PRIORITY, CFG_MAX_CR_PRIORITY, DEF_CR_PRIORITY, CFG_SH(cr_priority) },
    { NVS_NS_CLIMATE, K_HYST_RH, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_HYST_RH,
      CFG_MIN_HYST_RH, CFG_MAX_HYST_RH, DEF_HYST_RH, CFG_SH(hyst_rh) },
    { NVS_NS_CLIMATE, K_HYST_T, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_HYST_T,
      CFG_MIN_HYST_T, CFG_MAX_HYST_T, DEF_HYST_T, CFG_SH(hyst_t) },
    { NVS_NS_CLIMATE, K_RH_CTRL_EN, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_RH_CTRL_EN,
      0, 1, DEF_RH_CTRL_EN, CFG_SH(rh_ctrl_en) },
    { NVS_NS_CLIMATE, K_RH_MAX_DAY, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_RH_MAX_DAY,
      CFG_MIN_RH_MAX, CFG_MAX_RH_MAX, DEF_RH_MAX_DAY, CFG_SH(rh_max_day) },
    { NVS_NS_CLIMATE, K_RH_MAX_NGT, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_RH_MAX_NGT,
      CFG_MIN_RH_MAX, CFG_MAX_RH_MAX, DEF_RH_MAX_NGT, CFG_SH(rh_max_ngt) },
    { NVS_NS_CLIMATE, K_RH_MIN_DAY, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_RH_MIN_DAY,
      CFG_MIN_RH_MIN, CFG_MAX_RH_MIN, DEF_RH_MIN_DAY, CFG_SH(rh_min_day) },
    { NVS_NS_CLIMATE, K_RH_MIN_NGT, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_RH_MIN_NGT,
      CFG_MIN_RH_MIN, CFG_MAX_RH_MIN, DEF_RH_MIN_NGT, CFG_SH(rh_min_ngt) },
    { NVS_NS_CLIMATE, K_T_MAX_DAY, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_T_MAX_DAY,
      CFG_MIN_T_MAX_DAY, CFG_MAX_T_MAX_DAY, DEF_T_MAX_DAY, CFG_SH(t_max_day) },
    { NVS_NS_CLIMATE, K_T_MAX_NGT, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_T_MAX_NGT,
      CFG_MIN_T_MAX_NGT, CFG_MAX_T_MAX_NGT, DEF_T_MAX_NGT, CFG_SH(t_max_ngt) },
    { NVS_NS_CLIMATE, K_T_MIN_DAY, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_T_MIN_DAY,
      CFG_MIN_T_MIN_DAY, CFG_MAX_T_MIN_DAY, DEF_T_MIN_DAY, CFG_SH(t_min_day) },
    { NVS_NS_CLIMATE, K_T_MIN_NGT, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_T_MIN_NGT,
      CFG_MIN_T_MIN_NGT, CFG_MAX_T_MIN_NGT, DEF_T_MIN_NGT, CFG_SH(t_min_ngt) },

    /* ---- NVS_NS_WIND ---- */
    { NVS_NS_WIND, K_AVG_WIN_WIND, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_AVG_WIN_WIND,
      CFG_MIN_AVG_WIN, CFG_MAX_AVG_WIN, DEF_AVG_WIN_WIND, CFG_SH(avg_win_wind) },
    { NVS_NS_WIND, K_DIR_EXCL_HIGH, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_DIR_EXCL_HI,
      CFG_MIN_DIR, CFG_MAX_DIR, DEF_DIR_EXCL_HIGH, CFG_SH(dir_excl_high) },
    { NVS_NS_WIND, K_DIR_EXCL_LOW, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_DIR_EXCL_LOW,
      CFG_MIN_DIR, CFG_MAX_DIR, DEF_DIR_EXCL_LOW, CFG_SH(dir_excl_low) },
    { NVS_NS_WIND, K_V_MAX, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_V_MAX,
      CFG_MIN_V_MAX, CFG_MAX_V_MAX, DEF_V_MAX, CFG_SH(v_max) },
    { NVS_NS_WIND, K_WIND_HYST, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_WIND_HYST,
      CFG_MIN_WIND_HYST, CFG_MAX_WIND_HYST, DEF_WIND_HYST, CFG_SH(wind_hyst) },
    { NVS_NS_WIND, K_WIND_PROT_EN, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_WIND_PROT_EN,
      0, 1, DEF_WIND_PROT_EN, CFG_SH(wind_prot_en) },

    /* ---- NVS_NS_MOTOR ---- */
    { NVS_NS_MOTOR, K_DWELL_CLOSE_M1, CFG_KEY_SHADOW, 1u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_DWELL_CLOSE,
      CFG_MIN_DWELL_CLOSE_S, CFG_MAX_DWELL_CLOSE_S, DEF_DWELL_CLOSE_M1_S, CFG_SH(dwell_close_s[0]) },
    { NVS_NS_MOTOR, K_DWELL_CLOSE_M2, CFG_KEY_SHADOW, 2u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_DWELL_CLOSE,
      CFG_MIN_DWELL_CLOSE_S, CFG_MAX_DWELL_CLOSE_S, DEF_DWELL_CLOSE_M2_S, CFG_SH(dwell_close_s[1]) },
    { NVS_NS_MOTOR, K_DWELL_CLOSE_M3, CFG_KEY_SHADOW, 3u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_DWELL_CLOSE,
      CFG_MIN_DWELL_CLOSE_S, CFG_MAX_DWELL_CLOSE_S, DEF_DWELL_CLOSE_M3_S, CFG_SH(dwell_close_s[2]) },
    /* Added after the migration, by hand -- the generator derives the 51 rows
     * that replaced the six tables and knows nothing of keys added since.
     * THIS ROW IS THE WHOLE ENROLMENT: clamp, shadow write, boot default,
     * audit id, kind, route and the published limits all read it. */
    { NVS_NS_MOTOR, K_DEADZONE_M3, CFG_KEY_SHADOW, 3u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_DEADZONE_M3,
      CFG_MIN_DEADZONE_MM, CFG_MAX_DEADZONE_MM, DEF_DEADZONE_M3_MM, CFG_SH(deadzone_m3_mm) },
    /* gh#73 (2.9.0), also by hand: the operator's statement that M3 has a
     * position sensor. A boolean, so literal bounds like rh_ctrl_en. */
    { NVS_NS_MOTOR, K_WPOS_FITTED_M3, CFG_KEY_SHADOW, 3u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_WPOS_FITTED_M3,
      0, 1, DEF_WPOS_FITTED_M3, CFG_SH(wpos_fitted_m3) },
    /* 2.12.0 (plan §5b), by hand: the DESIRED control mode for M3. A boolean
     * in range, like wpos_fitted_m3 above. What the controller acts on is the
     * EFFECTIVE mode (dm_m3_ctrl_mode()), which also needs a trusted position;
     * this key is only the operator's half of that. */
    { NVS_NS_MOTOR, K_CTRL_MODE_M3, CFG_KEY_SHADOW, 3u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_CTRL_MODE_M3,
      0, 1, DEF_CTRL_MODE_M3, CFG_SH(ctrl_mode_m3) },
    { NVS_NS_MOTOR, K_DWELL_OPEN_M1, CFG_KEY_SHADOW, 1u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_DWELL_OPEN,
      CFG_MIN_DWELL_OPEN_S, CFG_MAX_DWELL_OPEN_S, DEF_DWELL_OPEN_M1_S, CFG_SH(dwell_open_s[0]) },
    { NVS_NS_MOTOR, K_DWELL_OPEN_M2, CFG_KEY_SHADOW, 2u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_DWELL_OPEN,
      CFG_MIN_DWELL_OPEN_S, CFG_MAX_DWELL_OPEN_S, DEF_DWELL_OPEN_M2_S, CFG_SH(dwell_open_s[1]) },
    { NVS_NS_MOTOR, K_DWELL_OPEN_M3, CFG_KEY_SHADOW, 3u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_DWELL_OPEN,
      CFG_MIN_DWELL_OPEN_S, CFG_MAX_DWELL_OPEN_S, DEF_DWELL_OPEN_M3_S, CFG_SH(dwell_open_s[2]) },
    { NVS_NS_MOTOR, K_TRAVEL_M1, CFG_KEY_SHADOW, 1u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_TRAVEL,
      CFG_MIN_TRAVEL_S, CFG_MAX_TRAVEL_S, MOTOR_M1_TRAVEL_S_DEFAULT, CFG_SH(travel_s[0]) },
    { NVS_NS_MOTOR, K_TRAVEL_M2, CFG_KEY_SHADOW, 2u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_TRAVEL,
      CFG_MIN_TRAVEL_S, CFG_MAX_TRAVEL_S, MOTOR_M2_TRAVEL_S_DEFAULT, CFG_SH(travel_s[1]) },
    { NVS_NS_MOTOR, K_TRAVEL_M3, CFG_KEY_SHADOW, 3u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_TRAVEL,
      CFG_MIN_TRAVEL_S, CFG_MAX_TRAVEL_S, MOTOR_M3_TRAVEL_S_DEFAULT, CFG_SH(travel_s[2]) },

    /* ---- NVS_NS_WIFI ---- */
    { NVS_NS_WIFI, "ap_enable", CFG_KEY_NVS_ONLY, 0u, CFG_P_Q4, LOG_PARAM_NONE,
      0, 1, 0, CFG_NOSH },

    /* ---- NVS_NS_SYSTEM ---- */
    { NVS_NS_SYSTEM, K_AP_TIMEOUT, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_NONE,
      CFG_MIN_AP_TIMEOUT, CFG_MAX_TIMEOUT_MIN, DEF_AP_TIMEOUT_MIN, CFG_SH(ap_timeout_min) },
    { NVS_NS_SYSTEM, K_LAT_DEG, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_F_SUN | CFG_P_Q4, LOG_PARAM_LAT_LON,
      CFG_MIN_LAT_DEG, CFG_MAX_LAT_DEG, DEF_LAT_DEG, CFG_SH(lat_deg) },
    { NVS_NS_SYSTEM, K_LAT_FRAC, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_F_SUN | CFG_P_Q4, LOG_PARAM_LAT_LON,
      CFG_MIN_COORD_FRAC, CFG_MAX_COORD_FRAC, DEF_LAT_FRAC, CFG_SH(lat_frac) },
    { NVS_NS_SYSTEM, K_LED_DAY_BRT, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_NONE,
      CFG_MIN_LED_BRT, CFG_MAX_LED_BRT, DEF_LED_DAY_BRT, CFG_SH(led_day_brt) },
    { NVS_NS_SYSTEM, K_LED_NITE_BRT, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_NONE,
      CFG_MIN_LED_BRT, CFG_MAX_LED_BRT, DEF_LED_NITE_BRT, CFG_SH(led_nite_brt) },
    { NVS_NS_SYSTEM, K_LED_NITE_FROM, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_NONE,
      CFG_MIN_HOUR, CFG_MAX_HOUR, DEF_LED_NITE_FROM, CFG_SH(led_nite_from) },
    { NVS_NS_SYSTEM, K_LED_NITE_TO, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_NONE,
      CFG_MIN_HOUR, CFG_MAX_HOUR, DEF_LED_NITE_TO, CFG_SH(led_nite_to) },
    { NVS_NS_SYSTEM, K_LOG_UPLOAD_H, CFG_KEY_SHADOW, 0u, CFG_P_Q4 | CFG_P_WEB, LOG_PARAM_LOG_UPLOAD_H,
      CFG_MIN_HOUR, CFG_MAX_HOUR, DEF_LOG_UPLOAD_H, CFG_SH(log_upload_h) },
    { NVS_NS_SYSTEM, K_LOG_UPLOAD_M, CFG_KEY_SHADOW, 0u, CFG_P_Q4 | CFG_P_WEB, LOG_PARAM_LOG_UPLOAD_M,
      CFG_MIN_MINUTE, CFG_MAX_MINUTE, DEF_LOG_UPLOAD_M, CFG_SH(log_upload_m) },
    { NVS_NS_SYSTEM, K_LOG_UPLOAD_ROT, CFG_KEY_SHADOW, 0u, CFG_P_Q4 | CFG_P_WEB, LOG_PARAM_LOG_UPLOAD_ROT,
      0, 1, DEF_LOG_UPLOAD_ROT, CFG_SH(log_upload_rot) },
    { NVS_NS_SYSTEM, K_LON_DEG, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_F_SUN | CFG_P_Q4, LOG_PARAM_LAT_LON,
      CFG_MIN_LON_DEG, CFG_MAX_LON_DEG, DEF_LON_DEG, CFG_SH(lon_deg) },
    { NVS_NS_SYSTEM, K_LON_FRAC, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_F_SUN | CFG_P_Q4, LOG_PARAM_LAT_LON,
      CFG_MIN_COORD_FRAC, CFG_MAX_COORD_FRAC, DEF_LON_FRAC, CFG_SH(lon_frac) },
    { NVS_NS_SYSTEM, K_OTA_CHECK_H, CFG_KEY_SHADOW, 0u, CFG_P_OTA, LOG_PARAM_OTA_CHECK_H,
      CFG_MIN_OTA_CHECK_H, CFG_MAX_OTA_CHECK_H, DEF_OTA_CHECK_H, CFG_SH(ota_check_h) },
    { NVS_NS_SYSTEM, K_OTA_ENABLE, CFG_KEY_SHADOW, 0u, CFG_P_OTA, LOG_PARAM_OTA_ENABLE,
      0, 1, DEF_OTA_ENABLE, CFG_SH(ota_enable) },
    { NVS_NS_SYSTEM, K_OTA_WIN_HI, CFG_KEY_SHADOW, 0u, CFG_P_OTA, LOG_PARAM_OTA_WIN_HI,
      CFG_MIN_HOUR, CFG_MAX_HOUR, DEF_OTA_WIN_HI, CFG_SH(ota_win_hi) },
    { NVS_NS_SYSTEM, K_OTA_WIN_LO, CFG_KEY_SHADOW, 0u, CFG_P_OTA, LOG_PARAM_OTA_WIN_LO,
      CFG_MIN_HOUR, CFG_MAX_HOUR, DEF_OTA_WIN_LO, CFG_SH(ota_win_lo) },
    { NVS_NS_SYSTEM, K_POLL_INTERVAL, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_POLL_INTV,
      CFG_MIN_POLL_S, CFG_MAX_POLL_S, DEF_POLL_INTERVAL_S, CFG_SH(poll_interval_s) },
    { NVS_NS_SYSTEM, K_SESSION_TIMEOUT, CFG_KEY_SHADOW, 0u, CFG_F_PUB | CFG_P_Q4, LOG_PARAM_NONE,
      CFG_MIN_TIMEOUT_MIN, CFG_MAX_TIMEOUT_MIN, DEF_SESSION_TIMEOUT_MIN, CFG_SH(session_timeout_min) },
    { NVS_NS_SYSTEM, K_STATUS_ENABLE, CFG_KEY_SHADOW, 0u, CFG_P_Q4 | CFG_P_WEB, LOG_PARAM_STATUS_ENABLE,
      0, 1, DEF_STATUS_ENABLE, CFG_SH(status_enable) },
    { NVS_NS_SYSTEM, K_STATUS_EXPOSE, CFG_KEY_SHADOW, 0u, CFG_P_Q4 | CFG_P_WEB, LOG_PARAM_STATUS_EXPOSE,
      0, 63, DEF_STATUS_EXPOSE, CFG_SH(status_expose) },
    { NVS_NS_SYSTEM, K_STATUS_INTERVAL, CFG_KEY_SHADOW, 0u, CFG_P_Q4 | CFG_P_WEB, LOG_PARAM_STATUS_INTV,
      CFG_MIN_STATUS_INTERVAL_S, CFG_MAX_STATUS_INTERVAL_S, DEF_STATUS_INTERVAL_S, CFG_SH(status_interval_s) },

};

#undef CFG_SH
#undef CFG_NOSH
