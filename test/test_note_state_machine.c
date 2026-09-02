/**
 * @file test_note_state_machine.c
 * @brief Host-side tests for components/voice_midi/note_state_machine.c
 *        (project spec section 7, Milestone 5).
 *
 * The single most important property this state machine exists to
 * provide, per the spec: "A small pitch fluctuation must not generate:
 * Note Off / Note On / Note Off / Note On continuously." That is not a
 * side effect to hope for here - it is asserted on directly (see
 * test_jitter_does_not_flicker).
 */
#include "test_common.h"
#include "voice_midi.h"

#define HOP_US  8000 /* matches YP_AUDIO_HOP_SIZE/YP_AUDIO_SAMPLE_RATE_HZ = 8ms */

static yp_voice_frame_t reliable(float freq_hz, float confidence, float level)
{
    yp_voice_frame_t f = { .frequency_hz = freq_hz, .confidence = confidence, .level = level, .voice_active = true };
    return f;
}

static yp_voice_frame_t silence(void)
{
    yp_voice_frame_t f = { .frequency_hz = 0.0f, .confidence = 0.0f, .level = 0.0f, .voice_active = false };
    return f;
}

/* Drive `count` hops of `frame` through sm, advancing the clock by
 * HOP_US each time, and return the last event produced (if the caller
 * only cares whether/when *something* eventually fires, use
 * drive_until_event() instead). */
static yp_note_event_t drive(yp_note_sm_t *sm, yp_voice_frame_t frame, int count, int64_t *clock_us)
{
    yp_note_event_t ev = { YP_NOTE_EVENT_NONE, -1, -1, 0 };
    for (int i = 0; i < count; i++) {
        ev = yp_note_sm_process(sm, &frame, *clock_us);
        *clock_us += HOP_US;
    }
    return ev;
}

static void test_no_event_below_min_stable_frames(void)
{
    TEST_CASE("a reliable pitch must hold for YP_NOTE_MIN_STABLE_FRAMES before Note On fires");
    yp_note_sm_t sm;
    yp_note_sm_init(&sm);
    int64_t clock = 0;

    yp_voice_frame_t f = reliable(440.0f, 0.9f, 0.1f);
    for (int i = 0; i < YP_NOTE_MIN_STABLE_FRAMES - 1; i++) {
        yp_note_event_t ev = yp_note_sm_process(&sm, &f, clock);
        clock += HOP_US;
        TEST_CHECK_TRUE(ev.type == YP_NOTE_EVENT_NONE, "no event before the stability threshold is reached");
        TEST_CHECK_TRUE(sm.state == YP_NOTE_STATE_SILENCE, "state should still be SILENCE");
    }
}

static void test_note_on_after_stable_attack(void)
{
    TEST_CASE("Note On fires exactly on the frame stability is reached, with a sane velocity");
    yp_note_sm_t sm;
    yp_note_sm_init(&sm);
    int64_t clock = 0;

    yp_voice_frame_t f = reliable(440.0f, 0.9f, 0.3f);
    yp_note_event_t ev = drive(&sm, f, YP_NOTE_MIN_STABLE_FRAMES, &clock);

    TEST_CHECK_TRUE(ev.type == YP_NOTE_EVENT_NOTE_ON, "should fire Note On once stable");
    TEST_CHECK_EQ_INT(ev.note_on_number, 69, "440Hz -> MIDI 69 (A4)");
    TEST_CHECK_TRUE(ev.velocity >= 1 && ev.velocity <= 127, "velocity must be in valid MIDI range");
    TEST_CHECK_TRUE(sm.state == YP_NOTE_STATE_NOTE_ACTIVE, "state should now be NOTE_ACTIVE");
    TEST_CHECK_EQ_INT(sm.active_note, 69, "active_note should be recorded");
}

static void test_jitter_does_not_flicker(void)
{
    TEST_CASE("small pitch jitter around a held note must NOT generate repeated Note Off/On (spec's own wording)");
    yp_note_sm_t sm;
    yp_note_sm_init(&sm);
    int64_t clock = 0;

    /* Establish A4. */
    drive(&sm, reliable(440.0f, 0.9f, 0.3f), YP_NOTE_MIN_STABLE_FRAMES, &clock);
    TEST_CHECK_TRUE(sm.state == YP_NOTE_STATE_NOTE_ACTIVE, "A4 should now be active");

    /* Jitter by a few cents either side of A4 (well inside
     * YP_NOTE_CHANGE_TOLERANCE_ST) for many frames - simulating natural
     * micro-variation in a held vocal pitch, not a deliberate note
     * change. */
    int events_seen = 0;
    for (int i = 0; i < 60; i++) {
        float wobble_cents = (i % 2 == 0) ? 15.0f : -15.0f;
        float freq = 440.0f * powf(2.0f, wobble_cents / 1200.0f);
        yp_note_event_t ev = yp_note_sm_process(&sm, &(yp_voice_frame_t){
            .frequency_hz = freq, .confidence = 0.9f, .level = 0.3f, .voice_active = true }, clock);
        clock += HOP_US;
        if (ev.type != YP_NOTE_EVENT_NONE) {
            events_seen++;
        }
    }

    TEST_CHECK_EQ_INT(events_seen, 0, "jitter within tolerance must produce zero additional events");
    TEST_CHECK_EQ_INT(sm.active_note, 69, "should still be holding A4, not have changed or released");
    TEST_CHECK_TRUE(sm.state == YP_NOTE_STATE_NOTE_ACTIVE, "should still be NOTE_ACTIVE");
}

