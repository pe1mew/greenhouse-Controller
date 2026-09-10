/**
 * @file window_pos.cpp
 * @brief Wire-encoder window position sensor driver. See window_pos.h.
 */

#include "window_pos.h"

#include "modbus_rtu.h"

/* Contract register offsets. The hex column, not the 3xxxx/4xxxx labels. */
#define IN_BLOCK_START   0x0000u  /* 30001 */
#define IN_BLOCK_COUNT   15u      /* through 30015 */
#define IN_IDENT         0x0006u  /* 30007 */
#define IN_CAP_CLOSED    0x000Cu  /* 30013 */
#define IN_CAP_OPEN      0x000Du  /* 30014 */

#define HOLD_BLOCK_START 0x0000u  /* 40001 */
#define HOLD_BLOCK_COUNT 7u       /* through 40007 */
#define HOLD_WINDOW_MS   0x0001u  /* 40002 */
#define HOLD_TEACH       0x0006u  /* 40007 */

/* Indices into the 15-register input block. */
#define IX_OPENING   0
#define IX_AVG       1
#define IX_RAW_ADC   4
#define IX_STATUS    5
#define IX_RATE      11
#define IX_PERCENT   14

#define WINDOW_MS_MIN 100u
#define WINDOW_MS_MAX 60000u

/**
 * @brief Map a Modbus status to a driver status.
 *
 * MODBUS_ERR_PARAM passes through as a parameter error; everything else --
 * timeout, CRC, exception, and MODBUS_ERR_BUSY -- is a comms failure from the
 * caller's point of view. BUSY is folded in deliberately: it means another
 * task held the bus lock, which is a transient the caller retries exactly as
 * it would a timeout.
 */
static windowpos_status_t map_status(modbus_status_t s)
{
    if (s == MODBUS_OK)        { return WINDOWPOS_OK; }
    if (s == MODBUS_ERR_PARAM) { return WINDOWPOS_ERR_PARAM; }
    return WINDOWPOS_ERR_COMM;
}

/** @brief Write one register as FC16 quantity 1 -- there is no FC06 here. */
static windowpos_status_t write_one(uint8_t addr, uint16_t reg, uint16_t value)
{
    if (addr == 0u) { return WINDOWPOS_ERR_PARAM; }
    const uint16_t v = value;
    return map_status(modbus_write_multiple_registers(addr, reg, 1u, &v));
}

windowpos_status_t windowpos_read(uint8_t slave_addr, windowpos_reading_t *out)
{
    if (out == NULL || slave_addr == 0u) { return WINDOWPOS_ERR_PARAM; }

    uint16_t r[IN_BLOCK_COUNT] = {0};
    const modbus_status_t s =
        modbus_read_input_registers(slave_addr, IN_BLOCK_START, IN_BLOCK_COUNT, r);
    if (s != MODBUS_OK) { return map_status(s); }

    const uint16_t st = r[IX_STATUS];

    out->opening_mm_x10  = r[IX_OPENING];
    out->percent_x10     = r[IX_PERCENT];
    /* 30012 is the ONE signed register (contract 2.7). Read unsigned, a
     * closing window reports ~65 000 instead of a negative rate. */
    out->rate_mm_s_x10   = (int16_t)r[IX_RATE];
    out->opening_avg_x10 = r[IX_AVG];
    out->raw_adc         = r[IX_RAW_ADC];
    out->status_bits     = st;

    out->starting_up     = (st & (WINDOWPOS_ST_STARTUP_WINDOW | WINDOWPOS_ST_STARTUP_AVG)) != 0u;
    out->wiper_fault     = (st & WINDOWPOS_ST_WIPER_OPEN)    != 0u;
    out->at_end_sensor   = (st & WINDOWPOS_ST_END_SENSOR)    != 0u;
    out->both_end_sensors= (st & WINDOWPOS_ST_BOTH_ENDS)     != 0u;
    out->teach_armed     = (st & WINDOWPOS_ST_TEACH_ARMED)   != 0u;
    out->implausible     = (st & WINDOWPOS_ST_IMPLAUSIBLE)   != 0u;
    out->not_following   = (st & WINDOWPOS_ST_NOT_FOLLOWING) != 0u;

    /* Two independent signals mean "do not use the position": the wiper bit,
     * and the 65535 sentinel in the value registers. They do not always appear
     * together -- the sentinel arrives ~2 s before the bit during a wiper
     * failure (contract 3.3 row 2) -- so both are folded into one flag here
     * rather than left for every caller to remember. */
    out->sensor_fault = out->wiper_fault
                        || (r[IX_OPENING] == WINDOWPOS_FAULT_SENTINEL)
                        || (r[IX_PERCENT] == WINDOWPOS_FAULT_SENTINEL);

    return WINDOWPOS_OK;
}

