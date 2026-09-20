/**
 * @file window_pos_task.cpp
 * @brief T17 — window position task. See window_pos_task.h.
 */

#include "window_pos_task.h"
#include "commission.h"   /* §6.3 item 4 — the teach runs from these readings (gh#77: every build) */

#include "../types/app_types.h"
#include "../data_manager/data_manager.h"
#include "../relay_controller/relay_controller.h"
#include "../event_logger/event_logger.h"

#include "modbus_rtu.h"
#include "cfg_limits.h"     /* CFG_MAX_TRAVEL_S (gh#72 drive age bound) */
#include "cfg_defaults.h"   /* MOTOR_TRAVEL_MARGIN_S_DEFAULT */
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

/**
 * §12.4 rule 2, "a stop that arrives too early is a fault, not a success":
 * how early is too early, and how many consecutive samples must agree.
 *
 * The plan says "far less than `travel_m3`"; half is the reading taken here,
 * and the timing is deliberately the WEAKER half of the test. The load-bearing
 * condition is bit 3: at the closed switch the device reads 0 **and makes bit 3**
 * (plan §2a), so a position of ~0 with no end sensor is a claim nothing
 * corroborates. That is what makes a legitimate part-way CLOSE safe -- a window
 * starting at 30 % genuinely reaches 0 at 30 % of travel, well inside this
 * window, but it arrives at the switch and bit 3 is made, so the rule stays
 * silent.
 *
 * CONFIRM_SAMPLES exists only for the race that leaves: position may read 0 one
 * poll before bit 3 is made. Two accepted samples is ~1.3 % of any stroke (the
 * poll is travel/150), long enough for a mechanical switch and short enough
 * that a genuine fault is still caught within a percent of the traverse.
 */
#define RULE2_EARLY_DIVISOR     2u
#define RULE2_CONFIRM_SAMPLES   2u

/**
 * 2.10.0 (gh#78): how long a ~0 claim may wait for the CLOSED end sensor.
 *
 * On the rig the position reads 0 for about 1.2 s (7 polls) before the closed
 * end sensor makes, in every close checked -- the closed-end headroom (plan
 * §2a.6). RULE2_CONFIRM_SAMPLES assumed one poll, so a close that reached ~0 in
 * under half the traverse without passing an end sensor was reported as an
 * early stop although it closed normally. The gap is geometry, a fixed
 * distance, so it scales with the traverse: 1.2 s is about 9 % of the rig's
 * 13 s. A quarter of `travel_m3` leaves 2.7x headroom (3.25 s on the rig) and
 * still catches a position that reaches 0 while the leaf is well open, which is
 * what the rule exists for. Production's gap is unmeasured until it has a
 * sensor.
 */
#define RULE2_GAP_DIVISOR       4u

/**
 * 2.10.0 (plan §5d): a drive counts as a FULL traverse, for the travel check,
 * only if T17's first look at it came this soon after relay-on and found an end
 * sensor made (bit 3), so the leaf started at an end. Later than that, where
 * the leaf started is not known. max(2 polls, this). See drive_observe().
 */
#define FULL_START_MIN_MS    1500u

/** 2.10.0: clear bit-3 readings in a row before the leaf counts as having left
 *  its starting end (see drive_observe()). */
#define LEAVE_READINGS          2u

/** Device floor for `40002` (contract §4.2). Also the fastest useful poll. */
#define DEVICE_MIN_WINDOW_MS  100u
#define DEVICE_MAX_WINDOW_MS 60000u

/** Never poll slower than this while travelling, however long the stroke. */
#define POLL_MAX_MS          5000u

/** Sleep between checks for "is anything moving?" while idle. */
#define IDLE_TICK_MS          500u

/**
 * Idle READ cadence (Phase 3, plan 3a).
 *
 * Phase 3 read the sensor at rest to LOG it, a deliberately temporary cost
 * (operator decision 2026-09-07). The read has since become load-bearing: it is
 * how the presence gate judges the sensor at rest (AT-WP06), and how end-sensor
 * events, restarts, orphaned teaches and the teach's STANDBY release are seen
 * while M3 is still. So the read stays. The ROW it wrote every time was the
 * temporary part, and since 2026-09-16 it is written only when the reading says
 * something (rest_row_due()).
 */
#define IDLE_READ_MS        30000u

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

/* ---- fitted or not (gh#73). Owned by the task, no locking needed. ---------- */
static bool     s_fitted            = false;
static bool     s_fitted_known      = false;   /* false until the first read */
static uint32_t s_fitted_checked_ms = 0u;

/** Longest T17 waits at start for T4 to load the configuration. */
#define CFG_WAIT_MAX_MS     10000u

/** gh#72: the longest one drive can last -- the largest `travel_m3` plus T2's
 *  fixed margin. A drive start older than this is not the drive in progress. */
#define DRIVE_AGE_MAX_MS \
    (((uint32_t)CFG_MAX_TRAVEL_S + (uint32_t)MOTOR_TRAVEL_MARGIN_S_DEFAULT) * 1000u)

/* ---- fail-first build for gh#72 ---------------------------------------------
 * `-DWPOS_FAILFIRST_GH72` restores the four behaviours gh#72 changed, so its
 * acceptance test (bin/at_wp_gh72.py) can be shown to FAIL first:
 *  - one verdict per stroke, so the second drive of a reversal is not judged;
 *  - the probe opens the gate on the identity alone, so a self-reported fault
 *    makes the gate flap;
 *  - a shut gate keeps the stroke, so a later stroke inherits it, and a stroke
 *    may be promoted without a rest having been seen;
 *  - a drive is timed from T17's first look, so a drive joined late (a boot
 *    recalibration) is judged on a truncated elapsed time.
 * GET /api/diag/windowpos reports it, so a fail-first result is never read as
 * a real one. Bench builds only. */
#if defined(WPOS_FAILFIRST_GH72) && !defined(MODBUS_BENCH)
#error "WPOS_FAILFIRST_GH72 is a bench-only fail-first build"
#endif

/* ---- fail-first build for 2.10.0 ---------------------------------------------
 * `-DWPOS_FAILFIRST_292` restores 2.9.2's behaviour in everything 2.10.0's
 * acceptance stages exercise, so each can be shown to FAIL first on the same
 * build and with the same bench injections:
 *  - no drive verdicts and no travel check (plan §5d);
 *  - rule 2 as it was (gh#78): any end sensor corroborates, and a ~0 claim
 *    is judged on its second sample;
 *  - bit 4 (both end sensors) does not shut the gate;
 *  - readings carrying the device's start-up bits count as evidence.
 * GET /api/diag/windowpos reports it. Bench builds only. */
#if defined(WPOS_FAILFIRST_292) && !defined(MODBUS_BENCH)
#error "WPOS_FAILFIRST_292 is a bench-only fail-first build"
#endif

/* ---- fail-first build for 2.12.0 ---------------------------------------------
 * `-DWPOS_FAILFIRST_212` restores what mode 2's target rules fixed, so
 * `bin/at_wp_target.py` can be shown to FAIL first on each of them:
 *  - the START of a drive judges freshness by the STOP rule's 3 s limit, so a
 *    target from a resting window is refused as stale (T17 reads every 30 s at
 *    rest, so this refuses essentially every first move);
 *  - the stop rule has no grace, so the first tick of a drive reads the
 *    pre-drive sample as "the position went away" and falls back to the timer;
 *  - the stop rule has no overshoot guard, so a leaf that steps past a narrow
 *    band in one sample runs on to the end;
 *  - a full-travel command does NOT disarm an armed target, so a safety close
 *    can be stopped short by a stale target -- the one that matters.
 * GET /api/diag/windowpos reports it. Bench builds only. */
#if defined(WPOS_FAILFIRST_212) && !defined(MODBUS_BENCH)
#error "WPOS_FAILFIRST_212 is a bench-only fail-first build"
#endif

/* ---- bench test hook (gh#72): see windowpos_task_inject() ----------------- */
#ifdef MODBUS_BENCH
static volatile uint8_t s_inject        = (uint8_t)WPOS_INJECT_NONE;
static volatile bool    s_inject_probe  = false;   /* set when cleared: probe at once */
static bool             s_stuck_valid   = false;   /* T17 only from here on */
static uint16_t         s_stuck_mm_x10  = 0u;
static uint16_t         s_stuck_pct_x10 = 0u;
static uint16_t         s_stuck_avg_x10 = 0u;

void windowpos_task_inject(windowpos_inject_t how)
{
    s_inject = (uint8_t)how;
    if (how == WPOS_INJECT_NONE) { s_inject_probe = true; }
    ESP_LOGW(TAG, "TEST INJECTION -> %u (0 none, 1 absent, 2 fault, 3 stuck, "
                  "4 ends, 5 race)", (unsigned)how);
}

