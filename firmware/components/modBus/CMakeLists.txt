# firmware/components/modBus/CMakeLists.txt — IDF proxy for drivers/modBus (LIB-6)
#
# Phase 2.6 (2.0.0-alpha.2.6) — sixth migrated driver and the second
# non-trivial migration after i2c (LIB-2). About ~120 lines of arduino
# Serial1 / micros / millis / delayMicroseconds replaced with ESP-IDF
# uart_driver_* / esp_timer_get_time / esp_rom_delay_us. CRC, frame
# parsing, RS-485 direction sequencing all framework-agnostic — unchanged.
#
# Same proxy pattern as the earlier driver components. Dependency chain:
#   modBus → gpio (LIB-1, for RS-485 direction control via gpio_util)
#         → ESP-IDF driver  (UART v1 API — `driver/uart.h`)
#         → ESP-IDF esp_timer (high-resolution microsecond clock)
#         → ESP-IDF esp_rom  (esp_rom_delay_us tight busy-wait)

idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/modBus/src/modbus_rtu.cpp"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/modBus/src"
        # pin_config.h supplies PIN_RS485_TX / PIN_RS485_RX macros.
        "${CMAKE_CURRENT_LIST_DIR}/../../config"
    REQUIRES
        # LIB-1 wrapper — gpio_rs485_init / gpio_set_rs485_direction.
        gpio
        # ESP-IDF UART driver (driver/uart.h: uart_driver_install, etc.).
        driver
        # esp_timer_get_time() for micros() / millis() equivalents.
        esp_timer
        # FreeRTOS for pdMS_TO_TICKS in uart_wait_tx_done timeout arg.
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
