#include "ws_client.h"
#include "esp_websocket_client.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ws_client";

#define BIT_CONNECTED       BIT0   // hello_ack received
#define BIT_PENDING_READY   BIT1   // a pending_ready matching expected id arrived

static esp_websocket_client_handle_t s_client = NULL;
static EventGroupHandle_t s_events = NULL;
static SemaphoreHandle_t s_expect_mutex = NULL;
static char s_unit_id[24];       // "aa:bb:cc:dd:ee:ff\0"
static char s_fw_version[32];
static char s_expect_request_id[16];  // request_id the voice turn is waiting on

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
            if (d->op_code == 0x01 /* TEXT */ && d->data_len > 0) {
                _handle_server_event((const char *)d->data_ptr, d->data_len);
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
        .buffer_size = 2048,             // events are small
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
