/**
 * @file web_server_tickle.h
 * @brief Phase-5 ESP-IDF web server tickle — IDF-native esp_http_server.
 *
 * Self-contained validation module for the v2.0.0 migration's Phase 5
 * (2.0.0-alpha.5). Exercises:
 *   - httpd_handle_t = httpd_start(&config)
 *   - httpd_register_uri_handler with multiple route methods
 *   - httpd_resp_send / httpd_resp_send_chunk
 *   - URL parsing + query-string extraction (deferred to alpha.5.1+)
 *
 * Replaces (for Phase 5 onward):
 *   - `AsyncWebServer server(80);` — Arduino-only `ESPAsyncWebServer`
 *   - `server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {...});`
 *   - `req->send(200, "text/html", body);`
 *
 * Tickle scope (alpha.5):
 *   - `GET /`             → simple HTML page showing uptime, heap, version
 *   - `GET /api/status`   → JSON-ish key=value blob (one-shot snapshot)
 *   - `GET /api/info`     → firmware identity (version, MAC, chip)
 *
 * Server stays running indefinitely once started — the user can open a
 * browser at any point during the boot session to confirm it works.
 *
 * Defers to alpha.5.1+ / Phase 6:
 *   - The 25 production endpoints (login, config, status, SD, log, OTA, …)
 *     → split across web_routes_static / _auth / _config / _status / _sd /
 *       _ota / _ws files.
 *   - WebSocket (/ws) for the 2-second status push.
 *   - Session-cookie handling (Cookie: header parse + session table).
 *   - Multipart upload (firmware + assets OTA).
 *   - Cache-bust injection in static-file serving.
 *
 * @author Greenhouse Controller project
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Return status from web_server_tickle_start(). */
typedef enum {
    WEB_SERVER_TICKLE_OK              = 0, /**< Server up + URIs registered */
    WEB_SERVER_TICKLE_INIT_FAILED     = 1, /**< httpd_start returned !=ESP_OK */
    WEB_SERVER_TICKLE_REGISTER_FAILED = 2, /**< httpd_register_uri_handler failed */
} web_server_tickle_status_t;

/**
 * @brief Start the Phase-5 HTTP server tickle.
 *
 * Spins up `esp_http_server` on TCP port 80 with the tickle's route set
 * registered. Server runs in its own background task; this function
 * returns immediately after registration.
 *
 * Requires WiFi STA to be up (the Phase-3 wifi_tickle has left this state).
 *
 * Logs the listening URL to ESP_LOG so the user can copy-paste into a
 * browser without guessing the IP.
 *
 * @return WEB_SERVER_TICKLE_OK on success.
 */
web_server_tickle_status_t web_server_tickle_start(void);

#ifdef __cplusplus
}
#endif
