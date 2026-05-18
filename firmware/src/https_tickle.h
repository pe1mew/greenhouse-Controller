/**
 * @file https_tickle.h
 * @brief Phase-4 ESP-IDF HTTPS client tickle — IDF-native esp_http_client + esp_tls.
 *
 * Self-contained validation module for the v2.0.0 migration's Phase 4
 * (2.0.0-alpha.4). Exercises:
 *   - esp_http_client_init with HTTP_TRANSPORT_OVER_SSL
 *   - The HTTP event handler API (HTTP_EVENT_ON_CONNECTED, _ON_DATA, _ON_FINISH)
 *   - esp_http_client_perform — full request lifecycle
 *   - Heap measurement before/after each call (gh#23 / gh#24 signal)
 *
 * Replaces:
 *   - `WiFiClientSecure s_secure; HTTPClient http; http.begin(client, url);` etc.
 *   - The arduino-era `setInsecure()` / `setTimeout()` pattern
 *
 * The gh#23 payoff is the per-handshake heap pattern. Under arduino-esp32's
 * WiFiClientSecure (which wraps mbedtls but hides its config) the heap dropped
 * 20+ KB per TLS handshake and didn't recover — driving a planned reboot
 * every 5.5 to 11 hours on production. The IDF native stack lets us:
 *
 *   1. Set `keep_alive_enable = true` so multiple requests share a single TLS
 *      session (one handshake, then N requests on the same socket).
 *   2. Cap buffer_size + buffer_size_tx to 1024 bytes (arduino's default was
 *      16 KB each — most of that was unused). Smaller buffers = smaller heap
 *      footprint per connection.
 *   3. Apply mbedtls knobs via HTTP_EVENT_ON_CONNECTED — defer to alpha.4.1+.
 *
 * Alpha.4 establishes the baseline. Alpha.4.1+ will tune mbedtls further if
 * heap drops are still observed.
 *
 * Defers to Phase 6 (full status_post.cpp port):
 *   - POST + JSON body (gh#24 signed-balance heap detector)
 *   - File upload streaming (do_log_upload's chunked write pattern)
 *   - Session-ticket persistence across boots
 *   - Production status-server URL from NVS
 *   - gh#25 dedup latch and gh#26 SD-unmount-before-reset interaction
 *
 * @author Greenhouse Controller project
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Return status from a single https_tickle_run() call. */
typedef enum {
    HTTPS_TICKLE_OK              = 0, /**< 2xx response received cleanly */
    HTTPS_TICKLE_NO_URL          = 1, /**< URL param was NULL or empty */
    HTTPS_TICKLE_INIT_FAILED     = 2, /**< esp_http_client_init returned NULL */
    HTTPS_TICKLE_PERFORM_FAILED  = 3, /**< esp_http_client_perform returned !=ESP_OK */
    HTTPS_TICKLE_HTTP_ERROR      = 4, /**< Response status code was not 2xx */
} https_tickle_status_t;

/** @brief Detailed result struct populated by https_tickle_run(). */
typedef struct {
    https_tickle_status_t status;
    int      http_status_code;     /**< HTTP status (e.g. 204), 0 if request never completed */
    int64_t  elapsed_ms;           /**< Wall-clock time spent in esp_http_client_perform() */
    /* Heap-instrumentation fields (gh#23 / gh#24 signal source) */
    size_t   free_heap_before;     /**< heap_caps_get_free_size(MALLOC_CAP_INTERNAL) before perform */
    size_t   free_heap_after;      /**< Same, after perform */
    size_t   largest_block_before; /**< heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) before */
    size_t   largest_block_after;  /**< Same, after */
} https_tickle_result_t;

/**
 * @brief Run a single HTTPS GET against @p url with heap instrumentation.
 *
 * Requires WiFi STA to be up + IP obtained (the Phase-3 wifi_tickle should
 * have left this state). Uses HTTPS over TLS with:
 *   - skip_cert_common_name_check = true (matches the arduino-era setInsecure)
 *   - buffer_size / buffer_size_tx = 1024 bytes (vs arduino's 16 KB)
 *   - keep_alive_enable = true (so back-to-back calls reuse the TLS session)
 *
 * On the first call: full TLS handshake (visible in the heap delta).
 * On subsequent calls within the same session lifetime: TLS resume,
 * no handshake → small heap delta. Pre-2.0 arduino code never achieved
 * this because HTTPClient::end() tore down the TLS context every time.
 *
 * Synchronous; blocks until the response is fully received or the
 * perform-internal timeout fires.
 *
 * @param url   Full URL beginning with "https://" — caller-owned, copied
 *              internally by esp_http_client.
 * @param out   Result struct (caller-allocated, populated on return).
 * @return Status code (also written into @p out->status).
 */
https_tickle_status_t https_tickle_run(const char *url, https_tickle_result_t *out);

#ifdef __cplusplus
}
#endif
