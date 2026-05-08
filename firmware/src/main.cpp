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
    /* USB-CDC for interactive debug sessions (monitor_dtr=1 in platformio.ini).
     * Diagnostic output uses ESP_LOGI — no wait loop needed. */
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);   /* Non-blocking; never hang if no host connected */

    ESP_LOGI(TAG, "=== Greenhouse Controller v" FIRMWARE_VERSION " ===");
    ESP_LOGI(TAG, "Phase 0 boot");

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

    /* ---- Spawn all tasks ---- */
    /*                                          name      stack   param  prio               handle    core */
    xTaskCreatePinnedToCore(task_watchdog_heartbeat, "T1_WDT",  4096, NULL, TASK_PRIO_HIGHEST,  &task_t1,  1);
    xTaskCreatePinnedToCore(task_relay_controller,  "T2_RLY",  8192, NULL, TASK_PRIO_HIGH,     &task_t2,  1);
    xTaskCreatePinnedToCore(task_safety_monitor,    "T3_SAF",  4096, NULL, TASK_PRIO_HIGH,     &task_t3,  1);
    xTaskCreatePinnedToCore(task_data_manager,      "T4_DAT",  6144, NULL, TASK_PRIO_MED_HIGH, &task_t4,  1);
    xTaskCreatePinnedToCore(task_sensor_poll,       "T5_SEN",  4096, NULL, TASK_PRIO_MED_HIGH, &task_t5,  1);
    xTaskCreatePinnedToCore(task_climate_control,   "T6_CLI",  4096, NULL, TASK_PRIO_MEDIUM,   &task_t6,  1);
    xTaskCreatePinnedToCore(task_keypad_scan,       "T7_KPD",  4096, NULL, TASK_PRIO_MED_HIGH, &task_t7,  1);
    xTaskCreatePinnedToCore(task_ui_display,        "T8_UI",   8192, NULL, TASK_PRIO_MEDIUM,   &task_t8,  1);
    xTaskCreatePinnedToCore(task_event_logger,      "T9_LOG",  6144, NULL, TASK_PRIO_LOW,      &task_t9,  1);
    xTaskCreatePinnedToCore(task_network_manager,   "T10_NET", 8192, NULL, TASK_PRIO_LOW,      &task_t10, 0);
    xTaskCreatePinnedToCore(task_web_server,        "T11_WEB", 8192, NULL, TASK_PRIO_LOW,      &task_t11, 0);
    xTaskCreatePinnedToCore(task_mqtt_client,       "T12_MQT", 8192, NULL, TASK_PRIO_LOW,      &task_t12, 0);
    /* T13 (OTA) is spawned on demand by T11 — no permanent handle. */

    ESP_LOGI(TAG, "All tasks spawned - scheduler running");
}

/* ============================================================
 * loop() — not used; delete the Arduino loop task.
 * ============================================================ */

void loop()
{
    vTaskDelete(NULL);
}
