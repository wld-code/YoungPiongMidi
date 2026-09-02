#include <string.h>
#include "audio_dsp.h"
#include "audio_dsp_internal.h"
#include "yp_config.h"
#include "esp_log.h"

static const char *TAG = "audio_dsp";

static envelope_t s_envelope;
static bool s_voice_active;
static int s_active_run;   /* consecutive frames above threshold */
static int s_silent_run;   /* consecutive frames below threshold */

esp_err_t audio_dsp_init(void)
{
    lpf_reset();

    float frame_period_ms = 1000.0f * (float)YP_AUDIO_HOP_SIZE / (float)YP_AUDIO_SAMPLE_RATE_HZ;
    envelope_init(&s_envelope, YP_ENVELOPE_ATTACK_MS, YP_ENVELOPE_RELEASE_MS, frame_period_ms);

    s_voice_active = false;
    s_active_run = 0;
    s_silent_run = 0;

    ESP_LOGI(TAG, "init: frame_period=%.2fms attack=%.1fms release=%.1fms vad_thresh=%.3f",
             frame_period_ms, YP_ENVELOPE_ATTACK_MS, YP_ENVELOPE_RELEASE_MS, YP_VAD_RMS_THRESHOLD);
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

    /* Debounced voice-activity detection: require YP_VAD_ATTACK_FRAMES
     * consecutive frames above threshold to declare active, and
     * YP_VAD_RELEASE_FRAMES consecutive frames below to declare silence.
     * Prevents single-frame noise spikes/dropouts from toggling state. */
    if (level >= YP_VAD_RMS_THRESHOLD) {
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

    out->frequency_hz = 0.0f;  /* Milestone 3: pitch component */
    out->confidence = 0.0f;
    out->rms = rms;
    out->level = level;
    out->voice_active = s_voice_active;
}
