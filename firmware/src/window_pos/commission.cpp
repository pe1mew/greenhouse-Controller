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

#include "cfg_defaults.h"   /* MOTOR_TRAVEL_MARGIN_S_DEFAULT -- T2's stroke is travel + this */
#include "../data_manager/data_manager.h"
#include "../relay_controller/relay_controller.h"
#include "../web_server/web_server.h"   /* web_session_is_live() -- the STANDBY hold */
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

/** T2 drains Q1 every 20 ms, so a stroke that has not begun by now is not coming. */
#define LEG_START_MS        3000u
/**
 * A stroke counts as ended once T2 has been at rest this long. The pause lets
 * the device's debounced end-sensor bit reach a reading before the leg is
 * judged, and it is shorter than any dwell, so T6 cannot slip a move in between
 * two legs (T6 observes dwell; our SRC_OPERATOR_MANUAL legs do not).
 */
#define LEG_END_MS          1000u
/** A leg may take this multiple of T2's own stroke time. T2 always ends a stroke
 *  on its own timer, so this is a backstop, not a measurement. */
#define LEG_TIMEOUT_MULT    2u
/** The device must show bit 5 within this long of the arm write. */
#define ARM_CONFIRM_MS      3000u
/**
 * Both ends made, T2 at rest, bit 5 still set: after this long the device has
 * refused. The commit needs one read after the second capture plus one
 * measurement window -- at most ~0.8 s on production (derived window 760 ms),
 * T17 reads every 500 ms here, and the second capture happened before the
 * stroke's overdrive even began.
 */
#define COMMIT_SETTLE_MS    5000u
/** Prompt readings kept up after a run ends. See commission_wants_prompt_read(). */
#define LINGER_READS        10u

static portMUX_TYPE        s_mux = portMUX_INITIALIZER_UNLOCKED;
static commission_status_t s_st;
static uint8_t             s_linger;       /* prompt reads still owed after a run */
/* Both keep T17's orphan check off a teach that is ours. s_starting spans
 * commission_teach_start()'s arm write, before TEACH_ARMING is published.
 * s_quiet spans our own DISARM: bit 5 stays set for a moment after the write
 * while the state already says it is over, so without it an operator abort
 * was logged as an orphan (FDA4, 2026-09-16 12:47:29). If our disarm write
 * failed, the orphan check takes over once the grace has passed. */
static bool                s_starting;
static bool                s_quiet;
static TickType_t          s_quiet_from;
#define DISARM_GRACE_MS    5000u

/* The teach's STANDBY hold (operator decision, 2026-09-16: automatic control
 * pauses during a teach, until the admin's session ends). s_hold_token is the
 * web session that started the most recent teach; s_hold_ended says it has
 * logged out. Both under s_mux.
 *
 * s_hold_mtx makes "take the hold and record its owner" (T11) and "decide and
 * release" (T17, or T11 at logout) atomic with respect to each other. Without
 * it a release check running between the take and the record saw no owner and
 * dropped a hold a new teach had just taken. */
static char                s_hold_token[WEB_SESSION_TOKEN_LEN + 1];
static bool                s_hold_ended;
static SemaphoreHandle_t   s_hold_mtx;
static portMUX_TYPE        s_hold_mtx_init = portMUX_INITIALIZER_UNLOCKED;

static bool hold_lock(void)
{
    if (s_hold_mtx == NULL) {
        /* Created on first use; creating allocates, so not inside the
         * critical section. A task that loses the race deletes its copy. */
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        portENTER_CRITICAL(&s_hold_mtx_init);
        if (s_hold_mtx == NULL) { s_hold_mtx = m; m = NULL; }
        portEXIT_CRITICAL(&s_hold_mtx_init);
        if (m != NULL) { vSemaphoreDelete(m); }
    }
    if (s_hold_mtx != NULL &&
        xSemaphoreTake(s_hold_mtx, pdMS_TO_TICKS(2000)) == pdTRUE) {
        return true;
    }
    ESP_LOGW(TAG, "STANDBY hold lock not free after 2 s -- continuing unserialised");
    return false;
}

