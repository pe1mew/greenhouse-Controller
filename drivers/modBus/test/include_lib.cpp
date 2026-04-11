/**
 * @file include_lib.cpp
 * @brief Forces compilation of modbus_rtu.cpp in the native (host) test build.
 *
 * PlatformIO 6 does not automatically compile a library's src/ files when
 * running `pio test -e native` within the library's own directory.
 * Including the implementation here pulls it into the test translation unit
 * without changing the project structure or relying on a self-referencing
 * lib_deps entry (which triggers a Windows lock-file bug in PlatformIO 6).
 *
 * This file is only active in the native build (UNIT_TEST defined).
 * It must never be compiled for the target board — it is excluded from the
 * exported library by library.json's srcFilter (this directory is not src/).
 */

#ifdef UNIT_TEST
  #include "../src/modbus_rtu.cpp"
#endif
