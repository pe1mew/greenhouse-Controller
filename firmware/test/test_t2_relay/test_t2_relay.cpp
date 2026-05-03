/**
 * @file test_t2_relay.cpp
 * @brief On-device integration test suite for T2 — Relay Controller.
 *
 * Flashes a minimal firmware that starts only T2 (relay controller) and runs
 * a structured sequence of Unity test cases to verify correct behaviour before
 * any real motor or wiring is connected.
 *
 * ## How to run
 *
 *   pio test -e test_t2_relay -f test_t2_relay
 *
 * PlatformIO flashes the test firmware, streams serial output, and reports the
 * Unity pass/fail summary.  Open the serial monitor separately if you want to
 * follow the prompts in real time:
 *
 *   pio device monitor -e test_t2_relay
 *
 * ## What you need
 *
 *   - WEMOS LOLIN S3 board with relay PCB attached (relay LED indicators and
 *     audible clicks replace a multimeter for most checks).
 *   - A multimeter in DC voltage mode (3.3 V range) is useful for spot-checks
 *     on relay output pins, but is NOT required for tests to pass.
 *   - A single jumper wire for IT-07 and IT-09 (GPIO42 → GND; see prompts).
 *
 * ## Test sequence
 *
 *   IT-01  NVS factory defaults    — travel times and pre-configured dwell
 *   IT-02  Boot calibration        — all CLOSE relays energise; M1+M2 extinguish
 *                                    at ~26 s (asserted), M3 at ~176 s; all pins
 *                                    LOW after ~185 s
 *   IT-03  CMD_OPEN energises      — M1_OPEN pin goes HIGH within 50 ms
 *   IT-04  Direction reversal      — mid-travel CLOSE: M1_OPEN LOW, 2 s gap,
 *                                    M1_CLOSE HIGH  (audible: CLICK-off … 2 s … CLICK-on)
 *   IT-05  Mutual exclusion        — samples CH1 pins during MOVING_CLOSE;
 *                                    OPEN + CLOSE simultaneously HIGH never occurs
 *   IT-06  CLOSE_ALL (T3)          — open CH2, issue T3 CLOSE_ALL; CH2_OPEN → LOW
 *   IT-07  Alarm onset             — [MANUAL] connect GPIO42 (pin 42) to GND and
 *                                    HOLD STILL for ≥1 s; the opto-coupler uses a
 *                                    75 ms debounce — a bouncy or momentary contact
 *                                    will NOT register; EG1_BIT_MOTOR_ALARM set, all
 *                                    pins LOW (CH1+CH2 MOVING_CLOSE at alarm time)
 *   IT-08  Cmd rejected (alarm)    — CMD_OPEN while alarm active; pin stays LOW
 *   IT-09  Alarm clearance         — [MANUAL] remove jumper and keep wire clear;
 *                                    do NOT reconnect to GND; EG1 bit cleared; 60 s
 *                                    guard elapses; CLOSE relays re-energise; test
 *                                    blocks until re-calibration completes (~240 s)
 *   IT-10  OPEN travel expiry +    — CMD_OPEN CH1; waits for travel timer to
 *          dwell enforcement         expire (CH_OPEN); then verifies T6 CMD_CLOSE
 *                                    is blocked during the 3 s open-dwell window
 *                                    and succeeds after the window expires
 *   IT-11  CLOSE→OPEN reversal gap — from MOVING_CLOSE: CMD_OPEN inserts 2 s gap
 *                                    before OPEN relay energises (mirrors IT-04)
 *   IT-12  CMD_RESUME no-op        — RESUME does not energise or de-energise relays
 *   IT-13  Invalid channel ignored — channel 0 and channel 4 commands are discarded
 *
 * ## Multimeter spot-check points (optional, parallel with serial output)
 *
 *   | Test | Pin to probe vs GND | Expected      |
 *   |------|---------------------|---------------|
 *   | IT-02 ~30 s  | GPIO 13 (M1_CLOSE) | ~0 V  |
 *   | IT-02 ~30 s  | GPIO 21 (M3_CLOSE) | ~3.3 V|
 *   | IT-02 after  | All relay pins     | ~0 V  |
 *   | IT-03        | GPIO 12 (M1_OPEN)  | ~3.3 V|
 *   | IT-04 gap    | GPIO 12 + 13       | Both ~0 V |
 *   | IT-04 post   | GPIO 13 (M1_CLOSE) | ~3.3 V|
 *   | IT-07        | All relay pins     | ~0 V  |
 *   | IT-10 during | GPIO 12 (M1_OPEN)  | ~3.3 V → ~0 V at ~26 s |
 *   | IT-11 gap    | GPIO 13 + 12       | Both ~0 V |
 *   | IT-11 post   | GPIO 12 (M1_OPEN)  | ~3.3 V|
 *
 * ## Expected relay LED / audio behaviour (replaces logic analyser)
 *
 *   IT-02  Three CLOSE LEDs light together; M1+M2 extinguish at ~26 s,
 *          M3 extinguishes at ~176 s.
 *   IT-03  M1 OPEN LED lights.
 *   IT-04  M1 OPEN LED extinguishes → ~2 s silence → M1 CLOSE LED lights.
 *          Audible: CLICK-off … 2 s … CLICK-on
 *   IT-06  M2 OPEN LED extinguishes (T3 CLOSE_ALL).
 *   IT-07  All lit LEDs extinguish immediately on alarm.
 *   IT-09  All three CLOSE LEDs light again for re-calibration.
 *   IT-10  M1 OPEN LED lights; extinguishes at ~26 s (travel complete);
 *          ~3.5 s later M1 CLOSE LED lights (dwell expired).
 *   IT-11  M1 CLOSE LED extinguishes → ~2 s silence → M1 OPEN LED lights.
 *          Audible: CLICK-off … 2 s … CLICK-on
 *
 * ## Pre-test NVS configuration
 *
 *   setup() writes motor/dwell_open_m1 = 3 s to NVS before spawning T2.
 *   This exercises the dwell-enforcement path in IT-10.  All other motor
 *   parameters use compile-time factory defaults.
 *
 * ## Expected total duration
 *
 *   ~185 s  IT-02 boot calibration
 *   ~ 15 s  IT-07 interactive alarm onset  (user window; jumper to GND)
 *   ~  1 s  IT-07 debounce + relay de-energise
 *   ~ 15 s  IT-09 interactive alarm clearance (user window; remove jumper)
 *   ~ 60 s  IT-09 guard time (motor coast-down; see jitter open issue)
 *   ~176 s  IT-09 re-calibration (CH3 travel; CH1+CH2 done at ~86 s)
 *   ~ 30 s  IT-10 OPEN travel expiry + dwell (26.5 s travel + 3.5 s dwell)
 *   ~  3 s  IT-11 CLOSE→OPEN gap
 *   <  3 s  remaining automated tests
 *   -------
 *   ~490 s  nominal  (user responds within 5 s of each prompt)
 *   ~560 s  worst case (user takes full 15 s for both prompts, no jitter)
 *   + 60 s  per additional alarm jitter event during IT-09 guard
 *
 *   Logic analyser capture window: 600 s (10 min) from power-on.
 *   If the alarm jitters during the IT-09 guard, add 60 s per jitter event.
 *
 * @author  Greenhouse Controller project
 */

