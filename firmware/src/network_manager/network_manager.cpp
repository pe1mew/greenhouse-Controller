/**
 * @file network_manager.cpp
 * @brief T10 — Network Manager task (Phase 8).
 *
 * Manages WiFi station (client) and soft-AP lifecycle, triggers NTP
 * synchronisation, updates the DS1307 RTC via TN4 after NTP sync, and
 * reports network status to T8 via Q5.
 *
 * ── WiFi credentials (NVS namespace "wifi") ────────────────────────────────
 *  Key         Type    Default   Meaning
 *  ssid        str     ""        Station SSID; empty disables client mode
 *  psk         str     ""        Station passphrase (empty = open network)
 *  ap_enable   i32     0         1 = start soft-AP; 0 = stop soft-AP
 *  ap_psk      str     ""        Soft-AP passphrase (empty = open AP)
 *
 * ── Client state machine ───────────────────────────────────────────────────
 *  NET_IDLE        No SSID configured; station disabled.
 *  NET_CONNECTING  WiFi.begin() called; polling for WL_CONNECTED.
 *                  Timeout 30 s → NET_BACKOFF.
 *  NET_CONNECTED   WL_CONNECTED; configTime() running.
 *  NET_RUNNING     Client up; NTP result resolved (success or timeout).
 *  NET_BACKOFF     Connection lost; waiting exponential backoff then retry.
 *                  Backoff sequence: 2 → 4 → 8 → 16 → 32 → 60 s (cap).
 *
 * ── AP mode ────────────────────────────────────────────────────────────────
 *  AP mode is independent of client state (WIFI_AP_STA).
 *  AP SSID: "Greenhouse-XXYY" (last two bytes of MAC address).
 *  AP auto-shutdown: cfg.ap_timeout_min (from MX4, 0 = never shutdown).
 *  T10 polls NVS "wifi/ap_enable" every NET_POLL_MS; starts or stops AP when
 *  the flag changes.  T8 or T11 enable AP by writing Q4 {ns="wifi",
 *  key="ap_enable", value=1}; T4 persists it to NVS; T10 reads it here.
 *
 * ── NTP synchronisation ────────────────────────────────────────────────────
 *  On WL_CONNECTED: configTime(0, 0, "pool.ntp.org").
 *  Poll time(NULL) up to NTP_WAIT_STEPS × 1 s for a plausible timestamp
 *  (> 1 700 000 000 = 2023-11-14, safely below any real current date).
 *  On success: xTaskNotify(task_t4, DM_NOTIFY_NTP_SYNCED, eSetBits).
 *  T4 then calls rtc_set_time() under MX1 to update the DS1307.
 *  Periodic resync: while NET_RUNNING, run_ntp_sync() is called again every
 *  NTP_RESYNC_INTERVAL_S (86400 s = 24 h) to keep the DS1307 accurate.
 *  Geo/timezone is NOT re-fetched on periodic resyncs (location is stable).
 *
 * ── Q5 status posting ──────────────────────────────────────────────────────
 *  xQueueOverwrite(Q5, &status) on every state change.
 *  Fields: client_connected, ap_active, ip_str (STA IP or "" if not connected).
 *
 * @author  Greenhouse Controller project
 */

#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include <Arduino.h>
#include <esp_log.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>    /* atof */
#include <math.h>      /* fabs, floorf */

#include <WiFi.h>      /* Arduino-ESP32 WiFi */
#include <HTTPClient.h> /* HTTP geo/timezone lookup */

#include "network_manager.h"
#include "../types/app_types.h"
#include "../data_manager/data_manager.h"
#include "../event_logger/event_logger.h"
#include "nvs_config.h"

static const char *TAG = "T10_NET";

/* ============================================================
 * Compile-time constants
 * ============================================================ */
