"""Otii Ace Pro control for intermittent-power benchmark runs.

Replays a recorded harvesting trace on the Otii main output while a Qoitech
Switchboard, driven from an expansion-port GPO, connects or isolates the
ez-FET debugger's SBW and 3V3 jumper lines. The target must be isolated
from the debugger while it runs on replayed power.

Requires the ``otii-tcp-client`` package (``uv sync --extra otii``) and the
``otii_server`` binary (path via the ``OTII_SERVER_BIN`` environment
variable).
"""

from __future__ import annotations

import logging
import os
import socket
import subprocess
import time
from collections.abc import Iterator
from contextlib import contextmanager
from dataclasses import dataclass
from typing import Any

from ..errors import DeviceError

logger = logging.getLogger(__name__)

# Switchboard relay control (GPO number on the expansion port).
_RELAY_GPO = 2
# Settle time after closing the relays before mspdebug can talk to the target.
_DEBUGGER_SETTLE_SECONDS = 3.0
# Settle time after opening the relays: VCC must decay below POR threshold.
_ISOLATION_SETTLE_SECONDS = 3.0

# The switchboard relay logic is powered from the +5V pin and driven at the
# expansion-port digital voltage.
_EXP_VOLTAGE = 5.0
_MAX_CURRENT_A = 0.01
_VOLTAGE_CLAMP_V = 3.6  # MSP430FR5994 maximum operating voltage

# A GPI1 high window at least this wide is the stop pulse (~5 ms);
# shorter windows are start pulses (~10 us). Matches the Saleae trigger
# threshold in device/saleae.py.
_STOP_PULSE_MIN_SECONDS = 0.001

_SERVER_READY_TIMEOUT_SECONDS = 10.0
_GPI_CHANNEL = "i1"


@dataclass
class OtiiSession:
    """A connected Otii device added to a project, ready for replay runs."""

    otii: Any
    project: Any
    arc: Any
    device_id: str


@dataclass
class ReplayResult:
    """Outcome of one trace replay."""

    completed: bool  # stop pulse observed on GPI1
    execution_time_us: float | None  # start-pulse fall -> stop-pulse rise
    replay_seconds: float  # wall-clock time spent replaying


def _import_otii():
    try:
        from otii_tcp_client import otii_client  # pyright: ignore[reportMissingImports]
        from otii_tcp_client.arc import Arc  # pyright: ignore[reportMissingImports]
    except ImportError as exc:
        raise DeviceError(
            "otii-tcp-client package not installed. Install with: uv sync --extra otii"
        ) from exc
    return otii_client, Arc


