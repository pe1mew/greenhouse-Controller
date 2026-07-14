/**
 * @file web_server.cpp
 * @brief T11 — Web Server task: 25+ HTTP routes + /ws WebSocket push.
 *
 * Replaces alpha.5 `web_server_tickle.cpp` (3-route hardcoded HTML) with
 * the production T11 backed by `esp_http_server` + LittleFS. The original
 * 1.20.3 file (1330 lines, ESPAsyncWebServer-based) is archived as
 * `web_server_1.20.3_original.cpp.archived`.
 *
 * ## Route taxonomy
 *
 * **Static (5)** — served from active LittleFS partition; placeholder page
 * on factory-fresh unit so the operator can see T11 is up before OTA:
 *   - GET  /                  → /index.html
 *   - GET  /index.html        → /index.html (explicit alias of root)
 *   - GET  /style.css         → /style.css
 *   - GET  /app.js            → /app.js
 *   - GET  /manifest.json     → /manifest.json
 *
 * **Auth (3)**:
 *   - GET  /api/whoami        → {role:"farmer"|"admin"} or 401
 *   - POST /api/login         → {role, pin} → set cookie + return ok or 401
 *   - POST /api/logout        → clear cookie + invalidate session
 *
 * **Status (2, public)**:
 *   - GET  /api/status        — canonical JSON snapshot (live tiles)
 *   - GET  /api/history       — ?n=N last sensor rows (cap N=60)
 *
 * **Config (3)**:
 *   - GET  /api/config        — full cfg shadow dump (any session)
 *   - GET  /api/config/limits — bounds for input validation (public)
 *   - POST /api/config        — farmer-keys + admin-keys policy
 *
 * **Admin (2)**:
 *   - POST /api/wifi          — credentials change → scheduled reboot
 *   - POST /api/pin           — change farmer / admin PIN
 *
 * **SD + log (5)**:
 *   - GET  /api/sd/status     — mounted, free / total (public)
 *   - POST /api/sd/mount      — remount via T9 (admin)
 *   - POST /api/sd/unmount    — flush + unmount (admin)
 *   - GET  /api/log/files     — list .csv on SD (admin)
 *   - GET  /api/log/download  — ?file=NAME (admin)
 *
 * **Coredump (3, admin + rate-limited)**:
 *   - GET  /api/coredump/status
 *   - GET  /api/coredump/download   — value_a=19 audit
 *   - POST /api/coredump/erase      — value_a=20 audit
 *
 * **OTA + web-tab (5)**:
 *   - GET  /api/ota/status    (any session)
 *   - POST /api/ota/firmware  (admin)
 *   - POST /api/ota/assets    (admin)
 *   - GET  /api/web           (admin)
 *   - POST /api/web           (admin; validates + audit-logs every field)
 *
 * **WebSocket (1, public)**:
 *   - /ws                     — status push every 2 s
 *
 * ## Session model
 * In-memory table of `MAX_SESSIONS=4` slots, each holding a 16-hex-char
 * token, a `web_session_role_t` (WEB_ROLE_FARMER=0 or WEB_ROLE_ADMIN=1),
 * and an `expiry` Unix timestamp. Browsers store the token in a
 * `Set-Cookie: session=TOKEN; Path=/; HttpOnly` cookie. Each authenticated
 * request slides the expiry forward by `cfg.session_timeout_min × 60` s.
 * Sessions are lost on reboot.
 *
 * ## Audit trail
 * Config changes that go via Q4 → T4 get their LOG_SETPOINT row emitted
 * by T4 after the NVS write succeeds (initiator carried in
 * `config_update_t::initiator`). Config changes that bypass T4 (PIN,
 * WiFi creds, /api/web settings) emit their own audit rows from this
 * file via `log_web_setpoint()`. Sensitive values (PINs, passphrases,
 * shared secrets) log only a "set" marker (value_a=1) — the actual
 * value is never written to the CSV.
 *
 * ## LittleFS fallback
 * On a factory-fresh unit (LittleFS empty), `/index.html` returns a tiny
 * built-in placeholder page that says "Web assets not yet uploaded — use
 * OTA /api/web". The placeholder lets the operator visually confirm T11
 * is responding even before web-asset OTA.
 *
 * ## Threading
 * - httpd runs in its own task pool (spawned by `httpd_start()`) — each
 *   handler can run concurrently with another.
 * - Session table is guarded by `s_sess_mux` (200 ms acquire timeout).
 * - `task_ws_push` is pinned to core 1 (APP_CPU) so a slow snapshot+JSON
 *   build can't block httpd's accept/dispatch on core 0.
 *
 * @author  Greenhouse Controller project
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_heap_caps.h"

#include "esp_mac.h"           /* alpha.6.18 — esp_read_mac for AP SSID */
#include "esp_system.h"        /* alpha.6.18 — esp_restart() for /api/wifi apply */
#include "esp_core_dump.h"     /* a.6.35.6  — /api/coredump endpoints */
#include "esp_partition.h"     /* a.6.35.6  — coredump partition read */
#include "esp_timer.h"         /* a.6.35.6  — rate-limit timestamp */

#include "web_server.h"
#include "../types/app_types.h"
#include "../data_manager/data_manager.h"
#include "../auth/pin_auth.h"
#include "../status_post/status_json.h"   /* alpha.6.17 — build_canonical_status_json */
#include "../event_logger/event_logger.h" /* alpha.6.19 — event_logger_sd_remount / _unmount */
#include "../ota_manager/ota_manager.h"   /* alpha.6.20 — ota_firmware_/assets_/get_* */
#include "../status_post/status_post.h"   /* alpha.6.20 — status_post_last_str (web tab) */
#include "../ota_client/ota_client.h"     /* 2.2.0 (ROTA) — rota_cert_set/_is_custom for /api/ota/config */
#include "../system_id/system_id.h"       /* 2.2.0 (ROTA) — system_mac_str: device id for /api/ota/check */
#include "littlefs_storage.h"
#include "sd_storage.h"        /* alpha.6.19 — storage_sd_* for SD status + log list/download */
#include "nvs_config.h"        /* alpha.6.18 — nvs_cfg_get_str / nvs_cfg_set_str for /api/wifi + /api/config GET */
#include "cfg_limits.h"        /* alpha.6.18 — CFG_MIN_/MAX_ macros stringified into /api/config/limits */

static const char *TAG = "T11_WEB";

#ifndef FIRMWARE_VERSION
#  define FIRMWARE_VERSION "unstamped"
#endif

/* ============================================================
 * Compile-time constants
 * ============================================================ */
#define MAX_SESSIONS        4    /**< Max concurrent web sessions */
#define TOKEN_LEN          16    /**< Session token length (hex chars) */
#define COOKIE_HEADER_MAX  128   /**< Max Cookie: header size we'll parse */
#define LFS_READ_BUF       4096  /**< LittleFS chunk buffer for streaming */
#define SESSION_DEFAULT_S  600u  /**< 10 min default if cfg.session_timeout_min unset */

/* ============================================================
 * Session table — in-memory only (lost on reboot)
 *
 * `web_session_role_t` distinguishes "no session" from the two valid
 * roles. The pin_auth.h enum has only FARMER=0 and ADMIN=1; we need a
 * third state to mean "not authenticated", hence the local typedef.
 * ============================================================ */
typedef enum {
    WEB_ROLE_NONE   = -1, /**< No session / not authenticated */
    WEB_ROLE_FARMER = 0,  /**< Matches web_session_role_t PIN_ROLE_FARMER */
    WEB_ROLE_ADMIN  = 1,  /**< Matches web_session_role_t PIN_ROLE_ADMIN */
} web_session_role_t;

typedef struct {
    char               token[TOKEN_LEN + 1]; /**< Hex-string session ID. Empty = slot free. */
    web_session_role_t role;                  /**< WEB_ROLE_FARMER or WEB_ROLE_ADMIN */
    int32_t            expiry;                /**< Unix time when session expires; 0 = slot free */
    int32_t            timeout_s;             /**< Configured timeout, for sliding renewal */
} web_session_t;

static web_session_t      s_sessions[MAX_SESSIONS] = {};
static SemaphoreHandle_t  s_sess_mux = NULL;
static httpd_handle_t     s_server = NULL;

/* ============================================================
 * Session helpers
 * ============================================================ */

/**
 * @brief Generate a 16-character hex token using esp_random.
 *
 * Output is NUL-terminated. 64 bits of entropy — sufficient for the
 * single-unit-per-operator threat model. (The session_timeout_min ≤ 60
 * upper bound limits any brute-force window further.)
 */
static void gen_token(char out[TOKEN_LEN + 1])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < TOKEN_LEN; i++) {
        out[i] = hex[esp_random() & 0xF];
    }
    out[TOKEN_LEN] = '\0';
}

/**
 * @brief Find a valid session by token. Returns role on hit, or
 *        WEB_ROLE_NONE (0) on miss/expired.
 *
 * On hit, slides the expiry forward by timeout_s (renewal pattern).
 */
static web_session_role_t session_find_and_renew(const char *token)
{
    if (token == NULL || token[0] == '\0') return WEB_ROLE_NONE;

    web_session_role_t role = WEB_ROLE_NONE;
    const int32_t now = (int32_t)time(NULL);

    xSemaphoreTake(s_sess_mux, pdMS_TO_TICKS(200));
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].expiry > 0 &&
            s_sessions[i].expiry > now &&
            strcmp(s_sessions[i].token, token) == 0) {
            role = s_sessions[i].role;
            s_sessions[i].expiry = now + s_sessions[i].timeout_s;
            break;
        }
    }
    xSemaphoreGive(s_sess_mux);
    return role;
}

/* True if any web session OTHER THAN exempt_token is live (ROTA quiet gate,
 * R-P02). exempt_token NULL/empty counts every session. The "except" form lets
 * an operator's own GUI-triggered ROTA update ignore the triggering session so
 * the apply is not deferred forever on their own login (gh#41). */
bool web_any_active_session_except(const char *exempt_token)
{
    bool active = false;
    const int32_t now = (int32_t)time(NULL);
    if (s_sess_mux == NULL) return false;
    if (xSemaphoreTake(s_sess_mux, pdMS_TO_TICKS(200)) != pdTRUE) return true; /* busy → treat as active (safe) */
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].expiry > now) {
            if (exempt_token != NULL && exempt_token[0] != '\0' &&
                strcmp(s_sessions[i].token, exempt_token) == 0) {
                continue;                    /* the triggering session — exempt (gh#41) */
            }
            active = true; break;
        }
    }
    xSemaphoreGive(s_sess_mux);
    return active;
}

/* True if any web session is currently live (ROTA quiet gate, R-P02). */
bool web_any_active_session(void)
{
    return web_any_active_session_except(NULL);
}

/**
 * @brief Allocate a session slot, generate a token, return it.
 *
 * If all slots are full, the OLDEST (smallest expiry) is evicted.
 * Returns false only on mutex failure.
 */
static bool session_open(web_session_role_t role, int32_t timeout_s,
                          char out_token[TOKEN_LEN + 1])
{
    if (out_token == NULL) return false;
    if (timeout_s <= 0)    timeout_s = SESSION_DEFAULT_S;

    const int32_t now = (int32_t)time(NULL);

    if (xSemaphoreTake(s_sess_mux, pdMS_TO_TICKS(200)) != pdTRUE) {
        return false;
    }

    /* Find a free slot, OR the slot with smallest expiry. */
    int chosen = 0;
    int32_t oldest = s_sessions[0].expiry;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].expiry == 0) {
            chosen = i;
            oldest = -1;            /* free slot wins */
            break;
        }
        if (s_sessions[i].expiry < oldest) {
            oldest = s_sessions[i].expiry;
            chosen = i;
        }
    }

    gen_token(out_token);
    strncpy(s_sessions[chosen].token, out_token, TOKEN_LEN);
    s_sessions[chosen].token[TOKEN_LEN] = '\0';
    s_sessions[chosen].role      = role;
    s_sessions[chosen].timeout_s = timeout_s;
    s_sessions[chosen].expiry    = now + timeout_s;

    xSemaphoreGive(s_sess_mux);

    ESP_LOGI(TAG, "[T11] session opened: role=%d slot=%d timeout=%lds",
             (int)role, chosen, (long)timeout_s);
    return true;
}

/**
 * @brief Invalidate a session by token. No-op if token not found.
 */
static void session_close(const char *token)
{
    if (token == NULL || token[0] == '\0') return;
    xSemaphoreTake(s_sess_mux, pdMS_TO_TICKS(200));
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (strcmp(s_sessions[i].token, token) == 0) {
            memset(&s_sessions[i], 0, sizeof(s_sessions[i]));
            break;
        }
    }
    xSemaphoreGive(s_sess_mux);
}

/* ============================================================
 * Cookie parsing helper
 *
 * Cookie headers look like:
 *   Cookie: session=abc123; other=def
 *
 * We extract the value of the "session=" key into out_token. Returns
 * true on found, false otherwise. out_token must be at least
 * TOKEN_LEN+1 bytes.
 * ============================================================ */
static bool cookie_get_session(httpd_req_t *req, char out_token[TOKEN_LEN + 1])
{
    if (out_token == NULL) return false;
    out_token[0] = '\0';

    char hdr[COOKIE_HEADER_MAX] = {0};
    esp_err_t err = httpd_req_get_hdr_value_str(req, "Cookie", hdr, sizeof(hdr));
    if (err != ESP_OK) return false;

    /* Find "session=" — must be at the start or after "; ". */
    const char *p = strstr(hdr, "session=");
    if (p == NULL) return false;
    /* Ensure it's a standalone cookie name, not part of another like "x-session=" */
    if (p != hdr && p[-1] != ' ' && p[-1] != ';') return false;
    p += strlen("session=");

    /* Copy until ';' or end-of-string, up to TOKEN_LEN chars. */
    int i = 0;
    while (*p && *p != ';' && *p != ' ' && i < TOKEN_LEN) {
        out_token[i++] = *p++;
    }
    out_token[i] = '\0';
    return (i == TOKEN_LEN);
}

/**
 * @brief Set a session cookie in the response.
 *
 * `Set-Cookie: session=TOKEN; Path=/; HttpOnly; Max-Age=N`
 *
 * **CRITICAL**: the caller must supply `hdr` — `httpd_resp_set_hdr` does
 * NOT copy the value; it stores a pointer that must remain valid until
 * `httpd_resp_send` is invoked. A stack-local buffer inside this helper
 * would fall out of scope as soon as the helper returned, so the response
 * cookie would read garbage. (alpha.6.16 acceptance test caught this on
 * the first curl pass — `Set-Cookie: ???` instead of the hex token.)
 *
 * @param hdr     Caller-owned buffer >= 96 bytes. Lives until the
 *                response is sent.
 * @param hdr_cap Size of @p hdr in bytes (>= 96).
 */
static void cookie_set_session(httpd_req_t *req, const char *token,
                                int32_t max_age_s,
                                char *hdr, size_t hdr_cap)
{
    snprintf(hdr, hdr_cap,
             "session=%s; Path=/; HttpOnly; Max-Age=%ld",
             token, (long)max_age_s);
    httpd_resp_set_hdr(req, "Set-Cookie", hdr);
}

/**
 * @brief Clear the session cookie (Max-Age=0 expires it immediately).
 *
 * The string literal here has static storage duration, so passing it
 * directly to `httpd_resp_set_hdr` is safe (unlike the per-request
 * `Set-Cookie` value in cookie_set_session above).
 */
static void cookie_clear_session(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Set-Cookie",
                       "session=; Path=/; HttpOnly; Max-Age=0");
}

/* ============================================================
 * Auth-check helper for protected routes
 *
 * Returns role on success, WEB_ROLE_NONE on failure (and sends 401).
 *
 * Currently unused — the 7 minimal routes don't gate on this helper
 * (whoami is intentionally public; login/logout are pre-session). The
 * 18+ deferred routes (config, sd, ota, etc.) all need it, so we keep
 * it here ready for them. __attribute__((unused)) suppresses the
 * -Wunused-function warning until a deferred route calls in.
 * ============================================================ */
__attribute__((unused))
static web_session_role_t require_auth(httpd_req_t *req, web_session_role_t min_role)
{
    char token[TOKEN_LEN + 1] = {0};
    if (!cookie_get_session(req, token)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"no_session\"}", HTTPD_RESP_USE_STRLEN);
        return WEB_ROLE_NONE;
    }
    web_session_role_t role = session_find_and_renew(token);
    if (role == WEB_ROLE_NONE || (int)role < (int)min_role) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"unauthorized\"}", HTTPD_RESP_USE_STRLEN);
        return WEB_ROLE_NONE;
    }
    return role;
}

/* ============================================================
 * LittleFS-served static handlers
 *
 * Pattern: try littlefs_read on the requested path. If found, set
 * the appropriate Content-Type and stream the bytes in LFS_READ_BUF
 * chunks. If not found, return a small placeholder.
 *
 * Active partition is queried via littlefs_active_partition() (an A/B
 * dual-partition wrapper from LIB-9). T11 always reads from the
 * active partition (the inactive partition is used by T13 OTA writes).
 * ============================================================ */

