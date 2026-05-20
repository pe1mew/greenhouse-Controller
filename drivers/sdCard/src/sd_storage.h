/**
 * @file sd_storage.h
 * @brief SD card file I/O driver — FAT32 over SPI (LIB-8).
 *
 * Provides the primary event-log storage for the greenhouse controller.
 * Implements append-only writes and log rotation (512 KB per file, 10
 * files retained).  Used exclusively by T9 (Event Logger).  The driver
 * is optional — absent when the SD card feature is not fitted.
 *
 * ## Hardware
 *   - Card slot   : Standard µSD card holder on the LOLIN S3 carrier.
 *   - Interface   : SPI mode (one-bit, native CMD line not used).
 *   - SPI bus     : Dedicated VSPI instance (does NOT share with other
 *                   peripherals on this project).
 *   - Filesystem  : FAT32 (8.3 names, long names supported via VFS).
 *   - Mount path  : @c /sdcard (managed by ESP-IDF VFS).
 *
 * ## SPI pin assignment (LOLIN S3 — see @c pin_config.h)
 *   - @c PIN_SD_MOSI  GPIO 47   (Master Out → DI)
 *   - @c PIN_SD_MISO  GPIO 48   (DO → Master In)
 *   - @c PIN_SD_CLK   GPIO 39   (SCK)
 *   - @c PIN_SD_CS    GPIO 40   (chip select, active LOW)
 *
 * ## API summary
 *   - @ref storage_init               Bring up SPI bus and mount FAT32.
 *   - @ref storage_sd_available       Quick mount-state query.
 *   - @ref storage_sd_write_append    Append one line to a log file.
 *   - @ref storage_sd_read            Random-access read.
 *   - @ref storage_sd_file_size       File-size query.
 *   - @ref storage_sd_free_bytes / @ref storage_sd_total_bytes
 *                                     Volume capacity queries.
 *   - @ref storage_sd_list_csv        Directory listing by extension.
 *   - @ref storage_sd_delete          Remove a file.
 *   - @ref storage_sd_unmount         Graceful tear-down before card removal.
 *
 * ## Thread safety
 *   ESP-IDF VFS serialises POSIX-style file I/O internally.  This driver
 *   adds no extra locking; callers from multiple tasks are safe at the
 *   file-system level.  However, append patterns from concurrent tasks
 *   interleave at line boundaries — consumers needing strict ordering
 *   must serialise externally.
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "../../../firmware/config/pin_config.h"

/* ---------------------------------------------------------------------------
 * SPI pin aliases (canonical names defined in pin_config.h)
 * --------------------------------------------------------------------------- */
#define SD_PIN_MOSI  PIN_SD_MOSI
#define SD_PIN_MISO  PIN_SD_MISO
#define SD_PIN_CLK   PIN_SD_CLK
#define SD_PIN_CS    PIN_SD_CS

/* ---------------------------------------------------------------------------
 * Status codes
 * --------------------------------------------------------------------------- */

/** @brief Return codes for all @c storage_sd_* functions. */
typedef enum {
    STORAGE_OK = 0,       /**< Operation succeeded. */
    STORAGE_ERR_NO_CARD,  /**< No SD card detected / card not inserted. */
    STORAGE_ERR_MOUNT,    /**< SPI bus or FAT32 mount failure. */
    STORAGE_ERR_IO,       /**< Read / write error reported by the FAT layer. */
    STORAGE_ERR_NOT_FOUND,/**< File or path does not exist. */
    STORAGE_ERR_FULL,     /**< Card or file system is full. */
    STORAGE_ERR_PARAM     /**< Invalid parameter (NULL pointer, zero length, …). */
} storage_status_t;

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

/**
 * @brief Initialise the SPI bus and mount the FAT32 file system.
 *
 * Uses a dedicated SPI instance to drive the non-default LOLIN S3 pins
 * (see @ref SD_PIN_MOSI / @ref SD_PIN_MISO / @ref SD_PIN_CLK /
 * @ref SD_PIN_CS).  Card capacity and format are read at mount time.
 * The volume is mounted at @c /sdcard.
 *
 * @return @ref STORAGE_OK on success (card detected and mounted),
 *         @ref STORAGE_ERR_NO_CARD if no card is inserted,
 *         @ref STORAGE_ERR_MOUNT if the card is present but the FAT32
 *         mount failed (corrupt FS, unsupported format).
 * @warning Must be called once before any other @c storage_sd_* function.
 * @see    storage_sd_unmount(), storage_sd_available().
 */
