[← Tutorial 6](06-freertos-realtime.md) · [Tutorials index](README.md)

# Tutorial 7: Audio synthesis and PDM output

**Code**: `components/midi/onboard_synth.c`, `main/self_test.c`,
`tools/acid_synth_monitor.py`

The rest of this pipeline turns sound *into* MIDI. This last piece runs
the other direction: turning the MIDI this project generates back
*into* sound, on the board's own speaker, so you can hear what it
decided without any other hardware. This tutorial covers the two new
ideas that requires: how a digital oscillator generates a tone in the
first place, and how PDM - the specific digital audio format this
board's speaker amplifier expects - works.

## Building a tone from nothing: the phase accumulator

[Tutorial 1](01-audio-acquisition.md) covered turning a real wave into
samples. Synthesis is the reverse problem: given a target frequency,
generate the samples of a wave yourself, one at a time.

The simplest reliable way to do this is a **phase accumulator**: a
counter that represents "where in the wave's cycle am I right now,"
which you advance by a fixed amount every sample and let wrap around:

```
phase (0..max, wraps around)
   ^
   |        /|        /|        /|
   |       / |       / |       / |
   |      /  |      /  |      /  |
   |     /   |     /   |     /   |
   +----/----+----/----+----/----+---> sample number
        ^wraps to 0 here, one full cycle = one period of the tone
```

