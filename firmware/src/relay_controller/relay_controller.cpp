/**
 * @file relay_controller.cpp
 * @brief T2 — Relay Controller task implementation (Phase 2).
 *
 * Sole owner of the 6 relay GPIO outputs.  Implements per-channel window
 * state machines, 2 s inter-relay gap enforcement, travel and dwell
 * timers, and the deferred-ISR motor alarm handler (GPIO42 / RRK-3).
 *
 * ## Design references
 *  - firmwareImplementationPlan.md §Phase 2
 *  - design/tasks.md  T2
 *  - design/technicalSoftwareDesignSpecification.md  §5.x T2
 *  - FRS §5.3a (FR-MA01–FR-MA08)
 *
 * ## Phase 2 constraints
 *  - Travel and dwell times are read from NVS at startup using
 *    nvs_cfg_get_i32_or_default().  T4 (Data Manager) is not yet
 *    implemented; NVS holds the canonical values.
 *  - T3 and T6 are stubs; Q1 will be empty until Phase 3+ activates them.
 *    All Q1 infrastructure is in place and functional.
 *  - Manual window commands from LCD / web / MQTT are out of scope (C9).
 *
 * @author  Greenhouse Controller project
 */

#include "relay_controller.h"
#include "../types/app_types.h"
#include "../event_logger/event_logger.h"
#include "gpio_util.h"
#include "nvs_config.h"
#include "cfg_defaults.h"   /* MOTOR_M*_TRAVEL_S_DEFAULT, MOTOR_TRAVEL_MARGIN_S_DEFAULT,
                             * DEF_DWELL_OPEN_S, DEF_DWELL_CLOSE_S */
#include "cfg_limits.h"     /* CFG_MIN_TRAVEL_S, CFG_MAX_TRAVEL_S */

#include <Arduino.h>
#include <esp_log.h>
#include <time.h>

static const char *TAG = "T2";

/* ============================================================
 * Constants
 * ============================================================ */

#define NUM_CHANNELS         3u
#define RELAY_GAP_MS      2000u   /**< Min gap between complementary relays (ms) */
#define ALARM_DEBOUNCE_MS   75u   /**< GPIO42 pin-confirm window (ms) */
#define LOOP_TICK_MS        20u   /**< Main loop tick interval (ms) */
#define CALIB_CHUNK_MS     400u   /**< WDT-friendly chunk size for blocking calib */
#define ALARM_GUARD_MS    60000u  /**< Guard time after alarm clears before re-cal (ms) */
#define ALARM_GUARD_CHUNK_MS 5000u /**< WDT-friendly chunk size for guard wait */

/** NVS keys (namespace NVS_NS_MOTOR = "motor"); max 15 printable chars. */
static const char * const NVS_KEY_TRAVEL[NUM_CHANNELS] = {
    "travel_m1", "travel_m2", "travel_m3"
};
static const char * const NVS_KEY_DWELL_OPEN[NUM_CHANNELS] = {
    "dwell_open_m1", "dwell_open_m2", "dwell_open_m3"
};
static const char * const NVS_KEY_DWELL_CLOSE[NUM_CHANNELS] = {
    "dwell_close_m1", "dwell_close_m2", "dwell_close_m3"
};

static const int32_t TRAVEL_S_DEFAULT[NUM_CHANNELS] = {
    MOTOR_M1_TRAVEL_S_DEFAULT,
    MOTOR_M2_TRAVEL_S_DEFAULT,
    MOTOR_M3_TRAVEL_S_DEFAULT,
};

/* Dwell defaults are pulled from cfg_defaults.h directly: DEF_DWELL_OPEN_S,
 * DEF_DWELL_CLOSE_S.  Used below as the NVS-load fallback in case T2 boots
 * before T4 has seeded the keys. */

/** Relay pin pairs: [channel][0=OPEN relay, 1=CLOSE relay]. */
static const uint8_t RELAY_OPEN_PIN[NUM_CHANNELS]  = {
    PIN_RELAY_M1_OPEN,  PIN_RELAY_M2_OPEN,  PIN_RELAY_M3_OPEN
};
static const uint8_t RELAY_CLOSE_PIN[NUM_CHANNELS] = {
    PIN_RELAY_M1_CLOSE, PIN_RELAY_M2_CLOSE, PIN_RELAY_M3_CLOSE
};

