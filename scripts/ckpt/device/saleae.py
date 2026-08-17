"""Saleae Logic 2 automation for execution timing measurement.

Captures a GPIO pulse waveform during benchmark execution and computes
the time from the start-pulse falling edge to the stop-pulse rising edge.

Hardware wiring and Logic 2 app setup are documented in ``docs/saleae.md``.
"""

from __future__ import annotations

import logging
import os
import tempfile
import threading
import time
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path
from typing import TYPE_CHECKING

# Suppress noisy gRPC fork warnings from logic2-automation.
# Must be set before grpcio is imported anywhere in the process.
if "GRPC_ENABLE_FORK_SUPPORT" not in os.environ:
    os.environ["GRPC_ENABLE_FORK_SUPPORT"] = "0"

from ..errors import DeviceError
from .flash import HeldFlashSession, flash_and_hold
from .otii import OtiiSession, connect_debugger, isolate_target, power_target

if TYPE_CHECKING:
    from saleae.automation import Capture, Manager

_SALEAE_CHANNEL = 0
_SALEAE_SAMPLE_RATE = 100_000_000  # 100 MHz
_MAX_CAPTURE_ATTEMPTS = 3
_CAPTURE_START_RETRY_DELAY_SECONDS = 0.5
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


def _start_target(
    session: HeldFlashSession, otii: OtiiSession | None, flash_timeout: int
) -> None:
    """Start the flashed program: release it under the ez-FET, or cut the
    ez-FET off and power the target from the Otii."""
    if otii is None:
        session.release(flash_timeout)
        return
    try:
        isolate_target(otii)
    finally:
        session.abort()
    power_target(otii)


