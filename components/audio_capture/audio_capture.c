#include <math.h>
#include <string.h>
#include "audio_capture.h"
#include "yp_board.h"
#include "esp_adc/adc_continuous.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "audio_capture";

/* Task sizing. Documented here (rather than only in docs/architecture.md)
 * because this is where they take effect. The dominant stack user is the
 * local `adc_continuous_data_t parsed[YP_AUDIO_DMA_FRAME_SAMPLES]` scratch
 * array in capture_task (~16 bytes/element -> ~2 KB by itself at the
 * default hop size), plus ESP_LOG's own formatting stack use. Measured by
 * a stack-protection fault at 3 KB on real hardware; 8 KB gives headroom. */
#define CAPTURE_TASK_STACK_BYTES   8192
#define CAPTURE_TASK_PRIORITY      12   /* high: time-sensitive, but below
                                            any true ISR-adjacent work */

#define OUTPUT_QUEUE_LENGTH        4

/* ADC continuous driver sizing. */
#define ADC_CONV_FRAME_BYTES  (YP_AUDIO_DMA_FRAME_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES)
#define ADC_POOL_BYTES         (ADC_CONV_FRAME_BYTES * YP_AUDIO_DMA_BUFFER_COUNT)

static adc_continuous_handle_t s_adc_handle;
static TaskHandle_t s_capture_task_handle;
static QueueHandle_t s_output_queue;
static volatile bool s_running;

/* --- signal conditioning state (single mono channel) ------------------ */
static float s_dc_estimate;

/* 2nd-order (biquad) high-pass filter state and coefficients - Direct
 * Form I. See the big comment on condition_sample() below for why this
 * replaced a 1-pole filter. */
static float s_hpf_x1, s_hpf_x2;   /* x[n-1], x[n-2] */
static float s_hpf_y1, s_hpf_y2;   /* y[n-1], y[n-2] */
static float s_hpf_b0, s_hpf_b1, s_hpf_b2, s_hpf_a1, s_hpf_a2;

/* Assembly buffer for the block currently being filled. */
static audio_block_t s_building;
static size_t s_building_count;

static void conditioning_reset(void)
{
    s_dc_estimate = 2048.0f; /* mid-scale guess; converges quickly */

    s_hpf_x1 = s_hpf_x2 = 0.0f;
    s_hpf_y1 = s_hpf_y2 = 0.0f;

    /* RBJ cookbook high-pass biquad, Q = 1/sqrt(2) (Butterworth -
     * maximally flat passband, the standard choice absent a reason to
     * pick otherwise). Coefficients computed once here (trig is not
     * cheap, but this runs once at init, never in the per-sample path
     * below), matching the same "float only where it's not the hot
     * loop" rule this project applies to YIN's difference function -
     * see docs/tutorials/03-pitch-detection-yin.md. */
    const float Q = 0.70710678f;
    float w0 = 2.0f * (float)M_PI * YP_AUDIO_HPF_CUTOFF_HZ / (float)YP_AUDIO_SAMPLE_RATE_HZ;
    float cosw0 = cosf(w0);
    float alpha = sinf(w0) / (2.0f * Q);

    float b0 = (1.0f + cosw0) / 2.0f;
    float b1 = -(1.0f + cosw0);
    float b2 = (1.0f + cosw0) / 2.0f;
    float a0 = 1.0f + alpha;
    float a1 = -2.0f * cosw0;
    float a2 = 1.0f - alpha;

    s_hpf_b0 = b0 / a0;
    s_hpf_b1 = b1 / a0;
    s_hpf_b2 = b2 / a0;
    s_hpf_a1 = a1 / a0;
    s_hpf_a2 = a2 / a0;
}

