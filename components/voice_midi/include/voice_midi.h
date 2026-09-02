/**
 * @file voice_midi.h
 * @brief Frequency <-> MIDI note conversion (project spec section 6,
 *        Milestone 4), dynamics -> velocity mapping (section 8,
 *        Milestone 6, only as much as Milestone 5 needs to emit a valid
 *        Note On), and the note-stabilization state machine (section 7,
 *        Milestone 5).
 *
 * Pure math/state, no ESP-IDF dependency, deliberately - so all of it can
 * be (and is, see test/) unit tested on the host without hardware. This
 * file does not know about MIDI transports, queues, or FreeRTOS - it
 * decides *when* a Note On/Off/Change should happen and what note/
 * velocity it should carry; components/midi turns that into an actual
 * midi_event_t and queues it (see docs/midi.md).
 *
 * Convention: MIDI note 69 = A4 = 440 Hz (equal temperament), MIDI note
 * 60 = C4 (middle C) = octave 4. octave = midi_note / 12 - 1.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "yp_config.h"

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

/* -------------------------------------------------------------------- */
/*  Dynamics -> MIDI velocity (project spec section 8)                   */
/* -------------------------------------------------------------------- */

typedef enum {
    YP_VEL_CURVE_LINEAR = YP_VELOCITY_CURVE_LINEAR,
    YP_VEL_CURVE_LOG    = YP_VELOCITY_CURVE_LOG,
} yp_vel_curve_t;

/**
 * @brief Map an envelope-followed amplitude ("level", ~0..1) to a MIDI
 *        velocity (YP_MIDI_VELOCITY_MIN..YP_MIDI_VELOCITY_MAX).
 *
 * `level` is clamped to [YP_DYNAMICS_NOISE_FLOOR, YP_DYNAMICS_MAX_RMS]
 * before mapping, so callers do not need to pre-clamp. YP_VEL_CURVE_LOG
 * maps in the dB (20*log10(level)) domain rather than linearly in level,
 * which tracks perceived loudness more closely than a raw linear
 * mapping - quiet-to-medium level changes get proportionally more of the
 * velocity range than they would linearly, matching how a singer's own
 * sense of "getting louder" works. YP_VEL_CURVE_LINEAR is provided for
 * comparison/tuning (see yp_config.h's YP_DEFAULT_VELOCITY_CURVE) and is
 * tested for the same monotonicity/bounds properties.
 */
int yp_level_to_velocity(float level, yp_vel_curve_t curve);

/* -------------------------------------------------------------------- */
/*  Note-stabilization state machine (project spec section 7)            */
/* -------------------------------------------------------------------- */

typedef enum {
    YP_NOTE_STATE_SILENCE = 0,
    YP_NOTE_STATE_ATTACK,
    YP_NOTE_STATE_NOTE_ACTIVE,
    YP_NOTE_STATE_NOTE_CHANGE,
    YP_NOTE_STATE_RELEASE,
} yp_note_state_t;

/** Human-readable name for a state, for logging/debugging. */
const char *yp_note_state_name(yp_note_state_t state);

/**
 * @brief One hop's worth of pitch/level input to the state machine.
 *
 * Deliberately not audio_dsp's voice_analysis_t: keeping this struct
 * local to voice_midi keeps the whole component free of any dependency
 * on audio_dsp/ESP-IDF (the caller - main.c's dsp_task - already has a
 * voice_analysis_t and just copies the four fields the state machine
 * actually needs into one of these).
 */
typedef struct {
    float frequency_hz;
    float confidence;
    float level;
    bool  voice_active;
} yp_voice_frame_t;

typedef enum {
    YP_NOTE_EVENT_NONE = 0,   /**< nothing to send this hop */
    YP_NOTE_EVENT_NOTE_ON,    /**< send Note On for note_on_number */
    YP_NOTE_EVENT_NOTE_OFF,   /**< send Note Off for note_off_number */
    /** send Note Off for note_off_number, then Note On for
     *  note_on_number - a stable pitch change while a note was already
     *  active. Kept as one event (not two separate hops' worth) so a
     *  transport can choose to order/coalesce them as it prefers. */
    YP_NOTE_EVENT_NOTE_CHANGE,
} yp_note_event_type_t;

typedef struct {
    yp_note_event_type_t type;
    int note_off_number;   /**< valid for NOTE_OFF and NOTE_CHANGE */
    int note_on_number;    /**< valid for NOTE_ON and NOTE_CHANGE */
    int velocity;          /**< valid for NOTE_ON and NOTE_CHANGE, 1..127 */
} yp_note_event_t;

typedef struct {
    yp_note_state_t state;
    int    active_note;          /* -1 if none */
    int    candidate_note;       /* -1 if no candidate being tracked */
    int    candidate_frames;     /* consecutive frames candidate has held */
    int    unstable_frames;      /* consecutive unreliable-pitch frames while a note is active */
    int64_t active_since_us;     /* when active_note last turned on/changed */
} yp_note_sm_t;

/** Reset a state machine to SILENCE with no active/candidate note. */
void yp_note_sm_init(yp_note_sm_t *sm);

/**
 * @brief Advance the state machine by one hop.
 *
 * @param sm      state, updated in place
 * @param frame   this hop's pitch/level input
 * @param now_us  monotonic timestamp (e.g. esp_timer_get_time()); only
 *                used for relative comparisons against YP_NOTE_MIN_DURATION_MS,
 *                so any consistent, monotonically-nondecreasing clock
 *                works, including a synthetic one in tests.
 * @return the MIDI event (if any) this hop should produce.
 */
yp_note_event_t yp_note_sm_process(yp_note_sm_t *sm, const yp_voice_frame_t *frame, int64_t now_us);

#ifdef __cplusplus
}
#endif
