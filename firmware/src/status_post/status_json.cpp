/**
 * @file status_json.cpp
 * @brief Canonical controller-status JSON builder.
 *
 * Implementation notes:
 *  - Output is hand-rolled snprintf rather than ArduinoJson; the schema is
 *    fixed and small so a JSON library would be pure overhead.
 *  - All payload-derived strings are produced from snapshot fields (no caller
 *    pass-through), so XSS-style payload injection is structurally impossible
 *    on the consumer side.
 *  - Numeric formatting matches the local UI's existing expectations: temp
 *    and wind are decimals with one fractional digit, RH/dir are integers.
 *
 * @see status_json.h (public API and parameter semantics)
 *
 * @author Greenhouse Controller project
 */

#include "status_json.h"
#include "status_post.h"   /* status_post_backoff_active() — gh#18 Phase 1 */
#include "../system_id/system_id.h"  /* unit_id (gh#17, since 1.18.3) */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *window_state_str(window_state_t s)
{
    switch (s) {
        case WIN_OPEN:         return "OPEN";
        case WIN_CLOSED:       return "CLOSED";
        case WIN_MOVING_OPEN:  return "MOVING_OPEN";
        case WIN_MOVING_CLOSE: return "MOVING_CLOSE";
        case WIN_UNKNOWN:      /* fall-through */
        default:               return "UNKNOWN";
    }
}

const char *op_mode_str(op_mode_t m)
{
    switch (m) {
        case MODE_STANDBY:        return "STANDBY";
        case MODE_WIND_OVERRIDE:  return "WIND_OVERRIDE";
        case MODE_MOTOR_ALARM:    return "MOTOR_ALARM";
        case MODE_AUTOMATIC:      /* fall-through */
        default:                  return "AUTOMATIC";
    }
}

/**
 * @brief Bounds-checked printf-append into a growing buffer.
 *
 * On overflow the buffer is NUL-terminated at the last writable byte and
 * the caller's running success flag is cleared via the false return; the
 * builder then short-circuits all subsequent `append()` calls.
 *
 * @param buf  Output buffer.
 * @param cap  Total capacity of @p buf.
 * @param pos  In/out write cursor (bytes already written excl. NUL).
 * @param fmt  printf-style format string.
 * @return true if the formatted text fit; false on overflow or vsnprintf error.
 */
static bool append(char *buf, size_t cap, size_t *pos, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static bool append(char *buf, size_t cap, size_t *pos, const char *fmt, ...)
{
    if (*pos >= cap) { return false; }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *pos, cap - *pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *pos) {
        /* Truncated; ensure NUL and signal overflow. */
        buf[cap - 1] = '\0';
        return false;
    }
    *pos += (size_t)n;
    return true;
}

/* ============================================================
 * EG1-bit → dashboard flag-name table.
 *
 * The public dashboard renders one badge per active flag in the mode tile.
 * The mapping is fixed by the dashboard's app.js (FLAG_CLASS table); we
 * emit only the bits that have a corresponding flag name. EG1_BIT_CALIBRATING
 * also drives the WINDOW_CAL "current" mode string, but a separate badge is
 * still useful to indicate the cause.
 * ============================================================ */
/** @brief Static row in the EG1-bit → dashboard-flag-name lookup table. */
typedef struct {
    uint32_t    bit;   /**< Mask in EG1 (single bit, EG1_BIT_*). */
    const char *name;  /**< Public flag string for the JSON `mode.flags[]` array. */
} eg1_flag_t;

static const eg1_flag_t EG1_FLAGS[] = {
    { EG1_BIT_WIND_OVERRIDE,    "wind_override"     },
    { EG1_BIT_SENSOR_FAULT_T,   "sensor_fault_temp" },
    { EG1_BIT_SENSOR_FAULT_W,   "sensor_fault_wind" },
    { EG1_BIT_OTA_IN_PROGRESS,  "ota_in_progress"   },
    { EG1_BIT_MOTOR_ALARM,      "motor_alarm"       },
    { EG1_BIT_CALIBRATING,      "calibrating"       },
};

/**
 * @brief Highest-priority active mode label for the JSON `mode.current` field.
 *
 * Priority order: MOTOR_ALARM > WIND_OVERRIDE > WINDOW_CAL (calibrating) >
 * `op_mode_str(s->mode)`. Matches the dashboard's MODE_CLASS lookup keys.
 *
 * @param s  Snapshot — read-only.
 * @return Static string literal — never NULL.
 */
static const char *current_mode_label(const status_snapshot_t *s)
{
    if (s->eg1_bits & EG1_BIT_MOTOR_ALARM)   return "MOTOR_ALARM";
    if (s->eg1_bits & EG1_BIT_WIND_OVERRIDE) return "WIND_OVERRIDE";
    if (s->eg1_bits & EG1_BIT_CALIBRATING)   return "WINDOW_CAL";
    return op_mode_str(s->mode);
}

