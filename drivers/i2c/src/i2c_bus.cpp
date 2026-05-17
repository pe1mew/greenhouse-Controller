/**
 * @file i2c_bus.cpp
 * @brief Shared I2C bus driver implementation — ESP-IDF v5 i2c_master_* API (LIB-2).
 *
 * Migrated from Arduino Wire.h to ESP-IDF v5's new i2c_master_* API in
 * 2.0.0-alpha.2.4 (Phase 2.4). The IDF v5 API differs significantly from
 * both Arduino's Wire and IDF's legacy `i2c_master_cmd_begin` pattern:
 * each device on the bus gets its own `i2c_master_dev_handle_t`, created
 * via `i2c_master_bus_add_device()`. The bus is a separate object
 * (`i2c_master_bus_handle_t`) created once in `i2c_init()`.
 *
 * Implementation strategy: the public API takes an address per call (legacy
 * pattern from the Wire era). To preserve it without forcing callers to
 * pre-register every device, this driver creates a transient device handle
 * inside each i2c_write / i2c_read / i2c_write_read call, performs the I/O,
 * then removes the handle. Cost is roughly one small bookkeeping struct
 * allocation per call — negligible at the sub-Hz transaction rates we
 * actually see (LCD ~10 Hz max during a status-screen update, RTC once
 * per minute).
 *
 * For i2c_scan, the v5 API exposes `i2c_master_probe()` directly on the
 * bus handle, no device handle needed — much cleaner than the Wire-era
 * "begin + endTransmission" probe loop.
 *
 * Bus-level config:
 *   - Port I2C_NUM_0 (ESP32-S3 has two; we use the first)
 *   - Internal pull-ups ENABLED (matches Arduino Wire default; hardware
 *     does not have external pull-up resistors per project schematic)
 *   - Glitch filter at 7 (IDF's standard default for low-noise environments)
 *   - Clock speed I2C_FREQ_HZ = 400 kHz per device (Fast-mode)
 *
 * Concurrency: i2c_lock() / i2c_unlock() remain no-ops in alpha.2.4 —
 * the FreeRTOS mutex was deferred in the original LIB-2 spec and the
 * migration doesn't change that. Each i2c_master_transmit / _receive
 * call IS internally synchronised by the IDF driver, so single-shot
 * calls from concurrent tasks are safe; multi-call atomic sequences
 * still need higher-level coordination.
 */

#ifdef UNIT_TEST
  #include "../test/mock_wire.h"
#endif

#include "i2c_bus.h"

#ifndef UNIT_TEST
  #include "driver/i2c_master.h"
  #include "esp_err.h"
#endif

/* ---------------------------------------------------------------------------
 * Module-static bus handle.  Created lazily in i2c_init(), torn down only
 * by i2c_deinit() (not currently exposed — there's no use case yet).
 * --------------------------------------------------------------------------- */
#ifndef UNIT_TEST
static i2c_master_bus_handle_t s_bus = NULL;
#endif

/* I/O timeout per call (ms).  Generous default — matches the implicit
 * Arduino Wire timeout for low-rate transactions.  Overrideable via
 * the I2C_OP_TIMEOUT_MS macro at build time if a specific use case
 * needs tighter bounds. */
#ifndef I2C_OP_TIMEOUT_MS
#  define I2C_OP_TIMEOUT_MS  1000
#endif

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */

#ifndef UNIT_TEST
/* Map an esp_err_t from an i2c_master_* call to our i2c_status_t.
 *
 * NACK distinction (refined 2026-05-17 during alpha.2.5 LCD acceptance):
 * IDF v5 reports a slave NACK via TWO different esp_err_t codes depending
 * on which API call produced it:
 *   - i2c_master_probe()    → ESP_ERR_NOT_FOUND   (the dedicated probe code)
 *   - i2c_master_transmit() → ESP_FAIL            (per the data-write path)
 * Both must map to our I2C_ERR_NACK so callers that distinguish "device
 * absent" from "bus error" (e.g. lcd1602's optional PCA9633 RGB detection
 * at 0x60) behave correctly. Missing the ESP_ERR_NOT_FOUND mapping caused
 * pca9633_init() to return LCD_ERR_COMM on Unit 2 (where the RGB backlight
 * is absent), aborting the entire lcd_init() chain. */
