/**
 * @file ota_client.h
 * @brief ROTA (internet-pull OTA) client — request layer.
 *
 * Implements the wire-contract request primitives of rota_tds.md §4 for the
 * T16 pull-OTA task (2.2.0):
 *   - the per-unit HMAC `X-OTA-Auth` header (§4.2), and
 *   - a certificate-pinned HTTPS GET (R-A02: the OTA server is authenticated
 *     by a pinned self-signed cert, NOT the public CA bundle).
 *
 * This module is transport only — it neither parses manifests nor touches
 * flash. Higher layers (manifest decision, download+verify, apply via T13)
 * build on these. Kept separate from ota_manager (T13 push-OTA state machine).
 *
 * @author Greenhouse Controller project
 * @since 2.2.0 (ROTA)
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Minimum buffer for the X-OTA-Auth header value.
 *
 * `<id 12>:<ts ≤11>:<nonce 16>:<mac 64>` + 3 colons + NUL = 107. 128 gives
 * margin for a longer `ts`.
 */
#define ROTA_AUTH_HDR_LEN 128u

/**
 * @brief Build the `X-OTA-Auth` header value for a request (rota_tds.md §4.2).
 *
 * Computes `<id>:<ts>:<nonce>:<mac>` where
 *   - `id`    = full WiFi-STA MAC, 12 lowercase hex (system_mac_str),
 *   - `ts`    = current Unix time (decimal),
 *   - `nonce` = 8 random bytes as 16 lowercase hex (esp_random),
 *   - `mac`   = HMAC-SHA256(secret, id + "|" + ts + "|" + nonce + "|" + request_uri),
 *              64 lowercase hex.
 *
 * The signed `request_uri` must be byte-identical to the path+query actually
 * sent (e.g. `"/manifest.php?fw=2.2.0"`), or the server rejects the HMAC.
 *
 * @param secret       Per-unit `ota_secret` (raw ASCII, non-empty).
 * @param request_uri  Path and query string exactly as sent.
 * @param out          Destination; must be ≥ ROTA_AUTH_HDR_LEN.
 * @param out_len      Capacity of @p out.
 * @return true on success; false on bad args or if wall time is not yet valid
 *         (Unix time < 2020 — caller must ensure SNTP synced first, R-C03).
 */
bool rota_build_auth_header(const char *secret, const char *request_uri,
                            char *out, size_t out_len);

/**
 * @brief Certificate-pinned HTTPS GET against the OTA server (R-A02).
 *
 * Uses esp_http_client over TLS with the caller-supplied PEM as the ONLY
 * trusted certificate (`cert_pem`), so a MITM presenting any other cert —
 * even a publicly-CA-signed one for the same host — is rejected. Attaches the
 * `X-OTA-Auth` header. The response body is collected into a caller-owned
 * heap buffer (PSRAM-preferred) capped at @p max_body.
 *
 * TLS handshakes must be serialised with T14 (R-C07); the caller holds the
 * shared TLS mutex around this call.
 *
 * @param url          Full https:// URL (path+query already appended).
 * @param request_uri  Path+query of @p url, for HMAC signing (must match).
 * @param cert_pem     Pinned server certificate, PEM, NUL-terminated.
 * @param secret       Per-unit `ota_secret` for the auth header.
 * @param max_body     Cap on collected body bytes.
 * @param out_status   [out] HTTP status code (e.g. 200, 204, 404).
 * @param out_body     [out] Heap buffer with the body (NUL-terminated); the
 *                     caller frees with free(). NULL on failure/empty.
 * @param out_len      [out] Body length in bytes (excluding the NUL).
 * @return ESP_OK if the request completed (inspect *out_status); an esp_err_t
 *         error code on transport/TLS/alloc failure.
 */
esp_err_t rota_https_get(const char *url, const char *request_uri,
                         const char *cert_pem, const char *secret,
                         size_t max_body,
                         int *out_status, char **out_body, size_t *out_len);

#ifdef __cplusplus
}
#endif
