# SD Card Driver — LIB-8

Driver for FAT32 file I/O over SPI on the ESP32-S3 (LOLIN S3). Provides append-only writes, offset reads, file management, and free-space queries for sensor data logging.

---

## API

| Function | Description |
|---|---|
| `storage_init()` | Initialize SPI bus and mount FAT32 file system |
| `storage_sd_available()` | Return true if a card is mounted |
| `storage_sd_write_append()` | Append a text line to a file (creates file if absent) |
| `storage_sd_read()` | Read bytes from a file at a given offset |
| `storage_sd_file_size()` | Return file size in bytes |
| `storage_sd_free_bytes()` | Return available storage in bytes |
| `storage_sd_list_csv()` | List files matching a given extension |
| `storage_sd_delete()` | Delete a file |

All functions return a `storage_status_t` status code: `STORAGE_OK`, `STORAGE_ERR_NO_CARD`, `STORAGE_ERR_MOUNT`, `STORAGE_ERR_IO`, `STORAGE_ERR_NOT_FOUND`, `STORAGE_ERR_FULL`, or `STORAGE_ERR_PARAM`.

---

## Build environments

Two PlatformIO environments are defined in [platformio.ini](platformio.ini):

| Environment | Target | Purpose |
|---|---|---|
| `lolin_s3` | ESP32-S3 hardware | Hardware integration tests |
| `native` | Host machine | Unit tests with mock SD backend |

---

## Unit tests

**Location:** [test/test_sd_storage.cpp](test/test_sd_storage.cpp)

Run on the host (no hardware required) using the Unity framework and an in-memory mock SD card. The mock stores files as `std::map<std::string, std::string>` and exposes controls to simulate card presence and free space.

**Run:**

```bash
~/.platformio/penv/Scripts/pio.exe test -e native
```

| Test ID | What is verified |
|---|---|
| UT-SD-001 | `storage_init()` returns `STORAGE_ERR_NO_CARD` when no card is present |
| UT-SD-002 | `storage_sd_available()` returns false after a failed init |
| UT-SD-003 | Writing to a non-existent file creates it with the correct content |
| UT-SD-004 | Successive appends preserve order; both lines are present |
| UT-SD-005 | Reading from offset 0 returns the full file content |
| UT-SD-006 | A non-zero offset correctly skips the preceding bytes |
| UT-SD-007 | `storage_sd_file_size()` returns the accurate byte count |
| UT-SD-008 | Querying the size of a non-existent file returns 0 without crashing |
| UT-SD-009 | Extension filter in `storage_sd_list_csv()` excludes non-matching files |
| UT-SD-010 | `storage_sd_delete()` removes the file (size becomes 0) |
| UT-SD-011 | Deleting a non-existent file returns `STORAGE_ERR_NOT_FOUND` |
| UT-SD-012 | Read into an undersized buffer is truncated and NUL-terminated |

---

## Hardware tests

**Location:** [src/main.cpp](src/main.cpp)

Run on an actual LOLIN S3 board with a FAT32-formatted SD card inserted. Tests execute sequentially and print `[PASS]` or `[FAIL]` over UART.

**Setup:**

1. Flash the `lolin_s3` environment to the board.
2. Insert a FAT32-formatted SD card.
3. Open a serial monitor at **115200 baud**.
4. Tests run automatically on boot and report results.

| Test ID | What is verified |
|---|---|
| HW-SD-001 | SPI bus initializes and card mounts (`storage_init()` returns `STORAGE_OK`) |
| HW-SD-002 | `storage_sd_free_bytes()` returns a value greater than 0 |
| HW-SD-003 | Writing to a non-existent file creates it |
| HW-SD-004 | A second write to the same file increases its size |
| HW-SD-005 | `storage_sd_read()` from offset 0 returns the written content |
| HW-SD-006 | The written `.csv` file appears in `storage_sd_list_csv()` results |
| HW-SD-007 | 1024 × 512-byte chunks (512 KB total) write without error |
| HW-SD-008 | `storage_sd_delete()` removes the file |
| HW-SD-009 | A second delete attempt returns `STORAGE_ERR_NOT_FOUND` |
| HW-SD-010 | Removing the card and resetting the board produces `STORAGE_ERR_NO_CARD` |

> HW-SD-010 requires manually removing the SD card before the board resets.
