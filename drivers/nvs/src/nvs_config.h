/**
 * @file nvs_config.h
 * @brief NVS configuration driver — typed get/set, schema versioning, and
 *        _or_default helpers (LIB-7).
 *
 * All persistent configuration for the greenhouse controller passes through
 * this driver.  It wraps the ESP-IDF NVS API and adds:
 *
 *  - Schema versioning: a @c schema_ver integer and a @c fw_version string
 *    are stored in the @c system namespace. @ref nvs_cfg_init compares
 *    @c schema_ver against @ref NVS_SCHEMA_VERSION at startup and always
 *    overwrites @c fw_version with the current @ref FIRMWARE_VERSION.  On a
 *    version mismatch namespaces are NOT erased — existing user settings
 *    are preserved, new keys pick up factory defaults via the
 *    @c _or_default helpers on first access, and
 *    @ref NVS_CFG_ERR_MIGRATION is returned so the caller can log the event.
 *
 *  - @c _or_default helpers: if a key is absent the default is written and
 *    returned, so callers never need to handle
 *    @ref NVS_CFG_ERR_NOT_FOUND for optional/configurable parameters.
 *
 * ## Hardware
 *   - Storage      : ESP32-S3 internal NOR flash, dedicated @c nvs partition
 *                    (see @c partitions.csv).
 *   - Encryption   : NOT enabled in this firmware — secrets stored here are
 *                    plaintext.  Use the @c access namespace for hashes only.
 *
 * ## API summary
 *   - @ref nvs_cfg_init                      One-shot setup + schema check.
 *   - @ref nvs_cfg_get_schema_version        Read stored schema version.
 *   - @ref nvs_cfg_get_i32 / @ref nvs_cfg_set_i32         Integer.
 *   - @ref nvs_cfg_get_str / @ref nvs_cfg_set_str         String.
 *   - @ref nvs_cfg_get_blob / @ref nvs_cfg_set_blob       Binary blob.
 *   - @ref nvs_cfg_get_i32_or_default / @ref nvs_cfg_get_str_or_default
 *                                            Read-with-default helpers.
 *   - @ref nvs_cfg_erase_namespace           Wipe a namespace.
 *
 * ## Thread safety
 *   ESP-IDF NVS serialises all access internally; this driver adds no extra
 *   locking and is safe to call concurrently from any task.  Caller-supplied
 *   buffers (@p val, @p buf) are owned by the calling task only.
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
/* NVS_NS_LOG removed in 2.0.0-alpha.6.5 — the NVS event-log ringbuffer
 * (gh#22) was retired as redundant with the SD/CSV logging in T9.
 * See changelog [2.0.0-alpha.6.5] and design/tasks.md T9. */

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

/* CONFIG_NVS_LOG_CAPACITY default removed in 2.0.0-alpha.6.5 along with
 * the NVS event-log ringbuffer (gh#22). The build flag is also removed
 * from firmware/platformio.ini. */

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

/**
 * @brief Read a signed 32-bit integer.
 *
 * @param[in]  ns   Namespace (e.g. @ref NVS_NS_CLIMATE).
 * @param[in]  key  Key name.
 * @param[out] val  Receives the value (must not be NULL).
 * @return @ref NVS_CFG_OK on success, @ref NVS_CFG_ERR_NOT_FOUND if absent,
 *         @ref NVS_CFG_ERR_INIT on NVS-layer error.
 * @see    nvs_cfg_get_i32_or_default() — write-default-on-absent variant.
 */
nvs_cfg_status_t nvs_cfg_get_i32(const char *ns, const char *key, int32_t *val);

/**
 * @brief Write a signed 32-bit integer and commit.
 *
 * @param ns   Namespace.
 * @param key  Key name.
 * @param val  Value to write.
 * @return @ref NVS_CFG_OK on success, @ref NVS_CFG_ERR_WRITE on failure.
 */
nvs_cfg_status_t nvs_cfg_set_i32(const char *ns, const char *key, int32_t val);

/**
 * @brief Read a null-terminated string into @p buf (max @p buf_len bytes).
 *
 * If the stored string is longer than @p buf_len the result is silently
 * truncated and null-terminated.
 *
 * @param[in]  ns       Namespace.
 * @param[in]  key      Key name.
 * @param[out] buf      Destination buffer (must not be NULL).
 * @param[in]  buf_len  Capacity of @p buf in bytes (including NUL).
 * @return @ref NVS_CFG_OK on success, @ref NVS_CFG_ERR_NOT_FOUND if absent,
 *         @ref NVS_CFG_ERR_INIT on NVS-layer error.
 */
nvs_cfg_status_t nvs_cfg_get_str(const char *ns, const char *key,
                                  char *buf, size_t buf_len);

/**
 * @brief Write a null-terminated string and commit.
 *
 * @param ns   Namespace.
 * @param key  Key name.
 * @param val  Null-terminated string to store.
 * @return @ref NVS_CFG_OK, @ref NVS_CFG_ERR_WRITE.
 */
nvs_cfg_status_t nvs_cfg_set_str(const char *ns, const char *key,
                                  const char *val);

/**
 * @brief Read a binary blob.
 *
 * On entry @p *len is the buffer capacity; on exit it is set to the number
 * of bytes actually read.
 *
 * @param[in]      ns   Namespace.
 * @param[in]      key  Key name.
 * @param[out]     buf  Destination buffer (must not be NULL).
 * @param[in,out]  len  In: buffer capacity in bytes.  Out: bytes read.
 * @return @ref NVS_CFG_OK, @ref NVS_CFG_ERR_NOT_FOUND if absent,
 *         @ref NVS_CFG_ERR_INIT if @p buf is too small.
 */
nvs_cfg_status_t nvs_cfg_get_blob(const char *ns, const char *key,
                                   void *buf, size_t *len);

/**
 * @brief Write a binary blob and commit.
 *
 * @param ns    Namespace.
 * @param key   Key name.
 * @param data  Bytes to store (may contain NUL).
 * @param len   Number of bytes to write.
 * @return @ref NVS_CFG_OK, @ref NVS_CFG_ERR_WRITE.
 */
nvs_cfg_status_t nvs_cfg_set_blob(const char *ns, const char *key,
                                   const void *data, size_t len);

/**
 * @brief Erase every key in namespace @p ns and commit.
 *
 * @param ns  Namespace to wipe.
 * @return @ref NVS_CFG_OK on success, @ref NVS_CFG_ERR_WRITE on failure.
 * @warning Destructive: deletes every key in the namespace, including any
 *          factory-default values previously written by @c _or_default
 *          helpers.  Intended for a "factory reset" command only.
 */
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

/* ---------------------------------------------------------------------------
 * Ring-buffer event log — REMOVED in 2.0.0-alpha.6.5
 *
 * The NVS-backed event-log ringbuffer (gh#22, originally added for
 * boot-survival event recording when SD might fail) was retired during
 * the v2.0.0 migration. SD-based logging in T9 (event_logger) is the
 * single source of truth; the additional NVS ring served no purpose
 * that wasn't already covered by SD persistence + the production-
 * proven NVS-fallback policy that has been removed from T9 as part
 * of the same design change.
 *
 * Build-flag CONFIG_NVS_LOG_CAPACITY (platformio.ini) and the doc
 * mentions in design/tasks.md / TSDS were removed in the same alpha
 * (and in alpha.6.5.1 documentation follow-up).
 * --------------------------------------------------------------------------- */
