/**
 * @file onboard_synth.c
 * @brief See onboard_synth.h.
 *
 * Signal path: square/pulse oscillator (fixed-point phase accumulator,
 * no trig, no float in the per-sample loop) -> linear Q15 amplitude
 * envelope (attack while gated, release once released) -> PDM output.
 * CC11 Expression modulates the oscillator's pulse width (10%..90%
 * duty) rather than a filter cutoff - see the file header in
 * onboard_synth.h for why a resonant filter was deliberately not
 * attempted here.
 *
 * Threading: onboard_synth_handle_event() is called from midi_task
 * (whatever task calls it); synth_task is this file's own render task.
 * The handful of shared scalars between them are guarded by a spinlock
 * (portMUX) rather than a full mutex - each critical section is a few
 * integer reads/writes, well within what a spinlock is for.
 */
#include <stdbool.h>
#include <string.h>
#include "onboard_synth.h"
#include "yp_board.h"
#include "voice_midi.h"
#include "driver/i2s_pdm.h"
#include "esp_rom_gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"

static const char *TAG = "onboard_synth";

#define SYNTH_SAMPLE_RATE_HZ   16000
#define SYNTH_BLOCK_SAMPLES    128
#define OSC_AMPLITUDE          16000  /* leaves headroom under int16 full scale */

#define SYNTH_TASK_STACK_BYTES 3072
#define SYNTH_TASK_PRIORITY    7   /* above midi_task (6): audible glitches
                                       from being preempted matter here in
                                       a way a delayed log line doesn't -
                                       but still well below the audio
                                       capture/DSP path (11-12) */

/* Q15 amplitude envelope step sizes, precomputed from the desired
 * attack/release times at this sample rate. */
#define ENV_ATTACK_MS   5
#define ENV_RELEASE_MS  80
#define ENV_ATTACK_STEP  (32767 / (SYNTH_SAMPLE_RATE_HZ * ENV_ATTACK_MS / 1000))
#define ENV_RELEASE_STEP (32767 / (SYNTH_SAMPLE_RATE_HZ * ENV_RELEASE_MS / 1000))

static i2s_chan_handle_t s_tx_handle;
static bool s_ready;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_phase_step;   /* Q32 phase increment for the current note's frequency */
static bool     s_gate;
static uint8_t  s_velocity;
static uint8_t  s_expression = 64; /* neutral (~50% duty) until the first CC11 arrives */
static int      s_active_note = -1;

static void synth_note_on(uint8_t note, uint8_t velocity)
{
    /* yp_midi_note_to_frequency() uses powf() - fine here, it runs once
     * per Note On/Change, not in the per-sample render loop below. */
    float freq = yp_midi_note_to_frequency(note);
    uint32_t step = (uint32_t)((double)freq * 4294967296.0 / (double)SYNTH_SAMPLE_RATE_HZ);

    portENTER_CRITICAL(&s_lock);
    s_phase_step = step;
    s_gate = true;
    s_velocity = velocity;
    s_active_note = note;
    portEXIT_CRITICAL(&s_lock);
}

static void synth_note_off(uint8_t note)
{
    portENTER_CRITICAL(&s_lock);
    if (s_active_note == note) {
        s_gate = false;
    }
    portEXIT_CRITICAL(&s_lock);
}

void onboard_synth_handle_event(const midi_event_t *event)
{
    if (!s_ready || !event) {
        return;
    }
    switch (event->type) {
        case MIDI_EVENT_NOTE_ON:
            synth_note_on(event->data1, (uint8_t)event->data2);
            break;
        case MIDI_EVENT_NOTE_OFF:
            synth_note_off(event->data1);
            break;
        case MIDI_EVENT_CC:
            if (event->data1 == YP_MIDI_CC_EXPRESSION) {
                portENTER_CRITICAL(&s_lock);
                s_expression = (uint8_t)event->data2;
                portEXIT_CRITICAL(&s_lock);
            }
            break;
        case MIDI_EVENT_PITCH_BEND:
        default:
            break; /* not rendered - this voice has no pitch-bend model */
    }
}

static void synth_task(void *arg)
{
    uint32_t phase = 0;
    int32_t amp = 0; /* Q15, current envelope level */
    int16_t buf[SYNTH_BLOCK_SAMPLES];

    while (1) {
        uint32_t phase_step;
        bool gate;
        uint8_t velocity, expression;

        portENTER_CRITICAL(&s_lock);
        phase_step = s_phase_step;
        gate = s_gate;
        velocity = s_velocity;
        expression = s_expression;
        portEXIT_CRITICAL(&s_lock);

        int32_t amp_target = gate ? ((int32_t)velocity * 32767 / 127) : 0;

        /* CC11 (0..127) -> pulse duty cycle, 100..900 per-mille (10%..90%).
         * Recomputed once per block, not per sample - expression changes
         * far slower than the sample rate. */
        uint32_t duty_permille = 100 + ((uint32_t)expression * 800) / 127;
        uint32_t duty_threshold = (uint32_t)(((uint64_t)duty_permille * (uint64_t)UINT32_MAX) / 1000);

        for (int i = 0; i < SYNTH_BLOCK_SAMPLES; i++) {
            phase += phase_step; /* uint32_t wraps on its own - that's the oscillator */
            int32_t osc = (phase < duty_threshold) ? OSC_AMPLITUDE : -OSC_AMPLITUDE;

            if (amp < amp_target) {
                amp += ENV_ATTACK_STEP;
                if (amp > amp_target) amp = amp_target;
            } else if (amp > amp_target) {
                amp -= ENV_RELEASE_STEP;
                if (amp < amp_target) amp = amp_target;
            }

            buf[i] = (int16_t)((osc * amp) >> 15);
        }

        size_t written = 0;
        i2s_channel_write(s_tx_handle, buf, sizeof(buf), &written, portMAX_DELAY);
    }
}

esp_err_t onboard_synth_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_handle, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_pdm_tx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(SYNTH_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_PDM_TX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = GPIO_NUM_NC,
            .dout = YP_PIN_PDM_P,
            .invert_flags = { .clk_inv = false },
        },
    };
    err = i2s_channel_init_pdm_tx_mode(s_tx_handle, &pdm_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_pdm_tx_mode failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx_handle);
        return err;
    }

    err = i2s_channel_enable(s_tx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx_handle);
        return err;
    }

    /* Differential PDM_N, mirrored+inverted from PDM_P - same technique
     * as self_test.c's boot melody (see its comments for the caveats);
     * safe to repeat here since that channel is fully torn down
     * (i2s_channel_disable + i2s_del_channel) before this ever runs. */
    PIN_FUNC_SELECT(IO_MUX_GPIO8_REG, PIN_FUNC_GPIO);
    gpio_set_direction(YP_PIN_PDM_N, GPIO_MODE_OUTPUT);
    esp_rom_gpio_connect_out_signal(YP_PIN_PDM_N, I2SO_SD_OUT_IDX, /*out_inv=*/true, /*oen_inv=*/false);

    gpio_set_level(YP_PIN_PA_CTL, 1);
    vTaskDelay(pdMS_TO_TICKS(5));

    BaseType_t ok = xTaskCreate(synth_task, "onboard_synth",
                                 SYNTH_TASK_STACK_BYTES / sizeof(StackType_t),
                                 NULL, SYNTH_TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create synth_task");
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    ESP_LOGI(TAG, "ready: %d Hz square/PWM voice on the board speaker", SYNTH_SAMPLE_RATE_HZ);
    return ESP_OK;
}
