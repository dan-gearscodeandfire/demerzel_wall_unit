#include "audio_in.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "audio_in";

#define MIC_BCLK_PIN     4
#define MIC_WS_PIN       5
#define MIC_DIN_PIN      6
#define SAMPLE_RATE      16000
#define WARMUP_READS     20
#define WARMUP_BUF_SIZE  4096

static i2s_chan_handle_t rx_chan = NULL;

esp_err_t audio_in_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 1024;

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &rx_chan);
    if (ret != ESP_OK) return ret;

    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    // INMP441 drives the LEFT channel when its L/R pin is tied low.
    // Without overriding this, the default slot_mask captures BOTH channels
    // interleaved, producing alternating real-audio / noise-floor samples.
    slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_BCLK_PIN,
            .ws   = MIC_WS_PIN,
            .din  = MIC_DIN_PIN,
            .dout = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(rx_chan, &std_cfg);
    if (ret != ESP_OK) {
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
        return ret;
    }

    ret = i2s_channel_enable(rx_chan);
    if (ret != ESP_OK) {
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
        return ret;
    }

    // Warmup: discard first 20 reads to let INMP441 PLL settle
    uint8_t *dummy = malloc(WARMUP_BUF_SIZE);
    if (dummy) {
        size_t bytes_read;
        for (int i = 0; i < WARMUP_READS; i++) {
            i2s_channel_read(rx_chan, dummy, WARMUP_BUF_SIZE, &bytes_read, portMAX_DELAY);
        }
        free(dummy);
    }

    ESP_LOGI(TAG, "I2S0 RX initialized: %d Hz, 32-bit mono", SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t audio_in_record(int16_t *out_buf, size_t num_samples, size_t *actual_samples)
{
    if (!rx_chan) return ESP_ERR_INVALID_STATE;

    size_t raw_size = num_samples * sizeof(int32_t);
    int32_t *raw = heap_caps_malloc(raw_size, MALLOC_CAP_SPIRAM);
    if (!raw) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes PSRAM for raw capture", (unsigned)raw_size);
        return ESP_ERR_NO_MEM;
    }

    // Read raw 32-bit samples
    size_t total_read = 0;
    size_t bytes_read;
    uint8_t *dst = (uint8_t *)raw;
    while (total_read < raw_size) {
        esp_err_t ret = i2s_channel_read(rx_chan, dst + total_read,
                                          raw_size - total_read, &bytes_read, portMAX_DELAY);
        if (ret != ESP_OK) {
            heap_caps_free(raw);
            return ret;
        }
        total_read += bytes_read;
    }

    size_t n = total_read / sizeof(int32_t);

    // First pass: compute DC offset
    int64_t dc_sum = 0;
    for (size_t i = 0; i < n; i++) {
        dc_sum += (raw[i] >> 8);
    }
    int32_t dc = (int32_t)(dc_sum / (int64_t)n);

    // Second pass: DC removal + clamp to int16
    for (size_t i = 0; i < n; i++) {
        int32_t s = (raw[i] >> 8) - dc;
        if (s > 32767) s = 32767;
        else if (s < -32768) s = -32768;
        out_buf[i] = (int16_t)s;
    }

    // Peak amplitude for diagnostics (skip sqrt — just log mean-square)
    int64_t ms_sum = 0;
    int32_t peak = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t s = out_buf[i];
        if (s < 0) s = -s;
        if (s > peak) peak = s;
        ms_sum += (int64_t)out_buf[i] * out_buf[i];
    }
    int32_t ms = (int32_t)(ms_sum / (int64_t)n);

    heap_caps_free(raw);
    if (actual_samples) *actual_samples = n;

    ESP_LOGI(TAG, "Recorded %u samples, DC=%ld, peak=%ld, mean_sq=%ld",
             (unsigned)n, (long)dc, (long)peak, (long)ms);
    return ESP_OK;
}

void audio_in_deinit(void)
{
    if (rx_chan) {
        i2s_channel_disable(rx_chan);
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
        ESP_LOGI(TAG, "I2S0 RX deinitialized");
    }
}
