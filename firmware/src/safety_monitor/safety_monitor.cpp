/**
 * @file safety_monitor.cpp
 * @brief T3 — Safety Monitor stub (Phase 0).
 *
 * Full implementation: Phase 3 of firmwareImplementationPlan.md.
 *
 * @author  Greenhouse Controller project
 */

#include "safety_monitor.h"
#include "../types/app_types.h"

void task_safety_monitor(void *pvParameters)
{
    (void)pvParameters;
    /* TODO Phase 3: wind threshold evaluation, CMD_CLOSE_ALL to Q1,
     * EG1.WIND_OVERRIDE set/clear, LOG_ALARM to Q3. */
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}
