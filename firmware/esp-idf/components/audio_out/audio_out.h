#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

esp_err_t audio_out_init(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels);
esp_err_t audio_out_write(const void *data, size_t len, size_t *bytes_written);
esp_err_t audio_out_reconfigure(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels);
void      audio_out_mute(void);
void      audio_out_unmute(void);
void      audio_out_deinit(void);
