"""Saleae Logic 2 automation for execution timing measurement.

Captures GPIO waveform (P3.4 -> digital channel 0) during benchmark
execution and computes the time between first rising edge and last
falling edge.

Requires the Logic 2 desktop app with automation server enabled
(localhost:10430) and the ``logic2-automation`` package::

    uv sync --extra saleae
"""

from __future__ import annotations

import os
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING

# Suppress noisy gRPC fork warnings from logic2-automation.
# Must be set before grpcio is imported anywhere in the process.
if "GRPC_ENABLE_FORK_SUPPORT" not in os.environ:
    os.environ["GRPC_ENABLE_FORK_SUPPORT"] = "0"

from ..runner import DeviceError
from .flash import flash

if TYPE_CHECKING:
    from saleae.automation import Manager

_SALEAE_CHANNEL = 0
_SALEAE_SAMPLE_RATE = 100_000_000  # 100 MHz


def discover_saleae() -> Manager:
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
    elf_path: Path,
    manager: Manager,
    flash_timeout: int,
    after_trigger_seconds: float,
) -> float:
    """Flash ELF, capture GPIO waveform, return execution_time_us.

    Uses digital channel ``_SALEAE_CHANNEL`` at ``_SALEAE_SAMPLE_RATE`` Hz.
    GPIO uses pulse-based signalling (BOR-safe):
    - Start pulse: ~10 us positive pulse (below trigger threshold)
    - Stop pulse:  ~5 ms positive pulse (above trigger threshold)

    Capture triggers on ``PULSE_HIGH`` with ``min_pulse_width = 1 ms``,
    so only the stop pulse fires the trigger.

    Flow:
        1. Start Saleae capture (trigger: PULSE_HIGH, min 1 ms)
        2. Flash the ELF via mspdebug (device resets and starts running)
        3. Start pulse (~10 us) is captured but does not trigger
        4. Program runs (GPIO LOW — BOR resets are invisible)
        5. Stop pulse (~5 ms) fires the trigger
        6. Record *after_trigger_seconds* more, then capture ends
        7. Export raw digital data, measure first falling → last rising
        8. Return delta in microseconds

    Raises DeviceError if no edges are detected (GPIO never pulsed)
    or no stop pulse (program hung — capture.wait blocks until
    trigger fires).
    """
    from saleae.automation import (
        CaptureConfiguration,
        DigitalTriggerCaptureMode,
        DigitalTriggerType,
        LogicDeviceConfiguration,
    )

    device_config = LogicDeviceConfiguration(
        enabled_digital_channels=[_SALEAE_CHANNEL],
        digital_sample_rate=_SALEAE_SAMPLE_RATE,
    )
    capture_config = CaptureConfiguration(
        capture_mode=DigitalTriggerCaptureMode(
            trigger_type=DigitalTriggerType.PULSE_HIGH,
            trigger_channel_index=_SALEAE_CHANNEL,
            min_pulse_width_seconds=0.001,  # 1 ms — ignores 10 us start pulse
            max_pulse_width_seconds=2,
            after_trigger_seconds=after_trigger_seconds,
        ),
    )

    capture = manager.start_capture(
        device_configuration=device_config,
        capture_configuration=capture_config,
    )

    try:
        flash(elf_path, flash_timeout)
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
    GPIO uses pulse-based signalling (UP→DOWN at start and stop),
    so execution time is measured as:
        first falling edge (end of start pulse) →
        last rising edge (beginning of stop pulse).

    Raises DeviceError if edges are missing.
    """
    first_falling: float | None = None
    last_rising: float | None = None
    prev_val: int | None = None

    with open(csv_path) as f:
        f.readline()  # skip header
        for line in f:
            parts = line.strip().split(",")
            if len(parts) < 2:
                continue
            timestamp = float(parts[0])
            value = int(parts[1])

            if prev_val is not None:
                if prev_val == 1 and value == 0 and first_falling is None:
                    first_falling = timestamp
                elif prev_val == 0 and value == 1:
                    last_rising = timestamp

            prev_val = value

    if first_falling is None:
        raise DeviceError(
            "No falling edge detected on Saleae channel "
            f"{_SALEAE_CHANNEL}. GPIO never pulsed -- "
            "check that the benchmark was compiled with benchmark.h"
        )
    if last_rising is None:
        raise DeviceError(
            "No rising edge after start pulse on Saleae channel "
            f"{_SALEAE_CHANNEL}. Program may have hung or "
            "capture timeout was too short."
        )

    return (last_rising - first_falling) * 1_000_000  # seconds -> us
