#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

esp_err_t audio_in_init(void);
esp_err_t audio_in_record(int16_t *out_buf, size_t num_samples, size_t *actual_samples);
void      audio_in_deinit(void);
