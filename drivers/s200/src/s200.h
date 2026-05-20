/**
 * @file s200.h
 * @brief SenseCAP ONE V2 S200 wind sensor driver — LIB-S200.
 *
 * Thin driver over LIB-6 (modbus_rtu) for the Seeed SenseCAP ONE V2 S200
 * compact wind station mounted on the greenhouse roof.  Provides:
 *   - Measurement reads (FC04, wind direction 0x0008–0x000D, wind speed
 *     0x000E–0x0013, heating temperature 0x001C–0x001D)
 *   - A FreeRTOS periodic polling task
 *
 * ## Hardware
 *   - Sensor      : Seeed SenseCAP ONE V2 S200 (ultrasonic, no moving parts).
 *   - Bus         : Modbus RTU over RS-485 (UART1 + SIT65HVD08P, see LIB-6).
 *   - Wiring      : A/B differential pair; see @c PIN_RS485_* in
 *                   @c pin_config.h.  Internal heater prevents icing.
 *   - Speed       : 0–60 m/s, resolution 0.001 m/s.
 *   - Direction   : 0–360°, resolution 0.001°.
 *   - Heater temp : −40 to 85 °C (internal anti-icing heater).
 *   - Comm spec   : 9600 baud, 8N1 (matches @ref MODBUS_BAUD).
 *   - Encoding    : raw register pair is int32 × 1000 of the engineering
 *                   value, big-endian word order (high register first).
 *   - Slave addr  : 44 (factory default, see @ref S200_DEFAULT_ADDR).
 *
 * ## API summary
 *   - @ref s200_read_measurements  Read all wind channels + heater temp.
 *   - @ref s200_task               FreeRTOS periodic-poll task.
 *
 * ## Thread safety
 *   No internal mutex — LIB-6 (modBus) serialises wire traffic, so two
 *   threads each calling this driver will not collide on the wire.
 *   Per-call output structs (@p out) are written by the calling thread
 *   only; if a shared buffer is updated by @ref s200_task, callers must
 *   hold the @c task_param.mutex when reading it.
 *
 * Prerequisites:
 *   Call @c modbus_init() (LIB-6) once before any function in this driver.
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#pragma once

#include <stdint.h>

#ifndef NATIVE_TEST
  #include "freertos/FreeRTOS.h"
  #include "freertos/semphr.h"
#endif

/* ---------------------------------------------------------------------------
 * @defgroup s200_addr Slave address default
 * @{
 * --------------------------------------------------------------------------- */

/**
 * @brief Factory-default Modbus slave address.
 *
 * Valid range is 1–247.  Address 0 (broadcast) is rejected by all read calls.
 */
#define S200_DEFAULT_ADDR  44u

/** @} */

/* ---------------------------------------------------------------------------
 * @defgroup s200_regs Register map (Modbus input-register addresses, FC04)
 * @{
 * --------------------------------------------------------------------------- */

/* --- Wind direction registers (read-only via FC04) ---
 * Each channel occupies two consecutive 16-bit registers (int32, big-endian).
 * Raw value / 1000 = degrees (0–360). */

/** @brief Minimum wind direction, high register.  Pair: 0x0008–0x0009. */
#define S200_REG_WIND_DIR_MIN   0x0008u

/** @brief Maximum wind direction, high register.  Pair: 0x000A–0x000B. */
#define S200_REG_WIND_DIR_MAX   0x000Au

/** @brief Average wind direction, high register.  Pair: 0x000C–0x000D. */
#define S200_REG_WIND_DIR_AVG   0x000Cu

/* --- Wind speed registers (read-only via FC04) ---
 * Raw value / 1000 = m/s (0–60). */

/** @brief Minimum wind speed, high register.  Pair: 0x000E–0x000F. */
#define S200_REG_WIND_SPEED_MIN 0x000Eu

/** @brief Maximum wind speed, high register.  Pair: 0x0010–0x0011. */
#define S200_REG_WIND_SPEED_MAX 0x0010u

/** @brief Average wind speed, high register.  Pair: 0x0012–0x0013. */
#define S200_REG_WIND_SPEED_AVG 0x0012u

/* --- Heating temperature register (read-only via FC04) ---
 * Raw value / 1000 = °C (−40 to 85). */

/** @brief Internal heater temperature, high register.  Pair: 0x001C–0x001D. */
#define S200_REG_HEATING_TEMP   0x001Cu

/* --- Configuration holding registers (FC03 read / FC16 write) --- */

/** @brief Modbus slave address (1–247). */
#define S200_REG_DEVICE_ADDR    0x1000u

/** @brief Baud rate code (96=9600, 192=19200, 384=38400, 576=57600, 1152=115200). */
#define S200_REG_BAUD_RATE      0x1001u

/** @} */ /* end s200_regs */

/* ---------------------------------------------------------------------------
 * @defgroup s200_types Data types
 * @{
 * --------------------------------------------------------------------------- */

/**
 * @brief Return codes for all S200 API functions.
 */
