"""Summary statistics for intermittent computing simulation."""

from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Optional
import json

from .state_machine import StateStatistics


@dataclass
class SimulationSummary:
    """Summary of a simulation run."""
    # Duration
    total_duration_s: float
    running_time_s: float
    charging_time_s: float
    duty_cycle_percent: float

    # Events
    power_failure_count: int
    reboot_count: int

    # Energy (in microjoules)
    total_energy_consumed_uJ: float
    total_energy_harvested_uJ: float
    net_energy_uJ: float

    # Power (in microwatts)
    average_power_consumption_uW: float
    average_power_running_uW: float

    # Voltage
    final_voltage_V: float
    min_voltage_observed_V: float
    max_voltage_observed_V: float

    # Samples
    sample_count: int

    def to_dict(self) -> dict:
        """Convert to dictionary."""
        return asdict(self)

    def to_json(self, path: str | Path, indent: int = 2) -> None:
        """Save summary to JSON file."""
        with open(path, "w") as f:
            json.dump(self.to_dict(), f, indent=indent)

    @classmethod
    def from_json(cls, path: str | Path) -> "SimulationSummary":
        """Load summary from JSON file."""
        with open(path, "r") as f:
            data = json.load(f)
        return cls(**data)

    def print_summary(self) -> None:
        """Print summary to stdout."""
        print("\n" + "=" * 50)
        print("SIMULATION SUMMARY")
        print("=" * 50)

        print(f"\nDuration:")
        print(f"  Total:    {self.total_duration_s:.3f} s")
        print(f"  Running:  {self.running_time_s:.3f} s")
        print(f"  Charging: {self.charging_time_s:.3f} s")
        print(f"  Duty:     {self.duty_cycle_percent:.1f}%")

        print(f"\nEvents:")
        print(f"  Power failures: {self.power_failure_count}")
        print(f"  Reboots:        {self.reboot_count}")

        print(f"\nEnergy:")
        print(f"  Consumed:  {self.total_energy_consumed_uJ:.2f} uJ")
        print(f"  Harvested: {self.total_energy_harvested_uJ:.2f} uJ")
        print(f"  Net:       {self.net_energy_uJ:.2f} uJ")

        print(f"\nPower:")
        print(f"  Average (total):   {self.average_power_consumption_uW:.2f} uW")
        print(f"  Average (running): {self.average_power_running_uW:.2f} uW")

        print(f"\nVoltage:")
        print(f"  Final: {self.final_voltage_V:.3f} V")
        print(f"  Range: {self.min_voltage_observed_V:.3f} - {self.max_voltage_observed_V:.3f} V")

        print(f"\nSamples logged: {self.sample_count}")
        print("=" * 50)


class StatisticsCollector:
    """Collects statistics during simulation."""

    def __init__(self) -> None:
        self.reset()

    def reset(self) -> None:
        """Reset all statistics."""
        self._total_consumed_uJ = 0.0
        self._total_harvested_uJ = 0.0
        self._min_voltage_V: Optional[float] = None
        self._max_voltage_V: Optional[float] = None
        self._final_voltage_V = 0.0
        self._running_power_sum_W = 0.0
        self._running_sample_count = 0
        self._total_power_sum_W = 0.0
        self._total_sample_count = 0

    def update(
        self,
        voltage_V: float,
        power_W: float,
        is_running: bool,
        energy_consumed_uJ: float,
        energy_harvested_uJ: float,
    ) -> None:
        """Update statistics with a new sample.

        Args:
            voltage_V: Current voltage.
            power_W: Current power consumption.
            is_running: Whether MSP430 is running.
            energy_consumed_uJ: Cumulative energy consumed.
            energy_harvested_uJ: Cumulative energy harvested.
        """
        # Track voltage range
        if self._min_voltage_V is None or voltage_V < self._min_voltage_V:
            self._min_voltage_V = voltage_V
        if self._max_voltage_V is None or voltage_V > self._max_voltage_V:
            self._max_voltage_V = voltage_V
        self._final_voltage_V = voltage_V

        # Track energy totals
        self._total_consumed_uJ = energy_consumed_uJ
        self._total_harvested_uJ = energy_harvested_uJ

        # Track power for averaging
        self._total_power_sum_W += power_W
        self._total_sample_count += 1

        if is_running:
            self._running_power_sum_W += power_W
            self._running_sample_count += 1

    def get_summary(
        self,
        state_stats: StateStatistics,
        sample_count: int,
    ) -> SimulationSummary:
        """Generate summary from collected statistics.

        Args:
            state_stats: Statistics from state machine.
            sample_count: Number of samples logged.

        Returns:
            Complete simulation summary.
        """
        total_duration = state_stats.running_time_s + state_stats.charging_time_s

        # Calculate duty cycle
        duty_cycle = 0.0
        if total_duration > 0:
            duty_cycle = (state_stats.running_time_s / total_duration) * 100

        # Calculate average power
        avg_power_total = 0.0
        if self._total_sample_count > 0:
            avg_power_total = self._total_power_sum_W / self._total_sample_count

        avg_power_running = 0.0
        if self._running_sample_count > 0:
            avg_power_running = self._running_power_sum_W / self._running_sample_count

        return SimulationSummary(
            total_duration_s=total_duration,
            running_time_s=state_stats.running_time_s,
            charging_time_s=state_stats.charging_time_s,
            duty_cycle_percent=duty_cycle,
            power_failure_count=state_stats.failure_count,
            reboot_count=state_stats.reboot_count,
            total_energy_consumed_uJ=self._total_consumed_uJ,
            total_energy_harvested_uJ=self._total_harvested_uJ,
            net_energy_uJ=self._total_harvested_uJ - self._total_consumed_uJ,
            average_power_consumption_uW=avg_power_total * 1e6,
            average_power_running_uW=avg_power_running * 1e6,
            final_voltage_V=self._final_voltage_V,
            min_voltage_observed_V=self._min_voltage_V or 0.0,
            max_voltage_observed_V=self._max_voltage_V or 0.0,
            sample_count=sample_count,
        )
