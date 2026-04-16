#include "audio_in.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "audio_in";

#define MIC_BCLK_PIN     4
#define MIC_WS_PIN       5
#define MIC_DIN_PIN      6
#define WARMUP_READS     20
#define WARMUP_BUF_SIZE  4096

// Reader task pulls this many int32 samples per i2s_channel_read.
// 1024 samples = 64 ms @ 16 kHz.
#define READ_CHUNK_SAMPLES 1024

// Wake-word ring: ~1 s of int16 PCM = 32 KB. Sized for the wake-word
// task to fall behind by up to a second without losing samples.
#define WAKE_RING_BYTES   (AUDIO_IN_SAMPLE_RATE * sizeof(int16_t))

#define READER_TASK_STACK 4096
#define READER_TASK_PRIO  6

static i2s_chan_handle_t rx_chan = NULL;
static TaskHandle_t reader_task = NULL;
static volatile bool reader_run = false;

static RingbufHandle_t wake_ring = NULL;
static RingbufHandle_t capture_ring = NULL;
static volatile bool capture_armed = false;

// No DC tracker — INMP441 is a MEMS mic with no appreciable DC bias, and the
// old MicroPython pipeline (which produced the wake-word training data) did
// not subtract DC inline. Keeping the sample path identical avoids distribution
// drift relative to training.

