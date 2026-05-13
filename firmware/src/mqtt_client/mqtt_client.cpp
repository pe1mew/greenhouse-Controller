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
#include <esp_task_wdt.h>   /* WDT subscription (1.17.29 / gh#13) */

void task_mqtt_client(void *pvParameters)
{
    (void)pvParameters;

    /* Subscribe to WDT (1.17.29 / gh#13). T12 is a Phase-0 stub but
     * subscribing now means the slot is exercised. Wake every 2 s to kick
     * the WDT; replace this with the real MQTT loop in Phase 9. */
    esp_task_wdt_add(NULL);

    /* TODO Phase 9: publish sensor/status topics; subscribe command topics;
     * post config_update_t to Q4; window commands ignored (C9). */
    for (;;) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
