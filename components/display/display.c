/**
 * @file display.c
 * @brief ST7789P3 driver (direct SPI, no esp_lcd) + tiny bitmap-font UI
 *        primitives for ESP-SensairShuttle's 240x284 panel.
 *
 * The panel init command sequence below (MADCTL/COLMOD/porch/gate/VCOM/
 * power/gamma register values) is not guessed: it matches the sequence
 * Espressif ships in its own factory firmware for this exact board
 * (espressif/esp-dev-kits, examples/esp-sensairshuttle .../setup_device.c),
 * which is known-good on real ESP-SensairShuttle v1.0 hardware. See
 * docs/hardware.md for the full trail.
 */
#include <string.h>
#include "display.h"
#include "yp_board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "display";

/* -------------------------------------------------------------------- */
/*  ST7789 command set (subset used here)                                */
/* -------------------------------------------------------------------- */
#define LCD_CMD_SWRESET   0x01
#define LCD_CMD_SLPOUT    0x11
#define LCD_CMD_INVON     0x21
#define LCD_CMD_DISPON    0x29
#define LCD_CMD_CASET     0x2A
#define LCD_CMD_RASET     0x2B
#define LCD_CMD_RAMWR     0x2C
#define LCD_CMD_MADCTL    0x36
#define LCD_CMD_COLMOD    0x3A
#define LCD_CMD_PORCTRL   0xB2
#define LCD_CMD_GCTRL     0xB7
#define LCD_CMD_VCOMS     0xBB
#define LCD_CMD_LCMCTRL   0xC0
#define LCD_CMD_VDVVRHEN  0xC2
#define LCD_CMD_VRHS      0xC3
#define LCD_CMD_FRCTRL2   0xC6
#define LCD_CMD_PWCTRL1   0xD0
#define LCD_CMD_GATE_GND  0xD6
#define LCD_CMD_PVGAMCTRL 0xE0
#define LCD_CMD_NVGAMCTRL 0xE1
#define LCD_CMD_GATE_POS  0xE4

typedef struct {
    uint8_t cmd;
    const uint8_t *params;
    uint8_t params_len;
    uint16_t delay_ms;
} lcd_init_cmd_t;

static const uint8_t k_madctl[]    = {0x00};
static const uint8_t k_colmod[]    = {0x05};
static const uint8_t k_porctrl[]   = {0x0C, 0x0C, 0x00, 0x33, 0x33};
static const uint8_t k_gctrl[]     = {0x05};
static const uint8_t k_vcoms[]     = {0x21};
static const uint8_t k_lcmctrl[]   = {0x2C};
static const uint8_t k_vdvvrhen[]  = {0x01};
static const uint8_t k_vrhs[]      = {0x15};
static const uint8_t k_frctrl2[]   = {0x0F};
static const uint8_t k_pwctrl1a[]  = {0xA7};
static const uint8_t k_pwctrl1b[]  = {0xA4, 0xA1};
static const uint8_t k_gategnd[]   = {0xA1};
static const uint8_t k_pgamma[]    = {0xF0, 0x05, 0x0E, 0x08, 0x0A, 0x17, 0x39, 0x54,
                                       0x4E, 0x37, 0x12, 0x12, 0x31, 0x37};
static const uint8_t k_ngamma[]    = {0xF0, 0x10, 0x14, 0x0D, 0x0B, 0x05, 0x39, 0x44,
                                       0x4D, 0x38, 0x14, 0x14, 0x2E, 0x35};
static const uint8_t k_gatepos[]   = {0x23, 0x00, 0x00};

