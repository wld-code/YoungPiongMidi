# Host-side test build for YoungPiongMidi's hardware-independent DSP/
# conversion code (project spec section 17: "DSP algorithms should be
# testable without the ESP32 when possible").
#
# Each test_*.c file builds into its own standalone executable (see
# test_common.h for why) linking only the plain-C component sources it
# actually exercises - no ESP-IDF, no target board, no simulator.
#
# Usage:
#   make            build and run every test, stop at the first failure
#   make test       same as above
#   make build/test_pitch && ./build/test_pitch     run just one

CC       ?= cc
CFLAGS   ?= -std=c11 -Wall -Wextra -O2 -g
LDLIBS   := -lm

ROOT       := ..
BOARD_INC  := $(ROOT)/components/board/include
DSP_DIR    := $(ROOT)/components/audio_dsp
PITCH_INC  := $(ROOT)/components/pitch/include
PITCH_DIR  := $(ROOT)/components/pitch
VMIDI_INC  := $(ROOT)/components/voice_midi/include
VMIDI_DIR  := $(ROOT)/components/voice_midi

INCLUDES := -I. -I$(BOARD_INC) -I$(DSP_DIR) -I$(PITCH_INC) -I$(VMIDI_INC)

BUILD := build

TESTS := test_midi_notes test_rms test_envelope test_pitch test_note_state_machine test_dynamics test_expression test_noise_gate

.PHONY: test all clean
test: all
	@echo "==================================================="
	@ok=1; \
	for t in $(TESTS); do \
		echo "--- running $$t ---"; \
		./$(BUILD)/$$t || ok=0; \
		echo; \
	done; \
	if [ $$ok -eq 1 ]; then \
		echo "ALL TEST SUITES PASSED"; \
	else \
		echo "SOME TEST SUITES FAILED"; \
		exit 1; \
	fi

all: $(addprefix $(BUILD)/,$(TESTS))

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_midi_notes: test_midi_notes.c $(VMIDI_DIR)/voice_midi.c test_common.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_midi_notes.c $(VMIDI_DIR)/voice_midi.c $(LDLIBS)

$(BUILD)/test_rms: test_rms.c $(DSP_DIR)/rms.c test_common.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_rms.c $(DSP_DIR)/rms.c $(LDLIBS)

$(BUILD)/test_envelope: test_envelope.c $(DSP_DIR)/envelope.c test_common.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_envelope.c $(DSP_DIR)/envelope.c $(LDLIBS)

$(BUILD)/test_pitch: test_pitch.c $(PITCH_DIR)/yin.c $(VMIDI_DIR)/voice_midi.c test_common.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_pitch.c $(PITCH_DIR)/yin.c $(VMIDI_DIR)/voice_midi.c $(LDLIBS)

$(BUILD)/test_note_state_machine: test_note_state_machine.c $(VMIDI_DIR)/voice_midi.c $(VMIDI_DIR)/note_state_machine.c test_common.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_note_state_machine.c $(VMIDI_DIR)/voice_midi.c $(VMIDI_DIR)/note_state_machine.c $(LDLIBS)

$(BUILD)/test_dynamics: test_dynamics.c $(VMIDI_DIR)/voice_midi.c test_common.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_dynamics.c $(VMIDI_DIR)/voice_midi.c $(LDLIBS)

$(BUILD)/test_expression: test_expression.c $(VMIDI_DIR)/voice_midi.c $(VMIDI_DIR)/expression.c test_common.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_expression.c $(VMIDI_DIR)/voice_midi.c $(VMIDI_DIR)/expression.c $(LDLIBS)

$(BUILD)/test_noise_gate: test_noise_gate.c $(DSP_DIR)/noise_gate.c test_common.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_noise_gate.c $(DSP_DIR)/noise_gate.c $(LDLIBS)

clean:
	rm -rf $(BUILD)
