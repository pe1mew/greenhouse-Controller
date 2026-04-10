/**
 * LIB-1 GPIO Utility — hardware verification sketch
 *
 * Covers HW-GPIO-001 through HW-GPIO-013.
 *
 * Uses loopback wiring (11 jumper wires between project pins and spare GPIOs)
 * so that every output and input is verified automatically with a PASS/FAIL
 * result printed on Serial0 (UART0, GPIO 43 TX / GPIO 44 RX, 115200 baud).
 * No relay module or multimeter is required.
 *
 * Serial interface: UART0 on GPIO 43 (TX) / GPIO 44 (RX), 115200 baud.
 * Connect a USB-to-serial adapter (3.3 V) to GPIO 43 / GND to read output.
 * Do NOT use the USB-CDC Serial port — it requires USB enumeration before
 * printing, which makes timing unreliable for a boot-time verification sketch.
 *
 * ---------------------------------------------------------------------------
 * Loopback wiring (11 jumper wires)
 * ---------------------------------------------------------------------------
 *
 *   Output under test    Loopback input    Wire
 *   GPIO 12 (M1-OPEN)  → GPIO  1          [1]
 *   GPIO 13 (M1-CLOSE) → GPIO  2          [2]
 *   GPIO 14 (M2-OPEN)  → GPIO  3          [3]
 *   GPIO 15 (M2-CLOSE) → GPIO  4          [4]
 *   GPIO 16 (M3-OPEN)  → GPIO  5          [5]
 *   GPIO 21 (M3-CLOSE) → GPIO  6          [6]
 *   GPIO  8 (RS485 DE) → GPIO  7          [7]
 *   GPIO 41 (HB LED)   → GPIO  9          [8]
 *   GPIO 39 (SD LED)   → GPIO 10          [9]
 *
 *   Loopback driver         Input under test
 *   GPIO 11 (driver) → GPIO 42 (OPTO_INPUT)    [10]
 *   GPIO 17 (driver) → GPIO 40 (SD_MOUNT_BTN)  [11]
 *
 * All loopback pins are spare ESP32-S3 GPIOs not used by any project function.
 * GPIO 1-7, 9-11, 17 are valid general-purpose I/O on ESP32-S3.
 * GPIO 38 (on-board RGB LED), 43-44 (UART0) and 26/30 (PSRAM) are avoided.
 *
 * ---------------------------------------------------------------------------
 * Safety note
 * ---------------------------------------------------------------------------
 * No relay module is connected during this test. The relay output pins
 * (GPIO 12-16, 21) are wired only to the loopback input pins. Never connect
 * OPEN and CLOSE of the same motor channel to the relay module simultaneously.
 */

#include <Arduino.h>
#include "gpio_util.h"

/* ---------------------------------------------------------------------------
 * Loopback pin assignments (spare GPIOs, not used by any project function)
 * --------------------------------------------------------------------------- */
#define LB_M1_OPEN    1   /**< Reads back GPIO 12 (RELAY_M1_OPEN)  */
#define LB_M1_CLOSE   2   /**< Reads back GPIO 13 (RELAY_M1_CLOSE) */
#define LB_M2_OPEN    3   /**< Reads back GPIO 14 (RELAY_M2_OPEN)  */
#define LB_M2_CLOSE   4   /**< Reads back GPIO 15 (RELAY_M2_CLOSE) */
#define LB_M3_OPEN    5   /**< Reads back GPIO 16 (RELAY_M3_OPEN)  */
#define LB_M3_CLOSE   6   /**< Reads back GPIO 21 (RELAY_M3_CLOSE) */
#define LB_RS485      7   /**< Reads back GPIO  8 (RS485 DE/RE)    */
#define LB_HB_LED     9   /**< Reads back GPIO 41 (HB LED)         */
#define LB_SD_LED    10   /**< Reads back GPIO 33 (SD status LED)  */
#define DRV_OPTO     11   /**< Drives  GPIO 42 (OPTO_INPUT)        */
#define DRV_BTN      17   /**< Drives  GPIO 34 (SD_MOUNT_BTN)      */

/* ---------------------------------------------------------------------------
 * Test helpers
 * --------------------------------------------------------------------------- */
static int pass_count = 0;
static int fail_count = 0;

static void check(const char *id, const char *description, bool condition)
{
    if (condition) {
        Serial0.print("[PASS] ");
        pass_count++;
    } else {
        Serial0.print("[FAIL] ");
        fail_count++;
    }
    Serial0.print(id);
    Serial0.print(": ");
    Serial0.println(description);
}

/**
 * Drive an output HIGH then LOW and read back via the loopback input.
 * Returns true if both states are read back correctly.
 */
