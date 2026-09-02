# MIDI

**Status: not yet implemented (Milestones 5, 8, 9, 10).** This document
describes the intended design, per the project spec, so the interface is
settled before code lands on top of it.

## Design

A transport-independent MIDI engine, so `audio_dsp`/`pitch`/`voice_midi`
never contain BLE or UART code:

```c
typedef enum {
    MIDI_EVENT_NOTE_ON,
    MIDI_EVENT_NOTE_OFF,
    MIDI_EVENT_CC,
    MIDI_EVENT_PITCH_BEND
} midi_event_type_t;

typedef struct {
    midi_event_type_t type;
    uint8_t channel;
    uint8_t data1;
    uint16_t data2;
    int64_t timestamp_us;
} midi_event_t;
```

```
Audio Task -> DSP/Voice Analysis Task -> Voice-to-MIDI State Machine
    -> [FreeRTOS queue, midi_event_t, depth YP_MIDI_QUEUE_LENGTH]
    -> MIDI Task -> Transport (BLE, optionally UART DIN)
```

`voice_midi` (the note-stabilization state machine + dynamics mapping)
produces `midi_event_t`s and pushes them onto the queue; it never calls a
transport function directly. `midi` owns the queue and dispatches to
whichever transport(s) are enabled (`YP_MIDI_BLE_ENABLED`,
`YP_MIDI_UART_ENABLED` in `yp_config.h`).

## Transports

- **BLE MIDI** (Milestone 8, default/primary): advertises as
  `YP_BLE_DEVICE_NAME` ("YoungPiongMidi"). Own ESP-IDF component
  (`midi_ble.c`), so NimBLE/Bluedroid specifics never leak into `midi.c`.
- **DIN MIDI over UART** (Milestone 9, optional): standard 31250 baud
  (`YP_MIDI_UART_BAUD`). Not wired to a GPIO yet - ESP-SensairShuttle's
  documented pinout (see `docs/hardware.md`) does not reserve one, so
  `YP_MIDI_UART_ENABLED` stays 0 until a pin is explicitly confirmed,
  per the project spec's "do not assume a GPIO until it is explicitly
  configured" rule.
- **USB MIDI**: deliberately not planned - ESP32-C5's USB-Serial/JTAG
  peripheral is not a generic USB-OTG MIDI device.

## Note stabilization -> MIDI mapping (Milestone 5+)

The note-stabilization state machine (SILENCE / ATTACK / NOTE_ACTIVE /
NOTE_CHANGE / RELEASE, project spec section 7) is independent from the raw
pitch detector: pitch naturally fluctuates frame to frame, and the state
machine's job is to decide when that fluctuation should and should not
become a MIDI event, using `YP_NOTE_MIN_STABLE_FRAMES`,
`YP_NOTE_MIN_DURATION_MS`, `YP_NOTE_CHANGE_TOLERANCE_ST` and
`YP_NOTE_RELEASE_FRAMES` from `yp_config.h`.

## Dynamics -> velocity / CC11 (Milestone 6-7)

Velocity is derived from the envelope (`voice_analysis_t.level`) at note
onset, using `YP_DEFAULT_VELOCITY_CURVE` (linear or logarithmic - log by
default, since perceived loudness is closer to logarithmic than linear in
RMS). Once a note is active, continued envelope changes are optionally
streamed as CC11 Expression (`YP_CC11_ENABLED`), rate-limited by both a
minimum value delta (`YP_CC11_MIN_DELTA`) and a minimum time interval
(`YP_CC11_MIN_INTERVAL_MS`) so a continuously-varying voice does not flood
the MIDI connection.

## Pitch bend (Milestone 10, optional)

Off by default (`YP_PITCH_BEND_ENABLED = 0`). When enabled, a quantized
MIDI note plus a smoothed pitch-bend offset (`YP_PITCH_BEND_SMOOTH_MS`,
range `YP_PITCH_BEND_RANGE_ST`) represents continuous vocal intonation
rather than snapping fully to the nearest semitone.
