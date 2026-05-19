/**
 * @file network_manager.cpp
 * @brief T10 — Network Manager task (Phase 6.N.1, minimal).
 *
 * **alpha.6.14 minimal-T10 status** (2026-05-18):
 *
 * The original 1.20.3 file (720 lines, Arduino-WiFi.h + Arduino-HTTPClient
 * based) is archived alongside this file as `network_manager_1.20.3_original
 * .cpp.archived`. Per the migration plan, the 1.20.3 state machine
 * (`WiFi.begin / WL_CONNECTED / configTime / WiFi.softAP / HTTPClient geo
 * lookup`) was always destined for a from-scratch IDF rewrite — the
 * arduino-WiFi APIs don't map 1:1 to esp_wifi/esp_netif/esp_event.
 *
 * This minimal T10 implements the **operationally essential** subset:
 *  1. Posts `net_status_t` to Q5 (consumed by T8's LCD WiFi status page).
 *  2. Sends `DM_NOTIFY_NTP_SYNCED` (TN4) to T4 after the initial NTP sync,
 *     so T4 writes the post-SNTP system time back to the DS1307 RTC.
 *  3. Polls esp_netif/esp_wifi state every NET_POLL_MS, re-posts Q5 on
 *     any change.
 *
 * **Landed since the original minimal-T10:**
 *  - **IP geolocation + timezone sync** (alpha.6.28 / Phase 6.14.X step 1)
 *    via `http://ip-api.com/json` + the verbatim 1.20.3 IANA→POSIX table.
 *  - **AP+STA simultaneous mode** (alpha.6.29 / Phase 6.14.X step 2) —
 *    operator-toggled, NOT a fail-state fallback. SSID `Greenhouse-XXYY`
 *    (last 2 bytes of MAC); PSK from NVS `wifi/ap_psk` or factory default
 *    `0123456789`; enable flag NVS `wifi/ap_enable` polled every
 *    `NET_POLL_MS`; auto-shutdown after `cfg.ap_timeout_min` minutes
 *    (0 = never). The earlier alpha.6.14 header called this "AP fallback"
 *    which was a misreading — 1.20.3 ran AP alongside STA whenever the
 *    operator enabled it (typically for first-time WiFi setup or a wrong-
 *    credentials recovery path; the operator joins the AP, opens the GUI,
 *    sets the real SSID/PSK, then turns the AP off again).
 *
 * **Still deferred to 2.0.1 / 2.1.x:**
 *  - **Exponential backoff state machine** (2 → 4 → 8 → 16 → 32 → 60 s).
 *    esp_wifi's internal reconnect retries at a fixed cadence; sufficient
 *    for the soak.
 *  - **Periodic 24 h NTP resync.** The DS1307 RTC is precise enough for
 *    multi-day operation; the SD-card daily logs and T4 RTC-readback every
 *    minute provide enough monitoring. Add periodic resync if Phase 7
 *    soak shows drift.
 *
 * **Dependencies in place**:
 *  - `wifi_tickle_run()` (alpha.3.2) called from main.cpp BEFORE
 *    T10 spawns, so esp_wifi is already initialised + connected + SNTP
 *    synced by the time T10 starts its main loop.
 *  - `Q5` queue (depth 1, xQueueOverwrite) created by `system_globals_init`.
 *  - `task_t4` handle populated by alpha.6.7 spawn.
 *  - `task_t10` handle declared in `system_globals.cpp`, populated by
 *    main.cpp's spawn block (named app_main_stub.cpp through alpha.6.22).
 *
 * @author  Greenhouse Controller project
 */

#include <stdlib.h>   /* atof (alpha.6.28 — geo sync) */
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"                      /* alpha.6.29 — esp_read_mac for AP SSID */
#include "esp_http_client.h"   /* alpha.6.28 — ip-api.com lookup */
#include "esp_sntp.h"                     /* a.6.33 — periodic 24 h NTP resync */
#include "esp_timer.h"                    /* a.6.33 — esp_timer_get_time for resync cadence */
#include "lwip/ip4_addr.h"

#include "network_manager.h"
#include "../types/app_types.h"           /* Q4, Q5, net_status_t, task_t4 */
#include "../data_manager/data_manager.h" /* DM_NOTIFY_NTP_SYNCED, dm_cfg_snapshot */
#include "../event_logger/event_logger.h" /* a.6.33 — log_post + log_event_t */
#include "nvs_config.h"                   /* alpha.6.28 — NVS_NS_SYSTEM, NVS_NS_WIFI, nvs_cfg_set_str */
#include "tz_table.h"                     /* alpha.6.28 — iana_to_posix() */

