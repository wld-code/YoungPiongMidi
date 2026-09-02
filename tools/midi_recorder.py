#!/usr/bin/env python3
"""
midi_recorder.py - real-time MIDI performance recorder for Young Piong
Groovebox (tools/groovebox.py). Captures incoming note_on/
note_off events with their actual real-time timing while armed - a
genuine MIDI recording (a "take"), not audio - then can play it back
through the exact recorded timing and save it as a standard MIDI file
(.mid) that any DAW can open.

Deliberately separate from sequencer.py: the step sequencer edits a
fixed 16-step/8-bank quantized pattern; this instead records an
arbitrary-length, arbitrarily-timed performance - the same idea as
hitting Record on a hardware sequencer/DAW and then singing or playing,
capturing exactly what happened and when, unquantized.

MidiRecorder is deliberately NOT threaded or lock-protected, unlike
SequencerPlayer/the rest of this app's cross-thread state: it is always
driven from the Tk main thread's own ~30Hz redraw tick, fed the same
already-ordered `SynthEngine.event_log` snapshot the piano-roll and MIDI
log already read (see groovebox.py's _tick()) - there is no
producer/consumer split here to guard, and reusing that single source of
truth means a recording captures a note regardless of where it came from
(the board, the sequencer, Demo mode, the Test Note button) with zero
extra wiring at each call site.
"""
import threading
import time


class MidiTake:
    """An immutable recorded performance: a time-ordered list of
    (t_seconds, kind, note, velocity) tuples, t=0 at the moment
    recording started. `kind` is "note_on" or "note_off"."""

    def __init__(self, events):
        self.events = events

    @property
    def duration(self) -> float:
        return self.events[-1][0] if self.events else 0.0

    def is_empty(self) -> bool:
        return len(self.events) == 0

    def note_on_count(self) -> int:
        return sum(1 for e in self.events if e[1] == "note_on")


class MidiRecorder:
    def __init__(self):
        self.active = False
        self._events = []
        self._start_time = None   # lazy - see start()'s docstring for why
        self._armed_at = None     # wall-clock time.time() at start() - for elapsed() display only
        self._last_seen_ts = 0.0

    def start(self, last_seen_ts: float = None):
        """`last_seen_ts`, if given, overrides the dedup cursor's
        starting point instead of defaulting to time.time() - used by
        LiveRecordSession to carry the stream position forward across a
        bank switch rather than resetting to "now" (see its own
        set_active_bank() comment for why "now" would risk silently
        dropping an event that already happened moments earlier but
        hasn't reached ingest() yet this tick). The default (time.time())
        is correct for a brand new session (a plain arm()), so stale
        events already sitting in the last-100 event-log window from
        long before this session don't get resurrected.

        `_start_time` (the relative-time-zero baseline every captured
        event's offset is measured from) is deliberately NOT set here at
        all - see ingest(): it's set lazily, to the actual timestamp of
        the first event this segment ends up capturing. Setting it
        eagerly to time.time() here caused exactly this class of bug:
        a background thread (the sequencer's own pattern, or the board)
        can log an event microseconds before this call actually runs,
        which would then look like it happened *before* this segment
        started (a negative relative time) and get silently dropped -
        a real, observed bug (a bank's own first note vanishing right
        after a chain-mode transition, or Record's very first note)."""
        self._events = []
        self._start_time = None
        self._armed_at = time.time()
        self._last_seen_ts = last_seen_ts if last_seen_ts is not None else time.time()
        self.active = True

    def ingest(self, events):
        """`events`: SynthEngine.event_log-shaped dicts, chronological -
        each at least {"t": wall_clock_seconds, "kind": "note_on"/
        "note_off"/"cc", "note": ..., "velocity": ... (note_on only)}.
        Safe to call repeatedly with an overlapping/growing log (only
        events strictly newer than the last-ingested one are kept) and
        safe to call while inactive (a no-op) - callers don't need to
        track their own cursor separately from what NoteHistory/the log
        view already track."""
        if not self.active:
            return
        for e in events:
            if e["t"] <= self._last_seen_ts:
                continue
            self._last_seen_ts = e["t"]
            if e["kind"] not in ("note_on", "note_off"):
                continue
            if self._start_time is None:
                self._start_time = e["t"]  # this segment's first captured event defines its own t=0
            rel_t = e["t"] - self._start_time
            if rel_t < 0:
                continue  # can't happen now that _start_time is lazy - kept as a defensive guard
            velocity = e.get("velocity", 0)
            self._events.append((rel_t, e["kind"], e["note"], velocity))

    def elapsed(self) -> float:
        if not self.active or self._armed_at is None:
            return 0.0
        return time.time() - self._armed_at

    def stop(self) -> MidiTake:
        self.active = False
        take = MidiTake(list(self._events))
        self._events = []
        return take


