/**
 * @file sd_storage.cpp
 * @brief SD card file I/O driver — FAT32 over SPI (LIB-8).
 *
 * Target build: uses the Arduino SD library (Adafruit fork) together with a
 * custom SPIClass to drive the non-default LOLIN S3 SPI pins.
 *
 * Native (unit-test) build: all SD / SPI calls are replaced by the stubs
 * defined in test/mock_sd.h.
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#include "sd_storage.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Platform abstraction — Arduino target vs. native unit-test build
 * --------------------------------------------------------------------------- */
#ifndef UNIT_TEST
  #include <Arduino.h>
  #include <SPI.h>
  #include <SD.h>
  static SPIClass g_spi(FSPI);
#else
  #include "mock_sd.h"
#endif

/* ---------------------------------------------------------------------------
 * Module state
 * --------------------------------------------------------------------------- */
static bool g_mounted = false;

/* ---------------------------------------------------------------------------
 * storage_init
 * --------------------------------------------------------------------------- */
storage_status_t storage_init(void)
{
    g_mounted = false;

#ifndef UNIT_TEST
    g_spi.begin(SD_PIN_CLK, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS);
    if (!SD.begin(SD_PIN_CS, g_spi)) {
        /* Distinguish between absent card and mount failure.
         * The Adafruit SD library does not expose a separate "no card" error;
         * a failed begin() with no card inserted behaves the same way —
         * treat it uniformly as STORAGE_ERR_NO_CARD. */
        return STORAGE_ERR_NO_CARD;
    }
    uint8_t card_type = SD.cardType();
    if (card_type == CARD_NONE) {
        return STORAGE_ERR_NO_CARD;
    }
    /* gh#14 (since 1.17.32): the Arduino-ESP32 SD library's SPI-level state
     * survives SD.end()/SD.begin() cycles. After a clean unmount + physical
     * removal, the next storage_init() can see SD.begin() return true and
     * SD.cardType() return the previously-cached type — both fail-safes
     * pass even though no card is present.  SD.totalBytes() is the honest
     * function in this chain: it round-trips to the card hardware and
     * returns 0 when none is there.  A zero total before we flip g_mounted
     * means "lying state — treat as absent". */
    if (SD.totalBytes() == 0) {
        SD.end();   /* release SPI claim so a future re-mount starts clean */
        return STORAGE_ERR_NO_CARD;
    }
#else
    if (!mock_sd_card_present()) {
        return STORAGE_ERR_NO_CARD;
    }
    if (!mock_sd_begin()) {
        return STORAGE_ERR_MOUNT;
    }
#endif

    g_mounted = true;
    return STORAGE_OK;
}

/* ---------------------------------------------------------------------------
 * storage_sd_available
 * --------------------------------------------------------------------------- */
bool storage_sd_available(void)
{
    return g_mounted;
}

/* ---------------------------------------------------------------------------
 * storage_sd_write_append
 * --------------------------------------------------------------------------- */
storage_status_t storage_sd_write_append(const char *filename, const char *line)
{
    if (!filename || !line) {
        return STORAGE_ERR_PARAM;
    }
    if (!g_mounted) {
        return STORAGE_ERR_NO_CARD;
    }

#ifndef UNIT_TEST
    File f = SD.open(filename, FILE_APPEND);
    if (!f) {
        return STORAGE_ERR_IO;
    }
    size_t len = strlen(line);
    size_t written = f.write(reinterpret_cast<const uint8_t *>(line), len);
    f.close();
    if (written != len) {
        return STORAGE_ERR_FULL;
    }
#else
    if (!mock_sd_write_append(filename, line)) {
        return STORAGE_ERR_IO;
    }
#endif

    return STORAGE_OK;
}

/* ---------------------------------------------------------------------------
 * storage_sd_read
 * --------------------------------------------------------------------------- */
storage_status_t storage_sd_read(const char *filename, uint32_t offset,
                                 char *buf, size_t buf_len, size_t *bytes_read)
{
    if (!filename || !buf || buf_len == 0 || !bytes_read) {
        return STORAGE_ERR_PARAM;
    }
    *bytes_read = 0;
    if (!g_mounted) {
        return STORAGE_ERR_NO_CARD;
    }

#ifndef UNIT_TEST
    if (!SD.exists(filename)) {
        return STORAGE_ERR_NOT_FOUND;
    }
    File f = SD.open(filename, FILE_READ);
    if (!f) {
        return STORAGE_ERR_IO;
    }
    if (offset > 0) {
        if (!f.seek(offset)) {
            f.close();
            return STORAGE_ERR_IO;
        }
    }
    size_t max_read = buf_len - 1;
    size_t n = f.read(reinterpret_cast<uint8_t *>(buf), max_read);
    f.close();
    buf[n] = '\0';
    *bytes_read = n;
#else
    size_t n = 0;
    storage_status_t st = mock_sd_read(filename, offset, buf, buf_len, &n);
    if (st != STORAGE_OK) {
        return st;
    }
    *bytes_read = n;
#endif

    return STORAGE_OK;
}

