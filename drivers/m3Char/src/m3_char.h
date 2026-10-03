/**
 * @file m3_char.h
 * @brief The arithmetic of M3's characterisation run: pure functions, no I/O.
 *
 * Plan: `design/integrateWindowPositionSensor.md` §5e (decided and designed
 * 2026-10-03). The run itself -- commanding M3, the rest between starts, the
 * guards, the hold -- lives in the firmware (`window_pos/characterise.cpp`).
 * This library is what the run computes from what it measured:
 *
 * | Phase | Function |
 * |---|---|
 * | 1. Speed | m3c_speed() |
 * | 2. Reversal loss | m3c_mark_reversals(), m3c_reversal_loss() |
 * | 3. Minimum move | m3c_noise_x1000(), m3c_moved_threshold_x1000(), m3c_search_*(), m3c_minmove() |
 * | 4a. AT-WP02 | m3c_wp02() |
 * | 4b. The deadband | m3c_band_candidate(), m3c_check_step_x10(), m3c_band_check() |
 *
 * ## The one rule, as for drivers/ventModel
 *
 * **This header and its source compile with a plain C/C++ compiler: no
 * ESP-IDF, FreeRTOS or project headers, no `firmware/` include paths.** The
 * same source runs in the firmware and in the host tests
 * (`pio test -e native`), and the host tests feed it the rig's archived raw
 * data. So a limit the firmware owns, such as the deadzone setting's bounds,
 * is a parameter here, never a copy.
 *
 * ## Faithful to the harnesses, on purpose
 *
 * Phases 2-4a restate `bin/at_wp_minmove.py` and `bin/at_wp02.py`, which made
 * the measurements recorded in plan §3.6 and §0 item 4. The host tests replay
 * those runs' raw data and must reproduce the harnesses' printed figures, so a
 * difference between the on-unit run and the bench measurement is the
 * mechanism's, never the arithmetic's. Where the harness rule looks arbitrary
 * (a pulse "moved" when it moved MORE than the threshold; floor 2 is an
 * UNBROKEN run from the longest width), that is why it is kept.
 *
 * ## Units
 *
 * | Quantity | Unit | Matches |
 * |---|---|---|
 * | Opening, distance | 0.1 mm (`_x10`) | T17's `opening_mm_x10` |
 * | Aperture | 0.1 % (`_x10`) | T17's `percent_x10`, T2's targets |
 * | Statistics | 0.01 mm or 0.01 % (`_x100`); a threshold 0.001 mm (`_x1000`) | |
 * | Speed | 0.1 mm/s | the encoder's rate register `30012` |
 * | Pulse width | requested in ms; measured in us | T2's relay edges |
 *
 * Internally the statistics use double: they are a few hundred operations per
 * run, and the host tests compare against Python's doubles.
 */

#ifndef M3_CHAR_H
#define M3_CHAR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- constants (the harnesses' and the plan's, not tunables) ------------- */

/** A pulse never counts as "moved" for less than 1.0 mm, whatever the noise
 *  (`at_wp_minmove.py`: `max(3.0 * noise_mm, 1.0)`). 0.001 mm. */
#define M3C_MOVED_MIN_X1000      1000
/** AT-WP02 passes when ten resting positions span at most 2.0 % (the
 *  requirement's +/-1 %). 0.1 %. */
#define M3C_WP02_PASS_X10        20
/** The band check commands corrections of 1.25 x the band, and raises a band
 *  that fails by the same 25 % (plan §5e, the rule, steps 2-3). */
#define M3C_CHECK_FACTOR_PCT     125
/** Raises before the check gives up (plan §5e, the rule, step 3). */
#define M3C_CHECK_MAX_RAISES     4
/** Distinct pulse widths one direction of phase 3 may hold. */
#define M3C_MAX_WIDTHS           16
/** Readings one move of phase 1 may hold. Above it, m3c_speed() refuses. */
#define M3C_MAX_SAMPLES          512

/* ---- phase 1: speed ------------------------------------------------------- */

