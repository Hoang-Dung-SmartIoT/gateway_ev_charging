#include "gpio_ctrl.h"

void gpio_ctrl_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask =
            (1ULL << LED_RED_GPIO) |
            (1ULL << LED_BLUE_GPIO) |
            (1ULL << LED_GREEN_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    led_red_off();
    led_green_off();
    led_blue_off();
}

/* ===== LED ===== */
void led_red_on(void)    { gpio_set_level(LED_RED_GPIO, 1); }
void led_red_off(void)   { gpio_set_level(LED_RED_GPIO, 0); }

void led_green_on(void)  { gpio_set_level(LED_GREEN_GPIO, 1); }
void led_green_off(void) { gpio_set_level(LED_GREEN_GPIO, 0); }

void led_blue_on(void)   { gpio_set_level(LED_BLUE_GPIO, 1); }
void led_blue_off(void)  { gpio_set_level(LED_BLUE_GPIO, 0); }

/* ===== GENERIC ===== */
void gpio_set_output(gpio_num_t gpio, int level)
{
    gpio_set_level(gpio, level);
}
