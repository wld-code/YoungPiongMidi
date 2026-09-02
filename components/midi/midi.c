#include <string.h>
#include "midi.h"
#include "onboard_synth.h"
#include "yp_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "midi";

#define MIDI_TASK_STACK_BYTES  3072
#define MIDI_TASK_PRIORITY     6   /* above ui_task (5): a stuck transport
                                       must not stall behind LCD drawing,
                                       but this is still well below the
                                       audio/DSP path (11-12) - MIDI
                                       output is not a hard real-time
                                       deadline the way ADC draining is */

static QueueHandle_t s_queue;

static void log_event(const midi_event_t *ev)
{
    switch (ev->type) {
        case MIDI_EVENT_NOTE_ON:
            ESP_LOGI(TAG, "NOTE_ON  ch=%u note=%u vel=%u", ev->channel, ev->data1, ev->data2);
            break;
        case MIDI_EVENT_NOTE_OFF:
            ESP_LOGI(TAG, "NOTE_OFF ch=%u note=%u vel=%u", ev->channel, ev->data1, ev->data2);
            break;
        case MIDI_EVENT_CC:
            ESP_LOGI(TAG, "CC       ch=%u cc=%u val=%u", ev->channel, ev->data1, ev->data2);
            break;
        case MIDI_EVENT_PITCH_BEND:
            ESP_LOGI(TAG, "PBEND    ch=%u val=%u", ev->channel, ev->data2);
            break;
    }
}

/* Dispatch one dequeued event to every enabled transport. With both
 * YP_MIDI_BLE_ENABLED and YP_MIDI_UART_ENABLED at 0 (current default -
 * see yp_config.h), the #if below compiles away to nothing but
 * log_event()/onboard_synth_handle_event(); flipping either flag once
 * its transport component exists (Milestones 8/9) adds a real send call
 * here without anything upstream changing.
 *
 * onboard_synth is not one of the spec's BLE/UART transports - it's a
 * "hear it on the board itself, no laptop needed" path, self-contained
 * and always on (onboard_synth_handle_event() no-ops safely if the
 * synth failed to init, so this is never a reason for a dequeued event
 * to be lost). */
static void dispatch_event(const midi_event_t *ev)
{
    log_event(ev);
    onboard_synth_handle_event(ev);

#if YP_MIDI_BLE_ENABLED
#error "YP_MIDI_BLE_ENABLED is set but midi_ble.c does not exist yet (Milestone 8)"
#endif
#if YP_MIDI_UART_ENABLED
#error "YP_MIDI_UART_ENABLED is set but midi_uart.c does not exist yet (Milestone 9)"
#endif
}

static void midi_task(void *arg)
{
    midi_event_t ev;
    while (1) {
        if (xQueueReceive(s_queue, &ev, portMAX_DELAY) == pdTRUE) {
            dispatch_event(&ev);
        }
    }
}

esp_err_t midi_init(void)
{
    s_queue = xQueueCreate(YP_MIDI_QUEUE_LENGTH, sizeof(midi_event_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(midi_task, "midi_task", MIDI_TASK_STACK_BYTES / sizeof(StackType_t),
                                 NULL, MIDI_TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "init done (queue depth=%d, no wire transport enabled - see docs/midi.md)",
             YP_MIDI_QUEUE_LENGTH);
    return ESP_OK;
}

static esp_err_t enqueue(midi_event_type_t type, uint8_t channel, uint8_t data1, uint16_t data2)
{
    midi_event_t ev = {
        .type = type,
        .channel = channel,
        .data1 = data1,
        .data2 = data2,
        .timestamp_us = esp_timer_get_time(),
    };
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, dropped a MIDI event (type=%d)", type);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t midi_send_note_on(uint8_t channel, uint8_t note, uint8_t velocity)
{
    return enqueue(MIDI_EVENT_NOTE_ON, channel, note, velocity);
}

esp_err_t midi_send_note_off(uint8_t channel, uint8_t note, uint8_t velocity)
{
    return enqueue(MIDI_EVENT_NOTE_OFF, channel, note, velocity);
}

esp_err_t midi_send_cc(uint8_t channel, uint8_t controller, uint8_t value)
{
    return enqueue(MIDI_EVENT_CC, channel, controller, value);
}

esp_err_t midi_send_pitch_bend(uint8_t channel, uint16_t value)
{
    return enqueue(MIDI_EVENT_PITCH_BEND, channel, 0, value);
}
