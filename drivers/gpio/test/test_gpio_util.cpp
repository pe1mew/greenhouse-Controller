/**
 * LIB-1 GPIO Utility — unit tests (native build)
 *
 * Test IDs: UT-GPIO-001 … UT-GPIO-010
 *
 * Run with:  pio test -e native
 */

#include <unity.h>
#include "../src/gpio_util.h"
#include "mock_gpio.h"

void setUp(void)
{
    mock_gpio_reset();
}

void tearDown(void) {}

/* -------------------------------------------------------------------------
 * UT-GPIO-001 — gpio_set_pin_mode stores the mode in the mock
 * ------------------------------------------------------------------------- */
void test_set_pin_mode_output(void)
{
    gpio_set_pin_mode(12, GPIO_OUTPUT);
    TEST_ASSERT_EQUAL_UINT8(OUTPUT, pin_mode_arr[12]);
}

void test_set_pin_mode_input(void)
{
    gpio_set_pin_mode(42, GPIO_INPUT);
    TEST_ASSERT_EQUAL_UINT8(INPUT, pin_mode_arr[42]);
}

void test_set_pin_mode_input_pullup(void)
{
    gpio_set_pin_mode(23, GPIO_INPUT_PULLUP);
    TEST_ASSERT_EQUAL_UINT8(INPUT_PULLUP, pin_mode_arr[23]);
}

/* -------------------------------------------------------------------------
 * UT-GPIO-002 — gpio_write HIGH sets pin_state to HIGH
 * ------------------------------------------------------------------------- */
void test_gpio_write_high(void)
{
    gpio_write(12, GPIO_HIGH);
    TEST_ASSERT_EQUAL_UINT8(HIGH, pin_state[12]);
}

/* -------------------------------------------------------------------------
 * UT-GPIO-003 — gpio_write LOW sets pin_state to LOW
 * ------------------------------------------------------------------------- */
void test_gpio_write_low(void)
{
    pin_state[12] = HIGH; /* pre-condition: pin was HIGH */
    gpio_write(12, GPIO_LOW);
    TEST_ASSERT_EQUAL_UINT8(LOW, pin_state[12]);
}

/* -------------------------------------------------------------------------
 * UT-GPIO-004 — gpio_read returns the preset mock state
 * ------------------------------------------------------------------------- */
void test_gpio_read_returns_preset_state(void)
{
    pin_state[42] = HIGH;
    TEST_ASSERT_EQUAL(GPIO_HIGH, gpio_read(42));

    pin_state[42] = LOW;
    TEST_ASSERT_EQUAL(GPIO_LOW, gpio_read(42));
}

/* -------------------------------------------------------------------------
 * UT-GPIO-005 — gpio_toggle flips HIGH to LOW
 * ------------------------------------------------------------------------- */
void test_gpio_toggle_high_to_low(void)
{
    pin_state[41] = HIGH;
    gpio_toggle(41);
    TEST_ASSERT_EQUAL_UINT8(LOW, pin_state[41]);
}

/* -------------------------------------------------------------------------
 * UT-GPIO-006 — gpio_toggle flips LOW to HIGH
 * ------------------------------------------------------------------------- */
void test_gpio_toggle_low_to_high(void)
{
    pin_state[41] = LOW;
    gpio_toggle(41);
    TEST_ASSERT_EQUAL_UINT8(HIGH, pin_state[41]);
}

/* -------------------------------------------------------------------------
 * UT-GPIO-007 — gpio_set_rs485_direction(true) drives PIN_RS485_DE_RE HIGH
 * ------------------------------------------------------------------------- */
void test_rs485_direction_transmit(void)
{
    gpio_set_rs485_direction(true);
    TEST_ASSERT_EQUAL_UINT8(HIGH, pin_state[PIN_RS485_DE_RE]);
}

/* -------------------------------------------------------------------------
 * UT-GPIO-008 — gpio_set_rs485_direction(false) drives PIN_RS485_DE_RE LOW
 * ------------------------------------------------------------------------- */
void test_rs485_direction_receive(void)
{
    pin_state[PIN_RS485_DE_RE] = HIGH; /* pre-condition */
    gpio_set_rs485_direction(false);
    TEST_ASSERT_EQUAL_UINT8(LOW, pin_state[PIN_RS485_DE_RE]);
}

/* -------------------------------------------------------------------------
 * UT-GPIO-009 — all 9 pin constants are unique GPIO numbers
 * ------------------------------------------------------------------------- */
void test_pin_constants_are_unique(void)
{
    const uint8_t pins[9] = {
        PIN_RELAY_M1_OPEN,
        PIN_RELAY_M1_CLOSE,
        PIN_RELAY_M2_OPEN,
        PIN_RELAY_M2_CLOSE,
        PIN_RELAY_M3_OPEN,
        PIN_RELAY_M3_CLOSE,
        PIN_OPTO_INPUT,
        PIN_HB_LED,
        PIN_RS485_DE_RE
    };

    for (int i = 0; i < 9; i++) {
        for (int j = i + 1; j < 9; j++) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(pins[i], pins[j],
                "Two pin constants share the same GPIO number");
        }
    }
}

/* -------------------------------------------------------------------------
 * UT-GPIO-010 — no defined pin falls in the ESP32-S3 reserved set
 *   Reserved: {0, 19, 20, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37,
 *              43, 44, 45, 46}
 * ------------------------------------------------------------------------- */
void test_no_pin_in_reserved_set(void)
{
    const uint8_t reserved[] = {
        0, 19, 20,
        26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37,
        43, 44, 45, 46
    };
    const uint8_t reserved_count = sizeof(reserved) / sizeof(reserved[0]);

    const uint8_t pins[9] = {
        PIN_RELAY_M1_OPEN,
        PIN_RELAY_M1_CLOSE,
        PIN_RELAY_M2_OPEN,
        PIN_RELAY_M2_CLOSE,
        PIN_RELAY_M3_OPEN,
        PIN_RELAY_M3_CLOSE,
        PIN_OPTO_INPUT,
        PIN_HB_LED,
        PIN_RS485_DE_RE
    };

    for (int i = 0; i < 9; i++) {
        for (int r = 0; r < reserved_count; r++) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(reserved[r], pins[i],
                "A pin constant uses a reserved ESP32-S3 GPIO");
        }
    }
}

/* -------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    UNITY_BEGIN();

    /* UT-GPIO-001 */
    RUN_TEST(test_set_pin_mode_output);
    RUN_TEST(test_set_pin_mode_input);
    RUN_TEST(test_set_pin_mode_input_pullup);

    /* UT-GPIO-002 */
    RUN_TEST(test_gpio_write_high);

    /* UT-GPIO-003 */
    RUN_TEST(test_gpio_write_low);

    /* UT-GPIO-004 */
    RUN_TEST(test_gpio_read_returns_preset_state);

    /* UT-GPIO-005 */
    RUN_TEST(test_gpio_toggle_high_to_low);

    /* UT-GPIO-006 */
    RUN_TEST(test_gpio_toggle_low_to_high);

    /* UT-GPIO-007 */
    RUN_TEST(test_rs485_direction_transmit);

    /* UT-GPIO-008 */
    RUN_TEST(test_rs485_direction_receive);

    /* UT-GPIO-009 */
    RUN_TEST(test_pin_constants_are_unique);

    /* UT-GPIO-010 */
    RUN_TEST(test_no_pin_in_reserved_set);

    return UNITY_END();
}
