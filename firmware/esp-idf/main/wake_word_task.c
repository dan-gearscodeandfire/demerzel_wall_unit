#include "wake_word_task.h"
#include "wake_word.h"
#include "audio_in.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>

static const char *TAG = "wake_word_task";

// 10 ms of 16 kHz int16 = 160 samples = exactly one feature hop.
#define CHUNK_SAMPLES  160

// Fallback self-mute in case main.c forgets to pause/resume us around
// voice_turn — a full voice_turn can run ~20 s, so give ample headroom.
#define POST_DETECT_MUTE_MS  25000

static EventGroupHandle_t s_trigger_events = NULL;
static EventBits_t        s_trigger_bit    = 0;
static atomic_bool        s_paused         = false;
static atomic_uint        s_last_score     = 0;

// After a reset, swallow this many inference steps before honoring detections.
// The streaming model can emit transient high scores for the first ~30 ms as
// its VAR_HANDLE state rebuilds from zero; we want that to settle first.
// 20 steps * 10 ms/step = 200 ms blind window.
#define WARMUP_STEPS_AFTER_RESET 20
static atomic_uint s_warmup_left = 0;

static void wake_word_task_fn(void *arg)
{
    int16_t pcm[CHUNK_SAMPLES];

    esp_err_t r = wake_word_init(WAKE_WORD_FRONTEND_TFLM);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "wake_word_init failed: %s — task exiting", esp_err_to_name(r));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "wake-word task running (chunk=%d samples, threshold=%u)",
             CHUNK_SAMPLES, wake_word_get_threshold());

    uint8_t  window_max     = 0;
    uint32_t window_steps   = 0;
    int32_t  window_pcm_peak = 0;
    int64_t  window_pcm_ms   = 0;     // sum of squares
    uint32_t window_samples  = 0;
    int64_t  last_log_us    = esp_timer_get_time();

    while (1) {
        size_t got = audio_in_consume_wake(pcm, CHUNK_SAMPLES, portMAX_DELAY);
        if (got == 0) continue;

        // Track PCM peak + RMS for this heartbeat window.
        for (size_t i = 0; i < got; ++i) {
            int32_t s = pcm[i];
            int32_t a = s < 0 ? -s : s;
            if (a > window_pcm_peak) window_pcm_peak = a;
            window_pcm_ms += (int64_t)s * (int64_t)s;
        }
        window_samples += (uint32_t)got;

        if (atomic_load(&s_paused)) {
            // While paused, drain samples but DO NOT feed the model — we don't
            // want TTS playback / our own voice to propagate into the streaming
            // state and produce a spurious detection immediately after resume.
            continue;
        }

        size_t steps = 0;
        int score = wake_word_feed(pcm, got, &steps);
        atomic_store(&s_last_score, (unsigned)score);

        // Drain warmup credits for any inference steps that just fired.
        // The streaming model can emit transient spikes for the first few
        // steps after a state reset; we ignore detections during that window.
        unsigned warm = atomic_load(&s_warmup_left);
        if (warm > 0) {
            unsigned consume = (unsigned)steps;
            if (consume > warm) consume = warm;
            atomic_store(&s_warmup_left, warm - consume);
            continue;
        }

        if ((uint8_t)score > window_max) window_max = (uint8_t)score;
        window_steps += (uint32_t)steps;

        int64_t now = esp_timer_get_time();
        if (now - last_log_us > 5000000LL) {  // every 5 s
            int32_t rms = window_samples ? (int32_t)(window_pcm_ms / (int64_t)window_samples) : 0;
            ESP_LOGI(TAG, "heartbeat: score_max=%u steps=%lu pcm_peak=%ld pcm_meansq=%ld threshold=%u",
                     window_max, (unsigned long)window_steps,
                     (long)window_pcm_peak, (long)rms,
                     wake_word_get_threshold());
            window_max       = 0;
            window_steps     = 0;
            window_pcm_peak  = 0;
            window_pcm_ms    = 0;
            window_samples   = 0;
            last_log_us      = now;
        }

        if (wake_word_detected()) {
            ESP_LOGI(TAG, ">>> WAKE WORD DETECTED <<< score=%d", score);
            xEventGroupSetBits(s_trigger_events, s_trigger_bit);
            wake_word_reset();
            atomic_store(&s_paused, true);
            vTaskDelay(pdMS_TO_TICKS(POST_DETECT_MUTE_MS));
            wake_word_reset();
            atomic_store(&s_warmup_left, WARMUP_STEPS_AFTER_RESET);
            atomic_store(&s_paused, false);
        }
    }
}

esp_err_t wake_word_task_start(EventGroupHandle_t events, EventBits_t bit)
{
    if (!events) return ESP_ERR_INVALID_ARG;
    s_trigger_events = events;
    s_trigger_bit    = bit;

    BaseType_t ok = xTaskCreate(wake_word_task_fn, "wake_word", 8192, NULL, 5, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

void wake_word_task_pause(void)  { atomic_store(&s_paused, true); }
void wake_word_task_resume(void)
{
    // Clear streaming state before unpausing so the first inference step after
    // resume sees a fresh model context, not one flavored by whatever we
    // accumulated right before the pause. Also reset warmup credits so we
    // silently consume the first 200 ms of post-resume scores.
    wake_word_reset();
    atomic_store(&s_warmup_left, WARMUP_STEPS_AFTER_RESET);
    atomic_store(&s_paused, false);
}

uint8_t wake_word_task_last_score(void)
{
    return (uint8_t)atomic_load(&s_last_score);
}
