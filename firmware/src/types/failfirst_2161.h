/**
 * @file failfirst_2161.h
 * @brief 2.16.1's fail-first build (gh#89): restore T13's old format rule.
 *
 * `-DOTA_FAILFIRST_2161=1` makes T13 format the inactive LittleFS on ANY mount
 * failure again, as before 2.16.1, so the `noformat` stage of bin/at_t13_vfs.py
 * can be shown to FAIL (the inactive partition's GUI wiped) before its pass
 * means anything:
 *
 *    1  T13 formats on any mount failure, not only on a refusal of the
 *       partition's contents
 *
 * The other half of gh#89, CONFIG_VFS_MAX_COUNT, is build configuration, not a
 * code path: its fail-first is the 2.16.0 image itself, under the same tests.
 *
 * **Pass a value.** GCC defines a bare `-DOTA_FAILFIRST_2161` as 1, which is
 * the one bit there is.
 *
 * Bench builds only: the source refuses the flag otherwise, and
 * GET /api/diag/ota reports `failfirst_2161` so a result can never be read
 * against the wrong build.
 */
#pragma once

#if defined(OTA_FAILFIRST_2161) && !defined(MODBUS_BENCH)
#error "OTA_FAILFIRST_2161 is a bench-only fail-first build"
#endif

#ifdef OTA_FAILFIRST_2161
#  define FF2161 ((unsigned)(OTA_FAILFIRST_2161 + 0))
/* A fail-first build that removes NOTHING passes every test, which is the one
 * result such a build must never be able to give. */
#  if (OTA_FAILFIRST_2161 + 0) == 0
#    error "OTA_FAILFIRST_2161 is defined but removes nothing: pass =1"
#  endif
#  if (OTA_FAILFIRST_2161 + 0) > 1
#    error "OTA_FAILFIRST_2161 has bits that remove nothing: =1"
#  endif
#else
#  define FF2161 0u
#endif

#define FF2161_FORMAT_ANY (FF2161 & 1u)
