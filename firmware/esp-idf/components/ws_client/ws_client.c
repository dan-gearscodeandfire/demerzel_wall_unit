#include "ws_client.h"
#include "esp_websocket_client.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ws_client";

#define BIT_CONNECTED       BIT0   // hello_ack received
#define BIT_PENDING_READY   BIT1   // a pending_ready matching expected id arrived
#define BIT_TTS_END         BIT2   // tts_end for the expected tts request_id

// Biggest decoded PCM we expect per tts_chunk: the server's TTS_CHUNK_BYTES
// (1280) plus slack. Static to avoid heap churn in the hot path.
#define TTS_DECODE_BUF_BYTES 2048

// Frame-assembly buffer for multi-part WEBSOCKET_EVENT_DATA events. The
// esp_websocket_client delivers a single WS frame as multiple events when
// the frame crosses its internal read boundary — observed splitting at odd
// offsets like 389/1396. Without assembly the handler parses each fragment
// as independent JSON, which fails ("bad JSON from server" with dropped
// chunks). 4 KB covers any tts_chunk frame (~1760 B) with margin.
#define RX_ASSEMBLY_BYTES 4096

static esp_websocket_client_handle_t s_client = NULL;
static EventGroupHandle_t s_events = NULL;
static SemaphoreHandle_t s_expect_mutex = NULL;
static char s_unit_id[24];       // "aa:bb:cc:dd:ee:ff\0"
static char s_fw_version[32];
static char s_expect_request_id[16];    // pending_ready id the voice turn wants
static char s_expect_tts_request_id[16]; // tts stream id the voice turn wants

static ws_tts_handler_t s_tts_handler = NULL;
static void *s_tts_handler_ctx = NULL;
static uint8_t s_tts_decode_buf[TTS_DECODE_BUF_BYTES];

static uint8_t s_rx_assembly[RX_ASSEMBLY_BYTES];
static size_t  s_rx_assembly_len = 0;
static int     s_rx_expected_len = 0;     // -1 if unknown

static void _send_hello(void)
{
    // Keep the frame short; stack-only buffer.
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\"type\":\"hello\","
        "\"unit_id\":\"%s\","
        "\"room\":\"%s\","
        "\"mic_id\":\"%s\","
        "\"fw_version\":\"%s\","
        "\"caps\":[\"voice_turn\",\"two_phase\",\"pending_ready\"]}",
        s_unit_id, CONFIG_DWU_ROOM_NAME, CONFIG_DWU_MIC_ID, s_fw_version);
    if (n <= 0 || n >= (int)sizeof(buf)) {
        ESP_LOGW(TAG, "hello frame truncated (%d)", n);
        return;
    }
    int sent = esp_websocket_client_send_text(s_client, buf, n, pdMS_TO_TICKS(1000));
    if (sent < 0) {
        ESP_LOGW(TAG, "hello send failed");
    }
}

