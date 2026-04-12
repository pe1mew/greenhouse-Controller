/**
 * @file test_modbus_hw_loopback.cpp
 * @brief Modbus RTU hardware loopback tests — LIB-6 / test2
 *
 * Verifies every signal on the RS485 bus path by wiring spare GPIO pins as
 * a two-UART loopback instrument on the LOLIN S3 board.  No RS485 transceiver
 * or real slave device is required.
 *
 * UART roles
 * ──────────
 *   Serial1  (GPIO 17 TX / GPIO 18 RX)  Modbus RTU driver under test
 *   Serial2  (GPIO 21 TX / GPIO 38 RX)  Loopback instrument
 *     RX sniffs every byte the driver sends on the RS485 bus.
 *     TX injects the slave response back onto the driver's RX line.
 *
 * DE/RE monitor
 * ─────────────
 *   GPIO 16 is configured as a digital input and wired to GPIO 8 (DE/RE).
 *   This lets the test observe the RS485 direction-control signal independently
 *   of the driver, confirming it is asserted and deasserted at the right times.
 *
 * ┌──────────────────────────────────────────────────────────────────────────┐
 * │  REQUIRED JUMPER WIRES (fit before running pio test -e lolin_s3_loopback) │
 * │                                                                          │
 * │  Signal           Driver pin    Instrument pin  Direction               │
 * │  ─────────────────────────────────────────────────────────────────────  │
 * │  MODBUS TX        GPIO 17   ──► GPIO 38         Serial2 RX (sniff)      │
 * │  MODBUS RX        GPIO 18  ◄──  GPIO 21         Serial2 TX (inject)     │
 * │  RS485 DE/RE      GPIO  8   ──► GPIO 16         GPIO input (monitor)    │
 * │                                                                          │
 * │  GPIO 16 = PIN_RELAY_M3_OPEN  in the main firmware — not initialised    │
 * │  GPIO 21 = PIN_RELAY_M3_CLOSE in the main firmware — not initialised    │
 * │  GPIO 38 = unassigned in the main firmware                              │
 * │  All three are safe to repurpose in this standalone test.               │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * Run with:  pio test -e lolin_s3_loopback
 *
 * Test IDs: HW-MB-001 … HW-MB-011
 *
 * @author  Greenhouse Controller project
 * @version 0.1.0
 */

#include <Arduino.h>
#include <unity.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include "modbus_rtu.h"   /* driver under test */
#include "gpio_util.h"    /* gpio_set_rs485_direction() */
#include "pin_config.h"   /* PIN_RS485_DE_RE, pin definitions */

/* ===========================================================================
 * Loopback instrument pin assignments (see wiring table in the file header)
 * =========================================================================== */

/** UART2 RX — sniffs bytes on the MODBUS TX line (wire from GPIO 17). */
#define TEST_PIN_SNIFF_RX    38

/** UART2 TX — injects response bytes onto the MODBUS RX line (wire to GPIO 18). */
#define TEST_PIN_INJECT_TX   21

/** GPIO input — monitors the RS485 DE/RE direction signal (wire from GPIO 8). */
#define TEST_PIN_DERE_MON    16

/** Loopback baud rate must equal MODBUS_BAUD so UART2 decodes every bit. */
#define TEST_LOOPBACK_BAUD   MODBUS_BAUD

/* ===========================================================================
 * Shared context between the main test task and the responder task
 * =========================================================================== */

/**
 * @brief Everything the responder task captures and the test task configures.
 *
 * The main task fills resp_frame / resp_len / inject_response before creating
 * the responder task, then reads the captured fields after the transaction.
 */
