/**
 * @file ota_manager.cpp
 * @brief T13 — OTA Manager stub (Phase 0).
 *
 * Full implementation: Phase 10 of firmwareImplementationPlan.md.
 *
 * @author  Greenhouse Controller project
 */

#include "ota_manager.h"
#include "../types/app_types.h"

void task_ota_manager(void *pvParameters)
{
    (void)pvParameters;
    /* TODO Phase 10: dual-bank OTA update, LittleFS bank coupling,
     * 3-fail rollback, EG1.OTA_IN_PROGRESS set/clear. Self-deletes on completion. */
    vTaskDelete(NULL);
}
