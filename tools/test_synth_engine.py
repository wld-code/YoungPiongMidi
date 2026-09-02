#!/usr/bin/env python3
"""
test_synth_engine.py - headless correctness/stability tests for
synth_instruments.SynthEngine.

Why this exists and runs the way it does: tools/synth_studio.py opens a
GUI window, which cannot be visually inspected by an automated agent.
What CAN be verified without a display is the thing that actually
matters for "does this work" - the audio engine underneath it never
produces NaN/Inf/runaway output, for every one of the 10 instruments,
under both normal and adversarial (rapid note-change, voice-stealing)
conditions. Run with: python3 tools/test_synth_engine.py
"""
import sys

import numpy as np

from synth_instruments import SynthEngine, INSTRUMENTS, MAX_VOICES

SAMPLE_RATE = 44100
BLOCK = 256

failures = []


def check(name, condition, detail=""):
    status = "PASS" if condition else "FAIL"
    print(f"[{status}] {name}" + (f" - {detail}" if detail and not condition else ""))
    if not condition:
        failures.append(name)


def render_seconds(engine: SynthEngine, seconds: float) -> np.ndarray:
    n_blocks = int(seconds * SAMPLE_RATE / BLOCK) + 1
    chunks = [engine.render(BLOCK) for _ in range(n_blocks)]
    return np.concatenate(chunks)


def test_each_instrument_clean_note():
    for inst_id, name, _ in INSTRUMENTS:
        engine = SynthEngine(SAMPLE_RATE)
        engine.set_instrument(inst_id)
        engine.note_on(57, 100)   # A3
        audio = render_seconds(engine, 0.3)
        engine.note_off(57)
        audio = np.concatenate([audio, render_seconds(engine, 0.6)])  # let release finish

        finite = np.all(np.isfinite(audio))
        peak = float(np.max(np.abs(audio))) if finite else float("inf")
        check(f"instrument {inst_id} ({name}): finite output", finite)
        check(f"instrument {inst_id} ({name}): peak <= 1.0 (post-clip)", finite and peak <= 1.0 + 1e-6,
              f"peak={peak}")
        check(f"instrument {inst_id} ({name}): produces audible sound", finite and peak > 0.01,
              f"peak={peak} (suspiciously silent)")


def test_expression_sweep():
    for inst_id, name, _ in INSTRUMENTS:
        engine = SynthEngine(SAMPLE_RATE)
        engine.set_instrument(inst_id)
        engine.note_on(69, 90)
        audio_chunks = []
        for val in range(0, 128, 4):
            engine.cc(11, val)
            audio_chunks.append(engine.render(BLOCK))
        engine.note_off(69)
        audio_chunks.append(render_seconds(engine, 0.3))
        audio = np.concatenate(audio_chunks)
        finite = np.all(np.isfinite(audio))
        peak = float(np.max(np.abs(audio))) if finite else float("inf")
        check(f"instrument {inst_id} ({name}): stable across full CC11 sweep", finite and peak <= 1.0 + 1e-6,
              f"peak={peak}")


def test_rapid_note_changes_and_voice_stealing():
    """Adversarial: hammer note on/off far faster than any envelope can
    settle, across all instruments in rotation, deliberately exceeding
    MAX_VOICES concurrently held notes to force voice stealing."""
    engine = SynthEngine(SAMPLE_RATE)
    rng = np.random.default_rng(1234)
    all_finite = True
    max_peak = 0.0
    held = []
    for step in range(4000):
        engine.set_instrument(int(rng.integers(0, len(INSTRUMENTS))))
        note = int(rng.integers(40, 84))
        vel = int(rng.integers(1, 128))
        engine.note_on(note, vel)
        held.append(note)
        if len(held) > MAX_VOICES + 3 and rng.random() < 0.5:
            engine.note_off(held.pop(0))
        if step % 7 == 0:
            engine.cc(11, int(rng.integers(0, 128)))
        block = engine.render(BLOCK // 4)
        if not np.all(np.isfinite(block)):
            all_finite = False
            break
        max_peak = max(max_peak, float(np.max(np.abs(block))))
    for n in list(held):
        engine.note_off(n)
    tail = render_seconds(engine, 0.5)
    tail_finite = np.all(np.isfinite(tail))
    check("stress test: all output finite under rapid note-change/voice-stealing", all_finite and tail_finite)
    check("stress test: peak stays within clip bound", max_peak <= 1.0 + 1e-6, f"max_peak={max_peak}")


def test_instrument_switch_mid_session_does_not_affect_sounding_note():
    """Switching the selected instrument must not retroactively change a
    note that's already sounding (standard, expected synth behavior)."""
    engine = SynthEngine(SAMPLE_RATE)
    engine.set_instrument(0)
    engine.note_on(60, 100)
    engine.render(BLOCK)  # let the voice actually start
    active_instrument_before = next(v.instrument for v in engine.voices if v.active)
    engine.set_instrument(5)
    engine.render(BLOCK)
    active_instrument_after = next(v.instrument for v in engine.voices if v.active)
    check("switching instrument mid-note leaves the already-sounding voice untouched",
          active_instrument_before == active_instrument_after == 0)


def test_engine_silent_at_rest():
    engine = SynthEngine(SAMPLE_RATE)
    audio = render_seconds(engine, 0.2)
    check("engine renders exact silence with no notes held", np.max(np.abs(audio)) == 0.0)


def main():
    print(f"Testing {len(INSTRUMENTS)} instruments, MAX_VOICES={MAX_VOICES}, sr={SAMPLE_RATE}\n")
    test_engine_silent_at_rest()
    test_each_instrument_clean_note()
    test_expression_sweep()
    test_instrument_switch_mid_session_does_not_affect_sounding_note()
    test_rapid_note_changes_and_voice_stealing()

    print()
    if failures:
        print(f"{len(failures)} check(s) FAILED:")
        for f in failures:
            print(f"  - {f}")
        sys.exit(1)
    else:
        print("ALL CHECKS PASSED")


if __name__ == "__main__":
    main()
