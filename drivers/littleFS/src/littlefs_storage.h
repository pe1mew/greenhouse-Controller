/**
 * @file littlefs_storage.h
 * @brief LittleFS dual-partition driver — internal flash (LIB-9).
 *
 * Provides read/write access to the two LittleFS partitions on the ESP32-S3
 * internal flash.  Each partition is permanently paired with its same-letter
 * OTA firmware bank (Bank A ↔ LittleFS A, Bank B ↔ LittleFS B).
 *
 * Both partitions are always present in the flash layout — the driver is
 * not optional.
 *
 * ## Hardware
 *   - Storage   : ESP32-S3 internal NOR flash (no external IC).
 *   - Layout    : Two dedicated LittleFS partitions, names @c littlefs_a /
 *                 @c littlefs_b in @c partitions.csv.
 *   - Mountpts  : @c /lfsa  for @ref LFS_PARTITION_A,
 *                 @c /lfsb  for @ref LFS_PARTITION_B.  Each partition has
 *                 its OWN mountpoint; sharing a path (e.g. both at @c /lfs)
 *                 corrupts the VFS lookup.
 *   - FS lib    : esp_littlefs (ESP-IDF managed component).
 *
 * ## Consumers
 *   - T11 (Web Server)  mounts the active partition once at boot to serve
 *                       HTML/CSS/JS assets.
 *   - T13 (OTA)         independently mounts the inactive partition to write
 *                       updated web assets during an OTA update cycle.
 *
 * ## API summary
 *   - @ref littlefs_mount / @ref littlefs_unmount   Lifecycle.
 *   - @ref littlefs_read / @ref littlefs_write      Whole-file I/O.
 *   - @ref littlefs_exists / @ref littlefs_format / @ref littlefs_free_bytes
 *                                                   Filesystem queries.
 *   - @ref littlefs_active_partition                Match active OTA bank.
 *   - @ref littlefs_mountpoint                      VFS path prefix for stdio.
 *
 * ## Thread safety
 *   esp_littlefs serialises VFS calls internally.  This driver adds no
 *   additional locking; concurrent calls from multiple tasks are safe at
 *   the file-system level but interleaving order is not defined.
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * Status codes
 * --------------------------------------------------------------------------- */

/** @brief Return codes for all @c littlefs_* functions. */
typedef enum {
    LFS_OK = 0,         /**< Operation succeeded. */
    LFS_ERR_MOUNT,      /**< Partition could not be mounted (label missing / corrupt). */
    LFS_ERR_NOT_FOUND,  /**< File or path does not exist on the partition. */
    LFS_ERR_IO,         /**< Read / write error reported by the file system. */
    LFS_ERR_FULL        /**< Partition has no free space remaining. */
} lfs_status_t;

/* ---------------------------------------------------------------------------
 * Partition selector
 * --------------------------------------------------------------------------- */

/** @brief Selects which LittleFS partition a call targets. */
typedef enum {
    LFS_PARTITION_A = 0,  /**< LittleFS partition A — paired with OTA Bank A; mountpoint @c /lfsa. */
    LFS_PARTITION_B = 1   /**< LittleFS partition B — paired with OTA Bank B; mountpoint @c /lfsb. */
} lfs_partition_t;

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

/**
 * @brief Mount the specified LittleFS partition.
 *
 * Safe to call on an already-mounted partition (no-op, returns
 * @ref LFS_OK).  T11 calls this once at boot with the active partition;
 * T13 calls this with the inactive partition before writing web assets.
 *
 * @param partition  Partition to mount (@ref LFS_PARTITION_A or
 *                   @ref LFS_PARTITION_B).
 * @return @ref LFS_OK on success, @ref LFS_ERR_MOUNT on failure (missing
 *         label, corrupted filesystem).
 * @warning Each partition has its OWN mountpoint (@c /lfsa or @c /lfsb).
 *          Re-using one path for both partitions corrupts the VFS lookup.
 * @see    littlefs_mountpoint() — query the mount path.
 */
lfs_status_t littlefs_mount(lfs_partition_t partition);

/**
 * @brief Unmount the specified partition.
 *
 * T13 calls this after completing a web asset write.
 * Safe to call on an already-unmounted partition (no-op).
 *
 * @param partition  Partition to unmount.
 */
