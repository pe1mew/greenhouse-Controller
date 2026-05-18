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

#include "web_server.h"
#include "../types/app_types.h"
#include "../data_manager/data_manager.h"
#include "../auth/pin_auth.h"
#include "littlefs_storage.h"

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
    cfg.max_uri_handlers = 16;       /* room for the 7 routes + the deferred 18+ */
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
        &s_uri_whoami, &s_uri_login, &s_uri_logout
    };
    for (size_t i = 0; i < sizeof(uris)/sizeof(uris[0]); i++) {
        err = httpd_register_uri_handler(s_server, uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[T11] register %s failed: %s",
                     uris[i]->uri, esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "[T11] HTTP server running on port 80 — 7 routes registered");
    ESP_LOGI(TAG, "[T11]   static: /  /style.css  /app.js  /manifest.json");
    ESP_LOGI(TAG, "[T11]   auth:   GET /api/whoami  POST /api/login  POST /api/logout");

    /* Task body: just idle. httpd runs in its own task (spawned by httpd_start).
     * T11 task could be deleted here, but we keep it around as a host for
     * future maintenance work (session expiry sweep, WS push, etc.). */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));   /* 60 s idle tick */
    }
}
