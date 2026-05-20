/**
 * @file watchdog.cpp
 * @brief T1 — Watchdog / Heartbeat task (full instrumentation — 2.0.0-a.6.32).
 *
 * See watchdog.h for the high-level design contract. This file implements:
 *
 *   1. TWDT subscription + 500 ms kick (boot-time minimal, since a.6.22)
 *   2. ota_mark_healthy() at OTA_HEALTHY_MS                  (since a.6.22)
 *   3. PIN_HB_LED 1 Hz toggle                                (since a.6.24)
 *   4. NeoPixel day/night colour from EG1 priority           (a.6.32)
 *   5. 60 s LOG_SYSTEM heap rows (value_a=7,8,12)            (a.6.32)
 *   6. 30 s-offset heap-integrity check (value_a=9 on fail)  (a.6.32)
 *   7. 10-min stack-HWM serial log over T1..T15 handles      (a.6.32)
 *
 * Tick cadence (500 ms): heap rows at tick % 120 == 0; heap integrity at
 * tick % 120 == 60; stack HWM at tick % 1200 == 0. Spreads the per-tick
 * cost so the WDT-critical kick never collides with the heap walk.
 *
 * NeoPixel driver: espressif/led_strip managed component (idf_component.yml
 * in firmware/src/, declared 2.0.0-a.6.32). The single WS2812B on
 * PIN_RGB_LED is read-back-friendly (set + refresh), and brightness is
 * applied at the R/G/B component level — NOT via led_strip_set_brightness,
 * which re-scales the internal pixel buffer on every call and degrades
 * values over repeated day↔night transitions. Documented choice from the
 * 1.20.3 file header.
 *
 * Colour priority (EG1 bits, highest first):
 *   MOTOR_ALARM | WIND_OVERRIDE  → RED    (critical: equipment / safety)
 *   SENSOR_FAULT_T | _W          → AMBER  (degraded but operating)
 *   CALIBRATING                  → BLUE   (boot calibration in progress)
 *   default                      → GREEN  (OK)
 *
 * Aligned with ui_display.cpp::status_colour_for_bits() — the LCD backlight
 * and the LED never disagree about severity.
 *
 * @author  Greenhouse Controller project
 */

#include "watchdog.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <time.h>
#include <string.h>

#include "led_strip.h"                    /* a.6.32 — managed component */
#include "../ota_manager/ota_manager.h"   /* OTA_HEALTHY_MS, ota_mark_healthy */
#include "../data_manager/data_manager.h" /* dm_cfg_snapshot for brightness */
#include "../event_logger/event_logger.h" /* log_post + log_event_t */
#include "../types/app_types.h"           /* EG1 + task_t1..task_t15 */
#include "gpio_util.h"                    /* PIN_HB_LED toggle */
#include "pin_config.h"                   /* PIN_RGB_LED */

static const char *TAG = "T1";

/* ============================================================
 * NeoPixel (single WS2812B on PIN_RGB_LED)
 * ============================================================ */

#define RGB_LED_COUNT  1
#define RMT_RES_HZ     (10 * 1000 * 1000)   /* led_strip default 10 MHz */

static led_strip_handle_t s_strip = NULL;

/** Initialise the led_strip handle. Idempotent. */
static void neopixel_init(void)
{
    if (s_strip != NULL) return;

    led_strip_config_t strip_cfg = {
        .strip_gpio_num   = PIN_RGB_LED,
        .max_leds         = RGB_LED_COUNT,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,   /* WS2812B canonical wire order */
        .led_model        = LED_MODEL_WS2812,
        .flags            = { .invert_out = 0 },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = RMT_RES_HZ,
        .mem_block_symbols = 0,                  /* let driver pick default */
        .flags             = { .with_dma = 0 },  /* single LED, DMA overkill */
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[T1] led_strip_new_rmt_device failed: %s",
                 esp_err_to_name(err));
        s_strip = NULL;
        return;
    }
    /* Boot indicator — amber. Will be overwritten by the first tick once
     * EG1 + cfg are queryable, but the brief amber confirms the LED works. */
    (void)led_strip_set_pixel(s_strip, 0, 255u, 128u, 0u);
    (void)led_strip_refresh(s_strip);
    ESP_LOGI(TAG, "[T1] NeoPixel ready on GPIO%d (amber boot indicator)",
             (int)PIN_RGB_LED);
}

/** Compute the brightness scalar (0..255) for the current local hour. */
static uint8_t neopixel_dim_for_now(const cfg_shadow_t *cfg)
{
    /* Use localtime — tz is set via setenv("TZ", ...) by T10's geo sync
     * (defaults to CET if not yet geo-resolved). */
    time_t now = time(NULL);
    struct tm t_now = {};
    localtime_r(&now, &t_now);
    int hour = t_now.tm_hour;

    /* Night window: from led_nite_from inclusive to led_nite_to exclusive,
     * wrapping across midnight. e.g. from=22, to=6 → night = [22..24) ∪ [0..6). */
    const int from = (int)cfg->led_nite_from;
    const int to   = (int)cfg->led_nite_to;
    bool is_night;
    if (from <= to) {
        is_night = (hour >= from && hour < to);
    } else {
        is_night = (hour >= from || hour < to);
    }
    int32_t dim = is_night ? cfg->led_nite_brt : cfg->led_day_brt;
    if (dim < 0)   dim = 0;
    if (dim > 255) dim = 255;
    return (uint8_t)dim;
}

