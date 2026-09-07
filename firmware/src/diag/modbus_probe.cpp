/**
 * @file modbus_probe.cpp
 * @brief Concurrency probe for the Modbus bus lock (gh#49) — DEV BUILDS ONLY.
 *
 * See modbus_probe.h for what this measures and how to read the result, and
 * design/addModbusMutex.md §4.3b for why it exists.
 */

#ifdef MODBUS_CONCURRENCY_PROBE

#include "modbus_probe.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "modbus_rtu.h"
#include "../types/app_types.h"   /* task_t5 */

static const char *TAG = "MB_PROBE";

/* ---------------------------------------------------------------------------
 * Probe parameters
 *
 * A successful transaction is ~25-30 ms, so 500 iterations per caller is
 * ~1000 transactions in roughly 30 s of contention. Long enough to make an
 * unsynchronised driver fail loudly; short enough to sit through.
 * --------------------------------------------------------------------------- */
#define PROBE_ITERATIONS   500u

/** FG6485A — address 1, FC03 at 0x0000 (mirrors fg6485a.cpp:61). */
#define PROBE_A_ADDR         1u
#define PROBE_A_REG     0x0000u

/**
 * S200 — address 44, FC04 at 0x0008 (mirrors s200.cpp:78).
 *
 * The production driver reads the full 12-register wind block (0x0008-0x0013);
 * two registers is a subset, kept symmetric with caller A. If the S200 rejects
 * the partial read with an exception, widen this to 12 — a longer frame is
 * *better* here, since more wire time means more overlap to contend over.
 */
#define PROBE_B_ADDR        44u
#define PROBE_B_REG     0x0008u

#define PROBE_REG_COUNT      2u

/* ---------------------------------------------------------------------------
 * Per-caller tally
 * --------------------------------------------------------------------------- */
typedef struct {
    uint8_t  addr;          /**< slave address this caller polls              */
    bool     fc03;          /**< true = FC03 holding, false = FC04 input      */
    uint16_t reg;           /**< start register                              */

    uint32_t ok;            /**< MODBUS_OK                                    */
    uint32_t framing;       /**< MODBUS_ERR_FRAMING — THE FINDING             */
    uint32_t crc;           /**< MODBUS_ERR_CRC                               */
    uint32_t timeout;       /**< MODBUS_ERR_TIMEOUT                           */
    uint32_t busy;          /**< MODBUS_ERR_BUSY — lock contention            */
    uint32_t exception;     /**< MODBUS_ERR_EXCEPTION (slave said no)         */
    uint32_t other;         /**< anything else                                */

    volatile bool done;
} probe_ctx_t;

static probe_ctx_t s_a;
static probe_ctx_t s_b;

/* ---------------------------------------------------------------------------
 * Caller task — hammer the driver, classify every outcome, never assert.
 *
 * Deliberately does no filtering or retrying: the point is to count what the
 * driver actually returns under contention.
 * --------------------------------------------------------------------------- */
