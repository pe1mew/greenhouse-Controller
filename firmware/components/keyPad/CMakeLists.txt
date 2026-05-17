# firmware/components/keyPad/CMakeLists.txt — IDF proxy for drivers/keyPad (LIB-5)
#
# Phase 2.2 (2.0.0-alpha.2.2) — second migrated driver.
#
# Same proxy pattern as firmware/components/gpio/ (see that CMakeLists.txt
# for the full rationale of why proxies-here instead of EXTRA_COMPONENT_DIRS).
#
# Dependency chain:
#   keyPad → gpio (LIB-1, migrated in alpha.2.1)
# This component does NOT directly REQUIRES the ESP-IDF `driver` component
# because all gpio access goes through gpio_util's wrappers. Should gpio_util
# ever pull in more dependencies (e.g. ISR service in a later phase), this
# component picks them up transitively via REQUIRES gpio.

idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/keyPad/src/keypad_matrix.cpp"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/keyPad/src"
        # pin_config.h lives in firmware tree; the public header pulls it in
        # for KP_ROW1..4 / KP_COL1..4 macros.
        "${CMAKE_CURRENT_LIST_DIR}/../../config"
    REQUIRES
        # LIB-1 wrapper — provides gpio_set_pin_mode / gpio_write / gpio_read.
        gpio
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
