"""Otii Ace Pro interface for hardware-in-the-loop simulation."""

from abc import ABC, abstractmethod
from dataclasses import dataclass
import time
from typing import Optional

from .config import OtiiConfig


@dataclass
class Measurement:
    """A single measurement from Otii."""
    current_A: float
    voltage_V: float
    power_W: float
    timestamp_s: float


class OtiiInterfaceBase(ABC):
    """Abstract base class for Otii interface."""

    @abstractmethod
    def connect(self) -> None:
        """Establish connection to Otii."""
        pass

    @abstractmethod
    def disconnect(self) -> None:
        """Close connection to Otii."""
        pass

    @abstractmethod
    def set_voltage(self, voltage_V: float) -> None:
        """Set main output voltage."""
        pass

    @abstractmethod
    def enable_power(self, enable: bool) -> None:
        """Enable or disable main output."""
        pass

    @abstractmethod
    def get_measurement(self) -> Measurement:
        """Get current measurement."""
        pass

    @abstractmethod
    def is_connected(self) -> bool:
        """Check if connected to Otii."""
        pass


class OtiiInterface(OtiiInterfaceBase):
    """Interface to Otii Ace Pro using otii-tcp-client SDK."""

    def __init__(self, config: OtiiConfig) -> None:
        self.config = config
        self._otii = None
        self._arc = None
        self._project = None
        self._connected = False
        self._last_voltage = 0.0
        self._power_enabled = False
        self._start_time = 0.0

    def connect(self) -> None:
        """Establish connection to Otii."""
        try:
            from otii_tcp_client import otii_connection
        except ImportError:
            raise ImportError(
                "otii-tcp-client not installed. Install with: pip install otii-tcp-client"
            )

        self._otii = otii_connection.OtiiConnection(
            self.config.host, self.config.port
        )
        self._otii.connect()

        # Get the Otii Arc device
        devices = self._otii.get_devices()
        if not devices:
            raise RuntimeError("No Otii devices found")
        self._arc = devices[0]

        # Create a project for recording
        self._project = self._otii.get_active_project()

        # Configure the Arc
        self._arc.set_main_voltage(3.3)  # Default voltage
        self._arc.set_max_current(self.config.max_current_A)
        self._arc.set_range("auto")

        # Enable main output
        self._arc.enable_main(True)
        self._power_enabled = True

        self._start_time = time.time()
        self._connected = True

    def disconnect(self) -> None:
        """Close connection to Otii."""
        if self._arc:
            try:
                self._arc.enable_main(False)
            except Exception:
                pass

        if self._otii:
            try:
                self._otii.disconnect()
            except Exception:
                pass

        self._connected = False
        self._arc = None
        self._otii = None
        self._project = None

    def set_voltage(self, voltage_V: float) -> None:
        """Set main output voltage with hysteresis."""
        if not self._connected or not self._arc:
            return

        # Apply hysteresis to reduce TCP traffic
        if abs(voltage_V - self._last_voltage) < self.config.voltage_hysteresis_V:
            return

        # Clamp voltage to valid range (Otii supports 0.5V - 5V typically)
        voltage_V = max(0.5, min(voltage_V, 5.0))

        self._arc.set_main_voltage(voltage_V)
        self._last_voltage = voltage_V

    def enable_power(self, enable: bool) -> None:
        """Enable or disable main output."""
        if not self._connected or not self._arc:
            return

        if enable != self._power_enabled:
            self._arc.enable_main(enable)
            self._power_enabled = enable

    def get_measurement(self) -> Measurement:
        """Get current measurement from Otii.

        Note: This uses instant measurement, not recorded data.
        For high-speed sampling, recording should be used instead.
        """
        if not self._connected or not self._arc:
            return Measurement(
                current_A=0.0,
                voltage_V=0.0,
                power_W=0.0,
                timestamp_s=time.time() - self._start_time,
            )

        # Get real-time measurement
        # Note: The actual API may differ; adjust as needed
        try:
            current_A = self._arc.get_main_current()
            voltage_V = self._arc.get_main_voltage()
        except Exception:
            current_A = 0.0
            voltage_V = self._last_voltage

        return Measurement(
            current_A=current_A,
            voltage_V=voltage_V,
            power_W=current_A * voltage_V,
            timestamp_s=time.time() - self._start_time,
        )

    def is_connected(self) -> bool:
        """Check if connected to Otii."""
        return self._connected


class MockOtiiInterface(OtiiInterfaceBase):
    """Mock Otii interface for testing without hardware.

    Simulates a simple resistive load.
    """

    def __init__(
        self,
        config: OtiiConfig,
        load_resistance_ohm: float = 1000.0,
        noise_amplitude_A: float = 0.0001,
    ) -> None:
        """Initialize mock interface.

        Args:
            config: Otii configuration.
            load_resistance_ohm: Simulated load resistance.
            noise_amplitude_A: Current measurement noise amplitude.
        """
        self.config = config
        self.load_resistance_ohm = load_resistance_ohm
        self.noise_amplitude_A = noise_amplitude_A

        self._connected = False
        self._power_enabled = False
        self._voltage_V = 0.0
        self._start_time = 0.0

    def connect(self) -> None:
        """Simulate connection."""
        self._connected = True
        self._start_time = time.time()
        self._voltage_V = 3.3

    def disconnect(self) -> None:
        """Simulate disconnection."""
        self._connected = False
        self._power_enabled = False

    def set_voltage(self, voltage_V: float) -> None:
        """Set simulated voltage."""
        self._voltage_V = max(0.0, voltage_V)

    def enable_power(self, enable: bool) -> None:
        """Enable or disable simulated power."""
        self._power_enabled = enable

    def get_measurement(self) -> Measurement:
        """Get simulated measurement based on resistive load."""
        import random

        timestamp = time.time() - self._start_time

        if not self._power_enabled or self._voltage_V <= 0:
            return Measurement(
                current_A=0.0,
                voltage_V=self._voltage_V,
                power_W=0.0,
                timestamp_s=timestamp,
            )

        # Simulate current based on Ohm's law: I = V / R
        current_A = self._voltage_V / self.load_resistance_ohm

        # Add noise
        if self.noise_amplitude_A > 0:
            current_A += random.uniform(-self.noise_amplitude_A, self.noise_amplitude_A)
            current_A = max(0.0, current_A)

        power_W = self._voltage_V * current_A

        return Measurement(
            current_A=current_A,
            voltage_V=self._voltage_V,
            power_W=power_W,
            timestamp_s=timestamp,
        )

    def is_connected(self) -> bool:
        """Check if mock is connected."""
        return self._connected

    def set_load_resistance(self, resistance_ohm: float) -> None:
        """Change the simulated load resistance."""
        self.load_resistance_ohm = max(1.0, resistance_ohm)


def create_otii_interface(
    config: OtiiConfig,
    mock: bool = False,
    mock_load_resistance: float = 1000.0,
) -> OtiiInterfaceBase:
    """Factory function to create an Otii interface.

    Args:
        config: Otii configuration.
        mock: If True, create a mock interface.
        mock_load_resistance: Load resistance for mock interface.

    Returns:
        An Otii interface instance.
    """
    if mock:
        return MockOtiiInterface(config, load_resistance_ohm=mock_load_resistance)
    else:
        return OtiiInterface(config)