typedef struct {
    /* ---- TX-side observations ------------------------------------------ */
    uint8_t  tx_frame[8];       /**< Bytes captured on the MODBUS TX line.   */
    int      tx_count;          /**< Number of bytes actually captured.       */

    /* ---- DE/RE observations -------------------------------------------- */
    bool     dere_idle;         /**< DE/RE state at responder-task entry.     */
    bool     dere_during_tx;    /**< DE/RE when first TX byte was sniffed.    */
    bool     dere_before_resp;  /**< DE/RE state just before response inject. */

    /* ---- Response to inject (set by the test before task creation) ------ */
    uint8_t  resp_frame[256];   /**< Pre-built response frame bytes.          */
    int      resp_len;          /**< Number of bytes in resp_frame.           */
    bool     inject_response;   /**< false → no response (timeout test).      */
} loopback_ctx_t;

static loopback_ctx_t  lb_ctx;
static SemaphoreHandle_t lb_done_sem;   /**< Signals the responder is done.  */

/* ===========================================================================
 * CRC16 helper — local copy, independent of the driver internals
 * =========================================================================== */
static uint16_t hw_crc16(const uint8_t *buf, uint8_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= (uint16_t)buf[i];
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc & 0x0001) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
    return crc;
}

/* ===========================================================================
 * Frame builder — constructs a valid FC03/FC04 normal response
 * =========================================================================== */

/**
 * @brief Fill @p frame with a valid Modbus response and return its length.
 *
 * Frame layout: [addr][fc][byte_cnt][hi0][lo0]…[crc_lo][crc_hi]
 *
 * @param frame  Destination buffer (must be ≥ count*2+5 bytes).
 * @param addr   Slave address.
 * @param fc     Function code (0x03 or 0x04).
 * @param vals   Register values (big-endian in the frame).
 * @param count  Number of registers.
 * @return       Total frame length in bytes.
 */
static int build_response(uint8_t *frame, uint8_t addr, uint8_t fc,
                           const uint16_t *vals, uint8_t count)
{
    uint8_t n = 0;
    frame[n++] = addr;
    frame[n++] = fc;
    frame[n++] = (uint8_t)(count * 2);
    for (uint8_t i = 0; i < count; i++) {
        frame[n++] = (uint8_t)(vals[i] >> 8);
        frame[n++] = (uint8_t)(vals[i] & 0xFF);
    }
    uint16_t crc = hw_crc16(frame, n);
    frame[n++] = (uint8_t)(crc & 0xFF);   /* CRC low byte first */
    frame[n++] = (uint8_t)(crc >> 8);
    return (int)n;
}

/* ===========================================================================
 * Responder task
 *
 * Runs on core 0 while the Modbus driver runs on core 1 (Arduino loop core).
 *
 * Sequence
 * ────────
 *  1. Sample DE/RE at entry — must be LOW (receive mode / driver idle).
 *  2. Wait for the first byte from UART2 RX (driver has started transmitting).
 *  3. Record DE/RE — must be HIGH (driver asserted transmit enable).
 *  4. Drain all 8 request bytes from UART2 RX.
 *  5. If inject_response:
 *       a. Poll until DE/RE goes LOW (driver finished TX and switched to RX).
 *       b. Record DE/RE state — must be LOW.
 *       c. Send response via UART2 TX into the driver's RX line.
 *  6. Give lb_done_sem so run_with_responder() can proceed.
 * =========================================================================== */
