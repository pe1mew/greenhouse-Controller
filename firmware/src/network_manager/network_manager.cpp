/**
 * @file network_manager.cpp
 * @brief T10 — Network Manager stub (Phase 0).
 *
 * Full implementation: Phase 8 of firmwareImplementationPlan.md.
 *
 * @author  Greenhouse Controller project
 */

#include "network_manager.h"
#include "../types/app_types.h"

void task_network_manager(void *pvParameters)
{
    (void)pvParameters;
    /* TODO Phase 8: WiFi AP/client lifecycle, NTP sync, DS1307 update,
     * Q5 net_status_t overwrite post. */
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}
