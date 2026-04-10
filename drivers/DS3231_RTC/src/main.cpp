/**
 * LIB-3 DS1307 RTC — hardware verification sketch
 *
 * Covers HW-RTC-001 through HW-RTC-006.
 *
 * Wiring:
 *   GPIO 1 (SDA) — DS1307 SDA
 *   GPIO 2 (SCL) — DS1307 SCL
 *   3.3 V         — DS1307 VCC
 *   GND           — DS1307 GND
 *   CR2032 battery must be inserted before running HW-RTC-006.
 *
 * Serial interface: UART0 on GPIO 43 (TX) / GPIO 44 (RX), 115200 baud.
 * Connect a USB-to-serial adapter (3.3 V) to GPIO 43 / GND to read output.
 */

#include <Arduino.h>
#include "i2c_bus.h"
#include "ds1307_rtc.h"

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

static void print_datetime(const rtc_datetime_t *dt)
{
    Serial0.printf("%04u-%02u-%02u %02u:%02u:%02u (dow %u)\n",
                   dt->year, dt->month, dt->day,
                   dt->hour, dt->minute, dt->second,
                   dt->day_of_week);
}

/* ---------------------------------------------------------------------------
 * Setup — runs tests once on boot
 * --------------------------------------------------------------------------- */
void setup()
{
    Serial0.begin(115200);
    delay(200);

    Serial0.println("=== LIB-3 DS1307 RTC — hardware verification ===");
    Serial0.println();

    /* ---------------------------------------------------------------------- *
     * Initialise I2C bus first (prerequisite)
     * ---------------------------------------------------------------------- */
    i2c_init();

    /* ---------------------------------------------------------------------- *
     * HW-RTC-001 — Driver initialises and detects device
     * ---------------------------------------------------------------------- */
    rtc_status_t st = rtc_init();
    Serial0.printf("DS1307 init: %s\n", (st == RTC_OK) ? "OK" : "FAIL");
    check("HW-RTC-001", "Driver initialises and detects device", st == RTC_OK);

    /* ---------------------------------------------------------------------- *
     * HW-RTC-003 — Time can be set (also clears OSF as a side effect)
     * ---------------------------------------------------------------------- */
    rtc_datetime_t set_dt = { 0, 0, 12, 5, 10, 4, 2026 }; /* 2026-04-10 12:00:00 Fri */
    st = rtc_set_time(&set_dt);
    check("HW-RTC-003", "Time can be set", st == RTC_OK);
    if (st == RTC_OK) {
        Serial0.print("Set time: ");
        print_datetime(&set_dt);
    }

    /* ---------------------------------------------------------------------- *
     * HW-RTC-002 — Oscillator stop flag (checked after set_time clears it)
     * ---------------------------------------------------------------------- */
    bool osf = rtc_oscillator_stopped();
    Serial0.printf("Oscillator stop flag: %s\n", osf ? "SET (time invalid)" : "CLEAR");
    check("HW-RTC-002", "Oscillator stop flag is clear", !osf);

    /* ---------------------------------------------------------------------- *
     * HW-RTC-004 — Time is advancing (read twice, 3 s apart)
     * ---------------------------------------------------------------------- */
    rtc_datetime_t dt1, dt2;
    st = rtc_get_time(&dt1);
    if (st == RTC_OK) {
        Serial0.print("Read 1:   ");
        print_datetime(&dt1);
    }
    delay(3000);
    st = rtc_get_time(&dt2);
    if (st == RTC_OK) {
        Serial0.print("Read 2:   ");
        print_datetime(&dt2);
    }
    /* Compare only seconds for simplicity; allow ±1 s tolerance */
    int delta = (int)dt2.second - (int)dt1.second;
    if (delta < 0) { delta += 60; } /* wrap */
    Serial0.printf("Delta: %d s\n", delta);
    check("HW-RTC-004", "Time is advancing (3 s ±1 s)", delta >= 2 && delta <= 4);

    /* ---------------------------------------------------------------------- *
     * HW-RTC-006 — Battery backup: RTC keeps time when VCC is removed
     *
     * The DS1307 is powered from the board 3.3 V rail; the CR2032 takes over
     * when VCC drops. The ESP32 stays powered throughout — only the DS1307
     * VCC wire (not GND, not SDA/SCL, not the battery) is disconnected.
     * ---------------------------------------------------------------------- */
    Serial0.println();
    Serial0.println("HW-RTC-006: Battery backup test.");
    Serial0.println("Disconnect ONLY the DS1307 VCC wire when prompted.");
    Serial0.println("Keep GND, SDA, SCL and the CR2032 battery connected.");

    /* 10-second countdown so the developer can get ready */
    for (int c = 10; c > 0; c--) {
        Serial0.printf("  Disconnect VCC in %d s ...\n", c);
        delay(1000);
    }

    /* Capture reference time immediately before asking for disconnect so the
     * countdown itself is not included in the elapsed measurement. */
    rtc_datetime_t t006_before, t006_after;
    rtc_get_time(&t006_before);
    Serial0.println(">>> DISCONNECT DS1307 VCC NOW <<<");

    Serial0.println("Waiting 10 s (RTC running on CR2032 only) ...");
    delay(10000);

    Serial0.println(">>> RECONNECT DS1307 VCC NOW <<<");

    /* 10-second countdown to give the developer time to reconnect the wire */
    for (int c = 10; c > 0; c--) {
        Serial0.printf("  Reconnect VCC in %d s ...\n", c);
        delay(1000);
    }

    Serial0.println("Settling 2 s ...");
    delay(2000);

    st = rtc_get_time(&t006_after);

    bool comm_ok   = (st == RTC_OK);
    bool osf_clear = !rtc_oscillator_stopped();

    /* Compute elapsed seconds from the reference snapshot.
     * Expected: disconnect latency (~1 s) + 10 s on battery
     *           + 10 s reconnect countdown + 2 s settle ≈ 23 s.
     * Accept 18–120 s to tolerate slow manual operation. */
    int elapsed006 = 0;
    if (comm_ok) {
        elapsed006 = ((int)t006_after.hour  * 3600 + (int)t006_after.minute  * 60 + (int)t006_after.second)
                   - ((int)t006_before.hour * 3600 + (int)t006_before.minute * 60 + (int)t006_before.second);
        if (elapsed006 < 0) { elapsed006 += 86400; } /* handle midnight wrap */
        Serial0.printf("Elapsed since reference: %d s\n", elapsed006);
    }
    Serial0.printf("OSF after reconnect: %s\n", osf_clear ? "CLEAR" : "SET");

    check("HW-RTC-006", "Battery backup: time retained across RTC power cycle",
          comm_ok && osf_clear && elapsed006 >= 18 && elapsed006 <= 120);

    /* ---------------------------------------------------------------------- *
     * Summary
     * ---------------------------------------------------------------------- */
    Serial0.println();
    Serial0.printf("Result: %d passed, %d failed\n", pass_count, fail_count);
    Serial0.println((fail_count == 0) ? "=== PASS ===" : "=== FAIL ===");
}

void loop() {}
