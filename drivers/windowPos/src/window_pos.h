/**
 * @file window_pos.h
 * @brief Driver for the wire-encoder window position sensor (Modbus RTU).
 *
 * Thin layer over `modbus_rtu` (LIB-6). Implements the read side of the
 * device contract in `design/modbusInterfaceContractSpecification.md` v1.2,
 * plus the small write surface needed for commissioning.
 *
 * Phase 1 of `design/integrateWindowPositionSensor.md`. **This driver reads and
 * decodes; it holds no state and starts no task.** The polling task and the
 * derived configuration are Phase 2.
 *
 * ## Register map
 *
 * Offsets below are the contract's own hex column, NOT the 3xxxx/4xxxx labels.
 * Getting that wrong reads the neighbouring register and looks plausible.
 *
 * | FC | offset | contract | meaning |
 * |---|---|---|---|
 * | 04 | `0x0000` | 30001 | opening, instantaneous, 0.1 mm |
 * | 04 | `0x0001` | 30002 | opening, averaged over `40003` |
 * | 04 | `0x0004` | 30005 | raw ADC, pre-calibration, 0..1023 |
 * | 04 | `0x0005` | 30006 | status bits (see below) |
 * | 04 | `0x0006` | 30007 | ident: high byte build type, low byte version |
 * | 04 | `0x000B` | 30012 | movement rate, **SIGNED int16**, 0.1 mm/s |
 * | 04 | `0x000E` | 30015 | opening as 0.1 %, 0..1000 |
 * | 03 | `0x0001` | 40002 | measurement window, ms, 100..60000 |
 * | 03 | `0x0003` | 40004 | full travel between calibration points, 0.1 mm |
 * | 03 | `0x0006` | 40007 | teach command: 0 idle/abort, 1 arm |
 *
 * ## Three traps this driver exists to absorb
 *
 * 1. **`30012` is signed.** Read unsigned, a closing window reports ~65 000
 *    (contract FR-E10). @ref windowpos_reading_t::rate_mm_s_x10 is `int16_t`.
 * 2. **`65535` is a fault sentinel, not a position.** `30001`..`30004` and
 *    `30015` all use it. Treating it as a number puts the window at 6.5 m.
 * 3. **The device has no FC06.** Our Modbus layer has no FC06 either, so every
 *    single-register write here goes out as FC16 with quantity 1. `FR-MB28`
 *    rejects quantity 0 only, so this is legal.
 *
 * ## Two measured limits of the installation (rig, 2026-09-10)
 *
 * - **Position CLAMPS at `40004`.** Beyond the open end switch the leaf keeps
 *   moving into the blind overlap, but `30001` saturates. So position cannot
 *   distinguish *at the open switch* from *driven into the end protection*;
 *   both read 100 % with bit 3 set.
 * - **Bit 6 (implausible) is inert in the CLOSED direction** on this
 *   installation: the raw code sits at 0 at the closed stop, so a shorted
 *   wiper is indistinguishable from a genuinely closed window. See
 *   `integrateWindowPositionSensor.md` 2a.6 — the controller-side cross-check
 *   is the only defence and must actually be implemented.
 *
 * @see design/modbusInterfaceContractSpecification.md
 * @see design/integrateWindowPositionSensor.md
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>   /* NULL — the API documents NULL-tolerant out-params */
#include <stdint.h>

/** Factory address of the sensor on this installation (45 is the alternative). */
#define WINDOWPOS_DEFAULT_ADDR   40u

/** `30001`..`30004` and `30015` report this instead of a value when faulted. */
#define WINDOWPOS_FAULT_SENTINEL 65535u

/** Build type in the high byte of `30007`. */
#define WINDOWPOS_BUILD_RELEASE  0x01u
/** Bench build of the *sensor* — carries a deliberate hang hook. Never deploy. */
#define WINDOWPOS_BUILD_BENCH    0x81u