class LiveRecordSession:
    """Orchestrates a MidiRecorder across bank switches for a live-
    looper-style recording workflow: while armed, notes are always
    captured into whichever bank is *currently selected* - selecting a
    different bank while armed finalizes whatever was just captured for
    the bank you were on and immediately begins a fresh capture for the
    newly selected bank, so recording never has to be stopped and
    restarted by hand just to move to another bank.

    The one rule that makes this safe rather than destructive: a bank's
    existing take is only ever replaced by finalizing a segment that
    actually captured at least one note. Switching to (or re-arming on)
    a bank that already holds a take, then switching away again without
    having played anything, leaves that take completely untouched -
    "il ne supprime pas sauf si je joue un son." The caller (groovebox.py)
    is the one that actually writes into `self.takes[bank]`; this class
    only ever *offers* a finalized (bank, MidiTake) pair when there's
    something genuinely new to store, and returns None otherwise -
    deciding "clear vs. keep" by never handing back an empty result.

    Deliberately holds no reference to SequencerModel/NUM_BANKS - the
    caller is the single source of truth for which bank is selected
    (via set_active_bank()); this class only needs to know when that
    changes.
    """

    def __init__(self):
        self.armed = False
        self.current_bank = 0
        self._recorder = MidiRecorder()

    def set_active_bank(self, bank_index: int):
        """Call whenever the (single, shared) bank selector changes,
        armed or not. Returns (old_bank_index, MidiTake) if something
        was actually captured for the bank being left, else None."""
        if bank_index == self.current_bank:
            return None
        finished = None
        if self.armed:
            # Carry the dedup cursor forward from wherever the just-
            # finalized segment's ingest() calls had actually reached -
            # NOT time.time() ("now") - see MidiRecorder.start()'s
            # docstring for why "now" can be later than an event the new
            # bank already logged (its own very first note, fired by the
            # player thread right at the transition) and silently drop it.
            carried_cursor = self._recorder._last_seen_ts
            finished = self._finalize_if_nonempty()
            self._recorder.start(last_seen_ts=carried_cursor)  # keep capturing, now for the new bank
        self.current_bank = bank_index
        return finished

    def arm(self, bank_index: int = None):
        """`bank_index`, if given, primes which bank the very first
        captured note gets attributed to - without it, arming keeps
        whatever `current_bank` was left over from the last
        set_active_bank() call, which is very often stale (e.g. a
        manual bank-button click made minutes earlier) and not actually
        where playback is starting from right now. This matters
        specifically for chain-mode playback: a fresh Play/Record always
        starts the chain at bank 0 regardless of which bank was last
        selected, so the caller passes that in explicitly rather than
        this class guessing or the caller reaching in to poke
        current_bank directly."""
        self.armed = True
        if bank_index is not None:
            self.current_bank = bank_index
        self._recorder.start()

    def disarm(self):
        """Stops capturing. Returns (bank_index, MidiTake) if something
        was actually recorded this segment, else None - in which case
        the caller must NOT touch that bank's existing take."""
        if not self.armed:
            return None
        self.armed = False
        return self._finalize_if_nonempty()

    def _finalize_if_nonempty(self):
        take = self._recorder.stop()
        if take.is_empty():
            return None
        return (self.current_bank, take)

    def ingest(self, events):
        self._recorder.ingest(events)

    def elapsed(self) -> float:
        return self._recorder.elapsed()


