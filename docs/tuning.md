# Tuning notes

Live findings from real-hardware bring-up, kept here instead of only in
commit messages so they survive.

## Pitch: ESP32-C5 has no hardware FPU - float YIN measured ~79ms/hop

**Symptom observed on hardware**: after wiring up YIN (Milestone 3), the
UI's level meter appeared to stop responding to voice, and the console
showed repeated `audio_capture: output queue full, dropped a block`
followed by a task-watchdog reset on `dsp_task`.

**Measured, not assumed**: `audio_dsp`'s own pitch-stage timing (added
alongside YIN specifically to measure this, per the project spec's
"measure, don't claim" latency rule) showed the first all-`float`
implementation of YIN's difference function costing **~79 ms average per
hop**, against an 8 ms hop budget - roughly 10x over.

**Cause**: ESP32-C5 is RISC-V `rv32imac` - no `f` (hardware
single-precision float) extension. Confirmed in
`components/soc/project_include.cmake`: ESP32-C5/C6/H2 get `rv32imac`,
only ESP32-H4/P4 get `rv32imafc`. Every `float` multiply/add in YIN's
O(window x tau_range) inner loop (~57,700 iterations/hop at the default
window/tau settings) was therefore a soft-float library call - tens of
cycles each - not a single hardware instruction.

**Fix, in two steps**:
1. Rewrote the difference function's hot inner loop in `int16_t` (Q14
   fixed-point) samples accumulated in `int64_t`, which runs on the
   core's native hardware-multiplier integer path. Kept only the cheap,
   O(tau_range)-not-O(window x tau_range) normalization/threshold/
   interpolation stages as float. Measured result: ~13 ms average / ~17ms
   worst case - about 6x faster, but still over budget on its own.
2. Added `YP_PITCH_UPDATE_STRIDE_HOPS` (default 3): the analysis window
   still slides every hop, but the expensive CMNDF computation itself
   only runs once every 3 hops (~24 ms), returning the last computed
   estimate in between. Legitimate technique, not a shortcut - pitch
   doesn't need 125 Hz updates. Measured result: `dsp_task` average
   dropped to ~4.7 ms/hop, zero watchdog resets or dropped blocks over
   sustained runs.

**Takeaway**: don't estimate DSP cost on an MCU from "how many float ops"
without first checking whether the target actually has a hardware FPU -
the `-march` string in the toolchain file is the ground truth, and the
gap between "should be sub-millisecond" and "measured 79ms" is exactly
the soft-float tax. See docs/dsp.md's "Pitch detection" section for the
full numbers and reasoning.

## Display: LCD power rail polarity was inverted

**Symptom observed on hardware**: LCD physically connected, firmware
flashed and logged `display: ST7789P3 init done` with no error, but the
panel showed nothing - no backlight, no image.

**Cause**: `GPIO5`/`PWR_CTRL` was assumed active-high (drive high to power
the panel) with no hardware evidence either way. Espressif's own
`esp-sensairshuttle-mainboard-sch-lcd-v1_0.png` schematic shows it actually
drives the gate of a P-channel MOSFET (Q2, AO3401A) wired as a high-side
switch from `VCC_3V3` to `LCD_3V3` - active-**low**. Driving it high held
the switch off, so the ESP32-C5 was faithfully executing a correct SPI
init sequence against a panel that had no power at all, which is exactly
why the "init done" log line gave no indication anything was wrong: it
only reflects the SPI peripheral finishing its own transfer, not any
acknowledgment from the panel (nothing on this board wires MISO back).

**Fix**: drive `GPIO5` low in `display_init()` instead. See
`docs/hardware.md` for the full schematic-based reasoning and the pin
table update.

**Bonus, unexpected side-effect**: this same fix also resolved the
"`clipped` is always 1" finding recorded earlier below/in
`docs/hardware.md` - the two circuits share no GPIOs, so the working
theory is that the unpowered-but-still-SPI-clocked LCD was coupling
electrical noise into the analog mic front-end via a shared rail or
ground. Recorded here as a correction, not left as a stale claim: the
original "probably no microphone connected" guess was reasonable given
what was known at the time, but turned out to be wrong.

