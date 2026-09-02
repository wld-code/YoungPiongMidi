#!/usr/bin/env python3
"""
sequencer.py - pure-logic step sequencer model for Young Piong Synth
Studio (tools/synth_studio.py): 8 banks ("patterns") of 16 steps each,
tempo-driven.

Deliberately has zero Tkinter/audio dependency - the timing math and
bank/step data model are the part that actually needs to be *correct*
(wrong tempo math or a race on bank-switch-during-playback would be a
real, silent bug), and that correctness can be fully unit-tested without
opening a window or a display - see tools/test_sequencer.py. Everything
Tk-specific (drawing the step grid, wiring clicks) lives in
synth_studio.py and only ever calls into this module.
"""
import threading
import time

NUM_BANKS = 8
NUM_STEPS = 16
DEFAULT_NOTE = 60          # C4
DEFAULT_VELOCITY = 100
ACCENT_VELOCITY = 127
MIN_BPM = 40
MAX_BPM = 240
DEFAULT_BPM = 120
STEPS_PER_BEAT = 4          # 16 steps/bank == 4 steps/beat == 16th notes
GATE_RATIO = 0.72            # fraction of a step's duration the note is held for,
                              # leaving an audible gap before the next step


class Step:
    """One cell of a bank: on/off, which note it plays, and whether it's
    accented (louder - a nod to the Acid Bass instrument's TB-303
    heritage, but applies to any instrument)."""
    __slots__ = ("on", "note", "accent")

    def __init__(self, on=False, note=DEFAULT_NOTE, accent=False):
        self.on = on
        self.note = note
        self.accent = accent

    def velocity(self) -> int:
        return ACCENT_VELOCITY if self.accent else DEFAULT_VELOCITY


def clamp_bpm(bpm: float) -> float:
    return max(MIN_BPM, min(MAX_BPM, bpm))


def step_duration_seconds(bpm: float) -> float:
    """Duration of one step (a 16th note) at the given tempo, clamped to
    the supported BPM range."""
    beat_seconds = 60.0 / clamp_bpm(bpm)
    return beat_seconds / STEPS_PER_BEAT


class SequencerModel:
    """Holds the 8 banks x 16 steps of state, which bank is currently
    selected, and the tempo. No timing/threading here at all - see
    SequencerPlayer below - so this class alone is trivial to unit test
    and safe to mutate directly from the GUI thread (a click handler)
    while SequencerPlayer reads it from its own thread; the only field
    read concurrently during playback is `active_bank`/`bpm` and the
    step list contents, all plain-old-data reads/writes on values that
    are never partially updated (an int reassignment, a Step's fields
    set together in toggle_step) - no lock needed for the same reason
    the DSP pipeline this project is built around treats single aligned
    reads/writes of plain values as safe across threads."""

    def __init__(self):
        self.banks = [[Step() for _ in range(NUM_STEPS)] for _ in range(NUM_BANKS)]
        self.active_bank = 0
        self.bpm = DEFAULT_BPM

    def set_bpm(self, bpm):
        self.bpm = clamp_bpm(bpm)

    def select_bank(self, index: int):
        if 0 <= index < NUM_BANKS:
            self.active_bank = index

    def toggle_step(self, bank_index: int, step_index: int, note: int) -> Step:
        """Off -> on (with `note`, accent cleared); on -> off. Returns
        the step so callers (the GUI) can redraw it immediately."""
        step = self.banks[bank_index][step_index]
        if step.on:
            step.on = False
        else:
            step.on = True
            step.note = note
            step.accent = False
        return step

    def toggle_accent(self, bank_index: int, step_index: int) -> Step:
        step = self.banks[bank_index][step_index]
        if step.on:
            step.accent = not step.accent
        return step

    def set_step(self, bank_index: int, step_index: int, note: int, accent: bool = False) -> Step:
        """Unconditionally sets a step ON with `note` (overwrite, not
        toggle-off-if-already-on) - unlike toggle_step(), meant for
        live step-recording (StepRecorder below): "this step now holds
        this note" regardless of the step's prior state. Never touches
        any other step, so a bank being recorded into is only ever
        changed one step at a time, exactly where a new note actually
        landed - see StepRecorder's own docstring for why that matters."""
        step = self.banks[bank_index][step_index]
        step.on = True
        step.note = note
        step.accent = accent
        return step

    def clear_bank(self, bank_index: int):
        self.banks[bank_index] = [Step() for _ in range(NUM_STEPS)]

    def active_steps(self):
        return self.banks[self.active_bank]


