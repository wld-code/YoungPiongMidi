/**
 * @file test_rms.c
 * @brief Host-side tests for components/audio_dsp/rms.c.
 */
#include "test_common.h"
#include "audio_dsp_internal.h"

static void test_silence(void)
{
    TEST_CASE("all-zero buffer has zero RMS");
    float samples[64] = { 0 };
    TEST_CHECK_NEAR(rms_calculate(samples, 64), 0.0f, 1e-6f, "silence -> 0 RMS");
}

static void test_dc(void)
{
    TEST_CASE("constant (DC) buffer: RMS equals the constant's magnitude");
    float samples[100];
    for (int i = 0; i < 100; i++) samples[i] = 0.5f;
    TEST_CHECK_NEAR(rms_calculate(samples, 100), 0.5f, 1e-5f, "constant 0.5 -> RMS 0.5");

    for (int i = 0; i < 100; i++) samples[i] = -0.5f;
    TEST_CHECK_NEAR(rms_calculate(samples, 100), 0.5f, 1e-5f, "constant -0.5 -> RMS 0.5 (sign-independent)");
}

static void test_sine_rms(void)
{
    TEST_CASE("full-scale sine wave -> RMS = amplitude / sqrt(2)");
    /* An integer number of periods avoids edge effects from a partial
     * final cycle, so the analytic RMS = A/sqrt(2) result applies almost
     * exactly rather than only in the limit of many samples. */
    const int n = 8000;
    const float amplitude = 1.0f;
    const float cycles = 37.0f; /* not a divisor of n on purpose */
    float samples[8000];
    for (int i = 0; i < n; i++) {
        samples[i] = amplitude * sinf(2.0f * (float)M_PI * cycles * (float)i / (float)n);
    }
    float expected = amplitude / sqrtf(2.0f);
    TEST_CHECK_NEAR(rms_calculate(samples, n), expected, 0.01f, "sine RMS should be ~0.707x amplitude");
}

static void test_zero_count(void)
{
    TEST_CASE("count == 0 must not crash and should return 0");
    float dummy = 0.0f;
    TEST_CHECK_NEAR(rms_calculate(&dummy, 0), 0.0f, 1e-9f, "zero-length input -> 0 RMS");
}

int main(void)
{
    printf("test_rms\n");
    test_silence();
    test_dc();
    test_sine_rms();
    test_zero_count();
    return TEST_MAIN_END();
}
