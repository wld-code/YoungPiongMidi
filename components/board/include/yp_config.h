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

/** High-pass filter cutoff used to remove DC and rumble before analysis.
 *  Implemented as a 2nd-order (biquad) IIR, not a 1-pole filter - a
 *  1-pole filter's gentle -6dB/octave rolloff barely dented real
 *  mechanical/handling noise from touching the microphone (which is
 *  broadband but strongest at very low frequency); the -12dB/octave
 *  rolloff of a proper 2nd-order filter is a real, measured-necessary
 *  strengthening, not a stylistic choice - see docs/tuning.md. Q is
 *  fixed at 1/sqrt(2) (Butterworth - maximally flat passband), the
 *  standard choice for a single biquad section, so it isn't a separate
 *  macro. */
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

/** Run the (expensive) YIN analysis once every this many hops, rather
 *  than every hop. The analysis window still slides every hop (no audio
 *  is skipped), only the O(window x tau_range) computation itself is
 *  decimated - reasonable because pitch does not need to update at the
 *  8ms hop rate: even fast vocal vibrato is ~5-8 Hz, far below the
 *  ~1000/3 = 333 Hz+ effective update rate this still gives. Needed in
 *  practice, not just in theory: measured on real ESP32-C5 hardware
 *  (no hardware FPU - see yin.c's header comment), the fixed-point YIN
 *  computation itself costs ~13ms average / ~17ms worst case, still over
 *  the raw 8ms hop budget on its own. See docs/tuning.md. */
#define YP_PITCH_UPDATE_STRIDE_HOPS    3

/* -------------------------------------------------------------------- */
/*  Voice activity / envelope                                            */
/* -------------------------------------------------------------------- */

/** Number of consecutive frames above threshold required to declare voice
 *  active, and below threshold required to declare silence. Debounces
 *  short transients. Works together with the adaptive noise gate below -
 *  this handles "don't trust one frame", the gate handles "what counts
 *  as loud enough to trust at all". */
#define YP_VAD_ATTACK_FRAMES           2
#define YP_VAD_RELEASE_FRAMES          4

/** Envelope follower time constants, in milliseconds. */
#define YP_ENVELOPE_ATTACK_MS          8.0f
#define YP_ENVELOPE_RELEASE_MS         120.0f

/* -------------------------------------------------------------------- */
/*  Adaptive noise gate                                                  */
/* -------------------------------------------------------------------- */

/** Real hardware/rooms have a real, non-zero, non-constant noise floor
 *  (electrical hum, fan noise, handling the microphone) - a *fixed*
 *  RMS threshold either lets that noise through (set too low) or misses
 *  quiet real speech (set too high), and cannot adapt if the ambient
 *  level changes. Confirmed on real hardware: touching the microphone,
 *  or just ambient room noise, was crossing a fixed threshold and
 *  generating real MIDI events with no one singing - see docs/tuning.md.
 *
 *  The fix is the same one a studio noise gate or a radio squelch uses:
 *  a slow follower continuously tracks the ambient floor, and the
 *  actual gate threshold is that floor times a margin, not a constant.
 *  See noise_gate.c. */

/** Time constant (ms) for the floor tracker adapting DOWNWARD, toward a
 *  quieter room - fast enough to find true silence within about this
 *  long after it starts. */
#define YP_NOISE_GATE_DOWN_TIME_MS     1000.0f

/** Time constant (ms) for the floor tracker adapting UPWARD -
 *  deliberately much slower than the down time constant, and only
 *  applied while the gate is currently *closed* (see noise_gate.c), so
 *  a loud, real voice can never drag the floor up. Still lets the floor
 *  eventually follow a genuinely noisier environment (e.g. an AC unit
 *  turning on) rather than staying stuck low forever. */
#define YP_NOISE_GATE_UP_TIME_MS       30000.0f

/** How far above the tracked noise floor the signal must rise before
 *  the gate opens, as a linear ratio (not dB - ~2.5x is approximately
 *  +8 dB). Higher = more resistant to noise, but also to quiet speech. */
#define YP_NOISE_GATE_MARGIN_RATIO     2.5f

/** Absolute floor safety clamp: the adaptive floor is never allowed to
 *  track below this, so the gate threshold can't collapse toward zero
 *  during genuine silence and become hypersensitive to any tiny noise. */
#define YP_NOISE_GATE_MIN_FLOOR        0.006f

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

/* Transports. Both start disabled: BLE MIDI (Milestone 8) and DIN MIDI
 * over UART (Milestone 9, additionally blocked on a confirmed TX pin -
 * see docs/hardware.md) are not implemented yet. Until at least one of
 * these is 1, midi_task's only "transport" is a diagnostic log line per
 * event - see docs/midi.md. Flip YP_MIDI_BLE_ENABLED to 1 once
 * components/midi/midi_ble.c exists. */
#define YP_MIDI_BLE_ENABLED            0
#define YP_MIDI_UART_ENABLED           0
#define YP_MIDI_UART_BAUD              31250

#define YP_BLE_DEVICE_NAME             "YoungPiongMidi"

/* The board's own speaker voice (components/midi/onboard_synth.c) -
 * see that file's header comment for what it is. Set to 0 to keep the
 * board silent and use only the PC-side output instead: tools/
 * synth_studio.py (Young Piong Synth Studio, 10 instruments + a
 * sequencer) or tools/acid_synth_monitor.py, both of which read the
 * exact same MIDI event log this board always produces regardless of
 * this flag - disabling onboard sound does not disable MIDI, only this
 * one local playback path. When 0, onboard_synth_init() is never
 * called: the PDM/I2S output is never configured at all (not just
 * muted), so there is no PDM clock activity on GPIO1/7/8 either. */
#define YP_ONBOARD_SYNTH_ENABLED       0

/* The boot self-test's speaker melody (main/self_test.c) - a one-shot
 * hardware check, unrelated to and independent of YP_ONBOARD_SYNTH_ENABLED
 * above, but still real board audio output. Set to 0 along with it to
 * keep the board fully silent at boot too; the LCD half of the same
 * self-test (self_test.c's lcd_color_cycle()) is unaffected - it's
 * silent already and still worth running. */
#define YP_SELF_TEST_SPEAKER_ENABLED   0

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