static const lcd_init_cmd_t k_init_seq[] = {
    { LCD_CMD_SLPOUT,    NULL,          0,  120 },
    { LCD_CMD_MADCTL,    k_madctl,      1,  0   },
    { LCD_CMD_COLMOD,    k_colmod,      1,  0   },
    { LCD_CMD_PORCTRL,   k_porctrl,     5,  0   },
    { LCD_CMD_GCTRL,     k_gctrl,       1,  0   },
    { LCD_CMD_VCOMS,     k_vcoms,       1,  0   },
    { LCD_CMD_LCMCTRL,   k_lcmctrl,     1,  0   },
    { LCD_CMD_VDVVRHEN,  k_vdvvrhen,    1,  0   },
    { LCD_CMD_VRHS,      k_vrhs,        1,  0   },
    { LCD_CMD_FRCTRL2,   k_frctrl2,     1,  0   },
    { LCD_CMD_PWCTRL1,   k_pwctrl1a,    1,  0   },
    { LCD_CMD_PWCTRL1,   k_pwctrl1b,    2,  0   },
    { LCD_CMD_GATE_GND,  k_gategnd,     1,  0   },
    { LCD_CMD_PVGAMCTRL, k_pgamma,      14, 0   },
    { LCD_CMD_NVGAMCTRL, k_ngamma,      14, 0   },
    { LCD_CMD_GATE_POS,  k_gatepos,     3,  0   },
    { LCD_CMD_INVON,     NULL,          0,  0   },
    { LCD_CMD_DISPON,    NULL,          0,  20  },
};

static spi_device_handle_t s_spi = NULL;

/* -------------------------------------------------------------------- */
/*  Low-level bus helpers                                                */
/* -------------------------------------------------------------------- */

static esp_err_t lcd_write_cmd(uint8_t cmd)
{
    gpio_set_level(YP_PIN_LCD_DC, 0);
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t lcd_write_data(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return ESP_OK;
    }
    gpio_set_level(YP_PIN_LCD_DC, 1);
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t lcd_set_window(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    uint8_t caset[4] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF };
    uint8_t raset[4] = { y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF };
    esp_err_t err;

    err = lcd_write_cmd(LCD_CMD_CASET);
    if (err != ESP_OK) return err;
    err = lcd_write_data(caset, sizeof(caset));
    if (err != ESP_OK) return err;

    err = lcd_write_cmd(LCD_CMD_RASET);
    if (err != ESP_OK) return err;
    err = lcd_write_data(raset, sizeof(raset));
    if (err != ESP_OK) return err;

    return lcd_write_cmd(LCD_CMD_RAMWR);
}

/* -------------------------------------------------------------------- */
/*  Init                                                                  */
/* -------------------------------------------------------------------- */