static i2c_status_t to_status(esp_err_t e)
{
    switch (e) {
        case ESP_OK:
            return I2C_OK;
        case ESP_ERR_TIMEOUT:
            return I2C_ERR_TIMEOUT;
        case ESP_FAIL:                 /* i2c_master_transmit NACK */
        case ESP_ERR_NOT_FOUND:        /* i2c_master_probe    NACK */
        case ESP_ERR_INVALID_RESPONSE: /* malformed slave reply — treat as NACK */
            return I2C_ERR_NACK;
        case ESP_ERR_INVALID_STATE:
            return I2C_ERR_BUS_BUSY;
        default:
            return I2C_ERR_BUS_BUSY;
    }
}

/* Create a transient device handle for a single transaction, run a body
 * (passed as a lambda-like helper), and tear down. Centralises the
 * create-use-destroy boilerplate so the public API functions stay short. */
static i2c_status_t with_device(uint8_t addr,
                                 i2c_status_t (*body)(i2c_master_dev_handle_t))
{
    if (s_bus == NULL) return I2C_ERR_BUS_BUSY;   /* i2c_init not called */

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = addr;
    dev_cfg.scl_speed_hz    = I2C_FREQ_HZ;

    i2c_master_dev_handle_t dev = NULL;
    esp_err_t e = i2c_master_bus_add_device(s_bus, &dev_cfg, &dev);
    if (e != ESP_OK) return to_status(e);

    i2c_status_t st = body(dev);

    (void)i2c_master_bus_rm_device(dev);   /* best-effort tear-down */
    return st;
}
#endif  /* !UNIT_TEST */

/* ---------------------------------------------------------------------------
 * API implementation
 * --------------------------------------------------------------------------- */

i2c_status_t i2c_init(void)
{
#ifndef UNIT_TEST
    if (s_bus != NULL) {
        /* Idempotent — already initialised. */
        return I2C_OK;
    }

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port          = I2C_NUM_0;
    bus_cfg.sda_io_num        = (gpio_num_t)PIN_I2C_SDA;
    bus_cfg.scl_io_num        = (gpio_num_t)PIN_I2C_SCL;
    bus_cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;                  /* IDF default */
    bus_cfg.flags.enable_internal_pullup = true;    /* matches Wire default */

    esp_err_t e = i2c_new_master_bus(&bus_cfg, &s_bus);
    return to_status(e);
#else
    return I2C_OK;
#endif
}

/* Need module-scope thunk objects for the with_device helper because
 * C++ lambdas with captures can't decay to plain function pointers. The
 * unit-test path takes a separate codepath below so this is target-only. */
#ifndef UNIT_TEST
static const uint8_t *s_tx_buf;
static size_t          s_tx_len;
static uint8_t        *s_rx_buf;
static size_t          s_rx_len;

static i2c_status_t do_write(i2c_master_dev_handle_t dev)
{
    /* Zero-length write = address-only probe.  i2c_master_transmit with
     * len=0 is the documented way to do this on the v5 API. */
    esp_err_t e = i2c_master_transmit(dev, s_tx_buf, s_tx_len, I2C_OP_TIMEOUT_MS);
    return to_status(e);
}

static i2c_status_t do_read(i2c_master_dev_handle_t dev)
{
    esp_err_t e = i2c_master_receive(dev, s_rx_buf, s_rx_len, I2C_OP_TIMEOUT_MS);
    return to_status(e);
}

static i2c_status_t do_write_read(i2c_master_dev_handle_t dev)
{
    esp_err_t e = i2c_master_transmit_receive(
        dev, s_tx_buf, s_tx_len, s_rx_buf, s_rx_len, I2C_OP_TIMEOUT_MS);
    return to_status(e);
}
#endif

