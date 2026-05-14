/**
 * @file main.cpp
 * @brief Greenhouse Controller firmware entry point.
 *
 * setup():  Initialises drivers, creates all FreeRTOS primitives, spawns all tasks.
 * loop():   Deletes itself — FreeRTOS scheduler handles everything.
 *
 * T1 (Watchdog/Heartbeat) is fully implemented here.
 * All other tasks are stubs that block on portMAX_DELAY; they will be
 * filled in during subsequent implementation phases.
 *
 * @author  Greenhouse Controller project
 */

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <esp_log.h>
#include <esp_system.h>      /* esp_reset_reason() — boot diagnostic */
#include <esp_heap_caps.h>   /* heap_caps_get_free_size, integrity check (1.17.29) */
#include <time.h>
#include <string.h>
#include <Adafruit_NeoPixel.h>

static const char *TAG = "GHC";

#include "types/app_types.h"
#include "gpio_util.h"
#include "i2c_bus.h"
#include "ds1307_rtc.h"
#include "nvs_config.h"

#include "auth/pin_auth.h"
#include "relay_controller/relay_controller.h"
#include "safety_monitor/safety_monitor.h"
#include "data_manager/data_manager.h"
#include "sensor_poll/sensor_poll.h"
#include "climate_control/climate_control.h"
#include "keypad_scan/keypad_scan.h"
#include "ui_display/ui_display.h"
#include "event_logger/event_logger.h"
#include "network_manager/network_manager.h"
#include "web_server/web_server.h"
#include "mqtt_client/mqtt_client.h"
#include "ota_manager/ota_manager.h"
#include "status_post/status_post.h"
#include "status_post_supervisor/status_post_supervisor.h"
#include "system_id/system_id.h"

/* ============================================================
 * RTOS primitive definitions (declared extern in app_types.h)
 * ============================================================ */

QueueHandle_t Q1;
QueueHandle_t Q2;
QueueHandle_t Q3;
QueueHandle_t Q4;
QueueHandle_t Q5;
QueueHandle_t Q6;

TaskHandle_t task_t1;
TaskHandle_t task_t2;
TaskHandle_t task_t3;
TaskHandle_t task_t4;
TaskHandle_t task_t5;
TaskHandle_t task_t6;
TaskHandle_t task_t7;
TaskHandle_t task_t8;
TaskHandle_t task_t9;
TaskHandle_t task_t10;
TaskHandle_t task_t11;
TaskHandle_t task_t12;
/* task_t13 created on demand by T11 */
TaskHandle_t task_t14;

EventGroupHandle_t EG1;

SemaphoreHandle_t MX1;
SemaphoreHandle_t MX2;
SemaphoreHandle_t MX3;
SemaphoreHandle_t MX4;
SemaphoreHandle_t MX5;

/* ============================================================
 * Queue depths
 * ============================================================ */
#define Q1_DEPTH   8
#define Q2_DEPTH  16
#define Q3_DEPTH  32
#define Q4_DEPTH   8
#define Q5_DEPTH   1   /* overwrite queue; T10 posts latest; T8 reads */
#define Q6_DEPTH   1   /* overwrite queue; T5 posts latest; T4 reads */

/* ============================================================
 * Task priorities (FreeRTOS; 0 = lowest; keep below system tasks ~22)
 * ============================================================ */
#define TASK_PRIO_HIGHEST   8   /* T1  — watchdog must not be starved */
#define TASK_PRIO_HIGH      7   /* T2, T3 — relay control, safety */
#define TASK_PRIO_MED_HIGH  6   /* T4, T5, T7 — data, sensors, keypad */
#define TASK_PRIO_MEDIUM    5   /* T6, T8 — climate, display */
#define TASK_PRIO_LOW       3   /* T9–T13 — logging, network, OTA */

/* ============================================================
 * T1 — Watchdog / Heartbeat  (fully implemented, Phase 0)
 *
 * - Kicks the hardware WDT every T1_TICK_MS.
 * - Toggles HB LED (GPIO41) on every tick → 1 Hz blink.
 * - Updates WS2812B RGB LED (GPIO38) colour from EG1 bits (§5.12):
 *     Red   — EG1.MOTOR_ALARM
 *     Amber — EG1.SENSOR_FAULT_T | EG1.SENSOR_FAULT_W | EG1.WIND_OVERRIDE
 *     Green — normal operation
 * - Applies day/night brightness dimming; Phase 0 uses hardcoded defaults.
 *   Phase 1 will read led_day_brt, led_nite_brt, led_nite_from, led_nite_to
 *   from T4 via MX4.
 * ============================================================ */

