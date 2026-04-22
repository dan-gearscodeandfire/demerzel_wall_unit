#include "followup.h"
#include "audio_in.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "followup";

#define CHUNK_SAMPLES  160   // 10 ms at 16 kHz

bool followup_detect_speech(void)
{
    int16_t pcm[CHUNK_SAMPLES];

    // Flush stale samples (TTS bleed-through sitting in the wake ring).
    for (int i = 0; i < 100; i++) {
        if (audio_in_consume_wake(pcm, CHUNK_SAMPLES, 0) == 0) break;
    }

    // Settling delay — the speaker amp + room acoustics need ~1 s to go
    // quiet after TTS playback. Without this, the followup detector picks
    // up the tail of the just-played response and loops endlessly.
    vTaskDelay(pdMS_TO_TICKS(1000));
    while (audio_in_consume_wake(pcm, CHUNK_SAMPLES, 0) > 0) {}

    // Calibrate to room ambient: 200 ms of chunks right after settle.
    // Threshold = max(baseline * 3, 30 M) — matches the capture-loop
    // energy threshold as a floor. Adaptive per-turn so a motor spooling
    // up doesn't re-fire followup on its own noise.
    const int CAL_CHUNKS = 20;  // 200 ms at 10 ms/chunk
    int64_t cal_sum = 0;
    int cal_got = 0;
    for (int i = 0; i < CAL_CHUNKS; i++) {
        size_t got = audio_in_consume_wake(pcm, CHUNK_SAMPLES, pdMS_TO_TICKS(50));
        if (got == 0) continue;
        int64_t sum_sq = 0;
        for (size_t j = 0; j < got; j++) {
            sum_sq += (int64_t)pcm[j] * (int64_t)pcm[j];
        }
        cal_sum += sum_sq / (int64_t)got;
        cal_got++;
    }
    int64_t baseline = cal_got ? cal_sum / cal_got : 0;
    int64_t threshold = baseline * 3;
    if (threshold < 30000000) threshold = 30000000;
    if (threshold < CONFIG_DWU_FOLLOWUP_ENERGY_THRESHOLD) {
        // Never drop below the Kconfig floor — user may have tuned it up
        // for a specific environment.
        threshold = CONFIG_DWU_FOLLOWUP_ENERGY_THRESHOLD;
    }

    ESP_LOGI(TAG, "followup window open (%d ms, baseline=%lld, threshold=%lld, "
                  "debounce=%d)",
             CONFIG_DWU_FOLLOWUP_WINDOW_MS, (long long)baseline,
             (long long)threshold, CONFIG_DWU_FOLLOWUP_DEBOUNCE_CHUNKS);

    int64_t deadline = esp_timer_get_time()
                     + (int64_t)CONFIG_DWU_FOLLOWUP_WINDOW_MS * 1000;
    int consec = 0;

    while (esp_timer_get_time() < deadline) {
        size_t got = audio_in_consume_wake(pcm, CHUNK_SAMPLES, pdMS_TO_TICKS(50));
        if (got == 0) continue;

        int64_t sum_sq = 0;
        for (size_t i = 0; i < got; i++) {
            sum_sq += (int64_t)pcm[i] * (int64_t)pcm[i];
        }
        int64_t mean_sq = sum_sq / (int64_t)got;

        if (mean_sq >= threshold) {
            if (++consec >= CONFIG_DWU_FOLLOWUP_DEBOUNCE_CHUNKS) {
                ESP_LOGI(TAG, "speech detected (energy=%lld, consec=%d)",
                         (long long)mean_sq, consec);
                return true;
            }
        } else {
            consec = 0;
        }
    }

    ESP_LOGI(TAG, "followup window expired — silence");
    return false;
}
