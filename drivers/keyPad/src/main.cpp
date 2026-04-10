/**
 * LIB-5 Keypad Matrix — hardware verification sketch
 *
 * Covers HW-KP-001 through HW-KP-004.
 *
 * The sketch requests each of the 16 keys one at a time over Serial0
 * (UART0, GPIO 43 TX / GPIO 44 RX, 115200 baud) and waits up to 30 seconds
 * for the developer to press it. keypad_scan() is polled every 20 ms inside
 * the wait loop, providing the same timing as the production task (T7).
 *
 * Multi-press inputs are silently discarded by the driver — the sketch simply
 * keeps waiting until a single valid key is detected or the timeout expires.
 *
 * Serial interface: UART0 on GPIO 43 (TX) / GPIO 44 (RX), 115200 baud.
 * Connect a USB-to-serial adapter (3.3 V) to GPIO 43 / GND before reset.
 *
 * ---------------------------------------------------------------------------
 * Wiring
 * ---------------------------------------------------------------------------
 * Keypad ribbon   → LOLIN S3 GPIO
 *   Row 1         → GPIO  3  (KP_ROW1)
 *   Row 2         → GPIO  4  (KP_ROW2)
 *   Row 3         → GPIO  5  (KP_ROW3)
 *   Row 4         → GPIO  6  (KP_ROW4)
 *   Col 1         → GPIO  7  (KP_COL1)
 *   Col 2         → GPIO  9  (KP_COL2)
 *   Col 3         → GPIO 10  (KP_COL3)
 *   Col 4         → GPIO 11  (KP_COL4)
 *
 * Serial adapter
 *   RX            → GPIO 43  (TX0)
 *   GND           → GND
 *
 * No external pull-up resistors are required; internal pull-ups on the
 * column pins are enabled by keypad_init().
 *
 * ---------------------------------------------------------------------------
 * Key layout (standard 4×4 membrane keypad)
 * ---------------------------------------------------------------------------
 *   [ 1 ][ 2 ][ 3 ][ A ]
 *   [ 4 ][ 5 ][ 6 ][ B ]
 *   [ 7 ][ 8 ][ 9 ][ C ]
 *   [ * ][ 0 ][ # ][ D ]
 */

#include <Arduino.h>
#include "keypad_matrix.h"

/* ---------------------------------------------------------------------------
 * Test key sequence (row-by-row order as specified in HW-KP-001)
 * --------------------------------------------------------------------------- */
static const struct {
    char        expected;
    const char *position;   /**< Keypad location description */
} test_sequence[] = {
    {'1', "Row1/Col1"}, {'2', "Row1/Col2"}, {'3', "Row1/Col3"}, {'A', "Row1/Col4"},
    {'4', "Row2/Col1"}, {'5', "Row2/Col2"}, {'6', "Row2/Col3"}, {'B', "Row2/Col4"},
    {'7', "Row3/Col1"}, {'8', "Row3/Col2"}, {'9', "Row3/Col3"}, {'C', "Row3/Col4"},
    {'*', "Row4/Col1"}, {'0', "Row4/Col2"}, {'#', "Row4/Col3"}, {'D', "Row4/Col4"},
};

static const int KEY_COUNT      = (int)(sizeof(test_sequence) / sizeof(test_sequence[0]));
static const unsigned long TIMEOUT_MS = 30000UL;
static const unsigned long SCAN_MS    =    20UL;

/* ---------------------------------------------------------------------------
 * Counters
 * --------------------------------------------------------------------------- */
static int pass_count    = 0;
static int fail_count    = 0;
static int timeout_count = 0;

/* ---------------------------------------------------------------------------
 * Setup — runs all hardware tests once, then idles
 * --------------------------------------------------------------------------- */
