# firmware/components/littleFS/CMakeLists.txt — IDF proxy for drivers/littleFS (LIB-9)
#
# Phase 2.10 (2.0.0-alpha.2.10) — tenth migrated driver, first non-trivial rewrite
# since modBus (alpha.2.6). The arduino-esp32 LittleFS class (fs::LittleFSFS) is
# replaced with the IDF-native esp_vfs_littlefs API surface (from the
# managed component joltwallet/littlefs declared in idf_component.yml).
#
# Internal-flash dual-partition layout preserved:
#   LFS_PARTITION_A → partition label "lfs0" → mount /lfsa
#   LFS_PARTITION_B → partition label "lfs1" → mount /lfsb
# (See drivers/littleFS/src/littlefs_storage.cpp inline note — same mount-path
#  rule documented in MEMORY.md "LittleFS dual-partition basePath bug".)
#
# Dependencies:
#   joltwallet__littlefs   — esp_vfs_littlefs_register/_unregister/_format/_info
#   app_update             — esp_ota_get_running_partition for active-bank detection
#   esp_partition          — partition table lookup (pulled in transitively by app_update)
#   vfs                    — POSIX fopen/fread/fwrite/stat over the LittleFS mount

idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/littleFS/src/littlefs_storage.cpp"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/littleFS/src"
    REQUIRES
        # The managed component name is the slash-separated registry path with
        # the slash replaced by double-underscore. After first build it lives
        # at firmware/managed_components/joltwallet__littlefs/.
        joltwallet__littlefs
        # esp_ota_get_running_partition lives in app_update; pulls in
        # esp_partition transitively.
        app_update
        vfs
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
