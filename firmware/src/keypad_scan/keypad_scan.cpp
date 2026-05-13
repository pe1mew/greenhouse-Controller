/**
 * @file keypad_scan.cpp
 * @brief T7 — Keypad Scan task (Phase 7).
 *
 * Scans the 4×4 membrane keypad at a 20 ms period using the LIB-5
 * keypad_matrix driver.  Two-scan debouncing is handled internally by
 * the driver; T7 adds key-repeat on top:
 *
 *   - New press  → post key_event_t{ key, repeated=false } to Q2
 *   - Hold > 500 ms → post key_event_t{ key, repeated=true } every 100 ms
 *
 * Q2 capacity is 16 items.  On overflow the event is dropped; first-press
 * events emit a warning, repeat events emit a debug log only (T8 should
 * drain Q2 well within 100 ms).
 *
 * Key layout (keypad_matrix driver):
 * @verbatim
 *   Row/Col  1    2    3    4
 *     1     '1'  '2'  '3'  'A'
 *     2     '4'  '5'  '6'  'B'
 *     3     '7'  '8'  '9'  'C'
 *     4     '*'  '0'  '#'  'D'
 * @endverbatim
 *
 * @author  Greenhouse Controller project
 */

#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include <Arduino.h>
#include <esp_log.h>
#include <esp_task_wdt.h>   /* WDT subscription (1.17.29 / gh#13) */

#include "keypad_scan.h"
#include "../types/app_types.h"
#include "keypad_matrix.h"

static const char *TAG = "T7_KPD";

/* ============================================================
 * Timing constants
 * ============================================================ */
#define KP_SCAN_MS          20u   /**< Scan period (ms) */
#define KP_REPEAT_HOLD_MS  500u   /**< Hold duration before first repeat */
#define KP_REPEAT_INTV_MS  100u   /**< Interval between repeat events */

/* Tick equivalents */
#define KP_HOLD_TICKS    (KP_REPEAT_HOLD_MS / KP_SCAN_MS)   /* 25 ticks */
#define KP_REPEAT_TICKS  (KP_REPEAT_INTV_MS / KP_SCAN_MS)   /*  5 ticks */

/* ============================================================
 * Task entry point
 * ============================================================ */

void task_keypad_scan(void *pvParameters)
{
    (void)pvParameters;

    /* Subscribe to WDT (1.17.29 / gh#13). 20 ms scan period — well under
     * the 5 s WDT window. */
    esp_task_wdt_add(NULL);

    ESP_LOGI(TAG, "T7 task alive");
    keypad_init();

    char    last_key     = KP_NO_KEY; /**< Key observed in previous scan tick   */
    int32_t hold_ticks   = 0;         /**< Consecutive ticks the same key is held */
    int32_t repeat_accum = 0;         /**< Ticks accumulated toward next repeat  */

    for (;;) {
        esp_task_wdt_reset();
        char key = keypad_scan();

        if (key == KP_NO_KEY) {
            /* No key — reset all repeat state */
            last_key     = KP_NO_KEY;
            hold_ticks   = 0;
            repeat_accum = 0;

        } else if (key != last_key) {
            /* Newly pressed key — post first-press event */
            last_key     = key;
            hold_ticks   = 1;
            repeat_accum = 0;

            key_event_t evt = { key, false };
            if (xQueueSend(Q2, &evt, 0) != pdTRUE) {
                ESP_LOGW(TAG, "Q2 full — first-press '%c' dropped", key);
            }

        } else {
            /* Same key still held — accumulate toward repeat */
            hold_ticks++;
            if (hold_ticks >= KP_HOLD_TICKS) {
                repeat_accum++;
                if (repeat_accum >= KP_REPEAT_TICKS) {
                    repeat_accum = 0;
                    key_event_t evt = { key, true };
                    if (xQueueSend(Q2, &evt, 0) != pdTRUE) {
                        ESP_LOGD(TAG, "Q2 full — repeat '%c' dropped", key);
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(KP_SCAN_MS));
    }
}
