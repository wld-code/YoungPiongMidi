/**
 * @file main.c
 * @brief YoungPiongMidi entry point.
 *
 * Current milestones implemented: 1 (continuous microphone acquisition +
 * basic signal display), 2 (RMS/envelope + voice activity detection),
 * 3 (YIN fundamental frequency detection - see components/pitch), and
 * 4 (frequency -> MIDI note conversion - see components/voice_midi).
 * See docs/architecture.md for the full roadmap and README.md for status.
 *
 * Task layout (see docs/architecture.md "FreeRTOS architecture" for the
 * rationale behind priorities/stack sizes):
 *
 *   audio_capture task (owned by the audio_capture component)
 *     -> [audio_block_t queue]
 *   dsp_task (this file): RMS / envelope / VAD, latency instrumentation
 *     -> [mutex-guarded latest voice_analysis_t]
 *   ui_task (this file): LCD refresh, decoupled from the DSP loop
 */
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"

#include "yp_board.h"
#include "yp_config.h"
#include "audio_capture.h"
#include "audio_dsp.h"
#include "display.h"
#include "self_test.h"
#include "voice_midi.h"

static const char *TAG = "main";

#define DSP_TASK_STACK_BYTES  4096
#define DSP_TASK_PRIORITY     11   /* below audio_capture (12), above UI */

#define UI_TASK_STACK_BYTES   3072
#define UI_TASK_PRIORITY      5

static SemaphoreHandle_t s_state_mutex;
static voice_analysis_t s_latest_analysis;
static bool s_latest_clipped;

/* -------------------------------------------------------------------- */
/*  dsp_task: consumes audio blocks, runs the DSP pipeline, publishes    */
/*  the latest result, logs rate-limited diagnostics and latency stats.  */
/* -------------------------------------------------------------------- */
static void dsp_task(void *arg)
{
    audio_block_t block;

    int64_t last_log_us = 0;
    int64_t last_stats_us = 0;
    int64_t proc_time_sum_us = 0;
    int64_t proc_time_max_us = 0;
    int64_t acq_to_done_sum_us = 0;
    uint32_t stats_frame_count = 0;

    while (1) {
        esp_err_t err = audio_capture_get_block(&block, pdMS_TO_TICKS(500));
        if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "no audio block received in 500ms - check microphone wiring/ADC init");
            continue;
        }

        int64_t t_process_start = esp_timer_get_time();
        voice_analysis_t analysis;
        audio_dsp_process_block(&block, &analysis);
        int64_t t_process_end = esp_timer_get_time();

        int64_t proc_time_us = t_process_end - t_process_start;
        int64_t acq_to_done_us = t_process_end - block.timestamp_us;

        proc_time_sum_us += proc_time_us;
        acq_to_done_sum_us += acq_to_done_us;
        stats_frame_count++;
        if (proc_time_us > proc_time_max_us) {
            proc_time_max_us = proc_time_us;
        }

        if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) == pdTRUE) {
            s_latest_analysis = analysis;
            s_latest_clipped = block.clipped;
            xSemaphoreGive(s_state_mutex);
        }

        int64_t now = t_process_end;

        if (YP_DEBUG_VOICE_LOG && (now - last_log_us) >= (YP_DEBUG_LOG_INTERVAL_MS * 1000)) {
            last_log_us = now;
            if (analysis.confidence >= YP_PITCH_CONFIDENCE_THRESHOLD && analysis.frequency_hz > 0.0f) {
                yp_note_info_t note = yp_frequency_to_note_info(analysis.frequency_hz);
                ESP_LOGI(TAG, "pitch=%.1fHz note=%s%d midi=%d cents=%.1f confidence=%.2f rms=%.4f level=%.4f voice_active=%d clipped=%d",
                         analysis.frequency_hz, note.note_name, note.octave, note.midi_note, note.cents,
                         analysis.confidence, analysis.rms, analysis.level, analysis.voice_active, block.clipped);
            } else {
                ESP_LOGI(TAG, "pitch=%.1fHz note=--- confidence=%.2f rms=%.4f level=%.4f voice_active=%d clipped=%d",
                         analysis.frequency_hz, analysis.confidence,
                         analysis.rms, analysis.level, analysis.voice_active, block.clipped);
            }
        }

        if ((now - last_stats_us) >= (YP_DEBUG_STATS_INTERVAL_MS * 1000) && stats_frame_count > 0) {
            last_stats_us = now;
            ESP_LOGI(TAG, "stats: frames=%" PRIu32 " dsp_proc_avg=%" PRId64 "us dsp_proc_max=%" PRId64
                          "us acq_to_done_avg=%" PRId64 "us free_heap=%" PRIu32,
                     stats_frame_count,
                     proc_time_sum_us / stats_frame_count,
                     proc_time_max_us,
                     acq_to_done_sum_us / stats_frame_count,
                     (uint32_t)esp_get_free_heap_size());
            proc_time_sum_us = 0;
            proc_time_max_us = 0;
            acq_to_done_sum_us = 0;
            stats_frame_count = 0;
        }
    }
}

