/**
 * @file window_pos_task.cpp
 * @brief T17 — window position task. See window_pos_task.h.
 */

#include "window_pos_task.h"
#ifdef MODBUS_BENCH
#include "commission.h"   /* §6.3 item 4 — traverse measurement watches the same readings */
#endif

#include "../types/app_types.h"
#include "../data_manager/data_manager.h"
#include "../relay_controller/relay_controller.h"
#include "../event_logger/event_logger.h"

#include "modbus_rtu.h"
#include <time.h>

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

/**
 * §12.4 rule 1, "moving means moving": how long the relay may be energised
 * before the absence of movement is a fault, and the fraction of nominal the
 * measured rate must beat to count as moving.
 *
 * The plan specifies "~half nominal within ~5 s". Both numbers are deliberately
 * loose: this detects a leaf that is not moving AT ALL, not one moving slightly
 * slow, so the threshold sits far below anything a healthy stroke produces
 * (a real stroke runs at nominal by construction -- nominal IS full travel over
 * `travel_m3`). Sizing it tight would trade the thing it catches for false
 * trips on motor run-up.
 *
 * The grace is additionally capped at half the stroke, because `travel_m3` can
 * legally be as low as CFG_MIN_TRAVEL_S = 5 s -- exactly the ungated grace. At
 * that setting a fixed 5 s would expire only as the stroke ended, so the rule
 * would never get a verdict on the fastest windows. Half the stroke always
 * leaves half of it to measure.
 */
#define RULE1_GRACE_MS       5000u
#define RULE1_RATE_DIVISOR      2u

/** Device floor for `40002` (contract §4.2). Also the fastest useful poll. */
#define DEVICE_MIN_WINDOW_MS  100u
#define DEVICE_MAX_WINDOW_MS 60000u

/** Never poll slower than this while travelling, however long the stroke. */
#define POLL_MAX_MS          5000u

/** Sleep between checks for "is anything moving?" while idle. */
#define IDLE_TICK_MS          500u

/**
 * Idle logging cadence (Phase 3, plan 3a).
 *
 * **Deliberately temporary** (operator decision 2026-09-07): it exists to build
 * trust in the implementation and costs ~2880 rows/day, roughly +37 % of total
 * log volume. Phase 2 idled with zero bus cost at rest; this trades that for
 * visibility and should be removed once the trace is trusted.
 */
#define IDLE_LOG_MS         30000u

/** SENSOR_HR channel for position samples (0/1/2 taken; plan 3a). */
#define LOG_CH_POSITION         3u
/** LOG_ALARM channel for position events (4 = T/RH, 5 = wind; plan 3b). */
#define LOG_CH_WPOS_EVENT       6u
/** Which window these samples describe. One channel serves all three. */
#define LOG_PARAM_WINDOW_M3     3u

/* ---- sensor-presence gate (Phase 4) ---------------------------------------
 * PROBE_FAIL_LIMIT is 2 to match T5's house convention (sensor_poll.cpp: "on
 * two consecutive failures"), not because 2 is special. One failure is a
 * transient; two running is a sensor that is not answering.
 *
 * PROBE_RETRY_MS is 30 s -- the same cadence the idle path already spent on one
 * read -- so a shut gate costs no more bus time than Phase 3 spent at rest, and
 * strictly less than it spent during a stroke. */
#define PROBE_FAIL_LIMIT        2u
#define PROBE_RETRY_MS      30000u

/* ------------------------------------------------------------------- state */

static portMUX_TYPE       s_mux = portMUX_INITIALIZER_UNLOCKED;
static windowpos_reading_t s_last;
static uint32_t            s_last_ms      = 0u;
static bool                s_have_reading = false;
static windowpos_derived_t s_derived;
static bool                s_have_derived = false;
static windowpos_counters_t s_cnt;

/* ---- gate state. Published via windowpos_task_ctrl_mode(). -----------------
 * s_gate_open means "the sensor answers and is sane". s_ctrl_mode is what other
 * tasks act on. They are deliberately NOT the same variable: the gate may open
 * mid-stroke, but the mode is only promoted at a stroke boundary. */
