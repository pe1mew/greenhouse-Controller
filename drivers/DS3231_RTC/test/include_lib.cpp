/**
 * @file include_lib.cpp
 * @brief Forces compilation of ds1307_rtc.cpp in the native (host) test build.
 *
 * PlatformIO 6 does not automatically compile a library's src/ files when
 * running `pio test -e native` within the library's own directory.
 * Including the implementation here pulls it into the test translation unit
 * without changing the project structure or relying on a self-referencing
 * lib_deps entry (which triggers a Windows lock-file bug in PlatformIO 6).
 *
 * This file is listed in build_src_filter in platformio.ini (native env) and
 * must not be compiled in any other environment.
 */

#include "../src/ds1307_rtc.cpp"  /* NOLINT — intentional unity build for testing */
