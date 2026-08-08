#!/usr/bin/env python3
"""Downsample 1 kHz Mementos voltage traces by block averaging.

    uv run python scripts/otii/downsample_traces.py benchmarks/traces/[0-9]*.txt \
        -o benchmarks/traces/50hz

Each output sample is the mean of the corresponding block of input samples
(e.g. 20 samples for 50 Hz), which acts as the anti-aliasing filter; picking
every 20th sample instead would alias fast spikes onto the output grid.
Writes ``<out>/<n>.csv`` with time_s,voltage_v columns, ready for
replay_trace.py.
"""

import argparse
import csv
import os
import sys

import numpy as np

SAMPLE_RATE = 1000.0


def load_voltage(path):
    """Read the second column of a raw two-column Mementos trace."""
    return np.loadtxt(path, usecols=1)


def block_average(voltage, factor):
    """Average consecutive blocks of `factor` samples, dropping the tail."""
    usable = len(voltage) - len(voltage) % factor
    return voltage[:usable].reshape(-1, factor).mean(axis=1)


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
    args = parser.parse_args()

    factor = round(SAMPLE_RATE / args.rate)
    if not np.isclose(SAMPLE_RATE / args.rate, factor):
        raise SystemExit(f"rate {args.rate} must divide {SAMPLE_RATE:.0f} Hz evenly")

    os.makedirs(args.out_dir, exist_ok=True)
    for path in args.traces:
        voltage = load_voltage(path)
        averaged = block_average(voltage, factor)
        name = os.path.splitext(os.path.basename(path))[0]
        out_path = os.path.join(args.out_dir, f"{name}.csv")
        write_csv(out_path, averaged, args.rate)
        print(
            f"{path} -> {out_path}: {len(voltage)} samples @ {SAMPLE_RATE:.0f} Hz "
            f"-> {len(averaged)} @ {args.rate:g} Hz"
        )


if __name__ == "__main__":
    sys.exit(main())
