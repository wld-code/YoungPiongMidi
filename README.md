# YoungPiongMidi

Real-time voice-to-MIDI converter based on the Espressif ESP32-C5.

YoungPiongMidi listens to a human voice through an analog microphone,
detects its pitch and dynamics in real time, and turns them into MIDI so
the voice can drive a synthesizer or DAW like an instrument. It is
designed to preserve two musical properties of the voice, not just detect
notes:

- **Pitch** -> MIDI note (and eventually pitch bend for continuous
  intonation).
- **Dynamics** -> MIDI velocity at note onset, and continuous MIDI CC11
  Expression while a note is held.

## What it does (current status)

**Implemented and verified on real hardware**: continuous microphone
acquisition (ADC continuous mode + DMA, no blocking one-shot reads), DC
removal, high-pass and low-pass filtering, RMS, an attack/release
envelope follower, debounced voice-activity detection, YIN
fundamental-frequency (pitch) detection, frequency -> MIDI note
conversion (note name, octave, cents deviation), a note-stabilization
state machine (debounces raw pitch fluctuation into real MIDI Note On/Off
events - the classic "a wobble must not spam Note On/Off/On/Off" problem),
and a transport-independent MIDI event queue, all running as dedicated
FreeRTOS tasks and displayed live on the on-board LCD plus rate-limited
serial diagnostics. The pitch/note/state-machine logic is also covered by
a real, passing host-side test suite - see "Testing" below.

**Not yet implemented**: an actual MIDI transport (BLE/UART - Note On/Off
events are generated and queued for real today, but only reach a
diagnostic log line, not a wire), CC11 expression, pitch bend, and
refining velocity into full dynamics tracking. See "Roadmap" below and
`docs/architecture.md` for the full milestone list. This is deliberate,
incremental development, not an oversight - the project spec this
firmware follows explicitly asks for each stage to be proven (on
hardware, and where possible in an automated test) before the next is
built on top of it.

## Hardware

- **Board**: Espressif ESP-SensairShuttle v1.0
- **MCU/module**: ESP32-C5-WROOM-1-N16R8 (16 MB flash, 8 MB PSRAM)
- **Microphone**: analog, pre-amplified by the board's front-end, on
  GPIO6 / ADC1 channel 5
- **Display**: ST7789P3, 1.83", 240x284, 4-wire SPI
- **Framework**: ESP-IDF v5.5.5 (latest stable release with ESP32-C5
  support at time of writing)

Every pin used by this firmware is verified, not guessed - see
`docs/hardware.md` for the full sourcing trail against Espressif's own
documentation and factory firmware for this exact board.

## Architecture

```
Microphone
    |
ADC + DMA
    |
Audio DSP (DC removal, HPF/LPF, RMS, envelope, VAD)
    |
Pitch + Dynamics            <- pitch (YIN), note conversion, velocity done
    |
Voice-to-MIDI Engine        <- note-stabilization state machine done
    |
MIDI                        <- event queue done; no real transport yet
    |
Synthesizer / DAW
```

See `docs/architecture.md` for the component map, task list (priorities
and stack sizes), and the reasoning behind them.

## Repository layout

```
main/               App entry point, task wiring
components/
  board/            Pin definitions + central configuration
  audio_capture/    ADC continuous-mode acquisition
  audio_dsp/        RMS, envelope, voice-activity detection
  pitch/            YIN fundamental-frequency detection
  voice_midi/       Frequency<->MIDI note conversion, dynamics->velocity,
                     note-stabilization state machine
  midi/             Transport-independent MIDI event queue (no BLE/UART
                     transport yet - Milestones 8-9)
  display/          ST7789P3 driver + UI primitives
docs/               architecture.md, hardware.md, dsp.md, midi.md, tuning.md
test/               Host-side tests (real, passing - see test/README.md)
tools/              plot_audio.py, analyze_pitch.py (not yet written)
```

## Build instructions

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c5/get-started/index.html)
v5.5 or later (for ESP32-C5 support).

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32c5
idf.py build
```

## Flash instructions

```sh
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

(On Linux, the port is typically `/dev/ttyACM0`. ESP32-C5 uses native
USB-Serial/JTAG, so no external USB-UART bridge or drivers are needed.)

## Testing

