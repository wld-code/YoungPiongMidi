idf_component_register(
    SRCS "audio_capture.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_adc board esp_timer
)
