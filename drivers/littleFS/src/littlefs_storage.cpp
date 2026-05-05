/**
 * @file littlefs_storage.cpp
 * @brief LittleFS dual-partition driver — internal flash (LIB-9).
 *
 * Target build: uses the Arduino ESP32 LittleFS library together with the
 * ESP-IDF OTA API to detect the active firmware bank.
 *
 * Native (unit-test) build: all LittleFS / OTA calls are replaced by the
 * stubs defined in test/mock_lfs.h.
 *
 * Partition label mapping:
 *   LFS_PARTITION_A  →  "littlefs_a"
 *   LFS_PARTITION_B  →  "littlefs_b"
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#include "littlefs_storage.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Platform abstraction
 * --------------------------------------------------------------------------- */
#ifndef UNIT_TEST
  #include <Arduino.h>
  #include <LittleFS.h>
  #include <esp_ota_ops.h>

  /* One LittleFS instance per partition (Arduino ESP32 allows this via the
   * partition-label overload of begin()). */
  static fs::LittleFSFS g_lfs_a;
  static fs::LittleFSFS g_lfs_b;

  static const char * const LABEL_A = "lfs0";
  static const char * const LABEL_B = "lfs1";
#else
  #include "mock_lfs.h"
#endif

/* ---------------------------------------------------------------------------
 * Module state
 * --------------------------------------------------------------------------- */
static bool g_mounted[2] = {false, false};

/* helper — select the correct fs instance or mock index */
#ifndef UNIT_TEST
static fs::LittleFSFS &select_fs(lfs_partition_t p)
{
    return (p == LFS_PARTITION_A) ? g_lfs_a : g_lfs_b;
}
static const char *select_label(lfs_partition_t p)
{
    return (p == LFS_PARTITION_A) ? LABEL_A : LABEL_B;
}
#endif

/* ---------------------------------------------------------------------------
 * littlefs_mount
 * --------------------------------------------------------------------------- */
lfs_status_t littlefs_mount(lfs_partition_t partition)
{
    if (partition != LFS_PARTITION_A && partition != LFS_PARTITION_B) {
        return LFS_ERR_MOUNT;
    }
    if (g_mounted[partition]) {
        return LFS_OK;  /* already mounted — no-op */
    }

#ifndef UNIT_TEST
    fs::LittleFSFS &fs = select_fs(partition);
    /* begin(formatOnFail=false, basePath, maxFiles, partitionLabel) */
    if (!fs.begin(false, "/lfs", 10, select_label(partition))) {
        return LFS_ERR_MOUNT;
    }
#else
    if (!mock_lfs_begin(partition)) {
        return LFS_ERR_MOUNT;
    }
#endif

    g_mounted[partition] = true;
    return LFS_OK;
}

/* ---------------------------------------------------------------------------
 * littlefs_unmount
 * --------------------------------------------------------------------------- */
void littlefs_unmount(lfs_partition_t partition)
{
    if (partition != LFS_PARTITION_A && partition != LFS_PARTITION_B) {
        return;
    }
    if (!g_mounted[partition]) {
        return;  /* already unmounted — no-op */
    }

#ifndef UNIT_TEST
    select_fs(partition).end();
#else
    mock_lfs_end(partition);
#endif

    g_mounted[partition] = false;
}

/* ---------------------------------------------------------------------------
 * littlefs_read
 * --------------------------------------------------------------------------- */
lfs_status_t littlefs_read(lfs_partition_t partition, const char *path,
                            char *buf, size_t buf_len)
{
    if (!path || !buf || buf_len == 0) {
        return LFS_ERR_IO;
    }
    if (!g_mounted[partition]) {
        return LFS_ERR_MOUNT;
    }

#ifndef UNIT_TEST
    fs::LittleFSFS &fs = select_fs(partition);
    if (!fs.exists(path)) {
        buf[0] = '\0';
        return LFS_ERR_NOT_FOUND;
    }
    File f = fs.open(path, "r");
    if (!f) {
        buf[0] = '\0';
        return LFS_ERR_IO;
    }
    size_t max_read = buf_len - 1;
    size_t n = f.read(reinterpret_cast<uint8_t *>(buf), max_read);
    f.close();
    buf[n] = '\0';
#else
    return mock_lfs_read(partition, path, buf, buf_len);
#endif

    return LFS_OK;
}

/* ---------------------------------------------------------------------------
 * littlefs_write
 * --------------------------------------------------------------------------- */
lfs_status_t littlefs_write(lfs_partition_t partition, const char *path,
                             const void *data, size_t len)
{
    if (!path || !data) {
        return LFS_ERR_IO;
    }
    if (!g_mounted[partition]) {
        return LFS_ERR_MOUNT;
    }

#ifndef UNIT_TEST
    fs::LittleFSFS &fs = select_fs(partition);
    File f = fs.open(path, "w");
    if (!f) {
        return LFS_ERR_IO;
    }
    size_t written = f.write(reinterpret_cast<const uint8_t *>(data), len);
    f.close();
    if (written != len) {
        return LFS_ERR_FULL;
    }
#else
    return mock_lfs_write(partition, path, data, len);
#endif

    return LFS_OK;
}

/* ---------------------------------------------------------------------------
 * littlefs_exists
 * --------------------------------------------------------------------------- */
bool littlefs_exists(lfs_partition_t partition, const char *path)
{
    if (!path || !g_mounted[partition]) {
        return false;
    }

#ifndef UNIT_TEST
    return select_fs(partition).exists(path);
#else
    return mock_lfs_exists(partition, path);
#endif
}

/* ---------------------------------------------------------------------------
 * littlefs_free_bytes
 * --------------------------------------------------------------------------- */
uint64_t littlefs_free_bytes(lfs_partition_t partition)
{
    if (!g_mounted[partition]) {
        return 0;
    }

#ifndef UNIT_TEST
    fs::LittleFSFS &fs = select_fs(partition);
    uint64_t total = fs.totalBytes();
    uint64_t used  = fs.usedBytes();
    return (used <= total) ? (total - used) : 0;
#else
    return mock_lfs_free_bytes(partition);
#endif
}

/* ---------------------------------------------------------------------------
 * littlefs_active_partition
 * --------------------------------------------------------------------------- */
lfs_partition_t littlefs_active_partition(void)
{
#ifndef UNIT_TEST
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running && running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) {
        return LFS_PARTITION_B;
    }
    return LFS_PARTITION_A;
#else
    return mock_lfs_active_partition();
#endif
}
