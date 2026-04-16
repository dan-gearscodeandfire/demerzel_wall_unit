set(MICROFRONTEND_DIR microfrontend/tensorflow/lite/experimental/microfrontend/lib)

idf_component_register(
    SRCS
        "wake_word.cc"
        "frontend_tflm.cc"
        "frontend_handroll.c"
        # Vendored TFLM legacy audio frontend (microWakeWord training pipeline).
        "${MICROFRONTEND_DIR}/frontend.c"
        "${MICROFRONTEND_DIR}/frontend_util.c"
        "${MICROFRONTEND_DIR}/window.c"
        "${MICROFRONTEND_DIR}/window_util.c"
        "${MICROFRONTEND_DIR}/fft.cc"
        "${MICROFRONTEND_DIR}/fft_util.cc"
        "${MICROFRONTEND_DIR}/filterbank.c"
        "${MICROFRONTEND_DIR}/filterbank_util.c"
        "${MICROFRONTEND_DIR}/noise_reduction.c"
        "${MICROFRONTEND_DIR}/noise_reduction_util.c"
        "${MICROFRONTEND_DIR}/pcan_gain_control.c"
        "${MICROFRONTEND_DIR}/pcan_gain_control_util.c"
        "${MICROFRONTEND_DIR}/log_scale.c"
        "${MICROFRONTEND_DIR}/log_scale_util.c"
        "${MICROFRONTEND_DIR}/log_lut.c"
        "${MICROFRONTEND_DIR}/kiss_fft_int16.cc"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "."
        "microfrontend"          # for #include "tensorflow/lite/experimental/microfrontend/lib/*.h"
        "microfrontend/kissfft"  # for kiss_fft_int16.cc's #include "kiss_fft.c" and "tools/kiss_fftr.c"
    REQUIRES
        esp_psram
        freertos
        log
    PRIV_REQUIRES
        espressif__esp-tflite-micro
    EMBED_FILES
        "models/yo_demerzel_v1.tflite"
)

# Vendored microfrontend triggers a few unused/sign-compare warnings under
# -Werror=all; soften them just for these files.
set(MICROFRONTEND_SOURCES
    "${MICROFRONTEND_DIR}/frontend.c"
    "${MICROFRONTEND_DIR}/frontend_util.c"
    "${MICROFRONTEND_DIR}/window.c"
    "${MICROFRONTEND_DIR}/window_util.c"
    "${MICROFRONTEND_DIR}/fft.cc"
    "${MICROFRONTEND_DIR}/fft_util.cc"
    "${MICROFRONTEND_DIR}/filterbank.c"
    "${MICROFRONTEND_DIR}/filterbank_util.c"
    "${MICROFRONTEND_DIR}/noise_reduction.c"
    "${MICROFRONTEND_DIR}/noise_reduction_util.c"
    "${MICROFRONTEND_DIR}/pcan_gain_control.c"
    "${MICROFRONTEND_DIR}/pcan_gain_control_util.c"
    "${MICROFRONTEND_DIR}/log_scale.c"
    "${MICROFRONTEND_DIR}/log_scale_util.c"
    "${MICROFRONTEND_DIR}/log_lut.c"
    "${MICROFRONTEND_DIR}/kiss_fft_int16.cc"
)
set_source_files_properties(
    ${MICROFRONTEND_SOURCES}
    PROPERTIES COMPILE_FLAGS "-Wno-error=all -Wno-unused-function -Wno-unused-variable"
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
