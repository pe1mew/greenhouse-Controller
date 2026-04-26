/**
 * @file include_lib.cpp
 * @brief Forces compilation of s200.cpp in the native (host) test build.
 *
 * PlatformIO 6 does not automatically compile a library's src/ files when
 * running `pio test -e native` within the library's own directory.
 * Including the implementation here pulls it into the test translation unit
 * without changing the project structure or relying on a self-referencing
 * lib_deps entry.
 *
 * This file is only active when NATIVE_TEST is defined.
 * It must never be compiled for the target board — this directory is excluded
 * from the library by library.json's srcFilter.
 */

#ifdef NATIVE_TEST
  #include "../../src/s200.cpp"
#endif
