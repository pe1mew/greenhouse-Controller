/**
 * @file web_server.cpp
 * @brief T11 — Web Server task (Phase 9).
 *
 * Serves HTML/CSS/JS from the active LittleFS partition via ESPAsyncWebServer.
 * Implements cookie-based session authentication (PIN verified via pin_auth),
 * a REST API for status and configuration, and a WebSocket endpoint for live
 * dashboard updates pushed every WS_PUSH_MS milliseconds.
 *
 * ── REST endpoints ─────────────────────────────────────────────────────────
 *  GET  /               → index.html (from LittleFS)
 *  GET  /style.css      → stylesheet
 *  GET  /app.js         → JavaScript
 *  GET  /api/whoami     → {role} or 401 — lets the browser check cookie validity
 *  POST /api/login      → {role, pin} → {ok, role} or {ok:false, locked, remaining}
 *  POST /api/logout     → {} → {ok:true}
 *  GET  /api/status     → full status JSON (same as WS push)
 *  GET  /api/config        → all configuration parameters as JSON
 *  GET  /api/config/limits → per-key {min,max} bounds (public; used by web UI)
 *  POST /api/config        → {ns, key, value} (int) or {ns, key, str_value} (string)
 *  POST /api/wifi       → {ssid, psk} or {ap_psk} — writes directly to NVS wifi ns
 *  POST /api/pin        → {role, pin} — admin-only; changes farmer/admin PIN
 *  GET  /api/history    → ?n=N — last N sensor ring buffer entries as JSON
 *  GET  /api/sd/status    → {mounted, free_mb, size_mb} — SD card state (farmer+admin)
 *  POST /api/sd/mount    → {} — mount SD card (admin)
 *  POST /api/sd/unmount  → {} — unmount SD card (admin)
 *  GET  /api/log/files   → {nvs_count, sd_files:[...]} — log source list (admin)
 *  GET  /api/log/download → ?src=nvs | ?src=sd&file=NAME — download log CSV (admin)
 *  WS   /ws              → push status JSON every WS_PUSH_MS
 *
 * ── Access control ────────────────────────────────────────────────────────
 *  Unauthenticated GET on / returns 200 (login overlay rendered by JS).
 *  Unauthenticated API calls return 401.
 *  POST /api/config with farmer session: allowed only for farmer-level keys.
 *  POST /api/wifi, /api/pin: admin session required.
 *
 * ── Thread safety ─────────────────────────────────────────────────────────
 *  Request handlers run in the async_tcp task context (Core 0).
 *  Session table access is protected by s_sess_mux.
 *  MX2/MX4 are acquired for data reads via dm_*_snapshot().
 *  NVS writes (wifi credentials, PIN) call nvs_cfg and pin_auth functions,
 *  which are internally mutex-protected.
 *
 * @author  Greenhouse Controller project
 */

#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include <Arduino.h>
#include <esp_log.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <ESPAsyncWebServer.h>
#include <WiFi.h>

#include "web_server.h"
#include "../types/app_types.h"
#include "../data_manager/data_manager.h"
#include "../relay_controller/relay_controller.h"
#include "../event_logger/event_logger.h"
#include "../auth/pin_auth.h"
#include "../status_post/status_json.h"
#include "../status_post/status_post.h"
#include "littlefs_storage.h"
#include "nvs_config.h"
#include "../../../drivers/sdCard/src/sd_storage.h"
#include "../ota_manager/ota_manager.h"
#include "cfg_limits.h"

static const char *TAG = "T11_WEB";

/* ============================================================
 * Compile-time constants
 * ============================================================ */
#define WS_PUSH_MS       2000u  /**< WebSocket status push interval */
#define MAX_SESSIONS        4   /**< Max concurrent web sessions */
#define TOKEN_LEN          16   /**< Session token length (hex chars) */
#define LFS_BUF_SIZE    65536u  /**< Max LittleFS file read buffer (PSRAM). Sized at 64 KiB to leave ~30 KiB headroom above the current largest static asset (index.html, ~33 KiB at 1.16.36). serve_lfs() allocates this whole buffer per request and beginResponse() treats it as a null-terminated C string, so files larger than LFS_BUF_SIZE-1 are silently truncated — that bit 1.16.35 when index.html crossed 32767 bytes and the footer's GitHub link disappeared. */
#define HIST_MAX_ROWS      60   /**< Max history rows returned by /api/history */

/* Farmer-visible NVS keys: POST /api/config accepts these without admin */
#define FARMER_NS  "climate"
static const char * const FARMER_KEYS[] = {
    "t_max_day","t_min_day","t_max_ngt","t_min_ngt",
    "rh_max_day","rh_min_day","rh_max_ngt","rh_min_ngt",
    "rh_ctrl_en","cr_priority",
    NULL
};
/* Wind-protection enable is farmer-writable */
static const char * const FARMER_WIND_KEYS[] = { "wind_prot_en", NULL };

/* ============================================================
 * Session management
 * ============================================================ */
typedef struct {
    char      token[TOKEN_LEN + 1];
    session_t role;
    time_t    expiry;      /**< Unix timestamp; 0 = free slot */
    time_t    timeout_s;   /**< Idle timeout in seconds; used to slide expiry on activity */
} web_session_t;

static web_session_t      s_sessions[MAX_SESSIONS];
static SemaphoreHandle_t  s_sess_mux;

/** Generate a 16-char hex token from 8 random bytes. */
static void gen_token(char out[TOKEN_LEN + 1])
{
    uint8_t rnd[8];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 8; i++) {
        snprintf(out + i * 2, 3, "%02x", rnd[i]);
    }
    out[TOKEN_LEN] = '\0';
}

/** Find session by token; returns role or SESSION_NONE if not found/expired.
 *  Slides the expiry deadline forward by timeout_s on every successful match
 *  so that user activity resets the idle timer. */
static session_t session_find(const char *token)
{
    if (!token || token[0] == '\0') return SESSION_NONE;
    session_t role = SESSION_NONE;
    time_t now = time(NULL);
    xSemaphoreTake(s_sess_mux, pdMS_TO_TICKS(200));
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].expiry > 0 &&
            s_sessions[i].expiry > now &&
            strcmp(s_sessions[i].token, token) == 0)
        {
            role = s_sessions[i].role;
            s_sessions[i].expiry = now + s_sessions[i].timeout_s;  /* slide window */
            break;
        }
    }
    xSemaphoreGive(s_sess_mux);
    return role;
}

/** Find session by token without sliding the expiry deadline.
 *  Used for probe-only calls (/api/whoami) so the browser's periodic
 *  validity check does not prevent the idle timeout from firing. */
static session_t session_find_peek(const char *token)
{
    if (!token || token[0] == '\0') return SESSION_NONE;
    session_t role = SESSION_NONE;
    time_t now = time(NULL);
    xSemaphoreTake(s_sess_mux, pdMS_TO_TICKS(200));
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].expiry > 0 &&
            s_sessions[i].expiry > now &&
            strcmp(s_sessions[i].token, token) == 0)
        {
            role = s_sessions[i].role;
            break;  /* no slide — expiry is not modified */
        }
    }
    xSemaphoreGive(s_sess_mux);
    return role;
}

