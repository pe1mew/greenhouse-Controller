/**
 * @file sd_storage.cpp
 * @brief SD card file I/O driver — FAT32 over SPI (LIB-8).
 *
 * Target build (since 2.0.0-alpha.2.11): pure ESP-IDF, using
 * `esp_vfs_fat_sdspi_mount` / `esp_vfs_fat_sdcard_unmount` for the SPI host +
 * FAT32 mount lifecycle, and standard POSIX `fopen`/`fread`/`fwrite`/`stat`/
 * `remove`/`opendir`/`readdir`/`closedir` against the `/sdcard` VFS mountpoint
 * for file I/O. Both `fatfs` and `sdmmc` are built-in IDF components — no
 * managed-component dependency (unlike Phase 2.10 LittleFS).
 *
 * Native (unit-test) build: all SD / SPI calls are replaced by the stubs
 * defined in test/mock_sd.h.
 *
 * SPI pin assignment (LOLIN S3, unchanged from arduino-era):
 *   MOSI = GPIO 47   MISO = GPIO 48   CLK = GPIO 39   CS = GPIO 40
 *   Host bus: SPI2_HOST (= "FSPI" on ESP32-S3, same as arduino-esp32's FSPI
 *   alias used by the old SPIClass instance).
 *
 * Mount path: "/sdcard". Public API still takes path strings RELATIVE to the
 * SD root (e.g. "/log001.csv"); the driver concatenates the mountpoint
 * internally — same pattern as Phase 2.10's LittleFS rewrite.
 *
 * @author Greenhouse Controller project
 * @version 0.2.0
 */

#include "sd_storage.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Platform abstraction — IDF target vs. native unit-test build
 * --------------------------------------------------------------------------- */
#ifndef UNIT_TEST
  /* ESP-IDF migration 2.0.0-alpha.2.11 — replaced arduino-esp32 includes
   * (<Arduino.h>, <SPI.h>, <SD.h>) with the IDF FAT/SDSPI stack plus POSIX
   * file API. */
  #include "esp_vfs_fat.h"        /* esp_vfs_fat_sdspi_mount, esp_vfs_fat_sdcard_unmount */
  #include "sdmmc_cmd.h"          /* sdmmc_card_t, sdmmc_card_print_info */
  #include "driver/sdspi_host.h"  /* SDSPI_HOST_DEFAULT, SDSPI_DEVICE_CONFIG_DEFAULT */
  #include "driver/spi_common.h"  /* spi_bus_initialize, spi_bus_free */
  #include "esp_log.h"
  #include <stdio.h>               /* fopen/fread/fwrite/fclose, remove */
  #include <sys/stat.h>            /* stat() for existence/size */
  #include <dirent.h>              /* opendir/readdir/closedir */
  #include <errno.h>
  /* Note: ESP-IDF newlib does NOT ship <sys/statvfs.h>. The IDF-native call
   * `esp_vfs_fat_info(base_path, &total, &free)` (declared in esp_vfs_fat.h
   * already included above) returns both numbers in one shot. It walks the
   * FAT internally via FATFS f_getfree — the most authoritative source. */

  static const char *TAG_SD = "LIB-SD";

  /* Single mountpoint — SD card is the only FAT32 volume in the system. */
  static const char * const SD_MOUNT_POINT = "/sdcard";

  /* SPI host: SPI2_HOST is the FSPI peripheral on ESP32-S3, matching the
   * arduino-era SPIClass(FSPI) instance the old driver used. */
  static const spi_host_device_t SD_HOST_ID = SPI2_HOST;

  /* sdmmc_card_t lives here; populated by esp_vfs_fat_sdspi_mount, used by
   * esp_vfs_fat_sdcard_unmount and by free/total-bytes queries that don't
   * route through statvfs. */
  static sdmmc_card_t *g_card = NULL;
#else
  #include "mock_sd.h"
#endif

/* ---------------------------------------------------------------------------
 * Module state
 * --------------------------------------------------------------------------- */
static bool g_mounted = false;

/* ---------------------------------------------------------------------------
 * Internal helpers (target build only)
 * --------------------------------------------------------------------------- */
#ifndef UNIT_TEST
/**
 * Concatenate the SD mountpoint with a caller-supplied path that is relative
 * to the SD root (e.g. "/log001.csv"). The output is placed in @p out
 * (caller-allocated, @p out_len bytes including NUL). The caller-supplied
 * path is expected to start with '/'; if it doesn't, one is inserted between
 * the mountpoint and the path.
 *
 * Returns true on success, false on truncation or invalid input.
 */