storage_status_t storage_init(void);

/**
 * @brief Query whether the SD card is currently mounted and usable.
 *
 * @return true if mounted; false otherwise.
 */
bool storage_sd_available(void);

/**
 * @brief Append a single text line to a file.
 *
 * The file is created if it does not already exist.  The line is written
 * verbatim; the caller is responsible for appending @c "\n" if desired.
 *
 * @param filename  Absolute path on the FAT32 volume, e.g. @c "/log001.csv".
 *                  Must start with @c "/" (relative paths are rejected).
 * @param line      NUL-terminated string to write.
 * @return @ref STORAGE_OK, @ref STORAGE_ERR_PARAM, @ref STORAGE_ERR_NO_CARD,
 *         @ref STORAGE_ERR_FULL, @ref STORAGE_ERR_IO.
 * @note   Each call performs an @c fopen + @c fwrite + @c fclose cycle —
 *         high-rate callers should batch lines themselves to amortise the
 *         FAT update cost.
 */
storage_status_t storage_sd_write_append(const char *filename, const char *line);

/**
 * @brief Read up to @p buf_len−1 bytes from a file at a given byte offset.
 *
 * The result is always NUL-terminated.  When the file content from @p offset
 * to end-of-file is shorter than @p buf_len−1, only the available bytes are
 * copied.  Partial reads due to a small buffer are not an error.
 *
 * @param filename    Absolute path on the FAT32 volume.
 * @param offset      Byte offset from which to start reading.
 * @param buf         Destination buffer (must be at least @p buf_len bytes).
 * @param buf_len     Size of @p buf including the NUL terminator.
 * @param bytes_read  Set to the number of bytes actually copied (excluding NUL).
 * @return STORAGE_OK, STORAGE_ERR_PARAM, STORAGE_ERR_NO_CARD,
 *         STORAGE_ERR_NOT_FOUND, STORAGE_ERR_IO.
 */
storage_status_t storage_sd_read(const char *filename, uint32_t offset,
                                 char *buf, size_t buf_len, size_t *bytes_read);

/**
 * @brief Return the size of a file in bytes.
 *
 * @param filename  Absolute path on the FAT32 volume.
 * @return File size in bytes, or 0 if the file does not exist or on error.
 */
uint32_t storage_sd_file_size(const char *filename);

/**
 * @brief Return the number of free bytes remaining on the FAT32 volume.
 *
 * @return Free bytes available, or 0 on error / no card.
 */
uint64_t storage_sd_free_bytes(void);

/**
 * @brief Return the total capacity of the FAT32 volume in bytes.
 *
 * @return Total bytes, or 0 on error / no card.
 */
uint64_t storage_sd_total_bytes(void);

/**
 * @brief Gracefully unmount the FAT32 volume and release the SPI bus.
 *
 * After this call, @ref storage_sd_available returns @c false and T9
 * falls back to in-memory event buffering.  Call @ref storage_init to
 * re-mount after card re-insertion.
 *
 * @warning In-flight writes from other tasks are NOT aborted — callers
 *          MUST ensure no @c storage_sd_* call is in progress before
 *          invoking this function.
 * @see    storage_init().
 */
void storage_sd_unmount(void);

/**
 * @brief List files matching an extension into a comma-separated buffer.
 *
 * Scans the root directory and appends each matching filename (just the name,
 * no path) followed by a comma.  The buffer is NUL-terminated.  Truncation
 * (buffer exhausted) is silent.
 *
 * @param ext      Extension to match, including the dot, e.g. ".csv".
 * @param buf      Destination buffer.
 * @param buf_len  Size of @p buf.
 * @return STORAGE_OK, STORAGE_ERR_PARAM, STORAGE_ERR_NO_CARD.
 */
storage_status_t storage_sd_list_csv(const char *ext, char *buf, size_t buf_len);

/**
 * @brief Delete a file from the FAT32 volume.
 *
 * @param filename  Absolute path on the FAT32 volume.
 * @return STORAGE_OK, STORAGE_ERR_PARAM, STORAGE_ERR_NO_CARD,
 *         STORAGE_ERR_NOT_FOUND, STORAGE_ERR_IO.
 */
storage_status_t storage_sd_delete(const char *filename);