/* ============================================================
 * T2-internal channel state
 *
 * Extends window_state_t with two transient gap states that are internal
 * to T2.  External consumers (T8, T11, T12) see only the public
 * window_state_t; mapping occurs in the reporting path (Phase 8+).
 * ============================================================ */

typedef enum {
    CH_UNKNOWN,         /**< Position not established */
    CH_CLOSED,          /**< Fully closed; dwell timer may be running */
    CH_MOVING_OPEN,     /**< OPEN relay energised; travel timer running */
    CH_OPEN,            /**< Fully open; dwell timer may be running */
    CH_MOVING_CLOSE,    /**< CLOSE relay energised; travel timer running */
    CH_GAP_TO_OPEN,     /**< Both relays off; 2 s gap; will open next */
    CH_GAP_TO_CLOSE,    /**< Both relays off; 2 s gap; will close next */
} ch_state_t;

typedef struct {
    ch_state_t state;
    uint32_t   relay_deadline_ms;  /**< millis() when travel timer expires */
    uint32_t   gap_deadline_ms;    /**< millis() when 2 s gap expires */
    uint32_t   dwell_deadline_ms;  /**< millis() when dwell timer expires */
    uint32_t   travel_ms;          /**< Full energisation duration (ms) */
    uint32_t   dwell_open_ms;      /**< Dwell after reaching OPEN (ms) */
    uint32_t   dwell_close_ms;     /**< Dwell after reaching CLOSED (ms) */
} ch_t;

static ch_t s_ch[NUM_CHANNELS];

/** Spinlock protecting s_ch[].state for cross-task reads (e.g. T11 web status). */
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

/* ============================================================
 * Motor alarm ISR state
 *
 * Written ONLY from the ISR (IRAM_ATTR); read/cleared from the T2 task.
 * volatile ensures no CPU register caching.  bool writes are atomic on
 * ESP32-S3 (single-byte, aligned).
 * ============================================================ */

static volatile bool       s_alarm_edge      = false;
static volatile TickType_t s_alarm_edge_tick = 0;

static void IRAM_ATTR isr_motor_alarm(void)
{
    s_alarm_edge_tick = xTaskGetTickCountFromISR();
    s_alarm_edge      = true;
}

/* ============================================================
 * Relay GPIO helpers
 * ============================================================ */

/** De-energise all 6 relay outputs immediately. */
static void relay_all_off(void)
{
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
        gpio_write(RELAY_OPEN_PIN[ch],  GPIO_LOW);
        gpio_write(RELAY_CLOSE_PIN[ch], GPIO_LOW);
    }
}

/** De-energise both relays on a single channel. */
static inline void relay_ch_off(uint8_t ch)
{
    gpio_write(RELAY_OPEN_PIN[ch],  GPIO_LOW);
    gpio_write(RELAY_CLOSE_PIN[ch], GPIO_LOW);
}

/** Energise the OPEN relay for channel ch (CLOSE relay cleared first). */
static inline void relay_ch_open(uint8_t ch)
{
    gpio_write(RELAY_CLOSE_PIN[ch], GPIO_LOW);   /* belt-and-suspenders */
    gpio_write(RELAY_OPEN_PIN[ch],  GPIO_HIGH);
}

/** Energise the CLOSE relay for channel ch (OPEN relay cleared first). */
static inline void relay_ch_close(uint8_t ch)
{
    gpio_write(RELAY_OPEN_PIN[ch],  GPIO_LOW);   /* belt-and-suspenders */
    gpio_write(RELAY_CLOSE_PIN[ch], GPIO_HIGH);
}

/* ============================================================
 * Logging helpers
 * ============================================================ */

static void log_relay_event(uint8_t ch_1based, ch_state_t state)
{
    log_event_t evt;
    evt.timestamp  = (uint32_t)time(NULL);
    evt.event_type = (uint8_t)LOG_RELAY;
    evt.initiator  = (uint8_t)LOG_BY_SYSTEM;
    evt.channel    = ch_1based;
    evt.param_id   = (uint8_t)LOG_PARAM_NONE;
    evt.value_a    = (int16_t)state;
    evt.value_b    = 0;
    log_post(&evt);
}

