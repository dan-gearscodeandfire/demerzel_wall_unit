#include "wav_util.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "wav_util";

esp_err_t wav_wrap(const int16_t *pcm, size_t num_samples, uint32_t sample_rate,
                   uint8_t **out_wav, size_t *out_wav_len)
{
    uint16_t channels = 1;
    uint16_t bps = 16;
    uint32_t byte_rate = sample_rate * channels * bps / 8;
    uint16_t block_align = channels * bps / 8;
    uint32_t data_size = num_samples * block_align;
    uint32_t wav_size = 44 + data_size;

    uint8_t *wav = heap_caps_malloc(wav_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) return ESP_ERR_NO_MEM;

    // RIFF header
    memcpy(wav, "RIFF", 4);
    uint32_t riff_size = wav_size - 8;
    memcpy(wav + 4, &riff_size, 4);
    memcpy(wav + 8, "WAVE", 4);

    // fmt chunk
    memcpy(wav + 12, "fmt ", 4);
    uint32_t fmt_size = 16;
    memcpy(wav + 16, &fmt_size, 4);
    uint16_t audio_fmt = 1;  // PCM
    memcpy(wav + 20, &audio_fmt, 2);
    memcpy(wav + 22, &channels, 2);
    memcpy(wav + 24, &sample_rate, 4);
    memcpy(wav + 28, &byte_rate, 4);
    memcpy(wav + 32, &block_align, 2);
    memcpy(wav + 34, &bps, 2);

    // data chunk
    memcpy(wav + 36, "data", 4);
    memcpy(wav + 40, &data_size, 4);
    memcpy(wav + 44, pcm, data_size);

    *out_wav = wav;
    *out_wav_len = wav_size;
    return ESP_OK;
}

esp_err_t wav_parse(const uint8_t *wav, size_t wav_len, wav_header_info_t *info)
{
    if (wav_len < 44) return ESP_ERR_INVALID_SIZE;
    if (memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Not a WAV file");
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));
    info->bits_per_sample = 16;
    info->channels = 1;

    size_t p = 12;
    while (p + 8 <= wav_len) {
        uint32_t chunk_size;
        memcpy(&chunk_size, wav + p + 4, 4);

        if (memcmp(wav + p, "fmt ", 4) == 0 && p + 8 + 16 <= wav_len) {
            memcpy(&info->channels, wav + p + 10, 2);
            memcpy(&info->sample_rate, wav + p + 12, 4);
            memcpy(&info->bits_per_sample, wav + p + 22, 2);
        } else if (memcmp(wav + p, "data", 4) == 0) {
            info->data_offset = p + 8;
            info->data_size = chunk_size;
            break;
        }
        p += 8 + chunk_size;
    }

    if (info->data_offset == 0 || info->sample_rate == 0) {
        ESP_LOGE(TAG, "WAV missing fmt or data chunk");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "WAV: %lu Hz, %u ch, %u bps, data=%u bytes @ offset %u",
             info->sample_rate, info->channels, info->bits_per_sample,
             (unsigned)info->data_size, (unsigned)info->data_offset);
    return ESP_OK;
}
