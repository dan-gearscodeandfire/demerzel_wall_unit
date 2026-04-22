#include "voice_turn.h"
#include "audio_in.h"
#include "audio_out.h"
#include "status_led.h"
#include "wav_util.h"
#include "http_client.h"
#include "ws_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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

// Descending two-tone "something went wrong" pattern. Audible feedback for
// any voice_turn error path (HTTP timeout, WAV parse, etc).
static void beep_error(void)
{
    beep(600, 150, 16000);
    vTaskDelay(pdMS_TO_TICKS(80));
    beep(400, 250, 16000);
}

static const char *TAG = "voice_turn";

#define RECORD_RATE    16000
#define PLAYBACK_CHUNK 4096

// Jitter buffer for streaming TTS playback. 200 ms at 16 kHz mono int16 =
// 6400 bytes. Kconfig override lands in Kconfig.projbuild.
#ifndef CONFIG_DWU_TTS_JITTER_MS
#define CONFIG_DWU_TTS_JITTER_MS 200
#endif

// Streaming TTS handler state — only one turn streams at a time, so plain
// statics suffice. The handler runs on the ws_client event-handler task and
// must not block; audio_out_stream_push is a StreamBuffer send.
static bool       s_tts_stream_opened = false;
static SemaphoreHandle_t s_tts_registered_mutex = NULL;

static void _tts_stream_handler(const ws_tts_event_t *evt, void *ctx)
{
    (void)ctx;
    esp_err_t ret;
    switch (evt->type) {
    case WS_TTS_EVT_START: {
        if (s_tts_stream_opened) {
            ESP_LOGW(TAG, "tts_start but stream already open — tearing down");
            audio_out_stream_end(pdMS_TO_TICKS(100));
            s_tts_stream_opened = false;
        }
        size_t jitter = (evt->sample_rate * CONFIG_DWU_TTS_JITTER_MS / 1000)
                        * 2 /* int16 */ * evt->channels;
        ret = audio_out_stream_begin((uint32_t)evt->sample_rate, 16,
                                     (uint8_t)evt->channels, jitter);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "audio_out_stream_begin failed: %s",
                     esp_err_to_name(ret));
            return;
        }
        s_tts_stream_opened = true;
        break;
    }
    case WS_TTS_EVT_CHUNK: {
        if (!s_tts_stream_opened) {
            ESP_LOGW(TAG, "tts_chunk before tts_start, dropping");
            return;
        }
        // Short timeout: if the ring is full we'd rather drop this chunk and
        // log an underflow than block the ws event task (which would stop
        // processing subsequent frames). In practice the ring + writer keep
        // up with a 16 kHz stream trivially.
        ret = audio_out_stream_push(evt->pcm, evt->pcm_len, pdMS_TO_TICKS(100));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "stream_push dropped seq=%d (%u bytes): %s",
                     evt->seq, (unsigned)evt->pcm_len, esp_err_to_name(ret));
        }
        break;
    }
    case WS_TTS_EVT_END: {
        if (!s_tts_stream_opened) return;
        ret = audio_out_stream_end(pdMS_TO_TICKS(5000));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "stream_end returned %s", esp_err_to_name(ret));
        }
        s_tts_stream_opened = false;
        break;
    }
    }
}

// Register the TTS stream handler once. Called at the first two-phase turn.
static void _ensure_tts_handler_registered(void)
{
    if (!s_tts_registered_mutex) {
        s_tts_registered_mutex = xSemaphoreCreateMutex();
    }
    static bool registered = false;
    xSemaphoreTake(s_tts_registered_mutex, portMAX_DELAY);
    if (!registered) {
        ws_client_set_tts_handler(_tts_stream_handler, NULL);
        registered = true;
    }
    xSemaphoreGive(s_tts_registered_mutex);
}

static esp_err_t play_wav(const uint8_t *wav, size_t wav_len)
{
    wav_header_info_t info;
    esp_err_t ret = wav_parse(wav, wav_len, &info);
    if (ret != ESP_OK) return ret;

    ret = audio_out_init(info.sample_rate, info.bits_per_sample, info.channels);
    if (ret != ESP_OK) return ret;

    audio_out_unmute();
    size_t pos = info.data_offset;
    size_t end = info.data_offset + info.data_size;
    if (end > wav_len) end = wav_len;

    while (pos < end) {
        size_t chunk = (end - pos > PLAYBACK_CHUNK) ? PLAYBACK_CHUNK : (end - pos);
        size_t written;
        audio_out_write(wav + pos, chunk, &written);
        pos += written;
    }

    audio_out_mute();
    audio_out_deinit();
    return ESP_OK;
}

