/**
 * @file mock_lfs.cpp
 * @brief Arduino LittleFS / OTA stub implementations for the native build.
 *
 * Backing store: two std::map<std::string, std::vector<uint8_t>> instances,
 * one per partition.
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#ifdef UNIT_TEST

#include "mock_lfs.h"
#include <string.h>
#include <string>
#include <vector>
#include <map>

/* ---------------------------------------------------------------------------
 * Internal state
 * --------------------------------------------------------------------------- */
static std::map<std::string, std::vector<uint8_t>> g_files[2];
static bool            g_mount_fail[2]  = {false, false};
static uint64_t        g_free_bytes[2]  = {512ULL * 1024, 512ULL * 1024};
static lfs_partition_t g_active         = LFS_PARTITION_A;

/* ---------------------------------------------------------------------------
 * Mock control
 * --------------------------------------------------------------------------- */
void mock_lfs_reset(void)
{
    g_files[0].clear();
    g_files[1].clear();
    g_mount_fail[0]  = false;
    g_mount_fail[1]  = false;
    g_free_bytes[0]  = 512ULL * 1024;
    g_free_bytes[1]  = 512ULL * 1024;
    g_active         = LFS_PARTITION_A;
}

void mock_lfs_set_active_partition(lfs_partition_t p)
{
    g_active = p;
}

void mock_lfs_set_mount_fail(lfs_partition_t p, bool fail)
{
    if (p == LFS_PARTITION_A || p == LFS_PARTITION_B) {
        g_mount_fail[p] = fail;
    }
}

void mock_lfs_set_free_bytes(lfs_partition_t p, uint64_t free_bytes)
{
    if (p == LFS_PARTITION_A || p == LFS_PARTITION_B) {
        g_free_bytes[p] = free_bytes;
    }
}

/* ---------------------------------------------------------------------------
 * Stubs
 * --------------------------------------------------------------------------- */
bool mock_lfs_begin(lfs_partition_t p)
{
    if (p != LFS_PARTITION_A && p != LFS_PARTITION_B) {
        return false;
    }
    return !g_mount_fail[p];
}

void mock_lfs_end(lfs_partition_t /*p*/) {}

lfs_status_t mock_lfs_read(lfs_partition_t p, const char *path,
                            char *buf, size_t buf_len)
{
    if (!path || !buf || buf_len == 0) {
        return LFS_ERR_IO;
    }
    auto it = g_files[p].find(std::string(path));
    if (it == g_files[p].end()) {
        buf[0] = '\0';
        return LFS_ERR_NOT_FOUND;
    }
    const std::vector<uint8_t> &content = it->second;
    size_t max_copy = buf_len - 1;
    size_t n = content.size() < max_copy ? content.size() : max_copy;
    memcpy(buf, content.data(), n);
    buf[n] = '\0';
    return LFS_OK;
}

lfs_status_t mock_lfs_write(lfs_partition_t p, const char *path,
                             const void *data, size_t len)
{
    if (!path || !data) {
        return LFS_ERR_IO;
    }
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(data);
    g_files[p][std::string(path)] = std::vector<uint8_t>(bytes, bytes + len);
    return LFS_OK;
}

bool mock_lfs_exists(lfs_partition_t p, const char *path)
{
    if (!path) {
        return false;
    }
    return g_files[p].find(std::string(path)) != g_files[p].end();
}

uint64_t mock_lfs_free_bytes(lfs_partition_t p)
{
    if (p != LFS_PARTITION_A && p != LFS_PARTITION_B) {
        return 0;
    }
    return g_free_bytes[p];
}

lfs_partition_t mock_lfs_active_partition(void)
{
    return g_active;
}

#endif /* UNIT_TEST */
