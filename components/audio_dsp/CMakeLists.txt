idf_component_register(
    SRCS "audio_dsp.c" "filters.c" "rms.c" "envelope.c" "noise_gate.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES audio_capture board pitch esp_timer
)