esp_err_t display_init(void)
{
    esp_err_t err;

    gpio_config_t pwr_cfg = {
        .pin_bit_mask = 1ULL << YP_PIN_LCD_PWR,
        .mode = GPIO_MODE_OUTPUT,
    };
    err = gpio_config(&pwr_cfg);
    if (err != ESP_OK) return err;

    gpio_config_t dc_cfg = {
        .pin_bit_mask = 1ULL << YP_PIN_LCD_DC,
        .mode = GPIO_MODE_OUTPUT,
    };
    err = gpio_config(&dc_cfg);
    if (err != ESP_OK) return err;

    /* Power the panel rail and let it settle before driving SPI. */
    gpio_set_level(YP_PIN_LCD_PWR, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = YP_PIN_LCD_SDA,
        .miso_io_num = -1,
        .sclk_io_num = YP_PIN_LCD_SCL,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * 2 * 4,
    };
    err = spi_bus_initialize(YP_LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = YP_LCD_SPI_CLOCK_HZ,
        .mode = YP_LCD_SPI_MODE,
        .spics_io_num = YP_PIN_LCD_CS,
        .queue_size = 1,
    };
    err = spi_bus_add_device(YP_LCD_SPI_HOST, &dev_cfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    /* No hardware RESET pin on this board revision (see yp_board.h):
     * perform a software reset instead. */
    err = lcd_write_cmd(LCD_CMD_SWRESET);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(150));

    for (size_t i = 0; i < sizeof(k_init_seq) / sizeof(k_init_seq[0]); i++) {
        const lcd_init_cmd_t *c = &k_init_seq[i];
        err = lcd_write_cmd(c->cmd);
        if (err != ESP_OK) return err;
        err = lcd_write_data(c->params, c->params_len);
        if (err != ESP_OK) return err;
        if (c->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }

    ESP_LOGI(TAG, "ST7789P3 init done (%dx%d, SPI %d MHz)",
             DISPLAY_WIDTH, DISPLAY_HEIGHT, YP_LCD_SPI_CLOCK_HZ / 1000000);

    return display_clear(DISPLAY_COLOR_BLACK);
}

/* -------------------------------------------------------------------- */
/*  Drawing primitives                                                    */
/* -------------------------------------------------------------------- */

esp_err_t display_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DISPLAY_WIDTH)  w = DISPLAY_WIDTH - x;
    if (y + h > DISPLAY_HEIGHT) h = DISPLAY_HEIGHT - y;
    if (w <= 0 || h <= 0) {
        return ESP_OK;
    }

    esp_err_t err = lcd_set_window(x, y, x + w - 1, y + h - 1);
    if (err != ESP_OK) return err;

    uint8_t row[DISPLAY_WIDTH * 2];
    uint8_t hi = color >> 8, lo = color & 0xFF;
    for (int16_t i = 0; i < w; i++) {
        row[2 * i] = hi;
        row[2 * i + 1] = lo;
    }

    gpio_set_level(YP_PIN_LCD_DC, 1);
    for (int16_t r = 0; r < h; r++) {
        spi_transaction_t t = {
            .length = (size_t)w * 2 * 8,
            .tx_buffer = row,
        };
        err = spi_device_polling_transmit(s_spi, &t);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t display_clear(uint16_t color)
{
    return display_fill_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, color);
}

esp_err_t display_draw_meter(int16_t x, int16_t y, int16_t w, int16_t h,
                              float fraction, uint16_t fg, uint16_t bg)
{
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;

    esp_err_t err = display_fill_rect(x, y, w, h, DISPLAY_COLOR_GRAY);
    if (err != ESP_OK) return err;

    int16_t inner_w = w - 2, inner_h = h - 2;
    int16_t fill_w = (int16_t)(inner_w * fraction);

    err = display_fill_rect(x + 1, y + 1, fill_w, inner_h, fg);
    if (err != ESP_OK) return err;
    if (fill_w < inner_w) {
        err = display_fill_rect(x + 1 + fill_w, y + 1, inner_w - fill_w, inner_h, bg);
    }
    return err;
}

/* -------------------------------------------------------------------- */
/*  Tiny 5x7 bitmap font                                                  */
/*                                                                        */
/*  Purpose-built for this UI's character set (A-Z, 0-9, and the         */
/*  punctuation used in note names / units / labels). Each glyph is 7    */
/*  rows of 5 bits, MSB..LSB = left..right column, 1 = pixel on.         */
/* -------------------------------------------------------------------- */

typedef struct {
    char c;
    uint8_t rows[7];
} glyph_t;

