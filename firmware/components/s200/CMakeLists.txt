# firmware/components/s200/CMakeLists.txt — IDF proxy for drivers/s200 (LIB-S200)
#
# Phase 2.7 (2.0.0-alpha.2.7) — seventh migrated driver. Trivial cleanup:
# drivers/s200/src/s200.cpp had a vestigial `#include <Arduino.h>` from
# the arduino-esp32 era but made no actual Arduino calls — body was already
# pure FreeRTOS + modbus_rtu (LIB-6). Migration cost: one #include line.
#
# Same proxy pattern as the earlier driver components. Dependency chain:
#   s200 → modBus (LIB-6, migrated in alpha.2.6)
#       → FreeRTOS (for the optional s200_task polling helper)

idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/s200/src/s200.cpp"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/s200/src"
        # No firmware/config dep — s200.h doesn't reference pin_config.h
        # (slave address comes from the caller, default S200_DEFAULT_ADDR = 44).
    REQUIRES
        # LIB-6 wrapper — modbus_read_input_registers for FC04 polls.
        modBus
        # FreeRTOS for the s200_task helper (xSemaphoreTake, vTaskDelay).
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
