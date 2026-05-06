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
 *  GET  /api/config     → all configuration parameters as JSON
 *  POST /api/config     → {ns, key, value} (int) or {ns, key, str_value} (string)
 *  POST /api/wifi       → {ssid, psk} or {ap_psk} — writes directly to NVS wifi ns
 *  POST /api/pin        → {role, pin} — admin-only; changes farmer/admin PIN
 *  GET  /api/history    → ?n=N — last N sensor ring buffer entries as JSON
 *  GET  /api/sd/status  → {mounted, free_mb, size_mb} — SD card state (farmer+admin)
 *  POST /api/sd/mount   → {} — mount SD card (admin)
 *  POST /api/sd/unmount → {} — unmount SD card (admin)
 *  WS   /ws             → push status JSON every WS_PUSH_MS
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
#include "littlefs_storage.h"
#include "nvs_config.h"
#include "../../../drivers/sdCard/src/sd_storage.h"
#include "../ota_manager/ota_manager.h"

static const char *TAG = "T11_WEB";

/* ============================================================
 * Compile-time constants
 * ============================================================ */
#define WS_PUSH_MS       2000u  /**< WebSocket status push interval */
#define MAX_SESSIONS        4   /**< Max concurrent web sessions */
#define TOKEN_LEN          16   /**< Session token length (hex chars) */
#define LFS_BUF_SIZE    32768u  /**< Max LittleFS file read buffer (PSRAM) */
#define HIST_MAX_ROWS      60   /**< Max history rows returned by /api/history */

