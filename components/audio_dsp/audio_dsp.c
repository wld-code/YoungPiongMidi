#include <string.h>
#include <inttypes.h>
#include "audio_dsp.h"
#include "audio_dsp_internal.h"
#include "yp_config.h"
#include "pitch_detector.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "audio_dsp";

static envelope_t s_envelope;
static noise_gate_t s_noise_gate;
static bool s_voice_active;
static int s_active_run;   /* consecutive frames above threshold */
static int s_silent_run;   /* consecutive frames below threshold */

/* Pitch-stage latency instrumentation (project spec section 15: measure
 * pitch detection time specifically, not just the DSP block as a whole -
 * YIN is by far the heaviest stage in this pipeline). Logged on the same
 * cadence as main.c's own stats line. */
static int64_t s_pitch_time_sum_us;
static int64_t s_pitch_time_max_us;
static uint32_t s_pitch_frame_count;
static int64_t s_pitch_last_log_us;

esp_err_t audio_dsp_init(void)
{
    lpf_reset();
    pitch_detector_init();

    float frame_period_ms = 1000.0f * (float)YP_AUDIO_HOP_SIZE / (float)YP_AUDIO_SAMPLE_RATE_HZ;
    envelope_init(&s_envelope, YP_ENVELOPE_ATTACK_MS, YP_ENVELOPE_RELEASE_MS, frame_period_ms);
    noise_gate_init(&s_noise_gate, frame_period_ms);

    s_voice_active = false;
    s_active_run = 0;
    s_silent_run = 0;

    s_pitch_time_sum_us = 0;
    s_pitch_time_max_us = 0;
    s_pitch_frame_count = 0;
    s_pitch_last_log_us = 0;

    ESP_LOGI(TAG, "init: frame_period=%.2fms attack=%.1fms release=%.1fms",
             frame_period_ms, YP_ENVELOPE_ATTACK_MS, YP_ENVELOPE_RELEASE_MS);
    ESP_LOGI(TAG, "noise gate: initial_floor=%.4f margin=%.1fx min_floor=%.4f (adaptive, not fixed - see docs/tuning.md)",
             s_noise_gate.floor, YP_NOISE_GATE_MARGIN_RATIO, YP_NOISE_GATE_MIN_FLOOR);
    ESP_LOGI(TAG, "pitch: yin window=%d samples (%.1fms) range=%.0f-%.0fHz threshold=%.2f",
             YP_AUDIO_FRAME_SIZE, 1000.0f * YP_AUDIO_FRAME_SIZE / YP_AUDIO_SAMPLE_RATE_HZ,
             YP_PITCH_MIN_HZ, YP_PITCH_MAX_HZ, YP_PITCH_YIN_THRESHOLD);
    return ESP_OK;
}

void audio_dsp_process_block(const audio_block_t *block, voice_analysis_t *out)
{
    static float filtered[YP_AUDIO_HOP_SIZE];

    size_t n = block->count;
    if (n > YP_AUDIO_HOP_SIZE) {
        n = YP_AUDIO_HOP_SIZE;
    }
    for (size_t i = 0; i < n; i++) {
        filtered[i] = lpf_process(block->samples[i]);
    }

    float rms = rms_calculate(filtered, n);
    float level = envelope_process(&s_envelope, rms);

    /* Adaptive noise gate: the threshold itself tracks the ambient noise
     * floor (electrical hum, fan noise, handling the microphone) instead
     * of being a fixed number - see yp_config.h's YP_NOISE_GATE_* block
     * and noise_gate.c. gate_was_open is last hop's decision, since this
     * hop's decision doesn't exist yet (and the gate must not adapt
     * toward a level it is *currently* trusting as real voice). */
    float gate_threshold = noise_gate_process(&s_noise_gate, rms, s_voice_active);

    /* Debounced voice-activity detection: require YP_VAD_ATTACK_FRAMES
     * consecutive frames above threshold to declare active, and
     * YP_VAD_RELEASE_FRAMES consecutive frames below to declare silence.
     * Prevents single-frame noise spikes/dropouts from toggling state. */
    if (level >= gate_threshold) {
        s_active_run++;
        s_silent_run = 0;
        if (!s_voice_active && s_active_run >= YP_VAD_ATTACK_FRAMES) {
            s_voice_active = true;
        }
    } else {
        s_silent_run++;
        s_active_run = 0;
        if (s_voice_active && s_silent_run >= YP_VAD_RELEASE_FRAMES) {
            s_voice_active = false;
        }
    }

    int64_t pitch_t0 = esp_timer_get_time();
    pitch_estimate_t pitch = pitch_detector_process(filtered, n);
    int64_t pitch_us = esp_timer_get_time() - pitch_t0;

    s_pitch_time_sum_us += pitch_us;
    s_pitch_frame_count++;
    if (pitch_us > s_pitch_time_max_us) {
        s_pitch_time_max_us = pitch_us;
    }
    if ((pitch_t0 - s_pitch_last_log_us) >= (YP_DEBUG_STATS_INTERVAL_MS * 1000) && s_pitch_frame_count > 0) {
        s_pitch_last_log_us = pitch_t0;
        ESP_LOGI(TAG, "pitch stats: avg=%lldus max=%lldus (over %" PRIu32 " frames)",
                 (long long)(s_pitch_time_sum_us / s_pitch_frame_count),
                 (long long)s_pitch_time_max_us, s_pitch_frame_count);
        s_pitch_time_sum_us = 0;
        s_pitch_time_max_us = 0;
        s_pitch_frame_count = 0;
    }

    out->frequency_hz = pitch.frequency_hz;
    out->confidence = pitch.confidence;
    out->rms = rms;
    out->level = level;
    out->voice_active = s_voice_active;
}