static const char *TAG = "T10_NET";

/** AP-mode latch (alpha.6.29). Forward-declared here because snapshot_state
 *  below reads it. The rest of the AP module-level state + helper functions
 *  live in their own block further down. */
static bool s_ap_active = false;

/** Main-loop tick (ms). Mirrors 1.20.3's NET_POLL_MS for behavioural parity. */
#define NET_POLL_MS  5000u

/** Plausibility threshold for "NTP-synced wall clock" (2023-11-14). */
#define NTP_MIN_EPOCH  ((time_t)1700000000)

/* a.6.33 — periodic 24 h NTP resync.
 *
 * Tracks the timestamp (in esp_timer_get_time microseconds) of the last
 * successful NTP sync — whether boot-time via wifi_tickle, or periodic via
 * run_ntp_resync() below. The check fires inside the main loop: when STA
 * is connected and the elapsed since last sync exceeds NTP_RESYNC_INTERVAL,
 * a single SNTP cycle is kicked off. Geo is NOT re-fetched (location is
 * stable; matches 1.20.3).
 *
 * NTP_RESYNC_INTERVAL_S = 86400 (24 h) for production. For verification
 * builds the symbol may be redefined at compile time via -D.
 */
#ifndef NTP_RESYNC_INTERVAL_S
#define NTP_RESYNC_INTERVAL_S  86400u
#endif
#define NTP_RESYNC_INTERVAL_US ((uint64_t)NTP_RESYNC_INTERVAL_S * 1000000ULL)

static int64_t s_last_ntp_sync_us = 0;   /* 0 = never synced this boot */

/** Post a single LOG_SYSTEM event to Q3 (T9 drains to SD CSV).
 *  a.6.33 — used by start_ap / stop_ap / do_geo_sync to log audit events
 *  with value_a=3 (AP, value_b=1 start / 0 stop) and value_a=4 (geo,
 *  value_b=1 success). Matches the 1.20.3 LOG_SYSTEM value_a encoding
 *  documented in event_logger.h. */
static void log_sys(int16_t value_a, int16_t value_b)
{
    log_event_t ev = {};
    ev.timestamp  = (uint32_t)time(NULL);
    ev.event_type = (uint8_t)LOG_SYSTEM;
    ev.initiator  = (uint8_t)LOG_BY_SYSTEM;
    ev.value_a    = value_a;
    ev.value_b    = value_b;
    log_post(&ev);
}

/**
 * @brief Build a `net_status_t` snapshot from the current esp_wifi/esp_netif state.
 *
 * client_connected = `esp_wifi_sta_get_ap_info` succeeds.
 * ap_active        = always false in this minimal T10 (AP support deferred).
 * ntp_synced       = `time(NULL) > NTP_MIN_EPOCH` (wall clock is plausible).
 * ip_str           = STA netif IP as dotted-decimal, or "" if not up.
 */
static void snapshot_state(net_status_t *out)
{
    memset(out, 0, sizeof(*out));

    /* STA connection state. */
    wifi_ap_record_t ap = {};
    out->client_connected = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);

    /* AP state — alpha.6.29: reflects start_ap/stop_ap. The module-level
     * latch (`s_ap_active`) is single-task-owned by T10 so a direct read is
     * safe; no need to query esp_wifi_get_mode here. */
    out->ap_active = s_ap_active;

    /* NTP sync state. */
    out->ntp_synced = (time(NULL) > NTP_MIN_EPOCH);

    /* IP address — query the default STA netif. */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL) {
        esp_netif_ip_info_t ip = {};
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK &&
            ip.ip.addr != 0) {
            snprintf(out->ip_str, sizeof(out->ip_str), IPSTR, IP2STR(&ip.ip));
        }
    }
}

/**
 * @brief Compare two net_status_t snapshots for material equality.
 *
 * "Material" excludes flag-bit jitter that doesn't matter to T8's LCD.
 * In practice the entire struct is compared since every field drives
 * something visible.
 */
static bool snapshots_equal(const net_status_t *a, const net_status_t *b)
{
    return a->client_connected == b->client_connected
        && a->ap_active        == b->ap_active
        && a->ntp_synced       == b->ntp_synced
        && (strncmp(a->ip_str, b->ip_str, sizeof(a->ip_str)) == 0);
}

