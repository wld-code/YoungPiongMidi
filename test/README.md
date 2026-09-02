# Host-side tests

Tests for this project's hardware-independent DSP/conversion code
(frequency<->MIDI note conversion, RMS, the envelope follower, YIN pitch
detection), per the project spec's section 17: "DSP algorithms should be
testable without the ESP32 when possible." These build and run on your
Mac/Linux machine with a plain C compiler - no ESP-IDF, no board, no
simulator.

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

`test_common.h` is a ~60-line assert-style framework (not a dependency on
Unity/CMock/etc - the code under test is a handful of pure functions, and
pulling in a full framework's own build system for that would be more
machinery than the problem needs). Each `test_*.c` builds to its own
standalone executable; `make test` runs all of them and reports a single
pass/fail.

## Why these functions specifically are host-testable

`rms.c`, `envelope.c`, `yin.c` and `voice_midi.c` have zero ESP-IDF
dependency by design - they only ever include `yp_config.h` (plain
macros) and the C standard library (`<math.h>`, `<string.h>`, ...). That
is not an accident of these particular files; it is why they were kept
free of `esp_log.h`/FreeRTOS/etc in the first place. Code that does need
ESP-IDF (`audio_capture`, `display`, the tasks in `main.c`) can only be
verified on real hardware - see `docs/hardware.md` and `docs/tuning.md`
for that side of testing.
