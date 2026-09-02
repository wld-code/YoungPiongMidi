/**
 * @file audio_dsp.h
 * @brief Per-hop voice analysis: optional low-pass smoothing, RMS,
 *        envelope following, voice-activity detection, and pitch.
 *
 * Pipeline position (see docs/dsp.md):
 *
 *   audio_capture (DC removal, HPF, normalize)
 *     -> audio_dsp: optional LPF -> RMS -> envelope -> VAD -> pitch
 *
 * Pitch detection itself lives in the separate, independently replaceable
 * `pitch` component (currently YIN, see pitch/yin.c) - this file only
 * calls into it and merges its result into voice_analysis_t, once per
 * hop, timed for the latency stats in docs/hardware.md/tuning.md.
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
    /** YIN pitch estimate, Hz. 0 if the analysis window hasn't filled yet
     *  (first ~YP_AUDIO_FRAME_SIZE/YP_AUDIO_HOP_SIZE hops after init).
     *  Always check `confidence` before trusting this for anything - a
     *  low-confidence value is still a real (if unreliable) estimate,
     *  not a sentinel. */
    float frequency_hz;
    /** 0..1. Compare against YP_PITCH_CONFIDENCE_THRESHOLD. */
    float confidence;
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
