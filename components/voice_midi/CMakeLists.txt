idf_component_register(
    SRCS "voice_midi.c" "note_state_machine.c" "expression.c"
    INCLUDE_DIRS "include"
    REQUIRES board
)