#define NET_POLL_MS          5000u   /**< Main loop tick (ms) */
#define STA_CONNECT_TIMEOUT_MS 30000u/**< Time to wait for WL_CONNECTED */
#define NTP_WAIT_STEPS          30   /**< Seconds to poll for NTP sync */
#define NTP_MIN_EPOCH  1700000000L   /**< Plausibility threshold (2023-11-14) */
#define BACKOFF_INIT_MS   2000u      /**< Initial reconnect backoff */
#define BACKOFF_MAX_MS   60000u      /**< Maximum reconnect backoff */
#define NTP_RESYNC_INTERVAL_S  86400UL /**< Re-sync NTP once per 24 h while connected */

/** Default WPA2 passphrase for the soft-AP.
 *  Stored as plaintext in NVS (wifi/ap_psk); WPA2 requires the raw key.
 *  Configurable by the administrator via the web interface (T11).
 *  See TSDS §5.x WiFi AP mode. */
#define AP_PSK_DEFAULT  "0123456789"

/* ============================================================
 * Client FSM states
 * ============================================================ */
typedef enum {
    NET_IDLE,        /**< No SSID configured */
    NET_CONNECTING,  /**< WiFi.begin() called; awaiting WL_CONNECTED */
    NET_CONNECTED,   /**< WL_CONNECTED; running NTP sync inline */
    NET_RUNNING,     /**< Fully up; monitoring for drops */
    NET_BACKOFF,     /**< Lost connection; waiting before retry */
} net_client_state_t;

/* ============================================================
 * Module state
 * ============================================================ */
static net_client_state_t s_state          = NET_IDLE;
static bool               s_ap_active      = false;
static bool               s_ntp_synced     = false;
static uint32_t           s_backoff_ms     = BACKOFF_INIT_MS;
static TickType_t         s_conn_start     = 0;   /**< Tick when NET_CONNECTING entered */
static TickType_t         s_ap_started     = 0;   /**< Tick when AP was started */
static TickType_t         s_last_ntp_tick  = 0;   /**< Tick of last successful NTP sync */
static int32_t            s_ap_enabled_nvs = 0;   /**< Last-known NVS ap_enable value */

/* WiFi credentials (read from NVS at startup) */
static char s_ssid[64]   = {0};
static char s_psk[64]    = {0};
static char s_ap_psk[64] = {0};
static char s_ap_ssid[24] = {0};  /**< "Greenhouse-XXYY", built from MAC */

/* ============================================================
 * IANA → POSIX TZ lookup table
 * ============================================================ */

