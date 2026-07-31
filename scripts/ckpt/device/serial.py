"""Serial (UART) communication with MSP430 devices.

Absorbs scripts/lib/read_serial.py into the ckpt package as a library module.
Provides device auto-detection and line-oriented reading with end-marker
detection.
"""

from __future__ import annotations

import glob
import re
import subprocess
import time

import serial as pyserial

from ..errors import DeviceError


def find_device() -> str | None:
    """Auto-detect MSP430 UART serial device.

    Searches ``/dev/tty.usbmodem*`` (macOS) then ``/dev/ttyACM*`` (Linux).
    When multiple matches exist, returns the one with the largest trailing
    number (typically the UART port; the smaller number is the debug port).

    Returns ``None`` if no matching device is found.
    """
    for pattern in ["/dev/tty.usbmodem*", "/dev/ttyACM*"]:
        matches = glob.glob(pattern)
        if matches:

            def _trailing_number(path: str) -> int:
                m = re.search(r"(\d+)$", path)
                return int(m.group(1)) if m else 0

            matches.sort(key=_trailing_number, reverse=True)
            return matches[0]
    return None


def read_serial_output(
    *,
    device: str | None = None,
    baud: int = 9600,
    timeout: float = 30,
    end_marker: str = "END_OUTPUT",
    reset_cmd: str | None = None,
) -> tuple[list[str], bool]:
    """Read UART output until end marker or timeout.

    The serial port is opened first, and then *reset_cmd* (if given) is
    launched as a background shell process.  This ordering avoids the race
    condition where the device starts transmitting before the host port is
    ready.

    Returns ``(lines, found_marker)`` where *lines* is a list of decoded
    strings and *found_marker* indicates whether *end_marker* was seen.

    Raises DeviceError on port open failure or if no device is found.
    """
    if device is None:
        device = find_device()
    if device is None:
        raise DeviceError("No serial device found")

    try:
        ser = pyserial.Serial(device, baudrate=baud)
    except pyserial.SerialException as exc:
        raise DeviceError(f"Cannot open {device}: {exc}") from exc

    # Run reset command after port is open (fixes race condition)
    reset_proc = None
    if reset_cmd is not None:
        reset_proc = subprocess.Popen(
            reset_cmd,
            shell=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    try:
        lines, found = _read_until_marker(ser, end_marker, timeout)
    finally:
        ser.close()
        if reset_proc is not None:
            reset_proc.terminate()
            reset_proc.wait()

    return lines, found


def _read_until_marker(
    ser: pyserial.Serial,
    end_marker: str,
    timeout: float,
) -> tuple[list[str], bool]:
    """Read lines from an open serial port until *end_marker* or *timeout*.

    Returns ``(lines, found_marker)``.
    """
    lines: list[str] = []
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        ser.timeout = min(remaining, 1.0)
        try:
            raw = ser.readline()
        except pyserial.SerialException:
            break
        if not raw:
            continue
        line = raw.decode("ascii", errors="replace").rstrip("\r\n")
        lines.append(line)
        if end_marker in line:
            return lines, True

    return lines, False
