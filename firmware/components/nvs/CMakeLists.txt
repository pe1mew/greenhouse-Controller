# firmware/components/nvs/CMakeLists.txt — IDF proxy for drivers/nvs (LIB-7)
#
# Phase 2.3 (2.0.0-alpha.2.3) — third migrated driver.
#
# Trivial migration: drivers/nvs was already 100% ESP-IDF native (raw nvs_*
# API, no Arduino.h in nvs_config.cpp/h since the start of the project — the
# only Arduino remnants live in drivers/nvs/src/main.cpp which is the
# standalone hardware-verification test, intentionally excluded from this
# component's SRCS just like the gpio and keyPad proxies exclude their
# test mains).
#
# Same proxy pattern as gpio + keyPad components. See those CMakeLists.txt
# files (or BRANCH_NOTES.md) for full rationale of why proxies instead of
# EXTRA_COMPONENT_DIRS.

idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/nvs/src/nvs_config.cpp"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/nvs/src"
        # No firmware/config dep — nvs_config.h doesn't include pin_config.h.
    REQUIRES
        # ESP-IDF NVS components: nvs_flash provides nvs_flash_init / _erase,
        # the underlying `nvs` library is pulled in transitively.
        nvs_flash
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
