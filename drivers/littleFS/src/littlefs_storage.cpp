/**
 * @file littlefs_storage.cpp
 * @brief LittleFS dual-partition driver — internal flash (LIB-9).
 *
 * Target build (since 2.0.0-alpha.2.10): pure ESP-IDF, using the
 * joltwallet/littlefs managed component (`esp_vfs_littlefs_register/_unregister`)
 * for partition mount lifecycle, and standard POSIX `fopen`/`fread`/`fwrite`/
 * `stat`/`fclose` against the per-partition VFS mountpoint for file I/O.
 *
 * Native (unit-test) build: all LittleFS / OTA calls are replaced by the
 * stubs defined in test/mock_lfs.h.
 *
 * Partition / mount mapping (UNCHANGED from the arduino-era driver):
 *   LFS_PARTITION_A  →  label "lfs0"  →  mount "/lfsa"
 *   LFS_PARTITION_B  →  label "lfs1"  →  mount "/lfsb"
 *
 * The two partitions MUST have separate mount paths. Using the same path for
 * both (e.g. "/lfs") silently re-binds the second `register` and causes
 * cross-bank corruption during paired OTA updates. This rule is recorded in
 * MEMORY.md "LittleFS dual-partition basePath bug" and preserved verbatim
 * across the migration.
 *
 * @author Greenhouse Controller project
 * @version 0.2.0
 */

#include "littlefs_storage.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Platform abstraction
 * --------------------------------------------------------------------------- */
#ifndef UNIT_TEST
  /* ESP-IDF migration 2.0.0-alpha.2.10 — replaced arduino-esp32 includes
   * (<Arduino.h>, <LittleFS.h>) with esp_vfs_littlefs.h (joltwallet/littlefs
   * managed component) and the POSIX file API. */
  #include "esp_littlefs.h"          /* esp_vfs_littlefs_conf_t, *_register, *_format, *_info */
  #include "esp_ota_ops.h"
  #include "esp_log.h"
  #include <stdio.h>                  /* fopen/fread/fwrite/fclose */
  #include <sys/stat.h>               /* stat() for existence checks */
  #include <errno.h>

  static const char *TAG_LFS = "LIB-9";

  /* Partition labels (immutable; defined in partitions.csv). */
  static const char * const LABEL_A = "lfs0";
  static const char * const LABEL_B = "lfs1";

  /* Each partition gets a UNIQUE VFS mount point. Using the same path
   * (e.g. "/lfs" for both) silently caused the OTA cross-bank bug observed
   * in 1.17.4–1.17.8a: T11 mounted lfs0 at /lfs, then during a paired OTA
   * T13 tried to mount lfs1 also at /lfs. The IDF VFS rejects (or silently
   * rebinds) a second mount at the same path; the result was that T13's
   * writes appeared to "succeed" but never reached the inactive LittleFS
   * partition. After reboot, T11 mounted what it thought was the new
   * partition's filesystem but read the OLD content. */
  static const char * const MOUNT_A = "/lfsa";
  static const char * const MOUNT_B = "/lfsb";
#else
  #include "mock_lfs.h"
#endif

/* ---------------------------------------------------------------------------
 * Module state
 * --------------------------------------------------------------------------- */
static bool g_mounted[2] = {false, false};

/* ---------------------------------------------------------------------------
 * Internal helpers (target build only)
 * --------------------------------------------------------------------------- */
#ifndef UNIT_TEST
static const char *select_label(lfs_partition_t p)
{
    return (p == LFS_PARTITION_A) ? LABEL_A : LABEL_B;
}
static const char *select_mountpoint(lfs_partition_t p)
{
    return (p == LFS_PARTITION_A) ? MOUNT_A : MOUNT_B;
}

/**
 * Concatenate the partition's VFS mountpoint with a caller-supplied path that
 * is relative to the LittleFS root (e.g. "/index.html"). The output is
 * placed in @p out (caller-allocated, @p out_len bytes including NUL). The
 * caller-supplied path is expected to start with '/'; if it doesn't, one is
 * inserted between the mountpoint and the path so the result is always a
 * valid absolute VFS path.
 *
 * Returns true on success, false on truncation or invalid input.
 */
