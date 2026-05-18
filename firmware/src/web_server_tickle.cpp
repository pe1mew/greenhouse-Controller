/**
 * @file web_server_tickle.cpp
 * @brief Phase-5 ESP-IDF web server tickle implementation.
 *
 * See web_server_tickle.h for rationale. This file uses direct esp_http_server
 * — NO ESPAsyncWebServer, NO AsyncTCP.
 *
 * Architecture:
 *   - One httpd_handle_t (s_server) kept module-static so handlers can
 *     reference it if needed (e.g. for shutdown or list-clients).
 *   - Three URI handlers registered at startup:
 *       GET /            — operator-facing HTML
 *       GET /api/status  — machine-readable text/plain key=value snapshot
 *       GET /api/info    — firmware identity
 *   - All handlers are stateless and reentrant. Each call samples free
 *     heap + RTC time + uptime + IP from globals and renders into a
 *     stack buffer. No malloc, no globals mutated.
 *
 * The server runs in its own task (default stack 4 KB, priority 5).
 * Reading firmware identity from FIRMWARE_VERSION macro means the HTML
 * always shows the current build — useful for the operator to confirm
 * which firmware is responding.
 *
 * @author Greenhouse Controller project
 */

#include "web_server_tickle.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_chip_info.h"
#include "esp_mac.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* LIB-3 wrapper from drivers/DS1307_RTC (migrated alpha.2.9) — pulls the
 * real wall-clock from the battery-backed RTC for the status page. */
#include "ds1307_rtc.h"

static const char *TAG = "T-WEB";

#ifndef FIRMWARE_VERSION
#  define FIRMWARE_VERSION "unstamped"
#endif

/* Module-static server handle. Set on success, NULL otherwise. */
static httpd_handle_t s_server = NULL;

/* ===========================================================================
 * Helpers
 * =========================================================================== */

/**
 * Format the WiFi STA IP address into @p out (caller-provided, ≥ 16 bytes).
 * Returns true on success. If the netif isn't up yet or get_ip_info fails,
 * writes "0.0.0.0" and returns false.
 */
static bool get_sta_ip_str(char *out, size_t out_len)
{
    if (out == NULL || out_len < 16) return false;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        snprintf(out, out_len, "0.0.0.0");
        return false;
    }
    esp_netif_ip_info_t ip = {};
    esp_err_t err = esp_netif_get_ip_info(netif, &ip);
    if (err != ESP_OK) {
        snprintf(out, out_len, "0.0.0.0");
        return false;
    }
    snprintf(out, out_len, IPSTR, IP2STR(&ip.ip));
    return true;
}

/**
 * Read the live wall-clock from the DS1307 (LIB-3). Returns true on
 * success. The string is formatted as "YYYY-MM-DD HH:MM:SS" (19 chars + NUL).
 */
static bool get_rtc_str(char *out, size_t out_len)
{
    if (out == NULL || out_len < 20) return false;
    rtc_datetime_t now = {};
    rtc_status_t st = rtc_get_time(&now);
    if (st != RTC_OK) {
        snprintf(out, out_len, "----- ---- -- --:--:--");
        return false;
    }
    snprintf(out, out_len, "%04u-%02u-%02u %02u:%02u:%02u",
             (unsigned)now.year, (unsigned)now.month, (unsigned)now.day,
             (unsigned)now.hour, (unsigned)now.minute, (unsigned)now.second);
    return true;
}

/* ===========================================================================
 * URI handlers
 * =========================================================================== */

/**
 * GET / — operator-facing HTML status page.
 *
 * Plain HTML (no external CSS/JS) so it works on any browser without our
 * web-asset bundle being uploaded yet. Auto-refreshes every 5 s via
 * meta-refresh so the operator sees live values without our WebSocket
 * push (which lifts into alpha.5.5).
 */
