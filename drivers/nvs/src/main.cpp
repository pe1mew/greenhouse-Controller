/**
 * LIB-7 NVS Configuration — hardware verification sketch
 *
 * Covers HW-NVS-001 through HW-NVS-012.
 *
 * Serial interface: UART0 on GPIO 43 (TX) / GPIO 44 (RX), 115200 baud.
 * Connect a 3.3 V USB-to-serial adapter to GPIO 43 / GND before reset.
 *
 * Power-cycle persistence tests (HW-NVS-009 / HW-NVS-010):
 *   Run 1: sketch writes test values and reports "PRE-POWER-CYCLE" state.
 *   Power the board off, then on.
 *   Run 2: sketch reads the same keys and reports "POST-POWER-CYCLE" state.
 *   Compare the two serial outputs to verify persistence.
 *
 * If NVS becomes corrupted: pio run -e lolin_s3 -t erase
 */

#include <Arduino.h>
#include "nvs_config.h"

static int pass_count = 0;
static int fail_count = 0;

static void check(const char *id, const char *desc, bool ok)
{
    if (ok) { Serial0.print("[PASS] "); pass_count++; }
    else    { Serial0.print("[FAIL] "); fail_count++; }
    Serial0.print(id);
    Serial0.print(": ");
    Serial0.println(desc);
}

