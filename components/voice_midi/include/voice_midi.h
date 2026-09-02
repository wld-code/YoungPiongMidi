/**
 * @file voice_midi.h
 * @brief Frequency <-> MIDI note conversion utilities (project spec
 *        section 6, Milestone 4).
 *
 * Pure math, no ESP-IDF dependency, deliberately - so it can be (and is,
 * see test/test_midi_notes.c) unit tested on the host without hardware.
 * This file does NOT decide when a pitch estimate is stable/confident
 * enough to trust (that is voice_midi.c's future note-stabilization
 * state machine, Milestone 5, not yet implemented) - it only answers
 * "given this frequency, what note is that".
 *
 * Convention: MIDI note 69 = A4 = 440 Hz (equal temperament), MIDI note
 * 60 = C4 (middle C) = octave 4. octave = midi_note / 12 - 1.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** false if the input frequency was <= 0, non-finite, or otherwise
     *  not convertible - every other field is meaningless when false. */
    bool  valid;
    /** Nearest MIDI note, 0..127 (clamped at the ends of that range). */
    int   midi_note;
    /** Note name without octave, e.g. "A", "C#". Always null-terminated,
     *  at most 3 bytes used (2 chars + '\0'). */
    char  note_name[4];
    /** Octave number, e.g. 4 for middle C (MIDI note 60). */
    int   octave;
    /** Deviation from the exact equal-temperament pitch of midi_note, in
     *  cents. Normally in [-50, +50]; can be larger if frequency_hz maps
     *  outside [0, 127] and midi_note was clamped. */
    float cents;
} yp_note_info_t;

/**
 * @brief Exact (fractional) MIDI note number for a frequency.
 *
 * midi = 69 + 12 * log2(frequency_hz / 440)
 *
 * @return the fractional note number, or NAN if frequency_hz <= 0 or not
 *         finite.
 */
float yp_frequency_to_midi_float(float frequency_hz);

/**
 * @brief Nearest integer MIDI note for a frequency, clamped to [0, 127].
 * @return -1 if frequency_hz <= 0 or not finite.
 */
int yp_frequency_to_midi_note(float frequency_hz);

/** @brief Frequency, in Hz, of a MIDI note (equal temperament, A4=440Hz). */
float yp_midi_note_to_frequency(int midi_note);

/**
 * @brief Note name without octave: "C", "C#", "D", ... "B".
 * @return a pointer to a static string; "?" if midi_note is outside
 *         [0, 127].
 */
const char *yp_midi_note_name(int midi_note);

/** @brief Octave number for a MIDI note (MIDI 60 = C4). */
int yp_midi_note_octave(int midi_note);

/**
 * @brief Full breakdown of a frequency: nearest note, name, octave, and
 *        cents deviation from that note's exact equal-temperament pitch.
 */
yp_note_info_t yp_frequency_to_note_info(float frequency_hz);

#ifdef __cplusplus
}
#endif
