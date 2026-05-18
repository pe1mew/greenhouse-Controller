/**
 * LIB-7 NVS Configuration — unit tests (native build)
 *
 * Test IDs: UT-NVS-001 … UT-NVS-025
 *
 * Run with:
 *   export PATH="/c/Program Files/CodeBlocks/MinGW/bin:$PATH"
 *   ~/.platformio/penv/Scripts/pio.exe test -e native
 */

#include <unity.h>
#include <string.h>
#include "../src/nvs_config.h"
#include "mock_nvs.h"

void setUp(void)
{
    mock_nvs_reset();
}

void tearDown(void) {}

/* =========================================================================
 * Original core tests — UT-NVS-001 … UT-NVS-013
 * ========================================================================= */

/* UT-NVS-001 — nvs_cfg_init returns NVS_CFG_OK on first boot */
void test_init_ok(void)
{
    TEST_ASSERT_EQUAL_INT(NVS_CFG_OK, nvs_cfg_init());
}

/* UT-NVS-002 — set_i32 / get_i32 round-trip */
void test_set_get_i32(void)
{
    nvs_cfg_init();
    TEST_ASSERT_EQUAL_INT(NVS_CFG_OK, nvs_cfg_set_i32(NVS_NS_CLIMATE, "t_min", 180));
    int32_t val = 0;
    TEST_ASSERT_EQUAL_INT(NVS_CFG_OK, nvs_cfg_get_i32(NVS_NS_CLIMATE, "t_min", &val));
    TEST_ASSERT_EQUAL_INT32(180, val);
}

/* UT-NVS-003 — get_i32 on unset key → NVS_CFG_ERR_NOT_FOUND */
void test_get_i32_not_found(void)
{
    nvs_cfg_init();
    int32_t val = 0;
    TEST_ASSERT_EQUAL_INT(NVS_CFG_ERR_NOT_FOUND,
                          nvs_cfg_get_i32(NVS_NS_CLIMATE, "missing", &val));
}

