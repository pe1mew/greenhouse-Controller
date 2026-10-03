/**
 * @file failfirst_216.h
 * @brief 2.16.0's fail-first build: which of the characterisation's protections
 *        to remove (plan §5e, step 5).
 *
 * `-DWPOS_FAILFIRST_216=<mask>` removes one protection per bit, so each of
 * step 5's acceptance tests can be shown to FAIL on a build without it before
 * its pass means anything (bin/at_wp_char_accept.py):
 *
 *    1  the run's safety guards: it no longer ends on the wind override, the
 *       motor alarm, a recalibration, a sensor fault or both end sensors, so
 *       it carries on through each -- the harness must see that
 *    2  phase 4b, the band check: the run takes the candidate b0 as the band
 *       without checking a single correction, so on the rig (T17's first-read
 *       latency) short corrections land outside the band it derived
 *    4  ROTA's quiet gate asks only whether a TEACH runs, not a run, so an
 *       update offered during a run that outlives its session applies (and
 *       reboots the unit) in the middle of it
 *
 * **Pass a value.** GCC defines a bare `-DWPOS_FAILFIRST_216` as 1, which is
 * bit 1 alone. All three is `=7`.
 *
 * Bench builds only: the source refuses the flag otherwise, and
 * GET /api/diag/windowpos reports `gate.failfirst_216` so a result can never be
 * read against the wrong build.
 */
#pragma once

#if defined(WPOS_FAILFIRST_216) && !defined(MODBUS_BENCH)
#error "WPOS_FAILFIRST_216 is a bench-only fail-first build"
#endif

#ifdef WPOS_FAILFIRST_216
#  define FF216 ((unsigned)(WPOS_FAILFIRST_216 + 0))
/* A fail-first build that removes NOTHING passes every test, which is the one
 * result such a build must never be able to give. */
#  if (WPOS_FAILFIRST_216 + 0) == 0
#    error "WPOS_FAILFIRST_216 is defined but removes nothing: pass a mask, =1..7"
#  endif
#  if (WPOS_FAILFIRST_216 + 0) > 7
#    error "WPOS_FAILFIRST_216 has bits above 4 that remove nothing: =1..7"
#  endif
#else
#  define FF216 0u
#endif

#define FF216_GUARDS (FF216 & 1u)
#define FF216_NO4B   (FF216 & 2u)
#define FF216_ROTA   (FF216 & 4u)