#include <unity.h>
#include <Arduino.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <time.h>

#include "types/app_types.h"
#include "relay_controller/relay_controller.h"
#include "event_logger/event_logger.h"
#include "gpio_util.h"
#include "nvs_config.h"

static const char *TAG = "T2TEST";

/* ============================================================
 * Minimal heartbeat task (replaces T1 from main.cpp)
 *
 * Toggles PIN_HB_LED at 1 Hz so the board's heartbeat LED blinks during
 * the test run.  The hardware WDT is disabled for the test firmware, so
 * esp_task_wdt_add() is intentionally omitted here.
 * ============================================================ */

static void task_test_heartbeat(void *pvParameters)
{
    (void)pvParameters;
    for (;;) {
        gpio_toggle(PIN_HB_LED);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ============================================================
 * RTOS handle definitions
 *
 * Normally created and defined in firmware/src/main.cpp.
 * Redefined here for the isolated test build (build_src_filter
 * excludes main.cpp from compilation in the test_t2_relay env).
 * ============================================================ */

QueueHandle_t Q1;
QueueHandle_t Q2;
QueueHandle_t Q3;
QueueHandle_t Q4;
QueueHandle_t Q5;
QueueHandle_t Q6;

TaskHandle_t task_t1  = NULL;
TaskHandle_t task_t2  = NULL;
TaskHandle_t task_t3  = NULL;
TaskHandle_t task_t4  = NULL;
TaskHandle_t task_t5  = NULL;
TaskHandle_t task_t6  = NULL;
TaskHandle_t task_t7  = NULL;
TaskHandle_t task_t8  = NULL;
TaskHandle_t task_t9  = NULL;
TaskHandle_t task_t10 = NULL;
TaskHandle_t task_t11 = NULL;
TaskHandle_t task_t12 = NULL;

EventGroupHandle_t EG1;

SemaphoreHandle_t MX1;
SemaphoreHandle_t MX2;
SemaphoreHandle_t MX3;
SemaphoreHandle_t MX4;
SemaphoreHandle_t MX5;

/* ============================================================
 * Test helpers
 * ============================================================ */

/** Send a window command to T2 via Q1. */
static void send_cmd(cmd_action_t action, uint8_t channel, cmd_source_t source)
{
    window_cmd_t cmd;
    cmd.action  = action;
    cmd.channel = channel;
    cmd.source  = source;
    xQueueSend(Q1, &cmd, portMAX_DELAY);
}

/** True if EG1_BIT_MOTOR_ALARM is currently set. */
static bool alarm_active(void)
{
    return (xEventGroupGetBits(EG1) & EG1_BIT_MOTOR_ALARM) != 0;
}

/** True when all six relay output pins are LOW (all relays de-energised). */
static bool all_relays_low(void)
{
    return (gpio_read(PIN_RELAY_M1_OPEN)  == GPIO_LOW) &&
           (gpio_read(PIN_RELAY_M1_CLOSE) == GPIO_LOW) &&
           (gpio_read(PIN_RELAY_M2_OPEN)  == GPIO_LOW) &&
           (gpio_read(PIN_RELAY_M2_CLOSE) == GPIO_LOW) &&
           (gpio_read(PIN_RELAY_M3_OPEN)  == GPIO_LOW) &&
           (gpio_read(PIN_RELAY_M3_CLOSE) == GPIO_LOW);
}

/* ============================================================
 * IT-01 — NVS factory defaults
 *
 * Verifies that T2's startup NVS read (nvs_cfg_get_i32_or_default)
 * wrote and returned the compile-time factory defaults for travel times,
 * and that the dwell_open_m1 = 3 s written by setup() was preserved.
 * ============================================================ */

void test_nvs_factory_defaults(void)
{
    int32_t val = 0;

    nvs_cfg_get_i32(NVS_NS_MOTOR, "travel_m1", &val);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(
        MOTOR_M1_TRAVEL_S_DEFAULT, val,
        "NVS motor/travel_m1 should equal MOTOR_M1_TRAVEL_S_DEFAULT (21 s)");

    nvs_cfg_get_i32(NVS_NS_MOTOR, "travel_m2", &val);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(
        MOTOR_M2_TRAVEL_S_DEFAULT, val,
        "NVS motor/travel_m2 should equal MOTOR_M2_TRAVEL_S_DEFAULT (21 s)");

    nvs_cfg_get_i32(NVS_NS_MOTOR, "travel_m3", &val);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(
        MOTOR_M3_TRAVEL_S_DEFAULT, val,
        "NVS motor/travel_m3 should equal MOTOR_M3_TRAVEL_S_DEFAULT (171 s)");

    nvs_cfg_get_i32(NVS_NS_MOTOR, "dwell_open_m1", &val);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(3, val,
        "NVS motor/dwell_open_m1 should equal 3 s (pre-written by test setup)");
}

/* ============================================================
 * IT-02 — Boot calibration with per-channel stop assertion
 *
 * T2 runs a synchronous CLOSE_ALL at startup.  Travel times:
 *   M1, M2: (21 + 5) s = 26 s
 *   M3:    (171 + 5) s = 176 s
 *
 * Intermediate assertion at ~30 s (slice 3):
 *   M1_CLOSE and M2_CLOSE must be LOW  (travel timers expired at ~26 s)
 *   M3_CLOSE must be HIGH              (176 s timer still running)
 *   All OPEN pins must be LOW throughout
 *
 * Final assertion at ~185 s: all six relay pins LOW.
 *
 * Observable on relay board: all three CLOSE LEDs light simultaneously
 * at startup; M1+M2 LEDs extinguish at ~26 s; M3 LED extinguishes at ~176 s.
 * ============================================================ */

void test_boot_calibration(void)
{
    ESP_LOGI(TAG, "IT-02: waiting for boot calibration (~176 s)...");
    ESP_LOGI(TAG, "  Watch CLOSE LEDs: M1+M2 extinguish at ~26 s, M3 at ~176 s");

    const uint32_t wait_ms     = 185000u;
    const uint32_t slice_ms    = 10000u;
    const uint32_t log_every_n = 3u;
    uint32_t slices = wait_ms / slice_ms;   /* 18 slices = 180 s */

    for (uint32_t i = 0; i < slices; i++) {
        vTaskDelay(pdMS_TO_TICKS(slice_ms));

        if ((i + 1u) % log_every_n == 0u) {
            ESP_LOGI(TAG, "  ...%lu s elapsed", (unsigned long)((i + 1u) * slice_ms / 1000u));
        }

        /* At slice 3 (30 s elapsed): M1 and M2 travel timers must have
         * expired at 26 s; M3 (176 s timer) must still be running.
         * This asserts per-channel independent stopping behaviour. */
        if (i + 1u == 3u) {
            ESP_LOGI(TAG, "IT-02: intermediate check at ~30 s");

            TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW,  (int)gpio_read(PIN_RELAY_M1_CLOSE),
                "M1_CLOSE must be LOW at ~30 s (M1 travel = 26 s, already complete)");
            TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW,  (int)gpio_read(PIN_RELAY_M2_CLOSE),
                "M2_CLOSE must be LOW at ~30 s (M2 travel = 26 s, already complete)");
            TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_HIGH, (int)gpio_read(PIN_RELAY_M3_CLOSE),
                "M3_CLOSE must be HIGH at ~30 s (M3 travel = 176 s, still running)");
            TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW,  (int)gpio_read(PIN_RELAY_M1_OPEN),
                "M1_OPEN must be LOW throughout calibration");
            TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW,  (int)gpio_read(PIN_RELAY_M2_OPEN),
                "M2_OPEN must be LOW throughout calibration");
            TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW,  (int)gpio_read(PIN_RELAY_M3_OPEN),
                "M3_OPEN must be LOW throughout calibration");

            ESP_LOGI(TAG, "IT-02: per-channel stop assertion passed — M3 still travelling");
        }
    }
    vTaskDelay(pdMS_TO_TICKS(wait_ms % slice_ms));  /* 5 s remainder */

    ESP_LOGI(TAG, "IT-02: 185 s elapsed — checking all relay pins");

    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M1_OPEN),
                                  "M1_OPEN should be LOW after boot calibration");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M1_CLOSE),
                                  "M1_CLOSE should be LOW after boot calibration");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M2_OPEN),
                                  "M2_OPEN should be LOW after boot calibration");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M2_CLOSE),
                                  "M2_CLOSE should be LOW after boot calibration");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M3_OPEN),
                                  "M3_OPEN should be LOW after boot calibration");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M3_CLOSE),
                                  "M3_CLOSE should be LOW after boot calibration");

    TEST_ASSERT_FALSE_MESSAGE(alarm_active(),
                              "No motor alarm should be active after boot calibration");
}

