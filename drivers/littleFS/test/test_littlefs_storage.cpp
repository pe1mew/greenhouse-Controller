/**
 * LIB-9 LittleFS — unit tests (native build)
 *
 * Test IDs: UT-LFS-001 … UT-LFS-018
 *
 * Run with:
 *   export PATH="/c/Program Files/CodeBlocks/MinGW/bin:$PATH"
 *   ~/.platformio/penv/Scripts/pio.exe test -e native
 */

#include <unity.h>
#include <string.h>
#include "../src/littlefs_storage.h"
#include "mock_lfs.h"

void setUp(void)
{
    /* The driver's mount flags outlive a test; without this a test would see a
     * partition an earlier one left mounted, and its mount would be a no-op. */
    littlefs_unmount(LFS_PARTITION_A);
    littlefs_unmount(LFS_PARTITION_B);
    mock_lfs_reset();
}

void tearDown(void) {}

/* ---------------------------------------------------------------------------
 * UT-LFS-001 — littlefs_mount(A) returns LFS_OK
 * --------------------------------------------------------------------------- */
void test_mount_partition_a(void)
{
    TEST_ASSERT_EQUAL_INT(LFS_OK, littlefs_mount(LFS_PARTITION_A));
}

/* ---------------------------------------------------------------------------
 * UT-LFS-002 — littlefs_mount(B) returns LFS_OK
 * --------------------------------------------------------------------------- */
void test_mount_partition_b(void)
{
    TEST_ASSERT_EQUAL_INT(LFS_OK, littlefs_mount(LFS_PARTITION_B));
}

/* ---------------------------------------------------------------------------
 * UT-LFS-003 — littlefs_read returns content of existing file on correct partition
 * --------------------------------------------------------------------------- */