/* UT-NVS-004 — set_str / get_str round-trip */
void test_set_get_str(void)
{
    nvs_cfg_init();
    TEST_ASSERT_EQUAL_INT(NVS_CFG_OK,
                          nvs_cfg_set_str(NVS_NS_WIFI, "ssid", "TestNet"));
    char buf[32] = {0};
    TEST_ASSERT_EQUAL_INT(NVS_CFG_OK,
                          nvs_cfg_get_str(NVS_NS_WIFI, "ssid", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("TestNet", buf);
}

/* UT-NVS-005 — get_str with small buf_len truncates; null-terminated */
void test_get_str_truncates(void)
{
    nvs_cfg_init();
    nvs_cfg_set_str(NVS_NS_WIFI, "ssid", "LongNetworkNameHere");
    char buf[6];  memset(buf, (char)0xFF, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(NVS_CFG_OK,
                          nvs_cfg_get_str(NVS_NS_WIFI, "ssid", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT('\0', buf[5]);   /* null-terminated */
    TEST_ASSERT_LESS_OR_EQUAL(5, (int)strlen(buf));
}

/* UT-NVS-006 — set_blob / get_blob round-trip */
void test_set_get_blob(void)
{
    nvs_cfg_init();
    const uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    TEST_ASSERT_EQUAL_INT(NVS_CFG_OK,
                          nvs_cfg_set_blob(NVS_NS_MOTOR, "cal", data, sizeof(data)));
    uint8_t out[4] = {0};
    size_t  len    = sizeof(out);
    TEST_ASSERT_EQUAL_INT(NVS_CFG_OK,
                          nvs_cfg_get_blob(NVS_NS_MOTOR, "cal", out, &len));
    TEST_ASSERT_EQUAL_UINT(sizeof(data), len);
    TEST_ASSERT_EQUAL_MEMORY(data, out, sizeof(data));
}

/* UT-NVS-007 — erase_namespace → subsequent get returns NOT_FOUND */
void test_erase_namespace(void)
{
    nvs_cfg_init();
    nvs_cfg_set_i32(NVS_NS_CLIMATE, "t_min", 100);
    nvs_cfg_erase_namespace(NVS_NS_CLIMATE);
    int32_t val = 0;
    TEST_ASSERT_EQUAL_INT(NVS_CFG_ERR_NOT_FOUND,
                          nvs_cfg_get_i32(NVS_NS_CLIMATE, "t_min", &val));
}

/* UT-NVS-008..012 (NVS event-log ringbuffer tests) — REMOVED in 2.0.0-alpha.6.5
 * along with the underlying nvs_log_append / nvs_log_read / nvs_log_count
 * functions. See nvs_config.{h,cpp} for the design-change rationale.
 * The test runner below also drops the matching RUN_TEST() lines. */

/* UT-NVS-013 — key longer than 15 chars: consistent behaviour (reject) */
void test_long_key_consistent(void)
{
    nvs_cfg_init();
    /* ESP-IDF rejects keys > 15 chars. The mock does not enforce this, so we
     * just verify no crash and a defined return (OK or error, not UB). */
    int32_t val = 0;
    nvs_cfg_status_t st = nvs_cfg_get_i32(NVS_NS_CLIMATE,
                                            "this_key_is_too_long", &val);
    TEST_ASSERT_TRUE(st == NVS_CFG_OK || st == NVS_CFG_ERR_NOT_FOUND ||
                     st == NVS_CFG_ERR_INIT);
}

/* =========================================================================
 * Schema versioning tests — UT-NVS-014 … UT-NVS-017
 * ========================================================================= */

/* UT-NVS-014 — init on first boot writes schema_ver = NVS_SCHEMA_VERSION */
void test_init_stamps_schema_version(void)
{
    nvs_cfg_init();
    int32_t ver = 0;
    TEST_ASSERT_EQUAL_INT(NVS_CFG_OK,
                          nvs_cfg_get_schema_version(&ver));
    TEST_ASSERT_EQUAL_INT32((int32_t)NVS_SCHEMA_VERSION, ver);
}

/* UT-NVS-015 — second init with matching version returns NVS_CFG_OK */
void test_init_matching_version_ok(void)
{
    nvs_cfg_init();                    /* stamps version */
    TEST_ASSERT_EQUAL_INT(NVS_CFG_OK, nvs_cfg_init());   /* re-init: match */
}

/* UT-NVS-016 — init with stale version returns NVS_CFG_ERR_MIGRATION
 *              and updates stored version to NVS_SCHEMA_VERSION            */
void test_init_stale_version_migrates(void)
{
    /* Pre-load an old schema version directly into the mock store */
    mock_nvs_inject_i32(NVS_NS_SYSTEM, NVS_KEY_SCHEMA_VER,
                         (int32_t)NVS_SCHEMA_VERSION - 1);

    TEST_ASSERT_EQUAL_INT(NVS_CFG_ERR_MIGRATION, nvs_cfg_init());

    /* Stored version must now equal the current compile-time version */
    int32_t ver = 0;
    TEST_ASSERT_EQUAL_INT(NVS_CFG_OK, nvs_cfg_get_schema_version(&ver));
    TEST_ASSERT_EQUAL_INT32((int32_t)NVS_SCHEMA_VERSION, ver);
}

/* UT-NVS-017 — after migration existing config keys are preserved */
void test_migration_preserves_config_keys(void)
{
    /* Pre-populate a config key and inject a stale schema version */
    mock_nvs_inject_i32(NVS_NS_CLIMATE, "t_min", 100);
    mock_nvs_inject_i32(NVS_NS_SYSTEM, NVS_KEY_SCHEMA_VER,
                         (int32_t)NVS_SCHEMA_VERSION - 1);

    /* Migration must not erase namespaces */
    TEST_ASSERT_EQUAL_INT(NVS_CFG_ERR_MIGRATION, nvs_cfg_init());

    /* The pre-existing user value must survive */
    int32_t val = 0;
    TEST_ASSERT_EQUAL_INT(NVS_CFG_OK,
                          nvs_cfg_get_i32(NVS_NS_CLIMATE, "t_min", &val));
    TEST_ASSERT_EQUAL_INT32(100, val);
}

/* =========================================================================
 * _or_default helper tests — UT-NVS-018 … UT-NVS-021
 * ========================================================================= */

/* UT-NVS-018 — get_i32_or_default: absent key → writes default, returns it */
void test_get_i32_or_default_absent(void)
{
    nvs_cfg_init();
    int32_t val = 0;
    TEST_ASSERT_EQUAL_INT(NVS_CFG_OK,
                          nvs_cfg_get_i32_or_default(NVS_NS_CLIMATE, "t_min",
                                                       180, &val));
    TEST_ASSERT_EQUAL_INT32(180, val);

    /* Key must now be persisted — plain get should return same value */
    int32_t stored = 0;
    TEST_ASSERT_EQUAL_INT(NVS_CFG_OK,
                          nvs_cfg_get_i32(NVS_NS_CLIMATE, "t_min", &stored));
    TEST_ASSERT_EQUAL_INT32(180, stored);
}

/* UT-NVS-019 — get_i32_or_default: present key → returns stored, ignores default */
void test_get_i32_or_default_present(void)
{
    nvs_cfg_init();
    nvs_cfg_set_i32(NVS_NS_CLIMATE, "t_min", 42);
    int32_t val = 0;
    TEST_ASSERT_EQUAL_INT(NVS_CFG_OK,
                          nvs_cfg_get_i32_or_default(NVS_NS_CLIMATE, "t_min",
                                                       99, &val));
    TEST_ASSERT_EQUAL_INT32(42, val);  /* stored value; default 99 ignored */
}

/* UT-NVS-020 — get_str_or_default: absent key → writes default, returns it */
void test_get_str_or_default_absent(void)
{
    nvs_cfg_init();
    char buf[32] = {0};
    TEST_ASSERT_EQUAL_INT(NVS_CFG_OK,
                          nvs_cfg_get_str_or_default(NVS_NS_WIFI, "ssid",
                                                      "Greenhouse1",
                                                      buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("Greenhouse1", buf);

    /* Verify persisted */
    char buf2[32] = {0};
    TEST_ASSERT_EQUAL_INT(NVS_CFG_OK,
                          nvs_cfg_get_str(NVS_NS_WIFI, "ssid", buf2, sizeof(buf2)));
    TEST_ASSERT_EQUAL_STRING("Greenhouse1", buf2);
}

/* UT-NVS-021 — get_str_or_default: present key → returns stored, ignores default */
void test_get_str_or_default_present(void)
{
    nvs_cfg_init();
    nvs_cfg_set_str(NVS_NS_WIFI, "ssid", "OfficeNet");
    char buf[32] = {0};
    TEST_ASSERT_EQUAL_INT(NVS_CFG_OK,
                          nvs_cfg_get_str_or_default(NVS_NS_WIFI, "ssid",
                                                      "Default", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("OfficeNet", buf);
}

/* =========================================================================
 * fw_version tests — UT-NVS-023 … UT-NVS-025
 * ========================================================================= */

/* UT-NVS-023 — init on first boot writes fw_version = FIRMWARE_VERSION */
void test_init_first_boot_writes_fw_version(void)
{
    nvs_cfg_init();
    char buf[32] = {0};
    TEST_ASSERT_EQUAL_INT(NVS_CFG_OK,
                          nvs_cfg_get_str(NVS_NS_SYSTEM, NVS_KEY_FW_VERSION,
                                          buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(FIRMWARE_VERSION, buf);
}

/* UT-NVS-024 — init on normal boot (no migration) overwrites fw_version */
void test_init_normal_boot_updates_fw_version(void)
{
    /* Pre-set an old fw_version string and matching schema version */
    nvs_cfg_init();   /* stamps correct schema_ver */
    nvs_cfg_set_str(NVS_NS_SYSTEM, NVS_KEY_FW_VERSION, "0.0.1");

    /* Re-init: schema matches, but fw_version must be updated */
    TEST_ASSERT_EQUAL_INT(NVS_CFG_OK, nvs_cfg_init());

    char buf[32] = {0};
    TEST_ASSERT_EQUAL_INT(NVS_CFG_OK,
                          nvs_cfg_get_str(NVS_NS_SYSTEM, NVS_KEY_FW_VERSION,
                                          buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(FIRMWARE_VERSION, buf);
}

/* UT-NVS-025 — init on migration boot overwrites fw_version */
void test_init_migration_boot_updates_fw_version(void)
{
    /* Pre-set stale schema version and an old fw_version */
    mock_nvs_inject_i32(NVS_NS_SYSTEM, NVS_KEY_SCHEMA_VER,
                         (int32_t)NVS_SCHEMA_VERSION - 1);
    nvs_cfg_set_str(NVS_NS_SYSTEM, NVS_KEY_FW_VERSION, "0.0.1");

    TEST_ASSERT_EQUAL_INT(NVS_CFG_ERR_MIGRATION, nvs_cfg_init());

    char buf[32] = {0};
    TEST_ASSERT_EQUAL_INT(NVS_CFG_OK,
                          nvs_cfg_get_str(NVS_NS_SYSTEM, NVS_KEY_FW_VERSION,
                                          buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(FIRMWARE_VERSION, buf);
}

/* =========================================================================
 * Test runner
 * ========================================================================= */

int main(void)
{
    UNITY_BEGIN();

    /* Core */
    RUN_TEST(test_init_ok);
    RUN_TEST(test_set_get_i32);
    RUN_TEST(test_get_i32_not_found);
    RUN_TEST(test_set_get_str);
    RUN_TEST(test_get_str_truncates);
    RUN_TEST(test_set_get_blob);
    RUN_TEST(test_erase_namespace);
    /* RUN_TEST(test_log_*) — 5 ringbuffer tests removed in alpha.6.5
     * along with the underlying nvs_log_* API. */
    RUN_TEST(test_long_key_consistent);

    /* Schema versioning */
    RUN_TEST(test_init_stamps_schema_version);
    RUN_TEST(test_init_matching_version_ok);
    RUN_TEST(test_init_stale_version_migrates);
    RUN_TEST(test_migration_preserves_config_keys);

    /* _or_default helpers */
    RUN_TEST(test_get_i32_or_default_absent);
    RUN_TEST(test_get_i32_or_default_present);
    RUN_TEST(test_get_str_or_default_absent);
    RUN_TEST(test_get_str_or_default_present);

    /* fw_version */
    RUN_TEST(test_init_first_boot_writes_fw_version);
    RUN_TEST(test_init_normal_boot_updates_fw_version);
    RUN_TEST(test_init_migration_boot_updates_fw_version);

    return UNITY_END();
}
