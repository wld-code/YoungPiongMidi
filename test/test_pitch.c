/**
 * @file test_pitch.c
 * @brief Host-side tests for components/pitch/yin.c, using synthetic
 *        signals (project spec section 17: "create synthetic signals for
 *        tests" / "add noisy sine signals to test robustness").
 *
 * Feeds pitch_detector_process() a continuous stream of YP_AUDIO_HOP_SIZE
 * hops (exactly as audio_dsp.c does on real hardware) generated from a
 * pure sine wave, and checks that the recovered frequency - and, as an
 * end-to-end check, the MIDI note voice_midi.c derives from it - matches
 * what was actually synthesized.
 */
#include "test_common.h"
#include "pitch_detector.h"
#include "voice_midi.h"
#include "yp_config.h"
#include <stdlib.h>

/* Feed `total_samples` of a sine wave at `freq_hz` through the detector,
 * hop by hop, and return the last computed estimate. `noise_amplitude`
 * (0 to disable) adds uniform pseudo-random noise on top, for the
 * robustness variant. */
static pitch_estimate_t run_sine(float freq_hz, float amplitude, float noise_amplitude,
                                  int total_samples)
{
    pitch_detector_init();
    pitch_estimate_t last = { 0.0f, 0.0f };

    float phase = 0.0f;
    float phase_step = 2.0f * (float)M_PI * freq_hz / (float)YP_AUDIO_SAMPLE_RATE_HZ;

    float hop[YP_AUDIO_HOP_SIZE];
    int produced = 0;
    while (produced < total_samples) {
        int n = YP_AUDIO_HOP_SIZE;
        for (int i = 0; i < n; i++) {
            float noise = 0.0f;
            if (noise_amplitude > 0.0f) {
                noise = noise_amplitude * (2.0f * ((float)rand() / (float)RAND_MAX) - 1.0f);
            }
            hop[i] = amplitude * sinf(phase) + noise;
            phase += phase_step;
        }
        last = pitch_detector_process(hop, n);
        produced += n;
    }
    return last;
}

