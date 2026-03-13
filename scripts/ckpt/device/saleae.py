"""Saleae Logic 2 automation for execution timing measurement.

Captures GPIO waveform (P3.4 -> digital channel 0) during benchmark
execution and computes the time between first rising edge and last
falling edge.

Requires the Logic 2 desktop app with automation server enabled
(localhost:10430) and the ``logic2-automation`` package::

    uv sync --extra saleae
"""

from __future__ import annotations

import tempfile
from pathlib import Path

from ..runner import DeviceError
from ..toolchain import Toolchain
from .flash import flash

_SALEAE_CHANNEL = 0
_SALEAE_SAMPLE_RATE = 10_000_000  # 10 MHz


def discover_saleae():  # -> Manager (type omitted to defer import)
    """Connect to Logic 2 automation server (localhost:10430).

    Returns a ``saleae.automation.Manager`` instance.

    Raises DeviceError if:
    - ``logic2-automation`` package is not installed
    - Logic 2 app is not running or automation is not enabled
    """
    try:
        from saleae.automation import Manager
    except ImportError:
        raise DeviceError(
            "logic2-automation package not installed. "
            "Install with: uv sync --extra saleae"
        )

    try:
        return Manager.connect()
    except Exception as exc:
        raise DeviceError(
            f"Cannot connect to Saleae Logic 2 automation server "
            f"(localhost:10430). Is Logic 2 running with automation enabled? "
            f"Error: {exc}"
        ) from exc


def saleae_run(
    tc: Toolchain,
    elf_path: Path,
    manager: object,
    flash_timeout: int,
    capture_timeout: int,
) -> float:
    """Flash ELF, capture GPIO waveform, return execution_time_us.

    Uses digital channel ``_SALEAE_CHANNEL`` at ``_SALEAE_SAMPLE_RATE`` Hz.

    Flow:
        1. Start Saleae capture
        2. Flash the ELF via mspdebug (device resets and starts running)
        3. Wait up to *capture_timeout* seconds for the falling edge
        4. Stop capture
        5. Export raw digital data, find first rising->last falling edge
        6. Return delta in microseconds

    Raises DeviceError if no rising edge is detected (GPIO never went
    high) or no falling edge within *capture_timeout* (program hung).
    """
    from saleae.automation import (
        CaptureConfiguration,
        LogicDeviceConfiguration,
        TimedCaptureMode,
    )

    device_config = LogicDeviceConfiguration(
        enabled_digital_channels=[_SALEAE_CHANNEL],
        digital_sample_rate=_SALEAE_SAMPLE_RATE,
    )
    capture_config = CaptureConfiguration(
        capture_mode=TimedCaptureMode(capture_timeout),
    )

    capture = manager.start_capture(
        device_configuration=device_config,
        capture_configuration=capture_config,
    )

    try:
        flash(tc, elf_path, flash_timeout)
        capture.wait()
    except Exception:
        capture.stop()
        raise

    with tempfile.TemporaryDirectory() as tmpdir:
        csv_path = Path(tmpdir) / "digital.csv"
        capture.export_raw_data_csv(
            directory=tmpdir,
            digital_channels=[_SALEAE_CHANNEL],
        )
        return _extract_timing(csv_path)


def _extract_timing(csv_path: Path) -> float:
    """Parse Saleae digital CSV export to find execution time.

    The CSV has columns like ``Time [s], Channel 0``.
    Finds the first rising edge and last falling edge on the channel.
    Returns the delta in microseconds.

    Raises DeviceError if edges are missing.
    """
    first_rising: float | None = None
    last_falling: float | None = None
    prev_val: int | None = None

    with open(csv_path) as f:
        header = f.readline()  # skip header
        for line in f:
            parts = line.strip().split(",")
            if len(parts) < 2:
                continue
            timestamp = float(parts[0])
            value = int(parts[1])

            if prev_val is not None:
                if prev_val == 0 and value == 1 and first_rising is None:
                    first_rising = timestamp
                elif prev_val == 1 and value == 0:
                    last_falling = timestamp

            prev_val = value

    if first_rising is None:
        raise DeviceError(
            "No rising edge detected on Saleae channel "
            f"{_SALEAE_CHANNEL}. GPIO never went high -- "
            "check that the benchmark was compiled with benchmark.h"
        )
    if last_falling is None:
        raise DeviceError(
            "No falling edge detected on Saleae channel "
            f"{_SALEAE_CHANNEL}. Program may have hung or "
            "capture timeout was too short."
        )

    return (last_falling - first_rising) * 1_000_000  # seconds -> us