/* ============================================================
 * IT-03 — CMD_OPEN energises the correct relay
 *
 * Sends CMD_OPEN ch1 via Q1 (SRC_T6).  T2's main loop runs every 20 ms;
 * within 50 ms the OPEN relay for CH1 must be HIGH and the CLOSE relay
 * must remain LOW.
 *
 * Observable on relay board: M1 OPEN LED lights.
 * Multimeter: GPIO 12 (M1_OPEN) vs GND → ~3.3 V.
 * ============================================================ */

void test_open_ch1_energises_relay(void)
{
    send_cmd(CMD_OPEN, 1, SRC_T6);
    vTaskDelay(pdMS_TO_TICKS(50));

    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_HIGH, (int)gpio_read(PIN_RELAY_M1_OPEN),
                                  "M1_OPEN should be HIGH after CMD_OPEN ch1");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M1_CLOSE),
                                  "M1_CLOSE must remain LOW while M1_OPEN is HIGH");
}

/* ============================================================
 * IT-04 — Direction reversal inserts 2 s inter-relay gap
 *
 * CH1 is currently MOVING_OPEN from IT-03.  Sending CMD_CLOSE ch1
 * must:
 *   a) De-energise M1_OPEN immediately (within 50 ms)
 *   b) Hold both relays LOW for 2 s (CH_GAP_TO_CLOSE state)
 *   c) Energise M1_CLOSE after the gap (within 50 ms of gap expiry)
 *
 * Observable on relay board: M1 OPEN LED off → ~2 s silence → M1 CLOSE LED on.
 * Multimeter: GPIO 12 reads ~0 V during gap; GPIO 13 reads ~3.3 V after gap.
 * ============================================================ */

