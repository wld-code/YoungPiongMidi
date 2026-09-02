/**
 * @file self_test.c
 * @brief See self_test.h.
 *
 * LCD test: fills the panel with solid RED/GREEN/BLUE/WHITE/BLACK in turn.
 * If the panel has power and a working SPI link at all, this is
 * impossible to miss - unlike small text, there is no font-rendering
 * bitmap that could be subtly wrong.
 *
 * Speaker test: plays a short melody over the PDM speaker output
 * (PA_CTL=GPIO1, PDM_P=GPIO7, PDM_N=GPIO8). PDM_P carries a standard
 * ESP-IDF I2S PDM TX (PCM2PDM) output; PDM_N is driven as a hardware-
 * inverted copy of the same serial bit via the GPIO matrix
 * (esp_rom_gpio_connect_out_signal(..., out_inv=true, ...)), which is the
 * differential-PDM wiring pattern Espressif's own (disabled, unshipped)
 * reference code uses for this exact board family. This path is
 * genuinely less certain than the LCD test - it is not exercised by any
 * shipping Espressif example for ESP-SensairShuttle - so it is treated as
 * best-effort: errors are logged and the boot continues either way.
 */
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "self_test.h"
#include "yp_board.h"
#include "yp_config.h"
#include "display.h"
#include "driver/i2s_pdm.h"
#include "esp_rom_gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"

static const char *TAG = "self_test";

/* -------------------------------------------------------------------- */
/*  LCD color cycle                                                       */
/* -------------------------------------------------------------------- */

static void lcd_color_cycle(void)
{
    struct { const char *name; uint16_t color; } steps[] = {
        { "RED",   DISPLAY_COLOR_RED },
        { "GREEN", DISPLAY_COLOR_GREEN },
        { "BLUE",  DISPLAY_COLOR_BLUE },
        { "WHITE", DISPLAY_COLOR_WHITE },
        { "BLACK", DISPLAY_COLOR_BLACK },
    };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        ESP_LOGI(TAG, "LCD self-test: full screen %s", steps[i].name);
        display_clear(steps[i].color);
        vTaskDelay(pdMS_TO_TICKS(400));
    }
}

/* -------------------------------------------------------------------- */
/*  Speaker melody                                                       */
/* -------------------------------------------------------------------- */

#if YP_SELF_TEST_SPEAKER_ENABLED

#define MELODY_SAMPLE_RATE_HZ  16000
#define MELODY_NOTE_MS         150
#define MELODY_GAP_MS          25
#define MELODY_AMPLITUDE       8000  /* of int16 full scale (32767) */

static const float k_melody_hz[] = {
    523.25f, 659.25f, 783.99f, 1046.50f, 783.99f, 659.25f, 523.25f,
};

static void fill_sine(int16_t *buf, size_t n, float freq_hz)
{
    for (size_t i = 0; i < n; i++) {
        float t = (float)i / (float)MELODY_SAMPLE_RATE_HZ;
        buf[i] = (int16_t)(MELODY_AMPLITUDE * sinf(2.0f * (float)M_PI * freq_hz * t));
    }
}

static void speaker_melody(void)
{
    esp_err_t err;

    i2s_chan_handle_t tx_handle = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    err = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "speaker self-test: i2s_new_channel failed: %s (skipping)", esp_err_to_name(err));
        return;
    }

    i2s_pdm_tx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(MELODY_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_PDM_TX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = GPIO_NUM_NC,
            .dout = YP_PIN_PDM_P,
            .invert_flags = { .clk_inv = false },
        },
    };
    err = i2s_channel_init_pdm_tx_mode(tx_handle, &pdm_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "speaker self-test: i2s_channel_init_pdm_tx_mode failed: %s (skipping)", esp_err_to_name(err));
        i2s_del_channel(tx_handle);
        return;
    }

    err = i2s_channel_enable(tx_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "speaker self-test: i2s_channel_enable failed: %s (skipping)", esp_err_to_name(err));
        i2s_del_channel(tx_handle);
        return;
    }

    /* PDM_P now carries the standard PCM2PDM bitstream. Mirror it onto
     * PDM_N, inverted, to form the differential pair this board's
     * amplifier expects - see the file header for why this is
     * best-effort. */
    PIN_FUNC_SELECT(IO_MUX_GPIO8_REG, PIN_FUNC_GPIO);
    gpio_set_direction(YP_PIN_PDM_N, GPIO_MODE_OUTPUT);
    esp_rom_gpio_connect_out_signal(YP_PIN_PDM_N, I2SO_SD_OUT_IDX, /*out_inv=*/true, /*oen_inv=*/false);

    gpio_set_level(YP_PIN_PA_CTL, 1); /* enable speaker amplifier */
    vTaskDelay(pdMS_TO_TICKS(5));     /* let the amp settle before driving it */

    ESP_LOGI(TAG, "speaker self-test: playing melody");

    size_t note_samples = MELODY_SAMPLE_RATE_HZ * MELODY_NOTE_MS / 1000;
    size_t gap_samples = MELODY_SAMPLE_RATE_HZ * MELODY_GAP_MS / 1000;
    int16_t *buf = malloc(note_samples * sizeof(int16_t));
    if (!buf) {
        ESP_LOGW(TAG, "speaker self-test: no memory for note buffer (skipping)");
    } else {
        int16_t *gap = calloc(gap_samples, sizeof(int16_t));
        for (size_t n = 0; n < sizeof(k_melody_hz) / sizeof(k_melody_hz[0]); n++) {
            fill_sine(buf, note_samples, k_melody_hz[n]);
            size_t written = 0;
            i2s_channel_write(tx_handle, buf, note_samples * sizeof(int16_t), &written, pdMS_TO_TICKS(500));
            if (gap) {
                i2s_channel_write(tx_handle, gap, gap_samples * sizeof(int16_t), &written, pdMS_TO_TICKS(200));
            }
        }
        free(gap);
        free(buf);
    }

    gpio_set_level(YP_PIN_PA_CTL, 0); /* back to the "unused" default state */
    i2s_channel_disable(tx_handle);
    i2s_del_channel(tx_handle);
}

#endif /* YP_SELF_TEST_SPEAKER_ENABLED */

/* -------------------------------------------------------------------- */

void self_test_run(void)
{
    ESP_LOGI(TAG, "=== boot self-test start ===");
    lcd_color_cycle();
#if YP_SELF_TEST_SPEAKER_ENABLED
    speaker_melody();
#else
    ESP_LOGI(TAG, "speaker self-test skipped (YP_SELF_TEST_SPEAKER_ENABLED=0 - "
                  "board audio disabled, use the PC-side tools instead)");
#endif
    ESP_LOGI(TAG, "=== boot self-test done ===");
}
