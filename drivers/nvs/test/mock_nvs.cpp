/**
 * @file mock_nvs.cpp
 * @brief ESP-IDF NVS stub implementation for LIB-7 unit tests.
 */

#include "mock_nvs.h"
#include <string.h>
#include <map>
#include <string>
#include <vector>

/* ---------------------------------------------------------------------------
 * Backing store
 * --------------------------------------------------------------------------- */
std::map<std::string, std::vector<uint8_t>> mock_nvs_store;

/* ---------------------------------------------------------------------------
 * Handle table: handle → namespace name
 * --------------------------------------------------------------------------- */
static std::map<nvs_handle_t, std::string> active_handles;
static nvs_handle_t next_handle = 1;

/* ---------------------------------------------------------------------------
 * Internal key builder
 * --------------------------------------------------------------------------- */
static std::string make_key(nvs_handle_t handle, const char *key)
{
    return active_handles.at(handle) + ":" + key;
}

/* ---------------------------------------------------------------------------
 * Mock control
 * --------------------------------------------------------------------------- */

void mock_nvs_reset(void)
{
    mock_nvs_store.clear();
    active_handles.clear();
    next_handle = 1;
}

void mock_nvs_inject_i32(const char *ns, const char *key, int32_t val)
{
    std::string fkey = std::string(ns) + ":" + key;
    std::vector<uint8_t> bytes(sizeof(int32_t));
    memcpy(bytes.data(), &val, sizeof(int32_t));
    mock_nvs_store[fkey] = bytes;
}

/* ---------------------------------------------------------------------------
 * ESP-IDF NVS stub implementations
 * --------------------------------------------------------------------------- */

esp_err_t nvs_flash_init(void)
{
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void)
{
    mock_nvs_store.clear();
    return ESP_OK;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t /*mode*/, nvs_handle_t *out_handle)
{
    *out_handle = next_handle++;
    active_handles[*out_handle] = std::string(name);
    return ESP_OK;
}

esp_err_t nvs_get_i32(nvs_handle_t handle, const char *key, int32_t *out)
{
    std::string fkey = make_key(handle, key);
    auto it = mock_nvs_store.find(fkey);
    if (it == mock_nvs_store.end()) return ESP_ERR_NVS_NOT_FOUND;
    if (it->second.size() != sizeof(int32_t)) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, it->second.data(), sizeof(int32_t));
    return ESP_OK;
}

esp_err_t nvs_set_i32(nvs_handle_t handle, const char *key, int32_t val)
{
    std::string fkey = make_key(handle, key);
    std::vector<uint8_t> bytes(sizeof(int32_t));
    memcpy(bytes.data(), &val, sizeof(int32_t));
    mock_nvs_store[fkey] = bytes;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key,
                       char *out, size_t *len)
{
    std::string fkey = make_key(handle, key);
    auto it = mock_nvs_store.find(fkey);
    if (it == mock_nvs_store.end()) return ESP_ERR_NVS_NOT_FOUND;

    const std::vector<uint8_t>& data = it->second;
    size_t stored = data.size();   /* includes null terminator */

    if (out == nullptr) {
        /* Caller queries required length */
        *len = stored;
        return ESP_OK;
    }

    if (*len < stored) {
        /* Buffer too small — truncate and null-terminate */
        memcpy(out, data.data(), *len - 1);
        out[*len - 1] = '\0';
        return ESP_ERR_NVS_INVALID_LENGTH;
    }

    memcpy(out, data.data(), stored);
    *len = stored;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *val)
{
    std::string fkey = make_key(handle, key);
    size_t len = strlen(val) + 1;   /* include null terminator */
    std::vector<uint8_t> bytes(len);
    memcpy(bytes.data(), val, len);
    mock_nvs_store[fkey] = bytes;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key,
                        void *out, size_t *len)
{
    std::string fkey = make_key(handle, key);
    auto it = mock_nvs_store.find(fkey);
    if (it == mock_nvs_store.end()) return ESP_ERR_NVS_NOT_FOUND;

    const std::vector<uint8_t>& data = it->second;
    if (*len < data.size()) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, data.data(), data.size());
    *len = data.size();
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                        const void *data, size_t len)
{
    std::string fkey = make_key(handle, key);
    const uint8_t *src = static_cast<const uint8_t *>(data);
    mock_nvs_store[fkey] = std::vector<uint8_t>(src, src + len);
    return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t handle)
{
    if (active_handles.find(handle) == active_handles.end())
        return ESP_ERR_NVS_INVALID_HANDLE;

    std::string prefix = active_handles[handle] + ":";
    for (auto it = mock_nvs_store.begin(); it != mock_nvs_store.end(); ) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            it = mock_nvs_store.erase(it);
        } else {
            ++it;
        }
    }
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t /*handle*/)
{
    return ESP_OK;   /* no-op: all writes are immediately visible in the map */
}

void nvs_close(nvs_handle_t handle)
{
    active_handles.erase(handle);
}