static bool build_vfs_path(const char *path, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0 || path == NULL) {
        return false;
    }
    int n;
    if (path[0] == '/') {
        n = snprintf(out, out_len, "%s%s", SD_MOUNT_POINT, path);
    } else {
        n = snprintf(out, out_len, "%s/%s", SD_MOUNT_POINT, path);
    }
    return (n > 0 && (size_t)n < out_len);
}
#endif /* !UNIT_TEST */

/* ---------------------------------------------------------------------------
 * storage_init
 * --------------------------------------------------------------------------- */
storage_status_t storage_init(void)
{
    g_mounted = false;

#ifndef UNIT_TEST
    /* Step 1: initialise the SPI bus. SDSPI_DEFAULT_DMA picks DMA channel
     * 1 on ESP32-S3 (auto-selected by IDF). The buffer sizes are large
     * enough to do back-to-back 512-byte FAT block reads without
     * fragmenting. */
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num     = SD_PIN_MOSI;
    bus_cfg.miso_io_num     = SD_PIN_MISO;
    bus_cfg.sclk_io_num     = SD_PIN_CLK;
    bus_cfg.quadwp_io_num   = -1;
    bus_cfg.quadhd_io_num   = -1;
    bus_cfg.max_transfer_sz = 4000;

    esp_err_t err = spi_bus_initialize(SD_HOST_ID, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        /* ESP_ERR_INVALID_STATE means the host is already initialised —
         * tolerable on repeated init calls. Any other failure is fatal. */
        ESP_LOGW(TAG_SD, "spi_bus_initialize failed: %s (0x%x)",
                 esp_err_to_name(err), (unsigned)err);
        return STORAGE_ERR_MOUNT;
    }

    /* Step 2: configure the sdspi slot — CS pin + host id. */
    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs   = (gpio_num_t)SD_PIN_CS;
    slot_cfg.host_id   = SD_HOST_ID;

    /* Step 3: host driver config (default). */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_HOST_ID;

    /* Step 4: VFS mount config.
     *   .format_if_mount_failed = false   — refuse to format a card that
     *                                       won't mount; we don't want to
     *                                       wipe a user-supplied card.
     *   .max_files              = 5       — concurrent open files. The
     *                                       event-logger pattern opens one
     *                                       file at a time; 5 is generous
     *                                       headroom for the log-rotate +
     *                                       upload-read overlap.
     *   .allocation_unit_size   = 16k     — matches the default SD format. */
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files              = 5;
    mount_cfg.allocation_unit_size   = 16u * 1024u;

    /* Step 5: mount. Returns ESP_OK on success and populates g_card.
     * ESP_FAIL with sdmmc_init = no card present (or wrong wiring).
     * ESP_ERR_INVALID_STATE = already mounted. */
    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cfg,
                                   &mount_cfg, &g_card);
    if (err == ESP_ERR_INVALID_STATE) {
        /* Already mounted — treat as success, reuse existing handle. */
        g_mounted = true;
        return STORAGE_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG_SD, "esp_vfs_fat_sdspi_mount failed: %s (0x%x)",
                 esp_err_to_name(err), (unsigned)err);
        /* Release the SPI bus so a future init can retry cleanly.
         * Note: don't unmount because the mount didn't actually succeed. */
        spi_bus_free(SD_HOST_ID);
        g_card = NULL;
        /* ESP_FAIL with the SD diskio is the "no card / wrong CS" case;
         * ESP_ERR_NOT_FOUND likewise on some IDF versions. */
        if (err == ESP_FAIL || err == ESP_ERR_NOT_FOUND ||
            err == ESP_ERR_TIMEOUT) {
            return STORAGE_ERR_NO_CARD;
        }
        return STORAGE_ERR_MOUNT;
    }

    /* Sanity check — g_card should be non-NULL after a successful mount. */
    if (g_card == NULL) {
        ESP_LOGW(TAG_SD, "mount returned OK but g_card is NULL");
        spi_bus_free(SD_HOST_ID);
        return STORAGE_ERR_MOUNT;
    }
#else
    if (!mock_sd_card_present()) {
        return STORAGE_ERR_NO_CARD;
    }
    if (!mock_sd_begin()) {
        return STORAGE_ERR_MOUNT;
    }
