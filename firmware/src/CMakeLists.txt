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
