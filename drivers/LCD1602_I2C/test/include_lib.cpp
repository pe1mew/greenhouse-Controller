/**
 * @file include_lib.cpp
 * @brief Forces compilation of lcd1602.cpp in the native (host) test build.
 *
 * PlatformIO 6 does not automatically compile a library's src/ files when
 * running `pio test -e native` within the library's own directory.
 * Including the implementation here pulls it into the test translation unit
 * without changing the project structure or relying on a self-referencing
 * lib_deps entry (which triggers a Windows lock-file bug in PlatformIO 6).
 *
 * **The ESP-IDF branch is the one compiled.** `pio test` always defines
 * UNIT_TEST, and in lcd1602.cpp that compiles every lcd_delay_ms() away.
 * The AiP31068L's serial interface cannot report busy, so those delays ARE
 * the protocol; a build without them cannot tell whether the driver waits.
 * UNIT_TEST is therefore undefined for this one include: the driver's own
 * vTaskDelay() calls reach test/mock_freertos.cpp, its i2c_write() calls
 * reach test/mock_i2c_bus.cpp, and the real i2c_bus.h comes from
 * ../i2c/src. The driver file is unchanged.
 *
 * This file is listed in build_src_filter in platformio.ini (native env) and
 * must not be compiled in any other environment.
 */

#undef UNIT_TEST
#include "../src/lcd1602.cpp"  /* NOLINT — intentional unity build for testing */
