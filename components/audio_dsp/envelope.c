/**
 * @file envelope.c
 * @brief Attack/release envelope follower, applied to per-hop RMS to
 *        produce a smoothed amplitude ("level") independent of frame-to-
 *        frame RMS jitter. This is the signal later mapped to MIDI
 *        velocity/CC11 (Milestones 6-7).
 */
#include <math.h>
#include "audio_dsp_internal.h"

static float coeff_from_time_ms(float time_ms, float frame_period_ms)
{
    if (time_ms <= 0.0f) {
        return 0.0f; /* instantaneous */
    }
    /* Standard per-frame exponential coefficient: after `time_ms`, the
     * response reaches ~63% (1 - 1/e) of a step input. */
    return expf(-frame_period_ms / time_ms);
}

void envelope_init(envelope_t *env, float attack_ms, float release_ms, float frame_period_ms)
{
    env->attack_coeff = coeff_from_time_ms(attack_ms, frame_period_ms);
    env->release_coeff = coeff_from_time_ms(release_ms, frame_period_ms);
    env->state = 0.0f;
}

float envelope_process(envelope_t *env, float rectified_input)
{
    float coeff = (rectified_input > env->state) ? env->attack_coeff : env->release_coeff;
    env->state = coeff * env->state + (1.0f - coeff) * rectified_input;
    return env->state;
}