static const glyph_t k_font[] = {
    {'0', {0b01110,0b10001,0b10011,0b10101,0b11001,0b10001,0b01110}},
    {'1', {0b00100,0b01100,0b00100,0b00100,0b00100,0b00100,0b01110}},
    {'2', {0b01110,0b10001,0b00001,0b00010,0b00100,0b01000,0b11111}},
    {'3', {0b11111,0b00010,0b00100,0b00010,0b00001,0b10001,0b01110}},
    {'4', {0b00010,0b00110,0b01010,0b10010,0b11111,0b00010,0b00010}},
    {'5', {0b11111,0b10000,0b11110,0b00001,0b00001,0b10001,0b01110}},
    {'6', {0b00110,0b01000,0b10000,0b11110,0b10001,0b10001,0b01110}},
    {'7', {0b11111,0b00001,0b00010,0b00100,0b01000,0b01000,0b01000}},
    {'8', {0b01110,0b10001,0b10001,0b01110,0b10001,0b10001,0b01110}},
    {'9', {0b01110,0b10001,0b10001,0b01111,0b00001,0b00010,0b01100}},
    {'A', {0b01110,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001}},
    {'B', {0b11110,0b10001,0b10001,0b11110,0b10001,0b10001,0b11110}},
    {'C', {0b01110,0b10001,0b10000,0b10000,0b10000,0b10001,0b01110}},
    {'D', {0b11100,0b10010,0b10001,0b10001,0b10001,0b10010,0b11100}},
    {'E', {0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b11111}},
    {'F', {0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b10000}},
    {'G', {0b01110,0b10001,0b10000,0b10111,0b10001,0b10001,0b01111}},
    {'H', {0b10001,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001}},
    {'I', {0b01110,0b00100,0b00100,0b00100,0b00100,0b00100,0b01110}},
    {'J', {0b00111,0b00010,0b00010,0b00010,0b00010,0b10010,0b01100}},
    {'K', {0b10001,0b10010,0b10100,0b11000,0b10100,0b10010,0b10001}},
    {'L', {0b10000,0b10000,0b10000,0b10000,0b10000,0b10000,0b11111}},
    {'M', {0b10001,0b11011,0b10101,0b10101,0b10001,0b10001,0b10001}},
    {'N', {0b10001,0b11001,0b10101,0b10101,0b10011,0b10001,0b10001}},
    {'O', {0b01110,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110}},
    {'P', {0b11110,0b10001,0b10001,0b11110,0b10000,0b10000,0b10000}},
    {'Q', {0b01110,0b10001,0b10001,0b10001,0b10101,0b10010,0b01101}},
    {'R', {0b11110,0b10001,0b10001,0b11110,0b10100,0b10010,0b10001}},
    {'S', {0b01111,0b10000,0b10000,0b01110,0b00001,0b00001,0b11110}},
    {'T', {0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b00100}},
    {'U', {0b10001,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110}},
    {'V', {0b10001,0b10001,0b10001,0b10001,0b10001,0b01010,0b00100}},
    {'W', {0b10001,0b10001,0b10001,0b10101,0b10101,0b10101,0b01010}},
    {'X', {0b10001,0b10001,0b01010,0b00100,0b01010,0b10001,0b10001}},
    {'Y', {0b10001,0b10001,0b01010,0b00100,0b00100,0b00100,0b00100}},
    {'Z', {0b11111,0b00001,0b00010,0b00100,0b01000,0b10000,0b11111}},
    {'.', {0,0,0,0,0,0b01100,0b01100}},
    {',', {0,0,0,0,0b01100,0b01100,0b01000}},
    {':', {0,0b01100,0b01100,0,0b01100,0b01100,0}},
    {'-', {0,0,0,0b11111,0,0,0}},
    {'%', {0b11001,0b11010,0b00010,0b00100,0b01000,0b01011,0b10011}},
    {'+', {0,0b00100,0b00100,0b11111,0b00100,0b00100,0}},
    {'/', {0b00001,0b00010,0b00100,0b00100,0b01000,0b10000,0b10000}},
    {'#', {0b01010,0b01010,0b11111,0b01010,0b11111,0b01010,0b01010}},
};
#define FONT_COUNT (sizeof(k_font) / sizeof(k_font[0]))
#define GLYPH_W 5
#define GLYPH_H 7
#define GLYPH_SPACING 1

static const uint8_t *find_glyph(char c)
{
    for (size_t i = 0; i < FONT_COUNT; i++) {
        if (k_font[i].c == c) {
            return k_font[i].rows;
        }
    }
    return NULL; /* space / unknown -> blank cell */
}