static void log_alarm_event(int16_t onset)   /* 1 = onset, 0 = clearance */
{
    log_event_t evt;
    evt.timestamp  = (uint32_t)time(NULL);
    evt.event_type = (uint8_t)LOG_ALARM;
    evt.initiator  = (uint8_t)LOG_BY_SYSTEM;
    evt.channel    = 0;
    evt.param_id   = (uint8_t)LOG_PARAM_NONE;
    evt.value_a    = onset;
    evt.value_b    = 0;
    log_post(&evt);
}

/* ============================================================
 * Channel FSM — start-close / start-open
 * ============================================================ */

/**
 * @brief Initiate a CLOSE move on channel ch.
 *
 * If the channel is currently moving in the OPEN direction a 2 s gap is
 * inserted before the CLOSE relay is energised.  The dwell timer (post-OPEN
 * rest period) is bypassed when source == SRC_T3 (safety commands always
 * execute immediately).
 */
static void ch_start_close(uint8_t ch, uint32_t now_ms, cmd_source_t source)
{
    ch_t *c = &s_ch[ch];

    switch (c->state) {

    case CH_CLOSED:
    case CH_MOVING_CLOSE:
    case CH_GAP_TO_CLOSE:
        return;  /* already at target or already moving there */

    case CH_MOVING_OPEN:
        /* Reversal: de-energise OPEN relay, insert gap, then close. */
        relay_ch_off(ch);
        c->gap_deadline_ms = now_ms + RELAY_GAP_MS;
        c->state = CH_GAP_TO_CLOSE;
        ESP_LOGD(TAG, "CH%u: OPEN→GAP_TO_CLOSE (2 s reversal gap)", ch + 1u);
        return;

    case CH_GAP_TO_OPEN:
        /* Change of mind while still in gap — pivot to close. */
        c->state = CH_GAP_TO_CLOSE;
        ESP_LOGD(TAG, "CH%u: GAP_TO_OPEN pivoted → GAP_TO_CLOSE", ch + 1u);
        return;

    case CH_OPEN:
        /* Check dwell timer; SRC_T3 commands bypass it. */
        if (source != SRC_T3 &&
            (int32_t)(now_ms - c->dwell_deadline_ms) < 0) {
            ESP_LOGD(TAG, "CH%u: CLOSE deferred — dwell %lu ms remaining",
                     ch + 1u, (unsigned long)(c->dwell_deadline_ms - now_ms));
            return;
        }
        break;

    case CH_UNKNOWN:
        /* Close from unknown position (e.g. after motor alarm). */
        break;
    }

    /* Energise the CLOSE relay and start travel timer. */
    relay_ch_close(ch);
    c->relay_deadline_ms = now_ms + c->travel_ms;
    c->state = CH_MOVING_CLOSE;
    log_relay_event((uint8_t)(ch + 1u), CH_MOVING_CLOSE);
    ESP_LOGI(TAG, "CH%u: → MOVING_CLOSE  (travel %lu ms)", ch + 1u, (unsigned long)c->travel_ms);
}

/**
 * @brief Initiate an OPEN move on channel ch.
 *
 * Mirror of ch_start_close().  The dwell timer (post-CLOSED rest period) is
 * bypassed when source == SRC_T3.
 */
