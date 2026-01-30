"""Power state machine for intermittent computing simulation."""

from dataclasses import dataclass
from enum import Enum, auto
from typing import Optional, Callable


class PowerState(Enum):
    """Power states of the intermittent system."""
    CHARGING = auto()       # Capacitor is charging, MSP430 is off
    REBOOT = auto()         # Transitional state: voltage reached V_max, starting up
    RUNNING = auto()        # MSP430 is powered and executing
    POWER_FAILURE = auto()  # Voltage dropped below V_min, shutting down


@dataclass
class StateStatistics:
    """Statistics tracked by the state machine."""
    charging_time_s: float = 0.0
    running_time_s: float = 0.0
    failure_count: int = 0
    reboot_count: int = 0
    last_state_change_time_s: float = 0.0

    @property
    def total_time_s(self) -> float:
        """Total time tracked."""
        return self.charging_time_s + self.running_time_s

    @property
    def duty_cycle(self) -> float:
        """Fraction of time spent running."""
        total = self.total_time_s
        if total <= 0:
            return 0.0
        return self.running_time_s / total


class PowerStateMachine:
    """State machine managing power transitions.

    State transitions:
        CHARGING ──[V >= V_max]──> REBOOT ──[immediate]──> RUNNING
            ^                                                  |
            └────────[immediate]──── POWER_FAILURE <──[V <= V_min]
    """

    def __init__(
        self,
        V_max: float,
        V_min: float,
        on_state_change: Optional[Callable[[PowerState, PowerState], None]] = None,
    ) -> None:
        """Initialize the state machine.

        Args:
            V_max: Voltage threshold to start running (reboot).
            V_min: Voltage threshold for power failure.
            on_state_change: Optional callback for state changes (old_state, new_state).
        """
        self.V_max = V_max
        self.V_min = V_min
        self._state = PowerState.CHARGING
        self._stats = StateStatistics()
        self._current_time_s = 0.0
        self._on_state_change = on_state_change

    @property
    def state(self) -> PowerState:
        """Current power state."""
        return self._state

    @property
    def stats(self) -> StateStatistics:
        """State machine statistics."""
        return self._stats

    @property
    def is_running(self) -> bool:
        """Check if MSP430 should be powered."""
        return self._state == PowerState.RUNNING

    @property
    def is_charging(self) -> bool:
        """Check if in charging state."""
        return self._state == PowerState.CHARGING

    def reset(self) -> None:
        """Reset state machine to initial state."""
        self._state = PowerState.CHARGING
        self._stats = StateStatistics()
        self._current_time_s = 0.0

    def _transition_to(self, new_state: PowerState) -> None:
        """Transition to a new state."""
        if new_state == self._state:
            return

        old_state = self._state
        self._state = new_state
        self._stats.last_state_change_time_s = self._current_time_s

        if self._on_state_change:
            self._on_state_change(old_state, new_state)

    def update(self, voltage_V: float, dt_s: float = 0.0) -> PowerState:
        """Update state machine based on current voltage.

        Args:
            voltage_V: Current capacitor voltage.
            dt_s: Time elapsed since last update (for statistics).

        Returns:
            Current power state after update.
        """
        self._current_time_s += dt_s

        # Track time in current state
        if self._state == PowerState.RUNNING:
            self._stats.running_time_s += dt_s
        elif self._state == PowerState.CHARGING:
            self._stats.charging_time_s += dt_s

        # State transitions
        if self._state == PowerState.CHARGING:
            if voltage_V >= self.V_max:
                self._stats.reboot_count += 1
                self._transition_to(PowerState.REBOOT)
                # REBOOT is immediate, transition directly to RUNNING
                self._transition_to(PowerState.RUNNING)

        elif self._state == PowerState.RUNNING:
            if voltage_V <= self.V_min:
                self._stats.failure_count += 1
                self._transition_to(PowerState.POWER_FAILURE)
                # POWER_FAILURE is immediate, transition to CHARGING
                self._transition_to(PowerState.CHARGING)

        elif self._state == PowerState.REBOOT:
            # Should not stay in REBOOT; transition immediately to RUNNING
            self._transition_to(PowerState.RUNNING)

        elif self._state == PowerState.POWER_FAILURE:
            # Should not stay in POWER_FAILURE; transition immediately to CHARGING
            self._transition_to(PowerState.CHARGING)

        return self._state

    def get_state_name(self) -> str:
        """Get human-readable state name."""
        return self._state.name

    def get_event(self) -> Optional[str]:
        """Get event name if a transition just occurred."""
        # This is called by logger to track events
        # Actual event tracking would need more sophisticated implementation
        return None
