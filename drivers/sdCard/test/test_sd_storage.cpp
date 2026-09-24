/**
 * LIB-8 SD Card — unit tests (native build)
 *
 * Test IDs: UT-SD-001 … UT-SD-012
 *
 * Run with:
 *   export PATH="/c/Program Files/CodeBlocks/MinGW/bin:$PATH"
 *   ~/.platformio/penv/Scripts/pio.exe test -e native
 */

#include <unity.h>
#include <string.h>
#include "../src/sd_storage.h"
#include "mock_sd.h"

void setUp(void)
{
    mock_sd_reset();
}

void tearDown(void) {}

/* ---------------------------------------------------------------------------
 * UT-SD-001 — storage_init → STORAGE_ERR_NO_CARD when mock reports no card
 * --------------------------------------------------------------------------- */
void test_init_no_card(void)
{
    mock_sd_set_card_present(false);
    TEST_ASSERT_EQUAL_INT(STORAGE_ERR_NO_CARD, storage_init());
}

/* ---------------------------------------------------------------------------
 * UT-SD-002 — storage_sd_available false when not mounted
 * --------------------------------------------------------------------------- */
void test_available_false_when_not_mounted(void)
{
    mock_sd_set_card_present(false);
    storage_init();
    TEST_ASSERT_FALSE(storage_sd_available());
}

/* ---------------------------------------------------------------------------
 * UT-SD-003 — write_append creates file; content matches line
 * --------------------------------------------------------------------------- */
