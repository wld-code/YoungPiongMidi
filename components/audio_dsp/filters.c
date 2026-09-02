/**
 * @file filters.c
 * @brief Optional single-pole low-pass filter applied ahead of RMS/pitch
 *        analysis (docs/dsp.md pipeline: HPF [audio_capture] -> LPF (here)
 *        -> RMS -> envelope -> VAD -> pitch).
 *
 * A gentle low-pass ahead of pitch detection reduces high-frequency
 * breath/sibilance energy that would otherwise show up as spurious zero
 * crossings and hurt YIN's period estimate, without materially affecting
 * the fundamentals of the 80-1000 Hz voice range this project targets.
 */
#include <math.h>
#include "audio_dsp_internal.h"
#include "yp_config.h"

static float s_prev_out;
static float s_alpha;

void lpf_reset(void)
{
    s_prev_out = 0.0f;
    float dt = 1.0f / (float)YP_AUDIO_SAMPLE_RATE_HZ;
    float rc = 1.0f / (2.0f * (float)M_PI * YP_AUDIO_LPF_CUTOFF_HZ);
    s_alpha = dt / (rc + dt);
}

float lpf_process(float x)
{
    s_prev_out += s_alpha * (x - s_prev_out);
    return s_prev_out;
}
