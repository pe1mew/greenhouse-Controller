/**
 * @file characterise.cpp
 * @brief M3's characterisation run -- see characterise.h and plan §5e.
 *
 * The run is a list of steps, each through the same cycle (ST_REST ->
 * ST_START -> ST_DRIVE -> ST_SETTLE -> ST_READ). plan_next() decides the next
 * step from the phase and its progress; step_done() files what a step
 * measured. Both run in T17, through characterise_tick() and
 * characterise_reading(), and nothing else touches the run once it is
 * published. The web task only starts a run, asks it to end, and reads the
 * published status.
 *
 * The run's buffers (one move's readings, a phase's pulses) live in one
 * context allocated from PSRAM when a run starts and freed when it ends, so an
 * idle unit pays nothing for them.
 */

#include "characterise.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "cfg_defaults.h"   /* MOTOR_TRAVEL_MARGIN_S_DEFAULT -- T2's stroke is travel + this */
#include "cfg_limits.h"     /* CFG_MIN/MAX_DEADZONE_MM -- the band the run may derive */
#include "m3_char.h"        /* drivers/m3Char: the arithmetic, host-tested */
#include "commission.h"
#include "window_pos_task.h"
#include "../data_manager/data_manager.h"
#include "../event_logger/event_logger.h"
#include "../relay_controller/relay_controller.h"
#include "../types/app_types.h"
#include "../types/failfirst_216.h"   /* bench fail-first: bits 1 (guards) and 2 (no 4b) */

#ifndef FIRMWARE_VERSION
#  define FIRMWARE_VERSION "unstamped"
#endif

static const char *TAG = "CHARACTERISE";

/* ---- the cycle ------------------------------------------------------------ */
/** T2 drains Q1 every 20 ms, so a drive that has not begun by now is not coming. */
#define CH_START_MS         3000u
/** A pulse's drive ends within its width plus this. */
#define CH_PULSE_SLACK_MS   3000u
/** A target's drive ends within this multiple of T2's own stroke time. */
#define CH_TARGET_MULT      2u
/** T17's own settle rule (window_pos_task.cpp, SETTLE_BASE_MS): the leaf rests
 *  once this, plus one measurement window, has passed since the drive ended. */
#define CH_SETTLE_BASE_MS   1000u
/** Readings averaged where the leaf rests. */
#define CH_READS            2u
/** Prompt readings come every T17 idle tick (500 ms). Allow this much per
 *  reading still owed before deciding the sensor has gone. */
#define CH_READ_GRACE_MS    1500u

/* ---- the phases (plan §5e, "The run") --------------------------------------- */
#define CH_MAX_SAMPLES      192u    /* one long move's readings: ~45 on the rig, ~90 on 5C88 */
#define CH_MAX_PULSES       160u    /* phase 3's pulses, both directions */
#define CH_REV_PULSES       16u     /* phase 2: C C O O, four times */
#define CH_REPS             5u      /* phase 3: pulses per width */
#define CH_NOISE_READS      6u      /* phase 3: readings at rest; the first is left out */
#define CH_WP02_N           10u
#define CH_CHECK_N          8u
#define CH_MIN_PULSE_MS     10u
#define CH_MAX_PULSE_MS     30000u
/** The leaf stays between 10 % and 90 % during the pulse phases: nothing is
 *  pulsed against an end (the 2026-10-02 lesson). */
#define CH_KEEP_LO_X10      100
#define CH_KEEP_HI_X10      900
/** Widths as a share of the window, 0.1 %. */
#define CH_REV_D_X10        30      /* phase 2: 3 % */
#define CH_W0_D_X10         10      /* phase 3: the search starts at 1 % */
#define CH_TAKEUP_EXTRA_X10 5       /* phase 3: a take-up moves the reversal loss + 0.5 % */
/** About how many starts a whole run makes, and how much M3 runs: the design's
 *  estimate (110-160 starts; ~6 x travel_m3 of running). */
#define CH_STARTS_EST       130u
#define CH_RUN_TRAVELS      6u

/* ---- the log rows: ALARM ch 6, param 253, value_a = what --------------------- */
#define LOG_CH_WPOS_EVENT   6u      /* as window_pos_task.cpp */
enum {
    LI_START = 1, LI_END = 2,
    LI_SPEED_OPEN = 10, LI_SPEED_CLOSE = 11, LI_READ_MS = 12,
    LI_LOSS_OPEN = 20, LI_LOSS_CLOSE = 21,
    LI_DEAD_OPEN = 30, LI_DEAD_CLOSE = 31,
    LI_F2_MS_OPEN = 32, LI_F2_MS_CLOSE = 33, LI_F2_MM_OPEN = 34, LI_F2_MM_CLOSE = 35,
    LI_SPREAD = 40, LI_HYST = 41, LI_RMS = 42,
    LI_B0 = 50, LI_ROUNDS = 51, LI_WORST = 52,
    LI_BAND = 60, LI_IN_FORCE = 61,
};

typedef enum { ST_REST = 0, ST_START, ST_DRIVE, ST_SETTLE, ST_READ } step_t;
typedef enum { K_TARGET = 0, K_PULSE, K_READ } kind_t;
typedef enum {
    P_POSITION = 0,  /* a move to where the next measurement starts */
    P_SPEED,         /* phase 1: a long move, its readings collected */
    P_REV,           /* phase 2: a pulse */
    P_TAKEUP,        /* phase 3: an uncounted take-up pulse */
    P_MINMOVE,       /* phase 3: a counted pulse */
    P_NOISE,         /* phase 3: readings at rest, no command */
    P_APPROACH,      /* phase 4a: to the start of an approach */
    P_WP02,          /* phase 4a: the approach itself */
    P_CHECK,         /* phase 4b: a correction */
} purpose_t;

