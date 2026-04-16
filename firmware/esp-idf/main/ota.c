#include "ota.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ota";

#define OTA_CHECK_INTERVAL_MS (60 * 60 * 1000)
#define NVS_NAMESPACE         "dwu"
#define NVS_KEY_FW_VERSION    "fw_version"
#define FW_VERSION_DEFAULT    "0.1.0"

static char s_current_version[32] = FW_VERSION_DEFAULT;
static TimerHandle_t s_ota_timer = NULL;

static void load_version_from_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_current_version);
        if (nvs_get_str(nvs, NVS_KEY_FW_VERSION, s_current_version, &len) != ESP_OK) {
            strncpy(s_current_version, FW_VERSION_DEFAULT, sizeof(s_current_version));
        }
        nvs_close(nvs);
    }
}

static void save_version_to_nvs(const char *version)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, NVS_KEY_FW_VERSION, version);
        nvs_commit(nvs);
        nvs_close(nvs);
        strncpy(s_current_version, version, sizeof(s_current_version));
    }
}

static char *fetch_manifest(void)
{
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/ota/manifest.json",
             CONFIG_DWU_OKDEMERZEL_HOST, CONFIG_DWU_OKDEMERZEL_PORT);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return NULL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return NULL;
    }

    int content_len = esp_http_client_fetch_headers(client);
    if (content_len <= 0 || content_len > 4096) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }

    char *buf = malloc(content_len + 1);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }

    int read = esp_http_client_read(client, buf, content_len);
    buf[read > 0 ? read : 0] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return buf;
}

esp_err_t ota_check_and_update(void)
{
    ESP_LOGI(TAG, "Checking for OTA update (current: %s)...", s_current_version);

    char *manifest_json = fetch_manifest();
    if (!manifest_json) {
        ESP_LOGW(TAG, "Could not fetch OTA manifest");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(manifest_json);
    free(manifest_json);
    if (!root) {
        ESP_LOGW(TAG, "Invalid manifest JSON");
        return ESP_FAIL;
    }

    const cJSON *ver = cJSON_GetObjectItem(root, "version");
    const cJSON *url = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(ver) || !cJSON_IsString(url)) {
        ESP_LOGW(TAG, "Manifest missing version or url");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    if (strcmp(ver->valuestring, s_current_version) == 0) {
        ESP_LOGI(TAG, "Already running latest version %s", s_current_version);
        cJSON_Delete(root);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "New version available: %s (current: %s)", ver->valuestring, s_current_version);
    ESP_LOGI(TAG, "Downloading from %s", url->valuestring);

    esp_http_client_config_t ota_config = {
        .url = url->valuestring,
        .timeout_ms = 120000,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &ota_config,
    };

    esp_err_t ret = esp_https_ota(&ota_cfg);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA succeeded — saving version %s and rebooting", ver->valuestring);
        save_version_to_nvs(ver->valuestring);
        cJSON_Delete(root);
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
    }

    cJSON_Delete(root);
    return ret;
}

void ota_mark_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "Marking current firmware as valid");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
}

static void ota_timer_callback(TimerHandle_t timer)
{
    ota_check_and_update();
}

const char *ota_get_current_version(void)
{
    return s_current_version;
}

esp_err_t ota_init(void)
{
    load_version_from_nvs();
    ESP_LOGI(TAG, "Firmware version: %s", s_current_version);

    ota_check_and_update();

    s_ota_timer = xTimerCreate("ota_check", pdMS_TO_TICKS(OTA_CHECK_INTERVAL_MS),
                                pdTRUE, NULL, ota_timer_callback);
    if (s_ota_timer) {
        xTimerStart(s_ota_timer, 0);
    }

    return ESP_OK;
}
