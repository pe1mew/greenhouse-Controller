/**
 * @file commission.cpp
 * @brief Teach the wire sensor and judge the calibration — see commission.h.
 */

#ifdef MODBUS_BENCH

#include "commission.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "../data_manager/data_manager.h"
#include "../relay_controller/relay_controller.h"
#include "window_pos_task.h"
#include "../types/app_types.h"

static const char *TAG = "COMMISSION";

/** ADC full scale on this device (contract: `30005` is 0..1023). */
#define ADC_FULL_SCALE      1023u
/**
 * A teach whose two captures barely differ did not see a traverse.
 *
 * The rig taught 0..858 — **84 %** of the range — so a healthy calibration uses
 * most of the ADC. 30 % is well below anything a real full traverse produces and
 * well above the noise of a wiper that never moved.
 */
#define SPAN_MIN_PCT        30u
/** Give up on a traverse after this multiple of the configured travel time. */
#define TEACH_TIMEOUT_MULT  2u

static portMUX_TYPE        s_mux = portMUX_INITIALIZER_UNLOCKED;
static commission_status_t s_st;
static uint32_t            s_t_start_ms;
static bool                s_prev_at_end;
static bool                s_have_prev;

/* The just-committed-teach window (CAL_ERR_VERIFYING). See commission.h. */
static bool                s_verifying;
static uint32_t            s_verify_start_ms;   /* compared by elapsed time: wrap-safe */

/** How long a committed teach may wait for the sensor to clear bit 5 before the
 *  real verdict is shown regardless. A healthy sensor clears it in well under a
 *  second; one that never does has REFUSED the teach, and this is what lets that
 *  surface as INVALID instead of "verifying" forever. */
#define VERIFY_TIMEOUT_MS  30000u

static uint16_t configured_travel_s(void)
{
    cfg_shadow_t cfg;
    dm_cfg_snapshot(&cfg);
    return (uint16_t)cfg.travel_s[2];
}

static void teach_fail(teach_err_t why)
{
    portENTER_CRITICAL(&s_mux);
    s_st.state      = TEACH_FAILED;
    s_st.run_reason = why;
    portEXIT_CRITICAL(&s_mux);
    (void)windowpos_teach(WINDOWPOS_DEFAULT_ADDR, false);   /* leave nothing armed */
    ESP_LOGW(TAG, "teach aborted, reason %d", (int)why);
}

