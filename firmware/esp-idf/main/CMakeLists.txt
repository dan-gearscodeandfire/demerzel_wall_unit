idf_component_register(
    SRCS "main.c" "wifi.c" "voice_turn.c" "wav_util.c" "http_client.c" "ota.c" "log_server.c" "wake_word_task.c" "followup.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_http_client esp_https_ota app_update esp_psram json freertos esp_timer esp_event esp_netif lwip log
    PRIV_REQUIRES audio_in audio_out bme280 ld2410c pir status_led wake_word
)
