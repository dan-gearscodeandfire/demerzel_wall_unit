#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char transcript[512];
    char reply_text[512];
    int  latency_ms;
} voice_turn_meta_t;

esp_err_t http_post_voice_turn(const uint8_t *wav_data, size_t wav_len,
                                uint8_t **out_wav, size_t *out_wav_len,
                                voice_turn_meta_t *meta);
