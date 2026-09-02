#!/usr/bin/env python3
"""
synth_instruments.py - a small polyphonic synth engine with 10 selectable
instruments, driven by MIDI note/velocity/CC11 events, for
tools/groovebox.py.

Design notes (why it's built this way):

- Voices are plain-Python per-sample loops, same style as
  AcidVoice.render() in tools/acid_synth_monitor.py - proven fast enough
  in practice for this project's actual load. That tool is monophonic;
  this engine is polyphonic (MAX_VOICES) purely as a safety margin for
  fast note_on/note_off overlaps during a note change, since the
  firmware's own pitch tracker is monophonic and rarely holds more than
  one note at a time.

- Only one instrument (id 0, "Acid Bass") uses a resonant filter (the
  Chamberlin state-variable filter), and it reuses the exact coefficient
  ranges and MAX_STATE safety clamp already tuned and stress-tested in
  acid_synth_monitor.py - see that file's comments for why those bounds
  matter. None of the other 9 instruments use a modulated resonant
  filter at all (only fixed/slowly-varying one-pole filters, additive
  synthesis, saturation, or Karplus-Strong string synthesis, all of
  which are structurally unable to diverge to inf/NaN the way a
  modulated high-Q filter can) - so no new instability risk was
  introduced by adding 9 more voices.

- Envelopes use the same "coefficient from a time constant" pattern used
  throughout this project's own DSP (components/audio_dsp/envelope.c,
  AcidVoice's amp/filter envelopes): coeff = exp(-1 / (time_s * sr)),
  applied as value += (target - value) * (1 - coeff) each sample.
"""
import math
import threading
import time

import numpy as np

MAX_VOICES = 6

# --- Instrument catalog -----------------------------------------------
# id -> (name, short description shown in the UI)
INSTRUMENTS = [
    (0, "Acid Bass", "TB-303-style resonant saw, squelch envelope"),
    (1, "Sine Lead", "pure sine, fast clean attack"),
    (2, "Square Lead", "smoothed square wave, buzzy mid lead"),
    (3, "Saw Pad", "sawtooth through a mellow low-pass, slow swell"),
    (4, "FM Bell", "2-operator FM, metallic percussive decay"),
    (5, "Pluck", "Karplus-Strong plucked string"),
    (6, "Sub Bass", "saturated sine, deep and round"),
    (7, "Brass", "sawtooth with an envelope-brightened low-pass"),
    (8, "Organ", "additive drawbars, fast attack / slow release"),
    (9, "Vibraphone", "sine with tremolo shimmer, long soft fade"),
]
INSTRUMENT_NAMES = [name for _, name, _ in INSTRUMENTS]


def midi_note_to_freq(note: int) -> float:
    return 440.0 * (2.0 ** ((note - 69) / 12.0))


def _coeff(time_ms: float, sr: int) -> float:
    time_ms = max(time_ms, 0.5)
    return math.exp(-1.0 / (time_ms * 0.001 * sr))


