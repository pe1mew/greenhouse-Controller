/**
 * LIB-2 I2C Bus — hardware verification sketch
 *
 * Covers HW-I2C-001 through HW-I2C-005.
 *
 * Requires both a Waveshare LCD1602 I2C module (PCF8574A at 0x3E) and a
 * DS3231 RTC module (at 0x68) to be wired to the I2C bus simultaneously.
 * Both modules include their own 4.7 kΩ pull-up resistors; no external
 * resistors are needed.
 *
 * Wiring:
 *   GPIO 1 (SDA) — SDA on both modules
 *   GPIO 2 (SCL) — SCL on both modules
 *   3.3 V         — VCC on both modules
 *   GND           — GND on both modules
 *
 * Serial interface: UART0 on GPIO 43 (TX) / GPIO 44 (RX), 115200 baud.
 * Connect a USB-to-serial adapter (3.3 V) to GPIO 43 / GND to read output.
 * Do NOT use the USB-CDC Serial port — it requires USB enumeration before
 * printing, which makes timing unreliable for a boot-time verification sketch.
 */

#include <Arduino.h>
#include "i2c_bus.h"

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

/* ---------------------------------------------------------------------------
 * Setup — runs tests once on boot
 * --------------------------------------------------------------------------- */
void setup()
{
    Serial0.begin(115200);
    delay(200);

    Serial0.println("=== LIB-2 I2C Bus — hardware verification ===");
    Serial0.println();

    /* ---------------------------------------------------------------------- *
     * HW-I2C-001 — Bus initialises at correct speed
     * ---------------------------------------------------------------------- */
    i2c_status_t st = i2c_init();
    if (st == I2C_OK) {
        Serial0.printf("I2C init: SDA=GPIO%d SCL=GPIO%d %lu kHz\n",
                       PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ / 1000UL);
    }
    check("HW-I2C-001", "Bus initialises at correct speed", st == I2C_OK);

    /* ---------------------------------------------------------------------- *
     * HW-I2C-002 & HW-I2C-003 — Scan bus: find LCD (0x3E) and RTC (0x68)
     * ---------------------------------------------------------------------- */
    uint8_t found_addrs[16];
    uint8_t n = i2c_scan(found_addrs, 16);

    Serial0.printf("Scan found %u device(s):", n);
    for (uint8_t i = 0; i < n; i++) {
        Serial0.printf(" 0x%02X", found_addrs[i]);
    }
    Serial0.println();

    bool found_3e = false;
    bool found_68 = false;
    for (uint8_t i = 0; i < n; i++) {
        if (found_addrs[i] == 0x3E) { found_3e = true; }
        if (found_addrs[i] == 0x68) { found_68 = true; }
    }
    check("HW-I2C-002", "LCD PCF8574A detected on scan (0x3E)", found_3e);
    check("HW-I2C-003", "DS3231 RTC detected on scan (0x68)", found_68);

    /* ---------------------------------------------------------------------- *
     * HW-I2C-004 — Write 1 byte to LCD address (0x3E)
     * ---------------------------------------------------------------------- */
    const uint8_t lcd_byte = 0x00;
    st = i2c_write(0x3E, &lcd_byte, 1);
    Serial0.printf("Write 1 byte to 0x3E: %s\n", (st == I2C_OK) ? "OK" : "FAIL");
    check("HW-I2C-004", "Write to 0x3E succeeds", st == I2C_OK);

    /* ---------------------------------------------------------------------- *
     * HW-I2C-005 — Write register address then read 1 byte from RTC (0x68)
     * ---------------------------------------------------------------------- */
    const uint8_t rtc_reg = 0x00; /* seconds register */
    uint8_t rx_byte = 0;
    st = i2c_write_read(0x68, &rtc_reg, 1, &rx_byte, 1);
    Serial0.printf("Write-read from 0x68: %s, value = 0x%02X\n",
                   (st == I2C_OK) ? "OK" : "FAIL", rx_byte);
    check("HW-I2C-005", "Write-read from 0x68 succeeds", st == I2C_OK);

    /* ---------------------------------------------------------------------- *
     * Summary
     * ---------------------------------------------------------------------- */
    Serial0.println();
    Serial0.printf("Result: %d passed, %d failed\n", pass_count, fail_count);
    Serial0.println((fail_count == 0) ? "=== PASS ===" : "=== FAIL ===");
}

void loop() {}
