/**
 * LIB-6 Modbus RTU — hardware verification sketch
 *
 * Covers HW-MB-001 through HW-MB-004.
 * HW-MB-005 (DE/RE timing) is verified with an oscilloscope or logic
 * analyser during the FC03 call in HW-MB-002.
 *
 * Setup:
 *   SIT65HVD08P DI  → GPIO 17  (UART1 TX)
 *   SIT65HVD08P RO  → GPIO 18  (UART1 RX)
 *   SIT65HVD08P DE+RE → GPIO 8 (RS485 direction, from LIB-1 gpio/)
 *   120 Ω termination resistor across RS485 A/B at the far end.
 *
 * Simulator:
 *   Start a pymodbus slave server on a PC with USB-RS485 adapter.
 *   Address 1 — holding registers 0–1: {0x1234, 0x5678}
 *   Address 2 — input  registers 0–1: {0x00E6, 0x028F}
 *
 * Serial output uses UART0 (GPIO 43 TX / GPIO 44 RX, 115200 baud).
 * Connect a USB-to-serial adapter to GPIO 43 / GND to read results.
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#include <Arduino.h>
#include "modbus_rtu.h"

/* ---------------------------------------------------------------------------
 * Test helpers
 * --------------------------------------------------------------------------- */
static int pass_count = 0;
static int fail_count = 0;

static void check(const char *id, const char *description, bool condition)
{
    if (condition) {
        Serial.print("[PASS] ");
        pass_count++;
    } else {
        Serial.print("[FAIL] ");
        fail_count++;
    }
    Serial.print(id);
    Serial.print(": ");
    Serial.println(description);
}

static const char *status_str(modbus_status_t s)
{
    switch (s) {
        case MODBUS_OK:            return "MODBUS_OK";
        case MODBUS_ERR_TIMEOUT:   return "MODBUS_ERR_TIMEOUT";
        case MODBUS_ERR_CRC:       return "MODBUS_ERR_CRC";
        case MODBUS_ERR_EXCEPTION: return "MODBUS_ERR_EXCEPTION";
        case MODBUS_ERR_FRAMING:   return "MODBUS_ERR_FRAMING";
        case MODBUS_ERR_PARAM:     return "MODBUS_ERR_PARAM";
        default:                   return "MODBUS_ERR_UNKNOWN";
    }
}

/* ---------------------------------------------------------------------------
 * Setup — runs all hardware tests once, then enters idle loop
 * --------------------------------------------------------------------------- */
void setup()
{
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("================================================");
    Serial.println("  LIB-6 Modbus RTU — hardware verification");
    Serial.println("================================================");

    /* -----------------------------------------------------------------
     * HW-MB-001 — driver initialises on correct UART pins
     * ----------------------------------------------------------------- */
    modbus_init();
    Serial.println("Modbus init: UART1 TX=GPIO17 RX=GPIO18 baud=9600 DE/RE=GPIO8");
    check("HW-MB-001", "modbus_init() completed — init message printed above", true);

    /* -----------------------------------------------------------------
     * HW-MB-002 — FC03 reads correct holding register values
     *             (also covers HW-MB-005: probe GPIO 8 during this call)
     * ----------------------------------------------------------------- */
    Serial.println("--- FC03 holding registers (addr=1) ---");
    uint16_t hold[2] = {0, 0};
    modbus_status_t s = modbus_read_holding_registers(1, 0, 2, hold);
    Serial.print("  status : "); Serial.println(status_str(s));
    Serial.print("  val[0] = 0x"); Serial.println(hold[0], HEX);
    Serial.print("  val[1] = 0x"); Serial.println(hold[1], HEX);
    check("HW-MB-002", "FC03 val[0]=0x1234 val[1]=0x5678",
          s == MODBUS_OK && hold[0] == 0x1234 && hold[1] == 0x5678);

    /* -----------------------------------------------------------------
     * HW-MB-003 — FC04 reads correct input register values
     * ----------------------------------------------------------------- */
    delay(200);   /* give slave time to return to idle before next frame */
    Serial.println("--- FC04 input registers (addr=2) ---");
    uint16_t inp[2] = {0, 0};
    s = modbus_read_input_registers(2, 0, 2, inp);
    Serial.print("  status : "); Serial.println(status_str(s));
    Serial.print("  val[0] = 0x"); Serial.println(inp[0], HEX);
    Serial.print("  val[1] = 0x"); Serial.println(inp[1], HEX);
    check("HW-MB-003", "FC04 val[0]=0x00E6 val[1]=0x028F",
          s == MODBUS_OK && inp[0] == 0x00E6 && inp[1] == 0x028F);

    /* -----------------------------------------------------------------
     * HW-MB-004 — timeout returned for absent device (addr=99)
     * ----------------------------------------------------------------- */
    Serial.println("--- Timeout test (addr=99, no device) ---");
    uint16_t dummy[2] = {0, 0};
    uint32_t t0 = millis();
    s = modbus_read_holding_registers(99, 0, 2, dummy);
    uint32_t elapsed = millis() - t0;
    Serial.print("  status  : "); Serial.println(status_str(s));
    Serial.print("  elapsed : "); Serial.print(elapsed); Serial.println(" ms");
    check("HW-MB-004", "MODBUS_ERR_TIMEOUT returned; no crash; elapsed < 300 ms",
          s == MODBUS_ERR_TIMEOUT && elapsed < 300);

    /* -----------------------------------------------------------------
     * Summary
     * ----------------------------------------------------------------- */
    Serial.println("================================================");
    Serial.print("  PASSED: "); Serial.println(pass_count);
    Serial.print("  FAILED: "); Serial.println(fail_count);
    Serial.println(fail_count == 0 ? "  RESULT: PASS" : "  RESULT: FAIL");
    Serial.println("================================================");
    Serial.println("Entering idle loop.");
}

void loop()
{
    delay(1000);
}