**Takeaway**: a driver logging success only proves the MCU-side half of a
transaction completed without a bus error. It says nothing about whether
the peripheral on the other end had power, and "no error, still nothing
visible" is a power/wiring question, not a protocol one - check the power
switch/rail before re-reading the datasheet's command tables.

## Display: text rendering must batch its SPI transactions

**Symptom observed on hardware**: firmware built and flashed cleanly, but
within a few seconds the console showed repeated
`Task watchdog got triggered ... IDLE (CPU 0)` with `ui_task` reported as
the currently-running task, and no audio blocks were reaching `dsp_task`
(a `dsp_task` symptom that turned out to be unrelated - see below).

**Cause**: the first `display_draw_text()` implementation called
`display_fill_rect()` once per glyph pixel cell (up to 35 times per
character), and each `display_fill_rect()` call issued its own
CASET/RASET/RAMWR command sequence plus one SPI data transfer. A single
short line of UI text (e.g. `"RMS 0.123"`, redrawn every UI tick) produced
on the order of a thousand tiny SPI transactions. FreeRTOS's
`vTaskDelayUntil()` does not insert a delay once a task is behind its
target period, so once per-frame render time exceeded the UI period,
`ui_task` never yielded at all, permanently starving the idle task at
priority 0 below it.

**Fix**: `display_draw_text()` now renders a whole line into a small
line-shaped buffer in RAM first, then issues one CASET/RASET/RAMWR
sequence and a handful of chunked SPI transfers for the entire line. This
cut the SPI transaction count by roughly two orders of magnitude per
redraw and resolved the watchdog resets - confirmed by a subsequent
20+ second run on hardware with zero watchdog events.

**Takeaway for future UI work**: never structure a drawing primitive so
its SPI (or any bus) transaction count scales with glyph pixel count times
redraw rate. Batch first, transfer once.

## audio_capture task stack size

**Symptom observed on hardware**: after fixing the above, a
`Guru Meditation Error: ... (Stack protection fault)` appeared, reported
in task `"audio_capture"`.

**Cause**: `CAPTURE_TASK_STACK_BYTES` was set to 3072 B by estimate rather
than measurement. The task's local
`adc_continuous_data_t parsed[YP_AUDIO_DMA_FRAME_SAMPLES]` scratch array
alone is ~2 KB at the default `YP_AUDIO_DMA_FRAME_SAMPLES` (128), and
`ESP_LOGx` calls add their own (non-trivial) stack use on top of that.

**Fix**: raised to 8192 B. Confirmed stable over a 20+ second run with no
further stack faults.

**Takeaway**: "no large local arrays" is not always obvious at a glance -
a `sizeof()`-based estimate of a task's actual locals (including anything
a called library function puts on the stack, like logging) is worth doing
before picking a stack size, not after a fault reports it.

## ADC clipping flag: a real, unresolved hardware observation

`audio_block_t.clipped` has been true on every captured block so far, even
though measured RMS is very low (near-silent room level). This is very
likely because nothing is physically connected to the board's analog
microphone input right now (an open input into a high-gain amplifier
stage tends to rail intermittently), not a software bug - see
`docs/hardware.md`'s "A real finding, reported honestly" section for the
full reasoning. **Not fixed in software** because there is nothing to fix
in software until it's confirmed whether a microphone element is actually
attached.

## Measured, not assumed: DSP timing

With Milestones 1-2 running on real hardware (ADC continuous acquisition
-> DC removal/HPF -> LPF -> RMS -> envelope -> VAD), `esp_timer_get_time()`
instrumentation in `main.c`'s `dsp_task` reports, sustained over multiple
minutes:

- DSP processing time per 512-sample frame: ~325 us average, ~335-385 us
  observed max.
- Acquisition-to-analysis latency (ADC block timestamp to DSP-done
  timestamp): ~352 us average.

These numbers will change once pitch detection (YIN, Milestone 3) is
added - it is the heaviest single stage in the eventual pipeline - and
should be re-measured rather than assumed to still hold.
