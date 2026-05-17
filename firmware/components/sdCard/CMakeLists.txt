# firmware/components/sdCard/CMakeLists.txt — IDF proxy for drivers/sdCard (LIB-8)
#
# Phase 2.11 (2.0.0-alpha.2.11) — eleventh migrated driver, the LAST in
# Phase 2. The arduino-esp32 SD library + custom SPIClass instance is
# replaced with the IDF FAT/SDSPI stack:
#   - SPI host setup via spi_bus_initialize from driver/spi_common.h
#   - SD-over-SPI initialisation via SDSPI_HOST_DEFAULT() + SDSPI_DEVICE_CONFIG_DEFAULT()
#   - FAT32 mount via esp_vfs_fat_sdspi_mount() (mountpoint = "/sdcard")
#   - File I/O via standard POSIX fopen/fread/fwrite/stat/remove/opendir/readdir
#
# Unlike LIB-9 (littleFS, alpha.2.10) this driver needs NO managed component:
# fatfs and sdmmc are both bundled in ESP-IDF 5.5 out of the box.
#
# Dependencies:
#   fatfs            — esp_vfs_fat_sdspi_mount, esp_vfs_fat_sdcard_unmount
#   sdmmc            — sdmmc_card_t and SDSPI_HOST_DEFAULT macros
#   driver           — spi_bus_initialize / spi_bus_free / sdspi_host driver
#   vfs              — POSIX fopen/fwrite/opendir against the /sdcard mount
#   (transitively: esp_partition is pulled in by fatfs internals)
#
# Include dirs:
#   drivers/sdCard/src — the driver header (sd_storage.h)
#   firmware/config    — pin_config.h supplies PIN_SD_{MOSI,MISO,CLK,CS}
#                        which sd_storage.h aliases as SD_PIN_{MOSI,MISO,CLK,CS}.
#                        Note that sd_storage.h itself does a
#                        `#include "../../../firmware/config/pin_config.h"`
#                        relative-path include; the explicit INCLUDE_DIRS
#                        entry here is belt-and-braces so other code can
#                        include the header even if the relative path
#                        becomes brittle.

idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/sdCard/src/sd_storage.cpp"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/sdCard/src"
        "${CMAKE_CURRENT_LIST_DIR}/../../config"
    REQUIRES
        fatfs
        sdmmc
        driver
        vfs
)

# Tier-1 hardening flags scoped to this component only (gh#13 idiom).
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wextra
    -Wformat=2
    -Wshadow
    -Wstack-usage=2200
    -Wlogical-op
    -Wstrict-overflow=2
    -Wnull-dereference
)
