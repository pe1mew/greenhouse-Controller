/**
 * @file sensor_poll.cpp
 * @brief T5 — Sensor Poll stub (Phase 0).
 *
 * Full implementation: Phase 3 of firmwareImplementationPlan.md.
 *
 * @author  Greenhouse Controller project
 */

#include "sensor_poll.h"
#include "../types/app_types.h"

void task_sensor_poll(void *pvParameters)
{
    (void)pvParameters;
    /* TODO Phase 3: Modbus poll loop for FG6485A (T/RH) and S200 (wind),
     * sliding averages, Q6 overwrite, LOG_SENSOR to Q3 on every poll. */
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}
