#include "ds3231.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include <string.h>
#include "esp_sntp.h"
#include <time.h>

static const char *TAG = "DS3231";

static uint8_t dec2bcd(uint8_t val)
{
    return ((val / 10) << 4) | (val % 10);
}

static uint8_t bcd2dec(uint8_t val)
{
    return ((val >> 4) * 10) + (val & 0x0F);
}

esp_err_t ds3231_set_time(const ds3231_time_t *t)
{
    uint8_t buf[7];

    buf[0] = dec2bcd(t->sec);
    buf[1] = dec2bcd(t->min);
    buf[2] = dec2bcd(t->hour);
    buf[3] = dec2bcd(t->day);
    buf[4] = dec2bcd(t->date);
    buf[5] = dec2bcd(t->month) & 0x1F;   // month, clear century
    buf[6] = dec2bcd(t->year - 2000);
    buf[3] = dec2bcd((t->day >= 1 && t->day <= 7) ? t->day : 1);



    /* 🔑 BẮT BUỘC PHẢI KHAI BÁO */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS3231_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, 0x00, true);  // start register = seconds
    i2c_master_write(cmd, buf, sizeof(buf), true);
    i2c_master_stop(cmd);

    esp_err_t err = i2c_master_cmd_begin(
        DS3231_I2C_PORT,
        cmd,
        pdMS_TO_TICKS(1000)
    );

    i2c_cmd_link_delete(cmd);

    if (err != ESP_OK) {
        ESP_LOGE("DS3231", "I2C write failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI("DS3231", "RTC time written successfully");
    return ESP_OK;
}


/* ===== SYNC RTC FROM NTP ===== */
void ds3231_sync_from_ntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");

    setenv("TZ", "ICT-7", 1);
    tzset();

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    int retry = 0;
    while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < 15) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        retry++;
    }

    time_t now;
    struct tm tm;
    time(&now);
    localtime_r(&now, &tm);

    if (tm.tm_year + 1900 >= 2023) {

        ds3231_time_t rtc = {
            .sec   = tm.tm_sec,
            .min   = tm.tm_min,
            .hour  = tm.tm_hour,
            .date  = tm.tm_mday,
            .month = tm.tm_mon + 1,
            .year  = tm.tm_year + 1900,
            .day   = (tm.tm_wday == 0) ? 7 : tm.tm_wday  // Sunday = 7
        };

        ESP_LOGI(TAG, "NTP time OK: %02d:%02d:%02d %02d/%02d/%04d",
                 rtc.hour, rtc.min, rtc.sec,
                 rtc.date, rtc.month, rtc.year);

        ds3231_set_time(&rtc);
        ESP_LOGI(TAG, "DS3231 updated from NTP");
    }
}

void ds3231_set_compile_time(void)
{
    ds3231_time_t t;

    const char *date = __DATE__; // "Jan  6 2025"
    const char *time = __TIME__; // "05:17:33"

    char month_str[4];
    sscanf(date, "%3s %hhu %hu", month_str, &t.date, &t.year);
    sscanf(time, "%hhu:%hhu:%hhu", &t.hour, &t.min, &t.sec);

    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    t.month = (strstr(months, month_str) - months) / 3 + 1;

    t.day = 1; // optional, không ảnh hưởng nhiều

    ds3231_set_time(&t);

    ESP_LOGI("DS3231", "RTC set to compile time");
}


/* ---------- Helpers ---------- */

static uint8_t bcd_to_dec(uint8_t val)
{
    return (val >> 4) * 10 + (val & 0x0F);
}

/* ---------- I2C init ---------- */

esp_err_t ds3231_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = DS3231_SDA_GPIO,
        .scl_io_num = DS3231_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = DS3231_I2C_FREQ_HZ,
    };

    ESP_ERROR_CHECK(i2c_param_config(DS3231_I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(
        DS3231_I2C_PORT,
        conf.mode,
        0,
        0,
        0
    ));

    ESP_LOGI(TAG, "DS3231 I2C initialized (SDA=%d, SCL=%d)",
             DS3231_SDA_GPIO, DS3231_SCL_GPIO);

    return ESP_OK;
}

/* ---------- Read time ---------- */

esp_err_t ds3231_get_time(ds3231_time_t *time)
{
    uint8_t reg = 0x00;
    uint8_t data[7];

    esp_err_t err = i2c_master_write_read_device(
        DS3231_I2C_PORT,
        DS3231_ADDR,
        &reg, 1,
        data, 7,
        pdMS_TO_TICKS(100)
    );
    if (err != ESP_OK) return err;

    time->sec   = bcd2dec(data[0] & 0x7F);
    time->min   = bcd2dec(data[1]);
    time->hour  = bcd2dec(data[2] & 0x3F);
    time->day   = bcd2dec(data[3]);        // 1–7
    time->date  = bcd2dec(data[4]);
    time->month = bcd2dec(data[5] & 0x1F);
    time->year  = 2000 + bcd2dec(data[6]);

    return ESP_OK;
}