/* -------------------------------------------------------------------- */
/*  ui_task: redraws the LCD at a fixed, DSP-independent rate.           */
/*                                                                        */
/*  Layout (240x284 portrait): title block at the top, pitch readout     */
/*  below it, the level meter vertically centered on the panel, and      */
/*  RMS/status below that. Y positions are named constants rather than   */
/*  scattered literals so the "meter in the middle" requirement is       */
/*  visibly a deliberate computation, not a guess.                       */
/* -------------------------------------------------------------------- */
#define UI_Y_TITLE      8
#define UI_Y_SUBTITLE   24
#define UI_Y_NOTE       44
#define UI_Y_FREQ       60
#define UI_Y_CONF       76
#define UI_METER_H      20
#define UI_Y_METER      ((DISPLAY_HEIGHT - UI_METER_H) / 2)   /* vertically centered */
#define UI_Y_LEVEL_LBL  (UI_Y_METER - 16)
#define UI_Y_RMS        (UI_Y_METER + UI_METER_H + 12)
#define UI_Y_STATUS_LBL (UI_Y_RMS + 20)
#define UI_Y_STATUS_VAL (UI_Y_STATUS_LBL + 16)

static void ui_draw_static(void)
{
    display_clear(DISPLAY_COLOR_BLACK);
    display_draw_text(8, UI_Y_TITLE, "Young Piong Midi controller",
                       DISPLAY_COLOR_CYAN, DISPLAY_COLOR_BLACK, 1);
    display_draw_text(8, UI_Y_SUBTITLE, "VOICE TO MIDI", DISPLAY_COLOR_GRAY, DISPLAY_COLOR_BLACK, 1);
    display_draw_text(8, UI_Y_LEVEL_LBL, "LEVEL", DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);
    display_draw_text(8, UI_Y_STATUS_LBL, "STATUS", DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);
}

