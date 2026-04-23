#include "opus_stream.h"
#include <opus.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "opus_stream";

#define INITIAL_CAP   (8 * 1024)
// Max encoded Opus packet size per RFC 6716. Our typical VOIP 16 kHz/24 kbps
// packets are ~60 bytes; 1500 is comfortable headroom without exploding.
// Heap-allocated (not on caller's stack) so the main task stack doesn't
// have to carry it through each encode call.
#define MAX_PACKET    1500

struct opus_stream {
    OpusEncoder *encoder;
    int sample_rate;
    int channels;
    int frame_samples;
    uint8_t *buf;
    size_t buf_used;
    size_t buf_cap;
    uint8_t *packet_scratch;  // MAX_PACKET bytes, reused per frame
};

esp_err_t opus_stream_create(int sample_rate, int channels, int bitrate_bps,
                              opus_stream_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;

    opus_stream_t *s = heap_caps_calloc(1, sizeof(*s),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s) return ESP_ERR_NO_MEM;

    int err;
    s->encoder = opus_encoder_create(sample_rate, channels,
                                      OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK) {
        ESP_LOGE(TAG, "opus_encoder_create failed: %d", err);
        heap_caps_free(s);
        return ESP_FAIL;
    }
    opus_encoder_ctl(s->encoder, OPUS_SET_BITRATE(bitrate_bps));
    opus_encoder_ctl(s->encoder, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(s->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    s->sample_rate = sample_rate;
    s->channels = channels;
    s->frame_samples = sample_rate / 50;  // 20 ms frames

    s->buf_cap = INITIAL_CAP;
    s->buf = heap_caps_malloc(s->buf_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s->packet_scratch = heap_caps_malloc(MAX_PACKET,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->buf || !s->packet_scratch) {
        if (s->buf) heap_caps_free(s->buf);
        if (s->packet_scratch) heap_caps_free(s->packet_scratch);
        opus_encoder_destroy(s->encoder);
        heap_caps_free(s);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "encoder ready: %d Hz, %d ch, %d bps, %d samples/frame",
             sample_rate, channels, bitrate_bps, s->frame_samples);
    *out = s;
    return ESP_OK;
}

static esp_err_t grow_buf(opus_stream_t *s, size_t need)
{
    if (s->buf_used + need <= s->buf_cap) return ESP_OK;
    size_t new_cap = s->buf_cap;
    while (new_cap < s->buf_used + need) new_cap *= 2;
    uint8_t *new_buf = heap_caps_realloc(s->buf, new_cap,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!new_buf) return ESP_ERR_NO_MEM;
    s->buf = new_buf;
    s->buf_cap = new_cap;
    return ESP_OK;
}

esp_err_t opus_stream_encode_frame(opus_stream_t *s, const int16_t *pcm,
                                    int frame_samples)
{
    if (!s || !pcm || !s->buf) return ESP_ERR_INVALID_ARG;
    if (frame_samples != s->frame_samples) {
        ESP_LOGE(TAG, "frame_samples=%d, expected=%d",
                 frame_samples, s->frame_samples);
        return ESP_ERR_INVALID_ARG;
    }

    int nbytes = opus_encode(s->encoder, pcm, frame_samples,
                              s->packet_scratch, MAX_PACKET);
    if (nbytes < 0) {
        ESP_LOGE(TAG, "opus_encode failed: %d", nbytes);
        return ESP_FAIL;
    }
    if (nbytes <= 2) {
        // DTX (<=2 byte) — whisper doesn't need strict alignment so skip.
        return ESP_OK;
    }

    esp_err_t err = grow_buf(s, 2 + (size_t)nbytes);
    if (err != ESP_OK) return err;

    // [u16 LE length][packet bytes]
    s->buf[s->buf_used++] = (uint8_t)(nbytes & 0xFF);
    s->buf[s->buf_used++] = (uint8_t)((nbytes >> 8) & 0xFF);
    memcpy(s->buf + s->buf_used, s->packet_scratch, (size_t)nbytes);
    s->buf_used += (size_t)nbytes;
    return ESP_OK;
}

esp_err_t opus_stream_finalize(opus_stream_t *s, uint8_t **out_buf, size_t *out_len)
{
    if (!s || !out_buf || !out_len) return ESP_ERR_INVALID_ARG;
    *out_buf = s->buf;
    *out_len = s->buf_used;
    s->buf = NULL;
    s->buf_used = 0;
    s->buf_cap = 0;
    return ESP_OK;
}

void opus_stream_destroy(opus_stream_t *s)
{
    if (!s) return;
    if (s->encoder) opus_encoder_destroy(s->encoder);
    if (s->buf) heap_caps_free(s->buf);
    if (s->packet_scratch) heap_caps_free(s->packet_scratch);
    heap_caps_free(s);
}