static bool build_vfs_path(lfs_partition_t p, const char *path,
                            char *out, size_t out_len)
{
    if (out == NULL || out_len == 0 || path == NULL) {
        return false;
    }
    const char *mp = select_mountpoint(p);
    int n;
    if (path[0] == '/') {
        n = snprintf(out, out_len, "%s%s", mp, path);
    } else {
        n = snprintf(out, out_len, "%s/%s", mp, path);
    }
    return (n > 0 && (size_t)n < out_len);
}
#endif /* !UNIT_TEST */

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
    /* esp_vfs_littlefs_conf_t — opt-in config struct.
     *   .base_path              — VFS mount path; MUST be unique per partition.
     *   .partition_label        — matches the "Name" column in partitions.csv.
     *   .format_if_mount_failed — false matches the arduino-era behaviour
     *                             (begin(false, ...)); a freshly-erased
     *                             partition will fail to mount rather than
     *                             auto-formatting. T13 (OTA) calls
     *                             littlefs_format() explicitly when it wants
     *                             a clean slate; T11 callers want to know if
     *                             the partition is broken rather than silently
     *                             re-init it.
     *   .dont_mount             — false; we want the mount as a side effect.
     */
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path              = select_mountpoint(partition);
    conf.partition_label        = select_label(partition);
    conf.format_if_mount_failed = false;
    conf.dont_mount             = false;

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_LFS, "mount %s at %s failed: %s (0x%x)",
                 conf.partition_label, conf.base_path,
                 esp_err_to_name(err), (unsigned)err);
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
    esp_err_t err = esp_vfs_littlefs_unregister(select_label(partition));
    if (err != ESP_OK) {
        ESP_LOGW(TAG_LFS, "unmount %s failed: %s",
                 select_label(partition), esp_err_to_name(err));
        /* fall through; consumer still wants g_mounted cleared so a
         * subsequent mount can re-attempt. */
    }
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
    /* The public API takes a path relative to the LittleFS root (e.g.
     * "/index.html"); POSIX fopen wants the full VFS path. */
    char vfs_path[128];
    if (!build_vfs_path(partition, path, vfs_path, sizeof(vfs_path))) {
        buf[0] = '\0';
        return LFS_ERR_IO;
    }

    /* Existence check first so the caller sees LFS_ERR_NOT_FOUND distinctly
     * from LFS_ERR_IO. stat() is cheap. */
    struct stat st;
    if (stat(vfs_path, &st) != 0) {
        buf[0] = '\0';
        return LFS_ERR_NOT_FOUND;
    }

    FILE *f = fopen(vfs_path, "rb");
    if (f == NULL) {
        buf[0] = '\0';
        return LFS_ERR_IO;
    }

    /* Copy at most buf_len-1 bytes; always NUL-terminate. fread returns
     * the number of full elements read (with size=1 this is bytes). */
    size_t max_read = buf_len - 1;
    size_t n = fread(buf, 1, max_read, f);
    fclose(f);
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
    char vfs_path[128];
    if (!build_vfs_path(partition, path, vfs_path, sizeof(vfs_path))) {
        return LFS_ERR_IO;
    }

    /* "wb" truncates if the file exists; matches the arduino-era
     * fs::LittleFSFS::open(path, "w") behaviour. */
    FILE *f = fopen(vfs_path, "wb");
    if (f == NULL) {
        return LFS_ERR_IO;
    }

    size_t written = fwrite(data, 1, len, f);

    /* fflush + fclose; check fclose result so we surface deferred I/O errors
     * (LittleFS may not commit blocks until close/sync). A short write
     * during fwrite indicates a full partition; an fclose error after a
     * successful write means a commit-time failure (also treated as full). */
    int close_rc = fclose(f);

    if (written != len || close_rc != 0) {
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
    char vfs_path[128];
    if (!build_vfs_path(partition, path, vfs_path, sizeof(vfs_path))) {
        return false;
    }
    struct stat st;
    return (stat(vfs_path, &st) == 0);
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
    size_t total = 0;
    size_t used  = 0;
    esp_err_t err = esp_littlefs_info(select_label(partition), &total, &used);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_LFS, "esp_littlefs_info(%s) failed: %s",
                 select_label(partition), esp_err_to_name(err));
        return 0;
    }
    if (used > total) {
        /* Defensive: shouldn't happen, but matches old behaviour of returning
         * 0 on underflow rather than a giant uint64_t wraparound. */
        return 0;
    }
    return (uint64_t)(total - used);
#else
    return mock_lfs_free_bytes(partition);
#endif
}

/* ---------------------------------------------------------------------------
 * littlefs_format
 * --------------------------------------------------------------------------- */
lfs_status_t littlefs_format(lfs_partition_t partition)
{
    if (partition != LFS_PARTITION_A && partition != LFS_PARTITION_B) {
        return LFS_ERR_IO;
    }
    /* Unmount first if currently mounted. esp_littlefs_format works on an
     * unmounted partition; if mounted, the underlying library will return
     * ESP_ERR_INVALID_STATE. */
    if (g_mounted[partition]) {
        littlefs_unmount(partition);
    }

#ifndef UNIT_TEST
    esp_err_t err = esp_littlefs_format(select_label(partition));
    if (err != ESP_OK) {
        ESP_LOGW(TAG_LFS, "esp_littlefs_format(%s) failed: %s",
                 select_label(partition), esp_err_to_name(err));
        return LFS_ERR_IO;
    }
    return LFS_OK;
#else
    return LFS_OK;
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