static void responder_task(void *pvParam)
{
    loopback_ctx_t *ctx = (loopback_ctx_t *)pvParam;

    /* Step 1 — DE/RE at task entry (should be LOW: driver is in receive mode) */
    ctx->dere_idle     = (digitalRead(TEST_PIN_DERE_MON) == LOW);
    ctx->dere_during_tx = false;
    ctx->dere_before_resp = false;

    /* Step 2 — wait for first TX byte (500 ms safety limit) */
    uint32_t deadline = millis() + 500UL;
    while (Serial2.available() == 0 && millis() < deadline)
        vTaskDelay(pdMS_TO_TICKS(1));

    /* Step 3 — DE/RE when the first byte is seen on the bus */
    ctx->dere_during_tx = (digitalRead(TEST_PIN_DERE_MON) == HIGH);

    /* Step 4 — collect all 8 request bytes (50 ms >> 8 chars at 9600 baud) */
    ctx->tx_count = 0;
    deadline = millis() + 50UL;
    while (ctx->tx_count < 8 && millis() < deadline) {
        if (Serial2.available())
            ctx->tx_frame[ctx->tx_count++] = (uint8_t)Serial2.read();
        else
            vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (ctx->inject_response) {
        /* Step 5a — wait for driver to deassert DE/RE (receive window opens) */
        deadline = millis() + 500UL;
        while (digitalRead(TEST_PIN_DERE_MON) == HIGH && millis() < deadline)
            vTaskDelay(pdMS_TO_TICKS(1));

        /* Step 5b — record DE/RE state right before injection */
        ctx->dere_before_resp = (digitalRead(TEST_PIN_DERE_MON) == LOW);

        /* Small gap to let the driver's receive loop start. */
        vTaskDelay(pdMS_TO_TICKS(5));

        /* Step 5c — inject response onto MODBUS RX line via UART2 TX */
        Serial2.write(ctx->resp_frame, (size_t)ctx->resp_len);
        Serial2.flush();
    }

    /* Step 6 — signal completion */
    xSemaphoreGive(lb_done_sem);
    vTaskDelete(NULL);
}

/* ===========================================================================
 * Test-runner helper
 *
 * Spawns the responder task on core 0, calls the driver function (blocking,
 * core 1), then waits for the responder to finish.
 * =========================================================================== */
static modbus_status_t run_with_responder(uint8_t  addr,
                                          bool     fc03,
                                          uint16_t reg,
                                          uint8_t  count,
                                          uint16_t *out)
{
    /* Clear any stale semaphore token */
    xSemaphoreTake(lb_done_sem, 0);

    xTaskCreatePinnedToCore(responder_task,
                            "mb_responder",
                            4096,
                            &lb_ctx,
                            5,      /* higher than loop() priority */
                            NULL,
                            0);     /* core 0 */

    modbus_status_t status;
    if (fc03)
        status = modbus_read_holding_registers(addr, reg, count, out);
    else
        status = modbus_read_input_registers(addr, reg, count, out);

    /* Wait for responder to signal completion (1 s hard deadline) */
    xSemaphoreTake(lb_done_sem, pdMS_TO_TICKS(1000));

    return status;
}

/* ===========================================================================
 * Unity fixtures
 * =========================================================================== */
void setUp(void)
{
    /* Drain any stale bytes in the instrument UART */
    while (Serial2.available())
        (void)Serial2.read();

    memset(&lb_ctx, 0, sizeof(lb_ctx));
    lb_ctx.inject_response = false;

    /* Re-initialise the driver before each test */
    modbus_init();
}

void tearDown(void) {}

/* ===========================================================================
 * HW-MB-001  DE/RE LOW after modbus_init() — driver starts in receive mode
 *
 * modbus_init() calls gpio_set_rs485_direction(false) → GPIO 8 LOW.
 * The monitor pin (GPIO 16), wired to GPIO 8, must read LOW.
 * =========================================================================== */
void test_dere_low_after_init(void)
{
    /* modbus_init() was already called in setUp() */
    int level = digitalRead(TEST_PIN_DERE_MON);
    TEST_ASSERT_EQUAL_INT_MESSAGE(LOW, level,
        "DE/RE was not LOW after modbus_init() — "
        "check GPIO 8 → GPIO 16 wire and that GPIO 8 is OUTPUT");
}

/* ===========================================================================
 * HW-MB-002  DE/RE HIGH while driver is transmitting
 *
 * The driver asserts DE/RE HIGH before the first Serial1.write().
 * The responder task samples GPIO 16 the moment the first byte appears on
 * UART2 RX (= GPIO 38, wired to GPIO 17 MODBUS TX).
 * =========================================================================== */
void test_dere_high_during_tx(void)
{
    const uint16_t vals[2] = {0x0000, 0x0000};
    lb_ctx.resp_len = build_response(lb_ctx.resp_frame, 1, 0x03, vals, 2);
    lb_ctx.inject_response = true;

    uint16_t out[2] = {0};
    run_with_responder(1, true, 0x0000, 2, out);

    TEST_ASSERT_TRUE_MESSAGE(lb_ctx.dere_during_tx,
        "DE/RE was not HIGH when first TX byte appeared on GPIO 17 — "
        "driver failed to assert transmit-enable before writing to UART");
}

/* ===========================================================================
 * HW-MB-003  TX frame content correct — FC03, addr=1, reg=0x0000, count=2
 *
 * Expected 8 bytes: [0x01][0x03][0x00][0x00][0x00][0x02][0xC4][0x0B]
 *   CRC of first 6 bytes = 0x0BC4 → transmitted as lo=0xC4, hi=0x0B
 * =========================================================================== */
void test_tx_frame_fc03_correct(void)
{
    const uint16_t vals[2] = {0x0000, 0x0000};
    lb_ctx.resp_len = build_response(lb_ctx.resp_frame, 1, 0x03, vals, 2);
    lb_ctx.inject_response = true;

    uint16_t out[2] = {0};
    run_with_responder(1, true, 0x0000, 2, out);

    TEST_ASSERT_EQUAL_INT_MESSAGE(8, lb_ctx.tx_count,
        "Did not capture 8 bytes on GPIO 17 — check GPIO 17 → GPIO 38 wire");

    TEST_ASSERT_EQUAL_HEX8(0x01, lb_ctx.tx_frame[0]);  /* device address     */
    TEST_ASSERT_EQUAL_HEX8(0x03, lb_ctx.tx_frame[1]);  /* function code FC03 */
    TEST_ASSERT_EQUAL_HEX8(0x00, lb_ctx.tx_frame[2]);  /* register high byte */
    TEST_ASSERT_EQUAL_HEX8(0x00, lb_ctx.tx_frame[3]);  /* register low byte  */
    TEST_ASSERT_EQUAL_HEX8(0x00, lb_ctx.tx_frame[4]);  /* count high byte    */
    TEST_ASSERT_EQUAL_HEX8(0x02, lb_ctx.tx_frame[5]);  /* count = 2          */
    TEST_ASSERT_EQUAL_HEX8(0xC4, lb_ctx.tx_frame[6]);  /* CRC low byte       */
    TEST_ASSERT_EQUAL_HEX8(0x0B, lb_ctx.tx_frame[7]);  /* CRC high byte      */
}

/* ===========================================================================
 * HW-MB-004  TX frame function code is 0x04 for FC04 request
 * =========================================================================== */
void test_tx_frame_fc04_function_code(void)
{
    const uint16_t vals[1] = {0xBEEF};
    lb_ctx.resp_len = build_response(lb_ctx.resp_frame, 2, 0x04, vals, 1);
    lb_ctx.inject_response = true;

    uint16_t out[1] = {0};
    run_with_responder(2, false, 0x0010, 1, out);

    TEST_ASSERT_EQUAL_INT_MESSAGE(8, lb_ctx.tx_count,
        "Did not capture 8 bytes on the TX line");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x04, lb_ctx.tx_frame[1],
        "Frame byte[1] is not 0x04 — driver used wrong function code for FC04");
}

