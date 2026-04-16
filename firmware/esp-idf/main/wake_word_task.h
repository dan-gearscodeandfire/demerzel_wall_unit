#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// Spawn the wake-word inference task. It continuously drains audio_in's wake
// ring, runs streaming inference, and sets `bit` on `events` when the model
// crosses the detection threshold. Then enters a cooldown to suppress
// re-triggers while voice_turn is running.
//
// Call audio_in_init() before this so the wake ring is already filling.
esp_err_t wake_word_task_start(EventGroupHandle_t events, EventBits_t bit);

// Pause/resume detection. Inference still runs (cheap to keep state warm),
// but detections do not set the trigger event. Use during voice_turn so the
// device's own TTS playback can't trip the wake word.
void wake_word_task_pause(void);
void wake_word_task_resume(void);

// Most recent score (0..255) — exposed for log_server / debugging.
uint8_t wake_word_task_last_score(void);
