/**
 * Relay + HB LED sequence test
 *
 * Exercises all six relay outputs and the heartbeat LED in the order:
 *   M1-OPEN, M1-CLOSE, M2-OPEN, M2-CLOSE, M3-OPEN, M3-CLOSE
 *
 * Each relay is activated for 1 second (GPIO driven LOW — active-low driver)
 * then released.  The HB LED mirrors each relay: on while the relay is active.
 * After the relay sequence the HB LED blinks 5 times as a visual LED check,
 * then the sketch enters a 0.5 Hz heartbeat loop.
 *
 * Serial output : UART0, 115200 baud (GPIO 43 TX / GPIO 44 RX, 3.3 V).
 * Connect a USB-serial adapter — do NOT rely on USB-CDC.
 *
 * WARNING: the relay module MUST be connected.  Do not run while relay output
 * pins are jumpered to the loopback inputs used by the GPIO loopback sketch.
 */

#include <Arduino.h>
#include "gpio_util.h"

#define RELAY_ON_MS    1000   /* relay hold time */
#define RELAY_GAP_MS    500   /* pause between relays */


// Relay driver is active-low: LOW = coil energised, HIGH = coil released.
#define RELAY_ON   GPIO_HIGH
#define RELAY_OFF  GPIO_LOW

// Define alarm relay pin (adjust as needed)
#define PIN_ALARM  42
#define ALARM_ON   GPIO_HIGH
#define ALARM_OFF  GPIO_LOW
#define ALARM_ACTUATE_MS  1200

static void run_relay(uint8_t pin, const char *label)
{
    Serial0.print("  ");
    Serial0.print(label);
    Serial0.print(" ...");

    gpio_write(PIN_HB_LED, GPIO_HIGH);
    gpio_write(pin, RELAY_ON);
    delay(RELAY_ON_MS);
    gpio_write(pin, RELAY_OFF);
    gpio_write(PIN_HB_LED, GPIO_LOW);

    Serial0.println(" released");
    delay(RELAY_GAP_MS);
}

void setup()
{
    Serial0.begin(115200);
    delay(500);

    /* Initialise all relay outputs to the safe (off) state before enabling
     * the output drivers, so the relay module sees a stable HIGH from the
     * first moment the pins are driven. */
    gpio_write(PIN_RELAY_M1_OPEN,  RELAY_OFF);
    gpio_write(PIN_RELAY_M1_CLOSE, RELAY_OFF);
    gpio_write(PIN_RELAY_M2_OPEN,  RELAY_OFF);
    gpio_write(PIN_RELAY_M2_CLOSE, RELAY_OFF);
    gpio_write(PIN_RELAY_M3_OPEN,  RELAY_OFF);
    gpio_write(PIN_RELAY_M3_CLOSE, RELAY_OFF);
    gpio_write(PIN_HB_LED,         GPIO_LOW);

    // Alarm relay off
    gpio_write(PIN_ALARM, ALARM_OFF);

    gpio_set_pin_mode(PIN_RELAY_M1_OPEN,  GPIO_OUTPUT);
    gpio_set_pin_mode(PIN_RELAY_M1_CLOSE, GPIO_OUTPUT);
    gpio_set_pin_mode(PIN_RELAY_M2_OPEN,  GPIO_OUTPUT);
    gpio_set_pin_mode(PIN_RELAY_M2_CLOSE, GPIO_OUTPUT);
    gpio_set_pin_mode(PIN_RELAY_M3_OPEN,  GPIO_OUTPUT);
    gpio_set_pin_mode(PIN_RELAY_M3_CLOSE, GPIO_OUTPUT);
    gpio_set_pin_mode(PIN_HB_LED,         GPIO_OUTPUT);

    // Alarm relay output
    gpio_set_pin_mode(PIN_ALARM, GPIO_OUTPUT);

    // OPTO_INPUT as input with pull-up (if supported by gpio_util)
#ifdef GPIO_INPUT_PULLUP
    gpio_set_pin_mode(PIN_OPTO_INPUT, GPIO_INPUT_PULLUP);
#else
    pinMode(PIN_OPTO_INPUT, INPUT_PULLUP);
#endif

    Serial0.println();
    Serial0.println("================================================");
    Serial0.println("  Relay + HB LED sequence test");
    Serial0.println("  Each relay: 1 s ON  ->  release  ->  next");
    Serial0.println("================================================");

    run_relay(PIN_RELAY_M1_OPEN,  "M1 OPEN  (GPIO 12)");
    run_relay(PIN_RELAY_M1_CLOSE, "M1 CLOSE (GPIO 13)");
    run_relay(PIN_RELAY_M2_OPEN,  "M2 OPEN  (GPIO 14)");
    run_relay(PIN_RELAY_M2_CLOSE, "M2 CLOSE (GPIO 15)");
    run_relay(PIN_RELAY_M3_OPEN,  "M3 OPEN  (GPIO 16)");
    run_relay(PIN_RELAY_M3_CLOSE, "M3 CLOSE (GPIO 21)");


    // Control M1 OPEN relay with OPTO_INPUT
    Serial0.println("\nM1 OPEN relay will follow OPTO_INPUT (LOW = closed, relay ON; HIGH = open, relay OFF)");
    gpio_write(PIN_RELAY_M1_OPEN, RELAY_OFF); // Ensure relay is off initially
    int last_opto = gpio_read(PIN_OPTO_INPUT);
    bool toggled_low = false;
    bool toggled_high = false;
    unsigned long start = millis();
    while (!(toggled_low && toggled_high)) {
        int opto = gpio_read(PIN_OPTO_INPUT);
        if (opto == GPIO_LOW) {
            gpio_write(PIN_RELAY_M1_OPEN, RELAY_ON);
            if (last_opto == GPIO_HIGH) toggled_low = true;
        } else {
            gpio_write(PIN_RELAY_M1_OPEN, RELAY_OFF);
            if (last_opto == GPIO_LOW && toggled_low) toggled_high = true;
        }
        last_opto = opto;
        delay(20);
        // Optional: add a timeout to avoid infinite loop
        if (millis() - start > 15000) break;
    }
    gpio_write(PIN_RELAY_M1_OPEN, RELAY_OFF); // Ensure relay is off at end
    Serial0.println("M1 OPEN relay input test complete.");

    /* HB LED standalone check — 5 fast blinks */
    Serial0.print("  HB LED (GPIO 41) blink test ...");
    for (int i = 0; i < 5; i++) {
        gpio_write(PIN_HB_LED, GPIO_HIGH);
        delay(100);
        gpio_write(PIN_HB_LED, GPIO_LOW);
        delay(100);
    }
    Serial0.println(" done");

    Serial0.println("================================================");
    Serial0.println("  Sequence complete. Entering heartbeat loop.");
    Serial0.println("================================================");
}

void loop()
{
    gpio_toggle(PIN_HB_LED);
    delay(1000);
}

