#!/usr/bin/env python3
"""
recorder.py - lock-protected audio recorder for Young Piong Synth
Studio (tools/synth_studio.py).

Kept independent of Tkinter and of sounddevice's callback machinery so
it can be unit-tested headlessly (tools/test_recorder.py): the audio
callback thread calls write() with every rendered block regardless of
whether recording is active (a cheap no-op check when it's not), and
the GUI thread calls start()/stop_and_get() in response to a button
click - the lock exists for exactly that producer/consumer split, the
same reasoning as SynthEngine's own lock in synth_instruments.py.
"""
import threading
import wave

import numpy as np


class AudioRecorder:
    def __init__(self, sample_rate: int):
        self.sample_rate = sample_rate
        self.lock = threading.Lock()
        self.active = False
        self._chunks = []

    def start(self):
        with self.lock:
            self._chunks = []
            self.active = True

    def write(self, block: np.ndarray):
        if not self.active:
            return
        with self.lock:
            if self.active:
                self._chunks.append(np.array(block, dtype=np.float32, copy=True))

    def stop_and_get(self) -> np.ndarray:
        """Stops recording (if active) and returns everything captured
        since the last start(), as float32 in [-1, 1]. Safe to call even
        if never started (returns an empty array)."""
        with self.lock:
            self.active = False
            if not self._chunks:
                return np.zeros(0, dtype=np.float32)
            audio = np.concatenate(self._chunks)
            self._chunks = []
            return audio

    def save_wav(self, audio: np.ndarray, path: str) -> int:
        """Writes `audio` (float32, [-1, 1]) as 16-bit PCM mono WAV at
        this recorder's sample rate. Returns the sample count written."""
        clipped = np.clip(audio, -1.0, 1.0)
        pcm16 = (clipped * 32767.0).astype(np.int16)
        with wave.open(path, "wb") as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)
            wf.setframerate(self.sample_rate)
            wf.writeframes(pcm16.tobytes())
        return len(audio)
