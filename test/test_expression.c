/**
 * @file test_expression.c
 * @brief Host-side tests for components/voice_midi/expression.c
 *        (continuous CC11 Expression, project spec section 9,
 *        Milestone 7).
 *
 * The throttling rule is the one genuinely ambiguous piece of spec
 * wording in this whole project ("only transmit when the value changes
 * sufficiently OR after a configurable minimum interval") - see
 * yp_expression_process()'s doc comment in voice_midi.h for why this
 * implementation requires BOTH conditions, not either alone. These tests
 * make that choice an explicit, checked behavior rather than an
 * implementation detail nobody verified.
 */
#include "test_common.h"
#include "voice_midi.h"

#define HOP_US  8000

/* Level whose yp_level_to_cc_value() (default/log curve) sits roughly in
 * the middle of the 0..127 range, used as a convenient baseline several
 * tests perturb from. */
#define MID_LEVEL 0.10f

static void test_first_call_always_sends(void)
{
    TEST_CASE("the first process() call after init always sends, establishing a baseline");
    yp_expression_t ex;
    yp_expression_init(&ex);

    int value = -1;
    bool sent = yp_expression_process(&ex, MID_LEVEL, 0, &value);

    TEST_CHECK_TRUE(sent, "first call must send unconditionally");
    TEST_CHECK_TRUE(value >= 0 && value <= 127, "sent value must be a valid CC value");
    TEST_CHECK_EQ_INT(ex.last_sent_value, value, "tracker should record what it just sent");
}

static void test_unchanged_level_does_not_resend(void)
{
    TEST_CASE("an unchanged level never re-sends, even after a long time");
    yp_expression_t ex;
    yp_expression_init(&ex);

    int value;
    int64_t clock = 0;
    TEST_CHECK_TRUE(yp_expression_process(&ex, MID_LEVEL, clock, &value), "first call sends");

    for (int i = 0; i < 500; i++) {
        clock += HOP_US;
        bool sent = yp_expression_process(&ex, MID_LEVEL, clock, &value);
        TEST_CHECK_TRUE(!sent, "identical level must never trigger a resend");
    }
}

static void test_small_delta_alone_does_not_send(void)
{
    TEST_CASE("a delta below YP_CC11_MIN_DELTA never sends, no matter how much time passes (delta gate)");
    yp_expression_t ex;
    yp_expression_init(&ex);
    int value;
    int64_t clock = 0;
    yp_expression_process(&ex, MID_LEVEL, clock, &value);
    int baseline_cc = ex.last_sent_value;

    /* Find a level whose CC value is exactly baseline+1 (i.e. a delta
     * that is guaranteed to be < YP_CC11_MIN_DELTA, since
     * YP_CC11_MIN_DELTA >= 1 by construction in yp_config.h... actually
     * assert it directly instead of assuming). */
    TEST_CHECK_TRUE(YP_CC11_MIN_DELTA >= 2, "test assumes YP_CC11_MIN_DELTA >= 2 (true as configured)");

    /* Nudge the level up gradually and stop as soon as the CC value has
     * moved by exactly 1 from the baseline - a delta guaranteed smaller
     * than YP_CC11_MIN_DELTA. */
    float level = MID_LEVEL;
    int cc = baseline_cc;
    for (int i = 0; i < 2000 && cc < baseline_cc + 1; i++) {
        level += 0.0001f;
        cc = yp_level_to_cc_value(level, YP_VEL_CURVE_LOG);
    }
    TEST_CHECK_EQ_INT(cc, baseline_cc + 1, "test setup: should have found a +1 CC step");

    bool sent_early = false;
    for (int i = 0; i < 500; i++) {
        clock += HOP_US;
        int v;
        if (yp_expression_process(&ex, level, clock, &v)) {
            sent_early = true;
        }
    }
    TEST_CHECK_TRUE(!sent_early, "a 1-unit delta (< YP_CC11_MIN_DELTA) must never be sent, however long we wait");
}

static void test_large_delta_before_interval_is_withheld(void)
{
    TEST_CASE("a large delta is still withheld until YP_CC11_MIN_INTERVAL_MS has elapsed (interval gate)");
    yp_expression_t ex;
    yp_expression_init(&ex);
    int value;
    int64_t clock = 0;
    yp_expression_process(&ex, YP_DYNAMICS_NOISE_FLOOR, clock, &value);

    /* Jump straight to max level (guarantees a large delta) one hop
     * later - well before YP_CC11_MIN_INTERVAL_MS has elapsed at the
     * default 8ms hop period, unless YP_CC11_MIN_INTERVAL_MS <= 8. */
    if (YP_CC11_MIN_INTERVAL_MS > HOP_US / 1000) {
        clock += HOP_US;
        bool sent = yp_expression_process(&ex, YP_DYNAMICS_MAX_RMS, clock, &value);
        TEST_CHECK_TRUE(!sent, "large delta alone must not bypass the minimum interval gate");
    }
}

