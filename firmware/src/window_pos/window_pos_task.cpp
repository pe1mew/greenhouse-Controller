/**
 * @file window_pos_task.cpp
 * @brief T17 — window position task. See window_pos_task.h.
 */

#include "window_pos_task.h"

#include "../types/app_types.h"
#include "../data_manager/data_manager.h"
#include "../relay_controller/relay_controller.h"

#include <esp_log.h>
#include <esp_task_wdt.h>

static const char *TAG = "T17";

/* ------------------------------------------------------------------ tuning */

/**
 * `poll = travel_ms / POLL_DIVISOR`.
 *
 * The plan's §3.1 specified /100. **Measured on the rig, /100 is wrong**: FR-WP04
 * allows 1 % of stroke of overshoot, and overshoot = poll × speed =
 * (t/100) × (100/t) = **exactly 1.00 %, by construction, at every travel time**.
 * The rule was written to hit the requirement, so it spends the entire budget
 * and leaves nothing for poll jitter, scheduling or a Modbus retry. /150 leaves
 * a third of the budget as margin. See `integrateWindowPositionSensor.md` §3.1.
 */
#define POLL_DIVISOR        150u

/**
 * FR-WP20 reject threshold = RATE_LIMIT_MULT x nominal. See derive() for why
 * this is 3 rather than the 2 the plan specified.
 */
#define RATE_LIMIT_MULT       3u

/** Device floor for `40002` (contract §4.2). Also the fastest useful poll. */
#define DEVICE_MIN_WINDOW_MS  100u
#define DEVICE_MAX_WINDOW_MS 60000u

/** Never poll slower than this while travelling, however long the stroke. */
#define POLL_MAX_MS          5000u

/** Sleep between checks for "is anything moving?" while idle. */
#define IDLE_TICK_MS          500u

/* ------------------------------------------------------------------- state */

static portMUX_TYPE       s_mux = portMUX_INITIALIZER_UNLOCKED;
static windowpos_reading_t s_last;
static uint32_t            s_last_ms      = 0u;
static bool                s_have_reading = false;
static windowpos_derived_t s_derived;
static bool                s_have_derived = false;
static windowpos_counters_t s_cnt;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    if (v < lo) { return lo; }
    if (v > hi) { return hi; }
    return v;
}

/**
 * @brief Recompute the derived configuration from `travel_m3` and the device.
 *
 * @param cfg_travel_s  `travel_m3` in seconds, from the T4 shadow.
 * @param full_travel_x10 `40004`, read back from the device.
 * @param out           Destination.
 *
 * The device's 100 ms floor on `40002` is the binding constraint on a fast rig:
 * at ~150 mm/s that floor alone is 15 mm, which IS 1 % of a 1500 mm stroke. So
 * on the rig FR-WP04 is met exactly and cannot be beaten by polling harder —
 * the sensor, not the controller, is the limit. On production (171 s, ~8.8 mm/s)
 * the same floor is 0.88 mm, 0.06 % of stroke, and irrelevant.
 */
