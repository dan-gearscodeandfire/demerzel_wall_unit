idf_component_register(
    SRCS "bme280.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c freertos log
)