/* Max scale this renderer supports, sized so the line buffer below stays a
 * small, fixed BSS allocation instead of a large stack array: one SPI
 * transaction covers a whole line instead of one per glyph pixel, which is
 * what makes text drawing fast enough to run every UI tick (see
 * docs/tuning.md - the original per-cell-fill_rect implementation could
 * take longer to draw one line than a whole UI frame period allows). */
#define GLYPH_MAX_SCALE 3
#define TEXT_BUF_MAX_W  DISPLAY_WIDTH
#define TEXT_BUF_MAX_H  (GLYPH_H * GLYPH_MAX_SCALE)
static uint8_t s_text_buf[TEXT_BUF_MAX_W * TEXT_BUF_MAX_H * 2];

int16_t display_draw_text(int16_t x, int16_t y, const char *str,
                           uint16_t fg, uint16_t bg, uint8_t scale)
{
    if (scale == 0) scale = 1;
    if (scale > GLYPH_MAX_SCALE) scale = GLYPH_MAX_SCALE;

    size_t char_count = strlen(str);
    int16_t glyph_w = GLYPH_W * scale;
    int16_t glyph_h = GLYPH_H * scale;
    int16_t advance = (GLYPH_W + GLYPH_SPACING) * scale;

    int16_t total_w = (int16_t)(char_count * advance);
    if (total_w > TEXT_BUF_MAX_W) {
        char_count = TEXT_BUF_MAX_W / advance;
        total_w = (int16_t)(char_count * advance);
    }
    if (x + total_w > DISPLAY_WIDTH) {
        int16_t max_w = DISPLAY_WIDTH - x;
        if (max_w < 0) max_w = 0;
        char_count = max_w / advance;
        total_w = (int16_t)(char_count * advance);
    }
    if (total_w <= 0 || glyph_h > TEXT_BUF_MAX_H) {
        return 0;
    }

    uint8_t fg_hi = fg >> 8, fg_lo = fg & 0xFF;
    uint8_t bg_hi = bg >> 8, bg_lo = bg & 0xFF;

    /* Render the whole line into s_text_buf (row-major, RGB565 big-endian
     * per pixel) before touching the SPI bus at all. */
    for (int16_t row = 0; row < glyph_h; row++) {
        uint8_t *dst_row = &s_text_buf[(size_t)row * total_w * 2];
        int glyph_row = row / scale;

        for (size_t ci = 0; ci < char_count; ci++) {
            char c = str[ci];
            if (c >= 'a' && c <= 'z') {
                c -= ('a' - 'A');
            }
            const uint8_t *rows = find_glyph(c);
            uint8_t bits = rows ? rows[glyph_row] : 0;
            uint8_t *dst_char = dst_row + (size_t)ci * glyph_w * 2;

            for (int16_t col = 0; col < glyph_w; col++) {
                bool on = (bits >> (GLYPH_W - 1 - (col / scale))) & 0x1;
                dst_char[col * 2]     = on ? fg_hi : bg_hi;
                dst_char[col * 2 + 1] = on ? fg_lo : bg_lo;
            }
        }
    }

    esp_err_t err = lcd_set_window(x, y, x + total_w - 1, y + glyph_h - 1);
    if (err != ESP_OK) {
        return 0;
    }
    gpio_set_level(YP_PIN_LCD_DC, 1);

    size_t total_bytes = (size_t)total_w * glyph_h * 2;
    size_t sent = 0;
    while (sent < total_bytes) {
        size_t chunk = total_bytes - sent;
        size_t max_chunk = DISPLAY_WIDTH * 2 * 4; /* matches bus max_transfer_sz */
        if (chunk > max_chunk) chunk = max_chunk;
        spi_transaction_t t = {
            .length = chunk * 8,
            .tx_buffer = s_text_buf + sent,
        };
        if (spi_device_polling_transmit(s_spi, &t) != ESP_OK) {
            break;
        }
        sent += chunk;
    }

    return total_w;
}
