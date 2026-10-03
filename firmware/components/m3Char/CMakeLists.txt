# firmware/components/m3Char/CMakeLists.txt — IDF proxy for drivers/m3Char
#
# Plan §5e step 3 (2026-10-03): the characterisation run's arithmetic, wired in
# with its first consumer, window_pos/characterise.cpp. Deliberately absent in
# steps 1-2, as ventModel's proxy was before 2.12.0: an IDF component is
# compiled into every build, and until the run existed the library was a
# host-tested reference with no consumer.
#
# The library is arithmetic and nothing else: no ESP-IDF, no FreeRTOS, no
# project headers, by the same rule as ventModel. So this proxy has no REQUIRES
# at all, and the same source compiles here and in the host tests
# (`pio test -e native` in drivers/m3Char), which replay the rig's archived runs.

idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/m3Char/src/m3_char.cpp"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/m3Char/src"
)

# Tier-1 hardening flags scoped to this component only (gh#13 idiom), as
# ventModel's. The library was host-compiled clean with them and -Werror.
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
