/**
 * @file rms.c
 * @brief Root-mean-square amplitude of one audio hop.
 */
#include <math.h>
#include "audio_dsp_internal.h"

float rms_calculate(const float *samples, size_t count)
{
    if (count == 0) {
        return 0.0f;
    }
    double sum_sq = 0.0;
    for (size_t i = 0; i < count; i++) {
        sum_sq += (double)samples[i] * (double)samples[i];
    }
    return sqrtf((float)(sum_sq / (double)count));
}
