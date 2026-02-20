# Intermittent Computing Simulator

A Python-based hardware-in-the-loop simulator that uses Otii Ace Pro to power an MSP430 with dynamically varying voltage, simulating capacitor-based energy harvesting for intermittent computing systems.

## Overview

The simulator:
1. Measures real MSP430 current consumption via Otii Ace Pro
2. Updates a capacitor energy model based on the measured current
3. Adjusts Otii output voltage to match the simulated capacitor voltage
4. Creates a realistic intermittent power environment with power failures and reboots

## Installation

```bash
cd simulator
uv sync
```

For development with test dependencies:

```bash
uv sync --dev
```

## Quick Start

### Mock Mode (No Hardware)

Run a simulation without Otii hardware to test the control logic:

```bash
uv run python -m intermittent_sim --mock --duration 10
```

### With Otii Hardware

1. Start Otii software and enable TCP server (port 1905)
2. Connect MSP430 to Otii Ace Pro main output
3. Run simulation:

```bash
uv run python -m intermittent_sim -c configs/default_config.json
```

## Usage

```
usage: intermittent-sim [-h] [-c CONFIG] [--capacitor uF] [--vmax V]
                        [--vmin V] [--harvest uW] [--duration s]
                        [--output-dir OUTPUT_DIR] [--mock] [--mock-load ohm]
                        [-q] [-v] [--print-config] [--generate-config PATH]

Hardware-in-the-loop simulator for intermittent computing using Otii Ace Pro

options:
  -h, --help            show this help message and exit
  -c CONFIG, --config CONFIG
                        Path to configuration JSON file
  --capacitor uF        Buffer capacitance in microfarads
  --vmax V              Maximum voltage / reboot threshold
  --vmin V              Minimum voltage / failure threshold
  --harvest uW          Harvest power in microwatts
  --duration s          Maximum simulation duration in seconds
  --output-dir OUTPUT_DIR
                        Output directory for logs
  --mock                Run in mock mode without real Otii hardware
  --mock-load ohm       Simulated load resistance for mock mode (default: 1000)
  -q, --quiet           Suppress progress output
  -v, --verbose         Enable verbose output
  --print-config        Print the effective configuration and exit
  --generate-config PATH
                        Generate a default configuration file and exit
```

### Examples

```bash
# Run with default config
uv run python -m intermittent_sim -c configs/default_config.json

# Override capacitor size and duration
uv run python -m intermittent_sim --capacitor 47.0 --harvest 1000 --duration 30

# Generate a config file
uv run python -m intermittent_sim --generate-config my_config.json

# Print effective configuration
uv run python -m intermittent_sim -c configs/default_config.json --capacitor 100 --print-config

# Or use the installed script directly
uv run intermittent-sim --mock --duration 10
```

## Configuration

Configuration is specified via JSON file:

```json
{
  "capacitor": {
    "C_buf_uF": 22.2,
    "V_max": 3.6,
    "V_min": 1.8,
    "V_initial": 3.6
  },
  "harvest": {
    "P_harvest_uW": 500.0
  },
  "otii": {
    "host": "localhost",
    "port": 1905,
    "sample_rate_hz": 10000,
    "max_current_A": 0.1,
    "voltage_hysteresis_V": 0.01
  },
  "control": {
    "loop_period_ms": 1.0,
    "max_duration_s": 60.0
  },
  "log": {
    "output_dir": "./logs",
    "trace_filename": "trace.csv",
    "summary_filename": "summary.json",
    "log_interval_ms": 1.0
  }
}
```

### Parameters

| Section | Parameter | Description |
|---------|-----------|-------------|
| capacitor | `C_buf_uF` | Buffer capacitance in microfarads |
| capacitor | `V_max` | Maximum voltage (reboot threshold) |
| capacitor | `V_min` | Minimum voltage (power failure threshold) |
| capacitor | `V_initial` | Initial capacitor voltage |
| harvest | `P_harvest_uW` | Constant harvest power in microwatts |
| otii | `host` | Otii TCP server hostname |
| otii | `port` | Otii TCP server port |
| otii | `sample_rate_hz` | Measurement sample rate |
| otii | `max_current_A` | Maximum expected current |
| otii | `voltage_hysteresis_V` | Minimum voltage change to update Otii |
| control | `loop_period_ms` | Control loop period in milliseconds |
| control | `max_duration_s` | Maximum simulation duration |
| log | `output_dir` | Output directory for logs |
| log | `trace_filename` | Trace CSV filename |
| log | `summary_filename` | Summary JSON filename |
| log | `log_interval_ms` | Logging interval (0 = every sample) |

