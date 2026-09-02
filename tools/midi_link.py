#!/usr/bin/env python3
"""
midi_link.py - shared serial-MIDI-log parsing for YoungPiongMidi's
host-side Python tools (tools/acid_synth_monitor.py, tools/synth_studio.py).

The board has no wire MIDI transport yet (BLE/UART are Milestones 8-9 -
see docs/midi.md), so every host tool that reacts to MIDI events reads
the same thing: the plain-text NOTE_ON/NOTE_OFF/CC lines midi_task logs
over the serial console (components/midi/midi.c's log_event()). This
module exists so that parsing format and port autodetection live in
exactly one place instead of being copy-pasted between tools.
"""
import glob
import re

LOG_NOTE_ON = re.compile(r"NOTE_ON\s+ch=(\d+)\s+note=(\d+)\s+vel=(\d+)")
LOG_NOTE_OFF = re.compile(r"NOTE_OFF\s+ch=(\d+)\s+note=(\d+)\s+vel=(\d+)")
LOG_CC = re.compile(r"CC\s+ch=(\d+)\s+cc=(\d+)\s+val=(\d+)")

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def note_label(note: int) -> str:
    return f"{NOTE_NAMES[note % 12]}{note // 12 - 1}"


def midi_note_to_freq(note: int) -> float:
    return 440.0 * (2.0 ** ((note - 69) / 12.0))


def find_default_port():
    """Returns the first /dev/cu.usbmodem* port, or None if none is
    attached - callers decide how to handle "no hardware found" (exit
    with an error, or fall back to a self-test mode)."""
    candidates = sorted(glob.glob("/dev/cu.usbmodem*"))
    return candidates[0] if candidates else None


def serial_reader_thread(port, baud, on_note_on, on_note_off, on_cc, stop_event,
                          on_connect=None, on_error=None, on_raw_line=None):
    """Generic serial reader: reads lines from `port` and calls
    on_note_on(ch, note, vel) / on_note_off(ch, note, vel) /
    on_cc(ch, cc, val) as matching lines arrive.

    Runs until stop_event is set, or the port errors out - in which case
    on_error(exc) is called once (if given) and the thread simply
    returns rather than raising, since this always runs as a daemon
    thread where a raised exception would otherwise vanish silently.

    `on_raw_line`, if given, is called with every line read (including
    ones that don't match any pattern) - useful for a GUI that wants to
    show a live raw log independent of MIDI parsing.

    Import of `serial` (pyserial) is deferred into this function so that
    tools which only need the parsing helpers above (regexes, note_label)
    don't require pyserial to be installed just to import this module.
    """
    import serial

    try:
        ser = serial.Serial(port, baud, timeout=0.5)
    except Exception as exc:
        if on_error:
            on_error(exc)
        return

    if on_connect:
        on_connect(port)

    try:
        while not stop_event.is_set():
            raw = ser.readline()
            if not raw:
                continue
            try:
                line = raw.decode(errors="replace").rstrip()
            except Exception:
                continue

            if on_raw_line:
                on_raw_line(line)

            m = LOG_NOTE_ON.search(line)
            if m:
                ch, note, vel = (int(x) for x in m.groups())
                on_note_on(ch, note, vel)
                continue
            m = LOG_NOTE_OFF.search(line)
            if m:
                ch, note, vel = (int(x) for x in m.groups())
                on_note_off(ch, note, vel)
                continue
            m = LOG_CC.search(line)
            if m:
                ch, cc, val = (int(x) for x in m.groups())
                on_cc(ch, cc, val)
                continue
    except Exception as exc:
        if on_error:
            on_error(exc)
    finally:
        ser.close()