/**
 * @brief Stream a LittleFS file as the HTTP response body.
 *
 * @param req      esp_http_server request handle
 * @param fs_path  Path within the active LittleFS partition (must start with '/')
 * @param mime     Content-Type to set
 * @return ESP_OK on file-found-and-streamed; sends 404 placeholder on
 *         file-not-found (returns ESP_OK from httpd's view either way
 *         — the placeholder body is the 404 user-facing content).
 */
static esp_err_t serve_lfs_file(httpd_req_t *req, const char *fs_path,
                                 const char *mime)
{
    const lfs_partition_t active = littlefs_active_partition();

    if (!littlefs_exists(active, fs_path)) {
        /* Placeholder: 404 with a small explanation. 512 bytes is generous
         * for the ~300-byte body; gcc's -Wformat-truncation analyser is
         * pessimistic about %s widths so 256 was too tight. */
        char body[512];
        int n = snprintf(body, sizeof(body),
            "<!DOCTYPE html><html><body style='font-family:sans-serif;padding:2em'>"
            "<h2>Greenhouse Controller %s</h2>"
            "<p>Web assets not yet uploaded.</p>"
            "<p>Upload the GUI asset bundle (STORE-only ZIP) with: "
            "<code>POST /api/ota/assets</code> (admin auth required). "
            "Use 1.20.x's <code>web-assets-X.Y.Z.zip</code> as a starting point.</p>"
            "<p>Requested path: <code>%s</code></p>"
            "</body></html>",
            FIRMWARE_VERSION, fs_path);
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, body, (n > 0) ? (size_t)n : 0);
        return ESP_OK;
    }

    /* alpha.6.24 — chunked stdio streaming. The old single-read path was:
     *   1. heap_caps_malloc(LFS_READ_BUF=4096) once
     *   2. littlefs_read(buf, 4096) which NUL-terminates inside the buffer
     *   3. httpd_resp_send(buf, strlen(buf))
     * That truncated any file > 4 KB (index.html and app.js are both ~40 KB)
     * AND treated binary 0x00 bytes as EOF (style.css survives but a future
     * binary asset would corrupt past the first NUL).
     *
     * The fix uses stdio against the active partition's VFS mountpoint,
     * fstat for the true file size, and httpd_resp_send_chunk to stream
     * arbitrarily large files in LFS_READ_BUF chunks. */
    char vfs_path[64];
    int n = snprintf(vfs_path, sizeof(vfs_path), "%s%s",
                     littlefs_mountpoint(active), fs_path);
    if (n <= 0 || (size_t)n >= sizeof(vfs_path)) {
        ESP_LOGW(TAG, "[T11] serve_lfs path too long: %s", fs_path);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    FILE *fp = fopen(vfs_path, "rb");
    if (fp == NULL) {
        ESP_LOGW(TAG, "[T11] fopen(%s) failed", vfs_path);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char *buf = (char *)heap_caps_malloc(LFS_READ_BUF, MALLOC_CAP_INTERNAL);
    if (buf == NULL) {
        ESP_LOGE(TAG, "[T11] serve_lfs %s: malloc(%u) failed",
                 fs_path, (unsigned)LFS_READ_BUF);
        fclose(fp);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, mime);

    /* Stream in chunks until fread returns 0. esp_http_server's chunked-
     * encoding path is engaged automatically when httpd_resp_send_chunk is
     * called without a prior httpd_resp_send. Terminate with a zero-length
     * chunk per HTTP/1.1 chunked spec. */
    size_t total = 0;
    esp_err_t err = ESP_OK;
    for (;;) {
        size_t got = fread(buf, 1, LFS_READ_BUF, fp);
        if (got == 0) break;
        err = httpd_resp_send_chunk(req, buf, got);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "[T11] serve_lfs %s: send_chunk failed at %u B: %s",
                     fs_path, (unsigned)total, esp_err_to_name(err));
            break;
        }
        total += got;
    }
    /* Terminator chunk — required even on error to avoid leaking a partial
     * connection state into the next request on the same keepalive socket. */
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    } else {
        (void)httpd_resp_send_chunk(req, NULL, 0);
    }

    heap_caps_free(buf);
    fclose(fp);
    ESP_LOGD(TAG, "[T11] served %s (%u B)", fs_path, (unsigned)total);
    return err;
}

/**
 * @brief HTTP GET / — serve the dashboard SPA index page.
 *
 * Used as the handler for both `/` and `/index.html`. Streams the file
 * from the active LittleFS partition; falls back to a built-in placeholder
 * on factory-fresh units.
 *
 * @param req esp_http_server request handle (no auth required).
 * @return ESP_OK if a body was sent (200 or 404 placeholder); ESP_FAIL on
 *         internal error (5xx already sent).
 * @note Auth requirement: Public.
 * @note Rate limit: none.
 */
static esp_err_t root_handler(httpd_req_t *req)
{
    return serve_lfs_file(req, "/index.html", "text/html; charset=utf-8");
}

/**
 * @brief HTTP GET /style.css — serve the SPA stylesheet from active LittleFS.
 * @param req esp_http_server request handle.
 * @return ESP_OK on success; ESP_FAIL on internal error.
 * @note Auth requirement: Public.
 */
static esp_err_t style_handler(httpd_req_t *req)
{
    return serve_lfs_file(req, "/style.css", "text/css; charset=utf-8");
}

/**
 * @brief HTTP GET /app.js — serve the dashboard SPA JavaScript bundle.
 * @param req esp_http_server request handle.
 * @return ESP_OK on success; ESP_FAIL on internal error.
 * @note Auth requirement: Public.
 */
static esp_err_t appjs_handler(httpd_req_t *req)
{
    return serve_lfs_file(req, "/app.js", "application/javascript; charset=utf-8");
}

/**
 * @brief HTTP GET /manifest.json — serve the PWA manifest.
 * @param req esp_http_server request handle.
 * @return ESP_OK on success; ESP_FAIL on internal error.
 * @note Auth requirement: Public.
 */
static esp_err_t manifest_handler(httpd_req_t *req)
{
    return serve_lfs_file(req, "/manifest.json", "application/manifest+json");
}

/* ============================================================
 * Auth handlers
 * ============================================================ */

/**
 * @brief HTTP GET /api/whoami — report the current session role.
 *
 * Browsers call this on page load to know whether to show the login
 * overlay. Slides the session expiry forward on a hit (renewal).
 *
 * @param req esp_http_server request handle (cookie session is parsed inside).
 * @return ESP_OK — response sent. 200 + `{"role":"farmer"|"admin"}` on valid
 *         session, 401 + `{"ok":false,"error":"no_session"}` otherwise.
 * @note Auth requirement: Public (and reveals only the role string on hit).
 * @note Rate limit: none.
 * @note Audit-logged: no (read-only endpoint).
 */
static esp_err_t whoami_handler(httpd_req_t *req)
{
    char token[TOKEN_LEN + 1] = {0};
    web_session_role_t role = WEB_ROLE_NONE;
    if (cookie_get_session(req, token)) {
        role = session_find_and_renew(token);
    }

    httpd_resp_set_type(req, "application/json");
    if (role == WEB_ROLE_FARMER) {
        return httpd_resp_send(req, "{\"role\":\"farmer\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (role == WEB_ROLE_ADMIN) {
        return httpd_resp_send(req, "{\"role\":\"admin\"}", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"no_session\"}",
                           HTTPD_RESP_USE_STRLEN);
}

/**
 * @brief Find the value of a JSON string field via crude substring matching.
 *
 * Looks for `"field":"value"` or `"field":value` in the request body.
 * Returns true on found, writes the value into `out` (NUL-terminated, up
 * to `out_cap-1` chars).
 *
 * This is a deliberately minimal JSON parser — adequate for the small,
 * trusted login payloads. A real JSON parser lands when we port
 * status_json.cpp + add cJSON.
 */
static bool json_get_field(const char *json, const char *field,
                            char *out, size_t out_cap)
{
    if (json == NULL || field == NULL || out == NULL || out_cap == 0) return false;

    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\"", field);
    const char *p = strstr(json, needle);
    if (p == NULL) return false;

    p += strlen(needle);
    /* Skip whitespace, colon, more whitespace. */
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;

    /* String value: "..." */
    if (*p == '"') {
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < out_cap) {
            out[i++] = *p++;
        }
        out[i] = '\0';
        return true;
    }

    /* Bare value (number, true, false, null) — copy until ',' or '}' or ws. */
    size_t i = 0;
    while (*p && *p != ',' && *p != '}' && *p != ' ' && *p != '\t' &&
           *p != '\r' && *p != '\n' && i + 1 < out_cap) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (i > 0);
}

/**
 * @brief HTTP POST /api/login — verify a PIN and open a session.
 *
 * Body: `{"role":"farmer"|"admin","pin":"NNNN"}`. Calls
 * `pin_auth_verify()` which enforces the lockout policy.
 *
 * @param req esp_http_server request handle.
 * @return ESP_OK on response sent (200 or 401); ESP_FAIL on internal recv error.
 *         - 200 + `{"ok":true,"role":"R"}` + `Set-Cookie: session=TOKEN` on match.
 *         - 401 + `{"ok":false,"locked":false}` on wrong PIN.
 *         - 401 + `{"ok":false,"locked":true,"remaining":N}` on lockout.
 *         - 400 on bad payload.
 * @note Auth requirement: Public (this IS the auth gate).
 * @note Rate limit: enforced by `pin_auth_verify()` lockout policy
 *       (PIN_LOCKOUT_MAX_DEFAULT failures → PIN_LOCKOUT_SECS_DEFAULT seconds).
 * @note Audit-logged: implicit — `pin_auth_verify()` updates the NVS-stored
 *       failure counters and lockout expiries.
 */
static esp_err_t login_handler(httpd_req_t *req)
{
    /* Read body (max 128 bytes — login payload is tiny). */
    char body[128] = {0};
    int total = (int)req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"bad_body\"}", HTTPD_RESP_USE_STRLEN);
    }

    int read_total = 0;
    while (read_total < total) {
        int r = httpd_req_recv(req, body + read_total, total - read_total);
        if (r <= 0) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        read_total += r;
    }
    body[read_total] = '\0';

    char role_str[16] = {0};
    char pin_str[16]  = {0};
    if (!json_get_field(body, "role", role_str, sizeof(role_str)) ||
        !json_get_field(body, "pin",  pin_str,  sizeof(pin_str))) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"bad_payload\"}", HTTPD_RESP_USE_STRLEN);
    }

    web_session_role_t role = WEB_ROLE_NONE;
    if      (strcmp(role_str, "farmer") == 0) role = WEB_ROLE_FARMER;
    else if (strcmp(role_str, "admin")  == 0) role = WEB_ROLE_ADMIN;
    else {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"bad_role\"}", HTTPD_RESP_USE_STRLEN);
    }

    pin_auth_result_t r = pin_auth_verify((pin_role_t)role, pin_str);
    httpd_resp_set_type(req, "application/json");

    if (r == PIN_AUTH_OK) {
        cfg_shadow_t cfg = {};
        dm_cfg_snapshot(&cfg);
        int32_t timeout_s = (cfg.session_timeout_min > 0)
                            ? (int32_t)cfg.session_timeout_min * 60
                            : (int32_t)SESSION_DEFAULT_S;

        char token[TOKEN_LEN + 1] = {0};
        if (!session_open(role, timeout_s, token)) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        /* hdr_buf lives in this stack frame, so it outlives the
         * httpd_resp_set_hdr → httpd_resp_send sequence. Required by
         * the IDF httpd contract — the helper does NOT copy the value. */
        char hdr_buf[96];
        cookie_set_session(req, token, timeout_s, hdr_buf, sizeof(hdr_buf));
        char resp[64];
        int n = snprintf(resp, sizeof(resp),
                         "{\"ok\":true,\"role\":\"%s\"}", role_str);
        ESP_LOGI(TAG, "[T11] /api/login OK role=%s", role_str);
        return httpd_resp_send(req, resp, (size_t)n);
    }

    if (r == PIN_AUTH_LOCKED_OUT) {
        uint32_t remaining = pin_auth_lockout_remaining_secs((pin_role_t)role);
        char resp[80];
        int n = snprintf(resp, sizeof(resp),
                         "{\"ok\":false,\"locked\":true,\"remaining\":%lu}",
                         (unsigned long)remaining);
        ESP_LOGW(TAG, "[T11] /api/login locked role=%s remaining=%lus",
                 role_str, (unsigned long)remaining);
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_send(req, resp, (size_t)n);
    }

    /* Wrong PIN or some other error. */
    ESP_LOGW(TAG, "[T11] /api/login FAIL role=%s rc=%d", role_str, (int)r);
    httpd_resp_set_status(req, "401 Unauthorized");
    return httpd_resp_send(req,
        "{\"ok\":false,\"locked\":false}", HTTPD_RESP_USE_STRLEN);
}

/**
 * @brief HTTP POST /api/logout — invalidate the session, clear the cookie.
 *
 * Parses the session cookie (if any), closes the slot, then emits
 * `Set-Cookie: session=; Max-Age=0` so the browser drops its copy too.
 *
 * @param req esp_http_server request handle.
 * @return ESP_OK — always 200 with `{"ok":true}`, even if no session existed.
 * @note Auth requirement: Public (idempotent on missing session).
 * @note Rate limit: none.
 * @note Audit-logged: no.
 */