typedef enum {
    S200_OK        = 0, /**< Operation completed successfully. */
    S200_ERR_COMM  = 1, /**< Modbus communication error (timeout / CRC / exception). */
    S200_ERR_PARAM = 2, /**< Caller supplied an invalid parameter (NULL pointer, addr=0). */
} s200_status_t;

/**
 * @brief Wind measurements and heater status from the S200.
 */
typedef struct {
    float wind_dir_min_deg;      /**< Minimum wind direction  (°, 0–360). */
    float wind_dir_max_deg;      /**< Maximum wind direction  (°, 0–360). */
    float wind_dir_avg_deg;      /**< Average wind direction  (°, 0–360). */
    float wind_speed_min_ms;     /**< Minimum wind speed      (m/s, 0–60). */
    float wind_speed_max_ms;     /**< Maximum wind speed      (m/s, 0–60). */
    float wind_speed_avg_ms;     /**< Average wind speed      (m/s, 0–60). */
    float heating_temperature_c; /**< Internal heater temperature (°C, −40…85). */
} s200_measurement_t;

#ifndef NATIVE_TEST
/**
 * @brief Parameters for the FreeRTOS periodic polling task.
 *
 * Allocate statically or on the heap and pass as @c pvParameters when
 * creating the task via @c xTaskCreate() or @c xTaskCreatePinnedToCore().
 * All fields must be initialised before the task starts; @p mutex must be
 * created with @c xSemaphoreCreateMutex() by the caller.
 *
 * @par Typical usage
 * @code
 *   static s200_measurement_t g_meas;
 *   static s200_status_t      g_status;
 *   static s200_task_param_t  g_param = {
 *       .slave_addr       = S200_DEFAULT_ADDR,
 *       .poll_interval_ms = 5000,
 *       .out_data         = &g_meas,
 *       .out_status       = &g_status,
 *       .mutex            = NULL,   // created below
 *   };
 *
 *   void app_main(void) {
 *       modbus_init();
 *       g_param.mutex = xSemaphoreCreateMutex();
 *       xTaskCreatePinnedToCore(s200_task, "s200", 2048,
 *                               &g_param, 5, NULL, APP_CPU_NUM);
 *   }
 * @endcode
 */
typedef struct {
    uint8_t             slave_addr;        /**< Modbus slave address (1–247). */
    uint32_t            poll_interval_ms;  /**< Measurement interval in milliseconds. */
    s200_measurement_t *out_data;          /**< Shared output buffer (protected by mutex). */
    s200_status_t      *out_status;        /**< Last operation status (protected by mutex). */
    SemaphoreHandle_t   mutex;             /**< Caller-created mutex protecting the two pointers above. */
} s200_task_param_t;
#endif /* NATIVE_TEST */

/** @} */ /* end s200_types */

/* ---------------------------------------------------------------------------
 * @defgroup s200_api API
 * @{
 * --------------------------------------------------------------------------- */

/**
 * @brief Read all wind measurements and heating temperature.
 *
 * Issues two FC04 requests:
 *   1. Registers @ref S200_REG_WIND_DIR_MIN … 0x0013 (12 registers):
 *      wind direction and speed (min/max/avg).
 *   2. Registers @ref S200_REG_HEATING_TEMP … 0x001D (2 registers):
 *      internal heater temperature.
 *
 * Raw int32 values (big-endian word order) are divided by 1000 before
 * storing in @p out.  Returns the first error encountered.
 *
 * @param slave_addr  Modbus slave address of the sensor (1–247).
 * @param out         Caller-supplied @ref s200_measurement_t to fill (must
 *                    not be NULL).
 * @return @ref S200_OK on success, @ref S200_ERR_COMM on bus error,
 *         @ref S200_ERR_PARAM if @p out is NULL or @p slave_addr is 0.
 * @note   The two FC04 transactions are issued back-to-back; the average
 *         wind values therefore reflect the sensor-side averaging window,
 *         not a host-side average across the two reads.
 * @warning A persistent @ref S200_ERR_COMM after install often indicates a
 *          missing 120 Ω bus terminator at the sensor end.
 * @see    s200_measurement_t.
 */
s200_status_t s200_read_measurements(uint8_t slave_addr, s200_measurement_t *out);

#ifndef NATIVE_TEST
/**
 * @brief FreeRTOS periodic task — polls all channels on a fixed interval.
 *
 * Pass as the task function to @c xTaskCreate() or @c xTaskCreatePinnedToCore().
 * @p pvParameters must point to a fully initialised @ref s200_task_param_t.
 * The task never returns; call @c vTaskDelete(handle) to stop it.
 *
 * @par Recommended task configuration
 *   - Stack : 2048 words
 *   - Priority: 5
 *   - Core  : APP_CPU_NUM (core 1)
 *
 * @param pvParameters  Pointer to @ref s200_task_param_t (must not be NULL).
 * @warning Lifetime of @p pvParameters must outlive the task (use static
 *          storage or a long-lived heap allocation).
 * @see    s200_read_measurements(), s200_task_param_t.
 */
void s200_task(void *pvParameters);
#endif /* NATIVE_TEST */

/** @} */ /* end s200_api */
