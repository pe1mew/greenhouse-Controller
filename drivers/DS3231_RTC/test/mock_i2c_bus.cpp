#include "mock_i2c_bus.h"

/* ---------------------------------------------------------------------------
 * Observable state
 * --------------------------------------------------------------------------- */
uint8_t mock_rtc_regs[MOCK_RTC_REG_COUNT];
bool    mock_nack_next = false;

/* ---------------------------------------------------------------------------
 * Helper functions
 * --------------------------------------------------------------------------- */
void mock_rtc_reset(void)
{
    memset(mock_rtc_regs, 0, sizeof(mock_rtc_regs));
    mock_nack_next = false;
}

void mock_rtc_set_register(uint8_t reg, uint8_t val)
{
    if (reg < MOCK_RTC_REG_COUNT) {
        mock_rtc_regs[reg] = val;
    }
}

/* ---------------------------------------------------------------------------
 * i2c_bus API stubs
 * --------------------------------------------------------------------------- */

/**
 * i2c_write behaviour:
 *   len == 0 or data == NULL → device probe, return OK or NACK.
 *   len >= 1                 → data[0] is register address;
 *                              data[1..len-1] written to register array.
 */
i2c_status_t i2c_write(uint8_t addr, const uint8_t *data, size_t len)
{
    (void)addr;
    if (mock_nack_next) {
        mock_nack_next = false;
        return I2C_ERR_NACK;
    }
    if (len == 0 || data == NULL) {
        return I2C_OK; /* address-only probe */
    }
    uint8_t reg = data[0];
    for (size_t i = 1; i < len; i++) {
        uint8_t r = (uint8_t)(reg + i - 1u);
        if (r < MOCK_RTC_REG_COUNT) {
            mock_rtc_regs[r] = data[i];
        }
    }
    return I2C_OK;
}

/**
 * i2c_read — not used by ds3231_rtc; provided as a no-op stub.
 */
i2c_status_t i2c_read(uint8_t addr, uint8_t *buf, size_t len)
{
    (void)addr; (void)buf; (void)len;
    return I2C_OK;
}

/**
 * i2c_write_read behaviour:
 *   tx[0] is the starting register address; rx_len bytes are copied
 *   from the register array starting at that offset.
 */
i2c_status_t i2c_write_read(uint8_t addr,
                              const uint8_t *tx, size_t tx_len,
                              uint8_t       *rx, size_t rx_len)
{
    (void)addr; (void)tx_len;
    if (mock_nack_next) {
        mock_nack_next = false;
        return I2C_ERR_NACK;
    }
    uint8_t reg = tx[0];
    for (size_t i = 0; i < rx_len; i++) {
        uint8_t r = (uint8_t)(reg + i);
        rx[i] = (r < MOCK_RTC_REG_COUNT) ? mock_rtc_regs[r] : 0x00u;
    }
    return I2C_OK;
}

void i2c_lock(void)   { /* no-op */ }
void i2c_unlock(void) { /* no-op */ }
