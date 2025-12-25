#pragma once
#include "driver/gpio.h"

/* ===== GPIO DEFINE ===== */
#define LED_RED_GPIO     13
#define LED_BLUE_GPIO    14
#define LED_GREEN_GPIO   21

/* ===== INIT ===== */
void gpio_ctrl_init(void);

/* ===== LED API ===== */
void led_red_on(void);
void led_red_off(void);

void led_green_on(void);
void led_green_off(void);

void led_blue_on(void);
void led_blue_off(void);

/* ===== GENERIC OUTPUT ===== */
void gpio_set_output(gpio_num_t gpio, int level);