static esp_err_t logout_handler(httpd_req_t *req)
{
    char token[TOKEN_LEN + 1] = {0};
    if (cookie_get_session(req, token)) {
        session_close(token);
        ESP_LOGI(TAG, "[T11] /api/logout session closed");
    }
    cookie_clear_session(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* ============================================================
 * Status routes (alpha.6.17 / 6.16-γ)
 *
 * GET /api/status   — canonical status JSON snapshot for dashboard tiles.
 * GET /api/history  — last N sensor ring entries (?n=N, default 60).
 *
 * Both are intentionally PUBLIC (no auth gate). Rationale: the web GUI
 * dashboard polls these on every page load to render the status tiles
 * before the operator decides to log in. The 1.20.3 web_server.cpp had
 * the same policy. Setpoint changes are still admin-only via /api/config.
 * ============================================================ */

#define HIST_MAX_ROWS  60  /**< Cap on /api/history?n=N */

/* ============================================================
 * Audit-log helpers (a.6.35.5) — emit LOG_SETPOINT rows for the four
 * config paths that bypass Q4 → T4 (and therefore T4's automatic audit
 * emission).
 *
 * Sensitive-value semantics: for PIN / WiFi credentials / shared secrets,
 * value_a=1 means "field was changed" — the actual value is intentionally
 * NOT logged so the SD CSV doesn't become a credential exfil surface.
 *
 * Numeric /api/web fields (status_interval_s, status_enable, etc.) still
 * encode old → new the way the climate/wind setpoints do.
 * ============================================================ */
/**
 * @brief Post a LOG_SETPOINT row with initiator=LOG_BY_WEB.
 *
 * Used by the /api/wifi, /api/pin, /api/config (tz_str only) and /api/web
 * paths to record audit events for changes that don't go through Q4 → T4.
 *
 * @param pid      Parameter identifier (LOG_PARAM_*).
 * @param value_a  First payload — for sensitive fields, 1 = "changed";
 *                 for numeric fields, the old value.
 * @param value_b  Second payload — 0 for sensitive fields, new value otherwise.
 */
static void log_web_setpoint(log_param_id_t pid, int16_t value_a, int16_t value_b)
{
    log_event_t ev = {};
    ev.timestamp  = (uint32_t)time(NULL);
    ev.event_type = (uint8_t)LOG_SETPOINT;
    ev.initiator  = (uint8_t)LOG_BY_WEB;
    ev.param_id   = (uint8_t)pid;
    ev.value_a    = value_a;
    ev.value_b    = value_b;
    log_post(&ev);
}

/**
 * @brief HTTP GET /api/status — canonical status JSON snapshot.
 *
 * Calls `dm_status_snapshot()` then `build_canonical_status_json()` with
 * STATUS_EXPOSE_ALL + include_disabled_setpoints=true (local-UI mode,
 * shows every tile group regardless of operator's expose mask). Same JSON
 * shape as the /ws WebSocket push.
 *
 * @param req esp_http_server request handle.
 * @return ESP_OK on response sent; ESP_FAIL on heap alloc or JSON build failure
 *         (a 500 is sent in that case).
 * @note Auth requirement: Public — same rationale as /ws (operator-visible
 *       data is open by design; sensitive surfaces are individually gated).
 * @note Rate limit: none.
 */
static esp_err_t status_handler(httpd_req_t *req)
{
    /* status_snapshot_t is large (~600 B); use a heap allocation rather
     * than the task stack. Done inside the handler so concurrent requests
     * each get their own buffer (httpd may dispatch handlers in parallel). */
    status_snapshot_t *snap =
        (status_snapshot_t *)heap_caps_malloc(sizeof(status_snapshot_t),
                                              MALLOC_CAP_INTERNAL);
    if (snap == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    memset(snap, 0, sizeof(*snap));
    dm_status_snapshot(snap);

    /* 4 KB body buffer — canonical JSON is typically 1.5–2.5 KB with all
     * tiles + windows + alarms. The build helper returns 0 on overflow. */
    const size_t cap = 4096;
    char *body = (char *)heap_caps_malloc(cap, MALLOC_CAP_INTERNAL);
    if (body == NULL) {
        heap_caps_free(snap);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t n = build_canonical_status_json(body, cap, snap,
                                           STATUS_EXPOSE_ALL,
                                           /*include_disabled_setpoints=*/true);
    heap_caps_free(snap);

    if (n == 0) {
        ESP_LOGW(TAG, "[T11] /api/status: build_canonical_status_json returned 0 "
                      "(buffer overflow?)");
        heap_caps_free(body);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, body, n);
    heap_caps_free(body);
    return err;
}

/**
 * @brief HTTP GET /api/history — last N sensor ring entries.
 *
 * Query string: `?n=N` (default 60, clamped to `1..HIST_MAX_ROWS`). Reads
 * the most-recent N rows from T4's sensor ring buffer and emits JSON
 * shaped `{"rows":[...]}` with field names matching the canonical status
 * JSON's climate/wind blocks (so the dashboard's loader code can use the
 * same accessor names everywhere).
 *
 * @param req esp_http_server request handle.
 * @return ESP_OK on response sent; ESP_FAIL on heap alloc or read failure.
 * @note Auth requirement: Public (read-only history of sensor data).
 * @note Rate limit: none.
 * @see  webUiMock/mock_server.py::_build_history for the JSON envelope
 *       reference (matched at alpha.6.27).
 */
static esp_err_t history_handler(httpd_req_t *req)
{
    /* Parse ?n=N from the query string. Bounds: 1 ≤ n ≤ HIST_MAX_ROWS. */
    int n_req = 60;
    {
        char query[32] = {0};
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            char val[8] = {0};
            if (httpd_query_key_value(query, "n", val, sizeof(val)) == ESP_OK) {
                int parsed = atoi(val);
                if (parsed > 0) n_req = parsed;
            }
        }
    }
    if (n_req < 1)              n_req = 1;
    if (n_req > HIST_MAX_ROWS)  n_req = HIST_MAX_ROWS;

    const uint16_t total = dm_ring_count();
    uint16_t n = (uint16_t)n_req;
    if (n > total) n = total;

    /* Pull the last n entries from T4's ring buffer. dm_ring_read takes
     * an offset measured from the oldest entry, so we read starting at
     * (total - n) to get the most-recent n. */
    sensor_reading_t *rows = NULL;
    if (n > 0) {
        rows = (sensor_reading_t *)heap_caps_malloc(
            (size_t)n * sizeof(sensor_reading_t), MALLOC_CAP_INTERNAL);
        if (rows == NULL) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        uint16_t actually_read = 0;
        dm_ring_read((uint16_t)(total - n), rows, n, &actually_read);
        n = actually_read;
    }

    /* alpha.6.27 — corrected JSON output shape.
     *
     * Previous alpha.6.17 output had two defects observable in the GUI:
     *
     *   1. Bare array envelope `[…]` instead of `{"rows":[…]}`. The
     *      dashboard's loadHistory() in firmware/data/app.js short-circuits
     *      on `if (!data || !data.rows) return;` — silent no-op on a bare
     *      array. Symptom: history table never populates.
     *
     *   2. Raw field names `t/rh/ws/wd` instead of the dashboard's
     *      `temp_c/temp_avg_c/rh_pct/rh_avg_pct/speed_ms/speed_avg_ms/
     *      direction_deg/direction_variation_deg`. Even if (1) were fixed,
     *      every row cell would render as "—" because the readers all use
     *      the long names.
     *
     * The mock at webUiMock/mock_server.py::_build_history is the design
     * reference for both the envelope and the field-naming convention. The
     * comment on `_build_history` says: "Field names match the keys inside
     * the canonical status JSON's `climate` and `wind` blocks so the same
     * name carries the same number on /api/status and /api/history."
     *
     * Per-row size grew from ~80 B → ~160 B (more fields, decimals); bumped
     * the body cap from 8 KB to 12 KB for the n=60 worst case. */
    const size_t cap = 12288;
    char *body = (char *)heap_caps_malloc(cap, MALLOC_CAP_INTERNAL);
    if (body == NULL) {
        if (rows) heap_caps_free(rows);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t pos = 0;
    int w;
    w = snprintf(body + pos, cap - pos, "{\"rows\":[");
    if (w > 0) pos += (size_t)w;

    for (uint16_t i = 0; i < n && pos < cap - 1; i++) {
        const sensor_reading_t *e = &rows[i];
        /* Temperature uses the ×10 fields populated by T5 from the FG6485A's
         * native 0.1 °C resolution (rc.1.3.1). Wind is ×10 fixed-point in
         * storage; emit `%u.%u`. Same tenths-format trick as
         * `status_json.cpp:154-155` so the dashboard's .toFixed(1) renders
         * "21.4" rather than the previous "21.0"-stuck output. */
        w = snprintf(body + pos, cap - pos,
            "%s{\"ts\":%lu,"
              "\"temp_c\":%d.%d,\"temp_avg_c\":%d.%d,"
              "\"rh_pct\":%u,\"rh_avg_pct\":%u,"
              "\"speed_ms\":%u.%u,\"speed_avg_ms\":%u.%u,"
              "\"direction_deg\":%u,\"direction_variation_deg\":%u}",
            (i > 0) ? "," : "",
            (unsigned long)e->timestamp,
            e->temperature_c10 / 10,
            (e->temperature_c10 < 0 ? -e->temperature_c10 : e->temperature_c10) % 10,
            e->t_avg_c10 / 10,
            (e->t_avg_c10 < 0 ? -e->t_avg_c10 : e->t_avg_c10) % 10,
            (unsigned)e->humidity_pct,
            (unsigned)e->rh_avg_pct,
            (unsigned)(e->wind_speed_ms10     / 10u),
            (unsigned)(e->wind_speed_ms10     % 10u),
            (unsigned)(e->wind_speed_avg_ms10 / 10u),
            (unsigned)(e->wind_speed_avg_ms10 % 10u),
            (unsigned)e->wind_dir_deg,
            (unsigned)e->wind_dir_variation_deg);
        if (w < 0 || (size_t)w >= cap - pos) {
            ESP_LOGW(TAG, "[T11] /api/history: row %u truncated", (unsigned)i);
            break;
        }
        pos += (size_t)w;
    }

    if (pos < cap - 2) {
        body[pos++] = ']';
        body[pos++] = '}';
        body[pos]   = '\0';
    }

    if (rows) heap_caps_free(rows);

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, body, pos);
    heap_caps_free(body);
    return err;
}

/* ============================================================
 * Config routes (alpha.6.18 / 6.16-δ)
 *
 * GET  /api/config         — full cfg_shadow_t dump (auth required)
 * GET  /api/config/limits  — per-key bounds (PUBLIC, used for input validation)
 * POST /api/config         — {ns, key, value} or {ns, key, str_value}
 *                            farmer can write the climate.* keys and
 *                            wind/wind_prot_en;
 *                            admin can write anything
 * POST /api/wifi           — {ssid, psk} / {ap_psk} (admin only); restarts unit
 * POST /api/pin            — {role, pin} (admin only); changes farmer/admin PIN
 *
 * The "farmer-allowed keys" table mirrors 1.20.3 production exactly. Setpoint
 * changes via T8 LCD use the same keys, so farmer NVS write rights are
 * already in scope for the operator.
 * ============================================================ */

/** Farmer-writable NVS namespace + key list. Anything not here requires admin. */
#define FARMER_NS  "climate"
static const char * const FARMER_KEYS[] = {
    "t_max_day","t_min_day","t_max_ngt","t_min_ngt",
    "rh_max_day","rh_min_day","rh_max_ngt","rh_min_ngt",
    "rh_ctrl_en","cr_priority",
    NULL
};
static const char * const FARMER_WIND_KEYS[] = { "wind_prot_en", NULL };

static bool is_farmer_key(const char *ns, const char *key)
{
    if (strcmp(ns, FARMER_NS) == 0) {
        for (int i = 0; FARMER_KEYS[i]; i++) {
            if (strcmp(key, FARMER_KEYS[i]) == 0) return true;
        }
    } else if (strcmp(ns, "wind") == 0) {
        for (int i = 0; FARMER_WIND_KEYS[i]; i++) {
            if (strcmp(key, FARMER_WIND_KEYS[i]) == 0) return true;
        }
    }
    return false;
}

/**
 * @brief HTTP GET /api/config — full configuration as a JSON object.
 *
 * Mirrors 1.20.3's build_config_json exactly — same key names, same
 * ordering, so the web UI's `app.js` reads fields by the same identifiers
 * it always has. Includes the running fw_version from NVS, the AP SSID
 * computed from the WiFi STA MAC, and the WiFi STA SSID. The PSK is
 * deliberately write-only (never echoed back).
 *
 * @param req esp_http_server request handle (cookie session is parsed inside).
 * @return ESP_OK on response sent; ESP_FAIL on heap alloc failure.
 * @note Auth requirement: Farmer or Admin.
 * @note Rate limit: none.
 */
static esp_err_t config_get_handler(httpd_req_t *req)
{
    if (require_auth(req, WEB_ROLE_FARMER) == WEB_ROLE_NONE) return ESP_OK;

    cfg_shadow_t cfg = {};
    dm_cfg_snapshot(&cfg);

    char wifi_ssid[64] = {};
    nvs_cfg_get_str("wifi", "ssid", wifi_ssid, sizeof(wifi_ssid));

    char fw_version[32] = {};
    nvs_cfg_get_str(NVS_NS_SYSTEM, NVS_KEY_FW_VERSION,
                    fw_version, sizeof(fw_version));

    char ap_ssid[24] = {};
    {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(ap_ssid, sizeof(ap_ssid), "Greenhouse-%02X%02X", mac[4], mac[5]);
    }

    const size_t cap = 1536;
    char *body = (char *)heap_caps_malloc(cap, MALLOC_CAP_INTERNAL);
    if (body == NULL) { httpd_resp_send_500(req); return ESP_FAIL; }

    int n = snprintf(body, cap,
        "{"
        "\"wifi_ssid\":\"%s\","
        "\"ap_ssid\":\"%s\","
        "\"t_max_day\":%d,\"t_min_day\":%d,"
        "\"t_max_ngt\":%d,\"t_min_ngt\":%d,"
        "\"rh_max_day\":%d,\"rh_min_day\":%d,"
        "\"rh_max_ngt\":%d,\"rh_min_ngt\":%d,"
        "\"hyst_t\":%d,\"hyst_rh\":%d,"
        "\"rh_ctrl_en\":%d,\"cr_priority\":%d,"
        "\"avg_win_t\":%d,\"avg_win_rh\":%d,"
        "\"avg_win_wind\":%d,"
        "\"v_max\":%d,\"wind_prot_en\":%d,"
        "\"dir_excl_low\":%d,\"dir_excl_high\":%d,"
        "\"travel_s\":[%d,%d,%d],"
        "\"dwell_open_min\":[%d,%d,%d],"
        "\"dwell_close_min\":[%d,%d,%d],"
        "\"poll_interval_s\":%ld,"
        "\"session_timeout_min\":%ld,"
        "\"ap_timeout_min\":%ld,"
        "\"lat_deg\":%ld,\"lat_frac\":%ld,"
        "\"lon_deg\":%ld,\"lon_frac\":%ld,"
        "\"tz_str\":\"%s\","
        "\"fw_ver\":\"%s\""
        "}",
        wifi_ssid, ap_ssid,
        (int)cfg.t_max_day, (int)cfg.t_min_day,
        (int)cfg.t_max_ngt, (int)cfg.t_min_ngt,
        (int)cfg.rh_max_day, (int)cfg.rh_min_day,
        (int)cfg.rh_max_ngt, (int)cfg.rh_min_ngt,
        (int)cfg.hyst_t, (int)cfg.hyst_rh,
        (int)cfg.rh_ctrl_en, (int)cfg.cr_priority,
        (int)cfg.avg_win_t, (int)cfg.avg_win_rh,
        (int)cfg.avg_win_wind,
        (int)cfg.v_max, (int)cfg.wind_prot_en,
        (int)cfg.dir_excl_low, (int)cfg.dir_excl_high,
        (int)cfg.travel_s[0], (int)cfg.travel_s[1], (int)cfg.travel_s[2],
        (int)cfg.dwell_open_min[0], (int)cfg.dwell_open_min[1], (int)cfg.dwell_open_min[2],
        (int)cfg.dwell_close_min[0], (int)cfg.dwell_close_min[1], (int)cfg.dwell_close_min[2],
        (long)cfg.poll_interval_s, (long)cfg.session_timeout_min,
        (long)cfg.ap_timeout_min,
        (long)cfg.lat_deg, (long)cfg.lat_frac,
        (long)cfg.lon_deg, (long)cfg.lon_frac,
        cfg.tz_str, fw_version);

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, body, (n > 0) ? (size_t)n : 0);
    heap_caps_free(body);
    return err;
}

/**
 * @brief HTTP GET /api/config/limits — per-key min/max bounds for input validation.
 *
 * Single source of truth: cfg_limits.h. Stringified at compile time via
 * the `_LIMITS_STR` macro — no runtime overhead, no allocation. The
 * dashboard fetches this once at page load and applies min/max to every
 * `<input>` in the GUI.
 *
 * @param req esp_http_server request handle.
 * @return ESP_OK on response sent.
 * @note Auth requirement: Public.
 * @note Rate limit: none.
 */
static esp_err_t config_limits_handler(httpd_req_t *req)
{
#define _LIMITS_STR2(x) #x
#define _LIMITS_STR(x)  _LIMITS_STR2(x)
    static const char LIMITS_JSON[] =
        "{"
        "\"t_max_day\":"      "[" _LIMITS_STR(CFG_MIN_T_MAX_DAY)    "," _LIMITS_STR(CFG_MAX_T_MAX_DAY)    "],"
        "\"t_min_day\":"      "[" _LIMITS_STR(CFG_MIN_T_MIN_DAY)    "," _LIMITS_STR(CFG_MAX_T_MIN_DAY)    "],"
        "\"t_max_ngt\":"      "[" _LIMITS_STR(CFG_MIN_T_MAX_NGT)    "," _LIMITS_STR(CFG_MAX_T_MAX_NGT)    "],"
        "\"t_min_ngt\":"      "[" _LIMITS_STR(CFG_MIN_T_MIN_NGT)    "," _LIMITS_STR(CFG_MAX_T_MIN_NGT)    "],"
        "\"rh_max_day\":"     "[" _LIMITS_STR(CFG_MIN_RH_MAX)       "," _LIMITS_STR(CFG_MAX_RH_MAX)       "],"
        "\"rh_min_day\":"     "[" _LIMITS_STR(CFG_MIN_RH_MIN)       "," _LIMITS_STR(CFG_MAX_RH_MIN)       "],"
        "\"rh_max_ngt\":"     "[" _LIMITS_STR(CFG_MIN_RH_MAX)       "," _LIMITS_STR(CFG_MAX_RH_MAX)       "],"
        "\"rh_min_ngt\":"     "[" _LIMITS_STR(CFG_MIN_RH_MIN)       "," _LIMITS_STR(CFG_MAX_RH_MIN)       "],"
        "\"hyst_t\":"         "[" _LIMITS_STR(CFG_MIN_HYST_T)       "," _LIMITS_STR(CFG_MAX_HYST_T)       "],"
        "\"hyst_rh\":"        "[" _LIMITS_STR(CFG_MIN_HYST_RH)      "," _LIMITS_STR(CFG_MAX_HYST_RH)      "],"
        "\"avg_win_t\":"      "[" _LIMITS_STR(CFG_MIN_AVG_WIN)      "," _LIMITS_STR(CFG_MAX_AVG_WIN)      "],"
        "\"avg_win_rh\":"     "[" _LIMITS_STR(CFG_MIN_AVG_WIN)      "," _LIMITS_STR(CFG_MAX_AVG_WIN)      "],"
        "\"avg_win_wind\":"   "[" _LIMITS_STR(CFG_MIN_AVG_WIN)      "," _LIMITS_STR(CFG_MAX_AVG_WIN)      "],"
        "\"v_max\":"          "[" _LIMITS_STR(CFG_MIN_V_MAX)        "," _LIMITS_STR(CFG_MAX_V_MAX)        "],"
        "\"dir_excl_low\":"   "[" _LIMITS_STR(CFG_MIN_DIR)          "," _LIMITS_STR(CFG_MAX_DIR)          "],"
        "\"dir_excl_high\":"  "[" _LIMITS_STR(CFG_MIN_DIR)          "," _LIMITS_STR(CFG_MAX_DIR)          "],"
        "\"travel_m1\":"      "[" _LIMITS_STR(CFG_MIN_TRAVEL_S)     "," _LIMITS_STR(CFG_MAX_TRAVEL_S)     "],"
        "\"travel_m2\":"      "[" _LIMITS_STR(CFG_MIN_TRAVEL_S)     "," _LIMITS_STR(CFG_MAX_TRAVEL_S)     "],"
        "\"travel_m3\":"      "[" _LIMITS_STR(CFG_MIN_TRAVEL_S)     "," _LIMITS_STR(CFG_MAX_TRAVEL_S)     "],"
        "\"dwell_open_m1\":"  "[" _LIMITS_STR(CFG_MIN_DWELL_OPEN_S)  "," _LIMITS_STR(CFG_MAX_DWELL_OPEN_S)  "],"
        "\"dwell_open_m2\":"  "[" _LIMITS_STR(CFG_MIN_DWELL_OPEN_S)  "," _LIMITS_STR(CFG_MAX_DWELL_OPEN_S)  "],"
        "\"dwell_open_m3\":"  "[" _LIMITS_STR(CFG_MIN_DWELL_OPEN_S)  "," _LIMITS_STR(CFG_MAX_DWELL_OPEN_S)  "],"
        "\"dwell_close_m1\":" "[" _LIMITS_STR(CFG_MIN_DWELL_CLOSE_S) "," _LIMITS_STR(CFG_MAX_DWELL_CLOSE_S) "],"
        "\"dwell_close_m2\":" "[" _LIMITS_STR(CFG_MIN_DWELL_CLOSE_S) "," _LIMITS_STR(CFG_MAX_DWELL_CLOSE_S) "],"
        "\"dwell_close_m3\":" "[" _LIMITS_STR(CFG_MIN_DWELL_CLOSE_S) "," _LIMITS_STR(CFG_MAX_DWELL_CLOSE_S) "],"
        "\"poll_interval\":"  "[" _LIMITS_STR(CFG_MIN_POLL_S)       "," _LIMITS_STR(CFG_MAX_POLL_S)       "],"
        "\"session_timeout\":" "[" _LIMITS_STR(CFG_MIN_TIMEOUT_MIN)  "," _LIMITS_STR(CFG_MAX_TIMEOUT_MIN)  "],"
        "\"ap_timeout\":"     "[" _LIMITS_STR(CFG_MIN_AP_TIMEOUT)   "," _LIMITS_STR(CFG_MAX_TIMEOUT_MIN)  "]"
        "}";
#undef _LIMITS_STR2
#undef _LIMITS_STR
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, LIMITS_JSON, HTTPD_RESP_USE_STRLEN);
}

/**
 * @brief Read the full HTTP request body into a caller-supplied buffer.
 *
 * Loops on `httpd_req_recv` until `req->content_len` bytes have been
 * received, then NUL-terminates. Rejects bodies that don't fit in `cap-1`
 * bytes; callers should size the buffer for the worst-case JSON payload.
 *
 * @param buf  Destination buffer (`buf[cap-1]` reserved for the NUL).
 * @param cap  Size of `buf` in bytes.
 * @return true if all `content_len` bytes were received; false on overflow,
 *         empty body, or recv failure.
 */
static bool read_request_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) return false;
    buf[0] = '\0';
    int total = (int)req->content_len;
    if (total <= 0 || total >= (int)cap) return false;

    int read_total = 0;
    while (read_total < total) {
        int r = httpd_req_recv(req, buf + read_total, total - read_total);
        if (r <= 0) return false;
        read_total += r;
    }
    buf[read_total] = '\0';
    return true;
}

