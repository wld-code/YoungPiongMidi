/**
 * @file yp_config.h
 * @brief Central, compile-time configuration for YoungPiongMidi.
 *
 * Every tunable constant used by more than one module lives here so that
 * behaviour can be adjusted without hunting for magic numbers scattered
 * across components. Values are grouped by subsystem and documented with
 * their unit and valid range where relevant.
 *
 * Rule of thumb: if you are tempted to write a literal number in a .c file
 * for anything that affects timing, thresholds or protocol behaviour, add a
 * macro here instead and use that.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------- */
/*  Audio acquisition                                                    */
/* -------------------------------------------------------------------- */

/** ADC sample rate in Hz. Voice fundamentals of interest (80-1000 Hz) are
 *  well below Nyquist at 16 kHz, leaving headroom for the anti-alias
 *  behaviour of the analog front-end on ESP-SensairShuttle. */
#define YP_AUDIO_SAMPLE_RATE_HZ        16000

/** Number of samples analysed per DSP frame. */
#define YP_AUDIO_FRAME_SIZE            512

/** Number of samples the analysis window advances between frames.
 *  YP_AUDIO_HOP_SIZE < YP_AUDIO_FRAME_SIZE gives overlapping analysis
 *  windows, which reduces pitch-detection latency versus frame size alone. */
#define YP_AUDIO_HOP_SIZE              128

/** Number of ADC conversion results the continuous-mode driver DMA's per
 *  interrupt. Kept a multiple of YP_AUDIO_HOP_SIZE so the capture task can
 *  hand off whole hops without splitting DMA buffers. */
#define YP_AUDIO_DMA_FRAME_SAMPLES     128

/** Number of DMA-sized buffers queued by the ADC continuous driver. */
#define YP_AUDIO_DMA_BUFFER_COUNT      4

/** ADC attenuation. The microphone signal is already amplified by the
 *  board's analog front-end before reaching GPIO6/ADC1 channel 5, so a
 *  moderate attenuation (ADC_ATTEN_DB_12, full ~3.3 V range) is used rather
 *  than assuming a small-signal input. Verify against real signal levels
 *  during Milestone 1 bring-up and adjust if clipping or poor resolution is
 *  observed. */
#define YP_AUDIO_ADC_ATTEN             ADC_ATTEN_DB_12

/** High-pass filter cutoff used to remove DC and rumble before analysis. */
#define YP_AUDIO_HPF_CUTOFF_HZ         60.0f

/** Optional low-pass filter cutoff (anti-alias / hiss reduction) applied
 *  ahead of pitch detection. */
#define YP_AUDIO_LPF_CUTOFF_HZ         2000.0f

/** Clipping detection threshold as a fraction of full-scale (0..1). */
#define YP_AUDIO_CLIP_THRESHOLD        0.98f

/* -------------------------------------------------------------------- */
/*  Pitch detection                                                      */
/* -------------------------------------------------------------------- */

/** Monophonic voice pitch search range. */
#define YP_PITCH_MIN_HZ                80.0f
#define YP_PITCH_MAX_HZ                1000.0f

/** YIN difference-function threshold. Lower = stricter periodicity
 *  requirement, fewer false positives, more missed low-confidence notes. */
#define YP_PITCH_YIN_THRESHOLD         0.15f

/** Minimum confidence (0..1, 1 - YIN's normalized minimum) required before
 *  a pitch estimate is considered usable by the note state machine. */
#define YP_PITCH_CONFIDENCE_THRESHOLD  0.55f

/* -------------------------------------------------------------------- */
/*  Voice activity / envelope                                            */
/* -------------------------------------------------------------------- */

/** RMS (0..1 normalized) below which the signal is considered silence. */
#define YP_VAD_RMS_THRESHOLD           0.02f

/** Number of consecutive frames above threshold required to declare voice
 *  active, and below threshold required to declare silence. Debounces
 *  short transients. */
#define YP_VAD_ATTACK_FRAMES           2
#define YP_VAD_RELEASE_FRAMES          4

/** Envelope follower time constants, in milliseconds. */
#define YP_ENVELOPE_ATTACK_MS          8.0f
#define YP_ENVELOPE_RELEASE_MS         120.0f

/* -------------------------------------------------------------------- */
/*  Note stabilization state machine                                     */
/* -------------------------------------------------------------------- */

/** Number of consecutive stable (same MIDI note, confident) pitch frames
 *  required before a NOTE_ON is emitted. */
#define YP_NOTE_MIN_STABLE_FRAMES      3

/** Minimum time a note must remain active before it can be replaced by a
 *  new note, in milliseconds. Prevents rapid note flicker. */
