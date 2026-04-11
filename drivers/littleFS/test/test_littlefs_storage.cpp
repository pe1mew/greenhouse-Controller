/**
 * LIB-9 LittleFS — unit tests (native build)
 *
 * Test IDs: UT-LFS-001 … UT-LFS-012
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
    return UNITY_END();
}
