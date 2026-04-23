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

// esp_vad.h intentionally not included — WebRTC-port VAD mis-classifies
// ambient motor/HVAC noise as speech at every mode. Capture is now
// energy-threshold-based. esp-sr dep is retained for future AFE work.

#if CONFIG_DWU_ENCODE_OPUS
#include "opus_stream.h"
#endif

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

// Max samples allocated for a single utterance. With VAD the utterance
// ends early at end-of-speech; without VAD this is the full fixed window.
#if CONFIG_DWU_VAD_ENABLED
#define MAX_RECORD_SAMPLES (RECORD_RATE * CONFIG_DWU_VAD_MAX_RECORD_SECONDS)
#else
#define MAX_RECORD_SAMPLES (RECORD_RATE * CONFIG_DWU_RECORD_SECONDS)
#endif

// Streaming ring size. The server pushes all tts_chunks in a burst as soon
// as synthesis completes — typically during ack WAV playback, which still
// owns the I2S peripheral. The ring has to hold the entire reply until
// voice_turn calls audio_out_stream_activate() after play_wav(ack) returns.
// 128 KB = 4 s of 16 kHz mono int16 — comfortable margin for a typical
// multi-sentence slow-class reply. Kconfig override lands in Kconfig.projbuild.
#ifndef CONFIG_DWU_TTS_RING_KB
#define CONFIG_DWU_TTS_RING_KB 128
#endif

// Streaming TTS handler state. Only one turn streams at a time, so plain
// statics suffice. The handler runs on the ws_client event-handler task and
// must not block — audio_out_stream_begin/push are non-blocking ring ops.
// The actual I2S + writer task spin up is deferred to voice_turn (stream
// activate) after ack WAV playback releases the peripheral.
static SemaphoreHandle_t s_tts_registered_mutex = NULL;