class MidiTakePlayer:
    """Plays a MidiTake back through note_on(note, velocity)/note_off
    (note) callables at its original recorded timing (unquantized,
    unlike sequencer.SequencerPlayer) - same background-thread-per-run
    pattern as SequencerPlayer, including the same fix for the same
    class of restart race (see sequencer.py's SequencerPlayer.start()
    comment): each play() gives its run its own fresh stop Event and
    joins any still-alive previous thread first, and stop() flips
    `.playing` synchronously rather than leaving it to the thread's own
    (necessarily asynchronous) cleanup.
    """

    def __init__(self, note_on, note_off, on_finished=None):
        self.note_on = note_on
        self.note_off = note_off
        self.on_finished = on_finished  # optional callable(), invoked from the player's
                                         # OWN thread when playback ends - callers on Tk
                                         # must not touch widgets in it directly (same rule
                                         # as SequencerPlayer's on_step)
        self._thread = None
        self._stop_event = threading.Event()
        self.playing = False
        self.position = 0.0

    def play(self, take: MidiTake, loop: bool = False):
        """`loop=True` repeats the take indefinitely (jumping back to
        t=0 after the last event) until stop() is called - used for the
        "it must repeat automatically" live-looper behavior in
        groovebox.py. Default is a single pass, matching a plain
        "play this once" expectation for any other caller."""
        if take.is_empty():
            return
        if self._thread is not None and self._thread.is_alive():
            self._stop_event.set()
            self._thread.join(timeout=2.0)

        self.playing = True
        self.position = 0.0
        stop_event = threading.Event()
        self._stop_event = stop_event
        self._thread = threading.Thread(target=self._run, args=(take, stop_event, loop), daemon=True)
        self._thread.start()

    def stop(self, join=False):
        self._stop_event.set()
        self.playing = False
        if join and self._thread is not None:
            self._thread.join(timeout=2.0)

    def _run(self, take: MidiTake, stop_event: threading.Event, loop: bool):
        held_notes = set()
        try:
            while not stop_event.is_set():
                t_prev = 0.0
                for (t, kind, note, velocity) in take.events:
                    wait = t - t_prev
                    if wait > 0 and stop_event.wait(wait):
                        return
                    t_prev = t
                    self.position = t
                    if kind == "note_on":
                        self.note_on(note, velocity)
                        held_notes.add(note)
                    elif kind == "note_off":
                        self.note_off(note)
                        held_notes.discard(note)
                if not loop:
                    break
                # Loop straight back to t=0 - no gap is inserted between
                # repeats (the take's own trailing silence, if any, IS
                # the gap), so a tightly-timed performance loops cleanly.
        finally:
            for n in held_notes:
                self.note_off(n)
            self.playing = False
            self.position = 0.0
            if self.on_finished:
                self.on_finished()


# --- Standard MIDI File (format 0) writer, no external dependencies ----

def _vlq(value: int) -> bytes:
    """Encodes a non-negative int as a MIDI variable-length quantity."""
    buf = [value & 0x7F]
    value >>= 7
    while value:
        buf.insert(0, (value & 0x7F) | 0x80)
        value >>= 7
    return bytes(buf)


def save_midi_file(take: MidiTake, path: str, ppqn: int = 480, bpm: float = 120.0):
    """Writes `take` as a standard format-0 .mid file with one track.

    The recording has no inherent tempo (it's a real-time capture, not
    a quantized pattern), so rather than guessing one, this fixes a
    tempo/PPQN pair purely to define a tick resolution fine enough to
    preserve the original timing: ticks_per_second = ppqn * bpm / 60 -
    the defaults (480 PPQN @ 120 BPM = 960 ticks/sec, ~1.04ms per tick)
    comfortably beat this app's own ~30Hz event-ingestion rate, so no
    audible timing is lost to quantization. Any DAW opening the file
    will show it at 120 BPM, but the actual note-to-note timing played
    back is the real, recorded timing regardless of that displayed
    tempo - a MIDI file's absolute event times are ticks, not "beats
    that happen to look like 120 BPM had significance."
    """
    ticks_per_second = ppqn * bpm / 60.0
    track = bytearray()

    us_per_quarter = int(round(60_000_000 / bpm))
    track += _vlq(0) + bytes([0xFF, 0x51, 0x03]) + us_per_quarter.to_bytes(3, "big")

    last_tick = 0
    for (t, kind, note, velocity) in take.events:
        tick = round(t * ticks_per_second)
        delta = max(0, tick - last_tick)
        last_tick = tick
        status = 0x90 if kind == "note_on" else 0x80
        note = max(0, min(127, int(note)))
        vel = max(0, min(127, int(velocity))) if kind == "note_on" else 0
        track += _vlq(delta) + bytes([status, note, vel])

    track += _vlq(0) + bytes([0xFF, 0x2F, 0x00])  # End of Track (required)

    header = (b"MThd" + (6).to_bytes(4, "big") + (0).to_bytes(2, "big")
              + (1).to_bytes(2, "big") + ppqn.to_bytes(2, "big"))
    track_chunk = b"MTrk" + len(track).to_bytes(4, "big") + bytes(track)

    with open(path, "wb") as f:
        f.write(header + track_chunk)
