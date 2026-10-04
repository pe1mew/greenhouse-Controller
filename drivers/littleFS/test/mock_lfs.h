/**
 * @file mock_lfs.h
 * @brief Arduino LittleFS / ESP-IDF OTA stubs for the native unit-test build.
 *
 * Provides an in-memory backing store (one std::map per partition, keyed on
 * path → content bytes) together with function stubs that replicate every
 * LittleFS / OTA call used by littlefs_storage.cpp.  No Arduino or ESP-IDF
 * headers are needed in the native build.
 *
 * @note Do NOT include this header in production (target) builds.
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../src/littlefs_storage.h"

/* ---------------------------------------------------------------------------
 * Mock control API — call from test setUp / test body
 * --------------------------------------------------------------------------- */
#ifdef __cplusplus
extern "C" {
#endif

/** Reset the mock: clear all files in both partitions, set defaults. */
void mock_lfs_reset(void);

/**
 * Set which partition mock_lfs_active_partition() will report.
 * Default: LFS_PARTITION_A.
 */
void mock_lfs_set_active_partition(lfs_partition_t p);

/* The esp_err_t values the target driver sees (esp_err.h), for the mount
 * stubs below (gh#89). */
#define MOCK_ESP_FAIL               (-1)    /* lfs_mount refused the contents */
#define MOCK_ESP_ERR_NO_MEM         0x101   /* no VFS slot, or no memory */
#define MOCK_ESP_ERR_INVALID_STATE  0x103   /* the label in use */
#define MOCK_ESP_ERR_NOT_FOUND      0x105   /* the label missing */

/**
 * Simulate a mount failure for the specified partition: a refusal that is NOT
 * about its contents (MOCK_ESP_ERR_NO_MEM). Cleared by mock_lfs_reset().
 */
void mock_lfs_set_mount_fail(lfs_partition_t p, bool fail);

/**
 * Make the next mounts of @p p fail with @p esp_err (0 = they succeed) until
 * it is changed, mock_lfs_reset() runs, or -- for MOCK_ESP_FAIL only -- the
 * partition is formatted (gh#89).
 */
void mock_lfs_set_mount_err(lfs_partition_t p, int esp_err);

/** Set the free-bytes value returned for the specified partition. Default: 512 KB. */
void mock_lfs_set_free_bytes(lfs_partition_t p, uint64_t free_bytes);

/* ---------------------------------------------------------------------------
 * Stubs called by littlefs_storage.cpp in UNIT_TEST builds
 * --------------------------------------------------------------------------- */
int              mock_lfs_mount_err(lfs_partition_t partition);  /* 0 = mounts */
void             mock_lfs_format(lfs_partition_t partition);
void             mock_lfs_end(lfs_partition_t partition);
lfs_status_t     mock_lfs_read(lfs_partition_t partition, const char *path,
                                char *buf, size_t buf_len);
lfs_status_t     mock_lfs_write(lfs_partition_t partition, const char *path,
                                 const void *data, size_t len);
bool             mock_lfs_exists(lfs_partition_t partition, const char *path);
uint64_t         mock_lfs_free_bytes(lfs_partition_t partition);
lfs_partition_t  mock_lfs_active_partition(void);

#ifdef __cplusplus
}
#endif