/** Status bits in `30006` (contract 3.3). */
#define WINDOWPOS_ST_STARTUP_WINDOW  0x0001u /**< no measurement window completed yet */
#define WINDOWPOS_ST_STARTUP_AVG     0x0002u /**< averaging accumulator not filled */
#define WINDOWPOS_ST_WIPER_OPEN      0x0004u /**< wiper open (FR-E07) */
#define WINDOWPOS_ST_END_SENSOR      0x0008u /**< an end sensor is active */
#define WINDOWPOS_ST_BOTH_ENDS       0x0010u /**< both active — distrust bit 3 (FR-E16) */
#define WINDOWPOS_ST_TEACH_ARMED     0x0020u /**< teach armed */
#define WINDOWPOS_ST_IMPLAUSIBLE     0x0040u /**< raw code outside the calibrated band */
#define WINDOWPOS_ST_NOT_FOLLOWING   0x0080u /**< switches saw movement, position did not */

/** Return codes. Mirrors the fg6485a/s200 convention. */
typedef enum {
    WINDOWPOS_OK        = 0, /**< Transaction completed. */
    WINDOWPOS_ERR_COMM  = 1, /**< Modbus timeout / CRC / exception / bus busy. */
    WINDOWPOS_ERR_PARAM = 2, /**< NULL pointer, addr 0, or out-of-range argument. */
} windowpos_status_t;

/**
 * @brief One coherent snapshot of the sensor.
 *
 * Taken in a single FC04 transaction, so the fields are consistent with each
 * other. Read the decoded booleans rather than re-deriving them from
 * @ref status_bits — the bit numbering is easy to get wrong and the meaning of
 * bit 3 in particular is subtle (contract 5.3).
 */
typedef struct {
    /* --- control surface ------------------------------------------------ */
    uint16_t opening_mm_x10;   /**< 30001, 0.1 mm. **Invalid if @ref sensor_fault.** */
    uint16_t percent_x10;      /**< 30015, 0.1 % (0..1000). Invalid if faulted. */
    int16_t  rate_mm_s_x10;    /**< 30012, 0.1 mm/s. **Signed**: + opening, − closing. */

    /* --- diagnostic ----------------------------------------------------- */
    uint16_t opening_avg_x10;  /**< 30002, averaged over `40003`. */
    uint16_t raw_adc;          /**< 30005, pre-calibration, 0..1023. */
    uint16_t status_bits;      /**< 30006, raw. Prefer the decoded flags below. */

    /* --- decoded status ------------------------------------------------- */
    bool starting_up;    /**< bit 0 or 1 — readings not yet meaningful. */
    bool wiper_fault;    /**< bit 2 — wiper open. */
    bool at_end_sensor;  /**< bit 3 — an end sensor is active. See the caveat below. */
    bool both_end_sensors; /**< bit 4 — distrust @ref at_end_sensor while set. */
    bool teach_armed;    /**< bit 5. */
    bool implausible;    /**< bit 6 — inert in the closed direction here, see the header. */
    bool not_following;  /**< bit 7 — mechanism fault: wire detached, slipping or seized. */

    /**
     * @brief True when the position fields must not be used.
     *
     * Set by the wiper-open bit **or** the `65535` sentinel. A caller that
     * checks only one of those will silently use a 6553.5 mm position.
     */
    bool sensor_fault;
} windowpos_reading_t;

/** Holding registers, as read back for commissioning. */
typedef struct {
    uint16_t zero_offset_x10;   /**< 40001 */
    uint16_t window_ms;         /**< 40002 */
    uint16_t averaging_s;       /**< 40003 */
    uint16_t full_travel_x10;   /**< 40004 — switch-to-switch, NOT stop-to-stop. */
    uint16_t raw_closed;        /**< 40005 */
    uint16_t raw_open;          /**< 40006 */
    uint16_t teach_cmd;         /**< 40007 — reads 0 after every device reset. */
} windowpos_config_t;