## Output

### Trace File (CSV)

The trace file contains timestamped samples with columns:

| Column | Description |
|--------|-------------|
| `sim_time_s` | Simulation time in seconds |
| `wall_time_s` | Wall clock time in seconds |
| `state` | Power state (CHARGING, RUNNING, etc.) |
| `voltage_V` | Capacitor voltage |
| `current_A` | Load current |
| `power_W` | Power consumption |
| `energy_cap_uJ` | Energy stored in capacitor |
| `energy_consumed_uJ` | Total energy consumed |
| `energy_harvested_uJ` | Total energy harvested |
| `event` | Event (reboot, power_failure, etc.) |

### Summary File (JSON)

The summary file contains aggregate statistics:

- Total duration, running time, charging time
- Duty cycle percentage
- Power failure and reboot counts
- Total energy consumed and harvested
- Average power consumption
- Voltage range observed

## Architecture

```
┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐
│  CapacitorModel │────▶│  SimController   │────▶│  OtiiInterface  │
│  E, V tracking  │     │  main loop       │     │  set_voltage()  │
└─────────────────┘     └──────────────────┘     └─────────────────┘
                               │                        │
                               ▼                        ▼
                        ┌──────────────────┐     ┌─────────────────┐
                        │  StateMachine    │     │    MSP430 DUT   │
                        │  CHARGE/RUN/FAIL │     │  (actual HW)    │
                        └──────────────────┘     └─────────────────┘
                               │
                               ▼
                        ┌──────────────────┐
                        │  TraceLogger     │
                        │  CSV output      │
                        └──────────────────┘
```

### Components

| Module | Description |
|--------|-------------|
| `config.py` | Configuration dataclasses and JSON I/O |
| `capacitor.py` | Capacitor physics model (E = 0.5*C*V²) |
| `state_machine.py` | Power state machine (CHARGING ↔ RUNNING) |
| `otii_interface.py` | Otii Ace Pro SDK wrapper + mock |
| `controller.py` | Main control loop |
| `logger.py` | CSV trace logging |
| `statistics.py` | Summary statistics computation |
| `main.py` | CLI entry point |

## State Machine

```
CHARGING ──[V >= V_max]──> REBOOT ──[immediate]──> RUNNING
    ^                                                  │
    └────────[immediate]──── POWER_FAILURE <──[V <= V_min]
```

- **CHARGING**: Capacitor is charging, MSP430 is powered off
- **REBOOT**: Transitional state when V reaches V_max
- **RUNNING**: MSP430 is powered and executing
- **POWER_FAILURE**: Voltage dropped below V_min

## Physics Model

The capacitor energy is updated each loop iteration:

```
E_cap += P_harvest * dt - V_out * I_load * dt
E_cap = max(0, E_cap)          # Cannot go negative
V_cap = sqrt(2 * E_cap / C)    # Voltage from energy
V_cap = min(V_cap, V_max)      # Cannot exceed V_max
```

## Dependencies

- **Python 3.10+**
- **uv**: Fast Python package manager
- **otii-tcp-client**: Otii Ace Pro Python SDK

## Matching RockClimb Parameters

The default configuration matches RockClimb checkpoint insertion parameters:

| RockClimb Parameter | Simulator Config |
|---------------------|------------------|
| `C_buf_uF = 22.2` | `capacitor.C_buf_uF = 22.2` |
| `V_max = 3.6` | `capacitor.V_max = 3.6` |
| `V_min = 1.8` | `capacitor.V_min = 1.8` |

To match a specific E_safe value from RockClimb:
```
E_safe = 0.5 * C_buf_uF * (V_max² - V_min²) - N_reg * reg_restore_energy
C_buf_uF = (E_safe + 8) / 4.86
```
