/**
 * @file expression.c
 * @brief Continuous CC11 Expression rate-limiter (project spec section 9,
 *        Milestone 7). See the yp_expression_process() doc comment in
 *        voice_midi.h for the throttling design and why "value changed
 *        enough" AND "enough time has passed" are both required, not
 *        either alone.
 */
#include "voice_midi.h"
#include "yp_config.h"

void yp_expression_init(yp_expression_t *ex)
{
    ex->last_sent_value = -1;
    ex->last_sent_time_us = 0;
}

bool yp_expression_process(yp_expression_t *ex, float level, int64_t now_us, int *out_value)
{
    int candidate = yp_level_to_cc_value(level, (yp_vel_curve_t)YP_DEFAULT_VELOCITY_CURVE);

    if (ex->last_sent_value < 0) {
        /* Nothing sent since the last reset: establish a baseline
         * unconditionally, so the receiving synth has a known starting
         * expression value for this note rather than inheriting
         * whatever the previous note last left it at. */
        ex->last_sent_value = candidate;
        ex->last_sent_time_us = now_us;
        *out_value = candidate;
        return true;
    }

    int delta = candidate - ex->last_sent_value;
    if (delta < 0) delta = -delta;
    int64_t elapsed_ms = (now_us - ex->last_sent_time_us) / 1000;

    if (delta >= YP_CC11_MIN_DELTA && elapsed_ms >= YP_CC11_MIN_INTERVAL_MS) {
        ex->last_sent_value = candidate;
        ex->last_sent_time_us = now_us;
        *out_value = candidate;
        return true;
    }

    return false;
}
