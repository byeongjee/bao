"""Trace logging for intermittent computing simulation."""

import csv
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Optional, TextIO

from .config import LogConfig
from .state_machine import PowerState


@dataclass
class LogSample:
    """A single log sample."""
    sim_time_s: float        # Simulation time in seconds
    wall_time_s: float       # Wall clock time in seconds
    state: PowerState        # Current power state
    voltage_V: float         # Capacitor voltage
    current_A: float         # Load current
    power_W: float           # Power consumption
    energy_cap_uJ: float     # Energy stored in capacitor
    energy_consumed_uJ: float   # Total energy consumed
    energy_harvested_uJ: float  # Total energy harvested
    event: Optional[str]     # Event (reboot, failure, etc.)


class TraceLogger:
    """CSV trace logger for simulation data."""

    CSV_HEADER = [
        "sim_time_s",
        "wall_time_s",
        "state",
        "voltage_V",
        "current_A",
        "power_W",
        "energy_cap_uJ",
        "energy_consumed_uJ",
        "energy_harvested_uJ",
        "event",
    ]

    def __init__(self, config: LogConfig) -> None:
        """Initialize the logger.

        Args:
            config: Logging configuration.
        """
        self.config = config
        self._file: Optional[TextIO] = None
        self._writer: Optional[csv.writer] = None
        self._sample_count = 0
        self._last_log_time_s = 0.0
        self._log_interval_s = config.log_interval_ms / 1000.0

    def open(self) -> None:
        """Open the trace file for writing."""
        # Create output directory if needed
        self.config.output_path.mkdir(parents=True, exist_ok=True)

        self._file = open(self.config.trace_path, "w", newline="")
        self._writer = csv.writer(self._file)
        self._writer.writerow(self.CSV_HEADER)
        self._sample_count = 0
        self._last_log_time_s = 0.0

    def close(self) -> None:
        """Close the trace file."""
        if self._file:
            self._file.close()
            self._file = None
            self._writer = None

    def __enter__(self) -> "TraceLogger":
        self.open()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()

    def should_log(self, sim_time_s: float, force: bool = False) -> bool:
        """Check if a sample should be logged based on interval.

        Args:
            sim_time_s: Current simulation time.
            force: If True, always log.

        Returns:
            True if sample should be logged.
        """
        if force:
            return True
        if self._log_interval_s <= 0:
            return True  # Log every sample
        return (sim_time_s - self._last_log_time_s) >= self._log_interval_s

    def log_sample(self, sample: LogSample, force: bool = False) -> None:
        """Log a sample to the trace file.

        Args:
            sample: The sample to log.
            force: If True, log regardless of interval.
        """
        if not self._writer:
            return

        if not self.should_log(sample.sim_time_s, force=force or sample.event is not None):
            return

        self._writer.writerow([
            f"{sample.sim_time_s:.6f}",
            f"{sample.wall_time_s:.6f}",
            sample.state.name,
            f"{sample.voltage_V:.4f}",
            f"{sample.current_A:.6f}",
            f"{sample.power_W:.6f}",
            f"{sample.energy_cap_uJ:.2f}",
            f"{sample.energy_consumed_uJ:.2f}",
            f"{sample.energy_harvested_uJ:.2f}",
            sample.event or "",
        ])

        self._sample_count += 1
        self._last_log_time_s = sample.sim_time_s

    def log(
        self,
        sim_time_s: float,
        wall_time_s: float,
        state: PowerState,
        voltage_V: float,
        current_A: float,
        power_W: float,
        energy_cap_uJ: float,
        energy_consumed_uJ: float,
        energy_harvested_uJ: float,
        event: Optional[str] = None,
        force: bool = False,
    ) -> None:
        """Log a sample using individual parameters.

        Convenience method that creates a LogSample internally.
        """
        sample = LogSample(
            sim_time_s=sim_time_s,
            wall_time_s=wall_time_s,
            state=state,
            voltage_V=voltage_V,
            current_A=current_A,
            power_W=power_W,
            energy_cap_uJ=energy_cap_uJ,
            energy_consumed_uJ=energy_consumed_uJ,
            energy_harvested_uJ=energy_harvested_uJ,
            event=event,
        )
        self.log_sample(sample, force=force)

    def log_event(
        self,
        sim_time_s: float,
        wall_time_s: float,
        state: PowerState,
        voltage_V: float,
        current_A: float,
        power_W: float,
        energy_cap_uJ: float,
        energy_consumed_uJ: float,
        energy_harvested_uJ: float,
        event: str,
    ) -> None:
        """Log an event sample (always logged regardless of interval)."""
        self.log(
            sim_time_s=sim_time_s,
            wall_time_s=wall_time_s,
            state=state,
            voltage_V=voltage_V,
            current_A=current_A,
            power_W=power_W,
            energy_cap_uJ=energy_cap_uJ,
            energy_consumed_uJ=energy_consumed_uJ,
            energy_harvested_uJ=energy_harvested_uJ,
            event=event,
            force=True,
        )

    def flush(self) -> None:
        """Flush the trace file."""
        if self._file:
            self._file.flush()

    @property
    def sample_count(self) -> int:
        """Number of samples logged."""
        return self._sample_count
