/**
 * @file m3char_record.h
 * @brief What the characterisation run leaves behind in NVS (plan §5e).
 *
 * One blob, `m3char`, in the motor namespace. Like the motor settings it
 * belongs to the installation, so an IO0 reset at stage 2 or 3 erases it with
 * them. It is written once at the end of every run, complete or aborted
 * (window_pos/characterise.cpp). T4 reads it at boot and on every reload of
 * the motor settings (data_manager.cpp, nvs_load_motor()).
 *
 * It has two parts:
 *  - **the last run**, whatever its outcome: what it measured, for the
 *    commissioning card;
 *  - **the measured band of the last COMPLETE run**, with its date and build.
 *    This is the band `deadzone_src_m3` = measured puts in force. An aborted
 *    run replaces the first part and carries this one over unchanged.
 *
 * Units follow drivers/m3Char (m3_char.h): 0.1 mm/s, 0.01 mm, 0.01 %, ms.
 *
 * **Versioned, and read whole or not at all.** A record whose version or
 * size differs from this build's is ignored, never half-read: a firmware that
 * acts on a field it does not understand is the gh#51 shape. So a change to
 * this layout bumps M3CHAR_REC_VERSION, and the first run after the update
 * replaces the old record.
 */

#ifndef M3CHAR_RECORD_H
#define M3CHAR_RECORD_H

#include <stdint.h>

#define M3CHAR_REC_KEY      "m3char"
#define M3CHAR_REC_VERSION  1u

/** m3char_rec_t::outcome of a record that no run wrote: the bench test hook
 *  (`POST /api/diag/windowpos {"meas_band_mm":N}`, bench builds only). The
 *  run's own outcomes (0 complete, else why it ended) arrive with it. */
#define M3CHAR_OUT_BENCH    0xFFu

/** Phases a run completed, as bits of m3char_rec_t::phases. */
#define M3CHAR_PH_SPEED     0x01u   /**< 1: speed and T17's read interval */
#define M3CHAR_PH_REVERSAL  0x02u   /**< 2: reversal loss */
#define M3CHAR_PH_MINMOVE   0x04u   /**< 3: floor 2 and the dead time */
#define M3CHAR_PH_WP02      0x08u   /**< 4a: AT-WP02 */
#define M3CHAR_PH_BAND      0x10u   /**< 4b: the band check passed */

typedef struct {
    /* ---- the record itself ---- */
    uint8_t  version;            /**< M3CHAR_REC_VERSION */
    uint8_t  outcome;            /**< 0 = complete; else why the run ended */
    uint8_t  phases;             /**< M3CHAR_PH_* it completed */
    uint8_t  check_rounds;       /**< phase 4b: rounds it ran */

    /* ---- the last run ---- */
    uint32_t when;               /**< when it ended, Unix seconds; 0 = clock not set */
    char     fw[16];             /**< the firmware that ran it */
    uint16_t travel_s;           /**< `travel_m3` then */
    uint16_t window_mm;          /**< the taught window then */
    uint16_t rest_s;             /**< the rest between motor starts it used */
    uint16_t starts;             /**< motor starts it made */
    uint16_t speed_x10[2];       /**< phase 1, [0] opening, [1] closing, 0.1 mm/s */
    uint16_t read_ms;            /**< phase 1: T17's read interval during a stroke */
    uint16_t floor2_ms[2];       /**< phase 3, 0 = none */
    int16_t  dead_ms[2];         /**< phase 3 */
    uint16_t b0_mm;              /**< phase 4b: the candidate band */
    int32_t  rev_loss_x100[2];   /**< phase 2, 0.01 mm */
    int32_t  floor2_x100[2];     /**< phase 3: floor 2's displacement, 0.01 mm */
    uint32_t noise_x1000;        /**< phase 3: the noise at rest, 0.001 mm */
    int16_t  wp02_err_x100;      /**< phase 4a: mean minus the target, 0.01 % */
    int16_t  wp02_spread_x100;
    int16_t  wp02_hyst_x100;
    int16_t  wp02_rms_x100;
    uint8_t  wp02_pass;          /**< phase 4a: the spread is within 2.0 % */
    uint8_t  b0_term;            /**< which term set b0 (m3c_term_t) */
    uint16_t band_mm;            /**< the band THIS run derived; 0 = none */
    int32_t  check_worst_x10;    /**< phase 4b: the last round's worst landing, 0.1 mm */

    /* ---- the measured band in force: the last COMPLETE run's ---- */
    uint16_t meas_band_mm;       /**< 0 = no complete run yet */
    uint16_t reserved;
    uint32_t meas_when;          /**< Unix seconds; 0 = clock not set */
    char     meas_fw[16];        /**< the firmware that measured it */
} m3char_rec_t;

#ifdef __cplusplus
static_assert(sizeof(m3char_rec_t) <= 256u, "the m3char record is meant to stay small");
#endif

#endif /* M3CHAR_RECORD_H */
