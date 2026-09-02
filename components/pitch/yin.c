/**
 * @file yin.c
 * @brief YIN fundamental-frequency estimator (de Cheveigne & Kawahara,
 *        "YIN, a fundamental frequency estimator for speech and music",
 *        JASA 2002).
 *
 * Algorithm, in the four steps this file implements:
 *
 *  1. Difference function: d(tau) = sum_{j=0}^{W-1} (x[j] - x[j+tau])^2
 *     for each lag tau in [0, tau_max], where W is the integration length
 *     (see WINDOW_SIZE/W_INT below). Low d(tau) means the signal at lag
 *     tau looks like the signal at lag 0 - i.e. tau is (a multiple of)
 *     the period.
 *  2. Cumulative mean normalized difference function (CMNDF): divides
 *     d(tau) by its running average, which turns the naturally-growing
 *     d(tau) into something with a comparable scale across tau, and
 *     defines CMNDF(0) = 1 so tau=0 is never mistaken for a minimum.
 *  3. Absolute threshold: walk tau upward from tau_min and take the
 *     first *local minimum* under YP_PITCH_YIN_THRESHOLD, rather than the
 *     global minimum - this is YIN's key trick for avoiding octave
 *     errors (locking onto 2x or 3x the true period). If nothing crosses
 *     the threshold, fall back to the global minimum over the search
 *     range, with correspondingly lower confidence.
 *  4. Parabolic interpolation around the chosen tau for sub-sample
 *     precision, since integer-sample lag resolution alone is far too
 *     coarse for musical pitch (at 16 kHz, tau=182 vs tau=183 for a
 *     voice around 88 Hz is already a ~9-cent step).
 *
 * Window sizing: the difference function needs a buffer of
 * W_INT + tau_max samples (see the WINDOW_SIZE/W_INT derivation below).
 * This implementation reuses YP_AUDIO_FRAME_SIZE as that buffer and
 * maintains it as a sliding window, advanced by YP_AUDIO_HOP_SIZE on each
 * call - the "overlapping analysis window" YP_AUDIO_FRAME_SIZE vs
 * YP_AUDIO_HOP_SIZE was sized for in yp_config.h.
 *
 * Fixed-point, not float, for the difference function - measured, not
 * guessed: ESP32-C5's RISC-V core builds with -march=rv32imac, i.e. no
 * 'F' (hardware single-precision float) extension (compare ESP32-H4/P4,
 * which get rv32imafc - see components/soc/project_include.cmake). Every
 * `float` multiply/add in this file's hot O(window x tau_range) loop was
 * therefore a soft-float library call, not a CPU instruction. The first
 * all-float version of this function measured ~79ms/hop on real hardware
 * (see docs/tuning.md) against an 8ms hop budget - almost exactly the
 * "several dozen cycles per soft-float op vs ~1 for a hardware integer
 * multiply" gap you'd expect. The difference function's inner loop below
 * is therefore int16 fixed-point (Q14, i.e. +/-1.0 audio maps to
 * +/-16384) accumulated in int64_t, which runs on the core's native
 * (hardware-multiplier) integer path; only the per-tau normalization,
 * threshold search and interpolation - O(tau_max) instead of
 * O(window x tau_max), a couple hundred ops per hop - are left as float,
 * where soft-float's cost is negligible.
 */
#include <string.h>
#include <math.h>
#include "pitch_detector.h"
#include "yp_config.h"

#define WINDOW_SIZE   YP_AUDIO_FRAME_SIZE

/* Q14 fixed point: +/-1.0f audio maps to +/-FIXED_SCALE. Chosen to leave
 * headroom under INT16_MAX (32767) against samples that briefly overshoot
 * +/-1.0 (the LPF ahead of this component is a convex combination of past
 * inputs, so in practice it does not, but the margin costs nothing). */
#define FIXED_SCALE   16384.0f

/* tau range, derived from the configured voice pitch range. tau_max is
 * clamped so the difference function's integration length (W_INT =
 * WINDOW_SIZE - tau_max) stays a healthy fraction of the window - too
 * large a tau_max relative to WINDOW_SIZE starves the difference
 * function of samples to average over and makes low estimates noisy. */
#define TAU_MAX_HARD_CAP  (WINDOW_SIZE / 2)

static int16_t s_window[WINDOW_SIZE];  /* Q14 fixed-point samples */
static size_t  s_fill_count;      /* samples written since last reset; caps at WINDOW_SIZE */
static bool    s_window_filled;

static int    s_tau_min;
static int    s_tau_max;
static int    s_w_int;           /* integration length used by the difference function */

/* CMNDF values for tau = 0..s_tau_max, reused across calls. */
static float s_cmndf[TAU_MAX_HARD_CAP + 1];

/* Holds the last *computed* estimate, returned on hops where the full
 * analysis is skipped (see YP_PITCH_UPDATE_STRIDE_HOPS). */
static pitch_estimate_t s_last_result;
static int s_stride_counter;

void pitch_detector_init(void)
{
    memset(s_window, 0, sizeof(s_window));
    s_fill_count = 0;
    s_window_filled = false;
    s_stride_counter = 0;
    s_last_result.frequency_hz = 0.0f;
    s_last_result.confidence = 0.0f;

    s_tau_min = (int)(YP_AUDIO_SAMPLE_RATE_HZ / YP_PITCH_MAX_HZ);
    if (s_tau_min < 2) {
        s_tau_min = 2;
    }
    s_tau_max = (int)(YP_AUDIO_SAMPLE_RATE_HZ / YP_PITCH_MIN_HZ);
    if (s_tau_max > TAU_MAX_HARD_CAP) {
        s_tau_max = TAU_MAX_HARD_CAP;
    }
    s_w_int = WINDOW_SIZE - s_tau_max;
}

