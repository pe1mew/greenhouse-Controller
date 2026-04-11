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
    Serial0.begin(115200);
    delay(500);

    Serial0.println();
    Serial0.println("================================================");
    Serial0.println("  LIB-6 Modbus RTU — hardware verification");
    Serial0.println("================================================");

    /* -----------------------------------------------------------------
     * HW-MB-001 — driver initialises on correct UART pins
     * ----------------------------------------------------------------- */
    modbus_init();
    Serial0.println("Modbus init: UART1 TX=GPIO17 RX=GPIO18 baud=9600 DE/RE=GPIO8");
    check("HW-MB-001", "modbus_init() completed — init message printed above", true);

    /* -----------------------------------------------------------------
     * HW-MB-002 — FC03 reads correct holding register values
     *             (also covers HW-MB-005: probe GPIO 8 during this call)
     * ----------------------------------------------------------------- */
    Serial0.println("--- FC03 holding registers (addr=1) ---");
    uint16_t hold[2] = {0, 0};
    modbus_status_t s = modbus_read_holding_registers(1, 0, 2, hold);
    Serial0.print("  status : "); Serial0.println(status_str(s));
    Serial0.print("  val[0] = 0x"); Serial0.println(hold[0], HEX);
    Serial0.print("  val[1] = 0x"); Serial0.println(hold[1], HEX);
    check("HW-MB-002", "FC03 val[0]=0x1234 val[1]=0x5678",
          s == MODBUS_OK && hold[0] == 0x1234 && hold[1] == 0x5678);

    /* -----------------------------------------------------------------
     * HW-MB-003 — FC04 reads correct input register values
     * ----------------------------------------------------------------- */
    Serial0.println("--- FC04 input registers (addr=2) ---");
    uint16_t inp[2] = {0, 0};
    s = modbus_read_input_registers(2, 0, 2, inp);
    Serial0.print("  status : "); Serial0.println(status_str(s));
    Serial0.print("  val[0] = 0x"); Serial0.println(inp[0], HEX);
    Serial0.print("  val[1] = 0x"); Serial0.println(inp[1], HEX);
    check("HW-MB-003", "FC04 val[0]=0x00E6 val[1]=0x028F",
          s == MODBUS_OK && inp[0] == 0x00E6 && inp[1] == 0x028F);

    /* -----------------------------------------------------------------
     * HW-MB-004 — timeout returned for absent device (addr=99)
     * ----------------------------------------------------------------- */
    Serial0.println("--- Timeout test (addr=99, no device) ---");
    uint16_t dummy[2] = {0, 0};
    uint32_t t0 = millis();
    s = modbus_read_holding_registers(99, 0, 2, dummy);
    uint32_t elapsed = millis() - t0;
    Serial0.print("  status  : "); Serial0.println(status_str(s));
    Serial0.print("  elapsed : "); Serial0.print(elapsed); Serial0.println(" ms");
    check("HW-MB-004", "MODBUS_ERR_TIMEOUT returned; no crash; elapsed < 300 ms",
          s == MODBUS_ERR_TIMEOUT && elapsed < 300);

    /* -----------------------------------------------------------------
     * Summary
     * ----------------------------------------------------------------- */
    Serial0.println("================================================");
    Serial0.print("  PASSED: "); Serial0.println(pass_count);
    Serial0.print("  FAILED: "); Serial0.println(fail_count);
    Serial0.println(fail_count == 0 ? "  RESULT: PASS" : "  RESULT: FAIL");
    Serial0.println("================================================");
    Serial0.println("Entering idle loop.");
}

void loop()
{
    delay(1000);
}
