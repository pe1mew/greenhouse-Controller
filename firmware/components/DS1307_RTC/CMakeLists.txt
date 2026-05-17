# firmware/components/DS1307_RTC/CMakeLists.txt — IDF proxy for drivers/DS1307_RTC (LIB-3)
#
# Phase 2.9 (2.0.0-alpha.2.9) — ninth migrated driver. Trivial cleanup:
# drivers/DS1307_RTC/src/ds1307_rtc.cpp had a vestigial `#include <Arduino.h>`
# from the arduino-esp32 era but made no actual Arduino calls — body was
# already pure i2c_bus consumer. Migration cost: one #include line.
#
# Same proxy pattern as the earlier driver components. Dependency chain:
#   DS1307_RTC → i2c (LIB-2, migrated in alpha.2.4)
#
# Note the driver also has a UNIT_TEST branch that pulls in test/mock_i2c_bus.h
# for off-target unit tests. That path is not used in the firmware build
# (the IDF build never defines UNIT_TEST), so the proxy doesn't need any
# special handling — `i2c_bus.h` arrives via the i2c proxy's INCLUDE_DIRS.

idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/DS1307_RTC/src/ds1307_rtc.cpp"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/DS1307_RTC/src"
        # No firmware/config dep — ds1307_rtc.h doesn't reference pin_config.h
        # (the I2C address DS1307_I2C_ADDR=0x68 is hard-coded by the chip).
    REQUIRES
        # LIB-2 wrapper — i2c_write / i2c_write_read for the BCD-encoded
        # date/time registers (0x00–0x06) and the CH-bit probe in register 0x00.
        i2c
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
