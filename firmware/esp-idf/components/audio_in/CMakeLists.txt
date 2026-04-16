idf_component_register(
    SRCS "audio_in.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2s esp_psram freertos log
)
