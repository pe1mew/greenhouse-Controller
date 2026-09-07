/**
 * @file modbus_bench.cpp
 * @brief Arbitrary Modbus access for bench commissioning — DEV BUILDS ONLY.
 *
 * See modbus_bench.h for the safety contract and why this exists.
 */

#ifdef MODBUS_BENCH

#include "modbus_bench.h"

#include "esp_log.h"
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

#endif /* MODBUS_BENCH */