static void derive(uint16_t cfg_travel_s, uint16_t full_travel_x10,
                   windowpos_derived_t *out)
{
    const uint32_t travel_ms = (uint32_t)cfg_travel_s * 1000u;

    const uint32_t raw_poll = travel_ms / POLL_DIVISOR;
    const uint32_t window   = clamp_u32((raw_poll * 2u) / 3u,
                                        DEVICE_MIN_WINDOW_MS, DEVICE_MAX_WINDOW_MS);
    /* Never poll faster than the device refreshes: below `40002` we would just
     * re-read the same value and spend bus time doing it. */
    uint32_t poll = clamp_u32(raw_poll, DEVICE_MIN_WINDOW_MS, POLL_MAX_MS);
    if (poll < window) { poll = window; }

    out->travel_ms  = travel_ms;
    out->poll_ms    = poll;
    out->window_ms  = (uint16_t)window;

    /* Nominal rate in 0.1 mm/s, from the device's own travel figure so the two
     * cannot disagree. Guard the divide: travel_s is clamped >= CFG_MIN_TRAVEL_S
     * upstream, but this task must not fault if that ever changes. */
    const uint32_t denom = (cfg_travel_s == 0u) ? 1u : (uint32_t)cfg_travel_s;
    const uint32_t nominal = (uint32_t)full_travel_x10 / denom;
    out->nominal_rate_x10 = (uint16_t)clamp_u32(nominal, 1u, 65535u);
    /* RATE_LIMIT_MULT is 3, not the 2 that plan section 3.3 specified.
     *
     * Measured on the rig 2026-09-10: nominal derives from travel_m3 (13 s), but
     * travel_m3 covers switch-to-LIMIT while the position span is
     * switch-to-SWITCH (10 s) -- the leaf spends ~3.5 s in the blind overlap
     * with position clamped. So the real speed across the measured span is ~30 %
     * higher than this nominal, and 2x nominal is not 2x real.
     *
     * Observed peak was 2100 against a 2x threshold of 2306: 9 % margin. That
     * would false-trip on a slightly faster stroke and silently discard good
     * position samples. 3x restores the margin while still catching what this
     * check is for -- a rate "well beyond the actuator's known speed"
     * (contract section 4), not a slightly brisk one. */
    out->rate_limit_x10   = (uint16_t)clamp_u32(nominal * RATE_LIMIT_MULT, 2u, 65535u);
}

bool windowpos_task_snapshot(windowpos_reading_t *out, uint32_t *out_age_ms)
{
    bool have;
    portENTER_CRITICAL(&s_mux);
    have = s_have_reading;
    if (have) {
        if (out != NULL)        { *out = s_last; }
        if (out_age_ms != NULL) { *out_age_ms = now_ms() - s_last_ms; }
    }
    portEXIT_CRITICAL(&s_mux);
    return have;
}

void windowpos_task_counters(windowpos_counters_t *out)
{
    if (out == NULL) { return; }
    portENTER_CRITICAL(&s_mux);
    *out = s_cnt;
    portEXIT_CRITICAL(&s_mux);
}

bool windowpos_task_derived(windowpos_derived_t *out)
{
    bool have;
    portENTER_CRITICAL(&s_mux);
    have = s_have_derived;
    if (have && out != NULL) { *out = s_derived; }
    portEXIT_CRITICAL(&s_mux);
    return have;
}

/** @brief True while any channel is mid-stroke. */
static bool any_channel_travelling(void)
{
    window_state_t st[3];
    t2_get_window_states(st);
    for (int i = 0; i < 3; i++) {
        if (st[i] == WIN_MOVING_OPEN || st[i] == WIN_MOVING_CLOSE) { return true; }
    }
    return false;
}

