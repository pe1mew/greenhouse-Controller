/**
 * @file include_driver.cpp
 * @brief Forces compilation of modbus_rtu.cpp in the hardware loopback test build.
 *
 * PlatformIO 6 does not automatically compile a library's src/ files when
 * running `pio test` within the library's own project directory.  Including
 * the implementation here pulls it into the build without a self-referencing
 * lib_deps entry.
 *
 * This file is active only in hardware test builds (UNIT_TEST defined but
 * NATIVE_TEST NOT defined).  In the native (host) build the equivalent file
 * is test/test_modbus_rtu/include_lib.cpp.
 */

#if defined(UNIT_TEST) && !defined(NATIVE_TEST)
  #include "../../src/modbus_rtu.cpp"
#endif