static void test_genuine_note_change(void)
{
    TEST_CASE("a sustained, stable pitch change (beyond tolerance, held long enough) produces NOTE_CHANGE");
    yp_note_sm_t sm;
    yp_note_sm_init(&sm);
    int64_t clock = 0;

    drive(&sm, reliable(440.0f, 0.9f, 0.3f), YP_NOTE_MIN_STABLE_FRAMES, &clock);
    TEST_CHECK_EQ_INT(sm.active_note, 69, "A4 established");

    /* Advance the clock past YP_NOTE_MIN_DURATION_MS before attempting a
     * change - the state machine must not honor a change request before
     * the current note has been held that long. */
    clock += (int64_t)(YP_NOTE_MIN_DURATION_MS + 10) * 1000;

    /* Move to C5 (523.25 Hz, MIDI 72) - a full 3 semitones away, well
     * beyond hysteresis. */
    yp_note_event_t ev = drive(&sm, reliable(523.25f, 0.9f, 0.3f), YP_NOTE_MIN_STABLE_FRAMES, &clock);

    TEST_CHECK_TRUE(ev.type == YP_NOTE_EVENT_NOTE_CHANGE, "should report a NOTE_CHANGE event");
    TEST_CHECK_EQ_INT(ev.note_off_number, 69, "note_off_number should be the old note (A4)");
    TEST_CHECK_EQ_INT(ev.note_on_number, 72, "note_on_number should be the new note (C5)");
    TEST_CHECK_EQ_INT(sm.active_note, 72, "active_note should now be C5");
}

static void test_change_blocked_before_min_duration(void)
{
    TEST_CASE("a note change is not honored before YP_NOTE_MIN_DURATION_MS has elapsed on the current note");
    yp_note_sm_t sm;
    yp_note_sm_init(&sm);
    int64_t clock = 0;

    drive(&sm, reliable(440.0f, 0.9f, 0.3f), YP_NOTE_MIN_STABLE_FRAMES, &clock);
    /* Do NOT advance the clock past YP_NOTE_MIN_DURATION_MS this time. */

    yp_note_event_t ev = drive(&sm, reliable(523.25f, 0.9f, 0.3f), YP_NOTE_MIN_STABLE_FRAMES, &clock);
    TEST_CHECK_TRUE(ev.type == YP_NOTE_EVENT_NONE, "change should be withheld: not enough time has passed");
    TEST_CHECK_EQ_INT(sm.active_note, 69, "should still be holding the original note (A4)");
}

static void test_release_after_sustained_silence(void)
{
    TEST_CASE("Note Off fires after YP_NOTE_RELEASE_FRAMES of unreliable pitch, not sooner");
    yp_note_sm_t sm;
    yp_note_sm_init(&sm);
    int64_t clock = 0;

    drive(&sm, reliable(440.0f, 0.9f, 0.3f), YP_NOTE_MIN_STABLE_FRAMES, &clock);

    /* One dropout frame short of the release threshold: must NOT release yet. */
    yp_voice_frame_t sil = silence();
    for (int i = 0; i < YP_NOTE_RELEASE_FRAMES - 1; i++) {
        yp_note_event_t ev = yp_note_sm_process(&sm, &sil, clock);
        clock += HOP_US;
        TEST_CHECK_TRUE(ev.type == YP_NOTE_EVENT_NONE, "must not release before YP_NOTE_RELEASE_FRAMES");
        TEST_CHECK_TRUE(sm.state == YP_NOTE_STATE_NOTE_ACTIVE, "should still be NOTE_ACTIVE mid-dropout");
    }

    /* One more dropout frame crosses the threshold. */
    yp_note_event_t ev = yp_note_sm_process(&sm, &sil, clock);
    TEST_CHECK_TRUE(ev.type == YP_NOTE_EVENT_NOTE_OFF, "should release exactly on the threshold-crossing frame");
    TEST_CHECK_EQ_INT(ev.note_off_number, 69, "should release the note that was active (A4)");
    TEST_CHECK_TRUE(sm.state == YP_NOTE_STATE_SILENCE, "state should return to SILENCE");
    TEST_CHECK_EQ_INT(sm.active_note, -1, "active_note should be cleared");
}

