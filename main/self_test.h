/**
 * @file self_test.h
 * @brief Boot-time hardware proof-of-life: an unmistakable LCD color cycle
 *        plus an audible speaker melody, run once before the normal UI/
 *        audio pipeline starts.
 *
 * This exists purely for hardware bring-up (see docs/hardware.md /
 * docs/tuning.md for why it was added) - it is not part of the
 * voice-to-MIDI signal chain and touches no shared state with it. Safe to
 * remove once the board's display and speaker wiring are confirmed good.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Flash the LCD through solid colors and play a short melody.
 *
 * Call once, after yp_board_init() and display_init(), before starting the
 * regular dsp_task/ui_task/audio_capture pipeline. Blocking; takes a
 * couple of seconds. The speaker portion is best-effort: PDM_N's
 * differential-inversion wiring is not exercised anywhere else in this
 * codebase, so failures there are logged, not fatal - the LCD portion of
 * the test is the more solid diagnostic of the two (see the header
 * comment in self_test.c).
 */
void self_test_run(void);

#ifdef __cplusplus
}
#endif
