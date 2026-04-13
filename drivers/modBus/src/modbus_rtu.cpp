/**
 * @file modbus_rtu.cpp
 * @brief Modbus RTU master driver implementation (LIB-6).
 *
 * Transaction sequence per request:
 *  1. Assert DE/RE HIGH via gpio_set_rs485_direction(true).
 *  2. Write 8-byte request frame to Serial1.
 *  3. Call Serial1.flush() to wait for TX to complete.
 *  4. Assert DE/RE LOW via gpio_set_rs485_direction(false).
 *  5. Read bytes with per-frame timeout until (count*2 + 5) bytes received.
 *     After byte 1: if fc|0x80, adjust expected length to 5 (exception frame).
 *  6. Validate CRC16 (polynomial 0xA001).
 *  7. Return parsed register values, or an error code.
 *
 * Frame format:
 *   Request  (8 bytes): [addr][fc][reg_hi][reg_lo][cnt_hi][cnt_lo][crc_lo][crc_hi]
 *   Response           : [addr][fc][byte_cnt][data...][crc_lo][crc_hi]
 *   Exception (5 bytes): [addr][fc|0x80][exc_code][crc_lo][crc_hi]
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#ifndef NATIVE_TEST
  #include <Arduino.h>
  #include "gpio_util.h"
#else
  #include "../test/mock_uart.h"
  #include "../test/mock_gpio.h"
#endif

#include "modbus_rtu.h"

/* ---------------------------------------------------------------------------
 * CRC16 — Modbus (polynomial 0xA001, init 0xFFFF)
 * Static in production; exported as modbus_crc16() in unit-test builds.
 * --------------------------------------------------------------------------- */