/* ============================================================
 * IP geolocation + timezone sync (alpha.6.28 — Phase 6.14.X)
 *
 * Calls http://ip-api.com/json once after the first NTP sync to obtain
 * lat/lon and IANA timezone from the device's public IP. On success:
 *   - lat_deg/lat_frac and lon_deg/lon_frac are pushed to Q4 (T4 writes
 *     them to NVS and recalculates today's sunrise/sunset).
 *   - tz_str is written directly to NVS_NS_SYSTEM via nvs_cfg_set_str
 *     and applied via setenv/tzset so localtime_r returns the local
 *     wall-clock time immediately.
 *
 * Plain HTTP (not HTTPS) — ip-api.com's free tier doesn't offer TLS.
 * Data is non-sensitive (just lat/lon for sunrise math); the risk of
 * a man-in-the-middle returning bogus coordinates is operationally
 * minor (worst case: sunrise calc drifts by a few minutes).
 *
 * Runs at most once per boot — `s_geo_done` latches after the first
 * successful response.
 * ============================================================ */

static bool s_geo_done = false;

/** HTTP response accumulator — captured by the event handler below. */
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} geo_resp_t;

/** esp_http_client event callback — appends body chunks into a geo_resp_t. */
static esp_err_t geo_http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data != NULL) {
        geo_resp_t *r = (geo_resp_t *)evt->user_data;
        if (r->buf == NULL || r->len >= r->cap - 1) return ESP_OK;
        size_t avail = r->cap - 1 - r->len;
        size_t take  = (size_t)evt->data_len < avail ? (size_t)evt->data_len : avail;
        memcpy(r->buf + r->len, evt->data, take);
        r->len += take;
        r->buf[r->len] = '\0';
    }
    return ESP_OK;
}

/** Convert a float coordinate to integer degrees + millidegree fraction.
 *  Example: 52.3676 → deg=52, frac=368  (frac = round(fractional * 1000)).
 *  Negative coordinates: deg is negative, frac is always non-negative. */
static void float_to_deg_frac(float val, int32_t *deg, int32_t *frac)
{
    bool neg = (val < 0.0f);
    if (neg) val = -val;
    *deg  = (int32_t)val;
    *frac = (int32_t)((val - (float)*deg) * 1000.0f + 0.5f);
    if (*frac >= 1000) { *deg += 1; *frac -= 1000; }   /* carry */
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
 *  T4 writes to NVS and updates the in-RAM shadow + recalcs sunrise. */
static void post_q4(const char *ns, const char *key, int32_t value)
{
    config_update_t upd = {};
    /* strncpy then explicit NUL — see alpha.6.18 build-trap notes for why
     * snprintf into a 16-byte field is unsafe with -Werror=format-truncation. */
    strncpy(upd.ns,  ns,  sizeof(upd.ns)  - 1);
    strncpy(upd.key, key, sizeof(upd.key) - 1);
    upd.value = value;
    (void)xQueueSend(Q4, &upd, pdMS_TO_TICKS(200));
}

/** One-shot geo sync via ip-api.com. Caller gates on `s_geo_done`. */
static void do_geo_sync(void)
{
    ESP_LOGI(TAG, "[T10] Geo sync starting via ip-api.com");

    char body[256] = {0};
    geo_resp_t resp = { .buf = body, .cap = sizeof(body), .len = 0 };

    esp_http_client_config_t cfg = {};
    cfg.url            = "http://ip-api.com/json?fields=status,lat,lon,timezone";
    cfg.method         = HTTP_METHOD_GET;
    cfg.timeout_ms     = 5000;
    cfg.event_handler  = geo_http_event_cb;
    cfg.user_data      = &resp;
    cfg.transport_type = HTTP_TRANSPORT_OVER_TCP;   /* HTTP, not HTTPS */
    cfg.buffer_size    = 512;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGW(TAG, "[T10] geo_sync: esp_http_client_init failed");
        return;
    }

    esp_err_t err   = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status_code != 200) {
        ESP_LOGW(TAG, "[T10] geo_sync HTTP failed: err=%s status=%d",
                 esp_err_to_name(err), status_code);
        return;
    }

    float lat = 0.0f, lon = 0.0f;
    char  iana_tz[64] = {};
    if (!parse_geo_response(body, &lat, &lon, iana_tz, sizeof(iana_tz))) {
        ESP_LOGW(TAG, "[T10] geo_sync: failed to parse response: %s", body);
        return;
    }
    ESP_LOGI(TAG, "[T10] geo_sync: lat=%.4f lon=%.4f timezone=\"%s\"",
             (double)lat, (double)lon, iana_tz);

    int32_t lat_deg, lat_frac, lon_deg, lon_frac;
    float_to_deg_frac(lat, &lat_deg, &lat_frac);
    float_to_deg_frac(lon, &lon_deg, &lon_frac);

    /* Update lat/lon via Q4 — T4 writes NVS + refreshes the cfg shadow +
     * recalculates today's sunrise/sunset. */
    post_q4(NVS_NS_SYSTEM, "lat_deg",  lat_deg);
    post_q4(NVS_NS_SYSTEM, "lat_frac", lat_frac);
    post_q4(NVS_NS_SYSTEM, "lon_deg",  lon_deg);
    post_q4(NVS_NS_SYSTEM, "lon_frac", lon_frac);

    /* a.6.33 — audit event: value_a=4 (geo), value_b=1 (success). */
    log_sys(4, 1);

    /* IANA → POSIX TZ. Falls back to "TZ unchanged" if the zone is not in
     * the table — better than overwriting NVS with garbage. */
    const char *posix_tz = iana_to_posix(iana_tz);
    if (posix_tz) {
        (void)nvs_cfg_set_str(NVS_NS_SYSTEM, "tz_str", posix_tz);
        setenv("TZ", posix_tz, 1);
        tzset();
        ESP_LOGI(TAG, "[T10] TZ applied: \"%s\" (IANA=\"%s\")", posix_tz, iana_tz);
    } else {
        ESP_LOGW(TAG, "[T10] geo_sync: IANA zone \"%s\" not in table — TZ unchanged",
                 iana_tz);
    }

    s_geo_done = true;
}