#endif

    g_mounted = true;
    return STORAGE_OK;
}

/* ---------------------------------------------------------------------------
 * storage_sd_available
 * --------------------------------------------------------------------------- */
bool storage_sd_available(void)
{
    return g_mounted;
}

/* ---------------------------------------------------------------------------
 * storage_sd_write_append
 * --------------------------------------------------------------------------- */
storage_status_t storage_sd_write_append(const char *filename, const char *line)
{
    if (!filename || !line) {
        return STORAGE_ERR_PARAM;
    }
    if (!g_mounted) {
        return STORAGE_ERR_NO_CARD;
    }

#ifndef UNIT_TEST
    char vfs_path[128];
    if (!build_vfs_path(filename, vfs_path, sizeof(vfs_path))) {
        return STORAGE_ERR_PARAM;
    }

    /* "ab" — append-binary. The file is created if it doesn't exist; writes
     * go to end-of-file. Matches the arduino-era FILE_APPEND semantics. */
    FILE *f = fopen(vfs_path, "ab");
    if (f == NULL) {
        return STORAGE_ERR_IO;
    }
    size_t len = strlen(line);
    size_t written = fwrite(line, 1, len, f);
    int close_rc = fclose(f);

    if (written != len || close_rc != 0) {
        return STORAGE_ERR_FULL;
    }
#else
    if (!mock_sd_write_append(filename, line)) {
        return STORAGE_ERR_IO;
    }
#endif

    return STORAGE_OK;
}

/* ---------------------------------------------------------------------------
 * storage_sd_read
 * --------------------------------------------------------------------------- */
storage_status_t storage_sd_read(const char *filename, uint32_t offset,
                                 char *buf, size_t buf_len, size_t *bytes_read)
{
    if (!filename || !buf || buf_len == 0 || !bytes_read) {
        return STORAGE_ERR_PARAM;
    }
    *bytes_read = 0;
    if (!g_mounted) {
        return STORAGE_ERR_NO_CARD;
    }

#ifndef UNIT_TEST
    char vfs_path[128];
    if (!build_vfs_path(filename, vfs_path, sizeof(vfs_path))) {
        return STORAGE_ERR_PARAM;
    }

    /* Existence check first so the caller sees STORAGE_ERR_NOT_FOUND
     * distinctly from STORAGE_ERR_IO. */
    struct stat st;
    if (stat(vfs_path, &st) != 0) {
        return STORAGE_ERR_NOT_FOUND;
    }

    FILE *f = fopen(vfs_path, "rb");
    if (f == NULL) {
        return STORAGE_ERR_IO;
    }
    if (offset > 0) {
        if (fseek(f, (long)offset, SEEK_SET) != 0) {
            fclose(f);
            return STORAGE_ERR_IO;
        }
    }
    size_t max_read = buf_len - 1;
    size_t n = fread(buf, 1, max_read, f);
    fclose(f);
    buf[n] = '\0';
    *bytes_read = n;
#else
    size_t n = 0;
    storage_status_t st = mock_sd_read(filename, offset, buf, buf_len, &n);
    if (st != STORAGE_OK) {
        return st;
    }
    *bytes_read = n;
#endif

    return STORAGE_OK;
}

/* ---------------------------------------------------------------------------
 * storage_sd_file_size
 * --------------------------------------------------------------------------- */
uint32_t storage_sd_file_size(const char *filename)
{
    if (!filename || !g_mounted) {
        return 0;
    }

#ifndef UNIT_TEST
    char vfs_path[128];
    if (!build_vfs_path(filename, vfs_path, sizeof(vfs_path))) {
        return 0;
    }
    struct stat st;
    if (stat(vfs_path, &st) != 0) {
        return 0;
    }
    return (uint32_t)st.st_size;
#else
    return mock_sd_file_size(filename);
#endif
}

/* ---------------------------------------------------------------------------
 * storage_sd_free_bytes
 * --------------------------------------------------------------------------- */
uint64_t storage_sd_free_bytes(void)
{
    if (!g_mounted) {
        return 0;
    }

#ifndef UNIT_TEST
    uint64_t total = 0;
    uint64_t free_b = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_b) != ESP_OK) {
        return 0;
    }
    return free_b;
#else
    return mock_sd_free_bytes();
#endif
}

