[← Tutorial 3](03-pitch-detection-yin.md) · [Tutorials index](README.md)

# Tutorial 4: MIDI basics

**Code**: `components/voice_midi/voice_midi.c`, `components/midi/midi.h`

MIDI (Musical Instrument Digital Interface) is a decades-old, simple
protocol for describing musical *events* - not sound itself, but
instructions like "start playing this note, this loud" that a
synthesizer, DAW, or another instrument can turn into sound however it
likes. This tutorial covers the pieces YoungPiongMidi actually uses.

## MIDI note numbers and equal temperament

MIDI represents a pitch as an integer 0-127, not a frequency in Hz.
Converting between the two requires understanding **equal temperament**,
the tuning system almost all modern instruments use: each **octave**
(a doubling of frequency) is divided into 12 equal steps, called
**semitones**, where each semitone is a fixed *ratio* apart (not a fixed
Hz difference) - specifically, the 12th root of 2 (~1.0595) times the
previous note's frequency.

MIDI note 69 is defined as A4 = 440 Hz (concert pitch), and every other
note is a whole number of semitones away from it:

```
midi_note = 69 + 12 * log2(frequency_hz / 440)
frequency_hz = 440 * 2^((midi_note - 69) / 12)
```

`yp_frequency_to_midi_float()` and `yp_midi_note_to_frequency()` in
`voice_midi.c` are exactly these two formulas. Because a detected
frequency is essentially never *exactly* on a note (see the next
section), `yp_frequency_to_midi_note()` rounds to the nearest integer -
`lroundf()` on the fractional value above.

Octave and note name follow from the MIDI number by simple arithmetic:
`note_name = names[midi_note % 12]` (the 12 semitone names repeat every
octave) and `octave = midi_note / 12 - 1` (MIDI note 60, "middle C", is
defined as C4 by convention - hence the `-1`).

## Cents: how far off, in a musically meaningful unit

A semitone is further divided into 100 **cents** - a small enough unit
that humans can (with training) hear differences of a few cents, but a
useful frequency generally isn't landing exactly on a cent boundary
either. `yp_frequency_to_note_info()` reports how far the *actual*
detected frequency sits from its nearest note's *exact* equal-tempered
frequency:

```
cents = 1200 * log2(actual_frequency / exact_frequency_of_nearest_note)
```

(1200 because there are 12 semitones per octave x 100 cents per
semitone.) A singer landing exactly on pitch reads close to 0 cents;
consistently sharp or flat singing reads a consistent positive or
negative offset. This is purely informational in the current firmware
(shown on the LCD and in the debug log) - nothing acts on it yet, though
it's the natural building block for pitch bend
(`YP_PITCH_BEND_ENABLED`, project spec Milestone 10, not yet
implemented).

## Velocity: how hard was the note struck

MIDI's velocity field (1-127 for Note On) represents *how hard a key was
struck* on a real keyboard - louder/harder = higher velocity. For a
voice, the natural equivalent is: how loud was the singer at the moment
the note started (its **attack**)?

The obvious mapping - `velocity = level * 127` - has a real problem:
human hearing perceives loudness roughly *logarithmically*, not
linearly (this is also why audio volume controls and decibels exist as
concepts at all). A linear mapping spends most of its numeric range on
levels a voice rarely reaches, cramming all "normal" singing into a
narrow low-velocity band:

```
level (linear, 0..1)      velocity if linear      velocity if log-mapped
  0.02  (soft)                    5                        25
  0.10  (normal)                 23                         67
  0.45  (strong)                104                        120
```

`yp_level_to_velocity()` in `voice_midi.c` therefore maps in the
**decibel** domain (`20 * log10(level)`) rather than directly in level -
a logarithmic curve, which is what "measuring loudness in dB" already
*is*. See `docs/midi.md`/`test/test_dynamics.c` for the measured
comparison and how closely this lands to the project spec's own example
figures (soft/normal/strong voice -> velocity ~20/~70/~120).

## CC11: dynamics *after* the note starts

Velocity only captures the *onset*. A note held for a second or two can
get louder or softer while it's sustained - a singer's vibrato, a
crescendo - and that's musically important too. MIDI has **Continuous
Controllers** (CC) for exactly this: an independent 0-127 value stream,
tagged with a controller number, that a synth can map to something like
volume. **CC11 ("Expression")** is the conventional choice for "how loud
is this sustained note right now."

Unlike velocity (one value, once, at note-on), CC11 could in principle
be sent on every single analysis hop (every 8ms) - but a MIDI connection
has finite bandwidth, and most of those updates would carry almost no
new information. `components/voice_midi/expression.c` throttles this:
a new CC11 message is only sent once the value has changed by at least
`YP_CC11_MIN_DELTA` *and* at least `YP_CC11_MIN_INTERVAL_MS` has passed
since the last one - see [Tutorial 5](05-note-stabilization.md) and
`docs/midi.md` for why *both* conditions (not either alone) are needed
to actually bound the message rate.

## The event model this project uses

Rather than the raw wire-protocol byte sequences MIDI hardware actually
sends, this project represents each MIDI action as a small, explicit
struct (`components/midi/midi.h`):

```c
typedef struct {
    midi_event_type_t type;  // NOTE_ON, NOTE_OFF, CC, or PITCH_BEND
    uint8_t  channel;
    uint8_t  data1;   // note number, or CC controller number
    uint16_t data2;   // velocity/CC value, or 14-bit pitch bend
    int64_t  timestamp_us;
} midi_event_t;
```

This is deliberately **transport-independent**: nothing that decides
*what* MIDI to generate (the pitch detector, the note state machine)
knows or cares whether that event ends up on the console log, a speaker,
BLE, or a MIDI cable - it just builds one of these structs. Whatever
eventually receives it decides what to do. See
[Tutorial 6](06-freertos-realtime.md) for how that hand-off actually
happens, and why it isn't quite as simple as "just call a function."

---
**Previous:** [← Tutorial 3 - Pitch detection with YIN](03-pitch-detection-yin.md)
**Next:** [Tutorial 5 - Note stabilization →](05-note-stabilization.md)
