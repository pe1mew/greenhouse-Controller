# firmware/components/FG6485A/CMakeLists.txt — IDF proxy for drivers/FG6485A (LIB-FG)
#
# Phase 2.8 (2.0.0-alpha.2.8) — eighth migrated driver. Trivial cleanup:
# drivers/FG6485A/src/fg6485a.cpp had a vestigial `#include <Arduino.h>` from
# the arduino-esp32 era but made no actual Arduino calls — body was already
# pure FreeRTOS + modbus_rtu (LIB-6). Migration cost: one #include line.
#
# Same proxy pattern as the earlier driver components. Dependency chain:
#   FG6485A → modBus (LIB-6, migrated in alpha.2.6)
#           → FreeRTOS (for the optional fg6485a_task polling helper)

idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/FG6485A/src/fg6485a.cpp"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/FG6485A/src"
        # No firmware/config dep — fg6485a.h doesn't reference pin_config.h
        # (slave address comes from the caller, default FG6485A_DEFAULT_ADDR=1).
    REQUIRES
        # LIB-6 wrapper — modbus_read_holding_registers (FC03) for measurements,
        # modbus_write_multiple_registers (FC16) for alarm/correction writes.
        modBus
        # FreeRTOS for the fg6485a_task helper (xSemaphoreTake, vTaskDelay).
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
