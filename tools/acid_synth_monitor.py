#!/usr/bin/env python3
"""
acid_synth_monitor.py - real-time audio monitor for YoungPiongMidi.

Reads the firmware's serial console, parses the MIDI events midi_task
logs (NOTE_ON/NOTE_OFF/CC - see components/midi/midi.c's log_event()),
and plays them live through a small monophonic "acid" (TB-303-style)
synthesizer: sawtooth oscillator -> resonant lowpass filter, with the
filter's classic "squelch" envelope retriggered on every Note On, note
velocity driving accent (extra resonance/sweep), and CC11 Expression
additionally sweeping the filter cutoff.

Why this exists: YoungPiongMidi has no wire MIDI transport yet (BLE/UART
are Milestones 8-9 - see docs/midi.md), so the only way to *hear* what
the note-stabilization state machine and CC11 expression tracker are
actually deciding, in real time, is to listen to what they log and turn
that back into sound on the host. This is a development/verification
tool, not a project deliverable in its own right - it does not touch or
depend on any change to the firmware.

Usage:
    python3 tools/acid_synth_monitor.py [--port /dev/cu.usbmodemXXXX]

Requires: pyserial, numpy, sounddevice (`pip install pyserial numpy
sounddevice`).
"""
import argparse
import glob
import math
import re
import sys
import threading
import time

import numpy as np
import sounddevice as sd
import serial

SAMPLE_RATE = 44100
BLOCK_SIZE = 256

# --- Acid voice tuning -----------------------------------------------
GLIDE_MS = 45.0          # portamento time constant between notes
AMP_ATTACK_MS = 4.0      # fresh note (from silence) attack
AMP_RELEASE_MS = 90.0    # note off release
FILTER_ENV_DECAY_MS = 220.0   # base "squelch" decay time
FILTER_ENV_DECAY_ACCENT_MS = 420.0  # decay time at full velocity (accent = longer sweep)
BASE_CUTOFF_HZ = 200.0
FILTER_ENV_AMOUNT_HZ = 2200.0     # how far the squelch envelope opens the filter
EXPRESSION_AMOUNT_HZ = 900.0      # how far CC11 sweeps the filter on top of that
# The naive Chamberlin SVF below is only numerically stable while
# comfortably clear of f=2*sin(pi*cutoff/sr) -> 2 *and* while resonance
# isn't pushed so high (q so low) that transient coefficient changes
# (this filter is modulated every sample - cutoff sweeps with the
# envelope/expression, not just note to note) can't inject more energy
# than the state variables bleed off. 4000 Hz / q>=1.0 stays well clear
# of that - measured empirically (see MAX_STATE clamp below, which is a
# second, independent line of defense: if you tighten these further and
# still see loud/clicky output, that clamp is silently saving you from
# what would otherwise be inf/NaN, which is a sign to loosen it further).
MAX_CUTOFF_HZ = 4000.0
BASE_RESONANCE_Q = 2.2    # SVF q; higher number = LESS resonant (see chamberlin_svf)
ACCENT_RESONANCE_Q = 1.0  # q at full accent (more resonant/squealy)
MAX_STATE = 8.0           # hard safety clamp on filter state, see above
MASTER_GAIN = 0.35


def midi_note_to_freq(note: int) -> float:
    return 440.0 * (2.0 ** ((note - 69) / 12.0))


class SharedMidiState:
    """Updated by the serial-reading thread, read by the audio callback.
    A plain lock is plenty here: updates are rare (tens of Hz at most)
    compared to the audio block rate."""

    def __init__(self):
        self.lock = threading.Lock()
        self.target_freq = midi_note_to_freq(60)
        self.gate = False
        self.fresh_attack = False   # True: real attack; False: legato slide
        self.retrigger_filter = False
        self.velocity = 0
        self.expression = 0.0       # 0..1, from CC11
        self.note_number = None

    def note_on(self, note: int, velocity: int):
        with self.lock:
            self.target_freq = midi_note_to_freq(note)
            self.fresh_attack = not self.gate  # slide if a note was already held
            self.gate = True
            self.retrigger_filter = True
            self.velocity = velocity
            self.note_number = note

    def note_off(self, note: int):
        with self.lock:
            if self.note_number == note:
                self.gate = False

    def cc(self, controller: int, value: int):
        if controller == 11:  # Expression
            with self.lock:
                self.expression = value / 127.0

    def snapshot_and_clear_triggers(self):
        with self.lock:
            fresh = self.fresh_attack
            retrig = self.retrigger_filter
            self.fresh_attack = False
            self.retrigger_filter = False
            return (self.target_freq, self.gate, fresh, retrig,
                    self.velocity, self.expression)