static void ch_start_open(uint8_t ch, uint32_t now_ms, cmd_source_t source)
{
    ch_t *c = &s_ch[ch];

    switch (c->state) {

    case CH_OPEN:
    case CH_MOVING_OPEN:
    case CH_GAP_TO_OPEN:
        return;

    case CH_MOVING_CLOSE:
        relay_ch_off(ch);
        c->gap_deadline_ms = now_ms + RELAY_GAP_MS;
        c->state = CH_GAP_TO_OPEN;
        ESP_LOGD(TAG, "CH%u: CLOSE→GAP_TO_OPEN (2 s reversal gap)", ch + 1u);
        return;

    case CH_GAP_TO_CLOSE:
        c->state = CH_GAP_TO_OPEN;
        ESP_LOGD(TAG, "CH%u: GAP_TO_CLOSE pivoted → GAP_TO_OPEN", ch + 1u);
        return;

    case CH_CLOSED:
        if (source != SRC_T3 &&
            (int32_t)(now_ms - c->dwell_deadline_ms) < 0) {
            ESP_LOGD(TAG, "CH%u: OPEN deferred — dwell %lu ms remaining",
                     ch + 1u, (unsigned long)(c->dwell_deadline_ms - now_ms));
            return;
        }
        break;

    case CH_UNKNOWN:
        break;
    }

    relay_ch_open(ch);
    c->relay_deadline_ms = now_ms + c->travel_ms;
    c->state = CH_MOVING_OPEN;
    log_relay_event((uint8_t)(ch + 1u), CH_MOVING_OPEN);
    ESP_LOGI(TAG, "CH%u: → MOVING_OPEN  (travel %lu ms)", ch + 1u, (unsigned long)c->travel_ms);
}

/* ============================================================
 * Channel FSM — periodic update (timer expiry)
 * ============================================================ */

/**
 * @brief Advance the channel ch FSM: check travel and gap timers.
 *
 * Called every LOOP_TICK_MS from the main loop.  De-energises the relay
 * when the travel timer expires, or energises the next-direction relay
 * when the reversal gap expires.
 */
static void ch_update(uint8_t ch, uint32_t now_ms)
{
    ch_t *c = &s_ch[ch];

    switch (c->state) {

    case CH_MOVING_OPEN:
        if ((int32_t)(now_ms - c->relay_deadline_ms) >= 0) {
            relay_ch_off(ch);
            c->state = CH_OPEN;
            c->dwell_deadline_ms = now_ms + c->dwell_open_ms;
            log_relay_event((uint8_t)(ch + 1u), CH_OPEN);
            ESP_LOGI(TAG, "CH%u: OPEN (travel complete)", ch + 1u);
        }
        break;

    case CH_MOVING_CLOSE:
        if ((int32_t)(now_ms - c->relay_deadline_ms) >= 0) {
            relay_ch_off(ch);
            c->state = CH_CLOSED;
            c->dwell_deadline_ms = now_ms + c->dwell_close_ms;
            log_relay_event((uint8_t)(ch + 1u), CH_CLOSED);
            ESP_LOGI(TAG, "CH%u: CLOSED (travel complete)", ch + 1u);
        }
        break;

    case CH_GAP_TO_OPEN:
        if ((int32_t)(now_ms - c->gap_deadline_ms) >= 0) {
            relay_ch_open(ch);
            c->relay_deadline_ms = now_ms + c->travel_ms;
            c->state = CH_MOVING_OPEN;
            log_relay_event((uint8_t)(ch + 1u), CH_MOVING_OPEN);
            ESP_LOGI(TAG, "CH%u: gap ended → MOVING_OPEN", ch + 1u);
        }
        break;

    case CH_GAP_TO_CLOSE:
        if ((int32_t)(now_ms - c->gap_deadline_ms) >= 0) {
            relay_ch_close(ch);
            c->relay_deadline_ms = now_ms + c->travel_ms;
            c->state = CH_MOVING_CLOSE;
            log_relay_event((uint8_t)(ch + 1u), CH_MOVING_CLOSE);
            ESP_LOGI(TAG, "CH%u: gap ended → MOVING_CLOSE", ch + 1u);
        }
        break;

    default:
        break;
    }
}

/* ============================================================
 * Boot / re-calibration (synchronous CLOSE_ALL)
 * ============================================================ */

/* Forward declaration — handle_alarm_onset is defined after calib_close_all
 * but is called from inside it to abort calibration on alarm onset. */
static void handle_alarm_onset(void);

