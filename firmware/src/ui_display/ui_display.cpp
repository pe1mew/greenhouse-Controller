/**
 * @file ui_display.cpp
 * @brief T8 — UI / Display stub (Phase 0).
 *
 * Full implementation: Phase 4 of firmwareImplementationPlan.md.
 *
 * @author  Greenhouse Controller project
 */

#include "ui_display.h"
#include "../types/app_types.h"

void task_ui_display(void *pvParameters)
{
    (void)pvParameters;
    /* TODO Phase 4: LCD FSM, menu navigation, PIN session, Q2 receive,
     * Q4 config post, Q5 receive for network status display. */
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}
