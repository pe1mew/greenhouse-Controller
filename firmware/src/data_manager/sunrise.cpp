/**
 * @file sunrise.cpp
 * @brief NOAA General Solar Position sunrise/sunset calculation.
 *
 * See sunrise.h for the algorithm description, accuracy bounds, and the
 * coordinate convention. This translation unit only uses `<math.h>`; no
 * RTOS / ESP-IDF dependency, so the same code can be unit-tested on the
 * host without a stub layer.
 */

#include "sunrise.h"
#include <math.h>

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */

/** @brief Convert degrees to radians. */
static inline float deg2rad(float d) { return d * (float)M_PI / 180.0f; }

/** @brief Convert radians to degrees. */
static inline float rad2deg(float r) { return r * 180.0f / (float)M_PI; }

/** @brief Reduce an angle in degrees to the half-open range [0, 360). */
static inline float wrap360(float x)
{
    x = fmodf(x, 360.0f);
    return (x < 0.0f) ? x + 360.0f : x;
}

/**
 * @brief Compute the Julian Day Number for the UTC date in @p unix_ts.
 *
 * The time-of-day is stripped so the JD corresponds to noon on that date,
 * which is the convention used by the NOAA algorithm (JD is referenced to
 * the start of the Julian period, noon UT on 1 Jan 4713 BC).
 *
 * @param unix_ts  Unix timestamp (seconds since 1970-01-01 UTC).
 * @return         Julian Day Number at noon on that UTC date.
 */