/**
 * @brief HTTP POST /api/config — write a single setpoint or string field.
 *
 * Body: `{"ns","key","value" | "str_value"}`. Integer writes go via Q4 →
 * T4 → NVS (T4 validates against cfg_limits.h and emits the LOG_SETPOINT
 * audit row). String writes (tz_str) go straight to NVS via
 * `nvs_cfg_set_str`. On tz_str the helper also applies the timezone live
 * so `localtime_r` picks it up.
 *
 * @param req esp_http_server request handle.
 * @return ESP_OK on response sent (200, 400, 403, 503); never returns ESP_FAIL.
 * @note Auth requirement: Farmer if (ns,key) is in FARMER_KEYS /
 *       FARMER_WIND_KEYS, otherwise Admin. Farmer attempting an
 *       admin-key write gets 403.
 * @note Rate limit: none (gated by Q4 depth of 16; queue-full returns 503).
 * @note Audit-logged: integer fields via T4 from Q4 (LOG_SETPOINT, initiator
 *       carried in `config_update_t::initiator = LOG_BY_WEB`); tz_str via
 *       this file's `log_web_setpoint(LOG_PARAM_TZ_STR, 1, 0)`.
 */
static esp_err_t config_post_handler(httpd_req_t *req)
{
    web_session_role_t role = require_auth(req, WEB_ROLE_FARMER);
    if (role == WEB_ROLE_NONE) return ESP_OK;

    char body[256] = {0};
    if (!read_request_body(req, body, sizeof(body))) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req,
            "{\"ok\":false,\"err\":\"bad body\"}", HTTPD_RESP_USE_STRLEN);
    }

    char ns[16] = {}, key[32] = {}, str_value[80] = {}, val_buf[16] = {};
    if (!json_get_field(body, "ns",  ns,  sizeof(ns)) ||
        !json_get_field(body, "key", key, sizeof(key))) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req,
            "{\"ok\":false,\"err\":\"bad request\"}", HTTPD_RESP_USE_STRLEN);
    }
    const bool has_int = json_get_field(body, "value",     val_buf,   sizeof(val_buf));
    const bool has_str = json_get_field(body, "str_value", str_value, sizeof(str_value));

    if (!has_int && !has_str) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req,
            "{\"ok\":false,\"err\":\"no value\"}", HTTPD_RESP_USE_STRLEN);
    }

    /* Farmer-vs-admin policy: farmer can only write farmer-level keys. */
    if (role == WEB_ROLE_FARMER && !is_farmer_key(ns, key)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_send(req,
            "{\"ok\":false,\"err\":\"forbidden\"}", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "application/json");

    if (has_str) {
        (void)nvs_cfg_set_str(ns, key, str_value);
        if (strcmp(key, "tz_str") == 0 && str_value[0] != '\0') {
            setenv("TZ", str_value, 1);
            tzset();
            /* a.6.35.5 — audit row for the TZ change. The actual TZ string
             * isn't logged (value_a=1 = "set"); the param_id identifies
             * the field, the initiator (WEB) identifies the operator path. */
            log_web_setpoint(LOG_PARAM_TZ_STR, 1, 0);
        }
        ESP_LOGI(TAG, "[T11] /api/config %s/%s set str=\"%s\"", ns, key, str_value);
        return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    }

    /* Integer path: post via Q4. Enforce ns/key length so we don't silently
     * truncate into config_update_t.ns/key (16 bytes each, NUL-terminated). */
    if (strlen(ns) >= sizeof(((config_update_t *)0)->ns) ||
        strlen(key) >= sizeof(((config_update_t *)0)->key)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req,
            "{\"ok\":false,\"err\":\"ns/key too long\"}", HTTPD_RESP_USE_STRLEN);
    }

    /* upd.ns/upd.key are 16-byte buffers; src ns/key may be up to 16/32
     * (the strlen check above already rejects oversize). strncpy + explicit
     * NUL terminator avoids the gcc -Wformat-truncation pessimism.
     *
     * a.6.35.5 — set upd.initiator = LOG_BY_WEB so T4's audit row
     * correctly attributes this change to the web GUI. */
    config_update_t upd = {};
    strncpy(upd.ns,  ns,  sizeof(upd.ns)  - 1);
    upd.ns[sizeof(upd.ns)  - 1] = '\0';
    strncpy(upd.key, key, sizeof(upd.key) - 1);
    upd.key[sizeof(upd.key) - 1] = '\0';
    upd.value     = (int32_t)atoi(val_buf);
    upd.initiator = (uint8_t)LOG_BY_WEB;

    if (xQueueSend(Q4, &upd, pdMS_TO_TICKS(500)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req,
            "{\"ok\":false,\"err\":\"Q4 full\"}", HTTPD_RESP_USE_STRLEN);
    }

    ESP_LOGI(TAG, "[T11] /api/config %s/%s set int=%ld → Q4", ns, key, (long)upd.value);
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/**
 * @brief Worker task that delays 1 s then calls esp_restart().
 *
 * Spawned by /api/wifi so the HTTP response can flush before the reset.
 *
 * @param arg Unused.
 * @warning Never returns; calls `esp_restart()` which reboots the chip.
 */
static void wifi_apply_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGW(TAG, "[T11] /api/wifi apply: restarting now");
    esp_restart();
}

/**
 * @brief Inline admin-auth check that sends the appropriate error response on miss.
 *
 * Parses the session cookie, looks up the role, and on failure sends:
 *   - 401 + `{"ok":false,"error":"no_session"}` if no cookie / unknown token.
 *   - 403 + `{"ok":false,"err":"admin only"}` if the session is farmer.
 *
 * @param req esp_http_server request handle.
 * @return true if the request is authenticated as admin (caller proceeds);
 *         false otherwise (caller MUST return ESP_OK — response already sent).
 */
static bool admin_only_or_send_error(httpd_req_t *req)
{
    char token[TOKEN_LEN + 1] = {0};
    if (!cookie_get_session(req, token)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"no_session\"}",
                        HTTPD_RESP_USE_STRLEN);
        return false;
    }
    web_session_role_t role = session_find_and_renew(token);
    if (role != WEB_ROLE_ADMIN) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_send(req, "{\"ok\":false,\"err\":\"admin only\"}",
                        HTTPD_RESP_USE_STRLEN);
        return false;
    }
    return true;
}

/**
 * @brief HTTP POST /api/wifi — change WiFi credentials, scheduled reboot.
 *
 * Body may include any of `{"ssid","psk","ap_psk"}`. Each field provided
 * is written to NVS namespace "wifi". On any STA-cred or AP-cred change,
 * schedules a 1-second-deferred `esp_restart()` (in a dedicated worker
 * task) so T10's NVS-load picks up the new values. The HTTP response
 * flushes before the reset.
 *
 * @param req esp_http_server request handle.
 * @return ESP_OK on response sent.
 * @note Auth requirement: Admin only.
 * @note Rate limit: none (reboot is the natural rate limit).
 * @note Audit-logged: yes — LOG_PARAM_WIFI_SSID/PSK/AP_PSK with value_a=1
 *       ("set"); the actual credentials are NEVER written to the CSV.
 */
static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    if (!admin_only_or_send_error(req)) return ESP_OK;

    char body[256] = {0};
    if (!read_request_body(req, body, sizeof(body))) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req,
            "{\"ok\":false,\"err\":\"bad body\"}", HTTPD_RESP_USE_STRLEN);
    }

    char ssid[64] = {}, psk[64] = {}, ap_psk[64] = {};
    const bool has_ssid   = json_get_field(body, "ssid",   ssid,   sizeof(ssid));
    const bool has_psk    = json_get_field(body, "psk",    psk,    sizeof(psk));
    const bool has_ap_psk = json_get_field(body, "ap_psk", ap_psk, sizeof(ap_psk));

    if (has_ssid)             (void)nvs_cfg_set_str("wifi", "ssid",   ssid);
    if (has_psk && psk[0])    (void)nvs_cfg_set_str("wifi", "psk",    psk);    /* never blank PSK */
    if (has_ap_psk && ap_psk[0]) (void)nvs_cfg_set_str("wifi", "ap_psk", ap_psk);

    /* a.6.35.5 — audit rows for credential changes. value_a=1 indicates
     * "set/changed"; the actual SSID / passphrase is NEVER logged (would
     * make the SD CSV a credential exfil surface). The CSV row stamps
     * who-changed-what-when; the actual new value can be confirmed via
     * the next /api/wifi GET if needed. */
    if (has_ssid)                log_web_setpoint(LOG_PARAM_WIFI_SSID,    1, 0);
    if (has_psk    && psk[0])    log_web_setpoint(LOG_PARAM_WIFI_PSK,     1, 0);
    if (has_ap_psk && ap_psk[0]) log_web_setpoint(LOG_PARAM_WIFI_AP_PSK,  1, 0);

    ESP_LOGI(TAG, "[T11] /api/wifi updated: ssid=%s%s%s",
             has_ssid ? ssid : "(unchanged)",
             (has_psk && psk[0]) ? " psk=***" : "",
             (has_ap_psk && ap_psk[0]) ? " ap_psk=***" : "");

    const bool need_restart = has_ssid || (has_psk && psk[0]) || (has_ap_psk && ap_psk[0]);
    httpd_resp_set_type(req, "application/json");

    if (need_restart) {
        ESP_LOGW(TAG, "[T11] /api/wifi: scheduling restart in 1 s");
        esp_err_t err = httpd_resp_send(req,
            "{\"ok\":true,\"restarting\":true}", HTTPD_RESP_USE_STRLEN);
        xTaskCreate(wifi_apply_restart_task, "wifi-restart",
                    2048, NULL, 1, NULL);
        return err;
    }
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/**
 * @brief HTTP POST /api/pin — change a stored PIN.
 *
 * Body: `{"role":"farmer"|"admin","pin":"NNNN"}`. Calls `pin_auth_set()`
 * which writes the salted SHA-256 hash to NVS (the plaintext PIN never
 * leaves this stack frame).
 *
 * @param req esp_http_server request handle.
 * @return ESP_OK on response sent; 200 + `{"ok":true}` on success, else 200
 *         with `{"ok":false,"err":"..."}`.
 * @note Auth requirement: Admin only (admin may change either PIN).
 * @note Rate limit: none.
 * @note Audit-logged: yes — LOG_PARAM_PIN_FARMER / LOG_PARAM_PIN_ADMIN
 *       with value_a=1 ("changed"); the new PIN is NEVER written to the CSV.
 */
static esp_err_t pin_post_handler(httpd_req_t *req)
{
    if (!admin_only_or_send_error(req)) return ESP_OK;

    char body[128] = {0};
    if (!read_request_body(req, body, sizeof(body))) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req,
            "{\"ok\":false,\"err\":\"bad body\"}", HTTPD_RESP_USE_STRLEN);
    }

    char role_str[12] = {}, pin_str[16] = {};
    json_get_field(body, "role", role_str, sizeof(role_str));
    json_get_field(body, "pin",  pin_str,  sizeof(pin_str));

    pin_role_t pr = (strcmp(role_str, "admin") == 0) ? PIN_ROLE_ADMIN
                                                     : PIN_ROLE_FARMER;
    pin_auth_result_t res = pin_auth_set(pr, pin_str);

    httpd_resp_set_type(req, "application/json");
    if (res == PIN_AUTH_OK) {
        ESP_LOGI(TAG, "[T11] /api/pin: role=%s changed", role_str);
        /* a.6.35.5 — audit row for the PIN change. Critical security event:
         * value_a=1 = "changed"; the new PIN itself is NEVER logged. The
         * separate LOG_PARAM_PIN_FARMER / LOG_PARAM_PIN_ADMIN param ids
         * let the operator filter by role in the parsed CSV. */
        log_web_setpoint(
            (pr == PIN_ROLE_ADMIN) ? LOG_PARAM_PIN_ADMIN : LOG_PARAM_PIN_FARMER,
            1, 0);
        return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    }
    ESP_LOGW(TAG, "[T11] /api/pin: pin_auth_set failed rc=%d", (int)res);
    return httpd_resp_send(req,
        "{\"ok\":false,\"err\":\"pin_auth_set failed\"}", HTTPD_RESP_USE_STRLEN);
}

/* ============================================================
 * Operating-mode route (rc.1.5.0 / gh#28)
 * ============================================================ */

/**
 * @brief HTTP POST /api/mode — set the operating mode.
 *
 * Body: `{"mode":"standby"|"automatic"}`. Accepts either Farmer or Admin
 * session (STANDBY is a routine operational toggle per the gh#28 locked
 * decision; only safety-critical configuration is admin-only).
 *
 * The actual transition is performed by `dm_set_standby()`, which:
 *  - sets/clears EG1_BIT_STANDBY,
 *  - persists the new state to NVS (`system/mode_standby`),
 *  - emits a LOG_MODE_CHANGE audit row (initiator = LOG_BY_WEB,
 *    channel = 0 [web surface], value_a = 1 enter / 0 leave),
 *  - on STANDBY exit, posts CMD_RECALIBRATE to Q1 so T2 re-runs the
 *    synchronous CLOSE_ALL sweep (visible to the operator as
 *    "Mode: Window Cal." on the LCD until calibration completes).
 *
 * @param req esp_http_server request handle.
 * @return ESP_OK on response sent (200 / 400 / 401); ESP_FAIL on recv error.
 *         - 200 + `{"ok":true,"mode":"standby"|"automatic"}` on success.
 *         - 400 on bad payload.
 * @note Auth requirement: Farmer or Admin.
 * @note Rate limit: none.
 * @note Audit-logged: yes, by dm_set_standby() via LOG_MODE_CHANGE.
 */