static void _handle_server_event(const char *text, int len)
{
    cJSON *root = cJSON_ParseWithLength(text, len);
    if (!root) {
        ESP_LOGW(TAG, "bad JSON from server (%d bytes)", len);
        return;
    }
    cJSON *type_j = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type_j) || !type_j->valuestring) {
        cJSON_Delete(root);
        return;
    }
    const char *etype = type_j->valuestring;

    if (strcmp(etype, "hello_ack") == 0) {
        xEventGroupSetBits(s_events, BIT_CONNECTED);
        ESP_LOGI(TAG, "session up (unit=%s)", s_unit_id);
    } else if (strcmp(etype, "pending_ready") == 0) {
        cJSON *rid_j = cJSON_GetObjectItemCaseSensitive(root, "request_id");
        const char *rid = cJSON_IsString(rid_j) ? rid_j->valuestring : NULL;
        bool matched = false;
        if (rid) {
            xSemaphoreTake(s_expect_mutex, portMAX_DELAY);
            if (s_expect_request_id[0] && strcmp(rid, s_expect_request_id) == 0) {
                matched = true;
            }
            xSemaphoreGive(s_expect_mutex);
        }
        if (matched) {
            xEventGroupSetBits(s_events, BIT_PENDING_READY);
            ESP_LOGI(TAG, "pending_ready: %s (matched)", rid);
        } else {
            ESP_LOGI(TAG, "pending_ready: %s (ignored, not expected)",
                     rid ? rid : "(null)");
        }
    } else if (strcmp(etype, "tts_start") == 0 ||
               strcmp(etype, "tts_chunk") == 0 ||
               strcmp(etype, "tts_end") == 0) {
        cJSON *rid_j = cJSON_GetObjectItemCaseSensitive(root, "request_id");
        const char *rid = cJSON_IsString(rid_j) ? rid_j->valuestring : NULL;

        // Gate by expected request_id (under the mutex).
        xSemaphoreTake(s_expect_mutex, portMAX_DELAY);
        bool matched = rid && s_expect_tts_request_id[0]
                        && strcmp(rid, s_expect_tts_request_id) == 0;
        xSemaphoreGive(s_expect_mutex);

        if (!matched) {
            ESP_LOGD(TAG, "%s: %s (ignored, not expected)", etype,
                     rid ? rid : "(null)");
            cJSON_Delete(root);
            return;
        }

        ws_tts_event_t evt = { .request_id = rid };

        if (strcmp(etype, "tts_start") == 0) {
            cJSON *sr_j = cJSON_GetObjectItemCaseSensitive(root, "sample_rate");
            cJSON *ch_j = cJSON_GetObjectItemCaseSensitive(root, "channels");
            evt.type = WS_TTS_EVT_START;
            evt.sample_rate = cJSON_IsNumber(sr_j) ? sr_j->valueint : 16000;
            evt.channels    = cJSON_IsNumber(ch_j) ? ch_j->valueint : 1;
            ESP_LOGI(TAG, "tts_start: req=%s sr=%d ch=%d",
                     rid, evt.sample_rate, evt.channels);
            if (s_tts_handler) s_tts_handler(&evt, s_tts_handler_ctx);

        } else if (strcmp(etype, "tts_chunk") == 0) {
            cJSON *seq_j = cJSON_GetObjectItemCaseSensitive(root, "seq");
            cJSON *pay_j = cJSON_GetObjectItemCaseSensitive(root, "payload");
            if (!cJSON_IsString(pay_j) || !pay_j->valuestring) {
                ESP_LOGW(TAG, "tts_chunk missing payload");
                cJSON_Delete(root);
                return;
            }
            size_t decoded = 0;
            int rc = mbedtls_base64_decode(
                s_tts_decode_buf, sizeof(s_tts_decode_buf), &decoded,
                (const unsigned char *)pay_j->valuestring,
                strlen(pay_j->valuestring));
            if (rc != 0) {
                ESP_LOGW(TAG, "tts_chunk base64 decode failed: -0x%04x", -rc);
                cJSON_Delete(root);
                return;
            }
            evt.type = WS_TTS_EVT_CHUNK;
            evt.seq = cJSON_IsNumber(seq_j) ? seq_j->valueint : -1;
            evt.pcm = s_tts_decode_buf;
            evt.pcm_len = decoded;
            if (s_tts_handler) s_tts_handler(&evt, s_tts_handler_ctx);

        } else {  // tts_end
            cJSON *ts_j = cJSON_GetObjectItemCaseSensitive(root, "total_seq");
            evt.type = WS_TTS_EVT_END;
            evt.total_seq = cJSON_IsNumber(ts_j) ? ts_j->valueint : -1;
            ESP_LOGI(TAG, "tts_end: req=%s total_seq=%d", rid, evt.total_seq);
            if (s_tts_handler) s_tts_handler(&evt, s_tts_handler_ctx);
            xEventGroupSetBits(s_events, BIT_TTS_END);
        }
    } else if (strcmp(etype, "barge_in") == 0 ||
               strcmp(etype, "suppress") == 0 ||
               strcmp(etype, "config") == 0) {
        // Stage-4 stubs — land the wire but don't act yet.
        ESP_LOGI(TAG, "received %s (stub, no action)", etype);
    } else {
        ESP_LOGW(TAG, "unknown server event type: %s", etype);
    }
    cJSON_Delete(root);
}