static void hold_unlock(bool locked)
{
    if (locked) { (void)xSemaphoreGive(s_hold_mtx); }
}

/*
 * The run. commission_teach_start() (web task) initialises all of it BEFORE it
 * publishes TEACH_ARMING; from then on only T17, through commission_tick(),
 * touches it.
 */
typedef enum {
    LEG_NONE = 0,      /* no stroke of ours in progress */
    LEG_STARTING,      /* Q1 command sent; T2 has not shown the stroke yet */
    LEG_RUNNING,       /* T2 is driving M3 */
    LEG_STOPPING,      /* T2 at rest; waiting LEG_END_MS before judging the leg */
} leg_phase_t;

static leg_phase_t s_phase;
static bool        s_stamped;       /* s_arm_ms holds T17's clock */
static uint32_t    s_arm_ms;        /* first ARMING tick */
static uint32_t    s_leg_ms;        /* when the current leg was commanded */
static uint32_t    s_rest_ms;       /* when T2 was first seen at rest this leg */
static uint32_t    s_settle_ms;     /* when the last leg ended with both ends made */
static bool        s_have_prev;
static bool        s_prev_at_end;   /* bit 3 on the previous reading */
static bool        s_seen_armed;    /* a reading has shown bit 5 during this run */
static bool        s_leg_left_end;  /* bit 3 was clear at some point this leg */
static bool        s_leg_made;      /* an end sensor made during this leg */
static uint8_t     s_legs;          /* legs commanded so far */
static uint8_t     s_noop_legs;     /* consecutive legs that never left their end */
static uint8_t     s_ends;          /* end sensors made since arming, 0..2 */

static uint16_t configured_travel_s(void)
{
    cfg_shadow_t cfg;
    dm_cfg_snapshot(&cfg);
    return (uint16_t)cfg.travel_s[2];
}

static bool teach_active(teach_state_t s)
{
    return s == TEACH_ARMING || s == TEACH_TRAVERSING || s == TEACH_COMMITTING;
}

static bool m3_moving(void)
{
    window_state_t w[3];
    t2_get_window_states(w);   /* reversal gaps report as MOVING too */
    return w[2] == WIN_MOVING_OPEN || w[2] == WIN_MOVING_CLOSE;
}

void commission_status(commission_status_t *out)
{
    if (out == NULL) { return; }
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
    out->standby_held = (dm_standby_holders() & DM_STANDBY_HOLD_TEACH) != 0u;
}

/**
 * @brief Release the teach's STANDBY hold once its session is over.
 *
 * "Over" is: logged out, or no longer valid in the session table (idle
 * timeout, evicted by a fifth login, or gone with a reboot). A RUNNING teach
 * keeps the hold whatever the session does -- the release recalibrates, and a
 * CLOSE_ALL in the middle of a leg would wreck the run -- so the release then
 * waits for the run to end. Called on every T17 reading and at logout.
 */
static void hold_forget(void)
{
    portENTER_CRITICAL(&s_mux);
    s_hold_token[0] = '\0';
    s_hold_ended    = false;
    portEXIT_CRITICAL(&s_mux);
}

static void hold_check(void)
{
    const bool locked = hold_lock();
    if ((dm_standby_holders() & DM_STANDBY_HOLD_TEACH) == 0u) {
        /* Nothing held: never was, already released, or an explicit STANDBY
         * or AUTOMATIC request took the pause over. Forget the owner. */
        hold_forget();
        hold_unlock(locked);
        return;
    }

    char tok[sizeof(s_hold_token)];
    bool ended, running;
    portENTER_CRITICAL(&s_mux);
    running = s_starting || teach_active(s_st.state);
    ended   = s_hold_ended;
    memcpy(tok, s_hold_token, sizeof(tok));
    portEXIT_CRITICAL(&s_mux);

    if (running || (!ended && tok[0] != '\0' && web_session_is_live(tok))) {
        hold_unlock(locked);
        return;
    }

    ESP_LOGW(TAG, "teach STANDBY released: its admin session %s",
             ended ? "logged out" : "has ended (timeout, eviction or reboot)");
    dm_standby_release(DM_STANDBY_HOLD_TEACH, LOG_BY_WEB, 0u /*=web*/);
    hold_forget();
    hold_unlock(locked);
}