def saleae_run(
    elf_path: Path,
    manager: Manager,
    flash_timeout: int,
    after_trigger_seconds: float,
    capture_timeout_seconds: float,
    otii: OtiiSession | None,
) -> float:
    """Flash ELF, capture GPIO waveform, return execution_time_us.

    Uses digital channel ``_SALEAE_CHANNEL`` at ``_SALEAE_SAMPLE_RATE`` Hz.
    GPIO uses pulse-based signalling (BOR-safe):
    - Start pulse: ~10 us positive pulse (below trigger threshold)
    - Stop pulse:  ~5 ms positive pulse (above trigger threshold)

    Capture triggers on ``PULSE_HIGH`` with ``min_pulse_width = 1 ms``,
    so only the stop pulse fires the trigger.

    Flow:
        1. Flash the ELF via mspdebug, keeping the session open — the
           target stays halted, so no stray pulses can occur while the
           capture is armed
        2. Start Saleae capture (trigger: PULSE_HIGH, min 1 ms)
        3. Start the target inside the armed capture window: release the
           mspdebug session so it resets and free-runs exactly once, or —
           with an Otii in the loop — isolate it from the ez-FET and power
           it from the Otii main output
        4. Start pulse (~10 us) is captured but does not trigger; a cold
           power-up first emits a sub-us glitch, which is filtered out
        5. Program runs (GPIO LOW — BOR resets are invisible)
        6. Stop pulse (~5 ms) fires the trigger
        7. Record *after_trigger_seconds* more, then capture ends
        8. Export raw digital data, measure start falling -> stop rising
        9. Retry if the waveform is ambiguous, else return delta in us

    Raises DeviceError if no edges are detected (GPIO never pulsed)
    or the stop pulse does not arrive within *capture_timeout_seconds*.
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
    last_start_error: Exception | None = None
    for attempt in range(1, _MAX_CAPTURE_ATTEMPTS + 1):
        # Flash first, holding the target halted, so the capture window
        # armed below cannot see pulses from programming-induced resets.
        session = flash_and_hold(elf_path, flash_timeout)

        try:
            capture_context = manager.start_capture(
                device_configuration=device_config,
                capture_configuration=capture_config,
            )
        # Deliberately broad: any start failure is retried, then wrapped in
        # DeviceError below.
        except Exception as exc:  # noqa: BLE001
            session.abort()
            last_start_error = exc
            if attempt == _MAX_CAPTURE_ATTEMPTS:
                break
            logger.warning(
                "Saleae start_capture failed for %s on attempt %d/%d; retrying in %.1fs. Error: %s",
                elf_path,
                attempt,
                _MAX_CAPTURE_ATTEMPTS,
                _CAPTURE_START_RETRY_DELAY_SECONDS,
                exc,
            )
            time.sleep(_CAPTURE_START_RETRY_DELAY_SECONDS)
            continue

        try:
            with capture_context as capture:
                try:
                    _start_target(session, otii, flash_timeout)
                    _wait_for_capture(capture, capture_timeout_seconds)
                except Exception:
                    _stop_capture_quietly(capture)
                    raise
                finally:
                    if otii is not None:
                        try:
                            connect_debugger(otii)
                        except DeviceError:
                            logger.exception(
                                "Failed to reconnect debugger after capture"
                            )

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
        except DeviceError:
            # Our own errors (no pulses, capture wait timeout, mspdebug
            # failures) keep their existing semantics.
            raise
        # Deliberately broad: Logic 2 runtime hiccups (e.g. "Error
        # interacting with device during capture: ReadTimeout" raised from
        # the capture context's close()) are transient — retry the attempt.
        except Exception as exc:  # noqa: BLE001
            last_start_error = exc
            if attempt == _MAX_CAPTURE_ATTEMPTS:
                break
            logger.warning(
                "Saleae capture attempt %d/%d failed for %s; retrying in %.1fs. Error: %s",
                attempt,
                _MAX_CAPTURE_ATTEMPTS,
                elf_path,
                _CAPTURE_START_RETRY_DELAY_SECONDS,
                exc,
            )
            time.sleep(_CAPTURE_START_RETRY_DELAY_SECONDS)

    if last_ambiguous_error is not None:
        raise DeviceError(
            f"{last_ambiguous_error} after {_MAX_CAPTURE_ATTEMPTS} attempts"
        )

    assert last_start_error is not None
    raise DeviceError(
        "Saleae capture did not complete after "
        f"{_MAX_CAPTURE_ATTEMPTS} attempts. "
        "Is a physical Logic device connected and visible to Logic 2? "
        f"Error: {last_start_error}"
    ) from last_start_error


_SHORT_PULSE_THRESHOLD = 0.001  # 1 ms — start pulse is ~10 us, stop pulse is ~5 ms

# Narrower than this is a cold power-up glitch, not a start pulse: while VCC
# ramps, P3.4 is high-impedance — the code cannot clear LOCKLPM5 and drive the
# pin low until the CPU runs, milliseconds later — so the line follows the rail
# across the analyzer threshold for ~0.4 us. Every Otii-powered run sees it.
_GLITCH_PULSE_THRESHOLD = 0.000002  # 2 us

# Deadline of a single completion poll: bounds how long the watcher thread
# keeps running after it was told to stop.
_WATCH_POLL_SECONDS = 0.5


def _wait_for_capture(capture: Capture, timeout_seconds: float) -> None:
    """Wait for capture completion with a client-side gRPC deadline."""
    import grpc
    from saleae.grpc import saleae_pb2

    request = saleae_pb2.WaitCaptureRequest(capture_id=capture.capture_id)  # pyright: ignore[reportAttributeAccessIssue]
    try:
        capture.manager.stub.WaitCapture(request, timeout=timeout_seconds)
    except grpc.RpcError as exc:
        if exc.code() == grpc.StatusCode.DEADLINE_EXCEEDED:
            raise DeviceError(
                "Saleae capture timed out after "
                f"{timeout_seconds:g} seconds waiting for the end signal"
            ) from exc
        raise


def _stop_capture_quietly(capture: Capture) -> None:
    """Best-effort stop for captures that failed before normal completion."""
    try:
        capture.stop()
    # Deliberately broad: the capture already failed and this cleanup must
    # not mask the original error.
    except Exception as exc:  # noqa: BLE001
        logger.debug("Ignoring error while stopping failed capture: %s", exc)


def _is_ambiguous_capture(exc: DeviceError) -> bool:
    """Return True when *exc* indicates an ambiguous multi-start waveform."""
    return str(exc).startswith(_AMBIGUOUS_CAPTURE_PREFIX)


def _collect_pulses(csv_path: Path) -> list[tuple[float, float]]:
    """Parse a Saleae digital CSV export into (rising_time, falling_time) pairs.

    The CSV has columns like ``Time [s], Channel 0``.
    """
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

    return pulses


def _start_pulses(pulses: list[tuple[float, float]]) -> list[tuple[float, float]]:
    """Short pulses wide enough to be a start pulse, not a power-up glitch."""
    return [
        (r, f)
        for r, f in pulses
        if _GLITCH_PULSE_THRESHOLD <= (f - r) < _SHORT_PULSE_THRESHOLD
    ]


def _stop_pulses(pulses: list[tuple[float, float]]) -> list[tuple[float, float]]:
    """Long pulses — the ones that fire the capture trigger."""
    return [(r, f) for r, f in pulses if (f - r) >= _SHORT_PULSE_THRESHOLD]


def _extract_timing(csv_path: Path) -> float:
    """Parse Saleae digital CSV export to find execution time.

    GPIO uses pulse-based signalling:
    - Start pulse: ~10 us HIGH (short, below 1 ms threshold)
    - Stop pulse:  ~5 ms HIGH (long, above 1 ms threshold)

    Execution time is measured as:
        the short pulse falling edge immediately before the first
        long pulse rising edge.

    Pulses below _GLITCH_PULSE_THRESHOLD are power-up glitches and do not
    count as start pulses. Multiple start pulses before the stop pulse are
    treated as ambiguous: they indicate the target likely restarted during
    capture, so the extractor refuses to guess which partial run is "correct".

    Raises DeviceError if edges are missing.
    """
    pulses = _collect_pulses(csv_path)

    if not pulses:
        raise DeviceError(
            "No pulses detected on Saleae channel "
            f"{_SALEAE_CHANNEL}. GPIO never pulsed -- "
            "check that the benchmark was compiled with benchmark.h"
        )

    # Classify pulses by duration.
    short_pulses = _start_pulses(pulses)
    long_pulses = _stop_pulses(pulses)

    if not short_pulses:
        raise DeviceError(
            "No start pulse (short HIGH between 2 us and 1 ms) detected on "
            f"Saleae channel {_SALEAE_CHANNEL}. Check benchmark GPIO signalling."
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


# ---------------------------------------------------------------------------
# Intermittent-power replay captures (used by ckpt intermittent)
# ---------------------------------------------------------------------------


def start_pulse_capture(manager: Manager, after_trigger_seconds: float):
    """Arm a capture that triggers on the >=1 ms stop pulse on channel 0.

    Returns the capture context manager from ``manager.start_capture``;
    the caller enters it and later calls :func:`finish_pulse_capture`.
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
            min_pulse_width_seconds=0.001,  # 1 ms — ignores 10 us start pulses
            max_pulse_width_seconds=2,
            after_trigger_seconds=after_trigger_seconds,
        ),
    )
    return manager.start_capture(
        device_configuration=device_config,
        capture_configuration=capture_config,
    )


