/**
 * @file modbus_rtu.cpp
 * @brief Modbus RTU master driver implementation (LIB-6).
 *
 * Migrated from arduino-esp32's `Serial1` to ESP-IDF's `uart_driver_*`
 * API in 2.0.0-alpha.2.6 (Phase 2.6). All RS-485 direction control goes
 * through gpio_util (LIB-1, migrated in alpha.2.1).
 *
 * Transaction sequence per request:
 *  1. Assert DE/RE HIGH via gpio_set_rs485_direction(true).
 *  2. Write 8-byte request frame to UART1.
 *  3. Wait for TX FIFO to drain via uart_wait_tx_done().
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
 * API mapping (arduino → ESP-IDF):
 *   Serial1.begin(...)          → uart_driver_install + uart_param_config + uart_set_pin
 *   Serial1.write(buf, n)       → uart_write_bytes(UART_NUM_1, buf, n)
 *   Serial1.flush()             → uart_wait_tx_done(UART_NUM_1, timeout)
 *   Serial1.available()         → uart_get_buffered_data_len(UART_NUM_1, &n); n > 0
 *   Serial1.read()              → uart_read_bytes(UART_NUM_1, &byte, 1, 0)
 *   micros()                    → esp_timer_get_time() (cast to uint32_t for wrap-compat)
 *   millis()                    → esp_timer_get_time() / 1000 (likewise)
 *   delayMicroseconds(us)       → esp_rom_delay_us(us)
 *
 * The migration preserves the original code's exact pacing (IFG enforcement,
 * RS-485 direction flip timing, 8-byte echo drain, counted RX with timeout).
 * Modbus timing is critical — any deviation can break frame detection on
 * slow buses or with slaves that produce bursty replies.
 *
 * @author Greenhouse Controller project
 */

#ifdef NATIVE_TEST
  #include "mock_uart.h"
  #include "mock_gpio.h"
#endif

#include "modbus_rtu.h"

#ifndef NATIVE_TEST
  #include "gpio_util.h"
  #include "driver/uart.h"
  #include "esp_timer.h"
  #include "esp_rom_sys.h"           /* esp_rom_delay_us */
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"          /* pdMS_TO_TICKS, vTaskDelay (not used; ticks only) */
#endif

/* ---------------------------------------------------------------------------
 * UART configuration constants — local to this module.
 *
 * RX_BUF_SIZE: ESP-IDF requires >= 128. Set to 256 — comfortably larger than
 * the longest possible Modbus response (256 bytes data + header/CRC = 261).
 * For our use (max 2-register reads = 9 bytes response) 256 is generous.
 *
 * TX_BUF_SIZE: 0 disables TX buffering — uart_write_bytes blocks until each
 * byte is in the hardware FIFO. Matches Arduino Serial1.write semantics for
 * short frames (FIFO is 128 bytes on ESP32-S3 UART1).
 *
 * EVENT_QUEUE: 0/NULL — we poll buffered-bytes count rather than using
 * event-driven RX. Preserves the existing single-threaded transaction model.
 * --------------------------------------------------------------------------- */
#ifndef NATIVE_TEST
#define MODBUS_UART_PORT  UART_NUM_1
#define MODBUS_RX_BUF     256
#define MODBUS_TX_BUF     0          /* blocking writes; no SW TX buffer */
#endif

/* ---------------------------------------------------------------------------
 * micros() / millis() / delayMicroseconds() shims
 *
 * Wrap ESP-IDF primitives so the rest of the file reads identical to the
 * arduino-era code. micros() and millis() return uint32_t for wrap-equivalence
 * with the original arduino-style subtraction (modulo arithmetic survives
 * across the 32-bit wrap at ~71 minutes for micros, 49.7 days for millis).
 * --------------------------------------------------------------------------- */
#ifndef NATIVE_TEST
static inline uint32_t micros(void) {
    return (uint32_t)esp_timer_get_time();
}
static inline uint32_t millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}
static inline void delayMicroseconds(uint32_t us) {
    esp_rom_delay_us(us);
}
#endif

/* ---------------------------------------------------------------------------
 * UART helpers — preserve Arduino-style API at the cpp call sites
 * --------------------------------------------------------------------------- */
#ifndef NATIVE_TEST
/* Returns the number of bytes in the RX ring buffer ready to read. */
static inline int uart1_available(void) {
    size_t n = 0;
    uart_get_buffered_data_len(MODBUS_UART_PORT, &n);
    return (int)n;
}

