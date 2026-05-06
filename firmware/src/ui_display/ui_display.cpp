/**
 * @file ui_display.cpp
 * @brief T8 — UI / Display task (Phase 7).
 *
 * T8 owns the 16×2 LCD (AiP31068L bridge, I2C 0x3E).  All LCD bus access is
 * protected by MX1 (shared with T4's RTC reads).
 *
 * ── Main loop (100 ms tick) ────────────────────────────────────────────────
 *  1. xQueueReceive(Q2, &evt, 100 ms)  — wait for key_event_t from T7
 *  2. xQueueReceive(Q5, &net, 0)       — poll latest network status
 *  3. Advance session-idle counter; fire timeout when threshold reached
 *  4. Dispatch key to current FSM state handler
 *  5. Advance status-page rotation timer (UI_STATUS state only)
 *  6. If display_dirty: render → lcd_flush
 *
 * ── FSM states ─────────────────────────────────────────────────────────────
 *  UI_STATUS         auto-rotate 4 pages × 5 s; any key → UI_MENU_ROOT
 *  UI_MENU_ROOT      1=Climate  2=Wind  3=Access  4=System  *=back
 *  UI_MENU_CLIMATE   11 climate params, 2 per page; #=next page  *=back
 *  UI_MENU_WIND       2 wind params, 1 page
 *  UI_MENU_ACCESS    1=Login farmer  2=Login admin  3=Logout  *=back
 *  UI_MENU_SYSTEM    stub — "See web UI"
 *  UI_PIN_ENTRY      numeric PIN; # submit  * backspace/cancel
 *  UI_EDIT_VALUE     numeric edit; # confirm  * backspace/cancel  B sign-toggle
 *
 * ── Session ────────────────────────────────────────────────────────────────
 *  Idle counter reset on every non-repeat keypress.
 *  Timeout = cfg.session_timeout_min × 60 × (1000 / UI_LOOP_MS) ticks.
 *  On timeout: LOG_SESSION close event → SESSION_NONE → UI_STATUS.
 *
 * ── Max 4 keypresses to any first-level setting (FR-UI07) ──────────────────
 *  When already authenticated:
 *    any key → MENU_ROOT [1] → MENU_CLIMATE [2] → select item [3] → EDIT_VALUE
 *  When unauthenticated, key [3] enters PIN_ENTRY; EDIT_VALUE follows on
 *  successful PIN, so edit access is still reachable within the same session.
 *
 * @author  Greenhouse Controller project
 */

#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include <Arduino.h>
#include <WiFi.h>
#include <esp_log.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "nvs_config.h"
#include "ui_display.h"
#include "../types/app_types.h"
#include "../data_manager/data_manager.h"
#include "../event_logger/event_logger.h"
#include "../auth/pin_auth.h"
#include "lcd1602.h"

static const char *TAG = "T8_UI";

/* ============================================================
 * Timing
 * ============================================================ */
#define UI_LOOP_MS          100u   /**< Main-loop tick (ms) */
#define STATUS_PAGE_TICKS    50u   /**< 5 s auto-rotate = 50 × 100 ms */
#define STATUS_PAGES          5u   /**< Number of status pages (0-3 sensors/net, 4=time) */
#define MX1_TIMEOUT_MS      200u   /**< MX1 acquire timeout */
#define DEF_SESSION_MIN       5    /**< Session timeout default (minutes) */

/* ============================================================
 * Parameter descriptor
 * ============================================================ */
typedef struct {
    const char      *short_lbl;  /**< ≤8 char sub-menu label */
    const char      *edit_lbl;   /**< ≤16 char edit-screen label (row 0) */
    const char      *nvs_ns;     /**< NVS namespace */
    const char      *nvs_key;    /**< NVS key */
    int32_t          val_min;    /**< Minimum valid value */
    int32_t          val_max;    /**< Maximum valid value */
    session_t        min_sess;   /**< Minimum session required to edit */
    log_param_id_t   log_id;     /**< LOG_PARAM_* (LOG_PARAM_NONE = no log) */
} param_def_t;

/* Climate parameters (11) */
static const param_def_t CLIMATE_PARAMS[] = {
    { "T-max-dy", "T-max day (C)   ", "climate", "t_max_day",   0, 50, SESSION_FARMER, LOG_PARAM_T_MAX_DAY  },
    { "T-max-ng", "T-max ngt (C)   ", "climate", "t_max_ngt",   0, 50, SESSION_FARMER, LOG_PARAM_T_MAX_NGT  },
    { "T-min-dy", "T-min day (C)   ", "climate", "t_min_day",   0, 50, SESSION_FARMER, LOG_PARAM_T_MIN_DAY  },
    { "T-min-ng", "T-min ngt (C)   ", "climate", "t_min_ngt",   0, 50, SESSION_FARMER, LOG_PARAM_T_MIN_NGT  },
    { "RH-max-d", "RH-max day (%)  ", "climate", "rh_max_day",  0,100, SESSION_FARMER, LOG_PARAM_RH_MAX_DAY },
    { "RH-max-n", "RH-max ngt (%)  ", "climate", "rh_max_ngt",  0,100, SESSION_FARMER, LOG_PARAM_RH_MAX_NGT },
    { "RH-min-d", "RH-min day (%)  ", "climate", "rh_min_day",  0,100, SESSION_FARMER, LOG_PARAM_RH_MIN_DAY },
    { "RH-min-n", "RH-min ngt (%)  ", "climate", "rh_min_ngt",  0,100, SESSION_FARMER, LOG_PARAM_RH_MIN_NGT },
    { "Hyst-T  ", "Hyst temp (C)   ", "climate", "hyst_t",      1, 10, SESSION_FARMER, LOG_PARAM_HYST_T     },
    { "Hyst-RH ", "Hyst humid (%)  ", "climate", "hyst_rh",     1, 20, SESSION_FARMER, LOG_PARAM_HYST_RH    },
    { "RH-ctrl ", "RH ctrl (0/1)   ", "climate", "rh_ctrl_en",  0,  1, SESSION_FARMER, LOG_PARAM_RH_CTRL_EN },
};
#define N_CLIMATE  (int)(sizeof(CLIMATE_PARAMS) / sizeof(CLIMATE_PARAMS[0]))

