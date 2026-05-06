/**
 * @file nvs_config.cpp
 * @brief NVS configuration driver implementation — LIB-7.
 */

#ifndef UNIT_TEST
  #include <nvs_flash.h>
  #include <nvs.h>
#else
  #include "../test/mock_nvs.h"
#endif

#include "nvs_config.h"
#include <inttypes.h>
#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */

/** Open a handle, execute a lambda-like pattern via macros (see below). */
static nvs_cfg_status_t open_handle(const char *ns, nvs_open_mode_t mode,
                                     nvs_handle_t *h)
{
    esp_err_t e = nvs_open(ns, mode, h);
    if (e == ESP_OK) return NVS_CFG_OK;
    /* On real ESP-IDF, opening a non-existent namespace read-only returns
     * ESP_ERR_NVS_NOT_FOUND.  Propagate that as NOT_FOUND so callers can
     * distinguish "namespace/key absent" from a genuine flash error. */
    if (e == ESP_ERR_NVS_NOT_FOUND && mode == NVS_READONLY)
        return NVS_CFG_ERR_NOT_FOUND;
    return NVS_CFG_ERR_INIT;
}

/* ---------------------------------------------------------------------------
 * Initialisation
 * --------------------------------------------------------------------------- */

nvs_cfg_status_t nvs_cfg_init(void)
{
    esp_err_t ret = nvs_flash_init();

#ifndef UNIT_TEST
    /* Handle a corrupted or full NVS partition gracefully */
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
#endif

    if (ret != ESP_OK) return NVS_CFG_ERR_INIT;

    /* Read the stored schema version */
    int32_t stored_ver = 0;
    nvs_cfg_status_t st = nvs_cfg_get_i32(NVS_NS_SYSTEM, NVS_KEY_SCHEMA_VER,
                                            &stored_ver);

    nvs_cfg_status_t result = NVS_CFG_OK;

    if (st == NVS_CFG_ERR_NOT_FOUND) {
        /* First boot — stamp the current schema version */
        nvs_cfg_set_i32(NVS_NS_SYSTEM, NVS_KEY_SCHEMA_VER,
                         (int32_t)NVS_SCHEMA_VERSION);
    } else if (st != NVS_CFG_OK) {
        return NVS_CFG_ERR_INIT;
    } else if (stored_ver != (int32_t)NVS_SCHEMA_VERSION) {
        /*
         * Schema mismatch — update the stored version.
         * Namespaces are intentionally NOT erased: existing user settings are
         * preserved, new keys receive factory defaults via _or_default on first
         * access, and removed keys become harmless orphans.
         */
        nvs_cfg_set_i32(NVS_NS_SYSTEM, NVS_KEY_SCHEMA_VER,
                         (int32_t)NVS_SCHEMA_VERSION);
        result = NVS_CFG_ERR_MIGRATION;
    }

    /* Always record the currently running firmware version */
    nvs_cfg_set_str(NVS_NS_SYSTEM, NVS_KEY_FW_VERSION, FIRMWARE_VERSION);

    return result;
}

/* ---------------------------------------------------------------------------
 * Schema version query
 * --------------------------------------------------------------------------- */

nvs_cfg_status_t nvs_cfg_get_schema_version(int32_t *ver)
{
    return nvs_cfg_get_i32(NVS_NS_SYSTEM, NVS_KEY_SCHEMA_VER, ver);
}

/* ---------------------------------------------------------------------------
 * Typed get / set — i32
 * --------------------------------------------------------------------------- */

nvs_cfg_status_t nvs_cfg_get_i32(const char *ns, const char *key, int32_t *val)
{
    nvs_handle_t h;
    nvs_cfg_status_t os = open_handle(ns, NVS_READONLY, &h);
    if (os != NVS_CFG_OK) return os;
    esp_err_t e = nvs_get_i32(h, key, val);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return NVS_CFG_ERR_NOT_FOUND;
    if (e != ESP_OK)                return NVS_CFG_ERR_INIT;
    return NVS_CFG_OK;
}

