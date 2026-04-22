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
