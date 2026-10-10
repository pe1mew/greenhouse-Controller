/**
 * @file mock_i2c_bus.cpp
 * @brief LIB-2 i2c_write() answered from a simulated AiP31068L and PCA9633 (LIB-4 native tests).
 *
 * See mock_i2c_bus.h for the model and its sources. Only i2c_write() is
 * implemented because it is the only LIB-2 call lcd1602.cpp makes; if the
 * driver starts using another, the link fails instead of a stub answering.
 */

#include "mock_i2c_bus.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Clock
 * --------------------------------------------------------------------------- */
static uint64_t s_now_ns;

uint64_t mock_now_ns(void)            { return s_now_ns; }
void     mock_advance_ns(uint64_t ns) { s_now_ns += ns; }
void     mock_advance_ms(uint32_t ms) { s_now_ns += (uint64_t)ms * 1000000ULL; }

/* ---------------------------------------------------------------------------
 * Bus log
 * --------------------------------------------------------------------------- */
static mock_xfer_t s_log[MOCK_XFER_LOG_MAX];
static int         s_log_count;
static mock_xfer_t s_log_overflow;
int                mock_i2c_misuse;

int mock_xfer_count(void)
{
    return s_log_count;
}

const mock_xfer_t *mock_xfer(int index)
{
    return (index >= 0 && index < s_log_count) ? &s_log[index] : NULL;
}

/* ---------------------------------------------------------------------------
 * AiP31068L
 * --------------------------------------------------------------------------- */
typedef struct {
    bool     present;
    uint64_t busy_until_ns;
    uint16_t busy_with;
    uint64_t busy_since_ns;
    uint64_t first_instruction_ns;
    uint8_t  ddram[0x80];
    uint8_t  cgram[64];
    uint8_t  ac;
    bool     ac_cgram;
    bool     display_on;
    bool     cursor_on;
    bool     blink_on;
    bool     eight_bit;
    bool     two_line;
    bool     big_font;
    bool     increment;
    bool     shift;
} aip_t;

static aip_t                s_aip;
static mock_aip_violation_t s_viol[MOCK_VIOLATION_MAX];
static int                  s_viol_count;

static void aip_power_on(void)
{
    memset(&s_aip, 0, sizeof(s_aip));
    s_aip.present = true;
    /* Datasheet 4.3: the power-on reset clears the display and sets DL=1,
     * N=0, F=0, display/cursor/blink off, I/D=1, SH=0, holding BF high. */
    memset(s_aip.ddram, 0x20, sizeof(s_aip.ddram));
    memset(s_aip.cgram, 0x5A, sizeof(s_aip.cgram));   /* undefined: junk */
    s_aip.eight_bit            = true;
    s_aip.increment            = true;
    s_aip.busy_with            = MOCK_AIP_BUSY_POWER_ON;
    s_aip.busy_since_ns        = 0;
    s_aip.busy_until_ns        = MOCK_AIP_T_POWER_ON_NS;
    s_aip.first_instruction_ns = UINT64_MAX;
}

static void aip_step_ac(bool forward)
{
    if (s_aip.ac_cgram) {
        s_aip.ac = (uint8_t)((s_aip.ac + (forward ? 1 : 0x3F)) & 0x3F);
        return;
    }
    if (!s_aip.two_line) {                       /* 00h-4Fh */
        s_aip.ac = forward ? (uint8_t)((s_aip.ac >= 0x4F) ? 0x00 : s_aip.ac + 1)
                           : (uint8_t)((s_aip.ac == 0x00) ? 0x4F : s_aip.ac - 1);
        return;
    }
    /* 2-line: 00h-27h, then 40h-67h, then 00h again. */
    if (forward) {
        s_aip.ac = (s_aip.ac == 0x27) ? 0x40
                 : (s_aip.ac == 0x67) ? 0x00
                 : (uint8_t)(s_aip.ac + 1);
    } else {
        s_aip.ac = (s_aip.ac == 0x40) ? 0x27
                 : (s_aip.ac == 0x00) ? 0x67
                 : (uint8_t)(s_aip.ac - 1);
    }
}

