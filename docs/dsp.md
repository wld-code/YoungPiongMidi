# DSP pipeline

## Current pipeline (implemented)

```
ADC raw sample (12-bit)
    |  audio_capture.c: condition_sample()
    v
DC removal (slow exponential tracker, alpha=0.001)
    v
High-pass filter (one-pole IIR, YP_AUDIO_HPF_CUTOFF_HZ = 60 Hz)
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
Voice-activity detection (debounced: 2 frames to trigger,
                           4 frames to release)                    [audio_dsp.c]
    v
voice_analysis_t { frequency_hz=0, confidence=0, rms, level, voice_active }
```

`frequency_hz` and `confidence` are always 0 today; they exist in the
struct now so integrating the pitch detector (Milestone 3) does not change
`voice_analysis_t`'s shape, only who fills in those two fields.

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
`YP_VAD_RELEASE_FRAMES` = 4) against a single RMS threshold
(`YP_VAD_RMS_THRESHOLD` = 0.02) prevent single-frame noise spikes or
dropouts from toggling `voice_active`. This is intentionally simpler than
the full note-stabilization state machine described in the project spec's
section 7 (SILENCE/ATTACK/NOTE_ACTIVE/NOTE_CHANGE/RELEASE) - that state
machine belongs to Milestone 5+ (`voice_midi`, not yet implemented) and
operates on *note* stability, not just voice-present/absent.

## Pitch detection (Milestone 3, not yet implemented)

Per the project spec: YIN (or a carefully normalized autocorrelation
method) over FFT peak-picking, because FFT bin resolution at
`YP_AUDIO_SAMPLE_RATE_HZ` / `YP_AUDIO_FRAME_SIZE` is too coarse to resolve
cents-level pitch across the 80-1000 Hz voice range without heavy
interpolation, whereas YIN's parabolic-interpolated minimum of the
cumulative mean normalized difference function is designed for exactly
this. It will live in `components/pitch` as its own component with a
narrow interface (samples in, `{frequency_hz, confidence}` out) specifically
so it can be swapped for a different algorithm later without touching
`audio_dsp` or anything downstream.

## Host-side testing

Not yet set up (see `test/` - empty). Planned, per the project spec's
section 17: synthetic sine inputs (440 Hz -> A4/69, 261.63 Hz -> C4/60,
329.63 Hz -> E4/64, plus noisy variants) exercising
frequency<->MIDI-note conversion, RMS, the envelope follower, and (once
implemented) YIN and note stabilization, all without hardware. `rms.c` and
`envelope.c` are already written as pure functions with no ESP-IDF
dependency for exactly this reason, so this is a matter of adding a host
build target, not restructuring the DSP code.