static void _ws_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "TCP connected, sending hello...");
            _send_hello();
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
            xEventGroupClearBits(s_events, BIT_CONNECTED);
            ESP_LOGW(TAG, "disconnected");
            break;
        case WEBSOCKET_EVENT_DATA:
            if ((d->op_code == 0x01 /* TEXT */ || d->op_code == 0x00 /* CONT */)
                    && d->data_len > 0) {
                // New WS frame starts at payload_offset=0. Reset assembly.
                if (d->payload_offset == 0) {
                    s_rx_assembly_len = 0;
                    s_rx_expected_len = d->payload_len;  // may be -1 if unknown
                }
                // Append current fragment.
                if (s_rx_assembly_len + d->data_len <= sizeof(s_rx_assembly)) {
                    memcpy(s_rx_assembly + s_rx_assembly_len,
                            d->data_ptr, d->data_len);
                    s_rx_assembly_len += d->data_len;
                } else {
                    ESP_LOGW(TAG, "rx assembly overflow (%u+%u > %u), dropping",
                             (unsigned)s_rx_assembly_len, (unsigned)d->data_len,
                             (unsigned)sizeof(s_rx_assembly));
                    s_rx_assembly_len = 0;
                    s_rx_expected_len = 0;
                    break;
                }
                // Parse when the frame is complete. If payload_len was
                // unknown, assume each delivery is a complete frame (old
                // behavior).
                bool complete = (s_rx_expected_len <= 0) ||
                                ((int)s_rx_assembly_len >= s_rx_expected_len);
                if (complete) {
                    _handle_server_event((const char *)s_rx_assembly,
                                          s_rx_assembly_len);
                    s_rx_assembly_len = 0;
                    s_rx_expected_len = 0;
                }
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGW(TAG, "transport error");
            break;
        default:
            break;
    }
}

static void _format_mac(char *out, size_t out_cap)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(out, out_cap, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

esp_err_t ws_client_start(const char *fw_version)
{
    if (s_client) return ESP_ERR_INVALID_STATE;

    s_events = xEventGroupCreate();
    s_expect_mutex = xSemaphoreCreateMutex();
    if (!s_events || !s_expect_mutex) return ESP_ERR_NO_MEM;

    _format_mac(s_unit_id, sizeof(s_unit_id));
    strncpy(s_fw_version, fw_version ? fw_version : "unknown",
            sizeof(s_fw_version) - 1);
    s_fw_version[sizeof(s_fw_version) - 1] = '\0';

    char uri[128];
    snprintf(uri, sizeof(uri), "ws://%s:%d/ws",
             CONFIG_DWU_OKDEMERZEL_HOST, CONFIG_DWU_OKDEMERZEL_PORT);

    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .reconnect_timeout_ms = 3000,    // retry cadence when down
        .network_timeout_ms = 10000,
        .ping_interval_sec = 30,
        .pingpong_timeout_sec = 20,
        // tts_chunk frames carry ~1708 chars of base64-encoded PCM plus the
        // JSON envelope (~1760 bytes total). Observed fragmentation at 4096
        // during back-to-back burst of 55+ frames — rx would split a frame
        // and we'd see "bad JSON" warnings as fragments arrived separately.
        // 8192 gives comfortable margin for 2+ frames-per-buffer without
        // forcing assembly logic in _handle_server_event.
        .buffer_size = 8192,
    };

    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) return ESP_FAIL;

    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                   _ws_event_handler, NULL);
    esp_err_t err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        return err;
    }
    ESP_LOGI(TAG, "connecting to %s (unit_id=%s)", uri, s_unit_id);
    return ESP_OK;
}

bool ws_client_is_connected(void)
{
    if (!s_events) return false;
    return (xEventGroupGetBits(s_events) & BIT_CONNECTED) != 0;
}

const char *ws_client_unit_id(void)
{
    return s_unit_id;
}

// Send raw JSON text. Returns ESP_OK if queued or if we're not connected
// (we silently drop — callers treat WS as best-effort).
static esp_err_t _send_text(const char *json, int len)
{
    if (!s_client) return ESP_OK;
    if (!ws_client_is_connected()) return ESP_OK;
    int sent = esp_websocket_client_send_text(s_client, json, len,
                                                pdMS_TO_TICKS(200));
    if (sent < 0) {
        ESP_LOGD(TAG, "send dropped (busy or offline)");
    }
    return ESP_OK;
}

static void _escape_str(char *dst, size_t cap, const char *src)
{
    // Minimal JSON string escaper: handles the characters that can appear
    // in our event fields (room names, state labels, turn IDs). Callers are
    // responsible for passing short, printable ASCII.
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 2 < cap; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            if (j + 3 >= cap) break;
            dst[j++] = '\\';
            dst[j++] = c;
        } else if (c >= 0x20) {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
}