/* One instruction (rs = false) or data byte (rs = true), latched at t_ns. */
static void aip_accept(uint8_t byte, bool rs, uint64_t t_ns)
{
    if (t_ns < s_aip.busy_until_ns) {
        if (s_viol_count < MOCK_VIOLATION_MAX) {
            mock_aip_violation_t *v = &s_viol[s_viol_count++];
            v->t_ns          = t_ns;
            v->byte          = byte;
            v->is_data       = rs;
            v->busy_with     = s_aip.busy_with;
            v->busy_since_ns = s_aip.busy_since_ns;
            v->busy_until_ns = s_aip.busy_until_ns;
        }
        return;                                  /* datasheet: "cannot be accepted" */
    }

    uint64_t exec_ns = MOCK_AIP_T_INSTR_NS;
    if (rs) {
        if (s_aip.ac_cgram) s_aip.cgram[s_aip.ac & 0x3F] = byte;
        else                s_aip.ddram[s_aip.ac & 0x7F] = byte;
        aip_step_ac(s_aip.increment);
        exec_ns = MOCK_AIP_T_DATA_NS;
    } else {
        if (s_aip.first_instruction_ns == UINT64_MAX) {
            s_aip.first_instruction_ns = t_ns;
        }
        if (byte & 0x80) {                       /* Set DDRAM address */
            s_aip.ac       = byte & 0x7F;
            s_aip.ac_cgram = false;
        } else if (byte & 0x40) {                /* Set CGRAM address */
            s_aip.ac       = byte & 0x3F;
            s_aip.ac_cgram = true;
        } else if (byte & 0x20) {                /* Function set */
            s_aip.eight_bit = (byte & 0x10) != 0;
            s_aip.two_line  = (byte & 0x08) != 0;
            s_aip.big_font  = (byte & 0x04) != 0;
        } else if (byte & 0x10) {                /* Cursor or display shift */
            if ((byte & 0x08) == 0) {            /* S/C = 0: cursor moves; R/L = direction */
                aip_step_ac((byte & 0x04) != 0);
            }                                    /* display shift: not modelled */
        } else if (byte & 0x08) {                /* Display ON/OFF control */
            s_aip.display_on = (byte & 0x04) != 0;
            s_aip.cursor_on  = (byte & 0x02) != 0;
            s_aip.blink_on   = (byte & 0x01) != 0;
        } else if (byte & 0x04) {                /* Entry mode set */
            s_aip.increment = (byte & 0x02) != 0;
            s_aip.shift     = (byte & 0x01) != 0;
        } else if (byte & 0x02) {                /* Return home */
            s_aip.ac       = 0x00;
            s_aip.ac_cgram = false;
            exec_ns        = MOCK_AIP_T_SLOW_NS;
        } else if (byte & 0x01) {                /* Clear display */
            memset(s_aip.ddram, 0x20, sizeof(s_aip.ddram));
            s_aip.ac        = 0x00;
            s_aip.ac_cgram  = false;
            s_aip.increment = true;
            exec_ns         = MOCK_AIP_T_SLOW_NS;
        }
    }
    s_aip.busy_with     = rs ? (uint16_t)(0x100u | byte) : byte;
    s_aip.busy_since_ns = t_ns;
    s_aip.busy_until_ns = t_ns + exec_ns;
}

/* A payload byte is latched with its ACK: START, the address byte, then
 * 9 bits for each payload byte up to and including this one. */
static uint64_t byte_latch_ns(uint64_t t0_ns, size_t index, uint64_t bit_ns)
{
    return t0_ns + (uint64_t)(1 + 9 + 9 * (index + 1)) * bit_ns;
}

static void aip_receive(const uint8_t *p, size_t len, uint64_t t0_ns, uint64_t bit_ns)
{
    size_t i = 0;
    while (i < len) {
        const uint8_t ctrl = p[i++];
        const bool    co   = (ctrl & 0x80) != 0;   /* 1: another control byte follows */
        const bool    rs   = (ctrl & 0x40) != 0;
        do {
            if (i >= len) {
                return;
            }
            aip_accept(p[i], rs, byte_latch_ns(t0_ns, i, bit_ns));
            i++;
        } while (!co);                             /* Co = 0: a stream up to STOP */
    }
}

int mock_aip_violation_count(void)
{
    return s_viol_count;
}

const mock_aip_violation_t *mock_aip_violation(int index)
{
    return (index >= 0 && index < s_viol_count) ? &s_viol[index] : NULL;
}

uint64_t       mock_aip_first_instruction_ns(void) { return s_aip.first_instruction_ns; }
uint8_t        mock_aip_ddram(uint8_t a)           { return s_aip.ddram[a & 0x7F]; }
const uint8_t *mock_aip_cgram(uint8_t slot)        { return &s_aip.cgram[(slot & 7u) * 8u]; }
uint8_t        mock_aip_ac(void)                   { return s_aip.ac; }
bool           mock_aip_ac_in_cgram(void)          { return s_aip.ac_cgram; }
bool           mock_aip_display_on(void)           { return s_aip.display_on; }
bool           mock_aip_cursor_on(void)            { return s_aip.cursor_on; }
bool           mock_aip_blink_on(void)             { return s_aip.blink_on; }
bool           mock_aip_two_line(void)             { return s_aip.two_line; }
bool           mock_aip_increment(void)            { return s_aip.increment; }
bool           mock_aip_shift(void)                { return s_aip.shift; }

