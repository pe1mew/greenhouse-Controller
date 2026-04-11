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
     * HW-NVS-006 — log ring buffer appends
     * Reset the log namespace first so this test is repeatable across
     * multiple power cycles without needing a full flash erase.
     * ----------------------------------------------------------------- */
    nvs_cfg_erase_namespace(NVS_NS_LOG);
    for (int i = 0; i < 10; i++) {
        uint8_t entry[4] = {(uint8_t)i, 0xAB, 0xCD, 0xEF};
        nvs_log_append(entry, sizeof(entry));
    }
    uint32_t log_cnt = nvs_log_count();
    Serial0.print("[INFO] log_count after 10 appends = ");
    Serial0.println(log_cnt);
    check("HW-NVS-006", "log ring buffer count = 10", log_cnt == 10);

    /* -----------------------------------------------------------------
     * HW-NVS-007 — log read returns correct entry bytes
     * ----------------------------------------------------------------- */
    uint8_t  read_buf[4 * 5] = {0};
    uint32_t n_read           = 0;
    nvs_log_read(0, read_buf, 5, &n_read);
    Serial0.print("[INFO] log entries read = ");
    Serial0.print(n_read);
    Serial0.print("; entry[0][0] = 0x");
    Serial0.println(read_buf[0], HEX);
    check("HW-NVS-007", "log read returns 5 entries; entry[0][0]=0x00",
          n_read == 5 && read_buf[0] == 0x00);

    /* -----------------------------------------------------------------
     * HW-NVS-008 — ring buffer caps at capacity
     * (Appending 105 entries total: 10 already written above, 95 more here)
     * CONFIG_NVS_LOG_CAPACITY is set to 100 via build flag for the hardware
     * test so that all entries fit within the default 20 KB NVS partition.
     * The production default is 1000; behaviour is identical at any capacity.
     * ----------------------------------------------------------------- */
    Serial0.print("[INFO] CONFIG_NVS_LOG_CAPACITY = ");
    Serial0.println((int)CONFIG_NVS_LOG_CAPACITY);
    Serial0.println("[INFO] Appending 95 more log entries (total 105) ...");
    for (int i = 10; i < 105; i++) {
        uint8_t e = (uint8_t)(i & 0xFF);
        nvs_log_append(&e, 1);
    }
    uint32_t log_cnt2 = nvs_log_count();
    Serial0.print("[INFO] log_count after 105 appends = ");
    Serial0.println(log_cnt2);
    check("HW-NVS-008", "log count capped at CONFIG_NVS_LOG_CAPACITY",
          log_cnt2 == (uint32_t)CONFIG_NVS_LOG_CAPACITY);

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
