/**
 * @file keypad_scan.cpp
 * @brief T7 — Keypad Scan stub (Phase 0).
 *
 * Full implementation: Phase 4 of firmwareImplementationPlan.md.
 *
 * @author  Greenhouse Controller project
 */

#include "keypad_scan.h"
#include "../types/app_types.h"

void task_keypad_scan(void *pvParameters)
{
    (void)pvParameters;
    /* TODO Phase 4: 20 ms scan period, debounce, key-repeat, Q2 post. */
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}