static float julian_day(int32_t unix_ts)
{
    /* Strip time-of-day: keep only whole days. */
    int32_t unix_days = unix_ts / 86400;
    /* JD of Unix epoch (1970-01-01 00:00 UTC) = 2440587.5 */
    return (float)unix_days + 2440588.0f;   /* +0.5 for noon convention */
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

/**
 * @brief Compute sunrise and sunset for a date+location. See sunrise.h.
 *
 * Ten-step NOAA solar position pipeline:
 *  1. Julian Day (JD) and Julian Century (T) from the Unix timestamp.
 *  2. Geometric mean longitude L0.
 *  3. Geometric mean anomaly M.
 *  4. Equation of center C.
 *  5. True longitude θ and apparent longitude λ.
 *  6. Obliquity of the ecliptic ε.
 *  7. Solar declination δ.
 *  8. Equation of Time E.
 *  9. Hour angle ω₀ at sunrise (uses the 90.833° zenith — atmospheric
 *     refraction + solar disc radius).
 * 10. UTC minutes from midnight: rise = noon − 4·ω₀, set = noon + 4·ω₀.
 *
 * Detects polar day / polar night when cos(ω₀) falls outside [-1, 1].
 */
sunrise_result_t sunrise_calc(int32_t unix_ts, float lat_deg, float lon_deg,
                               int32_t *rise_mins_utc, int32_t *set_mins_utc)
{
    if (rise_mins_utc == NULL || set_mins_utc == NULL)
        return SUNRISE_ERR_PARAM;

    /* ---- Step 1: Julian quantities ---------------------------------------- */
    float JD = julian_day(unix_ts);
    /* Julian Century from J2000.0 */
    float T  = (JD - 2451545.0f) / 36525.0f;

    /* ---- Step 2: Geometric mean longitude L0 (degrees) -------------------- */
    float L0 = wrap360(280.46646f + T * (36000.76983f + T * 0.0003032f));

    /* ---- Step 3: Geometric mean anomaly M (degrees → radians) ------------- */
    float M_deg = 357.52911f + T * (35999.05029f - T * 0.0001537f);
    float M     = deg2rad(M_deg);

    /* ---- Step 4: Equation of center C (degrees) --------------------------- */
    float C = (1.914602f - T * (0.004817f + T * 0.000014f)) * sinf(M)
            + (0.019993f - T * 0.000101f)                    * sinf(2.0f * M)
            + 0.000289f                                       * sinf(3.0f * M);

    /* ---- Step 5: Sun's true and apparent longitude (degrees) -------------- */
    float theta  = L0 + C;                              /* true longitude */
    float omega  = 125.04f - 1934.136f * T;            /* Moon's ascending node */
    float lambda = theta - 0.00569f
                         - 0.00478f * sinf(deg2rad(omega)); /* apparent longitude */

    /* ---- Step 6: Obliquity of the ecliptic (degrees) ---------------------- */
    float eps0 = 23.0f
               + (26.0f
               + (21.448f - T * (46.8150f + T * (0.00059f - T * 0.001813f)))
               / 60.0f) / 60.0f;
    float eps  = eps0 + 0.00256f * cosf(deg2rad(omega));  /* corrected obliquity */

    /* ---- Step 7: Sun's declination δ (degrees) ---------------------------- */
    float decl = rad2deg(asinf(sinf(deg2rad(eps)) * sinf(deg2rad(lambda))));

    /* ---- Step 8: Equation of Time E (minutes) ----------------------------- */
    float e = 0.016708634f - T * (0.000042037f + T * 0.0000001267f); /* eccentricity */
    float y = tanf(deg2rad(eps / 2.0f));
    y       = y * y;                                      /* tan²(ε/2)  */
    float L0r = deg2rad(L0);
    float E = 4.0f * rad2deg(
          y       * sinf(2.0f * L0r)
        - 2.0f * e * sinf(M)
        + 4.0f * e * y * sinf(M) * cosf(2.0f * L0r)
        - 0.5f * y * y * sinf(4.0f * L0r)
        - 1.25f * e * e * sinf(2.0f * M)
    );

    /* ---- Step 9: Hour angle at sunrise ω₀ (degrees) ---------------------- */
    /*
     * cos(ω₀) = cos(90.833°) / (cos(lat) · cos(δ)) − tan(lat) · tan(δ)
     *
     * 90.833° = standard solar zenith at sunrise/sunset, accounting for
     * atmospheric refraction (0.583°) and solar disc radius (0.25°).
     */
    float cos_ha = cosf(deg2rad(90.833f))
                 / (cosf(deg2rad(lat_deg)) * cosf(deg2rad(decl)))
                 - tanf(deg2rad(lat_deg)) * tanf(deg2rad(decl));

    if (cos_ha < -1.0f) {
        /* Polar day — sun never sets */
        *rise_mins_utc = 0;
        *set_mins_utc  = 1439;
        return SUNRISE_POLAR_DAY;
    }
    if (cos_ha > 1.0f) {
        /* Polar night — sun never rises */
        *rise_mins_utc = 0;
        *set_mins_utc  = 0;
        return SUNRISE_POLAR_NIGHT;
    }

    float ha_deg = rad2deg(acosf(cos_ha));   /* hour angle in degrees */

    /* ---- Step 10: Solar noon and sunrise/sunset in UTC minutes ------------ */
    /*
     * Solar noon (UTC minutes from midnight):
     *   720 − 4·lon − E
     * Sunrise/sunset offset: ±4·ω₀ minutes (4 min per degree of hour angle).
     */
    float solar_noon = 720.0f - 4.0f * lon_deg - E;

    *rise_mins_utc = (int32_t)(solar_noon - 4.0f * ha_deg + 0.5f);
    *set_mins_utc  = (int32_t)(solar_noon + 4.0f * ha_deg + 0.5f);

    /* Clamp to valid day range (rounding can push slightly outside [0, 1439]). */
    if (*rise_mins_utc < 0)    *rise_mins_utc = 0;
    if (*rise_mins_utc > 1439) *rise_mins_utc = 1439;
    if (*set_mins_utc  < 0)    *set_mins_utc  = 0;
    if (*set_mins_utc  > 1439) *set_mins_utc  = 1439;

    return SUNRISE_OK;
}

/**
 * @brief Convenience wrapper: is the moment in @p unix_ts daytime? See sunrise.h.
 *
 * Calls sunrise_calc() then compares the UTC time-of-day extracted from
 * @p unix_ts against the [rise, set) window. Includes the FR-DN05
 * "no location configured" fallback (both lat and lon zero → return true,
 * i.e. apply daytime setpoints).
 */
bool sunrise_is_daytime(int32_t unix_ts, float lat_deg, float lon_deg)
{
    /* FR-DN05: no location configured (both zero) — default to daytime setpoints. */
    if (lat_deg == 0.0f && lon_deg == 0.0f)
        return true;

    int32_t rise, set;
    sunrise_result_t result = sunrise_calc(unix_ts, lat_deg, lon_deg, &rise, &set);

    if (result == SUNRISE_POLAR_DAY)   return true;
    if (result == SUNRISE_POLAR_NIGHT) return false;

    /* UTC minutes from midnight encoded in the Unix timestamp. */
    int32_t utc_mins = (int32_t)((unix_ts % 86400L) / 60L);
    if (utc_mins < 0) utc_mins += 1440;  /* handle negative modulo on some compilers */

    return (utc_mins >= rise && utc_mins < set);
}
