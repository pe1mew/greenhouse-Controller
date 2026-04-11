/**
 * @file mock_nvs.h
 * @brief ESP-IDF NVS stubs for the native (host) unit-test build of LIB-7.
 *
 * Provides an in-memory backing store (std::map keyed on "namespace:key")
 * together with function stubs that replicate the ESP-IDF NVS API used by
 * nvs_config.cpp.  All ESP-IDF types and error codes are also defined here so
 * that no ESP32 SDK headers are needed in the native build.
 *
 * @note Do NOT include this header in production (target) builds.
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <map>
#include <vector>

/* ---------------------------------------------------------------------------
 * ESP-IDF type stubs
 * --------------------------------------------------------------------------- */
typedef int    esp_err_t;
typedef int    nvs_handle_t;

#define ESP_OK                         0
#define ESP_ERR_NVS_NOT_FOUND          0x1102
#define ESP_ERR_NVS_INVALID_LENGTH     0x1104
#define ESP_ERR_NVS_INVALID_HANDLE     0x1105
#define ESP_ERR_NVS_NO_FREE_PAGES      0x1106
#define ESP_ERR_NVS_NEW_VERSION_FOUND  0x1107

typedef enum {
    NVS_READONLY  = 0,
    NVS_READWRITE = 1
} nvs_open_mode_t;

/* printf format macro for uint32_t on both MinGW and Linux */
#ifndef PRIu32
  #define PRIu32 "u"
#endif

/* ---------------------------------------------------------------------------
 * In-memory backing store
 *
 * Key format: "<namespace>:<key>"
 * Value: raw bytes (int32 stored as 4 bytes LE, string includes null
 *        terminator, blob is stored as-is).
 *
 * Tests may inspect or inject values directly via mock_nvs_store, or use
 * the helper mock_nvs_inject_i32() for convenience.
 * --------------------------------------------------------------------------- */
extern std::map<std::string, std::vector<uint8_t>> mock_nvs_store;

/* ---------------------------------------------------------------------------
 * Mock control
 * --------------------------------------------------------------------------- */

/** @brief Clear the backing store and all open handles. Call from setUp(). */
void mock_nvs_reset(void);

/**
 * @brief Inject an i32 value directly into the backing store.
 *
 * Useful for pre-populating NVS state before calling nvs_cfg_init() in tests
 * that exercise schema mismatch (UT-NVS-016/017).
 */
void mock_nvs_inject_i32(const char *ns, const char *key, int32_t val);

/* ---------------------------------------------------------------------------
 * ESP-IDF NVS stub declarations
 * --------------------------------------------------------------------------- */
esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out_handle);
esp_err_t nvs_get_i32(nvs_handle_t handle, const char *key, int32_t *out);
esp_err_t nvs_set_i32(nvs_handle_t handle, const char *key, int32_t val);
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key,
                       char *out, size_t *len);
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *val);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key,
                        void *out, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                        const void *data, size_t len);
esp_err_t nvs_erase_all(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);
void      nvs_close(nvs_handle_t handle);