#ifdef NATIVE_TEST
uint16_t modbus_crc16(const uint8_t *buf, uint8_t len)
#else
static uint16_t modbus_crc16(const uint8_t *buf, uint8_t len)
#endif
{
    uint16_t crc = 0xFFFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= (uint16_t)buf[i];
        for (uint8_t b = 0; b < 8; b++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

void modbus_init(void)
{
    gpio_rs485_init();                 /* configure DE/RE pin as output, LOW */
    Serial1.begin(MODBUS_BAUD, SERIAL_8N1, MODBUS_UART_RX, MODBUS_UART_TX);
    gpio_set_rs485_direction(false);   /* start in receive mode */
}

/* ---------------------------------------------------------------------------
 * Internal transaction helper — shared by FC03 and FC04.
 * --------------------------------------------------------------------------- */
static modbus_status_t modbus_transaction(uint8_t  device_addr,
                                           uint8_t  fc,
                                           uint16_t start_reg,
                                           uint8_t  count,
                                           uint16_t *out)
{
    /* Parameter validation */
    if (device_addr == 0) {
        return MODBUS_ERR_PARAM;   /* broadcast address not allowed for reads */
    }
    if (count == 0 || count > 125) {
        return MODBUS_ERR_PARAM;   /* FC03/FC04 register-count limit */
    }

    /* Build 8-byte request frame */
    uint8_t req[8];
    req[0] = device_addr;
    req[1] = fc;
    req[2] = (uint8_t)(start_reg >> 8);
    req[3] = (uint8_t)(start_reg & 0xFF);
    req[4] = 0x00;
    req[5] = count;
    uint16_t req_crc = modbus_crc16(req, 6);
    req[6] = (uint8_t)(req_crc & 0xFF);  /* CRC low byte first */
    req[7] = (uint8_t)(req_crc >> 8);    /* CRC high byte second */

    /* Transmit
     * Enforce the Modbus RTU inter-frame silent interval (3.5 character
     * times) before asserting DE/RE so the slave can detect the start of
     * a new frame.  At 9600 baud, 1 char ≈ 1.04 ms → 3.5 chars ≈ 3.65 ms;
     * 5 ms gives comfortable margin. */
    delayMicroseconds(5000);
    gpio_set_rs485_direction(true);   /* DE/RE HIGH — driver enable */
    Serial1.write(req, 8);
    Serial1.flush();                  /* wait for TX FIFO to drain */
    /* Guard: one extra character time so the shift register finishes
     * clocking out the stop bit before DE/RE is deasserted.
     * At 9600 baud, 1 character ≈ 1.04 ms → 2 ms is ample. */
    delayMicroseconds(2000);
    gpio_set_rs485_direction(false);  /* DE/RE LOW — receiver enable */

    /* Wait one full character time (≈1.04 ms at 9600 baud) so the last
     * echoed stop bit has settled into the RX FIFO, then discard any
     * bytes that arrived during TX (half-duplex echo). */
    delayMicroseconds(1500);
    while (Serial1.available())
        (void)Serial1.read();

    /* Receive response */
    uint8_t  resp[256];
    uint8_t  received     = 0;
    uint8_t  expected_len = (uint8_t)(count * 2 + 5);  /* normal response length */
    uint32_t start        = millis();

    while (received < expected_len) {
        if (millis() - start > MODBUS_TIMEOUT_MS) {
            /* Drain any late-arriving bytes before returning so the next
             * transaction starts with a clean buffer. */
            delayMicroseconds(5000);
            while (Serial1.available()) (void)Serial1.read();
            return MODBUS_ERR_TIMEOUT;
        }
        if (Serial1.available()) {
            resp[received++] = (uint8_t)Serial1.read();
            /* After the function-code byte: check for exception response */
            if (received == 2 && (resp[1] & 0x80)) {
                expected_len = 5;   /* exception frame: addr+fc+exc_code+crc_lo+crc_hi */
            }
        }
    }

    /* Validate CRC over all bytes except the trailing two CRC bytes */
    uint16_t recv_crc = (uint16_t)resp[expected_len - 2]
                      | ((uint16_t)resp[expected_len - 1] << 8);
    uint16_t calc_crc = modbus_crc16(resp, (uint8_t)(expected_len - 2));
    if (recv_crc != calc_crc) {
        return MODBUS_ERR_CRC;
    }

    /* Detect exception after CRC is confirmed valid */
    if (resp[1] & 0x80) {
        return MODBUS_ERR_EXCEPTION;
    }

    /* Validate address and function code */
    if (resp[0] != device_addr || resp[1] != fc) {
        return MODBUS_ERR_FRAMING;
    }

    /* Validate data byte count */
    if (resp[2] != (uint8_t)(count * 2)) {
        return MODBUS_ERR_FRAMING;
    }

    /* Parse register values (big-endian pairs starting at byte 3) */
    for (uint8_t i = 0; i < count; i++) {
        out[i] = ((uint16_t)resp[3 + i * 2] << 8) | resp[4 + i * 2];
    }

    /* Drain any surplus bytes (e.g. interleaved echoes) so the next
     * transaction starts with a clean buffer. */
    delayMicroseconds(2000);
    while (Serial1.available()) (void)Serial1.read();

    return MODBUS_OK;
}

modbus_status_t modbus_write_multiple_registers(uint8_t         device_addr,
                                                 uint16_t        start_reg,
                                                 uint8_t         count,
                                                 const uint16_t *values)
{
    if (device_addr == 0) {
        return MODBUS_ERR_PARAM;
    }
    if (count == 0 || count > 123) {
        return MODBUS_ERR_PARAM;   /* FC16 data limit: 246 bytes max → 123 registers */
    }
    if (values == nullptr) {
        return MODBUS_ERR_PARAM;
    }

    /* Build request frame: 7-byte header + count*2 data bytes + 2 CRC bytes */
    uint8_t  req[256];
    uint8_t  byte_count   = (uint8_t)(count * 2);
    uint8_t  payload_len  = (uint8_t)(7 + byte_count);

    req[0] = device_addr;
    req[1] = 0x10;                          /* FC16 */
    req[2] = (uint8_t)(start_reg >> 8);
    req[3] = (uint8_t)(start_reg & 0xFF);
    req[4] = 0x00;
    req[5] = count;
    req[6] = byte_count;
    for (uint8_t i = 0; i < count; i++) {
        req[7 + i * 2]     = (uint8_t)(values[i] >> 8);
        req[7 + i * 2 + 1] = (uint8_t)(values[i] & 0xFF);
    }
    uint16_t req_crc = modbus_crc16(req, payload_len);
    req[payload_len]     = (uint8_t)(req_crc & 0xFF);
    req[payload_len + 1] = (uint8_t)(req_crc >> 8);

    /* Transmit — same timing discipline as FC03/FC04 */
    delayMicroseconds(5000);
    gpio_set_rs485_direction(true);
    Serial1.write(req, (size_t)(payload_len + 2));
    Serial1.flush();
    delayMicroseconds(2000);
    gpio_set_rs485_direction(false);

    /* Discard half-duplex echo */
    delayMicroseconds(1500);
    while (Serial1.available())
        (void)Serial1.read();

    /* Receive 8-byte normal response (or 5-byte exception) */
    uint8_t  resp[8];
    uint8_t  received     = 0;
    uint8_t  expected_len = 8;  /* addr+fc+reg_hi+reg_lo+cnt_hi+cnt_lo+crc_lo+crc_hi */
    uint32_t start        = millis();

    while (received < expected_len) {
        if (millis() - start > MODBUS_TIMEOUT_MS) {
            delayMicroseconds(5000);
            while (Serial1.available()) (void)Serial1.read();
            return MODBUS_ERR_TIMEOUT;
        }
        if (Serial1.available()) {
            resp[received++] = (uint8_t)Serial1.read();
            if (received == 2 && (resp[1] & 0x80)) {
                expected_len = 5;   /* exception frame */
            }
        }
    }

    /* Validate CRC */
    uint16_t recv_crc = (uint16_t)resp[expected_len - 2]
                      | ((uint16_t)resp[expected_len - 1] << 8);
    uint16_t calc_crc = modbus_crc16(resp, (uint8_t)(expected_len - 2));
    if (recv_crc != calc_crc) {
        return MODBUS_ERR_CRC;
    }

    if (resp[1] & 0x80) {
        return MODBUS_ERR_EXCEPTION;
    }

    if (resp[0] != device_addr || resp[1] != 0x10) {
        return MODBUS_ERR_FRAMING;
    }

    /* Drain surplus bytes */
    delayMicroseconds(2000);
    while (Serial1.available()) (void)Serial1.read();

    return MODBUS_OK;
}

modbus_status_t modbus_read_holding_registers(uint8_t  device_addr,
                                               uint16_t start_reg,
                                               uint8_t  count,
                                               uint16_t *out)
{
    return modbus_transaction(device_addr, 0x03, start_reg, count, out);
}

modbus_status_t modbus_read_input_registers(uint8_t  device_addr,
                                             uint16_t start_reg,
                                             uint8_t  count,
                                             uint16_t *out)
{
    return modbus_transaction(device_addr, 0x04, start_reg, count, out);
}