void test_direction_reversal_gap(void)
{
    send_cmd(CMD_CLOSE, 1, SRC_T6);

    /* a) OPEN relay must de-energise within 50 ms */
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M1_OPEN),
                                  "M1_OPEN must de-energise within 50 ms of CMD_CLOSE");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M1_CLOSE),
                                  "M1_CLOSE must still be LOW during the 2 s gap");

    /* b) Both pins remain LOW during the gap (spot-check at ~1.15 s) */
    vTaskDelay(pdMS_TO_TICKS(1100));
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M1_OPEN),
                                  "M1_OPEN must remain LOW during gap (1150 ms check)");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M1_CLOSE),
                                  "M1_CLOSE must remain LOW during gap (1150 ms check)");

    /* c) CLOSE relay must energise after gap (wait another 1.5 s; total ~2.65 s) */
    vTaskDelay(pdMS_TO_TICKS(1500));
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW,  (int)gpio_read(PIN_RELAY_M1_OPEN),
                                  "M1_OPEN must stay LOW after gap");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_HIGH, (int)gpio_read(PIN_RELAY_M1_CLOSE),
                                  "M1_CLOSE must be HIGH after gap expires (~2.65 s)");
}

/* ============================================================
 * IT-05 — Mutual exclusion: OPEN + CLOSE never simultaneously HIGH
 *
 * Samples CH1 relay pins every 20 ms for 200 ms while CH1 is in
 * MOVING_CLOSE (following IT-04).
 * ============================================================ */

void test_no_simultaneous_relays_ch1(void)
{
    for (int i = 0; i < 10; i++) {
        bool open_high  = (gpio_read(PIN_RELAY_M1_OPEN)  == GPIO_HIGH);
        bool close_high = (gpio_read(PIN_RELAY_M1_CLOSE) == GPIO_HIGH);

        if (open_high && close_high) {
            TEST_FAIL_MESSAGE(
                "SAFETY VIOLATION: M1_OPEN and M1_CLOSE simultaneously HIGH");
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
    TEST_PASS();
}

/* ============================================================
 * IT-06 — CLOSE_ALL from T3 immediately overrides active T6 command
 *
 * Opens CH2 via T6, then issues CMD_CLOSE_ALL from T3.  CH2's OPEN
 * relay must de-energise within 50 ms.  CH1 is already MOVING_CLOSE
 * and must not be disturbed.
 *
 * Note: at the start of IT-07 (15 s later), CH1, CH2, and CH3 are all
 * MOVING_CLOSE — M1, M2, and M3 CLOSE relays are HIGH.  IT-07's alarm
 * therefore exercises the "alarm during MOVING" scenario.
 * ============================================================ */

void test_close_all_t3_override(void)
{
    send_cmd(CMD_OPEN, 2, SRC_T6);
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_HIGH, (int)gpio_read(PIN_RELAY_M2_OPEN),
                                  "M2_OPEN should be HIGH before CLOSE_ALL");

    send_cmd(CMD_CLOSE_ALL, 0, SRC_T3);
    vTaskDelay(pdMS_TO_TICKS(50));

    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M2_OPEN),
                                  "M2_OPEN must de-energise within 50 ms of T3 CLOSE_ALL");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_HIGH, (int)gpio_read(PIN_RELAY_M1_CLOSE),
                                  "M1_CLOSE should still be HIGH (already closing)");
}

/* ============================================================
 * IT-07 — Motor alarm onset during MOVING: all relays de-energise
 *
 * *** MANUAL STEP REQUIRED ***
 *
 * Connect GPIO42 (pin 42) to GND using a jumper wire and HOLD IT STILL.
 *
 * The RRK-3 alarm relay is a normally-open dry contact.  Closing it to GND
 * pulls GPIO42 LOW.  GPIO42 is configured INPUT_PULLUP, so idle state is HIGH.
 * T2's IRAM ISR fires on any edge; the task confirms after 75 ms continuous
 * LOW before declaring the alarm.  A bouncy or momentary contact (< 75 ms
 * stable) will NOT be confirmed and the alarm will not fire.
 *
 * At the time of the prompt, CH1 and CH2 are MOVING_CLOSE (CLOSE relays HIGH).
 * This exercises the "alarm during MOVING" scenario — the primary failure
 * case identified in FR-MA01.
 *
 * Expected response: all lit relay LEDs extinguish within ~1 s of making
 * a stable contact.
 *
 * All six relay pins must be LOW; EG1_BIT_MOTOR_ALARM must be set.
 *
 * Keep the jumper in place until IT-09 tells you to remove it.
 * ============================================================ */