windowpos_status_t windowpos_read_ident(uint8_t slave_addr,
                                        uint8_t *out_build,
                                        uint8_t *out_ver)
{
    if (slave_addr == 0u) { return WINDOWPOS_ERR_PARAM; }

    uint16_t v = 0u;
    const modbus_status_t s =
        modbus_read_input_registers(slave_addr, IN_IDENT, 1u, &v);
    if (s != MODBUS_OK) { return map_status(s); }

    if (out_build != NULL) { *out_build = (uint8_t)((v >> 8) & 0xFFu); }
    if (out_ver   != NULL) { *out_ver   = (uint8_t)(v & 0xFFu); }
    return WINDOWPOS_OK;
}

windowpos_status_t windowpos_read_config(uint8_t slave_addr,
                                         windowpos_config_t *out)
{
    if (out == NULL || slave_addr == 0u) { return WINDOWPOS_ERR_PARAM; }

    uint16_t r[HOLD_BLOCK_COUNT] = {0};
    const modbus_status_t s =
        modbus_read_holding_registers(slave_addr, HOLD_BLOCK_START,
                                      HOLD_BLOCK_COUNT, r);
    if (s != MODBUS_OK) { return map_status(s); }

    out->zero_offset_x10 = r[0];
    out->window_ms       = r[1];
    out->averaging_s     = r[2];
    out->full_travel_x10 = r[3];
    out->raw_closed      = r[4];
    out->raw_open        = r[5];
    out->teach_cmd       = r[6];
    return WINDOWPOS_OK;
}

windowpos_status_t windowpos_set_window_ms(uint8_t slave_addr, uint16_t window_ms)
{
    if (window_ms < WINDOW_MS_MIN || window_ms > WINDOW_MS_MAX) {
        return WINDOWPOS_ERR_PARAM;
    }
    return write_one(slave_addr, HOLD_WINDOW_MS, window_ms);
}

windowpos_status_t windowpos_teach(uint8_t slave_addr, bool arm)
{
    return write_one(slave_addr, HOLD_TEACH, arm ? 1u : 0u);
}

windowpos_status_t windowpos_read_captures(uint8_t slave_addr,
                                           uint16_t *out_closed,
                                           uint16_t *out_open)
{
    if (slave_addr == 0u) { return WINDOWPOS_ERR_PARAM; }

    /* Two single-register reads rather than one block: the commit in contract
     * 6.2 step d fires once BOTH have been read, so reading them as a pair is
     * the deliberate act. Keeping them separate lets a caller read just one
     * without triggering it. */
    uint16_t c = 0u, o = 0u;
    modbus_status_t s = modbus_read_input_registers(slave_addr, IN_CAP_CLOSED, 1u, &c);
    if (s != MODBUS_OK) { return map_status(s); }
    s = modbus_read_input_registers(slave_addr, IN_CAP_OPEN, 1u, &o);
    if (s != MODBUS_OK) { return map_status(s); }

    if (out_closed != NULL) { *out_closed = c; }
    if (out_open   != NULL) { *out_open   = o; }
    return WINDOWPOS_OK;
}