static void _tts_stream_handler(const ws_tts_event_t *evt, void *ctx)
{
    (void)ctx;
    esp_err_t ret;
    switch (evt->type) {
    case WS_TTS_EVT_START: {
        if (audio_out_stream_is_active()) {
            // Stale session from a prior turn that was never cleaned up.
            // Wipe and start fresh.
            ESP_LOGW(TAG, "tts_start while prior stream still active — resetting");
            audio_out_stream_end(pdMS_TO_TICKS(100));
        }
        size_t ring_bytes = CONFIG_DWU_TTS_RING_KB * 1024;
        ret = audio_out_stream_begin((uint32_t)evt->sample_rate, 16,
                                     (uint8_t)evt->channels, ring_bytes);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "audio_out_stream_begin failed: %s",
                     esp_err_to_name(ret));
            return;
        }
        break;
    }
    case WS_TTS_EVT_CHUNK: {
        if (!audio_out_stream_is_active()) {
            ESP_LOGW(TAG, "tts_chunk before tts_start, dropping");
            return;
        }
        // During pre-activation accumulation, push must succeed — the ring
        // is sized to hold the whole reply. Timeout > 0 so a momentary full
        // buffer (e.g. ring almost drained under activate race) retries.
        ret = audio_out_stream_push(evt->pcm, evt->pcm_len, pdMS_TO_TICKS(500));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "stream_push dropped seq=%d (%u bytes): %s — "
                          "ring may be undersized",
                     evt->seq, (unsigned)evt->pcm_len, esp_err_to_name(ret));
        }
        break;
    }
    case WS_TTS_EVT_END: {
        // Don't tear down here — voice_turn owns the teardown so it can wait
        // for the drain. We've already produced everything; the writer task
        // (started by stream_activate) will empty the ring and exit via
        // stream_end.
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

// Capture one utterance into pcm using energy-threshold end-of-utterance.
// Returns ESP_ERR_TIMEOUT if no speech-level energy was seen in the lead
// window (spurious wake), ESP_FAIL if speech was too short to route, and
// ESP_OK with *actual set on normal end-of-utterance or max-duration hit.
static esp_err_t _capture_utterance(int16_t *pcm, size_t max_samples,
                                     size_t *actual)
{
    if (actual) *actual = 0;

#if CONFIG_DWU_VAD_ENABLED
    const int frame_ms = 20;
    const int frame_samples = (RECORD_RATE / 1000) * frame_ms;  // 320
    const int hangover_frames = CONFIG_DWU_VAD_HANGOVER_MS / frame_ms;
    const int max_lead_frames = CONFIG_DWU_VAD_MAX_LEAD_SILENCE_MS / frame_ms;
    const int min_utter_frames = CONFIG_DWU_VAD_MIN_UTTERANCE_MS / frame_ms;
    const int64_t speech_threshold =
        (int64_t)CONFIG_DWU_VAD_SPEECH_ENERGY_THRESHOLD;

    esp_err_t ret = audio_in_capture_arm(max_samples);
    if (ret != ESP_OK) return ret;

    size_t total = 0;
    int lead_frames = 0;
    int speech_frames = 0;
    int trailing_silence = 0;
    bool speech_started = false;
    int64_t peak_mean_sq = 0;
    int64_t t_capture = esp_timer_get_time();

    while (total + (size_t)frame_samples <= max_samples) {
        size_t got = 0;
        while (got < (size_t)frame_samples) {
            size_t r = audio_in_capture_read(pcm + total + got,
                                              (size_t)frame_samples - got,
                                              pdMS_TO_TICKS(300));
            if (r == 0) break;
            got += r;
        }
        if (got != (size_t)frame_samples) {
            ESP_LOGW(TAG, "capture underrun (%u/%d) at total=%u",
                     (unsigned)got, frame_samples, (unsigned)total);
            total += got;
            break;
        }

        int64_t sum_sq = 0;
        for (int i = 0; i < frame_samples; i++) {
            int32_t s = pcm[total + i];
            sum_sq += (int64_t)s * s;
        }
        int64_t mean_sq = sum_sq / frame_samples;
        if (mean_sq > peak_mean_sq) peak_mean_sq = mean_sq;
        bool is_speech = (mean_sq >= speech_threshold);

        total += (size_t)frame_samples;

        if (is_speech) {
            speech_frames++;
            trailing_silence = 0;
            if (!speech_started) {
                speech_started = true;
                int pre_ms = (int)((esp_timer_get_time() - t_capture) / 1000);
                ESP_LOGI(TAG, "speech onset @ %d ms (meansq=%lld)",
                         pre_ms, (long long)mean_sq);
            }
        } else {
            if (!speech_started) {
                lead_frames++;
                if (lead_frames >= max_lead_frames) {
                    ESP_LOGW(TAG, "no speech in %d ms (peak_meansq=%lld, "
                                  "threshold=%lld) — abandoning turn",
                             CONFIG_DWU_VAD_MAX_LEAD_SILENCE_MS,
                             (long long)peak_mean_sq,
                             (long long)speech_threshold);
                    audio_in_capture_disarm();
                    return ESP_ERR_TIMEOUT;
                }
            } else {
                trailing_silence++;
                if (trailing_silence >= hangover_frames) {
                    int dur_ms = (int)((esp_timer_get_time() - t_capture) / 1000);
                    ESP_LOGI(TAG, "end-of-speech @ %d ms "
                                  "(speech=%d frames, trail=%d frames, "
                                  "peak=%lld)",
                             dur_ms, speech_frames, trailing_silence,
                             (long long)peak_mean_sq);
                    break;
                }
            }
        }
    }

    audio_in_capture_disarm();

    if (speech_frames < min_utter_frames) {
        ESP_LOGW(TAG, "utterance too short (%d ms of speech < %d ms min, "
                      "peak_meansq=%lld)",
                 speech_frames * frame_ms,
                 CONFIG_DWU_VAD_MIN_UTTERANCE_MS,
                 (long long)peak_mean_sq);
        return ESP_FAIL;
    }

    if (actual) *actual = total;
    ESP_LOGI(TAG, "captured %u samples (%d ms, %d speech frames, peak=%lld)",
             (unsigned)total, (int)(total * 1000 / RECORD_RATE),
             speech_frames, (long long)peak_mean_sq);
    return ESP_OK;
#else
    return audio_in_record(pcm, max_samples, actual);
#endif
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
#if CONFIG_DWU_VAD_ENABLED
    ESP_LOGI(TAG, ">>> RECORDING (VAD, max %d s) <<<",
             CONFIG_DWU_VAD_MAX_RECORD_SECONDS);
#else
    ESP_LOGI(TAG, ">>> RECORDING %d SECONDS <<<", CONFIG_DWU_RECORD_SECONDS);
#endif

    size_t num_samples = MAX_RECORD_SAMPLES;
    int16_t *pcm = heap_caps_malloc(num_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) {
        ESP_LOGE(TAG, "Failed to allocate PCM buffer (%u bytes)", (unsigned)(num_samples * 2));
        status_led_set(LED_RED);
        beep_error();
        return ESP_ERR_NO_MEM;
    }

    size_t actual = 0;
    int64_t t_rec_start = esp_timer_get_time();
    ret = _capture_utterance(pcm, num_samples, &actual);

    if (ret == ESP_ERR_TIMEOUT) {
        // Spurious wake: no speech heard. Quiet return — don't waste a
        // server roundtrip. Error beep disabled here so a mis-fire doesn't
        // produce an audible glitch.
        heap_caps_free(pcm);
        status_led_set(LED_OFF);
        ws_client_send_state("idle", NULL);
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "capture failed: %s", esp_err_to_name(ret));
        heap_caps_free(pcm);
        status_led_set(LED_RED);
        beep_error();
        return ret;
    }

    int rec_ms = (int)((esp_timer_get_time() - t_rec_start) / 1000);
    ESP_LOGI(TAG, "Recorded %u samples (%u bytes) in %d ms", (unsigned)actual, (unsigned)(actual * 2), rec_ms);

    // Audible "stop" beep (lower pitch) so user knows recording ended
    beep(600, 150, 16000);  // 600 Hz, 150 ms

    // Process: encode (Opus or WAV), POST to server
    status_led_set(LED_BLUE);
    ws_client_send_state("uploading", NULL);
    ESP_LOGI(TAG, "Uploading + processing on okDemerzel...");

    uint8_t *body = NULL;
    size_t body_len = 0;
    const char *content_type = "audio/wav";
    int opus_rate = 0, opus_channels = 0, opus_frame_ms = 0;

#if CONFIG_DWU_ENCODE_OPUS
    const int frame_samples = RECORD_RATE / 50;  // 20 ms
    opus_stream_t *enc = NULL;
    int64_t t_enc_start = esp_timer_get_time();
    ret = opus_stream_create(RECORD_RATE, 1, CONFIG_DWU_OPUS_BITRATE_BPS, &enc);
    if (ret == ESP_OK) {
        size_t off = 0;
        while (off + (size_t)frame_samples <= actual) {
            esp_err_t er = opus_stream_encode_frame(enc, pcm + off, frame_samples);
            if (er != ESP_OK) {
                ESP_LOGW(TAG, "opus encode stopped at off=%u: %s",
                         (unsigned)off, esp_err_to_name(er));
                break;
            }
            off += (size_t)frame_samples;
        }
        ret = opus_stream_finalize(enc, &body, &body_len);
        opus_stream_destroy(enc);
        if (ret == ESP_OK && body_len > 0) {
            int enc_ms = (int)((esp_timer_get_time() - t_enc_start) / 1000);
            int pcm_bytes = (int)(actual * sizeof(int16_t));
            ESP_LOGI(TAG, "Opus encoded: %u bytes (pcm=%d bytes, %.1fx "
                          "reduction, %d ms)",
                     (unsigned)body_len, pcm_bytes,
                     (double)pcm_bytes / (double)body_len, enc_ms);
            content_type = "application/x-dwu-opus";
            opus_rate = RECORD_RATE;
            opus_channels = 1;
            opus_frame_ms = 20;
        } else {
            ESP_LOGW(TAG, "opus path produced empty body — falling back to WAV");
            if (body) heap_caps_free(body);
            body = NULL;
            body_len = 0;
        }
    } else {
        ESP_LOGW(TAG, "opus_stream_create failed: %s — falling back to WAV",
                 esp_err_to_name(ret));
    }
#endif

    if (body == NULL) {
        ret = wav_wrap(pcm, actual, RECORD_RATE, &body, &body_len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "wav_wrap failed");
            heap_caps_free(pcm);
            status_led_set(LED_RED);
            beep_error();
            return ret;
        }
    }
    heap_caps_free(pcm);

    uint8_t *wav_out = NULL;
    size_t wav_out_len = 0;
    voice_turn_meta_t meta;

    int64_t t_http_start = esp_timer_get_time();
    ret = http_post_voice_turn(body, body_len, content_type,
                                opus_rate, opus_channels, opus_frame_ms,
                                &wav_out, &wav_out_len, &meta);
    heap_caps_free(body);

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

    // Empty body = server dropped the turn (e.g. 204 non-routable).
    // Silently return to idle — no playback, no error beep.
    if (wav_out_len == 0) {
        if (wav_out) heap_caps_free(wav_out);
        status_led_set(LED_OFF);
        ws_client_send_state("idle", NULL);
        ESP_LOGI(TAG, "Server dropped turn — silent return");
        return ESP_OK;
    }

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
            if (audio_out_stream_is_active()) {
                audio_out_stream_end(pdMS_TO_TICKS(200));
            }
            return ret;
        }

        // Ack has released the I2S peripheral. If tts_start arrived during
        // ack (the common case — server bursts all chunks into the ring
        // while synthesis happens in parallel with ack playback), activate
        // now and the writer task will immediately drain the pre-buffered
        // PCM into I2S.
        bool activated = false;
        if (audio_out_stream_is_active()) {
            esp_err_t act_ret = audio_out_stream_activate();
            if (act_ret != ESP_OK) {
                ESP_LOGW(TAG, "stream_activate failed: %s — falling back",
                         esp_err_to_name(act_ret));
                audio_out_stream_end(pdMS_TO_TICKS(200));
            } else {
                activated = true;
                status_led_set(LED_GREEN);
                ws_client_send_state("playing", meta.pending_id);
            }
        } else {
            ESP_LOGI(TAG, "no tts_start yet, will wait on tts_end or fall back");
        }

        // Wait for tts_end to signal all chunks have been pushed. Hard cap
        // of 35 s for the pathological slow case. Writer task is draining
        // in parallel.
        int64_t t_stream_wait = esp_timer_get_time();
        esp_err_t stream_ret = ws_client_wait_tts_end(35000);
        int stream_ms = (int)((esp_timer_get_time() - t_stream_wait) / 1000);

        // If tts_start came LATE (after the wait started but before tts_end),
        // audio_out_stream_is_active is now true but draining isn't — activate now.
        if (stream_ret == ESP_OK && !activated && audio_out_stream_is_active()) {
            esp_err_t act_ret = audio_out_stream_activate();
            if (act_ret == ESP_OK) {
                activated = true;
                status_led_set(LED_GREEN);
                ws_client_send_state("playing", meta.pending_id);
            }
        }

        if (stream_ret == ESP_OK && activated) {
            // Block until the writer drains the remaining ring.
            esp_err_t end_ret = audio_out_stream_end(pdMS_TO_TICKS(15000));
            uint32_t underruns = audio_out_stream_underrun_count();
            ESP_LOGI(TAG, "Real answer streamed over WS (wait=%d ms, underruns=%lu, end=%s)",
                     stream_ms, (unsigned long)underruns,
                     esp_err_to_name(end_ret));
        } else {
            // Stream didn't complete in time (or tts_start never arrived) —
            // tear down any half-open stream and fall back to HTTP.
            ws_client_expect_tts_stream(NULL);
            if (audio_out_stream_is_active()) {
                audio_out_stream_end(pdMS_TO_TICKS(500));
            }
            ESP_LOGW(TAG, "TTS stream %s after %d ms, falling back to GET /voice_result",
                     (stream_ret == ESP_OK) ? "couldn't activate" : "timed out",
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
