"""Main control loop for intermittent computing simulation."""

import time
from typing import Optional, Callable

from .config import SimulatorConfig
from .capacitor import CapacitorModel
from .state_machine import PowerStateMachine, PowerState
from .otii_interface import OtiiInterfaceBase, create_otii_interface
from .logger import TraceLogger
from .statistics import StatisticsCollector, SimulationSummary


class SimController:
    """Main simulation controller.

    Orchestrates the capacitor model, state machine, Otii interface,
    and logging to run the intermittent computing simulation.
    """

    def __init__(
        self,
        config: SimulatorConfig,
        otii: Optional[OtiiInterfaceBase] = None,
        mock: bool = False,
    ) -> None:
        """Initialize the controller.

        Args:
            config: Simulator configuration.
            otii: Optional pre-created Otii interface.
            mock: If True and otii is None, create a mock interface.
        """
        self.config = config

        # Create components
        self.capacitor = CapacitorModel(config.capacitor)
        self.state_machine = PowerStateMachine(
            V_max=config.capacitor.V_max,
            V_min=config.capacitor.V_min,
            on_state_change=self._on_state_change,
        )
        self.logger = TraceLogger(config.log)
        self.stats_collector = StatisticsCollector()

        # Otii interface (created or provided)
        if otii is not None:
            self.otii = otii
        else:
            self.otii = create_otii_interface(config.otii, mock=mock)

        # Runtime state
        self._running = False
        self._sim_time_s = 0.0
        self._wall_start_time = 0.0
        self._last_event: Optional[str] = None
        self._on_event: Optional[Callable[[str, float, float], None]] = None

    def _on_state_change(self, old_state: PowerState, new_state: PowerState) -> None:
        """Handle state machine transitions."""
        if new_state == PowerState.REBOOT:
            self._last_event = "reboot"
        elif new_state == PowerState.POWER_FAILURE:
            self._last_event = "power_failure"
        elif new_state == PowerState.RUNNING and old_state == PowerState.CHARGING:
            self._last_event = "start"
        elif new_state == PowerState.CHARGING and old_state == PowerState.RUNNING:
            self._last_event = "stop"

    def run(
        self,
        on_event: Optional[Callable[[str, float, float], None]] = None,
        print_progress: bool = True,
    ) -> SimulationSummary:
        """Run the simulation.

        Args:
            on_event: Optional callback for events (event_name, sim_time, voltage).
            print_progress: If True, print periodic progress updates.

        Returns:
            Simulation summary.
        """
        self._on_event = on_event
        self._running = True
        self._sim_time_s = 0.0

        dt_s = self.config.control.loop_period_s
        max_time_s = self.config.control.max_duration_s
        progress_interval_s = max(1.0, max_time_s / 20)  # ~20 progress updates
        last_progress_time = 0.0

        try:
            # Connect to Otii
            self.otii.connect()

            # Initialize
            cap_state = self.capacitor.initialize()
            self.state_machine.reset()
            self.stats_collector.reset()

            # Set initial voltage
            self.otii.set_voltage(cap_state.voltage_V)

            # Determine initial state
            if cap_state.voltage_V >= self.config.capacitor.V_max:
                self.state_machine.update(cap_state.voltage_V)
                self.otii.enable_power(True)
            else:
                self.otii.enable_power(False)

            # Open logger
            self.logger.open()
            self._wall_start_time = time.time()

            # Log initial state
            self._log_sample(cap_state, force=True)

            if print_progress:
                print(f"Starting simulation (max duration: {max_time_s:.1f}s)")
                print(f"Initial voltage: {cap_state.voltage_V:.3f}V")
                print(f"Harvest power: {self.config.harvest.P_harvest_uW:.1f}uW")
                print("-" * 40)

            # Main control loop
            while self._running and self._sim_time_s < max_time_s:
                loop_start = time.time()

                # Get current measurement
                measurement = self.otii.get_measurement()

                # Update capacitor model
                if self.state_machine.is_running:
                    cap_state = self.capacitor.update(
                        dt_s=dt_s,
                        P_harvest_W=self.config.harvest.P_harvest_W,
                        I_load_A=measurement.current_A,
                        V_out_V=cap_state.voltage_V,
                    )
                else:
                    cap_state = self.capacitor.charge_only(
                        dt_s=dt_s,
                        P_harvest_W=self.config.harvest.P_harvest_W,
                    )

                # Update state machine
                prev_state = self.state_machine.state
                self.state_machine.update(cap_state.voltage_V, dt_s)
                new_state = self.state_machine.state

                # Handle state transitions
                if new_state != prev_state:
                    if new_state == PowerState.RUNNING:
                        self.otii.enable_power(True)
                    elif new_state == PowerState.CHARGING:
                        self.otii.enable_power(False)

                # Update Otii voltage
                self.otii.set_voltage(cap_state.voltage_V)

                # Update statistics
                self.stats_collector.update(
                    voltage_V=cap_state.voltage_V,
                    power_W=measurement.power_W,
                    is_running=self.state_machine.is_running,
                    energy_consumed_uJ=cap_state.energy_consumed_uJ,
                    energy_harvested_uJ=cap_state.energy_harvested_uJ,
                )

                # Log sample
                self._log_sample(cap_state, measurement=measurement)

                # Handle events
                if self._last_event:
                    if self._on_event:
                        self._on_event(
                            self._last_event,
                            self._sim_time_s,
                            cap_state.voltage_V,
                        )
                    self._last_event = None

                # Progress update
                if print_progress and (self._sim_time_s - last_progress_time) >= progress_interval_s:
                    self._print_progress(cap_state)
                    last_progress_time = self._sim_time_s

                # Advance time
                self._sim_time_s += dt_s

                # Sleep to maintain loop timing (real-time simulation)
                elapsed = time.time() - loop_start
                sleep_time = dt_s - elapsed
                if sleep_time > 0:
                    time.sleep(sleep_time)

        except KeyboardInterrupt:
            if print_progress:
                print("\nSimulation interrupted by user")

        finally:
            self._running = False

            # Final log
            try:
                cap_state = self.capacitor.get_state()
                self._log_sample(cap_state, force=True, event="end")
                self.logger.flush()
            except Exception:
                pass

            # Close logger
            self.logger.close()

            # Disable power and disconnect
            try:
                self.otii.enable_power(False)
                self.otii.disconnect()
            except Exception:
                pass

        # Generate summary
        summary = self.stats_collector.get_summary(
            state_stats=self.state_machine.stats,
            sample_count=self.logger.sample_count,
        )

        # Save summary
        try:
            self.config.log.output_path.mkdir(parents=True, exist_ok=True)
            summary.to_json(self.config.log.summary_path)
        except Exception as e:
            print(f"Warning: Could not save summary: {e}")

        return summary

    def _log_sample(
        self,
        cap_state,
        measurement=None,
        force: bool = False,
        event: Optional[str] = None,
    ) -> None:
        """Log a sample to the trace file."""
        event = event or self._last_event

        current_A = 0.0
        power_W = 0.0
        if measurement is not None:
            current_A = measurement.current_A
            power_W = measurement.power_W

        self.logger.log(
            sim_time_s=self._sim_time_s,
            wall_time_s=time.time() - self._wall_start_time,
            state=self.state_machine.state,
            voltage_V=cap_state.voltage_V,
            current_A=current_A,
            power_W=power_W,
            energy_cap_uJ=cap_state.energy_uJ,
            energy_consumed_uJ=cap_state.energy_consumed_uJ,
            energy_harvested_uJ=cap_state.energy_harvested_uJ,
            event=event,
            force=force,
        )

    def _print_progress(self, cap_state) -> None:
        """Print progress update."""
        stats = self.state_machine.stats
        print(
            f"t={self._sim_time_s:6.2f}s | "
            f"V={cap_state.voltage_V:.3f}V | "
            f"state={self.state_machine.get_state_name():12s} | "
            f"failures={stats.failure_count} | "
            f"duty={stats.duty_cycle * 100:.1f}%"
        )

    def stop(self) -> None:
        """Stop the simulation."""
        self._running = False

    @property
    def is_running(self) -> bool:
        """Check if simulation is running."""
        return self._running

    @property
    def sim_time_s(self) -> float:
        """Current simulation time in seconds."""
        return self._sim_time_s
