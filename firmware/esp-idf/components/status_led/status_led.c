#include "status_led.h"
#include "led_strip.h"
#include "esp_log.h"

static const char *TAG = "status_led";

#define LED_GPIO 48

static led_strip_handle_t strip = NULL;

esp_err_t status_led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,   /* 10 MHz */
        .flags.with_dma = false,
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &strip);
    if (ret != ESP_OK) return ret;

    led_strip_clear(strip);
    ESP_LOGI(TAG, "WS2812 initialized on GPIO%d", LED_GPIO);
    return ESP_OK;
}

void status_led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!strip) return;
    led_strip_set_pixel(strip, 0, r, g, b);
    led_strip_refresh(strip);
}

void status_led_set(led_state_t state)
{
    switch (state) {
        case LED_OFF:   status_led_set_rgb(0, 0, 0);       break;
        case LED_AMBER: status_led_set_rgb(60, 30, 0);     break;
        case LED_RED:   status_led_set_rgb(120, 0, 0);     break;
        case LED_BLUE:  status_led_set_rgb(0, 0, 100);     break;
        case LED_GREEN: status_led_set_rgb(0, 100, 0);     break;
        case LED_WHITE: status_led_set_rgb(60, 60, 60);    break;
    }
}
