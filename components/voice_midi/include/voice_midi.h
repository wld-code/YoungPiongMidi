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

/**
 * @brief Map an envelope-followed amplitude to a raw MIDI controller
 *        value, 0..127 (the full CC range - unlike velocity, 0 is not
 *        reserved). Same clamping/curve behavior as yp_level_to_velocity(),
 *        just scaled to [0, 127] instead of [YP_MIDI_VELOCITY_MIN,
 *        YP_MIDI_VELOCITY_MAX]. Used for CC11 Expression (see
 *        yp_expression_process() below).
 */
int yp_level_to_cc_value(float level, yp_vel_curve_t curve);

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

/* -------------------------------------------------------------------- */
/*  Continuous CC11 Expression (project spec section 9, Milestone 7)     */
/* -------------------------------------------------------------------- */

/**
 * @brief Rate-limiting state for streaming CC11 while a note is held.
 *
 * Owned by the caller (one instance per active note, or reused/reset
 * across notes - see yp_expression_init()). Deliberately separate from
 * yp_note_sm_t: the note state machine decides *whether a note exists at
 * all*; this decides *whether this hop's continued dynamics are worth a
 * CC11 message*, which is a different, continuous concern the spec
 * explicitly calls out as distinct from note-onset velocity ("dynamics
 * must not only be measured at the note onset").
 */
typedef struct {
    int     last_sent_value;   /**< -1 = nothing sent yet since init/reset */
    int64_t last_sent_time_us;
} yp_expression_t;

/** Reset so the next yp_expression_process() call always sends (establishes
 *  a fresh baseline) - call once when a note turns on. */
void yp_expression_init(yp_expression_t *ex);

/**
 * @brief Decide whether this hop's level warrants a new CC11 message.
 *
 * Only meaningful while a note is active - callers should not invoke
 * this during SILENCE (there is nothing to attach continued dynamics
 * to).
 *
 * Throttling: per the project spec ("do not flood the MIDI connection;
 * only transmit when the value changes sufficiently or after a
 * configurable minimum interval"), a new value is sent only once BOTH
 * gates pass: the candidate value differs from the last *sent* value by
 * at least YP_CC11_MIN_DELTA, AND at least YP_CC11_MIN_INTERVAL_MS has
 * elapsed since the last send. Read literally, the spec's "or" could
 * also mean either gate alone should be sufficient - deliberately not
 * implemented that way here: an "or" of two independent conditions is
 * not actually a rate *cap* (a large, fast-changing delta would still
 * fire every hop), whereas requiring both is what actually bounds the
 * message rate to at most one per YP_CC11_MIN_INTERVAL_MS while still
 * filtering out sub-threshold noise - see docs/midi.md for the full
 * reasoning. The very first call after yp_expression_init() always
 * sends, regardless of both gates, to establish a starting value.
 *
 * @param ex       tracker state, updated in place
 * @param level    this hop's envelope level
 * @param now_us   monotonic timestamp (see yp_note_sm_process() re: clocks)
 * @param out_value set to the CC value (0..127) to send, only when this
 *                   function returns true
 * @return true if a CC11 message should be sent this hop
 */
bool yp_expression_process(yp_expression_t *ex, float level, int64_t now_us, int *out_value);

#ifdef __cplusplus
}
#endif
