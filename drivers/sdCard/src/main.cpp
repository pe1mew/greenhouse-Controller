/**
 * LIB-8 SD Card — hardware verification sketch
 *
 * Covers HW-SD-001 through HW-SD-010.
 *
 * Serial interface: UART0 on GPIO 43 (TX) / GPIO 44 (RX), 115200 baud.
 * Connect a 3.3 V USB-to-serial adapter to GPIO 43 / GND before reset.
 *
 * Procedure:
 *   1. Insert a FAT32-formatted SD card (≤ 32 GB).
 *   2. Upload this sketch and open the serial monitor at 115200 baud.
 *   3. The sketch executes all hardware test cases sequentially and prints
 *      [PASS] / [FAIL] for each one.
 *   4. For HW-SD-010 (absent card): remove the card, reset the board,
 *      and observe the STORAGE_ERR_NO_CARD result printed on startup.
 *
 * FAT32 note: SD cards > 32 GB often format as exFAT by default, which the
 * Arduino SD library does not support.  Reformat using:
 *   Windows:  format /FS:FAT32 X:
 *   or the SD Association's SD Formatter tool.
 */

#include <Arduino.h>
#include "sd_storage.h"

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
    Serial0.println("  LIB-8 SD Card — hardware verification");
    Serial0.println("================================================");

    /* -----------------------------------------------------------------
     * HW-SD-001 — SPI bus initialises and card mounts
     * ----------------------------------------------------------------- */
    storage_status_t init_st = storage_init();
    Serial0.print("[INFO] storage_init returned: ");
    Serial0.println(init_st);
    if (init_st == STORAGE_OK) {
        Serial0.println("[INFO] SD card mounted (FAT32)");
    } else if (init_st == STORAGE_ERR_NO_CARD) {
        Serial0.println("[INFO] STORAGE_ERR_NO_CARD — check HW-SD-010");
    }
    check("HW-SD-001", "SPI bus initialises and card mounts",
          init_st == STORAGE_OK);

    if (init_st != STORAGE_OK) {
        /* Card absent — remaining tests cannot run */
        Serial0.println("[INFO] SD absent: remaining tests skipped.");
        Serial0.println();
        Serial0.print("PASS: "); Serial0.println(pass_count);
        Serial0.print("FAIL: "); Serial0.println(fail_count);
        return;
    }

    /* Clean up any artefacts left by a previous test run. */
    storage_sd_delete("/20260410120000.csv");
    storage_sd_delete("/bigfile.csv");

    /* -----------------------------------------------------------------
     * HW-SD-002 — Free space is reported
     * ----------------------------------------------------------------- */
    uint64_t free_bytes = storage_sd_free_bytes();
    Serial0.print("[INFO] Free bytes: ");
    Serial0.println((unsigned long)free_bytes);
    check("HW-SD-002", "Free bytes > 0", free_bytes > 0);

    /* -----------------------------------------------------------------
     * HW-SD-003 — Write-append creates file
     * ----------------------------------------------------------------- */
    const char *test_file = "/20260410120000.csv";
    storage_status_t st = storage_sd_write_append(test_file, "line1,data,value\n");
    Serial0.print("[INFO] Write append line 1: ");
    Serial0.println(st);
    check("HW-SD-003", "Write-append creates file", st == STORAGE_OK);

    /* -----------------------------------------------------------------
     * HW-SD-004 — Write-append grows existing file
     * ----------------------------------------------------------------- */
    uint32_t size_after_1 = storage_sd_file_size(test_file);
    st = storage_sd_write_append(test_file, "line2,data,value\n");
    uint32_t size_after_2 = storage_sd_file_size(test_file);
    Serial0.print("[INFO] File size after 2 appends: ");
    Serial0.print(size_after_2);
    Serial0.println(" bytes");
    check("HW-SD-004", "Write-append grows existing file",
          st == STORAGE_OK && size_after_2 > size_after_1);

    /* -----------------------------------------------------------------
     * HW-SD-005 — Read from offset 0 returns correct content
     * ----------------------------------------------------------------- */
    char read_buf[64] = {0};
    size_t bytes_read = 0;
    st = storage_sd_read(test_file, 0, read_buf, sizeof(read_buf), &bytes_read);
    Serial0.print("[INFO] Read offset 0: \"");
    Serial0.print(read_buf);
    Serial0.println("\"");
    check("HW-SD-005", "Read from offset 0 returns correct content",
          st == STORAGE_OK && strncmp(read_buf, "line1", 5) == 0);

    /* -----------------------------------------------------------------
     * HW-SD-006 — CSV file appears in directory listing
     * ----------------------------------------------------------------- */
    char list_buf[256] = {0};
    st = storage_sd_list_csv(".csv", list_buf, sizeof(list_buf));
    Serial0.print("[INFO] list_csv: ");
    Serial0.println(list_buf);
    check("HW-SD-006", "CSV file appears in directory listing",
          st == STORAGE_OK && strstr(list_buf, "20260410120000.csv") != nullptr);

    /* -----------------------------------------------------------------
     * HW-SD-007 — 512 KB stress write succeeds
     * ----------------------------------------------------------------- */
    const char *big_file = "/bigfile.csv";
    /* Write in 512-byte chunks — 1024 iterations = 512 KB.
     * chunk[513]: 511 × 'X' + '\n' + '\0' → strlen returns 512 bytes per write. */
    char chunk[513];
    memset(chunk, 'X', sizeof(chunk) - 2);
    chunk[sizeof(chunk) - 2] = '\n';
    chunk[sizeof(chunk) - 1] = '\0';
    bool stress_ok = true;
    for (int i = 0; i < 1024; i++) {
        if (storage_sd_write_append(big_file,
                reinterpret_cast<const char *>(chunk)) != STORAGE_OK) {
            stress_ok = false;
            break;
        }
    }
    uint32_t big_size = storage_sd_file_size(big_file);
    Serial0.print("[INFO] bigfile.csv size: ");
    Serial0.print(big_size);
    Serial0.println(" bytes");
    check("HW-SD-007", "512 KB stress write succeeds",
          stress_ok && big_size == 524288UL);

    /* -----------------------------------------------------------------
     * HW-SD-008 — Delete removes file
     * ----------------------------------------------------------------- */
    st = storage_sd_delete(big_file);
    Serial0.print("[INFO] Delete bigfile.csv: ");
    Serial0.println(st);
    check("HW-SD-008", "Delete removes file", st == STORAGE_OK);

    /* -----------------------------------------------------------------
     * HW-SD-009 — Delete non-existent file returns correct error
     * ----------------------------------------------------------------- */
    st = storage_sd_delete(big_file);
    Serial0.print("[INFO] Delete bigfile.csv (2nd time): ");
    Serial0.println(st);
    check("HW-SD-009", "Delete non-existent file → STORAGE_ERR_NOT_FOUND",
          st == STORAGE_ERR_NOT_FOUND);

    /* -----------------------------------------------------------------
     * Summary
     * ----------------------------------------------------------------- */
    Serial0.println();
    Serial0.println("------------------------------------------------");
    Serial0.print("PASS: "); Serial0.println(pass_count);
    Serial0.print("FAIL: "); Serial0.println(fail_count);
    Serial0.println("------------------------------------------------");
    Serial0.println("Note: HW-SD-010 (absent card) — remove card, reset board.");
}

void loop() {}
