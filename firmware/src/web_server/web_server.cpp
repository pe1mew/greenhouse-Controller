/**
 * @file web_server.cpp
 * @brief T11 — Web Server stub (Phase 0).
 *
 * Full implementation: Phase 9 of firmwareImplementationPlan.md.
 *
 * @author  Greenhouse Controller project
 */

#include "web_server.h"
#include "../types/app_types.h"

void task_web_server(void *pvParameters)
{
    (void)pvParameters;
    /* TODO Phase 9: ESPAsyncWebServer on LittleFS; session model;
     * REST API for config/status; Q4 config post; no Q1 (C9). */
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}