void test_alarm_onset_interactive(void)
{
    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "IT-07: *** MANUAL STEP REQUIRED ***");
    ESP_LOGI(TAG, "  Connect GPIO42 (pin 42) to GND using a jumper wire.");
    ESP_LOGI(TAG, "  HOLD STILL for >= 1 s  (75 ms debounce; brief contact ignored).");
    ESP_LOGI(TAG, "  Relay board: lit CLOSE LEDs should extinguish within ~1 s.");
    ESP_LOGI(TAG, "  Keep jumper in place — IT-09 will ask you to remove it.");
    ESP_LOGI(TAG, "  You have 15 seconds. Waiting...");
    ESP_LOGI(TAG, "======================================");

    vTaskDelay(pdMS_TO_TICKS(15000));

    TEST_ASSERT_TRUE_MESSAGE(alarm_active(),
                             "EG1_BIT_MOTOR_ALARM should be set after GPIO42 goes LOW");

    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M1_OPEN),
                                  "M1_OPEN must be LOW during alarm");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M1_CLOSE),
                                  "M1_CLOSE must be LOW during alarm");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M2_OPEN),
                                  "M2_OPEN must be LOW during alarm");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M2_CLOSE),
                                  "M2_CLOSE must be LOW during alarm");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M3_OPEN),
                                  "M3_OPEN must be LOW during alarm");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M3_CLOSE),
                                  "M3_CLOSE must be LOW during alarm");
}

/* ============================================================
 * IT-08 — Commands rejected while alarm is active
 *
 * Sends CMD_OPEN ch3 via T6 while MOTOR_ALARM is active (carry-over from
 * IT-07 — jumper still connected).
 * ============================================================ */

void test_cmd_rejected_during_alarm(void)
{
    TEST_ASSERT_TRUE_MESSAGE(alarm_active(),
                             "Alarm must still be active (IT-07 jumper still connected)");

    send_cmd(CMD_OPEN, 3, SRC_T6);
    vTaskDelay(pdMS_TO_TICKS(50));

    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M3_OPEN),
                                  "M3_OPEN must stay LOW — command rejected during alarm");
}

/* ============================================================
 * IT-09 — Alarm clearance: 60 s guard + re-calibration; blocks until complete
 *
 * *** MANUAL STEP REQUIRED ***
 *
 * Remove the jumper from GPIO42 and keep the wire clear.  Do NOT reconnect
 * it to GND — any reconnection during the guard or re-calibration will
 * restart the whole guard + recal cycle.
 *
 * T2 debounces the rising edge (contact open → GPIO42 HIGH), clears
 * EG1_BIT_MOTOR_ALARM immediately, then observes a 60 s guard before
 * starting CLOSE_ALL re-calibration (FR-MA07):
 *
 *   0–60 s  from alarm clearance : guard (all relays idle)
 *   ~60 s                        : CLOSE relays energise (re-cal starts)
 *   ~86 s                        : M1+M2 CLOSE de-energise (26 s travel)
 *   ~236 s                       : M3 CLOSE de-energises (176 s travel)
 *   ~236 s                       : all pins LOW → T2 resumes AUTOMATIC
 *
 * If the alarm re-asserts during the guard (contact jitter), the guard
 * restarts from zero.  This test handles that case: the 300 s timeout
 * covers the worst case of one additional alarm cycle.
 *
 * This test blocks until all relay pins are LOW to guarantee IT-10+
 * start from a fully CH_CLOSED board.
 * ============================================================ */