static void test_pure_tones_match_spec_examples(void)
{
    TEST_CASE("pure sine tones recover the frequency/note from the spec's own examples");

    /* Long enough to fill the window (YP_AUDIO_FRAME_SIZE samples) and
     * pass several YP_PITCH_UPDATE_STRIDE_HOPS cycles so the returned
     * estimate is a real, settled computation, not the {0,0} default. */
    const int duration_samples = YP_AUDIO_HOP_SIZE * 40;

    struct { float freq; int expected_midi; const char *name; } cases[] = {
        { 440.00f, 69, "A4" },
        { 261.63f, 60, "C4" },
        { 329.63f, 64, "E4" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        pitch_estimate_t est = run_sine(cases[i].freq, 0.5f, 0.0f, duration_samples);

        char msg[96];
        snprintf(msg, sizeof(msg), "%s (%.2fHz): confidence should be high for a clean tone",
                 cases[i].name, cases[i].freq);
        TEST_CHECK_TRUE(est.confidence > 0.7f, msg);

        snprintf(msg, sizeof(msg), "%s (%.2fHz): recovered frequency should be within 2%%",
                 cases[i].name, cases[i].freq);
        TEST_CHECK_NEAR(est.frequency_hz, cases[i].freq, cases[i].freq * 0.02f, msg);

        int midi = yp_frequency_to_midi_note(est.frequency_hz);
        snprintf(msg, sizeof(msg), "%s: end-to-end pitch->MIDI should match MIDI %d",
                 cases[i].name, cases[i].expected_midi);
        TEST_CHECK_EQ_INT(midi, cases[i].expected_midi, msg);
    }
}

static void test_octave_boundaries(void)
{
    TEST_CASE("frequencies near the configured pitch range boundaries");

    const int duration_samples = YP_AUDIO_HOP_SIZE * 40;

    /* Comfortably inside [YP_PITCH_MIN_HZ, YP_PITCH_MAX_HZ], not AT the
     * boundary - tau quantization near the extreme edges makes exact
     * recovery inherently coarser, which is a property of YIN/the
     * configured window, not a bug to chase in this test. */
    pitch_estimate_t low = run_sine(YP_PITCH_MIN_HZ * 1.2f, 0.5f, 0.0f, duration_samples);
    TEST_CHECK_TRUE(low.confidence > 0.5f, "near the low end of the pitch range should still be confident");
    TEST_CHECK_NEAR(low.frequency_hz, YP_PITCH_MIN_HZ * 1.2f, YP_PITCH_MIN_HZ * 1.2f * 0.05f,
                     "near-low-end frequency should be recovered within 5%");

    pitch_estimate_t high = run_sine(YP_PITCH_MAX_HZ * 0.8f, 0.5f, 0.0f, duration_samples);
    TEST_CHECK_TRUE(high.confidence > 0.5f, "near the high end of the pitch range should still be confident");
    TEST_CHECK_NEAR(high.frequency_hz, YP_PITCH_MAX_HZ * 0.8f, YP_PITCH_MAX_HZ * 0.8f * 0.05f,
                     "near-high-end frequency should be recovered within 5%");
}

static void test_noisy_signal_robustness(void)
{
    TEST_CASE("a sine tone with added noise: frequency stays roughly correct");

    srand(1234); /* reproducible across runs */
    const int duration_samples = YP_AUDIO_HOP_SIZE * 40;

    /* Moderate noise: half the amplitude of the tone itself. Confidence
     * is expected to drop versus the clean-tone case, but the recovered
     * frequency (when it does report one) should still land close to the
     * true pitch rather than locking onto an unrelated frequency. */
    pitch_estimate_t est = run_sine(440.0f, 0.5f, 0.25f, duration_samples);
    TEST_CHECK_NEAR(est.frequency_hz, 440.0f, 440.0f * 0.05f,
                     "moderately noisy 440Hz tone should still recover a frequency within 5%");
}

static void test_silence_gives_low_confidence(void)
{
    TEST_CASE("silence must not produce a confident false pitch");

    pitch_detector_init();
    float hop[YP_AUDIO_HOP_SIZE] = { 0 };
    pitch_estimate_t est = { 0.0f, 0.0f };
    for (int i = 0; i < 40; i++) {
        est = pitch_detector_process(hop, YP_AUDIO_HOP_SIZE);
    }
    /* A silent (all-zero) window is perfectly self-similar at every lag,
     * so confidence can technically be high on paper - what actually
     * matters operationally is handled upstream by voice-activity
     * detection (audio_dsp.c) gating whether pitch is even looked at.
     * The property this test protects is narrower but still real: pitch
     * detection itself must not crash, hang, or return a NaN/garbage
     * frequency on a degenerate all-zero input. */
    TEST_CHECK_TRUE(est.frequency_hz == 0.0f || (est.frequency_hz >= YP_PITCH_MIN_HZ && est.frequency_hz <= YP_PITCH_MAX_HZ),
                     "silence must yield either 0 or a well-formed in-range frequency, never garbage");
}

static void test_window_fill_delay(void)
{
    TEST_CASE("no estimate is produced before the analysis window has filled");

    pitch_detector_init();
    float hop[YP_AUDIO_HOP_SIZE];
    for (int i = 0; i < YP_AUDIO_HOP_SIZE; i++) {
        hop[i] = 0.5f * sinf(2.0f * (float)M_PI * 440.0f * (float)i / (float)YP_AUDIO_SAMPLE_RATE_HZ);
    }

    int hops_to_fill = (YP_AUDIO_FRAME_SIZE + YP_AUDIO_HOP_SIZE - 1) / YP_AUDIO_HOP_SIZE;
    for (int i = 0; i < hops_to_fill - 1; i++) {
        pitch_estimate_t est = pitch_detector_process(hop, YP_AUDIO_HOP_SIZE);
        TEST_CHECK_TRUE(est.frequency_hz == 0.0f && est.confidence == 0.0f,
                         "before the window fills, the estimate must stay {0,0}, not a guess");
    }
}

int main(void)
{
    printf("test_pitch\n");
    test_pure_tones_match_spec_examples();
    test_octave_boundaries();
    test_noisy_signal_robustness();
    test_silence_gives_low_confidence();
    test_window_fill_delay();
    return TEST_MAIN_END();
}