void littlefs_unmount(lfs_partition_t partition);

/**
 * @brief Read an entire file into a NUL-terminated buffer.
 *
 * At most @p buf_len−1 bytes are copied; the result is always
 * NUL-terminated.  Partial reads due to a small buffer are not an error.
 *
 * @param partition  Partition to read from.
 * @param path       Absolute path on the partition, e.g. @c "/index.html".
 * @param buf        Destination buffer (at least @p buf_len bytes).
 * @param buf_len    Buffer size including the NUL terminator.
 * @return @ref LFS_OK, @ref LFS_ERR_NOT_FOUND, @ref LFS_ERR_IO.
 * @warning NOT suitable for binary content (truncates at the first 0x00
 *          byte) or for files larger than the buffer.  Use the stdio path
 *          (@ref littlefs_mountpoint + @c fopen) for HTTP file serving.
 * @see    littlefs_mountpoint().
 */
lfs_status_t littlefs_read(lfs_partition_t partition, const char *path,
                            char *buf, size_t buf_len);

/**
 * @brief Write (overwrite) a file on the specified partition.
 *
 * Creates the file if it does not exist.  Truncates and replaces existing
 * content if the file already exists.
 *
 * @param partition  Partition to write to.
 * @param path       Absolute path on the partition.
 * @param data       Data to write (may contain binary bytes).
 * @param len        Number of bytes to write.
 * @return LFS_OK, LFS_ERR_IO, LFS_ERR_FULL.
 */
lfs_status_t littlefs_write(lfs_partition_t partition, const char *path,
                             const void *data, size_t len);

/**
 * @brief Check whether a file exists on the specified partition.
 *
 * @param partition  Partition to query.
 * @param path       Absolute path to check.
 * @return true if the file exists, false otherwise.
 */
bool littlefs_exists(lfs_partition_t partition, const char *path);

/**
 * @brief Format (wipe) the specified LittleFS partition.
 *
 * Erases all files on the partition and reinitialises the filesystem.
 * Unmounts the partition first if it is currently mounted.  Intended for
 * use by T13 (OTA) before writing fresh web assets so that stale files
 * from a previous OTA cycle cannot persist.
 *
 * @param partition  Partition to format (must be the INACTIVE partition).
 * @return @ref LFS_OK on success, @ref LFS_ERR_IO on failure.
 * @warning Formatting the partition paired with the currently-running OTA
 *          bank will destroy the assets served by T11; only ever format
 *          the INACTIVE partition.
 */
lfs_status_t littlefs_format(lfs_partition_t partition);

/**
 * @brief Return free bytes remaining on the specified partition.
 *
 * @param partition  Partition to query.
 * @return Free bytes available, or 0 on error / not mounted.
 */
uint64_t littlefs_free_bytes(lfs_partition_t partition);

/**
 * @brief Return the partition letter that matches the active OTA firmware bank.
 *
 * Queries the ESP-IDF OTA API to determine which app partition is currently
 * running, then maps it to the paired LittleFS partition.
 *
 * In the native unit-test build the result is controlled by
 * mock_lfs_set_active_partition().
 *
 * @return LFS_PARTITION_A or LFS_PARTITION_B.
 */
lfs_partition_t littlefs_active_partition(void);

/**
 * @brief Return the VFS mountpoint prefix for the partition (no trailing slash).
 *
 * Use with stdio (fopen, fread, fstat) when streaming large files: the LFS_OK
 * `littlefs_read` API caps at one fixed-size buffer + NUL-terminates the
 * result, which truncates files > buffer size and corrupts binary content past
 * the first 0x00 byte. For HTTP file serving of 10-40 KB assets the stdio path
 * via this mountpoint is the correct primitive.
 *
 * Example:
 *   char path[64];
 *   snprintf(path, sizeof(path), "%s/index.html",
 *            littlefs_mountpoint(littlefs_active_partition()));
 *   FILE *f = fopen(path, "rb");
 *   // … fread loop …
 *   fclose(f);
 *
 * @param partition  Partition to query.
 * @return "/lfsa" for LFS_PARTITION_A, "/lfsb" for LFS_PARTITION_B. Never NULL.
 */
const char *littlefs_mountpoint(lfs_partition_t partition);
