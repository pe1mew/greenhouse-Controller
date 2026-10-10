/**
 * @file driver/i2c_master.h
 * @brief Host-build stand-in for ESP-IDF's driver/i2c_master.h (LIB-2 native tests).
 *
 * The native build compiles the ESP-IDF branch of i2c_bus.cpp (see
 * test/include_lib.cpp), which includes "driver/i2c_master.h"; the native
 * env puts test/mock_idf/ on the include path so this file answers.
 * Never on a target include path.
 *
 * Declarations mirror ESP-IDF 5.5.0 for the ESP32-S3: driver/i2c_master.h,
 * driver/i2c_types.h, hal/i2c_types.h, soc/esp32s3 clk_tree_defs.h and
 * gpio_num.h. Only what i2c_bus.cpp uses is declared. The behaviour is
 * simulated in mock_i2c_master.cpp; see mock_i2c_master.h.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/* soc/esp32s3/include/soc/gpio_num.h (abbreviated; the driver only casts). */
typedef enum {
    GPIO_NUM_NC  = -1,
    GPIO_NUM_0   = 0,
    GPIO_NUM_48  = 48,
    GPIO_NUM_MAX = 49,
} gpio_num_t;

/* hal/i2c_types.h */
typedef enum {
    I2C_NUM_0 = 0,
    I2C_NUM_1,
    I2C_NUM_MAX,              /* the S3 has two HP ports and no LP port */
} i2c_port_t;

typedef enum {
    I2C_ADDR_BIT_LEN_7  = 0,
    I2C_ADDR_BIT_LEN_10 = 1,
} i2c_addr_bit_len_t;

/* soc/esp32s3/include/soc/clk_tree_defs.h — the real enumerators are
 * SOC_MOD_CLK_XTAL (11) and SOC_MOD_CLK_RC_FAST (9) of soc_module_clk_t. */
typedef enum {
    I2C_CLK_SRC_XTAL    = 11,
    I2C_CLK_SRC_RC_FAST = 9,
    I2C_CLK_SRC_DEFAULT = I2C_CLK_SRC_XTAL,
} soc_periph_i2c_clk_src_t;

typedef soc_periph_i2c_clk_src_t i2c_clock_source_t;

/* driver/i2c_types.h */
typedef int i2c_port_num_t;
typedef struct i2c_master_bus_t *i2c_master_bus_handle_t;
typedef struct i2c_master_dev_t *i2c_master_dev_handle_t;

/* driver/i2c_master.h */
typedef struct {
    i2c_port_num_t i2c_port;
    gpio_num_t     sda_io_num;
    gpio_num_t     scl_io_num;
    union {
        i2c_clock_source_t clk_source;
    };
    uint8_t        glitch_ignore_cnt;
    int            intr_priority;
    size_t         trans_queue_depth;
    struct {
        uint32_t enable_internal_pullup : 1;
        uint32_t allow_pd               : 1;
    } flags;
} i2c_master_bus_config_t;

typedef struct {
    i2c_addr_bit_len_t dev_addr_length;
    uint16_t           device_address;
    uint32_t           scl_speed_hz;
    uint32_t           scl_wait_us;
    struct {
        uint32_t disable_ack_check : 1;
    } flags;
} i2c_device_config_t;

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *bus_config,
                             i2c_master_bus_handle_t *ret_bus_handle);

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus_handle,
                                    const i2c_device_config_t *dev_config,
                                    i2c_master_dev_handle_t *ret_handle);

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t handle);

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t i2c_dev,
                              const uint8_t *write_buffer, size_t write_size,
                              int xfer_timeout_ms);

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t i2c_dev,
                                      const uint8_t *write_buffer, size_t write_size,
                                      uint8_t *read_buffer, size_t read_size,
                                      int xfer_timeout_ms);

esp_err_t i2c_master_receive(i2c_master_dev_handle_t i2c_dev,
                             uint8_t *read_buffer, size_t read_size,
                             int xfer_timeout_ms);

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus_handle, uint16_t address,
                           int xfer_timeout_ms);