/** Pick R/G/B for the current EG1 priority. */
static void neopixel_colour_for_eg1(EventBits_t bits,
                                     uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (bits & (EG1_BIT_MOTOR_ALARM | EG1_BIT_WIND_OVERRIDE)) {
        *r = 255; *g =   0; *b = 0;     /* RED   — critical safety event */
    } else if (bits & (EG1_BIT_SENSOR_FAULT_T |
                       EG1_BIT_SENSOR_FAULT_W)) {
        *r = 255; *g = 128; *b = 0;     /* AMBER — sensor fault, degraded */
    } else if (bits & EG1_BIT_CALIBRATING) {
        *r =   0; *g =   0; *b = 255;   /* BLUE  — boot calibration */
    } else {
        *r =   0; *g = 200; *b = 0;     /* GREEN — OK (slightly dimmed) */
    }
}

/** Update the LED — called every tick. */
static void neopixel_tick(void)
{
    if (s_strip == NULL) return;

    cfg_shadow_t cfg = {};
    dm_cfg_snapshot(&cfg);
    uint8_t dim = neopixel_dim_for_now(&cfg);

    EventBits_t bits = (EG1 != NULL) ? xEventGroupGetBits(EG1) : 0;
    uint8_t r, g, b;
    neopixel_colour_for_eg1(bits, &r, &g, &b);

    /* Scale R/G/B at the application layer — DO NOT use
     * led_strip_set_brightness (it re-scales internal buffer per call and
     * degrades values over many day↔night transitions). */
    r = (uint8_t)(((uint16_t)r * dim) >> 8);
    g = (uint8_t)(((uint16_t)g * dim) >> 8);
    b = (uint8_t)(((uint16_t)b * dim) >> 8);

    (void)led_strip_set_pixel(s_strip, 0, r, g, b);
    (void)led_strip_refresh(s_strip);
}

/* ============================================================
 * LOG_SYSTEM heap rows + integrity check + stack-HWM sweep
 * ============================================================ */

/** Post a single LOG_SYSTEM event to Q3 (T9 drains into SD CSV). */
static void post_system_event(int16_t value_a, int16_t value_b)
{
    log_event_t ev = {};
    ev.timestamp  = (uint32_t)time(NULL);
    ev.event_type = (uint8_t)LOG_SYSTEM;
    ev.initiator  = (uint8_t)LOG_BY_SYSTEM;
    ev.value_a    = value_a;
    ev.value_b    = value_b;
    log_post(&ev);
}

/** Emit three heap-row LOG_SYSTEM events (value_a=7, 8, 12). */
static void emit_heap_rows(void)
{
    /* INTERNAL heap free → value_a=7, value_b=KB */
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int32_t kb_int = (int32_t)(free_int >> 10);
    if (kb_int > 32767) kb_int = 32767;
    post_system_event(7, (int16_t)kb_int);

    /* PSRAM heap free → value_a=8, value_b=KB */
    size_t free_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    int32_t kb_ps = (int32_t)(free_ps >> 10);
    if (kb_ps > 32767) kb_ps = 32767;
    post_system_event(8, (int16_t)kb_ps);

    /* INTERNAL largest contiguous → value_a=12, value_b=KB (gh#20).
     * Reveals heap fragmentation independent of total-free. The mbedTLS
     * gh#23 pattern is "free=130 KB but largest-block=8 KB" — the latter
     * is what causes 16 KB allocations to fail and corrupt-or-panic. */
    size_t lb_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    int32_t kb_lb = (int32_t)(lb_int >> 10);
    if (kb_lb > 32767) kb_lb = 32767;
    post_system_event(12, (int16_t)kb_lb);
}

/** Heap integrity walk — non-panicking. */
static void heap_integrity_check(void)
{
    /* panic=false: log corruption but don't abort. T15 supervisor (when
     * re-enabled) handles escalation if it persists. heap_caps_check_*
     * with panic=false aborts on assertion failures INSIDE the walk but
     * not on detected corruption — that's the IDF default behaviour. */
    if (!heap_caps_check_integrity_all(false)) {
        ESP_LOGE(TAG, "[T1] HEAP CORRUPTION DETECTED");
        post_system_event(9, 0);
    }
}

/** Sweep stack high-water marks over all known task handles. Serial only —
 *  too noisy for SD CSV. LOGW promotes any task below 1 KB free for
 *  visibility in the warning stream. */
