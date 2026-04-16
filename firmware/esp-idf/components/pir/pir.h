#pragma once
#include "esp_err.h"
#include <stdbool.h>

typedef void (*pir_callback_t)(bool motion);

esp_err_t pir_init(pir_callback_t cb);
bool      pir_get_state(void);
