/**
 * @file modbus_probe.h
 * @brief Concurrency probe for the Modbus bus lock (gh#49) — DEV BUILDS ONLY.
 *
 * Verifies that `drivers/modBus`'s bus mutex actually serialises transactions,
 * by having two tasks poll the two **real** slaves on the RS-485 bus at once:
 *
 *   - FG6485A @ address 1  read with FC03 (as `fg6485a.cpp:61` does)
 *   - S200    @ address 44 read with FC04 (as `s200.cpp:78` does)
 *
 * ## Why this detects a stolen frame for free
 *
 * `modbus_rtu.cpp:391` validates the response address and function code, and
 * that check runs **only after the CRC check at `:381` has passed**.  A frame
 * that passes CRC is intact; an *intact* frame carrying the wrong address is
 * therefore not corruption but **another transaction's response**, handed to
 * the wrong caller out of the shared UART RX FIFO.
 *
 *     CRC-valid + wrong address  =  proof of a stolen frame  =  MODBUS_ERR_FRAMING
 *
 * The two callers differ in **both** fields that check tests (address *and*
 * function code), so the discrimination is doubled.
 *
 * ## Reading the tally
 *
 * | Status    | Meaning |
 * |-----------|---------|
 * | `FRAMING` | A stolen frame (CRC-valid, wrong address). Secondary: on this driver total failure shows as TIMEOUT, see below |
 * | `CRC`     | Frames interleaved on the wire (colliding DE/RE, or a violated t3.5 gap via `s_frame_end_us`) |
 * | `TIMEOUT` | A response was consumed by the other caller, or the slave was still in its inter-frame silence |
 * | `BUSY`    | Lock contention beyond `MODBUS_LOCK_TIMEOUT_MS`.  Should be zero |
 * | `OK`      | Clean transaction |
 *
 * **PASS = every iteration clean.**  Measured on FDA4 2026-09-07, the
 * unpatched driver produced **0/1000 clean, all timeouts, zero FRAMING and
 * zero CRC** — corruption is *total*, not partial: each transaction's 8-byte
 * echo drain eats the other caller's bytes, so no frame ever assembles and
 * there is nothing left to mis-address or fail a CRC.  **Never read "zero
 * FRAMING, zero CRC" as "no corruption"** — check the clean count first.
 * With the lock: 1000/1000 clean, every counter zero.
 *
 * ## Fail-first — not optional
 *
 * Run the probe **twice**: once against a build with the gh#49 mutex reverted,
 * and once with it.  The first run **must fail** — on FDA4 it reported 0/1000
 * clean, every transaction a timeout.  If the broken driver passes, the probe
 * is not exercising contention (most likely the two tasks are not
 * overlapping) and a green run against the fixed driver proves nothing.
 *
 * ## Where to run it
 *
 * **FDA4, the development unit** — sensors and SD fitted, no windows or motors
 * attached.  Never on 5C88 (production).  The probe suspends T5 while it runs
 * (see modbus_probe_run) so T5's read failures cannot raise sensor faults; on
 * FDA4 a wind fault would actuate nothing anyway, but a clean run is easier to
 * read without ALARM rows interleaved.
 *
 * Compiled in only when `MODBUS_CONCURRENCY_PROBE` is defined.  It is not part
 * of any release build.
 *
 * @author Greenhouse Controller project
 * @see design/addModbusMutex.md §4.3b
 */

#ifndef MODBUS_PROBE_H
#define MODBUS_PROBE_H

#ifdef MODBUS_CONCURRENCY_PROBE

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run the concurrency probe once, then log a verdict.
 *
 * Spawns two contending caller tasks pinned to the same core, waits for both,
 * prints a per-caller tally and an overall PASS/FAIL, and returns.  Blocks the
 * calling task for the duration (~30 s at the default iteration count).
 *
 * Suspends @c task_t5 for the duration and resumes it afterwards, so the
 * probe's bus saturation cannot drive T5's two-consecutive-failure sensor-fault
 * path while the measurement is running.
 *
 * @note Safe to call once at boot from app_main after T5 exists.  Not
 *       re-entrant; do not call concurrently with itself.
 */
void modbus_probe_run(void);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_CONCURRENCY_PROBE */
#endif /* MODBUS_PROBE_H */
