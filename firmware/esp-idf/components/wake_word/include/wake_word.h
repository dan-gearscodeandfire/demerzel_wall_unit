#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Number of mel features per frame. Matches the trained model's input width.
#define WAKE_WORD_FEATURE_SIZE  40

// Per-frame audio window/hop, matching microWakeWord training defaults.
#define WAKE_WORD_FEATURE_WINDOW_MS  30
#define WAKE_WORD_FEATURE_HOP_MS     10

typedef enum {
    WAKE_WORD_FRONTEND_TFLM     = 0,
    WAKE_WORD_FRONTEND_HANDROLL = 1,
} wake_word_frontend_id_t;

// Init the wake-word pipeline: load the embedded model, allocate tensor arena
// (PSRAM), select the named frontend. Idempotent.
esp_err_t wake_word_init(wake_word_frontend_id_t frontend);

// Free interpreter, frontend state, and tensor arena.
void wake_word_deinit(void);

// Reset frontend buffers and the streaming model's input window. Call after a
// detection event or after a long idle period.
void wake_word_reset(void);

// Feed PCM samples (16 kHz int16 mono). Internally extracts mel features at
// 10 ms hops, pushes each frame through the streaming model, and updates the
// most-recent score. Returns the highest score observed during this call,
// in [0, 255] (raw uint8 model output). Optionally reports how many inference
// steps fired in *steps_out.
int wake_word_feed(const int16_t *pcm, size_t n_samples, size_t *steps_out);

// Detection threshold in [0, 255]. Default 252 (~0.99 sigmoid).
void    wake_word_set_threshold(uint8_t threshold);
uint8_t wake_word_get_threshold(void);

// True iff the most recent score crossed the threshold.
bool wake_word_detected(void);

// Most recent raw uint8 score from the model.
uint8_t wake_word_last_score(void);

// Run only the frontend (no inference) over the given PCM. Writes one int8[40]
// feature frame per 10 ms hop into features_out (capacity max_frames frames).
// Returns the number of frames written. Used by the comparison harness.
size_t wake_word_extract_features(const int16_t *pcm, size_t n_samples,
                                   int8_t *features_out, size_t max_frames);

#ifdef __cplusplus
}
#endif
