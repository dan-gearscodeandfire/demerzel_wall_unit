idf_component_register(
    SRCS "status_led.c"
    INCLUDE_DIRS "."
    REQUIRES driver
    PRIV_REQUIRES led_strip
)
