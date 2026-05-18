/**
 * @file tz_table.h
 * @brief IANA → POSIX timezone string table for T10's IP-geolocation sync.
 *
 * Ported verbatim from the 1.20.3 archived `network_manager.cpp` so that
 * the alpha.6.28 geo sync behaviour matches production for every zone the
 * original supported. Coverage: Europe (full incl. CET/CEST/WET/EET),
 * Africa (major), Asia (full), Australia, Pacific, Americas.
 *
 * The table is a header (not a .cpp) so it stays static-const at compile
 * time — `iana_to_posix()` is the single consumer.
 *
 * Format reference: POSIX TZ strings as documented by IEEE Std 1003.1.
 * Each entry maps a key returned in ip-api.com's `timezone` field to the
 * corresponding TZ string suitable for `setenv("TZ", …)` + `tzset()`.
 *
 * Sentinel: terminator row with NULL.iana ends the table.
 */

#pragma once

#include <stddef.h>
#include <string.h>

struct tz_entry { const char *iana; const char *posix; };

static const struct tz_entry s_tz_table[] = {
    /* UTC */
    { "UTC",                              "UTC0" },
    { "Etc/UTC",                          "UTC0" },
    { "Etc/GMT",                          "UTC0" },
    /* Europe — CET/CEST (UTC+1/+2) */
    { "Europe/Amsterdam",                 "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Berlin",                    "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Brussels",                  "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Copenhagen",                "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Luxembourg",                "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Madrid",                    "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Malta",                     "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Oslo",                      "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Paris",                     "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Prague",                    "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Rome",                      "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Stockholm",                 "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Vienna",                    "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Warsaw",                    "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Zurich",                    "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Africa/Algiers",                   "CET-1" },
    { "Africa/Tunis",                     "CET-1" },
    /* Europe — WET/WEST (UTC+0/+1) */
    { "Europe/Lisbon",                    "WET0WEST,M3.5.0/1,M10.5.0" },
    { "Atlantic/Canary",                  "WET0WEST,M3.5.0/1,M10.5.0" },
    { "Atlantic/Madeira",                 "WET0WEST,M3.5.0/1,M10.5.0" },
    /* Europe — GMT/BST (UTC+0/+1) */
    { "Europe/London",                    "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Europe/Dublin",                    "IST-1GMT0,M10.5.0,M3.5.0/1" },
    /* Europe — EET/EEST (UTC+2/+3) */
    { "Europe/Athens",                    "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Bucharest",                 "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Helsinki",                  "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Kiev",                      "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Kyiv",                      "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Riga",                      "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Sofia",                     "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Tallinn",                   "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Vilnius",                   "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Asia/Nicosia",                     "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Asia/Famagusta",                   "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    /* Europe — no DST */
    { "Europe/Moscow",                    "MSK-3" },
    { "Europe/Minsk",                     "FET-3" },
    { "Europe/Istanbul",                  "TRT-3" },
    { "Asia/Istanbul",                    "TRT-3" },
    /* Africa */
    { "Africa/Cairo",                     "EET-2" },
    { "Africa/Johannesburg",              "SAST-2" },
    { "Africa/Harare",                    "CAT-2" },
    { "Africa/Nairobi",                   "EAT-3" },
    { "Africa/Addis_Ababa",               "EAT-3" },
    { "Africa/Lagos",                     "WAT-1" },
    { "Africa/Casablanca",                "WET0" },
    { "Africa/Abidjan",                   "GMT0" },
    { "Africa/Accra",                     "GMT0" },
    /* Asia — Middle East */
    { "Asia/Dubai",                       "GST-4" },
    { "Asia/Muscat",                      "GST-4" },
    { "Asia/Riyadh",                      "AST-3" },
    { "Asia/Baghdad",                     "AST-3" },
    { "Asia/Kuwait",                      "AST-3" },
    { "Asia/Beirut",                      "EET-2EEST,M3.5.0/0,M10.5.0/0" },
    { "Asia/Amman",                       "AST-3" },
    { "Asia/Jerusalem",                   "IST-2IDT,M3.4.4/26,M10.5.0" },
    { "Asia/Tehran",                      "IRST-3:30IRDT,80/0,264/0" },
    { "Asia/Kabul",                       "AFT-4:30" },
    /* Asia — South */
    { "Asia/Karachi",                     "PKT-5" },
    { "Asia/Kolkata",                     "IST-5:30" },
    { "Asia/Calcutta",                    "IST-5:30" },
    { "Asia/Colombo",                     "IST-5:30" },
    { "Asia/Kathmandu",                   "NPT-5:45" },
    { "Asia/Dhaka",                       "BDT-6" },
    { "Asia/Tashkent",                    "UZT-5" },
    { "Asia/Almaty",                      "ALMT-6" },
    /* Asia — SE */
    { "Asia/Bangkok",                     "ICT-7" },
    { "Asia/Ho_Chi_Minh",                 "ICT-7" },
    { "Asia/Phnom_Penh",                  "ICT-7" },
    { "Asia/Vientiane",                   "ICT-7" },
    { "Asia/Jakarta",                     "WIB-7" },
    { "Asia/Singapore",                   "SGT-8" },
    { "Asia/Kuala_Lumpur",                "MYT-8" },
    { "Asia/Manila",                      "PHT-8" },
    /* Asia — East */
    { "Asia/Shanghai",                    "CST-8" },
    { "Asia/Hong_Kong",                   "HKT-8" },
    { "Asia/Taipei",                      "CST-8" },
    { "Asia/Seoul",                       "KST-9" },
    { "Asia/Tokyo",                       "JST-9" },
    /* Australia */
    { "Australia/Perth",                  "AWST-8" },
    { "Australia/Darwin",                 "ACST-9:30" },
    { "Australia/Adelaide",               "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
    { "Australia/Brisbane",               "AEST-10" },
    { "Australia/Sydney",                 "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Australia/Melbourne",              "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Australia/Hobart",                 "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    /* Pacific */
    { "Pacific/Honolulu",                 "HST10" },
    { "Pacific/Auckland",                 "NZST-12NZDT,M9.5.0,M4.1.0/3" },
    { "Pacific/Fiji",                     "FJT-12" },
    { "Pacific/Guam",                     "ChST-10" },
    { "Pacific/Port_Moresby",             "PGT-10" },
    /* Americas */
    { "America/New_York",                 "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Detroit",                  "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Toronto",                  "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Indiana/Indianapolis",     "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Chicago",                  "CST6CDT,M3.2.0,M11.1.0" },
    { "America/Winnipeg",                 "CST6CDT,M3.2.0,M11.1.0" },
    { "America/Denver",                   "MST7MDT,M3.2.0,M11.1.0" },
    { "America/Edmonton",                 "MST7MDT,M3.2.0,M11.1.0" },
    { "America/Phoenix",                  "MST7" },
    { "America/Los_Angeles",              "PST8PDT,M3.2.0,M11.1.0" },
    { "America/Vancouver",                "PST8PDT,M3.2.0,M11.1.0" },
    { "America/Anchorage",                "AKST9AKDT,M3.2.0,M11.1.0" },
    { "America/Halifax",                  "AST4ADT,M3.2.0,M11.1.0" },
    { "America/Mexico_City",              "CST6CDT,M4.1.0,M10.5.0" },
    { "America/Bogota",                   "COT5" },
    { "America/Lima",                     "PET5" },
    { "America/Caracas",                  "VET4:30" },
    { "America/Santiago",                 "CLT4CLST,M10.2.6/24,M3.2.6/24" },
    { "America/Sao_Paulo",                "BRT3" },
    { "America/Argentina/Buenos_Aires",   "ART3" },
    { "America/Montevideo",               "UYT3" },
    { "America/Manaus",                   "AMT4" },
    { NULL, NULL }
};

static inline const char *iana_to_posix(const char *iana)
{
    for (size_t i = 0; s_tz_table[i].iana != NULL; i++) {
        if (strcmp(s_tz_table[i].iana, iana) == 0) {
            return s_tz_table[i].posix;
        }
    }
    return NULL;
}
