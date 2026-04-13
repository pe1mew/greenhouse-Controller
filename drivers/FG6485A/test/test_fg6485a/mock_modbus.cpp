/**
 * @file mock_modbus.cpp
 * @brief Modbus RTU stubs implementation for the native FG6485A unit-test build.
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#include <string.h>
#include "mock_modbus.h"

/* ---------------------------------------------------------------------------
 * Internal state
 * --------------------------------------------------------------------------- */

static const int BANK_SIZE = 256;    /* register address space covered */
static const int WBUF_SIZE = 128;    /* max registers per single write */

static uint16_t s_regs[BANK_SIZE];
static modbus_status_t s_read_status;
static modbus_status_t s_write_status;

static uint8_t   s_last_write_addr;
static uint16_t  s_last_write_reg;
static uint8_t   s_last_write_count;
static uint16_t  s_last_written[WBUF_SIZE];

/* ---------------------------------------------------------------------------
 * Test control functions
 * --------------------------------------------------------------------------- */

void mock_modbus_reset(void)
{
    memset(s_regs, 0, sizeof(s_regs));
    s_read_status      = MODBUS_OK;
    s_write_status     = MODBUS_OK;
    s_last_write_addr  = 0u;
    s_last_write_reg   = 0u;
    s_last_write_count = 0u;
    memset(s_last_written, 0, sizeof(s_last_written));
}

void mock_modbus_set_registers(uint16_t start_reg,
                                const uint16_t *values, uint8_t count)
{
    for (uint8_t i = 0; i < count; i++) {
        int idx = (int)start_reg + i;
        if (idx < BANK_SIZE) {
            s_regs[idx] = values[i];
        }
    }
}

void mock_modbus_set_read_status(modbus_status_t status)
{
    s_read_status = status;
}

void mock_modbus_set_write_status(modbus_status_t status)
{
    s_write_status = status;
}

uint8_t  mock_modbus_get_last_write_addr(void)  { return s_last_write_addr; }
uint16_t mock_modbus_get_last_write_reg(void)   { return s_last_write_reg; }
uint8_t  mock_modbus_get_last_write_count(void) { return s_last_write_count; }

const uint16_t *mock_modbus_get_last_written_values(void)
{
    return s_last_written;
}

/* ---------------------------------------------------------------------------
 * Modbus API stubs
 * --------------------------------------------------------------------------- */

modbus_status_t modbus_read_holding_registers(uint8_t  device_addr,
                                               uint16_t start_reg,
                                               uint8_t  count,
                                               uint16_t *out)
{
    (void)device_addr;

    if (s_read_status != MODBUS_OK) {
        return s_read_status;
    }

    for (uint8_t i = 0; i < count; i++) {
        int idx = (int)start_reg + i;
        out[i] = (idx < BANK_SIZE) ? s_regs[idx] : 0u;
    }
    return MODBUS_OK;
}

modbus_status_t modbus_write_multiple_registers(uint8_t         device_addr,
                                                 uint16_t        start_reg,
                                                 uint8_t         count,
                                                 const uint16_t *values)
{
    s_last_write_addr  = device_addr;
    s_last_write_reg   = start_reg;
    s_last_write_count = count;

    uint8_t n = (count < (uint8_t)WBUF_SIZE) ? count : (uint8_t)WBUF_SIZE;
    memcpy(s_last_written, values, (size_t)n * sizeof(uint16_t));

    return s_write_status;
}