class Voice:
    """One sounding (or releasing) note. Instrument-specific state is
    kept generic (a handful of floats/buffers reused differently per
    instrument) rather than subclassed, since the engine only ever needs
    a small, fixed pool of these (MAX_VOICES)."""

    def __init__(self, sr: int):
        self.sr = sr
        self.active = False
        self.instrument = 0
        self.note = -1
        self.velocity = 0
        self.gate = False
        self.freq = 220.0
        self.age = 0          # increasing "trigger order" counter, for voice stealing
        self.amp = 0.0        # amplitude envelope, 0..~1
        self.env_stage = "attack"  # attack -> decay -> release, classic ADSR

        self.phase = 0.0
        self.phase2 = 0.0     # FM modulator / secondary oscillator phase
        self.lp_state = 0.0   # generic one-pole filter state (saw pad, brass)
        self.lp_state2 = 0.0

        # Acid (id 0) - Chamberlin SVF state, matches acid_synth_monitor.py
        self.svf_low = 0.0
        self.svf_band = 0.0
        self.filter_env = 0.0
        self.filter_env_decay_coeff = 0.0
        self.accent = 0.0

        # Pluck (id 5) - Karplus-Strong ring buffer
        self.ks_buffer = None
        self.ks_index = 0

        # Bell/organ/vibraphone shimmer LFO
        self.lfo_phase = 0.0

    def trigger(self, instrument: int, note: int, velocity: int, sr: int, age: int):
        self.active = True
        self.instrument = instrument
        self.note = note
        self.velocity = velocity
        self.gate = True
        self.freq = midi_note_to_freq(note)
        self.age = age
        self.phase = 0.0
        self.phase2 = 0.0
        self.lfo_phase = 0.0
        self.env_stage = "attack"
        vel_norm = max(0.0, min(1.0, velocity / 127.0))

        if instrument == 0:  # Acid Bass
            self.accent = vel_norm
            self.filter_env = 1.0
            decay_ms = 220.0 + self.accent * (420.0 - 220.0)
            self.filter_env_decay_coeff = _coeff(decay_ms, sr)
            self.svf_low = 0.0
            self.svf_band = 0.0
        elif instrument == 5:  # Pluck: Karplus-Strong
            n = max(4, int(sr / self.freq))
            rng = np.random.default_rng()
            burst = (rng.random(n).astype(np.float64) * 2.0 - 1.0) * (0.4 + 0.6 * vel_norm)
            self.ks_buffer = burst
            self.ks_index = 0
        # amp envelope always starts from 0 on a fresh trigger (no legato
        # glide in this engine - each MIDI note is its own voice)
        self.amp = 0.0

    def note_off(self):
        self.gate = False

    def render_add(self, out: np.ndarray, expression: float):
        """Adds this voice's contribution into `out` (float64, same
        length as the block). Marks the voice inactive once its
        amplitude envelope has fully decayed after note-off."""
        sr = self.sr
        n = len(out)
        vel_norm = max(0.0, min(1.0, self.velocity / 127.0))
        instrument = self.instrument

        # Per-instrument classic ADSR: attack always rises to a peak of
        # 1.0 first, then decays toward sustain_level while still held
        # (sustain_level=0.0 gives a one-shot percussive character, e.g.
        # FM Bell/Pluck, that fades out even if the note is held down),
        # then releases to 0 once note-off arrives. (attack_ms, decay_ms,
        # sustain_level, release_ms)
        attack_ms, decay_ms, sustain_level, release_ms = 6.0, 120.0, 1.0, 80.0
        if instrument == 1:      # Sine Lead
            attack_ms, decay_ms, sustain_level, release_ms = 3.0, 40.0, 1.0, 60.0
        elif instrument == 2:    # Square Lead
            attack_ms, decay_ms, sustain_level, release_ms = 5.0, 60.0, 1.0, 70.0
        elif instrument == 3:    # Saw Pad
            attack_ms, decay_ms, sustain_level, release_ms = 180.0, 300.0, 0.85, 500.0
        elif instrument == 4:    # FM Bell
            attack_ms, decay_ms, sustain_level, release_ms = 2.0, 900.0, 0.0, 300.0
        elif instrument == 5:    # Pluck (the KS buffer itself provides
                                  # most of the decay character - the amp
                                  # envelope here is a long safety fade,
                                  # not the primary decay)
            attack_ms, decay_ms, sustain_level, release_ms = 2.0, 3500.0, 0.0, 2000.0
        elif instrument == 6:    # Sub Bass
            attack_ms, decay_ms, sustain_level, release_ms = 10.0, 60.0, 1.0, 140.0
        elif instrument == 7:    # Brass
            attack_ms, decay_ms, sustain_level, release_ms = 35.0, 80.0, 0.9, 120.0
        elif instrument == 8:    # Organ
            attack_ms, decay_ms, sustain_level, release_ms = 8.0, 40.0, 1.0, 200.0
        elif instrument == 9:    # Vibraphone
            attack_ms, decay_ms, sustain_level, release_ms = 15.0, 600.0, 0.55, 900.0

        attack_coeff = _coeff(attack_ms, sr)
        decay_coeff = _coeff(decay_ms, sr)
        release_coeff = _coeff(release_ms, sr)

        still_active = False
        for i in range(n):
            if not self.gate and self.env_stage != "release":
                self.env_stage = "release"

            if self.env_stage == "attack":
                self.amp += (1.0 - self.amp) * (1.0 - attack_coeff)
                if self.amp >= 0.999:
                    self.amp = 1.0
                    self.env_stage = "decay"
            elif self.env_stage == "decay":
                self.amp += (sustain_level - self.amp) * (1.0 - decay_coeff)
            else:  # release
                self.amp += (0.0 - self.amp) * (1.0 - release_coeff)

            self.phase += self.freq / sr
            if self.phase >= 1.0:
                self.phase -= 1.0

            sample = 0.0

            if instrument == 0:  # Acid Bass (Chamberlin SVF, TB-303 style)
                saw = 2.0 * self.phase - 1.0
                self.filter_env *= self.filter_env_decay_coeff
                cutoff = 200.0 + self.filter_env * 2200.0 + expression * 900.0
                cutoff = max(60.0, min(cutoff, 4000.0))
                q = 2.2 + self.accent * (1.0 - 2.2)
                f = 2.0 * math.sin(math.pi * cutoff / sr)
                high = saw - self.svf_low - q * self.svf_band
                self.svf_band += f * high
                self.svf_low += f * self.svf_band
                if self.svf_low > 8.0: self.svf_low = 8.0
                elif self.svf_low < -8.0: self.svf_low = -8.0
                if self.svf_band > 8.0: self.svf_band = 8.0
                elif self.svf_band < -8.0: self.svf_band = -8.0
                sample = self.svf_low

            elif instrument == 1:  # Sine Lead
                sample = math.sin(2.0 * math.pi * self.phase)

            elif instrument == 2:  # Square Lead (smoothed, avoids raw harsh aliasing)
                raw = 1.0 if self.phase < 0.5 else -1.0
                smooth_coeff = 1.0 - min(1.0, 6000.0 / sr)
                self.lp_state += (raw - self.lp_state) * (1.0 - smooth_coeff)
                sample = self.lp_state

            elif instrument == 3:  # Saw Pad: saw -> fixed one-pole LPF
                saw = 2.0 * self.phase - 1.0
                cutoff = 900.0 + vel_norm * 600.0 + expression * 400.0
                pole = math.exp(-2.0 * math.pi * cutoff / sr)
                self.lp_state = saw * (1.0 - pole) + self.lp_state * pole
                sample = self.lp_state * 1.4

            elif instrument == 4:  # FM Bell: 2-op FM, decaying modulation index
                mod_index = 3.5 * self.amp  # index tracks the (fast-decaying) envelope
                self.phase2 += (self.freq * 3.01) / sr
                if self.phase2 >= 1.0:
                    self.phase2 -= 1.0
                modulator = math.sin(2.0 * math.pi * self.phase2)
                sample = math.sin(2.0 * math.pi * self.phase + mod_index * modulator)

            elif instrument == 5:  # Pluck: Karplus-Strong
                buf = self.ks_buffer
                m = len(buf)
                idx = self.ks_index
                nxt = (idx + 1) % m
                # Classic KS: average two adjacent samples (a one-pole
                # lowpass baked into the delay loop) with a fixed <1
                # damping factor - this is what makes the string decay;
                # it is structurally stable (strictly loses energy every
                # pass through the loop, can't diverge).
                new_val = 0.5 * (buf[idx] + buf[nxt]) * 0.996
                sample = buf[idx]
                buf[idx] = new_val
                self.ks_index = nxt

            elif instrument == 6:  # Sub Bass: saturated sine for weight
                s = math.sin(2.0 * math.pi * self.phase)
                sample = math.tanh(1.6 * s) * 0.85

            elif instrument == 7:  # Brass: saw -> one-pole LPF, envelope brightens cutoff
                saw = 2.0 * self.phase - 1.0
                brightness = 400.0 + self.amp * 1800.0 + vel_norm * 900.0 + expression * 500.0
                cutoff = min(brightness, sr * 0.45)
                pole = math.exp(-2.0 * math.pi * cutoff / sr)
                self.lp_state = saw * (1.0 - pole) + self.lp_state * pole
                sample = self.lp_state * 1.3

            elif instrument == 8:  # Organ: fixed additive drawbars
                p = self.phase
                h2 = (p * 2.0) % 1.0
                h3 = (p * 3.0) % 1.0
                h4 = (p * 4.0) % 1.0
                sample = (math.sin(2 * math.pi * p) * 1.0
                          + math.sin(2 * math.pi * h2) * 0.5
                          + math.sin(2 * math.pi * h3) * 0.33
                          + math.sin(2 * math.pi * h4) * 0.25) * 0.55

            elif instrument == 9:  # Vibraphone: sine + slow tremolo + gentle shimmer
                self.lfo_phase += 5.5 / sr
                if self.lfo_phase >= 1.0:
                    self.lfo_phase -= 1.0
                tremolo = 0.75 + 0.25 * math.sin(2.0 * math.pi * self.lfo_phase)
                self.phase2 += (self.freq * 4.0) / sr
                if self.phase2 >= 1.0:
                    self.phase2 -= 1.0
                shimmer = 0.06 * math.sin(2.0 * math.pi * self.phase2) * self.amp
                sample = (math.sin(2.0 * math.pi * self.phase) + shimmer) * tremolo

            level = self.amp * (1.0 * vel_norm + 0.35)
            out[i] += sample * level

            if not self.gate and self.amp > 0.0005:
                still_active = True
            elif self.gate:
                still_active = True

        if not still_active:
            self.active = False


