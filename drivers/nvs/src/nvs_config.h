/**
 * @file nvs_config.h
 * @brief NVS configuration driver — typed get/set, schema versioning,
 *        _or_default helpers, and ring-buffer event log (LIB-7).
 *
 * All persistent configuration for the greenhouse controller passes through
 * this driver. It wraps the ESP-IDF NVS API and adds:
 *
 *  - Schema versioning: a `schema_ver` integer and a `fw_version` string are
 *    stored in the `system` namespace. nvs_cfg_init() compares `schema_ver`
 *    against NVS_SCHEMA_VERSION at startup and always overwrites `fw_version`
 *    with the current FIRMWARE_VERSION. On a version mismatch namespaces are
 *    NOT erased — existing user settings are preserved, new keys pick up
 *    factory defaults via _or_default on first access, and NVS_CFG_ERR_MIGRATION
 *    is returned so the caller can log the event.
 *
 *  - _or_default helpers: if a key is absent the default is written and
 *    returned, so callers never need to handle NVS_CFG_ERR_NOT_FOUND for
 *    optional/configurable parameters.
 *
 *  - Ring-buffer log: a 1000-entry FIFO stored in the `log` namespace as the
 *    fallback event store when no SD card is present.
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * NVS namespaces — match TSDS §5.10
 * --------------------------------------------------------------------------- */
#define NVS_NS_CLIMATE  "climate"
#define NVS_NS_WIND     "wind"
#define NVS_NS_MOTOR    "motor"
#define NVS_NS_ACCESS   "access"
#define NVS_NS_WIFI     "wifi"
#define NVS_NS_MQTT     "mqtt"
#define NVS_NS_SYSTEM   "system"
#define NVS_NS_LOG      "log"

/* ---------------------------------------------------------------------------
 * Schema versioning
 *
 * Increment NVS_SCHEMA_VERSION whenever any key name, value type, or
 * namespace layout changes in a way that makes stored NVS data incompatible
 * with the new firmware. nvs_cfg_init() will detect the mismatch, stamp the
 * new schema version, overwrite fw_version, and return NVS_CFG_ERR_MIGRATION.
 * Namespaces are NOT erased — existing user settings are preserved.
 * _or_default helpers write factory defaults for any new keys on first access.
 * --------------------------------------------------------------------------- */

/** @brief Current schema version. Bump this whenever the NVS layout changes. */
#ifndef NVS_SCHEMA_VERSION
  #define NVS_SCHEMA_VERSION  1
#endif

/** @brief Running firmware version string written to NVS on every boot. */
#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION  "0.1.0"
#endif

/** @brief NVS key used to store the schema version (in NVS_NS_SYSTEM). */
#define NVS_KEY_SCHEMA_VER  "schema_ver"

/** @brief NVS key used to store the running firmware version (in NVS_NS_SYSTEM). */
#define NVS_KEY_FW_VERSION  "fw_version"

/* ---------------------------------------------------------------------------
 * Log ring-buffer capacity
 * --------------------------------------------------------------------------- */
#ifndef CONFIG_NVS_LOG_CAPACITY
  #define CONFIG_NVS_LOG_CAPACITY  1000
#endif

/* ---------------------------------------------------------------------------
 * Status codes
 * --------------------------------------------------------------------------- */

/**
 * @brief Return codes for all nvs_cfg_* functions.
 */
typedef enum {
    NVS_CFG_OK = 0,           /**< Operation succeeded. */
    NVS_CFG_ERR_NOT_FOUND,    /**< Key does not exist in NVS. */
    NVS_CFG_ERR_WRITE,        /**< NVS write or commit failed. */
    NVS_CFG_ERR_INIT,         /**< NVS flash init or handle open failed. */
    NVS_CFG_ERR_MIGRATION     /**< Schema version mismatch — defaults applied;
                                    caller should log this event. */
} nvs_cfg_status_t;

/* ---------------------------------------------------------------------------
 * Initialisation
 * --------------------------------------------------------------------------- */

/**
 * @brief Initialise NVS flash and enforce schema versioning.
 *
 * Must be called once at startup before any other nvs_cfg_* function.
 *
 * Behaviour:
 *  - Calls nvs_flash_init() (with automatic erase-and-reinit on corruption).
 *  - Always writes `system/fw_version = FIRMWARE_VERSION`.
 *  - Reads `system/schema_ver` from flash.
 *    - If absent (first boot): writes NVS_SCHEMA_VERSION; returns NVS_CFG_OK.
 *    - If equal to NVS_SCHEMA_VERSION: returns NVS_CFG_OK.
 *    - If different: writes new NVS_SCHEMA_VERSION; namespaces are NOT erased
 *      (user settings preserved); returns NVS_CFG_ERR_MIGRATION.
 *
 * @return NVS_CFG_OK on success, NVS_CFG_ERR_MIGRATION if migration ran,
 *         NVS_CFG_ERR_INIT if the flash could not be initialised.
 */
nvs_cfg_status_t nvs_cfg_init(void);

/* ---------------------------------------------------------------------------
 * Schema version query
 * --------------------------------------------------------------------------- */

/**
 * @brief Read the schema version currently stored in NVS.
 *
 * @param[out] ver  Receives the stored version number.
 * @return NVS_CFG_OK, NVS_CFG_ERR_NOT_FOUND, or NVS_CFG_ERR_INIT.
 */
