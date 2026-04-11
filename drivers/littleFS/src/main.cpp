/**
 * LIB-9 LittleFS — hardware verification sketch
 *
 * Covers HW-LFS-001 through HW-LFS-009.
 *
 * Pre-condition:
 *   Place a file  littleFS/data/test.html  containing  <h1>OK</h1>
 *   and run:
 *       pio run -e lolin_s3 -t uploadfs
 *   BEFORE uploading this sketch so that HW-LFS-002 and HW-LFS-003 can
 *   verify that pre-flashed assets are accessible via littlefs_read().
 *
 * Serial interface: UART0 on GPIO 43 (TX) / GPIO 44 (RX), 115200 baud.
 * Connect a 3.3 V USB-to-serial adapter to GPIO 43 / GND before reset.
 */

#include <Arduino.h>
#include "littlefs_storage.h"

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
    delay(3000);

    Serial0.println();
    Serial0.println("================================================");
    Serial0.println("  LIB-9 LittleFS — hardware verification");
    Serial0.println("================================================");

    /* -----------------------------------------------------------------
     * HW-LFS-001 — Mount active LittleFS partition
     * ----------------------------------------------------------------- */
    lfs_partition_t active = littlefs_active_partition();
    Serial0.print("[INFO] Active partition: ");
    Serial0.println(active == LFS_PARTITION_A ? "A" : "B");

    lfs_status_t st = littlefs_mount(active);
    Serial0.print("[INFO] littlefs_mount returned: ");
    Serial0.println(st);
    if (st == LFS_OK) {
        Serial0.println("[INFO] LittleFS init OK");
    }
    check("HW-LFS-001", "LittleFS partition mounts", st == LFS_OK);

    if (st != LFS_OK) {
        Serial0.println("[INFO] Mount failed — remaining tests skipped.");
        Serial0.print("PASS: "); Serial0.println(pass_count);
        Serial0.print("FAIL: "); Serial0.println(fail_count);
        return;
    }

    /* -----------------------------------------------------------------
     * HW-LFS-002 — Pre-uploaded file is found
     * ----------------------------------------------------------------- */
    bool exists_test = littlefs_exists(active, "/test.html");
    Serial0.print("[INFO] exists /test.html: ");
    Serial0.println(exists_test ? "true" : "false");
    check("HW-LFS-002", "Pre-uploaded file is found", exists_test);

    /* -----------------------------------------------------------------
     * HW-LFS-003 — Pre-uploaded file content is correct
     * ----------------------------------------------------------------- */
    char html_buf[64] = {0};
    st = littlefs_read(active, "/test.html", html_buf, sizeof(html_buf));
    Serial0.print("[INFO] /test.html content: \"");
    Serial0.print(html_buf);
    Serial0.println("\"");
    check("HW-LFS-003", "Pre-uploaded file content is correct",
          st == LFS_OK && strstr(html_buf, "<h1>OK</h1>") != nullptr);

    /* -----------------------------------------------------------------
     * HW-LFS-004 — Non-existent file returns false on exists check
     * ----------------------------------------------------------------- */
    bool exists_missing = littlefs_exists(active, "/missing.txt");
    Serial0.print("[INFO] exists /missing.txt: ");
    Serial0.println(exists_missing ? "true" : "false");
    check("HW-LFS-004", "Non-existent file returns false", !exists_missing);

    /* -----------------------------------------------------------------
     * HW-LFS-005 — Non-existent file read returns LFS_ERR_NOT_FOUND
     * ----------------------------------------------------------------- */
    char miss_buf[32] = {0};
    st = littlefs_read(active, "/missing.txt", miss_buf, sizeof(miss_buf));
    Serial0.print("[INFO] read /missing.txt returned: ");
    Serial0.println(st);
    check("HW-LFS-005", "Non-existent file read → LFS_ERR_NOT_FOUND",
          st == LFS_ERR_NOT_FOUND);

    /* -----------------------------------------------------------------
     * HW-LFS-006 — Runtime write creates file
     * ----------------------------------------------------------------- */
    st = littlefs_write(active, "/runtime.txt", "hello", 5);
    Serial0.print("[INFO] write /runtime.txt returned: ");
    Serial0.println(st);
    check("HW-LFS-006", "Runtime write creates file", st == LFS_OK);

    /* -----------------------------------------------------------------
     * HW-LFS-007 — Written file content is correct
     * ----------------------------------------------------------------- */
    char rt_buf[16] = {0};
    st = littlefs_read(active, "/runtime.txt", rt_buf, sizeof(rt_buf));
    Serial0.print("[INFO] /runtime.txt content: \"");
    Serial0.print(rt_buf);
    Serial0.println("\"");
    check("HW-LFS-007", "Written file content is correct",
          st == LFS_OK && strcmp(rt_buf, "hello") == 0);

    /* -----------------------------------------------------------------
     * HW-LFS-008 — Overwrite replaces previous content
     * ----------------------------------------------------------------- */
    st = littlefs_write(active, "/runtime.txt", "world", 5);
    char rt_buf2[16] = {0};
    littlefs_read(active, "/runtime.txt", rt_buf2, sizeof(rt_buf2));
    Serial0.print("[INFO] /runtime.txt after overwrite: \"");
    Serial0.print(rt_buf2);
    Serial0.println("\"");
    check("HW-LFS-008", "Overwrite replaces previous content",
          st == LFS_OK &&
          strcmp(rt_buf2, "world") == 0 &&
          strstr(rt_buf2, "hello") == nullptr);

    /* -----------------------------------------------------------------
     * HW-LFS-009 — Free bytes reported
     * ----------------------------------------------------------------- */
    uint64_t free_b = littlefs_free_bytes(active);
    Serial0.print("[INFO] Free bytes on active partition: ");
    Serial0.println((unsigned long)free_b);
    check("HW-LFS-009", "Free bytes > 0", free_b > 0);

    /* -----------------------------------------------------------------
     * Summary
     * ----------------------------------------------------------------- */
    Serial0.println();
    Serial0.println("------------------------------------------------");
    Serial0.print("PASS: "); Serial0.println(pass_count);
    Serial0.print("FAIL: "); Serial0.println(fail_count);
    Serial0.println("------------------------------------------------");
}

void loop() {}
