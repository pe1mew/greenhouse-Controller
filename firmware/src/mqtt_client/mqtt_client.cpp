/**
 * @file mqtt_client.cpp
 * @brief T12 — MQTT Client stub (Phase 0).
 *
 * Full implementation: Phase 9 of firmwareImplementationPlan.md.
 *
 * @author  Greenhouse Controller project
 */

#include "mqtt_client.h"
#include "../types/app_types.h"

void task_mqtt_client(void *pvParameters)
{
    (void)pvParameters;
    /* TODO Phase 9: publish sensor/status topics; subscribe command topics;
     * post config_update_t to Q4; window commands ignored (C9). */
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}