/**
 * @brief Condition one raw 12-bit ADC code into a normalized float sample.
 *
 * Two distinct stages, per the project's audio-acquisition spec:
 *  1. DC offset removal: a very slow exponential tracker of the signal's
 *     mean, subtracted out. Handles bias-point drift with temperature/
 *     supply, not just the nominal mid-scale bias.
 *  2. High-pass filter: a 2nd-order (biquad) IIR at
 *     YP_AUDIO_HPF_CUTOFF_HZ, removing rumble/handling noise the DC
 *     tracker alone would not (it only removes near-0 Hz content).
 *     Confirmed necessary on real hardware, not a guess: touching the
 *     microphone produces broadband mechanical noise strongest at very
 *     low frequency, and a 1st-order filter's gentle -6dB/octave
 *     rolloff barely attenuated it - real MIDI events kept firing with
 *     no one singing. A 2nd-order filter's -12dB/octave rolloff is a
 *     real fix, not a style upgrade - see docs/tuning.md. This is only
 *     half of that fix; see audio_dsp.c's adaptive noise gate for the
 *     other half (rejecting whatever handling noise gets past this
 *     filter, and ambient/electrical noise this filter was never meant
 *     to touch).
 */
static float condition_sample(uint32_t raw12, bool *out_near_rail)
{
    *out_near_rail = (raw12 <= 8) || (raw12 >= 4087);

    /* 1. DC removal */
    const float kDcTrackerAlpha = 0.001f;
    s_dc_estimate += kDcTrackerAlpha * ((float)raw12 - s_dc_estimate);
    float dc_removed = (float)raw12 - s_dc_estimate;

    /* 2. High-pass filter (Direct Form I biquad) */
    float x0 = dc_removed;
    float y0 = s_hpf_b0 * x0 + s_hpf_b1 * s_hpf_x1 + s_hpf_b2 * s_hpf_x2
               - s_hpf_a1 * s_hpf_y1 - s_hpf_a2 * s_hpf_y2;
    s_hpf_x2 = s_hpf_x1;
    s_hpf_x1 = x0;
    s_hpf_y2 = s_hpf_y1;
    s_hpf_y1 = y0;

    /* 3. Normalize. Half of the 12-bit range approximates full-scale swing
     *    around the analog front-end's bias point. */
    float normalized = y0 / 2048.0f;
    if (normalized > 1.0f) normalized = 1.0f;
    if (normalized < -1.0f) normalized = -1.0f;
    return normalized;
}

static bool IRAM_ATTR adc_conv_done_cb(adc_continuous_handle_t handle,
                                        const adc_continuous_evt_data_t *edata,
                                        void *user_data)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_capture_task_handle, &woken);
    return woken == pdTRUE;
}

static void capture_task(void *arg)
{
    adc_continuous_data_t parsed[YP_AUDIO_DMA_FRAME_SAMPLES];
    uint32_t warn_ratelimit = 0;

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_running) {
            continue;
        }

        for (;;) {
            uint32_t num_parsed = 0;
            esp_err_t err = adc_continuous_read_parse(
                s_adc_handle, parsed, YP_AUDIO_DMA_FRAME_SAMPLES, &num_parsed, 0);
            if (err == ESP_ERR_TIMEOUT || num_parsed == 0) {
                break; /* drained everything currently available */
            }
            if (err != ESP_OK) {
                if ((warn_ratelimit++ % 200) == 0) {
                    ESP_LOGW(TAG, "adc_continuous_read_parse: %s", esp_err_to_name(err));
                }
                break;
            }

            for (uint32_t i = 0; i < num_parsed; i++) {
                if (!parsed[i].valid ||
                    parsed[i].unit != YP_MIC_ADC_UNIT ||
                    parsed[i].channel != YP_MIC_ADC_CHANNEL) {
                    continue;
                }

                bool near_rail = false;
                float sample = condition_sample(parsed[i].raw_data, &near_rail);

                s_building.samples[s_building_count++] = sample;
                if (near_rail) {
                    s_building.clipped = true;
                }

                if (s_building_count >= YP_AUDIO_HOP_SIZE) {
                    s_building.count = s_building_count;
                    s_building.timestamp_us = esp_timer_get_time();

                    if (xQueueSend(s_output_queue, &s_building, 0) != pdTRUE) {
                        /* Consumer (dsp_task) is behind. Drop the oldest
                         * queued block rather than blocking the capture
                         * task, which must never stall on downstream
                         * consumers. */
                        audio_block_t discard;
                        xQueueReceive(s_output_queue, &discard, 0);
                        xQueueSend(s_output_queue, &s_building, 0);
                        if ((warn_ratelimit++ % 200) == 0) {
                            ESP_LOGW(TAG, "output queue full, dropped a block");
                        }
                    }

                    s_building_count = 0;
                    s_building.clipped = false;
                }
            }
        }
    }
}

