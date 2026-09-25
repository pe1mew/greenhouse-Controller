/**
 * @file failfirst_212.h
 * @brief 2.12.0's fail-first build: which of mode 2's rules to restore to broken.
 *
 * `-DWPOS_FAILFIRST_212=<mask>` restores the defects 2.12.0 fixed, ONE BIT EACH,
 * so every acceptance stage can be shown to FAIL before a pass means anything
 * (bin/at_wp_target.py, bin/at_wp_fallback.py). It is a bitmask rather than a
 * switch because the defects mask each other in layers (2026-09-20): with all
 * of them restored no target ever arms, so the rules behind the first one pass
 * VACUOUSLY -- and a fail-first arm that passes for the wrong reason certifies
 * the rule it never touched.
 *
 *    1  T2: the START of a targeted drive judges freshness by the STOP rule's
 *       3 s limit, so a target from a resting window is refused as stale
 *    2  T2: the stop rule has no grace for the first sample of a drive
 *    4  T2: the stop rule has no overshoot guard
 *    8  T2: a full-travel command does not disarm an armed target, so a safety
 *       close can be stopped short by a stale one -- the safety-relevant bit
 *   16  T6: the apply filter omits PART_OPEN, so after mode 2 leaves M3 part-open
 *       the mode-1 law's CLOSE or OPEN is dropped and M3 is STRANDED (found in
 *       the 2026-09-20 soak; bin/at_wp_fallback.py)
 *   32  T17: rule 1's at-end exemption is LATCHED from the first sample, so a
 *       drive that starts inside the target end's region but short of its
 *       switch reports a stall that is not one (the 2026-09-20 soak's
 *       `stall_faults` 1; bin/at_wp_rule1.py)
 *   64  T2: a targeted stop cuts the relay on ENTERING the band around the
 *       target, with no lead for the overrun, so every stop comes to rest ~2 %
 *       past it and AT-WP02 fails FR-WP05 (bin/at_wp02.py)
 *  128  T17: the read after a stroke is taken AT ONCE, while the leaf still
 *       coasts, and published for 30 s as where it rests (bin/at_wp02.py's
 *       `settle` check)
 *  256  T2: a T6 reversal that gh#48 DEFERS disarms the stroke's target first,
 *       so the stroke under way runs on to the end switch (found by the model
 *       session's simulator; bin/at_wp_target.py `deferred`)
 *  512  T6: never reports VENT_RES_ABORTED -- a window taken by safety, the
 *       operator or a recalibration reads as FAIL_TIMEOUT
 *       (bin/at_wp_fallback.py `aborted`)
 * 1024  T2/T6: "never moved" reads 0 (a law's "just moved"), and a
 *       recalibration sets no move time and arms the close dwell whatever the
 *       mode (bin/at_wp_fallback.py `sincemove`)
 * 2048  T6: an end target is judged by POSITION, so a close to 0 % that stops
 *       inside the arrival band counts as arrived -- mode 2 never finishes a
 *       close and leaves the window a deadzone ajar (gh#83, seen on 2344 the
 *       night of 2026-09-23: 20.8 mm short, all night;
 *       bin/at_wp_fallback.py `endstop`)
 *
 * 4096  T17: the control law is promoted ONLY at a stroke boundary, so mode 2
 *       never engages until something else happens to move M3 -- an operator
 *       who sets Linear and satisfies every precondition keeps timed control
 *       indefinitely (gh#86; bin/at_wp_rest_promote.py)
 *
 * **Pass a value.** GCC defines a bare `-DWPOS_FAILFIRST_212` as 1, so a bare
 * flag restores bit 1 ALONE. An earlier comment said a bare flag meant "all
 * four"; that was never true of the bitmask and was never exercised. All thirteen
 * is `=8191`.
 *
 * Bench builds only: the source refuses the flag otherwise, and
 * GET /api/diag/windowpos reports `gate.failfirst_212` so a result can never be
 * read against the wrong build.
 */
#pragma once

#if defined(WPOS_FAILFIRST_212) && !defined(MODBUS_BENCH)
#error "WPOS_FAILFIRST_212 is a bench-only fail-first build"
#endif

#ifdef WPOS_FAILFIRST_212
#  define FF212 ((unsigned)(WPOS_FAILFIRST_212 + 0))
/* A fail-first build that restores NOTHING passes every stage, which is the one
 * result such a build must never be able to give. T2's old local block mapped an
 * EMPTY definition (`-DWPOS_FAILFIRST_212=`) to all four bits; this header would
 * map it to 0 -- so refuse it, and refuse bits nobody has defined. */
#  if (WPOS_FAILFIRST_212 + 0) == 0
#    error "WPOS_FAILFIRST_212 is defined but restores nothing: pass a mask, =1..8191"
#  endif
#  if (WPOS_FAILFIRST_212 + 0) > 8191
#    error "WPOS_FAILFIRST_212 has bits above 4096 that restore nothing: =1..8191"
#  endif
#else
#  define FF212 0u
#endif

#define FF212_AGE       (FF212 & 1u)
#define FF212_GRACE     (FF212 & 2u)
#define FF212_OVERSHOOT (FF212 & 4u)
#define FF212_DISARM    (FF212 & 8u)
#define FF212_STRANDED  (FF212 & 16u)
#define FF212_R1_LATCH  (FF212 & 32u)
#define FF212_LEAD      (FF212 & 64u)
#define FF212_SETTLE    (FF212 & 128u)
#define FF212_DEFER_DISARM (FF212 & 256u)
#define FF212_ABORTED   (FF212 & 512u)
#define FF212_SINCE     (FF212 & 1024u)
#define FF212_ENDTARGET (FF212 & 2048u)
#define FF212_STROKEONLY (FF212 & 4096u)
