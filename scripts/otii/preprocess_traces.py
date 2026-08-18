#!/usr/bin/env python3
"""Turn raw 1 kHz Mementos voltage traces into replay-ready traces.

    uv run python scripts/otii/preprocess_traces.py benchmarks/traces/original/[0-9]*.txt \
        -o benchmarks/traces --vmax 3.6 --speedup 10 --repeat 80

Per trace, in order: compress time by --speedup, block-average to --rate,
scale the traces listed in SCALE_TO_LEVEL, clip to --vmax, repeat --repeat
times back to back.

--speedup plays the recording that many times faster than real time (the
Mementos walks have a power failure only every 5-50 s, far slower than the
benchmarks run). Each output sample is the mean of the corresponding block of
input samples (e.g. 20 samples for 50 Hz at --speedup 1, 200 at --speedup
10), which acts as the anti-aliasing filter; picking every 20th sample
instead would alias fast spikes onto the output grid.
Writes ``<out>/<n>.csv`` with time_s,voltage_v columns.
"""

import argparse
import csv
import os
import sys

import numpy as np

SAMPLE_RATE = 1000.0

# Traces whose harvested voltage never reaches the target's operating range are
# scaled up so this percentile lands on the target level.
SCALE_PERCENTILE = 99
SCALE_TO_LEVEL = {"3": 3.6}


def load_voltage(path):
    """Read the second column of a raw two-column Mementos trace."""
    return np.loadtxt(path, usecols=1)


def block_average(voltage, factor):
    """Average consecutive blocks of `factor` samples, dropping the tail."""
    usable = len(voltage) - len(voltage) % factor
    return voltage[:usable].reshape(-1, factor).mean(axis=1)


def scale_percentile_to(voltage, percentile, target):
    """Scale `voltage` so its `percentile`-th percentile becomes `target`."""
    reference = np.percentile(voltage, percentile)
    if reference <= 0:
        raise SystemExit("cannot scale a trace whose reference level is not positive")
    return voltage * (target / reference)


def write_csv(path, voltage, rate):
    with open(path, "w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["time_s", "voltage_v"])
        for index, value in enumerate(voltage):
            writer.writerow([f"{index / rate:.4f}", f"{value:.6f}"])


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("traces", nargs="+", help="raw 1 kHz trace files (.txt)")
    parser.add_argument("-o", "--out-dir", required=True, help="output directory")
    parser.add_argument("--rate", type=float, default=50.0, help="target rate in Hz")
    parser.add_argument(
        "--speedup",
        type=float,
        default=1.0,
        help="compress time by this factor (replay the recording this many times faster)",
    )
    parser.add_argument(
        "--vmax", type=float, default=None, help="clip output voltages to this maximum"
    )
    parser.add_argument(
        "--repeat",
        type=int,
        default=1,
        help="write this many back-to-back repetitions of each trace",
    )
    args = parser.parse_args()

    if args.repeat < 1:
        raise SystemExit("--repeat must be at least 1")

    if args.speedup <= 0:
        raise SystemExit("--speedup must be positive")

    input_rate = SAMPLE_RATE * args.speedup
    factor = round(input_rate / args.rate)
    if not np.isclose(input_rate / args.rate, factor):
        raise SystemExit(
            f"rate {args.rate} must divide {input_rate:g} Hz "
            f"({SAMPLE_RATE:.0f} Hz x speedup {args.speedup:g}) evenly"
        )

    os.makedirs(args.out_dir, exist_ok=True)
    for path in args.traces:
        voltage = load_voltage(path)
        name = os.path.splitext(os.path.basename(path))[0]
        averaged = block_average(voltage, factor)
        if name in SCALE_TO_LEVEL:
            averaged = scale_percentile_to(
                averaged, SCALE_PERCENTILE, SCALE_TO_LEVEL[name]
            )
        if args.vmax is not None:
            averaged = np.minimum(averaged, args.vmax)
        repeated = np.tile(averaged, args.repeat)
        out_path = os.path.join(args.out_dir, f"{name}.csv")
        write_csv(out_path, repeated, args.rate)
        print(
            f"{path} -> {out_path}: {len(voltage)} samples @ {SAMPLE_RATE:.0f} Hz "
            f"(x{args.speedup:g}) -> {len(repeated)} @ {args.rate:g} Hz"
        )


if __name__ == "__main__":
    sys.exit(main())