/** One reading T17 took during a move. */
typedef struct {
    uint32_t t_ms;       /**< when it was sampled, any monotonic ms clock */
    int32_t  pos_x10;    /**< opening, 0.1 mm */
    int32_t  rate_x10;   /**< the encoder's own rate (`30012`), 0.1 mm/s, signed */
} m3c_sample_t;

typedef struct {
    uint16_t speed_x10;  /**< median |rate| over the middle 60 % of the move, 0.1 mm/s */
    uint16_t read_ms;    /**< median interval between consecutive readings */
    uint16_t n_mid;      /**< readings the speed came from */
} m3c_speed_t;

/**
 * @brief The speed of one long targeted move, and T17's read interval in it.
 *
 * The speed is the encoder's own rate, not a difference of positions: two
 * readings are one or two device windows apart rather than one read interval
 * (relay_controller.cpp, `m3_ahead_x10()`). Only readings whose position lies
 * in the middle 60 % of the move count, which leaves out the start (not yet at
 * speed) and the cut (coasting).
 *
 * @param s        the move's readings, in the order taken
 * @param from_x10 where the move started, 0.1 mm
 * @param to_x10   where it was aimed, 0.1 mm; either direction
 * @return false with fewer than three readings in the middle, or a move that
 *         spans nothing; @p out is then zeroed.
 */
bool m3c_speed(const m3c_sample_t *s, size_t n, int32_t from_x10, int32_t to_x10,
               m3c_speed_t *out);

/* ---- phases 2 and 3: pulses ------------------------------------------------ */

/** One pulse and the displacement it produced. */
typedef struct {
    uint16_t req_ms;     /**< the width commanded */
    uint32_t width_us;   /**< the width at the relay's own edges; 0 = unknown */
    int32_t  disp_x10;   /**< displacement in the pulse's own direction, 0.1 mm */
    bool     opening;
    bool     takeup;     /**< an uncounted take-up pulse (phase 3 leaves it out) */
    bool     valid;      /**< the pulse completed and both readings exist */
    bool     reversal;   /**< set by m3c_mark_reversals() */
} m3c_pulse_t;

/**
 * @brief Tag each pulse that reversed the leaf's last direction of travel.
 *
 * The rope's slack is taken up first, so such a pulse measures the slack as
 * well as the actuator (`at_wp_minmove.py`, `mark_reversals()`). A pulse that
 * measured nothing reverses nothing.
 *
 * @param last_opening the direction of the drive before the first pulse
 */
void m3c_mark_reversals(m3c_pulse_t *p, size_t n, bool last_opening);

typedef struct {
    uint8_t n_rev;            /**< pulses after a reversal */
    uint8_t n_same;           /**< pulses in the same direction as the drive before */
    int32_t rev_mean_x100;    /**< their mean displacements, 0.01 mm */
    int32_t same_mean_x100;
    int32_t loss_x100;        /**< same - after a reversal: the travel the slack took, 0.01 mm */
} m3c_reversal_t;

/**
 * @brief The reversal loss in one direction, at one width.
 *
 * Every valid pulse of that direction and width counts, take-up pulses
 * included -- as in the harness, whose 200 ms take-ups are its reversal
 * samples. m3c_mark_reversals() must have run.
 *
 * @return false unless both kinds have at least one pulse. The counts, and the
 *         mean of a kind that has pulses, are filled in either way.
 */
bool m3c_reversal_loss(const m3c_pulse_t *p, size_t n, bool opening, uint16_t req_ms,
                       m3c_reversal_t *out);

/**
 * @brief The sample standard deviation of readings at rest, 0.001 mm.
 *
 * The caller chooses the readings. The harness leaves out the first since
 * 2026-10-02, because it can catch the leaf still settling (1.8 mm then).
 * Fewer than two readings give 0.
 */
uint32_t m3c_noise_x1000(const int32_t *pos_x10, size_t n);

/** @brief What a pulse must move MORE than to count as moved:
 *         max(3 x noise, 1.0 mm), 0.001 mm. */