/* ===========================================================================
 * HW-MB-005  TX frame CRC16 is valid per Modbus CRC16 (polynomial 0xA001)
 *
 * Recomputes the CRC over frame bytes [0..5] and compares to the CRC
 * appended by the driver at bytes [6] (lo) and [7] (hi).
 * =========================================================================== */
void test_tx_frame_crc_valid(void)
{
    const uint16_t vals[2] = {0x0000, 0x0000};
    lb_ctx.resp_len = build_response(lb_ctx.resp_frame, 1, 0x03, vals, 2);
    lb_ctx.inject_response = true;

    uint16_t out[2] = {0};
    run_with_responder(1, true, 0x0000, 2, out);

    TEST_ASSERT_EQUAL_INT_MESSAGE(8, lb_ctx.tx_count, "TX frame incomplete");

    uint16_t computed = hw_crc16(lb_ctx.tx_frame, 6);
    uint16_t in_frame = (uint16_t)lb_ctx.tx_frame[6]
                      | ((uint16_t)lb_ctx.tx_frame[7] << 8);

    TEST_ASSERT_EQUAL_HEX16_MESSAGE(computed, in_frame,
        "CRC16 in transmitted frame is incorrect");
}

/* ===========================================================================
 * HW-MB-006  DE/RE LOW before response bytes arrive on MODBUS RX line
 *
 * The driver must deassert DE/RE (enable the RS485 receiver) before any
 * response byte is placed on the bus.  The responder task confirms GPIO 8
 * is LOW the instant before it injects the response.
 * =========================================================================== */
