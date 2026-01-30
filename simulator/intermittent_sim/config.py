"""Configuration dataclasses for the intermittent computing simulator."""

from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Optional
import json


@dataclass
class CapacitorConfig:
    """Capacitor model configuration."""
    C_buf_uF: float = 22.2  # Buffer capacitance in microfarads
    V_max: float = 3.6      # Maximum voltage (reboot threshold)
    V_min: float = 1.8      # Minimum voltage (power failure threshold)
    V_initial: float = 3.6  # Initial voltage at simulation start

    def __post_init__(self) -> None:
        if self.V_min >= self.V_max:
            raise ValueError(f"V_min ({self.V_min}) must be less than V_max ({self.V_max})")
        if not (self.V_min <= self.V_initial <= self.V_max):
            raise ValueError(f"V_initial ({self.V_initial}) must be between V_min and V_max")
        if self.C_buf_uF <= 0:
            raise ValueError(f"C_buf_uF ({self.C_buf_uF}) must be positive")

    @property
    def C_buf_F(self) -> float:
        """Capacitance in Farads."""
        return self.C_buf_uF * 1e-6


@dataclass
class HarvestConfig:
    """Energy harvesting configuration."""
    P_harvest_uW: float = 500.0  # Harvest power in microwatts

    def __post_init__(self) -> None:
        if self.P_harvest_uW < 0:
            raise ValueError(f"P_harvest_uW ({self.P_harvest_uW}) cannot be negative")

    @property
    def P_harvest_W(self) -> float:
        """Harvest power in Watts."""
        return self.P_harvest_uW * 1e-6


@dataclass
class OtiiConfig:
    """Otii Ace Pro connection configuration."""
    host: str = "localhost"
    port: int = 1905
    sample_rate_hz: int = 10000
    max_current_A: float = 0.1
    voltage_hysteresis_V: float = 0.01  # Only update if voltage changes by more than this

    def __post_init__(self) -> None:
        if self.sample_rate_hz <= 0:
            raise ValueError(f"sample_rate_hz ({self.sample_rate_hz}) must be positive")
        if self.max_current_A <= 0:
            raise ValueError(f"max_current_A ({self.max_current_A}) must be positive")


@dataclass
class ControlConfig:
    """Control loop configuration."""
    loop_period_ms: float = 1.0     # Control loop period in milliseconds
    max_duration_s: float = 60.0    # Maximum simulation duration in seconds

    def __post_init__(self) -> None:
        if self.loop_period_ms <= 0:
            raise ValueError(f"loop_period_ms ({self.loop_period_ms}) must be positive")
        if self.max_duration_s <= 0:
            raise ValueError(f"max_duration_s ({self.max_duration_s}) must be positive")

    @property
    def loop_period_s(self) -> float:
        """Loop period in seconds."""
        return self.loop_period_ms / 1000.0


@dataclass
class LogConfig:
    """Logging configuration."""
    output_dir: str = "./logs"
    trace_filename: str = "trace.csv"
    summary_filename: str = "summary.json"
    log_interval_ms: float = 1.0  # How often to log samples (0 = every loop iteration)

    @property
    def output_path(self) -> Path:
        """Output directory as Path object."""
        return Path(self.output_dir)

    @property
    def trace_path(self) -> Path:
        """Full path to trace file."""
        return self.output_path / self.trace_filename

    @property
    def summary_path(self) -> Path:
        """Full path to summary file."""
        return self.output_path / self.summary_filename


@dataclass
class SimulatorConfig:
    """Complete simulator configuration."""
    capacitor: CapacitorConfig = field(default_factory=CapacitorConfig)
    harvest: HarvestConfig = field(default_factory=HarvestConfig)
    otii: OtiiConfig = field(default_factory=OtiiConfig)
    control: ControlConfig = field(default_factory=ControlConfig)
    log: LogConfig = field(default_factory=LogConfig)

    @classmethod
    def from_json(cls, path: str | Path) -> "SimulatorConfig":
        """Load configuration from a JSON file."""
        with open(path, "r") as f:
            data = json.load(f)
        return cls.from_dict(data)

    @classmethod
    def from_dict(cls, data: dict) -> "SimulatorConfig":
        """Create configuration from a dictionary."""
        return cls(
            capacitor=CapacitorConfig(**data.get("capacitor", {})),
            harvest=HarvestConfig(**data.get("harvest", {})),
            otii=OtiiConfig(**data.get("otii", {})),
            control=ControlConfig(**data.get("control", {})),
            log=LogConfig(**data.get("log", {})),
        )

    def to_dict(self) -> dict:
        """Convert configuration to a dictionary."""
        return {
            "capacitor": asdict(self.capacitor),
            "harvest": asdict(self.harvest),
            "otii": asdict(self.otii),
            "control": asdict(self.control),
            "log": asdict(self.log),
        }

    def to_json(self, path: str | Path, indent: int = 2) -> None:
        """Save configuration to a JSON file."""
        with open(path, "w") as f:
            json.dump(self.to_dict(), f, indent=indent)

    def with_overrides(
        self,
        capacitor_uF: Optional[float] = None,
        V_max: Optional[float] = None,
        V_min: Optional[float] = None,
        P_harvest_uW: Optional[float] = None,
        duration_s: Optional[float] = None,
    ) -> "SimulatorConfig":
        """Create a new config with the specified overrides applied."""
        # Create copies of sub-configs
        cap_dict = asdict(self.capacitor)
        harvest_dict = asdict(self.harvest)
        control_dict = asdict(self.control)

        # Apply overrides
        if capacitor_uF is not None:
            cap_dict["C_buf_uF"] = capacitor_uF
        if V_max is not None:
            cap_dict["V_max"] = V_max
            if cap_dict["V_initial"] > V_max:
                cap_dict["V_initial"] = V_max
        if V_min is not None:
            cap_dict["V_min"] = V_min
        if P_harvest_uW is not None:
            harvest_dict["P_harvest_uW"] = P_harvest_uW
        if duration_s is not None:
            control_dict["max_duration_s"] = duration_s

        return SimulatorConfig(
            capacitor=CapacitorConfig(**cap_dict),
            harvest=HarvestConfig(**harvest_dict),
            otii=self.otii,
            control=ControlConfig(**control_dict),
            log=self.log,
        )