void task_window_pos(void *pvParameters)
{
    (void)pvParameters;
    esp_task_wdt_add(NULL);

    ESP_LOGI(TAG, "T17 starting (window position, addr %u)", (unsigned)WINDOWPOS_DEFAULT_ADDR);

    /* ---- identify before trusting anything (contract §9) ---------------- */
    uint8_t build = 0u, ver = 0u;
    if (windowpos_read_ident(WINDOWPOS_DEFAULT_ADDR, &build, &ver) != WINDOWPOS_OK) {
        ESP_LOGW(TAG, "no sensor at addr %u -- T17 idle, position unavailable",
                 (unsigned)WINDOWPOS_DEFAULT_ADDR);
    } else if (build == WINDOWPOS_BUILD_BENCH) {
        /* Contract §9: this build carries a deliberate hang hook. Refusing is
         * the specified behaviour, not caution. */
        ESP_LOGE(TAG, "sensor reports BENCH build 0x%02X -- REFUSING to use it", (unsigned)build);
    } else {
        ESP_LOGI(TAG, "sensor build 0x%02X fw v%u", (unsigned)build, (unsigned)ver);
    }

    uint16_t pushed_window_ms = 0u;
    bool     was_travelling   = false;

    for (;;) {
        esp_task_wdt_reset();

        if (!any_channel_travelling()) {
            was_travelling = false;
            vTaskDelay(pdMS_TO_TICKS(IDLE_TICK_MS));
            continue;
        }

        /* ---- stroke start: derive, log, push 40002 ---------------------- */
        if (!was_travelling) {
            was_travelling = true;
            portENTER_CRITICAL(&s_mux);
            s_cnt.strokes++;
            portEXIT_CRITICAL(&s_mux);

            cfg_shadow_t cfg;
            dm_cfg_snapshot(&cfg);

            windowpos_config_t dev;
            uint16_t full_travel_x10 = 0u;
            if (windowpos_read_config(WINDOWPOS_DEFAULT_ADDR, &dev) == WINDOWPOS_OK) {
                full_travel_x10 = dev.full_travel_x10;
            }

            windowpos_derived_t d;
            derive((uint16_t)cfg.travel_s[2], full_travel_x10, &d);

            portENTER_CRITICAL(&s_mux);
            s_derived      = d;
            s_have_derived = true;
            portEXIT_CRITICAL(&s_mux);

            /* Log every derived number. A mistyped travel_m3 silently corrupts
             * three constants at once (§3.5); this line is what makes that
             * visible instead of showing up later as "positioning feels off". */
            ESP_LOGI(TAG,
                     "stroke: travel_m3=%u s -> poll=%lu ms window=%u ms "
                     "nominal=%u (0.1mm/s) reject>%u | device 40004=%u",
                     (unsigned)cfg.travel_s[2], (unsigned long)d.poll_ms,
                     (unsigned)d.window_ms, (unsigned)d.nominal_rate_x10,
                     (unsigned)d.rate_limit_x10, (unsigned)full_travel_x10);

            if (d.window_ms != pushed_window_ms) {
                if (windowpos_set_window_ms(WINDOWPOS_DEFAULT_ADDR, d.window_ms) == WINDOWPOS_OK) {
                    pushed_window_ms = d.window_ms;
                    ESP_LOGI(TAG, "40002 set to %u ms", (unsigned)d.window_ms);
                } else {
                    ESP_LOGW(TAG, "could not set 40002 -- device keeps its own window");
                }
            }
        }

        /* ---- poll ------------------------------------------------------- */
        windowpos_derived_t d;
        (void)windowpos_task_derived(&d);

        windowpos_reading_t r;
        const windowpos_status_t st = windowpos_read(WINDOWPOS_DEFAULT_ADDR, &r);
        if (st == WINDOWPOS_OK) {
            /* FR-WP20: a rate beyond twice nominal is not a fast window, it is a
             * reading to distrust. Reject the sample rather than the sensor --
             * one implausible rate says nothing about the next one. */
            const int32_t rate = (int32_t)r.rate_mm_s_x10;
            const int32_t mag  = (rate < 0) ? -rate : rate;
            if (d.rate_limit_x10 != 0u && mag > (int32_t)d.rate_limit_x10) {
                portENTER_CRITICAL(&s_mux);
                s_cnt.rejected_rate++;
                portEXIT_CRITICAL(&s_mux);
                ESP_LOGW(TAG, "rate %ld beyond %ux nominal (%u) -- sample rejected",
                         (long)rate, (unsigned)RATE_LIMIT_MULT, (unsigned)d.rate_limit_x10);
            } else {
                portENTER_CRITICAL(&s_mux);
                s_last         = r;
                s_last_ms      = now_ms();
                s_have_reading = true;
                s_cnt.reads_ok++;
                portEXIT_CRITICAL(&s_mux);
            }
        } else if (st == WINDOWPOS_ERR_BUSY) {
            /* T5 held the bus. Counted separately because AT-WP05 asks exactly
             * how often a second caller loses that race. */
            portENTER_CRITICAL(&s_mux);
            s_cnt.err_busy++;
            portEXIT_CRITICAL(&s_mux);
            ESP_LOGD(TAG, "bus busy");
        } else if (st == WINDOWPOS_ERR_COMM) {
            portENTER_CRITICAL(&s_mux);
            s_cnt.err_comm++;
            portEXIT_CRITICAL(&s_mux);
            ESP_LOGD(TAG, "read failed (comm)");
        }

        vTaskDelay(pdMS_TO_TICKS(d.poll_ms ? d.poll_ms : IDLE_TICK_MS));
    }
}
