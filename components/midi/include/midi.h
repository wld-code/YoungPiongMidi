/**
 * @file midi.h
 * @brief Transport-independent MIDI event engine (project spec
 *        section 10).
 *
 * Nothing upstream of this component (audio_dsp, pitch, voice_midi) ever
 * touches BLE or UART - they call midi_send_*(), which builds a
 * midi_event_t and queues it; midi_task (in midi.c) dequeues and hands
 * each event to whichever transport(s) are enabled.
 *
 * Current status (see docs/midi.md): YP_MIDI_BLE_ENABLED and
 * YP_MIDI_UART_ENABLED are both 0 - no real transport exists yet
 * (Milestones 8/9). Until at least one is implemented and enabled,
 * midi_task's only "transport" is a diagnostic ESP_LOGI line per event,
 * so Note On/Off/CC/Pitch Bend generation (Milestone 5+) can be verified
 * end to end on the console before any wire protocol exists.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MIDI_EVENT_NOTE_ON,
    MIDI_EVENT_NOTE_OFF,
    MIDI_EVENT_CC,
    MIDI_EVENT_PITCH_BEND,
} midi_event_type_t;

typedef struct {
    midi_event_type_t type;
    uint8_t  channel;      /**< 0-based (0 = MIDI channel 1) */
    uint8_t  data1;        /**< note number, or CC controller number */
    uint16_t data2;        /**< velocity/CC value (0..127), or 14-bit pitch bend (0..16383, 8192 = center) */
    int64_t  timestamp_us; /**< esp_timer_get_time() at the moment this event was generated */
} midi_event_t;

/**
 * @brief Create the MIDI event queue and start midi_task.
 *
 * Call once, before any midi_send_*() call. Safe to call even with no
 * transport enabled - see the file header.
 */
esp_err_t midi_init(void);

/** @name Event producers
 *  Build a midi_event_t (with the current timestamp) and enqueue it.
 *  Non-blocking: if the queue is momentarily full, the event is dropped
 *  and a rate-limited warning is logged, exactly like audio_capture's
 *  own queue-full handling - a MIDI engine must never let a slow/stuck
 *  transport stall the DSP/state-machine caller.
 *  @{ */
esp_err_t midi_send_note_on(uint8_t channel, uint8_t note, uint8_t velocity);
esp_err_t midi_send_note_off(uint8_t channel, uint8_t note, uint8_t velocity);
esp_err_t midi_send_cc(uint8_t channel, uint8_t controller, uint8_t value);
esp_err_t midi_send_pitch_bend(uint8_t channel, uint16_t value);
/** @} */

#ifdef __cplusplus
}
#endif