/* ============================================================
 * AP mode (alpha.6.29 — Phase 6.14.X step 2)
 *
 * Operator-toggled soft-AP that runs alongside the STA mode (WIFI_MODE_APSTA).
 * Used for first-time WiFi setup and for credential-recovery: the operator
 * joins `Greenhouse-XXYY` (last 2 bytes of MAC), opens http://192.168.4.1,
 * and writes the real SSID/PSK through the web GUI.
 *
 * Lifecycle:
 *   - At boot: read NVS `wifi/ap_psk` (factory default "0123456789" if unset),
 *     compute SSID from MAC, do NOT start the AP yet.
 *   - Each poll (NET_POLL_MS = 5 s): read NVS `wifi/ap_enable`. On the
 *     0→1 edge call start_ap(); on the 1→0 edge call stop_ap().
 *   - While active: check uptime against `cfg.ap_timeout_min`. On expiry
 *     call stop_ap() AND clear `wifi/ap_enable` so a stale flag in NVS
 *     doesn't restart the AP on the next tick.
 *
 * The operator enables/disables via T8 (LCD menu) or T11 (web GUI), both of
 * which write the flag through Q4 → T4 → NVS. T10 polls NVS directly here
 * to keep the state machine self-contained — no cross-task notify needed.
 *
 * AP_PSK_DEFAULT is identical to 1.20.3 (`0123456789`) so the operator
 * manual instructions still apply. NEVER an open AP — WPA2 requires the
 * raw key, and an unsecured AP would expose the GUI's admin functions to
 * anyone in radio range.
 * ============================================================ */

#define AP_CHANNEL          1u
#define AP_MAX_CONN         4u
#define AP_PSK_DEFAULT      "0123456789"

static esp_netif_t  *s_ap_netif      = NULL;
/* s_ap_active is forward-declared near the top of the file (snapshot_state
 * needs it). */
static int32_t       s_ap_enable_nvs = -1;     /* sentinel: first poll always logs a transition */
static TickType_t    s_ap_started    = 0;
static char          s_ap_ssid[20]   = {};
static char          s_ap_psk[64]    = {};

/** Reload AP SSID + PSK from NVS. Called once at task start AND on every
 *  start_ap() invocation so admin changes to NVS `wifi/ap_ssid` /
 *  `wifi/ap_psk` take effect on the next enable cycle without a reboot. */