def _wait_for_port(host: str, port: int, timeout: float) -> bool:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def _start_server() -> subprocess.Popen:
    bin_path = os.environ.get("OTII_SERVER_BIN", "otii_server")
    host = os.environ.get("OTII_SERVER_HOST", "127.0.0.1")
    port = int(os.environ.get("OTII_SERVER_PORT", "1905"))

    logger.info("Starting otii_server: %s (%s:%d)", bin_path, host, port)
    try:
        proc = subprocess.Popen(
            [bin_path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
    except FileNotFoundError as exc:
        raise DeviceError(
            f"otii_server binary not found: {bin_path!r} "
            "(set OTII_SERVER_BIN to its path)"
        ) from exc

    if not _wait_for_port(host, port, _SERVER_READY_TIMEOUT_SECONDS):
        proc.terminate()
        raise DeviceError(
            f"otii_server did not become ready within "
            f"{_SERVER_READY_TIMEOUT_SECONDS:.0f}s on {host}:{port}"
        )
    return proc


def _stop_server(proc: subprocess.Popen) -> None:
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        logger.warning("Force killing otii_server")
        proc.kill()


def _single_arc(otii: Any, arc_cls: Any) -> tuple[Any, str]:
    """Resolve the single connected Otii device to (Arc, device_id)."""
    devices = otii.get_devices()
    if not devices:
        raise DeviceError("No Otii devices found")
    first = devices[0]
    if isinstance(first, arc_cls):
        arc = first
        device_id = getattr(arc, "device_id", getattr(arc, "id", None))
        if not device_id:
            name = getattr(arc, "name", None)
            device_id = otii.get_device_id(name) if name else None
    else:
        name = first.get("name")
        device_id = otii.get_device_id(name)
        arc = arc_cls(
            {"device_id": device_id, "name": name, "type": first.get("type", "Arc")},
            otii.connection,
        )
    if not device_id:
        raise DeviceError("Could not resolve device_id for the connected Otii device")
    return arc, device_id


@contextmanager
def otii_session() -> Iterator[OtiiSession]:
    """Start otii_server, connect, and configure the device for replay runs.

    On exit: main power off, switchboard relays opened, server stopped.
    """
    otii_client, arc_cls = _import_otii()
    server = _start_server()
    otii = None
    arc = None
    try:
        otii = otii_client.OtiiClient().connect()
        logger.info("Connected to Otii server")

        project = otii.create_project()
        arc, device_id = _single_arc(otii, arc_cls)
        arc.add_to_project()

        arc.enable_5v(True)  # powers the switchboard relays
        arc.enable_exp_port(True)
        arc.set_exp_voltage(_EXP_VOLTAGE)
        for channel in ("mc", "mp", _GPI_CHANNEL):
            arc.enable_channel(channel, True)
        arc.set_max_current(_MAX_CURRENT_A)
        arc.set_main(False)

        yield OtiiSession(otii=otii, project=project, arc=arc, device_id=device_id)
    finally:
        if otii is not None:
            try:
                if arc is not None:
                    arc.set_main(False)
                    arc.set_gpo(_RELAY_GPO, False)
                otii.shutdown()
            # Deliberately broad: cleanup must not mask the original error.
            except Exception:
                logger.exception("Error shutting down Otii client")
        _stop_server(server)


def connect_debugger(session: OtiiSession) -> None:
    """Close the switchboard relays: SBW + 3V3 connect the ez-FET to the target.

    Main power is forced off first — the target must never see the Otii
    supply and the debugger's 3V3 rail at the same time.
    """
    session.arc.set_main(False)
    logger.info("Closing switchboard relays (debugger connected)")
    session.arc.set_gpo(_RELAY_GPO, True)
    time.sleep(_DEBUGGER_SETTLE_SECONDS)


def isolate_target(session: OtiiSession) -> None:
    """Open the switchboard relays, cutting SBW and 3V3 to the target."""
    logger.info("Opening switchboard relays (target isolated)")
    session.arc.set_gpo(_RELAY_GPO, False)
    time.sleep(_ISOLATION_SETTLE_SECONDS)


class _PulseDetector:
    """Classify GPI1 high windows: <1 ms start pulses vs >=1 ms stop pulse.

    The runtime emits a ~10 us start pulse in BENCH_INIT (once per fresh
    boot — a run that dies before its first checkpoint emits another) and
    a ~5 ms stop pulse in BENCH_EXIT. Execution time spans the first
    start-pulse falling edge to the stop-pulse rising edge, outages
    included.
    """

    def __init__(self) -> None:
        self._rise_ts: float | None = None
        self.start_fall_ts: float | None = None
        self.stop_rise_ts: float | None = None

    def feed(self, events: list[dict]) -> None:
        for event in events:
            if self.stop_rise_ts is not None:
                return
            ts = event["timestamp"]
            if event["value"]:
                self._rise_ts = ts
            elif self._rise_ts is not None:
                if ts - self._rise_ts >= _STOP_PULSE_MIN_SECONDS:
                    self.stop_rise_ts = self._rise_ts
                elif self.start_fall_ts is None:
                    self.start_fall_ts = ts
                self._rise_ts = None


def _poll_gpi(
    recording, device_id: str, detector: _PulseDetector, read_idx: int
) -> int:
    count = recording.get_channel_data_count(device_id, _GPI_CHANNEL)
    if count > read_idx:
        data = recording.get_channel_data(
            device_id, _GPI_CHANNEL, read_idx, count - read_idx
        )
        detector.feed(data["values"])
        read_idx = count
    return read_idx


def _clamp_voltage(voltage: float) -> float:
    if voltage < 0.0:
        return 0.0
    if voltage > _VOLTAGE_CLAMP_V:
        return _VOLTAGE_CLAMP_V
    return voltage


def replay_trace(
    session: OtiiSession,
    samples: list[tuple[float, float]],
) -> ReplayResult:
    """Replay *samples* ``(time_s, voltage_v)`` on the main output.

    Sets the main voltage at each sample's timestamp while watching GPI1
    for the benchmark's stop pulse; replay stops early once it arrives.
    Main power is switched off before returning.
    """
    if not samples:
        raise DeviceError("Power trace is empty")

    arc = session.arc
    detector = _PulseDetector()

    arc.set_main_voltage(_clamp_voltage(samples[0][1]))
    session.project.start_recording()
    recording = session.project.get_last_recording()
    read_idx = 0

    arc.set_main(True)
    t0 = time.monotonic()
    try:
        for time_s, voltage_v in samples:
            delay = t0 + time_s - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            arc.set_main_voltage(_clamp_voltage(voltage_v))
            read_idx = _poll_gpi(recording, session.device_id, detector, read_idx)
            if detector.stop_rise_ts is not None:
                break
        else:
            # Trace exhausted: one last poll for a pulse in the final interval.
            read_idx = _poll_gpi(recording, session.device_id, detector, read_idx)
    finally:
        replay_seconds = time.monotonic() - t0
        arc.set_main(False)
        session.project.stop_recording()

    stop_rise_ts = detector.stop_rise_ts
    completed = stop_rise_ts is not None
    execution_time_us: float | None = None
    if stop_rise_ts is not None:
        if detector.start_fall_ts is not None:
            execution_time_us = (stop_rise_ts - detector.start_fall_ts) * 1e6
        else:
            logger.warning("Stop pulse seen but no start pulse; no execution time")

    logger.info(
        "Replay %s after %.1fs%s",
        "completed" if completed else "ended (no stop pulse)",
        replay_seconds,
        f" (execution {execution_time_us / 1000.0:.1f}ms)"
        if execution_time_us is not None
        else "",
    )
    return ReplayResult(
        completed=completed,
        execution_time_us=execution_time_us,
        replay_seconds=replay_seconds,
    )
