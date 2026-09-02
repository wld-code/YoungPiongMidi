#include <math.h>
#include <string.h>
#include "voice_midi.h"

static const char *const k_note_names[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
};

float yp_frequency_to_midi_float(float frequency_hz)
{
    if (!(frequency_hz > 0.0f) || !isfinite(frequency_hz)) {
        return NAN;
    }
    return 69.0f + 12.0f * log2f(frequency_hz / 440.0f);
}

int yp_frequency_to_midi_note(float frequency_hz)
{
    float note_f = yp_frequency_to_midi_float(frequency_hz);
    if (isnan(note_f)) {
        return -1;
    }
    int note = (int)lroundf(note_f);
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    return note;
}

float yp_midi_note_to_frequency(int midi_note)
{
    return 440.0f * powf(2.0f, (float)(midi_note - 69) / 12.0f);
}

const char *yp_midi_note_name(int midi_note)
{
    if (midi_note < 0 || midi_note > 127) {
        return "?";
    }
    return k_note_names[midi_note % 12];
}

int yp_midi_note_octave(int midi_note)
{
    /* Integer division on a possibly-negative-before-clamp value is not
     * a concern here: callers are expected to pass already-clamped
     * [0,127] notes (as yp_frequency_to_midi_note always returns), for
     * which this is a plain floor division. */
    return midi_note / 12 - 1;
}

yp_note_info_t yp_frequency_to_note_info(float frequency_hz)
{
    yp_note_info_t info = { 0 };

    int midi_note = yp_frequency_to_midi_note(frequency_hz);
    if (midi_note < 0) {
        info.valid = false;
        return info;
    }

    info.valid = true;
    info.midi_note = midi_note;
    info.octave = yp_midi_note_octave(midi_note);
    strncpy(info.note_name, yp_midi_note_name(midi_note), sizeof(info.note_name) - 1);
    info.note_name[sizeof(info.note_name) - 1] = '\0';

    float exact_freq = yp_midi_note_to_frequency(midi_note);
    info.cents = 1200.0f * log2f(frequency_hz / exact_freq);

    return info;
}

int yp_level_to_velocity(float level, yp_vel_curve_t curve)
{
    float clamped = level;
    if (clamped < YP_DYNAMICS_NOISE_FLOOR) clamped = YP_DYNAMICS_NOISE_FLOOR;
    if (clamped > YP_DYNAMICS_MAX_RMS) clamped = YP_DYNAMICS_MAX_RMS;

    float normalized; /* 0..1 */
    if (curve == YP_VEL_CURVE_LOG) {
        /* Linear in dB, not in level: quiet-to-medium changes get
         * proportionally more of the velocity range, tracking perceived
         * loudness better than a raw linear mapping (see voice_midi.h). */
        float db_min = 20.0f * log10f(YP_DYNAMICS_NOISE_FLOOR);
        float db_max = 20.0f * log10f(YP_DYNAMICS_MAX_RMS);
        float db = 20.0f * log10f(clamped);
        normalized = (db - db_min) / (db_max - db_min);
    } else {
        normalized = (clamped - YP_DYNAMICS_NOISE_FLOOR) / (YP_DYNAMICS_MAX_RMS - YP_DYNAMICS_NOISE_FLOOR);
    }
    if (normalized < 0.0f) normalized = 0.0f;
    if (normalized > 1.0f) normalized = 1.0f;

    int velocity = YP_MIDI_VELOCITY_MIN
                    + (int)lroundf(normalized * (float)(YP_MIDI_VELOCITY_MAX - YP_MIDI_VELOCITY_MIN));
    if (velocity < YP_MIDI_VELOCITY_MIN) velocity = YP_MIDI_VELOCITY_MIN;
    if (velocity > YP_MIDI_VELOCITY_MAX) velocity = YP_MIDI_VELOCITY_MAX;
    return velocity;
}