/* Non-blocking single-byte read. Returns the byte (0..255) or -1 if none. */
static inline int uart1_read(void) {
    uint8_t b = 0;
    int rc = uart_read_bytes(MODBUS_UART_PORT, &b, 1, 0);
    return (rc == 1) ? (int)b : -1;
}

/* Blocking write of n bytes. */
static inline void uart1_write(const uint8_t *buf, size_t n) {
    (void)uart_write_bytes(MODBUS_UART_PORT, (const char *)buf, n);
}

/* Block until the hardware FIFO has drained (matches Serial1.flush). */
static inline void uart1_flush_tx(void) {
    /* 50 ms is ample for the longest frame we send (~261 bytes @ 9600 baud
     * = ~272 ms in theory but write() above blocks until in FIFO; this just
     * waits for the FIFO to actually clock out). For a Modbus 8-byte request
     * the FIFO drain is < 10 ms — 50 ms is a safety ceiling. */
    (void)uart_wait_tx_done(MODBUS_UART_PORT, pdMS_TO_TICKS(50));
}
#else
/* In NATIVE_TEST builds the mock_uart.h header provides Arduino-shaped
 * Serial1.* functions; the rest of the file uses them directly via the
 * existing arduino call shapes (Serial1.available() etc.). The shims
 * above are not needed. */
#endif

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
 * Inter-frame gap enforcement
 *
 * Modbus RTU requires at least 3.5 character-times of silence between
 * frames.  At 9600 baud, 8N1 (10 bits/char): 3.5 × (10/9600) × 1e6 = 3646 μs.
 * MODBUS_IFG_US = 4000 μs gives a comfortable margin above this floor.
 *
 * s_frame_end_us records micros() at the moment the last bit of each frame
 * arrived or left the wire.  The IFG guard at the start of every transmission
 * spin-waits only as long as needed to reach MODBUS_IFG_US from that
 * timestamp, so the gap is enforced against the actual wire event regardless
 * of how long the caller takes between transactions.
 * --------------------------------------------------------------------------- */
#define MODBUS_IFG_US  4000u

static uint32_t s_frame_end_us = 0u;

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

