/**
 * @file fmt_tenths.h
 * @brief Print a signed value held in tenths (x10) as a decimal: -5 -> "-0.5".
 *
 * The obvious way -- `v / 10` as the whole part, `|v| % 10` as the fraction --
 * LOSES THE SIGN between -0.9 and -0.1. C division truncates towards zero, so
 * `-5 / 10` is 0, and the minus sign lived only in the whole part. The status
 * payload and the sensor-history JSON both did exactly that until 2.14.0
 * (gh#88), sending -0.5 C as 0.5: on the public status site, the local GUI and
 * the history graph, and on the one kind of night a grower most wants to know
 * the greenhouse is below zero. The history code was a copy of the status
 * builder's, which is how one defect came to live in two places; both now use
 * this.
 *
 * Usage -- TENTHS_FMT inside the format, TENTHS_ARGS(v) in the argument list:
 * @code
 *   snprintf(buf, n, "\"temp_c\":" TENTHS_FMT, TENTHS_ARGS(t_c10));
 * @endcode
 * The sign travels as its own %s, and the magnitude is split with unsigned
 * arithmetic, so no division ever sees a negative number. Zero prints "0.0",
 * never "-0.0". Under the `format(printf)` attribute on a caller such as
 * status_json.cpp's append(), the compiler checks these types.
 *
 * `v` is evaluated more than once: pass a plain variable or field, never an
 * expression with side effects. Valid for every value an int32_t can hold.
 */
#pragma once

#include <stdint.h>

/** |v| as uint32_t; the int64_t detour makes INT32_MIN safe. */
#define TENTHS_ABS(v)   ((v) < 0 ? (uint32_t)(-(int64_t)(v)) : (uint32_t)(v))

/** printf conversion for one tenths value: sign, whole part, one decimal. */
#define TENTHS_FMT      "%s%lu.%lu"

/** The three arguments TENTHS_FMT consumes. */
#define TENTHS_ARGS(v)  (((v) < 0) ? "-" : ""), \
                        (unsigned long)(TENTHS_ABS(v) / 10u), \
                        (unsigned long)(TENTHS_ABS(v) % 10u)
