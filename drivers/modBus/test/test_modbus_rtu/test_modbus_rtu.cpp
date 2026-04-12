/**
 * LIB-6 Modbus RTU — unit tests (native build)
 *
 * Test IDs: UT-MB-001 … UT-MB-012
 *
 * Run with:  pio test -e native
 *
 * CRC16/Modbus: polynomial 0xA001, initial value 0xFFFF.
 * Known test vectors (verified by running the implementation):
 *   {0x01,0x03,0x00,0x00,0x00,0x02}  →  0x0BC4  (UT-MB-001)
 *     The standard Modbus frame ends with bytes C4 0B (low byte first).
 *     As a uint16_t the CRC value is therefore 0x0BC4, not 0xC40B.
 *     (The spec note "0xC40B" listed the bytes in frame order, not as a
 *      16-bit integer — confirmed by the frame check in UT-MB-003.)
 *   {0xFF}                            →  0x00FF  (UT-MB-002)
 */

#include <unity.h>
#include "../src/modbus_rtu.h"
#include "mock_uart.h"
#include "mock_gpio.h"

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */

/**
 * Local CRC16/Modbus implementation — used to build valid response frames
 * inside the tests without depending on the driver's internal function.
 */
static uint16_t test_crc16(const uint8_t *buf, uint8_t len)
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

/**
 * Build and queue a valid FC03/FC04 response frame for the given parameters.
 *
 * @param device_addr  Slave address echoed in the response.
 * @param fc           Function code (0x03 or 0x04).
 * @param values       Register values to embed (big-endian).
 * @param count        Number of registers.
 */
static void queue_valid_response(uint8_t device_addr, uint8_t fc,
                                 const uint16_t *values, uint8_t count)
{
    /* Frame: [addr][fc][byte_cnt][hi0][lo0]...[crc_lo][crc_hi] */
    uint8_t frame[256];
    uint8_t data_bytes = (uint8_t)(count * 2);
    frame[0] = device_addr;
    frame[1] = fc;
    frame[2] = data_bytes;
    for (uint8_t i = 0; i < count; i++) {
        frame[3 + i * 2]     = (uint8_t)(values[i] >> 8);
        frame[3 + i * 2 + 1] = (uint8_t)(values[i] & 0xFF);
    }
    uint8_t payload_len = (uint8_t)(3 + data_bytes);
    uint16_t crc = test_crc16(frame, payload_len);
    frame[payload_len]     = (uint8_t)(crc & 0xFF);
    frame[payload_len + 1] = (uint8_t)(crc >> 8);
    mock_uart_queue_response(frame, (uint8_t)(payload_len + 2));
}

/**
 * Find the index of the first occurrence of @p evt in the event log
 * at or after position @p after_idx.
 *
 * @return Index (≥ 0) on success, -1 if not found.
 */