esp_err_t ws_client_send_state(const char *state, const char *turn_id)
{
    char buf[128];
    char s_esc[32], t_esc[32];
    _escape_str(s_esc, sizeof(s_esc), state ? state : "");
    _escape_str(t_esc, sizeof(t_esc), turn_id ? turn_id : "");
    int n;
    if (turn_id && turn_id[0]) {
        n = snprintf(buf, sizeof(buf),
            "{\"type\":\"state\",\"state\":\"%s\",\"turn_id\":\"%s\"}",
            s_esc, t_esc);
    } else {
        n = snprintf(buf, sizeof(buf),
            "{\"type\":\"state\",\"state\":\"%s\"}", s_esc);
    }
    if (n <= 0 || n >= (int)sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    return _send_text(buf, n);
}

esp_err_t ws_client_send_wake(int score_peak)
{
    char buf[96];
    int n = snprintf(buf, sizeof(buf),
        "{\"type\":\"wake\",\"score_peak\":%d}", score_peak);
    if (n <= 0 || n >= (int)sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    return _send_text(buf, n);
}

esp_err_t ws_client_send_env(float temp_c, float humidity, float pressure_hpa)
{
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
        "{\"type\":\"env\",\"temp_c\":%.2f,\"humidity\":%.1f,\"pressure_hpa\":%.2f}",
        temp_c, humidity, pressure_hpa);
    if (n <= 0 || n >= (int)sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    return _send_text(buf, n);
}

esp_err_t ws_client_send_presence(bool pir, bool radar)
{
    char buf[64];
    int n = snprintf(buf, sizeof(buf),
        "{\"type\":\"presence\",\"pir\":%s,\"radar\":%s}",
        pir ? "true" : "false", radar ? "true" : "false");
    if (n <= 0 || n >= (int)sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    return _send_text(buf, n);
}

void ws_client_expect_pending_ready(const char *request_id)
{
    xSemaphoreTake(s_expect_mutex, portMAX_DELAY);
    if (request_id) {
        strncpy(s_expect_request_id, request_id,
                sizeof(s_expect_request_id) - 1);
        s_expect_request_id[sizeof(s_expect_request_id) - 1] = '\0';
    } else {
        s_expect_request_id[0] = '\0';
    }
    xSemaphoreGive(s_expect_mutex);
    // Clear any previously-set bit so a stale matching event doesn't fire.
    xEventGroupClearBits(s_events, BIT_PENDING_READY);
}

esp_err_t ws_client_wait_pending_ready(uint32_t timeout_ms)
{
    if (!s_events) return ESP_ERR_INVALID_STATE;
    EventBits_t bits = xEventGroupWaitBits(
        s_events, BIT_PENDING_READY,
        pdTRUE /* clear on exit */,
        pdFALSE /* wait any */,
        pdMS_TO_TICKS(timeout_ms));
    if (bits & BIT_PENDING_READY) {
        // Clear the expected id so a spurious late duplicate can't re-fire.
        xSemaphoreTake(s_expect_mutex, portMAX_DELAY);
        s_expect_request_id[0] = '\0';
        xSemaphoreGive(s_expect_mutex);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

// --- TTS streaming ---

void ws_client_set_tts_handler(ws_tts_handler_t handler, void *ctx)
{
    s_tts_handler = handler;
    s_tts_handler_ctx = ctx;
}

void ws_client_expect_tts_stream(const char *request_id)
{
    xSemaphoreTake(s_expect_mutex, portMAX_DELAY);
    if (request_id && request_id[0]) {
        strncpy(s_expect_tts_request_id, request_id,
                sizeof(s_expect_tts_request_id) - 1);
        s_expect_tts_request_id[sizeof(s_expect_tts_request_id) - 1] = '\0';
    } else {
        s_expect_tts_request_id[0] = '\0';
    }
    xSemaphoreGive(s_expect_mutex);
    // Clear any previously-set bit so a stale matching event doesn't fire.
    xEventGroupClearBits(s_events, BIT_TTS_END);
}

esp_err_t ws_client_wait_tts_end(uint32_t timeout_ms)
{
    if (!s_events) return ESP_ERR_INVALID_STATE;
    EventBits_t bits = xEventGroupWaitBits(
        s_events, BIT_TTS_END,
        pdTRUE /* clear on exit */,
        pdFALSE /* wait any */,
        pdMS_TO_TICKS(timeout_ms));
    // Always clear expectation, regardless of outcome.
    xSemaphoreTake(s_expect_mutex, portMAX_DELAY);
    s_expect_tts_request_id[0] = '\0';
    xSemaphoreGive(s_expect_mutex);
    return (bits & BIT_TTS_END) ? ESP_OK : ESP_ERR_TIMEOUT;
}