static void load_ap_credentials(void)
{
    /* SSID source order: NVS `wifi/ap_ssid` (admin override) → MAC-derived
     * `Greenhouse-XXYY` default. The MAC-derived default ensures every
     * fresh unit has a unique SSID without the admin needing to set one. */
    char ssid_buf[sizeof(s_ap_ssid)] = {0};
    nvs_cfg_status_t st = nvs_cfg_get_str(NVS_NS_WIFI, "ap_ssid",
                                          ssid_buf, sizeof(ssid_buf));
    if (st == NVS_CFG_OK && ssid_buf[0] != '\0') {
        strncpy(s_ap_ssid, ssid_buf, sizeof(s_ap_ssid) - 1);
    } else {
        uint8_t mac[6] = {};
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
            snprintf(s_ap_ssid, sizeof(s_ap_ssid), "Greenhouse-%02X%02X",
                     mac[4], mac[5]);
        } else {
            snprintf(s_ap_ssid, sizeof(s_ap_ssid), "Greenhouse-0000");
            ESP_LOGW(TAG, "[T10] esp_read_mac failed — AP SSID fallback");
        }
    }

    /* PSK source order: NVS `wifi/ap_psk` (admin override) → AP_PSK_DEFAULT.
     * Never an open AP (WPA2 requires the raw key; an unsecured AP would
     * expose the admin GUI to anyone in radio range). */
    char psk_buf[sizeof(s_ap_psk)] = {0};
    st = nvs_cfg_get_str(NVS_NS_WIFI, "ap_psk", psk_buf, sizeof(psk_buf));
    if (st == NVS_CFG_OK && psk_buf[0] != '\0') {
        strncpy(s_ap_psk, psk_buf, sizeof(s_ap_psk) - 1);
    } else {
        strncpy(s_ap_psk, AP_PSK_DEFAULT, sizeof(s_ap_psk) - 1);
    }
    s_ap_ssid[sizeof(s_ap_ssid) - 1] = '\0';
    s_ap_psk[sizeof(s_ap_psk)  - 1] = '\0';
}

/** One-shot AP init: load SSID + PSK from NVS. Called once at task start;
 *  no radio side effects. */
static void ap_init(void)
{
    load_ap_credentials();
    ESP_LOGI(TAG, "[T10] AP config ready: SSID=\"%s\" PSK=(%u chars)",
             s_ap_ssid, (unsigned)strlen(s_ap_psk));
}

/** Start the soft-AP. Idempotent. Reloads SSID/PSK from NVS each call so
 *  admin can change them via the GUI and re-enable AP without a reboot. */
static void start_ap(void)
{
    if (s_ap_active) return;

    /* Reload AP creds from NVS so admin GUI changes take effect on next
     * enable without a reboot. The previous values are kept on a failed
     * NVS read (load_ap_credentials() falls back to MAC + default). */
    load_ap_credentials();

    /* Default AP netif — idempotent: returns the existing handle if a prior
     * start_ap already created it. */
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) {
            ESP_LOGE(TAG, "[T10] esp_netif_create_default_wifi_ap returned NULL");
            return;
        }
    }

    /* Configure AP. Authmode WPA2-PSK; min PSK length is 8 chars (enforced
     * by esp_wifi_set_config). max_connection=4 matches 1.20.3. */
    wifi_config_t ap_cfg = {};
    strncpy((char *)ap_cfg.ap.ssid,     s_ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    strncpy((char *)ap_cfg.ap.password, s_ap_psk,  sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.ssid_len       = (uint8_t)strlen(s_ap_ssid);
    ap_cfg.ap.channel        = AP_CHANNEL;
    ap_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.max_connection = AP_MAX_CONN;
    ap_cfg.ap.beacon_interval = 100;

    /* Switch mode to APSTA. esp_wifi_set_mode is safe while the radio is
     * running — STA stays connected. */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[T10] esp_wifi_set_mode(APSTA) failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[T10] esp_wifi_set_config(AP) failed: %s", esp_err_to_name(err));
        /* Roll back to STA-only to leave a clean state. */
        (void)esp_wifi_set_mode(WIFI_MODE_STA);
        return;
    }

    /* esp_netif_create_default_wifi_ap auto-starts DHCP on the AP netif
     * with 192.168.4.0/24 — matches the operator manual instructions
     * ("open http://192.168.4.1"). */
    s_ap_active  = true;
    s_ap_started = xTaskGetTickCount();
    ESP_LOGI(TAG, "[T10] AP started: SSID=\"%s\" (channel %u, max %u clients)",
             s_ap_ssid, (unsigned)AP_CHANNEL, (unsigned)AP_MAX_CONN);
    /* a.6.33 — audit event: value_a=3 (AP), value_b=1 (started). */
    log_sys(3, 1);
}

