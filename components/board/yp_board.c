#include "yp_board.h"
#include "esp_log.h"

static const char *TAG = "yp_board";

esp_err_t yp_board_init(void)
{
    esp_err_t err;

    /* Boot button: standard ESP32 BOOT button wiring is active-low with an
     * external/internal pull-up. Enable the internal pull-up defensively;
     * it is a no-op if the board also has one. */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << YP_PIN_BOOT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&btn_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "boot button gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Speaker PA is not used by the voice-to-MIDI signal chain. Drive
     * PA_CTL low explicitly so the amplifier stays off and does not draw
     * power or inject noise while unused. */
    gpio_config_t pa_cfg = {
        .pin_bit_mask = 1ULL << YP_PIN_PA_CTL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&pa_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PA_CTL gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }
    gpio_set_level(YP_PIN_PA_CTL, 0);

    ESP_LOGI(TAG, "board init done (ESP-SensairShuttle v1.0 / ESP32-C5-WROOM-1-N16R8)");
    return ESP_OK;
}

bool yp_board_button_pressed(void)
{
    return gpio_get_level(YP_PIN_BOOT_BUTTON) == 0;
}