void test_dere_low_before_response_injected(void)
{
    const uint16_t vals[2] = {0x0001, 0x0002};
    lb_ctx.resp_len = build_response(lb_ctx.resp_frame, 1, 0x03, vals, 2);
    lb_ctx.inject_response = true;

    uint16_t out[2] = {0};
    run_with_responder(1, true, 0x0000, 2, out);

    TEST_ASSERT_TRUE_MESSAGE(lb_ctx.dere_before_resp,
        "DE/RE was not LOW when the response window opened — "
        "driver did not deassert transmit-enable before listening");
}

/* ===========================================================================
 * HW-MB-007  Full FC03 round-trip: MODBUS_OK and correct register values
 * =========================================================================== */
void test_fc03_roundtrip_returns_ok_and_values(void)
{
    const uint16_t vals[2] = {0x1234, 0x5678};
    lb_ctx.resp_len = build_response(lb_ctx.resp_frame, 1, 0x03, vals, 2);
    lb_ctx.inject_response = true;

    uint16_t out[2] = {0, 0};
    modbus_status_t status = run_with_responder(1, true, 0x0000, 2, out);

    TEST_ASSERT_EQUAL_INT(MODBUS_OK, status);
    TEST_ASSERT_EQUAL_HEX16(0x1234, out[0]);
    TEST_ASSERT_EQUAL_HEX16(0x5678, out[1]);
}

/* ===========================================================================
 * HW-MB-008  Full FC04 round-trip: MODBUS_OK and correct register values
 * =========================================================================== */
void test_fc04_roundtrip_returns_ok_and_values(void)
{
    const uint16_t vals[3] = {0xDEAD, 0xBEEF, 0xCAFE};
    lb_ctx.resp_len = build_response(lb_ctx.resp_frame, 3, 0x04, vals, 3);
    lb_ctx.inject_response = true;

    uint16_t out[3] = {0, 0, 0};
    modbus_status_t status = run_with_responder(3, false, 0x0100, 3, out);

    TEST_ASSERT_EQUAL_INT(MODBUS_OK, status);
    TEST_ASSERT_EQUAL_HEX16(0xDEAD, out[0]);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, out[1]);
    TEST_ASSERT_EQUAL_HEX16(0xCAFE, out[2]);
}

/* ===========================================================================
 * HW-MB-009  No response from slave → MODBUS_ERR_TIMEOUT
 *
 * The responder task captures the TX frame but injects nothing.
 * The driver must return MODBUS_ERR_TIMEOUT after MODBUS_TIMEOUT_MS.
 * =========================================================================== */
void test_no_response_returns_timeout(void)
{
    lb_ctx.inject_response = false;   /* capture only — do not inject */

    uint16_t out[2] = {0};
    modbus_status_t status = run_with_responder(1, true, 0x0000, 2, out);

    TEST_ASSERT_EQUAL_INT(MODBUS_ERR_TIMEOUT, status);
}

/* ===========================================================================
 * HW-MB-010  Response with a flipped CRC byte → MODBUS_ERR_CRC
 * =========================================================================== */
