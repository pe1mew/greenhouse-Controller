# firmware/components/m3Hold/CMakeLists.txt — IDF proxy for drivers/m3Hold
#
# gh#93 (2.17.0): the LCD's tap-or-hold timing for M3's Open and Close keys,
# wired in with its one consumer, T8 (ui_display). design/lcdM3HoldControl.md.
#
# The library is timing logic and nothing else: no ESP-IDF, no FreeRTOS, no
# project headers, by the rule of ventModel and m3Char. So this proxy has no
# REQUIRES at all, and the same source compiles here and in the host tests
# (`pio test -e native` in drivers/m3Hold).

idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/m3Hold/src/m3_hold.cpp"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/m3Hold/src"
)

# Tier-1 hardening flags scoped to this component only (gh#13 idiom), as
# m3Char's. The library was host-compiled clean with -Wall -Wextra.
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
