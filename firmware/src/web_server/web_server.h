/**
 * @file web_server.h
 * @brief T11 — Web Server task declaration.
 *
 * Serves configuration pages from LittleFS using ESPAsyncWebServer.
 * Enforces the session model (farmer/admin roles).  Posts config_update_t
 * to Q4.  Does NOT post window commands to Q1 — manual window control
 * from the web interface is out of scope (C9).
 *
 * Full implementation: Phase 9 of firmwareImplementationPlan.md.
 *
 * @author  Greenhouse Controller project
 */

#pragma once

/**
 * @brief T11 — Web Server task entry point.
 * @param pvParameters  Unused; pass NULL.
 */
void task_web_server(void *pvParameters);
