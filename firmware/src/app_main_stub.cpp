/**
 * @file app_main_stub.cpp
 * @brief Minimal heartbeat — Phase-1 ESP-IDF migration scaffold (v2.0.0-alpha.1).
 *
 * Purpose
 * -------
 * The single goal of this file is to prove that the `framework = espidf` flip
 * in `platformio.ini` (Phase 1 of the ESP-IDF migration, see BRANCH_NOTES.md)
 * produces a binary that boots and runs stably on real LOLIN S3 hardware.
 *
 * Acceptance bar for v2.0.0-alpha.1
 *  - Boot reason on next reset reports `ESP_RST_POWERON` (= 1).
 *  - One log line emitted every 5 s to USB-CDC serial.
 *  - Sustained for at least 1 hour without panic, INT_WDT, TASK_WDT or
 *    brownout.
 *  - Free internal heap reported by the log line stays above 100 KB
 *    (rough sanity bound; with no subsystems active we expect 200+ KB).
 *
 * What this file does NOT do (intentionally)
 *  - No WiFi association, no networking, no web server.
 *  - No I2C / SPI / UART peripherals — no sensors, no SD card, no LCD.
 *  - No NVS reads (defaults assumed; nothing to persist).
 *  - No watchdog subscription. The default IDF task watchdog is *not*
 *    configured to monitor this task; if the task hangs, the bootloader
 *    won't catch it. That's acceptable for Phase 1 — the goal is to verify
 *    the framework boots, not to exercise the supervisor architecture.
 *
 * Replaced by the full main port in Phase 6 (2.0.0-alpha.6). The original
 * arduino-era `firmware/src/main.cpp` is preserved on disk (not in the
 * build) until that phase rewrites it into a clean IDF `app_main()`.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_err.h"
#include "nvs_flash.h"

/* Phase 2 driver-layer migration — proves component-linkage works for each
 * migrated driver. Each driver gets one line of "tickle" code in the
 * heartbeat path so a build/link failure surfaces immediately rather than
 * waiting until the full firmware is reassembled in Phases 4-6.
 *
 * 2.0.0-alpha.2.1: gpio_util (LIB-1) — toggle the heartbeat LED on each tick.
 * 2.0.0-alpha.2.2: keypad_matrix (LIB-5) — log count_pressed on each tick.
 *                                          On Unit-2 hardware (membrane
 *                                          keypad wired) reports key
 *                                          presses in real time.
 * 2.0.0-alpha.2.3: nvs_config (LIB-7) — initialise NVS, read the previously-
 *                                       stored fw_version string at boot.
 *                                       On Unit 2 (last ran 1.20.3 prod) the
 *                                       first read will report "1.20.3";
 *                                       nvs_cfg_init then overwrites it
 *                                       with the current FIRMWARE_VERSION
 *                                       per schema policy. One-shot in
 *                                       app_main — no heartbeat noise. */
#include "gpio_util.h"
#include "keypad_matrix.h"
#include "nvs_config.h"

static const char *TAG = "GHC-STUB";

/* The CMake `idf_component_register` doesn't propagate -DFIRMWARE_VERSION
 * the same way PlatformIO's `build_flags` does for plain C/C++ compile
 * units, but in this project PlatformIO injects build_flags as global
 * compiler args, so FIRMWARE_VERSION should arrive as a -D from there.
 * If the macro is somehow undefined, fall back to a literal so the build
 * doesn't fail — Phase 1's job is to boot, not to gatekeep version strings. */
#ifndef FIRMWARE_VERSION
#  define FIRMWARE_VERSION "2.0.0-alpha.1-unstamped"
#endif

/**
 * @brief One-shot boot banner: prints chip info, MAC, heap, reset reason.
 */
static void log_boot_banner(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "================================================================");
    ESP_LOGI(TAG, "Greenhouse Controller v%s — ESP-IDF stub (Phase 1)", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "Chip: ESP32-S3 rev v%d.%d, %d cores, %s%s%s",
             info.revision / 100, info.revision % 100,
             info.cores,
             (info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/BGN " : "",
             (info.features & CHIP_FEATURE_BT)        ? "BT "       : "",
             (info.features & CHIP_FEATURE_BLE)       ? "BLE "      : "");
    ESP_LOGI(TAG, "Flash: %lu MB", (unsigned long)(flash_size / (1024UL * 1024UL)));
    ESP_LOGI(TAG, "STA MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Boot reason: esp_reset_reason=%d", (int)esp_reset_reason());
    ESP_LOGI(TAG, "Free heap (INTERNAL): %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "Free heap (SPIRAM):   %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "================================================================");
}

/**
 * @brief Heartbeat task — emits one log line every 5 s with current heap.
 *
 * Stack: 4 KB is generous for printf + ESP_LOGI; default IDF task stacks
 * are 3 KB which we'd want to grow when stack-canary checks are tight.
 * Priority 5 is the IDF default for application tasks.
 */
