/**
 * @file test_envelope.c
 * @brief Host-side tests for components/audio_dsp/envelope.c (the
 *        attack/release envelope follower).
 */
#include "test_common.h"
#include "audio_dsp_internal.h"

static void test_starts_at_zero(void)
{
    TEST_CASE("a fresh envelope starts at 0");
    envelope_t env;
    envelope_init(&env, 8.0f, 120.0f, 8.0f);
    TEST_CHECK_NEAR(env.state, 0.0f, 1e-9f, "initial state should be 0");
}

static void test_attack_faster_than_release(void)
{
    TEST_CASE("attack (rising input) settles faster than release (falling input) for attack_ms < release_ms");
    envelope_t env;
    envelope_init(&env, 8.0f, 120.0f, 8.0f); /* 1 frame period == attack time constant */

    /* Step up to 1.0 and hold: after enough frames the envelope should
     * have risen close to 1.0. */
    float last = 0.0f;
    for (int i = 0; i < 40; i++) {
        last = envelope_process(&env, 1.0f);
    }
    TEST_CHECK_TRUE(last > 0.95f, "envelope should closely track a sustained step after 40 frames");

    /* Step down to 0.0 and hold for the SAME number of frames: because
     * release_ms (120) >> attack_ms (8), it should NOT have caught up to
     * 0 the way the attack caught up to 1.0 - it should still be
     * clearly above 0. */
    for (int i = 0; i < 5; i++) {
        last = envelope_process(&env, 0.0f);
    }
    TEST_CHECK_TRUE(last > 0.3f, "slow release should still be well above 0 after only 5 frames");
}

static void test_monotonic_rise_and_fall(void)
{
    TEST_CASE("envelope moves monotonically toward a held step input, never overshoots");
    envelope_t env;
    envelope_init(&env, 10.0f, 50.0f, 5.0f);

    float prev = 0.0f;
    bool monotonic = true;
    for (int i = 0; i < 30; i++) {
        float v = envelope_process(&env, 1.0f);
        if (v < prev - 1e-6f || v > 1.0f + 1e-6f) {
            monotonic = false;
        }
        prev = v;
    }
    TEST_CHECK_TRUE(monotonic, "rising toward a held step should never decrease or exceed the target");

    monotonic = true;
    for (int i = 0; i < 60; i++) {
        float v = envelope_process(&env, 0.0f);
        if (v > prev + 1e-6f || v < 0.0f - 1e-6f) {
            monotonic = false;
        }
        prev = v;
    }
    TEST_CHECK_TRUE(monotonic, "falling toward a held step of 0 should never increase or go negative");
}

static void test_zero_time_constant_is_instantaneous(void)
{
    TEST_CASE("attack_ms/release_ms == 0 means the envelope tracks its input exactly");
    envelope_t env;
    envelope_init(&env, 0.0f, 0.0f, 8.0f);

    TEST_CHECK_NEAR(envelope_process(&env, 0.7f), 0.7f, 1e-6f, "zero attack time -> instant jump to input");
    TEST_CHECK_NEAR(envelope_process(&env, 0.1f), 0.1f, 1e-6f, "zero release time -> instant jump to input");
}

int main(void)
{
    printf("test_envelope\n");
    test_starts_at_zero();
    test_attack_faster_than_release();
    test_monotonic_rise_and_fall();
    test_zero_time_constant_is_instantaneous();
    return TEST_MAIN_END();
}
