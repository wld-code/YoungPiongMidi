# Tuning notes

Live findings from real-hardware bring-up, kept here instead of only in
commit messages so they survive.

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
