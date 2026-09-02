#!/usr/bin/env python3
"""
synth_studio.py - Young Piong Synth Studio: a real-time Python GUI
synthesizer that connects to the board over the same serial MIDI log
tools/acid_synth_monitor.py reads (see docs/midi.md - BLE/UART wire
transports are Milestones 8-9, so this log is still the only "transport"
that exists), plays it through a 10-instrument polyphonic synth engine
(synth_instruments.py), and shows the incoming audio waveform, MIDI
event log, and a scrolling piano-roll of the melody the board generates
- all live. Also includes a built-in 8-bank x 16-step sequencer
(sequencer.py), and each of those same 8 banks can additionally hold a
live-recorded MIDI take (midi_recorder.py) - a real-time capture of
whatever notes actually happened (from the board, the sequencer, or
Demo mode), played back at its original timing and exportable as a
standard .mid file.

Quick start:
    pip install pyserial numpy sounddevice
    python3 tools/synth_studio.py
(plug the board in first; if no /dev/cu.usbmodem* port is found, the app
still opens in Demo mode - click "Start Demo" to hear the synth engine
play a fixed riff with no hardware attached.)

Architecture (why threads are split this way):
    - serial-reader thread (midi_link.serial_reader_thread): parses the
      board's log, calls straight into SynthEngine.note_on/note_off/cc.
    - sequencer-player thread (sequencer.SequencerPlayer): steps through
      the active bank's step pattern at the configured tempo, also
      calling straight into SynthEngine.note_on/note_off - from the
      engine's point of view a sequencer step and a board note are
      indistinguishable, so both show up in the same waveform/log/
      piano-roll for free, AND get captured by an active MIDI recording
      with zero extra wiring (see below).
    - take-player thread (midi_recorder.MidiTakePlayer): replays a
      recorded take's notes at their original recorded timing.
    - sounddevice audio callback thread: calls SynthEngine.render() and
      feeds a small lock-protected ring buffer the waveform view reads
      from - kept separate from the Tk main thread since Tkinter is not
      thread-safe and audio callbacks must never block on GUI work.
    - Tk main thread: runs the event loop and a periodic (~30 Hz) timer
      that pulls cheap snapshots (SynthEngine.snapshot_for_ui(), the
      waveform ring buffer, SequencerPlayer/MidiTakePlayer state) and
      redraws - it never touches engine/voice/player internals directly.
      LiveRecordSession is fed from this same tick, ingesting the
      engine's own event_log snapshot - see midi_recorder.py's own
      header comment for why that means recording needs no per-source
      wiring at all.
"""
import argparse
import os
import sys
import threading
import time
from collections import deque

import numpy as np

from midi_link import find_default_port, note_label, serial_reader_thread
from synth_instruments import SynthEngine, INSTRUMENTS
from sequencer import (SequencerModel, SequencerPlayer, step_duration_seconds,
                        NUM_BANKS, NUM_STEPS, MIN_BPM, MAX_BPM, DEFAULT_BPM)
from midi_recorder import LiveRecordSession, MidiTakePlayer, save_midi_file

try:
    import tkinter as tk
    from tkinter import ttk
except ImportError:
    print("This tool needs Tkinter (part of the Python standard library on "
          "most installs, but sometimes a separate OS package - e.g. "
          "'brew install python-tk' on macOS Homebrew Python).", file=sys.stderr)
    sys.exit(1)

SAMPLE_RATE = 44100
BLOCK_SIZE = 256
UI_FPS = 30
RECORDINGS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "recordings")

INSTRUMENT_COLORS = [
    "#ff5f5f", "#ff9f4a", "#f4d03f", "#7ed957", "#3fd9c0",
    "#4aa3ff", "#8c6bff", "#e06bff", "#ff6bb3", "#c0c0c0",
]

BG = "#12141a"
PANEL_BG = "#1b1e27"
FG = "#eef0f5"
MUTED_FG = "#8991a5"
ACCENT = "#4aa3ff"


class WaveformBuffer:
    """Lock-protected ring buffer the audio callback writes into and the
    UI timer reads a snapshot of - the only data shared between the
    audio thread and the Tk thread besides the engine's own snapshot."""

    def __init__(self, size=2048):
        self.size = size
        self.buf = np.zeros(size, dtype=np.float32)
        self.lock = threading.Lock()

    def write(self, block: np.ndarray):
        n = len(block)
        with self.lock:
            if n >= self.size:
                self.buf[:] = block[-self.size:]
            else:
                self.buf[:-n] = self.buf[n:]
                self.buf[-n:] = block

    def snapshot(self) -> np.ndarray:
        with self.lock:
            return self.buf.copy()


class NoteHistory:
    """Turns the engine's flat note_on/note_off event log into note
    "spans" (start time, end time or still-open) for the piano-roll
    view - kept in the GUI, not the engine, since it's purely a display
    concern."""

    def __init__(self, max_spans=300):
        self.spans = deque(maxlen=max_spans)
        self.open = {}
        self.last_seen_ts = 0.0

    def ingest(self, events):
        for e in events:
            if e["t"] <= self.last_seen_ts:
                continue
            self.last_seen_ts = e["t"]
            if e["kind"] == "note_on":
                span = {"note": e["note"], "instrument": e["instrument"],
                        "start": e["t"], "end": None}
                self.spans.append(span)
                self.open[e["note"]] = span
            elif e["kind"] == "note_off":
                span = self.open.pop(e["note"], None)
                if span is not None:
                    span["end"] = e["t"]


