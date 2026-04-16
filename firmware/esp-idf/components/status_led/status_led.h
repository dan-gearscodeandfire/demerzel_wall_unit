#pragma once
#include "esp_err.h"
#include <stdint.h>

typedef enum {
    LED_OFF,
    LED_AMBER,
    LED_RED,
    LED_BLUE,
    LED_GREEN,
    LED_WHITE,
} led_state_t;

esp_err_t status_led_init(void);
void      status_led_set(led_state_t state);
void      status_led_set_rgb(uint8_t r, uint8_t g, uint8_t b);