/* Wind parameters (2) */
static const param_def_t WIND_PARAMS[] = {
    { "Wnd-max ", "Wind max (m/s)  ", "wind", "v_max",        0, 30, SESSION_FARMER, LOG_PARAM_V_MAX },
    { "Wnd-prot", "Wind prot (0/1) ", "wind", "wind_prot_en", 0,  1, SESSION_FARMER, LOG_PARAM_NONE  },
};
#define N_WIND  (int)(sizeof(WIND_PARAMS) / sizeof(WIND_PARAMS[0]))

/* ============================================================
 * FSM state enum
 * ============================================================ */
typedef enum {
    UI_STATUS,
    UI_MENU_ROOT,
    UI_MENU_CLIMATE,
    UI_MENU_WIND,
    UI_MENU_ACCESS,
    UI_MENU_SYSTEM,
    UI_PIN_ENTRY,
    UI_EDIT_VALUE,
    UI_SET_DATE,     /**< Admin: enter date DDMMYY; # applies, * cancels */
    UI_SET_TIME,     /**< Admin: enter time HHMM;   # writes RTC, * back to date */
} ui_state_t;

/* ============================================================
 * Module-level state
 * ============================================================ */
static ui_state_t   s_state         = UI_STATUS;
static bool         s_dirty         = true;

/* Status page rotation */
static uint32_t     s_status_ticks  = 0;
static uint8_t      s_status_page   = 0;

/* Session */
static session_t    s_session       = SESSION_NONE;
static uint32_t     s_idle_ticks    = 0;

/* Latest network status (from Q5) */
static net_status_t s_net           = { false, false, "---" };

/* Sub-menu navigation */
static uint8_t      s_sub_page      = 0;  /**< Page within sub-menu (2 params/page) */

/* Pending edit (set before PIN_ENTRY; restored on success) */
static int          s_pending_param = -1; /**< Param index to edit after PIN; -1 = none */
static bool         s_pending_wind  = false;
static ui_state_t   s_return_menu   = UI_STATUS;

/* Edit value state */
static int          s_edit_param    = -1;
static bool         s_edit_is_wind  = false;
static int32_t      s_edit_old_val  = 0;
static char         s_edit_buf[8]   = {0};
static uint8_t      s_edit_len      = 0;
static bool         s_edit_neg      = false;

/* PIN entry state */
static char         s_pin_buf[12]   = {0};
static uint8_t      s_pin_len       = 0;
static pin_role_t   s_pin_role      = PIN_ROLE_FARMER;

/* Manual date/time set state (UI_SET_DATE / UI_SET_TIME) */
static bool         s_pending_settime = false; /**< # on time status page pending admin PIN */
static bool         s_pending_ap      = false; /**< # on WiFi status page pending admin PIN */
static char         s_dt_buf[9]       = {0};   /**< Digit accumulator for date/time entry */
static uint8_t      s_dt_len          = 0;     /**< Digits entered so far */
static int          s_dt_saved_year   = 0;     /**< Year from date entry, passed to time entry */
static int          s_dt_saved_mon    = 0;     /**< Month (1–12) from date entry */
static int          s_dt_saved_mday   = 0;     /**< Day (1–31) from date entry */

/* LCD row buffers (always 16 visible chars) */
static char s_row0[17] = {0};
static char s_row1[17] = {0};

/* ============================================================
 * LCD helpers
 * ============================================================ */

/**
 * @brief Write s_row0 and s_row1 to the LCD under MX1.
 */
static void lcd_flush(void)
{
    if (xSemaphoreTake(MX1, pdMS_TO_TICKS(MX1_TIMEOUT_MS)) == pdTRUE) {
        lcd_write_row(0, s_row0);
        lcd_write_row(1, s_row1);
        xSemaphoreGive(MX1);
    } else {
        ESP_LOGW(TAG, "MX1 timeout — LCD flush skipped");
    }
}

/**
 * @brief Format both rows into the internal buffers.
 * @note  Does NOT flush to hardware; does NOT touch s_dirty.
 */
static void lcd_set(const char *row0, const char *row1)
{
    snprintf(s_row0, sizeof(s_row0), "%-16.16s", row0 ? row0 : "");
    snprintf(s_row1, sizeof(s_row1), "%-16.16s", row1 ? row1 : "");
}

/**
 * @brief Show a transient message, flush immediately, block for delay_ms,
 *        then mark display dirty so the main loop re-renders the current state.
 */
static void show_msg(const char *r0, const char *r1, uint32_t delay_ms)
{
    lcd_set(r0, r1);
    lcd_flush();
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    s_dirty = true;
}

/* ============================================================
 * Config helpers
 * ============================================================ */

/**
 * @brief Read the current value of a parameter from the config shadow.
 */
static int32_t param_get(bool is_wind, int idx)
{
    cfg_shadow_t cfg;
    dm_cfg_snapshot(&cfg);

    if (!is_wind) {
        switch (idx) {
            case  0: return cfg.t_max_day;
            case  1: return cfg.t_max_ngt;
            case  2: return cfg.t_min_day;
            case  3: return cfg.t_min_ngt;
            case  4: return cfg.rh_max_day;
            case  5: return cfg.rh_max_ngt;
            case  6: return cfg.rh_min_day;
            case  7: return cfg.rh_min_ngt;
            case  8: return cfg.hyst_t;
            case  9: return cfg.hyst_rh;
            case 10: return cfg.rh_ctrl_en;
            default: return 0;
        }
    } else {
        switch (idx) {
            case 0: return cfg.v_max;
            case 1: return cfg.wind_prot_en;
            default: return 0;
        }
    }
}

