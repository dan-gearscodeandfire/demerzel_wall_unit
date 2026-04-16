#pragma once

#include "esp_err.h"

esp_err_t ota_init(void);
esp_err_t ota_check_and_update(void);
const char *ota_get_current_version(void);
void ota_mark_valid(void);
