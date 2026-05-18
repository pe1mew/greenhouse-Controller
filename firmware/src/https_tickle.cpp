/**
 * @file https_tickle.cpp
 * @brief Phase-4 ESP-IDF HTTPS client tickle implementation.
 *
 * See https_tickle.h for rationale. This file uses direct esp_http_client +
 * esp_tls — NO HTTPClient.h, NO WiFiClientSecure, NO String/Stream.
 *
 * The TLS config strategy for the gh#23 payoff:
 *   - `skip_cert_common_name_check = true` matches the production
 *     `setInsecure()` policy (the status server has a self-signed-style
 *     cert; cert pinning is out-of-scope for v2.0.0).
 *   - `buffer_size` / `buffer_size_tx` are pinned at 1024 bytes each
 *     instead of arduino's 16 KB default. The status-post payload is
 *     ~400 bytes typical; 1 KB is more than enough and saves 30 KB per
 *     connection.
 *   - `keep_alive_enable = true` so multiple requests in the same boot
 *     share one TLS session. Arduino-era HTTPClient::end() destroyed the
 *     TLS state every call — every status post paid the full handshake
 *     cost (~20 KB transient heap). IDF's reuse drops that to a single
 *     handshake per boot.
 *
 * Future tuning (alpha.4.1+, kept in scope for Phase 6):
 *   - mbedtls `max_frag_len = 1024` — caps TLS record size to fit our
 *     buffer choice; reduces peak heap during the handshake.
 *   - Single cipher suite (TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256) — cuts
 *     the cipher-negotiation memory.
 *   - Session-ticket persistence across boots (esp_tls_session_save).
 *
 * @author Greenhouse Controller project
 */

#include "https_tickle.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"   /* alpha.4.1 — IDF-bundled public CA store */

static const char *TAG = "T-HTTPS";

/**
 * HTTP event handler — receives lifecycle events from the client.
 *
 * For the tickle we mostly log. The ON_DATA path accumulates the response
 * length so a 204 (No Content) and a 200-with-body can be distinguished.
 *
 * Runs in the calling task's context (esp_http_client_perform is synchronous;
 * the event handler is invoked inline from within perform()).
 */
static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    static size_t s_response_bytes = 0;

    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGW(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            /* TCP + TLS handshake complete. Future hook point for
             * per-handshake mbedtls knobs (alpha.4.1+). */
            ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
            s_response_bytes = 0;
            break;
        case HTTP_EVENT_HEADERS_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADERS_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER %s: %s",
                     evt->header_key ? evt->header_key : "?",
                     evt->header_value ? evt->header_value : "?");
            break;
        case HTTP_EVENT_ON_DATA:
            /* Body chunk arrived. Count bytes; don't store. */
            s_response_bytes += evt->data_len;
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA +%d B (cumulative %u)",
                     evt->data_len, (unsigned)s_response_bytes);
            break;
        case HTTP_EVENT_ON_FINISH:
            /* Response fully received. Future hook point for
             * mbedtls session-ticket save (alpha.4.1+). */
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH (response body = %u bytes)",
                     (unsigned)s_response_bytes);
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT");
            break;
        default:
            break;
    }
    return ESP_OK;
}

