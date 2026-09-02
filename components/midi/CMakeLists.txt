idf_component_register(
    SRCS "midi.c" "onboard_synth.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_timer voice_midi driver esp_driver_i2s esp_rom
)