/* ============================================================
 * Session helpers
 * ============================================================ */
static void session_open(session_t level)
{
    s_session    = level;
    s_idle_ticks = 0;
    log_event_t ev = {};
    ev.timestamp  = (uint32_t)time(NULL);
    ev.event_type = LOG_SESSION;
    ev.initiator  = (level == SESSION_FARMER) ? LOG_BY_FARMER : LOG_BY_ADMIN;
    ev.value_a    = (int16_t)level;
    log_post(&ev);
    ESP_LOGI(TAG, "Session opened: level=%d", (int)level);
}

static void session_close(bool timeout)
{
    if (s_session == SESSION_NONE) return;
    log_event_t ev = {};
    ev.timestamp  = (uint32_t)time(NULL);
    ev.event_type = LOG_SESSION;
    ev.initiator  = (s_session == SESSION_FARMER) ? LOG_BY_FARMER : LOG_BY_ADMIN;
    ev.value_a    = 0;
    log_post(&ev);
    ESP_LOGI(TAG, "Session closed (%s)", timeout ? "timeout" : "logout");
    s_session = SESSION_NONE;
}

/* ============================================================
 * Config change: post Q4 + log event
 * ============================================================ */
static void apply_param_change(const param_def_t *p, int32_t new_val, int32_t old_val)
{
    config_update_t upd = {};
    snprintf(upd.ns,  sizeof(upd.ns),  "%s", p->nvs_ns);
    snprintf(upd.key, sizeof(upd.key), "%s", p->nvs_key);
    upd.value = new_val;

    if (xQueueSend(Q4, &upd, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "Q4 full — config '%s/%s' lost", p->nvs_ns, p->nvs_key);
    }

    if (p->log_id != LOG_PARAM_NONE) {
        log_event_t ev = {};
        ev.timestamp  = (uint32_t)time(NULL);
        ev.event_type = LOG_SETPOINT;
        ev.initiator  = (s_session == SESSION_FARMER) ? LOG_BY_FARMER : LOG_BY_ADMIN;
        ev.param_id   = (uint8_t)p->log_id;
        ev.value_a    = (int16_t)old_val;
        ev.value_b    = (int16_t)new_val;
        log_post(&ev);
    }
    ESP_LOGI(TAG, "Config '%s/%s' changed: %ld -> %ld",
             p->nvs_ns, p->nvs_key, (long)old_val, (long)new_val);
}

/* ============================================================
 * Transition helpers
 * ============================================================ */
static void go_status(void)
{
    s_state        = UI_STATUS;
    s_status_ticks = 0;
    s_dirty        = true;
}

/**
 * @brief Begin editing a parameter.  If session is insufficient, enter
 *        PIN_ENTRY and store the pending param for later.
 */
static void begin_edit(bool is_wind, int param_idx)
{
    const param_def_t *p = is_wind ? &WIND_PARAMS[param_idx] : &CLIMATE_PARAMS[param_idx];

    if (s_session < p->min_sess) {
        /* Need higher session — request PIN */
        s_pin_role      = (p->min_sess >= SESSION_ADMIN) ? PIN_ROLE_ADMIN : PIN_ROLE_FARMER;
        s_pin_len       = 0;
        memset(s_pin_buf, 0, sizeof(s_pin_buf));
        s_pending_param = param_idx;
        s_pending_wind  = is_wind;
        s_return_menu   = is_wind ? UI_MENU_WIND : UI_MENU_CLIMATE;
        s_state         = UI_PIN_ENTRY;
        s_dirty         = true;
        return;
    }

    /* Session OK — go directly to edit */
    s_edit_param   = param_idx;
    s_edit_is_wind = is_wind;
    s_edit_old_val = param_get(is_wind, param_idx);
    s_edit_len     = 0;
    s_edit_neg     = false;
    memset(s_edit_buf, 0, sizeof(s_edit_buf));
    s_return_menu  = is_wind ? UI_MENU_WIND : UI_MENU_CLIMATE;
    s_state        = UI_EDIT_VALUE;
    s_dirty        = true;
}

/* ============================================================
 * Render helpers
 * ============================================================ */

/**
 * @brief Convert a wind direction in degrees to an 8-point cardinal string.
 * @param deg  Wind direction 0–359 (values ≥ 360 are wrapped).
 * @return  Pointer to a literal string: "N","NE","E","SE","S","SW","W","NW".
 */
static const char *deg_to_cardinal(uint16_t deg)
{
    uint16_t d = deg % 360u;
    if (d <=  22u || d >= 338u) return "N";
    if (d <=  67u)               return "NE";
    if (d <= 112u)               return "E";
    if (d <= 157u)               return "SE";
    if (d <= 202u)               return "S";
    if (d <= 247u)               return "SW";
    if (d <= 292u)               return "W";
    return "NW";
}

/* ============================================================
 * Render functions — fill s_row0 / s_row1; no flush
 * ============================================================ */

