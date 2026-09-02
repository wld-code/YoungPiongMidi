/**
 * @file audio_dsp.h
 * @brief Per-hop voice analysis: optional low-pass smoothing, RMS,
 *        envelope following and voice-activity detection.
 *
 * Pipeline position (see docs/dsp.md):
 *
 *   audio_capture (DC removal, HPF, normalize)
 *     -> audio_dsp: optional LPF -> RMS -> envelope -> VAD -> [pitch]
 *
 * Pitch detection (component `pitch`) is a separate, independently
 * replaceable stage; this component's job stops at "is there voice, and
 * how loud is it". voice_analysis_t already carries frequency_hz/
 * confidence fields so the interface does not change shape when pitch
 * detection lands (Milestone 3) - until then this component always
 * reports frequency_hz = 0 and confidence = 0.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "audio_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float frequency_hz;   /**< 0 until the pitch component is integrated. */
    float confidence;     /**< 0..1, 0 until the pitch component is integrated. */
    float rms;            /**< Raw per-hop RMS, ~0..1. */
    float level;          /**< Envelope-followed amplitude, ~0..1. */
    bool  voice_active;   /**< Debounced voice-activity flag. */
} voice_analysis_t;

/** Reset internal filter/envelope/VAD state. Call once before first use. */
esp_err_t audio_dsp_init(void);

/**
 * @brief Analyze one hop of conditioned audio.
 *
 * Pure function of (block, internal state) -> out; safe to call from a
 * single dedicated task (the dsp_task). Not reentrant / not thread-safe -
 * only one caller at a time.
 */
void audio_dsp_process_block(const audio_block_t *block, voice_analysis_t *out);

#ifdef __cplusplus
}
#endif