esp_err_t voice_turn_execute(void)
{
    int64_t t_start = esp_timer_get_time();
    esp_err_t ret;

    // Brief heads-up + audible "start" beep (higher pitch)
    status_led_set(LED_AMBER);
    ESP_LOGI(TAG, "Wake word detected — beep + recording starts...");
    beep(1200, 200, 16000);  // 1200 Hz, 200 ms
    vTaskDelay(pdMS_TO_TICKS(150));

    // Record
    status_led_set(LED_RED);
    ws_client_send_state("recording", NULL);
    ESP_LOGI(TAG, ">>> RECORDING %d SECONDS <<<", CONFIG_DWU_RECORD_SECONDS);

    size_t num_samples = RECORD_RATE * CONFIG_DWU_RECORD_SECONDS;
    int16_t *pcm = heap_caps_malloc(num_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) {
        ESP_LOGE(TAG, "Failed to allocate PCM buffer (%u bytes)", (unsigned)(num_samples * 2));
        status_led_set(LED_RED);
        beep_error();
        return ESP_ERR_NO_MEM;
    }

    size_t actual = 0;
    int64_t t_rec_start = esp_timer_get_time();
    ret = audio_in_record(pcm, num_samples, &actual);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio_in_record failed: %s", esp_err_to_name(ret));
        heap_caps_free(pcm);
        status_led_set(LED_RED);
        beep_error();
        return ret;
    }

    int rec_ms = (int)((esp_timer_get_time() - t_rec_start) / 1000);
    ESP_LOGI(TAG, "Recorded %u samples (%u bytes) in %d ms", (unsigned)actual, (unsigned)(actual * 2), rec_ms);

    // Audible "stop" beep (lower pitch) so user knows recording ended
    beep(600, 150, 16000);  // 600 Hz, 150 ms

    // Process: wrap WAV, POST to server
    status_led_set(LED_BLUE);
    ws_client_send_state("uploading", NULL);
    ESP_LOGI(TAG, "Uploading + processing on okDemerzel...");

    uint8_t *wav_in = NULL;
    size_t wav_in_len = 0;
    ret = wav_wrap(pcm, actual, RECORD_RATE, &wav_in, &wav_in_len);
    heap_caps_free(pcm);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wav_wrap failed");
        status_led_set(LED_RED);
        beep_error();
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
        beep_error();
        return ret;
    }

    int http_ms = (int)((esp_timer_get_time() - t_http_start) / 1000);
    ESP_LOGI(TAG, "Round-trip %d ms, response %u bytes", http_ms, (unsigned)wav_out_len);
    ESP_LOGI(TAG, "Heard:    %s", meta.transcript);
    ESP_LOGI(TAG, "Replying: %s", meta.reply_text);
    ESP_LOGI(TAG, "Server:   %d ms", meta.latency_ms);

    // --- Play response (single-phase or two-phase) ---

    if (meta.pending_id[0] != '\0') {
        // TWO-PHASE: play the ack first, then either stream the real answer
        // over WS (fast path) or fall back to GET /voice_result (compat path).
        ESP_LOGI(TAG, "Two-phase: playing ack (pending_id=%s)...", meta.pending_id);

        // Arm both TTS streaming AND pending_ready expectations BEFORE playing
        // the ack — server may finish synthesis during ack playback and push
        // tts_start immediately.
        _ensure_tts_handler_registered();
        ws_client_expect_tts_stream(meta.pending_id);
        ws_client_expect_pending_ready(meta.pending_id);

        status_led_set(LED_GREEN);
        ws_client_send_state("playing", meta.pending_id);
        ret = play_wav(wav_out, wav_out_len);
        heap_caps_free(wav_out);
        wav_out = NULL;

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to play ack WAV");
            status_led_set(LED_RED);
            beep_error();
            ws_client_expect_tts_stream(NULL);
            return ret;
        }

        // Wait for tts_end to fire (stream handler drains audio_out in
        // parallel). Typical completion within 1-3 s of ack end for a
        // multi-sentence reply. Hard cap of 35 s for the pathological slow
        // case.
        int64_t t_stream_wait = esp_timer_get_time();
        esp_err_t stream_ret = ws_client_wait_tts_end(35000);
        int stream_ms = (int)((esp_timer_get_time() - t_stream_wait) / 1000);

        if (stream_ret == ESP_OK) {
            ESP_LOGI(TAG, "Real answer streamed over WS (wait=%d ms, underruns=%lu)",
                     stream_ms,
                     (unsigned long)audio_out_stream_underrun_count());
        } else {
            // Stream didn't complete — clear expectation and fall back to HTTP.
            ws_client_expect_tts_stream(NULL);
            if (s_tts_stream_opened) {
                audio_out_stream_end(pdMS_TO_TICKS(500));
                s_tts_stream_opened = false;
            }
            ESP_LOGW(TAG, "TTS stream timed out after %d ms, falling back to GET /voice_result",
                     stream_ms);

            status_led_set(LED_BLUE);
            ws_client_send_state("uploading", meta.pending_id);

            uint8_t *real_wav = NULL;
            size_t real_wav_len = 0;
            voice_turn_meta_t real_meta;

            int64_t t_fetch = esp_timer_get_time();
            ret = http_get_voice_result(meta.pending_id, &real_wav, &real_wav_len, &real_meta);
            int fetch_ms = (int)((esp_timer_get_time() - t_fetch) / 1000);

            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to fetch real answer (%d ms): %s",
                         fetch_ms, esp_err_to_name(ret));
                status_led_set(LED_RED);
                beep_error();
                return ret;
            }

            ESP_LOGI(TAG, "Fallback real answer: %d ms, %u bytes — %s",
                     fetch_ms, (unsigned)real_wav_len, real_meta.reply_text);

            status_led_set(LED_GREEN);
            ws_client_send_state("playing", meta.pending_id);
            ret = play_wav(real_wav, real_wav_len);
            heap_caps_free(real_wav);

            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to play real answer WAV");
                status_led_set(LED_RED);
                beep_error();
                return ret;
            }
        }
    } else {
        // SINGLE-PHASE: play the response directly.
        status_led_set(LED_GREEN);
        ws_client_send_state("playing", NULL);
        ESP_LOGI(TAG, "Playing response...");
        ret = play_wav(wav_out, wav_out_len);
        heap_caps_free(wav_out);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to play response WAV");
            status_led_set(LED_RED);
            beep_error();
            return ret;
        }
    }

    status_led_set(LED_OFF);

    int total_ms = (int)((esp_timer_get_time() - t_start) / 1000);
    ESP_LOGI(TAG, "=== Total wall time: %d ms ===", total_ms);

    return ESP_OK;
}