void test_alarm_clearance_starts_recal(void)
{
    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "IT-09: *** MANUAL STEP REQUIRED ***");
    ESP_LOGI(TAG, "  Remove the jumper from GPIO42 (pull wire free from GND).");
    ESP_LOGI(TAG, "  Keep wire CLEAR — do NOT reconnect to GND.");
    ESP_LOGI(TAG, "  GPIO42 must stay HIGH for the 60 s guard + ~176 s re-calibration.");
    ESP_LOGI(TAG, "  You have 15 seconds. Waiting...");
    ESP_LOGI(TAG, "======================================");

    vTaskDelay(pdMS_TO_TICKS(15000));

    /* a) Alarm bit must be cleared — GPIO42 is now HIGH (contact open).
     *    EG1_BIT_MOTOR_ALARM is cleared at signal deassertion, before the
     *    60 s guard starts, so it must be clear by the time we check here. */
    TEST_ASSERT_FALSE_MESSAGE(alarm_active(),
                              "EG1_BIT_MOTOR_ALARM should be cleared after GPIO42 "
                              "returns HIGH (jumper removed, contact open)");

    /* b+c) Single completion poll — robust to mid-guard alarm jitter.
     *
     *  Timeout = 60 s guard + 176 s recal + 60 s safety margin = 296 → 300 s.
     *
     *  A second alarm onset during the guard restarts T2's 60 s guard from zero.
     *  The poll tracks that via alarm_active() and logs the phase; the timeout
     *  covers one full extra guard+recal cycle.
     *
     *  recal_started is logged (but not asserted) because recal can start and
     *  complete before the next 1 s poll tick catches the rising edge.
     *
     *  Observable on relay board during this wait:
     *    No LED activity during the 60 s guard.
     *    All 3 CLOSE LEDs light together when the guard expires.
     *    M1+M2 CLOSE LEDs extinguish ~26 s into re-cal.
     *    M3 CLOSE LED extinguishes ~176 s into re-cal. */
    ESP_LOGI(TAG, "IT-09: waiting for 60 s guard + re-calibration to complete (up to 300 s)...");
    ESP_LOGI(TAG, "  No relay activity during the 60 s guard.");
    ESP_LOGI(TAG, "  Watch CLOSE LEDs: all 3 light at guard expiry;");
    ESP_LOGI(TAG, "  M1+M2 extinguish ~26 s later, M3 ~176 s later.");

    bool recal_started = false;
    bool recal_done    = false;

    for (uint32_t t = 0u; t < 300u; t++) {
        vTaskDelay(pdMS_TO_TICKS(1000u));

        bool any_close_high = (gpio_read(PIN_RELAY_M1_CLOSE) == GPIO_HIGH) ||
                              (gpio_read(PIN_RELAY_M2_CLOSE) == GPIO_HIGH) ||
                              (gpio_read(PIN_RELAY_M3_CLOSE) == GPIO_HIGH);

        if (!recal_started && any_close_high) {
            recal_started = true;
            ESP_LOGI(TAG, "  Re-calibration started (+%lu s).", (unsigned long)(t + 1u));
        }

        if (recal_started && all_relays_low() && !alarm_active()) {
            recal_done = true;
            ESP_LOGI(TAG, "  Re-calibration complete (+%lu s).", (unsigned long)(t + 1u));
            break;
        }

        if (t % 30u == 29u) {
            const char *phase = recal_started        ? "re-cal running" :
                                alarm_active()        ? "alarm active — guard will restart" :
                                                        "guard running";
            ESP_LOGI(TAG, "  ...%s (%lu s elapsed)", phase, (unsigned long)(t + 1u));
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(recal_done,
        "Re-calibration must complete within 300 s "
        "(60 s guard + 176 s recal + margin). "
        "Ensure jumper is removed and GPIO42 stays HIGH.");
    TEST_ASSERT_FALSE_MESSAGE(alarm_active(),
        "EG1_BIT_MOTOR_ALARM must remain cleared after re-calibration");

    ESP_LOGI(TAG, "IT-09: complete. Board in CH_CLOSED. Proceeding to IT-10.");
}

/* ============================================================
 * IT-10 — OPEN travel timer expiry (CH_OPEN) + dwell enforcement
 *
 * setup() pre-wrote motor/dwell_open_m1 = 3 s to NVS before T2 started,
 * so s_ch[0].dwell_open_ms = 3000 ms.  This test exercises two paths:
 *
 *  a) OPEN travel expiry: CMD_OPEN CH1 → MOVING_OPEN → travel timer
 *     expires at ~26 s → CH_OPEN (M1_OPEN pin de-energises automatically)
 *
 *  b) Dwell enforcement: immediately after OPEN, SRC_T6 CMD_CLOSE is
 *     blocked while the 3 s open-dwell window is unexpired; it succeeds
 *     once the dwell expires.
 *
 * State at entry: all channels CLOSED (confirmed by IT-09).
 *
 * Observable on relay board:
 *   M1 OPEN LED lights → extinguishes at ~26 s (travel complete) →
 *   M1 CLOSE LED lights ~3.5 s later (dwell expired).
 * ============================================================ */

void test_open_travel_expiry_and_dwell(void)
{
    /* ---- a) OPEN travel expiry ---- */
    send_cmd(CMD_OPEN, 1, SRC_T6);
    ESP_LOGI(TAG, "IT-10a: CMD_OPEN ch1 sent — waiting 26.5 s for travel timer expiry");
    ESP_LOGI(TAG, "  Watch M1 OPEN LED: should extinguish at ~26 s.");

    /* Travel = (21 + 5) * 1000 = 26 000 ms.  Wait 26 500 ms (500 ms margin)
     * to be past the expiry regardless of loop-tick jitter. */
    vTaskDelay(pdMS_TO_TICKS(26500u));

    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M1_OPEN),
        "M1_OPEN must be LOW at 26.5 s — travel timer expired, channel reached CH_OPEN");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M1_CLOSE),
        "M1_CLOSE must remain LOW — channel is CH_OPEN, not moving close");

    ESP_LOGI(TAG, "IT-10a: PASS — M1_OPEN LOW, CH_OPEN state reached");

    /* ---- b) Dwell blocking: ~2.5 s of open-dwell remains (started at ~26 s) ---- */
    ESP_LOGI(TAG, "IT-10b: testing T6 CMD_CLOSE while open-dwell is unexpired (~2.5 s left)");
    send_cmd(CMD_CLOSE, 1, SRC_T6);
    vTaskDelay(pdMS_TO_TICKS(50u));

    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M1_CLOSE),
        "M1_CLOSE must remain LOW — SRC_T6 CMD_CLOSE blocked by open-dwell timer");

    /* ---- c) Wait for dwell to expire, then retry ---- */
    /* Dwell started at ~26 s, expires at ~29 s.  Currently at ~26.55 s.
     * Wait 3 s to be safely past the 29 s dwell deadline. */
    ESP_LOGI(TAG, "IT-10c: waiting 3 s for open-dwell to expire...");
    vTaskDelay(pdMS_TO_TICKS(3000u));

    send_cmd(CMD_CLOSE, 1, SRC_T6);
    vTaskDelay(pdMS_TO_TICKS(50u));

    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_HIGH, (int)gpio_read(PIN_RELAY_M1_CLOSE),
        "M1_CLOSE must be HIGH — open-dwell expired, SRC_T6 CMD_CLOSE now accepted");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M1_OPEN),
        "M1_OPEN must remain LOW during MOVING_CLOSE");

    ESP_LOGI(TAG, "IT-10: PASS — CH_OPEN reached, dwell blocked T6, dwell expiry allowed T6");
}

