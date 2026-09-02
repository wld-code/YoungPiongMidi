/**
 * @file audio_dsp_internal.h
 * @brief Private interfaces between audio_dsp.c and its filters/rms/
 *        envelope sub-modules. Not part of the component's public API.
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- filters.c: single-pole low-pass, pre-RMS ------------------------- */
void   lpf_reset(void);
float  lpf_process(float x);

/* --- rms.c -------------------------------------------------------------*/
float  rms_calculate(const float *samples, size_t count);

/* --- envelope.c: attack/release envelope follower ---------------------*/
typedef struct {
    float attack_coeff;
    float release_coeff;
    float state;
} envelope_t;

void   envelope_init(envelope_t *env, float attack_ms, float release_ms, float frame_period_ms);
float  envelope_process(envelope_t *env, float rectified_input);

#ifdef __cplusplus
}
#endif
