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
 *
 * **Pass a value.** GCC defines a bare `-DWPOS_FAILFIRST_212` as 1, so a bare
 * flag restores bit 1 ALONE. An earlier comment said a bare flag meant "all
 * four"; that was never true of the bitmask and was never exercised. All six
 * is `=63`.
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
#    error "WPOS_FAILFIRST_212 is defined but restores nothing: pass a mask, =1..63"
#  endif
#  if (WPOS_FAILFIRST_212 + 0) > 63
#    error "WPOS_FAILFIRST_212 has bits above 32 that restore nothing: =1..63"
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
