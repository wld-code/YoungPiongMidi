/**
 * @file test_dynamics.c
 * @brief Host-side tests for vocal dynamics -> MIDI velocity mapping
 *        (project spec section 8, Milestone 6): yp_level_to_velocity()
 *        in components/voice_midi/voice_midi.c.
 */
#include "test_common.h"
#include "voice_midi.h"

static void test_bounds_and_clamping(void)
{
    TEST_CASE("bounds: noise floor -> minimum velocity, max RMS (and above) -> maximum velocity");

    yp_vel_curve_t curves[2] = { YP_VEL_CURVE_LINEAR, YP_VEL_CURVE_LOG };
    const char *names[2] = { "linear", "log" };

    for (int c = 0; c < 2; c++) {
        char msg[96];

        snprintf(msg, sizeof(msg), "%s: below noise floor clamps to minimum velocity", names[c]);
        TEST_CHECK_EQ_INT(yp_level_to_velocity(0.0f, curves[c]), YP_MIDI_VELOCITY_MIN, msg);

        snprintf(msg, sizeof(msg), "%s: exactly at noise floor -> minimum velocity", names[c]);
        TEST_CHECK_EQ_INT(yp_level_to_velocity(YP_DYNAMICS_NOISE_FLOOR, curves[c]), YP_MIDI_VELOCITY_MIN, msg);

        snprintf(msg, sizeof(msg), "%s: exactly at max RMS -> maximum velocity", names[c]);
        TEST_CHECK_EQ_INT(yp_level_to_velocity(YP_DYNAMICS_MAX_RMS, curves[c]), YP_MIDI_VELOCITY_MAX, msg);

        snprintf(msg, sizeof(msg), "%s: above max RMS clamps (does not wrap/overflow)", names[c]);
        TEST_CHECK_EQ_INT(yp_level_to_velocity(YP_DYNAMICS_MAX_RMS * 10.0f, curves[c]), YP_MIDI_VELOCITY_MAX, msg);

        snprintf(msg, sizeof(msg), "%s: negative level clamps the same as 0", names[c]);
        TEST_CHECK_EQ_INT(yp_level_to_velocity(-1.0f, curves[c]), YP_MIDI_VELOCITY_MIN, msg);
    }
}

static void test_monotonicity(void)
{
    TEST_CASE("velocity is non-decreasing as level increases, for both curves");

    yp_vel_curve_t curves[2] = { YP_VEL_CURVE_LINEAR, YP_VEL_CURVE_LOG };
    for (int c = 0; c < 2; c++) {
        int prev = YP_MIDI_VELOCITY_MIN;
        bool monotonic = true;
        for (int i = 1; i <= 50; i++) {
            float level = YP_DYNAMICS_NOISE_FLOOR
                          + (YP_DYNAMICS_MAX_RMS - YP_DYNAMICS_NOISE_FLOOR) * ((float)i / 50.0f);
            int v = yp_level_to_velocity(level, curves[c]);
            if (v < prev) monotonic = false;
            prev = v;
        }
        TEST_CHECK_TRUE(monotonic, "velocity must never decrease as level increases");
    }
}

static void test_matches_spec_reference_mapping(void)
{
    TEST_CASE("the default (log) curve roughly matches the spec's own example mapping");

    /* Project spec section 8, "example conceptual mapping":
     *   very soft voice -> velocity around 20
     *   normal voice     -> velocity around 70
     *   strong voice      -> velocity around 120
     * These levels are illustrative operating points within
     * [YP_DYNAMICS_NOISE_FLOOR, YP_DYNAMICS_MAX_RMS], not defined by the
     * spec as exact RMS values - the point of this test is that the
     * curve's *shape* lands close to the spec's intent at plausible
     * soft/normal/strong points, not that any specific RMS number is
     * gospel. A generous +/-20 velocity tolerance reflects that. */
    TEST_CHECK_NEAR(yp_level_to_velocity(0.03f, YP_VEL_CURVE_LOG), 20, 20, "soft voice (~0.03 RMS) -> velocity near 20");
    TEST_CHECK_NEAR(yp_level_to_velocity(0.10f, YP_VEL_CURVE_LOG), 70, 20, "normal voice (~0.10 RMS) -> velocity near 70");
    TEST_CHECK_NEAR(yp_level_to_velocity(0.45f, YP_VEL_CURVE_LOG), 120, 20, "strong voice (~0.45 RMS) -> velocity near 120");
}

static void test_log_curve_differs_meaningfully_from_linear(void)
{
    TEST_CASE("log curve gives quiet/medium levels more of the velocity range than linear (spec: avoid poor musical behaviour)");

    /* At a moderate level, well below max RMS, a raw linear mapping
     * spends most of its output range on levels the singer will rarely
     * produce (near max RMS), clustering ordinary singing into a narrow
     * low-velocity band - the "poor musical behaviour" the spec warns
     * against. The log (dB-linear) curve should read noticeably higher
     * here, not just "different by a few units". */
    float moderate_level = 0.08f;
    int log_vel = yp_level_to_velocity(moderate_level, YP_VEL_CURVE_LOG);
    int lin_vel = yp_level_to_velocity(moderate_level, YP_VEL_CURVE_LINEAR);

    TEST_CHECK_TRUE(log_vel > lin_vel + 20,
                     "log-curve velocity should be substantially higher than linear at a moderate level");
}

static void test_default_curve_is_log(void)
{
    TEST_CASE("YP_DEFAULT_VELOCITY_CURVE is the log curve (perceptual, per spec section 8)");
    TEST_CHECK_EQ_INT(YP_DEFAULT_VELOCITY_CURVE, YP_VELOCITY_CURVE_LOG, "default curve should be log, not linear");
}

int main(void)
{
    printf("test_dynamics\n");
    test_bounds_and_clamping();
    test_monotonicity();
    test_matches_spec_reference_mapping();
    test_log_curve_differs_meaningfully_from_linear();
    test_default_curve_is_log();
    return TEST_MAIN_END();
}
