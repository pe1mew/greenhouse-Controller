/**
 * LIB-1 GPIO Utility — unit tests (native build)
 *
 * Test IDs: UT-GPIO-001 … UT-GPIO-011
 *
 * gpio_util.cpp is compiled unchanged against a stand-in for ESP-IDF's
 * driver/gpio.h (test/mock_idf/). mock_gpio.cpp simulates the pads behind
 * it, so these tests check two things: the configuration ESP-IDF receives,
 * and the level the pin would really carry. mock_gpio.h lists what the
 * model covers.
 *
 * Run with:  pio test -e native
 */

#include <unity.h>
#include <stdio.h>
#include "../src/gpio_util.h"
#include "mock_gpio.h"

void setUp(void)
{
    mock_gpio_reset();
}

void tearDown(void) {}

/* Every gpio_set_pin_mode() call must hand ESP-IDF one pad, no interrupt
 * and no pull-down; only mode and pull-up vary. */
static void assert_pad_config(uint8_t pin, gpio_mode_t mode, gpio_pullup_t pull_up)
{
    const mock_gpio_pad_t *p = mock_gpio_pad(pin);
    TEST_ASSERT_TRUE_MESSAGE(p->configured, "gpio_config() never reached this pad");
    TEST_ASSERT_TRUE_MESSAGE(p->last_config.pin_bit_mask == (1ULL << pin),
                             "pin_bit_mask must select exactly this pin");
    TEST_ASSERT_EQUAL_INT(mode, p->last_config.mode);
    TEST_ASSERT_EQUAL_INT(pull_up, p->last_config.pull_up_en);
    TEST_ASSERT_EQUAL_INT(GPIO_PULLDOWN_DISABLE, p->last_config.pull_down_en);
    TEST_ASSERT_EQUAL_INT(GPIO_INTR_DISABLE, p->last_config.intr_type);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mock_gpio_config_rejected,
                                  "ESP-IDF refused a gpio_config() call");
}

/* -------------------------------------------------------------------------
 * UT-GPIO-001 — gpio_set_pin_mode hands ESP-IDF the matching pad config
 * ------------------------------------------------------------------------- */
void test_set_pin_mode_output(void)
{
    gpio_set_pin_mode(PIN_RELAY_M1_OPEN, GPIO_OUTPUT);

    /* INPUT_OUTPUT, not OUTPUT: with the input buffer off, gpio_get_level()
     * reads 0 and gpio_read()/gpio_toggle() cannot see the output latch. */
    assert_pad_config(PIN_RELAY_M1_OPEN, GPIO_MODE_INPUT_OUTPUT, GPIO_PULLUP_DISABLE);
    TEST_ASSERT_EQUAL_INT(1, mock_gpio_config_calls);
}

void test_set_pin_mode_input(void)
{
    gpio_set_pin_mode(PIN_OPTO_INPUT, GPIO_INPUT);

    assert_pad_config(PIN_OPTO_INPUT, GPIO_MODE_INPUT, GPIO_PULLUP_DISABLE);
    TEST_ASSERT_FALSE_MESSAGE(mock_gpio_pad(PIN_OPTO_INPUT)->output_en,
                              "an input must never drive its line");
}

void test_set_pin_mode_input_pullup(void)
{
    gpio_set_pin_mode(KP_COL1, GPIO_INPUT_PULLUP);

    assert_pad_config(KP_COL1, GPIO_MODE_INPUT, GPIO_PULLUP_ENABLE);
    /* Electrically: an open line now reads HIGH instead of floating. */
    TEST_ASSERT_EQUAL(GPIO_HIGH, gpio_read(KP_COL1));
    TEST_ASSERT_EQUAL_INT(0, mock_gpio_floating_reads);
}

/* -------------------------------------------------------------------------
 * UT-GPIO-002 — gpio_write HIGH drives the pad HIGH
 * ------------------------------------------------------------------------- */
void test_gpio_write_high(void)
{
    gpio_set_pin_mode(PIN_RELAY_M1_OPEN, GPIO_OUTPUT);
    gpio_write(PIN_RELAY_M1_OPEN, GPIO_HIGH);

    TEST_ASSERT_EQUAL_INT(1, mock_gpio_pad(PIN_RELAY_M1_OPEN)->out_latch);
    TEST_ASSERT_EQUAL_INT(1, mock_gpio_pad_level(PIN_RELAY_M1_OPEN));
}

