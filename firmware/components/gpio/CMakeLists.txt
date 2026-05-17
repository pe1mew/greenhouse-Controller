# firmware/components/gpio/CMakeLists.txt — IDF proxy for drivers/gpio (LIB-1)
#
# Phase 2.1 (2.0.0-alpha.2.1) — first migrated driver.
#
# This file is a THIN PROXY: it registers a standard IDF component named
# `gpio` that simply points at the actual driver source under
# ../../../drivers/gpio/src/. The migration plan's "Option A" intent is
# preserved — driver sources stay at their original location.
#
# Why a proxy instead of placing CMakeLists.txt inside drivers/gpio/ and
# using EXTRA_COMPONENT_DIRS: PlatformIO+espidf's post-configure
# introspection bug (verified 2026-05-17 on platform pin espressif32@6.12.0)
# rejects projects with non-empty EXTRA_COMPONENT_DIRS. The IDF-conventional
# `firmware/components/<name>/` location uses automatic discovery and
# works around the bug. See firmware/CMakeLists.txt for the full reasoning.
#
# A future v2.1.x could revisit this and either (a) move the driver
# physically into firmware/components/gpio/src/, or (b) switch to native
# idf.py (which is reported not to have the EXTRA_COMPONENT_DIRS bug) and
# revert to the in-place layout.

idf_component_register(
    SRCS
        # Driver source lives at its original location under ../drivers/.
        # CMAKE_CURRENT_LIST_DIR resolves to firmware/components/gpio/,
        # so ../../../drivers/gpio/src is the repo-root-relative path.
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/gpio/src/gpio_util.cpp"
    INCLUDE_DIRS
        # Header is exposed via the original driver location.
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/gpio/src"
        # pin_config.h lives in the firmware tree; the driver header
        # depends on it (pin assignments are project-specific).
        "${CMAKE_CURRENT_LIST_DIR}/../../config"
    REQUIRES
        # ESP-IDF GPIO driver (gpio_config_t, gpio_set_level, gpio_get_level).
        driver
)

# Tier-1 hardening flags scoped to this component only (gh#13 idiom).
# Same set as firmware/src/CMakeLists.txt: keeps ESP-IDF framework sources
# untouched while our own code stays under strict review.
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
