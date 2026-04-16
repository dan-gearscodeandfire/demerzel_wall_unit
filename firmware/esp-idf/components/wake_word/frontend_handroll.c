// Hand-rolled mel-spectrogram + PCAN + noise-reduction frontend.
//
// STUB: not yet implemented. The TFLM frontend is the working baseline; this
// file exists so the build supports both paths and the comparison harness has
// somewhere to call. Filling this in is task #9.
//
// When implemented, must match the TFLM pipeline EXACTLY (window 30 ms, hop
// 10 ms, 40 mel bins, Wiener noise reduction, PCAN gain control, log scale,
// int8 quantization with formula (raw * 256 / 666) - 128) — the trained model
// only fires on features matching the training distribution.

#include "wake_word_frontend.h"
#include "esp_log.h"

static const char *TAG = "ww_handroll";

static esp_err_t handroll_init(void)
{
    ESP_LOGW(TAG, "hand-rolled frontend not implemented yet — returns 0 features");
    return ESP_OK;
}

static void handroll_reset(void) {}
static void handroll_deinit(void) {}

static size_t handroll_process(const int16_t *pcm, size_t n_samples,
                                int8_t *features_out, size_t max_frames)
{
    (void)pcm; (void)n_samples; (void)features_out; (void)max_frames;
    return 0;
}

const wake_word_frontend_t wake_word_frontend_handroll = {
    .name    = "handroll",
    .init    = handroll_init,
    .reset   = handroll_reset,
    .deinit  = handroll_deinit,
    .process = handroll_process,
};
