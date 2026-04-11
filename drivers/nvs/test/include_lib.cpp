/**
 * @file include_lib.cpp
 * @brief Forces compilation of nvs_config.cpp in the native (host) test build.
 *
 * PlatformIO 6 does not automatically compile a library's src/ files when
 * running `pio test -e native` within the library's own directory.
 */

#ifdef UNIT_TEST
  #include "../src/nvs_config.cpp"
#endif