/** Create a new session; returns token string (in caller's buffer). */
static bool session_create(session_t role, int32_t timeout_min, char out_token[TOKEN_LEN + 1])
{
    time_t expiry = time(NULL) + (time_t)(timeout_min > 0 ? timeout_min : 5) * 60;
    bool ok = false;
    xSemaphoreTake(s_sess_mux, pdMS_TO_TICKS(200));
    /* Evict oldest expired slot first */
    int slot = -1;
    time_t oldest_exp = LONG_MAX;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].expiry == 0 || s_sessions[i].expiry <= time(NULL)) {
            slot = i; break;
        }
        if (s_sessions[i].expiry < oldest_exp) {
            oldest_exp = s_sessions[i].expiry; slot = i;
        }
    }
    if (slot >= 0) {
        gen_token(s_sessions[slot].token);
        s_sessions[slot].role      = role;
        s_sessions[slot].expiry    = expiry;
        s_sessions[slot].timeout_s = (time_t)(timeout_min > 0 ? timeout_min : 5) * 60;
        memcpy(out_token, s_sessions[slot].token, TOKEN_LEN + 1);
        ok = true;
    }
    xSemaphoreGive(s_sess_mux);
    return ok;
}

/** Invalidate a session by token. */
static void session_destroy(const char *token)
{
    if (!token || token[0] == '\0') return;   /* ignore empty / null token */
    xSemaphoreTake(s_sess_mux, pdMS_TO_TICKS(200));
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].expiry > 0 &&
            strcmp(s_sessions[i].token, token) == 0)
        {
            s_sessions[i].expiry    = 0;
            s_sessions[i].token[0]  = '\0';  /* clear token so it cannot be re-matched */
            break;
        }
    }
    xSemaphoreGive(s_sess_mux);
}

/** Extract session token from Cookie header. */
static void cookie_get_session(AsyncWebServerRequest *req, char out[TOKEN_LEN + 1])
{
    out[0] = '\0';
    if (!req->hasHeader("Cookie")) return;
    const String &cookie = req->getHeader("Cookie")->value();
    const char *p = strstr(cookie.c_str(), "session=");
    if (!p) return;
    p += 8;
    int i;
    for (i = 0; i < TOKEN_LEN && p[i] && p[i] != ';' && p[i] != ' '; i++) {
        out[i] = p[i];
    }
    out[i] = '\0';
}

/** Get session role for an incoming request. */
static session_t req_role(AsyncWebServerRequest *req)
{
    char token[TOKEN_LEN + 1];
    cookie_get_session(req, token);
    return session_find(token);
}

/* ============================================================
 * LittleFS helpers
 * ============================================================ */
static lfs_partition_t s_lfs_part;

/** Inject `?v=<FIRMWARE_VERSION>` after the named asset reference in the
 *  HTML buffer. Used on index.html to bust browser caches of `app.js` and
 *  `style.css` across OTA updates. The replacement happens in-place: we
 *  shift the bytes after the match right by the length of the version
 *  query string, then write the query string into the gap.
 *
 *  Why this matters: every browser obeys `Cache-Control: no-store` for the
 *  HTML document itself, but if the user holds the page open across an OTA
 *  reboot some browsers reuse the in-memory `app.js` / `style.css` they
 *  already parsed. Changing the URL on every release forces a fresh fetch
 *  even when the browser would otherwise believe its copy is current.
 *
 *  Returns the new content length (≤ original_len + room_added). The
 *  buffer must have at least @p slack bytes free past the original NUL.
 */
static size_t inject_cache_buster(char *buf, size_t buf_cap,
                                  size_t cur_len,
                                  const char *target,
                                  const char *qparam)
{
    /* Find `target` followed by `"` (e.g. `app.js"` ). Restricting the
     * match to the closing quote avoids hitting `app.js` mentions in
     * tooltip text or comments elsewhere in the document. */
    char needle[24];
    snprintf(needle, sizeof(needle), "%s\"", target);

    char *p = strstr(buf, needle);
    if (!p) return cur_len;

    size_t qparam_len = strlen(qparam);
    size_t target_len = strlen(target);

    /* Position of the closing quote. */
    char *quote = p + target_len;

    if (cur_len + qparam_len + 1 >= buf_cap) return cur_len;   /* no room */

    /* Shift the tail (including the NUL) right by qparam_len. */
    size_t tail = cur_len - (quote - buf) + 1u;
    memmove(quote + qparam_len, quote, tail);
    memcpy(quote, qparam, qparam_len);

    return cur_len + qparam_len;
}

/** Serve a file from LittleFS. Falls back to 404 if not found.
 *  Cache-Control: no-store prevents the browser from caching static assets
 *  across firmware/web-asset OTA updates. For index.html we additionally
 *  rewrite the `app.js` / `style.css` references with a `?v=<version>`
 *  query string so browsers that ignore no-store still pull fresh assets. */
static void serve_lfs(AsyncWebServerRequest *req, const char *path, const char *ct)
{
    char *buf = (char *)ps_malloc(LFS_BUF_SIZE);
    if (!buf) { req->send(500, "text/plain", "OOM"); return; }

    lfs_status_t st = littlefs_read(s_lfs_part, path, buf, LFS_BUF_SIZE);
    if (st == LFS_OK) {
        /* Cache-bust asset references when serving the HTML shell.
         * The version comes from FIRMWARE_VERSION (compile-time): assets
         * shipped together with this firmware will share the same string,
         * and an asset/firmware mismatch surfaces as a stale URL on the
         * page (the browser is forced to revalidate). */
        if (strcmp(path, "/index.html") == 0) {
            size_t cur_len = strnlen(buf, LFS_BUF_SIZE);
            cur_len = inject_cache_buster(buf, LFS_BUF_SIZE, cur_len,
                                          "app.js",    "?v=" FIRMWARE_VERSION);
            cur_len = inject_cache_buster(buf, LFS_BUF_SIZE, cur_len,
                                          "style.css", "?v=" FIRMWARE_VERSION);
            (void)cur_len;
        }
        AsyncWebServerResponse *resp = req->beginResponse(200, ct, buf);
        resp->addHeader("Cache-Control", "no-store");
        req->send(resp);
    } else {
        req->send(404, "text/plain", "Not found");
    }
    free(buf);
}

/* ============================================================
 * Status JSON builder
 * ============================================================ */

static void build_status_json(char *buf, size_t len)
{
    /* Single source of truth: dm_status_snapshot() acquires MX2/MX4/relay
     * spinlock under the hood; build_canonical_status_json() formats the
     * spec-shaped payload (design/technical-spec-statusWebsite.md §9.2).
     * The local UI receives every tile (STATUS_EXPOSE_ALL); T14 passes the
     * user-configured expose mask. */
    status_snapshot_t snap;
    dm_status_snapshot(&snap);
    /* include_disabled_setpoints=true: local UI gets the values even when
     * RH ctrl is off, so it can render them dimmed instead of hiding. */
    build_canonical_status_json(buf, len, &snap, STATUS_EXPOSE_ALL, true);
}

/* ============================================================
 * Config JSON builder
 * ============================================================ */