static void render_status(void)
{
    sensor_reading_t meas = {};
    bool valid = false;
    dm_meas_snapshot(&meas, &valid);
    EventBits_t bits = xEventGroupGetBits(EG1);

    char r0[17], r1[17];

    switch (s_status_page % STATUS_PAGES) {

        case 0: /* Temperature / humidity */
            if (valid) {
                /* \xDF = HD44780 ROM A00 degree symbol; split literal so 'C'
                 * is not absorbed into the hex escape sequence (\xDFC). */
                snprintf(r0, sizeof(r0), "Temp:%3d \xDF" "C     ", (int)meas.t_avg_c);
                if (bits & EG1_BIT_SENSOR_FAULT_T) {
                    snprintf(r1, sizeof(r1), "** SENSOR FAULT ");
                } else {
                    snprintf(r1, sizeof(r1), "  RH:%3d %%      ", (int)meas.rh_avg_pct);
                }
            } else {
                snprintf(r0, sizeof(r0), "Temp: --- \xDF" "C    ");
                snprintf(r1, sizeof(r1), "  RH: ---  %%    ");
            }
            break;

        case 1: /* Wind */
            if (valid) {
                snprintf(r0, sizeof(r0), "Wind:%2d.%1d m/s   ",
                         (int)(meas.wind_speed_avg_ms10 / 10),
                         (int)(meas.wind_speed_avg_ms10 % 10));
                snprintf(r1, sizeof(r1), " Dir:%3d \xDF (%-2s) ",
                         (int)meas.wind_dir_avg_deg,
                         deg_to_cardinal((uint16_t)meas.wind_dir_avg_deg));
            } else {
                snprintf(r0, sizeof(r0), "Wind: -- m/s    ");
                snprintf(r1, sizeof(r1), " Dir: --- \xDF     ");
            }
            break;

        case 2: { /* Mode / alarms */
            const char *mode;
            if      (bits & EG1_BIT_MOTOR_ALARM)    mode = "ALARM";
            else if (bits & EG1_BIT_WIND_OVERRIDE)  mode = "WIND ";
            else                                     mode = "AUTO ";

            snprintf(r0, sizeof(r0), "Mode: %-5s      ", mode);
            snprintf(r1, sizeof(r1), "Sess:%-4s %s",
                     (s_session == SESSION_ADMIN)  ? "ADMN" :
                     (s_session == SESSION_FARMER) ? "FRMR" : "NONE",
                     (bits & EG1_BIT_OTA_IN_PROGRESS) ? " OTA" : "    ");
            break;
        }

        case 3: /* Network */
            if (s_net.client_connected) {
                snprintf(r0, sizeof(r0), "WiFi: connected ");
                snprintf(r1, sizeof(r1), "%-16.16s", s_net.ip_str);
            } else if (s_net.ap_active) {
                uint8_t mac[6] = {};
                WiFi.macAddress(mac);
                char ap_ssid[17] = {};
                snprintf(ap_ssid, sizeof(ap_ssid), "Greenhouse-%02X%02X", mac[4], mac[5]);
                snprintf(r0, sizeof(r0), "WiFi: AP active ");
                snprintf(r1, sizeof(r1), "%-12.12s #=AP", ap_ssid);
            } else {
                snprintf(r0, sizeof(r0), "WiFi: --------  ");
                snprintf(r1, sizeof(r1), "            #=AP");
            }
            break;

        case 4: { /* Time + NTP/RTC source */
            cfg_shadow_t cfg_t;
            dm_cfg_snapshot(&cfg_t);
            if (cfg_t.current_unix_ts > 100000u) {
                struct tm t;
                time_t ts = (time_t)cfg_t.current_unix_ts;
                localtime_r(&ts, &t);
                snprintf(r0, sizeof(r0), "%02d-%02d-%04d %02d:%02d",
                         t.tm_mday, t.tm_mon + 1, t.tm_year + 1900,
                         t.tm_hour, t.tm_min);
            } else {
                snprintf(r0, sizeof(r0), "--/--/---- --:--");
            }
            const char *src = s_net.ntp_synced ? "NTP" : "RTC";
            snprintf(r1, sizeof(r1), "Src:%-3s    #=Set", src);
            break;
        }

        default:
            snprintf(r0, sizeof(r0), "Greenhouse Ctrl ");
            snprintf(r1, sizeof(r1), "                ");
            break;
    }
    lcd_set(r0, r1);
}

static void render_menu_root(void)
{
    lcd_set("1:Clim  2:Wind  ", "3:Access 4:Sys *");
}

/**
 * @brief Render a parameter sub-menu page (2 params per page).
 *
 * Format:  "1 <8-char label> <4-char value>"
 */
static void render_param_menu(bool is_wind)
{
    const param_def_t *params = is_wind ? WIND_PARAMS   : CLIMATE_PARAMS;
    int                n      = is_wind ? N_WIND         : N_CLIMATE;
    int                base   = s_sub_page * 2;
    char r0[17], r1[17];
    int  pages = (n + 1) / 2;

    /* Row 0 — first item on this page */
    if (base < n) {
        int32_t v = param_get(is_wind, base);
        snprintf(r0, sizeof(r0), "1 %-8.8s  %4ld", params[base].short_lbl, (long)v);
    } else {
        snprintf(r0, sizeof(r0), "#:next *:back   ");
    }

    /* Row 1 — second item on this page or navigation hint */
    if (base + 1 < n) {
        int32_t v = param_get(is_wind, base + 1);
        snprintf(r1, sizeof(r1), "2 %-8.8s  %4ld", params[base + 1].short_lbl, (long)v);
    } else {
        /* Only one item on last page — show nav hint */
        snprintf(r1, sizeof(r1), "%s",
                 (pages > 1) ? "#:next *:back   " : "*:back          ");
    }
    lcd_set(r0, r1);
}

static void render_menu_access(void)
{
    lcd_set("1:Farmer 2:Admin",
            s_session != SESSION_NONE ? "3:Logout  *:Back" : "          *:Back");
}