i2c_status_t i2c_write(uint8_t addr, const uint8_t *data, size_t len)
{
#ifndef UNIT_TEST
    /* Zero-length write = address-only probe per the LIB-2 contract
     * (i2c_bus.h documents this explicitly: "Zero-length writes are
     * accepted and serve as an address-only probe — the device will
     * ACK or NACK its address"). Under arduino-esp32's Wire library
     * this just worked: Wire.beginTransmission(addr) + endTransmission(true)
     * sent only the address byte and reported NACK/ACK.
     *
     * The ESP-IDF v5 i2c_master_transmit API does NOT accept zero-length
     * transmissions (returns ESP_ERR_INVALID_ARG with "buffer or size
     * invalid"). For probes, IDF v5 instead provides i2c_master_probe()
     * which works directly on the bus handle (no device handle needed)
     * and sends exactly a START + addr + W + STOP, reporting ACK/NACK
     * as ESP_OK / ESP_FAIL.
     *
     * Redirecting here preserves the LIB-2 public contract — callers
     * doing zero-length probes (e.g. the LCD driver's optional RGB-
     * backlight detection at LCD_RGB_I2C_ADDR, or its own presence
     * probe at LCD_I2C_ADDR before sending any commands) keep working
     * unchanged. Surfaced 2026-05-17 during alpha.2.5 LCD acceptance —
     * the LCD's i2c_write(addr, NULL, 0) returned COMM error and
     * lcd_init() refused to initialise; redirecting through probe
     * fixed it. */
    if (len == 0) {
        if (s_bus == NULL) return I2C_ERR_BUS_BUSY;   /* i2c_init not called */
        esp_err_t e = i2c_master_probe(s_bus, addr, I2C_OP_TIMEOUT_MS);
        return to_status(e);
    }

    s_tx_buf = data;
    s_tx_len = len;
    return with_device(addr, do_write);
#else
    /* Native unit-test path falls through to mock_wire.h's helpers. */
    (void)addr; (void)data; (void)len;
    return I2C_OK;
#endif
}

i2c_status_t i2c_read(uint8_t addr, uint8_t *buf, size_t len)
{
#ifndef UNIT_TEST
    s_rx_buf = buf;
    s_rx_len = len;
    return with_device(addr, do_read);
#else
    (void)addr; (void)buf; (void)len;
    return I2C_OK;
#endif
}

i2c_status_t i2c_write_read(uint8_t addr,
                              const uint8_t *tx, size_t tx_len,
                              uint8_t       *rx, size_t rx_len)
{
#ifndef UNIT_TEST
    s_tx_buf = tx;
    s_tx_len = tx_len;
    s_rx_buf = rx;
    s_rx_len = rx_len;
    return with_device(addr, do_write_read);
#else
    (void)addr; (void)tx; (void)tx_len; (void)rx; (void)rx_len;
    return I2C_OK;
#endif
}

uint8_t i2c_scan(uint8_t *found_addrs, uint8_t max_count)
{
#ifndef UNIT_TEST
    if (s_bus == NULL) return 0;

    uint8_t count = 0;
    /* IDF v5 exposes i2c_master_probe() — sends START + addr + W and
     * checks for ACK. No device handle needed. Much cleaner than the
     * Wire-era beginTransmission + endTransmission probe pattern. */
    for (uint8_t addr = 1; addr < 127 && count < max_count; addr++) {
        esp_err_t e = i2c_master_probe(s_bus, addr, I2C_OP_TIMEOUT_MS);
        if (e == ESP_OK) {
            found_addrs[count++] = addr;
        }
    }
    return count;
#else
    (void)found_addrs; (void)max_count;
    return 0;
#endif
}

void i2c_lock(void)   { /* no-op — FreeRTOS mutex not yet implemented */ }
void i2c_unlock(void) { /* no-op — FreeRTOS mutex not yet implemented */ }
