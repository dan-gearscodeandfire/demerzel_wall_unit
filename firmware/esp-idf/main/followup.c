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
    for (int i = 0; i < 30; i++) {
        if (audio_in_consume_wake(pcm, CHUNK_SAMPLES, 0) == 0) break;
    }

    // Settling delay — let the speaker amp go quiet + mic echo decay.
    vTaskDelay(pdMS_TO_TICKS(200));
    while (audio_in_consume_wake(pcm, CHUNK_SAMPLES, 0) > 0) {}

    ESP_LOGI(TAG, "followup window open (%d ms, threshold=%d, debounce=%d)",
             CONFIG_DWU_FOLLOWUP_WINDOW_MS,
             CONFIG_DWU_FOLLOWUP_ENERGY_THRESHOLD,
             CONFIG_DWU_FOLLOWUP_DEBOUNCE_CHUNKS);

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

        if (mean_sq >= CONFIG_DWU_FOLLOWUP_ENERGY_THRESHOLD) {
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
