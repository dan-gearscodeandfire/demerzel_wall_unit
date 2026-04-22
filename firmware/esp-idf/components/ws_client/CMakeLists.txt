idf_component_register(
    SRCS "ws_client.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi json freertos esp_timer esp_event log
    PRIV_REQUIRES espressif__esp_websocket_client
)