void modbus_init(void)
{
    gpio_rs485_init();                 /* configure DE/RE pin as output, LOW */

#ifndef NATIVE_TEST
    /* Install UART driver if not already installed (idempotent guard). */
    if (uart_is_driver_installed(MODBUS_UART_PORT)) {
        uart_driver_delete(MODBUS_UART_PORT);
    }

    uart_config_t cfg = {};
    cfg.baud_rate  = MODBUS_BAUD;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    (void)uart_driver_install(MODBUS_UART_PORT,
                              MODBUS_RX_BUF, MODBUS_TX_BUF,
                              0, NULL, 0);
    (void)uart_param_config(MODBUS_UART_PORT, &cfg);
    (void)uart_set_pin(MODBUS_UART_PORT,
                       MODBUS_UART_TX, MODBUS_UART_RX,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
#else
    Serial1.begin(MODBUS_BAUD, SERIAL_8N1, MODBUS_UART_RX, MODBUS_UART_TX);
#endif

    gpio_set_rs485_direction(false);   /* start in receive mode */
    s_frame_end_us = micros();         /* start IFG timer from driver init */
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
     * Enforce the Modbus RTU 3.5-char-time inter-frame gap relative to the
     * actual end of the last frame on the wire, regardless of caller latency. */
    {
        uint32_t elapsed = micros() - s_frame_end_us;
        if (elapsed < MODBUS_IFG_US) {
            delayMicroseconds(MODBUS_IFG_US - elapsed);
        }
    }
    gpio_set_rs485_direction(true);   /* DE/RE HIGH — driver enable */
#ifndef NATIVE_TEST
    uart1_write(req, 8);
    uart1_flush_tx();
#else
    Serial1.write(req, 8);
    Serial1.flush();
#endif
    s_frame_end_us = micros();        /* last TX bit left the wire */
    /* Guard: one extra character time so the shift register finishes
     * clocking out the stop bit before DE/RE is deasserted.
     * At 9600 baud, 1 character ≈ 1.04 ms → 2 ms is ample. */
    delayMicroseconds(2000);
    gpio_set_rs485_direction(false);  /* DE/RE LOW — receiver enable */

    /* Wait one full character time (≈1.04 ms at 9600 baud) so the last
     * echoed stop bit has settled into the RX FIFO, then discard exactly
     * the 8 echo bytes produced by the half-duplex transceiver during TX.
     * A counted drain (not a "drain all") avoids discarding an early slave
     * response byte that may have arrived before DE/RE settled. */
    delayMicroseconds(1500);
    for (uint8_t i = 0; i < 8u; i++) {
#ifndef NATIVE_TEST
        if (uart1_available()) (void)uart1_read();
#else
        if (Serial1.available()) (void)Serial1.read();
#endif
    }

    /* Receive response */
    uint8_t  resp[256];
    uint8_t  received     = 0;
    uint8_t  expected_len = (uint8_t)(count * 2 + 5);  /* normal response length */
    uint32_t start        = millis();

    while (received < expected_len) {
        if (millis() - start > MODBUS_TIMEOUT_MS) {
            /* Drain any late-arriving bytes before returning so the next
             * transaction starts with a clean buffer. */
            delayMicroseconds(2000);
#ifndef NATIVE_TEST
            while (uart1_available()) (void)uart1_read();
#else
            while (Serial1.available()) (void)Serial1.read();
#endif
            s_frame_end_us = micros();   /* IFG measured from here */
            return MODBUS_ERR_TIMEOUT;
        }
#ifndef NATIVE_TEST
        if (uart1_available()) {
            resp[received++] = (uint8_t)uart1_read();
#else
        if (Serial1.available()) {
            resp[received++] = (uint8_t)Serial1.read();
#endif
            /* After the function-code byte: check for exception response */
            if (received == 2 && (resp[1] & 0x80)) {
                expected_len = 5;   /* exception frame: addr+fc+exc_code+crc_lo+crc_hi */
            }
        }
    }
    /* Record the wire timestamp as soon as the last response byte is in hand.
     * This is used by the IFG guard at the start of the next transaction. */
    s_frame_end_us = micros();

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
#ifndef NATIVE_TEST
    while (uart1_available()) (void)uart1_read();
#else
    while (Serial1.available()) (void)Serial1.read();
#endif

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

    /* Transmit — same IFG discipline as FC03/FC04 */
    {
        uint32_t elapsed = micros() - s_frame_end_us;
        if (elapsed < MODBUS_IFG_US) {
            delayMicroseconds(MODBUS_IFG_US - elapsed);
        }
    }
    gpio_set_rs485_direction(true);
#ifndef NATIVE_TEST
    uart1_write(req, (size_t)(payload_len + 2));
    uart1_flush_tx();
#else
    Serial1.write(req, (size_t)(payload_len + 2));
    Serial1.flush();
#endif
    s_frame_end_us = micros();        /* last TX bit left the wire */
    delayMicroseconds(2000);
    gpio_set_rs485_direction(false);

    /* Counted drain: discard exactly the echo bytes produced during TX.
     * For FC16 the frame is (payload_len + 2) bytes long. */
    delayMicroseconds(1500);
    for (uint8_t i = 0; i < (uint8_t)(payload_len + 2u); i++) {
#ifndef NATIVE_TEST
        if (uart1_available()) (void)uart1_read();
#else
        if (Serial1.available()) (void)Serial1.read();
#endif
    }

    /* Receive 8-byte normal response (or 5-byte exception) */
    uint8_t  resp[8];
    uint8_t  received     = 0;
    uint8_t  expected_len = 8;  /* addr+fc+reg_hi+reg_lo+cnt_hi+cnt_lo+crc_lo+crc_hi */
    uint32_t start        = millis();

    while (received < expected_len) {
        if (millis() - start > MODBUS_TIMEOUT_MS) {
            delayMicroseconds(2000);
#ifndef NATIVE_TEST
            while (uart1_available()) (void)uart1_read();
#else
            while (Serial1.available()) (void)Serial1.read();
#endif
            s_frame_end_us = micros();
            return MODBUS_ERR_TIMEOUT;
        }
#ifndef NATIVE_TEST
        if (uart1_available()) {
            resp[received++] = (uint8_t)uart1_read();
#else
        if (Serial1.available()) {
            resp[received++] = (uint8_t)Serial1.read();
#endif
            if (received == 2 && (resp[1] & 0x80)) {
                expected_len = 5;   /* exception frame */
            }
        }
    }
    s_frame_end_us = micros();   /* last response bit received */

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
#ifndef NATIVE_TEST
    while (uart1_available()) (void)uart1_read();
#else
    while (Serial1.available()) (void)Serial1.read();
#endif

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
