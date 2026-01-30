"""CLI entry point for the intermittent computing simulator."""

import argparse
import signal
import sys
from pathlib import Path
from typing import Optional

from .config import SimulatorConfig
from .controller import SimController


def parse_args() -> argparse.Namespace:
    """Parse command line arguments."""
    parser = argparse.ArgumentParser(
        prog="intermittent-sim",
        description="Hardware-in-the-loop simulator for intermittent computing using Otii Ace Pro",
    )

    # Configuration file
    parser.add_argument(
        "-c", "--config",
        type=str,
        default=None,
        help="Path to configuration JSON file",
    )

    # Capacitor parameters
    parser.add_argument(
        "--capacitor",
        type=float,
        default=None,
        metavar="uF",
        help="Buffer capacitance in microfarads (overrides config)",
    )
    parser.add_argument(
        "--vmax",
        type=float,
        default=None,
        metavar="V",
        help="Maximum voltage / reboot threshold (overrides config)",
    )
    parser.add_argument(
        "--vmin",
        type=float,
        default=None,
        metavar="V",
        help="Minimum voltage / failure threshold (overrides config)",
    )

    # Harvest power
    parser.add_argument(
        "--harvest",
        type=float,
        default=None,
        metavar="uW",
        help="Harvest power in microwatts (overrides config)",
    )

    # Simulation duration
    parser.add_argument(
        "--duration",
        type=float,
        default=None,
        metavar="s",
        help="Maximum simulation duration in seconds (overrides config)",
    )

    # Output
    parser.add_argument(
        "--output-dir",
        type=str,
        default=None,
        help="Output directory for logs (overrides config)",
    )

    # Mock mode
    parser.add_argument(
        "--mock",
        action="store_true",
        help="Run in mock mode without real Otii hardware",
    )

    parser.add_argument(
        "--mock-load",
        type=float,
        default=1000.0,
        metavar="ohm",
        help="Simulated load resistance in ohms for mock mode (default: 1000)",
    )

    # Verbosity
    parser.add_argument(
        "-q", "--quiet",
        action="store_true",
        help="Suppress progress output",
    )

    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Enable verbose output",
    )

    # Print config
    parser.add_argument(
        "--print-config",
        action="store_true",
        help="Print the effective configuration and exit",
    )

    # Generate default config
    parser.add_argument(
        "--generate-config",
        type=str,
        default=None,
        metavar="PATH",
        help="Generate a default configuration file and exit",
    )

    return parser.parse_args()


def load_config(args: argparse.Namespace) -> SimulatorConfig:
    """Load and apply configuration from args."""
    # Start with default or load from file
    if args.config:
        config = SimulatorConfig.from_json(args.config)
    else:
        config = SimulatorConfig()

    # Apply overrides
    config = config.with_overrides(
        capacitor_uF=args.capacitor,
        V_max=args.vmax,
        V_min=args.vmin,
        P_harvest_uW=args.harvest,
        duration_s=args.duration,
    )

    # Override output directory if specified
    if args.output_dir:
        config.log.output_dir = args.output_dir

    return config


def main() -> int:
    """Main entry point."""
    args = parse_args()

    # Handle --generate-config
    if args.generate_config:
        config = SimulatorConfig()
        config.to_json(args.generate_config)
        print(f"Generated default configuration: {args.generate_config}")
        return 0

    # Load configuration
    try:
        config = load_config(args)
    except FileNotFoundError:
        print(f"Error: Configuration file not found: {args.config}", file=sys.stderr)
        return 1
    except Exception as e:
        print(f"Error loading configuration: {e}", file=sys.stderr)
        return 1

    # Handle --print-config
    if args.print_config:
        import json
        print(json.dumps(config.to_dict(), indent=2))
        return 0

    # Create controller
    controller = SimController(config, mock=args.mock)

    # Set up signal handler for graceful shutdown
    def signal_handler(sig, frame):
        if not args.quiet:
            print("\nReceived interrupt signal, stopping simulation...")
        controller.stop()

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # Event callback for verbose mode
    def on_event(event: str, sim_time: float, voltage: float) -> None:
        if args.verbose:
            print(f"[{sim_time:.3f}s] Event: {event} (V={voltage:.3f})")

    # Run simulation
    if not args.quiet:
        print("=" * 50)
        print("Intermittent Computing Simulator")
        print("=" * 50)
        print(f"Mode: {'Mock' if args.mock else 'Hardware'}")
        print(f"Capacitor: {config.capacitor.C_buf_uF:.1f} uF")
        print(f"Voltage range: {config.capacitor.V_min:.2f}V - {config.capacitor.V_max:.2f}V")
        print(f"Harvest power: {config.harvest.P_harvest_uW:.1f} uW")
        print(f"Max duration: {config.control.max_duration_s:.1f} s")
        print(f"Output: {config.log.output_path}")
        print("=" * 50)

    try:
        summary = controller.run(
            on_event=on_event if args.verbose else None,
            print_progress=not args.quiet,
        )
    except Exception as e:
        print(f"Error during simulation: {e}", file=sys.stderr)
        if args.verbose:
            import traceback
            traceback.print_exc()
        return 1

    # Print summary
    if not args.quiet:
        summary.print_summary()
        print(f"\nTrace saved to: {config.log.trace_path}")
        print(f"Summary saved to: {config.log.summary_path}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
