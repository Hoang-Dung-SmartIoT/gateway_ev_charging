#pragma once

#include <stdint.h>
#include "esp_err.h"

/* I2C config */
#define DS3231_I2C_PORT     I2C_NUM_0
#define DS3231_SDA_GPIO     4
#define DS3231_SCL_GPIO     5
#define DS3231_I2C_FREQ_HZ  100000

#define DS3231_ADDR         0x68

/* Time struct */
typedef struct {
    uint8_t sec;
    uint8_t min;
    uint8_t hour;
    uint8_t day;
    uint8_t date;
    uint8_t month;
    uint16_t year;
} ds3231_time_t;

/* API */
esp_err_t ds3231_init(void);
esp_err_t ds3231_get_time(ds3231_time_t *time);
void ds3231_set_compile_time();
void ds3231_sync_from_ntp(void);