/**
 * @brief Synchronous CLOSE_ALL calibration — blocks until all channels CLOSED.
 *
 * Energises all three CLOSE relays simultaneously.  Each channel's relay is
 * de-energised individually when its own travel timer expires, so faster
 * channels (M1, M2 at 26 s) stop well before the slowest (M3 at 176 s).
 * The function returns when the last channel's deadline has passed.
 *
 * Called at boot (initial calibration) and after motor alarm clearance
 * (re-calibration, FR-MA07).
 *
 * T1 keeps running on Core 1 during the wait and kicks the hardware WDT
 * at its normal 500 ms rate, so no WDT reset is needed here.
 */
static void calib_close_all(void)
{
    /* Entry guard: do not energise relays if the alarm pin is already
     * asserted.  The boot-time check in task_relay_controller catches the
     * common case; this guard closes the narrow race between that check
     * and the first relay_ch_close() call. */
    if (gpio_read(PIN_OPTO_INPUT) == GPIO_LOW) {
        s_alarm_edge = false;
        ESP_LOGW(TAG, "[T2] MOTOR_ALARM at calib_close_all entry — "
                      "calibration skipped; alarm takes priority");
        handle_alarm_onset();
        return;
    }

    ESP_LOGI(TAG, "CLOSE_ALL calibration start");
    xEventGroupSetBits(EG1, EG1_BIT_CALIBRATING);

    uint32_t start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    /* Record per-channel deadlines and find the overall end time. */
    uint32_t deadline_ms[NUM_CHANNELS];
    bool     done[NUM_CHANNELS];
    uint32_t max_deadline_ms = 0;

    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
        relay_ch_close(ch);
        s_ch[ch].state  = CH_MOVING_CLOSE;
        deadline_ms[ch] = start_ms + s_ch[ch].travel_ms;
        done[ch]        = false;
        if (deadline_ms[ch] > max_deadline_ms) {
            max_deadline_ms = deadline_ms[ch];
        }
        log_relay_event((uint8_t)(ch + 1u), CH_MOVING_CLOSE);
        ESP_LOGI(TAG, "CH%u: CLOSE relay energised (deadline %lu ms from boot)",
                 ch + 1u, (unsigned long)deadline_ms[ch]);
    }

    /* Poll every CALIB_CHUNK_MS; de-energise each channel as its timer
     * expires, exit when the last channel is done.
     *
     * Motor alarm check on every chunk: if the alarm asserts while
     * CLOSE relays are energised, abort immediately — relay_all_off()
     * inside handle_alarm_onset() de-energises any still-active channels. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CALIB_CHUNK_MS));
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        if (gpio_read(PIN_OPTO_INPUT) == GPIO_LOW) {
            /* Clear CALIBRATING before onset so status consumers see only
             * MOTOR_ALARM, not CALIBRATING|MOTOR_ALARM simultaneously. */
            xEventGroupClearBits(EG1, EG1_BIT_CALIBRATING);
            s_alarm_edge = false;
            ESP_LOGW(TAG, "[T2] MOTOR_ALARM during CLOSE_ALL calibration — "
                          "calibration aborted; de-energising all relays");
            handle_alarm_onset();
            return;
        }

        for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
            if (!done[ch] && (int32_t)(now_ms - deadline_ms[ch]) >= 0) {
                relay_ch_off(ch);
                s_ch[ch].state             = CH_CLOSED;
                s_ch[ch].dwell_deadline_ms = now_ms + s_ch[ch].dwell_close_ms;
                done[ch]                   = true;
                log_relay_event((uint8_t)(ch + 1u), CH_CLOSED);
                ESP_LOGI(TAG, "CH%u: CLOSED (calibration complete)", ch + 1u);
            }
        }

        if ((int32_t)(now_ms - max_deadline_ms) >= 0) {
            break;
        }
    }

    xEventGroupClearBits(EG1, EG1_BIT_CALIBRATING);
    ESP_LOGI(TAG, "CLOSE_ALL calibration complete — all channels CLOSED");
}

/* ============================================================
 * Motor alarm onset / clearance
 * ============================================================ */

static void handle_alarm_onset(void)
{
    /* Immediately de-energise all 6 relays — highest priority action. */
    relay_all_off();

    /* Mark all channels as position-unknown. */
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
        s_ch[ch].state = CH_UNKNOWN;
    }

    xEventGroupSetBits(EG1, EG1_BIT_MOTOR_ALARM);
    log_alarm_event(1);

    ESP_LOGE(TAG, "MOTOR_ALARM asserted — all relays de-energised, "
                  "all window control suspended (FR-MA01–FR-MA04)");
}

