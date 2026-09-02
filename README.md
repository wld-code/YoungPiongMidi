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
acquisition (ADC continuous mode + DMA, no blocking one-shot reads),
DC removal, high-pass and low-pass filtering, RMS, an attack/release
envelope follower, and debounced voice-activity detection, all running as
dedicated FreeRTOS tasks and displayed live on the on-board LCD plus
rate-limited serial diagnostics.

**Not yet implemented**: pitch detection, MIDI note generation, BLE/DIN
MIDI output, dynamics-to-velocity/CC11 mapping, pitch bend. See "Roadmap"
below and `docs/architecture.md` for the full milestone list. This is
deliberate, incremental development, not an oversight - the project spec
this firmware follows explicitly asks for acquisition to be proven on
hardware before pitch detection is built on top of it.

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
Pitch + Dynamics            <- pitch detection not yet implemented
    |
Voice-to-MIDI Engine        <- not yet implemented
    |
MIDI                        <- not yet implemented
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
  pitch/            (placeholder - Milestone 3)
  voice_midi/       (placeholder - Milestones 5-10)
  midi/             (placeholder - Milestones 5, 8-10)
  display/          ST7789P3 driver + UI primitives
docs/               architecture.md, hardware.md, dsp.md, midi.md, tuning.md
test/               Host-side DSP tests (not yet populated)
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

## How to use it

Power the board (or plug it into a PC over USB) and watch the console:
diagnostics print at a rate-limited interval (`YP_DEBUG_LOG_INTERVAL_MS`
in `yp_config.h`) as `rms=... level=... voice_active=... clipped=...`, and
the LCD shows a live level meter plus the same numbers. Speaking or
singing into the microphone should move the level meter and eventually
flip `voice_active` to 1. There is no MIDI output yet.

## Current status

As of the last verification pass (see `docs/hardware.md` for details),
this firmware was built, flashed, and run on a physical ESP-SensairShuttle
v1.0 / ESP32-C5. It boots cleanly, initializes the display and
microphone, and runs the acquisition + RMS/envelope/VAD pipeline
continuously with no crashes and stable memory usage over multi-minute
runs. Measured DSP processing time is ~325 us/frame and
acquisition-to-analysis latency ~352 us - both well inside the project's
end-to-end latency target, though this only covers the stages implemented
so far.

**LCD note**: the panel initially showed nothing at all when first
connected - traced (via Espressif's own mainboard schematic, not a guess)
to `GPIO5`/`PWR_CTRL` being an active-**low** power-rail switch, driven
active-high in the first firmware revision. Fixed and reflashed; the fix
also happened to resolve an earlier `clipped` finding that had been
(incorrectly) attributed to the microphone - see `docs/hardware.md` and
`docs/tuning.md` for the full story. Still needs eyes-on confirmation that
the panel now actually displays the UI, since this session cannot see the
board.

## Roadmap

| Milestone | Description | Status |
|---|---|---|
| 1 | Continuous mic acquisition + basic signal display | Done |
| 2 | RMS/envelope + voice activity detection | Done |
| 3 | Fundamental frequency detection (YIN) | Planned |
| 4 | Frequency -> MIDI note conversion | Planned |
| 5 | MIDI Note On/Off generation | Planned |
| 6 | Vocal dynamics -> MIDI velocity | Planned |
| 7 | Continuous CC11 Expression | Planned |
| 8 | BLE MIDI | Planned |
| 9 | DIN MIDI over UART (optional) | Planned |
| 10 | Pitch bend for continuous vocal pitch | Planned |

## License

MIT - see `LICENSE`.