How much to advance the phase each sample (the **phase step**) is
exactly `frequency / sample_rate` (scaled to the counter's range):
a higher target frequency means bigger steps, so the counter wraps
around (completes a cycle) more often per second - which is exactly
what a higher pitch *is*. Once you have this phase value, you can turn
it into a waveform shape however you like: a simple **square/pulse
wave** just checks whether the phase is in the first or second half of
its cycle (or, more generally, before or after some *duty cycle*
threshold) and outputs one of two fixed levels:

```c
phase += phase_step;
sample = (phase < duty_threshold) ? +AMPLITUDE : -AMPLITUDE;
```

`onboard_synth.c`'s oscillator is exactly this - deliberately the
*simplest* waveform shape that still sounds like a distinct pitch,
because (unlike a smooth sine wave) it needs no trigonometry inside the
per-sample loop, which matters on a chip with no hardware floating-point
unit (the same constraint [Tutorial 3](03-pitch-detection-yin.md)
covers for YIN). The only floating-point math here - converting a MIDI
note number to a frequency - happens once per Note On, not once per
sample; see `voice_midi.h`'s `yp_midi_note_to_frequency()`
([Tutorial 4](04-midi-basics.md)).

`onboard_synth.c` also modulates that duty-cycle threshold using the
live CC11 expression value ([Tutorial 4](04-midi-basics.md)) - shifting
where in the cycle the wave flips changes its harmonic content (its
timbre), giving a changing, "talking" character as the singer's dynamics
change, entirely without needing a filter.

## Why not a filter? (and where one *did* get built, safely)

A classic "acid" synthesizer sound (the project's `tools/` companion
tool is deliberately styled after one) comes from a **resonant lowpass
filter** swept by an envelope - a much richer, more expressive sound
than plain PWM. `tools/acid_synth_monitor.py` (a Python tool that runs
on a connected computer, not the board - see below) does exactly this.

The board's own synth deliberately does *not* attempt the same filter.
While building the Python version, a stress test simulating the exact
kind of rapid, chaotic note changes this board's own ambient-noise pitch
tracking produces made that filter's internal math run away to
astronomically large, meaningless numbers (a **numerical instability**
- explained and fixed in that file's own comments). That happened in
Python, on a laptop, with 64-bit floating point and no real-time
constraint, where it was straightforward to catch, understand, and fix
by iterating quickly. Reproducing a resonant filter safely in
fixed-point, on a chip with no hardware FPU, debuggable only by
re-flashing and listening for something going wrong - was judged not
worth the risk for what the onboard synth is fundamentally *for*:
proving MIDI generation works, audibly, before any real transport
exists. This is a real, deliberate engineering trade-off, not an
oversight - see `docs/midi.md` for the full reasoning.

## Envelopes, again - now shaping loudness over one note

[Tutorial 2](02-dsp-fundamentals.md) covered envelope followers
*measuring* an incoming signal's loudness trend. Synthesis needs the
mirror-image idea: *generating* a loudness trend for an outgoing note,
so it doesn't switch on and off like a light switch (which sounds like
an ugly digital click, since jumping straight from silence to full
volume is a very sudden, high-frequency event). `onboard_synth.c` ramps
its amplitude smoothly toward a target - up quickly on Note On (a short
**attack**), back down toward zero on Note Off (a longer **release**) -
using the same "coefficient blending toward a target" idea as
[Tutorial 2](02-dsp-fundamentals.md)'s envelope follower, just running
in the opposite direction (generating a signal, not smoothing one).

## PDM: how the raw samples become sound on this board

The samples this project generates are **PCM** (Pulse-Code Modulation) -
one number per sample, describing the instantaneous amplitude, exactly
what every earlier tutorial in this series has assumed. This board's
speaker amplifier, though, expects **PDM** (Pulse-Density Modulation): a
much simpler-to-build-in-hardware format that represents amplitude as
the *density* of pulses in a very fast 1-bit stream, rather than as a
multi-bit number:

```
Loud (near max amplitude):   1 1 1 0 1 1 1 1 0 1 1 1  (mostly 1s)
Quiet (near zero):           0 1 0 0 1 0 0 0 1 0 0 0  (few, scattered 1s)
Silence:                     0 0 0 0 0 0 0 0 0 0 0 0  (equal-ish, or none)
```

You never build this bitstream by hand: the ESP32-C5's I2S peripheral
has a hardware **PCM-to-PDM** converter built in - you configure it with
a target sample rate, feed it ordinary PCM samples exactly like every
other stage in this pipeline produces, and it outputs the correctly
pulse-density-encoded bitstream on the wire, at a much higher bit rate
than your actual sample rate (the driver's own default clock
configuration runs the PDM bitstream at 128x the PCM sample rate - see
`I2S_PDM_TX_CLK_DEFAULT_CONFIG`'s documentation in
`components/esp_driver_i2s`).

One more board-specific wrinkle, not a PDM concept in general: this
speaker amplifier expects a **differential** signal - the same
bitstream sent out on two pins, one of them electrically inverted
(`PDM_P`/`PDM_N`). Differential signaling is a common trick for noise
immunity in wiring - interference tends to affect both wires equally, so
the *difference* between them stays cleaner than either wire alone. The
ESP32-C5's I2S hardware only produces one output pin's worth of signal
directly, so `onboard_synth.c` and `main/self_test.c`'s boot melody both
use a small GPIO-matrix trick (`esp_rom_gpio_connect_out_signal(...,
out_inv=true, ...)`) to mirror that same internal signal onto the second
pin, inverted, in hardware - not something either file invented, but
copied faithfully from Espressif's own (unshipped) reference code for
this exact board, per `docs/hardware.md`'s sourcing trail.

## Buffering and latency: the DMA connection back to Tutorial 6

Getting samples from software to the PDM hardware goes through a DMA
buffer, exactly like [Tutorial 1](01-audio-acquisition.md)'s ADC
acquisition did, just in the opposite direction - and exactly as
covered in [Tutorial 6](06-freertos-realtime.md)'s case study, how
*deep* that buffer is configured directly trades off against latency:
a deeper buffer tolerates the producer task being briefly late without
an audible glitch, but every sample sitting in that buffer is a sample
that hasn't reached the speaker yet. `onboard_synth.c`'s buffer is sized
explicitly (not left at the driver's default) for roughly a 12ms
round-trip - see that tutorial and `docs/tuning.md` for the full,
measured story of getting this right.

---
**Previous:** [← Tutorial 6 - FreeRTOS and real time](06-freertos-realtime.md)

**That's the whole pipeline, mic to speaker.** [Back to the tutorials index →](README.md)
See [`docs/README.md`](../README.md) for the rest of the documentation, or
the [top-level README](../../README.md) for build/flash instructions and
project status.