uint32_t m3c_moved_threshold_x1000(uint32_t noise_x1000);

/** One width of a minimum-move table: the harness's printed row. */
typedef struct {
    uint16_t req_ms;
    uint8_t  n;               /**< pulses at this width */
    uint8_t  moved;           /**< of which moved more than the threshold */
    uint32_t real_us_mean;    /**< mean width at the relay */
    int32_t  disp_mean_x100;  /**< mean displacement, 0.01 mm */
    int32_t  disp_min_x10;
} m3c_width_row_t;

typedef struct {
    m3c_width_row_t row[M3C_MAX_WIDTHS];   /**< longest first */
    uint8_t  n_rows;
    uint16_t floor2_ms;           /**< floor 2, the width commanded; 0 = no width
                                   *   moved the leaf on every pulse */
    uint32_t floor2_real_us;
    int32_t  floor2_disp_x100;    /**< how far it moved the leaf, 0.01 mm */
    bool     fit_ok;              /**< at least three pulses and a positive slope */
    uint16_t fit_n;               /**< pulses in the fit */
    uint16_t fit_speed_x10;       /**< the line's slope, 0.1 mm/s */
    int16_t  dead_ms;             /**< where the line crosses zero */
} m3c_minmove_t;

/**
 * @brief Floor 2 and the dead time in one direction (plan §3.6).
 *
 * Same-direction pulses only: take-up pulses and pulses after a reversal are
 * left out, so m3c_mark_reversals() must have run. Floor 2 is the shortest
 * width in an UNBROKEN run from the longest at which every pulse moved more
 * than @p thresh_x1000: one width that failed ends the run, however a shorter
 * one then does. The line displacement = speed x (width - dead time) is fitted
 * by least squares through the pulses of that run, at their real widths.
 *
 * @return false with no qualifying pulse, or more than M3C_MAX_WIDTHS widths.
 */
bool m3c_minmove(const m3c_pulse_t *p, size_t n, bool opening, uint32_t thresh_x1000,
                 m3c_minmove_t *out);

/* ---- phase 3: which width to try next -------------------------------------- */

/**
 * The width search. It halves from the start width until a width has a pulse
 * that did not move, then tries the midpoint between the shortest width that
 * always moved and the longest that did not, twice. Its pulses then give
 * m3c_minmove() an unbroken run that ends at floor 2.
 */
typedef struct {
    uint16_t good_ms;    /**< the shortest width so far at which every pulse moved; 0 = none */
    uint16_t bad_ms;     /**< the longest width so far at which a pulse did not; 0 = none */
    uint8_t  refined;    /**< midpoints tried */
    uint16_t next_ms;    /**< the width to try next; 0 = the search is over */
} m3c_search_t;

/** @brief Start a search at @p w0_ms (plan §5e: 1 % of the window at the measured speed). */
void m3c_search_start(m3c_search_t *s, uint16_t w0_ms);

/**
 * @brief Record whether every pulse at `s->next_ms` moved, and choose the next width.
 *
 * The search ends when the start width itself failed (there is nothing above
 * it to refine towards), when halving would go below @p min_ms, or after two
 * midpoints.
 */
void m3c_search_result(m3c_search_t *s, bool all_moved, uint16_t min_ms);

/* ---- phase 4a: AT-WP02 ------------------------------------------------------ */

typedef struct {
    uint8_t n;
    uint8_t n_below;
    uint8_t n_above;
    int32_t mean_x100;    /**< mean resting aperture, 0.01 % */
    int32_t err_x100;     /**< mean minus the target */
    int32_t spread_x100;  /**< max minus min */
    int32_t hyst_x100;    /**< mean from below minus mean from above; 0 unless both exist */
    int32_t rms_x100;     /**< sqrt(mean((rest - target)^2)): the landing error */
    bool    pass;         /**< spread <= 2.0 % (M3C_WP02_PASS_X10) */
} m3c_wp02_t;

