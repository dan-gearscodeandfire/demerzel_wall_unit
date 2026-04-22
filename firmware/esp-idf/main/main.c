#include "wifi.h"
#include "voice_turn.h"
#include "ota.h"
#include "log_server.h"
#include "status_led.h"
#include "pir.h"
#include "bme280.h"
#include "ld2410c.h"
#include "audio_in.h"
#include "audio_out.h"
#include "wake_word_task.h"
#include "followup.h"
#include "ws_client.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include <math.h>

static const char *TAG = "dwu";

#define WAKE_WORD_BIT   BIT0

static EventGroupHandle_t s_trigger_events;

// PIR callback retained for future presence-gating use; no longer triggers
// voice_turn. The wake-word task is the sole trigger source.
static void pir_callback(bool motion)
{
    (void)motion;
}

static void log_heap_stats(void)
{
    ESP_LOGI(TAG, "Heap: %lu free, PSRAM: %lu free, min ever: %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned long)esp_get_minimum_free_heap_size());
}

static void mdns_register(void)
{
    char hostname[32];
    snprintf(hostname, sizeof(hostname), "dwu-%s", CONFIG_DWU_ROOM_NAME);

    mdns_init();
    mdns_hostname_set(hostname);

    char instance[64];
    snprintf(instance, sizeof(instance), "Demerzel Wall Unit (%s)", CONFIG_DWU_ROOM_NAME);
    mdns_instance_name_set(instance);

    mdns_service_add(NULL, "_dwu-log", "_tcp", CONFIG_DWU_LOG_TCP_PORT, NULL, 0);
    ESP_LOGI(TAG, "mDNS: %s.local", hostname);
}

#ifdef CONFIG_DWU_TEST_ALL_DRIVERS
static void run_driver_tests(void)
{
    ESP_LOGI(TAG, "=== DRIVER SELF-TEST ===");

    // LED
    ESP_LOGI(TAG, "[1/6] status_led");
    led_state_t colors[] = {LED_RED, LED_GREEN, LED_BLUE, LED_AMBER, LED_WHITE, LED_OFF};
    for (int i = 0; i < 6; i++) {
        status_led_set(colors[i]);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "  PASS: LED cycled");

    // PIR
    ESP_LOGI(TAG, "[2/6] pir (10s)");
    for (int i = 0; i < 50; i++) {
        bool state = pir_get_state();
        if (i % 10 == 0) ESP_LOGI(TAG, "  PIR=%d", state);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGI(TAG, "  PASS: PIR polled");

    // BME280
    ESP_LOGI(TAG, "[3/6] bme280");
    bme280_reading_t reading;
    if (bme280_read(&reading) == ESP_OK) {
        ESP_LOGI(TAG, "  T=%.1fC H=%.1f%% P=%.1f hPa", reading.temperature_c, reading.humidity_pct, reading.pressure_hpa);
        ESP_LOGI(TAG, "  PASS: BME280");
    } else {
        ESP_LOGW(TAG, "  FAIL: BME280 read");
    }

    // LD2410C
    ESP_LOGI(TAG, "[4/6] ld2410c (10s)");
    for (int i = 0; i < 50; i++) {
        ld2410c_state_t state;
        if (ld2410c_get_state(&state) == ESP_OK && i % 10 == 0) {
            ESP_LOGI(TAG, "  presence=%d target=%d uart_valid=%d",
                     state.presence, state.target_state, state.uart_valid);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGI(TAG, "  PASS: LD2410C polled");

    // Mic — audio_in is already initialized in app_main before this runs.
    ESP_LOGI(TAG, "[5/6] audio_in (3s record)");
    size_t test_samples = 16000 * 3;
    int16_t *test_buf = heap_caps_malloc(test_samples * 2, MALLOC_CAP_SPIRAM);
    if (test_buf) {
        size_t actual;
        if (audio_in_record(test_buf, test_samples, &actual) == ESP_OK) {
            int32_t min_val = 32767, max_val = -32768;
            int64_t rms_sum = 0;
            for (size_t i = 0; i < actual; i++) {
                int16_t s = test_buf[i];
                if (s < min_val) min_val = s;
                if (s > max_val) max_val = s;
                rms_sum += (int64_t)s * s;
            }
            float rms = sqrtf((float)(rms_sum / (int64_t)actual));
            ESP_LOGI(TAG, "  min=%ld max=%ld rms=%.1f pp=%ld",
                     (long)min_val, (long)max_val, rms, (long)(max_val - min_val));
            ESP_LOGI(TAG, "  PASS: audio_in");
        } else {
            ESP_LOGW(TAG, "  FAIL: audio_in record");
        }
        heap_caps_free(test_buf);
    }

    // Amp
    ESP_LOGI(TAG, "[6/6] audio_out (440 Hz, 2s)");
    audio_out_init(16000, 16, 1);
    audio_out_unmute();
    int16_t tone[16000];
    int samples_per_period = 16000 / 440;
    for (int i = 0; i < 16000; i++) {
        tone[i] = (int16_t)(16000.0f * sinf(2.0f * 3.14159265f * (float)(i % samples_per_period) / (float)samples_per_period));
    }
    for (int rep = 0; rep < 2; rep++) {
        size_t written;
        audio_out_write(tone, sizeof(tone), &written);
    }
    audio_out_mute();
    audio_out_deinit();
    ESP_LOGI(TAG, "  PASS: audio_out (you should have heard a 440 Hz tone)");

    ESP_LOGI(TAG, "=== ALL DRIVER TESTS COMPLETE ===");
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "Demerzel Wall Unit starting...");

    // NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Status LED (immediate visual feedback)
    status_led_init();
    status_led_set(LED_WHITE);

    // WiFi
    status_led_set(LED_AMBER);
    ret = wifi_init_sta();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connection failed");
        status_led_set(LED_RED);
        vTaskDelay(pdMS_TO_TICKS(5000));
    } else {
        status_led_set(LED_GREEN);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // mDNS
    mdns_register();

    // TCP log server
    log_server_init();
    ESP_LOGI(TAG, "TCP log server started on port %d", CONFIG_DWU_LOG_TCP_PORT);

    // Peripheral drivers
    pir_init(pir_callback);
    bme280_init();
    ld2410c_init();

    // Audio capture is always-on from this point — wake-word task drains the
    // wake ring continuously; voice_turn arms the capture ring on demand.
    ret = audio_in_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio_in_init failed: %s", esp_err_to_name(ret));
        status_led_set(LED_RED);
    }

    log_heap_stats();

    // OTA check (non-blocking if no update available)
    ota_init();
    ota_mark_valid();

    ESP_LOGI(TAG, "FW version: %s, room: %s, mic: %s",
             ota_get_current_version(), CONFIG_DWU_ROOM_NAME, CONFIG_DWU_MIC_ID);

    // Signaling-only WebSocket to voice_server. Best-effort — voice turns
    // keep working over HTTP if this channel is down.
    ws_client_start(ota_get_current_version());

#ifdef CONFIG_DWU_TEST_ALL_DRIVERS
    run_driver_tests();
    ESP_LOGI(TAG, "Test mode — halting. Disable DWU_TEST_ALL_DRIVERS for normal operation.");
    while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); }
#endif

    // Main voice_turn loop — gated on wake-word, not PIR.
    s_trigger_events = xEventGroupCreate();

    ret = wake_word_task_start(s_trigger_events, WAKE_WORD_BIT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wake_word_task_start failed: %s", esp_err_to_name(ret));
        status_led_set(LED_RED);
    } else {
        ESP_LOGI(TAG, "Ready — listening for wake word (\"Yo Demerzel\")");
    }
    status_led_set(LED_OFF);

    ws_client_send_state("idle", NULL);
    while (1) {
        xEventGroupWaitBits(s_trigger_events, WAKE_WORD_BIT,
                            pdTRUE, pdFALSE, portMAX_DELAY);

        ws_client_send_wake((int)wake_word_task_last_score());

        if (!wifi_is_connected()) {
            ESP_LOGW(TAG, "WiFi not connected, skipping voice turn");
            status_led_set(LED_RED);
            vTaskDelay(pdMS_TO_TICKS(2000));
            status_led_set(LED_OFF);
            ws_client_send_state("idle", NULL);
            continue;
        }

        // Pause wake-word for the entire conversation chain (initial turn +
        // any followup turns). Resume only when the chain ends.
        wake_word_task_pause();

        bool do_turn = true;
        while (do_turn) {
            ret = voice_turn_execute();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "voice_turn failed: %s", esp_err_to_name(ret));
                status_led_set(LED_RED);
                vTaskDelay(pdMS_TO_TICKS(2000));
                break;
            }

            // Followup window — dim teal LED signals "still listening".
            status_led_set_rgb(0, 40, 60);
            ws_client_send_state("followup", NULL);
            do_turn = followup_detect_speech();
            if (do_turn) {
                ESP_LOGI(TAG, "Followup speech detected — another turn");
            }
        }

        status_led_set(LED_OFF);
        vTaskDelay(pdMS_TO_TICKS(500));
        wake_word_task_resume();
        ws_client_send_state("idle", NULL);
        log_heap_stats();
    }
}
