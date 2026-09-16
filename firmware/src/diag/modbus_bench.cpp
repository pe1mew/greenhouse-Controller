/**
 * @file modbus_bench.cpp
 * @brief Arbitrary Modbus access for bench commissioning — DEV BUILDS ONLY.
 *
 * See modbus_bench.h for the safety contract and why this exists.
 */

#ifdef MODBUS_BENCH

#include "modbus_bench.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "modbus_rtu.h"

static const char *TAG = "MB_BENCH";

/** Map a driver status to a stable, greppable string. */
static const char *status_str(modbus_status_t s)
{
    switch (s) {
        case MODBUS_OK:            return "MODBUS_OK";
        case MODBUS_ERR_TIMEOUT:   return "MODBUS_ERR_TIMEOUT";
        case MODBUS_ERR_CRC:       return "MODBUS_ERR_CRC";
        case MODBUS_ERR_EXCEPTION: return "MODBUS_ERR_EXCEPTION";
        case MODBUS_ERR_FRAMING:   return "MODBUS_ERR_FRAMING";
        case MODBUS_ERR_PARAM:     return "MODBUS_ERR_PARAM";
        case MODBUS_ERR_BUSY:      return "MODBUS_ERR_BUSY";
        default:                   return "MODBUS_ERR_UNKNOWN";
    }
}

bool modbus_bench_exec(uint8_t   addr,
                       uint8_t   fc,
                       uint16_t  reg,
                       uint16_t  arg,
                       uint16_t *out,
                       size_t    out_cap,
                       size_t   *n_out,
                       const char **err)
{
    if (n_out != NULL) *n_out = 0;
    if (err   != NULL) *err   = "unset";

    if (addr == 0u || addr > 247u) {
        if (err) *err = "bad_addr";
        return false;
    }

    modbus_status_t s;

    switch (fc) {
        case 3u:
        case 4u: {
            if (out == NULL || n_out == NULL) {
                if (err) *err = "no_buffer";
                return false;
            }
            if (arg == 0u || arg > MODBUS_BENCH_MAX_REGS || arg > out_cap) {
                if (err) *err = "bad_count";
                return false;
            }
            s = (fc == 3u)
                ? modbus_read_holding_registers(addr, reg, (uint8_t)arg, out)
                : modbus_read_input_registers(addr, reg, (uint8_t)arg, out);
            if (s == MODBUS_OK) *n_out = arg;
            break;
        }

        case 16u: {
            /* Writes are confined to the position sensor's two possible
             * addresses. An unrestricted write here could reconfigure the
             * FG6485A or the S200 — silently corrupting climate control with
             * no obvious link back to a bench command typed hours earlier. */
            if (addr != MODBUS_BENCH_WRITE_ADDR_A && addr != MODBUS_BENCH_WRITE_ADDR_B) {
                ESP_LOGW(TAG, "refused write to addr %u (only %u/%u are writable)",
                         (unsigned)addr,
                         (unsigned)MODBUS_BENCH_WRITE_ADDR_A,
                         (unsigned)MODBUS_BENCH_WRITE_ADDR_B);
                if (err) *err = "write_addr_refused";
                return false;
            }
            const uint16_t v = arg;   /* single-register write, quantity 1 */
            s = modbus_write_multiple_registers(addr, reg, 1u, &v);
            break;
        }

        default:
            /* the driver has no FC06 — use FC16 with quantity 1 instead */
            if (err) *err = "bad_fc";
            return false;
    }

    if (err) *err = status_str(s);
    ESP_LOGI(TAG, "addr=%u fc=%u reg=%u arg=%u -> %s",
             (unsigned)addr, (unsigned)fc, (unsigned)reg, (unsigned)arg, status_str(s));
    return (s == MODBUS_OK);
}

/* ===========================================================================
 * gh#69 re-init stress -- see modbus_bench_reinit_start()
 * =========================================================================== */

#define REINIT_MAX_COUNT     5000u
#define REINIT_MIN_IVAL_MS   5u
#define REINIT_MAX_IVAL_MS   1000u
/** T5's priority: the re-init that panicked the board came from T5. */
#define STORM_PRIO           5
/** Below T5 and T17, so the product's own polls win the bus when both wait. */
#define HAMMER_PRIO          3
#define STRESS_STACK         4096
/** The encoder's identification register (30007): any read will do. */
#define HAMMER_ADDR          MODBUS_BENCH_WRITE_ADDR_A
#define HAMMER_REG           0x0006u

static portMUX_TYPE          s_rmux = portMUX_INITIALIZER_UNLOCKED;
static modbus_bench_reinit_t s_run;
static volatile bool         s_hammer_stop  = true;
static volatile bool         s_hammer_alive = false;

static void hammer_task(void *arg)
{
    (void)arg;
    uint16_t v = 0u;
    while (!s_hammer_stop) {
        const modbus_status_t st = modbus_read_input_registers(HAMMER_ADDR, HAMMER_REG, 1u, &v);
        portENTER_CRITICAL(&s_rmux);
        if (st == MODBUS_OK) {
            s_run.hammer_ok++;
        } else {
            s_run.hammer_fail++;
            if (st == MODBUS_ERR_BUSY) { s_run.hammer_busy++; }
        }
        portEXIT_CRITICAL(&s_rmux);
        vTaskDelay(1);   /* one tick: the idle task runs, the bus stays nearly always busy */
    }
    s_hammer_alive = false;
    vTaskDelete(NULL);
}