static const struct { const char *iana; const char *posix; } s_tz_table[] = {
    /* UTC */
    { "UTC",                              "UTC0" },
    { "Etc/UTC",                          "UTC0" },
    { "Etc/GMT",                          "UTC0" },
    /* Europe — CET/CEST (UTC+1/+2) */
    { "Europe/Amsterdam",                 "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Berlin",                    "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Brussels",                  "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Copenhagen",                "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Luxembourg",                "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Madrid",                    "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Malta",                     "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Oslo",                      "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Paris",                     "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Prague",                    "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Rome",                      "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Stockholm",                 "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Vienna",                    "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Warsaw",                    "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Zurich",                    "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Africa/Algiers",                   "CET-1" },
    { "Africa/Tunis",                     "CET-1" },
    /* Europe — WET/WEST (UTC+0/+1) */
    { "Europe/Lisbon",                    "WET0WEST,M3.5.0/1,M10.5.0" },
    { "Atlantic/Canary",                  "WET0WEST,M3.5.0/1,M10.5.0" },
    { "Atlantic/Madeira",                 "WET0WEST,M3.5.0/1,M10.5.0" },
    /* Europe — GMT/BST (UTC+0/+1) */
    { "Europe/London",                    "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Europe/Dublin",                    "IST-1GMT0,M10.5.0,M3.5.0/1" },
    /* Europe — EET/EEST (UTC+2/+3) */
    { "Europe/Athens",                    "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Bucharest",                 "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Helsinki",                  "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Kiev",                      "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Kyiv",                      "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Riga",                      "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Sofia",                     "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Tallinn",                   "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Vilnius",                   "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Asia/Nicosia",                     "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Asia/Famagusta",                   "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    /* Europe — no DST */
    { "Europe/Moscow",                    "MSK-3" },
    { "Europe/Minsk",                     "FET-3" },
    { "Europe/Istanbul",                  "TRT-3" },
    { "Asia/Istanbul",                    "TRT-3" },
    /* Africa */
    { "Africa/Cairo",                     "EET-2" },
    { "Africa/Johannesburg",              "SAST-2" },
    { "Africa/Harare",                    "CAT-2" },
    { "Africa/Nairobi",                   "EAT-3" },
    { "Africa/Addis_Ababa",               "EAT-3" },
    { "Africa/Lagos",                     "WAT-1" },
    { "Africa/Casablanca",                "WET0" },
    { "Africa/Abidjan",                   "GMT0" },
    { "Africa/Accra",                     "GMT0" },
    /* Asia — Middle East */
    { "Asia/Dubai",                       "GST-4" },
    { "Asia/Muscat",                      "GST-4" },
    { "Asia/Riyadh",                      "AST-3" },
    { "Asia/Baghdad",                     "AST-3" },
    { "Asia/Kuwait",                      "AST-3" },
    { "Asia/Beirut",                      "EET-2EEST,M3.5.0/0,M10.5.0/0" },
    { "Asia/Amman",                       "AST-3" },
    { "Asia/Jerusalem",                   "IST-2IDT,M3.4.4/26,M10.5.0" },
    { "Asia/Tehran",                      "IRST-3:30IRDT,80/0,264/0" },
    { "Asia/Kabul",                       "AFT-4:30" },
    /* Asia — South */
    { "Asia/Karachi",                     "PKT-5" },
    { "Asia/Kolkata",                     "IST-5:30" },
    { "Asia/Calcutta",                    "IST-5:30" },
    { "Asia/Colombo",                     "IST-5:30" },
    { "Asia/Kathmandu",                   "NPT-5:45" },
    { "Asia/Dhaka",                       "BDT-6" },
    { "Asia/Tashkent",                    "UZT-5" },
    { "Asia/Almaty",                      "ALMT-6" },
    /* Asia — SE */
    { "Asia/Bangkok",                     "ICT-7" },
    { "Asia/Ho_Chi_Minh",                 "ICT-7" },
    { "Asia/Phnom_Penh",                  "ICT-7" },
    { "Asia/Vientiane",                   "ICT-7" },
    { "Asia/Jakarta",                     "WIB-7" },
    { "Asia/Singapore",                   "SGT-8" },
    { "Asia/Kuala_Lumpur",                "MYT-8" },
    { "Asia/Manila",                      "PHT-8" },
    /* Asia — East */
    { "Asia/Shanghai",                    "CST-8" },
    { "Asia/Hong_Kong",                   "HKT-8" },
    { "Asia/Taipei",                      "CST-8" },
    { "Asia/Seoul",                       "KST-9" },
    { "Asia/Tokyo",                       "JST-9" },
    /* Australia */
    { "Australia/Perth",                  "AWST-8" },
    { "Australia/Darwin",                 "ACST-9:30" },
    { "Australia/Adelaide",               "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
    { "Australia/Brisbane",               "AEST-10" },
    { "Australia/Sydney",                 "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Australia/Melbourne",              "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Australia/Hobart",                 "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    /* Pacific */
    { "Pacific/Honolulu",                 "HST10" },
    { "Pacific/Auckland",                 "NZST-12NZDT,M9.5.0,M4.1.0/3" },
    { "Pacific/Fiji",                     "FJT-12" },
    { "Pacific/Guam",                     "ChST-10" },
    { "Pacific/Port_Moresby",             "PGT-10" },
    /* Americas */
    { "America/New_York",                 "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Detroit",                  "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Toronto",                  "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Indiana/Indianapolis",     "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Chicago",                  "CST6CDT,M3.2.0,M11.1.0" },
    { "America/Winnipeg",                 "CST6CDT,M3.2.0,M11.1.0" },
    { "America/Denver",                   "MST7MDT,M3.2.0,M11.1.0" },
    { "America/Edmonton",                 "MST7MDT,M3.2.0,M11.1.0" },
    { "America/Phoenix",                  "MST7" },
    { "America/Los_Angeles",              "PST8PDT,M3.2.0,M11.1.0" },
    { "America/Vancouver",                "PST8PDT,M3.2.0,M11.1.0" },
    { "America/Anchorage",                "AKST9AKDT,M3.2.0,M11.1.0" },
    { "America/Halifax",                  "AST4ADT,M3.2.0,M11.1.0" },
    { "America/Mexico_City",              "CST6CDT,M4.1.0,M10.5.0" },
    { "America/Bogota",                   "COT5" },
    { "America/Lima",                     "PET5" },
    { "America/Caracas",                  "VET4:30" },
    { "America/Santiago",                 "CLT4CLST,M10.2.6/24,M3.2.6/24" },
    { "America/Sao_Paulo",                "BRT3" },
    { "America/Argentina/Buenos_Aires",   "ART3" },
    { "America/Montevideo",               "UYT3" },
    { "America/Manaus",                   "AMT4" },
    { NULL, NULL }
};

/** Look up a POSIX TZ string for the given IANA timezone name.
 *  Returns NULL if not found in the table. */
static const char *iana_to_posix(const char *iana)
{
    for (int i = 0; s_tz_table[i].iana != NULL; i++) {
        if (strcmp(s_tz_table[i].iana, iana) == 0) {
            return s_tz_table[i].posix;
        }
    }
    return NULL;
}

/** Convert a float coordinate to integer degrees + millidegree fraction.
 *  Example: 52.3676 → deg=52, frac=368  (frac = round(fractional * 1000))
 *  Negative latitudes/longitudes: deg is negative, frac is always non-negative. */
static void float_to_deg_frac(float val, int32_t *deg, int32_t *frac)
{
    bool neg = (val < 0.0f);
    if (neg) val = -val;
    *deg  = (int32_t)val;
    *frac = (int32_t)((val - (float)*deg) * 1000.0f + 0.5f);
    if (*frac >= 1000) { *deg += 1; *frac -= 1000; }  /* carry */
    if (neg) *deg = -*deg;
}

/** Parse the ip-api.com JSON response.
 *  Expected: {"status":"success","lat":52.37,"lon":4.90,"timezone":"Europe/Amsterdam"}
 *  Returns true on success. */
static bool parse_geo_response(const char *body,
                                float *out_lat, float *out_lon,
                                char *out_tz, size_t tz_len)
{
    if (!strstr(body, "\"status\":\"success\"")) return false;

    const char *p = strstr(body, "\"lat\":");
    if (!p) return false;
    *out_lat = (float)atof(p + 6);

    p = strstr(body, "\"lon\":");
    if (!p) return false;
    *out_lon = (float)atof(p + 6);

    p = strstr(body, "\"timezone\":\"");
    if (!p) return false;
    p += 12;
    const char *end = strchr(p, '"');
    if (!end) return false;
    size_t len = (size_t)(end - p);
    if (len >= tz_len) len = tz_len - 1;
    memcpy(out_tz, p, len);
    out_tz[len] = '\0';

    return true;
}

/** Post a single i32 update to Q4 (T4 — Data Manager).
 *  T4 writes to NVS and updates the in-RAM shadow. */
static void post_q4(const char *ns, const char *key, int32_t value)
{
    config_update_t upd;
    memset(&upd, 0, sizeof(upd));
    snprintf(upd.ns,  sizeof(upd.ns),  "%s", ns);
    snprintf(upd.key, sizeof(upd.key), "%s", key);
    upd.value = value;
    xQueueSend(Q4, &upd, pdMS_TO_TICKS(200));
}

/* ============================================================
 * Helpers
 * ============================================================ */

static void post_q5(bool client_conn, bool ap_active, const char *ip)
{
    net_status_t st;
    st.client_connected = client_conn;
    st.ap_active        = ap_active;
    st.ntp_synced       = s_ntp_synced;
    snprintf(st.ip_str, sizeof(st.ip_str), "%s", ip ? ip : "");
    xQueueOverwrite(Q5, &st);
}

static void log_sys(int16_t value_a, int16_t value_b)
{
    log_event_t ev = {};
    ev.timestamp  = (uint32_t)time(NULL);
    ev.event_type = LOG_SYSTEM;
    ev.initiator  = LOG_BY_SYSTEM;
    ev.value_a    = value_a;
    ev.value_b    = value_b;
    log_post(&ev);
}

/* ============================================================
 * AP management
 * ============================================================ */

static void start_ap(void)
{
    if (s_ap_active) return;

    /* Use NVS password if set; fall back to factory default (never open AP). */
    const char *psk = (s_ap_psk[0] != '\0') ? s_ap_psk : AP_PSK_DEFAULT;
    bool ok = WiFi.softAP(s_ap_ssid, psk, /*channel=*/1,
                          /*hidden=*/0, /*max_conn=*/4);
    if (ok) {
        s_ap_active  = true;
        s_ap_started = xTaskGetTickCount();
        ESP_LOGI(TAG, "AP started: SSID='%s' IP=%s",
                 s_ap_ssid, WiFi.softAPIP().toString().c_str());
        log_sys(3, 1);   /* value_a=3: AP event; value_b=1: started */
        post_q5(s_state == NET_RUNNING, true,
                s_state == NET_RUNNING ? WiFi.localIP().toString().c_str() : "");
    } else {
        ESP_LOGE(TAG, "WiFi.softAP() failed");
    }
}

static void stop_ap(void)
{
    if (!s_ap_active) return;
    WiFi.softAPdisconnect(false);  /* false = leave radio up for STA */
    s_ap_active = false;
    ESP_LOGI(TAG, "AP stopped");
    log_sys(3, 0);   /* value_a=3: AP event; value_b=0: stopped */
    post_q5(s_state == NET_RUNNING, false,
            s_state == NET_RUNNING ? WiFi.localIP().toString().c_str() : "");
}

/**
 * @brief Check NVS "wifi/ap_enable" and start/stop AP accordingly.
 *        Also enforces AP auto-shutdown timer.
 */
static void poll_ap(void)
{
    /* ---- ap_enable flag ---- */
    int32_t ap_en = 0;
    nvs_cfg_get_i32(NVS_NS_WIFI, "ap_enable", &ap_en);

    if (ap_en != s_ap_enabled_nvs) {
        s_ap_enabled_nvs = ap_en;
        if (ap_en) {
            start_ap();
        } else {
            stop_ap();
        }
    }

    /* ---- Auto-shutdown ---- */
    if (s_ap_active) {
        cfg_shadow_t cfg;
        dm_cfg_snapshot(&cfg);
        if (cfg.ap_timeout_min > 0) {
            uint32_t elapsed_ms = (uint32_t)(
                (xTaskGetTickCount() - s_ap_started) * portTICK_PERIOD_MS);
            uint32_t limit_ms = (uint32_t)cfg.ap_timeout_min * 60u * 1000u;
            if (elapsed_ms >= limit_ms) {
                ESP_LOGI(TAG, "AP auto-shutdown after %ld min",
                         (long)cfg.ap_timeout_min);
                /* Clear the NVS flag so it doesn't restart on next poll */
                nvs_cfg_set_i32(NVS_NS_WIFI, "ap_enable", 0);
                s_ap_enabled_nvs = 0;
                stop_ap();
            }
        }
    }
}

/* ============================================================
 * Geolocation + timezone sync
 *
 * Fetches location and IANA timezone from ip-api.com using the
 * device's public IP address.  On success:
 *  - lat/lon are posted to Q4 so T4 updates s_cfg and recalculates
 *    sunrise/sunset (and persists to NVS via apply_config_update).
 *  - tz_str is written to NVS directly and applied immediately via
 *    setenv/tzset so localtime_r returns the correct local time.
 *
 * Called once after a successful NTP sync, while T10 still holds
 * the WiFi connection.  Timeout is 5 s.
 * ============================================================ */

static void do_geo_sync(void)
{
    ESP_LOGI(TAG, "Starting geo/timezone sync via ip-api.com");

    HTTPClient http;
    http.begin("http://ip-api.com/json?fields=status,lat,lon,timezone");
    http.setTimeout(5000);
    int code = http.GET();
    if (code != 200) {
        ESP_LOGW(TAG, "Geo sync HTTP GET failed (code=%d)", code);
        http.end();
        return;
    }

    String body = http.getString();
    http.end();

    float lat = 0.0f, lon = 0.0f;
    char  iana_tz[64] = {};
    if (!parse_geo_response(body.c_str(), &lat, &lon, iana_tz, sizeof(iana_tz))) {
        ESP_LOGW(TAG, "Geo sync: failed to parse response: %s", body.c_str());
        return;
    }

    ESP_LOGI(TAG, "Geo: lat=%.4f lon=%.4f timezone='%s'", lat, lon, iana_tz);

    /* Convert to deg + millidegree fraction for NVS storage */
    int32_t lat_deg, lat_frac, lon_deg, lon_frac;
    float_to_deg_frac(lat, &lat_deg, &lat_frac);
    float_to_deg_frac(lon, &lon_deg, &lon_frac);

    /* Update lat/lon via Q4 — T4 writes NVS, updates shadow, recalcs sunrise */
    post_q4(NVS_NS_SYSTEM, "lat_deg",  lat_deg);
    post_q4(NVS_NS_SYSTEM, "lat_frac", lat_frac);
    post_q4(NVS_NS_SYSTEM, "lon_deg",  lon_deg);
    post_q4(NVS_NS_SYSTEM, "lon_frac", lon_frac);

    /* Look up POSIX TZ string and apply */
    const char *posix_tz = iana_to_posix(iana_tz);
    if (posix_tz) {
        nvs_cfg_set_str(NVS_NS_SYSTEM, "tz_str", posix_tz);
        setenv("TZ", posix_tz, 1);
        tzset();
        ESP_LOGI(TAG, "TZ applied: '%s'  (IANA='%s')", posix_tz, iana_tz);
    } else {
        ESP_LOGW(TAG, "Geo sync: IANA zone '%s' not in table — TZ not changed", iana_tz);
    }

    log_sys(4, 1);   /* value_a=4: geo event; value_b=1: success */
}

/* ============================================================
 * NTP sync
 *
 * @param do_geo  true  = also fetch geolocation + timezone (initial connect).
 *                false = skip geo (periodic 24-h resync; location is stable).
 * ============================================================ */

static void run_ntp_sync(bool do_geo = true)
{
    ESP_LOGI(TAG, "Starting NTP sync (pool.ntp.org)%s",
             do_geo ? "" : " [periodic resync — geo skipped]");
    configTime(0, 0, "pool.ntp.org");

    /* configTime() resets the TZ env to UTC. Re-apply the persisted POSIX TZ
     * so localtime_r() keeps working after every sync (initial and 24h resync).
     * NVS is the source of truth — the in-memory shadow may lag a geo-sync
     * update because do_geo_sync() / web_server write NVS without posting Q4. */
    char tz_buf[64] = {};
    nvs_cfg_get_str(NVS_NS_SYSTEM, "tz_str", tz_buf, sizeof(tz_buf));
    if (tz_buf[0] != '\0') {
        setenv("TZ", tz_buf, 1);
        tzset();
    }

    for (int i = 0; i < NTP_WAIT_STEPS; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (time(NULL) > NTP_MIN_EPOCH) {
            break;
        }
    }

    if (time(NULL) > NTP_MIN_EPOCH) {
        s_ntp_synced    = true;
        s_last_ntp_tick = xTaskGetTickCount();
        ESP_LOGI(TAG, "NTP sync OK — unix=%lu", (unsigned long)time(NULL));
        /* Notify T4 to write the new time to the DS1307 */
        xTaskNotify(task_t4, DM_NOTIFY_NTP_SYNCED, eSetBits);
        log_sys(2, 1);   /* value_a=2: NTP event; value_b=1: synced */
        if (do_geo) {
            /* Fetch geolocation and apply timezone (initial connect only) */
            do_geo_sync();
        }
    } else {
        ESP_LOGW(TAG, "NTP sync timeout after %d s", NTP_WAIT_STEPS);
        log_sys(2, 0);   /* value_a=2: NTP event; value_b=0: timeout */
    }
}

/* ============================================================
 * Client state-machine step — called every NET_POLL_MS
 * Returns the delay to apply before the next call (ms).
 * ============================================================ */

static uint32_t step_client(void)
{
    wl_status_t wifi_st = WiFi.status();

    switch (s_state) {

        /* ── NET_IDLE ─────────────────────────────────────────── */
        case NET_IDLE:
            /* Nothing to do; re-check SSID periodically in case it was
             * written to NVS by the web server after first boot. */
            nvs_cfg_get_str(NVS_NS_WIFI, "ssid", s_ssid, sizeof(s_ssid));
            if (s_ssid[0] != '\0') {
                nvs_cfg_get_str(NVS_NS_WIFI, "psk", s_psk, sizeof(s_psk));
                ESP_LOGI(TAG, "SSID now configured — connecting to '%s'", s_ssid);
                WiFi.begin(s_ssid, s_psk[0] ? s_psk : nullptr);
                s_conn_start = xTaskGetTickCount();
                s_state      = NET_CONNECTING;
            }
            return NET_POLL_MS;

        /* ── NET_CONNECTING ───────────────────────────────────── */
        case NET_CONNECTING:
            if (wifi_st == WL_CONNECTED) {
                ESP_LOGI(TAG, "WiFi connected: IP=%s RSSI=%d dBm",
                         WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
                log_sys(1, 1);   /* value_a=1: STA event; value_b=1: connected */
                s_state = NET_CONNECTED;
                /* Post Q5 immediately so T8 can show IP before NTP wait */
                post_q5(true, s_ap_active, WiFi.localIP().toString().c_str());
                /* Run NTP sync inline (blocks up to NTP_WAIT_STEPS s) */
                run_ntp_sync();
                s_state      = NET_RUNNING;
                s_backoff_ms = BACKOFF_INIT_MS;   /* Reset backoff on success */

                /* Auto-stop AP when client is up: AP was only needed for
                 * initial configuration.  Clear NVS flag so it does not
                 * restart on the next boot or poll_ap() call. */
                if (s_ap_active) {
                    nvs_cfg_set_i32(NVS_NS_WIFI, "ap_enable", 0);
                    s_ap_enabled_nvs = 0;
                    stop_ap();
                }

                post_q5(true, s_ap_active, WiFi.localIP().toString().c_str());

            } else {
                uint32_t elapsed_ms = (uint32_t)(
                    (xTaskGetTickCount() - s_conn_start) * portTICK_PERIOD_MS);
                if (elapsed_ms >= STA_CONNECT_TIMEOUT_MS) {
                    ESP_LOGW(TAG, "WiFi connect timeout — backoff %lu ms",
                             (unsigned long)s_backoff_ms);
                    s_state = NET_BACKOFF;
                }
            }
            return NET_POLL_MS;

        /* ── NET_CONNECTED ────────────────────────────────────── */
        case NET_CONNECTED:
            /* Transient state — handled inline in NET_CONNECTING branch above.
             * Should not normally be entered from the loop directly. */
            s_state = NET_RUNNING;
            return NET_POLL_MS;

        /* ── NET_RUNNING ──────────────────────────────────────── */
        case NET_RUNNING:
            if (wifi_st != WL_CONNECTED) {
                ESP_LOGW(TAG, "WiFi lost (status=%d) — backoff %lu ms",
                         (int)wifi_st, (unsigned long)s_backoff_ms);
                log_sys(1, 0);   /* value_a=1: STA event; value_b=0: disconnected */
                s_ntp_synced = false;
                s_state      = NET_BACKOFF;
                post_q5(false, s_ap_active, "");
            } else if (s_ntp_synced) {
                /* Periodic 24-hour NTP resync to keep the DS1307 accurate.
                 * Geo/timezone is NOT re-fetched — location is assumed stable. */
                TickType_t elapsed = xTaskGetTickCount() - s_last_ntp_tick;
                if (elapsed >= pdMS_TO_TICKS(NTP_RESYNC_INTERVAL_S * 1000UL)) {
                    ESP_LOGI(TAG, "24 h periodic NTP resync");
                    run_ntp_sync(false);   /* do_geo = false */
                }
            }
            return NET_POLL_MS;

        /* ── NET_BACKOFF ──────────────────────────────────────── */
        case NET_BACKOFF: {
            uint32_t delay = s_backoff_ms;
            /* Double backoff for next time (capped) */
            s_backoff_ms = (s_backoff_ms < BACKOFF_MAX_MS)
                           ? (s_backoff_ms * 2u) : BACKOFF_MAX_MS;

            /* Re-read credentials in case they were updated via web server */
            nvs_cfg_get_str(NVS_NS_WIFI, "ssid", s_ssid, sizeof(s_ssid));
            nvs_cfg_get_str(NVS_NS_WIFI, "psk",  s_psk,  sizeof(s_psk));

            if (s_ssid[0] == '\0') {
                ESP_LOGI(TAG, "SSID cleared — back to IDLE");
                WiFi.disconnect(false);
                s_state = NET_IDLE;
                return NET_POLL_MS;
            }

            ESP_LOGI(TAG, "Reconnecting to '%s' in %lu ms", s_ssid,
                     (unsigned long)delay);
            WiFi.disconnect(false);
            vTaskDelay(pdMS_TO_TICKS(delay));
            WiFi.begin(s_ssid, s_psk[0] ? s_psk : nullptr);
            s_conn_start = xTaskGetTickCount();
            s_state      = NET_CONNECTING;
            return NET_POLL_MS;
        }
    }
    return NET_POLL_MS;
}

/* ============================================================
 * T10 task entry point
 * ============================================================ */

void task_network_manager(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "T10 task alive");

    /* ── Read WiFi credentials from NVS ── */
    nvs_cfg_get_str_or_default(NVS_NS_WIFI, "ssid",   "", s_ssid,   sizeof(s_ssid));
    nvs_cfg_get_str_or_default(NVS_NS_WIFI, "psk",    "", s_psk,    sizeof(s_psk));
    nvs_cfg_get_str_or_default(NVS_NS_WIFI, "ap_psk", AP_PSK_DEFAULT, s_ap_psk, sizeof(s_ap_psk));

    ESP_LOGI(TAG, "NVS WiFi: ssid='%s' psk=%s",
             s_ssid[0] ? s_ssid : "(empty)",
             s_psk[0]  ? "***"  : "(empty)");

    /* ── Set WiFi mode (AP+STA allows both simultaneously) ── */
    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(false);  /* T10 manages reconnection itself */

    /* ── Build AP SSID from last 2 MAC bytes ── */
    {
        uint8_t mac[6] = {0};
        WiFi.macAddress(mac);
        snprintf(s_ap_ssid, sizeof(s_ap_ssid),
                 "Greenhouse-%02X%02X", mac[4], mac[5]);
        ESP_LOGI(TAG, "AP SSID will be '%s'", s_ap_ssid);
    }

    /* ── AP always starts disabled — admin must enable explicitly each boot ── */
    nvs_cfg_set_i32(NVS_NS_WIFI, "ap_enable", 0);
    s_ap_enabled_nvs = 0;

    /* ── Start client if SSID is configured ── */
    if (s_ssid[0] != '\0') {
        ESP_LOGI(TAG, "Connecting to SSID '%s'", s_ssid);
        WiFi.begin(s_ssid, s_psk[0] ? s_psk : nullptr);
        s_conn_start = xTaskGetTickCount();
        s_state      = NET_CONNECTING;
    } else {
        ESP_LOGI(TAG, "No SSID configured — station disabled");
        s_state = NET_IDLE;
    }

    /* Initial Q5 post */
    post_q5(false, s_ap_active, "");

    /* ── Main loop ── */
    for (;;) {
        /* 1. AP management (start/stop/timeout) */
        poll_ap();

        /* 2. Client state machine step */
        uint32_t delay_ms = step_client();

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}
