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
 *                                       halt) via rtc_oscillator_stopped(). */
#include "gpio_util.h"
#include "keypad_matrix.h"
#include "nvs_config.h"
#include "i2c_bus.h"
#include "lcd1602.h"
#include "modbus_rtu.h"
#include "s200.h"
#include "fg6485a.h"
#include "ds1307_rtc.h"

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

        /* alpha.2.8 — Poll the FG6485A T/RH sensor via the FG6485A driver
         * (LIB-FG). Internally fg6485a_read_measurements() does one FC03
         * to slave 1 reading 2 holding regs (same wire traffic as the
         * raw call this replaced) but returns decoded floats:
         *   meas.humidity_pct    = int16(reg[0]) / 10.0f
         *   meas.temperature_c   = int16(reg[1]) / 10.0f
         * Status is collapsed: MODBUS_OK → FG6485A_OK (=0), MODBUS_ERR_PARAM
         * → FG6485A_ERR_PARAM (=1), other → FG6485A_ERR_COMM (=2). */
        fg6485a_measurement_t fg = {};
        fg6485a_status_t fg_st = fg6485a_read_measurements(1, &fg);

        /* Poll the SenseCAP S200 wind sensor via the s200 driver. Issues
         * two FC04 reads internally (wind dir+speed at 0x0008/12 regs,
         * heating temp at 0x001C/2 regs) and returns engineering units
         * (m/s and degrees) decoded from int32×1000 register pairs. */
        s200_measurement_t wind = {};
        s200_status_t s200_st = s200_read_measurements(44, &wind);

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
                 "heartbeat %lu | free=%u largest=%u psram_free=%u uptime=%lus | hb_led=%d keys=%d | fg6485a=%d rh=%.1f temp=%.1f | s200=%d dir=%.1f wind=%.2f | rtc=%d %04u-%02u-%02u %02u:%02u:%02u",
                 (unsigned long)counter,
                 (unsigned)free_internal,
                 (unsigned)largest_block,
                 (unsigned)free_spiram,
                 (unsigned long)((xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000UL),
                 hb_state,
                 keys_pressed,
                 (int)fg_st,
                 fg.humidity_pct,
                 fg.temperature_c,
                 (int)s200_st,
                 wind.wind_dir_avg_deg,
                 wind.wind_speed_avg_ms,
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