static void handle_alarm_clearance(void)
{
    /* Clear alarm state and log immediately so EG1 readers see the
     * transition (FR-MA06).  Windows remain de-energised and in
     * CH_UNKNOWN; re-calibration follows after the guard time. */
    xEventGroupClearBits(EG1, EG1_BIT_MOTOR_ALARM);
    log_alarm_event(0);

    /* 60 s guard time before re-energising the CLOSE relays.
     *
     * Rationale: the RRK-3 emergency switch cuts motor power, but the
     * motor may still be coasting when the operator manually resets the
     * alarm relay.  Energising the CLOSE relay onto a still-moving motor
     * risks a mechanical impact at the end-switch.  The guard provides a
     * conservative wait for the motor to come to rest.
     *
     * T2 is blocked here; Q1 commands accumulate and are processed
     * (from a fully-CLOSED position) once re-calibration completes.
     *
     * The pin is re-checked on every ALARM_GUARD_CHUNK_MS (5 s) interval
     * so that a re-assertion mid-guard is detected within one chunk, not
     * after the full 60 s.  handle_alarm_onset() is called directly (not
     * delegated to the main loop via s_alarm_edge) to give the same
     * latency as a fresh-boot onset. */
    ESP_LOGI(TAG, "MOTOR_ALARM cleared — 60 s guard before re-calibration "
                  "(motor may still be coasting)");

    for (uint32_t elapsed = 0u; elapsed < ALARM_GUARD_MS;
         elapsed += ALARM_GUARD_CHUNK_MS) {
        vTaskDelay(pdMS_TO_TICKS(ALARM_GUARD_CHUNK_MS));
        const uint32_t elapsed_s = (elapsed + ALARM_GUARD_CHUNK_MS) / 1000u;
        ESP_LOGI(TAG, "  alarm guard: %lu s / %lu s",
                 (unsigned long)elapsed_s,
                 (unsigned long)(ALARM_GUARD_MS / 1000u));

        /* Re-check pin on every chunk boundary so a re-assertion during
         * the guard is acted on within ALARM_GUARD_CHUNK_MS (5 s) rather
         * than after the full 60 s. */
        if (gpio_read(PIN_OPTO_INPUT) == GPIO_LOW) {
            /* Consume the ISR edge flag so the main loop does not issue
             * a duplicate onset when it resumes after we return. */
            s_alarm_edge = false;
            ESP_LOGW(TAG, "MOTOR_ALARM re-asserted at guard +%lu s — "
                          "guard aborted; asserting alarm immediately",
                     (unsigned long)elapsed_s);
            handle_alarm_onset();
            return;
        }
    }

    ESP_LOGI(TAG, "Guard complete — starting CLOSE_ALL re-calibration (FR-MA07)");
    calib_close_all();
    ESP_LOGI(TAG, "Re-calibration complete — resuming AUTOMATIC");
}

/* ============================================================
 * Q1 command processing
 * ============================================================ */