static int find_event(mock_event_t evt, int after_idx)
{
    for (int i = after_idx; i < mock_event_count; i++) {
        if (mock_event_log[i] == evt) {
            return i;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * Unity fixtures
 * --------------------------------------------------------------------------- */
void setUp(void)
{
    mock_uart_reset();   /* clears TX/RX buffers, event log, millis counter */
    mock_gpio_reset();   /* resets DE/RE direction state */
}

void tearDown(void) {}

/* =========================================================================
 * UT-MB-001 — CRC16 of {0x01,0x03,0x00,0x00,0x00,0x02} = 0xC40B
 * ========================================================================= */
void test_crc16_known_vector(void)
{
    const uint8_t buf[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x02};
    /* The standard Modbus frame 01 03 00 00 00 02 ends with bytes C4 0B.
     * Modbus sends CRC low byte first, so frame[n]=0xC4 is the low byte
     * and frame[n+1]=0x0B is the high byte → uint16_t value = 0x0BC4. */
    TEST_ASSERT_EQUAL_HEX16(0x0BC4, modbus_crc16(buf, sizeof(buf)));
}

/* =========================================================================
 * UT-MB-002 — CRC16 of single byte {0xFF} = 0x00FF
 * ========================================================================= */
void test_crc16_single_byte_ff(void)
{
    const uint8_t buf[] = {0xFF};
    TEST_ASSERT_EQUAL_HEX16(0x00FF, modbus_crc16(buf, sizeof(buf)));
}

/* =========================================================================
 * UT-MB-003 — FC03 request frame bytes are correct
 *             (addr=1, reg=0x0000, count=2)
 *
 * Expected: [0x01][0x03][0x00][0x00][0x00][0x02][0x0B][0xC4]
 *           CRC of first 6 bytes = 0xC40B → lo=0x0B, hi=0xC4
 * ========================================================================= */
void test_fc03_request_frame(void)
{
    const uint16_t vals[2] = {0x1234, 0x5678};
    queue_valid_response(1, 0x03, vals, 2);

    uint16_t out[2] = {0};
    modbus_read_holding_registers(1, 0x0000, 2, out);

    uint8_t tx[8];
    int n = mock_uart_get_transmitted(tx, sizeof(tx));

    TEST_ASSERT_EQUAL_INT(8, n);
    TEST_ASSERT_EQUAL_HEX8(0x01, tx[0]);   /* device address */
    TEST_ASSERT_EQUAL_HEX8(0x03, tx[1]);   /* function code FC03 */
    TEST_ASSERT_EQUAL_HEX8(0x00, tx[2]);   /* register high byte */
    TEST_ASSERT_EQUAL_HEX8(0x00, tx[3]);   /* register low byte */
    TEST_ASSERT_EQUAL_HEX8(0x00, tx[4]);   /* count high byte */
    TEST_ASSERT_EQUAL_HEX8(0x02, tx[5]);   /* count = 2 */
    TEST_ASSERT_EQUAL_HEX8(0xC4, tx[6]);   /* CRC low  byte  (0x0BC4 & 0xFF) */
    TEST_ASSERT_EQUAL_HEX8(0x0B, tx[7]);   /* CRC high byte  (0x0BC4 >> 8)   */
}

/* =========================================================================
 * UT-MB-004 — FC03 response parsed to correct uint16_t values
 * ========================================================================= */
void test_fc03_response_parsed(void)
{
    const uint16_t vals[2] = {0x1234, 0x5678};
    queue_valid_response(1, 0x03, vals, 2);

    uint16_t out[2] = {0, 0};
    modbus_status_t status = modbus_read_holding_registers(1, 0, 2, out);

    TEST_ASSERT_EQUAL_INT(MODBUS_OK, status);
    TEST_ASSERT_EQUAL_HEX16(0x1234, out[0]);
    TEST_ASSERT_EQUAL_HEX16(0x5678, out[1]);
}

/* =========================================================================
 * UT-MB-005 — FC04 request uses function code 0x04
 * ========================================================================= */
void test_fc04_uses_function_code_04(void)
{
    const uint16_t vals[2] = {0x00E6, 0x028F};
    queue_valid_response(2, 0x04, vals, 2);

    uint16_t out[2] = {0};
    modbus_read_input_registers(2, 0, 2, out);

    uint8_t tx[8];
    mock_uart_get_transmitted(tx, sizeof(tx));
    TEST_ASSERT_EQUAL_HEX8(0x04, tx[1]);   /* frame byte[1] must be FC04 */
}

/* =========================================================================
 * UT-MB-006 — No response → MODBUS_ERR_TIMEOUT
 *
 * millis() advances by MODBUS_TIMEOUT_MS+1 per call so the very first
 * timeout check inside the receive loop fires immediately.
 * ========================================================================= */
void test_no_response_returns_timeout(void)
{
    /* No bytes queued — millis advances fast to trigger timeout */
    mock_set_millis(0, MODBUS_TIMEOUT_MS + 1);

    uint16_t out[2] = {0};
    modbus_status_t status = modbus_read_holding_registers(1, 0, 2, out);
    TEST_ASSERT_EQUAL_INT(MODBUS_ERR_TIMEOUT, status);
}

/* =========================================================================
 * UT-MB-007 — Response with flipped CRC byte → MODBUS_ERR_CRC
 * ========================================================================= */
void test_flipped_crc_returns_crc_error(void)
{
    /* Build a valid frame, then corrupt the CRC low byte */
    const uint16_t vals[2] = {0x1234, 0x5678};
    uint8_t frame[9];
    frame[0] = 0x01;  /* addr */
    frame[1] = 0x03;  /* fc */
    frame[2] = 0x04;  /* byte count */
    frame[3] = 0x12; frame[4] = 0x34;
    frame[5] = 0x56; frame[6] = 0x78;
    uint16_t crc = test_crc16(frame, 7);
    frame[7] = (uint8_t)(crc & 0xFF) ^ 0xFF;   /* flip all bits in CRC lo */
    frame[8] = (uint8_t)(crc >> 8);
    mock_uart_queue_response(frame, 9);

    uint16_t out[2] = {0};
    modbus_status_t status = modbus_read_holding_registers(1, 0, 2, out);
    TEST_ASSERT_EQUAL_INT(MODBUS_ERR_CRC, status);
}

/* =========================================================================
 * UT-MB-008 — Exception response (FC | 0x80) → MODBUS_ERR_EXCEPTION
 *
 * Exception frame: [addr][fc|0x80][exc_code][crc_lo][crc_hi] (5 bytes)
 * ========================================================================= */
void test_exception_response(void)
{
    uint8_t frame[5];
    frame[0] = 0x01;          /* addr */
    frame[1] = 0x03 | 0x80;   /* FC03 exception */
    frame[2] = 0x02;          /* exception code: Illegal Data Address */
    uint16_t crc = test_crc16(frame, 3);
    frame[3] = (uint8_t)(crc & 0xFF);
    frame[4] = (uint8_t)(crc >> 8);
    mock_uart_queue_response(frame, 5);

    uint16_t out[2] = {0};
    modbus_status_t status = modbus_read_holding_registers(1, 0, 2, out);
    TEST_ASSERT_EQUAL_INT(MODBUS_ERR_EXCEPTION, status);
}

/* =========================================================================
 * UT-MB-009 — DE/RE HIGH before first TX byte
 *
 * Event log order: MOCK_EVT_GPIO_HIGH must appear before MOCK_EVT_UART_WRITE.
 * ========================================================================= */
void test_de_re_high_before_uart_write(void)
{
    const uint16_t vals[2] = {0x0000, 0x0000};
    queue_valid_response(1, 0x03, vals, 2);

    uint16_t out[2] = {0};
    modbus_read_holding_registers(1, 0, 2, out);

    int gpio_high_idx = find_event(MOCK_EVT_GPIO_HIGH, 0);
    int uart_write_idx = find_event(MOCK_EVT_UART_WRITE, 0);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, gpio_high_idx,
        "No MOCK_EVT_GPIO_HIGH event recorded");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, uart_write_idx,
        "No MOCK_EVT_UART_WRITE event recorded");
    /* gpio_high must come before uart_write */
    TEST_ASSERT_LESS_THAN_MESSAGE(uart_write_idx, gpio_high_idx,
        "DE/RE was not driven HIGH before the first UART write");
}

/* =========================================================================
 * UT-MB-010 — DE/RE LOW before RX
 *
 * Event log order: MOCK_EVT_GPIO_LOW must appear before MOCK_EVT_UART_READ.
 * ========================================================================= */
void test_de_re_low_before_uart_read(void)
{
    const uint16_t vals[2] = {0x0000, 0x0000};
    queue_valid_response(1, 0x03, vals, 2);

    uint16_t out[2] = {0};
    modbus_read_holding_registers(1, 0, 2, out);

    int gpio_low_idx  = find_event(MOCK_EVT_GPIO_LOW,   0);
    int uart_read_idx = find_event(MOCK_EVT_UART_READ,  0);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, gpio_low_idx,
        "No MOCK_EVT_GPIO_LOW event recorded");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, uart_read_idx,
        "No MOCK_EVT_UART_READ event recorded");
    /* gpio_low must come before first uart_read */
    TEST_ASSERT_LESS_THAN_MESSAGE(uart_read_idx, gpio_low_idx,
        "DE/RE was not driven LOW before the first UART read");
}

