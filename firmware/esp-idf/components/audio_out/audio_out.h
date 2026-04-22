#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

esp_err_t audio_out_init(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels);
esp_err_t audio_out_write(const void *data, size_t len, size_t *bytes_written);
esp_err_t audio_out_reconfigure(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels);
void      audio_out_mute(void);
void      audio_out_unmute(void);
void      audio_out_deinit(void);

// --- Streaming playback API ---
//
// Producer/consumer model for playing PCM as it arrives (e.g. over WebSocket).
// _begin configures I2S + spins up a writer task that drains a SPIRAM ring.
// _push copies bytes into the ring; if the ring is full it blocks up to
// `timeout_ticks`. The writer task pads with silence on underrun to keep the
// MAX98357A DMA fed cleanly — no audible glitch on empty ring, just quiet.
// _end signals the writer task to finish, blocks until the ring drains, then
// tears down I2S. All functions return ESP_ERR_INVALID_STATE if called out
// of order.
//
// Concurrency: only one stream session at a time. _begin while already
// active returns ESP_ERR_INVALID_STATE.

esp_err_t audio_out_stream_begin(uint32_t sample_rate,
                                 uint8_t bits_per_sample,
                                 uint8_t channels,
                                 size_t jitter_bytes);
esp_err_t audio_out_stream_push(const void *pcm, size_t len,
                                TickType_t timeout_ticks);
esp_err_t audio_out_stream_end(TickType_t drain_timeout_ticks);
bool      audio_out_stream_is_active(void);
uint32_t  audio_out_stream_underrun_count(void);