/* -------------------------------------------------------------------------
 * UT-GPIO-003 — gpio_write LOW drives a HIGH pad LOW
 * ------------------------------------------------------------------------- */
void test_gpio_write_low(void)
{
    gpio_set_pin_mode(PIN_RELAY_M1_OPEN, GPIO_OUTPUT);
    gpio_write(PIN_RELAY_M1_OPEN, GPIO_HIGH);        /* pre-condition: pin was HIGH */
    TEST_ASSERT_EQUAL_INT(1, mock_gpio_pad_level(PIN_RELAY_M1_OPEN));

    gpio_write(PIN_RELAY_M1_OPEN, GPIO_LOW);

    TEST_ASSERT_EQUAL_INT(0, mock_gpio_pad_level(PIN_RELAY_M1_OPEN));
}

/* -------------------------------------------------------------------------
 * UT-GPIO-004 — gpio_read returns the level the outside world drives
 * ------------------------------------------------------------------------- */
void test_gpio_read_returns_preset_state(void)
{
    gpio_set_pin_mode(PIN_OPTO_INPUT, GPIO_INPUT);

    mock_gpio_drive_external(PIN_OPTO_INPUT, MOCK_EXT_HIGH);
    TEST_ASSERT_EQUAL(GPIO_HIGH, gpio_read(PIN_OPTO_INPUT));

    mock_gpio_drive_external(PIN_OPTO_INPUT, MOCK_EXT_LOW);
    TEST_ASSERT_EQUAL(GPIO_LOW, gpio_read(PIN_OPTO_INPUT));

    TEST_ASSERT_EQUAL_INT(0, mock_gpio_floating_reads);
    TEST_ASSERT_EQUAL_INT(0, mock_gpio_contention);
}

/* -------------------------------------------------------------------------
 * UT-GPIO-005 — gpio_toggle flips HIGH to LOW
 *
 * The discriminating case: gpio_toggle() has to READ the latch back. On a
 * pad configured GPIO_MODE_OUTPUT that read returns 0, the toggle writes
 * HIGH again, and the pin never goes LOW.
 * ------------------------------------------------------------------------- */
void test_gpio_toggle_high_to_low(void)
{
    gpio_set_pin_mode(PIN_HB_LED, GPIO_OUTPUT);
    gpio_write(PIN_HB_LED, GPIO_HIGH);

    gpio_toggle(PIN_HB_LED);

    TEST_ASSERT_EQUAL_INT(0, mock_gpio_pad_level(PIN_HB_LED));
}

/* -------------------------------------------------------------------------
 * UT-GPIO-006 — gpio_toggle flips LOW to HIGH
 * ------------------------------------------------------------------------- */
void test_gpio_toggle_low_to_high(void)
{
    gpio_set_pin_mode(PIN_HB_LED, GPIO_OUTPUT);
    gpio_write(PIN_HB_LED, GPIO_LOW);

    gpio_toggle(PIN_HB_LED);

    TEST_ASSERT_EQUAL_INT(1, mock_gpio_pad_level(PIN_HB_LED));
}

/* -------------------------------------------------------------------------
 * UT-GPIO-007 — gpio_set_rs485_direction(true) drives PIN_RS485_DE_RE HIGH
 * ------------------------------------------------------------------------- */
void test_rs485_direction_transmit(void)
{
    gpio_rs485_init();

    gpio_set_rs485_direction(true);

    TEST_ASSERT_EQUAL_INT(1, mock_gpio_pad_level(PIN_RS485_DE_RE));
}

/* -------------------------------------------------------------------------
 * UT-GPIO-008 — gpio_set_rs485_direction(false) drives PIN_RS485_DE_RE LOW
 * ------------------------------------------------------------------------- */
void test_rs485_direction_receive(void)
{
    gpio_rs485_init();
    gpio_set_rs485_direction(true);                  /* pre-condition: transmitting */

    gpio_set_rs485_direction(false);

    TEST_ASSERT_EQUAL_INT(0, mock_gpio_pad_level(PIN_RS485_DE_RE));
}

