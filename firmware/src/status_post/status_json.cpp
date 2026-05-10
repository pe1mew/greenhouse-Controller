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
 */

#include "status_json.h"

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

/* Append-with-bounds helper. Returns false on overflow. */
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
typedef struct { uint32_t bit; const char *name; } eg1_flag_t;
static const eg1_flag_t EG1_FLAGS[] = {
    { EG1_BIT_WIND_OVERRIDE,    "wind_override"     },
    { EG1_BIT_SENSOR_FAULT_T,   "sensor_fault_temp" },
    { EG1_BIT_SENSOR_FAULT_W,   "sensor_fault_wind" },
    { EG1_BIT_OTA_IN_PROGRESS,  "ota_in_progress"   },
    { EG1_BIT_MOTOR_ALARM,      "motor_alarm"       },
    { EG1_BIT_CALIBRATING,      "calibrating"       },
};

/* Highest-priority active mode label. Matches the dashboard's MODE_CLASS
 * lookup keys (AUTOMATIC / WIND_OVERRIDE / WINDOW_CAL / MOTOR_ALARM). */
static const char *current_mode_label(const status_snapshot_t *s)
{
    if (s->eg1_bits & EG1_BIT_MOTOR_ALARM)   return "MOTOR_ALARM";
    if (s->eg1_bits & EG1_BIT_WIND_OVERRIDE) return "WIND_OVERRIDE";
    if (s->eg1_bits & EG1_BIT_CALIBRATING)   return "WINDOW_CAL";
    return op_mode_str(s->mode);
}

size_t build_canonical_status_json(char *buf, size_t cap,
                                   const status_snapshot_t *s,
                                   uint32_t expose_mask)
{
    if (buf == NULL || cap == 0u || s == NULL) { return 0u; }

    size_t pos = 0u;
    bool   ok  = true;

    /* Top-level open + the mandatory "type" tag for the local WS dispatcher. */
    ok = ok && append(buf, cap, &pos, "{\"type\":\"status\"");

    /* climate — dashboard reads temp_c, rh_pct (see hbwv assets/app.js). The
     * temp_avg_c / rh_avg_pct fields are local-UI extras the dashboard ignores. */
    if (ok && (expose_mask & STATUS_EXPOSE_CLIMATE)) {
        ok = ok && append(buf, cap, &pos,
            ",\"climate\":{\"temp_c\":%d.%d,\"temp_avg_c\":%d.%d,"
            "\"rh_pct\":%u,\"rh_avg_pct\":%u}",
            s->t_c10 / 10, (s->t_c10 < 0 ? -s->t_c10 : s->t_c10) % 10,
            s->t_avg_c10 / 10, (s->t_avg_c10 < 0 ? -s->t_avg_c10 : s->t_avg_c10) % 10,
            (unsigned)s->rh_pct, (unsigned)s->rh_avg_pct);
    }

    /* wind — dashboard reads speed_ms, direction_deg. avg_* are local extras. */
    if (ok && (expose_mask & STATUS_EXPOSE_WIND)) {
        ok = ok && append(buf, cap, &pos,
            ",\"wind\":{\"speed_ms\":%u.%u,\"speed_avg_ms\":%u.%u,"
            "\"direction_deg\":%u,\"direction_avg_deg\":%u}",
            (unsigned)(s->w_ms10 / 10u),     (unsigned)(s->w_ms10 % 10u),
            (unsigned)(s->w_avg_ms10 / 10u), (unsigned)(s->w_avg_ms10 % 10u),
            (unsigned)s->w_dir_deg,
            (unsigned)s->w_avg_dir_deg);
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
     * ts_unix / time_iso / eg1 / uptime_s are local-UI extras. */
    if (ok && (expose_mask & STATUS_EXPOSE_SYSTEM)) {
        ok = ok && append(buf, cap, &pos,
            ",\"system\":{\"wifi_ip\":\"%s\",\"wifi_rssi_dbm\":%d,"
            "\"ntp_synced\":%s,\"fw_ver\":\"%s\","
            "\"uptime_s\":%lu,\"ts_unix\":%lu,"
            "\"time_iso\":\"%s\",\"eg1\":%lu}",
            s->ip, (int)s->rssi,
            s->ntp_synced ? "true" : "false",
            s->fw,
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
