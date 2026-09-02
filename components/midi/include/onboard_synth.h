/**
 * @file onboard_synth.h
 * @brief A tiny monophonic synth voice that renders this project's own
 *        MIDI events to sound on ESP-SensairShuttle's own speaker (PDM,
 *        PA_CTL=GPIO1 / PDM_P=GPIO7 / PDM_N=GPIO8) in real time.
 *
 * Why this exists: YoungPiongMidi has no wire MIDI transport yet
 * (BLE/UART are Milestones 8-9 - see docs/midi.md), so `midi_task`'s
 * only "transport" so far has been a diagnostic log line. This is a
 * second, additional transport - one that needs no laptop, no serial
 * cable, and no separate tool (unlike tools/acid_synth_monitor.py,
 * which does the same job but on the host, parsing the log over
 * serial): it lets the board play what it's generating on its own
 * speaker, live.
 *
 * Deliberately simple, and deliberately NOT the same design as the
 * Python acid synth: square/pulse oscillator (no resonant filter). A
 * resonant filter with coefficients modulated every sample went
 * numerically unstable in the Python prototype even with 64-bit
 * doubles and no CPU budget concerns (see tools/acid_synth_monitor.py's
 * tuning comments) - reproducing that safely in fixed-point on a chip
 * with no hardware FPU, debuggable only by flashing and listening,
 * was not a risk worth taking for what is fundamentally the same
 * "hear it now" goal. CC11 Expression instead modulates the
 * oscillator's pulse width (a classic, unconditionally stable PWM
 * timbre technique - no feedback loop to diverge).
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "midi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the PDM output and start the render task.
 *
 * Call once, after any temporary I2S user (self_test.c's boot melody)
 * has released I2S_NUM_0, and before midi_init() so no event can race
 * ahead of initialization - onboard_synth_handle_event() no-ops safely
 * either way if called too early, but this ordering avoids relying on
 * that.
 */
esp_err_t onboard_synth_init(void);

/**
 * @brief Render one MIDI event to the synth voice.
 *
 * Safe to call from any task (guarded internally by a spinlock - see
 * onboard_synth.c). Called synchronously from midi.c's midi_send_*()
 * functions (in the caller's own task, e.g. dsp_task), NOT from
 * midi_task's queued dispatch like log_event() and future BLE/UART
 * sends - a real latency bug (audio audibly behind the console's MIDI
 * log) was traced to that extra queue+task-switch hop being on the
 * audio path for no benefit (a spinlock-guarded assignment cannot
 * usefully be "queued" the way a possibly-blocking transport send can).
 * See midi.c's enqueue() for the full reasoning.
 */
void onboard_synth_handle_event(const midi_event_t *event);

#ifdef __cplusplus
}
#endif
