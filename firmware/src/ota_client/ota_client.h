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
 * Beyond the request primitives, the T16 task also implements the full pull
 * pipeline (tasks 3.6–3.8): manifest decision (SemVer + seq high-water +
 * min_version, R-V01/V02/V03), download + SHA-256/size verify of both
 * artefacts into PSRAM before any flash write (R-C04/C05), and apply under the
 * night-window and quiet-gate policy by feeding the existing T13 push-OTA
 * entry points (ota_firmware_begin/write/end, then ota_assets_begin/end).
 * Kept separate from ota_manager (T13 owns the dual-bank flash state machine;
 * this module drives it).
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

/* ── Pinned-cert storage (R-A03/A04) ───────────────────────────────────── */

/** @brief Max PEM size for the pinned server cert (RSA-3072 self-signed ≈ 1.5 KB). */
#define ROTA_CERT_MAX 2048u

/**
 * @brief Copy the active pinned server certificate into @p buf.
 *
 * Returns the operator-uploaded cert (NVS `system/ota_cert`) if present,
 * otherwise the firmware-embedded default (`OTA_DEFAULT_CERT_PEM`, R-A04).
 * The result is a NUL-terminated PEM suitable to pass as `cert_pem` to
 * rota_https_get().
 *
 * @param buf  Destination; recommend ≥ ROTA_CERT_MAX.
 * @param cap  Capacity of @p buf.
 * @return PEM length (excl NUL) on success, -1 if @p buf is too small / bad args.
 */
int rota_cert_get(char *buf, size_t cap);

/**
 * @brief Store an operator-uploaded pinned certificate (PEM) in NVS.
 * @param pem  NUL-terminated PEM (≤ ROTA_CERT_MAX). Empty/NULL reverts to the
 *             embedded default (equivalent to rota_cert_clear()).
 * @return ESP_OK on success.
 */
esp_err_t rota_cert_set(const char *pem);

/** @brief Remove any uploaded cert; subsequent rota_cert_get() returns the default. */
esp_err_t rota_cert_clear(void);

/** @brief True if an operator-uploaded certificate is currently stored. */
bool rota_cert_is_custom(void);

/* ── T16 task ──────────────────────────────────────────────────────────── */

/**
 * @brief T16 — ROTA pull-OTA client task (rota_tds.md §2.4).
 *
 * Periodically (every `ota_check_h` hours ± jitter, after a short boot settle)
 * checks the OTA server for a newer release: preconditions gate → pinned-cert
 * manifest GET with HMAC auth → SemVer/seq/min_version decision (audit 22/23)
 * → download + verify both artefacts → apply under the night/quiet policy via
 * T13 (audit 24), which switches the boot bank and reboots. A manual check can
 * be requested at any time (see rota_status / POST /api/ota/check).
 *
 * @param pvParameters Unused; pass NULL.
 */
void task_ota_client(void *pvParameters);

/* ── T16 observability (rota_tds.md §2.4, task 3.9) ────────────────────── */

/**
 * @brief Snapshot of T16's last manifest-check outcome, for `/api/ota/status`.
 *
 * Written only by T16, read by the web task. Fields are small scalars plus a
 * short string; a reader may observe a struct mid-update (benign for a status
 * view — at worst a stale field for one poll).
 */
typedef struct {
    int64_t  last_check_epoch;  /**< Wall-clock (Unix s) of the last completed check; 0 = never. */
    int32_t  last_result;       /**< Last check audit sub (22): -1 none, 0 up-to-date, 1 update-avail, 2 unreachable, 3 skipped, 4 auth-fail. */
    int32_t  last_http;         /**< Last manifest HTTP status; 0 = transport failure. */
    uint32_t checks_total;      /**< Manifest GETs attempted since boot. */
    char     offered_ver[40];   /**< Server-offered version at the last HTTP 200; "" otherwise. */
    int32_t  last_dl;           /**< Last download/verify audit sub (23): -1 none, 0 ok, 1 TLS/pin, 2 SHA/size, 3 downgrade, 4 min_version. */
    int32_t  last_apply;        /**< Last apply audit sub (24): -1 none, 0 committed, 1 deferred, 2 failed. */
} rota_status_t;

/**
 * @brief Copy T16's current status snapshot into @p out (never fails).
 * @param out Destination; ignored if NULL.
 */
void rota_status_get(rota_status_t *out);

/**
 * @brief True when a verified update is downloaded and awaiting its apply
 *        window (i.e. an apply was deferred by the night-window/quiet gate).
 *
 * Drives the "update pending" badge on the status shield (mirrors the
 * `coredump_available` informational-flag pattern). Cleared once the update is
 * applied (the unit then reboots into it), no longer offered, or ROTA is
 * disabled. Volatile — false at boot, re-derived on the next check.
 */
bool rota_update_pending(void);

#ifdef __cplusplus
}
#endif
