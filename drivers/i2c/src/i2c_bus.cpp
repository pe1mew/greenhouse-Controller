#ifndef UNIT_TEST
  #include <Arduino.h>
  #include <Wire.h>
#else
  #include "../test/mock_wire.h"
#endif

#include "i2c_bus.h"

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */

/**
 * @brief Map a Wire.endTransmission() return code to i2c_status_t.
 *
 * Arduino Wire endTransmission codes:
 *   0 — success
 *   1 — data too long for transmit buffer
 *   2 — NACK on transmit of address
 *   3 — NACK on transmit of data
 *   4 — other error
 *   5 — timeout
 */
static i2c_status_t wire_result(uint8_t r)
{
    switch (r) {
        case 0:  return I2C_OK;
        case 2:  /* fall-through */
        case 3:  return I2C_ERR_NACK;
        case 5:  return I2C_ERR_TIMEOUT;
        default: return I2C_ERR_BUS_BUSY;
    }
}

/* ---------------------------------------------------------------------------
 * API implementation
 * --------------------------------------------------------------------------- */

i2c_status_t i2c_init(void)
{
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
    return I2C_OK;
}

i2c_status_t i2c_write(uint8_t addr, const uint8_t *data, size_t len)
{
    Wire.beginTransmission(addr);
    if (len > 0) {
        Wire.write(data, len);
    }
    return wire_result(Wire.endTransmission(true));
}

i2c_status_t i2c_read(uint8_t addr, uint8_t *buf, size_t len)
{
    uint8_t received = Wire.requestFrom(addr, (uint8_t)len);
    for (uint8_t i = 0; i < received; i++) {
        buf[i] = (uint8_t)Wire.read();
    }
    return (received == (uint8_t)len) ? I2C_OK : I2C_ERR_NACK;
}

i2c_status_t i2c_write_read(uint8_t addr,
                              const uint8_t *tx, size_t tx_len,
                              uint8_t       *rx, size_t rx_len)
{
    Wire.beginTransmission(addr);
    if (tx_len > 0) {
        Wire.write(tx, tx_len);
    }
    uint8_t r = Wire.endTransmission(false); /* no stop — repeated start */
    if (r != 0) {
        return wire_result(r);
    }
    return i2c_read(addr, rx, rx_len);
}

uint8_t i2c_scan(uint8_t *found_addrs, uint8_t max_count)
{
    uint8_t count = 0;
    for (uint8_t addr = 1; addr < 127 && count < max_count; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission(true) == 0) {
            found_addrs[count++] = addr;
        }
    }
    return count;
}

void i2c_lock(void)   { /* no-op — FreeRTOS mutex not yet implemented */ }
void i2c_unlock(void) { /* no-op — FreeRTOS mutex not yet implemented */ }
