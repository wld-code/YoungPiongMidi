/**
 * @file test_midi_notes.c
 * @brief Host-side tests for components/voice_midi (frequency <-> MIDI
 *        note conversion, project spec section 6 / Milestone 4).
 *
 * Build/run: `make -C test test_midi_notes && ./test/build/test_midi_notes`
 * or simply `make -C test` to build and run every test in this directory.
 */
#include "test_common.h"
#include "voice_midi.h"

static void test_reference_frequencies(void)
{
    TEST_CASE("reference frequencies from the project spec");

    /* 440 Hz -> A4 -> MIDI 69 -> 0 cents (spec section 6/20 example) */
    yp_note_info_t a4 = yp_frequency_to_note_info(440.0f);
    TEST_CHECK_TRUE(a4.valid, "440Hz should be a valid note");
    TEST_CHECK_EQ_INT(a4.midi_note, 69, "440Hz -> MIDI 69");
    TEST_CHECK_STR_EQ(a4.note_name, "A", "440Hz -> note name A");
    TEST_CHECK_EQ_INT(a4.octave, 4, "440Hz -> octave 4");
    TEST_CHECK_NEAR(a4.cents, 0.0f, 1.0f, "440Hz -> ~0 cents");

    /* 261.63 Hz -> C4 -> MIDI 60 (spec section 6 example) */
    yp_note_info_t c4 = yp_frequency_to_note_info(261.63f);
    TEST_CHECK_TRUE(c4.valid, "261.63Hz should be a valid note");
    TEST_CHECK_EQ_INT(c4.midi_note, 60, "261.63Hz -> MIDI 60");
    TEST_CHECK_STR_EQ(c4.note_name, "C", "261.63Hz -> note name C");
    TEST_CHECK_EQ_INT(c4.octave, 4, "261.63Hz -> octave 4");

    /* 329.63 Hz -> E4 -> MIDI 64 (spec section 17 example) */
    yp_note_info_t e4 = yp_frequency_to_note_info(329.63f);
    TEST_CHECK_TRUE(e4.valid, "329.63Hz should be a valid note");
    TEST_CHECK_EQ_INT(e4.midi_note, 64, "329.63Hz -> MIDI 64");
    TEST_CHECK_STR_EQ(e4.note_name, "E", "329.63Hz -> note name E");
}

static void test_cents_deviation(void)
{
    TEST_CASE("cents deviation for slightly-off-pitch frequencies");

    /* A4 sharp by ~20 cents: 440 * 2^(20/1200) = ~445.09 Hz */
    float sharp_freq = 440.0f * powf(2.0f, 20.0f / 1200.0f);
    yp_note_info_t sharp = yp_frequency_to_note_info(sharp_freq);
    TEST_CHECK_EQ_INT(sharp.midi_note, 69, "slightly sharp A4 still rounds to MIDI 69");
    TEST_CHECK_NEAR(sharp.cents, 20.0f, 0.5f, "should measure ~+20 cents sharp");

    /* A4 flat by ~20 cents */
    float flat_freq = 440.0f * powf(2.0f, -20.0f / 1200.0f);
    yp_note_info_t flat = yp_frequency_to_note_info(flat_freq);
    TEST_CHECK_EQ_INT(flat.midi_note, 69, "slightly flat A4 still rounds to MIDI 69");
    TEST_CHECK_NEAR(flat.cents, -20.0f, 0.5f, "should measure ~-20 cents flat");

    /* Just below vs. just above the A4/A#4 rounding boundary (50 cents
     * above A4) must round to the correct side, not the same note twice. */
    float just_below = 440.0f * powf(2.0f, 49.0f / 1200.0f);  /* A4 + 49 cents */
    float just_above = 440.0f * powf(2.0f, 51.0f / 1200.0f);  /* A4 + 51 cents */
    TEST_CHECK_EQ_INT(yp_frequency_to_midi_note(just_below), 69, "A4+49c should still round down to MIDI 69 (A4)");
    TEST_CHECK_EQ_INT(yp_frequency_to_midi_note(just_above), 70, "A4+51c should round up to MIDI 70 (A#4)");
}

static void test_full_chromatic_octave(void)
{
    TEST_CASE("note names across one full chromatic octave (MIDI 60-71)");

    static const char *const expected[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
    };
    for (int note = 60; note <= 71; note++) {
        TEST_CHECK_STR_EQ(yp_midi_note_name(note), expected[note - 60], "note name mismatch");
        TEST_CHECK_EQ_INT(yp_midi_note_octave(note), 4, "MIDI 60-71 should all be octave 4");
    }
}

static void test_round_trip(void)
{
    TEST_CASE("frequency -> MIDI -> frequency round-trip for every note");

    for (int note = 0; note <= 127; note++) {
        float freq = yp_midi_note_to_frequency(note);
        int back = yp_frequency_to_midi_note(freq);
        TEST_CHECK_EQ_INT(back, note, "round-trip should return the same MIDI note");
    }
}

static void test_clamping(void)
{
    TEST_CASE("out-of-range frequencies clamp to [0, 127] rather than wrapping/crashing");

    TEST_CHECK_EQ_INT(yp_frequency_to_midi_note(1.0f), 0, "near-DC frequency clamps to MIDI 0");
    TEST_CHECK_EQ_INT(yp_frequency_to_midi_note(20000.0f), 127, "ultrasonic frequency clamps to MIDI 127");

    yp_note_info_t low = yp_frequency_to_note_info(1.0f);
    TEST_CHECK_TRUE(low.valid, "clamped-low frequency is still a valid (if extreme) note");
    TEST_CHECK_EQ_INT(low.midi_note, 0, "clamped-low midi_note is 0");
}

static void test_invalid_input(void)
{
    TEST_CASE("invalid input (<=0, NaN, inf) is rejected, not silently miscomputed");

    TEST_CHECK_EQ_INT(yp_frequency_to_midi_note(0.0f), -1, "0Hz is invalid");
    TEST_CHECK_EQ_INT(yp_frequency_to_midi_note(-440.0f), -1, "negative frequency is invalid");
    TEST_CHECK_EQ_INT(yp_frequency_to_midi_note(NAN), -1, "NaN frequency is invalid");
    TEST_CHECK_EQ_INT(yp_frequency_to_midi_note(INFINITY), -1, "infinite frequency is invalid");

    yp_note_info_t invalid = yp_frequency_to_note_info(0.0f);
    TEST_CHECK_TRUE(!invalid.valid, "0Hz should produce an invalid note_info_t");

    TEST_CHECK_STR_EQ(yp_midi_note_name(-1), "?", "out-of-range note index returns \"?\"");
    TEST_CHECK_STR_EQ(yp_midi_note_name(128), "?", "out-of-range note index returns \"?\"");
}

int main(void)
{
    printf("test_midi_notes\n");
    test_reference_frequencies();
    test_cents_deviation();
    test_full_chromatic_octave();
    test_round_trip();
    test_clamping();
    test_invalid_input();
    return TEST_MAIN_END();
}
