/**
 * @file mock_sd.cpp
 * @brief Arduino SD / SPI stub implementations for the native unit-test build.
 *
 * Backing store: std::map<std::string, std::string> (filename → full content).
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#ifdef UNIT_TEST

#include "mock_sd.h"
#include <string.h>
#include <string>
#include <map>

/* ---------------------------------------------------------------------------
 * Internal state
 * --------------------------------------------------------------------------- */
static std::map<std::string, std::string> g_files;
static bool     g_card_present = true;
static uint64_t g_free_bytes   = 64ULL * 1024 * 1024; /* 64 MB default */

/* ---------------------------------------------------------------------------
 * Mock control
 * --------------------------------------------------------------------------- */
void mock_sd_reset(void)
{
    g_files.clear();
    g_card_present = true;
    g_free_bytes   = 64ULL * 1024 * 1024;
}

void mock_sd_set_card_present(bool present)
{
    g_card_present = present;
}

void mock_sd_set_free_bytes(uint64_t free_bytes)
{
    g_free_bytes = free_bytes;
}

/* ---------------------------------------------------------------------------
 * Stubs
 * --------------------------------------------------------------------------- */
bool mock_sd_card_present(void)
{
    return g_card_present;
}

bool mock_sd_begin(void)
{
    return g_card_present;
}

bool mock_sd_write_append(const char *filename, const char *line)
{
    if (!filename || !line) {
        return false;
    }
    g_files[std::string(filename)] += std::string(line);
    return true;
}

storage_status_t mock_sd_read(const char *filename, uint32_t offset,
                              char *buf, size_t buf_len, size_t *bytes_read)
{
    if (!filename || !buf || buf_len == 0 || !bytes_read) {
        return STORAGE_ERR_PARAM;
    }
    *bytes_read = 0;

    auto it = g_files.find(std::string(filename));
    if (it == g_files.end()) {
        buf[0] = '\0';
        return STORAGE_ERR_NOT_FOUND;
    }

    const std::string &content = it->second;
    if (offset >= content.size()) {
        buf[0] = '\0';
        return STORAGE_OK;
    }

    size_t available = content.size() - offset;
    size_t max_copy  = buf_len - 1;
    size_t n         = available < max_copy ? available : max_copy;
    memcpy(buf, content.c_str() + offset, n);
    buf[n]      = '\0';
    *bytes_read = n;
    return STORAGE_OK;
}

uint32_t mock_sd_file_size(const char *filename)
{
    if (!filename) {
        return 0;
    }
    auto it = g_files.find(std::string(filename));
    if (it == g_files.end()) {
        return 0;
    }
    return static_cast<uint32_t>(it->second.size());
}

uint64_t mock_sd_free_bytes(void)
{
    return g_free_bytes;
}

void mock_sd_list_csv(const char *ext, char *buf, size_t buf_len)
{
    if (!ext || !buf || buf_len == 0) {
        return;
    }
    buf[0] = '\0';
    size_t ext_len = strlen(ext);
    size_t pos = 0;

    for (const auto &kv : g_files) {
        const std::string &name = kv.first;
        if (name.size() >= ext_len &&
            name.compare(name.size() - ext_len, ext_len, ext) == 0) {
            /* Strip leading '/' that the driver stores internally */
            const char *display = name.c_str();
            if (display[0] == '/') {
                display++;
            }
            size_t dlen = strlen(display);
            size_t needed = dlen + 1; /* +1 for comma */
            if (pos + needed < buf_len) {
                memcpy(buf + pos, display, dlen);
                pos += dlen;
                buf[pos++] = ',';
                buf[pos]   = '\0';
            }
        }
    }
}

storage_status_t mock_sd_delete(const char *filename)
{
    if (!filename) {
        return STORAGE_ERR_PARAM;
    }
    auto it = g_files.find(std::string(filename));
    if (it == g_files.end()) {
        return STORAGE_ERR_NOT_FOUND;
    }
    g_files.erase(it);
    return STORAGE_OK;
}

#endif /* UNIT_TEST */