static void process_command(const window_cmd_t *cmd, uint32_t now_ms)
{
    /* Discard all commands while motor alarm is active (FR-MA03). */
    if (xEventGroupGetBits(EG1) & EG1_BIT_MOTOR_ALARM) {
        ESP_LOGW(TAG, "Q1 cmd (action=%d ch=%u src=%d) discarded — MOTOR_ALARM active",
                 (int)cmd->action, cmd->channel, (int)cmd->source);
        return;
    }

    switch (cmd->action) {

    case CMD_CLOSE_ALL:
        ESP_LOGI(TAG, "CMD_CLOSE_ALL from %s", cmd->source == SRC_T3 ? "T3" : "T6");
        for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
            ch_start_close(ch, now_ms, cmd->source);
        }
        break;

    case CMD_OPEN:
        if (cmd->channel >= 1u && cmd->channel <= NUM_CHANNELS) {
            ESP_LOGI(TAG, "CMD_OPEN ch%u from %s",
                     cmd->channel, cmd->source == SRC_T3 ? "T3" : "T6");
            ch_start_open((uint8_t)(cmd->channel - 1u), now_ms, cmd->source);
        } else {
            ESP_LOGW(TAG, "CMD_OPEN: invalid channel %u", cmd->channel);
        }
        break;

    case CMD_CLOSE:
        if (cmd->channel >= 1u && cmd->channel <= NUM_CHANNELS) {
            ESP_LOGI(TAG, "CMD_CLOSE ch%u from %s",
                     cmd->channel, cmd->source == SRC_T3 ? "T3" : "T6");
            ch_start_close((uint8_t)(cmd->channel - 1u), now_ms, cmd->source);
        } else {
            ESP_LOGW(TAG, "CMD_CLOSE: invalid channel %u", cmd->channel);
        }
        break;

    case CMD_RESUME:
        /* T6 signals end of wind override — T2 has no action; T6 will
         * issue new OPEN commands as climate control dictates. */
        ESP_LOGI(TAG, "CMD_RESUME — acknowledged (no T2 action)");
        break;

    default:
        ESP_LOGW(TAG, "Q1: unknown action %d", (int)cmd->action);
        break;
    }
}

/* ============================================================
 * Public state getter (T11 web dashboard)
 * ============================================================ */

void t2_get_window_states(window_state_t out[3])
{
    portENTER_CRITICAL(&s_state_mux);
    for (int i = 0; i < 3; i++) {
        switch (s_ch[i].state) {
            case CH_OPEN:          out[i] = WIN_OPEN;         break;
            case CH_CLOSED:        out[i] = WIN_CLOSED;       break;
            case CH_MOVING_OPEN:
            case CH_GAP_TO_OPEN:   out[i] = WIN_MOVING_OPEN;  break;
            case CH_MOVING_CLOSE:
            case CH_GAP_TO_CLOSE:  out[i] = WIN_MOVING_CLOSE; break;
            default:               out[i] = WIN_UNKNOWN;      break;
        }
    }
    portEXIT_CRITICAL(&s_state_mux);
}

/* ============================================================
 * Task entry point
 * ============================================================ */

