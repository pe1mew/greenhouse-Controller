/**
 * @file status_post.cpp
 * @brief T14 — Status website POST task (Phase 6.N.2, minimal).
 *
 * **alpha.6.15 minimal-T14 status** (2026-05-18):
 *
 * The original 1.20.3 file (942 lines, Arduino-HTTPClient based, with the
 * persistent WiFiClientSecure + streaming SD-log upload + heap-drop
 * accumulator + circuit breaker + planned-reboot path) is archived
 * alongside this file as `status_post_1.20.3_original.cpp.archived`. The
 * migration plan §"Phase 4 — HTTPS client" describes a full rewrite using
 * esp_http_client + esp_tls + mbedtls knobs (max_frag_len, session-ticket
 * reuse, single cipher suite) as the **gh#23 fix payoff** — this is the
 * file where that work eventually lands.
 *
 * **This minimal T14 deliberately defers the gh#23 mitigations.** The goal
 * here is to remove the alpha.5 https_tickle one-shot and replace it with
 * a long-running task that posts a status JSON every cfg.status_interval_s
 * — proving the symbol layout, the spawn integration, the task lifecycle,
 * and the cfg-driven URL/interval reads.
 *
 * **Implemented:**
 *  1. `task_status_post` — long-running task. Reads `cfg.status_url` +
 *     `cfg.status_interval_s` from T4 via dm_cfg_snapshot. If url is empty
 *     ("") or interval is zero, sleeps and re-checks every 60 s. Otherwise
 *     POSTs a minimal JSON every status_interval_s seconds.
 *  2. JSON payload: `{"unit_id":"NNNN","fw_version":"X","uptime_s":N,
 *     "free_heap":N}` — ~80 bytes. Adequate for connectivity testing;
 *     full sensor + relay snapshot lands when status_json.cpp activates
 *     in a follow-up.
 *  3. esp_http_client with `crt_bundle_attach` + `keep_alive_enable=true`
 *     + 1 KB buffers (same shape as https_tickle, proven gh#23 baseline).
 *  4. Exposes all 6 symbols from status_post.h so future ui_display +
 *     web_server callers link without further force-removal stubs:
 *      - `status_post_backoff_active()` — always false (no breaker yet)
 *      - `status_post_last_str()` / `status_post_last_log_str()` —
 *        format last attempt outcome
 *      - `status_post_heartbeat()` — increments per loop tick (T15 hook)
 *      - `status_post_heap_drop_bytes()` — always 0 (no accumulator yet)
 *      - `status_post_force_teardown()` — no-op (no persistent TLS state
 *        to tear down)
 *
 * **Deferred** (preserves the gh#23 fix work for a focused follow-up):
 *  - mbedtls session-ticket reuse (`HTTP_EVENT_ON_FINISH` save +
 *    `HTTP_EVENT_ON_CONNECTED` restore).
 *  - mbedtls `max_frag_len = 1024` + single cipher suite
 *    (TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256).
 *  - Streaming SD-log upload (`SDFileChunkedStream` → `esp_http_client_open
 *    + write + fetch_headers`).
 *  - gh#24 heap-drop accumulator (record_heap_drop, signed-balance math).
 *  - gh#25 log-upload dedup latch.
 *  - Circuit breaker (10 consecutive failures → 60 s lockout).
 *  - Planned-reboot path via T15 supervisor (currently dormant).
 *
 * **Dependencies satisfied**:
 *  - cfg_shadow_t via dm_cfg_snapshot (T4 alpha.6.7 active).
 *  - esp_http_client / esp_tls available (Phase 4 dependencies in
 *    CMakeLists.txt REQUIRES already pulled in by https_tickle).
 *  - log_post (T9 alpha.6.6 active) for posting LOG_NET events.
 *
 * @author Greenhouse Controller project
 */

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "status_post.h"
#include "../types/app_types.h"
#include "../data_manager/data_manager.h"
#include "../event_logger/event_logger.h"
#include "../system_id/system_id.h"

static const char *TAG = "T14_STA";

/** Re-check interval when status_url is empty or interval==0. */
#define STATUS_IDLE_RECHECK_MS  60000u

/** Connect timeout for esp_http_client. Generous for high-latency cells. */
#define STATUS_HTTP_TIMEOUT_MS  10000u

/* ============================================================
 * Module state — exposed via accessor functions in status_post.h
 * ============================================================ */

/** Heartbeat counter — incremented at the top of each loop tick. */
static volatile uint32_t s_heartbeat = 0;

/** Cumulative heap drop bytes (always 0 in minimal T14). */
static volatile uint32_t s_heap_drop_bytes = 0;

/** Last-attempt outcome string (rendered on T8 LCD + web Web tab). */
static char s_last_str[40]     = {0};
static char s_last_log_str[40] = {0};

/* ============================================================
 * Public accessors — implementations required by status_post.h
 * ============================================================ */

bool status_post_backoff_active(void)
{
    /* Minimal T14: no breaker. The full T14 will track consecutive POST
     * failures and report `true` while the breaker is open. */
    return false;
}

uint32_t status_post_heartbeat(void)
{
    return s_heartbeat;
}

uint32_t status_post_heap_drop_bytes(void)
{
    return s_heap_drop_bytes;
}

void status_post_force_teardown(void)
{
    /* No persistent TLS state in the minimal T14 — esp_http_client_init/
     * cleanup is per-call. When session-ticket reuse lands (full T14),
     * this becomes esp_http_client_cleanup + clear the session ticket. */
}

