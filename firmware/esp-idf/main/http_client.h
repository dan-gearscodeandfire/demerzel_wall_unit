#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char transcript[512];
    char reply_text[512];
    int  latency_ms;
    char pending_id[16];   // non-empty if two-phase (server set X-DWU-Pending)
} voice_turn_meta_t;

esp_err_t http_post_voice_turn(const uint8_t *wav_data, size_t wav_len,
                                uint8_t **out_wav, size_t *out_wav_len,
                                voice_turn_meta_t *meta);

// Fetch the real answer for a two-phase turn (long-poll, blocks up to ~35 s).
esp_err_t http_get_voice_result(const char *request_id,
                                 uint8_t **out_wav, size_t *out_wav_len,
                                 voice_turn_meta_t *meta);
