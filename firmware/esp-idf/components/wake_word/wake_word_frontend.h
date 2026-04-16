#pragma once

#include "esp_err.h"
#include "wake_word.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Internal frontend strategy interface. Each implementation is one of these.
typedef struct {
    const char *name;
    esp_err_t (*init)(void);
    void      (*reset)(void);
    void      (*deinit)(void);
    // Process PCM. Each time a 30 ms window fills (one per 10 ms hop), append
    // one int8[WAKE_WORD_FEATURE_SIZE] feature frame to features_out (up to
    // max_frames frames). Return frames written. Stateful — the implementation
    // remembers leftover samples between calls.
    size_t    (*process)(const int16_t *pcm, size_t n_samples,
                          int8_t *features_out, size_t max_frames);
} wake_word_frontend_t;

extern const wake_word_frontend_t wake_word_frontend_tflm;
extern const wake_word_frontend_t wake_word_frontend_handroll;

#ifdef __cplusplus
}
#endif
