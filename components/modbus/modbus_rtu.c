#include "modbus_rtu.h"
#include "modbus_crc.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>

#define TAG "MODBUS"

/* ===== USER CONFIG ===== */
#define UART_MB         UART_NUM_1
#define UART_TX         17
#define UART_RX         16
#define UART_DE         18
#define BAUDRATE        9600

#define BUF_SIZE        256
/* ======================= */

static uint8_t g_slave_id;

/* RS485 direction */
static inline void rs485_tx(void)
{
    gpio_set_level(UART_DE, 1);
}
static inline void rs485_rx(void)
{
    gpio_set_level(UART_DE, 0);
}

/* ===== INIT ===== */
void modbus_rtu_init(uint8_t slave_id)
{
    g_slave_id = slave_id;

    uart_config_t cfg = {
        .baud_rate = BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };

    uart_driver_install(UART_MB, BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_MB, &cfg);
    uart_set_pin(UART_MB, UART_TX, UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << UART_DE,
        .mode = GPIO_MODE_OUTPUT
    };
    gpio_config(&io);

    rs485_rx();
    ESP_LOGI(TAG, "Modbus RTU init ID=%d", g_slave_id);
}
