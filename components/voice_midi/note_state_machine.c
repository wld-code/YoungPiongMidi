/**
 * @file note_state_machine.c
 * @brief Note-stabilization state machine (project spec section 7).
 *
 * Purpose: a raw per-hop pitch estimate (from YIN, via audio_dsp) must
 * NOT directly drive MIDI Note On/Off - human voice pitch, and any
 * estimator's confidence, both fluctuate frame to frame. This file's job
 * is entirely about *when* that raw estimate is trustworthy and stable
 * enough to become a MIDI event, using exactly the mechanisms the spec
 * calls for: a confidence threshold (applied via frame_is_reliable(),
 * using YP_PITCH_CONFIDENCE_THRESHOLD), a minimum number of consecutive
 * stable frames before committing to a note (YP_NOTE_MIN_STABLE_FRAMES),
 * a minimum note duration before allowing a change
 * (YP_NOTE_MIN_DURATION_MS), pitch hysteresis around the active note
 * (YP_NOTE_CHANGE_TOLERANCE_ST), and a release debounce
 * (YP_NOTE_RELEASE_FRAMES). This is why a small pitch fluctuation does
 * not produce Note Off/On/Off/On spam.
 *
 * States, and how this implementation represents them: SILENCE and
 * NOTE_ACTIVE are the two states actually *persisted* in yp_note_sm_t
 * across calls. ATTACK, NOTE_CHANGE and RELEASE are the *transitions*
 * between them - each happens within a single yp_note_sm_process() call
 * and is fully described by that call's returned yp_note_event_t
 * (NOTE_ON, NOTE_CHANGE, NOTE_OFF respectively), so persisting them as a
 * separate stored state for a subsequent call to immediately transition
 * out of again would add a call round-trip with no behavioral
 * difference. yp_note_state_name() still names all five, and a caller
 * that wants to log "ATTACK"/"NOTE_CHANGE"/"RELEASE" can do so for the
 * one hop where the corresponding event fired (see main.c's diagnostic
 * log for exactly that).
 *
 * Note comparison uses the *fractional* MIDI note number
 * (yp_frequency_to_midi_float), not the rounded integer, for the
 * hysteresis check against the active note: rounding alone already
 * groups anything within +/-0.5 semitones into one note, so comparing
 * rounded integers could never express a tolerance narrower or wider
 * than that. Comparing the fractional distance against
 * YP_NOTE_CHANGE_TOLERANCE_ST (configured intentionally >0.5 semitones)
 * is what actually damps flicker for a pitch sitting right at a
 * rounding boundary.
 */
#include <math.h>
#include "voice_midi.h"
#include "yp_config.h"

const char *yp_note_state_name(yp_note_state_t state)
{
    switch (state) {
        case YP_NOTE_STATE_SILENCE:     return "SILENCE";
        case YP_NOTE_STATE_ATTACK:      return "ATTACK";
        case YP_NOTE_STATE_NOTE_ACTIVE: return "NOTE_ACTIVE";
        case YP_NOTE_STATE_NOTE_CHANGE: return "NOTE_CHANGE";
        case YP_NOTE_STATE_RELEASE:     return "RELEASE";
        default:                        return "?";
    }
}

void yp_note_sm_init(yp_note_sm_t *sm)
{
    sm->state = YP_NOTE_STATE_SILENCE;
    sm->active_note = -1;
    sm->candidate_note = -1;
    sm->candidate_frames = 0;
    sm->unstable_frames = 0;
    sm->active_since_us = 0;
}

/** A frame is "reliable" - trustworthy enough to even consider - only if
 *  voice is active, YIN's confidence clears the configured threshold,
 *  and the frequency converts to a valid MIDI note at all. */
static bool frame_is_reliable(const yp_voice_frame_t *frame, int *out_note)
{
    if (!frame->voice_active || frame->confidence < YP_PITCH_CONFIDENCE_THRESHOLD) {
        return false;
    }
    int note = yp_frequency_to_midi_note(frame->frequency_hz);
    if (note < 0) {
        return false;
    }
    *out_note = note;
    return true;
}