static void ui_task(void *arg)
{
    ui_draw_static();

    const TickType_t period = pdMS_TO_TICKS(1000 / YP_UI_REFRESH_RATE_HZ);
    TickType_t last_wake = xTaskGetTickCount();

    char line[32];

    while (1) {
        voice_analysis_t analysis;
        bool clipped;
        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            analysis = s_latest_analysis;
            clipped = s_latest_clipped;
            xSemaphoreGive(s_state_mutex);

            bool pitch_ok = analysis.confidence >= YP_PITCH_CONFIDENCE_THRESHOLD
                             && analysis.frequency_hz > 0.0f;

            /* NOTE line: note name/octave vary enough in rendered width
             * (e.g. "A4" vs "C#-1") that a fixed printf field is fragile,
             * so clear the line's rect before redrawing it rather than
             * relying on field-width padding like the lines below do. */
            display_fill_rect(8, UI_Y_NOTE, DISPLAY_WIDTH - 16, 8, DISPLAY_COLOR_BLACK);
            if (pitch_ok) {
                yp_note_info_t note = yp_frequency_to_note_info(analysis.frequency_hz);
                if (note.valid) {
                    snprintf(line, sizeof(line), "NOTE %s%d %+.0fC", note.note_name, note.octave, note.cents);
                    display_draw_text(8, UI_Y_NOTE, line, DISPLAY_COLOR_YELLOW, DISPLAY_COLOR_BLACK, 1);
                }
            } else {
                display_draw_text(8, UI_Y_NOTE, "NOTE ---", DISPLAY_COLOR_GRAY, DISPLAY_COLOR_BLACK, 1);
            }

            /* Fixed field widths so a shorter new string never leaves a
             * fragment of a longer old one behind on screen. */
            if (pitch_ok) {
                snprintf(line, sizeof(line), "FREQ%7.1fHZ", analysis.frequency_hz);
            } else {
                snprintf(line, sizeof(line), "FREQ%7sHZ", "---");
            }
            display_draw_text(8, UI_Y_FREQ, line,
                               pitch_ok ? DISPLAY_COLOR_YELLOW : DISPLAY_COLOR_GRAY,
                               DISPLAY_COLOR_BLACK, 1);

            snprintf(line, sizeof(line), "CONF%5.2f", analysis.confidence);
            display_draw_text(8, UI_Y_CONF, line, DISPLAY_COLOR_GRAY, DISPLAY_COLOR_BLACK, 1);

            /* Voice levels sit well under 1.0 in practice (VAD threshold
             * is 0.02); scale up so the meter uses its visual range. */
            display_draw_meter(8, UI_Y_METER, DISPLAY_WIDTH - 16, UI_METER_H, analysis.level * 3.0f,
                                DISPLAY_COLOR_GREEN, DISPLAY_COLOR_BLACK);

            snprintf(line, sizeof(line), "RMS %.3f", analysis.rms);
            display_draw_text(8, UI_Y_RMS, line, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);

            const char *status = clipped ? "CLIPPING" : (analysis.voice_active ? "VOICE" : "SILENCE");
            uint16_t status_color = clipped ? DISPLAY_COLOR_RED
                                     : (analysis.voice_active ? DISPLAY_COLOR_GREEN : DISPLAY_COLOR_GRAY);
            display_fill_rect(8, UI_Y_STATUS_VAL, DISPLAY_WIDTH - 16, 10, DISPLAY_COLOR_BLACK);
            display_draw_text(8, UI_Y_STATUS_VAL, status, status_color, DISPLAY_COLOR_BLACK, 1);
        }

        vTaskDelayUntil(&last_wake, period);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "YoungPiongMidi starting");

    ESP_ERROR_CHECK(yp_board_init());

    if (YP_LCD_ENABLED) {
        ESP_ERROR_CHECK(display_init());
        self_test_run();
    }

    ESP_ERROR_CHECK(audio_dsp_init());
    ESP_ERROR_CHECK(audio_capture_init());

    s_state_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex) {
        ESP_LOGE(TAG, "failed to create state mutex");
        abort();
    }

    BaseType_t ok = xTaskCreate(dsp_task, "dsp_task", DSP_TASK_STACK_BYTES / sizeof(StackType_t),
                                 NULL, DSP_TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create dsp_task");
        abort();
    }

    if (YP_LCD_ENABLED) {
        ok = xTaskCreate(ui_task, "ui_task", UI_TASK_STACK_BYTES / sizeof(StackType_t),
                          NULL, UI_TASK_PRIORITY, NULL);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "failed to create ui_task");
            abort();
        }
    }

    ESP_ERROR_CHECK(audio_capture_start());

    ESP_LOGI(TAG, "init complete: sample_rate=%dHz hop=%d frame=%d",
             YP_AUDIO_SAMPLE_RATE_HZ, YP_AUDIO_HOP_SIZE, YP_AUDIO_FRAME_SIZE);

    /* app_main can return on ESP-IDF (its task is cleaned up); all real
     * work happens in dsp_task / ui_task / the audio_capture task. */
}