void commission_status(commission_status_t *out)
{
    if (out == NULL) { return; }
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

/**
 * @brief Re-read the device's calibration and judge it.
 *
 * Every check here is mechanical. That is the point: a completed teach is
 * self-consistent by construction, so the operator is not asked to assess the
 * numbers — only to act when a check fails.
 *
 * **One gap, and it is worth stating rather than hiding:** bit 6 (implausible)
 * is inert in the CLOSED direction on this installation. The raw code sits at 0
 * at the closed stop, so a shorted wiper is indistinguishable from a genuinely
 * closed window (plan §2a.6). A verdict of VALID does not exclude that fault.
 */
/*
 * @param fresh  A reading taken NOW, or NULL to take one here.
 *
 * The live status bits must be current, and "current" is the whole bug this
 * parameter exists for (2026-09-16). This used to read them from T17's cached
 * snapshot. On the commit path that snapshot is the reading in which the leaf
 * reached the far end -- taken BEFORE windowpos_read_captures(), which IS the
 * commit and clears the teach. So every successful teach was judged against a
 * reading that still had bit 5 set, and ended "teach complete" beside
 * "INVALID: a teach is still armed". Deterministic: it happened on the very
 * first teach anyone ran from the GUI. The verdict was then cached and never
 * recomputed, so it stayed wrong while the device said otherwise.
 */
static void commission_refresh_with(const windowpos_reading_t *fresh)
{
    windowpos_config_t dev;
    cal_verdict_t v = CAL_UNKNOWN;
    cal_err_t     why = CAL_ERR_NONE;
    uint16_t mm = 0u, lo = 0u, hi = 0u, span = 0u;
    uint8_t  pct = 0u;
    bool     armed = false;

    if (windowpos_read_config(WINDOWPOS_DEFAULT_ADDR, &dev) != WINDOWPOS_OK) {
        v = CAL_UNKNOWN;
        why = CAL_ERR_NO_DEVICE;
    } else {
        mm = (uint16_t)((dev.full_travel_x10 + 5u) / 10u);
        lo = dev.raw_closed;
        hi = dev.raw_open;
        span = (uint16_t)((hi > lo) ? (hi - lo) : (lo - hi));
        pct = (uint8_t)((span * 100u) / ADC_FULL_SCALE);

        windowpos_reading_t r;
        bool have;
        if (fresh != NULL) {
            r = *fresh;
            have = true;
        } else {
            /* ERR_BUSY (T5 holds the bus) leaves `have` false, which skips the
             * live-bit checks rather than guessing. A genuinely armed teach is
             * then caught by commission_tick()'s self-correction on T17's next
             * reading, so this fails towards "recheck", not towards "wrong". */
            have = (windowpos_read(WINDOWPOS_DEFAULT_ADDR, &r) == WINDOWPOS_OK);
        }
        armed = have && r.teach_armed;

        v = CAL_VALID;
        if (dev.full_travel_x10 == 0u)          { v = CAL_INVALID; why = CAL_ERR_NO_WINDOW_SIZE; }
        else if (span == 0u)                    { v = CAL_INVALID; why = CAL_ERR_NOT_TAUGHT; }
        else if (pct < SPAN_MIN_PCT)            { v = CAL_INVALID; why = CAL_ERR_SPAN_NARROW; }
        else if (armed)                         { v = CAL_INVALID; why = CAL_ERR_TEACH_ARMED; }
        else if (have && r.wiper_fault)         { v = CAL_INVALID; why = CAL_ERR_WIPER_OPEN; }
        else if (have && r.implausible)         { v = CAL_INVALID; why = CAL_ERR_IMPLAUSIBLE; }
        else if (have && r.not_following)       { v = CAL_INVALID; why = CAL_ERR_NOT_FOLLOWING; }
    }

    portENTER_CRITICAL(&s_mux);
    s_st.verdict       = v;
    s_st.cal_reason    = why;
    s_st.window_mm     = mm;
    s_st.taught_closed = lo;
    s_st.taught_open   = hi;
    s_st.span          = span;
    s_st.span_pct      = pct;
    s_st.teach_armed   = armed;
    s_verifying        = false;      /* a real judgement ends the verify window */
    portEXIT_CRITICAL(&s_mux);
}

bool commission_wants_prompt_read(void)
{
    portENTER_CRITICAL(&s_mux);
    const bool v = s_verifying;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

void commission_refresh(void)
{
    commission_refresh_with(NULL);   /* take a fresh reading -- never the cache */
}

bool commission_set_window_mm(uint16_t mm)
{
    /* 100 mm is smaller than any real vent; 5000 mm is larger than the 2 m
     * draw-wire unit can reach. Both ends are sanity, not policy. */
    if (mm < 100u || mm > 5000u) { return false; }
    const windowpos_status_t s =
        windowpos_set_full_travel(WINDOWPOS_DEFAULT_ADDR, (uint16_t)(mm * 10u));
    ESP_LOGW(TAG, "window size (end sensor to end sensor) = %u mm -> status %d",
             (unsigned)mm, (int)s);
    if (s == WINDOWPOS_OK) { commission_refresh(); }
    return (s == WINDOWPOS_OK);
}

bool commission_teach_start(void)
{
    /* A FRESH reading, not T17's cache. This check gates a window MOVEMENT, and
     * it used to read windowpos_task_snapshot() while ignoring the age that call
     * returns -- although that call's own header warns that ages of many minutes
     * are normal at rest, because T17 stops polling. With a short dwell, T6 can
     * move M3 well inside that window, so a stale "at an end sensor" could arm a
     * teach on a leaf that is actually mid-travel: one capture and a useless
     * calibration, which is precisely what this check is here to refuse. A read
     * that fails -- including ERR_BUSY -- refuses the teach rather than moving
     * the window on uncertain data; the operator can simply press it again. */
    windowpos_reading_t r;
    if (windowpos_read(WINDOWPOS_DEFAULT_ADDR, &r) != WINDOWPOS_OK || r.sensor_fault) {
        teach_fail(TEACH_ERR_SENSOR);
        return false;
    }
    if (r.both_end_sensors) { teach_fail(TEACH_ERR_BOTH_ENDS); return false; }
    /* The capture register is chosen by DIRECTION of travel, so a teach has to
     * start parked on one end sensor and cross to the other. Starting mid-travel
     * produces one capture and a useless calibration. */
    if (!r.at_end_sensor)   { teach_fail(TEACH_ERR_NOT_AT_END); return false; }

    portENTER_CRITICAL(&s_mux);
    s_st.state      = TEACH_ARMING;
    s_st.run_reason = TEACH_ERR_NONE;
    portEXIT_CRITICAL(&s_mux);

    /* Contract §6.2 order: measurement window BEFORE arming. `30005` refreshes
     * once per window, so a stale capture is silent calibration error -- ~86
     * counts at rig speed with the 1000 ms default. */
    windowpos_derived_t d;
    windowpos_task_derived(&d);
    if (d.window_ms != 0u &&
        windowpos_set_window_ms(WINDOWPOS_DEFAULT_ADDR, d.window_ms) != WINDOWPOS_OK) {
        teach_fail(TEACH_ERR_DEVICE_WRITE);
        return false;
    }
    if (windowpos_teach(WINDOWPOS_DEFAULT_ADDR, true) != WINDOWPOS_OK) {
        teach_fail(TEACH_ERR_DEVICE_WRITE);
        return false;
    }

    /* Drive AWAY from the end we are parked on. Position is meaningful only
     * after the teach, so the direction comes from the leaf's own reading:
     * near zero means closed, so open; otherwise close. */
    const bool go_open = (r.percent_x10 < 500u);
    window_cmd_t cmd = {};
    cmd.action  = go_open ? CMD_OPEN : CMD_CLOSE;
    cmd.channel = 3u;                      /* M3 */
    cmd.source  = SRC_OPERATOR_MANUAL;     /* an admin asked for this, explicitly */
    if (xQueueSend(Q1, &cmd, pdMS_TO_TICKS(500)) != pdTRUE) {
        teach_fail(TEACH_ERR_DEVICE_WRITE);
        return false;
    }

    portENTER_CRITICAL(&s_mux);
    s_st.state       = TEACH_TRAVERSING;
    s_st.dir_is_open = go_open;
    portEXIT_CRITICAL(&s_mux);
    s_t_start_ms = 0u;                     /* set on the first tick */
    s_have_prev  = false;
    ESP_LOGW(TAG, "teach armed; driving M3 %s -- THE WINDOW WILL MOVE",
             go_open ? "OPEN" : "CLOSED");
    return true;
}

void commission_teach_abort(void)
{
    (void)windowpos_teach(WINDOWPOS_DEFAULT_ADDR, false);
    portENTER_CRITICAL(&s_mux);
    s_st.state      = TEACH_IDLE;
    s_st.run_reason = TEACH_ERR_NONE;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "teach abandoned by the operator");
    commission_refresh();
}

void commission_tick(const windowpos_reading_t *r, uint32_t now_ms)
{
    teach_state_t state;
    bool          cached_armed;
    bool          verifying;
    uint32_t      verify_start;
    portENTER_CRITICAL(&s_mux);
    state        = s_st.state;
    cached_armed = s_st.teach_armed;
    verifying    = s_verifying;
    verify_start = s_verify_start_ms;
    portEXIT_CRITICAL(&s_mux);

    /* A committed teach whose sensor never clears bit 5 has been REFUSED. Stop
     * saying "verifying" and show the real verdict, which will be INVALID. */
    if (verifying && (uint32_t)(now_ms - verify_start) >= VERIFY_TIMEOUT_MS) {
        ESP_LOGW(TAG, "teach commit not confirmed within %u ms -- judging anyway",
                 (unsigned)VERIFY_TIMEOUT_MS);
        commission_refresh();
        bool still_armed;
        portENTER_CRITICAL(&s_mux);
        still_armed = s_st.teach_armed;
        portEXIT_CRITICAL(&s_mux);
        if (still_armed) {
            /* The sensor never accepted the commit. That is a FAILED teach, not
             * a complete one -- leaving the state at DONE would recreate the
             * "teach complete" / "still armed" contradiction this whole change
             * exists to remove. teach_fail() also disarms the sensor, so nothing
             * is left armed; re-judge after that. */
            teach_fail(TEACH_ERR_DEVICE_WRITE);
            commission_refresh();
        }
        return;
    }

    /* Self-correction: the stored verdict must follow the live teach bit.
     *
     * A verdict computed once and stored can drift from the device, and
     * nothing re-judged it -- that is how "teach complete" sat beside
     * "teach still armed" indefinitely. Re-judge whenever a real reading shows
     * bit 5 differing from what the verdict assumed, using THAT reading (this
     * is called before T17 updates its snapshot on the idle path, so the
     * snapshot would be stale here too). Skipped during an active teach, where
     * bit 5 is legitimately set and re-judging would only cost bus reads.
     * Changes are rare, so so is the config read this triggers. */
    if (r != NULL && !r->sensor_fault && r->teach_armed != cached_armed &&
        state != TEACH_TRAVERSING && state != TEACH_COMMITTING) {
        commission_refresh_with(r);
    }

    if (state != TEACH_TRAVERSING) { return; }

    if (s_t_start_ms == 0u) { s_t_start_ms = now_ms; }

    if (r == NULL || r->sensor_fault) { teach_fail(TEACH_ERR_SENSOR); return; }
    if (r->both_end_sensors)          { teach_fail(TEACH_ERR_BOTH_ENDS); return; }

    /* A teach interrupted by safety is not a teach. The leaf may have been
     * commanded somewhere else entirely mid-traverse. */
    const EventBits_t eg = xEventGroupGetBits(EG1);
    if (eg & EG1_BIT_MOTOR_ALARM)   { teach_fail(TEACH_ERR_MOTOR_ALARM); return; }
    if (eg & EG1_BIT_WIND_OVERRIDE) { teach_fail(TEACH_ERR_WIND); return; }

    if ((now_ms - s_t_start_ms) >
        (uint32_t)configured_travel_s() * 1000u * TEACH_TIMEOUT_MULT) {
        teach_fail(TEACH_ERR_TIMEOUT);
        return;
    }

    if (!s_have_prev) { s_prev_at_end = r->at_end_sensor; s_have_prev = true; }
    const bool reached_far_end = (!s_prev_at_end && r->at_end_sensor);
    s_prev_at_end = r->at_end_sensor;
    if (!reached_far_end) { return; }

    /* Both ends seen. Reading the captures is the COMMIT (contract §6.2 step d):
     * it persists them to 40005/40006 and clears the teach. It is not a look. */
    portENTER_CRITICAL(&s_mux);
    s_st.state = TEACH_COMMITTING;
    portEXIT_CRITICAL(&s_mux);

    uint16_t cap_closed = 0u, cap_open = 0u;
    const windowpos_status_t s =
        windowpos_read_captures(WINDOWPOS_DEFAULT_ADDR, &cap_closed, &cap_open);
    if (s != WINDOWPOS_OK) { teach_fail(TEACH_ERR_DEVICE_WRITE); return; }

    ESP_LOGW(TAG, "teach committed: closed=%u open=%u", (unsigned)cap_closed,
             (unsigned)cap_open);

    /* Do NOT judge here. The commit has only just been requested; the sensor
     * clears bit 5 and persists the captures a moment later, so any reading
     * taken now -- even a fresh one -- still says "armed" and still holds the
     * previous capture. Judging here produced "teach complete" beside
     * "INVALID: teach still armed" on every teach (measured 2026-09-16, and the
     * first attempt at fixing it, a fresh read, did not help for exactly this
     * reason).
     *
     * Instead: say VERIFYING, keep teach_armed true (it IS still armed on the
     * device), and let the self-correction above judge on the first reading
     * with bit 5 clear. That reading is also late enough to carry the
     * persisted captures. commission_wants_prompt_read() makes T17 take that
     * reading promptly instead of on its 30 s idle cadence. */
    portENTER_CRITICAL(&s_mux);
    s_st.state           = TEACH_DONE;
    s_st.verdict         = CAL_UNKNOWN;
    s_st.cal_reason      = CAL_ERR_VERIFYING;
    s_st.teach_armed     = true;
    s_verifying          = true;
    s_verify_start_ms    = now_ms;
    portEXIT_CRITICAL(&s_mux);
}

#endif /* MODBUS_BENCH */