void commission_session_ended(const char *token)
{
    if (token == NULL || token[0] == '\0') { return; }
    bool ours = false;
    portENTER_CRITICAL(&s_mux);
    if (s_hold_token[0] != '\0' && strcmp(s_hold_token, token) == 0) {
        s_hold_ended = true;
        ours = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (ours) { hold_check(); }        /* now, unless a teach is still running */
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
 *
 * @param fresh  A reading taken NOW, or NULL to take one here. Never T17's
 *               cached snapshot: its age is unbounded at rest, and a verdict
 *               judged on a stale bit 5 was the first bug this module had.
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
    portEXIT_CRITICAL(&s_mux);
}

void commission_refresh(void)
{
    commission_refresh_with(NULL);   /* take a fresh reading -- never the cache */
}

bool commission_owns_teach(void)
{
    portENTER_CRITICAL(&s_mux);
    bool v = s_starting || teach_active(s_st.state);
    if (!v && s_quiet) {
        if ((TickType_t)(xTaskGetTickCount() - s_quiet_from) < pdMS_TO_TICKS(DISARM_GRACE_MS)) {
            v = true;
        } else {
            s_quiet = false;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return v;
}

bool commission_wants_prompt_read(void)
{
    portENTER_CRITICAL(&s_mux);
    const bool v = teach_active(s_st.state) || (s_linger != 0u);
    portEXIT_CRITICAL(&s_mux);
    return v;
}

/**
 * @brief End a run as FAILED and leave nothing armed.
 *
 * The disarm matters beyond tidiness: a device left armed keeps capturing, so
 * the next ordinary T6 strokes could complete a teach nobody is watching --
 * which is exactly how the first version of this module appeared to work. If
 * this write fails, T17's orphan check retries it (window_pos_task.cpp).
 */
static void teach_fail(teach_err_t why)
{
    portENTER_CRITICAL(&s_mux);
    s_st.state      = TEACH_FAILED;
    s_st.run_reason = why;
    s_linger        = LINGER_READS;
    s_quiet         = true;             /* our disarm follows: not an orphan */
    s_quiet_from    = xTaskGetTickCount();
    portEXIT_CRITICAL(&s_mux);
    s_phase = LEG_NONE;
    (void)windowpos_teach(WINDOWPOS_DEFAULT_ADDR, false);
    ESP_LOGW(TAG, "teach FAILED, reason %d (legs %u, ends %u)",
             (int)why, (unsigned)s_legs, (unsigned)s_ends);
    /* The device clears bit 5 a moment after the disarm, so this verdict may
     * still say "armed"; the self-correction in commission_tick() fixes that
     * on one of the LINGER_READS prompt readings. */
    commission_refresh();
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

bool commission_teach_start(const char *owner_token)
{
    portENTER_CRITICAL(&s_mux);
    const bool running = teach_active(s_st.state);
    portEXIT_CRITICAL(&s_mux);
    if (running) { return false; }   /* one teach at a time; the status says which */

    /* Initialise the run before anything can observe it -- and before any
     * refusal below, whose log line reports these counters. No run is active,
     * so T17 is not touching them. */
    s_phase        = LEG_NONE;
    s_stamped      = false;
    s_arm_ms       = 0u;
    s_leg_ms       = 0u;
    s_rest_ms      = 0u;
    s_settle_ms    = 0u;
    s_have_prev    = false;
    s_prev_at_end  = false;
    s_seen_armed   = false;
    s_leg_left_end = false;
    s_leg_made     = false;
    s_legs         = 0u;
    s_noop_legs    = 0u;
    s_ends         = 0u;

    /* A FRESH reading, not T17's cache: this check gates a window movement,
     * and T17's cache can be many minutes old at rest. A read that fails --
     * including ERR_BUSY -- refuses rather than moving the window on
     * uncertain data; the operator can simply press again. */
    windowpos_reading_t r;
    if (windowpos_read(WINDOWPOS_DEFAULT_ADDR, &r) != WINDOWPOS_OK || r.sensor_fault) {
        teach_fail(TEACH_ERR_SENSOR);
        return false;
    }
    if (r.both_end_sensors) { teach_fail(TEACH_ERR_BOTH_ENDS); return false; }
    if (m3_moving())        { teach_fail(TEACH_ERR_M3_BUSY);   return false; }
    const EventBits_t eg = xEventGroupGetBits(EG1);
    if (eg & EG1_BIT_MOTOR_ALARM)   { teach_fail(TEACH_ERR_MOTOR_ALARM); return false; }
    if (eg & EG1_BIT_WIND_OVERRIDE) { teach_fail(TEACH_ERR_WIND);        return false; }
    /* There is deliberately NO "must be parked at an end" check any more.
     * Every leg moves the leaf, so the device always knows the direction of
     * the movement that made each sensor, whatever the starting point. */

    /* Automatic control pauses for the teach and stays paused until the
     * admin's session ends (operator decision, 2026-09-16) -- held only now
     * that the refusals have passed, so a teach that never starts leaves
     * climate control alone. What remains is a moment between the M3 check
     * above and this line in which T6 could still start a stroke; the first
     * leg then fails with m3_busy, and a retry runs with T6 paused.
     * The hold lock stays taken until the run is published (or refused), so
     * no release check can judge the hold in between: until then nothing says
     * a teach is running. */
    const bool hold_locked = hold_lock();
    if (dm_standby_hold(DM_STANDBY_HOLD_TEACH, LOG_BY_WEB, 0u /*=web*/)) {
        const size_t n = (owner_token != NULL) ? strnlen(owner_token, WEB_SESSION_TOKEN_LEN) : 0u;
        portENTER_CRITICAL(&s_mux);
        memcpy(s_hold_token, (n != 0u) ? owner_token : "", n);
        s_hold_token[n] = '\0';
        s_hold_ended = (n == 0u);          /* no session to wait for: ends with the teach */
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "automatic control paused (STANDBY held) until the admin session ends");
    }

    /* Contract §6.2 order: measurement window BEFORE arming. `30005` refreshes
     * once per window, so a stale capture is silent calibration error -- ~86
     * counts at rig speed with the 1000 ms default. Derived here from the
     * configured travel rather than taken from T17, which has nothing to offer
     * before its first stroke since boot. An unchanged value is a no-op on the
     * device (contract §8), so T17 re-writing the same number at the first
     * leg's stroke start costs nothing. */
    const uint16_t win_ms = windowpos_task_window_ms_for(configured_travel_s());
    if (windowpos_set_window_ms(WINDOWPOS_DEFAULT_ADDR, win_ms) != WINDOWPOS_OK) {
        teach_fail(TEACH_ERR_DEVICE_WRITE);
        hold_unlock(hold_locked);
        return false;
    }

    s_have_prev    = true;
    s_prev_at_end  = r.at_end_sensor;

    portENTER_CRITICAL(&s_mux);
    s_starting = true;
    portEXIT_CRITICAL(&s_mux);
    if (windowpos_teach(WINDOWPOS_DEFAULT_ADDR, true) != WINDOWPOS_OK) {
        portENTER_CRITICAL(&s_mux);
        s_starting = false;
        portEXIT_CRITICAL(&s_mux);
        teach_fail(TEACH_ERR_DEVICE_WRITE);
        hold_unlock(hold_locked);
        return false;
    }

    portENTER_CRITICAL(&s_mux);
    s_st.state      = TEACH_ARMING;     /* published last: T17 acts on it */
    s_st.run_reason = TEACH_ERR_NONE;
    s_st.leg        = 0u;
    s_st.ends_made  = 0u;
    s_linger        = 0u;
    s_starting      = false;            /* the state now says it */
    portEXIT_CRITICAL(&s_mux);
    hold_unlock(hold_locked);
    ESP_LOGW(TAG, "teach armed (window %u ms); M3 moves once the sensor confirms",
             (unsigned)win_ms);
    return true;
}

void commission_teach_abort(void)
{
    /* State first, so a T17 tick already past its state read is the only one
     * that can still act -- and a leg it starts is ended by T2's own timer. */
    portENTER_CRITICAL(&s_mux);
    s_st.state      = TEACH_IDLE;
    s_st.run_reason = TEACH_ERR_NONE;
    s_linger        = LINGER_READS;
    s_quiet         = true;             /* our disarm follows: not an orphan */
    s_quiet_from    = xTaskGetTickCount();
    portEXIT_CRITICAL(&s_mux);
    (void)windowpos_teach(WINDOWPOS_DEFAULT_ADDR, false);
    ESP_LOGW(TAG, "teach abandoned by the operator");
    commission_refresh();
}

/**
 * @brief Command the next leg: the opposite way to where T2 believes M3 is.
 *
 * T2 ignores a command to go where it thinks M3 already is
 * (`ch_start_open/close`), so this is the direction that always makes T2
 * energise the relay. If the belief is wrong, the leaf is already at that
 * end: the motor's end switch cuts the drive, nothing moves, and the leg is
 * a no-op that the next leg recovers from. UNKNOWN drives either way; close
 * first, towards the safe end.
 */
static bool start_leg(uint32_t now_ms)
{
    window_state_t w[3];
    t2_get_window_states(w);
    if (w[2] == WIN_MOVING_OPEN || w[2] == WIN_MOVING_CLOSE) {
        teach_fail(TEACH_ERR_M3_BUSY);   /* someone else is driving M3 */
        return false;
    }
    const bool go_open = (w[2] == WIN_CLOSED);

    /* The operator may have aborted since this tick read the state; an abort
     * must not be followed by one more stroke. What is left of the race is the
     * few instructions between here and the send. */
    portENTER_CRITICAL(&s_mux);
    const bool still_running = teach_active(s_st.state);
    portEXIT_CRITICAL(&s_mux);
    if (!still_running) {
        s_phase = LEG_NONE;
        return false;
    }

    window_cmd_t cmd = {};
    cmd.action  = go_open ? CMD_OPEN : CMD_CLOSE;
    cmd.channel = 3u;                      /* M3 */
    cmd.source  = SRC_OPERATOR_MANUAL;     /* an admin asked for this, explicitly */
    if (xQueueSend(Q1, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        teach_fail(TEACH_ERR_NO_START);
        return false;
    }

    s_legs++;
    s_phase        = LEG_STARTING;
    s_leg_ms       = now_ms;
    s_leg_made     = false;
    s_leg_left_end = !s_prev_at_end;   /* part-way already counts as off an end */

    portENTER_CRITICAL(&s_mux);
    s_st.leg         = s_legs;
    s_st.dir_is_open = go_open;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "teach leg %u: driving M3 %s -- THE WINDOW WILL MOVE",
             (unsigned)s_legs, go_open ? "OPEN" : "CLOSED");
    return true;
}

/**
 * @brief A stroke has ended: judge the leg, then start the next or wait.
 * @return false if the run is over (failed).
 */
static bool finish_leg(uint32_t now_ms)
{
    s_phase = LEG_NONE;

    if (s_leg_made) {
        s_noop_legs = 0u;
    } else if (!s_leg_left_end) {
        /* Never left its end sensor: driven into the end it was already at.
         * Expected once, when T2's belief was wrong. Twice in a row means the
         * leaf will not move in EITHER direction. */
        if (++s_noop_legs >= 2u) { teach_fail(TEACH_ERR_NO_MOVE); return false; }
        ESP_LOGW(TAG, "teach leg %u moved nothing -- M3 was already there",
                 (unsigned)s_legs);
    } else {
        /* Left a sensor (or started between them) and stopped short of the
         * next one. T2 drives travel_m3 + margin; if that is shorter than the
         * real traverse, this is where it shows. */
        teach_fail(TEACH_ERR_END_MISSED);
        return false;
    }

    if (s_ends >= 2u) {
        s_settle_ms = now_ms;          /* wait for bit 5 to clear */
        return true;
    }
    if (s_legs >= COMMISSION_TEACH_MAX_LEGS) {
        /* Not reachable with the leg rule above -- after a made leg the next
         * one always moves -- but a run must end somewhere. */
        teach_fail(TEACH_ERR_TIMEOUT);
        return false;
    }
    return start_leg(now_ms);
}

void commission_tick(const windowpos_reading_t *r, uint32_t now_ms)
{
    teach_state_t state;
    bool          cached_armed;
    portENTER_CRITICAL(&s_mux);
    state        = s_st.state;
    cached_armed = s_st.teach_armed;
    if (!teach_active(state) && s_linger != 0u) { s_linger--; }
    portEXIT_CRITICAL(&s_mux);

    hold_check();                         /* the STANDBY hold outlives no session */

    if (!teach_active(state)) {
        /* Self-correction: the stored verdict must follow the live teach bit.
         * A verdict computed once and stored drifts from the device, and bit 5
         * in particular clears a moment AFTER a disarm. Re-judge whenever a
         * real reading disagrees, using THAT reading (T17 calls this before it
         * updates its snapshot on the idle path). Changes are rare, so so is
         * the config read this costs. */
        if (r != NULL && !r->sensor_fault && r->teach_armed != cached_armed) {
            commission_refresh_with(r);
        }
        return;
    }

    /* ---- guards that end any run ------------------------------------- */
    if (r == NULL || r->sensor_fault) { teach_fail(TEACH_ERR_SENSOR); return; }
    if (r->both_end_sensors)          { teach_fail(TEACH_ERR_BOTH_ENDS); return; }
    /* A teach interrupted by safety is not a teach: the leaf may have been
     * commanded somewhere else entirely mid-leg. */
    const EventBits_t eg = xEventGroupGetBits(EG1);
    if (eg & EG1_BIT_MOTOR_ALARM)   { teach_fail(TEACH_ERR_MOTOR_ALARM); return; }
    if (eg & EG1_BIT_WIND_OVERRIDE) { teach_fail(TEACH_ERR_WIND); return; }

    /* ---- end-sensor makes: the events the device captures on --------- */
    const bool at_end = r->at_end_sensor;
    if (s_phase != LEG_NONE) {
        if (!at_end) { s_leg_left_end = true; }
        if (s_have_prev && !s_prev_at_end && at_end && !s_leg_made) {
            s_leg_made = true;               /* one per leg: it moves one way */
            if (s_ends < 2u) { s_ends++; }
            ESP_LOGW(TAG, "teach leg %u: end sensor made (%u of 2), raw %u",
                     (unsigned)s_legs, (unsigned)s_ends, (unsigned)r->raw_adc);
        }
    }
    s_prev_at_end = at_end;
    s_have_prev   = true;

    const bool moving = m3_moving();
    portENTER_CRITICAL(&s_mux);
    s_st.teach_armed = r->teach_armed;       /* live bit 5, for the screen */
    s_st.ends_made   = s_ends;
    if (state == TEACH_TRAVERSING && s_ends >= 2u) {
        s_st.state = TEACH_COMMITTING;       /* the rest is the device's move */
        state = TEACH_COMMITTING;
    }
    portEXIT_CRITICAL(&s_mux);

    /* ---- bit 5: the commit, or the device giving up on its own -------- */
    if (r->teach_armed) {
        s_seen_armed = true;
    } else if (s_seen_armed) {
        if (s_ends < 2u) {
            /* Nobody but this module writes 40007, so a clear before both
             * ends means the device dropped the teach -- a restart does that. */
            teach_fail(TEACH_ERR_DROPPED);
            return;
        }
        /* Committed. The contract orders it commit, persist, clear bit 5, so
         * THIS reading is late enough to judge on, and the config read inside
         * commission_refresh_with() carries the new 40005/40006. */
        commission_refresh_with(r);
        portENTER_CRITICAL(&s_mux);
        s_st.state = TEACH_DONE;
        s_linger   = LINGER_READS;
        const uint16_t lo = s_st.taught_closed, hi = s_st.taught_open;
        portEXIT_CRITICAL(&s_mux);
        s_phase = LEG_NONE;
        ESP_LOGW(TAG, "teach COMMITTED after %u legs: closed=%u open=%u%s",
                 (unsigned)s_legs, (unsigned)lo, (unsigned)hi,
                 moving ? " (M3 still finishing its stroke)" : "");
        return;
    }

    /* ---- arming: nothing moves until the device confirms -------------- */
    if (state == TEACH_ARMING) {
        if (!s_stamped) { s_arm_ms = now_ms; s_stamped = true; }
        if (s_seen_armed) {
            if (start_leg(now_ms)) {
                portENTER_CRITICAL(&s_mux);
                if (s_st.state == TEACH_ARMING) { s_st.state = TEACH_TRAVERSING; }
                portEXIT_CRITICAL(&s_mux);
            }
        } else if ((uint32_t)(now_ms - s_arm_ms) >= ARM_CONFIRM_MS) {
            teach_fail(TEACH_ERR_DEVICE_WRITE);
        }
        return;
    }

    /* ---- the current leg ---------------------------------------------- */
    switch (s_phase) {
    case LEG_STARTING:
        if (moving) {
            s_phase = LEG_RUNNING;
        } else if ((uint32_t)(now_ms - s_leg_ms) >= LEG_START_MS) {
            teach_fail(TEACH_ERR_NO_START);
        }
        return;

    case LEG_RUNNING: {
        const uint32_t stroke_ms =
            ((uint32_t)configured_travel_s() + MOTOR_TRAVEL_MARGIN_S_DEFAULT) * 1000u;
        if ((uint32_t)(now_ms - s_leg_ms) > stroke_ms * LEG_TIMEOUT_MULT) {
            teach_fail(TEACH_ERR_TIMEOUT);
        } else if (!moving) {
            s_phase   = LEG_STOPPING;
            s_rest_ms = now_ms;
        }
        return;
    }

    case LEG_STOPPING:
        if (moving) {
            /* M3 started again, and not on our command: T6 with a zero dwell,
             * or an operator. The leaf is no longer where this run thinks. */
            teach_fail(TEACH_ERR_M3_BUSY);
        } else if ((uint32_t)(now_ms - s_rest_ms) >= LEG_END_MS) {
            (void)finish_leg(now_ms);
        }
        return;

    case LEG_NONE:
    default:
        /* Only reached with both ends made and the last stroke over. */
        if (moving) {
            teach_fail(TEACH_ERR_M3_BUSY);
        } else if ((uint32_t)(now_ms - s_settle_ms) >= COMMIT_SETTLE_MS) {
            /* Both ends made and read, yet bit 5 stays set: the device refused
             * the pair (contract §6.2: captures closer than 64 counts). Show
             * what it captured before disarming discards it. */
            uint16_t c = 0u, o = 0u;
            if (windowpos_read_captures(WINDOWPOS_DEFAULT_ADDR, &c, &o) == WINDOWPOS_OK) {
                ESP_LOGW(TAG, "teach refused by the sensor: captured closed=%u open=%u",
                         (unsigned)c, (unsigned)o);
            }
            teach_fail(TEACH_ERR_REFUSED);
        }
        return;
    }
}

#endif /* MODBUS_BENCH */