class AcidVoice:
    """Monophonic sawtooth -> Chamberlin state-variable resonant lowpass,
    with a fast-decay filter envelope (the acid "squelch") and a
    conventional amplitude envelope. Pure Python per-sample loop inside
    the audio callback - trivially fast enough for one voice."""

    def __init__(self, sample_rate: int):
        self.sr = sample_rate
        self.phase = 0.0
        self.current_freq = 110.0
        self.amp = 0.0
        self.filter_env = 0.0
        self.filter_env_decay_coeff = 0.0
        self.svf_low = 0.0
        self.svf_band = 0.0
        self.accent = 0.0  # 0..1, latched per note from velocity

        self.glide_coeff = math.exp(-1.0 / (GLIDE_MS * 0.001 * self.sr))
        self.amp_attack_coeff = math.exp(-1.0 / (AMP_ATTACK_MS * 0.001 * self.sr))
        self.amp_release_coeff = math.exp(-1.0 / (AMP_RELEASE_MS * 0.001 * self.sr))

    def render(self, n: int, state: SharedMidiState) -> np.ndarray:
        target_freq, gate, fresh, retrig, velocity, expression = \
            state.snapshot_and_clear_triggers()

        if retrig:
            self.accent = max(0.0, min(1.0, velocity / 127.0))
            self.filter_env = 1.0
            decay_ms = FILTER_ENV_DECAY_MS + self.accent * (
                FILTER_ENV_DECAY_ACCENT_MS - FILTER_ENV_DECAY_MS)
            self.filter_env_decay_coeff = math.exp(-1.0 / (decay_ms * 0.001 * self.sr))
        if fresh:
            self.amp = 0.0  # force a real attack ramp from 0, not a slide

        out = np.empty(n, dtype=np.float32)

        for i in range(n):
            # Portamento glide toward the target note.
            self.current_freq += (target_freq - self.current_freq) * (1.0 - self.glide_coeff)

            # Sawtooth oscillator, -1..1.
            self.phase += self.current_freq / self.sr
            if self.phase >= 1.0:
                self.phase -= 1.0
            saw = 2.0 * self.phase - 1.0

            # Amplitude envelope: attack toward 1 while gated, release
            # toward 0 once gate drops. A slide (legato) never resets
            # self.amp, so consecutive slid notes stay connected.
            amp_target = 1.0 if gate else 0.0
            coeff = self.amp_attack_coeff if gate else self.amp_release_coeff
            self.amp += (amp_target - self.amp) * (1.0 - coeff)

            # Filter envelope: fast decay "squelch", retriggered on every
            # Note On (including slides) - this is what gives acid its
            # characteristic per-note sweep even across a glide.
            self.filter_env *= self.filter_env_decay_coeff

            cutoff = (BASE_CUTOFF_HZ
                      + self.filter_env * FILTER_ENV_AMOUNT_HZ
                      + expression * EXPRESSION_AMOUNT_HZ)
            cutoff = max(60.0, min(cutoff, MAX_CUTOFF_HZ))
            q = BASE_RESONANCE_Q + self.accent * (ACCENT_RESONANCE_Q - BASE_RESONANCE_Q)

            f = 2.0 * math.sin(math.pi * cutoff / self.sr)
            # Chamberlin state-variable filter, lowpass output.
            high = saw - self.svf_low - q * self.svf_band
            self.svf_band += f * high
            self.svf_low += f * self.svf_band

            # Safety net: this filter's coefficients are modulated every
            # sample (by the decaying filter envelope and live CC11
            # expression), which can transiently push even a nominally
            # stable f/q combination into runaway feedback. Clamp state
            # rather than let it diverge to inf/NaN - self-corrects
            # within a few samples once the transient passes, at the
            # cost of a brief, inaudible-in-practice clip rather than a
            # blown-up or silent output. See the tuning comment above.
            if self.svf_low > MAX_STATE: self.svf_low = MAX_STATE
            elif self.svf_low < -MAX_STATE: self.svf_low = -MAX_STATE
            if self.svf_band > MAX_STATE: self.svf_band = MAX_STATE
            elif self.svf_band < -MAX_STATE: self.svf_band = -MAX_STATE

            out[i] = self.svf_low * self.amp * MASTER_GAIN

        return out


LOG_NOTE_ON = re.compile(r"NOTE_ON\s+ch=(\d+)\s+note=(\d+)\s+vel=(\d+)")
LOG_NOTE_OFF = re.compile(r"NOTE_OFF\s+ch=(\d+)\s+note=(\d+)\s+vel=(\d+)")
LOG_CC = re.compile(r"CC\s+ch=(\d+)\s+cc=(\d+)\s+val=(\d+)")

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def note_label(note: int) -> str:
    return f"{NOTE_NAMES[note % 12]}{note // 12 - 1}"


def find_default_port() -> str:
    candidates = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not candidates:
        print("No /dev/cu.usbmodem* port found - pass --port explicitly.", file=sys.stderr)
        sys.exit(1)
    return candidates[0]


