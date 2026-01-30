"""Capacitor physics model for energy harvesting simulation."""

from dataclasses import dataclass
import math
from typing import Optional

from .config import CapacitorConfig


@dataclass
class CapacitorState:
    """Current state of the capacitor."""
    energy_uJ: float      # Stored energy in microjoules
    voltage_V: float      # Current voltage
    energy_consumed_uJ: float   # Total energy consumed this session
    energy_harvested_uJ: float  # Total energy harvested this session


class CapacitorModel:
    """Physics model for capacitor-based energy storage.

    The capacitor stores energy according to: E = 0.5 * C * V^2
    Energy flows in from harvesting and out to the load.
    """

    def __init__(self, config: CapacitorConfig) -> None:
        self.config = config
        self._energy_J: float = 0.0
        self._total_consumed_J: float = 0.0
        self._total_harvested_J: float = 0.0
        self.initialize()

    def initialize(self, V_initial: Optional[float] = None) -> CapacitorState:
        """Initialize capacitor to a given voltage.

        Args:
            V_initial: Initial voltage. Defaults to config.V_initial.

        Returns:
            Initial capacitor state.
        """
        V = V_initial if V_initial is not None else self.config.V_initial
        V = max(0.0, min(V, self.config.V_max))  # Clamp to valid range

        # E = 0.5 * C * V^2
        self._energy_J = 0.5 * self.config.C_buf_F * V * V
        self._total_consumed_J = 0.0
        self._total_harvested_J = 0.0

        return self.get_state()

    def get_state(self) -> CapacitorState:
        """Get current capacitor state."""
        return CapacitorState(
            energy_uJ=self._energy_J * 1e6,
            voltage_V=self._voltage_from_energy(self._energy_J),
            energy_consumed_uJ=self._total_consumed_J * 1e6,
            energy_harvested_uJ=self._total_harvested_J * 1e6,
        )

    def _voltage_from_energy(self, energy_J: float) -> float:
        """Calculate voltage from stored energy.

        V = sqrt(2 * E / C)
        """
        if energy_J <= 0:
            return 0.0
        V = math.sqrt(2 * energy_J / self.config.C_buf_F)
        return min(V, self.config.V_max)

    def _energy_from_voltage(self, voltage_V: float) -> float:
        """Calculate stored energy from voltage.

        E = 0.5 * C * V^2
        """
        return 0.5 * self.config.C_buf_F * voltage_V * voltage_V

    def update(
        self,
        dt_s: float,
        P_harvest_W: float,
        I_load_A: float,
        V_out_V: float,
    ) -> CapacitorState:
        """Update capacitor state for one time step.

        Energy balance: E_cap += P_harvest * dt - V_out * I_load * dt

        Args:
            dt_s: Time step in seconds.
            P_harvest_W: Harvest power in Watts.
            I_load_A: Load current in Amps.
            V_out_V: Output voltage in Volts (voltage delivered to load).

        Returns:
            Updated capacitor state.
        """
        # Calculate energy flows
        E_harvest = P_harvest_W * dt_s
        E_consumed = V_out_V * I_load_A * dt_s

        # Update energy
        self._energy_J += E_harvest - E_consumed
        self._energy_J = max(0.0, self._energy_J)  # Cannot go negative

        # Cap voltage at V_max (excess energy is lost)
        E_max = self._energy_from_voltage(self.config.V_max)
        if self._energy_J > E_max:
            self._energy_J = E_max

        # Track totals
        self._total_harvested_J += E_harvest
        self._total_consumed_J += E_consumed

        return self.get_state()

    def charge_only(self, dt_s: float, P_harvest_W: float) -> CapacitorState:
        """Update capacitor with only charging (no load).

        Used when the MSP430 is powered off during charging phase.

        Args:
            dt_s: Time step in seconds.
            P_harvest_W: Harvest power in Watts.

        Returns:
            Updated capacitor state.
        """
        return self.update(dt_s, P_harvest_W, I_load_A=0.0, V_out_V=0.0)

    @property
    def voltage_V(self) -> float:
        """Current capacitor voltage."""
        return self._voltage_from_energy(self._energy_J)

    @property
    def energy_uJ(self) -> float:
        """Current stored energy in microjoules."""
        return self._energy_J * 1e6

    @property
    def energy_percent(self) -> float:
        """Current energy as percentage of maximum capacity."""
        E_max = self._energy_from_voltage(self.config.V_max)
        if E_max <= 0:
            return 0.0
        return (self._energy_J / E_max) * 100.0

    def is_above_vmax(self) -> bool:
        """Check if voltage is at or above V_max."""
        return self.voltage_V >= self.config.V_max

    def is_below_vmin(self) -> bool:
        """Check if voltage is at or below V_min."""
        return self.voltage_V <= self.config.V_min
