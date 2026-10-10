/**
 * @file mock_i2c_master.cpp
 * @brief ESP-IDF I2C master API answered from a simulated bus (LIB-2 native tests).
 *
 * See mock_i2c_master.h for the model and for where each return code comes
 * from in ESP-IDF 5.5.0.
 */

#include "mock_i2c_master.h"
#include <string.h>

/* The real handles are pointers to these driver-private structs. */
struct i2c_master_bus_t {
    int port;
};

struct i2c_master_dev_t {
    bool                live;
    i2c_device_config_t cfg;
};

/* ---------------------------------------------------------------------------
 * State
 * --------------------------------------------------------------------------- */

/* Bus: survives mock_i2c_reset(), as the port stays acquired on the board. */
static struct i2c_master_bus_t s_bus;
static bool                    s_bus_created;
static i2c_master_bus_config_t s_bus_cfg;
static int                     s_bus_create_calls;

static struct i2c_master_dev_t s_handles[MOCK_I2C_HANDLE_MAX];

typedef struct {
    bool    present;       /* ACKs its address */
    bool    hold_scl;      /* stretches SCL forever */
    uint8_t ptr;           /* register pointer */
    uint8_t reg[256];
} sim_device_t;

static sim_device_t s_dev[128];

static mock_i2c_xfer_t s_log[MOCK_I2C_LOG_MAX];
static int             s_log_count;
static mock_i2c_xfer_t s_log_overflow;   /* absorbs writes once the log is full */

int                 mock_i2c_live_handles;
int                 mock_i2c_handles_added;
i2c_device_config_t mock_i2c_last_device_config;
int                 mock_i2c_stale_handle_calls;
int                 mock_i2c_invalid_arg_calls;

/* Bus status a transfer ends in, as i2c_master.c tracks it. */
typedef enum {
    BUS_DONE,         /* I2C_STATUS_DONE */
    BUS_ACK_ERROR,    /* I2C_STATUS_ACK_ERROR: address NACKed */
    BUS_TIMEOUT       /* I2C_STATUS_TIMEOUT: SCL never released */
} bus_status_t;

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */

/* soc/esp32s3 soc_caps.h: GPIO 0-48 exist except 22-25. */
static bool gpio_exists(int n)
{
    return n >= 0 && n < GPIO_NUM_MAX && !(n >= 22 && n <= 25);
}

static bool handle_is_live(i2c_master_dev_handle_t h)
{
    for (int i = 0; i < MOCK_I2C_HANDLE_MAX; i++) {
        if (h == &s_handles[i]) {
            return s_handles[i].live;
        }
    }
    return false;
}

static mock_i2c_xfer_t *log_xfer(mock_i2c_kind_t kind, uint16_t addr,
                                 const uint8_t *tx, size_t tx_len, size_t rx_len,
                                 uint32_t speed_hz, int timeout_ms)
{
    mock_i2c_xfer_t *x = (s_log_count < MOCK_I2C_LOG_MAX) ? &s_log[s_log_count++]
                                                          : &s_log_overflow;
    memset(x, 0, sizeof(*x));
    x->kind         = kind;
    x->addr         = addr;
    x->tx_len       = tx_len;
    x->rx_len       = rx_len;
    x->scl_speed_hz = speed_hz;
    x->timeout_ms   = timeout_ms;
    if (tx != NULL) {
        memcpy(x->tx, tx, (tx_len < MOCK_I2C_XFER_MAX_BYTES) ? tx_len : MOCK_I2C_XFER_MAX_BYTES);
    }
    return x;
}

static bus_status_t address_phase(uint16_t addr)
{
    if (addr >= 128) {
        return BUS_ACK_ERROR;           /* 10-bit addressing is not modelled */
    }
    if (s_dev[addr].hold_scl) {
        return BUS_TIMEOUT;
    }
    return s_dev[addr].present ? BUS_DONE : BUS_ACK_ERROR;
}

/* s_i2c_transaction_start(): "if (status != I2C_STATUS_DONE)
 * ret = ESP_ERR_INVALID_STATE;" — NACK and stall alike. */
static esp_err_t sync_transfer_result(bus_status_t st)
{
    return (st == BUS_DONE) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void device_write(sim_device_t *d, const uint8_t *buf, size_t len)
{
    d->ptr = buf[0];
    for (size_t i = 1; i < len; i++) {
        d->reg[d->ptr++] = buf[i];
    }
}

static void device_read(sim_device_t *d, uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        buf[i] = d->reg[d->ptr++];
    }
}