static void build_config_json(char *buf, size_t len)
{
    cfg_shadow_t c;
    dm_cfg_snapshot(&c);

    /* Read WiFi SSID from NVS — not in cfg_shadow_t.
     * PSK is intentionally omitted (write-only from the web UI). */
    char wifi_ssid[64] = {};
    nvs_cfg_get_str("wifi", "ssid", wifi_ssid, sizeof(wifi_ssid));

    /* Read fw_version from NVS — written on every boot by nvs_cfg_init().
     * This is the single source of truth for the running firmware version. */
    char fw_version[16] = {};
    nvs_cfg_get_str(NVS_NS_SYSTEM, NVS_KEY_FW_VERSION, fw_version, sizeof(fw_version));

    /* Build AP SSID the same way T10 does — last 2 MAC bytes. */
    char ap_ssid[24] = {};
    {
        uint8_t mac[6] = {0};
        WiFi.macAddress(mac);
        snprintf(ap_ssid, sizeof(ap_ssid), "Greenhouse-%02X%02X", mac[4], mac[5]);
    }

    snprintf(buf, len,
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
        c.t_max_day, c.t_min_day, c.t_max_ngt, c.t_min_ngt,
        c.rh_max_day, c.rh_min_day, c.rh_max_ngt, c.rh_min_ngt,
        c.hyst_t, c.hyst_rh, c.rh_ctrl_en, c.cr_priority,
        c.avg_win_t, c.avg_win_rh,
        c.v_max, c.wind_prot_en,
        c.dir_excl_low, c.dir_excl_high,
        c.travel_s[0], c.travel_s[1], c.travel_s[2],
        c.dwell_open_min[0], c.dwell_open_min[1], c.dwell_open_min[2],
        c.dwell_close_min[0], c.dwell_close_min[1], c.dwell_close_min[2],
        (long)c.poll_interval_s, (long)c.session_timeout_min, (long)c.ap_timeout_min,
        (long)c.lat_deg, (long)c.lat_frac,
        (long)c.lon_deg, (long)c.lon_frac,
        c.tz_str,
        fw_version
    );
}

/* ============================================================
 * Farmer key check
 * ============================================================ */
static bool is_farmer_key(const char *ns, const char *key)
{
    if (strcmp(ns, FARMER_NS) == 0) {
        for (int i = 0; FARMER_KEYS[i]; i++) {
            if (strcmp(key, FARMER_KEYS[i]) == 0) return true;
        }
    }
    if (strcmp(ns, "wind") == 0) {
        for (int i = 0; FARMER_WIND_KEYS[i]; i++) {
            if (strcmp(key, FARMER_WIND_KEYS[i]) == 0) return true;
        }
    }
    return false;
}

/* ============================================================
 * JSON body parser — minimal, no external lib
 * ============================================================ */
/** Extract a string field from a flat JSON object body. */
static bool json_get_str(const char *body, const char *field,
                         char *out, size_t out_len)
{
    char search[48];
    snprintf(search, sizeof(search), "\"%s\":", field);
    const char *p = strstr(body, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ') p++;
    if (*p == '"') {
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < out_len) out[i++] = *p++;
        out[i] = '\0';
        return true;
    }
    return false;
}

/** Extract a numeric field (integer) from a flat JSON object body. */
static bool json_get_int(const char *body, const char *field, int32_t *out)
{
    char search[48];
    snprintf(search, sizeof(search), "\"%s\":", field);
    const char *p = strstr(body, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ') p++;
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        *out = (int32_t)strtol(p, NULL, 10);
        return true;
    }
    return false;
}

/* ============================================================
 * WebSocket
 * ============================================================ */
static AsyncWebSocket s_ws("/ws");

static void on_ws_event(AsyncWebSocket *server, AsyncWebSocketClient *client,
                        AwsEventType type, void *arg, uint8_t *data, size_t len)
{
    (void)server; (void)arg; (void)data; (void)len;
    if (type == WS_EVT_CONNECT) {
        ESP_LOGD(TAG, "WS client #%u connected", client->id());
    } else if (type == WS_EVT_DISCONNECT) {
        ESP_LOGD(TAG, "WS client #%u disconnected", client->id());
    }
}

/* ============================================================
 * Route registration
 * ============================================================ */