/* ============================================================
 * IT-11 — CLOSE→OPEN reversal gap (CH_GAP_TO_OPEN)
 *
 * CH1 is currently MOVING_CLOSE from IT-10.  Sending CMD_OPEN ch1
 * must:
 *   a) De-energise M1_CLOSE immediately (within 50 ms) → CH_GAP_TO_OPEN
 *   b) Hold both relays LOW for 2 s
 *   c) Energise M1_OPEN after the gap
 *
 * This mirrors IT-04 (OPEN→CLOSE gap) but exercises the symmetric
 * CH_GAP_TO_OPEN code path, which was not covered by IT-04.
 *
 * Observable on relay board: M1 CLOSE LED off → ~2 s silence → M1 OPEN LED on.
 * Audible: CLICK-off … 2 s … CLICK-on
 * ============================================================ */

void test_close_to_open_reversal_gap(void)
{
    /* CH1 is in MOVING_CLOSE from IT-10 */
    send_cmd(CMD_OPEN, 1, SRC_T6);

    /* a) CLOSE relay must de-energise within 50 ms */
    vTaskDelay(pdMS_TO_TICKS(50u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M1_CLOSE),
                                  "M1_CLOSE must de-energise within 50 ms of CMD_OPEN reversal");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M1_OPEN),
                                  "M1_OPEN must still be LOW at start of 2 s gap");

    /* b) Both pins remain LOW during gap (spot-check at ~1.15 s) */
    vTaskDelay(pdMS_TO_TICKS(1100u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M1_CLOSE),
                                  "M1_CLOSE must remain LOW during gap (1150 ms check)");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M1_OPEN),
                                  "M1_OPEN must remain LOW during gap (1150 ms check)");

    /* c) OPEN relay must energise after gap (wait another 1.5 s; total ~2.65 s) */
    vTaskDelay(pdMS_TO_TICKS(1500u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_HIGH, (int)gpio_read(PIN_RELAY_M1_OPEN),
                                  "M1_OPEN must be HIGH after gap expires (~2.65 s)");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW,  (int)gpio_read(PIN_RELAY_M1_CLOSE),
                                  "M1_CLOSE must stay LOW after gap");
}

/* ============================================================
 * IT-12 — CMD_RESUME is a no-op for T2
 *
 * CMD_RESUME signals end of a wind override; T2 acknowledges it but
 * takes no relay action (T6 will issue new OPEN commands as needed).
 * CH1 is currently MOVING_OPEN from IT-11; M1_OPEN must remain HIGH.
 * ============================================================ */

void test_cmd_resume_no_op(void)
{
    /* CH1 in MOVING_OPEN from IT-11; M1_OPEN must be HIGH */
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_HIGH, (int)gpio_read(PIN_RELAY_M1_OPEN),
                                  "Pre-condition: M1_OPEN must be HIGH (CH1 MOVING_OPEN from IT-11)");

    send_cmd(CMD_RESUME, 0, SRC_T6);
    vTaskDelay(pdMS_TO_TICKS(50u));

    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_HIGH, (int)gpio_read(PIN_RELAY_M1_OPEN),
                                  "M1_OPEN must remain HIGH — CMD_RESUME is a no-op for T2");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M1_CLOSE),
                                  "M1_CLOSE must remain LOW after CMD_RESUME");
}

/* ============================================================
 * IT-13 — Invalid channel numbers are silently discarded
 *
 * CMD_OPEN with channel 0 (below range) and channel 4 (above range)
 * must not energise any relay.  CH1 is still MOVING_OPEN from IT-11;
 * M2 and M3 are CLOSED and must not be affected.
 * ============================================================ */

void test_invalid_channel_ignored(void)
{
    send_cmd(CMD_OPEN, 0, SRC_T6);   /* channel 0 — invalid (valid range 1–3) */
    send_cmd(CMD_OPEN, 4, SRC_T6);   /* channel 4 — invalid (valid range 1–3) */
    vTaskDelay(pdMS_TO_TICKS(50u));

    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M2_OPEN),
                                  "M2_OPEN must be LOW — channel 0 command discarded");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M2_CLOSE),
                                  "M2_CLOSE must be LOW — not affected by invalid commands");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M3_OPEN),
                                  "M3_OPEN must be LOW — channel 4 command discarded");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_LOW, (int)gpio_read(PIN_RELAY_M3_CLOSE),
                                  "M3_CLOSE must be LOW — not affected by invalid commands");
}

/* ============================================================
 * Unity setUp / tearDown  (called before / after each test)
 * ============================================================ */

void setUp(void)  {}   /* state carries across tests by design */
void tearDown(void) {}

/* ============================================================
 * Test entry point  (Arduino setup / loop)
 * ============================================================ */