static esp_err_t mode_post_handler(httpd_req_t *req)
{
    web_session_role_t role = require_auth(req, WEB_ROLE_FARMER);
    if (role == WEB_ROLE_NONE) return ESP_OK;   /* require_auth already sent 401 */

    /* Tiny payload — `{"mode":"automatic"}` is ~22 bytes. 64 is plenty. */
    char body[64] = {0};
    int total = (int)req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"bad_body\"}", HTTPD_RESP_USE_STRLEN);
    }
    int read_total = 0;
    while (read_total < total) {
        int r = httpd_req_recv(req, body + read_total, total - read_total);
        if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
        read_total += r;
    }
    body[read_total] = '\0';

    char mode_str[16] = {0};
    if (!json_get_field(body, "mode", mode_str, sizeof(mode_str))) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"missing_mode\"}", HTTPD_RESP_USE_STRLEN);
    }

    bool want_standby;
    if      (strcmp(mode_str, "standby")   == 0) want_standby = true;
    else if (strcmp(mode_str, "automatic") == 0) want_standby = false;
    else {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"bad_mode\"}", HTTPD_RESP_USE_STRLEN);
    }

    /* Initiator = LOG_BY_WEB (web surface). channel = 0 (web). The role
     * (farmer vs admin) is not preserved in the log row — the design
     * locks STANDBY as accepted from both, so a single LOG_BY_WEB row
     * captures the surface; surface vs role distinction lives in the
     * session-event audit trail. */
    dm_set_standby(want_standby, LOG_BY_WEB, 0u);

    /* Reply with the new (or unchanged, on idempotent calls) state. */
    char resp[64];
    int n = snprintf(resp, sizeof(resp),
                     "{\"ok\":true,\"mode\":\"%s\"}",
                     dm_get_standby() ? "standby" : "automatic");
    httpd_resp_set_type(req, "application/json");
    ESP_LOGI(TAG, "[T11] /api/mode role=%s -> %s",
             (role == WEB_ROLE_ADMIN) ? "admin" : "farmer",
             dm_get_standby() ? "standby" : "automatic");
    return httpd_resp_send(req, resp, (n > 0) ? (size_t)n : 0);
}

/* ============================================================
 * SD-card + log routes (alpha.6.19 / Phase 6.16-ε)
 *
 * GET  /api/sd/status      — {mounted, free_mb, size_mb} (PUBLIC)
 * POST /api/sd/mount       — re-mount via event_logger_sd_remount (admin)
 * POST /api/sd/unmount     — flush + unmount via event_logger_sd_unmount (admin)
 * GET  /api/log/files      — {sd_files:[...]} listing of .csv files (admin)
 * GET  /api/log/download   — ?file=NAME stream a CSV from SD (admin)
 *
 * SD lifecycle note: T9 (event_logger) owns the SD mount under normal
 * operation — it keeps the card mounted continuously and rotates the
 * daily CSV file. /api/sd/{mount,unmount} go through event_logger_sd_*
 * so T9's internal state stays consistent with the operator's actions.
 * ============================================================ */

/**
 * @brief HTTP GET /api/sd/status — SD card mount state and capacity.
 *
 * @param req esp_http_server request handle.
 * @return ESP_OK on response sent — 200 + `{"mounted":bool,"free_mb":N,"size_mb":N}`.
 * @note Auth requirement: Public (status only; mount/unmount are admin-gated).
 * @note Rate limit: none.
 */
static esp_err_t sd_status_handler(httpd_req_t *req)
{
    const bool mounted = storage_sd_available();
    const uint64_t total = mounted ? storage_sd_total_bytes() : 0;
    const uint64_t freeb = mounted ? storage_sd_free_bytes()  : 0;
    char body[128];
    int n = snprintf(body, sizeof(body),
        "{\"mounted\":%s,\"free_mb\":%lu,\"size_mb\":%lu}",
        mounted ? "true" : "false",
        (unsigned long)(freeb / (1024UL * 1024UL)),
        (unsigned long)(total / (1024UL * 1024UL)));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, (n > 0) ? (size_t)n : 0);
}

/**
 * @brief HTTP POST /api/sd/mount — remount the SD card via T9.
 *
 * Forwards to `event_logger_sd_remount()` so T9's internal state stays
 * consistent with the operator's action.
 *
 * @param req esp_http_server request handle.
 * @return ESP_OK on response sent — 200 `{"ok":true}` or `{"ok":false,...}` on failure.
 * @note Auth requirement: Admin only.
 * @note Rate limit: none.
 * @note Audit-logged: no (state is reflected in subsequent /api/sd/status calls).
 */
static esp_err_t sd_mount_handler(httpd_req_t *req)
{
    if (!admin_only_or_send_error(req)) return ESP_OK;
    bool ok = event_logger_sd_remount();
    httpd_resp_set_type(req, "application/json");
    if (ok) {
        ESP_LOGI(TAG, "[T11] /api/sd/mount: remount OK");
        return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    }
    ESP_LOGW(TAG, "[T11] /api/sd/mount: remount FAILED");
    return httpd_resp_send(req,
        "{\"ok\":false,\"err\":\"mount failed\"}", HTTPD_RESP_USE_STRLEN);
}

/**
 * @brief HTTP POST /api/sd/unmount — flush and unmount the SD card via T9.
 *
 * Forwards to `event_logger_sd_unmount()` so T9 closes the active CSV
 * file cleanly before the unmount.
 *
 * @param req esp_http_server request handle.
 * @return ESP_OK on response sent — 200 `{"ok":true}`.
 * @note Auth requirement: Admin only.
 * @note Rate limit: none.
 * @note Audit-logged: no.
 */
static esp_err_t sd_unmount_handler(httpd_req_t *req)
{
    if (!admin_only_or_send_error(req)) return ESP_OK;
    event_logger_sd_unmount();
    ESP_LOGI(TAG, "[T11] /api/sd/unmount: unmounted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/**
 * @brief HTTP GET /api/log/files — list .csv files on SD, sorted chronologically.
 *
 * Filenames follow YYYYMMDDHHMMSS.csv from T9, so lexicographic sort =
 * chronological order. Includes any imported names too (e.g. 1.20.3-era
 * log_YYYYMMDD_HHMMSS.csv) — the sort order is approximately right for
 * those. The NVS-ringbuffer log source was retired in alpha.6.5; SD is the
 * only source.
 *
 * @param req esp_http_server request handle.
 * @return ESP_OK on response sent; ESP_FAIL on heap alloc failure.
 * @note Auth requirement: Admin only.
 * @note Rate limit: none (capped at SD_MAX_FILES names in the response).
 */
static esp_err_t log_files_handler(httpd_req_t *req)
{
    if (!admin_only_or_send_error(req)) return ESP_OK;

    /* List buffer for storage_sd_list_csv (comma-separated string). */
    const size_t LIST_LEN = SD_LIST_BUF_LEN;
    char *list_buf = (char *)heap_caps_malloc(LIST_LEN, MALLOC_CAP_INTERNAL);
    if (list_buf == NULL) { httpd_resp_send_500(req); return ESP_FAIL; }
    list_buf[0] = '\0';
    if (storage_sd_available()) {
        (void)storage_sd_list_csv(".csv", list_buf, LIST_LEN);
    }

    /* Tokenize → fixed-size name array. */
    enum { LOG_FILES_MAX = (int)SD_MAX_FILES, LOG_FNAME_MAX = SD_NAME_ONLY_LEN };
    char names[LOG_FILES_MAX][LOG_FNAME_MAX] = {};
    int n_names = 0;
    char *save = NULL;
    char *tok = strtok_r(list_buf, ",", &save);
    while (tok && n_names < LOG_FILES_MAX) {
        while (*tok == ' ') tok++;
        if (*tok) {
            strncpy(names[n_names], tok, LOG_FNAME_MAX - 1);
            names[n_names][LOG_FNAME_MAX - 1] = '\0';
            n_names++;
        }
        tok = strtok_r(NULL, ",", &save);
    }
    /* Bubble sort — n ≤ 12, negligible. */
    for (int i = 0; i < n_names - 1; i++) {
        for (int j = 0; j < n_names - 1 - i; j++) {
            if (strcmp(names[j], names[j + 1]) > 0) {
                char tmp[LOG_FNAME_MAX];
                memcpy(tmp,           names[j],     LOG_FNAME_MAX);
                memcpy(names[j],      names[j + 1], LOG_FNAME_MAX);
                memcpy(names[j + 1],  tmp,          LOG_FNAME_MAX);
            }
        }
    }

    /* Build the JSON response. */
    const size_t OUT_LEN = 1024u;
    char *out = (char *)heap_caps_malloc(OUT_LEN, MALLOC_CAP_INTERNAL);
    if (out == NULL) { heap_caps_free(list_buf); httpd_resp_send_500(req); return ESP_FAIL; }

    int pos = snprintf(out, OUT_LEN, "{\"sd_files\":[");
    for (int i = 0; i < n_names && (size_t)pos < OUT_LEN - 32u; i++) {
        int w = snprintf(out + pos, OUT_LEN - (size_t)pos,
                         "%s\"%s\"", (i > 0) ? "," : "", names[i]);
        if (w < 0) break;
        pos += w;
    }
    int wTail = snprintf(out + pos, OUT_LEN - (size_t)pos, "]}");
    if (wTail > 0) pos += wTail;

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, out, (size_t)pos);
    heap_caps_free(list_buf);
    heap_caps_free(out);
    return err;
}

/**
 * @brief HTTP GET /api/log/download?file=NAME — stream a single CSV file.
 *
 * Sends `Content-Disposition: attachment; filename="NAME"`. Rejects path-
 * traversal attempts (any '/' or "..") and requires the `file` query
 * param. PSRAM-allocates the whole file (CSV files are typically < 100 KB;
 * with 8 MB PSRAM that's comfortable).
 *
 * @param req esp_http_server request handle.
 * @return ESP_OK on response sent (200, 400, 404, 503); ESP_FAIL on PSRAM
 *         alloc or read failure.
 * @note Auth requirement: Admin only.
 * @note Rate limit: none.
 */
static esp_err_t log_download_handler(httpd_req_t *req)
{
    if (!admin_only_or_send_error(req)) return ESP_OK;

    /* ?file= query string */
    char query[64] = {0};
    char fname[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "file", fname, sizeof(fname)) != ESP_OK ||
        fname[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req,
            "{\"ok\":false,\"err\":\"missing file param\"}", HTTPD_RESP_USE_STRLEN);
    }

    /* Reject path traversal */
    if (strchr(fname, '/') || strstr(fname, "..")) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req,
            "{\"ok\":false,\"err\":\"bad filename\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (!storage_sd_available()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req,
            "{\"ok\":false,\"err\":\"SD not mounted\"}", HTTPD_RESP_USE_STRLEN);
    }

    char abs_path[48];
    snprintf(abs_path, sizeof(abs_path), "/%s", fname);
    uint32_t fsize = storage_sd_file_size(abs_path);
    if (fsize == 0u) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_send(req,
            "{\"ok\":false,\"err\":\"not found or empty\"}", HTTPD_RESP_USE_STRLEN);
    }

    /* Allocate from PSRAM — files can be 10s-100s of KB. */
    char *buf = (char *)heap_caps_malloc((size_t)fsize + 1u, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        ESP_LOGE(TAG, "[T11] /api/log/download: PSRAM alloc(%lu) failed",
                 (unsigned long)fsize + 1u);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t got = 0;
    storage_status_t st = storage_sd_read(abs_path, 0, buf,
                                          (size_t)fsize + 1u, &got);
    if (st != STORAGE_OK) {
        ESP_LOGW(TAG, "[T11] /api/log/download: storage_sd_read failed st=%d", (int)st);
        heap_caps_free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[got] = '\0';

    /* Caller-owned hdr buffer survives until httpd_resp_send returns. */
    char disp[80];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", fname);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_type(req, "text/csv");

    esp_err_t err = httpd_resp_send(req, buf, got);
    heap_caps_free(buf);
    ESP_LOGI(TAG, "[T11] /api/log/download(%s) sent %u bytes", fname, (unsigned)got);
    return err;
}

/* ============================================================
 * /api/coredump — Pre-soak post-mortem retrieval (a.6.35.6)
 *
 * Three admin-only endpoints expose the IDF coredump partition through the
 * GUI so an operator can grab and decode a panic dump from anywhere on the
 * LAN without physically connecting to the unit.
 *
 *   GET  /api/coredump/status    {"present": bool, "size_bytes": N,
 *                                  "size_kb": N, "version": "..."}
 *   GET  /api/coredump/download  application/octet-stream, raw partition bytes
 *                                Content-Disposition: attachment with a
 *                                versioned filename so the operator's
 *                                Downloads folder ends up with a clearly-
 *                                named .bin per panic.
 *   POST /api/coredump/erase     {"ok": true} after esp_core_dump_image_erase
 *                                Operator confirms a successful decode first.
 *
 * Security envelope:
 *   - admin_only_or_send_error() — same session/role gate as /api/wifi etc.
 *   - LOG_SYSTEM audit row on every download (value_a=19) and erase
 *     (value_a=20). The operator can grep the SD CSV for unexpected access
 *     after the fact.
 *   - Rate limit: at most one coredump operation per
 *     COREDUMP_RATE_LIMIT_MS (10 s). A captured admin cookie can't be used
 *     to scrape the partition repeatedly without triggering 429 responses.
 *   - Confirm-then-erase: download does NOT auto-erase. Operator decides
 *     to erase only after confirming the decode succeeded offline.
 *   - Not surfaced via T14 status POST — the canonical JSON only emits
 *     the `coredump_available` mode flag, never the partition contents.
 *
 * Coredump may contain RAM snapshots that include WiFi creds / PINs / the
 * status-website shared secret if those were in-flight at panic time. The
 * admin-only gate + per-LAN deployment is the operational boundary.
 * ============================================================ */

/** @brief Minimum interval (ms) between any two /api/coredump operations. */
#define COREDUMP_RATE_LIMIT_MS  10000u
/** @brief Streaming chunk size for /api/coredump/download (bytes). */
#define COREDUMP_CHUNK_BYTES    4096u

/** @brief Rate-limit timestamp shared by /api/coredump/download + /erase. Single int64 = atomic 64-bit load/store on ESP32. */
static int64_t s_coredump_last_access_us = 0;

/**
 * @brief Enforce the 10 s rate limit on /api/coredump operations.
 *
 * On violation: sends 429 Too Many Requests + Retry-After: 10 + a JSON
 * error body, and the caller MUST return ESP_OK (response already sent).
 * On success: records the current timestamp so the next call sees the limit.
 *
 * @param req esp_http_server request handle.
 * @return true if the caller may proceed; false on rate-limit miss
 *         (429 already sent — caller returns ESP_OK).
 */
static bool coredump_rate_limit_ok(httpd_req_t *req)
{
    const int64_t now_us = esp_timer_get_time();
    if (s_coredump_last_access_us != 0 &&
        (now_us - s_coredump_last_access_us) < (int64_t)(COREDUMP_RATE_LIMIT_MS * 1000)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_set_hdr(req, "Retry-After", "10");
        (void)httpd_resp_send(req,
            "{\"ok\":false,\"err\":\"rate limited — one /api/coredump op per 10 s\"}",
            HTTPD_RESP_USE_STRLEN);
        return false;
    }
    s_coredump_last_access_us = now_us;
    return true;
}

/**
 * @brief Emit a LOG_SYSTEM audit row for a coredump operation.
 *
 * @param value_a 19 = downloaded, 20 = erased.
 * @param value_b For downloads, approximate size in 256-byte units (clamped
 *                to int16). For erases, 0.
 */
static void coredump_audit(int16_t value_a, int16_t value_b)
{
    log_event_t ev = {};
    ev.timestamp  = (uint32_t)time(NULL);
    ev.event_type = (uint8_t)LOG_SYSTEM;
    ev.initiator  = (uint8_t)LOG_BY_WEB;
    ev.value_a    = value_a;   /* 19 = downloaded, 20 = erased */
    ev.value_b    = value_b;
    log_post(&ev);
}

/**
 * @brief HTTP GET /api/coredump/status — JSON status of the stored coredump.
 *
 * Useful even without `present=true` — the GUI polls it on the Log tab to
 * display "no coredump stored" vs "coredump available (N bytes)". No flash
 * read other than the cheap cached state from T4.
 *
 * @param req esp_http_server request handle.
 * @return ESP_OK on response sent — 200 + `{"present":bool,"size_bytes":N,...}`.
 * @note Auth requirement: Admin only.
 * @note Rate limit: none (cheap cached read).
 */
static esp_err_t coredump_status_handler(httpd_req_t *req)
{
    if (!admin_only_or_send_error(req)) return ESP_OK;

    const bool   present = dm_coredump_present();
    const size_t bytes   = dm_coredump_size_bytes();

    char body[160];
    int n = snprintf(body, sizeof(body),
        "{\"ok\":true,\"present\":%s,\"size_bytes\":%u,\"size_kb\":%u,"
         "\"fw_ver\":\"" FIRMWARE_VERSION "\"}",
        present ? "true" : "false",
        (unsigned)bytes,
        (unsigned)((bytes + 1023u) / 1024u));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, (n > 0) ? (size_t)n : 0);
}

/**
 * @brief HTTP GET /api/coredump/download — stream the partition contents.
 *
 * Returns 404 when no coredump is present. Otherwise reads the coredump
 * partition in 4 KB chunks and writes them out as application/octet-stream
 * with a Content-Disposition that names the file:
 * `coredump-<FIRMWARE_VERSION>-<unix_ts>.bin`. Operator decodes offline with
 * `idf.py coredump-info -t raw -c <file> bin/<ver>/firmware-<ver>.elf`.
 *
 * Does NOT auto-erase — operator confirms a successful decode before
 * calling POST /api/coredump/erase.
 *
 * @param req esp_http_server request handle.
 * @return ESP_OK on response sent; ESP_FAIL on heap alloc or partition_read failure.
 * @note Auth requirement: Admin only.
 * @note Rate limit: 1 op / 10 s via `coredump_rate_limit_ok()`; 429 + Retry-After
 *       on violation. Shared with /api/coredump/erase via `s_coredump_last_access_us`.
 * @note Audit-logged via LOG_SYSTEM value_a=19 on success
 *       (value_b = size/256 clamped to int16).
 * @warning The dumped image may contain RAM snapshots that include WiFi creds /
 *          PINs / the status-website shared secret if those were in-flight at
 *          panic time. The admin-only gate is the operational boundary.
 */
