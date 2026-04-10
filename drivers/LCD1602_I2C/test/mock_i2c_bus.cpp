#include "mock_i2c_bus.h"

/* ---------------------------------------------------------------------------
 * Observable state
 * --------------------------------------------------------------------------- */
uint8_t  mock_lcd_tx_buf[MOCK_LCD_TX_BUF];
uint16_t mock_lcd_tx_len = 0;
bool     mock_nack_next  = false;

/* ---------------------------------------------------------------------------
 * Helper functions
 * --------------------------------------------------------------------------- */

void mock_lcd_reset(void)
{
    memset(mock_lcd_tx_buf, 0, sizeof(mock_lcd_tx_buf));
    mock_lcd_tx_len = 0;
    mock_nack_next  = false;
}

uint16_t mock_lcd_get_transmitted_bytes(uint8_t *buf, uint16_t maxlen)
{
    uint16_t n = (mock_lcd_tx_len < maxlen) ? mock_lcd_tx_len : maxlen;
    memcpy(buf, mock_lcd_tx_buf, n);
    return n;
}

/* ---------------------------------------------------------------------------
 * i2c_bus API stubs
 * --------------------------------------------------------------------------- */

/**
 * i2c_write behaviour:
 *   - NACK injection: if mock_nack_next is set, return I2C_ERR_NACK and clear flag.
 *   - Probe (len == 0 or data == NULL): return I2C_OK without logging.
 *   - Otherwise: append each byte to the TX log.
 */
i2c_status_t i2c_write(uint8_t addr, const uint8_t *data, size_t len)
{
    (void)addr;

    if (mock_nack_next) {
        mock_nack_next = false;
        return I2C_ERR_NACK;
    }

    if (data == NULL || len == 0u) {
        return I2C_OK;  /* address-only probe — no bytes logged */
    }

    for (size_t i = 0u; i < len; i++) {
        if (mock_lcd_tx_len < MOCK_LCD_TX_BUF) {
            mock_lcd_tx_buf[mock_lcd_tx_len++] = data[i];
        }
    }
    return I2C_OK;
}

/** i2c_read — LCD is write-only; stub returns OK without touching buf. */
i2c_status_t i2c_read(uint8_t addr, uint8_t *buf, size_t len)
{
    (void)addr; (void)buf; (void)len;
    return I2C_OK;
}

/** i2c_write_read — not used by lcd1602; stub returns OK. */
i2c_status_t i2c_write_read(uint8_t addr,
                              const uint8_t *tx, size_t tx_len,
                              uint8_t       *rx, size_t rx_len)
{
    (void)addr; (void)tx; (void)tx_len; (void)rx; (void)rx_len;
    return I2C_OK;
}

void i2c_lock(void)   { /* no-op */ }
void i2c_unlock(void) { /* no-op */ }