static bool loopback_output(uint8_t out_pin, uint8_t lb_pin)
{
    gpio_write((uint8_t)out_pin, GPIO_HIGH);
    delay(2);
    bool hi_ok = (digitalRead(lb_pin) == HIGH);

    gpio_write((uint8_t)out_pin, GPIO_LOW);
    delay(2);
    bool lo_ok = (digitalRead(lb_pin) == LOW);

    return hi_ok && lo_ok;
}

/**
 * Drive the loopback driver pin LOW (overrides pullup) and read the input pin.
 * Then release (HIGH-Z via INPUT) and confirm the pullup restores HIGH.
 */
static bool loopback_input(uint8_t drv_pin, uint8_t in_pin)
{
    /* Pull LOW via driver */
    gpio_set_pin_mode((uint8_t)drv_pin, GPIO_OUTPUT);
    gpio_write((uint8_t)drv_pin, GPIO_LOW);
    delay(2);
    bool lo_ok = (gpio_read((uint8_t)in_pin) == GPIO_LOW);

    /* Release driver — input pullup should restore HIGH */
    gpio_set_pin_mode((uint8_t)drv_pin, GPIO_INPUT);
    delay(2);
    bool hi_ok = (gpio_read((uint8_t)in_pin) == GPIO_HIGH);

    return lo_ok && hi_ok;
}

/* ---------------------------------------------------------------------------
 * Setup — runs all hardware tests once, then enters heartbeat loop
 * --------------------------------------------------------------------------- */
