"""Saleae Logic 2 automation for execution timing measurement.

Captures GPIO waveform (P3.4 -> digital channel 0) during benchmark
execution and computes the time from the start-pulse falling edge to
the stop-pulse rising edge.

Requires the Logic 2 desktop app with automation server enabled
(localhost:10430) and the ``logic2-automation`` package::

    uv sync --extra saleae
"""

from __future__ import annotations

import logging
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
_MAX_CAPTURE_ATTEMPTS = 3
_AMBIGUOUS_CAPTURE_PREFIX = "Ambiguous Saleae capture"

logger = logging.getLogger(__name__)


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
        2. Flash the ELF via mspdebug
        3. Programming session exits and the target free-runs
        4. Start pulse (~10 us) is captured but does not trigger
        5. Program runs (GPIO LOW — BOR resets are invisible)
        6. Stop pulse (~5 ms) fires the trigger
        7. Record *after_trigger_seconds* more, then capture ends
        8. Export raw digital data, measure start falling -> stop rising
        9. Retry if the waveform is ambiguous, else return delta in us

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

    last_ambiguous_error: DeviceError | None = None
    for attempt in range(1, _MAX_CAPTURE_ATTEMPTS + 1):
        capture = manager.start_capture(
            device_configuration=device_config,
            capture_configuration=capture_config,
        )

        try:
            flash(elf_path, flash_timeout)
            capture.wait()
        except Exception:
            _stop_capture_quietly(capture)
            raise

        with tempfile.TemporaryDirectory() as tmpdir:
            csv_path = Path(tmpdir) / "digital.csv"
            capture.export_raw_data_csv(
                directory=tmpdir,
                digital_channels=[_SALEAE_CHANNEL],
            )
            try:
                return _extract_timing(csv_path)
            except DeviceError as exc:
                if not _is_ambiguous_capture(exc):
                    raise
                last_ambiguous_error = exc
                if attempt == _MAX_CAPTURE_ATTEMPTS:
                    break
                logger.warning(
                    "Ambiguous Saleae capture for %s on attempt %d/%d; retrying.",
                    elf_path,
                    attempt,
                    _MAX_CAPTURE_ATTEMPTS,
                )

    assert last_ambiguous_error is not None
    raise DeviceError(
        f"{last_ambiguous_error} after {_MAX_CAPTURE_ATTEMPTS} attempts"
    )


_SHORT_PULSE_THRESHOLD = 0.001  # 1 ms — start pulse is ~10 us, stop pulse is ~5 ms


def _stop_capture_quietly(capture: object) -> None:
    """Best-effort stop for captures that failed before normal completion."""
    try:
        capture.stop()
    except Exception:
        pass


def _is_ambiguous_capture(exc: DeviceError) -> bool:
    """Return True when *exc* indicates an ambiguous multi-start waveform."""
    return str(exc).startswith(_AMBIGUOUS_CAPTURE_PREFIX)


def _extract_timing(csv_path: Path) -> float:
    """Parse Saleae digital CSV export to find execution time.

    The CSV has columns like ``Time [s], Channel 0``.
    GPIO uses pulse-based signalling:
    - Start pulse: ~10 us HIGH (short, below 1 ms threshold)
    - Stop pulse:  ~5 ms HIGH (long, above 1 ms threshold)

    Execution time is measured as:
        the short pulse falling edge immediately before the first
        long pulse rising edge.

    Multiple short pulses before the stop pulse are treated as ambiguous:
    they indicate the target likely restarted during capture, so the
    extractor refuses to guess which partial run is "correct".

    Raises DeviceError if edges are missing.
    """
    # Collect all pulses as (rising_time, falling_time) pairs.
    pulses: list[tuple[float, float]] = []
    prev_val: int | None = None
    rising_time: float | None = None

    with open(csv_path) as f:
        f.readline()  # skip header
        for line in f:
            parts = line.strip().split(",")
            if len(parts) < 2:
                continue
            timestamp = float(parts[0])
            value = int(parts[1])

            if prev_val is not None:
                if prev_val == 0 and value == 1:
                    rising_time = timestamp
                elif prev_val == 1 and value == 0 and rising_time is not None:
                    pulses.append((rising_time, timestamp))
                    rising_time = None

            prev_val = value

    if not pulses:
        raise DeviceError(
            "No pulses detected on Saleae channel "
            f"{_SALEAE_CHANNEL}. GPIO never pulsed -- "
            "check that the benchmark was compiled with benchmark.h"
        )

    # Classify pulses by duration.
    short_pulses = [(r, f) for r, f in pulses if (f - r) < _SHORT_PULSE_THRESHOLD]
    long_pulses = [(r, f) for r, f in pulses if (f - r) >= _SHORT_PULSE_THRESHOLD]

    if not short_pulses:
        raise DeviceError(
            "No start pulse (short HIGH < 1 ms) detected on Saleae channel "
            f"{_SALEAE_CHANNEL}. Check benchmark GPIO signalling."
        )
    if not long_pulses:
        raise DeviceError(
            "No stop pulse (long HIGH >= 1 ms) detected on Saleae channel "
            f"{_SALEAE_CHANNEL}. Program may have hung or "
            "capture timeout was too short."
        )

    # The first long pulse is the stop pulse that triggered the capture.
    stop_time = long_pulses[0][0]

    # Exactly one short pulse must precede the stop pulse. More than one means
    # the target likely restarted during capture, and choosing one would
    # silently under/over-report execution time.
    start_pulses = [(r, f) for r, f in short_pulses if f < stop_time]
    if not start_pulses:
        raise DeviceError(
            "No start pulse found before the stop pulse on Saleae channel "
            f"{_SALEAE_CHANNEL}. Check benchmark GPIO signalling."
        )
    if len(start_pulses) > 1:
        raise DeviceError(
            "Ambiguous Saleae capture on channel "
            f"{_SALEAE_CHANNEL}: detected {len(start_pulses)} start pulses "
            "before the stop pulse. The target likely restarted during "
            "capture; refusing to guess execution time."
        )

    start_time = start_pulses[0][1]

    return (stop_time - start_time) * 1_000_000  # seconds -> us
