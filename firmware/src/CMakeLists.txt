# Greenhouse Controller — "main" component CMakeLists.txt
#
# Phase 1 (2.0.0-alpha.1): minimal stub. Only `app_main_stub.cpp` is compiled.
# All other .cpp files under firmware/src/<subdir>/ remain on disk but are
# excluded from the build — they will be re-introduced as each phase
# migrates the corresponding subsystem.
#
# Phase progression (lines to be added back as each phase ships):
#   Phase 2 (alpha.2): driver headers via INCLUDE_DIRS additions
#   Phase 3 (alpha.3): "network_manager/network_manager.cpp"
#   Phase 4 (alpha.4): "status_post/status_post.cpp"
#                      "status_post_supervisor/status_post_supervisor.cpp"
#                      "status_post/status_json.cpp"
#   Phase 5 (alpha.5): "web_server/web_server.cpp" + web_routes_*.cpp
#   Phase 6 (alpha.6): main.cpp replaces app_main_stub.cpp; remaining
#                      subsystems get added (climate, relay, sensor, ui,
#                      keypad, data_manager, event_logger, ota_manager,
#                      auth, mqtt_client, safety_monitor, system_id)
#
# Until then, the "existing files in src/<subdir>/" are dormant. They will
# fail to compile against framework=espidf (Arduino.h not available) but
# CMake doesn't see them because they're not listed in SRCS below.

idf_component_register(
    SRCS
        "app_main_stub.cpp"
        "wifi_tickle.cpp"        # Phase 3 (2.0.0-alpha.3) — IDF-native STA + SNTP
        "https_tickle.cpp"       # Phase 4 (2.0.0-alpha.4) — IDF-native HTTPS client + esp_tls
        "web_server_tickle.cpp"  # Phase 5 (2.0.0-alpha.5) — IDF-native esp_http_server
        "system_globals.cpp"     # Phase 6.1 (2.0.0-alpha.6.1) — FreeRTOS infrastructure (Q*, MX*, EG1, task_t*)
        "data_manager/sunrise.cpp"  # Phase 6.2 (2.0.0-alpha.6.2) — NOAA solar position math (pure math, no deps)
        "system_id/system_id.cpp"   # Phase 6.3 (2.0.0-alpha.6.3) — per-unit ID from factory MAC (gh#17)
        "keypad_scan/keypad_scan.cpp" # Phase 6.4 (2.0.0-alpha.6.4) — T7 keypad scan task (LIB-5 + Q2)
        "event_logger/event_logger.cpp"         # Phase 6.6 (2.0.0-alpha.6.6) — T9 event logger task (Q3 → SD CSV)
        # data_manager_stub.cpp REMOVED in 2.0.0-alpha.6.7 — the real data_manager.cpp below provides dm_get_unix_time
        "data_manager/data_manager.cpp"         # Phase 6.7 (2.0.0-alpha.6.7) — T4 data manager (cfg_shadow + RTC + sunrise + Q4/Q6)
        # relay_controller_stub.cpp REMOVED in 2.0.0-alpha.6.9 — the real relay_controller.cpp below provides t2_get_window_states
        "relay_controller/relay_controller.cpp" # Phase 6.9 (2.0.0-alpha.6.9) — T2 relay controller (3 channels × OPEN/CLOSE + GPIO42 alarm ISR)
        "sensor_poll/sensor_poll.cpp"           # Phase 6.8 (2.0.0-alpha.6.8) — T5 sensor poll task (Modbus FG6485A+S200 → Q6)
    INCLUDE_DIRS
        "."
    REQUIRES
        # ESP-IDF components used by app_main_stub.cpp.
        # Extends as later phases pull in more subsystems.
        freertos
        log
        esp_system
        esp_hw_support
        heap
        nvs_flash     # used directly by app_main_stub for the pre-init NVS readback (alpha.2.3)
        # Phase 2 driver components (extends per migration sub-step).
        # Each REQUIRES entry corresponds to a proxy CMakeLists.txt
        # under firmware/components/<name>/ which forwards to the
        # actual driver source at ../../drivers/<name>/src/.
        gpio          # LIB-1 — migrated 2.0.0-alpha.2.1
        keyPad        # LIB-5 — migrated 2.0.0-alpha.2.2 (depends on gpio)
        nvs           # LIB-7 — migrated 2.0.0-alpha.2.3 (trivial — already raw nvs_*)
        i2c           # LIB-2 — migrated 2.0.0-alpha.2.4 (Wire.h → ESP-IDF v5 i2c_master_*)
        LCD1602_I2C   # LIB-4 — migrated 2.0.0-alpha.2.5 (delay()→vTaskDelay, depends on i2c)
        modBus        # LIB-6 — migrated 2.0.0-alpha.2.6 (Serial1 → uart_driver_*, depends on gpio)
        s200          # LIB-S200 — migrated 2.0.0-alpha.2.7 (trivial cleanup, depends on modBus)
        FG6485A       # LIB-FG — migrated 2.0.0-alpha.2.8 (trivial cleanup, depends on modBus)
        DS1307_RTC    # LIB-3 — migrated 2.0.0-alpha.2.9 (trivial cleanup, depends on i2c)
        littleFS      # LIB-9 — migrated 2.0.0-alpha.2.10 (LittleFS.h → esp_vfs_littlefs_*; managed component)
        sdCard        # LIB-8 — migrated 2.0.0-alpha.2.11 (SD.h → esp_vfs_fat_sdspi_*; FAT32 over SPI; LAST Phase-2 driver)
        # Phase 3 (2.0.0-alpha.3) — IDF-native WiFi tickle requirements.
        # Pulls in the WiFi + lwip stack (~200 KB).
        esp_wifi       # esp_wifi_init/_set_config/_start/_connect
        esp_event      # default event loop + WIFI_EVENT / IP_EVENT
        esp_netif      # esp_netif_create_default_wifi_sta, IPSTR/IP2STR
        lwip           # esp_netif_sntp_*, lwip/ip4_addr.h for IPSTR macros
        # Phase 4 (2.0.0-alpha.4) — IDF-native HTTPS client.
        # Pulls in esp_http_client + esp-tls (uses bundled mbedtls).
        esp_http_client    # esp_http_client_init/_perform/_cleanup + event API
        esp-tls            # transitive: TLS context for HTTP_TRANSPORT_OVER_SSL
        esp_timer          # esp_timer_get_time for elapsed-ms measurement
        # Phase 5 (2.0.0-alpha.5) — IDF-native web server.
        esp_http_server    # httpd_start/_stop/_register_uri_handler
)

# -----------------------------------------------------------------------------
# Tier-1 hardening flags (gh#13, since 1.17.29).
#
# Applied ONLY to this component's source files. ESP-IDF framework sources
# are NOT subjected to these flags because they have shadow-variable and
# stack-usage violations we don't want to patch. The original arduino-esp32
# build could safely put these in global build_flags because the framework
# was distributed as a precompiled library; under framework=espidf the
# IDF sources are compiled in-tree and we need component-scoped flags.
#
# _FORTIFY_SOURCE=2 from the original list is intentionally absent — it
# defines `memcpy` as a macro that collides with ESP-IDF's use of `memcpy`
# as a struct field name (esp_async_memcpy.c). See platformio.ini comment.
# -----------------------------------------------------------------------------
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