static void register_routes(AsyncWebServer &srv)
{
    /* ── Static files ───────────────────────────────────────── */
    srv.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        serve_lfs(req, "/index.html", "text/html");
    });
    srv.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *req) {
        serve_lfs(req, "/style.css", "text/css");
    });
    srv.on("/app.js", HTTP_GET, [](AsyncWebServerRequest *req) {
        serve_lfs(req, "/app.js", "application/javascript");
    });
    /* /manifest.json — exposed as an HTTP endpoint so the asset version is
     * directly inspectable from a browser or curl. The firmware also reads
     * the same file internally in dm_status_snapshot() to populate
     * system.asset_version; the HTTP route is purely diagnostic and proves
     * which physical file is on the active LittleFS partition. */
    srv.on("/manifest.json", HTTP_GET, [](AsyncWebServerRequest *req) {
        serve_lfs(req, "/manifest.json", "application/json");
    });

    /* ── Whoami ─────────────────────────────────────────────── */
    /* Uses session_find_peek (no slide) so this probe call does not reset
     * the idle timeout — the browser polls this every 60 s to detect
     * server-side expiry without preventing it from happening. */
    srv.on("/api/whoami", HTTP_GET, [](AsyncWebServerRequest *req) {
        char token[TOKEN_LEN + 1] = {0};
        cookie_get_session(req, token);
        session_t role = session_find_peek(token);
        if (role == SESSION_NONE) {
            req->send(401, "application/json", "{\"ok\":false}");
        } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "{\"ok\":true,\"role\":\"%s\"}",
                     role == SESSION_ADMIN ? "admin" : "farmer");
            req->send(200, "application/json", buf);
        }
    });

    /* ── Login ──────────────────────────────────────────────── */
    srv.on("/api/login", HTTP_POST, [](AsyncWebServerRequest *req) {}, NULL,
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
        char body[256] = {};
        memcpy(body, data, len < sizeof(body) - 1 ? len : sizeof(body) - 1);

        char role_str[12] = {}, pin_str[16] = {};
        json_get_str(body, "role", role_str, sizeof(role_str));
        json_get_str(body, "pin",  pin_str,  sizeof(pin_str));

        pin_role_t pr = (strcmp(role_str, "admin") == 0) ? PIN_ROLE_ADMIN : PIN_ROLE_FARMER;
        pin_auth_result_t res = pin_auth_verify(pr, pin_str);

        if (res == PIN_AUTH_OK) {
            cfg_shadow_t cfg; dm_cfg_snapshot(&cfg);
            char token[TOKEN_LEN + 1];
            session_t sess_role = (pr == PIN_ROLE_ADMIN) ? SESSION_ADMIN : SESSION_FARMER;
            session_create(sess_role, cfg.session_timeout_min, token);

            char cookie[TOKEN_LEN + 64];
            snprintf(cookie, sizeof(cookie),
                     "session=%s; Path=/; HttpOnly; SameSite=Strict", token);

            AsyncWebServerResponse *resp =
                req->beginResponse(200, "application/json",
                    pr == PIN_ROLE_ADMIN
                    ? "{\"ok\":true,\"role\":\"admin\"}"
                    : "{\"ok\":true,\"role\":\"farmer\"}");
            resp->addHeader("Set-Cookie", cookie);
            req->send(resp);

            ESP_LOGI(TAG, "Login OK: role=%s", role_str);
        } else if (res == PIN_AUTH_LOCKED_OUT) {
            req->send(200, "application/json",
                      "{\"ok\":false,\"locked\":true,\"remaining\":0}");
        } else {
            req->send(200, "application/json",
                      "{\"ok\":false,\"locked\":false}");
        }
    });

    /* ── Logout ─────────────────────────────────────────────── */
    /* Logout reads only the Cookie header — no request body is needed.
     * The handler MUST live in onRequest, not in the body callback, because
     * POST /api/logout is sent without a body and ESPAsyncWebServer does not
     * invoke the body callback for bodyless requests. */
    srv.on("/api/logout", HTTP_POST, [](AsyncWebServerRequest *req) {
        char token[TOKEN_LEN + 1];
        cookie_get_session(req, token);
        session_destroy(token);
        AsyncWebServerResponse *resp =
            req->beginResponse(200, "application/json", "{\"ok\":true}");
        resp->addHeader("Set-Cookie",
            "session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
        req->send(resp);
    });

    /* ── Status ─────────────────────────────────────────────── */
    /* 2 KB buffer for the canonical status JSON. Worst-case payload (every
     * tile present + every flag set) is ~720 bytes; 2 KB leaves headroom
     * for future schema additions. Must match the size passed to
     * build_status_json() below. */
    srv.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        char *buf = (char *)ps_malloc(2048);
        if (!buf) { req->send(500); return; }
        build_status_json(buf, 2048);
        req->send(200, "application/json", buf);
        free(buf);
    });

    /* ── Config limits (public — no auth required) ──────────── */
    /* Single source of truth: bounds are defined in config/cfg_limits.h and
     * baked into this static string at compile time via the _LIMITS_STR() macro.
     * app.js fetches this once at page load and applies min/max to every
     * <input> element, so the HTML never needs hardcoded range attributes. */
    srv.on("/api/config/limits", HTTP_GET, [](AsyncWebServerRequest *req) {
#define _LIMITS_STR2(x) #x
#define _LIMITS_STR(x)  _LIMITS_STR2(x)
        static const char LIMITS_JSON[] =
            "{"
            "\"t_max_day\":"      "[" _LIMITS_STR(CFG_MIN_T_MAX_DAY)   "," _LIMITS_STR(CFG_MAX_T_MAX_DAY)   "],"
            "\"t_min_day\":"      "[" _LIMITS_STR(CFG_MIN_T_MIN_DAY)   "," _LIMITS_STR(CFG_MAX_T_MIN_DAY)   "],"
            "\"t_max_ngt\":"      "[" _LIMITS_STR(CFG_MIN_T_MAX_NGT)   "," _LIMITS_STR(CFG_MAX_T_MAX_NGT)   "],"
            "\"t_min_ngt\":"      "[" _LIMITS_STR(CFG_MIN_T_MIN_NGT)   "," _LIMITS_STR(CFG_MAX_T_MIN_NGT)   "],"
            "\"rh_max_day\":"     "[" _LIMITS_STR(CFG_MIN_RH_MAX)      "," _LIMITS_STR(CFG_MAX_RH_MAX)      "],"
            "\"rh_min_day\":"     "[" _LIMITS_STR(CFG_MIN_RH_MIN)      "," _LIMITS_STR(CFG_MAX_RH_MIN)      "],"
            "\"rh_max_ngt\":"     "[" _LIMITS_STR(CFG_MIN_RH_MAX)      "," _LIMITS_STR(CFG_MAX_RH_MAX)      "],"
            "\"rh_min_ngt\":"     "[" _LIMITS_STR(CFG_MIN_RH_MIN)      "," _LIMITS_STR(CFG_MAX_RH_MIN)      "],"
            "\"hyst_t\":"         "[" _LIMITS_STR(CFG_MIN_HYST_T)      "," _LIMITS_STR(CFG_MAX_HYST_T)      "],"
            "\"hyst_rh\":"        "[" _LIMITS_STR(CFG_MIN_HYST_RH)     "," _LIMITS_STR(CFG_MAX_HYST_RH)     "],"
            "\"avg_win_t\":"      "[" _LIMITS_STR(CFG_MIN_AVG_WIN)     "," _LIMITS_STR(CFG_MAX_AVG_WIN)     "],"
            "\"avg_win_rh\":"     "[" _LIMITS_STR(CFG_MIN_AVG_WIN)     "," _LIMITS_STR(CFG_MAX_AVG_WIN)     "],"
            "\"v_max\":"          "[" _LIMITS_STR(CFG_MIN_V_MAX)        "," _LIMITS_STR(CFG_MAX_V_MAX)       "],"
            "\"dir_excl_low\":"   "[" _LIMITS_STR(CFG_MIN_DIR)          "," _LIMITS_STR(CFG_MAX_DIR)         "],"
            "\"dir_excl_high\":"  "[" _LIMITS_STR(CFG_MIN_DIR)          "," _LIMITS_STR(CFG_MAX_DIR)         "],"
            "\"travel_m1\":"      "[" _LIMITS_STR(CFG_MIN_TRAVEL_S)    "," _LIMITS_STR(CFG_MAX_TRAVEL_S)    "],"
            "\"travel_m2\":"      "[" _LIMITS_STR(CFG_MIN_TRAVEL_S)    "," _LIMITS_STR(CFG_MAX_TRAVEL_S)    "],"
            "\"travel_m3\":"      "[" _LIMITS_STR(CFG_MIN_TRAVEL_S)    "," _LIMITS_STR(CFG_MAX_TRAVEL_S)    "],"
            "\"dwell_open_m1\":"  "[" _LIMITS_STR(CFG_MIN_DWELL_OPEN_S) "," _LIMITS_STR(CFG_MAX_DWELL_OPEN_S) "],"
            "\"dwell_open_m2\":"  "[" _LIMITS_STR(CFG_MIN_DWELL_OPEN_S) "," _LIMITS_STR(CFG_MAX_DWELL_OPEN_S) "],"
            "\"dwell_open_m3\":"  "[" _LIMITS_STR(CFG_MIN_DWELL_OPEN_S) "," _LIMITS_STR(CFG_MAX_DWELL_OPEN_S) "],"
            "\"dwell_close_m1\":" "[" _LIMITS_STR(CFG_MIN_DWELL_CLOSE_S) "," _LIMITS_STR(CFG_MAX_DWELL_CLOSE_S) "],"
            "\"dwell_close_m2\":" "[" _LIMITS_STR(CFG_MIN_DWELL_CLOSE_S) "," _LIMITS_STR(CFG_MAX_DWELL_CLOSE_S) "],"
            "\"dwell_close_m3\":" "[" _LIMITS_STR(CFG_MIN_DWELL_CLOSE_S) "," _LIMITS_STR(CFG_MAX_DWELL_CLOSE_S) "],"
            "\"poll_interval\":"  "[" _LIMITS_STR(CFG_MIN_POLL_S)      "," _LIMITS_STR(CFG_MAX_POLL_S)      "],"
            "\"session_timeout\":" "[" _LIMITS_STR(CFG_MIN_TIMEOUT_MIN) "," _LIMITS_STR(CFG_MAX_TIMEOUT_MIN) "],"
            "\"ap_timeout\":"     "[" _LIMITS_STR(CFG_MIN_AP_TIMEOUT)  "," _LIMITS_STR(CFG_MAX_TIMEOUT_MIN) "]"
            "}";
#undef _LIMITS_STR2
#undef _LIMITS_STR
        req->send(200, "application/json", LIMITS_JSON);
    });

    /* ── Config GET ─────────────────────────────────────────── */
    srv.on("/api/config", HTTP_GET, [](AsyncWebServerRequest *req) {
        session_t role = req_role(req);
        if (role == SESSION_NONE) {
            req->send(401, "application/json", "{\"ok\":false}"); return;
        }
        char *buf = (char *)ps_malloc(1024);
        if (!buf) { req->send(500); return; }
        build_config_json(buf, 1024);
        req->send(200, "application/json", buf);
        free(buf);
    });

    /* ── Config POST ────────────────────────────────────────── */
    srv.on("/api/config", HTTP_POST, [](AsyncWebServerRequest *req) {}, NULL,
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
        session_t role = req_role(req);
        if (role == SESSION_NONE) {
            req->send(401, "application/json", "{\"ok\":false}"); return;
        }
        char body[256] = {};
        memcpy(body, data, len < sizeof(body) - 1 ? len : sizeof(body) - 1);

        char ns[16] = {}, key[32] = {}, str_value[80] = {};
        int32_t int_value = 0;
        bool has_int = json_get_int(body, "value", &int_value);
        bool has_str = json_get_str(body, "str_value", str_value, sizeof(str_value));

        if (!json_get_str(body, "ns",  ns,  sizeof(ns))  ||
            !json_get_str(body, "key", key, sizeof(key))  ||
            (!has_int && !has_str))
        {
            req->send(400, "application/json", "{\"ok\":false,\"err\":\"bad request\"}");
            return;
        }

        /* Farmer can only write farmer-level keys */
        if (role == SESSION_FARMER && !is_farmer_key(ns, key)) {
            req->send(403, "application/json", "{\"ok\":false,\"err\":\"forbidden\"}");
            return;
        }

        if (has_str) {
            /* String value — write directly to NVS (e.g. tz_str) */
            nvs_cfg_set_str(ns, key, str_value);
            /* Apply timezone immediately so localtime_r uses the new zone
             * without requiring a reboot.  The same call is made at boot
             * in data_manager.cpp::nvs_load_system(). */
            if (strcmp(key, "tz_str") == 0 && str_value[0] != '\0') {
                setenv("TZ", str_value, 1);
                tzset();
            }
            req->send(200, "application/json", "{\"ok\":true}");
        } else {
            /* Integer value — post via Q4 so T4 validates and persists */
            config_update_t upd = {};
            snprintf(upd.ns,  sizeof(upd.ns),  "%s", ns);
            snprintf(upd.key, sizeof(upd.key), "%s", key);
            upd.value = int_value;
            if (xQueueSend(Q4, &upd, pdMS_TO_TICKS(500)) == pdTRUE) {
                req->send(200, "application/json", "{\"ok\":true}");
            } else {
                req->send(503, "application/json",
                          "{\"ok\":false,\"err\":\"Q4 full\"}");
            }
        }
    });

    /* ── WiFi credentials ───────────────────────────────────── */
    srv.on("/api/wifi", HTTP_POST, [](AsyncWebServerRequest *req) {}, NULL,
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
        if (req_role(req) != SESSION_ADMIN) {
            req->send(403, "application/json", "{\"ok\":false,\"err\":\"admin only\"}");
            return;
        }
        char body[256] = {};
        memcpy(body, data, len < sizeof(body) - 1 ? len : sizeof(body) - 1);

        char ssid[64] = {}, psk[64] = {}, ap_psk[64] = {};
        bool has_ssid   = json_get_str(body, "ssid",   ssid,   sizeof(ssid));
        bool has_psk    = json_get_str(body, "psk",    psk,    sizeof(psk));
        bool has_ap_psk = json_get_str(body, "ap_psk", ap_psk, sizeof(ap_psk));

        if (has_ssid)              nvs_cfg_set_str("wifi", "ssid",   ssid);
        if (has_psk && psk[0])    nvs_cfg_set_str("wifi", "psk",    psk);    /* never blank PSK */
        if (has_ap_psk && ap_psk[0]) nvs_cfg_set_str("wifi", "ap_psk", ap_psk);

        ESP_LOGI(TAG, "WiFi creds updated via web: ssid=%s", has_ssid ? ssid : "(unchanged)");
        req->send(200, "application/json", "{\"ok\":true}");
    });

    /* ── Status website / Web tab (admin) ───────────────────── */
    /* GET — returns the current web-tab settings plus the live last-attempt
     * indicators. The shared secret is intentionally never echoed; the UI
     * uses an empty input + "send empty to keep" semantics on POST.        */
    srv.on("/api/web", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (req_role(req) != SESSION_ADMIN) {
            req->send(403, "application/json", "{\"ok\":false,\"err\":\"admin only\"}");
            return;
        }
        cfg_shadow_t c;
        dm_cfg_snapshot(&c);

        char last_post[48] = {};
        char last_log[48]  = {};
        status_post_last_str(last_post, sizeof(last_post));
        status_post_last_log_str(last_log, sizeof(last_log));

        char buf[640];
        snprintf(buf, sizeof(buf),
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
            c.status_url,
            (long)c.status_interval_s,
            (long)c.status_enable,
            (long)c.status_expose,
            (long)c.log_upload_h,
            (long)c.log_upload_m,
            (long)c.log_upload_rot,
            last_post, last_log, c.log_last_up);
        req->send(200, "application/json", buf);
    });

    /* POST — single-transaction Apply. Validates bounds before any NVS
     * write; on success notifies T4 to reload the cfg shadow.             */
    srv.on("/api/web", HTTP_POST, [](AsyncWebServerRequest *req) {}, NULL,
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
        if (req_role(req) != SESSION_ADMIN) {
            req->send(403, "application/json", "{\"ok\":false,\"err\":\"admin only\"}");
            return;
        }
        char body[640] = {};
        memcpy(body, data, len < sizeof(body) - 1 ? len : sizeof(body) - 1);

        char    url[CFG_MAX_URL_LEN + 1]    = {};
        char    secret[CFG_MAX_SECRET_LEN + 1] = {};
        int32_t interval = 0, enable = 0, expose = 0;
        int32_t log_h = 0, log_m = 0, log_rot = 0;
        bool h_url    = json_get_str(body, "url",    url,    sizeof(url));
        bool h_sec    = json_get_str(body, "secret", secret, sizeof(secret));
        bool h_iv     = json_get_int(body, "interval_s", &interval);
        bool h_en     = json_get_int(body, "enable",     &enable);
        bool h_ex     = json_get_int(body, "expose",     &expose);
        bool h_lh     = json_get_int(body, "log_h",      &log_h);
        bool h_lm     = json_get_int(body, "log_m",      &log_m);
        bool h_lr     = json_get_int(body, "log_rot",    &log_rot);

        /* URL: empty disables the feature; non-empty must use a known scheme,
         * carry no query/fragment (T14 appends ?action=log itself), and end
         * in "api.php" — Apache routing varies and HTTPClient does not follow
         * redirects, so requiring the exact endpoint avoids silent FAILs. */
        if (h_url && url[0] != '\0') {
            if (strncmp(url, "http://",  7) != 0 &&
                strncmp(url, "https://", 8) != 0) {
                req->send(400, "application/json",
                          "{\"ok\":false,\"err\":\"URL must start with http:// or https://\"}");
                return;
            }
            if (strchr(url, '?') != NULL || strchr(url, '#') != NULL) {
                req->send(400, "application/json",
                          "{\"ok\":false,\"err\":\"URL must not contain ? or #\"}");
                return;
            }
            const size_t ulen = strlen(url);
            const char  *suffix = "api.php";
            const size_t slen = 7u;
            if (ulen < slen || strcmp(url + ulen - slen, suffix) != 0) {
                req->send(400, "application/json",
                          "{\"ok\":false,\"err\":\"URL must end with \\\"api.php\\\"\"}");
                return;
            }
        }
        /* Secret: empty = keep existing; non-empty must meet minimum length. */
        if (h_sec && secret[0] != '\0' &&
            strlen(secret) < (size_t)CFG_MIN_SECRET_LEN) {
            req->send(400, "application/json",
                      "{\"ok\":false,\"err\":\"secret too short\"}");
            return;
        }
        if (h_iv && (interval < CFG_MIN_STATUS_INTERVAL_S || interval > CFG_MAX_STATUS_INTERVAL_S)) {
            req->send(400, "application/json",
                      "{\"ok\":false,\"err\":\"interval out of range\"}");
            return;
        }
        if (h_lh && (log_h < CFG_MIN_HOUR   || log_h > CFG_MAX_HOUR))   { goto bad; }
        if (h_lm && (log_m < CFG_MIN_MINUTE || log_m > CFG_MAX_MINUTE)) { goto bad; }
        if (h_en && (enable  < 0 || enable  > 1))  { goto bad; }
        if (h_lr && (log_rot < 0 || log_rot > 1))  { goto bad; }
        if (h_ex && (expose  < 0 || expose  > 0x3F)) { goto bad; }

        if (h_url)                  nvs_cfg_set_str(NVS_NS_SYSTEM, "status_url",     url);
        if (h_sec && secret[0])     nvs_cfg_set_str(NVS_NS_SYSTEM, "status_secret",  secret);
        if (h_iv)                   nvs_cfg_set_i32(NVS_NS_SYSTEM, "status_intv_s",  interval);
        if (h_en)                   nvs_cfg_set_i32(NVS_NS_SYSTEM, "status_enable",  enable);
        if (h_ex)                   nvs_cfg_set_i32(NVS_NS_SYSTEM, "status_expose",  expose);
        if (h_lh)                   nvs_cfg_set_i32(NVS_NS_SYSTEM, "log_upload_h",   log_h);
        if (h_lm)                   nvs_cfg_set_i32(NVS_NS_SYSTEM, "log_upload_m",   log_m);
        if (h_lr)                   nvs_cfg_set_i32(NVS_NS_SYSTEM, "log_upload_rot", log_rot);

        dm_reload_web_cfg();   /* TN5 → T4 republishes under MX4 */

        ESP_LOGI(TAG, "Web cfg updated: url=%s interval=%ld enable=%ld expose=0x%02lX",
                 h_url ? url : "(unchanged)",
                 (long)interval, (long)enable, (long)expose);
        req->send(200, "application/json", "{\"ok\":true}");
        return;

bad:
        req->send(400, "application/json",
                  "{\"ok\":false,\"err\":\"bounds\"}");
    });

    /* ── PIN change ─────────────────────────────────────────── */
    srv.on("/api/pin", HTTP_POST, [](AsyncWebServerRequest *req) {}, NULL,
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
        if (req_role(req) != SESSION_ADMIN) {
            req->send(403, "application/json", "{\"ok\":false,\"err\":\"admin only\"}");
            return;
        }
        char body[128] = {};
        memcpy(body, data, len < sizeof(body) - 1 ? len : sizeof(body) - 1);

        char role_str[12] = {}, pin_str[16] = {};
        json_get_str(body, "role", role_str, sizeof(role_str));
        json_get_str(body, "pin",  pin_str,  sizeof(pin_str));

        pin_role_t pr = (strcmp(role_str, "admin") == 0) ? PIN_ROLE_ADMIN : PIN_ROLE_FARMER;
        pin_auth_result_t res = pin_auth_set(pr, pin_str);
        if (res == PIN_AUTH_OK) {
            ESP_LOGI(TAG, "PIN changed for role=%s", role_str);
            req->send(200, "application/json", "{\"ok\":true}");
        } else {
            req->send(200, "application/json",
                      "{\"ok\":false,\"err\":\"pin_auth_set failed\"}");
        }
    });

    /* ── Sensor history ─────────────────────────────────────── */
    srv.on("/api/history", HTTP_GET, [](AsyncWebServerRequest *req) {
        int n = HIST_MAX_ROWS;
        if (req->hasParam("n")) n = req->getParam("n")->value().toInt();
        if (n < 1 || n > HIST_MAX_ROWS) n = HIST_MAX_ROWS;

        sensor_reading_t *rows =
            (sensor_reading_t *)ps_malloc((size_t)n * sizeof(sensor_reading_t));
        if (!rows) { req->send(500); return; }

        /* Read the NEWEST n entries.  dm_ring_read uses a logical offset from
         * the oldest entry, so compute offset = max(0, avail - n). */
        uint16_t avail  = dm_ring_count();
        uint16_t offset = (avail > (uint16_t)n) ? (uint16_t)(avail - (uint16_t)n) : 0u;
        uint16_t got    = 0;
        dm_ring_read(offset, rows, (uint16_t)n, &got);

        /* Build JSON array — allocate from PSRAM.  Per-row payload grew from
         * ~85 chars (legacy 4 columns) to ~160 chars (1.17.21+: raw + avg
         * for T/RH/wind speed, plus direction and direction_variation). At
         * 60 rows that's ~10 KB; 12 KB gives 20 % headroom. Field names
         * mirror the canonical status JSON so the same numbers carry the
         * same key on /api/status, /ws and /api/history. */
        const size_t HIST_BUF = 12288;
        char *buf = (char *)ps_malloc(HIST_BUF);
        if (!buf) { free(rows); req->send(500); return; }

        int pos = 0;
        pos += snprintf(buf + pos, HIST_BUF - pos, "{\"rows\":[");
        for (uint16_t i = 0; i < got; i++) {
            int written = snprintf(buf + pos, HIST_BUF - (size_t)pos,
                "%s{\"ts\":%lu,"
                "\"temp_c\":%.1f,\"temp_avg_c\":%.1f,"
                "\"rh_pct\":%u,\"rh_avg_pct\":%u,"
                "\"speed_ms\":%.1f,\"speed_avg_ms\":%.1f,"
                "\"direction_deg\":%u,\"direction_variation_deg\":%u}",
                i ? "," : "",
                (unsigned long)rows[i].timestamp,
                (float)rows[i].temperature_c,
                (float)rows[i].t_avg_c,
                rows[i].humidity_pct,
                rows[i].rh_avg_pct,
                rows[i].wind_speed_ms10     / 10.0f,
                rows[i].wind_speed_avg_ms10 / 10.0f,
                rows[i].wind_dir_deg,
                rows[i].wind_dir_variation_deg);
            if (written < 0 || pos + written >= (int)HIST_BUF - 4) break;
            pos += written;
        }
        snprintf(buf + pos, HIST_BUF - (size_t)pos, "]}");

        req->send(200, "application/json", buf);
        free(rows);
        free(buf);
    });

    /* ── SD card status ─────────────────────────────────────── */
    srv.on("/api/sd/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        /* Public — visible on the status page without login. */
        bool mounted   = storage_sd_available();
        uint64_t total = mounted ? storage_sd_total_bytes() : 0;
        uint64_t free_b = mounted ? storage_sd_free_bytes()  : 0;
        char buf[128];
        snprintf(buf, sizeof(buf),
            "{\"mounted\":%s,\"free_mb\":%lu,\"size_mb\":%lu}",
            mounted ? "true" : "false",
            (unsigned long)(free_b  / (1024UL * 1024UL)),
            (unsigned long)(total   / (1024UL * 1024UL)));
        req->send(200, "application/json", buf);
    });

    /* ── SD card mount ──────────────────────────────────────── */
    srv.on("/api/sd/mount", HTTP_POST, [](AsyncWebServerRequest *req) {}, NULL,
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
        (void)data; (void)len;
        if (req_role(req) != SESSION_ADMIN) {
            req->send(403, "application/json", "{\"ok\":false,\"err\":\"admin only\"}");
            return;
        }
        if (event_logger_sd_remount()) {
            req->send(200, "application/json", "{\"ok\":true}");
        } else {
            req->send(200, "application/json", "{\"ok\":false,\"err\":\"mount failed\"}");
        }
    });

    /* ── SD card unmount ────────────────────────────────────── */
    srv.on("/api/sd/unmount", HTTP_POST, [](AsyncWebServerRequest *req) {}, NULL,
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
        (void)data; (void)len;
        if (req_role(req) != SESSION_ADMIN) {
            req->send(403, "application/json", "{\"ok\":false,\"err\":\"admin only\"}");
            return;
        }
        event_logger_sd_unmount();
        req->send(200, "application/json", "{\"ok\":true}");
    });

    /* ── Log: list available sources ───────────────────────── */
    srv.on("/api/log/files", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (req_role(req) != SESSION_ADMIN) {
            req->send(403, "application/json", "{\"ok\":false}"); return;
        }
        uint32_t nvs_cnt = nvs_log_count();

        /* Collect SD CSV filenames (comma-separated) */
        const size_t LIST_LEN = 512u;
        char *list_buf = (char *)ps_malloc(LIST_LEN);
        if (!list_buf) { req->send(500); return; }
        list_buf[0] = '\0';
        if (storage_sd_available()) {
            storage_sd_list_csv(".csv", list_buf, LIST_LEN);
        }

        /* Collect filenames into a small array and sort lexicographically
         * (lexicographic order = chronological for YYYYMMDDHHMMSS names). */
        static const int LOG_FILES_MAX = 12;   /* SD_MAX_FILES + headroom */
        static const int LOG_FNAME_MAX = 20;   /* "YYYYMMDDHHMMSS.csv\0" */
        char  names[LOG_FILES_MAX][LOG_FNAME_MAX];
        int   n_names = 0;
        char *tok = strtok(list_buf, ",");
        while (tok && n_names < LOG_FILES_MAX) {
            while (*tok == ' ') tok++;
            if (*tok) {
                strncpy(names[n_names], tok, LOG_FNAME_MAX - 1);
                names[n_names][LOG_FNAME_MAX - 1] = '\0';
                n_names++;
            }
            tok = strtok(nullptr, ",");
        }
        /* Bubble sort — at most 10 entries, negligible cost. */
        for (int i = 0; i < n_names - 1; i++) {
            for (int j = 0; j < n_names - 1 - i; j++) {
                if (strcmp(names[j], names[j + 1]) > 0) {
                    char tmp[LOG_FNAME_MAX];
                    memcpy(tmp,         names[j],     LOG_FNAME_MAX);
                    memcpy(names[j],    names[j + 1], LOG_FNAME_MAX);
                    memcpy(names[j + 1], tmp,         LOG_FNAME_MAX);
                }
            }
        }

        /* Build JSON */
        const size_t OUT_LEN = 1024u;
        char *out = (char *)ps_malloc(OUT_LEN);
        if (!out) { free(list_buf); req->send(500); return; }

        int pos = snprintf(out, OUT_LEN, "{\"nvs_count\":%lu,\"sd_files\":[",
                           (unsigned long)nvs_cnt);
        for (int i = 0; i < n_names && (size_t)pos < OUT_LEN - 32u; i++) {
            pos += snprintf(out + pos, OUT_LEN - (size_t)pos,
                            "%s\"%s\"", i ? "," : "", names[i]);
        }
        snprintf(out + pos, OUT_LEN - (size_t)pos, "]}");

        req->send(200, "application/json", out);
        free(list_buf);
        free(out);
    });

    /* ── Log: download as CSV ───────────────────────────────── */
    srv.on("/api/log/download", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (req_role(req) != SESSION_ADMIN) {
            req->send(403); return;
        }

        static const char * const TYPE_NAMES[] = {
            "SENSOR","RELAY","MODE","SETPT","SESSION","ALARM","SYSTEM"
        };
        static const char * const INIT_NAMES[] = {
            "SYS","FARMER","ADMIN","MQTT","WEB"
        };

        const char *src = req->hasParam("src")
                          ? req->getParam("src")->value().c_str() : "nvs";

        if (strcmp(src, "nvs") == 0) {
            /* Export NVS ring buffer as CSV */
            uint32_t cnt = nvs_log_count();
            /* Each line: "YYYY-MM-DDTHH:MM:SS,SENSOR,FARMER,255,255,-32768,-32768\n"
             * is ≤ 56 chars; 80 bytes per entry gives comfortable headroom. */
            size_t csv_len = (size_t)(cnt + 2u) * 80u + 64u;
            char *csv = (char *)ps_malloc(csv_len);
            if (!csv) { req->send(500); return; }

            int pos = snprintf(csv, csv_len,
                "timestamp,type,initiator,ch,param,value_a,value_b\n");

            if (cnt > 0u) {
                log_entry_t *entries =
                    (log_entry_t *)ps_malloc(cnt * sizeof(log_entry_t));
                if (entries) {
                    uint32_t got = 0;
                    nvs_log_read(0, entries, cnt, &got);
                    for (uint32_t i = 0; i < got; i++) {
                        if ((size_t)pos >= csv_len - 80u) break;
                        const log_entry_t &e = entries[i];
                        const char *tname = (e.event_type < 7u)
                                            ? TYPE_NAMES[e.event_type] : "?";
                        const char *iname = (e.initiator  < 5u)
                                            ? INIT_NAMES[e.initiator]  : "?";
                        /* ISO 8601 UTC timestamp */
                        time_t ts = (time_t)e.timestamp;
                        struct tm tm_utc;
                        gmtime_r(&ts, &tm_utc);
                        char ts_str[20];
                        strftime(ts_str, sizeof(ts_str),
                                 "%Y-%m-%dT%H:%M:%S", &tm_utc);
                        pos += snprintf(csv + pos, csv_len - (size_t)pos,
                            "%s,%s,%s,%u,%u,%d,%d\n",
                            ts_str, tname, iname,
                            (unsigned)e.channel, (unsigned)e.param_id,
                            (int)e.value_a, (int)e.value_b);
                    }
                    free(entries);
                }
            }

            AsyncWebServerResponse *resp =
                req->beginResponse(200, "text/csv", csv);
            resp->addHeader("Content-Disposition",
                            "attachment; filename=\"nvs_log.csv\"");
            req->send(resp);
            free(csv);

        } else if (strcmp(src, "sd") == 0) {
            if (!req->hasParam("file")) { req->send(400); return; }
            const String &fname_param = req->getParam("file")->value();

            /* Reject path traversal */
            if (strchr(fname_param.c_str(), '/') ||
                strstr(fname_param.c_str(), "..")) {
                req->send(400); return;
            }
            if (!storage_sd_available()) {
                req->send(503, "application/json",
                          "{\"ok\":false,\"err\":\"SD not mounted\"}");
                return;
            }
            char abs_path[48];
            snprintf(abs_path, sizeof(abs_path), "/%s", fname_param.c_str());

            uint32_t fsize = storage_sd_file_size(abs_path);
            if (fsize == 0u) { req->send(404); return; }

            char *buf = (char *)ps_malloc((size_t)fsize + 1u);
            if (!buf) { req->send(500); return; }

            size_t got = 0;
            storage_sd_read(abs_path, 0, buf, (size_t)fsize + 1u, &got);
            buf[got] = '\0';

            char disp[80];
            snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"",
                     fname_param.c_str());

            AsyncWebServerResponse *resp =
                req->beginResponse(200, "text/csv", buf);
            resp->addHeader("Content-Disposition", disp);
            req->send(resp);
            free(buf);

        } else {
            req->send(400);
        }
    });

    /* ── OTA status ────────────────────────────────────────── */
    srv.on("/api/ota/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (req_role(req) == SESSION_NONE) {
            req->send(401, "application/json", "{\"ok\":false}"); return;
        }
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

        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"state\":\"%s\",\"progress\":%u,\"error\":\"%s\","
            "\"bank\":\"%c\",\"accepted\":%s}",
            sname, (unsigned)pct, err ? err : "",
            bank, acc ? "true" : "false");
        req->send(200, "application/json", buf);
    });

    /* ── OTA firmware upload ────────────────────────────────── */
    /*
     * Accepts a raw .bin firmware image as the POST body.  Chunks arrive via
     * the body callback; T13 OTA logic is called inline (no separate task for
     * the write itself — streaming OTA needs no staging buffer).
     *
     * On success the device reboots after 1 s (response is sent first).
     * On error after the first chunk was accepted, subsequent body callbacks
     * are silently skipped (the response was already sent on the error chunk).
     */
    srv.on("/api/ota/firmware", HTTP_POST,
        [](AsyncWebServerRequest *req) { (void)req; },
        NULL,
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len,
           size_t index, size_t total) {
            if (req_role(req) != SESSION_ADMIN) {
                req->send(403, "application/json",
                          "{\"ok\":false,\"err\":\"admin only\"}");
                return;
            }
            ota_state_t cur = ota_get_state();
            if (index == 0) {
                /* First chunk — begin OTA. */
                if (!ota_firmware_begin(total)) {
                    req->send(500, "application/json",
                        "{\"ok\":false,\"err\":\"OTA begin failed\"}");
                    return;
                }
            } else if (cur == OTA_STATE_ERROR || cur == OTA_STATE_IDLE) {
                /* Error already set on a previous chunk; response already sent. */
                return;
            }

            if (!ota_firmware_write(data, len)) {
                req->send(500, "application/json",
                    "{\"ok\":false,\"err\":\"OTA write failed\"}");
                return;
            }

            if (index + len >= total) {
                /* Last chunk. */
                if (!ota_firmware_end()) {
                    req->send(500, "application/json",
                        "{\"ok\":false,\"err\":\"OTA verify failed\"}");
                    return;
                }
                req->send(200, "application/json",
                    "{\"ok\":true,\"rebooting\":false,"
                    "\"awaiting_assets\":true}");
            }
        });

    /* ── OTA web-asset upload ───────────────────────────────── */
    /*
     * Accepts a STORE-only .zip archive as the POST body.  The ZIP is
     * accumulated entirely in PSRAM; on receipt of the last chunk T13 is
     * spawned to extract it to the inactive LittleFS partition.
     * The response is 202 Accepted; the caller should poll GET /api/ota/status
     * for extraction progress.
     *
     * Build the archive with:
     *   zip -0 assets.zip index.html style.css app.js
     */
    srv.on("/api/ota/assets", HTTP_POST,
        [](AsyncWebServerRequest *req) { (void)req; },
        NULL,
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len,
           size_t index, size_t total) {
            if (req_role(req) != SESSION_ADMIN) {
                req->send(403, "application/json",
                          "{\"ok\":false,\"err\":\"admin only\"}");
                return;
            }
            ota_state_t cur = ota_get_state();
            if (index == 0) {
                if (!ota_assets_begin(total)) {
                    req->send(500, "application/json",
                        "{\"ok\":false,\"err\":\"OTA assets begin failed\"}");
                    return;
                }
            } else if (cur == OTA_STATE_ERROR) {
                return;  /* Previous chunk already failed and sent a response. */
            }

            if (!ota_assets_accumulate(data, len, index)) {
                req->send(500, "application/json",
                    "{\"ok\":false,\"err\":\"OTA assets accumulate failed\"}");
                return;
            }

            if (index + len >= total) {
                if (!ota_assets_end()) {
                    req->send(500, "application/json",
                        "{\"ok\":false,\"err\":\"OTA assets spawn failed\"}");
                    return;
                }
                req->send(202, "application/json",
                    "{\"ok\":true,"
                    "\"message\":\"extracting — poll GET /api/ota/status\"}");
            }
        });

    /* ── 404 fallback ───────────────────────────────────────── */
    srv.onNotFound([](AsyncWebServerRequest *req) {
        req->send(404, "text/plain", "Not found");
    });
}