static int velocity_for(const yp_voice_frame_t *frame)
{
    return yp_level_to_velocity(frame->level, (yp_vel_curve_t)YP_DEFAULT_VELOCITY_CURVE);
}

yp_note_event_t yp_note_sm_process(yp_note_sm_t *sm, const yp_voice_frame_t *frame, int64_t now_us)
{
    yp_note_event_t event = { YP_NOTE_EVENT_NONE, -1, -1, 0 };

    int note = -1;
    bool reliable = frame_is_reliable(frame, &note);

    if (sm->state == YP_NOTE_STATE_SILENCE) {
        if (!reliable) {
            sm->candidate_note = -1;
            sm->candidate_frames = 0;
            return event;
        }

        if (note == sm->candidate_note) {
            sm->candidate_frames++;
        } else {
            sm->candidate_note = note;
            sm->candidate_frames = 1;
        }

        if (sm->candidate_frames < YP_NOTE_MIN_STABLE_FRAMES) {
            return event; /* still accumulating stability - no event yet */
        }

        /* ATTACK (this call only) -> NOTE_ACTIVE */
        event.type = YP_NOTE_EVENT_NOTE_ON;
        event.note_on_number = note;
        event.velocity = velocity_for(frame);

        sm->active_note = note;
        sm->active_since_us = now_us;
        sm->candidate_note = -1;
        sm->candidate_frames = 0;
        sm->unstable_frames = 0;
        sm->state = YP_NOTE_STATE_NOTE_ACTIVE;
        return event;
    }

    /* sm->state == YP_NOTE_STATE_NOTE_ACTIVE */

    if (!reliable) {
        sm->unstable_frames++;
        sm->candidate_note = -1;
        sm->candidate_frames = 0;

        if (sm->unstable_frames < YP_NOTE_RELEASE_FRAMES) {
            return event; /* transient dropout - not yet a real release */
        }

        /* RELEASE (this call only) -> SILENCE */
        event.type = YP_NOTE_EVENT_NOTE_OFF;
        event.note_off_number = sm->active_note;

        sm->active_note = -1;
        sm->unstable_frames = 0;
        sm->state = YP_NOTE_STATE_SILENCE;
        return event;
    }

    sm->unstable_frames = 0;

    float note_f = yp_frequency_to_midi_float(frame->frequency_hz);
    float distance_from_active = fabsf(note_f - (float)sm->active_note);

    if (distance_from_active <= YP_NOTE_CHANGE_TOLERANCE_ST) {
        /* Still the held note (within hysteresis) - not a change
         * candidate. This is what absorbs vibrato/jitter around the
         * held pitch without emitting anything. */
        sm->candidate_note = -1;
        sm->candidate_frames = 0;
        return event;
    }

    if (note == sm->candidate_note) {
        sm->candidate_frames++;
    } else {
        sm->candidate_note = note;
        sm->candidate_frames = 1;
    }

    int64_t held_ms = (now_us - sm->active_since_us) / 1000;
    if (sm->candidate_frames < YP_NOTE_MIN_STABLE_FRAMES || held_ms < YP_NOTE_MIN_DURATION_MS) {
        return event; /* candidate not yet stable/old enough to commit */
    }

    /* NOTE_CHANGE (this call only) -> NOTE_ACTIVE (on the new note) */
    event.type = YP_NOTE_EVENT_NOTE_CHANGE;
    event.note_off_number = sm->active_note;
    event.note_on_number = note;
    event.velocity = velocity_for(frame);

    sm->active_note = note;
    sm->active_since_us = now_us;
    sm->candidate_note = -1;
    sm->candidate_frames = 0;
    sm->state = YP_NOTE_STATE_NOTE_ACTIVE;
    return event;
}