static void test_brief_dropout_does_not_release(void)
{
    TEST_CASE("a brief dropout shorter than YP_NOTE_RELEASE_FRAMES, followed by recovery, never releases");
    yp_note_sm_t sm;
    yp_note_sm_init(&sm);
    int64_t clock = 0;

    drive(&sm, reliable(440.0f, 0.9f, 0.3f), YP_NOTE_MIN_STABLE_FRAMES, &clock);

    yp_voice_frame_t sil = silence();
    for (int i = 0; i < YP_NOTE_RELEASE_FRAMES - 1; i++) {
        yp_note_sm_process(&sm, &sil, clock);
        clock += HOP_US;
    }
    /* Recover before crossing the release threshold. */
    yp_voice_frame_t recovered = reliable(440.0f, 0.9f, 0.3f);
    yp_note_event_t ev = yp_note_sm_process(&sm, &recovered, clock);
    TEST_CHECK_TRUE(ev.type == YP_NOTE_EVENT_NONE, "recovering just in time must not produce a spurious event");
    TEST_CHECK_TRUE(sm.state == YP_NOTE_STATE_NOTE_ACTIVE, "should still be holding the original note");
    TEST_CHECK_EQ_INT(sm.active_note, 69, "active_note unchanged across the brief dropout");
}

static void test_low_confidence_never_triggers(void)
{
    TEST_CASE("a pitch below YP_PITCH_CONFIDENCE_THRESHOLD never triggers Note On, no matter how long it's held");
    yp_note_sm_t sm;
    yp_note_sm_init(&sm);
    int64_t clock = 0;

    yp_voice_frame_t f = reliable(440.0f, YP_PITCH_CONFIDENCE_THRESHOLD - 0.05f, 0.3f);
    for (int i = 0; i < 200; i++) {
        yp_note_event_t ev = yp_note_sm_process(&sm, &f, clock);
        clock += HOP_US;
        TEST_CHECK_TRUE(ev.type == YP_NOTE_EVENT_NONE, "low-confidence input must never produce an event");
    }
    TEST_CHECK_TRUE(sm.state == YP_NOTE_STATE_SILENCE, "should remain in SILENCE indefinitely");
}

static void test_velocity_mapping_bounds_and_monotonicity(void)
{
    TEST_CASE("yp_level_to_velocity: bounds, clamping and monotonicity for both curves");

    yp_vel_curve_t curves[2] = { YP_VEL_CURVE_LINEAR, YP_VEL_CURVE_LOG };
    for (int c = 0; c < 2; c++) {
        int at_floor = yp_level_to_velocity(0.0f, curves[c]);
        int at_or_below_floor = yp_level_to_velocity(YP_DYNAMICS_NOISE_FLOOR, curves[c]);
        int at_max = yp_level_to_velocity(YP_DYNAMICS_MAX_RMS, curves[c]);
        int above_max = yp_level_to_velocity(YP_DYNAMICS_MAX_RMS * 10.0f, curves[c]);

        TEST_CHECK_EQ_INT(at_floor, YP_MIDI_VELOCITY_MIN, "at/below noise floor -> minimum velocity");
        TEST_CHECK_EQ_INT(at_or_below_floor, YP_MIDI_VELOCITY_MIN, "exactly at noise floor -> minimum velocity");
        TEST_CHECK_EQ_INT(at_max, YP_MIDI_VELOCITY_MAX, "at configured max RMS -> maximum velocity");
        TEST_CHECK_EQ_INT(above_max, YP_MIDI_VELOCITY_MAX, "above max RMS clamps to maximum velocity, does not overflow");

        int prev = YP_MIDI_VELOCITY_MIN;
        bool monotonic = true;
        for (int i = 1; i <= 20; i++) {
            float level = YP_DYNAMICS_NOISE_FLOOR
                          + (YP_DYNAMICS_MAX_RMS - YP_DYNAMICS_NOISE_FLOOR) * ((float)i / 20.0f);
            int v = yp_level_to_velocity(level, curves[c]);
            if (v < prev) monotonic = false;
            prev = v;
        }
        TEST_CHECK_TRUE(monotonic, "velocity must never decrease as level increases");
    }
}

int main(void)
{
    printf("test_note_state_machine\n");
    test_no_event_below_min_stable_frames();
    test_note_on_after_stable_attack();
    test_jitter_does_not_flicker();
    test_genuine_note_change();
    test_change_blocked_before_min_duration();
    test_release_after_sustained_silence();
    test_brief_dropout_does_not_release();
    test_low_confidence_never_triggers();
    test_velocity_mapping_bounds_and_monotonicity();
    return TEST_MAIN_END();
}
