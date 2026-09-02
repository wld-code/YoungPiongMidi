# Architecture

## Signal chain (target, end state)

```
Microphone
    |
ADC acquisition (continuous mode + DMA)
    |
Audio preprocessing (DC removal, HPF, LPF)
    |
Voice activity detection
    |
Pitch detection
    |
Amplitude / envelope detection
    |
MIDI note generation
    |
MIDI output (BLE, optionally DIN UART)
```

## Component map

```
components/
  board/         Pin definitions (yp_board.h) + central config (yp_config.h).
                 No DSP, no protocol code - just "what pin is what" and
                 "what are the tunable numbers".
  audio_capture/ Owns the ESP-IDF ADC continuous-mode driver. Produces
                 audio_block_t (conditioned, normalized float samples) on
                 a queue. DC removal + a fixed-cutoff HPF happen here,
                 because they are acquisition-layer concerns per the
                 project spec, and because doing them once, close to the
                 ADC, is simpler than repeating them per downstream
                 consumer.
  audio_dsp/     Per-hop analysis: optional LPF -> RMS -> envelope ->
                 voice-activity detection -> pitch (calls into `pitch`).
                 Produces voice_analysis_t. Does not know about MIDI or
                 the display.
  pitch/         YIN fundamental-frequency detector (Milestone 3, done).
                 Fixed-point (not float - see docs/dsp.md/tuning.md for
                 why) for its hot inner loop; narrow samples-in/
                 {frequency_hz,confidence}-out interface so it stays
                 swappable for a different algorithm later.
  voice_midi/    Frequency<->MIDI note conversion (Milestone 4, done -
                 see voice_midi.c/.h: pure math, no ESP-IDF dependency,
                 host-tested in test/test_midi_notes.c). Note
                 stabilization state machine + dynamics->MIDI mapping
                 (Milestones 5-10) not yet implemented - will land in
                 note_state_machine.c alongside this file.
  midi/          Not yet implemented (Milestones 8-9). Transport-
                 independent MIDI event queue + BLE/UART backends.
  display/       Direct-SPI ST7789P3 driver + tiny bitmap-font UI
                 primitives. Deliberately not a full LVGL/esp_lcd_panel
                 stack - the UI need (section 13 of the spec) is a
                 handful of text fields and a level meter at 10-20 Hz,
                 not a general-purpose GUI.
main/            Task wiring: creates dsp_task and ui_task, starts
                 acquisition, owns the mutex-guarded "latest analysis"
                 hand-off between them.
```

`components/midi` exists as a directory (matching the repository layout
the project spec asks for) but currently contains only a README pointing
back here - see "Roadmap" below; nothing in `main/` references it yet.
`components/pitch` and `components/voice_midi` are implemented (see
above) - `voice_midi` currently holds only frequency<->MIDI conversion
(voice_midi.c); its future note_state_machine.c (Milestones 5+) does not
exist yet.

## FreeRTOS architecture

| Task | Owned by | Priority | Stack | Wakes on |
|---|---|---|---|---|
| `audio_capture` | `audio_capture` component | 12 (highest) | 8192 B | ADC continuous-mode `on_conv_done` ISR notification |
| `dsp_task` | `main.c` | 11 | 4096 B | `audio_capture_get_block()` (queue receive, 500 ms timeout) |
| `ui_task` | `main.c` | 5 | 3072 B | Fixed period, `YP_UI_REFRESH_RATE_HZ` (15 Hz default) |
| `main_task` (IDF-owned) | ESP-IDF | default | default | runs `app_main()` once, then exits |

Rationale:

- **audio_capture is highest priority** because it is the only task with a
  hard real-time deadline (drain the ADC continuous-mode driver's internal
  pool before it overflows). Its ISR callback does nothing but a task
  notify - all actual work (raw-to-float conversion, DC removal, HPF,
  clip detection) happens in the task, per the project spec's "interrupts
  only move or signal data" rule.
- **dsp_task is next** so RMS/envelope/VAD analysis is not delayed behind
  UI work, but can still be preempted by fresh audio data.
- **ui_task is low priority and rate-limited independently of the DSP
  loop**, per the project spec's "the DSP loop must never wait for the
  display" requirement. It communicates with dsp_task only through a
  mutex-guarded snapshot struct in `main.c`, never through direct calls.

`CAPTURE_TASK_STACK_BYTES` (8192) looks generous for what the task does,
and was arrived at empirically, not guessed: an earlier value of 3072 B
produced a real stack-protection fault on hardware, because the local
`adc_continuous_data_t parsed[YP_AUDIO_DMA_FRAME_SAMPLES]` scratch array
alone is close to 2 KB at the default hop size, plus `ESP_LOGx`'s own
formatting stack use. See `docs/hardware.md` and the comment above the
macro in `audio_capture.c`.

## Threading / data-hand-off model

```
[ADC ISR] --notify--> [audio_capture task] --xQueueSend(audio_block_t)-->
    [dsp_task] --mutex-guarded snapshot--> [ui_task]
```

No component reaches into another's internals; everything crosses a task
boundary through a typed queue or a mutex-guarded struct. There is exactly
one producer and one consumer for each hand-off, so no priority-inversion-
prone multi-writer state exists today.

## Why display is a direct SPI driver, not esp_lcd_panel/LVGL

Early text rendering used one `display_fill_rect()` (and therefore one
SPI command+data sequence) per glyph pixel cell. That is correct but far
too slow: redrawing two short numeric fields 15 times a second could
enqueue thousands of tiny SPI transactions, and because a busy task never
naturally yields to the idle task, this measurably starved the FreeRTOS
idle task and triggered real task-watchdog resets on hardware (see the
"clean build, but watchdog resets on first hardware run" note in git
history / docs/tuning.md). The fix - render each text line into a small
line buffer first, then issue one SPI window write for the whole line -
cut the SPI transaction count by roughly two orders of magnitude and
resolved it. This is exactly the kind of "verify on the board rather than
assume" failure mode the project spec warns about, kept here as the
documented reason for the current design rather than silently fixed away.

## Roadmap (implementation order)

Matches the project spec's Section 22. Status as of this document:

| # | Milestone | Status |
|---|---|---|
| 1 | Continuous mic acquisition + basic signal display | Done, verified on hardware |
| 2 | RMS/envelope + voice activity detection | Done, verified on hardware |
| 3 | Fundamental frequency detection (YIN) | Done, verified on hardware |
| 4 | Frequency -> MIDI note conversion | Done, host-tested + verified on hardware |
| 5 | MIDI Note On/Off generation | Not started |
| 6 | Vocal dynamics -> MIDI velocity | Not started |
| 7 | Continuous CC11 Expression | Not started |
| 8 | BLE MIDI | Not started |
| 9 | DIN MIDI over UART (optional) | Not started |
| 10 | Pitch bend for continuous vocal pitch | Not started |

See `README.md` for the user-facing status summary.