/* Farmer-visible NVS keys: POST /api/config accepts these without admin */
#define FARMER_NS  "climate"
static const char * const FARMER_KEYS[] = {
    "t_max_day","t_min_day","t_max_ngt","t_min_ngt",
    "rh_max_day","rh_min_day","rh_max_ngt","rh_min_ngt",
    "rh_ctrl_en",
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

/** Serve a file from LittleFS. Falls back to 404 if not found. */
static void serve_lfs(AsyncWebServerRequest *req, const char *path, const char *ct)
{
    char *buf = (char *)ps_malloc(LFS_BUF_SIZE);
    if (!buf) { req->send(500, "text/plain", "OOM"); return; }

    lfs_status_t st = littlefs_read(s_lfs_part, path, buf, LFS_BUF_SIZE);
    if (st == LFS_OK) {
        req->send(200, ct, buf);
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
    sensor_reading_t meas;
    bool meas_valid;
    dm_meas_snapshot(&meas, &meas_valid);

    cfg_shadow_t cfg;
    dm_cfg_snapshot(&cfg);

    window_state_t wins[3];
    t2_get_window_states(wins);

    EventBits_t eg1 = xEventGroupGetBits(EG1);

    /* Derive operating mode from EG1 */
    const char *mode_str;
    if      (eg1 & EG1_BIT_MOTOR_ALARM)   mode_str = "MOTOR_ALARM";
    else if (eg1 & EG1_BIT_WIND_OVERRIDE)  mode_str = "WIND_OVERRIDE";
    else if (eg1 & EG1_BIT_CALIBRATING)    mode_str = "WINDOW_CAL";
    else                                    mode_str = "AUTOMATIC";

    static const char * const WIN_STR[] = {
        "UNKNOWN","CLOSED","MOVING_OPEN","OPEN","MOVING_CLOSE"
    };
    const char *w0 = (wins[0] < 5) ? WIN_STR[wins[0]] : "UNKNOWN";
    const char *w1 = (wins[1] < 5) ? WIN_STR[wins[1]] : "UNKNOWN";
    const char *w2 = (wins[2] < 5) ? WIN_STR[wins[2]] : "UNKNOWN";

    /* ISO-8601 time string */
    char tstr[25] = "—";
    if (cfg.current_unix_ts > 1000000) {
        struct tm tm_info;
        time_t ts = (time_t)cfg.current_unix_ts;
        localtime_r(&ts, &tm_info);
        strftime(tstr, sizeof(tstr), "%Y-%m-%dT%H:%M:%S", &tm_info);
    }

    /* Wifi */
    bool ntp_synced = (cfg.current_unix_ts > 1700000000UL);
    int  rssi       = WiFi.isConnected() ? (int)WiFi.RSSI() : 0;

    snprintf(buf, len,
        "{\"type\":\"status\","
        "\"temp_c\":%.1f,\"temp_avg\":%.1f,"
        "\"rh_pct\":%u,\"rh_avg\":%u,"
        "\"wind_ms\":%.1f,\"wind_dir\":%u,"
        "\"wind_avg\":%.1f,\"wind_avg_dir\":%u,"
        "\"windows\":[\"%s\",\"%s\",\"%s\"],"
        "\"mode\":\"%s\","
        "\"is_daytime\":%s,"
        "\"sunrise_utc\":%ld,\"sunset_utc\":%ld,"
        "\"eg1\":%lu,"
        "\"time\":\"%s\","
        "\"ntp_synced\":%s,"
        "\"wifi_ip\":\"%s\","
        "\"wifi_rssi\":%d,"
        "\"fw_ver\":\"" FIRMWARE_VERSION "\"}",
        meas_valid ? (float)meas.t_avg_c   : 0.0f,
        meas_valid ? (float)meas.t_avg_c   : 0.0f,
        meas_valid ? meas.rh_avg_pct        : 0u,
        meas_valid ? meas.rh_avg_pct        : 0u,
        meas_valid ? meas.wind_speed_avg_ms10 / 10.0f : 0.0f,
        meas_valid ? meas.wind_dir_avg_deg  : 0u,
        meas_valid ? meas.wind_speed_avg_ms10 / 10.0f : 0.0f,
        meas_valid ? meas.wind_dir_avg_deg  : 0u,
        w0, w1, w2,
        mode_str,
        cfg.is_daytime ? "true" : "false",
        (long)cfg.sunrise_mins_utc, (long)cfg.sunset_mins_utc,
        (unsigned long)eg1,
        tstr,
        ntp_synced ? "true" : "false",
        WiFi.isConnected() ? WiFi.localIP().toString().c_str() : "",
        rssi
    );
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

    /* ── Whoami ─────────────────────────────────────────────── */
    srv.on("/api/whoami", HTTP_GET, [](AsyncWebServerRequest *req) {
        session_t role = req_role(req);
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
    srv.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        char *buf = (char *)ps_malloc(1024);
        if (!buf) { req->send(500); return; }
        build_status_json(buf, 1024);
        req->send(200, "application/json", buf);
        free(buf);
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

        uint16_t got = 0;
        dm_ring_read(0, rows, (uint16_t)n, &got);

        /* Build JSON array — allocate from PSRAM */
        /* 60 entries × ~85 chars/entry + overhead ≈ 5200 bytes max */
        const size_t HIST_BUF = 6144;
        char *buf = (char *)ps_malloc(HIST_BUF);
        if (!buf) { free(rows); req->send(500); return; }

        int pos = 0;
        pos += snprintf(buf + pos, HIST_BUF - pos, "{\"rows\":[");
        for (uint16_t i = 0; i < got; i++) {
            int written = snprintf(buf + pos, HIST_BUF - (size_t)pos,
                "%s{\"ts\":%lu,\"temp_c\":%.1f,\"rh_pct\":%u,"
                "\"wind_ms\":%.1f,\"wind_dir\":%u}",
                i ? "," : "",
                (unsigned long)rows[i].timestamp,
                (float)rows[i].t_avg_c,
                rows[i].rh_avg_pct,
                rows[i].wind_speed_avg_ms10 / 10.0f,
                rows[i].wind_dir_avg_deg);
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
    TickType_t last_push = xTaskGetTickCount();
    char *push_buf = (char *)ps_malloc(1024);
    if (!push_buf) {
        ESP_LOGE(TAG, "ps_malloc failed for WS push buffer");
        push_buf = nullptr;
    }

    for (;;) {
        vTaskDelayUntil(&last_push, pdMS_TO_TICKS(WS_PUSH_MS));

        /* Clean up stale WebSocket clients */
        s_ws.cleanupClients();

        if (push_buf && s_ws.count() > 0) {
            build_status_json(push_buf, 1024);
            s_ws.textAll(push_buf);
        }
    }
}
