/**
 * @file mock_i2c_bus.h
 * @brief Simulated I2C bus, AiP31068L and PCA9633 for the native (host) unit-test build of LIB-4.
 *
 * mock_i2c_bus.cpp implements the LIB-2 API that lcd1602.cpp calls —
 * against the REAL i2c_bus.h (found through `-I ../i2c/src`), so the types
 * cannot drift from the driver's — and answers from simulated hardware:
 *
 * **Time.** One clock, in nanoseconds, advanced by bus traffic and by
 * vTaskDelay() (mock_freertos.cpp). A write takes its bit time at
 * I2C_FREQ_HZ: START, 9 bits per byte including the address, STOP. A
 * zero-length probe runs at 100 kHz, as i2c_master_probe() does. Software
 * overhead counts as zero, so every gap the simulation shows is the least
 * the driver can rely on.
 *
 * **AiP31068L at 0x3E** (documentation/Sensors/lcd1602/AIP31068L.pdf).
 * Control byte Co/RS, then instruction or DDRAM/CGRAM data, decoded per the
 * datasheet's instruction set (Table 3). Each accepted byte keeps the chip
 * busy for its Table 3 execution time at fosc = 270 kHz: 1.53 ms for Clear
 * Display and Return Home, 39 µs for other instructions, 43 µs for data.
 * The serial interface cannot read the busy flag ("it must be waiting
 * enough execution time between the two instructions"), and a busy chip
 * "cannot accept" the next instruction: a byte that arrives early is
 * DROPPED and recorded as a violation. After power-on the chip is busy
 * resetting for 40 ms — the lcd1602.cpp figure; the datasheet flowchart says
 * 15 ms at 4.5 V, and HD44780-class parts need 40 ms at 2.7 V.
 *
 * **PCA9633 at 0x60.** Control byte = register pointer (bits 3..0) and
 * auto-increment option (bits 7..5); registers 00h-0Ch with their reset
 * values. Option 100b (all registers, rolling over to 00h) and 000b (none)
 * are modelled; any other option is counted and not stored.
 *
 * **Contract.** A write to an absent device returns I2C_ERR_NACK, as
 * i2c_bus.h promises for a NACK on "address or data". (The LIB-2 suite
 * shows the real driver returns I2C_ERR_BUS_BUSY for a data write on
 * ESP-IDF 5.5; this suite tests LIB-4 against the documented contract.)
 *
 * This header is also what lcd1602.cpp's own UNIT_TEST branch includes.
 *
 * @note Do **not** include this header in production (target) builds.
 *
 * @author Greenhouse Controller project
 * @version 0.2.0
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "i2c_bus.h"   /* the real LIB-2 header: this mock implements its API */

/* ---------------------------------------------------------------------------
 * Fixed hardware facts
 * --------------------------------------------------------------------------- */
#define MOCK_AIP_ADDR          0x3E   /**< AiP31068L, 8-bit 0x7C (module datasheet). */
#define MOCK_PCA_ADDR          0x60   /**< PCA9633DP2, 8-bit 0xC0 (module datasheet). */

#define MOCK_AIP_T_POWER_ON_NS  40000000ULL  /**< Busy after VDD rises. */
#define MOCK_AIP_T_SLOW_NS       1530000ULL  /**< Clear Display, Return Home. */
#define MOCK_AIP_T_INSTR_NS        39000ULL  /**< Every other instruction. */
#define MOCK_AIP_T_DATA_NS         43000ULL  /**< Write data to RAM. */

/** Instant with which vTaskDelay() outlasts its whole tick periods. */
#define MOCK_TICK_INSTANT_NS        1000ULL

/** mock_aip_violation_t::busy_with value while the power-on reset runs. */
#define MOCK_AIP_BUSY_POWER_ON  0xFFFF

#define MOCK_XFER_MAX_BYTES  16
#define MOCK_XFER_LOG_MAX   128
#define MOCK_VIOLATION_MAX   32

/* ---------------------------------------------------------------------------
 * Clock
 * --------------------------------------------------------------------------- */