windowpos_inject_t windowpos_task_injected(void)
{
    return (windowpos_inject_t)s_inject;
}

/** True once after the injection was cleared: a shut gate probes at once. */
static bool inject_wants_probe(void)
{
    const bool want = s_inject_probe;
    s_inject_probe = false;
    return want;
}

/** T17's identify, through the injection. */
static windowpos_status_t t17_ident(uint8_t addr, uint8_t *build, uint8_t *ver)
{
    if (s_inject == (uint8_t)WPOS_INJECT_ABSENT) { return WINDOWPOS_ERR_COMM; }
    return windowpos_read_ident(addr, build, ver);
}

/** T17's position read, through the injection. */
static windowpos_status_t t17_read(uint8_t addr, windowpos_reading_t *r)
{
    const uint8_t how = s_inject;
    if (how != (uint8_t)WPOS_INJECT_STUCK) { s_stuck_valid = false; }
    if (how == (uint8_t)WPOS_INJECT_ABSENT) { return WINDOWPOS_ERR_COMM; }
    const windowpos_status_t st = windowpos_read(addr, r);
    if (st != WINDOWPOS_OK) { return st; }
    if (how == (uint8_t)WPOS_INJECT_FAULT) {
        r->wiper_fault  = true;
        r->sensor_fault = true;
        r->status_bits  = (uint16_t)(r->status_bits | 0x0004u);   /* bit 2 */
    } else if (how == (uint8_t)WPOS_INJECT_STUCK) {
        if (!s_stuck_valid) {
            s_stuck_valid   = true;
            s_stuck_mm_x10  = r->opening_mm_x10;
            s_stuck_pct_x10 = r->percent_x10;
            s_stuck_avg_x10 = r->opening_avg_x10;
        }
        r->opening_mm_x10  = s_stuck_mm_x10;
        r->percent_x10     = s_stuck_pct_x10;
        r->opening_avg_x10 = s_stuck_avg_x10;
        r->rate_mm_s_x10   = 0;
    } else if (how == (uint8_t)WPOS_INJECT_ENDS) {
        r->both_end_sensors = true;
        r->status_bits      = (uint16_t)(r->status_bits | 0x0010u);   /* bit 4 */
    } else if (how == (uint8_t)WPOS_INJECT_RACE) {
        r->opening_mm_x10  = 0u;
        r->percent_x10     = 0u;
        r->opening_avg_x10 = 0u;
    }
    return st;
}
#else
static inline bool inject_wants_probe(void) { return false; }
#define t17_ident windowpos_read_ident
#define t17_read  windowpos_read
#endif

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

uint16_t windowpos_task_window_ms_for(uint16_t travel_s)
{
    windowpos_derived_t d;
    derive(travel_s, 0u, &d);        /* the window does not depend on 40004 */
    return d.window_ms;
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
/* value_a of the last ch3 row written (-1 = fault); REST_ROW_NONE before the
 * first. Only T17 writes rows, so no lock. */
#define REST_ROW_NONE  INT32_MIN
static int32_t s_logged_x10 = REST_ROW_NONE;

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
    s_logged_x10 = e.value_a;
}

/**
 * @brief Should a reading taken at REST become a ch3 row?
 *
 * Until 2026-09-16 every idle read did: one row per IDLE_READ_MS, ~2880 a day,
 * about +37 % of the whole log -- a deliberately temporary trade to build trust
 * in the trace (plan §3a). The trace is trusted now, and the resting state is
 * already in ch 2's window bitmask, so at rest a row is written only when it
 * says something:
 *  - the first read after a stroke: where the leaf actually settled;
 *  - the first read after boot, and any change between fault and no fault;
 *  - movement of at least the deadzone WITHOUT a stroke -- the motor box's
 *    hand switches, or slip. T2 cannot see either, so this row is their only
 *    record. Never less than REST_MOVE_MIN_X10: one ADC count is ~1.5 mm here,
 *    and a 1 mm deadzone would log sampling jitter.
 */
