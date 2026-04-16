#include "voice_turn.h"
#include "audio_in.h"
#include "audio_out.h"
#include "status_led.h"
#include "wav_util.h"
#include "http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

// Generate a short tone (sine wave) and play via audio_out.
// freq_hz: tone frequency, duration_ms: length, rate: sample rate.
static void beep(int freq_hz, int duration_ms, int rate)
{
    int num_samples = (rate * duration_ms) / 1000;
    int16_t *buf = heap_caps_malloc(num_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) return;
    for (int i = 0; i < num_samples; i++) {
        float t = (float)i / (float)rate;
        float env = 1.0f;
        // Fade-in/out to avoid clicks (10ms each)
        int fade = rate / 100;
        if (i < fade) env = (float)i / (float)fade;
        else if (i > num_samples - fade) env = (float)(num_samples - i) / (float)fade;
        buf[i] = (int16_t)(env * 8000.0f * sinf(2.0f * 3.14159265f * freq_hz * t));
    }
    audio_out_init(rate, 16, 1);
    audio_out_unmute();
    size_t written;
    audio_out_write(buf, num_samples * sizeof(int16_t), &written);
    // Wait for DMA to finish pushing out the samples before muting/deiniting
    vTaskDelay(pdMS_TO_TICKS(duration_ms + 100));
    audio_out_mute();
    audio_out_deinit();
    heap_caps_free(buf);
}

static const char *TAG = "voice_turn";

#define RECORD_RATE    16000
#define PLAYBACK_CHUNK 4096

esp_err_t voice_turn_execute(void)
{
    int64_t t_start = esp_timer_get_time();
    esp_err_t ret;

    // Brief heads-up + audible "start" beep (higher pitch)
    status_led_set(LED_AMBER);
    ESP_LOGI(TAG, "PIR triggered — beep + recording starts...");
    beep(1200, 200, 16000);  // 1200 Hz, 200 ms
    vTaskDelay(pdMS_TO_TICKS(150));

    // Record
    status_led_set(LED_RED);
    ESP_LOGI(TAG, ">>> RECORDING %d SECONDS <<<", CONFIG_DWU_RECORD_SECONDS);

    size_t num_samples = RECORD_RATE * CONFIG_DWU_RECORD_SECONDS;
    int16_t *pcm = heap_caps_malloc(num_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) {
        ESP_LOGE(TAG, "Failed to allocate PCM buffer (%u bytes)", (unsigned)(num_samples * 2));
        status_led_set(LED_RED);
        return ESP_ERR_NO_MEM;
    }

    ret = audio_in_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio_in_init failed: %s", esp_err_to_name(ret));
        heap_caps_free(pcm);
        status_led_set(LED_RED);
        return ret;
    }

    size_t actual = 0;
    int64_t t_rec_start = esp_timer_get_time();
    ret = audio_in_record(pcm, num_samples, &actual);
    audio_in_deinit();

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio_in_record failed: %s", esp_err_to_name(ret));
        heap_caps_free(pcm);
        status_led_set(LED_RED);
        return ret;
    }

    int rec_ms = (int)((esp_timer_get_time() - t_rec_start) / 1000);
    ESP_LOGI(TAG, "Recorded %u samples (%u bytes) in %d ms", (unsigned)actual, (unsigned)(actual * 2), rec_ms);

    // Audible "stop" beep (lower pitch) so user knows recording ended
    beep(600, 150, 16000);  // 600 Hz, 150 ms

    // Process: wrap WAV, POST to server
    status_led_set(LED_BLUE);
    ESP_LOGI(TAG, "Uploading + processing on okDemerzel...");

    uint8_t *wav_in = NULL;
    size_t wav_in_len = 0;
    ret = wav_wrap(pcm, actual, RECORD_RATE, &wav_in, &wav_in_len);
    heap_caps_free(pcm);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wav_wrap failed");
        status_led_set(LED_RED);
        return ret;
    }

    uint8_t *wav_out = NULL;
    size_t wav_out_len = 0;
    voice_turn_meta_t meta;

    int64_t t_http_start = esp_timer_get_time();
    ret = http_post_voice_turn(wav_in, wav_in_len, &wav_out, &wav_out_len, &meta);
    heap_caps_free(wav_in);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(ret));
        status_led_set(LED_RED);
        return ret;
    }

    int http_ms = (int)((esp_timer_get_time() - t_http_start) / 1000);
    ESP_LOGI(TAG, "Round-trip %d ms, response %u bytes", http_ms, (unsigned)wav_out_len);
    ESP_LOGI(TAG, "Heard:    %s", meta.transcript);
    ESP_LOGI(TAG, "Replying: %s", meta.reply_text);
    ESP_LOGI(TAG, "Server:   %d ms", meta.latency_ms);

    // Play response
    status_led_set(LED_GREEN);
    ESP_LOGI(TAG, "Playing response...");

    wav_header_info_t wav_info;
    ret = wav_parse(wav_out, wav_out_len, &wav_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse response WAV");
        heap_caps_free(wav_out);
        status_led_set(LED_RED);
        return ret;
    }

    ret = audio_out_init(wav_info.sample_rate, wav_info.bits_per_sample, wav_info.channels);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio_out_init failed: %s", esp_err_to_name(ret));
        heap_caps_free(wav_out);
        status_led_set(LED_RED);
        return ret;
    }

    audio_out_unmute();

    size_t pos = wav_info.data_offset;
    size_t end = wav_info.data_offset + wav_info.data_size;
    if (end > wav_out_len) end = wav_out_len;

    while (pos < end) {
        size_t chunk = (end - pos > PLAYBACK_CHUNK) ? PLAYBACK_CHUNK : (end - pos);
        size_t written;
        audio_out_write(wav_out + pos, chunk, &written);
        pos += written;
    }

    audio_out_mute();
    audio_out_deinit();
    heap_caps_free(wav_out);

    status_led_set(LED_OFF);

    int total_ms = (int)((esp_timer_get_time() - t_start) / 1000);
    ESP_LOGI(TAG, "=== Total wall time: %d ms ===", total_ms);

    return ESP_OK;
}