class SequencerPlayer:
    """Drives playback of a SequencerModel's *currently active* bank in
    a background thread, calling note_on(note, velocity)/note_off(note)
    at each step boundary. Decoupled from any specific synth engine or
    GUI on purpose - note_on/note_off/on_step are plain callables, so
    tests can pass list-appending fakes instead of a real audio engine
    (see tools/test_sequencer.py) and the GUI just passes
    SynthEngine.note_on/note_off directly (which already logs the event
    for the piano-roll/MIDI log - no separate wiring needed there).

    Bank switches mid-playback take effect at the *next* step boundary
    (active_steps() is re-read every iteration) rather than either
    ignoring the switch or corrupting an in-flight step - this matches
    how a real hardware sequencer's pattern-change behaves.
    """

    def __init__(self, model: SequencerModel, note_on, note_off, on_step=None):
        self.model = model
        self.note_on = note_on
        self.note_off = note_off
        self.on_step = on_step  # optional callback(step_index) after each step fires
        self._thread = None
        self._stop_event = threading.Event()
        self.current_step = -1
        self.playing = False

    def start(self):
        # Each run gets its own fresh Event rather than clear()-ing a
        # shared one - if start() is called again quickly enough that
        # the previous run's thread hasn't fully unwound yet, reusing
        # one Event risks start()'s clear() racing the old thread's
        # in-flight wait() on that same object (the old thread could
        # then miss ever seeing it as "set" and keep running past its
        # intended stop, now orphaned since self._thread has moved on
        # to the new run). Ensure the previous thread has actually
        # exited first - in the overwhelmingly common case (a GUI
        # button click well after the prior stop()) it already has, so
        # this is a no-op; join() is only a real wait in the pathological
        # case of two calls milliseconds apart.
        if self._thread is not None and self._thread.is_alive():
            self._stop_event.set()
            self._thread.join(timeout=2.0)

        self.playing = True
        stop_event = threading.Event()
        self._stop_event = stop_event
        self._thread = threading.Thread(target=self._run, args=(stop_event,), daemon=True)
        self._thread.start()

    def stop(self, join=False):
        self._stop_event.set()
        # Set synchronously (not just left to _run's `finally`, which
        # only executes once the background thread notices the stop
        # event) so that a GUI button reading `.playing` right after
        # calling stop() sees it flip immediately - otherwise a fast
        # stop-then-start click could see a stale `playing == True` and
        # start() would silently no-op. _run's own assignment on exit is
        # then a harmless, idempotent repeat of this.
        self.playing = False
        if join and self._thread is not None:
            self._thread.join(timeout=2.0)

    def _run(self, stop_event: threading.Event):
        held_note = None
        try:
            idx = 0
            while not stop_event.is_set():
                steps = self.model.active_steps()
                step = steps[idx % NUM_STEPS]
                self.current_step = idx % NUM_STEPS

                if held_note is not None:
                    self.note_off(held_note)
                    held_note = None
                if step.on:
                    self.note_on(step.note, step.velocity())
                    held_note = step.note
                if self.on_step:
                    self.on_step(self.current_step)

                duration = step_duration_seconds(self.model.bpm)
                gate = duration * GATE_RATIO
                # Wait in two parts (gate, then the remaining silence) so
                # stop() interrupts within a fraction of a step instead
                # of waiting out a full one, and so the note-off for the
                # gap is timed correctly rather than only happening at
                # the very end of the step.
                if stop_event.wait(gate):
                    break
                if held_note is not None:
                    self.note_off(held_note)
                    held_note = None
                remaining = duration - gate
                if remaining > 0 and stop_event.wait(remaining):
                    break
                idx += 1
        finally:
            if held_note is not None:
                self.note_off(held_note)
            self.current_step = -1
            self.playing = False


# Velocity at/above which a step-recorded note is written in as
# accented, mirroring the manual "right-click to accent" gesture -
# singing/playing louder accents the step automatically.
STEP_RECORD_ACCENT_VELOCITY_THRESHOLD = 110


class StepRecorder:
    """Writes incoming *external* (i.e. not the pattern's own output -
    see the `source` check below) note_on events straight into a
    SequencerModel's step grid in real time, quantized to whichever
    step is currently playing: "record my singing into the pattern"
    step-record, the same idea as a hardware groovebox's real-time
    record mode - sing/play a note while the pattern loops and it lands
    on whatever step is currently active, overwriting only that one
    step (SequencerModel.set_step(), not toggle_step()) with the new
    note. A step nothing is ever played into during a recording session
    is left completely untouched - the "never erases unless you
    actually play a sound" property is automatic here, not something
    this class has to enforce separately, since it only ever calls
    set_step() on the exact one step index a note actually landed on.

    Always writes into the model's *own* `active_bank` (not a bank it
    tracks itself, unlike LiveRecordSession) - since SequencerModel
    already IS the single shared source of truth for which bank is
    selected, and steps live directly in their bank with no "finalize on
    switch" step needed, switching banks while armed here needs zero
    extra code: the very next write just lands in whatever bank is now
    active.

    Deliberately independent of LiveRecordSession (the freeform,
    unquantized MIDI take capture) - both can be armed from the exact
    same "Record" button and read the exact same incoming event stream;
    they just do two different things with it. Events tagged
    `source="pattern"` (the sequencer's own notes) or any other
    non-"external" source (e.g. take playback) are ignored, so this
    can never write its own output - or a played-back take's - back
    into the grid.
    """

    def __init__(self, model: SequencerModel):
        self.model = model
        self.armed = False
        self._last_seen_ts = 0.0

    def arm(self):
        self.armed = True
        self._last_seen_ts = time.time()

    def disarm(self):
        self.armed = False

    def ingest(self, events, current_step: int):
        """`events`: chronological SynthEngine.event_log-shaped dicts
        (same shape LiveRecordSession.ingest() takes), each optionally
        carrying `source` (treated as "external" if absent, so plain
        board/Demo/Test-Note events - which don't set it - are
        recordable by default; only the pattern's own playback and take
        playback explicitly tag themselves otherwise).
        `current_step`: SequencerPlayer.current_step at the moment this
        is called - -1 (pattern not running) means there's nothing to
        quantize against, so note_on events are skipped (but the cursor
        still advances - they're not "missed", there's just no
        meaningful step to write them to)."""
        if not self.armed:
            return
        newest_ts = self._last_seen_ts
        for e in events:
            if e["t"] <= self._last_seen_ts:
                continue
            newest_ts = max(newest_ts, e["t"])
            if e["kind"] != "note_on":
                continue
            if e.get("source", "external") != "external":
                continue
            if current_step < 0:
                continue
            velocity = e.get("velocity", 0)
            accent = velocity >= STEP_RECORD_ACCENT_VELOCITY_THRESHOLD
            self.model.set_step(self.model.active_bank, current_step, e["note"], accent=accent)
        self._last_seen_ts = newest_ts
