# firmware/components/windowPos/CMakeLists.txt — IDF proxy for drivers/windowPos
#
# Phase 1 of design/integrateWindowPositionSensor.md — the wire-encoder window
# position sensor (contract v1.2, address 40 on this installation).
#
# Same proxy pattern as the other driver components. Dependency chain:
#   windowPos → modBus (LIB-6)
#
# No FreeRTOS requirement: unlike s200/FG6485A this driver ships no polling
# task helper. The position task is Phase 2 and lives in firmware/src, because
# it needs the derived configuration (travel_m3) that the driver has no
# business knowing about.

idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/windowPos/src/window_pos.cpp"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/windowPos/src"
        # No firmware/config dep — window_pos.h takes the slave address from the
        # caller (WINDOWPOS_DEFAULT_ADDR = 40) and references no pin_config.h.
    REQUIRES
        # LIB-6 wrapper — FC04 input reads, FC03 holdings, FC16 single writes.
        modBus
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