https_tickle_status_t https_tickle_run(const char *url, https_tickle_result_t *out)
{
    /* Always write defaults into *out so the caller can read it
     * unconditionally — even on early return. */
    https_tickle_result_t local = {};
    local.status = HTTPS_TICKLE_NO_URL;
    if (out != NULL) {
        *out = local;
    }

    if (url == NULL || url[0] == '\0') {
        ESP_LOGW(TAG, "https_tickle_run: NULL/empty URL");
        return HTTPS_TICKLE_NO_URL;
    }

    /* Sample heap BEFORE init. Both quantities matter:
     *   - free heap: total heap available (lwIP / mbedtls allocations
     *                during the call are counted out of this).
     *   - largest block: the gh#23 / gh#24 signal source — under the
     *                    arduino code this dropped to 77-83 KB and stayed,
     *                    even after free heap recovered. The fragmentation
     *                    metric is what matters for long-running stability. */
    local.free_heap_before     = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    local.largest_block_before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    /* Build client config. The tickle deliberately keeps this minimal —
     * the production status_post in Phase 6 will add more fields.
     *
     * alpha.4.1 fix — IDF 5.5's esp-tls REQUIRES an explicit server-
     * verification option (skip_cert_common_name_check alone returns
     * ESP_ERR_MBEDTLS_SSL_SETUP_FAILED, error 0x8017). Using the bundled
     * CA store (esp_crt_bundle_attach) is the right answer for the
     * Google tickle endpoint — IDF ships a curated public-CA bundle
     * covering ~80 commonly-encountered roots, including the ISRG /
     * Google Trust Services chain. ~17 KB flash impact.
     *
     * The production status server uses a self-signed cert — that path
     * (CONFIG_ESP_TLS_INSECURE + CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
     * in sdkconfig.defaults) lifts into Phase 6 alongside the full
     * status_post.cpp port. */
    esp_http_client_config_t cfg = {};
    cfg.url                          = url;
    cfg.event_handler                = &http_event_cb;
    cfg.transport_type               = HTTP_TRANSPORT_OVER_SSL;
    cfg.timeout_ms                   = 10000;
    cfg.buffer_size                  = 1024;   /* arduino default was 16 KB */
    cfg.buffer_size_tx               = 1024;   /* same */
    cfg.crt_bundle_attach            = esp_crt_bundle_attach;  /* alpha.4.1: required by IDF 5.5 esp-tls */
    cfg.skip_cert_common_name_check  = false;  /* let CN verification happen; bundle covers Google */
    cfg.disable_auto_redirect        = false;  /* follow 301/302 if Google sends one */
    cfg.keep_alive_enable            = true;   /* reuse TLS session across calls */
    cfg.keep_alive_idle              = 5;      /* seconds */
    cfg.keep_alive_interval          = 5;
    cfg.keep_alive_count             = 3;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init returned NULL");
        local.status               = HTTPS_TICKLE_INIT_FAILED;
        local.free_heap_after      = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        local.largest_block_after  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        if (out) *out = local;
        return HTTPS_TICKLE_INIT_FAILED;
    }

    esp_http_client_set_method(client, HTTP_METHOD_GET);

    /* Perform — synchronous; calls event_handler inline. */
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);
    int64_t t1 = esp_timer_get_time();
    local.elapsed_ms = (t1 - t0) / 1000;

    if (err == ESP_OK) {
        local.http_status_code = esp_http_client_get_status_code(client);
        int64_t content_length = esp_http_client_get_content_length(client);
        ESP_LOGI(TAG, "perform OK: status=%d content_length=%lld elapsed=%lld ms",
                 local.http_status_code,
                 (long long)content_length,
                 (long long)local.elapsed_ms);

        if (local.http_status_code >= 200 && local.http_status_code < 300) {
            local.status = HTTPS_TICKLE_OK;
        } else {
            ESP_LOGW(TAG, "non-2xx response: %d", local.http_status_code);
            local.status = HTTPS_TICKLE_HTTP_ERROR;
        }
    } else {
        ESP_LOGW(TAG, "perform FAILED: %s (0x%x) elapsed=%lld ms",
                 esp_err_to_name(err), (unsigned)err, (long long)local.elapsed_ms);
        local.status = HTTPS_TICKLE_PERFORM_FAILED;
    }

    /* esp_http_client_cleanup releases the TLS context. With keep_alive_enable
     * the underlying socket is kept warm for the next call IF cleanup isn't
     * called — but for the tickle we want a clean lifecycle each iteration
     * so the heap-delta measurement reflects the worst case (handshake +
     * connect + transfer + teardown). The full status_post in Phase 6 will
     * keep a single client handle alive across calls. */
    esp_http_client_cleanup(client);

    /* Sample heap AFTER cleanup. Brief delay first to let any deferred
     * frees (mbedtls cleanup, lwIP socket teardown) settle. */
    vTaskDelay(pdMS_TO_TICKS(50));
    local.free_heap_after     = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    local.largest_block_after = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    int32_t free_delta    = (int32_t)local.free_heap_after - (int32_t)local.free_heap_before;
    int32_t largest_delta = (int32_t)local.largest_block_after - (int32_t)local.largest_block_before;
    ESP_LOGI(TAG, "heap delta: free %+ld B, largest %+ld B (after=%u/%u)",
             (long)free_delta, (long)largest_delta,
             (unsigned)local.free_heap_after, (unsigned)local.largest_block_after);

    if (out) *out = local;
    return local.status;
}