nvs_cfg_status_t nvs_cfg_get_schema_version(int32_t *ver);

/* ---------------------------------------------------------------------------
 * Typed get / set
 * --------------------------------------------------------------------------- */

/** @brief Read a signed 32-bit integer. Returns NVS_CFG_ERR_NOT_FOUND if absent. */
nvs_cfg_status_t nvs_cfg_get_i32(const char *ns, const char *key, int32_t *val);

/** @brief Write a signed 32-bit integer and commit. */
nvs_cfg_status_t nvs_cfg_set_i32(const char *ns, const char *key, int32_t val);

/**
 * @brief Read a null-terminated string into @p buf (max @p buf_len bytes).
 *
 * If the stored string is longer than @p buf_len the result is silently
 * truncated and null-terminated. Returns NVS_CFG_ERR_NOT_FOUND if absent.
 */
nvs_cfg_status_t nvs_cfg_get_str(const char *ns, const char *key,
                                  char *buf, size_t buf_len);

/** @brief Write a null-terminated string and commit. */
nvs_cfg_status_t nvs_cfg_set_str(const char *ns, const char *key,
                                  const char *val);

/**
 * @brief Read a binary blob. On entry @p *len is the buffer capacity; on exit
 *        it is set to the number of bytes read. Returns NVS_CFG_ERR_NOT_FOUND
 *        if absent, NVS_CFG_ERR_INIT if the buffer is too small.
 */
nvs_cfg_status_t nvs_cfg_get_blob(const char *ns, const char *key,
                                   void *buf, size_t *len);

/** @brief Write a binary blob and commit. */
nvs_cfg_status_t nvs_cfg_set_blob(const char *ns, const char *key,
                                   const void *data, size_t len);

/** @brief Erase every key in namespace @p ns and commit. */
nvs_cfg_status_t nvs_cfg_erase_namespace(const char *ns);

/* ---------------------------------------------------------------------------
 * _or_default helpers
 *
 * These combine get + conditional write into a single call.
 *
 * If the key is present:  reads it; @p default_val / @p default_data are
 *                         ignored; returns NVS_CFG_OK.
 * If the key is absent:   writes the supplied default to NVS; fills the
 *                         caller's output with the default; returns NVS_CFG_OK.
 *
 * This means callers never need to handle NVS_CFG_ERR_NOT_FOUND for
 * configuration parameters that have a compile-time default.
 * --------------------------------------------------------------------------- */

/**
 * @brief Get an i32, or write @p default_val if absent.
 *
 * @param[in]  ns           Namespace.
 * @param[in]  key          Key name.
 * @param[in]  default_val  Value to write and return when the key is absent.
 * @param[out] val          Receives the value (stored or default).
 */
nvs_cfg_status_t nvs_cfg_get_i32_or_default(const char *ns, const char *key,
                                              int32_t default_val, int32_t *val);

/**
 * @brief Get a string, or write @p default_val if absent.
 *
 * @param[in]  ns           Namespace.
 * @param[in]  key          Key name.
 * @param[in]  default_val  Null-terminated string to write and return when absent.
 * @param[out] buf          Caller-supplied buffer for the result.
 * @param[in]  buf_len      Size of @p buf including the null terminator.
 */
nvs_cfg_status_t nvs_cfg_get_str_or_default(const char *ns, const char *key,
                                              const char *default_val,
                                              char *buf, size_t buf_len);

/**
 * @brief Get a blob, or write @p default_data if absent.
 *
 * @param[in]     ns            Namespace.
 * @param[in]     key           Key name.
 * @param[in]     default_data  Bytes to write and return when absent.
 * @param[in]     default_len   Length of @p default_data.
 * @param[out]    buf           Caller-supplied buffer.
 * @param[in,out] len           In: buffer capacity. Out: bytes written.
 */
nvs_cfg_status_t nvs_cfg_get_blob_or_default(const char *ns, const char *key,
                                               const void *default_data,
                                               size_t default_len,
                                               void *buf, size_t *len);

/* ---------------------------------------------------------------------------
 * Ring-buffer event log  (NVS_NS_LOG namespace)
 *
 * Fixed-capacity FIFO.  Oldest entries are overwritten when the ring is full.
 * Keys used internally:
 *   "head"  (i32) — next write slot, 0-based, wraps at CONFIG_NVS_LOG_CAPACITY
 *   "count" (i32) — number of valid entries (≤ capacity)
 *   "eNNNN" (blob)— entry at slot NNNN (zero-padded 4-digit index)
 * --------------------------------------------------------------------------- */

/** @brief Append one entry of @p entry_size bytes. Overwrites oldest on wrap. */
nvs_cfg_status_t nvs_log_append(const void *entry, size_t entry_size);

/**
 * @brief Read up to @p count entries starting at logical @p offset.
 *        offset=0 is the oldest surviving entry.
 * @param[out] count_out  Actual number of entries read (≤ count).
 */
nvs_cfg_status_t nvs_log_read(uint32_t offset, void *buf,
                                uint32_t count, uint32_t *count_out);

/** @brief Return the number of valid log entries currently stored. */
uint32_t nvs_log_count(void);