/* Step 1 + 2 combined: fills s_cmndf[0..s_tau_max].
 *
 * The inner (j) loop is the O(W_int * tau_max) hot path - pure int32/
 * int64 integer math, no float in sight. The tau-th difference sum d(tau)
 * can reach ~s_w_int * (2*INT16_MAX)^2 ~= 312 * 1.7e9 ~= 5.3e11, safely
 * within int64_t range with headroom to spare. */
static void compute_cmndf(void)
{
    s_cmndf[0] = 1.0f;
    int64_t running_sum = 0;

    for (int tau = 1; tau <= s_tau_max; tau++) {
        int64_t d = 0;
        const int16_t *a = s_window;
        const int16_t *b = s_window + tau;
        for (int j = 0; j < s_w_int; j++) {
            int32_t diff = (int32_t)a[j] - (int32_t)b[j];
            d += (int64_t)diff * (int64_t)diff;
        }
        running_sum += d;
        s_cmndf[tau] = (running_sum > 0)
                           ? (float)(((double)d * tau) / (double)running_sum)
                           : 1.0f;
    }
}

/* Step 3: absolute threshold with local-minimum search, falling back to
 * the global minimum over [tau_min, tau_max] if nothing crosses the
 * threshold. Returns the chosen tau, or -1 if the range is empty. */
static int pick_tau(void)
{
    int best_tau = -1;
    float best_val = 2.0f; /* CMNDF is always < 2 in practice */

    for (int tau = s_tau_min; tau <= s_tau_max; tau++) {
        if (s_cmndf[tau] < best_val) {
            best_val = s_cmndf[tau];
            best_tau = tau;
        }

        if (s_cmndf[tau] < YP_PITCH_YIN_THRESHOLD) {
            /* First dip under threshold: walk forward while it keeps
             * dropping (local minimum), then stop - this is the
             * octave-error-avoiding step that makes YIN, YIN. */
            while (tau + 1 <= s_tau_max && s_cmndf[tau + 1] < s_cmndf[tau]) {
                tau++;
            }
            return tau;
        }
    }

    /* Nothing crossed the threshold - report the global minimum found
     * above, with whatever (low) confidence it implies. */
    return best_tau;
}

/* Step 4: parabolic interpolation around tau using its two neighbors. */
static void interpolate(int tau, float *out_tau, float *out_val)
{
    if (tau <= 0 || tau >= s_tau_max) {
        *out_tau = (float)tau;
        *out_val = s_cmndf[tau];
        return;
    }

    float y0 = s_cmndf[tau - 1];
    float y1 = s_cmndf[tau];
    float y2 = s_cmndf[tau + 1];

    float a = (y0 - 2.0f * y1 + y2) / 2.0f;
    float b = (y2 - y0) / 2.0f;

    if (fabsf(a) < 1e-9f) {
        *out_tau = (float)tau;
        *out_val = y1;
        return;
    }

    float shift = -b / (2.0f * a);
    /* Guard against a pathological fit pushing the vertex outside the
     * neighboring samples it was computed from. */
    if (shift < -1.0f) shift = -1.0f;
    if (shift > 1.0f)  shift = 1.0f;

    *out_tau = (float)tau + shift;
    *out_val = y1 - (b * b) / (4.0f * a);
}

static int16_t quantize(float x)
{
    float v = x * FIXED_SCALE;
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    return (int16_t)v;
}

pitch_estimate_t pitch_detector_process(const float *hop_samples, size_t count)
{
    if (count > YP_AUDIO_HOP_SIZE) {
        count = YP_AUDIO_HOP_SIZE;
    }

    /* Slide the analysis window every hop, regardless of whether this
     * hop actually runs the analysis below - the window must always
     * reflect the true, continuous, most recent audio. */
    memmove(s_window, s_window + count, (WINDOW_SIZE - count) * sizeof(int16_t));
    for (size_t i = 0; i < count; i++) {
        s_window[WINDOW_SIZE - count + i] = quantize(hop_samples[i]);
    }

    if (!s_window_filled) {
        s_fill_count += count;
        if (s_fill_count < WINDOW_SIZE) {
            return s_last_result; /* not enough history yet: stays {0,0} */
        }
        s_window_filled = true;
    }

    /* Decimate the expensive part only - see YP_PITCH_UPDATE_STRIDE_HOPS. */
    s_stride_counter++;
    if (s_stride_counter < YP_PITCH_UPDATE_STRIDE_HOPS) {
        return s_last_result;
    }
    s_stride_counter = 0;

    pitch_estimate_t result = { .frequency_hz = 0.0f, .confidence = 0.0f };

    compute_cmndf();
    int tau = pick_tau();
    if (tau < 0) {
        s_last_result = result;
        return result;
    }

    float tau_interp, cmndf_at_tau;
    interpolate(tau, &tau_interp, &cmndf_at_tau);

    if (tau_interp <= 0.0f) {
        s_last_result = result;
        return result;
    }

    result.frequency_hz = (float)YP_AUDIO_SAMPLE_RATE_HZ / tau_interp;
    float confidence = 1.0f - cmndf_at_tau;
    if (confidence < 0.0f) confidence = 0.0f;
    if (confidence > 1.0f) confidence = 1.0f;
    result.confidence = confidence;

    s_last_result = result;
    return result;
}
