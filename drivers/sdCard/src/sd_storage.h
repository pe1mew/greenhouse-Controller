/**
 * @file sd_storage.h
 * @brief SD card file I/O driver — FAT32 over SPI (LIB-8).
 *
 * Provides the primary event log storage for the greenhouse controller.
 * Implements append-only writes and log rotation (512 KB per file, 10 files
 * retained). Used exclusively by T9 (Event Logger). The driver is optional —
 * absent when the SD card feature is not fitted.
 *
 * SPI pin assignment (LOLIN S3):
 *   MOSI = GPIO 47   MISO = GPIO 48   CLK = GPIO 39   CS = GPIO 40
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
typedef enum {
    STORAGE_OK = 0,       /**< Operation succeeded */
    STORAGE_ERR_NO_CARD,  /**< No SD card detected / card not inserted */
    STORAGE_ERR_MOUNT,    /**< SPI bus or FAT32 mount failure */
    STORAGE_ERR_IO,       /**< Read / write error */
    STORAGE_ERR_NOT_FOUND,/**< File or path does not exist */
    STORAGE_ERR_FULL,     /**< Card or file system is full */
    STORAGE_ERR_PARAM     /**< Invalid parameter (NULL pointer, zero length, …) */
} storage_status_t;

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

/**
 * @brief Initialise the SPI bus and mount the FAT32 file system.
 *
 * Must be called once before any other storage_sd_* function.
 * Uses a custom SPIClass instance to drive the non-default LOLIN S3 pins.
 *
 * @return STORAGE_OK        — card detected and mounted.
 * @return STORAGE_ERR_NO_CARD — no card inserted.
 * @return STORAGE_ERR_MOUNT — card present but FAT32 mount failed.
 */
storage_status_t storage_init(void);

/**
 * @brief Query whether the SD card is currently mounted and usable.
 *
 * @return true if mounted; false otherwise.
 */
bool storage_sd_available(void);

/**
 * @brief Append a single text line (including its newline) to a file.
 *
 * The file is created if it does not already exist.  The line is written
 * verbatim; the caller is responsible for appending \\n if desired.
 *
 * @param filename  Absolute path on the FAT32 volume, e.g. "/log001.csv".
 * @param line      NUL-terminated string to write.
 * @return STORAGE_OK, STORAGE_ERR_PARAM, STORAGE_ERR_NO_CARD,
 *         STORAGE_ERR_FULL, STORAGE_ERR_IO.
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
 * After this call, storage_sd_available() returns false and T9 falls back to
 * NVS-only logging.  Call storage_init() to re-mount after card re-insertion.
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