/* -------------------------------------------------------------------------
 * Every GPIO assignment in firmware/config/pin_config.h. UT-GPIO-009/010
 * originally checked the first nine; the map has grown since.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t     pin;
    const char *name;
} named_pin_t;

static const named_pin_t k_pins[] = {
    { PIN_RELAY_M1_OPEN,  "PIN_RELAY_M1_OPEN"  },
    { PIN_RELAY_M1_CLOSE, "PIN_RELAY_M1_CLOSE" },
    { PIN_RELAY_M2_OPEN,  "PIN_RELAY_M2_OPEN"  },
    { PIN_RELAY_M2_CLOSE, "PIN_RELAY_M2_CLOSE" },
    { PIN_RELAY_M3_OPEN,  "PIN_RELAY_M3_OPEN"  },
    { PIN_RELAY_M3_CLOSE, "PIN_RELAY_M3_CLOSE" },
    { PIN_OPTO_INPUT,     "PIN_OPTO_INPUT"     },
    { PIN_HB_LED,         "PIN_HB_LED"         },
    { PIN_RGB_LED,        "PIN_RGB_LED"        },
    { PIN_RS485_DE_RE,    "PIN_RS485_DE_RE"    },
    { PIN_RS485_TX,       "PIN_RS485_TX"       },
    { PIN_RS485_RX,       "PIN_RS485_RX"       },
    { PIN_I2C_SDA,        "PIN_I2C_SDA"        },
    { PIN_I2C_SCL,        "PIN_I2C_SCL"        },
    { KP_ROW1,            "KP_ROW1"            },
    { KP_ROW2,            "KP_ROW2"            },
    { KP_ROW3,            "KP_ROW3"            },
    { KP_ROW4,            "KP_ROW4"            },
    { KP_COL1,            "KP_COL1"            },
    { KP_COL2,            "KP_COL2"            },
    { KP_COL3,            "KP_COL3"            },
    { KP_COL4,            "KP_COL4"            },
    { PIN_SD_MOSI,        "PIN_SD_MOSI"        },
    { PIN_SD_MISO,        "PIN_SD_MISO"        },
    { PIN_SD_CLK,         "PIN_SD_CLK"         },
    { PIN_SD_CS,          "PIN_SD_CS"          },
};
static const int k_pin_count = (int)(sizeof(k_pins) / sizeof(k_pins[0]));

/* -------------------------------------------------------------------------
 * UT-GPIO-009 — no two pin constants share a GPIO number
 * ------------------------------------------------------------------------- */
void test_pin_constants_are_unique(void)
{
    char msg[96];
    for (int i = 0; i < k_pin_count; i++) {
        for (int j = i + 1; j < k_pin_count; j++) {
            snprintf(msg, sizeof(msg), "%s and %s are both GPIO %u",
                     k_pins[i].name, k_pins[j].name, (unsigned)k_pins[i].pin);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(k_pins[i].pin, k_pins[j].pin, msg);
        }
    }
}

/* -------------------------------------------------------------------------
 * UT-GPIO-010 — no pin constant uses a reserved or missing ESP32-S3 GPIO
 *
 * Reserved list as pin_config.h states it: 0, 19, 20, 22–25 (absent on
 * the S3), 26–37, 43–46.
 * ------------------------------------------------------------------------- */
void test_no_pin_in_reserved_set(void)
{
    static const uint8_t reserved[] = {
        0, 19, 20,
        22, 23, 24, 25,
        26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37,
        43, 44, 45, 46
    };
    const int reserved_count = (int)(sizeof(reserved) / sizeof(reserved[0]));

    char msg[96];
    for (int i = 0; i < k_pin_count; i++) {
        snprintf(msg, sizeof(msg), "%s uses GPIO %u",
                 k_pins[i].name, (unsigned)k_pins[i].pin);
        TEST_ASSERT_TRUE_MESSAGE(k_pins[i].pin < SOC_GPIO_PIN_COUNT, msg);
        for (int r = 0; r < reserved_count; r++) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(reserved[r], k_pins[i].pin, msg);
        }
    }
}

/* -------------------------------------------------------------------------
 * UT-GPIO-011 — gpio_rs485_init leaves the transceiver receiving
 *
 * gpio_util.h: an output, driven LOW, "the safe idle state on the
 * differential bus".
 * ------------------------------------------------------------------------- */
void test_rs485_init_idles_in_receive(void)
{
    gpio_rs485_init();

    assert_pad_config(PIN_RS485_DE_RE, GPIO_MODE_INPUT_OUTPUT, GPIO_PULLUP_DISABLE);
    TEST_ASSERT_EQUAL_INT(0, mock_gpio_pad_level(PIN_RS485_DE_RE));
    TEST_ASSERT_EQUAL(GPIO_LOW, gpio_read(PIN_RS485_DE_RE));
}

/* -------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

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

    /* UT-GPIO-011 */
    RUN_TEST(test_rs485_init_idles_in_receive);

    return UNITY_END();
}
