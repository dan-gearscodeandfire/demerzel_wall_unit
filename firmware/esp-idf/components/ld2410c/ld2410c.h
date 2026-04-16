#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     presence;       /* from OUT pin (GPIO8) */
    uint8_t  target_state;   /* 0=none, 1=moving, 2=stationary, 3=both */
    bool     uart_valid;     /* true if at least one frame parsed */
} ld2410c_state_t;

esp_err_t ld2410c_init(void);
esp_err_t ld2410c_get_state(ld2410c_state_t *out);
void      ld2410c_deinit(void);