def _capture_ended(capture, timeout_seconds: float) -> bool:
    """Return whether the capture finished within *timeout_seconds*."""
    try:
        _wait_for_capture(capture, timeout_seconds)
    except DeviceError:
        return False
    return True


@contextmanager
def capture_completion_watcher(capture) -> Iterator[threading.Event]:
    """Set an event as soon as the armed capture completes.

    Lets the caller stop replaying a power trace the moment the benchmark
    signalled its stop pulse, instead of playing the remaining minutes of
    trace into an already-finished program. The returned event stays clear
    if the capture never triggers.
    """
    completed = threading.Event()
    stop_watching = threading.Event()

    def watch() -> None:
        while not stop_watching.is_set():
            if _capture_ended(capture, _WATCH_POLL_SECONDS):
                completed.set()
                return

    thread = threading.Thread(target=watch, name="saleae-capture-watch", daemon=True)
    thread.start()
    try:
        yield completed
    finally:
        stop_watching.set()
        thread.join()


def finish_pulse_capture(capture, wait_seconds: float) -> tuple[bool, float | None]:
    """Resolve an armed pulse capture after the trace replay ended.

    By this point the target is unpowered, so the stop pulse either already
    fired the trigger or never will — *wait_seconds* only needs to cover the
    capture's after-trigger tail.

    Returns ``(completed, execution_time_us)``:
    - ``(False, None)`` — the trigger never fired (run incomplete); the
      capture is stopped.
    - ``(True, None)`` — the trigger fired but no timing could be extracted.
    - ``(True, us)`` — execution time from the first start-pulse falling
      edge to the stop-pulse rising edge. Unlike :func:`saleae_run`, several
      start pulses are expected: a run that dies before its first checkpoint
      boots fresh and pulses again, and the wall-clock time (outages
      included) starts at the first attempt.
    """
    try:
        _wait_for_capture(capture, wait_seconds)
    except DeviceError:
        _stop_capture_quietly(capture)
        return False, None

    with tempfile.TemporaryDirectory() as tmpdir:
        csv_path = Path(tmpdir) / "digital.csv"
        capture.export_raw_data_csv(
            directory=tmpdir,
            digital_channels=[_SALEAE_CHANNEL],
        )
        pulses = _collect_pulses(csv_path)

    return True, _replay_timing_from_pulses(pulses)


def _replay_timing_from_pulses(pulses: list[tuple[float, float]]) -> float | None:
    """First start-pulse falling edge -> first stop-pulse rising edge, in us."""
    long_pulses = _stop_pulses(pulses)
    if not long_pulses:
        logger.warning("Capture triggered but no stop pulse found in the export")
        return None
    stop_time = long_pulses[0][0]

    start_falls = [f for _, f in _start_pulses(pulses) if f < stop_time]
    if not start_falls:
        logger.warning("Stop pulse captured but no start pulse precedes it")
        return None

    return (stop_time - start_falls[0]) * 1_000_000  # seconds -> us