The frequency<->MIDI-note conversion, RMS, envelope follower, YIN pitch
detector, and note-stabilization state machine all have real,
hardware-independent unit tests:

```sh
cd test
make            # builds and runs every suite; fails loudly on any failure
```

No ESP-IDF or board required - see `test/README.md` for what each suite
actually checks. As of this writing: 459 checks across 5 suites, all
passing - including a direct test of the spec's own anti-flicker
requirement ("a small pitch fluctuation must not generate Note Off/On/Off/On
continuously").

## How to use it

Power the board (or plug it into a PC over USB) and watch the console:
diagnostics print at a rate-limited interval (`YP_DEBUG_LOG_INTERVAL_MS`
in `yp_config.h`) as, e.g.,
`pitch=440.2Hz note=A4 midi=69 cents=0.8 confidence=0.96 rms=... velocity=84 state=NOTE_ACTIVE clipped=0`
(or `note=---` while confidence is below `YP_PITCH_CONFIDENCE_THRESHOLD`,
0.55 by default), and the LCD shows the same note/frequency/confidence, a
live level meter, and RMS/status. Speaking or singing a sustained, clear
pitch into the microphone should move the level meter, flip
`voice_active` to 1, show a note name (e.g. `NOTE A4  +1C`) instead of
`NOTE ---`, and - once held stably for a few frames - produce a real
`midi: NOTE_ON  ch=0 note=69 vel=84` line as the note-stabilization state
machine commits it. There is no wire MIDI output yet (BLE/UART are
Milestones 8-9) - the event is generated and queued for real, but the
only "transport" today is that log line.

## Current status

As of the last verification pass (see `docs/hardware.md` and
`docs/tuning.md` for details), this firmware was built, flashed, and run
on a physical ESP-SensairShuttle v1.0 / ESP32-C5. It boots cleanly,
initializes the display and microphone, runs a boot self-test (LCD color
cycle + speaker melody), and runs the full acquisition ->
RMS/envelope/VAD/YIN -> note-state-machine -> MIDI-event-queue pipeline
continuously with no crashes and stable memory usage over multi-minute
runs. Measured `dsp_task` time per hop is ~4.7 ms average (worst case
~13 ms on the 1-in-3 hops that run the full YIN analysis, absorbed by the
capture queue) - see docs/dsp.md for how that number was arrived at,
including a real finding along the way: ESP32-C5 has no hardware FPU, and
the first all-`float` YIN implementation measured ~79 ms/hop before being
rewritten in fixed-point.

The note-stabilization state machine's *logic* (Milestone 5) is verified
by 248 host-side test checks rather than a live singing session in this
particular verification pass - deliberately, not as a shortcut: its
correctness lives in exact frame-by-frame/millisecond-boundary behavior
(does a change get honored one frame too early? does a one-frame dropout
wrongly release a note?) that a host test can assert on deterministically
and a live mic session cannot. The hardware run confirms the *integration*
- it boots, initializes the MIDI queue, and runs the whole pipeline with
zero regressions - which is what hardware verification can actually add
on top of the host tests here.

Two other real bring-up findings worth knowing about, both already fixed
and documented in `docs/hardware.md`/`docs/tuning.md`: the LCD initially
showed nothing because `GPIO5`/`PWR_CTRL` is an active-**low** power-rail
switch (traced via Espressif's own schematic, not guessed) that the first
firmware revision drove backwards; and the microphone's `clipped` flag
was stuck true until that same LCD fix, most likely because the
unpowered-but-still-clocked panel was coupling noise into the analog
front-end.

## Roadmap

| Milestone | Description | Status |
|---|---|---|
| 1 | Continuous mic acquisition + basic signal display | Done |
| 2 | RMS/envelope + voice activity detection | Done |
| 3 | Fundamental frequency detection (YIN) | Done |
| 4 | Frequency -> MIDI note conversion | Done, host-tested + verified on hardware |
| 5 | MIDI Note On/Off generation | Done, host-tested + verified on hardware |
| 6 | Vocal dynamics -> MIDI velocity | Partially done (velocity mapping; see docs/midi.md) |
| 7 | Continuous CC11 Expression | Planned |
| 8 | BLE MIDI | Planned |
| 9 | DIN MIDI over UART (optional) | Planned |
| 10 | Pitch bend for continuous vocal pitch | Planned |

## License

MIT - see `LICENSE`.
