#pragma once
#include "esp_err.h"

typedef struct {
    float temperature_c;
    float humidity_pct;
    float pressure_hpa;
} bme280_reading_t;

esp_err_t bme280_init(void);
esp_err_t bme280_read(bme280_reading_t *out);
