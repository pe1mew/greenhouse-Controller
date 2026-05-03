/**
 * @file relay_controller.cpp
 * @brief T2 — Relay Controller stub (Phase 0).
 *
 * Full implementation: Phase 2 of firmwareImplementationPlan.md.
 *
 * @author  Greenhouse Controller project
 */

#include "relay_controller.h"
#include "../types/app_types.h"

void task_relay_controller(void *pvParameters)
{
    (void)pvParameters;
    /* TODO Phase 2: window state machines, relay GPIO, dwell timers,
     * GPIO42 motor alarm ISR (deferred-ISR, not suppressed during MOVING). */
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}