static bool                    s_gate_open     = false;
static windowpos_gate_reason_t s_gate_reason   = WPOS_GATE_PROBING;
static bool                    s_bench_latched = false;
static uint32_t                s_probe_fail    = 0u;
static uint32_t                s_last_probe_ms = 0u;
static windowpos_ctrl_mode_t   s_ctrl_mode     = WPOS_CTRL_TIMED;

/* Phase 3 event-edge tracking. Owned by the task, no locking needed. */
static bool     s_ev_init        = false;
static bool     s_ev_fault       = false;
static bool     s_ev_teach       = false;
static uint16_t s_ev_status      = 0u;
static uint16_t s_ev_uptime      = 0u;
static uint16_t s_ev_raw_closed  = 0u;
static uint16_t s_ev_raw_open    = 0u;

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

windowpos_ctrl_mode_t windowpos_task_ctrl_mode(windowpos_gate_reason_t *out_reason)
{
    windowpos_ctrl_mode_t m;
    windowpos_gate_reason_t why;
    portENTER_CRITICAL(&s_mux);
    m   = s_ctrl_mode;
    why = s_gate_reason;
    portEXIT_CRITICAL(&s_mux);
    if (out_reason != NULL) { *out_reason = why; }
    return m;
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

/**
 * @brief Emit one position sample (SENSOR_HR, channel 3).
 *
 * `value_a` is the RAW position in 0.1 mm, never the percentage. `30015` is
 * derived from `40004`; if the travel figure is ever mis-measured a logged
 * percentage is corrupt beyond recovery, whereas percentage is always
 * recomputable from a logged millimetre value (plan 3a).
 *
 * On sensor fault the position is logged as **-1**, matching the wind-fault
 * convention (`LOG_PARAM_ALARM_WIND_FAULT` uses va = -1). The device's own
 * 65535 sentinel would truncate to -1 in the int16 log field anyway; making it
 * explicit means the intent survives a reader who does not know that.
 */
static void log_position(const windowpos_reading_t *r)
{
    log_event_t e = {};
    e.timestamp  = (uint32_t)time(NULL);
    e.event_type = (uint8_t)LOG_SENSOR_HR;
    e.initiator  = (uint8_t)LOG_BY_SYSTEM;
    e.channel    = LOG_CH_POSITION;
    e.param_id   = LOG_PARAM_WINDOW_M3;
    e.value_a    = r->sensor_fault ? (int16_t)-1 : (int16_t)r->opening_mm_x10;
    e.value_b    = r->sensor_fault ? (int16_t)0  : r->rate_mm_s_x10;
    log_post(&e);
}

/** @brief Emit one position event (ALARM, channel 6, param 244..247). */
static void log_wpos_event(uint8_t param, int16_t va, int16_t vb)
{
    log_event_t e = {};
    e.timestamp  = (uint32_t)time(NULL);
    e.event_type = (uint8_t)LOG_ALARM;
    e.initiator  = (uint8_t)LOG_BY_SYSTEM;
    e.channel    = LOG_CH_WPOS_EVENT;
    e.param_id   = param;
    e.value_a    = va;
    e.value_b    = vb;
    log_post(&e);
}

/**
 * @brief Emit events for whatever changed since the previous reading.
 *
 * Edge-triggered, so the log explains *why* a trace looks how it does without
 * repeating steady state.
 *
 * Bits 0 and 1 are masked out of the status-change comparison: they are the
 * startup gates and toggle whenever `40002` is written, which would otherwise
 * emit an event on every stroke start.
 */
static void emit_events(const windowpos_reading_t *r, uint8_t addr)
{
    const uint16_t st_cmp = (uint16_t)(r->status_bits & ~0x0003u);

    if (!s_ev_init) {
        s_ev_init   = true;
        s_ev_fault  = r->sensor_fault;
        s_ev_teach  = r->teach_armed;
        s_ev_status = st_cmp;
        return;                    /* first reading is a baseline, not an edge */
    }

    if (r->sensor_fault != s_ev_fault) {
        s_ev_fault = r->sensor_fault;
        log_wpos_event(LOG_PARAM_WPOS_FAULT, s_ev_fault ? 1 : 0, (int16_t)r->status_bits);
        ESP_LOGW(TAG, "position sensor fault %s", s_ev_fault ? "SET" : "cleared");
    }

    if (st_cmp != s_ev_status) {
        s_ev_status = st_cmp;
        log_wpos_event(LOG_PARAM_WPOS_STATUS, (int16_t)r->status_bits, 0);
    }

    /* Teach lifecycle. T17 only OBSERVES: it never reads 30013/30014, because
     * that read is what commits (contract 6.2 d) and T17 must not commit a
     * calibration as a side effect of logging.
     *
     * On disarm, the holdings say which way it went: changed endpoints mean the
     * device committed, unchanged means aborted. **`3 = refused` is NOT emitted
     * here** -- refusal leaves bit 5 SET with 40007 still 1, which is a
     * non-transition T17 cannot distinguish from a teach still in progress. It
     * is reserved for the commissioning path that drives the teach (plan 6.3). */
    if (r->teach_armed != s_ev_teach) {
        s_ev_teach = r->teach_armed;
        if (s_ev_teach) {
            windowpos_config_t c;
            if (windowpos_read_config(addr, &c) == WINDOWPOS_OK) {
                s_ev_raw_closed = c.raw_closed;
                s_ev_raw_open   = c.raw_open;
            }
            log_wpos_event(LOG_PARAM_WPOS_TEACH, 1, 0);
            ESP_LOGI(TAG, "teach armed");
        } else {
            windowpos_config_t c;
            int16_t what = 0;                       /* aborted */
            if (windowpos_read_config(addr, &c) == WINDOWPOS_OK
                && (c.raw_closed != s_ev_raw_closed || c.raw_open != s_ev_raw_open)) {
                what = 2;                           /* committed */
            }
            log_wpos_event(LOG_PARAM_WPOS_TEACH, what, 0);
            ESP_LOGI(TAG, "teach %s", (what == 2) ? "COMMITTED" : "aborted");
        }
    }
}

/**
 * @brief Emit param 247 if the device restarted since the last check.
 *
 * `30008` is a saturating uptime; **it going backwards is the only tell** that
 * the sensor rebooted (contract 7). That matters because a restart resets
 * `40007` to 0 and re-asserts the startup gates, so a teach in progress is
 * silently lost -- and the calibration in 40005/40006 survives, which makes the
 * restart otherwise invisible.
 *
 * Checked at the idle cadence rather than every poll: it is diagnostic, not
 * control, and 30 s is well inside the window where it still explains a trace.
 */
static void check_restart(uint8_t addr)
{
    uint16_t regs[9] = {0};
    if (modbus_read_input_registers(addr, 0x0000u, 9u, regs) != MODBUS_OK) { return; }
    const uint16_t up = regs[7];             /* 0x0007 = 30008 */
    if (s_ev_uptime != 0u && up < s_ev_uptime) {
        log_wpos_event(LOG_PARAM_WPOS_RESTART, (int16_t)(up & 0x7FFFu), 0);
        ESP_LOGW(TAG, "sensor restarted (uptime %u -> %u) -- any armed teach is lost",
                 (unsigned)s_ev_uptime, (unsigned)up);
    }
    s_ev_uptime = up;
}

/** @brief True while any channel is mid-stroke. */
/**
 * @brief Is **M3** travelling?
 *
 * M3 only, and the name says so. Until 2026-09-14 this was
 * `any_channel_travelling()` and returned true for any of the three windows,
 * which put T17 into stroke cadence whenever M1 or M2 moved — against a sensor
 * that measures M3 and nothing else.
 *
 * On the dev rig that cost **~155 transactions per M1/M2 stroke** (100 ms poll,
 * ~168 ms effective, 26 s believed MOVING) re-reading a position that had not
 * changed; in production ~18, because a 171 s travel derives a 1140 ms poll.
 * Wrong on both, and it inflated the very bus load the AT-WP05 arms exist to
 * measure.
 *
 * It also let M3's control mode be **promoted** on another window's stroke
 * boundary. The promotion asymmetry (immediate demotion, promotion only at a
 * boundary) exists so position authority is not gained underneath a consumer
 * already committed to a *timed M3 stroke*; an M1 move says nothing about that.
 *
 * A second instrumented window does **not** belong in this predicate — it gets
 * its own task keyed by its own Modbus address, the way the driver is already
 * structured.
 */
static bool m3_travelling(void)
{
    window_state_t st[3];
    t2_get_window_states(st);   /* M1 = st[0], M2 = st[1], M3 = st[2] */
    return (st[2] == WIN_MOVING_OPEN || st[2] == WIN_MOVING_CLOSE);
}

/**
 * @brief Publish the control mode, logging only on a transition.
 *
 * Edge-triggered for the reason gh#59 exists: a row per poll would bury the one
 * event that matters, and no row at all leaves the log unable to say which
 * control law M3 was under. Uses LOG_PARAM_WPOS_MODE (248) on the ALARM ch6
 * band T17 already owns, so no new event type is needed.
 */
static void publish_mode(windowpos_ctrl_mode_t mode, windowpos_gate_reason_t why)
{
    portENTER_CRITICAL(&s_mux);
    const bool mode_changed   = (s_ctrl_mode != mode);
    const bool reason_changed = (s_gate_reason != why);
    s_ctrl_mode   = mode;
    s_gate_reason = why;
    if (mode_changed) { s_cnt.mode_changes++; }
    portEXIT_CRITICAL(&s_mux);

    if (!mode_changed && !reason_changed) { return; }

    ESP_LOGW(TAG, "M3 control mode -> %s (reason %u)",
             (mode == WPOS_CTRL_POSITION) ? "POSITION (opening distance)"
                                          : "TIMED (travel timer fallback)",
             (unsigned)why);
    log_wpos_event((uint8_t)LOG_PARAM_WPOS_MODE, (int16_t)mode, (int16_t)why);
}

/**
 * @brief Shut the gate and fall back. Demotion is immediate, by design.
 *
 * WINDOWPOS_ERR_BUSY must never reach here: losing the bus lock to T5 says the
 * bus was busy, not that the sensor is absent. Counting it would let ordinary
 * contention disable a working sensor, which is the opposite of what gh#49's
 * mutex was for.
 */
static void gate_close(windowpos_gate_reason_t why)
{
    /* Count the TRANSITION, not the attempt. Guarding on s_gate_open alone
     * would never count a sensor that was absent from boot (the gate was never
     * open), and counting every call would climb every PROBE_RETRY_MS for as
     * long as the sensor stays away. Neither is what the field documents. */
    if (s_gate_open || s_gate_reason != why) {
        portENTER_CRITICAL(&s_mux);
        s_cnt.probe_fail++;
        portEXIT_CRITICAL(&s_mux);
    }
    s_gate_open = false;
    publish_mode(WPOS_CTRL_TIMED, why);
}

/**
 * @brief One presence probe: identify, and decide whether the gate may open.
 *
 * Identify first because contract 9 requires it: a BENCH build carries a
 * deliberate hang hook and must be refused. That refusal is latched permanently
 * because it cannot change without reflashing the device, so there is nothing a
 * re-probe could discover.
 *
 * @return true if the gate is open after this probe.
 */
static bool probe_sensor(void)
{
    s_last_probe_ms = now_ms();

    uint8_t build = 0u, ver = 0u;
    const windowpos_status_t ist =
        windowpos_read_ident(WINDOWPOS_DEFAULT_ADDR, &build, &ver);

    if (ist == WINDOWPOS_ERR_BUSY) {
        /* No evidence either way. Leave the counter and the gate untouched. */
        portENTER_CRITICAL(&s_mux);
        s_cnt.err_busy++;
        portEXIT_CRITICAL(&s_mux);
        return s_gate_open;
    }

    if (ist != WINDOWPOS_OK) {
        if (s_probe_fail < PROBE_FAIL_LIMIT) { s_probe_fail++; }
        if (s_probe_fail >= PROBE_FAIL_LIMIT) { gate_close(WPOS_GATE_NO_SENSOR); }
        return false;
    }

    if (build == WINDOWPOS_BUILD_BENCH) {
        s_bench_latched = true;
        ESP_LOGE(TAG, "sensor reports BENCH build 0x%02X -- REFUSING permanently "
                      "(contract 9)", (unsigned)build);
        gate_close(WPOS_GATE_BENCH_BUILD);
        return false;
    }

    s_probe_fail = 0u;
    if (!s_gate_open) {
        s_gate_open = true;
        ESP_LOGI(TAG, "sensor present: build 0x%02X fw v%u -- gate OPEN",
                 (unsigned)build, (unsigned)ver);
        /* Deliberately NOT promoting the mode here. Promotion waits for a stroke
         * boundary so no consumer sees position control gain authority
         * underneath a movement already committed to the timer.
         *
         * The REASON is published now, though, keeping the mode as it is: the
         * sensor has been identified, so leaving the reason at
         * WPOS_GATE_PROBING would claim we are still deciding. Between boot and
         * the first stroke the honest state is TIMED-because-not-yet-promoted,
         * not TIMED-because-probing. */
        publish_mode(s_ctrl_mode, WPOS_GATE_OK);
    }
    return true;
}

void task_window_pos(void *pvParameters)
{
    (void)pvParameters;
    esp_task_wdt_add(NULL);

    ESP_LOGI(TAG, "T17 starting (window position, addr %u)", (unsigned)WINDOWPOS_DEFAULT_ADDR);

    /* ---- presence gate: probe before trusting anything (contract 9) ------
     * Before Phase 4 these three branches logged and then fell through into the
     * loop regardless, so "T17 idle" did not idle and "REFUSING" did not
     * refuse. The probe now decides, and the loop below respects it. */
    (void)probe_sensor();
    if (!s_gate_open && s_gate_reason == WPOS_GATE_PROBING) {
        /* One failure is not a verdict; PROBE_FAIL_LIMIT is 2. Say so rather
         * than leaving the log implying the sensor was ruled absent. */
        ESP_LOGW(TAG, "first probe failed -- retrying, no verdict yet");
    }

    uint16_t pushed_window_ms  = 0u;
    bool     was_travelling    = false;
    uint32_t last_idle_log_ms  = 0u;

    /* §12.4 rule 1 state, reset at every stroke boundary. Stroke-local rather
     * than static: the question is always "did THIS stroke move?", and carrying
     * a verdict across strokes would let one stalled stroke silence the next. */
    uint32_t stroke_start_ms   = 0u;
    uint16_t stroke_peak_x10   = 0u;
    uint16_t stroke_samples    = 0u;
    bool     stall_reported    = false;

    for (;;) {
        esp_task_wdt_reset();

        /* ---- the gate ---------------------------------------------------
         * Placed here, above every bus-touching path, rather than at each
         * call site: the stroke poll, the 30 s idle sample and check_restart()
         * all touch the bus, and guarding them one by one guarantees the next
         * one added is missed. (T17 never *drives* a teach -- it decodes teach
         * state out of the ordinary poll -- so there is no teach request left
         * stranded here. The commissioning path drives a teach through the
         * bench endpoint, which reads the device directly and is deliberately
         * outside the gate.) */
        if (!s_gate_open) {
            /* Count only ticks with an **M3** stroke in progress. That is the
             * case the gate exists to remove -- a 100 ms derived poll against a
             * ~215 ms timeout holds the bus continuously for the whole stroke.
             * Counting every tick instead would reach ~172 800/day on an idle
             * unit and measure nothing.
             *
             * M3-only since 2026-09-14: counting M1/M2 strokes here overstated
             * what the gate saves, because with the predicate fixed T17 would
             * not have polled during those strokes either. */
            if (m3_travelling()) {
                portENTER_CRITICAL(&s_mux);
                s_cnt.gated_polls++;
                portEXIT_CRITICAL(&s_mux);
            }

            if (!s_bench_latched &&
                (uint32_t)(now_ms() - s_last_probe_ms) >= PROBE_RETRY_MS) {
                (void)probe_sensor();
            }
            vTaskDelay(pdMS_TO_TICKS(IDLE_TICK_MS));
            continue;
        }

        if (!m3_travelling()) {
            was_travelling = false;
            /* Phase 3: still sample at the idle cadence so the log shows the
             * window sitting still, not a gap. Phase 2 idled with zero bus
             * cost; this is the deliberate, temporary trade (see IDLE_LOG_MS). */
            if ((uint32_t)(now_ms() - last_idle_log_ms) >= IDLE_LOG_MS) {
                last_idle_log_ms = now_ms();
                windowpos_reading_t ir;
                const windowpos_status_t ist = windowpos_read(WINDOWPOS_DEFAULT_ADDR, &ir);
                if (ist == WINDOWPOS_OK) {
                    check_restart(WINDOWPOS_DEFAULT_ADDR);
                    emit_events(&ir, WINDOWPOS_DEFAULT_ADDR);
                    log_position(&ir);
#ifdef MODBUS_BENCH
                    commission_tick(&ir, now_ms());
#endif
                    portENTER_CRITICAL(&s_mux);
                    s_last = ir; s_last_ms = now_ms(); s_have_reading = true; s_cnt.reads_ok++;
                    portEXIT_CRITICAL(&s_mux);
                    if (ir.sensor_fault) {
                        gate_close(WPOS_GATE_DEVICE_FAULT);
                    } else {
                        s_probe_fail = 0u;
                    }
                } else if (ist == WINDOWPOS_ERR_BUSY) {
                    /* T5 held the bus. No evidence about the sensor. */
                    portENTER_CRITICAL(&s_mux);
                    s_cnt.err_busy++;
                    portEXIT_CRITICAL(&s_mux);
                } else {
                    /* The idle read MUST judge the sensor too, not just log it.
                     * Phase 3 wrote `if (read == OK) {...}` with no else, and
                     * AT-WP06 on FDA4 (2026-09-12) showed what that costs once
                     * a gate depends on it: the encoder was unplugged for 50 s,
                     * two idle reads vanished without a trace (a 91 s hole in
                     * the ch3 rows), err_comm stayed 0 and the mode stayed
                     * POSITION with nothing on the other end of the cable.
                     *
                     * Every other demotion path needs a stroke in progress or
                     * an already-shut gate, so without this branch the gate is
                     * blind exactly when M3 is at rest -- which is most of the
                     * time, and all night. Same counter and same limit as the
                     * stroke poll, so "absent at rest", "absent at boot" and
                     * "went away mid-stroke" stay one state machine. */
                    portENTER_CRITICAL(&s_mux);
                    s_cnt.err_comm++;
                    portEXIT_CRITICAL(&s_mux);
                    if (s_probe_fail < PROBE_FAIL_LIMIT) { s_probe_fail++; }
                    if (s_probe_fail >= PROBE_FAIL_LIMIT) {
                        gate_close(WPOS_GATE_NO_SENSOR);
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(IDLE_TICK_MS));
            continue;
        }

        /* ---- stroke start: derive, log, push 40002 ---------------------- */
        if (!was_travelling) {
            was_travelling = true;

            /* §12.4 rule 1: a fresh verdict for this stroke. */
            stroke_start_ms = now_ms();
            stroke_peak_x10 = 0u;
            stroke_samples  = 0u;
            stall_reported  = false;

            /* Stroke boundary: the only place the mode is allowed to be
             * PROMOTED. See windowpos_task_ctrl_mode()'s note on the
             * asymmetry -- demotion is immediate, promotion waits. */
            if (s_gate_open && s_ctrl_mode != WPOS_CTRL_POSITION) {
                publish_mode(WPOS_CTRL_POSITION, WPOS_GATE_OK);
            }
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
                /* §12.4 rule 1 evidence, from ACCEPTED samples only. A sample
                 * the plausibility check rejected says nothing about movement
                 * in either direction (FR-WP20's own reasoning), so it must not
                 * be counted as proof the leaf moved. The consequence is
                 * deliberate: a stroke whose every sample is implausible trips
                 * rule 1, because there is then no trustworthy evidence the
                 * window moved -- which is exactly the state worth reporting. */
                if ((uint32_t)mag > (uint32_t)stroke_peak_x10) {
                    stroke_peak_x10 = (uint16_t)clamp_u32((uint32_t)mag, 0u, 65535u);
                }
                if (stroke_samples < 0xFFFFu) { stroke_samples++; }
                portENTER_CRITICAL(&s_mux);
                s_last         = r;
                s_last_ms      = now_ms();
                s_have_reading = true;
                s_cnt.reads_ok++;
                portEXIT_CRITICAL(&s_mux);
                emit_events(&r, WINDOWPOS_DEFAULT_ADDR);
                log_position(&r);
#ifdef MODBUS_BENCH
                commission_tick(&r, now_ms());
#endif
            }
            /* Talking, but useless: the device says its own reading is bad, so
             * position cannot drive the window even though the sensor is there.
             * Distinct from absence, hence its own reason code. */
            if (r.sensor_fault) {
                gate_close(WPOS_GATE_DEVICE_FAULT);
            } else {
                s_probe_fail = 0u;
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
            /* Same two-consecutive-failures rule as the boot probe, and the
             * same counter, so "absent at boot" and "went away mid-stroke"
             * share one state machine instead of two that can disagree. */
            if (s_probe_fail < PROBE_FAIL_LIMIT) { s_probe_fail++; }
            if (s_probe_fail >= PROBE_FAIL_LIMIT) { gate_close(WPOS_GATE_NO_SENSOR); }
        }

        /* ---- §12.4 rule 1 — "moving means moving" -----------------------
         * The relay is energised (we are in the travelling branch). If the
         * measured rate has not reached half nominal by the grace deadline, the
         * leaf is not following the motor.
         *
         * This is the ONLY detector for a shorted wiper: that fault makes the
         * device report a perfectly plausible CONSTANT position, so every
         * status bit stays clear, `sensor_fault` stays false, and bit 6 is
         * inert on this installation for lack of electrical headroom. Without
         * this row a shorted wiper reads as a window that never leaves 0 %.
         *
         * Reports and does not act (Phase 4: "detects and records"). Nothing
         * consumes position yet, so demoting the gate here would change no
         * behaviour while committing to a recovery policy that has no consumer
         * to validate it -- that decision belongs with the T2 change that first
         * makes position drive the actuator.
         *
         * One row per stroke: the latch is cleared only at a stroke boundary. A
         * row per poll would bury the event, which is the gh#59 lesson.
         *
         * `stroke_samples != 0` is load-bearing, not defensive. Without it an
         * encoder that goes ABSENT mid-stroke trips this rule: its reads fail,
         * the peak stays 0, and the grace expires -- reporting "the leaf is not
         * following" when the truth is "the sensor is gone", which
         * WPOS_GATE_NO_SENSOR already says correctly. The gate does shut first
         * in practice (PROBE_FAIL_LIMIT is 2 reads, ~340 ms, against a grace of
         * 2.5-5 s), but that ordering is a timing accident and not something to
         * rest a fault attribution on. Requiring one accepted sample makes the
         * two faults discriminable by construction: no data is never read as
         * no movement. */
        if (!stall_reported && d.nominal_rate_x10 != 0u && stroke_samples != 0u) {
            const uint32_t threshold = (uint32_t)d.nominal_rate_x10 / RULE1_RATE_DIVISOR;
            uint32_t grace = RULE1_GRACE_MS;
            if (d.travel_ms != 0u && (d.travel_ms / 2u) < grace) {
                grace = d.travel_ms / 2u;
            }
            if ((uint32_t)stroke_peak_x10 >= threshold) {
                /* Moved. Settle the verdict for this stroke so the deadline
                 * cannot trip later in a long traverse that pauses. */
                stall_reported = true;
            } else if ((uint32_t)(now_ms() - stroke_start_ms) >= grace) {
                stall_reported = true;
                portENTER_CRITICAL(&s_mux);
                s_cnt.stall_faults++;
                portEXIT_CRITICAL(&s_mux);
                log_wpos_event((uint8_t)LOG_PARAM_WPOS_STALL,
                               (int16_t)clamp_u32((uint32_t)stroke_peak_x10, 0u, 32767u),
                               (int16_t)clamp_u32(threshold, 0u, 32767u));
                ESP_LOGW(TAG,
                         "12.4 rule 1: M3 energised %lu ms, peak rate %u < %lu "
                         "(0.1mm/s) -- leaf not following (wire, obstruction, "
                         "or shorted wiper)",
                         (unsigned long)grace, (unsigned)stroke_peak_x10,
                         (unsigned long)threshold);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(d.poll_ms ? d.poll_ms : IDLE_TICK_MS));
    }
}
