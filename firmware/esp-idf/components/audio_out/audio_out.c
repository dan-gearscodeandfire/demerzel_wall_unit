#include "audio_out.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "audio_out";

#define AMP_BCLK_PIN     15
#define AMP_WS_PIN       16
#define AMP_DOUT_PIN     7
#define AMP_SD_MODE_PIN  12

static i2s_chan_handle_t tx_chan = NULL;

// --- Streaming session state ---
static StreamBufferHandle_t s_stream = NULL;
static TaskHandle_t          s_writer_task = NULL;
static volatile bool         s_stream_active = false;
static volatile bool         s_stream_stop_request = false;
static uint32_t              s_stream_sample_rate = 0;
static uint8_t               s_stream_bits = 16;
static uint8_t               s_stream_channels = 1;
static uint32_t              s_underrun_count = 0;
static SemaphoreHandle_t     s_drain_done = NULL;

// Silence padding: 20 ms at 16 kHz mono int16 = 640 bytes. Small enough to
// not audibly over-pad, large enough that the DMA never starves.
#define STREAM_SILENCE_PAD_MS     20
#define STREAM_READ_TIMEOUT_MS    STREAM_SILENCE_PAD_MS

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

// --- Streaming playback ---

static void _writer_task(void *arg)
{
    (void)arg;
    const TickType_t read_timeout = pdMS_TO_TICKS(STREAM_READ_TIMEOUT_MS);
    const size_t pad_samples = (s_stream_sample_rate * STREAM_SILENCE_PAD_MS) / 1000;
    const size_t pad_bytes = pad_samples * (s_stream_bits / 8) * s_stream_channels;
    const size_t read_chunk = 2048;  // balance latency vs. syscall overhead

    // One pre-zeroed silence buffer on the heap, reused every underrun.
    uint8_t *silence = heap_caps_calloc(1, pad_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *scratch = heap_caps_malloc(read_chunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!silence || !scratch) {
        ESP_LOGE(TAG, "writer_task alloc failed (silence=%p scratch=%p)",
                 silence, scratch);
        if (silence) heap_caps_free(silence);
        if (scratch) heap_caps_free(scratch);
        s_writer_task = NULL;
        xSemaphoreGive(s_drain_done);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "writer_task: started (pad=%ums / %u bytes)",
             STREAM_SILENCE_PAD_MS, (unsigned)pad_bytes);

    for (;;) {
        size_t got = xStreamBufferReceive(s_stream, scratch, read_chunk, read_timeout);
        if (got > 0) {
            size_t written = 0;
            i2s_channel_write(tx_chan, scratch, got, &written, portMAX_DELAY);
            if (written != got) {
                ESP_LOGW(TAG, "short i2s write: %u of %u", (unsigned)written, (unsigned)got);
            }
            continue;
        }

        // Read timed out with nothing available. If we've been asked to stop
        // AND the buffer is empty, exit. Otherwise pad silence and keep the
        // DMA fed.
        if (s_stream_stop_request &&
                xStreamBufferBytesAvailable(s_stream) == 0) {
            break;
        }
        s_underrun_count++;
        size_t written = 0;
        i2s_channel_write(tx_chan, silence, pad_bytes, &written, portMAX_DELAY);
    }

    heap_caps_free(silence);
    heap_caps_free(scratch);

    ESP_LOGI(TAG, "writer_task: stopped (underruns=%lu)",
             (unsigned long)s_underrun_count);

    s_writer_task = NULL;
    xSemaphoreGive(s_drain_done);
    vTaskDelete(NULL);
}

esp_err_t audio_out_stream_begin(uint32_t sample_rate, uint8_t bits_per_sample,
                                 uint8_t channels, size_t jitter_bytes)
{
    if (s_stream_active) return ESP_ERR_INVALID_STATE;
    if (jitter_bytes < 1024) jitter_bytes = 1024;

    // A StreamBuffer's trigger level controls when Receive wakes; we set 1 so
    // any arrival unblocks us promptly.
    s_stream = xStreamBufferCreate(jitter_bytes, 1);
    if (!s_stream) return ESP_ERR_NO_MEM;

    s_drain_done = xSemaphoreCreateBinary();
    if (!s_drain_done) {
        vStreamBufferDelete(s_stream);
        s_stream = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = audio_out_init(sample_rate, bits_per_sample, channels);
    if (ret != ESP_OK) {
        vStreamBufferDelete(s_stream);
        vSemaphoreDelete(s_drain_done);
        s_stream = NULL;
        s_drain_done = NULL;
        return ret;
    }
    audio_out_unmute();

    s_stream_sample_rate = sample_rate;
    s_stream_bits = bits_per_sample;
    s_stream_channels = channels;
    s_stream_stop_request = false;
    s_underrun_count = 0;
    s_stream_active = true;

    BaseType_t ok = xTaskCreatePinnedToCore(_writer_task, "audio_stream_wr",
                                             4096, NULL, 10, &s_writer_task, 1);
    if (ok != pdPASS) {
        s_stream_active = false;
        audio_out_deinit();
        vStreamBufferDelete(s_stream);
        vSemaphoreDelete(s_drain_done);
        s_stream = NULL;
        s_drain_done = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "stream begin: %lu Hz %u-bit %u ch, jitter=%u bytes",
             sample_rate, bits_per_sample, channels, (unsigned)jitter_bytes);
    return ESP_OK;
}

esp_err_t audio_out_stream_push(const void *pcm, size_t len,
                                TickType_t timeout_ticks)
{
    if (!s_stream_active || !s_stream) return ESP_ERR_INVALID_STATE;
    size_t sent = xStreamBufferSend(s_stream, pcm, len, timeout_ticks);
    if (sent != len) {
        ESP_LOGW(TAG, "stream_push short: %u of %u", (unsigned)sent, (unsigned)len);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t audio_out_stream_end(TickType_t drain_timeout_ticks)
{
    if (!s_stream_active) return ESP_ERR_INVALID_STATE;

    s_stream_stop_request = true;

    // Block until the writer task exits (or timeout — in which case it's still
    // running but we proceed with teardown, which will be ugly but bounded).
    BaseType_t ok = xSemaphoreTake(s_drain_done, drain_timeout_ticks);
    if (ok != pdTRUE) {
        ESP_LOGW(TAG, "stream_end: writer didn't drain in %lu ms",
                 (unsigned long)(drain_timeout_ticks * portTICK_PERIOD_MS));
    }

    s_stream_active = false;
    if (s_stream) {
        vStreamBufferDelete(s_stream);
        s_stream = NULL;
    }
    if (s_drain_done) {
        vSemaphoreDelete(s_drain_done);
        s_drain_done = NULL;
    }

    // Short delay to let the last DMA descriptors flush through the amp.
    vTaskDelay(pdMS_TO_TICKS(50));

    audio_out_mute();
    audio_out_deinit();
    return ESP_OK;
}

bool audio_out_stream_is_active(void)
{
    return s_stream_active;
}

uint32_t audio_out_stream_underrun_count(void)
{
    return s_underrun_count;
}
