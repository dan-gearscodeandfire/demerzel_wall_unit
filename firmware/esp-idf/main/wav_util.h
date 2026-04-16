#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    size_t   data_offset;
    size_t   data_size;
} wav_header_info_t;

esp_err_t wav_wrap(const int16_t *pcm, size_t num_samples, uint32_t sample_rate,
                   uint8_t **out_wav, size_t *out_wav_len);

esp_err_t wav_parse(const uint8_t *wav, size_t wav_len, wav_header_info_t *info);