static esp_err_t coredump_download_handler(httpd_req_t *req)
{
    if (!admin_only_or_send_error(req)) return ESP_OK;
    if (!coredump_rate_limit_ok(req))   return ESP_OK;

    if (!dm_coredump_present()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_send(req,
            "{\"ok\":false,\"err\":\"no coredump stored\"}",
            HTTPD_RESP_USE_STRLEN);
    }

    /* Look up the coredump partition. esp_core_dump_image_get gives an
     * absolute flash address; convert to a partition-relative offset so
     * we can use esp_partition_read (which bounds-checks against the
     * partition size). */
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (p == NULL) {
        ESP_LOGE(TAG, "[T11] /api/coredump/download: no coredump partition");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t cd_addr = 0u, cd_size = 0u;
    esp_err_t e = esp_core_dump_image_get(&cd_addr, &cd_size);
    if (e != ESP_OK || cd_size == 0u) {
        ESP_LOGE(TAG, "[T11] /api/coredump/download: image_get failed (%d)", (int)e);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    if (cd_addr < p->address || (cd_addr + cd_size) > (p->address + p->size)) {
        ESP_LOGE(TAG, "[T11] /api/coredump/download: image spans outside partition");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    const size_t part_off = cd_addr - p->address;

    /* Versioned + timestamped filename so the operator's Downloads folder
     * disambiguates dumps from multiple panic cycles. */
    char fname[64];
    snprintf(fname, sizeof(fname),
             "coredump-" FIRMWARE_VERSION "-%lu.bin",
             (unsigned long)time(NULL));
    char disp[96];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", fname);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_type(req, "application/octet-stream");

    /* Stream in 4 KB chunks. Allocated from INTERNAL heap so we don't
     * exercise PSRAM for a one-shot ~tens-of-KB transfer. */
    uint8_t *chunk = (uint8_t *)heap_caps_malloc(COREDUMP_CHUNK_BYTES, MALLOC_CAP_INTERNAL);
    if (chunk == NULL) {
        ESP_LOGE(TAG, "[T11] /api/coredump/download: chunk alloc failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t sent = 0u;
    while (sent < cd_size) {
        const size_t want = (cd_size - sent > COREDUMP_CHUNK_BYTES)
                              ? COREDUMP_CHUNK_BYTES : (cd_size - sent);
        if (esp_partition_read(p, part_off + sent, chunk, want) != ESP_OK) {
            ESP_LOGE(TAG, "[T11] /api/coredump/download: partition_read failed at %u",
                     (unsigned)(part_off + sent));
            heap_caps_free(chunk);
            return ESP_FAIL;   /* socket already partially written; just bail */
        }
        if (httpd_resp_send_chunk(req, (const char *)chunk, want) != ESP_OK) {
            ESP_LOGW(TAG, "[T11] /api/coredump/download: send_chunk failed at %u",
                     (unsigned)sent);
            heap_caps_free(chunk);
            return ESP_FAIL;
        }
        sent += want;
    }
    /* End chunked response. */
    (void)httpd_resp_send_chunk(req, NULL, 0);
    heap_caps_free(chunk);

    /* Audit row: value_a=19, value_b = bytes/256 clamped to int16 so the
     * parser can render an approximate size without losing precision on
     * typical 5-30 KB dumps (5 KB → 20, 30 KB → 120). */
    int16_t vb = (int16_t)((cd_size + 255u) / 256u);
    if (vb < 0) vb = 32767;
    coredump_audit(19, vb);

    ESP_LOGI(TAG, "[T11] /api/coredump/download: %u bytes streamed", (unsigned)sent);
    return ESP_OK;
}

/**
 * @brief HTTP POST /api/coredump/erase — wipe the coredump partition.
 *
 * Operator confirms a successful decode offline first, then clicks Erase
 * in the GUI. `esp_core_dump_image_erase()` clears the partition; T4's
 * cached state is dropped via `dm_coredump_clear()` so the next status
 * snapshot omits the `coredump_available` mode flag and the GUI badge
 * disappears.
 *
 * @param req esp_http_server request handle.
 * @return ESP_OK on response sent (idempotent — 200 even when no coredump existed).
 * @note Auth requirement: Admin only.
 * @note Rate limit: 1 op / 10 s via `coredump_rate_limit_ok()` (shared with
 *       /api/coredump/download).
 * @note Audit-logged via LOG_SYSTEM value_a=20 on every successful erase.
 */
static esp_err_t coredump_erase_handler(httpd_req_t *req)
{
    if (!admin_only_or_send_error(req)) return ESP_OK;
    if (!coredump_rate_limit_ok(req))   return ESP_OK;

    httpd_resp_set_type(req, "application/json");

    /* Idempotent: no coredump → 200 OK, nothing to do. Operator clicking
     * the button on a clean unit is benign. */
    if (!dm_coredump_present()) {
        return httpd_resp_send(req,
            "{\"ok\":true,\"note\":\"no coredump to erase\"}",
            HTTPD_RESP_USE_STRLEN);
    }

    esp_err_t e = esp_core_dump_image_erase();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "[T11] /api/coredump/erase: image_erase failed (%d)", (int)e);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req,
            "{\"ok\":false,\"err\":\"image_erase failed\"}",
            HTTPD_RESP_USE_STRLEN);
    }

    dm_coredump_clear();
    coredump_audit(20, 0);   /* value_a=20 = coredump erased by admin */
    ESP_LOGI(TAG, "[T11] /api/coredump/erase: partition cleared");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* ============================================================
 * OTA + web-tab routes (alpha.6.20 / Phase 6.16-ζ)
 *
 * GET  /api/ota/status     — OTA state machine (auth required)
 * POST /api/ota/firmware   — admin; raw .bin upload, streams to T13 OTA
 * POST /api/ota/assets     — admin; STORE-only ZIP, PSRAM accum, T13 extract
 * GET  /api/web            — admin; web-tab settings (status URL, interval, ...)
 * POST /api/web            — admin; validates + writes web-tab settings; T4 reload
 *
 * esp_http_server runs each handler once per request — the chunked-upload
 * pattern is `httpd_req_recv` in a loop until `content_len` bytes read.
 * ============================================================ */

/**
 * @brief HTTP GET /api/ota/status — current OTA state machine snapshot.
 *
 * Reports the OTA state, progress percentage, last error string, active
 * bank ('A'/'B') and whether the running image has been accepted yet.
 *
 * @param req esp_http_server request handle.
 * @return ESP_OK on response sent.
 * @note Auth requirement: Farmer or Admin.
 * @note Rate limit: none.
 */
static esp_err_t ota_status_handler(httpd_req_t *req)
{
    if (require_auth(req, WEB_ROLE_FARMER) == WEB_ROLE_NONE) return ESP_OK;

    static const char * const STATE_NAMES[] = {
        "idle", "fw_writing", "fw_verifying",
        "assets_buffering", "assets_writing",
        "rebooting", "error", "fw_done"
    };
    ota_state_t st   = ota_get_state();
    uint8_t     pct  = ota_get_progress_pct();
    const char *err  = ota_get_error();
    char        bank = ota_get_active_bank();
    bool        acc  = ota_is_accepted();
    const char *sname = ((unsigned)st < 8) ? STATE_NAMES[st] : "unknown";

    char body[256];
    int n = snprintf(body, sizeof(body),
        "{\"ok\":true,\"state\":\"%s\",\"progress\":%u,\"error\":\"%s\","
        "\"bank\":\"%c\",\"accepted\":%s}",
        sname, (unsigned)pct, err ? err : "",
        bank, acc ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, (n > 0) ? (size_t)n : 0);
}

/**
 * @brief HTTP POST /api/ota/firmware — stream a firmware .bin to T13.
 *
 * Receives the .bin body in chunks via `httpd_req_recv`; each chunk is fed
 * straight into `ota_firmware_write()`. content_len is required
 * (Content-Length header) so T13 can pre-validate the image size against
 * the inactive bank.
 *
 * @param req esp_http_server request handle.
 * @return ESP_OK on success — 200 + `{"ok":true,"awaiting_assets":true}`;
 *         ESP_FAIL on recv/write/end failure.
 * @note Auth requirement: Admin only.
 * @note Rate limit: none (OTA mutex inside ota_manager serialises).
 * @note Audit-logged: T13 emits its own LOG_SYSTEM rows for OTA milestones.
 */
static esp_err_t ota_firmware_post_handler(httpd_req_t *req)
{
    if (!admin_only_or_send_error(req)) return ESP_OK;

    const size_t total = (size_t)req->content_len;
    if (total == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req,
            "{\"ok\":false,\"err\":\"Content-Length required\"}",
            HTTPD_RESP_USE_STRLEN);
    }

    if (!ota_firmware_begin(total)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* 4 KB chunk buffer — large enough that we don't thrash httpd_req_recv,
     * small enough that it lives on the stack without bloating the httpd
     * task. Read in a loop until total bytes consumed. */
    uint8_t buf[4096];
    size_t received = 0;
    while (received < total) {
        int want = (int)((total - received) > sizeof(buf)
                         ? sizeof(buf) : (total - received));
        int n = httpd_req_recv(req, (char *)buf, (size_t)want);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "[T11] /api/ota/firmware: recv failed at %u/%u",
                     (unsigned)received, (unsigned)total);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        if (!ota_firmware_write(buf, (size_t)n)) {
            ESP_LOGE(TAG, "[T11] /api/ota/firmware: write failed at %u/%u",
                     (unsigned)received, (unsigned)total);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req,
                "{\"ok\":false,\"err\":\"OTA write failed\"}",
                HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }
        received += (size_t)n;
    }

    if (!ota_firmware_end()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req,
            "{\"ok\":false,\"err\":\"OTA verify failed\"}",
            HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "[T11] /api/ota/firmware: OK %u bytes; awaiting assets ZIP",
             (unsigned)total);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req,
        "{\"ok\":true,\"rebooting\":false,\"awaiting_assets\":true}",
        HTTPD_RESP_USE_STRLEN);
}

/**
 * @brief HTTP POST /api/ota/assets — stream a STORE-only ZIP of web assets.
 *
 * Accumulates the body into the T13 PSRAM buffer via
 * `ota_assets_accumulate(data, len, offset)`. On the last chunk calls
 * `ota_assets_end()` which spawns T13 to extract to inactive LittleFS.
 *
 * @param req esp_http_server request handle.
 * @return ESP_OK on success — 202 + `{"ok":true,"message":"extracting..."}`;
 *         ESP_FAIL on recv/accumulate/end failure.
 * @note Auth requirement: Admin only.
 * @note Rate limit: none.
 */
static esp_err_t ota_assets_post_handler(httpd_req_t *req)
{
    if (!admin_only_or_send_error(req)) return ESP_OK;

    const size_t total = (size_t)req->content_len;
    if (total == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req,
            "{\"ok\":false,\"err\":\"Content-Length required\"}",
            HTTPD_RESP_USE_STRLEN);
    }

    if (!ota_assets_begin(total)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    uint8_t buf[4096];
    size_t received = 0;
    while (received < total) {
        int want = (int)((total - received) > sizeof(buf)
                         ? sizeof(buf) : (total - received));
        int n = httpd_req_recv(req, (char *)buf, (size_t)want);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "[T11] /api/ota/assets: recv failed at %u/%u",
                     (unsigned)received, (unsigned)total);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        if (!ota_assets_accumulate(buf, (size_t)n, received)) {
            ESP_LOGE(TAG, "[T11] /api/ota/assets: accumulate failed at %u",
                     (unsigned)received);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req,
                "{\"ok\":false,\"err\":\"accumulate failed\"}",
                HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }
        received += (size_t)n;
    }

    if (!ota_assets_end()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req,
            "{\"ok\":false,\"err\":\"assets spawn failed\"}",
            HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "[T11] /api/ota/assets: %u bytes accumulated; T13 extracting",
             (unsigned)total);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_send(req,
        "{\"ok\":true,\"message\":\"extracting — poll GET /api/ota/status\"}",
        HTTPD_RESP_USE_STRLEN);
}

/**
 * @brief HTTP GET /api/web — read web-tab settings + last upload attempts.
 *
 * Returns the status-website URL, interval, enable flag, expose bitmask,
 * scheduled log-upload time, and the last "[OK at ts]" / "[err ...]"
 * strings from T14 and the log-uploader.
 *
 * @param req esp_http_server request handle.
 * @return ESP_OK on response sent.
 * @note Auth requirement: Admin only.
 * @note Rate limit: none.
 * @note Secret is intentionally NOT echoed (write-only from the UI; the
 *       input stays blank and "empty=keep" on POST).
 */
static esp_err_t web_get_handler(httpd_req_t *req)
{
    if (!admin_only_or_send_error(req)) return ESP_OK;

    cfg_shadow_t cfg = {};
    dm_cfg_snapshot(&cfg);

    char last_post[48] = {};
    char last_log[48]  = {};
    status_post_last_str(last_post, sizeof(last_post));
    status_post_last_log_str(last_log, sizeof(last_log));

    char body[640];
    int n = snprintf(body, sizeof(body),
        "{\"url\":\"%s\","
         "\"interval_s\":%ld,"
         "\"enable\":%ld,"
         "\"expose\":%ld,"
         "\"log_h\":%ld,"
         "\"log_m\":%ld,"
         "\"log_rot\":%ld,"
         "\"last_post\":\"%s\","
         "\"last_log_up\":\"%s\","
         "\"log_last_up\":\"%s\"}",
        cfg.status_url,
        (long)cfg.status_interval_s,
        (long)cfg.status_enable,
        (long)cfg.status_expose,
        (long)cfg.log_upload_h,
        (long)cfg.log_upload_m,
        (long)cfg.log_upload_rot,
        last_post, last_log, cfg.log_last_up);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, (n > 0) ? (size_t)n : 0);
}

/**
 * @brief HTTP POST /api/web — apply web-tab settings (single transaction).
 *
 * Single-transaction Apply with bounds-check before any NVS write, then
 * `dm_reload_web_cfg()` so the cfg shadow refreshes synchronously (T4
 * publishes under MX4 before this returns).
 *
 * URL validation: must use `https://` (a.6.35 — plain HTTP rejected so the
 * sourceidentifier shared secret cannot leak on the wire), must NOT
 * contain `?` or `#`, must end with `api.php` (T14 appends ?action=log
 * itself; HTTPClient followed redirects silently which masked routing bugs).
 *
 * @param req esp_http_server request handle.
 * @return ESP_OK on response sent (200, 400); 400 includes a human-readable
 *         error string identifying the failed field.
 * @note Auth requirement: Admin only.
 * @note Rate limit: none.
 * @note Audit-logged: yes — per-field via `log_web_setpoint()`. Sensitive
 *       fields (URL, secret) log only "set" (value_a=1); numeric fields
 *       log old → new. No-op Apply (all fields unchanged) emits zero rows.
 */