static void render_menu_system(void)
{
    /* Show current AP status on row 1 so user knows state before pressing */
    if (s_net.ap_active) {
        lcd_set("System settings ", "1=AP(on)    *:Bk");
    } else {
        lcd_set("System settings ", "1=WiFi AP   *:Bk");
    }
}

static void handle_menu_system(char key)
{
    switch (key) {
        case '1': /* Enable / disable WiFi AP */
            if (s_session < SESSION_ADMIN) {
                show_msg("Admin login req.", "3=Access menu   ", 2000);
                s_state = UI_MENU_SYSTEM;
            } else {
                /* Toggle: if AP already on, turn it off; if off, turn it on */
                int32_t new_val = s_net.ap_active ? 0 : 1;
                config_update_t upd = {};
                snprintf(upd.ns,  sizeof(upd.ns),  "wifi");
                snprintf(upd.key, sizeof(upd.key), "ap_enable");
                upd.value = new_val;
                if (xQueueSend(Q4, &upd, pdMS_TO_TICKS(200)) != pdTRUE) {
                    ESP_LOGW(TAG, "Q4 full — ap_enable change lost");
                }
                log_event_t ev = {};
                ev.timestamp  = (uint32_t)time(NULL);
                ev.event_type = LOG_SYSTEM;
                ev.initiator  = LOG_BY_ADMIN;
                ev.value_a    = (int16_t)new_val;
                log_post(&ev);
                if (new_val) {
                    show_msg("WiFi AP         ", "enabling...     ", 1500);
                } else {
                    show_msg("WiFi AP         ", "disabling...    ", 1500);
                }
                s_state = UI_MENU_SYSTEM;
            }
            break;

        case '*':
            s_state = UI_MENU_ROOT;
            s_dirty = true;
            break;

        default:
            break;
    }
}

static void render_set_date(void)
{
    /* Row 0: current date for reference (DD-MM-YYYY) */
    char r0[17], r1[17];
    time_t now = time(NULL);
    struct tm tn;
    localtime_r(&now, &tn);
    snprintf(r0, sizeof(r0), "Now %02d-%02d-%04d ",
             tn.tm_mday, tn.tm_mon + 1, tn.tm_year + 1900);

    /* Row 1: template "DD-MM-YY"; typed digits overwrite the template letters */
    char c0 = (s_dt_len > 0) ? s_dt_buf[0] : 'D';
    char c1 = (s_dt_len > 1) ? s_dt_buf[1] : 'D';
    char c2 = (s_dt_len > 2) ? s_dt_buf[2] : 'M';
    char c3 = (s_dt_len > 3) ? s_dt_buf[3] : 'M';
    char c4 = (s_dt_len > 4) ? s_dt_buf[4] : 'Y';
    char c5 = (s_dt_len > 5) ? s_dt_buf[5] : 'Y';
    snprintf(r1, sizeof(r1), "%c%c-%c%c-%c%c #OK *Bk",
             c0, c1, c2, c3, c4, c5);
    lcd_set(r0, r1);
}

static void render_set_time(void)
{
    /* Row 0: current time for reference */
    char r0[17], r1[17];
    time_t now = time(NULL);
    struct tm tn;
    localtime_r(&now, &tn);
    snprintf(r0, sizeof(r0), "Now:  %02d:%02d      ",
             tn.tm_hour, tn.tm_min);

    /* Row 1: template "HH:mm"; typed digits overwrite the template letters */
    char c0 = (s_dt_len > 0) ? s_dt_buf[0] : 'H';
    char c1 = (s_dt_len > 1) ? s_dt_buf[1] : 'H';
    char c2 = (s_dt_len > 2) ? s_dt_buf[2] : 'm';
    char c3 = (s_dt_len > 3) ? s_dt_buf[3] : 'm';
    snprintf(r1, sizeof(r1), "%c%c:%c%c    #OK *Bk",
             c0, c1, c2, c3);
    lcd_set(r0, r1);
}

static void render_pin_entry(void)
{
    int max_len = (s_pin_role == PIN_ROLE_FARMER) ? PIN_FARMER_DIGITS : PIN_ADMIN_DIGITS;
    char r0[17], r1[17];
    snprintf(r0, sizeof(r0), "PIN (%d dig) *=Bk", max_len);

    /* Build masked input: filled digits show '*', remaining show '_' */
    for (int i = 0; i < 16; i++) {
        if      (i < s_pin_len)  r1[i] = '*';
        else if (i < max_len)    r1[i] = '_';
        else                     r1[i] = ' ';
    }
    r1[16] = '\0';
    lcd_set(r0, r1);
}

static void render_edit_value(void)
{
    const param_def_t *p = s_edit_is_wind ? &WIND_PARAMS[s_edit_param]
                                          : &CLIMATE_PARAMS[s_edit_param];

    char r0[17], r1[17];
    snprintf(r0, sizeof(r0), "%-16.16s", p->edit_lbl);

    /* Build current edit string */
    char val_str[10];
    if (s_edit_len == 0) {
        /* Show existing value as placeholder */
        snprintf(val_str, sizeof(val_str), "%ld", (long)s_edit_old_val);
    } else {
        s_edit_buf[s_edit_len] = '\0';
        snprintf(val_str, sizeof(val_str), "%s%s",
                 s_edit_neg ? "-" : "", s_edit_buf);
    }
    snprintf(r1, sizeof(r1), "%-6.6s  #=OK *Bk", val_str);
    lcd_set(r0, r1);
}

/* ============================================================
 * Render dispatch
 * ============================================================ */