typedef struct {
    /* the run */
    uint16_t rest_s, window_mm, travel_s, win_ms;
    uint32_t start_ms;
    bool     stamped;
    /* the step in progress */
    step_t    step;
    kind_t    kind;
    purpose_t purpose;
    int16_t   arg;            /* target 0.1 %, or pulse ms signed (+ open) */
    bool      opening;
    bool      takeup;         /* the pulse in progress is an uncounted take-up */
    uint32_t  t_cmd, settle_at;
    uint32_t  epoch0;
    uint16_t  pulse_seq0;
    bool      drove;
    uint8_t   nreads;
    int32_t   sum_mm, sum_pct;
    int32_t   before_mm;
    /* where the leaf rests, as this run last read it */
    int32_t   rest_mm, rest_pct;
    bool      last_open;      /* the direction of the last drive that moved M3 */
    uint32_t  last_end;       /* when it ended */
    uint32_t  epoch;          /* T2's drive counter as the run last knew it */
    uint16_t  starts;
    /* the plan */
    uint8_t   phase, sub, phases_done;
    /* phase 1 */
    m3c_sample_t samples[CH_MAX_SAMPLES];
    uint16_t  n_samples;
    int32_t   move_to_mm;
    uint16_t  speed_x10[2];
    uint16_t  read_ms;
    /* phases 2 and 3 */
    m3c_pulse_t pulses[CH_MAX_PULSES];
    uint16_t  n_pulses;
    bool      dir_before;     /* the direction before the phase's first pulse */
    uint16_t  rev_w[2];
    int32_t   rev_loss_x100[2];
    int32_t   noise[CH_NOISE_READS];
    uint8_t   n_noise;
    uint32_t  noise_x1000, thresh_x1000;
    m3c_search_t search;
    bool      search_open, takeup_due, first_block;
    uint8_t   rep;
    bool      block_all, block_any;
    uint16_t  takeup_w[2];
    uint16_t  floor2_ms[2];
    int32_t   floor2_x100[2];
    int16_t   dead_ms[2];
    /* phase 4a */
    int16_t   wp_rest[CH_WP02_N];
    bool      wp_below[CH_WP02_N];
    uint8_t   wp_n;
    m3c_wp02_t wp;
    int16_t   lead_x100[2];
    /* phase 4b */
    m3c_band_cand_t cand;
    uint16_t  band_mm, band_found;
    uint8_t   raises, round, chk_i;
    bool      chk_d0;
    int32_t   chk_err[CH_CHECK_N];
    int32_t   chk_target_mm;
    int32_t   worst_x10;
} run_t;

static portMUX_TYPE          s_mux = portMUX_INITIALIZER_UNLOCKED;
static characterise_status_t s_st;
static bool                  s_starting;
static volatile bool         s_abort_req;
static run_t                *s_run;     /* T17 only, once published */

/* ---- small helpers -------------------------------------------------------- */

/* 0.01 mm -> 0.1 mm for a log row, ROUNDED half away from zero: the card
 * shows the record's 0.01 mm value to one decimal, and a truncated row (3.48
 * logged as 3.4) would disagree with it. */
static int32_t x100_to_x10(int32_t v)
{
    return (v >= 0 ? v + 5 : v - 5) / 10;
}

static void char_log(int16_t what, int32_t value)
{
    if (value > INT16_MAX) { value = INT16_MAX; }
    if (value < INT16_MIN) { value = INT16_MIN; }
    log_event_t e = {};
    e.timestamp  = (uint32_t)time(NULL);
    e.event_type = (uint8_t)LOG_ALARM;
    e.initiator  = (uint8_t)LOG_BY_SYSTEM;
    e.channel    = LOG_CH_WPOS_EVENT;
    e.param_id   = (uint8_t)LOG_PARAM_WPOS_CHAR;
    e.value_a    = what;
    e.value_b    = (int16_t)value;
    log_post(&e);
}

static uint32_t rest_ms(const run_t *r) { return (uint32_t)r->rest_s * 1000u; }

/** Milliseconds of drive for a distance of `d_x10` (0.1 % of the window). */
static uint16_t ms_for(const run_t *r, int32_t d_x10, bool open)
{
    const uint16_t v = r->speed_x10[open ? 0 : 1];
    if (v == 0u || r->window_mm == 0u) { return 0u; }
    /* d [mm] = d_x10 x window / 1000;  ms = d / (v / 10) x 1000 */
    const double ms = (double)d_x10 * (double)r->window_mm * 10.0 / (double)v;
    if (ms < CH_MIN_PULSE_MS) { return CH_MIN_PULSE_MS; }
    if (ms > CH_MAX_PULSE_MS) { return CH_MAX_PULSE_MS; }
    return (uint16_t)lround(ms);
}

/** The same for a distance in 0.01 mm. */
static uint16_t ms_for_mm(const run_t *r, int32_t d_x100, bool open)
{
    const uint16_t v = r->speed_x10[open ? 0 : 1];
    if (v == 0u) { return 0u; }
    const double ms = (double)d_x100 / 100.0 / ((double)v / 10.0) * 1000.0;
    if (ms < CH_MIN_PULSE_MS) { return CH_MIN_PULSE_MS; }
    if (ms > CH_MAX_PULSE_MS) { return CH_MAX_PULSE_MS; }
    return (uint16_t)lround(ms);
}

static bool m3_moving(void)
{
    window_state_t w[3];
    t2_get_window_states(w);
    return w[2] == WIN_MOVING_OPEN || w[2] == WIN_MOVING_CLOSE;
}

static uint16_t pulse_seq_now(void)
{
    uint16_t seq = 0u;
    (void)t2_get_pulse(0u, NULL, &seq);
    return seq;
}

/** T2's record of the newest pulse, when it is the one after `seq0`. */
static bool pulse_after(uint16_t seq0, t2_pulse_rec_t *out)
{
    uint16_t seq = 0u;
    (void)t2_get_pulse(0u, NULL, &seq);
    if (seq != (uint16_t)(seq0 + 1u)) { return false; }
    const uint16_t kept = (seq < 32u) ? seq : 32u;
    return t2_get_pulse((uint16_t)(kept - 1u), out, NULL);
}