#define REST_MOVE_MIN_X10  50u   /* 5 mm */
static bool rest_row_due(const windowpos_reading_t *r, bool after_stroke,
                         uint32_t deadzone_x10)
{
    const int32_t v = r->sensor_fault ? -1 : (int32_t)r->opening_mm_x10;
    if (after_stroke || s_logged_x10 == REST_ROW_NONE) { return true; }
    if ((v < 0) != (s_logged_x10 < 0))                 { return true; }
    if (v < 0)                                         { return false; }
    const uint32_t moved = (uint32_t)((v > s_logged_x10) ? (v - s_logged_x10)
                                                         : (s_logged_x10 - v));
    const uint32_t floor_x10 = (deadzone_x10 > REST_MOVE_MIN_X10) ? deadzone_x10
                                                                 : REST_MOVE_MIN_X10;
    return moved >= floor_x10;
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
        if (s_ev_teach) {
            /* Armed before T17's first look. The disarm below compares the
             * endpoints against the ones taken at arm time, so take them now:
             * left at 0/0, any disarm on a taught sensor logged as COMMITTED --
             * an abort included. Seen 2026-09-16, when a teach started before
             * T17's first idle sample after a reboot. Logged as armed too, so
             * the COMMITTED/aborted row that follows has its opening row. */
            windowpos_config_t c;
            if (windowpos_read_config(addr, &c) == WINDOWPOS_OK) {
                s_ev_raw_closed = c.raw_closed;
                s_ev_raw_open   = c.raw_open;
            }
            log_wpos_event(LOG_PARAM_WPOS_TEACH, 1, 0);
            ESP_LOGI(TAG, "teach already armed at the first reading");
        }
        return;                    /* otherwise a baseline, not an edge */
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

    /* Teach lifecycle, observed from the ordinary poll.
     *
     * Correction, 2026-09-16: this comment used to say T17 never reads
     * 30013/30014. It reads them on EVERY poll -- windowpos_read() is the full
     * 15-register map, and contract 6.3 says that read satisfies the commit
     * handshake. So T17's poll is what completes an armed teach once both ends
     * are captured; the only thing keeping that from being a side effect is
     * that nothing but the commissioning path ever arms one.
     *
     * On disarm, the holdings say which way it went: changed endpoints mean the
     * device committed, unchanged means aborted -- so a re-teach that captures
     * exactly the old endpoints logs as "aborted" here; the commissioning log
     * line is the authority on that. **`3 = refused` is NOT emitted here** --
     * refusal leaves bit 5 SET with 40007 still 1, which is a non-transition
     * T17 cannot distinguish from a teach still in progress. */
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

/** `LOG_PARAM_WPOS_TEACH` value_a: an orphaned teach was found and aborted. */
#define WPOS_TEACH_EV_ORPHAN  4

static bool s_orphan_reported = false;   /* one log row per orphan episode */

/**
 * @brief Abort a teach that nothing on this controller is running.
 *
 * **No teach may run unwatched.** The sensor keeps an armed teach across a
 * CONTROLLER restart -- only its own reset clears `40007` (contract 6.2) -- and
 * a teach whose disarm write failed, or whose sensor dropped out mid-run, is
 * left armed too. The device then keeps capturing, and T17's ordinary poll is
 * the full-map read that satisfies the commit handshake (contract 6.3), so the
 * next two strokes that make both end sensors would commit a calibration
 * nobody asked for or watched. A release build can never be running a teach,
 * so there every armed device is an orphan.
 *
 * Checked on every reading T17 takes, from the flag that reading already
 * carries, so it costs a bus write only when it acts.
 *
 * @param r    a reading taken just now.
 * @param addr device address.
 * @return true if an abort reached the device -- the caller should read again
 *         soon, so the log and the verdict follow bit 5 clearing. A FAILED
 *         write returns false: it is retried at the normal cadence, not every
 *         idle tick, so a device that ignores it is not polled flat out.
 */
static bool check_orphan_teach(const windowpos_reading_t *r, uint8_t addr)
{
    if (!r->teach_armed) {
        s_orphan_reported = false;
        return false;
    }
    if (commission_owns_teach()) { return false; }   /* ours, and watched */
    const windowpos_status_t ws = windowpos_teach(addr, false);
    if (!s_orphan_reported) {
        s_orphan_reported = true;
        portENTER_CRITICAL(&s_mux);
        s_cnt.orphan_aborts++;
        portEXIT_CRITICAL(&s_mux);
        log_wpos_event(LOG_PARAM_WPOS_TEACH, WPOS_TEACH_EV_ORPHAN, (int16_t)ws);
        ESP_LOGW(TAG, "sensor armed with no teach running -- aborting it (write status %d)",
                 (int)ws);
    }
    return ws == WINDOWPOS_OK;
}

/**
 * @brief The gate has just opened: clear an orphan BEFORE anything judges the
 *        calibration, and wait briefly for the device to show it.
 *
 * Judged on the probe's own fresh reading rather than on the first idle
 * sample, which is up to 30 s away -- long enough for the commissioning card
 * to report "teach still armed" after every boot that follows an interrupted
 * teach. (Until gh#72 the probe read only the identity, and this helper took a
 * reading of its own.)
 *
 * @param addr   device address.
 * @param first  the reading the probe just took.
 */
static void clear_orphan_at_gate_open(uint8_t addr, const windowpos_reading_t *first)
{
    windowpos_reading_t r;
    if (!check_orphan_teach(first, addr)) {
        return;
    }
    /* Bit 5 clears a moment after the write. Up to ~2 s, polled here rather
     * than through the idle path, whose failures count towards closing the
     * gate: on 2026-09-16 fast idle reads during an OTA asset upload closed
     * it within 1.5 s. These reads count for nothing.
     *
     * The watchdog is fed on every pass: a read that waits out the bus lock
     * (500 ms) and then times out (~215 ms) makes eight passes ~7.7 s, past
     * T17's 5 s TWDT. */
    for (int i = 0; i < 8; i++) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(250));
        if (t17_read(addr, &r) == WINDOWPOS_OK && !r.teach_armed) {
            s_orphan_reported = false;
            break;
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
 * @brief How T2 is driving M3 right now, and its drive counter (gh#72).
 *
 * The fault checks judge a DRIVE, not a stroke. Until gh#72 they latched the
 * direction once per stroke, from m3_travelling()'s view, in which T2's 2 s
 * reversal gap reads as moving. So a reversal was one stroke to T17: rule 1
 * kept the first direction's settled verdict, and rule 2 kept its direction.
 * T2 bumps the counter every time it energises a relay, so a new value is a
 * new drive even if T17 never saw the gap between them.
 */
static t2_drive_t m3_drive(uint32_t *out_epoch, uint32_t *out_started_ms)
{
    return t2_get_drive(2u, out_epoch, out_started_ms);   /* M3 is channel index 2 */
}

/* ---- 2.10.0: the drive verdict and the travel check (plan §5d) -------------
 * Reports only. T2 still drives on to its timer and believes OPEN or CLOSED
 * afterwards; this says whether the leaf got there.
 *
 * A drive ends in one of four ways, and each is where its verdict is written:
 *  - T2's timer ran out, and T2 rests in the drive's target state (the rest
 *    branch of the loop): judged, confirmed or not reached;
 *  - a reversal: T17 sees T2's gap, or a new drive counter value: not judged;
 *  - T2 rests somewhere else, a motor alarm (WIN_UNKNOWN): not judged;
 *  - the gate shuts, the sensor gone, faulted or reporting both end sensors:
 *    not judged.
 *
 * `s_drv` is T17's own (no lock). `s_confirm` is read by other tasks through
 * windowpos_task_confirm(), so it is written under s_mux. */
typedef enum {
    DRV_END_TIMER = 0,    /* T2 at rest in the target state */
    DRV_END_INTERRUPTED,  /* a reversal, or a new drive */
    DRV_END_NOT_TARGET,   /* T2 at rest elsewhere: a motor alarm */
    DRV_END_SENSOR,       /* the gate shut: absent or faulted */
    DRV_END_BOTH_ENDS,    /* the gate shut: bit 4 */
    DRV_END_PARTIAL,      /* 2.12.0: T2 stopped at a commanded position. The
                           * verdict asks "did it reach the END it was sent
                           * to", and this drive was not sent to one, so the
                           * question does not apply. Without this it reported
                           * NOT_TARGET, which means a motor alarm. */
} drive_end_t;

/* LOG_PARAM_WPOS_CONFIRM value_a, signed by direction (see app_types.h). */
#define VERDICT_CONFIRMED_FULL   1
#define VERDICT_CONFIRMED        2
#define VERDICT_NOT_REACHED      3
#define VERDICT_NOT_JUDGED       4
/* ...and value_b for VERDICT_NOT_JUDGED. */
#define NOTJ_INTERRUPTED         1
#define NOTJ_NOT_TARGET          2
#define NOTJ_SENSOR              3
#define NOTJ_BOTH_ENDS           4
#define NOTJ_NO_READING          5
#define NOTJ_PARTIAL             6   /* 2.12.0: a targeted drive, no end expected */

typedef struct {
    bool     active;        /* a drive is being judged, and has no verdict yet */
    bool     closing;
    uint32_t start_ms;      /* relay-on, from T2 (gh#72) */
    uint32_t travel_ms;     /* `travel_m3` in force when the stroke started */
    bool     first_done;    /* the full-traverse start test has been made */
    bool     full_start;    /* bit 3 at the first look: began at an end, a full traverse */
    bool     left_start;    /* bit 3 seen clear: the leaf left its starting end */
    uint8_t  clear_run;     /* consecutive readings with bit 3 clear */
    bool     made;          /* bit 3 made again after leaving -- the target end */
    uint32_t made_ms;
    bool     target_seen;   /* an accepted reading AT the target end, after leaving */
    bool     have_pct;      /* at least one accepted, usable reading */
    uint16_t last_pct_x10;  /* the opening at the last of them */
} drive_state_t;

static drive_state_t       s_drv;
static windowpos_confirm_t s_confirm;

void windowpos_task_confirm(windowpos_confirm_t *out)
{
    if (out == NULL) { return; }
    portENTER_CRITICAL(&s_mux);
    *out = s_confirm;
    portEXIT_CRITICAL(&s_mux);
}

/** Start judging a drive. */
static void drive_begin(bool closing, uint32_t start_ms, uint32_t travel_ms)
{
    s_drv           = {};
    s_drv.active    = true;
    s_drv.closing   = closing;
    s_drv.start_ms  = start_ms;
    s_drv.travel_ms = travel_ms;
}

/**
 * @brief Evidence from one reading of the drive being judged.
 *
 * Two kinds, and they differ in what they may trust:
 *  - **Bit 3 comes from the end sensors, not the wiper**, so it counts even on
 *    a reading FR-WP20 rejected for its rate. That is what lets the travel
 *    check survive rejected position samples (plan §3.5), and it matters: a
 *    production `travel_m3` on the rig makes every moving sample implausible.
 *  - **The position needs an accepted reading.**
 *
 * Only the target end counts (gh#78): the sensor at the starting end stays
 * made for the first ~1.8 s of every full drive, so a make is the target end's
 * only after bit 3 has been seen clear in this drive.
 *
 * Nothing is taken from a reading that is faulted or carries bit 4 (the gate
 * shuts on both).
 *
 * **The device's start-up bits hold back the POSITION only** (plan §5d, made
 * precise 2026-09-19). `0x01`, no measurement window completed since `40002`
 * was written, means 30001 and 30012 are not yet from a whole window, so the
 * position waits for it; it lasts one window. `0x02`, averaging not yet
 * filled, concerns only the averaged 30002, which T17 never judges on, and
 * lasts the whole averaging window (10 s): refusing readings on it made the
 * first drive after a `travel_m3` change unmeasurable. Bit 3 comes from the
 * end sensors, which neither bit touches.
 *
 * @param accepted     the reading passed FR-WP20.
 * @param poll_ms      the derived poll, for the full-traverse start bound.
 * @param full_x10     `40004`, the open end's position; 0 if unknown.
 * @param deadzone_x10 `deadzone_m3`, 0.1 mm.
 */
static void drive_observe(const windowpos_reading_t *r, bool accepted, uint32_t now,
                          uint32_t poll_ms, uint16_t full_x10, uint16_t deadzone_x10)
{
    if (!s_drv.active || r->sensor_fault || r->both_end_sensors) { return; }
#ifdef WPOS_FAILFIRST_292
    const bool pos_ok = accepted;
#else
    const bool pos_ok = accepted && (r->status_bits & WINDOWPOS_ST_STARTUP_WINDOW) == 0u;
#endif
    const bool bit3 = r->at_end_sensor;

    if (!s_drv.first_done) {
        /* A full traverse starts at an end, and where the leaf started is
         * known only if T17 looked at once. Bit 3 at that first look says it
         * sat at an end -- the opposite one, because a drive toward the end it
         * already sits at never leaves it, and so never produces the make that
         * is timed. Judged on bit 3 rather than the position: with a wrong
         * `travel_m3` FR-WP20 rejects every moving sample, and the first
         * accepted one can then come only at the far end. That is exactly the
         * case the "much longer" warning exists for (the `long` stage of
         * bin/at_wp_confirm.py, 2026-09-19, first failed on this). */
        s_drv.first_done = true;
        uint32_t bound = 2u * poll_ms;
        if (bound < FULL_START_MIN_MS) { bound = FULL_START_MIN_MS; }
        s_drv.full_start = bit3 && (uint32_t)(now - s_drv.start_ms) <= bound;
    }

    if (pos_ok) {
        const uint32_t pos      = r->opening_mm_x10;
        const bool     at_open  = (full_x10 != 0u) &&
                                  (pos + (uint32_t)deadzone_x10 >= (uint32_t)full_x10);
        const bool     at_closed = (pos <= (uint32_t)deadzone_x10);
        const bool     at_target = s_drv.closing ? at_closed : at_open;

        s_drv.have_pct     = true;
        s_drv.last_pct_x10 = r->percent_x10;
        if (bit3 && at_target && s_drv.left_start) { s_drv.target_seen = true; }
    }

    /* Left the starting end only after LEAVE_READINGS clear readings in a
     * row, so a sensor chattering at the edge of its zone cannot make its
     * own re-make pass for the target end's: the travel check would then
     * measure a traverse of a few hundred milliseconds. Not seen on the rig
     * (one release and one make per drive in the 2.9.2 soak's status rows);
     * the guard costs one poll. */
    if (!bit3) {
        if (s_drv.clear_run < 0xFFu) { s_drv.clear_run++; }
        if (s_drv.clear_run >= LEAVE_READINGS) { s_drv.left_start = true; }
    } else {
        s_drv.clear_run = 0u;
        if (s_drv.left_start && !s_drv.made) {
            s_drv.made    = true;
            s_drv.made_ms = now;
        }
    }
}

/**
 * @brief The travel check, on one confirmed full traverse (plan §5d, §3.5).
 *
 * Warns only, and edge-triggered: a row when a direction's state changes, and
 * the flag held while it stands. Nothing here changes `travel_m3`.
 *  - too short: the end sensor made later than `travel_m3`, so only T2's fixed
 *    5 s margin still carries the drive; the next step down is "not reached";
 *  - much longer: it made within half of `travel_m3`. The end switch cuts the
 *    drive, so the mechanism does not mind, but every constant T17 derives
 *    from `travel_m3` (plan §3) is then wrong -- a production value on a rig
 *    module, or a mistyped one.
 */
#ifndef WPOS_FAILFIRST_292   /* the fail-first build has no travel check */
static void travel_check(bool closing, uint32_t measured_ms, uint32_t travel_ms)
{
    const unsigned dir = closing ? 1u : 0u;
    uint8_t state = 0u;
    if (travel_ms != 0u) {
        if (measured_ms > travel_ms)            { state = 1u; }
        else if (measured_ms < travel_ms / 2u)  { state = 2u; }
    }
    portENTER_CRITICAL(&s_mux);
    const uint8_t was = s_confirm.travel_state[dir];
    s_confirm.travel_state[dir] = state;
    s_confirm.traverse_ms[dir]  = measured_ms;
    portEXIT_CRITICAL(&s_mux);
    if (state == was) { return; }

    const int32_t x10 = (int32_t)clamp_u32(measured_ms / 100u, 0u, 32767u);
    log_wpos_event((uint8_t)LOG_PARAM_WPOS_TRAVEL,
                   (int16_t)(state * 1000u + clamp_u32(travel_ms / 1000u, 0u, 999u)),
                   (int16_t)(closing ? -x10 : x10));
    if (state == 0u) {
        ESP_LOGI(TAG, "travel check: M3 %s traverse %lu ms is within travel_m3 %lu ms again",
                 closing ? "CLOSE" : "OPEN", (unsigned long)measured_ms,
                 (unsigned long)travel_ms);
    } else {
        ESP_LOGW(TAG, "travel check: M3 %s traverse %lu ms against travel_m3 %lu ms -- %s",
                 closing ? "CLOSE" : "OPEN", (unsigned long)measured_ms,
                 (unsigned long)travel_ms,
                 (state == 1u) ? "travel_m3 TOO SHORT, only the 5 s margin is left"
                               : "travel_m3 much longer than needed");
    }
}
#endif

/**
 * @brief Write the verdict for the drive being judged, and stop judging it.
 *
 * @param how        how the drive ended.
 * @param at_target  rule 1's "began and stayed on its target end": a drive toward
 *                   the end the leaf already sits at, such as the recalibration
 *                   of a closed M3, is confirmed without ever leaving.
 * @param samples    rule 1's accepted, usable readings of this drive.
 */
static void drive_finish(drive_end_t how, bool at_target, uint16_t samples)
{
    if (!s_drv.active) { return; }
    s_drv.active = false;
#ifdef WPOS_FAILFIRST_292
    (void)how; (void)at_target; (void)samples;
#else
    int code = VERDICT_NOT_JUDGED;
    int vb   = NOTJ_INTERRUPTED;
    switch (how) {
    case DRV_END_TIMER: {
        const bool confirmed = (at_target && samples != 0u) || s_drv.target_seen;
        if (confirmed) {
            const bool full = s_drv.full_start && s_drv.made;
            code = full ? VERDICT_CONFIRMED_FULL : VERDICT_CONFIRMED;
            vb   = s_drv.made
                   ? (int)clamp_u32((s_drv.made_ms - s_drv.start_ms) / 100u, 0u, 32767u)
                   : 0;
            if (full) {
                travel_check(s_drv.closing, s_drv.made_ms - s_drv.start_ms, s_drv.travel_ms);
            }
        } else if (!s_drv.have_pct) {
            vb = NOTJ_NO_READING;
        } else {
            code = VERDICT_NOT_REACHED;
            vb   = (int)clamp_u32((uint32_t)s_drv.last_pct_x10, 0u, 32767u);
        }
        break;
    }
    case DRV_END_INTERRUPTED: vb = NOTJ_INTERRUPTED; break;
    case DRV_END_NOT_TARGET:  vb = NOTJ_NOT_TARGET;  break;
    case DRV_END_SENSOR:      vb = NOTJ_SENSOR;      break;
    case DRV_END_BOTH_ENDS:   vb = NOTJ_BOTH_ENDS;   break;
    case DRV_END_PARTIAL:     vb = NOTJ_PARTIAL;     break;
    }

    portENTER_CRITICAL(&s_mux);
    if (code == VERDICT_NOT_REACHED) {
        s_cnt.not_reached++;
        s_confirm.not_confirmed = true;
    } else if (code == VERDICT_NOT_JUDGED) {
        s_cnt.not_judged++;
    } else {
        s_cnt.confirmed++;
        s_confirm.not_confirmed = false;
    }
    portEXIT_CRITICAL(&s_mux);

    log_wpos_event((uint8_t)LOG_PARAM_WPOS_CONFIRM,
                   (int16_t)(s_drv.closing ? -code : code), (int16_t)vb);
    if (code == VERDICT_NOT_REACHED) {
        ESP_LOGW(TAG, "M3 %s NOT REACHED: the drive ran its full timer and the leaf "
                      "stopped at %d.%d %% -- travel_m3 too short, or the mechanism",
                 s_drv.closing ? "CLOSE" : "OPEN", vb / 10, vb % 10);
    } else {
        ESP_LOGI(TAG, "M3 %s verdict %d (%d)", s_drv.closing ? "CLOSE" : "OPEN", code, vb);
    }
#endif
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
    /* 2.10.0: a drive the gate shuts on cannot be judged. It finishes on T2's
     * timer, as it always did, and the fault is reported by the mode row. */
    drive_finish((why == WPOS_GATE_END_SENSORS) ? DRV_END_BOTH_ENDS : DRV_END_SENSOR,
                 false, 0u);
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
        t17_ident(WINDOWPOS_DEFAULT_ADDR, &build, &ver);

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

    /* gh#72: identifying is not enough. A device that answers and reports its
     * own fault (wiper open, or the 65535 sentinel) identifies fine, so a probe
     * that stopped here re-opened the gate at every 30 s retry, and the next
     * idle read shut it again: two mode rows per cycle, a probe failure counted
     * each time, and any stroke starting in between promoted, for as long as
     * the fault lasted. So read the position too, and stay shut on a faulted
     * reading. */
    windowpos_reading_t pr;
    const windowpos_status_t rst = t17_read(WINDOWPOS_DEFAULT_ADDR, &pr);
    if (rst == WINDOWPOS_ERR_BUSY) {
        portENTER_CRITICAL(&s_mux);
        s_cnt.err_busy++;
        portEXIT_CRITICAL(&s_mux);
        return s_gate_open;
    }
    if (rst != WINDOWPOS_OK) {
        if (s_probe_fail < PROBE_FAIL_LIMIT) { s_probe_fail++; }
        if (s_probe_fail >= PROBE_FAIL_LIMIT) { gate_close(WPOS_GATE_NO_SENSOR); }
        return false;
    }
    s_probe_fail = 0u;                        /* it answers, so it is not absent */
#ifndef WPOS_FAILFIRST_GH72
    if (pr.sensor_fault) {
        gate_close(WPOS_GATE_DEVICE_FAULT);   /* counts and logs only a transition */
        return false;
    }
#endif
#ifndef WPOS_FAILFIRST_292
    /* 2.10.0: both end sensors active is an end-sensor wiring fault, and bit 3
     * cannot be believed either way until it clears. */
    if (pr.both_end_sensors) {
        gate_close(WPOS_GATE_END_SENSORS);
        return false;
    }
#endif

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
        clear_orphan_at_gate_open(WINDOWPOS_DEFAULT_ADDR, &pr);
        /* Judge the sensor's calibration now that it is confirmed present.
         *
         * Before 2026-09-16 nothing did this at boot: commission_refresh() ran
         * only after an operator action (set window size, teach, abort, or an
         * explicit "refresh" POST that the GUI never sends), and the GET
         * handler serves a cached status. So after every reboot the Linear
         * control card reported a fully calibrated encoder as verdict UNKNOWN
         * with window size 0 -- right beside a "Teach (moves M3)" button. That
         * is an invitation to re-teach a working sensor, which MOVES the
         * window and can mis-calibrate it.
         *
         * Here rather than once at task start: this runs every time the gate
         * re-opens, so a sensor that was absent at boot, or unplugged and
         * refitted, is re-judged when it comes back instead of keeping a stale
         * NO_DEVICE verdict. It costs one holding-register read per gate
         * opening, on the task that already owns this device.
         *
         * After clear_orphan_at_gate_open(), above, so a teach left armed by a
         * restart is judged as the abort left it, not as "still armed".
         *
         * gh#77 (2026-09-20): in every build. A release build used to leave the
         * verdict at UNKNOWN for ever, which was invisible while the card was
         * greyed and would have been the first thing an operator saw once it
         * was not. It costs one holding-register read per gate opening. */
        commission_refresh();
    }
    return true;
}

/**
 * @brief Follow `motor/wpos_fitted_m3`, acting on a change (gh#73).
 *
 * Read at most once per IDLE_TICK_MS, so a stroke polling at 100 ms does not
 * take T4's lock ten times a second for a value an operator changes once.
 *
 * On any change, start over:
 *  - **not fitted:** shut the gate WITHOUT counting a probe failure (nothing
 *    failed), forget the last reading so nothing reports a position from a
 *    sensor the operator says is not there, and publish TIMED / NOT_FITTED;
 *  - **fitted:** no verdict and no failures yet, as at boot, and a probe due
 *    at once. The bench-build latch is cleared too: switching the setting off
 *    and on is how an operator asks for a fresh look, after swapping the
 *    encoder for instance, and one identify read costs nothing.
 *
 * The event baseline and the rest-row memory are reset in both directions, so
 * the first reading after a change is logged as a fresh start rather than as
 * edges against a reading from before it.
 *
 * @return true if a sensor is fitted.
 */
static bool follow_fitted(void)
{
    const uint32_t now = now_ms();
    if (s_fitted_known && (uint32_t)(now - s_fitted_checked_ms) < IDLE_TICK_MS) {
        return s_fitted;
    }
    s_fitted_checked_ms = now;
    const bool fitted = dm_wpos_fitted_m3();
    if (s_fitted_known && fitted == s_fitted) {
        return fitted;
    }
    s_fitted_known = true;
    s_fitted       = fitted;

    s_gate_open       = false;
    s_probe_fail      = 0u;
    s_bench_latched   = false;
    s_ev_init         = false;
    s_logged_x10      = REST_ROW_NONE;
    s_orphan_reported = false;
    s_drv.active      = false;   /* a drive in progress is not judged: no row */
    portENTER_CRITICAL(&s_mux);
    s_have_reading = false;
    s_confirm      = {};         /* no sensor, no verdict flags */
    portEXIT_CRITICAL(&s_mux);

    if (!fitted) {
        ESP_LOGI(TAG, "no position sensor fitted (motor/wpos_fitted_m3 = 0) -- "
                      "T17 stays off the bus");
        publish_mode(WPOS_CTRL_TIMED, WPOS_GATE_NOT_FITTED);
    } else {
        ESP_LOGI(TAG, "position sensor fitted -- probing address %u",
                 (unsigned)WINDOWPOS_DEFAULT_ADDR);
        /* At boot this is no change (PROBING is the initial reason) and logs
         * nothing. After a switch-on it logs one row, which is the record
         * that the operator turned the sensor on. */
        publish_mode(WPOS_CTRL_TIMED, WPOS_GATE_PROBING);
        s_last_probe_ms = now - PROBE_RETRY_MS;   /* due on the next pass */
    }
    return fitted;
}

void task_window_pos(void *pvParameters)
{
    (void)pvParameters;
    esp_task_wdt_add(NULL);

    ESP_LOGI(TAG, "T17 starting (window position, addr %u)", (unsigned)WINDOWPOS_DEFAULT_ADDR);

    /* ---- fitted? (gh#73) ----------------------------------------------------
     * The setting decides whether T17 touches the bus at all, so read it only
     * once T4 has loaded it: before that the shadow is zeros, which reads as
     * "not fitted" and would log a NOT_FITTED row on every fitted unit's boot.
     * The wait is capped; past it T17 goes on with whatever the shadow holds,
     * and a zero there keeps it off the bus, which is the safe side. */
    for (uint32_t waited = 0u; !dm_cfg_loaded() && waited < CFG_WAIT_MAX_MS;
         waited += 100u) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!dm_cfg_loaded()) {
        ESP_LOGW(TAG, "configuration not loaded after %u ms -- going on without it",
                 (unsigned)CFG_WAIT_MAX_MS);
    }

    /* ---- presence gate: probe before trusting anything (contract 9) ------
     * Before Phase 4 these three branches logged and then fell through into the
     * loop regardless, so "T17 idle" did not idle and "REFUSING" did not
     * refuse. The probe now decides, and the loop below respects it. */
    if (follow_fitted()) {
        (void)probe_sensor();
        if (!s_gate_open && s_gate_reason == WPOS_GATE_PROBING) {
            /* One failure is not a verdict; PROBE_FAIL_LIMIT is 2. Say so
             * rather than leaving the log implying the sensor was ruled absent. */
            ESP_LOGW(TAG, "first probe failed -- retrying, no verdict yet");
        }
    }

    uint16_t pushed_window_ms  = 0u;
    bool     was_travelling    = false;
    uint32_t last_idle_read_ms = 0u;
    bool     resample_soon     = false;   /* an orphan abort wants a prompt re-read */
    bool     settle_row_due    = false;   /* a stroke just ended: log where the leaf settled */

    /* gh#72: the fault checks judge one DRIVE at a time, not one stroke. A
     * stroke is M3 away from rest; a drive is one energisation of one relay,
     * and a reversal puts two drives in one stroke. `seg_active` says a drive
     * of the current stroke is being judged, `seg_epoch` which one (T2's drive
     * counter, see m3_drive()). */
    bool     seg_active        = false;
    uint32_t seg_epoch         = 0u;

    /* gh#72: the mode may be promoted only for a stroke that STARTED with the
     * gate open, which T17 knows only if it saw M3 at rest with the gate open.
     * A gate that re-opens mid-stroke now forgets that stroke (see the gate
     * branch), and the stroke must not then pass for a fresh one. */
    bool     rest_seen         = false;

    /* §12.4 rule 1 state, reset at every drive (gh#72; every stroke before).
     * Local rather than static: the question is always "did THIS drive move
     * the leaf?", and carrying a verdict across drives would let one silence
     * the next -- which is what a reversal did until gh#72. */
    uint32_t stroke_start_ms   = 0u;
    uint16_t stroke_peak_x10   = 0u;
    uint16_t stroke_samples    = 0u;
    bool     stall_reported    = false;

    /* §12.4 rule 2 state, same lifetime. `stroke_closing` is latched when the
     * drive starts and `stroke_deadzone_x10` when the stroke starts: T2 may
     * end the stroke while the rule is still confirming, and re-reading either
     * would change the question being asked. */
    bool     stroke_closing      = false;
    bool     stroke_end_seen     = false;
    uint16_t stroke_deadzone_x10 = 0u;
    uint8_t  near_zero_run       = 0u;
    bool     early_reported      = false;

    /* 2.10.0 (gh#78): the ~0 claim and when it was made. Rule 2 now asks
     * whether the CLOSED end sensor follows the claim within travel / 4
     * (RULE2_GAP_DIVISOR), not whether any end sensor was seen, and not on the
     * claim's second sample. `stroke_end_seen` survives for the fail-first
     * build only. */
    bool     zero_claimed        = false;
    uint32_t zero_claim_ms       = 0u;

    /* §12.4 rule 1 exemption: a stroke driven toward the end the leaf is
     * ALREADY at cannot move -- the motor's end switch cuts the drive, by
     * design and outside the controller's view -- so "no movement" there is
     * correct behaviour, not a stall. Found by the 2026-09-16 soak: every
     * CLOSE_ALL recalibration on a closed M3 tripped rule 1 -- each boot
     * (twice per OTA push), and each LCD logout.
     *
     * `stroke_at_target` starts true and is cleared by the first accepted
     * sample that is NOT sitting on the target end. See its use for the
     * reasoning; `stroke_full_x10` is `40004`, the open end's position. */
    uint16_t stroke_full_x10    = 0u;
    bool     stroke_at_target   = true;

    for (;;) {
        esp_task_wdt_reset();

        /* ---- not fitted (gh#73): nothing on the bus, nothing to judge -----
         * Above the gate, for the gate's own reason: every bus-touching path is
         * below this point. Forgetting the stroke means a sensor switched on
         * mid-stroke starts a fresh verdict when its gate opens, rather than
         * inheriting the state of a stroke it never saw begin. */
        if (!follow_fitted()) {
            was_travelling = false;
            rest_seen      = false;
            /* A teach running when the sensor was switched off ends as a
             * sensor failure, exactly as when the sensor goes away. */
            commission_tick(NULL, now_ms());
            vTaskDelay(pdMS_TO_TICKS(IDLE_TICK_MS));
            continue;
        }

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

            /* gh#72: forget the stroke. Its end is not seen while the gate is
             * shut, so a gate re-opening during a LATER stroke used to carry
             * the old stroke's direction, start time, settled verdict and peak
             * into the new one, without a fresh derive or 40002 push either.
             * Forgetting it makes a re-open mid-stroke start a fresh verdict
             * from that moment; `rest_seen` keeps it from promoting the mode. */
#ifndef WPOS_FAILFIRST_GH72
            was_travelling = false;
            rest_seen      = false;
#endif

            /* A bench test that clears its injection wants the re-open now,
             * not at the next 30 s retry (windowpos_task_inject()). */
            const bool probe_now = inject_wants_probe();
            if (!s_bench_latched &&
                (probe_now ||
                 (uint32_t)(now_ms() - s_last_probe_ms) >= PROBE_RETRY_MS)) {
                (void)probe_sensor();
            }
            /* No readings reach the teach runner while the gate is shut, so a
             * teach running when the sensor went away would otherwise wait for
             * ever. NULL tells it there is no sensor; idle, it does nothing. */
            commission_tick(NULL, now_ms());
            vTaskDelay(pdMS_TO_TICKS(IDLE_TICK_MS));
            continue;
        }

        if (!m3_travelling()) {
            if (was_travelling) {
                /* The stroke just ended: read at once, and log where the leaf
                 * settled (rest_row_due()). */
                settle_row_due = true;
                resample_soon  = true;
                /* 2.10.0: the drive in progress ended here. At rest in its
                 * target state means T2's timer ran out, so it is judged. At
                 * rest anywhere else -- a motor alarm leaves M3 UNKNOWN -- it
                 * cannot be. */
                if (s_drv.active) {
                    window_state_t wst[3];
                    t2_get_window_states(wst);
                    const window_state_t target = s_drv.closing ? WIN_CLOSED : WIN_OPEN;
                    /* 2.12.0: a drive that ended part-open was a targeted one.
                     * It is not "the wrong end", it is no end at all, and the
                     * end-sensor verdict has nothing to say about it. */
                    const drive_end_t how = (wst[2] == WIN_PART_OPEN) ? DRV_END_PARTIAL
                                          : (wst[2] == target)        ? DRV_END_TIMER
                                                                      : DRV_END_NOT_TARGET;
                    drive_finish(how,
                                 stroke_at_target, stroke_samples);
                }
            }
            was_travelling = false;
            rest_seen      = true;      /* at rest, gate open: the next stroke may promote */
            /* Keep reading at rest: the gate, the events, the orphan check and
             * the teach's STANDBY release depend on it (IDLE_READ_MS). */
            bool idle_sample_due = resample_soon ||
                (uint32_t)(now_ms() - last_idle_read_ms) >= IDLE_READ_MS;
            /* A running teach needs readings at rest as well: to see bit 5
             * appear before the first leg, to start each next leg when a
             * stroke ends, and to see bit 5 clear. The 30 s idle cadence would
             * stall it between legs, so sample every idle tick while it runs
             * (and for a few ticks after, see commission_wants_prompt_read). */
            if (commission_wants_prompt_read()) { idle_sample_due = true; }
            if (idle_sample_due) {
                last_idle_read_ms = now_ms();
                resample_soon     = false;
                windowpos_reading_t ir;
                const windowpos_status_t ist = t17_read(WINDOWPOS_DEFAULT_ADDR, &ir);
                if (ist == WINDOWPOS_OK) {
                    check_restart(WINDOWPOS_DEFAULT_ADDR);
                    emit_events(&ir, WINDOWPOS_DEFAULT_ADDR);
                    if (rest_row_due(&ir, settle_row_due, stroke_deadzone_x10)) {
                        log_position(&ir);
                    }
                    settle_row_due = false;
                    resample_soon = check_orphan_teach(&ir, WINDOWPOS_DEFAULT_ADDR);
                    commission_tick(&ir, now_ms());
                    portENTER_CRITICAL(&s_mux);
                    s_last = ir; s_last_ms = now_ms(); s_have_reading = true; s_cnt.reads_ok++;
                    portEXIT_CRITICAL(&s_mux);
                    if (ir.sensor_fault) {
                        gate_close(WPOS_GATE_DEVICE_FAULT);
#ifndef WPOS_FAILFIRST_292
                    } else if (ir.both_end_sensors) {
                        gate_close(WPOS_GATE_END_SENSORS);   /* 2.10.0 */
#endif
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
            /* The verdicts start with the stroke's first DRIVE, below: T17 may
             * look first during a reversal gap, when nothing is energised. */
            seg_active = false;
            /* 2.10.0: a drive still open here ended where T17 did not see it
             * end, so it cannot be judged. */
            drive_finish(DRV_END_INTERRUPTED, false, 0u);

            /* Stroke boundary: the only place the mode is allowed to be
             * PROMOTED. See windowpos_task_ctrl_mode()'s note on the
             * asymmetry -- demotion is immediate, promotion waits. And only
             * for a stroke that started with the gate open (`rest_seen`,
             * gh#72): a gate re-opening mid-stroke must not promote. */
#ifdef WPOS_FAILFIRST_GH72
            const bool may_promote = true;
            (void)rest_seen;
#else
            const bool may_promote = rest_seen;
#endif
            if (s_gate_open && may_promote && s_ctrl_mode != WPOS_CTRL_POSITION) {
                publish_mode(WPOS_CTRL_POSITION, WPOS_GATE_OK);
            }

            cfg_shadow_t cfg;
            dm_cfg_snapshot(&cfg);

            /* §12.4 rule 2's "~0" band. `deadzone_m3` is the operator's own
             * statement of the smallest position error worth acting on, so it
             * is the right definition of "close enough to closed" -- inventing a
             * second constant here would let the two disagree. Position is
             * 0.1 mm, the key is mm. */
            stroke_deadzone_x10 = (uint16_t)clamp_u32(
                (uint32_t)(cfg.deadzone_m3_mm > 0 ? cfg.deadzone_m3_mm : 0) * 10u,
                0u, 65535u);

            windowpos_config_t dev;
            uint16_t full_travel_x10 = 0u;
            if (windowpos_read_config(WINDOWPOS_DEFAULT_ADDR, &dev) == WINDOWPOS_OK) {
                full_travel_x10 = dev.full_travel_x10;
            }
            /* The OPEN end's position for rule 1's at-target test. The device
             * clamps position at 40004, so a leaf at the open end reads
             * exactly this. 0 means the read failed: the OPEN test then
             * cannot pass, which fails SAFE toward judging the stroke. */
            stroke_full_x10 = full_travel_x10;

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

        /* ---- drive boundary: a fresh verdict per drive (gh#72) ------------
         * T2 reverses M3 immediately for a wind override (T3), a manual LCD
         * command and a recalibration, with a 2 s gap in which both relays are
         * off. Each drive is judged on its own: a new drive counter value
         * starts a new verdict, timed from the moment T17 first sees that
         * relay energised, so the gap cannot read as a stall. During the gap
         * nothing is judged and no evidence is gathered. */
        windowpos_derived_t d;
        (void)windowpos_task_derived(&d);

        uint32_t drive_epoch = 0u;
        uint32_t drive_started_ms = 0u;
        const t2_drive_t drive = m3_drive(&drive_epoch, &drive_started_ms);
        const bool driving = (drive == T2_DRIVE_OPEN || drive == T2_DRIVE_CLOSE);
#ifdef WPOS_FAILFIRST_GH72
        /* The pre-gh#72 behaviour: the first drive starts the only verdict of
         * the stroke, and it is held through gaps and later drives. */
        const bool new_drive = driving && !seg_active;
#else
        const bool new_drive = driving && (!seg_active || drive_epoch != seg_epoch);
#endif
        /* 2.10.0: a reversal ends the drive being judged, before it reached
         * anything. T2's gap is the usual sign; a new counter value with no
         * gap seen is the other (the recalibration takes a moving M3 over
         * directly, as on 2344 at 15:58:20 on 2026-09-18). */
        if (drive == T2_DRIVE_GAP || new_drive) {
            drive_finish(DRV_END_INTERRUPTED, false, 0u);
        }
        if (new_drive) {
            const bool redrive = seg_active;   /* a second drive within one stroke */
            seg_active       = true;
            seg_epoch        = drive_epoch;
            /* Time the drive from when T2 energised the relay, not from this
             * first look (gh#72, 2026-09-17). T17 can join a drive late: at
             * boot, T2's recalibration of a window that was not closed is
             * already running, and a gate that re-opens joins mid-drive. From
             * the first look, rule 2 judged "~0 in under half the traverse"
             * on a truncated time and reported a false early stop at the
             * closed end (2344, a power cycle with M3 open). A late join may
             * reach rule 1's verdict on its first sample; that is right, the
             * grace had passed. */
            const uint32_t seen_ms = now_ms();
            const uint32_t age_ms  = seen_ms - drive_started_ms;
#ifdef WPOS_FAILFIRST_GH72
            stroke_start_ms  = seen_ms;              /* the old timing */
#else
            /* Both values come from one locked read, so a start time from the
             * future (a huge unsigned age) or older than any drive can last
             * should not occur. If one does, it is not this drive's start:
             * fall back to the first look rather than trust it. */
            stroke_start_ms  = (age_ms <= DRIVE_AGE_MAX_MS) ? drive_started_ms : seen_ms;
#endif
            if (age_ms > 1000u) {
                ESP_LOGI(TAG, "M3 drive joined %lu ms after T2 energised it",
                         (unsigned long)age_ms);
            }
            stroke_peak_x10  = 0u;
            stroke_samples   = 0u;
            stall_reported   = false;
            stroke_closing   = (drive == T2_DRIVE_CLOSE);
            stroke_end_seen  = false;
            near_zero_run    = 0u;
            early_reported   = false;
            stroke_at_target = true;
            zero_claimed     = false;
            zero_claim_ms    = 0u;
            /* 2.10.0: judged from T2's energise time, like the rules. */
            drive_begin(stroke_closing, stroke_start_ms, d.travel_ms);
            /* `strokes` counts judged drives, so a soak's "judged strokes"
             * (strokes - at_end_exempt) includes both halves of a reversal. */
            portENTER_CRITICAL(&s_mux);
            s_cnt.strokes++;
            if (redrive) { s_cnt.redrives++; }
            portEXIT_CRITICAL(&s_mux);
            if (redrive) {
                ESP_LOGI(TAG, "M3 driven again within the stroke, now %s -- "
                              "judging this drive on its own",
                         stroke_closing ? "CLOSE" : "OPEN");
            }
        }
#ifdef WPOS_FAILFIRST_GH72
        const bool judging = seg_active;
#else
        const bool judging = seg_active && driving;
#endif

        /* ---- poll ------------------------------------------------------- */
        windowpos_reading_t r;
        const windowpos_status_t st = t17_read(WINDOWPOS_DEFAULT_ADDR, &r);
        if (st == WINDOWPOS_OK) {
            /* 2.10.0 (plan §5d): start-up bit 0x01 -- no measurement window
             * completed since `40002` was written, which T17 does at the first
             * stroke after a boot or a `travel_m3` change -- says the position
             * and rate are not yet from a whole window. Such a reading is still
             * logged and published, but its position and rate are evidence for
             * nothing: not the rules, not the verdict. It lasts one window.
             * 0x02 (averaging not filled) is not tested: it concerns only the
             * averaged 30002, which nothing here judges on, and it lasts 10 s.
             * See drive_observe(). */
#ifdef WPOS_FAILFIRST_292
            const bool meaningful = true;
#else
            const bool meaningful = (r.status_bits & WINDOWPOS_ST_STARTUP_WINDOW) == 0u;
#endif
            const bool evidence = judging && meaningful;

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
                /* 2.10.0: bit 3 still counts for the verdict -- it comes from
                 * the end sensors, not the wiper (drive_observe()). */
                if (judging) {
                    drive_observe(&r, false, now_ms(), d.poll_ms,
                                  stroke_full_x10, stroke_deadzone_x10);
                }
                /* The teach runner still gets it. The rejection distrusts the
                 * POSITION; bits 3, 4 and 5 come from the end sensors and the
                 * teach state, not the wiper, and a leg whose every sample was
                 * rejected would otherwise hide its end-sensor make. */
                commission_tick(&r, now_ms());
            } else {
                /* §12.4 rule 1 evidence, from ACCEPTED samples only. A sample
                 * the plausibility check rejected says nothing about movement
                 * in either direction (FR-WP20's own reasoning), so it must not
                 * be counted as proof the leaf moved. The consequence is
                 * deliberate: a stroke whose every sample is implausible trips
                 * rule 1, because there is then no trustworthy evidence the
                 * window moved -- which is exactly the state worth reporting.
                 *
                 * gh#72: evidence only while a drive is being judged. A sample
                 * from the reversal gap belongs to no drive -- the leaf is
                 * coasting or stopped -- and counting it would let the old
                 * direction's motion pass for the new one. 2.10.0: nor from a
                 * reading carrying the start-up bits (`evidence`). */
                if (evidence && (uint32_t)mag > (uint32_t)stroke_peak_x10) {
                    stroke_peak_x10 = (uint16_t)clamp_u32((uint32_t)mag, 0u, 65535u);
                }
                if (evidence && stroke_samples < 0xFFFFu) { stroke_samples++; }

                /* §12.4 rule 1 exemption evidence: is the leaf SITTING ON the
                 * end it is being driven toward? Cleared by the first sample
                 * that is not, and never re-set within the drive.
                 *
                 * Both conditions, on EVERY sample of the grace window:
                 *
                 *  - bit 3 made (and bit 4 clear -- bit 4 means the end-sensor
                 *    loop is faulted and bit 3 is not to be believed), and
                 *  - position within `deadzone_m3` of the TARGET end.
                 *
                 * CONTINUITY is what makes this safe, not either condition on
                 * its own. The case that matters is the one rule 1 exists for:
                 * a SHORTED WIPER reads a constant 0 whatever the leaf does,
                 * so on an open window a CLOSE starts at "position 0, bit 3
                 * made" -- bit 3 because the OPEN end sensor is active. A
                 * first-sample test would call that "already closed" and
                 * exempt the very fault the rule is for. But bit 3 comes from
                 * the end sensor, not the wiper, and it DROPS as soon as the
                 * leaf leaves the open end -- ~1.8 s on this rig, inside the
                 * 5 s grace -- so continuity catches it.
                 *
                 * T2's own belief cannot stand in for this: after a reboot T2
                 * reports WIN_UNKNOWN until the CLOSE_ALL calibration finishes,
                 * which is exactly the stroke that needs the exemption.
                 *
                 * The TARGET end matters too: an OPEN stroke on a leaf sitting
                 * at the CLOSED end (the detached-wire case, 2026-09-15 22:36)
                 * fails the position test and is still judged. */
                if (evidence) {
                    const bool on_end = r.at_end_sensor && !r.both_end_sensors
                                        && !r.sensor_fault;
                    bool at_pos;
                    if (stroke_closing) {
                        at_pos = (r.opening_mm_x10 <= stroke_deadzone_x10);
                    } else {
                        at_pos = (stroke_full_x10 != 0u) &&
                                 ((uint32_t)r.opening_mm_x10 + (uint32_t)stroke_deadzone_x10
                                  >= (uint32_t)stroke_full_x10);
                    }
                    if (!(on_end && at_pos)) {
                        stroke_at_target = false;
                    }
                }

#ifdef WPOS_FAILFIRST_292
                /* §12.4 rule 2 evidence, as it was until 2.10.0 (gh#78). */
                if (!judging) {
                    /* gh#72: in a reversal gap; the next drive starts afresh. */
                } else if (r.at_end_sensor || r.both_end_sensors) {
                    stroke_end_seen = true;
                    near_zero_run   = 0u;
                } else if (stroke_closing && !r.sensor_fault &&
                           r.opening_mm_x10 <= stroke_deadzone_x10) {
                    if (near_zero_run < 0xFFu) { near_zero_run++; }
                } else {
                    near_zero_run = 0u;
                }
#else
                /* §12.4 rule 2 evidence, 2.10.0 (gh#78): only the ~0 CLAIM is
                 * gathered here. What corroborates it is the closed end
                 * sensor making after the leaf left its starting end, which
                 * drive_observe() records; the judgement is below the poll.
                 * Until 2.10.0 any bit 3 counted, so the open end sensor at the
                 * start of a full close switched the rule off for the whole
                 * drive, and a claim was judged on its second sample although
                 * the position reads 0 for ~1.2 s before the closed end sensor
                 * makes. */
                if (evidence && stroke_closing && !r.sensor_fault && !r.both_end_sensors) {
                    if (r.opening_mm_x10 <= stroke_deadzone_x10) {
                        if (near_zero_run < 0xFFu) { near_zero_run++; }
                        if (!zero_claimed && near_zero_run >= RULE2_CONFIRM_SAMPLES) {
                            zero_claimed  = true;
                            zero_claim_ms = now_ms();
                        }
                    } else {
                        near_zero_run = 0u;
                    }
                }
#endif
                /* 2.10.0: the verdict's evidence (plan §5d). */
                if (judging) {
                    drive_observe(&r, true, now_ms(), d.poll_ms,
                                  stroke_full_x10, stroke_deadzone_x10);
                }
                portENTER_CRITICAL(&s_mux);
                s_last         = r;
                s_last_ms      = now_ms();
                s_have_reading = true;
                s_cnt.reads_ok++;
                portEXIT_CRITICAL(&s_mux);
                emit_events(&r, WINDOWPOS_DEFAULT_ADDR);
                log_position(&r);
                (void)check_orphan_teach(&r, WINDOWPOS_DEFAULT_ADDR);
                commission_tick(&r, now_ms());
            }
            /* Talking, but useless: the device says its own reading is bad, so
             * position cannot drive the window even though the sensor is there.
             * Distinct from absence, hence its own reason code. */
            if (r.sensor_fault) {
                gate_close(WPOS_GATE_DEVICE_FAULT);
#ifndef WPOS_FAILFIRST_292
            } else if (r.both_end_sensors) {
                gate_close(WPOS_GATE_END_SENSORS);   /* 2.10.0 */
#endif
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
         * One row per drive: the latch is cleared only when a drive starts
         * (gh#72; per stroke before, so a reversal was never judged). A row per
         * poll would bury the event, which is the gh#59 lesson.
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
        if (judging && !stall_reported && d.nominal_rate_x10 != 0u && stroke_samples != 0u) {
            const uint32_t threshold = (uint32_t)d.nominal_rate_x10 / RULE1_RATE_DIVISOR;
            uint32_t grace = RULE1_GRACE_MS;
            if (d.travel_ms != 0u && (d.travel_ms / 2u) < grace) {
                grace = d.travel_ms / 2u;
            }
            if ((uint32_t)stroke_peak_x10 >= threshold) {
                /* Moved. Settle the verdict for this stroke so the deadline
                 * cannot trip later in a long traverse that pauses. */
                stall_reported = true;
            } else if ((uint32_t)(now_ms() - stroke_start_ms) >= grace &&
                       stroke_at_target) {
                /* Driven toward the end it was already at, and it never left
                 * it: the end switch did its job and the leaf correctly did
                 * not move. Not a stall. Counted, so an exemption that fires
                 * too often -- or one that hides a real fault -- is visible in
                 * the soak rather than silent, and so the soak can refuse to
                 * count these strokes towards its power. */
                stall_reported = true;
                portENTER_CRITICAL(&s_mux);
                s_cnt.at_end_exempt++;
                portEXIT_CRITICAL(&s_mux);
                ESP_LOGI(TAG, "12.4 rule 1: M3 %s stroke began and stayed on its "
                              "target end (bit 3 continuous) -- no movement expected, "
                              "not judged",
                         stroke_closing ? "CLOSE" : "OPEN");
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

        /* ---- §12.4 rule 2 — an early stop is a fault, not a success -----
         * A CLOSE whose position claims ~0 in far less than `travel_m3`, with
         * bit 3 never made for the whole stroke, is a claim nothing
         * corroborates: at the closed switch the device reads 0 AND makes bit 3
         * (plan §2a), so the two should arrive together.
         *
         * `stroke_end_seen` is checked over the WHOLE stroke rather than the
         * current sample, which is what the plan's "never set" asks for: a
         * stroke that touched an end sensor at any point has physical
         * corroboration and is not this fault.
         *
         * Reports and does not act, for the same reason as rule 1 -- nothing
         * consumes position yet, and T2 stops this stroke on its own timer
         * regardless. What it changes is that the log no longer records a
         * CLOSE that "succeeded" in a fifth of the time it physically takes.
         *
         * 2.10.0 (gh#78): the claim is judged on the GAP to the closed end
         * sensor, not on its second sample, and only a make after the leaf
         * left its starting end corroborates it. A drive that began and stayed
         * on the closed end (rule 1's `stroke_at_target`, the recalibration of
         * a closed M3) corroborates itself. The gap may be at most travel / 4
         * (RULE2_GAP_DIVISOR). A claim still waiting when the drive ends is
         * not judged here: T2 may simply have stopped in the closed-end
         * headroom, where the position already reads 0 and the sensor has not
         * made yet, and the verdict (not reached) says that honestly. */
#ifndef WPOS_FAILFIRST_292
        (void)stroke_end_seen;  /* the old rule 2's state, unused in this build */
        if (judging && stroke_closing && !early_reported && zero_claimed &&
            d.travel_ms != 0u) {
            const bool corroborated = s_drv.made || (stroke_at_target && stroke_samples != 0u);
            const uint32_t waited   = (uint32_t)(now_ms() - zero_claim_ms);
            if (!corroborated && waited > (d.travel_ms / RULE2_GAP_DIVISOR)) {
                early_reported = true;
                const uint32_t at_s = (uint32_t)(zero_claim_ms - stroke_start_ms) / 1000u;
                portENTER_CRITICAL(&s_mux);
                s_cnt.early_stops++;
                portEXIT_CRITICAL(&s_mux);
                log_wpos_event((uint8_t)LOG_PARAM_WPOS_EARLY,
                               (int16_t)clamp_u32(at_s, 0u, 32767u),
                               (int16_t)clamp_u32(d.travel_ms / 1000u, 0u, 32767u));
                ESP_LOGW(TAG,
                         "12.4 rule 2: M3 CLOSE claimed <= %u (0.1mm) %lu s into a "
                         "%lu s traverse, and the closed end sensor has not followed "
                         "in %lu ms -- position not believed",
                         (unsigned)stroke_deadzone_x10, (unsigned long)at_s,
                         (unsigned long)(d.travel_ms / 1000u), (unsigned long)waited);
            }
        }
#else
        (void)zero_claimed;     /* 2.10.0's rule 2 state, unused in this build */
        (void)zero_claim_ms;
        if (judging && !early_reported && stroke_closing && !stroke_end_seen &&
            near_zero_run >= RULE2_CONFIRM_SAMPLES && d.travel_ms != 0u) {
            const uint32_t elapsed = (uint32_t)(now_ms() - stroke_start_ms);
            if (elapsed < (d.travel_ms / RULE2_EARLY_DIVISOR)) {
                early_reported = true;
                portENTER_CRITICAL(&s_mux);
                s_cnt.early_stops++;
                portEXIT_CRITICAL(&s_mux);
                log_wpos_event((uint8_t)LOG_PARAM_WPOS_EARLY,
                               (int16_t)clamp_u32(elapsed / 1000u, 0u, 32767u),
                               (int16_t)clamp_u32(d.travel_ms / 1000u, 0u, 32767u));
                ESP_LOGW(TAG,
                         "12.4 rule 2: M3 CLOSE claims %u (0.1mm) <= deadzone "
                         "after %lu s of a %lu s traverse, no end sensor -- "
                         "position not believed",
                         (unsigned)stroke_deadzone_x10,
                         (unsigned long)(elapsed / 1000u),
                         (unsigned long)(d.travel_ms / 1000u));
            }
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(d.poll_ms ? d.poll_ms : IDLE_TICK_MS));
    }
}
