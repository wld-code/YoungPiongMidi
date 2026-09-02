# MIDI

**Status: Milestone 5 implemented (note-stabilization state machine +
Note On/Off event generation). Milestones 8, 9, 10 (real BLE/UART
transports, pitch bend) not yet implemented** - see "Transports" below
for exactly what that means today: events are generated and queued for
real, but only reach a diagnostic log line, not a wire.

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

`voice_midi`'s note-stabilization state machine (`note_state_machine.c`)
decides *when* a Note On/Off/Change should happen and computes the note
number and velocity; `main.c`'s `dsp_task` takes that decision and calls
`midi_send_note_on()`/`midi_send_note_off()` (in `components/midi`),
which builds the `midi_event_t` and pushes it onto the queue.
`voice_midi.c`/`note_state_machine.c` themselves never touch the queue,
FreeRTOS, or ESP-IDF at all - see their own header comments - so they
stay host-testable (`test/test_note_state_machine.c`). `midi.c` owns the
queue and `midi_task`, which dequeues each event and dispatches it to
whichever transport(s) are enabled (`YP_MIDI_BLE_ENABLED`,
`YP_MIDI_UART_ENABLED` in `yp_config.h` - both 0 today).

## Transports

- **Right now**: neither BLE nor UART exists, so both flags above are 0
  and `midi_task`'s only "transport" is a diagnostic `ESP_LOGI` line per
  event (`midi.c`'s `log_event()`) - e.g. `NOTE_ON  ch=0 note=69 vel=84`.
  This is not a stand-in to be embarrassed about: it is what makes Note
  On/Off generation (Milestone 5) verifiable end to end on the console
  *before* any wire protocol exists, exactly as the project spec's
  incremental-milestones approach intends.
- **BLE MIDI** (Milestone 8, default/primary, not yet implemented):
  advertises as `YP_BLE_DEVICE_NAME` ("YoungPiongMidi"). Own ESP-IDF
  component (`midi_ble.c`), so NimBLE/Bluedroid specifics never leak into
  `midi.c`. `dispatch_event()` in `midi.c` has a `#if YP_MIDI_BLE_ENABLED`
  guard already in place that intentionally fails the build (`#error`)
  if the flag is ever set before `midi_ble.c` exists, rather than silently
  compiling a no-op transport.
- **DIN MIDI over UART** (Milestone 9, optional, not yet implemented):
  standard 31250 baud (`YP_MIDI_UART_BAUD`). Not wired to a GPIO yet -
  ESP-SensairShuttle's documented pinout (see `docs/hardware.md`) does
  not reserve one, so `YP_MIDI_UART_ENABLED` stays 0 until a pin is
  explicitly confirmed, per the project spec's "do not assume a GPIO
  until it is explicitly configured" rule.
- **USB MIDI**: deliberately not planned - ESP32-C5's USB-Serial/JTAG
  peripheral is not a generic USB-OTG MIDI device.

## Note stabilization -> MIDI mapping (Milestone 5, implemented)

The note-stabilization state machine (project spec section 7) is
independent from the raw pitch detector: pitch naturally fluctuates frame
to frame, and the state machine's job is to decide when that fluctuation
should and should not become a MIDI event. Two states are actually
persisted across hops in `yp_note_sm_t`, `SILENCE` and `NOTE_ACTIVE`;
`ATTACK`, `NOTE_CHANGE` and `RELEASE` are represented as the
`yp_note_event_t` returned on the one hop each transition happens (see
`note_state_machine.c`'s header comment for the reasoning) rather than as
separately-stored states with no behavioral difference.

Mechanisms, each tied to the exact `yp_config.h` constant the spec asks
for:
- **Confidence threshold** (`YP_PITCH_CONFIDENCE_THRESHOLD`): a frame is
  only considered at all if voice is active, confidence clears this bar,
  and the frequency converts to a valid note.
- **Minimum stable frames** (`YP_NOTE_MIN_STABLE_FRAMES`): a candidate
  note (whether the very first one, or a proposed change) must repeat for
  this many consecutive reliable frames before it is committed.
- **Pitch hysteresis** (`YP_NOTE_CHANGE_TOLERANCE_ST`): compared against
  the *fractional* MIDI note number, not the rounded integer (rounding
  alone already groups anything within +/-0.5 semitones into one note,
  so an integer comparison couldn't express a tolerance narrower or wider
  than that) - this is what damps vibrato/jitter around a held pitch
  without a single spurious event, which is directly asserted on in
  `test_jitter_does_not_flicker`.
- **Minimum note duration** (`YP_NOTE_MIN_DURATION_MS`): a note change is
  withheld until the currently-held note has been active at least this
  long, even if a stable new candidate has already appeared - prevents
  rapid back-and-forth between two adjacent notes.
- **Release frames** (`YP_NOTE_RELEASE_FRAMES`): Note Off is withheld
  until pitch has been unreliable for this many *consecutive* frames, so
  a single dropped/low-confidence frame mid-note doesn't cut it off.

239 host-test checks in `test/test_note_state_machine.c` exercise all of
the above, including the exact spec wording this state machine exists to
satisfy: "a small pitch fluctuation must not generate Note Off/On/Off/On
continuously."

## Dynamics -> velocity (Milestone 6, implemented)

`yp_level_to_velocity()` (`voice_midi.c`) maps the envelope-followed
level - the vocal *attack* level, per the spec's own wording - to a MIDI
velocity using `YP_DEFAULT_VELOCITY_CURVE`:

- **linear**: velocity proportional to level, clamped between
  `YP_DYNAMICS_NOISE_FLOOR` (-> `YP_MIDI_VELOCITY_MIN`) and
  `YP_DYNAMICS_MAX_RMS` (-> `YP_MIDI_VELOCITY_MAX`).
- **log (default)**: the same clamping, but the mapping is linear in dB
  (`20*log10(level)`) rather than in level itself - quiet-to-medium level
  changes get proportionally more of the velocity range, tracking
  perceived loudness better than a raw linear mapping. Concretely (see
  `test/test_dynamics.c`'s `test_log_curve_differs_meaningfully_from_linear`):
  at a moderate level, the log curve reads more than 20 velocity units
  higher than linear - a real, measured demonstration of the spec's own
  warning against "a simple raw linear mapping" producing "poor musical
  behaviour" (with linear, ordinary singing volume would cluster into a
  narrow low-velocity band, leaving most of the 1-127 range for levels a
  singer will rarely reach).

The log curve's shape lands close to the spec's own example mapping
(soft voice ~20, normal ~70, strong ~120) at plausible operating points -
verified in `test_matches_spec_reference_mapping` with a deliberately
generous tolerance, since the spec gives these as illustrative points,
not exact RMS-to-velocity gospel.

Velocity is computed once, at the exact hop a Note On or Note Change
commits (`note_state_machine.c` calls `yp_level_to_velocity()` there),
using that hop's envelope level - by the time
`YP_NOTE_MIN_STABLE_FRAMES` (24 ms at the default hop size) has elapsed
since a candidate pitch first appeared, the envelope follower's fast
attack time constant (`YP_ENVELOPE_ATTACK_MS` = 8 ms) has already caught
up to the true attack level, so a separate peak-tracking mechanism isn't
needed for this to reflect the actual attack, not a stale/ramping value -
`test_note_on_carries_velocity_from_the_triggering_frame` checks a softer
vs. louder attack produce correspondingly different velocities.

Explicitly **not** what this milestone covers, and not a gap in it:
tracking dynamics *after* note onset, while a note is held. That is
Milestone 7's job.

## CC11 Expression (Milestone 7, not yet implemented)

Would stream continued vocal-intensity changes while a note is held as
MIDI CC11 (`YP_CC11_ENABLED`, rate-limited by `YP_CC11_MIN_DELTA`/
`YP_CC11_MIN_INTERVAL_MS` so a continuously-varying voice does not flood
the MIDI connection). `midi_send_cc()` already exists in
`components/midi/midi.h` (per the spec's transport-independent engine
design, built once in Milestone 5) but nothing calls it yet.

## Pitch bend (Milestone 10, optional)

Off by default (`YP_PITCH_BEND_ENABLED = 0`). When enabled, a quantized
MIDI note plus a smoothed pitch-bend offset (`YP_PITCH_BEND_SMOOTH_MS`,
range `YP_PITCH_BEND_RANGE_ST`) represents continuous vocal intonation
rather than snapping fully to the nearest semitone.
