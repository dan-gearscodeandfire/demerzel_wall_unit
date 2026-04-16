#include "audio_out.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_out";

#define AMP_BCLK_PIN     15
#define AMP_WS_PIN       16
#define AMP_DOUT_PIN     7
#define AMP_SD_MODE_PIN  12

static i2s_chan_handle_t tx_chan = NULL;

static void sd_mode_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << AMP_SD_MODE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(AMP_SD_MODE_PIN, 0);
}

esp_err_t audio_out_init(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels)
{
    sd_mode_init();

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 1024;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &tx_chan, NULL);
    if (ret != ESP_OK) return ret;

    i2s_data_bit_width_t bit_width = (bits_per_sample == 32) ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT;
    i2s_slot_mode_t slot_mode = (channels >= 2) ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bit_width, slot_mode),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AMP_BCLK_PIN,
            .ws   = AMP_WS_PIN,
            .din  = I2S_GPIO_UNUSED,
            .dout = AMP_DOUT_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(tx_chan, &std_cfg);
    if (ret != ESP_OK) {
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
        return ret;
    }

    ret = i2s_channel_enable(tx_chan);
    if (ret != ESP_OK) {
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "I2S1 TX initialized: %lu Hz, %u-bit, %u ch", sample_rate, bits_per_sample, channels);
    return ESP_OK;
}

esp_err_t audio_out_write(const void *data, size_t len, size_t *bytes_written)
{
    if (!tx_chan) return ESP_ERR_INVALID_STATE;
    return i2s_channel_write(tx_chan, data, len, bytes_written, portMAX_DELAY);
}

esp_err_t audio_out_reconfigure(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels)
{
    if (!tx_chan) return ESP_ERR_INVALID_STATE;

    i2s_channel_disable(tx_chan);

    i2s_data_bit_width_t bit_width = (bits_per_sample == 32) ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT;
    i2s_slot_mode_t slot_mode = (channels >= 2) ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO;

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    esp_err_t ret = i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg);
    if (ret != ESP_OK) return ret;

    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bit_width, slot_mode);
    ret = i2s_channel_reconfig_std_slot(tx_chan, &slot_cfg);
    if (ret != ESP_OK) return ret;

    ret = i2s_channel_enable(tx_chan);
    ESP_LOGI(TAG, "I2S1 TX reconfigured: %lu Hz, %u-bit, %u ch", sample_rate, bits_per_sample, channels);
    return ret;
}

void audio_out_mute(void)
{
    gpio_set_level(AMP_SD_MODE_PIN, 0);
}

void audio_out_unmute(void)
{
    gpio_set_level(AMP_SD_MODE_PIN, 1);
}

void audio_out_deinit(void)
{
    audio_out_mute();
    if (tx_chan) {
        i2s_channel_disable(tx_chan);
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
        ESP_LOGI(TAG, "I2S1 TX deinitialized");
    }
}