/* ============================================================
 * Task entry point
 * ============================================================ */

void task_web_server(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "T11 task alive");

    /* ── Session mutex ── */
    s_sess_mux = xSemaphoreCreateMutex();
    configASSERT(s_sess_mux);
    memset(s_sessions, 0, sizeof(s_sessions));

    /* ── Mount active LittleFS partition ── */
    s_lfs_part = littlefs_active_partition();
    lfs_status_t lfs_st = littlefs_mount(s_lfs_part);
    if (lfs_st != LFS_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed (part=%d, err=%d) — web UI unavailable",
                 (int)s_lfs_part, (int)lfs_st);
    } else {
        ESP_LOGI(TAG, "LittleFS partition %c mounted OK",
                 s_lfs_part == LFS_PARTITION_A ? 'A' : 'B');
        bool has_index = littlefs_exists(s_lfs_part, "/index.html");
        ESP_LOGI(TAG, "index.html: %s", has_index ? "present" : "NOT FOUND — run uploadfs");
    }

    /* ── Set up server + WebSocket ── */
    static AsyncWebServer server(80);
    s_ws.onEvent(on_ws_event);
    server.addHandler(&s_ws);
    register_routes(server);
    server.begin();
    ESP_LOGI(TAG, "HTTP server started on port 80");

    /* ── Main loop: periodic WebSocket status push ── */
    /* 2 KB push buffer matches the /api/status allocation. Canonical JSON
     * worst case is ~720 bytes; the larger size leaves headroom and keeps
     * the two surfaces' buffer sizing in lockstep. */
    TickType_t last_push = xTaskGetTickCount();
    char *push_buf = (char *)ps_malloc(2048);
    if (!push_buf) {
        ESP_LOGE(TAG, "ps_malloc failed for WS push buffer");
        push_buf = nullptr;
    }

    for (;;) {
        vTaskDelayUntil(&last_push, pdMS_TO_TICKS(WS_PUSH_MS));

        /* Clean up stale WebSocket clients */
        s_ws.cleanupClients();

        if (push_buf && s_ws.count() > 0) {
            build_status_json(push_buf, 2048);
            s_ws.textAll(push_buf);
        }
    }
}
