#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t wifi_init_sta(void);
bool      wifi_is_connected(void);
