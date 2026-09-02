/**
 * @file yp_board.h
 * @brief Pin definitions and low-level board bring-up for the Espressif
 *        ESP-SensairShuttle v1.0 (ESP32-C5-WROOM-1-N16R8, 16 MB flash,
 *        8 MB PSRAM).
 *
 * Every GPIO number below is taken from the official Espressif
 * "ESP-SensairShuttle v1.0" user guide and cross-checked against the
 * `board_devices.yaml` / `board_peripherals.yaml` / `setup_device.c` board
 * support files published by Espressif for this exact board in
 * espressif/esp-dev-kits (examples/esp-sensairshuttle/examples/factory_demo).
 * See docs/hardware.md for the full verification trail and sources.
 *
 * Do NOT add a pin here from guesswork. If a pin is not documented,
 * leave it undefined and say so in docs/hardware.md.
 */
#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "hal/adc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------- */
/*  Microphone / analog audio input                                      */
/* -------------------------------------------------------------------- */

/** Analog microphone input pin. The signal is already amplified by the
 *  board's analog front-end ahead of the ESP32-C5 ADC. */
#define YP_PIN_MIC_ADC              GPIO_NUM_6

/** ADC unit/channel corresponding to YP_PIN_MIC_ADC on ESP32-C5. */
#define YP_MIC_ADC_UNIT             ADC_UNIT_1
#define YP_MIC_ADC_CHANNEL          ADC_CHANNEL_5

/* -------------------------------------------------------------------- */
/*  Speaker / PDM audio output (not used by the voice-to-MIDI signal      */
/*  chain, reserved here so the pins are never accidentally reused)       */
/* -------------------------------------------------------------------- */

#define YP_PIN_PA_CTL               GPIO_NUM_1   /* active high enables PA */
#define YP_PIN_PDM_P                GPIO_NUM_7
#define YP_PIN_PDM_N                GPIO_NUM_8

/* -------------------------------------------------------------------- */
/*  LCD: ST7789P3, 1.83", 240x284, 4-wire SPI                            */
/* -------------------------------------------------------------------- */

#define YP_PIN_LCD_SDA              GPIO_NUM_23  /* SPI MOSI */
#define YP_PIN_LCD_SCL              GPIO_NUM_24  /* SPI SCLK */
#define YP_PIN_LCD_CS               GPIO_NUM_25
#define YP_PIN_LCD_DC               GPIO_NUM_26
#define YP_PIN_LCD_PWR              GPIO_NUM_5   /* panel power rail switch */

/** No hardware RESET pin is broken out to the ESP32-C5 for this panel
 *  (confirmed against Espressif's own board_devices.yaml, which sets
 *  reset_gpio_num = -1). Reset is performed in software (command 0x01)
 *  after the power rail is enabled. */
#define YP_LCD_HAS_RESET_PIN        0

#define YP_LCD_SPI_HOST             SPI2_HOST
#define YP_LCD_SPI_CLOCK_HZ         (20 * 1000 * 1000)  /* verified working
                                                            on this board by
                                                            Espressif's own
                                                            factory firmware */
#define YP_LCD_SPI_MODE             3

/* -------------------------------------------------------------------- */
/*  User input                                                           */
/* -------------------------------------------------------------------- */

#define YP_PIN_BOOT_BUTTON          GPIO_NUM_28

/* -------------------------------------------------------------------- */
/*  Board-level init                                                      */
/* -------------------------------------------------------------------- */

/**
 * @brief Configure pins that are board-global and not owned by a specific
 *        peripheral driver (boot button, PA_CTL default-off state).
 *
 * Peripheral-owning components (audio_capture, display) configure their own
 * pins internally; this only sets up what is left over so main.c has a
 * single, obvious place to call for "bring the board to a known state".
 */
esp_err_t yp_board_init(void);

/**
 * @brief Read the boot button state.
 * @return true if pressed.
 */
bool yp_board_button_pressed(void);

#ifdef __cplusplus
}
#endif
