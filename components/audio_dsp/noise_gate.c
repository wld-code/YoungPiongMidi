/**
 * @file noise_gate.c
 * @brief Adaptive ambient-noise-floor tracker, replacing a fixed VAD
 *        threshold. See the big comment block on YP_NOISE_GATE_* in
 *        yp_config.h for the reasoning (confirmed on real hardware,
 *        not a guess: a fixed threshold let touching the microphone,
 *        or plain ambient noise, generate real MIDI events).
 */
#include <math.h>
#include "audio_dsp_internal.h"
#include "yp_config.h"

static float coeff_from_time_ms(float time_ms, float frame_period_ms)
{
    /* Same "per-frame exponential coefficient from a time constant"
     * derivation as envelope.c - see that file for the reasoning. */
    return expf(-frame_period_ms / time_ms);
}

void noise_gate_init(noise_gate_t *gate, float frame_period_ms)
{
    gate->floor = YP_NOISE_GATE_MIN_FLOOR;
    gate->down_coeff = coeff_from_time_ms(YP_NOISE_GATE_DOWN_TIME_MS, frame_period_ms);
    gate->up_coeff = coeff_from_time_ms(YP_NOISE_GATE_UP_TIME_MS, frame_period_ms);
}

float noise_gate_process(noise_gate_t *gate, float rms, bool gate_was_open)
{
    if (rms < gate->floor) {
        /* Always safe to track a quieter room quickly, regardless of
         * whether the gate is open - a genuine voice pausing mid-phrase
         * should let the floor start re-converging immediately, not
         * wait for the gate to formally close first. */
        gate->floor += (1.0f - gate->down_coeff) * (rms - gate->floor);
    } else if (!gate_was_open) {
        /* Signal is above the floor AND the gate was closed on the
         * previous frame - i.e. this level wasn't trusted as voice.
         * Let the floor slowly rise to meet it. This is what lets a
         * persistently noisier environment (not a real voice) actually
         * raise the threshold instead of triggering forever. */
        gate->floor += (1.0f - gate->up_coeff) * (rms - gate->floor);
    }
    /* else: above the floor and the gate was open (this is being
     * trusted as real voice) - do not adapt at all. This is the crux of
     * the whole mechanism: a loud, sustained, real voice must never be
     * allowed to drag its own gate threshold up underneath it. */

    if (gate->floor < YP_NOISE_GATE_MIN_FLOOR) {
        gate->floor = YP_NOISE_GATE_MIN_FLOOR;
    }

    return gate->floor * YP_NOISE_GATE_MARGIN_RATIO;
}
