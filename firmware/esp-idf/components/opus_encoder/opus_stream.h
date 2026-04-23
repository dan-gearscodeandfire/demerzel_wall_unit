#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// Lightweight streaming Opus encoder for DWU voice capture. Wraps libopus
// (provided by the micro-opus managed component) and accumulates encoded
// packets into a single heap buffer with a simple length-prefixed wire
// format: [u16 LE packet_len][packet bytes][u16 LE packet_len][packet bytes]...
//
// Per-frame metadata (sample rate, frame_ms, channels) is NOT in the body —
// callers send it as HTTP headers on the upload.

typedef struct opus_stream opus_stream_t;

// Create an encoder configured for VOIP. Frame size is implied by
// sample_rate (20 ms frames = sample_rate / 50 samples per call).
esp_err_t opus_stream_create(int sample_rate, int channels, int bitrate_bps,
                              opus_stream_t **out);

// Encode one 20 ms frame. frame_samples must match sample_rate / 50.
esp_err_t opus_stream_encode_frame(opus_stream_t *s, const int16_t *pcm,
                                    int frame_samples);

// Finalize and transfer ownership of the accumulated buffer to the caller
// (caller frees via heap_caps_free). After this, the stream's internal
// buffer is NULL; further encode calls will fail unless destroyed first.
esp_err_t opus_stream_finalize(opus_stream_t *s,
                                uint8_t **out_buf, size_t *out_len);

void opus_stream_destroy(opus_stream_t *s);
