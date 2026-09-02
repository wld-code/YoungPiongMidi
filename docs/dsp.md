[← Docs index](README.md) · New here? Read [tutorials 1-3](tutorials/) first for the concepts behind this document.

# DSP pipeline

## Current pipeline (implemented)

```
ADC raw sample (12-bit)
    |  audio_capture.c: condition_sample()
    v
DC removal (slow exponential tracker, alpha=0.001)
    v
High-pass filter (2nd-order biquad IIR, YP_AUDIO_HPF_CUTOFF_HZ = 60 Hz)
    v
Normalize (/2048, clamp to [-1, 1])
    |  -> audio_block_t, YP_AUDIO_HOP_SIZE (128) samples, over a queue
    v
audio_dsp.c: audio_dsp_process_block()
    |
Low-pass filter (one-pole IIR, YP_AUDIO_LPF_CUTOFF_HZ = 2000 Hz)   [filters.c]
    v
RMS over the hop                                                   [rms.c]
    v
Envelope follower (attack 8 ms / release 120 ms)                   [envelope.c]
    v
Adaptive noise gate threshold (tracks ambient floor)             [noise_gate.c]
    v
Voice-activity detection (debounced: 2 frames to trigger,
                           4 frames to release)                    [audio_dsp.c]
    v
YIN pitch detection (decimated - see "Pitch detection" below)        [pitch/]
    v
voice_analysis_t { frequency_hz, confidence, rms, level, voice_active }
```

All fields of `voice_analysis_t` are live today - pitch detection
(Milestone 3), the note-stabilization state machine, dynamics mapping,
and CC11 expression (Milestones 4-7) are all implemented; see
`docs/architecture.md`'s roadmap table for current status.

## Why DC removal and the HPF are split into two stages

A single filter could arguably do both jobs. They are kept separate
because they solve different problems:

- The **DC tracker** follows the bias point itself (which can drift with
  temperature, supply voltage, or simply differ from the assumed 2048
  mid-scale code), on a timescale far slower than any audio content
  (alpha = 0.001 at 16 kHz => a ~1 second time constant).
- The **HPF** removes genuine low-frequency content (handling noise,
  breath rumble) at a musically meaningful, configurable cutoff
  (`YP_AUDIO_HPF_CUTOFF_HZ`), independent of how well the DC tracker is
  converged at any given moment.

## Why the LPF lives in audio_dsp, not audio_capture

The project spec's acquisition-layer checklist (section 4) lists DC
removal and high-pass filtering; its DSP-pipeline checklist (section 5)
additionally lists an *optional* low-pass filter ahead of RMS/pitch
analysis. Splitting it this way keeps `audio_capture` responsible only for
turning ADC codes into clean, normalized samples - a low-pass filter is a
pitch/RMS analysis concern (reduce breath/sibilance energy that would
otherwise show up as spurious zero-crossings), so it lives next to the
code that consumes it.

## Envelope follower

Applied to per-hop RMS (not to raw rectified samples), producing a value
one order of magnitude smoother than frame-to-frame RMS jitter. This
`level` signal is:

- What voice-activity detection thresholds against.
- What the LCD level meter displays.
- What Milestones 6-7 (velocity / CC11 expression) will map to MIDI values.

Attack/release are configured as times-to-~63%-of-a-step
(`YP_ENVELOPE_ATTACK_MS` / `YP_ENVELOPE_RELEASE_MS`), converted internally
to a per-frame exponential coefficient using the actual hop period
(`YP_AUDIO_HOP_SIZE / YP_AUDIO_SAMPLE_RATE_HZ`), so changing the hop size
in `yp_config.h` does not silently change the envelope's real-world time
constants.

## Voice activity detection

Two independent debounce counters (`YP_VAD_ATTACK_FRAMES` = 2,
`YP_VAD_RELEASE_FRAMES` = 4) require several consecutive frames above or
below threshold before flipping `voice_active`, preventing single-frame
noise spikes or dropouts from toggling it. The threshold itself is *not*
a fixed number - see "Adaptive noise gate" below, added after real
hardware showed a fixed threshold let ambient noise and mic-handling
noise through. This VAD stage is intentionally simpler than the full
note-stabilization state machine in `components/voice_midi` (see
`docs/midi.md`) - that operates on *note* stability once a pitch exists;
this just decides whether there's a signal worth analyzing at all.

## Adaptive noise gate

`components/audio_dsp/noise_gate.c` replaces what was originally a fixed
VAD threshold with one that tracks the ambient noise floor continuously
and adapts to it - the same idea as a studio noise gate or a radio
squelch. A slow follower (`YP_NOISE_GATE_DOWN_TIME_MS` = 1000ms to adapt
toward quiet, `YP_NOISE_GATE_UP_TIME_MS` = 30000ms to adapt toward
louder) tracks `floor`; the actual gate threshold used by VAD is
`floor * YP_NOISE_GATE_MARGIN_RATIO` (2.5x, clamped to at least
`YP_NOISE_GATE_MIN_FLOOR`).