static esp_err_t web_post_handler(httpd_req_t *req)
{
    if (!admin_only_or_send_error(req)) return ESP_OK;

    char body[640] = {0};
    if (!read_request_body(req, body, sizeof(body))) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req,
            "{\"ok\":false,\"err\":\"bad body\"}", HTTPD_RESP_USE_STRLEN);
    }

    char url[CFG_MAX_URL_LEN + 1]      = {};
    char secret[CFG_MAX_SECRET_LEN + 1] = {};
    char vbuf[16] = {};
    const bool h_url = json_get_field(body, "url",    url,    sizeof(url));
    const bool h_sec = json_get_field(body, "secret", secret, sizeof(secret));
    int32_t interval = 0, enable = 0, expose = 0;
    int32_t log_h = 0, log_m = 0, log_rot = 0;
    const bool h_iv = json_get_field(body, "interval_s", vbuf, sizeof(vbuf));
    if (h_iv) interval = (int32_t)atoi(vbuf);
    const bool h_en = json_get_field(body, "enable", vbuf, sizeof(vbuf));
    if (h_en) enable  = (int32_t)atoi(vbuf);
    const bool h_ex = json_get_field(body, "expose", vbuf, sizeof(vbuf));
    if (h_ex) expose  = (int32_t)atoi(vbuf);
    const bool h_lh = json_get_field(body, "log_h",  vbuf, sizeof(vbuf));
    if (h_lh) log_h   = (int32_t)atoi(vbuf);
    const bool h_lm = json_get_field(body, "log_m",  vbuf, sizeof(vbuf));
    if (h_lm) log_m   = (int32_t)atoi(vbuf);
    const bool h_lr = json_get_field(body, "log_rot", vbuf, sizeof(vbuf));
    if (h_lr) log_rot = (int32_t)atoi(vbuf);

    httpd_resp_set_type(req, "application/json");

    /* URL validation.
     *
     * a.6.35 (item G): https-only. Plain HTTP exposes the `sourceidentifier`
     * shared secret on the wire — once T14 starts attaching it as a header
     * (item A in this same alpha), an HTTP endpoint becomes a credential
     * leak. Reject http:// at the validator so an operator can't accidentally
     * configure one via the GUI. */
    if (h_url && url[0] != '\0') {
        if (strncmp(url, "https://", 8) != 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_send(req,
                "{\"ok\":false,\"err\":\"URL must use https:// — plain HTTP exposes the shared secret on the wire\"}",
                HTTPD_RESP_USE_STRLEN);
        }
        if (strchr(url, '?') != NULL || strchr(url, '#') != NULL) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_send(req,
                "{\"ok\":false,\"err\":\"URL must not contain ? or #\"}",
                HTTPD_RESP_USE_STRLEN);
        }
        const size_t ulen = strlen(url);
        const char *suffix = "api.php";
        const size_t slen = 7u;
        if (ulen < slen || strcmp(url + ulen - slen, suffix) != 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_send(req,
                "{\"ok\":false,\"err\":\"URL must end with \\\"api.php\\\"\"}",
                HTTPD_RESP_USE_STRLEN);
        }
    }
    if (h_sec && secret[0] != '\0' &&
        strlen(secret) < (size_t)CFG_MIN_SECRET_LEN) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req,
            "{\"ok\":false,\"err\":\"secret too short\"}",
            HTTPD_RESP_USE_STRLEN);
    }
    if (h_iv && (interval < CFG_MIN_STATUS_INTERVAL_S ||
                 interval > CFG_MAX_STATUS_INTERVAL_S)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req,
            "{\"ok\":false,\"err\":\"interval out of range\"}",
            HTTPD_RESP_USE_STRLEN);
    }
    if ((h_lh && (log_h < CFG_MIN_HOUR   || log_h > CFG_MAX_HOUR))   ||
        (h_lm && (log_m < CFG_MIN_MINUTE || log_m > CFG_MAX_MINUTE)) ||
        (h_en && (enable  < 0 || enable  > 1))                       ||
        (h_lr && (log_rot < 0 || log_rot > 1))                       ||
        (h_ex && (expose  < 0 || expose  > 0x3F))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req,
            "{\"ok\":false,\"err\":\"bounds\"}", HTTPD_RESP_USE_STRLEN);
    }

    /* a.6.35.5 — capture old values BEFORE writing so the audit rows
     * can show old → new. Snapshot under MX4 happens once for all fields. */
    cfg_shadow_t prev = {};
    dm_cfg_snapshot(&prev);

    if (h_url)               (void)nvs_cfg_set_str(NVS_NS_SYSTEM, "status_url",     url);
    if (h_sec && secret[0])  (void)nvs_cfg_set_str(NVS_NS_SYSTEM, "status_secret",  secret);
    if (h_iv)                (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, "status_intv_s",  interval);
    if (h_en)                (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, "status_enable",  enable);
    if (h_ex)                (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, "status_expose",  expose);
    if (h_lh)                (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, "log_upload_h",   log_h);
    if (h_lm)                (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, "log_upload_m",   log_m);
    if (h_lr)                (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, "log_upload_rot", log_rot);

    dm_reload_web_cfg();

    /* a.6.35.5 — emit audit rows for every changed field. String fields
     * (URL, secret) log "set" without exposing the value; integer fields
     * log old → new the same way climate/wind setpoints do. The "changed"
     * test guards against logging a no-op Apply where the operator clicked
     * without actually editing the field. */
    if (h_url && strcmp(url, prev.status_url) != 0) {
        log_web_setpoint(LOG_PARAM_STATUS_URL, 1, 0);
    }
    if (h_sec && secret[0]) {
        /* The secret comparison would require the existing value, which is
         * never echoed via /api/web GET. Conservative: log any non-empty
         * write as a "set". Operator side: redundant Applies of the same
         * secret will log redundant rows, which is acceptable for a
         * security-sensitive field. */
        log_web_setpoint(LOG_PARAM_STATUS_SECRET, 1, 0);
    }
    if (h_iv && interval != prev.status_interval_s) {
        log_web_setpoint(LOG_PARAM_STATUS_INTV,
                         (int16_t)prev.status_interval_s, (int16_t)interval);
    }
    if (h_en && enable != prev.status_enable) {
        log_web_setpoint(LOG_PARAM_STATUS_ENABLE,
                         (int16_t)prev.status_enable, (int16_t)enable);
    }
    if (h_ex && expose != prev.status_expose) {
        log_web_setpoint(LOG_PARAM_STATUS_EXPOSE,
                         (int16_t)prev.status_expose, (int16_t)expose);
    }
    if (h_lh && log_h != prev.log_upload_h) {
        log_web_setpoint(LOG_PARAM_LOG_UPLOAD_H,
                         (int16_t)prev.log_upload_h, (int16_t)log_h);
    }
    if (h_lm && log_m != prev.log_upload_m) {
        log_web_setpoint(LOG_PARAM_LOG_UPLOAD_M,
                         (int16_t)prev.log_upload_m, (int16_t)log_m);
    }
    if (h_lr && log_rot != prev.log_upload_rot) {
        log_web_setpoint(LOG_PARAM_LOG_UPLOAD_ROT,
                         (int16_t)prev.log_upload_rot, (int16_t)log_rot);
    }

    ESP_LOGI(TAG, "[T11] /api/web cfg updated: url=%s interval=%ld enable=%ld expose=0x%02lX",
             h_url ? url : "(unchanged)",
             (long)interval, (long)enable, (long)expose);
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* ============================================================
 * /api/ota/config — ROTA pull-OTA config (admin) — 2.2.0
 *
 * Dedicated endpoint (rota_tds.md R-F02/R-F03) because ota_url (128) exceeds
 * the generic /api/config str cap, and it also carries the pinned-cert PEM.
 * GET never echoes the secret (R-A09) — only a "secret_set" boolean.
 * ============================================================ */

static esp_err_t ota_config_get_handler(httpd_req_t *req)
{
    if (!admin_only_or_send_error(req)) return ESP_OK;
    cfg_shadow_t cfg = {};
    dm_cfg_snapshot(&cfg);
    char out[420];
    int n = snprintf(out, sizeof(out),
        "{\"ok\":true,\"enable\":%ld,\"check_h\":%ld,\"url\":\"%s\","
        "\"secret_set\":%s,\"win_lo\":%ld,\"win_hi\":%ld,\"cert_custom\":%s}",
        (long)cfg.ota_enable, (long)cfg.ota_check_h, cfg.ota_url,
        cfg.ota_secret[0] ? "true" : "false",
        (long)cfg.ota_win_lo, (long)cfg.ota_win_hi,
        rota_cert_is_custom() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, (n > 0 && n < (int)sizeof(out)) ? n : 0);
}

/* GET /api/ota/check — T16 last-check observability (task 3.9). Admin-only.
 * (Distinct from /api/ota/status, which reports the push-OTA state machine.) */
static esp_err_t rota_check_get_handler(httpd_req_t *req)
{
    if (!admin_only_or_send_error(req)) return ESP_OK;
    rota_status_t st;
    rota_status_get(&st);
    static const char *const RES[] = {
        "up_to_date", "update_available", "unreachable", "skipped", "auth_fail" };
    const char *res = (st.last_result >= 0 && st.last_result <= 4)
                      ? RES[st.last_result] : "none";
    char id[13] = {0};
    system_mac_str(id, sizeof(id));   /* device id the unit signs with (§4.2) */
    char out[460];
    int n = snprintf(out, sizeof(out),
        "{\"ok\":true,\"id\":\"%s\",\"last_check\":%ld,\"result\":\"%s\",\"result_code\":%ld,"
        "\"http\":%ld,\"checks\":%lu,\"offered\":\"%s\",\"running\":\"%s\","
        "\"dl\":%ld,\"apply\":%ld}",
        id, (long)st.last_check_epoch, res, (long)st.last_result,
        (long)st.last_http, (unsigned long)st.checks_total,
        st.offered_ver, FIRMWARE_VERSION,
        (long)st.last_dl, (long)st.last_apply);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, (n > 0 && n < (int)sizeof(out)) ? n : 0);
}

/* POST /api/ota/check — ask T16 to run a manifest check now (R-F04). Admin-only.
 * Passes the triggering admin's session token so the apply quiet gate exempts
 * this one session — otherwise a GUI-triggered update defers forever on the
 * operator's own login (gh#41). */
static esp_err_t rota_check_post_handler(httpd_req_t *req)
{
    if (!admin_only_or_send_error(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    char token[TOKEN_LEN + 1] = {0};
    (void)cookie_get_session(req, token);   /* admin already validated above */
    ota_client_request_check(token);
    return httpd_resp_send(req, "{\"ok\":true,\"queued\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ota_config_post_handler(httpd_req_t *req)
{
    if (!admin_only_or_send_error(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");

    /* Body may carry a ~1.5 KB PEM → heap-allocate to spare the httpd stack. */
    const size_t BODY_MAX = 2600u;
    char *body = (char *)calloc(1, BODY_MAX);
    char *cert = (char *)calloc(1, ROTA_CERT_MAX);
    if (body == NULL || cert == NULL) {
        free(body); free(cert);
        return httpd_resp_send(req, "{\"ok\":false,\"err\":\"nomem\"}", HTTPD_RESP_USE_STRLEN);
    }

    esp_err_t rc = ESP_OK;
    if (!read_request_body(req, body, BODY_MAX)) {
        httpd_resp_set_status(req, "400 Bad Request");
        rc = httpd_resp_send(req, "{\"ok\":false,\"err\":\"bad body\"}", HTTPD_RESP_USE_STRLEN);
        goto done;
    }

    {
        char url[CFG_MAX_URL_LEN + 1] = {};
        char secret[CFG_MAX_SECRET_LEN + 1] = {};
        char vbuf[16] = {};
        const bool h_url  = json_get_field(body, "url",    url,    sizeof(url));
        const bool h_sec  = json_get_field(body, "secret", secret, sizeof(secret));
        const bool h_cert = json_get_field(body, "cert",   cert,   ROTA_CERT_MAX);
        int32_t enable = 0, check_h = 0, win_lo = 0, win_hi = 0;
        const bool h_en = json_get_field(body, "enable",  vbuf, sizeof(vbuf)); if (h_en) enable  = atoi(vbuf);
        const bool h_ch = json_get_field(body, "check_h", vbuf, sizeof(vbuf)); if (h_ch) check_h = atoi(vbuf);
        const bool h_wl = json_get_field(body, "win_lo",  vbuf, sizeof(vbuf)); if (h_wl) win_lo  = atoi(vbuf);
        const bool h_wh = json_get_field(body, "win_hi",  vbuf, sizeof(vbuf)); if (h_wh) win_hi  = atoi(vbuf);

        /* ---- validate (reject before any write, R-F03) ---- */
        const char *err = NULL;
        if (h_url && url[0] != '\0') {
            if (strncmp(url, "https://", 8) != 0)                       err = "URL must use https://";
            else if (strchr(url, '?') || strchr(url, '#'))             err = "URL must not contain ? or #";
        }
        if (!err && h_sec && secret[0] && strlen(secret) < (size_t)CFG_MIN_SECRET_LEN) err = "secret too short";
        if (!err && h_en && (enable  < 0 || enable  > 1))              err = "enable out of range";
        if (!err && h_ch && (check_h < CFG_MIN_OTA_CHECK_H || check_h > CFG_MAX_OTA_CHECK_H)) err = "check_h out of range";
        if (!err && h_wl && (win_lo  < CFG_MIN_HOUR || win_lo > CFG_MAX_HOUR)) err = "win_lo out of range";
        if (!err && h_wh && (win_hi  < CFG_MIN_HOUR || win_hi > CFG_MAX_HOUR)) err = "win_hi out of range";
        if (!err && h_cert && cert[0] && strncmp(cert, "-----BEGIN", 10) != 0)  err = "cert must be PEM";
        if (err != NULL) {
            httpd_resp_set_status(req, "400 Bad Request");
            char msg[96];
            snprintf(msg, sizeof(msg), "{\"ok\":false,\"err\":\"%s\"}", err);
            rc = httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
            goto done;
        }

        /* ---- persist (empty secret/cert = keep current, R-F03) ---- */
        cfg_shadow_t prev = {};
        dm_cfg_snapshot(&prev);
        if (h_url)              (void)nvs_cfg_set_str(NVS_NS_SYSTEM, "ota_url",    url);
        if (h_sec && secret[0]) (void)nvs_cfg_set_str(NVS_NS_SYSTEM, "ota_secret", secret);
        if (h_en)               (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, "ota_enable", enable);
        if (h_ch)               (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, "ota_check_h", check_h);
        if (h_wl)               (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, "ota_win_lo",  win_lo);
        if (h_wh)               (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, "ota_win_hi",  win_hi);
        if (h_cert && cert[0])  (void)rota_cert_set(cert);
        dm_reload_web_cfg();

        /* ---- audit (strings log "set"; ints log old → new; R-F04, R-A09) ---- */
        if (h_url && strcmp(url, prev.ota_url) != 0) log_web_setpoint(LOG_PARAM_OTA_URL, 1, 0);
        if (h_sec && secret[0])                      log_web_setpoint(LOG_PARAM_OTA_SECRET, 1, 0);
        if (h_cert && cert[0])                       log_web_setpoint(LOG_PARAM_OTA_SECRET, 1, 0);
        if (h_en && enable  != prev.ota_enable)  log_web_setpoint(LOG_PARAM_OTA_ENABLE,  (int16_t)prev.ota_enable,  (int16_t)enable);
        if (h_ch && check_h != prev.ota_check_h) log_web_setpoint(LOG_PARAM_OTA_CHECK_H, (int16_t)prev.ota_check_h, (int16_t)check_h);
        if (h_wl && win_lo  != prev.ota_win_lo)  log_web_setpoint(LOG_PARAM_OTA_WIN_LO,  (int16_t)prev.ota_win_lo,  (int16_t)win_lo);
        if (h_wh && win_hi  != prev.ota_win_hi)  log_web_setpoint(LOG_PARAM_OTA_WIN_HI,  (int16_t)prev.ota_win_hi,  (int16_t)win_hi);

        /* Wake T16 to apply the new config on its next tick (R-F04). */
        if (task_t16 != NULL) { xTaskNotifyGive(task_t16); }

        ESP_LOGI(TAG, "[T11] /api/ota/config updated: enable=%ld check_h=%ld url=%s",
                 (long)enable, (long)check_h, h_url ? url : "(unchanged)");
        rc = httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    }

done:
    free(body);
    free(cert);
    return rc;
}

/* ============================================================
 * WebSocket — /ws  (Phase 6.16-η, alpha.6.21)
 *
 * Pushes the canonical status JSON every WS_PUSH_MS (2 s) to every
 * connected client. Behaviour matches 1.20.3:
 *   • Same JSON shape as GET /api/status (STATUS_EXPOSE_ALL,
 *     include_disabled_setpoints=true — local-UI mode).
 *   • 2 s push interval.
 *   • Bidirectional — incoming frames are read and discarded
 *     (the dashboard does not send any client→server WS messages,
 *     but the IDF httpd contract requires the URI handler to drain
 *     received frames or the socket stalls on the next ping).
 *
 * Client tracking uses esp_http_server's own client list instead of
 * a parallel table: httpd_get_client_list() returns every connected
 * fd, then httpd_ws_get_fd_info() filters down to fds that are still
 * in the WebSocket state. Stale fds are pruned implicitly on the
 * next push attempt — httpd_ws_send_frame_async returns ESP_ERR_*
 * for a closed socket and we simply skip it.
 *
 * The push runs in its own task (task_ws_push) spawned from
 * task_web_server. Keeping it off the httpd worker threads means a
 * slow push (LittleFS read, sensor snapshot copy) can't block
 * concurrent HTTP requests.
 * ============================================================ */

/** @brief WebSocket status-push cadence in ms — matches 1.20.3 (2 s). */
#define WS_PUSH_MS     2000u
/** @brief WS push body buffer in bytes — fits the 1.5–2.5 KB canonical JSON + headroom. */
#define WS_PUSH_BUF    4096u
/** @brief Maximum simultaneous WS clients tracked. Must be ≥ httpd `cfg.max_open_sockets - 2`. */
#define WS_MAX_CLIENTS    5

/**
 * @brief WebSocket URI handler for /ws.
 *
 * Called by esp_http_server twice per client lifetime: once at the
 * upgrade handshake (method = HTTP_GET, no frame), and on every
 * subsequent inbound frame. The handshake is auto-completed by the
 * httpd when this handler returns ESP_OK from the first call.
 *
 * The dashboard sends no payloads, but per protocol we must drain any
 * received frame to keep the socket alive.
 *
 * @param req esp_http_server request handle.
 * @return ESP_OK on handshake or frame drained successfully; error code
 *         from `httpd_ws_recv_frame()` on recv failure.
 * @note Auth requirement: Public — same rationale as /api/status (the
 *       pushed payload is byte-identical to that endpoint). Sensitive
 *       surfaces stay gated separately. The gate added in alpha.6.21
 *       was a mistake (logout would silently freeze the dashboard tiles
 *       even though /api/status still answered).
 * @note Rate limit: none.
 */
static esp_err_t ws_handler(httpd_req_t *req)
{
    /* GET = upgrade handshake. No auth gate. */
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "[T11] /ws upgrade fd=%d", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    /* Inbound frame from a subscribed client. Read it and discard.
     * The dashboard never sends WS payloads in 1.20.3 — but per the
     * protocol we still must drain to keep the socket alive. */
    httpd_ws_frame_t frame = {};
    /* First call with max_len=0 discovers the frame length. */
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[T11] /ws recv_frame(len) failed: %s", esp_err_to_name(err));
        return err;
    }
    if (frame.len == 0) return ESP_OK;

    /* Cap the payload — we don't expect anything legitimate from
     * the dashboard, but mute any large garbage. 256 B is enough
     * for a CLOSE control frame's reason string and any future
     * ping payload. */
    if (frame.len > 256) frame.len = 256;
    uint8_t buf[256];
    frame.payload = buf;
    err = httpd_ws_recv_frame(req, &frame, sizeof(buf));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[T11] /ws recv_frame(payload) failed: %s", esp_err_to_name(err));
    }
    /* Discard. No reply. */
    return ESP_OK;
}

/**
 * @brief Send `payload` of length `len` to every connected WS client.
 *
 * Uses `httpd_get_client_list` to enumerate active fds, then filters
 * by `httpd_ws_get_fd_info` to keep only WS-upgraded ones, then
 * `httpd_ws_send_frame_async` per matched fd. Stale fds (closed since
 * last enumeration) return an error from `send_frame_async` and are
 * skipped — esp_http_server prunes them from its internal list on
 * its own schedule.
 *
 * @param payload UTF-8 JSON string (typically the canonical status JSON).
 * @param len     Length in bytes (excluding any trailing NUL).
 */
static void ws_broadcast(const char *payload, size_t len)
{
    if (s_server == NULL || payload == NULL || len == 0) return;

    int fds[WS_MAX_CLIENTS];
    size_t n_fds = WS_MAX_CLIENTS;
    esp_err_t err = httpd_get_client_list(s_server, &n_fds, fds);
    if (err != ESP_OK || n_fds == 0) return;

    httpd_ws_frame_t frame = {};
    frame.type    = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t *)payload;
    frame.len     = len;
    frame.final   = true;

    for (size_t i = 0; i < n_fds; i++) {
        int fd = fds[i];
        httpd_ws_client_info_t info = httpd_ws_get_fd_info(s_server, fd);
        if (info != HTTPD_WS_CLIENT_WEBSOCKET) continue;   /* not a WS client */
        err = httpd_ws_send_frame_async(s_server, fd, &frame);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "[T11] /ws send fd=%d failed: %s (client gone)",
                     fd, esp_err_to_name(err));
            /* fd pruned by httpd on its own; nothing for us to do */
        }
    }
}

/**
 * @brief WS push task — wakes every WS_PUSH_MS, builds canonical
 *        status JSON, broadcasts to all subscribed clients.
 *
 * Runs independent of the httpd worker pool so a slow
 * `dm_status_snapshot()` / `build_canonical_status_json()` can't block
 * concurrent HTTP requests. Buffers are heap-allocated once and reused;
 * the task is the sole owner. Skips the snapshot+build cost entirely
 * when no WS client is currently subscribed.
 *
 * @param pvParameters Unused.
 * @note Suggested xTaskCreatePinnedToCore: stack 4096 B, prio 4, core 1 (APP_CPU).
 */
static void task_ws_push(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "[T11] WS push task alive — %ums interval", (unsigned)WS_PUSH_MS);

    /* Snapshot + JSON buffers, allocated once. snap goes in internal
     * RAM (~600 B); body in PSRAM-preferred (~4 KB). */
    status_snapshot_t *snap = (status_snapshot_t *)
        heap_caps_malloc(sizeof(status_snapshot_t), MALLOC_CAP_INTERNAL);
    char *body = (char *)heap_caps_malloc(WS_PUSH_BUF, MALLOC_CAP_DEFAULT);
    if (!snap || !body) {
        ESP_LOGE(TAG, "[T11] WS push: heap alloc failed (snap=%p body=%p) — task exit",
                 snap, body);
        if (snap) heap_caps_free(snap);
        if (body) heap_caps_free(body);
        vTaskDelete(NULL);
        return;
    }

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(WS_PUSH_MS));

        /* Skip the snapshot+build cost entirely when nobody is
         * subscribed — a connected client is the typical case during
         * an open dashboard tab, but in a deployed greenhouse the
         * UI is often idle. */
        int probe_fds[WS_MAX_CLIENTS];
        size_t probe_n = WS_MAX_CLIENTS;
        if (s_server == NULL) continue;
        if (httpd_get_client_list(s_server, &probe_n, probe_fds) != ESP_OK) continue;
        bool any_ws = false;
        for (size_t i = 0; i < probe_n; i++) {
            if (httpd_ws_get_fd_info(s_server, probe_fds[i]) ==
                HTTPD_WS_CLIENT_WEBSOCKET) {
                any_ws = true;
                break;
            }
        }
        if (!any_ws) continue;

        memset(snap, 0, sizeof(*snap));
        dm_status_snapshot(snap);
        size_t n = build_canonical_status_json(body, WS_PUSH_BUF, snap,
                                               STATUS_EXPOSE_ALL,
                                               /*include_disabled_setpoints=*/true);
        if (n == 0) {
            ESP_LOGW(TAG, "[T11] WS push: build_canonical_status_json returned 0 "
                          "(buffer overflow?) — skipping cycle");
            continue;
        }
        ws_broadcast(body, n);
    }

    /* Unreachable but keep for static analysers. */
    heap_caps_free(snap);
    heap_caps_free(body);
    vTaskDelete(NULL);
}