#define T1_TICK_MS          500u
#define RGB_LED_COUNT         1

/* Day/night brightness defaults — overridden by NVS in Phase 1 */
#define LED_DAY_BRT_DEF    200
#define LED_NITE_BRT_DEF    20
#define LED_NITE_FROM_DEF   22  /* 22:00 local time */
#define LED_NITE_TO_DEF      6  /* 06:00 local time */

static void task_watchdog_heartbeat(void *pvParameters)
{
    (void)pvParameters;

    /* Constructed here (not at file scope) so malloc runs after heap init. */
    Adafruit_NeoPixel s_rgb(RGB_LED_COUNT, PIN_RGB_LED, NEO_GRB + NEO_KHZ800);

    /* Subscribe this task to the hardware WDT watchlist. */
    esp_task_wdt_add(NULL);

    s_rgb.begin();
    /* Fix brightness at 255 so setPixelColor() stores raw values unchanged.
     * We apply day/night dimming by scaling the colour components directly
     * rather than calling setBrightness() in the loop — setBrightness() is
     * designed for one-time use and re-scales the internal pixel buffer on
     * every change, which degrades values over repeated day↔night transitions. */
    s_rgb.setBrightness(255);
    s_rgb.setPixelColor(0, s_rgb.Color(255, 128, 0));  /* Amber during init */
    s_rgb.show();

    uint32_t tick_count = 0;
    /* Number of 500 ms T1 ticks before ota_mark_healthy() is called.
     * OTA_HEALTHY_MS / T1_TICK_MS = 30000 / 500 = 60 ticks. */
    const uint32_t OTA_HEALTHY_TICKS = OTA_HEALTHY_MS / T1_TICK_MS;
    bool ota_healthy_marked = false;

    for (;;) {
        /* Kick the WDT before anything else — highest priority concern. */
        esp_task_wdt_reset();

        /* Toggle heartbeat LED. */
        gpio_toggle(PIN_HB_LED);

        /* Log a heartbeat line every 10 ticks (5 s). */
        if (tick_count % 10 == 0) {
            ESP_LOGI(TAG, "T1 tick %lu  uptime %lu s",
                     (unsigned long)tick_count,
                     (unsigned long)(millis() / 1000));
        }

        /* ---------------------------------------------------------
         * Hardening instrumentation (1.17.29 / gh#13).
         * Three rhythms, all gated on tick_count modulo:
         *   - every 120 ticks (60 s)         heap-free SD-log rows
         *   - 60 ticks after each heap log   heap-integrity check
         *   - every 1200 ticks (10 min)      stack-HWM serial log
         * --------------------------------------------------------- */
        if (tick_count % 120 == 0) {
            /* INTERNAL heap free → LOG_SYSTEM value_a=7, value_b=KB */
            log_event_t ev = {};
            ev.timestamp  = (uint32_t)time(NULL);
            ev.event_type = (uint8_t)LOG_SYSTEM;
            ev.initiator  = (uint8_t)LOG_BY_SYSTEM;
            ev.value_a    = (int16_t)7;
            size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            ev.value_b    = (int16_t)((free_int >> 10) > 32767u ? 32767
                                                                 : (free_int >> 10));
            log_post(&ev);

            /* PSRAM heap free → LOG_SYSTEM value_a=8, value_b=KB */
            ev.value_a    = (int16_t)8;
            size_t free_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            ev.value_b    = (int16_t)((free_ps >> 10) > 32767u ? 32767
                                                                : (free_ps >> 10));
            log_post(&ev);

            /* INTERNAL largest-contiguous block → LOG_SYSTEM value_a=12, value_b=KB
             * (gh#20, since 1.18.2). Phase 4's supervisor measures cumulative
             * free-heap drop, but mbedTLS handshake churn can fragment the
             * heap while free-total stays flat. The classic signature is
             * "free=130 KB but largest-block=8 KB", which makes the next
             * 16 KB mbedTLS allocation fail and corrupt-or-panic the chip
             * inside the library before any breaker can react.
             * Cross-reference: arduino-esp32 #7884 / #4523. */
            ev.value_a    = (int16_t)12;
            size_t lb_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            ev.value_b    = (int16_t)((lb_int >> 10) > 32767u ? 32767
                                                              : (lb_int >> 10));
            log_post(&ev);
        }

        if (tick_count % 120 == 60) {
            /* Heap integrity check — offset 30 s from the heap-row tick
             * to spread the cost. heap_caps_check_integrity_all walks every
             * heap block; takes ~10 ms on this part. The (true) arg means
             * abort-on-corruption is OFF — we want to keep running and log. */
            if (!heap_caps_check_integrity_all(true)) {
                ESP_LOGE(TAG, "HEAP CORRUPTION DETECTED at tick %lu",
                         (unsigned long)tick_count);
                log_event_t ev = {};
                ev.timestamp  = (uint32_t)time(NULL);
                ev.event_type = (uint8_t)LOG_SYSTEM;
                ev.initiator  = (uint8_t)LOG_BY_SYSTEM;
                ev.value_a    = (int16_t)9;
                ev.value_b    = 0;
                log_post(&ev);
            }
        }

        if (tick_count % 1200 == 0 && tick_count > 0) {
            /* Stack high-water-mark sweep over all known task handles.
             * Serial only — too noisy for SD. Below-1-KB-free is promoted
             * to LOGW for visibility in the warning stream. */
            struct {
                const char    *name;
                TaskHandle_t   handle;
            } tasks[] = {
                { "T1",  task_t1  }, { "T2",  task_t2  }, { "T3",  task_t3  },
                { "T4",  task_t4  }, { "T5",  task_t5  }, { "T6",  task_t6  },
                { "T7",  task_t7  }, { "T8",  task_t8  }, { "T9",  task_t9  },
                { "T10", task_t10 }, { "T11", task_t11 }, { "T12", task_t12 },
                { "T14", task_t14 }, { "T15", task_t15 },
            };
            for (auto &t : tasks) {
                if (t.handle == NULL) continue;
                UBaseType_t hwm_words = uxTaskGetStackHighWaterMark(t.handle);
                size_t      hwm_bytes = (size_t)hwm_words * sizeof(StackType_t);
                if (hwm_bytes < 1024) {
                    ESP_LOGW(TAG, "stack low: %s hwm=%u B", t.name,
                             (unsigned)hwm_bytes);
                } else {
                    ESP_LOGI(TAG, "stack %-3s hwm=%u B", t.name,
                             (unsigned)hwm_bytes);
                }
            }
        }
        /* End hardening instrumentation. */

        /* After OTA_HEALTHY_MS of stable uptime, reset the OTA fail counter.
         * Called only once per boot to avoid redundant NVS writes. */
        if (!ota_healthy_marked && tick_count >= OTA_HEALTHY_TICKS) {
            ota_mark_healthy();
            ota_healthy_marked = true;
        }

        tick_count++;

        /* ---------------------------------------------------------
         * Day/night brightness — scale colour components directly.
         * setBrightness() is NOT called in the loop; it is designed for
         * one-time initialisation and degrades pixel values on every
         * day↔night transition.  We scale R/G/B ourselves instead:
         *   stored_channel = (raw_channel * dim) >> 8
         * With NeoPixel fixed at brightness=255, setPixelColor() stores
         * the values unmodified, so what we pass is what the LED outputs.
         * --------------------------------------------------------- */
        struct tm t_now;
        time_t now = time(NULL);
        localtime_r(&now, &t_now);
        int hour = t_now.tm_hour;

        bool is_night = (hour >= LED_NITE_FROM_DEF || hour < LED_NITE_TO_DEF);
        uint8_t dim   = is_night ? (uint8_t)LED_NITE_BRT_DEF
                                 : (uint8_t)LED_DAY_BRT_DEF;

        /* ---------------------------------------------------------
         * Determine RGB colour from EG1 (lock-free read).
         * Priority (highest first): Red → Amber → Green.
         * Wind override is grouped with motor alarm (red) because both are
         * critical safety events — windows force-closed against equipment
         * damage. Aligned with the LCD1602RGB backlight palette in
         * ui_display.cpp::status_colour_for_bits() so the on-board LED and
         * the LCD never disagree about severity.
         * --------------------------------------------------------- */
        EventBits_t bits = xEventGroupGetBits(EG1);
        uint8_t r, g, b;
        if (bits & (EG1_BIT_MOTOR_ALARM | EG1_BIT_WIND_OVERRIDE)) {
            r = 255; g =   0; b = 0;   /* Red   — critical safety event (motor or wind) */
        } else if (bits & (EG1_BIT_SENSOR_FAULT_T |
                           EG1_BIT_SENSOR_FAULT_W)) {
            r = 255; g = 128; b = 0;   /* Amber — sensor fault (degraded but operating)  */
        } else {
            r =   0; g = 255; b = 0;   /* Green — normal operation                       */
        }

        /* Apply day/night dim factor to each channel. */
        r = (uint8_t)((r * dim) >> 8);
        g = (uint8_t)((g * dim) >> 8);
        b = (uint8_t)((b * dim) >> 8);

        s_rgb.setPixelColor(0, r, g, b);
        s_rgb.show();

        vTaskDelay(pdMS_TO_TICKS(T1_TICK_MS));
    }
}

