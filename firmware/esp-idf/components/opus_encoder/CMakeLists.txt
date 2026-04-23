idf_component_register(
    SRCS "opus_stream.c"
    INCLUDE_DIRS "."
    REQUIRES esp_common log
    PRIV_REQUIRES esphome__micro-opus
)
