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

# Pull in PlatformIO's IDF integration helpers if they're present, otherwise
# fall back to the native IDF glue (lets the same CMakeLists.txt work both
# under `pio run` and a future `idf.py build`).
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(greenhouse_controller)
