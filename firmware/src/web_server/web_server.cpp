/**
 * @file web_server.cpp
 * @brief T11 — Web Server task (Phase 6.16-α/β, minimal: static + auth).
 *
 * **alpha.6.16 minimal-T11 status** (2026-05-18):
 *
 * Replaces alpha.5 `web_server_tickle.cpp` (3-route hardcoded HTML) with
 * the real T11 backed by `esp_http_server` + LittleFS. The original
 * 1.20.3 file (1330 lines, ESPAsyncWebServer-based) is archived as
 * `web_server_1.20.3_original.cpp.archived`.
 *
 * **This alpha lands the 7 baseline routes** required for the web GUI's
 * login flow to function:
 *
 *  Static (4):
 *   GET  /                  → /index.html from active LittleFS
 *   GET  /style.css         → /style.css
 *   GET  /app.js            → /app.js
 *   GET  /manifest.json     → /manifest.json
 *
 *  Auth (3):
 *   GET  /api/whoami        → {role:"farmer"|"admin"} or 401
 *   POST /api/login         → {role, pin} → set cookie + return ok or 401
 *   POST /api/logout        → clear cookie + invalidate session
 *
 * **Deferred to follow-up alphas (6.16.X)** — listed here so the unmigrated
 * routes are not lost:
 *  - GET  /api/status              (needs status_json.cpp)
 *  - GET  /api/config              (full cfg dump)
 *  - GET  /api/config/limits       (per-key bounds)
 *  - POST /api/config              (farmer-keys + admin-keys policy)
 *  - POST /api/wifi                (admin)
 *  - POST /api/pin                 (admin)
 *  - GET  /api/history             (sensor ring buffer)
 *  - GET  /api/sd/status           (farmer + admin)
 *  - POST /api/sd/mount,unmount    (admin)
 *  - GET  /api/log/files
 *  - GET  /api/log/download
 *  - POST /api/ota/firmware        (multipart upload to T13)
 *  - POST /api/ota/assets          (PSRAM accumulator → T13 ZIP extract)
 *  - POST /api/web                 (asset bundle upload)
 *  - GET  /api/ota/status          (T13 progress)
 *  - WS   /ws                      (status push every 2 s)
 *
 * **Session model**: in-memory table of MAX_SESSIONS=4 slots, each holding
 * a 16-hex-char token, a `web_session_role_t` (PIN_ROLE_FARMER=1 or PIN_ROLE_ADMIN=2),
 * and an `expiry` Unix timestamp. Browsers store the token in a
 * `Set-Cookie: session=TOKEN; Path=/; HttpOnly` cookie. Each authenticated
 * request slides the expiry forward by `cfg.session_timeout_min × 60` s.
 *
 * **LittleFS fallback**: on a factory-fresh unit (LittleFS empty),
 * `/index.html` returns a tiny built-in placeholder page that says
 * "Web assets not yet uploaded — use OTA /api/web". Same for the other 3
 * static routes (return 404, but with content-type set so curl + browser
 * see a useful message). The placeholder lets the operator visually
 * confirm T11 is responding even before web-asset OTA.
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

#include "web_server.h"
#include "../types/app_types.h"
#include "../data_manager/data_manager.h"
#include "../auth/pin_auth.h"
#include "../status_post/status_json.h"   /* alpha.6.17 — build_canonical_status_json */
#include "../event_logger/event_logger.h" /* alpha.6.19 — event_logger_sd_remount / _unmount */
#include "../ota_manager/ota_manager.h"   /* alpha.6.20 — ota_firmware_/assets_/get_* */
#include "../status_post/status_post.h"   /* alpha.6.20 — status_post_last_str (web tab) */
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
            "<p>Use OTA endpoint <code>POST /api/web</code> to upload "
            "the asset bundle ZIP.</p>"
            "<p>Requested path: <code>%s</code></p>"
            "</body></html>",
            FIRMWARE_VERSION, fs_path);
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, body, (n > 0) ? (size_t)n : 0);
        return ESP_OK;
    }

    /* Allocate a heap buffer. littlefs_read NUL-terminates the buffer, so
     * we read up to LFS_READ_BUF-1 bytes of file content + 1 NUL. For
     * binary files this would corrupt content past the first NUL byte,
     * but our 4 static routes (HTML, CSS, JS, JSON) are all text. */
    char *buf = (char *)heap_caps_malloc(LFS_READ_BUF, MALLOC_CAP_INTERNAL);
    if (buf == NULL) {
        ESP_LOGE(TAG, "[T11] serve_lfs %s: malloc(%u) failed",
                 fs_path, (unsigned)LFS_READ_BUF);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    lfs_status_t st = littlefs_read(active, fs_path, buf, LFS_READ_BUF);
    if (st != LFS_OK) {
        ESP_LOGW(TAG, "[T11] littlefs_read(%s) failed: %d", fs_path, (int)st);
        heap_caps_free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    const size_t n_read = strlen(buf);

    httpd_resp_set_type(req, mime);
    esp_err_t err = httpd_resp_send(req, buf, n_read);
    heap_caps_free(buf);
    ESP_LOGD(TAG, "[T11] served %s (%u B)", fs_path, (unsigned)n_read);
    return err;
}

static esp_err_t root_handler(httpd_req_t *req)
{
    return serve_lfs_file(req, "/index.html", "text/html; charset=utf-8");
}
static esp_err_t style_handler(httpd_req_t *req)
{
    return serve_lfs_file(req, "/style.css", "text/css; charset=utf-8");
}
static esp_err_t appjs_handler(httpd_req_t *req)
{
    return serve_lfs_file(req, "/app.js", "application/javascript; charset=utf-8");
}
static esp_err_t manifest_handler(httpd_req_t *req)
{
    return serve_lfs_file(req, "/manifest.json", "application/manifest+json");
}

/* ============================================================
 * Auth handlers
 * ============================================================ */

/**
 * GET /api/whoami → 200 {"role":"farmer"|"admin"} on valid session,
 *                  401 {"ok":false,"error":"no_session"} otherwise.
 *
 * Browsers call this on page load to know whether to show the login
 * overlay. Slides the session expiry forward on a hit (renewal).
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
 * POST /api/login
 *
 * Body: {"role":"farmer"|"admin","pin":"NNNN"}
 *
 * On match: 200 {"ok":true,"role":"R"} + Set-Cookie session=TOKEN.
 * On wrong PIN: 401 {"ok":false,"locked":false}.
 * On lockout: 401 {"ok":false,"locked":true,"remaining":N}.
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
 * POST /api/logout → invalidate session, clear cookie. Always returns 200.
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

    /* JSON array. Each entry ~80 chars; 60 entries → ~5 KB. Allocate 8 KB. */
    const size_t cap = 8192;
    char *body = (char *)heap_caps_malloc(cap, MALLOC_CAP_INTERNAL);
    if (body == NULL) {
        if (rows) heap_caps_free(rows);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t pos = 0;
    int w;
    w = snprintf(body + pos, cap - pos, "[");
    if (w > 0) pos += (size_t)w;

    for (uint16_t i = 0; i < n && pos < cap - 1; i++) {
        const sensor_reading_t *e = &rows[i];
        w = snprintf(body + pos, cap - pos,
            "%s{\"ts\":%lu,\"t\":%d,\"rh\":%u,\"ws\":%u,\"wd\":%u}",
            (i > 0) ? "," : "",
            (unsigned long)e->timestamp,
            (int)e->t_avg_c,
            (unsigned)e->rh_avg_pct,
            (unsigned)e->wind_speed_avg_ms10,
            (unsigned)e->wind_dir_avg_deg);
        if (w < 0 || (size_t)w >= cap - pos) {
            ESP_LOGW(TAG, "[T11] /api/history: row %u truncated", (unsigned)i);
            break;
        }
        pos += (size_t)w;
    }

    if (pos < cap - 1) {
        body[pos++] = ']';
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
 * GET /api/config — full configuration as a JSON object.
 *
 * Auth-required (any logged-in session). Mirrors 1.20.3's build_config_json
 * exactly — same key names, same ordering, so the web UI's `app.js` reads
 * fields by the same identifiers it always has. Includes the running
 * fw_version from NVS (which nvs_cfg_init writes at every boot), the AP
 * SSID computed from the WiFi STA MAC, and the WiFi STA SSID (psk is
 * deliberately write-only).
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
 * GET /api/config/limits — per-key min/max bounds (PUBLIC).
 *
 * Single source of truth: cfg_limits.h. Stringified at compile time via
 * _LIMITS_STR — no runtime overhead, no allocation. app.js fetches this
 * once at page load and applies min/max to every <input> in the GUI.
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
 * @brief Helper: read up to `cap-1` bytes from the request body, NUL-terminate.
 * Returns true on success.
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
 * POST /api/config — body {"ns","key","value" | "str_value"}.
 *
 * Auth: farmer if (ns,key) is in FARMER_KEYS / FARMER_WIND_KEYS, else admin.
 * Integer writes go via Q4 → T4 → NVS (T4 validates against cfg_limits.h).
 * String writes (tz_str) go straight to NVS via nvs_cfg_set_str. On tz_str
 * the helper also applies the timezone live so localtime_r picks it up.
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
     * NUL terminator avoids the gcc -Wformat-truncation pessimism. */
    config_update_t upd = {};
    strncpy(upd.ns,  ns,  sizeof(upd.ns)  - 1);
    upd.ns[sizeof(upd.ns)  - 1] = '\0';
    strncpy(upd.key, key, sizeof(upd.key) - 1);
    upd.key[sizeof(upd.key) - 1] = '\0';
    upd.value = (int32_t)atoi(val_buf);

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
 */
static void wifi_apply_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGW(TAG, "[T11] /api/wifi apply: restarting now");
    esp_restart();
}

/**
 * POST /api/wifi — admin-only. Body may include any of {"ssid","psk","ap_psk"}.
 *
 * Each field provided is written to NVS namespace "wifi". On any STA-cred
 * or AP-cred change, schedules a 1-second-deferred esp_restart so T10's
 * NVS-load picks up the new values. The HTTP response flushes before the
 * reset.
 */
/**
 * @brief Inline auth check that returns 401 (no session) or 403 (wrong role)
 *        with the right error body. Returns true on success.
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
 * POST /api/pin — admin-only. Body {"role":"farmer"|"admin","pin":"NNNN"}.
 *
 * Calls pin_auth_set which writes the salted SHA-256 hash to NVS. Returns
 * {"ok":true} on success; otherwise {"ok":false,"err":"..."}.
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
        return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    }
    ESP_LOGW(TAG, "[T11] /api/pin: pin_auth_set failed rc=%d", (int)res);
    return httpd_resp_send(req,
        "{\"ok\":false,\"err\":\"pin_auth_set failed\"}", HTTPD_RESP_USE_STRLEN);
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

static esp_err_t sd_unmount_handler(httpd_req_t *req)
{
    if (!admin_only_or_send_error(req)) return ESP_OK;
    event_logger_sd_unmount();
    ESP_LOGI(TAG, "[T11] /api/sd/unmount: unmounted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/**
 * GET /api/log/files — list .csv files on SD, sorted chronologically.
 *
 * `nvs_count` is intentionally absent — the NVS-ringbuffer log source
 * was retired in alpha.6.5. SD is the only source.
 *
 * Filename names follow YYYYMMDDHHMMSS.csv from T9, so lexicographic
 * sort = chronological order. Includes any imported names too
 * (e.g. 1.20.3-era log_YYYYMMDD_HHMMSS.csv) — the sort order is
 * approximately right for those.
 */
static esp_err_t log_files_handler(httpd_req_t *req)
{
    if (!admin_only_or_send_error(req)) return ESP_OK;

    /* List buffer for storage_sd_list_csv (comma-separated string). */
    const size_t LIST_LEN = 512u;
    char *list_buf = (char *)heap_caps_malloc(LIST_LEN, MALLOC_CAP_INTERNAL);
    if (list_buf == NULL) { httpd_resp_send_500(req); return ESP_FAIL; }
    list_buf[0] = '\0';
    if (storage_sd_available()) {
        (void)storage_sd_list_csv(".csv", list_buf, LIST_LEN);
    }

    /* Tokenize → fixed-size name array. */
    enum { LOG_FILES_MAX = 12, LOG_FNAME_MAX = 24 };
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
 * GET /api/log/download?file=NAME — stream a CSV file as
 *   Content-Disposition: attachment; filename="NAME".
 *
 * Rejects path-traversal attempts (any '/' or "..") and requires
 * `file` query param. PSRAM-allocates the whole file (CSV files are
 * typically < 100 KB; with 8 MB PSRAM that's comfortable).
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
 * POST /api/ota/firmware — admin only.
 *
 * Receives the .bin body in chunks via httpd_req_recv; each chunk is fed
 * straight into ota_firmware_write. content_len is required (Content-Length
 * header) so T13 can pre-validate the image size against the inactive bank.
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
 * POST /api/ota/assets — admin only.
 *
 * Accumulates a STORE-only .zip body into the T13 PSRAM buffer via
 * ota_assets_accumulate(data, len, offset). On the last chunk calls
 * ota_assets_end() which spawns T13 to extract to inactive LittleFS.
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
 * GET /api/web — admin only. Returns web-tab settings + last-attempt strings.
 *
 * Secret is intentionally NOT echoed (write-only from the UI; the input
 * stays blank and "empty=keep" on POST).
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
 * POST /api/web — admin only. Single-transaction Apply with bounds-check
 * before any NVS write, then `dm_reload_web_cfg()` so the cfg shadow
 * refreshes synchronously (T4 publishes under MX4 before this returns).
 *
 * URL validation matches 1.20.3: must start with http:// or https://, must
 * NOT contain ? or #, must end with "api.php" (T14 appends ?action=log
 * itself; HTTPClient followed redirects silently which masked routing bugs).
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

    /* URL validation. */
    if (h_url && url[0] != '\0') {
        if (strncmp(url, "http://", 7) != 0 &&
            strncmp(url, "https://", 8) != 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_send(req,
                "{\"ok\":false,\"err\":\"URL must start with http:// or https://\"}",
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

    if (h_url)               (void)nvs_cfg_set_str(NVS_NS_SYSTEM, "status_url",     url);
    if (h_sec && secret[0])  (void)nvs_cfg_set_str(NVS_NS_SYSTEM, "status_secret",  secret);
    if (h_iv)                (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, "status_intv_s",  interval);
    if (h_en)                (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, "status_enable",  enable);
    if (h_ex)                (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, "status_expose",  expose);
    if (h_lh)                (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, "log_upload_h",   log_h);
    if (h_lm)                (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, "log_upload_m",   log_m);
    if (h_lr)                (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, "log_upload_rot", log_rot);

    dm_reload_web_cfg();
    ESP_LOGI(TAG, "[T11] /api/web cfg updated: url=%s interval=%ld enable=%ld expose=0x%02lX",
             h_url ? url : "(unchanged)",
             (long)interval, (long)enable, (long)expose);
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
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

#define WS_PUSH_MS  2000u   /**< matches 1.20.3 — 2 s status push cadence */
#define WS_PUSH_BUF 4096u   /**< matches /api/status — 1.5–2.5 KB JSON + headroom */
#define WS_MAX_CLIENTS 5    /**< must be ≥ httpd cfg.max_open_sockets - 2 */

/**
 * @brief WebSocket URI handler for /ws.
 *
 * Called by esp_http_server twice per client lifetime: once at the
 * upgrade handshake (method = HTTP_GET, no frame), and on every
 * subsequent inbound frame. The handshake is auto-completed by the
 * httpd when this handler returns ESP_OK from the first call.
 *
 * Auth: GATED to farmer-or-higher. Without this gate any browser on
 * the LAN could subscribe to the live status stream — same gate as
 * the local LCD displays, where the operator must authenticate at
 * the keypad before reading detailed sensor values. Login is
 * checked once at upgrade-time; once subscribed the client stays
 * connected until it disconnects or the session times out (the
 * push task does not re-verify per push — symmetric with 1.20.3).
 */
static esp_err_t ws_handler(httpd_req_t *req)
{
    /* GET = upgrade handshake. Gate it here. */
    if (req->method == HTTP_GET) {
        if (require_auth(req, WEB_ROLE_FARMER) == WEB_ROLE_NONE) {
            /* require_auth already sent a 401 with JSON body. The
             * httpd will not perform the WS upgrade because we
             * return ESP_OK *after* a body was sent. */
            return ESP_OK;
        }
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
 * Uses httpd_get_client_list to enumerate active fds, then filters
 * by httpd_ws_get_fd_info to keep only WS-upgraded ones, then
 * httpd_ws_send_frame_async per matched fd. Stale fds (closed since
 * last enumeration) return an error from send_frame_async and are
 * skipped — esp_http_server prunes them from its internal list on
 * its own schedule.
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
 * dm_status_snapshot() / build_canonical_status_json() can't block
 * concurrent HTTP requests. Buffers are heap-allocated once and
 * reused; the task is the sole owner.
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
 * ============================================================ */
static const httpd_uri_t s_uri_root = {
    .uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = NULL };
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
    cfg.max_uri_handlers = 28;       /* room for the 19 routes now + room for OTA (5) + WS (1) in 6.16-ζ/η */
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
        &s_uri_root, &s_uri_style, &s_uri_appjs, &s_uri_manifest,
        &s_uri_whoami, &s_uri_login, &s_uri_logout,
        &s_uri_status, &s_uri_history,
        &s_uri_config_get, &s_uri_config_limits, &s_uri_config_post,
        &s_uri_wifi_post, &s_uri_pin_post,
        &s_uri_sd_status, &s_uri_sd_mount, &s_uri_sd_unmount,
        &s_uri_log_files, &s_uri_log_download,
        &s_uri_ota_status, &s_uri_ota_firmware, &s_uri_ota_assets,
        &s_uri_web_get, &s_uri_web_post,
        &s_uri_ws,
    };
    for (size_t i = 0; i < sizeof(uris)/sizeof(uris[0]); i++) {
        err = httpd_register_uri_handler(s_server, uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[T11] register %s failed: %s",
                     uris[i]->uri, esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "[T11] HTTP server running on port 80 — 25 routes registered");
    ESP_LOGI(TAG, "[T11]   static: /  /style.css  /app.js  /manifest.json");
    ESP_LOGI(TAG, "[T11]   auth:   GET /api/whoami  POST /api/login  POST /api/logout");
    ESP_LOGI(TAG, "[T11]   status: GET /api/status  GET /api/history?n=N");
    ESP_LOGI(TAG, "[T11]   config: GET /api/config  GET /api/config/limits  POST /api/config");
    ESP_LOGI(TAG, "[T11]   admin:  POST /api/wifi  POST /api/pin");
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