static void render(void)
{
    switch (s_state) {
        case UI_STATUS:        render_status();              break;
        case UI_MENU_ROOT:     render_menu_root();           break;
        case UI_MENU_CLIMATE:  render_param_menu(false);     break;
        case UI_MENU_WIND:     render_param_menu(true);      break;
        case UI_MENU_ACCESS:   render_menu_access();         break;
        case UI_MENU_SYSTEM:   render_menu_system();         break;
        case UI_PIN_ENTRY:     render_pin_entry();           break;
        case UI_EDIT_VALUE:    render_edit_value();          break;
        case UI_SET_DATE:      render_set_date();            break;
        case UI_SET_TIME:      render_set_time();            break;
    }
}

/* ============================================================
 * Key handlers — one per FSM state
 * ============================================================ */

static void enter_set_date(void)
{
    memset(s_dt_buf, 0, sizeof(s_dt_buf));
    s_dt_len = 0;
    s_state  = UI_SET_DATE;
    s_dirty  = true;
}

static void handle_status(char key)
{
    /* # on the WiFi/network page (page 3) → go to System menu (admin only) */
    if (key == '#' && (s_status_page % STATUS_PAGES) == 3u) {
        if (s_session >= SESSION_ADMIN) {
            s_state = UI_MENU_SYSTEM;
            s_dirty = true;
        } else {
            s_pin_role      = PIN_ROLE_ADMIN;
            s_pin_len       = 0;
            memset(s_pin_buf, 0, sizeof(s_pin_buf));
            s_pending_param = -1;
            s_pending_ap    = true;
            s_return_menu   = UI_STATUS;
            s_state         = UI_PIN_ENTRY;
            s_dirty         = true;
        }
        return;
    }
    /* # on the time page (page 4) → set date/time (admin only) */
    if (key == '#' && (s_status_page % STATUS_PAGES) == 4u) {
        if (s_session >= SESSION_ADMIN) {
            enter_set_date();
        } else {
            s_pin_role        = PIN_ROLE_ADMIN;
            s_pin_len         = 0;
            memset(s_pin_buf, 0, sizeof(s_pin_buf));
            s_pending_param   = -1;
            s_pending_settime = true;
            s_return_menu     = UI_STATUS;
            s_state           = UI_PIN_ENTRY;
            s_dirty           = true;
        }
        return;
    }
    /* Any other key → main menu */
    s_state    = UI_MENU_ROOT;
    s_sub_page = 0;
    s_dirty    = true;
}

static void handle_menu_root(char key)
{
    switch (key) {
        case '1': s_state = UI_MENU_CLIMATE; s_sub_page = 0; s_dirty = true; break;
        case '2': s_state = UI_MENU_WIND;    s_sub_page = 0; s_dirty = true; break;
        case '3': s_state = UI_MENU_ACCESS;  s_dirty = true;                 break;
        case '4': s_state = UI_MENU_SYSTEM;  s_dirty = true;                 break;
        case '*': go_status();                                                break;
        default:  break;
    }
}

static void handle_param_menu(char key, bool is_wind)
{
    const int n     = is_wind ? N_WIND   : N_CLIMATE;
    const int base  = s_sub_page * 2;
    const int pages = (n + 1) / 2;

    switch (key) {
        case '1':
            if (base < n) begin_edit(is_wind, base);
            break;
        case '2':
            if (base + 1 < n) begin_edit(is_wind, base + 1);
            break;
        case '#':
            /* Next page (wrap) */
            s_sub_page = (uint8_t)((s_sub_page + 1) % pages);
            s_dirty    = true;
            break;
        case '*':
            s_state = UI_MENU_ROOT;
            s_dirty = true;
            break;
        default:
            break;
    }
}

static void handle_menu_access(char key)
{
    switch (key) {
        case '1': /* Login farmer */
            if (s_session >= SESSION_FARMER) {
                show_msg("Already logged  ", "in as farmer    ", 1500);
                s_state = UI_MENU_ACCESS;
            } else {
                s_pin_role      = PIN_ROLE_FARMER;
                s_pin_len       = 0;
                memset(s_pin_buf, 0, sizeof(s_pin_buf));
                s_pending_param = -1;
                s_return_menu   = UI_MENU_ACCESS;
                s_state         = UI_PIN_ENTRY;
                s_dirty         = true;
            }
            break;

        case '2': /* Login admin */
            if (s_session >= SESSION_ADMIN) {
                show_msg("Already logged  ", "in as admin     ", 1500);
                s_state = UI_MENU_ACCESS;
            } else {
                s_pin_role      = PIN_ROLE_ADMIN;
                s_pin_len       = 0;
                memset(s_pin_buf, 0, sizeof(s_pin_buf));
                s_pending_param = -1;
                s_return_menu   = UI_MENU_ACCESS;
                s_state         = UI_PIN_ENTRY;
                s_dirty         = true;
            }
            break;

        case '3': /* Logout */
            session_close(false);
            show_msg("Logged out      ", "                ", 1500);
            go_status();
            break;

        case '*':
            s_state = UI_MENU_ROOT;
            s_dirty = true;
            break;

        default:
            break;
    }
}