/**
 * @brief AT-WP02's figures from the resting positions (`bin/at_wp02.py`).
 *
 * @param rest_x10   each approach's resting aperture, 0.1 %, read live after the settle
 * @param from_below for each, whether it approached from below
 * @param target_x10 the target, 0.1 %
 * @return false with no approach.
 */
bool m3c_wp02(const int16_t *rest_x10, const bool *from_below, size_t n, int16_t target_x10,
              m3c_wp02_t *out);

/* ---- phase 4b: the deadband ------------------------------------------------- */

/** What the candidate is made of. Each input is a figure the run measured. */
typedef struct {
    uint16_t read_ms;          /**< T17's read interval during a stroke (phase 1) */
    uint16_t speed_x10[2];     /**< phase 1, [0] opening, [1] closing, 0.1 mm/s */
    int32_t  floor2_x100[2];   /**< phase 3's floor-2 displacements, 0.01 mm; 0 = none */
    int32_t  rms_x100;         /**< phase 4a's landing error, 0.01 % */
    int16_t  lead_x100[2];     /**< T2's learned leads after phase 4a, 0.01 % */
    uint16_t window_mm;        /**< the taught window */
} m3c_band_in_t;

/** Which term set the candidate. */
typedef enum {
    M3C_TERM_FLOOR1 = 1,
    M3C_TERM_FLOOR2,
    M3C_TERM_LANDING,
    M3C_TERM_LEAD,
} m3c_term_t;

typedef struct {
    int32_t    floor1_x100;    /**< read interval x the faster speed, 0.01 mm */
    int32_t    floor2_x100;    /**< the larger floor-2 displacement */
    int32_t    landing_x100;   /**< 3 x the landing error */
    int32_t    lead_x100;      /**< the larger learned lead */
    uint16_t   b0_mm;          /**< the largest, rounded up to whole mm */
    m3c_term_t which;
} m3c_band_cand_t;

/**
 * @brief The candidate band b0 (plan §5e, the rule, step 1).
 *
 * b0 = the largest of floor 1, floor 2, 3 x the landing error and T2's larger
 * learned lead, in mm, rounded up and at least @p min_mm. The lead is there
 * because a correction shorter than it is cut on its drive's FIRST reading
 * (`ch_start_target()`), which can come long after the relay closed.
 *
 * @param min_mm, max_mm the deadzone setting's bounds, from the firmware
 * @return false with no window, or a b0 above @p max_mm; @p out still says why.
 */
bool m3c_band_candidate(const m3c_band_in_t *in, uint16_t min_mm, uint16_t max_mm,
                        m3c_band_cand_t *out);

/**
 * @brief The size of one check correction, 0.1 % of the window, rounded up.
 *
 * 1.25 x the band. The run commands it from where the leaf RESTS, never from
 * the previous target, so it always exceeds the band T2 compares it with.
 * 0 with no window.
 */
uint16_t m3c_check_step_x10(uint16_t band_mm, uint16_t window_mm);

typedef enum {
    M3C_CHECK_PASS = 0,   /**< every correction landed within the band */
    M3C_CHECK_RAISE,      /**< one did not: check again at *next_mm */
    M3C_CHECK_GIVE_UP,    /**< raised M3C_CHECK_MAX_RAISES times, or past max_mm */
} m3c_check_t;

/**
 * @brief Judge one round of the band check (plan §5e, the rule, steps 2-4).
 *
 * @param land_err_x10  each correction's resting position minus its target, 0.1 mm
 * @param raises_done   rounds that already raised the band
 * @param next_mm       set on RAISE: the band for the next round
 * @param worst_x10     set always: the largest |landing error| of this round
 */
m3c_check_t m3c_band_check(uint16_t band_mm, const int32_t *land_err_x10, size_t n,
                           uint8_t raises_done, uint16_t max_mm,
                           uint16_t *next_mm, int32_t *worst_x10);

#ifdef __cplusplus
}
#endif

#endif /* M3_CHAR_H */