static esp_err_t root_handler(httpd_req_t *req)
{
    /* Sample live values into local vars first — minimises time between
     * sample and emission so the values look "consistent" to the operator. */
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    uint32_t uptime_s    = (uint32_t)((xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000UL);
    char ip_str[16] = {0};
    char rtc_str[24] = {0};
    get_sta_ip_str(ip_str, sizeof(ip_str));
    get_rtc_str(rtc_str, sizeof(rtc_str));

    /* Build the response body in a stack buffer. 1.5 KB is comfortably
     * larger than the actual rendered HTML (~700 bytes) plus the head-room
     * GCC's -Wformat-truncation static analyser wants to see.
     *
     * NB: 1024 B triggered -Werror=format-truncation here. The analyser is
     * pessimistic about the widths of `%u` (4-byte uint could in theory
     * render as "4294967295" = 10 chars) and adds those worst-case widths
     * to the static template length. The runtime check below handles real
     * truncation; the buffer bump just makes the static analyser happy. */
    char body[1536];
    int n = snprintf(body, sizeof(body),
        "<!DOCTYPE html>\n"
        "<html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta http-equiv=\"refresh\" content=\"5\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Greenhouse Controller — v%s</title>"
        "<style>"
        "body{font-family:system-ui,sans-serif;max-width:32em;margin:2em auto;padding:0 1em;background:#0d2818;color:#e0f2e0}"
        "h1{color:#7ee87e;margin:0 0 .2em 0}"
        ".sub{color:#5a8a5a;font-size:.9em;margin-bottom:1.5em}"
        "table{width:100%%;border-collapse:collapse}"
        "th{text-align:left;color:#7ee87e;font-weight:normal;padding:.4em 0;border-bottom:1px solid #2a4a2a;width:12em}"
        "td{padding:.4em 0;border-bottom:1px solid #2a4a2a;font-variant-numeric:tabular-nums}"
        "footer{margin-top:2em;color:#5a8a5a;font-size:.85em}"
        "</style></head><body>"
        "<h1>Greenhouse Controller</h1>"
        "<div class=\"sub\">v%s — ESP-IDF stub / Phase 5 web tickle</div>"
        "<table>"
        "<tr><th>Wall clock (RTC)</th><td>%s</td></tr>"
        "<tr><th>Uptime</th><td>%u s</td></tr>"
        "<tr><th>STA IP</th><td>%s</td></tr>"
        "<tr><th>Free heap (internal)</th><td>%u B</td></tr>"
        "<tr><th>Largest free block</th><td>%u B</td></tr>"
        "</table>"
        "<footer>"
        "Tickle endpoints: "
        "<code>/api/status</code> (key=value text), "
        "<code>/api/info</code> (firmware identity)"
        "<br>Page auto-refreshes every 5 s."
        "</footer>"
        "</body></html>\n",
        FIRMWARE_VERSION, FIRMWARE_VERSION, rtc_str,
        (unsigned)uptime_s, ip_str,
        (unsigned)free_internal, (unsigned)largest_block);

    if (n < 0 || (size_t)n >= sizeof(body)) {
        ESP_LOGW(TAG, "/ body truncated (n=%d)", n);
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, body, (n > 0) ? (size_t)n : 0);
}

/**
 * GET /api/status — machine-readable key=value snapshot.
 *
 * Plain text (text/plain). Same data as the HTML page but parser-friendly.
 * A future curl-based smoke test can hit this endpoint and grep for keys.
 */
static esp_err_t status_handler(httpd_req_t *req)
{
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t free_spiram   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t uptime_s    = (uint32_t)((xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000UL);
    char ip_str[16] = {0};
    char rtc_str[24] = {0};
    get_sta_ip_str(ip_str, sizeof(ip_str));
    get_rtc_str(rtc_str, sizeof(rtc_str));

    char body[512];
    int n = snprintf(body, sizeof(body),
        "fw_version=%s\n"
        "uptime_s=%u\n"
        "rtc=%s\n"
        "sta_ip=%s\n"
        "free_heap_internal=%u\n"
        "free_heap_largest=%u\n"
        "free_heap_spiram=%u\n",
        FIRMWARE_VERSION, (unsigned)uptime_s, rtc_str, ip_str,
        (unsigned)free_internal, (unsigned)largest_block,
        (unsigned)free_spiram);

    if (n < 0 || (size_t)n >= sizeof(body)) {
        ESP_LOGW(TAG, "/api/status body truncated (n=%d)", n);
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, body, (n > 0) ? (size_t)n : 0);
}

/**
 * GET /api/info — firmware identity. Returned once, doesn't change between
 * calls; cacheable.
 */
static esp_err_t info_handler(httpd_req_t *req)
{
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char body[256];
    int n = snprintf(body, sizeof(body),
        "fw_version=%s\n"
        "chip=ESP32-S3 rev v%d.%d\n"
        "cores=%d\n"
        "sta_mac=%02X:%02X:%02X:%02X:%02X:%02X\n"
        "idf_version=5.5.0\n",
        FIRMWARE_VERSION,
        chip.revision / 100, chip.revision % 100,
        chip.cores,
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, body, (n > 0) ? (size_t)n : 0);
}

/* ===========================================================================
 * URI table
 * =========================================================================== */

static const httpd_uri_t s_uri_root = {
    .uri      = "/",
    .method   = HTTP_GET,
    .handler  = root_handler,
    .user_ctx = NULL
};

static const httpd_uri_t s_uri_status = {
    .uri      = "/api/status",
    .method   = HTTP_GET,
    .handler  = status_handler,
    .user_ctx = NULL
};

static const httpd_uri_t s_uri_info = {
    .uri      = "/api/info",
    .method   = HTTP_GET,
    .handler  = info_handler,
    .user_ctx = NULL
};

/* ===========================================================================
 * Public API
 * =========================================================================== */

web_server_tickle_status_t web_server_tickle_start(void)
{
    if (s_server != NULL) {
        ESP_LOGW(TAG, "server already running");
        return WEB_SERVER_TICKLE_OK;
    }

    /* HTTPD config: HTTPD_DEFAULT_CONFIG = port 80, stack 4 KB, prio 5,
     * 7 max URI handlers (more than we need; head-room for alpha.5.1+).
     * The default max_open_sockets = 7 also matches the production
     * arduino-era server (which set 4-6 — we have more room). */
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.stack_size       = 4096;
    cfg.task_priority    = 5;
    cfg.max_uri_handlers = 8;     /* room for the 3 tickle URIs + future */
    cfg.max_open_sockets = 7;
    /* lru_purge_enable closes least-recently-used sockets when capacity
     * is hit — useful for the eventual WebSocket workload. */
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s (0x%x)",
                 esp_err_to_name(err), (unsigned)err);
        s_server = NULL;
        return WEB_SERVER_TICKLE_INIT_FAILED;
    }

    /* Register URI handlers. Failure here is fatal — we shut down the
     * server again so the caller doesn't end up with a half-initialised
     * one. */
    const httpd_uri_t *uris[] = { &s_uri_root, &s_uri_status, &s_uri_info };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        err = httpd_register_uri_handler(s_server, uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", uris[i]->uri,
                     esp_err_to_name(err));
            httpd_stop(s_server);
            s_server = NULL;
            return WEB_SERVER_TICKLE_REGISTER_FAILED;
        }
    }

    /* Log the listening URL so the operator can paste it into a browser. */
    char ip_str[16] = {0};
    bool ip_ok = get_sta_ip_str(ip_str, sizeof(ip_str));
    if (ip_ok) {
        ESP_LOGI(TAG, "HTTP server running — open http://%s/ in a browser", ip_str);
        ESP_LOGI(TAG, "  endpoints: /  /api/status  /api/info");
    } else {
        ESP_LOGW(TAG, "HTTP server running but STA IP not resolvable yet");
    }

    return WEB_SERVER_TICKLE_OK;
}