/**
 * @brief Read and decode one coherent snapshot.
 *
 * One FC04 transaction over `0x0000`..`0x000E`.
 *
 * @param slave_addr Modbus address (@ref WINDOWPOS_DEFAULT_ADDR).
 * @param out        Destination; untouched on error.
 * @return @ref WINDOWPOS_OK, or an error. **@ref WINDOWPOS_OK with
 *         @ref windowpos_reading_t::sensor_fault set is a successful read of a
 *         faulted sensor** — a distinct case from a failed read, and callers
 *         must treat them differently: one means the bus is broken, the other
 *         means the sensor is.
 */
windowpos_status_t windowpos_read(uint8_t slave_addr, windowpos_reading_t *out);

/**
 * @brief Read the identification register (`30007`).
 *
 * @param slave_addr Modbus address.
 * @param out_build  High byte: @ref WINDOWPOS_BUILD_RELEASE or
 *                   @ref WINDOWPOS_BUILD_BENCH. May be NULL.
 * @param out_ver    Low byte, firmware version. May be NULL.
 * @return @ref WINDOWPOS_OK or an error.
 * @warning Contract 9: **refuse to run against @ref WINDOWPOS_BUILD_BENCH.**
 *          That build carries a deliberate hang hook.
 */
windowpos_status_t windowpos_read_ident(uint8_t slave_addr,
                                        uint8_t *out_build,
                                        uint8_t *out_ver);

/**
 * @brief Read all seven holding registers.
 * @param slave_addr Modbus address.
 * @param out        Destination; untouched on error.
 * @return @ref WINDOWPOS_OK or an error.
 */
windowpos_status_t windowpos_read_config(uint8_t slave_addr,
                                         windowpos_config_t *out);

/**
 * @brief Set the measurement window (`40002`, ms).
 *
 * @param slave_addr Modbus address.
 * @param window_ms  100..60000.
 * @return @ref WINDOWPOS_OK, or @ref WINDOWPOS_ERR_PARAM if out of range.
 * @note **Set this before any teach.** The teach captures `30005` at the
 *       sensor transition, and `30005` only refreshes once per window — at the
 *       1000 ms default and rig speed a capture can be a full second stale,
 *       which is ~86 counts of silent calibration error.
 * @note Writing it restarts the averaging accumulator (status bit 1).
 */
windowpos_status_t windowpos_set_window_ms(uint8_t slave_addr, uint16_t window_ms);

/**
 * @brief Arm (`arm=true`) or abort (`arm=false`) the commanded teach (`40007`).
 *
 * Sent as FC16 quantity 1 — neither this driver nor the device's contract
 * assumes FC06 is available.
 *
 * @param slave_addr Modbus address.
 * @param arm        true to arm, false to abort.
 * @return @ref WINDOWPOS_OK or an error.
 * @warning Arming discards earlier captures. **Always teach with movement**
 *          (contract 6.2): the register a capture lands in is chosen by
 *          direction of travel, and the "hasn't moved since power-on" fallback
 *          is meaningless on a device still holding its factory calibration.
 */
windowpos_status_t windowpos_teach(uint8_t slave_addr, bool arm);

/**
 * @brief Read the two teach captures (`30013`, `30014`).
 *
 * @param slave_addr Modbus address.
 * @param out_closed Raw code captured at the closed end. May be NULL.
 * @param out_open   Raw code captured at the open end. May be NULL.
 * @return @ref WINDOWPOS_OK or an error.
 * @warning **This call has a side effect.** Contract 6.2 step d: once both
 *          captures exist and both have been read, the device commits them to
 *          `40005`/`40006`, persists, and clears the teach. Do not call this to
 *          "just look" during an armed teach — inspect them, then accept that
 *          the calibration is now written.
 */
windowpos_status_t windowpos_read_captures(uint8_t slave_addr,
                                           uint16_t *out_closed,
                                           uint16_t *out_open);
