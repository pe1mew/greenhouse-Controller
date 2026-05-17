# Greenhouse Controller — top-level CMakeLists.txt
#
# Required by PlatformIO + framework=espidf (introduced in v2.0.0-alpha.1
# during the ESP-IDF migration; see BRANCH_NOTES.md at repo root).
#
# This file is the entry point for the ESP-IDF CMake project. It pulls in
# the IDF's project.cmake glue, which then walks the directory tree looking
# for components (each component = a directory with its own CMakeLists.txt
# calling idf_component_register).
#
# The "main" component for this project lives at firmware/src/CMakeLists.txt.
# Other components (drivers/, etc.) will be added in later migration phases.

cmake_minimum_required(VERSION 3.16.0)

# Phase 2 (2.0.0-alpha.2): the ../drivers/ tree is registered as an extra
# components location. Each subdirectory that contains a `CMakeLists.txt`
# with `idf_component_register` is treated by IDF as a candidate component
# and is available to be REQUIREd from the main component (firmware/src/).
#
# Driver subdirectories that have NOT yet migrated to IDF-component shape
# in their CMakeLists are simply skipped by IDF — no harm, no foul.
# This list extends commit-by-commit as Phase 2 progresses through the
# 11 drivers (gpio → keypad_matrix → nvs → i2c_bus → lcd1602 →
# modbus_rtu → s200 → fg6485a → DS1307_RTC → littleFS → sdCard).
#
# Option A from the migration plan: drivers stay under ../drivers/ where
# they've always lived; they get IDF component scaffolding in-place. This
# preserves the standalone-test setup each driver has (its own .pio dir +
# library.json from the arduino era stay on disk, harmless under IDF).
# Phase 2 (2.0.0-alpha.2): the ../drivers/ tree is exposed to the IDF
# build via thin proxy components placed under the IDF-conventional
# firmware/components/<name>/ location. Each proxy CMakeLists.txt
# `idf_component_register`s the driver and references its source files
# at the absolute path under ../../drivers/<name>/src/.
#
# Why proxies instead of EXTRA_COMPONENT_DIRS:
# PlatformIO+espidf's post-configure introspection ("Reading CMake
# configuration...") fails with "Couldn't find the main target of the
# project!" when EXTRA_COMPONENT_DIRS is set to any non-empty value —
# reproduced bisecting 2026-05-17 on platform pin espressif32@6.12.0.
# Putting components under firmware/components/ uses IDF's automatic
# discovery (no EXTRA_COMPONENT_DIRS needed) and works around the bug.
#
# Why proxies instead of moving the drivers:
# Option A from the migration plan — "drivers stay under ../drivers/ where
# they've always lived" — is preserved. The driver source files don't
# move. The proxy CMakeLists is the only NEW artefact per driver. The
# standalone-test setups under each drivers/<name>/ (their own
# platformio.ini etc.) remain functional for native test builds.
#
# Proxies are added one per migration sub-step (Phase 2.1 = gpio, ...).

# Pull in IDF's project.cmake glue. Works both under `pio run` and a
# future `idf.py build`.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(greenhouse_controller)
