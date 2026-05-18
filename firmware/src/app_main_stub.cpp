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
#include <time.h>   /* time_t, time() — Phase 6.2 sunrise tickle */

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
 *                                       app_main — no heartbeat noise.
 * 2.0.0-alpha.2.4: i2c_bus (LIB-2)    — initialise the I2C bus, then scan
 *                                       the whole address space and log
 *                                       which devices respond. On Unit 2
 *                                       expect 0x3E (AiP31068L LCD) and
 *                                       0x68 (DS1307 RTC).
 * 2.0.0-alpha.2.5: lcd1602 (LIB-4)    — initialise the AiP31068L LCD,
 *                                       write a visible greeting on both
 *                                       rows. First time alpha.2.x produces
 *                                       output on the LCD itself rather
 *                                       than serial only.
 * 2.0.0-alpha.2.6: modbus_rtu (LIB-6) — initialise Modbus RTU master on
 *                                       UART1/RS-485. On each heartbeat,
 *                                       read 2 holding registers from the
 *                                       FG6485A T/RH sensor (slave addr 1,
 *                                       regs 0x0000=RH 0x0001=Temp) and
 *                                       log the result. First time alpha.2.x
 *                                       exercises the full UART + RS-485
 *                                       direction-control + Modbus protocol
 *                                       stack against a real sensor.
 * 2.0.0-alpha.2.7: s200 (LIB-S200)    — call s200_read_measurements()
 *                                       against the SenseCAP S200 wind
 *                                       sensor at slave addr 44 alongside
 *                                       the FG6485A poll. Exercises the
 *                                       multi-slave Modbus pattern (IFG
 *                                       enforcement across consecutive
 *                                       transactions to different slaves),
 *                                       and decodes engineering units
 *                                       (m/s and degrees) via the s200
 *                                       driver's int32×1000 helpers.
 * 2.0.0-alpha.2.8: fg6485a (LIB-FG)   — replace the raw FC03 readout used
 *                                       since 2.6 with the FG6485A driver's
 *                                       fg6485a_read_measurements(). Same
 *                                       wire traffic, but the result is now
 *                                       decoded into engineering units
 *                                       (humidity_pct, temperature_c) by
 *                                       the driver's int16/10 helpers. The
 *                                       heartbeat log changes from raw uint16
 *                                       (rh_raw/t_raw) to floats (rh/temp) —
 *                                       this validates LIB-FG's status-mapping
 *                                       (MODBUS_OK → FG6485A_OK, other → ERR_COMM)
 *                                       and its reg-decode arithmetic on the
 *                                       same live bus as 2.7.
 * 2.0.0-alpha.2.9: ds1307_rtc (LIB-3) — initialise the DS1307 RTC at I2C
 *                                       address 0x68 (already detected by
 *                                       i2c_scan since alpha.2.4) and read
 *                                       the wall-clock time on each heartbeat.
 *                                       Exercises i2c_write_read for the
 *                                       BCD-encoded time registers (0x00–0x06)
 *                                       and the LIB-3 BCD-decode + range-
 *                                       validation path. On Unit 2 the battery-
 *                                       backed RTC has been running continuously
 *                                       since the 1.20.3 deployment so the
 *                                       expected reading is real wall-clock
 *                                       time. Also reports the CH-bit (clock-
 *                                       halt) via rtc_oscillator_stopped().
 * 2.0.0-alpha.2.10: littlefs (LIB-9) — query the active LittleFS partition
 *                                       (via OTA bank readback), mount it,
 *                                       log total/free bytes, probe for
 *                                       `index.html` (the canonical bundled
 *                                       web-asset that EVERY 1.x build has
 *                                       written to whichever partition is
 *                                       active for that OTA cycle). One-shot
 *                                       tickle in app_main; the mount stays
 *                                       up for the rest of the boot so any
 *                                       future filesystem regression test
 *                                       can use it. Exercises the new
 *                                       esp_vfs_littlefs_register path and
 *                                       the POSIX fopen/stat layer that
 *                                       replaces the Arduino fs::File class.
 * 2.0.0-alpha.2.11: sd_storage (LIB-8) — initialise SPI host (FSPI/SPI2 on
 *                                       LOLIN S3, pins CLK=39 MISO=48 MOSI=47
 *                                       CS=40), mount FAT32 over SPI at
 *                                       /sdcard. If card present: log
 *                                       capacity, free bytes, append a test
 *                                       log line, list .csv files. If no
 *                                       card: log "no SD card present" and
 *                                       move on (acceptable — Phase 2.11
 *                                       does not require SD hardware to be
 *                                       fitted; some operators don't fit one).
 *                                       This is the LAST driver migrated
 *                                       in Phase 2.
 *
 * Phase 3 — Network stack (2.0.0-alpha.3):
 *   wifi_tickle (firmware/src/wifi_tickle.cpp) — IDF-native esp_wifi +
 *                                       esp_netif + esp_event STA bring-up,
 *                                       reads SSID/PSK from NVS (LIB-7),
 *                                       waits for IP_EVENT_STA_GOT_IP via
 *                                       event group, runs esp_netif_sntp_*
 *                                       for time sync. Replaces the
 *                                       arduino-era WiFi.begin/status loop
 *                                       in network_manager.cpp (full task
 *                                       port deferred to Phase 6 alongside
 *                                       main.cpp; this tickle proves the
 *                                       IDF WiFi stack works end-to-end
 *                                       on Unit 2 hardware and structurally
 *                                       precludes gh#21 lwIP-init race). */
#include "gpio_util.h"
#include "keypad_matrix.h"
#include "nvs_config.h"
#include "i2c_bus.h"
#include "lcd1602.h"
#include "modbus_rtu.h"
#include "s200.h"
#include "fg6485a.h"
#include "ds1307_rtc.h"
#include "littlefs_storage.h"
#include "sd_storage.h"
#include "wifi_tickle.h"
#include "https_tickle.h"
#include "web_server_tickle.h"
#include "system_globals.h"
#include "data_manager/sunrise.h"
#include "system_id/system_id.h"
#include "keypad_scan/keypad_scan.h"
#include "event_logger/event_logger.h"
#include "data_manager/data_manager.h"
#include "sensor_poll/sensor_poll.h"
#include "relay_controller/relay_controller.h"
#include "types/app_types.h"  /* Q1..Q6, MX1..MX5, EG1, task_t1..15, key_event_t etc. */

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
    /* alpha.6.3 — surface the per-unit ID derived from MAC bytes 4-5 via
     * system_id (gh#17). Matches the AP-SSID convention `Greenhouse-XXXX`
     * the future network_manager (Phase 6.12) will use. */
    char unit_id_str[5] = {0};
    system_unit_id_str(unit_id_str, sizeof(unit_id_str));
    ESP_LOGI(TAG, "Unit ID: %s (AP-SSID would be Greenhouse-%s)",
             unit_id_str, unit_id_str);
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

        /* alpha.6.7 — sample T4's cfg_shadow_t once per heartbeat and log
         * a few representative fields. Validates that T4 successfully
         * loaded the cfg from NVS (values should be the production
         * 1.20.3-written setpoints, NOT cfg_defaults.h's compile-time
         * defaults) and that current_unix_ts is being kept current. */
        {
            cfg_shadow_t cfg = {};
            dm_cfg_snapshot(&cfg);
            ESP_LOGI(TAG,
                     "alpha.6.7 dm: t_min_day=%d t_max_day=%d hyst_t=%d "
                     "v_max=%d unix_ts=%lu is_day=%d sunrise=%ld set=%ld",
                     (int)cfg.t_min_day, (int)cfg.t_max_day, (int)cfg.hyst_t,
                     (int)cfg.v_max, (unsigned long)cfg.current_unix_ts,
                     cfg.is_daytime ? 1 : 0,
                     (long)cfg.sunrise_mins_utc, (long)cfg.sunset_mins_utc);
        }

        /* alpha.6.6 — feed Q3 with a synthetic LOG_SYSTEM event each
         * heartbeat. T9 (event_logger) drains Q3 and persists each event
         * as one CSV line on SD. Producing one heartbeat-cadence event
         * gives the operator a steady "T9 is alive" signal on the SD
         * card: open the latest YYYYMMDDHHMMSS.csv and see it grow by
         * one line every 5 seconds.
         *
         * Event encoding:
         *   timestamp  = libc time(NULL)
         *   event_type = LOG_SYSTEM (6)
         *   initiator  = LOG_BY_SYSTEM (0)
         *   value_a    = uptime in seconds (i16; wraps at ~9 hours)
         *   value_b    = free heap in KB (i16; fits comfortably) */
        {
            uint32_t uptime_s = (uint32_t)(
                (xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000UL);
            log_event_t syn = {};
            syn.timestamp  = (uint32_t)time(NULL);
            syn.event_type = (uint8_t)LOG_SYSTEM;
            syn.initiator  = (uint8_t)LOG_BY_SYSTEM;
            syn.channel    = 0;
            syn.param_id   = 0;
            syn.value_a    = (int16_t)((uptime_s > 32767u) ? 32767 : uptime_s);
            syn.value_b    = (int16_t)(free_internal / 1024u);
            log_post(&syn);
        }

        /* alpha.6.4 — drain Q2 of any key events produced by the T7
         * keypad-scan task since the last heartbeat. The task posts
         * `key_event_t { key, repeated }` on each edge-detected press
         * (and on repeat after 500 ms hold). Q2 has capacity 8, so up
         * to 8 events between 5 s ticks are captured. We pop them all
         * non-blocking and log each one. If nothing's been pressed,
         * this is a zero-cost no-op (xQueueReceive returns pdFALSE
         * immediately on empty). */
        {
            key_event_t key_ev;
            int q2_drained = 0;
            while (xQueueReceive(Q2, &key_ev, 0) == pdTRUE) {
                ESP_LOGI(TAG, "Q2 key event #%d: '%c' repeated=%d",
                         q2_drained, key_ev.key, key_ev.repeated ? 1 : 0);
                q2_drained++;
            }
            if (q2_drained > 0) {
                ESP_LOGI(TAG, "Q2 drained %d event(s) this tick", q2_drained);
            }
        }

        /* alpha.6.8 — Removed the heartbeat's fg6485a_read_measurements()
         * and s200_read_measurements() polls. T5 (sensor_poll, activated
         * this phase) is now the sole owner of the Modbus RTU bus —
         * dual polling would cause half-duplex contention and corrupt
         * sensor responses. The canonical readings now appear in T5's
         * own log lines ("[T5_SEN] T=N°C RH=N% ws=N.N m/s wd=N° | avg ...").
         * The fg6485a/s200 fields previously embedded in the heartbeat
         * format string are dropped to match. */

        /* alpha.2.9 — Poll the DS1307 RTC via LIB-3. Reads 7 BCD registers
         * (seconds..year), decodes to decimal, validates ranges. Status:
         *   RTC_OK (=0)         = clean read
         *   RTC_ERR_COMM (=2)   = I2C error (bus contention / wiring)
         *   RTC_ERR_INVALID (=3)= range check failed (BCD nonsense in a
         *                        register — points at battery-low or
         *                        a corrupted RTC). */
        rtc_datetime_t now = {};
        rtc_status_t rtc_st = rtc_get_time(&now);

        ESP_LOGI(TAG,
                 "heartbeat %lu | free=%u largest=%u psram_free=%u uptime=%lus | hb_led=%d keys=%d | rtc=%d %04u-%02u-%02u %02u:%02u:%02u",
                 (unsigned long)counter,
                 (unsigned)free_internal,
                 (unsigned)largest_block,
                 (unsigned)free_spiram,
                 (unsigned long)((xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000UL),
                 hb_state,
                 keys_pressed,
                 (int)rtc_st,
                 (unsigned)now.year,
                 (unsigned)now.month,
                 (unsigned)now.day,
                 (unsigned)now.hour,
                 (unsigned)now.minute,
                 (unsigned)now.second);
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

    /* alpha.6.1 — Phase 6.1 FreeRTOS infrastructure bootstrap.
     *
     * Creates every queue, mutex, and event group declared as extern in
     * types/app_types.h. Task handles are initialised to NULL — the
     * Phase 6.2..6.13 alphas each spawn one task and assign its handle.
     *
     * Done FIRST in app_main (before any driver init) so that any
     * subsequent code path can xQueueSend / xSemaphoreTake without
     * worrying about creation order. Phase-2/3/4/5 tickles currently
     * don't touch these globals (they all live in firmware/src/[X]_tickle.cpp
     * which is self-contained), but Phase 6 task activations will. */
    int globals_rc = system_globals_init();
    if (globals_rc != 0) {
        ESP_LOGE(TAG, "FATAL: system_globals_init failed (rc=%d) — halting", globals_rc);
        for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

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

    /* alpha.6.4 — spawn T7 keypad-scan task. The task scans the 4×4 membrane
     * matrix every 20 ms via LIB-5, debounces, generates key-repeat events
     * on 500 ms hold, and posts key_event_t to Q2. The heartbeat task
     * (below) drains Q2 each tick and surfaces presses to the serial log,
     * giving tactile acceptance: physically press a key → see the event
     * within 5 s.
     *
     * The task subscribes to the IDF TWDT internally (esp_task_wdt_add).
     * 20 ms scan period is well within the 5 s TWDT window.
     *
     * NB: keypad_init() is called above (alpha.2.2 tickle) AND again inside
     * the task at startup. LIB-5's init is idempotent (GPIO config) so the
     * double-init is harmless. The stub's earlier call keeps LIB-5 alive
     * for the heartbeat's keypad_count_pressed() polling (a separate
     * read path from the task's event-edge detection). */
    {
        BaseType_t rc = xTaskCreatePinnedToCore(
            task_keypad_scan,
            "T7-keypad",
            3072,                  /* stack words */
            NULL,                  /* arg */
            4,                     /* priority */
            &task_t7,              /* handle written into global */
            tskNO_AFFINITY);
        if (rc != pdPASS) {
            ESP_LOGE(TAG, "alpha.6.4: xTaskCreate T7 failed (rc=%d)", (int)rc);
        } else {
            ESP_LOGI(TAG, "alpha.6.4: T7 keypad_scan task spawned (handle=%p); "
                          "press a key to see Q2 events drained in the heartbeat",
                     (void *)task_t7);
        }
    }

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

    /* alpha.2.4 — I2C bus driver tickle. Initialise the bus, scan the
     * full 7-bit address space, log responding devices.
     *
     * Expected on Unit 2 hardware (production wiring):
     *   0x27  LCD1602 backpack (PCF8574 I2C-to-parallel)
     *   0x68  DS1307 RTC
     *
     * If the scan finds these two addresses, the new ESP-IDF v5 i2c_master
     * API works end-to-end against real hardware AND the pull-up + clock
     * configuration matches what the LCD/RTC need.
     *
     * If the scan finds zero devices, either i2c_init failed (pin config
     * wrong) or no device acknowledges (wiring fault / wrong pull-up
     * config). The next driver migration (alpha.2.5, LCD1602) needs this
     * to work, so failure here gates further phase 2 progress. */
    {
        i2c_status_t i2c_st = i2c_init();
        ESP_LOGI(TAG, "i2c_init returned %d (%s)", (int)i2c_st,
                 (i2c_st == I2C_OK)         ? "OK" :
                 (i2c_st == I2C_ERR_TIMEOUT)? "TIMEOUT" :
                 (i2c_st == I2C_ERR_NACK)   ? "NACK" :
                 (i2c_st == I2C_ERR_BUS_BUSY)? "BUS_BUSY" : "?");

        uint8_t found[16] = {0};
        uint8_t n = i2c_scan(found, sizeof(found));
        ESP_LOGI(TAG, "i2c_scan: %u device(s) found", (unsigned)n);
        for (uint8_t i = 0; i < n; i++) {
            ESP_LOGI(TAG, "  device[%u] @ 0x%02X", (unsigned)i, (unsigned)found[i]);
        }
    }

    /* alpha.2.5 — LCD driver tickle. After i2c_init succeeds and we've
     * confirmed the AiP31068L responded to the bus scan at 0x3E, init
     * the LCD and write a recognisable greeting that proves end-to-end
     * I2C → LCD command sequencing works.
     *
     * Row 0: firmware identity (so an operator glancing at the screen
     *        immediately sees this is the ESP-IDF migration build, not
     *        production 1.20.3).
     * Row 1: a benign "boot OK" message + the dev unit's last MAC byte
     *        so multiple bench units can be told apart by sight.
     *
     * The greeting stays on the LCD for the rest of the alpha.2.5 boot —
     * no heartbeat updates yet (those land in later phases when more of
     * the system is online). */
    {
        lcd_status_t lcd_st = lcd_init();
        ESP_LOGI(TAG, "lcd_init returned %d (%s)", (int)lcd_st,
                 (lcd_st == LCD_OK)            ? "OK" :
                 (lcd_st == LCD_ERR_NO_DEVICE) ? "NO_DEVICE" :
                 (lcd_st == LCD_ERR_COMM)      ? "COMM" : "?");
        if (lcd_st == LCD_OK) {
            (void)lcd_clear();
            (void)lcd_print(0, 0, "ESP-IDF stub OK");
            (void)lcd_print(1, 0, "v" FIRMWARE_VERSION);
            ESP_LOGI(TAG, "lcd_print: \"ESP-IDF stub OK\" / \"v%s\" written", FIRMWARE_VERSION);
        }
    }

    /* alpha.2.6 — Modbus RTU master tickle. Initialise UART1 + RS-485
     * direction control. The heartbeat task uses the bus to poll the
     * FG6485A (slave 1, via LIB-FG since alpha.2.8) AND the S200 wind
     * sensor (slave 44, via LIB-S200 since alpha.2.7).
     *
     * Init only here; the actual polls move into heartbeat_task below. */
    modbus_init();
    ESP_LOGI(TAG, "modbus_init() done — heartbeat will poll FG6485A@1 + S200@44");

    /* alpha.2.9 — DS1307 RTC tickle. Probe the chip at 0x68 (already
     * confirmed present by the alpha.2.4 i2c_scan), report the CH bit
     * (clock-halt — if set, the oscillator stopped and the time is
     * invalid; on Unit 2 the battery-backed RTC has been running since
     * 1.20.3 deployment so CH must be 0).
     *
     * If rtc_init returns RTC_ERR_NO_DEVICE here, that contradicts the
     * i2c_scan above and means the LIB-3 init path is broken — the bus
     * is the same. RTC_ERR_COMM would mean i2c_write succeeded the scan
     * but the LIB-3 0-byte probe doesn't (which is *exactly* the contract
     * caught by the alpha.2.5 LIB-2 fix — zero-length writes route through
     * i2c_master_probe). This is a regression check for that fix. */
    {
        rtc_status_t rtc_st = rtc_init();
        ESP_LOGI(TAG, "rtc_init returned %d (%s)", (int)rtc_st,
                 (rtc_st == RTC_OK)             ? "OK" :
                 (rtc_st == RTC_ERR_NO_DEVICE)  ? "NO_DEVICE" :
                 (rtc_st == RTC_ERR_COMM)       ? "COMM" :
                 (rtc_st == RTC_ERR_INVALID)    ? "INVALID" : "?");
        bool ch_halted = rtc_oscillator_stopped();
        ESP_LOGI(TAG, "RTC clock-halt bit (CH): %d (%s)", (int)ch_halted,
                 ch_halted ? "OSCILLATOR HALTED — time is invalid"
                           : "running — time is valid");
    }

    /* alpha.2.10 / alpha.2.10.1 — LittleFS dual-partition driver tickle.
     *
     * Workflow:
     *   1) Read which OTA app bank is currently running via
     *      littlefs_active_partition() (esp_ota_get_running_partition under
     *      the hood). On Unit 2 we expect LFS_PARTITION_A because Unit 2 is
     *      running the freshly-flashed binary at app0; OTA bank N ↔ LittleFS
     *      partition N by design.
     *   2) Try to mount the active partition.
     *   3) FALLBACK: if mount fails (typical fresh-flash scenario — the
     *      lfs0 partition has uninitialised or arduino-era content that the
     *      IDF joltwallet/littlefs implementation rejects with LFS_ERR_CORRUPT
     *      = -84), format the partition and re-mount. This exercises the
     *      littlefs_format() code path that we'd otherwise never hit before
     *      Phase 5; it's also a realistic production-recovery scenario
     *      (factory-erase, paired-OTA pre-write).
     *   4) Write a small known-content file, read it back, byte-compare to
     *      prove fopen("wb")+fwrite()+fclose() and fopen("rb")+fread()+fclose()
     *      via the VFS mountpoint all work end-to-end.
     *   5) Report total + used + free bytes via esp_littlefs_info.
     *
     * Acceptance signal:
     *   - One of:
     *       littlefs_mount returns 0 (OK) — partition was already initialised
     *       littlefs_mount returns 1 (MOUNT), then format+remount → 0 (OK)
     *   - test-file write/read/verify all succeed
     *   - free_bytes is positive and plausible (~960 KB on a 1 MB partition
     *     with one tiny test file)
     *
     * The mount stays up for the rest of the boot so any future filesystem
     * regression test can use it. */
    {
        lfs_partition_t active = littlefs_active_partition();
        const char *active_str = (active == LFS_PARTITION_A) ? "A (lfs0)" :
                                 (active == LFS_PARTITION_B) ? "B (lfs1)" : "?";
        ESP_LOGI(TAG, "littlefs_active_partition = %s", active_str);

        lfs_status_t lfs_st = littlefs_mount(active);
        ESP_LOGI(TAG, "littlefs_mount(%s) returned %d (%s)",
                 active_str, (int)lfs_st,
                 (lfs_st == LFS_OK)            ? "OK" :
                 (lfs_st == LFS_ERR_MOUNT)     ? "MOUNT" :
                 (lfs_st == LFS_ERR_NOT_FOUND) ? "NOT_FOUND" :
                 (lfs_st == LFS_ERR_IO)        ? "IO" :
                 (lfs_st == LFS_ERR_FULL)      ? "FULL" : "?");

        /* alpha.2.10.1 fallback: if the partition didn't mount, format it
         * and try again. The partition table reserves 1 MB at offset
         * 0x420000; a fresh format gives us a clean LittleFS filesystem. */
        if (lfs_st == LFS_ERR_MOUNT) {
            ESP_LOGW(TAG, "LFS mount failed — partition is uninitialised or "
                          "carries arduino-era content; formatting now...");
            lfs_status_t fmt_st = littlefs_format(active);
            ESP_LOGI(TAG, "littlefs_format(%s) returned %d (%s)",
                     active_str, (int)fmt_st,
                     (fmt_st == LFS_OK) ? "OK" : "ERR");

            if (fmt_st == LFS_OK) {
                lfs_st = littlefs_mount(active);
                ESP_LOGI(TAG, "littlefs_mount(%s) after format returned %d (%s)",
                         active_str, (int)lfs_st,
                         (lfs_st == LFS_OK) ? "OK" : "FAIL");
            }
        }

        if (lfs_st == LFS_OK) {
            /* Write/read/verify cycle. The file path is intentionally distinct
             * from any production asset name so an operator looking at the
             * partition later can tell which build the file was written by. */
            static const char *test_path = "/phase_2_10_test.txt";
            static const char  test_data[] =
                "Greenhouse Controller v2.0.0-alpha.2.10.1 — LIB-9 ESP-IDF port works.\n";
            const size_t test_data_len = sizeof(test_data) - 1u;

            lfs_status_t w_st = littlefs_write(active, test_path,
                                               test_data, test_data_len);
            ESP_LOGI(TAG, "littlefs_write(%s) %u bytes -> %d (%s)",
                     test_path, (unsigned)test_data_len, (int)w_st,
                     (w_st == LFS_OK)        ? "OK" :
                     (w_st == LFS_ERR_FULL)  ? "FULL" :
                     (w_st == LFS_ERR_IO)    ? "IO"   :
                     (w_st == LFS_ERR_MOUNT) ? "MOUNT": "?");

            if (w_st == LFS_OK) {
                bool exists = littlefs_exists(active, test_path);
                ESP_LOGI(TAG, "littlefs_exists(%s) = %s",
                         test_path, exists ? "true" : "false");

                char readbuf[128] = {0};
                lfs_status_t r_st = littlefs_read(active, test_path,
                                                   readbuf, sizeof(readbuf));
                ESP_LOGI(TAG, "littlefs_read(%s) -> %d (%s); %u bytes; first 40 chars: \"%.40s\"",
                         test_path, (int)r_st,
                         (r_st == LFS_OK) ? "OK" : "FAIL",
                         (unsigned)strlen(readbuf), readbuf);

                /* Byte-compare what we wrote vs what we read back. */
                bool match = (r_st == LFS_OK)
                          && (strlen(readbuf) == test_data_len)
                          && (memcmp(readbuf, test_data, test_data_len) == 0);
                ESP_LOGI(TAG, "LFS write/read verify: %s",
                         match ? "PASS — bytes identical"
                               : "FAIL — content mismatch (driver bug)");
            }

            uint64_t free_b = littlefs_free_bytes(active);
            ESP_LOGI(TAG, "LFS free bytes on partition %s: %llu",
                     active_str, (unsigned long long)free_b);

            bool have_index = littlefs_exists(active, "/index.html");
            ESP_LOGI(TAG, "LFS file probe: /index.html %s",
                     have_index ? "exists (likely arduino-era survivor)"
                                : "not found (clean post-format state)");
        }
    }

    /* alpha.2.11 — SD card storage driver tickle.
     *
     * The dev unit may or may not have an SD card fitted; LIB-8 has always
     * been optional. The tickle handles all three states:
     *   STORAGE_OK            — card present + mounted. Log capacity, free
     *                           bytes, write one test log line, read it back,
     *                           list .csv files, unmount cleanly.
     *   STORAGE_ERR_NO_CARD   — no card detected. Log "no SD card present"
     *                           and move on. This is NOT a failure; many
     *                           operator units skip the SD-card slot.
     *   STORAGE_ERR_MOUNT     — card present but FAT32 mount failed (card
     *                           formatted as something else, or hardware
     *                           fault). Log warning; move on.
     *
     * The tickle exercises the same code path that T9 (Event Logger) will
     * eventually use in Phase 6: mount → append → read → list → unmount.
     * gh#26 SD-flush-before-reset behaviour is preserved by the new
     * esp_vfs_fat_sdcard_unmount call which f_syncs internally before
     * releasing the disk-IO layer (same synchronous-flush contract as the
     * arduino-era SD.end() call). */
    {
        storage_status_t sd_st = storage_init();
        ESP_LOGI(TAG, "storage_init returned %d (%s)", (int)sd_st,
                 (sd_st == STORAGE_OK)            ? "OK" :
                 (sd_st == STORAGE_ERR_NO_CARD)   ? "NO_CARD" :
                 (sd_st == STORAGE_ERR_MOUNT)     ? "MOUNT" :
                 (sd_st == STORAGE_ERR_IO)        ? "IO" :
                 (sd_st == STORAGE_ERR_NOT_FOUND) ? "NOT_FOUND" :
                 (sd_st == STORAGE_ERR_FULL)      ? "FULL" :
                 (sd_st == STORAGE_ERR_PARAM)     ? "PARAM" : "?");

        if (sd_st == STORAGE_OK) {
            uint64_t total = storage_sd_total_bytes();
            uint64_t free_b = storage_sd_free_bytes();
            ESP_LOGI(TAG, "SD total = %llu bytes, free = %llu bytes",
                     (unsigned long long)total,
                     (unsigned long long)free_b);

            /* alpha.6.7.1 — Phase 2.11.1 test-file write/read/verify probe
             * removed. It used to append one line per boot to
             * "/phase_2_11_test.csv" ("boot,2026-05-17,LIB-SD ESP-IDF port
             * works (LFN OK)") to prove the LFN-enabled FATFS write path
             * worked. T9 (event_logger, activated alpha.6.6) now exercises
             * the same write path with real RTC-stamped lines into the
             * daily CSV — the probe is fully redundant and was just
             * growing developer cruft on the SD card. The leftover file
             * "/phase_2_11_test.csv" on the dev card must be deleted by
             * hand from a PC (the firmware does not unlink it). */

            /* List .csv files to exercise opendir/readdir. */
            char csv_list[256] = {0};
            storage_status_t l_st = storage_sd_list_csv(".csv", csv_list, sizeof(csv_list));
            ESP_LOGI(TAG, "storage_sd_list_csv(.csv) -> %d; result: \"%s\"",
                     (int)l_st, csv_list);

            /* Unmount cleanly — exercises the gh#26 sync-before-release path. */
            storage_sd_unmount();
            ESP_LOGI(TAG, "storage_sd_unmount() done; storage_sd_available() = %s",
                     storage_sd_available() ? "true (BUG)" : "false (OK)");
        } else if (sd_st == STORAGE_ERR_NO_CARD) {
            ESP_LOGI(TAG, "no SD card present — LIB-8 tickle skipped (acceptable)");
        }
    }

    /* alpha.6.7 — spawn T4 data_manager task.
     *
     * T4 is the central data hub: loads cfg_shadow_t from NVS at boot,
     * reads the DS1307 RTC under MX1 → sets the system clock via
     * settimeofday(), computes today's sunrise/sunset from cfg lat/lon
     * (via sunrise.cpp activated alpha.6.2), then enters a main loop
     * draining Q4 (config updates from web/UI) and Q6 (sensor readings
     * from T5). On a TN4 notification from T10 it writes the post-NTP
     * time back to the RTC.
     *
     * Stack: 8 KB — needs more than T9 because NVS reads can transiently
     * allocate ~2 KB on the stack for blob decode, and the cfg_shadow_t
     * struct itself is ~1 KB on snapshot copies.
     * Priority 5 — one higher than T9 (logging) so cfg/measurement updates
     * land before any synthetic logging derived from them.
     *
     * Dependencies satisfied:
     *   - NVS (LIB-7) — initialised earlier in app_main (alpha.2.3)
     *   - RTC (LIB-3, via MX1) — initialised in the LCD/RTC tickle (alpha.2.9)
     *   - sunrise math — sunrise.cpp linked alpha.6.2
     *   - Q4/Q6 — created in system_globals_init() alpha.6.1; producers
     *             (T11 web, T5 sensor) are still dormant so the queues
     *             stay empty until Phase 6.8+ task activations.
     *   - t2_get_window_states() — stubbed via relay_controller_stub.cpp
     *
     * This activation also FORCE-REMOVES data_manager_stub.cpp via the
     * linker (the real dm_get_unix_time in data_manager.cpp conflicts
     * with the stub). Same pattern documented in data_manager_stub.cpp's
     * removal comment. */
    {
        BaseType_t rc = xTaskCreatePinnedToCore(
            task_data_manager,
            "T4-data",
            8192,                  /* stack words */
            NULL,
            5,                     /* priority — above T9 (4) */
            &task_t4,
            tskNO_AFFINITY);
        if (rc != pdPASS) {
            ESP_LOGE(TAG, "alpha.6.7: xTaskCreate T4 failed (rc=%d)", (int)rc);
        } else {
            ESP_LOGI(TAG, "alpha.6.7: T4 data_manager task spawned (handle=%p); "
                          "cfg_shadow loads from NVS, RTC reads → settimeofday, "
                          "heartbeat will dm_cfg_snapshot to validate",
                     (void *)task_t4);
        }
    }

    /* alpha.6.6 — spawn T9 event_logger task.
     *
     * T9 is the sole consumer of Q3. log_post() from any task adds an
     * event; T9 drains the queue and persists each event as a CSV line
     * on SD. The task internally re-mounts the SD card (the tickle above
     * unmounted), scans for existing CSV files, resumes the most recent
     * if it has room, otherwise creates a new YYYYMMDDHHMMSS.csv file.
     *
     * Stack: 6 KB — FAT writes need more stack than the lighter T7.
     * Priority 4 — same as T7, neither realtime nor idle.
     *
     * Heartbeat below also calls log_post() with a synthetic LOG_SYSTEM
     * event each tick to feed T9 with traffic — operator can observe
     * the SD file growing as proof T9 is alive.
     *
     * data_manager dep (dm_get_unix_time) satisfied via the alpha.6.6
     * stub at firmware/src/data_manager/data_manager_stub.cpp. */
    {
        BaseType_t rc = xTaskCreatePinnedToCore(
            task_event_logger,
            "T9-evlog",
            6144,                  /* stack words */
            NULL,
            4,                     /* priority */
            &task_t9,
            tskNO_AFFINITY);
        if (rc != pdPASS) {
            ESP_LOGE(TAG, "alpha.6.6: xTaskCreate T9 failed (rc=%d)", (int)rc);
        } else {
            ESP_LOGI(TAG, "alpha.6.6: T9 event_logger task spawned (handle=%p); "
                          "heartbeat will log_post() synthetic events to Q3",
                     (void *)task_t9);
        }
    }

    /* alpha.6.8 — spawn T5 sensor_poll task.
     *
     * T5 is the sole owner of the Modbus RTU bus. It polls FG6485A (T/RH)
     * at slave 1 and S200 (wind speed/direction) at slave 44 every
     * cfg.poll_interval_s (clamped [15..120] s, NVS default 30 s), maintains
     * sliding-window averages (arithmetic for T/RH/ws, unit-vector atan2
     * for wind dir to handle the 0°/360° wrap), builds a sensor_reading_t,
     * and pushes it onto Q6 via xQueueOverwrite (depth 1, latest-wins).
     *
     * The heartbeat used to do its own naive fg6485a_read_measurements +
     * s200_read_measurements polls every 5 s. Those have been removed in
     * this alpha — running two pollers on a half-duplex RS-485 bus would
     * scramble responses. T5 owns the bus now; the canonical readings
     * surface in T5's own log lines ("[T5_SEN] T=N°C RH=N% ws=N.N m/s …").
     *
     * Boot grace: T5's task body waits 8 s before its first modbus_init
     * call, then sleeps poll_interval_s (30 s default) before its first
     * poll. Total ~38 s of bus quiet after spawn — plenty of headroom for
     * everything else to settle.
     *
     * Modbus driver init: modbus_init() was already called earlier in
     * app_main (Phase 2.6 tickle). T5 calls it again on entry — the
     * driver is idempotent (uart_driver_delete-if-installed then reinstall),
     * so double-init is safe. We keep both call sites: app_main_stub gets
     * the bus ready immediately (the heartbeat's RTC reads still run), and
     * T5's own modbus_init reconfirms the driver state at task entry.
     *
     * Stack: 8 KB — Modbus retry path, two driver call chains, and the
     * 360-deep sliding-average buffers (BSS already, but local working
     * vars during dir_avg_variation's insertion sort fit comfortably).
     * Priority 5 — same as T4 (the consumer). Q6 is depth 1 xQueueOverwrite
     * so producer/consumer relative priority doesn't matter for liveness;
     * matching T4 keeps the wake-up locality simple. Core pinning:
     * tskNO_AFFINITY (1.20.3 pinned to core 1; for now we let the scheduler
     * pick — Phase 7 soak will reveal if we need to pin it).
     *
     * Q6 consumer: T4 (data_manager). T4 must be running before T5
     * starts producing, otherwise the first xQueueOverwrite has no
     * receiver. T4 was activated alpha.6.7 and is spawned earlier in
     * this app_main, so the ordering holds.
     *
     * Dependencies satisfied:
     *   - Modbus RTU (LIB-MB / drivers/modBus) — Phase 2.6
     *   - FG6485A driver (LIB-FG / drivers/FG6485A) — Phase 2.8
     *   - S200 driver (LIB-S200 / drivers/s200) — Phase 2.7
     *   - EG1 (sensor fault bits 2-3) — declared in app_types.h
     *   - Q6 (sensor_reading_t carrier) — created system_globals_init alpha.6.1
     *   - dm_get_unix_time, dm_get_poll_interval_s, dm_cfg_snapshot — T4 alpha.6.7
     *   - log_post (LOG_ALARM fault events) — T9 alpha.6.6 */
    {
        BaseType_t rc = xTaskCreatePinnedToCore(
            task_sensor_poll,
            "T5-sensor",
            8192,                  /* stack words */
            NULL,
            5,                     /* priority — matches T4 (producer/consumer pair) */
            &task_t5,
            tskNO_AFFINITY);
        if (rc != pdPASS) {
            ESP_LOGE(TAG, "alpha.6.8: xTaskCreate T5 failed (rc=%d)", (int)rc);
        } else {
            ESP_LOGI(TAG, "alpha.6.8: T5 sensor_poll task spawned (handle=%p); "
                          "first poll in ~38 s (8 s boot grace + poll_interval_s)",
                     (void *)task_t5);
        }
    }

    /* alpha.6.9 — relay GPIO pin configuration + spawn T2 relay_controller.
     *
     * Pin map (firmware/config/pin_config.h):
     *   PIN_RELAY_M1_OPEN=12   PIN_RELAY_M1_CLOSE=13
     *   PIN_RELAY_M2_OPEN=14   PIN_RELAY_M2_CLOSE=15
     *   PIN_RELAY_M3_OPEN=16   PIN_RELAY_M3_CLOSE=21
     *   PIN_OPTO_INPUT=42 (RRK-3 motor alarm, active LOW through opto-coupler)
     *
     * Configure all 6 relay outputs as push-pull OUTPUTs and drive them LOW
     * BEFORE T2 spawns. Two reasons:
     *  1. The relays must be in a known de-energised state before T2 reads
     *     the alarm pin and decides whether to run CLOSE_ALL calibration.
     *  2. T2's calib_close_all() assumes all relays start LOW (it energises
     *     CLOSE relays; reversal logic depends on a known starting state).
     *
     * GPIO42 (PIN_OPTO_INPUT) is configured as INPUT_PULLUP. T2's first
     * task action is to read this pin — if it's LOW the alarm is already
     * asserted at boot and calibration is skipped (handle_alarm_onset).
     * The pull-up is essential: the RRK-3 contact is normally OPEN (alarm
     * cleared), and without the pull-up the pin would float.
     *
     * T2 then attaches an ISR on PIN_OPTO_INPUT via gpio_install_isr_service
     * + gpio_isr_handler_add (ESP-IDF native, alpha.6.9). Pin configuration
     * has to happen BEFORE the ISR install (otherwise the ISR sees a
     * default pin mode and may not behave consistently).
     *
     * **Physical safety note (alpha.6.9 acceptance)**: if NVS has saved any
     * channel state OTHER than CLOSED, T2 will energise the 3 CLOSE relays
     * simultaneously for up to ~3 minutes (M3 travel = 171 s). If the dev
     * unit has motors physically attached, they will close. The gh#18
     * Phase 3 NVS-skip optimization avoids this entirely when all three
     * channels were saved CLOSED on the previous clean reboot — which is
     * usually true if the unit was running production firmware.
     */
    {
        static const uint8_t RELAY_PINS_OUT[6] = {
            PIN_RELAY_M1_OPEN, PIN_RELAY_M1_CLOSE,
            PIN_RELAY_M2_OPEN, PIN_RELAY_M2_CLOSE,
            PIN_RELAY_M3_OPEN, PIN_RELAY_M3_CLOSE,
        };
        for (size_t i = 0; i < sizeof(RELAY_PINS_OUT) / sizeof(RELAY_PINS_OUT[0]); i++) {
            gpio_set_pin_mode(RELAY_PINS_OUT[i], GPIO_OUTPUT);
            gpio_write(RELAY_PINS_OUT[i], GPIO_LOW);
        }
        gpio_set_pin_mode(PIN_OPTO_INPUT, GPIO_INPUT_PULLUP);
        ESP_LOGI(TAG, "alpha.6.9: 6 relay output pins configured + driven LOW; "
                      "PIN_OPTO_INPUT=42 configured INPUT_PULLUP");

        /* T2 stack: 8 KB — calib_close_all polls every 400 ms for up to
         * 171 s with on-stack arrays + log lines; 8 KB matches 1.20.3 prod.
         * Priority 6 — one higher than T4/T5 (5) so T2 preempts data and
         * sensor tasks when commanded. In 1.20.3 prod this was
         * TASK_PRIO_HIGH; using 6 here matches that intent without
         * pulling in the priority-enum header.
         * Core pinning: tskNO_AFFINITY (1.20.3 prod pinned to core 1;
         * defer pinning until Phase 7 soak says we need it). */
        BaseType_t rc = xTaskCreatePinnedToCore(
            task_relay_controller,
            "T2-relay",
            8192,                  /* stack words */
            NULL,
            6,                     /* priority — above T4/T5 (5) */
            &task_t2,
            tskNO_AFFINITY);
        if (rc != pdPASS) {
            ESP_LOGE(TAG, "alpha.6.9: xTaskCreate T2 failed (rc=%d)", (int)rc);
        } else {
            ESP_LOGI(TAG, "alpha.6.9: T2 relay_controller task spawned (handle=%p); "
                          "boot calibration may run (up to 171 s) unless NVS "
                          "recovered all 3 channels CLOSED",
                     (void *)task_t2);
        }
    }

    /* alpha.3 — Phase 3 WiFi network stack tickle.
     *
     * Note (historical): alpha.3.1 carried a one-shot bench-credentials
     * writer here to seed NVS keys `wifi/ssid` and `wifi/psk`. That block
     * was removed in alpha.3.2 once the NVS partition was confirmed
     * populated on the dev board; the credentials persist there now
     * across reflashes (NVS lives on its own partition). For any future
     * bench unit, set credentials either via 1.20.3's web UI before
     * reflashing, via `nvs_partition_gen.py`, or by temporarily restoring
     * a similar one-shot writer block (the alpha.3.1 commit shows the
     * pattern).
     *
     * IDF-native STA bring-up: esp_netif_init → esp_event_loop_create_default
     * → esp_netif_create_default_wifi_sta → esp_wifi_init → register handlers
     * for WIFI_EVENT + IP_EVENT → set STA config from NVS creds → esp_wifi_start.
     * The WIFI_EVENT_STA_START handler calls esp_wifi_connect; the
     * IP_EVENT_STA_GOT_IP handler sets a bit on a FreeRTOS event group; the
     * tickle blocks on xEventGroupWaitBits with a 10 s budget.
     *
     * Three possible outcomes:
     *   WIFI_TICKLE_OK              — connected, got IP, SNTP synced.
     *   WIFI_TICKLE_OK_NO_NTP       — connected, got IP, SNTP timed out
     *                                 (this can happen if the bench AP has
     *                                 no internet route to pool.ntp.org).
     *   WIFI_TICKLE_NO_SSID         — NVS wifi/ssid empty (factory state
     *                                 or operator-cleared) — graceful skip.
     *   WIFI_TICKLE_CONNECT_TIMEOUT — STA_GOT_IP not received in budget;
     *                                 most likely cause is the WiFi
     *                                 network not being in range of Unit 2.
     *   WIFI_TICKLE_DISCONNECTED    — auth failure or AP not found after
     *                                 retries.
     *
     * The Unit 2 NVS holds whatever ssid/psk the 1.20.3 firmware persisted
     * there (lossless across reflashes). If the bench unit is physically
     * located within reach of that network, we should connect cleanly. If
     * not — the tickle reports CONNECT_TIMEOUT and we move on. That's
     * EXPECTED behaviour and not a regression in the WiFi migration.
     *
     * Per the migration plan: STA_GOT_IP should arrive in < 5 s. The 10 s
     * budget here gives 2× headroom for slow APs / weak signal. */
    bool wifi_up = false;
    {
        wifi_tickle_status_t wifi_st = wifi_tickle_run(/*connect_timeout_ms=*/10000u);
        const char *wifi_msg =
            (wifi_st == WIFI_TICKLE_OK)              ? "OK (connected + SNTP synced)" :
            (wifi_st == WIFI_TICKLE_OK_NO_NTP)       ? "OK (connected, NTP timed out)" :
            (wifi_st == WIFI_TICKLE_NO_SSID)         ? "NO_SSID (NVS wifi/ssid empty)" :
            (wifi_st == WIFI_TICKLE_INIT_FAILED)     ? "INIT_FAILED" :
            (wifi_st == WIFI_TICKLE_CONNECT_TIMEOUT) ? "CONNECT_TIMEOUT (AP out of range?)" :
            (wifi_st == WIFI_TICKLE_DISCONNECTED)    ? "DISCONNECTED (auth fail / AP missing)" :
                                                       "?";
        ESP_LOGI(TAG, "wifi_tickle_run() returned %d (%s)", (int)wifi_st, wifi_msg);
        wifi_up = (wifi_st == WIFI_TICKLE_OK || wifi_st == WIFI_TICKLE_OK_NO_NTP);
    }

    /* alpha.6.2 — Phase 6.2 first firmware/src/ subsystem activation: sunrise.cpp.
     *
     * sunrise.cpp implements the NOAA General Solar Position Equations
     * (±2 min accuracy for latitudes between 60°S and 60°N). Pure math —
     * no FreeRTOS, no drivers, no Arduino dependencies. The lowest-risk
     * first activation of a firmware/src/ file: this validates that
     * adding a file to the build pipeline works end-to-end before we
     * tackle the heavier task .cpp activations in Phase 6.3+.
     *
     * Tickle inputs:
     *   - lat/lon: Amsterdam-area Dutch coordinates (52.37°N, 4.90°E),
     *     close enough to Unit 2's actual location for a representative
     *     sunrise/sunset computation. Phase-6.3 (data_manager) will read
     *     the real lat/lon from NVS.
     *   - unix_ts: time(NULL) after SNTP. If SNTP failed, the date will
     *     be wrong but the math still works — the tickle just reports
     *     sunrise for whatever date the libc time-keeper believes.
     *
     * Result interpretation:
     *   - rise_min / set_min are UTC minutes from midnight (0..1439).
     *   - To convert to UTC HH:MM: hour = m / 60; minute = m % 60.
     *   - In CEST (UTC+2 in May), sunrise in Amsterdam is ~03:55 UTC
     *     = 05:55 local; sunset is ~19:30 UTC = 21:30 local. */
    if (wifi_up) {
        time_t now = time(NULL);
        int32_t rise_m = 0;
        int32_t set_m  = 0;
        sunrise_result_t s_st = sunrise_calc((int32_t)now, 52.37f, 4.90f,
                                              &rise_m, &set_m);
        bool is_day = sunrise_is_daytime((int32_t)now, 52.37f, 4.90f);

        const char *s_msg =
            (s_st == SUNRISE_OK)          ? "OK" :
            (s_st == SUNRISE_POLAR_DAY)   ? "POLAR_DAY (sun never sets)" :
            (s_st == SUNRISE_POLAR_NIGHT) ? "POLAR_NIGHT (sun never rises)" :
            (s_st == SUNRISE_ERR_PARAM)   ? "ERR_PARAM" : "?";
        ESP_LOGI(TAG,
                 "alpha.6.2 sunrise tickle: lat=52.37 lon=4.90 unix=%ld -> %s; "
                 "rise=%02d:%02d UTC set=%02d:%02d UTC is_daytime=%s",
                 (long)now, s_msg,
                 (int)(rise_m / 60), (int)(rise_m % 60),
                 (int)(set_m  / 60), (int)(set_m  % 60),
                 is_day ? "true" : "false");
    } else {
        ESP_LOGW(TAG, "alpha.6.2 sunrise tickle: skipped — no SNTP-synced time");
    }

    /* alpha.4 — Phase 4 HTTPS client tickle (gh#23 payoff).
     *
     * Runs 5 back-to-back HTTPS GETs against Google's captive-portal
     * detection endpoint (returns HTTP 204, fast, TLS-friendly, universally
     * available). Each call logs:
     *   - HTTP status code (expected: 204)
     *   - elapsed ms (full TLS handshake on call 1; resume on calls 2-5
     *                 IF the IDF stack keeps the session warm)
     *   - free heap + largest block before/after the call
     *
     * The gh#23 signal source is the DELTA from call to call. Under
     * arduino-esp32 HTTPClient + WiFiClientSecure, each call:
     *   - dropped free heap ~20 KB (mbedtls handshake state held until end)
     *   - dropped largest-block to 77-83 KB (fragmentation pinned)
     *   - did NOT recover before the next call
     *
     * Cumulative drop over many calls drove the planned-reboot cadence
     * (every 5.5 to 11 hours on Unit 2 under status_interval_s=240 s).
     *
     * With esp_http_client + keep_alive_enable + buffer_size=1024 the
     * expectation is:
     *   - Call 1: full handshake, ~20 KB transient drop (recoverable)
     *   - Calls 2-5: near-zero drop (session reused)
     *
     * If WiFi tickle failed (no IP), skip — esp_http_client_perform would
     * block until lwIP timeout (long; bad for boot UX). */
    if (wifi_up) {
        ESP_LOGI(TAG, "alpha.4 HTTPS tickle: 5 back-to-back HTTPS GETs");
        for (int i = 0; i < 5; i++) {
            https_tickle_result_t r = {};
            https_tickle_status_t st = https_tickle_run(
                "https://www.google.com/generate_204", &r);
            const char *msg =
                (st == HTTPS_TICKLE_OK)              ? "OK" :
                (st == HTTPS_TICKLE_NO_URL)          ? "NO_URL" :
                (st == HTTPS_TICKLE_INIT_FAILED)     ? "INIT_FAILED" :
                (st == HTTPS_TICKLE_PERFORM_FAILED)  ? "PERFORM_FAILED" :
                (st == HTTPS_TICKLE_HTTP_ERROR)      ? "HTTP_ERROR" : "?";
            ESP_LOGI(TAG, "HTTPS #%d: %s status=%d elapsed=%lld ms; "
                          "heap free %u -> %u (delta %+d), largest %u -> %u (delta %+d)",
                     i + 1, msg, r.http_status_code, (long long)r.elapsed_ms,
                     (unsigned)r.free_heap_before, (unsigned)r.free_heap_after,
                     (int)((int32_t)r.free_heap_after - (int32_t)r.free_heap_before),
                     (unsigned)r.largest_block_before, (unsigned)r.largest_block_after,
                     (int)((int32_t)r.largest_block_after - (int32_t)r.largest_block_before));
            /* Short pause between calls so any deferred lwIP teardown
             * settles before the next sample. */
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        ESP_LOGI(TAG, "alpha.4 HTTPS tickle: done — see deltas above for gh#23 signal");
    } else {
        ESP_LOGW(TAG, "alpha.4 HTTPS tickle: skipped — WiFi not up");
    }

    /* alpha.5 — Phase 5 web server tickle.
     *
     * Spins up esp_http_server on port 80 with 3 lightweight handlers:
     *   GET /           — operator-facing HTML status page (auto-refresh 5s)
     *   GET /api/status — machine-readable key=value snapshot
     *   GET /api/info   — firmware identity (version, MAC, chip rev)
     *
     * Once started the server runs in its own task indefinitely — the user
     * can open a browser at any point during the boot session to confirm
     * the IDF httpd is serving correctly. The listening URL is logged
     * prominently so it's easy to copy/paste from serial.
     *
     * Skipped if WiFi is not up (esp_http_server still starts, but nobody
     * could reach it; log noise rather than functional failure). */
    if (wifi_up) {
        web_server_tickle_status_t web_st = web_server_tickle_start();
        const char *web_msg =
            (web_st == WEB_SERVER_TICKLE_OK)              ? "OK (running on port 80)" :
            (web_st == WEB_SERVER_TICKLE_INIT_FAILED)     ? "INIT_FAILED" :
            (web_st == WEB_SERVER_TICKLE_REGISTER_FAILED) ? "REGISTER_FAILED" : "?";
        ESP_LOGI(TAG, "web_server_tickle_start() returned %d (%s)", (int)web_st, web_msg);
    } else {
        ESP_LOGW(TAG, "alpha.5 web server tickle: skipped — WiFi not up");
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
