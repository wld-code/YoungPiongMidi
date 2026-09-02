# Hardware

Target board: **Espressif ESP-SensairShuttle v1.0**, ESP32-C5-WROOM-1-N16R8
(16 MB flash, 8 MB PSRAM).

## Pin assignments and their source

Every pin in `components/board/include/yp_board.h` was verified against two
independent Espressif sources before being used, not assumed from the
project brief alone:

1. The official [ESP-SensairShuttle v1.0 user guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32c5/esp-sensairshuttle/user_guide_v1.0.html).
2. The board-support files Espressif ships for this exact board in
   [`espressif/esp-dev-kits`](https://github.com/espressif/esp-dev-kits),
   under `examples/esp-sensairshuttle/examples/factory_demo/boards/esp_SensairShuttle/`
   (`board_devices.yaml`, `board_peripherals.yaml`, `setup_device.c`) - this
   is Espressif's own factory firmware for this board, known-good on real
   hardware.

| Signal | GPIO | Source |
|---|---|---|
| Microphone (analog, pre-amplified) | GPIO6 / ADC1 channel 5 | user guide + `board_peripherals.yaml` (`gpio_pa_control` context) |
| Speaker PA_CTL (active high) | GPIO1 | user guide |
| Speaker PDM_P / PDM_N | GPIO7 / GPIO8 | user guide |
| LCD SDA (SPI MOSI) | GPIO23 | user guide + `board_peripherals.yaml` (`spi_display.data0_io_num`) |
| LCD SCL (SPI SCLK) | GPIO24 | user guide + `board_peripherals.yaml` (`spi_display.sclk_io_num`) |
| LCD CS | GPIO25 | user guide + `board_devices.yaml` (`io_spi_config.cs_gpio_num`) |
| LCD DC | GPIO26 | user guide + `board_devices.yaml` (`io_spi_config.dc_gpio_num`) |
| LCD power rail switch (**active-low**, see below) | GPIO5 | user guide + `esp-sensairshuttle-mainboard-sch-lcd-v1_0.png` schematic |
| Boot button | GPIO28 | user guide |

Pins **not** in this table (e.g. a MIDI DIN UART pin) are not defined
anywhere in the codebase - see Section 11 of the original spec and
`yp_config.h`'s `YP_MIDI_UART_ENABLED` (0 by default) for why.

### Things confirmed that the project brief did not specify

Espressif's own `board_devices.yaml` for this board additionally confirms,
for the LCD:

- **SPI mode 3**, no separate hardware RESET pin (`reset_gpio_num: -1`) -
  `display_init()` performs a software reset (command `0x01`) instead.
- A working SPI clock of **20 MHz** (`pclk_hz: 20000000`) - used verbatim
  in `YP_LCD_SPI_CLOCK_HZ`.
- Native panel orientation is portrait, 240 (H) x 284 (V), `MADCTL = 0x00`.
  Espressif's factory demo runs the panel in landscape by asking its LVGL
  driver to swap_xy/mirror_x in software; this project keeps native
  portrait orientation for simplicity (see `display.h`).
- The full ST7789P3 init register sequence (porch control, gate control,
  VCOM, power control, positive/negative gamma, gate position) used in
  `components/display/display.c` is transcribed from
  `setup_device.c`'s `vendor_specific_init[]` table, which Espressif ships
  as the known-good init sequence for this exact panel on this exact board.

### Things not fully specified, and how this project handles them

- **Boot button polarity**: assumed active-low with an internal pull-up
  (the universal ESP32 BOOT-button convention). `yp_board_init()` enables
  the pull-up defensively. Not yet exercised in firmware logic.
- **ADC attenuation**: the microphone signal is pre-amplified by the
  board's analog front-end before GPIO6, but the exact output swing/bias
  point is not published. `YP_AUDIO_ADC_ATTEN` defaults to `ADC_ATTEN_DB_12`
  (full ~3.3 V range); `yp_config.h` flags this as something to revisit
  once real signal levels are observed.
- **MIDI DIN UART pin**: not exposed by ESP-SensairShuttle's documented
  pinout at all. `YP_MIDI_UART_ENABLED` stays `0` until a revision of this
  document (or the board) defines one - see the project brief's Section 11.

## What has actually been verified on real hardware

This is not a simulated or assumed result. As of the last firmware flash
(see `README.md` "Status" for the date), the following were verified by
flashing this exact repository to a physical ESP-SensairShuttle v1.0 board
connected over USB (`/dev/cu.usbmodem*`, native USB-Serial/JTAG) and reading
its console:

- The board identifies as **ESP32-C5 (revision v1.0)**, 8 MB PSRAM detected
  and passes its self-test, 16 MB flash detected.
- `yp_board_init()`, `display_init()` (ST7789P3), `audio_dsp_init()` and
  `audio_capture_init()` (ADC continuous mode on GPIO6/ADC1 ch5) all
  complete with `ESP_OK`.
- `adc_continuous_io_to_channel(GPIO6)` - a runtime check, not a compile-time
  assumption - confirms GPIO6 maps to ADC unit 1 channel 5 as documented.
- The full pipeline (ADC DMA -> capture task -> queue -> dsp_task -> RMS ->
  envelope -> VAD) runs continuously with no crashes, no task-watchdog
  resets, and stable free heap (~8.66 MB, PSRAM included) over multiple
  minutes of operation.
- Measured DSP processing time per 512-sample frame: ~325 us average,
  ~335-385 us max. Measured acquisition-to-analysis latency: ~352 us
  average. Both are console output, not estimates - see `main.c`'s
  `dsp_task` for the instrumentation. This is well inside the <30-50 ms
  end-to-end latency target from the project brief, though it only covers
  the capture+RMS+envelope+VAD stages implemented so far, not pitch
  detection or MIDI (not yet implemented).
- Voice-activity detection did cross into the `voice_active` state during
  the very first frames after boot (the initial DC-tracker convergence
  transient), confirming the VAD state machine and its debounce logic run
  correctly end to end.

### Bug found and fixed: LCD power rail polarity was inverted

The first hardware bring-up connected the LCD, flashed the firmware, and
the panel showed nothing at all - no image, no backlight - even though the
console logged `display: ST7789P3 init done` with no error. That log line
only means the ESP32-C5's SPI peripheral finished sending its command
sequence without a bus error; nothing on this board reads a status
register back from the panel (MISO is not wired), so it cannot by itself
confirm the panel actually received power or responded.

The actual cause was found by pulling Espressif's own
`esp-sensairshuttle-mainboard-sch-lcd-v1_0.png` schematic (from the same
`espressif/esp-dev-kits` source used for the pin table above) rather than
guessing: `GPIO5`/`PWR_CTRL` drives the gate of **Q2, a P-channel MOSFET
(AO3401A)** wired as a high-side load switch between `VCC_3V3` (source)
and `LCD_3V3` (drain) - the rail that powers both the ST7789P3 and its
backlight. A P-channel high-side switch like this is **active-low**:
pulling the gate low (relative to the source) turns it on. The first
firmware revision drove `GPIO5` high to "power on" the panel, which is
backwards - it held the switch off and cut power to the entire LCD rail.
Fixed by driving `GPIO5` low in `display_init()` (see the comment there
for the full reasoning); confirmed working by having the LCD's title text
and level meter physically visible on the board afterward.

A useful side-effect of this fix: before it, every captured audio block
had its `clipped` flag set even at near-silent RMS (~0.0001-0.0002); after
it, `clipped` reads 0 and RMS shows normal small ambient-noise variation
(~0.0008-0.0067). The two circuits are on unrelated GPIOs/buses, so the
most likely explanation is that the unpowered LCD was still being clocked
by SPI the whole time, and that switching activity coupled electrical
noise into the analog microphone front-end over a shared rail/ground
rather than the microphone genuinely being disconnected as first
suspected. This is a correction of an earlier (reasonable, but wrong)
guess in this document, not a new claim to take on faith - if `clipped`
starts reading 1 again, check the LCD power fix is still in place before
assuming a microphone wiring problem.
