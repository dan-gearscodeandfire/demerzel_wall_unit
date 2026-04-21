#include "http_client.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <strings.h>

static const char *TAG = "http_client";

#define MAX_RESPONSE_SIZE (2 * 1024 * 1024)

typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
    voice_turn_meta_t *meta;
} response_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    response_ctx_t *ctx = (response_ctx_t *)evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            if (evt->header_key && evt->header_value && ctx && ctx->meta) {
                if (strcasecmp(evt->header_key, "X-Transcript") == 0) {
                    strncpy(ctx->meta->transcript, evt->header_value,
                            sizeof(ctx->meta->transcript) - 1);
                    ctx->meta->transcript[sizeof(ctx->meta->transcript) - 1] = '\0';
                } else if (strcasecmp(evt->header_key, "X-Reply-Text") == 0) {
                    strncpy(ctx->meta->reply_text, evt->header_value,
                            sizeof(ctx->meta->reply_text) - 1);
                    ctx->meta->reply_text[sizeof(ctx->meta->reply_text) - 1] = '\0';
                } else if (strcasecmp(evt->header_key, "X-Latency-Ms") == 0) {
                    ctx->meta->latency_ms = atoi(evt->header_value);
                } else if (strcasecmp(evt->header_key, "X-DWU-Pending") == 0) {
                    strncpy(ctx->meta->pending_id, evt->header_value,
                            sizeof(ctx->meta->pending_id) - 1);
                    ctx->meta->pending_id[sizeof(ctx->meta->pending_id) - 1] = '\0';
                }
            }
            break;

        case HTTP_EVENT_ON_DATA:
            if (ctx && evt->data && evt->data_len > 0) {
                if (ctx->len + evt->data_len > ctx->cap) {
                    size_t new_cap = ctx->cap * 2;
                    if (new_cap < ctx->len + evt->data_len)
                        new_cap = ctx->len + evt->data_len + 4096;
                    if (new_cap > MAX_RESPONSE_SIZE) return ESP_FAIL;
                    uint8_t *new_buf = heap_caps_realloc(ctx->buf, new_cap,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (!new_buf) return ESP_ERR_NO_MEM;
                    ctx->buf = new_buf;
                    ctx->cap = new_cap;
                }
                memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
                ctx->len += evt->data_len;
            }
            break;

        default:
            break;
    }
    return ESP_OK;
}

esp_err_t http_post_voice_turn(const uint8_t *wav_data, size_t wav_len,
                                uint8_t **out_wav, size_t *out_wav_len,
                                voice_turn_meta_t *meta)
{
    memset(meta, 0, sizeof(*meta));

    response_ctx_t ctx = {
        .buf = heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .len = 0,
        .cap = 64 * 1024,
        .meta = meta,
    };
    if (!ctx.buf) return ESP_ERR_NO_MEM;

    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/voice_turn",
             CONFIG_DWU_OKDEMERZEL_HOST, CONFIG_DWU_OKDEMERZEL_PORT);

    // timeout_ms applies per network operation (connect, send, recv). A dead
    // or hung server can still drag total wall time to ~3x this before we
    // fully give up, but keep it tight enough that the user doesn't stare at
    // a blue LED for minutes when okDemerzel crashes.
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        heap_caps_free(ctx.buf);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    esp_http_client_set_post_field(client, (const char *)wav_data, (int)wav_len);

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
        heap_caps_free(ctx.buf);
        esp_http_client_cleanup(client);
        return err;
    }

    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "Server returned HTTP %d", status);
        heap_caps_free(ctx.buf);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    esp_http_client_cleanup(client);

    *out_wav = ctx.buf;
    *out_wav_len = ctx.len;

    ESP_LOGI(TAG, "Response: %u bytes, server latency %d ms%s",
             (unsigned)ctx.len, meta->latency_ms,
             meta->pending_id[0] ? " [TWO-PHASE]" : "");
    return ESP_OK;
}

esp_err_t http_get_voice_result(const char *request_id,
                                 uint8_t **out_wav, size_t *out_wav_len,
                                 voice_turn_meta_t *meta)
{
    memset(meta, 0, sizeof(*meta));

    response_ctx_t ctx = {
        .buf = heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .len = 0,
        .cap = 64 * 1024,
        .meta = meta,
    };
    if (!ctx.buf) return ESP_ERR_NO_MEM;

    char url[160];
    snprintf(url, sizeof(url), "http://%s:%d/voice_result/%s",
             CONFIG_DWU_OKDEMERZEL_HOST, CONFIG_DWU_OKDEMERZEL_PORT, request_id);

    // Longer timeout: server long-polls up to 30 s, give 5 s extra headroom.
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 35000,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        heap_caps_free(ctx.buf);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "voice_result GET failed: err=%s status=%d",
                 esp_err_to_name(err), status);
        heap_caps_free(ctx.buf);
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    *out_wav = ctx.buf;
    *out_wav_len = ctx.len;

    ESP_LOGI(TAG, "voice_result: %u bytes", (unsigned)ctx.len);
    return ESP_OK;
}