size_t build_canonical_status_json(char *buf, size_t cap,
                                   const status_snapshot_t *s,
                                   uint32_t expose_mask,
                                   bool include_disabled_setpoints)
{
    if (buf == NULL || cap == 0u || s == NULL) { return 0u; }

    size_t pos = 0u;
    bool   ok  = true;

    /* Top-level open + the mandatory "type" tag for the local WS dispatcher. */
    ok = ok && append(buf, cap, &pos, "{\"type\":\"status\"");

    /* climate — dashboard reads temp_c, rh_pct (see hbwv assets/app.js). The
     * temp_avg_c / rh_avg_pct and the *_active setpoint fields are extras
     * that the public dashboard ignores; the local web GUI surfaces them
     * on the Temperature and Humidity status tiles so the operator sees
     * the live measurement together with the threshold currently in force.
     *
     * RH-setpoint gating: the two `rh_*_active` fields are conditional —
     * emitted when RH control is active OR when the caller explicitly
     * asks for them (local UI passes include_disabled_setpoints=true so it
     * can render the values in a dimmed style; T14 passes false so the
     * public dashboard receives no inert setpoints when the operator has
     * disabled RH control). `rh_ctrl_enabled` is always emitted so the
     * recipient knows which mode is active. */
    if (ok && (expose_mask & STATUS_EXPOSE_CLIMATE)) {
        ok = ok && append(buf, cap, &pos,
            ",\"climate\":{\"temp_c\":%d.%d,\"temp_avg_c\":%d.%d,"
            "\"rh_pct\":%u,\"rh_avg_pct\":%u,"
            "\"temp_max_active\":%d",
            s->t_c10 / 10, (s->t_c10 < 0 ? -s->t_c10 : s->t_c10) % 10,
            s->t_avg_c10 / 10, (s->t_avg_c10 < 0 ? -s->t_avg_c10 : s->t_avg_c10) % 10,
            (unsigned)s->rh_pct, (unsigned)s->rh_avg_pct,
            (int)s->t_max_active);

        if (ok && (s->rh_ctrl_enabled || include_disabled_setpoints)) {
            ok = ok && append(buf, cap, &pos,
                ",\"rh_max_active\":%u,\"rh_min_active\":%u",
                (unsigned)s->rh_max_active, (unsigned)s->rh_min_active);
        }
        ok = ok && append(buf, cap, &pos,
            ",\"rh_ctrl_enabled\":%s}",
            s->rh_ctrl_enabled ? "true" : "false");
    }

    /* wind — dashboard reads speed_ms, direction_deg. avg_* and
     * direction_variation_deg are local extras; the variation captures the
     * angular sector that the wind direction is currently oscillating
     * within (a tight sector means steady wind, a wide sector means
     * shifting wind). */
    if (ok && (expose_mask & STATUS_EXPOSE_WIND)) {
        ok = ok && append(buf, cap, &pos,
            ",\"wind\":{\"speed_ms\":%u.%u,\"speed_avg_ms\":%u.%u,"
            "\"direction_deg\":%u,\"direction_avg_deg\":%u,"
            "\"direction_variation_deg\":%u}",
            (unsigned)(s->w_ms10 / 10u),     (unsigned)(s->w_ms10 % 10u),
            (unsigned)(s->w_avg_ms10 / 10u), (unsigned)(s->w_avg_ms10 % 10u),
            (unsigned)s->w_dir_deg,
            (unsigned)s->w_avg_dir_deg,
            (unsigned)s->w_dir_variation_deg);
    }

    /* windows — object keyed M1/M2/M3. */
    if (ok && (expose_mask & STATUS_EXPOSE_WINDOWS)) {
        ok = ok && append(buf, cap, &pos,
            ",\"windows\":{\"M1\":\"%s\",\"M2\":\"%s\",\"M3\":\"%s\"}",
            window_state_str(s->win[0]),
            window_state_str(s->win[1]),
            window_state_str(s->win[2]));
    }

    /* mode — object {current, flags[]}. The dashboard renders `current` as
     * a coloured pill and each entry in `flags` as a badge in the mode tile. */
    if (ok && (expose_mask & STATUS_EXPOSE_MODE)) {
        ok = ok && append(buf, cap, &pos,
            ",\"mode\":{\"current\":\"%s\",\"flags\":[",
            current_mode_label(s));
        bool first = true;
        for (size_t i = 0; ok && i < (sizeof(EG1_FLAGS) / sizeof(EG1_FLAGS[0])); i++) {
            if (s->eg1_bits & EG1_FLAGS[i].bit) {
                ok = ok && append(buf, cap, &pos,
                    "%s\"%s\"", first ? "" : ",", EG1_FLAGS[i].name);
                first = false;
            }
        }
        /* T14 circuit-breaker state (gh#18 Phase 1). Not an EG1 bit: the
         * breaker is private to T14 and would not benefit from cross-task
         * event-group machinery. Polled here directly. Phase 1 returns
         * false unconditionally; Phase 2 wires it to real breaker state. */
        if (ok && status_post_backoff_active()) {
            ok = ok && append(buf, cap, &pos,
                "%s\"net_backoff_active\"", first ? "" : ",");
            first = false;
        }

        /* a.6.35.4 — operator-disabled-feature flags. Surfaced as mode.flags
         * entries so both the local GUI's Alarms card and the public status
         * dashboard pick them up via the same flag-name → badge mapping.
         *
         * Not EG1 bits: these are cfg-shadow boolean states, not transient
         * runtime events. cfg.v_max ≤ 0 disables wind-protection (operator
         * decision); cfg.rh_ctrl_en == 0 disables humidity-driven window
         * control. Both states are persistent across reboots — they reflect
         * configuration, not alarm conditions, so a separate flag namespace
         * is appropriate. */
        if (ok && !s->wind_protect_enabled) {
            ok = ok && append(buf, cap, &pos,
                "%s\"wind_protect_off\"", first ? "" : ",");
            first = false;
        }
        if (ok && !s->rh_ctrl_enabled) {
            ok = ok && append(buf, cap, &pos,
                "%s\"humidity_ctrl_off\"", first ? "" : ",");
            first = false;
        }

        /* a.6.35.6 — coredump-available indicator. Set when T4's boot-time
         * esp_core_dump_image_check() found a valid dump in flash from a
         * previous panic. Surfaced to both the local GUI (blue "Coredump
         * available" badge in the Alarms card + Diagnostics panel in the
         * Log tab with Download/Erase buttons) AND the public status
         * dashboard via this flag string. Cleared after the operator
         * downloads and erases the partition via /api/coredump. */
        if (ok && s->coredump_available) {
            ok = ok && append(buf, cap, &pos,
                "%s\"coredump_available\"", first ? "" : ",");
            first = false;
        }
        ok = ok && append(buf, cap, &pos, "]}");
    }

    /* sun — dashboard reads sunrise_min / sunset_min as minutes-from-midnight
     * and renders them verbatim as HH:MM, so the firmware sends LOCAL minutes
     * (DST-adjusted in dm_status_snapshot). */
    if (ok && (expose_mask & STATUS_EXPOSE_SUN)) {
        ok = ok && append(buf, cap, &pos,
            ",\"sun\":{\"is_daytime\":%s,"
            "\"sunrise_min\":%ld,\"sunset_min\":%ld}",
            s->is_daytime ? "true" : "false",
            (long)s->sunrise_mins_local, (long)s->sunset_mins_local);
    }

    /* system — dashboard reads wifi_ip, wifi_rssi_dbm, ntp_synced, fw_ver.
     * ts_unix / time_iso / eg1 / uptime_s / asset_version are local-UI
     * extras. asset_version comes from /manifest.json on the active LFS;
     * compared with fw_ver in the local UI to detect a stale-LFS-after-OTA
     * mismatch.
     *
     * `unit_id` (since 1.18.3, gh#17) is the 4-hex-char short ID derived
     * from the last 2 MAC bytes — matches the AP-SSID `Greenhouse-XXXX`
     * convention so operators identify units consistently across the SSID,
     * the LCD/log boot row, and the dashboard. */
    if (ok && (expose_mask & STATUS_EXPOSE_SYSTEM)) {
        char unit_id_str[8] = {0};
        system_unit_id_str(unit_id_str, sizeof(unit_id_str));
        ok = ok && append(buf, cap, &pos,
            ",\"system\":{\"unit_id\":\"%s\","
            "\"wifi_ip\":\"%s\",\"wifi_rssi_dbm\":%d,"
            "\"ntp_synced\":%s,\"fw_ver\":\"%s\","
            "\"asset_version\":\"%s\","
            "\"uptime_s\":%lu,\"ts_unix\":%lu,"
            "\"time_iso\":\"%s\",\"eg1\":%lu}",
            unit_id_str,
            s->ip, (int)s->rssi,
            s->ntp_synced ? "true" : "false",
            s->fw, s->assets,
            (unsigned long)s->uptime_s,
            (unsigned long)s->ts_unix, s->time_iso,
            (unsigned long)s->eg1_bits);
    }

    /* update_interval_s — always emitted. */
    ok = ok && append(buf, cap, &pos,
        ",\"update_interval_s\":%u}",
        (unsigned)s->update_interval_s);

    if (!ok) {
        if (cap > 0u) { buf[0] = '\0'; }
        return 0u;
    }
    return pos;
}
