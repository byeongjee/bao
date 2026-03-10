#!/usr/bin/env python3
"""Read UART output from MSP430 via serial port.

Opens the serial port first, then optionally runs a reset command (fixing
the race condition where the device starts sending before the port is open).
Exits early when the end marker is detected, or after a wall-clock timeout.

Exit codes:
    0 - success (end marker found)
    1 - error (no device, port failure)
    2 - timeout (partial output printed)
"""

import argparse
import glob
import re
import subprocess
import sys
import time

import serial


def find_device():
    """Auto-detect the MSP430 UART serial device.

    Searches /dev/tty.usbmodem* (macOS) then /dev/ttyACM* (Linux).
    When multiple matches exist, returns the one with the largest trailing
    number (typically the UART port; the smaller number is the debug port).
    """
    for pattern in ["/dev/tty.usbmodem*", "/dev/ttyACM*"]:
        matches = glob.glob(pattern)
        if matches:
            # Sort by trailing number descending, pick largest
            def trailing_number(path):
                m = re.search(r"(\d+)$", path)
                return int(m.group(1)) if m else 0

            matches.sort(key=trailing_number, reverse=True)
            return matches[0]
    return None


def read_until_marker(ser, end_marker, timeout):
    """Read lines from serial port until end marker or timeout.

    Returns (lines, found_marker) where lines is a list of decoded strings
    and found_marker indicates whether the end marker was seen.
    """
    lines = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        ser.timeout = min(remaining, 1.0)
        try:
            raw = ser.readline()
        except serial.SerialException:
            break
        if not raw:
            continue
        line = raw.decode("ascii", errors="replace").rstrip("\r\n")
        lines.append(line)
        if end_marker in line:
            return lines, True
    return lines, False


def main():
    parser = argparse.ArgumentParser(
        description="Read UART output from MSP430 serial port."
    )
    parser.add_argument(
        "--device", default=None,
        help="Serial device path (default: auto-detect)"
    )
    parser.add_argument(
        "--baud", type=int, default=9600,
        help="Baud rate (default: 9600)"
    )
    parser.add_argument(
        "--timeout", type=float, default=30,
        help="Wall-clock timeout in seconds (default: 30)"
    )
    parser.add_argument(
        "--end-marker", default="[END_OUTPUT]",
        help="End-of-output marker string (default: [END_OUTPUT])"
    )
    parser.add_argument(
        "--reset-cmd", default=None,
        help="Shell command to run after port is open (e.g. mspdebug reset)"
    )
    args = parser.parse_args()

    device = args.device or find_device()
    if not device:
        print("Error: No serial device found", file=sys.stderr)
        sys.exit(1)

    try:
        ser = serial.Serial(device, baudrate=args.baud)
    except serial.SerialException as e:
        print(f"Error: Cannot open {device}: {e}", file=sys.stderr)
        sys.exit(1)

    # Run reset command after port is open (fixes race condition)
    reset_proc = None
    if args.reset_cmd:
        reset_proc = subprocess.Popen(
            args.reset_cmd, shell=True,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )

    lines, found = read_until_marker(ser, args.end_marker, args.timeout)
    ser.close()

    if reset_proc is not None:
        reset_proc.wait()

    for line in lines:
        print(line)

    if not found:
        print("Warning: Timeout waiting for end marker", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
