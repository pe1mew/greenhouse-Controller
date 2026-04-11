/**
 * @file mock_sd.h
 * @brief Arduino SD / SPI stubs for the native (host) unit-test build of LIB-8.
 *
 * Provides an in-memory backing store (std::map keyed on filename → content)
 * together with function stubs that replicate the SD library calls used by
 * sd_storage.cpp.  All Arduino types are also defined here so that no ESP32
 * SDK headers are needed in the native build.
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
#include "../src/sd_storage.h"

#ifdef __cplusplus
#include <string>
#include <map>
#endif

/* ---------------------------------------------------------------------------
 * Mock control API — call from test setUp / test body
 * --------------------------------------------------------------------------- */
#ifdef __cplusplus
extern "C" {
#endif

/** Reset the mock: clear all files, set card present, clear free-bytes override. */
void mock_sd_reset(void);

/** Simulate an absent SD card (storage_init will return STORAGE_ERR_NO_CARD). */
void mock_sd_set_card_present(bool present);

/** Override the value returned by mock_sd_free_bytes(). Default: 64 MB. */
void mock_sd_set_free_bytes(uint64_t free_bytes);

/* ---------------------------------------------------------------------------
 * Stubs called by sd_storage.cpp in UNIT_TEST builds
 * --------------------------------------------------------------------------- */
bool             mock_sd_card_present(void);
bool             mock_sd_begin(void);
bool             mock_sd_write_append(const char *filename, const char *line);
storage_status_t mock_sd_read(const char *filename, uint32_t offset,
                              char *buf, size_t buf_len, size_t *bytes_read);
uint32_t         mock_sd_file_size(const char *filename);
uint64_t         mock_sd_free_bytes(void);
void             mock_sd_list_csv(const char *ext, char *buf, size_t buf_len);
storage_status_t mock_sd_delete(const char *filename);

#ifdef __cplusplus
}
#endif