void status_post_last_str(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) return;
    strncpy(buf, s_last_str, cap - 1);
    buf[cap - 1] = '\0';
}

void status_post_last_log_str(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) return;
    strncpy(buf, s_last_log_str, cap - 1);
    buf[cap - 1] = '\0';
}

/* ============================================================
 * HTTP event callback — minimal (no session-ticket reuse yet)
 * ============================================================ */

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGW(TAG, "[T14] HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "[T14] HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "[T14] HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "[T14] HTTP_EVENT_DISCONNECTED");
            break;
        default:
            break;
    }
    return ESP_OK;
}

/* ============================================================
 * Internal: build the minimal status JSON
 *
 * Format: {"unit_id":"NNNN","fw_version":"V","uptime_s":N,"free_heap":N}
 *
 * The full status_json.cpp builds a much richer payload (sensor snapshot,
 * relay states, EG1 flag bits, alarms array) for the web Web tab to
 * render. Minimal T14 only sends connectivity-test fields; full payload
 * lands when status_json.cpp activates (Phase 6.15.X).
 * ============================================================ */
static size_t build_min_status_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) return 0;

    const uint16_t unit_id = system_unit_id_u16();
    const uint32_t uptime_s =
        (uint32_t)(esp_timer_get_time() / 1000000ULL);
    const uint32_t free_heap = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    int n = snprintf(buf, cap,
                     "{\"unit_id\":\"%04u\","
                      "\"fw_version\":\"%s\","
                      "\"uptime_s\":%lu,"
                      "\"free_heap\":%lu}",
                     (unsigned)unit_id,
                     FIRMWARE_VERSION,
                     (unsigned long)uptime_s,
                     (unsigned long)free_heap);
    if (n < 0)               return 0;
    if ((size_t)n >= cap)    return cap - 1;
    return (size_t)n;
}

/* ============================================================
 * Internal: execute one HTTPS POST cycle
 *
 * Returns true on HTTP 2xx, false otherwise. Records s_last_str.
 * ============================================================ */
static bool do_one_post(const cfg_shadow_t *cfg)
{
    char body[160] = {0};
    const size_t body_len = build_min_status_json(body, sizeof(body));

    esp_http_client_config_t cfg_http = {};
    cfg_http.url                          = cfg->status_url;
    cfg_http.method                       = HTTP_METHOD_POST;
    cfg_http.timeout_ms                   = STATUS_HTTP_TIMEOUT_MS;
    cfg_http.event_handler                = http_event_cb;
    cfg_http.transport_type               = HTTP_TRANSPORT_OVER_SSL;
    cfg_http.crt_bundle_attach            = esp_crt_bundle_attach;
    cfg_http.keep_alive_enable            = true;
    cfg_http.buffer_size                  = 1024;
    cfg_http.buffer_size_tx               = 1024;
    cfg_http.skip_cert_common_name_check  = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg_http);
    if (client == NULL) {
        ESP_LOGW(TAG, "[T14] esp_http_client_init failed");
        snprintf(s_last_str, sizeof(s_last_str), "INIT_FAIL");
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)body_len);

    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);
    const int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
    const int status_code = esp_http_client_get_status_code(client);
    const int content_len = esp_http_client_get_content_length(client);
    esp_http_client_cleanup(client);

    /* Format the last-attempt string for T8 / web display. */
    char ts[20] = {0};
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);

    bool ok = (err == ESP_OK && status_code >= 200 && status_code < 300);
    snprintf(s_last_str, sizeof(s_last_str), "%s %s",
             ok ? "OK" : "FAIL", ts);

    if (ok) {
        ESP_LOGI(TAG, "[T14] POST OK: status=%d len=%d elapsed=%lld ms (body=%u B)",
                 status_code, content_len, elapsed_ms, (unsigned)body_len);
    } else {
        ESP_LOGW(TAG, "[T14] POST FAIL: err=%s status=%d elapsed=%lld ms",
                 esp_err_to_name(err), status_code, elapsed_ms);
    }

    return ok;
}

/* ============================================================
 * Task entry point
 * ============================================================ */

void task_status_post(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "[T14] task alive (minimal T14 — see file header for deferred features)");

    /* Initial settling delay — T4 is already up but cfg may still be
     * loading from NVS. 2 s is plenty. */
    vTaskDelay(pdMS_TO_TICKS(2000));

    uint32_t last_post_ms = 0;

    for (;;) {
        s_heartbeat++;

        cfg_shadow_t cfg = {};
        dm_cfg_snapshot(&cfg);

        /* Bail-out cases: status disabled (empty URL) or interval=0. */
        if (cfg.status_url[0] == '\0' || cfg.status_interval_s <= 0) {
            ESP_LOGD(TAG, "[T14] status disabled (url=\"%s\" interval=%ld s)",
                     cfg.status_url, (long)cfg.status_interval_s);
            vTaskDelay(pdMS_TO_TICKS(STATUS_IDLE_RECHECK_MS));
            continue;
        }

        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        const uint32_t interval_ms = (uint32_t)cfg.status_interval_s * 1000u;

        if (last_post_ms == 0 || (now_ms - last_post_ms) >= interval_ms) {
            (void)do_one_post(&cfg);
            last_post_ms = now_ms;
        }

        /* Sleep 1 s and re-evaluate. Lets cfg changes (new URL, new
         * interval) take effect within 1 s of the operator writing them
         * via the web/LCD menu — same responsiveness as 1.20.3 prod. */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
