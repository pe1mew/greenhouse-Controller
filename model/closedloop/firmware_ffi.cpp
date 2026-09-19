/**
 * @file firmware_ffi.cpp
 * @brief Firmware sources the simulator runs unchanged, with plain names for ctypes.
 *
 * sunrise.cpp is T4's day/night: the NOAA sun times from the site's
 * coordinates, and is_daytime, which picks the day or the night setpoints.
 * firmware.py compiles it here straight from firmware/src, so the simulator's
 * sun times cannot drift from the controller's. It is C++ without
 * extern "C", so these wrappers give ctypes names it can find.
 *
 * Built by firmware.py (ventmodel.build_dll); host-only, never part of the
 * firmware.
 */

#include <stdint.h>

#include "sunrise.h"

#if defined(_WIN32)
#  define FW_EXPORT extern "C" __declspec(dllexport)
#else
#  define FW_EXPORT extern "C" __attribute__((visibility("default")))
#endif

/** sunrise_calc(): UTC minutes from midnight; returns sunrise_result_t. */
FW_EXPORT int fw_sunrise_calc(int32_t unix_ts, float lat_deg, float lon_deg,
                              int32_t *rise_mins_utc, int32_t *set_mins_utc)
{
    return (int)sunrise_calc(unix_ts, lat_deg, lon_deg, rise_mins_utc, set_mins_utc);
}

/** sunrise_is_daytime(): what T4 writes to s_cfg.is_daytime. */
FW_EXPORT int fw_sunrise_is_daytime(int32_t unix_ts, float lat_deg, float lon_deg)
{
    return sunrise_is_daytime(unix_ts, lat_deg, lon_deg) ? 1 : 0;
}