void test_write_append_creates_file(void)
{
    TEST_ASSERT_EQUAL_INT(STORAGE_OK, storage_init());
    TEST_ASSERT_EQUAL_INT(STORAGE_OK,
        storage_sd_write_append("/test.csv", "hello,world\n"));
    TEST_ASSERT_GREATER_THAN_UINT32(0, storage_sd_file_size("/test.csv"));

    char buf[64] = {0};
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(STORAGE_OK,
        storage_sd_read("/test.csv", 0, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_STRING("hello,world\n", buf);
}

/* ---------------------------------------------------------------------------
 * UT-SD-004 — write_append appends to existing file; both lines present
 * --------------------------------------------------------------------------- */
void test_write_append_appends(void)
{
    TEST_ASSERT_EQUAL_INT(STORAGE_OK, storage_init());
    storage_sd_write_append("/test.csv", "line1\n");
    storage_sd_write_append("/test.csv", "line2\n");

    char buf[64] = {0};
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(STORAGE_OK,
        storage_sd_read("/test.csv", 0, buf, sizeof(buf), &n));
    TEST_ASSERT_NOT_NULL(strstr(buf, "line1\n"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "line2\n"));
    /* line1 must appear before line2 */
    TEST_ASSERT_LESS_THAN(strstr(buf, "line2\n"), strstr(buf, "line1\n") + 1);
}

/* ---------------------------------------------------------------------------
 * UT-SD-005 — read offset=0 reads from start
 * --------------------------------------------------------------------------- */
void test_read_offset_zero(void)
{
    TEST_ASSERT_EQUAL_INT(STORAGE_OK, storage_init());
    storage_sd_write_append("/test.csv", "ABCDEFGH\n");

    char buf[32] = {0};
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(STORAGE_OK,
        storage_sd_read("/test.csv", 0, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_STRING("ABCDEFGH\n", buf);
}

/* ---------------------------------------------------------------------------
 * UT-SD-006 — read non-zero offset skips bytes
 * --------------------------------------------------------------------------- */
void test_read_nonzero_offset(void)
{
    TEST_ASSERT_EQUAL_INT(STORAGE_OK, storage_init());
    storage_sd_write_append("/test.csv", "ABCDEFGH");

    char buf[16] = {0};
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(STORAGE_OK,
        storage_sd_read("/test.csv", 4, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_STRING("EFGH", buf);
}

/* ---------------------------------------------------------------------------
 * UT-SD-007 — file_size returns correct byte count
 * --------------------------------------------------------------------------- */
void test_file_size_correct(void)
{
    TEST_ASSERT_EQUAL_INT(STORAGE_OK, storage_init());
    const char *data = "12345678901234567890"; /* 20 bytes */
    storage_sd_write_append("/test.csv", data);
    TEST_ASSERT_EQUAL_UINT32(20, storage_sd_file_size("/test.csv"));
}

/* ---------------------------------------------------------------------------
 * UT-SD-008 — file_size of non-existent file → 0; no crash
 * --------------------------------------------------------------------------- */
void test_file_size_nonexistent(void)
{
    TEST_ASSERT_EQUAL_INT(STORAGE_OK, storage_init());
    TEST_ASSERT_EQUAL_UINT32(0, storage_sd_file_size("/nofile.csv"));
}

/* ---------------------------------------------------------------------------
 * UT-SD-009 — list_csv ".csv" excludes ".txt" files
 * --------------------------------------------------------------------------- */
void test_list_csv_excludes_txt(void)
{
    TEST_ASSERT_EQUAL_INT(STORAGE_OK, storage_init());
    storage_sd_write_append("/data.csv", "x\n");
    storage_sd_write_append("/notes.txt", "y\n");
    storage_sd_write_append("/log.csv",   "z\n");

    char buf[128] = {0};
    TEST_ASSERT_EQUAL_INT(STORAGE_OK,
        storage_sd_list_csv(".csv", buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "data.csv"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "log.csv"));
    TEST_ASSERT_NULL(strstr(buf, "notes.txt"));
}

/* ---------------------------------------------------------------------------
 * UT-SD-010 — delete removes file; subsequent file_size returns 0
 * --------------------------------------------------------------------------- */
void test_delete_removes_file(void)
{
    TEST_ASSERT_EQUAL_INT(STORAGE_OK, storage_init());
    storage_sd_write_append("/del.csv", "data\n");
    TEST_ASSERT_EQUAL_INT(STORAGE_OK, storage_sd_delete("/del.csv"));
    TEST_ASSERT_EQUAL_UINT32(0, storage_sd_file_size("/del.csv"));
}

/* ---------------------------------------------------------------------------
 * UT-SD-011 — delete non-existent file → STORAGE_ERR_NOT_FOUND
 * --------------------------------------------------------------------------- */
void test_delete_nonexistent(void)
{
    TEST_ASSERT_EQUAL_INT(STORAGE_OK, storage_init());
    TEST_ASSERT_EQUAL_INT(STORAGE_ERR_NOT_FOUND,
                          storage_sd_delete("/ghost.csv"));
}

/* ---------------------------------------------------------------------------
 * UT-SD-012 — read with buf_len smaller than file: truncates and NUL-terminates
 * --------------------------------------------------------------------------- */
void test_read_truncates_and_null_terminates(void)
{
    TEST_ASSERT_EQUAL_INT(STORAGE_OK, storage_init());
    storage_sd_write_append("/test.csv", "ABCDEFGHIJKLMNOPQRSTUVWXYZ");

    unsigned char buf[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(STORAGE_OK,
        storage_sd_read("/test.csv", 0, reinterpret_cast<char *>(buf), sizeof(buf), &n));
    /* buf_len = 5 → at most 4 bytes copied + NUL terminator */
    TEST_ASSERT_EQUAL_UINT(4, n);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[4]);
    /* No overrun: last byte must be NUL */
    TEST_ASSERT_EQUAL_CHAR('\0', buf[n]);
}

/* ---------------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * UT-SD-013 — foreach visits every match, where list_csv silently truncates
 *
 * gh#82: the listing drops the names that do not fit and still returns OK, so
 * a caller that counts files, or picks the oldest or newest among them, is
 * deciding from a partial view. 40 files is more than any sensible list buffer
 * holds; the iterator must see all of them.
 * ------------------------------------------------------------------------- */
struct visit_ctx {
    int count;
    char first[32];
    char last[32];
};

static void visit_cb(const char *name, void *ctx)
{
    struct visit_ctx *v = (struct visit_ctx *)ctx;
    v->count++;
    if (v->count == 1) {
        snprintf(v->first, sizeof(v->first), "%s", name);
    }
    snprintf(v->last, sizeof(v->last), "%s", name);
}

static void test_foreach_sees_every_file_where_the_listing_truncates(void)
{
    mock_sd_reset();
    TEST_ASSERT_EQUAL_INT(STORAGE_OK, storage_init());
    for (int i = 0; i < 40; i++) {
        char name[40];
        snprintf(name, sizeof(name), "/2344_202609%02d000000.csv", i + 1);
        TEST_ASSERT_EQUAL_INT(STORAGE_OK, storage_sd_write_append(name, "h\n"));
    }

    struct visit_ctx v = { 0, "", "" };
    TEST_ASSERT_EQUAL_INT(STORAGE_OK, storage_sd_foreach_csv(".csv", visit_cb, &v));
    TEST_ASSERT_EQUAL_INT(40, v.count);

    /* The control: a buffer the size the event logger used holds far fewer,
     * and says STORAGE_OK all the same. */
    char small[871];
    TEST_ASSERT_EQUAL_INT(STORAGE_OK, storage_sd_list_csv(".csv", small, sizeof(small)));
    int listed = 0;
    for (const char *p = small; *p; p++) {
        if (*p == ',') listed++;
    }
    TEST_ASSERT_TRUE(listed < 40);
}

/* ---------------------------------------------------------------------------
 * UT-SD-014 — foreach filters by extension, and refuses what it cannot do
 * ------------------------------------------------------------------------- */
static void test_foreach_filters_and_refuses(void)
{
    mock_sd_reset();
    TEST_ASSERT_EQUAL_INT(STORAGE_OK, storage_init());
    TEST_ASSERT_EQUAL_INT(STORAGE_OK, storage_sd_write_append("/2344_20260924000000.csv", "h\n"));
    TEST_ASSERT_EQUAL_INT(STORAGE_OK, storage_sd_write_append("/notes.txt", "x\n"));

    struct visit_ctx v = { 0, "", "" };
    TEST_ASSERT_EQUAL_INT(STORAGE_OK, storage_sd_foreach_csv(".csv", visit_cb, &v));
    TEST_ASSERT_EQUAL_INT(1, v.count);
    TEST_ASSERT_EQUAL_STRING("2344_20260924000000.csv", v.first);

    TEST_ASSERT_EQUAL_INT(STORAGE_ERR_PARAM, storage_sd_foreach_csv(".csv", NULL, &v));
    TEST_ASSERT_EQUAL_INT(STORAGE_ERR_PARAM, storage_sd_foreach_csv(NULL, visit_cb, &v));

    mock_sd_set_card_present(false);
    storage_sd_unmount();
    TEST_ASSERT_EQUAL_INT(STORAGE_ERR_NO_CARD, storage_sd_foreach_csv(".csv", visit_cb, &v));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_no_card);
    RUN_TEST(test_available_false_when_not_mounted);
    RUN_TEST(test_write_append_creates_file);
    RUN_TEST(test_write_append_appends);
    RUN_TEST(test_read_offset_zero);
    RUN_TEST(test_read_nonzero_offset);
    RUN_TEST(test_file_size_correct);
    RUN_TEST(test_file_size_nonexistent);
    RUN_TEST(test_list_csv_excludes_txt);
    RUN_TEST(test_delete_removes_file);
    RUN_TEST(test_delete_nonexistent);
    RUN_TEST(test_read_truncates_and_null_terminates);
    RUN_TEST(test_foreach_sees_every_file_where_the_listing_truncates);
    RUN_TEST(test_foreach_filters_and_refuses);
    return UNITY_END();
}