#define YP_NOTE_MIN_DURATION_MS        60

/** Pitch must move by at least this many semitones (persistently) to be
 *  treated as a note change rather than vibrato/jitter around the current
 *  note. */
#define YP_NOTE_CHANGE_TOLERANCE_ST    0.6f

/** Consecutive silent/unvoiced frames required before NOTE_OFF is sent. */
#define YP_NOTE_RELEASE_FRAMES         6

/* -------------------------------------------------------------------- */
/*  Dynamics -> MIDI velocity / expression mapping                        */
/* -------------------------------------------------------------------- */

/** RMS values at/below this are treated as noise floor (velocity ~ 0). */
#define YP_DYNAMICS_NOISE_FLOOR        0.015f

/** RMS value mapped to maximum velocity/expression (127). Voices above
 *  this are clamped, not remapped further. */
#define YP_DYNAMICS_MAX_RMS            0.55f

typedef enum {
    YP_VELOCITY_CURVE_LINEAR = 0,
    YP_VELOCITY_CURVE_LOG    = 1,
} yp_velocity_curve_t;

/** Default velocity mapping curve. Logarithmic tracks perceived loudness
 *  (roughly proportional to dB) better than a raw linear RMS mapping. */
#define YP_DEFAULT_VELOCITY_CURVE      YP_VELOCITY_CURVE_LOG

#define YP_MIDI_VELOCITY_MIN           1
#define YP_MIDI_VELOCITY_MAX           127

/* -------------------------------------------------------------------- */
/*  CC11 (Expression) streaming                                          */
/* -------------------------------------------------------------------- */

#define YP_CC11_ENABLED                1

/** Minimum change in CC value (0..127) before a new CC11 message is sent. */
#define YP_CC11_MIN_DELTA              2

/** Minimum time between CC11 messages, in milliseconds, even if the value
 *  is changing continuously. Bounds MIDI bandwidth usage. */
#define YP_CC11_MIN_INTERVAL_MS        20

/* -------------------------------------------------------------------- */
/*  Pitch bend (Milestone 10, disabled by default)                       */
/* -------------------------------------------------------------------- */

#define YP_PITCH_BEND_ENABLED          0

/** Pitch bend range assumed on the receiving synth, in semitones (+/-). */
#define YP_PITCH_BEND_RANGE_ST         2

/** Pitch bend smoothing time constant, in milliseconds. */
#define YP_PITCH_BEND_SMOOTH_MS        30.0f

/* -------------------------------------------------------------------- */
/*  MIDI                                                                  */
/* -------------------------------------------------------------------- */

#define YP_MIDI_CHANNEL                0   /* 0-based: channel 1 */
#define YP_MIDI_CC_EXPRESSION          11

/** MIDI event queue depth between voice_midi_task and midi_task. */
#define YP_MIDI_QUEUE_LENGTH           32

/* Transports. BLE MIDI is the default/primary transport; DIN MIDI over
 * UART is optional and off until a UART TX pin is confirmed for a given
 * hardware revision (see docs/hardware.md). */
#define YP_MIDI_BLE_ENABLED            1
#define YP_MIDI_UART_ENABLED           0
#define YP_MIDI_UART_BAUD              31250

#define YP_BLE_DEVICE_NAME             "YoungPiongMidi"

/* -------------------------------------------------------------------- */
/*  Display                                                               */
/* -------------------------------------------------------------------- */

#define YP_LCD_ENABLED                 1

/** Panel geometry, native (portrait) orientation, MADCTL = 0x00. */
#define YP_LCD_WIDTH                   240
#define YP_LCD_HEIGHT                  284

/** UI refresh rate, Hz. Deliberately decoupled from the DSP loop. */
#define YP_UI_REFRESH_RATE_HZ          15

/* -------------------------------------------------------------------- */
/*  Diagnostics                                                          */
/* -------------------------------------------------------------------- */

/** Compile-time switch for verbose per-frame voice analysis logging.
 *  Independent of esp_log's runtime level so it can be stripped from
 *  release builds entirely. */
#ifndef YP_DEBUG_VOICE_LOG
#define YP_DEBUG_VOICE_LOG             1
#endif

/** Minimum time between diagnostic log lines, in milliseconds, so the
 *  console stays readable at 16 kHz / 512-sample frame rates. */
#define YP_DEBUG_LOG_INTERVAL_MS       200

/** Minimum time between latency-statistics log lines, in milliseconds. */
#define YP_DEBUG_STATS_INTERVAL_MS     2000

#ifdef __cplusplus
}
#endif