void test_corrupt_crc_response_returns_crc_error(void)
{
    const uint16_t vals[2] = {0x1111, 0x2222};
    lb_ctx.resp_len = build_response(lb_ctx.resp_frame, 1, 0x03, vals, 2);

    /* Corrupt the CRC high byte (last byte of the frame) */
    lb_ctx.resp_frame[lb_ctx.resp_len - 1] ^= 0xFF;
    lb_ctx.inject_response = true;

    uint16_t out[2] = {0};
    modbus_status_t status = run_with_responder(1, true, 0x0000, 2, out);

    TEST_ASSERT_EQUAL_INT(MODBUS_ERR_CRC, status);
}

/* ===========================================================================
 * HW-MB-011  Exception response (FC | 0x80) → MODBUS_ERR_EXCEPTION
 *
 * Exception frame: [addr][FC|0x80][exc_code][crc_lo][crc_hi]  (5 bytes)
 * =========================================================================== */
void test_exception_response_returns_exception_error(void)
{
    uint8_t *f = lb_ctx.resp_frame;
    f[0] = 0x01;           /* slave address */
    f[1] = 0x03 | 0x80;    /* FC03 exception: bit 7 set */
    f[2] = 0x02;           /* exception code: Illegal Data Address */
    uint16_t crc = hw_crc16(f, 3);
    f[3] = (uint8_t)(crc & 0xFF);
    f[4] = (uint8_t)(crc >> 8);
    lb_ctx.resp_len       = 5;
    lb_ctx.inject_response = true;

    uint16_t out[2] = {0};
    modbus_status_t status = run_with_responder(1, true, 0x0000, 2, out);

    TEST_ASSERT_EQUAL_INT(MODBUS_ERR_EXCEPTION, status);
}

/* ===========================================================================
 * Arduino entry point — Unity test runner
 * =========================================================================== */
void setup(void)
{
    /* With ARDUINO_USB_CDC_ON_BOOT=0, Serial maps to hardware UART0
     * (GPIO 43 TX / 44 RX = COM3), the same port esptool uses to flash.
     * pio test will find it automatically without CDC re-enumeration.  */
    Serial.begin(115200);
    /* Hardware UART0 is available immediately after upload — 2 s is enough
     * for pio test to open the port and start reading before Unity output. */
    delay(2000);

    /*
     * DE/RE monitor: GPIO 16 as a plain digital input (no pull — the driver
     * actively drives GPIO 8, so a pull is neither needed nor useful).
     */
    pinMode(TEST_PIN_DERE_MON, INPUT);

    /*
     * Serial2: loopback instrument UART.
     *   RX = GPIO 38  — sniffs the MODBUS TX line (wired to GPIO 17)
     *   TX = GPIO 21  — injects responses onto the MODBUS RX line (wired to GPIO 18)
     */
    Serial2.begin(TEST_LOOPBACK_BAUD, SERIAL_8N1,
                  TEST_PIN_SNIFF_RX, TEST_PIN_INJECT_TX);

    lb_done_sem = xSemaphoreCreateBinary();

    UNITY_BEGIN();

    /* Signal-direction tests (no full round-trip) */
    RUN_TEST(test_dere_low_after_init);              /* HW-MB-001 */
    RUN_TEST(test_dere_high_during_tx);              /* HW-MB-002 */
    RUN_TEST(test_dere_low_before_response_injected);/* HW-MB-006 */

    /* TX frame content and CRC tests */
    RUN_TEST(test_tx_frame_fc03_correct);            /* HW-MB-003 */
    RUN_TEST(test_tx_frame_fc04_function_code);      /* HW-MB-004 */
    RUN_TEST(test_tx_frame_crc_valid);               /* HW-MB-005 */

    /* Full round-trip tests */
    RUN_TEST(test_fc03_roundtrip_returns_ok_and_values); /* HW-MB-007 */
    RUN_TEST(test_fc04_roundtrip_returns_ok_and_values); /* HW-MB-008 */

    /* Error-path tests */
    RUN_TEST(test_no_response_returns_timeout);          /* HW-MB-009 */
    RUN_TEST(test_corrupt_crc_response_returns_crc_error); /* HW-MB-010 */
    RUN_TEST(test_exception_response_returns_exception_error); /* HW-MB-011 */

    UNITY_END();
}

void loop(void) {}
