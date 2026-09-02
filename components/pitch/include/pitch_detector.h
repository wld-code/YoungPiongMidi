/**
 * @file pitch_detector.h
 * @brief Monophonic fundamental-frequency (pitch) detection.
 *
 * Narrow, algorithm-agnostic interface: feed it one hop of audio, get back
 * a frequency estimate and a confidence. The current implementation
 * (yin.c) uses YIN (de Cheveigne & Kawahara, 2002); nothing outside this
 * component or its own internal windowing needs to know that - a
 * different algorithm could replace yin.c without changing this header
 * or any caller, per the project's "pitch detector must be its own
 * swappable component" requirement.
 *
 * The implementation owns its own sliding analysis window internally
 * (sized YP_AUDIO_FRAME_SIZE, advanced by YP_AUDIO_HOP_SIZE per call) -
 * callers just feed hops, they do not manage windowing.
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** Estimated fundamental frequency in Hz. 0 if no pitch could be
     *  estimated yet (e.g. the analysis window has not filled since
     *  init/reset) - NOT the same as low confidence, which still returns
     *  a best-effort frequency. Always within
     *  [YP_PITCH_MIN_HZ, YP_PITCH_MAX_HZ] when nonzero. */
    float frequency_hz;
    /** 0..1. Higher = more periodic/confident. Compare against
     *  YP_PITCH_CONFIDENCE_THRESHOLD before trusting frequency_hz for
     *  anything downstream (note stabilization, display, etc). */
    float confidence;
} pitch_estimate_t;

/** Reset internal windowing/state. Call once before first use, or to
 *  discard history (e.g. after a long silence). */
void pitch_detector_init(void);

/**
 * @brief Feed one hop of audio and get the latest pitch estimate.
 *
 * @param hop_samples audio, ideally already DC-removed/filtered to
 *        roughly the [YP_PITCH_MIN_HZ, YP_PITCH_MAX_HZ] voice range -
 *        see docs/dsp.md for where this sits in the pipeline.
 * @param count number of samples in hop_samples (normally
 *        YP_AUDIO_HOP_SIZE; smaller is accepted, larger is truncated).
 */
pitch_estimate_t pitch_detector_process(const float *hop_samples, size_t count);

#ifdef __cplusplus
}
#endif
