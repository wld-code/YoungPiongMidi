[← Docs index](README.md) · New here? Read [tutorials 4-6](tutorials/) first for the concepts behind this document.

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

- **Right now**: neither BLE nor UART exists, so both flags above are 0.
  `midi_task` always does one real thing with every event regardless:
  logs it (`midi.c`'s `log_event()` - e.g. `NOTE_ON  ch=0 note=69
  vel=84`) - this, not the board's own speaker, is what every PC-side
  tool (`tools/acid_synth_monitor.py`, `tools/groovebox.py`) actually
  reads, and it is unconditional, unaffected by anything below.
- **onboard_synth** (not a spec milestone - a bring-up/verification aid,
  **disabled by default** - `YP_ONBOARD_SYNTH_ENABLED` in `yp_config.h`
  is 0): a tiny monophonic square/PWM voice - fixed-point phase
  accumulator oscillator, Q15 linear amplitude envelope, CC11 Expression
  modulating pulse width (10%..90% duty) - played over the board's
  existing PDM speaker output (the same PA_CTL/PDM_P/PDM_N path as the
  boot self-test's melody in `main/self_test.c`, initialized after that
  temporary use tears down its I2S channel). Deliberately no resonant
  filter: `tools/acid_synth_monitor.py`'s Python prototype of exactly
  that went numerically unstable under rapid note changes even with
  64-bit floats and no CPU budget pressure (see that file's tuning
  comments) - reproducing it safely in fixed-point on a chip with no
  hardware FPU, debuggable only by flashing and listening, wasn't a risk
  worth taking for what it and the Python tools are fundamentally for:
  hearing MIDI output before Milestone 8/9 exist. Because the board's
  microphone and speaker are physically close together, playing loud
  enough for the mic to pick the output back up can create an audio
  feedback loop (the synth's own note re-triggering a new detection) -
  this, on top of most users preferring one clean output (the PC-side
  Young Piong Groovebox, with its 10 instruments and sequencer) over
  a second, simpler one on the board itself, is why it now defaults off.
  Flip `YP_ONBOARD_SYNTH_ENABLED` to 1 to get board-native sound back;
  when 0, `onboard_synth_init()` is never called at all (in `main.c`) and
  `onboard_synth_handle_event()` is never called either (in `midi.c`'s
  `enqueue()`) - the PDM/I2S peripheral is never configured, not just
  muted, so there's no PDM clock activity on GPIO1/7/8 either. The boot
  self-test's own speaker melody (`main/self_test.c`) is independently
  gated by `YP_SELF_TEST_SPEAKER_ENABLED` (also 0 by default) for the
  same reason - the LCD half of that same self-test is silent and still
  runs unconditionally.

  When enabled, driven *synchronously* from `midi_send_*()` (in
  `midi.c`'s `enqueue()`), not from `midi_task`'s queued dispatch the way
  `log_event()` and future BLE/UART sends are - and its I2S DMA sizing is
  explicitly overridden to a ~12ms budget rather than the driver's
  ~90ms-implying default. Both were real, measured, user-reported
  latency bugs, not design choices made up front - see docs/tuning.md
  ("Onboard synth: audio audibly lagged the MIDI log by ~90ms") for the
  full story and the numbers behind them.
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

## CC11 Expression (Milestone 7, implemented)

Streams continued vocal-intensity changes while a note is held as MIDI
CC11 (`YP_CC11_ENABLED`). New `components/voice_midi/expression.c`:
`yp_expression_t` + `yp_expression_process()`, called from `dsp_task`
(`main.c`) once per hop whenever `s_note_sm.state == YP_NOTE_STATE_NOTE_ACTIVE`
- outside an active note there is nothing to attach continued dynamics
to, per the spec's own "once a note is active" wording. Uses
`yp_level_to_cc_value()` (`voice_midi.c`), the same clamp/curve logic as
`yp_level_to_velocity()` but scaled to the CC value's full 0..127 range
(0 is not reserved for CC the way it is for Note On velocity).

**Throttling - the one genuinely ambiguous piece of spec wording in this
project**: "Only transmit CC11 when the value changes sufficiently or
after a configurable minimum interval." Read literally, that "or" could
mean either condition alone is enough to send. This implementation
requires **both**: the candidate value must differ from the last *sent*
value by at least `YP_CC11_MIN_DELTA`, *and* at least
`YP_CC11_MIN_INTERVAL_MS` must have elapsed since the last send. Chosen
deliberately, not by default: an "or" of two independent triggers isn't
actually a rate *cap* - a large, fast-changing delta would still fire
every single hop, which is exactly the flooding the spec says to avoid.
Requiring both is what bounds the message rate to at most one per
`YP_CC11_MIN_INTERVAL_MS` while still filtering out sub-threshold noise.
The first CC after a note turns on (`yp_expression_init()`, called from
`main.c` on `YP_NOTE_EVENT_NOTE_ON`) always sends immediately regardless
of both gates, to establish a starting value for the note rather than
inheriting whatever the previous note left the channel's expression at.
`test/test_expression.c`'s `test_large_delta_before_interval_is_withheld`
and `test_small_delta_alone_does_not_send` check each gate independently,
so this choice is an asserted behavior, not just a comment.

Deliberately **not** reset on a `NOTE_CHANGE` (only on a fresh `NOTE_ON`
from silence) - CC11 tracks the *channel's* continued dynamics across a
sung phrase, not any one note's, so a pitch change mid-phrase shouldn't
discard an expression trend the singer was already building.

Verified end to end on real hardware, not just in host tests: with the
board picking up ambient sound, the console shows real sequences like
`NOTE_ON ch=0 note=57 vel=83` followed by several `CC ch=0 cc=11 val=...`
messages roughly `YP_CC11_MIN_INTERVAL_MS` apart tracking the level up
and down, then `NOTE_OFF` - see docs/tuning.md.

## Pitch bend (Milestone 10, optional)

Off by default (`YP_PITCH_BEND_ENABLED = 0`). When enabled, a quantized
MIDI note plus a smoothed pitch-bend offset (`YP_PITCH_BEND_SMOOTH_MS`,
range `YP_PITCH_BEND_RANGE_ST`) represents continuous vocal intonation
rather than snapping fully to the nearest semitone.