static void reader_task_fn(void *arg)
{
    int32_t *raw = heap_caps_malloc(READ_CHUNK_SAMPLES * sizeof(int32_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *pcm = heap_caps_malloc(READ_CHUNK_SAMPLES * sizeof(int16_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!raw || !pcm) {
        ESP_LOGE(TAG, "reader task buffer alloc failed");
        if (raw) heap_caps_free(raw);
        if (pcm) heap_caps_free(pcm);
        reader_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "reader task started");

    while (reader_run) {
        size_t bytes_read = 0;
        i2s_channel_read(rx_chan, raw,
                          READ_CHUNK_SAMPLES * sizeof(int32_t),
                          &bytes_read, pdMS_TO_TICKS(200));
        // i2s_channel_read returns ESP_ERR_TIMEOUT (263) while still
        // delivering partial bytes — check bytes_read, not the status code.
        if (bytes_read == 0) continue;

        size_t n = bytes_read / sizeof(int32_t);
        for (size_t i = 0; i < n; i++) {
            int32_t s = raw[i] >> 8;       // 32-bit MSB-aligned → 24-bit signed
            if (s > 32767) s = 32767;
            else if (s < -32768) s = -32768;
            pcm[i] = (int16_t)s;
        }

        // Wake ring: drop on full so a stalled consumer never back-pressures
        // the I2S read loop.
        if (wake_ring) {
            xRingbufferSend(wake_ring, pcm, n * sizeof(int16_t), 0);
        }

        // Capture ring: short wait — recording must not silently drop.
        if (capture_armed && capture_ring) {
            xRingbufferSend(capture_ring, pcm, n * sizeof(int16_t),
                            pdMS_TO_TICKS(50));
        }
    }

    heap_caps_free(raw);
    heap_caps_free(pcm);
    reader_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t audio_in_init(void)
{
    if (rx_chan) return ESP_OK;

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
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_IN_SAMPLE_RATE),
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_BCLK_PIN,
            .ws   = MIC_WS_PIN,
            .din  = MIC_DIN_PIN,
            .dout = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    ret = i2s_channel_init_std_mode(rx_chan, &std_cfg);
    if (ret != ESP_OK) goto fail;

    ret = i2s_channel_enable(rx_chan);
    if (ret != ESP_OK) goto fail;

    // Warmup: discard first 20 reads to let INMP441 PLL settle.
    uint8_t *dummy = malloc(WARMUP_BUF_SIZE);
    if (dummy) {
        size_t bytes_read;
        for (int i = 0; i < WARMUP_READS; i++) {
            i2s_channel_read(rx_chan, dummy, WARMUP_BUF_SIZE, &bytes_read, portMAX_DELAY);
        }
        free(dummy);
    }

    wake_ring = xRingbufferCreateWithCaps(WAKE_RING_BYTES, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    if (!wake_ring) { ret = ESP_ERR_NO_MEM; goto fail; }

    reader_run = true;
    BaseType_t ok = xTaskCreate(reader_task_fn, "audio_in_rdr", READER_TASK_STACK,
                                 NULL, READER_TASK_PRIO, &reader_task);
    if (ok != pdPASS) { ret = ESP_ERR_NO_MEM; goto fail; }

    ESP_LOGI(TAG, "audio_in continuous: %d Hz, 32→16-bit mono, wake_ring=%u B",
             AUDIO_IN_SAMPLE_RATE, (unsigned)WAKE_RING_BYTES);
    return ESP_OK;

fail:
    if (rx_chan) {
        i2s_channel_disable(rx_chan);
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
    }
    if (wake_ring) {
        vRingbufferDeleteWithCaps(wake_ring);
        wake_ring = NULL;
    }
    return ret;
}

void audio_in_deinit(void)
{
    reader_run = false;
    while (reader_task) vTaskDelay(pdMS_TO_TICKS(10));

    if (capture_ring) {
        vRingbufferDeleteWithCaps(capture_ring);
        capture_ring = NULL;
        capture_armed = false;
    }
    if (wake_ring) {
        vRingbufferDeleteWithCaps(wake_ring);
        wake_ring = NULL;
    }
    if (rx_chan) {
        i2s_channel_disable(rx_chan);
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
        ESP_LOGI(TAG, "audio_in deinit");
    }
}

static size_t ring_drain(RingbufHandle_t ring, int16_t *buf, size_t n_samples,
                          TickType_t timeout_ticks)
{
    if (!ring || !buf || n_samples == 0) return 0;

    size_t want_bytes = n_samples * sizeof(int16_t);
    size_t got_bytes = 0;
    uint8_t *dst = (uint8_t *)buf;

    while (got_bytes < want_bytes) {
        size_t chunk_bytes = 0;
        void *p = xRingbufferReceiveUpTo(ring, &chunk_bytes, timeout_ticks,
                                          want_bytes - got_bytes);
        if (!p) break;
        memcpy(dst + got_bytes, p, chunk_bytes);
        vRingbufferReturnItem(ring, p);
        got_bytes += chunk_bytes;
        // First read may block; subsequent partial reads are non-blocking.
        timeout_ticks = 0;
    }

    return got_bytes / sizeof(int16_t);
}

size_t audio_in_consume_wake(int16_t *buf, size_t n_samples, TickType_t timeout_ticks)
{
    return ring_drain(wake_ring, buf, n_samples, timeout_ticks);
}

esp_err_t audio_in_capture_arm(size_t expected_samples)
{
    if (capture_ring) return ESP_ERR_INVALID_STATE;

    size_t bytes = expected_samples * sizeof(int16_t);
    bytes = bytes + bytes / 4;  // 25% headroom
    capture_ring = xRingbufferCreateWithCaps(bytes, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    if (!capture_ring) return ESP_ERR_NO_MEM;

    capture_armed = true;
    ESP_LOGI(TAG, "capture armed: %u samples (%u bytes)",
             (unsigned)expected_samples, (unsigned)bytes);
    return ESP_OK;
}

size_t audio_in_capture_read(int16_t *buf, size_t n_samples, TickType_t timeout_ticks)
{
    return ring_drain(capture_ring, buf, n_samples, timeout_ticks);
}

void audio_in_capture_disarm(void)
{
    capture_armed = false;
    if (capture_ring) {
        vRingbufferDeleteWithCaps(capture_ring);
        capture_ring = NULL;
        ESP_LOGI(TAG, "capture disarmed");
    }
}

esp_err_t audio_in_record(int16_t *out_buf, size_t num_samples, size_t *actual_samples)
{
    if (!rx_chan) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = audio_in_capture_arm(num_samples);
    if (ret != ESP_OK) return ret;

    uint32_t budget_ms = (uint32_t)((num_samples * 1000U) / AUDIO_IN_SAMPLE_RATE) + 5000U;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(budget_ms);
    size_t total = 0;
    int32_t peak = 0;
    int64_t ms_sum = 0;

    while (total < num_samples) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) break;
        size_t got = audio_in_capture_read(out_buf + total, num_samples - total,
                                            deadline - now);
        if (got == 0) break;
        for (size_t i = 0; i < got; i++) {
            int32_t s = out_buf[total + i];
            int32_t a = s < 0 ? -s : s;
            if (a > peak) peak = a;
            ms_sum += (int64_t)s * s;
        }
        total += got;
    }

    audio_in_capture_disarm();

    if (actual_samples) *actual_samples = total;
    int32_t ms = total ? (int32_t)(ms_sum / (int64_t)total) : 0;
    ESP_LOGI(TAG, "Recorded %u samples, peak=%ld, mean_sq=%ld",
             (unsigned)total, (long)peak, (long)ms);
    return total > 0 ? ESP_OK : ESP_FAIL;
}