void task_relay_controller(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "T2 starting");

    /* ------------------------------------------------------------------
     * 1. Read travel and dwell times from NVS.
     *
     *    nvs_cfg_get_i32_or_default() writes the factory default to NVS
     *    on first boot if the key is absent, so subsequent reads always
     *    return a valid value.
     * ------------------------------------------------------------------ */
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
        int32_t travel_s    = 0;
        int32_t dwell_open  = 0;
        int32_t dwell_close = 0;

        nvs_cfg_get_i32_or_default(NVS_NS_MOTOR, NVS_KEY_TRAVEL[ch],
                                    TRAVEL_S_DEFAULT[ch], &travel_s);
        nvs_cfg_get_i32_or_default(NVS_NS_MOTOR, NVS_KEY_DWELL_OPEN[ch],
                                    DEF_DWELL_OPEN_S,  &dwell_open);
        nvs_cfg_get_i32_or_default(NVS_NS_MOTOR, NVS_KEY_DWELL_CLOSE[ch],
                                    DEF_DWELL_CLOSE_S, &dwell_close);

        /* Clamp travel to valid range — same bounds as cfg_clamp() and the
         * web GUI (single source of truth: cfg_limits.h). */
        if (travel_s < CFG_MIN_TRAVEL_S) travel_s = CFG_MIN_TRAVEL_S;
        if (travel_s > CFG_MAX_TRAVEL_S) travel_s = CFG_MAX_TRAVEL_S;
        if (dwell_open  < 0) dwell_open  = 0;
        if (dwell_close < 0) dwell_close = 0;

        s_ch[ch].travel_ms        = (uint32_t)(travel_s + MOTOR_TRAVEL_MARGIN_S_DEFAULT) * 1000u;
        s_ch[ch].dwell_open_ms    = (uint32_t)dwell_open  * 1000u;
        s_ch[ch].dwell_close_ms   = (uint32_t)dwell_close * 1000u;
        s_ch[ch].state            = CH_UNKNOWN;
        s_ch[ch].relay_deadline_ms = 0u;
        s_ch[ch].gap_deadline_ms   = 0u;
        s_ch[ch].dwell_deadline_ms = 0u;

        ESP_LOGI(TAG, "CH%u: travel=%ld s  dwell_open=%ld s  dwell_close=%ld s",
                 (unsigned)(ch + 1u),
                 (long)travel_s, (long)dwell_open, (long)dwell_close);
    }

    /* ------------------------------------------------------------------
     * 2. Attach GPIO42 interrupt.
     *
     *    CHANGE mode fires on both assert (alarm onset) and deassert
     *    (alarm clearance).  The ISR is NOT suppressed during MOVING
     *    states — a motor reaching the emergency switch during a
     *    T2-commanded move is the primary failure scenario (FR-MA01).
     * ------------------------------------------------------------------ */
    attachInterrupt(PIN_OPTO_INPUT, isr_motor_alarm, CHANGE);
    ESP_LOGI(TAG, "GPIO42 ISR attached (MOTOR_ALARM, CHANGE, not suppressed during MOVING)");

    /* ------------------------------------------------------------------
     * 3. Boot CLOSE_ALL calibration.
     *
     *    Establishes a known CLOSED position on all channels before the
     *    main control loop starts.  All relays are de-energised by
     *    setup() before T2 starts, so no gap delay is required here.
     *
     *    Boot-time alarm check: attachInterrupt uses CHANGE mode, so a
     *    pin that is already LOW at boot never fires the ISR.  Read the
     *    initial state here and skip calibration if the alarm is active —
     *    it is not safe to energise CLOSE relays while the RRK-3 alarm
     *    relay is latched.
     * ------------------------------------------------------------------ */
    if (gpio_read(PIN_OPTO_INPUT) == GPIO_LOW) {
        ESP_LOGW(TAG, "GPIO42 alarm pin already asserted at boot — "
                      "skipping CLOSE_ALL calibration (clear alarm to resume)");
        handle_alarm_onset();
        /* Do not call calib_close_all: relays are already de-energised by
         * setup() and by handle_alarm_onset().  Normal operation resumes
         * from the main loop once the operator clears the alarm. */
    } else {
        calib_close_all();
    }

    /* ------------------------------------------------------------------
     * 4. Main control loop
     * ------------------------------------------------------------------ */
    for (;;) {
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        /* ---- 4a. Motor alarm ISR debounce ---- */
        if (s_alarm_edge) {
            uint32_t edge_ms = (uint32_t)(s_alarm_edge_tick * portTICK_PERIOD_MS);
            if ((now_ms - edge_ms) >= ALARM_DEBOUNCE_MS) {
                /* Consume the edge flag before reading the live pin so a
                 * new edge arriving immediately after is not lost. */
                s_alarm_edge = false;

                /* RRK-3 alarm relay is a normally-open dry contact wired to
                 * J10 (OPTO_INPUT / GND).  The opto-coupler output is active-
                 * low: contact closed (alarm active) → GPIO LOW;
                 * contact open (alarm cleared) → GPIO HIGH (INPUT_PULLUP). */
                bool alarm_signal = (gpio_read(PIN_OPTO_INPUT) == GPIO_LOW);
                bool alarm_active = (xEventGroupGetBits(EG1) & EG1_BIT_MOTOR_ALARM) != 0;

                if (alarm_signal && !alarm_active) {
                    handle_alarm_onset();
                } else if (!alarm_signal && alarm_active) {
                    handle_alarm_clearance();
                    /* now_ms is stale after the blocking calibration. */
                    now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                }
            }
        }

        /* ---- 4b. Update per-channel FSMs (travel / gap timer expiry) ---- */
        for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
            ch_update(ch, now_ms);
        }

        /* ---- 4c. Drain Q1 (non-blocking; process all pending commands) ---- */
        window_cmd_t cmd;
        while (xQueueReceive(Q1, &cmd, 0) == pdTRUE) {
            process_command(&cmd, now_ms);
        }

        vTaskDelay(pdMS_TO_TICKS(LOOP_TICK_MS));
    }
}
