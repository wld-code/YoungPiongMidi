#!/usr/bin/env python3
"""
midi_link.py - shared serial-MIDI-log parsing for YoungPiongMidi's
host-side Python tools (tools/acid_synth_monitor.py, tools/groovebox.py).

The board has no wire MIDI transport yet (BLE/UART are Milestones 8-9 -
see docs/midi.md), so every host tool that reacts to MIDI events reads
the same thing: the plain-text NOTE_ON/NOTE_OFF/CC lines midi_task logs
over the serial console (components/midi/midi.c's log_event()). This
module exists so that parsing format and port autodetection live in
exactly one place instead of being copy-pasted between tools.
"""
import re

LOG_NOTE_ON = re.compile(r"NOTE_ON\s+ch=(\d+)\s+note=(\d+)\s+vel=(\d+)")
LOG_NOTE_OFF = re.compile(r"NOTE_OFF\s+ch=(\d+)\s+note=(\d+)\s+vel=(\d+)")
LOG_CC = re.compile(r"CC\s+ch=(\d+)\s+cc=(\d+)\s+val=(\d+)")

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

# Espressif's registered USB vendor ID - this board's native USB-Serial/
# JTAG interface enumerates under it (confirmed directly via pyserial's
# own port listing on the actual connected board: VID 0x303A, PID
# 0x1001, description "USB JTAG/serial debug unit", manufacturer
# "Espressif" - not guessed). Used to actually *recognize* the ESP32
# among whatever other serial devices happen to be attached, rather
# than assuming "the first port found" or matching a macOS-specific
# device name pattern.
ESPRESSIF_USB_VID = 0x303A


def note_label(note: int) -> str:
    return f"{NOTE_NAMES[note % 12]}{note // 12 - 1}"


def midi_note_to_freq(note: int) -> float:
    return 440.0 * (2.0 ** ((note - 69) / 12.0))


def list_serial_ports():
    """Returns every detected serial port as (device, label, is_esp32)
    tuples, using pyserial's own cross-platform port enumeration (not a
    glob pattern - works regardless of OS/naming scheme, and surfaces a
    real description/manufacturer string instead of a bare device path).
    `is_esp32` is True when the port's USB VID matches
    ESPRESSIF_USB_VID - this is what "recognizing the ESP32" actually
    means here, not just guessing from the device name."""
    import serial.tools.list_ports
    ports = []
    for p in serial.tools.list_ports.comports():
        is_esp32 = p.vid == ESPRESSIF_USB_VID
        desc = p.description if p.description and p.description != "n/a" else None
        label = f"{p.device} ({desc})" if desc else p.device
        ports.append((p.device, label, is_esp32))
    return ports


def find_default_port():
    """Auto-detects the board's serial port: prefers a port whose USB
    VID matches Espressif's (an actual hardware identification, not
    just "the first port found" or a device-name guess), falling back
    to the first serial port at all if no Espressif-VID match exists
    (e.g. a different board). Returns None if nothing is connected -
    callers decide how to handle that (exit with an error, or fall back
    to a self-test/Demo mode)."""
    ports = list_serial_ports()
    if not ports:
        return None
    for device, _label, is_esp32 in ports:
        if is_esp32:
            return device
    return ports[0][0]


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
