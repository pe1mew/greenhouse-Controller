/**
 * @file include_lib.cpp
 * @brief Forces compilation of i2c_bus.cpp in the native (host) test build.
 *
 * PlatformIO 6 does not automatically compile a library's src/ files when
 * running `pio test -e native` within the library's own directory.
 * Including the implementation here pulls it into the test translation unit
 * without changing the project structure or relying on a self-referencing
 * lib_deps entry (which triggers a Windows lock-file bug in PlatformIO 6).
 *
 * **The ESP-IDF branch is the one compiled.** `pio test` always defines
 * UNIT_TEST, and in i2c_bus.cpp that selects stubs that return I2C_OK
 * without touching anything — a test of them tests nothing (that is how
 * UT-I2C-002…006 came to fail with "Was 0" once the driver moved to
 * ESP-IDF). UNIT_TEST is therefore undefined for this one include, and the
 * driver's real code runs against test/mock_idf/driver/i2c_master.h, whose
 * behaviour mock_i2c_master.cpp simulates. The driver file is unchanged.
 *
 * This file is listed in build_src_filter in platformio.ini (native env) and
 * must not be compiled in any other environment.
 */

#undef UNIT_TEST
#include "../src/i2c_bus.cpp"  /* NOLINT — intentional unity build for testing */