nvs_cfg_status_t nvs_cfg_set_i32(const char *ns, const char *key, int32_t val)
{
    nvs_handle_t h;
    if (open_handle(ns, NVS_READWRITE, &h) != NVS_CFG_OK) return NVS_CFG_ERR_INIT;
    esp_err_t e = nvs_set_i32(h, key, val);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return (e == ESP_OK) ? NVS_CFG_OK : NVS_CFG_ERR_WRITE;
}

/* ---------------------------------------------------------------------------
 * Typed get / set — string
 * --------------------------------------------------------------------------- */

nvs_cfg_status_t nvs_cfg_get_str(const char *ns, const char *key,
                                  char *buf, size_t buf_len)
{
    if (buf_len == 0) return NVS_CFG_ERR_INIT;
    nvs_handle_t h;
    nvs_cfg_status_t os = open_handle(ns, NVS_READONLY, &h);
    if (os != NVS_CFG_OK) return os;
    size_t sz = buf_len;
    esp_err_t e = nvs_get_str(h, key, buf, &sz);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND)    return NVS_CFG_ERR_NOT_FOUND;
    if (e == ESP_ERR_NVS_INVALID_LENGTH) {
        /* String is longer than buf — result is truncated and null-terminated */
        buf[buf_len - 1] = '\0';
        return NVS_CFG_OK;
    }
    if (e != ESP_OK) return NVS_CFG_ERR_INIT;
    return NVS_CFG_OK;
}

nvs_cfg_status_t nvs_cfg_set_str(const char *ns, const char *key,
                                  const char *val)
{
    nvs_handle_t h;
    if (open_handle(ns, NVS_READWRITE, &h) != NVS_CFG_OK) return NVS_CFG_ERR_INIT;
    esp_err_t e = nvs_set_str(h, key, val);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return (e == ESP_OK) ? NVS_CFG_OK : NVS_CFG_ERR_WRITE;
}

/* ---------------------------------------------------------------------------
 * Typed get / set — blob
 * --------------------------------------------------------------------------- */

nvs_cfg_status_t nvs_cfg_get_blob(const char *ns, const char *key,
                                   void *buf, size_t *len)
{
    nvs_handle_t h;
    nvs_cfg_status_t os = open_handle(ns, NVS_READONLY, &h);
    if (os != NVS_CFG_OK) return os;
    esp_err_t e = nvs_get_blob(h, key, buf, len);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND)    return NVS_CFG_ERR_NOT_FOUND;
    if (e == ESP_ERR_NVS_INVALID_LENGTH) return NVS_CFG_ERR_INIT;
    if (e != ESP_OK)                   return NVS_CFG_ERR_INIT;
    return NVS_CFG_OK;
}