static void storm_task(void *arg)
{
    (void)arg;
    const int64_t t0 = esp_timer_get_time();
    uint16_t n, ival;
    uint32_t dur;
    portENTER_CRITICAL(&s_rmux);
    n    = s_run.requested;
    ival = s_run.interval_ms;
    dur  = s_run.duration_ms;
    portEXIT_CRITICAL(&s_rmux);

    /* Traffic-only run: just keep time while the companion reads. */
    while (dur != 0u) {
        const uint32_t el = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        portENTER_CRITICAL(&s_rmux);
        s_run.elapsed_ms = el;
        portEXIT_CRITICAL(&s_rmux);
        if (el >= dur) { break; }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    for (uint16_t i = 0u; i < n; i++) {
        modbus_init();
        const uint32_t el = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        portENTER_CRITICAL(&s_rmux);
        s_run.done       = (uint16_t)(i + 1u);
        s_run.elapsed_ms = el;
        portEXIT_CRITICAL(&s_rmux);
        vTaskDelay(pdMS_TO_TICKS(ival));
    }

    s_hammer_stop = true;
    for (int w = 0; w < 100 && s_hammer_alive; w++) { vTaskDelay(pdMS_TO_TICKS(10)); }

    const uint32_t el = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    portENTER_CRITICAL(&s_rmux);
    s_run.elapsed_ms = el;
    s_run.running    = false;
    const modbus_bench_reinit_t done = s_run;
    portEXIT_CRITICAL(&s_rmux);
    ESP_LOGW(TAG, "re-init stress done: %u re-inits in %lu ms, hammer ok %lu fail %lu (busy %lu)",
             (unsigned)done.done, (unsigned long)done.elapsed_ms, (unsigned long)done.hammer_ok,
             (unsigned long)done.hammer_fail, (unsigned long)done.hammer_busy);
    vTaskDelete(NULL);
}

bool modbus_bench_reinit_start(uint16_t count, uint16_t interval_ms, bool hammer)
{
    if (count == 0u || count > REINIT_MAX_COUNT ||
        interval_ms < REINIT_MIN_IVAL_MS || interval_ms > REINIT_MAX_IVAL_MS) {
        return false;
    }
    if (s_hammer_alive) { return false; }   /* the last run's companion is still stopping */
    const uint32_t heap = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    portENTER_CRITICAL(&s_rmux);
    if (s_run.running) {
        portEXIT_CRITICAL(&s_rmux);
        return false;
    }
    s_run = modbus_bench_reinit_t{};
    s_run.running     = true;
    s_run.hammer      = hammer;
    s_run.requested   = count;
    s_run.interval_ms = interval_ms;
    s_run.heap_before = heap;
    portEXIT_CRITICAL(&s_rmux);

    s_hammer_stop = !hammer;
    if (hammer) {
        s_hammer_alive = true;
        if (xTaskCreatePinnedToCore(hammer_task, "mbR-hammer", STRESS_STACK, NULL,
                                    HAMMER_PRIO, NULL, tskNO_AFFINITY) != pdPASS) {
            s_hammer_alive = false;
            portENTER_CRITICAL(&s_rmux);
            s_run.running = false;
            portEXIT_CRITICAL(&s_rmux);
            return false;
        }
    }
    if (xTaskCreatePinnedToCore(storm_task, "mbR-storm", STRESS_STACK, NULL,
                                STORM_PRIO, NULL, tskNO_AFFINITY) != pdPASS) {
        s_hammer_stop = true;
        portENTER_CRITICAL(&s_rmux);
        s_run.running = false;
        portEXIT_CRITICAL(&s_rmux);
        return false;
    }
    ESP_LOGW(TAG, "re-init stress: %u x modbus_init() every %u ms, hammer %s, re-init %s",
             (unsigned)count, (unsigned)interval_ms, hammer ? "on" : "off",
             modbus_reinit_is_locked() ? "LOCKED" : "UNLOCKED -- fail-first build");
    return true;
}

bool modbus_bench_traffic_start(uint32_t duration_ms)
{
    if (duration_ms < 1000u || duration_ms > 600000u) { return false; }
    if (s_hammer_alive) { return false; }
    const uint32_t heap = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    portENTER_CRITICAL(&s_rmux);
    if (s_run.running) {
        portEXIT_CRITICAL(&s_rmux);
        return false;
    }
    s_run = modbus_bench_reinit_t{};
    s_run.running     = true;
    s_run.hammer      = true;
    s_run.duration_ms = duration_ms;
    s_run.heap_before = heap;
    portEXIT_CRITICAL(&s_rmux);

    s_hammer_stop  = false;
    s_hammer_alive = true;
    if (xTaskCreatePinnedToCore(hammer_task, "mbR-hammer", STRESS_STACK, NULL,
                                HAMMER_PRIO, NULL, tskNO_AFFINITY) != pdPASS) {
        s_hammer_alive = false;
        portENTER_CRITICAL(&s_rmux);
        s_run.running = false;
        portEXIT_CRITICAL(&s_rmux);
        return false;
    }
    /* The timekeeper: the same task as a re-init run, with nothing to re-init. */
    if (xTaskCreatePinnedToCore(storm_task, "mbR-storm", STRESS_STACK, NULL,
                                STORM_PRIO, NULL, tskNO_AFFINITY) != pdPASS) {
        s_hammer_stop = true;
        portENTER_CRITICAL(&s_rmux);
        s_run.running = false;
        portEXIT_CRITICAL(&s_rmux);
        return false;
    }
    ESP_LOGW(TAG, "bus traffic: companion reads for %lu ms", (unsigned long)duration_ms);
    return true;
}

void modbus_bench_reinit_status(modbus_bench_reinit_t *out)
{
    if (out == NULL) { return; }
    /* Outside the critical section: the heap query takes a lock of its own. */
    const uint32_t heap = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    portENTER_CRITICAL(&s_rmux);
    *out = s_run;
    portEXIT_CRITICAL(&s_rmux);
    out->heap_now = heap;
}

#endif /* MODBUS_BENCH */