static bool send_q1(cmd_action_t a, int16_t arg)
{
    window_cmd_t cmd = {};
    cmd.action     = a;
    cmd.channel    = 3u;                    /* M3 */
    cmd.source     = SRC_OPERATOR_MANUAL;   /* an admin asked for this, explicitly */
    cmd.target_x10 = arg;
    return xQueueSend(Q1, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

static void publish(run_t *r, uint32_t now)
{
    const uint32_t el = r->stamped ? (now - r->start_ms) / 1000u : 0u;
    uint32_t eta = 0u;
    if (r->starts < CH_STARTS_EST) {
        const uint32_t left = (uint32_t)(CH_STARTS_EST - r->starts);
        const uint32_t per = (r->rest_s > 3u) ? r->rest_s : 3u;
        eta = left * per + (uint32_t)CH_RUN_TRAVELS * r->travel_s * left / CH_STARTS_EST;
    }
    portENTER_CRITICAL(&s_mux);
    s_st.phase     = r->phase;
    s_st.round     = (r->phase == CHAR_PHASE_BAND) ? r->round : 0u;
    s_st.band_mm   = (r->phase == CHAR_PHASE_BAND) ? r->band_mm : 0u;
    s_st.starts    = r->starts;
    s_st.elapsed_s = el;
    s_st.eta_s     = eta;
    portEXIT_CRITICAL(&s_mux);
}

/* ---- the end ------------------------------------------------------------------ */

static void run_end(run_t *r, char_err_t why)
{
    dm_m3_band_override(0u);           /* however the run ends */

    /* The record: this run, and the measured band of the last COMPLETE run --
     * this one if it derived a band, else the one stored before it. */
    m3char_rec_t prev;
    const bool have_prev = dm_m3char_load(&prev);
    m3char_rec_t rec = {};
    rec.version      = M3CHAR_REC_VERSION;
    rec.outcome      = (uint8_t)why;
    rec.phases       = r->phases_done;
    rec.check_rounds = r->round;
    const time_t t   = time(NULL);
    rec.when         = (t > 1600000000) ? (uint32_t)t : 0u;   /* 0 = the clock was not set */
    strncpy(rec.fw, FIRMWARE_VERSION, sizeof(rec.fw) - 1u);
    rec.travel_s     = r->travel_s;
    rec.window_mm    = r->window_mm;
    rec.rest_s       = r->rest_s;
    rec.starts       = r->starts;
    rec.speed_x10[0] = r->speed_x10[0];
    rec.speed_x10[1] = r->speed_x10[1];
    rec.read_ms      = r->read_ms;
    rec.floor2_ms[0] = r->floor2_ms[0];
    rec.floor2_ms[1] = r->floor2_ms[1];
    rec.dead_ms[0]   = r->dead_ms[0];
    rec.dead_ms[1]   = r->dead_ms[1];
    rec.b0_mm        = r->cand.b0_mm;
    rec.rev_loss_x100[0] = r->rev_loss_x100[0];
    rec.rev_loss_x100[1] = r->rev_loss_x100[1];
    rec.floor2_x100[0]   = r->floor2_x100[0];
    rec.floor2_x100[1]   = r->floor2_x100[1];
    rec.noise_x1000      = r->noise_x1000;
    rec.wp02_err_x100    = (int16_t)r->wp.err_x100;
    rec.wp02_spread_x100 = (int16_t)r->wp.spread_x100;
    rec.wp02_hyst_x100   = (int16_t)r->wp.hyst_x100;
    rec.wp02_rms_x100    = (int16_t)r->wp.rms_x100;
    rec.wp02_pass        = r->wp.pass ? 1u : 0u;
    rec.b0_term          = (uint8_t)r->cand.which;
    rec.band_mm          = r->band_found;
    rec.check_worst_x10  = r->worst_x10;
    if (why == CHAR_ERR_NONE && r->band_found != 0u) {
        rec.meas_band_mm = r->band_found;
        rec.meas_when    = rec.when;
        strncpy(rec.meas_fw, FIRMWARE_VERSION, sizeof(rec.meas_fw) - 1u);
    } else if (have_prev) {
        rec.meas_band_mm = prev.meas_band_mm;
        rec.meas_when    = prev.meas_when;
        memcpy(rec.meas_fw, prev.meas_fw, sizeof(rec.meas_fw));
    }
    (void)dm_m3char_save(&rec);

    char_log(LI_BAND, r->band_found);
    dm_m3_dz_t dz;
    dm_m3_deadzone(&dz);
    char_log(LI_IN_FORCE, dz.mm);
    char_log(LI_END, (int32_t)why);
    ESP_LOGW(TAG, "run ended: %s after %u starts, phases 0x%02x, band %u mm (in force %u mm, %s)",
             characterise_err_name(why), (unsigned)r->starts, (unsigned)r->phases_done,
             (unsigned)r->band_found, (unsigned)dz.mm, dm_m3_dz_source_name(dz.source));

    portENTER_CRITICAL(&s_mux);
    s_st.state  = (why == CHAR_ERR_NONE || why == CHAR_ERR_NO_BAND) ? CHAR_DONE : CHAR_FAILED;
    s_st.reason = why;
    s_st.eta_s  = 0u;
    s_st.round  = 0u;
    s_st.band_mm = 0u;
    s_st.starts = r->starts;
    s_run       = NULL;
    portEXIT_CRITICAL(&s_mux);
    heap_caps_free(r);
}

/* ---- commanding ------------------------------------------------------------- */

static bool issue_target(run_t *r, int16_t x10, purpose_t why, uint32_t now)
{
    if (x10 < CH_KEEP_LO_X10 / 2) { x10 = CH_KEEP_LO_X10 / 2; }   /* never an end */
    if (x10 > 1000 - CH_KEEP_LO_X10 / 2) { x10 = 1000 - CH_KEEP_LO_X10 / 2; }
    r->kind = K_TARGET; r->purpose = why; r->arg = x10;
    r->opening = (x10 > r->rest_pct);
    r->before_mm = r->rest_mm;
    r->epoch0 = r->epoch; r->drove = false; r->t_cmd = now;
    if (why == P_SPEED) {
        r->n_samples = 0u;
        r->move_to_mm = (int32_t)x10 * (int32_t)r->window_mm / 100;   /* 0.1 % -> 0.1 mm */
    }
    if (!send_q1(CMD_TARGET, x10)) { run_end(r, CHAR_ERR_NO_START); return false; }
    r->step = ST_START;
    return true;
}

static bool issue_pulse(run_t *r, bool open, uint16_t ms, purpose_t why, bool takeup,
                        uint32_t now)
{
    r->kind = K_PULSE; r->purpose = why; r->arg = (int16_t)(open ? ms : -(int32_t)ms);
    r->opening = open;
    r->takeup = takeup;
    r->before_mm = r->rest_mm;
    r->epoch0 = r->epoch; r->drove = false; r->t_cmd = now;
    r->pulse_seq0 = pulse_seq_now();
    if (!send_q1(CMD_PULSE, r->arg)) { run_end(r, CHAR_ERR_NO_START); return false; }
    r->step = ST_START;
    return true;
}

static void issue_reads(run_t *r, purpose_t why, uint32_t now)
{
    r->kind = K_READ; r->purpose = why; r->drove = false;
    r->settle_at = now; r->nreads = 0u; r->sum_mm = 0; r->sum_pct = 0; r->n_noise = 0u;
    r->step = ST_READ;
}

/* ---- what each step measured -------------------------------------------------- */

static void add_pulse(run_t *r)
{
    const bool takeup = r->takeup;
    if (r->n_pulses >= CH_MAX_PULSES) { return; }
    t2_pulse_rec_t pr = {};
    const bool have = r->drove && pulse_after(r->pulse_seq0, &pr);
    m3c_pulse_t *p = &r->pulses[r->n_pulses++];
    memset(p, 0, sizeof(*p));
    p->req_ms   = (uint16_t)((r->arg < 0) ? -r->arg : r->arg);
    p->width_us = have ? pr.width_us : 0u;
    p->disp_x10 = r->opening ? (r->rest_mm - r->before_mm) : (r->before_mm - r->rest_mm);
    p->opening  = r->opening;
    p->takeup   = takeup;
    p->valid    = have && pr.completed;
}

static void step_done(run_t *r, uint32_t now)
{
    switch (r->purpose) {
    case P_SPEED: {
        const int32_t from = r->before_mm;
        m3c_speed_t sp;
        if (!m3c_speed(r->samples, r->n_samples, from, r->move_to_mm, &sp)) {
            ESP_LOGW(TAG, "too few readings in the speed move (%u)", (unsigned)r->n_samples);
            run_end(r, CHAR_ERR_SENSOR);
            return;
        }
        r->speed_x10[r->opening ? 0 : 1] = sp.speed_x10;
        if (sp.read_ms > r->read_ms) { r->read_ms = sp.read_ms; }
        break;
    }
    case P_REV:
        add_pulse(r);                   /* phase 2's first pulse is its take-up */
        break;
    case P_TAKEUP:
        add_pulse(r);
        r->takeup_due = false;
        break;
    case P_MINMOVE: {
        add_pulse(r);
        const m3c_pulse_t *p = &r->pulses[r->n_pulses - 1u];
        const bool moved = p->valid && (int64_t)p->disp_x10 * 100 > (int64_t)r->thresh_x1000;
        r->block_all = r->block_all && moved;
        r->block_any = r->block_any || moved;
        r->rep++;
        break;
    }
    case P_WP02:
        if (r->wp_n < CH_WP02_N) {
            r->wp_rest[r->wp_n]  = (int16_t)r->rest_pct;
            r->wp_below[r->wp_n] = (r->wp_n % 2u) == 0u;
            r->wp_n++;
        }
        break;
    case P_CHECK:
        if (r->chk_i < CH_CHECK_N) {
            r->chk_err[r->chk_i++] = r->rest_mm - r->chk_target_mm;
        }
        break;
    default:
        break;
    }
    if (r->drove) { r->last_open = r->opening; }
    r->step = ST_REST;
}

/* ---- the plan -------------------------------------------------------------- */

static void phase_begin(run_t *r, uint8_t phase)
{
    r->phase = phase;
    r->sub = 0u;
    ESP_LOGW(TAG, "phase %u", (unsigned)phase);
}

/** Phase 1: to 20 %, then 20 -> 80 % and back, each read for its speed. */
static void plan_speed(run_t *r, uint32_t now)
{
    switch (r->sub++) {
    case 0: (void)issue_target(r, 200, P_POSITION, now); return;
    case 1: (void)issue_target(r, 800, P_SPEED, now); return;
    case 2: (void)issue_target(r, 200, P_SPEED, now); return;
    default: break;
    }
    if (r->speed_x10[0] == 0u || r->speed_x10[1] == 0u) { run_end(r, CHAR_ERR_SENSOR); return; }
    r->phases_done |= M3CHAR_PH_SPEED;
    char_log(LI_SPEED_OPEN, r->speed_x10[0]);
    char_log(LI_SPEED_CLOSE, r->speed_x10[1]);
    char_log(LI_READ_MS, r->read_ms);
    ESP_LOGW(TAG, "speed %u.%u / %u.%u mm/s, T17 reads every %u ms",
             (unsigned)(r->speed_x10[0] / 10u), (unsigned)(r->speed_x10[0] % 10u),
             (unsigned)(r->speed_x10[1] / 10u), (unsigned)(r->speed_x10[1] % 10u),
             (unsigned)r->read_ms);
    phase_begin(r, CHAR_PHASE_REVERSAL);
}

/** Phase 2: to 50 %, a take-up opening, then C C O O four times at 3 %. */
static void plan_reversal(run_t *r, uint32_t now)
{
    if (r->sub == 0u) {
        r->rev_w[0] = ms_for(r, CH_REV_D_X10, true);
        r->rev_w[1] = ms_for(r, CH_REV_D_X10, false);
        r->n_pulses = 0u;
        r->sub++;
        (void)issue_target(r, 500, P_POSITION, now);
        return;
    }
    if (r->sub == 1u) { r->dir_before = r->last_open; }
    if (r->sub <= 1u + CH_REV_PULSES) {
        const uint8_t k = r->sub;
        r->sub++;
        const bool open = (k == 1u) ? true : (((k - 2u) % 4u) >= 2u);   /* take-up, then C C O O */
        (void)issue_pulse(r, open, r->rev_w[open ? 0 : 1], P_REV, k == 1u, now);
        return;
    }
    m3c_mark_reversals(r->pulses, r->n_pulses, r->dir_before);
    for (int d = 0; d < 2; d++) {
        m3c_reversal_t rv;
        const bool ok = m3c_reversal_loss(r->pulses, r->n_pulses, d == 0, r->rev_w[d], &rv);
        r->rev_loss_x100[d] = ok ? rv.loss_x100 : 0;
    }
    r->phases_done |= M3CHAR_PH_REVERSAL;
    char_log(LI_LOSS_OPEN, x100_to_x10(r->rev_loss_x100[0]));
    char_log(LI_LOSS_CLOSE, x100_to_x10(r->rev_loss_x100[1]));
    ESP_LOGW(TAG, "reversal loss %ld / %ld (0.01 mm) at %u / %u ms",
             (long)r->rev_loss_x100[0], (long)r->rev_loss_x100[1],
             (unsigned)r->rev_w[0], (unsigned)r->rev_w[1]);
    phase_begin(r, CHAR_PHASE_MINMOVE);
}

/** Phase 3, one direction: take-up, five pulses at the search's width, repeat. */
static bool plan_search_block(run_t *r, uint32_t now)
{
    const bool open = r->search_open;
    if (r->takeup_due) {
        if (r->rest_pct < CH_KEEP_LO_X10 || r->rest_pct > CH_KEEP_HI_X10) {
            (void)issue_target(r, 500, P_POSITION, now);     /* back to the middle first */
            return true;
        }
        (void)issue_pulse(r, open, r->takeup_w[open ? 0 : 1], P_TAKEUP, true, now);
        return true;
    }
    if (r->rep < CH_REPS) {
        (void)issue_pulse(r, open, r->search.next_ms, P_MINMOVE, false, now);
        return true;
    }
    /* A block is done. */
    if (r->first_block && !r->block_any) { run_end(r, CHAR_ERR_NO_MOVE); return true; }
    m3c_search_result(&r->search, r->block_all, CH_MIN_PULSE_MS);
    r->first_block = false;
    if (r->search.next_ms == 0u) { return false; }   /* this direction is done */
    r->rep = 0u; r->block_all = true; r->block_any = false; r->takeup_due = true;
    return plan_search_block(r, now);
}

static void search_begin(run_t *r, bool open)
{
    r->search_open = open;
    m3c_search_start(&r->search, ms_for(r, CH_W0_D_X10, open));
    r->rep = 0u; r->block_all = true; r->block_any = false;
    r->takeup_due = true; r->first_block = true;
}

/** Phase 3: to 40 %, the noise, the opening search; to 60 %, the closing one. */
static void plan_minmove(run_t *r, uint32_t now)
{
    switch (r->sub) {
    case 0:
        r->n_pulses = 0u;
        for (int d = 0; d < 2; d++) {
            const int32_t loss = (r->rev_loss_x100[d] > 0) ? r->rev_loss_x100[d] : 0;
            const int32_t extra = (int32_t)CH_TAKEUP_EXTRA_X10 * r->window_mm / 10;  /* 0.5 % in 0.01 mm */
            r->takeup_w[d] = ms_for_mm(r, loss + extra, d == 0);
        }
        r->sub = 1u;
        (void)issue_target(r, 400, P_POSITION, now);
        return;
    case 1:
        r->sub = 2u;
        issue_reads(r, P_NOISE, now);
        return;
    case 2:
        r->dir_before = r->last_open;
        search_begin(r, true);
        r->sub = 3u;
        /* fall through */
    case 3:
        if (plan_search_block(r, now)) { return; }
        r->sub = 4u;
        (void)issue_target(r, 600, P_POSITION, now);
        return;
    case 4:
        search_begin(r, false);
        r->sub = 5u;
        /* fall through */
    case 5:
        if (plan_search_block(r, now)) { return; }
        break;
    default:
        break;
    }
    m3c_mark_reversals(r->pulses, r->n_pulses, r->dir_before);
    for (int d = 0; d < 2; d++) {
        m3c_minmove_t mm;
        if (m3c_minmove(r->pulses, r->n_pulses, d == 0, r->thresh_x1000, &mm)) {
            r->floor2_ms[d]   = mm.floor2_ms;
            r->floor2_x100[d] = mm.floor2_disp_x100;
            r->dead_ms[d]     = mm.fit_ok ? mm.dead_ms : 0;
        }
    }
    r->phases_done |= M3CHAR_PH_MINMOVE;
    char_log(LI_DEAD_OPEN, r->dead_ms[0]);
    char_log(LI_DEAD_CLOSE, r->dead_ms[1]);
    char_log(LI_F2_MS_OPEN, r->floor2_ms[0]);
    char_log(LI_F2_MS_CLOSE, r->floor2_ms[1]);
    char_log(LI_F2_MM_OPEN, x100_to_x10(r->floor2_x100[0]));
    char_log(LI_F2_MM_CLOSE, x100_to_x10(r->floor2_x100[1]));
    ESP_LOGW(TAG, "floor 2 %u / %u ms (%ld / %ld x0.01 mm), dead %d / %d ms, %u pulses",
             (unsigned)r->floor2_ms[0], (unsigned)r->floor2_ms[1],
             (long)r->floor2_x100[0], (long)r->floor2_x100[1],
             (int)r->dead_ms[0], (int)r->dead_ms[1], (unsigned)r->n_pulses);
    phase_begin(r, CHAR_PHASE_WP02);
}

/** Phase 4a: ten approaches to 50 %, alternately from 35 % and 65 %. */
static void plan_wp02(run_t *r, uint32_t now)
{
    if (r->sub < 2u * CH_WP02_N) {
        const uint8_t i = (uint8_t)(r->sub / 2u);
        const bool below = (i % 2u) == 0u;
        const bool approach = (r->sub % 2u) == 0u;
        r->sub++;
        if (approach) { (void)issue_target(r, below ? 350 : 650, P_APPROACH, now); }
        else          { (void)issue_target(r, 500, P_WP02, now); }
        return;
    }
    (void)m3c_wp02(r->wp_rest, r->wp_below, r->wp_n, 500, &r->wp);
    t2_m3_lead_t ld;
    t2_get_m3_lead(&ld);
    r->lead_x100[0] = ld.open_x100;
    r->lead_x100[1] = ld.close_x100;
    r->phases_done |= M3CHAR_PH_WP02;
    char_log(LI_SPREAD, r->wp.spread_x100);
    char_log(LI_HYST, r->wp.hyst_x100);
    char_log(LI_RMS, r->wp.rms_x100);
    ESP_LOGW(TAG, "AT-WP02 spread %ld hyst %ld rms %ld (0.01 %%) %s; leads %d / %d",
             (long)r->wp.spread_x100, (long)r->wp.hyst_x100, (long)r->wp.rms_x100,
             r->wp.pass ? "PASS" : "FAIL", (int)r->lead_x100[0], (int)r->lead_x100[1]);
    phase_begin(r, CHAR_PHASE_BAND);
}

/** Phase 4b: corrections of 1.25 x the band from where the leaf rests, S R S R. */
static void plan_band(run_t *r, uint32_t now)
{
    if (r->sub == 0u) {
        m3c_band_in_t in = {};
        in.read_ms        = r->read_ms;
        in.speed_x10[0]   = r->speed_x10[0];
        in.speed_x10[1]   = r->speed_x10[1];
        in.floor2_x100[0] = r->floor2_x100[0];
        in.floor2_x100[1] = r->floor2_x100[1];
        in.rms_x100       = r->wp.rms_x100;
        in.lead_x100[0]   = r->lead_x100[0];
        in.lead_x100[1]   = r->lead_x100[1];
        in.window_mm      = r->window_mm;
        const bool ok = m3c_band_candidate(&in, CFG_MIN_DEADZONE_MM, CFG_MAX_DEADZONE_MM, &r->cand);
        char_log(LI_B0, r->cand.b0_mm);
        ESP_LOGW(TAG, "band candidate %u mm (term %d): floor1 %ld floor2 %ld landing %ld lead %ld",
                 (unsigned)r->cand.b0_mm, (int)r->cand.which, (long)r->cand.floor1_x100,
                 (long)r->cand.floor2_x100, (long)r->cand.landing_x100, (long)r->cand.lead_x100);
        if (!ok) { run_end(r, CHAR_ERR_NO_BAND); return; }
        r->band_mm = r->cand.b0_mm;
        if (FF216_NO4B) {
            /* Fail-first bit 2: b0 unchecked becomes the band. */
            r->band_found = r->band_mm;
            r->phases_done |= M3CHAR_PH_BAND;
            run_end(r, CHAR_ERR_NONE);
            return;
        }
        r->round = 1u; r->raises = 0u; r->chk_i = 0u;
        r->chk_d0 = r->last_open;
        dm_m3_band_override(r->band_mm);
        r->sub = 1u;
    }
    if (r->chk_i < CH_CHECK_N) {
        /* S R S R S R S R from the last drive's direction: d0, !d0, !d0, d0, ... */
        static const bool k_same[CH_CHECK_N] = { true, false, false, true, true, false, false, true };
        const bool open = k_same[r->chk_i] ? r->chk_d0 : !r->chk_d0;
        const int32_t step = (int32_t)m3c_check_step_x10(r->band_mm, r->window_mm);
        int32_t tgt = r->rest_pct + (open ? step : -step);
        if (tgt < CH_KEEP_LO_X10) { tgt = CH_KEEP_LO_X10; }
        if (tgt > CH_KEEP_HI_X10) { tgt = CH_KEEP_HI_X10; }
        r->chk_target_mm = tgt * (int32_t)r->window_mm / 100;
        (void)issue_target(r, (int16_t)tgt, P_CHECK, now);
        return;
    }
    uint16_t next = 0u;
    int32_t worst = 0;
    const m3c_check_t v = m3c_band_check(r->band_mm, r->chk_err, CH_CHECK_N, r->raises,
                                         CFG_MAX_DEADZONE_MM, &next, &worst);
    r->worst_x10 = worst;
    ESP_LOGW(TAG, "band check round %u at %u mm: worst landing %ld (0.1 mm) -> %s",
             (unsigned)r->round, (unsigned)r->band_mm, (long)worst,
             v == M3C_CHECK_PASS ? "pass" : (v == M3C_CHECK_RAISE ? "raise" : "give up"));
    if (v == M3C_CHECK_RAISE) {
        r->raises++; r->round++; r->band_mm = next; r->chk_i = 0u;
        r->chk_d0 = r->last_open;
        dm_m3_band_override(r->band_mm);
        plan_band(r, now);
        return;
    }
    char_log(LI_ROUNDS, r->round);
    char_log(LI_WORST, worst);
    if (v == M3C_CHECK_PASS) {
        r->band_found = r->band_mm;
        r->phases_done |= M3CHAR_PH_BAND;
        run_end(r, CHAR_ERR_NONE);
    } else {
        run_end(r, CHAR_ERR_NO_BAND);
    }
}

static void plan_next(run_t *r, uint32_t now)
{
    switch (r->phase) {
    case CHAR_PHASE_SPEED:    plan_speed(r, now);    break;
    case CHAR_PHASE_REVERSAL: plan_reversal(r, now); break;
    case CHAR_PHASE_MINMOVE:  plan_minmove(r, now);  break;
    case CHAR_PHASE_WP02:     plan_wp02(r, now);     break;
    case CHAR_PHASE_BAND:     plan_band(r, now);     break;
    default:                  run_end(r, CHAR_ERR_SENSOR); break;
    }
}

/* ---- the public face -------------------------------------------------------- */

const char *characterise_err_name(char_err_t e)
{
    static const char *k_names[] = {
        "none", "operator", "m3_busy", "sensor", "both_ends", "wind", "motor_alarm",
        "calibrating", "hold_lost", "no_start", "timeout", "end_reached", "no_move",
        "not_fitted", "not_taught", "teach_running", "bad_rest", "no_memory", "no_band",
    };
    static_assert(sizeof(k_names) / sizeof(k_names[0]) == (size_t)CHAR_ERR_COUNT_,
                  "k_names[] must have one string per char_err_t value, in order");
    return ((unsigned)e < (unsigned)CHAR_ERR_COUNT_) ? k_names[e] : "unknown";
}

void characterise_status(characterise_status_t *out)
{
    if (out == NULL) { return; }
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

void characterise_estimate(uint16_t *starts, uint32_t *run_s)
{
    cfg_shadow_t cfg;
    dm_cfg_snapshot(&cfg);
    if (starts != NULL) { *starts = CH_STARTS_EST; }
    if (run_s != NULL) { *run_s = (uint32_t)CH_RUN_TRAVELS * (uint32_t)cfg.travel_s[2]; }
}

bool characterise_active(void)
{
    portENTER_CRITICAL(&s_mux);
    const bool v = s_starting || s_st.state == CHAR_RUNNING;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

void characterise_abort(void)
{
    if (characterise_active()) {
        s_abort_req = true;
        ESP_LOGW(TAG, "abort requested by the operator");
    }
}

bool characterise_wants_prompt_read(void)
{
    const run_t *r = s_run;              /* T17's own pointer */
    return r != NULL && (r->step == ST_SETTLE || r->step == ST_READ);
}

static void refuse(char_err_t why)
{
    portENTER_CRITICAL(&s_mux);
    s_st = {};
    s_st.state  = CHAR_FAILED;
    s_st.reason = why;
    s_starting  = false;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "run refused: %s", characterise_err_name(why));
}

bool characterise_start(const char *owner_token, uint16_t rest_s, char_err_t *why)
{
    portENTER_CRITICAL(&s_mux);
    const bool busy = s_starting || s_st.state == CHAR_RUNNING;
    if (!busy) { s_starting = true; }
    portEXIT_CRITICAL(&s_mux);
    if (busy) { if (why != NULL) { *why = CHAR_ERR_M3_BUSY; } return false; }

    char_err_t err = CHAR_ERR_NONE;
    commission_status_t cs;
    commission_status(&cs);
    windowpos_reading_t rd = {};
    if (rest_s < CHAR_REST_MIN_S || rest_s > CHAR_REST_MAX_S) {
        err = CHAR_ERR_BAD_REST;
    } else if (!dm_wpos_fitted_m3()) {
        err = CHAR_ERR_NOT_FITTED;
    } else if (cs.verdict != CAL_VALID || cs.window_mm == 0u) {
        err = CHAR_ERR_NOT_TAUGHT;
    } else if (commission_teach_running()) {
        err = CHAR_ERR_TEACH_RUNNING;
    } else if (windowpos_read(WINDOWPOS_DEFAULT_ADDR, &rd) != WINDOWPOS_OK || rd.sensor_fault) {
        err = CHAR_ERR_SENSOR;          /* a FRESH reading gates a window movement */
    } else if (rd.both_end_sensors) {
        err = CHAR_ERR_BOTH_ENDS;
    } else if (m3_moving()) {
        err = CHAR_ERR_M3_BUSY;
    } else {
        const EventBits_t eg = xEventGroupGetBits(EG1);
        if (eg & EG1_BIT_WIND_OVERRIDE)      { err = CHAR_ERR_WIND; }
        else if (eg & EG1_BIT_MOTOR_ALARM)   { err = CHAR_ERR_MOTOR_ALARM; }
        else if (eg & EG1_BIT_CALIBRATING)   { err = CHAR_ERR_CALIBRATING; }
        else {
            dm_m3_pos_t p;
            if (!dm_m3_position(&p)) { err = CHAR_ERR_SENSOR; }   /* the gate must trust it */
        }
    }
    run_t *r = NULL;
    if (err == CHAR_ERR_NONE) {
        r = (run_t *)heap_caps_calloc(1, sizeof(run_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (r == NULL) { r = (run_t *)heap_caps_calloc(1, sizeof(run_t), MALLOC_CAP_8BIT); }
        if (r == NULL) { err = CHAR_ERR_NO_MEMORY; }
    }
    if (err != CHAR_ERR_NONE) {
        refuse(err);
        if (why != NULL) { *why = err; }
        return false;
    }

    cfg_shadow_t cfg;
    dm_cfg_snapshot(&cfg);
    r->rest_s    = rest_s;
    r->window_mm = cs.window_mm;
    r->travel_s  = (uint16_t)cfg.travel_s[2];
    windowpos_derived_t d;
    r->win_ms    = windowpos_task_derived(&d) ? d.window_ms
                                              : windowpos_task_window_ms_for(r->travel_s);
    r->rest_mm   = rd.opening_mm_x10;
    r->rest_pct  = rd.percent_x10;
    window_state_t w[3];
    t2_get_window_states(w);
    r->last_open = (w[2] == WIN_OPEN);
    (void)t2_get_drive(2u, &r->epoch, NULL);
    r->step  = ST_REST;
    r->phase = CHAR_PHASE_SPEED;

    /* STANDBY, the teach's way: held until the run is over and its session has
     * ended. characterise_active() is already true (s_starting), so no release
     * check can drop the hold between here and the publish below. */
    (void)commission_hold_for_run(owner_token);
    if (!dm_get_standby()) {
        heap_caps_free(r);
        refuse(CHAR_ERR_HOLD_LOST);
        if (why != NULL) { *why = CHAR_ERR_HOLD_LOST; }
        return false;
    }

    s_abort_req = false;
    portENTER_CRITICAL(&s_mux);
    s_st = {};
    s_st.state      = CHAR_RUNNING;
    s_st.phase      = CHAR_PHASE_SPEED;
    s_st.rest_s     = rest_s;
    s_st.starts_est = CH_STARTS_EST;
    s_run           = r;                 /* T17 acts on it from here */
    s_starting      = false;
    portEXIT_CRITICAL(&s_mux);
    char_log(LI_START, rest_s);
    ESP_LOGW(TAG, "run started: rest %u s, window %u mm, travel %u s -- M3 WILL MOVE",
             (unsigned)rest_s, (unsigned)r->window_mm, (unsigned)r->travel_s);
    if (why != NULL) { *why = CHAR_ERR_NONE; }
    return true;
}

/* ---- T17 ---------------------------------------------------------------------- */

void characterise_tick(uint32_t now)
{
    run_t *r = s_run;
    if (r == NULL) { return; }
    if (!r->stamped) {
        r->start_ms = now;
        r->last_end = now - rest_ms(r);   /* the first command goes at once */
        r->stamped  = true;
    }

    /* ---- guards that end any run -------------------------------------- */
    if (s_abort_req) { s_abort_req = false; run_end(r, CHAR_ERR_OPERATOR); return; }
    const EventBits_t eg = xEventGroupGetBits(EG1);
    if (!FF216_GUARDS) {                 /* fail-first bit 1: the run carries on */
        if (eg & EG1_BIT_WIND_OVERRIDE)  { run_end(r, CHAR_ERR_WIND); return; }
        if (eg & EG1_BIT_MOTOR_ALARM)    { run_end(r, CHAR_ERR_MOTOR_ALARM); return; }
    }
    /* The hold BEFORE the recalibration: leaving STANDBY releases the hold and
     * starts a recalibration, so both are true at once, and the cause is the
     * hold. Found once T2 woke T17 at every drive start (2.16.0): T17 then woke
     * on the recalibration's first relay and saw CALIBRATING first, so an
     * operator who chose AUTOMATIC was told "a window recalibration ran". A
     * recalibration under a hold still held (an alarm's clearance, an LCD
     * logout) still ends the run as `calibrating`. */
    if (!dm_get_standby())           { run_end(r, CHAR_ERR_HOLD_LOST); return; }
    if (!FF216_GUARDS && (eg & EG1_BIT_CALIBRATING)) { run_end(r, CHAR_ERR_CALIBRATING); return; }

    uint32_t ep = 0u;
    const t2_drive_t drv = t2_get_drive(2u, &ep, NULL);
    const bool moving = m3_moving() || drv != T2_DRIVE_NONE;

    switch (r->step) {
    case ST_REST:
        /* Nothing of ours is driving: anything that moves M3 now is someone
         * else, and the leaf is no longer where the run thinks. */
        if (moving || ep != r->epoch) { run_end(r, CHAR_ERR_M3_BUSY); return; }
        if ((uint32_t)(now - r->last_end) < rest_ms(r)) { break; }
        plan_next(r, now);
        if (s_run == NULL) { return; }   /* the plan ended the run */
        break;

    case ST_START:
        if (ep != r->epoch) {
            r->drove = true;
            r->starts++;
            r->epoch = ep;
            if (drv == T2_DRIVE_OPEN)  { r->opening = true; }
            if (drv == T2_DRIVE_CLOSE) { r->opening = false; }
            r->step = ST_DRIVE;
        } else if ((uint32_t)(now - r->t_cmd) >= CH_START_MS) {
            const int32_t band = (int32_t)dm_m3_deadband_x10();
            const int32_t off  = r->rest_pct - r->arg;
            if (r->kind == K_TARGET && r->purpose != P_CHECK &&
                (off < 0 ? -off : off) <= band) {
                /* T2 judged the leaf already there: a measurement of where it
                 * rests is all this step still needs. */
                r->settle_at = now; r->nreads = 0u; r->sum_mm = 0; r->sum_pct = 0;
                r->step = ST_READ;
            } else {
                run_end(r, CHAR_ERR_NO_START);
                return;
            }
        }
        break;

    case ST_DRIVE: {
        if (ep != r->epoch) { run_end(r, CHAR_ERR_M3_BUSY); return; }   /* a second drive */
        const uint32_t limit = (r->kind == K_PULSE)
            ? (uint32_t)((r->arg < 0) ? -r->arg : r->arg) + CH_PULSE_SLACK_MS
            : ((uint32_t)r->travel_s + MOTOR_TRAVEL_MARGIN_S_DEFAULT) * 1000u * CH_TARGET_MULT;
        if (!moving) {
            r->last_end  = now;
            r->settle_at = now + CH_SETTLE_BASE_MS + r->win_ms;
            r->nreads = 0u; r->sum_mm = 0; r->sum_pct = 0;
            r->step = ST_SETTLE;
        } else if ((uint32_t)(now - r->t_cmd) > limit) {
            run_end(r, CHAR_ERR_TIMEOUT);
            return;
        }
        break;
    }

    case ST_SETTLE:
        if (moving || ep != r->epoch) { run_end(r, CHAR_ERR_M3_BUSY); return; }
        if ((int32_t)(now - r->settle_at) >= 0) { r->step = ST_READ; }
        break;

    case ST_READ: {
        if (moving || ep != r->epoch) { run_end(r, CHAR_ERR_M3_BUSY); return; }
        const uint32_t owed = (r->purpose == P_NOISE) ? CH_NOISE_READS : CH_READS;
        if (!FF216_GUARDS &&
            (int32_t)(now - r->settle_at) > (int32_t)(CH_READ_GRACE_MS * (owed + 2u))) {
            run_end(r, CHAR_ERR_SENSOR);   /* the prompt readings stopped coming */
            return;
        }
        break;
    }
    }
    if (s_run != NULL) { publish(r, now); }
}

void characterise_reading(const windowpos_reading_t *rd, uint32_t now)
{
    run_t *r = s_run;
    if (r == NULL) { return; }
    if (FF216_GUARDS) {
        /* Fail-first bit 1: no reading is no data, and a fault or both ends is
         * ignored -- the run carries on, which is what the harness must see. */
        if (rd == NULL) { return; }
    } else {
        if (rd == NULL || rd->sensor_fault) { run_end(r, CHAR_ERR_SENSOR); return; }
        if (rd->both_end_sensors)          { run_end(r, CHAR_ERR_BOTH_ENDS); return; }
    }
    if ((r->phase == CHAR_PHASE_REVERSAL || r->phase == CHAR_PHASE_MINMOVE) &&
        r->kind == K_PULSE && rd->at_end_sensor) {
        run_end(r, CHAR_ERR_END_REACHED);   /* never pulse against an end */
        return;
    }

    switch (r->step) {
    case ST_DRIVE:
        if (r->purpose == P_SPEED && r->n_samples < CH_MAX_SAMPLES) {
            m3c_sample_t *s = &r->samples[r->n_samples++];
            s->t_ms     = now;
            s->pos_x10  = rd->opening_mm_x10;
            s->rate_x10 = rd->rate_mm_s_x10;
        }
        break;

    case ST_READ:
        if ((int32_t)(now - r->settle_at) < 0) { break; }
        if (r->purpose == P_NOISE) {
            if (r->n_noise < CH_NOISE_READS) { r->noise[r->n_noise++] = rd->opening_mm_x10; }
            if (r->n_noise >= CH_NOISE_READS) {
                r->noise_x1000  = m3c_noise_x1000(&r->noise[1], CH_NOISE_READS - 1u);
                r->thresh_x1000 = m3c_moved_threshold_x1000(r->noise_x1000);
                r->rest_mm  = rd->opening_mm_x10;
                r->rest_pct = rd->percent_x10;
                ESP_LOGW(TAG, "noise %lu (0.001 mm): a pulse moved if more than %lu",
                         (unsigned long)r->noise_x1000, (unsigned long)r->thresh_x1000);
                r->step = ST_REST;
            }
            break;
        }
        r->sum_mm  += rd->opening_mm_x10;
        r->sum_pct += rd->percent_x10;
        if (++r->nreads >= CH_READS) {
            r->rest_mm  = (r->sum_mm  + (int32_t)CH_READS / 2) / (int32_t)CH_READS;
            r->rest_pct = (r->sum_pct + (int32_t)CH_READS / 2) / (int32_t)CH_READS;
            step_done(r, now);
        }
        break;

    default:
        break;
    }
}