void setup()
{
    Serial0.begin(115200);
    delay(500);

    keypad_init();

    Serial0.println();
    Serial0.println("================================================");
    Serial0.println("  LIB-5 Keypad Matrix — hardware verification");
    Serial0.println("================================================");
    Serial0.println("Press each key when requested (30 s timeout).");
    Serial0.println("Pressing multiple keys simultaneously is discarded;");
    Serial0.println("release all keys and press only the requested one.");
    Serial0.println("------------------------------------------------");

    /* -----------------------------------------------------------------
     * HW-KP-003 — idle test: no spurious output for 5 seconds
     * ----------------------------------------------------------------- */
    Serial0.println("[HW-KP-003] Idle test — do NOT press any key for 5 s ...");
    bool spurious        = false;
    unsigned long t_idle = millis() + 5000UL;
    while (millis() < t_idle) {
        if (keypad_scan() != KP_NO_KEY) {
            spurious = true;
            break;
        }
        delay(SCAN_MS);
    }
    if (!spurious) {
        Serial0.println("[PASS] HW-KP-003: no spurious output during idle period");
        pass_count++;
    } else {
        Serial0.println("[FAIL] HW-KP-003: spurious key detected while keypad was idle");
        fail_count++;
    }
    Serial0.println("------------------------------------------------");

    /* -----------------------------------------------------------------
     * HW-KP-005 — multi-press detection
     * Ask the developer to hold ANY two keys simultaneously.
     * keypad_count_pressed() is used to confirm that ≥2 keys are
     * physically down; keypad_scan() must return KP_NO_KEY throughout.
     * ----------------------------------------------------------------- */
    Serial0.println("[HW-KP-005] Multi-press test — hold ANY TWO keys simultaneously for 5 s ...");
    {
        bool multipress_confirmed = false;
        bool spurious_char        = false;
        unsigned long t_multi     = millis() + 30000UL;   /* 30 s to get into position */

        /* Wait until developer is actually holding two keys */
        Serial0.println("  Waiting for two keys to be held down...");
        while (millis() < t_multi) {
            if (keypad_count_pressed() >= 2) {
                multipress_confirmed = true;
                break;
            }
            delay(SCAN_MS);
        }

        if (!multipress_confirmed) {
            Serial0.println("[TIMEOUT] HW-KP-005: no multi-press detected within 30 s");
            timeout_count++;
        } else {
            /* Developer is holding ≥2 keys: scan for 5 s and verify KP_NO_KEY */
            Serial0.println("  Multi-press detected — verifying discard for 5 s...");
            unsigned long t_verify = millis() + 5000UL;
            while (millis() < t_verify) {
                if (keypad_count_pressed() < 2) {
                    /* Developer released early — extend the window */
                    t_verify = millis() + 1000UL;
                }
                if (keypad_scan() != KP_NO_KEY) {
                    spurious_char = true;
                    break;
                }
                delay(SCAN_MS);
            }

            if (!spurious_char) {
                Serial0.println("[PASS] HW-KP-005: multi-press correctly discarded (KP_NO_KEY)");
                pass_count++;
            } else {
                Serial0.println("[FAIL] HW-KP-005: multi-press produced a character — not discarded");
                fail_count++;
            }
        }

        /* Wait for all keys to be released before the single-key tests */
        Serial0.println("  Release all keys...");
        while (keypad_count_pressed() > 0) {
            delay(SCAN_MS);
        }
        delay(300);
    }
    Serial0.println("------------------------------------------------");

    /* -----------------------------------------------------------------
     * HW-KP-001 / HW-KP-002 / HW-KP-004
     * Request each of the 16 keys in sequence with a 30 s timeout.
     * ----------------------------------------------------------------- */
    for (int i = 0; i < KEY_COUNT; i++) {
        char        exp = test_sequence[i].expected;
        const char *pos = test_sequence[i].position;

        Serial0.print("Press key [ ");
        Serial0.print(exp);
        Serial0.print(" ]  (");
        Serial0.print(pos);
        Serial0.println(")  — timeout 30 s");

        char         got      = KP_NO_KEY;
        unsigned long deadline = millis() + TIMEOUT_MS;

        while (millis() < deadline) {
            got = keypad_scan();
            if (got != KP_NO_KEY) break;
            delay(SCAN_MS);
        }

        /* Print result */
        if (got == KP_NO_KEY) {
            Serial0.print("[TIMEOUT] HW-KP-00");
            Serial0.print(i + 4);    /* HW-KP-004 onwards */
            Serial0.print(": key [ ");
            Serial0.print(exp);
            Serial0.println(" ] — no press within 30 s");
            timeout_count++;
        } else if (got == exp) {
            Serial0.print("[PASS]    HW-KP-00");
            Serial0.print(i + 4);
            Serial0.print(": key [ ");
            Serial0.print(exp);
            Serial0.println(" ]");
            pass_count++;
        } else {
            Serial0.print("[FAIL]    HW-KP-00");
            Serial0.print(i + 4);
            Serial0.print(": key [ ");
            Serial0.print(exp);
            Serial0.print(" ] — expected [");
            Serial0.print(exp);
            Serial0.print("], got [");
            Serial0.print(got);
            Serial0.println("]");
            fail_count++;
        }

        /* Wait for key release before requesting the next key */
        while (keypad_scan() != KP_NO_KEY) {
            delay(SCAN_MS);
        }
        delay(200);   /* brief settling */
    }

    /* -----------------------------------------------------------------
     * Summary
     * ----------------------------------------------------------------- */
    Serial0.println("================================================");
    Serial0.print("  PASSED:  "); Serial0.println(pass_count);
    Serial0.print("  FAILED:  "); Serial0.println(fail_count);
    Serial0.print("  TIMEOUT: "); Serial0.println(timeout_count);
    bool ok = (fail_count == 0 && timeout_count == 0);
    Serial0.println(ok ? "  RESULT: PASS" : "  RESULT: FAIL");
    Serial0.println("================================================");
    Serial0.println("Verification complete. Board is idle.");
}

void loop()
{
    /* Nothing to do after verification */
    delay(1000);
}
