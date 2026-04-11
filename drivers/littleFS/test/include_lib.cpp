/**
 * @file include_lib.cpp
 * @brief Forces compilation of littlefs_storage.cpp in the native test build.
 *
 * PlatformIO 6 does not automatically compile a library's src/ files when
 * running `pio test -e native` within the library's own directory.
 */

#ifdef UNIT_TEST
  #include "../src/littlefs_storage.cpp"
#endif