/** Stop the soft-AP. Returns mode to STA-only. Idempotent. */
static void stop_ap(void)
{
    if (!s_ap_active) return;

    /* Returning to STA-only also tears down the AP netif's DHCP server.
     * The netif handle is kept for the next start_ap (cheap to reuse). */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[T10] esp_wifi_set_mode(STA) on AP stop failed: %s",
                 esp_err_to_name(err));
    }

    s_ap_active = false;
    ESP_LOGI(TAG, "[T10] AP stopped");
    /* a.6.33 — audit event: value_a=3 (AP), value_b=0 (stopped). */
    log_sys(3, 0);
}

/** Read NVS `wifi/ap_enable` and start/stop on edge; enforce auto-shutdown.
 *
 * alpha.6.31 — admin-only, no auto-enable. The AP is **only** enabled when
 * the admin writes `wifi/ap_enable=1` to NVS via the web GUI (security
 * policy: auto-enabling an open broadcast on credential loss would expose
 * an unconfigured greenhouse to anyone in radio range). After the
 * `cfg.ap_timeout_min` minutes timeout, T10 clears the flag in NVS so the
 * AP doesn't restart on the next tick.
 */
static void poll_ap(void)
{
    int32_t ap_en = 0;
    nvs_cfg_get_i32_or_default(NVS_NS_WIFI, "ap_enable", 0, &ap_en);

    if (ap_en != s_ap_enable_nvs) {
        s_ap_enable_nvs = ap_en;
        if (ap_en) start_ap();
        else       stop_ap();
    }

    /* Auto-shutdown after cfg.ap_timeout_min minutes. 0 = never shut down
     * (the operator must toggle the flag manually). */
    if (s_ap_active) {
        cfg_shadow_t cfg = {};
        dm_cfg_snapshot(&cfg);
        if (cfg.ap_timeout_min > 0) {
            uint32_t elapsed_ms = (uint32_t)(
                (xTaskGetTickCount() - s_ap_started) * portTICK_PERIOD_MS);
            uint32_t limit_ms = (uint32_t)cfg.ap_timeout_min * 60u * 1000u;
            if (elapsed_ms >= limit_ms) {
                ESP_LOGI(TAG, "[T10] AP auto-shutdown after %ld min",
                         (long)cfg.ap_timeout_min);
                /* Also clear the NVS flag so the next poll doesn't restart it.
                 * Write through Q4 so T4's cfg shadow is also updated. */
                stop_ap();
                post_q4(NVS_NS_WIFI, "ap_enable", 0);
                s_ap_enable_nvs = 0;
            }
        }
    }
}

/* ============================================================
 * Periodic 24 h NTP resync (a.6.33 — Phase T10 maturation)
 *
 * The DS1307 RTC is precise enough for multi-day operation, but slow drift
 * accumulates. The 1.20.3 design called for a once-per-24 h SNTP refresh
 * to keep the wall clock aligned. Called only when STA is connected and
 * the elapsed since last sync exceeds NTP_RESYNC_INTERVAL_S.
 *
 * Implementation notes:
 *   - esp_sntp_* directly, NOT wifi_tickle_run (which is a boot-time helper
 *     that re-initializes the event loop + netif).
 *   - esp_sntp_setoperatingmode / setservername are idempotent — they only
 *     change state if the new value differs. Safe to call on every resync.
 *   - esp_sntp_init() may be called repeatedly; second-and-later calls
 *     reinitialize the SNTP module.
 *   - Wait up to 10 s for a plausible epoch; on timeout, leave
 *     s_last_ntp_sync_us unchanged so the next iteration retries.
 *   - After success, re-apply cfg.tz_str (esp_sntp resets TZ to UTC) and
 *     notify T4 to re-write the DS1307.
 *   - Geo is NOT re-fetched (location is stable).
 * ============================================================ */

