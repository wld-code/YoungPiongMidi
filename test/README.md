[← Project README](../README.md) · [Docs index](../docs/README.md)

# Host-side tests

Tests for this project's hardware-independent DSP/conversion/state-machine
code (frequency<->MIDI note conversion, RMS, the envelope follower, YIN
pitch detection, the note-stabilization state machine, dynamics->velocity
mapping, CC11 expression throttling), per the project spec's section 17:
"DSP algorithms should be testable without the ESP32 when possible."
These build and run on your Mac/Linux machine with a plain C compiler -
no ESP-IDF, no board, no simulator.

## Running

```sh
cd test
make            # builds everything, runs every suite, fails loudly on any failure
```

Or build/run one suite at a time:

```sh
make build/test_pitch && ./build/test_pitch
```

## What's covered

| File | Exercises | Notable cases |
|---|---|---|
| `test_midi_notes.c` | `components/voice_midi` (frequency<->MIDI conversion) | the spec's own reference frequencies (440/261.63/329.63 Hz), cents deviation, rounding at note boundaries, a full round-trip over all 128 MIDI notes, clamping, and invalid input (0, negative, NaN, inf) |
| `test_rms.c` | `components/audio_dsp/rms.c` | silence, DC, a full-scale sine wave against its analytic RMS (amplitude/sqrt(2)), zero-length input |
| `test_envelope.c` | `components/audio_dsp/envelope.c` | attack faster than release, monotonic step response, zero-time-constant = instantaneous tracking |
| `test_pitch.c` | `components/pitch/yin.c` (+ an end-to-end check through `voice_midi`) | the spec's reference tones recovered by frequency *and* by the MIDI note they convert to, near the configured pitch-range boundaries, a noisy tone (robustness), silence (no garbage output), and that no estimate is produced before the analysis window has filled |
| `test_note_state_machine.c` | `components/voice_midi/note_state_machine.c` | the spec's own anti-flicker requirement (60 frames of +/-15-cent jitter around a held note -> zero events), Note On only after `YP_NOTE_MIN_STABLE_FRAMES`, Note Off only after `YP_NOTE_RELEASE_FRAMES` (and *not* on a shorter dropout that recovers), a note change withheld until `YP_NOTE_MIN_DURATION_MS` has elapsed, low confidence never triggering anything, and Note On velocity actually reflecting the triggering hop's level |
| `test_dynamics.c` | `yp_level_to_velocity` in `components/voice_midi/voice_midi.c` | bounds/clamping/monotonicity for both curves, the log curve landing close to the spec's own example mapping (soft/normal/strong voice -> ~20/~70/~120), and a measured demonstration that the log curve reads meaningfully higher than linear at ordinary levels (the spec's explicit warning against "poor musical behaviour" from a simple linear mapping) |
| `test_expression.c` | `components/voice_midi/expression.c` (`yp_expression_process`, CC11 throttling) | the first call after init always sends (baseline); an unchanged level never resends; a sub-`YP_CC11_MIN_DELTA` change never sends regardless of elapsed time (delta gate, tested independently); a large delta is still withheld until `YP_CC11_MIN_INTERVAL_MS` has passed (interval gate, tested independently); sends exactly once both gates pass; louder -> higher CC11, softer -> lower (the spec's own example); `yp_level_to_cc_value`'s full 0..127 range and clamping |

`test_common.h` is a ~60-line assert-style framework (not a dependency on
Unity/CMock/etc - the code under test is a handful of pure functions, and
pulling in a full framework's own build system for that would be more
machinery than the problem needs). Each `test_*.c` builds to its own
standalone executable; `make test` runs all of them and reports a single
pass/fail.

## Why these functions specifically are host-testable

`rms.c`, `envelope.c`, `yin.c`, `voice_midi.c` (which includes
`yp_level_to_velocity`/`yp_level_to_cc_value`), `note_state_machine.c`
and `expression.c` have zero ESP-IDF dependency by design - they only ever include
`yp_config.h` (plain macros) and the C standard library (`<math.h>`,
`<string.h>`, ...). That is not an accident of these particular files; it
is why they were kept free of `esp_log.h`/FreeRTOS/etc in the first
place - notably, `note_state_machine.c` takes a plain `yp_voice_frame_t`
and a caller-supplied timestamp rather than audio_dsp's `voice_analysis_t`
or `esp_timer_get_time()`, specifically so its millisecond-boundary logic
(minimum note duration, release debounce) can be driven by a synthetic,
fully-controlled clock in tests instead of real elapsed time. Code that
does need ESP-IDF (`audio_capture`, `display`, the tasks in `main.c`) can
only be verified on real hardware - see `docs/hardware.md` and
`docs/tuning.md` for that side of testing.