esp_err_t audio_capture_init(void)
{
    esp_err_t err;

    conditioning_reset();
    memset(&s_building, 0, sizeof(s_building));
    s_building_count = 0;

    s_output_queue = xQueueCreate(OUTPUT_QUEUE_LENGTH, sizeof(audio_block_t));
    if (!s_output_queue) {
        return ESP_ERR_NO_MEM;
    }

    /* Sanity-check the board's GPIO6 -> ADC1 channel 5 mapping via the
     * driver itself rather than trusting the macro blindly. */
    adc_unit_t unit;
    adc_channel_t channel;
    err = adc_continuous_io_to_channel(YP_PIN_MIC_ADC, &unit, &channel);
    if (err != ESP_OK || unit != YP_MIC_ADC_UNIT || channel != YP_MIC_ADC_CHANNEL) {
        ESP_LOGE(TAG, "GPIO%d does not map to ADC unit %d channel %d as expected (got unit=%d ch=%d, err=%s)",
                 YP_PIN_MIC_ADC, YP_MIC_ADC_UNIT, YP_MIC_ADC_CHANNEL, unit, channel, esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = ADC_POOL_BYTES,
        .conv_frame_size = ADC_CONV_FRAME_BYTES,
    };
    err = adc_continuous_new_handle(&handle_cfg, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_new_handle failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_digi_pattern_config_t pattern = {
        .atten = YP_AUDIO_ADC_ATTEN,
        .channel = YP_MIC_ADC_CHANNEL,
        .unit = YP_MIC_ADC_UNIT,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
    };
    adc_continuous_config_t dig_cfg = {
        .pattern_num = 1,
        .adc_pattern = &pattern,
        .sample_freq_hz = YP_AUDIO_SAMPLE_RATE_HZ,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };
    err = adc_continuous_config(s_adc_handle, &dig_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_config failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_continuous_evt_cbs_t cbs = {
        .on_conv_done = adc_conv_done_cb,
    };
    err = adc_continuous_register_event_callbacks(s_adc_handle, &cbs, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_register_event_callbacks failed: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreate(capture_task, "audio_capture",
                                 CAPTURE_TASK_STACK_BYTES / sizeof(StackType_t),
                                 NULL, CAPTURE_TASK_PRIORITY, &s_capture_task_handle);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create capture task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ADC continuous init done: %d Hz, GPIO%d (ADC%d ch%d), hop=%d",
             YP_AUDIO_SAMPLE_RATE_HZ, YP_PIN_MIC_ADC, YP_MIC_ADC_UNIT + 1,
             YP_MIC_ADC_CHANNEL, YP_AUDIO_HOP_SIZE);
    return ESP_OK;
}

esp_err_t audio_capture_start(void)
{
    esp_err_t err = adc_continuous_start(s_adc_handle);
    if (err == ESP_OK) {
        s_running = true;
    }
    return err;
}

esp_err_t audio_capture_stop(void)
{
    s_running = false;
    return adc_continuous_stop(s_adc_handle);
}

esp_err_t audio_capture_get_block(audio_block_t *out, TickType_t timeout)
{
    if (xQueueReceive(s_output_queue, out, timeout) == pdTRUE) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}