/* ---------------------------------------------------------------------------
 * storage_sd_total_bytes
 * --------------------------------------------------------------------------- */
uint64_t storage_sd_total_bytes(void)
{
    if (!g_mounted) {
        return 0;
    }

#ifndef UNIT_TEST
    uint64_t total = 0;
    uint64_t free_b = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_b) != ESP_OK) {
        return 0;
    }
    return total;
#else
    return mock_sd_free_bytes(); /* mock has no separate total; return free as proxy */
#endif
}

/* ---------------------------------------------------------------------------
 * storage_sd_unmount
 * --------------------------------------------------------------------------- */
void storage_sd_unmount(void)
{
    if (!g_mounted) {
        return;
    }
    g_mounted = false;

#ifndef UNIT_TEST
    /* Order matters: unmount FAT first (closes any open files, flushes the
     * cache to the card), then release the SPI bus.
     *
     * gh#26 (since 1.20.2): SD unmount before planned reset must finish
     * before the reset fires, or FAT's deferred writes can be lost. The IDF
     * esp_vfs_fat_sdcard_unmount is synchronous and calls f_sync via
     * ff_sd_card_disk_status before releasing the diskio layer, so the
     * synchronous-flush contract from the arduino era is preserved. */
    if (g_card != NULL) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, g_card);
        g_card = NULL;
    }
    spi_bus_free(SD_HOST_ID);
#endif
}

/* ---------------------------------------------------------------------------
 * storage_sd_list_csv
 *
 * Walks the SD root directory (via opendir/readdir), copies names with the
 * requested extension into the output buffer separated by commas. Truncation
 * on a full buffer is silent (matches the arduino-era behaviour).
 * --------------------------------------------------------------------------- */
storage_status_t storage_sd_list_csv(const char *ext, char *buf, size_t buf_len)
{
    if (!ext || !buf || buf_len == 0) {
        return STORAGE_ERR_PARAM;
    }
    buf[0] = '\0';
    if (!g_mounted) {
        return STORAGE_ERR_NO_CARD;
    }

#ifndef UNIT_TEST
    DIR *d = opendir(SD_MOUNT_POINT);
    if (d == NULL) {
        return STORAGE_ERR_IO;
    }
    size_t ext_len = strlen(ext);
    size_t pos = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        /* readdir on FATFS-VFS doesn't always populate d_type — fall back to
         * a stat() call only if needed. For now treat any entry whose name
         * ends with the requested extension as a candidate. The arduino-era
         * driver did the same: extension match + skip directories. */
        const char *name = entry->d_name;
        size_t name_len = strlen(name);
        if (name_len < ext_len) {
            continue;
        }
        if (strcmp(name + name_len - ext_len, ext) != 0) {
            continue;
        }
        /* Skip directories. dirent.d_type may be DT_UNKNOWN under FATFS;
         * if so, fall back to stat. */
        if (entry->d_type == DT_DIR) {
            continue;
        }
        if (entry->d_type == DT_UNKNOWN) {
            char full[160];
            if (build_vfs_path(name, full, sizeof(full))) {
                struct stat st;
                if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
                    continue;
                }
            }
        }
        /* Append "name," if it fits */
        size_t needed = name_len + 1; /* +1 for comma */
        if (pos + needed < buf_len) {
            memcpy(buf + pos, name, name_len);
            pos += name_len;
            buf[pos++] = ',';
            buf[pos]   = '\0';
        }
    }
    closedir(d);
#else
    mock_sd_list_csv(ext, buf, buf_len);
#endif

    return STORAGE_OK;
}

/* ---------------------------------------------------------------------------
 * storage_sd_delete
 * --------------------------------------------------------------------------- */
storage_status_t storage_sd_delete(const char *filename)
{
    if (!filename) {
        return STORAGE_ERR_PARAM;
    }
    if (!g_mounted) {
        return STORAGE_ERR_NO_CARD;
    }

#ifndef UNIT_TEST
    char vfs_path[128];
    if (!build_vfs_path(filename, vfs_path, sizeof(vfs_path))) {
        return STORAGE_ERR_PARAM;
    }
    struct stat st;
    if (stat(vfs_path, &st) != 0) {
        return STORAGE_ERR_NOT_FOUND;
    }
    if (remove(vfs_path) != 0) {
        return STORAGE_ERR_IO;
    }
#else
    return mock_sd_delete(filename);
#endif

    return STORAGE_OK;
}
