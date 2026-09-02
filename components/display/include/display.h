/**
 * @file display.h
 * @brief Minimal driver + UI helpers for the ST7789P3 1.83" 240x284 panel on
 *        ESP-SensairShuttle.
 *
 * This is deliberately a thin, direct-SPI driver rather than a full
 * esp_lcd_panel / LVGL stack: the UI requirement (section 13 of the project
 * spec) is a handful of text fields and a level meter refreshed at
 * 10-20 Hz, not a general-purpose GUI. See docs/architecture.md for the
 * rationale.
 *
 * Panel orientation is native/portrait (240 wide x 284 tall, MADCTL=0x00).
 * Colors are RGB565, big-endian on the wire (as required by the ST7789
 * RAMWR command), least significant color bits following the datasheet's
 * 16-bit interface format.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  284

/* A small set of RGB565 colors used by the UI. */
#define DISPLAY_COLOR_BLACK   0x0000
#define DISPLAY_COLOR_WHITE   0xFFFF
#define DISPLAY_COLOR_RED     0xF800
#define DISPLAY_COLOR_GREEN   0x07E0
#define DISPLAY_COLOR_BLUE    0x001F
#define DISPLAY_COLOR_YELLOW  0xFFE0
#define DISPLAY_COLOR_CYAN    0x07FF
#define DISPLAY_COLOR_GRAY    0x8410
#define DISPLAY_COLOR_ORANGE  0xFC00

/**
 * @brief Power up the panel rail, reset and initialize the ST7789P3
 *        controller, and turn the display on (blank/black).
 *
 * Must be called once, after yp_board_init(), before any other display_*
 * call. Blocking; takes on the order of 150-250 ms due to panel power-on
 * and sleep-out timing requirements.
 */
esp_err_t display_init(void);

/** Fill the whole screen with a solid color. */
esp_err_t display_clear(uint16_t color);

/** Fill an axis-aligned rectangle. Clipped to the panel bounds. */
esp_err_t display_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

/**
 * @brief Draw a left-to-right proportional level meter.
 *
 * @param x, y      top-left corner
 * @param w, h      bar footprint, including the 1px border
 * @param fraction  0.0..1.0 fill level
 * @param fg        fill color
 * @param bg        empty-area color
 */
esp_err_t display_draw_meter(int16_t x, int16_t y, int16_t w, int16_t h,
                              float fraction, uint16_t fg, uint16_t bg);

/**
 * @brief Draw a string using the built-in 5x7 bitmap font.
 *
 * Supports upper-case A-Z, digits 0-9, and space . , : - % + / #. Any other
 * character is rendered as a space. Lower-case input is upper-cased.
 *
 * @param x, y  top-left pixel of the first glyph
 * @param scale integer pixel scale (1 = 5x7, 2 = 10x14, ...)
 * @return width of the rendered string in pixels
 */
int16_t display_draw_text(int16_t x, int16_t y, const char *str,
                           uint16_t fg, uint16_t bg, uint8_t scale);

#ifdef __cplusplus
}
#endif
