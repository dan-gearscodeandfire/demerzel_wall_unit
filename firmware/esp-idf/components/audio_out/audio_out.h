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
// The lifecycle is split so that a producer can START BUFFERING before I2S is
// ready — necessary because the voice_server pushes tts_chunks in a fast
// burst during ack-WAV playback, which still owns the I2S TX peripheral.
//
//   _begin    — allocates a SPIRAM ring and records the stream format. No
//               I2S is touched. Safe to call while another audio_out_*
//               playback holds the peripheral.
//   _push     — appends bytes to the ring. Works before or after _activate.
//               If the ring is full, blocks up to `timeout_ticks`.
//   _activate — initializes I2S + spawns the writer task. Must be called
//               after any prior playback (e.g. the ack) has released I2S
//               via audio_out_deinit. Writer starts draining whatever is
//               already in the ring.
//   _end      — signals the writer to finish once the ring drains, blocks
//               until it exits, then tears down I2S.
//
// Underruns during _activate→_end pad with silence (STREAM_SILENCE_PAD_MS)
// to keep the MAX98357A DMA fed cleanly — quiet gap, no audible glitch.
// Ring size should comfortably cover ack duration + worst-case synthesis
// burst (~128-256 KB for a typical multi-sentence reply at 16 kHz).
//
// Concurrency: only one stream session at a time. _begin while active
// returns ESP_ERR_INVALID_STATE.

esp_err_t audio_out_stream_begin(uint32_t sample_rate,
                                 uint8_t bits_per_sample,
                                 uint8_t channels,
                                 size_t ring_bytes);
esp_err_t audio_out_stream_push(const void *pcm, size_t len,
                                TickType_t timeout_ticks);
esp_err_t audio_out_stream_activate(void);
esp_err_t audio_out_stream_end(TickType_t drain_timeout_ticks);
bool      audio_out_stream_is_active(void);     // begin called, not ended
bool      audio_out_stream_is_draining(void);   // activate called, not ended
uint32_t  audio_out_stream_underrun_count(void);