/* ============================================================
 * setup() — Arduino entry point; runs once before scheduler.
 * ============================================================ */

void setup()
{
    /* Capture the reason for *this* boot before anything else can perturb it.
     * On the ESP32 the value persists across the early-boot stub; reading it
     * here gives us the cleanest answer. Values per esp_reset_reason_t:
     *   1 ESP_RST_POWERON      cold boot or power loss
     *   2 ESP_RST_EXT          external reset pin
     *   3 ESP_RST_SW           software esp_restart() (e.g. OTA finalise)
     *   4 ESP_RST_PANIC        unhandled exception / assertion / stack overflow
     *   5 ESP_RST_INT_WDT      interrupt watchdog (CPU starved > 300 ms)
     *   6 ESP_RST_TASK_WDT     task watchdog (T1 missed its 500 ms kick)
     *   7 ESP_RST_WDT          other RTC WDT
     *   8 ESP_RST_DEEPSLEEP    wake from deep sleep
     *   9 ESP_RST_BROWNOUT     supply voltage dropped below threshold
     *  10 ESP_RST_SDIO         reset over SDIO
     * Posted to T9 below (value_a = 5 BOOT, value_b = reason code) so every
     * fresh log file carries a verdict on the previous boot. */
    esp_reset_reason_t s_boot_reason = esp_reset_reason();

    /* USB-CDC for interactive debug sessions (monitor_dtr=1 in platformio.ini).
     * Diagnostic output uses ESP_LOGI — no wait loop needed. */
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);   /* Non-blocking; never hang if no host connected */

    ESP_LOGI(TAG, "=== Greenhouse Controller v" FIRMWARE_VERSION " ===");
    /* Unit ID — last 2 bytes of WiFi-STA MAC, same format as the AP SSID
     * (Greenhouse-XXXX). Surfaces in serial here, in the LOG_SYSTEM
     * value_a=11 row T4 emits a few seconds later, in the SD-log preamble
     * at every rotation, and in the canonical status JSON. See gh#17. */
    char unit_id_str[8] = {0};
    system_unit_id_str(unit_id_str, sizeof(unit_id_str));
    ESP_LOGI(TAG, "Phase 0 boot — id=%s  esp_reset_reason=%d",
             unit_id_str, (int)s_boot_reason);

    /* ---- LIB-1: GPIO ---- */
    gpio_set_pin_mode(PIN_HB_LED, GPIO_OUTPUT);
    gpio_write(PIN_HB_LED, GPIO_HIGH);

    gpio_rs485_init();

    /* All relay outputs de-energised; T2 takes ownership after spawn. */
    const uint8_t relay_pins[] = {
        PIN_RELAY_M1_OPEN,  PIN_RELAY_M1_CLOSE,
        PIN_RELAY_M2_OPEN,  PIN_RELAY_M2_CLOSE,
        PIN_RELAY_M3_OPEN,  PIN_RELAY_M3_CLOSE,
    };
    for (uint8_t pin : relay_pins) {
        gpio_set_pin_mode(pin, GPIO_OUTPUT);
        gpio_write(pin, GPIO_LOW);
    }

    gpio_set_pin_mode(PIN_OPTO_INPUT, GPIO_INPUT_PULLUP);
    ESP_LOGI(TAG, "GPIO OK");

    /* ---- LIB-2: I2C ---- */
    i2c_init();
    ESP_LOGI(TAG, "I2C OK");

    /* ---- LIB-3: RTC probe ---- */
    rtc_status_t rtc_stat = rtc_init();
    if (rtc_stat != RTC_OK) {
        ESP_LOGW(TAG, "RTC not found (status %d)", (int)rtc_stat);
    } else if (rtc_oscillator_stopped()) {
        ESP_LOGW(TAG, "RTC oscillator halted — time invalid until NTP sync");
    } else {
        ESP_LOGI(TAG, "RTC OK");
    }

    /* ---- LIB-7: NVS ---- */
    nvs_cfg_status_t nvs_stat = nvs_cfg_init();
    if (nvs_stat == NVS_CFG_ERR_MIGRATION) {
        ESP_LOGI(TAG, "NVS: schema migration — defaults written for new keys");
    } else if (nvs_stat != NVS_CFG_OK) {
        ESP_LOGE(TAG, "NVS init failed (status %d)", (int)nvs_stat);
    } else {
        ESP_LOGI(TAG, "NVS OK");
    }

    /* ---- OTA rollback check (must follow NVS init) ---- */
    /* Increments the boot-fail counter; triggers rollback if counter ≥ 3.
     * T1 calls ota_mark_healthy() after OTA_HEALTHY_MS to reset the counter. */
    ota_check_rollback();

    /* ---- PIN auth (must follow NVS init) ---- */
    pin_auth_result_t pin_stat = pin_auth_init();
    if (pin_stat != PIN_AUTH_OK) {
        ESP_LOGW(TAG, "pin_auth_init failed (status %d) — defaults may not be set",
                 (int)pin_stat);
    } else {
        ESP_LOGI(TAG, "PIN auth OK");
    }

    /* ---- WDT: subscribe T1 after task spawn (done inside the task). ---- */
    /* The Arduino-ESP32 framework initialises the task WDT (default 5 s).
     * T1 calls esp_task_wdt_add(NULL) on entry to subscribe itself. */
    ESP_LOGI(TAG, "WDT: T1 will subscribe at 500 ms kick interval");

    /* ---- Create RTOS queues ---- */
    Q1 = xQueueCreate(Q1_DEPTH, sizeof(window_cmd_t));
    Q2 = xQueueCreate(Q2_DEPTH, sizeof(key_event_t));
    Q3 = xQueueCreate(Q3_DEPTH, sizeof(log_event_t));
    Q4 = xQueueCreate(Q4_DEPTH, sizeof(config_update_t));
    Q5 = xQueueCreate(Q5_DEPTH, sizeof(net_status_t));
    Q6 = xQueueCreate(Q6_DEPTH, sizeof(sensor_reading_t));
    configASSERT(Q1 && Q2 && Q3 && Q4 && Q5 && Q6);

    /* ---- Create event group ---- */
    EG1 = xEventGroupCreate();
    configASSERT(EG1);

    /* ---- Create mutexes ---- */
    MX1 = xSemaphoreCreateMutex();  /* I2C bus */
    MX2 = xSemaphoreCreateMutex();  /* Current measurement data */
    MX3 = xSemaphoreCreateMutex();  /* Measurement ring buffers */
    MX4 = xSemaphoreCreateMutex();  /* Configuration (NVS shadow) */
    MX5 = xSemaphoreCreateMutex();  /* LittleFS active partition */
    configASSERT(MX1 && MX2 && MX3 && MX4 && MX5);

    ESP_LOGI(TAG, "RTOS primitives created");

    /* The boot-reason event (LOG_SYSTEM, value_a=5, value_b=esp_reset_reason)
     * used to be posted here in 1.17.27–1.17.30. Problem: at this point in
     * setup() the RTC has not yet been read, so time(NULL) returns 0 and
     * the SD-log row gets a `1970-01-01T00:00:00` timestamp. Confirmed in
     * the 2026-05-13 SD capture — four POWERON boots all showed epoch-zero.
     *
     * 1.17.31 moves the emit into task_data_manager() right after
     * read_rtc_and_seed_clock(), where current_unix_ts is valid. The
     * `Phase 0 boot — esp_reset_reason=` ESP_LOGI line above continues to
     * fire here at setup-entry for serial-monitor visibility regardless of
     * RTC state. */

    /* ---- Spawn all tasks ---- */
    /*                                          name      stack   param  prio               handle    core */
    xTaskCreatePinnedToCore(task_watchdog_heartbeat, "T1_WDT",  4096, NULL, TASK_PRIO_HIGHEST,  &task_t1,  1);
    xTaskCreatePinnedToCore(task_relay_controller,  "T2_RLY",  8192, NULL, TASK_PRIO_HIGH,     &task_t2,  1);
    xTaskCreatePinnedToCore(task_safety_monitor,    "T3_SAF",  4096, NULL, TASK_PRIO_HIGH,     &task_t3,  1);
    xTaskCreatePinnedToCore(task_data_manager,      "T4_DAT",  6144, NULL, TASK_PRIO_MED_HIGH, &task_t4,  1);
    /* T5 stack: 1.17.29's stack-HWM probe found peak-used = 3932 B of the
     * original 4096 (only 164 B / 4 % free). 1.17.30 doubles to 8192 → ~52 %
     * headroom. Cost: +4 KB RAM (we have plenty). Removes the only "stack
     * low" warning observed in the 2026-05-13 capture. */
    xTaskCreatePinnedToCore(task_sensor_poll,       "T5_SEN",  8192, NULL, TASK_PRIO_MED_HIGH, &task_t5,  1);
    xTaskCreatePinnedToCore(task_climate_control,   "T6_CLI",  4096, NULL, TASK_PRIO_MEDIUM,   &task_t6,  1);
    xTaskCreatePinnedToCore(task_keypad_scan,       "T7_KPD",  4096, NULL, TASK_PRIO_MED_HIGH, &task_t7,  1);
    xTaskCreatePinnedToCore(task_ui_display,        "T8_UI",   8192, NULL, TASK_PRIO_MEDIUM,   &task_t8,  1);
    xTaskCreatePinnedToCore(task_event_logger,      "T9_LOG",  6144, NULL, TASK_PRIO_LOW,      &task_t9,  1);
    xTaskCreatePinnedToCore(task_network_manager,   "T10_NET", 8192, NULL, TASK_PRIO_LOW,      &task_t10, 0);
    xTaskCreatePinnedToCore(task_web_server,        "T11_WEB", 8192, NULL, TASK_PRIO_LOW,      &task_t11, 0);
    xTaskCreatePinnedToCore(task_mqtt_client,       "T12_MQT", 8192, NULL, TASK_PRIO_LOW,      &task_t12, 0);
    /* T13 (OTA) is spawned on demand by T11 — no permanent handle. */
    /* T15 — Status-POST supervisor (gh#18 Phase 4, since 1.18.0).
     * Priority 4 sits between TASK_PRIO_LOW (3) and TASK_PRIO_MEDIUM (5):
     * higher than T14 so a wedged-T14 cannot starve the supervisor that's
     * trying to recover it, lower than the climate-critical tasks so it
     * never preempts T2/T3/T6. Stack 4 KB: the supervisor's only work is
     * accessor reads + occasional NVS writes; no TLS, no Modbus. Spawned
     * BEFORE T14 so task_t15 (defined in status_post_supervisor.cpp) is
     * valid by the time T14 enters its main loop. */
    xTaskCreatePinnedToCore(task_status_post_supervisor, "T15_SUP", 4096, NULL, 4,                &task_t15, 0);
    /* T14 stack 12 KB: TLS handshake (WiFiClientSecure / mbedTLS) needs
     * substantially more stack than plain HTTPClient. 6 KB causes a
     * stack-overflow panic the first time the task POSTs to an https://
     * endpoint; 12 KB matches what other Arduino-ESP32 projects use for
     * outbound HTTPS clients. */
    xTaskCreatePinnedToCore(task_status_post,       "T14_WEB", 12288, NULL, TASK_PRIO_LOW,      &task_t14, 0);

    ESP_LOGI(TAG, "All tasks spawned - scheduler running");
}

/* ============================================================
 * loop() — not used; delete the Arduino loop task.
 * ============================================================ */

void loop()
{
    vTaskDelete(NULL);
}
