#ifndef UNIT_TEST
  #include <Arduino.h>
  #include "i2c_bus.h"
#else
  #include "../test/mock_i2c_bus.h"
#endif

#include "ds1307_rtc.h"

/* ---------------------------------------------------------------------------
 * DS3231 register addresses
 * --------------------------------------------------------------------------- */
#define REG_SECONDS   0x00   /* bit 7 = CH (Clock Halt) */

/* ---------------------------------------------------------------------------
 * BCD helpers (file-scope)
 * --------------------------------------------------------------------------- */

static uint8_t bcd_to_dec(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) & 0x0F) * 10u + (bcd & 0x0F));
}

static uint8_t dec_to_bcd(uint8_t dec)
{
    return (uint8_t)(((dec / 10u) << 4) | (dec % 10u));
}

/* ---------------------------------------------------------------------------
 * API implementation
 * --------------------------------------------------------------------------- */

rtc_status_t rtc_init(void)
{
    i2c_status_t st = i2c_write(DS1307_I2C_ADDR, NULL, 0);
    if (st == I2C_ERR_NACK) {
        return RTC_ERR_NO_DEVICE;
    }
    return (st == I2C_OK) ? RTC_OK : RTC_ERR_COMM;
}

rtc_status_t rtc_get_time(rtc_datetime_t *dt)
{
    uint8_t reg = REG_SECONDS;
    uint8_t buf[7];

    i2c_status_t st = i2c_write_read(DS1307_I2C_ADDR, &reg, 1, buf, 7);
    if (st != I2C_OK) {
        return RTC_ERR_COMM;
    }

    /* Mask unused/mode bits before BCD decode */
    uint8_t raw_sec  = buf[0] & 0x7F; /* bit 7 unused */
    uint8_t raw_min  = buf[1];
    uint8_t raw_hour = buf[2] & 0x3F; /* bit 6 = 12/24 mode; assume 24h */
    uint8_t raw_dow  = buf[3] & 0x07;
    uint8_t raw_day  = buf[4];
    uint8_t raw_mon  = buf[5] & 0x1F; /* bit 7 = century, not used here */
    uint8_t raw_yr   = buf[6];

    uint8_t sec  = bcd_to_dec(raw_sec);
    uint8_t min  = bcd_to_dec(raw_min);
    uint8_t hour = bcd_to_dec(raw_hour);
    uint8_t dow  = bcd_to_dec(raw_dow);
    uint8_t day  = bcd_to_dec(raw_day);
    uint8_t mon  = bcd_to_dec(raw_mon);
    uint8_t yr   = bcd_to_dec(raw_yr);

    /* Validate decoded values */
    if (sec  > 59 || min  > 59 || hour > 23 ||
        dow  < 1  || dow  > 7  ||
        day  < 1  || day  > 31 ||
        mon  < 1  || mon  > 12) {
        return RTC_ERR_INVALID;
    }

    dt->second      = sec;
    dt->minute      = min;
    dt->hour        = hour;
    dt->day_of_week = dow;
    dt->day         = day;
    dt->month       = mon;
    dt->year        = 2000u + (uint16_t)yr;

    return RTC_OK;
}

rtc_status_t rtc_set_time(const rtc_datetime_t *dt)
{
    uint8_t buf[8];
    buf[0] = REG_SECONDS;                              /* register pointer */
    buf[1] = dec_to_bcd(dt->second);
    buf[2] = dec_to_bcd(dt->minute);
    buf[3] = dec_to_bcd(dt->hour);                    /* 24-hour, bit 6 = 0 */
    buf[4] = dec_to_bcd(dt->day_of_week);
    buf[5] = dec_to_bcd(dt->day);
    buf[6] = dec_to_bcd(dt->month);                   /* century bit = 0 */
    buf[7] = dec_to_bcd((uint8_t)(dt->year - 2000u));

    /* Writing valid BCD seconds (0–59) always produces a value with bit 7 = 0,
     * which automatically clears the DS1307 CH bit and starts the oscillator. */
    i2c_status_t st = i2c_write(DS1307_I2C_ADDR, buf, 8);
    return (st == I2C_OK) ? RTC_OK : RTC_ERR_COMM;
}

bool rtc_oscillator_stopped(void)
{
    /* The CH bit is bit 7 of the seconds register (0x00) on the DS1307. */
    uint8_t reg = REG_SECONDS;
    uint8_t seconds = 0;
    i2c_write_read(DS1307_I2C_ADDR, &reg, 1, &seconds, 1);
    return (seconds & 0x80u) != 0u;
}