class SynthEngine:
    """Polyphonic engine: MAX_VOICES-voice pool with age-based stealing,
    a live-selectable current instrument (applied to new notes only -
    already-sounding notes keep the instrument they were triggered
    with), and CC11 Expression applied as an overall gain (the
    conventional MIDI Expression semantics), plus fed to each voice for
    the handful of instruments that also use it to brighten a filter.

    Thread-safety: note_on/note_off/cc/set_instrument are called from
    the serial-reader thread (or a GUI "test note" button on the main
    thread); render() is called from the sounddevice audio callback
    thread. A single lock guards the voice pool and the small amount of
    shared scalar state - contention is negligible since MIDI events
    arrive at tens of Hz at most, far below the audio block rate.
    """

    def __init__(self, sample_rate: int):
        self.sr = sample_rate
        self.lock = threading.Lock()
        self.voices = [Voice(sample_rate) for _ in range(MAX_VOICES)]
        self.current_instrument = 0
        self.expression = 1.0  # 0..1, CC11 / 127
        self._age_counter = 0
        self.master_gain = 0.28

        # Event history for the UI (timestamp, kind, ...) - bounded so it
        # can never grow unbounded during a long session.
        self.event_log = []
        self.max_event_log = 500
        # Currently-held notes, for a piano-roll style display.
        self.held_notes = {}  # note -> (instrument, velocity, start_time)

    def set_instrument(self, instrument_id: int):
        with self.lock:
            self.current_instrument = max(0, min(len(INSTRUMENTS) - 1, instrument_id))

    def note_on(self, note: int, velocity: int, source: str = "external"):
        """`source` tags where this note came from in the logged event
        (defaults to "external" - the board, Demo mode, the Test Note
        button, etc. all fall under that with zero call-site changes).
        Only the sequencer's own pattern playback and take playback tag
        themselves otherwise (groovebox.py wires those wrapped) - so
        StepRecorder (sequencer.py) can tell "someone actually played
        something" apart from "the pattern/a take is just repeating
        what's already there" and never write either of those back into
        the step grid it's recording into."""
        with self.lock:
            self._age_counter += 1
            # Prefer an idle voice; otherwise steal the oldest active one
            # (lowest .age) so a fast run of notes never drops silently.
            voice = next((v for v in self.voices if not v.active), None)
            if voice is None:
                voice = min(self.voices, key=lambda v: v.age)
            voice.trigger(self.current_instrument, note, velocity, self.sr, self._age_counter)
            self.held_notes[note] = (self.current_instrument, velocity, time.time())
            self._log("note_on", note=note, velocity=velocity, instrument=self.current_instrument,
                      source=source)

    def note_off(self, note: int, source: str = "external"):
        with self.lock:
            for v in self.voices:
                if v.active and v.note == note and v.gate:
                    v.note_off()
            self.held_notes.pop(note, None)
            self._log("note_off", note=note, source=source)

    def cc(self, controller: int, value: int):
        if controller == 11:
            with self.lock:
                self.expression = max(0.0, min(1.0, value / 127.0))
                self._log("cc", controller=controller, value=value)

    def _log(self, kind: str, **fields):
        self.event_log.append({"t": time.time(), "kind": kind, **fields})
        if len(self.event_log) > self.max_event_log:
            del self.event_log[0]

    def snapshot_for_ui(self):
        """Cheap, lock-protected snapshot for the GUI's redraw timer -
        never hands out the live voice/event objects themselves."""
        with self.lock:
            held = dict(self.held_notes)
            events = list(self.event_log[-100:])
            instrument = self.current_instrument
            expression = self.expression
            active_voices = sum(1 for v in self.voices if v.active)
        return {
            "held_notes": held,
            "events": events,
            "instrument": instrument,
            "expression": expression,
            "active_voices": active_voices,
        }

    def render(self, n: int) -> np.ndarray:
        out = np.zeros(n, dtype=np.float64)
        with self.lock:
            expression = self.expression
            active = [v for v in self.voices if v.active]
        for v in active:
            v.render_add(out, expression)
        # Expression as overall gain (standard MIDI CC11 semantics),
        # never fully muting so a forgotten low expression doesn't look
        # like "no sound at all" during a demo.
        gain = self.master_gain * (0.15 + 0.85 * expression)
        out *= gain
        np.clip(out, -1.0, 1.0, out=out)
        return out.astype(np.float32)