static void handle_pin(char key)
{
    const int max_len = (s_pin_role == PIN_ROLE_FARMER) ? PIN_FARMER_DIGITS
                                                        : PIN_ADMIN_DIGITS;

    if (key >= '0' && key <= '9') {
        if (s_pin_len < (uint8_t)max_len) {
            s_pin_buf[s_pin_len++] = key;
            s_dirty = true;
        }

    } else if (key == '*') {
        if (s_pin_len > 0) {
            /* Backspace */
            s_pin_buf[--s_pin_len] = '\0';
            s_dirty = true;
        } else {
            /* Cancel — return to the menu we came from */
            s_pending_param = -1;
            s_state = s_return_menu;
            s_dirty = true;
        }

    } else if (key == '#') {
        if (s_pin_len != (uint8_t)max_len) {
            show_msg("Need all digits ", "then press #    ", 1000);
            /* Stay in PIN_ENTRY */
            return;
        }

        s_pin_buf[s_pin_len] = '\0';
        pin_auth_result_t res = pin_auth_verify(s_pin_role, s_pin_buf);

        if (res == PIN_AUTH_OK) {
            session_t lvl = (s_pin_role == PIN_ROLE_FARMER) ? SESSION_FARMER
                                                             : SESSION_ADMIN;
            session_open(lvl);
            show_msg("Access granted  ", "Welcome!        ", 1500);

            if (s_pending_ap) {
                /* Resume pending AP enable — go to System menu */
                s_pending_ap = false;
                s_state      = UI_MENU_SYSTEM;
                s_dirty      = true;
            } else if (s_pending_settime) {
                /* Resume pending date/time set */
                s_pending_settime = false;
                enter_set_date();
            } else if (s_pending_param >= 0) {
                /* Resume pending param edit */
                begin_edit(s_pending_wind, s_pending_param);
            } else {
                s_state = s_return_menu;
                s_dirty = true;
            }

        } else if (res == PIN_AUTH_LOCKED_OUT) {
            uint32_t secs = pin_auth_lockout_remaining_secs(s_pin_role);
            char r1[17];
            snprintf(r1, sizeof(r1), "Try in %lu s    ", (unsigned long)secs);
            show_msg("Locked out!     ", r1, 2500);
            go_status();

        } else {
            /* Wrong PIN — reset digits, stay in PIN_ENTRY */
            show_msg("Wrong PIN!      ", "Try again       ", 1500);
            s_pin_len = 0;
            memset(s_pin_buf, 0, sizeof(s_pin_buf));
            s_dirty = true;
        }
    }
}

static void handle_edit(char key)
{
    bool is_wind = s_edit_is_wind;
    const param_def_t *p = is_wind ? &WIND_PARAMS[s_edit_param]
                                   : &CLIMATE_PARAMS[s_edit_param];

    if (key >= '0' && key <= '9') {
        if (s_edit_len < 5) {
            s_edit_buf[s_edit_len++] = key;
            s_dirty = true;
        }

    } else if (key == '*') {
        if (s_edit_len > 0) {
            /* Backspace digit */
            s_edit_buf[--s_edit_len] = '\0';
            s_dirty = true;
        } else if (s_edit_neg) {
            /* Remove negative sign */
            s_edit_neg = false;
            s_dirty    = true;
        } else {
            /* Cancel — back to sub-menu */
            s_state = s_return_menu;
            s_dirty = true;
        }

    } else if (key == 'B') {
        /* Toggle sign (only for params that allow negative values) */
        if (p->val_min < 0) {
            s_edit_neg = !s_edit_neg;
            s_dirty    = true;
        }

    } else if (key == '#') {
        /* Confirm */
        int32_t new_val;
        if (s_edit_len == 0) {
            new_val = s_edit_old_val;  /* No digits entered — keep current */
        } else {
            s_edit_buf[s_edit_len] = '\0';
            int32_t abs_val = (int32_t)atoi(s_edit_buf);
            new_val = s_edit_neg ? -abs_val : abs_val;
        }

        /* Clamp to declared range */
        if (new_val < p->val_min) new_val = p->val_min;
        if (new_val > p->val_max) new_val = p->val_max;

        if (new_val != s_edit_old_val) {
            apply_param_change(p, new_val, s_edit_old_val);
            char saved[17];
            snprintf(saved, sizeof(saved), "Saved: %ld", (long)new_val);
            show_msg(p->edit_lbl, saved, 1500);
        } else {
            show_msg(p->edit_lbl, "No change       ", 1000);
        }

        s_state = s_return_menu;
        s_dirty = true;
    }
}

static void handle_set_date(char key)
{
    if (key >= '0' && key <= '9') {
        if (s_dt_len < 6u) {
            s_dt_buf[s_dt_len++] = key;
            s_dirty = true;
        }
    } else if (key == '*') {
        if (s_dt_len > 0u) {
            s_dt_buf[--s_dt_len] = '\0';
            s_dirty = true;
        } else {
            go_status();   /* Cancel — back to status */
        }
    } else if (key == '#') {
        if (s_dt_len < 6u) {
            show_msg("Enter DDMMYY    ", "6 digits + #    ", 1500);
            return;
        }
        int dd = (s_dt_buf[0] - '0') * 10 + (s_dt_buf[1] - '0');
        int mm = (s_dt_buf[2] - '0') * 10 + (s_dt_buf[3] - '0');
        int yy = (s_dt_buf[4] - '0') * 10 + (s_dt_buf[5] - '0');
        if (dd < 1 || dd > 31 || mm < 1 || mm > 12) {
            show_msg("Invalid date    ", "DD 01-31 MM 1-12", 1800);
            s_dt_len = 0;
            memset(s_dt_buf, 0, sizeof(s_dt_buf));
            s_dirty = true;
            return;
        }
        s_dt_saved_mday = dd;
        s_dt_saved_mon  = mm;
        s_dt_saved_year = 2000 + yy;
        /* Advance to time entry */
        memset(s_dt_buf, 0, sizeof(s_dt_buf));
        s_dt_len = 0;
        s_state  = UI_SET_TIME;
        s_dirty  = true;
    }
}