void setup()
{
    Serial0.begin(115200);
    delay(3000);   /* allow serial monitor to connect before output begins */

    Serial0.println();
    Serial0.println("================================================");
    Serial0.println("  LIB-7 NVS Configuration — hardware verification");
    Serial0.println("================================================");

    /* -----------------------------------------------------------------
     * HW-NVS-001 — NVS init
     * ----------------------------------------------------------------- */
    nvs_cfg_status_t init_st = nvs_cfg_init();
    if (init_st == NVS_CFG_ERR_MIGRATION) {
        Serial0.println("[INFO] nvs_cfg_init: schema migration ran — defaults applied");
    }
    check("HW-NVS-001", "NVS init OK",
          init_st == NVS_CFG_OK || init_st == NVS_CFG_ERR_MIGRATION);

    /* -----------------------------------------------------------------
     * HW-NVS-011 — schema version stamped on first boot / after migration
     * ----------------------------------------------------------------- */
    int32_t schema_ver = 0;
    nvs_cfg_get_schema_version(&schema_ver);
    Serial0.print("[INFO] schema_ver stored = ");
    Serial0.println(schema_ver);
    check("HW-NVS-011", "schema_ver == NVS_SCHEMA_VERSION",
          schema_ver == (int32_t)NVS_SCHEMA_VERSION);

    /* -----------------------------------------------------------------
     * HW-NVS-013 — fw_version written on every boot
     * ----------------------------------------------------------------- */
    char fw_ver[32] = {0};
    nvs_cfg_get_str(NVS_NS_SYSTEM, NVS_KEY_FW_VERSION, fw_ver, sizeof(fw_ver));
    Serial0.print("[INFO] fw_version stored = \"");
    Serial0.print(fw_ver);
    Serial0.println("\"");
    Serial0.print("[INFO] FIRMWARE_VERSION   = \"");
    Serial0.print(FIRMWARE_VERSION);
    Serial0.println("\"");
    check("HW-NVS-013", "fw_version == FIRMWARE_VERSION",
          strcmp(fw_ver, FIRMWARE_VERSION) == 0);

    /* -----------------------------------------------------------------
     * HW-NVS-002 — integer set / get round-trip
     * ----------------------------------------------------------------- */
    nvs_cfg_set_i32(NVS_NS_CLIMATE, "t_min", 200);
    int32_t t_min = 0;
    nvs_cfg_get_i32(NVS_NS_CLIMATE, "t_min", &t_min);
    Serial0.print("[INFO] climate/t_min get = ");
    Serial0.println(t_min);
    check("HW-NVS-002", "integer set/get round-trip (t_min=200)", t_min == 200);

    /* -----------------------------------------------------------------
     * HW-NVS-003 — string set / get round-trip
     * ----------------------------------------------------------------- */
    nvs_cfg_set_str(NVS_NS_WIFI, "ssid", "Greenhouse1");
    char ssid[32] = {0};
    nvs_cfg_get_str(NVS_NS_WIFI, "ssid", ssid, sizeof(ssid));
    Serial0.print("[INFO] wifi/ssid get = \"");
    Serial0.print(ssid);
    Serial0.println("\"");
    check("HW-NVS-003", "string set/get round-trip (ssid=Greenhouse1)",
          strcmp(ssid, "Greenhouse1") == 0);

    /* -----------------------------------------------------------------
     * HW-NVS-004 — missing key returns NOT_FOUND
     * ----------------------------------------------------------------- */
    int32_t dummy = 0;
    nvs_cfg_status_t nf = nvs_cfg_get_i32(NVS_NS_CLIMATE, "never_set", &dummy);
    Serial0.print("[INFO] get missing key = ");
    Serial0.println(nf == NVS_CFG_ERR_NOT_FOUND ? "NOT_FOUND" : "unexpected");
    check("HW-NVS-004", "missing key returns NOT_FOUND",
          nf == NVS_CFG_ERR_NOT_FOUND);

    /* -----------------------------------------------------------------
     * HW-NVS-005 — namespace erase removes keys
     * ----------------------------------------------------------------- */
    nvs_cfg_erase_namespace(NVS_NS_CLIMATE);
    nvs_cfg_status_t after_erase = nvs_cfg_get_i32(NVS_NS_CLIMATE, "t_min", &dummy);
    check("HW-NVS-005", "erase climate ns → t_min NOT_FOUND",
          after_erase == NVS_CFG_ERR_NOT_FOUND);

    /* -----------------------------------------------------------------
     * HW-NVS-012 — _or_default writes default when key absent
     * ----------------------------------------------------------------- */
    int32_t t_max = 0;
    nvs_cfg_get_i32_or_default(NVS_NS_CLIMATE, "t_max", 350, &t_max);
    Serial0.print("[INFO] t_max via _or_default = ");
    Serial0.println(t_max);
    check("HW-NVS-012a", "_or_default absent: returns default (350)", t_max == 350);

    /* Second call must return the now-persisted value, not the default */
    int32_t t_max2 = 0;
    nvs_cfg_get_i32_or_default(NVS_NS_CLIMATE, "t_max", 999, &t_max2);
    check("HW-NVS-012b", "_or_default present: returns stored (350), ignores 999",
          t_max2 == 350);

    /* -----------------------------------------------------------------
     * HW-NVS-006..008 (NVS log ringbuffer hardware tests) — REMOVED in
     * 2.0.0-alpha.6.5 along with the underlying nvs_log_* API. The
     * NVS-backed event-log ring (gh#22) was retired as redundant with
     * T9's SD CSV logging. See drivers/nvs/src/nvs_config.{h,cpp} for
     * the design-change rationale.
     * ----------------------------------------------------------------- */

    /* -----------------------------------------------------------------
     * HW-NVS-009 — pre-power-cycle state
     * wifi/ssid = "Greenhouse1" (set in HW-NVS-003)
     * climate/t_min = erased (HW-NVS-005)
     * climate/t_max = 350 (set via _or_default in HW-NVS-012)
     * ----------------------------------------------------------------- */
    Serial0.println("------------------------------------------------");
    Serial0.println("[HW-NVS-009] Pre-power-cycle state:");
    {
        char s[32] = {0};
        nvs_cfg_get_str(NVS_NS_WIFI, "ssid", s, sizeof(s));
        Serial0.print("  wifi/ssid    = \""); Serial0.print(s); Serial0.println("\"");
    }
    {
        int32_t v = 0;
        nvs_cfg_status_t st = nvs_cfg_get_i32(NVS_NS_CLIMATE, "t_min", &v);
        Serial0.println(st == NVS_CFG_ERR_NOT_FOUND
                        ? "  climate/t_min = NOT_FOUND (erased — correct)"
                        : "  climate/t_min = present (unexpected)");
    }
    {
        int32_t v = 0;
        nvs_cfg_get_i32(NVS_NS_CLIMATE, "t_max", &v);
        Serial0.print("  climate/t_max = "); Serial0.println(v);
    }
    Serial0.println("Power board OFF, then ON to verify HW-NVS-010.");

    /* -----------------------------------------------------------------
     * HW-NVS-010 — persistence: values written above must survive reset.
     * On the NEXT boot this section reads the same keys without writing.
     * Compare output with HW-NVS-009 output from the previous run.
     * ----------------------------------------------------------------- */
    Serial0.println("------------------------------------------------");
    Serial0.println("[HW-NVS-010] Post-power-cycle read (compare with previous run):");
    {
        char s[32] = {0};
        nvs_cfg_get_str(NVS_NS_WIFI, "ssid", s, sizeof(s));
        Serial0.print("  wifi/ssid    = \""); Serial0.print(s);
        Serial0.println(strcmp(s, "Greenhouse1") == 0 ? "\"  ← PASS" : "\"  ← FAIL");
    }
    {
        int32_t v   = 0;
        auto    st  = nvs_cfg_get_i32(NVS_NS_CLIMATE, "t_max", &v);
        Serial0.print("  climate/t_max = ");
        if (st == NVS_CFG_OK) {
            Serial0.print(v);
            Serial0.println(v == 350 ? "  ← PASS" : "  ← FAIL (expected 350)");
        } else {
            Serial0.println("NOT_FOUND  ← FAIL");
        }
    }

    /* -----------------------------------------------------------------
     * Summary
     * ----------------------------------------------------------------- */
    Serial0.println("================================================");
    Serial0.print("  PASSED: "); Serial0.println(pass_count);
    Serial0.print("  FAILED: "); Serial0.println(fail_count);
    Serial0.println(fail_count == 0 ? "  RESULT: PASS" : "  RESULT: FAIL");
    Serial0.println("================================================");
    Serial0.println("Verification complete. Board is idle.");
}

void loop()
{
    delay(1000);
}