static void test_sends_once_both_gates_pass(void)
{
    TEST_CASE("sends exactly when both the delta and interval gates are satisfied");
    yp_expression_t ex;
    yp_expression_init(&ex);
    int value;
    int64_t clock = 0;
    yp_expression_process(&ex, YP_DYNAMICS_NOISE_FLOOR, clock, &value);
    int baseline = ex.last_sent_value;

    /* Advance the clock past the minimum interval, then present a large,
     * clearly-above-threshold delta. */
    clock += (int64_t)(YP_CC11_MIN_INTERVAL_MS + 5) * 1000;
    bool sent = yp_expression_process(&ex, YP_DYNAMICS_MAX_RMS, clock, &value);

    TEST_CHECK_TRUE(sent, "should send once both delta and interval gates are satisfied");
    TEST_CHECK_TRUE(value > baseline, "new value should reflect the louder level");
    TEST_CHECK_EQ_INT(ex.last_sent_value, value, "tracker should update to the newly sent value");
}

static void test_louder_increases_softer_decreases(void)
{
    TEST_CASE("voice becomes louder -> CC11 increases; voice becomes softer -> CC11 decreases (spec's own example)");
    yp_expression_t ex;
    yp_expression_init(&ex);
    int value;
    int64_t clock = 0;
    yp_expression_process(&ex, 0.05f, clock, &value);
    int v1 = ex.last_sent_value;

    clock += (int64_t)(YP_CC11_MIN_INTERVAL_MS + 5) * 1000;
    TEST_CHECK_TRUE(yp_expression_process(&ex, 0.30f, clock, &value), "should send: got louder");
    TEST_CHECK_TRUE(value > v1, "louder voice -> higher CC11 value");
    int v2 = value;

    clock += (int64_t)(YP_CC11_MIN_INTERVAL_MS + 5) * 1000;
    TEST_CHECK_TRUE(yp_expression_process(&ex, 0.02f, clock, &value), "should send: got softer");
    TEST_CHECK_TRUE(value < v2, "softer voice -> lower CC11 value");
}

static void test_reset_forces_fresh_baseline(void)
{
    TEST_CASE("yp_expression_init() resets the tracker so the very next call sends again immediately");
    yp_expression_t ex;
    yp_expression_init(&ex);
    int value;
    int64_t clock = 0;
    yp_expression_process(&ex, MID_LEVEL, clock, &value);

    /* Same level, same instant - would never send again without a
     * reset (see test_unchanged_level_does_not_resend). */
    yp_expression_init(&ex);
    bool sent = yp_expression_process(&ex, MID_LEVEL, clock, &value);
    TEST_CHECK_TRUE(sent, "after reset, an otherwise-identical call must send again");
}

static void test_cc_value_full_range_and_clamping(void)
{
    TEST_CASE("yp_level_to_cc_value: 0..127 full range (unlike velocity's 1..127), clamped both ends");

    TEST_CHECK_EQ_INT(yp_level_to_cc_value(0.0f, YP_VEL_CURVE_LOG), 0, "at/below noise floor -> CC 0 (not 1)");
    TEST_CHECK_EQ_INT(yp_level_to_cc_value(YP_DYNAMICS_MAX_RMS, YP_VEL_CURVE_LOG), 127, "at max RMS -> CC 127");
    TEST_CHECK_EQ_INT(yp_level_to_cc_value(YP_DYNAMICS_MAX_RMS * 5.0f, YP_VEL_CURVE_LOG), 127, "above max RMS clamps to 127");
    TEST_CHECK_EQ_INT(yp_level_to_cc_value(-1.0f, YP_VEL_CURVE_LOG), 0, "negative level clamps to 0");
}

int main(void)
{
    printf("test_expression\n");
    test_first_call_always_sends();
    test_unchanged_level_does_not_resend();
    test_small_delta_alone_does_not_send();
    test_large_delta_before_interval_is_withheld();
    test_sends_once_both_gates_pass();
    test_louder_increases_softer_decreases();
    test_reset_forces_fresh_baseline();
    test_cc_value_full_range_and_clamping();
    return TEST_MAIN_END();
}