static void handle_set_time(char key)
{
    if (key >= '0' && key <= '9') {
        if (s_dt_len < 4u) {
            s_dt_buf[s_dt_len++] = key;
            s_dirty = true;
        }
    } else if (key == '*') {
        if (s_dt_len > 0u) {
            s_dt_buf[--s_dt_len] = '\0';
            s_dirty = true;
        } else {
            /* Back to date entry — restore previously typed date */
            snprintf(s_dt_buf, sizeof(s_dt_buf), "%02d%02d%02d",
                     s_dt_saved_mday, s_dt_saved_mon,
                     s_dt_saved_year % 100);
            s_dt_len = 6;
            s_state  = UI_SET_DATE;
            s_dirty  = true;
        }
    } else if (key == '#') {
        if (s_dt_len < 4u) {
            show_msg("Enter HHMM      ", "4 digits + #    ", 1500);
            return;
        }
        int hh = (s_dt_buf[0] - '0') * 10 + (s_dt_buf[1] - '0');
        int mn = (s_dt_buf[2] - '0') * 10 + (s_dt_buf[3] - '0');
        if (hh > 23 || mn > 59) {
            show_msg("Invalid time    ", "HH 0-23 MM 0-59 ", 1800);
            s_dt_len = 0;
            memset(s_dt_buf, 0, sizeof(s_dt_buf));
            s_dirty = true;
            return;
        }
        /* Build local struct tm → convert to UTC epoch via mktime() */
        struct tm t = {};
        t.tm_year  = s_dt_saved_year - 1900;
        t.tm_mon   = s_dt_saved_mon  - 1;
        t.tm_mday  = s_dt_saved_mday;
        t.tm_hour  = hh;
        t.tm_min   = mn;
        t.tm_sec   = 0;
        t.tm_isdst = -1;   /* let mktime determine DST */
        time_t new_ts = mktime(&t);

        dm_set_manual_time(new_ts);

        char saved[17];
        snprintf(saved, sizeof(saved), "%02d/%02d/%04d %02d:%02d",
                 s_dt_saved_mday, s_dt_saved_mon, s_dt_saved_year, hh, mn);
        show_msg("Time set OK     ", saved, 2000);
        go_status();
    }
}

/* ============================================================
 * T8 task entry point
 * ============================================================ */

void task_ui_display(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "T8 task alive");

    /* ---- Initialise LCD under MX1 ---- */
    {
        lcd_status_t st = LCD_ERR_COMM;
        if (xSemaphoreTake(MX1, pdMS_TO_TICKS(3000)) == pdTRUE) {
            st = lcd_init();
            xSemaphoreGive(MX1);
        } else {
            ESP_LOGE(TAG, "MX1 timeout during lcd_init");
        }
        if (st != LCD_OK) {
            ESP_LOGE(TAG, "lcd_init failed (%d) — LCD unavailable", (int)st);
        } else {
            ESP_LOGI(TAG, "LCD init OK (0x3E)");
        }
    }

    /* Boot splash — row 0: product name, row 1: version + "Init." */
    {
        char r1[17];
        snprintf(r1, sizeof(r1), "v%-9.9sInit..", FIRMWARE_VERSION);
        show_msg("Greenhouse Ctrl ", r1, 2000);
    }

    /* Start in STATUS state */
    s_state        = UI_STATUS;
    s_status_page  = 0;
    s_status_ticks = 0;
    s_dirty        = true;

    for (;;) {

        /* ── 1. Receive key event from T7 ── */
        key_event_t evt = { '\0', false };  /* '\0' = no key (matches KP_NO_KEY) */
        bool got_key = (xQueueReceive(Q2, &evt, pdMS_TO_TICKS(UI_LOOP_MS)) == pdTRUE);

        /* ── 2. Poll Q5 for latest network status (non-blocking) ── */
        {
            net_status_t net_tmp;
            if (xQueueReceive(Q5, &net_tmp, 0) == pdTRUE) {
                s_net   = net_tmp;
                s_dirty = true;
            }
        }

        /* ── 3. Session timeout ── */
        if (s_session != SESSION_NONE) {
            cfg_shadow_t cfg;
            dm_cfg_snapshot(&cfg);
            uint32_t timeout_min = (cfg.session_timeout_min > 0)
                                   ? (uint32_t)cfg.session_timeout_min
                                   : (uint32_t)DEF_SESSION_MIN;
            uint32_t timeout_ticks = timeout_min * 60u * (1000u / UI_LOOP_MS);

            s_idle_ticks++;
            if (s_idle_ticks >= timeout_ticks) {
                session_close(true);
                show_msg("Session timeout ", "Returning home..", 1500);
                go_status();
                got_key = false;
            }
        }

        /* ── 4. Dispatch key event ── */
        if (got_key) {
            if (!evt.repeated) {
                s_idle_ticks = 0;  /* Reset session timeout on non-repeat */
            }

            switch (s_state) {
                case UI_STATUS:        handle_status(evt.key);             break;
                case UI_MENU_ROOT:     handle_menu_root(evt.key);          break;
                case UI_MENU_CLIMATE:  handle_param_menu(evt.key, false);  break;
                case UI_MENU_WIND:     handle_param_menu(evt.key, true);   break;
                case UI_MENU_ACCESS:   handle_menu_access(evt.key);        break;
                case UI_MENU_SYSTEM:   handle_menu_system(evt.key);         break;
                case UI_PIN_ENTRY:     handle_pin(evt.key);                break;
                case UI_EDIT_VALUE:    handle_edit(evt.key);               break;
                case UI_SET_DATE:      handle_set_date(evt.key);           break;
                case UI_SET_TIME:      handle_set_time(evt.key);           break;
            }
        }

        /* ── 5. Status page auto-rotation ── */
        if (s_state == UI_STATUS) {
            if (++s_status_ticks >= STATUS_PAGE_TICKS) {
                s_status_ticks = 0;
                s_status_page  = (uint8_t)((s_status_page + 1) % STATUS_PAGES);
                s_dirty        = true;
            }
        } else {
            s_status_ticks = 0;
        }

        /* ── 6. Render if dirty ── */
        if (s_dirty) {
            render();
            lcd_flush();
            s_dirty = false;
        }
    }
}