/* ---------------------------------------------------------------------------
 * storage_sd_file_size
 * --------------------------------------------------------------------------- */
uint32_t storage_sd_file_size(const char *filename)
{
    if (!filename || !g_mounted) {
        return 0;
    }

#ifndef UNIT_TEST
    if (!SD.exists(filename)) {
        return 0;
    }
    File f = SD.open(filename, FILE_READ);
    if (!f) {
        return 0;
    }
    uint32_t sz = static_cast<uint32_t>(f.size());
    f.close();
    return sz;
#else
    return mock_sd_file_size(filename);
#endif
}

/* ---------------------------------------------------------------------------
 * storage_sd_free_bytes
 * --------------------------------------------------------------------------- */
uint64_t storage_sd_free_bytes(void)
{
    if (!g_mounted) {
        return 0;
    }

#ifndef UNIT_TEST
    uint64_t total = SD.totalBytes();
    uint64_t used  = SD.usedBytes();
    if (used > total) {
        return 0;
    }
    return total - used;
#else
    return mock_sd_free_bytes();
#endif
}

/* ---------------------------------------------------------------------------
 * storage_sd_total_bytes
 * --------------------------------------------------------------------------- */
uint64_t storage_sd_total_bytes(void)
{
    if (!g_mounted) {
        return 0;
    }

#ifndef UNIT_TEST
    return SD.totalBytes();
#else
    return mock_sd_free_bytes(); /* mock has no separate total; return free as proxy */
#endif
}

/* ---------------------------------------------------------------------------
 * storage_sd_unmount
 * --------------------------------------------------------------------------- */
void storage_sd_unmount(void)
{
    if (!g_mounted) {
        return;
    }
    g_mounted = false;
#ifndef UNIT_TEST
    SD.end();
#endif
}

/* ---------------------------------------------------------------------------
 * storage_sd_list_csv
 * --------------------------------------------------------------------------- */
storage_status_t storage_sd_list_csv(const char *ext, char *buf, size_t buf_len)
{
    if (!ext || !buf || buf_len == 0) {
        return STORAGE_ERR_PARAM;
    }
    buf[0] = '\0';
    if (!g_mounted) {
        return STORAGE_ERR_NO_CARD;
    }

#ifndef UNIT_TEST
    File root = SD.open("/");
    if (!root) {
        return STORAGE_ERR_IO;
    }
    size_t ext_len = strlen(ext);
    size_t pos = 0;
    while (true) {
        File entry = root.openNextFile();
        if (!entry) {
            break;
        }
        if (!entry.isDirectory()) {
            const char *name = entry.name();
            size_t name_len = strlen(name);
            if (name_len >= ext_len &&
                strcmp(name + name_len - ext_len, ext) == 0) {
                /* Append "name," if it fits */
                size_t needed = name_len + 1; /* +1 for comma */
                if (pos + needed < buf_len) {
                    memcpy(buf + pos, name, name_len);
                    pos += name_len;
                    buf[pos++] = ',';
                    buf[pos]   = '\0';
                }
            }
        }
        entry.close();
    }
    root.close();
#else
    mock_sd_list_csv(ext, buf, buf_len);
#endif

    return STORAGE_OK;
}

/* ---------------------------------------------------------------------------
 * storage_sd_delete
 * --------------------------------------------------------------------------- */
storage_status_t storage_sd_delete(const char *filename)
{
    if (!filename) {
        return STORAGE_ERR_PARAM;
    }
    if (!g_mounted) {
        return STORAGE_ERR_NO_CARD;
    }

#ifndef UNIT_TEST
    if (!SD.exists(filename)) {
        return STORAGE_ERR_NOT_FOUND;
    }
    if (!SD.remove(filename)) {
        return STORAGE_ERR_IO;
    }
#else
    return mock_sd_delete(filename);
#endif

    return STORAGE_OK;
}