static void run_ntp_resync(void)
{
    ESP_LOGI(TAG, "[T10] Starting periodic NTP resync (24 h cadence)");

    /* esp_sntp_get_sync_status returns IN_PROGRESS while a previous sync is
     * still running. Avoid double-kicking. */
    sntp_sync_status_t st = esp_sntp_get_sync_status();
    if (st == SNTP_SYNC_STATUS_IN_PROGRESS) {
        ESP_LOGI(TAG, "[T10] NTP resync: another sync already in progress — skipping");
        return;
    }

    /* Setup. esp_sntp_init() is repeatable; calling it again kicks a new
     * query against the configured server. */
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    /* Wait up to 10 s for a plausible epoch. Note: we don't check for an
     * absolute time delta, just for the system clock to be > NTP_MIN_EPOCH
     * (which it already is from the boot sync — that's why we got here).
     * A fresh sync simply updates the clock with sub-second accuracy. */
    const int wait_steps = 20;   /* 20 × 500 ms = 10 s */
    bool ok = false;
    for (int i = 0; i < wait_steps; i++) {
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            ok = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (!ok) {
        ESP_LOGW(TAG, "[T10] NTP resync: timed out after 10 s — will retry next cycle");
        /* a.6.35.3 — emit the parser-documented NTP timeout audit row. */
        log_sys(2, 0);
        return;
    }

    /* Re-apply persisted POSIX TZ. esp_sntp resets TZ to UTC implicitly;
     * if cfg.tz_str is unset, leave whatever the prior setenv set. */
    cfg_shadow_t cfg = {};
    dm_cfg_snapshot(&cfg);
    if (cfg.tz_str[0] != '\0') {
        const char *cur_tz = getenv("TZ");
        if (cur_tz == NULL || strcmp(cur_tz, cfg.tz_str) != 0) {
            setenv("TZ", cfg.tz_str, 1);
            tzset();
            ESP_LOGI(TAG, "[T10] NTP resync: TZ re-applied (\"%s\")", cfg.tz_str);
        }
    }

    s_last_ntp_sync_us = esp_timer_get_time();
    ESP_LOGI(TAG, "[T10] NTP resync complete");

    /* Notify T4 so it re-writes the DS1307 RTC with the freshly-synced time.
     * Idempotent — DS1307 just gets refreshed with the same-to-the-second
     * value. T4 takes MX1 briefly. */
    if (task_t4 != NULL) {
        xTaskNotify(task_t4, DM_NOTIFY_NTP_SYNCED, eSetBits);
    }
}

void task_network_manager(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "[T10] task alive (geo sync since alpha.6.28; AP since alpha.6.29)");

    /* Brief settling delay so wifi_tickle's last log lines flush. The
     * esp_wifi + esp_netif state is queryable immediately, but logging
     * order is nicer if T10's first Q5 post comes after wifi_tickle's
     * "SNTP synced after N ms" line. */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* alpha.6.29 — AP one-shot init: compute SSID from MAC, read PSK from
     * NVS. No radio side effects; start_ap() is gated on `wifi/ap_enable`
     * in poll_ap() below. */
    ap_init();

    /* Initial snapshot + Q5 post. */
    net_status_t prev = {};
    snapshot_state(&prev);
    if (xQueueOverwrite(Q5, &prev) == pdTRUE) {
        ESP_LOGI(TAG, "[T10] initial Q5 post: client=%d ap=%d ntp=%d ip=\"%s\"",
                 (int)prev.client_connected, (int)prev.ap_active,
                 (int)prev.ntp_synced, prev.ip_str);
    } else {
        ESP_LOGW(TAG, "[T10] initial Q5 overwrite failed");
    }

    /* If NTP synced at boot (wifi_tickle's job), notify T4 so it can write
     * the post-SNTP system time back to the DS1307 via the TN4 path. T4's
     * dm_get_unix_time accessor is called immediately by event_logger and
     * sensor_poll; we want T4's RTC writeback to happen ASAP. */
    if (prev.ntp_synced && task_t4 != NULL) {
        xTaskNotify(task_t4, DM_NOTIFY_NTP_SYNCED, eSetBits);
        ESP_LOGI(TAG, "[T10] TN4 sent to T4 (DM_NOTIFY_NTP_SYNCED)");
        /* a.6.33 — seed the resync timer at boot so the 24 h cadence is
         * measured from the boot-time sync, not from T10 task start. */
        s_last_ntp_sync_us = esp_timer_get_time();
    }

    /* a.6.35.3 — emit boot-time STA / NTP snapshot rows.
     *
     * The main-loop transition handler (below) only fires log_sys on
     * snapshots_equal-detected edges. Because `prev` is seeded from the
     * post-boot state above, the very first iteration sees no edge and
     * never emits the "STA up" / "NTP synced" rows the logparser expects
     * (value_a=1, value_a=2). Emit them once here so every boot's CSV
     * has a definitive record of the network state the firmware came up
     * with — matches the way BOOT (a=5), Unit ID (a=11) and T2 boot-cal
     * (a=10) rows are all stamped once per boot. */
    log_sys(1, prev.client_connected ? 1 : 0);
    log_sys(2, prev.ntp_synced       ? 1 : 0);

    /* alpha.6.28 — one-shot IP geolocation + timezone sync. Gated on NTP
     * being synced (so DNS resolution + HTTP have a working network) and
     * latched after the first successful response. */
    if (prev.client_connected && prev.ntp_synced && !s_geo_done) {
        do_geo_sync();
    }

    /* Main loop — periodic state polling. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(NET_POLL_MS));

        /* alpha.6.29 — operator-toggle AP. Reads NVS wifi/ap_enable; starts
         * or stops the soft-AP on the 0↔1 edge. Also enforces the
         * cfg.ap_timeout_min auto-shutdown. */
        poll_ap();

        /* a.6.33 — periodic 24 h NTP resync. Gated on STA connected AND
         * a prior successful sync (so we have a non-zero last-sync timestamp
         * to compare against). The check is cheap; the actual resync work
         * only runs when the interval has elapsed. */
        if (s_last_ntp_sync_us != 0 && prev.client_connected) {
            int64_t elapsed_us = esp_timer_get_time() - s_last_ntp_sync_us;
            if ((uint64_t)elapsed_us >= NTP_RESYNC_INTERVAL_US) {
                run_ntp_resync();
                /* run_ntp_resync updates s_last_ntp_sync_us on success;
                 * on failure leaves it unchanged so we retry next cycle. */
            }
        }

        net_status_t cur = {};
        snapshot_state(&cur);

        if (!snapshots_equal(&prev, &cur)) {
            ESP_LOGI(TAG, "[T10] state changed: client=%d ap=%d ntp=%d ip=\"%s\"",
                     (int)cur.client_connected, (int)cur.ap_active,
                     (int)cur.ntp_synced, cur.ip_str);
            (void)xQueueOverwrite(Q5, &cur);

            /* a.6.35.3 — emit LOG_SYSTEM audit events for STA and NTP state
             * transitions. These were documented in event_logger.h and
             * logparser.md (value_a=1 STA, value_a=2 NTP) but never actually
             * produced by the firmware — the parser would never see them.
             * Edge-triggered (only on transitions) to keep the CSV signal-
             * to-noise high; AP transitions already get the same treatment
             * via start_ap()/stop_ap() so this just fills the remaining
             * two documented slots. */
            if (cur.client_connected != prev.client_connected) {
                log_sys(1, cur.client_connected ? 1 : 0);
            }
            if (cur.ntp_synced != prev.ntp_synced) {
                log_sys(2, cur.ntp_synced ? 1 : 0);
            }

            /* If NTP just transitioned to synced (e.g. after a reconnect
             * that included a fresh SNTP run), notify T4 again. The TN4
             * path in T4 is idempotent — multiple notifies in the same
             * boot just refresh the RTC. */
            if (cur.ntp_synced && !prev.ntp_synced && task_t4 != NULL) {
                xTaskNotify(task_t4, DM_NOTIFY_NTP_SYNCED, eSetBits);
                ESP_LOGI(TAG, "[T10] TN4 re-sent on NTP-synced transition");
                /* a.6.33 — also (re)seed the resync timer on an NTP-up edge,
                 * so the 24 h cadence is measured from when the wall clock
                 * actually became plausible. */
                s_last_ntp_sync_us = esp_timer_get_time();
            }

            prev = cur;
        }

        /* alpha.6.28 — retry geo sync on subsequent ticks if the initial
         * post-boot attempt was skipped (NTP not synced yet) or failed
         * (DNS hiccup, ip-api.com rate-limit etc). The `s_geo_done` latch
         * is set only on a fully parsed response, so failures retry until
         * we get a clean answer. */
        if (!s_geo_done && prev.client_connected && prev.ntp_synced) {
            do_geo_sync();
        }
    }
}
