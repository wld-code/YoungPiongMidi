/**
 * @file audio_capture.h
 * @brief Continuous-mode ADC microphone acquisition.
 *
 * Owns the ESP-IDF ADC continuous-mode driver for the ESP-SensairShuttle
 * analog microphone (GPIO6 / ADC1 channel 5) and turns its DMA'd raw
 * samples into blocks of conditioned (DC-removed, high-pass filtered,
 * normalized) floating point audio for the DSP layer.
 *
 * Threading model: a dedicated FreeRTOS task ("audio_capture") is woken by
 * the ADC continuous driver's conversion-done callback (ISR context - the
 * callback itself does nothing but a task notify) and does the actual
 * per-sample conditioning at task priority. Finished blocks are pushed onto
 * an internal queue; consumers call audio_capture_get_block() to receive
 * them. No DSP heavier than this conditioning happens in this component -
 * RMS/envelope/pitch live in audio_dsp/pitch, one hop behind.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "yp_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** Conditioned samples, normalized to approximately [-1.0, 1.0]. */
    float samples[YP_AUDIO_HOP_SIZE];
    /** Number of valid samples (== YP_AUDIO_HOP_SIZE in steady state). */
    size_t count;
    /** esp_timer_get_time() at which this block finished acquisition. */
    int64_t timestamp_us;
    /** true if any raw ADC sample in this block was within
     *  YP_AUDIO_CLIP_THRESHOLD of full scale. */
    bool clipped;
} audio_block_t;

/**
 * @brief Initialize the ADC continuous driver and the capture task.
 *
 * Must be called once, after yp_board_init(). Does not start acquisition;
 * call audio_capture_start() to begin producing blocks.
 */
esp_err_t audio_capture_init(void);

/** Begin continuous acquisition. */
esp_err_t audio_capture_start(void);

/** Stop acquisition (ADC hardware powers down its conversion path). */
esp_err_t audio_capture_stop(void);

/**
 * @brief Retrieve the next conditioned audio block, blocking up to timeout.
 *
 * @return ESP_OK with *out filled in, ESP_ERR_TIMEOUT if no block arrived
 *         in time, or an error from the underlying queue.
 */
esp_err_t audio_capture_get_block(audio_block_t *out, TickType_t timeout);

#ifdef __cplusplus
}
#endif
