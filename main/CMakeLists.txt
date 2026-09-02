idf_component_register(
    SRCS "main.c" "self_test.c"
    INCLUDE_DIRS "."
    REQUIRES board audio_capture audio_dsp display esp_timer esp_driver_i2s esp_rom voice_midi midi
)
