#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <stddef.h>
#include <stdint.h>

#define AUDIO_IN_SAMPLE_RATE 16000

// Call once at boot. Starts I2S0 RX + a continuous reader task that pushes
// int16 PCM samples to the wake-word ring (always live) and the capture
// ring (only when armed). Idempotent — second call is a no-op.
esp_err_t audio_in_init(void);

// Stop the reader task and tear down I2S. Frees both rings.
void audio_in_deinit(void);

// Pull up to n_samples int16 PCM samples from the wake-word stream.
// Blocking up to timeout_ticks on the first chunk; subsequent partial
// reads are non-blocking. Returns the actual sample count (0 on timeout).
size_t audio_in_consume_wake(int16_t *buf, size_t n_samples,
                              TickType_t timeout_ticks);

// Arm the capture ring with capacity for at least expected_samples.
// After this returns OK, every incoming sample is pushed to the capture
// ring until disarm. Allocated in PSRAM. Errors if already armed.
esp_err_t audio_in_capture_arm(size_t expected_samples);

// Pull captured samples (blocking up to timeout). Returns count read.
size_t audio_in_capture_read(int16_t *buf, size_t n_samples,
                              TickType_t timeout_ticks);

// Free the capture ring. Safe to call when not armed.
void audio_in_capture_disarm(void);

// Backwards-compat one-shot record. Internally arms, reads, disarms.
esp_err_t audio_in_record(int16_t *out_buf, size_t num_samples,
                           size_t *actual_samples);
