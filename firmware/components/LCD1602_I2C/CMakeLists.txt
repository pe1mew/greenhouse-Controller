# firmware/components/LCD1602_I2C/CMakeLists.txt — IDF proxy for drivers/LCD1602_I2C (LIB-4)
#
# Phase 2.5 (2.0.0-alpha.2.5) — fifth migrated driver.
#
# Trivial migration: the driver was already 100% I2C-bus-bound (no direct
# hardware register access, no Arduino Wire calls — all bus traffic goes
# through LIB-2's i2c_bus wrappers). The only Arduino dependency was a
# single delay() call inside lcd_delay_ms(), now replaced with
# vTaskDelay(pdMS_TO_TICKS(ms)).
#
# Same proxy pattern as the earlier driver components. Dependency chain:
#   LCD1602_I2C → i2c (LIB-2, migrated in alpha.2.4)
# Indirectly the LCD driver also gets the IDF v5 transient-handle pattern
# from i2c_bus.cpp; the LCD doesn't know or care about i2c_master_*.

idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/LCD1602_I2C/src/lcd1602.cpp"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/LCD1602_I2C/src"
        # No firmware/config dep — lcd1602.h pulls only stdint.h.
    REQUIRES
        # LIB-2 wrapper — provides i2c_init / i2c_write / i2c_write_read.
        i2c
        # FreeRTOS for the vTaskDelay() in lcd_delay_ms().
        freertos
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