char mock_aip_char_at(uint8_t row, uint8_t col)
{
    const uint8_t base = (row == 0) ? 0x00 : 0x40;
    return (char)s_aip.ddram[(base + col) & 0x7F];
}

/* ---------------------------------------------------------------------------
 * PCA9633
 * --------------------------------------------------------------------------- */
typedef struct {
    bool    present;
    uint8_t reg[13];
} pca_t;

static pca_t s_pca;
int          mock_pca_unmodelled_writes;

static void pca_power_on(void)
{
    /* MODE1 (SLEEP, ALLCALL), MODE2, PWM0-3, GRPPWM, GRPFREQ, LEDOUT,
     * SUBADR1-3, ALLCALLADR. */
    static const uint8_t k_reset[13] = {
        0x11, 0x05, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xE2, 0xE4, 0xE8, 0xE0
    };
    s_pca.present = true;
    memcpy(s_pca.reg, k_reset, sizeof(k_reset));
}

static void pca_receive(const uint8_t *p, size_t len)
{
    uint8_t       ptr = p[0] & 0x0F;
    const uint8_t ai  = p[0] & 0xE0;
    if (ai != 0x00 && ai != 0x80) {
        mock_pca_unmodelled_writes++;
        return;
    }
    for (size_t i = 1; i < len; i++) {
        if (ptr == 0x00) {
            s_pca.reg[0] = (uint8_t)((s_pca.reg[0] & 0xE0) | (p[i] & 0x1F));  /* AI bits read-only */
        } else if (ptr <= 0x0C) {
            s_pca.reg[ptr] = p[i];
        }
        if (ai == 0x80) {
            ptr = (ptr >= 0x0C) ? 0x00 : (uint8_t)(ptr + 1);
        }
    }
}

uint8_t mock_pca_reg(uint8_t reg)
{
    return (reg <= 0x0C) ? s_pca.reg[reg] : 0;
}

/* ---------------------------------------------------------------------------
 * Control
 * --------------------------------------------------------------------------- */

void mock_lcd_clear_logs(void)
{
    memset(s_log, 0, sizeof(s_log));
    s_log_count  = 0;
    memset(s_viol, 0, sizeof(s_viol));
    s_viol_count = 0;
}

void mock_lcd_reset(void)
{
    s_now_ns = 0;
    aip_power_on();
    pca_power_on();
    mock_lcd_clear_logs();
    mock_i2c_misuse            = 0;
    mock_pca_unmodelled_writes = 0;
    mock_task_delay_calls      = 0;
}

void mock_lcd_set_present(uint8_t addr, bool present)
{
    if (addr == MOCK_AIP_ADDR) s_aip.present = present;
    if (addr == MOCK_PCA_ADDR) s_pca.present = present;
}

/* ---------------------------------------------------------------------------
 * LIB-2 API (i2c_bus.h)
 * --------------------------------------------------------------------------- */

i2c_status_t i2c_write(uint8_t addr, const uint8_t *data, size_t len)
{
    if (len > 0 && data == NULL) {
        mock_i2c_misuse++;
        return I2C_ERR_BUS_BUSY;
    }

    /* i2c_bus runs device writes at I2C_FREQ_HZ; a zero-length write is an
     * i2c_master_probe(), which ESP-IDF clocks at 100 kHz. */
    const bool     probe  = (len == 0);
    const uint64_t bit_ns = probe ? 10000ULL : (1000000000ULL / I2C_FREQ_HZ);
    const uint64_t t0_ns  = s_now_ns;
    const bool     acked  = (addr == MOCK_AIP_ADDR && s_aip.present) ||
                            (addr == MOCK_PCA_ADDR && s_pca.present);

    mock_xfer_t *x = (s_log_count < MOCK_XFER_LOG_MAX) ? &s_log[s_log_count++]
                                                       : &s_log_overflow;
    memset(x, 0, sizeof(*x));
    x->t_start_ns = t0_ns;
    x->addr       = addr;
    x->len        = (uint8_t)((len < 255u) ? len : 255u);
    if (!probe) {
        memcpy(x->data, data, (len < MOCK_XFER_MAX_BYTES) ? len : MOCK_XFER_MAX_BYTES);
    }

    if (!acked) {
        s_now_ns  = t0_ns + (uint64_t)(1 + 9 + 1) * bit_ns;   /* START, address, NACK, STOP */
        x->result = I2C_ERR_NACK;
        return x->result;
    }
    if (!probe) {
        if (addr == MOCK_AIP_ADDR) aip_receive(data, len, t0_ns, bit_ns);
        else                       pca_receive(data, len);
    }
    s_now_ns  = t0_ns + (uint64_t)(1 + 9 * (len + 1) + 1) * bit_ns;
    x->result = I2C_OK;
    return x->result;
}
