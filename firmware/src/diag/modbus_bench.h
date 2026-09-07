/**
 * @file modbus_bench.h
 * @brief Arbitrary Modbus access for bench commissioning — DEV BUILDS ONLY.
 *
 * Phase 0 of design/integrateWindowPositionSensor.md needs to talk to a device
 * the firmware knows nothing about: the wire-encoder position sensor at address
 * 40 or 45. Read its identification, dump its holding registers, drive a teach,
 * read back the captured calibration points, and run the two installation
 * checks. None of that is possible through the product API, which only ever
 * addresses the FG6485A (1) and the S200 (44).
 *
 * The plan called Phase 0 "no firmware — bench tooling only". That was wrong:
 * **no bench tooling existed.** The alternatives were a USB-RS485 adapter
 * physically on the bus, or this. This wins because it works over the network
 * and is reusable for Phase 1 driver bring-up.
 *
 * ## Safety
 *
 * - Compiled in **only** when `MODBUS_BENCH` is defined; never in a release.
 * - The HTTP route is **admin-only**.
 * - **Writes are restricted to addresses 40 and 45** (@ref MODBUS_BENCH_WRITE_ADDR_A
 *   / `_B`) — the position sensor's two possible addresses. Reads may target any
 *   address, since a read cannot change anything, but an unrestricted write
 *   could reconfigure the FG6485A or S200 and silently corrupt climate control.
 * - Single-register writes only (FC16 with quantity 1). Everything Phase 0 needs
 *   — arm/abort teach, set `40002`/`40003`/`40004` — is one register at a time,
 *   and a range write has no use here beyond widening the blast radius.
 *
 * ## Bus safety
 *
 * Calls go through the ordinary driver, so they take the bus mutex (gh#49,
 * shipped 2.4.1) and cannot corrupt a concurrent T5 poll. Before that release
 * this tool would have been unsafe to write at all.
 *
 * @author Greenhouse Controller project
 * @see design/integrateWindowPositionSensor.md §5 Phase 0
 */

#ifndef MODBUS_BENCH_H
#define MODBUS_BENCH_H

#ifdef MODBUS_BENCH

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Position sensor address, solder jumper open (contract §2). */
#define MODBUS_BENCH_WRITE_ADDR_A  40u
/** @brief Position sensor address, solder jumper bridged. */
#define MODBUS_BENCH_WRITE_ADDR_B  45u

/** @brief Largest register count a single bench read may request. */
#define MODBUS_BENCH_MAX_REGS      32u

/**
 * @brief Execute one bench Modbus transaction.
 *
 * @param addr      Slave address (1–247).
 * @param fc        3 = read holding, 4 = read input, 16 = write one register.
 * @param reg       Start register (raw PDU address, i.e. 30001 → 0).
 * @param arg       FC03/FC04: register count. FC16: the value to write.
 * @param out       Buffer for read results; ignored for FC16.
 * @param out_cap   Capacity of @p out in uint16_t.
 * @param n_out     Receives the number of registers returned (0 for a write).
 * @param err       Receives a static human-readable status string, always set.
 *
 * @return true on success, false otherwise — inspect @p err for why.
 *
 * @note Refuses a write to any address other than
 *       @ref MODBUS_BENCH_WRITE_ADDR_A / `_B`; see the safety note above.
 */
bool modbus_bench_exec(uint8_t   addr,
                       uint8_t   fc,
                       uint16_t  reg,
                       uint16_t  arg,
                       uint16_t *out,
                       size_t    out_cap,
                       size_t   *n_out,
                       const char **err);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_BENCH */
#endif /* MODBUS_BENCH_H */
