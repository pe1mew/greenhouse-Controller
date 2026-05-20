/**
 * @file web_server.h
 * @brief T11 — Web Server task declaration.
 *
 * Serves configuration pages from LittleFS using ESP-IDF `esp_http_server`
 * (replaced ESPAsyncWebServer at alpha.6.16; old file archived as
 * `web_server_1.20.3_original.cpp.archived`). Enforces the cookie-based
 * session model (farmer / admin roles). Posts `config_update_t` to Q4.
 * Does NOT post window commands to Q1 — manual window control from the
 * web interface is out of scope (C9).
 *
 * ## Subsystem ownership
 *  - **MX5 reader**: serves static assets from the active LittleFS partition
 *    (`littlefs_active_partition()`); T13 OTA writes to the inactive bank.
 *  - **Q4 producer**: posts setpoint changes from `POST /api/config`.
 *  - **Q3 producer**: emits LOG_SETPOINT / LOG_SYSTEM audit rows via
 *    `log_post()` for web-driven config changes that bypass T4
 *    (PIN / WiFi credentials / web-tab settings).
 *  - **EG1 reader**: `OTA_IN_PROGRESS` gates the OTA endpoints' state machine.
 *
 * Full implementation: Phase 9 of firmwareImplementationPlan.md.
 *
 * @author  Greenhouse Controller project
 */

#pragma once

/**
 * @brief T11 — Web Server task entry point.
 *
 * Creates the session mutex, starts the IDF `esp_http_server` on port 80,
 * registers all URI handlers (~25 routes), spawns the WebSocket push task,
 * then idles at a 60 s tick. The httpd worker pool services requests
 * concurrently in its own tasks.
 *
 * @param pvParameters  Unused; pass NULL.
 * @note Suggested xTaskCreatePinnedToCore: stack 4096 B, prio 5, core 0.
 */
void task_web_server(void *pvParameters);
