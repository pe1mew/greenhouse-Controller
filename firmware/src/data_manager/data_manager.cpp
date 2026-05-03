/**
 * @file data_manager.cpp
 * @brief T4 — Data Manager stub (Phase 0).
 *
 * Full implementation: Phase 1 of firmwareImplementationPlan.md.
 *
 * @author  Greenhouse Controller project
 */

#include "data_manager.h"
#include "../types/app_types.h"

void task_data_manager(void *pvParameters)
{
    (void)pvParameters;
    /* TODO Phase 1: NVS load, MX4 config shadow, MX2/MX3 ring buffers,
     * DS1307 read, sunrise/sunset compute, TN1/TN2 task notifications. */
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}