def serial_reader_thread(port: str, baud: int, state: SharedMidiState, stop_event: threading.Event):
    ser = serial.Serial(port, baud, timeout=0.5)
    print(f"[serial] listening on {port} @ {baud}")
    try:
        while not stop_event.is_set():
            raw = ser.readline()
            if not raw:
                continue
            try:
                line = raw.decode(errors="replace").rstrip()
            except Exception:
                continue

            m = LOG_NOTE_ON.search(line)
            if m:
                ch, note, vel = (int(x) for x in m.groups())
                state.note_on(note, vel)
                print(f"  NOTE_ON  ch={ch} {note_label(note):<4} (midi={note:3d}) vel={vel:3d}")
                continue

            m = LOG_NOTE_OFF.search(line)
            if m:
                ch, note, vel = (int(x) for x in m.groups())
                state.note_off(note)
                print(f"  NOTE_OFF ch={ch} {note_label(note):<4} (midi={note:3d})")
                continue

            m = LOG_CC.search(line)
            if m:
                ch, cc, val = (int(x) for x in m.groups())
                state.cc(cc, val)
                continue
    finally:
        ser.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=None, help="serial port (default: auto-detect /dev/cu.usbmodem*)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--device", type=int, default=None, help="sounddevice output device index (see --list-devices)")
    ap.add_argument("--list-devices", action="store_true")
    ap.add_argument("--volume", type=float, default=1.0, help="output level multiplier")
    ap.add_argument("--record-seconds", type=float, default=None,
                     help="instead of live playback, listen for this many seconds and write a WAV "
                          "file (--wav-out) - no audio device needed, useful for headless verification")
    ap.add_argument("--wav-out", default="acid_capture.wav")
    ap.add_argument("--play-seconds", type=float, default=None,
                     help="for live playback (--self-test or normal serial mode): stop automatically "
                          "after this many seconds instead of running until Ctrl+C")
    ap.add_argument("--self-test", action="store_true",
                     help="play a fixed acid riff through the audio device instead of listening to "
                          "serial - use this first if you suspect no sound is an audio-routing problem "
                          "rather than a MIDI-generation problem")
    args = ap.parse_args()

    if args.list_devices:
        print(sd.query_devices())
        print("default:", sd.default.device)
        return

    state = SharedMidiState()
    voice = AcidVoice(SAMPLE_RATE)
    volume = args.volume
    stop_event = threading.Event()

    if args.self_test:
        print("Self-test: playing a fixed riff, no board/serial involved.")

        def self_test_thread():
            riff = [45, 45, 48, 45, 52, 45, 43, 45]  # a little acid-ish loop
            i = 0
            while not stop_event.is_set():
                state.note_on(riff[i % len(riff)], 100 if i % 4 == 0 else 60)
                time.sleep(0.16)
                state.note_off(riff[i % len(riff)])
                time.sleep(0.02)
                i += 1

        threading.Thread(target=self_test_thread, daemon=True).start()
    else:
        port = args.port or find_default_port()
        reader = threading.Thread(target=serial_reader_thread, args=(port, args.baud, state, stop_event), daemon=True)
        reader.start()

    if args.record_seconds is not None:
        import wave
        source = "self-test riff" if args.self_test else port
        print(f"Recording {args.record_seconds:.1f}s from {source} to {args.wav_out} (no audio device used)...")
        total_samples = int(args.record_seconds * SAMPLE_RATE)
        rendered = 0
        chunks = []
        while rendered < total_samples:
            n = min(BLOCK_SIZE, total_samples - rendered)
            chunks.append(voice.render(n, state) * volume)
            rendered += n
            time.sleep(n / SAMPLE_RATE)  # render at real-time pace so live serial events land correctly
        audio = np.concatenate(chunks)
        clipped = np.clip(audio, -1.0, 1.0)
        pcm16 = (clipped * 32767.0).astype(np.int16)
        with wave.open(args.wav_out, "wb") as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)
            wf.setframerate(SAMPLE_RATE)
            wf.writeframes(pcm16.tobytes())
        print(f"Wrote {args.wav_out}: {len(audio)} samples, "
              f"peak={np.max(np.abs(audio)):.3f}, rms={np.sqrt(np.mean(audio**2)):.4f}")
        stop_event.set()
        return

    def audio_callback(outdata, frames, time_info, status):
        if status:
            print(status, file=sys.stderr)
        block = voice.render(frames, state) * volume
        outdata[:, 0] = block

    if args.device is not None:
        resolved_device = args.device
    else:
        try:
            resolved_device = sd.default.device[1]
        except TypeError:
            resolved_device = sd.default.device
    device_name = sd.query_devices(resolved_device)["name"]
    print("YoungPiongMidi acid synth monitor"
          + (" (self-test riff)" if args.self_test else "")
          + (f" - auto-stopping after {args.play_seconds:.0f}s" if args.play_seconds else " - Ctrl+C to stop"))
    print(f"audio out: [{resolved_device}] {device_name}  ({SAMPLE_RATE} Hz, block={BLOCK_SIZE})")
    print("(if you don't hear anything, that device may not be what's physically playing sound - "
          "try --list-devices and pass --device N for e.g. built-in speakers)")

    try:
        with sd.OutputStream(samplerate=SAMPLE_RATE, blocksize=BLOCK_SIZE, channels=1,
                              dtype="float32", device=args.device, callback=audio_callback):
            if args.play_seconds is not None:
                time.sleep(args.play_seconds)
            else:
                while True:
                    time.sleep(0.2)
    except KeyboardInterrupt:
        pass
    finally:
        stop_event.set()


if __name__ == "__main__":
    main()