/* ============================================================
 * URI registration table
 *
 * Each entry is a `httpd_uri_t` descriptor registered with
 * `httpd_register_uri_handler()` from `task_web_server`. The descriptors
 * live at file scope so their addresses are stable for the duration of
 * the httpd's lifetime. The list is also walked in `task_web_server` so
 * adding a new route requires three edits: handler function, descriptor
 * here, and the `uris[]` array further below.
 * ============================================================ */
static const httpd_uri_t s_uri_root = {
    .uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = NULL };
/* alpha.6.24 — also serve /index.html directly. The 1.20.3 ESPAsyncWebServer
 * had a wildcard onNotFound that fell through to LFS; with esp_http_server we
 * register the path explicitly. Reusing root_handler keeps "/" and
 * "/index.html" byte-identical. */
static const httpd_uri_t s_uri_index = {
    .uri = "/index.html", .method = HTTP_GET, .handler = root_handler, .user_ctx = NULL };
static const httpd_uri_t s_uri_style = {
    .uri = "/style.css", .method = HTTP_GET, .handler = style_handler, .user_ctx = NULL };
static const httpd_uri_t s_uri_appjs = {
    .uri = "/app.js", .method = HTTP_GET, .handler = appjs_handler, .user_ctx = NULL };
static const httpd_uri_t s_uri_manifest = {
    .uri = "/manifest.json", .method = HTTP_GET, .handler = manifest_handler, .user_ctx = NULL };

static const httpd_uri_t s_uri_whoami = {
    .uri = "/api/whoami", .method = HTTP_GET, .handler = whoami_handler, .user_ctx = NULL };
static const httpd_uri_t s_uri_login = {
    .uri = "/api/login", .method = HTTP_POST, .handler = login_handler, .user_ctx = NULL };
static const httpd_uri_t s_uri_logout = {
    .uri = "/api/logout", .method = HTTP_POST, .handler = logout_handler, .user_ctx = NULL };

/* alpha.6.17 — status routes (Phase 6.16-γ). Public (no auth gate). */
static const httpd_uri_t s_uri_status = {
    .uri = "/api/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL };
static const httpd_uri_t s_uri_history = {
    .uri = "/api/history", .method = HTTP_GET, .handler = history_handler, .user_ctx = NULL };

/* alpha.6.18 — config routes (Phase 6.16-δ). */
static const httpd_uri_t s_uri_config_get = {
    .uri = "/api/config", .method = HTTP_GET, .handler = config_get_handler, .user_ctx = NULL };
static const httpd_uri_t s_uri_config_limits = {
    .uri = "/api/config/limits", .method = HTTP_GET, .handler = config_limits_handler, .user_ctx = NULL };
static const httpd_uri_t s_uri_config_post = {
    .uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler, .user_ctx = NULL };
static const httpd_uri_t s_uri_wifi_post = {
    .uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post_handler, .user_ctx = NULL };
static const httpd_uri_t s_uri_pin_post = {
    .uri = "/api/pin", .method = HTTP_POST, .handler = pin_post_handler, .user_ctx = NULL };

/* rc.1.5.0 (gh#28) — operating-mode toggle. Farmer or Admin session. */
static const httpd_uri_t s_uri_mode_post = {
    .uri = "/api/mode", .method = HTTP_POST, .handler = mode_post_handler, .user_ctx = NULL };

/* alpha.6.19 — SD + log routes (Phase 6.16-ε). */
static const httpd_uri_t s_uri_sd_status = {
    .uri = "/api/sd/status", .method = HTTP_GET, .handler = sd_status_handler, .user_ctx = NULL };
static const httpd_uri_t s_uri_sd_mount = {
    .uri = "/api/sd/mount", .method = HTTP_POST, .handler = sd_mount_handler, .user_ctx = NULL };
static const httpd_uri_t s_uri_sd_unmount = {
    .uri = "/api/sd/unmount", .method = HTTP_POST, .handler = sd_unmount_handler, .user_ctx = NULL };
static const httpd_uri_t s_uri_log_files = {
    .uri = "/api/log/files", .method = HTTP_GET, .handler = log_files_handler, .user_ctx = NULL };
static const httpd_uri_t s_uri_log_download = {
    .uri = "/api/log/download", .method = HTTP_GET, .handler = log_download_handler, .user_ctx = NULL };

/* a.6.35.6 — coredump retrieval routes. */
static const httpd_uri_t s_uri_coredump_status = {
    .uri = "/api/coredump/status",   .method = HTTP_GET,  .handler = coredump_status_handler,   .user_ctx = NULL };
static const httpd_uri_t s_uri_coredump_download = {
    .uri = "/api/coredump/download", .method = HTTP_GET,  .handler = coredump_download_handler, .user_ctx = NULL };
static const httpd_uri_t s_uri_coredump_erase = {
    .uri = "/api/coredump/erase",    .method = HTTP_POST, .handler = coredump_erase_handler,    .user_ctx = NULL };

/* alpha.6.20 — OTA + web-tab routes (Phase 6.16-ζ). */
static const httpd_uri_t s_uri_ota_status = {
    .uri = "/api/ota/status", .method = HTTP_GET, .handler = ota_status_handler, .user_ctx = NULL };
static const httpd_uri_t s_uri_ota_firmware = {
    .uri = "/api/ota/firmware", .method = HTTP_POST, .handler = ota_firmware_post_handler, .user_ctx = NULL };
static const httpd_uri_t s_uri_ota_assets = {
    .uri = "/api/ota/assets", .method = HTTP_POST, .handler = ota_assets_post_handler, .user_ctx = NULL };
static const httpd_uri_t s_uri_web_get = {
    .uri = "/api/web", .method = HTTP_GET, .handler = web_get_handler, .user_ctx = NULL };
static const httpd_uri_t s_uri_web_post = {
    .uri = "/api/web", .method = HTTP_POST, .handler = web_post_handler, .user_ctx = NULL };
/* 2.2.0 (ROTA) — pull-OTA config (admin). */
static const httpd_uri_t s_uri_ota_cfg_get = {
    .uri = "/api/ota/config", .method = HTTP_GET, .handler = ota_config_get_handler, .user_ctx = NULL };
static const httpd_uri_t s_uri_ota_cfg_post = {
    .uri = "/api/ota/config", .method = HTTP_POST, .handler = ota_config_post_handler, .user_ctx = NULL };
/* 2.2.0 (ROTA) — pull-OTA check status (GET) + manual trigger (POST), admin. */
static const httpd_uri_t s_uri_rota_check_get = {
    .uri = "/api/ota/check", .method = HTTP_GET, .handler = rota_check_get_handler, .user_ctx = NULL };
static const httpd_uri_t s_uri_rota_check_post = {
    .uri = "/api/ota/check", .method = HTTP_POST, .handler = rota_check_post_handler, .user_ctx = NULL };

/* alpha.6.21 — WebSocket route (Phase 6.16-η, final T11 route). */
static const httpd_uri_t s_uri_ws = {
    .uri          = "/ws",
    .method       = HTTP_GET,
    .handler      = ws_handler,
    .user_ctx     = NULL,
    .is_websocket = true,
};

/* ============================================================
 * Task entry point
 * ============================================================ */
/**
 * @brief T11 task body — start httpd, register routes, idle.
 *
 * Creates `s_sess_mux`, starts `esp_http_server` on port 80, registers
 * every URI from the descriptor table, and spawns `task_ws_push` pinned
 * to core 1. After that the task body just idles at a 60 s tick — the
 * httpd worker pool services requests concurrently in its own tasks.
 *
 * @param pvParameters Unused; pass NULL.
 * @warning On `httpd_start()` failure the task self-deletes; no web server
 *          will be available until reboot.
 */
void task_web_server(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "[T11] task alive (minimal T11 — static + auth only)");

    /* Session mutex. Must exist before httpd handlers can run. */
    s_sess_mux = xSemaphoreCreateMutex();
    if (s_sess_mux == NULL) {
        ESP_LOGE(TAG, "[T11] xSemaphoreCreateMutex failed — task exiting");
        vTaskDelete(NULL);
        return;
    }

    /* HTTPD config. HTTPD_DEFAULT_CONFIG: port 80, stack 4 KB, prio 5,
     * 8 max URI handlers (room for the 7 routes here + 1 spare). */
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.stack_size       = 8192;     /* +4 KB vs default for LFS_READ_BUF + JSON stack work */
    cfg.task_priority    = 5;
    cfg.max_uri_handlers = 36;       /* +4 for /api/ota/config + /api/ota/check GET+POST (2.2.0 ROTA); 34 routes total, 2 spare */
    cfg.max_open_sockets = 7;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[T11] httpd_start failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    /* Register URIs. */
    const httpd_uri_t *uris[] = {
        &s_uri_root, &s_uri_index, &s_uri_style, &s_uri_appjs, &s_uri_manifest,
        &s_uri_whoami, &s_uri_login, &s_uri_logout,
        &s_uri_status, &s_uri_history,
        &s_uri_config_get, &s_uri_config_limits, &s_uri_config_post,
        &s_uri_wifi_post, &s_uri_pin_post, &s_uri_mode_post,
        &s_uri_sd_status, &s_uri_sd_mount, &s_uri_sd_unmount,
        &s_uri_log_files, &s_uri_log_download,
        &s_uri_coredump_status, &s_uri_coredump_download, &s_uri_coredump_erase,
        &s_uri_ota_status, &s_uri_ota_firmware, &s_uri_ota_assets,
        &s_uri_web_get, &s_uri_web_post,
        &s_uri_ota_cfg_get, &s_uri_ota_cfg_post,
        &s_uri_rota_check_get, &s_uri_rota_check_post,
        &s_uri_ws,
    };
    for (size_t i = 0; i < sizeof(uris)/sizeof(uris[0]); i++) {
        err = httpd_register_uri_handler(s_server, uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[T11] register %s failed: %s",
                     uris[i]->uri, esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "[T11] HTTP server running on port 80 — 34 routes registered");
    ESP_LOGI(TAG, "[T11]   static: /  /style.css  /app.js  /manifest.json");
    ESP_LOGI(TAG, "[T11]   auth:   GET /api/whoami  POST /api/login  POST /api/logout");
    ESP_LOGI(TAG, "[T11]   status: GET /api/status  GET /api/history?n=N");
    ESP_LOGI(TAG, "[T11]   config: GET /api/config  GET /api/config/limits  POST /api/config");
    ESP_LOGI(TAG, "[T11]   admin:  POST /api/wifi  POST /api/pin");
    ESP_LOGI(TAG, "[T11]   mode:   POST /api/mode  (gh#28; farmer or admin)");
    ESP_LOGI(TAG, "[T11]   sd:     GET /api/sd/status  POST /api/sd/mount  POST /api/sd/unmount");
    ESP_LOGI(TAG, "[T11]   log:    GET /api/log/files  GET /api/log/download?file=NAME");
    ESP_LOGI(TAG, "[T11]   ota:    GET /api/ota/status  POST /api/ota/firmware  POST /api/ota/assets");
    ESP_LOGI(TAG, "[T11]   web:    GET /api/web  POST /api/web");
    ESP_LOGI(TAG, "[T11]   ws:     /ws (push every %ums)", (unsigned)WS_PUSH_MS);

    /* Spawn the WS push task. Pinned to APP_CPU (core 1) to keep httpd's
     * accept/dispatch on core 0; matches the 1.20.3 pin layout. */
    BaseType_t tres = xTaskCreatePinnedToCore(task_ws_push, "ws_push",
                                              4096, NULL, 4, NULL, 1);
    if (tres != pdPASS) {
        ESP_LOGE(TAG, "[T11] xTaskCreatePinnedToCore(ws_push) failed: %d",
                 (int)tres);
        /* Non-fatal: HTTP server still serves the other 24 routes. */
    }

    /* Task body: just idle. httpd runs in its own task (spawned by httpd_start),
     * WS push runs in its own task (above). T11 task could be deleted here, but
     * we keep it around as a host for future maintenance work (session expiry
     * sweep, OTA timeout sweep, etc.). */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));   /* 60 s idle tick */
    }
}
