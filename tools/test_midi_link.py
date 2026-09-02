#!/usr/bin/env python3
"""
test_midi_link.py - headless tests for midi_link.py's serial port
detection (list_serial_ports/find_default_port). Run with:
python3 tools/test_midi_link.py

Mocks serial.tools.list_ports.comports() rather than touching real
hardware, so this suite passes regardless of what's actually plugged in
- the real board's own VID/PID/description used below were confirmed
directly (pyserial's own enumeration on the real connected board), not
guessed; see midi_link.py's ESPRESSIF_USB_VID comment.
"""
import sys
from unittest.mock import patch

import midi_link

failures = []


def check(name, condition, detail=""):
    status = "PASS" if condition else "FAIL"
    print(f"[{status}] {name}" + (f" - {detail}" if detail and not condition else ""))
    if not condition:
        failures.append(name)


class _FakePort:
    def __init__(self, device, vid=None, pid=None, description="n/a"):
        self.device = device
        self.vid = vid
        self.pid = pid
        self.description = description


def _mock_comports(ports):
    return patch("serial.tools.list_ports.comports", return_value=ports)


def test_list_serial_ports_empty():
    with _mock_comports([]):
        check("no ports connected -> empty list", midi_link.list_serial_ports() == [])


def test_list_serial_ports_identifies_esp32_by_vid():
    ports = [
        _FakePort("/dev/cu.debug-console", description="n/a"),
        _FakePort("/dev/cu.usbmodem1101", vid=midi_link.ESPRESSIF_USB_VID, pid=0x1001,
                  description="USB JTAG/serial debug unit"),
        _FakePort("/dev/cu.Bluetooth-Incoming-Port", description="n/a"),
    ]
    with _mock_comports(ports):
        result = midi_link.list_serial_ports()
    check("returns one entry per port", len(result) == 3, f"got {result}")
    devices = [r[0] for r in result]
    check("device paths preserved in order", devices == [
        "/dev/cu.debug-console", "/dev/cu.usbmodem1101", "/dev/cu.Bluetooth-Incoming-Port"])
    esp_entry = result[1]
    check("the Espressif-VID port is flagged is_esp32=True", esp_entry[2] is True, f"got {esp_entry}")
    check("its label includes the real description", "USB JTAG/serial debug unit" in esp_entry[1],
          f"got {esp_entry[1]}")
    check("a port with no VID match is flagged is_esp32=False", result[0][2] is False and result[2][2] is False)
    check("a port with description 'n/a' falls back to a bare device-path label",
          result[0][1] == "/dev/cu.debug-console", f"got {result[0][1]}")


def test_find_default_port_prefers_esp32_vid_over_order():
    # ESP32 port listed SECOND - must still be picked over the first one.
    ports = [
        _FakePort("/dev/cu.debug-console", description="n/a"),
        _FakePort("/dev/cu.usbmodem1101", vid=midi_link.ESPRESSIF_USB_VID, pid=0x1001,
                  description="USB JTAG/serial debug unit"),
    ]
    with _mock_comports(ports):
        check("find_default_port() picks the Espressif-VID port even though it's not first",
              midi_link.find_default_port() == "/dev/cu.usbmodem1101")


def test_find_default_port_falls_back_to_first_port_if_no_esp32():
    ports = [
        _FakePort("/dev/cu.debug-console", description="n/a"),
        _FakePort("/dev/cu.Bluetooth-Incoming-Port", description="n/a"),
    ]
    with _mock_comports(ports):
        check("with no Espressif-VID port at all, falls back to the first port found",
              midi_link.find_default_port() == "/dev/cu.debug-console")


def test_find_default_port_none_when_nothing_connected():
    with _mock_comports([]):
        check("returns None (not an error) when no serial ports exist at all",
              midi_link.find_default_port() is None)


def main():
    test_list_serial_ports_empty()
    test_list_serial_ports_identifies_esp32_by_vid()
    test_find_default_port_prefers_esp32_vid_over_order()
    test_find_default_port_falls_back_to_first_port_if_no_esp32()
    test_find_default_port_none_when_nothing_connected()

    print()
    if failures:
        print(f"{len(failures)} check(s) FAILED:")
        for f in failures:
            print(f"  - {f}")
        sys.exit(1)
    print("ALL CHECKS PASSED")


if __name__ == "__main__":
    main()