class SynthStudioApp:
    def __init__(self, root, port, baud, device, demo_only=False, auto_close_after=None):
        self.root = root
        self.engine = SynthEngine(SAMPLE_RATE)
        self.waveform = WaveformBuffer()
        self.note_history = NoteHistory()
        self.stop_event = threading.Event()
        self.connection_status = "not connected"
        self.demo_thread = None
        self.demo_running = False
        self._tick_count = 0

        self.seq_model = SequencerModel()
        self.seq_player = SequencerPlayer(self.seq_model, self.engine.note_on, self.engine.note_off,
                                           on_step=self._on_seq_step)
        self._seq_playhead = -1  # written from the player thread, read from the Tk thread each
                                  # tick - a single int assignment, same reasoning as elsewhere
                                  # in this file for why that's safe without a lock

        # One recorded MIDI take slot per bank (parallel to seq_model's
        # one step-pattern per bank). While armed, recording always
        # follows whichever bank is currently selected - the same 8-bank
        # selector the step sequencer uses - rather than being locked to
        # one bank for the whole session: default is bank 1 (index 0),
        # switching banks while armed retargets recording to the new
        # bank without needing to stop/restart, and a bank's existing
        # take is only ever replaced when a switch or Stop finalizes a
        # segment that actually captured a note - see LiveRecordSession's
        # own docstring for the exact "never silently erases" rule.
        self.live_record = LiveRecordSession()
        self.takes = [None] * NUM_BANKS  # MidiTake or None, per bank
        self.take_player = MidiTakePlayer(self.engine.note_on, self.engine.note_off)
        self.last_saved_path = None

        self._build_ui()
        self._start_audio(device)

        if demo_only:
            self._set_status("Demo mode (no --port given)")
        else:
            self._connect_serial(port, baud)

        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self._tick()

        if auto_close_after is not None:
            self.start_demo()
            self._run_smoke_test_sequence(auto_close_after)
            self.root.after(int(auto_close_after * 1000), self._smoke_test_finish)

    # --- UI construction -------------------------------------------------
    def _build_ui(self):
        self.root.title("Young Piong Synth Studio")
        self.root.configure(bg=BG)
        self.root.geometry("1040x980")
        self.root.minsize(860, 760)

        header = tk.Frame(self.root, bg=BG)
        header.pack(fill="x", padx=16, pady=(14, 6))
        tk.Label(header, text="Young Piong Synth Studio", bg=BG, fg=FG,
                  font=("Helvetica", 18, "bold")).pack(side="left")
        status_col = tk.Frame(header, bg=BG)
        status_col.pack(side="right")
        self.status_var = tk.StringVar(value="starting...")
        tk.Label(status_col, textvariable=self.status_var, bg=BG, fg=MUTED_FG,
                  font=("Helvetica", 11), anchor="e", justify="right").pack(anchor="e")
        self.audio_device_var = tk.StringVar(value="audio out: (starting...)")
        tk.Label(status_col, textvariable=self.audio_device_var, bg=BG, fg=MUTED_FG,
                  font=("Helvetica", 9), anchor="e", justify="right").pack(anchor="e")

        # --- Instrument selector ---
        inst_panel = tk.Frame(self.root, bg=PANEL_BG)
        inst_panel.pack(fill="x", padx=16, pady=6)
        tk.Label(inst_panel, text="INSTRUMENTS", bg=PANEL_BG, fg=MUTED_FG,
                  font=("Helvetica", 9, "bold")).grid(row=0, column=0, columnspan=5,
                                                       sticky="w", padx=10, pady=(8, 2))
        self.inst_buttons = []
        for inst_id, name, desc in INSTRUMENTS:
            row, col = divmod(inst_id, 5)
            btn = tk.Button(inst_panel, text=f"{inst_id}: {name}",
                             command=lambda i=inst_id: self.select_instrument(i),
                             bg=PANEL_BG, fg=FG, activebackground=ACCENT,
                             relief="flat", font=("Helvetica", 10), padx=8, pady=8,
                             borderwidth=1)
            btn.grid(row=row + 1, column=col, sticky="ew", padx=4, pady=4)
            inst_panel.grid_columnconfigure(col, weight=1)
            self.inst_buttons.append(btn)
        self.desc_var = tk.StringVar(value=INSTRUMENTS[0][2])
        tk.Label(inst_panel, textvariable=self.desc_var, bg=PANEL_BG, fg=MUTED_FG,
                  font=("Helvetica", 9, "italic")).grid(row=3, column=0, columnspan=5,
                                                         sticky="w", padx=10, pady=(2, 8))

        # --- Visuals: waveform + level meter ---
        visuals = tk.Frame(self.root, bg=BG)
        visuals.pack(fill="both", expand=False, padx=16, pady=6)
        wf_frame = tk.Frame(visuals, bg=PANEL_BG)
        wf_frame.pack(side="left", fill="both", expand=True)
        tk.Label(wf_frame, text="LIVE WAVEFORM", bg=PANEL_BG, fg=MUTED_FG,
                  font=("Helvetica", 9, "bold")).pack(anchor="w", padx=10, pady=(8, 0))
        self.wave_canvas = tk.Canvas(wf_frame, height=110, bg="#05070b", highlightthickness=0)
        self.wave_canvas.pack(fill="both", expand=True, padx=10, pady=(2, 10))

        meter_frame = tk.Frame(visuals, bg=PANEL_BG, width=120)
        meter_frame.pack(side="left", fill="y", padx=(8, 0))
        meter_frame.pack_propagate(False)
        tk.Label(meter_frame, text="LEVEL", bg=PANEL_BG, fg=MUTED_FG,
                  font=("Helvetica", 9, "bold")).pack(anchor="w", padx=10, pady=(8, 0))
        self.meter_canvas = tk.Canvas(meter_frame, bg="#05070b", highlightthickness=0)
        self.meter_canvas.pack(fill="both", expand=True, padx=10, pady=(2, 10))
        self.meter_level = 0.0

        # --- Piano roll ---
        roll_frame = tk.Frame(self.root, bg=PANEL_BG)
        roll_frame.pack(fill="both", expand=True, padx=16, pady=6)
        roll_header = tk.Frame(roll_frame, bg=PANEL_BG)
        roll_header.pack(fill="x")
        tk.Label(roll_header, text="MELODY (piano roll, scrolling)", bg=PANEL_BG, fg=MUTED_FG,
                  font=("Helvetica", 9, "bold")).pack(side="left", padx=10, pady=(8, 0))
        self.held_var = tk.StringVar(value="Held: -")
        tk.Label(roll_header, textvariable=self.held_var, bg=PANEL_BG, fg=FG,
                  font=("Helvetica", 10, "bold")).pack(side="right", padx=10, pady=(8, 0))
        self.roll_canvas = tk.Canvas(roll_frame, bg="#05070b", highlightthickness=0)
        self.roll_canvas.pack(fill="both", expand=True, padx=10, pady=(2, 10))

        self._build_sequencer_ui(self.root)

        # --- Log + controls ---
        bottom = tk.Frame(self.root, bg=BG)
        bottom.pack(fill="both", expand=False, padx=16, pady=(0, 14))

        log_frame = tk.Frame(bottom, bg=PANEL_BG)
        log_frame.pack(side="left", fill="both", expand=True)
        tk.Label(log_frame, text="MIDI EVENT LOG", bg=PANEL_BG, fg=MUTED_FG,
                  font=("Helvetica", 9, "bold")).pack(anchor="w", padx=10, pady=(8, 0))
        log_inner = tk.Frame(log_frame, bg=PANEL_BG)
        log_inner.pack(fill="both", expand=True, padx=10, pady=(2, 10))
        self.log_text = tk.Text(log_inner, height=8, bg="#05070b", fg=FG, insertbackground=FG,
                                 font=("Menlo", 10), relief="flat", state="disabled", wrap="none")
        log_scroll = tk.Scrollbar(log_inner, command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=log_scroll.set)
        self.log_text.pack(side="left", fill="both", expand=True)
        log_scroll.pack(side="right", fill="y")

        controls = tk.Frame(bottom, bg=BG, width=170)
        controls.pack(side="left", fill="y", padx=(10, 0))
        controls.pack_propagate(False)
        self.demo_btn = tk.Button(controls, text="▶ Start Demo", command=self.toggle_demo,
                                   bg=ACCENT, fg="#04121f", relief="flat", font=("Helvetica", 10, "bold"),
                                   padx=6, pady=8)
        self.demo_btn.pack(fill="x", pady=(6, 4))
        tk.Button(controls, text="Test Note", command=self.play_test_note,
                  bg=PANEL_BG, fg=FG, relief="flat", font=("Helvetica", 10), padx=6, pady=8
                  ).pack(fill="x", pady=4)
        tk.Button(controls, text="Panic (all notes off)", command=self.panic,
                  bg="#3a1620", fg="#ffb3c0", relief="flat", font=("Helvetica", 10), padx=6, pady=8
                  ).pack(fill="x", pady=4)
        tk.Button(controls, text="Reconnect", command=self.reconnect,
                  bg=PANEL_BG, fg=FG, relief="flat", font=("Helvetica", 10), padx=6, pady=8
                  ).pack(fill="x", pady=4)

        tk.Label(controls, text="Output device", bg=BG, fg=MUTED_FG,
                  font=("Helvetica", 9)).pack(anchor="w", pady=(10, 0))
        self.device_choice = ttk.Combobox(controls, state="readonly", font=("Helvetica", 9))
        self.device_choice.pack(fill="x", pady=(2, 4))
        self.device_choice.bind("<<ComboboxSelected>>", self._on_device_chosen)
        self._populate_output_devices()

        self.voices_var = tk.StringVar(value="voices: 0")
        tk.Label(controls, textvariable=self.voices_var, bg=BG, fg=MUTED_FG,
                  font=("Helvetica", 9)).pack(anchor="w", pady=(10, 0))
        self.expr_var = tk.StringVar(value="expression: 100%")
        tk.Label(controls, textvariable=self.expr_var, bg=BG, fg=MUTED_FG,
                  font=("Helvetica", 9)).pack(anchor="w")

        self.select_instrument(0)

    def _build_sequencer_ui(self, parent):
        seq_frame = tk.Frame(parent, bg=PANEL_BG)
        seq_frame.pack(fill="x", padx=16, pady=6)

        top_row = tk.Frame(seq_frame, bg=PANEL_BG)
        top_row.pack(fill="x", padx=10, pady=(8, 4))
        tk.Label(top_row, text="SEQUENCER", bg=PANEL_BG, fg=MUTED_FG,
                  font=("Helvetica", 9, "bold")).pack(side="left")

        play_col = tk.Frame(top_row, bg=PANEL_BG)
        play_col.pack(side="right")
        self.seq_play_btn = tk.Button(play_col, text="▶ Play pattern", command=self._toggle_sequencer,
                                       bg=ACCENT, fg="#04121f", relief="flat",
                                       font=("Helvetica", 10, "bold"), padx=10, pady=4)
        self.seq_play_btn.pack(side="right")

        # --- MIDI take recording: Record / Play take / Save, all acting
        # on whichever bank is currently selected (mirrors the step
        # pattern - one recorded take slot per bank, same 8-bank
        # selector below drives both). ---
        rec_row = tk.Frame(seq_frame, bg=PANEL_BG)
        rec_row.pack(fill="x", padx=10, pady=(0, 6))
        tk.Label(rec_row, text="MIDI RECORDING (per bank)", bg=PANEL_BG, fg=MUTED_FG,
                  font=("Helvetica", 9, "bold")).pack(side="left")
        self.take_save_btn = tk.Button(rec_row, text="💾 Save", command=self._on_save_take,
                                        bg=PANEL_BG, fg=FG, relief="flat",
                                        font=("Helvetica", 10, "bold"), padx=10, pady=4, state="disabled")
        self.take_save_btn.pack(side="right")
        self.take_play_btn = tk.Button(rec_row, text="▶ Play take", command=self._toggle_take_playback,
                                        bg=PANEL_BG, fg=FG, relief="flat",
                                        font=("Helvetica", 10, "bold"), padx=10, pady=4, state="disabled")
        self.take_play_btn.pack(side="right", padx=8)
        self.record_btn = tk.Button(rec_row, text="● Record", command=self._toggle_recording,
                                     bg="#3a1620", fg="#ffb3c0", relief="flat",
                                     font=("Helvetica", 10, "bold"), padx=10, pady=4)
        self.record_btn.pack(side="right")
        self.record_status_var = tk.StringVar(value="Bank 1: no take")
        tk.Label(rec_row, textvariable=self.record_status_var, bg=PANEL_BG, fg=MUTED_FG,
                  font=("Helvetica", 9)).pack(side="left", padx=(16, 0))

        # --- Tempo + note-to-place + clear ---
        tempo_row = tk.Frame(seq_frame, bg=PANEL_BG)
        tempo_row.pack(fill="x", padx=10, pady=(0, 6))
        tk.Label(tempo_row, text="Tempo", bg=PANEL_BG, fg=MUTED_FG, font=("Helvetica", 9)
                  ).pack(side="left")
        # Deliberately NOT using Scale's own `command=` here - combined
        # with a bound `variable`, it's a well-known unreliable pairing
        # in Tkinter (the widget's internal Set doesn't consistently
        # invoke -command through every interaction path, including its
        # own .set() method - confirmed while testing this exact
        # control). A variable trace fires on every real change
        # regardless of how the value changed, so it's used instead.
        self.seq_bpm_var = tk.IntVar(value=DEFAULT_BPM)
        self.seq_bpm_var.trace_add("write", self._on_bpm_var_changed)
        tk.Scale(tempo_row, from_=MIN_BPM, to=MAX_BPM, orient="horizontal", length=180,
                 variable=self.seq_bpm_var, bg=PANEL_BG, fg=FG,
                 troughcolor="#0e1016", highlightthickness=0, relief="flat",
                 activebackground=ACCENT).pack(side="left", padx=(6, 4))
        self.seq_bpm_label_var = tk.StringVar(value=f"{DEFAULT_BPM} BPM")
        tk.Label(tempo_row, textvariable=self.seq_bpm_label_var, bg=PANEL_BG, fg=FG,
                  font=("Helvetica", 9, "bold"), width=8).pack(side="left", padx=(0, 20))

        tk.Label(tempo_row, text="Note to place", bg=PANEL_BG, fg=MUTED_FG, font=("Helvetica", 9)
                  ).pack(side="left")
        self.seq_note_var = tk.IntVar(value=60)
        tk.Spinbox(tempo_row, from_=0, to=127, textvariable=self.seq_note_var, width=4,
                   command=self._on_seq_note_changed, bg="#0e1016", fg=FG, buttonbackground=PANEL_BG,
                   relief="flat", justify="center").pack(side="left", padx=(6, 4))
        self.seq_note_label_var = tk.StringVar(value=note_label(60))
        tk.Label(tempo_row, textvariable=self.seq_note_label_var, bg=PANEL_BG, fg=FG,
                  font=("Helvetica", 9, "bold"), width=4).pack(side="left", padx=(0, 20))

        tk.Button(tempo_row, text="Clear bank", command=self._on_clear_bank,
                  bg=PANEL_BG, fg=FG, relief="flat", font=("Helvetica", 9)
                  ).pack(side="left")
        tk.Label(tempo_row, text="left-click: place/remove note · right-click: accent",
                  bg=PANEL_BG, fg=MUTED_FG, font=("Helvetica", 8, "italic")
                  ).pack(side="right")

        # --- Bank selector ---
        bank_row = tk.Frame(seq_frame, bg=PANEL_BG)
        bank_row.pack(fill="x", padx=10, pady=(0, 6))
        tk.Label(bank_row, text="Bank", bg=PANEL_BG, fg=MUTED_FG, font=("Helvetica", 9)
                  ).pack(side="left", padx=(0, 8))
        self.seq_bank_buttons = []
        for b in range(NUM_BANKS):
            btn = tk.Button(bank_row, text=str(b + 1), width=3,
                             command=lambda b=b: self._on_bank_select(b),
                             bg=PANEL_BG, fg=FG, relief="flat", font=("Helvetica", 10, "bold"))
            btn.pack(side="left", padx=2)
            self.seq_bank_buttons.append(btn)

        # --- Step grid ---
        step_row = tk.Frame(seq_frame, bg=PANEL_BG)
        step_row.pack(fill="x", padx=10, pady=(0, 10))
        self.seq_step_buttons = []
        for i in range(NUM_STEPS):
            btn = tk.Button(step_row, text="", width=4, height=2,
                             command=lambda i=i: self._on_step_click(i),
                             bg="#232838", fg=MUTED_FG, relief="flat", font=("Helvetica", 9, "bold"),
                             highlightthickness=2, highlightbackground=PANEL_BG)
            # a small gap after every 4th step to visually group 16ths into beats
            btn.grid(row=0, column=i, padx=(2, 10 if (i + 1) % 4 == 0 and i != NUM_STEPS - 1 else 2))
            btn.bind("<Button-2>", lambda e, i=i: self._on_step_accent(i))
            btn.bind("<Button-3>", lambda e, i=i: self._on_step_accent(i))
            step_row.grid_columnconfigure(i, weight=1)
            self.seq_step_buttons.append(btn)

        self._redraw_seq_bank_buttons()
        self._redraw_seq_steps()
        self._update_record_status_label()

    # --- Actions -----------------------------------------------------
    def select_instrument(self, inst_id):
        self.engine.set_instrument(inst_id)
        for i, btn in enumerate(self.inst_buttons):
            btn.configure(bg=INSTRUMENT_COLORS[i] if i == inst_id else PANEL_BG,
                          fg="#04121f" if i == inst_id else FG)
        self.desc_var.set(INSTRUMENTS[inst_id][2])

    def play_test_note(self):
        note = 60
        self.engine.note_on(note, 100)
        self.root.after(400, lambda: self.engine.note_off(note))

    def panic(self):
        for note in list(self.engine.held_notes.keys()):
            self.engine.note_off(note)

    def toggle_demo(self):
        if self.demo_running:
            self.stop_demo()
        else:
            self.start_demo()

    def start_demo(self):
        if self.demo_running:
            return
        self.demo_running = True
        self.demo_btn.configure(text="■ Stop Demo")

        def demo_loop():
            riff = [60, 63, 65, 60, 67, 65, 63, 58]
            i = 0
            while self.demo_running and not self.stop_event.is_set():
                note = riff[i % len(riff)]
                vel = 110 if i % 4 == 0 else 75
                self.engine.note_on(note, vel)
                time.sleep(0.22)
                self.engine.note_off(note)
                time.sleep(0.03)
                i += 1

        self.demo_thread = threading.Thread(target=demo_loop, daemon=True)
        self.demo_thread.start()

    def stop_demo(self):
        self.demo_running = False
        self.demo_btn.configure(text="▶ Start Demo")

    # --- Sequencer -----------------------------------------------------
    def _toggle_sequencer(self):
        if self.seq_player.playing:
            self.seq_player.stop()
            self.seq_play_btn.configure(text="▶ Play pattern")
        else:
            self.seq_player.start()
            self.seq_play_btn.configure(text="■ Stop")

    def _ensure_pattern_playing(self):
        """Starts the step sequencer if it isn't already running - Record
        and Play take both call this first, so pressing either actually
        has something being generated to capture/loop instead of
        silently capturing silence because the pattern was never
        started separately. Never stops it - Record/Play take finishing
        doesn't interrupt a groove the user may still want running;
        that's still Play pattern/Stop's own job. Since SequencerPlayer
        already re-reads the active bank's steps every single step (see
        sequencer.py), a running pattern automatically follows the same
        bank switches LiveRecordSession is already following while
        armed - no extra sync code needed for that part, they're both
        already reading the one shared self.seq_model.active_bank."""
        if not self.seq_player.playing:
            self.seq_player.start()
            self.seq_play_btn.configure(text="■ Stop")

    def _on_seq_step(self, step_index):
        # Called from SequencerPlayer's own background thread - must
        # never touch a Tk widget here. Just record the index; the ~30Hz
        # _tick() redraw loop (Tk thread) picks it up and draws the
        # playhead, same pattern as every other cross-thread value in
        # this app (WaveformBuffer, SynthEngine.snapshot_for_ui()).
        self._seq_playhead = step_index

    def _on_bpm_var_changed(self, *_trace_args):
        try:
            bpm = self.seq_bpm_var.get()
        except tk.TclError:
            return
        self.seq_model.set_bpm(bpm)
        self.seq_bpm_label_var.set(f"{self.seq_model.bpm} BPM")

    def _on_seq_note_changed(self):
        try:
            n = max(0, min(127, int(self.seq_note_var.get())))
        except (ValueError, tk.TclError):
            return
        self.seq_note_label_var.set(note_label(n))

    def _on_bank_select(self, bank_index):
        # If armed, this both finalizes whatever was captured for the
        # bank being left (only if non-empty - see LiveRecordSession)
        # and retargets recording to the new bank, all in one call.
        finished = self.live_record.set_active_bank(bank_index)
        if finished is not None:
            bank, take = finished
            self.takes[bank] = take
        self.seq_model.select_bank(bank_index)
        self._redraw_seq_bank_buttons()
        self._redraw_seq_steps()
        self._update_record_status_label()

    def _on_step_click(self, step_index):
        try:
            n = max(0, min(127, int(self.seq_note_var.get())))
        except (ValueError, tk.TclError):
            n = 60
        self.seq_model.toggle_step(self.seq_model.active_bank, step_index, n)
        self._redraw_seq_steps()

    def _on_step_accent(self, step_index):
        self.seq_model.toggle_accent(self.seq_model.active_bank, step_index)
        self._redraw_seq_steps()

    def _on_clear_bank(self):
        self.seq_model.clear_bank(self.seq_model.active_bank)
        self._redraw_seq_steps()

    def _redraw_seq_bank_buttons(self):
        active = self.seq_model.active_bank
        for i, btn in enumerate(self.seq_bank_buttons):
            btn.configure(bg=ACCENT if i == active else PANEL_BG,
                          fg="#04121f" if i == active else FG)

    def _redraw_seq_steps(self):
        steps = self.seq_model.active_steps()
        playhead = self._seq_playhead if self.seq_player.playing else -1
        inst_color = INSTRUMENT_COLORS[self.engine.current_instrument % len(INSTRUMENT_COLORS)]
        for i, (btn, step) in enumerate(zip(self.seq_step_buttons, steps)):
            if step.on:
                bg = "#ffcf5c" if step.accent else inst_color
                fg = "#04121f"
                text = note_label(step.note)
            else:
                bg = "#232838"
                fg = MUTED_FG
                text = ""
            btn.configure(text=text, bg=bg, fg=fg,
                          highlightbackground="#ffffff" if i == playhead else PANEL_BG)

    # --- MIDI take recording (Record / Play take / Save, per bank) ----
    #
    # Recording is a live looper, not a one-shot-per-bank capture: while
    # armed, it always follows whichever bank is currently selected (see
    # _on_bank_select() above, which retargets it) - by default that's
    # bank 1. A bank's existing take is only ever overwritten when a
    # segment that actually captured a note gets finalized for it
    # (LiveRecordSession guarantees this - see its docstring) - merely
    # switching to, or arming on top of, a bank that already has a take
    # never erases it.
    def _toggle_recording(self):
        if self.live_record.armed:
            finished = self.live_record.disarm()
            self.record_btn.configure(text="● Record", bg="#3a1620", fg="#ffb3c0")
            if finished is not None:
                bank, take = finished
                self.takes[bank] = take
            self._update_record_status_label()
        else:
            self._ensure_pattern_playing()
            self.live_record.arm()
            self.record_btn.configure(text="■ Stop Rec", bg="#ff5f5f", fg="#04121f")
            self.record_status_var.set(f"Bank {self.live_record.current_bank + 1}: recording...")

    def _toggle_take_playback(self):
        take = self.takes[self.seq_model.active_bank]
        if self.take_player.playing:
            self.take_player.stop()
            self.take_play_btn.configure(text="▶ Play take", bg=PANEL_BG, fg=FG)
        elif take is not None and not take.is_empty():
            self._ensure_pattern_playing()
            self.take_player.play(take, loop=True)  # loops automatically until Stop
            self.take_play_btn.configure(text="■ Stop take", bg=ACCENT, fg="#04121f")

    def _on_save_take(self):
        bank = self.seq_model.active_bank
        take = self.takes[bank]
        if take is None or take.is_empty():
            return
        os.makedirs(RECORDINGS_DIR, exist_ok=True)
        filename = f"take_bank{bank + 1}_{time.strftime('%Y%m%d_%H%M%S')}.mid"
        path = os.path.join(RECORDINGS_DIR, filename)
        save_midi_file(take, path)
        self.last_saved_path = path
        self._flash_record_status(f"Bank {bank + 1}: saved {filename}")

    def _flash_record_status(self, text, hold_ms=2500):
        """Shows `text` immediately, then reverts to the normal per-bank
        take summary after hold_ms - used for one-shot confirmations
        (e.g. "saved ...") so they don't get silently overwritten by the
        very next _tick() before anyone reads them, but also don't sit
        there stale forever."""
        self.record_status_var.set(text)
        self.root.after(hold_ms, self._update_record_status_label)

    def _update_record_status_label(self):
        if self.live_record.armed:
            return  # _tick() is already showing live elapsed time for this
        bank = self.seq_model.active_bank
        take = self.takes[bank]
        if take is None or take.is_empty():
            self.record_status_var.set(f"Bank {bank + 1}: no take")
        else:
            self.record_status_var.set(
                f"Bank {bank + 1}: {take.duration:.1f}s take, {take.note_on_count()} notes")
        can_act = take is not None and not take.is_empty()
        self.take_play_btn.configure(state="normal" if can_act else "disabled")
        self.take_save_btn.configure(state="normal" if can_act else "disabled")

    def reconnect(self):
        port = find_default_port()
        if port is None:
            self._set_status("Reconnect failed: no /dev/cu.usbmodem* port found")
            return
        self._connect_serial(port, 115200)

    def _connect_serial(self, port, baud):
        if port is None:
            port = find_default_port()
        if port is None:
            self._set_status("No board found - plug it in and click Reconnect, or Start Demo")
            return

        def on_connect(p):
            self._set_status(f"connected: {p} @ {baud}")

        def on_note_on(ch, note, vel):
            del ch
            self.engine.note_on(note, vel)

        def on_note_off(ch, note, vel):
            del ch, vel
            self.engine.note_off(note)

        def on_cc(ch, cc, val):
            del ch
            self.engine.cc(cc, val)

        def on_error(exc):
            self._set_status(f"serial error: {exc}")

        threading.Thread(
            target=serial_reader_thread,
            args=(port, baud, on_note_on, on_note_off, on_cc, self.stop_event),
            kwargs={"on_connect": on_connect, "on_error": on_error},
            daemon=True,
        ).start()

    def _set_status(self, text):
        self.connection_status = text
        self.status_var.set(text)

    def _populate_output_devices(self):
        import sounddevice as sd
        devices = sd.query_devices()
        self._output_device_indices = [i for i, d in enumerate(devices) if d["max_output_channels"] > 0]
        labels = [f"[{i}] {devices[i]['name']}" for i in self._output_device_indices]
        self.device_choice["values"] = labels
        if self._output_device_indices:
            try:
                default_out = sd.default.device[1]
            except TypeError:
                default_out = sd.default.device
            if default_out in self._output_device_indices:
                self.device_choice.current(self._output_device_indices.index(default_out))
            else:
                self.device_choice.current(0)

    def _on_device_chosen(self, event):
        del event
        idx = self.device_choice.current()
        if 0 <= idx < len(self._output_device_indices):
            self._start_audio(self._output_device_indices[idx])

    def _start_audio(self, device):
        import sounddevice as sd

        # Replacing a running stream (e.g. the user picked a different
        # output device) - stop the old one first so two callbacks never
        # run concurrently against the same engine/waveform buffer.
        old_stream = getattr(self, "stream", None)
        if old_stream is not None:
            try:
                old_stream.stop()
                old_stream.close()
            except Exception:
                pass

        def audio_callback(outdata, frames, time_info, status):
            del time_info
            if status:
                pass  # underruns can happen while the UI thread is busy; audible, not fatal
            block = self.engine.render(frames)
            self.waveform.write(block)
            outdata[:, 0] = block

        try:
            self.stream = sd.OutputStream(samplerate=SAMPLE_RATE, blocksize=BLOCK_SIZE,
                                           channels=1, dtype="float32", device=device,
                                           callback=audio_callback)
            self.stream.start()
            resolved = device if device is not None else sd.default.device[1]
            device_name = sd.query_devices(resolved)["name"]
            self.audio_device_var.set(f"audio out: [{resolved}] {device_name}")
        except Exception as exc:
            self.stream = None
            self._set_status(f"audio output failed to start: {exc}")

    # --- Redraw loop ---------------------------------------------------
    def _tick(self):
        self._tick_count += 1
        snap = self.engine.snapshot_for_ui()
        self.note_history.ingest(snap["events"])
        self.live_record.ingest(snap["events"])
        self._draw_waveform()
        self._draw_meter()
        self._draw_piano_roll()
        self._redraw_seq_steps()
        self._update_log(snap["events"])
        self.voices_var.set(f"voices: {snap['active_voices']}")
        self.expr_var.set(f"expression: {int(snap['expression'] * 100)}%")
        held = snap["held_notes"]
        if held:
            parts = [f"{note_label(n)} (vel {v[1]})" for n, v in sorted(held.items())]
            self.held_var.set("Held: " + ", ".join(parts))
        else:
            self.held_var.set("Held: -")
        if self.live_record.armed:
            self.record_status_var.set(
                f"Bank {self.live_record.current_bank + 1}: recording... {self.live_record.elapsed():0.1f}s")
        # The take player can finish on its own (reaching the end of the
        # take) without any button click, so its button state is synced
        # here every tick rather than only from _toggle_take_playback().
        if self.take_player.playing:
            self.take_play_btn.configure(text="■ Stop take", bg=ACCENT, fg="#04121f")
        else:
            self.take_play_btn.configure(text="▶ Play take", bg=PANEL_BG, fg=FG)
        if not self.stop_event.is_set():
            self.root.after(int(1000 / UI_FPS), self._tick)

    def _draw_waveform(self):
        c = self.wave_canvas
        c.delete("all")
        w = c.winfo_width() or 600
        h = c.winfo_height() or 110
        data = self.waveform.snapshot()
        if w < 4 or len(data) < 2:
            return
        step = max(1, len(data) // w)
        mid = h / 2
        points = []
        for x in range(0, len(data) - step, step):
            v = float(np.max(np.abs(data[x:x + step])))
            v = min(v, 1.0)
            px = (x / len(data)) * w
            points.append((px, mid - v * mid))
            points.append((px, mid + v * mid))
        c.create_line(0, mid, w, mid, fill="#233", width=1)
        if len(points) >= 2:
            flat = [coord for pt in points for coord in pt]
            c.create_line(*flat, fill=ACCENT, width=1)

    def _draw_meter(self):
        c = self.meter_canvas
        c.delete("all")
        w = c.winfo_width() or 40
        h = c.winfo_height() or 100
        data = self.waveform.snapshot()
        peak = float(np.max(np.abs(data))) if len(data) else 0.0
        # fast attack, slow release, like a VU meter
        if peak > self.meter_level:
            self.meter_level = peak
        else:
            self.meter_level += (peak - self.meter_level) * 0.15
        level = max(0.0, min(1.0, self.meter_level))
        fill_h = level * h
        color = "#4aff88" if level < 0.7 else ("#f4d03f" if level < 0.9 else "#ff5f5f")
        c.create_rectangle(4, h - fill_h, w - 4, h, fill=color, outline="")
        for frac in (0.25, 0.5, 0.75):
            y = h * (1 - frac)
            c.create_line(0, y, w, y, fill="#233", width=1)

    def _draw_piano_roll(self):
        c = self.roll_canvas
        c.delete("all")
        w = c.winfo_width() or 600
        h = c.winfo_height() or 200
        if w < 4 or h < 4:
            return
        now = time.time()
        window_s = 8.0
        note_lo, note_hi = 36, 96
        note_span = max(1, note_hi - note_lo)

        for frac in (0.25, 0.5, 0.75):
            c.create_line(0, h * frac, w, h * frac, fill="#1a1d26", width=1)

        for span in self.note_history.spans:
            start = span["start"]
            end = span["end"] if span["end"] is not None else now
            if now - start > window_s and now - end > window_s:
                continue
            x_start = w * (1.0 - (now - start) / window_s)
            x_end = w * (1.0 - (now - end) / window_s)
            x_start = max(0.0, min(w, x_start))
            x_end = max(0.0, min(w, x_end))
            note = max(note_lo, min(note_hi, span["note"]))
            y = h - ((note - note_lo) / note_span) * h
            color = INSTRUMENT_COLORS[span["instrument"] % len(INSTRUMENT_COLORS)]
            c.create_rectangle(min(x_start, x_end), y - 4, max(x_start, x_end), y + 4,
                                fill=color, outline="")

        c.create_line(w - 2, 0, w - 2, h, fill="#334", width=1)  # "now" edge

    def _update_log(self, events):
        if not events:
            return
        last_shown = getattr(self, "_last_logged_ts", 0.0)
        new = [e for e in events if e["t"] > last_shown]
        if not new:
            return
        self._last_logged_ts = events[-1]["t"]
        self.log_text.configure(state="normal")
        for e in new:
            ts = time.strftime("%H:%M:%S", time.localtime(e["t"]))
            if e["kind"] == "note_on":
                line = f"{ts}  NOTE_ON  {note_label(e['note']):<4} vel={e['velocity']:3d}  inst={INSTRUMENTS[e['instrument']][1]}"
            elif e["kind"] == "note_off":
                line = f"{ts}  NOTE_OFF {note_label(e['note']):<4}"
            elif e["kind"] == "cc":
                line = f"{ts}  CC11(expr) = {e['value']:3d}"
            else:
                continue
            self.log_text.insert("end", line + "\n")
        # cap the visible log so this Text widget never grows unbounded
        n_lines = int(self.log_text.index("end-1c").split(".")[0])
        if n_lines > 500:
            self.log_text.delete("1.0", f"{n_lines - 500}.0")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def _run_smoke_test_sequence(self, total_seconds):
        """Exercises the sequencer + live-looper MIDI recorder end to
        end, not just the demo riff - programs a pattern, starts
        playback (which also feeds the recorder, since it captures
        whatever the engine actually plays regardless of source), arms
        Record on the default bank (bank 1), then - via scheduled
        callbacks spread across the smoke-test window, not all at once
        synchronously before anything can actually play - switches to
        bank 2 (should finalize a real take for bank 1). Demo mode and
        pattern playback are then stopped (silence - nothing left to
        generate notes) before switching to bank 5, so bank 5 correctly
        demonstrates staying empty when nothing was actually played into
        it - switching to it must not fabricate a take just because it
        became the target. All verified in _smoke_test_finish() below."""
        self.select_instrument(0)
        for step_index, note in ((0, 48), (4, 55), (8, 60), (12, 63)):
            self.seq_model.toggle_step(0, step_index, note)
        self.seq_model.toggle_accent(0, 8)
        self.seq_bpm_var.set(180)
        # Deliberately NOT calling _toggle_sequencer() here - Record
        # itself must start the pattern automatically (see
        # _ensure_pattern_playing()); this is what actually verifies
        # that wiring rather than masking it by pre-starting playback.
        assert not self.seq_player.playing
        self._toggle_recording()   # arm MIDI take recording - defaults to bank 1
        assert self.seq_player.playing, "Record must auto-start the sequencer pattern"

        switch1_ms = int(total_seconds * 1000 * 0.30)
        go_silent_ms = int(total_seconds * 1000 * 0.55)
        switch2_ms = int(total_seconds * 1000 * 0.65)

        def go_silent():
            self.stop_demo()
            self.seq_player.stop()

        self.root.after(switch1_ms, lambda: self._on_bank_select(1))  # -> bank 2
        self.root.after(go_silent_ms, go_silent)
        self.root.after(switch2_ms, lambda: self._on_bank_select(4))  # -> bank 5 (must stay empty: silent by now)

    def _smoke_test_finish(self):
        seq_was_playing = self.seq_player.playing
        if self.live_record.armed:
            self._toggle_recording()  # stop + store, exactly like a user clicking the button
        self.seq_player.stop(join=True)

        bank1_ok = self.takes[0] is not None and not self.takes[0].is_empty()
        bank2_ok = self.takes[1] is not None and not self.takes[1].is_empty()
        bank5_empty = self.takes[4] is None  # never played into - must NOT have been created

        saved_path = None
        if bank1_ok:
            self._on_bank_select(0)
            self._on_save_take()
            saved_path = self.last_saved_path
        save_ok = saved_path is not None and os.path.exists(saved_path) and os.path.getsize(saved_path) > 20

        # Also exercise take playback itself (looping), not just capture+save.
        take_playback_ran = False
        if bank1_ok:
            self._toggle_take_playback()
            take_playback_ran = self.take_player.playing
            time.sleep(0.05)
            still_playing_after_a_moment = self.take_player.playing  # loop=True: must not have stopped on its own
            self.take_player.stop(join=True)
            take_playback_ran = take_playback_ran and still_playing_after_a_moment

        print(f"[smoke-test] {self._tick_count} UI redraw ticks completed without exception, "
              f"active_voices last seen={self.engine.snapshot_for_ui()['active_voices']}, "
              f"connection_status={self.connection_status!r}, "
              f"events_logged={len(self.engine.event_log)}, "
              f"sequencer_was_playing={seq_was_playing}, "
              f"bank1_take_captured={bank1_ok} "
              f"({self.takes[0].note_on_count() if bank1_ok else 0} notes), "
              f"bank2_take_captured_after_bank_switch={bank2_ok} "
              f"({self.takes[1].note_on_count() if bank2_ok else 0} notes), "
              f"bank5_correctly_left_empty={bank5_empty}, "
              f"take_looped_without_stopping={take_playback_ran}, "
              f"take_saved={save_ok} ({saved_path})")
        self._on_close()

    def _on_close(self):
        self.stop_event.set()
        self.demo_running = False
        self.seq_player.stop()
        self.take_player.stop()
        if getattr(self, "stream", None) is not None:
            try:
                self.stream.stop()
                self.stream.close()
            except Exception:
                pass
        self.root.destroy()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=None, help="serial port (default: auto-detect /dev/cu.usbmodem*)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--device", type=int, default=None, help="sounddevice output device index (see --list-devices)")
    ap.add_argument("--list-devices", action="store_true")
    ap.add_argument("--demo", action="store_true", help="start in Demo mode, skip auto-connecting to serial")
    ap.add_argument("--smoke-test", type=float, default=None,
                     help="internal: open the window, run the demo riff, auto-close after N seconds "
                          "and print a one-line summary (used to verify the app starts and runs "
                          "cleanly without a human at the keyboard)")
    args = ap.parse_args()

    if args.list_devices:
        import sounddevice as sd
        print(sd.query_devices())
        print("default:", sd.default.device)
        return

    root = tk.Tk()
    app = SynthStudioApp(root, args.port, args.baud, args.device,
                          demo_only=args.demo, auto_close_after=args.smoke_test)
    del app
    root.mainloop()


if __name__ == "__main__":
    main()