static void heartbeat_task(void *arg)
{
    (void)arg;
    uint32_t counter = 0;
    const TickType_t period = pdMS_TO_TICKS(5000);
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        size_t free_internal  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t largest_block  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        size_t free_spiram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

        /* Toggle PIN_HB_LED and read it back — software-level proof the
         * gpio driver works regardless of whether the dev unit actually
         * has an LED wired on GPIO41. Production hardware with the amber
         * HB LED on GPIO41 will also see visible blink in sync. */
        gpio_toggle(PIN_HB_LED);
        int hb_state = (gpio_read(PIN_HB_LED) == GPIO_HIGH) ? 1 : 0;

        /* Poll the keypad — exercises gpio_util on 8 pins (4 row outputs
         * + 4 col inputs with pullup). Returns 0 when nothing is pressed,
         * 1 for a single key, >1 during multi-press. The log line tracks
         * this so a user pressing keys during the test sees the count
         * change in real time on serial. */
        int keys_pressed = keypad_count_pressed();

        ESP_LOGI(TAG, "heartbeat %lu | free=%u largest=%u psram_free=%u uptime=%lus | hb_led=%d keys=%d",
                 (unsigned long)counter,
                 (unsigned)free_internal,
                 (unsigned)largest_block,
                 (unsigned)free_spiram,
                 (unsigned long)((xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000UL),
                 hb_state,
                 keys_pressed);
        counter++;

        vTaskDelayUntil(&last_wake, period);
    }
}

/**
 * @brief ESP-IDF application entry point.
 *
 * Called by the IDF startup code on Core 0 after FreeRTOS is up. Returns
 * are not allowed — must keep running or spawn workers and stay alive.
 *
 * Spawns the heartbeat task and then deletes itself: the spawned task
 * takes over from here. Matches the spawn-and-exit pattern the eventual
 * full main.cpp will use.
 */
extern "C" void app_main(void)
{
    log_boot_banner();

    /* Phase-2 driver linkage tickles. Each migrated driver gets one call
     * here to prove the component-linkage works against real hardware.
     *
     * alpha.2.1 — gpio_util (LIB-1): configure PIN_HB_LED as output, drive
     * LOW. Heartbeat task toggles it later; visible blink on Unit 2's
     * amber HB LED (GPIO41).
     *
     * alpha.2.2 — keypad_matrix (LIB-5): initialise row/col pins. Heartbeat
     * task calls keypad_count_pressed() each tick and logs the count.
     * Unit 2's membrane keypad reports 0 idle, >0 while keys held.
     * This also indirectly exercises gpio_util on 8 different pins
     * (KP_ROW1..4 outputs + KP_COL1..4 inputs with pullup) which is
     * meaningfully more coverage than the single-pin gpio test. */
    gpio_set_pin_mode(PIN_HB_LED, GPIO_OUTPUT);
    gpio_write(PIN_HB_LED, GPIO_LOW);
    keypad_init();

    /* alpha.2.3 — NVS driver tickle. Read system/fw_version BEFORE
     * nvs_cfg_init() so we capture the value left by the previous
     * firmware (1.20.3 on Unit 2 → "1.20.3"). Then init normally;
     * the init writes the current FIRMWARE_VERSION over the top per
     * the schema-versioning policy documented in nvs_config.h. */
    {
        char prev_fw[32] = {0};
        /* Direct nvs_flash_init for this pre-init read — nvs_open requires
         * the flash subsystem to be up. nvs_cfg_init() will be called
         * immediately after; it tolerates already-initialised state. */
        esp_err_t pre = nvs_flash_init();
        if (pre == ESP_ERR_NVS_NO_FREE_PAGES || pre == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            pre = nvs_flash_init();
        }
        if (pre == ESP_OK) {
            nvs_cfg_status_t st = nvs_cfg_get_str(NVS_NS_SYSTEM, NVS_KEY_FW_VERSION,
                                                   prev_fw, sizeof(prev_fw));
            ESP_LOGI(TAG, "NVS pre-init: previous fw_version = \"%s\" (status=%d)",
                     prev_fw, (int)st);
        } else {
            ESP_LOGW(TAG, "NVS flash init failed: %s", esp_err_to_name(pre));
        }
    }

    nvs_cfg_status_t init_st = nvs_cfg_init();
    ESP_LOGI(TAG, "nvs_cfg_init() returned %d (%s)", (int)init_st,
             (init_st == NVS_CFG_OK)             ? "OK" :
             (init_st == NVS_CFG_ERR_MIGRATION)  ? "MIGRATION" :
             (init_st == NVS_CFG_ERR_INIT)       ? "INIT" :
                                                    "OTHER");

    /* Confirm the post-init write took effect — should match FIRMWARE_VERSION now. */
    {
        char now_fw[32] = {0};
        nvs_cfg_status_t st = nvs_cfg_get_str(NVS_NS_SYSTEM, NVS_KEY_FW_VERSION,
                                               now_fw, sizeof(now_fw));
        ESP_LOGI(TAG, "NVS post-init: fw_version is now \"%s\" (status=%d)",
                 now_fw, (int)st);
    }

    BaseType_t rc = xTaskCreatePinnedToCore(
        heartbeat_task,
        "heartbeat",
        4096,                  /* stack words */
        NULL,                  /* arg */
        5,                     /* priority */
        NULL,                  /* handle */
        tskNO_AFFINITY         /* core */
    );

    if (rc != pdPASS) {
        ESP_LOGE(TAG, "FATAL: failed to spawn heartbeat task (rc=%d). System will idle.",
                 (int)rc);
        /* Don't return — the IDF startup expects app_main to either loop
         * or have spawned at least one other task. Idle here on failure
         * so a watchdog (if eventually configured) catches it. */
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    ESP_LOGI(TAG, "Phase-1 stub: heartbeat task spawned, app_main exiting cleanly.");
    /* app_main returns; the IDF startup-task is reaped by FreeRTOS. */
}
