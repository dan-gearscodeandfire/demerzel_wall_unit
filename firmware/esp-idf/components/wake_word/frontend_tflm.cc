// TFLM audio-frontend implementation. Uses the same mel-spectrogram +
// noise-reduction + PCAN + log-scale + int8 quantization pipeline that the
// microWakeWord / ESPHome stack uses, so the trained model sees the exact
// feature distribution it learned.
//
// Constants must match microWakeWord training defaults — these are mirrored
// from ESPHome's preprocessor_settings.h. Changing them silently breaks the
// model.

extern "C" {
#include "wake_word_frontend.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
}

#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"

static const char *TAG = "ww_tflm";

namespace {

constexpr int    SAMPLE_RATE                  = 16000;
constexpr int    FEATURE_DURATION_MS          = 30;
constexpr int    FEATURE_HOP_MS               = 10;
constexpr int    FILTERBANK_NUM_CHANNELS      = WAKE_WORD_FEATURE_SIZE;  // 40
constexpr float  FILTERBANK_LOWER_BAND_LIMIT  = 125.0f;
constexpr float  FILTERBANK_UPPER_BAND_LIMIT  = 7500.0f;
constexpr int    NOISE_REDUCTION_SMOOTHING_BITS    = 10;
constexpr float  NOISE_REDUCTION_EVEN_SMOOTHING    = 0.025f;
constexpr float  NOISE_REDUCTION_ODD_SMOOTHING     = 0.06f;
constexpr float  NOISE_REDUCTION_MIN_SIGNAL_REMAIN = 0.05f;
constexpr int    PCAN_ENABLE                       = 1;
constexpr float  PCAN_STRENGTH                     = 0.95f;
constexpr float  PCAN_OFFSET                       = 80.0f;
constexpr int    PCAN_GAIN_BITS                    = 21;
constexpr int    LOG_ENABLE                        = 1;
constexpr int    LOG_SCALE_SHIFT                   = 6;

FrontendState  state{};
bool           initialised = false;

// Map raw uint16 frontend output to int8 the way ESPHome does. The frontend
// emits values roughly in [0, 670]; this scales to [-128, 127].
inline int8_t quantize_int8(uint16_t raw) {
    int32_t v = (static_cast<int32_t>(raw) * 256) / 666 - 128;
    if (v > 127)  v = 127;
    if (v < -128) v = -128;
    return static_cast<int8_t>(v);
}

esp_err_t init_impl()
{
    if (initialised) return ESP_OK;

    FrontendConfig config{};
    config.window.size_ms       = FEATURE_DURATION_MS;
    config.window.step_size_ms  = FEATURE_HOP_MS;

    config.filterbank.num_channels      = FILTERBANK_NUM_CHANNELS;
    config.filterbank.lower_band_limit  = FILTERBANK_LOWER_BAND_LIMIT;
    config.filterbank.upper_band_limit  = FILTERBANK_UPPER_BAND_LIMIT;

    config.noise_reduction.smoothing_bits        = NOISE_REDUCTION_SMOOTHING_BITS;
    config.noise_reduction.even_smoothing        = NOISE_REDUCTION_EVEN_SMOOTHING;
    config.noise_reduction.odd_smoothing         = NOISE_REDUCTION_ODD_SMOOTHING;
    config.noise_reduction.min_signal_remaining  = NOISE_REDUCTION_MIN_SIGNAL_REMAIN;

    config.pcan_gain_control.enable_pcan = PCAN_ENABLE;
    config.pcan_gain_control.strength    = PCAN_STRENGTH;
    config.pcan_gain_control.offset      = PCAN_OFFSET;
    config.pcan_gain_control.gain_bits   = PCAN_GAIN_BITS;

    config.log_scale.enable_log  = LOG_ENABLE;
    config.log_scale.scale_shift = LOG_SCALE_SHIFT;

    if (!FrontendPopulateState(&config, &state, SAMPLE_RATE)) {
        ESP_LOGE(TAG, "FrontendPopulateState failed");
        return ESP_FAIL;
    }

    initialised = true;
    ESP_LOGI(TAG, "TFLM frontend ready: %d Hz, %d ms window, %d ms hop, %d mel bins",
             SAMPLE_RATE, FEATURE_DURATION_MS, FEATURE_HOP_MS, FILTERBANK_NUM_CHANNELS);
    return ESP_OK;
}

void reset_impl()
{
    if (initialised) FrontendReset(&state);
}

void deinit_impl()
{
    if (initialised) {
        FrontendFreeStateContents(&state);
        initialised = false;
    }
}

size_t process_impl(const int16_t *pcm, size_t n_samples,
                     int8_t *features_out, size_t max_frames)
{
    if (!initialised || !pcm || !features_out || max_frames == 0) return 0;

    size_t frames = 0;
    size_t consumed_total = 0;
    const int16_t *cursor = pcm;
    size_t remaining = n_samples;

    while (remaining > 0 && frames < max_frames) {
        size_t consumed = 0;
        FrontendOutput out = FrontendProcessSamples(&state, cursor, remaining, &consumed);
        if (consumed == 0) break;  // not enough samples to advance
        cursor    += consumed;
        remaining -= consumed;
        consumed_total += consumed;

        if (out.size == 0) continue;  // sample window not yet full

        // out.size should equal FILTERBANK_NUM_CHANNELS (40).
        if (out.size != WAKE_WORD_FEATURE_SIZE) {
            ESP_LOGW(TAG, "unexpected feature size %u (expected %d)",
                     (unsigned)out.size, WAKE_WORD_FEATURE_SIZE);
            break;
        }

        int8_t *dst = features_out + frames * WAKE_WORD_FEATURE_SIZE;
        for (size_t i = 0; i < WAKE_WORD_FEATURE_SIZE; ++i) {
            dst[i] = quantize_int8(out.values[i]);
        }
        ++frames;
    }

    return frames;
}

}  // namespace

extern "C" const wake_word_frontend_t wake_word_frontend_tflm = {
    .name    = "tflm",
    .init    = init_impl,
    .reset   = reset_impl,
    .deinit  = deinit_impl,
    .process = process_impl,
};