static void stack_hwm_sweep(void)
{
    struct { const char *name; TaskHandle_t handle; } tasks[] = {
        { "T1",  task_t1  }, { "T2",  task_t2  }, { "T3",  task_t3  },
        { "T4",  task_t4  }, { "T5",  task_t5  }, { "T6",  task_t6  },
        { "T7",  task_t7  }, { "T8",  task_t8  }, { "T9",  task_t9  },
        { "T10", task_t10 }, { "T11", task_t11 }, { "T12", task_t12 },
        { "T14", task_t14 }, { "T15", task_t15 },
    };
    for (size_t i = 0; i < sizeof(tasks)/sizeof(tasks[0]); i++) {
        if (tasks[i].handle == NULL) continue;   /* T12, T13, T15 may be NULL */
        UBaseType_t hwm_words = uxTaskGetStackHighWaterMark(tasks[i].handle);
        size_t      hwm_bytes = (size_t)hwm_words * sizeof(StackType_t);
        if (hwm_bytes < 1024) {
            ESP_LOGW(TAG, "stack low: %s hwm=%u B", tasks[i].name,
                     (unsigned)hwm_bytes);
        } else {
            ESP_LOGI(TAG, "stack %-3s hwm=%u B", tasks[i].name,
                     (unsigned)hwm_bytes);
        }
    }
}

/* ============================================================
 * Task entry
 * ============================================================ */

/**
 * @brief T1 task body — subscribe TWDT, then run the 500 ms tick loop.
 *
 * Each tick (in order):
 *  1. `esp_task_wdt_reset()` — highest priority concern.
 *  2. `gpio_toggle(PIN_HB_LED)` — 1 Hz heartbeat LED.
 *  3. `neopixel_tick()` — updates the WS2812B colour from EG1 + day/night.
 *  4. Every 10 ticks (5 s): info-level uptime log.
 *  5. Every 120 ticks (60 s): three heap rows (value_a 7/8/12).
 *  6. Every 120 ticks at offset 60 (30 s after heap rows): integrity check.
 *  7. Every 1200 ticks (10 min): stack-HWM sweep over T1..T15.
 *  8. After OTA_HEALTHY_TICKS: `ota_mark_healthy()` once.
 *
 * @param pvParameters Unused; pass NULL.
 */
void task_watchdog(void *pvParameters)
{
    (void)pvParameters;

    /* Subscribe to the IDF TWDT. The default panic-on-timeout behaviour is
     * retained — if T1 ever stops kicking, the panic handler dumps a
     * coredump to the dedicated partition. */
    esp_task_wdt_add(NULL);

    ESP_LOGI(TAG, "[T1] watchdog task alive — tick=%ums, ota_healthy_at=%ums",
             (unsigned)T1_TICK_MS, (unsigned)OTA_HEALTHY_MS);

    neopixel_init();

    /* Tick counter — wraps after ~24 days at 500 ms. Wrap is benign:
     * the ota_healthy_marked boolean is the actual gate, and the % 120
     * / % 1200 modulos all wrap cleanly. */
    uint32_t tick_count = 0;
    const uint32_t OTA_HEALTHY_TICKS = OTA_HEALTHY_MS / T1_TICK_MS;
    bool ota_healthy_marked = false;

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        /* Kick the WDT first — highest priority concern. */
        esp_task_wdt_reset();

        /* PIN_HB_LED 1 Hz heartbeat (amber LED on GPIO41). 500 ms toggle. */
        gpio_toggle(PIN_HB_LED);

        /* NeoPixel — every tick (500 ms). Cheap enough; keeps the LED
         * responsive to EG1 alarm transitions within half a second. */
        neopixel_tick();

        /* Heartbeat log every 10 ticks (= 5 s). Quiet enough not to flood
         * serial; frequent enough to confirm T1 is alive. */
        if (tick_count % 10 == 0) {
            uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
            ESP_LOGI(TAG, "[T1] tick=%lu  uptime=%lus",
                     (unsigned long)tick_count, (unsigned long)uptime_s);
        }

        /* 60 s heap rows + 30 s-offset integrity check.
         *   tick % 120 ==   0  →  emit_heap_rows()
         *   tick % 120 ==  60  →  heap_integrity_check()
         * Spreads cost so the WDT-critical kick never collides with the
         * (~10 ms) heap walk. */
        if (tick_count % 120 == 0) {
            emit_heap_rows();
        } else if (tick_count % 120 == 60) {
            heap_integrity_check();
        }

        /* 10-min stack-HWM sweep over all task handles. Serial only. */
        if (tick_count > 0 && tick_count % 1200 == 0) {
            stack_hwm_sweep();
        }

        /* After OTA_HEALTHY_MS of stable uptime, reset the OTA fail
         * counter. One NVS write per boot via the local boolean. */
        if (!ota_healthy_marked && tick_count >= OTA_HEALTHY_TICKS) {
            ota_mark_healthy();
            ota_healthy_marked = true;
            ESP_LOGI(TAG, "[T1] ota_mark_healthy() called at tick=%lu "
                          "(boot survived %ums)",
                     (unsigned long)tick_count, (unsigned)OTA_HEALTHY_MS);
        }

        tick_count++;
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(T1_TICK_MS));
    }
}