void setup()
{
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);
    delay(200);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " T2 Relay Controller Integration Tests");
    ESP_LOGI(TAG, "========================================");

    /* Disable hardware WDT.
     * Boot calibration + interactive alarm tests exceed the default 5 s timeout.
     * The test firmware is not safety-critical and does not require WDT coverage. */
    esp_task_wdt_deinit();

    /* ------------------------------------------------------------------
     * GPIO initialisation
     * ------------------------------------------------------------------ */
    gpio_set_pin_mode(PIN_HB_LED, GPIO_OUTPUT);
    gpio_write(PIN_HB_LED, GPIO_HIGH);

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
    ESP_LOGI(TAG, "GPIO: HB LED, relay outputs de-energised, GPIO42 input configured");

    /* ------------------------------------------------------------------
     * NVS initialisation
     * ------------------------------------------------------------------ */
    nvs_cfg_status_t nvs_stat = nvs_cfg_init();
    if (nvs_stat == NVS_CFG_ERR_MIGRATION) {
        ESP_LOGI(TAG, "NVS: schema migration — defaults written");
    } else if (nvs_stat != NVS_CFG_OK) {
        ESP_LOGE(TAG, "NVS init failed (%d) — test results may be unreliable", (int)nvs_stat);
    } else {
        ESP_LOGI(TAG, "NVS: OK");
    }

    /* ------------------------------------------------------------------
     * Pre-configure a non-zero open-dwell for CH1 so IT-10 can exercise
     * the dwell-enforcement code path.  T2 reads NVS at startup, so this
     * write must happen before task_relay_controller is spawned.
     * ------------------------------------------------------------------ */
    nvs_cfg_set_i32(NVS_NS_MOTOR, "dwell_open_m1", 3);
    ESP_LOGI(TAG, "NVS: motor/dwell_open_m1 = 3 s (for IT-10 dwell test)");

    /* ------------------------------------------------------------------
     * RTOS primitives
     * ------------------------------------------------------------------ */
    Q1  = xQueueCreate(8,  sizeof(window_cmd_t));
    Q2  = xQueueCreate(16, sizeof(key_event_t));
    Q3  = xQueueCreate(32, sizeof(log_event_t));
    Q4  = xQueueCreate(8,  sizeof(config_update_t));
    Q5  = xQueueCreate(1,  sizeof(net_status_t));
    Q6  = xQueueCreate(1,  sizeof(sensor_reading_t));
    EG1 = xEventGroupCreate();
    MX1 = xSemaphoreCreateMutex();
    MX2 = xSemaphoreCreateMutex();
    MX3 = xSemaphoreCreateMutex();
    MX4 = xSemaphoreCreateMutex();
    MX5 = xSemaphoreCreateMutex();
    configASSERT(Q1 && Q2 && Q3 && Q4 && Q5 && Q6 &&
                 EG1 && MX1 && MX2 && MX3 && MX4 && MX5);
    ESP_LOGI(TAG, "RTOS primitives created");

    /* ------------------------------------------------------------------
     * Spawn tasks
     * ------------------------------------------------------------------ */
    xTaskCreatePinnedToCore(task_test_heartbeat,
                            "T1_HB",  2048, NULL, 8, &task_t1, 1);
    ESP_LOGI(TAG, "T1 heartbeat spawned (HB LED blinks at 1 Hz; WDT disabled)");

    xTaskCreatePinnedToCore(task_relay_controller,
                            "T2_RLY", 8192, NULL, 7, &task_t2, 1);
    ESP_LOGI(TAG, "T2 spawned — boot calibration starting");
    ESP_LOGI(TAG, "Relay board: watch CLOSE LEDs for all 3 channels");

    /* ------------------------------------------------------------------
     * Unity test run
     * ------------------------------------------------------------------ */
    UNITY_BEGIN();
    RUN_TEST(test_nvs_factory_defaults);          /* IT-01: quick, no relay action     */
    RUN_TEST(test_boot_calibration);              /* IT-02: ~185 s + intermediate check */
    RUN_TEST(test_open_ch1_energises_relay);      /* IT-03: < 1 s                      */
    RUN_TEST(test_direction_reversal_gap);        /* IT-04: ~3 s (OPEN→CLOSE gap)      */
    RUN_TEST(test_no_simultaneous_relays_ch1);    /* IT-05: ~200 ms                    */
    RUN_TEST(test_close_all_t3_override);         /* IT-06: ~200 ms                    */
    RUN_TEST(test_alarm_onset_interactive);       /* IT-07: 15 s manual (during MOVING)*/
    RUN_TEST(test_cmd_rejected_during_alarm);     /* IT-08: < 1 s                      */
    RUN_TEST(test_alarm_clearance_starts_recal);  /* IT-09: 15 s manual + recal poll   */
    RUN_TEST(test_open_travel_expiry_and_dwell);  /* IT-10: ~30 s                      */
    RUN_TEST(test_close_to_open_reversal_gap);    /* IT-11: ~3 s (CLOSE→OPEN gap)      */
    RUN_TEST(test_cmd_resume_no_op);              /* IT-12: < 1 s                      */
    RUN_TEST(test_invalid_channel_ignored);       /* IT-13: < 1 s                      */
    int result = UNITY_END();

    /* ------------------------------------------------------------------
     * End-of-test banner
     *
     * Printed on serial after all Unity results so it is easy to spot
     * in the monitor output and on the logic analyser serial decode.
     *
     * NOTE: M1_OPEN is still HIGH at this point (CH1 in MOVING_OPEN from
     * IT-11; T2 de-energises it ~26 s later when the travel timer expires).
     * Logic analyser capture window: 600 s (10 min) from power-on.
     * Add 60 s per jitter event if alarm bounces during IT-09 guard.
     * ------------------------------------------------------------------ */
    ESP_LOGI(TAG, "########################################");
    if (result == 0) {
        ESP_LOGI(TAG, "#                                      #");
        ESP_LOGI(TAG, "#   ALL TESTS PASSED                   #");
        ESP_LOGI(TAG, "#                                      #");
    } else {
        ESP_LOGI(TAG, "#                                      #");
        ESP_LOGI(TAG, "#   FAILURES: %d                        #", result);
        ESP_LOGI(TAG, "#                                      #");
    }
    ESP_LOGI(TAG, "#   T2 Integration Test Complete       #");
    ESP_LOGI(TAG, "#   Relay board: safe to disconnect    #");
    ESP_LOGI(TAG, "########################################");
}

void loop()
{
    vTaskDelete(NULL);
}
