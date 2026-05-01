/**
 * @file sunrise.h
 * @brief Sunrise/sunset calculation — NOAA General Solar Position Equations.
 *
 * Implements the NOAA simplified solar position algorithm (NOAA Solar
 * Calculator, General Solar Position Calculations, ESRL Global Monitoring
 * Laboratory). Accuracy: ±2 minutes for latitudes between 60°S and 60°N;
 * fully adequate for the Netherlands (≈52°N).
 *
 * ## Algorithm summary
 *   1. Compute the Julian Day (JD) from the Unix timestamp.
 *   2. Derive the Julian Century (T) from JD relative to J2000.0.
 *   3. Compute the Sun's geometric mean longitude (L0), mean anomaly (M),
 *      equation of center (C), true longitude (Θ), and apparent longitude (λ).
 *   4. Compute the Sun's declination (δ) and the Equation of Time (E).
 *   5. Compute the hour angle at sunrise (ω₀) using:
 *        cos(ω₀) = cos(90.833°) / (cos(lat) · cos(δ)) − tan(lat) · tan(δ)
 *      The 90.833° zenith angle accounts for atmospheric refraction (0.583°)
 *      and the solar disc radius (0.25°).
 *   6. Convert to UTC minutes from midnight:
 *        solar_noon_UTC = 720 − 4 · lon − E
 *        sunrise_UTC    = solar_noon_UTC − 4 · ω₀
 *        sunset_UTC     = solar_noon_UTC + 4 · ω₀
 *
 * ## Coordinate convention
 *   - Latitude:  positive = North, negative = South
 *   - Longitude: positive = East,  negative = West
 *   - Both supplied as decimal degrees (float).
 *
 * ## NVS storage format (TSDS §5.10 `system` namespace)
 *   lat_deg / lat_frac : split storage; recombine as lat_deg + lat_frac / 1000.0f
 *   lon_deg / lon_frac : same pattern
 *
 * ## Usage in T4 (Data Manager)
 *   Call sunrise_is_daytime() after every DS1307 RTC read to refresh
 *   is_daytime.  Call sunrise_calc() once per day (or on location change)
 *   to obtain the sunrise_mins_utc / sunset_mins_utc values exposed via T4's
 *   shared state for web GUI display (FR-DN04).
 *
 * @author Greenhouse Controller project
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * Result codes
 * --------------------------------------------------------------------------- */

typedef enum {
    SUNRISE_OK          =  0,  /**< Calculation succeeded; outputs are valid. */
    SUNRISE_POLAR_DAY   =  1,  /**< Sun never sets at this location/date (Arctic summer).
                                     is_daytime = true; rise=0, set=1439. */
    SUNRISE_POLAR_NIGHT =  2,  /**< Sun never rises at this location/date (Arctic winter).
                                     is_daytime = false; rise=0, set=0. */
    SUNRISE_ERR_PARAM   = -1,  /**< NULL pointer passed for output argument. */
} sunrise_result_t;

/* ---------------------------------------------------------------------------
 * API
 * --------------------------------------------------------------------------- */

/**
 * @brief Compute sunrise and sunset times as UTC minutes from midnight.
 *
 * The date used is derived from the UTC date embedded in @p unix_ts; the
 * time-of-day component of @p unix_ts is ignored for the astronomical
 * computation (sunrise/sunset depend only on the date, not the exact time).
 *
 * @param[in]  unix_ts        Unix timestamp (seconds since 1970-01-01 UTC).
 *                            Provides the calendar date for the calculation.
 * @param[in]  lat_deg        Latitude in decimal degrees, positive = North.
 * @param[in]  lon_deg        Longitude in decimal degrees, positive = East.
 * @param[out] rise_mins_utc  Sunrise: minutes from midnight UTC (0–1439).
 *                            Set to 0 on SUNRISE_POLAR_NIGHT.
 * @param[out] set_mins_utc   Sunset: minutes from midnight UTC (0–1439).
 *                            Set to 1439 on SUNRISE_POLAR_DAY,
 *                            set to 0 on SUNRISE_POLAR_NIGHT.
 * @return SUNRISE_OK, SUNRISE_POLAR_DAY, SUNRISE_POLAR_NIGHT, or
 *         SUNRISE_ERR_PARAM if either output pointer is NULL.
 */
sunrise_result_t sunrise_calc(int32_t unix_ts, float lat_deg, float lon_deg,
                               int32_t *rise_mins_utc, int32_t *set_mins_utc);

/**
 * @brief Determine whether a Unix timestamp falls within daytime.
 *
 * Calls sunrise_calc() internally and compares the UTC time-of-day encoded
 * in @p unix_ts against the computed sunrise and sunset windows.
 *
 * Returns true on SUNRISE_POLAR_DAY (sun never sets).
 * Returns false on SUNRISE_POLAR_NIGHT (sun never rises).
 * Falls back to true (daytime setpoints) if lat/lon are zero (FR-DN05:
 * no location configured — apply daytime setpoints as default).
 *
 * @param unix_ts   Unix timestamp (seconds since 1970-01-01 UTC).
 * @param lat_deg   Latitude in decimal degrees, positive = North.
 * @param lon_deg   Longitude in decimal degrees, positive = East.
 * @return true if daytime, false if night-time.
 */
bool sunrise_is_daytime(int32_t unix_ts, float lat_deg, float lon_deg);