void test_read_existing_file(void)
{
    littlefs_mount(LFS_PARTITION_A);
    littlefs_write(LFS_PARTITION_A, "/hello.txt", "world", 5);

    char buf[16] = {0};
    lfs_status_t st = littlefs_read(LFS_PARTITION_A, "/hello.txt", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(LFS_OK, st);
    TEST_ASSERT_EQUAL_STRING("world", buf);
    /* NUL-terminated */
    TEST_ASSERT_EQUAL_CHAR('\0', buf[5]);
}

/* ---------------------------------------------------------------------------
 * UT-LFS-004 — littlefs_read on missing file → LFS_ERR_NOT_FOUND
 * --------------------------------------------------------------------------- */
void test_read_missing_file(void)
{
    littlefs_mount(LFS_PARTITION_A);
    char buf[16] = {0};
    TEST_ASSERT_EQUAL_INT(LFS_ERR_NOT_FOUND,
        littlefs_read(LFS_PARTITION_A, "/ghost.txt", buf, sizeof(buf)));
}

/* ---------------------------------------------------------------------------
 * UT-LFS-005 — littlefs_read with buf_len smaller than file truncates and
 *              null-terminates; no buffer overrun
 * --------------------------------------------------------------------------- */
void test_read_truncates(void)
{
    littlefs_mount(LFS_PARTITION_A);
    littlefs_write(LFS_PARTITION_A, "/big.txt", "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 26);

    char buf[5] = {0};
    lfs_status_t st = littlefs_read(LFS_PARTITION_A, "/big.txt", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(LFS_OK, st);
    /* At most 4 bytes + NUL */
    TEST_ASSERT_EQUAL_CHAR('\0', buf[4]);
    TEST_ASSERT_EQUAL_CHAR('A', buf[0]);
}

/* ---------------------------------------------------------------------------
 * UT-LFS-006 — littlefs_write creates file on specified partition; content correct
 * --------------------------------------------------------------------------- */
void test_write_creates_file(void)
{
    littlefs_mount(LFS_PARTITION_B);
    lfs_status_t st = littlefs_write(LFS_PARTITION_B, "/new.txt", "data", 4);
    TEST_ASSERT_EQUAL_INT(LFS_OK, st);

    char buf[16] = {0};
    littlefs_read(LFS_PARTITION_B, "/new.txt", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("data", buf);
}

/* ---------------------------------------------------------------------------
 * UT-LFS-007 — littlefs_write on partition B does not affect partition A
 * --------------------------------------------------------------------------- */
void test_write_partition_isolation(void)
{
    littlefs_mount(LFS_PARTITION_A);
    littlefs_mount(LFS_PARTITION_B);
    littlefs_write(LFS_PARTITION_B, "/isolated.txt", "B-only", 6);

    char buf[16] = {0};
    lfs_status_t st = littlefs_read(LFS_PARTITION_A, "/isolated.txt", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(LFS_ERR_NOT_FOUND, st);
}

/* ---------------------------------------------------------------------------
 * UT-LFS-008 — littlefs_write overwrites existing file; only second content present
 * --------------------------------------------------------------------------- */
void test_write_overwrites(void)
{
    littlefs_mount(LFS_PARTITION_A);
    littlefs_write(LFS_PARTITION_A, "/ow.txt", "first", 5);
    littlefs_write(LFS_PARTITION_A, "/ow.txt", "second", 6);

    char buf[16] = {0};
    littlefs_read(LFS_PARTITION_A, "/ow.txt", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("second", buf);
    TEST_ASSERT_NULL(strstr(buf, "first"));
}

/* ---------------------------------------------------------------------------
 * UT-LFS-009 — littlefs_exists returns true for existing file
 * --------------------------------------------------------------------------- */
void test_exists_true(void)
{
    littlefs_mount(LFS_PARTITION_A);
    littlefs_write(LFS_PARTITION_A, "/present.txt", "x", 1);
    TEST_ASSERT_TRUE(littlefs_exists(LFS_PARTITION_A, "/present.txt"));
}

/* ---------------------------------------------------------------------------
 * UT-LFS-010 — littlefs_exists returns false for absent file
 * --------------------------------------------------------------------------- */
void test_exists_false(void)
{
    littlefs_mount(LFS_PARTITION_A);
    TEST_ASSERT_FALSE(littlefs_exists(LFS_PARTITION_A, "/absent.txt"));
}

/* ---------------------------------------------------------------------------
 * UT-LFS-011 — littlefs_active_partition returns partition matching active bank
 * --------------------------------------------------------------------------- */
void test_active_partition(void)
{
    mock_lfs_set_active_partition(LFS_PARTITION_A);
    TEST_ASSERT_EQUAL_INT(LFS_PARTITION_A, littlefs_active_partition());

    mock_lfs_set_active_partition(LFS_PARTITION_B);
    TEST_ASSERT_EQUAL_INT(LFS_PARTITION_B, littlefs_active_partition());
}

/* ---------------------------------------------------------------------------
 * UT-LFS-012 — littlefs_unmount allows remount of same partition
 * --------------------------------------------------------------------------- */
void test_unmount_and_remount(void)
{
    TEST_ASSERT_EQUAL_INT(LFS_OK, littlefs_mount(LFS_PARTITION_A));
    littlefs_unmount(LFS_PARTITION_A);
    TEST_ASSERT_EQUAL_INT(LFS_OK, littlefs_mount(LFS_PARTITION_A));
}

/* ---------------------------------------------------------------------------
 * gh#89 — which mount failures a format cures
 *
 * On 2026-10-03/04 T13 formatted an intact inactive partition because its
 * mount was refused by a full VFS table, not for its contents. The driver now
 * tells the two apart, and these pin the rule.
 * --------------------------------------------------------------------------- */

/* UT-LFS-013 — contents refused (esp_littlefs's ESP_FAIL) is LFS_ERR_CORRUPT */
void test_mount_contents_refused_is_corrupt(void)
{
    mock_lfs_set_mount_err(LFS_PARTITION_A, MOCK_ESP_FAIL);
    TEST_ASSERT_EQUAL_INT(LFS_ERR_CORRUPT, littlefs_mount(LFS_PARTITION_A));
    TEST_ASSERT_EQUAL_INT(MOCK_ESP_FAIL, littlefs_last_mount_err());
    TEST_ASSERT_TRUE(littlefs_formatting_cures(LFS_ERR_CORRUPT));
}

/* UT-LFS-014 — refused for anything else is LFS_ERR_MOUNT, which a format does
 * not cure: no VFS slot or memory, the label in use, the label missing */
void test_mount_refused_otherwise_is_mount(void)
{
    static const int errs[] = { MOCK_ESP_ERR_NO_MEM, MOCK_ESP_ERR_INVALID_STATE,
                                MOCK_ESP_ERR_NOT_FOUND };
    for (size_t i = 0; i < sizeof(errs) / sizeof(errs[0]); i++) {
        mock_lfs_set_mount_err(LFS_PARTITION_B, errs[i]);
        const lfs_status_t st = littlefs_mount(LFS_PARTITION_B);
        TEST_ASSERT_EQUAL_INT(LFS_ERR_MOUNT, st);
        TEST_ASSERT_EQUAL_INT(errs[i], littlefs_last_mount_err());
        TEST_ASSERT_FALSE(littlefs_formatting_cures(st));
    }
}

/* UT-LFS-015 — after a contents refusal, a format and a new mount give an
 * empty, mounted partition and clear the error */
void test_format_cures_corruption(void)
{
    TEST_ASSERT_EQUAL_INT(LFS_OK, littlefs_mount(LFS_PARTITION_A));
    TEST_ASSERT_EQUAL_INT(LFS_OK, littlefs_write(LFS_PARTITION_A, "/index.html", "x", 1));
    littlefs_unmount(LFS_PARTITION_A);

    mock_lfs_set_mount_err(LFS_PARTITION_A, MOCK_ESP_FAIL);
    TEST_ASSERT_EQUAL_INT(LFS_ERR_CORRUPT, littlefs_mount(LFS_PARTITION_A));
    TEST_ASSERT_EQUAL_INT(LFS_OK, littlefs_format(LFS_PARTITION_A));
    TEST_ASSERT_EQUAL_INT(LFS_OK, littlefs_mount(LFS_PARTITION_A));
    TEST_ASSERT_FALSE(littlefs_exists(LFS_PARTITION_A, "/index.html"));
    TEST_ASSERT_EQUAL_INT(0, littlefs_last_mount_err());
}

/* UT-LFS-016 — formatting cures LFS_ERR_CORRUPT and no other status */
void test_formatting_cures_only_corrupt(void)
{
    for (int v = LFS_OK; v <= LFS_ERR_CORRUPT; v++) {
        TEST_ASSERT_EQUAL_INT((v == LFS_ERR_CORRUPT) ? 1 : 0,
                              littlefs_formatting_cures((lfs_status_t)v) ? 1 : 0);
    }
}

/* UT-LFS-017 — a successful mount clears the last mount error */
void test_success_clears_last_mount_err(void)
{
    mock_lfs_set_mount_err(LFS_PARTITION_A, MOCK_ESP_ERR_INVALID_STATE);
    TEST_ASSERT_EQUAL_INT(LFS_ERR_MOUNT, littlefs_mount(LFS_PARTITION_A));
    TEST_ASSERT_EQUAL_INT(MOCK_ESP_ERR_INVALID_STATE, littlefs_last_mount_err());
    mock_lfs_set_mount_err(LFS_PARTITION_A, 0);
    TEST_ASSERT_EQUAL_INT(LFS_OK, littlefs_mount(LFS_PARTITION_A));
    TEST_ASSERT_EQUAL_INT(0, littlefs_last_mount_err());
}

/* UT-LFS-018 — the status numbers are stable: logs print them, and
 * LFS_ERR_CORRUPT was appended */
void test_status_numbers_stable(void)
{
    TEST_ASSERT_EQUAL_INT(0, LFS_OK);
    TEST_ASSERT_EQUAL_INT(1, LFS_ERR_MOUNT);
    TEST_ASSERT_EQUAL_INT(2, LFS_ERR_NOT_FOUND);
    TEST_ASSERT_EQUAL_INT(3, LFS_ERR_IO);
    TEST_ASSERT_EQUAL_INT(4, LFS_ERR_FULL);
    TEST_ASSERT_EQUAL_INT(5, LFS_ERR_CORRUPT);
}

/* ---------------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------------- */
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_mount_partition_a);
    RUN_TEST(test_mount_partition_b);
    RUN_TEST(test_read_existing_file);
    RUN_TEST(test_read_missing_file);
    RUN_TEST(test_read_truncates);
    RUN_TEST(test_write_creates_file);
    RUN_TEST(test_write_partition_isolation);
    RUN_TEST(test_write_overwrites);
    RUN_TEST(test_exists_true);
    RUN_TEST(test_exists_false);
    RUN_TEST(test_active_partition);
    RUN_TEST(test_unmount_and_remount);
    RUN_TEST(test_mount_contents_refused_is_corrupt);
    RUN_TEST(test_mount_refused_otherwise_is_mount);
    RUN_TEST(test_format_cures_corruption);
    RUN_TEST(test_formatting_cures_only_corrupt);
    RUN_TEST(test_success_clears_last_mount_err);
    RUN_TEST(test_status_numbers_stable);
    return UNITY_END();
}
