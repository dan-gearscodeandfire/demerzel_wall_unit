#pragma once

#include <stdbool.h>

// Monitor the wake ring for speech energy during the followup window.
// Returns true if speech was detected before timeout, false on silence.
// Caller must ensure wake_word_task is paused before calling — this
// function drains the wake ring directly.
bool followup_detect_speech(void);
