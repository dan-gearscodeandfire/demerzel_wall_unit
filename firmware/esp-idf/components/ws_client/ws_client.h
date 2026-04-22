#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// Persistent signaling-only WebSocket to the voice_server /ws endpoint.
// Audio payloads still go over HTTP — this channel carries small JSON events
// (wake, state, env, presence up; pending_ready, barge_in, suppress down).
//
// Lifecycle: ws_client_start() spawns an internal task that connects with
// exponential backoff and sends `hello` on every (re)connect. Voice turns
// never block on the ws being up — every send is best-effort.

esp_err_t ws_client_start(const char *fw_version);

// True iff the session is open (TCP + hello_ack received).
bool ws_client_is_connected(void);

// "aa:bb:cc:dd:ee:ff" — the unit's WiFi STA MAC, lowercase. Valid from the
// point ws_client_start() returns. Pointer is static, do not free.
const char *ws_client_unit_id(void);

// --- unit → server events (best-effort; return ESP_OK even if not
// connected, but drop silently). ---

esp_err_t ws_client_send_state(const char *state, const char *turn_id);
esp_err_t ws_client_send_wake(int score_peak);
esp_err_t ws_client_send_env(float temp_c, float humidity, float pressure_hpa);
esp_err_t ws_client_send_presence(bool pir, bool radar);

// --- Two-phase short-circuit ---
//
// Call ws_client_expect_pending_ready(request_id) when you receive the ack
// WAV with X-DWU-Pending. Then call ws_client_wait_pending_ready(ms) after
// the ack has finished playing — it blocks until either the server pushes
// a matching pending_ready event or timeout elapses. Unrelated pending_ready
// events (e.g. for a stale prior turn) are ignored.

void      ws_client_expect_pending_ready(const char *request_id);
esp_err_t ws_client_wait_pending_ready(uint32_t timeout_ms);

// --- TTS streaming ---
//
// Server emits `tts_start` → `tts_chunk` × N → `tts_end` on the same WS
// channel for slow-class turns. The handler set here is called synchronously
// from the ws task when each event arrives — it MUST be fast (no blocking
// I/O, no long malloc). Typical implementation: push decoded PCM into an
// audio_out ring and return.
//
// Lifetime: register a handler once at startup; use _expect_tts_stream(id)
// per turn to gate which request_ids are surfaced. Events for non-expected
// request_ids are silently dropped at the ws layer (no handler call).
// Clear expectation with NULL/"".

typedef enum {
    WS_TTS_EVT_START,   // (sample_rate, channels)
    WS_TTS_EVT_CHUNK,   // (pcm, pcm_len, seq)
    WS_TTS_EVT_END,     // (total_seq)
} ws_tts_event_type_t;

typedef struct {
    ws_tts_event_type_t type;
    const char *request_id;   // borrowed, valid during callback only
    // START:
    int sample_rate;
    int channels;
    // CHUNK:
    const uint8_t *pcm;       // decoded int16 LE PCM; borrowed for callback
    size_t pcm_len;
    int seq;
    // END:
    int total_seq;
} ws_tts_event_t;

typedef void (*ws_tts_handler_t)(const ws_tts_event_t *evt, void *ctx);

void      ws_client_set_tts_handler(ws_tts_handler_t handler, void *ctx);
void      ws_client_expect_tts_stream(const char *request_id);

// Block until a matching `tts_end` fires or timeout elapses. Returns ESP_OK
// when stream completed, ESP_ERR_TIMEOUT otherwise. Clears the expectation
// on return.
esp_err_t ws_client_wait_tts_end(uint32_t timeout_ms);
