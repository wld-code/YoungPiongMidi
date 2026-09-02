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

/* --- noise_gate.c: adaptive ambient-noise-floor tracker ----------------*/
typedef struct {
    float floor;
    float down_coeff; /* per-frame coefficient, floor adapting toward quieter */
    float up_coeff;   /* per-frame coefficient, floor adapting toward louder (gate-closed only) */
} noise_gate_t;

void   noise_gate_init(noise_gate_t *gate, float frame_period_ms);
/**
 * @brief Update the adaptive floor from this frame's RMS and return the
 *        current gate threshold (floor * YP_NOISE_GATE_MARGIN_RATIO,
 *        clamped to YP_NOISE_GATE_MIN_FLOOR).
 *
 * @param rms         this frame's (unsmoothed) RMS
 * @param gate_was_open  whether the gate was open (voice active) on the
 *        *previous* frame - the floor only adapts upward while the gate
 *        is closed, so a real, loud voice can never drag its own
 *        threshold up out from under it. Always adapts downward
 *        regardless (finding a quieter room is always safe to do fast).
 */
float  noise_gate_process(noise_gate_t *gate, float rms, bool gate_was_open);

#ifdef __cplusplus
}
#endif