nvs_cfg_status_t nvs_cfg_set_blob(const char *ns, const char *key,
                                   const void *data, size_t len)
{
    nvs_handle_t h;
    if (open_handle(ns, NVS_READWRITE, &h) != NVS_CFG_OK) return NVS_CFG_ERR_INIT;
    esp_err_t e = nvs_set_blob(h, key, data, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return (e == ESP_OK) ? NVS_CFG_OK : NVS_CFG_ERR_WRITE;
}

/* ---------------------------------------------------------------------------
 * Erase namespace
 * --------------------------------------------------------------------------- */

nvs_cfg_status_t nvs_cfg_erase_namespace(const char *ns)
{
    nvs_handle_t h;
    if (open_handle(ns, NVS_READWRITE, &h) != NVS_CFG_OK) return NVS_CFG_ERR_INIT;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    return NVS_CFG_OK;
}

/* ---------------------------------------------------------------------------
 * _or_default helpers
 * --------------------------------------------------------------------------- */

nvs_cfg_status_t nvs_cfg_get_i32_or_default(const char *ns, const char *key,
                                              int32_t default_val, int32_t *val)
{
    nvs_cfg_status_t st = nvs_cfg_get_i32(ns, key, val);
    if (st == NVS_CFG_ERR_NOT_FOUND) {
        *val = default_val;
        st = nvs_cfg_set_i32(ns, key, default_val);
        if (st == NVS_CFG_OK) st = NVS_CFG_OK;  /* write succeeded */
    }
    return st;
}

nvs_cfg_status_t nvs_cfg_get_str_or_default(const char *ns, const char *key,
                                              const char *default_val,
                                              char *buf, size_t buf_len)
{
    nvs_cfg_status_t st = nvs_cfg_get_str(ns, key, buf, buf_len);
    if (st == NVS_CFG_ERR_NOT_FOUND) {
        strncpy(buf, default_val, buf_len - 1);
        buf[buf_len - 1] = '\0';
        st = nvs_cfg_set_str(ns, key, default_val);
        if (st == NVS_CFG_OK) st = NVS_CFG_OK;
    }
    return st;
}

/* ---------------------------------------------------------------------------
 * Ring-buffer event log
 * --------------------------------------------------------------------------- */

static void log_slot_key(char *out, size_t out_len, uint32_t slot)
{
    snprintf(out, out_len, "e%04" PRIu32, slot);
}

nvs_cfg_status_t nvs_log_append(const void *entry, size_t entry_size)
{
    /* Read head and count */
    int32_t head  = 0;
    int32_t count = 0;
    nvs_cfg_get_i32_or_default(NVS_NS_LOG, "head",  0, &head);
    nvs_cfg_get_i32_or_default(NVS_NS_LOG, "count", 0, &count);

    /* Write entry at current head slot */
    char slot_key[8];
    log_slot_key(slot_key, sizeof(slot_key), (uint32_t)head);
    nvs_cfg_status_t st = nvs_cfg_set_blob(NVS_NS_LOG, slot_key,
                                            entry, entry_size);
    if (st != NVS_CFG_OK) return st;

    /* Advance head (wrap) */
    head = (head + 1) % (int32_t)CONFIG_NVS_LOG_CAPACITY;

    /* Grow count until capacity reached */
    if (count < (int32_t)CONFIG_NVS_LOG_CAPACITY) count++;

    nvs_cfg_set_i32(NVS_NS_LOG, "head",  head);
    nvs_cfg_set_i32(NVS_NS_LOG, "count", count);
    return NVS_CFG_OK;
}

nvs_cfg_status_t nvs_log_read(uint32_t offset, void *buf,
                                uint32_t count, uint32_t *count_out)
{
    int32_t head  = 0;
    int32_t total = 0;
    nvs_cfg_get_i32_or_default(NVS_NS_LOG, "head",  0, &head);
    nvs_cfg_get_i32_or_default(NVS_NS_LOG, "count", 0, &total);

    if (offset >= (uint32_t)total) { *count_out = 0; return NVS_CFG_OK; }

    uint32_t available = (uint32_t)total - offset;
    if (count > available) count = available;

    /*
     * Oldest entry slot = (head - total + CONFIG_NVS_LOG_CAPACITY) % capacity
     * Logical offset 0 maps to that slot; each successive offset adds 1 (mod cap).
     */
    uint32_t capacity = (uint32_t)CONFIG_NVS_LOG_CAPACITY;
    uint32_t oldest   = ((uint32_t)head + capacity - (uint32_t)total) % capacity;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t read = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t slot = (oldest + offset + i) % capacity;
        char slot_key[8];
        log_slot_key(slot_key, sizeof(slot_key), slot);
        size_t sz = 256;   /* caller provides sufficient buffer per entry */
        nvs_cfg_status_t st = nvs_cfg_get_blob(NVS_NS_LOG, slot_key,
                                                dst, &sz);
        if (st != NVS_CFG_OK) break;
        dst += sz;
        read++;
    }
    *count_out = read;
    return NVS_CFG_OK;
}

uint32_t nvs_log_count(void)
{
    int32_t count = 0;
    nvs_cfg_get_i32(NVS_NS_LOG, "count", &count);
    return (count > 0) ? (uint32_t)count : 0u;
}