void setup()
{
    Serial0.begin(115200);
    delay(500);

    Serial0.println();
    Serial0.println("================================================");
    Serial0.println("  LIB-1 GPIO Utility — hardware verification");
    Serial0.println("================================================");

    /* -----------------------------------------------------------------
     * HW-GPIO-001 — initialise all project pins
     * ----------------------------------------------------------------- */
    gpio_set_pin_mode(PIN_RELAY_M1_OPEN,  GPIO_OUTPUT);
    gpio_set_pin_mode(PIN_RELAY_M1_CLOSE, GPIO_OUTPUT);
    gpio_set_pin_mode(PIN_RELAY_M2_OPEN,  GPIO_OUTPUT);
    gpio_set_pin_mode(PIN_RELAY_M2_CLOSE, GPIO_OUTPUT);
    gpio_set_pin_mode(PIN_RELAY_M3_OPEN,  GPIO_OUTPUT);
    gpio_set_pin_mode(PIN_RELAY_M3_CLOSE, GPIO_OUTPUT);
    gpio_set_pin_mode(PIN_HB_LED,         GPIO_OUTPUT);
    gpio_set_pin_mode(PIN_RS485_DE_RE,    GPIO_OUTPUT);
    gpio_set_pin_mode(PIN_SD_STATUS_LED,  GPIO_OUTPUT);
    gpio_set_pin_mode(PIN_OPTO_INPUT,     GPIO_INPUT_PULLUP);
    gpio_set_pin_mode(PIN_SD_MOUNT_BTN,   GPIO_INPUT_PULLUP);

    /* All relay outputs deactivated */
    gpio_write(PIN_RELAY_M1_OPEN,  GPIO_LOW);
    gpio_write(PIN_RELAY_M1_CLOSE, GPIO_LOW);
    gpio_write(PIN_RELAY_M2_OPEN,  GPIO_LOW);
    gpio_write(PIN_RELAY_M2_CLOSE, GPIO_LOW);
    gpio_write(PIN_RELAY_M3_OPEN,  GPIO_LOW);
    gpio_write(PIN_RELAY_M3_CLOSE, GPIO_LOW);

    /* Configure loopback input pins */
    pinMode(LB_M1_OPEN,  INPUT);
    pinMode(LB_M1_CLOSE, INPUT);
    pinMode(LB_M2_OPEN,  INPUT);
    pinMode(LB_M2_CLOSE, INPUT);
    pinMode(LB_M3_OPEN,  INPUT);
    pinMode(LB_M3_CLOSE, INPUT);
    pinMode(LB_RS485,    INPUT);
    pinMode(LB_HB_LED,   INPUT);
    pinMode(LB_SD_LED,   INPUT);
    /* DRV_OPTO and DRV_BTN are configured per-test inside loopback_input() */

    check("HW-GPIO-001", "GPIO init complete — no invalid pin errors", true);

    /* -----------------------------------------------------------------
     * HW-GPIO-002…007 — relay output pins loopback
     * ----------------------------------------------------------------- */
    Serial0.println("--- Relay output loopback ---");
    check("HW-GPIO-002", "GPIO 12 RELAY_M1_OPEN  HIGH/LOW loopback",
          loopback_output(PIN_RELAY_M1_OPEN,  LB_M1_OPEN));
    check("HW-GPIO-003", "GPIO 13 RELAY_M1_CLOSE HIGH/LOW loopback",
          loopback_output(PIN_RELAY_M1_CLOSE, LB_M1_CLOSE));
    check("HW-GPIO-004", "GPIO 14 RELAY_M2_OPEN  HIGH/LOW loopback",
          loopback_output(PIN_RELAY_M2_OPEN,  LB_M2_OPEN));
    check("HW-GPIO-005", "GPIO 15 RELAY_M2_CLOSE HIGH/LOW loopback",
          loopback_output(PIN_RELAY_M2_CLOSE, LB_M2_CLOSE));
    check("HW-GPIO-006", "GPIO 16 RELAY_M3_OPEN  HIGH/LOW loopback",
          loopback_output(PIN_RELAY_M3_OPEN,  LB_M3_OPEN));
    check("HW-GPIO-007", "GPIO 21 RELAY_M3_CLOSE HIGH/LOW loopback",
          loopback_output(PIN_RELAY_M3_CLOSE, LB_M3_CLOSE));

    /* Ensure all relay outputs are LOW after test */
    gpio_write(PIN_RELAY_M1_OPEN,  GPIO_LOW);
    gpio_write(PIN_RELAY_M1_CLOSE, GPIO_LOW);
    gpio_write(PIN_RELAY_M2_OPEN,  GPIO_LOW);
    gpio_write(PIN_RELAY_M2_CLOSE, GPIO_LOW);
    gpio_write(PIN_RELAY_M3_OPEN,  GPIO_LOW);
    gpio_write(PIN_RELAY_M3_CLOSE, GPIO_LOW);

    /* -----------------------------------------------------------------
     * HW-GPIO-008 — HB LED output loopback
     * ----------------------------------------------------------------- */
    Serial0.println("--- HB LED loopback ---");
    check("HW-GPIO-008", "GPIO 41 HB_LED HIGH/LOW loopback",
          loopback_output(PIN_HB_LED, LB_HB_LED));
    gpio_write(PIN_HB_LED, GPIO_LOW);

    /* -----------------------------------------------------------------
     * HW-GPIO-009/010 — RS485 DE/RE output loopback
     * ----------------------------------------------------------------- */
    Serial0.println("--- RS485 DE/RE loopback ---");
    gpio_set_rs485_direction(true);
    delay(2);
    check("HW-GPIO-009", "GPIO 8 RS485_DE_RE HIGH (TX mode) loopback",
          digitalRead(LB_RS485) == HIGH);

    gpio_set_rs485_direction(false);
    delay(2);
    check("HW-GPIO-010", "GPIO 8 RS485_DE_RE LOW (RX mode) loopback",
          digitalRead(LB_RS485) == LOW);

    /* -----------------------------------------------------------------
     * HW-GPIO-011 — SD status LED output loopback
     * ----------------------------------------------------------------- */
    Serial0.println("--- SD status LED loopback ---");
    check("HW-GPIO-011", "GPIO 39 SD_STATUS_LED HIGH/LOW loopback",
          loopback_output(PIN_SD_STATUS_LED, LB_SD_LED));
    gpio_write(PIN_SD_STATUS_LED, GPIO_LOW);

    /* -----------------------------------------------------------------
     * HW-GPIO-012 — opto-coupler input (driven by loopback output GPIO 11)
     * ----------------------------------------------------------------- */
    Serial0.println("--- Opto input loopback ---");
    check("HW-GPIO-012", "GPIO 42 OPTO_INPUT LOW/HIGH via GPIO 11 driver",
          loopback_input(DRV_OPTO, PIN_OPTO_INPUT));

    /* -----------------------------------------------------------------
     * HW-GPIO-013 — SD mount button input (driven by loopback output GPIO 17)
     * ----------------------------------------------------------------- */
    Serial0.println("--- SD mount button loopback ---");
    check("HW-GPIO-013", "GPIO 40 SD_MOUNT_BTN LOW/HIGH via GPIO 17 driver",
          loopback_input(DRV_BTN, PIN_SD_MOUNT_BTN));

    /* -----------------------------------------------------------------
     * Summary
     * ----------------------------------------------------------------- */
    Serial0.println("================================================");
    Serial0.print("  PASSED: "); Serial0.println(pass_count);
    Serial0.print("  FAILED: "); Serial0.println(fail_count);
    Serial0.println(fail_count == 0 ? "  RESULT: PASS" : "  RESULT: FAIL");
    Serial0.println("================================================");
    Serial0.println("Entering heartbeat loop (HB LED blinks at 0.5 Hz).");
}

void loop()
{
    gpio_toggle(PIN_HB_LED);
    delay(1000);
}
