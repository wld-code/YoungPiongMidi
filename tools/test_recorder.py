#!/usr/bin/env python3
"""
test_recorder.py - headless tests for recorder.py. Run with:
python3 tools/test_recorder.py
"""
import os
import sys
import tempfile
import wave

import numpy as np

from recorder import AudioRecorder

failures = []


def check(name, condition, detail=""):
    status = "PASS" if condition else "FAIL"
    print(f"[{status}] {name}" + (f" - {detail}" if detail and not condition else ""))
    if not condition:
        failures.append(name)


def test_inactive_recorder_ignores_writes():
    r = AudioRecorder(44100)
    r.write(np.ones(256, dtype=np.float32))
    audio = r.stop_and_get()
    check("write() before start() is ignored", len(audio) == 0)


def test_start_write_stop_concatenates_in_order():
    r = AudioRecorder(44100)
    r.start()
    check("active immediately after start()", r.active)
    block1 = np.full(100, 0.1, dtype=np.float32)
    block2 = np.full(100, 0.2, dtype=np.float32)
    r.write(block1)
    r.write(block2)
    audio = r.stop_and_get()
    check("inactive immediately after stop_and_get()", not r.active)
    check("captured length == sum of block lengths", len(audio) == 200, f"got {len(audio)}")
    check("blocks concatenated in write order",
          np.allclose(audio[:100], 0.1) and np.allclose(audio[100:], 0.2))


def test_stop_and_get_resets_for_next_recording():
    r = AudioRecorder(44100)
    r.start()
    r.write(np.ones(50, dtype=np.float32))
    first = r.stop_and_get()
    check("first recording captured", len(first) == 50)

    r.start()
    r.write(np.full(30, -1.0, dtype=np.float32))
    second = r.stop_and_get()
    check("second recording starts empty, not appended to the first",
          len(second) == 30 and np.allclose(second, -1.0), f"got {second}")


def test_write_while_inactive_between_recordings_is_ignored():
    r = AudioRecorder(44100)
    r.start()
    r.write(np.ones(10, dtype=np.float32))
    r.stop_and_get()
    r.write(np.ones(10, dtype=np.float32) * 5.0)  # not recording anymore
    r.start()
    audio = r.stop_and_get()
    check("a write() between stop and the next start() is dropped, not queued", len(audio) == 0)


def test_stop_and_get_on_never_started_recorder():
    r = AudioRecorder(44100)
    audio = r.stop_and_get()
    check("stop_and_get() on a never-started recorder returns empty, not an error", len(audio) == 0)


def test_save_wav_writes_valid_file():
    r = AudioRecorder(44100)
    n = 4410  # 0.1s
    t = np.arange(n) / 44100.0
    tone = (0.5 * np.sin(2 * np.pi * 440.0 * t)).astype(np.float32)
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "test.wav")
        written = r.save_wav(tone, path)
        check("save_wav returns the sample count written", written == n)
        check("save_wav actually created the file", os.path.exists(path))
        with wave.open(path, "rb") as wf:
            check("WAV is mono", wf.getnchannels() == 1)
            check("WAV is 16-bit", wf.getsampwidth() == 2)
            check("WAV sample rate matches the recorder's", wf.getframerate() == 44100)
            check("WAV frame count matches input length", wf.getnframes() == n,
                  f"got {wf.getnframes()}")
            raw = wf.readframes(wf.getnframes())
            pcm = np.frombuffer(raw, dtype=np.int16)
            check("WAV content is non-silent (actually contains the tone)",
                  np.max(np.abs(pcm)) > 1000, f"peak={np.max(np.abs(pcm))}")


def test_save_wav_clips_out_of_range_input():
    r = AudioRecorder(44100)
    loud = np.array([2.0, -2.0, 0.0], dtype=np.float32)
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "clip.wav")
        r.save_wav(loud, path)
        with wave.open(path, "rb") as wf:
            raw = wf.readframes(wf.getnframes())
            pcm = np.frombuffer(raw, dtype=np.int16)
            check("out-of-range samples are clipped, not wrapped/overflowed",
                  int(pcm[0]) > 30000 and int(pcm[1]) < -30000, f"got {pcm}")


def main():
    test_inactive_recorder_ignores_writes()
    test_start_write_stop_concatenates_in_order()
    test_stop_and_get_resets_for_next_recording()
    test_write_while_inactive_between_recordings_is_ignored()
    test_stop_and_get_on_never_started_recorder()
    test_save_wav_writes_valid_file()
    test_save_wav_clips_out_of_range_input()

    print()
    if failures:
        print(f"{len(failures)} check(s) FAILED:")
        for f in failures:
            print(f"  - {f}")
        sys.exit(1)
    print("ALL CHECKS PASSED")


if __name__ == "__main__":
    main()