/** @brief Simulated time since the last mock_lcd_reset() (power-on). */
uint64_t mock_now_ns(void);

/** @brief Let time pass, as the firmware does between two driver calls. */
void mock_advance_ns(uint64_t ns);

/** @brief mock_advance_ns() in milliseconds. */
void mock_advance_ms(uint32_t ms);

/** @brief vTaskDelay() calls since the last reset (mock_freertos.cpp). */
extern int mock_task_delay_calls;

/* ---------------------------------------------------------------------------
 * Hardware control
 * --------------------------------------------------------------------------- */

/**
 * @brief Power-cycle the module: both chips present, VDD rises at t = 0.
 *
 * Clears the clock, the bus log and the violation log. CGRAM comes up
 * filled with junk (0x5A): its power-on content is undefined.
 */
void mock_lcd_reset(void);

/** @brief Fit or remove a chip (MOCK_AIP_ADDR or MOCK_PCA_ADDR) without power-cycling. */
void mock_lcd_set_present(uint8_t addr, bool present);

/** @brief Clear the bus log and the violation log; the chips keep their state. */
void mock_lcd_clear_logs(void);

/* ---------------------------------------------------------------------------
 * Bus log
 * --------------------------------------------------------------------------- */

/** @brief One i2c_write() as it went out. A probe has len 0. */
typedef struct {
    uint64_t     t_start_ns;
    uint8_t      addr;
    uint8_t      data[MOCK_XFER_MAX_BYTES];
    uint8_t      len;
    i2c_status_t result;
} mock_xfer_t;

int                mock_xfer_count(void);
const mock_xfer_t *mock_xfer(int index);

/** @brief i2c_write() calls with len > 0 and a NULL buffer. */
extern int mock_i2c_misuse;

/* ---------------------------------------------------------------------------
 * AiP31068L
 * --------------------------------------------------------------------------- */

/** @brief A byte that reached the controller while it was still busy, and was lost. */
typedef struct {
    uint64_t t_ns;            /**< When the lost byte was latched. */
    uint8_t  byte;            /**< The lost instruction or data byte. */
    bool     is_data;         /**< RS = 1. */
    uint16_t busy_with;       /**< What was still executing: an instruction (00h-FFh),
                                   0x100 + a data byte, or MOCK_AIP_BUSY_POWER_ON. */
    uint64_t busy_since_ns;   /**< When that instruction was latched (0 for power-on). */
    uint64_t busy_until_ns;   /**< When the controller would have been ready. */
} mock_aip_violation_t;

int                         mock_aip_violation_count(void);
const mock_aip_violation_t *mock_aip_violation(int index);

/** @brief When the first instruction after power-on was accepted, or UINT64_MAX. */
uint64_t mock_aip_first_instruction_ns(void);

/** @brief Character at a visible position (row 0/1, col 0-15). */
char mock_aip_char_at(uint8_t row, uint8_t col);

/** @brief Raw DDRAM byte at address @p ddram_addr (0x00-0x27, 0x40-0x67). */
uint8_t mock_aip_ddram(uint8_t ddram_addr);

/** @brief The 8 pattern rows of CGRAM slot 0-7. */
const uint8_t *mock_aip_cgram(uint8_t slot);

/** @brief Address counter, and whether it points into CGRAM. */
uint8_t mock_aip_ac(void);
bool    mock_aip_ac_in_cgram(void);

bool mock_aip_display_on(void);
bool mock_aip_cursor_on(void);
bool mock_aip_blink_on(void);
bool mock_aip_two_line(void);    /**< Function set N. */
bool mock_aip_increment(void);   /**< Entry mode I/D. */
bool mock_aip_shift(void);       /**< Entry mode SH. */

/* ---------------------------------------------------------------------------
 * PCA9633
 * --------------------------------------------------------------------------- */

/** @brief Register 00h-0Ch. */
uint8_t mock_pca_reg(uint8_t reg);

/** @brief Writes with an auto-increment option the model does not cover. */
extern int mock_pca_unmodelled_writes;