/* =========================================================================
 * UT-MB-011 — Device address 0 → MODBUS_ERR_PARAM
 *             (broadcast address rejected for reads)
 * ========================================================================= */
void test_device_address_zero_returns_param_error(void)
{
    uint16_t out[2] = {0};
    modbus_status_t status = modbus_read_holding_registers(0, 0, 2, out);
    TEST_ASSERT_EQUAL_INT(MODBUS_ERR_PARAM, status);
}

/* =========================================================================
 * UT-MB-012 — Register count > 125 → MODBUS_ERR_PARAM
 *             (FC03/FC04 maximum enforced)
 * ========================================================================= */
void test_register_count_exceeds_maximum(void)
{
    uint16_t out[126] = {0};
    modbus_status_t status;

    /* count = 126 — one above maximum */
    status = modbus_read_holding_registers(1, 0, 126, out);
    TEST_ASSERT_EQUAL_INT(MODBUS_ERR_PARAM, status);

    /* Also verify FC04 rejects it */
    status = modbus_read_input_registers(1, 0, 126, out);
    TEST_ASSERT_EQUAL_INT(MODBUS_ERR_PARAM, status);
}

/* ---------------------------------------------------------------------------
 * Entry point
 * --------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    UNITY_BEGIN();

    /* UT-MB-001 */ RUN_TEST(test_crc16_known_vector);
    /* UT-MB-002 */ RUN_TEST(test_crc16_single_byte_ff);
    /* UT-MB-003 */ RUN_TEST(test_fc03_request_frame);
    /* UT-MB-004 */ RUN_TEST(test_fc03_response_parsed);
    /* UT-MB-005 */ RUN_TEST(test_fc04_uses_function_code_04);
    /* UT-MB-006 */ RUN_TEST(test_no_response_returns_timeout);
    /* UT-MB-007 */ RUN_TEST(test_flipped_crc_returns_crc_error);
    /* UT-MB-008 */ RUN_TEST(test_exception_response);
    /* UT-MB-009 */ RUN_TEST(test_de_re_high_before_uart_write);
    /* UT-MB-010 */ RUN_TEST(test_de_re_low_before_uart_read);
    /* UT-MB-011 */ RUN_TEST(test_device_address_zero_returns_param_error);
    /* UT-MB-012 */ RUN_TEST(test_register_count_exceeds_maximum);

    return UNITY_END();
}