The one property that makes this work rather than just being a slower
fixed threshold: the floor only adapts *upward* while the gate is
*closed* (`noise_gate_process()`'s `gate_was_open` parameter). A loud,
sustained, real voice keeps the gate open and therefore can never drag
its own threshold up underneath itself - but a persistent noise source
the gate never trusted (handling noise, hum) slowly raises the bar until
it stops registering as signal at all. This asymmetry is directly tested
in `test/test_noise_gate.c`
(`test_loud_open_gate_does_not_raise_floor`,
`test_closed_gate_noise_eventually_raises_floor`) and was necessary on
real hardware, not theoretical - see docs/tuning.md for the measured
before/after.

## Pitch detection (Milestone 3, implemented)

YIN (de Cheveigne & Kawahara, 2002) over FFT peak-picking, per the project
spec: FFT bin resolution at `YP_AUDIO_SAMPLE_RATE_HZ` / `YP_AUDIO_FRAME_SIZE`
is too coarse to resolve cents-level pitch across the 80-1000 Hz voice
range without heavy interpolation, whereas YIN's parabolic-interpolated
minimum of the cumulative mean normalized difference function (CMNDF) is
designed for exactly this. Lives in `components/pitch` (`yin.c`) behind
the narrow `pitch_detector_process(samples, count) -> {frequency_hz,
confidence}` interface, independent of `audio_dsp` and everything
downstream, so a different algorithm could replace it without touching
either.

**No hardware FPU, measured the hard way.** ESP32-C5 builds with
`-march=rv32imac` - no `f` (single-precision float hardware) extension,
unlike ESP32-H4/P4 (`rv32imafc`). The first, all-`float` implementation of
YIN's O(window x tau_range) difference function measured **~79 ms per
call** on real hardware against an 8 ms hop budget - every float multiply
in that loop was a soft-float library call, not a CPU instruction.
Rewritten with the difference function's hot inner loop in int16 (Q14
fixed-point) accumulated in `int64_t` - the native, hardware-multiplier
integer path - this dropped to **~13 ms average / ~17 ms worst case**:
about 6x faster, but still over an 8 ms hop budget on its own. Only the
per-tau normalization/threshold-search/interpolation stages (a couple
hundred float ops per call, not window x tau_range) were left as float,
where soft-float's cost is negligible. See `yin.c`'s header comment for
the full reasoning and docs/tuning.md for the numbers as measured at each
step.

**Decimated, not just optimized.** Even at ~13 ms, running full YIN every
8 ms hop leaves no headroom once RMS/envelope/VAD/UI/capture are also
competing for the CPU - confirmed on hardware by task-watchdog resets and
`audio_capture` queue overflows before this was added. `yin.c` now slides
its analysis window every hop (no audio is ever skipped), but only runs
the expensive CMNDF computation once every `YP_PITCH_UPDATE_STRIDE_HOPS`
(default 3) hops, returning the last computed estimate on the hops in
between. This is a legitimate, common technique (most pitch trackers
update well below their sample rate) rather than a compromise: even fast
vocal vibrato is ~5-8 Hz, far below the ~40 Hz this still gives. Measured
result: `dsp_task`'s average per-hop time dropped to ~4.7 ms (worst case
~13 ms, absorbed by the capture queue's depth), with zero watchdog resets
or dropped blocks over sustained runs.

## Host-side testing

Implemented - see `test/` and `test/README.md`. `make -C test test` builds
and runs, with a plain host C compiler (no ESP-IDF, no board): 211 checks
across 4 suites as of this writing, all passing. Covers frequency<->MIDI-
note conversion (including the spec's own reference frequencies - 440 Hz
-> A4/69, 261.63 Hz -> C4/60, 329.63 Hz -> E4/64), RMS, the envelope
follower, and YIN with both clean and noisy synthetic sine inputs
(project spec section 17). Note stabilization (Milestone 5, not yet
implemented) will get its own suite once it exists.

This exists because `rms.c`, `envelope.c`, `yin.c` and `voice_midi.c` are
all plain, portable C with no ESP-IDF dependency, on purpose (yin.c's
fixed-point math is standard `int16_t`/`int32_t`/`int64_t`, nothing
RISC-V- or ESP-IDF-specific) - keeping the DSP/conversion code free of
`esp_log.h`/FreeRTOS/etc is what made "add a host build target" the whole
job, rather than "restructure the DSP code first."

One of these tests (`test_pitch.c`'s `test_pure_tones_match_spec_examples`)
is a genuine end-to-end check spanning two components: it synthesizes a
sine wave, runs it through YIN, then feeds YIN's *output* frequency into
`yp_frequency_to_midi_note()` and asserts the resulting MIDI note matches
- catching a bug that a unit test confined to either component alone
would miss (e.g. YIN reporting a frequency a few Hz off that still
happens to round to the wrong MIDI note).
