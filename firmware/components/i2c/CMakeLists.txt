# firmware/components/i2c/CMakeLists.txt — IDF proxy for drivers/i2c (LIB-2)
#
# Phase 2.4 (2.0.0-alpha.2.4) — first non-trivial driver migration.
# Wire.h replaced with ESP-IDF v5's new `driver/i2c_master.h` API
# (i2c_master_bus_handle_t + per-device i2c_master_dev_handle_t).
# See drivers/i2c/src/i2c_bus.cpp header for the implementation strategy.
#
# Same proxy pattern as the earlier driver components. See gpio's
# CMakeLists.txt for the EXTRA_COMPONENT_DIRS-vs-proxy rationale.

idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/i2c/src/i2c_bus.cpp"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/i2c/src"
        # pin_config.h supplies PIN_I2C_SDA / PIN_I2C_SCL macros.
        "${CMAKE_CURRENT_LIST_DIR}/../../config"
    REQUIRES
        # ESP-IDF v5 I2C master driver. The legacy "driver" component still
        # provides i2c.h, but we want the v5 API exposed by "driver" — both
        # are part of the same `driver` component in IDF 5.x. Listing
        # `driver` covers both paths.
        driver
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