/* Common front end of the three device transfers: the checks ESP-IDF makes
 * before touching the bus. Returns ESP_OK to proceed. */
static esp_err_t check_handle(i2c_master_dev_handle_t dev)
{
    if (dev == NULL) {
        mock_i2c_invalid_arg_calls++;   /* "i2c handle not initialized" */
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle_is_live(dev)) {
        mock_i2c_stale_handle_calls++;  /* freed memory on the board */
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static bool buffer_ok(const uint8_t *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        mock_i2c_invalid_arg_calls++;   /* "buffer or size invalid" */
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Control and inspection
 * --------------------------------------------------------------------------- */

void mock_i2c_reset(void)
{
    memset(s_dev, 0, sizeof(s_dev));
    memset(s_handles, 0, sizeof(s_handles));
    memset(s_log, 0, sizeof(s_log));
    s_log_count                 = 0;
    mock_i2c_live_handles       = 0;
    mock_i2c_handles_added      = 0;
    mock_i2c_stale_handle_calls = 0;
    mock_i2c_invalid_arg_calls  = 0;
    memset(&mock_i2c_last_device_config, 0, sizeof(mock_i2c_last_device_config));
}

const i2c_master_bus_config_t *mock_i2c_bus_config(void)
{
    return s_bus_created ? &s_bus_cfg : NULL;
}

int mock_i2c_bus_create_calls(void)
{
    return s_bus_create_calls;
}

void mock_i2c_attach(uint8_t addr)
{
    if (addr < 128) {
        memset(&s_dev[addr], 0, sizeof(s_dev[addr]));
        s_dev[addr].present = true;
    }
}

void mock_i2c_set_registers(uint8_t addr, uint8_t first_reg,
                            const uint8_t *values, size_t count)
{
    if (addr >= 128) return;
    for (size_t i = 0; i < count && first_reg + i < 256; i++) {
        s_dev[addr].reg[first_reg + i] = values[i];
    }
}

void mock_i2c_set_pointer(uint8_t addr, uint8_t reg)
{
    if (addr < 128) s_dev[addr].ptr = reg;
}

uint8_t mock_i2c_register(uint8_t addr, uint8_t reg)
{
    return (addr < 128) ? s_dev[addr].reg[reg] : 0;
}

void mock_i2c_hold_scl(uint8_t addr, bool hold)
{
    if (addr < 128) s_dev[addr].hold_scl = hold;
}

int mock_i2c_xfer_count(void)
{
    return s_log_count;
}

const mock_i2c_xfer_t *mock_i2c_xfer(int index)
{
    return (index >= 0 && index < s_log_count) ? &s_log[index] : NULL;
}

/* ---------------------------------------------------------------------------
 * ESP-IDF API (driver/i2c_master.h)
 * --------------------------------------------------------------------------- */

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *bus_config,
                             i2c_master_bus_handle_t *ret_bus_handle)
{
    s_bus_create_calls++;
    if (bus_config == NULL || ret_bus_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!((bus_config->i2c_port >= 0 && bus_config->i2c_port < I2C_NUM_MAX) ||
          bus_config->i2c_port == -1)) {
        return ESP_ERR_INVALID_ARG;                 /* "invalid i2c port number" */
    }
    if (!gpio_exists(bus_config->sda_io_num) || !gpio_exists(bus_config->scl_io_num)) {
        return ESP_ERR_INVALID_ARG;                 /* "invalid SDA/SCL pin number" */
    }
    if (s_bus_created) {
        return ESP_ERR_INVALID_STATE;               /* "bus id has already been acquired" */
    }
    s_bus_created = true;
    s_bus_cfg     = *bus_config;
    s_bus.port    = (bus_config->i2c_port == -1) ? 0 : bus_config->i2c_port;
    *ret_bus_handle = &s_bus;
    return ESP_OK;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus_handle,
                                    const i2c_device_config_t *dev_config,
                                    i2c_master_dev_handle_t *ret_handle)
{
    if (bus_handle != &s_bus || !s_bus_created) {
        mock_i2c_invalid_arg_calls++;               /* "this bus is not initialized" */
        return ESP_ERR_INVALID_ARG;
    }
    if (dev_config == NULL || ret_handle == NULL || dev_config->scl_speed_hz == 0) {
        mock_i2c_invalid_arg_calls++;
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < MOCK_I2C_HANDLE_MAX; i++) {
        if (!s_handles[i].live) {
            s_handles[i].live = true;
            s_handles[i].cfg  = *dev_config;
            mock_i2c_live_handles++;
            mock_i2c_handles_added++;
            mock_i2c_last_device_config = *dev_config;
            *ret_handle = &s_handles[i];
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;                          /* handles leaked until the pool ran out */
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t handle)
{
    esp_err_t e = check_handle(handle);
    if (e != ESP_OK) {
        return e;
    }
    handle->live = false;
    mock_i2c_live_handles--;
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t i2c_dev,
                              const uint8_t *write_buffer, size_t write_size,
                              int xfer_timeout_ms)
{
    esp_err_t e = check_handle(i2c_dev);
    if (e != ESP_OK) return e;
    if (!buffer_ok(write_buffer, write_size)) return ESP_ERR_INVALID_ARG;

    uint16_t addr = i2c_dev->cfg.device_address;
    mock_i2c_xfer_t *x = log_xfer(MOCK_I2C_WRITE, addr, write_buffer, write_size, 0,
                                  i2c_dev->cfg.scl_speed_hz, xfer_timeout_ms);
    bus_status_t st = address_phase(addr);
    if (st == BUS_DONE) {
        device_write(&s_dev[addr], write_buffer, write_size);
    }
    x->result = sync_transfer_result(st);
    return x->result;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t i2c_dev,
                                      const uint8_t *write_buffer, size_t write_size,
                                      uint8_t *read_buffer, size_t read_size,
                                      int xfer_timeout_ms)
{
    esp_err_t e = check_handle(i2c_dev);
    if (e != ESP_OK) return e;
    if (!buffer_ok(write_buffer, write_size)) return ESP_ERR_INVALID_ARG;
    if (!buffer_ok(read_buffer, read_size))   return ESP_ERR_INVALID_ARG;

    uint16_t addr = i2c_dev->cfg.device_address;
    mock_i2c_xfer_t *x = log_xfer(MOCK_I2C_WRITE_READ, addr, write_buffer, write_size,
                                  read_size, i2c_dev->cfg.scl_speed_hz, xfer_timeout_ms);
    bus_status_t st = address_phase(addr);
    if (st == BUS_DONE) {
        device_write(&s_dev[addr], write_buffer, write_size);   /* request first ... */
        device_read(&s_dev[addr], read_buffer, read_size);      /* ... then the reply */
    }
    x->result = sync_transfer_result(st);
    return x->result;
}

esp_err_t i2c_master_receive(i2c_master_dev_handle_t i2c_dev,
                             uint8_t *read_buffer, size_t read_size,
                             int xfer_timeout_ms)
{
    esp_err_t e = check_handle(i2c_dev);
    if (e != ESP_OK) return e;
    if (!buffer_ok(read_buffer, read_size)) return ESP_ERR_INVALID_ARG;

    uint16_t addr = i2c_dev->cfg.device_address;
    mock_i2c_xfer_t *x = log_xfer(MOCK_I2C_READ, addr, NULL, 0, read_size,
                                  i2c_dev->cfg.scl_speed_hz, xfer_timeout_ms);
    bus_status_t st = address_phase(addr);
    if (st == BUS_DONE) {
        device_read(&s_dev[addr], read_buffer, read_size);
    }
    x->result = sync_transfer_result(st);
    return x->result;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus_handle, uint16_t address,
                           int xfer_timeout_ms)
{
    if (bus_handle != &s_bus || !s_bus_created) {
        mock_i2c_invalid_arg_calls++;               /* "i2c handle not initialized" */
        return ESP_ERR_INVALID_ARG;
    }
    /* i2c_master_probe() clocks the address at a fixed 100 kHz. */
    mock_i2c_xfer_t *x = log_xfer(MOCK_I2C_PROBE, address, NULL, 0, 0,
                                  100000u, xfer_timeout_ms);
    switch (address_phase(address)) {
        case BUS_DONE:      x->result = ESP_OK;            break;
        case BUS_ACK_ERROR: x->result = ESP_ERR_NOT_FOUND; break;
        case BUS_TIMEOUT:   x->result = ESP_ERR_TIMEOUT;   break;
    }
    return x->result;
}