static void probe_task(void *arg)
{
    probe_ctx_t *c = (probe_ctx_t *)arg;

    for (uint32_t i = 0; i < PROBE_ITERATIONS; i++) {
        uint16_t regs[PROBE_REG_COUNT] = { 0 };

        modbus_status_t s = c->fc03
            ? modbus_read_holding_registers(c->addr, c->reg, PROBE_REG_COUNT, regs)
            : modbus_read_input_registers(c->addr, c->reg, PROBE_REG_COUNT, regs);

        switch (s) {
            case MODBUS_OK:            c->ok++;        break;
            case MODBUS_ERR_FRAMING:   c->framing++;   break;
            case MODBUS_ERR_CRC:       c->crc++;       break;
            case MODBUS_ERR_TIMEOUT:   c->timeout++;   break;
            case MODBUS_ERR_BUSY:      c->busy++;      break;
            case MODBUS_ERR_EXCEPTION: c->exception++; break;
            default:                   c->other++;     break;
        }

        /* Yield once per transaction — MANDATORY, not politeness.
         *
         * sdkconfig has CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y with a 5 s
         * timeout, and the driver's receive loop (modbus_rtu.cpp:347) spins on
         * uart1_available() without ever yielding. Two priority-4 tasks pinned
         * to core 1 running transactions back to back therefore starve core 1's
         * idle task, and the TWDT panics the board within 5 s — before the
         * probe can report anything.
         *
         * One tick per ~25-30 ms transaction is ample for the idle task and
         * costs essentially no contention: both callers still want the bus
         * continuously. */
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    c->done = true;
    vTaskDelete(NULL);
}

static void log_tally(const char *who, const probe_ctx_t *c)
{
    ESP_LOGI(TAG,
             "%s addr=%u fc=%s | ok=%lu FRAMING=%lu CRC=%lu timeout=%lu "
             "busy=%lu exception=%lu other=%lu",
             who, (unsigned)c->addr, c->fc03 ? "03" : "04",
             (unsigned long)c->ok, (unsigned long)c->framing,
             (unsigned long)c->crc, (unsigned long)c->timeout,
             (unsigned long)c->busy, (unsigned long)c->exception,
             (unsigned long)c->other);
}

/* --------------------------------------------------------------------------- */
void modbus_probe_run(void)
{
    memset(&s_a, 0, sizeof(s_a));
    memset(&s_b, 0, sizeof(s_b));

    s_a.addr = PROBE_A_ADDR; s_a.fc03 = true;  s_a.reg = PROBE_A_REG;
    s_b.addr = PROBE_B_ADDR; s_b.fc03 = false; s_b.reg = PROBE_B_REG;

    ESP_LOGW(TAG, "=== Modbus concurrency probe (gh#49) — DEV BUILD ===");
    ESP_LOGW(TAG, "%u iterations x 2 callers; FG6485A@%u FC03 vs S200@%u FC04",
             (unsigned)PROBE_ITERATIONS, (unsigned)PROBE_A_ADDR, (unsigned)PROBE_B_ADDR);
    ESP_LOGW(TAG, "WITHOUT the mutex expect total failure: 0 clean, all timeouts "
                  "(measured 2026-09-07). A clean run against the unpatched "
                  "driver means the probe is not contending.");

    /* Suspend T5 so its own reads cannot fail during the probe and drive the
     * two-consecutive-failure sensor-fault path. On FDA4 (no windows fitted) a
     * wind fault would actuate nothing, but keeping T5 out makes the log
     * readable and the tally attributable to the two probe tasks alone. */
    if (task_t5 != NULL) {
        vTaskSuspend(task_t5);
        ESP_LOGI(TAG, "T5 suspended for the duration");
    } else {
        ESP_LOGW(TAG, "task_t5 is NULL — running with T5 unmanaged");
    }

    /* Re-initialise the bus before measuring — NOT redundant.
     *
     * main.cpp calls modbus_init() early (~t=1.4 s), but boot then brings up
     * the RTC (I2C), LittleFS, and the SD card over SPI — including an
     * unmount — and the bus does not survive it. Measured on FDA4 2026-09-07:
     * a probe running at t=1.8 s on main.cpp's init got 1000/1000 timeouts,
     * while T5 (which re-inits at its own task entry) polled both sensors
     * perfectly moments later in the same build.
     *
     * This is why main.cpp:1025 says T5's init "reconfirms the driver state".
     * The probe does the same rather than depending on boot ordering.
     * modbus_init() is idempotent and T5 is suspended, so nothing else is on
     * the bus while this runs. */
    modbus_init();
    vTaskDelay(pdMS_TO_TICKS(50));   /* let the transceiver settle */
    ESP_LOGI(TAG, "bus re-initialised for the probe");

    /* Both on core 1 so they genuinely contend for the driver rather than
     * being spread across cores by the scheduler. */
    xTaskCreatePinnedToCore(probe_task, "mbprobe_a", 4096, &s_a, 4, NULL, 1);
    xTaskCreatePinnedToCore(probe_task, "mbprobe_b", 4096, &s_b, 4, NULL, 1);

    /* Generous ceiling: every transaction timing out at ~215 ms would still
     * finish inside this. Poll rather than block so a wedge fails loudly
     * instead of hanging the boot. */
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(300000);
    while ((!s_a.done || !s_b.done) && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    const bool finished = (s_a.done && s_b.done);

    if (task_t5 != NULL) {
        vTaskResume(task_t5);
        ESP_LOGI(TAG, "T5 resumed");
    }

    log_tally("A", &s_a);
    log_tally("B", &s_b);

    if (!finished) {
        ESP_LOGE(TAG, "VERDICT: FAIL — a caller never finished. A missed unlock "
                      "wedges the bus permanently; check the take/give pairing "
                      "in modbus_rtu.cpp.");
        return;
    }

    const uint32_t framing = s_a.framing + s_b.framing;
    const uint32_t crc     = s_a.crc     + s_b.crc;
    const uint32_t busy    = s_a.busy    + s_b.busy;
    const uint32_t clean   = s_a.ok      + s_b.ok;

    if (framing > 0u) {
        ESP_LOGE(TAG, "VERDICT: FAIL — %lu stolen frames (CRC-valid, wrong "
                      "address/FC). The RX FIFO was shared; the lock is not "
                      "serialising transactions.", (unsigned long)framing);
    } else if (crc > 0u) {
        ESP_LOGE(TAG, "VERDICT: FAIL — %lu CRC errors. Frames interleaved on "
                      "the wire (colliding DE/RE or a violated t3.5 gap).",
                 (unsigned long)crc);
    } else if (busy > 0u) {
        ESP_LOGE(TAG, "VERDICT: FAIL — %lu MODBUS_ERR_BUSY. A transaction held "
                      "the lock beyond MODBUS_LOCK_TIMEOUT_MS (%u ms); "
                      "investigate rather than dismiss.",
                 (unsigned long)busy, (unsigned)MODBUS_LOCK_TIMEOUT_MS);
    } else if (clean == 0u) {
        /* Measured on FDA4 2026-09-07: this is exactly what the UNPATCHED
         * driver produces — 500/500 timeouts per caller, zero FRAMING, zero
         * CRC. Unsynchronised access is total rather than subtle: each task's
         * 8-byte echo drain consumes the other's response bytes, so no frame
         * ever assembles and every transaction times out. Do not read the
         * absence of FRAMING/CRC as "no corruption". */
        ESP_LOGE(TAG, "VERDICT: FAIL — 0 of %u transactions completed. Total "
                      "bus failure: the callers are destroying each other's "
                      "frames outright (each echo drain eats the other's "
                      "response). This is the unpatched-driver signature.",
                 (unsigned)(2u * PROBE_ITERATIONS));
    } else if (clean != (2u * PROBE_ITERATIONS)) {
        ESP_LOGW(TAG, "VERDICT: INCONCLUSIVE — no corruption, but only %lu of "
                      "%u transactions were clean. Check the timeout counts: a "
                      "quiet bus should give very few.",
                 (unsigned long)clean, (unsigned)(2u * PROBE_ITERATIONS));
    } else {
        ESP_LOGI(TAG, "VERDICT: PASS — %lu/%u clean, zero stolen frames, zero "
                      "CRC errors, zero BUSY.",
                 (unsigned long)clean, (unsigned)(2u * PROBE_ITERATIONS));
        ESP_LOGW(TAG, "This is only meaningful if the same probe FAILED against "
                      "a build with the mutex reverted. If you have not run "
                      "that, you have not tested anything yet.");
    }
}

#endif /* MODBUS_CONCURRENCY_PROBE */
