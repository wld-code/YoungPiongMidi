/**
 * @file test_noise_gate.c
 * @brief Host-side tests for components/audio_dsp/noise_gate.c - the
 *        adaptive VAD threshold added after real hardware showed a
 *        fixed threshold let touching the microphone (or plain ambient
 *        noise) generate MIDI events with no one singing.
 */
#include "test_common.h"
#include "audio_dsp_internal.h"
#include "yp_config.h"

#define FRAME_MS 8.0f

static void test_floor_tracks_down_toward_quiet(void)
{
    TEST_CASE("floor adapts down toward a genuinely quiet room");
    noise_gate_t gate;
    noise_gate_init(&gate, FRAME_MS);

    /* Start well above the minimum floor by forcing an up-adapt first
     * (closed gate, moderately loud): */
    for (int i = 0; i < 2000; i++) {
        noise_gate_process(&gate, 0.05f, false);
    }
    float raised = gate.floor;
    TEST_CHECK_TRUE(raised > YP_NOISE_GATE_MIN_FLOOR * 2.0f, "test setup: floor should have risen");

    /* Now feed genuine quiet for a while - the floor should come back down. */
    for (int i = 0; i < 500; i++) {
        noise_gate_process(&gate, 0.001f, false);
    }
    TEST_CHECK_TRUE(gate.floor < raised, "floor should have decreased toward the quiet input");
}

static void test_loud_open_gate_does_not_raise_floor(void)
{
    TEST_CASE("a loud, sustained, OPEN-gate signal (real voice) never raises the floor under itself");
    noise_gate_t gate;
    noise_gate_init(&gate, FRAME_MS);
    float initial = gate.floor;

    /* Simulate two seconds of a loud voice with the gate reported open
     * (gate_was_open=true) every single frame. */
    for (int i = 0; i < 250; i++) {
        noise_gate_process(&gate, 0.3f, true);
    }
    TEST_CHECK_NEAR(gate.floor, initial, 1e-9f, "floor must not move at all while the gate is open");
}

static void test_closed_gate_noise_eventually_raises_floor(void)
{
    TEST_CASE("sustained noise the gate never trusted (closed) does slowly raise the floor - self-correcting");
    noise_gate_t gate;
    noise_gate_init(&gate, FRAME_MS);

    float initial_threshold = gate.floor * YP_NOISE_GATE_MARGIN_RATIO;
    TEST_CHECK_TRUE(0.03f > initial_threshold,
                     "test setup: 0.03 should initially clear the fresh, low threshold (i.e. it would mistakenly open the gate at first)");

    /* Feed a constant 0.03 "touch noise" level for a long time, always
     * reporting the gate as closed (as if debounce/confidence downstream
     * kept rejecting it, or simulating that we want the floor itself to
     * eventually make 0.03 stop looking like signal). */
    float threshold = initial_threshold;
    for (int i = 0; i < 5000; i++) {
        threshold = noise_gate_process(&gate, 0.03f, false);
    }
    TEST_CHECK_TRUE(threshold > 0.03f * 0.9f,
                     "after sustained exposure, the threshold should have risen to approach the noise level");
}

static void test_threshold_never_below_min_floor_margin(void)
{
    TEST_CASE("returned threshold is always at least YP_NOISE_GATE_MIN_FLOOR * margin");
    noise_gate_t gate;
    noise_gate_init(&gate, FRAME_MS);

    float threshold = noise_gate_process(&gate, 0.0f, false);
    TEST_CHECK_NEAR(threshold, YP_NOISE_GATE_MIN_FLOOR * YP_NOISE_GATE_MARGIN_RATIO, 1e-6f,
                     "with zero input, threshold should sit at the clamped minimum");

    /* Feed a long run of exact-zero input - floor must never go negative
     * or below the clamp, however long we run it. */
    for (int i = 0; i < 10000; i++) {
        threshold = noise_gate_process(&gate, 0.0f, false);
    }
    TEST_CHECK_TRUE(threshold >= YP_NOISE_GATE_MIN_FLOOR * YP_NOISE_GATE_MARGIN_RATIO - 1e-6f,
                     "threshold must never collapse below the clamped minimum");
}

static void test_quiet_room_then_real_voice_opens_promptly(void)
{
    TEST_CASE("end-to-end shape: quiet room settles low, then a real voice still clears the gate");
    noise_gate_t gate;
    noise_gate_init(&gate, FRAME_MS);

    /* Let the floor settle on a quiet room (~0.003 RMS). */
    float threshold = 0.0f;
    for (int i = 0; i < 500; i++) {
        threshold = noise_gate_process(&gate, 0.003f, false);
    }
    TEST_CHECK_TRUE(threshold < 0.05f, "a genuinely quiet room should settle to a low threshold");

    /* A real voice at 0.15 RMS should clear that threshold immediately. */
    TEST_CHECK_TRUE(0.15f >= threshold, "normal voice level should clear the settled threshold");
}

int main(void)
{
    printf("test_noise_gate\n");
    test_floor_tracks_down_toward_quiet();
    test_loud_open_gate_does_not_raise_floor();
    test_closed_gate_noise_eventually_raises_floor();
    test_threshold_never_below_min_floor_margin();
    test_quiet_room_then_real_voice_opens_promptly();
    return TEST_MAIN_END();
}
